/* test_vocr_parallel.c - parallel lines (core/vocr_ocr.h VjoVocrPool) and the
 * line cache (VjoVocrCache) with real vocr_run and vocr_lm_decode calls. The test
 * builds two tiny valid weights blobs (the layer kinds of the release models:
 * stem, depthwise, pointwise, the 2x1 collapse, 1x5 temporal depthwise with a
 * residual pointwise, the head; vita-vn-ocr runtime/tests/test_vocr.c's builder)
 * and a tiny character LM (vita-vn-ocr runtime/tests/test_vocr_lm.c's), pins
 * them in the model and LM tables (test_parallel_models.h), and runs jobs on a
 * synthetic region of five lines:
 *  - serial against 1 and 2 helpers, greedy, with the LM, and with the LM and the
 *    cascade (the second model reading every line again), with real threads,
 *    slow ones, a pool that runs each line at once and one whose threads never
 *    start: the same text, greedy text, frames and per-line results in line
 *    order, every helper joined, the last line on the caller's thread;
 *  - the capacity: for every text capacity the serial job's rc and text, with
 *    helpers and with a warm cache;
 *  - the cache: a second job on the same screen runs no recognizer and gives the
 *    same text; one changed line is the only one recognized again; another
 *    variant or another model shares no entry; a line the cascade's time
 *    ceiling skipped is not kept; weights read from the file again empty it;
 *  - cancellation while helpers run (every helper joined, nothing cached), a
 *    failed join (the job fails and never retries the region; every later job
 *    is refused until vjo_vocr_unstick), the game's pool after a timed-out join;
 *    fewer helpers when the workspace holds fewer; the busy hook.
 * The cancellation, failed-job, capacity and failed-join cases follow
 * audit-ocr-speed-2's .refs/probe/test_audit_extra.c. */
#define main acutest_main
#include "acutest.h"
#undef main
#include "test_tmpdir.h"
#include "platform_posix.h"
#include "vocr.h"
#include "vocr_lm.h"
#include "vocr_ocr.h"
#include "../../core/fbgrey.h"
#include "cache_replay.h" /* #418: every cache operation replayed through the old layout (setup installs it) */
#include <bearssl_hash.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void test_pin_model(unsigned which, uint32_t bytes, const unsigned char sha256[32], uint32_t arena_bytes);
void test_pin_lm(uint32_t bytes, const unsigned char sha256[32]);
uint32_t vjo_vocr_test_len_left(void);
void vjo_vocr_test_lm_counts(unsigned *parses, unsigned *reuses);
void vjo_vocr_test_row_sums(const unsigned char *rows, unsigned w, unsigned n, uint32_t *sums);

#define M1 "TINY_w8.vocr"
#define M2 "TINY2_w8.vocr"

#include "fakes/vocr_blob.h" /* the tiny blobs, model_dir and write_file */

/* The two blobs and the LM in files, pinned in the tables. */
static void setup_files(void)
{
    static int done;
    static const uint32_t seeds[2] = {12345u, 777u};
    static const char *const names[2] = {M1, M2};
    unsigned char sha[32];
    vocr_model m;
    vocr_lm lm;
    vocr_opts o;
    size_t n;
    if (done) return;
    /* The product opens DIR/NAME in 256 bytes (core/vocr_ocr.c join_path): a long TMPDIR is refused here. */
    TEST_ASSERT_(vjo_test_tmpdir_new(model_dir, sizeof model_dir) != NULL &&
                     strlen(model_dir) + 1 + sizeof(M2) - 1 < 256,
                 "no models' directory in %s with room for %s in the product's 256-byte model path "
                 "(core/vocr_ocr.c join_path): is TMPDIR too long?", vjo_test_tmpdir_path, M2);
    for (unsigned i = 0; i < 2; i++) {
        n = build_blob(seeds[i]);
        TEST_ASSERT(vocr_load(&m, blob, n) == VOCR_OK);
        memset(&o, 0, sizeof o);
        o.mode = VOCR_MODE_W8I;
        write_file(names[i], blob, n, sha);
        test_pin_model(i, (uint32_t)n, sha, (uint32_t)vocr_arena_bytes(&m, &o));
    }
    n = build_lm();
    TEST_ASSERT(vocr_lm_load(&lm, lmb, n) == VOCR_OK);
    write_file("TINY_lm.vlm", lmb, n, sha);
    test_pin_lm((uint32_t)n, sha);
    done = 1;
}

/* ------------------------------------------------------------ the region */
#define W 420
#define H 200
#define LINES 5
static unsigned char pixels[W * H * 4];
static unsigned strips_read, cancel_after, tear_at, tear_seen;
static int cancel_now;
static void draw(unsigned which, unsigned seed);

static int strip(void *ud, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags)
{
    (void)ud; (void)flags;
    if (y + n > H) return VJO_E_SOURCE;
    strips_read++;
    if (tear_at && strips_read == tear_at) draw(2, 3);           /* the screen changes under the job */
    if (tear_at && strips_read >= tear_at && y < 112 && y + n > 88) tear_seen = 1; /* line 2's rows read */
    if (cancel_after && strips_read >= cancel_after) __atomic_store_n(&cancel_now, 1, __ATOMIC_RELEASE);
    for (unsigned r = 0; r < n; r++)
        for (unsigned x = 0; x < W; x++) {
            const unsigned char *p = pixels + ((size_t)(y + r) * W + x) * 4;
            dst[(size_t)r * stride + x] = (unsigned char)vjo_grey(p[0], p[1], p[2]);
        }
    return (int)n;
}
/* A P_LATE helper (below) counts its calls of the image's cancel test. */
static __thread int on_late_helper;
static unsigned late_cancel_calls;
static int is_cancelled(void *ud)
{
    (void)ud;
    if (on_late_helper) __atomic_fetch_add(&late_cancel_calls, 1u, __ATOMIC_RELAXED);
    return __atomic_load_n(&cancel_now, __ATOMIC_ACQUIRE);
}
static VjoGreySource im = {NULL, W, H, strip, is_cancelled, NULL, NULL};
static unsigned char capture[64 * 1024];

/* LINES lines of glyph-like strokes, each with its own spacing and length (seed
 * moves line `which`'s strokes, so its pixels change and the others' do not). */
static void draw(unsigned which, unsigned seed)
{
    for (size_t i = 0; i < sizeof(pixels); i += 4) {
        pixels[i] = pixels[i + 1] = pixels[i + 2] = 20;
        pixels[i + 3] = 255;
    }
    for (unsigned l = 0; l < LINES; l++) {
        unsigned y0 = 12 + l * 38, step = 5 + l, len = W - 30 - 40 * l, shift = l == which ? seed : 0;
        for (unsigned y = y0; y < y0 + 24; y++)
            for (unsigned x = 15 + shift; x < len; x += step)
                for (unsigned k = 0; k < 2 + (x / 7 + l) % 2; k++) {
                    unsigned char *p = pixels + ((size_t)y * W + x + k) * 4;
                    p[0] = p[1] = p[2] = (unsigned char)(200 + 7 * ((x / 11 + y) % 5));
                }
    }
}

/* ------------------------------------------------------------ pools */
enum {
    P_THREADS,      /* a thread per line, joined */
    P_AT_ONCE,      /* run() runs the line before it returns */
    P_NO_START,     /* run() fails: the line runs on the caller */
    P_JOIN_FAILS,   /* threads; join() fails and leaves the thread running */
    P_SLOW,         /* threads that sleep 30 ms before their line */
    P_GAME,         /* game_ocr/main.c's: threads that sleep 40 ms; the first join after the screen changed
                     * times out, and then the slot stays taken (run() refuses it) */
    P_LATE          /* threads that wait until the test lets them go; join() fails at once, so they run
                     * their line after the job has stopped and returned */
};
static int late_release;
typedef struct {
    pthread_t t[1 + VJO_VOCR_HELPERS_MAX];
    void (*fn[1 + VJO_VOCR_HELPERS_MAX])(void *);
    void *arg[1 + VJO_VOCR_HELPERS_MAX];
    int live[1 + VJO_VOCR_HELPERS_MAX];   /* a thread was started and not joined */
    int mode;
    unsigned runs, joins, max_live, now_live, failed_joins, runs_after_fail, stuck_slot;
    unsigned strips_at_fail;  /* strips the job had read when the first join failed */
    pthread_mutex_t mu;
} Pool;

