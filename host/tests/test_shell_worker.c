/* The actual Shell worker (shell/worker.c) with only Vita scheduling and memory stubbed. */
#include "acutest.h"
#include <stdarg.h>
#include <stdio.h>
#include "client.h"
#include "net_posix.h"
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
static unsigned vocr_allocs, vocr_frees, vocr_paf_allocs;
static int fail_vocr_user, fail_vocr_paf;
static int phases_checked, vocr_live, fail_join;
static unsigned char raw_region[200 * 60 * 4];
static VjoState raw_state;
static int has_result_memory(void);
static int held_cmd, held_view, retirement_checks;
/* Set by the refresh tests: whenever the view lock is released, an open local
 * view that shows local_text shows it with its own generation (a replaced
 * text never appears with the old one's entry or cursor). */
static int view_invariant, view_invariant_checks;
static uint32_t invariant_generation;
static unsigned invariant_seq;
uint64_t sceKernelGetProcessTimeWide(void) { return test_now; }
/* The log's clock (kernel.txt's): system time, here 3 s ahead of SceShell's process time. */
uint64_t sceKernelGetSystemTimeWide(void) { return test_now + 3000000; }
int sceKernelWaitThreadEnd(SceUID uid, int *status, void *timeout) { return fail_join ? -1 : 0; }
int sceKernelSetEventFlag(SceUID id, unsigned bits) { posted |= bits; return 0; }
int vjoGetState(VjoState *out) { *out = state; return 0; }
static VjoRect saved_region; /* w != 0: a saved region for every game */
static int regions_set, last_region_full;
int vjoSetRegion(const VjoRect *r) { regions_set++; last_region_full = r == NULL; return 0; }
int vjoSetTriggers(int a, int b) { return 0; }
/* The order of a game start's steps, one letter each: A vjoSetGameActive (a game), C a config load, S a
 * stamp read (the Overlay app check). */
static char start_order[64];
static void start_step(char c) { size_t n = strlen(start_order); if (n + 1 < sizeof(start_order)) start_order[n] = c, start_order[n + 1] = 0; }
static int active_mode = -1; /* the last vjoSetGameActive's */
int vjoSetGameActive(int pid, int mode) { active_mode = mode; if (mode != VJO_GAME_NONE) start_step('A'); return 0; }
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

#include "../../shell/worker.c"
#include "../../shell/ledger.c" /* the ledger worker.c counts in */
#include "../../shell/strip_view.c" /* the strip view model worker.c publishes through */

/* audit-overlay-shell-slim-1 A5: the overlay's thread posts a cursor move at the
 * n-th cmd_lock acquisition from now (vjo_post_cursor runs on Paf's thread). */
static int race_on_lock;
static void post_local_lookup(uint32_t generation);
static void cursor_race(void) { local_cursor_byte = 3; post_local_lookup(local_text_generation); }

