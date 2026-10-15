/* Actual Shell Anki miner (shell/anki.c), its offline file queue
 * (shell/anki_queue.c, core/anki_queue.c) and its screenshot path
 * (shell/capture.c, the real JPEG encoder over a synthetic 960x544 game
 * frame). Only Vita threads, locks, the Paf heap, the kernel capture and
 * sockets are substituted. The queue's files are real files in a temporary
 * directory, through the Vita file API mapped onto POSIX, with failed and
 * short writes, flushes, renames, removals and directory reads injected. A
 * started job thread runs to completion inside sceKernelStartThread (or
 * later, with defer_start), as the real thread does before the control
 * thread joins it. AnkiConnect is either a script of replies or a small fake
 * server that keeps notes by their vita_queue_ tag. Lines starting
 * "measure:" are the arena peaks quoted in docs/anki.md.
 *
 * `--restart-step DIR ACTION [CRASH] [HOST]` runs one step of
 * test_anki_restart.py in a fresh process instead (real sockets to HOST). */
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
#include "anki.h"
#include "base64.h"
#include "json.h"
#include "net_posix.h"
#define VJO_TEST_MUTEX_STUBS 1
#define VJO_TEST_THREAD_STUBS 1
#define VJO_TEST_CREATE_STUBS 1
#include <psp2/host_stubs.h>
#include <psp2/io/fcntl.h>
#include <psp2/rtc.h>
#include "../../shell/shell.h"

/* ---------------- kernel capture of the game frame ---------------- */

#define FRAME_W 960
#define FRAME_H 544
static uint8_t frame[FRAME_W * FRAME_H * 4];
static VjoState cap_state;
static int capture_rc; /* < 0: vjoRequestCapture fails with it */
static unsigned captures, rows_read, discards;
static uint32_t capture_flags;
static uint64_t test_now;

uint64_t sceKernelGetProcessTimeWide(void) { return test_now += 1000; }
int sceKernelSetEventFlag(SceUID uid, unsigned bits) { return 0; }
int vjoGetState(VjoState *out) { *out = cap_state; return 0; }
int vjoRequestCapture(uint32_t flags)
{
    if (flags == VJO_CAPTURE_DISCARD) {
        discards++;
        return 0;
    }
    capture_flags = flags;
    if (capture_rc < 0)
        return capture_rc;
    captures++;
    cap_state.size = sizeof(cap_state);
    cap_state.done_seq = captures;
    cap_state.width = FRAME_W;
    cap_state.height = FRAME_H;
    cap_state.raw_stride = FRAME_W * 4;
    cap_state.capture_result = 0;
    return (int)captures;
}
int vjoWaitEvent(uint32_t mask, uint32_t *bits, uint32_t timeout) { *bits = 0; return 0; }
int vjoReadRaw(uint32_t row, uint32_t n, void *dst)
{
    if (row >= FRAME_H)
        return -1;
    if (n > FRAME_H - row)
        n = FRAME_H - row;
    memcpy(dst, frame + (size_t)row * FRAME_W * 4, (size_t)n * FRAME_W * 4);
    rows_read += n;
    return (int)n;
}

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

/* capture.c and anki.c both have a static now_us. */
#define now_us capture_now_us
#include "../../shell/capture.c"
#undef now_us

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
    return !strcmp(name, "VjoAnkiBox") ? 91 : !strcmp(name, "VjoNetCancel") ? 92 : 93;
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
    TEST_CHECK(!strcmp(name, "VjoAnki") && stack == 0x10000);
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

