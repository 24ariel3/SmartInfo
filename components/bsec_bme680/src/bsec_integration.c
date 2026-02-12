#include "bsec_integration.h"
#include "bsp.h" // Use BSP for I2C configuration
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

// BSEC and BME68x headers
#include "bme68x.h"
#include "bsec_datatypes.h"
#include "bsec_interface.h"

#include "nvs.h"

static const char *TAG = "BSEC";

// State save interval: 60 minutes (in nanoseconds)
#define BSEC_STATE_SAVE_INTERVAL_NS (60LL * 60 * 1000 * 1000 * 1000)

#define BSEC_NVS_NAMESPACE "bsec_storage"
#define BSEC_NVS_KEY_STATE "state"

// Global BSEC work buffer (static allocation)
static uint8_t bsec_mem_block[BSEC_MAX_WORKBUFFER_SIZE];

void bsec_load_state(void) {
  nvs_handle_t my_handle;
  esp_err_t err = nvs_open(BSEC_NVS_NAMESPACE, NVS_READONLY, &my_handle);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Error (%s) opening NVS handle! (Might be first run)",
             esp_err_to_name(err));
    return;
  }

  size_t required_size = 0;
  err = nvs_get_blob(my_handle, BSEC_NVS_KEY_STATE, NULL, &required_size);
  if (err == ESP_OK && required_size > 0) {
    uint8_t *bsec_state = malloc(required_size);
    if (bsec_state) {
      err = nvs_get_blob(my_handle, BSEC_NVS_KEY_STATE, bsec_state,
                         &required_size);
      if (err == ESP_OK) {
        int ret = bsec_set_state(bsec_state, required_size, bsec_mem_block,
                                 BSEC_MAX_WORKBUFFER_SIZE);
        if (ret == BSEC_OK) {
          ESP_LOGI(TAG, "BSEC state loaded from NVS");
        } else {
          ESP_LOGE(TAG, "Failed to set BSEC state: %d", ret);
        }
      }
      free(bsec_state);
    }
  }
  nvs_close(my_handle);
}

void bsec_save_state(void) {
  ESP_LOGI(TAG, "Saving BSEC state...");
  uint8_t bsec_state[BSEC_MAX_STATE_BLOB_SIZE];
  uint32_t n_state = BSEC_MAX_STATE_BLOB_SIZE;

  int ret = bsec_get_state(0, bsec_state, n_state, bsec_mem_block,
                           BSEC_MAX_WORKBUFFER_SIZE, &n_state);

  if (ret == BSEC_OK) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(BSEC_NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
      err = nvs_set_blob(my_handle, BSEC_NVS_KEY_STATE, bsec_state, n_state);
      if (err == ESP_OK) {
        err = nvs_commit(my_handle);
        if (err == ESP_OK) {
          ESP_LOGI(TAG, "BSEC state saved to NVS");
        }
      }
      nvs_close(my_handle);
    } else {
      ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
    }
  } else {
    ESP_LOGE(TAG, "Failed to get BSEC state: %d", ret);
  }
}

// I2C Configuration moved to BSP
// #define I2C_MASTER_SCL_IO 22
// #define I2C_MASTER_SDA_IO 21
// #define I2C_MASTER_NUM I2C_NUM_0
// #define I2C_MASTER_FREQ_HZ 100000
// #define I2C_MASTER_TX_BUF_DISABLE 0
// #define I2C_MASTER_RX_BUF_DISABLE 0

#define BME680_I2C_ADDR 0x77 // Or 0x76
#define TOTAL_VIRTUAL_SENSORS 14
#define BSEC_TASK_STACK_SIZE 6144
#define BSEC_TASK_PRIORITY 5

// API and wrapper prototypes
BME68X_INTF_RET_TYPE bme68x_i2c_read(uint8_t reg_addr, uint8_t *reg_data,
                                     uint32_t len, void *intf_ptr);