static void nap_ms(unsigned ms)
{
    struct timespec d = {0, (long)ms * 1000000L};
    nanosleep(&d, NULL);
}
static void *tramp(void *p)
{
    Pool *pl = ((void **)p)[0];
    unsigned slot = (unsigned)(uintptr_t)((void **)p)[1];
    free(p);
    if (pl->mode == P_SLOW) nap_ms(30);
    if (pl->mode == P_GAME) nap_ms(40);
    if (pl->mode == P_LATE) {
        for (unsigned i = 0; i < 10000 && !__atomic_load_n(&late_release, __ATOMIC_ACQUIRE); i++) nap_ms(1);
        on_late_helper = 1;
    }
    pl->fn[slot](pl->arg[slot]);
    return NULL;
}
static int pool_run(void *ud, unsigned slot, void (*fn)(void *), void *arg)
{
    Pool *pl = (Pool *)ud;
    void **a;
    if (pl->failed_joins) pl->runs_after_fail++;
    if (pl->mode == P_GAME && pl->failed_joins && slot == pl->stuck_slot) return -1; /* helper_uid[slot] still set */
    TEST_CHECK(slot >= 1 && slot <= VJO_VOCR_HELPERS_MAX && !pl->live[slot]);
    if (pl->mode == P_NO_START) return -1;
    pl->fn[slot] = fn;
    pl->arg[slot] = arg;
    pl->runs++;
    pthread_mutex_lock(&pl->mu);
    if (++pl->now_live > pl->max_live) pl->max_live = pl->now_live;
    pthread_mutex_unlock(&pl->mu);
    if (pl->mode == P_AT_ONCE) {
        fn(arg);
        return 0;
    }
    a = malloc(2 * sizeof(void *));
    a[0] = pl;
    a[1] = (void *)(uintptr_t)slot;
    if (pthread_create(&pl->t[slot], NULL, tramp, a)) {
        free(a);
        return -1;
    }
    pl->live[slot] = 1;
    return 0;
}
static int pool_join(void *ud, unsigned slot)
{
    Pool *pl = (Pool *)ud;
    if (pl->mode == P_JOIN_FAILS || pl->mode == P_LATE || (pl->mode == P_GAME && (tear_seen || pl->failed_joins))) {
        if (!pl->failed_joins++) {
            pl->stuck_slot = slot;
            pl->strips_at_fail = strips_read;
        }
        return -1; /* WaitThreadEnd timed out: the thread may still run */
    }
    if (pl->live[slot]) pthread_join(pl->t[slot], NULL);
    pl->live[slot] = 0;
    pl->joins++;
    pthread_mutex_lock(&pl->mu);
    pl->now_live--;
    pthread_mutex_unlock(&pl->mu);
    return 0;
}
static void pool_init(Pool *pl, int mode)
{
    memset(pl, 0, sizeof(*pl));
    pthread_mutex_init(&pl->mu, NULL);
    pl->mode = mode;
}
/* every thread the pool started has ended; then a stuck job no longer holds the module */
/* A stuck job's workspace (a helper may still run in it): run() keeps it here, and
 * pool_done frees it once the threads are joined, as the game's worker_stop does. */
static void *stuck_ws;