static uint8_t heap[512 * 1024] __attribute__((aligned(16)));
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
static char directory_path[256], test_dir[128];
static int write_budget = -1, short_io, fail_rename, fail_sync, fail_remove, fail_close, fail_scan;
static int fail_json_write; /* the JPEG commits, the record does not */
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
int sceIoOpen(const char *p, int flags, unsigned mode)
{
    int fd = open(p, flags, mode);
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
    if (!write_budget || (fail_json_write && ends_with(write_path, ".json.tmp")))
        return -1;
    if (write_budget > 0 && n > (size_t)write_budget)
        n = (size_t)write_budget;
    if (short_io && n > 7)
        n = 7;
    if (write_budget > 0)
        write_budget -= (int)n;
    int wrote = (int)write(fd, p, n);
    if (wrote > 0 && fd == write_fd) {
        if (ends_with(write_path, ".jpg.tmp")) crash_at("jpeg_partial");
        if (ends_with(write_path, ".json.tmp")) crash_at("json_partial");
    }
    return wrote;
}
SceOff sceIoLseek(int fd, SceOff off, int whence) { return lseek(fd, off, whence); }
int sceIoMkdir(const char *p, unsigned mode) { return mkdir(p, mode); }
int sceIoGetstat(const char *p, SceIoStat *st)
{
    struct stat s;
    int rc = stat(p, &s);
    if (!rc) st->st_mode = s.st_mode;
    return rc;
}
int sceIoRename(const char *from, const char *to)
{
    int rc = fail_rename ? -1 : rename(from, to);
    if (!rc) {
        snprintf(last_rename, sizeof(last_rename), "%s", to);
        if (ends_with(to, ".jpg")) crash_at("jpeg_renamed");
        if (ends_with(to, ".json")) crash_at("json_renamed");
    }
    return rc;
}
int sceIoRemove(const char *p)
{
    int rc;
    if (ends_with(p, ".json")) crash_at("before_json_remove");
    if (ends_with(p, ".jpg")) crash_at("before_jpeg_remove");
    rc = fail_remove ? -1 : unlink(p);
    if (!rc && ends_with(p, ".json")) crash_at("after_json_remove");
    return rc;
}
int sceIoSyncByFd(int fd, int flag)
{
    int rc = fail_sync ? -1 : fsync(fd);
    if (!rc && fd == write_fd) {
        if (ends_with(write_path, ".jpg.tmp")) crash_at("jpeg_flushed");
        if (ends_with(write_path, ".json.tmp")) crash_at("json_flushed");
    }
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

/* ---------------- files, LAN and sockets ---------------- */

static char host_file[128];
static int host_file_writes;
char *vjo_file_read(VjoArena *a, const char *path, size_t *len)
{
    TEST_CHECK(!strcmp(path, VJO_ANKI_HOST_PATH));
    if (!host_file[0])
        return NULL;
    *len = strlen(host_file);
    return vjo_arena_strndup(a, host_file, *len);
}
int vjo_file_write(const char *path, const void *data, size_t len)
{
    TEST_CHECK(!"the Shell writes no file but the queue's"); /* anki_host.txt is the Overlay app's */
    host_file_writes++;
    return 0;
}

static unsigned cancels;

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
int vjo_net_cancel_init(VjoNetCancel *c)
{
    c->fd = -1;
    c->cancelled = 0;
    c->lock = sceKernelCreateMutex("VjoNetCancel", 0, 0, NULL);
    return c->lock < 0 ? -1 : 0;
}
void vjo_net_cancel(VjoNetCancel *c) { c->cancelled = 1; cancels++; }
void vjo_net_cancel_clear(VjoNetCancel *c) { c->cancelled = 0; }

/* AnkiConnect (and an audio source) on the LAN. Scripted: canned HTTP
 * responses in turn, NULL = nothing listening. Server: replies by action
 * (findNotes by tag, addNote keeping the note's vita_queue_ tag). What each
 * connection sent is kept (connection k in sent[k % MAX_CONNS]). */
#define MAX_CONNS 8
static struct {
    const char *responses[MAX_CONNS];
    int n, next, stop_at; /* stop_at: connection on which vjo_anki_stop begins */
    int server;
    char host[MAX_CONNS][64];
    int port[MAX_CONNS];
} net;
static char sent[MAX_CONNS][640 * 1024];

#define MAX_NOTES 160
static struct {
    char tag[MAX_NOTES][48];
    int n_notes, adds, finds, gets, versions, decks;
    int drop_add_reply;    /* the next addNote is kept but its reply is cut */
    const char *duplicate; /* an addNote whose body has this is a duplicate */
    int offline_at;        /* connection index from which nothing answers (-1: never) */
    int audio_offline;     /* the audio source (port 5050) refuses */
    const char *audio_list;
    char actions[2048];    /* "find add ..." */
} anki;

typedef struct {
    char *out;
    size_t out_len, out_cap;
    char in[4096];
    size_t in_len, pos;
    int answered, cut;
    const char *script;
} FakeConn;
static FakeConn fconn;

static void action(const char *what)
{
    if (strlen(anki.actions) + strlen(what) + 2 < sizeof(anki.actions)) {
        if (anki.actions[0])
            strcat(anki.actions, " ");
        strcat(anki.actions, what);
    }
}

static void respond(FakeConn *c, const char *json)
{
    c->in_len = (size_t)snprintf(c->in, sizeof(c->in),
                                 "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %lu\r\n\r\n%s",
                                 (unsigned long)strlen(json), json);
}

/* The fake AnkiConnect's reply to the request in c->out. */
static void serve(FakeConn *c)
{
    const char *body = strstr(c->out, "\r\n\r\n"), *t;
    char tag[48], json[2048];
    body = body ? body + 4 : "";
    if (!strncmp(c->out, "GET ", 4)) {
        anki.gets++;
        action("audio");
        respond(c, anki.audio_list ? anki.audio_list
                                   : "{\"type\":\"audioSourceList\",\"audioSources\":[{\"name\":\"a\","
                                     "\"url\":\"http://192.168.1.23:5050/su.opus\"}]}");
    } else if (strstr(body, "\"action\":\"version\"")) {
        anki.versions++;
        action("version");
        respond(c, "{\"result\": 6, \"error\": null}");
    } else if (strstr(body, "\"action\":\"findNotes\"")) {
        int n = 0, k;
        anki.finds++;
        action("find");
        t = strstr(body, "tag:vita_queue_");
        snprintf(tag, sizeof(tag), "%.43s", t ? t + 4 : "");
        k = snprintf(json, sizeof(json), "{\"result\": [");
        for (int i = 0; i < anki.n_notes; i++)
            if (!strcmp(anki.tag[i], tag))
                k += snprintf(json + k, sizeof(json) - k, "%s%lld", n++ ? ", " : "", 1791436200000ll + i);
        snprintf(json + k, sizeof(json) - k, "], \"error\": null}");
        respond(c, json);
    } else if (strstr(body, "\"action\":\"addNote\"")) {
        anki.adds++;
        action("add");
        if (anki.duplicate && strstr(body, anki.duplicate)) {
            respond(c, "{\"result\": null, \"error\": \"cannot create note because it is a duplicate\"}");
            return;
        }
        t = strstr(body, "\"vita_queue_");
        TEST_ASSERT(t && anki.n_notes < MAX_NOTES);
        snprintf(anki.tag[anki.n_notes], sizeof(anki.tag[0]), "%.43s", t + 1);
        snprintf(json, sizeof(json), "{\"result\": %lld, \"error\": null}", 1791436200000ll + anki.n_notes++);
        respond(c, json);
        if (anki.drop_add_reply) { /* the note is in Anki; its reply never arrives whole */
            anki.drop_add_reply = 0;
            c->in_len -= 12;
            c->cut = 1;
        }
    } else if (strstr(body, "\"action\":\"createDeck\"")) {
        anki.decks++;
        action("deck");
        respond(c, "{\"result\": 1700000000002, \"error\": null}");
    } else {
        action("?");
        respond(c, "{\"result\": null, \"error\": \"unsupported action\"}");
    }
}

static int fake_send(void *ctx, const void *p, size_t n)
{
    FakeConn *c = (FakeConn *)ctx;
    size_t k = c->out_cap - c->out_len < n ? c->out_cap - c->out_len : n;
    memcpy(c->out + c->out_len, p, k);
    c->out_len += k;
    c->out[c->out_len] = '\0';
    return (int)n;
}

static int fake_recv(void *ctx, void *p, size_t n)
{
    FakeConn *c = (FakeConn *)ctx;
    size_t k;
    if (!c->answered) {
        c->answered = 1;
        if (c->script) {
            c->in_len = strlen(c->script) < sizeof(c->in) ? strlen(c->script) : sizeof(c->in) - 1;
            memcpy(c->in, c->script, c->in_len);
        } else {
            serve(c);
        }
    }
    k = c->in_len - c->pos < n ? c->in_len - c->pos : n;
    memcpy(p, c->in + c->pos, k);
    c->pos += k;
    return k ? (int)k : c->cut ? VJO_E_HTTP : 0; /* a cut reply ends without an orderly close */
}

static void simulate_stop_begins(void);
static int fake_connect(void *ud, const char *host, int port, int timeout_us, int io_timeout_us, VjoConn *out)
{
    VjoNetCancel *c = (VjoNetCancel *)ud;
    int k = net.next, slot = k % MAX_CONNS;
    if (k == net.stop_at)
        simulate_stop_begins();
    if (c && c->cancelled)
        return VJO_E_CANCELLED;
    if (net.server ? (anki.offline_at >= 0 && k >= anki.offline_at) || (port == 5050 && anki.audio_offline)
                   : k >= net.n) {
        net.next++;
        return VJO_E_NET;
    }
    net.next++; /* a refused connection takes its turn too */
    if (!net.server && !net.responses[k])
        return VJO_E_NET;
    snprintf(net.host[slot], sizeof(net.host[slot]), "%s", host);
    net.port[slot] = port;
    memset(&fconn, 0, sizeof(fconn));
    fconn.out = sent[slot];
    fconn.out_cap = sizeof(sent[slot]) - 1;
    fconn.out[0] = '\0';
    fconn.script = net.server ? NULL : net.responses[k];
    out->ctx = &fconn;
    out->send = fake_send;
    out->recv = fake_recv;
    return VJO_OK;
}
static void fake_disconnect(void *ud, VjoConn *c) {}

/* Local dictionaries for the other pages a card records: small VJDICT2
 * files in memory (as test_local_cursor.c builds them), opened through the
 * platform's file_open as the Vita's local_open does. */
typedef struct {
    const char *key, *spelling, *reading, *meaning;
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

static int use_posix; /* restart steps: real sockets */
static PosixPlatform posix;
void vjo_platform_vita(VjoPlatform *p, VjoNetCancel *cancel)
{
    if (use_posix) {
        posix_platform_init(&posix, p);
        return;
    }
    memset(p, 0, sizeof(*p));
    p->ud = cancel;
    p->connect = fake_connect;
    p->disconnect = fake_disconnect;
    p->file_open = dict_open;
}

#include "../../shell/anki.c"
#include "../../shell/anki_queue.c"
#include "../../shell/ledger.c" /* the thread wrappers anki.c uses */

/* What vjo_anki_stop does on another thread before it joins the job. */
static void simulate_stop_begins(void)
{
    sceKernelLockMutex(box_lock, 1, NULL);
    started = 0;
    sceKernelUnlockMutex(box_lock, 1);
    vjo_net_cancel(&cancel);
}

/* ---------------- fixtures ---------------- */

static const char *ok(const char *json)
{
    static char bufs[MAX_CONNS][1024];
    static int k;
    char *b = bufs[k++ % MAX_CONNS];
    snprintf(b, sizeof(bufs[0]), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %lu\r\n\r\n%s",
             (unsigned long)strlen(json), json);
    return b;
}
#define NOTE_ID "{\"result\": 1700000000001, \"error\": null}"
#define EMPTY "{\"result\": [], \"error\": null}"
#define FOUND "{\"result\": [1700000000001], \"error\": null}"
#define DUPLICATE "{\"result\": null, \"error\": \"cannot create note because it is a duplicate\"}"
#define VERSION "{\"result\": 6, \"error\": null}"

/* Scripted AnkiConnect: n replies in turn. */
static void lan(int n, ...)
{
    va_list ap;
    memset(&net, 0, sizeof(net));
    net.stop_at = -1;
    va_start(ap, n);
    for (int i = 0; i < n; i++)
        net.responses[i] = va_arg(ap, const char *);
    va_end(ap);
    net.n = n;
    for (int i = 0; i < MAX_CONNS; i++)
        sent[i][0] = '\0';
}

/* The fake AnkiConnect server, with no notes yet. */
static void server(void)
{
    lan(0);
    net.server = 1;
    memset(&anki, 0, sizeof(anki));
    anki.offline_at = -1;
}

/* A shown local lookup: "猫が好き猫" with 猫 and 好き looked up. */
static const char *neko_meanings[] = {"cat", "shamisen \"neko\""};
static const char *suki_meanings[] = {"liked", "favourite"};
static VjoVocab neko = {.spelling = "猫", .reading = "ねこ", .rank = 1500, .meanings = neko_meanings, .n_meanings = 2};
static VjoVocab suki = {.spelling = "好き", .reading = "すき", .rank = VJO_NO_RANK, .meanings = suki_meanings,
                        .n_meanings = 2};
static VjoEntry entries[3];
static VjoEntryList list;
static VjoConfig cfg;

/* A dialogue-box game frame: smooth background, a dark box with rows of
 * glyph-like strokes. noise = 1: every pixel random (worst case). */
static void make_frame(int noise)
{
    uint32_t r = 12345;
    for (int y = 0; y < FRAME_H; y++) {
        for (int x = 0; x < FRAME_W; x++) {
            uint8_t *p = frame + ((size_t)y * FRAME_W + x) * 4;
            r = r * 1103515245u + 12345u;
            if (noise) {
                p[0] = (uint8_t)(r >> 24);
                p[1] = (uint8_t)(r >> 16);
                p[2] = (uint8_t)(r >> 8);
            } else if (y > 380 && y < 530 && x > 20 && x < 940) {
                int gx = (x - 40) % 26, gy = (y - 400) % 34, glyph = ((x - 40) / 26 * 7 + (y - 400) / 34 * 13) % 5;
                int ink = y >= 400 && gx < 22 && gy < 24 && glyph && ((gx * glyph + gy) % 6 < 2 || gy % 9 == glyph);
                p[0] = p[1] = p[2] = ink ? 240 : 24;
            } else {
                p[0] = (uint8_t)(90 + x / 8);
                p[1] = (uint8_t)(60 + y / 5);
                p[2] = (uint8_t)(140 + (x + y) / 16);
            }
            p[3] = 255;
        }
    }
}

/* The overlay showing that lookup, and mining on with anki_host set. */
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
    snprintf(cfg.anki_host, sizeof(cfg.anki_host), "192.168.1.23");
    clock_fails = 0;
    view_pages = NULL;
    view_page = 0;
}

static void clear_files(void)
{
    char p[512];
    DIR *d = opendir(VJO_ANKI_QUEUE_DIR);
    struct dirent *e;
    if (!d)
        return;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(p, sizeof(p), VJO_ANKI_QUEUE_DIR "/%s", e->d_name);
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
        n += e->d_name[0] != '.' && ends_with(e->d_name, suffix);
    closedir(d);
    return n;
}

/* A queue file, read with POSIX calls (NUL-terminated). */
static char *queue_file(const char *id, const char *ext, size_t *len)
{
    static char data[2][256 * 1024];
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
    clear_files();
    memset(&g_view, 0, sizeof(g_view));
    memset(&cap_state, 0, sizeof(cap_state));
    capture_rc = 0;
    captures = rows_read = discards = 0;
    held_box = held_view = mutex_deletes = fail_mutex = 0;
    threads_created = threads_started = threads_joined = threads_deleted = 0;
    fail_create = fail_start = fail_wait = defer_start = 0;
    heap_live = heap_attempts = heap_allocs = heap_frees = fail_heap = 0;
    write_budget = -1;
    short_io = fail_rename = fail_sync = fail_remove = fail_close = fail_scan = fail_mount_sync = 0;
    fail_json_write = 0;
    mount_sync_calls = dir_scans = 0;
    write_fd = -1;
    write_path[0] = last_rename[0] = '\0';
    host_file[0] = '\0';
    host_file_writes = 0;
    cancels = 0;
    rtc_unix_us = 1791158400007000ull;
    vjo_test_anki_text_only = 0;
    lan(0);
    /* the Shell starts the mailbox before its first config load */
    TEST_ASSERT(vjo_anki_start() == 0);
    TEST_ASSERT(capture_lock < 0 ? vjo_capture_init() == 0 : 1);
    fixture();
    TEST_ASSERT(vjo_anki_configure(&cfg) == 1);
    vjo_anki_set_game("PCSG00415", "CLANNAD");
    vjo_anki_set_ocr_model("VOCR3_H15_w8.vocr");
    make_frame(0);
}

static void reconfigure(void)
{
    TEST_ASSERT(vjo_anki_configure(&cfg) == 1);
}

/* Ends the test's mailbox; every job's memory and thread are gone. */
static void finish(void)
{
    vjo_anki_reap();
    TEST_CHECK(vjo_anki_stop() == 0);
    TEST_CHECK(!heap_live && heap_allocs == heap_frees);
    TEST_CHECK(threads_created == threads_deleted);
    TEST_CHECK(!held_box && !held_view);
    TEST_CHECK(host_file_writes == 0);
}

/* × on entry of the shown list, and △: each job runs to its end. */
static void cross(int entry)
{
    vjo_anki_post_add(g_view.list_seq, entry);
    vjo_anki_reap();
}
static void triangle(void)
{
    vjo_anki_post_send();
    vjo_anki_reap();
}

static const char *body_of(int k)
{
    const char *b = strstr(sent[k % MAX_CONNS], "\r\n\r\n");
    return b ? b + 4 : "";
}

static void only_id(char id[VJO_QUEUE_ID_SIZE])
{
    TEST_ASSERT(vjo_queue_next("", id) == 1 && id[0]);
}

/* ---------------- tests ---------------- */

static void test_off_by_default_allocates_nothing(void)
{
    VjoConfig d;
    struct stat st;
    reset();
    vjo_config_defaults(&d);
    TEST_CHECK(!d.anki_mining && !d.anki_host[0]);
    TEST_CHECK(vjo_anki_configure(&d) == 0);
    /* an anki_host left from an older config does not turn it on */
    snprintf(d.anki_host, sizeof(d.anki_host), "192.168.1.23");
    TEST_CHECK(vjo_anki_configure(&d) == 0);
    rmdir(VJO_ANKI_QUEUE_DIR);
    lan(1, ok(NOTE_ID));
    vjo_anki_post_count();
    vjo_anki_post_add(5, 0);
    vjo_anki_post_send();
    TEST_CHECK(threads_created == 0 && heap_attempts == 0 && captures == 0 && net.next == 0 && dir_scans == 0);
    TEST_CHECK(stat(VJO_ANKI_QUEUE_DIR, &st) < 0); /* not even the queue's directory */
    TEST_CHECK(!g_view.anki_status[0] && g_view.anki_version == 0);
    finish();
}

/* × with no computer and no network: the card is on the memory card. */
static void test_cross_saves_offline_without_host(void)
{
    char id[VJO_QUEUE_ID_SIZE];
    const uint8_t *jpeg;
    size_t len;
    const char *json;
    reset();
    cfg.anki_host[0] = '\0';
    reconfigure();
    lan(0);
    vjo_anki_post_add(5, 0);
    TEST_CHECK(threads_created == 1 && threads_started == 1 && net.next == 0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && g_view.anki_status_seq == 5 &&
               g_view.anki_status_kind == VJO_ANKI_STATUS_DIM);
    TEST_MSG("%s", g_view.anki_status);
    /* queued, on every occurrence of the word */
    TEST_CHECK(g_view.anki_pending == 1 && g_view.anki_marks_seq == 5 && g_view.anki_mark[0] == VJO_ANKI_MARK_QUEUED &&
               !g_view.anki_mark[1] && g_view.anki_mark[2] == VJO_ANKI_MARK_QUEUED);
    /* one record and its screenshot, and no temp file */
    TEST_CHECK(files_ending(".json") == 1 && files_ending(".jpg") == 1 && files_ending(".tmp") == 0);
    TEST_CHECK(captures == 1 && capture_flags == (VJO_CAPTURE_FULL | VJO_CAPTURE_ONCE) && rows_read == FRAME_H);
    TEST_CHECK(heap_allocs == 1 && heap_frees == 1 && !heap_live && heap_last == ANKI_PICTURE_MEM);
    /* the ledger: the arena and the thread's stack were counted in and are out
     * again. This binary builds the screenshot path (VJO_ANKI_SCREENSHOT 1),
     * whose arena is above the shipped text-only budget; the shipped build's is
     * checked by the build-config tests (vjo-shell-anki-build-tests). */
    TEST_CHECK(ANKI_STACK == VJO_STACK_SHELL_ANKI && ANKI_TEXT_MEM == VJO_BUDGET_PAF_ANKI);
    TEST_CHECK(vjo_ledger_peak(&g_shell_ledger, VJO_SL_PAF_ANKI) == ANKI_PICTURE_MEM);
    TEST_CHECK(vjo_ledger_current(&g_shell_ledger, VJO_SL_PAF_ANKI) == 0);
    TEST_CHECK(vjo_ledger_peak(&g_shell_ledger, VJO_SL_ANKI_STACK) == VJO_BUDGET_SHELL_ANKI_STACK);
    vjo_anki_reap();
    TEST_CHECK_(vjo_ledger_current(&g_shell_ledger, VJO_SL_ANKI_STACK) == 0, "%u", vjo_ledger_current(&g_shell_ledger, VJO_SL_ANKI_STACK));
    printf("measure: offline save with screenshot: arena %u bytes, peak %u\n", (unsigned)heap_last,
           (unsigned)last_peak);
    TEST_CHECK(last_peak > 0 && last_peak <= ANKI_PICTURE_MEM);
    TEST_CHECK(g_view.list == &list && !strcmp(list.header, "猫が好き猫")); /* recognized text untouched */
    /* readable UTF-8 JSON with every value a send may map, and the JPEG */
    only_id(id);
    json = queue_file(id, "json", &len);
    TEST_ASSERT(json != NULL);
    /* version 3: and the furigana, the dictionary, the other dictionaries'
     * entries, the game, its title ID and platform, the OCR model and the time */
    TEST_CHECK(!strcmp(json, "{\"version\":3,\"spelling\":\"猫\",\"reading\":\"ねこ\",\"sentence\":\"猫が好き猫\","
                             "\"rank\":1500,\"hl_start\":0,\"hl_end\":3,\"picture\":true,"
                             "\"meanings\":[\"cat\",\"shamisen \\\"neko\\\"\"],\"sentence_furigana\":\"猫が好き猫\","
                             "\"sf_hl_start\":0,\"sf_hl_end\":3,\"source\":null,\"pitch\":[],\"pos\":[],"
                             "\"misc\":\"CLANNAD (2026-10-12 09:30)\",\"furigana\":null,\"dictionary\":null,"
                             "\"more\":[],\"game\":\"CLANNAD\",\"title_id\":\"PCSG00415\",\"platform\":\"vita\","
                             "\"ocr_engine\":\"vocr\",\"ocr_model\":\"VOCR3_H15_w8.vocr\",\"time\":1791158400,"
                             "\"ocr_confidence\":null}\n"));
    TEST_MSG("%s", json);
    jpeg = (const uint8_t *)queue_file(id, "jpg", &len);
    TEST_CHECK(jpeg && len > 1000 && len <= VJO_ANKI_PICTURE_MAX && jpeg[0] == 0xFF && jpeg[1] == 0xD8);
    vjo_anki_reap();
    /* × again: the mark says so, without memory or I/O */
    vjo_anki_post_add(5, 2);
    TEST_CHECK(!strcmp(g_view.anki_status, "Already queued · △ sends it") && heap_attempts == 1);
    vjo_anki_reap();
    /* the same card from a new lookup of the same text: the storage knows it */
    g_view.list_seq = 6;
    cross(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Already queued") && files_ending(".json") == 1 &&
               g_view.anki_mark[0] == VJO_ANKI_MARK_QUEUED && g_view.anki_marks_seq == 6);
    TEST_MSG("%s", g_view.anki_status);
    finish();
}

/* A PSP game's card says so: Adrenaline's NPXS10028 and its PSPEMU* bubbles (its own
 * PSPEMUCFW, an Adrenaline Bubble Manager bubble), core/games.c's one rule. */
static void test_a_psp_games_card_says_psp(void)
{
    static const char *const ids[] = {"NPXS10028", "PSPEMUCFW", "PSPEMU001"};
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        char id[VJO_QUEUE_ID_SIZE];
        size_t len;
        const char *json;
        reset();
        vjo_test_anki_text_only = 1;
        vjo_anki_set_game(ids[i], "planetarian");
        cross(0);
        TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki"));
        only_id(id);
        json = queue_file(id, "json", &len);
        TEST_ASSERT(json != NULL);
        TEST_CHECK_(strstr(json, "\"platform\":\"psp\"") != NULL, "%s: %s", ids[i], json);
        finish();
    }
}

