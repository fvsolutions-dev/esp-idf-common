#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "eros_endpoint.h"
#include "eros_router.h"

/* Thin EROS bridge for a console owned elsewhere. No microrl, no task, no
   command table — this component only moves bytes:

     transports (CDC, BLE stdio, ...) → eros_console_feed_bytes → on_input
     console output → eros_console_publish → CONSOLE group → all subscribers

   The application wires both ends: on_input to its console's input feed, and
   eros_console_publish registered as one of the console's output sinks. The
   endpoint is unbuffered and exists purely as the source identity for the
   group publishes — input decoupling is the console core's job. */

typedef void (*eros_console_input_fn)(void *ctx, const uint8_t *data, size_t len);

typedef struct {
    eros_router_t *router;
    uint8_t endpoint_id;
    uint8_t console_group_id;   /* group the console output is published to */
    eros_console_input_fn on_input;
    void *on_input_ctx;
} eros_console_config_t;

esp_err_t eros_console_init(const eros_console_config_t *cfg);

/* Inbound bytes from any EROS-side transport; forwards to on_input.
   Task context only (whatever on_input requires — typically not ISR-safe). */
void eros_console_feed_bytes(const uint8_t *data, size_t n);

/* Console output → CONSOLE group, non-blocking. Register this (via a small
   adapter) as a sink on the console core. Returns 0 on success. */
int eros_console_publish(const char *data, size_t len);

eros_endpoint_t *eros_console_endpoint(void);