static void pool_done(Pool *pl)
{
    for (unsigned k = 1; k <= VJO_VOCR_HELPERS_MAX; k++)
        if (pl->live[k]) {
            pthread_join(pl->t[k], NULL);
            pl->live[k] = 0;
        }
    pthread_mutex_destroy(&pl->mu);
    vjo_vocr_unstick();
    free(stuck_ws);
    stuck_ws = NULL;
}
static uint64_t thread_us(void *ud)
{
    struct timespec ts;
    (void)ud;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* ------------------------------------------------------------ the job */
static int busy_on, busy_calls;
static void busy(void *ud, int on)
{
    (void)ud;
    TEST_CHECK(on != busy_on); /* on, then off, never twice */
    busy_on = on;
    busy_calls++;
}

typedef struct {
    int rc;
    char text[VJO_OCR_TEXT_CAP];
    char raw[VJO_OCR_TEXT_CAP];
    VjoVocrStats st;
} Run;

typedef struct {
    unsigned helpers;
    VjoVocrCache *cache;
    uint32_t variant;
    size_t ws;              /* 0: what the job asks for */
    size_t cap;             /* 0: VJO_OCR_TEXT_CAP */
    int lm, cascade;        /* the tiny LM; the second model reading doubtful lines again */
    uint32_t cascade_us;    /* 0: 60 s (never skipped) */
    unsigned cascade_conf;  /* 0: 1001, every line with text is doubtful */
    VjoVocrResident *res;   /* NULL: the shared one */
    const char *model;      /* NULL: M1 */
    /* #528: keep the cache's last region (VjoVocrCache.last) between jobs; 0: emptied before each job, so these
     * tests read every region and test the line cache */
    int last;
    uint32_t settle_us;     /* the stable-text wait (VjoVocrPolicy.settle_us), at most 2 s; 0: none */
    uint32_t salt;          /* VjoVocrPolicy.salt (#528: 0, no last region) */
} Opt;

static PosixPlatform pp;
static VjoPlatform plat;
static VjoVocrResident res, res2, cres;
static VjoVocrLmResident lres;
static void *heap_alloc(void *ud, size_t bytes)
{
    void *p = NULL;
    (void)ud;
    return posix_memalign(&p, 16, bytes) ? NULL : p;
}
static int res_release(void *ud) { free(((VjoVocrResident *)ud)->data); return 0; }
static int lres_release(void *ud) { free(((VjoVocrLmResident *)ud)->data); return 0; }

static size_t job_need(const Opt *o, unsigned helpers)
{
    const char *m = o->model ? o->model : M1;
    return vjo_vocr_job_bytes(m, W, H) + (o->lm || o->cascade ? vjo_vocr_lm_job_bytes(m) : 0) +
           (o->cascade ? vjo_vocr_cascade_job_bytes(M2) : 0) + vjo_vocr_helpers_bytes(m, helpers);
}

static void run(Run *r, Pool *pl, const Opt *o)
{
    VjoVocrPolicy pol;
    VjoVocrPool pool = {o->helpers, pool_run, pool_join, thread_us, pl};
    const char *m = o->model ? o->model : M1;
    size_t need = o->ws ? o->ws : job_need(o, o->helpers);
    void *ws = NULL;
    TEST_ASSERT(need > 0 && posix_memalign(&ws, 64, need) == 0);
    memset(&pol, 0, sizeof pol);
    pol.pool = pl ? &pool : NULL;
    pol.cache = o->cache;
    pol.cache_variant = o->variant;
    pol.settle_us = o->settle_us;
    pol.settle_max_us = o->settle_us ? 2000000u : 0u;
    pol.salt = o->salt;
    pol.busy = busy;
    pol.raw = r->raw;
    pol.raw_cap = sizeof(r->raw);
    if (o->lm) {
        pol.lm = vjo_vocr_lm_tier(1);
        pol.lm_res = &lres;
    }
    if (o->cascade) {
        pol.cascade = vjo_vocr_model(M2);
        pol.cascade_res = &cres;
        pol.cascade_us = o->cascade_us ? o->cascade_us : 60000000u;
        pol.cascade_conf = o->cascade_conf ? o->cascade_conf : 1001u;
    }
    busy_on = 0;
    strips_read = 0;
    tear_seen = 0;
    if (o->cache && !o->last) o->cache->last.salt = 0;
    memset(r, 0, sizeof(*r));
    r->rc = vjo_vocr_ocr_policy(&plat, model_dir, m, &im, o->res ? o->res : &res, ws, need, capture,
                                vjo_vocr_capture_bytes(W, H), &pol, r->text, o->cap ? o->cap : sizeof(r->text), &r->st);
    TEST_CHECK(!busy_on); /* switched off on every path */
    /* every line placed: the cascade's estimate counts nothing more */
    if (r->rc == VJO_OK) TEST_CHECK_(vjo_vocr_test_len_left() == 0, "length left %u", (unsigned)vjo_vocr_test_len_left());
    if (!r->st.stuck || stuck_ws) {
        free(ws); /* a stuck job after the first is the latch's refusal: nothing ran in it */
    } else {
        stuck_ws = ws; /* a helper may still run in it: freed by pool_done after the join */
    }
}

static void setup(void)
{
    setup_files();
    posix_platform_init(&pp, &plat);
    if (!res.alloc) {
        res = (VjoVocrResident){heap_alloc, res_release, &res, {NULL}, NULL, 0, 0, 0};
        res2 = (VjoVocrResident){heap_alloc, res_release, &res2, {NULL}, NULL, 0, 0, 0};
        cres = (VjoVocrResident){heap_alloc, res_release, &cres, {NULL}, NULL, 0, 0, 0};
        lres = (VjoVocrLmResident){heap_alloc, lres_release, &lres, {NULL}, NULL, 0, 0, 0};
    }
    vjo_vocr_unstick();
    cancel_now = 0;
    cancel_after = tear_at = 0;
    draw(LINES, 0);
    cache_replay_install();
}

/* the same job: rc, text, greedy text, frames, every line's result in line order */
static void same_job(const Run *a, const Run *b, const char *what)
{
    TEST_CHECK_(a->rc == b->rc, "%s: rc %d vs %d", what, a->rc, b->rc);
    TEST_CHECK_(!strcmp(a->text, b->text), "%s: \"%s\" vs \"%s\"", what, a->text, b->text);
    TEST_CHECK_(!strcmp(a->raw, b->raw), "%s: greedy \"%s\" vs \"%s\"", what, a->raw, b->raw);
    TEST_CHECK_(a->st.frames == b->st.frames && a->st.lines == b->st.lines && a->st.recognized == b->st.recognized,
                "%s: frames %u/%u lines %u/%u recognized %u/%u", what, a->st.frames, b->st.frames, a->st.lines,
                b->st.lines, a->st.recognized, b->st.recognized);
    for (unsigned i = 0; i < a->st.recognized && i < b->st.recognized; i++)
        TEST_CHECK_(a->st.line[i].width == b->st.line[i].width && a->st.line[i].height == b->st.line[i].height &&
                        a->st.line[i].frames == b->st.line[i].frames && a->st.line[i].flags == b->st.line[i].flags,
                    "%s: line %u: %ux%u %u frames flags %#x vs %ux%u %u frames flags %#x", what, i, a->st.line[i].width,
                    a->st.line[i].height, a->st.line[i].frames, a->st.line[i].flags, b->st.line[i].width,
                    b->st.line[i].height, b->st.line[i].frames, b->st.line[i].flags);
    TEST_CHECK_(a->st.lm_lines == b->st.lm_lines && a->st.lm_changed == b->st.lm_changed && a->st.lm_rc == b->st.lm_rc,
                "%s: LM lines %u/%u changed %u/%u rc %d/%d", what, a->st.lm_lines, b->st.lm_lines, a->st.lm_changed,
                b->st.lm_changed, a->st.lm_rc, b->st.lm_rc);
    TEST_CHECK_(a->st.cascade_lines == b->st.cascade_lines && a->st.cascade_changed == b->st.cascade_changed &&
                    a->st.cascade_frames == b->st.cascade_frames && a->st.cascade_blanked == b->st.cascade_blanked &&
                    a->st.cascade_skipped == b->st.cascade_skipped,
                "%s: cascade lines %u/%u changed %u/%u frames %u/%u", what, a->st.cascade_lines, b->st.cascade_lines,
                a->st.cascade_changed, b->st.cascade_changed, a->st.cascade_frames, b->st.cascade_frames);
}

static void test_parallel_equals_serial(void)
{
    static Run serial, par;
    static const char *const kinds[3] = {"greedy", "LM", "LM + cascade"};
    setup();
    for (int kind = 0; kind < 3; kind++) {
        Opt o = {0};
        o.lm = kind >= 1;
        o.cascade = kind == 2;
        run(&serial, NULL, &o);
        TEST_ASSERT(serial.rc == VJO_OK);
        TEST_CHECK(serial.st.lines == LINES && serial.st.recognized == LINES && serial.st.frames > 0);
        TEST_CHECK(serial.st.helpers == 0 && serial.st.dispatched == 0 && serial.text[0]);
        if (o.lm) TEST_CHECK(serial.st.lm == vjo_vocr_lm_tier(1) && serial.st.lm_lines > 0 && !serial.st.lm_rc);
        if (o.cascade) TEST_CHECK(serial.st.cascade_lines > 0 && serial.st.cascade_frames > 0 && !serial.st.cascade_rc);
        TEST_MSG("%s serial: %u lines, %u frames, \"%s\" (greedy \"%s\"), LM changed %u, cascade read %u changed %u",
                 kinds[kind], serial.st.lines, serial.st.frames, serial.text, serial.raw, serial.st.lm_changed,
                 serial.st.cascade_lines, serial.st.cascade_changed);
        for (int mode = P_THREADS; mode <= P_SLOW; mode++) {
            if (mode == P_JOIN_FAILS) continue;
            for (unsigned helpers = 1; helpers <= VJO_VOCR_HELPERS_MAX; helpers++) {
                Pool pl;
                char what[64];
                unsigned on_helpers = 0;
                pool_init(&pl, mode);
                o.helpers = helpers;
                run(&par, &pl, &o);
                snprintf(what, sizeof what, "%s, pool %d, %u helper(s)", kinds[kind], mode, helpers);
                same_job(&serial, &par, what);
                TEST_CHECK(par.st.helpers == helpers && !par.st.stuck);
                TEST_CHECK(pl.runs == pl.joins && pl.now_live == 0);
                if (mode == P_NO_START) TEST_CHECK(par.st.dispatched == 0 && par.st.run_failures > 0);
                else TEST_CHECK(par.st.dispatched > 0 && par.st.dispatched == pl.runs);
                if (mode == P_THREADS || mode == P_SLOW) TEST_CHECK(pl.max_live >= 1 && pl.max_live <= helpers);
                for (unsigned i = 0; i < par.st.recognized; i++) on_helpers += par.st.line[i].slot != 0;
                TEST_CHECK(on_helpers == par.st.dispatched + par.st.run_failures);
                /* the last line runs on the caller's thread: it would only wait otherwise */
                TEST_CHECK(par.st.recognized == LINES && par.st.line[LINES - 1].slot == 0);
                pool_done(&pl);
            }
        }
        o.helpers = 0;
    }
}

/* What a cache holds after a job: only texts the serial job shows anywhere, so a full-capacity job
 * that takes every line it can from it gives the serial job's text and greedy text. */
static void cache_gives_the_serial_text(const VjoVocrCache *after, const Opt *o, const Run *ref, const char *what)
{
    static VjoVocrCache c;
    static Run f;
    Opt full = *o;
    memcpy(&c, after, sizeof c);
    full.cap = 0;
    full.helpers = 0;
    full.cache = &c;
    run(&f, NULL, &full);
    TEST_CHECK_(f.rc == VJO_OK && !strcmp(f.text, ref->text) && !strcmp(f.raw, ref->raw),
                "%s: the cache then gives \"%s\" (greedy \"%s\"), the serial job \"%s\" (\"%s\")", what, f.text,
                f.raw, ref->text, ref->raw);
}

/* For every text capacity, the parallel job (with no cache, an empty one and a warm one) gives the serial
 * job's rc, text and per-line results, and leaves in the cache only what the serial job shows. */
static void test_capacity_like_serial(void)
{
    static Run ref, s, p;
    static VjoVocrCache warm, cache;
    unsigned too_large = 0;
    setup();
    for (int kind = 0; kind < 3; kind++) {
        Opt o = {0};
        o.lm = kind >= 1;
        o.cascade = kind == 2;
        run(&ref, NULL, &o);
        TEST_ASSERT(ref.rc == VJO_OK);
        memset(&warm, 0, sizeof warm);
        o.cache = &warm;
        run(&p, NULL, &o); /* every line in the cache */
        TEST_ASSERT(p.rc == VJO_OK && !strcmp(p.text, ref.text));
        size_t n = strlen(ref.text);
        for (size_t cap = 1; cap <= n + 2; cap++)
            for (int mode = P_THREADS; mode <= P_SLOW; mode++) {
                if (mode == P_NO_START || mode == P_JOIN_FAILS) continue;
                o.cap = cap;
                o.helpers = 0;
                o.cache = NULL;
                run(&s, NULL, &o);
                too_large += s.rc == VJO_E_TOO_LARGE;
                for (int c = 0; c < 3; c++) { /* no cache, an empty one, a warm one */
                    Pool pl;
                    char what[96];
                    snprintf(what, sizeof what, "kind %d cap %zu pool %d cache %d", kind, cap, mode, c);
                    if (c == 1) memset(&cache, 0, sizeof cache);
                    if (c == 2) memcpy(&cache, &warm, sizeof cache);
                    o.cache = c ? &cache : NULL;
                    o.helpers = 2;
                    pool_init(&pl, mode);
                    run(&p, &pl, &o);
                    TEST_CHECK_(s.rc == p.rc && !strcmp(s.text, p.text), "%s: serial %d \"%s\", parallel %d \"%s\"", what,
                                s.rc, s.text, p.rc, p.text);
                    if (c != 2)
                        TEST_CHECK_(s.st.recognized == p.st.recognized && s.st.frames == p.st.frames,
                                    "%s: recognized %u/%u frames %u/%u", what, s.st.recognized, p.st.recognized,
                                    s.st.frames, p.st.frames);
                    /* a text that holds nothing: nothing is recognized, not even on a helper */
                    if (cap == 1) TEST_CHECK_(p.st.dispatched == 0 && p.st.recognized == 0, "%s: dispatched %u", what,
                                              p.st.dispatched);
                    TEST_CHECK(pl.runs == pl.joins && !p.st.stuck);
                    pool_done(&pl);
                    if (c) cache_gives_the_serial_text(&cache, &o, &ref, what);
                }
            }
    }
    TEST_CHECK(too_large > 0);
}

/* A cached line is taken only where its longest reading (greedy, the LM's, the cascade's) fits, as
 * every reading the serial job tries there must: where only its shown text would fit, the region is
 * read again without the cache, as the serial job reads it. */
static void test_a_hit_whose_longest_reading_does_not_fit(void)
{
    static VjoVocrCache cache;
    static Run ref, p;
    VjoVocrCacheLine *last = NULL;
    Opt o = {0};
    size_t n;
    setup();
    o.lm = o.cascade = 1;
    run(&ref, NULL, &o);
    TEST_ASSERT(ref.rc == VJO_OK);
    n = strlen(ref.text);
    memset(&cache, 0, sizeof cache);
    o.cache = &cache;
    run(&p, NULL, &o);
    TEST_ASSERT(p.rc == VJO_OK && !strcmp(p.text, ref.text));
    for (unsigned i = 0; i < VJO_VOCR_CACHE_LINES; i++) /* the last line's entry: the most recently added */
        if (cache.line[i].key && (!last || cache.line[i].used > last->used)) last = &cache.line[i];
    TEST_ASSERT(last != NULL && last->len > 0 && last->need >= last->len);
    TEST_CHECK(n >= last->len && !strcmp(ref.text + n - last->len, last->text));
    /* a reading longer than the room the last line has, and another text, so a hit shows */
    last->need = (uint16_t)(last->len + 40u);
    memset(last->text, 'z', last->len);
    o.cap = n + 1; /* every line fits as the serial job reads it */
    o.helpers = 2;
    {
        Pool pl;
        pool_init(&pl, P_THREADS);
        run(&p, &pl, &o);
        pool_done(&pl);
    }
    TEST_CHECK_(p.rc == VJO_OK && !strcmp(p.text, ref.text) && p.st.cached == 0, "rc %d \"%s\" (cached %u)", p.rc,
                p.text, p.st.cached);
}

static void test_cache(void)
{
    static VjoVocrCache cache;
    static Run a, b, c, ref;
    Pool pl;
    Opt o = {0};
    setup();
    memset(&cache, 0, sizeof cache);
    pool_init(&pl, P_THREADS);
    o.helpers = 2;
    o.cache = &cache;
    run(&a, &pl, &o);
    TEST_ASSERT(a.rc == VJO_OK);
    TEST_CHECK(a.st.cached == 0 && busy_calls >= 2);
    /* The same screen again: every line from the cache, no recognizer, no busy. */
    busy_calls = 0;
    run(&b, &pl, &o);
    TEST_CHECK(b.rc == VJO_OK && !strcmp(b.text, a.text) && !strcmp(b.raw, a.raw));
    TEST_CHECK(b.st.cached == LINES && b.st.frames == 0 && b.st.dispatched == 0 && busy_calls == 0);
    TEST_CHECK(!b.st.model_loaded && b.st.model_cached);
    for (unsigned i = 0; i < b.st.recognized; i++) TEST_CHECK(b.st.line[i].cached && b.st.line[i].frames);
    /* One line changed: only it is recognized, and the text is the serial job's. */
    draw(2, 3);
    {
        Opt s = {0};
        run(&ref, NULL, &s);
    }
    run(&c, &pl, &o);
    TEST_CHECK(c.rc == VJO_OK && !strcmp(c.text, ref.text));
    TEST_CHECK(c.st.cached == LINES - 1 && c.st.frames > 0 && c.st.frames < a.st.frames);
    TEST_CHECK(!c.st.line[2].cached && c.st.line[1].cached && c.st.line[3].cached);
    /* Another cache variant (another decoder or cascade setting): no line from the cache;
     * the same variant again: every line (the 8 entries hold this job's 5). */
    o.variant = 7;
    run(&b, &pl, &o);
    TEST_CHECK(b.rc == VJO_OK && b.st.cached == 0 && !strcmp(b.text, ref.text));
    run(&b, &pl, &o);
    TEST_CHECK(b.rc == VJO_OK && b.st.cached == LINES && !strcmp(b.text, ref.text));
    /* A line's VjoVocrLineStat.flags come back with its cached text. */
    for (unsigned i = 0; i < VJO_VOCR_CACHE_LINES; i++)
        if (cache.line[i].key) cache.line[i].flags = 0x4123;
    run(&b, &pl, &o);
    TEST_CHECK(b.rc == VJO_OK && b.st.cached == LINES);
    for (unsigned i = 0; i < b.st.recognized; i++) TEST_CHECK(b.st.line[i].flags == 0x4123);
    /* Another model's lines never match: with its own resident weights the other model
     * does not empty the cache, and the first model, still resident, finds only the
     * other's entries (pixels and variant equal). */
    {
        Opt o2 = o;
        o2.model = M2;
        o2.res = &res2;
        run(&b, &pl, &o2); /* M2's weights read: the cache is emptied, then holds M2's lines */
        TEST_CHECK(b.rc == VJO_OK && b.st.model_loaded && b.st.cached == 0);
        run(&b, &pl, &o); /* M1's weights still resident (no reload): none of M2's entries is M1's */
        TEST_CHECK(b.rc == VJO_OK && !b.st.model_loaded && b.st.model_cached && b.st.cached == 0 &&
                   !strcmp(b.text, ref.text));
        vjo_vocr_resident_drop(&res2);
    }
    /* Weights read from the file again: the cache is emptied. */
    vjo_vocr_resident_drop(&res);
    run(&b, &pl, &o);
    TEST_CHECK(b.rc == VJO_OK && b.st.model_loaded && b.st.cached == 0 && !strcmp(b.text, ref.text));
    pool_done(&pl);
}

/* The LM's and the cascade's lines: cached with their shown and greedy text and their flags,
 * a hit gives what the job read; a line the cascade's time ceiling skipped is not kept. */
static void test_cache_with_lm_and_cascade(void)
{
    static VjoVocrCache cache;
    static Run ref, a, b;
    Pool pl;
    Opt o = {0};
    setup();
    o.lm = o.cascade = 1;
    run(&ref, NULL, &o);
    TEST_ASSERT(ref.rc == VJO_OK && ref.st.cascade_lines > 0);
    memset(&cache, 0, sizeof cache);
    pool_init(&pl, P_THREADS);
    o.helpers = 2;
    o.cache = &cache;
    run(&a, &pl, &o);
    same_job(&ref, &a, "LM + cascade, cache cold");
    run(&b, &pl, &o);
    TEST_CHECK(b.rc == VJO_OK && b.st.cached == LINES && b.st.frames == 0 && b.st.cascade_lines == 0);
    TEST_CHECK(!strcmp(b.text, ref.text) && !strcmp(b.raw, ref.raw));
    for (unsigned i = 0; i < b.st.recognized; i++) TEST_CHECK(b.st.line[i].flags == ref.st.line[i].flags);
    /* The ceiling at 1 us: every doubtful line is left as its first reading, which another
     * job with more time would not show: nothing is kept. */
    memset(&cache, 0, sizeof cache);
    o.cascade_us = 1;
    run(&a, &pl, &o);
    TEST_CHECK(a.rc == VJO_OK && a.st.cascade_skipped > 0);
    for (unsigned i = 0; i < VJO_VOCR_CACHE_LINES; i++) TEST_CHECK(cache.line[i].key == 0);
    /* With a confidence under which no line falls, no line is doubtful: all kept. */
    o.cascade_us = 0;
    o.cascade_conf = 1;
    o.variant = 1;
    run(&a, &pl, &o);
    TEST_CHECK(a.rc == VJO_OK && a.st.cascade_lines == 0 && a.st.cascade_skipped == 0);
    run(&b, &pl, &o);
    TEST_CHECK(b.rc == VJO_OK && b.st.cached == LINES && !strcmp(b.text, a.text));
    pool_done(&pl);
}

/* #528, the content a reused job gives: what a fresh read of the same screen gives (the real runtime's lines, with the
 * LM and the cascade), byte for byte: text, greedy text, lines, dark ones, and every line's crop, frames, flags, kana and
 * place. Only the work differs (none found or recognized, no frames: VjoVocrStats.reused). */
static void same_reading(const Run *fresh, const Run *r, const char *what)
{
    TEST_CHECK_(fresh->rc == VJO_OK && r->rc == VJO_OK, "%s: rc %d, %d", what, fresh->rc, r->rc);
    TEST_CHECK_(!strcmp(fresh->text, r->text) && !strcmp(fresh->raw, r->raw), "%s: \"%s\" (\"%s\") vs \"%s\" (\"%s\")",
                what, fresh->text, fresh->raw, r->text, r->raw);
    TEST_CHECK_(fresh->st.lines == r->st.lines && fresh->st.dark == r->st.dark && fresh->st.truncated == r->st.truncated &&
                    fresh->st.recognized == r->st.recognized,
                "%s: lines %u/%u recognized %u/%u", what, fresh->st.lines, r->st.lines, fresh->st.recognized,
                r->st.recognized);
    for (unsigned i = 0; i < fresh->st.recognized && i < r->st.recognized; i++) {
        const VjoVocrLineStat *a = &fresh->st.line[i], *b = &r->st.line[i];
        TEST_CHECK_(a->width == b->width && a->height == b->height && a->frames == b->frames && a->flags == b->flags &&
                        a->kana == b->kana && a->x == b->x && a->y == b->y,
                    "%s: line %u: %ux%u at %u,%u, %u frames, flags %#x, kana %u vs %ux%u at %u,%u, %u frames, flags %#x, "
                    "kana %u", what, i, a->width, a->height, a->x, a->y, a->frames, a->flags, a->kana, b->width, b->height,
                    b->x, b->y, b->frames, b->flags, b->kana);
    }
}

/* #528 (core/vocr_ocr.h VjoVocrCache.last): a job whose region reads the same at its first pass as the last job's
 * did, with the same model and policy, gets that job's text and lines without the finder or the recognizer: the
 * content of a fresh read of that screen. A row changed, another policy, another salt read again; a job with a
 * doubtful line skipped for time keeps nothing; the weights read again empty it; a reused job still waits for its
 * rows to hold still (the stable-text wait), and a change then is a tear: its next try reads the new screen. */
static void test_an_identical_region_reuses_the_last_text(void)
{
    static VjoVocrCache cache;
    static Run a, b, fresh, ref;
    const unsigned pass = (H + 63u) / 64u; /* strips of a pass over the region (vjo_vocr_strip_rows: 64 rows) */
    Pool pl;
    Opt o = {0}, s;
    setup();
    memset(&cache, 0, sizeof cache);
    pool_init(&pl, P_THREADS);
    o.helpers = 2;
    o.cache = &cache;
    o.last = 1;
    o.lm = o.cascade = 1;
    o.cascade_conf = 1; /* no doubtful line: none skipped for time, so the job keeps its region */
    o.variant = 3;
    o.salt = 7;         /* the caller's key for this reading (the worker's: its request and choices) */
    run(&a, &pl, &o);
    TEST_ASSERT(a.rc == VJO_OK && !a.st.reused && a.st.recognized == LINES && cache.last.salt == 7);
    TEST_CHECK(a.raw[0] && a.st.lm_lines == LINES);
    /* the same screen: its first pass only, no finder, no recognizer (busy never called), the same reading */
    busy_calls = 0;
    run(&b, &pl, &o);
    TEST_CHECK(b.rc == VJO_OK && b.st.reused && busy_calls == 0 && !b.st.frames && !b.st.find_us && !b.st.lm_lines);
    TEST_CHECK_(strips_read == pass && b.st.strips == pass, "%u strips (a pass is %u)", strips_read, pass);
    same_reading(&a, &b, "the same screen");
    /* a fresh read of it (the cache emptied: every line recognized again) gives the same */
    memset(&cache, 0, sizeof cache);
    run(&fresh, &pl, &o);
    TEST_CHECK(fresh.rc == VJO_OK && !fresh.st.reused && !fresh.st.cached && fresh.st.frames == a.st.frames);
    same_reading(&fresh, &b, "a fresh read");
    run(&b, &pl, &o);
    same_reading(&fresh, &b, "after a fresh read");
    TEST_CHECK(b.st.reused);
    /* another salt (another reading the caller asks for): read again, then that reading is kept; none: never */
    s = o;
    s.salt = 1;
    run(&b, &pl, &s);
    TEST_CHECK(b.rc == VJO_OK && !b.st.reused && !strcmp(b.text, fresh.text));
    run(&b, &pl, &s);
    TEST_CHECK(b.st.reused);
    s.salt = 0;
    run(&b, &pl, &s);
    TEST_CHECK(b.rc == VJO_OK && !b.st.reused && cache.last.salt == 1);
    /* one line changed: read again, the serial job's text */
    draw(2, 3);
    s = o;
    s.cache = NULL;
    s.last = 0;
    s.helpers = 0;
    run(&ref, NULL, &s);
    run(&b, &pl, &o);
    TEST_CHECK(b.rc == VJO_OK && !b.st.reused && !strcmp(b.text, ref.text));
    run(&b, &pl, &o);
    TEST_CHECK(b.st.reused);
    same_reading(&ref, &b, "the changed screen");
    /* a doubtful line skipped for time (the cascade's ceiling): its region is not kept */
    s = o;
    s.cascade_conf = 0; /* every line with text doubtful */
    s.cascade_us = 1;   /* and no time to read one again */
    s.variant = 9;      /* (the worker's cache variant holds the cascade's ceiling: no line from the cache) */
    s.salt = 9;
    run(&b, &pl, &s);
    TEST_CHECK(b.rc == VJO_OK && b.st.cascade_skipped > 0 && !cache.last.salt);
    run(&b, &pl, &s);
    TEST_CHECK(b.rc == VJO_OK && !b.st.reused);
    /* the weights read from the file again: the entry goes with the line cache */
    run(&b, &pl, &o);
    TEST_CHECK(b.rc == VJO_OK && !b.st.reused && cache.last.salt == 7);
    vjo_vocr_resident_drop(&res);
    run(&b, &pl, &o);
    TEST_CHECK(b.rc == VJO_OK && b.st.model_loaded && !b.st.reused);
    /* the stable-text wait: a reused job waits for the rows as a read's lines do; the screen changing under it is a
     * tear, and its next try reads (and recognizes) the new screen */
    draw(LINES, 0);
    o.settle_us = 1000;
    run(&a, &pl, &o);
    TEST_ASSERT(a.rc == VJO_OK && !a.st.reused && a.st.settled);
    run(&b, &pl, &o);
    TEST_CHECK(b.rc == VJO_OK && b.st.reused && b.st.settled && b.st.settle_passes > 0);
    same_reading(&a, &b, "the same screen, after the wait");
    tear_at = pass + 2u; /* in the reused job's first pass of the wait: line 2's rows change */
    run(&b, &pl, &o);
    tear_at = 0;
    s = o;
    s.cache = NULL;
    s.last = 0;
    s.helpers = 0;
    run(&ref, NULL, &s);
    TEST_CHECK(b.rc == VJO_OK && !b.st.reused && b.st.tries == 2 && b.st.tears > 0);
    TEST_CHECK_(!strcmp(b.text, ref.text) && strcmp(b.text, a.text), "\"%s\" (the new screen: \"%s\")", b.text, ref.text);
    pool_done(&pl);
}

/* #418: the line cache keeps a line's greedy text after its shown text, only where it is another (the LM or the
 * cascade changed the line): every test of this file replays its jobs' cache operations through the old layout,
 * the greedy text in a buffer of its own, and checks after each that both hold the same (cache_replay.h, installed
 * by setup). Here a sequence of every kind of job and its counts, which main's cache gives too (the same sequence's
 * cache.hits and cache.misses there): cold, again, one line changed, the LM and the cascade (changed lines, kept
 * with their greedy texts), another variant, the weights read again (emptied). */
static void test_the_cache_replayed_through_the_old_layout(void)
{
    static VjoVocrCache cache;
    static Run a;
    static const unsigned want[][2] = {{0, 5}, {5, 5}, {9, 6}, {9, 11}, {14, 11}, {14, 16}, {19, 16}, {0, 5}};
    unsigned seq[8][2], n = 0, kept_greedy = 0;
    Pool pl;
    Opt o = {0};
    setup();
    memset(&cache, 0, sizeof cache);
    pool_init(&pl, P_THREADS);
    o.helpers = 2;
    o.cache = &cache;
    run(&a, &pl, &o); /* cold: every line looked up and missed, then kept */
    seq[n][0] = cache.hits; seq[n++][1] = cache.misses;
    run(&a, &pl, &o); /* the same screen: every line a hit */
    seq[n][0] = cache.hits; seq[n++][1] = cache.misses;
    draw(2, 3); /* one line changed: it alone misses */
    run(&a, &pl, &o);
    seq[n][0] = cache.hits; seq[n++][1] = cache.misses;
    o.lm = o.cascade = 1; /* the LM and the cascade: another variant, every line misses, then hits */
    o.variant = 5;
    run(&a, &pl, &o);
    seq[n][0] = cache.hits; seq[n++][1] = cache.misses;
    run(&a, &pl, &o);
    seq[n][0] = cache.hits; seq[n++][1] = cache.misses;
    for (unsigned i = 0; i < VJO_VOCR_CACHE_LINES; i++) kept_greedy += cache.line[i].key && cache.line[i].greedy_at;
    o.variant = 6; /* yet another: misses, its lines replace the least recently used */
    run(&a, &pl, &o);
    seq[n][0] = cache.hits; seq[n++][1] = cache.misses;
    run(&a, &pl, &o);
    seq[n][0] = cache.hits; seq[n++][1] = cache.misses;
    vjo_vocr_resident_drop(&res); /* the weights read again: the cache emptied, every line misses */
    o.lm = o.cascade = 0;
    o.variant = 0;
    run(&a, &pl, &o);
    seq[n][0] = cache.hits; seq[n++][1] = cache.misses;
    pool_done(&pl);
    for (unsigned i = 0; i < n; i++)
        TEST_CHECK_(seq[i][0] == want[i][0] && seq[i][1] == want[i][1], "job %u: %u hits, %u misses (main's: %u, %u)",
                    i + 1u, seq[i][0], seq[i][1], want[i][0], want[i][1]);
    TEST_CHECK_(!replay.mismatches && !replay.left_out && replay.lookups == 8u * LINES && replay.hits == 19u &&
                    replay.puts == 21u && kept_greedy > 0,
                "replayed: %u lookups, %u hits, %u entries offered, %u synced, %u left out, %u differed; %u entries "
                "with another greedy text",
                replay.lookups, replay.hits, replay.puts, replay.synced, replay.left_out, replay.mismatches,
                kept_greedy);
}

/* #418: the cache's rule for a line's texts, through its own put and lookup and the old layout's: a shown text under
 * VJO_VOCR_CACHE_TEXT is kept with its greedy text when that is the same text (no copy) and when another fits after it
 * (the old layout kept both while each fit its own buffer); another that does not fit is left out, the one
 * difference; a shown text of VJO_VOCR_CACHE_TEXT bytes or more never was kept. Hits give the old layout's texts. */
static void test_the_cache_keeps_a_greedy_text_after_the_shown_one(void)
{
    static VjoVocrCache c;
    static OldCache old;
    static char t[VJO_VOCR_CACHE_TEXT + 8], g[VJO_VOCR_CACHE_TEXT + 8];
    static const struct { unsigned len, glen; int other, kept, old_kept; } cases[] = {
        {0, 0, 0, 1, 1}, {367, 367, 0, 1, 1}, {368, 368, 0, 0, 0}, {120, 90, 1, 1, 1}, {200, 166, 1, 1, 1},
        {200, 167, 1, 0, 1}, {366, 0, 1, 1, 1}, {367, 0, 1, 0, 1}, {0, 366, 1, 1, 1}, {0, 367, 1, 0, 1},
        {300, 300, 1, 0, 1}, {367, 368, 1, 0, 0},
    };
    const VjoVocrModel *m = vjo_vocr_model(M1);
    TEST_ASSERT(m != NULL);
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const unsigned len = cases[i].len, glen = cases[i].other ? cases[i].glen : cases[i].len;
        const uint64_t key = 0x1000u + i;
        memset(&c, 0, sizeof c);
        memset(&old, 0, sizeof old);
        memset(t, 'a' + (int)(i % 20u), len);
        t[len] = '\0';
        memset(g, 'A' + (int)(i % 20u), glen);
        g[glen] = '\0';
        const char *greedy = cases[i].other ? g : t;
        const int kept = vjo_vocr_test_cache_put(&c, m, key, t, len, greedy, glen, 7u, 0x8123u, len + 4u);
        old_cache_put(&old, m, key, t, len, greedy, glen, 7u, 0x8123u, len + 4u);
        const VjoVocrCacheLine *e = vjo_vocr_test_cache_find(&c, m, key);
        const OldCacheLine *o = old_cache_find(&old, m, key);
        TEST_CHECK_(kept == cases[i].kept && !!e == cases[i].kept && !!o == cases[i].old_kept,
                    "texts of %u and %u B (%s): kept %d (%d), the old layout %d", len, glen,
                    cases[i].other ? "another greedy" : "the same", kept, cases[i].kept, !!o);
        if (e && o) {
            TEST_CHECK(e->len == o->len && e->greedy_len == o->greedy_len && e->flags == o->flags &&
                       e->frames == o->frames && e->need == o->need && !strcmp(e->text, o->text) &&
                       !strcmp(e->text + e->greedy_at, o->greedy));
            TEST_CHECK(e->greedy_at == (cases[i].other ? len + 1u : 0u));
        }
    }
}

