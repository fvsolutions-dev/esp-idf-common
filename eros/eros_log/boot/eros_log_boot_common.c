/* Compiled into BOTH the bootloader and the app — see the two CMakeLists. Keep
   it free of anything the bootloader cannot link (no esp_log, no heap). */

#include "eros_log_boot.h"

#include <string.h>

#include "bootloader_common.h"
#include "esp_image_format.h"   /* rtc_retain_mem_t */
#include "esp_rom_serial_output.h"
#include "esp_rom_sys.h"
#include "sdkconfig.h"

#include "eros_log_boot_region.h"

#if CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC

static eros_log_boot_region_t *region(void)
{
    return (eros_log_boot_region_t *)bootloader_common_get_rtc_retain_mem()->custom;
}

/* Channel 1 is the ROM console channel esp_rom_printf writes through. */
#define ROM_CONSOLE_CHANNEL 1

static void capture_putc(char c)
{
    /* Console first, so a crash mid-capture still leaves the line on UART. */
    esp_rom_output_putc(c);

    eros_log_boot_region_t *r = region();
    if (r->magic != EROS_LOG_BOOT_MAGIC) {
        return;
    }
    if (r->len >= EROS_LOG_BOOT_CAPACITY) {
        r->dropped++;
        return;
    }
    r->text[r->len++] = c;
}

void eros_log_boot_start(void)
{
    eros_log_boot_region_t *r = region();
    if (r->magic != EROS_LOG_BOOT_MAGIC) {
        /* First writer this power cycle: claim the region. */
        r->magic   = EROS_LOG_BOOT_MAGIC;
        r->len     = 0;
        r->dropped = 0;
    }
    esp_rom_install_channel_putc(ROM_CONSOLE_CHANNEL, capture_putc);
}

bool eros_log_boot_armed(void)
{
    return region()->magic == EROS_LOG_BOOT_MAGIC;
}

size_t eros_log_boot_read(const char **out)
{
    eros_log_boot_region_t *r = region();
    if (r->magic != EROS_LOG_BOOT_MAGIC || r->len == 0) {
        return 0;
    }
    if (out) {
        *out = r->text;
    }
    return (r->len > EROS_LOG_BOOT_CAPACITY) ? EROS_LOG_BOOT_CAPACITY : r->len;
}

uint32_t eros_log_boot_dropped(void)
{
    eros_log_boot_region_t *r = region();
    return (r->magic == EROS_LOG_BOOT_MAGIC) ? r->dropped : 0;
}

void eros_log_boot_finish(void)
{
    esp_rom_install_channel_putc(ROM_CONSOLE_CHANNEL, esp_rom_output_putc);

    eros_log_boot_region_t *r = region();
    r->magic   = 0;   /* so the next boot does not replay this one */
    r->len     = 0;
    r->dropped = 0;
}

#else  /* !CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC */

void     eros_log_boot_start(void)                 {}
bool     eros_log_boot_armed(void)                 { return false; }
size_t   eros_log_boot_read(const char **out)      { (void)out; return 0; }
uint32_t eros_log_boot_dropped(void)               { return 0; }
void     eros_log_boot_finish(void)                {}

#endif
