#include "battery.h"

#include <assert.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>

#include "bsp.h"
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

/*
 * Battery sense wiring: ESP32 DevKit V1 and 18650 shield V8.
 * R1/R2/C1 below are external parts added for sensing.
 *
 * Shield BAT+ -- R1 100 kOhm --+-- white wire -- GPIO34 (BSP_BAT_SENSE)
 *                              |
 *                              +-- R2 100 kOhm -- GND
 *                              |
 *                              +-- C1 100 nF ---- GND
 *
 * Tap BAT+ at the PCB solder lug of the battery holder's positive metal
 * contact (marked '+'). Confirm continuity with the positive contact with
 * cells and USB power removed. Do not use the regulated 3 V/5 V output
 * or connect raw BAT+ directly to GPIO34.
 *
 * GND is one common return for R2, C1, the ESP32 and shield output:
 * - ESP32: use GND between GPIO13 and VIN on the 30-pin DevKit V1.
 *   With USB pointing down and components facing you, this is the left
 *   header's second pin from the bottom. Either ESP32 GND pin is equivalent.
 * - Shield: use a pad marked GND beside a 5 V output, shared with the
 *   ESP32 power connection. Use output GND, not the holder's negative lug.
 *
 * R1 and R2: 100 kOhm each, 1%. C1: 100 nF (0.1 uF, marking 104),
 * ceramic X7R, rated at least 10 V, in parallel with R2. Place C1 close
 * to GPIO34 with short leads to GND; 15 pF is not a substitute.
 * Charger presence: external charger VIN -- 100 kOhm --+-- GPIO35
 *                                                      +-- 33 kOhm -- GND
 *                                                      +-- 100 nF --- GND
 * Use the shared ESP32/shield/charger GND. GPIO35 sees about 1.24 V at 5 V VIN.
 * Shield keep-alive: GPIO33 -- 10 kOhm -- 2N3904 base; base -- 100 kOhm -- GND.
 * Q1 collector -> shield KEY (TP5602B pin 17); emitter -> shared output GND.
 * Keep the manual button connected. HIGH presses KEY; LOW releases it.
 * Use NORMAL mode after validating pulses; HOLD still dissipates its dummy load.
 *
 * The ADC filter capacitor follows Espressif's hardware guidelines:
 * https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32/schematic-checklist.html#adc
 *
 * A 4.2 V cell gives 2.1 V at GPIO34; battery_read() scales it by two.
 * Allow about 50 ms after the divider receives power for C1 to settle.
 * Before connecting GPIO34, verify the divider midpoint against GND
 * with a multimeter. The capacitor does not prevent off-state back-power.
 */

static const char *TAG = "BATTERY";
static adc_oneshot_unit_handle_t battery_adc;
static adc_channel_t battery_channel;
static adc_channel_t charger_channel;
static adc_cali_handle_t battery_calibration;

#define BATTERY_CHARGER_CONNECTED_MV 800
#define BATTERY_CHARGER_DISCONNECTED_MV 400
#define BATTERY_CHARGER_DEBOUNCE_SAMPLES 2
#define BATTERY_RUNTIME_SAVE_INTERVAL_US (60LL * 1000000)
#define BATTERY_RUNTIME_NVS_NAMESPACE "battery"
#define BATTERY_RUNTIME_NVS_KEY "runtime"
#define BATTERY_RUNTIME_VERSION 1
#define BATTERY_KEEP_ALIVE_INTERVAL_US (20ULL * 1000000)
#define BATTERY_KEEP_ALIVE_PULSE_US (200ULL * 1000)

typedef struct {
  uint32_t version;
  uint32_t charger_connected;
  uint64_t current_seconds;
  uint64_t previous_seconds;
} battery_runtime_record_t;

static nvs_handle_t battery_nvs;
static bool battery_nvs_opened;
static bool battery_runtime_writes_allowed;
static bool battery_runtime_dirty;
static uint64_t battery_current_seconds;
static uint64_t battery_previous_seconds;
static uint64_t battery_runtime_fraction_us;
static int64_t battery_last_runtime_update_us;
static int64_t battery_last_save_us;
static bool battery_saved_charger_connected;
static bool battery_charger_connected;
static bool battery_charger_state_known;
static bool battery_charger_candidate;
static unsigned battery_charger_candidate_count;
static bool battery_charger_adc_error_logged;
static esp_timer_handle_t battery_keep_alive_timer;
static esp_timer_handle_t battery_keep_alive_release_timer;
static portMUX_TYPE battery_keep_alive_lock = portMUX_INITIALIZER_UNLOCKED;
static bool battery_keep_alive_enabled;

