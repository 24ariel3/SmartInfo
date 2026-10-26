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
// LCD power: VCC -> 3V3 (red), GND -> GND (black).
#define BSP_LCD_SPI_MOSI (GPIO_NUM_23) // Yellow wire; LCD_TOUCH_DIN bridged here (yellow).
#define BSP_LCD_SPI_MISO (GPIO_NUM_19) // Green wire; LCD_TOUCH_DO bridged here (green).
#define BSP_LCD_SPI_CLK (GPIO_NUM_18) // Blue wire; LCD_TOUCH_CLK bridged here (blue).
#define BSP_LCD_SPI_CS (GPIO_NUM_25) // Green wire
#define BSP_LCD_DC (GPIO_NUM_32) // Blue wire
#define BSP_LCD_RST (GPIO_NUM_26) // White wire
#define BSP_LCD_TOUCH_CS (GPIO_NUM_13) // White wire
#define BSP_LCD_BACKLIGHT (GPIO_NUM_4) // Green wire
#define BSP_LCD_TOUCH_INT (GPIO_NUM_27) // Yellow wire; XPT2046 T_IRQ/PENIRQ (active LOW).

#define BSP_LED_BLUE (GPIO_NUM_2)
#define BSP_BUTTON_BOOT_IO (GPIO_NUM_0)
#define BSP_BAT_SENSE (GPIO_NUM_34) // White wire from battery divider; ADC1 channel 6.
#define BSP_CHARGER_SENSE (GPIO_NUM_35) // Charger VIN divider; ADC1 channel 7.

// I2C Configuration
// BME680 power: VCC -> 3V3 (red), GND -> GND (black).
#define BSP_I2C_SDA (GPIO_NUM_21) // BME680 SDA: green wire
#define BSP_I2C_SCL (GPIO_NUM_22) // BME680 SCL: yellow wire
#define BSP_I2C_NUM (I2C_NUM_0)
#define BSP_I2C_FREQ_HZ (100000)

/**
 * @brief Initialize all board support peripherals (GPIOs, SPI Bus, I2C Bus)
 *        Does NOT initialize Display/LVGL.
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t bsp_init(void);
