/* Anki card mining with an offline file queue (the design of #5 over the LAN
 * AnkiConnect path). × saves the shown dictionary entry as a card in
 * ux0:data/VitaJPOverlay/anki_queue/ (anki_queue.c): a readable JSON record
 * and an optional JPEG of the game frame, written the same way with Wi-Fi
 * off, Anki closed or anki_host empty. △ sends the queued cards to
 * AnkiConnect at anki_host on the local network, one at a time, with the
 * deck, note type, fields, tags and word audio configured at that moment. A
 * card leaves the queue only once AnkiConnect returns its note ID, or finds
 * its stable vita_queue_<id> tag after a lost reply; one Anki already has is
 * kept as <id>.duplicate.json. No AnkiWeb sync and no login. Nothing runs and
 * nothing is allocated while anki_mining is off, the default.
 *
 * Memory: one job at a time: counting the queue (overlay opened), saving a
 * card (×) or sending the queue (△). A job runs on its own short-lived
 * thread (ANKI_STACK), which the control thread joins once it is done
 * (vjo_anki_reap). Saving or sending takes one bounded Paf allocation, which
 * keeps the Shell UI reserve, and frees it before the thread ends; counting
 * allocates nothing. The overlay's result memory is only read under the view
 * lock, after checking that the list is still the one shown (the control
 * thread frees result memory under that lock), and the note is copied out of
 * it before anything is written. Anki never touches the recognized text or
 * its lookup: a refused allocation, a failed write or a failed request only
 * changes the Anki status line. */
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>

#include "../core/anki.h"
#include "../include/vjo_ledger_budget.h"
#include "../core/utf.h"
#include "anki_queue.h"
#include "shell.h"

/* Text-only cards: the settings and note copies, the record and, when
 * sending, the field values, the request and the reply. With a picture
 * field, also the encoder (one 16-row band and its tables, ~65 KiB) and the
 * JPEG (VJO_ANKI_PICTURE_MAX), which a save moves down over them once
 * encoded and a send reads back from the queue. */
#define ANKI_TEXT_MEM (128 * 1024)
#define ANKI_PICTURE_MEM (384 * 1024)
/* Left after the addNote request for its exchange: the 4 KiB base64 chunk,
 * the 2 KiB reply reader, AnkiConnect's short replies and a createDeck. */
#define ANKI_SEND_ROOM (16 * 1024)
#define ANKI_STACK VJO_STACK_SHELL_ANKI /* as VjoNet: BearSSL for an https audio source */
#define PICTURE_QUALITY 75
#define SCAN_WINDOW_US 350000 /* per batch of VJO_NET_PROBE_MAX hosts */

enum { JOB_NONE = 0, JOB_COUNT, JOB_SAVE, JOB_SEND };

static SceUID box_lock = -1;
static VjoNetCancel cancel = {-1, -1, 0}; /* the job thread's requests */

/* mailbox (box_lock) */
static VjoConfig next_cfg;   /* the control thread's settings, for the next job */
static char game_name[128];  /* the running game's, for MiscInfo */
static int started;          /* vjo_anki_start succeeded; vjo_anki_stop clears it */
static int job;              /* the running job (JOB_*) */
static int next_job;         /* run by the same thread after it (behind a count or a save) */
static unsigned add_seq;     /* JOB_SAVE: the list shown and its entry */
static int add_entry;
static SceUID thread = -1;   /* the job's thread until it is joined */
static int thread_done;      /* it has returned */

/* job thread only */
static const VjoConfig *acfg; /* in the arena */
static VjoPlatform plat;
static void *mem;
static size_t mem_size, last_peak; /* last_peak: the last job's arena peak */
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

/* Under the view lock: entry's mark (VJO_ANKI_MARK_*) in list seq, and on
 * the word's other occurrences in the text. */
