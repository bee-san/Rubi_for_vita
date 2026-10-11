/* Actual Lens control policy with only Vita scheduling and memory stubbed. */
#include "acutest.h"
#include "client.h"
#include "textfilter.h"
#define VJO_TEST_MEMORY_STUBS 1
#define VJO_TEST_THREAD_STUBS 1
#include <psp2/host_stubs.h>

static VjoState state;
static VjoConfig loaded;
static uint64_t test_now;
static unsigned posted, user_allocs, user_frees, heap_allocs, heap_frees, cancels;
static int fail_alloc, fail_base, input_blocked, fail_input_block, game_enabled = 1;
static uint8_t storage[2 * 1024 * 1024];
/* Separate simulated game process workspace. Shell allocator must never own it. */
static uint8_t vocr_storage[2 * 1024 * 1024] __attribute__((aligned(64)));
static unsigned vocr_allocs, vocr_frees, vocr_paf_allocs, jpeg_captures;
static int fail_vocr_user, fail_vocr_paf;
static int phases_checked, vocr_live, fail_join;
static unsigned char raw_region[200 * 60 * 4];
static VjoState raw_state;
static int has_result_memory(void);
uint64_t sceKernelGetProcessTimeWide(void) { return test_now; }
int sceKernelWaitThreadEnd(SceUID uid, int *status, void *timeout) { return fail_join ? -1 : 0; }
int sceKernelSetEventFlag(SceUID id, unsigned bits) { posted |= bits; return 0; }
int vjoGetState(VjoState *out) { *out = state; return 0; }
int vjoSetRegion(const VjoRect *r) { return 0; }
int vjoSetTriggers(int a, int b) { return 0; }
int vjoSetGameActive(int pid, int mode) { return 0; }
int vjoSetInputBlock(int on) { if (on && fail_input_block) return -123; input_blocked = on; return 0; }
int vjoGetVersion(void) { return VJO_API_VERSION; }
int vjoRegisterShell(void) { return 0; }
int vjoWaitEvent(uint32_t mask, uint32_t *out, uint32_t timeout) { *out = 0; return -1; }
SceUID sceKernelAllocMemBlock(const char *name, int type, unsigned size, void *opt)
{
    TEST_CHECK(strcmp(name, "VjoVocr") && strcmp(name, "VjoGameVocr"));
    if (phases_checked) TEST_CHECK(!vocr_live && size == 128 * 1024);
    user_allocs++; TEST_CHECK(size <= sizeof(storage)); return fail_alloc ? -1 : 7;
}
int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    if (uid == 9) { *base = vocr_storage; return 0; }
    *base = fail_base ? NULL : storage; return fail_base ? -1 : 0;
}
int sceKernelFreeMemBlock(SceUID uid) { if (uid == 9) { vocr_frees++; vocr_live = 0; } else user_frees++; return 0; }
int vjoReadRaw(uint32_t row, uint32_t n, void *dst)
{
    if (row >= raw_state.height) return -1;
    if (n > raw_state.height - row) n = raw_state.height - row;
    memcpy(dst, raw_region + (size_t)row * raw_state.raw_stride, (size_t)n * raw_state.raw_stride);
    return (int)n;
}

#include "../../shell/worker.c"

void *vjo_shell_heap_alloc(size_t size)
{ if (phases_checked) TEST_CHECK(!vocr_live && size == 128 * 1024);
  heap_allocs++; TEST_CHECK(size <= sizeof(storage)); return fail_alloc ? NULL : storage; }
