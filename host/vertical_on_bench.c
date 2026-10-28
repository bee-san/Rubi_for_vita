/* vjo-vertical-on-bench MODEL_DIR [--mode off|auto|on|columns] [--fps N] [--turn FRAMES] [--rest FRAMES]
 *                       [--icon X,Y] [--phases N] [--frame-us ROWS,CASCADE,COLUMNS] [--tall rows]
 *                       [--press retry[:MS]|nowait|wait] [--cluster] < JOB > RESULTS.jsonl
 *
 * #379: L + R on a dialogue region with an animated icon in it, as the game worker reads it (core/vocr_ocr.c, with
 * the policy game_ocr/main.c gives a press on config.ini's defaults: the 1 MB LM, the cascade with the next size up
 * within 2 s, the line finder's fixes, the head gate) and the Shell tries it again (shell/worker.c
 * note_screen_changed: a job whose screen changed under every read is dropped quietly, the next try at once, then
 * 250 ms later, doubling up to 4 s), for 20 s after the press.
 *  - The stable-text wait, --press: retry (the default, main's since #413's audit, shell/worker.c start_job_kind):
 *    the press's first job reads at once (#413: opening the overlay confirms that the text is ready), and once a job's
 *    screen changed under every read (the Shell's count of them, screen_changed_streak) the retries wait as a
 *    background read does, for 150 ms of still rows first (ocr_settle_ms; the kernel has not called a screen whose
 *    icon moves stable), within ocr_settle_max_ms, 2 s (retry:MS: within MS); nowait, #413's, no job waits; wait, the
 *    press before #413 (0.9.8's and #385's), every job 150 ms first within 1 s (ocr_settle_press_ms). The worker
 *    learns an icon's rows only in the wait (core/vocr_ocr.c stream_check): without it every change of them between
 *    two reads tears the read.
 *  - Every strip read is the game's next frame (kernel/game_ocr.c vjoOcrCaptureRows: the display hook copies the rows
 *    as the game shows its next frame), at --fps frames a second (30 by default).
 *  - The picture is the region with a 24-px icon at --icon (its top left corner in the region): a bar that turns one
 *    step a frame for --turn frames (15), then rests for --rest frames (15), and again; each phase of --phases (12)
 *    presses at another moment of it, phase p at p / phases of its cycle (#441's audit F3). --cluster: the phases
 *    7,919 us apart, all 12 within 87 ms of one moment of the cycle, as the bench placed them until then (#385's,
 *    #411's and #441's tables).
 *  - Recognition takes time on the same clock as the strips: each recognized line its CTC frames times a time a
 *    frame, --frame-us's three, in us: the Normal size's model (rows), the cascade's (the Large size's, for the
 *    frames it reads again) and the vertical model (columns). By default 2,610, 5,040 and 1,700: measured for rows,
 *    estimated for columns. 0.8.0-test.4's console logs measured VOCR5-N (VOCR6-N's shape) at 2.61 ms a frame and VOCR5-L
 *    (VOCR6-L's) at 4.98-5.10 ms, at 444 MHz, the clock a job raises by default, each a job's recognition time over
 *    its frames with the LM's decoding and the default two helpers (docs/guide.md); VOCR5-V's is VOCR5-S's estimate at
 *    444 MHz (docs/vita-vn-ocr.md). The bench recognizes one line at a time (the line callback turns the helpers off,
 *    core/vocr_ocr.c), each charged so. So the icon moves while a line is recognized, as on the console. Before #385's
 *    audit F1 the bench charged 3 times each line's time on this clock, which does not move while a line is
 *    recognized: nothing (--frame-us 0,0,0 is that bench); before #411's audit F1 the first line of a core attempt
 *    after one that tore with a line read went uncharged. The finder's time counts nothing. The times are estimates:
 *    no console has run this.
 * Modes: off, rows; auto, rows checked for vertical text (VJO_VOCR_CHECK_AUTO), then the columns; on, as the worker
 * reads it (API 53: rows checked the other way, VJO_VOCR_CHECK_ON, then the columns, but a region at least as tall as
 * wide read as columns at once, #385's audit F6; --tall rows: its rows first too, as API 53 had it before that; built
 * against a tree before API 53, the columns at once, as 0.9.8's on); columns, the vertical model at once. The weights
 * stay between phases, as a game keeps them.
 * JOB: one region, "ID WIDTH HEIGHT\n" and WIDTH x HEIGHT RGBA bytes (vjo-page-ocr's format). The docs' two regions
 * are crops of the committed samples: the horizontal box host/samples/vn_textbox.jpg's 900 x 160 at (30, 360), the
 * vertical one host/samples/vertical.jpg's 220 x 380 at (630, 50), e.g. with Pillow:
 *   python3 -c "import sys; from PIL import Image; im = Image.open(sys.argv[1]).convert('RGBA').crop((30, 360, 930,
 *   520)); sys.stdout.buffer.write(b'box 900 160\n' + im.tobytes())" host/samples/vn_textbox.jpg > box.job
 * RESULTS: one JSON object per phase: the press's wait (--press), rc (0 shown, -120 the screen changed under every try
 * for 20 s), the jobs, those torn, those that waited, the rows that tore, the strips read, ms from the press to the
 * text, the CTC frames recognized and the ms charged for them (the cascade's included), whether it was read as
 * columns, its lines and text ('|' between lines). A summary line on stderr. Host only. */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform_posix.h"