static void mark_locked(unsigned seq, int entry, int value)
{
    if (entry < 0 || entry >= VJO_MAX_ENTRIES)
        return;
    if (g_view.anki_marks_seq != seq) {
        sceClibMemset(g_view.anki_mark, 0, sizeof(g_view.anki_mark));
        g_view.anki_marks_seq = seq;
    }
    g_view.anki_mark[entry] = (uint8_t)value;
    if (g_view.list && entry < g_view.list->n_entries) {
        const VjoVocab *v = g_view.list->entries[entry].vocab;
        for (int i = 0; i < g_view.list->n_entries && i < VJO_MAX_ENTRIES; i++)
            if (g_view.list->entries[i].vocab == v)
                g_view.anki_mark[i] = (uint8_t)value;
    }
}

/* Status for list seq (0: for whatever is shown, as a send's) and, with
 * mark_entry >= 0, that entry's mark; dropped when another list is shown
 * by now. */
static void publish(unsigned seq, const char *msg, int kind, int mark_entry, int mark)
{
    vjo_view_lock();
    if (!seq || g_view.list_seq == seq) {
        if (mark_entry >= 0)
            mark_locked(seq, mark_entry, mark);
        sceClibSnprintf(g_view.anki_status, sizeof(g_view.anki_status), "%s", msg);
        vjo_utf8_trim_partial(g_view.anki_status);
        g_view.anki_status_kind = kind;
        g_view.anki_status_seq = seq;
        g_view.anki_version++;
    }
    vjo_view_unlock();
}

/* The queue line: pending cards (-1: the queue is unreadable) and whether
 * they are being sent (-1 keeps it). */
static void set_queue(int pending, int sending)
{
    vjo_view_lock();
    g_view.anki_pending = pending;
    if (sending >= 0)
        g_view.anki_sending = sending;
    g_view.anki_version++;
    vjo_view_unlock();
}

static int refresh_count(void)
{
    int n = vjo_queue_count();
    set_queue(n, -1);
    return n;
}

/* ---------------- memory ---------------- */

static int mem_get(size_t bytes)
{
    mem = vjo_shell_heap_alloc_for(VJO_SL_PAF_ANKI, bytes);
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
        vjo_shell_heap_free(VJO_SL_PAF_ANKI, mem, mem_size);
    }
    mem = NULL;
    mem_size = 0;
    acfg = NULL;
    vjo_arena_init(&arena, NULL, 0);
}

/* The job's arena (a picture field: the larger one) with a copy of the
 * settings at its start, so the control thread can load others meanwhile. */
static int mem_with_settings(void)
{
    VjoConfig *c;
    int picture;
    sceKernelLockMutex(box_lock, 1, NULL);
    picture = next_cfg.anki_field[VJO_ANKI_PICTURE][0] != '\0';
    sceKernelUnlockMutex(box_lock, 1);
    /* Allocated before any view lock is taken (Paf has its own lock). */
    if (mem_get(picture ? ANKI_PICTURE_MEM : ANKI_TEXT_MEM) < 0 ||
        !(c = (VjoConfig *)vjo_arena_alloc(&arena, sizeof(*c)))) {
        mem_put();
        return -1;
    }
    sceKernelLockMutex(box_lock, 1, NULL);
    *c = next_cfg;
    sceKernelUnlockMutex(box_lock, 1);
    acfg = c;
    return 0;
}

static int want_picture(void)
{
    return acfg->anki_field[VJO_ANKI_PICTURE][0] != '\0' && mem_size >= ANKI_PICTURE_MEM;
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

/* ---------------- saving a card (×) ---------------- */

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

/* Why a card has no screenshot. */
enum { PICTURE_OK = 0, PICTURE_CAPTURE_FAILED, PICTURE_TOO_LARGE };

/* The game's frame when × was pressed, into m (nothing when the capture
 * fails or its JPEG is over VJO_ANKI_PICTURE_MAX). The kernel frees its raw
 * frame after the last row is read; the encoder's band is returned to the
 * arena by moving the JPEG down over it. Returns PICTURE_*. */
static int take_screenshot(VjoAnkiMedia *m)
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
        vjo_log("anki: screenshot failed %d (%u bytes): saving without it", rc, (unsigned)jb.len);
        vjo_arena_release(&arena, mark);
        /* the JPEG outgrew the arena, or the cap; else no capture */
        return rc == VJO_OK || jb.oom ? PICTURE_TOO_LARGE : PICTURE_CAPTURE_FAILED;
    }
    /* The JPEG is the arena's last allocation, after the encoder's state. */
    vjo_arena_release(&arena, mark);
    keep = (uint8_t *)vjo_arena_alloc(&arena, jb.len);
    sceClibMemmove(keep, jb.data, jb.len);
    m->picture = keep;
    m->picture_len = jb.len;
    vjo_log("anki: screenshot %ux%u -> %u bytes in %d ms", st.width, st.height, (unsigned)jb.len,
            (int)((now_us() - t0) / 1000));
    return PICTURE_OK;
}

