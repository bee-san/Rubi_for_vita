/* Anki card mining with an offline file queue (the design of #5). Enter
 * saves the shown dictionary entry as a card in
 * ux0:data/VitaJPOverlay/anki_queue/ (anki_queue.c): a readable JSON record,
 * written the same way with Wi-Fi off and no computer anywhere. The Overlay
 * app exports the queue as Lapis notes in an .apkg file for Anki
 * (settings/anki_export.c); SceShell never sends anything, and nothing here
 * opens a socket. Nothing runs and nothing is allocated while anki_mining is
 * off, the default.
 *
 * Memory: one job at a time: counting the queue (overlay opened) or saving a
 * card (the enter button). A job runs on its own short-lived thread
 * (ANKI_STACK), which the control thread joins once it is done
 * (vjo_anki_reap). Saving takes one bounded Paf allocation, which keeps the
 * Shell UI reserve, and frees it before the thread ends; counting allocates
 * nothing. The overlay's result memory is only read under the view lock,
 * after checking that the list is still the one shown (the control thread
 * frees result memory under that lock), and the note is copied out of it
 * before anything is written. Anki never touches the recognized text or its
 * lookup: a refused allocation or a failed write only changes the Anki
 * status line.
 *
 * A card's screenshot (anki_screenshot, on by default; API 23) is the game
 * worker's: a job of its one slot (take_shot) has it read the game's frame,
 * encode the JPEG in the game's memory and write it beside the card through the
 * kernel. Here it is only a request and a result in the job's arena; no pixel
 * and no JPEG byte is ever in SceShell (AGENTS.md rule 1). */
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>
#include <string.h> /* strlen: shell/libc_shim.c */

#include "../core/anki.h"
#include "../core/games.h"
#include "../core/local_dict.h"
#include "../include/vjo_ledger_budget.h"
#include "../core/utf.h"
#include "anki_queue.h"
#include "shell.h"
#ifdef VJO_WITH_VOCR
#include "game_ocr_client.h"
#endif

/* A card: the settings and note copies, the other dictionaries' entries and
 * the record (at most VJO_QUEUE_TEXT_MAX). The ledger's
 * VJO_BUDGET_PAF_ANKI; anki.c checks its arena against it at compile time. */
#define ANKI_TEXT_MEM (128 * 1024)
typedef char vjo_anki_ledger_check[ANKI_TEXT_MEM <= VJO_BUDGET_PAF_ANKI ? 1 : -1];
#define ANKI_STACK VJO_STACK_SHELL_ANKI

enum { JOB_NONE = 0, JOB_COUNT, JOB_SAVE };

static SceUID box_lock = -1;

/* mailbox (box_lock) */
static VjoConfig next_cfg;   /* the control thread's settings, for the next job */
static char game_name[128];  /* the running game's, for MiscInfo */
static char game_tid[12];    /* its title ID */
static int game_psp;         /* a PSP game (the card's platform) */
static char ocr_model[32];   /* the model of the last recognition ("" before the game's first) */
static int started;          /* vjo_anki_start succeeded; vjo_anki_stop clears it */
static int job;              /* the running job (JOB_*) */
static int next_job;         /* run by the same thread after it (a save behind a count) */
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

static int64_t now_us(void)
{
    return (int64_t)sceKernelGetProcessTimeWide();
}

static int stopping(void)
{
    __sync_synchronize();
    return !started;
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

/* Status for list seq (0: for whatever is shown) and, with mark_entry >= 0,
 * that entry's mark; dropped when another list is shown by now. */
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

/* The queue line: pending cards (-1: the queue is unreadable). */
static void set_queue(int pending)
{
    vjo_view_lock();
    g_view.anki_pending = pending;
    g_view.anki_version++;
    vjo_view_unlock();
}

static int refresh_count(void)
{
    int n = vjo_queue_count();
    set_queue(n);
    vjo_dlog("anki: queue %d card(s)", n);
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
        vjo_dlog("anki: arena peak %u of %u", (unsigned)arena.peak, (unsigned)mem_size);
        vjo_shell_heap_free(VJO_SL_PAF_ANKI, mem, mem_size);
    }
    mem = NULL;
    mem_size = 0;
    acfg = NULL;
    vjo_arena_init(&arena, NULL, 0);
}

