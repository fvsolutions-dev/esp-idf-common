#pragma once
#include "esp_err.h"
#include "esp_netif.h"
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    spi_host_device_t spi_host;
    int mosi_gpio;
    int miso_gpio;
    int sclk_gpio;
    int cs_gpio;
    int rst_gpio;
    int int_gpio;
    int clock_speed_hz;
    const char *hostname;
    /* Default-route priority on the resulting netif. 0 = library default (200),
     * which beats the Wi-Fi STA default (100). Set higher to give Ethernet
     * precedence over Wi-Fi when both have an IP. */
    int route_priority;
} ethernet_config_t;

esp_err_t ethernet_init(const ethernet_config_t *cfg, esp_netif_t **out_netif);

#ifdef __cplusplus
}
#endif
