idf_component_register(
    SRCS "dashboard_ui.cpp"
    INCLUDE_DIRS "include"
    REQUIRES pacer_core esp_lcd esp_driver_gpio esp_driver_i2c esp_lcd_nv3041a
)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++2b)
