/* Actual Shell Anki miner (shell/anki.c) and its offline file queue
 * (shell/anki_queue.c, core/anki_queue.c). Only Vita threads, locks and the
 * Paf heap are substituted. The queue's files are real files in a temporary
 * directory, through the Vita file API mapped onto POSIX, with failed and
 * short writes, flushes, renames and directory reads injected. A started
 * job thread runs to completion inside sceKernelStartThread (or later, with
 * defer_start), as the real thread does before the control thread joins it.
 * Nothing here has a network: the Shell sends nothing (the Rubi app
 * exports the queue, host/tests/test_apkg.c). Lines starting "measure:" are
 * the arena peaks quoted in docs/anki.md.
 *
 * `--restart-step DIR ACTION [CRASH]` runs one step of test_anki_restart.py
 * in a fresh process instead. */
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define main acutest_main
#include "acutest.h"
#undef main
#include "test_tmpdir.h"
#include "afv_index_writer.h"
#include "anki.h"
#include "anki_index.h"
#include "json.h"
#define VJO_TEST_MUTEX_STUBS 1
#define VJO_TEST_THREAD_STUBS 1
#define VJO_TEST_CREATE_STUBS 1
#include <psp2/host_stubs.h>
#include <psp2/io/fcntl.h>
#include <psp2/rtc.h>
#include "../../shell/shell.h"

static uint64_t test_now;
uint64_t sceKernelGetProcessTimeWide(void) { return test_now += 1000; }
int sceKernelSetEventFlag(SceUID uid, unsigned bits) { return 0; }

/* The ledger's table is logged in a batch (shell/log_vita.c): lines here go out one by one. */
int vjo_log_batch_begin(VjoLogBatch *b, char *buf, int cap) { (void)b; (void)buf; (void)cap; return 0; }
void vjo_log_batch_end(VjoLogBatch *b) { (void)b; }
/* Formats like the Vita's, so every log call's arguments are exercised. */
static char last_log[512], last_queue_log[512];
static char all_logs[32768]; /* every line since reset(), for logged() */
static size_t all_logs_len;
void vjo_log(const char *fmt, ...)
{
    va_list ap;
    size_t n;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof(last_log), fmt, ap);
    va_end(ap);
    if (!strncmp(last_log, "anki queue: ", 12))
        memcpy(last_queue_log, last_log, sizeof(last_log));
    n = strlen(last_log);
    if (all_logs_len + n + 2 < sizeof(all_logs)) {
        memcpy(all_logs + all_logs_len, last_log, n);
        all_logs_len += n;
        all_logs[all_logs_len++] = '\n';
        all_logs[all_logs_len] = '\0';
    }
}
static int logged(const char *text) { return strstr(all_logs, text) != NULL; }
static int times_logged(const char *text)
{
    int n = 0;
    for (const char *p = all_logs; (p = strstr(p, text)) != NULL; p += strlen(text))
        n++;
    return n;
}

/* ---------------- threads, locks, view ---------------- */

static int held_box, held_view, mutex_deletes, fail_mutex;
static int threads_created, threads_started, threads_joined, threads_deleted;
static int fail_create, fail_start, fail_wait, defer_start;
static int (*thread_entry)(SceSize, void *);
VjoView g_view;

SceUID sceKernelCreateMutex(const char *name, unsigned int attr, int count, void *opt)
{
    if (fail_mutex)
        return -1;
    return !strcmp(name, "VjoAnkiBox") ? 91 : 93;
}
int sceKernelDeleteMutex(SceUID uid) { mutex_deletes++; return 0; }
/* Never the view inside the mailbox or the reverse: a lock order that the
 * paf thread (view) and the job (both) could deadlock on. */
int sceKernelLockMutex(SceUID uid, int count, void *timeout)
{
    if (uid == 91) { TEST_CHECK(!held_box && !held_view); held_box++; }
    if (uid == 81) { TEST_CHECK(!held_view && !held_box); held_view++; }
    return 0;
}
int sceKernelUnlockMutex(SceUID uid, int count)
{
    if (uid == 91) { TEST_CHECK(held_box == 1); held_box--; }
    if (uid == 81) { TEST_CHECK(held_view == 1); held_view--; }
    return 0;
}
void vjo_view_lock(void) { sceKernelLockMutex(81, 1, NULL); }
void vjo_view_unlock(void) { sceKernelUnlockMutex(81, 1); }

SceUID sceKernelCreateThread(const char *name, int (*entry)(SceSize, void *), int priority, unsigned int stack,
                             unsigned int attr, int affinity, void *opt)
{
    TEST_CHECK(!strcmp(name, "VjoAnki") && stack == 0x4000); /* VJO_STACK_SHELL_ANKI (checked below) */
    if (fail_create)
        return -1;
    thread_entry = entry;
    return 70 + ++threads_created;
}
int sceKernelStartThread(SceUID uid, SceSize size, void *args)
{
    if (fail_start)
        return -1;
    threads_started++;
    if (!defer_start)
        thread_entry(0, NULL);
    return 0;
}
int sceKernelWaitThreadEnd(SceUID uid, int *status, void *timeout)
{
    if (fail_wait)
        return -1;
    threads_joined++;
    return 0;
}
int sceKernelDeleteThread(SceUID uid) { threads_deleted++; return 0; }
static uint64_t rtc_unix_us = 1791158400007000ull; /* the UTC clock (a minute starts at 1791158400 s) */
int sceRtcGetCurrentTick(SceRtcTick *t) { t->tick = 62135596800000000ull + rtc_unix_us; return 0; }
static int clock_fails;
int sceRtcGetCurrentClockLocalTime(SceDateTime *t)
{
    if (clock_fails)
        return -1;
    *t = (SceDateTime){2026, 10, 12, 9, 30, 5, 0};
    return 0;
}

/* ---------------- Paf heap ---------------- */

static uint8_t heap[256 * 1024] __attribute__((aligned(16)));
static int heap_live, heap_attempts, heap_allocs, heap_frees, fail_heap;
static size_t heap_last;
void *vjo_shell_heap_alloc_for(int item, size_t bytes)
{
    TEST_CHECK(item == VJO_SL_PAF_ANKI && !heap_live);
    heap_attempts++;
    heap_last = bytes;
    if (fail_heap || bytes > sizeof(heap))
        return NULL;
    heap_allocs++;
    heap_live = 1;
    vjo_ledger_add(&g_shell_ledger, item, (uint32_t)bytes);
    memset(heap, 0xA5, bytes); /* reused memory is never zeroed */
    return heap;
}
void vjo_shell_heap_free(int item, void *p, size_t bytes)
{
    TEST_CHECK(p == heap && heap_live && item == VJO_SL_PAF_ANKI && bytes == heap_last);
    heap_live = 0;
    heap_frees++;
    vjo_ledger_sub(&g_shell_ledger, item, (uint32_t)bytes);
}
uint32_t vjo_shell_heap_free_size(void) { return 0; }

/* ---------------- the memory card: real files ---------------- */

static DIR *directory;
static char directory_path[256];
static int write_budget = -1, short_io, fail_rename, fail_sync, fail_close, fail_scan;
static int fail_mount_sync, mount_sync_calls, write_fd = -1, dir_scans;
static char write_path[256], last_rename[256];
static const char *crash_point;

/* Restart steps run this executable in fresh processes; _exit deliberately
 * skips every cleanup at the selected storage boundary. */
static void crash_at(const char *point)
{
    if (crash_point && !strcmp(crash_point, point))
        _exit(86);
}
static int ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s), k = strlen(suffix);
    return n >= k && !strcmp(s + n - k, suffix);
}
static int jpg_opens; /* sceIoOpen of a .jpg or .jpg.tmp: SceShell never opens one (AGENTS.md rule 1) */
int sceIoOpen(const char *p, int flags, unsigned mode)
{
    int fd = open(p, flags, mode);
    if (ends_with(p, ".jpg") || ends_with(p, ".jpg.tmp"))
        jpg_opens++;
    if (fd >= 0 && (flags & O_WRONLY)) {
        write_fd = fd;
        snprintf(write_path, sizeof(write_path), "%s", p);
    }
    return fd;
}
int sceIoClose(int fd) { int rc = close(fd); return fail_close ? -1 : rc; }
int sceIoRead(int fd, void *p, size_t n) { return (int)read(fd, p, short_io && n > 7 ? 7 : n); }
int sceIoWrite(int fd, const void *p, size_t n)
{
    if (!write_budget)
        return -1;
    if (write_budget > 0 && n > (size_t)write_budget)
        n = (size_t)write_budget;
    if (short_io && n > 7)
        n = 7;
    if (write_budget > 0)
        write_budget -= (int)n;
    int wrote = (int)write(fd, p, n);
    if (wrote > 0 && fd == write_fd && ends_with(write_path, ".json.tmp"))
        crash_at("json_partial");
    return wrote;
}
SceOff sceIoLseek(int fd, SceOff off, int whence) { return lseek(fd, off, whence); }
int sceIoMkdir(const char *p, unsigned mode) { return mkdir(p, mode); }
int sceIoGetstat(const char *p, SceIoStat *st)
{
    struct stat s;
    int rc = stat(p, &s);
    if (!rc) {
        st->st_mode = s.st_mode;
        st->st_size = s.st_size; /* (so a Shell that read a file it stats would show in the arena's peak too) */
    }
    return rc;
}
int sceIoRename(const char *from, const char *to)
{
    int rc = fail_rename ? -1 : rename(from, to);
    if (!rc) {
        snprintf(last_rename, sizeof(last_rename), "%s", to);
        if (ends_with(to, ".json")) crash_at("json_renamed");
    }
    return rc;
}
int sceIoRemove(const char *p) { return unlink(p); }
int sceIoSyncByFd(int fd, int flag)
{
    int rc = fail_sync ? -1 : fsync(fd);
    if (!rc && fd == write_fd && ends_with(write_path, ".json.tmp"))
        crash_at("json_flushed");
    return rc;
}
int sceIoSync(const char *device, unsigned flags)
{
    /* The mount-level metadata barrier, as real directory fsyncs. */
    const char *dirs[] = {VJO_ANKI_QUEUE_DIR, VJO_DATA_DIR, "ux0:data"};
    mount_sync_calls++;
    TEST_CHECK(!strcmp(device, "ux0:"));
    if (fail_sync || fail_mount_sync) return -1;
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(*dirs); i++) {
        int fd = open(dirs[i], O_RDONLY | O_DIRECTORY);
        if (fd < 0) return -1;
        int rc = fsync(fd);
        if (close(fd) < 0 || rc < 0) return -1;
    }
    if (ends_with(last_rename, ".json")) crash_at("json_committed");
    return 0;
}
int sceIoDopen(const char *p)
{
    TEST_ASSERT(!directory);
    dir_scans++;
    snprintf(directory_path, sizeof(directory_path), "%s", p);
    directory = opendir(p);
    return directory ? 10000 : -1;
}
int sceIoDread(int fd, SceIoDirent *ent)
{
    struct dirent *e;
    char path[512];
    if (fail_scan) return -1;
    e = readdir(directory);
    if (!e) return 0;
    snprintf(ent->d_name, sizeof(ent->d_name), "%s", e->d_name);
    snprintf(path, sizeof(path), "%s/%s", directory_path, e->d_name);
    TEST_ASSERT(sceIoGetstat(path, &ent->d_stat) == 0);
    return 1;
}
int sceIoDclose(int fd) { int rc = closedir(directory); directory = NULL; return rc; }

/* The shown result's dictionary page locators (worker.c), and their
 * entries for local_read_page below. */
static VjoLocalPages *view_pages;
static int view_page;
const VjoLocalPages *vjo_view_pages(int *page)
{
    TEST_CHECK(held_view == 1); /* under the view lock */
    *page = view_page;
    return view_pages;
}

/* Local dictionaries for the other pages a card records: small VJDICT2
 * files in memory (as test_local_cursor.c builds them), opened through the
 * platform's file_open as the Vita's local_open does. */
typedef struct {
    const char *key, *spelling, *reading, *meaning;
    const char *title; /* the file's own name (page 0 bytes 192..255); NULL: none */
} DictRow;
typedef struct {
    const char *name;
    uint8_t bytes[16384];
    size_t size;
    int active;
} DictFile;
static DictFile dict_files[8];
static int dict_opens;

