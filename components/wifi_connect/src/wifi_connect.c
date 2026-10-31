#include "wifi_connect.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "string.h"
#include <limits.h>

static const char *TAG = "WIFI";

static EventGroupHandle_t s_wifi_event_group;
static volatile bool s_is_connected = false;
static wifi_connection_state_t s_connection_state =
    WIFI_CONNECTION_IDLE; // New state variable
static SemaphoreHandle_t s_power_mutex;
static unsigned s_power_holds[WIFI_POWER_OWNER_COUNT];
static bool s_wifi_initialized;
static volatile bool s_radio_running;
static volatile bool s_radio_requested;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static int s_retry_num = 0;

static void reconnect_last_network(void) {
  char ssid[33];
  char password[65];
  nvs_handle_t handle;
  esp_err_t err = nvs_open("wifi_creds", NVS_READONLY, &handle);
  if (err != ESP_OK) return;
  size_t len = sizeof(ssid);
  err = nvs_get_str(handle, "last_ssid", ssid, &len);
  nvs_close(handle);
  if (err != ESP_OK || ssid[0] == '\0') return;
  err = wifi_load_credentials(ssid, password, sizeof(password));
  if (err == ESP_OK) err = wifi_connect_to_ap(ssid, password);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Saved network unavailable: %s; manual selection remains available",
             esp_err_to_name(err));
  }
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    reconnect_last_network();
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    s_is_connected = false;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

    // Check disconnect reason
    wifi_event_sta_disconnected_t *d_event =
        (wifi_event_sta_disconnected_t *)event_data;
    if (!s_radio_requested) {
      ESP_LOGI(TAG, "Radio is stopping; ignoring reconnect");
      return;
    }
    if (d_event->reason == WIFI_REASON_ASSOC_LEAVE) {
      ESP_LOGI(TAG, "Disconnected by user (ASSOC_LEAVE), ignoring retry");
      return;
    }

    // logic to distinguish between manual disconnect and retry
    if (s_connection_state != WIFI_CONNECTION_IDLE) {
      if (s_retry_num < 10) { // Retry a few times then wait
        esp_wifi_connect();
        s_retry_num++;
        s_connection_state = WIFI_CONNECTION_CONNECTING;
        ESP_LOGI(TAG, "retry to connect to the AP");
      } else {
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        s_connection_state = WIFI_CONNECTION_FAILED;
      }
    }
    ESP_LOGI(TAG, "connect to the AP fail");
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    s_is_connected = true;
    s_connection_state = WIFI_CONNECTION_CONNECTED;

    // Enable WiFi power save mode to reduce power consumption
    esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    if (ps_err == ESP_OK) {
      ESP_LOGI(TAG, "WiFi power save enabled (MAX_MODEM)");
    } else {
      ESP_LOGW(TAG, "Failed to enable WiFi power save: %s",
               esp_err_to_name(ps_err));
    }

    // Save credentials on successful connection
    wifi_config_t conf;
    if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK) {
      char ssid[33] = {0};
      char password[65] = {0};
      memcpy(ssid, conf.sta.ssid, sizeof(conf.sta.ssid));
      memcpy(password, conf.sta.password, sizeof(conf.sta.password));
      esp_err_t err = wifi_save_credentials(ssid, password);
      nvs_handle_t handle;
      if (err == ESP_OK) {
        err = nvs_open("wifi_creds", NVS_READWRITE, &handle);
        if (err == ESP_OK) {
          err = nvs_set_str(handle, "last_ssid", ssid);
          if (err == ESP_OK) err = nvs_commit(handle);
          nvs_close(handle);
        }
      }
      if (err != ESP_OK) ESP_LOGW(TAG, "Cannot remember successful network: %s", esp_err_to_name(err));
    }
  }
}

void wifi_connect_init(void) {
  s_wifi_event_group = xEventGroupCreate();
  if (s_wifi_event_group == NULL) ESP_ERROR_CHECK(ESP_ERR_NO_MEM);

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  s_power_mutex = xSemaphoreCreateMutex();
  if (s_power_mutex == NULL) ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
  s_wifi_initialized = true;

  ESP_LOGI(TAG, "Wi-Fi initialized; radio will start on demand.");
}

