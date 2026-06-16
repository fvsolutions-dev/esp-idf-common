idf_component_register(
    SRCS "eros_console.c"
    INCLUDE_DIRS "."
    REQUIRES eros microrl_console esp_system app_update
)