static void put16(uint8_t *p, uint16_t x) { p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8); }
static void put32(uint8_t *p, uint32_t x) { put16(p, (uint16_t)x); put16(p + 2, (uint16_t)(x >> 16)); }
static void put64(uint8_t *p, uint64_t x) { put32(p, (uint32_t)x); put32(p + 4, (uint32_t)(x >> 32)); }
static uint64_t key_hash(const char *s)
{
    uint64_t h = UINT64_C(14695981039346656037);
    for (; *s; s++) h = (h ^ (uint8_t)*s) * UINT64_C(1099511628211);
    h ^= h >> 33; h *= UINT64_C(0xff51afd7ed558ccd);
    h ^= h >> 33; h *= UINT64_C(0xc4ceb9fe1a85ec53);
    return h ^ (h >> 33);
}
/* One key with one entry (VJDICT2: header, one hash slot, the key, its posting, the payload). */
static void dict_build(DictFile *f, const char *name, const DictRow *r)
{
    size_t off = 8192, key_len = strlen(r->key), posting, payload;
    const char *parts[] = {r->spelling, r->reading, r->meaning};
    memset(f, 0, sizeof(*f));
    f->name = name;
    memcpy(f->bytes, "VJDICT2\0", 8);
    put32(f->bytes + 8, 2); put32(f->bytes + 12, 64);
    put32(f->bytes + 16, 1); put32(f->bytes + 20, 24);
    put64(f->bytes + 24, 4096); put64(f->bytes + 32, off);
    put64(f->bytes + 48, 1); put64(f->bytes + 56, 1);
    if (r->title) memcpy(f->bytes + 192, r->title, strlen(r->title));
    memcpy(f->bytes + 4096, "VJHP", 4);
    put64(f->bytes + 4112, key_hash(r->key)); put64(f->bytes + 4120, off);
    put32(f->bytes + 4128, 1); put16(f->bytes + 4132, (uint16_t)key_len);
    memcpy(f->bytes + off, r->key, key_len); off += key_len;
    posting = off; off += 16;
    payload = off;
    for (int i = 0; i < 3; i++) {
        size_t n = strlen(parts[i]) + 1;
        memcpy(f->bytes + off, parts[i], n); off += n;
    }
    put64(f->bytes + posting, payload); put32(f->bytes + posting + 8, (uint32_t)(off - payload));
    put32(f->bytes + 4100, 1);
    f->size = off; put64(f->bytes + 40, off);
}
static int dict_read(void *ctx, uint64_t off, void *dst, size_t n)
{
    DictFile *f = ctx;
    if (off > f->size || n > f->size - (size_t)off) return -1;
    memcpy(dst, f->bytes + (size_t)off, n);
    return 0;
}
static void dict_close(void *ctx) { ((DictFile *)ctx)->active--; }
static int dict_open(void *ud, const char *path, VjoFile *out)
{
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    (void)ud;
    for (int i = 0; i < 8; i++)
        if (dict_files[i].name && !strcmp(name, dict_files[i].name)) {
            dict_files[i].active++;
            dict_opens++;
            *out = (VjoFile){&dict_files[i], dict_files[i].size, dict_read, dict_close, 0};
            return 0;
        }
    return -1;
}

/* Anki for Vita's duplicate index (core/anki_index.h): real files at VJO_ANKI_INDEX_PATH and its .old,
 * opened as the Vita's local_open opens a file (shell/platform_vita.c), their opens and reads counted. */
static int index_opens, index_failed_opens, index_reads, index_closes;
static size_t index_read_bytes;
static int index_read(void *ctx, uint64_t off, void *dst, size_t n)
{
    index_reads++;
    index_read_bytes += n;
    return pread((int)(intptr_t)ctx, dst, n, (off_t)off) == (ssize_t)n ? 0 : -1;
}
static void index_close(void *ctx)
{
    index_closes++;
    close((int)(intptr_t)ctx);
}
static int any_open(void *ud, const char *path, VjoFile *out)
{
    struct stat st;
    int fd;
    if (strcmp(path, VJO_ANKI_INDEX_PATH) && strcmp(path, VJO_ANKI_INDEX_OLD_PATH))
        return dict_open(ud, path, out);
    if ((fd = open(path, O_RDONLY)) < 0 || fstat(fd, &st) < 0) {
        if (fd >= 0)
            close(fd);
        index_failed_opens++;
        return -1;
    }
    index_opens++;
    *out = (VjoFile){(void *)(intptr_t)fd, (uint64_t)st.st_size, index_read, index_close, 0};
    return 0;
}

/* The Shell's platform, for its file reads (it has nothing else: the overlay is offline). */
void vjo_platform_vita(VjoPlatform *p)
{
    memset(p, 0, sizeof(*p));
    p->file_open = any_open;
}

#include "../../core/anki_queue.h"
#include "../../include/vjo_ledger_budget.h"
/* ---------------- a card's screenshot (API 23) ----------------
 * What the kernel and the game worker do for a shot job (include/vjo_game_ocr.h
 * VJO_GAME_OCR_F_SHOT), at once: the JPEG is on the card as <id>.jpg in the
 * queue (a real file, the bytes the game writes through the kernel), and the
 * job's result says how it went. The kernel's part (test_kernel_game_ocr.c) and
 * the game worker's with the real kernel (test_game_shot.c) are tested apart;
 * here, what the Shell does with them. The Shell never sees a JPEG byte: these
 * fakes write the file, anki.c only names it. */
/* A JPEG of a real one's size, the median of the 294 Vita screens at quality 70 (docs/anki.md), so that a
 * Shell that read it would show in its arena's peak: a JFIF start, bytes no marker, its end. */
#define SHOT_JPEG_BYTES 56934u
static uint8_t shot_jpeg[SHOT_JPEG_BYTES];
static void make_shot_jpeg(void)
{
    static const uint8_t head[] = {0xFF, 0xD8, 0xFF, 0xE0, 0, 16, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0};
    memcpy(shot_jpeg, head, sizeof(head));
    for (size_t i = sizeof(head); i < SHOT_JPEG_BYTES - 2; i++)
        shot_jpeg[i] = (uint8_t)(i * 131u % 251u); /* never 0xFF */
    shot_jpeg[SHOT_JPEG_BYTES - 2] = 0xFF;
    shot_jpeg[SHOT_JPEG_BYTES - 1] = 0xD9;
}
static int shot_jobs, shot_rc, shot_submit_rc, shot_no_file, slot_busy, slot_begins, slot_ends, shot_close_overlay;
static int shot_reads, shot_job_rc;
static uint32_t shot_seq;
static char shot_id[VJO_QUEUE_ID_SIZE];
int vjo_game_job_begin(int (*stop)(void *), int64_t wait_us)
{
    TEST_CHECK(stop != NULL && wait_us > 0 && !held_box && !held_view && slot_begins == slot_ends);
    slot_begins++;
    if (slot_busy) {
        slot_ends++; /* not taken: nothing to give back */
        return -1;
    }
    return 0;
}
void vjo_game_job_end(void)
{
    TEST_CHECK(slot_ends + 1 == slot_begins);
    slot_ends++;
}
int vjo_gstep_format(char *buf, size_t cap, const char *who, const VjoGameOcrStep *s)
{
    return snprintf(buf, cap, "gstep %s %u rc=0x%08X", who, (unsigned)s->id, (unsigned)s->rc);
}
int sceKernelDelayThread(unsigned int us) { (void)us; return 0; }
int vjoOcrSubmit(const VjoGameOcrRequest *r)
{
    char path[256];
    FILE *f;
    TEST_CHECK(slot_begins == slot_ends + 1); /* in the game worker's slot */
    TEST_CHECK(r->size == sizeof(*r) && r->flags == VJO_GAME_OCR_F_SHOT && !r->tiers && !r->speed && !r->model[0] &&
               !r->model_dir[0] && strlen(r->shot) == 32 && vjo_queue_valid_id(r->shot));
    if (shot_submit_rc)
        return shot_submit_rc;
    memcpy(shot_id, r->shot, sizeof(shot_id));
    shot_jobs++;
    shot_job_rc = shot_rc;
    if (!shot_rc && !shot_no_file) {
        /* The game's JPEG, through the kernel, beside the card to be, as kernel/game_ocr.c saves it: written
         * to <id>.jpg.tmp, then renamed. That rename never replaces a file (on the Vita it fails, EEXIST;
         * test_kernel_game_ocr.c), so the shot fails and the card goes without it unless the Shell removed
         * an earlier save's <id>.jpg first. */
        char final[sizeof(path) - 8];
        struct stat st;
        snprintf(final, sizeof(final), VJO_ANKI_QUEUE_DIR "/%s.jpg", r->shot);
        snprintf(path, sizeof(path), "%s.tmp", final);
        TEST_ASSERT((f = fopen(path, "wb")) != NULL);
        TEST_ASSERT(fwrite(shot_jpeg, 1, sizeof(shot_jpeg), f) == sizeof(shot_jpeg));
        TEST_ASSERT(fclose(f) == 0);
        crash_at("jpeg_tmp"); /* written, not yet named: a stop during the kernel's save (#161) */
        TEST_CHECK_(stat(final, &st) < 0, "a screenshot asked for over an existing %s", final);
        if (stat(final, &st) == 0) {
            unlink(path); /* the kernel removes its .tmp, and the shot fails (VJO_E_LOCAL_IO) */
            shot_job_rc = VJO_E_LOCAL_IO;
        } else {
            TEST_ASSERT(rename(path, final) == 0);
            crash_at("jpeg_saved"); /* on the card, its record not yet */
        }
    }
    if (shot_close_overlay == 1)
        __atomic_store_n(&g_view.open, 0, __ATOMIC_RELEASE); /* the user closes it meanwhile */
    return (int)++shot_seq;
}
int vjoOcrRead(uint32_t seq, VjoGameOcrResult *out)
{
    TEST_CHECK(seq == shot_seq && slot_begins == slot_ends + 1);
    shot_reads++;
    memset(out, 0, sizeof(*out));
    out->size = sizeof(*out);
    out->seq = seq;
    out->rc = shot_job_rc;
    out->width = 960;
    out->height = 544;
    out->shot_quality = 70;
    out->shot_tries = 1;
    out->shot_block = VJO_BUDGET_GAME_SHOT;
    out->shot_bytes = shot_job_rc ? 0 : (uint32_t)sizeof(shot_jpeg);
    out->n_steps = 1;
    out->step[0].id = VJO_GSTEP_SHOT;
    out->step[0].rc = shot_job_rc;
    if (shot_close_overlay == 2 && shot_reads == 1) {
        __atomic_store_n(&g_view.open, 0, __ATOMIC_RELEASE);
        return 1; /* JPEG frozen; file still being written */
    }
    if (shot_close_overlay == 2)
        TEST_CHECK(!strcmp(g_view.anki_status, "Saving in background; you can close the overlay"));
    return 0;
}
int vjoOcrCancel(uint32_t seq) { (void)seq; return 0; }
static void shot_reset(void)
{
    shot_jobs = shot_rc = shot_submit_rc = shot_no_file = slot_busy = slot_begins = slot_ends = 0;
    shot_close_overlay = shot_reads = shot_job_rc = jpg_opens = 0;
    shot_id[0] = '\0';
}

#include "../../shell/anki.c"
#include "../../shell/anki_queue.c"
#include "../../shell/ledger.c" /* the thread wrappers anki.c uses */

/* worker.c's client, on the fakes above */
static int client_submit(void *ud, const VjoGameOcrRequest *r) { (void)ud; return vjoOcrSubmit(r); }
static int client_read(void *ud, uint32_t seq, VjoGameOcrResult *r) { (void)ud; return vjoOcrRead(seq, r); }
static int client_cancel(void *ud, uint32_t seq) { (void)ud; return vjoOcrCancel(seq); }
static int64_t client_clock(void *ud) { (void)ud; return now_us(); }
static void client_delay(void *ud, uint32_t us) { (void)ud; sceKernelDelayThread(us); }
const VjoGameOcrClient vjo_game_client = {NULL, client_submit, client_read, client_cancel, client_clock, client_delay};

/* ---------------- fixtures ---------------- */

/* A shown local lookup: "猫が好き猫" with 猫 and 好き looked up. */
static const char *neko_meanings[] = {"cat", "shamisen \"neko\""};
static const char *suki_meanings[] = {"liked", "favourite"};
static VjoVocab neko = {.spelling = "猫", .reading = "ねこ", .rank = 1500, .meanings = neko_meanings, .n_meanings = 2};
static VjoVocab suki = {.spelling = "好き", .reading = "すき", .rank = VJO_NO_RANK, .meanings = suki_meanings,
                        .n_meanings = 2};
static VjoEntry entries[3];
static VjoEntryList list;
static VjoConfig cfg;

/* The overlay showing that lookup, and mining on. */
static void fixture(void)
{
    entries[0] = (VjoEntry){.vocab = &neko, .pos16 = 0, .hl_start = 0, .hl_end = 3, .text = "猫 (ねこ)"};
    entries[1] = (VjoEntry){.vocab = &suki, .pos16 = 2, .hl_start = 6, .hl_end = 12, .text = "好き (すき)"};
    entries[2] = (VjoEntry){.vocab = &neko, .pos16 = 4, .hl_start = 12, .hl_end = 15, .text = "猫 (ねこ)"};
    list = (VjoEntryList){.header = "猫が好き猫", .entries = entries, .n_entries = 3};
    g_view.open = 1;
    g_view.list = &list;
    g_view.list_seq = 5;
    vjo_config_defaults(&cfg);
    cfg.anki_mining = 1;
    /* text-only cards unless a test asks for the screenshot (anki_screenshot is on by default) */
    cfg.anki_screenshot = 0;
    clock_fails = 0;
    view_pages = NULL;
    view_page = 0;
}

