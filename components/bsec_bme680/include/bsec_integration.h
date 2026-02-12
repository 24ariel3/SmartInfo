#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// BSEC Integration API

/**
 * @brief Initialize BSEC wrapper (mutex only)
 */
void bsec_wrapper_init(void);

/**
 * @brief Start the BSEC processing task
 */
void bsec_start_task(void);

/**
 * @brief Structure holding latest BSEC sensor values
 */
typedef struct {
  int64_t timestamp;           /*!< Timestamp in microseconds */
  float iaq;                   /*!< IAQ index (0-500) */
  uint8_t iaq_accuracy;        /*!< IAQ accuracy (0-3) */
  float static_iaq;            /*!< Static IAQ (unscaled) */
  float co2_equivalent;        /*!< CO2 equivalent (ppm) */
  float breath_voc_equivalent; /*!< Breath VOC equivalent (ppm) */
  float raw_temperature;       /*!< Raw temperature (degrees C) */
  float raw_pressure;          /*!< Raw pressure (Pa) */
  float raw_humidity;          /*!< Raw humidity (%rH) */
  float raw_gas;               /*!< Raw gas resistance (Ohms) */
  float comp_temperature;      /*!< Heat compensated temperature (C) */
  float comp_humidity;         /*!< Heat compensated humidity (%rH) */
  float comp_gas;              /*!< Compensated gas resistance (Ohms) */
  float gas_percentage;        /*!< Gas percentage */
  bool stabilization_status;   /*!< True if stable */
  bool run_in_status;          /*!< True if run-in finished */
} bsec_values_t;

/**
 * @brief Get the latest sensor values in a thread-safe manner
 *
 * @param[out] out_values Pointer to struct to be filled with data
 * @param[out] is_updated Pointer to bool, set to true if data changed from last
 * call
 * @return ESP_OK if successful, ESP_ERR_TIMEOUT if mutex timeout
 */
esp_err_t bsec_get_latest_values(bsec_values_t *out_values, bool *is_updated);

/**
 * @brief Get the last update timestamp (us) to check for changes without
 * locking
 * @return int64_t Timestamp in microseconds
 */
int64_t bsec_get_last_updated_time(void);

#ifdef __cplusplus
}
#endif
