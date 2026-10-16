/* shell/log_vita.c, the real file, on an in-memory file system: batched lines
 * (vjo_log_batch_begin/end) leave log.txt, log.old.txt and status.txt byte for
 * byte as the previous logger (main bcb780e's, unchanged since 4a7ae4d,
 * verbatim below as ref_*) leaves them line by line, through status.txt's
 * 64 KiB cap (a line that does not fit is skipped, a later shorter one is not),
 * log.txt's 1 MiB rotation, another thread logging inside a batch, a batch
 * fuller than its buffer, and logging before vjo_log_configure; with one open
 * per file per batch. Failed opens and writes lose lines but never the
 * logger's count of what each file holds. */
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
static unsigned opens[FILES], writes, renames, thread = 1, formats, log_path_opens, status_path_opens, status_writes;
static int fail_writes, fail_opens;

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
    File *f;
    (void)mode;
    if (fail_opens) return (int)0x80010018u; /* EMFILE */
    f = lookup(path, (flags & SCE_O_CREAT) != 0);
    if (!f) return -1;
    log_path_opens += !strcmp(path, VJO_LOG_PATH);
    status_path_opens += !strcmp(path, VJO_STATUS_PATH);
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
    status_writes += !strcmp(f->path, VJO_STATUS_PATH);
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
/* The two mutexes (VjoLog, VjoStatus), each held by at most one thread: a lock of a mutex
 * the caller holds already (the SDK's are not recursive), an unlock of one it does not hold,
 * or one still held when a logger call returns (lock_check) fails the test. */
static SceUID next_mutex = 41;
static struct { SceUID uid; SceUID holder; } mutexes[4];
static unsigned mutex_errors, lock_calls; /* lock_calls: the log's own mutex (log_mutex) taken, for the no-lock checks */
static int mutex_slot(SceUID uid)
{
    for (int i = 0; i < 4; i++)
        if (mutexes[i].uid == uid) return i;
    return -1;
}
SceUID sceKernelCreateMutex(const char *name, unsigned attr, int count, void *opt)
{
    (void)name; (void)attr; (void)count; (void)opt;
    for (int i = 0; i < 4; i++)
        if (!mutexes[i].uid) { mutexes[i].uid = next_mutex; mutexes[i].holder = 0; return next_mutex++; }
    return -1;
}
int sceKernelDeleteMutex(SceUID uid)
{
    int i = mutex_slot(uid);
    if (i < 0 || mutexes[i].holder) mutex_errors++;
    if (i >= 0) mutexes[i].uid = 0;
    return 0;
}
int sceKernelLockMutex(SceUID uid, int count, unsigned *timeout)
{
    int i = mutex_slot(uid);
    lock_calls += uid == log_mutex;
    (void)timeout;
    if (count != 1) mutex_errors++;
    if (i < 0 || mutexes[i].holder) { mutex_errors++; return -1; } /* a deadlock on the console */
    mutexes[i].holder = (SceUID)thread;
    return 0;
}
int sceKernelUnlockMutex(SceUID uid, int count)
{
    int i = mutex_slot(uid);
    if (count != 1) mutex_errors++;
    if (i < 0 || mutexes[i].holder != (SceUID)thread) { mutex_errors++; return -1; }
    mutexes[i].holder = 0;
    return 0;
}
/* After every logger call: nothing held, nothing misused. */
static void lock_check(void)
{
    for (int i = 0; i < 4; i++)
        if (mutexes[i].uid && mutexes[i].holder) mutex_errors++;
    TEST_CHECK(mutex_errors == 0);
    TEST_MSG("%u mutex errors", mutex_errors);
}
SceUID sceKernelGetThreadId(void) { return (SceUID)thread; }
/* The process clock: 1 ms more at every read, so every append takes 1 ms here. */
static uint64_t clock_us;
uint64_t sceKernelGetProcessTimeWide(void) { return clock_us += 1000; }

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

/* ---- the scenario ---- */

typedef struct {
    char *log, *old, *status;
    size_t log_n, old_n, status_n;
    unsigned log_opens, status_opens;
} Snapshot;

static void lock_check(void);
static void fs_reset(void)
{
    lock_check(); /* the previous scenario's lock use, before its record is cleared */
    for (int i = 0; i < FILES; i++) {
        free(files[i].data);
        memset(&files[i], 0, sizeof(files[i]));
    }
    memset(fds, 0, sizeof(fds));
    memset(opens, 0, sizeof(opens));
    memset(mutexes, 0, sizeof(mutexes));
    mutex_errors = 0;
    writes = renames = formats = log_path_opens = status_path_opens = status_writes = 0;
#if !VJO_DEBUG
    held = NULL; /* the logger's holder (vjo_log_hold): none */
    held_lines = 0;
#endif
    fail_writes = fail_opens = 0;
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
    s->log_opens = l ? opens[l - files] : 0;
    s->status_opens = st ? opens[st - files] : 0;
}

static int same(const char *a, size_t an, const char *b, size_t bn)
{
    return an == bn && !memcmp(a, b, an);
}

static void snap_free(Snapshot *s)
{
    free(s->log); free(s->old); free(s->status);
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
static void run(unsigned lines, size_t prefill, int file_on_, int threads, int bcap, Snapshot *ref, Snapshot *got)
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
        ref_status_append(line, n);
        if (ref_file_on) ref_file_append(line, n);
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
        log_mutex = -1; batch = NULL;
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
    lock_check();
}

static void check_same(const Snapshot *ref, const Snapshot *got)
{
    TEST_CHECK(same(ref->log, ref->log_n, got->log, got->log_n));
    TEST_CHECK(same(ref->old, ref->old_n, got->old, got->old_n));
    TEST_CHECK(same(ref->status, ref->status_n, got->status, got->status_n));
    TEST_MSG("log %zu/%zu old %zu/%zu status %zu/%zu", ref->log_n, got->log_n, ref->old_n, got->old_n, ref->status_n,
             got->status_n);
}

