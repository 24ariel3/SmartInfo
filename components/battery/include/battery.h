#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * Initialize battery sensing on the BSP_BAT_SENSE pin defined in bsp.h.
 * See src/battery.c for the divider/capacitor diagram and shield/GND tap notes.
 */
esp_err_t battery_init(void);

/** Read cell voltage in mV and approximate charge using the battery voltage LUT. */
esp_err_t battery_read(int *voltage_mv, int *percent);

/**
 * Update charger detection/runtime state and return the latest counters.
 * Returns ESP_ERR_NOT_FINISHED until charger presence is debounced; outputs
 * still contain the current in-memory counters while detection is pending.
 */
esp_err_t battery_runtime_update(uint64_t *current_seconds,
                                 uint64_t *previous_seconds,
                                 bool *charger_connected);
