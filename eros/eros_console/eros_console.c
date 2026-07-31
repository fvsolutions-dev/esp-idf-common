#include "eros_console.h"

#include "eros.h"

static eros_endpoint_t *endpoint;
static eros_console_config_t config;

static void console_ep_cb(eros_endpoint_t *ep, eros_package_t *pkg)
{
    /* Source-only endpoint: nothing routes TO it — transports call
       eros_console_feed_bytes directly. */
    (void)ep;
    (void)pkg;
}

esp_err_t eros_console_init(const eros_console_config_t *cfg)
{
    if (!cfg || !cfg->router || !cfg->on_input) return ESP_ERR_INVALID_ARG;
    if (endpoint) return ESP_ERR_INVALID_STATE;

    config = *cfg;

    endpoint = eros_unbuffered_endpoint_new(cfg->endpoint_id, cfg->router, console_ep_cb);
    if (!endpoint) return ESP_ERR_NO_MEM;
    eros_router_register_endpoint(cfg->router, endpoint);
    return ESP_OK;
}

void eros_console_feed_bytes(const uint8_t *data, size_t n)
{
    if (!endpoint || !data || n == 0) return;
    config.on_input(config.on_input_ctx, data, n);
}

int eros_console_publish(const char *data, size_t len)
{
    if (!endpoint || !data || len == 0) return -1;
    return eros_endpoint_publish_data(endpoint, config.console_group_id,
                                      (uint8_t *)data, len, 0);
}

eros_endpoint_t *eros_console_endpoint(void) { return endpoint; }