static void batches_write_the_same_bytes(void)
{
    Snapshot ref, got;
    /* Across status.txt's 64 KiB cap (about 500 lines), with threads. */
    run(1500, 0, 1, 1, 4096, &ref, &got);
    check_same(&ref, &got);
    TEST_CHECK(ref.status_n <= STATUS_CAP && ref.status_n > STATUS_CAP - 256);
    TEST_CHECK(got.log_opens < ref.log_opens);
    TEST_MSG("log.txt opened %u times line by line, %u batched", ref.log_opens, got.log_opens);
    snap_free(&ref); snap_free(&got);
    /* log.txt rotates inside a batch: 300 lines into a file 20 KiB under its cap. */
    run(300, LOG_FILE_CAP - 20000, 1, 0, 4096, &ref, &got);
    check_same(&ref, &got);
    TEST_CHECK(ref.old_n > LOG_FILE_CAP - 20000 && ref.log_n > 0);
    snap_free(&ref); snap_free(&got);
    /* A small buffer (lines often do not fit). */
    run(800, 0, 1, 1, 300, &ref, &got);
    check_same(&ref, &got);
    snap_free(&ref); snap_free(&got);
    /* log_file = off: status.txt only. */
    run(800, 0, 0, 1, 4096, &ref, &got);
    check_same(&ref, &got);
    TEST_CHECK(got.log_n == 0);
    snap_free(&ref); snap_free(&got);
    lock_check();
}

static void a_batch_is_one_open_per_file(void)
{
    VjoConfig cfg;
    VjoLogBatch b;
    char buf[2048], line[256];
    fs_reset();
    memset(&cfg, 0, sizeof(cfg));
    cfg.log_file = 1;
    log_mutex = -1; batch = NULL;
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
    lock_check();
}

#if VJO_DEBUG /* the debug build's own (docs/release-variants.md) */
/* ocr-first: every file append (log.txt, status.txt) is counted with its time, for
 * a job's done line (shell/worker.c): a batch of 16 lines is one append per file. */
static void appends_are_counted_with_their_time(void)
{
    VjoConfig cfg;
    VjoLogBatch b;
    char buf[2048], line[256];
    uint32_t n0, n1, n2, us0, us1, us2;
    fs_reset();
    memset(&cfg, 0, sizeof(cfg));
    cfg.log_file = 1;
    log_mutex = -1; batch = NULL;
    vjo_status_reset();
    vjo_log_configure(&cfg);
    us0 = vjo_log_io(&n0);
    vjo_log_raw(line, make_line(line, 0, 100)); /* log.txt and status.txt: two appends */
    us1 = vjo_log_io(&n1);
    TEST_CHECK(n1 - n0 == 2);
    TEST_CHECK(us1 - us0 >= 2000); /* the fake clock: 1 ms a read, two reads an append */
    TEST_CHECK(vjo_log_batch_begin(&b, buf, sizeof(buf)) == 1);
    for (unsigned i = 0; i < 16; i++) vjo_log_raw(line, make_line(line, i, 100));
    vjo_log_batch_end(&b);
    us2 = vjo_log_io(&n2);
    TEST_CHECK(n2 - n1 == 2); /* one per file */
    TEST_CHECK(us2 > us1);
    status_size = 64 * 1024; /* status.txt full: no append, none counted */
    vjo_log_raw(line, make_line(line, 1, 100));
    TEST_CHECK(vjo_log_io(&n0) > us2 && n0 - n2 == 1);
    fail_opens = 1; /* log.txt does not open (status.txt is still full): no append, none counted */
    vjo_log_raw(line, make_line(line, 2, 100));
    vjo_log_io(&n1);
    TEST_CHECK(n1 == n0);
    fail_opens = 0;
    lock_check();
}
#endif

static void before_configure_and_without_sinks(void)
{
    VjoLogBatch b;
    char buf[512];
    fs_reset();
    log_mutex = -1; batch = NULL; file_on = 0;
    vjo_status_reset();
    TEST_CHECK(vjo_log_batch_begin(&b, buf, sizeof(buf)) == 0); /* no batches before configure */
    vjo_log("boot %d", 1);
    TEST_CHECK(lookup(VJO_STATUS_PATH, 0)->size == 7 && !lookup(VJO_LOG_PATH, 0));
    /* status.txt full or closed, no file: nothing is even formatted */
    vjo_status_close();
    formats = 0;
    vjo_log("not formatted %d", 2);
    TEST_CHECK(formats == 0);
    lock_check();
}

