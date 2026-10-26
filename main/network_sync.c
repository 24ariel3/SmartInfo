#include "network_sync.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "stock_monitor.h"
#include "time_manager.h"
#include "weather.h"
#include "wifi_connect.h"
#include <time.h>

static const char *TAG = "NETWORK_SYNC";
#define STOCK_INTERVAL_US (5LL * 60 * 1000000)
#define HOURLY_INTERVAL_US (60LL * 60 * 1000000)
#define RETRY_INTERVAL_US STOCK_INTERVAL_US

static portMUX_TYPE s_request_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_pending_work;
static TaskHandle_t s_task;
static int64_t s_next_stock_us;
static int64_t s_next_weather_us;
static int64_t s_next_time_us;

static int64_t next_hour_us(int64_t now_us) {
  time_t now;
  struct tm timeinfo;
  time(&now);
  localtime_r(&now, &timeinfo);
  int seconds = (60 - timeinfo.tm_min) * 60 - timeinfo.tm_sec;
  if (seconds <= 0) seconds = 3600;
  return now_us + (int64_t)seconds * 1000000;
}

void network_sync_request(uint32_t work) {
  const uint32_t valid = NETWORK_SYNC_STOCKS | NETWORK_SYNC_WEATHER;
  configASSERT((work & ~valid) == 0);
  portENTER_CRITICAL(&s_request_lock);
  s_pending_work |= work & valid;
  portEXIT_CRITICAL(&s_request_lock);
  if (s_task) xTaskNotifyGive(s_task);
}

static uint32_t take_requests(void) {
  portENTER_CRITICAL(&s_request_lock);
  uint32_t work = s_pending_work;
  s_pending_work = 0;
  portEXIT_CRITICAL(&s_request_lock);
  return work;
}

static uint32_t get_due_work(int64_t now_us) {
  uint32_t work = take_requests();
  if (now_us >= s_next_stock_us) work |= NETWORK_SYNC_STOCKS;
  if (now_us >= s_next_weather_us) work |= NETWORK_SYNC_WEATHER;
  return work;
}

static TickType_t wait_until_next_work(int64_t now_us) {
  int64_t next = s_next_stock_us;
  if (s_next_weather_us < next) next = s_next_weather_us;
  if (s_next_time_us < next) next = s_next_time_us;
  if (next <= now_us) return 1;
  uint64_t wait_ms = (uint64_t)(next - now_us) / 1000;
  TickType_t ticks = pdMS_TO_TICKS(wait_ms);
  return ticks == 0 ? 1 : ticks;
}

static void network_sync_task(void *arg) {
  (void)arg;
  while (1) {
    int64_t now_us = esp_timer_get_time();
    uint32_t work = get_due_work(now_us);
    bool time_due = now_us >= s_next_time_us;
    if (work == 0 && !time_due) {
      uint32_t ignored;
      xTaskNotifyWait(0, UINT32_MAX, &ignored,
                      wait_until_next_work(now_us));
      continue;
    }

    esp_err_t hold_err = wifi_power_acquire(WIFI_POWER_SYNC);
    bool connected = hold_err == ESP_OK && wifi_wait_connected(30000);
    bool time_valid = false;
    esp_err_t time_err = hold_err;
    bool time_attempted = false;
    if (connected && (time_due || !time_manager_has_valid_time())) {
      time_attempted = true;
      time_err = time_manager_sync(10000);
      time_valid = time_manager_has_valid_time();
    } else {
      time_valid = time_manager_has_valid_time();
    }

    bool stock_ok = false;
    bool weather_ok = false;
    if (connected && time_valid) {
      if (work & NETWORK_SYNC_STOCKS) {
        stock_ok = stock_monitor_refresh() == ESP_OK;
        if (!stock_ok) ESP_LOGW(TAG, "Stock refresh failed");
      }
      if (work & NETWORK_SYNC_WEATHER) {
        weather_ok = weather_refresh() == ESP_OK;
        if (!weather_ok) ESP_LOGW(TAG, "Weather refresh failed");
      }
    } else if (!connected) {
      ESP_LOGW(TAG, "Wi-Fi unavailable; deferring sync work");
    } else {
      ESP_LOGW(TAG, "Clock unavailable; skipping HTTPS sync");
    }

    if (hold_err == ESP_OK) wifi_power_release(WIFI_POWER_SYNC);

    now_us = esp_timer_get_time();
    if (work & NETWORK_SYNC_STOCKS)
      s_next_stock_us = now_us + STOCK_INTERVAL_US;
    if (work & NETWORK_SYNC_WEATHER) {
      s_next_weather_us = weather_ok
                              ? next_hour_us(now_us)
                              : now_us + RETRY_INTERVAL_US;
    }
    if (time_due || !time_valid) {
      s_next_time_us = time_attempted && time_err == ESP_OK && time_valid
                           ? now_us + HOURLY_INTERVAL_US
                           : now_us + RETRY_INTERVAL_US;
    }
  }
}

esp_err_t network_sync_start(void) {
  if (s_task) return ESP_ERR_INVALID_STATE;
  BaseType_t result = xTaskCreatePinnedToCore(
      network_sync_task, "network_sync", 8192, NULL, 1, &s_task, 0);
  return result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
