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
#include "soak_track.h" /* #528 (mem-leak): soak_cycles */
#include "anki.h"
#include "json.h"
#include "local_dict.h" /* VJO_LOCAL_MAX_ENTRY: the dictionary fixtures' largest payload */
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
static void (*on_clock)(void); /* #528: card_facts' call, after the note's copy and before its record is named */
int sceRtcGetCurrentClockLocalTime(SceDateTime *t)
{
    if (on_clock)
        on_clock();
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
/* The memory card's files (fakes/sceio_posix.h; a failure is -1, as the suite's own calls return it), with the
 * suite's faults, crash points and its one listing at a time. */
#define SCEIO_ERROR(e) ((void)(e), -1)
#define SCEIO_OWN (SCEIO_OPEN | SCEIO_CLOSE | SCEIO_READ | SCEIO_WRITE | SCEIO_RENAME | SCEIO_SYNCBYFD | SCEIO_SYNC | \
                   SCEIO_DOPEN | SCEIO_DREAD | SCEIO_DCLOSE)
#include "fakes/sceio_posix.h"
#include "fakes/vjdict_writer.h"
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
int sceIoRename(const char *from, const char *to)
{
    int rc = fail_rename ? -1 : rename(from, to);
    if (!rc) {
        snprintf(last_rename, sizeof(last_rename), "%s", to);
        if (ends_with(to, ".json")) crash_at("json_renamed");
    }
    return rc;
}
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
    /* the writer's 8 KiB of header and index, keys and postings, and 4 whole payloads (or 8 halves) */
    uint8_t bytes[8192 + 2048 + 4 * VJO_LOCAL_MAX_ENTRY];
    size_t size;
    int active;
} DictFile;
static DictFile dict_files[8];
static int dict_opens;

/* One key with one entry (VJDICT2: fakes/vjdict_writer.h). */
static void dict_build(DictFile *f, const char *name, const DictRow *r)
{
    memset(f, 0, sizeof(*f));
    f->name = name;
    f->size = vjdict2_one(f->bytes, sizeof(f->bytes), r->key, r->spelling, r->reading, r->meaning, r->title);
    TEST_ASSERT(f->size > 0);
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

/* The Shell's platform, for its file reads (it has nothing else: the overlay is offline). */
void vjo_platform_vita(VjoPlatform *p)
{
    memset(p, 0, sizeof(*p));
    p->file_open = dict_open;
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
static uint8_t shot_jpeg_fake[SHOT_JPEG_BYTES];
/* The JPEG the fake game worker writes: the fake above, or (the restart steps, VJO_TEST_SHOT_JPEG=FILE) a file's,
 * the actual game worker's JPEG of a frame (host/tests/test_card_screenshot.py chains the two). */
static uint8_t *shot_jpeg = shot_jpeg_fake;
static size_t shot_jpeg_bytes = SHOT_JPEG_BYTES;
static int load_shot_jpeg(const char *path)
{
    static uint8_t real[VJO_GAME_OCR_SHOT_MAX];
    FILE *f = fopen(path, "rb");
    size_t n = f ? fread(real, 1, sizeof(real), f) : 0;
    if (f) fclose(f);
    if (n < 4 || n >= sizeof(real) || real[0] != 0xFF || real[1] != 0xD8) return -1;
    shot_jpeg = real;
    shot_jpeg_bytes = n;
    return 0;
}
static void make_shot_jpeg(void)
{
    uint8_t *shot_jpeg = shot_jpeg_fake;
    static const uint8_t head[] = {0xFF, 0xD8, 0xFF, 0xE0, 0, 16, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0};
    memcpy(shot_jpeg, head, sizeof(head));
    for (size_t i = sizeof(head); i < SHOT_JPEG_BYTES - 2; i++)
        shot_jpeg[i] = (uint8_t)(i * 131u % 251u); /* never 0xFF */
    shot_jpeg[SHOT_JPEG_BYTES - 2] = 0xFF;
    shot_jpeg[SHOT_JPEG_BYTES - 1] = 0xD9;
}
static int shot_jobs, shot_rc, shot_submit_rc, shot_no_file, slot_busy, slot_begins, slot_ends, shot_close_overlay;
static int shot_reads, shot_job_rc;
static uint32_t shot_speed; /* API 32: the last shot's speed word (VJO_GAME_OCR_S_TRIM or 0) */
static uint32_t shot_seq;
static char shot_id[VJO_QUEUE_ID_SIZE];
static void (*on_shot)(void); /* #528: the screenshot's job, after the record is named and before it is written */
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
    if (on_shot)
        on_shot();
    TEST_CHECK(r->size == sizeof(*r) && r->flags == VJO_GAME_OCR_F_SHOT && !r->tiers && !(r->speed & ~VJO_GAME_OCR_S_TRIM) &&
               !r->model[0] && !r->model_dir[0] && strlen(r->shot) == 32 && vjo_queue_valid_id(r->shot));
    shot_speed = r->speed;
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
        TEST_ASSERT(fwrite(shot_jpeg, 1, shot_jpeg_bytes, f) == shot_jpeg_bytes);
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
    out->shot_bytes = shot_job_rc ? 0 : (uint32_t)shot_jpeg_bytes;
    out->n_steps = 1;
    out->step[0].id = VJO_GSTEP_SHOT;
    out->step[0].rc = shot_job_rc;
    if (shot_speed & VJO_GAME_OCR_S_TRIM) { /* API 32: a PSP game's frame shown 1x, its bars trimmed */
        out->n_steps = 2;
        out->step[1] = (VjoGameOcrStep){.id = VJO_GSTEP_TRIM, .kind = 1, .arg = 1, .bytes = 238u << 16 | 134u,
                                        .sum = 484u << 16 | 276u, .us = 2100};
    }
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
    shot_speed = 0;
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
    on_clock = on_shot = NULL;
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
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && dict_opens == 5);
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
    TEST_CHECK(dict_opens == 5);
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
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && dict_opens == 5);
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

            cfg.anki_screenshot = with;
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