/* Japanese, supplementary characters, quotes, backslashes, tags and line
 * breaks survive the record and are escaped for Anki when sent. */
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
    vjo_test_anki_text_only = 1;
    entries[0] = (VjoEntry){.vocab = &v, .pos16 = 0, .hl_start = 0, .hl_end = 10, .text = "𠮷野家"};
    list = (VjoEntryList){.header = "𠮷野家で\r\n\"牛丼\" & <b>\\", .entries = entries, .n_entries = 1};
    cross(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && captures == 0 && heap_last == ANKI_TEXT_MEM);
    only_id(id);
    json = queue_file(id, "json", &len);
    TEST_ASSERT(json != NULL);
    /* raw UTF-8, only JSON's own escapes */
    TEST_CHECK(strstr(json, "\"spelling\":\"𠮷野家\"") && strstr(json, "\"sentence\":\"𠮷野家で\\r\\n\\\"牛丼\\\" & <b>\\\\\"") &&
               strstr(json, "\"cat \\\"quoted\\\" & <tag>\\nnext\\tline\",\"back\\\\slash\",\"日本語𠮷\""));
    TEST_MSG("%s", json);
    vjo_arena_init(&a, mem, sizeof(mem));
    TEST_ASSERT(vjo_queue_decode(&a, json, len, &n, &picture) == 3);
    TEST_CHECK(!picture && !strcmp(n.spelling, v.spelling) && !strcmp(n.reading, v.reading) &&
               !strcmp(n.sentence, list.header) && n.n_meanings == 3 && n.rank == 1234 && n.hl_end == 10);
    for (int i = 0; i < 3; i++)
        TEST_CHECK(!strcmp(n.meanings[i], meanings[i]));
    TEST_CHECK(!strcmp(n.misc, "CLANNAD (2026-10-12 09:30)"));
    /* sent as Anki's HTML: escaped, the sentence's line break as <br> */
    lan(2, ok(EMPTY), ok(NOTE_ID));
    triangle();
    TEST_CHECK(strstr(body_of(1), "\"Expression\":\"𠮷野家\"") &&
               strstr(body_of(1), "class=\\\"yomitan-glossary\\\"><ol><li><span><ul><li>cat &quot;quoted&quot; "
                                  "&amp; &lt;tag&gt;<br>next\\tline</li><li>back\\\\slash</li><li>日本語𠮷</li>"
                                  "</ul></span></li></ol></div>\"") &&
               strstr(body_of(1), "\"Sentence\":\"<b>𠮷野家</b>で<br>&quot;牛丼&quot; &amp; &lt;b&gt;\\\\\""));
    TEST_MSG("%s", body_of(1));
    TEST_CHECK(!strcmp(g_view.anki_status, "Sent 1 to Anki") && files_ending(".json") == 0);
    finish();
}

/* The queue outlives the plugin: after a restart (every byte of memory
 * gone), △ sends the card with the settings of that moment. */
static void test_queue_survives_restart_and_triangle_sends_it(void)
{
    char id[VJO_QUEUE_ID_SIZE], name[128], tags[96];
    static char b64[512 * 1024];
    size_t jpeg_len;
    const char *jpeg, *body, *data;
    int allocs;
    reset();
    cfg.anki_host[0] = '\0';
    reconfigure();
    cross(0);
    TEST_ASSERT(files_ending(".json") == 1);
    only_id(id);
    jpeg = queue_file(id, "jpg", &jpeg_len);
    TEST_ASSERT(jpeg && jpeg_len > 0);
    vjo_base64_encode((const uint8_t *)jpeg, jpeg_len, b64);
    b64[vjo_base64_len(jpeg_len)] = '\0';
    /* the plugin stops; its memory and the view are gone */
    finish();
    memset(&g_view, 0, sizeof(g_view));
    memset(heap, 0x5A, sizeof(heap));
    TEST_ASSERT(vjo_anki_start() == 0);
    /* the address set while it was queued applies now */
    fixture();
    reconfigure();
    TEST_CHECK(g_view.anki_pending == -2); /* not counted yet */
    allocs = heap_attempts;
    vjo_anki_post_count(); /* the overlay opens */
    vjo_anki_reap();
    TEST_CHECK(g_view.anki_pending == 1 && heap_attempts == allocs && net.next == 0);
    lan(2, ok(EMPTY), ok(NOTE_ID));
    triangle();
    /* findNotes with the card's tag first (a lost reply), then addNote */
    TEST_CHECK(net.next == 2 && strstr(body_of(0), "\"action\":\"findNotes\""));
    snprintf(tags, sizeof(tags), "\"query\":\"tag:vita_queue_%s\"", id);
    TEST_CHECK(strstr(body_of(0), tags) != NULL);
    body = body_of(1);
    {
        static const char start[] = "{\"action\":\"addNote\",\"version\":6,\"params\":{\"note\":{\"deckName\":\"Kiku\","
                                    "\"modelName\":\"Kiku\",\"fields\":{\"Expression\":\"猫\"";
        TEST_CHECK(!strncmp(body, start, strlen(start)));
    }
    TEST_MSG("%.300s", body);
    snprintf(tags, sizeof(tags), "\"tags\":[\"vita-jp-overlay\",\"vita_queue_%s\"]", id);
    TEST_CHECK(strstr(body, tags) != NULL);
    snprintf(name, sizeof(name), "\"picture\":[{\"filename\":\"vitajp_%s.jpg\",\"fields\":[\"Picture\"],\"data\":\"", id);
    data = strstr(body, name);
    TEST_ASSERT(data != NULL);
    data += strlen(name);
    TEST_CHECK(!strncmp(data, b64, strlen(b64)) && !strcmp(data + strlen(b64), "\"}]}}}"));
    /* removed only now: record, then its screenshot */
    TEST_CHECK(!strcmp(g_view.anki_status, "Sent 1 to Anki") && g_view.anki_pending == 0 && !g_view.anki_sending);
    TEST_CHECK(files_ending(".json") == 0 && files_ending(".jpg") == 0);
    TEST_CHECK(heap_last == ANKI_PICTURE_MEM && !heap_live);
    printf("measure: send with screenshot (%u-byte JPEG): arena %u bytes, peak %u\n", (unsigned)jpeg_len,
           (unsigned)heap_last, (unsigned)last_peak);
    TEST_CHECK(last_peak > jpeg_len && last_peak <= ANKI_PICTURE_MEM);
    finish();
}

/* The note △ sends: a Kiku note (v2.1.0's fields) in the Kiku deck, the tag
 * plus the card's stable tag, and the bounded screenshot. */
static void test_send_builds_the_configured_note_with_screenshot(void)
{
    static const char post[] = "POST / HTTP/1.1\r\nHost: 192.168.1.23:8765\r\nContent-Type: application/json\r\n";
    static const char note[] =
        "{\"action\":\"addNote\",\"version\":6,\"params\":{\"note\":{\"deckName\":\"Kiku\","
        "\"modelName\":\"Kiku\",\"fields\":{\"Expression\":\"猫\",\"ExpressionFurigana\":\"猫[ねこ]\","
        "\"ExpressionReading\":\"ねこ\","
        "\"MainDefinition\":\"<div style=\\\"text-align: left;\\\" class=\\\"yomitan-glossary\\\"><ol><li><span><ul>"
        "<li>cat</li><li>shamisen &quot;neko&quot;</li></ul></span></li></ol></div>\","
        "\"Sentence\":\"<b>猫</b>が好き猫\",\"SentenceFurigana\":\"<b>猫</b>が好き猫\",\"Picture\":\"\","
        "\"FreqSort\":\"1500\",\"MiscInfo\":\"CLANNAD (2026-10-12 09:30)\"},"
        "\"tags\":[\"vita-jp-overlay\",\"vita_queue_";
    char id[VJO_QUEUE_ID_SIZE], cl[64], soi[8] = {0};
    const char *body, *data, *end;
    reset();
    cross(0);
    only_id(id);
    lan(2, ok(EMPTY), ok(NOTE_ID));
    triangle();
    /* plain HTTP to anki_host's AnkiConnect on the LAN */
    TEST_CHECK(net.next == 2 && !strcmp(net.host[1], "192.168.1.23") && net.port[1] == VJO_ANKI_PORT);
    TEST_CHECK(!strncmp(sent[1], post, strlen(post)));
    body = body_of(1);
    snprintf(cl, sizeof(cl), "Content-Length: %lu\r\n", (unsigned long)strlen(body));
    TEST_CHECK(strstr(sent[1], cl) != NULL);
    TEST_CHECK(!strncmp(body, note, strlen(note)) && !strncmp(body + strlen(note), id, 32));
    TEST_MSG("%.600s", body);
    data = strstr(body, "\"data\":\"");
    TEST_ASSERT(data != NULL);
    data += 8;
    end = strchr(data, '"');
    TEST_ASSERT(end && !strcmp(end, "\"}]}}}"));
    TEST_CHECK((size_t)(end - data) % 4 == 0 && (size_t)(end - data) <= vjo_base64_len(VJO_ANKI_PICTURE_MAX));
    vjo_base64_encode((const uint8_t *)"\xFF\xD8\xFF", 3, soi);
    TEST_CHECK(!strncmp(data, soi, 4)); /* starts with the JPEG SOI marker */
    /* the √ replaces the queued mark on every occurrence */
    TEST_CHECK(!strcmp(g_view.anki_status, "Sent 1 to Anki") && g_view.anki_status_seq == 0 &&
               g_view.anki_status_kind == VJO_ANKI_STATUS_DIM);
    TEST_CHECK(g_view.anki_mark[0] == VJO_ANKI_MARK_ADDED && !g_view.anki_mark[1] &&
               g_view.anki_mark[2] == VJO_ANKI_MARK_ADDED);
    TEST_CHECK(heap_allocs == 2 && heap_frees == 2 && !heap_live && g_view.list == &list);
    printf("measure: picture card: JPEG %u bytes, request %u bytes, send peak %u of %u\n",
           (unsigned)((end - data) / 4 * 3), (unsigned)strlen(sent[1]), (unsigned)last_peak, (unsigned)heap_last);
    /* the control thread joins each finished job: no thread stays */
    TEST_CHECK(threads_created == 2 && threads_deleted == 2);
    finish();
}

/* The port comes from anki_host at △, and the word's audio is looked up
 * then, not when × saves. */
