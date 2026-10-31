idf_component_register(SRCS "src/stock_monitor.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_client json mbedtls esp_timer wifi_connect time_manager)
