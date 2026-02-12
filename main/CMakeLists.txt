file(GLOB_RECURSE UI_SOURCES "ui/*.c")

idf_component_register(SRCS "main.c" "ui_updater.c" ${UI_SOURCES}
                    PRIV_REQUIRES spi_flash bsec_bme680 nvs_flash wifi_connect stock_monitor bsp time_manager display weather
                    INCLUDE_DIRS "ui" ".")
