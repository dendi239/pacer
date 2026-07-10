idf_component_register(
    SRCS "storage.cpp"
    INCLUDE_DIRS "include"
    REQUIRES pacer_core fatfs sdmmc esp_driver_spi esp_driver_sdspi
)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++2b)
