/* shell/log_vita.c, the real file, on an in-memory file system: batched lines
 * (vjo_log_batch_begin/end) leave log.txt, log.old.txt and status.txt byte for
 * byte as the previous logger (4a7ae4d, verbatim below as ref_*) leaves them
 * line by line, through status.txt's 64 KiB cap (a line that does not fit is
 * skipped, a later shorter one is not), log.txt's 1 MiB rotation, another
 * thread logging inside a batch, a batch fuller than its buffer, and logging
 * before vjo_log_configure; with one open per file per batch. */
#include "acutest.h"

#include <stdlib.h>

#include <psp2/log_stubs.h>
#include "../../shell/log_vita.c"

/* ---- the in-memory file system ---- */

#define FILES 8
#define FDS 8
typedef struct {
    char path[96];
    char *data;
    size_t size, cap;
    int used;
} File;
static File files[FILES];
static struct { int file, open; } fds[FDS];
static unsigned opens[FILES], writes, renames, thread = 1, formats;
static int fail_writes;

static File *lookup(const char *path, int create)
{
    for (int i = 0; i < FILES; i++)
        if (files[i].used && !strcmp(files[i].path, path)) return &files[i];
    if (!create) return NULL;
    for (int i = 0; i < FILES; i++)
        if (!files[i].used) {
            memset(&files[i], 0, sizeof(files[i]));
            files[i].used = 1;
            snprintf(files[i].path, sizeof(files[i].path), "%s", path);
            return &files[i];
        }
    return NULL;
}

SceUID sceIoOpen(const char *path, int flags, int mode)
{
    File *f = lookup(path, (flags & SCE_O_CREAT) != 0);
    (void)mode;
    if (!f) return -1;
    if (flags & SCE_O_TRUNC) f->size = 0;
    for (int i = 0; i < FDS; i++)
        if (!fds[i].open) {
            fds[i].open = 1;
            fds[i].file = (int)(f - files);
            opens[f - files]++;
            return i + 3;
        }
    return -1;
}
int sceIoClose(SceUID fd) { fds[fd - 3].open = 0; return 0; }
int sceIoWrite(SceUID fd, const void *data, SceSize len)
{
    File *f = &files[fds[fd - 3].file];
    writes++;
    if (fail_writes) return -1;
    if (f->size + len > f->cap) {
        f->cap = (f->size + len) * 2;
        f->data = realloc(f->data, f->cap);
    }
    memcpy(f->data + f->size, data, len);
    f->size += len;
    return (int)len;
}
int sceIoGetstat(const char *path, SceIoStat *st)
{
    File *f = lookup(path, 0);
    if (!f) return (int)0x80010002u;
    st->st_mode = 0;
    st->st_size = (SceOff)f->size;
    return 0;
}
int sceIoRemove(const char *path)
{
    File *f = lookup(path, 0);
    if (!f) return (int)0x80010002u;
    free(f->data);
    memset(f, 0, sizeof(*f));
    return 0;
}
int sceIoRename(const char *from, const char *to)
{
    File *f = lookup(from, 0);
    if (!f || lookup(to, 0)) return -1;
    renames++;
    snprintf(f->path, sizeof(f->path), "%s", to);
    return 0;
}
int sceIoMkdir(const char *path, int mode) { (void)path; (void)mode; return 0; }

int sceClibVsnprintf(char *dst, SceSize cap, const char *fmt, va_list ap)
{
    formats++;
    return vsnprintf(dst, cap, fmt, ap);
}
SceUID sceKernelCreateMutex(const char *name, unsigned attr, int count, void *opt)
{
    (void)name; (void)attr; (void)count; (void)opt;
    return 41;
}
int sceKernelDeleteMutex(SceUID uid) { (void)uid; return 0; }
int sceKernelLockMutex(SceUID uid, int count, unsigned *timeout) { (void)uid; (void)count; (void)timeout; return 0; }
int sceKernelUnlockMutex(SceUID uid, int count) { (void)uid; (void)count; return 0; }
SceUID sceKernelGetThreadId(void) { return (SceUID)thread; }