static void test_mapping_port_and_audio_at_send(void)
{
    static const char get[] = "GET /?term=%E5%A5%BD%E3%81%8D&reading=%E3%81%99%E3%81%8D HTTP/1.1\r\n";
    const char *body;
    reset();
    vjo_test_anki_text_only = 1;
    snprintf(cfg.anki_host, sizeof(cfg.anki_host), "my-pc.local:9000");
    clock_fails = 1; /* MiscInfo without a date rather than a wrong one */
    snprintf(cfg.anki_audio_url, sizeof(cfg.anki_audio_url), "http://192.168.1.23:5050/?term={term}&reading={reading}");
    reconfigure();
    cross(1);
    TEST_CHECK(net.next == 0 && captures == 0 && heap_last == ANKI_TEXT_MEM); /* no audio, no capture */
    lan(3, ok(EMPTY),
        ok("{\"type\":\"audioSourceList\",\"audioSources\":[{\"name\":\"a\",\"url\":\"http://192.168.1.23:5050/su.opus\"}]}"),
        ok(NOTE_ID));
    triangle();
    /* the card's tag on AnkiConnect's port, the audio source, then addNote */
    TEST_CHECK(net.next == 3 && net.port[0] == 9000 && net.port[1] == 5050 && net.port[2] == 9000 &&
               !strcmp(net.host[2], "my-pc.local"));
    TEST_CHECK(!strncmp(sent[1], get, strlen(get)));
    TEST_MSG("%s", sent[1]);
    body = body_of(2);
    TEST_CHECK(strstr(sent[2], "Host: my-pc.local:9000\r\n") != NULL);
    TEST_CHECK(strstr(body, "\"modelName\":\"Kiku\",\"fields\":{\"Expression\":\"好き\","
                            "\"ExpressionFurigana\":\"好[す]き\",\"ExpressionReading\":\"すき\","
                            "\"ExpressionAudio\":\"\",\"MainDefinition\":") != NULL &&
               strstr(body, "<li>liked</li><li>favourite</li>") != NULL &&
               strstr(body, "\"MiscInfo\":\"CLANNAD\"},\"tags\":[\"vita-jp-overlay\",\"vita_queue_") != NULL);
    TEST_CHECK(strstr(body, "\"audio\":[{\"filename\":\"vitajp_1791158400007.opus\",\"fields\":[\"ExpressionAudio\"],"
                            "\"url\":\"http://192.168.1.23:5050/su.opus\"}]") != NULL);
    TEST_MSG("%s", body);
    TEST_CHECK(!strstr(body, "\"picture\"") && captures == 0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Sent 1 to Anki") && g_view.anki_mark[1] == VJO_ANKI_MARK_ADDED);
    TEST_CHECK(heap_last == ANKI_TEXT_MEM && heap_frees == 2);
    printf("measure: text card with audio: send arena %u bytes, peak %u\n", (unsigned)heap_last,
           (unsigned)last_peak);
    finish();
}

/* AnkiConnect's refusals and an unreachable computer: a status line, and
 * the card stays queued (but for a duplicate, which is archived). */
static void test_failures_keep_the_card_and_the_overlay(void)
{
    static const struct {
        int n;
        const char *a, *b, *c, *d;
        const char *status;
        int kind, queued, archived;
    } cases[] = {
        {0, NULL, NULL, NULL, NULL, "0 sent; 1 kept: Anki offline", VJO_ANKI_STATUS_DIM, 1, 0},
        {1, "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n", NULL, NULL, NULL,
         "0 sent; 1 kept: Anki refused the request (HTTP 403)", VJO_ANKI_STATUS_ERROR, 1, 0},
        {1, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi", NULL, NULL, NULL,
         "0 sent; 1 kept: Anki: unexpected response (-104). Is AnkiConnect running?", VJO_ANKI_STATUS_ERROR, 1, 0},
        {2, EMPTY, "{\"result\": null, \"error\": \"model was not found: Kiku\"}", NULL, NULL,
         "0 sent; 1 kept: Kiku note type not found in Anki: import Kiku (docs/anki.md)", VJO_ANKI_STATUS_ERROR, 1, 0},
        {2, EMPTY, DUPLICATE, NULL, NULL, "Sent 0 to Anki, 1 already there", VJO_ANKI_STATUS_DIM, 0, 1},
        /* a new deck is created, then the note added */
        {4, EMPTY, "{\"result\": null, \"error\": \"deck was not found: Kiku\"}", "{\"result\": 1, \"error\": null}",
         NOTE_ID, "Sent 1 to Anki", VJO_ANKI_STATUS_DIM, 0, 0},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const char *r[4] = {cases[i].a, cases[i].b, cases[i].c, cases[i].d};
        reset();
        cross(0);
        for (int k = 0; k < 4; k++)
            if (r[k] && r[k][0] == '{')
                r[k] = ok(r[k]);
        lan(cases[i].n, r[0], r[1], r[2], r[3]);
        g_view.status_is_error = 0;
        snprintf(g_view.status, sizeof(g_view.status), "kept");
        triangle();
        TEST_CHECK(!strcmp(g_view.anki_status, cases[i].status) && g_view.anki_status_kind == cases[i].kind);
        TEST_MSG("case %u: %s", i, g_view.anki_status);
        TEST_CHECK(vjo_queue_count() == cases[i].queued && g_view.anki_pending == cases[i].queued);
        TEST_CHECK(files_ending(".duplicate.json") == cases[i].archived);
        TEST_CHECK(g_view.anki_mark[0] == (cases[i].queued ? VJO_ANKI_MARK_QUEUED : VJO_ANKI_MARK_ADDED));
        TEST_CHECK(g_view.list == &list && !strcmp(g_view.status, "kept") && !heap_live && heap_frees == 2);
        if (i == 5)
            TEST_CHECK(strstr(body_of(2), "{\"action\":\"createDeck\",\"version\":6,\"params\":{\"deck\":\"Kiku\"}}") &&
                       strstr(body_of(3), "\"action\":\"addNote\""));
        /* × and △ work again after any result */
        TEST_CHECK(job == JOB_NONE && !g_view.anki_sending);
        finish();
    }
}

/* A reply lost after Anki added the card, a removal that failed, and
 * replies that are no note ID: the card is kept, and a retry never adds it
 * twice (its tag is found first). */
static void test_lost_reply_and_retry_add_once(void)
{
    static const char *invalid[] = {"{\"result\":null,\"error\":null}", "{\"result\":true,\"error\":null}",
                                    "{\"result\":0,\"error\":null}", "{\"result\":\"123\",\"error\":null}",
                                    "{\"result\":123.5,\"error\":null}", "{\"result\":123,\"error\":false}",
                                    "{\"result\":123,\"error\":null} {}", "{\"result\":"};
    reset();
    cross(0);
    server();
    anki.drop_add_reply = 1;
    triangle();
    TEST_CHECK(anki.adds == 1 && anki.n_notes == 1 && files_ending(".json") == 1);
    TEST_CHECK(!strncmp(g_view.anki_status, "0 sent; 1 kept: ", 16) && g_view.anki_pending == 1);
    TEST_MSG("%s", g_view.anki_status);
    triangle();
    TEST_CHECK(!strcmp(anki.actions, "find add find") && anki.adds == 1 && files_ending(".json") == 0 &&
               files_ending(".jpg") == 0 && !strcmp(g_view.anki_status, "Sent 1 to Anki"));
    TEST_MSG("%s / %s", anki.actions, g_view.anki_status);
    finish();

    /* Anki has it but the record cannot be removed here: △ retries safely */
    reset();
    cross(0);
    server();
    fail_remove = 1;
    triangle();
    TEST_CHECK(anki.adds == 1 && files_ending(".json") == 1 &&
               !strcmp(g_view.anki_status, "0 sent; Anki has one more but it stays queued here: △ again is safe"));
    fail_remove = 0;
    triangle();
    TEST_CHECK(anki.adds == 1 && anki.finds == 2 && files_ending(".json") == 0);
    finish();

    /* anything but a positive note ID keeps the card */
    reset();
    cross(0);
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        lan(2, ok(EMPTY), ok(invalid[i]));
        triangle();
        TEST_CHECK(files_ending(".json") == 1 && net.next == 2);
        TEST_MSG("reply %u: %s", i, g_view.anki_status);
    }
    lan(1, ok("{\"result\":[null],\"error\":null}")); /* nor a malformed findNotes */
    triangle();
    TEST_CHECK(files_ending(".json") == 1 && net.next == 1);
    finish();
}

/* A duplicate is archived with its screenshot and the next card is sent. */
static void test_duplicates_are_archived_and_sending_continues(void)
{
    char id[VJO_QUEUE_ID_SIZE];
    reset();
    cross(0);
    cross(1);
    TEST_ASSERT(vjo_queue_count() == 2);
    server();
    anki.duplicate = "\"Expression\":\"猫\"";
    triangle();
    TEST_CHECK(anki.adds == 2 && anki.n_notes == 1 && !strcmp(g_view.anki_status, "Sent 1 to Anki, 1 already there"));
    TEST_MSG("%s", g_view.anki_status);
    /* (the archive is the only JSON file left: no card is pending) */
    TEST_CHECK(vjo_queue_count() == 0 && files_ending(".duplicate.json") == 1 && files_ending(".jpg") == 1);
    TEST_CHECK(g_view.anki_pending == 0 && g_view.anki_mark[0] == VJO_ANKI_MARK_ADDED &&
               g_view.anki_mark[1] == VJO_ANKI_MARK_ADDED);
    /* saving that card again: Anki has it */
    TEST_CHECK(vjo_queue_next("", id) == 0 && !id[0]);
    g_view.list_seq = 6;
    cross(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Already in Anki (this card was sent before)") &&
               g_view.anki_mark[0] == VJO_ANKI_MARK_ADDED && vjo_queue_count() == 0);
    finish();
}

/* Anki goes away after one card: the rest stays queued; a retry sends it. */
static void test_partial_send_keeps_the_rest(void)
{
    reset();
    cross(0);
    cross(1);
    server();
    anki.offline_at = 2;
    triangle();
    TEST_CHECK(anki.adds == 1 && files_ending(".json") == 1 && g_view.anki_pending == 1);
    TEST_CHECK(!strcmp(g_view.anki_status, "1 sent; 1 kept: Anki offline") &&
               g_view.anki_status_kind == VJO_ANKI_STATUS_DIM);
    TEST_MSG("%s", g_view.anki_status);
    anki.offline_at = -1;
    net.next = 0;
    triangle();
    TEST_CHECK(anki.adds == 2 && files_ending(".json") == 0 && !strcmp(g_view.anki_status, "Sent 1 to Anki"));
    /* nothing left */
    triangle();
    TEST_CHECK(!strcmp(g_view.anki_status, "No cards queued: × saves the word shown") && heap_allocs == 4);
    /* a console that enters with ○ is told to press ○ */
    g_view.enter_circle = 1;
    triangle();
    TEST_CHECK(!strcmp(g_view.anki_status, "No cards queued: ○ saves the word shown"));
    TEST_MSG("%s", g_view.anki_status);
    g_view.enter_circle = 0;
    /* no computer configured: the cards wait, without memory */
    cfg.anki_host[0] = '\0';
    reconfigure();
    g_view.list_seq = 6;
    cross(0);
    triangle();
    TEST_CHECK(!strcmp(g_view.anki_status, "Set Anki's address in the Overlay app; cards stay queued") &&
               files_ending(".json") == 1 && heap_allocs == 5);
    finish();
}

/* Failed writes, renames, flushes and closes queue nothing and leave no
 * temp file; short I/O still commits whole files; leftovers of interrupted
 * saves are never cards; a damaged card is kept and skipped. */
static void test_failed_writes_never_queue_a_partial_card(void)
{
    static const char *const junk[] = {"0123456789abcdef0123456789abcdef.json.tmp",
                                       "0123456789abcdef0123456789abcdef.jpg.tmp",
                                       "0123456789abcdef0123456789abcdef.jpg",
                                       "0123456789ABCDEF0123456789ABCDEF.json", "notes.txt", "short.json"};
    char id[VJO_QUEUE_ID_SIZE], path[256];
    const char *body;
    FILE *f;
    reset();
    cfg.anki_host[0] = '\0';
    reconfigure();
    for (int mode = 0; mode < 5; mode++) {
        write_budget = mode == 0 ? 11 : -1; /* the JPEG's write fails part way */
        fail_rename = mode == 1;
        fail_sync = mode == 2;
        fail_close = mode == 3;
        fail_json_write = mode == 4; /* the JPEG is committed, its record is not */
        g_view.list_seq = 10 + (unsigned)mode;
        last_queue_log[0] = '\0';
        cross(0);
        TEST_CHECK(!strcmp(g_view.anki_status, "Card not saved: cannot write ux0:data/VitaJPOverlay/anki_queue (space?)") &&
                   g_view.anki_status_kind == VJO_ANKI_STATUS_ERROR);
        TEST_CHECK(strstr(last_queue_log, " failed 0x") != NULL); /* log.txt names the call and its code */
        TEST_MSG("mode %d: %s / %s", mode, g_view.anki_status, last_queue_log);
        TEST_CHECK(vjo_queue_count() == 0 && files_ending(".tmp") == 0 && !g_view.anki_mark[0]);
        write_budget = -1;
        fail_rename = fail_sync = fail_close = fail_json_write = 0;
    }
    TEST_CHECK(files_ending(".jpg") == 1); /* mode 4's orphan: no card */
    /* short reads and writes (7 bytes a call) still commit whole files, and
     * the orphan is replaced */
    short_io = 1;
    g_view.list_seq = 20;
    cross(0);
    short_io = 0;
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && vjo_queue_count() == 1 && files_ending(".jpg") == 1);
    /* leftovers of interrupted saves and other files are never cards */
    for (unsigned i = 0; i < sizeof(junk) / sizeof(*junk); i++) {
        snprintf(path, sizeof(path), VJO_ANKI_QUEUE_DIR "/%s", junk[i]);
        TEST_ASSERT((f = fopen(path, "wb")) != NULL);
        fputs("{\"version\":2}", f);
        fclose(f);
    }
    TEST_CHECK(vjo_queue_count() == 1);
    /* a damaged card is kept and skipped; the others are sent */
    only_id(id);
    g_view.list_seq = 21;
    cross(1);
    TEST_ASSERT(vjo_queue_count() == 2);
    snprintf(path, sizeof(path), VJO_ANKI_QUEUE_DIR "/%s.json", id);
    TEST_ASSERT(truncate(path, 20) == 0);
    snprintf(cfg.anki_host, sizeof(cfg.anki_host), "192.168.1.23"); /* a computer to send to */
    reconfigure();
    server();
    triangle();
    TEST_CHECK(anki.adds == 1 && vjo_queue_count() == 1 && access(path, F_OK) == 0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Sent 1 to Anki, 1 kept (unreadable, see log.txt)") &&
               g_view.anki_status_kind == VJO_ANKI_STATUS_ERROR);
    TEST_MSG("%s", g_view.anki_status);
    TEST_CHECK(strstr(last_queue_log, "checksum or format mismatch") != NULL);
    /* a card whose screenshot is gone: kept while a picture field wants it */
    TEST_ASSERT(unlink(path) == 0);
    g_view.list_seq = 22;
    cross(0);
    only_id(id);
    snprintf(path, sizeof(path), VJO_ANKI_QUEUE_DIR "/%s.jpg", id);
    TEST_ASSERT(unlink(path) == 0);
    triangle();
    TEST_CHECK(anki.adds == 1 && vjo_queue_count() == 1 &&
               !strcmp(g_view.anki_status, "Sent 0 to Anki, 1 kept (unreadable, see log.txt)"));
    /* ...and sent without it by a build that takes no screenshots */
    vjo_test_anki_text_only = 1;
    triangle();
    body = body_of(net.next - 1);
    TEST_CHECK(anki.adds == 2 && vjo_queue_count() == 0 && !strstr(body, "\"picture\"") &&
               !strcmp(g_view.anki_status, "Sent 1 to Anki") && heap_last == ANKI_TEXT_MEM);
    /* an unreadable queue directory */
    fail_scan = 1;
    vjo_anki_post_count();
    vjo_anki_reap();
    TEST_CHECK(g_view.anki_pending == -1);
    triangle();
    TEST_CHECK(!strcmp(g_view.anki_status, "Cannot read ux0:data/VitaJPOverlay/anki_queue; cards kept"));
    fail_scan = 0;
    finish();
}

