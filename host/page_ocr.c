/* vjo-page-ocr MODEL_DIR [--tier 1-3] [--lm 0-4] [--cascade on|off] [--psp] [--vertical off|on|auto]
 *               [--line-fix off|on|junk|all] [--helpers 0-2] < JOBS > RESULTS.jsonl
 *
 * The game worker's job (core/vocr_ocr.c, as game_ocr/main.c sets it up on the defaults of config.ini) on many
 * regions, with the weights, the LM and the cascade's model read once and kept, as a game keeps them between
 * jobs: the model of a size (vocr_tier: Small, Normal or Large), the LM up to that tier (ocr_lm = 1000k: tier 2),
 * the confidence cascade with the next size up within 2,000 ms (ocr_cascade_ms), the head gate and the two-level
 * head (ocr_head_gate = on, API 42: each where a blob carries it; --head-gate off: neither), the counter re-decode, the
 * short-line probe, the refinement on PSP titles only (--psp; ocr_refine_lines = psp), the text normalised as
 * the worker returns it (core/textnorm.c, the border rule: non_japanese_filter = lines) and filtered as the Shell
 * shows it (core/textfilter.c). API 47's vertical text (ocr_vertical): off reads rows; on reads the region's
 * columns with the vertical model (VJO_VOCR_VERTICAL_NAME), no cascade and no refinement; auto reads rows
 * checked first (VjoVocrPolicy.vertical_check) and, where they look like vertical text, the columns as on: the
 * worker's retry (game_ocr/main.c, VJO_E_VERTICAL). API 50's line finder fixes (ocr_line_fix): --line-fix on is
 * the worker's default (VOCR_FIX_DEFAULT), junk and all add JUNK and VALLEY; without the option the rows are the
 * frozen finder's, as they were when API 47's figures were measured.
 *
 * JOBS: for each region a header line "ID WIDTH HEIGHT [PSP]\n" (ID without spaces; PSP 1: a PSP game's frame,
 * whose rows the job refines, as --psp), then WIDTH x HEIGHT RGBA bytes.
 * RESULTS: one JSON object per job: rc, the text shown, whether it was read as columns (and whether auto's check
 * switched to them), every line read (its box in the region, dark flag, VJO_VOCR_LINE_* flags, text), the box the
 * worker suggests for the region read as a whole frame (API 52, #358: "suggest", x0, y0, x1, y1 on the 960 x 544
 * screen; all 0 none; tools/firstbox_eval.py), the job's
 * strips, finder, recognition and wall times (host clock: the work divides as on the Vita, the times are this
 * machine's), its workspace and the stats the worker's log carries. Host only; private frames stay with the
 * caller (tools/vertical_eval.py).
 *
 * --find-only: no recognition; for each region the row finder's lines as the job reads them (VOCR_LINES_
 * TEXTBOX_RECOVERY on the region's grey rows, with --line-fix's fixes: the job's own finder, before its short-line
 * probe), the column finder's columns (the vertical model's flags), and auto's verdict as the job's check gives it
 * (vjo_vocr_rows_vertical; with --line-fix, a region at least as tall as wide is checked on the frozen finder's
 * rows, as the job does, so the printed rows are not the ones checked there). */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "arena.h"
#include "config.h"
#include "platform_posix.h"
#include "textfilter.h"
#include "textnorm.h"
#include "vocr_ocr.h"
#include "vocr_lines.h"
#include "vocr_lines_fix.h"
#include "../include/vjo_vocr_sizes.h"
/* API 47's vertical text; without it (a tree before it, main's for the comparison) the tool reads rows only. */
#ifdef VJO_VOCR_LINE_COLUMN
#define HAVE_VERTICAL 1
#include "vocr_vertical.h"
#else
#define HAVE_VERTICAL 0
#define VJO_E_VERTICAL (-121)
#endif

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

