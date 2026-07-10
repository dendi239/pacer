idf_component_register(
    SRCS "app_main.cpp"
    REQUIRES pacer_core ubx_gps storage dashboard_ui esp_timer
)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++2b)
