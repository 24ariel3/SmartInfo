#pragma once

#include "esp_err.h"

/** Initialize battery sensing on GPIO34 (ADC1 channel 6). */
esp_err_t battery_init(void);

/** Read cell voltage in mV and approximate charge (3300 mV = 0%, 4200 mV = 100%). */
esp_err_t battery_read(int *voltage_mv, int *percent);
