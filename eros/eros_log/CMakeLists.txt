# WHOLE_ARCHIVE because eros_log_boot_rearm.c defines nothing anyone calls —
# just an ESP_SYSTEM_INIT_FN table entry. Without it the object is never pulled
# out of the archive, boot capture is never re-armed after cpu_start, and the
# only symptom is an empty buffer.
idf_component_register(
    SRCS "eros_log.c"
         "boot/eros_log_boot_common.c"
         "boot/eros_log_boot_rearm.c"
    INCLUDE_DIRS "." "boot"
    REQUIRES eros log esp_system bootloader_support
    WHOLE_ARCHIVE
)