#include "vocr_lines_fix.h"
#include "vocr_ocr.h"
#include "../include/vjo_vocr_sizes.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define ICON 24u
#define PRESS_US 20000000ull /* how long the user waits */

static uint64_t vclock;              /* the simulated clock, us */
static unsigned frame_us, turn = 15, rest = 15, icon_x, icon_y, rw, rh;
static unsigned char *still;         /* the region's grey rows, the icon left out */
static unsigned long strips;
/* --press: which jobs of a press wait for still rows, and within what (the header) */
enum { PRESS_RETRY, PRESS_NOWAIT, PRESS_WAIT };
static int press_mode = PRESS_RETRY;
static uint32_t retry_max_us = 2000000u; /* ocr_settle_max_ms */
#define SETTLE_US 150000u                /* ocr_settle_ms */
#define PRESS_CAP_US 1000000u            /* ocr_settle_press_ms: a press's wait budget before #413 */

static uint64_t now_cb(void *ud)
{
    (void)ud;
    return vclock;
}
static void sleep_cb(uint32_t us)
{
    vclock += us;
}

/* The icon in frame f: a bar from its centre, one step of a turn a frame while it turns, still while it rests. */
static unsigned char icon_px(uint64_t f, unsigned x, unsigned y, unsigned char bg)
{
    unsigned phase = (unsigned)(f % (turn + rest)), step = phase < turn ? phase : 0;
    double a = 2 * M_PI * step / (turn ? turn : 1), dx = x + 0.5 - ICON / 2.0, dy = y + 0.5 - ICON / 2.0;
    double along = dx * cos(a) + dy * sin(a), across = fabs(-dx * sin(a) + dy * cos(a));
    return along > 0 && along < ICON / 2.0 && across < 2.5 ? 230 : bg;
}

/* The display hook's copy: rows [y, y + n) of the game's next frame. */
static int strip_cb(void *ud, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags)
{
    (void)ud;
    (void)flags;
    vclock = (vclock / frame_us + 1u) * frame_us + 300u;
    const uint64_t f = vclock / frame_us;
    strips++;
    for (unsigned r = 0; r < n; r++) {
        const unsigned yy = y + r;
        unsigned char *d = dst + (size_t)r * stride;
        memcpy(d, still + (size_t)yy * rw, rw);
        if (yy >= icon_y && yy < icon_y + ICON)
            for (unsigned x = icon_x; x < icon_x + ICON && x < rw; x++) d[x] = icon_px(f, x - icon_x, yy - icon_y, d[x]);
    }
    return (int)n;
}

