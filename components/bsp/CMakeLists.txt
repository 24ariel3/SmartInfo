idf_component_register(SRCS "src/bsp.c"
                        INCLUDE_DIRS "include"
                        REQUIRES esp_lcd lvgl driver esp_timer)
