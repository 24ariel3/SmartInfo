#pragma once

#include "esp_http_server.h"

typedef struct {
  const char *phase;
  size_t received;
  size_t total;
  esp_err_t error;
} ota_update_status_t;

bool device_server_authorize(httpd_req_t *req);
esp_err_t ota_update_register(httpd_handle_t server);
void ota_update_get_status(ota_update_status_t *out);