/* ---- the worker's helper threads (VjoVocrPool), as host/vocr_parallel_bench.c ---- */
typedef struct {
    pthread_t t[1 + VJO_VOCR_HELPERS_MAX];
    void (*fn[1 + VJO_VOCR_HELPERS_MAX])(void *);
    void *arg[1 + VJO_VOCR_HELPERS_MAX];
} Pool;
static void *tramp(void *p)
{
    void **a = (void **)p;
    Pool *pl = (Pool *)a[0];
    unsigned slot = (unsigned)(uintptr_t)a[1];
    free(p);
    pl->fn[slot](pl->arg[slot]);
    return NULL;
}
static int pool_run(void *ud, unsigned slot, void (*fn)(void *), void *arg)
{
    Pool *pl = (Pool *)ud;
    void **a = malloc(2 * sizeof(void *));
    if (!a) return -1;
    pl->fn[slot] = fn;
    pl->arg[slot] = arg;
    a[0] = pl;
    a[1] = (void *)(uintptr_t)slot;
    if (pthread_create(&pl->t[slot], NULL, tramp, a)) {
        free(a);
        return -1;
    }
    return 0;
}
static int pool_join(void *ud, unsigned slot)
{
    return pthread_join(((Pool *)ud)->t[slot], NULL) ? -1 : 0;
}

/* A line taller than a strip (a column, most of the region's height), kept whole: the game worker's line block
 * (game_ocr/main.c line_buffer, VJO_VOCR_LINE_MAX_BYTES). */
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

/* The lines of the job's last try, as the policy's callback gives them (it reads every line in turn: a job
 * with it runs no helper, which changes no text: host/vocr_parallel_bench.c). */
#define MAX_KEPT 64
typedef struct {
    unsigned n;
    struct {
        unsigned x, y, w, h, dark, flags, blank;
        char text[512];
    } l[MAX_KEPT];
} Kept;
static void keep_line(void *ud, const VjoVocrLineInfo *info)
{
    Kept *k = ud;
    if (!info->index) k->n = 0; /* a try that began again */
    if (k->n >= MAX_KEPT) return;
    k->l[k->n].x = info->x;
    k->l[k->n].y = info->y;
    k->l[k->n].w = info->w;
    k->l[k->n].h = info->h;
    k->l[k->n].dark = info->dark;
    k->l[k->n].flags = info->flags;
    k->l[k->n].blank = (unsigned)(info->blank != 0);
    snprintf(k->l[k->n].text, sizeof(k->l[k->n].text), "%s", info->text ? info->text : "");
    k->n++;
}

typedef struct {
    const char *dir;
    const VjoVocrModel *model, *vertical, *cascade;
    const VjoVocrLm *lm;
    int psp, mode, head;
    unsigned helpers, lines_cb, find_only, line_fix;
} Setup;

static void print_set(const char *name, const VocrLineSet *set, int rc)
{
    printf(", \"%s_rc\": %d, \"%s_truncated\": %u, \"%s\": [", name, rc, name, set->truncated, name);
    for (unsigned i = 0; !rc && i < set->count; i++)
        printf("%s[%u, %u, %u, %u, %u]", i ? ", " : "", set->lines[i].x, set->lines[i].y, set->lines[i].w,
               set->lines[i].h, set->lines[i].dark);
    putchar(']');
}

/* --find-only: the region's grey rows as the job reads them, both finders, auto's rule. */
static void find_only(const Setup *su, const char *id, VjoGreySource *src)
{
    static unsigned char grey[VJO_OCR_MAX_WIDTH * VJO_OCR_MAX_HEIGHT];
    static unsigned char arena[256 * 1024];
    const unsigned w = src->width, h = src->height;
    VocrLinesImage im = {grey, w, h, w, VOCR_LINES_GREY};
    VocrLineSet rows, cols;
    double t0;
    int rrc, crc, vertical;
    unsigned row_us, col_us;
    memset(&rows, 0, sizeof(rows));
    memset(&cols, 0, sizeof(cols));
    if (src->strip(src->ud, 0, h, grey, w, VJO_STRIP_FIRST | VJO_STRIP_LAST) != (int)h) {
        printf("{\"id\": \"%s\", \"rc\": %d}\n", id, VJO_E_SOURCE);
        return;
    }
    t0 = now_s();
    rrc = vocr_find_lines_fix(&im, VOCR_LINES_TEXTBOX_RECOVERY, su->line_fix, arena, sizeof(arena), &rows);
    row_us = (unsigned)(1e6 * (now_s() - t0));
    t0 = now_s();
#if HAVE_VERTICAL
    if (su->line_fix && h >= w) { /* as the job: a tall region is checked on the frozen finder's rows */
        VocrLineSet frozen;
        memset(&frozen, 0, sizeof(frozen));
        vertical = !rrc && !vocr_find_lines(&im, VOCR_LINES_TEXTBOX_RECOVERY, arena, sizeof(arena), &frozen) &&
                   vjo_vocr_rows_vertical(frozen.lines, frozen.count, w, h);
    } else
        vertical = !rrc && vjo_vocr_rows_vertical(rows.lines, rows.count, w, h);
#else
    vertical = 0;
#endif
    double rule_ns = 1e9 * (now_s() - t0);
    t0 = now_s();
#if HAVE_VERTICAL
    crc = vocr_find_columns(&im, su->vertical->finder_mode & 0xFFu, arena, sizeof(arena), &cols);
#else
    crc = -1;
#endif
    col_us = (unsigned)(1e6 * (now_s() - t0));
    printf("{\"id\": \"%s\", \"rc\": 0, \"w\": %u, \"h\": %u, \"vertical\": %d, \"rows_us\": %u, "
           "\"rule_ns\": %.0f, \"cols_us\": %u", id, w, h, vertical, row_us, rule_ns, col_us);
    print_set("rows", &rows, rrc);
    print_set("cols", &cols, crc);
    puts("}");
    fflush(stdout);
}

