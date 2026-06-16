#include "influxdb_logger.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#ifndef CONFIG_INFLUXDB_LOGGER_LP_BUF_SIZE
#define CONFIG_INFLUXDB_LOGGER_LP_BUF_SIZE 2048
#endif
#ifndef CONFIG_INFLUXDB_LOGGER_HTTP_TIMEOUT_MS
#define CONFIG_INFLUXDB_LOGGER_HTTP_TIMEOUT_MS 5000
#endif
#ifndef CONFIG_INFLUXDB_LOGGER_TASK_STACK
#define CONFIG_INFLUXDB_LOGGER_TASK_STACK 6144
#endif
#ifndef CONFIG_INFLUXDB_LOGGER_TASK_PRIORITY
#define CONFIG_INFLUXDB_LOGGER_TASK_PRIORITY 3
#endif
#ifndef CONFIG_INFLUXDB_LOGGER_TOKEN
#define CONFIG_INFLUXDB_LOGGER_TOKEN ""
#endif

static const char *TAG = "influxdb";

static struct {
    influxdb_logger_config_t cfg;
    SemaphoreHandle_t        lock;
    SemaphoreHandle_t        push_now_sig;
    influxdb_kv_t           *kv_buf;
    char                    *lp_buf;
    bool                     running;
} S;

static int escape_string_into(char *out, int out_size, const char *in)
{
    int n = 0;
    if (out_size < 1) return 0;
    while (*in && n + 2 < out_size) {
        if (*in == '"' || *in == '\\') {
            if (n + 3 >= out_size) break;
            out[n++] = '\\';
        }
        out[n++] = *in++;
    }
    out[n] = '\0';
    return n;
}

static int format_value(char *out, int out_size, const influxdb_kv_t *kv)
{
    switch (kv->kind) {
    case INFLUXDB_INT:
        return snprintf(out, out_size, "%" PRId64 "i", kv->i);
    case INFLUXDB_DOUBLE:
        return snprintf(out, out_size, "%.6g", kv->d);
    case INFLUXDB_STRING: {
        if (out_size < 3) return -1;
        out[0] = '"';
        int escaped = escape_string_into(out + 1, out_size - 2, kv->s ? kv->s : "");
        out[1 + escaped] = '"';
        out[2 + escaped] = '\0';
        return 2 + escaped;
    }
    }
    return -1;
}

static int build_line_protocol(char *buf, int buf_size,
                               const char *measurement,
                               const char *tags,
                               const influxdb_kv_t *fields, int n_fields)
{
    if (n_fields <= 0) return 0;

    int off = 0;
    int n = snprintf(buf + off, buf_size - off, "%s", measurement ? measurement : "sensors");
    if (n < 0 || n >= buf_size - off) return -1;
    off += n;

    if (tags && tags[0]) {
        n = snprintf(buf + off, buf_size - off, ",%s", tags);
        if (n < 0 || n >= buf_size - off) return -1;
        off += n;
    }

    if (off >= buf_size - 1) return -1;
    buf[off++] = ' ';

    bool first = true;
    for (int i = 0; i < n_fields; i++) {
        if (!fields[i].key) continue;

        if (!first) {
            if (off >= buf_size - 1) return -1;
            buf[off++] = ',';
        }
        first = false;

        n = snprintf(buf + off, buf_size - off, "%s=", fields[i].key);
        if (n < 0 || n >= buf_size - off) return -1;
        off += n;

        n = format_value(buf + off, buf_size - off, &fields[i]);
        if (n < 0 || n >= buf_size - off) return -1;
        off += n;
    }

    if (first) return 0;  /* nothing emitted */
    buf[off] = '\0';
    return off;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        ESP_LOGD(TAG, "rsp: %.*s", evt->data_len, (char *)evt->data);
    }
    return ESP_OK;
}

static esp_err_t do_post(const char *url, const char *body, int body_len)
{
    esp_http_client_config_t cfg = {
        .url           = url,
        .event_handler = http_event_handler,
        .timeout_ms    = CONFIG_INFLUXDB_LOGGER_HTTP_TIMEOUT_MS,
        .method        = HTTP_METHOD_POST,
    };
    if (strncmp(url, "https://", 8) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    esp_http_client_set_header(client, "Content-Type", "text/plain");
    if (sizeof(CONFIG_INFLUXDB_LOGGER_TOKEN) > 1) {
        char auth[160];
        snprintf(auth, sizeof(auth), "Token %s", CONFIG_INFLUXDB_LOGGER_TOKEN);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    esp_http_client_set_post_field(client, body, body_len);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "perform failed: %s", esp_err_to_name(err));
        if (S.cfg.on_failure) S.cfg.on_failure(S.cfg.cb_ctx, -err);
        return err;
    }
    if (status != 204) {
        ESP_LOGW(TAG, "unexpected status %d for %s", status, url);
        if (S.cfg.on_failure) S.cfg.on_failure(S.cfg.cb_ctx, status);
        return ESP_FAIL;
    }
    if (S.cfg.on_success) S.cfg.on_success(S.cfg.cb_ctx);
    return ESP_OK;
}

