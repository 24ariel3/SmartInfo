idf_component_register(SRCS "time_manager.c"
                        INCLUDE_DIRS "include"
                        REQUIRES esp_netif lwip esp_wifi esp_event)