static void clear_dir(const char *dir)
{
    char p[512];
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d)
        return;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        unlink(p);
    }
    closedir(d);
}

static int files_ending(const char *suffix)
{
    DIR *d = opendir(VJO_ANKI_QUEUE_DIR);
    struct dirent *e;
    int n = 0;
    if (!d)
        return 0;
    while ((e = readdir(d)))
        n += e->d_name[0] != '.' && ends_with(e->d_name, suffix) && strcmp(e->d_name, "exported");
    closedir(d);
    return n;
}

/* The one queued card's ID (the test has saved exactly one). */
static void only_id(char id[VJO_QUEUE_ID_SIZE])
{
    DIR *d = opendir(VJO_ANKI_QUEUE_DIR);
    struct dirent *e;
    int n = 0;
    TEST_ASSERT(d != NULL);
    while ((e = readdir(d)))
        if (strlen(e->d_name) == 37 && ends_with(e->d_name, ".json")) {
            snprintf(id, VJO_QUEUE_ID_SIZE, "%.32s", e->d_name);
            n++;
        }
    closedir(d);
    TEST_ASSERT(n == 1);
}

/* A queue file, read with POSIX calls (NUL-terminated). */
static char *queue_file(const char *id, const char *ext, size_t *len)
{
    static char data[2][64 * 1024];
    static int k;
    char path[256], *out = data[k++ % 2];
    FILE *f;
    size_t n;
    snprintf(path, sizeof(path), VJO_ANKI_QUEUE_DIR "/%s.%s", id, ext);
    if (!(f = fopen(path, "rb")))
        return NULL;
    n = fread(out, 1, sizeof(data[0]) - 1, f);
    fclose(f);
    out[n] = '\0';
    if (len)
        *len = n;
    return out;
}

static void reset(void)
{
    all_logs_len = 0;
    all_logs[0] = '\0';
    vjo_test_shell_free_rc = -1;
    vjo_test_shell_free_user = 0;
    vjo_ledger_shell_init();
    clear_dir(VJO_ANKI_EXPORTED_DIR);
    rmdir(VJO_ANKI_EXPORTED_DIR);
    clear_dir(VJO_ANKI_QUEUE_DIR);
    memset(&g_view, 0, sizeof(g_view));
    held_box = held_view = mutex_deletes = fail_mutex = 0;
    threads_created = threads_started = threads_joined = threads_deleted = 0;
    fail_create = fail_start = fail_wait = defer_start = 0;
    heap_live = heap_attempts = heap_allocs = heap_frees = fail_heap = 0;
    write_budget = -1;
    short_io = fail_rename = fail_sync = fail_close = fail_scan = fail_mount_sync = 0;
    mount_sync_calls = dir_scans = 0;
    write_fd = -1;
    write_path[0] = last_rename[0] = '\0';
    rtc_unix_us = 1791158400007000ull;
    shot_reset();
    mkdir(VJO_AFV_DIR, 0777);
    mkdir(VJO_AFV_DIR "/index", 0777);
    unlink(VJO_ANKI_INDEX_PATH);
    unlink(VJO_ANKI_INDEX_OLD_PATH);
    index_opens = index_failed_opens = index_reads = index_closes = 0;
    index_read_bytes = 0;
    /* the Shell starts the mailbox before its first config load */
    TEST_ASSERT(vjo_anki_start() == 0);
    fixture();
    TEST_ASSERT(vjo_anki_configure(&cfg) == 1);
    vjo_anki_set_game("PCSG00415", "CLANNAD", 0);
    vjo_anki_set_ocr_model("VOCR3_H15_w8.vocr");
}

static void reconfigure(void)
{
    TEST_ASSERT(vjo_anki_configure(&cfg) == 1);
}

/* Ends the test's mailbox; every job's memory and thread are gone, and no
 * socket was ever asked for. */
static void finish(void)
{
    vjo_anki_reap();
    TEST_CHECK(vjo_anki_stop() == 0);
    TEST_CHECK(!heap_live && heap_allocs == heap_frees);
    TEST_CHECK(threads_created == threads_deleted);
    TEST_CHECK(!held_box && !held_view);
    TEST_CHECK_(jpg_opens == 0, "SceShell opened a card's JPEG %d time(s): it only names it (AGENTS.md rule 1)", jpg_opens);
}

/* Enter on entry of the shown list: the job runs to its end. */
static void enter(int entry)
{
    vjo_anki_post_add(g_view.list_seq, entry);
    vjo_anki_reap();
}

/* A card in the exported archive, as the Rubi app's export leaves it. */
static void export_card(const char *id)
{
    char from[256], to[256];
    mkdir(VJO_ANKI_EXPORTED_DIR, 0777);
    snprintf(from, sizeof(from), VJO_ANKI_QUEUE_DIR "/%s.json", id);
    snprintf(to, sizeof(to), VJO_ANKI_EXPORTED_DIR "/%s.json", id);
    TEST_ASSERT(rename(from, to) == 0);
}

/* ---------------- tests ---------------- */

static void test_off_by_default_allocates_nothing(void)
{
    VjoConfig d;
    struct stat st;
    reset();
    vjo_config_defaults(&d);
    TEST_CHECK(!d.anki_mining);
    TEST_CHECK(vjo_anki_configure(&d) == 0);
    rmdir(VJO_ANKI_QUEUE_DIR);
    vjo_anki_post_count();
    vjo_anki_post_add(5, 0);
    TEST_CHECK(threads_created == 0 && heap_attempts == 0 && dir_scans == 0);
    TEST_CHECK(stat(VJO_ANKI_QUEUE_DIR, &st) < 0); /* not even the queue's directory */
    TEST_CHECK(!g_view.anki_status[0] && g_view.anki_version == 0);
    finish();
}

/* Enter with no computer and no network: the card is on the memory card. */
static void test_enter_saves_offline(void)
{
    char id[VJO_QUEUE_ID_SIZE];
    size_t len;
    const char *json;
    reset();
    vjo_anki_post_add(5, 0);
    TEST_CHECK(threads_created == 1 && threads_started == 1);
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && g_view.anki_status_seq == 5 &&
               g_view.anki_status_kind == VJO_ANKI_STATUS_DIM);
    TEST_MSG("%s", g_view.anki_status);
    /* queued, on every occurrence of the word */
    TEST_CHECK(g_view.anki_pending == 1 && g_view.anki_marks_seq == 5 && g_view.anki_mark[0] == VJO_ANKI_MARK_QUEUED &&
               !g_view.anki_mark[1] && g_view.anki_mark[2] == VJO_ANKI_MARK_QUEUED);
    /* one record, no screenshot (this build takes none) and no temp file */
    TEST_CHECK(files_ending(".json") == 1 && files_ending(".jpg") == 0 && files_ending(".tmp") == 0);
    /* the text-only arena, which is the ledger's figure, freed after the job */
    TEST_CHECK(heap_allocs == 1 && heap_frees == 1 && !heap_live && heap_last == ANKI_TEXT_MEM);
    TEST_CHECK(ANKI_STACK == VJO_STACK_SHELL_ANKI && VJO_STACK_SHELL_ANKI == 0x4000 && ANKI_TEXT_MEM == VJO_BUDGET_PAF_ANKI);
    TEST_CHECK(vjo_ledger_peak(&g_shell_ledger, VJO_SL_PAF_ANKI) == VJO_BUDGET_PAF_ANKI);
    TEST_CHECK(vjo_ledger_current(&g_shell_ledger, VJO_SL_PAF_ANKI) == 0);
    TEST_CHECK(vjo_ledger_peak(&g_shell_ledger, VJO_SL_ANKI_STACK) == VJO_BUDGET_SHELL_ANKI_STACK);
    vjo_anki_reap();
    TEST_CHECK_(vjo_ledger_current(&g_shell_ledger, VJO_SL_ANKI_STACK) == 0, "%u",
                vjo_ledger_current(&g_shell_ledger, VJO_SL_ANKI_STACK));
    printf("measure: offline save: arena %u bytes, peak %u\n", (unsigned)heap_last, (unsigned)last_peak);
    TEST_CHECK(last_peak > 0 && last_peak < ANKI_TEXT_MEM / 4);
    TEST_CHECK(g_view.list == &list && !strcmp(list.header, "猫が好き猫")); /* recognized text untouched */
    /* readable UTF-8 JSON with every value an export makes into a note:
     * version 3, the furigana, the dictionary, the other dictionaries'
     * entries, the game, its title ID and platform, the OCR model and the time */
    only_id(id);
    json = queue_file(id, "json", &len);
    TEST_ASSERT(json != NULL);
    /* version 4 (3's keys, rank 0 for none) */
    TEST_CHECK(!strcmp(json, "{\"version\":4,\"spelling\":\"猫\",\"reading\":\"ねこ\",\"sentence\":\"猫が好き猫\","
                             "\"rank\":1500,\"hl_start\":0,\"hl_end\":3,\"picture\":false,"
                             "\"meanings\":[\"cat\",\"shamisen \\\"neko\\\"\"],\"sentence_furigana\":\"猫が好き猫\","
                             "\"sf_hl_start\":0,\"sf_hl_end\":3,\"source\":null,\"pitch\":[],\"pos\":[],"
                             "\"misc\":\"CLANNAD (2026-10-12 09:30)\",\"furigana\":null,\"dictionary\":null,"
                             "\"more\":[],\"game\":\"CLANNAD\",\"title_id\":\"PCSG00415\",\"platform\":\"vita\","
                             "\"ocr_engine\":\"vocr\",\"ocr_model\":\"VOCR3_H15_w8.vocr\",\"time\":1791158400,"
                             "\"ocr_confidence\":null}\n"));
    TEST_MSG("%s", json);
    /* every step logged: the record's bytes, the queue (the debug build's lines); the save
     * itself in both builds (docs/release-variants.md) */
#if VJO_DEBUG
    TEST_CHECK(logged("anki: save 猫 rc=0, "));
    TEST_CHECK(logged(" saved, ") && logged(" B record\n") && logged("anki: queue 1 card(s)\n") &&
               logged("anki: arena peak "));
#else
    TEST_CHECK(logged("anki: save rc=0, ") && !logged("猫")); /* the normal build logs no looked-up word */
    TEST_CHECK(!logged(" B record\n") && !logged("anki: queue ") && !logged("anki: arena peak "));
#endif
    TEST_MSG("%s", all_logs);
    /* enter again: the mark says so, without memory or I/O */
    vjo_anki_post_add(5, 2);
    TEST_CHECK(!strcmp(g_view.anki_status, "Already queued") && heap_attempts == 1);
    vjo_anki_reap();
    /* the same card from a new lookup of the same text: the storage knows it */
    g_view.list_seq = 6;
    enter(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Already queued") && files_ending(".json") == 1 &&
               g_view.anki_mark[0] == VJO_ANKI_MARK_QUEUED && g_view.anki_marks_seq == 6);
    TEST_MSG("%s", g_view.anki_status);
    /* the queue survives the plugin's restart (every byte of memory gone) */
    finish();
    memset(&g_view, 0, sizeof(g_view));
    memset(heap, 0x5A, sizeof(heap));
    TEST_ASSERT(vjo_anki_start() == 0);
    fixture();
    reconfigure();
    vjo_anki_post_count();
    vjo_anki_reap();
    TEST_CHECK(g_view.anki_pending == 1 && heap_attempts == 2); /* a count allocates nothing */
    finish();
}

/* A card the Rubi app has exported is not queued again: "Already
 * exported", and the √ on the word; so is one AnkiConnect had (an earlier
 * build's .duplicate.json). */
static void test_a_card_exported_before_says_so(void)
{
    char id[VJO_QUEUE_ID_SIZE], from[256], to[256];
    reset();
    enter(0);
    only_id(id);
    export_card(id);
    vjo_anki_post_count();
    vjo_anki_reap();
    TEST_CHECK(g_view.anki_pending == 0);
    g_view.list_seq = 6;
    enter(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Already exported (in an .apkg)") &&
               g_view.anki_mark[0] == VJO_ANKI_MARK_ADDED && g_view.anki_mark[2] == VJO_ANKI_MARK_ADDED &&
               files_ending(".json") == 0 && g_view.anki_pending == 0);
    TEST_MSG("%s", g_view.anki_status);
    /* the √ already says it: no memory for that */
    vjo_anki_post_add(6, 2);
    TEST_CHECK(!strcmp(g_view.anki_status, "Already exported (in an .apkg)") && heap_attempts == 2);
    vjo_anki_reap();
    /* an earlier build's duplicate */
    snprintf(from, sizeof(from), VJO_ANKI_EXPORTED_DIR "/%s.json", id);
    snprintf(to, sizeof(to), VJO_ANKI_QUEUE_DIR "/%s.duplicate.json", id);
    TEST_ASSERT(rename(from, to) == 0);
    g_view.list_seq = 7;
    enter(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Already exported (in an .apkg)") && files_ending(".duplicate.json") == 1 &&
               g_view.anki_pending == 0);
    /* another word is saved as usual */
    enter(1);
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && g_view.anki_pending == 1);
    finish();
}

