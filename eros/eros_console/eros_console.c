#include "eros_console.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "eros.h"

static const char *TAG = "eros_console";

#define TASK_STACK 4096
#define TASK_PRIO  5

static eros_endpoint_t *endpoint;
static microrl_t microrl_context;
static console_command_list_t command_list;
static uint8_t console_group;

static int  console_write_cb(struct microrl *mrl, const char *str);
static void console_task(void *arg);
static void cmd_reboot(microrl_t *mrl, int argc, const char *const *argv);
static void cmd_version(microrl_t *mrl, int argc, const char *const *argv);

esp_err_t eros_console_init(const eros_console_config_t *cfg)
{
    if (!cfg || !cfg->router || cfg->queue_depth == 0) return ESP_ERR_INVALID_ARG;
    if (endpoint) return ESP_ERR_INVALID_STATE;

    console_group = cfg->console_group_id;

    endpoint = eros_buffered_endpoint_new(cfg->endpoint_id, cfg->router, cfg->queue_depth);
    if (!endpoint) return ESP_ERR_NO_MEM;
    eros_router_register_endpoint(cfg->router, endpoint);

    microrl_init(&microrl_context, console_write_cb, console_microrl_execute);
    microrl_context.userdata_ptr = &command_list;
    command_list.count = 0;
    microrl_set_complete_callback(&microrl_context, console_microrl_complete);

    console_register_command(&microrl_context, "help",    print_help_cmd, "print help");
    console_register_command(&microrl_context, "?",       print_help_cmd, "print help");
    console_register_command(&microrl_context, "reboot",  cmd_reboot,     "restart the device");
    console_register_command(&microrl_context, "version", cmd_version,    "print firmware version");

    if (xTaskCreate(console_task, "eros_console", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "console task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void eros_console_register_command(const char *name,
                                   console_command_callback_t cb,
                                   const char *help)
{
    if (!endpoint || !name || !cb) return;
    console_register_command(&microrl_context, name, cb, help);
}

void eros_console_feed_bytes(const uint8_t *data, size_t n)
{
    if (!endpoint || !data || n == 0) return;
    eros_package_t *pkg = eros_package_new((uint8_t *)data, n);
    if (!pkg) return;
    eros_endpoint_send(endpoint, pkg, 0);
    eros_package_delete(pkg);
}

eros_endpoint_t *eros_console_endpoint(void) { return endpoint; }

static int console_write_cb(struct microrl *mrl, const char *str)
{
    (void)mrl;
    if (!endpoint || !str) return 0;
    size_t len = strlen(str);
    if (len == 0) return 0;
    eros_endpoint_publish_data(endpoint, console_group, (uint8_t *)str, len, 0);
    return 0;
}

static void console_task(void *arg)
{
    (void)arg;
    while (1) {
        eros_package_t *pkg = eros_buffered_endpoint_receive(endpoint, portMAX_DELAY);
        if (!pkg) continue;
        if (pkg->data && pkg->size > 0) {
            microrl_processing_input(&microrl_context, pkg->data, pkg->size);
        }
        eros_package_delete(pkg);
    }
}

static void cmd_reboot(microrl_t *mrl, int argc, const char *const *argv)
{
    (void)argc; (void)argv;
    console_print(mrl, "rebooting...\r\n");
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_restart();
}

static void cmd_version(microrl_t *mrl, int argc, const char *const *argv)
{
    (void)argc; (void)argv;
    const esp_app_desc_t *d = esp_app_get_description();
    console_printf(mrl, "version: %s\r\nidf:     %s\r\nbuilt:   %s %s\r\n",
                   d->version, d->idf_ver, d->date, d->time);
}
