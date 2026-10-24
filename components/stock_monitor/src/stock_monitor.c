#include "stock_monitor.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h" // For getting timestamp
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "wifi_connect.h"
#include <ctype.h>
#include <string.h>


static const char *TAG = "STOCK";

// NVS namespace and keys
#define STOCK_NVS_NAMESPACE "stock"
#define STOCK_NVS_KEY_SYM1 "sym1"
#define STOCK_NVS_KEY_SYM2 "sym2"

// Dynamic stock symbols (max 15 chars + null) - initialized with defaults
static char s_stock_symbol_1[16] = STOCK_DEFAULT_SYMBOL_1;
static char s_stock_symbol_2[16] = STOCK_DEFAULT_SYMBOL_2;

// Finnhub API URL Format:
// https://finnhub.io/api/v1/quote?symbol=SYMBOL&token=API_KEY
#define FINNHUB_URL_TEMPLATE                                                   \
  "https://finnhub.io/api/v1/quote?symbol=%s&token=%s"

// Thread-safe state storage
static stock_values_t stock_cache = {0};
static SemaphoreHandle_t stock_mutex = NULL;

// Helper to safely get double (or int) from JSON
static double get_json_double(cJSON *json, const char *key) {
  cJSON *item = cJSON_GetObjectItem(json, key);
  if (cJSON_IsNumber(item)) {
    return item->valuedouble;
  }
  return 0.0;
}

esp_err_t stock_fetch_price(const char *symbol, stock_data_t *out_data) {
  if (!symbol || !symbol[0]) return ESP_ERR_INVALID_ARG;
  char ticker[sizeof(s_stock_symbol_1)];
  size_t symbol_len = strlen(symbol);
  if (symbol_len >= sizeof(ticker)) return ESP_ERR_INVALID_SIZE;
  for (size_t i = 0; i <= symbol_len; ++i) {
    ticker[i] = (char)toupper((unsigned char)symbol[i]);
  }

  char url[256];
  snprintf(url, sizeof(url), FINNHUB_URL_TEMPLATE, ticker,
           CONFIG_FINNHUB_API_KEY);

  char *response_buffer = NULL;
  esp_err_t err = wifi_perform_http_get(url, &response_buffer);

  if (err == ESP_OK && response_buffer != NULL) {
    cJSON *json = cJSON_Parse(response_buffer);
    if (json) {
      cJSON *price = cJSON_GetObjectItem(json, "c");
      if (cJSON_IsNumber(price) && price->valuedouble > 0) {
        if (out_data) {
          out_data->current_price = (float)get_json_double(json, "c");
          out_data->change = (float)get_json_double(json, "d");
          out_data->percent_change = (float)get_json_double(json, "dp");
          out_data->high_price = (float)get_json_double(json, "h");
          out_data->low_price = (float)get_json_double(json, "l");
          out_data->open_price = (float)get_json_double(json, "o");
          out_data->previous_close = (float)get_json_double(json, "pc");
          // Timestamp 't' is int
          cJSON *ts = cJSON_GetObjectItem(json, "t");
          if (cJSON_IsNumber(ts)) {
            out_data->timestamp = (int64_t)ts->valuedouble;
          }
        }
        ESP_LOGI(TAG, "Stock %s: $%.2f", ticker,
                 out_data ? out_data->current_price : 0.0f);
        err = ESP_OK;
      } else {
        ESP_LOGW(TAG, "No valid quote for %s", ticker);
        err = ESP_FAIL;
      }
      cJSON_Delete(json);
    } else {
      ESP_LOGE(TAG, "Failed to parse JSON");
      err = ESP_FAIL;
    }
    free(response_buffer);
  } else {
    if (err == ESP_OK)
      err = ESP_FAIL;
  }

  return err;
}

// Static cache for change detection
// Static cache for change detection
static stock_values_t s_cached_stock = {0};
static bool s_stock_cache_initialized = false;
static int64_t s_last_update_ts = 0;

int64_t stock_get_last_updated_time(void) { return s_last_update_ts; }

// Refresh flag
static volatile bool s_refresh_requested = false;

void stock_trigger_refresh(void) {
  s_refresh_requested = true;
  ESP_LOGI(TAG, "Stock refresh requested");
}

