/* Card mining over AnkiConnect on the local network: × sends the selected
 * dictionary entry as one note (plus a screenshot of the game frame and the
 * word's audio when their fields are set) to anki_host. No offline queue and
 * no AnkiWeb sync. Nothing runs and nothing is allocated while anki_host is
 * empty, the default.
 *
 * Memory: one card at a time. × creates the card's thread (ANKI_STACK); the
 * card's arena is one bounded Paf allocation, which keeps the Shell UI
 * reserve, taken by that thread and freed before it ends; the control thread
 * then joins it (vjo_anki_reap). The overlay's result memory is only read
 * under the view lock, after checking that the list is still the one shown
 * (the control thread frees result memory under that lock), and the note is
 * copied out of it before any request. Anki never touches the recognized
 * text or its lookup: a refused allocation or a failed request only changes
 * the Anki status line. */
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>

#include "../core/anki.h"
#include "../core/utf.h"
#include "shell.h"

/* Text-only notes: the note copy, its field values, the request and the
 * reply. With a picture field, also the encoder (one 16-row band and its
 * tables, ~65 KiB) and the JPEG, which is moved down over them once
 * encoded (VJO_ANKI_PICTURE_MAX). */
#define ANKI_TEXT_MEM (128 * 1024)
#define ANKI_PICTURE_MEM (384 * 1024)
#define ANKI_STACK 0x10000 /* as VjoNet: BearSSL for an https audio source */
#define PICTURE_QUALITY 75
#define SCAN_WINDOW_US 350000 /* per batch of VJO_NET_PROBE_MAX hosts */

static SceUID box_lock = -1;
static VjoNetCancel cancel = {-1, -1, 0}; /* the card thread's requests */

/* mailbox (box_lock) */
static VjoConfig next_cfg;   /* the control thread's settings, for the next card */
static char game_name[128];  /* the running game's, for MiscInfo */
static int started;          /* vjo_anki_start succeeded; vjo_anki_stop clears it */
static int want_add;         /* a card is being sent (× is ignored meanwhile) */
static unsigned add_seq;
static int add_entry;
static SceUID thread = -1;   /* the card's thread until it is joined */
static int thread_done;      /* it has returned */

/* card thread only */
static const VjoConfig *acfg; /* in the arena */
static VjoPlatform plat;
static void *mem;
static size_t mem_size, last_peak; /* last_peak: the last card's arena peak */
static VjoArena arena;
static char saved_host[64]; /* anki_host.txt (auto) */
static int saved_port, saved_loaded;
static int scanned; /* searched the network since boot */

static int64_t now_us(void)
{
    return (int64_t)sceKernelGetProcessTimeWide();
}

static int stopping(void)
{
    __sync_synchronize();
    return !started || cancel.cancelled;
}

/* ---------------- view ---------------- */

/* Status for list seq (and, with mark_entry >= 0, that entry's √); dropped
 * when another list is shown by now. */
static void publish(unsigned seq, const char *msg, int kind, int mark_entry)
{
    vjo_view_lock();
    if (g_view.list_seq == seq) {
        if (mark_entry >= 0 && mark_entry < VJO_MAX_ENTRIES) {
            if (g_view.anki_marks_seq != seq) {
                sceClibMemset(g_view.anki_mark, 0, sizeof(g_view.anki_mark));
                g_view.anki_marks_seq = seq;
            }
            g_view.anki_mark[mark_entry] = 1;
            /* the word's other occurrences in the text too */
            if (g_view.list && mark_entry < g_view.list->n_entries) {
                const VjoVocab *v = g_view.list->entries[mark_entry].vocab;
                for (int i = 0; i < g_view.list->n_entries && i < VJO_MAX_ENTRIES; i++)
                    if (g_view.list->entries[i].vocab == v)
                        g_view.anki_mark[i] = 1;
            }
        }
        sceClibSnprintf(g_view.anki_status, sizeof(g_view.anki_status), "%s", msg);
        g_view.anki_status_kind = kind;
        g_view.anki_status_seq = seq;
        g_view.anki_version++;
    }
    vjo_view_unlock();
}

