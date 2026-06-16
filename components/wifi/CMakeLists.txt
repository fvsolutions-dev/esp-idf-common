idf_component_register(
    SRCS "wifi.c" "wifi_scan.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif nvs_flash odin
    PRIV_REQUIRES freertos
)