int sceKernelLockMutex(SceUID uid, int count, void *timeout)
{
    if (uid == 81) { TEST_CHECK(!held_view); held_view++; }
    if (uid == 82) { TEST_CHECK(!held_cmd && !held_view); held_cmd++;
        if (race_on_lock && --race_on_lock == 0) cursor_race(); }
    return 0;
}
int sceKernelUnlockMutex(SceUID uid, int count)
{
    if (uid == 81 && view_invariant && g_view.open && g_view.local_cursor_mode && g_view.list &&
        g_view.list->header == local_text && local_text_valid) {
        view_invariant_checks++;
        TEST_CHECK_(g_view.text_generation == local_text_generation && g_view.cursor_byte == local_cursor_byte &&
                    (g_view.list == &local_header || g_view.list == &cache_data[0].list),
                    "view generation %u, text %u", g_view.text_generation, local_text_generation);
        /* an entry shown is the current lookup's (never the replaced text's) */
        TEST_CHECK(g_view.list != &cache_data[0].list || cache_ok || !g_view.list->n_entries);
        /* a new text is published with a new list_seq: an Anki save asked for the old list fails */
        TEST_CHECK_(g_view.text_generation == invariant_generation || g_view.list_seq != invariant_seq,
                    "text %u shown under the old list_seq %u", g_view.text_generation, g_view.list_seq);
        invariant_generation = g_view.text_generation;
        invariant_seq = g_view.list_seq;
    }
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
void *vjo_shell_heap_alloc_for(int item, size_t size)
{
    if (item == VJO_SL_PAF_RESULTS) {
        void *p = vjo_shell_heap_alloc(size);
        if (p) vjo_ledger_add(&g_shell_ledger, item, (uint32_t)size);
        return p;
    }
    vocr_paf_allocs++;
    TEST_CHECK(0); /* game vocr must never borrow the Shell Paf heap */
    return NULL;
}
void vjo_shell_heap_free(int item, void *p, size_t bytes)
{
    TEST_CHECK(p == storage || p == vocr_storage);
    if (p == vocr_storage) { vocr_frees++; vocr_live = 0; } else heap_frees++;
    vjo_ledger_sub(&g_shell_ledger, item, (uint32_t)bytes);
}
uint32_t vjo_shell_heap_free_size(void) { return 2557952; }
void vjo_config_load(VjoConfig *out, VjoArena *a) { start_step('C'); *out = loaded; }
void vjo_log_configure(const VjoConfig *c) {}
/* The last log lines, for the game OCR measurement tests. */
static char log_lines[32][256];
static unsigned log_count;
/* Every config warning line (the last 16) and the Overlay app check's lines, counted since reset. */
static char warning_logs[16][128], last_app_log[256];
static unsigned config_warning_logs, app_logs;
static unsigned warnings_logged(const char *text)
{
    unsigned n = 0;
    for (unsigned i = 0; i < config_warning_logs && i < 16; i++)
        n += !strcmp(warning_logs[i], text);
    return n;
}
/* The logger's batches (shell/log_vita.c vjo_log_batch_begin/end), by its own
 * rules: a line outside a batch is one append of log.txt; a batch's lines go
 * out together when it ends, or when the next one would pass its
 * VJO_LOG_BATCH_LINES lines or its buffer (host/tests/test_shell_log.c shows
 * the real logger writes exactly these appends, byte for byte as line by line). */
static VjoLogBatch *open_batch;
static int batch_lines_held, batch_bytes_held, batch_cap;
static unsigned log_appends, batched_lines;
int vjo_log_batch_begin(VjoLogBatch *b, char *buf, int cap)
{
    (void)buf;
    if (open_batch || !b || cap <= 0) return 0;
    open_batch = b; batch_cap = cap; batch_lines_held = batch_bytes_held = 0;
    return 1;
}
void vjo_log_batch_end(VjoLogBatch *b)
{
    if (open_batch != b) return;
    if (batch_lines_held) log_appends++;
    open_batch = NULL;
}
void vjo_log(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(log_lines[log_count % 32], sizeof(log_lines[0]), fmt, ap);
    va_end(ap);
    if (!strncmp(log_lines[log_count % 32], "config warning: ", 16))
        snprintf(warning_logs[config_warning_logs++ % 16], sizeof(warning_logs[0]), "%s", log_lines[log_count % 32] + 16);
    if (!strncmp(log_lines[log_count % 32], "overlay app ", 12)) {
        app_logs++;
        snprintf(last_app_log, sizeof(last_app_log), "%s", log_lines[log_count % 32]);
    }
    log_count++;
    n = (n > 254 ? 254 : n) + 1; /* the line and its '\n', as vjo_log writes it */
    if (!open_batch) { log_appends++; return; }
    batched_lines++;
    if (batch_lines_held < VJO_LOG_BATCH_LINES && batch_cap - batch_bytes_held >= n) {
        batch_lines_held++; batch_bytes_held += n;
        return;
    }
    if (batch_lines_held) log_appends++; /* the batch so far goes out first */
    batch_lines_held = 1; batch_bytes_held = n;
}
static const char *logged(const char *needle)
{
    for (unsigned i = log_count > 32 ? log_count - 32 : 0; i < log_count; i++)
        if (strstr(log_lines[i % 32], needle)) return log_lines[i % 32];
    return NULL;
}
/* A debug-build line (docs/release-variants.md): the debug build logs it, the
 * normal build never does (vjo_dlog). */
#if VJO_DEBUG
#define DLOGGED(needle) (logged(needle) != NULL)
#else
#define DLOGGED(needle) (logged(needle) == NULL)
#endif
int vjo_region_load(const char *id, VjoRect *out, VjoArena *a) { if (!saved_region.w) return 0; *out = saved_region; return VJO_REGION_GAME; }
int vjo_region_save(const char *id, const VjoRect *r, VjoArena *a) { return 0; }
/* The Overlay app's stamps and its eboot.bin (shell/files.c), scripted (core/app_stamp.h). */
static char app_installed[64], app_record[64];
static int app_installed_rc, app_record_rc, app_eboot;
int vjo_file_line(const char *path, char *buf, int cap)
{
    int installed = !strcmp(path, VJO_APP_STAMP_INSTALLED);
    TEST_CHECK_(installed || !strcmp(path, VJO_APP_STAMP_PACKAGE), "read %s", path);
    start_step('S');
    int rc = installed ? app_installed_rc : app_record_rc;
    snprintf(buf, (size_t)cap, "%s", rc >= 0 ? (installed ? app_installed : app_record) : "");
    return rc;
}
int vjo_file_exists(const char *path)
{
    TEST_CHECK_(!strcmp(path, VJO_APP_EBOOT_INSTALLED), "stat %s", path);
    return app_eboot;
}
static int anki_configured, anki_reaps, anki_stop_rc, anki_counts;
/* The self-test's interface (shell/selftest.c writes the report): the stages
 * one job runs, recorded here. */
static uint32_t selftest_flags_seen[8], selftest_speed_seen[8]; /* every job's request (vjoOcrSubmit) */
#if VJO_DEBUG /* the normal build has no self-test: shell.h's stubs, worker.c's own vjo_selftest_init */
static int selftest_armed, selftest_games, selftest_steps, selftest_done;
static unsigned selftest_begins, selftest_ends, selftest_stage_count;
static int selftest_rcs[8];
static VjoRect selftest_region;
/* shell/selftest.c's order: the last stage's result is the job's */
static const char *const selftest_names[] = {"fallback", "textbox", "fullscreen", "probe", "textbox2"};
static const uint32_t selftest_stage_flags_tab[] = {VJO_GAME_OCR_F_FORCE_FALLBACK, 0, VJO_GAME_OCR_F_FULL,
                                                    VJO_GAME_OCR_F_PROBE, 0};
int vjo_selftest_armed(void) { return selftest_armed && !selftest_done; }
void vjo_selftest_init(const VjoConfig *cfg) {}
void vjo_selftest_step(const char *step) { selftest_steps++; }
void vjo_selftest_game(const char *title, int enabled, int mode, const VjoRect *region)
{ selftest_games++; if (region) selftest_region = *region; }
unsigned vjo_selftest_stages(void) { return vjo_selftest_armed() ? selftest_stage_count : 0; }
const char *vjo_selftest_stage_name(unsigned i) { return i < 5 ? selftest_names[i] : "?"; }
uint32_t vjo_selftest_stage_flags(unsigned i) { return i < 5 ? selftest_stage_flags_tab[i] : 0; }
static uint32_t selftest_begin_speed[8];
void vjo_selftest_stage_begin(unsigned i, const char *model, const char *fallback, uint32_t speed)
{ if (i < 8) selftest_begin_speed[i] = speed; selftest_begins++; }
/* shell/selftest.c's rule: no stage uses the line cache, textbox2 (stage 4) runs without helpers */
uint32_t vjo_selftest_stage_speed(unsigned i, uint32_t configured)
{ return (i == 4 ? configured & VJO_GAME_OCR_S_BOOST : configured) | VJO_GAME_OCR_S_NOCACHE; }
/* shell/selftest.c's rule (its report lines are host/tests/selftest_writer.c's). */
static int selftest_waits;
int vjo_selftest_stage_wait(unsigned i, int rc, const VjoGameOcrResult *r)
{
    if (!vjo_selftest_armed() || i || !((rc == VJO_OK && !r->lines) || rc == VJO_E_SCREEN_CHANGED) ||
        selftest_waits >= VJO_SELFTEST_WAITS)
        return 0;
    selftest_waits++;
    return 1;
}
void vjo_selftest_stage_end(unsigned i, int rc, const VjoGameOcrResult *r, uint32_t ms, const char *model)
{ if (selftest_ends < 8) selftest_rcs[selftest_ends] = rc; selftest_ends++; }
void vjo_selftest_done(void) { selftest_done = 1; }
int vjo_gstep_format(char *buf, size_t cap, const char *who, const VjoGameOcrStep *st)
{ return snprintf(buf, cap, "gstep %s %u rc=0x%08X", who, (unsigned)st->id, (unsigned)st->rc); }
#else
static int selftest_waits;
/* The normal build's vjo_selftest_init reads whether the marker is there (sceIoGetstat). */
static int selftest_marker, selftest_marker_reads;
int sceIoGetstat(const char *path, SceIoStat *st)
{
    selftest_marker_reads++;
    TEST_CHECK(!strcmp(path, "ux0:data/VitaJPOverlay/selftest.txt"));
    if (st) memset(st, 0, sizeof(*st));
    return selftest_marker ? 0 : (int)0x80010002;
}
#endif
static int guard_marks[8], guard_mark_rc;
int vjo_guard_mark(int stage) { if (stage >= 0 && stage < 8) guard_marks[stage]++; return guard_mark_rc; }
int vjo_anki_start(void) { return 0; }
int vjo_anki_stop(void) { return anki_stop_rc; }
int vjo_anki_configure(const VjoConfig *c) { anki_configured++; return c->anki_mining; }
void vjo_anki_reap(void) { anki_reaps++; }
void vjo_anki_post_count(void) { anki_counts++; }
static char anki_game[128], anki_tid[16], anki_model[64];
static int anki_psp;
void vjo_anki_set_game(const char *tid, const char *name, int psp)
{
    snprintf(anki_game, sizeof(anki_game), "%s", name);
    snprintf(anki_tid, sizeof(anki_tid), "%s", tid);
    anki_psp = psp;
}
void vjo_anki_set_ocr_model(const char *model) { snprintf(anki_model, sizeof(anki_model), "%s", model); }
void vjo_title_name(SceUID pid, const char *tid, char *name, int size) { snprintf(name, size, "Title of %s", tid); }
void vjo_platform_vita(VjoPlatform *p, VjoNetCancel *c) {}
int vjo_net_cancel_init(VjoNetCancel *c) { return 0; }
void vjo_net_cancel(VjoNetCancel *c) { c->cancelled = 1; cancels++; }
void vjo_net_cancel_clear(VjoNetCancel *c) { c->cancelled = 0; }
/* shell/game.c (its own suite, shell_game): not readable unless a test says. */
static int identify_rc = -1, enabled_psp = -1;
static VjoGameId identified;
int vjo_game_identify(SceUID pid, VjoGameId *out)
{ /* as shell/game.c: all zero when the title is not readable */
    if (!identify_rc) *out = identified; else memset(out, 0, sizeof(*out));
    return identify_rc;
}
int vjo_game_enabled(const char *id, int psp, VjoArena *a) { enabled_psp = psp; return game_enabled; }
static void activate(SceUID pid, const char *tid, int mode)
{
    VjoGameId id;
    memset(&id, 0, sizeof(id));
    snprintf(id.key, sizeof(id.key), "%s", tid);
    id.mode = mode;
    activate_game(pid, &id);
}

int sceKernelDelayThread(unsigned us) { test_now += us; return 0; }
#ifdef VJO_WITH_VOCR
#include "../../core/vocr_ocr.h"
#include "../../core/textnorm.h"
static VjoGameOcrRequest fake_game_request;
static VjoGameOcrResult fake_game_result;
static int32_t fake_model_io_error;
static const char *fake_recognized_text;
static unsigned fake_lines;
static int32_t fake_free_user;
static uint32_t fake_need_weights, fake_reserve;
static const char *fake_missing_model;
static int fake_memory_fallback, fake_missing_with_fallback, fake_cancel_on_model_error;
static int32_t fake_missing_io;
static unsigned submits;
static int fake_screen_changes; /* the next reads end with VJO_E_SCREEN_CHANGED */
static uint16_t fake_line_flags; /* VjoGameOcrLine.flags of the fake lines (0: 0x3FF, read greedily) */
static uint32_t fake_cascade_frames, fake_cascade_us; /* a CASCADE step with W20's frames and time (0: none) */
static int fake_steps; /* the steps of a resident job on main's defaults (game_ocr/main.c), as the worker reports */
/* A clock step (game_ocr/main.c job_busy): the ARM clock before, while and after, and the boost's rc. */
static int fake_clock_on, fake_clock_rc;
static uint32_t fake_clock_mhz[3];
/* API 21: tiers whose memory check the fake worker refuses (bit t - 1), in a
 * request by tiers: it logs a CHECK step per tier tried, largest first, as the
 * worker does, and runs the first one it does not refuse (all refused: OOM). */
static unsigned fake_tier_checks;
static unsigned fake_top(unsigned tiers)
{
    unsigned top = 0;
    for (unsigned t = 1; t <= VJO_VOCR_TIERS; t++) if (tiers & (1u << (t - 1u))) top = t;
    return top;
}
static void fake_check(unsigned tier, unsigned top, int refused)
{
    VjoGameOcrStep *s = &fake_game_result.step[fake_game_result.n_steps++];
    memset(s, 0, sizeof(*s));
    s->id = VJO_GSTEP_CHECK;
    s->kind = (uint8_t)(tier << 4);
    s->arg = tier != top;
    s->rc = refused;
    s->bytes = 1000000u * tier; /* a made-up need: workspace and weights */
    s->sum = 4096u * tier;      /* own resident blocks it would release first */
}
int vjoOcrSubmit(const VjoGameOcrRequest *r)
{
    TEST_CHECK(!open_batch); /* no log batch is held across the game's exchange */
    fake_game_request = *r;
    TEST_CHECK(!vocr_live && !(r->flags & ~VJO_GAME_OCR_F_ALL));
    if (submits < 8) selftest_flags_seen[submits] = r->flags, selftest_speed_seen[submits] = r->speed;
    submits++;
    fake_game_request.seq = 12;
    fake_game_request.width = raw_state.width; fake_game_request.height = raw_state.height;
    /* The game reads its first pass: the kernel publishes the capture (API 19). */
    state.done_seq++;
    memset(&fake_game_result, 0, sizeof(fake_game_result));
    fake_game_result.size = sizeof(fake_game_result); fake_game_result.seq = 12;
    return 12;
}
/* fake_republish: the job's first two reads are still pending; the kernel
 * meanwhile voids the job's first capture (region_seq 0: the screen changed,
 * the worker waits and reads again) and then publishes the retry's (scene 3,
 * capture 5), as API 20's worker does. */
static int fake_republish, republish_reads, captured_while_void;
/* shell/log_vita.c's counter of file appends and their time: each read 3 ms and 2
 * appends on from the last, as if a batch went out to both files meanwhile. */
static unsigned log_io_calls;
uint32_t vjo_log_io(uint32_t *appends) { log_io_calls++; *appends = 2u * log_io_calls; return 3000u * log_io_calls; }
static uint32_t fake_exchange_us; /* the game worker's time for a job, as the Shell's read waits for it */
int vjoOcrRead(uint32_t seq, VjoGameOcrResult *out)
{
    TEST_CHECK(seq == 12);
    test_now += fake_exchange_us;
    TEST_CHECK(!open_batch);
    if (fake_republish && republish_reads < 2) {
        if (republish_reads++ == 0) state.region_seq = 0;
        else { captured_while_void = job_captured; state.capture_scene = state.scene = state.region_scene = 3; state.region_seq = 5; }
        return 1;
    }
    vocr_allocs++;
    if (phases_checked) TEST_CHECK(!has_result_memory());
    if (fake_screen_changes > 0) {
        /* The real worker's result after VJO_VOCR_TRIES reads of a changing region. */
        fake_screen_changes--;
        fake_game_result.rc = VJO_E_SCREEN_CHANGED;
        fake_game_result.text[0] = 0;
        fake_game_result.tears = 3;
        vocr_frees++; /* the worker freed the job's workspace before answering */
        *out = fake_game_result;
        return 0;
    }
    /* The model the job runs (API 21): the request's largest tier less the tiers the
     * worker stepped down (its memory check), else the request's model by name. */
    unsigned top = fake_top(fake_game_request.tiers), ran = top;
    if (fake_tier_checks && top) {
        for (unsigned t = top; t >= 1u; t--) {
            int refused;
            if (!(fake_game_request.tiers & (1u << (t - 1u)))) continue;
            refused = (int)((fake_tier_checks >> (t - 1u)) & 1u);
            fake_check(t, top, refused);
            ran = t;
            if (!refused) break;
        }
        fake_game_result.fallback = top - ran;
        if ((fake_tier_checks >> (ran - 1u)) & 1u) { /* every tier refused: the worker's OOM */
            fake_game_result.rc = VJO_E_OOM;
            fake_game_result.free_user = fake_free_user; fake_game_result.reserve_bytes = 4194304;
            fake_game_result.need_workspace = 245760; fake_game_result.need_weights = 659456;
            *out = fake_game_result;
            return 0;
        }
    } else if (fake_memory_fallback && top > 1u) ran = top - 1u;
    const char *ran_model = top ? vjo_vocr_tier_model((int)ran) : fake_game_request.model;
    if (fake_missing_model && !strcmp(ran_model, fake_missing_model)) {
        fake_game_result.rc = VJO_E_OCR_MODEL; /* as the worker reports an absent file */
        fake_game_result.io_error = fake_missing_io;
        fake_game_result.fallback = top ? top - ran : (uint32_t)fake_missing_with_fallback; /* the tier that failed */
        if (fake_cancel_on_model_error) job_cancelled = 1;
        *out = fake_game_result;
        return 0;
    }
    if (fake_memory_fallback) {
        /* The real worker's report when H15 did not fit but FL10 did (FL10 resident). */
        fake_game_result.fallback = 1;
        fake_game_result.model_need_workspace = 303104; fake_game_result.model_need_weights = 1159168;
        fake_game_result.model_release_bytes = 659456;
    }
    if (fail_vocr_user) {
        /* The real worker reports its refused check (game_ocr/main.c). */
        fake_game_result.rc = VJO_E_OOM;
        fake_game_result.free_user = fake_free_user;
        fake_game_result.need_workspace = 245760; fake_game_result.need_weights = fake_need_weights;
        fake_game_result.reserve_bytes = fake_reserve;
        fake_game_result.workspace_bytes = 244896;
    }
    else {
        size_t bytes = vjo_vocr_workspace_bytes(ran_model, raw_state.width, raw_state.height);
        static unsigned char capture[64 * 1024];
        VjoPlatform game_platform = {0}; VjoVocrStats stats;
        VjoRgbaImage rgba = {raw_region, raw_state.width, raw_state.height, raw_state.raw_stride};
        VjoGreySource im;
        vjo_grey_source_rgba(&rgba, &im);
        vocr_live = 1;
        fake_game_result.rc = vjo_vocr_ocr(&game_platform, fake_game_request.model_dir,
            ran_model, &im, vocr_storage, bytes, capture,
            vjo_vocr_capture_bytes(raw_state.width, raw_state.height),
            fake_game_result.text, sizeof(fake_game_result.text), &stats);
        vocr_live = 0; vocr_frees++;
    }
    if (fake_game_result.rc == VJO_E_OCR_MODEL) fake_game_result.io_error = fake_model_io_error;
    /* A supplied successful worker response tests Shell publication, not OCR accuracy. */
    if (fake_game_result.rc == VJO_OK && fake_recognized_text) {
        snprintf(fake_game_result.text, sizeof(fake_game_result.text), "%s", fake_recognized_text);
        /* as the worker returns it (API 20): normalised, its own text kept after the NUL */
        vjo_text_normalize_keep(fake_game_result.text, sizeof(fake_game_result.text), NULL, NULL);
        /* A supplied measurement, as the real worker copies VjoVocrStats. */
        fake_game_result.free_user = fake_memory_fallback ? 5300000 : 91226112;
        fake_game_result.reserve_bytes = 4194304;
        fake_game_result.need_workspace = 245760; fake_game_result.need_weights = 0;
        fake_game_result.recognize_us = 92345; fake_game_result.frames = 205;
        fake_game_result.lines = fake_lines;
        fake_game_result.recognized = fake_lines;
        for (unsigned i = 0; i < fake_lines; i++)
            fake_game_result.line[i] = (VjoGameOcrLine){(uint16_t)(917 - i), 30, (uint16_t)(114 - i),
                                                        fake_line_flags ? fake_line_flags : 0x3FFu, 52100u + i};
        if (fake_steps) {
            static const uint8_t ids[] = {VJO_GSTEP_MEM, VJO_GSTEP_CHECK, VJO_GSTEP_ALLOC, VJO_GSTEP_ALLOC,
                                          VJO_GSTEP_PASS, VJO_GSTEP_SIG, VJO_GSTEP_FIND, VJO_GSTEP_SETTLE,
                                          VJO_GSTEP_LOAD, VJO_GSTEP_RECOGNIZE, VJO_GSTEP_LM, VJO_GSTEP_NORM,
                                          VJO_GSTEP_FREE, VJO_GSTEP_FREE};
            for (unsigned k = 0; k < sizeof(ids); k++) {
                VjoGameOcrStep *s = &fake_game_result.step[fake_game_result.n_steps++];
                memset(s, 0, sizeof(*s));
                s->id = ids[k];
                s->kind = ids[k] == VJO_GSTEP_ALLOC || ids[k] == VJO_GSTEP_FREE ? VJO_GAME_OCR_MEM_USER : 0;
                s->arg = ids[k] == VJO_GSTEP_ALLOC || ids[k] == VJO_GSTEP_FREE ? VJO_GITEM_CAPTURE + (k & 1) : 1;
                s->bytes = 57344; s->free_main = 90177536; s->free_cdram = 12582912; s->free_phycont = 27262976;
                s->us = 1000u * k + 17u;
            }
        }
        if (fake_clock_on) {
            VjoGameOcrStep *s = &fake_game_result.step[fake_game_result.n_steps++];
            memset(s, 0, sizeof(*s));
            s->id = VJO_GSTEP_CLOCK;
            s->arg = (uint16_t)fake_clock_mhz[0]; s->rc = fake_clock_rc;
            s->bytes = fake_clock_mhz[1]; s->sum = fake_clock_mhz[2];
        }
        if (fake_cascade_frames) {
            VjoGameOcrStep *s = &fake_game_result.step[fake_game_result.n_steps++];
            memset(s, 0, sizeof(*s));
            s->id = VJO_GSTEP_CASCADE;
            s->arg = 1;
            s->bytes = VJO_GLM_READ | 1u << 24 | fake_cascade_frames;
            s->us = fake_cascade_us + 1u;
        }
    }
    *out = fake_game_result;
    return 0;
}
int vjoOcrCancel(uint32_t seq) { return 0; }
#endif

static void reset(void)
{
    race_on_lock = 0;
    if (has_result_memory()) mem_free();
    memset(&g_view, 0, sizeof(g_view));
    view_lock = 81;
    cmd_lock = 82;
    held_cmd = held_view = retirement_checks = 0;
    local_text[0] = 0;
    local_text_valid = cursor_waiting = job_cursor_only = 0;
    local_text_generation = local_text_game_generation = 0;
    local_cursor_seq = local_cursor_byte = 0;
    job_cursor_byte = job_cursor_seq = job_text_generation = 0;
    local_page = job_page = pages_limit = 0;
    pages_generation = 0;
    pending_cmd = VJO_CMD_NONE;
    pending_cursor_byte = pending_cursor_generation = 0;
    memset(&state, 0, sizeof(state));
    memset(&plat, 0, sizeof(plat));
    vjo_config_defaults(&loaded);
    loaded.dictionary = VJO_DICT_LOCAL;
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
    vocr_allocs = vocr_frees = vocr_paf_allocs = 0;
    fail_vocr_user = fail_vocr_paf = 0;
#ifdef VJO_WITH_VOCR
    fake_model_io_error = 0;
    fake_recognized_text = NULL;
    fake_lines = 2;
    fake_line_flags = 0;
    fake_steps = 0;
    fake_clock_on = fake_clock_rc = 0;
    fake_cascade_frames = fake_cascade_us = 0;
    fake_free_user = 5000000;
    fake_need_weights = 659456;
    fake_reserve = 4194304;
    fake_missing_model = NULL;
    fake_memory_fallback = fake_missing_with_fallback = fake_cancel_on_model_error = 0;
    fake_missing_io = (int32_t)0x80010002u;
    vocr_unusable = 0; vocr_unusable_generation = 0;
    fake_tier_checks = 0;
    memset(job_title, 0, sizeof(job_title));
    log_count = 0;
    fake_screen_changes = 0;
#endif
    selftest_waits = 0;
    job_refresh = 0;
#if VJO_DEBUG
    job_selftest = 0;
#endif
#ifdef VJO_WITH_VOCR
    fake_exchange_us = 0;
#endif
    job_prepares = 1; /* run_vocr_job: an Auto job, the overlay closed */
    text_generation = 0;
    idle_since_us = 0;
    job_started_us = 0;
    refresh_pending = open_retry = 0;
    refresh_after_us = lookup_us = 0;
    screen_changed_streak = 0;
    screen_retry_until = 0;
    phases_checked = vocr_live = fail_join = 0;
    anki_configured = anki_reaps = anki_stop_rc = anki_counts = anki_on = 0;
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
    cache_ok = subtitles = stable_pending = job_running = job_cancelled = 0;
    view_waiting = view_job_text = 0;
    memset(&ocr_backoff, 0, sizeof(ocr_backoff));
    memset(&dict_backoff, 0, sizeof(dict_backoff));
    memset(cache_data, 0, sizeof(cache_data));
    app_state = -1;
    app_installed_rc = app_record_rc = (int)0x80010002u; /* neither stamp: a package from before them */
    app_eboot = 0;
    app_installed[0] = app_record[0] = 0;
    app_logs = config_warning_logs = 0;
    last_app_log[0] = 0;
}

static void test_local_open(void)
{
    reset();
    open_overlay();
    TEST_CHECK(job_running && job_lookup && input_blocked);
    TEST_CHECK(posted & NET_EV_JOB);
    TEST_CHECK(!g_view.status_is_error && want_lookup());
    /* Opening takes no memory: the job takes its one lookup arena after OCR
     * (in the network thread). */
    TEST_CHECK(!has_result_memory() && !user_allocs && !heap_allocs);
    TEST_ASSERT(mem_alloc() == 0); /* the job's lookup arena */
    TEST_CHECK(results[0].size == LOCAL_RESULT_ARENA_SIZE && !results[1].size);
    job_running = 0;
    mem_free();
    TEST_CHECK(!has_result_memory());
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
    /* the removed Hachidori relay's choice loads as local: the same pipeline, the cache kept */
    {
        const char *old = "dictionary = hachidori\nhachidori_host = 192.168.1.2\n"
                          "local_dictionary_dir = ux0:data/other\n"
                          "local_dictionaries = daijirin.vjdict,main.vjdict\n";
        VjoConfig c;
        vjo_config_defaults(&c);
        vjo_config_parse(&c, old, strlen(old));
        TEST_CHECK(c.dictionary == VJO_DICT_LOCAL && c.n_warnings == 1);
        cache_ok = 1;
        loaded = c;
        apply_config();
        TEST_CHECK(cache_ok && want_lookup());
    }
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

/* The removed Hachidori relay: a config.ini that chose it, with any hachidori_host (none, not a
 * host, a valid one), loads as local, and the overlay opens as with the local dictionaries: no
 * relay host message (the relay without a valid host once showed one, and no job started;
 * #117's audit N6, M27), a job starts. */
static void test_removed_relay_opens_as_local(void)
{
    static const char *const hosts[] = {"", "http://192.168.1.2", "192.168.1.2:19633"};
    for (unsigned i = 0; i < sizeof(hosts) / sizeof(hosts[0]); i++) {
        char ini[128];
        reset();
        snprintf(ini, sizeof(ini), "dictionary = hachidori\nhachidori_host = %s\n", hosts[i]);
        vjo_config_defaults(&loaded);
        vjo_config_parse(&loaded, ini, strlen(ini));
        TEST_CHECK(loaded.dictionary == VJO_DICT_LOCAL && loaded.n_warnings == 1);
        cfg = loaded;
        open_overlay();
        TEST_CHECK_(g_view.open && !g_view.status_is_error && !strstr(g_view.status, "achidori"),
                    "host '%s': %s", hosts[i], g_view.status);
        TEST_CHECK(job_running && job_lookup && (posted & NET_EV_JOB) && want_lookup());
        job_running = 0;
        close_overlay();
        mem_free();
    }
}

/* The lookup's result arena (taken after OCR): a refusal leaves nothing held
 * and the next allocation succeeds; a block whose base cannot be read is freed
 * again (the host build's memory-block path). A job's refused arena keeps its
 * recognized text: test_shell_result_oom_preserves_recognition. */
static void test_alloc_failure(void)
{
    reset();
    fail_alloc = 1;
    TEST_CHECK(mem_alloc() < 0 && !has_result_memory());
    fail_alloc = 0;
    TEST_CHECK(mem_alloc() == 0 && has_result_memory() && results[0].size == LOCAL_RESULT_ARENA_SIZE);
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
    /* Both removed features fall back to the local screenshot pipeline, each with a warning. */
    TEST_CHECK(c.text_source == VJO_SOURCE_OCR && c.dictionary == VJO_DICT_LOCAL);
    TEST_CHECK(c.n_warnings == 2);
}

static void test_changed_dictionary_drops_running_result(void)
{
    reset();
    TEST_ASSERT(mem_alloc() == 0);
    active = 0;
    cache_ok = 1;
    cache_data[0].filtered = "日本語";
    start_job("test", 1);
    ov = OV_OPEN_OLD_JOB;
    strcpy(loaded.local_dictionaries, "new.vjdict");
    apply_config();
    TEST_CHECK(job_cancelled && cancels == 1 && !cache_ok);
    on_job_done();
    TEST_CHECK(job_running && !cache_ok);
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
    cache_data[0].filtered = "日本語";
    cache_scene = state.scene;
    cache_seq = state.region_seq;
    start_job("old game", 1);
    TEST_ASSERT(mem_alloc() == 0); /* the job's lookup arena, taken after its OCR */
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
    TEST_CHECK(!has_result_memory());
}

static void test_game_start_rejects_previous_generation(void)
{
    reset();
    TEST_ASSERT(mem_alloc() == 0);
    active = 0;
    cache_ok = 1;
    cache_data[0].filtered = "日本語";
    cache_scene = state.scene;
    cache_seq = state.region_seq;
    start_job("old game", 1);
    cache_data[job_idx].sentence = "old game text";
    job_capture_scene = state.scene;
    job_capture_seq = state.region_seq;
    ov = OV_OPEN;
    input_blocked = 1;
    state.game_pid = 101;

    on_game_start(); /* a foreground switch need not include GAME_EXIT */
    TEST_CHECK(job_cancelled && cancels == 1 && !cache_ok);
    TEST_CHECK(ov == OV_CLOSED && !input_blocked && pending_pid == 101);
    activate(state.game_pid, "PCSG00202", VJO_GAME);
    open_overlay(); /* the kernel has not sampled a new scene ID yet */
    TEST_CHECK(ov == OV_OPEN_OLD_JOB && !g_view.list && !cache_ok);

    /* The generation check is independent of the socket cancellation flag. */
    job_cancelled = 0;
    job_text_ready = 1;
    on_job_text();
    TEST_CHECK(!g_view.list && !view_job_text);
    on_job_done();
    TEST_CHECK(job_running && job_generation == game_generation);
    TEST_CHECK(!cache_ok);
    TEST_CHECK(!g_view.list);
    job_running = 0;
    mem_free();
}

/* ---- #104: PS with the overlay open ---- */

#define PS_GAME_PID 0x62C03
/* The overlay open over the game in front, from a cached result (no job). */
static void open_over_the_game(void)
{
    if (!has_result_memory()) TEST_ASSERT(mem_alloc() == 0);
    state.game_pid = PS_GAME_PID;
    active = 0;
    cache_ok = 1;
    cache_scene = state.scene;
    cache_seq = state.region_seq;
    cache_data[0].list.header = "日本語";
    control_tick(VJO_EV_TRIGGER); /* L + R */
    TEST_ASSERT(ov == OV_OPEN && input_blocked && g_view.open && !job_running);
}

/* open -> PS -> released: the kernel's GAME_EXIT with no process in front
 * (SceShell's LiveArea is) closes the overlay in the same wakeup, releases the
 * game's input, and tells the page to leave SceShell's touch region alone; a
 * stale L + R does not open it over the LiveArea. resume -> closed: the game
 * comes back with the overlay closed and its input free, and only the next
 * L + R opens it, which Cancel closes in game, giving the region back. */
static void test_ps_with_the_overlay_open_releases_input_and_leaves_touch_to_shell(void)
{
    reset();
    open_over_the_game();
    state.game_pid = 0;                              /* PS: the game suspended, SceShell in front */
    control_tick(VJO_EV_GAME_EXIT | VJO_EV_TRIGGER); /* L + R a moment before PS: one wakeup */
    TEST_CHECK(ov == OV_CLOSED && !input_blocked && !g_view.open && !game_active());
    TEST_CHECK(g_view.touch_handback == VJO_TOUCH_LEAVE);
    TEST_CHECK(DLOGGED("overlay close (game left) t=13000 ms: input block off rc=0"));
    control_tick(VJO_EV_TRIGGER); /* over the LiveArea */
    TEST_CHECK(ov == OV_CLOSED && !g_view.open && !input_blocked);

    test_now += 2000000;
    state.game_pid = PS_GAME_PID; /* resume */
    control_tick(VJO_EV_GAME_START);
    TEST_CHECK(ov == OV_CLOSED && !g_view.open && !input_blocked && pending_pid == PS_GAME_PID);
    pending_pid = 0;
    activate(PS_GAME_PID, "PCSG00415", VJO_GAME); /* classify_pending's verdict */
    control_tick(0);
    TEST_CHECK(ov == OV_CLOSED && !g_view.open && !input_blocked && game_active());

    open_over_the_game();
    TEST_CHECK(DLOGGED("input block on rc=0 (00000000) t=15000 ms"));
    vjo_post_command(VJO_CMD_CLOSED, NULL); /* Cancel */
    control_tick(0);
    TEST_CHECK(ov == OV_CLOSED && !input_blocked && !g_view.open);
    TEST_CHECK(g_view.touch_handback == VJO_TOUCH_RELEASE);
    TEST_CHECK(DLOGGED("overlay close (in game) t=15000 ms: input block off rc=0"));
    mem_free();
}

/* Any other way out of the game's foreground keeps the in-game hand-back
 * (deactivate): a process started over the game (the game stays in front),
 * or the kernel going back to a game behind the one that left. */
static void test_foreground_switch_gives_the_touch_region_back(void)
{
    reset();
    open_over_the_game();
    state.game_pid = 0x1137D9; /* a system app starts over the game */
    control_tick(VJO_EV_GAME_START);
    TEST_CHECK(ov == OV_CLOSED && !input_blocked && !g_view.open);
    TEST_CHECK(g_view.touch_handback == VJO_TOUCH_RELEASE);
    TEST_CHECK(DLOGGED("overlay close (foreground changed) t=13000 ms"));

    reset();
    open_over_the_game();
    state.game_pid = 0x7000; /* the game left, the kernel is back at the game behind it */
    control_tick(VJO_EV_GAME_EXIT);
    TEST_CHECK(ov == OV_CLOSED && !input_blocked && g_view.touch_handback == VJO_TOUCH_RELEASE);
    mem_free();
}

/* Cancel and PS together: the in-game close a moment before the game left is
 * turned into "leave it" while the page may still be open; a later close sets
 * its own hand-back again. */
static void test_close_just_before_ps_leaves_touch_to_shell(void)
{
    reset();
    open_over_the_game();
    vjo_post_command(VJO_CMD_CLOSED, NULL);
    control_tick(0);
    TEST_CHECK(ov == OV_CLOSED && g_view.touch_handback == VJO_TOUCH_RELEASE);
    state.game_pid = 0;
    control_tick(VJO_EV_GAME_EXIT);
    TEST_CHECK(ov == OV_CLOSED && !input_blocked && g_view.touch_handback == VJO_TOUCH_LEAVE);
    activate(PS_GAME_PID, "PCSG00415", VJO_GAME);
    open_over_the_game();
    control_tick(VJO_EV_TRIGGER); /* L + R closes it in game */
    TEST_CHECK(ov == OV_CLOSED && g_view.touch_handback == VJO_TOUCH_RELEASE);

    /* Only the game's exit turns a close into "leave": Cancel, then a process
     * started over the game, keeps it (audit-overlay-ps-touch A1, N2). */
    reset();
    open_over_the_game();
    vjo_post_command(VJO_CMD_CLOSED, NULL);
    control_tick(0);
    state.game_pid = 0x1137D9;
    control_tick(VJO_EV_GAME_START);
    TEST_CHECK_(g_view.touch_handback == VJO_TOUCH_RELEASE, "handback %d", g_view.touch_handback);
    TEST_CHECK(!input_blocked && !g_view.open);
    mem_free();
}

/* ---- OCR: the game worker's vita-vn-ocr ---- */

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

#ifdef VJO_WITH_VOCR
/* A job asks by tiers (API 21): the configured size and every smaller one, the
 * largest's model naming it; the tier it reports is the one the worker ran, whose
 * model name (the self-test's stage record and the Anki cards read it after the
 * job) is a constant, never the request's copy, a local of the call. */
static void test_vocr_ran_names_outlive_the_job(void)
{
    VjoGameOcrResult result;
    unsigned ran = 0;
    reset();
    fake_recognized_text = "猫";
    TEST_ASSERT(mem_alloc() == 0);
    job_cfg = cfg;
    fake_memory_fallback = 1; /* the game ran the next smaller tier */
    (void)vocr_run_one(&result, 0, vocr_tiers(), &ran, -1);
    TEST_CHECK(fake_game_request.tiers == 3u && !strcmp(fake_game_request.model, "VOCR3_H15_w8.vocr") &&
               !fake_game_request.fallback[0]);
    TEST_CHECK(result.fallback && ran == VJO_VOCR_TIER_SMALL && !strcmp(vjo_vocr_tier_model((int)ran), "VOCR3_FL10_w8.vocr"));
    fake_memory_fallback = 0;
    (void)vocr_run_one(&result, 0, vocr_tiers(), &ran, -1);
    TEST_CHECK(!result.fallback && ran == VJO_VOCR_TIER_NORMAL);
    /* each size asks for itself and every smaller one */
    static const unsigned want[] = {0, 1u, 3u, 7u};
    static const char *const model[] = {"", "VOCR3_FL10_w8.vocr", "VOCR3_H15_w8.vocr", "VOCR4_W20_w8.vocr"};
    for (int t = 1; t <= 3; t++) {
        job_cfg.vocr_tier = t;
        (void)vocr_run_one(&result, 0, vocr_tiers(), &ran, -1);
        TEST_CHECK_(fake_game_request.tiers == want[t] && !strcmp(fake_game_request.model, model[t]) && ran == (unsigned)t,
                    "tier %d: tiers 0x%X model %s ran %u", t, fake_game_request.tiers, fake_game_request.model, ran);
    }
    mem_free();
}

/* The job's capture is its latest one: voided while the worker waits for a
 * changed screen to hold still, then the retry's. A settled screen cancels no
 * job whose retry already shows it. */
static void test_vocr_capture_follows_the_retry(void)
{
    reset();
    fake_republish = 1; republish_reads = captured_while_void = 0;
    TEST_CHECK(run_vocr_job() == VJO_OK);
    TEST_CHECK(republish_reads == 2 && !captured_while_void);
    TEST_CHECK(job_captured && job_capture_scene == 3 && job_capture_seq == 5);
    TEST_CHECK(shows_screen(job_capture_scene, job_capture_seq, &state));
    fake_republish = 0;
    mem_free();
}
#endif

#ifdef VJO_WITH_VOCR
#if VJO_DEBUG /* the debug build's log lines (the normal build's: test_normal_build_*) */
/* The recognizer's text is logged as it read it, a line per line; the sentence
 * shown, saved and looked up is normalised (one form of each mark, ー or 一 by
 * context), and what changed is logged. */
static void test_vocr_text_is_logged_raw_and_shown_normalised(void)
{
    reset();
    fake_recognized_text = "コ一ヒ一！？\n統ー的．．．";
    TEST_CHECK(run_vocr_job() == VJO_OK);
    TEST_CHECK(!strcmp(cache_data[0].sentence, "コーヒー!?\n統一的…"));
    TEST_MSG("shown: %s", cache_data[0].sentence);
    TEST_CHECK(logged("game vocr text 0: コ一ヒ一！？") != NULL && logged("game vocr text 1: 統ー的．．．") != NULL);
    TEST_CHECK(logged("game vocr text 0 shown: コーヒー!?") != NULL && logged("game vocr text 1 shown: 統一的…") != NULL);
    fake_recognized_text = "猫";
    log_count = 0;
    TEST_CHECK(run_vocr_job() == VJO_OK && !strcmp(cache_data[0].sentence, "猫"));
    TEST_CHECK(logged("game vocr text 0: 猫") != NULL && !logged("shown:")); /* unchanged: one line */
    mem_free();
}
#endif
#endif

/* Every job is the game worker's: on the console, no image or connection leaves it. */
static void test_vocr_empty_region(void)
{
    reset();
    int rc = run_vocr_job();
#ifdef VJO_WITH_VOCR
    /* An empty region: no line, no model read, the workspace is released. */
    TEST_CHECK(rc == VJO_OK && cache_data[0].failed_stage == VJO_STAGE_NONE);
    TEST_CHECK(cache_data[0].ocr_text && !cache_data[0].ocr_text[0]);
    TEST_CHECK(job_captured && job_capture_scene == 2 && job_capture_seq == 4);
    TEST_CHECK(vocr_allocs == 1 && vocr_frees == 1 && vocr_paf_allocs == 0);
#else
    TEST_CHECK(rc == VJO_E_OCR_UNAVAILABLE && cache_data[0].failed_stage == VJO_STAGE_OCR);
    TEST_CHECK(cache_data[0].err.detail && strstr(cache_data[0].err.detail, "game OCR worker is unavailable"));
    TEST_CHECK(vocr_allocs == 0);
#endif
    mem_free();
}

/* A config.ini that still chooses a removed engine (Google Lens, the VOCR2
 * trial, ncnn) runs the local engine: the configured size with the smaller
 * ones, in the game, with no screenshot and no connection. */
static void test_removed_engine_choice_runs_the_local_engine(void)
{
    static const char *const ini[] = {"ocr_backend = lens\n", "ocr_backend = vocr2\n", "ocr_backend = ncnn\n"};
    static const char *const warning[] = {
        "config warning: ocr_backend: Google Lens was removed (the overlay is offline); using vocr",
        "config warning: ocr_backend: VOCR2 was removed; using vocr",
        "config warning: ocr_backend: ncnn runs only in host tools; using vocr"};
    for (unsigned i = 0; i < 3; i++) {
        reset();
        vjo_config_defaults(&loaded);
        vjo_config_parse(&loaded, ini[i], strlen(ini[i]));
        apply_config();
        TEST_CHECK(cfg.n_warnings == 1);
        TEST_CHECK(cfg.dictionary == VJO_DICT_LOCAL && background_wanted());
        TEST_CHECK_(logged(warning[i]) != NULL, "%s", warning[i]);
        int rc = run_vocr_job();
#ifdef VJO_WITH_VOCR
        TEST_CHECK(rc == VJO_OK && job_captured && vocr_allocs == 1 && vocr_frees == 1);
        TEST_CHECK(!strcmp(fake_game_request.model, "VOCR3_H15_w8.vocr"));
        TEST_CHECK(fake_game_request.tiers == 3u && !fake_game_request.fallback[0]); /* Normal, then Small */
        TEST_CHECK(cache_data[0].failed_stage == VJO_STAGE_NONE);
#else
        TEST_CHECK(!job_captured && !vocr_allocs && cache_data[0].failed_stage == VJO_STAGE_OCR);
        TEST_CHECK(rc == VJO_E_OCR_UNAVAILABLE);
#endif
        TEST_CHECK(!cancels); /* nothing to cancel: no request leaves the console */
        mem_free();
    }
}

/* The cached result belongs to its model files: switching the size or the
 * models' directory drops it; a setting that is no pipeline's keeps it. */
static void test_engine_cache_tracks_backend_models_and_storage(void)
{
    reset();
    apply_config();
    cache_ok = 1;
    loaded.font_size_ja = 18; /* the overlay's look: no pipeline's */
    apply_config();
    TEST_CHECK(cache_ok);
    loaded.vocr_tier = VJO_VOCR_TIER_SMALL;
    apply_config();
    TEST_CHECK(!cache_ok);
    cache_ok = 1;
    loaded.vocr_tier = VJO_VOCR_TIER_LARGE; /* another model size: another pipeline */
    apply_config();
    TEST_CHECK(!cache_ok);
    cache_ok = 1;
    loaded.vocr_tier = VJO_VOCR_TIER_NORMAL;
    apply_config();
    TEST_CHECK(!cache_ok);
    cache_ok = 1;
    strcpy(loaded.ocr_model_dir, "ux0:data/other-ocr");
    apply_config();
    TEST_CHECK(!cache_ok);
}

static void test_vocr_config(void)
{
    VjoConfig c;
    const char ini[] = "ocr_backend = VOCR\nvocr_model = FL10_w8.vocr\n";
    const char bad[] = "vocr_model = ../H15_w8.vocr\nvocr_model = H15_w8.bin\nvocr_model =\n";
    vjo_config_defaults(&c);
    /* Normal (VOCR3-H15) by default; a job runs Small (VOCR3-FL10) when the game's memory has no room for it. */
    TEST_CHECK(c.vocr_tier == VJO_VOCR_TIER_NORMAL && c.log_file == 1);
    TEST_CHECK(!strcmp(vjo_vocr_tier_model(c.vocr_tier), "VOCR3_H15_w8.vocr") &&
               !strcmp(vjo_vocr_tier_model(VJO_VOCR_TIER_SMALL), "VOCR3_FL10_w8.vocr") &&
               !strcmp(vjo_vocr_tier_model(VJO_VOCR_TIER_LARGE), "VOCR4_W20_w8.vocr"));
    vjo_config_parse(&c, ini, sizeof(ini) - 1); /* v0.2.0's FL10 chose the smallest size */
    TEST_CHECK(c.vocr_tier == VJO_VOCR_TIER_SMALL && c.n_warnings == 0);
    vjo_config_defaults(&c);
    vjo_config_parse(&c, bad, sizeof(bad) - 1);
    TEST_CHECK(c.vocr_tier == VJO_VOCR_TIER_NORMAL && c.n_warnings == 3); /* each kept the default */
    const char fallbacks[] = "vocr_fallback_model = none\nvocr_fallback_model = ../x.vocr\n";
    vjo_config_defaults(&c);
    vjo_config_parse(&c, fallbacks, sizeof(fallbacks) - 1); /* no longer read: the tiers fall back */
    TEST_CHECK(c.vocr_tier == VJO_VOCR_TIER_NORMAL && !c.n_warnings);
    /* A saved VOCR2 choice keeps the configured model's size: the local engine runs it. */
    const char old_ini[] = "ocr_backend = VoCr2\nvocr_model = H15_w8.vocr\n";
    vjo_config_defaults(&c);
    vjo_config_parse(&c, old_ini, sizeof(old_ini) - 1);
    TEST_CHECK(c.n_warnings == 1 && c.vocr_tier == VJO_VOCR_TIER_NORMAL);
}

static void test_vocr_model_change_invalidates_cache(void)
{
    reset();
    apply_config();
    cache_ok = 1;
    loaded.vocr_tier = VJO_VOCR_TIER_SMALL;
    apply_config();
    TEST_CHECK(!cache_ok);
    cache_ok = 1;
    loaded.ocr_refine_lines = loaded.ocr_refine_lines == 1 ? 2 : 1; /* and so is the refinement's mode */
    apply_config();
    TEST_CHECK(!cache_ok);
    cache_ok = 1;
    apply_config(); /* the same again: nothing changed */
    TEST_CHECK(cache_ok);
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
    cfg.vocr_tier = VJO_VOCR_TIER_SMALL; /* the smallest size: one job, one error */
    strokes(15, 39); /* one light line on black */
    int rc = run_vocr_job();
    TEST_CHECK(rc == VJO_E_OCR_MODEL && cache_data[0].failed_stage == VJO_STAGE_OCR);
    TEST_CHECK(!cache_data[0].sentence && vocr_allocs == 1 && vocr_frees == 1 && fake_game_request.tiers == 1u);
    mem_free();
    reset(); /* the default size, both files missing: two jobs, the smaller one's error */
    strokes(15, 39);
    TEST_CHECK(run_vocr_job() == VJO_E_OCR_MODEL && cache_data[0].failed_stage == VJO_STAGE_OCR);
    TEST_CHECK(!cache_data[0].sentence && vocr_allocs == 2 && vocr_frees == 2);
    TEST_CHECK(!strcmp(fake_game_request.model, "VOCR3_FL10_w8.vocr") && fake_game_request.tiers == 1u &&
               !fake_game_request.fallback[0]);
    TEST_CHECK(logged("VOCR3_FL10_w8.vocr is unusable here (io=0x00000000), using none for this game") != NULL);
    mem_free();
}

/* The chosen size's model is missing or corrupt on the card: the same job is
 * repeated with the next smaller tier, and this game's later jobs leave it out
 * until the game changes. Only the log says so. The memory ladder itself is the
 * game worker's (test_game_vocr.c); this is the Shell's half. */
static void test_vocr_unusable_model_uses_the_fallback(void)
{
    reset();
    fake_missing_model = "VOCR3_H15_w8.vocr";
    fake_recognized_text = "猫";
    TEST_CHECK(run_vocr_job() == VJO_OK && cache_data[0].sentence && !strcmp(cache_data[0].sentence, "猫"));
    TEST_CHECK(vocr_allocs == 2 && !strcmp(fake_game_request.model, "VOCR3_FL10_w8.vocr") && fake_game_request.tiers == 1u);
    TEST_CHECK(logged("VOCR3_H15_w8.vocr is unusable here (io=0x80010002), using VOCR3_FL10_w8.vocr for this game") != NULL);
    TEST_CHECK(!cache_data[0].err.detail && cache_data[0].failed_stage == VJO_STAGE_NONE); /* nothing shown of it */
    TEST_CHECK(!strcmp(anki_model, "VOCR3_FL10_w8.vocr")); /* the cards name the model that read the text */
    mem_free();
    unsigned before = vocr_allocs;
    TEST_ASSERT(mem_alloc() == 0);
    job_cfg = cfg; vjo_arena_reset(&results[0]);
    TEST_CHECK(run_job(&results[0], &cache_data[0]) == VJO_OK); /* one job, Normal left out */
    TEST_CHECK(vocr_allocs == before + 1 && !strcmp(fake_game_request.model, "VOCR3_FL10_w8.vocr"));
    TEST_CHECK(fake_game_request.tiers == 1u && !fake_game_request.fallback[0]);
    mem_free();
    game_generation++; job_generation = game_generation; /* another game: Normal is tried again */
    fake_missing_model = NULL;
    TEST_ASSERT(mem_alloc() == 0);
    job_cfg = cfg; vjo_arena_reset(&results[0]);
    TEST_CHECK(run_job(&results[0], &cache_data[0]) == VJO_OK);
    TEST_CHECK(!strcmp(fake_game_request.model, "VOCR3_H15_w8.vocr") && fake_game_request.tiers == 3u);
    mem_free();
    /* Large chosen and Normal's file bad: Large stays in this game's ladder, Normal is left out. */
    reset();
    cfg.vocr_tier = VJO_VOCR_TIER_LARGE;
    fake_tier_checks = 1u << (VJO_VOCR_TIER_LARGE - 1u); /* Large does not fit this game */
    fake_missing_model = "VOCR3_H15_w8.vocr"; fake_recognized_text = "猫";
    TEST_CHECK(run_vocr_job() == VJO_OK && vocr_allocs == 2);
    TEST_CHECK(vocr_unusable == 1u << (VJO_VOCR_TIER_NORMAL - 1u) && fake_game_request.tiers == 1u);
    mem_free();
    TEST_ASSERT(mem_alloc() == 0);
    job_cfg = cfg; vjo_arena_reset(&results[0]);
    TEST_CHECK(run_job(&results[0], &cache_data[0]) == VJO_OK);
    TEST_CHECK(fake_game_request.tiers == 5u && !strcmp(fake_game_request.model, "VOCR4_W20_w8.vocr"));
    /* the worker went from Large to Small: two tiers down, Small read the text */
    TEST_CHECK(logged("tier large did not fit in this game (need 19777216, free 91238400); using small") != NULL);
    mem_free();
}

/* A transient failure (an Open timeout, unavailable storage) retries this job
 * with the next smaller tier but does not stop later jobs from trying Normal. */
static void test_vocr_transient_model_failure_is_not_sticky(void)
{
    const int32_t transient[] = {VJO_ERR_TIMEOUT, (int32_t)0x80010013u, VJO_ERR_CANCELLED};
    for (unsigned i = 0; i < 3; i++) {
        reset();
        fake_missing_model = "VOCR3_H15_w8.vocr"; fake_missing_io = transient[i];
        fake_recognized_text = "猫";
        TEST_CHECK(run_vocr_job() == VJO_OK && vocr_allocs == 2 && !strcmp(fake_game_request.model, "VOCR3_FL10_w8.vocr"));
        TEST_CHECK(logged("using VOCR3_FL10_w8.vocr for this job") != NULL && !vocr_unusable);
        mem_free();
        fake_missing_model = NULL;
        TEST_ASSERT(mem_alloc() == 0);
        job_cfg = cfg; vjo_arena_reset(&results[0]);
        TEST_CHECK(run_job(&results[0], &cache_data[0]) == VJO_OK && vocr_allocs == 3);
        TEST_CHECK(!strcmp(fake_game_request.model, "VOCR3_H15_w8.vocr") && fake_game_request.tiers == 3u);
        mem_free();
    }
    /* A hash/format rejection (io 0) is a bad file: sticky, like a missing one. */
    reset();
    fake_missing_model = "VOCR3_H15_w8.vocr"; fake_missing_io = 0; fake_recognized_text = "猫";
    TEST_CHECK(run_vocr_job() == VJO_OK && vocr_unusable == 1u << (VJO_VOCR_TIER_NORMAL - 1u));
    mem_free();
}

/* No retry when the tier the worker ran is already the smallest (its own file
 * failed: then it is left out, and later jobs still try Normal), when the job
 * was cancelled, or for the smallest size. */
static void test_vocr_model_error_retry_limits(void)
{
    reset();
    fake_memory_fallback = 1; fake_missing_model = "VOCR3_FL10_w8.vocr"; fake_missing_with_fallback = 1;
    TEST_CHECK(run_vocr_job() == VJO_E_OCR_MODEL && vocr_allocs == 1 && vocr_unusable == 1u);
    mem_free();
    TEST_ASSERT(mem_alloc() == 0);
    fake_memory_fallback = 0; fake_missing_model = NULL;
    job_cfg = cfg; vjo_arena_reset(&results[0]);
    TEST_CHECK(run_job(&results[0], &cache_data[0]) == VJO_OK && fake_game_request.tiers == 2u); /* Normal alone */
    mem_free();
    reset();
    fake_missing_model = "VOCR3_H15_w8.vocr"; fake_cancel_on_model_error = 1;
    run_vocr_job();
    TEST_CHECK(vocr_allocs == 1 && !vocr_unusable);
    mem_free();
    reset();
    cfg.vocr_tier = VJO_VOCR_TIER_SMALL;
    fake_missing_model = "VOCR3_FL10_w8.vocr";
    TEST_CHECK(run_vocr_job() == VJO_E_OCR_MODEL && vocr_allocs == 1 && fake_game_request.tiers == 1u);
    mem_free();
}

/* Neither tier fitted: the job line says the smaller one was refused too. */
static void test_vocr_refused_fallback_is_labelled(void)
{
    reset();
    fail_vocr_user = 1; fake_memory_fallback = 1; fake_free_user = 4300000;
    TEST_CHECK(run_vocr_job() == VJO_E_OOM);
#if VJO_DEBUG
    TEST_CHECK(logged("game vocr rc=-103 VOCR3_FL10_w8.vocr (fallback, refused too):") != NULL);
#else
    /* the normal build's job line, and its refused memory check */
    TEST_CHECK(logged("ocr rc=-103 VOCR3_FL10_w8.vocr: 0 lines, ") != NULL);
    TEST_CHECK(logged("game vocr memory: free USER=4300000 + own weights 0, needs workspace ") != NULL &&
               logged("check refused") != NULL);
#endif
    TEST_CHECK(cache_data[0].err.detail && !strcmp(cache_data[0].err.detail, OOM_GAME_REGION));
    mem_free();
    /* every tier refused by the ladder: one line per tier, the last says nothing smaller fits */
    reset();
    cfg.vocr_tier = VJO_VOCR_TIER_LARGE;
    fake_tier_checks = VJO_VOCR_TIERS_ALL; fake_free_user = 4300000;
    memcpy(job_title, "PCSG00415", 10);
    TEST_CHECK(run_vocr_job() == VJO_E_OOM);
    TEST_CHECK(logged("tier large did not fit in PCSG00415 (need 19777216, free 4312288); trying normal") != NULL);
    TEST_CHECK(logged("tier normal did not fit in PCSG00415 (need 10388608, free 4308192); trying small") != NULL);
    TEST_CHECK(logged("tier small did not fit in PCSG00415 (need 5194304, free 4304096); no smaller tier: "
                      "not enough memory") != NULL);
    mem_free();
}

/* API 21, the owner's 04:54Z rule: a size that does not fit the game is left
 * silently. The worker ran a smaller tier: the log names each tier that did not
 * fit, in that game, with what it needed against what was free, and the tier
 * used; the user sees the text as usual and no note of it anywhere. */
static void test_vocr_memory_fallback_is_logged(void)
{
    reset();
    cfg.vocr_tier = VJO_VOCR_TIER_LARGE;
    fake_recognized_text = "猫";
    fake_tier_checks = 1u << (VJO_VOCR_TIER_LARGE - 1u) | 1u << (VJO_VOCR_TIER_NORMAL - 1u);
    memcpy(job_title, "PCSG00415", 10);
    TEST_CHECK(run_vocr_job() == VJO_OK && !strcmp(cache_data[0].sentence, "猫") && vocr_allocs == 1);
    TEST_CHECK(fake_game_request.tiers == 7u && !strcmp(fake_game_request.model, "VOCR4_W20_w8.vocr"));
#if VJO_DEBUG
    const char *line = logged("game vocr rc=0 VOCR3_FL10_w8.vocr (fallback):");
#else
    const char *line = logged("ocr rc=0 VOCR3_FL10_w8.vocr: "); /* the normal build's job line: the size that ran */
#endif
    TEST_CHECK(line != NULL);
    /* need: the check's bytes and the tier's reserve; free: free USER and the own blocks released first */
    line = logged("tier large");
    TEST_CHECK(line && !strcmp(line, "tier large did not fit in PCSG00415 (need 19777216, free 91238400); trying normal"));
    TEST_MSG("%s", line ? line : "(no large line)");
    line = logged("tier normal");
    TEST_CHECK(line && !strcmp(line, "tier normal did not fit in PCSG00415 (need 10388608, free 91234304); using small"));
    TEST_MSG("%s", line ? line : "(no normal line)");
    TEST_CHECK(!logged("tier small")); /* it fitted */
    /* nothing of it in what the user sees: no error, no warning, the Anki cards name Small's model */
    TEST_CHECK(!cache_data[0].err.detail && cache_data[0].failed_stage == VJO_STAGE_NONE && !g_view.n_warnings);
    TEST_CHECK(!strcmp(anki_model, "VOCR3_FL10_w8.vocr"));
    mem_free();
    /* the largest tier fitted: no tier line at all */
    reset();
    cfg.vocr_tier = VJO_VOCR_TIER_LARGE;
    fake_recognized_text = "猫"; fake_tier_checks = 0;
    TEST_CHECK(run_vocr_job() == VJO_OK && !logged("did not fit") && !strcmp(anki_model, "VOCR4_W20_w8.vocr"));
    mem_free();
    /* a job's game is the title it started in, kept while the job runs */
    reset();
    memcpy(title_id, "PCSB00123", 10);
    TEST_ASSERT(mem_alloc() == 0);
    start_job("test", 0);
    TEST_CHECK(!strcmp(job_title, "PCSB00123"));
    memcpy(title_id, "PCSG00415", 10);
    TEST_CHECK(!strcmp(job_title, "PCSB00123"));
    job_running = 0;
    mem_free();
}

static void test_vocr_allocation_fallback_and_failure(void)
{
    reset();
    fail_vocr_user = 1;
    int rc = run_vocr_job();
    TEST_CHECK(rc == VJO_E_OOM && cache_data[0].failed_stage == VJO_STAGE_OCR);
    TEST_CHECK(vocr_paf_allocs == 0 && vocr_frees == 0);
    mem_free();
}

#if VJO_DEBUG /* the debug build's log lines (the normal build's: test_normal_build_*) */
static void test_vocr_logs_memory_check_and_frames(void)
{
    reset();
    fail_vocr_user = 1; /* refused: the numbers are logged, not workspace=0 */
    TEST_CHECK(run_vocr_job() == VJO_E_OOM);
    const char *line = logged("game vocr rc=");
    TEST_CHECK(line && strstr(line, "USER workspace=244896"));
    line = logged("game vocr memory:");
    /* 5,000,000 - 4,194,304 - 245,760 - 659,456 = -99,520: the job was 99,520 B short */
    TEST_CHECK(line && !strcmp(line, "game vocr memory: free USER=5000000, needs workspace 245760 + "
                                      "weights 659456 + reserve 4194304: check refused, headroom -99520"));
    TEST_MSG("%s", line ? line : "(no memory line)");
    TEST_CHECK(!logged("game vocr speed:") && !logged("game vocr lines"));
    mem_free();

    reset();
    fail_vocr_user = 1; fake_free_user = (int32_t)0x80020001u; /* the query itself failed */
    TEST_CHECK(run_vocr_job() == VJO_E_OOM);
    line = logged("game vocr memory:");
    TEST_CHECK(line && !strcmp(line, "game vocr memory: free USER unknown (error 0x80020001), needs "
                                      "workspace 245760 + weights 659456 + reserve 4194304: check refused"));
    TEST_MSG("%s", line ? line : "(no memory line)");
    mem_free();

    reset();
    fake_recognized_text = "猫";
    fake_lines = 14; /* three log lines of at most six */
    TEST_CHECK(run_vocr_job() == VJO_OK); /* blank region: the fake result is published */
    line = logged("game vocr rc=0");
    TEST_CHECK(line && strstr(line, "recognize=92 ms frames=205 "));
    line = logged("game vocr memory:");
    TEST_CHECK(line && strstr(line, "free USER=91226112") && strstr(line, "check passed, headroom "));
    line = logged("game vocr speed:");
    TEST_CHECK(line && !strcmp(line, "game vocr speed: 205 frames in 92345 us, 450 us per frame"));
    line = logged("game vocr lines:");
    TEST_CHECK(line && !strncmp(line, "game vocr lines: 917x30 114f 52.1ms 916x30 113f 52.1ms", 54));
    TEST_MSG("%s", line ? line : "(no lines)");
    unsigned chunks = 0, entries = 0;
    for (unsigned i = 0; i < log_count && i < 32; i++)
        if (!strncmp(log_lines[i], "game vocr lines", 15)) {
            chunks++;
            for (const char *c = log_lines[i]; (c = strstr(c, "ms")); c += 2) entries++;
            TEST_CHECK(strlen(log_lines[i]) < 254); /* vjo_log's line limit */
        }
    TEST_CHECK(chunks == 3 && entries == 14);
    mem_free();
    /* API 20: lines the LM decoded carry their confidence, a '*' where it changed the text;
     * still six to a log line and under vjo_log's limit at the longest */
    reset();
    fake_recognized_text = "猫";
    fake_lines = 6;
    fake_line_flags = 0x8000u | 0x4000u | 612u;
    TEST_CHECK(run_vocr_job() == VJO_OK);
    line = logged("game vocr lines:");
    TEST_CHECK(line && !strncmp(line, "game vocr lines: 917x30 114f 52.1ms c612* 916x30 113f 52.1ms c612*", 66));
    TEST_MSG("%s", line ? line : "(no lines)");
    TEST_CHECK(line && strlen(line) < 254 && !logged("game vocr lines (cont)"));
    reset();
    fake_recognized_text = "猫";
    fake_lines = 1;
    fake_line_flags = 0x4000u | 999u; /* decoded, unchanged */
    TEST_CHECK(run_vocr_job() == VJO_OK);
    line = logged("game vocr lines:");
    TEST_CHECK(line && !strcmp(line, "game vocr lines: 917x30 114f 52.1ms c999"));
    TEST_MSG("%s", line ? line : "(no lines)");
    /* read again by the cascade's larger model (its reading and confidence shown), changed */
    reset();
    fake_recognized_text = "猫";
    fake_lines = 1;
    fake_line_flags = 0x8000u | 0x4000u | 0x0400u | 845u;
    TEST_CHECK(run_vocr_job() == VJO_OK);
    line = logged("game vocr lines:");
    TEST_CHECK(line && !strcmp(line, "game vocr lines: 917x30 114f 52.1ms c845*^"));
    TEST_MSG("%s", line ? line : "(no lines)");
    TEST_CHECK(!logged("game vocr cascade speed:"));
    /* the cascade's model, from its step: its frames and time apart from the job's model's */
    reset();
    fake_recognized_text = "猫";
    fake_lines = 1;
    fake_line_flags = 0x8000u | 0x4000u | 0x0400u | 845u;
    fake_cascade_frames = 160;
    fake_cascade_us = 128000;
    TEST_CHECK(run_vocr_job() == VJO_OK);
    line = logged("game vocr speed:");
    TEST_CHECK(line && !strcmp(line, "game vocr speed: 205 frames in 92345 us, 450 us per frame"));
    line = logged("game vocr cascade speed:");
    TEST_CHECK(line && !strcmp(line, "game vocr cascade speed: 160 frames in 128000 us, 800 us per frame"));
    TEST_MSG("%s", line ? line : "(no cascade speed line)");
    fake_cascade_frames = fake_cascade_us = 0;
    fake_line_flags = 0;
    mem_free();
}
#endif

#if VJO_DEBUG /* the debug build's log lines (the normal build's: test_normal_build_*) */
/* The job policy in the request (API 20): the stable-text wait from its keys,
 * the blank-line check only with ocr_skip_blank_lines = on (off by default). */
/* A job's log on main's defaults (15 steps with the cascade, two lines, the
 * lookup): the result's lines (vocr_run_one) and the text's (log_text_lines)
 * each go out as one append, the rest one each, and no batch is held across
 * the game's exchange (vjoOcrSubmit/Read check it). On the console an append
 * of log.txt costs 12-46 ms (#81's self-test timestamps). */
static void test_vocr_job_log_is_appended_in_batches(void)
{
    reset();
    fake_recognized_text = "猫が\n好き"; fake_lines = 2;
    fake_steps = 1; fake_cascade_frames = 40; fake_cascade_us = 202000;
    unsigned lines0 = log_count, appends0 = log_appends;
    batched_lines = 0;
    TEST_CHECK(run_vocr_job() == VJO_OK);
    unsigned lines = log_count - lines0, appends = log_appends - appends0;
    /* rc, capture, memory, speed, cascade speed, lines, 15 steps: 21 in one append; 2 text lines in one */
    TEST_CHECK(batched_lines == 23);
    TEST_CHECK(lines - appends == 23 - 2);
    TEST_CHECK(!open_batch);
    TEST_MSG("%u lines, %u appends, %u batched", lines, appends, batched_lines);
    printf("\n  a job's %u log lines: %u appends of log.txt (line by line: %u)\n", lines, appends, lines);
    /* every line is still logged, the steps and the text included */
    TEST_CHECK(logged("game vocr rc=0") != NULL);
    TEST_CHECK(logged("gstep job 18 ") != NULL); /* the cascade's step (this file's vjo_gstep_format) */
    TEST_CHECK(logged("game vocr text 1: ") != NULL);
    fake_steps = 0; fake_cascade_frames = fake_cascade_us = 0;
    mem_free();
}
#endif

static void test_vocr_request_carries_the_job_policy(void)
{
    reset();
    fake_recognized_text = "猫";
    TEST_CHECK(run_vocr_job() == VJO_OK);
    TEST_CHECK(!(fake_game_request.flags & VJO_GAME_OCR_F_BLANK));
    TEST_CHECK((fake_game_request.flags >> VJO_GAME_OCR_SETTLE_SHIFT & 0xFFu) == 15u &&
               (fake_game_request.flags >> VJO_GAME_OCR_SETTLE_MAX_SHIFT & 0xFFu) == 40u);
    TEST_CHECK(!(fake_game_request.flags & VJO_GAME_OCR_F_SETTLE_FIRST)); /* the kernel saw the region settle */
    TEST_MSG("default flags 0x%08X", (unsigned)fake_game_request.flags);
    mem_free();
    /* a screen the kernel has not seen settle (VjoState.stable 0): the job waits first */
    reset();
    fake_recognized_text = "猫";
    state.stable = 0;
    TEST_CHECK(run_vocr_job() == VJO_OK);
    TEST_CHECK(fake_game_request.flags & VJO_GAME_OCR_F_SETTLE_FIRST);
    TEST_MSG("unsettled screen: flags 0x%08X", (unsigned)fake_game_request.flags);
    mem_free();
    /* ... unless the wait is off: then nothing of it is sent */
    reset();
    cfg.ocr_settle_ms = 0;
    fake_recognized_text = "猫";
    state.stable = 0;
    TEST_CHECK(run_vocr_job() == VJO_OK);
    TEST_CHECK(!(fake_game_request.flags & VJO_GAME_OCR_F_SETTLE_FIRST) &&
               !(fake_game_request.flags >> VJO_GAME_OCR_SETTLE_SHIFT));
    mem_free();
    reset();
    cfg.ocr_skip_blank_lines = 1;
    cfg.ocr_settle_ms = 0;
    fake_recognized_text = "猫";
    TEST_CHECK(run_vocr_job() == VJO_OK);
    TEST_CHECK((fake_game_request.flags & VJO_GAME_OCR_F_BLANK) &&
               !(fake_game_request.flags >> VJO_GAME_OCR_SETTLE_SHIFT & 0xFFu) &&
               !(fake_game_request.flags & VJO_GAME_OCR_F_SETTLE_FIRST));
    TEST_MSG("blank on, wait off: flags 0x%08X", (unsigned)fake_game_request.flags);
    mem_free();
    /* the refinement's mode: psp by default, on, off */
    static const unsigned modes[] = {VJO_GAME_OCR_REFINE_PSP, VJO_GAME_OCR_REFINE_ON, 0};
    for (unsigned i = 0; i < 3; i++) {
        reset();
        if (i) cfg.ocr_refine_lines = (int)modes[i];
        fake_recognized_text = "猫";
        TEST_CHECK(run_vocr_job() == VJO_OK);
        TEST_CHECK((fake_game_request.flags >> VJO_GAME_OCR_REFINE_SHIFT & VJO_GAME_OCR_REFINE_MASK) == modes[i]);
        TEST_MSG("mode %u: flags 0x%08X", modes[i], (unsigned)fake_game_request.flags);
        mem_free();
    }
    /* the cascade's ceiling in 250 ms steps: 2 s by default, off with 0, 1-124 ms one step, 3,750 the most */
    static const struct { int ms; unsigned units; } cascade[] = {{2000, 8}, {0, 0}, {100, 1}, {375, 2}, {3750, 15}};
    for (unsigned i = 0; i < sizeof(cascade) / sizeof(cascade[0]); i++) {
        reset();
        cfg.ocr_cascade_ms = cascade[i].ms;
        fake_recognized_text = "猫";
        TEST_CHECK(run_vocr_job() == VJO_OK);
        TEST_CHECK((fake_game_request.flags >> VJO_GAME_OCR_CASCADE_SHIFT & VJO_GAME_OCR_CASCADE_MASK) == cascade[i].units);
        TEST_MSG("ocr_cascade_ms %d: flags 0x%08X", cascade[i].ms, (unsigned)fake_game_request.flags);
        mem_free();
    }
    /* a wait of 1-4 ms is the shortest one (10 ms), not none; 1,000 ms and 10 s fit their bits */
    static const struct { int ms, max_ms; unsigned settle, max; } units[] = {
        {3, 100, 1, 2}, {14, 2000, 1, 40}, {15, 2000, 2, 40}, {1000, 10000, 100, 200}};
    for (unsigned i = 0; i < sizeof(units) / sizeof(units[0]); i++) {
        reset();
        cfg.ocr_settle_ms = units[i].ms;
        cfg.ocr_settle_max_ms = units[i].max_ms;
        fake_recognized_text = "猫";
        TEST_CHECK(run_vocr_job() == VJO_OK);
        TEST_CHECK((fake_game_request.flags >> VJO_GAME_OCR_SETTLE_SHIFT & 0xFFu) == units[i].settle &&
                   (fake_game_request.flags >> VJO_GAME_OCR_SETTLE_MAX_SHIFT & 0xFFu) == units[i].max);
        TEST_MSG("%d ms / %d ms: flags 0x%08X", units[i].ms, units[i].max_ms, (unsigned)fake_game_request.flags);
        mem_free();
    }
    /* ocr-first: a job the open overlay waits for (not Auto's, a refresh's or the self-test's)
     * waits at most ocr_settle_press_ms (1 s by default), ocr_settle_max_ms if that is less;
     * 0 is the job's own limit. */
    static const struct { int press, max_ms, prepares, refresh; unsigned max; } press[] = {
        {1000, 2000, 0, 0, 20}, {1000, 2000, 1, 0, 40}, {1000, 2000, 0, 1, 40}, {0, 2000, 0, 0, 40},
        {1000, 500, 0, 0, 10}, {300, 2000, 0, 0, 6}};
    for (unsigned i = 0; i < sizeof(press) / sizeof(press[0]); i++) {
        reset();
        cfg.ocr_settle_press_ms = press[i].press;
        cfg.ocr_settle_max_ms = press[i].max_ms;
        job_prepares = press[i].prepares;
        job_refresh = press[i].refresh;
        fake_recognized_text = "猫";
        TEST_CHECK(run_vocr_job() == VJO_OK);
        TEST_CHECK((fake_game_request.flags >> VJO_GAME_OCR_SETTLE_MAX_SHIFT & 0xFFu) == press[i].max);
        TEST_MSG("press %d ms, max %d ms, prepares %d, refresh %d: flags 0x%08X", press[i].press, press[i].max_ms,
                 press[i].prepares, press[i].refresh, (unsigned)fake_game_request.flags);
        job_refresh = 0;
        mem_free();
    }
}

/* The whole status line the user sees for an OCR-stage error. */
static const char *ocr_error_text(void)
{
    static char memory[512]; VjoArena arena;
    vjo_arena_init(&arena, memory, sizeof(memory));
    return vjo_err_text(&arena, cache_data[0].failed_stage, &cache_data[0].err);
}
static void check_oom_message(const char *advice, const char *must, const char *must_not)
{
    const char *text = ocr_error_text();
    TEST_CHECK(cache_data[0].err.rc == VJO_E_OOM && cache_data[0].failed_stage == VJO_STAGE_OCR);
    TEST_CHECK(cache_data[0].err.detail == advice || (advice && cache_data[0].err.detail &&
                                                      !strcmp(cache_data[0].err.detail, advice)));
    TEST_CHECK(!strncmp(text, "Text recognition failed: not enough memory", 42));
    if (advice) TEST_CHECK(!strncmp(text + 42, ". ", 2) && !strcmp(text + 44, advice));
    TEST_CHECK(strlen(text) < sizeof(g_view.status)); /* shown whole in the status line */
    if (must) TEST_CHECK(strstr(text, must) != NULL);
    if (must_not) TEST_CHECK(strstr(text, must_not) == NULL);
    /* No size, reserve or fallback on screen, in any memory advice (owner 2026-10-15 04:54Z). */
    static const char *const hints[] = {"smallest", "smaller", "fallback", "fall back", "falls back", "instead",
                                        "did not fit in", "Small", "Normal", "Large", "MiB", "tier", "size"};
    for (unsigned i = 0; i < sizeof(hints) / sizeof(hints[0]); i++)
        TEST_CHECK_(strstr(text, hints[i]) == NULL, "'%s' in: %s", hints[i], text);
    TEST_MSG("%s", text);
}

static void test_vocr_oom_advice_names_the_stage(void)
{
    /* The game worker's memory check, from its reported numbers (game_ocr/main.c):
     * job = reserve 4194304 + workspace 245760 = 4440064. */
    /* job + FL10's 659456-byte block = 5099520; job + H15's 1159168 = 5599232. A job asks by
     * tiers (API 21): the weights its last check refused are its smallest tier's. No advice
     * names a model, a model file, a size, a reserve, or that a smaller one was tried (owner
     * 2026-10-15 04:54Z: no fallback hint on screen); check_oom_message checks every text. */
    const struct { int32_t free_user; uint32_t weights; const char *advice, *must, *must_not; } cases[] = {
        {4000000, 659456, OOM_GAME_NONE, "any OCR right now", "MiB"},            /* below the reserve */
        {4300000, 659456, OOM_GAME_REGION, "tighter dialogue region", "FL10"},   /* the region's job */
        {4300000, 0, OOM_GAME_REGION, "tighter dialogue region", "FL10"},        /* resident weights */
        {5200000, 1159168, OOM_GAME_SMALLEST, "for the OCR model's weights", "vocr"},
        {5000000, 1159168, OOM_GAME_SMALLEST, "tighter dialogue region", "VOCR3"},
        {5000000, 659456, OOM_GAME_SMALLEST, "for the OCR model's weights", "Select FL10"}, /* FL10's weights */
        {6000000, 659456, OOM_GAME_ALLOCATION, "although the memory check passed", "FL10"},
        {5000000, 0, OOM_GAME_ALLOCATION, "Try again", "FL10"},
        {(int32_t)0x80020001u, 659456, OOM_GAME_UNKNOWN, "could not be checked", "FL10"},
    };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        reset();
        fail_vocr_user = 1; fake_free_user = cases[i].free_user; fake_need_weights = cases[i].weights;
        TEST_CHECK(run_vocr_job() == VJO_E_OOM);
        TEST_CASE_("memory check %u", i);
        check_oom_message(cases[i].advice, cases[i].must, cases[i].must_not);
        mem_free();
    }
    /* A game OOM without the check's numbers gets no specific advice. */
    reset();
    fail_vocr_user = 1; fake_reserve = 0;
    TEST_CHECK(run_vocr_job() == VJO_E_OOM);
    check_oom_message(NULL, "not enough memory", "FL10");
    TEST_CHECK(!strcmp(ocr_error_text(), "Text recognition failed: not enough memory"));
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
        fake_model_io_error = (int32_t)cases[i].io;
        strokes(15, 39);
        TEST_CHECK(run_vocr_job() == VJO_E_OCR_MODEL);
        TEST_CHECK(!!cache_data[0].err.detail == cases[i].specific);
        char memory[256]; VjoArena arena;
        vjo_arena_init(&arena, memory, sizeof(memory));
        const char *message = vjo_err_text(&arena, VJO_STAGE_OCR, &cache_data[0].err);
        TEST_CHECK(strstr(message, cases[i].message) != NULL);
        if (cases[i].specific) TEST_CHECK(strstr(message, "prepare_ocr.py") == NULL);
        /* The default fallback repeats the job once; the fallback's error is shown. */
        TEST_CHECK(!cache_data[0].sentence && vocr_allocs == 2 && vocr_frees == 2);
        mem_free();
    }
    reset();
    cfg.vocr_tier = VJO_VOCR_TIER_SMALL; /* the smallest size: no smaller tier to repeat the job with */
    fake_model_io_error = (int32_t)0x80010013u;
    strokes(15, 39);
    TEST_CHECK(run_vocr_job() == VJO_E_OCR_MODEL && cache_data[0].err.detail && vocr_allocs == 1);
    /* Every size left out in this game: refused before submitting, and the previous job's I/O
     * error is not reused. */
    vocr_unusable = VJO_VOCR_TIERS_ALL; vocr_unusable_generation = job_generation;
    unsigned before = submits;
    TEST_CHECK(run_vocr_job() == VJO_E_OCR_MODEL && !cache_data[0].err.detail && submits == before);
    mem_free();
}

