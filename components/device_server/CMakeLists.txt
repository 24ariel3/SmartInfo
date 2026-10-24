idf_component_register(SRCS "src/device_server.c" "src/ota_update.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_http_server app_update esp_app_format esp_timer json mbedtls lwip heap)