/* A PSP game's card says so, whatever its ID: the Shell says it is one
 * (shell/game.c: Adrenaline's NPXS10028, a bubble by its files, a disc ID),
 * and the card records the game's disc ID when the Rubi app found it. */
static void test_a_psp_games_card_says_psp(void)
{
    static const struct { const char *id; int psp; } games[] = {
        {"NPXS10028", 1}, {"PSPEMUCFW", 1}, {"ULJS00378", 1}, {"ABCD12345", 1}, {"PCSG00415", 0}};
    for (unsigned i = 0; i < sizeof(games) / sizeof(games[0]); i++) {
        char id[VJO_QUEUE_ID_SIZE], want[64];
        size_t len;
        const char *json;
        reset();
        vjo_anki_set_game(games[i].id, "planetarian", games[i].psp);
        enter(0);
        TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki"));
        only_id(id);
        json = queue_file(id, "json", &len);
        TEST_ASSERT(json != NULL);
        snprintf(want, sizeof(want), "\"title_id\":\"%s\",\"platform\":\"%s\"", games[i].id,
                 games[i].psp ? "psp" : "vita");
        TEST_CHECK_(strstr(json, want) != NULL, "%s: %s", games[i].id, json);
        finish();
    }
}

/* Japanese, supplementary characters, quotes, backslashes, tags and line
 * breaks survive the record, and are escaped for Anki in the note's fields. */
static void test_japanese_and_escaping_round_trip(void)
{
    static const char *meanings[] = {"cat \"quoted\" & <tag>\nnext\tline", "back\\slash", "日本語𠮷"};
    static VjoVocab v = {.spelling = "𠮷野家", .reading = "よしのや", .rank = 1234, .meanings = meanings,
                         .n_meanings = 3};
    static uint8_t mem[64 * 1024];
    char id[VJO_QUEUE_ID_SIZE];
    VjoArena a;
    VjoAnkiNote n;
    int picture;
    size_t len;
    char *json;
    reset();
    entries[0] = (VjoEntry){.vocab = &v, .pos16 = 0, .hl_start = 0, .hl_end = 10, .text = "𠮷野家"};
    list = (VjoEntryList){.header = "𠮷野家で\r\n\"牛丼\" & <b>\\", .entries = entries, .n_entries = 1};
    enter(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && heap_last == ANKI_TEXT_MEM);
    only_id(id);
    json = queue_file(id, "json", &len);
    TEST_ASSERT(json != NULL);
    vjo_arena_init(&a, mem, sizeof(mem));
    TEST_ASSERT(vjo_queue_decode(&a, json, len, &n, &picture) == 4);
    TEST_CHECK(!strcmp(n.spelling, "𠮷野家") && !strcmp(n.reading, "よしのや") && n.rank == 1234 &&
               n.n_meanings == 3 && !strcmp(n.meanings[0], meanings[0]) && !strcmp(n.meanings[1], "back\\slash") &&
               !strcmp(n.meanings[2], "日本語𠮷") && !strcmp(n.sentence, "𠮷野家で\r\n\"牛丼\" & <b>\\") && !picture &&
               n.unix_time == 1791158400);
    TEST_CHECK(!strcmp(vjo_anki_field_value(&a, VJO_LAPIS_SENTENCE, &n),
                       "<b>𠮷野家</b>で<br>&quot;牛丼&quot; &amp; &lt;b&gt;\\"));
    TEST_MSG("%s", vjo_anki_field_value(&a, VJO_LAPIS_SENTENCE, &n));
    TEST_CHECK(strstr(vjo_anki_field_value(&a, VJO_LAPIS_MAIN_DEFINITION, &n),
                      "<li>cat &quot;quoted&quot; &amp; &lt;tag&gt;<br>next\tline</li><li>back\\slash</li>"
                      "<li>日本語𠮷</li>") != NULL);
    finish();
}

/* The record holds the minute of the enter press: the same card saved again
 * within it (the marks gone: a new lookup of the same text) is the same
 * record; in the next minute it is another one. */
static void test_same_card_within_a_minute_is_one_record(void)
{
    reset();
    rtc_unix_us = 1791158405000000ull; /* 0:05 */
    enter(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && files_ending(".json") == 1);
    rtc_unix_us = 1791158459999000ull; /* 0:59 */
    g_view.list_seq = 6;
    enter(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Already queued") && files_ending(".json") == 1);
    TEST_MSG("%s", g_view.anki_status);
    rtc_unix_us = 1791158460000000ull; /* 1:00 */
    g_view.list_seq = 7;
    enter(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && files_ending(".json") == 2);
    finish();
}

static void test_failed_writes_never_queue_a_partial_card(void)
{
    static const char *const junk[] = {"0123456789abcdef0123456789abcdef.json.tmp",
                                       "0123456789abcdef0123456789abcdef.jpg",
                                       "0123456789ABCDEF0123456789ABCDEF.json", "notes.txt", "short.json"};
    char path[256];
    FILE *f;
    reset();
    for (int mode = 0; mode < 4; mode++) {
        write_budget = mode == 0 ? 11 : -1; /* the record's write fails part way */
        fail_rename = mode == 1;
        fail_sync = mode == 2;
        fail_close = mode == 3;
        g_view.list_seq = 10 + (unsigned)mode;
        last_queue_log[0] = '\0';
        enter(0);
        TEST_CHECK(!strcmp(g_view.anki_status, "Card not saved: cannot write ux0:data/VitaJPOverlay/anki_queue (space?)") &&
                   g_view.anki_status_kind == VJO_ANKI_STATUS_ERROR);
        TEST_CHECK(strstr(last_queue_log, " failed 0x") != NULL); /* log.txt names the call and its code */
        TEST_MSG("mode %d: %s / %s", mode, g_view.anki_status, last_queue_log);
        TEST_CHECK(vjo_queue_count() == 0 && files_ending(".tmp") == 0 && !g_view.anki_mark[0]);
        write_budget = -1;
        fail_rename = fail_sync = fail_close = 0;
    }
    /* short reads and writes (7 bytes a call) still commit whole files */
    short_io = 1;
    g_view.list_seq = 20;
    enter(0);
    short_io = 0;
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && vjo_queue_count() == 1);
    /* leftovers of interrupted saves and other files are never cards */
    for (unsigned i = 0; i < sizeof(junk) / sizeof(*junk); i++) {
        snprintf(path, sizeof(path), VJO_ANKI_QUEUE_DIR "/%s", junk[i]);
        TEST_ASSERT((f = fopen(path, "wb")) != NULL);
        fputs("{\"version\":2}", f);
        fclose(f);
    }
    TEST_CHECK(vjo_queue_count() == 1);
    /* an unreadable queue directory */
    fail_scan = 1;
    vjo_anki_post_count();
    vjo_anki_reap();
    TEST_CHECK(g_view.anki_pending == -1);
    fail_scan = 0;
    finish();
}

/* A record renamed into place whose directory flush failed is not yet a
 * saved card: a retry flushes again before reporting it; and an exported
 * card's move is flushed before it is reported. */
static void test_failed_directory_flush_is_retried(void)
{
    static const char *meanings[] = {"cat"};
    static uint8_t mem[64 * 1024];
    VjoAnkiNote n = {.spelling = "猫", .reading = "ねこ", .rank = 1, .meanings = meanings, .n_meanings = 1,
                     .sentence = "猫が", .hl_start = 0, .hl_end = 3, .sf_hl_start = -1, .sf_hl_end = -1};
    char id[VJO_QUEUE_ID_SIZE];
    VjoArena a;
    int barriers;
    reset();
    vjo_arena_init(&a, mem, sizeof(mem));
    fail_mount_sync = 1;
    TEST_CHECK(vjo_queue_save(&a, &n, NULL) == VJO_QUEUE_FAILED);
    TEST_CHECK(vjo_queue_count() == 1); /* visible, but never acknowledged */
    barriers = mount_sync_calls;
    vjo_arena_reset(&a);
    TEST_CHECK(vjo_queue_save(&a, &n, NULL) == VJO_QUEUE_FAILED && mount_sync_calls == barriers + 1);
    fail_mount_sync = 0;
    vjo_arena_reset(&a);
    TEST_CHECK(vjo_queue_save(&a, &n, NULL) == VJO_QUEUE_EXISTS && mount_sync_calls == barriers + 2);
    only_id(id);
    export_card(id);
    fail_mount_sync = 1;
    vjo_arena_reset(&a);
    TEST_CHECK(vjo_queue_save(&a, &n, NULL) == VJO_QUEUE_FAILED);
    fail_mount_sync = 0;
    vjo_arena_reset(&a);
    TEST_CHECK(vjo_queue_save(&a, &n, NULL) == VJO_QUEUE_ARCHIVED && vjo_queue_count() == 0);
    finish();
}

static void test_oom_keeps_recognized_text_and_cards(void)
{
    static char big[48 * 1024], huge[136 * 1024];
    static const char *big_meanings[] = {big}, *huge_meanings[] = {huge};
    static VjoVocab long_word = {.spelling = "長文", .reading = "ちょうぶん", .rank = VJO_NO_RANK,
                                 .meanings = big_meanings, .n_meanings = 1};
    static VjoVocab huge_word = {.spelling = "巨大", .reading = "きょだい", .rank = VJO_NO_RANK,
                                 .meanings = huge_meanings, .n_meanings = 1};
    memset(big, 'x', sizeof(big) - 1);
    memset(huge, 'y', sizeof(huge) - 1);

    /* the Paf heap keeps its UI reserve: nothing is written */
    reset();
    fail_heap = 1;
    snprintf(g_view.status, sizeof(g_view.status), "kept");
    vjo_anki_post_add(5, 0);
    TEST_CHECK(heap_attempts == 1 && heap_allocs == 0 && vjo_queue_count() == 0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Anki: not enough memory, card not saved") &&
               g_view.anki_status_kind == VJO_ANKI_STATUS_ERROR);
    TEST_CHECK(g_view.list == &list && !strcmp(g_view.status, "kept") && !g_view.anki_mark[0] && job == JOB_NONE);
    fail_heap = 0;
    vjo_anki_reap();
    enter(0); /* and a retry works */
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && vjo_queue_count() == 1);
    finish();

    /* no thread for the job */
    reset();
    fail_create = 1;
    vjo_anki_post_add(5, 0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Anki: not enough memory, card not saved"));
    TEST_CHECK(heap_attempts == 0 && job == JOB_NONE && thread < 0);
    fail_create = 0;
    fail_start = 1;
    vjo_anki_post_add(5, 0);
    TEST_CHECK(threads_created == 1 && threads_deleted == 1 && job == JOB_NONE && thread < 0 && heap_attempts == 0);
    fail_start = 0;
    finish();

    /* an entry over a record's 32 KiB of text */
    reset();
    entries[0].vocab = &long_word;
    enter(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Card not saved: over 32 KiB of text") && vjo_queue_count() == 0 &&
               files_ending(".tmp") == 0 && !heap_live && g_view.list == &list);
    TEST_MSG("%s", g_view.anki_status);
    finish();

    /* an entry larger than the arena */
    reset();
    entries[0].vocab = &huge_word;
    enter(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Anki: not enough memory for this entry; card not saved") &&
               vjo_queue_count() == 0 && heap_last == ANKI_TEXT_MEM && !heap_live);
    TEST_MSG("%s", g_view.anki_status);
    finish();
}