void *vjo_shell_heap_alloc_for(const char *what, size_t size)
{
    (void)what; (void)size;
    vocr_paf_allocs++;
    TEST_CHECK(0); /* game vocr must never borrow the Shell Paf heap */
    return NULL;
}
void vjo_shell_heap_free(void *p)
{
    TEST_CHECK(p == storage || p == vocr_storage);
    if (p == vocr_storage) { vocr_frees++; vocr_live = 0; } else heap_frees++;
}
void vjo_config_load(VjoConfig *out, VjoArena *a) { *out = loaded; }
void vjo_log_configure(const VjoConfig *c) {}
void vjo_log(const char *fmt, ...) {}
int vjo_region_load(const char *id, VjoRect *out, VjoArena *a) { return 0; }
int vjo_region_save(const char *id, const VjoRect *r, VjoArena *a) { return 0; }
int vjo_anki_start(void) { return -1; }
void vjo_anki_stop(void) {}
void vjo_anki_configure(const VjoConfig *c) {}
void vjo_anki_post_check(unsigned seq) {}
void vjo_platform_vita(VjoPlatform *p, VjoNetCancel *c) {}
int vjo_net_cancel_init(VjoNetCancel *c) { return 0; }
void vjo_net_cancel(VjoNetCancel *c) { c->cancelled = 1; cancels++; }
void vjo_net_cancel_clear(VjoNetCancel *c) { c->cancelled = 0; }
int vjo_capture_init(void) { return 0; }
void vjo_capture_fini(void) {}
int vjo_capture_jpeg(VjoArena *a, uint32_t f, int q, VjoBuf *b, VjoState *s)
{ jpeg_captures++; return VJO_E_SOURCE; }
/* A finished region capture of raw_region, read through vjoReadRaw. */
int vjo_capture_raw(uint32_t f, int (*consume)(void *ud, const VjoState *st), void *ud, VjoState *s)
{
    *s = raw_state;
    return consume(ud, s);
}
int vjo_title_id(SceUID pid, char *out, int cap) { return -1; }
int vjo_title_game_mode(const char *id) { return VJO_GAME; }
int vjo_game_enabled(const char *id, VjoArena *a) { return game_enabled; }

int sceKernelDelayThread(unsigned us) { test_now += us; return 0; }
#ifdef VJO_WITH_VOCR
#include "../../core/vocr_ocr.h"
static VjoGameOcrRequest fake_game_request;
static VjoGameOcrResult fake_game_result;
static int32_t fake_model_io_error;
static const char *fake_recognized_text;
static int fake_game_rows(void *ud, unsigned y, unsigned n, unsigned char *dst)
{ (void)ud; return vjoReadRaw(y, n, dst); }
int vjoOcrSubmit(const VjoGameOcrRequest *r)
{
    fake_game_request = *r;
    TEST_CHECK(r->done_seq == raw_state.done_seq && !vocr_live);
    fake_game_request.seq = 12;
    fake_game_request.width = raw_state.width; fake_game_request.height = raw_state.height;
    memset(&fake_game_result, 0, sizeof(fake_game_result));
    fake_game_result.size = sizeof(fake_game_result); fake_game_result.seq = 12;
    return 12;
}
int vjoOcrRead(uint32_t seq, VjoGameOcrResult *out)
{
    TEST_CHECK(seq == 12);
    vocr_allocs++;
    if (phases_checked) TEST_CHECK(!has_result_memory());
    if (fail_vocr_user) fake_game_result.rc = VJO_E_OOM;
    else {
        size_t bytes = vjo_vocr_workspace_bytes(fake_game_request.model, raw_state.width, raw_state.height);
        VjoPlatform game_platform = {0}; VjoVocrStats stats;
        VjoOcrImage im = {NULL, raw_state.width, raw_state.height, fake_game_rows, NULL};
        vocr_live = 1;
        fake_game_result.rc = vjo_vocr_ocr(&game_platform, fake_game_request.model_dir,
            fake_game_request.model, &im, vocr_storage, bytes,
            fake_game_result.text, sizeof(fake_game_result.text), &stats);
        vocr_live = 0; vocr_frees++;
    }
    if (fake_game_result.rc == VJO_E_OCR_MODEL) fake_game_result.io_error = fake_model_io_error;
    /* A supplied successful worker response tests Shell publication, not OCR accuracy. */
    if (fake_game_result.rc == VJO_OK && fake_recognized_text)
        snprintf(fake_game_result.text, sizeof(fake_game_result.text), "%s", fake_recognized_text);
    *out = fake_game_result;
    return 0;
}
int vjoOcrCancel(uint32_t seq) { return 0; }
#endif

