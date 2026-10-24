#include "device_server.h"
#include "ota_update.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "mbedtls/constant_time.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "DEVICE_SERVER";
static httpd_handle_t server;

static void close_connection(httpd_handle_t handle, int socket) {
  // Flush the reply with FIN before close; unread rejected bodies otherwise cause RST.
  shutdown(socket, SHUT_WR);
  close(socket);
}

bool device_server_authorize(httpd_req_t *req) {
  char header[sizeof("Bearer ") - 1 + 64 + 1];
  if (strlen(CONFIG_DEVICE_SERVER_AUTH_TOKEN) == 64 &&
      httpd_req_get_hdr_value_len(req, "Authorization") == sizeof(header) - 1 &&
      httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) == ESP_OK &&
      memcmp(header, "Bearer ", 7) == 0 &&
      mbedtls_ct_memcmp(header + 7, CONFIG_DEVICE_SERVER_AUTH_TOKEN, 64) == 0) {
    return true;
  }
  httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer");
  httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
  return false;
}

static esp_err_t status_handler(httpd_req_t *req) {
  if (!device_server_authorize(req)) return ESP_FAIL;
  const esp_app_desc_t *app = esp_app_get_description();
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t boot_state = ESP_OTA_IMG_UNDEFINED;
  esp_err_t state_err = esp_ota_get_state_partition(running, &boot_state);
  if (state_err != ESP_OK && state_err != ESP_ERR_NOT_FOUND) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot read boot state");
  }
  char hash[65];
  for (size_t i = 0; i < sizeof(app->app_elf_sha256); ++i) {
    hash[2 * i] = "0123456789abcdef"[app->app_elf_sha256[i] >> 4];
    hash[2 * i + 1] = "0123456789abcdef"[app->app_elf_sha256[i] & 15];
  }
  hash[64] = '\0';
  ota_update_status_t ota;
  ota_update_get_status(&ota);
  cJSON *root = cJSON_CreateObject();
  if (!root) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
  bool ok = cJSON_AddStringToObject(root, "project", app->project_name) &&
            cJSON_AddStringToObject(root, "version", app->version) &&
            cJSON_AddStringToObject(root, "build_hash", hash) &&
            cJSON_AddStringToObject(root, "slot", running->label) &&
            cJSON_AddNumberToObject(root, "boot_state", boot_state) &&
            cJSON_AddBoolToObject(root, "pending_verify", boot_state == ESP_OTA_IMG_PENDING_VERIFY) &&
            cJSON_AddStringToObject(root, "ota_phase", ota.phase) &&
            cJSON_AddNumberToObject(root, "free_heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) &&
            cJSON_AddNumberToObject(root, "largest_free_block", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) &&
            cJSON_AddNumberToObject(root, "received", ota.received) &&
            cJSON_AddNumberToObject(root, "total", ota.total) &&
            cJSON_AddStringToObject(root, "ota_error", esp_err_to_name(ota.error));
  char *json = ok ? cJSON_PrintUnformatted(root) : NULL;
  cJSON_Delete(root);
  if (!json) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t err = httpd_resp_sendstr(req, json);
  free(json);
  return err;
}

esp_err_t device_server_init(void) {
  if (server) return ESP_ERR_INVALID_STATE;
  const char *token = CONFIG_DEVICE_SERVER_AUTH_TOKEN;
  if (strlen(token) != 64 || strspn(token, "0123456789abcdefABCDEF") != 64) {
    ESP_LOGW(TAG, "HTTP disabled: configure a 64-character hexadecimal bearer token");
    return ESP_ERR_INVALID_STATE;
  }
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_open_sockets = 3;
  config.close_fn = close_connection;
  esp_err_t err = httpd_start(&server, &config);
  if (err != ESP_OK) return err;
  const httpd_uri_t status_uri = {.uri = "/status", .method = HTTP_GET, .handler = status_handler};
  err = httpd_register_uri_handler(server, &status_uri);
  if (err == ESP_OK) err = ota_update_register(server);
  if (err != ESP_OK) {
    httpd_stop(server);
    server = NULL;
    return err;
  }
  ESP_LOGI(TAG, "Authenticated HTTP ready on port 80 (trusted LAN only)");
  return ESP_OK;
}
