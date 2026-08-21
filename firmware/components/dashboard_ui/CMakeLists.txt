idf_component_register(
    SRCS "dashboard_ui.cpp"
         "fonts/pacer_font_mono_14.c"
         "fonts/pacer_font_mono_20.c"
         "fonts/pacer_font_mono_24.c"
         "fonts/pacer_font_mono_32.c"
         "fonts/pacer_font_mono_48.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "fonts"
    REQUIRES pacer_core esp_lcd esp_driver_gpio esp_driver_i2c esp_driver_ledc esp_lcd_nv3041a
)

# Guarded by language: the generated font tables next to dashboard_ui.cpp are C.
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++2b>)
