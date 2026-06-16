#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    INFLUXDB_INT,
    INFLUXDB_DOUBLE,
    INFLUXDB_STRING,
} influxdb_value_kind_t;

typedef struct {
    const char *key;
    influxdb_value_kind_t kind;
    union {
        int64_t      i;
        double       d;
        const char  *s;
    };
} influxdb_kv_t;

/* Provider populates `out` with up to `max` K/V pairs and returns the count
 * actually written. Return 0 to skip this cycle silently. Both keys and (for
 * STRING kind) values must remain valid until the provider call returns. */
typedef int (*influxdb_provider_fn)(influxdb_kv_t *out, int max, void *ctx);

typedef struct {
    const char *url;             /* "http(s)://host:port/path"; NULL/"" disables    */
    const char *measurement;     /* line-protocol measurement name; e.g. "sensors"  */
    const char *tags;            /* pre-formatted "k=v,k=v" or NULL                 */
    uint32_t    interval_ms;     /* 0 disables                                      */
    int         max_fields;      /* maximum K/V pairs per cycle, sizes the buffer   */
    influxdb_provider_fn provider;
    void               *provider_ctx;
    void              (*on_success)(void *ctx);
    void              (*on_failure)(void *ctx, int status_or_errno);
    void               *cb_ctx;
} influxdb_logger_config_t;

/* Spawns the background task. The config struct is copied; the strings inside
 * it are stored by pointer — `update_endpoint` / `update_tags` replace them. */
esp_err_t influxdb_logger_init(const influxdb_logger_config_t *cfg);

/* Live updates — safe to call from any task. NULL/empty url or 0 interval_ms
 * silences the logger until valid values are provided again. */
void influxdb_logger_update_endpoint(const char *url, uint32_t interval_ms);
void influxdb_logger_update_measurement(const char *measurement);
void influxdb_logger_update_tags(const char *tags);

/* Forces an immediate push outside the schedule. Returns ESP_OK on 204, an
 * esp_err_t / HTTP-status-mapped error otherwise. */
esp_err_t influxdb_logger_push_now(void);

#ifdef __cplusplus
}
#endif