static void do_save(unsigned seq, int entry)
{
    static const char *const pic_why[] = {
        [PICTURE_CAPTURE_FAILED] = "the capture failed",
        [PICTURE_TOO_LARGE] = "the screenshot was too large",
    };
    VjoAnkiNote note;
    VjoAnkiMedia media;
    char misc[sizeof(game_name) + 24], msg[sizeof(g_view.anki_status)];
    int shown, mark = 0, ok = 0, rc, picture = PICTURE_OK, wanted;
    int64_t t0 = now_us();

    /* The √ or the queued mark already says it: no memory for that. */
    vjo_view_lock();
    shown = g_view.open && g_view.list_seq == seq && g_view.list && entry >= 0 && entry < g_view.list->n_entries;
    if (shown && entry < VJO_MAX_ENTRIES && g_view.anki_marks_seq == seq)
        mark = g_view.anki_mark[entry];
    vjo_view_unlock();
    if (!shown) /* another word is shown by now */
        return;
    if (mark) {
        publish(seq, mark == VJO_ANKI_MARK_ADDED ? "Already in Anki" : "Already queued · △ sends it",
                VJO_ANKI_STATUS_DIM, -1, 0);
        return;
    }
    if (mem_with_settings() < 0) {
        publish(seq, "Anki: not enough memory, card not saved", VJO_ANKI_STATUS_ERROR, -1, 0);
        return;
    }
    wanted = want_picture();
    vjo_view_lock();
    shown = g_view.open && g_view.list_seq == seq && g_view.list && entry < g_view.list->n_entries;
    /* Every value, whatever the field mapping is now: the send applies its own. */
    if (shown)
        ok = vjo_anki_note_from_entry(&arena, NULL, g_view.list, entry, &note) == 0;
    vjo_view_unlock();
    if (!shown)
        return;
    if (!ok) {
        publish(seq, "Anki: not enough memory for this entry; card not saved", VJO_ANKI_STATUS_ERROR, -1, 0);
        return;
    }
    publish(seq, "Saving card…", VJO_ANKI_STATUS_DIM, -1, 0);
    misc_info(misc, sizeof(misc));
    note.misc = misc;
    sceClibMemset(&media, 0, sizeof(media));
    if (wanted && !stopping()) /* the frame of the × press */
        picture = take_screenshot(&media);
    rc = vjo_queue_save(&arena, &note, &media);
    vjo_log("anki: save %s rc=%d, %u-byte screenshot, %d ms", note.spelling, rc, (unsigned)media.picture_len,
            (int)((now_us() - t0) / 1000));
    if (rc == VJO_QUEUE_TOO_LARGE) {
        sceClibSnprintf(msg, sizeof(msg), "Card not saved: over %u KiB of text", VJO_QUEUE_TEXT_MAX >> 10);
        publish(seq, msg, VJO_ANKI_STATUS_ERROR, -1, 0);
        return;
    }
    if (rc < 0) {
        publish(seq, "Card not saved: cannot write ux0:data/VitaJPOverlay/anki_queue (space?)",
                VJO_ANKI_STATUS_ERROR, -1, 0);
        refresh_count();
        return;
    }
    refresh_count();
    if (rc == VJO_QUEUE_ARCHIVED) {
        publish(seq, "Already in Anki (this card was sent before)", VJO_ANKI_STATUS_DIM, entry, VJO_ANKI_MARK_ADDED);
        return;
    }
    if (wanted && !media.picture_len && picture != PICTURE_OK)
        sceClibSnprintf(msg, sizeof(msg), "%s without a screenshot (%s)",
                        rc == VJO_QUEUE_EXISTS ? "Already queued" : "Saved for Anki", pic_why[picture]);
    else
        sceClibSnprintf(msg, sizeof(msg), "%s", rc == VJO_QUEUE_EXISTS ? "Already queued" : "Saved for Anki");
    publish(seq, msg, VJO_ANKI_STATUS_DIM, entry, VJO_ANKI_MARK_QUEUED);
}

