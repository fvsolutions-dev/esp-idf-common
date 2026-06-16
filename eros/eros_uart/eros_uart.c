#include "eros_uart.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "eros.h"
#include "sdkconfig.h"

#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

static const char *TAG = "eros_uart";

#define RX_BUF_SIZE   1024
#define TX_BUF_SIZE   2048
#define RX_CHUNK      256
#define TASK_STACK    4096
#define TASK_PRIO     4

static eros_endpoint_t *endpoint;
#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t tx_lock;
#endif
static uart_port_t s_port;
static void (*s_on_rx)(const uint8_t *, size_t);

static void uart_sink_task(void *arg);
static void uart_rx_task(void *arg);

esp_err_t eros_uart_init(const eros_uart_config_t *cfg)
{
    if (!cfg || !cfg->router || cfg->queue_depth == 0) return ESP_ERR_INVALID_ARG;
    if (endpoint) return ESP_ERR_INVALID_STATE;

    s_port  = cfg->uart_port;
    s_on_rx = cfg->on_rx;

    endpoint = eros_buffered_endpoint_new(cfg->endpoint_id, cfg->router,
                                          cfg->queue_depth);
    if (!endpoint) return ESP_ERR_NO_MEM;
    eros_router_register_endpoint(cfg->router, endpoint);

#if CONFIG_PM_ENABLE
    /* PM lock blocks light sleep mid-byte — released after uart_wait_tx_done
       so the bit-shifter has clocked the last bit out before APB can stop. */
    ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "uart_tx", &tx_lock));
#endif

    ESP_ERROR_CHECK(uart_driver_install(s_port, RX_BUF_SIZE, TX_BUF_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(s_port, &cfg->uart_config));

    if (cfg->rx_pin >= 0) {
        /* GPIO low-level wake fires on the UART start bit (sub-bit-time);
           UART hw wake on >=3 edges is the backup when the pin is already
           held low by the host. */
        ESP_ERROR_CHECK(gpio_wakeup_enable((gpio_num_t)cfg->rx_pin, GPIO_INTR_LOW_LEVEL));
        ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());
        ESP_ERROR_CHECK(uart_set_wakeup_threshold(s_port, 3));
        ESP_ERROR_CHECK(esp_sleep_enable_uart_wakeup(s_port));
    }

    if (xTaskCreate(uart_sink_task, "eros_uart_tx", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS ||
        xTaskCreate(uart_rx_task,   "eros_uart_rx", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

eros_endpoint_t *eros_uart_endpoint(void) { return endpoint; }

static void uart_sink_task(void *arg)
{
    (void)arg;
    while (1) {
        eros_package_t *pkg = eros_buffered_endpoint_receive(endpoint, portMAX_DELAY);
        if (!pkg) continue;
        if (pkg->data && pkg->size > 0) {
#if CONFIG_PM_ENABLE
            esp_pm_lock_acquire(tx_lock);
#endif
            uart_write_bytes(s_port, (const char *)pkg->data, pkg->size);
            uart_wait_tx_done(s_port, portMAX_DELAY);
#if CONFIG_PM_ENABLE
            esp_pm_lock_release(tx_lock);
#endif
        }
        eros_package_delete(pkg);
    }
}

static void uart_rx_task(void *arg)
{
    (void)arg;
    uint8_t chunk[RX_CHUNK];
    while (1) {
        /* Block for the FIRST byte only, then drain whatever else is
           already buffered without waiting. uart_read_bytes with a
           multi-byte length waits for length-or-timeout, so reading
           256 at once added up to the full timeout of latency to every
           keystroke — the console felt delayed. This way a lone byte is
           delivered immediately and bursts (paste, host scripts) still
           arrive batched. */
        int n = uart_read_bytes(s_port, chunk, 1, portMAX_DELAY);
        if (n != 1) continue;
        int more = uart_read_bytes(s_port, chunk + 1, sizeof(chunk) - 1, 0);
        if (more > 0) n += more;
        if (s_on_rx) s_on_rx(chunk, (size_t)n);
    }
}
