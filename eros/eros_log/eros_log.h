#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "eros_endpoint.h"
#include "eros_router.h"

/* Reusable EROS log fan-out.

   Owns one unbuffered "stdout source" endpoint on the caller-provided router.
   Capture is strictly additive: ESP_LOGx keeps flowing to whatever handler was
   installed before (ESP-IDF stdio, i.e. the console UART) and a copy is
   published keep-newest to the configured log group. The default output path
   is never redirected — a panic or an early failure always still reaches the
   console. Raw printf() is deliberately not captured: taking it would mean
   redirecting stdout, which is exactly the debug-hostile move this module
   refuses to make. Any router endpoint subscribed to the group receives the
   copy. No project-specific endpoint or group IDs are baked in. */

typedef struct {
    eros_router_t *router;        /* router this log layer attaches to */
    uint8_t stdout_endpoint_id;   /* endpoint id for the stdout source */
    uint8_t log_group_id;         /* group bit subscribers join to receive logs */
} eros_log_config_t;

/* Create the stdout source endpoint and register it on the router. Safe to
   call before any subscriber exists. Does NOT touch ESP_LOG yet —
   call eros_log_install_capture once subscribers are up. */
esp_err_t eros_log_init(const eros_log_config_t *cfg);

/* Tee ESP_LOGx (esp_log_set_vprintf): the displaced handler still runs, then
   a copy goes to eros_log_publish. stdout/printf are left alone. Call AFTER
   subscribers (UART/CDC/HID/...) are registered so the first captured line
   has somewhere to land. */
esp_err_t eros_log_install_capture(void);

/* Publish a chunk through the log group with keep-newest semantics: if a
   subscriber's queue is full the oldest message is dropped to make room for
   the new one. Non-blocking. Bare LFs are CR-prefixed once at the source so
   every subscriber sees well-formed lines. */
void eros_log_publish(const uint8_t *data, size_t size);

/* Hand over the RTC boot buffer (bootloader + pre-app_main log captured by
   boot/eros_log_boot) to the log group, then release it. Call once, after
   subscribers are up and before anything else is logged, so the stream reads
   in boot order. Publishes the text raw rather than through ESP_LOG — it
   already carries its own timestamps and colour codes. Logs a diagnostic for
   every outcome, including "never armed" (the hooks fail by simply not being
   linked). No-op-ish without CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC. */
esp_err_t eros_log_replay_boot_buffer(void);

/* The stdout source endpoint owned by this module. */
eros_endpoint_t *eros_log_stdout_endpoint(void);
