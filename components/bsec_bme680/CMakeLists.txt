idf_component_register(SRCS "src/bsec_integration.c" "src/bme68x.c" "src/bsec_config.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES driver esp_timer nvs_flash bsp)

# Link against the precompiled BSEC library
# User must place libalgobsec.a in components/bsec_bme680/lib/
target_link_libraries(${COMPONENT_LIB} PRIVATE "-L${CMAKE_CURRENT_SOURCE_DIR}/lib" -lalgobsec)