/* core/arena.c's host-only cap (test_oom_paths.c's): the arena whose memory starts at vjo_test_arena_base holds only
 * its first vjo_test_arena_cap bytes, as a smaller one would. */
extern const void *vjo_test_arena_base;
extern size_t vjo_test_arena_cap;

/* What the same steps as do_save's make of the shown entry in a 1 MiB arena (no request of them refused): the
 * card's record (NULL when it makes none), and that arena's peak. The job's own statics (its arena, its settings'
 * copy) are borrowed and given back. */
static const char *reference_record(int entry, int picture, size_t *peak, int *more)
{
    static uint8_t ref_mem[1u << 20];
    static char ref_json[VJO_QUEUE_TEXT_MAX + 1];
    VjoAnkiNote note;
    VjoLocalPages *pages = NULL;
    char misc[sizeof(game_name) + 24];
    const char *json;
    VjoConfig *c;
    int page = 0, ok;
    vjo_arena_init(&arena, ref_mem, sizeof(ref_mem));
    TEST_ASSERT((c = vjo_arena_alloc(&arena, sizeof(*c))) != NULL);
    *c = next_cfg;
    acfg = c;
    vjo_view_lock();
    const VjoLocalPages *p = vjo_view_pages(&page);
    ok = vjo_anki_note_from_entry(&arena, g_view.list, entry, &note) == 0;
    if (p && p->count > 1 && (pages = vjo_arena_alloc(&arena, sizeof(*p)))) *pages = *p;
    vjo_view_unlock();
    TEST_ASSERT(ok);
    card_facts(&note, misc, sizeof(misc));
    read_more(&note, pages, page);
    if (more) *more = note.n_more;
    json = vjo_queue_encode(&arena, &note, picture);
    *peak = arena.peak;
    if (json) snprintf(ref_json, sizeof(ref_json), "%s", json);
    vjo_arena_init(&arena, NULL, 0);
    acfg = NULL;
    return json ? ref_json : NULL;
}

/* #528 (Mmem-lead-4): the arena at its bound by construction (ANKI_TEXT_MEM, then capped at ANKI_FULLEST itself):
 * cards at the inputs' bounds are the very records a 1 MiB arena makes. 8 dictionaries, every entry a whole payload (VJO_LOCAL_MAX_ENTRY,
 * the reader's largest) or half of one, shaped as kana, as text JSON escaping doubles (" \
 * and line breaks) or grows sixfold (control characters), or as an empty reading with a spelling the length of the
 * payload (an entry's strings twice); with a 63-byte title or with none (the "[name]" of the meaning), beside the
 * longest text the overlay shows (VJO_STRIP_MAX - 1 bytes). Each card's queue record, or its refusal, is the
 * reference's, byte for byte. */
static void test_the_arena_bound_keeps_every_card_whatever_its_entries(void)
{
    static char text[VJO_STRIP_MAX];
    static char spell[8][VJO_LOCAL_MAX_ENTRY], meaning[8][VJO_LOCAL_MAX_ENTRY], names[8][16], titles[8][64],
        list_names[160];
    static DictRow rows[8];
    static uint8_t mem[256 * 1024];
    static VjoLocalPages pages;
    static const char *const shapes[] = {"kana", "doubled by escaping", "six times by escaping", "empty reading"};
    const size_t whole = VJO_LOCAL_MAX_ENTRY; /* the reader's largest payload (core/local_dict.h) */
    size_t worst_ref = 0, worst_job = 0;
    unsigned cases = 0;
    /* the overlay's longest text: the word, then 。 to its last byte */
    size_t t = (size_t)snprintf(text, sizeof(text), "猫が好き猫");
    while (t + 3 < sizeof(text)) { memcpy(text + t, "。", 3); t += 3; }
    text[t] = '\0';
    for (unsigned capped = 0; capped < 2; capped++) /* the arena as it is, then only its bound (#532's host hook) */
    for (unsigned shape = 0; shape < 4; shape++)
        for (unsigned half = 0; half < 2; half++)
            for (unsigned titled = 0; titled < 2; titled++) {
                size_t payload = half ? whole / 2 : whole, job_peak, ref_peak;
                VjoArena a;
                VjoPlatform p;
                VjoDictResult res;
                VjoErr err;
                const char *json = NULL, *ref;
                char id[VJO_QUEUE_ID_SIZE];
                int saved;
                reset();
                list.header = text;
                list_names[0] = '\0';
                for (int k = 0; k < 8; k++) {
                    /* the payload's three strings and their NULs: payload bytes in all */
                    size_t room = payload - 3, o;
                    int ns = snprintf(meaning[k], sizeof(meaning[k]), "[Dictionary %d]\n", k);
                    if (shape == 3) { /* an empty reading: the spelling is most of the payload, the meaning short */
                        o = (size_t)snprintf(spell[k], sizeof(spell[k]), "好き");
                        while (o + 3 <= room - (size_t)ns) { memcpy(spell[k] + o, "す", 3); o += 3; }
                        spell[k][o] = '\0';
                    } else {
                        snprintf(spell[k], sizeof(spell[k]), "好き");
                        o = (size_t)ns;
                        while (o + 3 <= room - strlen("好き") * 2) {
                            const char *piece = shape == 0 ? "す" : shape == 1 ? "\"\n\\" : "\x01\x02\x03";
                            memcpy(meaning[k] + o, piece, 3);
                            o += 3;
                        }
                        meaning[k][o] = '\0';
                    }
                    snprintf(names[k], sizeof(names[k]), "d%d.vjdict", k);
                    snprintf(titles[k], sizeof(titles[k]), "%.63s",
                             "Bee's Ultimate Dictionary of very long titles for this card #528");
                    rows[k] = (DictRow){"好き", spell[k], shape == 3 ? "" : "すき", meaning[k], titled ? titles[k] : NULL};
                    dict_build(&dict_files[k], names[k], &rows[k]);
                    strcat(list_names, k ? "," : "");
                    strcat(list_names, names[k]);
                }
                snprintf(cfg.local_dictionary_dir, sizeof(cfg.local_dictionary_dir), "fixture");
                snprintf(cfg.local_dictionaries, sizeof(cfg.local_dictionaries), "%s", list_names);
                reconfigure();
                vjo_arena_init(&a, mem, sizeof(mem));
                vjo_platform_vita(&p);
                TEST_ASSERT(vjo_local_lookup_pages(&a, &p, &cfg, list.header, 6, 8, &pages, &res, &err) == VJO_OK);
                TEST_ASSERT(pages.count == 8 && res.n_vocab == 1);
                entries[1].vocab = &res.vocab[0];
                view_pages = &pages;
                view_page = 0;
                if (capped) { /* the job's arena holds only ANKI_FULLEST bytes, the bound itself */
                    vjo_test_arena_base = heap;
                    vjo_test_arena_cap = ANKI_FULLEST;
                }
                enter(1);
                vjo_test_arena_base = NULL;
                vjo_test_arena_cap = (size_t)-1;
                job_peak = last_peak;
                saved = !strcmp(g_view.anki_status, "Saved for Anki");
                if (saved) {
                    only_id(id);
                    json = queue_file(id, "json", NULL);
                }
                ref = reference_record(1, 0, &ref_peak, NULL);
                TEST_CHECK_(saved == (ref != NULL), "%s, %s payload, %s: the job %s, the reference %s", shapes[shape],
                            half ? "half a" : "a whole", titled ? "titled" : "no title", g_view.anki_status,
                            ref ? "makes a record" : "makes none");
                TEST_CHECK_(!saved || (json && ref && !strcmp(json, ref)),
                            "%s, %s payload, %s: the record differs from the 1 MiB arena's (%u and %u B)", shapes[shape],
                            half ? "half a" : "a whole", titled ? "titled" : "no title", json ? (unsigned)strlen(json) : 0u,
                            ref ? (unsigned)strlen(ref) : 0u);
                TEST_CHECK(job_peak <= (capped ? ANKI_FULLEST : ANKI_TEXT_MEM));
                if (ref_peak > worst_ref) worst_ref = ref_peak;
                if (job_peak > worst_job) worst_job = job_peak;
                cases++;
                printf("measure: bound, %s, %s payload of %u B, %s: %s, record %u B, peak %u in the job's %u B (%u in "
                       "1 MiB)\n", shapes[shape], half ? "half a" : "a whole", (unsigned)payload,
                       titled ? "titled" : "no title", saved ? "saved" : g_view.anki_status,
                       json ? (unsigned)strlen(json) : 0u, (unsigned)job_peak,
                       (unsigned)(capped ? ANKI_FULLEST : ANKI_TEXT_MEM), (unsigned)ref_peak);
                finish();
            }
    printf("measure: the arena's bound by construction %u B of its %u B; %u cards at the inputs' bounds, in both: "
           "the job's peak %u B, the 1 MiB arena's %u B\n", (unsigned)ANKI_FULLEST, (unsigned)ANKI_TEXT_MEM, cases,
           (unsigned)worst_job, (unsigned)worst_ref);
}