#endif

/* The subtitle strip shows the recognized text alone: its readings came only
 * from the removed online dictionaries (jpdb.io, jiten.moe). Subtitle jobs
 * stay OCR-only while the overlay is closed (no lookup, no result arena for a
 * local dictionary), and a strip's sentence is the job's text as recognized. */
static void test_subtitles_are_ocr_only(void)
{
    reset();
    cfg.ocr_mode = loaded.ocr_mode = VJO_OCR_ON_PRESS;
    subtitles = 1;
    TEST_CHECK(!want_lookup()); /* local dictionary: OCR only while the overlay is closed */
    /* a removed dictionary in config.ini (the online ones, the Hachidori relay) loads as
     * local: still OCR only */
    static const char *const olds[] = {
        "dictionary = jiten\njiten_api_key = test-key\nocr_mode = on_press\n",
        "dictionary = hachidori\nhachidori_host = 192.168.1.2\nocr_mode = on_press\n",
    };
    for (size_t i = 0; i < sizeof(olds) / sizeof(olds[0]); i++) {
        VjoConfig c;
        vjo_config_defaults(&c);
        vjo_config_parse(&c, olds[i], strlen(olds[i]));
        TEST_CHECK(c.dictionary == VJO_DICT_LOCAL && c.n_warnings == 1);
        cfg = loaded = c;
        TEST_CHECK(!want_lookup());
    }
    vjo_strip_configure(1, &cfg);
    vjo_strip_show("雨だ", VJO_STRIP_SENTENCE, 0);
    TEST_CHECK(g_view.strip_on && g_view.strip_kind == VJO_STRIP_SENTENCE && !strcmp(g_view.strip_text, "雨だ"));
    unsigned seq = g_view.strip_text_seq;
    vjo_strip_show("雨だ", VJO_STRIP_SENTENCE, 1); /* the same text: only busy changes */
    TEST_CHECK(g_view.strip_text_seq == seq && g_view.strip_busy);
    vjo_strip_show("次", VJO_STRIP_SENTENCE, 0);
    TEST_CHECK(g_view.strip_text_seq == seq + 1 && !strcmp(g_view.strip_text, "次"));
    subtitles = 0;
    TEST_CHECK(!want_lookup());
    vjo_arena_reset(&scratch);
}

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
    activate(123, "PCSA00029", VJO_GAME);
    TEST_CHECK(!game_active() && !input_blocked && job_cancelled && !g_view.open);
    job_running = 0;
    mem_free();
}