/* One try of the job with model m (columns: the vertical model): the worker's policy for it. */
static int run_one(const Setup *su, const VjoPlatform *plat, const VjoVocrModel *m, VjoGreySource *src,
                   VjoVocrResident *r, int check, char *text, size_t cap, char *raw, size_t raw_cap,
                   VjoVocrStats *st, Kept *kept, size_t *ws_bytes)
{
#if HAVE_VERTICAL
    const int cols = (m->finder_mode & VJO_VOCR_FINDER_COLUMNS) != 0;
#else
    const int cols = 0;
    (void)check;
#endif
    const VjoVocrModel *casc = cols ? NULL : su->cascade;
    static unsigned char capture[VJO_VOCR_CAPTURE_LIMIT];
    Pool pl;
    VjoVocrPool pool = {su->helpers, pool_run, pool_join, NULL, &pl};
    VjoVocrPolicy pol;
    size_t need = vjo_vocr_job_bytes(m->name, src->width, src->height);
    size_t refine = !cols && su->psp ? vjo_vocr_refine_bytes(src->width, src->height) : 0;
    size_t extra = su->lm || casc ? vjo_vocr_lm_job_bytes(m->name) + (casc ? vjo_vocr_cascade_job_bytes(casc->name) : 0)
                                  : 0;
    size_t helper = su->helpers ? vjo_vocr_helpers_bytes(m->name, su->helpers) : 0;
    void *ws = NULL;
    int rc;
    if (!need) return VJO_E_OCR_REGION;
    need += refine + extra + helper;
    if (posix_memalign(&ws, 64, need)) return VJO_E_OOM;
    memset(&pol, 0, sizeof(pol));
    pol.raw = raw;
    pol.raw_cap = raw_cap;
    if (su->lm) {
        pol.lm = su->lm;
        pol.lm_res = &lres;
    }
    if (casc) {
        pol.cascade = casc;
        pol.cascade_res = &cres;
        pol.cascade_us = 2000000u; /* ocr_cascade_ms = 2000 */
        pol.cascade_conf = 0; /* VJO_VOCR_CASCADE_CONF, the worker's (game_ocr/main.c VJO_GAME_CASCADE_CONF 0) */
    }
    pol.refine = refine ? VJO_VOCR_REFINE_ALL : 0u;
    pol.short_lines = 1; /* #194 */
    pol.counter_tau = VJO_VOCR_COUNTER_TAU;
    pol.counter_open = VJO_VOCR_COUNTER_OPEN;
    pol.head_gate = pol.two_level = su->head; /* API 42, config.ini ocr_head_gate = on */
    pol.line_fix = cols ? 0u : su->line_fix; /* API 50, config.ini ocr_line_fix (rows only) */
#if HAVE_VERTICAL
    pol.vertical_check = check;
#endif
    pol.pool = su->helpers && !su->lines_cb ? &pool : NULL;
    pol.line = su->lines_cb ? keep_line : NULL;
    pol.ud = kept;
    kept->n = 0;
    rc = vjo_vocr_ocr_policy(plat, su->dir, m->name, src, r, ws, need, capture, sizeof(capture), &pol, text, cap, st);
    *ws_bytes = need;
    free(ws);
    return rc;
}