static char udp[1 << 20];
static size_t udp_used;
static unsigned datagrams;
int sceNetSocket(const char *name, int domain, int type, int protocol)
{
    (void)name; (void)domain; (void)type; (void)protocol;
    return 77;
}
int sceNetSocketClose(int fd) { (void)fd; return 0; }
int sceNetSendto(int fd, const void *buf, unsigned len, int flags, const SceNetSockaddr *to, unsigned tolen)
{
    (void)fd; (void)flags; (void)to; (void)tolen;
    memcpy(udp + udp_used, buf, len);
    udp_used += len;
    datagrams++;
    return (int)len;
}
uint16_t sceNetHtons(uint16_t v) { return (uint16_t)(v << 8 | v >> 8); }
int sceNetInetPton(int af, const char *src, void *dst) { (void)af; (void)src; memset(dst, 0, 4); return 1; }

/* ---- the reference: shell/log_vita.c at 4a7ae4d, the file and status paths, verbatim ---- */

static SceOff ref_file_size = -1;
static int ref_file_on, ref_status_size = -1;

static void ref_file_append(const char *line, int len)
{
    SceUID fd;
    if (ref_file_size < 0) {
        SceIoStat st;
        ref_file_size = sceIoGetstat(VJO_LOG_PATH, &st) >= 0 ? st.st_size : 0;
    }
    if (ref_file_size + len > LOG_FILE_CAP) {
        sceIoRemove(VJO_LOG_OLD_PATH);
        sceIoRename(VJO_LOG_PATH, VJO_LOG_OLD_PATH);
        ref_file_size = 0;
    }
    fd = sceIoOpen(VJO_LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd < 0)
        return;
    if (sceIoWrite(fd, line, len) == len)
        ref_file_size += len;
    sceIoClose(fd);
}

static void ref_status_append(const char *line, int len)
{
    SceUID fd;
    if (ref_status_size < 0 || ref_status_size + len > STATUS_CAP)
        return;
    fd = sceIoOpen(VJO_STATUS_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd >= 0) {
        if (sceIoWrite(fd, line, len) == len)
            ref_status_size += len;
        sceIoClose(fd);
    }
}

static void ref_log_raw(const char *line, int len, int configured)
{
    if (len <= 0)
        return;
    ref_status_append(line, len);
    if (!configured)
        return;
    sceNetSendto(77, line, len, 0, NULL, 0);
    if (ref_file_on)
        ref_file_append(line, len);
}

/* ---- the scenario ---- */

typedef struct {
    char *log, *old, *status, *udp;
    size_t log_n, old_n, status_n, udp_n;
    unsigned log_opens, status_opens, datagrams;
} Snapshot;

static void fs_reset(void)
{
    for (int i = 0; i < FILES; i++) {
        free(files[i].data);
        memset(&files[i], 0, sizeof(files[i]));
    }
    memset(fds, 0, sizeof(fds));
    memset(opens, 0, sizeof(opens));
    udp_used = datagrams = writes = renames = formats = 0;
    fail_writes = 0;
    thread = 1;
}

static char *copy(const File *f, size_t *n)
{
    char *p = malloc(f && f->size ? f->size : 1);
    *n = f ? f->size : 0;
    if (f && f->size) memcpy(p, f->data, f->size);
    return p;
}

static void snap(Snapshot *s)
{
    File *l = lookup(VJO_LOG_PATH, 0), *o = lookup(VJO_LOG_OLD_PATH, 0), *st = lookup(VJO_STATUS_PATH, 0);
    s->log = copy(l, &s->log_n);
    s->old = copy(o, &s->old_n);
    s->status = copy(st, &s->status_n);
    s->udp = malloc(udp_used ? udp_used : 1);
    memcpy(s->udp, udp, udp_used);
    s->udp_n = udp_used;
    s->log_opens = l ? opens[l - files] : 0;
    s->status_opens = st ? opens[st - files] : 0;
    s->datagrams = datagrams;
}

static int same(const char *a, size_t an, const char *b, size_t bn)
{
    return an == bn && !memcmp(a, b, an);
}

static void snap_free(Snapshot *s)
{
    free(s->log); free(s->old); free(s->status); free(s->udp);
}