static void battery_keep_alive_release(void *arg) {
  (void)arg;
  portENTER_CRITICAL(&battery_keep_alive_lock);
  gpio_set_level(BSP_BAT_KEEP_ALIVE, 0);
  portEXIT_CRITICAL(&battery_keep_alive_lock);
}

static void battery_keep_alive_pulse(void *arg) {
  (void)arg;
  portENTER_CRITICAL(&battery_keep_alive_lock);
  bool enabled = battery_keep_alive_enabled;
  portEXIT_CRITICAL(&battery_keep_alive_lock);
  if (!enabled || esp_timer_is_active(battery_keep_alive_release_timer)) return;

  // Arm release before pressing KEY so a timer failure cannot leave it held.
  esp_err_t err = esp_timer_start_once(battery_keep_alive_release_timer,
                                       BATTERY_KEEP_ALIVE_PULSE_US);
  if (err != ESP_OK) {
    battery_keep_alive_release(NULL);
    ESP_LOGE(TAG, "Could not schedule KEY release: %s", esp_err_to_name(err));
    return;
  }

  portENTER_CRITICAL(&battery_keep_alive_lock);
  if (battery_keep_alive_enabled) gpio_set_level(BSP_BAT_KEEP_ALIVE, 1);
  portEXIT_CRITICAL(&battery_keep_alive_lock);
}

static esp_err_t battery_keep_alive_set_enabled(bool enabled) {
  portENTER_CRITICAL(&battery_keep_alive_lock);
  bool changed = battery_keep_alive_enabled != enabled;
  battery_keep_alive_enabled = enabled;
  if (!enabled) gpio_set_level(BSP_BAT_KEEP_ALIVE, 0);
  portEXIT_CRITICAL(&battery_keep_alive_lock);
  if (!changed) return ESP_OK;

  esp_err_t err = enabled
      ? esp_timer_start_periodic(battery_keep_alive_timer,
                                 BATTERY_KEEP_ALIVE_INTERVAL_US)
      : esp_timer_stop(battery_keep_alive_timer);
  if (!enabled && err == ESP_ERR_INVALID_STATE) return ESP_OK;
  if (err != ESP_OK) {
    portENTER_CRITICAL(&battery_keep_alive_lock);
    battery_keep_alive_enabled = false;
    gpio_set_level(BSP_BAT_KEEP_ALIVE, 0);
    portEXIT_CRITICAL(&battery_keep_alive_lock);
  }
  return err;
}

