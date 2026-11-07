#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * Initialize battery/charger sensing and shield KEY keep-alive on BSP pins.
 * GPIO33 defaults LOW, then pulses for 200 ms every 20 seconds.
 * See src/battery.c for the divider/capacitor diagram and shield/GND tap notes.
 */
esp_err_t battery_init(void);

/**
 * Return latest calibrated, burst-averaged cell mV and filtered usable-energy %.
 * Percentage uses a five-sample median and EMA before the reference LUT.
 * Read sequentially: once at initialization, then normally every 10 seconds.
 * Filter state is RAM-only; both caller outputs remain unchanged on failure.
 */
esp_err_t battery_read(int *voltage_mv, int *percent);

/**
 * Update charger detection/runtime state, gate KEY pulses on charger presence,
 * and return the latest counters. Call once per second from one task.
 * Returns ESP_ERR_NOT_FINISHED until charger presence is debounced; outputs
 * still contain the current in-memory counters while detection is pending.
 */
esp_err_t battery_runtime_update(uint64_t *current_seconds,
                                 uint64_t *previous_seconds,
                                 bool *charger_connected);