/* A log line of length len (2..255) with its index in it. */
static int make_line(char *buf, unsigned i, unsigned len)
{
    int n = snprintf(buf, 256, "line %u:", i);
    while ((unsigned)n < len - 1) {
        buf[n] = (char)('a' + (i + (unsigned)n) % 26);
        n++;
    }
    buf[len - 1] = '\n';
    return (int)len;
}

static uint32_t rng = 1;
static unsigned next(unsigned mod)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng % mod;
}

/* The same lines through the reference line by line, and through the new
 * logger with random batches (and threads); prefill: bytes already in log.txt. */
static void run(unsigned lines, size_t prefill, int file_on_, int udp_on, int threads, int bcap, Snapshot *ref,
                Snapshot *got)
{
    char line[256];
    char *fill = calloc(1, prefill + 1);
    memset(fill, 'x', prefill);
    /* reference */
    fs_reset();
    if (prefill) {
        File *f = lookup(VJO_LOG_PATH, 1);
        f->data = malloc(prefill); memcpy(f->data, fill, prefill); f->size = f->cap = prefill;
    }
    ref_file_size = -1; ref_file_on = file_on_; ref_status_size = 0;
    { SceUID fd = sceIoOpen(VJO_STATUS_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666); sceIoClose(fd); }
    rng = 7;
    for (unsigned i = 0; i < lines; i++) {
        int n = make_line(line, i, 2 + next(254));
        (void)next(6); (void)next(3); /* keep the stream aligned with the batched run */
        if (udp_on) ref_log_raw(line, n, 1);
        else { ref_status_append(line, n); if (ref_file_on) ref_file_append(line, n); }
    }
    snap(ref);
    /* the new logger */
    fs_reset();
    if (prefill) {
        File *f = lookup(VJO_LOG_PATH, 1);
        f->data = malloc(prefill); memcpy(f->data, fill, prefill); f->size = f->cap = prefill;
    }
    {
        VjoConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.log_file = file_on_;
        if (udp_on) snprintf(cfg.log_host, sizeof(cfg.log_host), "192.168.1.2");
        log_mutex = -1; udp_fd = -1; batch = NULL;
        vjo_status_reset();
        vjo_log_configure(&cfg);
    }
    rng = 7;
    {
        VjoLogBatch b;
        static char buf[4096];
        int open = 0;
        for (unsigned i = 0; i < lines; i++) {
            int n = make_line(line, i, 2 + next(254));
            unsigned what = next(6), who = next(3);
            if (!open && what == 0) open = vjo_log_batch_begin(&b, buf, bcap);
            else if (open && what == 1) { vjo_log_batch_end(&b); open = 0; }
            thread = threads && who == 0 ? 2 : 1; /* thread 2 never owns the batch */
            vjo_log_raw(line, n);
            thread = 1;
        }
        if (open) vjo_log_batch_end(&b);
    }
    snap(got);
    free(fill);
}

static void check_same(const Snapshot *ref, const Snapshot *got, int udp_on)
{
    TEST_CHECK(same(ref->log, ref->log_n, got->log, got->log_n));
    TEST_CHECK(same(ref->old, ref->old_n, got->old, got->old_n));
    TEST_CHECK(same(ref->status, ref->status_n, got->status, got->status_n));
    if (udp_on) TEST_CHECK(same(ref->udp, ref->udp_n, got->udp, got->udp_n) && ref->datagrams == got->datagrams);
    TEST_MSG("log %zu/%zu old %zu/%zu status %zu/%zu", ref->log_n, got->log_n, ref->old_n, got->old_n, ref->status_n,
             got->status_n);
}