static void test_cancellation(void)
{
    /* the list changed before the save ran: nothing is copied or written */
    reset();
    defer_start = 1;
    vjo_anki_post_add(5, 0);
    TEST_CHECK(threads_started == 1 && job == JOB_SAVE);
    vjo_anki_post_add(5, 1); /* a second enter meanwhile is refused, and says so */
    TEST_CHECK(threads_created == 1 && !strcmp(g_view.anki_status, "Anki: still saving the previous card"));
    g_view.list_seq = 6; /* another word shown */
    thread_entry(0, NULL);
    TEST_CHECK(vjo_queue_count() == 0 && heap_attempts == 0 && job == JOB_NONE);
    finish();

    /* the overlay closed before the save ran */
    reset();
    defer_start = 1;
    vjo_anki_post_add(5, 0);
    g_view.open = 0;
    thread_entry(0, NULL);
    TEST_CHECK(vjo_queue_count() == 0 && heap_attempts == 0 && !heap_live);
    finish();

    /* enter during a count saves right after it, on the same thread */
    reset();
    defer_start = 1;
    vjo_anki_post_count(); /* the overlay opens */
    vjo_anki_post_add(5, 0); /* and enter at once */
    TEST_CHECK(threads_created == 1 && job == JOB_COUNT && next_job == JOB_SAVE);
    thread_entry(0, NULL);
    TEST_CHECK(vjo_queue_count() == 1 && !strcmp(g_view.anki_status, "Saved for Anki"));
    finish();

    /* stop with a save posted: it does not run, and nothing is left */
    reset();
    defer_start = 1;
    vjo_anki_post_add(5, 0);
    TEST_CHECK(vjo_anki_stop() == 0 && threads_joined == 1 && threads_deleted == 1 && box_lock < 0);
    thread_entry(0, NULL); /* (the thread ends without its job) */
    TEST_CHECK(vjo_queue_count() == 0 && !heap_live);
    vjo_anki_post_add(5, 0); /* after stop: ignored */
    TEST_CHECK(threads_created == 1);

    /* a join that fails keeps the mailbox for a retry */
    reset();
    defer_start = 1;
    vjo_anki_post_add(5, 0);
    fail_wait = 1;
    TEST_CHECK(vjo_anki_stop() < 0 && box_lock >= 0 && threads_deleted == 0);
    fail_wait = 0;
    thread_entry(0, NULL); /* the job sees the stop and ends */
    TEST_CHECK(vjo_queue_count() == 0 && !heap_live);
    TEST_CHECK(vjo_anki_stop() == 0 && box_lock < 0 && threads_deleted == 1);
}

/* With Dictionaries shown above 1, enter records the other dictionaries'
 * entries for the word (their pages read from their files), which the
 * export makes into the note's Glossary. */
static void test_enter_records_every_dictionary(void)
{
    static const DictRow rows[3] = {
        {"好き", "好き", "すき", "[Daijirin4 Plain]\nliked; favourite", NULL},
        {"好き", "好き", "すき", "[JMdict Plain]\nliked", NULL},
        {"好き", "好き", "すき", "[Sankoku8 Plain]\nfond of", NULL},
    };
    static uint8_t mem[96 * 1024], note_mem[64 * 1024];
    static VjoLocalPages pages;
    VjoArena a, na;
    VjoPlatform p;
    VjoDictResult res;
    VjoErr err;
    VjoAnkiNote n;
    char id[VJO_QUEUE_ID_SIZE];
    const char *json;
    size_t len;
    int picture;
    reset();
    dict_build(&dict_files[0], "main.vjdict", &rows[0]);
    dict_build(&dict_files[1], "b.vjdict", &rows[1]);
    dict_build(&dict_files[2], "c.vjdict", &rows[2]);
    snprintf(cfg.local_dictionary_dir, sizeof(cfg.local_dictionary_dir), "fixture");
    snprintf(cfg.local_dictionaries, sizeof(cfg.local_dictionaries), "main.vjdict,b.vjdict,c.vjdict");
    cfg.local_dictionary_results = 3;
    reconfigure();
    /* the shown result: worker.c's lookup at 好き, its first page shown */
    vjo_arena_init(&a, mem, sizeof(mem));
    vjo_platform_vita(&p);
    TEST_ASSERT(vjo_local_lookup_pages(&a, &p, &cfg, list.header, 6, 3, &pages, &res, &err) == VJO_OK);
    TEST_ASSERT(pages.count == 3 && res.n_vocab == 1);
    entries[1].vocab = &res.vocab[0];
    view_pages = &pages;
    view_page = 0;
    dict_opens = 0;
    enter(1);
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && dict_opens == 2);
    TEST_CHECK(!dict_files[0].active && !dict_files[1].active && !dict_files[2].active);
    printf("measure: save with 2 other dictionaries' entries: arena %u bytes, peak %u\n", (unsigned)heap_last,
           (unsigned)last_peak);
    TEST_CHECK(heap_last == ANKI_TEXT_MEM && last_peak < ANKI_TEXT_MEM / 2);
    only_id(id);
    json = queue_file(id, "json", &len);
    TEST_ASSERT(json != NULL);
    TEST_CHECK(strstr(json, "\"meanings\":[\"[Daijirin4 Plain]\\nliked; favourite\"]") &&
               strstr(json, "\"dictionary\":\"Daijirin4 Plain\",\"more\":[{\"dictionary\":\"JMdict Plain\","
                            "\"spelling\":\"好き\",\"reading\":\"すき\",\"meanings\":[\"[JMdict Plain]\\nliked\"]},"
                            "{\"dictionary\":\"Sankoku8 Plain\",\"spelling\":\"好き\",\"reading\":\"すき\","
                            "\"meanings\":[\"[Sankoku8 Plain]\\nfond of\"]}],"));
    TEST_MSG("%s", json);
    /* the note an export makes of it: every dictionary in its Glossary (the
     * lookup's result stays in mem for the second save below) */
    vjo_arena_init(&na, note_mem, sizeof(note_mem));
    TEST_ASSERT(vjo_queue_decode(&na, json, len, &n, &picture) == 4);
    TEST_CHECK(!strcmp(vjo_anki_field_value(&na, VJO_LAPIS_GLOSSARY, &n),
                       "<div style=\"text-align: left;\" class=\"yomitan-glossary\"><ol>"
                       "<li data-dictionary=\"Daijirin4 Plain\"><i>(Daijirin4 Plain)</i> <span>liked; favourite"
                       "</span></li><li data-dictionary=\"JMdict Plain\"><i>(JMdict Plain)</i> <span>liked</span>"
                       "</li><li data-dictionary=\"Sankoku8 Plain\"><i>(Sankoku8 Plain)</i> <span>fond of</span>"
                       "</li></ol></div>"));
    TEST_MSG("%s", vjo_anki_field_value(&na, VJO_LAPIS_GLOSSARY, &n));
    /* the page shown is the main definition; the others are not read twice */
    finish();
    reset();
    snprintf(cfg.local_dictionary_dir, sizeof(cfg.local_dictionary_dir), "fixture");
    snprintf(cfg.local_dictionaries, sizeof(cfg.local_dictionaries), "main.vjdict,b.vjdict,c.vjdict");
    reconfigure();
    entries[1].vocab = &res.vocab[0];
    view_pages = &pages;
    view_page = 2;
    dict_opens = 0;
    enter(1);
    TEST_CHECK(dict_opens == 2);
    only_id(id);
    json = queue_file(id, "json", NULL);
    TEST_CHECK(json && strstr(json, "\"more\":[{\"dictionary\":\"Daijirin4 Plain\"") &&
               strstr(json, "{\"dictionary\":\"JMdict Plain\"") && !strstr(json, "{\"dictionary\":\"Sankoku8 Plain\""));
    TEST_MSG("%s", json ? json : "");
    finish();
}

/* Files with a title (Bee's Ultimate Dictionary's page 0) name their entries
 * on the card by it: the shown entry's dictionary and each other page's, read
 * from its own file; the definitions carry no label. A file without one still
 * names its entries by their "[name]" first line, and one with neither by
 * none. The export's Glossary pages by those names. */
static void test_enter_names_titled_dictionaries(void)
{
    static const DictRow rows[3] = {
        {"好き", "好き", "すき", "liked; favourite\n好=fond, pleasing, like", "Bee's Ultimate Dictionary"},
        {"好き", "好き", "すき", "[JMdict Plain]\nliked", NULL},
        {"好き", "好き", "すき", "fond of", "Other <&> Title"},
    };
    static uint8_t mem[96 * 1024], note_mem[64 * 1024];
    static VjoLocalPages pages;
    VjoArena a, na;
    VjoPlatform p;
    VjoDictResult res;
    VjoErr err;
    VjoAnkiNote n;
    char id[VJO_QUEUE_ID_SIZE];
    const char *json;
    size_t len;
    int picture;
    reset();
    dict_build(&dict_files[0], "main.vjdict", &rows[0]);
    dict_build(&dict_files[1], "b.vjdict", &rows[1]);
    dict_build(&dict_files[2], "c.vjdict", &rows[2]);
    snprintf(cfg.local_dictionary_dir, sizeof(cfg.local_dictionary_dir), "fixture");
    snprintf(cfg.local_dictionaries, sizeof(cfg.local_dictionaries), "main.vjdict,b.vjdict,c.vjdict");
    cfg.local_dictionary_results = 3;
    reconfigure();
    vjo_arena_init(&a, mem, sizeof(mem));
    vjo_platform_vita(&p);
    TEST_ASSERT(vjo_local_lookup_pages(&a, &p, &cfg, list.header, 6, 3, &pages, &res, &err) == VJO_OK);
    TEST_ASSERT(pages.count == 3 && res.n_vocab == 1);
    TEST_CHECK(res.vocab[0].dictionary && !strcmp(res.vocab[0].dictionary, "Bee's Ultimate Dictionary"));
    entries[1].vocab = &res.vocab[0];
    view_pages = &pages;
    view_page = 0;
    dict_opens = 0;
    enter(1);
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && dict_opens == 2);
    only_id(id);
    json = queue_file(id, "json", &len);
    TEST_ASSERT(json != NULL);
    TEST_CHECK(strstr(json, "\"meanings\":[\"liked; favourite\\n好=fond, pleasing, like\"]") &&
               strstr(json, "\"dictionary\":\"Bee's Ultimate Dictionary\",\"more\":[{\"dictionary\":\"JMdict Plain\",") &&
               strstr(json, "{\"dictionary\":\"Other <&> Title\",\"spelling\":\"好き\",\"reading\":\"すき\","
                            "\"meanings\":[\"fond of\"]}]"));
    TEST_MSG("%s", json);
    vjo_arena_init(&na, note_mem, sizeof(note_mem));
    TEST_ASSERT(vjo_queue_decode(&na, json, len, &n, &picture) == 4);
    TEST_CHECK(!strcmp(vjo_anki_field_value(&na, VJO_LAPIS_GLOSSARY, &n),
                       "<div style=\"text-align: left;\" class=\"yomitan-glossary\"><ol>"
                       "<li data-dictionary=\"Bee's Ultimate Dictionary\"><i>(Bee's Ultimate Dictionary)</i> <span>"
                       "liked; favourite<br>好=fond, pleasing, like</span></li>"
                       "<li data-dictionary=\"JMdict Plain\"><i>(JMdict Plain)</i> <span>liked</span></li>"
                       "<li data-dictionary=\"Other &lt;&amp;&gt; Title\"><i>(Other &lt;&amp;&gt; Title)</i> <span>"
                       "fond of</span></li></ol></div>"));
    TEST_MSG("%s", vjo_anki_field_value(&na, VJO_LAPIS_GLOSSARY, &n));
    finish();
    /* the shown page from the untitled, unlabelled file: no name; the titled others keep theirs */
    {
        static const DictRow plain = {"好き", "好き", "すき", "liked", NULL};
        reset();
        dict_build(&dict_files[0], "main.vjdict", &plain);
        dict_build(&dict_files[1], "b.vjdict", &rows[0]);
        snprintf(cfg.local_dictionary_dir, sizeof(cfg.local_dictionary_dir), "fixture");
        snprintf(cfg.local_dictionaries, sizeof(cfg.local_dictionaries), "main.vjdict,b.vjdict");
        cfg.local_dictionary_results = 2;
        reconfigure();
        vjo_arena_init(&a, mem, sizeof(mem));
        TEST_ASSERT(vjo_local_lookup_pages(&a, &p, &cfg, list.header, 6, 2, &pages, &res, &err) == VJO_OK);
        TEST_ASSERT(pages.count == 2 && !res.vocab[0].dictionary);
        entries[1].vocab = &res.vocab[0];
        view_pages = &pages;
        view_page = 0;
        enter(1);
        only_id(id);
        json = queue_file(id, "json", &len);
        TEST_CHECK(json && !strstr(json, "\"dictionary\":\"\"") &&
                   strstr(json, "\"more\":[{\"dictionary\":\"Bee's Ultimate Dictionary\""));
        TEST_MSG("%s", json ? json : "");
        finish();
    }
}

/* Whatever the dictionaries: with 8 dictionaries' entries of any size (and
 * text that HTML and JSON escaping grow), enter keeps the other
 * dictionaries' entries while the card's record fits (32 KiB), so every card
 * is saved; small ones are all kept. */
