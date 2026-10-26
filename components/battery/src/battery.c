#include "battery.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "bsp.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
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

// shortcut: estimated discharge curve; calibrate with this device's load and battery temperature for accuracy.
// The table's "<1%" point at 3050 mV is represented by its midpoint, 0.5%.
static const struct {
  int millivolts;
  float percent;
} battery_soc_lut[] = {
    {3000, 0.0f},  {3050, 0.5f},  {3100, 1.0f},  {3150, 1.5f},  {3200, 2.0f},
    {3250, 4.0f},  {3300, 9.0f},  {3350, 13.0f}, {3400, 17.0f}, {3450, 21.0f},
    {3500, 27.0f}, {3550, 35.0f}, {3600, 44.0f}, {3650, 52.0f}, {3700, 57.0f},
    {3750, 62.0f}, {3800, 67.0f}, {3850, 73.0f}, {3900, 79.0f}, {3950, 84.0f},
    {4000, 88.0f}, {4050, 92.0f}, {4100, 97.0f}, {4150, 100.0f},
};

static int battery_voltage_to_percent(int battery_mv) {
  if (battery_mv <= battery_soc_lut[0].millivolts) {
    return (int)(battery_soc_lut[0].percent + 0.5f);
  }

  for (size_t i = 1; i < sizeof(battery_soc_lut) / sizeof(battery_soc_lut[0]); ++i) {
    if (battery_mv <= battery_soc_lut[i].millivolts) {
      const int mv_range = battery_soc_lut[i].millivolts - battery_soc_lut[i - 1].millivolts;
      const float fraction =
          (float)(battery_mv - battery_soc_lut[i - 1].millivolts) / mv_range;
      const float percent = battery_soc_lut[i - 1].percent +
                            fraction * (battery_soc_lut[i].percent -
                                        battery_soc_lut[i - 1].percent);
      return (int)(percent + 0.5f);
    }
  }

  const size_t last = sizeof(battery_soc_lut) / sizeof(battery_soc_lut[0]) - 1;
  return (int)(battery_soc_lut[last].percent + 0.5f);
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
  *voltage_mv = battery_mv;
  *percent = battery_voltage_to_percent(battery_mv);
  assert(*percent >= 0 && *percent <= 100);
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

  battery_runtime_save(force_save);
  battery_runtime_set_outputs(current_seconds, previous_seconds,
                              charger_connected);
  if (read_err != ESP_OK) return read_err;
  return battery_charger_state_known ? ESP_OK : ESP_ERR_NOT_FINISHED;
}

esp_err_t battery_init(void) {
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
  esp_err_t err = battery_read(&battery_mv, &percent);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Battery: %d mV (~%d%%)", battery_mv, percent);
  } else {
    ESP_LOGW(TAG, "Battery read failed: %s", esp_err_to_name(err));
  }
  return ESP_OK;
}