/* ---------------- sending the queue (△) ---------------- */

/* The word's best recording from anki_audio_url, for Anki to download. */
static void find_audio(const VjoAnkiNote *note, VjoAnkiMedia *media, VjoErr *audio_err)
{
    int64_t ta = now_us();
    vjo_anki_find_audio(&arena, &plat, acfg, note, unix_ms(), media, audio_err);
    vjo_log("anki: audio %s: %s (rc=%d) in %d ms", note->spelling,
            media->audio_url ? media->audio_url : vjo_anki_audio_err_text(&arena, audio_err), audio_err->rc,
            (int)((now_us() - ta) / 1000));
}

/* Can the addNote request be built here, leaving ANKI_SEND_ROOM for its
 * exchange? A dry run: vjo_anki_add builds the same request at the same
 * place, so nothing is sent for a card that cannot be. */
static int request_fits(const VjoAnkiNote *note, const VjoAnkiMedia *media)
{
    size_t mark = vjo_arena_mark(&arena);
    VjoAnkiBody body;
    int fits = vjo_anki_add_request(&arena, acfg, note, media, &body) == 0 &&
               vjo_arena_free(&arena) >= ANKI_SEND_ROOM;
    vjo_arena_release(&arena, mark);
    return fits;
}

typedef struct {
    const VjoAnkiNote *note;
    VjoAnkiMedia *media;
    VjoErr audio_err;
    int audio_error, no_audio;
} SendOp;

static int op_send(const char *host, int port, void *ud, VjoErr *err)
{
    SendOp *o = (SendOp *)ud;
    int found;
    o->audio_error = o->no_audio = 0;
    /* A lost addNote reply, or a stop before the card was removed here:
     * Anki has it already. */
    if (vjo_anki_find_queued(&arena, &plat, host, port, o->note->queue_tag, &found, err))
        return err->rc;
    if (found)
        return VJO_OK;
    if (vjo_anki_audio_enabled(acfg) && !o->media->audio_url) {
        find_audio(o->note, o->media, &o->audio_err);
        if (o->audio_err.rc == VJO_E_CANCELLED)
            return err->rc = VJO_E_CANCELLED;
        if (o->audio_err.rc != VJO_OK && o->audio_err.rc != VJO_E_OOM) {
            /* Keep the card while the audio source is down. Anki has just
             * answered findNotes, so this must not look like a stale host to
             * with_anki (no network search); audio_err keeps the cause. */
            o->audio_error = 1;
            return err->rc = VJO_E_ANKI;
        }
        o->no_audio = !o->media->audio_url; /* no recording, or no memory for it */
    }
    return vjo_anki_add(&arena, &plat, host, port, acfg, o->note, o->media, err);
}