static void test_what_enter_saves_whatever_the_dictionaries(void)
{
    static const int sizes[] = {300, 900, 2700, 4000, 7000};
    static char meaning[8][7200], names[8][16], list_names[160];
    static DictRow rows[8];
    static uint8_t mem[256 * 1024];
    static VjoLocalPages pages;
    for (int heavy = 0; heavy < 2; heavy++) {
        /* each case twice: text only, then with a screenshot (API 23: its request and result in the arena) */
        for (unsigned zz = 0; zz < 2 * sizeof(sizes) / sizeof(sizes[0]); zz++) {
            unsigned z = zz / 2;
            int with = (int)(zz % 2);
            VjoArena a;
            VjoPlatform p;
            VjoDictResult res;
            VjoErr err;
            char id[VJO_QUEUE_ID_SIZE];
            const char *json;
            int kept = 0;
            reset();
            list_names[0] = '\0';
            for (int k = 0; k < 8; k++) {
                int o = snprintf(meaning[k], sizeof(meaning[k]), "[Dictionary %d]\n", k);
                while (o + 3 < sizes[z]) { /* kana, or text escaping grows: & < > " and line breaks */
                    const char *piece = heavy ? (o % 4 ? "\"&\"" : "<\n>") : "す";
                    memcpy(meaning[k] + o, piece, 3);
                    o += 3;
                }
                meaning[k][o] = '\0';
                snprintf(names[k], sizeof(names[k]), "d%d.vjdict", k);
                rows[k] = (DictRow){"好き", "好き", "すき", meaning[k], NULL};
                dict_build(&dict_files[k], names[k], &rows[k]);
                strcat(list_names, k ? "," : "");
                strcat(list_names, names[k]);
            }
            snprintf(cfg.local_dictionary_dir, sizeof(cfg.local_dictionary_dir), "fixture");
            snprintf(cfg.local_dictionaries, sizeof(cfg.local_dictionaries), "%s", list_names);
            cfg.local_dictionary_results = 8;
#ifdef VJO_WITH_VOCR
            cfg.anki_screenshot = with;
#else
            if (with) continue;
#endif
            reconfigure();
            vjo_arena_init(&a, mem, sizeof(mem));
            vjo_platform_vita(&p);
            TEST_ASSERT(vjo_local_lookup_pages(&a, &p, &cfg, list.header, 6, 8, &pages, &res, &err) == VJO_OK);
            TEST_ASSERT(pages.count == 8 && res.n_vocab == 1);
            entries[1].vocab = &res.vocab[0];
            view_pages = &pages;
            view_page = 0;
            enter(1);
            TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki"));
            TEST_MSG("%s", g_view.anki_status);
            only_id(id);
            json = queue_file(id, "json", NULL);
            TEST_ASSERT(json != NULL);
            for (const char *q = json; (q = strstr(q, "{\"dictionary\":")); q++)
                kept++;
            TEST_CHECK(strlen(json) <= VJO_QUEUE_TEXT_MAX && last_peak <= ANKI_TEXT_MEM);
            TEST_CHECK(files_ending(".jpg") == with && !strstr(json, with ? "\"picture\":false" : "\"picture\":true"));
            printf("measure: 8 dictionaries, entries of %d B (%s)%s: record %u B with %d of 7 others, peak %u\n",
                   sizes[z], heavy ? "escaping" : "kana", with ? ", with a screenshot" : "", (unsigned)strlen(json),
                   kept, (unsigned)last_peak);
            /* every other dictionary's entry fits a record while they are small: up to
             * 2,700 B of kana, 900 B of text that JSON escaping grows */
            if (sizes[z] <= (heavy ? 900 : 2700))
                TEST_CHECK(kept == 7 && !logged("would make the card too large"));
            if (kept < 7)
                TEST_CHECK(logged("other dictionaries' entries kept: the next would make the card too large"));
            TEST_MSG("entries of %d B: %d kept", sizes[z], kept);
            finish();
        }
    }
}

/* The record format on its own: every value round-trips, the same note
 * gives the same bytes (its ID), and anything but an exact record fails. */
static void test_record_format(void)
{
    static const char *meanings[] = {"to come", "to arrive"};
    static const char *pos[] = {"vk", "vi"};
    static uint8_t mem[64 * 1024];
    static char oversized[VJO_QUEUE_TEXT_MAX + 16];
    static const char *bad[] = {
        "{}", "null", "[]", "{\"version\":3}", "{\"version\":2,\"spelling\":\"猫\"}",
        "{\"version\":2,\"spelling\":\"\",\"reading\":\"\",\"sentence\":\"\",\"rank\":1,\"hl_start\":0,\"hl_end\":0,"
        "\"picture\":false,\"meanings\":[]}",
        "{\"version\":2,\"spelling\":\"猫\",\"reading\":\"\",\"sentence\":\"\",\"rank\":1.5,\"hl_start\":0,\"hl_end\":0,"
        "\"picture\":false,\"meanings\":[]}",
        "{\"version\":2,\"spelling\":\"猫\\u0000\",\"reading\":\"\",\"sentence\":\"\",\"rank\":1,\"hl_start\":0,"
        "\"hl_end\":0,\"picture\":false,\"meanings\":[]}",
        "{\"version\":2,\"spelling\":\"猫\",\"reading\":\"\",\"sentence\":\"\",\"rank\":1,\"hl_start\":0,\"hl_end\":0,"
        "\"picture\":false,\"meanings\":[],\"pitch\":[1,2,3,4,5]}",
        "{\"version\":2,\"spelling\":\"猫\",\"reading\":\"\",\"sentence\":\"\",\"rank\":1,\"hl_start\":0,\"hl_end\":0,"
        "\"picture\":false,\"meanings\":[],\"pitch\":[\"1\"]}",
        "{\"version\":2,\"spelling\":\"猫\",\"reading\":\"\",\"sentence\":\"\",\"rank\":1,\"hl_start\":0,\"hl_end\":0,"
        "\"picture\":false,\"meanings\":[],\"misc\":7}",
        "{\"version\":2,\"spelling\":\"猫\",\"reading\":\"\",\"sentence\":\"\",\"rank\":1,\"hl_start\":0,\"hl_end\":0,"
        "\"picture\":false,\"meanings\":[]} {}",
        /* version 3's own values, and a version this build does not know */
        "{\"version\":3,\"spelling\":\"猫\",\"reading\":\"\",\"sentence\":\"\",\"rank\":1,\"hl_start\":0,\"hl_end\":0,"
        "\"picture\":false,\"meanings\":[],\"more\":{}}",
        "{\"version\":3,\"spelling\":\"猫\",\"reading\":\"\",\"sentence\":\"\",\"rank\":1,\"hl_start\":0,\"hl_end\":0,"
        "\"picture\":false,\"meanings\":[],\"more\":[\"x\"]}",
        "{\"version\":3,\"spelling\":\"猫\",\"reading\":\"\",\"sentence\":\"\",\"rank\":1,\"hl_start\":0,\"hl_end\":0,"
        "\"picture\":false,\"meanings\":[],\"more\":[{\"dictionary\":\"a\"}]}",
        "{\"version\":3,\"spelling\":\"猫\",\"reading\":\"\",\"sentence\":\"\",\"rank\":1,\"hl_start\":0,\"hl_end\":0,"
        "\"picture\":false,\"meanings\":[],\"more\":[{\"meanings\":[]},{\"meanings\":[]},{\"meanings\":[]},"
        "{\"meanings\":[]},{\"meanings\":[]},{\"meanings\":[]},{\"meanings\":[]},{\"meanings\":[]}]}",
        "{\"version\":3,\"spelling\":\"猫\",\"reading\":\"\",\"sentence\":\"\",\"rank\":1,\"hl_start\":0,\"hl_end\":0,"
        "\"picture\":false,\"meanings\":[],\"furigana\":7}",
        "{\"version\":5,\"spelling\":\"猫\",\"reading\":\"\",\"sentence\":\"\",\"rank\":1,\"hl_start\":0,\"hl_end\":0,"
        "\"picture\":false,\"meanings\":[]}",
        /* version 4: no rank is 0, never negative */
        "{\"version\":4,\"spelling\":\"猫\",\"reading\":\"\",\"sentence\":\"\",\"rank\":-1,\"hl_start\":0,\"hl_end\":0,"
        "\"picture\":false,\"meanings\":[]}",
    };
    static const char *jm[] = {"[JMdict]\nto come"};
    static const VjoAnkiGloss more[1] = {{"JMdict", "来る", "くる", jm, 1}};
    VjoAnkiNote n = {.spelling = "来る", .reading = "くる", .rank = 384, .meanings = meanings, .n_meanings = 2,
                     .sentence = "明日来る", .hl_start = 6, .hl_end = 12, .sentence_furigana = "明日[あした]<b>来[く]る</b>",
                     .sf_hl_start = 18, .sf_hl_end = 34, .source = "Jiten", .pitch = {1, 0}, .n_pitch = 2,
                     .pos = pos, .n_pos = 2, .misc = "CLANNAD (2026-10-12 09:30)", .furigana = "来[く]る",
                     .dictionary = "Jiten", .more = more, .n_more = 1, .game = "CLANNAD", .title_id = "PCSG00415",
                     .platform = "vita", .unix_time = 4102444800LL /* 2100: past a 32-bit time */,
                     .ocr_engine = "vocr", .ocr_model = "VOCR3_FL10_w8.vocr"}, d;
    VjoArena a;
    char *json, *again, id[VJO_QUEUE_ID_SIZE], id2[VJO_QUEUE_ID_SIZE];
    int picture;
    vjo_arena_init(&a, mem, sizeof(mem));
    json = vjo_queue_encode(&a, &n, 1);
    again = vjo_queue_encode(&a, &n, 1);
    TEST_ASSERT(json && again && !strcmp(json, again));
    vjo_queue_id(json, strlen(json), id);
    vjo_queue_id(again, strlen(again), id2);
    TEST_CHECK(vjo_queue_valid_id(id) && !strcmp(id, id2));
    TEST_CHECK(strstr(json, ",\"misc\":\"CLANNAD (2026-10-12 09:30)\",\"furigana\":\"来[く]る\",\"dictionary\":\"Jiten\","
                            "\"more\":[{\"dictionary\":\"JMdict\",\"spelling\":\"来る\",\"reading\":\"くる\","
                            "\"meanings\":[\"[JMdict]\\nto come\"]}],\"game\":\"CLANNAD\",\"title_id\":\"PCSG00415\","
                            "\"platform\":\"vita\",\"ocr_engine\":\"vocr\",\"ocr_model\":\"VOCR3_FL10_w8.vocr\","
                            "\"time\":4102444800,\"ocr_confidence\":null}\n") != NULL);
    TEST_MSG("%s", json);
    TEST_ASSERT(vjo_queue_decode(&a, json, strlen(json), &d, &picture) == 4);
    TEST_CHECK(!strcmp(d.furigana, "来[く]る") && !strcmp(d.dictionary, "Jiten") && d.n_more == 1 &&
               !strcmp(d.more[0].dictionary, "JMdict") && !strcmp(d.more[0].spelling, "来る") &&
               d.more[0].n_meanings == 1 && !strcmp(d.more[0].meanings[0], jm[0]));
    TEST_CHECK(picture && !strcmp(d.spelling, n.spelling) && !strcmp(d.reading, n.reading) && d.rank == 384 &&
               d.n_meanings == 2 && !strcmp(d.meanings[1], "to arrive") && !strcmp(d.sentence, n.sentence) &&
               d.hl_start == 6 && d.hl_end == 12 && !strcmp(d.sentence_furigana, n.sentence_furigana) &&
               d.sf_hl_start == 18 && d.sf_hl_end == 34 && !strcmp(d.source, "Jiten") && d.n_pitch == 2 &&
               d.pitch[0] == 1 && d.pitch[1] == 0 && d.n_pos == 2 && !strcmp(d.pos[0], "vk") &&
               !strcmp(d.misc, n.misc) && d.unix_time == 4102444800LL);
    /* a different screenshot flag is a different card */
    again = vjo_queue_encode(&a, &n, 0);
    vjo_queue_id(again, strlen(again), id2);
    TEST_CHECK(strcmp(id, id2) != 0);
    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        vjo_arena_reset(&a);
        TEST_CHECK(vjo_queue_decode(&a, bad[i], strlen(bad[i]), &d, &picture) == -1);
        TEST_MSG("record %u", i);
    }
    /* No rank: 99999 in versions 1-3 (jpdb's), 0 in version 4, where 99999 is
     * one of Jiten's ranks; an encoded note without one writes 0. */
    {
        static const struct { const char *json; int version, rank; } ranks[] = {
            {"{\"version\":1,\"spelling\":\"猫\",\"reading\":\"\",\"sentence\":\"\",\"rank\":99999,\"hl_start\":0,"
             "\"hl_end\":0,\"picture\":false,\"meanings\":[]}", 1, VJO_NO_RANK},
            {"{\"version\":3,\"spelling\":\"猫\",\"reading\":\"\",\"sentence\":\"\",\"rank\":99999,\"hl_start\":0,"
             "\"hl_end\":0,\"picture\":false,\"meanings\":[]}", 3, VJO_NO_RANK},
            {"{\"version\":3,\"spelling\":\"猫\",\"reading\":\"\",\"sentence\":\"\",\"rank\":1536,\"hl_start\":0,"
             "\"hl_end\":0,\"picture\":false,\"meanings\":[]}", 3, 1536},
            {"{\"version\":4,\"spelling\":\"猫\",\"reading\":\"\",\"sentence\":\"\",\"rank\":99999,\"hl_start\":0,"
             "\"hl_end\":0,\"picture\":false,\"meanings\":[]}", 4, 99999},
            {"{\"version\":4,\"spelling\":\"猫\",\"reading\":\"\",\"sentence\":\"\",\"rank\":0,\"hl_start\":0,"
             "\"hl_end\":0,\"picture\":false,\"meanings\":[]}", 4, VJO_NO_RANK}};
        for (unsigned i = 0; i < sizeof(ranks) / sizeof(*ranks); i++) {
            vjo_arena_reset(&a);
            TEST_CHECK(vjo_queue_decode(&a, ranks[i].json, strlen(ranks[i].json), &d, &picture) == ranks[i].version);
            TEST_CHECK_(d.rank == ranks[i].rank, "record %u: rank %d", i, d.rank);
        }
        VjoAnkiNote none = n;
        none.rank = VJO_NO_RANK;
        vjo_arena_reset(&a);
        json = vjo_queue_encode(&a, &none, 0);
        TEST_ASSERT(json && strstr(json, "{\"version\":4,") == json && strstr(json, ",\"rank\":0,"));
        TEST_CHECK(vjo_queue_decode(&a, json, strlen(json), &d, &picture) == 4 && d.rank == VJO_NO_RANK);
        none.rank = 99999;
        json = vjo_queue_encode(&a, &none, 0);
        TEST_CHECK(json && vjo_queue_decode(&a, json, strlen(json), &d, &picture) == 4 && d.rank == 99999);
    }
    /* over the size bound: neither written nor read */
    memset(oversized, 'x', sizeof(oversized) - 1);
    n.sentence = oversized;
    vjo_arena_reset(&a);
    TEST_CHECK(vjo_queue_encode(&a, &n, 0) == NULL);
    TEST_CHECK(vjo_queue_decode(&a, oversized, sizeof(oversized) - 1, &d, &picture) == -1);
    TEST_CHECK(!vjo_queue_valid_id("../config") && !vjo_queue_valid_id("0123456789ABCDEF0123456789abcdef") &&
               !vjo_queue_valid_id("0123456789abcdef") && vjo_queue_valid_id(id));
}

