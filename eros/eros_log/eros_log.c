#include "eros_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "eros.h"

static const char *TAG = "eros_log";

#define EROS_LOG_LINE_MAX    256
#define EROS_LOG_VFS_PATH    "/eros_out"

static eros_router_t *router;
static eros_endpoint_t *stdout_ep;
static uint8_t log_group;

/* Forward declarations — definitions below the entry points. */
static void stdout_ep_cb(eros_endpoint_t *ep, eros_package_t *pkg);
static int  eros_log_vprintf(const char *fmt, va_list args);
static ssize_t eros_log_vfs_write(int fd, const void *data, size_t size);
static int  eros_log_vfs_fstat(int fd, struct stat *st);
static const esp_vfs_t eros_vfs;

esp_err_t eros_log_init(const eros_log_config_t *cfg)
{
    if (!cfg || !cfg->router) return ESP_ERR_INVALID_ARG;
    if (stdout_ep) return ESP_ERR_INVALID_STATE;

    router = cfg->router;
    log_group = cfg->log_group_id;

    stdout_ep = eros_unbuffered_endpoint_new(cfg->stdout_endpoint_id, router, stdout_ep_cb);
    if (!stdout_ep) return ESP_ERR_NO_MEM;
    eros_router_register_endpoint(router, stdout_ep);
    return ESP_OK;
}

esp_err_t eros_log_install_capture(void)
{
    if (!stdout_ep) return ESP_ERR_INVALID_STATE;

    esp_log_set_vprintf(eros_log_vprintf);

    esp_err_t err = esp_vfs_register(EROS_LOG_VFS_PATH, &eros_vfs, NULL);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    freopen(EROS_LOG_VFS_PATH, "w", stdout);
    static char stdout_linebuf[EROS_LOG_LINE_MAX];
    setvbuf(stdout, stdout_linebuf, _IOLBF, sizeof(stdout_linebuf));

    ESP_LOGI(TAG, "EROS log capture active");
    return ESP_OK;
}

eros_endpoint_t *eros_log_stdout_endpoint(void) { return stdout_ep; }

/* ---------------------- supporting code below ---------------------- */

static void stdout_ep_cb(eros_endpoint_t *ep, eros_package_t *pkg)
{
    (void)ep;
    (void)pkg;
}

static void try_send_keep_newest(eros_endpoint_t *ep, eros_package_t *pkg)
{
    if (!ep) return;
    if (eros_endpoint_send(ep, pkg, 0) == 0) return;

    eros_package_t *old = NULL;
    if (ep->type == EROS_GATEWAY_BUFFERED) {
        old = eros_buffered_gateway_endpoint_receive(ep, 0);
    } else {
        old = eros_buffered_endpoint_receive(ep, 0);
    }
    if (old) {
        eros_package_delete(old);
    }
    eros_endpoint_send(ep, pkg, 0);
}

/* Insert a '\r' before every bare '\n'. *out_buf is NULL when no expansion
   was needed; otherwise caller frees. */
static size_t crlf_expand(const uint8_t *in, size_t n, uint8_t **out_buf)
{
    size_t expanded = n;
    for (size_t i = 0; i < n; i++) {
        if (in[i] == '\n' && (i == 0 || in[i - 1] != '\r')) {
            expanded++;
        }
    }
    if (expanded == n) {
        *out_buf = NULL;
        return n;
    }
    uint8_t *buf = malloc(expanded);
    if (!buf) {
        *out_buf = NULL;
        return n;
    }
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        if (in[i] == '\n' && (i == 0 || in[i - 1] != '\r')) {
            buf[j++] = '\r';
        }
        buf[j++] = in[i];
    }
    *out_buf = buf;
    return expanded;
}

/* Per-task recursion guard. ESP_LOG from inside an unbuffered subscriber's
   callback would re-enter publish on the same task and run away.
   __thread maps to FreeRTOS Task Local Storage on xtensa-esp. */
static __thread int publishing_depth;

void eros_log_publish(const uint8_t *data, size_t size)
{
    if (!router || !stdout_ep || size == 0) {
        return;
    }
    if (publishing_depth) {
        return;
    }
    publishing_depth = 1;

    uint8_t *expanded = NULL;
    size_t publish_size = crlf_expand(data, size, &expanded);
    const uint8_t *publish_data = expanded ? expanded : data;

    eros_package_t *pkg = eros_package_new((uint8_t *)publish_data, publish_size);
    if (!pkg) {
        free(expanded);
        publishing_depth = 0;
        return;
    }
    pkg->source = stdout_ep->id;
    pkg->type = EROS_PACKAGE_TYPE_GROUP;
    pkg->target.group = log_group;

    const uint32_t mask = 1u << log_group;
    for (uint8_t i = 0; i < router->endpoint_count; i++) {
        eros_endpoint_t *ep = router->endpoints[i];
        if (!ep) continue;
        if (ep->subscribed_group_bitmap & mask) {
            try_send_keep_newest(ep, pkg);
        }
    }

    eros_package_delete(pkg);
    free(expanded);
    publishing_depth = 0;
}

static int eros_log_vprintf(const char *fmt, va_list args)
{
    char stackbuf[EROS_LOG_LINE_MAX];

    va_list args_copy;
    va_copy(args_copy, args);
    int n = vsnprintf(stackbuf, sizeof(stackbuf), fmt, args_copy);
    va_end(args_copy);

    if (n < 0) {
        return n;
    }
    if ((size_t)n < sizeof(stackbuf)) {
        eros_log_publish((const uint8_t *)stackbuf, (size_t)n);
        return n;
    }

    /* Oversize: format again into a fitting buffer to avoid truncating mid
       UTF-8 sequence. */
    size_t needed = (size_t)n + 1;
    char *heapbuf = malloc(needed);
    if (!heapbuf) {
        eros_log_publish((const uint8_t *)stackbuf, sizeof(stackbuf) - 1);
        return n;
    }
    vsnprintf(heapbuf, needed, fmt, args);
    eros_log_publish((const uint8_t *)heapbuf, (size_t)n);
    free(heapbuf);
    return n;
}

static ssize_t eros_log_vfs_write(int fd, const void *data, size_t size)
{
    (void)fd;
    if (size > 0) {
        eros_log_publish((const uint8_t *)data, size);
    }
    return (ssize_t)size;
}

static int eros_log_vfs_fstat(int fd, struct stat *st)
{
    (void)fd;
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR;
    return 0;
}

static const esp_vfs_t eros_vfs = {
    .flags = ESP_VFS_FLAG_DEFAULT,
    .write = &eros_log_vfs_write,
    .fstat = &eros_log_vfs_fstat,
};