static void reset(void)
{
    if (has_result_memory()) mem_free();
    memset(&g_view, 0, sizeof(g_view));
    memset(&state, 0, sizeof(state));
    memset(&plat, 0, sizeof(plat));
    plat.pool = &pool;
    vjo_config_defaults(&loaded);
    loaded.dictionary = VJO_DICT_LOCAL;
    loaded.ocr_backend = VJO_OCR_LENS; /* existing Lens policy regression matrix */
    cfg = loaded;
    memcpy(title_id, "PCSG00415", 10);
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    state.scene = state.capture_scene = state.region_scene = 2;
    state.region_seq = 4;
    state.stable = 1;
    test_now = 10000000;
    posted = user_allocs = user_frees = heap_allocs = heap_frees = cancels = 0;
    fail_alloc = fail_base = input_blocked = fail_input_block = input_block_error = 0;
    game_enabled = 1;
    running = 1; threads_started = 0;
    vocr_allocs = vocr_frees = vocr_paf_allocs = jpeg_captures = 0;
    fail_vocr_user = fail_vocr_paf = 0;
#ifdef VJO_WITH_VOCR
    fake_model_io_error = 0;
    fake_recognized_text = NULL;
#endif
    phases_checked = vocr_live = fail_join = 0;
    memset(&raw_state, 0, sizeof(raw_state));
    raw_state.done_seq = 11;
    raw_state.width = 200;
    raw_state.height = 60;
    raw_state.raw_stride = 200 * 4;
    raw_state.capture_scene = 2;
    raw_state.region_seq = 4;
    memset(raw_region, 0, sizeof(raw_region));
    ov = OV_CLOSED;
    active = -1;
    game_generation = job_generation = 0;
    cache_ok = subtitles = stable_pending = warm_running = job_running = job_cancelled = 0;
    view_waiting = view_job_text = 0;
    memset(&ocr_backoff, 0, sizeof(ocr_backoff));
    memset(&dict_backoff, 0, sizeof(dict_backoff));
    memset(cache_data, 0, sizeof(cache_data));
}

static void test_local_open(void)
{
    reset();
    open_overlay();
    TEST_CHECK(job_running && job_lookup && input_blocked);
    TEST_CHECK(posted & NET_EV_JOB);
    TEST_CHECK(!g_view.status_is_error && want_lookup());
    TEST_CHECK(pool.n == POOL_SLOTS);
    job_running = 0;
    mem_free();
    TEST_CHECK(!has_result_memory() && pool.n == 0);
#ifdef VJO_PAF_ALLOC
    TEST_CHECK(heap_allocs == 1 && heap_frees == 1 && user_allocs == 0);
#else
    TEST_CHECK(user_allocs == 1 && user_frees == 1);
#endif
}

static void test_config_cache(void)
{
    reset();
    cache_ok = 1;
    apply_config();
    TEST_CHECK(cache_ok);
    strcpy(loaded.local_dictionaries, "daijirin.vjdict,main.vjdict");
    apply_config();
    TEST_CHECK(!cache_ok);
    cache_ok = 1;
    strcpy(loaded.local_dictionary_dir, "ux0:data/other");
    apply_config();
    TEST_CHECK(!cache_ok);
    loaded.dictionary = VJO_DICT_HACHIDORI;
    strcpy(loaded.hachidori_host, "192.168.1.2");
    apply_config();
    TEST_CHECK(want_lookup());
}

static void test_cached_scene(void)
{
    reset();
    TEST_ASSERT(mem_alloc() == 0);
    active = 0;
    cache_ok = 1;
    cache_scene = state.scene;
    cache_seq = state.region_seq;
    cache_data[0].list.header = "日本語";
    open_overlay();
    TEST_CHECK(!job_running && g_view.list == &cache_data[0].list);
    TEST_CHECK(input_blocked && !g_view.status_is_error);
    close_overlay();
    TEST_CHECK(!g_view.list && !input_blocked);
    mem_free();
}

static void test_stale_cancel(void)
{
    reset();
    job_running = job_captured = 1;
    job_capture_scene = 1;
    job_capture_seq = 1;
    TEST_CHECK(cancel_outdated_job(&state));
    TEST_CHECK(job_cancelled && cancels == 1);
    TEST_CHECK(!cancel_outdated_job(&state) && cancels == 1);
    job_running = 0;
}

static void test_background_local(void)
{
    reset();
    TEST_ASSERT(mem_alloc() == 0);
    stable_pending = 1;
    auto_prefetch();
    TEST_CHECK(job_running && job_lookup && (posted & NET_EV_JOB));
    job_running = 0;
    mem_free();
}

static void test_alloc_failure(void)
{
    reset();
    fail_alloc = 1;
    open_overlay();
    TEST_CHECK(!has_result_memory() && !job_running && g_view.status_is_error);
    TEST_CHECK(strstr(g_view.status, "memory") != NULL);
    fail_alloc = 0;
    open_overlay();
    TEST_CHECK(has_result_memory() && job_running);
    job_running = 0;
    mem_free();
#ifndef VJO_PAF_ALLOC
    fail_base = 1;
    TEST_CHECK(mem_alloc() < 0 && !has_result_memory());
    TEST_CHECK(user_frees == 2);
#endif
}

static void test_ocr_config_migration(void)
{
    VjoConfig c;
    const char old[] = "text_source = hooks\nocr_backend = meiki\n";
    vjo_config_defaults(&c);
    vjo_config_parse(&c, old, sizeof(old)-1);
    TEST_CHECK(c.text_source == VJO_SOURCE_OCR && c.ocr_backend == VJO_OCR_VOCR);
    TEST_CHECK(c.n_warnings == 2);
}

static void test_changed_dictionary_drops_running_result(void)
{
    reset();
    TEST_ASSERT(mem_alloc() == 0);
    active = 0;
    cache_ok = 1;
    cache_dictionary = cfg.dictionary;
    cache_data[0].filtered = "日本語";
    start_job("test", 1);
    ov = OV_OPEN_OLD_JOB;
    job_same_text = 1;
    strcpy(loaded.local_dictionaries, "new.vjdict");
    apply_config();
    TEST_CHECK(job_cancelled && cancels == 1 && !cache_ok);
    on_job_done();
    TEST_CHECK(job_running && !job_cached_text && !job_same_text && !cache_ok);
    TEST_CHECK(strcmp(job_cfg.local_dictionaries, "new.vjdict") == 0);
    TEST_CHECK(g_view.list != &cache_data[0].list);
    job_running = 0;
    mem_free();
}

static void test_game_exit_drains_previous_job(void)
{
    reset();
    TEST_ASSERT(mem_alloc() == 0);
    active = 0;
    cache_ok = 1;
    cache_dictionary = cfg.dictionary;
    cache_data[0].filtered = "日本語";
    cache_scene = state.scene;
    cache_seq = state.region_seq;
    start_job("old game", 1);
    job_same_text = 1;
    ov = OV_OPEN;
    input_blocked = 1;

    on_game_exit();
    TEST_CHECK(job_cancelled && cancels == 1 && !cache_ok);
    TEST_CHECK(!game_active() && ov == OV_CLOSED && !input_blocked);
    TEST_CHECK(has_result_memory()); /* the running job still owns its arena */
    TEST_CHECK(job_generation != game_generation);
    on_job_done();
    TEST_CHECK(!job_running && !cache_ok && !stable_pending);
    TEST_CHECK(!g_view.list);
    mem_free();
    TEST_CHECK(!has_result_memory() && pool.n == 0);
}

static void test_game_start_rejects_previous_generation(void)
{
    reset();
    TEST_ASSERT(mem_alloc() == 0);
    active = 0;
    cache_ok = 1;
    cache_dictionary = cfg.dictionary;
    cache_data[0].filtered = "日本語";
    cache_scene = state.scene;
    cache_seq = state.region_seq;
    start_job("old game", 1);
    cache_data[job_idx].sentence = "old game text";
    job_capture_scene = state.scene;
    job_capture_seq = state.region_seq;
    job_same_text = 1;
    ov = OV_OPEN;
    input_blocked = 1;
    state.game_pid = 101;

    on_game_start(); /* a foreground switch need not include GAME_EXIT */
    TEST_CHECK(job_cancelled && cancels == 1 && !cache_ok);
    TEST_CHECK(ov == OV_CLOSED && !input_blocked && pending_pid == 101);
    activate_game(state.game_pid, "PCSG00202", VJO_GAME);
    open_overlay(); /* the kernel has not sampled a new scene ID yet */
    TEST_CHECK(ov == OV_OPEN_OLD_JOB && !g_view.list && !cache_ok);

    /* The generation check is independent of the socket cancellation flag. */
    job_cancelled = 0;
    job_text_ready = 1;
    on_job_text();
    TEST_CHECK(!g_view.list && !view_job_text);
    on_job_done();
    TEST_CHECK(job_running && job_generation == game_generation);
    TEST_CHECK(!job_cached_text && !job_same_text && !cache_ok);
    TEST_CHECK(!g_view.list);
    job_running = 0;
    mem_free();
}

/* ---- ocr_backend = vocr ---- */

static int run_vocr_job(void)
{
    raw_state.done_seq = 11;
    TEST_ASSERT(mem_alloc() == 0);
    job_cfg = cfg;
    job_cancelled = 0;
    job_captured = 0;
    vjo_arena_reset(&results[0]);
    return run_job(&results[0], &cache_data[0]);
}

static void test_vocr_never_uses_lens(void)
{
    reset();
    cfg.ocr_backend = loaded.ocr_backend = VJO_OCR_VOCR;
    int rc = run_vocr_job();
    TEST_CHECK(jpeg_captures == 0);
#ifdef VJO_WITH_VOCR
    /* An empty region: no line, no model read, the workspace is released. */
    TEST_CHECK(rc == VJO_OK && cache_data[0].failed_stage == VJO_STAGE_NONE);
    TEST_CHECK(cache_data[0].ocr_text && !cache_data[0].ocr_text[0]);
    TEST_CHECK(job_captured && job_capture_scene == 2 && job_capture_seq == 4);
    TEST_CHECK(vocr_allocs == 1 && vocr_frees == 1 && vocr_paf_allocs == 0);
#else
    TEST_CHECK(rc == VJO_E_OCR_UNAVAILABLE && cache_data[0].failed_stage == VJO_STAGE_OCR);
    TEST_CHECK(cache_data[0].err.detail && strstr(cache_data[0].err.detail, "vita-vn-ocr"));
    TEST_CHECK(vocr_allocs == 0);
#endif
    mem_free();
}

static void test_vocr_config(void)
{
    VjoConfig c;
    const char ini[] = "ocr_backend = VOCR\nvocr_model = FL10_w8.vocr\n";
    const char bad[] = "vocr_model = ../H15_w8.vocr\nvocr_model = H15_w8.bin\nvocr_model =\n";
    vjo_config_defaults(&c);
    TEST_CHECK(c.ocr_backend == VJO_OCR_VOCR && !strcmp(c.vocr_model, "FL10_w8.vocr"));
    vjo_config_parse(&c, ini, sizeof(ini) - 1);
    TEST_CHECK(c.ocr_backend == VJO_OCR_VOCR && !strcmp(c.vocr_model, "FL10_w8.vocr") && c.n_warnings == 0);
    vjo_config_defaults(&c);
    vjo_config_parse(&c, bad, sizeof(bad) - 1);
    TEST_CHECK(!strcmp(c.vocr_model, "FL10_w8.vocr") && c.n_warnings == 3); /* each kept the default */
}

static void test_vocr_warms_no_lens_connection(void)
{
    reset();
    TEST_ASSERT(mem_alloc() == 0);
    cfg.ocr_backend = VJO_OCR_VOCR;
    state.unsettled_ms = 100;
    warm_connections();
    TEST_CHECK(!(posted & NET_EV_WARM) && !warm_running);
    cfg.ocr_backend = VJO_OCR_LENS;
    warm_connections();
    TEST_CHECK((posted & NET_EV_WARM) && warm_running);
    warm_running = 0;
    mem_free();
}

static void test_vocr_model_change_invalidates_cache(void)
{
    reset();
    loaded.ocr_backend = VJO_OCR_VOCR;
    apply_config();
    cache_ok = 1;
    strcpy(loaded.vocr_model, "H15_w8.vocr");
    apply_config();
    TEST_CHECK(!cache_ok);
}

#ifdef VJO_WITH_VOCR
static void strokes(unsigned y0, unsigned y1)
{
    for (unsigned y = y0; y < y1; y++)
        for (unsigned x = 10; x < 190; x += 6)
            memset(raw_region + ((size_t)y * 200 + x) * 4, 235, 6); /* 1.5 px of R, G, B */
}

static void test_vocr_missing_model_releases_workspace(void)
{
    reset();
    cfg.ocr_backend = VJO_OCR_VOCR;
    strokes(15, 39); /* one light line on black */
    int rc = run_vocr_job();
    TEST_CHECK(rc == VJO_E_OCR_MODEL && cache_data[0].failed_stage == VJO_STAGE_OCR);
    TEST_CHECK(!cache_data[0].sentence && vocr_allocs == 1 && vocr_frees == 1);
    mem_free();
}

static void test_vocr_allocation_fallback_and_failure(void)
{
    reset();
    cfg.ocr_backend = VJO_OCR_VOCR;
    fail_vocr_user = 1;
    int rc = run_vocr_job();
    TEST_CHECK(rc == VJO_E_OOM && cache_data[0].failed_stage == VJO_STAGE_OCR);
    TEST_CHECK(vocr_paf_allocs == 0 && vocr_frees == 0);
    mem_free();
}

