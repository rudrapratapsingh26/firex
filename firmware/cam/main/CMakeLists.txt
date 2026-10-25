idf_component_register(
    SRCS "cam_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_app_format
        esp_driver_gpio
        esp_driver_uart
        mbedtls
)
