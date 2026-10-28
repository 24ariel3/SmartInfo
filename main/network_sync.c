#include "network_sync.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "stock_monitor.h"
#include "time_manager.h"
#include "weather.h"
#include "wifi_connect.h"
#include <inttypes.h>
#include <stdbool.h>
#include <time.h>

static const char *TAG = "NETWORK_SYNC";
#define NETWORK_SLOT_SECONDS 300LL
#define NETWORK_SLOT_US (NETWORK_SLOT_SECONDS * 1000000LL)

static portMUX_TYPE s_request_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_pending_work;
static uint32_t s_retry_work;
static TaskHandle_t s_task;
static bool s_startup_sync_pending = true;
static bool s_retry_time;
static int64_t s_next_slot_us;
static int64_t s_last_hour_queued = -1;

static int64_t next_slot_epoch(int64_t epoch_seconds) {
  return (epoch_seconds / NETWORK_SLOT_SECONDS + 1) * NETWORK_SLOT_SECONDS;
}

static int64_t next_periodic_slot_us(int64_t now_us) {
  if (time_manager_has_valid_time()) {
    time_t epoch;
    time(&epoch);
    int64_t seconds_until_slot = next_slot_epoch(epoch) - epoch;
    return now_us + seconds_until_slot * 1000000LL;
  }

  return now_us + NETWORK_SLOT_US;
}

static TickType_t wait_until_next_slot(int64_t now_us) {
  if (s_next_slot_us <= now_us) return 1;
  uint64_t wait_ms = (uint64_t)(s_next_slot_us - now_us) / 1000;
  TickType_t ticks = pdMS_TO_TICKS(wait_ms);
  return ticks == 0 ? 1 : ticks;
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

static void network_sync_task(void *arg) {
  (void)arg;
  while (1) {
    int64_t now_us = esp_timer_get_time();
    bool startup_due = s_startup_sync_pending;
    bool periodic_due = startup_due || s_next_slot_us == 0 ||
                        now_us >= s_next_slot_us;
    uint32_t work = take_requests();
    bool has_request = work != 0;

    if (!startup_due && !periodic_due && !has_request) {
      uint32_t ignored;
      xTaskNotifyWait(0, UINT32_MAX, &ignored, wait_until_next_slot(now_us));
      continue;
    }

    if (startup_due) {
      s_startup_sync_pending = false;
      work |= NETWORK_SYNC_STOCKS | NETWORK_SYNC_WEATHER;
    } else if (periodic_due) {
      work |= NETWORK_SYNC_STOCKS;
    }
    work |= s_retry_work;
    s_retry_work = 0;

    bool time_due = startup_due || s_retry_time;
    s_retry_time = false;

    bool time_valid_before_sync = time_manager_has_valid_time();
    if (periodic_due && time_valid_before_sync) {
      time_t epoch;
      time(&epoch);
      int64_t current_hour = (int64_t)epoch / 3600;
      if (startup_due) {
        s_last_hour_queued = current_hour;
      } else if (current_hour > s_last_hour_queued) {
        work |= NETWORK_SYNC_WEATHER;
        time_due = true;
        s_last_hour_queued = current_hour;
      }
    }
    if (!time_valid_before_sync && work != 0) time_due = true;

    ESP_LOGI(TAG, "%s sync window: work=0x%" PRIx32 ", time=%s",
             startup_due ? "Startup" : periodic_due ? "Periodic" : "Requested",
             work, time_due ? "yes" : "no");

    esp_err_t hold_err = wifi_power_acquire(WIFI_POWER_SYNC);
    bool connected = hold_err == ESP_OK && wifi_wait_connected(30000);
    bool time_attempted = false;
    esp_err_t time_err = hold_err;
    if (connected && (time_due || !time_manager_has_valid_time())) {
      time_attempted = true;
      time_err = time_manager_sync(10000);
    }

    bool time_valid = time_manager_has_valid_time();
    if (time_valid) {
      time_t epoch;
      time(&epoch);
      int64_t current_hour = (int64_t)epoch / 3600;
      if (s_last_hour_queued < 0) {
        s_last_hour_queued = current_hour;
      } else if (current_hour > s_last_hour_queued) {
        // SNTP can move the clock across an hour; fetch weather in this window.
        work |= NETWORK_SYNC_WEATHER;
        s_last_hour_queued = current_hour;
      }
    }

    if (!connected) {
      ESP_LOGW(TAG, "Wi-Fi unavailable; deferring work to the next window");
      s_retry_work |= work;
      if (time_due || !time_valid) s_retry_time = true;
    } else if (!time_valid) {
      ESP_LOGW(TAG, "Clock unavailable; deferring HTTPS work to the next window");
      s_retry_work |= work;
      s_retry_time = true;
    } else {
      if (work & NETWORK_SYNC_STOCKS) {
        if (stock_monitor_refresh() != ESP_OK) {
          ESP_LOGW(TAG, "Stock refresh failed; retrying in a scheduled window");
          s_retry_work |= NETWORK_SYNC_STOCKS;
        }
      }
      if (work & NETWORK_SYNC_WEATHER) {
        if (weather_refresh() != ESP_OK) {
          ESP_LOGW(TAG, "Weather refresh failed; retrying in a scheduled window");
          s_retry_work |= NETWORK_SYNC_WEATHER;
        }
      }
    }

    if (time_due && (!connected || !time_attempted || time_err != ESP_OK ||
                     !time_valid)) {
      s_retry_time = true;
      if (connected && time_attempted)
        ESP_LOGW(TAG, "Time sync failed; retrying in a scheduled window");
    }

    if (hold_err == ESP_OK) wifi_power_release(WIFI_POWER_SYNC);

    now_us = esp_timer_get_time();
    if (periodic_due || (!time_valid_before_sync && time_valid)) {
      s_next_slot_us = next_periodic_slot_us(now_us);
      ESP_LOGI(TAG, "Next scheduled Wi-Fi window in %" PRId64 " seconds",
               (s_next_slot_us - now_us) / 1000000);
    }
  }
}

esp_err_t network_sync_start(void) {
  if (s_task) return ESP_ERR_INVALID_STATE;

  configASSERT(next_slot_epoch(10 * 3600 + 3 * 60 + 20) == 10 * 3600 + 5 * 60);
  configASSERT(next_slot_epoch(10 * 3600 + 5 * 60) == 10 * 3600 + 10 * 60);
  configASSERT(next_slot_epoch(23 * 3600 + 59 * 60 + 59) == 24 * 3600);

  BaseType_t result = xTaskCreatePinnedToCore(
      network_sync_task, "network_sync", 8192, NULL, 1, &s_task, 0);
  return result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