static void do_send(void)
{
    char id[VJO_QUEUE_ID_SIZE], after[VJO_QUEUE_ID_SIZE] = "", tag[16 + VJO_QUEUE_ID_SIZE];
    char msg[sizeof(g_view.anki_status)];
    int sent = 0, duplicates = 0, unreadable = 0, too_large = 0, no_picture = 0, no_audio = 0;
    int pending, queued, rc, done = 0, has_host, kind;
    size_t base;
    int64_t t0 = now_us();

    pending = queued = refresh_count();
    sceKernelLockMutex(box_lock, 1, NULL);
    has_host = next_cfg.anki_host[0] != '\0';
    sceKernelUnlockMutex(box_lock, 1);
    if (pending < 0) {
        publish(0, "Cannot read ux0:data/VitaJPOverlay/anki_queue; cards kept", VJO_ANKI_STATUS_ERROR, -1, 0);
        return;
    }
    if (!pending) {
        publish(0, "No cards queued: × saves the word shown", VJO_ANKI_STATUS_DIM, -1, 0);
        return;
    }
    if (!has_host) {
        publish(0, "Set anki_host in config.ini to send; cards stay queued", VJO_ANKI_STATUS_ERROR, -1, 0);
        return;
    }
    if (mem_with_settings() < 0) {
        publish(0, "Anki: not enough memory to send; cards kept", VJO_ANKI_STATUS_ERROR, -1, 0);
        return;
    }
    set_queue(pending, 1);
    base = vjo_arena_mark(&arena);
    while (!stopping()) {
        VjoAnkiNote note;
        VjoAnkiMedia media;
        SendOp op;
        VjoErr err;
        int with_picture = want_picture();
        vjo_arena_release(&arena, base);
        rc = vjo_queue_next(after, id); /* the one directory scan per card */
        if (rc < 0) {
            sceClibSnprintf(msg, sizeof(msg), "%d sent; cannot read the queue, cards kept", sent);
            publish(0, msg, VJO_ANKI_STATUS_ERROR, -1, 0);
            break;
        }
        set_queue(queued = rc, -1);
        if (!id[0]) {
            done = 1;
            break;
        }
        sceClibSnprintf(msg, sizeof(msg), "Sending to Anki… %d sent, %d left", sent, rc - unreadable - too_large);
        publish(0, msg, VJO_ANKI_STATUS_DIM, -1, 0);
        sceClibMemcpy(after, id, sizeof(after)); /* the next scan starts after it */
        sceClibSnprintf(tag, sizeof(tag), "vita_queue_%s", id);
        if (vjo_queue_load(&arena, id, with_picture, &note, &media) < 0) {
            unreadable++; /* kept: damaged, edited, or its screenshot is missing */
            continue;
        }
        note.queue_tag = tag;
        if (!request_fits(&note, &media) && media.picture_len) {
            /* This card's text and screenshot do not both fit: it goes
             * without the screenshot (read again without it). */
            vjo_log("anki: no room for %s with its %u-byte screenshot", id, (unsigned)media.picture_len);
            vjo_arena_release(&arena, base);
            if (vjo_queue_load(&arena, id, 0, &note, &media) < 0) {
                unreadable++;
                continue;
            }
            note.queue_tag = tag;
            no_picture++;
        }
        if (!request_fits(&note, &media)) {
            vjo_log("anki: %s is too large to send in %u bytes; kept", id, (unsigned)mem_size);
            too_large++;
            continue;
        }
        sceClibMemset(&op, 0, sizeof(op));
        op.note = &note;
        op.media = &media;
        rc = with_anki(1, op_send, &op, &err);
        vjo_log("anki: send %s %s rc=%d %s", id, note.spelling, rc, err.detail ? err.detail : "");
        if (rc == VJO_E_CANCELLED || stopping())
            break;
        if (rc == VJO_E_ANKI_DUPLICATE) {
            if (vjo_queue_archive_duplicate(id) < 0)
                unreadable++; /* kept as it is, see log.txt */
            else
                duplicates++;
            continue;
        }
        if (rc != VJO_OK) {
            size_t mark = vjo_arena_mark(&arena);
            const char *why = op.audio_error ? vjo_anki_audio_err_text(&arena, &op.audio_err)
                                             : vjo_anki_err_text(&arena, &err);
            int offline = !op.audio_error && (err.rc == VJO_E_NET || err.rc == VJO_E_NOT_FOUND);
            sceClibSnprintf(msg, sizeof(msg), "%d sent; %d kept: %s%s", sent, pending - sent - duplicates,
                            op.audio_error ? "audio: " : "", why ? why : "not enough memory");
            publish(0, msg, offline ? VJO_ANKI_STATUS_DIM : VJO_ANKI_STATUS_ERROR, -1, 0);
            vjo_arena_release(&arena, mark);
            break;
        }
        if (vjo_queue_remove(id) < 0) {
            sceClibSnprintf(msg, sizeof(msg), "%d sent; Anki has one more but it stays queued here: △ again is safe",
                            sent);
            publish(0, msg, VJO_ANKI_STATUS_ERROR, -1, 0);
            break;
        }
        sent++;
        no_audio |= op.no_audio;
    }
    if (done) {
        char dup_text[32] = "", kept_text[48] = "", pic_text[40] = "";
        if (duplicates)
            sceClibSnprintf(dup_text, sizeof(dup_text), ", %d already there", duplicates);
        if (unreadable + too_large)
            sceClibSnprintf(kept_text, sizeof(kept_text), ", %d kept (%s)", unreadable + too_large,
                            unreadable ? "unreadable, see log.txt" : "too large");
        if (no_picture)
            sceClibSnprintf(pic_text, sizeof(pic_text), ", %d without screenshot", no_picture);
        sceClibSnprintf(msg, sizeof(msg), "Sent %d to Anki%s%s%s%s", sent, dup_text, kept_text, pic_text,
                        no_audio ? ", some without audio" : "");
        kind = unreadable + too_large ? VJO_ANKI_STATUS_ERROR : VJO_ANKI_STATUS_DIM;
        publish(0, msg, kind, -1, 0);
        if (!unreadable && !too_large) { /* every queued word is in Anki now */
            vjo_view_lock();
            for (int i = 0; i < VJO_MAX_ENTRIES; i++)
                if (g_view.anki_mark[i] == VJO_ANKI_MARK_QUEUED)
                    g_view.anki_mark[i] = VJO_ANKI_MARK_ADDED;
            g_view.anki_version++;
            vjo_view_unlock();
        }
    }
    /* Done: the last scan's count. Stopped: no more I/O (the Shell waits). */
    if (!done)
        queued = stopping() ? pending - sent - duplicates : refresh_count();
    vjo_log("anki: sent %d, %d duplicates, %d unreadable, %d too large, %d pending, %d ms", sent, duplicates,
            unreadable, too_large, queued, (int)((now_us() - t0) / 1000));
    set_queue(queued, 0);
}