/* Cancelled while helpers run (strips 11-17 land after the finder, with lines on helpers):
 * every helper joined before the job returns, and nothing from it in the cache. */
static void test_cancel_with_helpers_running(void)
{
    static Run r;
    static VjoVocrCache cache;
    unsigned with_helpers = 0, after_lines = 0;
    for (unsigned ca = 11; ca <= 17; ca++)
        for (int rep = 0; rep < 4; rep++) {
            Pool pl;
            Opt o = {0};
            setup();
            memset(&cache, 0, sizeof cache);
            pool_init(&pl, rep & 1 ? P_SLOW : P_THREADS);
            o.helpers = 2;
            o.cache = &cache;
            o.lm = rep >= 2;
            cancel_after = ca;
            run(&r, &pl, &o);
            TEST_CHECK(r.rc == VJO_E_CANCELLED || r.rc == VJO_OK);
            TEST_CHECK_(pl.runs == pl.joins && pl.now_live == 0 && !r.st.stuck, "cancel at strip %u: rc %d runs %u "
                        "joins %u", ca, r.rc, pl.runs, pl.joins);
            if (r.rc == VJO_E_CANCELLED) {
                with_helpers += pl.runs > 0;
                after_lines += r.st.recognized > 0;
                TEST_CHECK(!r.text[0]);
                for (unsigned k = 0; k < VJO_VOCR_CACHE_LINES; k++) TEST_CHECK(cache.line[k].key == 0);
            }
            pool_done(&pl);
        }
    TEST_CHECK(with_helpers > 0 && after_lines > 0);
    TEST_MSG("cancelled jobs that had started helpers %u, that had placed lines %u", with_helpers, after_lines);
}

