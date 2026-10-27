/* vjo-spinner-bench MODEL_DIR [--press-ms MS] [--max-ms MS] [--scenarios A,B,...] [--synthetic] [--find]
 *                   [--settle-max-ms MS] [--settle-press-ms MS] < JOBS > RESULTS.jsonl
 *
 * What an animated icon in the OCR region (a spinning "wait" icon, a blinking "next" mark) or an animated background
 * does to Rubi's reading, on the host, end to end, with the real code of each step (rubi-spinner, #353;
 * docs/text-stability.md "Animated icons and moving pictures"):
 * - the game's picture, 60 frames a second: a region (a hooked page's dialogue region, or --synthetic's box) with an
 *   icon drawn over it in each frame (beside the text, or right after its last line; at a constant or a variable
 *   speed), a moving background, or the last line typing out (a character every 3 frames; audit-rubi-spinner's
 *   scenarios: every 10 or 15 frames, pauses of 30 frames, an icon that follows the typed end);
 * - the kernel's change detection, kernel/scene.c itself: the display hook's region signature every 8th frame
 *   (VJO_CHECK_EVERY_FRAMES), the kernel worker's scene_update every 4th poll of 16 ms, its settled and quiet events,
 *   and a capture's scene as kernel/game_ocr.c publishes it (scene_capture; VjoState.region_scene: scene_match_since);
 * - the game worker's job, core/vocr_ocr.c itself, with the policy game_ocr/main.c gives it on config.ini's defaults
 *   (the stable-text wait of 150 ms within 2 s, 1 s for a press; the LM, the cascade, the line cache, the short-line
 *   probe, the finder's fixes, the head gate): every strip it reads is the next frame of the game, as the display
 *   hook copies it;
 * - a model of the Shell's decisions (shell/worker.c, each named after its function there): Automatically's job on a
 *   settled or quiet screen (auto_prefetch), L + R (open_overlay: the shown text or the background result when it
 *   shows the screen, else "Recognizing..." and a job), a job whose screen changed under it (note_screen_changed's
 *   pace), one cancelled for a newer screen (cancel_outdated_job), one whose result is for an earlier screen
 *   (restart_old_job), the open overlay's refresh (background_open, on_refresh_done).
 *
 * The console's time is estimated (marked est. in the output): a strip read is one frame (16.7 ms); the line finder
 * 0.82 us a pixel (v0.8.0-test.1's full screen, 0.43 s for 960 x 544), spread over its first pass (the line watch's
 * runs of it, #353: added when the job ends, before its result); recognition 1.96 ms a
 * CTC frame (VOCR5-N at 444 MHz, docs/vita-vn-ocr.md's table: 0.57 s for 3 lines of 240 frames, 0.1 s of it the
 * finder) and the cascade's re-reads 5.54 ms a frame (VOCR5-L the same way). No console has run this.
 *
 * JOBS: for each region a header line "ID WIDTH HEIGHT NBOX\n", NBOX lines "X Y W H\n" (its text lines' boxes, the
 * hooked page's, in region pixels: where the icon goes, which pixels the "outside" background moves, which line
 * types out), then WIDTH x HEIGHT RGB bytes. --synthetic: one box of glyph-like strokes instead, no input.
 * RESULTS: one JSON object per region and scenario: when the text was shown after the press (or never), the text
 * shown, how (the background result, a job's), every job's result, the kernel's events and scene changes, the
 * stable-text wait's outcome, the jobs whose text was recognized but never shown, and the memory the checks hold.
 * The texts stay with the caller (tools/spinner_eval.py keeps them local). --find: no job; for each region and
 * scenario the line finder's boxes over the scenario's first 4 s (find_boxes). */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "fbgrey.h"
#include "platform_posix.h"
#include "textfilter.h"
#include "textnorm.h"
#include "vocr_lines.h"
#include "vocr_lines_fix.h"
#include "vocr_ocr.h"
#include "../include/vjo_vocr_sizes.h"
#include "../kernel/scene.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FRAME_US 16667          /* 60 frames a second */
#define CHECK_EVERY_FRAMES 8u   /* kernel/vjo_kernel.h VJO_CHECK_EVERY_FRAMES */
#define POLL_US 16000           /* kernel/main.c POLL_US */
#define CHECK_EVERY 4u          /* kernel/main.c CHECK_EVERY: change detection every 4th poll */
#define SHELL_POLL_US 50000     /* shell/worker.c POLL_TIMEOUT_US */
#define SCREEN_RETRY_MS 250u    /* shell/worker.c */
#define SCREEN_RETRY_MAX_MS 4000u
#define REFRESH_GAP_US 2000000  /* shell/worker.c */
#define EXCHANGE_US 5000        /* est.: the result's way back to the Shell's control thread */
#define EST_FRAME_US 1960u      /* est.: VOCR5-N at 444 MHz (see the top) */
#define EST_CASCADE_FRAME_US 5540u
#define EST_FINDER_NS_PER_PX 820u
#define MAX_BOXES 16
#define TEXT_CAP VJO_OCR_TEXT_CAP

/* ---- the region and what moves in it ---- */
typedef struct {
    unsigned x, y, w, h;
} Box;
typedef struct {
    char id[160];
    unsigned w, h, nbox;
    Box box[MAX_BOXES];
    unsigned char *rgb;   /* w x h x 3, the still picture */
    unsigned char *grey;  /* its grey, (77 R + 150 G + 29 B) >> 8 */
    unsigned char *near;  /* 1: within 3 px of a text line's box */
    unsigned median;      /* the region's median grey */
} Region;