/* ---------------- thread ---------------- */

static int job_main(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    for (;;) {
        unsigned seq;
        int what, entry;
        sceKernelLockMutex(box_lock, 1, NULL);
        what = job;
        seq = add_seq;
        entry = add_entry;
        sceKernelUnlockMutex(box_lock, 1);
        if (!stopping()) {
            if (what == JOB_SAVE)
                do_save(seq, entry);
            else if (what == JOB_SEND)
                do_send();
            else
                refresh_count();
        }
        mem_put(); /* the job's arena, before any next one */
        sceKernelLockMutex(box_lock, 1, NULL);
        if (next_job && started) {
            job = next_job; /* × or △ pressed while this one ran */
            next_job = JOB_NONE;
            sceKernelUnlockMutex(box_lock, 1);
            continue;
        }
        job = next_job = JOB_NONE; /* × and △ work again */
        thread_done = 1;
        sceKernelUnlockMutex(box_lock, 1);
        return 0;
    }
}

/* Joins a job thread that has returned (box_lock held). */
static void join_done_locked(void)
{
    if (thread >= 0 && thread_done) {
        sceKernelWaitThreadEnd(thread, NULL, NULL);
        vjo_shell_thread_delete(VJO_SL_ANKI_STACK, thread, ANKI_STACK);
        thread = -1;
        thread_done = 0;
    }
}

enum { POST_STARTED, POST_OFF, POST_BUSY, POST_FAILED };

/* Starts a job, or has the running one's thread run it next (a save or a
 * send behind a count, a send behind a save). *busy: the running job when
 * it cannot. */
