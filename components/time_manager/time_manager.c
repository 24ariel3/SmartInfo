#include "time_manager.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <sys/time.h>
#include <time.h>

static const char *TAG = "TIME_MANAGER";
static SemaphoreHandle_t s_sync_semaphore;

static void on_time_sync(struct timeval *tv) {
  (void)tv;
  if (s_sync_semaphore) xSemaphoreGive(s_sync_semaphore);
}

void time_manager_init(void) {
  if (!s_sync_semaphore) s_sync_semaphore = xSemaphoreCreateBinary();

  // Israel time with daylight-saving transitions.
  setenv("TZ", "IST-2IDT,M3.4.4/26,M10.5.0", 1);
  tzset();
}

bool time_manager_has_valid_time(void) {
  time_t now;
  time(&now);
  return now >= 1704067200; // 2024-01-01 UTC; older usually means unsynced.
}

bool time_manager_get_new_york_time(time_t utc_epoch, struct tm *out) {
  struct tm utc;
  if (!out || !gmtime_r(&utc_epoch, &utc)) return false;
  int year = utc.tm_year + 1900;
  if (year < 2007) return false;

  bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  time_t january = utc_epoch - (time_t)utc.tm_yday * 86400 -
                   utc.tm_hour * 3600 - utc.tm_min * 60 - utc.tm_sec;
  int march_day = 59 + leap;
  int november_day = 304 + leap;
  int january_weekday = (utc.tm_wday - utc.tm_yday % 7 + 7) % 7;
  int march_weekday = (january_weekday + march_day) % 7;
  int november_weekday = (january_weekday + november_day) % 7;
  // shortcut: US rules since 2007; update if US DST legislation changes.
  time_t start = january + (time_t)(march_day + (7 - march_weekday) % 7 + 7) * 86400 + 7 * 3600;
  time_t end = january + (time_t)(november_day + (7 - november_weekday) % 7) * 86400 + 6 * 3600;
  bool dst = utc_epoch >= start && utc_epoch < end;
  time_t local_epoch = utc_epoch - (dst ? 4 : 5) * 3600;
  if (!gmtime_r(&local_epoch, out)) return false;
  out->tm_isdst = dst;
  return true;
}

esp_err_t time_manager_sync(uint32_t timeout_ms) {
  if (!s_sync_semaphore) return ESP_ERR_INVALID_STATE;
  if (esp_sntp_enabled()) esp_sntp_stop();
  xSemaphoreTake(s_sync_semaphore, 0);

  esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_set_time_sync_notification_cb(on_time_sync);
  esp_sntp_init();

  if (xSemaphoreTake(s_sync_semaphore, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
    esp_sntp_stop();
    ESP_LOGW(TAG, "NTP synchronization timed out");
    return ESP_ERR_TIMEOUT;
  }

  esp_sntp_stop();
  ESP_LOGI(TAG, "Time synchronized");
  return ESP_OK;
}

void time_manager_get_time(char *buf, size_t buf_len, bool is_24h) {
  time_t now;
  struct tm timeinfo;
  time(&now);
  localtime_r(&now, &timeinfo);

  if (is_24h) {
    strftime(buf, buf_len, "%H:%M", &timeinfo);
  } else {
    strftime(buf, buf_len, "%I:%M %p", &timeinfo);
  }
}

void time_manager_get_date(char *buf, size_t buf_len) {
  time_t now;
  struct tm timeinfo;
  time(&now);
  localtime_r(&now, &timeinfo);
  strftime(buf, buf_len, "%d/%m/%Y", &timeinfo);
}