/* A PSP game's launch as shell/game.c identifies it (its own suite checks
 * how): its key (a bubble's own ID) names the game, the kernel gets its mode
 * (the emulator's static framebuffer), the PSP fallback policy is asked, and
 * Anki records the game's disc ID, its name and the psp platform. */
static void test_psp_launch_uses_its_identity(void)
{
    static const struct { const char *key, *disc, *name, *tid; } games[] = {
        {"ULJS00378", "ULJS00378", "Synthetic PSP Game", "ULJS00378"},
        {"PSPEMU005", "ULJS00385", "Other Synthetic Game", "ULJS00385"},
        {"PSPEMU006", "", "", "PSPEMU006"}};
    for (unsigned i = 0; i < sizeof(games) / sizeof(games[0]); i++) {
        reset();
        memset(&identified, 0, sizeof(identified));
        snprintf(identified.key, sizeof(identified.key), "%s", games[i].key);
        snprintf(identified.disc, sizeof(identified.disc), "%s", games[i].disc);
        snprintf(identified.name, sizeof(identified.name), "%s", games[i].name);
        identified.mode = VJO_GAME_STATIC_FB;
        identified.psp = 1;
        identify_rc = 0;
        active_mode = enabled_psp = -1;
        state.game_pid = 77;
        on_game_start();
        classify_pending();
        TEST_CHECK_(pending_pid == 0 && active_mode == VJO_GAME_STATIC_FB && enabled_psp == 1, "%s", games[i].key);
        TEST_CHECK_(!strcmp(title_id, games[i].key) && !strcmp(anki_tid, games[i].tid) && anki_psp == 1, "%s", games[i].key);
        /* No description: the name comes from the title's app parameters, as for a Vita game. */
        TEST_CHECK_(!strcmp(anki_game, games[i].name[0] ? games[i].name : "Title of PSPEMU006"), "%s", anki_game);
        on_game_exit();
        TEST_CHECK(!game_active() && !anki_tid[0] && !anki_psp);
    }
    /* Turned OFF: input passes through and no game is active. */
    reset();
    game_enabled = 0;
    identify_rc = 0;
    state.game_pid = 78;
    on_game_start();
    classify_pending();
    TEST_CHECK(!game_active() && active_mode == VJO_GAME_NONE && enabled_psp == 1);
    game_enabled = 1;
    /* A Vita game keeps its own mode and no PSP fallback; a title not readable
     * for 3 s is a game named GAME, as before. */
    reset();
    memset(&identified, 0, sizeof(identified));
    memcpy(identified.key, "PCSG00415", 10);
    identified.mode = VJO_GAME;
    state.game_pid = 79;
    on_game_start();
    classify_pending();
    TEST_CHECK(active_mode == VJO_GAME && enabled_psp == 0 && !strcmp(title_id, "PCSG00415") && !anki_psp);
    identify_rc = -1;
    reset();
    state.game_pid = 80;
    on_game_start();
    classify_pending();
    TEST_CHECK(pending_pid == 80);
    test_now += 3100000;
    classify_pending();
    TEST_CHECK(pending_pid == 0 && active_mode == VJO_GAME && !strcmp(title_id, "GAME") && enabled_psp == 0);
    mem_free();
}