static void test_vocr_storage_errors_are_specific(void)
{
    const struct { uint32_t io; const char *message; int specific; } cases[] = {
        {0x80010013u, "storage is unavailable to this game", 1}, /* ENODEV */
        {0x8001000Du, "Access to the local OCR model was denied", 1}, /* EACCES */
        {0x80010002u, "Local OCR model is missing or invalid", 0}, /* ENOENT */
        {0, "Local OCR model is missing or invalid", 0}, /* size/hash/format rejection */
    };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        reset();
        cfg.ocr_backend = VJO_OCR_VOCR;
        fake_model_io_error = (int32_t)cases[i].io;
        strokes(15, 39);
        TEST_CHECK(run_vocr_job() == VJO_E_OCR_MODEL);
        TEST_CHECK(!!cache_data[0].err.detail == cases[i].specific);
        char memory[256]; VjoArena arena;
        vjo_arena_init(&arena, memory, sizeof(memory));
        const char *message = vjo_err_text(&arena, VJO_STAGE_OCR, &cache_data[0].err);
        TEST_CHECK(strstr(message, cases[i].message) != NULL);
        if (cases[i].specific) TEST_CHECK(strstr(message, "prepare_ocr.py") == NULL);
        TEST_CHECK(!cache_data[0].sentence && vocr_allocs == 1 && vocr_frees == 1);
        mem_free();
    }
    reset();
    cfg.ocr_backend = VJO_OCR_VOCR;
    fake_model_io_error = (int32_t)0x80010013u;
    strokes(15, 39);
    TEST_CHECK(run_vocr_job() == VJO_E_OCR_MODEL && cache_data[0].err.detail);
    /* Rejecting a name before submitting must not reuse the previous job's I/O error. */
    strcpy(cfg.vocr_model, "unsupported-model-name-longer-than-the-wire.vocr");
    TEST_CHECK(run_vocr_job() == VJO_E_OCR_MODEL && !cache_data[0].err.detail);
    mem_free();
}

#endif

static void test_input_block_denied(void)
{
    reset();
    fail_input_block = 1;
    open_overlay();
    TEST_CHECK(ov == OV_CLOSED && !job_running && !input_blocked && !g_view.open);
    TEST_CHECK(g_view.strip_on && g_view.strip_kind == VJO_STRIP_ERROR);
    fail_input_block = 0;
    open_overlay();
    TEST_CHECK(input_blocked && job_running && !input_block_error);
    job_running = 0;
    mem_free();
}

static void test_disabled_game_retires_work(void)
{
    reset();
    open_overlay();
    TEST_ASSERT(job_running && input_blocked);
    game_enabled = 0;
    on_game_start();
    activate_game(123, "PCSA00029", VJO_GAME);
    TEST_CHECK(!game_active() && !input_blocked && job_cancelled && !g_view.open);
    job_running = 0;
    mem_free();
}

static void test_failed_join_keeps_allocations(void)
{
    for (int local = 0; local < 2; local++) {
        reset();
        if (local) cfg.ocr_backend = VJO_OCR_VOCR;
        TEST_ASSERT(mem_alloc() == 0);
        uint8_t *base = results[0].base;
        size_t bytes = results[0].size;
        threads_started = 1;
        fail_join = 1;
        TEST_CHECK(vjo_worker_stop() < 0);
        TEST_CHECK(has_result_memory() && results[0].base == base && results[0].size == bytes && threads_started == 1);
        fail_join = 0;
        TEST_CHECK(vjo_worker_stop() == 0);
        TEST_CHECK(!has_result_memory() && threads_started == 0);
    }
}

#ifdef VJO_WITH_VOCR
static void test_shell_result_oom_preserves_recognition(void)
{
    reset();
    cfg.ocr_backend = loaded.ocr_backend = VJO_OCR_VOCR;
    cfg.non_japanese_filter = VJO_FILTER_LINES;
    fake_recognized_text = "HUD only\r\n  日本語です  \r\nEnglish only";
    subtitles = 1;
    ov = OV_OPEN;
    fail_alloc = 1;
    backoff_note(&ocr_backoff, 1); /* previous OCR failure must be cleared */
    start_job("Shell result refusal after recognition", 1);
    int idx = job_idx;
    TEST_CHECK(run_job(&results[idx], &cache_data[idx]) == VJO_E_OOM);
    VjoOverlayData *d = &cache_data[idx];
    TEST_CHECK(d->failed_stage == VJO_STAGE_DICT && d->err.dict == VJO_DICT_LOCAL);
    TEST_CHECK(d->ocr_text && !strcmp(d->ocr_text, fake_recognized_text));
    TEST_CHECK(d->sentence && !strcmp(d->sentence, "日本語です"));
    TEST_CHECK(d->list.header && !strcmp(d->list.header, "日本語です"));
    TEST_CHECK(!d->list.n_entries && !d->list.entries);
    TEST_CHECK(!has_result_memory() && !vocr_live && vocr_allocs == vocr_frees);
    on_job_done();
    TEST_CHECK(!ocr_backoff.until && !ocr_backoff.us);
    TEST_CHECK(dict_backoff.until > now_us() && dict_backoff.us == BACKOFF_MIN_US);
    TEST_CHECK(g_view.status_is_error && strstr(g_view.status, "Shell UI memory"));
    TEST_CHECK(!strstr(g_view.status, "Text recognition failed") && !strstr(g_view.status, "region"));
    TEST_CHECK(g_view.list == &d->list && !strcmp(g_view.list->header, "日本語です"));
    TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !strcmp(g_view.strip_text, "日本語です"));
    TEST_CHECK(!g_view.strip_busy && !job_running && !cache_ok);

    /* A new job removes every published fallback pointer before reuse. */
    close_overlay(); /* background OCR-only jobs run with the overlay closed */
    fail_alloc = 0;
    fake_recognized_text = "次の文";
    start_job("Shell memory recovered", 0);
    TEST_CHECK(!g_view.list && !has_result_memory());
    TEST_CHECK(run_job(&results[job_idx], &cache_data[job_idx]) == VJO_OK);
    on_job_done();
    TEST_CHECK(!strcmp(g_view.strip_text, "次の文") && !ocr_backoff.until);
    TEST_CHECK(!has_result_memory() && vocr_allocs == vocr_frees);
    mem_free();
    TEST_CHECK(!has_result_memory());
}

static void test_shell_result_oom_keeps_max_worker_text(void)
{
    char longest[VJO_GAME_OCR_TEXT_BYTES];
    for (size_t i = 0; i < sizeof(longest) - 1; i += 3)
        memcpy(longest + i, "語", 3);
    longest[sizeof(longest) - 1] = 0;
    for (int mode = VJO_FILTER_NONE; mode <= VJO_FILTER_LINES; mode++) {
        reset();
        cfg.ocr_backend = VJO_OCR_VOCR;
        cfg.non_japanese_filter = mode;
        fake_recognized_text = longest;
        fail_alloc = 1;
        start_job("maximum recognized text under Shell pressure", 1);
        TEST_CHECK(run_job(&results[job_idx], &cache_data[job_idx]) == VJO_E_OOM);
        VjoOverlayData *d = &cache_data[job_idx];
        TEST_CHECK(d->failed_stage == VJO_STAGE_DICT);
        TEST_CHECK(d->sentence && !strcmp(d->sentence, longest));
        TEST_CHECK(d->list.header && !strcmp(d->list.header, longest));
        TEST_CHECK(!has_result_memory() && !vocr_live && vocr_allocs == vocr_frees);
        job_running = 0;
    }
}

static void test_local_phases_and_recovery(void)
{
    reset();
    cfg.ocr_backend = loaded.ocr_backend = VJO_OCR_VOCR;
    phases_checked = 1;
    for (int i = 0; i < 50; i++) {
        start_job("local phase check", 0);
        TEST_CHECK(!has_result_memory() && pool.n == 0 && g_view.list == NULL);
        TEST_CHECK(run_job(&results[job_idx], &cache_data[job_idx]) == VJO_OK);
        TEST_CHECK(!has_result_memory() && !results[0].size && !results[1].size && pool.n == 0);
        TEST_CHECK(!user_allocs && !heap_allocs);
        TEST_CHECK(vocr_allocs == vocr_frees && !vocr_live);
        job_running = 0;
    }
    fail_vocr_user = fail_vocr_paf = 1;
    start_job("local allocation refusal", 0);
    TEST_CHECK(run_job(&results[job_idx], &cache_data[job_idx]) == VJO_E_OOM);
    TEST_CHECK(!has_result_memory() && !vocr_live);
    job_running = 0;
    fail_vocr_user = fail_vocr_paf = 0;
    start_job("local recovered", 0);
    TEST_CHECK(run_job(&results[job_idx], &cache_data[job_idx]) == VJO_OK);
    job_running = 0;
    mem_free();
    TEST_CHECK(!has_result_memory() && user_allocs == user_frees && heap_allocs == heap_frees);
    phases_checked = 0;
}

static void test_local_lookup_single_arena_and_network_double_buffers(void)
{
    reset();
    cfg.ocr_backend = VJO_OCR_VOCR;
    phases_checked = 1;
    ov = OV_OPEN;
    start_job("local lookup", 1);
    TEST_CHECK(job_idx == 0 && !g_view.list);
    TEST_CHECK(run_job(&results[job_idx], &cache_data[job_idx]) == VJO_OK);
    TEST_CHECK(has_result_memory() && results[0].size == 128 * 1024);
    TEST_CHECK(!results[1].base && !results[1].size && !pool.n);
    on_job_done();
    TEST_CHECK(cache_ok && active == 0 && g_view.list == &cache_data[0].list);
    start_job("local OCR only replaces cached lookup", 0);
    TEST_CHECK(job_idx == 0 && !has_result_memory() && !g_view.list && !cache_ok);
    TEST_CHECK(run_job(&results[job_idx], &cache_data[job_idx]) == VJO_OK);
    TEST_CHECK(!has_result_memory());
    job_running = 0;

    phases_checked = 0;
    cfg.ocr_backend = VJO_OCR_LENS;
    TEST_CHECK(mem_alloc() == 0 && !mem_local);
    TEST_CHECK(results[0].size == RESULT_ARENA_SIZE && results[1].size == RESULT_ARENA_SIZE);
    TEST_CHECK(results[1].base == results[0].base + RESULT_ARENA_SIZE && pool.n == POOL_SLOTS);
    active = 0;
    start_job("network keeps double buffering", 0);
    TEST_CHECK(job_idx == 1);
    job_running = 0;
    mem_free();
}

static void test_local_switch_drains_warm_pool(void)
{
    reset();
    TEST_ASSERT(mem_alloc() == 0 && pool.n > 0);
    uint8_t *base = results[0].base;
    warm_running = 1;
    cfg.ocr_backend = VJO_OCR_VOCR;
    active = 0; /* old network result must not select local result1 */
    start_job("switch from warm Lens to local OCR", 0);
    TEST_CHECK(job_idx == 0);
    /* The control thread must not free a pool still used by the worker. */
    TEST_CHECK(has_result_memory() && results[0].base == base && pool.n > 0);
    TEST_CHECK(job_cached_text == NULL);
    warm_running = 0;
    phases_checked = 1;
    TEST_CHECK(run_job(&results[job_idx], &cache_data[job_idx]) == VJO_OK);
    TEST_CHECK(!has_result_memory() && pool.n == 0 && !results[0].size && !results[1].size);
    phases_checked = 0;
    job_running = 0;
    cfg.ocr_backend = VJO_OCR_LENS;
    start_job("switch from local OCR to Lens", 0);
    TEST_CHECK(run_job(&results[job_idx], &cache_data[job_idx]) == VJO_E_SOURCE);
    TEST_CHECK(!mem_local && pool.n > 0 && results[0].size == RESULT_ARENA_SIZE);
    job_running = 0;
    mem_free();
}
#endif

TEST_LIST = {
    {"input_block_refusal_keeps_overlay_closed", test_input_block_denied},
    {"disabled_game_retires_previous_job", test_disabled_game_retires_work},
    {"failed_join_keeps_allocations_for_retry", test_failed_join_keeps_allocations},
#ifdef VJO_WITH_VOCR
    {"Shell_result_OOM_preserves_OCR_sentence_and_dictionary_backoff", test_shell_result_oom_preserves_recognition},
    {"Shell_result_OOM_preserves_maximum_filtered_worker_text", test_shell_result_oom_keeps_max_worker_text},
    {"local_lookup_uses_one_arena_and_network_keeps_two", test_local_lookup_single_arena_and_network_double_buffers},
    {"local_ocr_and_dictionary_phases_repeat_and_recover", test_local_phases_and_recovery},
    {"local_switch_waits_for_warm_pool_and_restores_network_memory", test_local_switch_drains_warm_pool},
#endif
    {"vocr_never_uploads_to_lens", test_vocr_never_uses_lens},
    {"vocr_config_parses_and_validates", test_vocr_config},
    {"vocr_opens_no_lens_connection", test_vocr_warms_no_lens_connection},
    {"vocr_model_change_invalidates_cache", test_vocr_model_change_invalidates_cache},
#ifdef VJO_WITH_VOCR
    {"vocr_missing_model_releases_workspace", test_vocr_missing_model_releases_workspace},
    {"vocr_storage_errors_have_specific_messages", test_vocr_storage_errors_are_specific},
    {"game_vocr_OOM_never_falls_back_to_Shell", test_vocr_allocation_fallback_and_failure},
#endif
    {"local_dictionary_opens_without_key", test_local_open},
    {"ordered_dictionary_config_invalidates_cache", test_config_cache},
    {"scene_cache_is_immediate", test_cached_scene},
    {"outdated_scene_cancels_once", test_stale_cancel},
    {"local_dictionary_background_prefetch", test_background_local},
    {"allocation_failure_recovers_and_releases", test_alloc_failure},
    {"experimental_config_migrates_to_ocr", test_ocr_config_migration},
    {"changed_dictionary_drops_inflight_cached_result", test_changed_dictionary_drops_running_result},
    {"game_exit_drains_previous_job_without_reviving_cache", test_game_exit_drains_previous_job},
    {"game_start_rejects_previous_generation_and_restarts", test_game_start_rejects_previous_generation},
    {NULL, NULL}
};
