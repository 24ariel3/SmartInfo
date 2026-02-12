/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#define LV_SQUARELINE_MOD__SWIPE 1  //if defined or 1, reverts back to LVGL8.3 swipe-gesture behaviour (LVGL-9.1 abandons swipe too early if it finds/leaves a new object in the swipe-path)

#include "esp_log.h"
#include "bsp/esp_wrover_kit.h"
#include "lvgl.h"
#include "ui/ui.h"


#define TAG "ESP-EXAMPLE"
#define APP_DISP_DEFAULT_BRIGHTNESS 50

/*******************************************************************************
* Private functions
*******************************************************************************/

// *INDENT-OFF*
void app_lvgl_display(void)
{
    bsp_display_lock(0);

    ui_init();

    bsp_display_unlock();
}

void app_main(void)
{
    /* Initialize display and LVGL */
    bsp_display_start();

    /* Set default display brightness */
    bsp_display_brightness_set(APP_DISP_DEFAULT_BRIGHTNESS);

    /* Add and show objects on display */
    app_lvgl_display();

    ESP_LOGI(TAG, "Example initialization done.");
}
// *INDENT-ON*
