/* Control thread (kernel events, overlay state, auto/on-press policy) and
 * network thread (a job of the game worker: vita-vn-ocr in the game; then the
 * dictionary). The overlay is offline: Google Lens was removed (2026-10-15). */
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#if !VJO_DEBUG
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#endif

#include "../core/boot_guard.h"
#include "../core/app_stamp.h"
#include "../include/vjo_ledger_budget.h"
#include "../core/regions.h"
#include "../core/local_dict.h"
#include "../core/textfilter.h"
#include "../core/textnorm.h"
#include "../core/utf.h"
#ifdef VJO_WITH_VOCR
#include "../core/local_ocr.h"
#include "game_ocr_client.h"
#include "../include/vjo_vocr_sizes.h"
#endif
#include "shell.h"
#include "touch_hold.h"

/* Result memory: one 64 KiB arena for the local dictionary's lookup, taken
 * after OCR and freed before the next job's OCR (the game holds the job's
 * memory meanwhile). OCR-only jobs use bounded static text. Control-thread
 * config/region/messages use separate scratch. Every job is local: Google
 * Lens and the Hachidori relay, the network paths (two 256 KiB arenas and a
 * connection pool), were removed. */
#define LOCAL_RESULT_ARENA_SIZE (64 * 1024)
#define SCRATCH_SIZE      (24 * 1024)
typedef char config_fits_scratch[(SCRATCH_SIZE >= VJO_CONFIG_TEXT_MAX + 1) ? 1 : -1];

#define POLL_TIMEOUT_US   50000
#define BACKOFF_MIN_US    2000000LL
#define BACKOFF_MAX_US    60000000LL
#define SUBTITLE_GAP_US   1000000LL /* between subtitle jobs */
#define UNSETTLED_US      2000000LL /* subtitles: a region that keeps changing is recognized this often */
/* The open overlay's quiet refresh (#81 request 4): at most one per settled
 * screen and REFRESH_GAP_US apart, and only once no lookup has started for
 * REFRESH_IDLE_US; a lookup cancels a refresh in progress. */
#define REFRESH_GAP_US      2000000LL
#define REFRESH_IDLE_US     1500000LL
/* VJO_E_SCREEN_CHANGED (the screen changed under every read of a job): the
 * next try starts at once, from the second in a row SCREEN_RETRY_MS later,
 * doubling up to SCREEN_RETRY_MAX_MS. The try itself waits in the game for the
 * text to hold still (vocr_run_one's request). Nothing is shown for either. */
#define SCREEN_RETRY_MS      250u
#define SCREEN_RETRY_MAX_MS 4000u

VjoView g_view;

static SceUID view_lock = -1;
static SceUID cmd_lock = -1;
static SceUID ctl_thread = -1, net_thread = -1;
static int threads_started; /* bits: network=1, control=2 */
static SceUID net_evf = -1;
static volatile int running = 1;

static SceUID mem_uid = -1;
static void *mem_heap; /* bounded allocation from ScePaf's existing heap */
static size_t mem_bytes; /* what either holds, for the ledger */
static VjoArena results[2];
static VjoArena scratch; /* static; control thread only */
static uint8_t scratch_mem[SCRATCH_SIZE];
static int active = -1;               /* results[] index shown/cached */
static uint32_t cache_scene; /* VjoState.scene of the cached result, 0 = unknown */
static uint32_t cache_seq;   /* VjoState.region_seq of its capture */
static int cache_ok;
static VjoOverlayData cache_data[2];
/* Local text outlives the single dictionary arena. Only an OCR job rewrites it,
 * after start_job has detached its prior view. Cursor jobs only read it. */
static char local_text[VJO_STRIP_MAX];
static VjoEntryList local_header;
static volatile int local_text_valid;
static uint32_t local_text_generation, local_text_game_generation;
static uint32_t local_text_scene, local_text_capture_seq;
static uint32_t local_cursor_byte, local_cursor_seq;
static int cursor_waiting;
static int job_cursor_only;
static uint32_t job_cursor_byte, job_cursor_seq, job_text_generation;
/* Dictionary paging at the cursor (local_dictionary_results > 1): the
 * requested page shares the cursor sequence. Page locators live at the start
 * of the single result arena (cache_data[0].pages); they are reused only for
 * the text generation and result limit that built them (network thread). */
static int local_page, job_page;
static uint32_t pages_generation;
static int pages_limit;

static VjoConfig cfg;      /* control thread */
static VjoConfig job_cfg;  /* snapshot used by the network thread */
static int anki_on;        /* vjo_anki_configure(&cfg): enter saves cards to the queue */
static VjoPlatform plat;
static char title_id[12];   /* set while a game is active ("" otherwise) */
static char job_title[12];  /* the job's game (title_id when the job started), for its log lines */
/* Scene IDs survive kernel resets. A foreground boundary must also reject
 * results captured for the preceding game, even before a new scene arrives. */
static uint32_t game_generation;
static uint32_t job_generation;

/* Control-thread state. The overlay and the network job are independent: an
 * auto job runs while the overlay is closed, the overlay can open while a
 * job runs, and a result can be shown while the next job runs.
 *   overlay: OV_CLOSED, OV_OPEN, or OV_OPEN_OLD_JOB (opened while a job was
 *            already running: its result may be for an earlier screen, which
 *            is checked when it ends)
 *   job:     job_running (set here, finished via job_done from the network
 *            thread)
 *   subtitles: on/off (the strip shows each job's sentence while the
 *            overlay is closed)
 * plus inputs to the background policy: stable_pending (a REGION_STABLE
 * or REGION_QUIET not acted on yet) and the error backoffs, one per stage (a
 * failing dictionary does not stop the subtitles, which need only OCR).
 * Each event has one handler below (on_*). */
enum { OV_CLOSED, OV_OPEN, OV_OPEN_OLD_JOB };
static int ov = OV_CLOSED;
static int subtitles; /* written by set_subtitles only */
static int input_block_error;
static int stable_pending;
/* The open overlay keeps the text it shows (#81 request 4): a lookup, a page
 * or a card always uses it. A settled screen that the shown text was not read
 * from (refresh_pending) gets a refresh: a quiet OCR job that leaves the text,
 * its lookup and the result arena alone and replaces the text only when it
 * reads a different one (on_refresh_done). refresh_after_us paces them. */
static int refresh_pending;
static int64_t refresh_after_us;
static int64_t lookup_us; /* the last lookup's start: a refresh waits REFRESH_IDLE_US after it */
/* VJO_E_SCREEN_CHANGED results in a row, and when the next try may start;
 * open_retry: the open overlay waits for a text and tries again then. */
static int screen_changed_streak;
static int64_t screen_retry_until;
static int open_retry;
/* The overlay waits for the running job ("Recognizing…"), or shows its
 * sentence before the lookup: job_text_list, in the job's arena, which
 * must not be shown once another job starts in it. */
static int view_waiting, view_job_text;
static VjoEntryList job_text_list;
typedef struct {
    int64_t until, us;
} Backoff;
static Backoff ocr_backoff, dict_backoff;
static int64_t job_started_us;

/* network job */
#define NET_EV_JOB  1u
#define NET_EV_QUIT 2u
static volatile int job_running;
static volatile int job_done;
static volatile int job_idx;
static volatile int job_rc;
static volatile int job_lookup;     /* the job looks the words up (else OCR only) */
/* The job is a refresh of the open overlay's text (local pipeline): it writes
 * neither local_text nor the result arena; its text stays in the worker's
 * result (cache_data[1].ocr_text) until on_refresh_done compares the two. */
static volatile int job_refresh;
static const char *refresh_model; /* the model that read the refresh's text (job_cfg's, or a constant) */
/* The job is the self-test's (shell/selftest.c, selftest_job_due): its stages
 * run on the screen the last job read text from, with the overlay closed, and
 * it shows nothing: the shown text, its lookup and the result arenas stay, and
 * its captures do not stand in for the shown text's (vocr_note_capture). */
#if VJO_DEBUG
static volatile int job_selftest;
#else
#define job_selftest 0 /* the normal build has no self-test (shell.h) */
#endif
/* The last OCR job that read text in the region: its capture, and the game it
 * was read in (0: the last job read none). Network thread; read once its job
 * is done. */
static uint32_t text_scene, text_seq, text_generation;
static int64_t idle_since_us; /* the last job's end or the overlay's close */
#if VJO_DEBUG
static int64_t job_game_us;   /* the job's exchanges with the game worker (network thread) */
#endif
static int job_prepares;            /* ...the first word, before the overlay opens (log) */
/* The cached result's text when the job started (NULL: no usable lookup).
 * Its arena is not touched while the job runs (the job fills the other). */
/* The job's capture is done (job_capture_scene, job_capture_seq: its
 * VjoState.capture_scene and region_seq). */
static volatile int job_captured;
static volatile uint32_t job_capture_scene, job_capture_seq;
/* The job is for an earlier screen: its requests were aborted, its result
 * is dropped. */
static volatile int job_cancelled;
/* cache_data[job_idx].sentence is ready (the lookup may still run): the
 * net thread only appends to the arena, so the control thread may read it */
static volatile int job_text_ready;

static volatile int pending_cmd;
static VjoRect pending_rect;
static uint32_t pending_cursor_byte, pending_cursor_generation;
static void start_cursor_job(void);
static void note_pages(int rc, const VjoOverlayData *out);

static uint32_t advance_generation(uint32_t *value)
{
    if (++*value == 0) *value = 1;
    return *value;
}

static void retire_local_text(int new_generation)
{
    sceKernelLockMutex(cmd_lock, 1, NULL);
    vjo_view_lock();
    local_text_valid = 0;
    cursor_waiting = 0;
    local_page = 0;
    if (new_generation) advance_generation(&local_text_generation);
    g_view.list = NULL;
    g_view.local_cursor_mode = 0;
    g_view.dict_page = g_view.dict_pages = 0;
    g_view.version++;
    vjo_view_unlock();
    sceKernelUnlockMutex(cmd_lock, 1);
}

void vjo_view_lock(void)
{
    sceKernelLockMutex(view_lock, 1, NULL);
}

void vjo_view_unlock(void)
{
    sceKernelUnlockMutex(view_lock, 1);
}

void vjo_post_command(int cmd, const VjoRect *rect)
{
    sceKernelLockMutex(cmd_lock, 1, NULL);
    pending_cmd = cmd;
    if (rect)
        pending_rect = *rect;
    sceKernelUnlockMutex(cmd_lock, 1);
}

/* Under cmd_lock and the view lock: queues the cursor job for the current
 * local_cursor_byte/local_page and detaches the shown entry before its arena
 * is reused. */
static void post_local_lookup(uint32_t text_generation)
{
    advance_generation(&local_cursor_seq);
    pending_cursor_byte = local_cursor_byte;
    pending_cursor_generation = text_generation;
    pending_cmd = VJO_CMD_CURSOR;
    local_header = (VjoEntryList){.header = local_text};
    g_view.list = &local_header; /* detach entries before their arena is reset */
    g_view.cursor_byte = local_cursor_byte;
    g_view.dict_page = local_page;
    g_view.list_seq++;
    sceClibSnprintf(g_view.status, sizeof(g_view.status), "Looking up words…");
    g_view.status_is_error = 0;
    g_view.version++;
}

void vjo_post_cursor(uint32_t byte_offset, uint32_t text_generation)
{
    /* This order is also used by cursor completion: a newer request cannot
     * race past the stale-result check and republish the old definition. */
    sceKernelLockMutex(cmd_lock, 1, NULL);
    if (pending_cmd != VJO_CMD_NONE && pending_cmd != VJO_CMD_CURSOR) {
        sceKernelUnlockMutex(cmd_lock, 1);
        return;
    }
    vjo_view_lock();
    if (!g_view.open || !g_view.local_cursor_mode || !local_text_valid ||
        !text_generation || text_generation != local_text_generation) {
        vjo_view_unlock();
        sceKernelUnlockMutex(cmd_lock, 1);
        return;
    }
    size_t len = sceClibStrnlen(local_text, sizeof(local_text));
    if ((len ? byte_offset >= len : byte_offset != 0) ||
        vjo_utf8_cursor_step(local_text, len, byte_offset, 0) != byte_offset ||
        (cfg.japanese_only_selection &&
         (byte_offset >= len || vjo_utf8_japanese_cursor_step(local_text, len, byte_offset, 0) != byte_offset))) {
        vjo_view_unlock();
        sceKernelUnlockMutex(cmd_lock, 1);
        return;
    }
    local_cursor_byte = byte_offset;
    local_page = 0; /* a new word starts at its best match */
    g_view.dict_pages = 0;
    post_local_lookup(text_generation);
    vjo_view_unlock();
    sceKernelUnlockMutex(cmd_lock, 1);
}

int vjo_post_page(int page, uint32_t text_generation)
{
    int accepted = 0;
    sceKernelLockMutex(cmd_lock, 1, NULL);
    if (pending_cmd == VJO_CMD_NONE || pending_cmd == VJO_CMD_CURSOR) {
        vjo_view_lock();
        /* Only pages of the published (or already pending) result for this
         * text and cursor; dict_pages is reset whenever either changes. */
        if (g_view.open && g_view.local_cursor_mode && local_text_valid && text_generation &&
            text_generation == local_text_generation && page >= 0 && page < g_view.dict_pages &&
            page != g_view.dict_page) {
            local_page = page;
            post_local_lookup(text_generation);
            accepted = 1;
        }
        vjo_view_unlock();
    }
    sceKernelUnlockMutex(cmd_lock, 1);
    return accepted;
}

static int64_t now_us(void)
{
    return (int64_t)sceKernelGetProcessTimeWide();
}

/* The system clock in ms, kernel.txt's (the kernel logs ksceKernelGetSystemTimeWide):
 * the foreground transitions in both logs line up (#104). */
unsigned vjo_log_ms(void)
{
    return (unsigned)(sceKernelGetSystemTimeWide() / 1000);
}

/* Dictionary file reads since the last lookup began (platform_vita.c counts). */
uint32_t vjo_dict_reads, vjo_dict_read_bytes;

#if VJO_DEBUG
/* Up to `chars` characters of s (the line at most) into out, for the log. */
static void log_text(char *out, size_t cap, const char *s, size_t len, int chars)
{
    size_t i = 0;
    while (i < len && chars-- > 0) {
        size_t j = i;
        uint32_t cp = vjo_utf8_next(s, len, &j);
        if (cp < 0x20 || j >= cap) break;
        i = j;
    }
    sceClibMemcpy(out, s, i);
    out[i] = 0;
}

/* One log.txt line per dictionary lookup: why (first word during OCR, on
 * open, cursor or tap, dictionary page), the cursor byte, the matched span
 * in characters, the dictionary file, the page, the file reads, rc and the
 * time; then the text at the cursor, the entry, the source named in its
 * definition and its frequency badge (" #1234", " unranked" or none). The
 * numbers come first and the texts are bounded (32 + 31 + 31 + 23 + 15
 * bytes, whole characters), so the line fits vjo_log's 254 bytes. */