static void *line_buffer(void *ud, size_t bytes)
{
    static unsigned char *mem;
    (void)ud;
    if (!mem) mem = malloc(VJO_VOCR_LINE_MAX_BYTES);
    return bytes <= VJO_VOCR_LINE_MAX_BYTES ? mem : NULL;
}
static void *heap_alloc(void *ud, size_t bytes)
{
    void *p = NULL;
    (void)ud;
    return posix_memalign(&p, 64, bytes) ? NULL : p;
}
static VjoVocrResident res, vres, cres;
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
/* Recognition on the simulated clock (#385's audit F1): each line the core recognized (core/vocr_ocr.c line_stat
 * counts it, then calls the line callback) its CTC frames times the try's model's time a frame, and the frames the
 * cascade read again since the last line times the cascade's. The core zeroes its counts at each of its own attempts
 * (run_job, VjoVocrStats.tries): the accounting starts again when tries changes (#411's audit F1: an attempt that tore
 * after one line left the next attempt's first line uncharged, the count not falling), and when a count falls (an
 * attempt run again under the same number, RETRY_NOCACHE). */
static const VjoVocrStats *cur_st;   /* the try's stats, while it runs */
static uint32_t cur_frame_us, casc_frame_us;
static unsigned seen_lines, seen_casc, seen_tries;
static unsigned long charged_frames; /* this phase's, the cascade's included */
static uint64_t charged_us;
static void line_cb(void *ud, const VjoVocrLineInfo *info)
{
    (void)ud;
    if (!cur_st || info->blank) return;
    if (cur_st->tries != seen_tries) { /* the core's next attempt */
        seen_tries = cur_st->tries;
        seen_lines = seen_casc = 0;
    }
    if (cur_st->recognized < seen_lines) seen_lines = 0;
    if (cur_st->cascade_frames < seen_casc) seen_casc = 0;
    for (; seen_lines < cur_st->recognized; seen_lines++) {
        const VjoVocrLineStat *ls = &cur_st->line[seen_lines];
        if (ls->cached) continue;
        vclock += (uint64_t)ls->frames * cur_frame_us;
        charged_us += (uint64_t)ls->frames * cur_frame_us;
        charged_frames += ls->frames;
    }
    vclock += (uint64_t)(cur_st->cascade_frames - seen_casc) * casc_frame_us;
    charged_us += (uint64_t)(cur_st->cascade_frames - seen_casc) * casc_frame_us;
    charged_frames += cur_st->cascade_frames - seen_casc;
    seen_casc = cur_st->cascade_frames;
}

typedef struct {
    const char *dir;
    const VjoVocrModel *model, *cascade, *vertical;
    const VjoVocrLm *lm;
    uint32_t frame_us[3]; /* a CTC frame's time: the rows' model, the cascade's, the vertical model */
    uint32_t settle_us;   /* the job's stable-text wait (VjoVocrPolicy.settle_us), 0: none; set for each job (--press) */
} Setup;