/* audit-mem-leak-2 F2: the cards the grid cannot reach. A case: n_dicts dictionaries, each with per_dict entries of
 * the shown spelling (more than one: its other readings, read_more's search), the spelling a 192-byte key (key192:
 * its entries under that key, the word's key listing the first), each meaning of kana meaning_len bytes ("[name]\n"
 * and the entry's number included) and the last dictionary's last_len (0: the same), titled or not, beside a text of
 * text_len bytes. */
typedef struct {
    int n_dicts, per_dict, key192, titled;
    size_t meaning_len, last_len, text_len;
} BoundCase;
typedef struct { int saved, same, more; size_t record, job_peak, ref_peak; } BoundRun;
static char bound_text[VJO_STRIP_MAX], bound_key192[200];
static char bound_meaning[8][8][VJO_LOCAL_MAX_ENTRY];
static char bound_names[8][24], bound_titles[8][64], bound_list[256];
static uint8_t bound_lookup_mem[256 * 1024];
static VjoLocalPages bound_pages;
static VjoDictResult bound_res;

/* head, then す while it fits in len bytes, then 'a' to len exactly: a record tuned byte by byte */
static void bound_fill(char *out, size_t len, const char *head)
{
    size_t o = (size_t)snprintf(out, VJO_LOCAL_MAX_ENTRY, "%s", head);
    while (o + 3 <= len) { memcpy(out + o, "す", 3); o += 3; }
    while (o < len) out[o++] = 'a';
    out[o] = '\0';
}