static void test_failed_join_keeps_allocations(void)
{
    reset();
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

/* Anki is off by default; anki_mining reaches the overlay with each config
 * reload, nothing about it changes the OCR pipeline, and only with it on
 * does opening the overlay count the offline queue. */
static void test_anki_mining_enables_cross_in_view(void)
{
    reset();
    anki_configured = 0;
    apply_config();
    TEST_CHECK(anki_configured == 1 && !anki_on);
    view_publish(1, NULL, NULL, 0);
    TEST_CHECK(!g_view.anki_enabled);
    apply_config(); /* a reload alone: still off */
    TEST_CHECK(!anki_on);
    open_overlay();
    TEST_CHECK(anki_counts == 0);
    close_overlay();
    loaded.anki_mining = 1;
    cache_ok = 1;
    apply_config();
    TEST_CHECK(anki_on && cache_ok); /* same pipeline: cache kept */
    view_publish(1, NULL, NULL, 0);
    TEST_CHECK(g_view.anki_enabled);
    open_overlay();
    TEST_CHECK(anki_counts == 1);
    close_overlay();
    loaded.anki_mining = 0;
    apply_config();
    view_publish(1, NULL, NULL, 0);
    TEST_CHECK(!g_view.anki_enabled && !anki_on);
}

/* A card that cannot be joined keeps the worker's memory and locks too. */
static void test_anki_stop_failure_keeps_worker(void)
{
    reset();
    TEST_ASSERT(mem_alloc() == 0);
    anki_stop_rc = -1;
    TEST_CHECK(vjo_worker_stop() < 0);
    TEST_CHECK(has_result_memory() && view_lock == 81 && cmd_lock == 82);
    anki_stop_rc = 0;
    TEST_CHECK(vjo_worker_stop() == 0);
    TEST_CHECK(!has_result_memory());
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
    *out = (VjoFile){NULL, cursor_dict_size, cursor_file_read, cursor_file_close, 0}; return 0;
}
static void cursor_setup(void)
{
    reset(); cursor_build_dict();
    cursor_opens = cursor_reads = cursor_closes = 0;
    cursor_handle_live = read_action = 0;
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
    net_job(); /* as the network thread: job_idx's slot and arena, the done line, job_done */
    TEST_ASSERT(job_done);
    on_job_done();
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
    TEST_CHECK(vocr_allocs == 1 && vocr_frees == 1);
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
    /* frequency_badge too: the popup's badge follows config.ini, with the
     * same text and lookup (default on, then off, then on again) */
    TEST_CHECK(g_view.frequency_badge == 1);
    close_overlay(); loaded.frequency_badge = 0;
    open_overlay();
    TEST_CHECK(g_view.frequency_badge == 0);
    TEST_CHECK(local_text_valid && !job_running && cursor_reads == reads_before && vocr_allocs == inferences);
    close_overlay(); loaded.frequency_badge = 1;
    open_overlay();
    TEST_CHECK(g_view.frequency_badge == 1 && local_text_valid && !job_running);
    close_overlay(); loaded.japanese_only_selection = 0;
    open_overlay();
    TEST_CHECK(!local_text_valid && job_running && !job_cursor_only);
    finish_current_job();
    TEST_CHECK(!g_view.japanese_only_selection && g_view.cursor_byte == 0);
    generation = local_text_generation;
    vjo_post_cursor(1, generation); on_command(); finish_current_job();
    TEST_CHECK(g_view.cursor_byte == 1);
    close_overlay(); loaded.vocr_tier = VJO_VOCR_TIER_SMALL;
    open_overlay();
    TEST_CHECK(cfg.vocr_tier == VJO_VOCR_TIER_SMALL && job_cfg.vocr_tier == VJO_VOCR_TIER_SMALL);
    TEST_CHECK(!local_text_valid && job_running && !job_cursor_only);
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

/* The Shell's word lookup end to end with the bundled plaintext.vjdict
 * (VJO_PLAINTEXT_DIR; skipped without it) and a real VOCR line
 * (host/fixtures/word_lookup, vita_tight_v13090_154662 H15): Auto OCR
 * prepares the first word while the overlay is closed, opening shows it with
 * no further file I/O, Right (the word step) and a tap each look one word up,
 * and every lookup writes its log line. */
static unsigned real_opens;
static int (*real_open)(void *, const char *, VjoFile *);
static VjoFile real_file;
/* As platform_vita.c's local_read: the lookup log's read count. */
static int counting_read(void *ctx, uint64_t off, void *dst, size_t n)
{ vjo_dict_reads++; vjo_dict_read_bytes += (uint32_t)n; return real_file.read(real_file.ctx, off, dst, n); }
static void counting_close(void *ctx) { real_file.close(real_file.ctx); }
static int counting_open(void *ud, const char *path, VjoFile *out)
{
    real_opens++;
    if (real_open(ud, path, &real_file)) return -1;
    *out = (VjoFile){NULL, real_file.size, counting_read, counting_close, real_file.stamp};
    return 0;
}
static const char *lookup_line(const char *why)
{
    for (unsigned i = log_count > 32 ? log_count - 32 : 0; i < log_count; i++)
        if (!strncmp(log_lines[i % 32], "lookup ", 7) && strstr(log_lines[i % 32], why)) return log_lines[i % 32];
    return NULL;
}
static void test_word_lookup_with_the_bundled_dictionary(void)
{
    const char *dir = getenv("VJO_PLAINTEXT_DIR");
    PosixPlatform pp = {0};
    VjoPlatform posix;
    if (!dir) { printf("  skipped: VJO_PLAINTEXT_DIR unset\n"); return; }
    cursor_setup();
    posix_platform_init(&pp, &posix);
    real_open = posix.file_open; real_opens = 0;
    plat.file_open = counting_open;
    snprintf(loaded.local_dictionary_dir, sizeof(loaded.local_dictionary_dir), "%s", dir);
    strcpy(loaded.local_dictionaries, "plaintext.vjdict");
    VjoConfig defaults;
    vjo_config_defaults(&defaults);
    loaded.non_japanese_filter = defaults.non_japanese_filter; /* the release default */
    cfg = loaded;
    fake_recognized_text = "伊佐那社\nふふ、春休み入ってから\nほとんど会ってなかったからね。\nふたりばデート？";
    TEST_CHECK(cfg.highlight_mode == VJO_HIGHLIGHT_WORD && want_lookup());
    stable_pending = 1; auto_prefetch(); /* Auto OCR on a settled screen, overlay closed */
    TEST_ASSERT(job_running && job_lookup && ov == OV_CLOSED);
    finish_current_job();
    TEST_CHECK(cache_ok && real_opens == 1);
#if VJO_DEBUG /* the lookup lines: the debug build's (the normal build logs a failed lookup only) */
    const char *first = lookup_line("first word at 0: ");
    TEST_ASSERT_(first != NULL, "no first-word lookup log line");
    TEST_CHECK(!strncmp(first, "lookup first word at 0: 2 chars, dictionary 1/1, page 1/1, ", 59));
    TEST_CHECK(strstr(first, " ms; \"伊佐那社\" -> 伊佐 (いさ) [Daijirin4 Plain]") != NULL && strstr(first, ", rc=0, ") != NULL);
    unsigned logged_reads = 0;
    TEST_CHECK(sscanf(first + 59, "%u reads", &logged_reads) == 1 && logged_reads > 0);
#else
    TEST_CHECK(!lookup_line("first word at 0: "));
#endif
    /* Opening shows the prepared word at once: no job, no file opened. */
    open_overlay();
    TEST_CHECK(!job_running && real_opens == 1);
    TEST_ASSERT(g_view.list && g_view.list->n_entries == 1);
    TEST_CHECK(!strcmp(g_view.list->entries[0].vocab->spelling, "伊佐"));
    TEST_CHECK(g_view.list->entries[0].hl_start == 0 && g_view.list->entries[0].hl_end == (int)strlen("伊佐"));
    /* Right: the word step past 伊佐 (shell/word_navigation.h) looks up 那. */
    uint32_t generation = local_text_generation;
    vjo_post_cursor((uint32_t)strlen("伊佐"), generation); on_command(); finish_current_job();
    TEST_ASSERT(g_view.list && g_view.list->n_entries == 1);
    TEST_CHECK(g_view.cursor_byte == strlen("伊佐") && real_opens == 2);
#if VJO_DEBUG
    TEST_CHECK(lookup_line("cursor at 6: 1 chars, dictionary 1/1"));
    /* Daijirin4 lists its one 那 article under な and だ; Jiten ranks 那 only
     * read だ (#38948), so the frequency tie-break shows だ. 伊佐那社 is いさな:
     * neither reading is this line's, and both entries carry the same article. */
    TEST_CHECK(lookup_line("\"那社\" -> 那 (だ) [Daijirin4 Plain] #38948"));
#endif
    TEST_CHECK(!strcmp(g_view.list->entries[0].vocab->spelling, "那"));
    /* A tap on 会 looks up the inflected word: 会ってなかった -> 会う. */
    uint32_t at = (uint32_t)(strstr(local_text, "会って") - local_text);
    vjo_post_cursor(at, generation); on_command(); finish_current_job();
    TEST_ASSERT(g_view.list && g_view.list->n_entries == 1);
    const VjoEntry *e = &g_view.list->entries[0];
    TEST_CHECK(!strcmp(e->vocab->spelling, "会う") && e->hl_start == (int)at &&
               e->hl_end == (int)(at + strlen("会ってなかった")));
    /* the bundled dictionary's frequency: 会う's Jiten rank, its list named */
    TEST_CHECK(e->vocab->rank == 134 && g_view.list->source && !strcmp(g_view.list->source, "Jiten"));
#if VJO_DEBUG
    const char *tap = lookup_line("会ってなかった");
    TEST_ASSERT(tap != NULL);
    TEST_CHECK(strstr(tap, ": 7 chars, dictionary 1/1, page 1/1, ") &&
               strstr(tap, "-> 会う (あう) [Daijirin4 Plain] #134"));
    /* a name Jiten does not rank is logged so */
    TEST_CHECK(strstr(first, "-> 伊佐 (いさ) [Daijirin4 Plain] unranked") != NULL);
    printf("\n  %s\n  %s\n  %s\n", first, lookup_line("cursor at 6: "), tap);
#else
    TEST_CHECK(!lookup_line("会ってなかった")); /* the normal build logs no lookup's text */
#endif
    close_overlay(); mem_free();
}
#if VJO_DEBUG /* the debug build's log lines (the normal build's: test_normal_build_*) */
/* vjo_log keeps 254 bytes of a line. The lookup line's numbers come first and
 * its texts are bounded in whole characters, so even with every field at its
 * largest (4-byte characters, eight dictionaries, a 4-digit cursor, the
 * largest rank) nothing is cut: the line ends with the frequency badge. */
static void test_lookup_log_line_fits_with_worst_case_fields(void)
{
    static char text[VJO_LOCAL_MAX_TEXT];
    static char name[40 * 4 + 1];
    const char *meaning = "[Daijirin4 Plain Plain12]\ndefinition";
    VjoVocab v; VjoEntry e; VjoLocalPages pages; VjoOverlayData out;
    reset();
    memset(text, 'a', 3900); text[3900] = 0;
    for (int i = 0; i < 40; i++) strcat(text, "\xF0\xA0\xAE\xB7");
    name[0] = 0;
    for (int i = 0; i < 40; i++) strcat(name, "\xF0\xA0\xAE\xB7");
    memset(&v, 0, sizeof(v)); memset(&e, 0, sizeof(e)); memset(&pages, 0, sizeof(pages)); memset(&out, 0, sizeof(out));
    v.spelling = name; v.reading = name + 4; v.meanings = &meaning; v.n_meanings = 1;
    v.rank = 16777215; /* the format's largest */
    e.vocab = &v; e.hl_start = 3900; e.hl_end = 3900 + 32 * 4;
    pages.count = 8; pages.page[7].dict = 7;
    out.sentence = text; out.list.entries = &e; out.list.n_entries = 1; out.pages = &pages; out.page = 7;
    out.list.source = "Jiten";
    strcpy(job_cfg.local_dictionaries, "a1.vjdict,a2.vjdict,a3.vjdict,a4.vjdict,a5.vjdict,a6.vjdict,a7.vjdict,a8.vjdict");
    vjo_dict_reads = 99999; vjo_dict_read_bytes = 9999999;
    log_lookup("first word", &out, 3900, -115, (int64_t)test_now - 99999000);
    const char *line = lookup_line("first word at 3900: 32 chars, dictionary 8/8, page 8/8, 99999 reads 9999999 B, rc=-115, 99999 ms; ");
    TEST_ASSERT(line != NULL);
    TEST_CHECK_(strlen(line) <= 254 && strlen(line) > 10 && !strcmp(line + strlen(line) - 10, " #16777215"),
                "%zu bytes: %s", strlen(line), line);
    TEST_CHECK(strstr(line, " [Daijirin4 Plain Plain12] #16777215") != NULL);
    printf("\n  worst case: %zu of 254 bytes\n", strlen(line));
}
#endif
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
    TEST_CHECK(local_text_generation == generation && vocr_allocs == 1);
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
/* #81 request 4: the region's scene moving on (an icon's mask ending, an
 * effect; in the test build's console log one open overlay saw ten new scene
 * numbers while the game's input was blocked) never retires the shown text or shows a
 * message, before a lookup or during it, and a settled screen meanwhile
 * does not cancel the lookup: it is looked up in the shown text and shown.
 * Reopening on the changed screen recognizes it again, as before. A region
 * change still retires the text: the user drew another region. */
static void test_character_scene_change_keeps_the_shown_text(void)
{
    for (int during_read = 0; during_read < 3; during_read++) {
        cursor_saved_text(); open_overlay(); finish_current_job();
        uint32_t generation = local_text_generation;
        int retired = retirement_checks;
        vjo_post_cursor(3, generation);
        if (!during_read) { state.scene = state.region_scene = 8; state.region_seq++; }
        on_command();
        TEST_ASSERT(job_running && job_cursor_only);
        if (during_read == 1) read_action = 5;
        if (during_read == 2) {
            state.scene = state.region_scene = 8; state.region_seq++;
            control_tick(VJO_EV_REGION_STABLE); /* a settled screen: the lookup is not cancelled */
            TEST_CHECK(job_running && job_cursor_only && !job_cancelled);
        }
        finish_current_job();
        check_current_definition("dog", 3);
        TEST_CHECK(local_text_valid && local_text_generation == generation && vocr_allocs == 1);
        TEST_CHECK(!g_view.status_is_error && !strstr(g_view.status, "changed") && retirement_checks == retired);
        vjo_post_cursor(6, generation); on_command(); finish_current_job();
        check_current_definition("bird", 6); /* still usable */
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

/* Three generated dictionaries for paging through the actual worker. The
 * definitions name their dictionary; third.vjdict has no 犬. */
#define PAGED_DICTS 3
static unsigned char paged_dict[PAGED_DICTS][1024];
static size_t paged_size[PAGED_DICTS];
static int paged_index[PAGED_DICTS] = {0, 1, 2};
static unsigned paged_opens[PAGED_DICTS], paged_reads, paged_live;
static int paged_action; /* page + 1 to request during the next read */
static const char *const paged_names[PAGED_DICTS] = {"main.vjdict", "extra.vjdict", "third.vjdict"};
static void paged_build(int index)
{
    const char *keys[] = {"犬", "猫", "鳥"};
    const char *animals[] = {"dog", "cat", "bird"};
    unsigned char *d = paged_dict[index];
    int first = index == 2 ? 1 : 0, n = 3 - first;
    size_t off = 64 + (size_t)n * 24;
    memset(d, 0, sizeof(paged_dict[index]));
    memcpy(d, "VJDICT1\0", 8);
    cursor_u32(d + 8, 1); cursor_u32(d + 12, 64); cursor_u32(d + 16, (uint32_t)n); cursor_u32(d + 20, 24);
    cursor_u64(d + 24, 64); cursor_u64(d + 32, off);
    for (int i = 0; i < n; i++) {
        unsigned char *record = d + 64 + 24 * i;
        char meaning[32];
        snprintf(meaning, sizeof(meaning), "%.5s %s", paged_names[index], animals[first + i]);
        cursor_u64(record, off); cursor_u16(record + 16, 3);
        memcpy(d + off, keys[first + i], 3); off += 3;
        size_t payload = off;
        const char *parts[] = {keys[first + i], keys[first + i], meaning};
        for (int j = 0; j < 3; j++) {
            size_t len = strlen(parts[j]) + 1;
            memcpy(d + off, parts[j], len); off += len;
        }
        cursor_u64(record + 8, payload); cursor_u32(record + 20, (uint32_t)(off - payload));
    }
    paged_size[index] = off; cursor_u64(d + 40, off);
}
static int paged_read(void *ud, uint64_t off, void *dst, size_t n)
{
    int i = *(int *)ud;
    paged_reads++;
    TEST_CHECK(paged_live & (1u << i));
    if (paged_action) {
        int page = paged_action - 1; paged_action = 0;
        TEST_CHECK(vjo_post_page(page, local_text_generation)); on_command();
    }
    if (off > paged_size[i] || n > paged_size[i] - off) return -1;
    memcpy(dst, paged_dict[i] + off, n); return 0;
}
static void paged_close(void *ud)
{ int i = *(int *)ud; TEST_CHECK(paged_live & (1u << i)); paged_live &= ~(1u << i); }
static int paged_open(void *ud, const char *path, VjoFile *out)
{
    (void)ud;
    for (int i = 0; i < PAGED_DICTS; i++) {
        if (!strstr(path, paged_names[i])) continue;
        TEST_CHECK(!(paged_live & (1u << i)));
        paged_live |= 1u << i; paged_opens[i]++;
        *out = (VjoFile){&paged_index[i], paged_size[i], paged_read, paged_close, 0};
        return 0;
    }
    return -1;
}
static void paged_setup(int results)
{
    cursor_setup();
    for (int i = 0; i < PAGED_DICTS; i++) paged_build(i);
    memset(paged_opens, 0, sizeof(paged_opens));
    paged_reads = paged_live = 0; paged_action = 0;
    strcpy(cfg.local_dictionaries, "main.vjdict,extra.vjdict,third.vjdict");
    strcpy(loaded.local_dictionaries, cfg.local_dictionaries);
    cfg.local_dictionary_results = loaded.local_dictionary_results = results;
    plat.file_open = paged_open;
}
static void check_page(const char *meaning, uint32_t cursor, int page, int pages)
{
    TEST_ASSERT(g_view.list != NULL && g_view.list->n_entries == 1);
    TEST_CHECK(strstr(g_view.list->entries[0].text, meaning) != NULL);
    TEST_MSG("shown: %s; expected %s", g_view.list->entries[0].text, meaning);
    TEST_CHECK(g_view.local_cursor_mode && g_view.cursor_byte == cursor);
    TEST_CHECK(g_view.dict_page == page && g_view.dict_pages == pages);
    TEST_MSG("page %d/%d, expected %d/%d", g_view.dict_page, g_view.dict_pages, page, pages);
    TEST_CHECK(!paged_live && !held_cmd && !held_view && !job_running);
}
static unsigned paged_total_opens(void) { return paged_opens[0] + paged_opens[1] + paged_opens[2]; }

static void test_dictionary_pages_read_one_entry_each(void)
{
    paged_setup(3);
    open_overlay(); finish_current_job();
    check_page("main. cat", 0, 0, 3);
    TEST_CHECK(paged_opens[0] == 1 && paged_opens[1] == 1 && paged_opens[2] == 1);
    TEST_CHECK(results[0].size == 64 * 1024 && !results[1].size && vocr_allocs == 1);
    unsigned arenas = user_allocs + heap_allocs;
    size_t first_peak = results[0].peak;
    uint32_t generation = local_text_generation;
    /* Down: only extra.vjdict is opened, at its located payload. */
    TEST_CHECK(vjo_post_page(1, generation));
    TEST_CHECK(pending_cmd == VJO_CMD_CURSOR && g_view.list == &local_header);
    TEST_CHECK(g_view.dict_page == 1 && g_view.dict_pages == 3 && !strcmp(g_view.status, "Looking up words…"));
    on_command(); TEST_ASSERT(job_running && job_cursor_only && job_page == 1);
    unsigned reads_before = paged_reads;
    finish_current_job();
    check_page("extra cat", 0, 1, 3);
    TEST_CHECK(paged_opens[0] == 1 && paged_opens[1] == 2 && paged_opens[2] == 1);
    TEST_CHECK(paged_reads - reads_before == 2); /* its header, then one payload */
    /* Ignored: the current page, out of range, a stale text. */
    TEST_CHECK(!vjo_post_page(1, generation) && !vjo_post_page(3, generation));
    TEST_CHECK(!vjo_post_page(-1, generation) && !vjo_post_page(2, generation + 1));
    TEST_CHECK(pending_cmd == VJO_CMD_NONE && !job_running);
    /* Rapid presses coalesce into the newest page. */
    TEST_CHECK(vjo_post_page(2, generation) && vjo_post_page(0, generation));
    on_command(); TEST_ASSERT(job_page == 0); finish_current_job();
    check_page("main. cat", 0, 0, 3);
    TEST_CHECK(vjo_post_page(2, generation)); on_command(); finish_current_job();
    check_page("third cat", 0, 2, 3);
    /* A press during a page read supersedes that page. */
    TEST_CHECK(vjo_post_page(1, generation)); on_command(); finish_current_job();
    check_page("extra cat", 0, 1, 3);
    TEST_CHECK(vjo_post_page(2, generation)); on_command();
    paged_action = 0 + 1; /* while reading page 2, ask for page 0 */
    finish_current_job();
    TEST_ASSERT(job_running && job_cursor_only && job_page == 0);
    finish_current_job(); check_page("main. cat", 0, 0, 3);
    TEST_CHECK(vjo_post_page(1, generation)); on_command(); finish_current_job();
    check_page("extra cat", 0, 1, 3);
    /* Paging never reallocates, recaptures, or needs lookup scratch. */
    TEST_CHECK(user_allocs + heap_allocs == arenas && vocr_allocs == 1);
    TEST_CHECK(results[0].peak <= first_peak && results[0].size == 64 * 1024 && !results[1].size);
    TEST_CHECK(local_text_generation == generation);

    /* A new character starts at its best match; pages come with its result. */
    vjo_post_cursor(3, generation);
    TEST_CHECK(g_view.dict_page == 0 && g_view.dict_pages == 0 && !vjo_post_page(1, generation));
    on_command(); finish_current_job();
    check_page("main. dog", 3, 0, 2); /* third.vjdict has no 犬 */
    TEST_CHECK(vjo_post_page(1, generation)); on_command(); finish_current_job();
    check_page("extra dog", 3, 1, 2);
    /* Reopening unchanged dialogue shows the same page without reads. */
    close_overlay(); reads_before = paged_reads; unsigned opens_before = paged_total_opens();
    open_overlay();
    TEST_CHECK(!job_running && paged_reads == reads_before && paged_total_opens() == opens_before);
    check_page("extra dog", 3, 1, 2);
    /* Closing during a page read drops it; reopening reads that page again. */
    TEST_CHECK(vjo_post_page(0, generation)); on_command();
    TEST_ASSERT(job_running); close_overlay(); TEST_CHECK(job_cancelled);
    job_rc = run_cursor_job(&results[0], &cache_data[0]); job_done = 1; on_job_done();
    TEST_CHECK(!job_running && !g_view.list);
    open_overlay(); TEST_ASSERT(job_running && job_cursor_only && job_page == 0);
    finish_current_job(); check_page("main. dog", 3, 0, 2);
    TEST_CHECK(vocr_allocs == 1 && user_allocs + heap_allocs == arenas);
    close_overlay(); mem_free();
}

static void test_dictionary_count_change_relooks_up_without_ocr(void)
{
    paged_setup(3);
    open_overlay(); finish_current_job();
    check_page("main. cat", 0, 0, 3);
    uint32_t generation = local_text_generation;
    TEST_CHECK(vjo_post_page(2, generation)); on_command(); finish_current_job();
    check_page("third cat", 0, 2, 3);
    /* Settings → Dictionaries shown 1: the same text, looked up again. */
    close_overlay(); loaded.local_dictionary_results = 1;
    open_overlay();
    TEST_CHECK(local_text_valid && local_text_generation == generation);
    TEST_ASSERT(job_running && job_cursor_only && job_page == 0);
    unsigned opens = paged_total_opens();
    finish_current_job();
    check_page("main. cat", 0, 0, 1);
    TEST_CHECK(!cache_data[0].pages && paged_total_opens() - opens == 3);
    TEST_CHECK(!vjo_post_page(1, generation) && pending_cmd == VJO_CMD_NONE);
    /* Back to 2: two pages again, still without OCR or recapture. */
    close_overlay(); loaded.local_dictionary_results = 2;
    open_overlay(); finish_current_job();
    check_page("main. cat", 0, 0, 2);
    TEST_CHECK(vocr_allocs == 1 && local_text_generation == generation);
    /* An unchanged count keeps the cached page. */
    TEST_CHECK(vjo_post_page(1, generation)); on_command(); finish_current_job();
    close_overlay(); open_overlay();
    TEST_CHECK(!job_running); check_page("extra cat", 0, 1, 2);
    /* A prepared lookup made with the old count is replaced on opening. */
    close_overlay(); retire_local_text(1); cache_ok = 0; /* as a region change */
    stable_pending = 1; auto_prefetch();
    TEST_ASSERT(job_running && job_lookup && !job_cursor_only);
    TEST_CHECK(run_job(&results[0], &cache_data[0]) == VJO_OK);
    loaded.local_dictionary_results = 3;
    open_overlay(); /* reloads config while the prepared lookup finishes */
    job_done = 1; on_job_done();
    TEST_ASSERT(job_running && job_cursor_only);
    finish_current_job(); check_page("main. cat", 0, 0, 3);
    /* One finishing after a reload (e.g. subtitles on) with another count is
     * not cached for the next opening. */
    close_overlay(); retire_local_text(1); cache_ok = 0;
    stable_pending = 1; auto_prefetch();
    TEST_ASSERT(job_running && job_lookup && job_cfg.local_dictionary_results == 3);
    TEST_CHECK(run_job(&results[0], &cache_data[0]) == VJO_OK);
    loaded.local_dictionary_results = 2; apply_config();
    job_done = 1; on_job_done();
    TEST_CHECK(!job_running && !cache_ok);
    open_overlay(); TEST_ASSERT(job_running && job_cursor_only);
    finish_current_job(); check_page("main. cat", 0, 0, 2);
    TEST_CHECK(vocr_allocs == 3);
    close_overlay(); mem_free();
}

static void test_dictionary_page_after_replaced_file_locates_again(void)
{
    paged_setup(3);
    open_overlay(); finish_current_job();
    check_page("main. cat", 0, 0, 3);
    uint32_t generation = local_text_generation;
    /* extra.vjdict is replaced by a file of another size between pages. */
    paged_size[1] += 1; cursor_u64(paged_dict[1] + 40, paged_size[1]);
    TEST_CHECK(vjo_post_page(1, generation)); on_command(); finish_current_job();
    TEST_ASSERT(g_view.list && !g_view.list->n_entries && !strcmp(g_view.list->header, "猫犬鳥"));
    TEST_CHECK(g_view.status_is_error && strstr(g_view.status, "changed") != NULL);
    TEST_CHECK(g_view.dict_page == 1 && g_view.dict_pages == 3 && !paged_live);
    /* The next page locates every dictionary again instead of trusting old offsets. */
    unsigned opens = paged_total_opens();
    TEST_CHECK(vjo_post_page(2, generation)); on_command(); finish_current_job();
    TEST_CHECK(paged_total_opens() - opens == 4); /* three to locate, then the page */
    check_page("third cat", 0, 2, 3);
    TEST_CHECK(vjo_post_page(1, generation)); on_command(); finish_current_job();
    check_page("extra cat", 0, 1, 3);
    TEST_CHECK(vocr_allocs == 1 && local_text_generation == generation);
    close_overlay(); mem_free();
}

static void test_dictionary_pages_oom_keeps_text_and_disables_paging(void)
{
    paged_setup(3);
    fail_alloc = 1;
    open_overlay(); finish_current_job();
    TEST_ASSERT(g_view.list && !g_view.list->n_entries && !strcmp(g_view.list->header, "猫犬鳥"));
    TEST_CHECK(g_view.status_is_error && strstr(g_view.status, "Shell UI memory"));
    TEST_CHECK(!g_view.dict_pages && !paged_total_opens() && !has_result_memory());
    TEST_CHECK(!vjo_post_page(1, local_text_generation));
    fail_alloc = 0;
    vjo_post_cursor(3, local_text_generation); on_command(); finish_current_job();
    check_page("main. dog", 3, 0, 2);
    close_overlay(); mem_free();
}

static void test_shell_result_oom_preserves_recognition(void)
{
    reset();
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
    phases_checked = 1;
    for (int i = 0; i < 50; i++) {
        start_job("local phase check", 0);
        TEST_CHECK(!has_result_memory() && g_view.list == NULL);
        TEST_CHECK(run_job(&results[job_idx], &cache_data[job_idx]) == VJO_OK);
        TEST_CHECK(!has_result_memory() && !results[0].size && !results[1].size);
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

static void test_local_lookup_single_arena(void)
{
    cursor_setup(); /* This allocation gate needs an actual selectable sentence. */
    phases_checked = 1;
    ov = OV_OPEN;
    start_job("local lookup", 1);
    TEST_CHECK(job_idx == 0 && !g_view.list);
    TEST_CHECK(run_job(&results[job_idx], &cache_data[job_idx]) == VJO_OK);
    TEST_CHECK(has_result_memory() && results[0].size == 64 * 1024);
    TEST_CHECK(!results[1].base && !results[1].size);
    on_job_done();
    TEST_CHECK(cache_ok && active == 0 && g_view.list == &cache_data[0].list);
    start_job("local OCR only replaces cached lookup", 0);
    TEST_CHECK(job_idx == 0 && !has_result_memory() && !g_view.list && !cache_ok);
    TEST_CHECK(run_job(&results[job_idx], &cache_data[job_idx]) == VJO_OK);
    TEST_CHECK(!has_result_memory());
    job_running = 0;
    phases_checked = 0;
    mem_free();
}

#endif

/* ---- the Shell worker's one result arena (audit-overlay-shell-slim-1's A1, A1b, A4, A5) ---- */
static void next_scene(uint32_t scene);
static void check_shown(const char *text, const char *gloss, uint32_t cursor);
#ifdef VJO_WITH_VOCR
static unsigned audit_live(void) { return heap_allocs + user_allocs - heap_frees - user_frees; }

/* A1: a game exit while the first open's lookup still runs. The job owns the
 * arena it took after OCR: the exit never frees it under the job; once the job
 * ends, with no game active, the same control tick frees it. */
static void test_audit_exit_during_lookup_frees_the_arena_after_the_job(void)
{
    cursor_setup();
    open_overlay();
    TEST_ASSERT(job_running && job_lookup && !job_cursor_only && !job_refresh && job_idx == 0);
    TEST_CHECK(!has_result_memory()); /* opening takes no memory */
    /* the network thread: OCR in the game, then the lookup's arena */
    job_rc = run_job(&results[job_idx], &cache_data[job_idx]);
    TEST_CHECK(job_rc == VJO_OK && has_result_memory() && results[0].size == LOCAL_RESULT_ARENA_SIZE);
    on_game_exit(); /* before the control thread saw job_done */
    TEST_CHECK(has_result_memory() && audit_live() == 1); /* still the job's */
    job_done = 1;
    control_tick(0); /* the job's end, then the idle check */
    TEST_CHECK(!job_running && !has_result_memory() && audit_live() == 0);
    TEST_CHECK(!held_cmd && !held_view);
}

/* A1b: the same with a cursor job (a word lookup in the shown text). */
static void test_audit_exit_during_cursor_job_frees_the_arena_after_the_job(void)
{
    cursor_setup();
    open_overlay(); finish_current_job();
    TEST_ASSERT(has_result_memory() && cache_ok && active == 0);
    vjo_post_cursor(3, local_text_generation);
    on_command();
    TEST_ASSERT(job_running && job_cursor_only);
    job_rc = run_cursor_job(&results[0], &cache_data[0]);
    on_game_exit();
    TEST_CHECK(has_result_memory() && audit_live() == 1);
    job_done = 1;
    control_tick(0);
    TEST_CHECK(!job_running && !has_result_memory() && audit_live() == 0);
    TEST_CHECK(!held_cmd && !held_view);
}

/* A4: a refresh (job_idx 1) has no arena of its own: results[1] stays {NULL, 0},
 * the job allocates nothing, and the shown entry's arena is left as it was. */
static void test_audit_refresh_has_no_arena(void)
{
    cursor_setup();
    open_overlay(); finish_current_job();
    TEST_ASSERT(has_result_memory() && active == 0 && cache_ok);
    uint8_t *base = results[0].base;
    size_t used = results[0].used;
    unsigned allocs = heap_allocs + user_allocs;
    next_scene(8);
    control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh && job_idx == 1 && !job_lookup);
    TEST_CHECK(!results[1].base && !results[1].size);
    job_rc = run_job(&results[1], &cache_data[1]);
    TEST_CHECK(job_rc == VJO_OK && !results[1].base && !results[1].size && !results[1].used && !results[1].peak);
    TEST_CHECK(results[0].base == base && results[0].used == used && heap_allocs + user_allocs == allocs);
    TEST_CHECK(cache_data[1].ocr_text != NULL && !cache_data[1].list.n_entries && !cache_data[1].pages);
    job_done = 1;
    on_job_done();
    TEST_CHECK(logged("refresh rc=0: same text") != NULL && has_result_memory() && results[0].base == base);
    TEST_CHECK(!held_cmd && !held_view);
    close_overlay(); mem_free();
}
#endif

#ifdef VJO_WITH_VOCR
/* A5: on_job_done's publish path when the overlay's thread posts a cursor move
 * just before on_job_done takes cmd_lock (vjo_post_cursor runs on Paf's thread,
 * under cmd_lock): the newer lookup runs, with cmd_lock released first. The
 * lock stub fires cursor_race at that acquisition. */
static void test_audit_cursor_posted_while_publishing(void)
{
    cursor_setup();
    open_overlay();
    TEST_ASSERT(job_running && job_lookup && !job_cursor_only);
    job_rc = run_job(&results[0], &cache_data[0]);
    job_done = 1;
    uint32_t seq = local_cursor_seq;
    race_on_lock = 1; /* the first cmd_lock taken from here: on_job_done's publish */
    on_job_done();
    TEST_CHECK(race_on_lock == 0 && local_cursor_seq != seq);
    TEST_CHECK(!held_cmd && !held_view);
    TEST_ASSERT(job_running && job_cursor_only && job_cursor_byte == 3);
    finish_current_job();
    check_shown("猫犬鳥", "dog", 3);
    TEST_CHECK(!held_cmd && !held_view);
    close_overlay(); mem_free();
}
#endif

/* The boot guard's "ok": the control thread alive for a minute, marked once;
 * a BUSY kernel (its own uptime shorter) is retried a bounded number of
 * times, five seconds apart. */
static void test_boot_guard_ok_after_a_stable_minute(void)
{
    memset(guard_marks, 0, sizeof(guard_marks));
    guard_mark_rc = 0;
    test_now = 1000000;
    ctl_started_us = now_us();
    boot_ok_attempts = 0;
    for (int s = 0; s < 60; s++) {
        boot_guard_ok();
        test_now += 1000000;
    }
    TEST_CHECK(guard_marks[VJO_BOOT_STAGE_OK] == 0); /* 59 s: not yet */
    boot_guard_ok();                                 /* 60 s */
    TEST_CHECK(guard_marks[VJO_BOOT_STAGE_OK] == 1);
    test_now += 3600LL * 1000000;
    boot_guard_ok();
    boot_guard_ok();
    TEST_CHECK(guard_marks[VJO_BOOT_STAGE_OK] == 1); /* once per boot */

    /* refused: retried every 5 s, at most BOOT_OK_ATTEMPTS times */
    memset(guard_marks, 0, sizeof(guard_marks));
    guard_mark_rc = -1;
    ctl_started_us = now_us();
    boot_ok_attempts = 0;
    test_now += 60LL * 1000000;
    for (int s = 0; s < 200; s++) {
        boot_guard_ok();
        test_now += 1000000;
    }
    TEST_CHECK_(guard_marks[VJO_BOOT_STAGE_OK] == BOOT_OK_ATTEMPTS, "%d attempts", guard_marks[VJO_BOOT_STAGE_OK]);
    guard_mark_rc = 0;
    boot_guard_ok();
    TEST_CHECK(guard_marks[VJO_BOOT_STAGE_OK] == BOOT_OK_ATTEMPTS); /* gave up */

    /* refused twice, then accepted */
    memset(guard_marks, 0, sizeof(guard_marks));
    guard_mark_rc = -1;
    ctl_started_us = now_us();
    boot_ok_attempts = 0;
    test_now += 61LL * 1000000;
    boot_guard_ok();
    test_now += 5LL * 1000000;
    boot_guard_ok();
    guard_mark_rc = 0;
    test_now += 5LL * 1000000;
    boot_guard_ok();
    boot_guard_ok();
    TEST_CHECK(guard_marks[VJO_BOOT_STAGE_OK] == 3);
    TEST_CHECK(boot_ok_attempts == -1);
}

/* The ledger's static table must describe this code: the result memory is the
 * local arena, LOCAL_RESULT_ARENA_SIZE (the only one: the overlay is offline),
 * and every allocation is counted in and out of the pool it comes from. */
static void test_ledger_counts_result_memory_against_the_budget(void)
{
    TEST_CHECK(LOCAL_RESULT_ARENA_SIZE == VJO_BUDGET_PAF_RESULTS_LOCAL);
    TEST_CHECK(VJO_WORST_SHELL_PAF <= VJO_LIMIT_SHELL_PAF);
    TEST_CHECK(VJO_WORST_SHELL_PAGES <= VJO_LIMIT_SHELL_PAGES);
    vjo_ledger_shell_init();
    TEST_CHECK(vjo_ledger_pool_current(&g_shell_ledger, VJO_LEDGER_POOL_SHELL_PAGES) ==
               VJO_BUDGET_SHELL_MODULE_PAGES + VJO_BUDGET_WORKER_IN_SHELL_PAGES);
    TEST_CHECK(VJO_BUDGET_WORKER_IN_SHELL_PAGES == 0); /* the worker is registered per game, not loaded in SceShell */
    TEST_CHECK(vjo_ledger_pool_current(&g_shell_ledger, VJO_LEDGER_POOL_SHELL_PAF) == VJO_BUDGET_PAF_PLUGIN);
    mem_free();
    fail_alloc = 0;
    cfg = loaded;
#ifdef VJO_PAF_ALLOC
    TEST_CHECK(mem_alloc() == 0);
    TEST_CHECK(vjo_ledger_current(&g_shell_ledger, VJO_SL_PAF_RESULTS) == VJO_BUDGET_PAF_RESULTS_LOCAL);
    TEST_CHECK(vjo_ledger_pool_current(&g_shell_ledger, VJO_LEDGER_POOL_SHELL_PAF) == VJO_BUDGET_PAF_PLUGIN + VJO_BUDGET_PAF_RESULTS_LOCAL);
    TEST_CHECK(vjo_ledger_pool_current(&g_shell_ledger, VJO_LEDGER_POOL_SHELL_PAGES) ==
               VJO_BUDGET_SHELL_MODULE_PAGES + VJO_BUDGET_WORKER_IN_SHELL_PAGES);
    mem_free();
    TEST_CHECK(vjo_ledger_current(&g_shell_ledger, VJO_SL_PAF_RESULTS) == 0);
    TEST_CHECK(vjo_ledger_peak(&g_shell_ledger, VJO_SL_PAF_RESULTS) == VJO_BUDGET_PAF_RESULTS_LOCAL);
    TEST_CHECK(vjo_ledger_pool_current(&g_shell_ledger, VJO_LEDGER_POOL_SHELL_PAF) == 8192 &&
               vjo_ledger_pool_peak(&g_shell_ledger, VJO_LEDGER_POOL_SHELL_PAF) == 73728); /* 8,192 + 65,536 */
#else
    TEST_CHECK(mem_alloc() == 0);
    TEST_CHECK(vjo_ledger_current(&g_shell_ledger, VJO_SL_USERRW_RESULTS) == VJO_BUDGET_PAF_RESULTS_LOCAL);
    TEST_CHECK(vjo_ledger_pool_current(&g_shell_ledger, VJO_LEDGER_POOL_SHELL_PAF) == VJO_BUDGET_PAF_PLUGIN);
    mem_free();
    TEST_CHECK(vjo_ledger_current(&g_shell_ledger, VJO_SL_USERRW_RESULTS) == 0);
#endif
    loaded.dictionary = VJO_DICT_LOCAL;
    cfg = loaded;
    vjo_ledger_shell_log("test");
    TEST_CHECK(!open_batch); /* the ledger table's batch is closed: no pointer into this stack frame */
}

#if VJO_DEBUG /* a self-test test: the debug build's */
/* The self-test, while armed, runs OCR automatically whatever config.ini says,
 * and keeps the user's region (#74: the API 18 test forced the full screen and,
 * never finishing after a failure, kept forcing it). Its full-screen stage asks
 * for the whole frame itself. */
static void test_selftest_keeps_the_user_region_and_auto_ocr(void)
{
    saved_region = (VjoRect){100, 200, 30000, 8000};
    strcpy(title_id, "PCSG00415");
    loaded.ocr_mode = VJO_OCR_ON_PRESS;
    selftest_armed = 1; selftest_done = 0;
    apply_config();
    TEST_CHECK(cfg.ocr_mode == VJO_OCR_AUTO);
    regions_set = 0;
    push_region();
    TEST_CHECK(regions_set == 1 && !last_region_full && game_region.w == 30000);
    selftest_armed = 0;
    apply_config();
    TEST_CHECK(cfg.ocr_mode == VJO_OCR_ON_PRESS);
    push_region();
    TEST_CHECK(regions_set == 2 && !last_region_full);
    saved_region.w = 0;
    push_region();
    TEST_CHECK(last_region_full && !game_region.w);
    loaded.ocr_mode = VJO_OCR_AUTO;
    apply_config();
}
#endif

#ifdef VJO_WITH_VOCR
#if VJO_DEBUG /* a self-test test: the debug build's */
/* The self-test's job (job_selftest) runs every stage in turn, each with its
 * record, a failed stage included, and then ends the test. It shows nothing:
 * neither its text nor a lookup reaches the overlay's text or result. */
static void test_selftest_stages_run_in_one_job_and_finish(void)
{
    reset();
    selftest_armed = 1; selftest_done = 0; selftest_stage_count = 5;
    selftest_begins = selftest_ends = submits = 0;
    fake_recognized_text = "猫";
    job_selftest = 1;
    TEST_CHECK(run_vocr_job() == VJO_OK);
    TEST_CHECK(selftest_begins == 5 && selftest_ends == 5 && submits == 5 && selftest_done);
    /* The stage's own flags, and the job policy every stage carries (API 20):
     * the default 150 ms stable-text wait (15 x 10 ms), its 2 s limit (40 x 50
     * ms); blank lines are kept by default. */
    const uint32_t stage_bits = VJO_GAME_OCR_F_FULL | VJO_GAME_OCR_F_FORCE_FALLBACK | VJO_GAME_OCR_F_PROBE;
    const uint32_t policy = 15u << VJO_GAME_OCR_SETTLE_SHIFT | 40u << VJO_GAME_OCR_SETTLE_MAX_SHIFT |
                            2u << VJO_GAME_OCR_LM_SHIFT |     /* the 1 MB LM at most */
                            8u << VJO_GAME_OCR_CASCADE_SHIFT | /* the cascade within 2 s */
                            VJO_GAME_OCR_REFINE_PSP << VJO_GAME_OCR_REFINE_SHIFT; /* refined in PSP games */
    TEST_CHECK((selftest_flags_seen[0] & stage_bits) == VJO_GAME_OCR_F_FORCE_FALLBACK &&
               (selftest_flags_seen[1] & stage_bits) == 0 &&
               (selftest_flags_seen[2] & stage_bits) == VJO_GAME_OCR_F_FULL &&
               (selftest_flags_seen[3] & stage_bits) == VJO_GAME_OCR_F_PROBE &&
               (selftest_flags_seen[4] & stage_bits) == 0);
    for (unsigned i = 0; i < 5; i++) {
        /* API 22: the default ocr_threads = 3 (two helpers), the default ocr_boost_clock = on (444 MHz while
         * recognizing), no line cache in any stage; textbox2 (the last) serially, to compare with textbox,
         * with the clock kept; the begin record says so */
        const uint32_t boost = VJO_GAME_OCR_S_BOOST | VJO_GAME_OCR_S_NOCACHE;
        TEST_CHECK(selftest_speed_seen[i] == (i == 4 ? boost : 2u | boost));
        TEST_CHECK(selftest_begin_speed[i] == selftest_speed_seen[i]);
        TEST_CHECK((selftest_flags_seen[i] & ~(stage_bits | VJO_GAME_OCR_F_SETTLE_FIRST)) == policy);
        TEST_MSG("stage %u flags 0x%08X", i, (unsigned)selftest_flags_seen[i]);
    }
    TEST_CHECK(!cache_data[0].sentence && !cache_data[0].ocr_text && !local_text_valid && !job_text_ready);
    TEST_CHECK(!text_generation); /* nor is its text the screen's for a later self-test */
    TEST_CHECK(!vjo_selftest_armed() && !vjo_selftest_stages()); /* done: the next job is a normal one */
    mem_free();
    /* A failing stage still gets its record and the test still finishes. */
    reset();
    selftest_armed = 1; selftest_done = 0; selftest_begins = selftest_ends = submits = 0;
    fail_vocr_user = 1; fake_free_user = 1000; fake_need_weights = 659456; fake_reserve = 4194304;
    job_selftest = 1;
    TEST_CHECK(run_vocr_job() == VJO_E_OOM);
    TEST_CHECK(selftest_ends == 5 && selftest_rcs[0] == VJO_E_OOM && selftest_rcs[1] == VJO_E_OOM && selftest_done);
    fail_vocr_user = 0; fake_recognized_text = NULL; selftest_armed = 0; selftest_stage_count = 0;
    mem_free();
}
#endif

#if VJO_DEBUG /* a self-test test: the debug build's */
/* #81's self-test began 2 s after the game started, on its opening notice: the
 * three region stages found no line (the full screen five, above the region).
 * The stages now wait for text in the region: a first stage with no line, or
 * whose screen changed under every read, ends the job with its own result and
 * the stages run on a later settled screen; after VJO_SELFTEST_WAITS such
 * screens they run anyway (their records then say there was no text). */
static void test_selftest_waits_for_text_in_the_region(void)
{
    /* The stage's own bits (a job's policy may ride in the other flags). */
    const uint32_t stage = VJO_GAME_OCR_F_FULL | VJO_GAME_OCR_F_FORCE_FALLBACK | VJO_GAME_OCR_F_PROBE;
    reset();
    selftest_armed = 1; selftest_done = 0; selftest_stage_count = 5;
    selftest_begins = selftest_ends = submits = 0;
    job_selftest = 1;
    TEST_CHECK(run_vocr_job() == VJO_OK); /* an empty region: no line */
    TEST_CHECK(selftest_begins == 1 && !selftest_ends && submits == 1 && selftest_waits == 1 && !selftest_done);
    TEST_CHECK(vjo_selftest_stages() == 5 && !cache_data[0].sentence);
    TEST_CHECK((selftest_flags_seen[0] & stage) == VJO_GAME_OCR_F_FORCE_FALLBACK);
    mem_free();
    fake_screen_changes = 1; /* typing text: the screen changed under every read */
    TEST_CHECK(run_vocr_job() == VJO_E_SCREEN_CHANGED && selftest_waits == 2 && !selftest_ends && !selftest_done);
    mem_free();
    fake_recognized_text = "猫"; /* dialogue in the region: every stage, then done */
    TEST_CHECK(run_vocr_job() == VJO_OK && !cache_data[0].sentence);
    TEST_CHECK(selftest_begins == 2 + 5 && selftest_ends == 5 && submits == 2 + 5 && selftest_done);
    TEST_CHECK((selftest_flags_seen[2] & stage) == VJO_GAME_OCR_F_FORCE_FALLBACK &&
               (selftest_flags_seen[4] & stage) == VJO_GAME_OCR_F_FULL);
    mem_free();
    /* Bounded: after VJO_SELFTEST_WAITS empty screens the stages run on the next anyway. */
    reset();
    selftest_armed = 1; selftest_done = 0; selftest_stage_count = 5;
    selftest_begins = selftest_ends = submits = 0;
    job_selftest = 1;
    for (int i = 0; i < VJO_SELFTEST_WAITS; i++) {
        TEST_CHECK(run_vocr_job() == VJO_OK && !selftest_done);
        mem_free();
    }
    TEST_CHECK(selftest_waits == VJO_SELFTEST_WAITS && !selftest_ends);
    TEST_CHECK(run_vocr_job() == VJO_OK && selftest_ends == 5 && selftest_done && selftest_rcs[0] == VJO_OK);
    selftest_armed = 0; selftest_stage_count = 0; job_selftest = 0;
    mem_free();
}
#endif

/* Nothing about a changed screen reaches the user (#81 request 4). */
static void check_quiet(void)
{
    TEST_CHECK(!g_view.status_is_error && !strstr(g_view.status, "changed") && !strstr(g_view.status, "reopen"));
    TEST_CHECK(g_view.strip_kind != VJO_STRIP_ERROR && !strstr(g_view.strip_text, "changed"));
}

/* #81: 4 of 21 console jobs read a screen that changed under every read
 * (VJO_E_SCREEN_CHANGED) and were dropped. They still are, quietly and logged;
 * the next try starts at once, from the second in a row 250 ms later,
 * doubling to 4 s, and a job that reads its screen ends the run. Auto (the
 * overlay closed, subtitles on) and an open overlay waiting for its text
 * ("Recognizing…" all along) follow the same pace; closing the overlay
 * during a wait leaves the retry to Auto. The error text no longer exists. */
static void test_screen_changed_is_retried_quietly_at_a_bounded_pace(void)
{
    char memory[64]; VjoArena arena;
    VjoErr err = {.rc = VJO_E_SCREEN_CHANGED};
    vjo_arena_init(&arena, memory, sizeof(memory));
    TEST_CHECK(!strcmp(vjo_err_text(&arena, VJO_STAGE_OCR, &err), ""));

    cursor_setup();
    fake_screen_changes = 8;
    const int64_t waits[] = {0, 250, 500, 1000, 2000, 4000, 4000, 4000}; /* ms, before tries 2..9 */
    stable_pending = 1; control_tick(0);
    for (int i = 0; i < 8; i++) {
        char line[128];
        TEST_ASSERT(job_running && !job_refresh);
        finish_current_job();
        snprintf(line, sizeof(line), "job dropped: the screen changed under it (%d in a row), "
                 "next try in %u ms", i + 1, (unsigned)waits[i]);
        TEST_CHECK_(DLOGGED(line), "%s", line);
        TEST_CHECK(!job_running && stable_pending);
        check_quiet();
        if (waits[i]) {
            test_now += waits[i] * 1000 - 1000;
            control_tick(0);
            TEST_CHECK(!job_running); /* not yet */
            test_now += 1000;
        }
        control_tick(0);
    }
    TEST_ASSERT(job_running);
    finish_current_job(); /* this one reads its screen */
    TEST_CHECK(local_text_valid && !strcmp(local_text, "猫犬鳥") && !screen_changed_streak && !screen_retry_until);
    TEST_CHECK(vocr_allocs == 9 && vocr_frees == 9);
    check_quiet();
    mem_free();

    /* The subtitles: nothing reaches the strip; the retry keeps its 1 s pace. */
    cursor_setup();
    subtitles = 1;
    fake_screen_changes = 1;
    stable_pending = 1; control_tick(0);
    TEST_ASSERT(job_running && g_view.strip_busy);
    finish_current_job();
    TEST_CHECK(!g_view.strip_busy && !g_view.strip_text[0]);
    check_quiet();
    control_tick(0);
    TEST_CHECK(!job_running); /* SUBTITLE_GAP_US after the last job */
    test_now += SUBTITLE_GAP_US; control_tick(0);
    TEST_ASSERT(job_running);
    finish_current_job();
    TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !strcmp(g_view.strip_text, "猫犬鳥"));
    subtitles = 0;
    mem_free();

    /* Another game (or the same one restarted) starts the pace over. */
    cursor_setup();
    fake_screen_changes = 2;
    stable_pending = 1; control_tick(0); finish_current_job();
    control_tick(0); finish_current_job();
    TEST_CHECK(screen_changed_streak == 2 && screen_retry_until > now_us());
    invalidate_game_work();
    TEST_CHECK(!screen_changed_streak && !screen_retry_until);
    stable_pending = 1; control_tick(0);
    TEST_ASSERT(job_running); /* at once */
    finish_current_job();
    mem_free();
    /* A job of the previous game that ends after the change counts for no game. */
    cursor_setup();
    fake_screen_changes = 1;
    stable_pending = 1; control_tick(0);
    TEST_ASSERT(job_running);
    invalidate_game_work();
    finish_current_job();
    TEST_CHECK(!screen_changed_streak && !screen_retry_until);
    mem_free();

    cursor_setup();
    fake_screen_changes = 3;
    open_overlay();
    TEST_ASSERT(job_running);
    finish_current_job(); /* the first: again at once */
    TEST_ASSERT(job_running && !strcmp(g_view.status, "Recognizing…"));
    finish_current_job(); /* the second: 250 ms */
    TEST_CHECK(!job_running && open_retry && !strcmp(g_view.status, "Recognizing…") && !g_view.list);
    check_quiet();
    test_now += 249000; control_tick(0);
    TEST_CHECK(!job_running);
    test_now += 1000; control_tick(0);
    TEST_ASSERT(job_running && !open_retry);
    finish_current_job(); /* the third: 500 ms */
    test_now += 500000; control_tick(0);
    TEST_ASSERT(job_running);
    finish_current_job();
    check_current_definition("cat", 0);
    check_quiet();
    close_overlay(); mem_free();

    cursor_setup();
    fake_screen_changes = 2;
    open_overlay(); finish_current_job(); finish_current_job();
    TEST_CHECK(open_retry && !job_running);
    close_overlay();
    TEST_CHECK(!open_retry && stable_pending);
    control_tick(0);
    TEST_CHECK(!job_running);
    test_now += 250000; control_tick(0);
    TEST_ASSERT(job_running && job_prepares); /* Auto's job for this screen */
    finish_current_job();
    TEST_CHECK(local_text_valid && !g_view.open && cache_ok);
    check_quiet();
    mem_free();
}

/* The region shows scene id (as the kernel numbers it): a job then captures that scene. */
static void next_scene(uint32_t id)
{
    state.scene = state.region_scene = state.capture_scene = id;
    state.region_seq++;
}

/* The open overlay with this text, its cursor and its entry. */
static void check_shown(const char *text, const char *meaning, uint32_t cursor)
{
    TEST_ASSERT(g_view.list != NULL && g_view.list->n_entries == 1);
    TEST_CHECK(g_view.open && g_view.local_cursor_mode && g_view.cursor_byte == cursor);
    TEST_CHECK(!strcmp(g_view.list->header, text) && !strcmp(local_text, text));
    TEST_CHECK(strstr(g_view.list->entries[0].text, meaning) != NULL && g_view.list->entries[0].hl_start == (int)cursor);
    TEST_CHECK(!held_cmd && !held_view);
    check_quiet();
}

/* #81 request 4, the open overlay: its text stays usable whatever the screen
 * does. A settled screen it was not read from is recognized in the
 * background (a refresh) while the entry, the arena and an Anki save keep
 * working; the same text changes nothing, a new text replaces the shown one
 * quietly with its first word looked up, no text or a changing screen keeps
 * it. Refreshes wait for a settled screen and are paced (REFRESH_GAP_US
 * apart), a lookup goes first (a refresh in progress
 * yields and is asked again once lookups pause), and closing the overlay
 * drops one in progress; reopening meanwhile waits for a recognition. */
static void test_open_overlay_refreshes_its_text_quietly(void)
{
    cursor_setup();
    view_invariant = 1; view_invariant_checks = 0; invariant_generation = 0; invariant_seq = 0;
    open_overlay(); finish_current_job();
    check_shown("猫犬鳥", "cat", 0);
    uint32_t generation = local_text_generation;
    unsigned seq = g_view.list_seq, inferences = vocr_allocs;
    const VjoEntryList *shown = g_view.list;
    control_tick(VJO_EV_REGION_STABLE); /* the screen the text was read from */
    TEST_CHECK(!job_running && !refresh_pending && vocr_allocs == inferences);

    next_scene(8); /* an icon's mask ended */
    control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh && !job_lookup && job_idx == 1);
    /* What anki.c checks before saving the shown entry still holds. */
    TEST_CHECK(g_view.open && g_view.list == shown && g_view.list_seq == seq && g_view.list->n_entries == 1);
    TEST_CHECK(local_text_valid && has_result_memory() && !g_view.status[0]);
    finish_current_job();
    TEST_CHECK(DLOGGED("refresh rc=0: same text") && local_text_scene == 8 && cache_scene == 8);
    check_shown("猫犬鳥", "cat", 0);
    TEST_CHECK(g_view.list_seq == seq && local_text_generation == generation && vocr_allocs == inferences + 1);
    control_tick(VJO_EV_REGION_STABLE);
    TEST_CHECK(!job_running && refresh_pending); /* paced: REFRESH_GAP_US after the last refresh */
    test_now += REFRESH_GAP_US; control_tick(0);
    TEST_CHECK(!job_running && !refresh_pending); /* the shown text is this screen's now */
    state.stable = 0; next_scene(14); /* nor one that still changes */
    control_tick(VJO_EV_REGION_STABLE);
    TEST_CHECK(!job_running && !refresh_pending && vocr_allocs == inferences + 1);
    state.stable = 1;

    /* The game moved on (auto-advance): replaced quietly. */
    fake_recognized_text = "犬猫";
    next_scene(9);
    control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    finish_current_job();
    TEST_CHECK(DLOGGED("refresh rc=0: new text"));
    TEST_ASSERT(job_running && job_cursor_only && job_cursor_byte == 0);
    TEST_CHECK(!strcmp(local_text, "犬猫") && local_text_generation != generation && local_text_scene == 9);
    TEST_CHECK(g_view.list == &local_header && !g_view.list->n_entries && g_view.text_generation == local_text_generation);
    TEST_CHECK(g_view.list_seq != seq && !strcmp(g_view.status, "Looking up words…") && !g_view.status_is_error);
    vjo_post_cursor(3, generation); /* made for the old text: refused */
    TEST_CHECK(pending_cmd == VJO_CMD_NONE);
    finish_current_job();
    check_shown("犬猫", "dog", 0);
    generation = local_text_generation;

    /* A lookup while a refresh reads: the refresh yields. */
    next_scene(10);
    test_now += REFRESH_GAP_US;
    control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    vjo_post_cursor(3, generation); control_tick(0);
    TEST_CHECK(job_cancelled && cursor_waiting);
    finish_current_job(); /* the refresh, cancelled: the lookup follows */
    TEST_CHECK(DLOGGED("refresh rc=-118: dropped"));
    TEST_ASSERT(job_running && job_cursor_only && job_cursor_byte == 3);
    finish_current_job();
    check_shown("犬猫", "cat", 3);
    TEST_CHECK(refresh_pending);
    control_tick(0);
    TEST_CHECK(!job_running); /* lookups have not paused for REFRESH_IDLE_US yet */
    test_now += REFRESH_IDLE_US; control_tick(0);
    TEST_CHECK(!job_running); /* nor REFRESH_GAP_US passed since the last refresh */
    test_now += REFRESH_GAP_US - REFRESH_IDLE_US; control_tick(0);
    TEST_ASSERT(job_running && job_refresh);
    finish_current_job(); /* the same text: the cursor and its entry stay */
    check_shown("犬猫", "cat", 3);

    /* A lookup long after the last refresh still goes first: a screen that
     * settles right after it waits REFRESH_IDLE_US; a command posted by the
     * overlay meanwhile (a move) also holds the refresh back. */
    test_now += 3 * REFRESH_GAP_US;
    vjo_post_cursor(0, local_text_generation); on_command(); finish_current_job();
    check_shown("犬猫", "dog", 0);
    next_scene(18); control_tick(VJO_EV_REGION_STABLE);
    TEST_CHECK(!job_running && refresh_pending);
    test_now += REFRESH_IDLE_US - 1; control_tick(0);
    TEST_CHECK(!job_running);
    test_now += 1;
    vjo_post_cursor(3, local_text_generation); /* the overlay's move, not handled yet */
    background_open();
    TEST_CHECK(!job_running && pending_cmd == VJO_CMD_CURSOR);
    on_command(); finish_current_job();
    check_shown("犬猫", "cat", 3);
    test_now += REFRESH_IDLE_US; control_tick(0);
    TEST_ASSERT(job_running && job_refresh);
    finish_current_job();
    check_shown("犬猫", "cat", 3);

    /* A refresh after each settled screen, REFRESH_GAP_US apart at most. */
    next_scene(15);
    test_now += REFRESH_GAP_US - 1; control_tick(VJO_EV_REGION_STABLE);
    TEST_CHECK(!job_running && refresh_pending);
    test_now += 1; control_tick(0);
    TEST_ASSERT(job_running && job_refresh);
    finish_current_job();
    check_shown("犬猫", "cat", 3);

    /* No text in the region (a box fading between lines), then a changing
     * screen: the shown text stays usable. */
    fake_recognized_text = "";
    next_scene(11);
    test_now += REFRESH_GAP_US; control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    finish_current_job();
    TEST_CHECK(DLOGGED("refresh rc=0: kept"));
    check_shown("犬猫", "cat", 3);
    fake_screen_changes = 1;
    next_scene(12);
    test_now += REFRESH_GAP_US; control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    finish_current_job();
    TEST_CHECK(DLOGGED("refresh dropped: the screen changed under it (1 in a row), next try in 0 ms"));
    check_shown("犬猫", "cat", 3);
    TEST_CHECK(refresh_pending && !job_running);
    fake_recognized_text = "鳥";
    control_tick(0); /* the next try, at once */
    TEST_ASSERT(job_running && job_refresh);
    finish_current_job(); finish_current_job();
    check_shown("鳥", "bird", 0);
    TEST_CHECK(!screen_changed_streak);

    /* Closing during a refresh drops it; Auto has the settled screen. */
    next_scene(13);
    test_now += REFRESH_GAP_US; control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    close_overlay();
    TEST_CHECK(job_cancelled && !refresh_pending);
    finish_current_job();
    TEST_CHECK(!job_running && !g_view.open && stable_pending && local_text_valid);
    check_quiet();
    /* Reopened while a refresh still runs, on a screen the text was not read
     * from: the opening waits for its own recognition. */
    fake_recognized_text = "犬";
    open_overlay(); finish_current_job();
    check_shown("犬", "dog", 0);
    next_scene(17);
    test_now += REFRESH_GAP_US; control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    close_overlay(); open_overlay();
    TEST_CHECK(ov == OV_OPEN_OLD_JOB && !strcmp(g_view.status, "Recognizing…"));
    finish_current_job();
    TEST_ASSERT(job_running && !job_refresh && !job_cursor_only && ov == OV_OPEN);
    fake_recognized_text = "猫";
    finish_current_job();
    check_shown("猫", "cat", 0);
    close_overlay(); mem_free();
    TEST_CHECK(view_invariant_checks > 10);
    view_invariant = 0;
}

/* ocr_mode = on_press recognizes only when the overlay opens: the open
 * overlay keeps its text and runs no refresh, however the screen settles. */
static void test_on_press_overlay_never_refreshes(void)
{
    cursor_setup();
    cfg.ocr_mode = loaded.ocr_mode = VJO_OCR_ON_PRESS;
    open_overlay(); finish_current_job();
    check_shown("猫犬鳥", "cat", 0);
    unsigned inferences = vocr_allocs;
    for (int i = 0; i < 3; i++) {
        next_scene(20 + i);
        test_now += 4 * REFRESH_GAP_US; control_tick(VJO_EV_REGION_STABLE);
        TEST_CHECK(!job_running && vocr_allocs == inferences);
    }
    check_shown("猫犬鳥", "cat", 0);
    close_overlay(); mem_free();
}

/* A replaced text's dictionary pages go with it: the overlay never offers
 * the old text's pages for the new text's word. */
static void test_refresh_replacement_resets_dictionary_pages(void)
{
    paged_setup(3);
    open_overlay(); finish_current_job();
    check_page("main. cat", 0, 0, 3);
    TEST_CHECK(vjo_post_page(1, local_text_generation)); on_command(); finish_current_job();
    check_page("extra cat", 0, 1, 3);
    fake_recognized_text = "犬";
    next_scene(8);
    test_now += REFRESH_IDLE_US; /* the page's lookup goes first */
    control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    finish_current_job();
    TEST_ASSERT(job_running && job_cursor_only);
    TEST_CHECK(!g_view.dict_page && !g_view.dict_pages && !local_page && !vjo_post_page(1, local_text_generation));
    finish_current_job();
    check_page("main. dog", 0, 0, 2); /* third.vjdict has no 犬 */
    close_overlay(); mem_free();
}

/* The job on the network thread, then one control-thread wakeup handles its
 * sentence and its end in ctl_main's order (finish_current_job skips the
 * sentence step). */
static void finish_by_tick(void)
{
    TEST_ASSERT(job_running);
    int idx = job_idx;
    job_rc = job_cursor_only ? run_cursor_job(&results[idx], &cache_data[idx]) : run_job(&results[idx], &cache_data[idx]);
    job_done = 1;
    control_tick(0);
}

/* Subtitles on, overlay open (#92's audit, B1): a refresh that reads a new text
 * shows it on the subtitle strip too, as every other OCR job does; its lookup
 * tags the cache with the refresh's capture, so after Circle no job reads that
 * screen again and the strip must already be right. A refresh that reads the
 * same text, or none, leaves the strip alone. */
static void test_refresh_updates_the_subtitle_strip(void)
{
    cursor_setup();
    subtitles = 1; /* Select+R before opening the overlay */
    open_overlay(); finish_current_job();
    check_shown("猫犬鳥", "cat", 0);
    TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !strcmp(g_view.strip_text, "猫犬鳥"));
    unsigned strip_seq = g_view.strip_text_seq;
    next_scene(8); /* the same text */
    control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh && !g_view.strip_busy);
    finish_current_job();
    TEST_CHECK(g_view.strip_text_seq == strip_seq && !strcmp(g_view.strip_text, "猫犬鳥"));
    fake_recognized_text = ""; /* no text: the shown one stays, and so does the strip */
    next_scene(9);
    test_now += REFRESH_GAP_US; control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    finish_current_job();
    TEST_CHECK(g_view.strip_text_seq == strip_seq && !strcmp(g_view.strip_text, "猫犬鳥"));
    fake_recognized_text = "犬猫"; /* the game moves on while the overlay is open */
    next_scene(10);
    test_now += REFRESH_GAP_US; control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    finish_current_job();
    TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !strcmp(g_view.strip_text, "犬猫") && !g_view.strip_busy);
    TEST_ASSERT(job_running && job_cursor_only);
    finish_current_job(); /* its first word: tags the cache with the refresh's capture */
    check_shown("犬猫", "dog", 0);
    unsigned inferences = vocr_allocs;
    close_overlay(); /* Circle */
    for (int i = 0; i < 3; i++) {
        test_now += SUBTITLE_GAP_US; control_tick(0);
        if (job_running) finish_current_job();
    }
    TEST_CHECK_(!strcmp(g_view.strip_text, "犬猫"), "strip shows [%s] while the region shows 犬猫", g_view.strip_text);
    TEST_CHECK(vocr_allocs == inferences); /* the screen's text was already read */
    subtitles = 0;
    mem_free();
}