/* A record renamed into place whose directory flush failed is not yet a
 * saved card: a retry flushes again before reporting it. */
static void test_failed_directory_flush_is_retried(void)
{
    static const char *meanings[] = {"cat"};
    static uint8_t mem[64 * 1024];
    VjoAnkiNote n = {.spelling = "猫", .reading = "ねこ", .rank = 1, .meanings = meanings, .n_meanings = 1,
                     .sentence = "猫が", .hl_start = 0, .hl_end = 3, .sf_hl_start = -1, .sf_hl_end = -1};
    VjoAnkiMedia m = {0};
    char id[VJO_QUEUE_ID_SIZE];
    VjoArena a;
    int barriers;
    reset();
    vjo_arena_init(&a, mem, sizeof(mem));
    fail_mount_sync = 1;
    TEST_CHECK(vjo_queue_save(&a, &n, &m) == VJO_QUEUE_FAILED);
    TEST_CHECK(vjo_queue_count() == 1); /* visible, but never acknowledged */
    barriers = mount_sync_calls;
    vjo_arena_reset(&a);
    TEST_CHECK(vjo_queue_save(&a, &n, &m) == VJO_QUEUE_FAILED && mount_sync_calls == barriers + 1);
    fail_mount_sync = 0;
    vjo_arena_reset(&a);
    TEST_CHECK(vjo_queue_save(&a, &n, &m) == VJO_QUEUE_EXISTS && mount_sync_calls == barriers + 2);
    only_id(id);
    TEST_ASSERT(vjo_queue_archive_duplicate(id) == 0);
    fail_mount_sync = 1;
    vjo_arena_reset(&a);
    TEST_CHECK(vjo_queue_save(&a, &n, &m) == VJO_QUEUE_FAILED);
    fail_mount_sync = 0;
    vjo_arena_reset(&a);
    TEST_CHECK(vjo_queue_save(&a, &n, &m) == VJO_QUEUE_ARCHIVED && vjo_queue_count() == 0);
    /* nothing outside the queue can be named */
    TEST_CHECK(vjo_queue_remove("../config") == -1 && vjo_queue_archive_duplicate("ABC") == -1);
    vjo_arena_reset(&a);
    TEST_CHECK(vjo_queue_load(&a, "../../config.ini", 1, &n, &m) == -1);
    finish();
}