/* A join that fails: the job fails with VJO_E_OCR_UNAVAILABLE and says its workspace may
 * still be in use; a try that also saw the screen change is never retried (the helper may
 * still run in the job's state); every later job is refused until the caller has seen the
 * helper end (vjo_vocr_unstick). */
static void test_failed_join(void)
{
    static Run r, s;
    unsigned stuck = 0, torn = 0;
    for (unsigned t = 0; t <= 20; t++) {
        Pool pl;
        Opt o = {0};
        if (t && t < 10) continue;
        setup();
        pool_init(&pl, P_JOIN_FAILS);
        o.helpers = 2;
        o.lm = t & 1;
        tear_at = t;
        run(&r, &pl, &o);
        tear_at = 0;
        if (r.st.stuck) {
            stuck++;
            torn += r.st.tears > 0;
            /* nothing after the failed join: no strip read (no other try), no helper started */
            TEST_CHECK_(r.rc == VJO_E_OCR_UNAVAILABLE && !r.text[0] && strips_read == pl.strips_at_fail &&
                            pl.runs_after_fail == 0,
                        "tear at strip %u: rc %d, tries %u, strips %u (%u at the failed join)", t, r.rc, r.st.tries,
                        strips_read, pl.strips_at_fail);
            /* the next job, even a serial one, is refused: nothing runs beside the stuck helper */
            {
                Opt so = {0};
                run(&s, NULL, &so);
                TEST_CHECK(s.rc == VJO_E_OCR_UNAVAILABLE && s.st.stuck && !s.st.lines);
            }
        }
        pool_done(&pl); /* joins the threads, then vjo_vocr_unstick */
        {
            Opt so = {0};
            run(&s, NULL, &so);
            TEST_CHECK(s.rc == VJO_OK && !s.st.stuck);
        }
    }
    TEST_CHECK(stuck > 0 && torn > 0);
    /* cancelled while a helper whose join fails runs: VJO_E_OCR_UNAVAILABLE, not the cancellation */
    stuck = 0;
    for (unsigned ca = 11; ca <= 17; ca++) {
        Pool pl;
        Opt o = {0};
        setup();
        pool_init(&pl, P_JOIN_FAILS);
        o.helpers = 2;
        cancel_after = ca;
        run(&r, &pl, &o);
        if (r.st.stuck) {
            stuck++;
            TEST_CHECK_(r.rc == VJO_E_OCR_UNAVAILABLE && !r.text[0], "cancel at strip %u: rc %d", ca, r.rc);
        }
        pool_done(&pl);
    }
    TEST_CHECK(stuck > 0);
}

