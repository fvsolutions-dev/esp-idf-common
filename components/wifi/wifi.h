#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "extensions/extensions.h"  // ODIN_string_serialisation_extension_ops_t

#ifdef __cplusplus
extern "C" {
#endif

#define DEFFAULT_WIFI_SECURITY_TYPE WIFI_AUTH_WPA2_PSK

typedef struct {
    char ssid[32];
    char password[64];
} wifi_connection_info_t;

// Odin string-codec ops for wifi_connection_info_t — referenced by the
// generated OD (via string_serialiser: in wifi.yaml). Prints SSID + masked
// password; see wifi.c. Declared here so the generated OD.c (which includes
// wifi.h as an extra include) can take its address.
extern ODIN_string_serialisation_extension_ops_t wifi_connection_info_string_codec_ops;

typedef struct {
    const char *hostname;
} wifi_module_config_t;

typedef void (*wifi_credentials_provider_fn)(
    const wifi_connection_info_t **out_list,
    size_t *out_count,
    void *ctx);

esp_err_t wifi_init(const wifi_module_config_t *cfg);
void wifi_set_credentials_provider(wifi_credentials_provider_fn fn, void *ctx);
esp_err_t wifi_request_reconnect(void);
bool wifi_is_connected(void);

#ifdef __cplusplus
}
#endif
