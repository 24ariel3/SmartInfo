#include "battery.h"

#include <assert.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "BATTERY";
static adc_oneshot_unit_handle_t battery_adc;
static adc_cali_handle_t battery_calibration;

esp_err_t battery_read(int *voltage_mv, int *percent) {
  if (!voltage_mv || !percent) return ESP_ERR_INVALID_ARG;
  if (!battery_adc || !battery_calibration) return ESP_ERR_INVALID_STATE;

  int raw_sum = 0;
  for (int i = 0; i < 32; ++i) {
    int raw;
    esp_err_t err = adc_oneshot_read(battery_adc, ADC_CHANNEL_6, &raw);
    if (err != ESP_OK) return err;
    raw_sum += raw;
  }

  int pin_mv;
  esp_err_t err = adc_cali_raw_to_voltage(battery_calibration, raw_sum / 32, &pin_mv);
  if (err != ESP_OK) return err;

  int battery_mv = pin_mv * 2;
  *voltage_mv = battery_mv;
  *percent = battery_mv <= 3300 ? 0 : battery_mv >= 4200 ? 100 :
             (battery_mv - 3300) * 100 / 900;
  assert(*percent >= 0 && *percent <= 100);
  return ESP_OK;
}

esp_err_t battery_init(void) {
  // shortcut: the always-on 100k/100k divider can back-power an unpowered ESP32; switch it if full-off isolation is needed.
  adc_oneshot_unit_init_cfg_t adc_config = {.unit_id = ADC_UNIT_1};
  ESP_ERROR_CHECK(adc_oneshot_new_unit(&adc_config, &battery_adc));
  adc_oneshot_chan_cfg_t channel_config = {
      .atten = ADC_ATTEN_DB_12,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  ESP_ERROR_CHECK(adc_oneshot_config_channel(battery_adc, ADC_CHANNEL_6, &channel_config));
  adc_cali_line_fitting_config_t calibration_config = {
      .unit_id = ADC_UNIT_1,
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
