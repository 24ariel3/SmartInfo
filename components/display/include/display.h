#pragma once

#include "esp_err.h"
#include "lvgl.h"
#include <stdbool.h>

/**
 * @brief Initialize the display and LVGL
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t display_init(void);

/**
 * @brief Get the LVGL display object
 *
 * @return lv_display_t* Pointer to display
 */
lv_display_t *display_get_handle(void);

/**
 * @brief Lock the display (LVGL) to perform thread-safe operations
 * @param timeout_ms Timeout in milliseconds (0 for wait forever)
 * @return true if lock acquired, false otherwise
 */
bool display_lock(uint32_t timeout_ms);

/**
 * @brief Set the display brightness (0-100%)
 * @param percent Brightness percentage (0-100)
 */
void display_set_brightness(uint8_t percent);

#define DISPLAY_BACKLIGHT_DEFAULT (100)

/**
 * @brief Unlock the display (LVGL)
 */
void display_unlock(void);