/* The cards name the model that read their text (#90's record): a refresh
 * that keeps the shown text leaves the model alone, even when another model
 * read it (the memory fallback); one that replaces the text names its own. */
static void test_refresh_names_its_model_on_the_cards_only_when_it_replaces(void)
{
    cursor_setup();
    open_overlay(); finish_current_job();
    check_shown("猫犬鳥", "cat", 0);
    TEST_CHECK(!strcmp(anki_model, "VOCR3_H15_w8.vocr"));
    fake_memory_fallback = 1; /* the next jobs: H15 did not fit, FL10 ran */
    next_scene(8); /* the same text */
    control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    finish_current_job();
    TEST_CHECK(!strcmp(anki_model, "VOCR3_H15_w8.vocr"));
    fake_recognized_text = "犬猫";
    next_scene(9);
    test_now += REFRESH_GAP_US; control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    finish_current_job(); finish_current_job();
    check_shown("犬猫", "dog", 0);
    TEST_CHECK(!strcmp(anki_model, "VOCR3_FL10_w8.vocr"));
    close_overlay(); mem_free();
}

/* A refresh logs its own lines only: no "subtitle text ready" (it has no
 * sentence until it is compared). A failing one (here the game's memory)
 * shows nothing, keeps the shown text, and backs off as any OCR job: 2 s,
 * 4 s, ... between tries; a refresh that works ends the backoff. */
