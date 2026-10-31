#include "stock_monitor.h"
#include "time_manager.h"
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

bool stock_monitor_is_market_open(time_t utc_epoch) {
  struct tm ny;
  if (!time_manager_get_new_york_time(utc_epoch, &ny)) return false;
  int minute = ny.tm_hour * 60 + ny.tm_min;
  // shortcut: holidays and early closes are ignored; add a calendar if required.
  return ny.tm_wday >= 1 && ny.tm_wday <= 5 &&
         minute >= 9 * 60 + 30 && minute < 16 * 60;
}

// Helper to safely get double (or int) from JSON
static double get_json_double(cJSON *json, const char *key) {
  cJSON *item = cJSON_GetObjectItem(json, key);
  if (cJSON_IsNumber(item)) {
    return item->valuedouble;
  }
  return 0.0;
}

static esp_err_t stock_build_url(const char *symbol, char *url, size_t url_size) {
  if (!symbol || !symbol[0]) return ESP_ERR_INVALID_ARG;
  char ticker[sizeof(s_stock_symbol_1)];
  size_t symbol_len = strlen(symbol);
  if (symbol_len >= sizeof(ticker)) return ESP_ERR_INVALID_SIZE;
  for (size_t i = 0; i <= symbol_len; ++i) {
    ticker[i] = (char)toupper((unsigned char)symbol[i]);
  }

  int length = snprintf(url, url_size, FINNHUB_URL_TEMPLATE, ticker,
           CONFIG_FINNHUB_API_KEY);

  return length < 0 || (size_t)length >= url_size ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

static esp_err_t stock_parse_price(const char *symbol, const char *response,
                                    stock_data_t *out_data) {
  if (!response) return ESP_FAIL;
  esp_err_t err;
  cJSON *json = cJSON_Parse(response);
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
      ESP_LOGI(TAG, "Stock %s: $%.2f", symbol,
               out_data ? out_data->current_price : 0.0f);
      err = ESP_OK;
    } else {
      ESP_LOGW(TAG, "No valid quote for %s", symbol);
      err = ESP_FAIL;
    }
    cJSON_Delete(json);
  } else {
    ESP_LOGE(TAG, "Failed to parse JSON");
    err = ESP_FAIL;
  }
  return err;
}

esp_err_t stock_fetch_price(const char *symbol, stock_data_t *out_data) {
  char url[256];
  esp_err_t err = stock_build_url(symbol, url, sizeof(url));
  if (err != ESP_OK) return err;
  char *response = NULL;
  err = wifi_perform_http_get(url, &response);
  if (err == ESP_OK) err = stock_parse_price(symbol, response, out_data);
  free(response);
  return err;
}

// Static cache for change detection
// Static cache for change detection
static stock_values_t s_cached_stock = {0};
static bool s_stock_cache_initialized = false;
static int64_t s_last_update_ts = 0;

int64_t stock_get_last_updated_time(void) { return s_last_update_ts; }

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
  char symbol_1[sizeof(s_stock_symbol_1)];
  char symbol_2[sizeof(s_stock_symbol_2)];
  if (stock_mutex && xSemaphoreTake(stock_mutex, portMAX_DELAY) == pdTRUE) {
    memcpy(symbol_1, s_stock_symbol_1, sizeof(symbol_1));
    memcpy(symbol_2, s_stock_symbol_2, sizeof(symbol_2));
    xSemaphoreGive(stock_mutex);
  } else {
    return;
  }

  nvs_handle_t handle;
  if (nvs_open(STOCK_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
    nvs_set_str(handle, STOCK_NVS_KEY_SYM1, symbol_1);
    nvs_set_str(handle, STOCK_NVS_KEY_SYM2, symbol_2);
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

  if (!stock_mutex || xSemaphoreTake(stock_mutex, portMAX_DELAY) != pdTRUE)
    return ESP_ERR_INVALID_STATE;
  char *target = (slot == 1) ? s_stock_symbol_1 : s_stock_symbol_2;
  strlcpy(target, symbol, 16);
  xSemaphoreGive(stock_mutex);

  stock_save_symbols_to_nvs();
  return ESP_OK;
}

esp_err_t stock_get_symbol(int slot, char *out_symbol, size_t len) {
  if (slot < 1 || slot > 2 || !out_symbol || len == 0)
    return ESP_ERR_INVALID_ARG;

  if (!stock_mutex || xSemaphoreTake(stock_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    return ESP_ERR_TIMEOUT;
  const char *src = (slot == 1) ? s_stock_symbol_1 : s_stock_symbol_2;
  strncpy(out_symbol, src, len - 1);
  out_symbol[len - 1] = '\0';
  xSemaphoreGive(stock_mutex);
  return ESP_OK;
}

void stock_monitor_init(void) {
  if (stock_mutex == NULL) {
    stock_mutex = xSemaphoreCreateMutex();
  }
  // Load saved symbols from NVS
  stock_load_symbols_from_nvs();
}

esp_err_t stock_monitor_refresh(void) {
  if (!stock_mutex) return ESP_ERR_INVALID_STATE;

  char symbol_1[sizeof(s_stock_symbol_1)];
  char symbol_2[sizeof(s_stock_symbol_2)];
  if (xSemaphoreTake(stock_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    return ESP_ERR_TIMEOUT;
  memcpy(symbol_1, s_stock_symbol_1, sizeof(symbol_1));
  memcpy(symbol_2, s_stock_symbol_2, sizeof(symbol_2));
  xSemaphoreGive(stock_mutex);

  stock_data_t data1 = {0};
  stock_data_t data2 = {0};
  char url1[256], url2[256];
  esp_err_t result1 = stock_build_url(symbol_1, url1, sizeof(url1));
  esp_err_t result2 = stock_build_url(symbol_2, url2, sizeof(url2));
  char *responses[2] = {NULL, NULL};
  esp_err_t results[2] = {result1, result2};
  if (result1 == ESP_OK && result2 == ESP_OK) {
    const char *urls[] = {url1, url2};
    wifi_perform_http_get_batch(urls, responses, results, 2);
  } else {
    // An invalid configured symbol must not prevent refreshing the other quote.
    if (result1 == ESP_OK) results[0] = wifi_perform_http_get(url1, &responses[0]);
    if (result2 == ESP_OK) results[1] = wifi_perform_http_get(url2, &responses[1]);
  }
  bool ok1 = results[0] == ESP_OK && stock_parse_price(symbol_1, responses[0], &data1) == ESP_OK;
  bool ok2 = results[1] == ESP_OK && stock_parse_price(symbol_2, responses[1], &data2) == ESP_OK;
  free(responses[0]);
  free(responses[1]);
  if (!ok1) ESP_LOGW(TAG, "Failed to fetch %s", symbol_1);
  if (!ok2) ESP_LOGW(TAG, "Failed to fetch %s", symbol_2);

  bool updated = false;
  if (xSemaphoreTake(stock_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    return ESP_ERR_TIMEOUT;
  if (ok1 && strcmp(s_stock_symbol_1, symbol_1) == 0) {
    stock_cache.stock_1 = data1;
    updated = true;
  }
  if (ok2 && strcmp(s_stock_symbol_2, symbol_2) == 0) {
    stock_cache.stock_2 = data2;
    updated = true;
  }
  if (updated) {
    stock_cache.last_update_time_us = esp_timer_get_time();
    s_last_update_ts = stock_cache.last_update_time_us;
  }
  xSemaphoreGive(stock_mutex);

  if (updated) ESP_LOGI(TAG, "Stock cache updated");
  return updated ? ESP_OK : ESP_FAIL;
}