/* Screenshot problems never cost the card. */
static void test_screenshot_failures_still_save(void)
{
    char id[VJO_QUEUE_ID_SIZE];
    reset();
    capture_rc = VJO_ERR_BUSY; /* the kernel's capture is taken */
    cross(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki without a screenshot (the capture failed)"));
    only_id(id);
    TEST_CHECK(strstr(queue_file(id, "json", NULL), "\"picture\":false") && files_ending(".jpg") == 0 && !heap_live);
    finish();

    /* a frame whose JPEG does not fit: dropped, and the arena still holds the card */
    reset();
    make_frame(1);
    cross(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki without a screenshot (the screenshot was too large)"));
    TEST_MSG("%s", g_view.anki_status);
    TEST_CHECK(vjo_queue_count() == 1 && files_ending(".jpg") == 0 && rows_read > 0 && !heap_live);
    printf("measure: noise frame (JPEG over the bound): save peak %u of %u\n", (unsigned)last_peak,
           (unsigned)heap_last);
    TEST_CHECK(last_peak > 0 && last_peak <= ANKI_PICTURE_MEM);
    finish();
}

/* Out of memory: the recognized text, its lookup and the queued cards stay. */
static void test_oom_keeps_recognized_text_and_cards(void)
{
    static char big[48 * 1024], huge[136 * 1024], amps[30 * 1024];
    static const char *big_meanings[] = {big}, *huge_meanings[] = {huge}, *amp_meanings[] = {amps};
    static VjoVocab long_word = {.spelling = "長文", .reading = "ちょうぶん", .rank = VJO_NO_RANK,
                                 .meanings = big_meanings, .n_meanings = 1};
    static VjoVocab huge_word = {.spelling = "巨大", .reading = "きょだい", .rank = VJO_NO_RANK,
                                 .meanings = huge_meanings, .n_meanings = 1};
    static VjoVocab amp_word = {.spelling = "記号", .reading = "きごう", .rank = VJO_NO_RANK,
                                .meanings = amp_meanings, .n_meanings = 1};
    memset(big, 'x', sizeof(big) - 1);
    memset(huge, 'y', sizeof(huge) - 1);
    memset(amps, '&', sizeof(amps) - 1); /* each one is 5 bytes as Anki's HTML */

    /* the Paf heap keeps its UI reserve: nothing is captured or written */
    reset();
    fail_heap = 1;
    snprintf(g_view.status, sizeof(g_view.status), "kept");
    vjo_anki_post_add(5, 0);
    TEST_CHECK(heap_attempts == 1 && heap_allocs == 0 && captures == 0 && vjo_queue_count() == 0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Anki: not enough memory, card not saved") &&
               g_view.anki_status_kind == VJO_ANKI_STATUS_ERROR);
    TEST_CHECK(g_view.list == &list && !strcmp(g_view.status, "kept") && !g_view.anki_mark[0] && job == JOB_NONE);
    fail_heap = 0;
    vjo_anki_reap();
    cross(0); /* and a retry works */
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && vjo_queue_count() == 1);
    /* no memory to send: the cards stay, nothing is sent */
    fail_heap = 1;
    lan(2, ok(EMPTY), ok(NOTE_ID));
    triangle();
    TEST_CHECK(!strcmp(g_view.anki_status, "Anki: not enough memory to send; cards kept") && net.next == 0 &&
               vjo_queue_count() == 1 && !g_view.anki_sending);
    fail_heap = 0;
    finish();

    /* no thread for the job */
    reset();
    fail_create = 1;
    vjo_anki_post_add(5, 0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Anki: not enough memory, card not saved"));
    vjo_anki_post_send();
    TEST_CHECK(!strcmp(g_view.anki_status, "Anki: not enough memory to send; cards kept"));
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
    cross(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Card not saved: over 32 KiB of text") && vjo_queue_count() == 0 &&
               files_ending(".jpg") == 0 && files_ending(".tmp") == 0 && !heap_live && g_view.list == &list);
    TEST_MSG("%s", g_view.anki_status);
    finish();

    /* an entry larger than the text-only arena */
    reset();
    vjo_test_anki_text_only = 1;
    entries[0].vocab = &huge_word;
    cross(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Anki: not enough memory for this entry; card not saved") &&
               vjo_queue_count() == 0 && heap_last == ANKI_TEXT_MEM && !heap_live);
    TEST_MSG("%s", g_view.anki_status);
    finish();

    /* a queued card too large to send in the text-only arena is kept, and
     * the others are sent */
    reset();
    vjo_test_anki_text_only = 1;
    entries[0].vocab = &amp_word;
    cross(0);
    TEST_ASSERT(!strcmp(g_view.anki_status, "Saved for Anki"));
    entries[0].vocab = &neko;
    g_view.list_seq = 6;
    cross(0);
    TEST_ASSERT(vjo_queue_count() == 2);
    server();
    triangle();
    TEST_CHECK(anki.adds == 1 && vjo_queue_count() == 1 && !strcmp(g_view.anki_status, "Sent 1 to Anki, 1 kept (too large: the Overlay app sends it)"));
    TEST_MSG("%s", g_view.anki_status);
    TEST_CHECK(g_view.anki_mark[0] == VJO_ANKI_MARK_QUEUED); /* not all were sent */
    finish();
}

static void test_cancellation(void)
{
    /* the list changed before the save ran: nothing is copied or written */
    reset();
    defer_start = 1;
    vjo_anki_post_add(5, 0);
    TEST_CHECK(threads_started == 1 && job == JOB_SAVE);
    vjo_anki_post_add(5, 1); /* a second × meanwhile is refused, and says so */
    TEST_CHECK(threads_created == 1 && !strcmp(g_view.anki_status, "Anki: still saving the previous card"));
    g_view.list_seq = 6; /* another word shown */
    thread_entry(0, NULL);
    TEST_CHECK(vjo_queue_count() == 0 && captures == 0 && heap_attempts == 0 && job == JOB_NONE);
    finish();

    /* the overlay closed before the save ran */
    reset();
    defer_start = 1;
    vjo_anki_post_add(5, 0);
    g_view.open = 0;
    thread_entry(0, NULL);
    TEST_CHECK(vjo_queue_count() == 0 && heap_attempts == 0 && !heap_live);
    finish();

    /* stop while a card is being sent: the request is aborted, the card is
     * kept, nothing more is published, the arena is freed and the thread joined */
    reset();
    cross(0);
    server();
    net.stop_at = 1; /* during its addNote */
    vjo_anki_post_send();
    TEST_CHECK(cancels == 1 && anki.finds == 1 && anki.adds == 0 && vjo_queue_count() == 1 && !heap_live);
    TEST_CHECK(!strncmp(g_view.anki_status, "Sending to Anki…", strlen("Sending to Anki…")));
    TEST_CHECK(!g_view.anki_sending && g_view.anki_pending == 1);
    TEST_CHECK(vjo_anki_stop() == 0 && threads_joined == 2 && threads_deleted == 2 && box_lock < 0);
    vjo_anki_post_add(5, 0); /* after stop: ignored */
    vjo_anki_post_send();
    TEST_CHECK(threads_created == 2);

    /* stop between cards: those sent are gone, the others stay */
    reset();
    cross(0);
    cross(1);
    g_view.list_seq = 6;
    list.header = "猫が好き猫。";
    cross(0);
    TEST_ASSERT(vjo_queue_count() == 3);
    server();
    net.stop_at = 2; /* the second card's findNotes */
    vjo_anki_post_send();
    TEST_CHECK(anki.adds == 1 && vjo_queue_count() == 2 && g_view.anki_pending == 2 && !heap_live);
    finish();

    /* × while the queue is being sent is refused; △ during a save sends
     * right after it, × during a count saves right after it */
    reset();
    cross(0);
    server();
    defer_start = 1;
    vjo_anki_post_send();
    vjo_anki_post_add(5, 1);
    TEST_CHECK(!strcmp(g_view.anki_status, "Anki: sending the queue; × saves again when it is done"));
    g_view.enter_circle = 1;
    vjo_anki_post_add(5, 1);
    TEST_CHECK(!strcmp(g_view.anki_status, "Anki: sending the queue; ○ saves again when it is done"));
    g_view.enter_circle = 0;
    vjo_anki_post_send();
    TEST_CHECK(!strcmp(g_view.anki_status, "Already sending to Anki"));
    thread_entry(0, NULL);
    TEST_CHECK(anki.adds == 1 && vjo_queue_count() == 0 && threads_created == 2);
    vjo_anki_reap();
    g_view.list_seq = 6;
    vjo_anki_post_add(6, 1);
    vjo_anki_post_send();
    TEST_CHECK(threads_created == 3 && job == JOB_SAVE && next_job == JOB_SEND);
    thread_entry(0, NULL); /* one thread: the save, then the send */
    TEST_CHECK(anki.adds == 2 && vjo_queue_count() == 0 && !strcmp(g_view.anki_status, "Sent 1 to Anki"));
    vjo_anki_reap();
    g_view.list_seq = 7;
    vjo_anki_post_count(); /* the overlay opens */
    vjo_anki_post_add(7, 0); /* and × at once */
    TEST_CHECK(threads_created == 4 && job == JOB_COUNT && next_job == JOB_SAVE);
    thread_entry(0, NULL);
    TEST_CHECK(vjo_queue_count() == 1 && !strcmp(g_view.anki_status, "Saved for Anki"));
    /* that card was sent before: its tag is found and no second note is added */
    defer_start = 0;
    triangle();
    TEST_CHECK(anki.adds == 2 && anki.finds == 3 && vjo_queue_count() == 0);
    finish();

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

/* anki_host = auto: △ sends to the computer the Overlay app's search found
 * (anki_host.txt), read again for every send; SceShell itself never
 * searches the network, opens no probe sockets and writes no host file. */
static void test_auto_finds_anki_on_the_lan(void)
{
    reset();
    snprintf(cfg.anki_host, sizeof(cfg.anki_host), "auto");
    reconfigure();
    cross(0);
    TEST_CHECK(net.next == 0);
    /* not found yet: the card waits, and the app is where to look */
    triangle();
    TEST_CHECK(net.next == 0 && vjo_queue_count() == 1 &&
               !strcmp(g_view.anki_status, "0 sent; 1 kept: Anki not found: test the connection in the Overlay app") &&
               g_view.anki_status_kind == VJO_ANKI_STATUS_DIM);
    TEST_MSG("%s", g_view.anki_status);
    /* the app found it */
    snprintf(host_file, sizeof(host_file), "192.168.1.23:8765\r\n");
    lan(2, ok(EMPTY), ok(NOTE_ID));
    triangle();
    TEST_CHECK(net.next == 2 && !strcmp(net.host[0], "192.168.1.23") && net.port[1] == 8765 &&
               strstr(body_of(0), "\"action\":\"findNotes\"") && strstr(body_of(1), "\"action\":\"addNote\""));
    TEST_CHECK(!strcmp(g_view.anki_status, "Sent 1 to Anki") && vjo_queue_count() == 0);
    /* it moved and the app found it again while the plugin ran: the next send uses it */
    snprintf(host_file, sizeof(host_file), "192.168.1.64:9000\n");
    g_view.list_seq = 6;
    cross(1);
    lan(2, ok(EMPTY), ok(NOTE_ID));
    triangle();
    TEST_CHECK(net.next == 2 && !strcmp(net.host[1], "192.168.1.64") && net.port[1] == 9000 &&
               !strcmp(g_view.anki_status, "Sent 1 to Anki"));
    /* the found computer does not answer: the app is to search again */
    g_view.list_seq = 8;
    list.header = "猫が好き猫。";
    cross(0);
    lan(0);
    triangle();
    TEST_CHECK(net.next == 1 && vjo_queue_count() == 1 &&
               !strcmp(g_view.anki_status, "0 sent; 1 kept: Anki not found: test the connection in the Overlay app") &&
               logged("anki: 192.168.1.64:9000 (auto) failed -100\n"));
    TEST_MSG("%s", g_view.anki_status);
    /* nor is a reply that is not AnkiConnect's */
    lan(1, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi");
    triangle();
    TEST_CHECK(vjo_queue_count() == 1 && strstr(g_view.anki_status, "Anki not found") != NULL);
    /* an AnkiConnect refusal is AnkiConnect answering: it is shown as it is */
    lan(2, ok(EMPTY), ok("{\"result\": null, \"error\": \"collection is not available\"}"));
    triangle();
    TEST_CHECK(vjo_queue_count() == 1 && !strcmp(g_view.anki_status, "0 sent; 1 kept: Anki: collection is not available"));
    TEST_MSG("%s", g_view.anki_status);
    clear_files();
    /* a damaged file is no address */
    snprintf(host_file, sizeof(host_file), "my pc\n");
    g_view.list_seq = 7;
    cross(0);
    lan(0);
    triangle();
    TEST_CHECK(net.next == 0 && strstr(g_view.anki_status, "Anki not found") && vjo_queue_count() == 1);
    finish();
}

/* A card saved by #5's queue (record version 1) still sends. */
static void test_version_1_record_still_sends(void)
{
    static const char v1[] = "{\"version\":1,\"spelling\":\"猫\",\"reading\":\"ねこ\",\"sentence\":\"猫が\\n好き\","
                             "\"rank\":1234,\"hl_start\":0,\"hl_end\":3,\"picture\":false,\"meanings\":[\"cat\"]}\n";
    char id[VJO_QUEUE_ID_SIZE], path[256], tag[64];
    FILE *f;
    reset();
    TEST_ASSERT(vjo_queue_count() == 0); /* the directory exists */
    vjo_queue_id(v1, strlen(v1), id);
    snprintf(path, sizeof(path), VJO_ANKI_QUEUE_DIR "/%s.json", id);
    TEST_ASSERT((f = fopen(path, "wb")) != NULL);
    fputs(v1, f);
    fclose(f);
    vjo_anki_post_count();
    vjo_anki_reap();
    TEST_CHECK(g_view.anki_pending == 1);
    lan(2, ok(EMPTY), ok(NOTE_ID));
    triangle();
    snprintf(tag, sizeof(tag), "\"vita_queue_%s\"", id);
    TEST_CHECK(strstr(body_of(1), "\"modelName\":\"Kiku\",\"fields\":{\"Expression\":\"猫\","
                                  "\"ExpressionFurigana\":\"猫[ねこ]\",\"ExpressionReading\":\"ねこ\",") &&
               strstr(body_of(1), "\"Sentence\":\"<b>猫</b>が<br>好き\",\"FreqSort\":\"1234\"}") &&
               strstr(body_of(1), tag));
    TEST_MSG("%s", body_of(1));
    TEST_CHECK(!strcmp(g_view.anki_status, "Sent 1 to Anki") && vjo_queue_count() == 0);
    finish();
}

/* A card saved by v0.8.0-test.1 (record version 2), in the shape of the
 * owner's console queue (#81: a local dictionary's entry, its name on its
 * definition's first line; no pitch, parts of speech or frequency source),
 * becomes a Kiku note: the dictionary's name on its glossary item. The text
 * here is made up: the real records stay private. */
static void test_version_2_record_from_the_test_build_sends_as_kiku(void)
{
    static const char v2[] =
        "{\"version\":2,\"spelling\":\"部屋\",\"reading\":\"へや\","
        "\"sentence\":\"隣の部屋で、妙に背の高い先生と話をして\",\"rank\":99999,\"hl_start\":6,\"hl_end\":12,"
        "\"picture\":false,\"meanings\":[\"[Daijirin4 Plain]\\nA room or chamber of a house.; A sumo stable.\"],"
        "\"sentence_furigana\":\"隣の部屋で、妙に背の高い先生と話をして\",\"sf_hl_start\":6,\"sf_hl_end\":12,"
        "\"source\":null,\"pitch\":[],\"pos\":[],\"misc\":\"SOME GAME (2026-10-11 17:11)\"}\n";
    char id[VJO_QUEUE_ID_SIZE], path[256];
    const char *body;
    FILE *f;
    reset();
    TEST_ASSERT(vjo_queue_count() == 0);
    vjo_queue_id(v2, strlen(v2), id);
    snprintf(path, sizeof(path), VJO_ANKI_QUEUE_DIR "/%s.json", id);
    TEST_ASSERT((f = fopen(path, "wb")) != NULL);
    fputs(v2, f);
    fclose(f);
    lan(2, ok(EMPTY), ok(NOTE_ID));
    triangle();
    body = body_of(1);
    TEST_CHECK(strstr(body, "{\"action\":\"addNote\",\"version\":6,\"params\":{\"note\":{\"deckName\":\"Kiku\","
                            "\"modelName\":\"Kiku\",\"fields\":{\"Expression\":\"部屋\",\"ExpressionFurigana\":"
                            "\"部屋[へや]\",\"ExpressionReading\":\"へや\",\"MainDefinition\":\"<div style=\\\"text-align: "
                            "left;\\\" class=\\\"yomitan-glossary\\\"><ol><li data-dictionary=\\\"Daijirin4 Plain\\\">"
                            "<i>(Daijirin4 Plain)</i> <span>A room or chamber of a house.; A sumo stable.</span></li></ol>"
                            "</div>\",\"Sentence\":\"隣の<b>部屋</b>で、妙に背の高い先生と話をして\","
                            "\"SentenceFurigana\":\"隣の<b>部屋</b>で、妙に背の高い先生と話をして\","
                            "\"MiscInfo\":\"SOME GAME (2026-10-11 17:11)\"},\"tags\":[\"vita-jp-overlay\",") == body);
    TEST_MSG("%s", body);
    TEST_CHECK(!strstr(body, "FreqSort") && !strstr(body, "Glossary")); /* no rank; one dictionary */
    TEST_CHECK(!strcmp(g_view.anki_status, "Sent 1 to Anki") && vjo_queue_count() == 0);
    finish();
}

/* × with Dictionaries shown above 1: the other dictionaries' entries are
 * read from their files then (one payload at a time, after the view lock)
 * and recorded with their names; △ puts every one into Kiku's Glossary. */
static void test_cross_records_every_dictionary_and_sends_the_glossary(void)
{
    static const DictRow rows[3] = {
        {"好き", "好き", "すき", "[Daijirin4 Plain]\nliked; favourite"},
        {"好き", "好き", "すき", "[JMdict Plain]\nliked"},
        {"好き", "好き", "すき", "[Sankoku8 Plain]\nfond of"},
    };
    static uint8_t mem[96 * 1024];
    static VjoLocalPages pages;
    VjoArena a;
    VjoPlatform p;
    VjoDictResult res;
    VjoErr err;
    char id[VJO_QUEUE_ID_SIZE];
    const char *json, *body;
    reset();
    vjo_test_anki_text_only = 1;
    dict_build(&dict_files[0], "main.vjdict", &rows[0]);
    dict_build(&dict_files[1], "b.vjdict", &rows[1]);
    dict_build(&dict_files[2], "c.vjdict", &rows[2]);
    snprintf(cfg.local_dictionary_dir, sizeof(cfg.local_dictionary_dir), "fixture");
    snprintf(cfg.local_dictionaries, sizeof(cfg.local_dictionaries), "main.vjdict,b.vjdict,c.vjdict");
    cfg.local_dictionary_results = 3;
    reconfigure();
    /* the shown result: worker.c's lookup at 好き, its first page shown */
    vjo_arena_init(&a, mem, sizeof(mem));
    vjo_platform_vita(&p, NULL);
    TEST_ASSERT(vjo_local_lookup_pages(&a, &p, &cfg, list.header, 6, 3, &pages, &res, &err) == VJO_OK);
    TEST_ASSERT(pages.count == 3 && res.n_vocab == 1);
    entries[1].vocab = &res.vocab[0];
    view_pages = &pages;
    view_page = 0;
    dict_opens = 0;
    cross(1);
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && dict_opens == 2);
    TEST_CHECK(!dict_files[0].active && !dict_files[1].active && !dict_files[2].active);
    printf("measure: save with 2 other dictionaries' entries: arena %u bytes, peak %u\n", (unsigned)heap_last,
           (unsigned)last_peak);
    TEST_CHECK(heap_last == ANKI_TEXT_MEM && last_peak < ANKI_TEXT_MEM / 2);
    only_id(id);
    json = queue_file(id, "json", NULL);
    TEST_ASSERT(json != NULL);
    TEST_CHECK(strstr(json, "\"meanings\":[\"[Daijirin4 Plain]\\nliked; favourite\"]") &&
               strstr(json, "\"dictionary\":\"Daijirin4 Plain\",\"more\":[{\"dictionary\":\"JMdict Plain\","
                            "\"spelling\":\"好き\",\"reading\":\"すき\",\"meanings\":[\"[JMdict Plain]\\nliked\"]},"
                            "{\"dictionary\":\"Sankoku8 Plain\",\"spelling\":\"好き\",\"reading\":\"すき\","
                            "\"meanings\":[\"[Sankoku8 Plain]\\nfond of\"]}],"));
    TEST_MSG("%s", json);
    lan(2, ok(EMPTY), ok(NOTE_ID));
    triangle();
    body = body_of(1);
    TEST_CHECK(strstr(body, "\"Glossary\":\"<div style=\\\"text-align: left;\\\" class=\\\"yomitan-glossary\\\"><ol>"
                            "<li data-dictionary=\\\"Daijirin4 Plain\\\"><i>(Daijirin4 Plain)</i> <span>liked; favourite"
                            "</span></li><li data-dictionary=\\\"JMdict Plain\\\"><i>(JMdict Plain)</i> <span>liked</span>"
                            "</li><li data-dictionary=\\\"Sankoku8 Plain\\\"><i>(Sankoku8 Plain)</i> <span>fond of</span>"
                            "</li></ol></div>\"") != NULL);
    TEST_MSG("%s", body);
    TEST_CHECK(!strcmp(g_view.anki_status, "Sent 1 to Anki"));
    /* the page shown is the main definition; the others are not read twice */
    reset();
    vjo_test_anki_text_only = 1;
    snprintf(cfg.local_dictionary_dir, sizeof(cfg.local_dictionary_dir), "fixture");
    snprintf(cfg.local_dictionaries, sizeof(cfg.local_dictionaries), "main.vjdict,b.vjdict,c.vjdict");
    reconfigure();
    entries[1].vocab = &res.vocab[0];
    view_pages = &pages;
    view_page = 2;
    dict_opens = 0;
    cross(1);
    TEST_CHECK(dict_opens == 2);
    only_id(id);
    json = queue_file(id, "json", NULL);
    TEST_CHECK(json && strstr(json, "\"more\":[{\"dictionary\":\"Daijirin4 Plain\"") &&
               strstr(json, "{\"dictionary\":\"JMdict Plain\"") && !strstr(json, "{\"dictionary\":\"Sankoku8 Plain\""));
    TEST_MSG("%s", json ? json : "");
    finish();
}

/* The record holds the minute of the × press: the same card saved again
 * within it (the marks gone: a new lookup of the same text) is the same
 * record; in the next minute it is another one. */
static void test_same_card_within_a_minute_is_one_record(void)
{
    reset();
    vjo_test_anki_text_only = 1;
    rtc_unix_us = 1791158405000000ull; /* 0:05 */
    cross(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && files_ending(".json") == 1);
    rtc_unix_us = 1791158459999000ull; /* 0:59 */
    g_view.list_seq = 6;
    cross(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Already queued") && files_ending(".json") == 1);
    TEST_MSG("%s", g_view.anki_status);
    rtc_unix_us = 1791158460000000ull; /* 1:00 */
    g_view.list_seq = 7;
    cross(0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && files_ending(".json") == 2);
    finish();
}

/* Whatever × saves, △ can send: with 8 dictionaries' entries of any size
 * (and text that HTML and JSON escaping grow), × keeps the other
 * dictionaries' entries only while the card stays sendable in △'s arena, as
 * a dry run of the send; small ones are all kept. */
static void test_what_cross_saves_triangle_sends(void)
{
    static const int sizes[] = {300, 900, 2700, 4000, 7000};
    static char meaning[8][7200], names[8][16], list_names[160];
    static DictRow rows[8];
    static uint8_t mem[256 * 1024];
    static VjoLocalPages pages;
    for (int heavy = 0; heavy < 2; heavy++) {
        for (unsigned z = 0; z < sizeof(sizes) / sizeof(sizes[0]); z++) {
            VjoArena a;
            VjoPlatform p;
            VjoDictResult res;
            VjoErr err;
            char id[VJO_QUEUE_ID_SIZE];
            const char *json;
            int kept = 0;
            reset();
            vjo_test_anki_text_only = 1;
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
                rows[k] = (DictRow){"好き", "好き", "すき", meaning[k]};
                dict_build(&dict_files[k], names[k], &rows[k]);
                strcat(list_names, k ? "," : "");
                strcat(list_names, names[k]);
            }
            snprintf(cfg.local_dictionary_dir, sizeof(cfg.local_dictionary_dir), "fixture");
            snprintf(cfg.local_dictionaries, sizeof(cfg.local_dictionaries), "%s", list_names);
            cfg.local_dictionary_results = 8;
            reconfigure();
            vjo_arena_init(&a, mem, sizeof(mem));
            vjo_platform_vita(&p, NULL);
            TEST_ASSERT(vjo_local_lookup_pages(&a, &p, &cfg, list.header, 6, 8, &pages, &res, &err) == VJO_OK);
            TEST_ASSERT(pages.count == 8 && res.n_vocab == 1);
            entries[1].vocab = &res.vocab[0];
            view_pages = &pages;
            view_page = 0;
            cross(1);
            TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki"));
            only_id(id);
            json = queue_file(id, "json", NULL);
            TEST_ASSERT(json != NULL);
            for (const char *q = json; (q = strstr(q, "{\"dictionary\":")); q++)
                kept++;
            server();
            triangle();
            /* sent; or, when even the shown entry alone is too large for △ (7 KB of
             * text that escaping grows about six times), kept for the Overlay app */
            int sent = !strcmp(g_view.anki_status, "Sent 1 to Anki") && anki.adds == 1 && vjo_queue_count() == 0;
            TEST_CHECK(sent || (heavy && !kept && vjo_queue_count() == 1 &&
                                !strcmp(g_view.anki_status, "Sent 0 to Anki, 1 kept (too large: the Overlay app sends it)")));
            TEST_MSG("%s: entries of %d B, %s text: record %u B, %d of 7 others kept", g_view.anki_status, sizes[z],
                     heavy ? "escaping" : "kana", (unsigned)strlen(json), kept);
            printf("measure: 8 dictionaries, entries of %d B (%s): record %u B with %d of 7 others, %s\n", sizes[z],
                   heavy ? "escaping" : "kana", (unsigned)strlen(json), kept, sent ? "sent" : "kept for the app");
            if (!heavy && sizes[z] <= 900)
                TEST_CHECK(kept == 7 && !logged("would be too large for △"));
            if (kept < 7)
                TEST_CHECK(logged("other dictionaries' entries kept: the next would be too large for △"));
            finish();
        }
    }
}

/* The largest card a record holds (8 dictionaries' long entries, under 32
 * KiB): its note does not fit the overlay's 128 KiB arena beside the record,
 * so △ keeps it and says the Overlay app sends it (test_anki_app sends it). */
static void test_largest_card_is_kept_for_the_app(void)
{
    static char text[8][3700], path[256];
    static const char *m[8][1];
    static uint8_t mem[96 * 1024];
    VjoAnkiGloss more[7];
    VjoAnkiNote n = {.spelling = "好き", .reading = "すき", .rank = VJO_NO_RANK, .sentence = "好き",
                     .hl_start = 0, .hl_end = 6, .sf_hl_start = -1, .sf_hl_end = -1, .misc = "G (2026-10-14 17:12)"};
    char id[VJO_QUEUE_ID_SIZE], *json;
    VjoArena a;
    FILE *f;
    reset();
    vjo_test_anki_text_only = 1;
    for (int k = 0; k < 8; k++) {
        int o = snprintf(text[k], sizeof(text[k]), "[Dictionary %d]\n", k);
        while (o + 3 < (int)sizeof(text[k]) - 1) {
            memcpy(text[k] + o, "好", 3);
            o += 3;
        }
        text[k][o] = '\0';
        m[k][0] = text[k];
        if (k)
            more[k - 1] = (VjoAnkiGloss){NULL, "好き", "すき", m[k], 1};
    }
    n.meanings = m[0];
    n.n_meanings = 1;
    n.more = more;
    n.n_more = 7;
    vjo_arena_init(&a, mem, sizeof(mem));
    TEST_ASSERT((json = vjo_queue_encode(&a, &n, 0)) != NULL && vjo_queue_count() == 0);
    vjo_queue_id(json, strlen(json), id);
    snprintf(path, sizeof(path), VJO_ANKI_QUEUE_DIR "/%s.json", id);
    TEST_ASSERT((f = fopen(path, "wb")) != NULL);
    fputs(json, f);
    fclose(f);
    server();
    triangle();
    TEST_CHECK(anki.adds == 0 && vjo_queue_count() == 1 &&
               !strcmp(g_view.anki_status, "Sent 0 to Anki, 1 kept (too large: the Overlay app sends it)"));
    TEST_MSG("%s (record %u B)", g_view.anki_status, (unsigned)strlen(json));
    TEST_CHECK(logged(" is too large to send in 131072 bytes; kept"));
    finish();
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
        "{\"version\":4,\"spelling\":\"猫\",\"reading\":\"\",\"sentence\":\"\",\"rank\":1,\"hl_start\":0,\"hl_end\":0,"
        "\"picture\":false,\"meanings\":[]}",
    };
    static const char *jm[] = {"[JMdict]\nto come"};
    static const VjoAnkiGloss more[1] = {{"JMdict", "来る", "くる", jm, 1}};
    VjoAnkiNote n = {.spelling = "来る", .reading = "くる", .rank = 384, .meanings = meanings, .n_meanings = 2,
                     .sentence = "明日来る", .hl_start = 6, .hl_end = 12, .sentence_furigana = "明日[あした]<b>来[く]る</b>",
                     .sf_hl_start = 18, .sf_hl_end = 34, .source = "Jiten", .pitch = {1, 0}, .n_pitch = 2,
                     .pos = pos, .n_pos = 2, .misc = "CLANNAD (2026-10-12 09:30)", .furigana = "来[く]る",
                     .dictionary = "Jiten", .more = more, .n_more = 1, .game = "CLANNAD", .title_id = "PCSG00415",
                     .platform = "vita", .unix_time = 1791727902, .ocr_engine = "vocr",
                     .ocr_model = "VOCR3_FL10_w8.vocr"}, d;
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
                            "\"time\":1791727902,\"ocr_confidence\":null}\n") != NULL);
    TEST_MSG("%s", json);
    TEST_ASSERT(vjo_queue_decode(&a, json, strlen(json), &d, &picture) == 3);
    TEST_CHECK(!strcmp(d.furigana, "来[く]る") && !strcmp(d.dictionary, "Jiten") && d.n_more == 1 &&
               !strcmp(d.more[0].dictionary, "JMdict") && !strcmp(d.more[0].spelling, "来る") &&
               d.more[0].n_meanings == 1 && !strcmp(d.more[0].meanings[0], jm[0]));
    TEST_CHECK(picture && !strcmp(d.spelling, n.spelling) && !strcmp(d.reading, n.reading) && d.rank == 384 &&
               d.n_meanings == 2 && !strcmp(d.meanings[1], "to arrive") && !strcmp(d.sentence, n.sentence) &&
               d.hl_start == 6 && d.hl_end == 12 && !strcmp(d.sentence_furigana, n.sentence_furigana) &&
               d.sf_hl_start == 18 && d.sf_hl_end == 34 && !strcmp(d.source, "Jiten") && d.n_pitch == 2 &&
               d.pitch[0] == 1 && d.pitch[1] == 0 && d.n_pos == 2 && !strcmp(d.pos[0], "vk") &&
               !strcmp(d.misc, n.misc) && !d.queue_tag);
    /* a different screenshot flag is a different card */
    again = vjo_queue_encode(&a, &n, 0);
    vjo_queue_id(again, strlen(again), id2);
    TEST_CHECK(strcmp(id, id2) != 0);
    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        vjo_arena_reset(&a);
        TEST_CHECK(vjo_queue_decode(&a, bad[i], strlen(bad[i]), &d, &picture) == -1);
        TEST_MSG("record %u", i);
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

/* Many cards: one allocation and one thread per save, one of each for the
 * whole send, and everything released. */
static void test_fifty_cards_then_one_send_release_everything(void)
{
    static char headers[50][64];
    size_t save_peak = 0;
    reset();
    for (int i = 0; i < 50; i++) {
        snprintf(headers[i], sizeof(headers[i]), "猫が好き%d", i);
        list.header = headers[i];
        g_view.list_seq = 100 + (unsigned)i;
        cross(i % 2);
        TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && !heap_live && job == JOB_NONE);
        if (last_peak > save_peak)
            save_peak = last_peak;
    }
    TEST_CHECK(vjo_queue_count() == 50 && files_ending(".jpg") == 50 && net.next == 0);
    TEST_CHECK(heap_allocs == 50 && heap_frees == 50 && threads_created == 50 && threads_deleted == 50);
    TEST_CHECK(captures == 50 && rows_read == 50u * FRAME_H && discards == 0);
    server();
    triangle();
    TEST_CHECK(anki.adds == 50 && anki.finds == 50 && anki.n_notes == 50 && vjo_queue_count() == 0 &&
               files_ending(".jpg") == 0 && !strcmp(g_view.anki_status, "Sent 50 to Anki"));
    TEST_CHECK(heap_allocs == 51 && heap_frees == 51 && threads_created == 51 && threads_deleted == 51);
    printf("measure: 50 picture cards: max save peak %u of %u; one send of all 50: peak %u of %u\n",
           (unsigned)save_peak, (unsigned)ANKI_PICTURE_MEM, (unsigned)last_peak, (unsigned)heap_last);
    TEST_CHECK(last_peak <= ANKI_PICTURE_MEM);
    finish();
}