static void log_lookup(const char *why, const VjoOverlayData *out, uint32_t cursor, int rc, int64_t t0)
{
    char query[33], head[32], reading[32], source[24], dict[8], rank[VJO_RANK_BADGE_MAX];
    const VjoEntry *e = out->list.n_entries > 0 ? &out->list.entries[0] : NULL;
    const char *text = out->sentence ? out->sentence : "";
    size_t len = sceClibStrnlen(text, VJO_LOCAL_MAX_TEXT);
    int chars = 0, page = out->page, files = vjo_local_names_count(job_cfg.local_dictionaries);
    /* Which file: known with one, or from the page locators (2-8 shown). */
    int file = files == 1 ? 1 : out->pages && page >= 0 && page < out->pages->count ? out->pages->page[page].dict + 1 : 0;
    query[0] = head[0] = reading[0] = source[0] = rank[0] = 0;
    if (cursor <= len) log_text(query, sizeof(query), text + cursor, len - cursor, 8);
    if (e) {
        size_t i = (size_t)e->hl_start;
        while (i < (size_t)e->hl_end && i < len) { vjo_utf8_next(text, len, &i); chars++; }
        log_text(head, sizeof(head), e->vocab->spelling, sceClibStrnlen(e->vocab->spelling, 256), 10);
        if (e->vocab->reading != e->vocab->spelling)
            log_text(reading, sizeof(reading), e->vocab->reading, sceClibStrnlen(e->vocab->reading, 256), 10);
        if (e->vocab->n_meanings && e->vocab->meanings[0][0] == '[') { /* "[Daijirin4 Plain]\n..." */
            const char *m = e->vocab->meanings[0] + 1;
            size_t n = 0;
            while (n + 1 < sizeof(source) && m[n] && m[n] != ']') n++;
            if (m[n] == ']') { sceClibMemcpy(source, m, n); source[n] = 0; }
        }
        vjo_rank_badge(rank, e->vocab->rank, out->list.source);
    }
    if (!e) sceClibSnprintf(dict, sizeof(dict), "-");
    else if (file) sceClibSnprintf(dict, sizeof(dict), "%d/%d", file, files);
    else sceClibSnprintf(dict, sizeof(dict), "?/%d", files);
    vjo_log("lookup %s at %u: %d chars, dictionary %s, page %d/%d, %u reads %u B, rc=%d, %u ms; \"%s\" -> %s%s%s%s%s%s%s%s",
            why, (unsigned)cursor, chars, dict, page + 1, vjo_overlay_page_count(out),
            (unsigned)vjo_dict_reads, (unsigned)vjo_dict_read_bytes, rc, (unsigned)((now_us() - t0) / 1000),
            query, e ? head : "no entry", reading[0] ? " (" : "", reading, reading[0] ? ")" : "",
            source[0] ? " [" : "", source, source[0] ? "]" : "", rank);
}
#else
/* The normal build logs a lookup only when it failed: no text, no timing
 * (docs/release-variants.md). */
static void log_lookup(const char *why, const VjoOverlayData *out, uint32_t cursor, int rc, int64_t t0)
{
    (void)out;
    (void)t0;
    if (rc)
        vjo_log("lookup %s at %u failed: rc=%d", why, (unsigned)cursor, rc);
}
#endif

/* Does a result for scene `a` show the current scene `b`? */
static int same_scene(uint32_t a, uint32_t b)
{
    return a != 0 && a == b;
}

/* Does a result for `scene`, from region capture `seq`, show the screen in
 * st? Its scene is the current one, or its capture still shows the screen
 * (checked now): an icon that kept changing when it was taken got masked
 * since, or is all that changes. */
static int shows_screen(uint32_t scene, uint32_t seq, const VjoState *st)
{
    return same_scene(scene, st->scene) ||
           (seq && seq == st->region_seq && same_scene(st->region_scene, st->scene));
}

/* ---------------- memory ---------------- */

static void mem_free(void);

static int has_result_memory(void)
{
    return mem_heap != NULL || mem_uid >= 0;
}

/* The lookup's result arena, taken when a lookup needs it (after OCR). */
static int mem_alloc(void)
{
    void *base = NULL;
    size_t bytes = LOCAL_RESULT_ARENA_SIZE;
    if (has_result_memory())
        return 0;
#ifdef VJO_PAF_ALLOC
    /* Games can exhaust USER while SceShell still has reserved heap pages.
     * The result arena has one bounded owner. */
    mem_heap = vjo_shell_heap_alloc_for(VJO_SL_PAF_RESULTS, bytes);
    if (!mem_heap) {
        vjo_log("result workspace Paf allocation failed (%u KiB)", (unsigned)(bytes >> 10));
        return -1;
    }
    base = mem_heap;
    mem_bytes = bytes;
#else
    /* Host tests only: the Vita build never takes partition pages here. */
    bytes = (bytes + 0xFFF) & ~(size_t)0xFFF;
    mem_uid = vjo_shell_memblock_alloc(VJO_SL_USERRW_RESULTS, "VjoShellMem", bytes, &base);
    if (mem_uid < 0)
        return mem_uid;
    mem_bytes = bytes;
#endif
    vjo_arena_init(&results[0], (uint8_t *)base, bytes);
    vjo_arena_init(&results[1], NULL, 0);
    vjo_dlog("local dictionary workspace: %u bytes", (unsigned)bytes);
    active = -1;
    cache_ok = 0;
    return 0;
}

/* Called by the idle control thread or by the network thread owning its job. */
static void mem_free(void)
{
    if (has_result_memory()) {
        vjo_view_lock();
        g_view.list = NULL;
        vjo_view_unlock();
        if (mem_heap)
            vjo_shell_heap_free(VJO_SL_PAF_RESULTS, mem_heap, mem_bytes);
        else
            vjo_shell_memblock_free(VJO_SL_USERRW_RESULTS, mem_uid, mem_bytes);
    }
    mem_heap = NULL;
    mem_uid = -1;
    mem_bytes = 0;
    vjo_arena_init(&results[0], NULL, 0);
    vjo_arena_init(&results[1], NULL, 0);
    cache_data[0].pages = cache_data[1].pages = NULL; /* their locators were in the arena */
    active = -1;
    cache_ok = 0;
}

/* ---------------- view ---------------- */

static void view_publish(int open, const VjoEntryList *list, const char *status, int is_error)
{
    vjo_view_lock();
    __atomic_store_n(&g_view.open, open, __ATOMIC_RELEASE); /* also read unlocked (overlay frame) */
    g_view.list = list;
    g_view.list_seq++;
    sceClibSnprintf(g_view.status, sizeof(g_view.status), "%s", status ? status : "");
    g_view.status_is_error = is_error;
    g_view.local_cursor_mode = local_text_valid && list &&
                               list->header == local_text;
    g_view.cursor_byte = local_cursor_byte;
    /* The static header placeholder keeps the requested page while its
     * lookup runs; a published result carries its own page count. */
    if (list != &local_header) {
        const VjoOverlayData *shown = list && active >= 0 && list == &cache_data[active].list
                                      ? &cache_data[active] : NULL;
        g_view.dict_page = shown && g_view.local_cursor_mode ? shown->page : 0;
        g_view.dict_pages = shown && g_view.local_cursor_mode ? vjo_overlay_page_count(shown) : 0;
    }
    g_view.japanese_only_selection = cfg.japanese_only_selection;
    g_view.highlight_mode = cfg.highlight_mode;
    g_view.frequency_badge = cfg.frequency_badge;
    g_view.stick_lookup_ms = cfg.stick_lookup_ms;
    g_view.text_generation = local_text_generation;
    g_view.font_size_ja = cfg.font_size_ja;
    g_view.font_size_en = cfg.font_size_en;
    g_view.anki_enabled = anki_on;
    g_view.n_warnings = cfg.n_warnings;
    sceClibMemcpy(g_view.warnings, cfg.warnings, sizeof(g_view.warnings));
    g_view.version++;
    vjo_view_unlock();
}

const VjoLocalPages *vjo_view_pages(int *page)
{
    for (int k = 0; k < 2; k++)
        if (g_view.list == &cache_data[k].list && cache_data[k].pages) {
            *page = cache_data[k].page;
            return cache_data[k].pages;
        }
    return NULL;
}

static void view_show_cache(void)
{
    view_waiting = view_job_text = 0;
    VjoOverlayData *d = &cache_data[active];
    size_t mark = vjo_arena_mark(&scratch);
    const char *err = d->err.rc ? vjo_err_text(&scratch, d->failed_stage, &d->err) : NULL;
    view_publish(1, &d->list, err, err != NULL);
    vjo_arena_release(&scratch, mark);
}

/* ---------------- network thread ---------------- */

/* ---- OCR: vita-vn-ocr in the game worker ---- */

/* Out-of-memory advice, by where the memory ran out (#53). Each fits the
 * status line after "Text recognition failed: not enough memory. ". */
#define OOM_TEXT_BUFFER "The recognized text did not fit the overlay's text buffer. Select a tighter " \
    "dialogue region with Square."
#define OOM_GAME_REGION "The game has too little free memory for OCR of this region. Select a tighter " \
    "dialogue region with Square."
#define OOM_GAME_NONE "The game has too little free memory for any OCR right now (it keeps a " \
    "reserve for the game). Try again later in the game, or restart it."
#define OOM_GAME_SMALLEST "The game has too little free memory for the OCR model's weights. Select " \
    "a tighter dialogue region with Square, or try again later."
#define OOM_GAME_ALLOCATION "The game's memory could not be allocated although the memory check " \
    "passed. Try again."
#define OOM_GAME_UNKNOWN "The game's free memory could not be checked (see log.txt), so OCR did not start."

#ifdef VJO_WITH_VOCR
/* Which part of the game worker's memory check failed: no OCR can fit
 * beside the reserve; the region's job workspace; the model's weights; an
 * allocation after a passed check; or the check itself. A job asks by tiers
 * (API 21): its last check is its smallest tier's. The advice names no model,
 * size or reserve: the screen never says which size ran or was tried (owner
 * 2026-10-15 04:54Z); log.txt has the tier lines. */
static const char *game_oom_detail(const VjoGameOcrResult *r)
{
    int64_t job, free_bytes;
    if (!r->reserve_bytes) return NULL; /* not from the memory check: no specific advice */
    if (r->free_user < 0) return OOM_GAME_UNKNOWN;
    /* With a smaller tier, need_* are its own: the last, smallest check. */
    free_bytes = (int64_t)r->free_user + r->release_bytes;
    job = (int64_t)r->reserve_bytes + r->need_workspace;
    if (free_bytes >= job + r->need_weights) return OOM_GAME_ALLOCATION;
    if (free_bytes < (int64_t)r->reserve_bytes) return OOM_GAME_NONE;
    if (free_bytes < job) return OOM_GAME_REGION;
    return OOM_GAME_SMALLEST;
}
#endif

#ifdef VJO_WITH_VOCR
/* ---- the game worker's one job slot (API 23: shared with anki.c's screenshots) ----
 * The kernel holds one job and its one result: an exchange is submit to read, and
 * another thread's submission in between would replace the result this one waits
 * for. game_lock is held for each exchange; a screenshot waiting for it
 * (shot_request) has a quiet refresh in progress yield, as a lookup does, and
 * keeps the next one from starting. */
static SceUID game_lock = -1;
static volatile int shot_request;

/* Waits for the slot in 50 ms steps, while stop(NULL) says no, at most wait_us.
 * Before vjo_worker_start made the lock no other thread submits (the Anki thread
 * starts after it): the slot is this thread's. */
static int game_job_lock(int (*stop)(void *), int64_t wait_us)
{
    int64_t until = now_us() + wait_us;
    for (;;) {
        unsigned int timeout = 50000;
        if (game_lock < 0 || sceKernelLockMutex(game_lock, 1, &timeout) >= 0)
            return 0;
        if (stop(NULL) || now_us() >= until)
            return -1;
    }
}

int vjo_game_job_begin(int (*stop)(void *), int64_t wait_us)
{
    shot_request = 1;
    __sync_synchronize();
    if (!game_job_lock(stop, wait_us))
        return 0;
    shot_request = 0;
    return -1;
}

static void game_job_unlock(void)
{
    if (game_lock >= 0)
        sceKernelUnlockMutex(game_lock, 1);
}

void vjo_game_job_end(void)
{
    game_job_unlock();
    shot_request = 0;
}

/* A screenshot waits for the slot or holds it (control thread: no refresh starts). */
static int shot_waiting(void)
{
    __sync_synchronize();
    return shot_request;
}

static int vocr_cancelled(void *ud)
{
    (void)ud;
    __sync_synchronize();
    return !running || job_cancelled || (job_refresh && shot_request);
}
static int ocr_submit(void *ud, const VjoGameOcrRequest *r) { (void)ud; return vjoOcrSubmit(r); }
static int ocr_read(void *ud, uint32_t seq, VjoGameOcrResult *r) { (void)ud; return vjoOcrRead(seq, r); }
static int ocr_cancel(void *ud, uint32_t seq) { (void)ud; return vjoOcrCancel(seq); }
static int64_t ocr_clock(void *ud) { (void)ud; return now_us(); }
static void ocr_delay(void *ud, uint32_t us) { (void)ud; sceKernelDelayThread(us); }
const VjoGameOcrClient vjo_game_client = {NULL, ocr_submit, ocr_read, ocr_cancel, ocr_clock, ocr_delay};

/* The job's headroom: free USER (+ another model's resident weights, released
 * first) minus the reserve and what the job needs; negative means refused.
 * Clamped to int32 for the log. */
static int32_t vocr_headroom(const VjoGameOcrResult *r)
{
    int64_t h = (int64_t)r->free_user + r->release_bytes - (int64_t)r->reserve_bytes -
                (int64_t)r->need_workspace - (int64_t)r->need_weights;
    return h > INT32_MAX ? INT32_MAX : h < INT32_MIN ? INT32_MIN : (int32_t)h;
}

/* The game worker's memory check and per-line recognition, after the job
 * line: free USER against what the job needed (also for a refused job), its
 * headroom, and each line's crop, CTC frames and time, i.e. ms per frame on
 * this console. */