static void publish_error(unsigned seq, const VjoErr *err)
{
    size_t mark = vjo_arena_mark(&arena);
    int offline = err->rc == VJO_E_NET || err->rc == VJO_E_NOT_FOUND;
    publish(seq, vjo_anki_err_text(&arena, err), offline ? VJO_ANKI_STATUS_DIM : VJO_ANKI_STATUS_ERROR, -1);
    vjo_arena_release(&arena, mark);
}

/* ---------------- memory ---------------- */

static int mem_get(size_t bytes)
{
    mem = vjo_shell_heap_alloc_for("Anki", bytes);
    if (!mem) {
        vjo_arena_init(&arena, NULL, 0);
        return -1;
    }
    mem_size = bytes;
    vjo_arena_init(&arena, mem, bytes);
    return 0;
}

static void mem_put(void)
{
    if (mem) {
        last_peak = arena.peak;
        vjo_log("anki: arena peak %u of %u", (unsigned)arena.peak, (unsigned)mem_size);
        vjo_shell_heap_free(mem);
    }
    mem = NULL;
    mem_size = 0;
    acfg = NULL;
    vjo_arena_init(&arena, NULL, 0);
}

/* ---------------- finding Anki ---------------- */

static void load_saved(void)
{
    size_t mark, len = 0;
    char *t;
    if (saved_loaded)
        return;
    saved_loaded = 1;
    mark = vjo_arena_mark(&arena);
    t = vjo_file_read(&arena, VJO_ANKI_HOST_PATH, &len);
    if (t) {
        while (len && (uint8_t)t[len - 1] <= ' ')
            t[--len] = '\0';
        if (vjo_anki_endpoint(t, saved_host, sizeof(saved_host), &saved_port) != VJO_ANKI_MANUAL)
            saved_host[0] = '\0';
    }
    vjo_arena_release(&arena, mark);
}

static void save_host(const char *host, int port)
{
    char line[80];
    int n = sceClibSnprintf(line, sizeof(line), "%s:%d\n", host, port);
    sceClibSnprintf(saved_host, sizeof(saved_host), "%s", host);
    saved_port = port;
    vjo_file_write(VJO_ANKI_HOST_PATH, line, (size_t)n);
}

static void ip_text(uint32_t ip, char *out, size_t cap)
{
    sceClibSnprintf(out, cap, "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)(ip >> 16) & 255,
                    (unsigned)(ip >> 8) & 255, (unsigned)ip & 255);
}

/* Searches the local /24 for AnkiConnect; the first that answers wins. */
static int scan(char *host, size_t cap, int *port)
{
    uint32_t ip, mask, cand[254];
    uint8_t open[VJO_NET_PROBE_MAX];
    int n, n_open = 0;
    int64_t t0 = now_us();
    char net[16];
    if (vjo_net_local_ipv4(&ip, &mask) < 0) {
        vjo_log("anki: scan: no IP address");
        return -1;
    }
    n = vjo_anki_scan_candidates(ip, mask, cand, 254);
    ip_text(ip, net, sizeof(net));
    for (int b = 0; b < n && !stopping(); b += VJO_NET_PROBE_MAX) {
        int k = vjo_net_probe_port(cand + b, n - b, VJO_ANKI_PORT, SCAN_WINDOW_US, open);
        for (int i = 0; i < k; i++) {
            VjoErr err;
            if (!open[i])
                continue;
            n_open++;
            ip_text(cand[b + i], host, cap);
            if (vjo_anki_probe(&arena, &plat, host, VJO_ANKI_PORT, &err) == VJO_OK) {
                *port = VJO_ANKI_PORT;
                vjo_log("anki: scan from %s -> %s in %d ms", net, host, (int)((now_us() - t0) / 1000));
                return 0;
            }
            vjo_log("anki: %s:%d is open but not AnkiConnect (%d)", host, VJO_ANKI_PORT, err.rc);
        }
    }
    vjo_log("anki: scan from %s (mask %08X, %d hosts): not found, %d open, %d ms", net, (unsigned)mask, n, n_open,
            (int)((now_us() - t0) / 1000));
    return -1;
}