esp_err_t wifi_power_acquire(wifi_power_owner_t owner) {
  if ((unsigned)owner >= WIFI_POWER_OWNER_COUNT) return ESP_ERR_INVALID_ARG;
  if (!s_wifi_initialized) return ESP_ERR_INVALID_STATE;

  xSemaphoreTake(s_power_mutex, portMAX_DELAY);
  s_power_holds[owner]++;
  if (!s_radio_running) {
    xEventGroupClearBits(s_wifi_event_group,
                         WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_radio_requested = true;
    s_radio_running = true;
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
      s_radio_requested = false;
      s_radio_running = false;
      s_power_holds[owner]--;
      xSemaphoreGive(s_power_mutex);
      return err;
    }
  }
  xSemaphoreGive(s_power_mutex);
  return ESP_OK;
}

void wifi_power_release(wifi_power_owner_t owner) {
  if ((unsigned)owner >= WIFI_POWER_OWNER_COUNT || !s_power_mutex) return;

  xSemaphoreTake(s_power_mutex, portMAX_DELAY);
  if (s_power_holds[owner] > 0) s_power_holds[owner]--;
  bool any_holds = false;
  for (size_t i = 0; i < WIFI_POWER_OWNER_COUNT; ++i)
    any_holds |= s_power_holds[i] != 0;

  if (!any_holds && s_radio_running) {
    s_radio_requested = false;
    s_connection_state = WIFI_CONNECTION_IDLE;
    s_is_connected = false;
    xEventGroupClearBits(s_wifi_event_group,
                         WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Wi-Fi stop failed: %s", esp_err_to_name(err));
      s_radio_running = true;
      s_radio_requested = true;
    } else {
      s_radio_running = false;
    }
  }
  xSemaphoreGive(s_power_mutex);
}

bool wifi_wait_connected(uint32_t timeout_ms) {
  if (wifi_is_connected()) return true;
  EventBits_t bits = xEventGroupWaitBits(
      s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
      pdMS_TO_TICKS(timeout_ms));
  return (bits & WIFI_CONNECTED_BIT) != 0 || wifi_is_connected();
}

bool wifi_is_connected(void) { return s_is_connected; }

wifi_connection_state_t wifi_get_connection_state(void) {
  return s_connection_state;
}

esp_err_t wifi_connect_to_ap(const char *ssid, const char *password) {
  if (ssid == NULL || password == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!s_radio_running) return ESP_ERR_INVALID_STATE;

  s_connection_state = WIFI_CONNECTION_CONNECTING;
  s_retry_num = 0; // Reset retries
  s_is_connected = false;
  xEventGroupClearBits(s_wifi_event_group,
                       WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

  wifi_config_t wifi_config = {
      .sta =
          {
              .threshold.authmode = WIFI_AUTH_WPA2_PSK,
              .pmf_cfg = {.capable = true, .required = false},
          },
  };

  strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
  strlcpy((char *)wifi_config.sta.password, password,
          sizeof(wifi_config.sta.password));

  ESP_LOGI(TAG, "Connecting to %s...", ssid);

  // Disconnect first if connected
  esp_wifi_disconnect();

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  return esp_wifi_connect();
}

// --- Persistence ---
// NVS keys are limited to 15 characters, but SSIDs can be up to 32 characters.
// Use FNV-1a hash to generate a short, unique key from any SSID.
static void ssid_to_nvs_key(const char *ssid, char *key_out, size_t key_size) {
  // FNV-1a 32-bit hash
  uint32_t hash = 2166136261u;
  const char *p = ssid;
  while (*p) {
    hash ^= (uint8_t)(*p++);
    hash *= 16777619u;
  }
  // Format: "wf_" + 8 hex chars = 11 chars (fits 15 char NVS limit)
  snprintf(key_out, key_size, "wf_%08lx", (unsigned long)hash);
}

esp_err_t wifi_save_credentials(const char *ssid, const char *password) {
  if (!ssid || !password)
    return ESP_ERR_INVALID_ARG;

  char nvs_key[16];
  ssid_to_nvs_key(ssid, nvs_key, sizeof(nvs_key));

  nvs_handle_t my_handle;
  esp_err_t err = nvs_open("wifi_creds", NVS_READWRITE, &my_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
    return err;
  }

  err = nvs_set_str(my_handle, nvs_key, password);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to save password for %s (key: %s)", ssid, nvs_key);
  } else {
    ESP_LOGI(TAG, "Saved credentials for %s (key: %s)", ssid, nvs_key);
    err = nvs_commit(my_handle);
  }

  nvs_close(my_handle);
  return err;
}