static void log_vocr_measurements(const VjoGameOcrResult *r)
{
#if VJO_DEBUG
    char buf[224];
    size_t n = 0;
    if (r->reserve_bytes && r->free_user < 0)
        vjo_log("game vocr memory: free USER unknown (error 0x%08X), needs workspace %u + weights %u + "
                "reserve %u: check refused", (unsigned)r->free_user, (unsigned)r->need_workspace,
                (unsigned)r->need_weights, (unsigned)r->reserve_bytes);
    else if (r->reserve_bytes && r->release_bytes)
        /* Another model's resident weights, released before this job's. */
        vjo_log("game vocr memory: free USER=%d + own weights %u, needs workspace %u + weights %u + "
                "reserve %u: check %s, headroom %d", (int)r->free_user, (unsigned)r->release_bytes,
                (unsigned)r->need_workspace, (unsigned)r->need_weights, (unsigned)r->reserve_bytes,
                vocr_headroom(r) >= 0 ? "passed" : "refused", (int)vocr_headroom(r));
    else if (r->reserve_bytes)
        vjo_log("game vocr memory: free USER=%d, needs workspace %u + weights %u + reserve %u: check %s, headroom %d",
                (int)r->free_user, (unsigned)r->need_workspace, (unsigned)r->need_weights,
                (unsigned)r->reserve_bytes, vocr_headroom(r) >= 0 ? "passed" : "refused", (int)vocr_headroom(r));
    if (r->frames)
        vjo_log("game vocr speed: %u frames in %u us, %u us per frame", (unsigned)r->frames,
                (unsigned)r->recognize_us, (unsigned)(r->recognize_us / r->frames));
    /* API 20: the cascade's model, from its step (its re-reads are not in recognize_us) */
    for (uint32_t i = 0; i < r->n_steps && i < VJO_GAME_OCR_STEPS; i++) {
        const VjoGameOcrStep *s = &r->step[i];
        uint32_t f = s->bytes & 0xFFFFu;
        if (s->id == VJO_GSTEP_CASCADE && f)
            vjo_log("game vocr cascade speed: %u frames in %u us, %u us per frame", (unsigned)f,
                    (unsigned)(s->us - 1u), (unsigned)((s->us - 1u) / f));
    }
    /* Crop WxH, CTC frames and time of each line; 6 per log line (256 bytes each). */
    for (uint32_t i = 0; i < r->recognized && i < VJO_GAME_OCR_LINES; i++) {
        const VjoGameOcrLine *l = &r->line[i];
        int w;
        if (!n) {
            w = sceClibSnprintf(buf, sizeof(buf), i ? "game vocr lines (cont):" : "game vocr lines:");
            n = w > 0 && (size_t)w < sizeof(buf) ? (size_t)w : 0;
        }
        w = sceClibSnprintf(buf + n, sizeof(buf) - n, " %ux%u %uf %u.%ums", l->width, l->height,
                            l->frames, (unsigned)(l->us / 1000), (unsigned)(l->us % 1000 / 100));
        /* API 20: the line's confidence (with the LM) and whether the LM changed it */
        if (w > 0 && n + (size_t)w < sizeof(buf) && (l->flags & 0x3FFu) != 0x3FFu) {
            int c = sceClibSnprintf(buf + n + w, sizeof(buf) - n - (size_t)w, " c%u%s%s", l->flags & 0x3FFu,
                                    l->flags & 0x8000u ? "*" : "", l->flags & 0x0400u ? "^" : "");
            if (c > 0) w += c;
        }
        n = w > 0 && n + (size_t)w < sizeof(buf) ? n + (size_t)w : sizeof(buf) - 1;
        if (i % 6 == 5 || i + 1 == r->recognized || i + 1 == VJO_GAME_OCR_LINES) {
            vjo_log("%s", buf);
            n = 0;
        }
    }
#else
    /* The normal build: the memory check's numbers only when it refused the job. */
    if (r->reserve_bytes && r->free_user < 0)
        vjo_log("game vocr memory: free USER unknown (error 0x%08X): check refused", (unsigned)r->free_user);
    else if (r->reserve_bytes && vocr_headroom(r) < 0)
        vjo_log("game vocr memory: free USER=%d + own weights %u, needs workspace %u + weights %u + reserve %u: "
                "check refused", (int)r->free_user, (unsigned)r->release_bytes, (unsigned)r->need_workspace,
                (unsigned)r->need_weights, (unsigned)r->reserve_bytes);
#endif
}

#if VJO_DEBUG /* the debug build's job line */
/* The game worker's memory check for the job it reported (with a smaller tier,
 * that tier's) refused it. */
static int fallback_check_refused(const VjoGameOcrResult *r)
{
    return r->reserve_bytes && (r->free_user < 0 ||
           (int64_t)r->free_user + r->release_bytes <
               (int64_t)r->need_workspace + r->need_weights + r->reserve_bytes);
}
#endif

/* Model tiers whose file failed to open or verify in this game (missing, wrong
 * or corrupt): bit t - 1 for tier t. Later jobs of the same game leave them out
 * (vocr_tiers). */
static unsigned vocr_unusable;
static uint32_t vocr_unusable_generation;

/* The tiers a job may run: the configured one and every smaller one, less the
 * ones found unusable in this game. */
static unsigned vocr_tiers(void)
{
    int top = job_cfg.vocr_tier >= 1 && job_cfg.vocr_tier <= (int)VJO_VOCR_TIERS ? job_cfg.vocr_tier
                                                                                 : (int)VJO_VOCR_TIER_DEFAULT;
    unsigned all = (1u << top) - 1u;
    return vocr_unusable_generation == job_generation ? all & ~vocr_unusable : all;
}

/* The largest tier in a request's tiers, or 0. */
static unsigned vocr_top_tier(unsigned tiers)
{
    unsigned top = 0;
    for (unsigned t = 1; t <= VJO_VOCR_TIERS; t++)
        if (tiers & (1u << (t - 1u))) top = t;
    return top;
}

/* The capture of the job in flight: the kernel publishes it (VjoState.done_seq
 * moves on, with capture_scene and region_seq) once the game has read its
 * first pass over the region. A try after the screen changed under the job
 * voids it (region_seq 0 while the rows settle and are read again) and
 * publishes its own, so the job's capture is always its latest one. Polled
 * from the exchange's cancel callback. */
static uint32_t vocr_done_before;
static int vocr_capture_fixed; /* full-frame and probe jobs: theirs is not the region's */

static void vocr_note_capture(void)
{
    VjoState st;
    if (vocr_capture_fixed || job_selftest) return;
    st.size = sizeof(st);
    if (vjoGetState(&st) < 0 || st.done_seq == vocr_done_before) return;
    if (!st.region_seq) {
        job_captured = 0;
        return;
    }
    job_capture_scene = st.capture_scene;
    job_capture_seq = st.region_seq;
    __sync_synchronize(); /* the scene is visible before job_captured */
    job_captured = 1;
}

static int vocr_poll_cancelled(void *ud)
{
    vocr_note_capture();
    return vocr_cancelled(ud);
}

#if !VJO_DEBUG
/* What the normal build has logged of the game's activation in front (a start or
 * a resume: game_generation moves on at each, invalidate_game_work): its first job
 * that read its screen, its first job that found a line in the region (often a
 * later one: a title screen's job finds none), and the model sizes that did not
 * fit, as the bits of the refused tiers and the tier the job went on to; a job
 * logs them again only when they change (vocr_run_one, log_tier_checks). */
enum { SHORT_FIRST_JOB = 1u, SHORT_FIRST_TEXT = 2u, SHORT_TIERS = 4u };
static uint32_t short_generation; /* the activation the bits below are for */
static unsigned short_logged;     /* SHORT_* */
static uint32_t short_tiers;      /* the last tier misses logged */

static void short_activation(uint32_t generation)
{
    if (short_generation != generation) {
        short_generation = generation;
        short_logged = 0;
        short_tiers = 0;
    }
}
#endif

#if VJO_DEBUG
/* The game worker's step log: one log.txt line per step (`step/rc/bytes/free`). */
static void log_vocr_steps(const char *who, const VjoGameOcrResult *r)
{
    char line[224];
    for (uint32_t i = 0; i < r->n_steps && i < VJO_GAME_OCR_STEPS; i++)
        if (vjo_gstep_format(line, sizeof(line), who, &r->step[i]) > 0)
            vjo_log("%s", line);
}
#else
/* The normal build's one line on the 444 MHz boost (ocr_boost_clock), from the
 * job's clock step: the ARM clock before, while and after it recognized, or what
 * stopped the boost. Logged for the first job of each game and whenever it
 * changes, so a session has a line or two (docs/release-variants.md). */
static void log_vocr_clock(const VjoGameOcrResult *r)
{
    static uint32_t logged_generation;
    static int32_t logged_rc;
    static uint32_t logged_mhz[3];
    for (uint32_t i = 0; i < r->n_steps && i < VJO_GAME_OCR_STEPS; i++) {
        const VjoGameOcrStep *s = &r->step[i];
        if (s->id != VJO_GSTEP_CLOCK)
            continue;
        if (logged_generation == job_generation && logged_rc == s->rc && logged_mhz[0] == s->arg &&
            logged_mhz[1] == s->bytes && logged_mhz[2] == s->sum)
            return;
        logged_generation = job_generation;
        logged_rc = s->rc;
        logged_mhz[0] = s->arg;
        logged_mhz[1] = s->bytes;
        logged_mhz[2] = s->sum;
        if (s->rc == 1 && !s->arg)
            vjo_log("clock: unreadable, left as it is while reading");
        else if (s->rc == 1)
            vjo_log("clock: %u MHz, left as it is while reading (444 or more)", (unsigned)s->arg);
        else if (s->rc < 0)
            vjo_log("clock: 444 MHz refused 0x%08X, %u MHz while reading", (unsigned)s->rc, (unsigned)s->bytes);
        else
            vjo_log("clock: %u -> %u MHz while reading, %u after", (unsigned)s->arg, (unsigned)s->bytes,
                    (unsigned)s->sum);
        return;
    }
}
#endif

/* The game worker's speed word from the config (API 22): the helpers (0-2, bits
 * 0-1), the clock (bit 2: VJO_GAME_OCR_S_BOOST). */
static uint32_t config_speed(void)
{
    return (uint32_t)job_cfg.ocr_helpers | (uint32_t)job_cfg.ocr_boost_clock << 2;
}

/* The memory check's tiers (API 21): a log line for each tier that did not fit
 * this game, with what it needed (its workspace and weights, and the reserve it
 * keeps) against what was free (free USER and the worker's own blocks it would
 * release first), and the tier the job went on to. Only the log has it: the
 * overlay shows the text as usual (owner 2026-10-15 04:54Z). */
static void log_tier_checks(const VjoGameOcrResult *r)
{
    const VjoGameOcrStep *check[VJO_VOCR_TIERS];
    unsigned n = 0;
    for (uint32_t i = 0; i < r->n_steps && i < VJO_GAME_OCR_STEPS && n < VJO_VOCR_TIERS; i++)
        if (r->step[i].id == VJO_GSTEP_CHECK && VJO_GCHECK_TIER(r->step[i].kind)) check[n++] = &r->step[i];
#if !VJO_DEBUG
    {   /* the normal build: once per activation, and again only when what did not fit, or the
         * tier the job went on to, changes: the refused tiers' bits | the next tier << 8 */
        uint32_t sig = 0;
        for (unsigned k = 0; k < n; k++)
            if (check[k]->rc == 1)
                sig |= 1u << VJO_GCHECK_TIER(check[k]->kind);
            else if (!(sig >> 8))
                sig |= (uint32_t)VJO_GCHECK_TIER(check[k]->kind) << 8;
        if (!(sig & 0xFFu))
            return; /* every size it tried fitted: no line */
        short_activation(job_generation);
        if ((short_logged & SHORT_TIERS) && short_tiers == sig)
            return;
        short_logged |= SHORT_TIERS;
        short_tiers = sig;
    }
#endif
    for (unsigned i = 0; i < n; i++) {
        unsigned t = VJO_GCHECK_TIER(check[i]->kind), next = i + 1u < n ? VJO_GCHECK_TIER(check[i + 1u]->kind) : 0u;
        /* refused: its last try was without the LM and the cascade, so its own reserve */
        uint32_t reserve = t >= VJO_VOCR_TIER_LARGE ? VJO_GAME_RESERVE_LARGE
                         : t == VJO_VOCR_TIER_NORMAL ? VJO_GAME_RESERVE_NORMAL : VJO_GAME_RESERVE_SMALL;
        int64_t avail = (int64_t)r->free_user + check[i]->sum;
        if (check[i]->rc != 1) continue;
        vjo_log("tier %s did not fit in %s (need %u, free %d); %s %s", vjo_vocr_tier_id((int)t),
                job_title[0] ? job_title : "this game", (unsigned)(check[i]->bytes + reserve),
                (int)(avail > INT32_MAX ? INT32_MAX : avail),
                !next ? "no smaller tier:" : check[i + 1u]->rc == 1 ? "trying" : "using",
                next ? vjo_vocr_tier_id((int)next) : "not enough memory");
    }
}

/* One job: request (flags: VJO_GAME_OCR_F_*; tiers: the model tiers it may run,
 * bit t - 1 for tier t), exchange, logs. *ran: the tier the worker chose (or
 * whose check refused it last). stage: the self-test stage the job is (its speed
 * word, vjo_selftest_stage_speed), or -1. */
