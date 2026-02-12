idf_component_register(SRCS "src/weather.c"
                        INCLUDE_DIRS "include"
                        REQUIRES "esp_http_client" "json" "log" "mbedtls" "wifi_connect")