/* Runs op on the endpoint: anki_host, or (auto) the saved host, then a
 * network search if that fails and may_scan (or nothing was searched since
 * boot). Returns op's result, VJO_E_NET or VJO_E_NOT_FOUND. */
typedef int (*AnkiOp)(const char *host, int port, void *ud, VjoErr *err);

static int with_anki(int may_scan, AnkiOp op, void *ud, VjoErr *err)
{
    char host[64];
    int port, mode = vjo_anki_endpoint(acfg->anki_host, host, sizeof(host), &port), rc;
    sceClibMemset(err, 0, sizeof(*err));
    if (mode == VJO_ANKI_MANUAL)
        return op(host, port, ud, err);
    if (mode != VJO_ANKI_AUTO)
        return err->rc = VJO_E_NET;
    load_saved();
    if (saved_host[0]) {
        rc = op(saved_host, saved_port, ud, err);
        if (!vjo_anki_host_stale(rc) || rc == VJO_E_CANCELLED)
            return rc;
        vjo_log("anki: saved host %s:%d failed (%d)", saved_host, saved_port, rc);
    }
    if ((scanned && !may_scan) || stopping())
        return err->rc = stopping() ? VJO_E_CANCELLED : VJO_E_NET;
    scanned = 1;
    if (scan(host, sizeof(host), &port) < 0) {
        sceClibMemset(err, 0, sizeof(*err));
        return err->rc = stopping() ? VJO_E_CANCELLED : VJO_E_NOT_FOUND;
    }
    save_host(host, port);
    return op(host, port, ud, err);
}

/* ---------------- one card ---------------- */

typedef struct {
    const VjoAnkiNote *note;
    const VjoAnkiMedia *media;
} AddOp;

static int op_add(const char *host, int port, void *ud, VjoErr *err)
{
    AddOp *o = (AddOp *)ud;
    return vjo_anki_add(&arena, &plat, host, port, acfg, o->note, o->media, err);
}

static uint64_t unix_ms(void)
{
    SceRtcTick t;
    if (sceRtcGetCurrentTick(&t) < 0)
        return 0;
    return t.tick / 1000ull - 62135596800000ull; /* microseconds since 0001-01-01 UTC */
}

/* MiscInfo: the game's name and the local time of the × press. */
static void misc_info(char *out, size_t cap)
{
    char game[sizeof(game_name)];
    SceDateTime t;
    sceKernelLockMutex(box_lock, 1, NULL);
    sceClibMemcpy(game, game_name, sizeof(game));
    sceKernelUnlockMutex(box_lock, 1);
    if (sceRtcGetCurrentClockLocalTime(&t) < 0) {
        vjo_anki_misc_info(out, cap, game, NULL); /* no date rather than a wrong one */
        return;
    }
    VjoAnkiDate when = {t.year, t.month, t.day, t.hour, t.minute};
    vjo_anki_misc_info(out, cap, game, &when);
}

/* Why a note has no screenshot. */
enum { PICTURE_OK = 0, PICTURE_CAPTURE_FAILED, PICTURE_TOO_LARGE, PICTURE_NO_ROOM };

/* The game's frame when × was pressed, into m (nothing when the capture
 * fails or its JPEG is over VJO_ANKI_PICTURE_MAX). The kernel frees its raw
 * frame after the last row is read; the encoder's band is returned to the
 * arena by moving the JPEG down over it. Returns PICTURE_*. */