static int vocr_run_one(VjoGameOcrResult *result, uint32_t flags, unsigned tiers, unsigned *ran, int stage)
{
    VjoGameOcrRequest request = {.size=sizeof(request), .flags=flags, .tiers=tiers};
    VjoState st0;
    unsigned top = vocr_top_tier(tiers);
    *ran = top;
    if (!top) return VJO_E_OCR_MODEL;
    /* At most the field's 127 characters (zeroed above, so terminated): the kernel accepts no other
     * directory than VJO_VOCR_MODEL_ROOT, a longer one included. */
    sceClibMemcpy(request.model_dir, job_cfg.ocr_model_dir,
                  sceClibStrnlen(job_cfg.ocr_model_dir, sizeof(request.model_dir) - 1));
    /* The largest tier's model names the job (the kernel's log); the worker opens each by its tier. */
    sceClibSnprintf(request.model, sizeof(request.model), "%s", vjo_vocr_tier_model((int)top));
    st0.size = sizeof(st0);
    if (vjoGetState(&st0) < 0) st0.done_seq = st0.stable = 0;
    vocr_done_before = st0.done_seq;
    vocr_capture_fixed = (flags & (VJO_GAME_OCR_F_FULL | VJO_GAME_OCR_F_PROBE)) != 0;
    /* The job's policy (API 20). The config keeps both waits in range, so the
     * counts fit their 8 bits (10 and 50 ms steps, rounded; a wait of 1-4 ms is
     * one step, not none); a screen not settled now is waited for first. */
    if (job_cfg.ocr_skip_blank_lines) request.flags |= VJO_GAME_OCR_F_BLANK;
    request.flags |= (uint32_t)(job_cfg.ocr_lm & VJO_GAME_OCR_LM_MASK) << VJO_GAME_OCR_LM_SHIFT |
                     (uint32_t)(job_cfg.ocr_refine_lines & VJO_GAME_OCR_REFINE_MASK) << VJO_GAME_OCR_REFINE_SHIFT;
    if (job_cfg.ocr_cascade_ms > 0) { /* in 250 ms steps, at least one */
        uint32_t c = (uint32_t)(job_cfg.ocr_cascade_ms + 125) / 250u;
        request.flags |= (c ? c > VJO_GAME_OCR_CASCADE_MASK ? VJO_GAME_OCR_CASCADE_MASK : c : 1u)
                         << VJO_GAME_OCR_CASCADE_SHIFT;
    }
    if (job_cfg.ocr_settle_ms > 0) {
        uint32_t settle = (uint32_t)(job_cfg.ocr_settle_ms + 5) / 10u;
        /* A job the open overlay waits for (not Auto's, a refresh or the self-test's)
         * waits at most ocr_settle_press_ms: test.2's waited up to 1.5 s (docs/first-ocr.md). */
        int wait_max = job_cfg.ocr_settle_max_ms;
        if (!job_prepares && !job_refresh && job_cfg.ocr_settle_press_ms > 0 && job_cfg.ocr_settle_press_ms < wait_max)
            wait_max = job_cfg.ocr_settle_press_ms;
        request.flags |= (settle ? settle : 1u) << VJO_GAME_OCR_SETTLE_SHIFT |
                         (uint32_t)(wait_max + 25) / 50u << VJO_GAME_OCR_SETTLE_MAX_SHIFT;
        if (!st0.stable) request.flags |= VJO_GAME_OCR_F_SETTLE_FIRST;
    }
    /* API 22 (ocr-speed): helper threads for parallel lines, the ARM clock. */
    request.speed = stage >= 0 ? vjo_selftest_stage_speed((unsigned)stage, config_speed()) : config_speed();
    int64_t t0 = now_us();
    /* the slot: a screenshot's exchange may hold it (a cancellation ends the wait) */
    int locked = !game_job_lock(vocr_poll_cancelled, INT64_MAX / 2);
    int rc = locked ? vjo_game_ocr_exchange(&vjo_game_client, &request, result, vocr_poll_cancelled, NULL, 60000000)
                    : VJO_E_CANCELLED;
    if (locked)
        game_job_unlock();
#if VJO_DEBUG
    job_game_us += now_us() - t0; /* the done line's (net_job) */
#endif
    vocr_note_capture();
#if VJO_DEBUG
    /* weights: read = from the card and SHA-256 checked, resident = kept from
     * an earlier job (file unchanged); resident = bytes the game keeps after it. */
    static const char *const weights[] = {"none", "read", "resident"};
    static const char *const sources[] = {"none", "hook", "kernel strip", "user"};
    static const char *const memories[] = {"none", "USER", "CDRAM", "PHYCONT", "kernel"};
#endif
    /* The tier that ran (or whose check refused the job last): the largest one less
     * the tiers the worker stepped down; its model's name is a constant, so it
     * outlives this call. */
    *ran = result->fallback < top ? top - result->fallback : VJO_VOCR_TIER_SMALL;
#if VJO_DEBUG
    /* The result's lines (about 21: the job, its capture, memory, speed and lines,
     * the tiers that did not fit, and every game-worker step), formatted from the
     * result alone, go out with one append per file instead of one each: a file
     * append costs 12-46 ms on the console (#81's self-test timestamps). */
    VjoLogBatch batch;
    char batch_buf[4096];
    vjo_log_batch_begin(&batch, batch_buf, sizeof(batch_buf));
    vjo_log("game vocr rc=%d %s%s: %ux%u at %u,%u of %ux%u fmt 0x%08X, lines=%u USER workspace=%u weights=%s resident=%u, "
            "find=%u ms load=%u ms recognize=%u ms frames=%u io=0x%08X stat=0x%08X cleanup=%d",
            rc, vjo_vocr_tier_model((int)*ran),
            !result->fallback ? "" : fallback_check_refused(result) ? " (fallback, refused too)" : " (fallback)",
            result->width, result->height, result->region_x, result->region_y, result->fb_width, result->fb_height,
            (unsigned)result->fb_format, result->lines, result->workspace_bytes,
            result->weights < 3 ? weights[result->weights] : "?", result->resident_bytes,
            result->find_us/1000, result->load_us/1000, result->recognize_us/1000, result->frames,
            (unsigned)result->io_error, (unsigned)result->stat_error, result->cleanup_status);
    vjo_log("game vocr capture: %s into %s, %u B block, %u-row strips, %u strips in %u ms, %u rows changed, sum 0x%08X",
            result->source < 4 ? sources[result->source] : "?",
            result->capture_memory < 5 ? memories[result->capture_memory] : "?", (unsigned)result->capture_bytes,
            (unsigned)result->strip_rows, (unsigned)result->captures, (unsigned)(result->capture_us / 1000),
            (unsigned)result->tears, (unsigned)result->region_sum);
    log_tier_checks(result);
    log_vocr_measurements(result);
    log_vocr_steps(flags & VJO_GAME_OCR_F_PROBE ? "probe" : "job", result);
    vjo_log_batch_end(&batch);
#else
    /* The normal build: a job's line (its result, the size that ran, the lines found, the
     * time in the game) when it failed, and for the first job of each activation of the game
     * (a start or a resume) that read its screen and its first that found a line (the first
     * is often a title screen's, with none); then the sizes that did not fit and the clock,
     * each once per activation and when it changes, and a refused memory check
     * (docs/release-variants.md). A cancelled job, or one whose screen changed under it, is
     * retried quietly as the overlay does: no line. */
    {
        int failed = rc != VJO_OK && rc != VJO_E_CANCELLED && rc != VJO_E_SCREEN_CHANGED;
        unsigned want = rc != VJO_OK ? 0u : SHORT_FIRST_JOB | (result->lines ? SHORT_FIRST_TEXT : 0u);
        short_activation(job_generation);
        if (failed || (want & ~short_logged)) {
            short_logged |= want;
            vjo_log("ocr rc=%d %s: %u lines, %u ms", rc, vjo_vocr_tier_model((int)*ran), (unsigned)result->lines,
                    (unsigned)((now_us() - t0) / 1000));
        }
    }
    log_tier_checks(result);
    log_vocr_measurements(result);
    log_vocr_clock(result);
#endif
    return rc;
}

#if VJO_DEBUG
/* The recognized text, a log line per line (60 characters each at most): what
 * the recognizer read (the game worker keeps it after the normalised text) and,
 * where it differs, what is shown and looked up. */
static void log_text_lines(const char *text, size_t cap)
{
    char line[184], batch_buf[1536];
    VjoLogBatch batch;
    const char *raw = vjo_text_raw(text, cap), *shown = text;
    vjo_log_batch_begin(&batch, batch_buf, sizeof(batch_buf)); /* one append for the text's lines */
    for (unsigned i = 0; *raw || *shown; i++) {
        size_t n = 0, m = 0, same;
        while (raw[n] && raw[n] != '\n') n++;
        while (shown[m] && shown[m] != '\n') m++;
        for (same = 0; same < n && same < m && raw[same] == shown[same]; same++) {
        }
        log_text(line, sizeof(line), raw, n, 60);
        vjo_log("game vocr text %u: %s", i, line);
        if (same != n || n != m) {
            log_text(line, sizeof(line), shown, m, 60);
            vjo_log("game vocr text %u shown: %s", i, line);
        }
        raw += n + (raw[n] == '\n');
        shown += m + (shown[m] == '\n');
    }
    vjo_log_batch_end(&batch);
}

/* The self-test (docs/self-test.md), its job's only work (selftest_job_due):
 * every stage on the screen the last job read text from, in turn, each with
 * its record, a failed one included. They share the job's one result (the
 * Shell's pages may only shrink), which nothing shows. The first stage also
 * decides whether this screen is one to test: #81's ran on the game's opening
 * notice, whose five lines lay outside the region, so every region stage found
 * nothing. With no line in the region (or a screen that changed under every
 * read) the stages wait for a later screen (shell/selftest.c bounds the wait). */
static int run_selftest_stages(VjoGameOcrResult *result, unsigned tiers, unsigned *ran)
{
    unsigned n = vjo_selftest_stages(), top = vocr_top_tier(tiers), low = 0;
    int rc = VJO_E_SOURCE;
    while (low < VJO_VOCR_TIERS && !(tiers & (1u << low))) low++; /* the smallest tier: the fallback stage's */
    for (unsigned i = 0; i < n && !vocr_cancelled(NULL); i++) {
        int64_t t = now_us();
        vjo_selftest_stage_begin(i, vjo_vocr_tier_model((int)top),
                                 low + 1u < top ? vjo_vocr_tier_model((int)low + 1) : "",
                                 vjo_selftest_stage_speed(i, config_speed()));
        sceClibMemset(result, 0, sizeof(*result));
        rc = vocr_run_one(result, vjo_selftest_stage_flags(i), tiers, ran, (int)i);
        if (vjo_selftest_stage_wait(i, rc, result))
            return rc;
        vjo_selftest_stage_end(i, rc, result, (uint32_t)((now_us() - t) / 1000), vjo_vocr_tier_model((int)*ran));
    }
    if (vocr_cancelled(NULL)) return VJO_E_CANCELLED; /* the stages run again on the next settled screen */
    vjo_selftest_done();
    return rc;
}
#endif

static int run_vocr(VjoOverlayData *out)
{
    static VjoGameOcrResult result;
    unsigned tiers = vocr_tiers(), ran = vocr_top_tier(tiers);
    sceClibMemset(&result, 0, sizeof(result));
#if VJO_DEBUG
    if (job_selftest) /* its records are its result: nothing is shown */
        return out->err.rc = run_selftest_stages(&result, tiers, &ran);
#endif
    out->err.rc = vocr_run_one(&result, 0, tiers, &ran, -1);
    /* A tier's model failed to open or verify: keep OCR working with the next
     * smaller tier for this job. Only a file that is absent, the wrong length or
     * fails its hash (io 0: rejected after reading) leaves the tier out for this
     * game; a timeout or unavailable storage does not. Log only. */
    while (out->err.rc == VJO_E_OCR_MODEL && ran >= VJO_VOCR_TIER_SMALL && ran <= VJO_VOCR_TIERS &&
           !vocr_cancelled(NULL)) {
        int file_bad = !result.io_error || result.io_error == VJO_ERR_FORMAT ||
                       (uint32_t)result.io_error == 0x80010002u; /* ENOENT */
        unsigned failed = ran, below = tiers & ((1u << (failed - 1u)) - 1u);
        vjo_log("game vocr: %s is unusable here (io=0x%08X), using %s%s", vjo_vocr_tier_model((int)failed),
                (unsigned)result.io_error, below ? vjo_vocr_tier_model((int)vocr_top_tier(below)) : "none",
                file_bad ? " for this game" : " for this job");
        if (file_bad) {
            if (vocr_unusable_generation != job_generation) vocr_unusable = 0;
            vocr_unusable |= 1u << (failed - 1u);
            vocr_unusable_generation = job_generation;
        }
        if (!below) break;
        tiers = below;
        sceClibMemset(&result, 0, sizeof(result));
        out->err.rc = vocr_run_one(&result, 0, tiers, &ran, -1);
    }
    /* The self-test's job runs on the screen of the last job that read text in the region
     * (a failed job's text is empty: the worker and the exchange clear it). */
    text_generation = result.text[0] ? job_generation : 0;
    text_scene = job_capture_scene;
    text_seq = job_capture_seq;
    if (out->err.rc != VJO_OK) {
        if (out->err.rc == VJO_E_OCR_MODEL) {
            /* Vita errno ENODEV and EACCES are distinct from absent/corrupt weights. */
            if ((uint32_t)result.io_error == 0x80010013u)
                out->err.detail = "The local OCR model storage is unavailable to this game. See log.txt for details.";
            else if ((uint32_t)result.io_error == 0x8001000Du)
                out->err.detail = "Access to the local OCR model was denied. See log.txt for details.";
        }
        if (out->err.rc == VJO_E_OOM)
            out->err.detail = game_oom_detail(&result);
        if (out->err.rc == VJO_E_SOURCE)
            out->err.detail = "The game's screen could not be read (see log.txt).";
        return out->err.rc;
    }
    /* The worker returns the text normalised (core/textnorm.h: one form of each
     * mark, ー or 一 by context); the recognizer's own text goes to the log
     * (the debug build's: the normal build logs no recognized text). */
#if VJO_DEBUG
    log_text_lines(result.text, sizeof(result.text));
#endif
    /* The model on this text's Anki cards: the tier's that ran. A constant string. */
    const char *read_by = vjo_vocr_tier_model((int)ran);
    if (!read_by) read_by = vjo_vocr_tier_model(VJO_VOCR_TIER_DEFAULT);
    if (job_refresh) {
        /* The open overlay shows local_text, its lookup and Anki card: this
         * job changes none of them. The control thread compares its text
         * with the shown one once it is done (on_refresh_done), and gives
         * the cards this model only if the text replaces the shown one. */
        out->ocr_text = result.text;
        refresh_model = read_by;
        return VJO_OK;
    }
    vjo_anki_set_ocr_model(read_by);
    VjoArena text_arena;
    vjo_arena_init(&text_arena, local_text, sizeof(local_text));
    out->ocr_text = result.text;
    out->filtered = vjo_filter_lines(&text_arena, result.text, job_cfg.non_japanese_filter);
    /* Filtering only removes bytes, so one complete worker text fits. */
    out->sentence = out->filtered ? vjo_java_trim((char *)out->filtered) : NULL;
    if (!out->sentence) {
        out->failed_stage = VJO_STAGE_OCR;
        out->err.detail = OOM_TEXT_BUFFER;
        return out->err.rc = VJO_E_OOM;
    }
    /* Cursor byte0 is the trimmed sentence, not discarded whitespace. */
    size_t len = sceClibStrnlen(out->sentence, sizeof(local_text) - 1);
    sceClibMemmove(local_text, out->sentence, len + 1);
    out->filtered = out->sentence = local_text;
    out->list.header = out->sentence ? out->sentence : "";
    sceKernelLockMutex(cmd_lock, 1, NULL);
    job_text_generation = advance_generation(&local_text_generation);
    job_cursor_seq = advance_generation(&local_cursor_seq);
    local_page = job_page = 0;
    local_cursor_byte = job_cursor_byte = job_cfg.japanese_only_selection
        ? (uint32_t)vjo_utf8_japanese_cursor_step(local_text, len, 0, 0) : 0;
    local_text_game_generation = job_generation;
    local_text_scene = job_capture_scene;
    local_text_capture_seq = job_capture_seq;
    __sync_synchronize();
    local_text_valid = 1;
    sceKernelUnlockMutex(cmd_lock, 1);
    if (!job_lookup || !len || job_cursor_byte >= len || mem_alloc() == 0)
        return VJO_OK;
    out->failed_stage = VJO_STAGE_DICT;
    out->err.dict = VJO_DICT_LOCAL;
    out->err.detail = "Text was recognized, but Shell UI memory is too low for dictionary results. Restart the Vita and try again.";
    return out->err.rc = VJO_E_OOM;
}
#else
static int run_vocr(VjoOverlayData *out)
{
    out->err.detail = "This build has no vita-vn-ocr backend. Install a build made with VJO_WITH_VOCR.";
    return out->err.rc = VJO_E_OCR_UNAVAILABLE;
}
static int shot_waiting(void) { return 0; } /* no game worker: no screenshot */
#endif

static int run_job(VjoArena *a, VjoOverlayData *out)
{
    sceClibMemset(out, 0, sizeof(*out));
    out->list.header = "";
    /* A refresh and the self-test's job keep the arena: the overlay's entry lives in it. */
    if (!job_refresh && !job_selftest)
        mem_free();
    vjo_arena_reset(a);
    /* On the console only: no image or connection leaves it. */
    if (run_vocr(out) != VJO_OK) {
        if (!out->failed_stage)
            out->failed_stage = VJO_STAGE_OCR;
        /* #138: the kernel loads the worker when a game that is ON starts
         * (kernel/loader.c), so the cure is the game's next start: turned ON
         * while it ran, L held as it started, the worker's load failed
         * (kernel.txt says which). */
        if (out->err.rc == VJO_E_OCR_UNAVAILABLE)
            out->err.detail = "OCR is not running in this game. Close the game and start it again.";
        return out->err.rc;
    }
    if (job_selftest)
        return VJO_OK;
    __sync_synchronize(); /* the sentence is visible before job_text_ready */
    job_text_ready = 1;
    if (!job_lookup || job_cursor_byte >= sceClibStrnlen(local_text, sizeof(local_text))) return VJO_OK;
    int64_t t0 = now_us();
    vjo_dict_reads = vjo_dict_read_bytes = 0;
    int rc = vjo_overlay_lookup_at(a, &plat, &job_cfg, job_cursor_byte, out);
    note_pages(rc, out);
    log_lookup(job_prepares ? "first word" : "on open", out, job_cursor_byte, rc, t0);
    return rc;
}

