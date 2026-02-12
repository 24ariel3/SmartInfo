#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include <sys/time.h>
#include <time.h>

static const char *TAG = "TIME_MANAGER";

static void on_got_ip(void *arg, esp_event_base_t event_base, int32_t event_id,
                      void *event_data) {
  // Prevent crash if SNTP is already initialized (reconnecting to WiFi)
  if (esp_sntp_enabled()) {
    ESP_LOGI(TAG, "Got IP: SNTP already running");
    return;
  }
  ESP_LOGI(TAG, "Got IP: Initializing SNTP");
  esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();
}

void time_manager_init(void) {
  // Register IP event handler to start SNTP when Wi-Fi connects
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             &on_got_ip, NULL));

  // Setting timezone to Israel (IST/IDT)
  // POSIX Format: IST-2IDT,M3.4.4/26,M10.5.0
  setenv("TZ", "IST-2IDT,M3.4.4/26,M10.5.0", 1);
  tzset();
}

void time_manager_get_time(char *buf, size_t buf_len, bool is_24h) {
  time_t now;
  struct tm timeinfo;
  time(&now);
  localtime_r(&now, &timeinfo);

  if (is_24h) {
    strftime(buf, buf_len, "%H:%M:%S", &timeinfo);
  } else {
    strftime(buf, buf_len, "%I:%M:%S %p", &timeinfo);
  }
}

void time_manager_get_date(char *buf, size_t buf_len) {
  time_t now;
  struct tm timeinfo;
  time(&now);
  localtime_r(&now, &timeinfo);

  strftime(buf, buf_len, "%d/%m/%Y", &timeinfo);
}
