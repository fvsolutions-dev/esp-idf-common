#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "eros_endpoint.h"
#include "eros_router.h"
#include "microrl.h"
#include "microrl_console.h"

/* Reusable EROS console: microrl on top of a buffered EROS endpoint.
   Inbound bytes via eros_console_feed_bytes go through microrl; output is
   published to the configured console group so every subscribed transport
   sees the same characters. Built-ins: help, ?, reboot, version. */

typedef struct {
    eros_router_t *router;
    uint8_t endpoint_id;
    uint8_t console_group_id;
    uint8_t queue_depth;
} eros_console_config_t;

/* Init AFTER every transport that subscribes to console_group_id is up —
   microrl emits its initial prompt during init. */
esp_err_t eros_console_init(const eros_console_config_t *cfg);

void eros_console_register_command(const char *name,
                                   console_command_callback_t cb,
                                   const char *help);

void eros_console_feed_bytes(const uint8_t *data, size_t n);

eros_endpoint_t *eros_console_endpoint(void);
