/* Panic capture: a copy of the panic dump into the RTC boot buffer, replayed
 * to the log group on the next boot by eros_log_replay_boot_buffer(). The
 * native UART output is untouched — every wrapper delegates to __real_*.
 *
 * Mechanism: GNU ld --wrap on the panic_print_* family plus esp_panic_handler
 * (link options added by this component's CMakeLists). --wrap only rewrites
 * CROSS-TU references, which shapes everything here:
 *
 *   - Register dumps and backtraces are printed from panic_handler.c,
 *     panic_arch.c and debug_helpers.c → their panic_print_* calls are
 *     wrapped → captured char-for-char.
 *   - The "Guru Meditation Error" headline, abort details and "Rebooting..."
 *     are printed from inside panic.c itself → intra-TU, NOT wrapped. The
 *     headline and abort details are synthesized in __wrap_esp_panic_handler
 *     instead (esp_panic_handler is called cross-TU from panic_handler.c).
 *   - __real_panic_print_str's inner char loop is intra-TU too, so a wrapped
 *     str call is captured exactly once — no double capture.
 *
 * A future IDF bump can move print sites between translation units; the
 * on-device crash test (crash → reboot → replayed dump) is the real check.
 *
 * Panic context rules: no heap, no locks, no flash access, IRAM_ATTR, and the
 * RTC region pointer is cached at startup rather than resolved through
 * bootloader_common_get_rtc_retain_mem() (flash) at panic time. RTC FAST RAM
 * stays CPU-writable during panic on the S3, from either core.
 *
 * The region may be in any state when a panic hits: still armed (crash before
 * the replay drained it — panic text appends after the boot log) or finished
 * (magic zeroed — claim it first). Either way the next bootloader run finds a
 * valid magic, appends its own lines after ours, and the whole region replays
 * in chronological order. Overflow keeps the oldest text and counts drops, so
 * the headline and registers survive and the tail is what goes missing. */

#include "sdkconfig.h"

#if CONFIG_EROS_LOG_PANIC_CAPTURE

#include <stdint.h>

#include "bootloader_common.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_image_format.h"
#include "esp_private/panic_internal.h"
#include "esp_private/startup_internal.h"

#include "eros_log_boot_region.h"

extern void __real_panic_print_char(char c);
extern void __real_panic_print_str(const char *str);
extern void __real_panic_print_hex(int h);
extern void __real_panic_print_dec(int d);
extern void __real_esp_panic_handler(panic_info_t *info);

static eros_log_boot_region_t *s_region;

/* Priority 2: right after the boot-capture rearm (1), long before anything
   that could crash with a heap up (init_heap is 100). */
ESP_SYSTEM_INIT_FN(eros_log_panic_arm, CORE, BIT(0), 2)
{
    s_region = (eros_log_boot_region_t *)bootloader_common_get_rtc_retain_mem()->custom;
    return ESP_OK;
}

static IRAM_ATTR void panic_append_char(char c)
{
    eros_log_boot_region_t *r = s_region;
    if (!r || r->magic != EROS_LOG_BOOT_MAGIC) return;
    if (r->len >= EROS_LOG_BOOT_CAPACITY) {
        r->dropped++;
        return;
    }
    r->text[r->len++] = c;
}

static IRAM_ATTR void panic_append_str(const char *s)
{
    if (!s) return;
    while (*s) panic_append_char(*s++);
}

/* Mirrors panic_print_dec: at most two digits, space-padded. */
static IRAM_ATTR void panic_append_dec(int d)
{
    int n1 = d % 10;
    int n2 = d / 10;
    panic_append_char(n2 == 0 ? ' ' : (char)('0' + n2));
    panic_append_char((char)('0' + n1));
}

/* Mirrors panic_print_hex: eight digits, no 0x. */
static IRAM_ATTR void panic_append_hex(int h)
{
    for (int x = 0; x < 8; x++) {
        int c = (h >> 28) & 0xf;
        panic_append_char((char)(c < 10 ? '0' + c : 'a' + c - 10));
        h <<= 4;
    }
}

void IRAM_ATTR __wrap_panic_print_char(char c)
{
    panic_append_char(c);
    __real_panic_print_char(c);
}

void IRAM_ATTR __wrap_panic_print_str(const char *str)
{
    panic_append_str(str);
    __real_panic_print_str(str);
}

void IRAM_ATTR __wrap_panic_print_hex(int h)
{
    panic_append_hex(h);
    __real_panic_print_hex(h);
}

void IRAM_ATTR __wrap_panic_print_dec(int d)
{
    panic_append_dec(d);
    __real_panic_print_dec(d);
}

void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info)
{
    /* Crash before the boot replay drained the region: append after the boot
       log. Crash after: the region was finished (magic zeroed) — claim it. */
    if (s_region && s_region->magic != EROS_LOG_BOOT_MAGIC) {
        s_region->magic   = EROS_LOG_BOOT_MAGIC;
        s_region->len     = 0;
        s_region->dropped = 0;
    }

    /* Synthesize the intra-TU lines. Mirrors esp_panic_handler including the
       abort override it applies to info before printing. */
    panic_append_str("\r\n");
    if (g_panic_abort) {
        if (g_panic_abort_details) {
            panic_append_str(g_panic_abort_details);
        }
    } else {
        if (info->reason) {
            panic_append_str("Guru Meditation Error: Core ");
            panic_append_dec(info->core);
            panic_append_str(" panic'ed (");
            panic_append_str(info->reason);
            panic_append_str("). ");
        }
        if (info->description) {
            panic_append_str(info->description);
        }
    }
    panic_append_str("\r\n");

    __real_esp_panic_handler(info);
}

#endif /* CONFIG_EROS_LOG_PANIC_CAPTURE */
