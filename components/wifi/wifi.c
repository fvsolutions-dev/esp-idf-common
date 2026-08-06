#include "esp_err.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

#include "wifi.h"
#include "wifi_scan.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "esp_netif.h"
#include "esp_log.h"

#include "odin.h"  // ODIN_string_serialisation_extension_ops_t, ODIN_parameter_t

#define CONFIG_ESP_MAXIMUM_RETRY 5

static const char *TAG = "WIFI";
#define stringify(s) #s

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);
static esp_err_t wifi_connect(void);
static const char *get_disconnect_reason_string(int reason);

static wifi_candidate_t *candidates;
static int candidate_count;

static volatile bool s_stopped;

static wifi_credentials_provider_fn s_creds_provider;
static void *s_creds_provider_ctx;

static void wifi_management_task(void *pvParameters);

void wifi_set_credentials_provider(wifi_credentials_provider_fn fn, void *ctx)
{
    s_creds_provider = fn;
    s_creds_provider_ctx = ctx;
}

esp_err_t wifi_init(const wifi_module_config_t *cfg)
{
    esp_netif_t *wifi_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t default_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&default_cfg));

    esp_event_handler_instance_t instance_any_id;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL,
        &instance_any_id));

    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set wifi mode");
        return ESP_FAIL;
    }
    
    ESP_ERROR_CHECK(esp_wifi_set_ps(cfg ? cfg->power_save : WIFI_PS_NONE));

    if (esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start wifi");
        return ESP_FAIL;
    }

    if (cfg && cfg->hostname) {
        if (esp_netif_set_hostname(wifi_netif, cfg->hostname) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set hostname");
            return ESP_FAIL;
        }
    }

    xTaskCreate(wifi_management_task, "wifi_startup_task", 4096, NULL, 5, NULL);

    return ESP_OK;
}

bool wifi_is_connected(void)
{
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return true;
    }
    return false;
}

static void wifi_management_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Starting wifi management task");
    while (true) {
        if (s_stopped) {
            ESP_LOGI(TAG, "Wi-Fi stopped — management task exiting");
            vTaskDelete(NULL);
        }
        if (!wifi_is_connected()) {
            if (s_creds_provider == NULL) {
                ESP_LOGW(TAG, "No credentials provider registered; skipping scan cycle");
            } else {
                const wifi_connection_info_t *list = NULL;
                size_t count = 0;
                s_creds_provider(&list, &count, s_creds_provider_ctx);

                wifi_scan_candidates(list, (int)count, &candidates, &candidate_count);

                print_scan_candidates(candidates, candidate_count);

                wifi_connect();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

static esp_err_t wifi_connect(void)
{
    // Deliberately stopped: a reconnect here would only fail with NOT_STARTED.
    if (s_stopped) return ESP_ERR_INVALID_STATE;

    if (candidate_count == 0) {
        ESP_LOGE(TAG, "No wifi candidates found");
        return ESP_FAIL;
    }

    wifi_candidate_t *candidate =
        get_next_candidate(candidates, candidate_count, 5);

    if (candidate == NULL) {
        ESP_LOGE(TAG, "Failed to get next candidate");
        return ESP_FAIL;
    }

    wifi_config_t wifi_config = {
        .sta = {.channel = candidate->ap_record.primary,
                .threshold.authmode = candidate->ap_record.authmode,
                .bssid_set = true}};
    memcpy(wifi_config.sta.bssid, candidate->ap_record.bssid,
           sizeof(wifi_config.sta.bssid));
    memcpy(wifi_config.sta.password, candidate->connection_info.password,
           sizeof(wifi_config.sta.password));
    memcpy(wifi_config.sta.ssid, candidate->connection_info.ssid,
           sizeof(wifi_config.sta.ssid));

    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), MACSTR,
             MAC2STR(candidate->ap_record.bssid));
    ESP_LOGI(TAG, "Connecting to %s (%s), attempt %d", wifi_config.sta.ssid,
             mac_str, candidate->attempt_count);

    if (esp_wifi_set_config(WIFI_IF_STA, &wifi_config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set wifi config");
        return ESP_FAIL;
    }

    candidate->attempt_count++;
    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect to the AP ret: %d", ret);
        return ret;
    }

    return ESP_OK;
}

esp_err_t wifi_stop(void)
{
    s_stopped = true;         // before the stop, so the handlers see it first
    return esp_wifi_stop();
}

esp_err_t wifi_resume(void)
{
    if (!s_stopped) return ESP_OK;   // never stopped — nothing to do
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi resume: esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }
    s_stopped = false;   // after the start, so a racing handler can't reconnect early
    /* The management task deleted itself when it saw s_stopped — respawn it
       so scan/connect/reconnect care resumes. */
    if (xTaskCreate(wifi_management_task, "wifi_startup_task", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "wifi resume: management task spawn failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Wi-Fi resumed");
    return ESP_OK;
}

esp_err_t wifi_request_reconnect(void)
{
    return wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    esp_err_t ret;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "Wi-Fi started");
            break;

        case WIFI_EVENT_SCAN_DONE:
            ESP_LOGI(TAG, "WIFI_EVENT_SCAN_DONE");
            break;

        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "Connected to AP.");
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *event =
                (wifi_event_sta_disconnected_t *)event_data;

            /* A deliberate wifi_stop() (e.g. time_sync done) also lands here
               (reason ASSOC_LEAVE — we left). Don't warn about it, and above
               all don't try to reconnect and then report the refusal as
               "all candidates exhausted". */
            if (s_stopped) {
                ESP_LOGI(TAG, "Disconnected (deliberate stop)");
                break;
            }

            ESP_LOGW(TAG, "Disconnected from AP. Reason: %s (%d)",
                     get_disconnect_reason_string(event->reason), event->reason);
            ret = wifi_connect();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "All candidates exhausted. No Wi-Fi connection possible.");
            }
            break;
        }

        default:
            break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            ESP_LOGI(TAG, "Got IP Address: " IPSTR, IP2STR(&event->ip_info.ip));
        }
    }
}