/* The case's dictionaries made, its text shown and its word looked up, as the grid's cards are: 0, else -1. */
static int bound_setup(const BoundCase *c)
{
    VjoArena a;
    VjoPlatform p;
    VjoErr err;
    const char *spelling;
    size_t t, room;
    reset();
    t = (size_t)snprintf(bound_text, sizeof(bound_text), "猫が好き猫");
    while (t + 3 <= c->text_len) { memcpy(bound_text + t, "。", 3); t += 3; }
    bound_text[t] = '\0';
    list.header = bound_text;
    t = (size_t)snprintf(bound_key192, sizeof(bound_key192), "好き");
    while (t + 3 <= 192) { memcpy(bound_key192 + t, "す", 3); t += 3; }
    while (t < 192) bound_key192[t++] = 'a';
    bound_key192[t] = '\0';
    spelling = c->key192 ? bound_key192 : "好き";
    room = VJO_LOCAL_MAX_ENTRY - strlen(spelling) - strlen("すき") - 3; /* a payload's three strings and NULs */
    bound_list[0] = '\0';
    if (c->n_dicts < 1 || c->n_dicts > 8 || c->per_dict < 1 || c->per_dict > 8) return -1;
    for (int k = 0; k < c->n_dicts; k++) {
        VjdictEntry e[8];
        const VjdictKey keys[2] = {{"好き", 0, c->key192 ? 1 : c->per_dict}, {bound_key192, 0, c->per_dict}};
        size_t len = k == c->n_dicts - 1 && c->last_len ? c->last_len : c->meaning_len;
        if (len > room) len = room;
        for (int j = 0; j < c->per_dict; j++) {
            char head[40];
            snprintf(head, sizeof(head), "[Dictionary %d]\n%d", k, j); /* the entries differ */
            bound_fill(bound_meaning[k][j], len, head);
            e[j] = (VjdictEntry){spelling, "すき", bound_meaning[k][j]};
        }
        snprintf(bound_names[k], sizeof(bound_names[k]), "b%d.vjdict", k);
        snprintf(bound_titles[k], sizeof(bound_titles[k]), "%.63s",
                 "Bee's Ultimate Dictionary of very long titles for this card #528");
        memset(&dict_files[k], 0, sizeof(dict_files[k]));
        dict_files[k].name = bound_names[k];
        dict_files[k].size = vjdict2_keys(dict_files[k].bytes, sizeof(dict_files[k].bytes), keys, c->key192 ? 2 : 1, e,
                                          c->per_dict, c->titled ? bound_titles[k] : NULL);
        if (!dict_files[k].size) return -1;
        strcat(bound_list, k ? "," : "");
        strcat(bound_list, bound_names[k]);
    }
    snprintf(cfg.local_dictionary_dir, sizeof(cfg.local_dictionary_dir), "fixture");
    snprintf(cfg.local_dictionaries, sizeof(cfg.local_dictionaries), "%s", bound_list);
    reconfigure();
    vjo_arena_init(&a, bound_lookup_mem, sizeof(bound_lookup_mem));
    vjo_platform_vita(&p);
    if (vjo_local_lookup_pages(&a, &p, &cfg, list.header, 6, 8, &bound_pages, &bound_res, &err) != VJO_OK ||
        (int)bound_pages.count != c->n_dicts || bound_res.n_vocab != 1)
        return -1;
    entries[1].vocab = &bound_res.vocab[0];
    view_pages = &bound_pages;
    view_page = 0;
    return 0;
}

/* The 1 MiB arena's card of the case alone (no save): whether it makes a record, its size and the entries kept. */
static int bound_probe(const BoundCase *c, int *more, size_t *record)
{
    size_t peak;
    const char *ref;
    TEST_ASSERT(bound_setup(c) == 0);
    ref = reference_record(1, 0, &peak, more);
    *record = ref ? strlen(ref) : 0;
    finish();
    return ref != NULL;
}

/* The job's save of the case, its arena capped at cap (0: as it is), against the 1 MiB arena's: the same record, or
 * both none with the job's "over 32 KiB" refusal. */
static BoundRun bound_save(const BoundCase *c, size_t cap)
{
    BoundRun r = {0};
    char id[VJO_QUEUE_ID_SIZE], status[128];
    const char *json = NULL, *ref;
    TEST_ASSERT(bound_setup(c) == 0);
    if (cap) {
        vjo_test_arena_base = heap;
        vjo_test_arena_cap = cap;
    }
    enter(1);
    vjo_test_arena_base = NULL;
    vjo_test_arena_cap = (size_t)-1;
    r.job_peak = last_peak;
    snprintf(status, sizeof(status), "%s", g_view.anki_status);
    r.saved = !strcmp(status, "Saved for Anki");
    if (r.saved) {
        only_id(id);
        json = queue_file(id, "json", NULL);
    }
    ref = reference_record(1, 0, &r.ref_peak, &r.more);
    r.record = ref ? strlen(ref) : 0;
    r.same = r.saved == (ref != NULL) && (!r.saved || (json && !strcmp(json, ref))) &&
             (ref || !strcmp(status, "Card not saved: over 32 KiB of text"));
    TEST_CHECK_(r.job_peak <= (cap ? cap : ANKI_TEXT_MEM), "the job's peak %u B", (unsigned)r.job_peak);
    finish();
    return r;
}

static void bound_check(const char *what, const BoundCase *c)
{
    BoundRun full = bound_save(c, 0), capped = bound_save(c, ANKI_FULLEST);
    TEST_CHECK_(full.same && capped.same, "%s: the job's card differs from the 1 MiB arena's (%s, %s)", what,
                full.same ? "same" : "differs at 88 KiB", capped.same ? "same" : "differs capped at ANKI_FULLEST");
    printf("measure: bound, %s: %s, record %u B, %d more kept; peak %u B (88 KiB), %u B (capped at %u), %u B (1 MiB)\n",
           what, full.saved ? "saved" : "not saved", (unsigned)full.record, full.more, (unsigned)full.job_peak,
           (unsigned)capped.job_peak, (unsigned)ANKI_FULLEST, (unsigned)full.ref_peak);
}

/* The fullest cards that save: 8 dictionaries' entries of kana, all 7 more kept, the last dictionary's meaning tuned
 * byte by byte so the record is as near VJO_QUEUE_TEXT_MAX as it gets; then one byte more, which leaves that entry
 * out. Beside the shortest text and the longest, titled or not. Each is the 1 MiB arena's card in the 88 KiB arena
 * and capped at ANKI_FULLEST (a 60 KiB arena, keep_entry not moving its entry down over its read, or a record buffer
 * that cannot grow to the exact size in place each change one of these cards: audit-mem-leak-2 M1b, M3, M4). Then
 * the other readings: several entries of the spelling in each dictionary, and a 192-byte spelling's key. */
