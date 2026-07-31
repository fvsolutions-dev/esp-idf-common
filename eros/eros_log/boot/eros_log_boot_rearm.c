/* App-side re-arm. cpu_start installs the console putc itself
   (esp_rom_install_channel_putc(1, NULL) + esp_rom_install_uart_printf), which
   wipes whatever the bootloader left installed — so the app has to put its own
   back. Priority 1 runs before init_heap (100) and add_psram_to_heap (103), which
   is what makes their output reachable. */

#include "eros_log_boot.h"

#include "esp_err.h"
#include "esp_private/startup_internal.h"
#include "sdkconfig.h"

#if CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC

ESP_SYSTEM_INIT_FN(eros_log_boot_rearm, CORE, BIT(0), 1)
{
    eros_log_boot_start();
    return ESP_OK;
}

#endif