/* One try of a job: the worker's policy for a press (game_ocr/main.c request_policy, vjo_game_ocr_run_request). */
static int run_try(const Setup *su, const VjoPlatform *plat, const VjoVocrModel *m, VjoGreySource *src, int check,
                   uint32_t settle_left, int first, char *text, size_t cap, VjoVocrStats *st)
{
    const int cols = (m->finder_mode & VJO_VOCR_FINDER_COLUMNS) != 0;
    const VjoVocrModel *casc = cols ? NULL : su->cascade;
    static unsigned char capture[VJO_VOCR_CAPTURE_LIMIT];
    static char raw[VJO_OCR_TEXT_CAP];
    VjoVocrPolicy pol;
    size_t need = vjo_vocr_job_bytes(m->name, src->width, src->height);
    void *ws = NULL;
    int rc;
    if (!need) return VJO_E_OCR_REGION;
    need += vjo_vocr_lm_job_bytes(m->name) + (casc ? vjo_vocr_cascade_job_bytes(casc->name) : 0);
    if (posix_memalign(&ws, 64, need)) return VJO_E_OOM;
    memset(&pol, 0, sizeof(pol));
    pol.raw = raw;
    pol.raw_cap = sizeof(raw);
    pol.lm = su->lm;
    pol.lm_res = &lres;
    if (casc) {
        pol.cascade = casc;
        pol.cascade_res = &cres;
        pol.cascade_us = 2000000u; /* ocr_cascade_ms = 2000 */
    }
    pol.short_lines = 1;
    pol.counter_tau = VJO_VOCR_COUNTER_TAU;
    pol.counter_open = VJO_VOCR_COUNTER_OPEN;
    pol.head_gate = pol.two_level = 1;
    pol.line_fix = cols ? 0u : VOCR_FIX_DEFAULT;
    pol.vertical_check = check;
    pol.settle_us = su->settle_us; /* ocr_settle_ms in a job that waits (--press), else 0 */
    pol.settle_max_us = settle_left; /* the job's wait budget, over its tries */
    pol.settle_first = first;
    pol.sleep = sleep_cb;
    pol.line = line_cb;
    memset(st, 0, sizeof(*st));
    cur_st = st;
    cur_frame_us = cols ? su->frame_us[2] : su->frame_us[0];
    casc_frame_us = su->frame_us[1];
    seen_lines = seen_casc = seen_tries = 0;
    rc = vjo_vocr_ocr_policy(plat, su->dir, m->name, src, cols ? &vres : &res, ws, need, capture, sizeof(capture), &pol,
                             text, cap, st);
    cur_st = NULL;
    free(ws);
    return rc;
}