static int take_screenshot(VjoAnkiMedia *m, char *name, size_t cap)
{
    VjoState st;
    VjoBuf jb;
    size_t mark = vjo_arena_mark(&arena);
    int64_t t0 = now_us();
    uint8_t *keep;
    int rc;
    vjo_buf_init(&jb, &arena);
    rc = vjo_capture_jpeg(&arena, VJO_CAPTURE_FULL, PICTURE_QUALITY, &jb, &st);
    if (rc != VJO_OK || jb.len > VJO_ANKI_PICTURE_MAX) {
        vjo_log("anki: screenshot failed %d (%u bytes): adding without it", rc, (unsigned)jb.len);
        vjo_arena_release(&arena, mark);
        return rc == VJO_OK || rc == VJO_E_OOM ? PICTURE_TOO_LARGE : PICTURE_CAPTURE_FAILED;
    }
    /* The JPEG is the arena's last allocation, after the encoder's state. */
    vjo_arena_release(&arena, mark);
    keep = (uint8_t *)vjo_arena_alloc(&arena, jb.len);
    sceClibMemmove(keep, jb.data, jb.len);
    vjo_anki_media_name(name, cap, unix_ms(), "jpg");
    m->picture = keep;
    m->picture_len = jb.len;
    m->picture_name = name;
    vjo_log("anki: screenshot %ux%u -> %u bytes in %d ms", st.width, st.height, (unsigned)jb.len,
            (int)((now_us() - t0) / 1000));
    return PICTURE_OK;
}

/* The word's best recording from anki_audio_url, for Anki to download. */
static void find_audio(const VjoAnkiNote *note, VjoAnkiMedia *media, VjoErr *audio_err)
{
    int64_t ta = now_us();
    vjo_anki_find_audio(&arena, &plat, acfg, note, unix_ms(), media, audio_err);
    vjo_log("anki: audio %s: %s (rc=%d) in %d ms", note->spelling,
            media->audio_url ? media->audio_url : vjo_anki_audio_err_text(&arena, audio_err), audio_err->rc,
            (int)((now_us() - ta) / 1000));
}

