# Changelog

All notable changes to this project are documented here.

## [Unreleased]

### eros_log
- Capture is now strictly a tee: the displaced `ESP_LOG` handler keeps running
  (console output unchanged), a copy publishes to the log group. The stdout
  VFS redirection — and with it raw `printf()` capture and the
  `replay_to_original_source` flag — is removed.
- Absorbed the RTC boot-log capture (previously the consuming project's
  `early_log`): `boot/` (shared bootloader+app source, `eros_log_boot_*` API),
  `bootloader/eros_log_boot/` (bootloader component), and
  `eros_log_replay_boot_buffer()`. Bootloader hook is guarded on
  `CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC` (was an undefined reference without
  the reservation).
- New: panic capture (`EROS_LOG_PANIC_CAPTURE`) — `ld --wrap` on
  `panic_print_*` + `esp_panic_handler` copies the panic dump into the RTC
  region for next-boot replay; native UART panic output untouched.

### eros_console
- Rewritten as a thin, self-contained bridge (REQUIRES `eros` only): inbound
  bytes → `on_input` callback, `eros_console_publish` → console group. The
  microrl instance, task, buffered endpoint and built-in commands moved to the
  consuming project's console core.

### Initial public release
- First public release of `esp-idf-common`.
