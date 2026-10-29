idf_component_register(SRCS "src/display.c"
                        INCLUDE_DIRS "include"
                        REQUIRES bsp esp_lcd lvgl driver esp_timer esp_lcd_ili9341 esp_lcd_touch_xpt2046
                        PRIV_REQUIRES esp_pm)