/* A long text-only queue: one directory scan per card and a small peak. */
static void test_hundred_text_cards_send_in_bounded_memory(void)
{
    static char headers[100][64];
    reset();
    vjo_test_anki_text_only = 1;
    for (int i = 0; i < 100; i++) {
        snprintf(headers[i], sizeof(headers[i]), "言葉%dの猫が好き", i);
        list.header = headers[i];
        g_view.list_seq = 200 + (unsigned)i;
        entries[0].hl_start = -1;
        entries[0].hl_end = -1;
        cross(0);
    }
    TEST_ASSERT(vjo_queue_count() == 100);
    server();
    dir_scans = 0;
    triangle();
    TEST_CHECK(anki.adds == 100 && vjo_queue_count() == 0 && !strcmp(g_view.anki_status, "Sent 100 to Anki"));
    /* the count, one scan per card (which also counts), one finding none
     * left, and the check above */
    TEST_CHECK(dir_scans == 1 + 100 + 1 + 1);
    TEST_MSG("directory scans: %d", dir_scans);
    printf("measure: 100 text cards: send peak %u of %u\n", (unsigned)last_peak, (unsigned)heap_last);
    TEST_CHECK(heap_last == ANKI_TEXT_MEM && last_peak < 32 * 1024);
    finish();
}



/* SceShell refuses the VjoAnki thread. With anki_mining on it is created on every
 * overlay open (to count the queue), and for × and △: each refusal is in log.txt
 * with its rc, the bytes asked for, SceShell's free USER and the Shell pages we hold,
 * and the job that did not start; a thread that is created but does not start too. */