/* Records which text/limit the locators in this arena belong to. */
static void note_pages(int rc, const VjoOverlayData *out)
{
    pages_generation = !rc && out->pages ? job_text_generation : 0;
    pages_limit = job_cfg.local_dictionary_results;
}

/* No capture, model, inference or sentence tokenization for cursor moves.
 * Another dictionary page at the same cursor reads only that page's entry
 * into the same arena, after the locators (no probing, no new allocation). */
static int run_cursor_job(VjoArena *a, VjoOverlayData *out)
{
    const VjoLocalPages *pages = out->pages;
    int64_t t0 = now_us();
    int rc;
    vjo_dict_reads = vjo_dict_read_bytes = 0;
    if (pages && has_result_memory() && pages_generation == job_text_generation &&
        pages_limit == job_cfg.local_dictionary_results &&
        pages->cursor_byte == job_cursor_byte && job_page < pages->count) {
        /* post_local_lookup already published the static header, so the
         * shown entry can be released without the view lock. */
        rc = vjo_overlay_lookup_page(a, &plat, &job_cfg, job_page, out);
        if (rc) pages_generation = 0; /* e.g. a replaced file: locate again next time */
        log_lookup("page", out, job_cursor_byte, rc, t0);
        return rc;
    }
    sceClibMemset(out, 0, sizeof(*out));
    out->ocr_text = out->filtered = out->sentence = local_text;
    out->list.header = local_text;
    if (job_cursor_byte >= sceClibStrnlen(local_text, sizeof(local_text))) return VJO_OK;
    if (mem_alloc() < 0) {
        out->failed_stage = VJO_STAGE_DICT;
        out->err.dict = VJO_DICT_LOCAL;
        out->err.detail = "Text was recognized, but Shell UI memory is too low for dictionary results. Restart the Vita and try again.";
        return out->err.rc = VJO_E_OOM;
    }
    vjo_view_lock(); /* the published list is the static header, never old entries */
    vjo_arena_reset(a);
    vjo_view_unlock();
    rc = vjo_overlay_lookup_at(a, &plat, &job_cfg, job_cursor_byte, out);
    note_pages(rc, out);
    /* Locators were stale (another limit): show the requested page if it exists. */
    if (!rc && job_page > 0 && out->pages && job_page < out->pages->count)
        rc = vjo_overlay_lookup_page(a, &plat, &job_cfg, job_page, out);
    log_lookup("cursor", out, job_cursor_byte, rc, t0);
    return rc;
}

/* One job on the network thread, then its done line: with the time in exchanges with
 * the game worker and in the log's file appends meanwhile (every thread's); the rest is
 * the Shell's own and the lookup (docs/first-ocr.md; tools/analyse_timeline.py reads it). */
static void net_job(void)
{
    int idx = job_idx;
#if VJO_DEBUG
    int64_t t0 = now_us();
    uint32_t appends0, appends1, log_us0 = vjo_log_io(&appends0), log_us1;
    job_game_us = 0;
#endif
    job_rc = job_cursor_only ? run_cursor_job(&results[idx], &cache_data[idx])
                             : run_job(&results[idx], &cache_data[idx]);
#if VJO_DEBUG
    log_us1 = vjo_log_io(&appends1);
    vjo_log("job %d done rc=%d in %d ms, arena peak %u, dictionary page %d/%d, game %u ms, log %u ms in %u "
            "appends", idx, job_rc, (int)((now_us() - t0) / 1000), (unsigned)results[idx].peak,
            cache_data[idx].page + 1, vjo_overlay_page_count(&cache_data[idx]),
            (unsigned)(job_game_us / 1000), (unsigned)((log_us1 - log_us0) / 1000),
            (unsigned)(appends1 - appends0));
#endif
    __sync_synchronize(); /* results are visible before job_done */
    job_done = 1;
}

/* VjoNet's stack use, measured on the console: at its start the thread fills its
 * stack below its own frame with a pattern (the stack is its own memory: nothing
 * lives below the stack pointer), and the deepest byte no longer holding it is
 * the most the thread has ever used. [SDK: Kernel-Reference, SceKernelThreadInfo:
 * pStack is the stack's bottom, stackSize its size; the TLS is outside the stack,
 * sceKernelCreateThread.] Logged at each game exit when it grew. */
#define STACK_PAINT 0xA5u
#define STACK_PAINT_GAP 512u /* left unpainted below the painting frame */
/* The stack's lowest bytes stay as the kernel left them: the PSP's thread manager, whose API the Vita
 * keeps, fills a new stack and writes the thread's UID in its lowest word, and the Vita's kernel
 * detects stack overflows (a core-dump cause; sceKernelCheckThreadStack). How it detects them is
 * unknown, so nothing here writes where a marker could be (audit-overlay-shell-slim-2c B2, -2d N2). */
#define STACK_PAINT_FLOOR 64u
static uint8_t *net_stack_lo;                    /* published last: the paint is done */
static uint8_t *net_stack_painted, *net_stack_top; /* [lo, painted): the pattern; top: the stack's top,
                                                    * pStack + its size (lo is pStack + the floor) */
static uint32_t net_stack_logged;                /* the high-water mark last logged */

/* Paints n bytes from lo (the thread's own stack, below its frame), then publishes
 * the bounds: the pattern is visible to a thread that sees net_stack_lo. */
static void stack_paint(uint8_t *lo, size_t n, uint8_t *top)
{
    volatile uint8_t *p = lo;
    for (size_t i = 0; i < n; i++) /* a count, not a pointer bound */
        p[i] = STACK_PAINT;
    net_stack_painted = lo + n;
    net_stack_top = top;
    __atomic_store_n(&net_stack_lo, lo, __ATOMIC_RELEASE);
}

/* The most of the stack ever used: from its top down to the lowest byte that no
 * longer holds the pattern (0: nothing painted). A live byte that happens to be
 * the pattern can only make it read lower than the truth by that run of bytes. */
static uint32_t stack_high_water(void)
{
    uint8_t *lo = __atomic_load_n(&net_stack_lo, __ATOMIC_ACQUIRE);
    if (!lo)
        return 0;
    size_t n = (size_t)(net_stack_painted - lo), i = 0;
    const volatile uint8_t *p = lo;
    while (i < n && p[i] == STACK_PAINT)
        i++;
    return (uint32_t)(net_stack_top - (lo + i));
}

/* Not inlined: its thread info stays out of net_main's frame, under every job. */
static __attribute__((noinline)) void net_stack_paint(void)
{
    SceKernelThreadInfo info;
    uint8_t here;
    sceClibMemset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0 || !info.stack) {
        vjo_log("VjoNet stack: not painted (no thread info)");
        return;
    }
    if (info.stackSize != (int)VJO_STACK_SHELL_NET) { /* not the stack this thread was created with */
        vjo_log("VjoNet stack: not painted (the kernel reports %d B, not %u)", info.stackSize,
                (unsigned)VJO_STACK_SHELL_NET);
        return;
    }
    /* As addresses: painted only when this frame is inside the reported stack, with room below it. */
    uintptr_t lo = (uintptr_t)info.stack, top = lo + (uintptr_t)info.stackSize, frame = (uintptr_t)&here;
    uintptr_t low = lo + STACK_PAINT_FLOOR; /* the paint's first byte */
    if (frame <= low + STACK_PAINT_GAP || frame >= top) {
        vjo_log("VjoNet stack: not painted (frame 0x%08X outside 0x%08X + %d)", (unsigned)frame, (unsigned)lo,
                info.stackSize);
        return;
    }
    stack_paint((uint8_t *)low, (size_t)(frame - STACK_PAINT_GAP - low), (uint8_t *)top);
}

/* At a game exit: the most VjoNet has used, when that grew since the last line. */
static void net_stack_check(void)
{
    uint32_t used = stack_high_water();
    if (!used || used <= net_stack_logged)
        return;
    net_stack_logged = used;
    /* "at least" once anything has used the stack below the paint (any job): a live byte that happens
     * to hold the pattern can only make the scan read low. Until then the figure is the paint's end,
     * which counts the 512 B gap below the painting frame as used (audit-overlay-shell-slim-4 N1). */
    vjo_log("VjoNet stack: at least %u of %u B used", (unsigned)used, (unsigned)VJO_STACK_SHELL_NET);
}

static int net_main(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    net_stack_paint();
    while (running) {
        unsigned int bits = 0;
        sceKernelWaitEventFlag(net_evf, NET_EV_JOB | NET_EV_QUIT,
                               SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &bits, NULL);
        if (bits & NET_EV_QUIT)
            break;
        if (bits & NET_EV_JOB)
            net_job();
    }
    return 0;
}

/* The network thread is at work (its memory must stay). */
static int net_busy(void)
{
    __sync_synchronize();
    return job_running;
}

/* lookup = 0: OCR only (the subtitles' sentence). refresh: the open
 * overlay's quiet refresh (background_open), which keeps what the overlay shows:
 * no retirement, no "Recognizing…", the result arena stays, and its result
 * goes to cache_data[1] (local lookups use [0] and one arena). */
static void start_job_kind(const char *why, int lookup, int refresh)
{
    if (job_running)
        return;
    if (!refresh) {
        /* Retire the previous lookup before OCR, then recreate result arenas
         * after all capture/model/inference allocations have been freed. */
        retire_local_text(0); /* atomic request exclusion and view detachment */
        if (ov != OV_CLOSED)
            view_publish(1, NULL, "Recognizing…", 0);
        mem_free();
    }
    vjo_dlog("job start (%s)%s", why, refresh ? ", refresh" : lookup ? "" : ", OCR only");
    job_refresh = refresh;
    job_lookup = lookup;
    job_prepares = ov == OV_CLOSED;
    job_cursor_only = 0;
    job_text_generation = 0;
    job_idx = refresh ? 1 : 0;
    job_cfg = cfg; /* the control thread may reload cfg while the job runs */
    job_generation = game_generation;
    sceClibMemcpy(job_title, title_id, sizeof(job_title));
    job_title[sizeof(job_title) - 1] = '\0';
    job_captured = 0;
    job_capture_scene = 0;
    job_capture_seq = 0;
    job_cancelled = 0;
    if (view_job_text) { /* its arena may be the new job's (never with a refresh: the overlay shows a lookup) */
        view_job_text = 0;
        view_publish(1, NULL, "Recognizing…", 0);
    }
    job_done = 0;
    job_text_ready = 0;
    job_running = 1;
    job_started_us = now_us();
    __sync_synchronize();
    sceKernelSetEventFlag(net_evf, NET_EV_JOB);
    if (subtitles && !refresh)
        vjo_strip_set_busy(1);
}

static void start_job(const char *why, int lookup)
{
    start_job_kind(why, lookup, 0);
}

/* A lookup in the text the overlay shows (the cursor's word, or another
 * dictionary's page), whatever the screen shows now. #81 request 4: a changed
 * screen never retires the shown text or says so (the region's scene changes
 * while the dialogue does not, e.g. as a "next" icon's mask comes and goes);
 * a refresh replaces the text quietly once it reads another. Only the text's
 * generation decides. A refresh in progress yields to the lookup: it is
 * cancelled and asked again once the lookups pause. */
static void start_cursor_job(void)
{
    if (job_running) {
        cursor_waiting = 1;
        job_cancelled |= job_refresh;
        return;
    }
    if (ov == OV_CLOSED || !title_id[0] || !local_text_valid ||
        local_text_game_generation != game_generation) {
        cursor_waiting = 0;
        return;
    }
    sceKernelLockMutex(cmd_lock, 1, NULL);
    job_cursor_only = 1;
    job_refresh = 0;
    cache_ok = 0; /* this job will overwrite the single result arena */
    job_lookup = 1;
    job_idx = 0;
    job_cfg = cfg;
    job_generation = game_generation;
    job_text_generation = local_text_generation;
    job_cursor_byte = local_cursor_byte;
    job_cursor_seq = local_cursor_seq;
    job_page = local_page;
    cursor_waiting = 0;
    job_cancelled = job_done = job_text_ready = 0;
    job_captured = 1;
    job_capture_scene = local_text_scene;
    job_capture_seq = local_text_capture_seq;
    local_header = (VjoEntryList){.header = local_text};
    view_publish(1, &local_header, "Looking up words…", 0);
    job_running = 1;
    job_started_us = lookup_us = now_us();
    __sync_synchronize();
    sceKernelSetEventFlag(net_evf, NET_EV_JOB);
    sceKernelUnlockMutex(cmd_lock, 1);
}

/* ---------------- control thread ---------------- */

static int same_pipeline(const VjoConfig *a, const VjoConfig *b)
{
    if (a->non_japanese_filter != b->non_japanese_filter ||
        a->japanese_only_selection != b->japanese_only_selection)
        return 0;
    if (sceClibStrcmp(a->ocr_model_dir, b->ocr_model_dir) || a->ocr_skip_blank_lines != b->ocr_skip_blank_lines ||
        a->ocr_lm != b->ocr_lm || a->ocr_cascade_ms != b->ocr_cascade_ms ||
        a->ocr_refine_lines != b->ocr_refine_lines || a->vocr_tier != b->vocr_tier)
        return 0;
    return !sceClibStrcmp(a->local_dictionary_dir, b->local_dictionary_dir) &&
           !sceClibStrcmp(a->local_dictionaries, b->local_dictionaries);
}

static void apply_config(void)
{
    VjoConfig previous = cfg;
    vjo_config_load(&cfg, &scratch);
    if (vjo_selftest_armed())
        cfg.ocr_mode = VJO_OCR_AUTO; /* the first settled screen is recognized unasked */
    if (!same_pipeline(&previous, &cfg)) {
        cache_ok = 0;
        retire_local_text(1);
        if (job_running && !same_pipeline(&job_cfg, &cfg))
            job_cancelled = 1;
    } else if (previous.local_dictionary_results != cfg.local_dictionary_results) {
        /* A presentation of the same recognized text: look the word up again
         * (from its best match) without recapturing or rerunning OCR. */
        cache_ok = 0;
        sceKernelLockMutex(cmd_lock, 1, NULL);
        vjo_view_lock();
        local_page = 0;
        g_view.dict_page = g_view.dict_pages = 0;
        vjo_view_unlock();
        sceKernelUnlockMutex(cmd_lock, 1);
    }
    vjo_log_configure(&cfg);
    vjo_strip_configure(subtitles, &cfg);
    anki_on = vjo_anki_configure(&cfg);
    if (vjoSetTriggers(cfg.toggle_button, cfg.subtitle_button) < 0)
        vjo_log("kernel refused the buttons: keeping the previous ones");
    /* Logged when they change, not at every load (each overlay open, game
     * start, subtitles on): test.2's log.txt had one line per load and key
     * (#101). The overlay still lists them each time it opens. */
    if (sceClibMemcmp(cfg.warnings, previous.warnings, sizeof(cfg.warnings)))
        for (int i = 0; i < cfg.n_warnings; i++)
            vjo_log("config warning: %s", cfg.warnings[i]);
}

