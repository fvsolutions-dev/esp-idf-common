idf_component_register(
    SRCS "eros_uart.c"
    INCLUDE_DIRS "."
    REQUIRES eros esp_driver_uart esp_driver_gpio esp_pm esp_hw_support
)
