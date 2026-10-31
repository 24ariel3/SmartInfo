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
static time_t s_next_slot_epoch;

static bool is_stock_slot(time_t slot_epoch) {
  // Include the final close quote while the UI already shows the market closed.
  return slot_epoch % NETWORK_SLOT_SECONDS == 0 &&
         (stock_monitor_is_market_open(slot_epoch) ||
          stock_monitor_is_market_open(slot_epoch - 1));
}

static time_t next_slot_epoch(time_t epoch) {
  time_t next_hour = (epoch / 3600 + 1) * 3600;
  for (time_t slot = (epoch / NETWORK_SLOT_SECONDS + 1) * NETWORK_SLOT_SECONDS;
       slot < next_hour; slot += NETWORK_SLOT_SECONDS) {
    if (is_stock_slot(slot)) return slot;
  }
  return next_hour;
}

static void schedule_next_slot(int64_t now_us) {
  s_next_slot_epoch = 0;
  s_next_slot_us = now_us + NETWORK_SLOT_US;
  if (time_manager_has_valid_time()) {
    time_t epoch = time(NULL);
    s_next_slot_epoch = next_slot_epoch(epoch);
    s_next_slot_us = now_us + (int64_t)(s_next_slot_epoch - epoch) * 1000000LL;
  }
  ESP_LOGI(TAG, "Next scheduled Wi-Fi window in %" PRId64 " seconds",
           (s_next_slot_us - now_us) / 1000000);
}

static TickType_t wait_until_next_slot(int64_t now_us) {
  if (s_next_slot_us <= now_us) return 1;
  uint64_t wait_ms = (uint64_t)(s_next_slot_us - now_us) / 1000;
  TickType_t ticks = pdMS_TO_TICKS(wait_ms);
  return ticks == 0 ? 1 : ticks;
}

static void merge_hourly_work(time_t epoch, uint32_t *work, bool *time_due) {
  int64_t hour = (int64_t)epoch / 3600;
  if (hour != s_last_hour_queued) {
    *work |= NETWORK_SYNC_WEATHER;
    *time_due = true;
    s_last_hour_queued = hour;
  }
}

static bool stock_due_in_window(time_t epoch, bool periodic_due) {
  if (!periodic_due) return false;
  // Use the target slot for normal delays, and the latest slot after clock jumps.
  time_t slot = s_next_slot_epoch;
  if (slot == 0 || epoch < slot || epoch - slot >= NETWORK_SLOT_SECONDS)
    slot = epoch / NETWORK_SLOT_SECONDS * NETWORK_SLOT_SECONDS;
  return is_stock_slot(slot);
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
    bool periodic_due = s_next_slot_us == 0 || now_us >= s_next_slot_us;
    uint32_t work = take_requests();
    bool manual_stock = (work & NETWORK_SYNC_STOCKS) != 0;

    if (!startup_due && !periodic_due && work == 0) {
      uint32_t ignored;
      xTaskNotifyWait(0, UINT32_MAX, &ignored, wait_until_next_slot(now_us));
      continue;
    }

    bool time_due = startup_due || s_retry_time || !time_manager_has_valid_time();
    s_retry_time = false;
    if (startup_due) {
      s_startup_sync_pending = false;
      work |= NETWORK_SYNC_STOCKS | NETWORK_SYNC_WEATHER;
    }

    int64_t window_start = now_us;
    esp_err_t hold_err = wifi_power_acquire(WIFI_POWER_SYNC);
    bool connected = hold_err == ESP_OK && wifi_wait_connected(30000);
    bool time_attempted = false;
    esp_err_t time_err = hold_err;
    // A boundary reached while connecting or synchronizing joins this session.
    for (int pass = 0; pass < 2; ++pass) {
      now_us = esp_timer_get_time();
      if (s_next_slot_us == 0 || now_us >= s_next_slot_us) periodic_due = true;
      if (time_manager_has_valid_time())
        merge_hourly_work(time(NULL), &work, &time_due);
      if (connected && time_due && !time_attempted) {
        time_attempted = true;
        time_err = time_manager_sync(10000);
      }
    }

    bool time_valid = time_manager_has_valid_time();
    bool stock_allowed = startup_due || manual_stock ||
                         (time_valid && stock_due_in_window(time(NULL), periodic_due));
    if (stock_allowed) work |= NETWORK_SYNC_STOCKS;
    uint32_t retry_mask = NETWORK_SYNC_WEATHER |
                          (stock_allowed ? NETWORK_SYNC_STOCKS : 0);
    work |= s_retry_work & retry_mask;
    s_retry_work &= ~work;

    // Rebase after NTP before HTTP so a manual fetch cannot skip a later boundary.
    if (time_attempted && time_valid && !periodic_due && !startup_due)
      schedule_next_slot(esp_timer_get_time());

    ESP_LOGI(TAG, "%s sync window: work=0x%" PRIx32 ", time=%s",
             startup_due ? "Startup" : periodic_due ? "Periodic" : "Requested",
             work, time_due ? "yes" : "no");
    if (!connected || !time_valid) {
      ESP_LOGW(TAG, "Wi-Fi or clock unavailable; deferring HTTPS work");
      s_retry_work |= work;
    } else {
      if ((work & NETWORK_SYNC_STOCKS) && stock_monitor_refresh() != ESP_OK) {
        ESP_LOGW(TAG, "Stock refresh failed; retrying in a market window");
        s_retry_work |= NETWORK_SYNC_STOCKS;
      }
      if ((work & NETWORK_SYNC_WEATHER) && weather_refresh() != ESP_OK) {
        ESP_LOGW(TAG, "Weather refresh failed; retrying in a scheduled window");
        s_retry_work |= NETWORK_SYNC_WEATHER;
      }
    }

    if (time_due && (!connected || !time_attempted || time_err != ESP_OK || !time_valid))
      s_retry_time = true;
    if (hold_err == ESP_OK) wifi_power_release(WIFI_POWER_SYNC);

    now_us = esp_timer_get_time();
    if (periodic_due || startup_due) schedule_next_slot(now_us);
    ESP_LOGD(TAG, "Wi-Fi window took %" PRId64 " ms", (now_us - window_start) / 1000);
  }
}

esp_err_t network_sync_start(void) {
  if (s_task) return ESP_ERR_INVALID_STATE;

  // Monday 2026-01-12: opening at 14:30 UTC, final quote at 21:00 UTC.
  configASSERT(is_stock_slot(1768228200));
  configASSERT(is_stock_slot(1768251600));
  configASSERT(!is_stock_slot(1768251900));
  configASSERT(next_slot_epoch(1768226460) == 1768228200);

  BaseType_t result = xTaskCreatePinnedToCore(
      network_sync_task, "network_sync", 8192, NULL, 1, &s_task, 0);
  return result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
