#pragma once

#include "esp_err.h"
#include <stdbool.h>

// Default stock symbols (used when NVS is empty)
#define STOCK_DEFAULT_SYMBOL_1 "QCOM"
#define STOCK_DEFAULT_SYMBOL_2 "SPY"

/**
 * @brief Detailed stock information from Finnhub API
 */
typedef struct {
  float current_price;  // "c"
  float change;         // "d"
  float percent_change; // "dp"
  float high_price;     // "h"
  float low_price;      // "l"
  float open_price;     // "o"
  float previous_close; // "pc"
  int64_t timestamp;    // "t"
} stock_data_t;

/**
 * @brief Structure holding cached values for both tracked stocks
 */
typedef struct {
  stock_data_t stock_1;
  stock_data_t stock_2;
  int64_t last_update_time_us;
} stock_values_t;

/**
 * @brief Task for monitoring stock prices
 *
 * Fetches data from Finnhub API every 5 minutes.
 */
void stock_monitor_task(void *pvParameters);

/**
 * @brief Initialize stock monitor (mutex, load symbols from NVS)
 */
void stock_monitor_init(void);

/**
 * @brief Start the stock monitor background task
 */
void stock_monitor_start(void);

/**
 * @brief Get the latest cached stock values in a thread-safe manner
 *
 * @param[out] out_values Pointer to struct to be filled with data
 * @param[out] is_updated Pointer to bool, set to true if data changed from last
 * call
 * @return ESP_OK if successful, ESP_ERR_TIMEOUT if mutex timeout
 */
esp_err_t stock_get_latest_values(stock_values_t *out_values, bool *is_updated);

/**
 * @brief Get the last update timestamp (us) to check for changes without
 * locking
 * @return int64_t Timestamp in microseconds
 */
int64_t stock_get_last_updated_time(void);

/**
 * @brief Fetch the current price of a stock on demand
 *
 * This is a blocking call that performs an HTTP request.
 *
 * @param[in] symbol Stock symbol (e.g. "AAPL")
 * @param[out] out_data Pointer to stock_data_t to store the data
 * @return ESP_OK if successful, ESP_FAIL otherwise
 */
esp_err_t stock_fetch_price(const char *symbol, stock_data_t *out_data);

/**
 * @brief Trigger an immediate stock data refresh
 */
void stock_trigger_refresh(void);
/**
 * @brief Set the stock symbol for a specific slot
 * @param slot 1 or 2
 * @param symbol Stock symbol (max 15 chars)
 * @return ESP_OK on success
 */
esp_err_t stock_set_symbol(int slot, const char *symbol);

/**
 * @brief Get the current stock symbol for a slot
 * @param slot 1 or 2
 * @param out_symbol Buffer to store symbol
 * @param len Buffer length
 * @return ESP_OK on success
 */
esp_err_t stock_get_symbol(int slot, char *out_symbol, size_t len);

/**
 * @brief Load stock symbols from NVS (called automatically in init)
 */
void stock_load_symbols_from_nvs(void);

/**
 * @brief Save stock symbols to NVS
 */
void stock_save_symbols_to_nvs(void);
