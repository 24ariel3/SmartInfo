#pragma once

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "lvgl.h"

// LCD Configuration
#define BSP_LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)
#define BSP_LCD_H_RES (240)
#define BSP_LCD_V_RES (320)
#define BSP_LCD_CMD_BITS (8)
#define BSP_LCD_PARAM_BITS (8)
#define BSP_LCD_SPI_HOST (SPI2_HOST)

// Pin Definitions
#define BSP_LCD_SPI_MOSI (GPIO_NUM_23)
#define BSP_LCD_SPI_MISO (GPIO_NUM_19)
#define BSP_LCD_SPI_CLK (GPIO_NUM_18)
#define BSP_LCD_SPI_CS (GPIO_NUM_25)
#define BSP_LCD_DC (GPIO_NUM_32)
#define BSP_LCD_RST (GPIO_NUM_26)
#define BSP_LCD_TOUCH_CS (GPIO_NUM_13)
#define BSP_LCD_BACKLIGHT (GPIO_NUM_4)
#define BSP_LCD_TOUCH_INT (GPIO_NUM_27) // XPT2046 T_IRQ/PENIRQ pin (active LOW)

#define BSP_LED_BLUE (GPIO_NUM_2)
#define BSP_BUTTON_BOOT_IO (GPIO_NUM_0)

// I2C Configuration
#define BSP_I2C_SDA (GPIO_NUM_21)
#define BSP_I2C_SCL (GPIO_NUM_22)
#define BSP_I2C_NUM (I2C_NUM_0)
#define BSP_I2C_FREQ_HZ (100000)

/**
 * @brief Initialize all board support peripherals (GPIOs, SPI Bus, I2C Bus)
 *        Does NOT initialize Display/LVGL.
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t bsp_init(void);
