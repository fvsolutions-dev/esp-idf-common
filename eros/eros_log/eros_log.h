#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "eros_endpoint.h"
#include "eros_router.h"

/* Reusable EROS log fan-out.

   Owns one unbuffered "stdout source" endpoint on the caller-provided router,
   and on demand redirects ESP_LOGx and printf/stdout into that endpoint as
   keep-newest publishes to the configured log group. Any router endpoint
   subscribed to that group receives a copy. No project-specific endpoint or
   group IDs are baked in. */

typedef struct {
    eros_router_t *router;        /* router this log layer attaches to */
    uint8_t stdout_endpoint_id;   /* endpoint id for the stdout source */
    uint8_t log_group_id;         /* group bit subscribers join to receive logs */
    /* Keep ESP_LOGx going to the handler installed before capture (ESP-IDF
       stdio, i.e. the console UART) as well as publishing it to the log group,
       and leave stdout alone so raw printf() stays on the console.

       Zero-initialising gives the historical behaviour: capture MOVES the log
       off stdio rather than copying it. That is right when a router endpoint
       puts the log back on the console (the UART transport), and wrong when
       none does — there, capture silently takes the monitor away. */
    bool replay_to_original_source;
} eros_log_config_t;

/* Create the stdout source endpoint and register it on the router. Safe to
   call before any subscriber exists. Does NOT touch ESP_LOG or stdout yet —
   call eros_log_install_capture once subscribers are up. */
esp_err_t eros_log_init(const eros_log_config_t *cfg);

/* Redirect ESP_LOGx (esp_log_set_vprintf) and stdout (esp_vfs) into
   eros_log_publish. Call AFTER subscribers (UART/CDC/HID/...) are registered
   so the first redirected line has somewhere to land. */
esp_err_t eros_log_install_capture(void);

/* Publish a chunk through the log group with keep-newest semantics: if a
   subscriber's queue is full the oldest message is dropped to make room for
   the new one. Non-blocking. Bare LFs are CR-prefixed once at the source so
   every subscriber sees well-formed lines. */
void eros_log_publish(const uint8_t *data, size_t size);

/* The stdout source endpoint owned by this module. */
eros_endpoint_t *eros_log_stdout_endpoint(void);
