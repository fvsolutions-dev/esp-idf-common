# WHOLE_ARCHIVE because eros_log_boot_rearm.c and eros_log_panic.c define
# nothing anyone calls — an ESP_SYSTEM_INIT_FN table entry and linker-wrap
# overrides. Without it the objects are never pulled out of the archive, boot
# capture is never re-armed after cpu_start, and the only symptom is an empty
# buffer.
set(srcs
    "eros_log.c"
    "boot/eros_log_boot_common.c"
    "boot/eros_log_boot_rearm.c"
)
if(CONFIG_EROS_LOG_PANIC_CAPTURE)
    list(APPEND srcs "panic/eros_log_panic.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "." "boot"
    REQUIRES eros log esp_system bootloader_support
    WHOLE_ARCHIVE
)

if(CONFIG_EROS_LOG_PANIC_CAPTURE)
    # Cross-TU panic output goes through these; esp_panic_handler covers the
    # intra-TU lines (headline, abort details) that --wrap cannot reach. See
    # panic/eros_log_panic.c for the full map.
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=panic_print_char"
        "-Wl,--wrap=panic_print_str"
        "-Wl,--wrap=panic_print_hex"
        "-Wl,--wrap=panic_print_dec"
        "-Wl,--wrap=esp_panic_handler")
endif()