/* The region in force for this game (w == 0: none, the whole frame). The
 * self-test uses it as it is; its full-screen stage asks for the whole frame
 * itself (VJO_GAME_OCR_F_FULL). */
static VjoRect game_region;

static void push_region(void)
{
    VjoRect r;
    int found = vjo_region_load(title_id, &r, &scratch);
    vjoSetRegion(found && r.w ? &r : NULL);
    if (found && r.w) game_region = r;
    else sceClibMemset(&game_region, 0, sizeof(game_region));
    vjo_log("region: %s%s", found == VJO_REGION_GAME ? "this game's" : found ? "all games'" : "none",
            found && r.w ? "" : ", full screen");
}

static int game_active(void)
{
    return title_id[0] != '\0';
}

/* The running job captured another screen than the one settled now: it is
 * abandoned (recognition and a relay's lookup take seconds), so the job for this
 * screen can start at once. Returns 1 if it was. A lookup in the shown text
 * is never abandoned: it does not depend on the screen. */
static int cancel_outdated_job(const VjoState *st)
{
    __sync_synchronize(); /* pairs with the network thread's barrier */
    if (!job_running || job_cursor_only || job_cancelled || !job_captured || !(st->stable || st->quiet) ||
        shows_screen(job_capture_scene, job_capture_seq, st))
        return 0;
    job_cancelled = 1;
    vjo_dlog("job cancelled: the screen changed since its capture");
    return 1;
}

/* Is the cached result for the screen in st? If so it is retagged with
 * st's scene, which stays valid through later captures. */
static int claim_cache_for(const VjoState *st)
{
    if (!cache_ok || active < 0 || !shows_screen(cache_scene, cache_seq, st))
        return 0;
    if (cache_scene != st->scene) {
        vjo_view_lock();
        cache_scene = st->scene;
        vjo_view_unlock();
    }
    return 1;
}

static void clear_input_block_error(void)
{
    if (input_block_error) {
        input_block_error = 0;
        vjo_strip_show("", VJO_STRIP_STATUS, 0);
    }
}

static void open_overlay(void)
{
    VjoState st;
    int block_rc = vjoSetInputBlock(1);
    vjo_dlog("input block on rc=%d (%08X) t=%u ms", block_rc, block_rc, vjo_log_ms());
    if (block_rc < 0) {
#if !VJO_DEBUG
        vjo_log("input block refused 0x%08X: the overlay cannot open", (unsigned)block_rc);
#endif
        input_block_error = 1;
        ov = OV_CLOSED;
        view_publish(0, NULL, NULL, 0);
        vjo_strip_show("Overlay unavailable: cannot block game controls.", VJO_STRIP_ERROR, 0);
        vjo_view_lock();
        __atomic_store_n(&g_view.strip_on, 1, __ATOMIC_RELEASE);
        vjo_view_unlock();
        return;
    }
    clear_input_block_error();
    ov = OV_OPEN;
#if VJO_DEBUG
    if (job_selftest && !job_cancelled) { /* set only while its job runs */
        job_cancelled = 1; /* the self-test yields: its stages run again later */
        vjo_log("self-test: stopped for the overlay");
    }
#endif
    apply_config(); /* settings are re-read on every open */
    if (anki_on)
        vjo_anki_post_count(); /* the queue line's count (Anki's thread, no allocation) */
    st.size = sizeof(st);
    vjoGetState(&st);
    if (local_text_valid &&
        local_text_game_generation == game_generation &&
        shows_screen(local_text_scene, local_text_capture_seq, &st)) {
        /* Reopening a completed lookup must not replace its word with the
         * pending character and repeat disk I/O. Cursor/text generations
         * exclude a result superseded before its next job has started. */
        if ((!job_running || job_selftest) && !cursor_waiting && job_cursor_seq == local_cursor_seq &&
            job_text_generation == local_text_generation && claim_cache_for(&st)) {
            vjo_dlog("open: saved local word");
            view_show_cache();
            return;
        }
        local_header = (VjoEntryList){.header = local_text};
        view_publish(1, &local_header, "Looking up words…", 0);
        /* Opening while the prepared first lookup is still finishing can
         * show that result directly; don't queue the same lookup again. */
        if (job_running && job_lookup && !job_cancelled &&
            job_text_generation == local_text_generation && job_cursor_seq == local_cursor_seq)
            return;
        start_cursor_job();
        return;
    }
    if (st.alloc_status == VJO_ALLOC_FAIL) {
        view_publish(1, NULL, "Not enough memory to capture the screen", 1);
        return;
    }
    if (claim_cache_for(&st)) { /* a background job's, or the subtitles' */
        vjo_dlog("open: background result");
        view_show_cache();
        return;
    }
    /* Why the background result can't be used (tuning). */
    vjo_dlog("open: %s (stable %u, quiet %u)",
            job_running ? "background job still running"
            : !cache_ok || active < 0 ? "no background result"
                                      : "screen changed since the background result",
            st.stable, st.quiet);
    view_publish(1, NULL, "Recognizing…", 0);
    view_waiting = 1;
    if (job_running) {
        ov = OV_OPEN_OLD_JOB; /* a cancelled one is followed by a new one */
        cancel_outdated_job(&st);
    } else {
        start_job("overlay opened", 1);
    }
}

/* Why the overlay closes (close_overlay_for): logged, and it decides what the
 * page does with SceShell's front touch region (touch_hold.h). */
enum { CLOSE_IN_GAME = 0, CLOSE_GAME_LEFT, CLOSE_FOREGROUND };

static void close_overlay_for(int why)
{
    static const char *const names[] = {"in game", "game left", "foreground changed"};
    int rc;
    if (ov == OV_CLOSED) {
        /* Closed in game a moment ago: if the page has not closed yet, it
         * leaves the region to SceShell as well (a later close sets it again). */
        if (why == CLOSE_GAME_LEFT) {
            vjo_view_lock();
            g_view.touch_handback = VJO_TOUCH_LEAVE;
            vjo_view_unlock();
        }
        return;
    }
    ov = OV_CLOSED;
    idle_since_us = now_us();
    cursor_waiting = 0;
    refresh_pending = open_retry = 0;
    /* A lookup's or a refresh's result is for the open overlay only; with the
     * overlay closed, Auto recognizes settled screens itself. */
    if (job_running && (job_cursor_only || job_refresh)) job_cancelled = 1;
    view_waiting = view_job_text = 0;
    rc = vjoSetInputBlock(0); /* the kernel already dropped it if the foreground changed */
    vjo_view_lock();
    __atomic_store_n(&g_view.open, 0, __ATOMIC_RELEASE);
    /* #104: with SceShell itself in front (the game suspended by PS, or gone)
     * its LiveArea owns the region; the page must not deactivate it then. */
    g_view.touch_handback = why == CLOSE_GAME_LEFT ? VJO_TOUCH_LEAVE : VJO_TOUCH_RELEASE;
    if (g_view.notice_shown) /* the Overlay app's notice is said once (check_app) */
        g_view.notice = NULL;
    /* The next job may reuse the shown arena; an open republishes it. */
    g_view.list = NULL;
    g_view.local_cursor_mode = 0;
    g_view.version++;
    vjo_view_unlock();
    vjo_dlog("overlay close (%s) t=%u ms: input block off rc=%d", names[why], vjo_log_ms(), rc);
#if !VJO_DEBUG
    if (rc < 0)
        vjo_log("input block off refused 0x%08X", (unsigned)rc);
#endif
}

/* Cancel, L + R, Select + R, a saved region...: its game is still in front. */
static void close_overlay(void)
{
    close_overlay_for(CLOSE_IN_GAME);
}

static void backoff_note(Backoff *b, int failed)
{
    if (!failed) {
        b->us = 0;
        b->until = 0;
        return;
    }
    b->us = b->us ? b->us * 2 : BACKOFF_MIN_US;
    if (b->us > BACKOFF_MAX_US)
        b->us = BACKOFF_MAX_US;
    b->until = now_us() + b->us;
}

/* The strip for a finished job: its sentence, else its error. */
static void strip_show_result(const VjoOverlayData *d)
{
    size_t mark = vjo_arena_mark(&scratch);
    if (d->sentence)
        vjo_strip_show(d->sentence, VJO_STRIP_SENTENCE, 0);
    else
        vjo_strip_show(vjo_err_text(&scratch, d->failed_stage, &d->err), VJO_STRIP_ERROR, 0);
    vjo_arena_release(&scratch, mark);
}

/* The overlay was opened while the job ran: is the result for the screen
 * shown now, with its lookup? Else a new job. Returns 1 if one started. */
static int restart_old_job(int has_lookup)
{
    VjoState st;
    if (ov != OV_OPEN_OLD_JOB)
        return 0;
    ov = OV_OPEN;
    st.size = sizeof(st);
    vjoGetState(&st);
    if (shows_screen(job_capture_scene, job_capture_seq, &st) && has_lookup)
        return 0;
    if (local_text_valid &&
        local_text_game_generation == game_generation &&
        shows_screen(local_text_scene, local_text_capture_seq, &st)) {
        start_cursor_job();
        return 1;
    }
    start_job(has_lookup ? "result was for an earlier screen" : "the running job was OCR only", 1);
    return 1;
}

/* The network thread finished the job. A lookup's result is shown when it is
 * for the text and cursor shown now; the screen no longer matters (#81
 * request 4). */
static void on_cursor_done(void)
{
    job_running = job_done = job_text_ready = 0;
    sceKernelLockMutex(cmd_lock, 1, NULL);
    int current = job_generation == game_generation &&
                local_text_valid && job_text_generation == local_text_generation &&
                same_pipeline(&job_cfg, &cfg) && ov != OV_CLOSED;
    int valid = current && !job_cancelled;
    int newer = valid && (job_cursor_seq != local_cursor_seq ||
                          job_cfg.local_dictionary_results != cfg.local_dictionary_results);
    if (valid && !newer) {
        active = 0;
        cache_ok = cache_data[0].err.rc == VJO_OK;
        cache_scene = job_capture_scene;
        cache_seq = job_capture_seq;
        local_page = cache_data[0].page; /* a missing page falls back to the best match */
        backoff_note(&dict_backoff, cache_data[0].err.rc != VJO_OK);
        view_show_cache();
    }
    job_cancelled = 0;
    sceKernelUnlockMutex(cmd_lock, 1);
    /* A close cancels the old result; a reopen may already be waiting for
     * another lookup of this still-current saved text. */
    if (newer || (current && cursor_waiting)) start_cursor_job();
    else cursor_waiting = 0;
}

/* VJO_E_SCREEN_CHANGED: the screen changed under every read of a job (the
 * worker reads a region again up to VJO_VOCR_TRIES times), so nothing is
 * shown for it, no error either. The next try starts at once, from the
 * second in a row SCREEN_RETRY_MS later, doubling up to SCREEN_RETRY_MAX_MS;
 * a job that reads its screen ends the run. */
static void note_screen_changed(const char *what)
{
    uint32_t wait_ms = screen_changed_streak ? SCREEN_RETRY_MS << (screen_changed_streak - 1) : 0;
    if (wait_ms > SCREEN_RETRY_MAX_MS)
        wait_ms = SCREEN_RETRY_MAX_MS;
    if (screen_changed_streak < 8)
        screen_changed_streak++;
    screen_retry_until = now_us() + (int64_t)wait_ms * 1000;
    vjo_dlog("%s dropped: the screen changed under it (%d in a row), next try in %u ms", what,
            screen_changed_streak, (unsigned)wait_ms);
}

/* A refresh ended (#81 request 4). Whatever happened, the shown text stays
 * usable. The same text keeps everything, now tagged with this capture, so
 * the next settled screen is compared with it; no text (a box between lines)
 * keeps it too; a different text replaces it quietly: the text, the cursor on
 * its first character and the static header leave nothing of the old text's
 * entry or pages in the view, so neither the overlay nor an Anki save pairs
 * them with the new text (requests made for the old one fail its generation),
 * the subtitle strip and the cards' model follow it, and its first word is
 * looked up as on opening. A cancelled refresh (a lookup went first) is asked
 * again; one whose screen changed under it follows note_screen_changed's pace;
 * a failed one backs off as any OCR job (ocr_backoff), quietly. */
static void on_refresh_done(void)
{
    const char *what = "kept";
    int rc, cancelled = job_cancelled, replaced = 0;
    size_t mark = vjo_arena_mark(&scratch);
    __sync_synchronize(); /* pairs with the network thread's barrier: its result is read after it */
    rc = cache_data[1].err.rc;
    /* (one that yielded to a screenshot ended cancelled too: it is asked again) */
    cancelled |= rc == VJO_E_CANCELLED;
    job_running = job_done = job_text_ready = job_refresh = job_cancelled = 0;
    refresh_after_us = now_us() + REFRESH_GAP_US;
    if (ov == OV_OPEN_OLD_JOB) {
        /* Reopened during it, on a screen the saved text was not read from:
         * that opening waits for a recognition of its own. */
        ov = OV_OPEN;
        start_job("result was for an earlier screen", 1);
        return;
    }
    /* A game, region or pipeline change cancels a refresh (and retires the text). */
    if (cancelled || ov != OV_OPEN || !local_text_valid) {
        refresh_pending = cancelled && ov == OV_OPEN;
        what = "dropped";
    } else if (rc == VJO_E_SCREEN_CHANGED) {
        note_screen_changed("refresh");
        refresh_pending = 1;
        refresh_after_us = screen_retry_until;
        what = NULL;
    } else {
        /* It read its screen: as on_job_done, the pace starts over, and a
         * failure backs off as any OCR job's (Auto's after close too). */
        screen_changed_streak = 0;
        backoff_note(&ocr_backoff, rc != VJO_OK);
        if (rc == VJO_OK) {
            char *filtered = vjo_filter_lines(&scratch, cache_data[1].ocr_text, job_cfg.non_japanese_filter);
            const char *text = filtered ? vjo_java_trim(filtered) : "";
            size_t len = sceClibStrnlen(text, sizeof(local_text));
            sceKernelLockMutex(cmd_lock, 1, NULL);
            vjo_view_lock();
            if (!sceClibStrcmp(text, local_text)) {
                local_text_scene = job_capture_scene;
                local_text_capture_seq = job_capture_seq;
                if (active == 0) { /* a cached lookup of this text is this screen's too */
                    cache_scene = job_capture_scene;
                    cache_seq = job_capture_seq;
                }
                what = "same text";
            } else if (len && len < sizeof(local_text)) {
                sceClibMemcpy(local_text, text, len + 1);
                advance_generation(&local_text_generation);
                local_page = 0;
                local_cursor_byte = job_cfg.japanese_only_selection
                    ? (uint32_t)vjo_utf8_japanese_cursor_step(local_text, len, 0, 0) : 0;
                local_text_scene = job_capture_scene;
                local_text_capture_seq = job_capture_seq;
                cache_ok = 0; /* the arena's entry is the old text's */
                local_header = (VjoEntryList){.header = local_text};
                /* As post_local_lookup: the new text and its pending lookup, at once. */
                g_view.list = &local_header;
                g_view.cursor_byte = local_cursor_byte;
                g_view.text_generation = local_text_generation;
                g_view.dict_page = g_view.dict_pages = 0;
                sceClibSnprintf(g_view.status, sizeof(g_view.status), "Looking up words…");
                g_view.status_is_error = 0;
                g_view.list_seq++;
                g_view.version++;
                replaced = 1;
                what = "new text";
            }
            vjo_view_unlock();
            sceKernelUnlockMutex(cmd_lock, 1);
        }
    }
    if (what)
        vjo_dlog("refresh rc=%d: %s", rc, what);
    vjo_arena_release(&scratch, mark);
    if (replaced) {
        /* The new text's cards name the model that read it; the subtitle
         * strip shows it, as on_job_done's strip_show_result for any job
         * (its lookup tags the cache, so no job reads this screen after
         * close). Both take their own locks, so after the view's. */
        vjo_anki_set_ocr_model(refresh_model);
        if (subtitles)
            vjo_strip_show(local_text, VJO_STRIP_SENTENCE, 0);
    }
    if (replaced || (cursor_waiting && ov != OV_CLOSED))
        start_cursor_job(); /* the new text's first word, or a lookup that waited */
}