/* The game's pool: a join that times out leaves the slot taken. The job stops there
 * (no try again, no line started after it), and the helper, which still runs, reads
 * nothing of the job's image, which lives on a stack frame that has returned. */
static VjoVocrPolicy game_pol;
static __attribute__((noinline)) int game_job(Pool *pl, Run *r, void *ws, size_t need)
{
    VjoGreySource local = im; /* on this frame, like the worker's */
    VjoVocrPool pool = {2, pool_run, pool_join, thread_us, pl};
    memset(&game_pol, 0, sizeof game_pol);
    game_pol.pool = &pool;
    strips_read = 0;
    tear_seen = 0;
    r->rc = vjo_vocr_ocr_policy(&plat, model_dir, M1, &local, &res, ws, need, capture, vjo_vocr_capture_bytes(W, H),
                                &game_pol, r->text, sizeof(r->text), &r->st);
    return r->rc;
}
static __attribute__((noinline)) void scribble(void)
{
    volatile unsigned char junk[4096];
    for (unsigned i = 0; i < sizeof junk; i++) junk[i] = 0xA5;
}
static void test_the_game_pool_after_a_timed_out_join(void)
{
    static Run r;
    unsigned stuck = 0;
    for (unsigned t = 10; t <= 22; t++) {
        Pool pl;
        Opt o = {0};
        size_t need;
        void *ws = NULL;
        setup();
        o.helpers = 2;
        need = job_need(&o, 2);
        pool_init(&pl, P_GAME);
        tear_at = t;
        TEST_ASSERT(posix_memalign(&ws, 64, need) == 0);
        game_job(&pl, &r, ws, need);
        tear_at = 0;
        scribble(); /* the job's frame is gone */
        TEST_CHECK(pl.runs_after_fail == 0);
        TEST_CHECK_(!r.st.stuck || (r.rc == VJO_E_OCR_UNAVAILABLE && strips_read == pl.strips_at_fail),
                    "tear at %u: rc %d tries %u stuck %u, strips %u (%u at the failed join)", t, r.rc, r.st.tries,
                    r.st.stuck, strips_read, pl.strips_at_fail);
        stuck += r.st.stuck;
        pool_done(&pl);
        free(ws);
    }
    TEST_CHECK(stuck > 0);
}