esp_err_t wifi_load_credentials(const char *ssid, char *out_password,
                                size_t len) {
  if (!ssid || !out_password || len == 0)
    return ESP_ERR_INVALID_ARG;

  char nvs_key[16];
  ssid_to_nvs_key(ssid, nvs_key, sizeof(nvs_key));

  nvs_handle_t my_handle;
  esp_err_t err = nvs_open("wifi_creds", NVS_READONLY, &my_handle);
  if (err != ESP_OK)
    return err;

  size_t required_size = 0;
  err = nvs_get_str(my_handle, nvs_key, NULL, &required_size);
  if (err == ESP_OK && required_size <= len) {
    err = nvs_get_str(my_handle, nvs_key, out_password, &required_size);
  } else if (err == ESP_OK) {
    err = ESP_ERR_INVALID_SIZE; // Buffer too small
  }

  nvs_close(my_handle);
  return err;
}

bool wifi_has_saved_credentials(const char *ssid) {
  if (!ssid)
    return false;

  char nvs_key[16];
  ssid_to_nvs_key(ssid, nvs_key, sizeof(nvs_key));

  nvs_handle_t my_handle;
  if (nvs_open("wifi_creds", NVS_READONLY, &my_handle) != ESP_OK)
    return false;

  size_t required_size = 0;
  esp_err_t err = nvs_get_str(my_handle, nvs_key, NULL, &required_size);
  nvs_close(my_handle);

  return (err == ESP_OK);
}

// --- Centralized HTTP Logic ---
#include <inttypes.h>
#include "esp_timer.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
static SemaphoreHandle_t s_http_mutex = NULL;

static void ensure_http_mutex(void) {
  if (s_http_mutex == NULL) {
    s_http_mutex = xSemaphoreCreateMutex();
  }
}

typedef struct {
  char *data;
  size_t length;
  size_t capacity;
  esp_err_t error;
  unsigned connections;
} http_response_t;

static esp_err_t http_response_event(esp_http_client_event_t *event) {
  http_response_t *response = event->user_data;
  if (event->event_id == HTTP_EVENT_ON_CONNECTED) response->connections++;
  if (event->event_id != HTTP_EVENT_ON_DATA || response->error != ESP_OK ||
      esp_http_client_get_status_code(event->client) != 200)
    return response->error;

  if (event->data_len < 0 || (size_t)event->data_len > INT_MAX - 1 - response->length) {
    response->error = ESP_ERR_INVALID_SIZE;
    return response->error;
  }
  size_t needed = response->length + (size_t)event->data_len + 1;
  if (needed > response->capacity) {
    size_t capacity = response->capacity;
    if (capacity == 0) {
      int64_t expected = esp_http_client_get_content_length(event->client);
      if (expected > INT_MAX - 1) {
        response->error = ESP_ERR_INVALID_SIZE;
        return response->error;
      }
      capacity = expected > 0 ? (size_t)expected + 1 : 1024;
    }
    while (capacity < needed) {
      if (capacity > (INT_MAX - 1) / 2) { capacity = needed; break; }
      capacity *= 2;
    }
    char *data = realloc(response->data, capacity);
    if (!data) {
      response->error = ESP_ERR_NO_MEM;
      return response->error;
    }
    response->data = data;
    response->capacity = capacity;
  }
  if (event->data_len) memcpy(response->data + response->length, event->data, event->data_len);
  response->length += event->data_len;
  response->data[response->length] = 0;
  return ESP_OK;
}

static bool http_transport_error(esp_err_t err) {
  return err == ESP_ERR_HTTP_CONNECT || err == ESP_ERR_HTTP_CONNECTION_CLOSED ||
         err == ESP_ERR_HTTP_WRITE_DATA || err == ESP_ERR_HTTP_FETCH_HEADER ||
         err == ESP_ERR_TIMEOUT;
}

