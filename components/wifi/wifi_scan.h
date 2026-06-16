#pragma once
#include "esp_wifi.h"
#include "wifi.h"

typedef struct {
    wifi_connection_info_t connection_info;
    wifi_ap_record_t ap_record;
    int attempt_count;
} wifi_candidate_t;

esp_err_t wifi_scan_candidates(const wifi_connection_info_t *connection_details,
                               int connection_details_count,
                               wifi_candidate_t **candidates,
                               int *candidate_count);
void print_scan_candidates(const wifi_candidate_t *candidates,
                           int candidate_count);
wifi_candidate_t *get_next_candidate(wifi_candidate_t *candidates,
                                     int candidate_count, int max_attempts);
