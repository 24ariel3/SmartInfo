file(GLOB_RECURSE UI_SOURCES "ui/*.c")

idf_component_register(SRCS "main.c" "ui_updater.c" "network_sync.c" ${UI_SOURCES}
                    PRIV_REQUIRES spi_flash bsec_bme680 nvs_flash wifi_connect stock_monitor bsp battery time_manager display weather device_server esp_pm
                    INCLUDE_DIRS "ui" ".")
