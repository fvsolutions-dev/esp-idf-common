# esp-idf-common

Reusable ESP-IDF components shared across fvsolutions-dev projects.

Each component group lives in its own top-level directory; add the ones you use to your project's `EXTRA_COMPONENT_DIRS`:

```cmake
list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/lib/esp-idf-common/components"
    "${CMAKE_CURRENT_LIST_DIR}/lib/esp-idf-common/eros"
)
```

## components/

- `ethernet` — W5500 SPI Ethernet driver wrapped in an `esp_netif` setup. Pins, hostname, and SPI host are caller-supplied via `ethernet_config_t`.
- `wifi` — Wi-Fi STA management task with scan-and-connect. Credentials are provided by the application through a registered callback (`wifi_set_credentials_provider`), keeping the component free of any project-specific config source.
- `influxdb_logger` — line-protocol writer over HTTP.

## eros/

EROS-based reusable plumbing (depends on the `eros` library). All take their router, endpoint id, and group ids by config — no project-specific defaults are baked in.

- `eros_log` — strictly-additive log copy: tees `esp_log_set_vprintf` (the original console handler keeps running — the default UART path is never redirected, raw `printf()` deliberately not captured) and publishes keep-newest to the configured log group. Owns the RTC boot buffer too: `boot/` captures the 2nd-stage-bootloader + pre-app_main log (compiled into both binaries from one source), `panic/` wraps the panic handler's print functions (`ld --wrap`) so the Guru Meditation dump lands in the same region, and `eros_log_replay_boot_buffer()` publishes it all on the next boot. The bootloader half (`bootloader/eros_log_boot/`) needs `BOOTLOADER_EXTRA_COMPONENT_DIRS` in the root CMakeLists **plus** a `bootloader_components/` glue component with `REQUIRES eros_log_boot` — extra-dir components the bootloader can discover are not built unless required. Note: with the tee always on, subscribing a UART0 endpoint to the log group double-prints.
- `eros_console` — thin console bridge, no microrl and no task: `eros_console_feed_bytes` forwards inbound transport bytes to a configured `on_input` callback, `eros_console_publish` sends console output to the configured console group (register it as an output sink on whatever console the project owns). Depends only on `eros`.
- `eros_uart` — UART transport. Single-writer sink task with `esp_pm` lock around `uart_write_bytes` + `uart_wait_tx_done`. Polled RX task forwards chunks through optional callbacks. Optional GPIO + UART hardware light-sleep wake on the RX pin. No longer wired by the sterigenics app (its UART0 is plain stdio + the app console's native binding).

## storage/

- `filestore` — rotating, day-foldered, crash-safe segment store over any mounted POSIX filesystem. Appends opaque bytes (CSV rows, log lines); owns rotation (time/size), naming, retention GC (bytes and/or count), crash recovery (atomic rename out of `wip/` is the commit point) and a per-day `.index` that makes listings cheap without a `stat()` per file. Segments written before the wall clock is valid go to `nodate/` instead of being dropped. Multi-instance, mutex-serialized, no EROS dependency. See its README.