static void test_refresh_logs_quietly_and_a_failing_one_backs_off(void)
{
    cursor_setup();
    open_overlay(); finish_current_job();
    check_shown("猫犬鳥", "cat", 0);
    next_scene(8);
    control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    log_count = 0;
    finish_by_tick();
    TEST_CHECK(!job_running && DLOGGED("refresh rc=0: same text") && !logged("subtitle text ready"));
    fail_vocr_user = 1; fake_free_user = 1000; /* the game has no memory for OCR now */
    next_scene(9);
    test_now += REFRESH_GAP_US; control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    finish_by_tick();
    TEST_CHECK(DLOGGED("refresh rc=-103: kept") && ocr_backoff.us == BACKOFF_MIN_US);
    check_shown("猫犬鳥", "cat", 0);
    TEST_CHECK(!g_view.status_is_error && !g_view.status[0]);
    next_scene(10);
    test_now += BACKOFF_MIN_US; control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh); /* 2 s later: the next try */
    finish_by_tick();
    TEST_CHECK(ocr_backoff.us == 2 * BACKOFF_MIN_US);
    next_scene(11);
    test_now += BACKOFF_MIN_US; control_tick(VJO_EV_REGION_STABLE);
    TEST_CHECK(!job_running && refresh_pending); /* backing off: 4 s now */
    fail_vocr_user = 0;
    test_now += BACKOFF_MIN_US; control_tick(0);
    TEST_ASSERT(job_running && job_refresh);
    finish_by_tick();
    TEST_CHECK(!ocr_backoff.us && !ocr_backoff.until);
    check_shown("猫犬鳥", "cat", 0);
    close_overlay(); mem_free();
}

#if VJO_DEBUG /* a self-test test: the debug build's */
/* Subtitles turned on while the self-test's job runs show "Recognizing…" and
 * wait for a job's result, which that job never brings: once it ends, the strip
 * shows the screen's cached sentence, or a job reads a screen the cache is not
 * for (audit-overlay-first-ocr S1). */
static void test_subtitles_on_during_the_selftest_job(void)
{
    cursor_setup();
    game_generation = 1;
    selftest_armed = 1; selftest_done = 0; selftest_stage_count = 5; selftest_begins = 0;
    apply_config();
    stable_pending = 1; control_tick(0);
    TEST_ASSERT(job_running && !job_selftest);
    finish_current_job();
    test_now += SELFTEST_IDLE_US; control_tick(0);
    TEST_ASSERT(job_running && job_selftest);
    set_subtitles(1);
    TEST_CHECK(g_view.strip_kind == VJO_STRIP_STATUS && g_view.strip_busy && job_selftest);
    finish_current_job();
    TEST_CHECK(!job_running && g_view.strip_kind == VJO_STRIP_SENTENCE && !strcmp(g_view.strip_text, "猫犬鳥"));
    TEST_CHECK(!g_view.strip_busy);
    set_subtitles(0);
    /* ... on a screen the cache is not for (the next line, settled after the stages began): a job reads it */
    selftest_done = 0; selftest_begins = 0;
    test_now += 2 * SELFTEST_IDLE_US; control_tick(0);
    TEST_ASSERT(job_running && job_selftest);
    set_subtitles(1);
    next_scene(41); /* no event yet: the stages run on */
    fake_recognized_text = "鳥";
    finish_current_job();
    TEST_ASSERT(job_running && !job_selftest && logged("job start (subtitles on)") != NULL);
    finish_current_job();
    TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !strcmp(g_view.strip_text, "鳥"));
    set_subtitles(0);
    selftest_armed = selftest_done = 0; selftest_stage_count = 0;
    mem_free();
}
#endif

#if VJO_DEBUG /* the debug build's log lines (the normal build's: test_normal_build_*) */
/* A job's done line (net_job): the time in exchanges with the game worker, each
 * job's own, and the log's appends meanwhile, in the format tools/analyse_timeline.py
 * reads (docs/first-ocr.md). */
static void test_the_done_line_has_the_exchange_and_log_times(void)
{
    reset();
    fake_recognized_text = "猫";
    fake_exchange_us = 7000;
    for (int k = 0; k < 2; k++) {
        int idx = -1, rc = -1, ms = -1, page = -1, pages = -1, end = -1;
        unsigned peak = 0, game = 0, log_ms = 0, appends = 0;
        const char *line;
        raw_state.done_seq = 11;
        job_cfg = cfg;
        job_idx = 0; job_cursor_only = 0; job_cancelled = job_captured = 0; job_done = 0;
        net_job();
        TEST_CHECK(job_done && job_rc == VJO_OK);
        line = logged("job 0 done rc=0 in ");
        TEST_ASSERT(line != NULL);
        TEST_CHECK(sscanf(line, "job %d done rc=%d in %d ms, arena peak %u, dictionary page %d/%d, game %u ms, "
                          "log %u ms in %u appends%n", &idx, &rc, &ms, &peak, &page, &pages, &game, &log_ms,
                          &appends, &end) == 9);
        TEST_CHECK(end > 0 && (line[end] == '\0' || line[end] == '\n')); /* to its last word */
        TEST_CHECK(idx == 0 && !rc && ms >= 7 && game == 7 && log_ms == 3 && appends == 2); /* each job's own */
        TEST_MSG("%s", line);
        log_count = 0;
        mem_free();
    }
}
#endif

#if VJO_DEBUG /* a self-test test: the debug build's */
/* The self-test's stages publish captures of their own, so the kernel's
 * region_seq moves on: a shown text that matched the screen only through its
 * capture (an icon masked after it, a new scene id without an event) is tagged
 * with the scene first, so a press after the stages still looks it up instead
 * of recognizing the screen again (audit-overlay-first-ocr). */
static void test_selftest_keeps_the_shown_texts_screen(void)
{
  for (int word = 0; word < 2; word++) {
    cursor_setup();
    if (!word) /* Character mode: no cached lookup; Whole word: a cached lookup, retagged too */
        cfg.highlight_mode = loaded.highlight_mode = VJO_HIGHLIGHT_CHARACTER;
    game_generation = 1;
    selftest_armed = 1; selftest_done = 0; selftest_stage_count = 5;
    apply_config();
    stable_pending = 1; control_tick(0);
    TEST_ASSERT(job_running && !job_selftest);
    finish_current_job();
    TEST_ASSERT(local_text_valid);
    state.scene = state.region_scene = 9; /* the icon masked: a new id, the same capture */
    TEST_CHECK(shows_screen(local_text_scene, local_text_capture_seq, &state) && local_text_scene != 9);
    test_now += SELFTEST_IDLE_US; control_tick(0);
    TEST_ASSERT(job_running && job_selftest);
    finish_current_job();
    TEST_CHECK(selftest_done);
    state.region_seq += 5; /* its stages' captures */
    unsigned n = submits;
    open_overlay();
    if (word) {
        TEST_CHECK(!job_running && submits == n && logged("open: saved local word") != NULL); /* at once */
    } else {
        TEST_ASSERT(job_running && job_cursor_only && submits == n); /* a lookup of the shown text, no OCR */
        finish_current_job();
    }
    check_shown("猫犬鳥", "cat", 0);
    close_overlay();
    selftest_armed = selftest_done = 0; selftest_stage_count = 0;
    mem_free();
  }
}
#endif

#if VJO_DEBUG /* a self-test test: the debug build's */
/* The overlay opened during the self-test's job keeps its text when the job ends,
 * whatever the screen did meanwhile (#81 request 4): with subtitles on, the strip
 * shows that text and starts no job of its own (one would retire the text and show
 * "Recognizing…"); a new screen is the quiet refresh's, whose text the strip then
 * shows (audit-overlay-first-ocr-b RB1, its PROBE7). */
static void test_selftest_end_keeps_the_open_overlays_text(void)
{
    for (int same = 0; same < 2; same++) {
        cursor_setup(); /* Whole word: the saved word shows at once */
        game_generation = 1;
        selftest_armed = 1; selftest_done = 0; selftest_stage_count = 5; selftest_begins = 0;
        apply_config();
        stable_pending = 1; control_tick(0);
        TEST_ASSERT(job_running && !job_selftest);
        finish_current_job();
        TEST_ASSERT(local_text_valid && cache_ok);
        set_subtitles(1);
        TEST_CHECK(!job_running && g_view.strip_kind == VJO_STRIP_SENTENCE);
        test_now += SELFTEST_IDLE_US; control_tick(0);
        TEST_ASSERT(job_running && job_selftest);
        open_overlay(); /* it yields; the saved word shows at once */
        TEST_CHECK(job_cancelled && logged("open: saved local word") != NULL);
        check_shown("猫犬鳥", "cat", 0);
        next_scene(41);
        control_tick(VJO_EV_REGION_STABLE); /* a new screen settles before the stage ends */
        fake_recognized_text = same ? "猫犬鳥" : "犬猫";
        finish_current_job(); /* the self-test's job ends */
        TEST_CHECK(!job_running && !logged("job start (subtitles on)"));
        TEST_CHECK(local_text_valid && !strcmp(local_text, "猫犬鳥") && strcmp(g_view.status, "Recognizing…"));
        check_shown("猫犬鳥", "cat", 0);
        TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !g_view.strip_busy &&
                   !strcmp(g_view.strip_text, "猫犬鳥"));
        test_now += REFRESH_GAP_US + REFRESH_IDLE_US; control_tick(0);
        TEST_ASSERT(job_running && job_refresh && !job_lookup); /* the quiet refresh */
        finish_current_job();
        TEST_CHECK(local_text_valid && !strcmp(local_text, same ? "猫犬鳥" : "犬猫"));
        TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !strcmp(g_view.strip_text, same ? "猫犬鳥" : "犬猫"));
        TEST_MSG("same %d: strip [%s], overlay [%s]", same, g_view.strip_text, local_text);
        while (job_running) finish_current_job(); /* the new text's first word */
        close_overlay();
        set_subtitles(0);
        selftest_armed = selftest_done = 0; selftest_stage_count = 0;
        mem_free();
    }
}
#endif

#if VJO_DEBUG /* a self-test test: the debug build's */
/* Subtitles turned on during the self-test's job wait in "Recognizing…"; an overlay
 * opened next on a text without its lookup (Character mode) waits for the job too.
 * When the job ends, the lookup runs and the strip shows the overlay's text at once,
 * not after the stages run again (audit-overlay-first-ocr-b, its PROBE10). */
static void test_selftest_end_shows_the_open_overlays_text_in_the_strip(void)
{
    cursor_setup();
    cfg.highlight_mode = loaded.highlight_mode = VJO_HIGHLIGHT_CHARACTER;
    game_generation = 1;
    selftest_armed = 1; selftest_done = 0; selftest_stage_count = 5; selftest_begins = 0;
    apply_config();
    stable_pending = 1; control_tick(0);
    TEST_ASSERT(job_running && !job_selftest);
    finish_current_job();
    test_now += SELFTEST_IDLE_US; control_tick(0);
    TEST_ASSERT(job_running && job_selftest);
    set_subtitles(1);
    TEST_CHECK(g_view.strip_kind == VJO_STRIP_STATUS && g_view.strip_busy);
    unsigned n = submits;
    open_overlay(); /* it yields; the lookup waits for the job */
    TEST_CHECK(job_cancelled && cursor_waiting);
    finish_current_job(); /* the self-test's job ends: the lookup runs */
    TEST_ASSERT(job_running && job_cursor_only);
    TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !g_view.strip_busy && !strcmp(g_view.strip_text, "猫犬鳥"));
    n = submits;
    finish_current_job();
    TEST_CHECK(submits == n && !job_running); /* a lookup, no OCR */
    check_shown("猫犬鳥", "cat", 0);
    close_overlay();
    set_subtitles(0);
    selftest_armed = selftest_done = 0; selftest_stage_count = 0;
    mem_free();
}
#endif

#if VJO_DEBUG /* a self-test test: the debug build's */
/* The self-test's job keeps the capture of the text it runs on: the stages'
 * own captures (published by the kernel as any job's) never stand in for it,
 * so a new screen still stops the job, whichever capture a stage published or
 * voided last. */
static void test_selftest_job_keeps_the_texts_capture(void)
{
    reset();
    job_capture_scene = 7; job_capture_seq = 9; job_captured = 1;
    job_selftest = 1;
    vocr_capture_fixed = 0;
    state.done_seq = vocr_done_before + 1; /* a stage's first pass is in */
    state.capture_scene = 12; state.region_seq = 13;
    vocr_note_capture();
    TEST_CHECK(job_capture_scene == 7 && job_capture_seq == 9 && job_captured);
    state.region_seq = 0; /* a stage's try voided its capture */
    vocr_note_capture();
    TEST_CHECK(job_captured && job_capture_scene == 7);
    job_selftest = 0; /* any other job takes the published capture */
    state.region_seq = 13;
    vocr_note_capture();
    TEST_CHECK(job_capture_scene == 12 && job_capture_seq == 13 && job_captured);
}
#endif

#if VJO_DEBUG /* a self-test test: the debug build's */
/* The open overlay's refresh runs while the self-test is armed: the stages run
 * in the self-test's own job only (job_selftest), never in a job the overlay
 * waits for or in a refresh, and never while the overlay is open. */
static void test_refresh_while_the_selftest_is_armed(void)
{
    cursor_setup();
    game_generation = 1;
    selftest_armed = 1; selftest_done = 0; selftest_stage_count = 5; selftest_begins = 0;
    open_overlay(); finish_current_job();
    check_shown("猫犬鳥", "cat", 0);
    TEST_CHECK(!selftest_begins); /* the overlay's own job ran no stage */
    next_scene(8);
    test_now += REFRESH_GAP_US; control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh && !job_selftest);
    finish_current_job();
    TEST_CHECK(!selftest_begins && logged("refresh rc=0: same text") != NULL);
    check_shown("猫犬鳥", "cat", 0);
    for (int i = 0; i < 3; i++) {
        test_now += 2 * SELFTEST_IDLE_US; control_tick(0);
        TEST_CHECK(!job_selftest && !selftest_begins); /* never with the overlay open */
        if (job_running) finish_current_job();
    }
    selftest_armed = selftest_done = 0; selftest_stage_count = 0;
    close_overlay(); mem_free();
}
#endif

#if VJO_DEBUG /* a self-test test: the debug build's */
/* test.2's first scans ran beside the self-test's stages and took 9-15 s
 * (docs/first-ocr.md). The user's jobs never run them now: the first settled
 * screen gets a plain job, and once a job read text the stages run in a
 * background job of their own on that screen, with the overlay closed and the
 * Shell idle for SELFTEST_IDLE_US; it changes neither the text nor its lookup.
 * Opening the overlay stops it at once (the saved word shows without waiting
 * for it), and so does a new screen, whose own job then comes first; the stages
 * run again later, from the first. */
static void test_selftest_runs_after_the_first_text_in_its_own_job(void)
{
    cursor_setup();
    game_generation = 1;
    selftest_armed = 1; selftest_done = 0; selftest_stage_count = 5;
    selftest_begins = selftest_ends = submits = 0;
    apply_config();
    stable_pending = 1; control_tick(0); /* the first settled screen: a plain Auto job */
    TEST_ASSERT(job_running && !job_selftest && job_lookup);
    finish_current_job();
    TEST_CHECK(submits == 1 && !selftest_begins && local_text_valid && !strcmp(local_text, "猫犬鳥") && cache_ok);
    TEST_CHECK(text_generation == game_generation);
    uint32_t generation = local_text_generation, cursor_seq = local_cursor_seq;
    control_tick(0);
    TEST_CHECK(!job_running); /* not before the Shell has been idle SELFTEST_IDLE_US */
    test_now += SELFTEST_IDLE_US - 1; control_tick(0);
    TEST_CHECK(!job_running);
    test_now += 1; control_tick(0);
    TEST_ASSERT(job_running && job_selftest && !job_lookup && job_idx == 1);
    TEST_CHECK(logged("job start (self-test)") != NULL);
    int64_t started = job_started_us;
    test_now += 2 * SELFTEST_IDLE_US; control_tick(0);
    TEST_CHECK(job_started_us == started); /* never a second job while one runs */
    finish_current_job();
    TEST_CHECK(selftest_begins == 5 && selftest_ends == 5 && submits == 1 + 5 && selftest_done && !job_running);
    for (unsigned i = 0; i < 1 + 5; i++) /* Auto's job and every stage: the 2 s limit, not a press's 1 s */
        TEST_CHECK((selftest_flags_seen[i] >> VJO_GAME_OCR_SETTLE_MAX_SHIFT & 0xFFu) == 40u);
    TEST_CHECK(local_text_valid && !strcmp(local_text, "猫犬鳥") && local_text_generation == generation);
    TEST_CHECK(local_cursor_seq == cursor_seq && cache_ok && active == 0);
    unsigned n = submits;
    open_overlay(); /* the prepared word, at once: no job */
    TEST_CHECK(logged("open: saved local word") != NULL && !job_running && submits == n);
    check_shown("猫犬鳥", "cat", 0);
    close_overlay();
    test_now += 2 * SELFTEST_IDLE_US; control_tick(0);
    TEST_CHECK(!job_running); /* done: once a boot */

    /* Armed again: the overlay opened while the stages run stops them, and the saved
     * word shows at once; they wait while it is open and run again after it closes. */
    selftest_done = 0; selftest_begins = selftest_ends = submits = 0;
    next_scene(30); control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && !job_selftest);
    finish_current_job();
    state.stable = 0; next_scene(33); /* the next line typing out: not settled, not read */
    test_now += 2 * SELFTEST_IDLE_US; control_tick(0);
    TEST_CHECK(!job_running); /* never on a screen the last job did not read */
    state.stable = 1; next_scene(30); /* the box shows the text read again */
    test_now += SELFTEST_IDLE_US; control_tick(0);
    TEST_ASSERT(job_running && job_selftest);
    open_overlay();
    TEST_CHECK(job_cancelled && logged("self-test: stopped for the overlay") != NULL);
    TEST_CHECK(logged("open: saved local word") != NULL);
    check_shown("猫犬鳥", "cat", 0);
    finish_current_job();
    TEST_CHECK(!job_running && !selftest_done && !selftest_begins && submits == 1);
    test_now += 2 * SELFTEST_IDLE_US; control_tick(0);
    TEST_CHECK(!job_running); /* not while the overlay is open */
    close_overlay();
    control_tick(0);
    TEST_CHECK(!job_running); /* idle first */
    test_now += SELFTEST_IDLE_US; control_tick(0);
    TEST_ASSERT(job_running && job_selftest); /* again, on the same screen */

    /* A new screen stops it; that screen's own job runs first, then the stages. */
    next_scene(31); control_tick(VJO_EV_REGION_STABLE);
    TEST_CHECK(job_cancelled);
    finish_current_job();
    TEST_CHECK(!job_running && !selftest_done);
    control_tick(0);
    TEST_ASSERT(job_running && !job_selftest); /* scene 31's plain job */
    finish_current_job();
    TEST_CHECK(!selftest_begins);
    test_now += SELFTEST_IDLE_US; control_tick(0);
    TEST_ASSERT(job_running && job_selftest);
    finish_current_job();
    TEST_CHECK(selftest_begins == 5 && selftest_ends == 5 && selftest_done);

    /* The overlay opened while the stages run on a screen the shown text is not
     * from (the next line, not settled yet): it waits for its own recognition,
     * which starts once the self-test's job has stopped. */
    selftest_done = 0; selftest_begins = 0;
    test_now += 2 * SELFTEST_IDLE_US; control_tick(0);
    TEST_ASSERT(job_running && job_selftest);
    state.stable = 0; next_scene(34);
    fake_recognized_text = "鳥";
    open_overlay();
    TEST_CHECK(job_cancelled && ov == OV_OPEN_OLD_JOB && !strcmp(g_view.status, "Recognizing…"));
    finish_current_job();
    TEST_ASSERT(job_running && !job_selftest && job_lookup && ov == OV_OPEN);
    state.stable = 1;
    finish_current_job();
    check_shown("鳥", "bird", 0);
    close_overlay();
    /* ... and on the text's own screen with its lookup gone: the lookup waits for the
     * stopped job, then runs. */
    test_now += 2 * SELFTEST_IDLE_US; control_tick(0);
    TEST_ASSERT(job_running && job_selftest);
    cache_ok = 0;
    open_overlay();
    TEST_CHECK(job_cancelled && cursor_waiting && ov == OV_OPEN);
    finish_current_job();
    TEST_ASSERT(job_running && job_cursor_only);
    finish_current_job();
    check_shown("鳥", "bird", 0);
    close_overlay();
    TEST_CHECK(!selftest_done && !selftest_begins);

    /* No text in the region: the self-test waits for a job that reads some. */
    selftest_done = 0; selftest_begins = 0;
    fake_recognized_text = "";
    next_scene(32); control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && !job_selftest);
    finish_current_job();
    TEST_CHECK(!text_generation);
    test_now += 2 * SELFTEST_IDLE_US; control_tick(0);
    TEST_CHECK(!job_running && !selftest_begins);
    selftest_armed = selftest_done = 0; selftest_stage_count = 0;
    mem_free();
}
#endif
#endif