/* The self-test's job ended (selftest_job_due): it showed nothing. Done, or
 * stopped by the overlay or a new screen, when its stages run again after a
 * later job's text; an overlay opened meanwhile gets what it waits for. */
static void subtitles_show(const VjoState *st);
#if VJO_DEBUG
static void on_selftest_done(void)
{
    VjoState st = {.size = sizeof(st)};
    __sync_synchronize(); /* pairs with the network thread's barrier */
    job_running = job_done = job_text_ready = job_selftest = job_cancelled = 0;
    if (restart_old_job(0))
        return;
    if (cursor_waiting && ov != OV_CLOSED)
        start_cursor_job();
    if (subtitles && vjoGetState(&st) >= 0)
        subtitles_show(&st); /* turned on while it ran: it brought no result to show */
}
#endif

static void on_job_done(void)
{
    idle_since_us = now_us();
    if (job_cursor_only) { on_cursor_done(); return; }
    if (job_refresh) { on_refresh_done(); return; }
#if VJO_DEBUG
    if (job_selftest) { on_selftest_done(); return; }
#endif
    int idx = job_idx, ocr_failed;
    VjoOverlayData *d = &cache_data[idx];
    __sync_synchronize(); /* pairs with the network thread's barrier */
    job_running = 0;
    job_done = 0;
    job_text_ready = 0;
    /* A job that read a screen while it changed is as stale as a cancelled one:
     * nothing is shown for it, no error either (#81 request 4); its next try
     * follows note_screen_changed's pace. */
    if (job_cancelled || job_generation != game_generation || !same_pipeline(&job_cfg, &cfg) ||
        d->err.rc == VJO_E_SCREEN_CHANGED) {
        if (d->err.rc == VJO_E_SCREEN_CHANGED && !job_cancelled) /* a game change cancels its jobs */
            note_screen_changed("job");
        cache_ok = 0;
        job_cancelled = 0;
        stable_pending = game_active(); /* only the current game's next screen (Auto) */
        if (ov != OV_CLOSED && stable_pending) { /* the overlay waits for this screen's */
            ov = OV_OPEN;
            if (now_us() < screen_retry_until)
                open_retry = 1; /* "Recognizing…" stays until background_open retries */
            else
                start_job("the screen changed during the last one", 1);
        } else if (subtitles) {
            vjo_strip_set_busy(0);
        }
        return;
    }
    screen_changed_streak = 0; /* this job read its screen */
    screen_retry_until = 0;
    ocr_failed = d->failed_stage == VJO_STAGE_OCR;
    backoff_note(&ocr_backoff, ocr_failed);
    if (subtitles)
        strip_show_result(d);
    if (!ocr_failed && job_lookup)
        backoff_note(&dict_backoff, d->err.rc != VJO_OK);
    /* Character moves received while initial lookup was working supersede
     * that definition; the saved OCR sentence is already immutable. So does
     * a local_dictionary_results change loaded by the overlay's opening. */
    if (local_text_valid && ov != OV_CLOSED &&
        (cursor_waiting || (job_lookup && (job_cursor_seq != local_cursor_seq ||
                                           job_cfg.local_dictionary_results != cfg.local_dictionary_results)))) {
        start_cursor_job();
        return;
    }
    if (!job_lookup && ov == OV_OPEN) {
        /* OCR only, while the overlay shows a result or an error: that stays
         * (and its arena stays active); the strip took the sentence */
        return;
    }
    /* Publish the result. */
    sceKernelLockMutex(cmd_lock, 1, NULL);
    if (ov != OV_CLOSED && job_lookup && job_cursor_seq != local_cursor_seq) {
        sceKernelUnlockMutex(cmd_lock, 1);
        start_cursor_job();
        return;
    }
    vjo_view_lock();
    active = idx;
    /* The overlay needs the lookup, made with the current page count. */
    cache_ok = d->err.rc == VJO_OK && job_lookup &&
               job_cfg.local_dictionary_results == cfg.local_dictionary_results;
    cache_scene = job_capture_scene;
    cache_seq = job_capture_seq;
    vjo_view_unlock();
    int needs_restart = ov == OV_OPEN_OLD_JOB;
    if (!needs_restart && ov != OV_CLOSED)
        view_show_cache();
    sceKernelUnlockMutex(cmd_lock, 1);
    /* Restart helpers may take cmd_lock; never call them while holding it. */
    if (needs_restart && !restart_old_job(job_lookup) && ov != OV_CLOSED) {
        sceKernelLockMutex(cmd_lock, 1, NULL);
        int newer = job_lookup && job_cursor_seq != local_cursor_seq;
        if (!newer) view_show_cache();
        sceKernelUnlockMutex(cmd_lock, 1);
        if (newer) start_cursor_job();
    }
}

/* Does a job started now look the words up? Yes when they are shown: the
 * overlay is open, or opens from the background result (auto: a local
 * dictionary prepares only the first whole-word match). Subtitle jobs stay
 * OCR-only (the strip shows the text alone) and allocate no result arena
 * for a local dictionary. */
static int want_lookup(void)
{
    return (ov != OV_CLOSED || (cfg.ocr_mode == VJO_OCR_AUTO && cfg.highlight_mode == VJO_HIGHLIGHT_WORD)) &&
           now_us() >= dict_backoff.until;
}

/* The network thread posted the job's sentence (before its lookup). */
static void on_job_text(void)
{
    const char *sentence;
    __sync_synchronize(); /* pairs with the network thread's barrier */
    sentence = cache_data[job_idx].sentence;
    job_text_ready = 0;
    if (job_refresh || job_cancelled || job_generation != game_generation)
        return; /* a refresh shows (and logs) nothing until it is done; or for an earlier screen */
    vjo_dlog("subtitle text ready in %d ms", (int)((now_us() - job_started_us) / 1000));
    if (subtitles && sentence)
        vjo_strip_show(sentence, VJO_STRIP_SENTENCE, 1);
    /* The overlay waits for this job: the sentence now, the words after
     * the lookup (if the job is for the screen shown). */
    if (ov != OV_CLOSED && view_waiting && job_lookup && sentence) {
        VjoState st;
        st.size = sizeof(st);
        vjoGetState(&st);
        if (shows_screen(job_capture_scene, job_capture_seq, &st)) {
            job_text_list.header = sentence;
            job_text_list.entries = NULL;
            job_text_list.n_entries = 0;
            view_publish(1, &job_text_list, "Looking up words…", 0);
            view_job_text = 1;
        }
    }
}

/* The subtitle strip for the screen in st: the cached result's sentence when it
 * is this screen's and no job runs; with the overlay open, the text it shows,
 * which stays whatever the screen does (#81 request 4: a new screen is the quiet
 * refresh's, whose text the strip then shows, on_refresh_done), never a job of
 * the strip's that would retire it (audit-overlay-first-ocr-b RB1); else
 * "Recognizing…" and a job (a running one brings its result: on_job_done). */
static void subtitles_show(const VjoState *st)
{
    const VjoOverlayData *d = active >= 0 ? &cache_data[active] : NULL;
    if (!job_running && d && d->sentence && shows_screen(cache_scene, cache_seq, st)) {
        vjo_strip_show(d->sentence, VJO_STRIP_SENTENCE, 0);
        return;
    }
    if (ov != OV_CLOSED) {
        const char *text = local_text_valid ? local_text : d ? d->sentence : NULL;
        if (text && text[0])
            vjo_strip_show(text, VJO_STRIP_SENTENCE, 0);
        return;
    }
    vjo_strip_show("Recognizing…", VJO_STRIP_STATUS, 1);
    start_job("subtitles on", want_lookup()); /* if one runs, its result comes */
}

static void set_subtitles(int on)
{
    if (on) apply_config();
    VjoState st;
    subtitles = on;
    vjo_strip_configure(on, &cfg);
    vjo_dlog("subtitles %s", on ? "on" : "off");
    if (!on) {
        vjo_strip_show("", VJO_STRIP_SENTENCE, 0);
        return;
    }
    /* settings were re-read before choosing the backend */
    st.size = sizeof(st);
    vjoGetState(&st);
    if (st.alloc_status == VJO_ALLOC_FAIL) {
        vjo_strip_show("Not enough memory to capture the screen", VJO_STRIP_ERROR, 0);
        return;
    }
    subtitles_show(&st);
}

/* subtitle_button: from the full overlay, everything closes. */
static void on_subtitle(void)
{
    if (!game_active())
        return;
    if (ov != OV_CLOSED) {
        close_overlay();
        if (subtitles)
            set_subtitles(0);
    } else {
        set_subtitles(!subtitles);
    }
}

/* A command from the overlay (paf main thread). */
static void on_command(void)
{
    int cmd;
    VjoRect r;
    uint32_t cursor_byte, cursor_generation;
    sceKernelLockMutex(cmd_lock, 1, NULL);
    cmd = pending_cmd;
    r = pending_rect;
    cursor_byte = pending_cursor_byte;
    cursor_generation = pending_cursor_generation;
    pending_cmd = VJO_CMD_NONE;
    sceKernelUnlockMutex(cmd_lock, 1);

    switch (cmd) {
    case VJO_CMD_CURSOR:
        if (ov == OV_CLOSED || !local_text_valid ||
            cursor_generation != local_text_generation || cursor_byte != local_cursor_byte)
            break;
        if (!job_running || !job_cursor_only || job_cursor_seq != local_cursor_seq)
            start_cursor_job();
        break;
    case VJO_CMD_CLOSED:
        close_overlay();
        break;
    case VJO_CMD_SET_REGION:
    case VJO_CMD_CLEAR_REGION:
        if (!game_active()) { /* the game exited meanwhile */
            close_overlay();
            break;
        }
        if (cmd == VJO_CMD_SET_REGION)
            vjo_log("region set for %s: %u,%u,%u,%u", title_id, r.x, r.y, r.w, r.h);
        else
            vjo_log("region for %s: full screen", title_id);
        if (vjo_region_save(title_id, cmd == VJO_CMD_SET_REGION ? &r : NULL, &scratch) < 0)
            vjo_log("region.ini: write failed");
        push_region();
        cache_ok = 0;
        retire_local_text(1);
        close_overlay();
        if (subtitles)
            start_job("region changed", want_lookup());
        break;
    }
}

/* ---- is the new foreground process a game? ----
 * Its title ID is often not readable right when the process starts, so the
 * lookup is retried for a few seconds. Unknown after that = a game. */
#define CLASSIFY_TIMEOUT_US 3000000

static SceUID pending_pid;
static int64_t pending_since;

/* The installed Overlay app against the one the plugins came with
 * (core/app_stamp.h), at each enabled game's start (the app can be
 * reinstalled while SceShell runs): logged when the answer changes, and a
 * mismatch is said once, in the body of the next overlay that draws its
 * page (an overlay refused Paf memory for it keeps the notice for the next). */
static int app_state = -1;

const char *vjo_view_notice(void)
{
    if (g_view.notice)
        g_view.notice_shown = 1; /* drawn: closing this overlay drops it (close_overlay) */
    return g_view.notice;
}

static void check_app(void)
{
    char installed[VJO_APP_STAMP_MAX], package[VJO_APP_STAMP_MAX];
    int irc = vjo_file_line(VJO_APP_STAMP_INSTALLED, installed, sizeof(installed));
    int prc = vjo_file_line(VJO_APP_STAMP_PACKAGE, package, sizeof(package));
    int app = vjo_file_exists(VJO_APP_EBOOT_INSTALLED);
    int s = vjo_app_state(irc, installed, app, prc, package);
    if (s == app_state)
        return;
    app_state = s;
    vjo_view_lock(); /* the overlay is closed: a game is starting */
    g_view.notice = s != VJO_APP_MISMATCH ? NULL
                    : vjo_app_stamp_api(installed) > vjo_app_stamp_api(package) ? VJO_APP_NOTICE_NEWER : VJO_APP_NOTICE;
    g_view.notice_shown = 0;
    vjo_view_unlock();
    vjo_log("overlay app %s: installed '%s' (0x%08X, app %d), plugins' '%s' (0x%08X)",
            s == VJO_APP_OK ? "ok" : s == VJO_APP_MISMATCH ? "MISMATCH" : "unknown", installed, irc, app, package,
            prc);
}

static void activate_game(SceUID pid, const VjoGameId *id)
{
    const char *tid = id->key;
    int mode = id->mode;
    if (!vjo_game_enabled(tid, id->psp, &scratch)) {
        title_id[0] = '\0';
        vjoSetGameActive(pid, VJO_GAME_NONE);
        vjo_log("%s: JP Overlay disabled; game input passes through", tid);
        vjo_selftest_game(tid, 0, mode, NULL);
        return;
    }
    sceClibSnprintf(title_id, sizeof(title_id), "%s", tid);
    {
        char name[128];
        vjo_title_name(pid, tid, name, sizeof(name)); /* "" for an unreadable title */
        /* recorded on Anki cards: a PSP game's disc ID and title when the Overlay app described it */
        vjo_anki_set_game(id->disc[0] ? id->disc : tid, id->name[0] ? id->name : name, id->psp);
    }
    apply_config();
    push_region();
    backoff_note(&ocr_backoff, 0);
    backoff_note(&dict_backoff, 0);
    vjoSetGameActive(pid, mode);
    vjo_log("game %s started: dictionary " VJO_DICT_NAME ", trigger %s, subtitles %s, ocr_mode %s", title_id,
            vjo_trigger_name(cfg.toggle_button), vjo_trigger_name(cfg.subtitle_button),
            cfg.ocr_mode == VJO_OCR_AUTO ? "auto" : "on_press");
    vjo_ledger_shell_log("start");
    vjo_selftest_game(tid, 1, mode, &game_region);
    check_app(); /* after the game is active: its file reads never delay that */
}

static void classify_pending(void)
{
    VjoGameId id;
    SceUID pid = pending_pid;
    int ret = vjo_game_identify(pid, &id);
    if (ret == 0) {
        pending_pid = 0;
        if (id.mode == VJO_GAME_NONE) {
            vjo_dlog("%s: system app, not a game", id.key);
            vjoSetGameActive(pid, VJO_GAME_NONE);
        } else {
            activate_game(pid, &id);
        }
    } else if (now_us() - pending_since > CLASSIFY_TIMEOUT_US) {
        pending_pid = 0;
        vjo_log("no title ID for 0x%X (0x%08X): treating it as a game", pid, ret);
        sceClibMemcpy(id.key, "GAME", 5); /* the rest is zero (vjo_game_identify) */
        id.mode = VJO_GAME;
        activate_game(pid, &id);
    }
}

