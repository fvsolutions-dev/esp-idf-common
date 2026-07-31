/* bootloader_after_init, not before_init.
 *
 * before_init runs first, but bootloader_init() then calls
 * bootloader_console_init(), which does esp_rom_install_channel_putc(1, NULL)
 * followed by esp_rom_install_uart_printf() — anything installed earlier is
 * discarded. So the earliest point a putc survives is after that, which costs us
 * the handful of lines bootloader_init() prints itself (the second-stage banner,
 * chip revision, SPI mode). Everything after — partition table, boot partition
 * selection, esp_image segment loads, the entry line — is captured. */

#include "sdkconfig.h"

/* Without the RTC reservation there is no region to capture into, and
   bootloader_common_update_rtc_retain_mem/-get_rtc_retain_mem do not even
   exist (they live under CONFIG_BOOTLOADER_RESERVE_RTC_MEM) — defining the
   hook would be an undefined reference. Omit it entirely and IDF's weak
   no-op wins. */
#if CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC

#include "eros_log_boot.h"

#include "bootloader_common.h"

void bootloader_after_init(void)
{
    /* Validate the retain memory BEFORE claiming the region, or the text we are
       about to capture gets thrown away.
       bootloader_utility.c calls bootloader_common_update_rtc_retain_mem(.., true)
       later in this boot, which does
           if (!is_retain_mem_valid()) bootloader_common_reset_rtc_retain_mem();
       and that reset is a memset of the WHOLE rtc_retain_mem_t, custom[] included.
       An invalid CRC is the normal state on a cold boot — and on the first boot
       after this feature landed, because enabling the reserve changed
       sizeof(rtc_retain_mem_t) and moved the crc field, so the old value reads as
       garbage. Doing the update here means the later call finds a valid CRC and
       leaves our buffer alone.

       Our own appends do not invalidate it: with CUSTOM_RESERVE_RTC_IN_CRC off,
       rtc_retain_mem_size() stops at offsetof(custom).

       Side effect: reboot_counter is incremented twice per boot. Nothing in this
       project reads it — bootloader_common_get_rtc_retain_mem_reboot_counter is
       for factory-reset-after-N-reboots, which is not enabled. */
    bootloader_common_update_rtc_retain_mem(NULL, true);

    eros_log_boot_start();
}

#endif /* CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC */
