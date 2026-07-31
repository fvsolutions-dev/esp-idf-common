#include "filestore.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "filestore";

#define BASE_MAX     96
#define PREFIX_MAX   12
#define EXT_MAX      8
#define PATH_MAX_LEN (BASE_MAX + FILESTORE_DAY_LEN + FILESTORE_NAME_LEN + 8)
#define INDEX_LINE_MAX 160
#define HEADER_MAX   512

#define WIP_DIR      "wip"
#define NODATE_DIR   "nodate"
#define INDEX_FILE   ".index"
#define INDEX_TMP    ".index.new"
#define INDEX_MAGIC  "#filestore-index v1"

/* Retention keeps day count bounded too (a day summary is tracked in RAM);
   far above any realistic rotate/cap combination. */
#define MAX_DAYS     128

#define DEFAULT_MIN_VALID_EPOCH 1700000000LL   /* ~2023-11 */

typedef struct {
    char     day[FILESTORE_DAY_LEN];
    uint32_t files;
    uint64_t bytes;
} fs_day_t;

struct filestore {
    /* configuration (strings copied — the caller's config may be on the stack) */
    char     base[BASE_MAX];
    char     prefix[PREFIX_MAX];
    char     ext[EXT_MAX];
    uint32_t rotate_s;
    uint64_t rotate_b;
    uint32_t max_files;
    uint64_t max_bytes;
    uint32_t sync_every;
    filestore_header_cb_t header_cb;
    void    *header_ctx;
    uint32_t boot_id;
    int64_t  min_valid;

    SemaphoreHandle_t mtx;

    /* day summaries, sorted: nodate first, then ascending date. Totals are
       maintained incrementally; the indexes are only re-read on demand. */
    fs_day_t days[MAX_DAYS];
    int      n_days;
    uint32_t total_files;
    uint64_t total_bytes;
    uint32_t next_seq;

    /* open segment */
    FILE    *fp;
    char     open_name[FILESTORE_NAME_LEN];
    bool     open_nodate;
    int64_t  open_start;     /* epoch at open; 0 for nodate segments */
    int64_t  open_start_us;  /* esp_timer at open — rotation clock for nodate */
    uint64_t open_bytes;
    uint32_t dirty_bytes;
};

/* ---- small helpers --------------------------------------------------------- */

static void str_copy(char *dst, size_t cap, const char *src);

static int64_t now_epoch(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec;
}

/* Civil UTC date/time -> epoch seconds (Howard Hinnant's days-from-civil);
   newlib has no timegm() and mktime() applies the local timezone. */
static int64_t utc_epoch(int y, int mo, int d, int h, int mi, int s)
{
    y -= mo <= 2;
    const int64_t  era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (unsigned)(mo + (mo > 2 ? -3 : 9)) + 2u) / 5u
                         + (unsigned)d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    const int64_t days = era * 146097 + (int64_t)doe - 719468;
    return days * 86400 + h * 3600 + mi * 60 + s;
}

static void day_of_epoch(int64_t epoch, char *buf, size_t cap)
{
    time_t t = (time_t)epoch;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    snprintf(buf, cap, "%04d-%02d-%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
}

static bool is_date_day(const char *name)
{
    if (strlen(name) != 10) return false;
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) {
            if (name[i] != '-') return false;
        } else if (name[i] < '0' || name[i] > '9') {
            return false;
        }
    }
    return true;
}

static bool is_day_dir_name(const char *name)
{
    return is_date_day(name) || strcmp(name, NODATE_DIR) == 0;
}

/* nodate sorts before any date so it is both listed first and evicted first. */
static int day_cmp(const char *a, const char *b)
{
    const bool na = strcmp(a, NODATE_DIR) == 0;
    const bool nb = strcmp(b, NODATE_DIR) == 0;
    if (na || nb) return (na && nb) ? 0 : (na ? -1 : 1);
    return strcmp(a, b);
}

/* Segment basename. Dated: <prefix>-<seq>_<YYYY-MM-DD>_<HH-MM-SS>Z.<ext>
   No clock yet:          <prefix>-<seq>_boot<id>_<uptime>s.<ext> */
static void seg_name(const filestore_t *fs, char *out, size_t cap,
                     uint32_t seq, int64_t epoch_or_0)
{
    if (epoch_or_0 > 0) {
        time_t t = (time_t)epoch_or_0;
        struct tm tmv;
        gmtime_r(&t, &tmv);
        snprintf(out, cap, "%s-%06u_%04d-%02d-%02d_%02d-%02d-%02dZ.%s",
                 fs->prefix, (unsigned)seq,
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec, fs->ext);
    } else {
        snprintf(out, cap, "%s-%06u_boot%u_%llus.%s",
                 fs->prefix, (unsigned)seq, (unsigned)fs->boot_id,
                 (unsigned long long)(esp_timer_get_time() / 1000000), fs->ext);
    }
}

/* "<prefix>-000424_2026-07-31_09-15-00Z.csv" -> "2026-07-31";
   "<prefix>-000004_boot17_35s.csv"           -> "nodate".
   Used by crash recovery, where the name is the only record of the day. */
static void day_of_name(const char *name, char *out, size_t cap)
{
    const char *p = strchr(name, '_');
    if (p && strlen(p + 1) >= 10) {
        char day[FILESTORE_DAY_LEN];
        memcpy(day, p + 1, 10);
        day[10] = '\0';
        if (is_date_day(day)) {
            str_copy(out, cap, day);
            return;
        }
    }
    str_copy(out, cap, NODATE_DIR);
}

/* Start epoch back out of a dated basename (0 for nodate names). */
static int64_t start_of_name(const char *name)
{
    const char *p = strchr(name, '_');
    int y, mo, d, h, mi, s;
    if (p && sscanf(p + 1, "%4d-%2d-%2d_%2d-%2d-%2d", &y, &mo, &d, &h, &mi, &s) == 6)
        return utc_epoch(y, mo, d, h, mi, s);
    return 0;
}

/* Sequence number out of a basename ("<prefix>-<seq>_..."). */
static bool seq_of_name(const char *name, uint32_t *seq)
{
    const char *dash = strchr(name, '-');
    if (!dash) return false;
    unsigned s;
    if (sscanf(dash + 1, "%6u", &s) != 1) return false;
    *seq = s;
    return true;
}

static void dir_path(const filestore_t *fs, char *out, size_t cap, const char *sub)
{
    snprintf(out, cap, "%s/%.*s", fs->base, FILESTORE_DAY_LEN - 1, sub);
}

static void file_path(const filestore_t *fs, char *out, size_t cap,
                      const char *sub, const char *name)
{
    snprintf(out, cap, "%s/%.*s/%.*s", fs->base, FILESTORE_DAY_LEN - 1, sub,
             FILESTORE_NAME_LEN - 1, name);
}

static esp_err_t ensure_dir(const char *path)
{
    if (mkdir(path, 0777) == 0 || errno == EEXIST) return ESP_OK;
    ESP_LOGE(TAG, "mkdir %s: %s", path, strerror(errno));
    return ESP_FAIL;
}

static void fsync_file(FILE *f)
{
    fflush(f);
    fsync(fileno(f));
}

/* Bounded copy with silent truncation (snprintf-"%s" trips
   -Werror=format-truncation on 256-byte dirent names; here truncation is the
   intended behavior — over-long foreign names are skipped by the parsers). */
static void str_copy(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ---- day summaries ---------------------------------------------------------- */

static fs_day_t *day_find(filestore_t *fs, const char *day)
{
    for (int i = 0; i < fs->n_days; i++)
        if (strcmp(fs->days[i].day, day) == 0) return &fs->days[i];
    return NULL;
}

static fs_day_t *day_add(filestore_t *fs, const char *day)
{
    fs_day_t *d = day_find(fs, day);
    if (d) return d;
    if (fs->n_days >= MAX_DAYS) return NULL;   /* caller handles (GC frees days) */
    int i = fs->n_days++;
    while (i > 0 && day_cmp(fs->days[i - 1].day, day) > 0) {
        fs->days[i] = fs->days[i - 1];
        i--;
    }
    memset(&fs->days[i], 0, sizeof(fs->days[i]));
    str_copy(fs->days[i].day, sizeof(fs->days[i].day), day);
    return &fs->days[i];
}

static void day_remove(filestore_t *fs, const char *day)
{
    for (int i = 0; i < fs->n_days; i++) {
        if (strcmp(fs->days[i].day, day) == 0) {
            memmove(&fs->days[i], &fs->days[i + 1],
                    (size_t)(fs->n_days - i - 1) * sizeof(fs->days[0]));
            fs->n_days--;
            return;
        }
    }
}

static void account_add(filestore_t *fs, fs_day_t *d, uint64_t bytes)
{
    d->files++;
    d->bytes += bytes;
    fs->total_files++;
    fs->total_bytes += bytes;
}

static void account_sub(filestore_t *fs, fs_day_t *d, uint64_t bytes)
{
    if (d->files) d->files--;
    d->bytes = (d->bytes >= bytes) ? d->bytes - bytes : 0;
    if (fs->total_files) fs->total_files--;
    fs->total_bytes = (fs->total_bytes >= bytes) ? fs->total_bytes - bytes : 0;
}

/* ---- index I/O --------------------------------------------------------------
 *
 * One text line per closed segment: "<name> <size> <start> <end> <flag>".
 * Append + fsync on the hot path (segment close); every destructive change
 * (GC, delete, fsck) rewrites the whole file to a temp name and renames it
 * into place, so a power cut leaves either the old or the new index — never
 * a torn one. Index loss is repairable from readdir+stat (rebuild_index). */

static bool index_parse(const char *line, filestore_entry_t *e)
{
    unsigned long long size;
    long long start, end;
    char flag;
    char name[FILESTORE_NAME_LEN];
    if (sscanf(line, "%47s %llu %lld %lld %c", name, &size, &start, &end, &flag) != 5)
        return false;
    if (name[0] == '#') return false;
    memcpy(e->name, name, sizeof(e->name));
    e->size = size;
    e->start_epoch = start;
    e->end_epoch = end;
    e->flags = flag;
    return true;
}

static void index_format_line(const filestore_entry_t *e, char *out, size_t cap)
{
    snprintf(out, cap, "%s %llu %lld %lld %c\n", e->name,
             (unsigned long long)e->size, (long long)e->start_epoch,
             (long long)e->end_epoch, e->flags);
}

static esp_err_t index_append(filestore_t *fs, const char *day,
                              const filestore_entry_t *e)
{
    char path[PATH_MAX_LEN];
    file_path(fs, path, sizeof(path), day, INDEX_FILE);

    struct stat st;
    const bool fresh = (stat(path, &st) != 0);

    FILE *f = fopen(path, "a");
    if (!f) {
        ESP_LOGE(TAG, "%s: index open %s: %s", fs->prefix, path, strerror(errno));
        return ESP_FAIL;
    }
    if (fresh) fprintf(f, "%s\n", INDEX_MAGIC);
    char line[INDEX_LINE_MAX];
    index_format_line(e, line, sizeof(line));
    fputs(line, f);
    fsync_file(f);
    fclose(f);
    return ESP_OK;
}

/* Stream a day's index entries; cb returns false to stop. Missing file is not
   an error (empty day). */
static void index_walk(filestore_t *fs, const char *day,
                       bool (*cb)(void *ctx, const filestore_entry_t *e), void *ctx)
{
    char path[PATH_MAX_LEN];
    file_path(fs, path, sizeof(path), day, INDEX_FILE);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[INDEX_LINE_MAX];
    while (fgets(line, sizeof(line), f)) {
        filestore_entry_t e;
        if (!index_parse(line, &e)) continue;
        if (!cb(ctx, &e)) break;
    }
    fclose(f);
}

/* Load a whole index into a heap array (destructive ops need the full set).
   Returns entry count, -1 on OOM; *out is malloc'd (caller frees, may be NULL
   when the day has no index). */
static int index_load(filestore_t *fs, const char *day, filestore_entry_t **out)
{
    *out = NULL;
    char path[PATH_MAX_LEN];
    file_path(fs, path, sizeof(path), day, INDEX_FILE);
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    filestore_entry_t *arr = NULL;
    int n = 0, cap = 0;
    char line[INDEX_LINE_MAX];
    while (fgets(line, sizeof(line), f)) {
        filestore_entry_t e;
        if (!index_parse(line, &e)) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            filestore_entry_t *bigger = realloc(arr, (size_t)cap * sizeof(*arr));
            if (!bigger) {
                free(arr);
                fclose(f);
                return -1;
            }
            arr = bigger;
        }
        arr[n++] = e;
    }
    fclose(f);
    *out = arr;
    return n;
}

/* Atomically replace a day's index with `n` entries; n == 0 removes it. */
static esp_err_t index_rewrite(filestore_t *fs, const char *day,
                               const filestore_entry_t *arr, int n)
{
    char path[PATH_MAX_LEN], tmp[PATH_MAX_LEN];
    file_path(fs, path, sizeof(path), day, INDEX_FILE);
    file_path(fs, tmp, sizeof(tmp), day, INDEX_TMP);

    if (n == 0) {
        unlink(path);
        return ESP_OK;
    }
    FILE *f = fopen(tmp, "w");
    if (!f) {
        ESP_LOGE(TAG, "%s: index tmp %s: %s", fs->prefix, tmp, strerror(errno));
        return ESP_FAIL;
    }
    fprintf(f, "%s\n", INDEX_MAGIC);
    for (int i = 0; i < n; i++) {
        char line[INDEX_LINE_MAX];
        index_format_line(&arr[i], line, sizeof(line));
        fputs(line, f);
    }
    fsync_file(f);
    fclose(f);
    if (rename(tmp, path) != 0) {
        ESP_LOGE(TAG, "%s: index rename %s: %s", fs->prefix, day, strerror(errno));
        unlink(tmp);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Rebuild a day's index from readdir+stat — the slow path, used only when the
   index is missing (legacy folder, lost index). Entries get flag 'X'; start
   epoch comes back out of the name when it is dated. */
static esp_err_t rebuild_index(filestore_t *fs, const char *day)
{
    char dpath[PATH_MAX_LEN];
    dir_path(fs, dpath, sizeof(dpath), day);
    DIR *d = opendir(dpath);
    if (!d) return ESP_FAIL;

    filestore_entry_t *arr = NULL;
    int n = 0, cap = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        uint32_t seq;
        if (ent->d_name[0] == '.') continue;
        if (!seq_of_name(ent->d_name, &seq)) continue;

        char fpath[PATH_MAX_LEN];
        file_path(fs, fpath, sizeof(fpath), day, ent->d_name);
        struct stat st;
        if (stat(fpath, &st) != 0) continue;

        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            filestore_entry_t *bigger = realloc(arr, (size_t)cap * sizeof(*arr));
            if (!bigger) {
                free(arr);
                closedir(d);
                return ESP_ERR_NO_MEM;
            }
            arr = bigger;
        }
        filestore_entry_t *e = &arr[n++];
        memset(e, 0, sizeof(*e));
        str_copy(e->name, sizeof(e->name), ent->d_name);
        e->size = (uint64_t)st.st_size;
        e->start_epoch = start_of_name(ent->d_name);
        e->end_epoch = 0;
        e->flags = 'X';
    }
    closedir(d);

    esp_err_t err = index_rewrite(fs, day, arr, n);
    if (err == ESP_OK && n > 0)
        ESP_LOGW(TAG, "%s: rebuilt %s/%s from directory (%d entries)",
                 fs->prefix, day, INDEX_FILE, n);
    free(arr);
    return err;
}

/* ---- startup: recovery + summary load --------------------------------------- */

/* Anything still in wip/ was open when power went. Move it to its day folder
   (parsed from its own name) and index it 'R'. The data inside is intact up
   to the last fsync; its true time span is in the data itself. */
static void recover_wip(filestore_t *fs)
{
    char wip[PATH_MAX_LEN];
    dir_path(fs, wip, sizeof(wip), WIP_DIR);

    for (;;) {
        /* Collect a bounded batch first — renaming while iterating a
           directory is not safe — and loop until a pass finds nothing. */
        char batch[8][FILESTORE_NAME_LEN];
        int n = 0;
        DIR *d = opendir(wip);
        if (!d) return;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL && n < 8) {
            if (ent->d_name[0] == '.') continue;
            str_copy(batch[n++], FILESTORE_NAME_LEN, ent->d_name);
        }
        closedir(d);
        if (n == 0) return;

        int progress = 0;
        for (int i = 0; i < n; i++) {
            char day[FILESTORE_DAY_LEN];
            day_of_name(batch[i], day, sizeof(day));

            char ddir[PATH_MAX_LEN], from[PATH_MAX_LEN], to[PATH_MAX_LEN];
            dir_path(fs, ddir, sizeof(ddir), day);
            file_path(fs, from, sizeof(from), WIP_DIR, batch[i]);
            file_path(fs, to, sizeof(to), day, batch[i]);

            struct stat st;
            const uint64_t size = (stat(from, &st) == 0) ? (uint64_t)st.st_size : 0;

            /* Nothing ever reached the disk before the cut — an empty orphan
               is noise, not data. Reap it instead of promoting it. */
            if (size == 0) {
                if (unlink(from) == 0) {
                    ESP_LOGI(TAG, "%s: reaped empty crash orphan %s",
                             fs->prefix, batch[i]);
                    progress++;
                }
                continue;
            }

            if (ensure_dir(ddir) != ESP_OK) continue;
            if (rename(from, to) != 0) {
                /* Unmovable — unlink so recovery can't loop on it forever. */
                ESP_LOGE(TAG, "%s: recover rename %s: %s — dropping",
                         fs->prefix, batch[i], strerror(errno));
                if (unlink(from) == 0) progress++;
                continue;
            }
            progress++;
            filestore_entry_t e = {
                .size        = size,
                .start_epoch = start_of_name(batch[i]),
                .end_epoch   = 0,
                .flags       = 'R',
            };
            str_copy(e.name, sizeof(e.name), batch[i]);
            index_append(fs, day, &e);
            ESP_LOGW(TAG, "%s: recovered crash orphan %s -> %s/ (%llu B)",
                     fs->prefix, batch[i], day, (unsigned long long)size);
        }
        if (progress == 0) return;   /* stuck files must not wedge boot */
    }
}

struct load_ctx {
    filestore_t *fs;
    fs_day_t    *d;
};

static bool load_entry_cb(void *ctx, const filestore_entry_t *e)
{
    struct load_ctx *lc = ctx;
    account_add(lc->fs, lc->d, e->size);
    uint32_t seq;
    if (seq_of_name(e->name, &seq) && seq >= lc->fs->next_seq)
        lc->fs->next_seq = seq + 1;
    return true;
}

/* Build the in-RAM day summaries by reading every day's index (one small file
   per day — never a per-segment stat). A day folder without an index gets one
   rebuilt first. Also clears stale index temp files from an interrupted
   rewrite (the rename never happened, so the real index is intact). */
static void load_summaries(filestore_t *fs)
{
    DIR *root = opendir(fs->base);
    if (!root) return;

    char days_seen[MAX_DAYS][FILESTORE_DAY_LEN];
    int n_seen = 0;
    struct dirent *ent;
    while ((ent = readdir(root)) != NULL) {
        if (!is_day_dir_name(ent->d_name)) continue;
        if (n_seen < MAX_DAYS)
            str_copy(days_seen[n_seen++], FILESTORE_DAY_LEN, ent->d_name);
    }
    closedir(root);

    for (int i = 0; i < n_seen; i++) {
        char tmp[PATH_MAX_LEN];
        file_path(fs, tmp, sizeof(tmp), days_seen[i], INDEX_TMP);
        unlink(tmp);   /* stale temp from an interrupted rewrite, if any */

        char ipath[PATH_MAX_LEN];
        file_path(fs, ipath, sizeof(ipath), days_seen[i], INDEX_FILE);
        struct stat st;
        if (stat(ipath, &st) != 0) {
            if (rebuild_index(fs, days_seen[i]) != ESP_OK) continue;
            if (stat(ipath, &st) != 0) {   /* empty folder: no index rebuilt */
                char ddir[PATH_MAX_LEN];
                dir_path(fs, ddir, sizeof(ddir), days_seen[i]);
                rmdir(ddir);
                continue;
            }
        }

        fs_day_t *d = day_add(fs, days_seen[i]);
        if (!d) break;   /* > MAX_DAYS on disk: excess untracked until GC/fsck */
        struct load_ctx lc = { .fs = fs, .d = d };
        index_walk(fs, days_seen[i], load_entry_cb, &lc);
        if (d->files == 0) day_remove(fs, days_seen[i]);
    }
}

/* ---- retention --------------------------------------------------------------
 *
 * Evict oldest-first: nodate, then the oldest day. Sizes come from the
 * summaries, so deciding costs no I/O; each pass unlinks the day's oldest
 * files and rewrites its index once. */

static bool over_cap(const filestore_t *fs)
{
    if (fs->max_files && fs->total_files > fs->max_files) return true;
    if (fs->max_bytes && fs->total_bytes > fs->max_bytes) return true;
    return false;
}

static void gc(filestore_t *fs)
{
    while (over_cap(fs) && fs->n_days > 0) {
        fs_day_t *victim_day = &fs->days[0];   /* sorted: oldest first */
        char day[FILESTORE_DAY_LEN];
        str_copy(day, sizeof(day), victim_day->day);

        filestore_entry_t *arr = NULL;
        int n = index_load(fs, day, &arr);
        if (n <= 0) {
            /* Index unreadable or empty while the summary says otherwise —
               drop the whole day rather than looping forever. */
            free(arr);
            fs->total_files -= victim_day->files;
            fs->total_bytes -= (victim_day->bytes <= fs->total_bytes)
                                   ? victim_day->bytes : fs->total_bytes;
            char ddir[PATH_MAX_LEN];
            dir_path(fs, ddir, sizeof(ddir), day);
            day_remove(fs, day);
            rmdir(ddir);
            continue;
        }

        /* Index order is close order (oldest first). Evict from the front
           until under cap or the day is drained. */
        int evicted = 0;
        while (evicted < n && over_cap(fs)) {
            char path[PATH_MAX_LEN];
            file_path(fs, path, sizeof(path), day, arr[evicted].name);
            if (unlink(path) != 0 && errno != ENOENT) {
                ESP_LOGW(TAG, "%s: gc unlink %s: %s", fs->prefix,
                         arr[evicted].name, strerror(errno));
                break;
            }
            account_sub(fs, victim_day, arr[evicted].size);
            evicted++;
        }
        if (evicted == 0) {
            free(arr);
            return;   /* something is stuck; don't spin */
        }

        if (evicted == n) {
            index_rewrite(fs, day, NULL, 0);
            char ddir[PATH_MAX_LEN];
            dir_path(fs, ddir, sizeof(ddir), day);
            day_remove(fs, day);
            rmdir(ddir);
        } else {
            index_rewrite(fs, day, arr + evicted, n - evicted);
        }
        ESP_LOGI(TAG, "%s: gc evicted %d from %s/ (%u files, %llu KB total left)",
                 fs->prefix, evicted, day, (unsigned)fs->total_files,
                 (unsigned long long)(fs->total_bytes / 1024));
        free(arr);
    }
}

/* ---- segment lifecycle ------------------------------------------------------ */

static esp_err_t open_segment(filestore_t *fs)
{
    gc(fs);

    const int64_t now = now_epoch();
    const bool dated = (now >= fs->min_valid);
    const uint32_t seq = fs->next_seq;

    char name[FILESTORE_NAME_LEN], path[PATH_MAX_LEN];
    seg_name(fs, name, sizeof(name), seq, dated ? now : 0);
    file_path(fs, path, sizeof(path), WIP_DIR, name);

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "%s: open %s: %s", fs->prefix, path, strerror(errno));
        return ESP_FAIL;
    }
    fs->next_seq = seq + 1;
    fs->fp = f;
    str_copy(fs->open_name, sizeof(fs->open_name), name);
    fs->open_nodate = !dated;
    fs->open_start = dated ? now : 0;
    fs->open_start_us = esp_timer_get_time();
    fs->open_bytes = 0;
    fs->dirty_bytes = 0;

    if (fs->header_cb) {
        char hdr[HEADER_MAX];
        size_t hl = fs->header_cb(fs->header_ctx, hdr, sizeof(hdr));
        if (hl > sizeof(hdr)) hl = sizeof(hdr);
        if (hl > 0) {
            if (fwrite(hdr, 1, hl, f) != hl) {
                ESP_LOGE(TAG, "%s: header write: %s", fs->prefix, strerror(errno));
                fclose(f);
                fs->fp = NULL;
                return ESP_FAIL;
            }
            fs->open_bytes = hl;
            fsync_file(f);   /* header durable before any data */
        }
    }
    ESP_LOGI(TAG, "%s: segment %s open%s", fs->prefix, name,
             dated ? "" : " (no clock — nodate)");
    return ESP_OK;
}

/* The rename out of wip/ is the commit point: closed exactly when no longer
   there. Failure leaves the file in wip/ for the next boot's recovery. */
static void close_segment(filestore_t *fs)
{
    if (!fs->fp) return;
    fsync_file(fs->fp);
    fclose(fs->fp);
    fs->fp = NULL;

    char day[FILESTORE_DAY_LEN];
    if (fs->open_nodate) str_copy(day, sizeof(day), NODATE_DIR);
    else                 day_of_epoch(fs->open_start, day, sizeof(day));

    char ddir[PATH_MAX_LEN], from[PATH_MAX_LEN], to[PATH_MAX_LEN];
    dir_path(fs, ddir, sizeof(ddir), day);
    file_path(fs, from, sizeof(from), WIP_DIR, fs->open_name);
    file_path(fs, to, sizeof(to), day, fs->open_name);

    if (ensure_dir(ddir) != ESP_OK || rename(from, to) != 0) {
        ESP_LOGW(TAG, "%s: close of %s failed (%s) — left in %s/ for recovery",
                 fs->prefix, fs->open_name, strerror(errno), WIP_DIR);
        return;
    }

    filestore_entry_t e = {
        .size        = fs->open_bytes,
        .start_epoch = fs->open_start,
        .end_epoch   = fs->open_nodate ? 0 : now_epoch(),
        .flags       = '-',
    };
    str_copy(e.name, sizeof(e.name), fs->open_name);
    index_append(fs, day, &e);

    fs_day_t *d = day_add(fs, day);
    if (d) account_add(fs, d, fs->open_bytes);

    ESP_LOGI(TAG, "%s: segment %s closed into %s/ (%llu B)",
             fs->prefix, fs->open_name, day, (unsigned long long)fs->open_bytes);
}

static bool rotation_due(const filestore_t *fs)
{
    if (!fs->fp) return false;
    if (fs->rotate_b && fs->open_bytes >= fs->rotate_b) return true;
    if (fs->open_nodate && now_epoch() >= fs->min_valid)
        return true;   /* clock came in — stop filing under nodate */
    if (fs->rotate_s) {
        /* Dated segments rotate on the wall clock; nodate ones on uptime
           (their epoch is meaningless and may jump when the clock is set). */
        const int64_t elapsed = fs->open_nodate
            ? (esp_timer_get_time() - fs->open_start_us) / 1000000
            : now_epoch() - fs->open_start;
        if (elapsed >= (int64_t)fs->rotate_s) return true;
    }
    return false;
}

/* ---- public API -------------------------------------------------------------- */

esp_err_t filestore_open(const filestore_config_t *cfg, filestore_t **out)
{
    if (!cfg || !out || !cfg->base_dir || !cfg->prefix || !cfg->ext)
        return ESP_ERR_INVALID_ARG;
    if (strlen(cfg->base_dir) >= BASE_MAX || strlen(cfg->prefix) >= PREFIX_MAX ||
        strlen(cfg->ext) >= EXT_MAX)
        return ESP_ERR_INVALID_ARG;
    for (const char *p = cfg->prefix; *p; p++)   /* '-' and '_' delimit the name */
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')))
            return ESP_ERR_INVALID_ARG;

    filestore_t *fs = calloc(1, sizeof(*fs));
    if (!fs) return ESP_ERR_NO_MEM;

    str_copy(fs->base, sizeof(fs->base), cfg->base_dir);
    str_copy(fs->prefix, sizeof(fs->prefix), cfg->prefix);
    str_copy(fs->ext, sizeof(fs->ext), cfg->ext);
    fs->rotate_s   = cfg->rotate_seconds;
    fs->rotate_b   = cfg->rotate_bytes;
    fs->max_files  = cfg->max_files;
    fs->max_bytes  = cfg->max_total_bytes;
    fs->sync_every = cfg->sync_every_bytes;
    fs->header_cb  = cfg->header_cb;
    fs->header_ctx = cfg->header_ctx;
    fs->boot_id    = cfg->boot_id;
    fs->min_valid  = cfg->min_valid_epoch ? cfg->min_valid_epoch
                                          : DEFAULT_MIN_VALID_EPOCH;

    fs->mtx = xSemaphoreCreateMutex();
    if (!fs->mtx) {
        free(fs);
        return ESP_ERR_NO_MEM;
    }

    /* base_dir may not exist yet (first boot on a fresh mount) — create one
       level. Its parent is the mount point, which must exist. */
    esp_err_t err = ensure_dir(fs->base);
    if (err == ESP_OK) {
        char wip[PATH_MAX_LEN];
        dir_path(fs, wip, sizeof(wip), WIP_DIR);
        err = ensure_dir(wip);
    }
    if (err != ESP_OK) {
        vSemaphoreDelete(fs->mtx);
        free(fs);
        return err;
    }

    recover_wip(fs);
    load_summaries(fs);

    /* wip orphans also advance the counter (they may be newer than any
       indexed segment — recovery indexed them, but belt and braces). */
    ESP_LOGI(TAG, "%s: %s ready — %u files / %llu KB in %d day(s), next seq %06u",
             fs->prefix, fs->base, (unsigned)fs->total_files,
             (unsigned long long)(fs->total_bytes / 1024), fs->n_days,
             (unsigned)fs->next_seq);
    *out = fs;
    return ESP_OK;
}

esp_err_t filestore_close(filestore_t *fs)
{
    if (!fs) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(fs->mtx, portMAX_DELAY);
    close_segment(fs);
    xSemaphoreGive(fs->mtx);
    vSemaphoreDelete(fs->mtx);
    free(fs);
    return ESP_OK;
}

esp_err_t filestore_append(filestore_t *fs, const void *data, size_t len)
{
    if (!fs || (!data && len)) return ESP_ERR_INVALID_ARG;
    if (len == 0) return ESP_OK;

    xSemaphoreTake(fs->mtx, portMAX_DELAY);
    if (rotation_due(fs)) close_segment(fs);
    if (!fs->fp) {
        esp_err_t err = open_segment(fs);
        if (err != ESP_OK) {
            xSemaphoreGive(fs->mtx);
            return err;
        }
    }

    esp_err_t err = ESP_OK;
    if (fwrite(data, 1, len, fs->fp) != len) {
        ESP_LOGE(TAG, "%s: write: %s", fs->prefix, strerror(errno));
        err = ESP_FAIL;
    } else {
        fs->open_bytes += len;
        fs->dirty_bytes += len;
        if (fs->sync_every == 0 || fs->dirty_bytes >= fs->sync_every) {
            fsync_file(fs->fp);
            fs->dirty_bytes = 0;
        }
    }
    xSemaphoreGive(fs->mtx);
    return err;
}

esp_err_t filestore_rotate(filestore_t *fs)
{
    if (!fs) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(fs->mtx, portMAX_DELAY);
    close_segment(fs);
    xSemaphoreGive(fs->mtx);
    return ESP_OK;
}

esp_err_t filestore_sync(filestore_t *fs)
{
    if (!fs) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(fs->mtx, portMAX_DELAY);
    if (fs->fp) {
        fsync_file(fs->fp);
        fs->dirty_bytes = 0;
    }
    xSemaphoreGive(fs->mtx);
    return ESP_OK;
}

esp_err_t filestore_poll(filestore_t *fs)
{
    if (!fs) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(fs->mtx, portMAX_DELAY);
    if (rotation_due(fs)) close_segment(fs);
    xSemaphoreGive(fs->mtx);
    return ESP_OK;
}

esp_err_t filestore_stats(filestore_t *fs, filestore_stats_t *out)
{
    if (!fs || !out) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(fs->mtx, portMAX_DELAY);
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < fs->n_days; i++)
        if (fs->days[i].files) out->days++;
    out->files = fs->total_files;
    out->total_bytes = fs->total_bytes;
    out->segment_open = (fs->fp != NULL);
    if (fs->fp) {
        memcpy(out->open_name, fs->open_name, sizeof(out->open_name));
        out->open_bytes = fs->open_bytes;
    }
    out->next_seq = fs->next_seq;
    out->clock_valid = now_epoch() >= fs->min_valid;
    xSemaphoreGive(fs->mtx);
    return ESP_OK;
}

esp_err_t filestore_list_days(filestore_t *fs, filestore_day_cb_t cb, void *ctx)
{
    if (!fs || !cb) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(fs->mtx, portMAX_DELAY);
    for (int i = 0; i < fs->n_days; i++) {
        if (fs->days[i].files == 0) continue;
        if (!cb(ctx, fs->days[i].day, fs->days[i].files, fs->days[i].bytes)) break;
    }
    xSemaphoreGive(fs->mtx);
    return ESP_OK;
}

