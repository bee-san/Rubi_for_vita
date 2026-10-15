/* Vita JP Overlay SceShell plugin: state shared between the control thread
 * (worker.c), the network thread and the ScePaf overlay (overlay.cpp). */
#ifndef VJO_SHELL_H
#define VJO_SHELL_H

#include <psp2/kernel/threadmgr.h>
#include <stdint.h>

#include "../core/client.h"
#include "../core/config.h"
#include "../core/ledger.h"
#include "../core/styled.h"
#include "../include/vjo_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VJO_DATA_DIR    "ux0:data/VitaJPOverlay"
#define VJO_CONFIG_PATH VJO_DATA_DIR "/config.ini"
#define VJO_REGION_PATH VJO_DATA_DIR "/region.ini" /* per game, see core/regions.h */
#define VJO_LOG_PATH    VJO_DATA_DIR "/log.txt"
#define VJO_LOG_OLD_PATH VJO_DATA_DIR "/log.old.txt"
#define VJO_STATUS_PATH VJO_DATA_DIR "/status.txt"
#define VJO_ANKI_HOST_PATH VJO_DATA_DIR "/anki_host.txt" /* found by anki_host = auto */
#define VJO_ANKI_QUEUE_DIR VJO_DATA_DIR "/anki_queue"     /* offline cards, see anki_queue.h */
#define VJO_RCO_PATH    "ur0:data/VitaJPOverlay/vitajpoverlay.rco"

#define VJO_MAX_ENTRIES 512 /* entries the overlay navigates */

enum { VJO_ANKI_STATUS_DIM = 0, VJO_ANKI_STATUS_ERROR = 1 };
/* VjoView.anki_mark: the word is in Anki (√), or saved in the offline queue */
enum { VJO_ANKI_MARK_NONE = 0, VJO_ANKI_MARK_ADDED = 1, VJO_ANKI_MARK_QUEUED = 2 };
/* VjoView.strip_kind */
enum { VJO_STRIP_SENTENCE = 0, VJO_STRIP_STATUS = 1, VJO_STRIP_ERROR = 2 };
#define VJO_STRIP_MAX 4096 /* bytes of UTF-8 */

/* ---- view model read by the overlay (under vjo_view_lock) ---- */
typedef struct {
    int open;                 /* overlay should be visible (stored atomically) */
    unsigned version;         /* bumps whenever content changes */
    unsigned list_seq;        /* bumps with every published list */
    const VjoEntryList *list; /* NULL while recognizing / on error */
    char status[256];         /* "Recognizing…", an error, or "" */
    int status_is_error;
    int local_cursor_mode;    /* local dictionary: select UTF-8 positions for point lookups */
    int japanese_only_selection; /* restrict local navigation to Han/kana */
    int highlight_mode;       /* whole matched word or selected source character */
    uint32_t cursor_byte;     /* byte offset in the immutable trimmed header */
    uint32_t text_generation; /* changes for OCR text/foreground, not cursor moves */
    int dict_page;            /* local: dictionary page shown or requested at the cursor */
    int dict_pages;           /* local: dictionaries with an entry there; 0 = unknown */
    char warnings[VJO_CONFIG_MAX_WARNINGS][96];
    int n_warnings;
    int font_size_ja;         /* config font_size_ja */
    int font_size_en;         /* config font_size_en */
    int enter_circle;         /* ○ enters and × cancels (core/buttons.h), as read when the overlay opened */
    /* Anki (anki.c): changes bump anki_version only, so the overlay keeps
     * the header, the selection and the definition's scroll */
    int anki_enabled;         /* anki_mining is on: enter saves the shown word, △ sends the queue */
    unsigned anki_version;
    int anki_pending;         /* cards in the offline queue; -1 unreadable, -2 not counted yet */
    int anki_sending;         /* △ is sending them */
    char anki_status[128];    /* "Saved for Anki", "Sending to Anki…", an error, or "" */
    int anki_status_kind;     /* VJO_ANKI_STATUS_* */
    unsigned anki_status_seq; /* list_seq the status is for; 0 = any (a send's) */
    unsigned anki_marks_seq;  /* list_seq the marks are for */
    uint8_t anki_mark[VJO_MAX_ENTRIES]; /* VJO_ANKI_MARK_* */
    /* subtitles: a strip at the top while the overlay is closed; changes
     * bump strip_version, and those of the text or its readings their seq */
    int strip_on;             /* subtitles are on (stored atomically) */
    unsigned strip_version;
    unsigned strip_text_seq, strip_ruby_seq;
    char strip_text[VJO_STRIP_MAX]; /* "" = no strip drawn */
    int strip_kind;           /* VJO_STRIP_* */
    int strip_busy;           /* a recognition is running */
    int strip_ruby_room;      /* a sentence keeps room for readings (they can come: an API key is set) */
    VjoStripRuby strip_ruby[VJO_STRIP_RUBY_MAX]; /* the sentence's readings, once looked up */
    int strip_n_ruby;
} VjoView;

