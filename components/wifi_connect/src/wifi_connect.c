#include "wifi_connect.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "string.h"

static const char *TAG = "WIFI";

static EventGroupHandle_t s_wifi_event_group;
static bool s_is_connected = false;
static wifi_connection_state_t s_connection_state =
    WIFI_CONNECTION_IDLE; // New state variable

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static int s_retry_num = 0;

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    // esp_wifi_connect(); // Removed auto-connect
    // s_connection_state = WIFI_CONNECTION_CONNECTING;
    ESP_LOGI(TAG, "WiFi Started. Waiting for user command...");
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    s_is_connected = false;

    // Check disconnect reason
    wifi_event_sta_disconnected_t *d_event =
        (wifi_event_sta_disconnected_t *)event_data;
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
      wifi_save_credentials((const char *)conf.sta.ssid,
                            (const char *)conf.sta.password);
    }
  }
}

void wifi_connect_init(void) {
  s_wifi_event_group = xEventGroupCreate();

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
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "wifi_init_sta finished.");
  // Removed blocking wait to allow non-blocking UI
}

void wifi_disconnect(void) {
  s_connection_state = WIFI_CONNECTION_IDLE;
  esp_wifi_disconnect();
  esp_wifi_stop();
}

bool wifi_is_connected(void) { return s_is_connected; }

wifi_connection_state_t wifi_get_connection_state(void) {
  return s_connection_state;
}

esp_err_t wifi_connect_to_ap(const char *ssid, const char *password) {
  if (ssid == NULL || password == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  s_connection_state = WIFI_CONNECTION_CONNECTING;
  s_retry_num = 0; // Reset retries

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
    nvs_commit(my_handle);
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
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_http_mutex = NULL;

static void ensure_http_mutex(void) {
  if (s_http_mutex == NULL) {
    s_http_mutex = xSemaphoreCreateMutex();
  }
}

// Helper to read HTTP response into a dynamic buffer
static char *http_download_to_buffer(esp_http_client_handle_t client,
                                     int *out_len) {
  int content_length = esp_http_client_get_content_length(client);
  if (content_length < 0) {
    ESP_LOGW(TAG, "Content-Length unknown, using default size");
    content_length = 4096; // Reasonable default for chunked
  }

  char *buffer = malloc(content_length + 1);
  if (buffer == NULL) {
    ESP_LOGE(TAG, "Failed to allocate %d bytes for response", content_length);
    return NULL;
  }

  int total_read = 0;
  while (1) {
    int read_len = esp_http_client_read(client, buffer + total_read,
                                        content_length - total_read);
    if (read_len <= 0) {
      break;
    }
    total_read += read_len;
    // Handle chunked encoding growth if content-length was unknown/wrong
    if (read_len > 0 && total_read >= content_length) {
      // if we filled the buffer, verify if we need more
      // For simple content-length, we shouldn't get here unless logic is off
      // For chunked, length might be -1 initally handled above?
      // Let's assume content_length was correct or we need realloc
      // But esp_http_client_read stops at drain?
      // Let's implement safe growth just in case
      content_length *= 2;
      char *new_buffer = realloc(buffer, content_length + 1);
      if (!new_buffer) {
        ESP_LOGE(TAG, "Failed to reallocate buffer");
        free(buffer);
        return NULL;
      }
      buffer = new_buffer;
    }
  }
  buffer[total_read] = 0; // Null terminate
  if (out_len)
    *out_len = total_read;
  return buffer;
}

esp_err_t wifi_perform_http_get(const char *url, char **out_buffer) {
  if (url == NULL || out_buffer == NULL)
    return ESP_ERR_INVALID_ARG;

  ensure_http_mutex();
  if (xSemaphoreTake(s_http_mutex, pdMS_TO_TICKS(10000)) !=
      pdTRUE) { // Increased timeout
    ESP_LOGE(TAG, "Failed to acquire HTTP mutex");
    return ESP_ERR_TIMEOUT;
  }

  esp_err_t ret = ESP_FAIL;
  *out_buffer = NULL;

  esp_http_client_config_t config = {
      .url = url,
      .method = HTTP_METHOD_GET,
      .timeout_ms = 20000,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .disable_auto_redirect = true,
  };

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    ESP_LOGE(TAG, "Failed to init http client");
    xSemaphoreGive(s_http_mutex);
    return ESP_FAIL;
  }

  esp_err_t err = esp_http_client_open(client, 0);
  if (err == ESP_OK) {
    int content_length = esp_http_client_fetch_headers(client);
    if (content_length >= 0 || content_length == ESP_ERR_HTTP_FETCH_HEADER) {
      int status_code = esp_http_client_get_status_code(client);
      if (status_code == 200) {
        int data_len = 0;
        char *resp = http_download_to_buffer(client, &data_len);
        if (resp) {
          *out_buffer = resp;
          ret = ESP_OK;
        }
      } else {
        ESP_LOGE(TAG, "HTTP GET failed status code: %d", status_code);
      }
    } else {
      ESP_LOGE(TAG, "Failed to fetch headers");
    }
  } else {
    ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
  }

  esp_http_client_cleanup(client);
  xSemaphoreGive(s_http_mutex);
  return ret;
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

  vTaskDelete(NULL);
}

esp_err_t wifi_start_scan(void) {
  if (s_scan_state == WIFI_SCANNING) {
    return ESP_ERR_INVALID_STATE;
  }

  if (s_scan_mutex == NULL) {
    s_scan_mutex = xSemaphoreCreateMutex();
  }

  s_scan_state = WIFI_SCANNING;

  BaseType_t ret =
      xTaskCreate(scan_task_wrapper, "wifi_scan", 4096, NULL, 5, NULL);
  if (ret != pdPASS) {
    s_scan_state = WIFI_SCAN_FAILED;
    return ESP_FAIL;
  }

  return ESP_OK;
}