static void invalidate_game_work(void)
{
    if (++game_generation == 0)
        game_generation = 1;
    cache_ok = 0;
    retire_local_text(1);
    stable_pending = 0;
    screen_changed_streak = 0; /* the next game's pace starts over */
    screen_retry_until = 0;
    if (job_running)
        job_cancelled = 1;
}

static void on_game_exit(void)
{
    /* The foreground now: none is SceShell itself (the game suspended by PS,
     * or gone); a process means the kernel went back to one behind it.
     * kernel.txt logs the transition itself (t=, the same clock). */
    VjoState st = {.size = sizeof(st)};
    vjoGetState(&st);
    invalidate_game_work();
    pending_pid = 0;
    close_overlay_for(st.game_pid ? CLOSE_FOREGROUND : CLOSE_GAME_LEFT);
    if (subtitles)
        set_subtitles(0);
    clear_input_block_error();
    /* Free memory unless the network thread still uses it. */
    if (!net_busy())
        mem_free();
    title_id[0] = '\0';
    vjo_anki_set_game("", "", 0);
    vjo_ledger_shell_log("exit");
    net_stack_check();
    vjo_selftest_step("exit");
}

/* A new foreground process: classified by classify_pending. The overlay is
 * closed, whatever it is: a resumed game starts with it closed. */
static void on_game_start(void)
{
    VjoState st;
    invalidate_game_work();
    close_overlay_for(CLOSE_FOREGROUND);
    if (subtitles) /* over a game, without its exit: the strip goes */
        set_subtitles(0);
    clear_input_block_error();
    st.size = sizeof(st);
    vjoGetState(&st);
    title_id[0] = '\0';
    pending_pid = st.game_pid;
    pending_since = now_us();
}

static void on_trigger(void)
{
    if (ov != OV_CLOSED)
        close_overlay();
    else if (game_active())
        open_overlay();
}

/* A background job for a settled screen: the auto prefetch (for an instant
 * overlay) and the subtitles' refresh (whatever ocr_mode says). */
static int background_wanted(void)
{
    return subtitles || cfg.ocr_mode == VJO_OCR_AUTO;
}

static int background_throttled(void)
{
    int64_t t = now_us();
    return t < ocr_backoff.until || t < screen_retry_until || (subtitles && t < job_started_us + SUBTITLE_GAP_US);
}

/* A stable event that arrives while busy (or throttled) is kept, so the
 * newest screen is still recognized afterwards. With subtitles, a region
 * that never settles (a video behind the text) is recognized every
 * UNSETTLED_US anyway. */
static void auto_prefetch(void)
{
    VjoState st;
    int unsettled_due;
    if (ov != OV_CLOSED || job_running || !game_active() || !background_wanted() || background_throttled())
        return;
    unsettled_due = subtitles && now_us() - job_started_us >= UNSETTLED_US;
    if (!stable_pending && !unsettled_due)
        return;
    st.size = sizeof(st);
    vjoGetState(&st);
    if (stable_pending) {
        stable_pending = 0;
        if ((st.stable || st.quiet) && !claim_cache_for(&st))
            start_job(st.stable ? "auto: screen settled" : "auto: screen quiet", want_lookup());
    } else if (st.unsettled_ms >= UNSETTLED_US / 1000) {
        start_job("subtitles: screen keeps changing", 0); /* no lookup every 2 s */
    }
}

/* The open overlay's own jobs, paced: the retry of one whose screen changed
 * under it (note_screen_changed: "Recognizing…" stays meanwhile), and the
 * refresh of the text it shows once a screen settled that the text was not
 * read from (#81 request 4; on_refresh_done), REFRESH_GAP_US apart, after the
 * OCR backoff of a failed one and REFRESH_IDLE_US after the last lookup: the
 * user's lookups go first. */
static void background_open(void)
{
    VjoState st = {.size = sizeof(st)};
    int64_t t = now_us();
    if (ov != OV_OPEN || job_running || pending_cmd != VJO_CMD_NONE)
        return;
    if (open_retry) {
        if (t >= screen_retry_until) {
            open_retry = 0;
            start_job("the screen changed during the last one", 1);
        }
        return;
    }
    if (!refresh_pending || t < refresh_after_us || t < ocr_backoff.until || t - lookup_us < REFRESH_IDLE_US ||
        shot_waiting())
        return;
    refresh_pending = 0;
    /* Local text is valid only for this game's local pipeline (retired at every
     * change). ocr_mode = on_press recognizes only when the overlay opens. */
    if (local_text_valid && cfg.ocr_mode == VJO_OCR_AUTO && vjoGetState(&st) >= 0 &&
        (st.stable || st.quiet) && !shows_screen(local_text_scene, local_text_capture_seq, &st))
        start_job_kind("auto: screen settled", 0, 1);
}

/* The self-test's stages run in a background job of their own, never in a
 * job the user waits for (test.2's first scans took 9-15 s beside them,
 * docs/first-ocr.md): once the last job read text in the region, while its
 * screen is shown, the overlay is closed and nothing has run for
 * SELFTEST_IDLE_US. It shows
 * nothing (job_selftest). Its job's capture stays the last job's, the text's
 * (job_capture_*: a lookup's is the shown text's, the same; vocr_note_capture
 * leaves it), so a new screen stops it as any job (cancel_outdated_job), and
 * opening the overlay does too. */
#define SELFTEST_IDLE_US 2000000LL
#if !VJO_DEBUG
/* The normal build has no self-test: an armed one (tools/install_ftp.sh
 * --self-test) is said so once, at start, and its marker stays for a debug
 * build (docs/release-variants.md). */
void vjo_selftest_init(const VjoConfig *c)
{
    SceIoStat st;
    (void)c;
    if (sceIoGetstat(VJO_DATA_DIR "/selftest.txt", &st) >= 0)
        vjo_log("self-test: armed, but this is the normal build: install the -debug build to run it");
}
#endif
#if VJO_DEBUG
static void selftest_job_due(void)
{
    VjoState st = {.size = sizeof(st)};
    /* text_generation is this game's only after a job of it read text (run_vocr clears it after a
     * failure or a changed screen, and a game change moves game_generation on), so the game is the
     * active one and no OCR backoff or retry pace holds; a settled screen not read yet is Auto's
     * (auto_prefetch runs first: its job makes net_busy()). */
    if (!vjo_selftest_stages() || ov != OV_CLOSED || net_busy() || text_generation != game_generation ||
        now_us() - idle_since_us < SELFTEST_IDLE_US || vjoGetState(&st) < 0 ||
        !shows_screen(text_scene, text_seq, &st))
        return;
    /* Its stages publish their own captures (the kernel's region_seq moves on): a shown text and
     * a cached lookup that show this screen are tagged with its scene, so they still do after. */
    sceKernelLockMutex(cmd_lock, 1, NULL);
    if (local_text_valid && shows_screen(local_text_scene, local_text_capture_seq, &st))
        local_text_scene = st.scene;
    sceKernelUnlockMutex(cmd_lock, 1);
    claim_cache_for(&st);
    vjo_log("job start (self-test)");
    job_selftest = job_prepares = 1;
    job_refresh = job_lookup = job_cursor_only = job_cancelled = 0;
    job_idx = 1; /* results[1], always empty (one arena since the network pipeline went): never the shown result's */
    job_cfg = cfg;
    job_generation = game_generation;
    job_done = job_text_ready = 0;
    job_running = 1;
    job_started_us = now_us();
    __sync_synchronize();
    sceKernelSetEventFlag(net_evf, NET_EV_JOB);
}
#endif

/* Boot guard: this boot counts as complete once the control thread has run
 * for a minute (core/boot_guard.h). The kernel may answer BUSY while its
 * own uptime is shorter; a few spaced retries cover that. */
#define BOOT_OK_US        (60LL * 1000 * 1000)
#define BOOT_OK_RETRY_US  (5LL * 1000 * 1000)
#define BOOT_OK_ATTEMPTS  12
static int64_t ctl_started_us;
static int boot_ok_attempts; /* -1 once marked (or given up) */

static void boot_guard_ok(void)
{
    static int64_t next_us;
    int64_t now;
    if (boot_ok_attempts < 0)
        return;
    now = now_us();
    if (now - ctl_started_us < BOOT_OK_US || now < next_us)
        return;
    if (vjo_guard_mark(VJO_BOOT_STAGE_OK) == 0) {
        vjo_log("boot guard: ok after a stable minute");
        boot_ok_attempts = -1;
        vjo_selftest_step("ok");
    } else if (++boot_ok_attempts >= BOOT_OK_ATTEMPTS) {
        vjo_log("boot guard: ok not recorded after %d attempts", BOOT_OK_ATTEMPTS);
        boot_ok_attempts = -1;
    } else {
        next_us = now + BOOT_OK_RETRY_US;
    }
}

/* One wakeup of the control thread with the kernel's event bits. The order of
 * the handlers within one wakeup is part of the policy (e.g. a trigger and a
 * game start in the same wakeup). */
static void control_tick(uint32_t bits)
{
    boot_guard_ok();
    if (job_running) sceKernelPowerTick(0);
    if (bits & VJO_EV_GAME_EXIT)
        on_game_exit();
    if (bits & VJO_EV_GAME_START)
        on_game_start();
    if (pending_pid > 0)
        classify_pending();
    if (bits & VJO_EV_TRIGGER)
        on_trigger();
    if (bits & VJO_EV_SUBTITLE)
        on_subtitle();
    if (pending_cmd != VJO_CMD_NONE)
        on_command();
    if (job_running && job_text_ready)
        on_job_text();
    if (job_running && job_done)
        on_job_done();
    if (bits & (VJO_EV_REGION_STABLE | VJO_EV_REGION_QUIET)) {
        stable_pending = 1;
        if (ov == OV_OPEN)
            refresh_pending = 1; /* background_open decides */
        if (job_running) {
            VjoState st;
            st.size = sizeof(st);
            vjoGetState(&st);
            cancel_outdated_job(&st);
        }
    }
    auto_prefetch();
    background_open();
#if VJO_DEBUG
    selftest_job_due();
#endif
    vjo_anki_reap(); /* an Anki job's thread and stack go once it is done */
    if (!game_active() && !net_busy() && has_result_memory() && ov == OV_CLOSED)
        mem_free();
}

static int ctl_main(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    ctl_started_us = now_us();
    boot_ok_attempts = 0;
    while (running) {
        uint32_t bits = 0;
        vjoWaitEvent(VJO_EV_TRIGGER | VJO_EV_SUBTITLE | VJO_EV_REGION_STABLE | VJO_EV_REGION_QUIET |
                         VJO_EV_GAME_START | VJO_EV_GAME_EXIT,
                     &bits, POLL_TIMEOUT_US);
        control_tick(bits);
    }
    return 0;
}

int vjo_worker_start(void)
{
    int ver = vjoGetVersion(), rc;
    vjo_ledger_shell_init();
    if (ver != VJO_API_VERSION) {
        vjo_log("kernel API version %d, expected %d: not starting", ver, VJO_API_VERSION);
        return -1;
    }
    rc = vjoRegisterShell();
    if (rc < 0) {
        vjo_log("kernel refused the shell registration %d: not starting", rc);
        return -1;
    }
    view_lock = sceKernelCreateMutex("VjoView", 0, 0, NULL);
    cmd_lock = sceKernelCreateMutex("VjoCmd", 0, 0, NULL);
    net_evf = sceKernelCreateEventFlag("VjoNetEv", 0, 0, NULL);
#ifdef VJO_WITH_VOCR
    game_lock = sceKernelCreateMutex("VjoGameJob", 0, 0, NULL); /* before Anki: its screenshots take it */
    if (game_lock < 0) {
        vjo_worker_stop();
        return -1;
    }
#endif
    if (view_lock < 0 || cmd_lock < 0 || net_evf < 0) {
        vjo_worker_stop();
        return -1;
    }
    vjo_platform_vita(&plat);
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    vjo_arena_init(&results[0], NULL, 0);
    vjo_arena_init(&results[1], NULL, 0);
    if (vjo_anki_start() < 0)
        vjo_log("anki: mailbox unavailable: no card can be saved");
    apply_config(); /* logging, triggers and Anki are set up before the first game */
    vjo_selftest_init(&cfg);
    if (vjo_selftest_armed())
        apply_config(); /* the self-test's forced settings */
#if VJO_DEBUG
    vjo_log("Vita JP Overlay shell started (kernel API %d)", ver);
#else
    vjo_log("Vita JP Overlay shell started (kernel API %d, the normal build: a short log)", ver);
#endif

    net_thread = vjo_shell_thread_create(VJO_SL_NET_STACK, "VjoNet", net_main, VJO_STACK_SHELL_NET);
    ctl_thread = vjo_shell_thread_create(VJO_SL_CTL_STACK, "VjoControl", ctl_main, VJO_STACK_SHELL_CTL);
    if (net_thread < 0 || ctl_thread < 0) {
        vjo_worker_stop();
        return -1;
    }
    if (sceKernelStartThread(net_thread, 0, NULL) < 0) {
        vjo_worker_stop();
        return -1;
    }
    threads_started = 1;
    if (sceKernelStartThread(ctl_thread, 0, NULL) < 0) {
        vjo_worker_stop();
        return -1;
    }
    threads_started |= 2;
    vjo_ledger_shell_log("boot");
    return 0;
}

/* Also undoes a partial vjo_worker_start. */
int vjo_worker_stop(void)
{
    running = 0;
    if (net_evf >= 0)
        sceKernelSetEventFlag(net_evf, NET_EV_QUIT);
    if (threads_started) {
        if (((threads_started & 1) && sceKernelWaitThreadEnd(net_thread, NULL, NULL) < 0) ||
            ((threads_started & 2) && sceKernelWaitThreadEnd(ctl_thread, NULL, NULL) < 0)) {
            vjo_log("worker join failed; keeping allocations until stop can be retried");
            return -1;
        }
        threads_started = 0;
    }
    if (net_thread >= 0)
        vjo_shell_thread_delete(VJO_SL_NET_STACK, net_thread, VJO_STACK_SHELL_NET);
    if (ctl_thread >= 0)
        vjo_shell_thread_delete(VJO_SL_CTL_STACK, ctl_thread, VJO_STACK_SHELL_CTL);
    net_thread = ctl_thread = -1;
    /* An Anki job uses the view and capture locks: it is cancelled and
     * joined before they are deleted (the control thread no longer reaps it). */
    if (vjo_anki_stop() < 0)
        return -1;
    mem_free();
    if (net_evf >= 0)
        sceKernelDeleteEventFlag(net_evf);
    if (cmd_lock >= 0)
        sceKernelDeleteMutex(cmd_lock);
    if (view_lock >= 0)
        sceKernelDeleteMutex(view_lock);
    net_evf = cmd_lock = view_lock = -1;
#ifdef VJO_WITH_VOCR
    if (game_lock >= 0) /* (the Anki job, which takes it for a screenshot, was joined above) */
        sceKernelDeleteMutex(game_lock);
    game_lock = -1;
#endif
    return 0;
}