extern VjoView g_view;
void vjo_view_lock(void);
void vjo_view_unlock(void);
/* Under the view lock: the shown local result's dictionary page locators and
 * the page shown, or NULL (worker.c; anki.c saves every dictionary's entry). */
const VjoLocalPages *vjo_view_pages(int *page);

/* ---- overlay -> control thread commands ---- */
enum {
    VJO_CMD_NONE = 0,
    VJO_CMD_CLOSED,          /* overlay closed by the cancel button */
    VJO_CMD_SET_REGION,      /* region selected (rect passed to vjo_post_command) */
    VJO_CMD_CLEAR_REGION,    /* hold □ */
    VJO_CMD_CURSOR,          /* dictionary-only lookup at a recognized character */
};
void vjo_post_command(int cmd, const VjoRect *rect);
/* Call without holding the view lock. Stale/closed requests are ignored. */
void vjo_post_cursor(uint32_t byte_offset, uint32_t text_generation);
/* Another dictionary's entry at the current cursor (0 <= page < dict_pages).
 * Call without holding the view lock; returns 0 for an ignored request. */
int vjo_post_page(int page, uint32_t text_generation);

/* worker.c */
int vjo_worker_start(void);
int vjo_worker_stop(void);

/* selftest.c: the on-device self-test (docs/self-test.md). */
#include "../include/vjo_game_ocr.h"
int vjo_selftest_armed(void);                 /* the marker was found and the stages are not all done */
void vjo_selftest_init(const VjoConfig *cfg); /* control thread, once the config is loaded */
void vjo_selftest_step(const char *step);     /* "ok" (boot guard), "exit" */
/* The first game: its title, policy, mode and the user's saved region (w == 0: none). */
void vjo_selftest_game(const char *title, int enabled, int mode, const VjoRect *region);
/* The stages one job runs in turn (network thread): how many (0 when not
 * armed), each one's name and request flags, its records. */
unsigned vjo_selftest_stages(void);
const char *vjo_selftest_stage_name(unsigned i);
uint32_t vjo_selftest_stage_flags(unsigned i);
void vjo_selftest_stage_begin(unsigned i, const char *model, const char *fallback);
/* After stage i ran (before its record): 1 when the stages wait for another
 * settled screen instead, because this one had no text in the region (rc 0, no
 * line) or changed under every read; the wait is recorded, and the job ends
 * with this stage's result. Only the first stage waits, at most
 * VJO_SELFTEST_WAITS screens; after that the stages run on whatever is shown. */
#define VJO_SELFTEST_WAITS 30
int vjo_selftest_stage_wait(unsigned i, int rc, const VjoGameOcrResult *r);
void vjo_selftest_stage_end(unsigned i, int rc, const VjoGameOcrResult *r, uint32_t ms, const char *model);
void vjo_selftest_done(void); /* every stage has its record: the test is complete, the marker removed */
/* One step of a game worker's log as text (log.txt and the report). */
int vjo_gstep_format(char *buf, size_t cap, const char *who, const VjoGameOcrStep *s);

/* guard.c: boot-safety guard (core/boot_guard.h). */
/* 1 when the kernel decided this boot is a safe one: the note is written to
 * status.txt and the shell is marked inert; the caller returns NO_RESIDENT. */
int vjo_guard_safe_mode(void);
/* Reports a stage reached (VJO_BOOT_STAGE_*), or 0 for "inert": the plugin
 * unloads itself on purpose, which completes this boot for the guard.
 * Returns the kernel's answer (< 0: not recorded). */
