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
    /* Anki (anki.c): changes bump anki_version only, so the overlay keeps
     * the header, the selection and the definition's scroll */
    int anki_enabled;         /* anki_mining is on: × saves the shown word, △ sends the queue */
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

/* ---- overlay -> control thread commands ---- */
enum {
    VJO_CMD_NONE = 0,
    VJO_CMD_CLOSED,          /* overlay closed by ○ */
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
 * out; serialized with the OCR job's capture (one kernel buffer). Returns
 * VJO_OK or a VJO_E_* code. When the capture itself failed, st->capture_result
 * holds the kernel's error: VJO_ERR_NO_MEMORY means its capture buffer could
 * not be allocated (VJO_E_OOM); any other VJO_E_OOM is the encoder's (Shell). */
int vjo_capture_jpeg(VjoArena *a, uint32_t flags, int quality, VjoBuf *out, VjoState *st);
/* The same single-pass capture for a raw-pixel reader: consume(ud, st) runs
 * with the capture held and reads its rows with vjoReadRaw, each once (the
 * kernel frees its buffer after the last). Returns consume's result, or the
 * capture's VJO_E_* code, with st->capture_result as for vjo_capture_jpeg. */
int vjo_capture_raw(uint32_t flags, int (*consume)(void *ud, const VjoState *st), void *ud, VjoState *st);

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
/* The running game's name for MiscInfo ("" = none; control thread). */
void vjo_anki_set_game(const char *name);
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
    VJO_SL_WORKER_MODULE,   /* the *ALL OCR worker's pages in SceShell (declared) */
    VJO_SL_NET_STACK,
    VJO_SL_CTL_STACK,
    VJO_SL_ANKI_STACK,
    VJO_SL_USERRW_RESULTS,  /* host tests only: the Vita build uses PAF */
    VJO_SL_PAF_PLUGIN,      /* the RCO and the Paf plugin object (estimate) */
    VJO_SL_PAF_RESULTS,
    VJO_SL_PAF_ANKI,
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

/* This console's IPv4 address and netmask (host byte order); -1 if none. */
int vjo_net_local_ipv4(uint32_t *ip, uint32_t *mask);
/* Connects to port of up to VJO_NET_PROBE_MAX hosts (host byte order) at
 * once; open[i] = 1 for those that accept within window_us. Returns the
 * number of hosts tried. */
#define VJO_NET_PROBE_MAX 32
int vjo_net_probe_port(const uint32_t *ips, int n, int port, int window_us, uint8_t *open);

/* log_vita.c */
void vjo_log_configure(const VjoConfig *cfg);
void vjo_log(const char *fmt, ...);
void vjo_log_raw(const char *line, int len);
void vjo_status_reset(void);
void vjo_status_close(void);

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
