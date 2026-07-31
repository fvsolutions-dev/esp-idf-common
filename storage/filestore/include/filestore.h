#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* filestore — a rotating, day-foldered, crash-safe segment store over any
 * mounted POSIX filesystem (LittleFS, FATFS, SPIFFS-with-dirs, a host tmpdir
 * under test). The caller appends opaque bytes (CSV rows, log lines); the
 * store owns segment rotation, naming, day-folder layout, retention, crash
 * recovery and a per-day index that makes listings cheap.
 *
 * ON-DISK LAYOUT
 *
 *   <base>/wip/<prefix>-<seq>_<YYYY-MM-DD>_<HH-MM-SS>Z.<ext>   <- being written
 *   <base>/<YYYY-MM-DD>/<same basename>                        <- closed
 *   <base>/<YYYY-MM-DD>/.index                                 <- one line per closed file
 *   <base>/nodate/...                                          <- closed before the clock was valid
 *
 * e.g. wip/data-000424_2026-07-31_09-15-00Z.csv moving, on close, to
 *      2026-07-31/data-000424_2026-07-31_09-15-00Z.csv
 *
 * The basename never changes: closing a segment is ONE atomic rename out of
 * wip/ into its day folder. "Finished" is expressed by location, so crash
 * recovery is "move whatever is left in wip/" and a reader never has to
 * filter half-written files out of a listing. Metadata that used to be
 * appended to the filename (duration, byte count) lives in the day folder's
 * .index instead — names stay clean and stable, and sizes are still available
 * without a stat() per file (on LittleFS a stat is a fresh path lookup,
 * ~70 ms each at scale; the index is one small read per day).
 *
 * INDEX. Append-only on the hot path: closing a segment appends one line to
 * <day>/.index and fsyncs it. Deletions (GC, filestore_delete) rewrite the
 * day's index atomically (tmp + rename). If an index is missing or drifts
 * from the directory (e.g. files unlinked behind the store's back), it is
 * rebuilt from readdir+stat — automatically when missing, on demand via
 * filestore_fsck(). Losing an index never loses data.
 *
 * CLOCK. Segment names and day folders need a real wall clock. While the
 * clock is invalid (epoch < min_valid_epoch), segments are still written —
 * named by boot id + uptime — and close into nodate/ instead of a day
 * folder. Nothing is ever dropped for lack of a date. When the clock becomes
 * valid, the open nodate segment is closed and the next one is dated.
 *
 * DURABILITY. The header (if any) is fsynced at segment open; appends are
 * fsynced every sync_every_bytes. After a power cut the wip file holds
 * everything up to the last fsync; recovery at open() moves it into its day
 * folder and indexes it with the 'R' flag (its true duration is inside the
 * data, e.g. row timestamps — the store does not guess).
 *
 * CONCURRENCY. Every call is serialized on an internal mutex; append from
 * one task while another lists or deletes is safe. Listing callbacks run
 * under that mutex: keep them short and never call back into the store.
 */

typedef struct filestore filestore_t;

/* Day-folder name incl. NUL: "YYYY-MM-DD" or "nodate". */
#define FILESTORE_DAY_LEN   11
/* Max basename length incl. NUL. */
#define FILESTORE_NAME_LEN  48

/* One closed segment, as recorded in its day's .index. */
typedef struct {
    char     name[FILESTORE_NAME_LEN];
    uint64_t size;         /* bytes on disk at close/recovery time */
    int64_t  start_epoch;  /* segment open time; 0 = unknown (nodate) */
    int64_t  end_epoch;    /* segment close time; 0 = unknown (recovered) */
    char     flags;        /* '-' clean close, 'R' crash-recovered, 'X' re-indexed */
} filestore_entry_t;

/* Fills `buf` with the header every fresh segment starts with (e.g. a CSV
 * column row) and returns its length (<= cap). The header is written and
 * fsynced before the first append lands. NULL config.header_cb = headerless
 * segments (a plain-text log tree). */
typedef size_t (*filestore_header_cb_t)(void *ctx, char *buf, size_t cap);

/* Listing callbacks. Return false to stop the walk early. Both run under the
 * store's mutex — no filestore_* calls from inside. */
typedef bool (*filestore_day_cb_t)(void *ctx, const char *day,
                                   uint32_t files, uint64_t bytes);
typedef bool (*filestore_entry_cb_t)(void *ctx, const filestore_entry_t *e);

typedef struct {
    const char *base_dir;       /* mounted writable dir, e.g. "/store/data".
                                   Created (one level) if missing. */
    const char *prefix;         /* basename stem, short [a-z0-9]+: "data", "log" */
    const char *ext;            /* extension without the dot: "csv", "txt" */

    uint32_t rotate_seconds;    /* close a segment after this long (0 = never by time) */
    uint64_t rotate_bytes;      /* ... or after this many bytes (0 = never by size) */

    uint32_t max_files;         /* retention: GC evicts oldest once over (0 = uncapped) */
    uint64_t max_total_bytes;   /* retention by bytes — cheap now, sizes live in the
                                   index (0 = uncapped) */

    uint32_t sync_every_bytes;  /* fsync cadence for appends (0 = every append) */

    filestore_header_cb_t header_cb;  /* optional; see typedef */
    void    *header_ctx;

    uint32_t boot_id;           /* names nodate segments (e.g. a persisted boot
                                   counter); only uniqueness matters */
    int64_t  min_valid_epoch;   /* wall clock below this is "invalid" and routes
                                   segments to nodate/ (0 = default, ~2023-11) */
} filestore_config_t;

typedef struct {
    uint32_t days;              /* day folders with content (incl. nodate) */
    uint32_t files;             /* closed segments across all days */
    uint64_t total_bytes;       /* their summed size */
    bool     segment_open;
    char     open_name[FILESTORE_NAME_LEN];
    uint64_t open_bytes;        /* written to the open segment so far */
    uint32_t next_seq;
    bool     clock_valid;
} filestore_stats_t;

/* Mount must already be done. Creates base_dir and wip/ if missing, recovers
 * crash orphans out of wip/, loads the per-day indexes (rebuilding any that
 * are missing), and derives the sequence counter from what exists. */
esp_err_t filestore_open(const filestore_config_t *cfg, filestore_t **out);

/* Closes (and finalizes) the open segment, frees the instance. */
esp_err_t filestore_close(filestore_t *fs);

/* Append bytes to the current segment. Opens a fresh segment when there is
 * none (or rotation is due) — retention GC runs at that boundary, then the
 * header callback provides the first bytes. Never splits `len` across two
 * segments: rotation happens between appends, so one append = one segment. */
esp_err_t filestore_append(filestore_t *fs, const void *data, size_t len);

/* Close the open segment now; the next append opens a fresh one. No-op when
 * nothing is open. Use for external boundaries (a batch ended). */
esp_err_t filestore_rotate(filestore_t *fs);

/* fsync the open segment (e.g. before an expected power-down). */
esp_err_t filestore_sync(filestore_t *fs);

/* Time-driven housekeeping for idle writers: closes the open segment when
 * rotate_seconds has elapsed without an append. Call it from a slow timer if
 * segments must close on time even when no data arrives. */
esp_err_t filestore_poll(filestore_t *fs);

esp_err_t filestore_stats(filestore_t *fs, filestore_stats_t *out);

/* Walk day folders (newest last; nodate first) / one day's closed segments
 * (index order: close order). */
esp_err_t filestore_list_days(filestore_t *fs, filestore_day_cb_t cb, void *ctx);
esp_err_t filestore_list_day(filestore_t *fs, const char *day,
                             filestore_entry_cb_t cb, void *ctx);

/* Compose the full path of a closed segment (for open()/cat-style reads).
 * The store never holds closed files open, so reading them concurrently is
 * safe. */
esp_err_t filestore_path(filestore_t *fs, const char *day, const char *name,
                         char *out, size_t cap);

/* Delete one closed segment (unlink + atomic index rewrite). */
esp_err_t filestore_delete(filestore_t *fs, const char *day, const char *name);

/* Wipe everything under base_dir (open segment included) and reset the
 * sequence counter to 0. */
esp_err_t filestore_format(filestore_t *fs);

/* Reconcile every day's .index against the directory: un-indexed files are
 * added (flag 'X'), entries whose file is gone are dropped. `fixed` (may be
 * NULL) reports the number of repairs. Never needed on the happy path —
 * this exists for drift caused by raw unlink()s behind the store's back. */
esp_err_t filestore_fsck(filestore_t *fs, uint32_t *fixed);

#ifdef __cplusplus
}
#endif
