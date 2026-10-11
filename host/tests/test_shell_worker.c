/* Actual Lens control policy with only Vita scheduling and memory stubbed. */
#include "acutest.h"
#include "client.h"
#include "textfilter.h"
#define VJO_TEST_MEMORY_STUBS 1
#define VJO_TEST_THREAD_STUBS 1
#define VJO_TEST_MUTEX_STUBS 1
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
static int held_cmd, held_view, retirement_checks;
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
    if (phases_checked) TEST_CHECK(!vocr_live && size == 64 * 1024);
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

int sceKernelLockMutex(SceUID uid, int count, void *timeout)
{
    if (uid == 81) { TEST_CHECK(!held_view); held_view++; }
    if (uid == 82) { TEST_CHECK(!held_cmd && !held_view); held_cmd++; }
    return 0;
}
int sceKernelUnlockMutex(SceUID uid, int count)
{
    if (uid == 81) { TEST_CHECK(held_view == 1); held_view--; }
    if (uid == 82) {
        TEST_CHECK(held_cmd == 1 && !held_view);
        if (!local_text_valid && !g_view.list && !g_view.local_cursor_mode) retirement_checks++;
        held_cmd--;
    }
    return 0;
}

void *vjo_shell_heap_alloc(size_t size)
{ if (phases_checked) TEST_CHECK(!vocr_live && size == 64 * 1024);
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
    else if (!strcmp(fake_game_request.model, VJO_MEIKI_MODEL_NAME)) {
        fake_game_result.rc = VJO_OK; vocr_frees++;
    } else {
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

static int saved_notes, save_rc;
int vjo_queue_count(void) { return saved_notes; }
int vjo_queue_save(VjoArena *a, const VjoAnkiNote *n, const VjoConfig *c, const char *id)
{ (void)a; TEST_CHECK(c->anki_mining); TEST_CHECK(!strcmp(id, "PCSG00415"));
  TEST_CHECK(n->spelling && *n->spelling); if (save_rc >= 0) saved_notes++; return save_rc; }

static void reset(void)
{
    if (has_result_memory()) mem_free();
    memset(&g_view, 0, sizeof(g_view));
    view_lock = 81;
    cmd_lock = 82;
    held_cmd = held_view = retirement_checks = 0;
    local_text[0] = 0;
    local_text_valid = cursor_waiting = job_cursor_only = job_save_only = 0;
    job_save_list = NULL;
    saved_notes = save_rc = 0;
    local_text_generation = local_text_game_generation = 0;
    local_cursor_seq = local_cursor_byte = 0;
    job_cursor_byte = job_cursor_seq = job_text_generation = 0;
    pending_cmd = VJO_CMD_NONE;
    pending_cursor_byte = pending_cursor_generation = 0;
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
    TEST_CHECK(c.text_source == VJO_SOURCE_OCR && c.ocr_backend == VJO_OCR_MEIKI);
    TEST_CHECK(c.n_warnings == 1);
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

static void test_meiki_manual_local_policy(void)
{
    reset(); cfg.ocr_backend = VJO_OCR_MEIKI; cfg.ocr_mode = VJO_OCR_AUTO;
    loaded = cfg; subtitles = 1;
    TEST_CHECK(local_pipeline(&cfg) && !background_wanted());
    subtitles = 0; set_subtitles(1);
    TEST_CHECK(!subtitles && !job_running);
    fake_recognized_text = "日本語";
    start_job("explicit Meiki opening", 0);
    TEST_CHECK(run_job(&results[job_idx], &cache_data[job_idx]) == VJO_OK);
    TEST_CHECK(!strcmp(fake_game_request.model, VJO_MEIKI_MODEL_NAME));
    TEST_CHECK(!has_result_memory() && !jpeg_captures && local_text_valid && !strcmp(local_text,"日本語"));
    job_cancelled=1; ov=OV_OPEN_OLD_JOB;
    unsigned before=vocr_allocs; on_job_done();
    TEST_CHECK(!job_running && vocr_allocs==before && strstr(g_view.status,"Close and reopen"));
    ov=OV_CLOSED; stable_pending=1; state.stable=1; auto_prefetch();
    TEST_CHECK(!job_running && vocr_allocs==before);
    fake_recognized_text=NULL;
    cfg.ocr_backend=VJO_OCR_VOCR; TEST_CHECK(background_wanted());
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

/* Tiny generated dictionary exercises actual point-lookup file callbacks.
 * All text below is a hand-written fixture, not game or dictionary data. */
static unsigned char cursor_dict[1024];
static size_t cursor_dict_size;
static unsigned cursor_opens, cursor_reads, cursor_closes;
static int cursor_handle_live, read_action;
static void cursor_u16(unsigned char *p, uint16_t x) { p[0] = x; p[1] = x >> 8; }
static void cursor_u32(unsigned char *p, uint32_t x) { cursor_u16(p, x); cursor_u16(p + 2, x >> 16); }
static void cursor_u64(unsigned char *p, uint64_t x) { cursor_u32(p, x); cursor_u32(p + 4, x >> 32); }
static void cursor_build_dict(void)
{
    const char *keys[] = {"犬", "猫", "鳥"};
    const char *meanings[] = {"dog", "cat", "bird"};
    size_t off = 64 + 3 * 24;
    memset(cursor_dict, 0, sizeof(cursor_dict));
    memcpy(cursor_dict, "VJDICT1\0", 8);
    cursor_u32(cursor_dict + 8, 1); cursor_u32(cursor_dict + 12, 64);
    cursor_u32(cursor_dict + 16, 3); cursor_u32(cursor_dict + 20, 24);
    cursor_u64(cursor_dict + 24, 64); cursor_u64(cursor_dict + 32, off);
    for (int i = 0; i < 3; i++) {
        unsigned char *record = cursor_dict + 64 + 24 * i;
        cursor_u64(record, off); cursor_u16(record + 16, 3);
        memcpy(cursor_dict + off, keys[i], 3); off += 3;
        size_t payload = off;
        const char *parts[] = {keys[i], keys[i], meanings[i]};
        for (int j = 0; j < 3; j++) {
            size_t n = strlen(parts[j]) + 1;
            memcpy(cursor_dict + off, parts[j], n); off += n;
        }
        cursor_u64(record + 8, payload); cursor_u32(record + 20, off - payload);
    }
    cursor_dict_size = off; cursor_u64(cursor_dict + 40, off);
}
static int cursor_file_read(void *ud, uint64_t off, void *dst, size_t n)
{
    (void)ud;
    cursor_reads++;
    TEST_CHECK(cursor_handle_live == 1);
    if (read_action) {
        int action = read_action; read_action = 0;
        if (action == 1) { vjo_post_cursor(6, local_text_generation); on_command(); }
        if (action == 2) close_overlay();
        if (action == 3) { close_overlay(); open_overlay(); }
        if (action == 4) invalidate_game_work();
        if (action == 5) { state.scene = state.region_scene = 8; state.region_seq++; }
    }
    if (off > cursor_dict_size || n > cursor_dict_size - off) return -1;
    memcpy(dst, cursor_dict + off, n); return 0;
}
static void cursor_file_close(void *ud)
{ (void)ud; TEST_CHECK(cursor_handle_live == 1); cursor_handle_live = 0; cursor_closes++; }
static int cursor_file_open(void *ud, const char *path, VjoFile *out)
{
    (void)ud;
    TEST_CHECK(strstr(path, "main.vjdict") != NULL && !cursor_handle_live);
    cursor_handle_live = 1; cursor_opens++;
    *out = (VjoFile){NULL, cursor_dict_size, cursor_file_read, cursor_file_close}; return 0;
}
static void cursor_setup(void)
{
    reset(); cursor_build_dict();
    cursor_opens = cursor_reads = cursor_closes = 0;
    cursor_handle_live = read_action = 0;
    cfg.ocr_backend = loaded.ocr_backend = VJO_OCR_VOCR;
    cfg.non_japanese_filter = loaded.non_japanese_filter = VJO_FILTER_NONE;
    cfg.ocr_mode = loaded.ocr_mode = VJO_OCR_AUTO;
    strcpy(cfg.local_dictionaries, "main.vjdict");
    strcpy(loaded.local_dictionaries, "main.vjdict");
    plat.file_open = cursor_file_open;
    fake_recognized_text = "猫犬鳥";
}
static void finish_current_job(void)
{
    TEST_ASSERT(job_running);
    int idx = job_idx;
    job_rc = job_save_only ? run_save_job(&results[idx])
           : job_cursor_only ? run_cursor_job(&results[idx], &cache_data[idx])
                            : run_job(&results[idx], &cache_data[idx]);
    job_done = 1; on_job_done();
}
static void cursor_saved_text(void)
{
    cursor_setup();
    cfg.highlight_mode = loaded.highlight_mode = VJO_HIGHLIGHT_CHARACTER;
    stable_pending = 1; auto_prefetch();
    TEST_ASSERT(job_running && !job_lookup && !job_cursor_only);
    finish_current_job();
    TEST_CHECK(local_text_valid && !strcmp(local_text, "猫犬鳥"));
    TEST_CHECK(!has_result_memory() && !cursor_opens && !user_allocs && !heap_allocs);
    TEST_CHECK(vocr_allocs == 1 && vocr_frees == 1 && !jpeg_captures);
}
static void check_current_definition(const char *meaning, uint32_t cursor)
{
    TEST_ASSERT(g_view.list != NULL);
    TEST_CHECK(g_view.local_cursor_mode && g_view.cursor_byte == cursor);
    TEST_CHECK(!strcmp(g_view.list->header, "猫犬鳥"));
    TEST_ASSERT(g_view.list->n_entries == 1);
    TEST_CHECK(strstr(g_view.list->entries[0].text, meaning) != NULL);
    TEST_CHECK(g_view.list->entries[0].hl_start == (int)cursor);
    TEST_CHECK(cursor_opens == cursor_closes && !cursor_handle_live);
    TEST_CHECK(!held_cmd && !held_view);
}
static void test_text_save_uses_existing_worker_and_survives_exit(void)
{
    cursor_saved_text(); open_overlay(); finish_current_job();
    cfg.anki_mining = 1;
    const VjoEntryList *list = g_view.list;
    unsigned allocations = user_allocs + heap_allocs, captures = vocr_allocs;
    size_t used = results[active].used;
    vjo_post_save(g_view.list_seq, 0); on_command();
    TEST_ASSERT(job_running && job_save_only && job_save_list == list);
    on_game_exit();
    TEST_CHECK(job_running && has_result_memory() && job_save_list == list);
    TEST_CHECK(!title_id[0] && !g_view.list);
    finish_current_job();
    TEST_CHECK(saved_notes == 1 && !job_running && !job_save_list);
    TEST_CHECK(!g_view.queue_error && strstr(g_view.queue_status, "Saved locally"));
    TEST_CHECK(user_allocs + heap_allocs == allocations && vocr_allocs == captures);
    TEST_CHECK(results[job_idx].used == used && !held_view && !held_cmd);
    mem_free();
}
static void test_text_save_rejects_disabled_stale_busy_requests(void)
{
    cursor_saved_text(); open_overlay(); finish_current_job();
    uint32_t seq = g_view.list_seq;
    vjo_post_save(seq, 0); on_command();
    TEST_CHECK(!job_running && !saved_notes && g_view.queue_error);
    cfg.anki_mining = 1;
    vjo_post_save(seq - 1, 0); on_command();
    TEST_CHECK(!job_running && !saved_notes && strstr(g_view.queue_status, "changed"));
    vjo_post_save(seq, 9); on_command();
    TEST_CHECK(!job_running && !saved_notes);
    warm_running = 1; vjo_post_save(seq, 0); on_command();
    TEST_CHECK(!job_running && !saved_notes && strstr(g_view.queue_status, "busy"));
    warm_running = 0;
    vjo_post_command(VJO_CMD_CLOSED, NULL); vjo_post_save(seq, 0);
    TEST_CHECK(pending_cmd == VJO_CMD_CLOSED); on_command();
    TEST_CHECK(ov == OV_CLOSED && !saved_notes);
    mem_free();
}
static void test_text_save_failure_keeps_definition_and_retry(void)
{
    cursor_saved_text(); open_overlay(); finish_current_job(); cfg.anki_mining = 1;
    const VjoEntryList *list = g_view.list;
    unsigned allocations = user_allocs + heap_allocs;
    save_rc = -3;
    vjo_post_save(g_view.list_seq, 0); on_command(); finish_current_job();
    TEST_CHECK(!saved_notes && g_view.list == list && g_view.queue_error);
    TEST_CHECK(strstr(g_view.queue_status, "memory") && !job_running);
    check_current_definition("cat", 0);
    save_rc = 0;
    vjo_post_save(g_view.list_seq, 0); on_command(); finish_current_job();
    TEST_CHECK(saved_notes == 1 && !g_view.queue_error && g_view.list == list);
    TEST_CHECK(user_allocs + heap_allocs == allocations);
    close_overlay(); mem_free();
}
static void test_japanese_only_selection_and_config_reload(void)
{
    cursor_setup();
    fake_recognized_text = "ABC「猫」 123 한글。犬!";
    open_overlay(); finish_current_job();
    uint32_t first = (uint32_t)(strstr(local_text, "猫") - local_text);
    uint32_t second = (uint32_t)(strstr(local_text, "犬") - local_text);
    TEST_CHECK(g_view.japanese_only_selection && g_view.cursor_byte == first);
    TEST_ASSERT(g_view.list && g_view.list->n_entries == 1);
    TEST_CHECK(strstr(g_view.list->entries[0].text, "cat") != NULL);
    unsigned inferences = vocr_allocs;
    uint32_t generation = local_text_generation;
    vjo_post_cursor(0, generation);
    TEST_CHECK(pending_cmd == VJO_CMD_NONE && g_view.cursor_byte == first);
    vjo_post_cursor(second, generation); on_command(); finish_current_job();
    TEST_CHECK(g_view.cursor_byte == second && vocr_allocs == inferences);
    TEST_ASSERT(g_view.list && g_view.list->n_entries == 1);
    TEST_CHECK(strstr(g_view.list->entries[0].text, "dog") != NULL);
    /* Presentation changes reuse the immutable recognition and lookup.
     * They must not recapture or allocate a second inference workspace. */
    TEST_CHECK(g_view.highlight_mode == VJO_HIGHLIGHT_WORD);
    close_overlay(); loaded.highlight_mode = VJO_HIGHLIGHT_CHARACTER;
    unsigned reads_before = cursor_reads;
    open_overlay();
    TEST_CHECK(g_view.highlight_mode == VJO_HIGHLIGHT_CHARACTER);
    TEST_CHECK(local_text_valid && !job_running && cursor_reads == reads_before);
    TEST_CHECK(local_text_valid && !job_running && vocr_allocs == inferences);
    TEST_CHECK(g_view.cursor_byte == second);
    close_overlay(); loaded.highlight_mode = VJO_HIGHLIGHT_WORD;
    open_overlay();
    TEST_CHECK(g_view.highlight_mode == VJO_HIGHLIGHT_WORD);
    TEST_CHECK(local_text_valid && !job_running && cursor_reads == reads_before);
    TEST_CHECK(local_text_valid && !job_running && vocr_allocs == inferences);
    close_overlay(); loaded.japanese_only_selection = 0;
    open_overlay();
    TEST_CHECK(!local_text_valid && job_running && !job_cursor_only);
    finish_current_job();
    TEST_CHECK(!g_view.japanese_only_selection && g_view.cursor_byte == 0);
    generation = local_text_generation;
    vjo_post_cursor(1, generation); on_command(); finish_current_job();
    TEST_CHECK(g_view.cursor_byte == 1);
    close_overlay(); loaded.ocr_backend = VJO_OCR_MEIKI;
    open_overlay();
    TEST_CHECK(cfg.ocr_backend == VJO_OCR_MEIKI && job_cfg.ocr_backend == VJO_OCR_MEIKI);
    TEST_CHECK(!background_wanted());
    close_overlay(); finish_current_job(); mem_free();
}

static void test_no_japanese_selection_skips_dictionary_memory_and_io(void)
{
    cursor_setup();
    fake_recognized_text = "\xF0\x83\x81\x82" "ABC 123 한글・「」😀";
    open_overlay(); finish_current_job();
    TEST_CHECK(g_view.local_cursor_mode && g_view.japanese_only_selection);
    TEST_ASSERT(g_view.list != NULL);
    TEST_CHECK(g_view.list->n_entries == 0 && !strcmp(g_view.list->header, fake_recognized_text));
    TEST_CHECK(g_view.cursor_byte == strlen(local_text));
    TEST_CHECK(!cursor_opens && !has_result_memory() && !user_allocs && !heap_allocs);
    vjo_post_cursor(0, local_text_generation);
    TEST_CHECK(pending_cmd == VJO_CMD_NONE && !job_running);
    close_overlay(); open_overlay();
    TEST_CHECK(!job_running && g_view.list && !g_view.list->n_entries);
    TEST_CHECK(!cursor_opens && !has_result_memory() && vocr_allocs == 1);
    close_overlay(); mem_free();
}

static void test_character_open_reuses_background_text(void)
{
    cursor_saved_text();
    uint32_t generation = local_text_generation;
    open_overlay();
    TEST_ASSERT(job_running && job_cursor_only);
    TEST_CHECK(!cursor_opens && vocr_allocs == 1);
    finish_current_job();
    check_current_definition("cat", 0);
    TEST_CHECK(local_text_generation == generation && vocr_allocs == 1);
    TEST_CHECK(results[0].size == 64 * 1024 && !results[1].size);

    /* Opening between recognition and completion starts only point lookup.
     * The real mutex stubs reject recursively reacquiring cmd_lock. */
    cursor_setup(); start_job("background still finishing", 0);
    TEST_CHECK(run_job(&results[0], &cache_data[0]) == VJO_OK);
    open_overlay(); TEST_CHECK(cursor_waiting && job_running && !job_cursor_only);
    on_job_done(); TEST_ASSERT(job_running && job_cursor_only);
    finish_current_job(); check_current_definition("cat", 0);
    TEST_CHECK(vocr_allocs == 1);
}

static void test_word_background_lookup_and_reopen_cache(void)
{
    cursor_setup();
    TEST_CHECK(want_lookup());
    cfg.ocr_mode = loaded.ocr_mode = VJO_OCR_ON_PRESS;
    TEST_CHECK(!want_lookup());
    cfg.ocr_mode = loaded.ocr_mode = VJO_OCR_AUTO;
    stable_pending = 1; auto_prefetch();
    TEST_ASSERT(job_running && job_lookup && !job_cursor_only && ov == OV_CLOSED);
    finish_current_job();
    TEST_CHECK(cache_ok && active == 0 && !g_view.open && !g_view.list);
    TEST_CHECK(results[0].size == 64 * 1024 && !results[1].size);
    unsigned reads_before = cursor_reads, inferences = vocr_allocs;
    uint32_t generation = local_text_generation;
    open_overlay();
    TEST_CHECK(!job_running && cursor_reads == reads_before && vocr_allocs == inferences);
    check_current_definition("cat", 0);
    close_overlay(); open_overlay();
    TEST_CHECK(!job_running && cursor_reads == reads_before && local_text_generation == generation);
    check_current_definition("cat", 0);
    /* A cursor posted before its job starts must exclude the old cache. */
    vjo_post_cursor(3, generation);
    close_overlay(); open_overlay();
    TEST_ASSERT(job_running && job_cursor_only && !g_view.list->n_entries);
    finish_current_job(); check_current_definition("dog", 3);
    close_overlay();
    state.scene = state.region_scene = 8; state.region_seq++;
    open_overlay();
    TEST_ASSERT(job_running && !job_cursor_only && !g_view.list);
    finish_current_job(); check_current_definition("cat", 0);
    TEST_CHECK(local_text_generation != generation && vocr_allocs == inferences + 1);
    close_overlay(); mem_free();

    cursor_setup(); stable_pending = 1; auto_prefetch();
    TEST_ASSERT(job_running && job_lookup);
    TEST_CHECK(run_job(&results[0], &cache_data[0]) == VJO_OK);
    reads_before = cursor_reads;
    open_overlay(); /* lookup finished, completion has not been processed */
    TEST_CHECK(job_running && !cursor_waiting && !job_cursor_only);
    job_done = 1; on_job_done();
    TEST_CHECK(!job_running && cursor_reads == reads_before && vocr_allocs == 1);
    check_current_definition("cat", 0);
    close_overlay(); mem_free();
}
static void test_character_coalesces_and_drops_inflight_definition(void)
{
    cursor_saved_text(); open_overlay(); finish_current_job();
    uint32_t generation = local_text_generation;
    unsigned reads_before = cursor_reads;
    vjo_post_cursor(3, generation); vjo_post_cursor(6, generation); vjo_post_cursor(0, generation);
    TEST_CHECK(cursor_reads == reads_before && pending_cmd == VJO_CMD_CURSOR);
    TEST_CHECK(g_view.cursor_byte == 0 && !g_view.list->n_entries);
    on_command(); TEST_ASSERT(job_running && job_cursor_byte == 0);
    finish_current_job(); check_current_definition("cat", 0);

    vjo_post_cursor(3, generation); on_command();
    read_action = 1; /* a later move arrives at an actual file-read boundary */
    finish_current_job();
    TEST_ASSERT(job_running && job_cursor_only && job_cursor_byte == 6);
    TEST_CHECK(g_view.cursor_byte == 6 && !g_view.list->n_entries);
    finish_current_job(); check_current_definition("bird", 6);
    TEST_CHECK(local_text_generation == generation && vocr_allocs == 1 && !jpeg_captures);
}
static void test_character_close_reopen_and_retirement(void)
{
    cursor_saved_text(); open_overlay(); finish_current_job();
    uint32_t generation = local_text_generation;
    vjo_post_cursor(3, generation); on_command(); read_action = 3;
    finish_current_job();
    TEST_ASSERT(job_running && job_cursor_only && !job_cancelled && ov == OV_OPEN);
    TEST_CHECK(!g_view.list->n_entries);
    finish_current_job(); check_current_definition("dog", 3);
    TEST_CHECK(vocr_allocs == 1 && local_text_generation == generation);

    vjo_post_cursor(6, generation); on_command(); read_action = 2;
    finish_current_job();
    TEST_CHECK(ov == OV_CLOSED && !job_running && !g_view.list);
    unsigned n = cursor_opens;
    vjo_post_cursor(0, generation); on_command();
    TEST_CHECK(!job_running && cursor_opens == n && !g_view.list);
    open_overlay(); finish_current_job();
    vjo_post_cursor(3, generation); on_command(); read_action = 4;
    finish_current_job();
    TEST_CHECK(!local_text_valid && !g_view.list && !job_running);
    TEST_CHECK(game_generation != job_generation && local_text_generation != generation);
    TEST_CHECK(vocr_allocs == 1 && retirement_checks > 0);
}
static void test_character_scene_and_region_invalidate_saved_text(void)
{
    for (int during_read = 0; during_read < 2; during_read++) {
        cursor_saved_text(); open_overlay(); finish_current_job();
        uint32_t generation = local_text_generation;
        vjo_post_cursor(3, generation);
        if (!during_read) { state.scene = state.region_scene = 8; state.region_seq++; }
        on_command();
        if (during_read) { read_action = 5; finish_current_job(); }
        TEST_CHECK(!local_text_valid && !job_running && !g_view.list);
        TEST_CHECK(local_text_generation != generation && g_view.status_is_error);
        TEST_CHECK(strstr(g_view.status, "dialogue changed") != NULL && vocr_allocs == 1);
        close_overlay(); open_overlay();
        TEST_CHECK(job_running && !job_cursor_only && !g_view.list);
        job_running = 0;
    }
    for (int cmd = VJO_CMD_SET_REGION; cmd <= VJO_CMD_CLEAR_REGION; cmd++) {
        cursor_saved_text(); open_overlay(); finish_current_job();
        uint32_t generation = local_text_generation;
        VjoRect r = {2, 4, 100, 50};
        vjo_post_command(cmd, &r); on_command();
        /* Kernel may retain scene ID after a region reset. */
        TEST_CHECK(state.scene == 2 && !local_text_valid && ov == OV_CLOSED);
        TEST_CHECK(local_text_generation != generation && !g_view.list);
        open_overlay(); TEST_CHECK(job_running && !job_cursor_only && !g_view.list);
        job_running = 0;
    }
}
static void test_character_command_priority_and_generation_boundaries(void)
{
    cursor_saved_text(); open_overlay(); finish_current_job();
    uint32_t generation = local_text_generation;
    unsigned n = cursor_opens;
    vjo_post_cursor(1, generation); vjo_post_cursor(9, generation);
    vjo_post_cursor(0, generation + 1);
    TEST_CHECK(pending_cmd == VJO_CMD_NONE && !job_running && cursor_opens == n);
    const int commands[] = {VJO_CMD_CLOSED, VJO_CMD_SET_REGION, VJO_CMD_CLEAR_REGION};
    for (size_t i = 0; i < sizeof(commands)/sizeof(commands[0]); i++) {
        vjo_post_command(commands[i], NULL);
        vjo_post_cursor(3, generation);
        TEST_CHECK(pending_cmd == commands[i] && g_view.cursor_byte == 0);
        pending_cmd = VJO_CMD_NONE;
    }
    local_cursor_seq = UINT32_MAX;
    vjo_post_cursor(3, generation); TEST_CHECK(local_cursor_seq == 1);
    on_command(); finish_current_job(); check_current_definition("dog", 3);
    local_text_generation = UINT32_MAX;
    retire_local_text(1);
    TEST_CHECK(local_text_generation == 1 && !local_text_valid && !g_view.list);
    vjo_post_cursor(0, generation); TEST_CHECK(pending_cmd == VJO_CMD_NONE);
    TEST_CHECK(retirement_checks > 0 && !held_cmd && !held_view);
}

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
    cursor_setup(); /* This allocation gate needs an actual selectable sentence. */
    cfg.ocr_backend = VJO_OCR_VOCR;
    phases_checked = 1;
    ov = OV_OPEN;
    start_job("local lookup", 1);
    TEST_CHECK(job_idx == 0 && !g_view.list);
    TEST_CHECK(run_job(&results[job_idx], &cache_data[job_idx]) == VJO_OK);
    TEST_CHECK(has_result_memory() && results[0].size == 64 * 1024);
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
    {"japanese_only_selects_first_letter_rejects_other_scripts_and_reloads_choices", test_japanese_only_selection_and_config_reload},
    {"no_Japanese_text_allocates_no_results_and_performs_no_dictionary_IO", test_no_japanese_selection_skips_dictionary_memory_and_io},
    {"character_open_reuses_background_OCR_without_dictionary_or_capture_work", test_character_open_reuses_background_text},
    {"text_save_uses_existing_worker_and_preserves_note_across_game_exit", test_text_save_uses_existing_worker_and_survives_exit},
    {"text_save_rejects_disabled_stale_busy_requests_and_preserves_close_priority", test_text_save_rejects_disabled_stale_busy_requests},
    {"text_save_failure_preserves_definition_and_allows_retry_without_new_memory", test_text_save_failure_keeps_definition_and_retry},
    {"word_Auto_prepares_first_match_and_reopen_reuses_only_current_cache", test_word_background_lookup_and_reopen_cache},
    {"character_moves_coalesce_and_reject_old_inflight_definition", test_character_coalesces_and_drops_inflight_definition},
    {"character_close_reopen_restarts_lookup_and_retirement_rejects_old_results", test_character_close_reopen_and_retirement},
    {"character_scene_and_region_changes_retire_saved_text", test_character_scene_and_region_invalidate_saved_text},
    {"character_commands_preserve_close_region_priority_and_nonzero_generations", test_character_command_priority_and_generation_boundaries},
    {"Shell_result_OOM_preserves_OCR_sentence_and_dictionary_backoff", test_shell_result_oom_preserves_recognition},
    {"Shell_result_OOM_preserves_maximum_filtered_worker_text", test_shell_result_oom_keeps_max_worker_text},
    {"local_lookup_uses_one_arena_and_network_keeps_two", test_local_lookup_single_arena_and_network_double_buffers},
    {"local_ocr_and_dictionary_phases_repeat_and_recover", test_local_phases_and_recovery},
    {"local_switch_waits_for_warm_pool_and_restores_network_memory", test_local_switch_drains_warm_pool},
#endif
    {"vocr_never_uploads_to_lens", test_vocr_never_uses_lens},
    {"meiki_is_explicit_manual_local_character_pipeline", test_meiki_manual_local_policy},
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