BME68X_INTF_RET_TYPE bme68x_i2c_write(uint8_t reg_addr, const uint8_t *reg_data,
                                      uint32_t len, void *intf_ptr);
void bme68x_delay_us(uint32_t period, void *intf_ptr);
int64_t bsec_get_timestamp_ns(void);

// Global BSEC state
static struct bme68x_dev bme_dev;

// Thread-safe state storage
static bsec_values_t bsec_latest_values = {0};
static SemaphoreHandle_t bsec_data_mutex = NULL;

// Static cache for change detection
static bsec_values_t s_cached_bsec = {0};
static bool s_bsec_cache_initialized = false;
static int64_t s_last_update_ts = 0;

int64_t bsec_get_last_updated_time(void) { return s_last_update_ts; }

esp_err_t bsec_get_latest_values(bsec_values_t *out_values, bool *is_updated) {
  if (!bsec_data_mutex)
    return ESP_ERR_INVALID_STATE;

  // Initialize is_updated to false
  if (is_updated)
    *is_updated = false;

  if (xSemaphoreTake(bsec_data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    *out_values = bsec_latest_values;
    xSemaphoreGive(bsec_data_mutex);

    // Check if data changed compared to cache
    if (is_updated) {
      if (!s_bsec_cache_initialized) {
        *is_updated = true; // First fetch, always updated
      } else {
        // Compare key values (IAQ, temp, humidity, accuracy)
        *is_updated = (int)out_values->iaq != (int)s_cached_bsec.iaq ||
                      (int)out_values->comp_temperature !=
                          (int)s_cached_bsec.comp_temperature ||
                      (int)out_values->comp_humidity !=
                          (int)s_cached_bsec.comp_humidity ||
                      out_values->iaq_accuracy != s_cached_bsec.iaq_accuracy;
      }
    }

    // Update cache
    s_cached_bsec = *out_values;
    s_bsec_cache_initialized = true;

    return ESP_OK;
  }
  return ESP_ERR_TIMEOUT;
}

void bsec_wrapper_init(void) {
  // Create mutex for data protection
  bsec_data_mutex = xSemaphoreCreateMutex();

  // I2C Initialized in App Main via bsp_i2c_init()
  ESP_LOGI(TAG, "BSEC I2C Helper Initialized (Mutex created)");
}

/*
 * Implementation of BME68x/BSEC platform callbacks
 */

BME68X_INTF_RET_TYPE bme68x_i2c_read(uint8_t reg_addr, uint8_t *reg_data,
                                     uint32_t len, void *intf_ptr) {
  uint8_t dev_addr = BME680_I2C_ADDR;

  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_WRITE, true);
  i2c_master_write_byte(cmd, reg_addr, true);
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_READ, true);
  if (len > 1) {
    i2c_master_read(cmd, reg_data, len - 1, I2C_MASTER_ACK);
  }
  i2c_master_read_byte(cmd, reg_data + len - 1, I2C_MASTER_NACK);
  i2c_master_stop(cmd);
  esp_err_t ret =
      i2c_master_cmd_begin(BSP_I2C_NUM, cmd, 1000 / portTICK_PERIOD_MS);
  i2c_cmd_link_delete(cmd);

  return (ret == ESP_OK) ? BME68X_OK : BME68X_E_COM_FAIL;
}

BME68X_INTF_RET_TYPE bme68x_i2c_write(uint8_t reg_addr, const uint8_t *reg_data,
                                      uint32_t len, void *intf_ptr) {
  uint8_t dev_addr = BME680_I2C_ADDR;

  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_WRITE, true);
  i2c_master_write_byte(cmd, reg_addr, true);
  i2c_master_write(cmd, reg_data, len, true);
  i2c_master_stop(cmd);
  esp_err_t ret =
      i2c_master_cmd_begin(BSP_I2C_NUM, cmd, 1000 / portTICK_PERIOD_MS);
  i2c_cmd_link_delete(cmd);

  return (ret == ESP_OK) ? BME68X_OK : BME68X_E_COM_FAIL;
}