esp_err_t wifi_perform_http_get_batch(const char *const *urls, char **out_buffers,
                                     esp_err_t *out_results, size_t count) {
  if (!urls || !out_buffers || !out_results || count == 0) return ESP_ERR_INVALID_ARG;
  for (size_t i = 0; i < count; ++i) {
    out_buffers[i] = NULL;
    out_results[i] = ESP_ERR_INVALID_ARG;
  }
  for (size_t i = 0; i < count; ++i)
    if (!urls[i] || !urls[i][0]) return ESP_ERR_INVALID_ARG;

  ensure_http_mutex();
  esp_err_t err = !s_http_mutex ? ESP_ERR_NO_MEM :
                 xSemaphoreTake(s_http_mutex, pdMS_TO_TICKS(10000)) == pdTRUE ?
                 ESP_OK : ESP_ERR_TIMEOUT;
  if (err != ESP_OK) {
    for (size_t i = 0; i < count; ++i) out_results[i] = err;
    return err;
  }

  int64_t started = esp_timer_get_time();
  http_response_t response = {0};
  esp_http_client_handle_t client = NULL;
  esp_err_t first_error = ESP_OK;
  for (size_t i = 0; i < count; ++i) {
    bool reused = client != NULL;
    for (int attempt = 0; attempt < 2; ++attempt) {
      response.error = ESP_OK;
      if (!client) {
        esp_http_client_config_t config = {
            .url = urls[i], .method = HTTP_METHOD_GET, .timeout_ms = 20000,
            .crt_bundle_attach = esp_crt_bundle_attach, .disable_auto_redirect = true,
            .event_handler = http_response_event, .user_data = &response,
        };
        client = esp_http_client_init(&config);
        err = client ? ESP_OK : ESP_ERR_NO_MEM;
      } else {
        err = esp_http_client_set_url(client, urls[i]);
      }
      if (err == ESP_OK) err = esp_http_client_perform(client);
      if (response.error != ESP_OK) err = response.error;
      int status = client ? esp_http_client_get_status_code(client) : 0;
      if (err == ESP_OK && !esp_http_client_is_complete_data_received(client))
        err = ESP_ERR_HTTP_FETCH_HEADER;
      bool retry = reused && attempt == 0 && http_transport_error(err);
      if (err == ESP_OK && status != 200) {
        ESP_LOGW(TAG, "HTTP GET failed status code: %d", status);
        err = ESP_FAIL;
      }
      if (err == ESP_OK && !response.data) {
        response.data = calloc(1, 1);
        if (!response.data) err = ESP_ERR_NO_MEM;
      }
      if (err == ESP_OK) {
        out_buffers[i] = response.data;
      } else {
        free(response.data);
        if (client) esp_http_client_cleanup(client);
        client = NULL;
      }
      response.data = NULL;
      response.length = response.capacity = 0;
      if (!retry) break;
    }
    out_results[i] = err;
    if (first_error == ESP_OK && err != ESP_OK) first_error = err;
  }
  if (client) esp_http_client_cleanup(client);
  ESP_LOGD(TAG, "HTTPS batch: requests=%u, connections=%u, duration=%" PRId64 " ms",
           (unsigned)count, response.connections, (esp_timer_get_time() - started) / 1000);
  xSemaphoreGive(s_http_mutex);
  return first_error;
}

esp_err_t wifi_perform_http_get(const char *url, char **out_buffer) {
  esp_err_t result;
  return wifi_perform_http_get_batch(&url, out_buffer, &result, 1);
}