esp_err_t stock_get_latest_values(stock_values_t *out_values,
                                  bool *is_updated) {
  if (!stock_mutex)
    return ESP_ERR_INVALID_STATE;

  // Initialize is_updated to false
  if (is_updated)
    *is_updated = false;

  if (xSemaphoreTake(stock_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    *out_values = stock_cache;
    xSemaphoreGive(stock_mutex);

    // Check if data changed compared to cache
    if (is_updated) {
      if (!s_stock_cache_initialized) {
        *is_updated = true; // First fetch, always updated
      } else {
        // Compare prices at 2 decimal precision
        *is_updated = (int)(out_values->stock_1.current_price * 100) !=
                          (int)(s_cached_stock.stock_1.current_price * 100) ||
                      (int)(out_values->stock_2.current_price * 100) !=
                          (int)(s_cached_stock.stock_2.current_price * 100);
      }
    }

    // Update cache
    s_cached_stock = *out_values;
    s_stock_cache_initialized = true;

    return ESP_OK;
  }
  return ESP_ERR_TIMEOUT;
}

void stock_load_symbols_from_nvs(void) {
  nvs_handle_t handle;
  if (nvs_open(STOCK_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
    size_t len = sizeof(s_stock_symbol_1);
    if (nvs_get_str(handle, STOCK_NVS_KEY_SYM1, s_stock_symbol_1, &len) ==
        ESP_OK) {
      ESP_LOGI(TAG, "Loaded symbol 1: %s", s_stock_symbol_1);
    }
    len = sizeof(s_stock_symbol_2);
    if (nvs_get_str(handle, STOCK_NVS_KEY_SYM2, s_stock_symbol_2, &len) ==
        ESP_OK) {
      ESP_LOGI(TAG, "Loaded symbol 2: %s", s_stock_symbol_2);
    }
    nvs_close(handle);
  }
}

void stock_save_symbols_to_nvs(void) {
  nvs_handle_t handle;
  if (nvs_open(STOCK_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
    nvs_set_str(handle, STOCK_NVS_KEY_SYM1, s_stock_symbol_1);
    nvs_set_str(handle, STOCK_NVS_KEY_SYM2, s_stock_symbol_2);
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Saved symbols to NVS");
  }
}

esp_err_t stock_set_symbol(int slot, const char *symbol) {
  if (slot < 1 || slot > 2 || !symbol)
    return ESP_ERR_INVALID_ARG;
  if (strlen(symbol) >= 16)
    return ESP_ERR_INVALID_SIZE;

  if (slot == 1) {
    strncpy(s_stock_symbol_1, symbol, sizeof(s_stock_symbol_1) - 1);
    s_stock_symbol_1[sizeof(s_stock_symbol_1) - 1] = '\0';
  } else {
    strncpy(s_stock_symbol_2, symbol, sizeof(s_stock_symbol_2) - 1);
    s_stock_symbol_2[sizeof(s_stock_symbol_2) - 1] = '\0';
  }

  stock_save_symbols_to_nvs();
  return ESP_OK;
}

esp_err_t stock_get_symbol(int slot, char *out_symbol, size_t len) {
  if (slot < 1 || slot > 2 || !out_symbol || len == 0)
    return ESP_ERR_INVALID_ARG;

  const char *src = (slot == 1) ? s_stock_symbol_1 : s_stock_symbol_2;
  strncpy(out_symbol, src, len - 1);
  out_symbol[len - 1] = '\0';
  return ESP_OK;
}

void stock_monitor_init(void) {
  if (stock_mutex == NULL) {
    stock_mutex = xSemaphoreCreateMutex();
  }
  // Load saved symbols from NVS
  stock_load_symbols_from_nvs();
}

void stock_monitor_task(void *pvParameters) {

  // Wait for WiFi
  while (!wifi_is_connected()) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  while (1) {
    stock_data_t data1 = {0};
    stock_data_t data2 = {0};
    bool ok1 = false;
    bool ok2 = false;

    // Fetch Stock 1
    ESP_LOGI(TAG, "Fetching stock: %s", s_stock_symbol_1);
    if (stock_fetch_price(s_stock_symbol_1, &data1) == ESP_OK) {
      ok1 = true;
    } else {
      ESP_LOGW(TAG, "Failed to fetch %s", s_stock_symbol_1);
    }

    // Small delay between requests to be nice to API
    // Also gives the mutex a chance to be taken by weather task if it's waiting
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Fetch Stock 2
    ESP_LOGI(TAG, "Fetching stock: %s", s_stock_symbol_2);
    if (stock_fetch_price(s_stock_symbol_2, &data2) == ESP_OK) {
      ok2 = true;
    } else {
      ESP_LOGW(TAG, "Failed to fetch %s", s_stock_symbol_2);
    }

    // ... existing code ...

    if (ok1 || ok2) {
      if (stock_mutex &&
          xSemaphoreTake(stock_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (ok1)
          stock_cache.stock_1 = data1;
        if (ok2)
          stock_cache.stock_2 = data2;
        stock_cache.last_update_time_us = esp_timer_get_time();
        s_last_update_ts = stock_cache.last_update_time_us;
        xSemaphoreGive(stock_mutex);
        ESP_LOGI(TAG, "Stock cache updated");
      }
    }

    ESP_LOGI(TAG, "Stock Stack Remaining: %lu bytes\n",
             (unsigned long)uxTaskGetStackHighWaterMark(NULL));
    // Wait 5 minutes, but check for refresh requests
    int wait_count = 0;
    while (wait_count < 300 && !s_refresh_requested) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      wait_count++;
    }
    s_refresh_requested = false; // Clear the flag
  }
}

void stock_monitor_start(void) {
  if (stock_mutex == NULL) {
    stock_monitor_init();
  }
  xTaskCreatePinnedToCore(stock_monitor_task, "stock_mon", 6144, NULL, 1, NULL,
                          0);
}