static esp_err_t collect_and_post(void)
{
    xSemaphoreTake(S.lock, portMAX_DELAY);
    const char *url         = S.cfg.url;
    const char *measurement = S.cfg.measurement;
    const char *tags        = S.cfg.tags;
    int max_fields          = S.cfg.max_fields;
    influxdb_provider_fn provider = S.cfg.provider;
    void *provider_ctx      = S.cfg.provider_ctx;
    xSemaphoreGive(S.lock);

    if (!url || !url[0]) return ESP_OK;
    if (!provider) return ESP_ERR_INVALID_STATE;

    int n = provider(S.kv_buf, max_fields, provider_ctx);
    if (n <= 0) return ESP_OK;

    int len = build_line_protocol(S.lp_buf, CONFIG_INFLUXDB_LOGGER_LP_BUF_SIZE,
                                  measurement, tags, S.kv_buf, n);
    if (len <= 0) {
        ESP_LOGW(TAG, "line protocol build failed (overflow?)");
        return ESP_ERR_NO_MEM;
    }

    return do_post(url, S.lp_buf, len);
}

static void logger_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(S.lock, portMAX_DELAY);
        uint32_t interval_ms = S.cfg.interval_ms;
        const char *url      = S.cfg.url;
        xSemaphoreGive(S.lock);

        if (!url || !url[0]) {
            /* No URL configured: sleep, re-check periodically. */
            xSemaphoreTake(S.push_now_sig, pdMS_TO_TICKS(1000));
            continue;
        }

        if (interval_ms == 0) {
            /* Manual-trigger mode: block until something calls push_now(), then
             * do exactly one post. The application drives the cadence. */
            if (xSemaphoreTake(S.push_now_sig, portMAX_DELAY) == pdTRUE) {
                collect_and_post();
            }
            continue;
        }

        /* Auto-interval mode: post immediately, then wait. push_now may wake
         * us early for an extra post. */
        collect_and_post();
        xSemaphoreTake(S.push_now_sig, pdMS_TO_TICKS(interval_ms));
    }
}

esp_err_t influxdb_logger_init(const influxdb_logger_config_t *cfg)
{
    if (!cfg || !cfg->provider) return ESP_ERR_INVALID_ARG;
    if (S.running) return ESP_ERR_INVALID_STATE;

    S.lock         = xSemaphoreCreateMutex();
    S.push_now_sig = xSemaphoreCreateBinary();
    if (!S.lock || !S.push_now_sig) return ESP_ERR_NO_MEM;

    int max_fields = cfg->max_fields > 0 ? cfg->max_fields : 64;
    S.kv_buf = calloc(max_fields, sizeof(influxdb_kv_t));
    S.lp_buf = malloc(CONFIG_INFLUXDB_LOGGER_LP_BUF_SIZE);
    if (!S.kv_buf || !S.lp_buf) return ESP_ERR_NO_MEM;

    S.cfg = *cfg;
    S.cfg.max_fields = max_fields;

    BaseType_t ok = xTaskCreate(logger_task, "influxdb", CONFIG_INFLUXDB_LOGGER_TASK_STACK,
                                NULL, CONFIG_INFLUXDB_LOGGER_TASK_PRIORITY, NULL);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;

    S.running = true;
    ESP_LOGI(TAG, "logger started (interval %u ms)", (unsigned)cfg->interval_ms);
    return ESP_OK;
}

void influxdb_logger_update_endpoint(const char *url, uint32_t interval_ms)
{
    if (!S.lock) return;
    xSemaphoreTake(S.lock, portMAX_DELAY);
    S.cfg.url         = url;
    S.cfg.interval_ms = interval_ms;
    xSemaphoreGive(S.lock);
    xSemaphoreGive(S.push_now_sig);
}

void influxdb_logger_update_measurement(const char *measurement)
{
    if (!S.lock) return;
    xSemaphoreTake(S.lock, portMAX_DELAY);
    S.cfg.measurement = measurement;
    xSemaphoreGive(S.lock);
}

void influxdb_logger_update_tags(const char *tags)
{
    if (!S.lock) return;
    xSemaphoreTake(S.lock, portMAX_DELAY);
    S.cfg.tags = tags;
    xSemaphoreGive(S.lock);
}

esp_err_t influxdb_logger_push_now(void)
{
    if (!S.running) return ESP_ERR_INVALID_STATE;
    xSemaphoreGive(S.push_now_sig);
    return ESP_OK;
}
