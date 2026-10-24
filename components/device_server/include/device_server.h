#pragma once

#include "esp_err.h"
#include <stdbool.h>

/** Start authenticated HTTP on port 80. Invalid/missing token returns INVALID_STATE. */
esp_err_t device_server_init(void);

/** Confirm a pending OTA boot, or roll back when healthy is false. No-op on other boots. */
esp_err_t device_server_confirm_boot(bool healthy);