void bme68x_delay_us(uint32_t period, void *intf_ptr) {
  uint32_t msec = period / 1000;
  if (msec < 1 && period > 0)
    msec = 1;
  vTaskDelay(msec / portTICK_PERIOD_MS);
}

int64_t bsec_get_timestamp_ns(void) { return esp_timer_get_time() * 1000; }

void bsec_task_impl(void *pvParameters) {
  bsec_library_return_t ret;
  bsec_version_t version_io;

  // Initialize BME68x interface
  bme_dev.intf = BME68X_I2C_INTF;
  bme_dev.read = bme68x_i2c_read;
  bme_dev.write = bme68x_i2c_write;
  bme_dev.delay_us = bme68x_delay_us;
  bme_dev.intf_ptr = NULL;
  bme_dev.amb_temp = 25; // Default ambient temp

  int8_t bme_ret = bme68x_init(&bme_dev);
  if (bme_ret != BME68X_OK) {
    ESP_LOGE(TAG, "BME68x init failed: %d", bme_ret);
    vTaskDelete(NULL);
    return;
  }
  ESP_LOGI(TAG, "BME68x initialized found chip ID: 0x%x", bme_dev.chip_id);

  // Initialize BSEC library
#include "bsec_config.h"

  // ... inside bsec_task_impl, after bsec_init ...
  ret = bsec_init();
  if (ret != BSEC_OK) {
    ESP_LOGE(TAG, "BSEC init failed: %d", ret);
    vTaskDelete(NULL);
    return;
  }

  // Set BSEC Configuration (Generic 3.3V 3s 4d)
  // This is required to match the LP sample rate (3s) and avoid the 5-min ULP
  // wait
  ret = bsec_set_configuration(bsec_config_iaq, bsec_config_iaq_len,
                               bsec_mem_block, BSEC_MAX_WORKBUFFER_SIZE);
  if (ret != BSEC_OK) {
    ESP_LOGE(TAG, "Failed to set BSEC configuration: %d", ret);
  } else {
    ESP_LOGI(TAG, "BSEC configuration loaded");
  }

  bsec_get_version(&version_io);
  ESP_LOGI(TAG, "BSEC version: %d.%d.%d.%d", version_io.major, version_io.minor,
           version_io.major_bugfix, version_io.minor_bugfix);

  // Update subscription to desired sensors
  float sample_rate = BSEC_SAMPLE_RATE_LP;
  // Load previous state if available
  bsec_load_state();

  bsec_sensor_configuration_t requested_virtual_sensors[TOTAL_VIRTUAL_SENSORS];
  uint8_t n_requested_virtual_sensors = TOTAL_VIRTUAL_SENSORS;

  // BSEC state variables
  bsec_bme_settings_t sensor_settings;
  memset(&sensor_settings, 0, sizeof(sensor_settings));

  // Main BSEC Loop
  while (1) {
    int64_t now_ns = bsec_get_timestamp_ns();

    // 1. Get next sensor settings from BSEC
    //    We check what BSEC wants us to do (sleep, force measure, etc)
    //    Usually we call bsec_sensor_control with the current timestamp.

    int64_t timestamp_ns = now_ns;
    ret = bsec_sensor_control(timestamp_ns, &sensor_settings);
    // ...

    requested_virtual_sensors[0].sensor_id = BSEC_OUTPUT_IAQ;
    requested_virtual_sensors[0].sample_rate = sample_rate;
    requested_virtual_sensors[1].sensor_id = BSEC_OUTPUT_STATIC_IAQ;
    requested_virtual_sensors[1].sample_rate = sample_rate;
    requested_virtual_sensors[2].sensor_id = BSEC_OUTPUT_CO2_EQUIVALENT;
    requested_virtual_sensors[2].sample_rate = sample_rate;
    requested_virtual_sensors[3].sensor_id = BSEC_OUTPUT_BREATH_VOC_EQUIVALENT;
    requested_virtual_sensors[3].sample_rate = sample_rate;
    requested_virtual_sensors[4].sensor_id = BSEC_OUTPUT_RAW_TEMPERATURE;
    requested_virtual_sensors[4].sample_rate = sample_rate;
    requested_virtual_sensors[5].sensor_id = BSEC_OUTPUT_RAW_PRESSURE;
    requested_virtual_sensors[5].sample_rate = sample_rate;
    requested_virtual_sensors[6].sensor_id = BSEC_OUTPUT_RAW_HUMIDITY;
    requested_virtual_sensors[6].sample_rate = sample_rate;
    requested_virtual_sensors[7].sensor_id = BSEC_OUTPUT_RAW_GAS;
    requested_virtual_sensors[7].sample_rate = sample_rate;
    requested_virtual_sensors[8].sensor_id = BSEC_OUTPUT_STABILIZATION_STATUS;
    requested_virtual_sensors[8].sample_rate = sample_rate;
    requested_virtual_sensors[9].sensor_id = BSEC_OUTPUT_RUN_IN_STATUS;
    requested_virtual_sensors[9].sample_rate = sample_rate;

    requested_virtual_sensors[10].sensor_id =
        BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE;
    requested_virtual_sensors[10].sample_rate = sample_rate;
    requested_virtual_sensors[11].sensor_id =
        BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY;
    requested_virtual_sensors[11].sample_rate = sample_rate;
    requested_virtual_sensors[12].sensor_id = BSEC_OUTPUT_COMPENSATED_GAS;
    requested_virtual_sensors[12].sample_rate = sample_rate;
    requested_virtual_sensors[13].sensor_id = BSEC_OUTPUT_GAS_PERCENTAGE;
    requested_virtual_sensors[13].sample_rate = sample_rate;

    bsec_sensor_configuration_t
        required_sensor_settings[BSEC_MAX_PHYSICAL_SENSOR];
    uint8_t n_required_sensor_settings = BSEC_MAX_PHYSICAL_SENSOR;

    ret = bsec_update_subscription(
        requested_virtual_sensors, n_requested_virtual_sensors,
        required_sensor_settings, &n_required_sensor_settings);

    if (ret < BSEC_OK) {
      ESP_LOGE(TAG, "BSEC update subscription failed: %d", ret);
    } else if (ret > BSEC_OK) {
      ESP_LOGW(TAG, "BSEC update subscription warning: %d", ret);
    }

    // Prepare temporary storage for atomic update
    bsec_values_t temp_values = {0};
    temp_values.timestamp = bsec_get_timestamp_ns();

    // BSEC state variables
    bsec_bme_settings_t sensor_settings;
    memset(&sensor_settings, 0, sizeof(sensor_settings));

    // Main BSEC Loop
    while (1) {
      // 1. Get next sensor settings from BSEC
      int64_t timestamp_ns = bsec_get_timestamp_ns();
      ret = bsec_sensor_control(timestamp_ns, &sensor_settings);

      // Save state periodically (moved here since inner loop never exits)
      static int64_t last_save_time_ns = 0;
      if (last_save_time_ns == 0) {
        last_save_time_ns = timestamp_ns;
      }
      if ((timestamp_ns - last_save_time_ns) >= BSEC_STATE_SAVE_INTERVAL_NS) {
        bsec_save_state();
        last_save_time_ns = timestamp_ns;
      }

      if (ret < BSEC_OK) {
        ESP_LOGE(TAG, "bsec_sensor_control failed: %d", ret);
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      } else if (ret > BSEC_OK) {
        if (ret != 100)
          ESP_LOGW(TAG, "bsec_sensor_control warning: %d", ret);
      }

      // 2. Configure BME68x based on settings
      struct bme68x_conf conf;
      struct bme68x_heatr_conf heatr_conf;
      struct bme68x_data data;
      uint8_t n_fields;

      if (sensor_settings.trigger_measurement) {
        conf.os_hum = sensor_settings.humidity_oversampling;
        conf.os_temp = sensor_settings.temperature_oversampling;
        conf.os_pres = sensor_settings.pressure_oversampling;
        conf.filter = BME68X_FILTER_OFF;
        conf.odr = BME68X_ODR_NONE;

        bme68x_set_conf(&conf, &bme_dev);

        heatr_conf.enable =
            sensor_settings.run_gas ? BME68X_ENABLE : BME68X_DISABLE;
        heatr_conf.heatr_temp = sensor_settings.heater_temperature;
        heatr_conf.heatr_dur = sensor_settings.heater_duration;
        heatr_conf.heatr_temp_prof = sensor_settings.heater_temperature_profile;
        heatr_conf.heatr_dur_prof = sensor_settings.heater_duration_profile;
        heatr_conf.profile_len = sensor_settings.heater_profile_len;

        bme68x_set_heatr_conf(BME68X_FORCED_MODE, &heatr_conf, &bme_dev);
        bme68x_set_op_mode(BME68X_FORCED_MODE, &bme_dev);

        uint32_t del_period =
            bme68x_get_meas_dur(BME68X_FORCED_MODE, &conf, &bme_dev) +
            (heatr_conf.heatr_dur * 1000);
        bme_dev.delay_us(del_period, bme_dev.intf_ptr);

        bme_ret =
            bme68x_get_data(BME68X_FORCED_MODE, &data, &n_fields, &bme_dev);

        if (bme_ret == BME68X_OK && n_fields > 0) {
          bsec_input_t inputs[BSEC_MAX_PHYSICAL_SENSOR];
          uint8_t n_inputs = 0;
          int64_t timestamp_now = bsec_get_timestamp_ns();

          if (sensor_settings.process_data & BSEC_PROCESS_TEMPERATURE) {
            inputs[n_inputs].sensor_id = BSEC_INPUT_TEMPERATURE;
            inputs[n_inputs].signal = data.temperature;
            inputs[n_inputs].time_stamp = timestamp_now;
            n_inputs++;
          }
          if (sensor_settings.process_data & BSEC_PROCESS_HUMIDITY) {
            inputs[n_inputs].sensor_id = BSEC_INPUT_HUMIDITY;
            inputs[n_inputs].signal = data.humidity;
            inputs[n_inputs].time_stamp = timestamp_now;
            n_inputs++;
          }
          if (sensor_settings.process_data & BSEC_PROCESS_PRESSURE) {
            inputs[n_inputs].sensor_id = BSEC_INPUT_PRESSURE;
            inputs[n_inputs].signal = data.pressure;
            inputs[n_inputs].time_stamp = timestamp_now;
            n_inputs++;
          }
          if (sensor_settings.process_data & BSEC_PROCESS_GAS) {
            inputs[n_inputs].sensor_id = BSEC_INPUT_GASRESISTOR;
            inputs[n_inputs].signal = data.gas_resistance;
            inputs[n_inputs].time_stamp = timestamp_now;
            n_inputs++;
          }

          bsec_output_t outputs[BSEC_NUMBER_OUTPUTS];
          uint8_t n_outputs = BSEC_NUMBER_OUTPUTS;

          ret = bsec_do_steps(inputs, n_inputs, outputs, &n_outputs);

          if (ret == BSEC_OK) {
            // printf("--- BSEC Outputs ---\n");
            for (int i = 0; i < n_outputs; i++) {
              switch (outputs[i].sensor_id) {
              case BSEC_OUTPUT_IAQ:
                temp_values.iaq = outputs[i].signal;
                temp_values.iaq_accuracy = outputs[i].accuracy;
                // printf("IAQ: %.2f (Accuracy: %d)\n", outputs[i].signal,
                // outputs[i].accuracy);
                break;
              case BSEC_OUTPUT_STATIC_IAQ:
                temp_values.static_iaq = outputs[i].signal;
                // printf("Static IAQ: %.2f\n", outputs[i].signal);
                break;
              case BSEC_OUTPUT_CO2_EQUIVALENT:
                temp_values.co2_equivalent = outputs[i].signal;
                // printf("CO2 Equivalent: %.2f ppm\n", outputs[i].signal);
                break;
              case BSEC_OUTPUT_BREATH_VOC_EQUIVALENT:
                temp_values.breath_voc_equivalent = outputs[i].signal;
                // printf("Breath VOC: %.2f ppm\n", outputs[i].signal);
                break;
              case BSEC_OUTPUT_RAW_TEMPERATURE:
                temp_values.raw_temperature = outputs[i].signal;
                // printf("Temperature: %.2f C\n", outputs[i].signal);
                break;
              case BSEC_OUTPUT_RAW_PRESSURE:
                temp_values.raw_pressure = outputs[i].signal;
                // printf("Pressure: %.2f Pa\n", outputs[i].signal);
                break;
              case BSEC_OUTPUT_RAW_HUMIDITY:
                temp_values.raw_humidity = outputs[i].signal;
                // printf("Humidity: %.2f %%\n", outputs[i].signal);
                break;
              case BSEC_OUTPUT_RAW_GAS:
                temp_values.raw_gas = outputs[i].signal;
                // printf("Gas Resistance: %.2f Ohm\n", outputs[i].signal);
                break;
              case BSEC_OUTPUT_STABILIZATION_STATUS:
                temp_values.stabilization_status = (outputs[i].signal > 0.0f);
                // printf("Stabilization Status: %s\n", outputs[i].signal ?
                // "Stable" : "Stabilizing");
                break;
              case BSEC_OUTPUT_RUN_IN_STATUS:
                temp_values.run_in_status = (outputs[i].signal > 0.0f);
                // printf("Run-in Status: %s\n", outputs[i].signal ? "Finished"
                // : "Ongoing");
                break;
              case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE:
                temp_values.comp_temperature = outputs[i].signal;
                // printf("Compensated Temp: %.2f C\n", outputs[i].signal);
                break;
              case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY:
                temp_values.comp_humidity = outputs[i].signal;
                // printf("Compensated Hum: %.2f %%\n", outputs[i].signal);
                break;
              case BSEC_OUTPUT_COMPENSATED_GAS:
                temp_values.comp_gas = outputs[i].signal;
                // printf("Compensated Gas: %.2f Ohm\n", outputs[i].signal);
                break;
              case BSEC_OUTPUT_GAS_PERCENTAGE:
                temp_values.gas_percentage = outputs[i].signal;
                // printf("Gas Percentage: %.2f %%\n", outputs[i].signal);
                break;
              default:
                break;
              }
            }

            // Atomically update global state
            if (bsec_data_mutex &&
                xSemaphoreTake(bsec_data_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
              bsec_latest_values = temp_values;
              s_last_update_ts = esp_timer_get_time();
              xSemaphoreGive(bsec_data_mutex);
            }

            // printf("--------------------\n");
          } else {
            ESP_LOGW(TAG, "bsec_do_steps failed: %d", ret);
          }
        }
      } else {
        int64_t next_call_ns = sensor_settings.next_call;
        int64_t now_ns = bsec_get_timestamp_ns();
        if (next_call_ns > now_ns) {
          int64_t wait_us = (next_call_ns - now_ns) / 1000;
          vTaskDelay(pdMS_TO_TICKS(wait_us / 1000));
        } else {
          vTaskDelay(pdMS_TO_TICKS(10));
        }
      }
    }
    ESP_LOGI(TAG, "BSEC Stack Remaining: %u bytes\n",
             uxTaskGetStackHighWaterMark(NULL));
  }
}

// Increased stack size to handle deep call chains + printf buffers
void bsec_start_task(void) {
  xTaskCreate(bsec_task_impl, "bsec_task", BSEC_TASK_STACK_SIZE, NULL,
              BSEC_TASK_PRIORITY, NULL);
}
