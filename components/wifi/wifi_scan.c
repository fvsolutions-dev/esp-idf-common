#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi_scan.h"

static const char *TAG = "WIFI_SCAN";

/* Outer bound on one full scan. Generous on purpose: with BLE advertising
   sharing the single 2.4 GHz radio, coexistence drip-feeds the scan RF slots
   and a 13-channel sweep can stretch far past its nominal duration. */
#define SCAN_PATIENT_TIMEOUT_MS 30000

/* Persistent — registered once, never unregistered: per-call
   register/unregister raced overlapping scan attempts ("handler already
   registered, overwriting"). */
static SemaphoreHandle_t s_scan_done;

static void scan_done_handler(void *arg, esp_event_base_t base, int32_t id,
                              void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    xSemaphoreGive(s_scan_done);
}

esp_err_t wifi_scan_candidates(const wifi_connection_info_t *connection_details,
                               int connection_details_count,
                               wifi_candidate_t **candidates,
                               int *candidate_count)
{
    esp_err_t ret;
    uint16_t ap_num = 0;
    /* Default scan times on purpose: the driver REFUSES custom active dwell
       when Bluetooth is enabled ("Should use default active scan time
       parameter... when Bluetooth is enabled!!!!!!") — and with the patient
       SCAN_DONE wait below, long dwells buy nothing anyway. */
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
    };

    ESP_LOGI(TAG, "Starting scan for wifi candidates");

    /* Patient scan: non-blocking start + our own SCAN_DONE wait. The blocking
       variant times its wait from a driver estimate; under BLE coexistence
       the real scan runs longer, esp_wifi_scan_start() returned
       ESP_ERR_WIFI_TIMEOUT — and the scan then completed anyway, results
       discarded. Waiting on the event takes however long the radio needs. */
    if (s_scan_done == NULL) {
        s_scan_done = xSemaphoreCreateBinary();
        if (s_scan_done == NULL) return ESP_ERR_NO_MEM;
        ret = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                         scan_done_handler, NULL);
        if (ret != ESP_OK) return ret;
    }
    xSemaphoreTake(s_scan_done, 0);   /* drain a stale completion, if any */

    ret = esp_wifi_scan_start(&scan_config, false);
    if (ret == ESP_OK &&
        xSemaphoreTake(s_scan_done, pdMS_TO_TICKS(SCAN_PATIENT_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "scan still not done after %d ms — aborting it",
                 SCAN_PATIENT_TIMEOUT_MS);
        esp_wifi_scan_stop();
        ret = ESP_ERR_TIMEOUT;
    }
    if (ret != ESP_OK) {
        ESP_LOGE("WIFI", "Failed to start scan: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Scan finished");
    ret = esp_wifi_scan_get_ap_num(&ap_num);
    if (ret != ESP_OK) {
        ESP_LOGE("WIFI", "Failed to get AP count: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Disovered %d APs", ap_num);

    if (ap_num == 0) {
        *candidate_count = 0;
        *candidates = NULL;
        return ESP_OK;
    }

    wifi_ap_record_t *ap_records = calloc(ap_num, sizeof(wifi_ap_record_t));
    if (ap_records == NULL) {
        ESP_LOGE("WIFI", "Failed to allocate memory for AP records");
        return ESP_ERR_NO_MEM;
    }

    ret = esp_wifi_scan_get_ap_records(&ap_num, ap_records);
    if (ret != ESP_OK) {
        ESP_LOGE("WIFI", "Failed to get AP records: %s", esp_err_to_name(ret));
        free(ap_records);
        return ret;
    }
    for (int i = 0; i < ap_num; i++) {
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), MACSTR, MAC2STR(ap_records[i].bssid));
        ESP_LOGI(TAG, " - SSID: %16s, RSSI: %d, MAC: %s", (char *)ap_records[i].ssid,
                 ap_records[i].rssi, mac_str);
    }

    int match_count = 0;
    for (int i = 0; i < ap_num; i++) {
        for (int j = 0; j < connection_details_count; j++) {
            if (strcmp((const char *)ap_records[i].ssid,
                       connection_details[j].ssid) == 0) {
                match_count++;
            }
        }
    }

    if (match_count == 0) {
        *candidate_count = 0;
        *candidates = NULL;
        free(ap_records);
        return ESP_OK;
    }

    wifi_candidate_t *cand_list = calloc(match_count, sizeof(wifi_candidate_t));
    if (cand_list == NULL) {
        ESP_LOGE("WIFI", "Failed to allocate memory for candidate list");
        free(ap_records);
        return ESP_ERR_NO_MEM;
    }

    int candidate_index = 0;
    for (int i = 0; i < ap_num; i++) {
        for (int j = 0; j < connection_details_count; j++) {
            if (strcmp((const char *)ap_records[i].ssid,
                       connection_details[j].ssid) == 0) {
                cand_list[candidate_index].connection_info = connection_details[j];
                cand_list[candidate_index].ap_record = ap_records[i];
                candidate_index++;
            }
        }
    }
    free(ap_records);

    for (int i = 0; i < match_count - 1; i++) {
        for (int j = 0; j < match_count - i - 1; j++) {
            if (cand_list[j].ap_record.rssi < cand_list[j + 1].ap_record.rssi) {
                wifi_candidate_t temp = cand_list[j];
                cand_list[j] = cand_list[j + 1];
                cand_list[j + 1] = temp;
            }
        }
    }

    *candidates = cand_list;
    *candidate_count = match_count;
    return ESP_OK;
}

void print_scan_candidates(const wifi_candidate_t *candidates,
                           int candidate_count)
{
    if (candidates == NULL || candidate_count <= 0) {
        ESP_LOGI(TAG, "No wifi candidates found.");
        return;
    }

    ESP_LOGI(TAG, "Found %d wifi candidate(s):", candidate_count);
    for (int i = 0; i < candidate_count; i++) {
        const wifi_candidate_t *candidate = &candidates[i];
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), MACSTR,
                 MAC2STR(candidate->ap_record.bssid));
        ESP_LOGI(TAG, "- Candidate %d: %s: %s, RSSI: %d, MAC: %s", i + 1,
                 candidate->connection_info.ssid, candidate->connection_info.password,
                 candidate->ap_record.rssi, mac_str);
    }
}

wifi_candidate_t *get_next_candidate(wifi_candidate_t *candidates,
                                     int candidate_count, int max_attempts)
{
    wifi_candidate_t *next_candidate = NULL;
    int lowest_attempts = max_attempts;

    for (int i = 0; i < candidate_count; i++) {
        if (candidates[i].attempt_count < lowest_attempts) {
            lowest_attempts = candidates[i].attempt_count;
            next_candidate = &candidates[i];
        }
    }

    return next_candidate;
}