static void batches_write_the_same_bytes(void)
{
    Snapshot ref, got;
    /* Across status.txt's 64 KiB cap (about 500 lines), with threads. */
    run(1500, 0, 1, 0, 1, 4096, &ref, &got);
    check_same(&ref, &got, 0);
    TEST_CHECK(ref.status_n <= STATUS_CAP && ref.status_n > STATUS_CAP - 256);
    TEST_CHECK(got.log_opens < ref.log_opens);
    TEST_MSG("log.txt opened %u times line by line, %u batched", ref.log_opens, got.log_opens);
    snap_free(&ref); snap_free(&got);
    /* log.txt rotates inside a batch: 300 lines into a file 20 KiB under its cap. */
    run(300, LOG_FILE_CAP - 20000, 1, 0, 0, 4096, &ref, &got);
    check_same(&ref, &got, 0);
    TEST_CHECK(ref.old_n > LOG_FILE_CAP - 20000 && ref.log_n > 0);
    snap_free(&ref); snap_free(&got);
    /* A small buffer (lines often do not fit), UDP on: the same datagrams too. */
    run(800, 0, 1, 1, 1, 300, &ref, &got);
    check_same(&ref, &got, 1);
    snap_free(&ref); snap_free(&got);
    /* log_file = off: status.txt only. */
    run(800, 0, 0, 0, 1, 4096, &ref, &got);
    check_same(&ref, &got, 0);
    TEST_CHECK(got.log_n == 0);
    snap_free(&ref); snap_free(&got);
}

static void a_batch_is_one_open_per_file(void)
{
    VjoConfig cfg;
    VjoLogBatch b;
    char buf[2048], line[256];
    fs_reset();
    memset(&cfg, 0, sizeof(cfg));
    cfg.log_file = 1;
    log_mutex = -1; udp_fd = -1; batch = NULL;
    vjo_status_reset();
    vjo_log_configure(&cfg);
    TEST_CHECK(vjo_log_batch_begin(&b, buf, sizeof(buf)) == 1);
    TEST_CHECK(vjo_log_batch_begin(&b, buf, sizeof(buf)) == 0); /* one batch at a time */
    for (unsigned i = 0; i < 16; i++) vjo_log_raw(line, make_line(line, i, 100));
    TEST_CHECK(opens[lookup(VJO_STATUS_PATH, 0) - files] == 1); /* only vjo_status_reset's */
    TEST_CHECK(!lookup(VJO_LOG_PATH, 0));                      /* nothing written yet */
    vjo_log_batch_end(&b);
    TEST_CHECK(opens[lookup(VJO_LOG_PATH, 0) - files] == 1);
    TEST_CHECK(opens[lookup(VJO_STATUS_PATH, 0) - files] == 2);
    TEST_CHECK(lookup(VJO_LOG_PATH, 0)->size == 1600);
    /* more lines than the batch holds: written as it fills, nothing lost */
    TEST_CHECK(vjo_log_batch_begin(&b, buf, sizeof(buf)) == 1);
    for (unsigned i = 0; i < 2 * VJO_LOG_BATCH_LINES + 3; i++) vjo_log_raw(line, make_line(line, i, 10));
    vjo_log_batch_end(&b);
    TEST_CHECK(lookup(VJO_LOG_PATH, 0)->size == 1600 + 10 * (2 * VJO_LOG_BATCH_LINES + 3));
    TEST_CHECK(opens[lookup(VJO_LOG_PATH, 0) - files] == 1 + 3);
}

static void before_configure_and_without_sinks(void)
{
    VjoLogBatch b;
    char buf[512];
    fs_reset();
    log_mutex = -1; udp_fd = -1; batch = NULL; file_on = 0;
    vjo_status_reset();
    TEST_CHECK(vjo_log_batch_begin(&b, buf, sizeof(buf)) == 0); /* no batches before configure */
    vjo_log("boot %d", 1);
    TEST_CHECK(lookup(VJO_STATUS_PATH, 0)->size == 7 && !lookup(VJO_LOG_PATH, 0));
    /* status.txt full or closed, no file, no UDP: nothing is even formatted */
    vjo_status_close();
    formats = 0;
    vjo_log("not formatted %d", 2);
    TEST_CHECK(formats == 0);
}

TEST_LIST = {
    {"batches_write_the_same_bytes", batches_write_the_same_bytes},
    {"a_batch_is_one_open_per_file", a_batch_is_one_open_per_file},
    {"before_configure_and_without_sinks", before_configure_and_without_sinks},
    {NULL, NULL},
};
