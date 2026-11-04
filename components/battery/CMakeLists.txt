idf_component_register(SRCS "src/battery.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES bsp esp_adc esp_driver_gpio esp_timer nvs_flash)
