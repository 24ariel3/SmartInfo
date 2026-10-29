/**
 * @file ui_updater.h
 * @brief UI updater module - handles periodic UI updates via LVGL timer
 *
 * This module creates an LVGL timer that polls data from various sources
 * (BSEC sensor, stock monitor, weather) and updates the UI. Since the timer
 * runs inside the LVGL task context, no mutex locking is required.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the UI updater
 *
 * Services battery/network UI every second and clock/sensor labels each minute.
 * Must be called after ui_init() and while holding the display lock.
 */
void ui_updater_init(void);

#ifdef __cplusplus
}
#endif
