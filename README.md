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

- `eros_log` — keep-newest log fan-out across every router endpoint subscribed to the configured log group. Hooks `esp_log_set_vprintf` and a stdout VFS on demand.
- `eros_console` — microrl on top of a buffered EROS endpoint. Built-in commands: `help`/`?`, `reboot`, `version`; `eros_console_register_command()` plugs in project commands.
- `eros_uart` — UART transport. Single-writer sink task with `esp_pm` lock around `uart_write_bytes` + `uart_wait_tx_done`. Polled RX task forwards chunks through optional callbacks. Optional GPIO + UART hardware light-sleep wake on the RX pin.