#if !VJO_DEBUG
/* ---- the normal build's short log (docs/release-variants.md) ---- */

/* One line per OCR job: its result, the model that ran, the lines and the time; never
 * the recognized text, the capture, a passed memory check, the step log or the timings. */
static void test_normal_build_logs_one_line_per_job(void)
{
    reset();
    cfg.ocr_backend = VJO_OCR_VOCR;
    fake_recognized_text = "猫が鳴いた";
    fake_steps = 1;
    TEST_CHECK(run_vocr_job() == VJO_OK && !strcmp(cache_data[0].sentence, "猫が鳴いた"));
    const char *line = logged("ocr rc=0 ");
    TEST_CHECK(line && !strncmp(line, "ocr rc=0 VOCR3_H15_w8.vocr: 2 lines, ", 37));
    TEST_MSG("%s", line ? line : "(none)");
    TEST_CHECK(log_count == 1); /* that line only */
    for (unsigned i = 0; i < log_count && i < 32; i++)
        TEST_CHECK_(!strstr(log_lines[i], "猫") && !strstr(log_lines[i], "game vocr") && !strstr(log_lines[i], "gstep"),
                    "%s", log_lines[i]);
    mem_free();
}

/* The 444 MHz boost's one line: the first job of a game and whenever it changes, never per job. */
static void test_normal_build_logs_the_clock_once_and_on_change(void)
{
    reset();
    cfg.ocr_backend = VJO_OCR_VOCR;
    fake_recognized_text = "猫";
    fake_clock_on = 1; fake_clock_rc = 0;
    fake_clock_mhz[0] = 333; fake_clock_mhz[1] = 444; fake_clock_mhz[2] = 333;
    game_generation = job_generation = 41;
    TEST_CHECK(run_vocr_job() == VJO_OK && logged("clock: 333 -> 444 MHz while reading, 333 after") != NULL);
    mem_free();
    log_count = 0;
    TEST_CHECK(run_vocr_job() == VJO_OK && !logged("clock:") && logged("ocr rc=0 ") != NULL); /* the same: nothing */
    mem_free();
    fake_clock_rc = 1; fake_clock_mhz[0] = fake_clock_mhz[1] = fake_clock_mhz[2] = 500; /* PSVshell's 500 */
    TEST_CHECK(run_vocr_job() == VJO_OK && logged("clock: 500 MHz, left as it is while reading (444 or more)") != NULL);
    mem_free();
    log_count = 0;
    fake_clock_rc = (int32_t)0x80010016; fake_clock_mhz[0] = 333; fake_clock_mhz[1] = 333; fake_clock_mhz[2] = 333;
    TEST_CHECK(run_vocr_job() == VJO_OK && logged("clock: 444 MHz refused 0x80010016, 333 MHz while reading") != NULL);
    mem_free();
    log_count = 0;
    job_generation = 42; /* the next game: its first job says it again */
    TEST_CHECK(run_vocr_job() == VJO_OK && logged("clock: 444 MHz refused") != NULL);
    mem_free();
    log_count = 0;
    fake_clock_on = 0; /* ocr_boost_clock = off: no step, no line */
    job_generation = 43;
    TEST_CHECK(run_vocr_job() == VJO_OK && !logged("clock:"));
    mem_free();
}

/* A lookup's line only when it failed, without its text. */
static void test_normal_build_logs_a_failed_lookup_only(void)
{
    VjoOverlayData out;
    memset(&out, 0, sizeof(out));
    out.sentence = "猫が鳴いた";
    log_count = 0;
    log_lookup("cursor", &out, 3, VJO_OK, 0);
    TEST_CHECK(log_count == 0);
    log_lookup("cursor", &out, 3, VJO_E_OOM, 0);
    TEST_CHECK(log_count == 1 && logged("lookup cursor at 3 failed: rc=") != NULL && !logged("猫"));
}

/* No self-test: never armed, no stage, no job of its own; an armed marker is said so once. */
static void test_normal_build_has_no_selftest(void)
{
    reset();
    selftest_marker = 1; selftest_marker_reads = 0;
    vjo_selftest_init(&cfg);
    TEST_CHECK(selftest_marker_reads == 1 &&
               logged("self-test: armed, but this is the normal build: install the -debug build to run it") != NULL);
    TEST_CHECK(!vjo_selftest_armed() && !vjo_selftest_stages());
    log_count = 0; selftest_marker = 0;
    vjo_selftest_init(&cfg);
    TEST_CHECK(log_count == 0); /* not armed: nothing */
}
#endif

/* test.2's log.txt (#101): each config load (an overlay open, a game start,
 * subtitles on) logged every warning again, 76 lines from 8 distinct ones.
 * A warning is logged when the loaded set changes; the overlay still lists
 * the warnings each time it opens. */
static void test_config_warnings_are_logged_once_per_change(void)
{
    static const char stale[] = "ocr_backend = vocr2\nmeiki_layout = auto\nocr_settle_ms = 150\n";
    reset();
    vjo_config_defaults(&loaded);
    vjo_config_parse(&loaded, stale, strlen(stale));
    TEST_ASSERT(loaded.n_warnings == 2);
    for (int i = 0; i < 3; i++) {
        apply_config();
        open_overlay(); /* re-reads config.ini */
        TEST_CHECK(g_view.n_warnings == 2 && !strcmp(g_view.warnings[1], "unknown setting 'meiki_layout'"));
        close_overlay();
        activate(state.game_pid, "PCSG00415", VJO_GAME);
        set_subtitles(1);
        set_subtitles(0);
    }
    TEST_CHECK_(config_warning_logs == 2, "%u config warning lines", config_warning_logs);
    TEST_CHECK(warnings_logged("ocr_backend: VOCR2 was removed; using vocr") == 1);
    TEST_CHECK(warnings_logged("unknown setting 'meiki_layout'") == 1);
    /* the file changes: its new set is logged once */
    vjo_config_defaults(&loaded);
    vjo_config_parse(&loaded, "ocr_backend = vocr2\n", 20);
    apply_config();
    apply_config();
    TEST_CHECK(config_warning_logs == 3 && warnings_logged("ocr_backend: VOCR2 was removed; using vocr") == 2);
    /* another warning in its place, the same count: a new set, logged once */
    vjo_config_defaults(&loaded);
    vjo_config_parse(&loaded, "ocr_backend = meiki\n", 20);
    TEST_ASSERT(loaded.n_warnings == 1);
    apply_config();
    apply_config();
    TEST_CHECK(config_warning_logs == 4 && warnings_logged("ocr_backend: Meiki was removed; using vocr") == 1);
    /* repaired (the app's next save drops the engine line): nothing more is logged or shown */
    vjo_config_defaults(&loaded);
    apply_config();
    open_overlay();
    TEST_CHECK(config_warning_logs == 4 && !g_view.n_warnings);
    close_overlay();
    mem_free();
}

static void app_files(int installed_rc, const char *installed, int eboot, int record_rc, const char *record)
{
    app_installed_rc = installed_rc;
    snprintf(app_installed, sizeof(app_installed), "%s", installed);
    app_eboot = eboot;
    app_record_rc = record_rc;
    snprintf(app_record, sizeof(app_record), "%s", record);
}
#define STAMP_21 "vjoapp1 api=21 build=0123456789abcdef"
#define STAMP_20 "vjoapp1 api=20 build=fedcba9876543210"
#define NOENT ((int)0x80010002u)

/* overlay.cpp's render: an open overlay's body takes the notice through vjo_view_notice, which marks
 * it drawn (the view lock is render's own). */
static const char *overlay_draws(void)
{
    return g_view.open ? vjo_view_notice() : NULL;
}
static int notice_is(const char *shown, const char *text) { return shown && !strcmp(shown, text); }
static void next_game(void)
{
    on_game_exit();
    activate(state.game_pid, "PCSG00415", VJO_GAME);
}

/* The installed Overlay app is checked against the plugins' record at each
 * enabled game's start: logged when the answer changes, and a mismatch is
 * said once, in the body of the next overlay that draws its page (core/app_stamp.h). */
static void test_overlay_app_mismatch_is_logged_and_said_once(void)
{
    reset();
    /* #101: the app from before the stamps (its eboot, no vjo_app.txt) beside newer plugins */
    app_files(NOENT, "", 1, 37, STAMP_21);
    activate(state.game_pid, "PCSG00415", VJO_GAME);
    TEST_CHECK(app_logs == 1);
    TEST_CHECK_(strstr(last_app_log, "overlay app MISMATCH: installed '' (0x80010002, app 1), plugins' '"
                       STAMP_21 "' (0x00000025)") != NULL, "%s", last_app_log);
    TEST_CHECK(!g_view.open && !g_view.strip_text[0]); /* nothing is drawn over the game */
    open_overlay();
    TEST_CHECK(notice_is(overlay_draws(), VJO_APP_NOTICE));
    view_publish(1, NULL, "Recognizing…", 0); /* the same overlay keeps it while it is open */
    TEST_CHECK(notice_is(overlay_draws(), VJO_APP_NOTICE));
    close_overlay();
    open_overlay();
    TEST_CHECK(g_view.open && !overlay_draws()); /* once */
    close_overlay();
    /* the next game start finds the same app: nothing new */
    next_game();
    open_overlay();
    TEST_CHECK(app_logs == 1 && !overlay_draws());
    close_overlay();
    /* the VPK installed meanwhile (VitaShell beside the running plugins): ok, logged once, no notice */
    app_files(37, STAMP_21, 1, 37, STAMP_21);
    next_game();
    TEST_CHECK(app_logs == 2 && strstr(last_app_log, "overlay app ok: installed '" STAMP_21 "'"));
    open_overlay();
    TEST_CHECK(!overlay_draws());
    close_overlay();
    /* an older app installed again: a mismatch again, said again */
    app_files(37, STAMP_20, 1, 37, STAMP_21);
    next_game();
    TEST_CHECK(app_logs == 3 && strstr(last_app_log, "overlay app MISMATCH: installed '" STAMP_20 "'"));
    open_overlay();
    TEST_CHECK(notice_is(overlay_draws(), VJO_APP_NOTICE));
    close_overlay();
    /* an overlay that cannot draw its page (no Paf memory for it, as 21 times in test.2's log): the
     * notice waits for the next overlay that does */
    app_files(37, STAMP_21, 1, 37, STAMP_21);
    next_game();
    app_files(NOENT, "", 1, 37, STAMP_21);
    next_game();
    TEST_CHECK(app_logs == 5);
    open_overlay(); /* not drawn */
    close_overlay();
    open_overlay();
    TEST_CHECK(notice_is(overlay_draws(), VJO_APP_NOTICE));
    close_overlay();
    open_overlay();
    TEST_CHECK(!overlay_draws());
    close_overlay();
    /* a mismatch that is gone before an overlay drew it leaves nothing to say */
    app_files(37, STAMP_21, 1, 37, STAMP_21);
    next_game();
    app_files(NOENT, "", 1, 37, STAMP_21);
    next_game();
    open_overlay(); /* not drawn */
    close_overlay();
    app_files(37, STAMP_21, 1, 37, STAMP_21);
    next_game();
    TEST_CHECK(app_logs == 8 && !g_view.notice);
    open_overlay();
    TEST_CHECK(!overlay_draws());
    close_overlay();
    /* an app newer than the plugins (the newer package's installer not run): the other notice */
    app_files(37, "vjoapp1 api=22 build=0123456789abcdef", 1, 37, STAMP_21);
    next_game();
    open_overlay();
    TEST_CHECK(notice_is(overlay_draws(), VJO_APP_NOTICE_NEWER));
    close_overlay();
    /* a disabled game starts no check */
    unsigned before = app_logs;
    app_files(NOENT, "", 1, 37, STAMP_21);
    game_enabled = 0;
    next_game();
    TEST_CHECK(app_logs == before);
    mem_free();
}

/* The check runs after the game is active and after the config load: its file reads never delay
 * the game's input or capture, and its log lines follow the start's own. */
static void test_overlay_app_check_runs_after_the_game_is_active(void)
{
    reset();
    app_files(37, STAMP_21, 1, 37, STAMP_21);
    start_order[0] = 0;
    activate(state.game_pid, "PCSG00415", VJO_GAME);
    TEST_CHECK_(!strcmp(start_order, "CASS"), "order %s", start_order);
    TEST_CHECK(logged("game PCSG00415 started") && app_logs == 1);
    /* a disabled game: no config load before it is refused, and no stamp read */
    start_order[0] = 0;
    game_enabled = 0;
    on_game_exit();
    activate(state.game_pid, "PCSG00415", VJO_GAME);
    TEST_CHECK_(!strchr(start_order, 'S') && !strchr(start_order, 'A'), "order %s", start_order);
    mem_free();
}

/* Nothing to compare, or nothing installed, is no mismatch: an older package
 * brings no record, the README lets the app be deleted, and a file the Shell
 * cannot read says nothing about the app. */
static void test_overlay_app_unknown_is_never_said(void)
{
    static const struct { int irc; const char *installed; int eboot, prc; const char *record; int state; } cases[] = {
        {NOENT, "", 1, NOENT, "", VJO_APP_UNKNOWN},             /* test.2's package: no record */
        {37, STAMP_20, 1, NOENT, "", VJO_APP_UNKNOWN},
        {37, STAMP_21, 1, 0, "", VJO_APP_UNKNOWN},             /* an empty record */
        {NOENT, "", 0, 37, STAMP_21, VJO_APP_UNKNOWN},          /* no app installed */
        {(int)0x80010013u, "", 1, 37, STAMP_21, VJO_APP_UNKNOWN}, /* the stamp not readable here */
        {37, STAMP_21, 1, (int)0x80010013u, "", VJO_APP_UNKNOWN},
        {37, STAMP_21, 1, 37, STAMP_21, VJO_APP_OK},
        {37, STAMP_21, 0, 37, STAMP_21, VJO_APP_OK},           /* the stamp decides, not the stat */
        {0, "", 1, 37, STAMP_21, VJO_APP_MISMATCH},            /* an empty stamp */
        {37, STAMP_20, 1, 37, STAMP_21, VJO_APP_MISMATCH},
        {NOENT, "", 1, 37, STAMP_21, VJO_APP_MISMATCH},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        reset();
        app_files(cases[i].irc, cases[i].installed, cases[i].eboot, cases[i].prc, cases[i].record);
        TEST_CHECK_(vjo_app_state(cases[i].irc, cases[i].installed, cases[i].eboot, cases[i].prc, cases[i].record) ==
                    cases[i].state, "case %u", i);
        activate(state.game_pid, "PCSG00415", VJO_GAME);
        TEST_CHECK_(app_logs == 1 && strstr(last_app_log, cases[i].state == VJO_APP_OK ? "overlay app ok: " :
                                            cases[i].state == VJO_APP_MISMATCH ? "overlay app MISMATCH: " :
                                            "overlay app unknown: "), "case %u: %s", i, last_app_log);
        open_overlay();
        TEST_CHECK_(!overlay_draws() == (cases[i].state != VJO_APP_MISMATCH), "case %u", i);
        close_overlay();
        mem_free();
    }
    TEST_CHECK(vjo_app_stamp_api(STAMP_21) == 21 && vjo_app_stamp_api("vjoapp1 api=7") == 7);
    TEST_CHECK(vjo_app_stamp_api("vjoapp2 api=21 build=0") == -1 && vjo_app_stamp_api("vjoapp1 api= build=") == -1);
    TEST_CHECK(vjo_app_stamp_api("vjoapp1 api=21x build=0") == -1 && vjo_app_stamp_api("") == -1);
}

TEST_LIST = {
    {"config_warnings_are_logged_once_per_change", test_config_warnings_are_logged_once_per_change},
    {"overlay_app_mismatch_is_logged_and_said_once", test_overlay_app_mismatch_is_logged_and_said_once},
    {"overlay_app_unknown_is_never_said", test_overlay_app_unknown_is_never_said},
    {"overlay_app_check_runs_after_the_game_is_active", test_overlay_app_check_runs_after_the_game_is_active},
#if !VJO_DEBUG
    {"normal_build_logs_one_line_per_job", test_normal_build_logs_one_line_per_job},
    {"normal_build_logs_the_clock_once_and_on_change", test_normal_build_logs_the_clock_once_and_on_change},
    {"normal_build_logs_a_failed_lookup_only", test_normal_build_logs_a_failed_lookup_only},
    {"normal_build_has_no_selftest", test_normal_build_has_no_selftest},
#endif
#if VJO_DEBUG
    {"selftest_keeps_the_user_region_and_auto_ocr", test_selftest_keeps_the_user_region_and_auto_ocr},
#endif
#ifdef VJO_WITH_VOCR
#if VJO_DEBUG
    {"selftest_stages_run_in_one_job_each_recorded_then_finish", test_selftest_stages_run_in_one_job_and_finish},
#endif
#if VJO_DEBUG
    {"selftest_waits_for_text_in_the_region_a_bounded_number_of_screens", test_selftest_waits_for_text_in_the_region},
#endif
    {"screen_changed_is_retried_quietly_at_a_bounded_pace", test_screen_changed_is_retried_quietly_at_a_bounded_pace},
    {"open_overlay_keeps_its_text_and_refreshes_it_quietly", test_open_overlay_refreshes_its_text_quietly},
    {"refresh_replacement_resets_dictionary_pages", test_refresh_replacement_resets_dictionary_pages},
    {"on_press_overlay_never_refreshes", test_on_press_overlay_never_refreshes},
    {"refresh_updates_the_subtitle_strip", test_refresh_updates_the_subtitle_strip},
    {"refresh_names_its_model_on_the_cards_only_when_it_replaces", test_refresh_names_its_model_on_the_cards_only_when_it_replaces},
    {"refresh_logs_quietly_and_a_failing_one_backs_off", test_refresh_logs_quietly_and_a_failing_one_backs_off},
#if VJO_DEBUG
    {"refresh_while_the_selftest_is_armed", test_refresh_while_the_selftest_is_armed},
#endif
#if VJO_DEBUG
    {"selftest_job_keeps_the_texts_capture", test_selftest_job_keeps_the_texts_capture},
#endif
#if VJO_DEBUG
    {"subtitles_on_during_the_selftest_job", test_subtitles_on_during_the_selftest_job},
#endif
#if VJO_DEBUG
    {"selftest_keeps_the_shown_texts_screen", test_selftest_keeps_the_shown_texts_screen},
#endif
#if VJO_DEBUG
    {"selftest_end_keeps_the_open_overlays_text", test_selftest_end_keeps_the_open_overlays_text},
#endif
#if VJO_DEBUG
    {"selftest_end_shows_the_open_overlays_text_in_the_strip", test_selftest_end_shows_the_open_overlays_text_in_the_strip},
#endif
#if VJO_DEBUG
    {"the_done_line_has_the_exchange_and_log_times", test_the_done_line_has_the_exchange_and_log_times},
#endif
#if VJO_DEBUG
    {"selftest_runs_after_the_first_text_in_its_own_job", test_selftest_runs_after_the_first_text_in_its_own_job},
#endif
#endif
    {"ledger_counts_result_memory_against_the_budget", test_ledger_counts_result_memory_against_the_budget},
    {"boot_guard_ok_after_a_stable_minute", test_boot_guard_ok_after_a_stable_minute},
#ifdef VJO_WITH_VOCR
    {"game_exit_during_a_lookup_frees_its_arena_after_the_job", test_audit_exit_during_lookup_frees_the_arena_after_the_job},
    {"game_exit_during_a_cursor_job_frees_its_arena_after_the_job", test_audit_exit_during_cursor_job_frees_the_arena_after_the_job},
    {"a_refresh_has_no_arena_and_leaves_the_shown_one", test_audit_refresh_has_no_arena},
    {"a_cursor_posted_while_publishing_runs_after_cmd_lock_is_released", test_audit_cursor_posted_while_publishing},
#endif
    {"subtitles_are_ocr_only", test_subtitles_are_ocr_only},
    {"removed_relay_opens_as_local", test_removed_relay_opens_as_local},
    {"input_block_refusal_keeps_overlay_closed", test_input_block_denied},
    {"disabled_game_retires_previous_job", test_disabled_game_retires_work},
    {"psp_launch_uses_its_identity", test_psp_launch_uses_its_identity},
    {"failed_join_keeps_allocations_for_retry", test_failed_join_keeps_allocations},
    {"anki_mining_enables_cross_and_keeps_the_pipeline", test_anki_mining_enables_cross_in_view},
    {"anki_stop_failure_keeps_worker_memory_for_retry", test_anki_stop_failure_keeps_worker},
#ifdef VJO_WITH_VOCR
    {"japanese_only_selects_first_letter_rejects_other_scripts_and_reloads_choices", test_japanese_only_selection_and_config_reload},
    {"no_Japanese_text_allocates_no_results_and_performs_no_dictionary_IO", test_no_japanese_selection_skips_dictionary_memory_and_io},
    {"character_open_reuses_background_OCR_without_dictionary_or_capture_work", test_character_open_reuses_background_text},
    {"word_Auto_prepares_first_match_and_reopen_reuses_only_current_cache", test_word_background_lookup_and_reopen_cache},
    {"word_lookup_with_the_bundled_dictionary_first_word_right_and_tap", test_word_lookup_with_the_bundled_dictionary},
#if VJO_DEBUG
    {"lookup_log_line_fits_with_worst_case_fields", test_lookup_log_line_fits_with_worst_case_fields},
#endif
    {"character_moves_coalesce_and_reject_old_inflight_definition", test_character_coalesces_and_drops_inflight_definition},
    {"character_close_reopen_restarts_lookup_and_retirement_rejects_old_results", test_character_close_reopen_and_retirement},
    {"character_scene_change_keeps_the_shown_text_and_region_change_retires_it", test_character_scene_change_keeps_the_shown_text},
    {"character_commands_preserve_close_region_priority_and_nonzero_generations", test_character_command_priority_and_generation_boundaries},
    {"dictionary_pages_read_one_entry_each_in_the_single_arena", test_dictionary_pages_read_one_entry_each},
    {"dictionary_count_change_looks_up_again_without_OCR", test_dictionary_count_change_relooks_up_without_ocr},
    {"dictionary_page_after_a_replaced_file_locates_again", test_dictionary_page_after_replaced_file_locates_again},
    {"dictionary_pages_OOM_keeps_text_and_disables_paging", test_dictionary_pages_oom_keeps_text_and_disables_paging},
    {"Shell_result_OOM_preserves_OCR_sentence_and_dictionary_backoff", test_shell_result_oom_preserves_recognition},
    {"Shell_result_OOM_preserves_maximum_filtered_worker_text", test_shell_result_oom_keeps_max_worker_text},
    {"local_lookup_uses_one_arena", test_local_lookup_single_arena},
    {"local_ocr_and_dictionary_phases_repeat_and_recover", test_local_phases_and_recovery},
#endif
    {"vocr_empty_region_reads_no_line_and_frees_the_workspace", test_vocr_empty_region},
#ifdef VJO_WITH_VOCR
    {"vocr_capture_follows_the_retry", test_vocr_capture_follows_the_retry},
    {"vocr_ran_names_outlive_the_job", test_vocr_ran_names_outlive_the_job},
#if VJO_DEBUG
    {"vocr_text_is_logged_raw_and_shown_normalised", test_vocr_text_is_logged_raw_and_shown_normalised},
#endif
#endif
    {"removed_engine_choice_VOCR2_or_ncnn_runs_the_local_model_with_its_fallback", test_removed_engine_choice_runs_the_local_engine},
    {"engine_cache_tracks_backend_local_models_and_storage", test_engine_cache_tracks_backend_models_and_storage},
    {"vocr_config_parses_and_validates", test_vocr_config},
    {"vocr_model_change_invalidates_cache", test_vocr_model_change_invalidates_cache},
#ifdef VJO_WITH_VOCR
    {"vocr_missing_model_releases_workspace", test_vocr_missing_model_releases_workspace},
    {"vocr_storage_errors_have_specific_messages", test_vocr_storage_errors_are_specific},
#if VJO_DEBUG
    {"vocr_logs_memory_check_frames_and_lines", test_vocr_logs_memory_check_and_frames},
#endif
#if VJO_DEBUG
    {"vocr_job_log_is_appended_in_batches", test_vocr_job_log_is_appended_in_batches},
#endif
    {"vocr_request_carries_the_job_policy", test_vocr_request_carries_the_job_policy},
    {"vocr_oom_advice_names_the_stage", test_vocr_oom_advice_names_the_stage},
    {"vocr_unusable_model_uses_the_fallback", test_vocr_unusable_model_uses_the_fallback},
    {"vocr_memory_fallback_is_logged", test_vocr_memory_fallback_is_logged},
    {"vocr_transient_model_failure_is_not_sticky", test_vocr_transient_model_failure_is_not_sticky},
    {"vocr_model_error_retry_limits", test_vocr_model_error_retry_limits},
    {"vocr_refused_fallback_is_labelled", test_vocr_refused_fallback_is_labelled},
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
    {"ps_with_the_overlay_open_releases_input_and_leaves_touch_to_shell",
     test_ps_with_the_overlay_open_releases_input_and_leaves_touch_to_shell},
    {"foreground_switch_gives_the_touch_region_back", test_foreground_switch_gives_the_touch_region_back},
    {"close_just_before_ps_leaves_touch_to_shell", test_close_just_before_ps_leaves_touch_to_shell},
    {NULL, NULL}
};