static void test_the_fullest_cards_that_save_are_the_same(void)
{
    static const BoundCase alternates[] = {
        /* n_dicts, per_dict, key192, titled, meaning_len, last_len, text_len */
        {8, 4, 0, 1, 8150, 0, VJO_STRIP_MAX - 1},
        {2, 8, 0, 0, 4000, 0, VJO_STRIP_MAX - 1},
        {2, 8, 1, 1, 4000, 0, VJO_STRIP_MAX - 1},
        {8, 3, 1, 0, 2000, 0, 15},
    };
    for (int longtext = 0; longtext < 2; longtext++)
        for (int titled = 0; titled < 2; titled++) {
            BoundCase c = {8, 1, 0, titled, 0, 0, longtext ? VJO_STRIP_MAX - 1 : 15};
            size_t lo = 200, hi = VJO_LOCAL_MAX_ENTRY, record = 0;
            int more = 0;
            char what[96];
            /* every meaning as long as all 7 more are still kept */
            while (hi - lo > 1) {
                c.meaning_len = (lo + hi) / 2;
                if (bound_probe(&c, &more, &record) && more == 7) lo = c.meaning_len;
                else hi = c.meaning_len;
            }
            c.meaning_len = lo;
            /* then the last one, to the record's last byte */
            hi = VJO_LOCAL_MAX_ENTRY;
            while (hi - lo > 1) {
                c.last_len = (lo + hi) / 2;
                if (bound_probe(&c, &more, &record) && more == 7) lo = c.last_len;
                else hi = c.last_len;
            }
            c.last_len = lo;
            TEST_ASSERT(bound_probe(&c, &more, &record) && more == 7);
            TEST_CHECK_(record + 8 > VJO_QUEUE_TEXT_MAX, "the fullest card's record is %u B", (unsigned)record);
            snprintf(what, sizeof(what), "the fullest card, text %u B, %s", (unsigned)c.text_len,
                     titled ? "titled" : "no title");
            bound_check(what, &c);
            c.last_len++;
            TEST_ASSERT(bound_probe(&c, &more, &record) && more == 6);
            snprintf(what, sizeof(what), "one byte more (the last entry left out), text %u B, %s",
                     (unsigned)c.text_len, titled ? "titled" : "no title");
            bound_check(what, &c);
        }
    for (unsigned i = 0; i < sizeof(alternates) / sizeof(alternates[0]); i++) {
        char what[96];
        snprintf(what, sizeof(what), "%d dictionaries x %d entries of %s, %u B meanings, text %u B, %s",
                 alternates[i].n_dicts, alternates[i].per_dict, alternates[i].key192 ? "a 192-byte spelling" : "it",
                 (unsigned)alternates[i].meaning_len, (unsigned)alternates[i].text_len,
                 alternates[i].titled ? "titled" : "no title");
        bound_check(what, &alternates[i]);
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

/* This process's open file descriptors (the queue's files are real ones here: a leaked sceIo descriptor is a
 * leaked host one), or -1 where /proc/self/fd cannot be read. */
static int open_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    int n = 0;
    if (!d) return -1;
    while (readdir(d)) n++;
    closedir(d);
    return n;
}

/* #528 (mem-leak): Anki mining soaked. A cycle is the overlay's open (its count of the queue), a save of a new card
 * (every other one with the game's screenshot, where the build has the game worker's client) and a save of a card
 * already queued; each cycle must end with the job's Paf arena and its thread gone (and out of the ledger), no file
 * descriptor more than the first ended with, and the queue one card longer. VJO_SOAK_CYCLES=N runs N cycles (100 by
 * default). */
static void test_soak_mining_holds_no_more_after_each_card(void)
{
    static char header[64];
    const unsigned cycles = soak_cycles(100);
    int fds0 = -1, queued = 0;
    reset();
    for (unsigned i = 0; i <= cycles; i++) {
        snprintf(header, sizeof(header), "猫が好き%u", i);
        list.header = header;
        g_view.list_seq = 1000 + i;
#ifdef VJO_WITH_VOCR
        cfg.anki_screenshot = (int)(i & 1u);
        reconfigure();
#endif
        vjo_anki_post_count(); /* the overlay opens: its queue line */
        vjo_anki_reap();
        enter(0);
        TEST_CHECK_(!strcmp(g_view.anki_status, "Saved for Anki"), "card %u: %s", i, g_view.anki_status);
        queued++;
        g_view.list_seq = 5000 + i; /* the same card from a new lookup: queued already */
        enter(0);
        TEST_CHECK_(!strcmp(g_view.anki_status, "Already queued"), "card %u again: %s", i, g_view.anki_status);
        TEST_CHECK_(!heap_live && heap_allocs == heap_frees && threads_created == threads_deleted && thread < 0,
                    "card %u: arena %d (%d/%d), threads %d/%d", i, heap_live, heap_allocs, heap_frees, threads_created,
                    threads_deleted);
        TEST_CHECK_(vjo_ledger_current(&g_shell_ledger, VJO_SL_PAF_ANKI) == 0 &&
                        vjo_ledger_current(&g_shell_ledger, VJO_SL_ANKI_STACK) == 0,
                    "card %u: the ledger holds %u B of arena and %u B of stack", i,
                    (unsigned)vjo_ledger_current(&g_shell_ledger, VJO_SL_PAF_ANKI),
                    (unsigned)vjo_ledger_current(&g_shell_ledger, VJO_SL_ANKI_STACK));
        if (!i) fds0 = open_fds();
        else if (fds0 >= 0) TEST_CHECK_(open_fds() == fds0, "card %u: %d descriptors open, %d after the first", i, open_fds(), fds0);
    }
    TEST_CHECK_(vjo_queue_count() == queued, "%d cards queued of %d", vjo_queue_count(), queued);
    fprintf(stderr, "soak shell: Anki mining %5u cycles: %d arenas and %d threads taken and given back, %d descriptors open "
            "after every cycle\n", cycles, heap_allocs, threads_created, fds0);
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
    TEST_CHECK(d.anki_screenshot_trim_bars == 1); /* API 32: on unless anki_screenshot_trim_bars = off */
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
    TEST_CHECK((jpg = queue_file(id, "jpg", &len)) && len == shot_jpeg_bytes && !memcmp(jpg, shot_jpeg, len));
    TEST_CHECK(shot_jobs == 1 && shot_reads == 1 && slot_begins == 1 && slot_ends == 1);
    TEST_CHECK(shot_speed == VJO_GAME_OCR_S_TRIM); /* API 32: its black bars trimmed (the default) */
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
    TEST_CHECK(logged("anki: screenshot bars 1 how 1: picture 484x276 at 238,134 of 960x544; scan 2100 us\n"));
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
    /* API 32: with anki_screenshot_trim_bars = off the shot asks for the whole frame (no speed bit), and the
     * debug build's log has no bars line */
    reset();
    screenshots_on();
    cfg.anki_screenshot_trim_bars = 0;
    reconfigure();
    enter(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && shot_jobs == 1 && shot_speed == 0);
    TEST_CHECK(!logged("anki: screenshot bars "));
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
    TEST_CHECK((jpg = queue_file(id, "jpg", &len)) && len == shot_jpeg_bytes && !memcmp(jpg, shot_jpeg, len));
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

/* Line history (docs/line-history.md): a card from an earlier line (the view shows the k-th earlier
 * one) takes no screenshot: the screen shows another line now, and Rubi keeps no frame of a line, so
 * a screenshot now would be of another screen. Its sentence is that line, and the status says it has
 * none; with screenshots off it says what any card's does. The current line's card still has one. */
static void test_an_earlier_lines_card_has_no_screenshot(void)
{
    char id[VJO_QUEUE_ID_SIZE];
    size_t len;
    const char *json;
    reset();
    screenshots_on();
    list.header = "前の猫が好き猫"; /* the earlier line, as worker.c history_show publishes it (local_text) */
    entries[0].hl_start = 6, entries[0].hl_end = 9;
    g_view.history = 2;
    enter(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki (an earlier line: no screenshot)") &&
               g_view.anki_mark[0] == VJO_ANKI_MARK_QUEUED);
    TEST_MSG("%s", g_view.anki_status);
    TEST_CHECK(!slot_begins && !shot_jobs && files_ending(".json") == 1 && files_ending(".jpg") == 0);
    only_id(id);
    TEST_CHECK((json = queue_file(id, "json", &len)) && strstr(json, "\"picture\":false") && strstr(json, "前の猫が好き猫"));
    /* no OCR model named: the last recognition's (VOCR3_H15 here) may not be the one that read that line */
    TEST_CHECK(json && strstr(json, "\"ocr_model\":null") && strstr(json, "\"ocr_engine\":\"vocr\""));
    TEST_MSG("%s", json ? json : "(none)");
    TEST_CHECK(!heap_live && heap_allocs == heap_frees);
    finish();
    /* screenshots off: as any card */
    reset();
    g_view.history = 1;
    enter(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && !shot_jobs && files_ending(".jpg") == 0);
    finish();
    /* the current line (history 0): its screenshot, as before line history */
    reset();
    screenshots_on();
    g_view.history = 0;
    enter(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && shot_jobs == 1 && files_ending(".jpg") == 1);
    only_id(id);
    TEST_CHECK((json = queue_file(id, "json", &len)) && strstr(json, "\"ocr_model\":\"VOCR3_H15_w8.vocr\""));
    finish();
}
#endif

/* ---------------- #528: the overlay's next lookup during a save ---------------- */
/* A card is the record of the entry shown when Enter was pressed, whatever the overlay does while it is saved. The
 * shown list's frequency source (the list's name, "Jiten" for the bundled dictionary) is in the lookup's memory, which
 * the overlay's next lookup writes over (shell/worker.c run_cursor_job: the cursor on another word, a text replaced
 * by a refresh, a new read). Until 0.10.2 the card kept a pointer to it: a lookup before the record was named gave
 * the card that lookup's bytes as its source; one between its name and its write, during the screenshot, a record
 * that is not its name's, which the Rubi app's export refused ("checksum mismatch") and settings/anki_export.c now
 * reads again (host/tests/test_apkg.c, every released writer's record). */
static char lookup_source[64];
static void lookup_writes_over_the_source(void)
{
    memset(lookup_source, 0, sizeof(lookup_source));
    memcpy(lookup_source, "to come; to arrive", 18); /* another entry's meaning, where the name was */
}
static void save_while_a_lookup_runs(int screenshot, int at_shot)
{
    char id[VJO_QUEUE_ID_SIZE], actual[VJO_QUEUE_ID_SIZE];
    const char *json;
    size_t len;
    reset();
    if (screenshot) {
        cfg.anki_screenshot = 1;
        reconfigure();
    }
    snprintf(lookup_source, sizeof(lookup_source), "Jiten");
    list.source = lookup_source;
    if (at_shot)
        on_shot = lookup_writes_over_the_source;
    else
        on_clock = lookup_writes_over_the_source;
    enter(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki"));
    TEST_MSG("%s", g_view.anki_status);
    TEST_CHECK(shot_jobs == screenshot);
    only_id(id);
    TEST_ASSERT((json = queue_file(id, "json", &len)) != NULL);
    vjo_queue_id(json, len, actual);
    TEST_CHECK_(!strcmp(actual, id), "%s (the lookup %s): named %s, its checksum %s", screenshot ? "with a screenshot" :
                "a text card", at_shot ? "during the screenshot" : "before its name", id, actual);
    TEST_CHECK_(strstr(json, "\"source\":\"Jiten\",") != NULL, "%s", json);
    list.source = NULL;
    finish();
}
static void test_a_lookup_during_the_save_changes_nothing(void)
{
    save_while_a_lookup_runs(0, 0); /* before its name: the card's own source, not the lookup's bytes */
    save_while_a_lookup_runs(1, 0);
    save_while_a_lookup_runs(1, 1); /* during the screenshot: the record its name's */
}

/* ---------------- #528: the save with its arena cut at every block (audit-mem-crash-1's squeeze) ---------------- */
/* core/arena.c's host-only cap (host/tests/test_oom_paths.c): the arena on this file's Paf block holds only the bytes
 * before a block's end. Every save the full run's record, the same record with fewer of the other dictionaries'
 * entries, or a refusal that saves nothing: never another record, never a crash. */
extern const void *vjo_test_arena_base;
extern size_t vjo_test_arena_cap, vjo_test_arena_ends[4096];
extern unsigned vjo_test_arena_n_ends;

/* The record without its "more" array (the other dictionaries' entries), and how many it has. */
static void split_more(const char *json, char *rest, size_t cap, int *more)
{
    const char *m = strstr(json, ",\"more\":["), *p;
    int depth = 0, in = 0;
    *more = 0;
    if (!m) { snprintf(rest, cap, "%s", json); return; }
    for (p = m + 8; *p; p++) {
        if (in) { if (*p == '\\') p++; else if (*p == '"') in = 0; continue; }
        if (*p == '"') in = 1;
        else if (*p == '[' || *p == '{') { if (*p == '{' && depth == 1) (*more)++; depth++; }
        else if (*p == ']' || *p == '}') { if (--depth == 0) { p++; break; } }
    }
    snprintf(rest, cap, "%.*s%s", (int)(m - json), json, p);
}

/* test_what_enter_saves_whatever_the_dictionaries' card: 8 dictionaries with entries of `size` bytes (kana, or text
 * whose escaping grows), the screenshot with or without. */
static int squeeze_case(int heavy, int size, int with, VjoLocalPages *pages, VjoArena *a, uint8_t *mem, size_t mem_size)
{
    static char meaning[8][7200], names[8][16], list_names[160];
    static DictRow rows[8];
    VjoPlatform p;
    VjoDictResult res;
    VjoErr err;
    reset();
    list_names[0] = '\0';
    for (int k = 0; k < 8; k++) {
        int o = snprintf(meaning[k], sizeof(meaning[k]), "[Dictionary %d]\n", k);
        while (o + 3 < size) {
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
    cfg.anki_screenshot = with;
    reconfigure();
    vjo_arena_init(a, mem, mem_size);
    vjo_platform_vita(&p);
    if (vjo_local_lookup_pages(a, &p, &cfg, list.header, 6, 8, pages, &res, &err) != VJO_OK || pages->count != 8)
        return -1;
    entries[1].vocab = &res.vocab[0];
    view_pages = pages;
    view_page = 0;
    return 0;
}

static void test_a_save_with_its_arena_cut_at_every_block(void)
{
    static const int sizes[] = {300, 2700, 4000, 7000};
    static uint8_t mem[256 * 1024];
    static VjoLocalPages pages;
    static size_t ends[4096];
    static char full_json[64 * 1024], full_rest[64 * 1024], rest[64 * 1024];
    unsigned total = 0, refused = 0, same = 0, fewer = 0, other = 0;
    for (int heavy = 0; heavy < 2; heavy++)
        for (unsigned z = 0; z < sizeof(sizes) / sizeof(sizes[0]); z++)
            for (int with = 0; with < 2; with++) {
                VjoArena a;
                char id[VJO_QUEUE_ID_SIZE];
                const char *json;
                unsigned n, m = 0;
                int full_more = 0, more = 0;
                TEST_ASSERT(squeeze_case(heavy, sizes[z], with, &pages, &a, mem, sizeof(mem)) == 0);
                vjo_test_arena_base = heap;
                vjo_test_arena_cap = (size_t)-1;
                vjo_test_arena_n_ends = 0;
                enter(1);
                vjo_test_arena_base = NULL;
                TEST_CHECK_(!strcmp(g_view.anki_status, "Saved for Anki"), "the full save: %s", g_view.anki_status);
                only_id(id);
                TEST_ASSERT((json = queue_file(id, "json", NULL)) != NULL);
                snprintf(full_json, sizeof(full_json), "%s", json);
                split_more(full_json, full_rest, sizeof(full_rest), &full_more);
                n = vjo_test_arena_n_ends;
                memcpy(ends, vjo_test_arena_ends, n * sizeof(ends[0]));
                finish();
                for (unsigned i = 0; i < n; i++) {
                    unsigned j;
                    for (j = 0; j < m && ends[j] != ends[i]; j++) {}
                    if (j == m) ends[m++] = ends[i];
                }
                for (unsigned i = 0; i < m; i++) {
                    TEST_ASSERT(squeeze_case(heavy, sizes[z], with, &pages, &a, mem, sizeof(mem)) == 0);
                    vjo_test_arena_base = heap;
                    vjo_test_arena_cap = ends[i] - 1;
                    enter(1);
                    vjo_test_arena_base = NULL;
                    vjo_test_arena_cap = (size_t)-1;
                    total++;
                    if (!strcmp(g_view.anki_status, "Saved for Anki")) {
                        only_id(id);
                        TEST_ASSERT((json = queue_file(id, "json", NULL)) != NULL);
                        split_more(json, rest, sizeof(rest), &more);
                        if (!strcmp(json, full_json)) same++;
                        else if (!strcmp(rest, full_rest) && more < full_more) fewer++;
                        else {
                            other++;
                            TEST_CHECK_(0, "cut at %zu (%d B entries%s%s): saved another record", ends[i] - 1, sizes[z],
                                        heavy ? ", escaping" : "", with ? ", screenshot" : "");
                        }
                    } else if (strstr(g_view.anki_status, "not enough memory") ||
                               strstr(g_view.anki_status, "Card not saved: over")) {
                        refused++;
                        TEST_CHECK_(files_ending(".json") == 0, "cut at %zu: a refused save left a record", ends[i] - 1);
                    } else if (strstr(g_view.anki_status, "without a screenshot")) {
                        fewer++; /* the record saved, its screenshot not (told) */
                    } else {
                        other++;
                        TEST_CHECK_(0, "cut at %zu: status \"%s\"", ends[i] - 1, g_view.anki_status);
                    }
                    finish();
                }
            }
    printf("\n  a save with its arena cut: %u cut runs: %u refused, %u the full record, %u fewer others or no "
           "screenshot, %u other\n", total, refused, same, fewer, other);
}

/* audit-mem-leak-4 (#569), Maudit-mem-leak-4-3: each alternates case reaches read_more's search and its card keeps
 * other readings; and the fullest card made of the spelling's other readings (n dictionaries x 8 entries: dictionary
 * 0's 7 others fill the card) is the same record in the bound's arena as in a 1 MiB one (bound_check). */
static void test_alternates_reach_the_search_and_the_card(void)
{
    static const BoundCase cases[] = {
        {8, 4, 0, 1, 8150, 0, VJO_STRIP_MAX - 1}, {2, 8, 0, 0, 4000, 0, VJO_STRIP_MAX - 1},
        {2, 8, 1, 1, 4000, 0, VJO_STRIP_MAX - 1}, {8, 3, 1, 0, 2000, 0, 15},
    };
    static uint8_t mem[128 * 1024];
    static VjoLocalPage out[VJO_LOCAL_MAX_RESULTS];
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const BoundCase *c = &cases[i];
        VjoArena a;
        VjoPlatform p;
        int full = 0, n, more = 0, alt_kept = 0;
        size_t peak;
        const char *ref;
        TEST_ASSERT(bound_setup(c) == 0);
        vjo_arena_init(&a, mem, sizeof(mem));
        vjo_platform_vita(&p);
        n = vjo_local_alternates(&a, &p, &cfg, &bound_pages, out, VJO_LOCAL_MAX_RESULTS, &full);
        ref = reference_record(1, 0, &peak, &more);
        /* an entry j >= 1 of a dictionary on the card is one of its other readings ("[Dictionary k]\n<j>") */
        for (int k = 0; k < c->n_dicts && ref; k++)
            for (int j = 1; j < c->per_dict; j++) {
                char needle[48];
                snprintf(needle, sizeof(needle), "\"[Dictionary %d]\\n%d", k, j);
                alt_kept += strstr(ref, needle) != NULL;
            }
        printf("%d x %d%s: search located %d (expected %d), full %d; card keeps %d more, %d of them other "
               "readings\n", c->n_dicts, c->per_dict, c->key192 ? " (192-byte spelling)" : "", n,
               c->n_dicts * (c->per_dict - 1), full, more, alt_kept);
        TEST_CHECK(n == c->n_dicts * (c->per_dict - 1) && alt_kept > 0);
        finish();
    }
}

/* The least cap (8-byte steps) at which case c's card is the 1 MiB arena's. */
static size_t fullest_least(const BoundCase *c)
{
    size_t cl = 4096, ch = ANKI_FULLEST;
    if (!bound_save(c, ch).same) return 0;
    while (ch - cl > 8) {
        size_t mid = (cl + (ch - cl) / 2) & ~(size_t)7;
        if (bound_save(c, mid).same) ch = mid; else cl = mid;
    }
    return ch;
}

/* The fullest card of other readings: n x 8 entries of the spelling, every meaning as long as all 7 others
 * (dictionary 0's other readings) are kept, the record within 8 B of VJO_QUEUE_TEXT_MAX; the search's upper end
 * (4,100 B) keeps 8 such payloads within DictFile's 43,008 B. Short text, titled or not. */
static void test_the_fullest_card_of_other_readings(void)
{
    static const int dicts[] = {2, 4, 8};
    for (unsigned di = 0; di < 3; di++)
    for (int titled = 0; titled < 2; titled++)
        for (int key192 = 0; key192 < 2; key192++) {
            BoundCase c = {dicts[di], 8, key192, titled, 0, 0, 15};
            size_t lo = 200, hi = 4100, record = 0, least;
            int more = 0;
            char what[160];
            while (hi - lo > 1) {
                c.meaning_len = (lo + hi) / 2;
                if (bound_probe(&c, &more, &record) && more == 7) lo = c.meaning_len;
                else hi = c.meaning_len;
            }
            c.meaning_len = lo;
            TEST_ASSERT(bound_probe(&c, &more, &record) && more == 7);
            snprintf(what, sizeof(what), "the fullest card of other readings (%d x 8%s, %s), meanings %u B, "
                     "%d located", c.n_dicts, key192 ? ", a 192-byte spelling" : "", titled ? "titled" : "no title",
                     (unsigned)lo, c.n_dicts * 7);
            bound_check(what, &c);
            least = fullest_least(&c);
            printf("least arena, %s: %u B\n", what, (unsigned)least);
        }
}


TEST_LIST = {
    {"alternates reach the search and the card (audit-mem-leak-4)", test_alternates_reach_the_search_and_the_card},
    {"the fullest card of other readings fits the bound (audit-mem-leak-4)", test_the_fullest_card_of_other_readings},
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
    {"a_save_with_its_arena_cut_at_every_block", test_a_save_with_its_arena_cut_at_every_block},
    {"#528 the arena's bound keeps every card whatever its entries", test_the_arena_bound_keeps_every_card_whatever_its_entries},
    {"#528 the fullest cards that save are the same in the bound", test_the_fullest_cards_that_save_are_the_same},
    {"record_round_trip_stable_ids_and_strict_decoding", test_record_format},
    {"fifty_cards_release_memory_and_threads", test_fifty_cards_release_everything},
    {"#528 soak: mining holds no more after each card", test_soak_mining_holds_no_more_after_each_card},
    {"a_refused_job_thread_is_logged_with_its_rc_and_sceshell_free", test_a_refused_job_thread_is_logged},
#ifdef VJO_WITH_VOCR
    {"api23_enter_saves_the_screenshot_beside_the_card", test_enter_saves_the_screenshot_beside_the_card},
    {"api23_a_failed_screenshot_saves_the_card_without_one", test_a_failed_screenshot_saves_the_card_without_one},
    {"api23_a_failed_record_takes_its_screenshot_with_it", test_a_failed_record_takes_its_screenshot_with_it},
    {"api23_a_captured_shot_finishes_after_overlay_closes", test_a_captured_shot_finishes_after_overlay_closes},
    {"api23_an_interrupted_saves_jpeg_is_replaced", test_an_interrupted_saves_jpeg_is_replaced},
    {"api23_no_screenshot_is_asked_for_when_off_or_without_a_game", test_no_screenshot_is_asked_for_when_off_or_without_a_game},
    {"line_history_an_earlier_lines_card_has_no_screenshot", test_an_earlier_lines_card_has_no_screenshot},
#endif
    {"#528 a lookup during the save changes nothing in the card's record", test_a_lookup_during_the_save_changes_nothing},
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
        const char *real = getenv("VJO_TEST_SHOT_JPEG"); /* the actual game worker's JPEG (test_card_screenshot.py) */
        if (real && load_shot_jpeg(real) < 0)
            return 2;
        cfg.anki_screenshot = 1;
        what = what[9] ? "save-other" : "save";
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
