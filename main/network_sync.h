#pragma once

#include "esp_err.h"
#include <stdint.h>

#define NETWORK_SYNC_STOCKS (1U << 0)
#define NETWORK_SYNC_WEATHER (1U << 1)

esp_err_t network_sync_start(void);
void network_sync_request(uint32_t work);