/* A helper that starts its line after its job has stopped (its join failed, the job returned) asks
 * the job nothing: the abort flag ends it at its first tile, before the image's cancel test, which
 * belongs to a job that is gone (re-audit S1; the first audit's M18). */
static void test_a_late_helper_asks_the_job_nothing(void)
{
    static Run r;
    Pool pl;
    Opt o = {0};
    setup();
    pool_init(&pl, P_LATE);
    o.helpers = 2;
    late_cancel_calls = 0;
    __atomic_store_n(&late_release, 0, __ATOMIC_RELEASE);
    run(&r, &pl, &o);
    TEST_CHECK(r.st.stuck && r.rc == VJO_E_OCR_UNAVAILABLE && pl.runs > 0);
    TEST_MSG("rc %d stuck %u, %u helper run(s)", r.rc, r.st.stuck, pl.runs);
    __atomic_store_n(&late_release, 1, __ATOMIC_RELEASE);
    pool_done(&pl); /* the helpers run their lines now; then they are joined and the workspace freed */
    TEST_CHECK(__atomic_load_n(&late_cancel_calls, __ATOMIC_RELAXED) == 0);
    TEST_MSG("%u call(s) of the image's cancel test by helpers that started after the job",
             __atomic_load_n(&late_cancel_calls, __ATOMIC_RELAXED));
    __atomic_store_n(&late_release, 0, __ATOMIC_RELEASE);
}