int main(int argc, char **argv)
{
    Setup su = {0};
    unsigned tier = VJO_VOCR_TIER_DEFAULT, lm_tier = 2;
    int cascade = 1;
    PosixPlatform pp;
    VjoPlatform plat;
    static char line[512], text[VJO_OCR_TEXT_CAP], raw[VJO_OCR_TEXT_CAP];
    static unsigned char arena_mem[64 * 1024];
    if (argc < 2) goto usage;
    su.dir = argv[1];
    su.mode = 0;
    su.lines_cb = 1;
    su.head = 1;
    for (int i = 2; i < argc; i++) {
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(argv[i], "--tier") && v) tier = (unsigned)atoi(v), i++;
        else if (!strcmp(argv[i], "--lm") && v) lm_tier = (unsigned)atoi(v), i++;
        else if (!strcmp(argv[i], "--cascade") && v) cascade = !strcmp(v, "on"), i++;
        else if (!strcmp(argv[i], "--psp")) su.psp = 1;
        else if (!strcmp(argv[i], "--helpers") && v) su.helpers = (unsigned)atoi(v), i++;
        else if (!strcmp(argv[i], "--no-lines")) su.lines_cb = 0;
        else if (!strcmp(argv[i], "--find-only")) su.find_only = 1;
        else if (!strcmp(argv[i], "--head-gate") && v) su.head = !strcmp(v, "on"), i++;
        else if (!strcmp(argv[i], "--line-fix") && v) {
            su.line_fix = !strcmp(v, "on")     ? VOCR_FIX_DEFAULT
                          : !strcmp(v, "junk") ? VOCR_FIX_DEFAULT | VOCR_FIX_JUNK
                          : !strcmp(v, "all")  ? VOCR_FIX_DEFAULT | VOCR_FIX_JUNK | VOCR_FIX_VALLEY
                                               : 0u;
            if (su.line_fix == 0u && strcmp(v, "off")) goto usage;
            i++;
        }
        else if (!strcmp(argv[i], "--vertical") && v) {
            su.mode = !strcmp(v, "on") ? 1 : !strcmp(v, "auto") ? 2 : 0;
            i++;
        } else goto usage;
    }
    if (tier < 1 || tier > VJO_VOCR_TIERS || lm_tier > VJO_VOCR_LM_TIERS || su.helpers > VJO_VOCR_HELPERS_MAX)
        goto usage;
    su.model = vjo_vocr_model(vjo_vocr_tier_model((int)tier));
#if HAVE_VERTICAL
    su.vertical = vjo_vocr_model(VJO_VOCR_VERTICAL_NAME);
#else
    su.vertical = su.model; /* never used: --vertical on and auto need API 47 */
    if (su.mode) goto usage;