int vjo_guard_mark(int stage);

/* strip_view.c: the strip's view model (control thread) */
/* Subtitles on/off and the settings the strip shows with (cfg loaded). */
void vjo_strip_configure(int on, const VjoConfig *cfg);
/* The strip's text, of kind VJO_STRIP_* (cut at a character boundary to
 * fit), and its busy mark. A new text has no readings. */
void vjo_strip_show(const char *text, int kind, int busy);
void vjo_strip_set_busy(int busy);
/* The readings of the strip's sentence from words, a lookup of it. */
void vjo_strip_show_readings(VjoArena *scratch, const VjoEntryList *words);

/* capture.c */
int vjo_capture_init(void);
void vjo_capture_fini(void);
/* Captures the screen (flags: VJO_CAPTURE_*) and encodes it as a JPEG into
 * out; one capture at a time (one kernel buffer). Returns
 * VJO_OK or a VJO_E_* code. When the capture itself failed, st->capture_result
 * holds the kernel's error: VJO_ERR_NO_MEMORY means its capture buffer could
 * not be allocated (VJO_E_OOM); any other VJO_E_OOM is the encoder's (Shell). */
int vjo_capture_jpeg(VjoArena *a, uint32_t flags, int quality, VjoBuf *out, VjoState *st);
/* Held by a local OCR job from submission to result (the game captures then). */
void vjo_capture_hold(void);
void vjo_capture_release(void);

/* game.c */
/* The process's title ID; 0, or < 0 when not readable (yet). */
int vjo_title_id(SceUID pid, char *tid, int size);
/* The game's name for Anki's MiscInfo ("" if unknown). */
void vjo_title_name(SceUID pid, const char *tid, char *name, int size);
/* vjoSetGameActive's mode (enum VjoGameMode) for a title ID. */
int vjo_title_game_mode(const char *tid);
/* Reloaded on game start/resume before capture or input filtering activates. */
int vjo_game_enabled(const char *tid, VjoArena *scratch);

/* anki.c: Anki card mining with an offline file queue, one job at a time */
/* The mailbox lock only: no thread and no memory until a job is posted. */
int vjo_anki_start(void);
/* Cancels a job in progress and joins its thread; -1 (keeping its memory)
 * if the join fails, so stop can be retried. */
int vjo_anki_stop(void);
/* Settings for the next job (control thread, after each config load).
 * Returns 1 when × and △ work (anki_mining is on). */
int vjo_anki_configure(const VjoConfig *cfg);
/* The running game's title ID and name for its cards ("" = none; control thread). */
void vjo_anki_set_game(const char *tid, const char *name);
/* The OCR model of the last recognition, recorded on its cards (network thread). */
void vjo_anki_set_ocr_model(const char *model);
/* Saves entry `entry` of list list_seq to the queue (overlay, × pressed).
 * Ignored while anki_mining is off. */
void vjo_anki_post_add(unsigned list_seq, int entry);
/* Sends the queue to anki_host (overlay, △ pressed). */
void vjo_anki_post_send(void);
/* Counts the queue for the overlay (control thread, overlay opened). */
void vjo_anki_post_count(void);
/* Joins the thread of a job that is done (control thread, each wakeup). */
void vjo_anki_reap(void);

/* overlay.cpp (paf main thread) */
void vjo_overlay_init(void *plugin);

/* platform_vita.c */
/* Lets another thread abandon the requests of the thread using a platform
 * set up with it: vjo_net_cancel aborts the socket in use, and every
 * connect fails until vjo_net_cancel_clear. */