int main(int argc, char **argv)
{
    const char *mode = "on";
    unsigned fps = 30, phases = 12;
    int tall_rows = 0, cluster = 0;
    char line[512], id[256], press_name[32] = "retry:2000";
    Setup su = {.frame_us = {2610u, 5040u, 1700u}};
    if (argc < 2) goto usage;
    su.dir = argv[1];
    for (int i = 2; i < argc; i++) {
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(argv[i], "--mode") && v) mode = v, i++;
        else if (!strcmp(argv[i], "--fps") && v) fps = (unsigned)atoi(v), i++;
        else if (!strcmp(argv[i], "--turn") && v) turn = (unsigned)atoi(v), i++;
        else if (!strcmp(argv[i], "--rest") && v) rest = (unsigned)atoi(v), i++;
        else if (!strcmp(argv[i], "--phases") && v) phases = (unsigned)atoi(v), i++;
        else if (!strcmp(argv[i], "--icon") && v && sscanf(v, "%u,%u", &icon_x, &icon_y) == 2) i++;
        else if (!strcmp(argv[i], "--frame-us") && v &&
                 sscanf(v, "%u,%u,%u", &su.frame_us[0], &su.frame_us[1], &su.frame_us[2]) == 3) i++;
        else if (!strcmp(argv[i], "--tall") && v && !strcmp(v, "rows")) tall_rows = 1, i++;
        else if (!strcmp(argv[i], "--cluster")) cluster = 1;
        else if (!strcmp(argv[i], "--press") && v) {
            unsigned ms = 2000u;
            char tail = 0;
            if (!strcmp(v, "wait")) press_mode = PRESS_WAIT;
            else if (!strcmp(v, "nowait")) press_mode = PRESS_NOWAIT;
            else if (!strcmp(v, "retry") || (sscanf(v, "retry:%u%c", &ms, &tail) == 1 && ms && ms <= 10000u))
                press_mode = PRESS_RETRY, retry_max_us = 1000u * ms;
            else goto usage;
            if (press_mode == PRESS_RETRY) snprintf(press_name, sizeof(press_name), "retry:%u", ms);
            else snprintf(press_name, sizeof(press_name), "%s", v);
            i++;
        }
        else goto usage;
    }
    const int m_off = !strcmp(mode, "off"), m_auto = !strcmp(mode, "auto"), m_on = !strcmp(mode, "on"),
              m_cols = !strcmp(mode, "columns");
    if (!(m_off || m_auto || m_on || m_cols) || !fps || fps > 240 || !phases || turn + rest == 0) goto usage;
    frame_us = 1000000u / fps;
    if (!fgets(line, sizeof(line), stdin) || sscanf(line, "%255s %u %u", id, &rw, &rh) != 3 || !rw || !rh ||
        rw > VJO_OCR_MAX_WIDTH || rh > VJO_OCR_MAX_HEIGHT) {
        fprintf(stderr, "bad job header\n");
        return 2;
    }
    unsigned char *px = malloc((size_t)rw * rh * 4);
    still = malloc((size_t)rw * rh);
    if (!px || !still || fread(px, 4, (size_t)rw * rh, stdin) != (size_t)rw * rh) {
        fprintf(stderr, "short job %s\n", id);
        return 2;
    }
    for (size_t i = 0; i < (size_t)rw * rh; i++)
        still[i] = (unsigned char)((77u * px[4 * i] + 150u * px[4 * i + 1] + 29u * px[4 * i + 2]) >> 8);
    free(px);
    PosixPlatform pp;
    VjoPlatform plat;
    posix_platform_init(&pp, &plat);
    plat.now_us = now_cb;
    su.model = vjo_vocr_model(vjo_vocr_tier_model(VJO_VOCR_TIER_DEFAULT));
    su.cascade = vjo_vocr_model(vjo_vocr_tier_model(VJO_VOCR_TIER_DEFAULT + 1));
    su.vertical = vjo_vocr_model(VJO_VOCR_VERTICAL_NAME);
    su.lm = vjo_vocr_lm_tier(2); /* ocr_lm = 1000k */
    if (!su.model || !su.vertical || !su.lm) goto usage;
    res = (VjoVocrResident){heap_alloc, res_release, &res, {NULL}, NULL, 0, 0, 0};
    vres = (VjoVocrResident){heap_alloc, res_release, &vres, {NULL}, NULL, 0, 0, 0};
    cres = (VjoVocrResident){heap_alloc, res_release, &cres, {NULL}, NULL, 0, 0, 0};
    lres = (VjoVocrLmResident){heap_alloc, lres_release, &lres, {NULL}, NULL, 0, 0, 0};
    VjoGreySource src;
    memset(&src, 0, sizeof(src));
    src.width = rw;
    src.height = rh;
    src.strip = strip_cb;
    src.line_buffer = line_buffer;
#ifdef VJO_VOCR_CHECK_ON
    /* on: a region at least as tall as wide as columns at once (game_ocr/main.c, #385's audit F6), but --tall rows */
    const int first_cols = m_cols || (m_on && !tall_rows && rh >= rw),
              check = m_auto ? VJO_VOCR_CHECK_AUTO : m_on ? VJO_VOCR_CHECK_ON : 0;
#else /* before API 53: on read the columns at once */
    const int first_cols = m_cols || m_on, check = m_auto;
    (void)tall_rows;