static void test_a_refused_job_thread_is_logged(void)
{
    reset();
    vjo_test_shell_free_rc = 0;
    vjo_test_shell_free_user = 28672; /* the lowest SceShell figure measured beside a game */
    fail_create = 1;
    vjo_anki_post_count();
    vjo_anki_reap();
    cross(0);
    triangle();
    fail_create = 0;
    TEST_CHECK(times_logged("thread VjoAnki refused 0xFFFFFFFF: 69632 B; SceShell free 28672 (0x00000000), held "
                            "282624\n") == 3);
    TEST_CHECK(logged("anki: count not started 0xFFFFFFFF\n") && logged("anki: save not started 0xFFFFFFFF\n") &&
               logged("anki: send not started 0xFFFFFFFF\n"));
    TEST_MSG("%s", all_logs);
    TEST_CHECK(!threads_created && !heap_attempts && !strcmp(g_view.anki_status, "Anki: not enough memory to send; cards kept"));
    /* created, then refused to start: deleted again, logged with the start's rc */
    fail_start = 1;
    cross(0);
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

#if !VJO_ANKI_SCREENSHOT
/* The shipped build (VJO_ANKI_SCREENSHOT 0). × with no anki_host (Wi-Fi
 * off, Anki closed): a text-only card is saved offline and the kernel is
 * never asked for a capture (its full-frame buffer is the 2,088,960 B block
 * #74's console refused). The job's arena is the ledger's
 * VJO_BUDGET_PAF_ANKI, every step is logged, and △ then sends the Kiku
 * note, without a picture. */
static void test_build_saves_text_cards_without_the_kernel_capture(void)
{
    static const char note[] =
        "{\"action\":\"addNote\",\"version\":6,\"params\":{\"note\":{\"deckName\":\"Kiku\","
        "\"modelName\":\"Kiku\",\"fields\":{\"Expression\":\"猫\",\"ExpressionFurigana\":\"猫[ねこ]\","
        "\"ExpressionReading\":\"ねこ\",\"MainDefinition\":\"<div style=\\\"text-align: left;\\\" "
        "class=\\\"yomitan-glossary\\\"><ol><li><span><ul><li>cat</li><li>shamisen &quot;neko&quot;</li></ul>"
        "</span></li></ol></div>\",\"Sentence\":\"<b>猫</b>が好き猫\",\"SentenceFurigana\":\"<b>猫</b>が好き猫\","
        "\"FreqSort\":\"1500\",\"MiscInfo\":\"CLANNAD (2026-10-12 09:30)\"},"
        "\"tags\":[\"vita-jp-overlay\",\"vita_queue_";
    const char *body;
    reset();
    cfg.anki_host[0] = '\0';
    reconfigure();
    cross(0);
    cross(1);
    TEST_CHECK(!strcmp(g_view.anki_status, "Saved for Anki") && g_view.anki_pending == 2);
    TEST_MSG("%s", g_view.anki_status);
    /* no capture of any kind, no network: two records, no screenshot */
    TEST_CHECK(captures == 0 && discards == 0 && rows_read == 0 && net.next == 0);
    TEST_CHECK(files_ending(".json") == 2 && files_ending(".jpg") == 0 && files_ending(".tmp") == 0);
    /* the text-only arena, which is the ledger's figure, freed after each job */
    TEST_CHECK(heap_last == ANKI_TEXT_MEM && ANKI_TEXT_MEM == VJO_BUDGET_PAF_ANKI && !heap_live &&
               heap_allocs == 2 && heap_frees == 2);
    TEST_CHECK(vjo_ledger_peak(&g_shell_ledger, VJO_SL_PAF_ANKI) == VJO_BUDGET_PAF_ANKI);
    TEST_CHECK(vjo_ledger_current(&g_shell_ledger, VJO_SL_PAF_ANKI) == 0);
    TEST_CHECK(vjo_ledger_peak(&g_shell_ledger, VJO_SL_ANKI_STACK) == VJO_BUDGET_SHELL_ANKI_STACK);
    /* every step logged: the record's bytes, the queue */
    TEST_CHECK(logged(" saved, ") && logged(" B record, 0 B picture\n"));
    TEST_CHECK(logged("anki: save 猫 rc=0, 0-byte screenshot, ") && logged("anki: save 好き rc=0, "));
    TEST_CHECK(logged("anki: queue 1 card(s)\n") && logged("anki: queue 2 card(s)\n"));
    TEST_CHECK(logged("anki: arena peak "));
    TEST_MSG("%s", all_logs);
    /* the queue survives the plugin's restart (every byte of memory gone) */
    finish();
    memset(&g_view, 0, sizeof(g_view));
    memset(heap, 0x5A, sizeof(heap));
    TEST_ASSERT(vjo_anki_start() == 0);
    fixture();
    reconfigure();
    vjo_anki_post_count();
    vjo_anki_reap();
    TEST_CHECK(g_view.anki_pending == 2);
    /* △ to AnkiConnect: the Kiku note without its Picture field and no picture */
    lan(4, ok(EMPTY), ok(NOTE_ID), ok(EMPTY), ok(NOTE_ID));
    triangle();
    TEST_CHECK(net.next == 4 && !strcmp(net.host[1], "192.168.1.23") && net.port[1] == VJO_ANKI_PORT);
    body = strstr(body_of(1), "\"Expression\":\"猫\"") ? body_of(1) : body_of(3);
    TEST_CHECK(!strncmp(body, note, strlen(note)) && !strstr(body, "\"picture\":") && !strstr(body, "Picture"));
    TEST_MSG("%.700s", body);
    TEST_CHECK(!strcmp(g_view.anki_status, "Sent 2 to Anki") && g_view.anki_pending == 0);
    TEST_CHECK(files_ending(".json") == 0 && captures == 0 && heap_last == ANKI_TEXT_MEM && !heap_live);
    TEST_CHECK(logged("anki: to 192.168.1.23:8765: vita_queue_") && logged("anki: send ") &&
               logged("anki: sent 2, 0 duplicates, 0 unreadable, 0 too large, 0 pending, "));
    finish();
}
#endif

TEST_LIST = {
    {"anki_off_by_default_starts_no_thread_and_allocates_nothing", test_off_by_default_allocates_nothing},
    {"japanese_text_and_escaping_round_trip_to_anki", test_japanese_and_escaping_round_trip},
    {"a_psp_games_card_says_psp", test_a_psp_games_card_says_psp},
    {"port_and_audio_resolved_at_send", test_mapping_port_and_audio_at_send},
    {"partial_send_keeps_the_rest_and_needs_anki_host", test_partial_send_keeps_the_rest},
    {"auto_host_sends_to_the_address_the_overlay_app_found", test_auto_finds_anki_on_the_lan},
    {"hundred_text_cards_send_with_one_scan_each_in_bounded_memory", test_hundred_text_cards_send_in_bounded_memory},
    {"a_refused_job_thread_is_logged_with_its_rc_and_sceshell_free", test_a_refused_job_thread_is_logged},
    {"version_2_record_from_the_test_build_sends_as_kiku", test_version_2_record_from_the_test_build_sends_as_kiku},
    {"cross_records_every_dictionary_and_sends_the_glossary", test_cross_records_every_dictionary_and_sends_the_glossary},
    {"largest_card_is_kept_by_triangle_for_the_overlay_app", test_largest_card_is_kept_for_the_app},
    {"same_card_saved_again_within_the_minute_is_one_record", test_same_card_within_a_minute_is_one_record},
    {"what_cross_saves_triangle_sends_whatever_the_dictionaries", test_what_cross_saves_triangle_sends},
#if VJO_ANKI_SCREENSHOT
    {"cross_saves_offline_without_host_or_network", test_cross_saves_offline_without_host},
    {"queue_survives_restart_and_triangle_sends_with_current_settings", test_queue_survives_restart_and_triangle_sends_it},
    {"triangle_sends_configured_note_tag_and_bounded_screenshot", test_send_builds_the_configured_note_with_screenshot},
    {"ankiconnect_failures_keep_the_card_and_the_overlay", test_failures_keep_the_card_and_the_overlay},
    {"lost_reply_failed_cleanup_and_bad_replies_retry_without_double_add", test_lost_reply_and_retry_add_once},
    {"duplicates_archived_with_screenshot_and_sending_continues", test_duplicates_are_archived_and_sending_continues},
    {"failed_writes_temp_files_and_damaged_cards_never_lose_cards", test_failed_writes_never_queue_a_partial_card},
    {"failed_directory_flush_is_retried_before_acknowledging", test_failed_directory_flush_is_retried},
    {"screenshot_failure_or_oversize_still_saves_the_card", test_screenshot_failures_still_save},
    {"oom_keeps_recognized_text_and_queued_cards", test_oom_keeps_recognized_text_and_cards},
    {"stale_list_close_stop_and_busy_jobs_cancel_without_leaks", test_cancellation},
    {"version_1_record_from_the_first_queue_still_sends", test_version_1_record_still_sends},
    {"record_round_trip_stable_ids_and_strict_decoding", test_record_format},
    {"fifty_cards_then_one_send_release_memory_and_threads", test_fifty_cards_then_one_send_release_everything},
#else
    {"build_saves_text_cards_offline_without_the_kernel_capture_and_sends_kiku",
     test_build_saves_text_cards_without_the_kernel_capture},
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

/* DIR ACTION [CRASH] [HOST]: save (the shown 猫), save-other (好き),
 * inspect (the first card), sync (△ to HOST) or mode (this binary's
 * VJO_ANKI_SCREENSHOT). Prints one JSON line. */
static int restart_step(int argc, char **argv)
{
    static uint8_t mem[ANKI_PICTURE_MEM];
    const char *what;
    VjoArena a;
    if (argc < 4 || chdir(argv[2]) < 0)
        return 2;
    what = argv[3];
    if (!strcmp(what, "mode")) {
        printf("{\"screenshot\":%d}\n", VJO_ANKI_SCREENSHOT);
        return 0;
    }
    mkdir("ux0:data", 0777);
    if (argc > 4 && argv[4][0])
        crash_point = argv[4];
    short_io = 1; /* genuinely incomplete writes at the *_partial checkpoints */
    use_posix = !strcmp(what, "sync");
    if (vjo_anki_start() < 0 || vjo_capture_init() < 0)
        return 2;
    fixture();
    cfg.anki_host[0] = '\0';
    if (argc > 5)
        snprintf(cfg.anki_host, sizeof(cfg.anki_host), "%s", argv[5]);
    vjo_anki_configure(&cfg);
    vjo_anki_set_game("PCSG00415", "CLANNAD");
    make_frame(0);
    vjo_arena_init(&a, mem, sizeof(mem));
    if (!strcmp(what, "save") || !strcmp(what, "save-other")) {
        vjo_anki_post_add(5, !strcmp(what, "save-other"));
        printf("{\"status\":");
        print_string(g_view.anki_status);
        printf(",\"count\":%d", g_view.anki_pending);
        if (!VJO_ANKI_SCREENSHOT) /* the shipped build: never a kernel capture */
            printf(",\"captures\":%u", captures + discards);
        printf("}\n");
        return strncmp(g_view.anki_status, "Saved", 5) && strncmp(g_view.anki_status, "Already", 7);
    }
    if (!strcmp(what, "inspect")) {
        char id[VJO_QUEUE_ID_SIZE], digest[VJO_QUEUE_ID_SIZE], *json;
        VjoAnkiNote n;
        VjoAnkiMedia m;
        int count = vjo_queue_next("", id);
        if (count < 0)
            return 3;
        if (!count) {
            puts("{\"count\":0}");
            return 0;
        }
        if (vjo_queue_load(&a, id, 1, &n, &m) < 0)
            return 4;
        vjo_queue_id((const char *)m.picture, m.picture_len, digest);
        if (!(json = vjo_queue_encode(&a, &n, m.picture_len != 0)))
            return 5;
        json[strlen(json) - 1] = '\0'; /* its newline */
        printf("{\"count\":%d,\"id\":\"%s\",\"picture_len\":%u,\"picture_hash\":\"%s\",\"note\":%s}\n", count, id,
               (unsigned)m.picture_len, m.picture_len ? digest : "", json);
        return 0;
    }
    if (!strcmp(what, "sync") && argc == 6) {
        vjo_anki_post_send();
        printf("{\"count\":%d,\"status\":", g_view.anki_pending);
        print_string(g_view.anki_status);
        printf("}\n");
        return 0;
    }
    return 2;
}

static pid_t test_dir_owner;
static void remove_tree(const char *path)
{
    char child[1024];
    struct stat st;
    struct dirent *e;
    DIR *d;
    if (lstat(path, &st) < 0)
        return;
    if (S_ISDIR(st.st_mode) && (d = opendir(path))) {
        while ((e = readdir(d))) {
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
                snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
                remove_tree(child);
            }
        }
        closedir(d);
    }
    remove(path);
}
/* acutest runs each test in a child that also exits through atexit. */
static void remove_test_dir(void)
{
    if (getpid() == test_dir_owner && chdir("/") == 0)
        remove_tree(test_dir);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "--restart-step"))
        return restart_step(argc, argv);
    /* One storage directory for every test (reset empties the queue). */
    snprintf(test_dir, sizeof(test_dir), "/tmp/vjo-anki-tests-XXXXXX");
    if (!mkdtemp(test_dir))
        return 2;
    test_dir_owner = getpid();
    atexit(remove_test_dir);
    if (chdir(test_dir) < 0 || mkdir("ux0:data", 0777) < 0)
        return 2;
    return acutest_main(argc, argv);
}
