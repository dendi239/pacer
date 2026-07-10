idf_component_register(
    SRCS "ubx_gps.cpp"
    INCLUDE_DIRS "include"
    REQUIRES pacer_core esp_driver_uart
)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++2b)
