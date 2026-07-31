# Generic component: no application headers, no project Kconfig — everything
# is configured through filestore_config_t so the component is reusable as-is.
idf_component_register(
    SRCS "filestore.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES log esp_timer
)
