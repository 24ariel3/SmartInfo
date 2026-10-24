#pragma once

#include "esp_err.h"

/**
 * Initialize battery sensing on the BSP_BAT_SENSE pin defined in bsp.h.
 * See src/battery.c for the divider/capacitor diagram and shield/GND tap notes.
 */
esp_err_t battery_init(void);

/** Read cell voltage in mV and approximate charge (3300 mV = 0%, 4200 mV = 100%). */
esp_err_t battery_read(int *voltage_mv, int *percent);
