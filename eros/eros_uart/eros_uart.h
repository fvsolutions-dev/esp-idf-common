#pragma once

#include <stddef.h>
#include <stdint.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "eros_endpoint.h"
#include "eros_router.h"

/* Reusable EROS UART transport. One buffered endpoint, one TX sink task
   (single writer, PM-lock guarded), one polled RX task that hands chunks
   to a caller-supplied callback. Caller subscribes the endpoint to groups
   after init. */

typedef struct {
    eros_router_t *router;
    uint8_t endpoint_id;
    uint8_t queue_depth;

    uart_port_t uart_port;
    uart_config_t uart_config;  /* baud, parity, source_clk, ... */
    int rx_pin;                 /* -1 to skip light-sleep wake config */

    void (*on_rx)(const uint8_t *data, size_t n);
} eros_uart_config_t;

esp_err_t eros_uart_init(const eros_uart_config_t *cfg);
eros_endpoint_t *eros_uart_endpoint(void);
