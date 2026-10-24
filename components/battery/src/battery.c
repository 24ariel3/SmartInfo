#include "battery.h"

#include <assert.h>
#include <stddef.h>

#include "bsp.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

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
static adc_cali_handle_t battery_calibration;

// shortcut: generic room-temperature curve, calibrate this pack if SOC accuracy under load or temperature changes matters.
static const struct {
  int millivolts;
  int percent;
} battery_soc_lut[] = {
    {3300, 0}, {3410, 10}, {3510, 20}, {3570, 30}, {3620, 40}, {3690, 50},
    {3790, 60}, {3880, 70}, {3960, 80}, {4050, 90}, {4100, 95}, {4150, 100},
};

static int battery_voltage_to_percent(int battery_mv) {
  if (battery_mv <= battery_soc_lut[0].millivolts) return battery_soc_lut[0].percent;

  for (size_t i = 1; i < sizeof(battery_soc_lut) / sizeof(battery_soc_lut[0]); ++i) {
    if (battery_mv <= battery_soc_lut[i].millivolts) {
      int mv_range = battery_soc_lut[i].millivolts - battery_soc_lut[i - 1].millivolts;
      int percent_range = battery_soc_lut[i].percent - battery_soc_lut[i - 1].percent;
      return battery_soc_lut[i - 1].percent +
             (battery_mv - battery_soc_lut[i - 1].millivolts) * percent_range / mv_range;
    }
  }

  return battery_soc_lut[sizeof(battery_soc_lut) / sizeof(battery_soc_lut[0]) - 1].percent;
}

esp_err_t battery_read(int *voltage_mv, int *percent) {
  if (!voltage_mv || !percent) return ESP_ERR_INVALID_ARG;
  if (!battery_adc || !battery_calibration) return ESP_ERR_INVALID_STATE;

  int raw_sum = 0;
  for (int i = 0; i < 32; ++i) {
    int raw;
    esp_err_t err = adc_oneshot_read(battery_adc, battery_channel, &raw);
    if (err != ESP_OK) return err;
    raw_sum += raw;
  }

  int pin_mv;
  esp_err_t err = adc_cali_raw_to_voltage(battery_calibration, raw_sum / 32, &pin_mv);
  if (err != ESP_OK) return err;

  int battery_mv = pin_mv * 2;
  *voltage_mv = battery_mv;
  *percent = battery_voltage_to_percent(battery_mv);
  assert(*percent >= 0 && *percent <= 100);
  return ESP_OK;
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
  adc_cali_line_fitting_config_t calibration_config = {
      .unit_id = adc_unit,
      .atten = ADC_ATTEN_DB_12,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
      .default_vref = 1100,
  };
  ESP_ERROR_CHECK(adc_cali_create_scheme_line_fitting(&calibration_config,
                                                      &battery_calibration));

  int battery_mv, percent;
  esp_err_t err = battery_read(&battery_mv, &percent);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Battery: %d mV (~%d%%)", battery_mv, percent);
  } else {
    ESP_LOGW(TAG, "Battery read failed: %s", esp_err_to_name(err));
  }
  return ESP_OK;
}
