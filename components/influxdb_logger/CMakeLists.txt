idf_component_register(
    SRCS "influxdb_logger.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_client esp-tls
    PRIV_REQUIRES freertos
)
