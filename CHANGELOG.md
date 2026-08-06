# Changelog

All notable changes to this project are documented here.

## [Unreleased]

### wifi
- Scan waits on WIFI_EVENT_SCAN_DONE with its own 30 s bound instead of the
  blocking `esp_wifi_scan_start` wait, whose driver-estimated budget a scan
  under BLE coexistence overruns (the scan then completed after the API had
  already returned ESP_ERR_WIFI_TIMEOUT, results discarded). The SCAN_DONE
  handler is registered once (per-call register/unregister raced overlapping
  scans), and the custom 250/800 ms active dwell is gone — the driver refuses
  non-default scan times when Bluetooth is enabled, and association measured
  ~2x faster with defaults.
- New `wifi_resume()`: undo `wifi_stop()` (restart driver + management task);
  backs the console `wifi on` command.
- The disconnect handler honors `s_stopped`: a deliberate `wifi_stop()` no
  longer triggers a reconnect attempt whose refusal was logged as the
  (false) error "All candidates exhausted. No Wi-Fi connection possible."

### filestore (new)
- New `storage/filestore` component: rotating, day-foldered, crash-safe
  segment store over any mounted POSIX filesystem. Stable basenames (the
  close is one atomic rename out of `wip/`), per-day `.index` files carrying
  size/time-span/flags (no more metadata renamed into filenames, no `stat()`
  per file to list), `nodate/` bucket while the wall clock is invalid
  (instead of dropping data), retention GC by bytes and/or file count,
  crash-orphan recovery with empty-orphan reaping, `fsck` for index drift.

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