enum { SP_NONE, SP_SPRITE8, SP_SPRITE24, SP_SMOOTH, SP_VARIABLE, SP_PAUSING };
enum { AT_INLINE, AT_BESIDE };
enum { BG_NONE, BG_OUTSIDE, BG_BEHIND };
typedef struct {
    const char *name;
    int spin, at, bg, typing;
    /* (audit-rubi-spinner's scenarios, F1) the typing line: frames a character (0: 3), a pause after so many characters
     * and its frames; follow: the icon right after the typed end */
    int type_every, pause_every, pause_frames, follow;
} Scenario;
static const Scenario scenarios[] = {
    {"still", SP_NONE, 0, BG_NONE, 0, 0, 0, 0, 0},
    {"inline-sprite8", SP_SPRITE8, AT_INLINE, BG_NONE, 0, 0, 0, 0, 0},
    {"inline-sprite24", SP_SPRITE24, AT_INLINE, BG_NONE, 0, 0, 0, 0, 0},
    {"inline-smooth", SP_SMOOTH, AT_INLINE, BG_NONE, 0, 0, 0, 0, 0},
    {"inline-variable", SP_VARIABLE, AT_INLINE, BG_NONE, 0, 0, 0, 0, 0},
    {"inline-pausing", SP_PAUSING, AT_INLINE, BG_NONE, 0, 0, 0, 0, 0},
    {"beside-sprite8", SP_SPRITE8, AT_BESIDE, BG_NONE, 0, 0, 0, 0, 0},
    {"beside-sprite24", SP_SPRITE24, AT_BESIDE, BG_NONE, 0, 0, 0, 0, 0},
    {"beside-smooth", SP_SMOOTH, AT_BESIDE, BG_NONE, 0, 0, 0, 0, 0},
    {"beside-variable", SP_VARIABLE, AT_BESIDE, BG_NONE, 0, 0, 0, 0, 0},
    {"beside-pausing", SP_PAUSING, AT_BESIDE, BG_NONE, 0, 0, 0, 0, 0},
    {"bg-outside", SP_NONE, 0, BG_OUTSIDE, 0, 0, 0, 0, 0},
    {"bg-behind", SP_NONE, 0, BG_BEHIND, 0, 0, 0, 0, 0},
    {"typing", SP_NONE, 0, BG_NONE, 1, 0, 0, 0, 0},
    {"typing-beside-variable", SP_VARIABLE, AT_BESIDE, BG_NONE, 1, 0, 0, 0, 0},
    {"typing-slow10", SP_NONE, 0, BG_NONE, 1, 10, 0, 0, 0},
    {"typing-slow15", SP_NONE, 0, BG_NONE, 1, 15, 0, 0, 0},
    {"typing-pauses", SP_NONE, 0, BG_NONE, 1, 3, 5, 30, 0},
    {"typing-slow10-beside-variable", SP_VARIABLE, AT_BESIDE, BG_NONE, 1, 10, 0, 0, 0},
    {"typing-pauses-beside-variable", SP_VARIABLE, AT_BESIDE, BG_NONE, 1, 3, 5, 30, 0},
    {"typing-slow10-follow-smooth", SP_SMOOTH, AT_INLINE, BG_NONE, 1, 10, 0, 0, 1},
    {"typing-pauses-follow-smooth", SP_SMOOTH, AT_INLINE, BG_NONE, 1, 3, 5, 30, 1},
    {"typing-pauses-follow-sprite8", SP_SPRITE8, AT_INLINE, BG_NONE, 1, 3, 5, 30, 1},
};
#define N_SCENARIOS (sizeof(scenarios) / sizeof(scenarios[0]))

typedef struct {
    const Region *rg;
    const Scenario *sc;
    double cx, cy, r;     /* the icon's centre and radius */
    unsigned char ink;    /* its grey level (light on a dark box, dark on a light one) */
    int x0, y0, x1, y1;   /* its bounding box, clipped */
    unsigned cov_frame;   /* the frame cov holds */
    float *cov;           /* its coverage there, (x1 - x0) x (y1 - y0) */
    unsigned type_line;   /* the line that types out */
    unsigned char *empty; /* per row, the box's colour beside that line (typing) */
} Picture;

/* The icon's angle in turns at frame f: sprite8 an 8-frame sprite at 12 frames a second; sprite24 a 24-frame one at
 * 30; smooth one turn a second; variable one turn a second on average, its speed 0.25-1.75 turns a second over a
 * 1.3 s cycle (an eased spinner); pausing a turn in 0.8 s, eased in and out, then still for 0.6 s. */
static double turns(int kind, unsigned f)
{
    double t = f * (FRAME_US / 1e6);
    switch (kind) {
    case SP_SPRITE8: return (double)((f / 5u) % 8u) / 8.0;
    case SP_SPRITE24: return (double)((f / 2u) % 24u) / 24.0;
    case SP_SMOOTH: return fmod(t, 1.0);
    case SP_VARIABLE: return fmod(t + 0.75 * 1.3 / (2.0 * M_PI) * (1.0 - cos(2.0 * M_PI * t / 1.3)), 1.0);
    case SP_PAUSING: {
        double c = fmod(t, 1.4);
        return c >= 0.8 ? 0.0 : 0.5 - 0.5 * cos(M_PI * c / 0.8);
    }
    default: return 0.0;
    }
}