/* The job's arena with a copy of the settings at its start, so the control
 * thread can load others meanwhile. */
static int mem_with_settings(void)
{
    VjoConfig *c;
    /* Allocated before any view lock is taken (Paf has its own lock). */
    if (mem_get(ANKI_TEXT_MEM) < 0 ||
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

/* ---------------- saving a card (enter) ---------------- */

static uint64_t unix_ms(void)
{
    SceRtcTick t;
    if (sceRtcGetCurrentTick(&t) < 0)
        return 0;
    return t.tick / 1000ull - 62135596800000ull; /* microseconds since 0001-01-01 UTC */
}

static const char *keep_str(const char *s)
{
    return *s ? vjo_arena_strndup(&arena, s, strlen(s)) : NULL;
}

/* What enter records besides the entry (the strings in the arena): MiscInfo
 * (the game's name and the local time), the game, its title ID and
 * platform, the time (UTC), the OCR engine and model. */
static void card_facts(VjoAnkiNote *n, char *misc, size_t cap)
{
    SceDateTime t = {0};
    int clock = sceRtcGetCurrentClockLocalTime(&t) >= 0;
    VjoAnkiDate d = {t.year, t.month, t.day, t.hour, t.minute};
    sceKernelLockMutex(box_lock, 1, NULL);
    n->game = keep_str(game_name);
    n->title_id = keep_str(game_tid);
    n->ocr_model = keep_str(ocr_model);
    n->platform = !n->title_id ? NULL : game_psp ? "psp" : "vita";
    sceKernelUnlockMutex(box_lock, 1);
    vjo_anki_misc_info(misc, cap, n->game, clock ? &d : NULL); /* no date rather than a wrong one */
    n->misc = misc;
    n->ocr_engine = "vocr"; /* the only engine (core/config.h); a record of an older build may say "lens" */
    /* To the minute, as MiscInfo: the same card saved again within it is the same record (its ID). */
    n->unix_time = (int64_t)(unix_ms() / 60000u) * 60;
}

/* Does the card, with what it has now, still make a record (at most
 * VJO_QUEUE_TEXT_MAX, in this arena)? A dry run of the save's encoding. */
static int fits(const VjoAnkiNote *note)
{
    size_t mark = vjo_arena_mark(&arena);
    int ok = vjo_queue_encode(&arena, note, 0) != NULL;
    vjo_arena_release(&arena, mark);
    return ok;
}

/* An entry read at mark, kept compact there: its strings (and its file's
 * title, the dictionary's name) are copied above the read's memory, then
 * moved down over it. */
static int keep_entry(const VjoVocab *v, size_t mark, VjoAnkiGloss *g)
{
    size_t n = (size_t)v->n_meanings, total = n * sizeof(char *), at = (mark + 7) & ~(size_t)7, len;
    const char *src[3] = {v->spelling, v->reading, v->dictionary ? v->dictionary : ""};
    char *top, *p, **m;
    for (size_t k = 0; k < n + 3; k++)
        total += strlen(k < 3 ? src[k] : v->meanings[k - 3]) + 1;
    if (!(top = (char *)vjo_arena_alloc(&arena, total)))
        return -1;
    m = (char **)top;
    p = top + n * sizeof(char *);
    for (size_t k = 0; k < n + 3; k++) { /* the strings, each one's offset in m[] (spelling, reading, title first) */
        const char *t = k < 3 ? src[k] : v->meanings[k - 3];
        len = strlen(t) + 1;
        sceClibMemcpy(p, t, len);
        if (k < 3)
            src[k] = (const char *)(uintptr_t)(p - top);
        else
            m[k - 3] = (char *)(uintptr_t)(p - top);
        p += len;
    }
    sceClibMemmove(arena.base + at, top, total);
    vjo_arena_release(&arena, mark);
    top = (char *)vjo_arena_alloc(&arena, total); /* the moved block: arena.base + at */
    m = (char **)top;
    for (size_t k = 0; k < n; k++)
        m[k] = top + (uintptr_t)m[k];
    g->spelling = top + (uintptr_t)src[0];
    g->reading = top + (uintptr_t)src[1];
    g->dictionary = top[(uintptr_t)src[2]] ? top + (uintptr_t)src[2] : NULL;
    g->meanings = (const char **)m;
    g->n_meanings = (int)n;
    return 0;
}

/* The other dictionaries' entries for the word (local_dictionary_results
 * above 1): each page's payload read from its file, one at a time, kept
 * while the card's record fits. */
static void read_more(VjoAnkiNote *n, const VjoLocalPages *pages, int shown)
{
    VjoAnkiGloss *g;
    char name[96];
    if (!pages || !(g = (VjoAnkiGloss *)vjo_arena_alloc(&arena, sizeof(*g) * VJO_ANKI_MAX_MORE)))
        return;
    n->more = g;
    for (int i = 0; i < pages->count && n->n_more < VJO_ANKI_MAX_MORE && !stopping(); i++) {
        size_t mark = vjo_arena_mark(&arena);
        VjoDictResult r;
        VjoErr err;
        if (i == shown)
            continue;
        if (vjo_local_read_page(&arena, &plat, acfg, pages, i, &r, &err) || !r.n_vocab || !r.vocab[0].n_meanings ||
            keep_entry(&r.vocab[0], mark, &g[n->n_more]) < 0) {
            vjo_arena_release(&arena, mark);
            continue;
        }
        /* (r is gone: keep_entry moved its entry, and its file's title, over the read) */
        if (!g[n->n_more].dictionary) {
            vjo_anki_meaning_source(g[n->n_more].meanings[0], name, sizeof(name));
            g[n->n_more].dictionary = keep_str(name);
        }
        n->n_more++;
        if (!fits(n)) {
            vjo_log("anki: %d other dictionaries' entries kept: the next would make the card too large", --n->n_more);
            vjo_arena_release(&arena, mark);
            break;
        }
    }
}

/* ---------------- the card's screenshot (API 23) ---------------- */

/* The game worker reads the game's whole frame, encodes it as a JPEG in the game's
 * memory and has the kernel write it as <id>.jpg in the queue
 * (include/vjo_game_ocr.h VJO_GAME_OCR_F_SHOT): SceShell holds no pixel and no byte
 * of it, only the request and the job's result, in this job's arena. The game
 * worker's one job slot is the network thread's OCR jobs' too: the exchange runs
 * under vjo_game_job_begin (worker.c), which has a quiet refresh in progress yield.
 * It stops when the overlay closes: the game then has its input back, and its
 * screen is no longer the card's. */
#define SHOT_WAIT_US 4000000    /* for the slot: a refresh yields within a strip, a lookup's OCR runs on */
#define SHOT_TIMEOUT_US 15000000 /* the job: 9 frames, the JPEG and its file, three tries at most */

#ifdef VJO_WITH_VOCR
static int shot_stopped(void *ud)
{
    (void)ud;
    return stopping() || !__atomic_load_n(&g_view.open, __ATOMIC_ACQUIRE);
}

static int take_shot(const char id[VJO_QUEUE_ID_SIZE])
{
    size_t mark = vjo_arena_mark(&arena);
    struct { /* 5,404 B */
        VjoGameOcrRequest req;
        VjoGameOcrResult res;
    } *job = vjo_arena_zalloc(&arena, sizeof(*job));
#if VJO_DEBUG
    int64_t t0 = now_us();
#endif
    int rc = VJO_E_OOM;
    if (job) {
        job->req.size = sizeof(job->req);
        job->req.flags = VJO_GAME_OCR_F_SHOT;
        sceClibMemcpy(job->req.shot, id, VJO_QUEUE_ID_SIZE);
        rc = VJO_E_OCR_UNAVAILABLE; /* the slot stayed busy */
        if (!vjo_game_job_begin(shot_stopped, SHOT_WAIT_US)) {
            rc = vjo_game_ocr_exchange(&vjo_game_client, &job->req, &job->res, shot_stopped, NULL, SHOT_TIMEOUT_US);
            vjo_game_job_end();
        }
#if VJO_DEBUG
        /* Every screenshot, with its figures (the normal build: do_save logs a failed one's reason only,
         * docs/release-variants.md, for the Shell's pages there), and a failed one's steps. */
        const VjoGameOcrResult *res = &job->res;
        vjo_log("anki: screenshot rc=%d %ux%u q%u try %u: %u B; block %u B mem %u, free USER %d; %u strips src %u "
                "%u ms, encode %u ms, write %u ms; %d ms", rc, res->width, res->height, res->shot_quality,
                res->shot_tries, res->shot_bytes, res->shot_block, res->shot_memory, (int)res->free_user,
                res->captures, res->source, res->capture_us / 1000, res->shot_encode_us / 1000,
                res->shot_write_us / 1000, (int)((now_us() - t0) / 1000));
        /* the game worker's steps (shell/selftest.c vjo_gstep_format, the debug build's) */
        for (uint32_t i = 0; rc && i < res->n_steps && i < VJO_GAME_OCR_STEPS; i++) {
            char line[224];
            if (vjo_gstep_format(line, sizeof(line), "shot", &res->step[i]) > 0)
                vjo_log("%s", line);
        }
#endif
    }
    vjo_arena_release(&arena, mark);
    return rc;
}
#else
#define take_shot NULL /* no game worker in this build: cards without a screenshot */
#endif

static void do_save(unsigned seq, int entry)
{
    VjoAnkiNote note;
    VjoLocalPages *pages = NULL;
    VjoQueueShot shot = {take_shot, 0};
    char misc[sizeof(game_name) + 24], msg[sizeof(g_view.anki_status)];
    int shown, mark_value = 0, ok = 0, rc, page = 0;
    int64_t t0 = now_us();

    /* The √ or the queued mark already says it: no memory for that. */
    vjo_view_lock();
    shown = g_view.open && g_view.list_seq == seq && g_view.list && entry >= 0 && entry < g_view.list->n_entries;
    if (shown && entry < VJO_MAX_ENTRIES && g_view.anki_marks_seq == seq)
        mark_value = g_view.anki_mark[entry];
    vjo_view_unlock();
    if (!shown) /* another word is shown by now */
        return;
    if (mark_value) {
        publish(seq, mark_value == VJO_ANKI_MARK_ADDED ? "Already exported (in an .apkg)" : "Already queued",
                VJO_ANKI_STATUS_DIM, -1, 0);
        return;
    }
    if (mem_with_settings() < 0) {
        publish(seq, "Anki: not enough memory, card not saved", VJO_ANKI_STATUS_ERROR, -1, 0);
        return;
    }
    vjo_view_lock();
    shown = g_view.open && g_view.list_seq == seq && g_view.list && entry < g_view.list->n_entries;
    /* Every value: the card is made into a note when it is exported. */
    if (shown) {
        const VjoLocalPages *p = vjo_view_pages(&page);
        ok = vjo_anki_note_from_entry(&arena, g_view.list, entry, &note) == 0;
        if (p && p->count > 1 && (pages = (VjoLocalPages *)vjo_arena_alloc(&arena, sizeof(*p))))
            *pages = *p; /* the other dictionaries' locators, in the result memory */
    }
    vjo_view_unlock();
    if (!shown)
        return;
    if (!ok) {
        publish(seq, "Anki: not enough memory for this entry; card not saved", VJO_ANKI_STATUS_ERROR, -1, 0);
        return;
    }
    publish(seq, "Saving card…", VJO_ANKI_STATUS_DIM, -1, 0);
    card_facts(&note, misc, sizeof(misc));
    read_more(&note, pages, page);
    /* The screenshot (anki_screenshot, on by default) of the game in front: the
     * game worker's, never SceShell's (take_shot). */
    rc = vjo_queue_save(&arena, &note, acfg->anki_screenshot && note.title_id ? &shot : NULL);
#if VJO_DEBUG
    vjo_log("anki: save %s rc=%d, %d ms", note.spelling, rc, (int)((now_us() - t0) / 1000));
#else
    vjo_log("anki: save rc=%d, %d ms", rc, (int)((now_us() - t0) / 1000)); /* the normal build logs no looked-up word */
    if (shot.rc) /* why the card has no screenshot: the status line sends the user to log.txt (take_shot's, debug) */
        vjo_log("anki: screenshot rc=%d", shot.rc);
#endif
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
        publish(seq, "Already exported (in an .apkg)", VJO_ANKI_STATUS_DIM, entry, VJO_ANKI_MARK_ADDED);
        return;
    }
    publish(seq, rc == VJO_QUEUE_EXISTS ? "Already queued"
                 : shot.rc ? "Saved for Anki, without a screenshot (log.txt)" : "Saved for Anki",
            VJO_ANKI_STATUS_DIM, entry, VJO_ANKI_MARK_QUEUED);
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
            else
                refresh_count();
        }
        mem_put(); /* the job's arena, before any next one */
        sceKernelLockMutex(box_lock, 1, NULL);
        if (next_job && started) {
            job = next_job; /* enter pressed while the count ran */
            next_job = JOB_NONE;
            sceKernelUnlockMutex(box_lock, 1);
            continue;
        }
        job = next_job = JOB_NONE; /* enter works again */
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

/* Starts a job, or has the running one's thread run it next (a save behind
 * a count). *busy: the running job when it cannot. */
static int post_job(int what, unsigned seq, int entry, int *busy)
{
    SceUID t = -1;
    int r = POST_STARTED, rc = 0;
    *busy = JOB_NONE;
    if (box_lock < 0)
        return POST_OFF;
    sceKernelLockMutex(box_lock, 1, NULL);
    if (!started || !next_cfg.anki_mining) {
        r = POST_OFF;
    } else if (job != JOB_NONE) {
        if (what == JOB_SAVE && !next_job && job == JOB_COUNT) {
            next_job = what;
            add_seq = seq;
            add_entry = entry;
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
            rc = t; /* vjo_shell_thread_create logged SceShell's refusal */
            r = POST_FAILED;
        }
    }
    sceKernelUnlockMutex(box_lock, 1);
    if (t >= 0 && (rc = sceKernelStartThread(t, 0, NULL)) < 0) {
        sceKernelLockMutex(box_lock, 1, NULL);
        vjo_shell_thread_delete(VJO_SL_ANKI_STACK, t, ANKI_STACK);
        thread = -1;
        job = next_job = JOB_NONE;
        sceKernelUnlockMutex(box_lock, 1);
        r = POST_FAILED;
    }
    if (r == POST_FAILED) /* a count runs on every overlay open with anki_mining on */
        vjo_log("anki: %s not started 0x%08X", what == JOB_COUNT ? "count" : "save", (unsigned)rc);
    return r;
}

void vjo_anki_set_game(const char *tid, const char *name, int psp)
{
    if (box_lock < 0)
        return;
    sceKernelLockMutex(box_lock, 1, NULL);
    sceClibSnprintf(game_tid, sizeof(game_tid), "%s", tid);
    game_psp = psp;
    sceClibSnprintf(game_name, sizeof(game_name), "%s", name);
    ocr_model[0] = '\0'; /* until this game's first recognition */
    vjo_utf8_trim_partial(game_name);
    sceKernelUnlockMutex(box_lock, 1);
}

void vjo_anki_set_ocr_model(const char *model)
{
    if (box_lock < 0)
        return;
    sceKernelLockMutex(box_lock, 1, NULL);
    sceClibSnprintf(ocr_model, sizeof(ocr_model), "%s", model);
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
        publish(list_seq, "Anki: still saving the previous card", VJO_ANKI_STATUS_DIM, -1, 0);
    else if (r == POST_FAILED)
        publish(list_seq, "Anki: not enough memory, card not saved", VJO_ANKI_STATUS_ERROR, -1, 0);
}

void vjo_anki_post_count(void)
{
    int busy;
    if (post_job(JOB_COUNT, 0, 0, &busy) != POST_STARTED)
        return; /* off, or a save or send that counts when it is done */
    /* A new look at the overlay: an earlier status for no list has been seen. */
    vjo_view_lock();
    if (g_view.anki_status_seq == 0 && g_view.anki_status[0]) {
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
    box_lock = sceKernelCreateMutex("VjoAnkiBox", 0, 0, NULL);
    if (box_lock < 0)
        return -1;
    vjo_platform_vita(&plat); /* the other dictionaries' entries */
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
    started = 0; /* enter is ignored from now on */
    next_job = JOB_NONE;
    t = thread;
    sceKernelUnlockMutex(box_lock, 1);
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
    return 0;
}
