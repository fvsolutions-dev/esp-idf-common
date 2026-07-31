#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Boot log from before anything can carry it.
 *
 * CDC does not exist until TinyUSB enumerates and a host opens the port, and
 * EROS log capture cannot be installed until app_main. Everything before that —
 * the second-stage bootloader, then heap_init / esp_psram / spi_flash / coexist —
 * only ever reached UART, which is exactly the log you do not have on a unit with
 * no serial header. This captures it into RTC memory and hands it to whoever is
 * listening later (eros_log_replay_boot_buffer()).
 *
 * The carrier is the bootloader's custom RTC-FAST reservation
 * (CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC): a fixed address that the bootloader and
 * the app both resolve to the same place, and that the app's heap is kept off.
 * Both binaries must be built with the option — it changes rtc_retain_mem_t.
 *
 * The bootloader half lives in bootloader/eros_log_boot/ next to this file and
 * compiles eros_log_boot_common.c too, so both binaries share one region layout.
 * Consuming projects register it with
 *   idf_build_set_property(BOOTLOADER_EXTRA_COMPONENT_DIRS
 *       "<...>/eros/eros_log/bootloader" APPEND)
 * plus a glue component in bootloader_components/ whose REQUIRES pulls it into
 * the bootloader's COMPONENTS list — discoverable is not built.
 *
 * Not captured, and not capturable: the first-stage ROM lines
 * ("ESP-ROM:esp32s3-...", "boot:0x28 (SPI_FAST_FLASH_BOOT)"). They are printed
 * before any of our code exists in either binary. Also missed are the few lines
 * bootloader_init() itself prints, because it installs the console putc partway
 * through and overwrites ours — see the hook comment. */

#ifdef __cplusplus
extern "C" {
#endif

/* Route esp_rom_printf's output through us as well as the console. That covers
   ESP_EARLY_LOGx and the whole bootloader, which is all this window has.
   Idempotent, and a no-op if the RTC region looks uninitialised. */
void eros_log_boot_start(void);

/* True once some writer has claimed the region this power cycle. Distinguishes
   "nobody ever armed capture" from "armed, but nothing was logged" — worth
   telling apart, because the first means a hook did not link (both halves
   define symbols nothing calls, so they need WHOLE_ARCHIVE) and the second is
   merely uninteresting. */
bool eros_log_boot_armed(void);

/* Bytes buffered so far, with *out pointing into the region. Valid until
   eros_log_boot_finish(). */
size_t eros_log_boot_read(const char **out);

/* Bytes that did not fit. Non-zero means the region is too small for this
   boot — raise CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE. */
uint32_t eros_log_boot_dropped(void);

/* Stop capturing (console putc restored) and empty the region. Call once the
   real log path is up and the contents have been handed over — otherwise the
   buffer keeps filling for the rest of the run and replays on the next boot. */
void eros_log_boot_finish(void);

#ifdef __cplusplus
}
#endif