char *wifi_scan_networks(void) {
  // Ensure WiFi is initialized in STA mode (wifi_connect_init does this)
  // Scan non-blocking? No, we need results.
  // Note: If connected, scan is performed in background but might cause
  // latency.

  wifi_scan_config_t scan_config = {
      .ssid = NULL,
      .bssid = NULL,
      .channel = 0,
      .show_hidden = false,
      .scan_type = WIFI_SCAN_TYPE_ACTIVE,
      .scan_time.active.min = 120, // 120ms per channel
      .scan_time.active.max = 200,
  };

  ESP_LOGI(TAG, "Starting WiFi scan...");
  esp_err_t err = esp_wifi_scan_start(&scan_config, true); // Blocking
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "WiFi scan start failed: %s", esp_err_to_name(err));
    return NULL;
  }

  uint16_t ap_num = 0;
  ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_num));
  ESP_LOGI(TAG, "Scan completed, found %d APs", ap_num);

  if (ap_num == 0) {
    return NULL;
  }

  wifi_ap_record_t *ap_records = malloc(ap_num * sizeof(wifi_ap_record_t));
  if (!ap_records) {
    ESP_LOGE(TAG, "Failed to allocate memory for APs");
    return NULL;
  }

  ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_num, ap_records));

  // Determine needed buffer size
  size_t total_len = 0;
  for (int i = 0; i < ap_num; i++) {
    // Skip empty SSIDs
    if (strlen((char *)ap_records[i].ssid) == 0)
      continue;

    // Check for duplicates in previous records?
    // We can just check if this SSID has appeared before in the *records* list
    // up to i
    bool duplicate = false;
    for (int j = 0; j < i; j++) {
      if (strcmp((char *)ap_records[i].ssid, (char *)ap_records[j].ssid) == 0) {
        duplicate = true;
        break;
      }
    }
    if (duplicate)
      continue;

    total_len += strlen((char *)ap_records[i].ssid) + 1; // +1 for \n or \0
  }
  if (total_len == 0)
    total_len = 1;

  char *result_buffer = malloc(total_len + 1);
  if (!result_buffer) {
    free(ap_records);
    return NULL;
  }
  result_buffer[0] = '\0';

  // Build string: "SSID1\nSSID2\n..."
  bool first = true;
  for (int i = 0; i < ap_num; i++) {
    // Skip empty SSIDs
    if (strlen((char *)ap_records[i].ssid) == 0)
      continue;

    // Check for duplicates
    bool duplicate = false;
    for (int j = 0; j < i; j++) {
      if (strcmp((char *)ap_records[i].ssid, (char *)ap_records[j].ssid) == 0) {
        duplicate = true;
        break;
      }
    }
    if (duplicate)
      continue;

    if (!first)
      strcat(result_buffer, "\n");
    strncat(result_buffer, (char *)ap_records[i].ssid, 32);
    first = false;
  }
  free(ap_records);
  return result_buffer;
}

static wifi_scan_state_t s_scan_state = WIFI_SCAN_IDLE;
static char *s_scan_result_buffer = NULL;
static SemaphoreHandle_t s_scan_mutex = NULL;

wifi_scan_state_t wifi_get_scan_state(void) { return s_scan_state; }

esp_err_t wifi_get_scan_results(char **out_networks) {
  if (s_scan_state != WIFI_SCAN_COMPLETED) {
    return ESP_ERR_INVALID_STATE;
  }

  if (s_scan_mutex == NULL) {
    return ESP_ERR_INVALID_STATE;
  }

  if (xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (s_scan_result_buffer) {
      // Caller takes ownership
      *out_networks = s_scan_result_buffer;
      s_scan_result_buffer = NULL;
    } else {
      *out_networks = NULL;
    }
    s_scan_state = WIFI_SCAN_IDLE; // Reset state
    xSemaphoreGive(s_scan_mutex);
    return ESP_OK;
  }
  return ESP_ERR_TIMEOUT;
}

static void scan_task_wrapper(void *param) {
  (void)param;

  ESP_LOGI(TAG, "Starting background scan...");

  // Perform blocking scan
  char *results = wifi_scan_networks();

  if (s_scan_mutex == NULL) {
    // Should have been created
    if (results)
      free(results);
    s_scan_state = WIFI_SCAN_FAILED;
    wifi_power_release(WIFI_POWER_SCAN);
    vTaskDelete(NULL);
    return;
  }

  if (xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    if (s_scan_result_buffer) {
      free(s_scan_result_buffer); // Should be NULL but safety first
    }
    s_scan_result_buffer = results;
    s_scan_state = WIFI_SCAN_COMPLETED;
    xSemaphoreGive(s_scan_mutex);
  } else {
    ESP_LOGE(TAG, "Failed to take mutex to store scan results");
    if (results)
      free(results);
    s_scan_state = WIFI_SCAN_FAILED;
  }

  wifi_power_release(WIFI_POWER_SCAN);
  vTaskDelete(NULL);
}

esp_err_t wifi_start_scan(void) {
  if (s_scan_state == WIFI_SCANNING) {
    return ESP_ERR_INVALID_STATE;
  }

  if (s_scan_mutex == NULL) {
    s_scan_mutex = xSemaphoreCreateMutex();
  }

  esp_err_t hold_err = wifi_power_acquire(WIFI_POWER_SCAN);
  if (hold_err != ESP_OK) return hold_err;

  s_scan_state = WIFI_SCANNING;

  BaseType_t ret =
      xTaskCreate(scan_task_wrapper, "wifi_scan", 4096, NULL, 5, NULL);
  if (ret != pdPASS) {
    s_scan_state = WIFI_SCAN_FAILED;
    wifi_power_release(WIFI_POWER_SCAN);
    return ESP_FAIL;
  }

  return ESP_OK;
}