static esp_err_t battery_keep_alive_init(void) {
  esp_err_t err = gpio_set_level(BSP_BAT_KEEP_ALIVE, 0);
  if (err != ESP_OK) return err;
  const gpio_config_t config = {
      .pin_bit_mask = 1ULL << BSP_BAT_KEEP_ALIVE,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  err = gpio_config(&config);
  if (err != ESP_OK) return err;
  err = gpio_sleep_sel_dis(BSP_BAT_KEEP_ALIVE);
  if (err != ESP_OK) return err;

  const esp_timer_create_args_t release_args = {
      .callback = battery_keep_alive_release,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "battery_key_release",
  };
  const esp_timer_create_args_t pulse_args = {
      .callback = battery_keep_alive_pulse,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "battery_key_pulse",
  };
  err = esp_timer_create(&release_args, &battery_keep_alive_release_timer);
  if (err != ESP_OK) goto fail;
  err = esp_timer_create(&pulse_args, &battery_keep_alive_timer);
  if (err != ESP_OK) goto fail;
  // shortcut: 200 ms every 20 s needs NORMAL-mode validation on this shield; retune if presses are missed.
  err = battery_keep_alive_set_enabled(true);
  if (err == ESP_OK) return ESP_OK;

fail:
  if (battery_keep_alive_timer) {
    esp_timer_delete(battery_keep_alive_timer);
    battery_keep_alive_timer = NULL;
  }
  if (battery_keep_alive_release_timer) {
    esp_timer_delete(battery_keep_alive_release_timer);
    battery_keep_alive_release_timer = NULL;
  }
  return err;
}

#define BATT_LUT_MIN_MV 3000
#define BATT_LUT_MAX_MV 4200
#define BATT_LUT_STEP_MV 50
#define BATTERY_FILTER_SAMPLES 5

// Bayreuth NCR18650B, https://doi.org/10.5281/zenodo.8369275; derivation in README.
// Median of 92 discharge curves at 0.05C/25 C, integrated above 3.0 V; units 0.1%.
// shortcut: reference curve is not load-calibrated; validate against device discharge measurements.
static const uint16_t battery_soc_lut_permille[] = {
    0,   3,   8,   13,  19,  33,  66,  105, 135,
    173, 223, 300, 390, 469, 523, 565, 619, 681,
    746, 802, 851, 901, 962, 995, 1000,
};
_Static_assert(sizeof(battery_soc_lut_permille) /
                       sizeof(battery_soc_lut_permille[0]) ==
                   (BATT_LUT_MAX_MV - BATT_LUT_MIN_MV) / BATT_LUT_STEP_MV + 1,
               "Battery LUT length must match its voltage range and step");

static int battery_voltage_history[BATTERY_FILTER_SAMPLES];
static unsigned battery_voltage_count;
static unsigned battery_voltage_next;
static int32_t battery_filtered_q8;

static int battery_voltage_to_percent(int battery_mv) {
  if (battery_mv <= BATT_LUT_MIN_MV) return 0;
  if (battery_mv >= BATT_LUT_MAX_MV) return 100;

  const int offset_mv = battery_mv - BATT_LUT_MIN_MV;
  const int idx = offset_mv / BATT_LUT_STEP_MV;
  const int rem_mv = offset_mv % BATT_LUT_STEP_MV;
  const int p0 = battery_soc_lut_permille[idx];
  const int p1 = battery_soc_lut_permille[idx + 1];
  const int numerator = p0 * BATT_LUT_STEP_MV + rem_mv * (p1 - p0);
  // Round only the final percentage to avoid rounding twice near half-percent values.
  return (numerator + 5 * BATT_LUT_STEP_MV) / (10 * BATT_LUT_STEP_MV);
}

static int battery_filter_voltage_mv(int raw_mv, int *median_mv) {
  battery_voltage_history[battery_voltage_next] = raw_mv;
  battery_voltage_next = (battery_voltage_next + 1) % BATTERY_FILTER_SAMPLES;
  if (battery_voltage_count < BATTERY_FILTER_SAMPLES) ++battery_voltage_count;

  int sorted[BATTERY_FILTER_SAMPLES];
  for (unsigned i = 0; i < battery_voltage_count; ++i) {
    int value = battery_voltage_history[i];
    unsigned j = i;
    while (j > 0 && sorted[j - 1] > value) {
      sorted[j] = sorted[j - 1];
      --j;
    }
    sorted[j] = value;
  }
  unsigned middle = battery_voltage_count / 2;
  *median_mv = battery_voltage_count % 2
                   ? sorted[middle]
                   : (sorted[middle - 1] + sorted[middle] + 1) / 2;
  if (battery_voltage_count == 1) {
    battery_filtered_q8 = *median_mv * 256;
  } else {
    battery_filtered_q8 += (*median_mv * 256 - battery_filtered_q8) / 4;
  }
  return (battery_filtered_q8 + 128) / 256;
}

static esp_err_t battery_adc_read_pin_mv(adc_channel_t channel, int *pin_mv) {
  int raw_sum = 0;
  for (int i = 0; i < 32; ++i) {
    int raw;
    esp_err_t err = adc_oneshot_read(battery_adc, channel, &raw);
    if (err != ESP_OK) return err;
    raw_sum += raw;
  }

  return adc_cali_raw_to_voltage(battery_calibration, raw_sum / 32, pin_mv);
}

esp_err_t battery_read(int *voltage_mv, int *percent) {
  if (!voltage_mv || !percent) return ESP_ERR_INVALID_ARG;
  if (!battery_adc || !battery_calibration) return ESP_ERR_INVALID_STATE;

  int pin_mv;
  esp_err_t err = battery_adc_read_pin_mv(battery_channel, &pin_mv);
  if (err != ESP_OK) return err;

  int battery_mv = pin_mv * 2;
  int median_mv;
  int filtered_mv = battery_filter_voltage_mv(battery_mv, &median_mv);
  *voltage_mv = battery_mv;
  *percent = battery_voltage_to_percent(filtered_mv);
  assert(*percent >= 0 && *percent <= 100);
  ESP_LOGD(TAG, "Battery sample: t=%" PRId64 " us, raw=%d mV, median=%d mV, "
                "filtered=%d mV, energy=%d%%, charger=%s",
           esp_timer_get_time(), battery_mv, median_mv, filtered_mv, *percent,
           battery_charger_state_known
               ? (battery_charger_connected ? "connected" : "disconnected")
               : "unknown");
  return ESP_OK;
}

static void battery_runtime_load(void) {
  esp_err_t err = nvs_open(BATTERY_RUNTIME_NVS_NAMESPACE, NVS_READWRITE,
                           &battery_nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Could not open runtime storage: %s", esp_err_to_name(err));
    return;
  }
  battery_nvs_opened = true;
  battery_runtime_writes_allowed = true;

  battery_runtime_record_t record;
  size_t record_size = sizeof(record);
  err = nvs_get_blob(battery_nvs, BATTERY_RUNTIME_NVS_KEY, &record,
                     &record_size);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    battery_runtime_dirty = true;
    return;
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Could not read runtime storage: %s; writes disabled",
             esp_err_to_name(err));
    battery_runtime_writes_allowed = false;
    return;
  }
  if (record_size != sizeof(record) ||
      record.version != BATTERY_RUNTIME_VERSION ||
      record.charger_connected > 1) {
    ESP_LOGE(TAG, "Runtime storage format is unsupported; writes disabled");
    battery_runtime_writes_allowed = false;
    return;
  }

  battery_current_seconds = record.current_seconds;
  battery_previous_seconds = record.previous_seconds;
  battery_saved_charger_connected = record.charger_connected != 0;
  ESP_LOGI(TAG, "Loaded battery runtime: current=%llu s, previous=%llu s",
           (unsigned long long)battery_current_seconds,
           (unsigned long long)battery_previous_seconds);
}

