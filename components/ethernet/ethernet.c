#include "ethernet.h"
#include "esp_eth.h"
#include "esp_eth_driver.h"
#include "esp_eth_netif_glue.h"
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_mac.h"
#include "driver/spi_master.h"

static const char *TAG = "ethernet";

static esp_netif_t *s_eth_netif;

static void on_eth_link_up(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    if (!s_eth_netif) return;
    /* Kick off IPv6 SLAAC: creating the link-local address starts DAD; once it
     * succeeds the netif gets IP_EVENT_GOT_IP6 and (if RAs are present)
     * additional global / ULA addresses follow. No-op when LWIP_IPV6 is off. */
    esp_err_t err = esp_netif_create_ip6_linklocal(s_eth_netif);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "create IPv6 LL failed: %s", esp_err_to_name(err));
    }
}

esp_err_t ethernet_init(const ethernet_config_t *cfg, esp_netif_t **out_netif)
{
    ESP_RETURN_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, TAG, "cfg is NULL");

    const spi_bus_config_t spi_bus = {
        .mosi_io_num   = cfg->mosi_gpio,
        .miso_io_num   = cfg->miso_gpio,
        .sclk_io_num   = cfg->sclk_gpio,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    esp_err_t bus_ret = spi_bus_initialize(cfg->spi_host, &spi_bus, SPI_DMA_CH_AUTO);
    if (bus_ret != ESP_OK && bus_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(bus_ret));
        return bus_ret;
    }

    spi_device_interface_config_t spi_devcfg = {
        .mode            = 0,
        .clock_speed_hz  = cfg->clock_speed_hz > 0 ? cfg->clock_speed_hz : 20 * 1000 * 1000,
        .spics_io_num    = cfg->cs_gpio,
        .queue_size      = 20,
        .input_delay_ns  = 20,
    };

    eth_w5500_config_t w5500_cfg = ETH_W5500_DEFAULT_CONFIG(cfg->spi_host, &spi_devcfg);
    w5500_cfg.int_gpio_num = cfg->int_gpio;

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_cfg, &mac_cfg);
    ESP_RETURN_ON_FALSE(mac, ESP_FAIL, TAG, "create W5500 MAC failed");

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.reset_gpio_num      = cfg->rst_gpio;
    phy_cfg.autonego_timeout_ms = 0;
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_cfg);
    if (!phy) {
        mac->del(mac);
        ESP_LOGE(TAG, "create W5500 PHY failed");
        return ESP_FAIL;
    }

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    esp_err_t ret = esp_eth_driver_install(&eth_cfg, &eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "eth driver install failed: %s", esp_err_to_name(ret));
        mac->del(mac);
        phy->del(phy);
        return ret;
    }

    uint8_t mac_addr[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac_addr, ESP_MAC_ETH), TAG, "read MAC failed");
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr),
                        TAG, "set MAC failed");

    esp_netif_inherent_config_t base_cfg = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base_cfg.route_prio = cfg->route_priority > 0 ? cfg->route_priority : 200;
    esp_netif_config_t netif_cfg = {
        .base  = &base_cfg,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    if (!netif) {
        esp_eth_driver_uninstall(eth_handle);
        mac->del(mac);
        phy->del(phy);
        ESP_LOGE(TAG, "create netif failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(
        esp_netif_attach(netif, esp_eth_new_netif_glue(eth_handle)),
        TAG, "netif attach failed");

    if (cfg->hostname) {
        ESP_RETURN_ON_ERROR(
            esp_netif_set_hostname(netif, cfg->hostname),
            TAG, "set hostname failed");
    }

    s_eth_netif = netif;
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_CONNECTED,
                                   &on_eth_link_up, NULL),
        TAG, "register link-up handler failed");

    ESP_RETURN_ON_ERROR(esp_eth_start(eth_handle), TAG, "eth start failed");

    ESP_LOGI(TAG, "W5500 Ethernet started");
    if (out_netif) *out_netif = netif;
    return ESP_OK;
}