static void test_sizes_and_fewer_helpers(void)
{
    static Run r;
    Pool pl;
    Opt o = {0};
    size_t h0, h1, h2, one;
    const VjoVocrModel *m;
    setup();
    m = vjo_vocr_model(M1);
    h0 = vjo_vocr_job_bytes_helpers(M1, W, H, 0);
    h1 = vjo_vocr_job_bytes_helpers(M1, W, H, 1);
    h2 = vjo_vocr_job_bytes_helpers(M1, W, H, 2);
    one = vjo_vocr_helper_bytes(M1);
    TEST_CHECK(h0 == vjo_vocr_job_bytes(M1, W, H) && !vjo_vocr_helpers_bytes(M1, 0));
    /* the caller's own line slot once, then one helper's part each */
    TEST_CHECK(h2 - h1 == one && h1 - h0 == vjo_vocr_helpers_bytes(M1, 1) && h1 - h0 > one);
    TEST_CHECK(!vjo_vocr_job_bytes_helpers(M1, W, H, VJO_VOCR_HELPERS_MAX + 1) && !vjo_vocr_helpers_bytes(M1, 3));
    TEST_CHECK(!vjo_vocr_helper_bytes("other.vocr"));
    /* a helper: its arena, context, a strip-sized line, its text and its top-K */
    TEST_CHECK(one >= ((m->arena_bytes + 15u) & ~(size_t)15u) + ((sizeof(vocr_ctx) + 15u) & ~(size_t)15u) +
                          VJO_VOCR_STRIP_BYTES + VJO_OCR_TEXT_CAP + (size_t)VJO_VOCR_LM_FRAMES * VJO_VOCR_LM_TOPK * 6u);
    /* A workspace that holds one helper: the job uses one; one byte less, none. */
    pool_init(&pl, P_THREADS);
    o.helpers = 2;
    o.ws = h1;
    run(&r, &pl, &o);
    TEST_CHECK(r.rc == VJO_OK && r.st.helpers == 1 && r.st.workspace == h1);
    o.ws = h1 - 1;
    run(&r, &pl, &o);
    TEST_CHECK(r.rc == VJO_OK && r.st.helpers == 0 && r.st.dispatched == 0 && r.st.workspace == h0);
    /* The helpers' slots come after the LM's and the cascade's extras: the same, with them. */
    o.lm = o.cascade = 1;
    o.ws = job_need(&o, 1);
    run(&r, &pl, &o);
    TEST_CHECK(r.rc == VJO_OK && r.st.helpers == 1 && r.st.workspace == o.ws && r.st.cascade_lines > 0);
    o.ws = job_need(&o, 1) - 1;
    run(&r, &pl, &o);
    TEST_CHECK(r.rc == VJO_OK && r.st.helpers == 0 && r.st.cascade_lines > 0);
    /* A pinned arena smaller than the plan: a helper refuses its context (no overrun). */
    {
        uint32_t arena = m->arena_bytes;
        unsigned char sha[32];
        memcpy(sha, m->sha256, 32);
        test_pin_model(0, m->bytes, sha, arena - 64u);
        o = (Opt){0};
        o.helpers = 2;
        run(&r, &pl, &o);
        TEST_CHECK(r.rc == VJO_E_OCR_MODEL || (r.rc == VJO_OK && r.st.dispatched == 0));
        test_pin_model(0, m->bytes, sha, arena);
        vjo_vocr_resident_drop(&res);
        run(&r, &pl, &o);
        TEST_CHECK(r.rc == VJO_OK && r.st.dispatched > 0);
    }
    pool_done(&pl);
}

/* #528's speed pass. The LM's parse is kept for the resident bytes it was made from (core/vocr_ocr.c lm_setup): a job
 * that keeps the LM's bytes takes it, a job that reads them again (or after their block was released) parses again,
 * and every job reads the same as the first. The strip's row checksums (row_sums) are vjo_fnv1a's, every width and
 * row count, odd ones included. */
static void test_lm_parse_kept_and_row_sums(void)
{
    static Run first, next;
    static unsigned char rows[64 * 961];
    static const unsigned widths[] = {1, 2, 3, 7, 64, 399, 400, 401, 959, 960, 961};
    static const unsigned counts[] = {1, 2, 3, 59, 63, 64};
    unsigned parses = 0, reuses = 0;
    Opt o = {0};
    setup();
    o.lm = 1;
    TEST_ASSERT(!vjo_vocr_lm_drop(&lres));
    vjo_vocr_test_lm_counts(&parses, &reuses);
    run(&first, NULL, &o);
    TEST_ASSERT(first.rc == VJO_OK && first.st.lm_loaded && first.st.lm_lines > 0);
    vjo_vocr_test_lm_counts(&parses, &reuses);
    TEST_CHECK_(parses == 1 && reuses == 0, "the LM read: %u parses, %u reuses", parses, reuses);
    for (int k = 0; k < 2; k++) {
        run(&next, NULL, &o);
        vjo_vocr_test_lm_counts(&parses, &reuses);
        TEST_CHECK_(!next.st.lm_loaded && next.st.lm_cached && parses == 0 && reuses == 1,
                    "the LM kept: loaded %u cached %u, %u parses, %u reuses", next.st.lm_loaded, next.st.lm_cached,
                    parses, reuses);
        same_job(&first, &next, "the kept parse");
    }
    TEST_ASSERT(!vjo_vocr_lm_drop(&lres)); /* its block released: the next job reads and parses it again */
    run(&next, NULL, &o);
    vjo_vocr_test_lm_counts(&parses, &reuses);
    TEST_CHECK_(next.st.lm_loaded && parses == 1 && reuses == 0, "the LM read again: %u parses, %u reuses", parses,
                reuses);
    same_job(&first, &next, "the LM read again");
    for (size_t i = 0; i < sizeof(rows); i++) rows[i] = (unsigned char)((i * 2654435761u) >> 13);
    for (unsigned a = 0; a < sizeof(widths) / sizeof(widths[0]); a++)
        for (unsigned b = 0; b < sizeof(counts) / sizeof(counts[0]); b++) {
            uint32_t sums[64];
            const unsigned w = widths[a], n = counts[b];
            memset(sums, 0, sizeof(sums));
            vjo_vocr_test_row_sums(rows, w, n, sums);
            for (unsigned r = 0; r < n; r++)
                TEST_CHECK_(sums[r] == vjo_fnv1a(VJO_FNV_INIT, rows + (size_t)r * w, w), "width %u, %u rows: row %u", w,
                            n, r);
        }
}

TEST_LIST = {
    {"parallel_equals_serial", test_parallel_equals_serial},
    {"capacity_like_serial", test_capacity_like_serial},
    {"a_hit_whose_longest_reading_does_not_fit", test_a_hit_whose_longest_reading_does_not_fit},
    {"cache", test_cache},
    {"cache_with_lm_and_cascade", test_cache_with_lm_and_cascade},
    {"the_cache_replayed_through_the_old_layout", test_the_cache_replayed_through_the_old_layout},
    {"an_identical_region_reuses_the_last_text", test_an_identical_region_reuses_the_last_text},
    {"the_cache_keeps_a_greedy_text_after_the_shown_one", test_the_cache_keeps_a_greedy_text_after_the_shown_one},
    {"cancel_with_helpers_running", test_cancel_with_helpers_running},
    {"failed_join", test_failed_join},
    {"the_game_pool_after_a_timed_out_join", test_the_game_pool_after_a_timed_out_join},
    {"a_late_helper_asks_the_job_nothing", test_a_late_helper_asks_the_job_nothing},
    {"sizes_and_fewer_helpers", test_sizes_and_fewer_helpers},
    {"lm_parse_kept_and_row_sums", test_lm_parse_kept_and_row_sums},
    {NULL, NULL},
};

int main(int argc, char **argv)
{
    if (!vjo_test_tmpdir("vocr-parallel")) return 2; /* each test's model files in it, all removed at the end */
    return acutest_main(argc, argv);
}