esp_err_t filestore_list_day(filestore_t *fs, const char *day,
                             filestore_entry_cb_t cb, void *ctx)
{
    if (!fs || !day || !cb || !is_day_dir_name(day)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(fs->mtx, portMAX_DELAY);
    index_walk(fs, day, cb, ctx);
    xSemaphoreGive(fs->mtx);
    return ESP_OK;
}

esp_err_t filestore_path(filestore_t *fs, const char *day, const char *name,
                         char *out, size_t cap)
{
    if (!fs || !day || !name || !out) return ESP_ERR_INVALID_ARG;
    if (!is_day_dir_name(day) || strchr(name, '/') ||
        strlen(name) >= FILESTORE_NAME_LEN || name[0] == '.')
        return ESP_ERR_INVALID_ARG;
    char tmp[PATH_MAX_LEN];
    file_path(fs, tmp, sizeof(tmp), day, name);
    if (strlen(tmp) >= cap) return ESP_ERR_INVALID_SIZE;
    strcpy(out, tmp);
    return ESP_OK;
}

esp_err_t filestore_delete(filestore_t *fs, const char *day, const char *name)
{
    if (!fs || !day || !name) return ESP_ERR_INVALID_ARG;
    if (!is_day_dir_name(day) || strchr(name, '/') || name[0] == '.')
        return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(fs->mtx, portMAX_DELAY);

    filestore_entry_t *arr = NULL;
    int n = index_load(fs, day, &arr);
    if (n < 0) {
        xSemaphoreGive(fs->mtx);
        return ESP_ERR_NO_MEM;
    }

    int hit = -1;
    for (int i = 0; i < n; i++)
        if (strcmp(arr[i].name, name) == 0) { hit = i; break; }

    char path[PATH_MAX_LEN];
    file_path(fs, path, sizeof(path), day, name);
    const bool unlinked = (unlink(path) == 0);

    esp_err_t err = ESP_OK;
    if (hit >= 0) {
        fs_day_t *d = day_find(fs, day);
        if (d) account_sub(fs, d, arr[hit].size);
        memmove(&arr[hit], &arr[hit + 1], (size_t)(n - hit - 1) * sizeof(*arr));
        n--;
        err = index_rewrite(fs, day, arr, n);
        if (n == 0) {
            char ddir[PATH_MAX_LEN];
            dir_path(fs, ddir, sizeof(ddir), day);
            day_remove(fs, day);
            rmdir(ddir);
        }
    } else if (!unlinked) {
        err = ESP_ERR_NOT_FOUND;   /* neither on disk nor in the index */
    }
    free(arr);
    xSemaphoreGive(fs->mtx);
    return err;
}

esp_err_t filestore_format(filestore_t *fs)
{
    if (!fs) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(fs->mtx, portMAX_DELAY);

    /* The open segment is about to be deleted — no finalize, just close. */
    if (fs->fp) {
        fclose(fs->fp);
        fs->fp = NULL;
    }

    /* Drain wip/ and every day folder, collect-then-unlink in batches (never
       mutate a directory while iterating it). */
    uint32_t removed = 0;
    for (int pass = 0; pass < 2; pass++) {   /* pass 0: wip, pass 1: days */
        for (;;) {
            char sub[FILESTORE_DAY_LEN];
            if (pass == 0) {
                str_copy(sub, sizeof(sub), WIP_DIR);
            } else {
                DIR *root = opendir(fs->base);
                if (!root) break;
                sub[0] = '\0';
                struct dirent *ent;
                while ((ent = readdir(root)) != NULL) {
                    if (is_day_dir_name(ent->d_name)) {
                        str_copy(sub, sizeof(sub), ent->d_name);
                        break;
                    }
                }
                closedir(root);
                if (sub[0] == '\0') break;
            }

            char dpath[PATH_MAX_LEN];
            dir_path(fs, dpath, sizeof(dpath), sub);
            bool any = true;
            while (any) {
                char batch[8][FILESTORE_NAME_LEN];
                int n = 0;
                DIR *d = opendir(dpath);
                if (!d) break;
                struct dirent *ent;
                while ((ent = readdir(d)) != NULL && n < 8) {
                    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                        continue;
                    str_copy(batch[n++], FILESTORE_NAME_LEN, ent->d_name);
                }
                closedir(d);
                any = (n > 0);
                int gone = 0;
                for (int i = 0; i < n; i++) {
                    char path[PATH_MAX_LEN];
                    file_path(fs, path, sizeof(path), sub, batch[i]);
                    if (unlink(path) == 0) {
                        removed++;
                        gone++;
                    }
                }
                if (any && gone == 0) break;   /* undeletable — don't spin */
            }
            if (pass == 0) break;      /* wip/ itself stays */
            rmdir(dpath);
        }
    }

    fs->n_days = 0;
    fs->total_files = 0;
    fs->total_bytes = 0;
    fs->next_seq = 0;
    ESP_LOGW(TAG, "%s: formatted — %u file(s) removed, seq reset",
             fs->prefix, (unsigned)removed);
    xSemaphoreGive(fs->mtx);
    return ESP_OK;
}

esp_err_t filestore_fsck(filestore_t *fs, uint32_t *fixed)
{
    if (!fs) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(fs->mtx, portMAX_DELAY);

    uint32_t repairs = 0;

    /* Reconcile each on-disk day folder against its index, then rebuild the
       in-RAM summaries from the result. */
    char days_seen[MAX_DAYS][FILESTORE_DAY_LEN];
    int n_seen = 0;
    DIR *root = opendir(fs->base);
    if (root) {
        struct dirent *ent;
        while ((ent = readdir(root)) != NULL)
            if (is_day_dir_name(ent->d_name) && n_seen < MAX_DAYS)
                str_copy(days_seen[n_seen++], FILESTORE_DAY_LEN, ent->d_name);
        closedir(root);
    }

    fs->n_days = 0;
    fs->total_files = 0;
    fs->total_bytes = 0;

    for (int i = 0; i < n_seen; i++) {
        filestore_entry_t *arr = NULL;
        int n = index_load(fs, days_seen[i], &arr);
        if (n < 0) continue;

        /* Directory pass: anything not indexed gets appended ('X'); anything
           indexed but gone gets dropped. */
        bool *seen = calloc((size_t)(n ? n : 1), sizeof(bool));
        bool changed = false;
        char dpath[PATH_MAX_LEN];
        dir_path(fs, dpath, sizeof(dpath), days_seen[i]);
        DIR *d = opendir(dpath);
        if (d && seen) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                uint32_t seq;
                if (ent->d_name[0] == '.') continue;
                if (!seq_of_name(ent->d_name, &seq)) continue;
                int hit = -1;
                for (int k = 0; k < n; k++)
                    if (strcmp(arr[k].name, ent->d_name) == 0) { hit = k; break; }
                if (hit >= 0) {
                    seen[hit] = true;
                    continue;
                }
                char fpath[PATH_MAX_LEN];
                file_path(fs, fpath, sizeof(fpath), days_seen[i], ent->d_name);
                struct stat st;
                if (stat(fpath, &st) != 0) continue;
                filestore_entry_t *bigger = realloc(arr, (size_t)(n + 1) * sizeof(*arr));
                bool *seen2 = bigger ? realloc(seen, (size_t)(n + 1) * sizeof(bool)) : NULL;
                if (!bigger || !seen2) {
                    free(bigger ? bigger : arr);
                    arr = NULL;
                    break;
                }
                arr = bigger;
                seen = seen2;
                memset(&arr[n], 0, sizeof(arr[n]));
                str_copy(arr[n].name, sizeof(arr[n].name), ent->d_name);
                arr[n].size = (uint64_t)st.st_size;
                arr[n].start_epoch = start_of_name(ent->d_name);
                arr[n].flags = 'X';
                seen[n] = true;
                n++;
                changed = true;
                repairs++;
            }
        }
        if (d) closedir(d);
        if (!arr) {
            free(seen);
            continue;
        }

        int kept = 0;
        for (int k = 0; k < n; k++) {
            if (!seen[k]) {   /* indexed but no longer on disk */
                changed = true;
                repairs++;
                continue;
            }
            arr[kept++] = arr[k];
        }
        free(seen);

        if (changed) index_rewrite(fs, days_seen[i], arr, kept);
        if (kept > 0) {
            fs_day_t *sum = day_add(fs, days_seen[i]);
            if (sum) {
                for (int k = 0; k < kept; k++) {
                    account_add(fs, sum, arr[k].size);
                    uint32_t seq;
                    if (seq_of_name(arr[k].name, &seq) && seq >= fs->next_seq)
                        fs->next_seq = seq + 1;
                }
            }
        } else {
            rmdir(dpath);
        }
        free(arr);
    }

    if (repairs)
        ESP_LOGW(TAG, "%s: fsck repaired %u entr%s", fs->prefix,
                 (unsigned)repairs, repairs == 1 ? "y" : "ies");
    if (fixed) *fixed = repairs;
    xSemaphoreGive(fs->mtx);
    return ESP_OK;
}