#endif
    unsigned shown = 0;
    double sum_ms = 0;
    for (unsigned ph = 0; ph < phases; ph++) {
        /* another moment of the icon's cycle each phase: over the whole cycle, or (--cluster) 7.9 ms apart */
        vclock = 10000000ull + (cluster ? (uint64_t)ph * 7919u : (uint64_t)ph * (turn + rest) * frame_us / phases);
        const uint64_t press = vclock;
        unsigned jobs = 0, torn = 0, tears = 0, streak = 0, waited = 0;
        static char text[VJO_OCR_TEXT_CAP];
        VjoVocrStats st;
        int rc = VJO_E_SCREEN_CHANGED;
        strips = 0;
        charged_frames = 0;
        charged_us = 0;
        while (vclock - press < PRESS_US) {
            jobs++;
            /* this job's wait (--press): retry's once a job tore under every read (shell/worker.c start_job_kind,
             * screen_changed_streak), wait's always, nowait's never */
            const uint32_t budget = press_mode == PRESS_WAIT ? PRESS_CAP_US
                                    : press_mode == PRESS_RETRY && streak ? retry_max_us : 0u;
            su.settle_us = budget ? SETTLE_US : 0u;
            waited += budget != 0;
            rc = run_try(&su, &plat, first_cols ? su.vertical : su.model, &src, first_cols ? 0 : check, budget, 1, text,
                         sizeof(text), &st);
            tears += st.tears;
            if (rc == VJO_E_VERTICAL) { /* the worker's next try: the columns, within what is left of the wait */
                const uint32_t left = budget > st.settle_us ? budget - st.settle_us : 1u;
                rc = run_try(&su, &plat, su.vertical, &src, 0, left, 0, text, sizeof(text), &st);
                tears += st.tears;
            }
            if (rc != VJO_E_SCREEN_CHANGED) break;
            torn++;
            unsigned wait_ms = streak ? 250u << (streak - 1u) : 0u; /* shell/worker.c note_screen_changed */
            if (wait_ms > 4000u) wait_ms = 4000u;
            if (streak < 8) streak++;
            vclock += (uint64_t)wait_ms * 1000u + 5000u; /* and the result's way to the Shell, est. */
        }
        const double ms = (double)(vclock - press) / 1000.0;
        if (rc == VJO_OK) {
            shown++;
            sum_ms += ms;
        }
        printf("{\"id\": \"%s\", \"mode\": \"%s\", \"fps\": %u, \"turn\": %u, \"rest\": %u, \"press\": \"%s\", "
               "\"cluster\": %d, "
               "\"phase\": %u, \"rc\": %d, \"jobs\": %u, \"torn\": %u, \"waited\": %u, \"tears\": %u, \"strips\": %lu, "
               "\"ms\": %.0f, \"frames\": %lu, \"recognition_ms\": %.0f, \"columns\": %u, \"lines\": %u, \"text\": \"",
               id, mode, fps, turn, rest, press_name, cluster, ph, rc, jobs, torn, waited, tears, strips, ms, charged_frames,
               (double)charged_us / 1000.0, rc == VJO_OK ? st.columns : 0u, rc == VJO_OK ? st.lines : 0u);
        for (const char *p = rc == VJO_OK ? text : ""; *p; p++) {
            if (*p == '\n') fputc('|', stdout);
            else if (*p == '"' || *p == '\\') printf("\\%c", *p);
            else fputc(*p, stdout);
        }
        puts("\"}");
        fflush(stdout);
    }
    fprintf(stderr, "%s %s, %u fps, turn %u rest %u, press %s: shown in %u of %u phases, mean %.0f ms\n", id, mode, fps,
            turn, rest, press_name, shown, phases, shown ? sum_ms / shown : 0.0);
    vjo_vocr_resident_drop(&res);
    vjo_vocr_resident_drop(&vres);
    vjo_vocr_resident_drop(&cres);
    vjo_vocr_lm_drop(&lres);
    return 0;
usage:
    fprintf(stderr, "usage: %s MODEL_DIR [--mode off|auto|on|columns] [--fps N] [--turn FRAMES] [--rest FRAMES] "
                    "[--icon X,Y] [--phases N] [--frame-us ROWS,CASCADE,COLUMNS] [--tall rows] "
                    "[--press retry[:MS]|nowait|wait] [--cluster] < JOB\n",
            argv[0]);
    return 2;
}
