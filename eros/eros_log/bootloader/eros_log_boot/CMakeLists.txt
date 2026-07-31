# Bootloader-side half of the eros_log boot capture. Pulls
# eros_log_boot_common.c straight out of the app component so both binaries
# compile the identical region layout from one source — a duplicated struct
# here would be a silent handoff corruption the first time one side changed.
#
# Getting this component INTO the bootloader takes two steps in the consuming
# project (discoverable is not built — the bootloader subproject links an
# explicit COMPONENTS list):
#   1. idf_build_set_property(BOOTLOADER_EXTRA_COMPONENT_DIRS
#          "<...>/eros/eros_log/bootloader" APPEND)     # root CMakeLists
#   2. a glue component in bootloader_components/ with REQUIRES eros_log_boot
#
# WHOLE_ARCHIVE because bootloader_after_init is a weak override — nothing in
# the bootloader references it, so archive resolution leaves the object out and
# the weak no-op in bootloader_hooks.h wins silently.
idf_component_register(
    SRCS "eros_log_boot_hook.c"
         "${CMAKE_CURRENT_LIST_DIR}/../../boot/eros_log_boot_common.c"
    INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../boot"
    PRIV_REQUIRES bootloader_support
    WHOLE_ARCHIVE
)