/* ---- Odin string-codec extension for wifi_connection_info_t ----
 *
 * Lets `odin-dump` render config.network.wifi.networks as readable entries
 * instead of the generic "<N elements x S B (custom)>" struct summary.
 * Attached via the type's `string_serialiser:` in wifi.yaml; the codegen wires
 * &wifi_connection_info_string_codec_ops onto the parameter's extension list,
 * and odin's string_codec invokes to_string() once per vector element.
 *
 * SECURITY: the password is NEVER printed. Only its presence/length is hinted
 * with a fixed mask, so a dump can't leak credentials over UART/CDC/BLE. */
static int wifi_connection_info_to_string(const ODIN_parameter_t *parameter,
                                          const void *data, size_t size,
                                          char *buffer, size_t buffer_size)
{
    (void)parameter;
    if (size < sizeof(wifi_connection_info_t)) {
        return snprintf(buffer, buffer_size, "<truncated wifi entry>");
    }
    const wifi_connection_info_t *net = (const wifi_connection_info_t *)data;

    // ssid is a fixed char[32]; guarantee a terminator before printing.
    char ssid[sizeof(net->ssid) + 1];
    memcpy(ssid, net->ssid, sizeof(net->ssid));
    ssid[sizeof(net->ssid)] = '\0';

    bool has_pass = net->password[0] != '\0';
    return snprintf(buffer, buffer_size, "ssid=\"%s\" pass=%s",
                    ssid, has_pass ? "****" : "(none)");
}

ODIN_string_serialisation_extension_ops_t wifi_connection_info_string_codec_ops = {
    .to_string   = wifi_connection_info_to_string,
    .from_string = NULL,  // dump-only; writing credentials via string is not supported
};

static const char *get_disconnect_reason_string(int reason)
{
    switch (reason) {
    case WIFI_REASON_UNSPECIFIED:
        return stringify(WIFI_REASON_UNSPECIFIED);
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return stringify(WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT);
    case WIFI_REASON_BEACON_TIMEOUT:
        return stringify(WIFI_REASON_NO_AP_FOUND);
    default:
        return "Other reason";
    }
}
