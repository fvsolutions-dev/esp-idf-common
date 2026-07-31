#pragma once

#include <stdint.h>

#include "sdkconfig.h"

/* RTC region layout — internal to eros_log. Shared by the boot capture
   (compiled into bootloader AND app, see eros_log_boot_common.c) and the
   panic writer (panic/eros_log_panic.c), so all three writers agree on one
   struct. The region is bootloader_common_get_rtc_retain_mem()->custom. */

/* "ERL1". The reservation is outside the retain-mem CRC by default, so on a
   cold boot custom[] holds whatever was in RTC RAM — the magic is what tells
   a real buffer from power-on garbage. */
#define EROS_LOG_BOOT_MAGIC 0x45524C31u

typedef struct {
    uint32_t magic;
    uint32_t len;
    uint32_t dropped;
    char     text[];
} eros_log_boot_region_t;

#if CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC
#define EROS_LOG_BOOT_CAPACITY \
    (CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE - sizeof(eros_log_boot_region_t))
#endif
