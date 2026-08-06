# filestore

A rotating, day-foldered, crash-safe segment store over any mounted POSIX
filesystem (LittleFS, FATFS, a host tmpdir under test). Append opaque bytes
(CSV rows, log lines); the store owns segment rotation, naming, day-folder
layout, retention, crash recovery and a per-day index that makes listings
cheap enough for a USB-MSC view.

```
<base>/wip/data-000424_2026-07-31_09-15-00.csv     <- being written (local time)
<base>/2026-07-31/<same basename>                  <- closed (one atomic rename)
<base>/2026-07-31/.index                           <- name/size/start/end/flags per file
<base>/nodate/...                                  <- closed before the clock was valid
```

Design points (the full contract is documented in `include/filestore.h`):

- **The basename never changes.** Closing = one atomic rename out of `wip/`.
  Metadata (size, time span, flags) lives in the day's `.index`, not in the
  filename, so names stay clean and listings need no per-file `stat()` — on
  LittleFS a stat is a fresh path lookup, ~70 ms each at scale.
- **Crash recovery is "move whatever is left in `wip/`"**, indexed with flag
  `R`. Data is intact up to the last fsync; nothing guesses at durations.
- **No date, no drops.** With an invalid wall clock, segments are named by
  sequence + boot id and filed under `nodate/`. When the clock arrives, the
  store rotates to dated segments by itself.
- **Retention by bytes and/or file count**, oldest first (`nodate` first),
  affordable because sizes live in the index.
- **Indexes are a cache, not a truth.** Append+fsync on close; atomic
  rewrite (tmp + rename) on delete; a missing index is rebuilt from the
  directory, and `filestore_fsck()` reconciles drift caused by raw unlinks.
- Multi-instance (e.g. one store for CSV data, one for text logs) and
  mutex-serialized; only ESP-IDF dependencies (`log`, `esp_timer`).

Typical use: two instances under one mount — a data tree fed by a sampler
with a CSV `header_cb`, and a headerless log tree fed by a log sink — with a
USB-MSC or shell view built from `filestore_list_days` / `filestore_list_day`
/ `filestore_path`.
