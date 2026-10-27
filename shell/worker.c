/* Control thread (kernel events, overlay state, auto/on-press policy) and
 * network thread (a job of the game worker: vita-vn-ocr in the game; then the
 * dictionary). The overlay is offline: Google Lens was removed (2026-10-15). */
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/rtc.h>
#if !VJO_DEBUG
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#endif

#include "../core/boot_guard.h"
#include "../core/app_stamp.h"
#include "../include/vjo_ledger_budget.h"
#include "../core/regions.h"
#include "../core/region_select.h" /* #358: the suggested box's word (vjo_rsel_publish) */
#include "../core/port.h"
#include "../core/anki.h"
#include "../core/local_dict.h"
#include "../core/textfilter.h"
#include "../core/textnorm.h"
#include "../core/utf.h"
#include "../core/text_rows.h"
#include "../core/history_lines.h"
#include "../core/charmap.h"
#ifdef VJO_WITH_VOCR
#include "../core/local_ocr.h"
#include "game_ocr_client.h"
#include "history.h"
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
 * text to hold still (vocr_run_one's request). Nothing is shown for either,
 * until a try of the open overlay starts after WAIT_HINT_AFTER in a row: that
 * one says why it waits (ocr_wait_hint, #358: start_job_kind). */
#define SCREEN_RETRY_MS      250u
#define SCREEN_RETRY_MAX_MS 4000u
#define WAIT_HINT_AFTER 2u /* #358: measured (docs/text-stability.md "Why the overlay waits") */

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
static int job_prepare;
static uint32_t prepared_seq, prepare_cursor;
static int prepare_left;
static size_t prepared_end;
static uint32_t job_cursor_byte, job_cursor_seq, job_text_generation;
/* Dictionary paging at the cursor (local_dictionary_results > 1): the
 * requested page shares the cursor sequence. Page locators live at the start
 * of the single result arena (cache_data[0].pages); they are reused only for
 * the text generation and result limit that built them (network thread). */
static int local_page, job_page;
static uint32_t pages_generation;
static int pages_limit;
/* Line history (owner 2026-10-20, docs/line-history.md): Up from the overlay's top row shows this
 * game's earlier lines as history.csv keeps them (Read text: Automatically's). While one is shown,
 * `history` is the visit's block (Paf, VJO_SL_PAF_HISTORY: history_open) and local_text is that line,
 * so its lookups, pages and cards are the current text's ways; the current text waits in it as line
 * 0. history_step: a step asked for (history_ask), 1 earlier, -1 later, taken once no job runs (one
 * reads local_text: control_tick). Control thread; the view's pointers under the view lock. */
typedef struct {
    VjoHistory h;
    VjoLineDebug current_debug;
    uint32_t cursor, scene, seq; /* the current text's cursor and capture, for the return to it */
} HistoryVisit;
static HistoryVisit *history;
static int history_step;

static VjoConfig cfg;      /* control thread */
static VjoConfig job_cfg;  /* snapshot used by the network thread */
static int anki_on;        /* vjo_anki_configure(&cfg): enter saves cards to the queue */
static VjoPlatform plat;
static int dict_close_pending; /* control thread: close after an in-flight lookup */
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
 * failing dictionary does not stop the subtitles, which need only OCR) and
 * one for the open overlay's refreshes (refresh_backoff).
 * Each event has one handler below (on_*). */
enum { OV_CLOSED, OV_OPEN, OV_OPEN_OLD_JOB };
static int ov = OV_CLOSED;
static int subtitles; /* written by set_subtitles only */
/* The strip says "Recognizing…" for the lookup or refresh in progress, which
 * brings it nothing (subtitles turned on after the close that dropped it): its
 * end gives the strip the screen's sentence, or a job of its own (#95 item 3a;
 * strip_after_wait). Set by subtitles_show only while such a job runs (or a
 * cancelled one, but not the self-test's), and by a region change while any job
 * but the self-test's runs (it cancels that job: it read the old region; #182),
 * so that job's end always clears it. */
static int strip_waiting;
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
/* API 31: source selection stays inside the game worker. Keep its installation counts for the
 * diagnostic log, once per session and whenever they change. */
static SceUID hook_pid;
static uint32_t hook_logged[3];
typedef struct {
    int64_t until, us;
} Backoff;
static Backoff ocr_backoff, dict_backoff;
/* The open overlay's refreshes back off on their own (#95 item 1): a failing
 * one (the game's memory, say) delays the next refresh, never Auto's job after
 * the overlay closes, which read the settled screen at once before refreshes
 * existed (#92). A job that reads its screen ends it too (on_job_done). */
static Backoff refresh_backoff;
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
/* The network thread's log batch (shell/log_vita.c), its buffer on net_main's stack: no job's line
 * stands between its result and the control thread (owner 2026-10-16, test.3; #360 for the debug
 * build; docs/release-variants.md "No log append on the result path"). Network thread only. */
#define NET_LOG_BYTES 2048 /* VJO_LOG_BATCH_LINES (24) lines of 85 B; a job's own: the normal build's none, or 1-5
                            * of 40-170 B; the debug build's 2-4 of a lookup (at most 255 B each), and an OCR
                            * job's text, 1-2 lines of at most 205 B a recognized line, its lookup's and done line */
#if !VJO_DEBUG
static VjoLogBatch net_log;  /* the network thread's batch; its buffer is on net_main's stack */
/* The normal build's lines of a job (its short log) never stand between a recognition and its result
 * (owner 2026-10-16, test.3; docs/release-variants.md): the network thread keeps a log batch open (its
 * buffer on net_main's stack), which holds every line it logs, and writes it, one append per file, once
 * its wait, timed while lines are held, ends after LOG_IDLE_US with no job and the control thread has
 * taken the result (net_main). Jobs started meanwhile keep them. Another thread's line, a configure that
 * changes log_file or a line the batch cannot take writes them at once, on that thread, in their order
 * (shell/log_vita.c). Network thread only. */
#define LOG_IDLE_US 500000u
/* At a job's wake: the batch opens unless it is open (its buf, set by the begin and cleared here when
 * it closes: only this thread closes it), so a job takes log_mutex for it only after a write. A begin
 * refused because another thread's batch is open (the ledger table's, microseconds at a game start or
 * exit) leaves this job's lines to go out one by one; the next job tries again. */
static void net_log_wake(char *buf)
{
    if (!net_log.buf)
        vjo_log_batch_begin(&net_log, buf, NET_LOG_BYTES);
}
/* After a wait (timed while a line is held): with the result taken (no job runs), what is held goes
 * out and the batch closes. A job's wake always finds job_running set (start_job_kind sets it before
 * NET_EV_JOB), so it writes nothing: only a timeout, or the stop, does. */
static void net_log_idle(void)
{
    if (!job_running) {
        vjo_log_batch_end(&net_log);
        net_log.buf = NULL;
    }
}
#else
/* The debug build's (#360): a job's lines from the start of a lookup job, and an OCR job's from the end of
 * its recognition (its text's lines: log_text_lines), are held until the job has published its result;
 * then net_job writes them at once, one append per file (and its done line with them): the same bytes in
 * the same order as line by line, within milliseconds. Before, its lookup line and its done line were an
 * append each (two while status.txt is under 64 KiB) before the result: 17 ms an append, and its done line
 * came 54 ms after the job's start against its lookup's 32 (medians, test.4's log.txt). The batch is never
 * open across an exchange with the game worker. Another thread's line, a configure (the debug build's writes
 * an open batch) or a line the batch cannot take writes the held lines first, in their order
 * (shell/log_vita.c). A crash before the write loses the held lines: at most 24 lines or 2 KiB. The batch
 * and its buffer are net_main's, on its stack (net_held: its b.buf is set while the batch is open). */
typedef struct {
    VjoLogBatch b;
    char buf[NET_LOG_BYTES];
} NetHeld;
static NetHeld *net_held;
static void net_log_hold(void)
{
    if (!net_held->b.buf) /* a begin refused beside another thread's batch: its lines go out one by one */
        vjo_log_batch_begin(&net_held->b, net_held->buf, NET_LOG_BYTES);
}
static uint32_t job_done_us; /* when the job published its result (job_done): the press-to-screen figure's */
#endif

static volatile int pending_cmd;
static VjoRect pending_rect;
static uint32_t pending_cursor_byte, pending_cursor_generation;
static void start_cursor_job(void);
static void note_pages(int rc, const VjoOverlayData *out);
static void subtitles_show(const VjoState *st);

static uint32_t advance_generation(uint32_t *value)
{
    if (++*value == 0) *value = 1;
    return *value;
}

/* Under the view lock: the view lets an earlier line go (its pointers are into the visit's block);
 * the block, which the caller frees once the lock is released (history_free), or NULL. */
static HistoryVisit *history_detach(void)
{
    HistoryVisit *v = history;
    history = NULL;
    history_step = 0;
    g_view.history = 0;
    g_view.history_before = g_view.history_after = NULL;
    return v;
}

static void history_free(HistoryVisit *v)
{
    if (v)
        vjo_shell_heap_free(VJO_SL_PAF_HISTORY, v, VJO_BUDGET_PAF_HISTORY);
}

static void retire_local_text(int new_generation)
{
    HistoryVisit *v;
    sceKernelLockMutex(cmd_lock, 1, NULL);
    vjo_view_lock();
    local_text_valid = 0;
    cursor_waiting = 0;
    local_page = 0;
    if (new_generation) advance_generation(&local_text_generation);
    g_view.list = NULL;
    g_view.local_cursor_mode = 0;
    sceClibMemset(&g_view.line_debug, 0, sizeof(g_view.line_debug));
    g_view.dict_page = g_view.dict_pages = 0;
    v = history_detach(); /* an earlier line goes with the text, the current one with it */
    g_view.version++;
    vjo_view_unlock();
    sceKernelUnlockMutex(cmd_lock, 1);
    history_free(v);
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
    /* a line-history step takes an empty slot only: never a close's, a region's or a lookup's */
    if (cmd < VJO_CMD_EARLIER || pending_cmd == VJO_CMD_NONE)
        pending_cmd = cmd;
    if (rect)
        pending_rect = *rect;
    sceKernelUnlockMutex(cmd_lock, 1);
    vjoNotifyShell();
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
#if VJO_DEBUG
    /* the press-to-screen figure: this request, from now (lookup_shown.h) */
    g_view.ask_seq = local_cursor_seq;
    g_view.ask_us = (uint32_t)sceKernelGetProcessTimeWide();
#endif
}

void vjo_post_cursor(uint32_t byte_offset, uint32_t text_generation)
{
    /* This order is also used by cursor completion: a newer request cannot
     * race past the stale-result check and republish the old definition.
     * A line-history step not taken yet gives the slot up: the latest press wins,
     * and an Up with nothing to go to never holds a move (audit-line-history F2). */
    sceKernelLockMutex(cmd_lock, 1, NULL);
    if (pending_cmd != VJO_CMD_NONE && pending_cmd < VJO_CMD_CURSOR) {
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
    vjoNotifyShell();
}

int vjo_post_page(int page, uint32_t text_generation)
{
    int accepted = 0;
    sceKernelLockMutex(cmd_lock, 1, NULL);
    if (pending_cmd == VJO_CMD_NONE || pending_cmd >= VJO_CMD_CURSOR) { /* (a step gives it up, as for a move) */
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
    if (accepted) vjoNotifyShell();
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
 * time; then the text at the cursor, the entry, its dictionary's name (the
 * file's title, as Bee's Ultimate Dictionary has, else the "[name]" first
 * line older conversions give a definition) and its frequency badge
 * (" #1234", " unranked" or none). The numbers come first and the texts are
 * bounded (32 + 31 + 31 + 31 + 15 bytes, whole characters), so the line
 * fits vjo_log's 254 bytes. */
static void log_lookup(const char *why, const VjoOverlayData *out, uint32_t cursor, int rc, int64_t t0)
{
    char query[33], head[32], reading[32], source[32], dict[8], rank[VJO_RANK_BADGE_MAX];
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
        if (e->vocab->dictionary)
            log_text(source, sizeof(source), e->vocab->dictionary, sceClibStrnlen(e->vocab->dictionary, 64), 31);
        else if (e->vocab->n_meanings) /* a "[JMdict Plain]" first line, as the converter writes one */
            vjo_anki_meaning_source(e->vocab->meanings[0], source, sizeof(source));
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
    /* Session metadata shares the 64 KiB allocation; resetting a lookup
     * never overwrites it. No new heap allocation or static file cache. */
    size_t session_bytes = vjo_local_session_size();
    plat.local_session = base;
    sceClibMemset(base, 0, session_bytes);
    vjo_arena_init(&results[0], (uint8_t *)base + session_bytes, bytes - session_bytes);
    vjo_arena_init(&results[1], NULL, 0);
    vjo_dlog("local dictionary workspace: %u bytes", (unsigned)bytes);
    active = -1;
    cache_ok = 0;
    return 0;
}

/* Called by the idle control thread or by the network thread owning its job. */
static void mem_free(void)
{
    vjo_local_session_close(plat.local_session);
    plat.local_session = NULL;
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
#if VJO_DEBUG
    /* the press-to-screen figure (lookup_shown.h): the lookup result shown is the single arena's, for the
     * cursor its job looked up; its job published it at job_done_us (the network thread, before job_done) */
    g_view.shown_seq = list == &cache_data[0].list ? job_cursor_seq : 0;
    g_view.done_us = job_done_us;
#endif
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
    g_view.on_text = cfg.overlay_on_text; /* API 46 */
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
static const char OOM_TEXT_BUFFER[] VJO_DATA_TABLE(OOM_TEXT_BUFFER) = "The recognized text did not fit the overlay's text buffer. Select a tighter "
    "dialogue region with Square.";
static const char OOM_GAME_REGION[] VJO_DATA_TABLE(OOM_GAME_REGION) = "Select a tighter dialogue region with Square.";
static const char OOM_GAME_NONE[] VJO_DATA_TABLE(OOM_GAME_NONE) = "No room for any OCR right now. Restart the game.";
static const char OOM_GAME_SMALLEST[] VJO_DATA_TABLE(OOM_GAME_SMALLEST) = "No room for the OCR model's weights. Try again later.";
static const char OOM_GAME_ALLOCATION[] VJO_DATA_TABLE(OOM_GAME_ALLOCATION) = "Allocation failed after the memory check passed. Try again.";
static const char OOM_GAME_UNKNOWN[] VJO_DATA_TABLE(OOM_GAME_UNKNOWN) = "Memory could not be checked. See log.txt.";
/* The dictionary's result arena refused (mem_alloc), after OCR or for a cursor's lookup. In the data
 * segment as the others: line history's code took the normal Shell's text room (docs/line-history.md). */
static const char OOM_SHELL_RESULTS[] VJO_DATA_TABLE(OOM_SHELL_RESULTS) = "Text was recognized, but Shell UI memory is too "
    "low for dictionary results. Restart the Vita and try again.";
/* The model's storage unavailable to the game (ENODEV): run_vocr. */
static const char MODEL_STORAGE[] VJO_DATA_TABLE(MODEL_STORAGE) = "The local OCR model storage is unavailable to this game. "
    "See log.txt for details.";

/* API 46, under the view lock: local_text's char map (core/charmap.h) is there (bytes), or not; the open overlay
 * reads it from local_text's tail (docs/overlay-on-text.md). */
static void cmap_publish(size_t bytes)
{
    if (!bytes && !g_view.cmap_text && !g_view.cmap_stale)
        return; /* no map before or now: the open overlay has nothing to redraw (audit-overlay-boxes F1) */
    g_view.cmap_text = bytes ? local_text : NULL;
    g_view.cmap_seq++;
    g_view.cmap_stale = 0;
}

#ifdef VJO_WITH_VOCR
/* Which part of the game worker's memory check failed: no OCR can fit
 * beside the reserve; the region's job workspace; the model's weights; an
 * allocation after a passed check; or the check itself. A job asks by tiers
 * (API 21): its last check is its smallest tier's. The advice names no model
 * or tier: log.txt has the tier lines. The cached error now also shows this
 * check's memory figures (owner 2026-10-23). */
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

/* The model a job that ran tier `ran` read its text with, a constant string: the
 * vertical model when its lines were columns (API 47: ocr_vertical on, or auto where
 * the rows looked like vertical text; the worker runs it whatever the tier), else
 * the tier's. A result without a recognized line has line[0] zeroed (the worker
 * clears it). The name is kept in the data segment: the normal Shell's code room
 * (docs/memory-ledger.md, "Vertical text"). */
static const char vertical_model[] VJO_DATA_TABLE(worker_vertical_model) = VJO_VOCR_VERTICAL_NAME;
static const char *job_model(const VjoGameOcrResult *r, unsigned ran)
{
    return r->line[0].flags & VJO_GAME_OCR_LINE_COLUMN ? vertical_model : vjo_vocr_tier_model((int)ran);
}

/* The tiers whose model is tier t's file, t's own bit included: a size may keep
 * the model of the size below it (two sizes, one file), and a file that failed
 * for one of them fails for both. */
static unsigned vocr_same_file(unsigned t)
{
    unsigned same = 0;
    for (unsigned s = 1; s <= VJO_VOCR_TIERS; s++)
        if (!sceClibStrcmp(vjo_vocr_tier_model((int)s), vjo_vocr_tier_model((int)t))) same |= 1u << (s - 1u);
    return same;
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
 * stopped the boost. The game worker writes that step only for a job that began
 * recognizing a line not in its line cache, when that recognition ends, done or
 * failed (core/vocr_ocr.c's busy callback, game_ocr/main.c), and none with
 * ocr_boost_clock = off. Logged for the first such job of each activation of the
 * game (a start or a resume) and whenever it changes, so a session has a line or
 * two (docs/release-variants.md). */
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
    /* API 43: hook_trust as VJO_GAME_OCR_S_TRUST (bit 20); the self-test's stages pick OCR, which ignores it */
    return (uint32_t)job_cfg.ocr_helpers | (uint32_t)job_cfg.ocr_boost_clock << 2 | (uint32_t)job_cfg.hook_trust << 20;
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

/* API 31: log what the worker installed as its game started (counts, no text), once per session and when it
 * changes (the normal build only where it hooked or refused something), and in the debug build each job's
 * source, flags and score. No hook text in the normal build (the debug build's text lines show the text
 * shown, a hook's included). */
static void hook_note(const VjoGameOcrResult *r, uint32_t pick)
{
    static const char hook_format[] VJO_DATA_TABLE(hook_format) = "hooks in %s: text source %s%s, %u table sites and %u string copies hooked, %u sites refused";
    static const char *const modes[] = {"ocr", "auto", "hooks"};
    if ((VJO_DEBUG || r->hook_installed || r->hook_refused) &&
        (hook_logged[0] != r->hook_mode + 1u || hook_logged[1] != r->hook_installed || hook_logged[2] != r->hook_refused)) {
        hook_logged[0] = r->hook_mode + 1u; /* 0: none logged in this session */
        hook_logged[1] = r->hook_installed;
        hook_logged[2] = r->hook_refused;
        /* hook_mode: the text source word the worker applied (its mode, and NO_SITES: hook_sites = off) */
        vjo_log(hook_format,
                job_title[0] ? job_title : "this game",
                (r->hook_mode & VJO_TEXT_SOURCE_MODE_MASK) < 3 ? modes[r->hook_mode & VJO_TEXT_SOURCE_MODE_MASK] : "?",
                (r->hook_mode & VJO_TEXT_SOURCE_NO_SITES) ? ", sites off" : "",
                (unsigned)(r->hook_installed >> 16), (unsigned)(r->hook_installed & 0xFFFFu), (unsigned)r->hook_refused);
    }
#if VJO_DEBUG
    if (r->hook_installed || pick != VJO_HOOK_PICK_AUTO)
        vjo_log("game vocr source: %s %u, flags 0x%02X, score %u, streams 0x%05X, pick %u", r->text_from ? "hook" : "ocr",
                (unsigned)r->text_from, (unsigned)r->hook_flags, (unsigned)r->hook_score, (unsigned)r->hook_streams,
                (unsigned)pick);
#else
    (void)pick;
#endif
}

/* View lock held. Snapshot only when this job's text is accepted, before its result is reused. */
static void line_debug_accept(const VjoGameOcrResult *r, const char *model)
{
    g_view.line_debug = (VjoLineDebug){
        .origin = r->text_from ? VJO_LINE_HOOK : VJO_LINE_OCR,
        .stream = r->text_from, .flags = r->hook_flags, .score = r->hook_score,
        .streams = r->hook_streams, .mode = r->hook_mode,
        .installed = r->hook_installed, .refused = r->hook_refused,
        .scene = job_capture_scene, .capture_seq = job_capture_seq, .model = model
    };
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
    /* The largest tier's model names the job (the kernel's log); the worker opens each by its tier
     * (and a job of columns the vertical model by its own open, API 47). */
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
    /* API 26: the text-presence gate, in the user's jobs (the self-test's stages read as before) */
    if (job_cfg.ocr_text_gate && stage < 0) request.speed |= VJO_GAME_OCR_S_GATE;
    /* API 42: vita-vn-ocr #117's head gate and two-level head, ocr_head_gate's VJO_HEAD_* bits, in the user's jobs */
    if (stage < 0) request.speed |= (uint32_t)job_cfg.ocr_head_gate << VJO_GAME_OCR_S_HEAD_SHIFT; /* 0-3: config.c */
    /* API 36: non_japanese_filter = lines (the default) also drops a line that is only a box's border read as
     * text, in the worker (core/textnorm.h), in the user's jobs; none keeps it, as do the self-test's stages */
    if (job_cfg.non_japanese_filter && stage < 0) request.speed |= VJO_GAME_OCR_S_DROP_BORDERS;
    /* API 47: vertical text (ocr_vertical off, on or auto: the field's own values), in the user's jobs; the
     * self-test's stages read rows */
    if (stage < 0) request.speed |= (uint32_t)job_cfg.ocr_vertical << VJO_GAME_OCR_S_VERTICAL_SHIFT;
    /* API 50: the line finder's fixes (ocr_line_fix = on, the default; junk adds the frame-strip rule, all also
     * the touching-rows split), in the user's jobs (the self-test's stages read as before) */
    if (job_cfg.ocr_line_fix && stage < 0)
        request.speed |= VJO_GAME_OCR_S_LINE_FIX | (job_cfg.ocr_line_fix >= 2 ? VJO_GAME_OCR_S_LINE_JUNK : 0u) |
                         (job_cfg.ocr_line_fix == 3 ? VJO_GAME_OCR_S_LINE_VALLEY : 0u);
    /* API 46: where each character of the text is on the screen, for the overlay on the game's text */
    if (job_cfg.overlay_on_text && stage < 0) request.speed |= VJO_GAME_OCR_S_CHARS;
    /* API 52: ocr_pause (off by default) pauses the game for its frame in the user's jobs (docs/game-pause.md) */
    if (job_cfg.ocr_pause && stage < 0) request.speed |= VJO_GAME_OCR_S_PAUSE;
    /* API 31: normal reads select the text automatically; diagnostic jobs force OCR. */
    request.hook_pick = stage >= 0 || flags ? VJO_HOOK_PICK_OCR : VJO_HOOK_PICK_AUTO;
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
    if (stage < 0 && !flags && result->seq && rc != VJO_E_CANCELLED &&
        !job_cancelled && job_generation == game_generation) /* a user job's result for this session's pick */
        hook_note(result, request.hook_pick);
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
            rc, job_model(result, *ran),
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
            vjo_log("ocr rc=%d %s: %u lines, %u ms", rc, job_model(result, *ran), (unsigned)result->lines,
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
 * where it differs, what is shown and looked up. They are the first lines the
 * job holds until its result is published (net_log_hold, net_job). */
static void log_text_lines(const char *text, size_t cap)
{
    char line[184];
    const char *raw = vjo_text_raw(text, cap), *shown = text;
    net_log_hold();
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

/* The existing transport result, also read by history after job_done is
 * published. Only VjoNet replaces it, on its next OCR job: no second text copy. */
static VjoGameOcrResult vocr_result;

/* API 46: the worker's char map of the sentence of len bytes in local_text, carried after its NUL (core/charmap.h);
 * its bytes, 0 for none */
static size_t cmap_carry_result(size_t len)
{
    return vjo_cmap_carry(local_text, sizeof(local_text), len, vocr_result.text, sizeof(vocr_result.text));
}

static int run_vocr(VjoOverlayData *out)
{
    unsigned tiers = vocr_tiers(), ran = vocr_top_tier(tiers);
    sceClibMemset(&vocr_result, 0, sizeof(vocr_result));
#if VJO_DEBUG
    if (job_selftest) /* its records are its result: nothing is shown */
        return out->err.rc = run_selftest_stages(&vocr_result, tiers, &ran);
#endif
    out->err.rc = vocr_run_one(&vocr_result, 0, tiers, &ran, -1);
    /* A tier's model failed to open or verify: keep OCR working with the next
     * smaller tier for this job. Only a file that is absent, the wrong length or
     * fails its hash (io 0: rejected after reading) leaves the tier out for this
     * game; a timeout or unavailable storage does not. Every tier with the same
     * file is left out with it (vocr_same_file). Log only. With ocr_vertical = on a
     * job's file is the vertical model's, whatever the tier: no tier is left out for
     * it (auto reads rows where the vertical model cannot be read, API 47). */
    while (out->err.rc == VJO_E_OCR_MODEL && job_cfg.ocr_vertical != VJO_VERTICAL_ON && ran >= VJO_VOCR_TIER_SMALL &&
           ran <= VJO_VOCR_TIERS && !vocr_cancelled(NULL)) {
        int file_bad = !vocr_result.io_error || vocr_result.io_error == VJO_ERR_FORMAT ||
                       (uint32_t)vocr_result.io_error == 0x80010002u; /* ENOENT */
        unsigned failed = ran, same = vocr_same_file(failed), below = tiers & ((1u << (failed - 1u)) - 1u) & ~same;
        vjo_log("game vocr: %s is unusable here (io=0x%08X), using %s%s", vjo_vocr_tier_model((int)failed),
                (unsigned)vocr_result.io_error, below ? vjo_vocr_tier_model((int)vocr_top_tier(below)) : "none",
                file_bad ? " for this game" : " for this job");
        if (file_bad) {
            if (vocr_unusable_generation != job_generation) vocr_unusable = 0;
            vocr_unusable |= same;
            vocr_unusable_generation = job_generation;
        }
        if (!below) break;
        tiers = below;
        sceClibMemset(&vocr_result, 0, sizeof(vocr_result));
        out->err.rc = vocr_run_one(&vocr_result, 0, tiers, &ran, -1);
    }
    /* The self-test's job runs on the screen of the last job that read text in the region
     * (a failed job's text is empty: the worker and the exchange clear it). */
    text_generation = vocr_result.text[0] ? job_generation : 0;
    text_scene = job_capture_scene;
    text_seq = job_capture_seq;
    if (out->err.rc != VJO_OK) {
        if (out->err.rc == VJO_E_OCR_MODEL) {
            /* Vita errno ENODEV and EACCES are distinct from absent/corrupt weights. */
            if ((uint32_t)vocr_result.io_error == 0x80010013u)
                out->err.detail = MODEL_STORAGE;
            else if ((uint32_t)vocr_result.io_error == 0x8001000Du) {
                /* in the data segment, for the normal Shell's text room (API 47's vertical text) */
                static const char denied[] VJO_DATA_TABLE(worker_model_denied) =
                    "Access to the local OCR model was denied. See log.txt for details.";
                out->err.detail = denied;
            }
        }
        if (out->err.rc == VJO_E_OOM) {
            out->err.detail = game_oom_detail(&vocr_result);
            out->err.memory.free = vocr_result.free_user;
            out->err.memory.reclaim = vocr_result.release_bytes;
            out->err.memory.workspace = vocr_result.need_workspace;
            out->err.memory.models = vocr_result.need_weights;
            out->err.memory.reserve = vocr_result.reserve_bytes;
        }
        if (out->err.rc == VJO_E_SOURCE)
            out->err.detail = "The game's screen could not be read (see log.txt).";
        return out->err.rc;
    }
    /* The worker returns the text normalised (core/textnorm.h: one form of each
     * mark, ー or 一 by context); the recognizer's own text goes to the log
     * (the debug build's: the normal build logs no recognized text). */
#if VJO_DEBUG
    log_text_lines(vocr_result.text, sizeof(vocr_result.text));
#endif
    /* The model on this text's Anki cards: the tier's that ran (the vertical model's for columns). A constant string. */
    const char *read_by = job_model(&vocr_result, ran);
    if (!read_by) read_by = vjo_vocr_tier_model(VJO_VOCR_TIER_DEFAULT);
    refresh_model = read_by; /* this result's constant model name, also for history */
    if (job_refresh) {
        /* The open overlay shows local_text, its lookup and Anki card: this
         * job changes none of them. The control thread compares its text
         * with the shown one once it is done (on_refresh_done), and gives
         * the cards this model only if the text replaces the shown one. */
        out->ocr_text = vocr_result.text;
        return VJO_OK;
    }
    vjo_anki_set_ocr_model(read_by);
    VjoArena text_arena;
    vjo_arena_init(&text_arena, local_text, sizeof(local_text));
    out->ocr_text = vocr_result.text;
    out->filtered = vjo_filter_lines(&text_arena, vocr_result.text, job_cfg.non_japanese_filter);
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
    /* API 46: the char map of this sentence, if the worker wrote one, follows its NUL (core/charmap.h) */
    size_t cmap = cmap_carry_result(len);
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
    vjo_view_lock();
    line_debug_accept(&vocr_result, read_by);
    cmap_publish(cmap);
    vjo_view_unlock();
    __sync_synchronize();
    local_text_valid = 1;
    sceKernelUnlockMutex(cmd_lock, 1);
    if (!job_lookup || !len || job_cursor_byte >= len || mem_alloc() == 0)
        return VJO_OK;
    out->failed_stage = VJO_STAGE_DICT;
    out->err.dict = VJO_DICT_LOCAL;
    out->err.detail = OOM_SHELL_RESULTS;
    return out->err.rc = VJO_E_OOM;
}
#else
static int run_vocr(VjoOverlayData *out)
{
    out->err.detail = "This build has no vita-vn-ocr backend. Install a build made with VJO_WITH_VOCR.";
    return out->err.rc = VJO_E_OCR_UNAVAILABLE;
}
static int shot_waiting(void) { return 0; } /* no game worker: no screenshot */
static size_t cmap_carry_result(size_t len) { (void)len; return 0; } /* API 46: no worker, no char map */
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
    vjoNotifyShell();
    if (!job_lookup || job_cursor_byte >= sceClibStrnlen(local_text, sizeof(local_text))) return VJO_OK;
    int64_t t0 = now_us();
    vjo_dict_reads = vjo_dict_read_bytes = 0;
    plat.local_text_generation = job_text_generation;
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
    plat.local_text_generation = job_text_generation;
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
        out->err.detail = OOM_SHELL_RESULTS;
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

/* One job on the network thread, then its done line: with the time in exchanges with the game worker
 * and in the log's file appends meanwhile, every thread's, before this line (docs/first-ocr.md;
 * tools/analyse_timeline.py reads it); the rest is the Shell's own and the lookup. In the debug build
 * the job's own lines this thread holds (net_log_hold: a lookup's, an OCR job's text and lookup lines)
 * and this one are written after the result, so they are in no job's appends: `N held` counts those still
 * held when this line is logged (another thread's line, or a batch filled up, wrote earlier ones before the
 * result; 0 with no batch open, refused beside another thread's, or when this line might not fit in it). */
static void net_job(void)
{
    if (job_prepare) {
        plat.local_text_generation = job_text_generation;
        job_rc = vjo_local_prepare_at(&results[0], &plat, &job_cfg, local_text, job_cursor_byte, &prepared_end);
        __sync_synchronize();
        job_done = 1;
        vjoNotifyShell();
        return;
    }
    int idx = job_idx;
#ifdef VJO_WITH_VOCR
    VjoHistoryRecord history;
    int save_history = 0;
#endif
#if VJO_DEBUG
    int64_t t0 = now_us();
    uint32_t appends0, appends1, log_us0 = vjo_log_io(&appends0), log_us1;
    job_game_us = 0;
    if (job_cursor_only)
        net_log_hold(); /* a lookup's lines (its workspace's, its own) wait for its result */
#endif
    job_rc = job_cursor_only ? run_cursor_job(&results[idx], &cache_data[idx])
                             : run_job(&results[idx], &cache_data[idx]);
#ifdef VJO_WITH_VOCR
    /* Snapshot mutable job/game facts before publishing job_done: the control
     * thread may immediately switch game or post another job after that. The
     * network thread owns vocr_result until its next recognition, so history
     * needs no retained text, allocation, queue or additional thread. */
    if (!job_cursor_only && !job_selftest && job_cfg.ocr_mode == VJO_OCR_AUTO &&
        !job_cancelled && job_generation == game_generation && vocr_result.rc == VJO_OK) {
        SceRtcTick tick;
        uint64_t elapsed = (uint64_t)(now_us() - job_started_us);
        const uint64_t epoch = 62135596800000000ull;
        if (sceRtcGetCurrentTick(&tick) >= 0 && tick.tick > epoch + elapsed) {
            history.platform = vjo_anki_get_game(history.name, history.title_id);
            if (!history.title_id[0])
                sceClibSnprintf(history.title_id, sizeof(history.title_id), "%s", job_title);
            if (!history.name[0])
                sceClibSnprintf(history.name, sizeof(history.name), "%s", history.title_id);
            history.result = &vocr_result;
            history.model = refresh_model;
            history.time_us = tick.tick - epoch - elapsed; /* UTC at job start, not import time */
            history.monotonic_us = (uint64_t)job_started_us;
            history.elapsed_us = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
            history.scene = job_capture_scene;
            history.region_seq = job_capture_seq;
            save_history = !job_cancelled && job_generation == game_generation;
        }
    }
#endif
#if VJO_DEBUG
    /* (in the data segment: the debug Shell's last text page is nearly full, docs/memory-ledger.md) */
    static const char done_format[] VJO_DATA_TABLE(job_done_format) =
        "job %d done rc=%d in %d ms, arena peak %u, dictionary page %d/%d, game %u ms, log %u ms in %u "
        "appends, %d held";
    net_log_hold(); /* a job that held none yet (a failed recognition, no text): its done line */
    log_us1 = vjo_log_io(&appends1);
    vjo_log(done_format, idx, job_rc, (int)((now_us() - t0) / 1000), (unsigned)results[idx].peak,
            cache_data[idx].page + 1, vjo_overlay_page_count(&cache_data[idx]),
            (unsigned)(job_game_us / 1000), (unsigned)((log_us1 - log_us0) / 1000),
            (unsigned)(appends1 - appends0),
            /* (none when no batch is open, or when this line may not fit: then the held go out before the result) */
            net_held->b.buf && net_held->b.n < VJO_LOG_BATCH_LINES && net_held->b.used <= NET_LOG_BYTES - 255
                ? net_held->b.n : 0);
    job_done_us = (uint32_t)now_us();
#endif
    __sync_synchronize(); /* results are visible before job_done */
    job_done = 1;
    vjoNotifyShell();
#if VJO_DEBUG
    /* The held lines, now that the result is out: one append per file. Formatted before (the lookup's
     * line reads the arena, which the control thread may reset now); written by this thread at once. */
    vjo_log_batch_end(&net_held->b);
    net_held->b.buf = NULL;
#endif
#ifdef VJO_WITH_VOCR
    /* Display/input never wait on storage. Only this thread can overwrite the
     * OCR result, and it cannot execute the next job until append returns. */
    if (save_history) {
        int rc = vjo_history_append(&history);
        if (rc < 0) vjo_dlog("history: append failed 0x%08X", (unsigned)rc);
    }
#endif
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
#if !VJO_DEBUG
    char held_buf[NET_LOG_BYTES];
#else
    NetHeld held; /* a job's held lines (net_log_hold) */
    held.b.buf = NULL; /* closed: the batch opens in a job */
    net_held = &held;
#endif
    (void)args;
    (void)argp;
    net_stack_paint(); /* its line, if any, goes out at once: not a job's */
    while (running) {
        unsigned int bits = 0;
#if VJO_DEBUG
        sceKernelWaitEventFlag(net_evf, NET_EV_JOB | NET_EV_QUIT,
                               SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &bits, NULL);
#else
        /* The wait is timed (LOG_IDLE_US) only while the batch holds a line (its n: this thread's own
         * lines raise it; a flush by another thread's line only zeroes it, shell/log_vita.c). */
        unsigned int timeout = LOG_IDLE_US;
        sceKernelWaitEventFlag(net_evf, NET_EV_JOB | NET_EV_QUIT, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT,
                               &bits, net_log.n ? &timeout : NULL);
        net_log_idle();
#endif
        if (bits & NET_EV_QUIT)
            break;
        if (bits & NET_EV_JOB) {
#if !VJO_DEBUG
            net_log_wake(held_buf); /* the job's lines are held (an earlier job's, still held, stay) */
#endif
            net_job();
        }
    }
#if !VJO_DEBUG
    vjo_log_batch_end(&net_log); /* the worker stops: what is held goes out before its buffer does */
    net_log.buf = NULL;
#endif
    return 0;
}

/* The network thread is at work (its memory must stay). */
static int net_busy(void)
{
    __sync_synchronize();
    return job_running;
}

/* #358 (the owner: "Say why the user is waiting"): what the open overlay says, with ocr_wait_hint on, as it starts a
 * try at its text once WAIT_HINT_AFTER jobs in a row found the text changed under every read (screen_changed_streak,
 * note_screen_changed; the press's own tries, or Automatically's before it): why it still is recognizing, and what
 * helps. A region drawn with □ helps only where it can leave the moving thing out (an icon beside the text), not an
 * icon inside a line, a picture behind the text or text still typing: so the fix is said as a condition (audit F1).
 * Not an error: the tries go on, and a read shows its text. In the data segment (the normal Shell's code room). */
#define WAIT_HINT_TEXT \
    "Recognizing… Something in the region keeps moving (an animated icon? text still typing?). If a region around " \
    "just the text leaves it out, press □ to draw one."
#if VJO_DEBUG /* test/combined: the debug Shell's data pages are full with every draft (16 B over): .rodata */
static const char WAIT_HINT[] = WAIT_HINT_TEXT;
#else
static const char WAIT_HINT[] VJO_DATA_TABLE(WAIT_HINT) = WAIT_HINT_TEXT;
#endif
/* API 52: the same, and the pause (docs/game-pause.md), while ocr_pause is off in a game it works in: it reads the
 * region from one frame, which an animated icon cannot change. Never in the PSP emulator's process (game_static_fb),
 * where the kernel refuses it. */
/* test/combined: in .rodata (the code segment), not the data segment: with every draft the Shell's data
 * pages are full and its code pages have room (SceShell's pages may only shrink: AGENTS.md rule 1) */
static const char WAIT_HINT_PAUSE[] =
    WAIT_HINT_TEXT " Or turn on Pause the game to read in the Rubi app.";
/* API 52: the active game is the PSP emulator's (VJO_GAME_STATIC_FB, activate_game): no pause, so no WAIT_HINT_PAUSE */
static int game_static_fb;
/* the overlay's and the strip's status while a job runs, in the data segment too (#358: its code room back) */
static const char RECOGNIZING[] VJO_DATA_TABLE(RECOGNIZING) = "Recognizing…";

/* lookup = 0: OCR only (the subtitles' sentence). refresh: the open
 * overlay's quiet refresh (background_open), which keeps what the overlay shows:
 * no retirement, no "Recognizing…", the result arena stays, and its result
 * goes to cache_data[1] (local lookups use [0] and one arena). */
static void start_job_kind(const char *why, int lookup, int refresh)
{
    const char *status;
    if (job_running)
        return;
    job_prepare = 0;
    /* the open overlay's "Recognizing…", or why it still is (#358, WAIT_HINT; API 52: and the pause, while it is off) */
    status = cfg.ocr_wait_hint && (unsigned)screen_changed_streak >= WAIT_HINT_AFTER
                 ? cfg.ocr_pause || game_static_fb ? WAIT_HINT : WAIT_HINT_PAUSE
                 : RECOGNIZING;
    if (!refresh) {
        /* Retire the previous lookup before OCR, then recreate result arenas
         * after all capture/model/inference allocations have been freed. */
        retire_local_text(0); /* atomic request exclusion and view detachment */
        if (ov != OV_CLOSED)
            view_publish(1, NULL, status, 0);
        mem_free();
    }
    vjo_dlog("job start (%s)%s", why, refresh ? ", refresh" : lookup ? "" : ", OCR only");
    job_refresh = refresh;
    job_lookup = lookup;
    job_prepares = ov == OV_CLOSED;
    job_cursor_only = 0;
    /* An OCR job has read no text yet (run_vocr sets it with the text). A
     * refresh leaves the shown text and its lookup alone (on_refresh_done), as
     * the self-test's job does: the lookup's text generation stays, so a reopen
     * on the screen of a refresh that read the same text shows the saved word
     * (open_overlay) instead of looking it up again. */
    if (!refresh)
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
        view_publish(1, NULL, status, 0);
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
    if (history_step) { /* the step to another line goes first: its own lookup follows (history_show) */
        cursor_waiting = 0;
        return;
    }
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
    job_prepare = 0;
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

/* API 42: ocr_head_gate's VJO_HEAD_* bits (core/config.h) are the request's head bits, shifted (vocr_run_one) */
typedef char vjo_head_bits_are_the_request_bits[(VJO_HEAD_GATE << VJO_GAME_OCR_S_HEAD_SHIFT) == VJO_GAME_OCR_S_HEAD_GATE &&
    (VJO_HEAD_TWO_LEVEL << VJO_GAME_OCR_S_HEAD_SHIFT) == VJO_GAME_OCR_S_TWO_LEVEL ? 1 : -1];

static int same_pipeline(const VjoConfig *a, const VjoConfig *b)
{
    /* API 46: a text read without its char map (or with one) is read again when overlay_on_text changes */
    if (a->non_japanese_filter != b->non_japanese_filter ||
        a->japanese_only_selection != b->japanese_only_selection || a->overlay_on_text != b->overlay_on_text)
        return 0;
    if (sceClibStrcmp(a->ocr_model_dir, b->ocr_model_dir) || a->ocr_skip_blank_lines != b->ocr_skip_blank_lines ||
        a->ocr_lm != b->ocr_lm || a->ocr_cascade_ms != b->ocr_cascade_ms ||
        a->ocr_refine_lines != b->ocr_refine_lines || a->vocr_tier != b->vocr_tier ||
        a->ocr_text_gate != b->ocr_text_gate || a->ocr_head_gate != b->ocr_head_gate ||
        a->ocr_vertical != b->ocr_vertical ||
        a->hook_trust != b->hook_trust || /* API 43: which text a job shows, a hook's or OCR's */
        a->ocr_line_fix != b->ocr_line_fix)
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
        prepared_seq = 0;
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
    /* API 31: the text source, for each game that starts from now on (its worker reads it then): the mode,
     * with VJO_TEXT_SOURCE_NO_SITES when hook_sites = off (no table site, the CALL source alone: audit-hooks F3) */
    if (vjoSetTextSource((uint32_t)cfg.text_source | (cfg.hook_sites ? 0u : VJO_TEXT_SOURCE_NO_SITES)) < 0)
        vjo_log("kernel refused the text source");
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
    /* #358: a game with no region line at all (not even "full", nor region.ini's line for every game, which the Rubi
     * app's Suggest a text box OFF writes as "region = full") reads its whole screen, and is offered the box its jobs
     * suggest (on_job_done) once in this activation; one with a region, none. Another game's box goes either way. */
    __atomic_store_n(&g_view.suggest, found ? VJO_RSEL_TAKEN : 0u, __ATOMIC_RELAXED);
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
    prepared_seq = 0;
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
        if ((!job_running || job_selftest || job_prepare) && !cursor_waiting && job_cursor_seq == local_cursor_seq &&
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
    view_publish(1, NULL, RECOGNIZING, 0);
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
    prepared_seq = local_cursor_seq;
    prepare_left = 0;
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
    history_step = 0; /* an earlier line shown goes once no job reads it: control_tick */
    if (net_busy()) dict_close_pending = 1;
    else vjo_local_session_close(plat.local_session);
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
    if (g_view.notice_shown) /* the Rubi app's notice is said once (check_app) */
        g_view.notice = NULL;
    /* The next job may reuse the shown arena; an open republishes it. */
    g_view.list = NULL;
    g_view.local_cursor_mode = 0;
#if VJO_DEBUG
    g_view.ask_seq = 0; /* a lookup not shown yet goes with the close: a reopen's figure is its own */
#endif
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

/* ---- line history (docs/line-history.md) ---- */

/* history.csv's last bytes are read into the control thread's scratch, with the byte before them. */
typedef char history_window_fits_scratch[VJO_HISTORY_WINDOW + 1 <= SCRATCH_SIZE ? 1 : -1];
/* A visit's block holds its table and, always, the current text. */
typedef char history_visit_fits[sizeof(HistoryVisit) + VJO_STRIP_MAX + 8 <= VJO_BUDGET_PAF_HISTORY ? 1 : -1];

/* The visit: its block, the current text as its line 0 and this game's earlier lines from
 * history.csv (history.csv's game: Anki's, as net_job writes it). A job may run: it only reads
 * local_text, which only this thread writes. 0, or -1 with no visit. */
/* Resolve the visible game's actual PSP/Vita ID and the console's local calendar day.
 * UTC conversion for both midnights respects the console's timezone/DST rules. */

#ifdef VJO_WITH_VOCR
__attribute__((noinline)) int vjo_history_context(VjoReadingScope *scope)
{
    char name[128], tid[12];
    const char *platform = vjo_anki_get_game(name, tid);
    SceDateTime date;
    SceRtcTick local, utc;
    const uint64_t epoch = 62135596800000000ull;
    if (!tid[0] || sceRtcGetCurrentClockLocalTime(&date) < 0) return 0;
    date.hour = date.minute = date.second = date.microsecond = 0;
    if (sceRtcGetTick(&date, &local) < 0 || sceRtcConvertLocalTimeToUtc(&local, &utc) < 0 || utc.tick < epoch)
        return 0;
    scope->start_us = utc.tick - epoch;
    local.tick += 86400000000ull;
    if (sceRtcConvertLocalTimeToUtc(&local, &utc) < 0) return 0;
    scope->end_us = utc.tick - epoch;
    sceClibSnprintf(scope->uuid, sizeof(scope->uuid), "%s-%s", platform, tid);
    return 1;
}
#endif

static int history_open(void)
{
    char name[128], tid[12], uuid[24]; /* vita-PCSG00415", */
    const char *platform = vjo_anki_get_game(name, tid);
    HistoryVisit *v = (HistoryVisit *)vjo_shell_heap_alloc_for(VJO_SL_PAF_HISTORY, VJO_BUDGET_PAF_HISTORY);
    VjoArena lines;
    int rc = -1;
    if (!v)
        return -1;
    sceClibMemset(v, 0, sizeof(*v));
    vjo_arena_init(&lines, v + 1, VJO_BUDGET_PAF_HISTORY - sizeof(*v));
    sceClibSnprintf(uuid, sizeof(uuid), "%s-%s\",", platform, tid[0] ? tid : title_id); /* the column and its end */
    v->h.line[0] = vjo_arena_strndup(&lines, local_text, sceClibStrnlen(local_text, sizeof(local_text) - 1));
#ifdef VJO_WITH_VOCR
    rc = vjo_history_load(&v->h, &lines, uuid, cfg.non_japanese_filter, &scratch);
#endif
#if VJO_DEBUG
    uuid[sceClibStrnlen(uuid, sizeof(uuid)) - 2] = '\0'; /* the game's ID, without the quote and comma of the match */
#endif
    vjo_dlog("history: %d earlier lines of %s, %u B", rc, uuid, (unsigned)lines.used);
    if (v->h.n) {
        history = v;
        return 0;
    }
    history_free(v);
    return -1;
}

/* Shows line `at` of the visit (0: the current text, which ends the visit) as the overlay's text:
 * local_text becomes it with a new generation and cursor sequence, published with its pending lookup
 * as a cursor move publishes one (post_local_lookup), so the overlay, its lookups, pages and cards
 * take it as a new text and requests for the line before fail. An earlier line was read from no
 * screen shown now (capture 0): neither a reopen nor a cache claims it for one. No job runs (one would
 * read local_text). With the overlay open, the line's word at the cursor is looked up. */
static void history_show(int at)
{
    HistoryVisit *v = history, *done = NULL;
    const char *text = v->h.line[at];
    size_t len = sceClibStrnlen(text, sizeof(local_text) - 1);
    int keep;
    sceKernelLockMutex(cmd_lock, 1, NULL);
    vjo_view_lock();
    if (!v->h.at) { /* leaving the current text: its cursor and capture, for the return to it */
        v->cursor = local_cursor_byte;
        v->scene = local_text_scene;
        v->seq = local_text_capture_seq;
        v->current_debug = g_view.line_debug;
    }
    sceClibMemcpy(local_text, text, len);
    local_text[len] = '\0';
    g_view.text_generation = advance_generation(&local_text_generation);
    g_view.line_debug = at ? (VjoLineDebug){.origin = v->h.origin[at]} : v->current_debug;
    local_page = 0;
    local_cursor_byte = at ? (cfg.japanese_only_selection ? (uint32_t)vjo_utf8_japanese_cursor_step(local_text, len, 0, 0) : 0)
                           : v->cursor;
    local_text_scene = at ? 0 : v->scene;
    local_text_capture_seq = at ? 0 : v->seq;
    /* the result arena's entry is the line before's, and its screen tag no longer the text's: a cached
     * sentence is local_text, which no screen shows now (the subtitle strip's first branch reads the tag
     * without cache_ok: audit-line-history F3) */
    cache_ok = 0;
    cache_scene = cache_seq = 0;
    v->h.at = at;
    if (at) {
        g_view.history = at;
        g_view.history_before = v->h.line[at + 1]; /* NULL past the oldest: the table is zeroed */
        g_view.history_after = v->h.line[at - 1];
    } else {
        done = history_detach();
    }
    if (ov != OV_CLOSED) {
        g_view.dict_pages = 0;
        keep = pending_cmd; /* (its cursor command is not the overlay's: the lookup starts below) */
        post_local_lookup(local_text_generation);
        pending_cmd = keep;
    }
    vjo_view_unlock();
    sceKernelUnlockMutex(cmd_lock, 1);
    history_free(done);
    if (ov != OV_CLOSED) {
        start_cursor_job();
    } else { /* the return after a close (a step needs the overlay open): */
        /* it leaves no lookup in the cache (the arena's is the earlier line's), so Auto, asked by a screen
         * that settled while the overlay was open (stable_pending), would read the screen the text was
         * read from again: one that still shows the text has nothing new (as claim_cache_for would say
         * with the text's lookup there) */
        VjoState st;
        st.size = sizeof(st);
        if (vjoGetState(&st) >= 0 && shows_screen(local_text_scene, local_text_capture_seq, &st))
            stable_pending = 0;
    }
}

/* The overlay's Up from its text's top row (d = 1) or Down from an earlier line's last row (d = -1):
 * the step to this game's line before or after the one shown, which control_tick takes once no job
 * runs (one reads local_text). Up opens the visit first (history_open). Only Read text:
 * Automatically keeps lines (docs/ocr-history.md): in on_press mode Up does what it did before line
 * history, nothing; so it does with no earlier line (none of this game's in history.csv's last bytes,
 * none before the oldest a visit keeps, no Paf memory for a visit: the debug log has a line for a read, and
 * both builds one for a refused block, the oldest line none), and while a
 * step waits. A step drops the lookup or refresh of the line it leaves (asked again after the visit),
 * and nothing else: a step asked for is always taken, unless a close or a retired text drops it. */
static void history_ask(int d)
{
    if (ov == OV_CLOSED || !local_text_valid || history_step ||
        (!history && (d < 0 || cfg.ocr_mode != VJO_OCR_AUTO || history_open() < 0)) ||
        (unsigned)(history->h.at + d) > (unsigned)history->h.n)
        return;
    history_step = d;
    if (job_running && (job_cursor_only || job_refresh)) /* (a preparation is short: it ends first) */
        job_cancelled = 1;
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

/* The lookup or refresh that just ended (or the OCR job a region change
 * cancelled: on_job_done) was what the subtitle strip waited for
 * (strip_waiting): it brought the strip nothing, so the strip gets the screen's
 * sentence now, or a job of its own (subtitles_show), as after the self-test's
 * job. An OCR job started at that end brings its own result (on_job_done). */
static void strip_after_wait(void)
{
    VjoState st = {.size = sizeof(st)};
    if (!strip_waiting)
        return;
    strip_waiting = 0;
    if (subtitles && (!job_running || job_cursor_only) && vjoGetState(&st) >= 0)
        subtitles_show(&st);
}

/* The overlay was opened while the job ran: is the result for the screen
 * shown now, with its lookup? Else a new job. Returns 1 if one started. A
 * lookup (on_cursor_done) has a result for an earlier screen's text when it
 * gets here: its text no longer shows the screen, or the close dropped it. */
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
    start_job(has_lookup || job_cursor_only ? "result was for an earlier screen" : "the running job was OCR only", 1);
    return 1;
}

/* The network thread finished the job. A lookup's result is shown when it is
 * for the text and cursor shown now; the screen no longer matters (#81
 * request 4). */
static void on_cursor_done(void)
{
    job_running = job_done = job_text_ready = 0;
    int current = job_generation == game_generation &&
                local_text_valid && job_text_generation == local_text_generation &&
                same_pipeline(&job_cfg, &cfg) && ov != OV_CLOSED;
    int valid = current && !job_cancelled;
    job_cancelled = 0;
    /* Reopened during the lookup on a screen its text was not read from
     * (open_overlay's OV_OPEN_OLD_JOB; the close dropped the lookup): as after
     * any job, that opening gets the screen's own recognition, or this text's
     * word if the text shows the screen again (#159 item 1: before, nothing
     * started and the overlay said "Recognizing…" until closed). A valid lookup
     * of a text that shows the screen is shown as with the overlay open. */
    if (ov != OV_OPEN_OLD_JOB || !restart_old_job(valid)) {
        sceKernelLockMutex(cmd_lock, 1, NULL);
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
        sceKernelUnlockMutex(cmd_lock, 1);
        /* A close cancels the old result; a reopen may already be waiting for
         * another lookup of this still-current saved text. */
        if (newer || (current && cursor_waiting)) start_cursor_job();
        else cursor_waiting = 0;
    }
    /* Select + R after the close that dropped this lookup: the strip waited
     * for it (#95 item 3a: before, "Recognizing…" with no job until the next
     * settled screen). */
    strip_after_wait();
}

/* VJO_E_SCREEN_CHANGED: the screen changed under every read of a job (the
 * worker reads a region again up to VJO_VOCR_TRIES times), so nothing is
 * shown for it, no error either. The next try starts at once, from the
 * second in a row SCREEN_RETRY_MS later, doubling up to SCREEN_RETRY_MAX_MS;
 * a job that reads its screen ends the run. #353: the third in a row says so in
 * the log (both builds), with the lines its game worker had recognized and that
 * are not shown; the open overlay says why it waits from WAIT_HINT_AFTER on
 * (#358, start_job_kind). For the debug Shell's room the line is short and in the
 * data segment, its call where the streak is still in a register, and the pace's
 * zero folded into its shift (the same waits). */
#ifdef VJO_WITH_VOCR
/* test/combined: in .rodata (the code segment), not the data segment: with every draft the Shell's data
 * pages are full and its code pages have room (SceShell's pages may only shrink: AGENTS.md rule 1) */
static const char SCREEN_MOVING_LOG[] =
    "ocr: screen changed under 3 jobs in a row; %u line(s) not shown";
#endif
static void note_screen_changed(const char *what)
{
    uint32_t wait_ms = SCREEN_RETRY_MS * ((1u << screen_changed_streak) >> 1); /* 0, then doubling */
    if (wait_ms > SCREEN_RETRY_MAX_MS)
        wait_ms = SCREEN_RETRY_MAX_MS;
    if (screen_changed_streak < 8)
        screen_changed_streak++;
#ifdef VJO_WITH_VOCR
    if (screen_changed_streak == 3) /* (the network thread's result: read after job_done) */
        vjo_log(SCREEN_MOVING_LOG, (unsigned)vocr_result.recognized);
#endif
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
 * a failed one backs off the next refresh (refresh_backoff), quietly, and never
 * Auto's job after the close (#95 item 1). One the subtitle strip waited for
 * (Select + R after the close that dropped it) gives the strip the screen's
 * sentence (#95 item 3a, strip_after_wait). */
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
        strip_after_wait(); /* that job's result is the strip's too */
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
        /* It read its screen: as on_job_done, the pace starts over; a failure
         * backs off the refreshes only (Auto after the close reads at once). */
        screen_changed_streak = 0;
        backoff_note(&refresh_backoff, rc != VJO_OK);
        if (rc == VJO_OK) {
            char *filtered = vjo_filter_lines(&scratch, cache_data[1].ocr_text, job_cfg.non_japanese_filter);
            const char *text = filtered ? vjo_java_trim(filtered) : "";
            size_t len = sceClibStrnlen(text, sizeof(local_text));
            sceKernelLockMutex(cmd_lock, 1, NULL);
            vjo_view_lock();
            if (!sceClibStrcmp(text, local_text)) {
                /* API 46: this screen's char map (the same text may sit elsewhere now) */
                cmap_publish(cmap_carry_result(len));
                local_text_scene = job_capture_scene;
                local_text_capture_seq = job_capture_seq;
                if (active == 0) { /* a cached lookup of this text is this screen's too */
                    cache_scene = job_capture_scene;
                    cache_seq = job_capture_seq;
                }
                what = "same text";
            } else if (len && len < sizeof(local_text)) {
                /* API 49: a line that grew (a hook that gives its text as it types) keeps the cursor where it was */
                size_t was = sceClibStrnlen(local_text, sizeof(local_text));
                int grown = was && was < len && !sceClibMemcmp(local_text, text, was);
                sceClibMemcpy(local_text, text, len + 1);
                cmap_publish(cmap_carry_result(len));
#ifdef VJO_WITH_VOCR
                line_debug_accept(&vocr_result, refresh_model);
#endif
                advance_generation(&local_text_generation);
                local_page = 0;
                if (!grown)
                    local_cursor_byte = job_cfg.japanese_only_selection
                        ? (uint32_t)vjo_utf8_japanese_cursor_step(local_text, len, 0, 0) : 0;
                local_text_scene = job_capture_scene;
                local_text_capture_seq = job_capture_seq;
                cache_ok = 0; /* the arena's entry is the old text's */
                local_header = (VjoEntryList){.header = local_text};
                /* As post_local_lookup: the new text and its pending lookup, at once. */
#if VJO_DEBUG
                g_view.ask_seq = 0; /* a word asked for in the old text is no press of the new one's */
#endif
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
    /* Select + R after the close that dropped this refresh: the strip waited
     * for it (#95 item 3a). */
    strip_after_wait();
}

/* The self-test's job ended (selftest_job_due): it showed nothing. Done, or
 * stopped by the overlay or a new screen, when its stages run again after a
 * later job's text; an overlay opened meanwhile gets what it waits for. */
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
    /* The network thread has published completion: no file read can race
     * this close, even if the overlay was reopened while the job finished. */
    __sync_synchronize();
    if ((ov == OV_CLOSED && job_cfg.ocr_mode != VJO_OCR_AUTO) || dict_close_pending || job_cancelled)
        vjo_local_session_close(plat.local_session);
    dict_close_pending = 0;
    idle_since_us = now_us();
    if (job_prepare) {
        job_running = job_done = job_prepare = 0;
        if (job_rc || job_cancelled || job_text_generation != local_text_generation) prepare_left = 0;
        else {
            VjoWordNavigation nav = {0};
            int row;
            size_t next = vjo_word_step(&nav, local_text, sceClibStrnlen(local_text, sizeof(local_text)),
                                      job_cursor_byte, prepared_end, 1, job_cfg.japanese_only_selection, NULL, 0, &row);
            prepare_cursor = (uint32_t)next;
            if (next <= job_cursor_byte) prepare_left = 0;
        }
        job_cancelled = 0;
        if (restart_old_job(0)) return;
        if (cursor_waiting && ov != OV_CLOSED) start_cursor_job();
        strip_after_wait();
        return;
    }
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
        /* A cancelled job's result goes, the text it published too: a region,
         * game or pipeline change that cancelled it after the game's answer (the
         * exchange's last cancel check) retired the text before run_vocr published
         * the old one again (#186's audit N1). */
        if (job_cancelled && local_text_valid && job_text_generation && job_text_generation == local_text_generation)
            retire_local_text(1);
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
        strip_after_wait(); /* a region drawn during it: the strip's job for the new region (#182) */
        return;
    }
    screen_changed_streak = 0; /* this job read its screen */
    screen_retry_until = 0;
#ifdef VJO_WITH_VOCR
    /* #358: the box this job on the whole screen suggests, for a game with no region of its own (the overlay offers it
     * once an activation): the newest screen's, none when its lines hold no dialogue (a box of an earlier screen is
     * never offered for this one); the network thread is done with the result */
    vjo_rsel_publish(&g_view.suggest, vocr_result.suggest_rows);
#endif
    ocr_failed = d->failed_stage == VJO_STAGE_OCR;
    backoff_note(&ocr_backoff, ocr_failed);
    if (!ocr_failed) /* OCR works again: the refreshes need not wait for theirs either */
        backoff_note(&refresh_backoff, 0);
    if (subtitles)
        strip_show_result(d);
    if (!ocr_failed && job_lookup)
        backoff_note(&dict_backoff, d->err.rc != VJO_OK);
    /* Character moves received while initial lookup was working supersede
     * that definition; the saved OCR sentence is already immutable. So does
     * a local_dictionary_results change loaded by the overlay's opening.
     * Only a job that read the shown text has a lookup to supersede: an OCR
     * job retired it as it started (start_job_kind), so with no text valid now
     * it read none (the game refused it, say), and job_cursor_seq is still an
     * earlier job's: a move made before this job, which a close dropped, is no
     * reason to look up a text it does not have; its error is shown (#95 item
     * 3b: before, the lookup found no text, and "Recognizing…" stayed). */
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
    if (ov != OV_CLOSED && job_lookup && local_text_valid && job_cursor_seq != local_cursor_seq) {
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
        int newer = job_lookup && local_text_valid && job_cursor_seq != local_cursor_seq; /* as above */
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

/* The subtitle strip for the screen in st: when no job runs, the cached
 * result's sentence when it is this screen's, or the text the overlay shows (or
 * showed) when that is (a lookup that took the result arena forgot the cache:
 * mem_alloc); with the overlay open, the text it shows,
 * which stays whatever the screen does (#81 request 4: a new screen is the quiet
 * refresh's, whose text the strip then shows, on_refresh_done), never a job of
 * the strip's that would retire it (audit-overlay-first-ocr-b RB1); else
 * "Recognizing…" and a job (a running one brings its result: on_job_done; a
 * lookup or a refresh brings none, so its end calls this again,
 * strip_after_wait: #95 item 3a; the self-test's job's end does too). The cached
 * sentence is the shown text (run_vocr reads it into local_text), so it is the
 * strip's only while that text is valid: a game switch, a region or a pipeline
 * change retires both, before the kernel's scene number may move on (#182). */
static void subtitles_show(const VjoState *st)
{
    const VjoOverlayData *d = active >= 0 ? &cache_data[active] : NULL;
    if (!job_running && d && d->sentence && local_text_valid && shows_screen(cache_scene, cache_seq, st)) {
        vjo_strip_show(d->sentence, VJO_STRIP_SENTENCE, 0);
        return;
    }
    if (!job_running && local_text_valid && local_text_game_generation == game_generation &&
        shows_screen(local_text_scene, local_text_capture_seq, st)) {
        vjo_strip_show(local_text, VJO_STRIP_SENTENCE, 0);
        return;
    }
    if (ov != OV_CLOSED) {
        const char *text = local_text_valid ? local_text : d ? d->sentence : NULL;
        if (text && text[0])
            vjo_strip_show(text, VJO_STRIP_SENTENCE, 0);
        return;
    }
    vjo_strip_show(RECOGNIZING, VJO_STRIP_STATUS, 1);
    /* A job in progress that brings the strip nothing: a lookup, a refresh, or
     * one that was cancelled (a region, game or pipeline change; not the
     * self-test's, whose end shows the strip's sentence itself): its end does
     * (strip_after_wait). Another job brings its result (on_job_done). #186's
     * audit N2: before, Select + R during a cancelled job left "Recognizing…" with
     * no job after it, until the screen settled. */
    strip_waiting = job_running && (job_cursor_only || job_refresh || (job_cancelled && !job_selftest));
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
    case VJO_CMD_EARLIER:
    case VJO_CMD_LATER:
        history_ask(cmd == VJO_CMD_EARLIER ? 1 : -1);
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
        /* #358: no box is offered after it in this activation, a region.ini that could not be written too (no line) */
        __atomic_store_n(&g_view.suggest, VJO_RSEL_TAKEN, __ATOMIC_RELAXED);
        cache_ok = 0;
        retire_local_text(1);
        /* #358: the old region's tears say nothing of the new one's: the wait hint (the region drawn as it asks)
         * and the retry pace start over, as a new game's do (invalidate_game_work) */
        screen_changed_streak = 0;
        screen_retry_until = 0;
        close_overlay();
        /* A job in progress read the old region (a lookup or a refresh, which the
         * close dropped, or OCR): its result goes, as the text and the cache did;
         * an OCR job's would publish the old region's text and lookup again (#182). */
        if (job_running)
            job_cancelled = 1;
        if (subtitles) {
            if (!job_running) {
                start_job("region changed", want_lookup());
            } else if (!job_selftest) { /* the self-test's end shows the strip's sentence itself */
                /* The strip's job for the new region cannot start beside it: the
                 * strip waits for its end (strip_after_wait). Before, it kept the
                 * old region's sentence until a screen settled (#182 a). */
                vjo_strip_show(RECOGNIZING, VJO_STRIP_STATUS, 1);
                strip_waiting = 1;
            }
        }
        break;
    }
}

/* ---- is the new foreground process a game? ----
 * Its title ID is often not readable right when the process starts, so the
 * lookup is retried for a few seconds. Unknown after that = a game. */
#define CLASSIFY_TIMEOUT_US 3000000

static SceUID pending_pid;
static int64_t pending_since;

/* The installed Rubi app against the one the plugins came with
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
    vjo_log("Rubi app %s: installed '%s' (0x%08X, app %d), plugins' '%s' (0x%08X)",
            s == VJO_APP_OK ? "ok" : s == VJO_APP_MISMATCH ? "MISMATCH" : "unknown", installed, irc, app, package,
            prc);
}

static void activate_game(SceUID pid, const VjoGameId *id)
{
    const char *tid = id->key;
    int mode = id->mode;
    game_static_fb = mode == VJO_GAME_STATIC_FB; /* API 52: WAIT_HINT_PAUSE's game */
    if (pid != hook_pid) { /* API 31: log this game session's installation counts again. */
        hook_pid = pid;
        hook_logged[0] = 0;
    }
    if (!vjo_game_enabled(tid, id->psp, &scratch)) {
        title_id[0] = '\0';
        vjoSetGameActive(pid, VJO_GAME_NONE);
        vjo_log("%s: Rubi disabled; game input passes through", tid);
        vjo_selftest_game(tid, 0, mode, NULL);
        return;
    }
    sceClibSnprintf(title_id, sizeof(title_id), "%s", tid);
    {
        char name[128];
        vjo_title_name(pid, tid, name, sizeof(name)); /* "" for an unreadable title */
        /* recorded on Anki cards: a PSP game's disc ID and title when the Rubi app described it */
        vjo_anki_set_game(id->disc[0] ? id->disc : tid, id->name[0] ? id->name : name, id->psp);
    }
    apply_config();
    push_region();
    backoff_note(&ocr_backoff, 0);
    backoff_note(&dict_backoff, 0);
    backoff_note(&refresh_backoff, 0);
    vjoSetGameActive(pid, mode);
    {
        /* once a game start: in the data segment, for the normal Shell's code room (#358) */
        static const char started[] VJO_DATA_TABLE(worker_game_started) =
            "game %s started: dictionary " VJO_DICT_NAME ", trigger %s, subtitles %s, ocr_mode %s";
        vjo_log(started, title_id, vjo_trigger_name(cfg.toggle_button), vjo_trigger_name(cfg.subtitle_button),
                cfg.ocr_mode == VJO_OCR_AUTO ? "auto" : "on_press");
    }
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
    /* API 31: a game's worker reads the text source as the game starts, before the Shell sees it in
     * front: the Rubi app's change reaches the kernel here (the app or a game left the front) */
    apply_config();
    close_overlay_for(st.game_pid ? CLOSE_FOREGROUND : CLOSE_GAME_LEFT);
    if (subtitles)
        set_subtitles(0);
    clear_input_block_error();
    /* Free memory unless the network thread still uses it. */
    if (!net_busy())
        mem_free();
    title_id[0] = '\0';
    vjo_anki_set_game("", "", 0);
#ifdef VJO_WITH_VOCR
    vjo_history_total_keep(); /* the reading total's file, when lines were added to it */
#endif
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
 * UNSETTLED_US anyway. API 49: a trusted hook's line (VjoState.hook_scene)
 * is read at once, past the throttle (docs/native-hooks.md). */
static void auto_prefetch(void)
{
    VjoState st = {.size = sizeof(st)};
    int unsettled_due;
    if (ov != OV_CLOSED || job_running || !game_active() || !background_wanted())
        return;
    unsettled_due = subtitles && now_us() - job_started_us >= UNSETTLED_US;
    if ((!stable_pending && !unsettled_due) || vjoGetState(&st) < 0 || (background_throttled() && !st.hook_scene))
        return;
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
 * backoff of a failed refresh (refresh_backoff) and REFRESH_IDLE_US after the
 * last lookup: the user's lookups go first. */
static void background_open(void)
{
    VjoState st = {.size = sizeof(st)};
    int64_t t = now_us();
    /* API 46: the marks on the game's text go once the screen no longer shows what they were read from; a quiet
     * refresh brings the new screen's (docs/overlay-on-text.md) */
    if (ov == OV_OPEN && cfg.overlay_on_text && g_view.cmap_text) {
        int stale = !local_text_valid || history ||
                    (vjoGetState(&st) >= 0 && !shows_screen(local_text_scene, local_text_capture_seq, &st));
        if (stale != g_view.cmap_stale) {
            vjo_view_lock();
            g_view.cmap_stale = stale;
            g_view.cmap_seq++;
            vjo_view_unlock();
        }
    }
    /* an earlier line is not this screen's: its refresh waits for the return to the current text */
    if (ov != OV_OPEN || job_running || pending_cmd != VJO_CMD_NONE || history)
        return;
    if (open_retry) {
        if (t >= screen_retry_until) {
            open_retry = 0;
            start_job("the screen changed during the last one", 1);
        }
        return;
    }
    /* API 49: a trusted hook's line replaces the text at once, past the pace (docs/native-hooks.md) */
    if (!refresh_pending || shot_waiting() || vjoGetState(&st) < 0 ||
        (!st.hook_scene && (t < refresh_after_us || t < refresh_backoff.until || t - lookup_us < REFRESH_IDLE_US)))
        return;
    refresh_pending = 0;
    /* Local text is valid only for this game's local pipeline (retired at every
     * change). ocr_mode = on_press recognizes only when the overlay opens. */
    if (local_text_valid && cfg.ocr_mode == VJO_OCR_AUTO &&
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
    job_refresh = job_lookup = job_cursor_only = job_cancelled = job_prepare = 0;
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

/* One bounded location-only lookup per idle job, at most eight positions from
 * the current selection. Existing scratch follows the displayed entry; OOM merely
 * stops preparation. The normal cursor job always wins the next worker slot. */
static void prepare_words(void)
{
    if (job_running || pending_cmd != VJO_CMD_NONE || now_us() - idle_since_us < 100000 ||
        !cache_ok || !local_text_valid || local_text_game_generation != game_generation ||
        refresh_pending || subtitles || shot_waiting() || vjo_selftest_stages() ||
        !has_result_memory() || !game_active() || cfg.ocr_mode != VJO_OCR_AUTO ||
        cfg.highlight_mode != VJO_HIGHLIGHT_WORD) return;
    if (prepared_seq != local_cursor_seq) {
        prepared_seq = local_cursor_seq;
        prepare_cursor = local_cursor_byte;
        prepare_left = 8;
    }
    if (!prepare_left || prepare_cursor >= sceClibStrnlen(local_text, sizeof(local_text))) return;
    prepare_left--;
    job_prepare = 1;
    job_cursor_only = job_refresh = job_cancelled = job_done = job_text_ready = 0;
    job_cfg = cfg;
    job_generation = game_generation;
    job_text_generation = local_text_generation;
    job_cursor_byte = prepare_cursor;
    job_running = 1;
    __sync_synchronize();
    sceKernelSetEventFlag(net_evf, NET_EV_JOB);
}

/* Kernel and local wakeups share the existing event. Handler order gives
 * input, refreshed text and foreground work priority over preparation. */
static void control_tick(uint32_t bits)
{
    int defer_prepare = bits || refresh_pending;
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
    /* line history, once no job reads local_text: a step asked for meanwhile (only with a visit);
     * else a visit that shows no earlier line (Up during a job, then a close: never shown, so the view
     * holds nothing of it) goes, so it holds no block and stops no refresh; else the current text
     * again after the overlay closed on an earlier line (history_show) */
    if (!job_running && history) {
        int d = history_step;
        history_step = 0;
        if (d) {
            history_show(history->h.at + d);
        } else if (!history->h.at) {
            history_free(history);
            history = NULL;
        } else if (ov == OV_CLOSED) {
            history_show(0);
        }
    }
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
    if (!defer_prepare) prepare_words();
    vjo_anki_reap(); /* an Anki job's thread and stack go once it is done */
    if (!game_active() && !net_busy() && has_result_memory() && ov == OV_CLOSED)
        mem_free();
#ifdef VJO_WITH_VOCR
    if (vjo_history_total_step(&scratch)) /* the reading total's count, a window a wakeup: the next at once */
        vjoNotifyShell();
#endif
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
                         VJO_EV_GAME_START | VJO_EV_GAME_EXIT | VJO_EV_SHELL,
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
#ifdef VJO_WITH_VOCR
    vjo_reading_rule = (uint32_t)cfg.reading_time; /* once: a Shell's totals are all counted with one rule */
#endif
    vjo_selftest_init(&cfg);
    if (vjo_selftest_armed())
        apply_config(); /* the self-test's forced settings */
#if VJO_DEBUG
    vjo_log("Rubi shell started (kernel API %d)", ver);
#else
    vjo_log("Rubi shell started (kernel API %d, the normal build: a short log)", ver);
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
    if (history) /* the threads are gone: the visit's block goes with the text */
        retire_local_text(0);
#ifdef VJO_WITH_VOCR
    vjo_history_total_stop(); /* and a count's file */
#endif
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