/* A fresh logger (log_file on) after fs_reset, log.txt holding `prefill` bytes. */
static void fresh_logger(size_t prefill)
{
    VjoConfig cfg;
    fs_reset();
    if (prefill) {
        File *f = lookup(VJO_LOG_PATH, 1);
        f->data = malloc(prefill);
        memset(f->data, 'x', prefill);
        f->size = f->cap = prefill;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.log_file = 1;
    log_mutex = -1; batch = NULL;
    vjo_status_reset();
    vjo_log_configure(&cfg);
}

/* lens[0..n): line lengths; lines [from, to) in one batch, the rest one by one. Afterwards
 * log.txt, log.old.txt and status.txt must equal the line-by-line reference's. */
static void edge_run(const unsigned *lens, unsigned n, unsigned from, unsigned to, size_t prefill)
{
    Snapshot ref, got;
    char line[256 + 64];
    VjoLogBatch b;
    static char buf[8192];
    fresh_logger(prefill);
    ref_file_size = -1; ref_file_on = 1; ref_status_size = 0;
    for (unsigned i = 0; i < n; i++) {
        int k = make_line(line, i, lens[i]);
        ref_status_append(line, k);
        ref_file_append(line, k);
    }
    snap(&ref);
    fresh_logger(prefill);
    for (unsigned i = 0; i < n; i++) {
        int k = make_line(line, i, lens[i]);
        if (i == from) TEST_CHECK(vjo_log_batch_begin(&b, buf, sizeof(buf)) == 1);
        vjo_log_raw(line, k);
        if (i + 1 == to) vjo_log_batch_end(&b);
    }
    snap(&got);
    check_same(&ref, &got);
    snap_free(&ref); snap_free(&got);
    lock_check();
}

/* Lines that bring log.txt or status.txt to exactly their cap are written (not rotated,
 * not skipped); the line after rotates or is skipped; a later line that fits again is
 * written; a batch writes log.txt once per run between rotations. */
static void lines_at_the_caps(void)
{
    static unsigned lens[300];
    unsigned n = 0;
    /* log.txt 80 B under its cap: a batch of 30 + 50 (exactly the cap) + 1 (rotates) + 20 */
    unsigned l1[] = {30, 50, 1, 20};
    edge_run(l1, 4, 0, 4, LOG_FILE_CAP - 80);
    TEST_CHECK(log_path_opens == 2); /* the run before the rotation and the one after */
    TEST_MSG("log.txt opened %u times", log_path_opens);
    TEST_CHECK(lookup(VJO_LOG_OLD_PATH, 0) && lookup(VJO_LOG_OLD_PATH, 0)->size == LOG_FILE_CAP);
    /* a single line, outside a batch, that fills log.txt exactly: no rotation */
    unsigned l2[] = {50, 2};
    edge_run(l2, 2, 2, 2, LOG_FILE_CAP - 50);
    TEST_CHECK(lookup(VJO_LOG_OLD_PATH, 0) && lookup(VJO_LOG_OLD_PATH, 0)->size == LOG_FILE_CAP);
    /* status.txt: 255 lines of 255 B and one of 211 bring it 300 B under its cap; then, in a
     * batch, 250 (fits), 60 (skipped), 50 (fits: exactly the cap), 1 (skipped) */
    for (n = 0; n < 255; n++) lens[n] = 255;
    lens[n++] = 211;
    lens[n++] = 250; lens[n++] = 60; lens[n++] = 50; lens[n++] = 1;
    edge_run(lens, n, n - 4, n, 0);
    TEST_CHECK(lookup(VJO_STATUS_PATH, 0)->size == STATUS_CAP);
    /* the same lines one by one */
    edge_run(lens, n, n, n, 0);
    TEST_CHECK(lookup(VJO_STATUS_PATH, 0)->size == STATUS_CAP);
    /* 100 B under the cap (256 x 255 + 156 = 65,436), then a batch of 60 + 40: exactly the cap */
    for (n = 0; n < 256; n++) lens[n] = 255;
    lens[n++] = 156;
    lens[n++] = 60; lens[n++] = 40;
    edge_run(lens, n, n - 2, n, 0);
    TEST_CHECK(lookup(VJO_STATUS_PATH, 0)->size == STATUS_CAP);
    lock_check();
}

/* A batch whose run fails to write just under the cap: the next line is judged against
 * what the file really holds, as line by line, so log.txt does not rotate early. */
static void a_failed_run_at_the_cap(void)
{
    VjoLogBatch b;
    static char buf[1024];
    char line[256];
    fresh_logger(LOG_FILE_CAP - 100);
    TEST_ASSERT(vjo_log_batch_begin(&b, buf, sizeof(buf)) == 1);
    vjo_log_raw(line, make_line(line, 0, 10));
    vjo_log_raw(line, make_line(line, 1, 100));
    fail_writes = 1;
    vjo_log_batch_end(&b); /* 10 + 100 pass the cap: the 10 is written (fails), then the 100 */
    fail_writes = 0;
    /* line by line, 10 and then 100 would both fail and the file stay 100 B under its cap:
     * 100 more fit exactly, no rotation */
    TEST_CHECK(!lookup(VJO_LOG_OLD_PATH, 0));
    TEST_CHECK(file_size == LOG_FILE_CAP - 100 && lookup(VJO_LOG_PATH, 0)->size == LOG_FILE_CAP - 100);
    vjo_log_raw(line, make_line(line, 2, 100));
    TEST_CHECK(!lookup(VJO_LOG_OLD_PATH, 0) && lookup(VJO_LOG_PATH, 0)->size == LOG_FILE_CAP);
    lock_check();
}

/* status.txt: a batch that reaches the cap exactly is one write, not one per line. */
static void status_runs_are_one_write(void)
{
    VjoLogBatch b;
    static char buf[1024];
    char line[256];
    fresh_logger(0);
    for (unsigned i = 0; i < 256; i++) vjo_log_raw(line, make_line(line, i, 255)); /* 65,280 B */
    vjo_log_raw(line, make_line(line, 256, 156)); /* 100 B under the cap */
    status_writes = 0;
    TEST_ASSERT(vjo_log_batch_begin(&b, buf, sizeof(buf)) == 1);
    vjo_log_raw(line, make_line(line, 257, 60));
    vjo_log_raw(line, make_line(line, 258, 40));
    vjo_log_batch_end(&b);
    TEST_CHECK(lookup(VJO_STATUS_PATH, 0)->size == STATUS_CAP && status_writes == 1);
    TEST_MSG("status.txt %zu B in %u writes", lookup(VJO_STATUS_PATH, 0)->size, status_writes);
    lock_check();
}

/* A configuration change inside a batch: the lines logged before it go out under the old
 * settings (log_file on), as line by line; the ones after under the new (off). */
static void configure_inside_a_batch(void)
{
    VjoConfig cfg;
    VjoLogBatch b;
    char buf[1024], line[256];
    fresh_logger(0);
    TEST_ASSERT(vjo_log_batch_begin(&b, buf, sizeof(buf)) == 1);
    vjo_log_raw(line, make_line(line, 0, 40));
    vjo_log_raw(line, make_line(line, 1, 40));
    memset(&cfg, 0, sizeof(cfg));
    cfg.log_file = 0;
    vjo_log_configure(&cfg);
    vjo_log_raw(line, make_line(line, 2, 40));
    vjo_log_batch_end(&b);
    TEST_CHECK(lookup(VJO_LOG_PATH, 0) && lookup(VJO_LOG_PATH, 0)->size == 80);
    TEST_CHECK(lookup(VJO_STATUS_PATH, 0)->size == 120);
    lock_check();
}

/* t3-log-path: the normal build's network thread holds a job's lines in a batch until the control
 * thread has taken the job's result (vjo_log_hold, from shell/worker.c net_main), and the control thread re-reads
 * config.ini at every overlay open (vjo_log_configure), which may come during a job's lookup. With
 * log_file unchanged the batch stays held (no append on the control thread's way to the open overlay);
 * a change of log_file writes it first, as line by line. Either way the files hold what line by line
 * gives, a rotation of log.txt inside the batch included. The debug build writes the batch at every
 * configure, as before. */
static void configure_keeps_a_held_batch(void)
{
    static const unsigned lens[] = {40, 40, 40};
    Snapshot ref, got;
    VjoConfig cfg;
    VjoLogBatch b;
    char buf[1024], line[256];
    for (int prefill = 0; prefill < 2; prefill++) {
        size_t fill = prefill ? LOG_FILE_CAP - 50 : 0; /* the second line rotates log.txt */
        /* line by line (the reference): every configure keeps log_file on */
        fresh_logger(fill);
        ref_file_size = -1; ref_file_on = 1; ref_status_size = 0;
        for (unsigned i = 0; i < 3; i++) {
            int k = make_line(line, i, lens[i]);
            ref_status_append(line, k);
            ref_file_append(line, k);
        }
        snap(&ref);
        /* batched, a configure with log_file on (unchanged) after the second line */
        fresh_logger(fill);
        TEST_ASSERT(vjo_log_batch_begin(&b, buf, sizeof(buf)) == 1);
        vjo_log_raw(line, make_line(line, 0, lens[0]));
        vjo_log_raw(line, make_line(line, 1, lens[1]));
        memset(&cfg, 0, sizeof(cfg));
        cfg.log_file = 1;
        log_path_opens = status_path_opens = 0;
        vjo_log_configure(&cfg);
#if VJO_DEBUG
        TEST_CHECK(log_path_opens >= 1 && status_path_opens == 1); /* written at the configure, as before */
#else
        TEST_CHECK(log_path_opens == 0 && status_path_opens == 0); /* still held */
        TEST_MSG("log.txt opened %u times, status.txt %u", log_path_opens, status_path_opens);
#endif
        vjo_log_raw(line, make_line(line, 2, lens[2]));
        vjo_log_batch_end(&b);
        snap(&got);
        check_same(&ref, &got);
#if !VJO_DEBUG
        TEST_CHECK(status_path_opens == 1); /* the three lines in one append of status.txt */
#endif
        snap_free(&ref); snap_free(&got);
    }
    /* log_file turned off inside a held batch: the lines before go out under the old setting at once */
    fresh_logger(0);
    TEST_ASSERT(vjo_log_batch_begin(&b, buf, sizeof(buf)) == 1);
    vjo_log_raw(line, make_line(line, 0, 40));
    memset(&cfg, 0, sizeof(cfg));
    cfg.log_file = 0;
    vjo_log_configure(&cfg);
    TEST_CHECK(lookup(VJO_LOG_PATH, 0) && lookup(VJO_LOG_PATH, 0)->size == 40);
    vjo_log_raw(line, make_line(line, 1, 40));
    vjo_log_batch_end(&b);
    TEST_CHECK(lookup(VJO_LOG_PATH, 0)->size == 40 && lookup(VJO_STATUS_PATH, 0)->size == 80);
    /* and turned on again: the line logged while it was off reaches status.txt only */
    TEST_ASSERT(vjo_log_batch_begin(&b, buf, sizeof(buf)) == 1);
    vjo_log_raw(line, make_line(line, 2, 40));
    cfg.log_file = 1;
    vjo_log_configure(&cfg);
    vjo_log_raw(line, make_line(line, 3, 40));
    vjo_log_batch_end(&b);
    TEST_CHECK(lookup(VJO_LOG_PATH, 0)->size == 80 && lookup(VJO_STATUS_PATH, 0)->size == 160);
    lock_check();
}

/* audit-t3-log-path's: one thread holds a batch across many lines (re-begun after each end), as the
 * normal build's network thread does, while another thread logs now and then and a third re-reads the
 * config (log_file kept, or turned off and on), across status.txt's 64 KiB cap and, with a prefill, a
 * rotation of log.txt: the files equal line by line, byte for byte, in both builds. */
static void long_held_batch_with_configures(void)
{
    char line[256];
    for (unsigned seed = 1; seed <= 8; seed++) {
        size_t fill = seed & 1 ? LOG_FILE_CAP - 40000 : 0;
        const unsigned n = 1500;
        Snapshot ref, got;
        fresh_logger(fill); /* line by line */
        ref_file_size = -1; ref_file_on = 1; ref_status_size = 0;
        rng = seed * 7919u;
        for (unsigned i = 0; i < n; i++) {
            unsigned len = 2 + next(254), what = next(12), who = next(4);
            int k;
            (void)who;
            if (what == 0) { ref_file_on = !ref_file_on; ref_file_size = -1; } /* configure: log_file toggled */
            else if (what == 1) ref_file_size = -1;                           /* configure: log_file kept */
            k = make_line(line, i, len);
            ref_status_append(line, k);
            if (ref_file_on) ref_file_append(line, k);
        }
        snap(&ref);
        fresh_logger(fill); /* the logger */
        rng = seed * 7919u;
        {
            VjoLogBatch b;
            static char buf[2048];
            VjoConfig cfg;
            int open = 0, on = 1;
            memset(&cfg, 0, sizeof(cfg));
            for (unsigned i = 0; i < n; i++) {
                unsigned len = 2 + next(254), what = next(12), who = next(4);
                int k;
                if (what == 0 || what == 1) {
                    if (what == 0) on = !on;
                    cfg.log_file = on;
                    thread = 3; vjo_log_configure(&cfg); thread = 1;
                } else if (what == 2 && open) {
                    vjo_log_batch_end(&b);
                    open = 0;
                }
                if (!open) open = vjo_log_batch_begin(&b, buf, sizeof(buf));
                k = make_line(line, i, len);
                thread = who == 0 ? 2 : 1;
                vjo_log_raw(line, k);
                thread = 1;
            }
            if (open) vjo_log_batch_end(&b);
        }
        snap(&got);
        check_same(&ref, &got);
        TEST_MSG("seed %u, prefill %zu", seed, fill);
        if (seed == 1) TEST_CHECK(ref.status_n > STATUS_CAP - 256 && ref.old_n > 0); /* the cap, the rotation */
        snap_free(&ref); snap_free(&got);
        lock_check();
    }
}

#if !VJO_DEBUG
/* ---- the normal build's held lines (vjo_log_hold, vjo_log_release) ---- */

/* The holder's lines wait until it releases them, then go out with one open per file, the same bytes as
 * line by line; a release with nothing held, and a quiet holder, take no lock at all (audit-t3-log-path
 * N3: a job that logs nothing waits for no other thread's append). */
static void quiet_holder_takes_no_lock(void)
{
    VjoLogBatch b;
    static char buf[2048];
    char line[256];
    fresh_logger(0);
    vjo_log_hold(&b, buf, sizeof(buf));
    lock_calls = 0;
    vjo_log_release(0); /* nothing held */
    vjo_log_release(0);
    TEST_CHECK(lock_calls == 0);
    TEST_MSG("%u locks for releases with nothing held", lock_calls);
    /* another thread's line with nothing held: written at once, no batch begun for it (audit-t3-log-path-b
     * C1: a batch begun as that thread's would hold its lines in the holder's buffer) */
    thread = 2;
    vjo_log_raw(line, make_line(line, 9, 30));
    TEST_CHECK(!batch && lookup(VJO_LOG_PATH, 0) && lookup(VJO_LOG_PATH, 0)->size == 30);
    vjo_log_raw(line, make_line(line, 10, 30)); /* the holder's line next, from thread 1 below, is still held */
    TEST_CHECK(!batch && lookup(VJO_LOG_PATH, 0)->size == 60);
    vjo_log_release(0); /* not the holder: nothing held, nothing done */
    thread = 1;
    lock_calls = 0;
    vjo_log_release(0);
    TEST_CHECK(lock_calls == 0);
    log_path_opens = status_path_opens = 0;
    vjo_log_raw(line, make_line(line, 0, 40)); /* the holder's line: its batch begins, nothing written */
    vjo_log_raw(line, make_line(line, 1, 60));
    TEST_CHECK(lock_calls == 2 && !log_path_opens && !status_path_opens && batch == &b);
    vjo_log_release(0);
    TEST_CHECK(lock_calls == 3 && log_path_opens == 1 && status_path_opens == 1 && !batch);
    TEST_CHECK(lookup(VJO_LOG_PATH, 0)->size == 60 + 100 && lookup(VJO_STATUS_PATH, 0)->size == 60 + 100);
    lock_calls = 0;
    vjo_log_release(0); /* written: nothing held again */
    TEST_CHECK(lock_calls == 0);
    vjo_log_release(1);
    TEST_CHECK(!held);
    lock_check();
}

/* Another thread's line writes the held lines first, then its own, and closes the holder's batch, so
 * another thread's batch can begin (a game start's ledger table); the holder's line while that batch is
 * open goes out at once (the begin's window, microseconds: audit-t3-log-path N6), and once it ends the
 * holder's next line is held again. Every byte as line by line, in the order of the calls. */
static void another_threads_line_writes_the_held_lines(void)
{
    static const unsigned lens[] = {30, 40, 50, 60, 70, 80, 90};
    static const int who[] = {1, 1, 2, 2, 1, 2, 1}; /* 1: the holder; 2: another thread (its batch: lines 3-5) */
    Snapshot ref, got;
    VjoLogBatch b, other;
    static char buf[2048], obuf[1024];
    char line[256];
    fresh_logger(0); /* line by line */
    ref_file_size = -1; ref_file_on = 1; ref_status_size = 0;
    for (unsigned i = 0; i < 7; i++) {
        int k = make_line(line, i, lens[i]);
        ref_status_append(line, k);
        ref_file_append(line, k);
    }
    snap(&ref);
    fresh_logger(0);
    thread = 1;
    vjo_log_hold(&b, buf, sizeof(buf));
    for (unsigned i = 0; i < 7; i++) {
        int k = make_line(line, i, lens[i]);
        thread = (unsigned)who[i];
        if (i == 3) {
            TEST_CHECK(vjo_log_batch_begin(&other, obuf, sizeof(obuf)) == 1); /* the held batch was closed */
        }
        vjo_log_raw(line, k);
        if (i == 2) /* the holder's 2 lines, then the other's line: all written */
            TEST_CHECK(!batch && lookup(VJO_LOG_PATH, 0)->size == 30 + 40 + 50);
        if (i == 4) /* the holder's line, the other's batch open: the batch so far, then it, at once */
            TEST_CHECK(batch == &other && lookup(VJO_LOG_PATH, 0)->size == 30 + 40 + 50 + 60 + 70);
        if (i == 5) {
            vjo_log_batch_end(&other);
            TEST_CHECK(!batch);
        }
        if (i == 6) /* the holder's next line: held */
            TEST_CHECK(batch == &b && lookup(VJO_LOG_PATH, 0)->size == 30 + 40 + 50 + 60 + 70 + 80);
        thread = 1;
    }
    vjo_log_release(1);
    snap(&got);
    check_same(&ref, &got);
    snap_free(&ref); snap_free(&got);
    lock_check();
}

/* release(1): the holder holds no more (its stack is going): its later lines go out at once; a new
 * hold takes over from an earlier holder after writing what that one held. */
static void a_stopped_holder_holds_no_more(void)
{
    VjoLogBatch b, c;
    static char buf[2048], cbuf[2048];
    char line[256];
    fresh_logger(0);
    vjo_log_hold(&b, buf, sizeof(buf));
    vjo_log_raw(line, make_line(line, 0, 40));
    TEST_CHECK(!lookup(VJO_LOG_PATH, 0));
    vjo_log_release(1);
    TEST_CHECK(!held && !batch && lookup(VJO_LOG_PATH, 0)->size == 40);
    vjo_log_raw(line, make_line(line, 1, 40)); /* no holder: at once */
    TEST_CHECK(lookup(VJO_LOG_PATH, 0)->size == 80 && !batch);
    vjo_log_hold(&b, buf, sizeof(buf));
    vjo_log_raw(line, make_line(line, 2, 40));
    thread = 2;
    vjo_log_hold(&c, cbuf, sizeof(cbuf)); /* another holder: the first one's line goes out first */
    TEST_CHECK(held == &c && !batch && lookup(VJO_LOG_PATH, 0)->size == 120);
    vjo_log_raw(line, make_line(line, 3, 40));
    TEST_CHECK(batch == &c && lookup(VJO_LOG_PATH, 0)->size == 120);
    vjo_log_release(1);
    thread = 1;
    TEST_CHECK(!held && lookup(VJO_LOG_PATH, 0)->size == 160);
    lock_check();
}

/* A configuration change: log_file kept, the held lines stay held (the overlay's open re-reads it during
 * a job's lookup); turned off, they go out as they were and the batch closes; the holder's next line
 * begins it again, under the new setting. */
static void configure_and_the_held_lines(void)
{
    VjoLogBatch b;
    static char buf[2048];
    VjoConfig cfg;
    char line[256];
    fresh_logger(0);
    vjo_log_hold(&b, buf, sizeof(buf));
    vjo_log_raw(line, make_line(line, 0, 40));
    memset(&cfg, 0, sizeof(cfg));
    cfg.log_file = 1;
    thread = 3; vjo_log_configure(&cfg); thread = 1;
    TEST_CHECK(batch == &b && !lookup(VJO_LOG_PATH, 0)); /* still held */
    cfg.log_file = 0;
    thread = 3; vjo_log_configure(&cfg); thread = 1;
    TEST_CHECK(!batch && lookup(VJO_LOG_PATH, 0)->size == 40); /* written under log_file on */
    vjo_log_raw(line, make_line(line, 1, 40)); /* held again; it goes to status.txt only */
    TEST_CHECK(batch == &b);
    vjo_log_release(0);
    TEST_CHECK(!batch && lookup(VJO_LOG_PATH, 0)->size == 40 && lookup(VJO_STATUS_PATH, 0)->size == 80);
    vjo_log_release(1);
    lock_check();
}

/* More than the batch holds (24 lines, or past its buffer): it is written as it fills, one open per file
 * per run, and the rest stays held; the files as line by line. */
static void a_full_held_batch_is_written_as_it_fills(void)
{
    static unsigned lens[60];
    Snapshot ref, got;
    VjoLogBatch b;
    static char buf[2048];
    char line[256];
    for (unsigned i = 0; i < 60; i++) lens[i] = i < 30 ? 20 : 150; /* 30 short lines, then 30 that fill the buffer */
    fresh_logger(0);
    ref_file_size = -1; ref_file_on = 1; ref_status_size = 0;
    for (unsigned i = 0; i < 60; i++) {
        int k = make_line(line, i, lens[i]);
        ref_status_append(line, k);
        ref_file_append(line, k);
    }
    snap(&ref);
    fresh_logger(0);
    vjo_log_hold(&b, buf, sizeof(buf));
    log_path_opens = 0;
    for (unsigned i = 0; i < 60; i++) vjo_log_raw(line, make_line(line, i, lens[i]));
    /* 24 short lines (the 24-line table); then 6 short and 12 long (120 + 1,800 of the 2,048 B); then 13
     * long (1,950 B); 5 left held */
    TEST_CHECK(log_path_opens == 3 && batch == &b && b.n == 5);
    TEST_MSG("%u opens of log.txt while holding, %d lines held", log_path_opens, b.n);
    vjo_log_release(1);
    snap(&got);
    check_same(&ref, &got);
    snap_free(&ref); snap_free(&got);
    lock_check();
}

/* The holder's lines among other threads' and configures, released at random points, across status.txt's
 * cap and a rotation: the files equal line by line, byte for byte. */
static void held_lines_random(void)
{
    char line[256];
    for (unsigned seed = 1; seed <= 8; seed++) {
        size_t fill = seed & 1 ? LOG_FILE_CAP - 40000 : 0;
        const unsigned n = 1500;
        Snapshot ref, got;
        fresh_logger(fill);
        ref_file_size = -1; ref_file_on = 1; ref_status_size = 0;
        rng = seed * 104729u;
        for (unsigned i = 0; i < n; i++) {
            unsigned len = 2 + next(254), what = next(16), who = next(4);
            int k;
            (void)who;
            if (what == 0) { ref_file_on = !ref_file_on; ref_file_size = -1; }
            else if (what == 1) ref_file_size = -1;
            k = make_line(line, i, len);
            ref_status_append(line, k);
            if (ref_file_on) ref_file_append(line, k);
        }
        snap(&ref);
        fresh_logger(fill);
        rng = seed * 104729u;
        {
            VjoLogBatch b;
            static char buf[2048];
            VjoConfig cfg;
            int on = 1;
            memset(&cfg, 0, sizeof(cfg));
            thread = 1;
            vjo_log_hold(&b, buf, sizeof(buf));
            for (unsigned i = 0; i < n; i++) {
                unsigned len = 2 + next(254), what = next(16), who = next(4);
                int k;
                if (what == 0 || what == 1) {
                    if (what == 0) on = !on;
                    cfg.log_file = on;
                    thread = 3; vjo_log_configure(&cfg); thread = 1;
                } else if (what == 2) {
                    vjo_log_release(0);
                }
                k = make_line(line, i, len);
                thread = who == 0 ? 2 : 1;
                vjo_log_raw(line, k);
                thread = 1;
            }
            vjo_log_release(1);
        }
        snap(&got);
        check_same(&ref, &got);
        TEST_MSG("seed %u, prefill %zu", seed, fill);
        snap_free(&ref); snap_free(&got);
        lock_check();
    }
}
#endif

/* Another thread's line during a batch is written at once, after the batch's lines so far;
 * the batch goes on. */
static void another_thread_is_not_held(void)
{
    VjoLogBatch b;
    char buf[1024], line[256];
    fresh_logger(0);
    TEST_ASSERT(vjo_log_batch_begin(&b, buf, sizeof(buf)) == 1);
    for (unsigned i = 0; i < 3; i++) vjo_log_raw(line, make_line(line, i, 30));
    TEST_CHECK(!lookup(VJO_LOG_PATH, 0));
    thread = 2;
    vjo_log_raw(line, make_line(line, 3, 30));
    thread = 1;
    TEST_CHECK(lookup(VJO_LOG_PATH, 0) && lookup(VJO_LOG_PATH, 0)->size == 120);
    vjo_log_raw(line, make_line(line, 4, 30));
    TEST_CHECK(lookup(VJO_LOG_PATH, 0)->size == 120); /* held again */
    vjo_log_batch_end(&b);
    TEST_CHECK(lookup(VJO_LOG_PATH, 0)->size == 150);
    lock_check();
}

/* The batch never writes past its line table or its buffer: with guards after both, more
 * lines than VJO_LOG_BATCH_LINES, lines that fill the buffer and a line longer than it. */
static void a_batch_stays_inside_its_memory(void)
{
    struct { VjoLogBatch b; uint32_t guard[16]; } tb;
    struct { char buf[200]; unsigned char guard[300]; } mem;
    char line[256];
    fresh_logger(0);
    memset(tb.guard, 0xA5, sizeof(tb.guard));
    memset(mem.guard, 0x5A, sizeof(mem.guard));
    /* 25 short lines: the table holds 24, so two appends */
    TEST_ASSERT(vjo_log_batch_begin(&tb.b, mem.buf, sizeof(mem.buf)) == 1);
    for (unsigned i = 0; i < VJO_LOG_BATCH_LINES + 1; i++) vjo_log_raw(line, make_line(line, i, 5));
    vjo_log_batch_end(&tb.b);
    TEST_CHECK(log_path_opens == 2);
    TEST_MSG("log.txt opened %u times for %d lines", log_path_opens, VJO_LOG_BATCH_LINES + 1);
    /* 60-byte lines: three fill 180 of the 200 bytes, the fourth starts the next append */
    log_path_opens = 0;
    TEST_ASSERT(vjo_log_batch_begin(&tb.b, mem.buf, sizeof(mem.buf)) == 1);
    for (unsigned i = 0; i < 7; i++) vjo_log_raw(line, make_line(line, i, 60));
    vjo_log_batch_end(&tb.b);
    TEST_CHECK(log_path_opens == 3);
    /* a 250-byte line, longer than the buffer: written alone, at once */
    log_path_opens = 0;
    TEST_ASSERT(vjo_log_batch_begin(&tb.b, mem.buf, sizeof(mem.buf)) == 1);
    vjo_log_raw(line, make_line(line, 0, 20));
    vjo_log_raw(line, make_line(line, 1, 250));
    TEST_CHECK(log_path_opens == 2 && lookup(VJO_LOG_PATH, 0)->size == 25 * 5 + 7 * 60 + 270);
    vjo_log_batch_end(&tb.b);
    for (unsigned i = 0; i < 16; i++) TEST_CHECK(tb.guard[i] == 0xA5A5A5A5u);
    for (unsigned i = 0; i < sizeof(mem.guard); i++)
        if (mem.guard[i] != 0x5A) { TEST_CHECK(mem.guard[i] == 0x5A); TEST_MSG("guard byte %u", i); break; }
    lock_check();
}

/* Writes and opens that fail for a while, inside and outside batches: the
 * lines written meanwhile are lost (each batch's at most), the logger's sizes
 * stay the files' real sizes (so rotation and the 64 KiB cap stay right), and
 * every line after the failures is written. */
static void failures_lose_lines_but_not_the_sizes(void)
{
    VjoConfig cfg;
    VjoLogBatch b;
    static char buf[4096];
    char line[256];
    fs_reset();
    memset(&cfg, 0, sizeof(cfg));
    cfg.log_file = 1;
    log_mutex = -1; batch = NULL;
    vjo_status_reset();
    vjo_log_configure(&cfg);
    rng = 11;
    for (unsigned i = 0; i < 1200; i++) {
        int n = make_line(line, i, 2 + next(254));
        unsigned what = next(10);
        fail_writes = i >= 200 && i < 260;
        fail_opens = i >= 700 && i < 730;
        if (what == 0) {
            if (vjo_log_batch_begin(&b, buf, sizeof(buf))) {
                for (unsigned k = 0; k < 8; k++) vjo_log_raw(line, n);
                vjo_log_batch_end(&b);
            }
        } else {
            vjo_log_raw(line, n);
        }
        File *l = lookup(VJO_LOG_PATH, 0), *st = lookup(VJO_STATUS_PATH, 0);
        TEST_CHECK(file_size == (SceOff)(l ? l->size : 0));
        TEST_CHECK(status_size == (int)(st ? st->size : 0) && status_size <= STATUS_CAP);
    }
    fail_writes = fail_opens = 0;
    size_t before = lookup(VJO_LOG_PATH, 0)->size;
    vjo_log_raw("after\n", 6);
    TEST_CHECK(lookup(VJO_LOG_PATH, 0)->size == before + 6);
    lock_check();
}

/* A begin refused because another thread's batch is open (VjoControl's ledger table while VjoNet
 * formats a job's lines), and that thread's end: neither touches the open batch, neither holds a
 * mutex after it returns; the open batch's lines go out at its own end, after the other
 * thread's line (written at once, flushing the batch so far first). */
static void a_refused_begin_leaves_the_open_batch(void)
{
    VjoLogBatch a, b;
    char abuf[1024], bbuf[1024], line[256];
    fresh_logger(0);
    TEST_ASSERT(vjo_log_batch_begin(&a, abuf, sizeof(abuf)) == 1);
    vjo_log_raw(line, make_line(line, 0, 30));
    thread = 2;
    TEST_CHECK(vjo_log_batch_begin(&b, bbuf, sizeof(bbuf)) == 0);
    lock_check();
    vjo_log_raw(line, make_line(line, 1, 40)); /* thread 2's line: the open batch's goes first */
    vjo_log_batch_end(&b);                      /* refused: not the open batch */
    lock_check();
    thread = 1;
    TEST_CHECK(batch == &a);
    TEST_CHECK(lookup(VJO_LOG_PATH, 0) && lookup(VJO_LOG_PATH, 0)->size == 70);
    vjo_log_raw(line, make_line(line, 2, 20));
    vjo_log_batch_end(&a);
    TEST_CHECK(!batch && lookup(VJO_LOG_PATH, 0)->size == 90);
    lock_check();
}

/* Whether vjo_log formats a line: with log.txt on it does even once status.txt is full; without
 * it and status.txt full (or closed) it does not; with one byte of status.txt left it still does
 * (audit-overlay-perf-3: dropping file_on from the check silenced log.txt once status.txt was
 * full). */
static void sinks_decide_whether_vjo_log_formats(void)
{
    VjoConfig cfg;
    fresh_logger(0); /* log_file on */
    status_size = STATUS_CAP; /* full */
    formats = 0;
    vjo_log("to log.txt %d", 1);
    TEST_CHECK(formats == 1 && lookup(VJO_LOG_PATH, 0) && lookup(VJO_LOG_PATH, 0)->size == 13);
    TEST_CHECK(lookup(VJO_STATUS_PATH, 0) && lookup(VJO_STATUS_PATH, 0)->size == 0);
    memset(&cfg, 0, sizeof(cfg)); /* log_file off */
    vjo_log_configure(&cfg);
    formats = 0;
    vjo_log("nowhere %d", 3);
    TEST_CHECK(formats == 0 && lookup(VJO_LOG_PATH, 0)->size == 13);
    status_size = STATUS_CAP - 1; /* one byte left: a sink still */
    vjo_log("%s", "");
    TEST_CHECK(formats == 1 && lookup(VJO_STATUS_PATH, 0) && lookup(VJO_STATUS_PATH, 0)->size == 1);
    lock_check();
}

TEST_LIST = {
#if VJO_DEBUG
    {"appends_are_counted_with_their_time", appends_are_counted_with_their_time},
#endif
    {"batches_write_the_same_bytes", batches_write_the_same_bytes},
    {"a_batch_is_one_open_per_file", a_batch_is_one_open_per_file},
    {"before_configure_and_without_sinks", before_configure_and_without_sinks},
    {"failures_lose_lines_but_not_the_sizes", failures_lose_lines_but_not_the_sizes},
    {"lines_at_the_caps", lines_at_the_caps},
    {"configure_inside_a_batch", configure_inside_a_batch},
    {"configure_keeps_a_held_batch", configure_keeps_a_held_batch},
    {"long_held_batch_with_configures", long_held_batch_with_configures},
#if !VJO_DEBUG
    {"quiet_holder_takes_no_lock", quiet_holder_takes_no_lock},
    {"another_threads_line_writes_the_held_lines", another_threads_line_writes_the_held_lines},
    {"a_stopped_holder_holds_no_more", a_stopped_holder_holds_no_more},
    {"configure_and_the_held_lines", configure_and_the_held_lines},
    {"a_full_held_batch_is_written_as_it_fills", a_full_held_batch_is_written_as_it_fills},
    {"held_lines_random", held_lines_random},
#endif
    {"another_thread_is_not_held", another_thread_is_not_held},
    {"a_batch_stays_inside_its_memory", a_batch_stays_inside_its_memory},
    {"a_failed_run_at_the_cap", a_failed_run_at_the_cap},
    {"status_runs_are_one_write", status_runs_are_one_write},
    {"sinks_decide_whether_vjo_log_formats", sinks_decide_whether_vjo_log_formats},
    {"a_refused_begin_leaves_the_open_batch", a_refused_begin_leaves_the_open_batch},
    {NULL, NULL},
};