static void battery_runtime_save(bool force) {
  if (!battery_runtime_dirty || !battery_nvs_opened ||
      !battery_runtime_writes_allowed) {
    return;
  }

  int64_t now_us = esp_timer_get_time();
  if (!force && now_us - battery_last_save_us < BATTERY_RUNTIME_SAVE_INTERVAL_US) {
    return;
  }

  const battery_runtime_record_t record = {
      .version = BATTERY_RUNTIME_VERSION,
      .charger_connected = battery_charger_connected ? 1 : 0,
      .current_seconds = battery_current_seconds,
      .previous_seconds = battery_previous_seconds,
  };
  esp_err_t err = nvs_set_blob(battery_nvs, BATTERY_RUNTIME_NVS_KEY, &record,
                               sizeof(record));
  if (err == ESP_OK) err = nvs_commit(battery_nvs);
  battery_last_save_us = now_us;
  if (err == ESP_OK) {
    battery_runtime_dirty = false;
  } else {
    ESP_LOGE(TAG, "Could not save battery runtime: %s", esp_err_to_name(err));
  }
}

static void battery_runtime_set_outputs(uint64_t *current_seconds,
                                        uint64_t *previous_seconds,
                                        bool *charger_connected) {
  *current_seconds = battery_current_seconds;
  *previous_seconds = battery_previous_seconds;
  *charger_connected = battery_charger_connected;
}