/* Many cards: one allocation and one thread per save, everything released. */
static void test_fifty_cards_release_everything(void)
{
    static char headers[50][64];
    size_t save_peak = 0;
    reset();
    for (int i = 0; i < 50; i++) {
        snprintf(headers[i], sizeof(headers[i]), "猫が好き%d", i);
        list.header = headers[i];
        g_view.list_seq = 100 + (unsigned)i;
        enter(i % 2);
        TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && !heap_live && job == JOB_NONE);
        if (last_peak > save_peak)
            save_peak = last_peak;
    }
    TEST_CHECK(vjo_queue_count() == 50 && g_view.anki_pending == 50);
    TEST_CHECK(heap_allocs == 50 && heap_frees == 50 && threads_created == 50 && threads_deleted == 50);
    printf("measure: 50 cards: max save peak %u of %u\n", (unsigned)save_peak, (unsigned)ANKI_TEXT_MEM);
    TEST_CHECK(save_peak <= ANKI_TEXT_MEM / 4);
    finish();
}

/* SceShell refuses the VjoAnki thread. With anki_mining on it is created on every
 * overlay open (to count the queue), and for enter: each refusal is in log.txt
 * with its rc, the bytes asked for, SceShell's free USER and the Shell pages we hold,
 * and the job that did not start; a thread that is created but does not start too. */
static void test_a_refused_job_thread_is_logged(void)
{
    char expected[160];
    reset();
    vjo_test_shell_free_rc = 0;
    vjo_test_shell_free_user = 28672; /* the lowest SceShell figure measured beside a game */
    fail_create = 1;
    vjo_anki_post_count();
    vjo_anki_reap();
    enter(0);
    fail_create = 0;
    snprintf(expected, sizeof(expected), "thread VjoAnki refused 0xFFFFFFFF: %u B; SceShell free 28672 (0x00000000), "
                                         "held %u\n", (unsigned)VJO_BUDGET_SHELL_ANKI_STACK,
             (unsigned)VJO_BUDGET_SHELL_MODULE_PAGES);
    TEST_CHECK(times_logged(expected) == 2);
    TEST_CHECK(logged("anki: count not started 0xFFFFFFFF\n") && logged("anki: save not started 0xFFFFFFFF\n"));
    TEST_MSG("%s", all_logs);
    TEST_CHECK(!threads_created && !heap_attempts && !strcmp(g_view.anki_status, "Anki: not enough memory, card not saved"));
    /* created, then refused to start: deleted again, logged with the start's rc */
    fail_start = 1;
    enter(0);
    fail_start = 0;
    TEST_CHECK(times_logged("anki: save not started 0xFFFFFFFF\n") == 2 && threads_created == 1 && threads_deleted == 1);
    TEST_CHECK(vjo_ledger_current(&g_shell_ledger, VJO_SL_ANKI_STACK) == 0);
    /* the query failed: its rc is logged, not a made-up figure */
    vjo_test_shell_free_rc = (int)0x80020001u;
    fail_create = 1;
    vjo_anki_post_count();
    fail_create = 0;
    TEST_CHECK(logged("SceShell free 0 (0x80020001)"));
    finish();
}


#ifdef VJO_WITH_VOCR
/* ---------------- a card's screenshot (API 23) ---------------- */

static void screenshots_on(void)
{
    cfg.anki_screenshot = 1;
    reconfigure();
}

/* Enter with screenshots on (the default): the game worker's JPEG is beside the
 * card, named by its record's ID, and the record says so; SceShell took only its
 * job's arena, as for a text-only card. */