/* A spinner: a ring whose arc covers 80 % of the turn with a fading tail, anti-aliased (4 x 4 samples a pixel). */
static void icon_cover(Picture *p, unsigned f)
{
    const double rin = p->r * 0.55, th = turns(p->sc->spin, f);
    const int w = p->x1 - p->x0;
    if (p->cov_frame == f) return;
    p->cov_frame = f;
    for (int y = p->y0; y < p->y1; y++)
        for (int x = p->x0; x < p->x1; x++) {
            double a = 0.0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    double dx = x + (sx + 0.5) / 4.0 - p->cx, dy = y + (sy + 0.5) / 4.0 - p->cy;
                    double rr = sqrt(dx * dx + dy * dy), ang, rel;
                    if (rr < rin || rr > p->r) continue;
                    ang = atan2(dy, dx) / (2.0 * M_PI);
                    rel = fmod(ang - th + 2.0, 1.0);
                    if (rel < 0.8) a += 0.25 + 0.75 * rel / 0.8;
                }
            p->cov[(y - p->y0) * w + (x - p->x0)] = (float)(a / 16.0);
        }
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* Characters of the typing line shown at frame f (audit-rubi-spinner's slow typewriters and pauses). */
static unsigned typed_chars(const Scenario *sc, unsigned f)
{
    unsigned te = sc->type_every ? (unsigned)sc->type_every : 3u, pe = (unsigned)sc->pause_every, cycle, in;
    if (!pe) return f / te + 1u;
    cycle = pe * te + (unsigned)sc->pause_frames;
    in = f % cycle / te;
    return f / cycle * pe + (in < pe ? in : pe) + 1u;
}

/* Row y of frame f, as RGB. */
static void render_row(Picture *p, unsigned y, unsigned f, unsigned char *out)
{
    const Region *rg = p->rg;
    const Scenario *sc = p->sc;
    memcpy(out, rg->rgb + (size_t)y * rg->w * 3, (size_t)rg->w * 3);
    if (sc->typing && p->type_line < rg->nbox) {
        /* the last line types out from frame 0, a character (about the line's height) every 3 frames */
        const Box *b = &rg->box[p->type_line];
        if (y >= b->y && y < b->y + b->h) {
            unsigned reveal = b->x + typed_chars(sc, f) * (b->h > 4 ? b->h - 4 : b->h);
            for (unsigned x = reveal; x < b->x + b->w && x < rg->w; x++)
                out[3 * x] = out[3 * x + 1] = out[3 * x + 2] = p->empty[y];
        }
    }
    if (sc->bg != BG_NONE) {
        const double t = f * (FRAME_US / 1e6);
        for (unsigned x = 0; x < rg->w; x++) {
            double s, c;
            if (sc->bg == BG_OUTSIDE && rg->near[(size_t)y * rg->w + x]) continue;
            s = 10.0 * sin(2.0 * M_PI * (x / 97.0 + y / 53.0 - t / 2.1));
            if (sc->bg == BG_BEHIND) { /* a translucent box over a moving picture: its glyphs stay */
                c = fabs((double)rg->grey[(size_t)y * rg->w + x] - rg->median) / 64.0;
                s *= 1.0 - (c > 1.0 ? 1.0 : c);
            }
            for (int k = 0; k < 3; k++) out[3 * x + k] = (unsigned char)clampi(out[3 * x + k] + (int)lrint(s), 0, 255);
        }
    }
    if (sc->follow && sc->typing && p->type_line < rg->nbox) { /* the icon right after the typed end */
        const Box *b = &rg->box[p->type_line];
        double end = b->x + (double)typed_chars(sc, f) * (b->h > 4 ? b->h - 4 : b->h), cx;
        if (end > b->x + b->w) end = b->x + b->w;
        cx = end + 0.6 * p->r + p->r;
        if (cx + p->r > rg->w) cx = rg->w - p->r - 1.0;
        if (cx != p->cx) {
            p->cx = cx;
            p->x0 = clampi((int)floor(p->cx - p->r) - 1, 0, (int)rg->w);
            p->x1 = clampi((int)ceil(p->cx + p->r) + 1, 0, (int)rg->w);
            p->cov_frame = ~0u;
        }
    }
    if (sc->spin != SP_NONE && (int)y >= p->y0 && (int)y < p->y1) {
        const int w = p->x1 - p->x0;
        icon_cover(p, f);
        for (int x = p->x0; x < p->x1; x++) {
            float a = p->cov[((int)y - p->y0) * w + (x - p->x0)];
            if (a <= 0.0f) continue;
            for (int k = 0; k < 3; k++)
                out[3 * x + k] = (unsigned char)lrintf(out[3 * x + k] * (1.0f - a) + p->ink * a);
        }
    }
}

static unsigned char grey_of(const unsigned char *c)
{
    return (unsigned char)((77u * c[0] + 150u * c[1] + 29u * c[2]) >> 8);
}

/* ---- the world's clock: frames, the kernel's polls, the Shell's ticks ---- */
typedef struct {
    int64_t now;       /* us since the page appeared */
    unsigned frame;    /* the frame on screen */
    int64_t next_frame, next_poll, next_shell;
    unsigned polls;
    unsigned bits;     /* kernel events the Shell has not taken */
} World;
static World W;
#define EV_STABLE 1u
#define EV_QUIET 2u

/* ---- the kernel ---- */
typedef struct {
    SceneTracker tr;
    SceneAcc acc, cap_acc;
    SceneSig sig, cap_sig;
    unsigned sig_seq, seen_seq;
    uint32_t region_seq, capture_scene, done_seq, seq;
    int64_t region_us, capture_begin_us;
    unsigned settled, quiet, changes, max_masked;
    uint32_t last_id;
} Kernel;
static Kernel K;
static Picture P;
static unsigned char row_rgb[VJO_OCR_MAX_WIDTH * 3];
static uint32_t row_abgr[VJO_OCR_MAX_WIDTH];

static void abgr_row(const unsigned char *rgb, unsigned w)
{
    for (unsigned x = 0; x < w; x++)
        row_abgr[x] = 0xFF000000u | (uint32_t)rgb[3 * x + 2] << 16 | (uint32_t)rgb[3 * x + 1] << 8 | rgb[3 * x];
}

/* kernel/display.c: the region's signature of this frame (the rows it samples only). */
static void display_hook(void)
{
    const Region *rg = P.rg;
    scene_acc_begin(&K.acc, &K.sig, rg->w, rg->h);
    while (scene_acc_next_row(&K.acc) < rg->h) {
        unsigned y = scene_acc_next_row(&K.acc);
        render_row(&P, y, W.frame, row_rgb);
        abgr_row(row_rgb, rg->w);
        scene_acc_row(&K.acc, y, row_abgr);
    }
    K.sig_seq++;
}

/* kernel/main.c change_detection: the newest signature, if new. No player input. */
static void kernel_check(void)
{
    SceneEvent ev;
    if (K.sig_seq == K.seen_seq) return;
    K.seen_seq = K.sig_seq;
    ev = scene_update(&K.tr, &K.sig, W.now, INT64_MIN / 4, 0);
    if (K.tr.id != K.last_id) K.changes++;
    K.last_id = K.tr.id;
    if ((unsigned)K.tr.masked > K.max_masked) K.max_masked = (unsigned)K.tr.masked;
    if (ev == SCENE_SETTLED) K.settled++, W.bits |= EV_STABLE;
    if (ev == SCENE_QUIET) K.quiet++, W.bits |= EV_QUIET;
}

typedef struct {
    uint32_t scene, stable, quiet, region_seq, region_scene, capture_scene, done_seq;
} St;
static St get_state(void) /* kernel/main.c vjoGetState */
{
    St s;
    memset(&s, 0, sizeof(s));
    s.scene = K.tr.id;
    s.stable = (uint32_t)K.tr.stable;
    s.quiet = (K.tr.quiet || scene_hook_drawing(&K.tr)) && !K.tr.stable;
    if (K.region_seq) {
        s.region_seq = K.region_seq;
        s.region_scene = scene_match_since(&K.tr, &K.cap_sig, K.region_us);
    }
    s.capture_scene = K.capture_scene;
    s.done_seq = K.done_seq;
    return s;
}

/* ---- the Shell (shell/worker.c's decisions) ---- */
enum { OV_CLOSED, OV_OPEN, OV_OPEN_OLD_JOB };
enum { JOB_BACKGROUND, JOB_PRESS, JOB_REFRESH };
#define MAX_JOBS 64
typedef struct {
    int kind, rc, settled, timeout, tries, cancelled, shown;
    unsigned tears, settle_passes, settle_changes, cycling_rows, lines, strips;
    unsigned watched, looks, finds, anyway, ignored; /* the line watch (#353): VjoVocrStats.watch* */
    unsigned recognized; /* lines its last try recognized (VjoVocrStats.recognized: in the debug log, shown or not) */
    int64_t start_us, end_us;
    uint32_t settle_ms;
    unsigned text_len;
} JobLog;
typedef struct {
    int ov;
    int job_running, job_pending, job_done_pending, job_kind, job_lookup, job_cancelled, job_captured, job_rc;
    int64_t job_done_at;
    uint32_t job_capture_scene, job_capture_seq;
    char job_text[TEXT_CAP];
    int local_text_valid, local_text_job; /* the job that read it */
    uint32_t local_text_scene, local_text_seq;
    char local_text[TEXT_CAP];
    int cache_ok;
    uint32_t cache_scene, cache_seq;
    int stable_pending, refresh_pending, open_retry, screen_changed_streak;
    int64_t screen_retry_until, refresh_after_us;
    /* the measure */
    int pressed, shown;
    int64_t press_us, shown_us;
    const char *shown_how;
    char shown_text[TEXT_CAP];
    unsigned refreshed;
    unsigned njobs;
    JobLog job[MAX_JOBS];
} Shell;
static Shell S;
static int64_t press_at_us; /* the player's L + R (run) */
static void open_overlay(void);

static int same_scene(uint32_t a, uint32_t b)
{
    return a != 0 && a == b;
}
static int shows_screen(uint32_t scene, uint32_t seq, const St *st)
{
    return same_scene(scene, st->scene) || (seq && seq == st->region_seq && same_scene(st->region_scene, st->scene));
}

/* The overlay shows the text the job at index `from` read (local_text's, or the cached result's: the same). */
static void show(const char *text, const char *how, int from)
{
    if (from >= 0) S.job[from].shown = 1;
    if (S.shown) return;
    S.shown = 1;
    S.shown_us = W.now;
    S.shown_how = how;
    snprintf(S.shown_text, sizeof(S.shown_text), "%s", text);
}

static void start_job(int kind, int lookup)
{
    if (S.job_running || S.njobs >= MAX_JOBS) return;
    if (kind != JOB_REFRESH) S.local_text_valid = 0; /* start_job_kind: retire_local_text */
    S.job_running = S.job_pending = 1;
    S.job_kind = kind;
    S.job_lookup = lookup;
    S.job_captured = S.job_cancelled = 0;
    S.job_capture_scene = S.job_capture_seq = 0;
}

static int cancel_outdated_job(const St *st)
{
    if (!S.job_running || S.job_done_pending || S.job_cancelled || !S.job_captured || !(st->stable || st->quiet) ||
        shows_screen(S.job_capture_scene, S.job_capture_seq, st))
        return 0;
    S.job_cancelled = 1;
    return 1;
}

static int claim_cache_for(const St *st)
{
    if (!S.cache_ok || !shows_screen(S.cache_scene, S.cache_seq, st)) return 0;
    S.cache_scene = st->scene;
    return 1;
}

static void note_screen_changed(void)
{
    uint32_t wait_ms = S.screen_changed_streak ? SCREEN_RETRY_MS << (S.screen_changed_streak - 1) : 0;
    if (wait_ms > SCREEN_RETRY_MAX_MS) wait_ms = SCREEN_RETRY_MAX_MS;
    if (S.screen_changed_streak < 8) S.screen_changed_streak++;
    S.screen_retry_until = W.now + (int64_t)wait_ms * 1000;
}

static void open_overlay(void)
{
    if (S.pressed) return;
    St st = get_state();
    S.pressed = 1;
    S.press_us = W.now;
    S.ov = OV_OPEN;
    if (S.local_text_valid && shows_screen(S.local_text_scene, S.local_text_seq, &st)) {
        show(S.local_text, "the shown text", S.local_text_job);
        return;
    }
    if (claim_cache_for(&st)) {
        show(S.local_text, "the background result", S.local_text_job);
        return;
    }
    if (S.job_running) {
        S.ov = OV_OPEN_OLD_JOB;
        cancel_outdated_job(&st);
    } else {
        start_job(JOB_PRESS, 1);
    }
}

static void on_refresh_done(void)
{
    S.refresh_after_us = W.now + REFRESH_GAP_US;
    if (S.ov == OV_OPEN_OLD_JOB) {
        S.ov = OV_OPEN;
        start_job(JOB_PRESS, 1);
        return;
    }
    if (S.job_cancelled || S.ov != OV_OPEN || !S.local_text_valid) {
        S.refresh_pending = S.job_cancelled && S.ov == OV_OPEN;
    } else if (S.job_rc == VJO_E_SCREEN_CHANGED) {
        note_screen_changed();
        S.refresh_pending = 1;
        S.refresh_after_us = S.screen_retry_until;
    } else {
        S.screen_changed_streak = 0;
        if (S.job_rc == VJO_OK && S.job_text[0] && strcmp(S.job_text, S.local_text)) {
            snprintf(S.local_text, sizeof(S.local_text), "%s", S.job_text);
            S.local_text_scene = S.job_capture_scene;
            S.local_text_seq = S.job_capture_seq;
            S.cache_ok = 0;
            S.refreshed++;
            S.local_text_job = (int)(S.njobs - 1);
            S.job[S.njobs - 1].shown = 1;
        } else if (S.job_rc == VJO_OK && !strcmp(S.job_text, S.local_text)) {
            S.local_text_scene = S.job_capture_scene;
            S.local_text_seq = S.job_capture_seq;
            S.job[S.njobs - 1].shown = 1; /* the text shown, read again */
        }
    }
    S.job_cancelled = 0;
}

static void on_job_done(void)
{
    St st;
    S.job_done_pending = 0;
    S.job_running = 0;
    if (S.job_kind == JOB_REFRESH) {
        on_refresh_done();
        return;
    }
    if (S.job_cancelled || S.job_rc == VJO_E_SCREEN_CHANGED) {
        if (S.job_rc == VJO_E_SCREEN_CHANGED && !S.job_cancelled) note_screen_changed();
        S.cache_ok = 0;
        if (S.job_cancelled) S.local_text_valid = 0; /* retire_local_text(1) */
        S.job_cancelled = 0;
        S.stable_pending = 1;
        if (S.ov != OV_CLOSED) {
            S.ov = OV_OPEN;
            if (W.now < S.screen_retry_until) S.open_retry = 1;
            else start_job(JOB_PRESS, 1);
        }
        return;
    }
    S.screen_changed_streak = 0;
    S.screen_retry_until = 0;
    S.cache_ok = S.job_rc == VJO_OK && S.job_lookup;
    S.cache_scene = S.job_capture_scene;
    S.cache_seq = S.job_capture_seq;
    if (S.ov == OV_OPEN) {
        if (S.job_lookup)
            show(S.job_rc == VJO_OK ? S.local_text : "", S.job_rc == VJO_OK ? "a job's result" : "an error",
                 S.job_rc == VJO_OK ? S.local_text_job : -1);
        return;
    }
    if (S.ov == OV_OPEN_OLD_JOB) { /* restart_old_job */
        S.ov = OV_OPEN;
        st = get_state();
        if (shows_screen(S.job_capture_scene, S.job_capture_seq, &st) && S.job_lookup) {
            show(S.job_rc == VJO_OK ? S.local_text : "", "the running job's result",
                 S.job_rc == VJO_OK ? S.local_text_job : -1);
        } else if (S.local_text_valid && shows_screen(S.local_text_scene, S.local_text_seq, &st)) {
            show(S.local_text, "the shown text (a lookup)", S.local_text_job);
        } else {
            start_job(JOB_PRESS, 1); /* "result was for an earlier screen" */
        }
    }
}

static void auto_prefetch(void)
{
    St st;
    if (S.ov != OV_CLOSED || S.job_running || !S.stable_pending || W.now < S.screen_retry_until) return;
    S.stable_pending = 0;
    st = get_state();
    if ((st.stable || st.quiet) && !claim_cache_for(&st)) start_job(JOB_BACKGROUND, 1);
}

static void background_open(void)
{
    St st;
    if (S.ov != OV_OPEN || S.job_running) return;
    if (S.open_retry) {
        if (W.now >= S.screen_retry_until) {
            S.open_retry = 0;
            start_job(JOB_PRESS, 1);
        }
        return;
    }
    if (!S.refresh_pending || W.now < S.refresh_after_us) return;
    S.refresh_pending = 0;
    st = get_state();
    if (S.local_text_valid && (st.stable || st.quiet) && !shows_screen(S.local_text_scene, S.local_text_seq, &st))
        start_job(JOB_REFRESH, 0);
}

static void shell_tick(void)
{
    unsigned bits = W.bits;
    W.bits = 0;
    if (S.job_done_pending && W.now >= S.job_done_at) on_job_done();
    if (bits & (EV_STABLE | EV_QUIET)) {
        S.stable_pending = 1;
        if (S.ov == OV_OPEN) S.refresh_pending = 1;
        if (S.job_running) {
            St st = get_state();
            cancel_outdated_job(&st);
        }
    }
    auto_prefetch();
    background_open();
}

/* Runs the world up to time t: frames (a signature every 8th), the kernel's polls, the Shell's ticks (every 50 ms,
 * on a kernel event and when a job's result is due). */
static void advance(int64_t t)
{
    while (W.now < t || (W.bits && !S.job_running)) {
        int64_t next = W.next_frame;
        if (W.next_poll < next) next = W.next_poll;
        if (W.next_shell < next) next = W.next_shell;
        if (S.job_done_pending && S.job_done_at < next) next = S.job_done_at;
        if (next > t) {
            if (!W.bits) {
                W.now = t;
                break;
            }
            next = W.now; /* an event waits: the Shell takes it now */
        }
        W.now = next;
        if (W.now >= W.next_frame) {
            W.frame++;
            W.next_frame = (int64_t)(W.frame + 1) * FRAME_US;
            if (W.frame % CHECK_EVERY_FRAMES == 0) display_hook();
        }
        if (W.now >= W.next_poll) {
            W.next_poll += POLL_US;
            if (++W.polls % CHECK_EVERY == 0) kernel_check();
        }
        if (W.now >= W.next_shell || W.bits || (S.job_done_pending && W.now >= S.job_done_at)) {
            if (W.now >= W.next_shell) W.next_shell += SHELL_POLL_US;
            shell_tick();
        }
        if (!S.pressed && W.now >= press_at_us) open_overlay(); /* L + R, also while a job runs */
    }
}

/* ---- the game worker's job ---- */
typedef struct {
    VjoVocrStats *st;
    unsigned cascade_frames;
} LineClock;
static LineClock line_clock;
static uint64_t sim_now(void *ud)
{
    (void)ud;
    return (uint64_t)W.now;
}
static void sim_sleep(uint32_t us)
{
    advance(W.now + us);
}
static int job_cancelled_cb(void *ud)
{
    (void)ud;
    return S.job_cancelled;
}

/* A strip of the region from the next frame (the display hook copies it there). The first pass's rows make the
 * capture's signature; its last strip publishes the capture (kernel/game_ocr.c publish_capture_locked), which the
 * Shell notes at once (vocr_note_capture). The first pass's strips also carry the finder's estimated time. */
static int strip_cb(void *ud, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags)
{
    const Region *rg = P.rg;
    (void)ud;
    advance((int64_t)(W.frame + 1) * FRAME_US);
    if ((flags & VJO_STRIP_FIRST) && y == 0) {
        K.region_seq = 0; /* capture_sig gets rewritten */
        K.capture_begin_us = W.now;
        scene_acc_begin(&K.cap_acc, &K.cap_sig, rg->w, rg->h);
        S.job_captured = 0;
    }
    for (unsigned r = 0; r < n; r++) {
        unsigned char *d = dst + (size_t)r * stride;
        render_row(&P, y + r, W.frame, row_rgb);
        for (unsigned x = 0; x < rg->w; x++) d[x] = grey_of(row_rgb + 3 * x);
        if (flags & VJO_STRIP_FIRST) {
            abgr_row(row_rgb, rg->w);
            scene_acc_row(&K.cap_acc, y + r, row_abgr);
        }
    }
    if (flags & VJO_STRIP_LAST) {
        K.capture_scene = scene_capture(&K.tr, &K.cap_sig, K.capture_begin_us);
        K.region_seq = K.done_seq = K.seq;
        K.region_us = W.now;
        S.job_capture_scene = K.capture_scene;
        S.job_capture_seq = K.region_seq;
        S.job_captured = 1;
    }
    if (flags & VJO_STRIP_FIRST) advance(W.now + (int64_t)n * rg->w * EST_FINDER_NS_PER_PX / 1000);
    return (int)n;
}

/* After each line: its estimated recognition time on the console (none for a line from the cache). */
static void line_cb(void *ud, const VjoVocrLineInfo *info)
{
    LineClock *lc = ud;
    const VjoVocrStats *st = lc->st;
    uint64_t us = 0;
    (void)info;
    if (st->recognized && !st->line[st->recognized - 1].cached)
        us += (uint64_t)st->line[st->recognized - 1].frames * EST_FRAME_US;
    /* the cascade's re-reads since the last line (a new try starts its count again) */
    if (st->cascade_frames < lc->cascade_frames) lc->cascade_frames = 0;
    us += (uint64_t)(st->cascade_frames - lc->cascade_frames) * EST_CASCADE_FRAME_US;
    lc->cascade_frames = st->cascade_frames;
    advance(W.now + (int64_t)us);
}

static void *line_buffer(void *ud, size_t bytes)
{
    static unsigned char *mem;
    (void)ud;
    if (!mem) mem = malloc(VJO_VOCR_LINE_MAX_BYTES);
    return bytes <= VJO_VOCR_LINE_MAX_BYTES ? mem : NULL;
}
static unsigned allocations; /* the job path's, counted: the weights' and LM's, once each */
static void *heap_alloc(void *ud, size_t bytes)
{
    void *p = NULL;
    (void)ud;
    allocations++;
    return posix_memalign(&p, 64, bytes) ? NULL : p;
}
static VjoVocrResident res, cres;
static VjoVocrLmResident lres;
static int res_release(void *ud)
{
    free(((VjoVocrResident *)ud)->data);
    return 0;
}
static int lres_release(void *ud)
{
    free(((VjoVocrLmResident *)ud)->data);
    return 0;
}

typedef struct {
    const char *dir;
    const VjoVocrModel *model, *cascade;
    const VjoVocrLm *lm;
    unsigned settle_max_ms, settle_press_ms;
} Setup;
static Setup su;
static PosixPlatform posix;
static VjoPlatform files;
static VjoVocrCache line_cache;
static unsigned char capture_block[VJO_VOCR_CAPTURE_LIMIT];
static unsigned char *workspace;
static size_t workspace_bytes;

static void run_job(void)
{
    const Region *rg = P.rg;
    VjoPlatform plat = files;
    VjoGreySource src = {NULL, rg->w, rg->h, strip_cb, job_cancelled_cb, line_buffer};
    VjoVocrPolicy pol;
    VjoVocrStats st;
    static char text[TEXT_CAP], raw[TEXT_CAP];
    static unsigned char arena_mem[64 * 1024];
    VjoArena arena;
    JobLog *jl = &S.job[S.njobs++];
    St st0 = get_state();
    size_t need;
    int rc;
    S.job_pending = 0;
    memset(jl, 0, sizeof(*jl));
    jl->kind = S.job_kind;
    jl->start_us = W.now;
    K.seq++; /* the request's capture_seq */
    plat.now_us = sim_now;
    memset(&pol, 0, sizeof(pol));
    /* game_ocr/main.c request_policy and its job, on config.ini's defaults (shell/worker.c vocr_run_one) */
    pol.settle_us = 150000u;
    pol.settle_max_us = (S.job_kind == JOB_PRESS ? su.settle_press_ms : su.settle_max_ms) * 1000u;
    pol.settle_first = !st0.stable;
    pol.sleep = sim_sleep;
    pol.raw = raw;
    pol.raw_cap = sizeof(raw);
    pol.lm = su.lm;
    pol.lm_res = su.lm ? &lres : NULL;
    if (su.cascade) {
        pol.cascade = su.cascade;
        pol.cascade_res = &cres;
        pol.cascade_us = 2000000u;
    }
    pol.short_lines = 1;
    pol.counter_tau = VJO_VOCR_COUNTER_TAU;
    pol.counter_open = VJO_VOCR_COUNTER_OPEN;
    pol.head_gate = pol.two_level = 1;
    pol.line_fix = VOCR_FIX_DEFAULT;
    pol.cache = &line_cache;
    pol.line = line_cb;
    pol.ud = &line_clock;
    line_clock.st = &st;
    line_clock.cascade_frames = 0;
    need = vjo_vocr_job_bytes(su.model->name, rg->w, rg->h) + vjo_vocr_lm_job_bytes(su.model->name) +
           (su.cascade ? vjo_vocr_cascade_job_bytes(su.cascade->name) : 0);
    if (need > workspace_bytes) {
        free(workspace);
        workspace_bytes = need;
        if (posix_memalign((void **)&workspace, 64, need)) exit(3);
    }
    memset(&st, 0, sizeof(st));
    rc = vjo_vocr_ocr_policy(&plat, su.dir, su.model->name, &src, &res, workspace, need, capture_block,
                             vjo_vocr_capture_bytes(rg->w, rg->h), &pol, text, sizeof(text), &st);
#ifdef VJO_VOCR_LINE_WATCH
    /* the line watch's finder runs (#353): their reads were frames already; their compute, est. as the first pass's */
    advance(W.now + (int64_t)st.watch_looks * rg->w * rg->h * EST_FINDER_NS_PER_PX / 1000);
#endif
    S.job_rc = rc;
    S.job_text[0] = '\0';
    if (rc == VJO_OK) { /* as the worker returns it and the Shell shows it */
        const char *shown;
        vjo_text_normalize_keep(text, sizeof(text), NULL, 0, NULL);
        vjo_arena_init(&arena, arena_mem, sizeof(arena_mem));
        shown = vjo_filter_lines(&arena, text, 1);
        snprintf(S.job_text, sizeof(S.job_text), "%s", shown ? shown : "");
        if (S.job_kind != JOB_REFRESH) { /* run_vocr: the text is the overlay's from here */
            snprintf(S.local_text, sizeof(S.local_text), "%s", S.job_text);
            S.local_text_valid = 1;
            S.local_text_job = (int)(S.njobs - 1);
            S.local_text_scene = S.job_capture_scene;
            S.local_text_seq = S.job_capture_seq;
        }
    }
    jl->rc = rc;
    jl->cancelled = S.job_cancelled;
    jl->settled = (int)st.settled;
    jl->timeout = (int)st.settle_timeout;
    jl->tries = (int)st.tries;
    jl->tears = st.tears;
    jl->settle_passes = st.settle_passes;
    jl->settle_changes = st.settle_changes;
    jl->cycling_rows = st.cycling_rows;
    jl->settle_ms = st.settle_us / 1000u;
    jl->lines = st.lines;
    jl->strips = st.strips;
    jl->recognized = st.recognized;
#ifdef VJO_VOCR_LINE_WATCH /* (a tree before #353 has no line watch: the bench measures main too) */
    jl->watched = st.watched;
    jl->looks = st.watch_looks;
    jl->finds = st.watch_finds;
    jl->anyway = st.read_anyway;
    jl->ignored = st.watch_ignored;
#endif
    jl->text_len = (unsigned)strlen(S.job_text);
    jl->end_us = W.now;
    S.job_done_pending = 1;
    S.job_done_at = W.now + EXCHANGE_US;
}

static void json_text(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
        else if (c == '\n') fputs("\\n", f);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static const char *kinds[] = {"background", "press", "refresh"};

/* --find: the line finder's boxes (the job's finder with the worker's fixes) on the scenario's frames 0 to 238, every
 * 7th: per frame the count, and per box the range its edges take over the frames, and whether the icon's box
 * overlaps it (where the finder puts the icon). */
static int find_mode;
static void find_boxes(void)
{
    static unsigned char grey[VJO_OCR_MAX_WIDTH * VJO_OCR_MAX_HEIGHT], arena[256 * 1024];
    const Region *rg = P.rg;
    VocrLinesImage im = {grey, rg->w, rg->h, rg->w, VOCR_LINES_GREY};
    unsigned lo[VOCR_LINES_MAX][4], hi[VOCR_LINES_MAX][4], counts[VOCR_LINES_MAX + 1], icon[VOCR_LINES_MAX];
    unsigned frames = 0, first = 0;
    memset(counts, 0, sizeof(counts));
    memset(icon, 0, sizeof(icon));
    for (unsigned f = 0; f < 240; f += 7, frames++) {
        VocrLineSet set;
        memset(&set, 0, sizeof(set));
        for (unsigned y = 0; y < rg->h; y++) {
            render_row(&P, y, f, row_rgb);
            for (unsigned x = 0; x < rg->w; x++) grey[(size_t)y * rg->w + x] = grey_of(row_rgb + 3 * x);
        }
        if (vocr_find_lines_fix(&im, VOCR_LINES_TEXTBOX_RECOVERY, VOCR_FIX_DEFAULT, arena, sizeof(arena), &set))
            continue;
        counts[set.count]++;
        if (!f) first = set.count;
        for (unsigned i = 0; i < set.count; i++) {
            const VocrLine *l = &set.lines[i];
            unsigned e[4] = {l->x, l->y, l->x + l->w, l->y + l->h};
            for (unsigned k = 0; k < 4; k++) {
                if (!f || e[k] < lo[i][k]) lo[i][k] = e[k];
                if (!f || e[k] > hi[i][k]) hi[i][k] = e[k];
            }
            icon[i] |= P.sc->spin != SP_NONE && (int)l->x < P.x1 && (int)(l->x + l->w) > P.x0 && (int)l->y < P.y1 &&
                       (int)(l->y + l->h) > P.y0;
        }
    }
    printf("\"frames\": %u, \"counts\": {", frames);
    for (unsigned c = 0, n = 0; c <= VOCR_LINES_MAX; c++)
        if (counts[c]) printf("%s\"%u\": %u", n++ ? ", " : "", c, counts[c]);
    printf("}, \"boxes\": [");
    for (unsigned i = 0; i < first; i++)
        printf("%s{\"x\": [%u, %u], \"y\": [%u, %u], \"right\": [%u, %u], \"bottom\": [%u, %u], \"icon\": %u}",
               i ? ", " : "", lo[i][0], hi[i][0], lo[i][1], hi[i][1], lo[i][2], hi[i][2], lo[i][3], hi[i][3], icon[i]);
    printf("]");
}

/* One region, one scenario: the page appears at 0, the player presses L + R at press_ms and waits up to max_ms. */
static void run(const Region *rg, const Scenario *sc, unsigned press_ms, unsigned max_ms)
{
    static float cov[160 * 160];
    const Box *last = rg->nbox ? &rg->box[rg->nbox - 1] : NULL;
    int64_t end;
    unsigned allocs_before;
    memset(&W, 0, sizeof(W));
    memset(&K, 0, sizeof(K));
    memset(&S, 0, sizeof(S));
    memset(&P, 0, sizeof(P));
    memset(&line_cache, 0, sizeof(line_cache));
    P.rg = rg;
    P.sc = sc;
    P.cov = cov;
    P.cov_frame = ~0u;
    W.next_frame = FRAME_US;
    W.next_poll = POLL_US;
    W.next_shell = SHELL_POLL_US;
    if (sc->spin != SP_NONE) {
        double d = last ? last->h * 0.9 : 24.0;
        d = d < 14.0 ? 14.0 : d > 48.0 ? 48.0 : d;
        P.r = d / 2.0;
        if (sc->at == AT_INLINE && last) { /* right after the last line, on its middle */
            P.cx = last->x + last->w + 0.3 * d + P.r;
            P.cy = last->y + last->h / 2.0;
        } else { /* the region's bottom right corner */
            P.cx = rg->w - P.r - 4.0;
            P.cy = rg->h - P.r - 4.0;
        }
        if (P.cx + P.r > rg->w) P.cx = rg->w - P.r - 1.0;
        if (P.cy + P.r > rg->h) P.cy = rg->h - P.r - 1.0;
        if (P.cx < P.r) P.cx = P.r;
        if (P.cy < P.r) P.cy = P.r;
        P.x0 = clampi((int)floor(P.cx - P.r) - 1, 0, (int)rg->w);
        P.x1 = clampi((int)ceil(P.cx + P.r) + 1, 0, (int)rg->w);
        P.y0 = clampi((int)floor(P.cy - P.r) - 1, 0, (int)rg->h);
        P.y1 = clampi((int)ceil(P.cy + P.r) + 1, 0, (int)rg->h);
        {
            unsigned long sum = 0, n = 0;
            for (int y = P.y0; y < P.y1; y++)
                for (int x = P.x0; x < P.x1; x++) sum += rg->grey[(size_t)y * rg->w + x], n++;
            P.ink = n && sum / n < 128 ? 240 : 16;
        }
    }
    if (sc->typing) {
        static unsigned char empty[VJO_OCR_MAX_HEIGHT];
        P.type_line = rg->nbox ? rg->nbox - 1 : 0;
        P.empty = empty;
        for (unsigned y = 0; y < rg->h; y++) {
            unsigned x = last && last->x + last->w + 2 < rg->w ? last->x + last->w + 2 : rg->w - 1;
            empty[y] = last ? rg->grey[(size_t)y * rg->w + x] : (unsigned char)rg->median;
        }
    }
    if (find_mode) {
        printf("{\"id\": ");
        json_text(stdout, rg->id);
        printf(", \"scenario\": \"%s\", \"icon\": [%d, %d, %d, %d], ", sc->name, P.x0, P.y0, P.x1 - P.x0, P.y1 - P.y0);
        find_boxes();
        puts("}");
        return;
    }
    allocs_before = allocations;
    press_at_us = (int64_t)press_ms * 1000;
    end = (int64_t)(press_ms + max_ms) * 1000;
    for (;;) {
        if (S.job_pending) run_job();
        if (!S.pressed && W.now >= (int64_t)press_ms * 1000) open_overlay();
        if (S.job_pending) continue;
        if (W.now >= end || (S.shown && W.now >= S.shown_us + 2000000)) break;
        advance(W.now + 1000);
    }
    /* the result */
    {
        unsigned ok = 0, with_text = 0, not_shown = 0, changed = 0, cancelled = 0, timeouts = 0, held = 0;
        unsigned after_press = 0, after_press_not_shown = 0, dropped = 0;
        for (unsigned i = 0; i < S.njobs; i++) {
            const JobLog *j = &S.job[i];
            ok += j->rc == VJO_OK;
            changed += j->rc == VJO_E_SCREEN_CHANGED;
            cancelled += j->cancelled;
            timeouts += j->timeout != 0;
            held += j->settled != 0;
            with_text += j->rc == VJO_OK && j->text_len;
            /* a job's text that never reached the overlay (shown, or the background result claimed at L + R) */
            not_shown += j->rc == VJO_OK && j->text_len && !j->shown;
            dropped += j->rc != VJO_OK && j->recognized; /* lines recognized, the result dropped (the user's log) */
            if (S.pressed && j->start_us >= S.press_us && (!S.shown || j->start_us <= S.shown_us)) {
                after_press++;
                if (j->rc == VJO_OK && j->text_len && !j->shown) after_press_not_shown++;
            }
        }
        printf("{\"id\": ");
        json_text(stdout, rg->id);
        printf(", \"scenario\": \"%s\", \"w\": %u, \"h\": %u, \"press_ms\": %u, \"shown\": %d, \"show_ms\": %lld, "
               "\"how\": \"%s\", \"text\": ", sc->name, rg->w, rg->h, press_ms, S.shown,
               S.shown ? (long long)((S.shown_us - S.press_us) / 1000) : -1LL, S.shown ? S.shown_how : "");
        json_text(stdout, S.shown_text);
        printf(", \"jobs\": %u, \"ok\": %u, \"with_text\": %u, \"screen_changed\": %u, \"cancelled\": %u, "
               "\"settle_timeouts\": %u, \"settle_held\": %u, \"text_not_shown\": %u, \"press_jobs\": %u, "
               "\"press_text_not_shown\": %u, \"refreshed\": %u, \"recognized_dropped\": %u, \"symptom\": %d, "
               "\"kernel\": {\"settled\": %u, "
               "\"quiet\": %u, \"scene_changes\": %u, \"max_masked\": %u}, \"memory\": {\"capture_block\": %zu, "
               "\"state_slots\": %u, \"tracker\": %zu, \"allocations\": %u}, \"job_log\": [",
               S.njobs, ok, with_text, changed, cancelled, timeouts, held, not_shown, after_press,
               after_press_not_shown,
               S.refreshed, dropped, !S.shown && (with_text > 0 || dropped > 0), K.settled, K.quiet, K.changes,
               K.max_masked,
               vjo_vocr_capture_bytes(rg->w, rg->h), vjo_vocr_state_slots(rg->w, rg->h), sizeof(SceneTracker),
               allocations - allocs_before);
        for (unsigned i = 0; i < S.njobs; i++) {
            const JobLog *j = &S.job[i];
            printf("%s{\"kind\": \"%s\", \"at_ms\": %lld, \"ms\": %lld, \"rc\": %d, \"cancelled\": %d, "
                   "\"settled\": %d, "
                   "\"timeout\": %d, \"tries\": %d, \"tears\": %u, \"settle_ms\": %u, \"settle_passes\": %u, "
                   "\"settle_changes\": %u, \"cycling_rows\": %u, \"lines\": %u, \"strips\": %u, \"chars\": %u, "
                   "\"shown\": %d, \"recognized\": %u, \"watched\": %u, \"looks\": %u, \"finds\": %u, \"anyway\": %u, "
                   "\"ignored\": %u}",
                   i ? ", " : "", kinds[j->kind], (long long)(j->start_us / 1000),
                   (long long)((j->end_us - j->start_us) / 1000), j->rc, j->cancelled, j->settled, j->timeout,
                   j->tries, j->tears, j->settle_ms, j->settle_passes, j->settle_changes, j->cycling_rows, j->lines,
                   j->strips, j->text_len, j->shown, j->recognized, j->watched, j->looks, j->finds, j->anyway,
                   j->ignored);
        }
        puts("]}");
        fflush(stdout);
    }
}

static void finish_region(Region *rg)
{
    unsigned hist[256] = {0}, half = rg->w * rg->h / 2, acc = 0;
    rg->grey = malloc((size_t)rg->w * rg->h);
    rg->near = calloc((size_t)rg->w * rg->h, 1);
    if (!rg->grey || !rg->near) exit(3);
    for (size_t i = 0; i < (size_t)rg->w * rg->h; i++) {
        rg->grey[i] = grey_of(rg->rgb + 3 * i);
        hist[rg->grey[i]]++;
    }
    for (rg->median = 0; rg->median < 255 && (acc += hist[rg->median]) < half; rg->median++) {
    }
    for (unsigned b = 0; b < rg->nbox; b++) {
        const Box *bx = &rg->box[b];
        for (int y = (int)bx->y - 3; y < (int)(bx->y + bx->h) + 3; y++)
            for (int x = (int)bx->x - 3; x < (int)(bx->x + bx->w) + 3; x++)
                if (x >= 0 && y >= 0 && x < (int)rg->w && y < (int)rg->h) rg->near[(size_t)y * rg->w + x] = 1;
    }
}

/* --synthetic: a dark 917 x 125 box (CLANNAD's is 852 x 135) with 3 lines of glyph-like strokes, 24 px tall. */
static void synthetic(Region *rg)
{
    static const unsigned chars[] = {26, 24, 11};
    memset(rg, 0, sizeof(*rg));
    snprintf(rg->id, sizeof(rg->id), "synthetic");
    rg->w = 917;
    rg->h = 125;
    rg->rgb = malloc((size_t)rg->w * rg->h * 3);
    if (!rg->rgb) exit(3);
    memset(rg->rgb, 24, (size_t)rg->w * rg->h * 3);
    rg->nbox = 3;
    for (unsigned l = 0; l < 3; l++) {
        unsigned top = 14 + l * 36;
        rg->box[l] = (Box){18, top - 3, chars[l] * 30 + 6, 30};
        for (unsigned c = 0; c < chars[l]; c++)
            for (unsigned y = top; y < top + 24; y++)
                for (unsigned x = 21 + c * 30; x < 21 + c * 30 + 24; x++) {
                    unsigned k = (x - 21 - c * 30) * 7 + (y - top) * 3 + c * 5;
                    int ink = (k % 11 < 2) || ((y - top) % 9 == c % 9) || ((x - c * 30) % 13 == (c * 3) % 13);
                    if (ink) memset(rg->rgb + ((size_t)y * rg->w + x) * 3, 230, 3);
                }
    }
    finish_region(rg);
}

int main(int argc, char **argv)
{
    unsigned press_ms = 3000, max_ms = 20000;
    int synth = 0;
    char which[N_SCENARIOS];
    static char line[512];
    memset(which, 1, sizeof(which));
    su.settle_max_ms = 2000;
    su.settle_press_ms = 1000;
    if (argc < 2) goto usage;
    su.dir = argv[1];
    for (int i = 2; i < argc; i++) {
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(argv[i], "--press-ms") && v) press_ms = (unsigned)atoi(v), i++;
        else if (!strcmp(argv[i], "--max-ms") && v) max_ms = (unsigned)atoi(v), i++;
        else if (!strcmp(argv[i], "--settle-max-ms") && v) su.settle_max_ms = (unsigned)atoi(v), i++;
        else if (!strcmp(argv[i], "--settle-press-ms") && v) su.settle_press_ms = (unsigned)atoi(v), i++;
        else if (!strcmp(argv[i], "--synthetic")) synth = 1;
        else if (!strcmp(argv[i], "--find")) find_mode = 1;
        else if (!strcmp(argv[i], "--scenarios") && v) {
            memset(which, 0, sizeof(which));
            for (const char *s = v; *s;) {
                size_t n = strcspn(s, ",");
                int found = 0;
                for (unsigned k = 0; k < N_SCENARIOS; k++)
                    if (strlen(scenarios[k].name) == n && !strncmp(scenarios[k].name, s, n)) which[k] = 1, found = 1;
                if (!found) goto usage;
                s += n + (s[n] == ',');
            }
            i++;
        } else goto usage;
    }
    su.model = vjo_vocr_model(vjo_vocr_tier_model(VJO_VOCR_TIER_DEFAULT));
    su.cascade = vjo_vocr_model(vjo_vocr_tier_model(VJO_VOCR_TIER_DEFAULT + 1));
    su.lm = vjo_vocr_lm_tier(2);
    if (!su.model) goto usage;
    res = (VjoVocrResident){heap_alloc, res_release, &res, NULL, NULL, 0, 0, 0};
    cres = (VjoVocrResident){heap_alloc, res_release, &cres, NULL, NULL, 0, 0, 0};
    lres = (VjoVocrLmResident){heap_alloc, lres_release, &lres, NULL, NULL, 0, 0, 0};
    posix_platform_init(&posix, &files); /* the model files are read for real; the clock is the world's */
    if (synth) {
        Region rg;
        synthetic(&rg);
        for (unsigned k = 0; k < N_SCENARIOS; k++)
            if (which[k]) run(&rg, &scenarios[k], press_ms, max_ms);
        return 0;
    }
    while (fgets(line, sizeof(line), stdin)) {
        Region rg;
        memset(&rg, 0, sizeof(rg));
        if (sscanf(line, "%159s %u %u %u", rg.id, &rg.w, &rg.h, &rg.nbox) != 4 || !rg.w || !rg.h ||
            rg.w > VJO_OCR_MAX_WIDTH || rg.h > VJO_OCR_MAX_HEIGHT || rg.nbox > MAX_BOXES) {
            fprintf(stderr, "bad job header: %s", line);
            return 2;
        }
        for (unsigned b = 0; b < rg.nbox; b++) {
            Box *bx = &rg.box[b];
            if (!fgets(line, sizeof(line), stdin) || sscanf(line, "%u %u %u %u", &bx->x, &bx->y, &bx->w, &bx->h) != 4) {
                fprintf(stderr, "bad box in %s\n", rg.id);
                return 2;
            }
        }
        rg.rgb = malloc((size_t)rg.w * rg.h * 3);
        if (!rg.rgb || fread(rg.rgb, 3, (size_t)rg.w * rg.h, stdin) != (size_t)rg.w * rg.h) {
            fprintf(stderr, "short job %s\n", rg.id);
            return 2;
        }
        finish_region(&rg);
        for (unsigned k = 0; k < N_SCENARIOS; k++)
            if (which[k]) run(&rg, &scenarios[k], press_ms, max_ms);
        free(rg.rgb);
        free(rg.grey);
        free(rg.near);
    }
    return 0;
usage:
    fprintf(stderr, "usage: vjo-spinner-bench MODEL_DIR [--press-ms MS] [--max-ms MS] [--scenarios A,B] [--synthetic]\n"
                    "       [--find] [--settle-max-ms MS] [--settle-press-ms MS] < JOBS\nscenarios:");
    for (unsigned k = 0; k < N_SCENARIOS; k++) fprintf(stderr, " %s", scenarios[k].name);
    fputc('\n', stderr);
    return 2;
}