#endif
    su.cascade = cascade && tier < VJO_VOCR_TIERS ? vjo_vocr_model(vjo_vocr_tier_model((int)tier + 1)) : NULL;
    su.lm = lm_tier ? vjo_vocr_lm_tier(lm_tier) : NULL;
    if (!su.model || !su.vertical || (lm_tier && !su.lm)) goto usage;
    posix_platform_init(&pp, &plat);
    res = (VjoVocrResident){heap_alloc, res_release, &res, NULL, NULL, 0, 0, 0};
    vres = (VjoVocrResident){heap_alloc, res_release, &vres, NULL, NULL, 0, 0, 0};
    cres = (VjoVocrResident){heap_alloc, res_release, &cres, NULL, NULL, 0, 0, 0};
    lres = (VjoVocrLmResident){heap_alloc, lres_release, &lres, NULL, NULL, 0, 0, 0};
    while (fgets(line, sizeof(line), stdin)) {
        char id[256];
        unsigned w, h, psp = 0;
        int fields = sscanf(line, "%255s %u %u %u", id, &w, &h, &psp);
        if (fields < 3 || !w || !h || w > VJO_OCR_MAX_WIDTH || h > VJO_OCR_MAX_HEIGHT) {
            fprintf(stderr, "bad job header: %s", line);
            return 2;
        }
        unsigned char *px = malloc((size_t)w * h * 4);
        if (!px || fread(px, 4, (size_t)w * h, stdin) != (size_t)w * h) {
            fprintf(stderr, "short job %s\n", id);
            return 2;
        }
        VjoRgbaImage img = {px, w, h, (size_t)w * 4};
        VjoGreySource src;
        VjoVocrStats st, first;
        static Kept kept;
        size_t ws = 0, ws_first = 0;
        int switched = 0, rc;
        double t0 = now_s(), t_first = 0;
        Setup job = su;
        job.psp = su.psp || (fields == 4 && psp);
        vjo_grey_source_rgba(&img, &src);
        src.line_buffer = line_buffer;
        if (su.find_only) {
            find_only(&job, id, &src);
            free(px);
            continue;
        }
        memset(&first, 0, sizeof(first));
        if (su.mode == 1) {
            rc = run_one(&job, &plat, su.vertical, &src, &vres, 0, text, sizeof(text), raw, sizeof(raw), &st, &kept,
                         &ws);
        } else {
            rc = run_one(&job, &plat, su.model, &src, &res, su.mode == 2, text, sizeof(text), raw, sizeof(raw), &st,
                         &kept, &ws);
            if (rc == VJO_E_VERTICAL) { /* auto: the rows looked like vertical text; the worker reads the columns */
                switched = 1;
                first = st;
                ws_first = ws;
                t_first = now_s() - t0;
                rc = run_one(&job, &plat, su.vertical, &src, &vres, 0, text, sizeof(text), raw, sizeof(raw), &st,
                             &kept, &ws);
            }
        }
        double wall = now_s() - t0;
        /* as the worker returns it (game_ocr/main.c: normalised, the border rule on) and the Shell shows it */
        if (!rc) vjo_text_normalize_keep(text, sizeof(text), NULL, 0, NULL);
        VjoArena arena;
        vjo_arena_init(&arena, arena_mem, sizeof(arena_mem));
        const char *shown = rc ? "" : vjo_filter_lines(&arena, text, 1);
        printf("{\"id\": ");
        json_text(stdout, id);
#if HAVE_VERTICAL
        const unsigned columns = st.columns, rotate_skipped = st.rotate_skipped;
#else
        const unsigned columns = 0, rotate_skipped = 0;
#endif
        printf(", \"rc\": %d, \"columns\": %u, \"switched\": %d, \"w\": %u, \"h\": %u, \"text\": ", rc, columns,
               switched, w, h);
        json_text(stdout, shown ? shown : text);
        printf(", \"lines\": [");
        for (unsigned i = 0; i < kept.n; i++) {
            printf("%s{\"box\": [%u, %u, %u, %u], \"dark\": %u, \"flags\": %u, \"blank\": %u, \"text\": ",
                   i ? ", " : "",
                   kept.l[i].x, kept.l[i].y, kept.l[i].w, kept.l[i].h, kept.l[i].dark, kept.l[i].flags,
                   kept.l[i].blank);
            json_text(stdout, kept.l[i].text);
            putchar('}');
        }
        /* API 52 (#358): the box the game worker's job on this whole frame suggests (core/region_suggest.h) */
        uint16_t sug[4];
        if (rc || !vjo_vocr_suggest(&st, w, h, sug)) memset(sug, 0, sizeof(sug));
        printf("], \"suggest\": [%u, %u, %u, %u]", sug[0], sug[1], sug[2], sug[3]);
        printf(", \"found\": %u, \"truncated\": %u, \"recognized\": %u, \"frames\": %u, \"strips\": %u, "
               "\"rotate_skipped\": %u, \"find_us\": %u, \"recognize_us\": %u, \"wall_us\": %.0f, \"workspace\": %zu, "
               "\"first\": {\"found\": %u, \"strips\": %u, \"find_us\": %u, \"wall_us\": %.0f, \"workspace\": %zu}}\n",
               st.lines, st.truncated, st.recognized, st.frames, st.strips, rotate_skipped, (unsigned)st.find_us,
               (unsigned)st.recognize_us, 1e6 * wall, ws, first.lines, first.strips, (unsigned)first.find_us,
               1e6 * t_first, ws_first);
        fflush(stdout);
        free(px);
    }
    vjo_vocr_resident_drop(&res);
    vjo_vocr_resident_drop(&vres);
    vjo_vocr_resident_drop(&cres);
    vjo_vocr_lm_drop(&lres);
    return 0;
usage:
    fprintf(stderr, "usage: %s MODEL_DIR [--tier 1-3] [--lm 0-4] [--cascade on|off] [--psp] [--vertical off|on|auto] "
                    "[--line-fix off|on|junk|all] "
                    "[--helpers 0-2] [--no-lines] [--find-only] [--head-gate on|off] < JOBS\n", argv[0]);
    return 2;
}