static void test_enter_saves_the_screenshot_beside_the_card(void)
{
    char id[VJO_QUEUE_ID_SIZE];
    size_t len, text_peak;
    const char *json, *jpg;
    VjoConfig d;
    vjo_config_defaults(&d);
    TEST_CHECK(d.anki_screenshot == 1); /* on unless anki_screenshot = off */
    /* the same card without its screenshot first: the arena peak a screenshot may add to is this one's */
    reset();
    enter(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && shot_jobs == 0);
    text_peak = last_peak;
    finish();
    reset();
    screenshots_on();
    enter(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && g_view.anki_mark[0] == VJO_ANKI_MARK_QUEUED);
    TEST_MSG("%s", g_view.anki_status);
    TEST_CHECK(files_ending(".json") == 1 && files_ending(".jpg") == 1 && files_ending(".tmp") == 0);
    only_id(id);
    TEST_CHECK(!strcmp(id, shot_id)); /* the shot named the card's record */
    TEST_CHECK((json = queue_file(id, "json", &len)) && strstr(json, "\"picture\":true"));
    TEST_CHECK((jpg = queue_file(id, "jpg", &len)) && len == sizeof(shot_jpeg) && !memcmp(jpg, shot_jpeg, len));
    TEST_CHECK(shot_jobs == 1 && shot_reads == 1 && slot_begins == 1 && slot_ends == 1);
    TEST_CHECK(!heap_live && heap_allocs == 1 && heap_frees == 1 && heap_last == ANKI_TEXT_MEM);
    printf("measure: save with a screenshot: arena peak %u of %u (%u without it)\n", (unsigned)last_peak,
           (unsigned)ANKI_TEXT_MEM, (unsigned)text_peak);
    /* SceShell holds the job's request and result for the screenshot, never its JPEG (AGENTS.md rule 1; the
     * fake's is a real one's size, 56,934 B): at most those above the text-only save (8 B: the arena's
     * alignment), and no .jpg opened (finish) */
    TEST_CHECK_(last_peak <= text_peak + sizeof(VjoGameOcrRequest) + sizeof(VjoGameOcrResult) + 8,
                "peak %u, %u without the screenshot", (unsigned)last_peak, (unsigned)text_peak);
    /* The normal build keeps phase timings for slow-card diagnosis; the debug
     * build includes the detailed screenshot and record metadata too. */
#if VJO_DEBUG
    TEST_CHECK(logged("anki: screenshot rc=0 960x544 q70 try 1: 56934 B; block 450560 B") && logged(" + .jpg\n"));
#else
    TEST_CHECK(logged("capture/encode 0 ms, write 0 ms; io=0x00000000") &&
               !logged(" + .jpg\n") && logged("anki: save rc=0, "));
#endif
    TEST_MSG("%s", all_logs);
    /* the same card from a new lookup in the same minute: queued already, no second screenshot */
    g_view.list_seq = 6;
    enter(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Already queued") && shot_jobs == 1 && files_ending(".jpg") == 1);
    /* ... and with screenshots off now: still the same card */
    cfg.anki_screenshot = 0;
    reconfigure();
    g_view.list_seq = 7;
    enter(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Already queued") && files_ending(".json") == 1 && shot_jobs == 1);
    finish();
}

/* Without the screenshot it asked for (the game worker busy, absent, out of
 * memory, a file that is not there, the overlay closed meanwhile), the card is
 * saved as it would have been with screenshots off, and nothing of the
 * screenshot is left; the status says so, log.txt why. */
static void test_a_failed_screenshot_saves_the_card_without_one(void)
{
    static const char *const why[] = {"the slot stayed busy", "no game worker", "the game's memory",
                                      "no file", "the overlay closed"};
    char id[VJO_QUEUE_ID_SIZE];
    size_t len;
    const char *json;
    for (int k = 0; k < 5; k++) {
        reset();
        screenshots_on();
        slot_busy = k == 0;
        shot_submit_rc = k == 1 ? VJO_ERR_NO_GAME : 0;
        shot_rc = k == 2 ? VJO_E_OOM : 0;
        shot_no_file = k == 3;
        shot_close_overlay = k == 4;
        enter(0);
        TEST_CHECK_(files_ending(".json") == 1 && files_ending(".jpg") == 0 && files_ending(".tmp") == 0, "%s", why[k]);
        only_id(id);
        TEST_CHECK_((json = queue_file(id, "json", &len)) && strstr(json, "\"picture\":false"), "%s", why[k]);
        TEST_CHECK_(!strcmp(g_view.anki_status, "Saved for Anki, without a screenshot (log.txt)"), "%s: %s", why[k],
                    g_view.anki_status);
        /* why, in both builds: the screenshot's failure, or its file not there (the debug build's take_shot
         * line and the queue's; the normal build's one line, after the save's: the reason's rc, -1 for no file) */
#if VJO_DEBUG
        TEST_CHECK_((k == 3 ? logged(".jpg missing\n") : logged("anki: screenshot rc=")) &&
                    slot_begins == slot_ends, "%s", why[k]);
#else
        TEST_CHECK_(logged(k == 3 ? "anki: save rc=0, " : "anki: screenshot rc=") &&
                    logged(k == 3 ? "anki: screenshot rc=-1\n" : "anki: save rc=0, ") &&
                    !logged(".jpg missing") && !logged(" free USER ") && slot_begins == slot_ends, "%s", why[k]);
#endif
        /* the game worker's steps: the debug build's (shell/selftest.c formats them), when it had any */
#if VJO_DEBUG
        TEST_CHECK_(k != 2 || logged("gstep shot "), "%s: the worker's steps", why[k]);
#else
        TEST_CHECK_(!logged("gstep "), "%s: no step lines in the normal build", why[k]);
#endif
        TEST_CHECK(!heap_live && heap_allocs == heap_frees);
        finish();
    }
}

/* The record that names the screenshot is committed after it, and a commit that
 * fails takes the screenshot with it: no card, no JPEG. */
static void test_a_captured_shot_finishes_after_overlay_closes(void)
{
    reset();
    screenshots_on();
    shot_close_overlay = 2;
    enter(0);
    TEST_CHECK(!g_view.open && shot_reads == 2 && slot_begins == slot_ends);
    TEST_CHECK(files_ending(".jpg") == 1 && files_ending(".json") == 1 && files_ending(".tmp") == 0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki"));
    TEST_CHECK(!heap_live && heap_allocs == heap_frees);
    finish();
}

static void test_a_failed_record_takes_its_screenshot_with_it(void)
{
    reset();
    screenshots_on();
    fail_rename = 1;
    enter(0);
    TEST_CHECK(!strncmp(g_view.anki_status, "Card not saved: cannot write", 28) && shot_jobs == 1);
    TEST_CHECK(files_ending(".json") == 0 && files_ending(".jpg") == 0 && files_ending(".tmp") == 0);
    fail_rename = 0;
    finish();
}

/* A save that stopped between the two writes left the card's JPEG with no record
 * (never a card: the queue counts records). Saving the card again replaces it. */
static void test_an_interrupted_saves_jpeg_is_replaced(void)
{
    char id[VJO_QUEUE_ID_SIZE], path[256];
    size_t len;
    const char *jpg;
    FILE *f;
    reset();
    screenshots_on();
    enter(0);
    only_id(id);
    snprintf(path, sizeof(path), VJO_ANKI_QUEUE_DIR "/%s.json", id);
    TEST_ASSERT(unlink(path) == 0);
    snprintf(path, sizeof(path), VJO_ANKI_QUEUE_DIR "/%s.jpg", id);
    TEST_ASSERT((f = fopen(path, "wb")) != NULL);
    fputs("not this card's picture", f);
    fclose(f);
    vjo_anki_post_count();
    vjo_anki_reap();
    TEST_CHECK(g_view.anki_pending == 0); /* a JPEG alone is no card */
    g_view.list_seq = 6;
    enter(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && shot_jobs == 2);
    TEST_CHECK(files_ending(".json") == 1 && files_ending(".jpg") == 1);
    TEST_CHECK((jpg = queue_file(id, "jpg", &len)) && len == sizeof(shot_jpeg) && !memcmp(jpg, shot_jpeg, len));
    finish();
}

/* With screenshots off, or no game in front, nothing asks the game worker. */
static void test_no_screenshot_is_asked_for_when_off_or_without_a_game(void)
{
    reset(); /* (the fixture's anki_screenshot = off) */
    enter(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && !slot_begins && !shot_jobs && files_ending(".jpg") == 0);
    finish();
    reset();
    screenshots_on();
    vjo_anki_set_game("", "", 0);
    enter(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && !slot_begins && !shot_jobs && files_ending(".jpg") == 0);
    finish();
}
#endif

/* ---------------- Anki for Vita's duplicate index (core/anki_index.h) ---------------- */

/* An index of these notes at path, as Anki for Vita writes one (host/tests/afv_index_writer.h). */
static void write_index_at(const char *path, const AfvWord *notes, int n)
{
    size_t size = 0;
    uint8_t *data = afv_write(notes, n, AFV_RUBI_REQUEST_HASH, 1791158400000ull, 1791158460000ull, &size);
    FILE *f;
    TEST_ASSERT(data != NULL);
    TEST_ASSERT((f = fopen(path, "wb")) != NULL);
    TEST_ASSERT(fwrite(data, 1, size, f) == size && fclose(f) == 0);
    free(data);
}
static void write_index(const AfvWord *notes, int n) { write_index_at(VJO_ANKI_INDEX_PATH, notes, n); }

/* Enter on a word the index has: "Already in Anki" and the √, nothing saved, no memory, one open and
 * three reads; a word it lacks is saved as before; the √ stays for the next Enter. */
static void test_a_word_in_anki_is_not_saved(void)
{
    static const AfvWord notes[] = {{"猫", "ネコ"}, {"犬", "いぬ"}}; /* 猫's reading in katakana in the note */
    reset();
    write_index(notes, 2);
    enter(0); /* 猫 (ねこ) */
    TEST_CHECK(!strcmp(g_view.anki_status, "Already in Anki") && g_view.anki_status_kind == VJO_ANKI_STATUS_DIM);
    TEST_MSG("%s", g_view.anki_status);
    TEST_CHECK(g_view.anki_marks_seq == 5 && g_view.anki_mark[0] == VJO_ANKI_MARK_IN_ANKI &&
               g_view.anki_mark[2] == VJO_ANKI_MARK_IN_ANKI && VJO_ANKI_MARK_TICKED(g_view.anki_mark[0]));
    TEST_CHECK(heap_attempts == 0 && files_ending(".json") == 0 && mount_sync_calls == 0);
    TEST_CHECK(index_opens == 1 && index_failed_opens == 0 && index_closes == 1 && index_reads == 3 &&
               index_read_bytes <= 128 + 8 + 512);
    TEST_CHECK(times_logged("anki: Anki for Vita's duplicate index read") == 1);
    /* Enter again: the mark says it, without reading the index again */
    enter(2);
    TEST_CHECK(!strcmp(g_view.anki_status, "Already in Anki") && index_opens == 1 && heap_attempts == 0);
    /* a word the index lacks: saved, as without one */
    enter(1); /* 好き (すき) */
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && files_ending(".json") == 1);
    TEST_CHECK(index_opens == 2 && heap_allocs == 1 && g_view.anki_mark[1] == VJO_ANKI_MARK_QUEUED);
    TEST_CHECK(!VJO_ANKI_MARK_TICKED(VJO_ANKI_MARK_QUEUED) && VJO_ANKI_MARK_TICKED(VJO_ANKI_MARK_ADDED));
    TEST_CHECK(times_logged("anki: Anki for Vita's duplicate index") == 1); /* its state did not change */
    finish();
}

/* The same form with another reading is another word; a note without a reading matches the form. */
static void test_the_reading_decides(void)
{
    static const AfvWord other_reading[] = {{"猫", "びょう"}};
    static const AfvWord no_reading[] = {{"猫", ""}};
    reset();
    write_index(other_reading, 1);
    enter(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && files_ending(".json") == 1);
    finish();
    reset();
    write_index(no_reading, 1);
    enter(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Already in Anki") && files_ending(".json") == 0 && heap_attempts == 0);
    finish();
}

/* No index, a damaged one, one of another version: every card saved as before, the index's state logged
 * once (and again only when it changes); the previous index (.old) when there is no rubi.idx. */
static void test_no_or_a_bad_index_saves_as_before(void)
{
    static const AfvWord notes[] = {{"猫", "ねこ"}};
    uint8_t data[4096];
    ssize_t n = 0;
    int fd;
    reset();
    enter(0); /* no index: rubi.idx and rubi.idx.old tried */
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && index_failed_opens == 2 && index_reads == 0);
    TEST_CHECK(times_logged("anki: Anki for Vita's duplicate index absent") == 1);
    clear_dir(VJO_ANKI_QUEUE_DIR);
    memset(g_view.anki_mark, 0, sizeof(g_view.anki_mark));
    enter(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && times_logged("duplicate index absent") == 1);
    /* damaged: the magic, another version, a cut file, a byte too many */
    write_index(notes, 1);
    fd = open(VJO_ANKI_INDEX_PATH, O_RDONLY);
    TEST_ASSERT(fd >= 0 && (n = read(fd, data, sizeof(data))) > 128);
    close(fd);
    for (int k = 0; k < 4; k++) {
        uint8_t bad[4096];
        size_t len = (size_t)n;
        memcpy(bad, data, (size_t)n);
        if (k == 0) bad[3] ^= 1;
        if (k == 1) bad[8] = 2;
        if (k == 2) len = (size_t)n - 1;
        if (k == 3) bad[len++] = 0;
        fd = open(VJO_ANKI_INDEX_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        TEST_ASSERT(fd >= 0 && write(fd, bad, len) == (ssize_t)len && close(fd) == 0);
        clear_dir(VJO_ANKI_QUEUE_DIR);
        memset(g_view.anki_mark, 0, sizeof(g_view.anki_mark));
        enter(0);
        TEST_CHECK_(!strcmp(g_view.anki_status, "Saved for Anki"), "damage %d: %s", k, g_view.anki_status);
    }
    TEST_CHECK(times_logged("anki: Anki for Vita's duplicate index not used (damaged, another version)") == 1);
    /* only the previous index (an Anki for Vita publish stopped between its renames): it is read */
    unlink(VJO_ANKI_INDEX_PATH);
    write_index_at(VJO_ANKI_INDEX_OLD_PATH, notes, 1);
    memset(g_view.anki_mark, 0, sizeof(g_view.anki_mark));
    enter(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Already in Anki") && times_logged("duplicate index read") == 1);
    finish();
}

/* anki_duplicate_check = off: the index is never opened, every card saved. */
static void test_off_reads_no_index(void)
{
    static const AfvWord notes[] = {{"猫", "ねこ"}};
    reset();
    write_index(notes, 1);
    cfg.anki_duplicate_check = 0;
    reconfigure();
    enter(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && index_opens == 0 && index_failed_opens == 0);
    TEST_CHECK(!logged("duplicate index"));
    finish();
}

TEST_LIST = {
    {"anki_off_by_default_starts_no_thread_and_allocates_nothing", test_off_by_default_allocates_nothing},
    {"enter_saves_offline_and_the_queue_survives_restart", test_enter_saves_offline},
    {"a_card_exported_before_says_so", test_a_card_exported_before_says_so},
    {"a_psp_games_card_says_psp", test_a_psp_games_card_says_psp},
    {"japanese_text_and_escaping_round_trip", test_japanese_and_escaping_round_trip},
    {"same_card_saved_again_within_the_minute_is_one_record", test_same_card_within_a_minute_is_one_record},
    {"failed_writes_and_temp_files_never_queue_a_partial_card", test_failed_writes_never_queue_a_partial_card},
    {"failed_directory_flush_is_retried_before_acknowledging", test_failed_directory_flush_is_retried},
    {"oom_keeps_recognized_text_and_queued_cards", test_oom_keeps_recognized_text_and_cards},
    {"stale_list_close_stop_and_busy_jobs_cancel_without_leaks", test_cancellation},
    {"enter_records_every_dictionary", test_enter_records_every_dictionary},
    {"enter_names_titled_dictionaries", test_enter_names_titled_dictionaries},
    {"what_enter_saves_whatever_the_dictionaries", test_what_enter_saves_whatever_the_dictionaries},
    {"record_round_trip_stable_ids_and_strict_decoding", test_record_format},
    {"fifty_cards_release_memory_and_threads", test_fifty_cards_release_everything},
    {"a_refused_job_thread_is_logged_with_its_rc_and_sceshell_free", test_a_refused_job_thread_is_logged},
#ifdef VJO_WITH_VOCR
    {"api23_enter_saves_the_screenshot_beside_the_card", test_enter_saves_the_screenshot_beside_the_card},
    {"api23_a_failed_screenshot_saves_the_card_without_one", test_a_failed_screenshot_saves_the_card_without_one},
    {"api23_a_failed_record_takes_its_screenshot_with_it", test_a_failed_record_takes_its_screenshot_with_it},
    {"api23_a_captured_shot_finishes_after_overlay_closes", test_a_captured_shot_finishes_after_overlay_closes},
    {"api23_an_interrupted_saves_jpeg_is_replaced", test_an_interrupted_saves_jpeg_is_replaced},
    {"api23_no_screenshot_is_asked_for_when_off_or_without_a_game", test_no_screenshot_is_asked_for_when_off_or_without_a_game},
    {"a_word_in_anki_is_not_saved", test_a_word_in_anki_is_not_saved},
    {"the_reading_decides_a_word_in_anki", test_the_reading_decides},
    {"no_or_a_bad_index_saves_as_before", test_no_or_a_bad_index_saves_as_before},
    {"duplicate_check_off_reads_no_index", test_off_reads_no_index},
#endif
    {NULL, NULL}
};

/* ---------------- restart steps (test_anki_restart.py) ---------------- */

static void print_string(const char *s)
{
    static uint8_t mem[4096];
    VjoArena a;
    VjoBuf b;
    vjo_arena_init(&a, mem, sizeof(mem));
    vjo_buf_init(&b, &a);
    vjo_json_write_string(&b, s);
    fputs(vjo_buf_cstr(&b), stdout);
}

/* DIR ACTION [CRASH]: save (the shown 猫) or save-other (好き), text only; save-shot and
 * save-shot-other the same with its screenshot (API 23: the game's JPEG, then the record).
 * Prints one JSON line. */
static int restart_step(int argc, char **argv)
{
    const char *what;
    if (argc < 4 || chdir(argv[2]) < 0)
        return 2;
    what = argv[3];
    mkdir("ux0:data", 0777);
    if (argc > 4 && argv[4][0])
        crash_point = argv[4];
    short_io = 1; /* genuinely incomplete writes at the *_partial checkpoints */
    if (vjo_anki_start() < 0)
        return 2;
    fixture();
    if (!strncmp(what, "save-shot", 9)) {
#ifdef VJO_WITH_VOCR
        cfg.anki_screenshot = 1;
        what = what[9] ? "save-other" : "save";
#else
        return 2;
#endif
    }
    vjo_anki_configure(&cfg);
    vjo_anki_set_game("PCSG00415", "CLANNAD", 0);
    if (strcmp(what, "save") && strcmp(what, "save-other"))
        return 2;
    vjo_anki_post_add(5, !strcmp(what, "save-other"));
    printf("{\"status\":");
    print_string(g_view.anki_status);
    printf(",\"count\":%d}\n", g_view.anki_pending);
    return strncmp(g_view.anki_status, "Saved", 5) && strncmp(g_view.anki_status, "Already", 7);
}

int main(int argc, char **argv)
{
    const char *dir;
    make_shot_jpeg();
    if (argc >= 2 && !strcmp(argv[1], "--restart-step"))
        return restart_step(argc, argv);
    /* One storage directory for every test (reset empties the queue), removed when the suite ends
     * (acutest runs each test in a child that also exits through atexit: test_tmpdir.h). */
    dir = vjo_test_tmpdir("shell-anki");
    if (!dir || chdir(dir) < 0 || mkdir("ux0:data", 0777) < 0)
        return 2;
    return acutest_main(argc, argv);
}