static int post_job(int what, unsigned seq, int entry, int *busy)
{
    SceUID t = -1;
    int r = POST_STARTED;
    *busy = JOB_NONE;
    if (box_lock < 0)
        return POST_OFF;
    sceKernelLockMutex(box_lock, 1, NULL);
    if (!started || !next_cfg.anki_mining) {
        r = POST_OFF;
    } else if (job != JOB_NONE) {
        if (what != JOB_COUNT && !next_job && (job == JOB_COUNT || (job == JOB_SAVE && what == JOB_SEND))) {
            next_job = what;
            if (what == JOB_SAVE) {
                add_seq = seq;
                add_entry = entry;
            }
        } else {
            *busy = next_job ? next_job : job;
            r = POST_BUSY;
        }
    } else {
        join_done_locked();
        t = vjo_shell_thread_create(VJO_SL_ANKI_STACK, "VjoAnki", job_main, ANKI_STACK);
        if (t >= 0) {
            job = what;
            next_job = JOB_NONE;
            add_seq = seq;
            add_entry = entry;
            thread_done = 0;
            thread = t;
        } else {
            r = POST_FAILED;
        }
    }
    sceKernelUnlockMutex(box_lock, 1);
    if (t >= 0 && sceKernelStartThread(t, 0, NULL) < 0) {
        sceKernelLockMutex(box_lock, 1, NULL);
        vjo_shell_thread_delete(VJO_SL_ANKI_STACK, t, ANKI_STACK);
        thread = -1;
        job = next_job = JOB_NONE;
        sceKernelUnlockMutex(box_lock, 1);
        r = POST_FAILED;
    }
    return r;
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
    on = started && cfg->anki_mining;
    sceKernelUnlockMutex(box_lock, 1);
    return on;
}

void vjo_anki_post_add(unsigned list_seq, int entry)
{
    int busy, r = post_job(JOB_SAVE, list_seq, entry, &busy);
    if (r == POST_BUSY)
        publish(list_seq, busy == JOB_SEND ? "Anki: sending the queue; × saves again when it is done"
                                           : "Anki: still saving the previous card",
                VJO_ANKI_STATUS_DIM, -1, 0);
    else if (r == POST_FAILED)
        publish(list_seq, "Anki: not enough memory, card not saved", VJO_ANKI_STATUS_ERROR, -1, 0);
}

void vjo_anki_post_send(void)
{
    int busy, r = post_job(JOB_SEND, 0, 0, &busy);
    if (r == POST_BUSY)
        publish(0, busy == JOB_SEND ? "Already sending to Anki" : "Anki: busy; press △ again",
                VJO_ANKI_STATUS_DIM, -1, 0);
    else if (r == POST_FAILED)
        publish(0, "Anki: not enough memory to send; cards kept", VJO_ANKI_STATUS_ERROR, -1, 0);
}

void vjo_anki_post_count(void)
{
    int busy;
    if (post_job(JOB_COUNT, 0, 0, &busy) != POST_STARTED)
        return; /* off, or a save or send that counts when it is done */
    /* A new look at the overlay: the last send's result has been seen. */
    vjo_view_lock();
    if (!g_view.anki_sending && g_view.anki_status_seq == 0 && g_view.anki_status[0]) {
        g_view.anki_status[0] = '\0';
        g_view.anki_version++;
    }
    vjo_view_unlock();
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
    vjo_view_lock();
    g_view.anki_pending = -2; /* not counted yet */
    vjo_view_unlock();
    started = 1;
    return 0;
}

int vjo_anki_stop(void)
{
    SceUID t;
    if (box_lock < 0)
        return 0;
    sceKernelLockMutex(box_lock, 1, NULL);
    started = 0; /* × and △ are ignored from now on */
    next_job = JOB_NONE;
    t = thread;
    sceKernelUnlockMutex(box_lock, 1);
    vjo_net_cancel(&cancel); /* a request in progress ends now; later ones fail */
    if (t >= 0) {
        if (sceKernelWaitThreadEnd(t, NULL, NULL) < 0) {
            vjo_log("anki: job thread join failed; keeping its memory until stop is retried");
            return -1;
        }
        vjo_shell_thread_delete(VJO_SL_ANKI_STACK, t, ANKI_STACK);
    }
    thread = -1;
    thread_done = 0;
    job = next_job = JOB_NONE;
    mem_put();
    sceKernelDeleteMutex(box_lock);
    box_lock = -1;
    if (cancel.lock >= 0)
        sceKernelDeleteMutex(cancel.lock);
    cancel.lock = -1;
    return 0;
}
