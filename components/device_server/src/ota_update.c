#include "device_server.h"
#include "ota_update.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "wifi_connect.h"
#include <string.h>

static const char *TAG = "OTA";
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool busy;
static ota_update_status_t status = {.phase = "idle", .error = ESP_OK};
// Reserve upload memory before runtime heap fragmentation can prevent an update.
static StackType_t worker_stack[8192 / sizeof(StackType_t)];
static StaticTask_t worker_control;
static TaskHandle_t worker;
static httpd_req_t *volatile pending_request;
static uint8_t buffer[4096];

void ota_update_get_status(ota_update_status_t *out) {
  portENTER_CRITICAL(&state_lock);
  *out = status;
  portEXIT_CRITICAL(&state_lock);
}

static void set_result(const char *phase, esp_err_t error, bool active) {
  portENTER_CRITICAL(&state_lock);
  status.phase = phase;
  status.error = error;
  busy = active;
  portEXIT_CRITICAL(&state_lock);
}

static void process_upload(httpd_req_t *req) {
  httpd_handle_t server = req->handle;
  int socket = httpd_req_to_sockfd(req);
  int64_t deadline = esp_timer_get_time() + 180LL * 1000000;
  const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
  esp_ota_handle_t handle = 0;
  bool started = false;
  bool success = false;
  esp_err_t err = ESP_FAIL;
  const char *http_status = "500 Internal Server Error";
  const char *message = "Cannot configure upload timeout";
  // Keep the normal HTTP handshake timeout; poll this upload socket for its deadline.
  struct timeval receive_timeout = {.tv_sec = 1};
  if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout)) != 0) {
    err = ESP_FAIL;
    message = "Cannot configure upload timeout";
    goto done;
  }
  err = esp_ota_begin(partition, req->content_len, &handle);
  if (err != ESP_OK) {
    message = "Cannot begin OTA";
    goto done;
  }
  started = true;
  size_t received = 0;
  while (received < req->content_len) {
    if (esp_timer_get_time() >= deadline) {
      err = ESP_ERR_TIMEOUT;
      http_status = "408 Request Timeout";
      message = "Upload deadline exceeded";
      goto done;
    }
    size_t remaining = req->content_len - received;
    int len = httpd_req_recv(req, (char *)buffer, remaining < 4096 ? remaining : 4096);
    if (len == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (len <= 0) {
      err = ESP_FAIL;
      http_status = "400 Bad Request";
      message = "Incomplete upload";
      goto done;
    }
    err = esp_ota_write(handle, buffer, len);
    if (err != ESP_OK) {
      message = "Image write failed";
      http_status = err == ESP_ERR_OTA_VALIDATE_FAILED ? "400 Bad Request" : "500 Internal Server Error";
      goto done;
    }
    received += len;
    portENTER_CRITICAL(&state_lock);
    status.received = received;
    portEXIT_CRITICAL(&state_lock);
    vTaskDelay(1); // Let the dashboard and idle watchdog tasks run during uploads.
  }
  if (esp_timer_get_time() >= deadline) {
    err = ESP_ERR_TIMEOUT;
    http_status = "408 Request Timeout";
    message = "Upload deadline exceeded";
    goto done;
  }
  set_result("verifying", ESP_OK, true);
  err = esp_ota_end(handle);
  started = false; // esp_ota_end releases the handle on both success and failure.
  if (err != ESP_OK) {
    http_status = "400 Bad Request";
    message = "Invalid firmware image";
    goto done;
  }
  esp_app_desc_t uploaded;
  err = esp_ota_get_partition_description(partition, &uploaded);
  if (err != ESP_OK || memcmp(uploaded.project_name, esp_app_get_description()->project_name,
                              sizeof(uploaded.project_name)) != 0) {
    err = ESP_ERR_INVALID_ARG;
    http_status = "400 Bad Request";
    message = "Firmware is for a different project";
    goto done;
  }
  err = esp_ota_set_boot_partition(partition);
  if (err != ESP_OK) {
    message = "Cannot select updated firmware";
    goto done;
  }
  success = true;
  http_status = "200 OK";
  message = "Firmware accepted; restarting";
  set_result("rebooting", ESP_OK, true);

done:
  if (started) esp_ota_abort(handle);
  if (!success) {
    ESP_LOGW(TAG, "%s: %s", message, esp_err_to_name(err));
  }
  httpd_resp_set_hdr(req, "Connection", "close");
  if (success) httpd_resp_sendstr(req, message);
  else httpd_resp_send_custom_err(req, http_status, message);
  // Preserve the reply even when an invalid upload leaves unread request bytes.
  shutdown(socket, SHUT_WR);
  httpd_req_async_handler_complete(req);
  httpd_sess_trigger_close(server, socket);
  if (success) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
  }
  set_result("failed", err, false);
  wifi_power_release(WIFI_POWER_OTA);
}