esp_err_t battery_runtime_update(uint64_t *current_seconds,
                                 uint64_t *previous_seconds,
                                 bool *charger_connected) {
  if (!current_seconds || !previous_seconds || !charger_connected) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!battery_adc || !battery_calibration || battery_last_runtime_update_us == 0) {
    return ESP_ERR_INVALID_STATE;
  }

  int64_t now_us = esp_timer_get_time();
  if (battery_charger_state_known && !battery_charger_connected &&
      now_us > battery_last_runtime_update_us) {
    uint64_t elapsed_us = (uint64_t)(now_us - battery_last_runtime_update_us) +
                          battery_runtime_fraction_us;
    uint64_t elapsed_seconds = elapsed_us / 1000000ULL;
    battery_runtime_fraction_us = elapsed_us % 1000000ULL;
    if (elapsed_seconds > 0) {
      battery_current_seconds += elapsed_seconds;
      battery_runtime_dirty = true;
    }
  }
  battery_last_runtime_update_us = now_us;

  int charger_pin_mv = 0;
  esp_err_t read_err = battery_adc_read_pin_mv(charger_channel, &charger_pin_mv);
  bool force_save = false;
  if (read_err != ESP_OK) {
    battery_charger_candidate_count = 0;
    if (!battery_charger_adc_error_logged) {
      ESP_LOGW(TAG, "Charger sense ADC read failed: %s", esp_err_to_name(read_err));
      battery_charger_adc_error_logged = true;
    }
  } else {
    if (battery_charger_adc_error_logged) {
      ESP_LOGI(TAG, "Charger sense ADC recovered");
      battery_charger_adc_error_logged = false;
    }

    bool candidate_valid = true;
    bool candidate_connected = false;
    if (charger_pin_mv >= BATTERY_CHARGER_CONNECTED_MV) {
      candidate_connected = true;
    } else if (charger_pin_mv <= BATTERY_CHARGER_DISCONNECTED_MV) {
      candidate_connected = false;
    } else {
      candidate_valid = false;
      battery_charger_candidate_count = 0;
    }

    if (candidate_valid) {
      if (battery_charger_candidate_count == 0 ||
          candidate_connected != battery_charger_candidate) {
        battery_charger_candidate = candidate_connected;
        battery_charger_candidate_count = 1;
      } else if (battery_charger_candidate_count <
                 BATTERY_CHARGER_DEBOUNCE_SAMPLES) {
        ++battery_charger_candidate_count;
      }

      if (battery_charger_candidate_count >= BATTERY_CHARGER_DEBOUNCE_SAMPLES) {
        if (!battery_charger_state_known) {
          if (candidate_connected != battery_saved_charger_connected) {
            if (candidate_connected) {
              if (battery_current_seconds > 0) {
                battery_previous_seconds = battery_current_seconds;
              }
              battery_current_seconds = 0;
            } else {
              battery_current_seconds = 0;
            }
            battery_runtime_fraction_us = 0;
          }
          battery_charger_connected = candidate_connected;
          battery_charger_state_known = true;
          battery_runtime_dirty = true;
          force_save = true;
          ESP_LOGI(TAG, "Charger state detected: %s (%d mV)",
                   candidate_connected ? "connected" : "disconnected",
                   charger_pin_mv);
        } else if (candidate_connected != battery_charger_connected) {
          if (candidate_connected) {
            if (battery_current_seconds > 0) {
              battery_previous_seconds = battery_current_seconds;
            }
            battery_current_seconds = 0;
          } else {
            battery_current_seconds = 0;
          }
          battery_runtime_fraction_us = 0;
          battery_charger_connected = candidate_connected;
          battery_runtime_dirty = true;
          force_save = true;
          ESP_LOGI(TAG, "Charger %s (%d mV)",
                   candidate_connected ? "connected" : "disconnected",
                   charger_pin_mv);
        }
      }
    }
  }

  esp_err_t keep_alive_err = ESP_OK;
  if (read_err != ESP_OK ||
      battery_charger_candidate_count >= BATTERY_CHARGER_DEBOUNCE_SAMPLES) {
    keep_alive_err = battery_keep_alive_set_enabled(
        read_err != ESP_OK || !battery_charger_connected);
    if (keep_alive_err != ESP_OK) {
      ESP_LOGE(TAG, "Could not update shield keep-alive: %s",
               esp_err_to_name(keep_alive_err));
    }
  }

  battery_runtime_save(force_save);
  battery_runtime_set_outputs(current_seconds, previous_seconds,
                              charger_connected);
  if (read_err != ESP_OK) return read_err;
  if (keep_alive_err != ESP_OK) return keep_alive_err;
  return battery_charger_state_known ? ESP_OK : ESP_ERR_NOT_FINISHED;
}

esp_err_t battery_init(void) {
  esp_err_t err = battery_keep_alive_init();
  if (err != ESP_OK) return err;

  // shortcut: the always-on 100k/100k divider can back-power an unpowered ESP32; switch it if full-off isolation is needed.
  adc_unit_t adc_unit;
  ESP_ERROR_CHECK(adc_oneshot_io_to_channel(BSP_BAT_SENSE, &adc_unit, &battery_channel));
  adc_oneshot_unit_init_cfg_t adc_config = {.unit_id = adc_unit};
  ESP_ERROR_CHECK(adc_oneshot_new_unit(&adc_config, &battery_adc));
  adc_oneshot_chan_cfg_t channel_config = {
      .atten = ADC_ATTEN_DB_12,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  ESP_ERROR_CHECK(adc_oneshot_config_channel(battery_adc, battery_channel, &channel_config));

  adc_unit_t charger_adc_unit;
  ESP_ERROR_CHECK(adc_oneshot_io_to_channel(BSP_CHARGER_SENSE, &charger_adc_unit,
                                            &charger_channel));
  if (charger_adc_unit != adc_unit) return ESP_ERR_NOT_SUPPORTED;
  ESP_ERROR_CHECK(adc_oneshot_config_channel(battery_adc, charger_channel,
                                              &channel_config));

  adc_cali_line_fitting_config_t calibration_config = {
      .unit_id = adc_unit,
      .atten = ADC_ATTEN_DB_12,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
      .default_vref = 1100,
  };
  ESP_ERROR_CHECK(adc_cali_create_scheme_line_fitting(&calibration_config,
                                                      &battery_calibration));

  battery_runtime_load();
  battery_last_runtime_update_us = esp_timer_get_time();

  int battery_mv, percent;
  err = battery_read(&battery_mv, &percent);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Battery: %d mV (~%d%%)", battery_mv, percent);
  } else {
    ESP_LOGW(TAG, "Battery read failed: %s", esp_err_to_name(err));
  }
  return ESP_OK;
}