static void do_add(unsigned seq, int entry)
{
    VjoAnkiNote note;
    VjoAnkiMedia media;
    AddOp o = {&note, &media};
    VjoErr err, audio_err;
    VjoConfig *c;
    char picture_name[40], misc[sizeof(game_name) + 24], msg[sizeof(g_view.anki_status)];
    int ok = 0, shown = 0, marked = 0, rc, want_picture, want_audio, picture = PICTURE_OK;
    size_t before_picture;
    int64_t t0 = now_us();

    sceKernelLockMutex(box_lock, 1, NULL);
    want_picture = next_cfg.anki_field[VJO_ANKI_PICTURE][0] != '\0';
    sceKernelUnlockMutex(box_lock, 1);
    /* Allocated before the view lock is taken (Paf has its own lock). */
    if (mem_get(want_picture ? ANKI_PICTURE_MEM : ANKI_TEXT_MEM) < 0 ||
        !(c = (VjoConfig *)vjo_arena_alloc(&arena, sizeof(*c)))) {
        mem_put();
        publish(seq, "Anki: not enough memory, card not sent. Close the overlay and try again.",
                VJO_ANKI_STATUS_ERROR, -1);
        return;
    }
    sceKernelLockMutex(box_lock, 1, NULL);
    *c = next_cfg; /* settings for this card, whatever the control thread loads meanwhile */
    sceKernelUnlockMutex(box_lock, 1);
    acfg = c;
    want_picture = acfg->anki_field[VJO_ANKI_PICTURE][0] != '\0';

    vjo_view_lock();
    if (g_view.open && g_view.list_seq == seq && g_view.list && entry >= 0 && entry < g_view.list->n_entries) {
        shown = 1;
        marked = entry < VJO_MAX_ENTRIES && g_view.anki_marks_seq == seq && g_view.anki_mark[entry];
        ok = marked || vjo_anki_note_from_entry(&arena, acfg, g_view.list, entry, &note) == 0;
    }
    vjo_view_unlock();
    if (!shown || marked) { /* another word is shown by now, or the √ already says so */
        mem_put();
        return;
    }
    if (!ok) {
        publish(seq, "Anki: not enough memory for this entry; card not sent", VJO_ANKI_STATUS_ERROR, -1);
        mem_put();
        return;
    }
    publish(seq, "Adding to Anki…", VJO_ANKI_STATUS_DIM, -1);
    misc_info(misc, sizeof(misc));
    note.misc = misc;

    sceClibMemset(&media, 0, sizeof(media));
    sceClibMemset(&audio_err, 0, sizeof(audio_err));
    want_audio = vjo_anki_audio_enabled(acfg);
    before_picture = vjo_arena_mark(&arena);
    if (want_picture && !stopping()) /* first: the frame of the × press */
        picture = take_screenshot(&media, picture_name, sizeof(picture_name));
    if (want_audio && !stopping())
        find_audio(&note, &media, &audio_err);
    rc = stopping() ? VJO_E_CANCELLED : with_anki(1, op_add, &o, &err);
    if (rc == VJO_E_OOM && media.picture_len && !stopping()) {
        /* The note's request does not fit beside its screenshot (nothing
         * was sent): without it, and its arena space back. */
        vjo_log("anki: no room for the request with a %u-byte screenshot: sending without it",
                (unsigned)media.picture_len);
        vjo_arena_release(&arena, before_picture);
        sceClibMemset(&media, 0, sizeof(media));
        picture = PICTURE_NO_ROOM;
        if (want_audio)
            find_audio(&note, &media, &audio_err);
        rc = with_anki(1, op_add, &o, &err);
    }
    vjo_log("anki: add %s rc=%d %s in %d ms, %u-byte screenshot", note.spelling, rc,
            err.detail ? err.detail : "", (int)((now_us() - t0) / 1000), (unsigned)media.picture_len);
    if (rc == VJO_E_CANCELLED || stopping()) {
        mem_put();
        return;
    }
    if (rc != VJO_OK && rc != VJO_E_ANKI_DUPLICATE) {
        publish_error(seq, &err);
        mem_put();
        return;
    }
    if (rc == VJO_E_ANKI_DUPLICATE) {
        sceClibSnprintf(msg, sizeof(msg), "Already in Anki");
    } else {
        static const char *const pic_why[] = {
            [PICTURE_CAPTURE_FAILED] = "the capture failed",
            [PICTURE_TOO_LARGE] = "the screenshot was too large",
            [PICTURE_NO_ROOM] = "no memory for both",
        };
        int no_picture = want_picture && !media.picture_len, no_audio = want_audio && !media.audio_url;
        const char *why = no_audio ? vjo_anki_audio_err_text(&arena, &audio_err) : "";
        const char *pic = pic_why[picture == PICTURE_OK ? PICTURE_CAPTURE_FAILED : picture];
        if (no_picture && no_audio)
            sceClibSnprintf(msg, sizeof(msg), "Added to Anki without a screenshot (%s) or audio (%s)", pic, why);
        else if (no_picture)
            sceClibSnprintf(msg, sizeof(msg), "Added to Anki without a screenshot (%s)", pic);
        else if (no_audio)
            sceClibSnprintf(msg, sizeof(msg), "Added to Anki without audio (%s)", why);
        else
            sceClibSnprintf(msg, sizeof(msg), "Added to Anki");
    }
    publish(seq, msg, VJO_ANKI_STATUS_DIM, entry);
    mem_put();
}

/* ---------------- thread ---------------- */

static int card_main(SceSize args, void *argp)
{
    unsigned seq;
    int entry;
    (void)args;
    (void)argp;
    sceKernelLockMutex(box_lock, 1, NULL);
    seq = add_seq;
    entry = add_entry;
    sceKernelUnlockMutex(box_lock, 1);
    if (!stopping())
        do_add(seq, entry);
    mem_put(); /* (already freed on every path) */
    sceKernelLockMutex(box_lock, 1, NULL);
    want_add = 0; /* × works again */
    thread_done = 1;
    sceKernelUnlockMutex(box_lock, 1);
    return 0;
}

/* Joins a card thread that has returned (box_lock held). */
static void join_done_locked(void)
{
    if (thread >= 0 && thread_done) {
        sceKernelWaitThreadEnd(thread, NULL, NULL);
        sceKernelDeleteThread(thread);
        thread = -1;
        thread_done = 0;
    }
}