static void upload_worker(void *arg) {
  while (1) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    httpd_req_t *req = pending_request;
    pending_request = NULL;
    configASSERT(req != NULL);
    process_upload(req);
  }
}

static esp_err_t upload_handler(httpd_req_t *req) {
  if (!device_server_authorize(req)) return ESP_FAIL;
  char content_type[40];
  if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
      strcmp(content_type, "application/octet-stream") != 0) {
    httpd_resp_send_custom_err(req, "415 Unsupported Media Type",
                              "Send a raw application .bin as application/octet-stream");
    return ESP_FAIL;
  }
  const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
  if (!partition || req->content_len == 0 || req->content_len > partition->size) {
    httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE,
                        "Firmware does not fit the inactive OTA slot");
    return ESP_FAIL;
  }
  esp_ota_img_states_t boot_state;
  esp_err_t err = esp_ota_get_state_partition(esp_ota_get_running_partition(), &boot_state);
  if ((err != ESP_OK && err != ESP_ERR_NOT_FOUND) ||
      (err == ESP_OK && boot_state == ESP_OTA_IMG_PENDING_VERIFY)) {
    httpd_resp_send_custom_err(req, "409 Conflict", "Running firmware has not been confirmed");
    return ESP_FAIL;
  }
  portENTER_CRITICAL(&state_lock);
  bool was_busy = busy;
  if (!was_busy) {
    busy = true;
    status = (ota_update_status_t){.phase = "uploading", .total = req->content_len, .error = ESP_OK};
  }
  portEXIT_CRITICAL(&state_lock);
  if (was_busy) {
    httpd_resp_send_custom_err(req, "409 Conflict", "Another update is in progress");
    return ESP_FAIL;
  }
  err = wifi_power_acquire(WIFI_POWER_OTA);
  if (err != ESP_OK) {
    set_result("failed", err, false);
    httpd_resp_send_custom_err(req, "503 Service Unavailable",
                               "Cannot keep Wi-Fi on for upload");
    return ESP_FAIL;
  }
  httpd_req_t *async_req = NULL;
  err = httpd_req_async_handler_begin(req, &async_req);
  if (err == ESP_OK) {
    pending_request = async_req;
    xTaskNotifyGive(worker);
    return ESP_OK;
  }
  set_result("failed", err, false);
  wifi_power_release(WIFI_POWER_OTA);
  ESP_LOGW(TAG, "Cannot allocate asynchronous upload request: %s", esp_err_to_name(err));
  httpd_resp_send_custom_err(req, "503 Service Unavailable", "Cannot allocate upload request; check /status heap values");
  return ESP_FAIL;
}

esp_err_t ota_update_register(httpd_handle_t server) {
  worker = xTaskCreateStatic(upload_worker, "ota_upload", sizeof(worker_stack), NULL,
                             4, worker_stack, &worker_control);
  if (!worker) return ESP_ERR_NO_MEM;
  const httpd_uri_t uri = {.uri = "/ota", .method = HTTP_POST, .handler = upload_handler};
  esp_err_t err = httpd_register_uri_handler(server, &uri);
  if (err != ESP_OK) {
    vTaskDelete(worker);
    worker = NULL;
  }
  return err;
}

esp_err_t device_server_confirm_boot(bool healthy) {
  esp_ota_img_states_t state;
  esp_err_t err = esp_ota_get_state_partition(esp_ota_get_running_partition(), &state);
  if (err == ESP_ERR_NOT_FOUND) return ESP_OK;
  if (err != ESP_OK) return err;
  if (state != ESP_OTA_IMG_PENDING_VERIFY) return ESP_OK;
  if (!healthy) {
    ESP_LOGE(TAG, "Startup health check failed; rolling back");
    return esp_ota_mark_app_invalid_rollback_and_reboot();
  }
  err = esp_ota_mark_app_valid_cancel_rollback();
  if (err == ESP_OK) ESP_LOGI(TAG, "Startup health check passed; firmware confirmed");
  return err;
}