typedef struct {
    SceUID lock; /* the socket in use vs an abort */
    int fd;      /* -1: none */
    volatile int cancelled;
} VjoNetCancel;
int vjo_net_cancel_init(VjoNetCancel *c);
void vjo_net_cancel(VjoNetCancel *c);
void vjo_net_cancel_clear(VjoNetCancel *c);
/* cancel: NULL, or the VjoNetCancel of the thread using p. */
void vjo_platform_vita(VjoPlatform *p, VjoNetCancel *cancel);
/* Reuses ScePaf's existing heap. Returns NULL when its bounded allocation fails. */
void *vjo_shell_heap_alloc(size_t bytes);
/* The same, for ledger item `item` (VJO_SL_PAF_*, named in the log). */
void *vjo_shell_heap_alloc_for(int item, size_t bytes);
/* Gives back what vjo_shell_heap_alloc_for(item, bytes) returned. */
void vjo_shell_heap_free(int item, void *ptr, size_t bytes);
/* The PAF heap's free size, or 0 when Paf is not ready. */
uint32_t vjo_shell_heap_free_size(void);

/* ledger.c: the memory ledger (core/ledger.h, include/vjo_ledger_budget.h) */
enum VjoShellLedgerItem {
    VJO_SL_MODULE = 0,      /* our module's pages (declared) */
    VJO_SL_WORKER_MODULE,   /* the OCR worker's pages in SceShell (declared 0: registered per game, not loaded here) */
    VJO_SL_NET_STACK,
    VJO_SL_CTL_STACK,
    VJO_SL_ANKI_STACK,
    VJO_SL_USERRW_RESULTS,  /* host tests only: the Vita build uses PAF */
    VJO_SL_PAF_PLUGIN,      /* the RCO and the Paf plugin object (estimate) */
    VJO_SL_PAF_RESULTS,
    VJO_SL_PAF_ANKI,
    VJO_SL_PAF_UI,          /* the overlay's text arena while its page is open */
    VJO_SL_COUNT
};
extern VjoLedger g_shell_ledger;
void vjo_ledger_shell_init(void);
/* The only way the Shell creates a thread: priority 0x10000100, stack bytes
 * (plus 4 KiB of TLS) charged to item. */
SceUID vjo_shell_thread_create(int item, const char *name, int (*entry)(SceSize, void *), unsigned stack);
int vjo_shell_thread_delete(int item, SceUID uid, unsigned stack);
/* A USER_RW block charged to item (host tests; the Vita build uses PAF). */
SceUID vjo_shell_memblock_alloc(int item, const char *name, size_t bytes, void **base);
void vjo_shell_memblock_free(int item, SceUID uid, size_t bytes);
/* Writes the pools, the items in use and the PAF free size to status.txt. */
void vjo_ledger_shell_log(const char *when);

/* log_vita.c */
/* Dictionary file reads and bytes since the current lookup began (worker.c's
 * lookup log; platform_vita.c counts them). */
extern uint32_t vjo_dict_reads, vjo_dict_read_bytes;
void vjo_log_configure(const VjoConfig *cfg);
void vjo_log(const char *fmt, ...);
void vjo_log_raw(const char *line, int len);
void vjo_status_reset(void);
void vjo_status_close(void);
/* A log batch: until vjo_log_batch_end, the lines this thread logs are kept in
 * buf (on its stack) and written with one open/write/close per file, the same
 * bytes in the same order as line by line (a line another thread logs
 * meanwhile writes them out first). For a burst of lines formatted from data
 * at hand only, never across a wait, I/O or a call that may log at length: a
 * crash loses the batch's unwritten lines. begin and end in one function.
 * Returns 1 when the batch is open, 0 when another one is (lines then go out
 * one by one, as always). */
#define VJO_LOG_BATCH_LINES 24
typedef struct {
    char *buf;
    int cap, used, n;
    SceUID owner;
    uint16_t len[VJO_LOG_BATCH_LINES];
} VjoLogBatch;
int vjo_log_batch_begin(VjoLogBatch *b, char *buf, int cap);
void vjo_log_batch_end(VjoLogBatch *b);

/* files.c */
char *vjo_file_read(VjoArena *a, const char *path, size_t *len);
int vjo_file_write(const char *path, const void *data, size_t len);
void vjo_config_load(VjoConfig *cfg, VjoArena *scratch);
/* title_id's region (VJO_REGION_* from core/regions.h). */
int vjo_region_load(const char *title_id, VjoRect *out, VjoArena *scratch);
/* Saves title_id's region; NULL = the full screen. Other games' lines are
 * kept. */
int vjo_region_save(const char *title_id, const VjoRect *r, VjoArena *scratch);

#ifdef __cplusplus
}
#endif

#endif