void vjo_anki_set_game(const char *name)
{
    if (box_lock < 0)
        return;
    sceKernelLockMutex(box_lock, 1, NULL);
    sceClibSnprintf(game_name, sizeof(game_name), "%s", name);
    vjo_utf8_trim_partial(game_name);
    sceKernelUnlockMutex(box_lock, 1);
}

int vjo_anki_configure(const VjoConfig *cfg)
{
    int on;
    if (box_lock < 0)
        return 0;
    sceKernelLockMutex(box_lock, 1, NULL);
    next_cfg = *cfg;
    on = started && cfg->anki_host[0] != '\0';
    sceKernelUnlockMutex(box_lock, 1);
    return on;
}

void vjo_anki_post_add(unsigned list_seq, int entry)
{
    SceUID t = -1;
    int busy = 0, failed = 0;
    if (box_lock < 0)
        return;
    sceKernelLockMutex(box_lock, 1, NULL);
    if (!started || !next_cfg.anki_host[0]) {
        sceKernelUnlockMutex(box_lock, 1);
        return; /* off */
    }
    if (want_add) {
        busy = 1;
    } else {
        join_done_locked();
        if (thread < 0) {
            t = sceKernelCreateThread("VjoAnki", card_main, 0x10000100, ANKI_STACK, 0, 0, NULL);
            if (t >= 0) {
                want_add = 1;
                add_seq = list_seq;
                add_entry = entry;
                thread_done = 0;
                thread = t;
            } else {
                failed = 1;
            }
        }
    }
    sceKernelUnlockMutex(box_lock, 1);
    if (t >= 0 && sceKernelStartThread(t, 0, NULL) < 0) {
        sceKernelLockMutex(box_lock, 1, NULL);
        sceKernelDeleteThread(t);
        thread = -1;
        want_add = 0;
        sceKernelUnlockMutex(box_lock, 1);
        failed = 1;
    }
    if (busy)
        publish(list_seq, "Anki: still sending the previous card", VJO_ANKI_STATUS_DIM, -1);
    else if (failed)
        publish(list_seq, "Anki: not enough memory, card not sent. Close the overlay and try again.",
                VJO_ANKI_STATUS_ERROR, -1);
}

void vjo_anki_reap(void)
{
    if (box_lock < 0)
        return;
    __sync_synchronize();
    if (!thread_done) /* (read unlocked: a stale value only delays the join) */
        return;
    sceKernelLockMutex(box_lock, 1, NULL);
    join_done_locked();
    sceKernelUnlockMutex(box_lock, 1);
}

int vjo_anki_start(void)
{
    vjo_arena_init(&arena, NULL, 0);
    if (vjo_net_cancel_init(&cancel) < 0)
        return -1;
    box_lock = sceKernelCreateMutex("VjoAnkiBox", 0, 0, NULL);
    if (box_lock < 0) {
        sceKernelDeleteMutex(cancel.lock);
        cancel.lock = -1;
        return -1;
    }
    vjo_platform_vita(&plat, &cancel); /* vjo_anki_stop aborts the socket in use */
    started = 1;
    return 0;
}

int vjo_anki_stop(void)
{
    SceUID t;
    if (box_lock < 0)
        return 0;
    sceKernelLockMutex(box_lock, 1, NULL);
    started = 0; /* × is ignored from now on */
    t = thread;
    sceKernelUnlockMutex(box_lock, 1);
    vjo_net_cancel(&cancel); /* a request in progress ends now; later ones fail */
    if (t >= 0) {
        if (sceKernelWaitThreadEnd(t, NULL, NULL) < 0) {
            vjo_log("anki: card thread join failed; keeping its memory until stop is retried");
            return -1;
        }
        sceKernelDeleteThread(t);
    }
    thread = -1;
    thread_done = want_add = 0;
    mem_put();
    sceKernelDeleteMutex(box_lock);
    box_lock = -1;
    if (cancel.lock >= 0)
        sceKernelDeleteMutex(cancel.lock);
    cancel.lock = -1;
    return 0;
}
