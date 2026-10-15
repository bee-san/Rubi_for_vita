/* vita-vn-ocr backend (vocr_ocr.h): region strips -> vocr_find_lines_rows ->
 * verified weights (resident between jobs, or one-shot) -> vocr_run per line,
 * straight from the strip that holds it. No copy of the whole region is made.
 * The runtime (vocr.c, vocr_lines.c, vocr_lines_rows.c) is bee-san/vita-vn-ocr's,
 * at the commit cmake/VocrOCR.cmake pins. */
#include "vocr_ocr.h"
#include "../include/vjo_vocr_sizes.h"

#include <bearssl_hash.h>
#include <string.h>

#include "fbgrey.h"
#include "vocr.h"
#include "vocr_lines.h"
#include "vocr_lines_rows.h"
#include "vocr_lines_refine.h"
#include "vocr_lm.h"

typedef char vjo_vocr_max_lines_matches_runtime[VJO_VOCR_MAX_LINES == VOCR_LINES_MAX ? 1 : -1];
typedef char vjo_vocr_refine_matches_runtime[VJO_VOCR_REFINE_ALL == VOCR_REFINE_DEFAULT ? 1 : -1];
/* The int8 assets of github.com/bee-san/vita-vn-ocr releases v0.2.5-vocr3.1
 * (VOCR3, the default and its fallback) and v0.2.0 (docs/vita-vn-ocr.md).
 * Arena bytes: vocr_arena_bytes() in W8I mode with the default 4-frame tiles
 * and 16-frame head batches, any line width. */
#ifdef VJO_VOCR_TEST_MODELS
/* Host lifecycle tests only (host/tests/test_game_resident.c): synthetic pins. */
#include VJO_VOCR_TEST_MODELS
#else
static const VjoVocrModel models[] = {
    /* VOCR3 (vita-vn-ocr #67): H15's and FL10's shapes retrained, the same sizes and arenas. */
    {VJO_VOCR3_H15_NAME, VJO_VOCR_H15_BYTES,
     {0xcb,0x3c,0x61,0xa8,0xf9,0x1f,0x9a,0xee,0xb3,0xef,0xcc,0x0e,0x79,0x31,0x3f,0x66,
      0x41,0x12,0xf2,0x5c,0x4b,0xd5,0x58,0x65,0xba,0xe9,0xdf,0x52,0x52,0x2b,0x36,0x0c},
     182944, VOCR_LINES_GRAD},
    {VJO_VOCR3_FL10_NAME, VJO_VOCR_FL10_BYTES,
     {0x87,0xfb,0x4d,0x0c,0x18,0x41,0x6f,0x5f,0x37,0x71,0xce,0xcd,0x27,0x4c,0x8d,0xb3,
      0xcf,0x69,0xb8,0xb4,0xa4,0xee,0x9f,0x20,0x4e,0xde,0xbb,0x4b,0x82,0x07,0x91,0x54},
     124832, VOCR_LINES_GRAD},
    /* v0.2.0, the earlier defaults: kept as a fallback (README 'Settings'). */
    {"H15_w8.vocr", VJO_VOCR_H15_BYTES,
     {0x31,0x6e,0xbc,0x37,0x7e,0xc3,0x45,0x20,0x44,0x09,0x91,0xc5,0x08,0x51,0x99,0xd8,
      0xe7,0x40,0x31,0xf0,0x1a,0x86,0x79,0x38,0xbe,0x90,0xa6,0x0c,0x1e,0x3f,0x03,0xcd},
     182944, VOCR_LINES_GRAD},
    {"FL10_w8.vocr", VJO_VOCR_FL10_BYTES,
     {0x50,0x0e,0x0b,0x84,0x2e,0x5f,0x0c,0xb3,0x26,0xf5,0x5e,0x7d,0x99,0x1a,0x52,0xab,
      0x66,0xdd,0x24,0x97,0x7c,0xcc,0x89,0xbd,0x18,0x82,0x28,0xae,0xdc,0x82,0x5e,0x84},
     124832, VOCR_LINES_GRAD},
    {"F20_w8.vocr", VJO_VOCR_F20_BYTES,
     {0xf8,0xc1,0xab,0xe4,0x74,0x3a,0x71,0xa7,0x42,0xe7,0x18,0x52,0x3f,0x42,0x06,0xf9,
      0x92,0xd3,0xbd,0x4f,0xb6,0x9d,0xe3,0x21,0x99,0x7b,0x2b,0xd6,0xf1,0x67,0x79,0xc0},
     241056, VOCR_LINES_GRAD},
    /* VOCR4-W20 (vita-vn-ocr v0.2.5-vocr4.1, F20's shape): the confidence
     * cascade's larger model; not qualified as a default (docs/vita-vn-ocr.md). */
    {VJO_VOCR4_W20_NAME, VJO_VOCR_CASCADE_BYTES,
     {0x27,0x45,0xe6,0x96,0x99,0xad,0x7b,0x2a,0xba,0x30,0x92,0xf8,0x3e,0x5d,0xdc,0x19,
      0x12,0xf4,0x05,0xf5,0x4b,0x9d,0xf8,0xa2,0xe5,0x5b,0xe8,0x2c,0x38,0x5b,0x71,0x6e},
     241056, VOCR_LINES_GRAD},
};
#endif /* VJO_VOCR_TEST_MODELS */
#define N_MODELS (sizeof(models) / sizeof(models[0]))

/* The LM tiers of bee-san/vita-vn-ocr pre-release v0.2.6-ctclm.2 (CC BY-SA 4.0,
 * ATTRIBUTION.md ships with them), and the per-character bonus each was tuned
 * with (docs/ctc-lm-decode.md, "settings"). */
static const VjoVocrLm lms[VJO_VOCR_LM_TIERS] = {
    {VJO_VOCR_LM1_NAME, VJO_VOCR_LM1_BYTES,
     {0x38,0xea,0x94,0xe0,0xc4,0xec,0x93,0x86,0x34,0x54,0x38,0x7e,0x7c,0x2c,0x46,0x9d,
      0x20,0x9a,0xaf,0x21,0xf2,0xf6,0xee,0x5d,0x04,0x0e,0x75,0xa0,0x9d,0x00,0xd2,0x86}, 3000, 1},
    {VJO_VOCR_LM2_NAME, VJO_VOCR_LM2_BYTES,
     {0x99,0x30,0x00,0x61,0x15,0x55,0x17,0x1d,0xaf,0x63,0x61,0xe2,0x9a,0x50,0x3b,0x08,
      0x71,0xa5,0xf3,0xdd,0x24,0x0a,0x19,0x55,0x5d,0x55,0xf1,0x6a,0xaa,0x3a,0xb3,0xd9}, 3500, 2},
    {VJO_VOCR_LM3_NAME, VJO_VOCR_LM3_BYTES,
     {0xac,0x33,0xe4,0x61,0xbd,0x34,0x66,0x17,0xaa,0xe2,0x71,0x68,0xae,0xe4,0x72,0x30,
      0xa7,0x58,0xf4,0xb3,0x01,0xd7,0x8b,0x59,0xeb,0x55,0xca,0x25,0xa2,0x1b,0xcb,0x34}, 3000, 3},
    {VJO_VOCR_LM4_NAME, VJO_VOCR_LM4_BYTES,
     {0x01,0xf4,0x9b,0xe1,0x69,0x1a,0x1a,0xab,0x5a,0x6d,0x33,0x41,0x4c,0xe7,0xd1,0x7c,
      0x3e,0x5b,0x2d,0xaf,0x97,0xdf,0xcc,0xb0,0xd3,0x41,0xa3,0x80,0xa6,0x8e,0x58,0x98}, 3500, 4},
};

const VjoVocrLm *vjo_vocr_lm(const char *name)
{
    if (!name) return NULL;
    for (unsigned i = 0; i < VJO_VOCR_LM_TIERS; i++)
        if (!strcmp(name, lms[i].name)) return &lms[i];
    return NULL;
}

const VjoVocrLm *vjo_vocr_lm_tier(unsigned tier)
{
    return tier >= 1 && tier <= VJO_VOCR_LM_TIERS ? &lms[tier - 1] : NULL;
}

/* The decoder settings for an LM (docs/ctc-lm-decode.md, the same for every model). */
static void lm_opts(const VjoVocrLm *lm, vocr_lm_opts *o)
{
    memset(o, 0, sizeof(*o));
    o->beam = 8;
    o->topk = VJO_VOCR_LM_TOPK;
    o->max_frames = 0;  /* the model's clamp: 1,024 px / 8 */
    o->alpha = 500;
    o->beta = lm->beta;
    o->gate = 8000;
    o->flags = VOCR_LM_EOS;
    o->bracket = 8000;
}

#define ALIGN16(x) (((x) + (size_t)15) & ~(size_t)15)
/* Weights are read and hashed in pieces of this size: the game worker's kernel
 * broker stages exactly one piece per request (VJO_GAME_OCR_MODEL_CHUNK). */
#define READ_CHUNK 32768u

const VjoVocrModel *vjo_vocr_models(unsigned *count)
{
    if (count) *count = (unsigned)N_MODELS;
    return models;
}

const VjoVocrModel *vjo_vocr_model(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < N_MODELS; i++)
        if (!strcmp(name, models[i].name)) return &models[i];
    return NULL;
}

/* Job workspace: [finder, then recognizer arena | vocr_model, vocr_ctx]. The
 * one-shot form puts the weights in front of it. The region's pixels are not in
 * it: they live one strip at a time in the capture block. */
typedef struct {
    size_t scratch_off, structs_off, total;
    size_t scratch, finder;
    int lm;               /* the LM extras (top-K, decoder) follow at total (lm_layout) */
    int cascade;          /* and after them the cascade's (cascade_layout) */
    size_t refine_off, refine_bytes; /* the refinement's arena, inside total (0 bytes: off) */
} Layout;

static int region_ok(unsigned w, unsigned h)
{
    return w && h && w <= VJO_OCR_MAX_WIDTH && h <= VJO_OCR_MAX_HEIGHT;
}

static int layout(const VjoVocrModel *m, unsigned w, unsigned h, Layout *l)
{
    if (!m || !region_ok(w, h)) return -1;
    l->finder = vocr_lines_arena_bytes(w, h);
    l->scratch = ALIGN16(l->finder > m->arena_bytes ? l->finder : m->arena_bytes);
    l->scratch_off = 0;
    l->structs_off = l->scratch;
    l->total = l->structs_off + ALIGN16(sizeof(vocr_model)) + ALIGN16(sizeof(vocr_ctx));
    l->lm = l->cascade = 0;
    l->refine_off = l->refine_bytes = 0;
    return 0;
}

/* The LM extras, after the job's workspace (VjoVocrPolicy.lm): the top-K of
 * every frame of a line, the line's greedy text, the decoder's structs and its
 * workspace. */
#define LM_LINE_TEXT (4u * VJO_VOCR_LM_FRAMES + 16u) /* a line's greedy text: a class a frame, 4 bytes each */
typedef struct {
    size_t topk_logit, topk_class, blank, line, lm, ctx, ws, total;
} LmLayout;

static void lm_layout(LmLayout *x)
{
    size_t off = 0;
    x->topk_logit = off; off += ALIGN16((size_t)VJO_VOCR_LM_FRAMES * VJO_VOCR_LM_TOPK * sizeof(float));
    x->topk_class = off; off += ALIGN16((size_t)VJO_VOCR_LM_FRAMES * VJO_VOCR_LM_TOPK * sizeof(uint16_t));
    x->blank = off;      off += ALIGN16((size_t)VJO_VOCR_LM_FRAMES * sizeof(float));
    x->line = off;       off += ALIGN16(LM_LINE_TEXT);
    x->lm = off;         off += ALIGN16(sizeof(vocr_lm));
    x->ctx = off;        off += ALIGN16(sizeof(vocr_lm_ctx));
    x->ws = off;         off += VJO_VOCR_LM_WS_BYTES;
    x->total = off;
}

size_t vjo_vocr_lm_job_bytes(const char *model)
{
    LmLayout x;
    if (!vjo_vocr_model(model)) return 0;
    lm_layout(&x);
    return x.total;
}

/* The cascade's part, after the LM extras: the larger model's arena, its
 * runtime structs, its LM decoder (the LM itself is parsed once, in the LM
 * extras), and its line's greedy and decoded text. */
typedef struct {
    size_t arena, model, ctx, lm_ctx, lm_ws, greedy, text, total;
} CascadeLayout;

static void cascade_layout(const VjoVocrModel *c, CascadeLayout *x)
{
    size_t off = 0;
    x->arena = off;  off += ALIGN16(c->arena_bytes);
    x->model = off;  off += ALIGN16(sizeof(vocr_model));
    x->ctx = off;    off += ALIGN16(sizeof(vocr_ctx));
    x->lm_ctx = off; off += ALIGN16(sizeof(vocr_lm_ctx));
    x->lm_ws = off;  off += VJO_VOCR_LM_WS_BYTES;
    x->greedy = off; off += ALIGN16(LM_LINE_TEXT);
    x->text = off;   off += ALIGN16(LM_LINE_TEXT);
    x->total = off;
}

size_t vjo_vocr_refine_bytes(unsigned width, unsigned height)
{
    return region_ok(width, height) ? ALIGN16(vocr_refine_arena_bytes(width, height)) : 0;
}

size_t vjo_vocr_cascade_job_bytes(const char *cascade)
{
    const VjoVocrModel *c = vjo_vocr_model(cascade);
    CascadeLayout x;
    if (!c) return 0;
    cascade_layout(c, &x);
    return x.total;
}

/* A shape's estimated recognition cost: ms for a 640-px line on the Cortex-A9
 * at 444 MHz, the low end of vita-vn-ocr's estimates (v0.2.5-vocr4.1 release
 * notes: FL10 94, H15 166, W20 235). Only their ratio is used: the cascade's
 * time per line from the job's own measured time per line. */
static unsigned model_cost(const VjoVocrModel *m)
{
    return m->bytes >= VJO_VOCR_F20_BYTES ? 235u : m->bytes >= VJO_VOCR_H15_BYTES ? 166u : 94u;
}

size_t vjo_vocr_job_bytes(const char *model, unsigned width, unsigned height)
{
    Layout l;
    return layout(vjo_vocr_model(model), width, height, &l) < 0 ? 0 : l.total;
}

size_t vjo_vocr_workspace_bytes(const char *model, unsigned width, unsigned height)
{
    const VjoVocrModel *m = vjo_vocr_model(model);
    Layout l;
    return layout(m, width, height, &l) < 0 ? 0 : ALIGN16(m->bytes) + l.total;
}

#define ALIGN4(x) (((x) + (size_t)3) & ~(size_t)3)

unsigned vjo_vocr_strip_rows(unsigned width, unsigned height)
{
    unsigned rows;
    if (!region_ok(width, height)) return 0;
    rows = VJO_VOCR_STRIP_BYTES / width;
    if (rows > VJO_VOCR_STRIP_MAX_ROWS) rows = VJO_VOCR_STRIP_MAX_ROWS;
    if (rows > height) rows = height;
    return rows ? rows : 1u;
}

/* Capture block: [strip: rows x width | sums: 4 x height | ring: slots x 2 x
 * height | seen, cycled: 1 bit per row each]. slots: as many earlier states
 * as fit. */
#define BIT_BYTES(h) (((size_t)(h) + 7u) / 8u)
static size_t capture_fixed(unsigned width, unsigned height, unsigned rows)
{
    return ALIGN4((size_t)rows * width) + 4u * (size_t)height + ALIGN4(2u * BIT_BYTES(height));
}

unsigned vjo_vocr_state_slots(unsigned width, unsigned height)
{
    unsigned rows = vjo_vocr_strip_rows(width, height), slots;
    size_t fixed, room;
    if (!rows) return 0;
    fixed = capture_fixed(width, height, rows);
    room = fixed < VJO_VOCR_CAPTURE_LIMIT ? VJO_VOCR_CAPTURE_LIMIT - fixed : 0;
    slots = (unsigned)(room / (2u * (size_t)height));
    while (slots > 2u && ALIGN4((size_t)slots * 2u * height) > room) slots--;
    return slots < 2u ? 2u : slots > VJO_VOCR_STATES_MAX ? VJO_VOCR_STATES_MAX : slots;
}

size_t vjo_vocr_capture_bytes(unsigned width, unsigned height)
{
    unsigned rows = vjo_vocr_strip_rows(width, height);
    if (!rows) return 0;
    return capture_fixed(width, height, rows) + ALIGN4((size_t)vjo_vocr_state_slots(width, height) * 2u * height);
}

static int cancelled(const VjoGreySource *im)
{
    return im->cancelled && im->cancelled(im->ud);
}

static int run_cancel(void *user)
{
    return cancelled((const VjoGreySource *)user);
}

static uint32_t clock_us(const VjoPlatform *p)
{
    return p->now_us ? (uint32_t)p->now_us(p->ud) : 0;
}

/* The region, a strip at a time, for the finder and the recognizer. Every row
 * keeps the checksum it had when first read; a later read that differs means
 * the screen changed under us (err = VJO_E_SCREEN_CHANGED), unless the row went
 * back to one of its recent states (an animated icon; cycle_cap rows at most:
 * new text, or a box that clears, changes more rows than an icon). While
 * settling, a change is only noted (changed). */
typedef struct {
    const VjoGreySource *src;
    const VjoPlatform *p;
    const VjoVocrPolicy *pol; /* never NULL: a zeroed one is API 19's job */
    VjoVocrStats *st;
    unsigned char *strip;   /* rows [y0, y0 + rows) of the region, width bytes each */
    uint32_t *sums;         /* per region row, valid where seen */
    uint16_t *ring;         /* per region row, slots earlier states (folded), the latest first */
    unsigned char *seen, *cyc; /* 1 bit per region row each; cyc: it went back to an earlier state */
    unsigned w, h, strip_rows, y0, rows, slots;
    unsigned first_next;    /* the first pass's next row (h once it is in) */
    unsigned cycle_cap;     /* rows that may go back to an earlier state without being a change */
    int learn;              /* the stable-text wait is on: such rows are learned */
    int settling, changed;  /* a pass that waits: changes are noted, not errors */
    int retract;            /* the next read from row 0 starts a first pass it does not finish */
    int published;          /* a first pass of this job has ended (the Shell was told of it) */
    int held;               /* the rows have read the same since pass_from (a wait ended settled) */
    int verified;           /* verified_from is set */
    uint32_t pass_from;     /* since when the rows have read the same (this try's first read, or the wait's) */
    uint32_t verified_from; /* start of the latest re-read of every row (a whole pass, in order) */
    uint32_t cover_from;    /* a re-read from row 0 in progress: its start, and its next row (0: none) */
    unsigned cover_next;
    uint32_t settle_left;   /* the job's waiting budget left */
    int err;
} Stream;

static void bits_clear(Stream *s)
{
    memset(s->seen, 0, 2u * BIT_BYTES(s->h));
}

static uint16_t fold16(uint32_t sum)
{
    return (uint16_t)(sum ^ (sum >> 16));
}

/* Checks (or records) the checksum of region row y, whose grey bytes are at p. */
static void stream_check(Stream *s, unsigned y, const unsigned char *p, int first)
{
    uint32_t sum = vjo_fnv1a(VJO_FNV_INIT, p, s->w);
    unsigned i = y >> 3, k, slots = s->slots;
    unsigned char bit = (unsigned char)(1u << (y & 7u));
    uint16_t *ring = s->ring + (size_t)y * slots, now = fold16(sum);
    if (first) s->st->region_sum = vjo_fnv1a(s->st->region_sum, (const unsigned char *)&sum, 4);
    if (!(s->seen[i] & bit)) {
        s->seen[i] |= bit;
        s->sums[y] = sum;
        for (k = 0; k < slots; k++) ring[k] = now;
        return;
    }
    if (s->sums[y] == sum) return;
    /* A state folding to the current one's 16 bits is a new one (the sums
     * differ), whatever copies of that fold the ring holds. */
    k = now == fold16(s->sums[y]) ? slots : 0;
    while (k < slots && ring[k] != now) k++;
    /* Back to a recent state: an animated icon, not text (text being typed
     * only ever moves on to new states). With the stable-text wait on, up to
     * cycle_cap rows of a job are taken for such rows; a page of new text,
     * or a box of several lines clearing back to what it showed before,
     * changes more rows than that. Without the wait every change is one, as
     * in API 19. */
    if (k < slots && ((s->cyc[i] & bit) || (s->learn && s->st->cycling_rows < s->cycle_cap))) {
        if (!(s->cyc[i] & bit)) {
            s->cyc[i] |= bit;
            s->st->cycling_rows++;
        }
        memmove(ring + 1, ring, k * sizeof(*ring));
        ring[0] = fold16(s->sums[y]);
        s->sums[y] = sum;
        s->st->cycles++;
        return;
    }
    if (k == slots) k = slots - 1; /* a new state: the oldest leaves */
    memmove(ring + 1, ring, k * sizeof(*ring));
    ring[0] = fold16(s->sums[y]);
    s->sums[y] = sum;
    if (s->settling) {
        s->st->settle_changes++;
        s->changed = 1;
        return;
    }
    s->st->tears++;
    s->err = VJO_E_SCREEN_CHANGED;
}

/* Rows [y, y + n) through the source into dst (stride w), checked. */
static int stream_read(Stream *s, unsigned y, unsigned n, unsigned char *dst)
{
    unsigned flags = 0;
    uint32_t t0;
    int got, first = 0;
    if (s->err) return -1;
    if (cancelled(s->src)) { s->err = VJO_E_CANCELLED; return -1; }
    if (s->retract && y == 0) {
        flags = VJO_STRIP_FIRST; /* a first pass begun and left: the earlier capture is void */
    } else if (!s->settling && y == s->first_next) {
        flags = VJO_STRIP_FIRST | (y + n == s->h ? VJO_STRIP_LAST : 0u);
        first = 1;
    }
    t0 = clock_us(s->p);
    got = s->src->strip(s->src->ud, y, n, dst, s->w, flags);
    s->st->capture_us += clock_us(s->p) - t0;
    s->st->strips++;
    if (got != (int)n) {
        s->err = got < 0 ? got : VJO_E_SOURCE;
        return -1;
    }
    if (flags == VJO_STRIP_FIRST && !first) s->retract = 0;
    if (first) {
        if (!y && !s->held) s->pass_from = t0;
        s->first_next = y + n;
        if (flags & VJO_STRIP_LAST) s->published = 1;
    } else {
        /* Reads from row 0 that follow each other to the last row re-read every
         * row since the first of them (the finder's second pass, a wait's pass). */
        if (!y) {
            s->cover_from = t0;
            s->cover_next = n;
        } else if (s->cover_next && y <= s->cover_next) {
            if (y + n > s->cover_next) s->cover_next = y + n;
        } else {
            s->cover_next = 0;
        }
    }
    for (unsigned r = 0; r < n; r++) stream_check(s, y + r, dst + (size_t)r * s->w, first);
    if (!s->err && !first && s->cover_next >= s->h) {
        s->verified_from = s->cover_from;
        s->verified = 1;
        s->cover_next = 0;
    }
    return s->err ? -1 : 0;
}

/* The strip from row y: during the first pass the strips follow each other
 * from row 0 (the kernel builds the capture's scene signature from them, and
 * the last one publishes it), otherwise it is moved up to stay inside the region. */
static int stream_fetch(Stream *s, unsigned y)
{
    unsigned n = s->strip_rows;
    if (y == s->first_next && s->first_next < s->h) {
        if (n > s->h - y) n = s->h - y;
    } else if (y + n > s->h) {
        y = s->h - n;
    }
    s->rows = 0;
    if (stream_read(s, y, n, s->strip) < 0) return -1;
    s->y0 = y;
    s->rows = n;
    return 0;
}

/* One whole pass over the region from row 0, a strip at a time, outside the
 * first pass: it re-reads (verifies) every row (stream_read records it). */
static int stream_pass(Stream *s)
{
    for (unsigned y = 0; y < s->h; y += s->strip_rows)
        if (stream_fetch(s, y) < 0) return -1;
    return 0;
}

static int settle_on(const Stream *s)
{
    return s->pol->settle_us && s->p->now_us && s->settle_left;
}

/* Spends us of the job's waiting budget; over: it ran out before the rows held
 * still, so the job goes on without waiting (now and on later tries). */
static void settle_spend(Stream *s, uint32_t us, int over)
{
    s->st->settle_us += us;
    s->settle_left = !over && us < s->settle_left ? s->settle_left - us : 0;
    if (over) s->st->settle_timeout = 1;
}

/* Waits for the rows to hold still: passes that only read the region, until
 * one that saw no change started settle_us after the last pass that did (or the
 * wait's first). A job that already told the Shell of a capture first voids it
 * (retract), so the Shell does not take the screen it read before for this
 * job's. Settled, the rows have read the same since pass_from. */
static int settle_wait(Stream *s)
{
    uint32_t t0 = clock_us(s->p), stable_since = t0;
    int done = 0, over = 0;
    s->settling = 1;
    s->retract = s->published;
    while (!done) {
        uint32_t ps = clock_us(s->p);
        if (ps - t0 >= s->settle_left || s->st->strips >= VJO_VOCR_SETTLE_READS) { over = 1; break; }
        s->changed = 0;
        if (stream_pass(s) < 0) break;
        s->st->settle_passes++;
        if (s->changed) stable_since = ps;
        else if (ps - stable_since >= s->pol->settle_us) done = 1;
    }
    s->settling = 0;
    if (done) {
        s->pass_from = stable_since;
        s->held = 1;
        s->st->settled = 1;
    }
    settle_spend(s, clock_us(s->p) - t0, over);
    return s->err ? -1 : 0;
}

/* After the finder: re-reads the region until a whole pass started settle_us
 * after the rows were first read the same (pass_from). A change is a tear: the
 * lines found are not the screen's any more. Once one re-read has matched, a
 * pass that would start too early is not read: the job sleeps until it is due
 * (with a sleep callback). */
static int settle_verify(Stream *s)
{
    uint32_t t0 = clock_us(s->p);
    int over = 0;
    while (!s->verified || s->verified_from - s->pass_from < s->pol->settle_us) {
        uint32_t now = clock_us(s->p), due = s->pass_from + s->pol->settle_us;
        if (now - t0 >= s->settle_left || s->st->strips >= VJO_VOCR_SETTLE_READS) { over = 1; break; }
        if (s->verified && s->pol->sleep && (int32_t)(due - now) > 0) {
            uint32_t nap = due - now, left = s->settle_left - (now - t0);
            s->pol->sleep(nap < left ? nap : left);
            if (cancelled(s->src)) { s->err = VJO_E_CANCELLED; break; }
        }
        if (stream_pass(s) < 0) break;
        s->st->settle_passes++;
    }
    if (!s->err && !over) s->st->settled = 1;
    settle_spend(s, clock_us(s->p) - t0, over);
    return s->err ? -1 : 0;
}

static const unsigned char *stream_row(void *user, unsigned y)
{
    Stream *s = (Stream *)user;
    if (s->err || y >= s->h) return NULL;
    if (!s->rows || y < s->y0 || y >= s->y0 + s->rows) {
        /* rows run in order within a pass: the strip starts at the row asked for,
         * except during the first pass, whose strips must follow each other */
        if (stream_fetch(s, y) < 0) return NULL;
    }
    return s->strip + (size_t)(y - s->y0) * s->w;
}

/* rc for a finder or row failure: the source's own error first. */
static int stream_error(const Stream *s, int fallback)
{
    return s->err ? s->err : fallback;
}

int vjo_vocr_resident_drop(VjoVocrResident *r)
{
    if (!r) return 0;
    r->model = NULL;
    r->size = r->stamp = 0;
    if (!r->data) return 0;
    if (r->release && r->release(r->ud)) return -1;
    r->data = NULL;
    r->capacity = 0;
    return 0;
}

/* rc, after releasing the resident memory; VJO_E_OCR_UNAVAILABLE if that failed. */
static int drop_with(VjoVocrResident *r, int rc)
{
    return vjo_vocr_resident_drop(r) ? VJO_E_OCR_UNAVAILABLE : rc;
}

/* Reads the whole open file (bytes long) into dst and checks its SHA-256 while
 * reading; cancellation is checked between pieces. */
static int read_verified(const VjoFile *file, uint32_t bytes, const unsigned char *sha256, unsigned char *dst,
                         const VjoGreySource *im)
{
    br_sha256_context hash;
    unsigned char digest[32];
    br_sha256_init(&hash);
    for (size_t off = 0; off < bytes;) {
        size_t n = bytes - off < READ_CHUNK ? bytes - off : READ_CHUNK;
        if (cancelled(im)) return VJO_E_CANCELLED;
        if (file->read(file->ctx, off, dst + off, n) < 0)
            return cancelled(im) ? VJO_E_CANCELLED : VJO_E_OCR_MODEL;
        br_sha256_update(&hash, dst + off, n);
        off += n;
    }
    br_sha256_out(&hash, digest);
    return memcmp(digest, sha256, sizeof(digest)) ? VJO_E_OCR_MODEL : VJO_OK;
}

/* dir/name into path; 0, or -1 when it does not fit. */
static int join_path(char *path, size_t cap, const char *dir, const char *name)
{
    size_t dn = strlen(dir), fn = strlen(name);
    if (!dn || dn + 1 + fn >= cap) return -1;
    memcpy(path, dir, dn);
    path[dn] = '/';
    memcpy(path + dn + 1, name, fn + 1);
    return 0;
}

int vjo_vocr_lm_drop(VjoVocrLmResident *r)
{
    if (!r) return 0;
    r->lm = NULL;
    r->size = r->stamp = 0;
    if (!r->data) return 0;
    if (r->release && r->release(r->ud)) return -1;
    r->data = NULL;
    r->capacity = 0;
    return 0;
}

static int lm_drop_with(VjoVocrLmResident *r, int rc)
{
    return vjo_vocr_lm_drop(r) ? VJO_E_OCR_UNAVAILABLE : rc;
}

/* Verified bytes of the LM lm in r->data, as resident_weights does for the
 * recognizer: opened every job, read and hashed again only when its size or
 * stamp changed (or none is held). */
static int resident_lm(const VjoPlatform *p, const char *dir, const VjoVocrLm *lm, VjoVocrLmResident *r,
                       const VjoGreySource *im, VjoVocrStats *st)
{
    char path[256];
    VjoFile file;
    int rc;
    if (r->lm != lm && vjo_vocr_lm_drop(r)) return VJO_E_OCR_UNAVAILABLE; /* another tier's: never two */
    if (join_path(path, sizeof(path), dir, lm->name) < 0) return lm_drop_with(r, VJO_E_OCR_MODEL);
    memset(&file, 0, sizeof(file));
    if (!p->file_open || p->file_open(p->ud, path, &file) < 0)
        return cancelled(im) ? VJO_E_CANCELLED : lm_drop_with(r, VJO_E_OCR_MODEL);
    if (file.size != lm->bytes || !file.read || !file.close) {
        if (file.close) file.close(file.ctx);
        return lm_drop_with(r, VJO_E_OCR_MODEL);
    }
    if (r->lm == lm && r->data && file.stamp && file.stamp == r->stamp && file.size == r->size) {
        file.close(file.ctx);
        st->lm_cached = 1;
        return VJO_OK;
    }
    r->lm = NULL;
    if (!r->data) {
        unsigned char *data = r->alloc ? (unsigned char *)r->alloc(r->ud, lm->bytes) : NULL;
        if (!data) {
            file.close(file.ctx);
            return VJO_E_OOM;
        }
        r->data = data;
        r->capacity = lm->bytes;
        if ((uintptr_t)data & 15u) {
            file.close(file.ctx);
            return lm_drop_with(r, VJO_E_OOM);
        }
    }
    rc = read_verified(&file, lm->bytes, lm->sha256, r->data, im);
    file.close(file.ctx);
    if (rc) return lm_drop_with(r, rc); /* partly read: memory is held only for verified bytes */
    r->lm = lm;
    r->size = file.size;
    r->stamp = file.stamp;
    st->lm_loaded = 1;
    return VJO_OK;
}

/* Verified weights of m in r->data. The file is always opened (its size and
 * stamp decide); it is read and hashed again only if they changed, the stamp
 * is unknown, or no verified copy is held. */
static int resident_weights(const VjoPlatform *p, const char *dir, const VjoVocrModel *m,
                            VjoVocrResident *r, const VjoGreySource *im, unsigned *loaded, unsigned *cached)
{
    char path[256];
    VjoFile file;
    int rc;
    /* Another model's weights: never hold two. */
    if (r->model != m && vjo_vocr_resident_drop(r)) return VJO_E_OCR_UNAVAILABLE;
    if (join_path(path, sizeof(path), dir, m->name) < 0) return drop_with(r, VJO_E_OCR_MODEL);
    memset(&file, 0, sizeof(file));
    if (!p->file_open || p->file_open(p->ud, path, &file) < 0)
        /* A cancelled check proves nothing about the file: keep verified weights. */
        return cancelled(im) ? VJO_E_CANCELLED : drop_with(r, VJO_E_OCR_MODEL);
    if (file.size != m->bytes || !file.read || !file.close) {
        if (file.close) file.close(file.ctx);
        return drop_with(r, VJO_E_OCR_MODEL);
    }
    if (r->model == m && r->data && file.stamp && file.stamp == r->stamp && file.size == r->size) {
        file.close(file.ctx);
        *cached = 1;
        return VJO_OK;
    }
    /* From here until the hash matches, the memory holds no weights. */
    r->model = NULL;
    if (!r->data) {
        unsigned char *data = r->alloc ? (unsigned char *)r->alloc(r->ud, m->bytes) : NULL;
        if (!data) {
            file.close(file.ctx);
            return VJO_E_OOM;
        }
        r->data = data;
        r->capacity = m->bytes;
        if ((uintptr_t)data & 15u) {
            file.close(file.ctx);
            return drop_with(r, VJO_E_OOM);
        }
    }
    rc = read_verified(&file, m->bytes, m->sha256, r->data, im);
    file.close(file.ctx);
    if (rc) return drop_with(r, rc);
    r->model = m;
    r->size = file.size;
    r->stamp = file.stamp;
    *loaded = 1;
    return VJO_OK;
}

static int run_error(int rc)
{
    switch (rc) {
    case VOCR_E_TEXT: return VJO_E_TOO_LARGE;
    case VOCR_E_CANCELLED: return VJO_E_CANCELLED;
    default: return VJO_E_OCR_INFERENCE;
    }
}

/* Line ln's grey pixels and their stride: from the strip (stride s->w), read
 * again from its top row if the strip does not hold the whole line; a line
 * taller than a strip goes through the strip a strip at a time (each row checked
 * as always) and only its own columns are kept, in the source's line buffer
 * (stride ln->w: ln->w x ln->h bytes). NULL: the line is left out (tall, no
 * buffer) or the read failed (s->err). */
static const unsigned char *line_pixels(Stream *s, const VocrLine *ln, size_t *stride)
{
    if (ln->h <= s->strip_rows) {
        if (!s->rows || ln->y < s->y0 || ln->y + ln->h > s->y0 + s->rows) {
            if (stream_fetch(s, ln->y) < 0) return NULL;
        }
        *stride = s->w;
        return s->strip + (size_t)(ln->y - s->y0) * s->w + ln->x;
    }
    {
        size_t bytes = (size_t)ln->w * ln->h;
        unsigned char *buf = bytes <= VJO_VOCR_LINE_MAX_BYTES && s->src->line_buffer
                             ? (unsigned char *)s->src->line_buffer(s->src->ud, bytes) : NULL;
        s->st->tall++;
        if (!buf) {
            s->st->tall_skipped++;
            return NULL;
        }
        if (bytes > s->st->line_bytes) s->st->line_bytes = bytes;
        for (unsigned y = 0; y < ln->h;) {
            unsigned n = ln->h - y < s->strip_rows ? ln->h - y : s->strip_rows;
            s->rows = 0;
            if (stream_read(s, ln->y + y, n, s->strip) < 0) return NULL;
            for (unsigned r = 0; r < n; r++)
                memcpy(buf + (size_t)(y + r) * ln->w, s->strip + (size_t)r * s->w + ln->x, ln->w);
            s->y0 = ln->y + y; /* the strip now holds these rows */
            s->rows = n;
            y += n;
        }
        *stride = ln->w;
        return buf;
    }
}

/* The weights resident and verified, the runtime's model parsed and its arena
 * planned in the scratch (the finder is done with it). */
static int load_model(const VjoPlatform *p, const char *dir, const VjoVocrModel *mi, const VjoGreySource *im,
                      VjoVocrResident *res, unsigned char *ws, const Layout *l, VjoVocrStats *st)
{
    vocr_model *model = (vocr_model *)(void *)(ws + l->structs_off);
    vocr_ctx *ctx = (vocr_ctx *)(void *)(ws + l->structs_off + ALIGN16(sizeof(vocr_model)));
    vocr_opts opts;
    uint32_t t0 = clock_us(p);
    int rc = resident_weights(p, dir, mi, res, im, &st->model_loaded, &st->model_cached);
    if (rc) return rc;
    /* Verified bytes the runtime refuses would fail every job: release them. */
    if (vocr_load(model, res->data, mi->bytes) != VOCR_OK) return drop_with(res, VJO_E_OCR_MODEL);
    memset(&opts, 0, sizeof(opts));
    opts.mode = VOCR_MODE_W8I;
    {
        size_t need = vocr_arena_bytes(model, &opts);
        if (!need || need > l->scratch) return drop_with(res, VJO_E_OCR_MODEL);
        if (vocr_init(ctx, model, &opts, ws + l->scratch_off, need) != VOCR_OK) return VJO_E_OCR_INFERENCE;
    }
    st->load_us = clock_us(p) - t0;
    return VJO_OK;
}

/* The job's LM decoder, once the model is parsed: the LM's bytes resident and
 * verified, parsed in place, the decoder planned in the LM extras at x0.
 * VJO_OK: ready. VJO_E_CANCELLED / VJO_E_OCR_UNAVAILABLE: the job stops. Any
 * other: the job reads greedily (st->lm_rc says why). */
static int lm_setup(const VjoPlatform *p, const char *dir, const VjoGreySource *im, const VjoVocrPolicy *pol,
                    unsigned char *x0, const vocr_model *model, VjoVocrStats *st)
{
    LmLayout x;
    vocr_lm_opts o;
    vocr_lm *lm;
    vocr_lm_ctx *dc;
    size_t need;
    uint32_t t0 = clock_us(p);
    int rc;
    lm_layout(&x);
    lm = (vocr_lm *)(void *)(x0 + x.lm);
    dc = (vocr_lm_ctx *)(void *)(x0 + x.ctx);
    if (!model->stride || model->max_width / model->stride + 1u > VJO_VOCR_LM_FRAMES) return st->lm_rc = VJO_E_OCR_MODEL;
    rc = resident_lm(p, dir, pol->lm, pol->lm_res, im, st);
    if (rc == VJO_E_CANCELLED || rc == VJO_E_OCR_UNAVAILABLE) return rc;
    if (rc) return st->lm_rc = rc;
    if (vocr_lm_load(lm, pol->lm_res->data, pol->lm->bytes) != VOCR_OK) {
        rc = lm_drop_with(pol->lm_res, VJO_E_OCR_MODEL); /* verified bytes the runtime refuses: every job would */
        return rc == VJO_E_OCR_UNAVAILABLE ? rc : (st->lm_rc = rc);
    }
    lm_opts(pol->lm, &o);
    need = vocr_lm_workspace_bytes(model, lm, &o);
    if (!need || need > VJO_VOCR_LM_WS_BYTES) return st->lm_rc = VJO_E_OOM;
    if (vocr_lm_init(dc, model, lm, &o, x0 + x.ws, need) != VOCR_OK) return st->lm_rc = VJO_E_OCR_INFERENCE;
    st->lm = pol->lm;
    st->lm_load_us = clock_us(p) - t0;
    return VJO_OK;
}

/* The cascade's larger model, at the first line it reads again: its weights
 * resident and verified (cascade_res), parsed, its arena planned in the
 * cascade extras at c0, and with the job's LM (parsed already, lm) a decoder
 * for it. VJO_OK: ready. VJO_E_CANCELLED / VJO_E_OCR_UNAVAILABLE: the job
 * stops. Any other: the job goes on without it (st->cascade_rc says why). */
static int cascade_setup(const VjoPlatform *p, const char *dir, const VjoGreySource *im, const VjoVocrPolicy *pol,
                         unsigned char *c0, const vocr_lm *lm, VjoVocrStats *st)
{
    const VjoVocrModel *c = pol->cascade;
    CascadeLayout x;
    vocr_model *model;
    vocr_opts opts;
    size_t need;
    uint32_t t0 = clock_us(p);
    int rc;
    cascade_layout(c, &x);
    model = (vocr_model *)(void *)(c0 + x.model);
    rc = resident_weights(p, dir, c, pol->cascade_res, im, &st->cascade_loaded, &st->cascade_cached);
    if (rc == VJO_E_CANCELLED || rc == VJO_E_OCR_UNAVAILABLE) return rc;
    if (rc) return st->cascade_rc = rc;
    if (vocr_load(model, pol->cascade_res->data, c->bytes) != VOCR_OK || !model->stride ||
        model->max_width / model->stride + 1u > VJO_VOCR_LM_FRAMES) {
        rc = drop_with(pol->cascade_res, VJO_E_OCR_MODEL); /* verified bytes the runtime refuses: every job would */
        return rc == VJO_E_OCR_UNAVAILABLE ? rc : (st->cascade_rc = rc);
    }
    memset(&opts, 0, sizeof(opts));
    opts.mode = VOCR_MODE_W8I;
    need = vocr_arena_bytes(model, &opts);
    if (!need || need > ALIGN16(c->arena_bytes)) {
        rc = drop_with(pol->cascade_res, VJO_E_OCR_MODEL);
        return rc == VJO_E_OCR_UNAVAILABLE ? rc : (st->cascade_rc = rc);
    }
    if (vocr_init((vocr_ctx *)(void *)(c0 + x.ctx), model, &opts, c0 + x.arena, need) != VOCR_OK)
        return st->cascade_rc = VJO_E_OCR_INFERENCE;
    if (lm) {
        vocr_lm_opts o;
        lm_opts(st->lm, &o);
        need = vocr_lm_workspace_bytes(model, lm, &o);
        if (!need || need > VJO_VOCR_LM_WS_BYTES) return st->cascade_rc = VJO_E_OOM;
        if (vocr_lm_init((vocr_lm_ctx *)(void *)(c0 + x.lm_ctx), model, lm, &o, c0 + x.lm_ws, need) != VOCR_OK)
            return st->cascade_rc = VJO_E_OCR_INFERENCE;
    }
    st->cascade = c;
    st->cascade_load_us += clock_us(p) - t0;
    return VJO_OK;
}

/* A line's confidence from the top-K's conf_min, in 1/1000; 1000 for a line
 * read as empty (no character to doubt). */
static uint16_t line_conf(const vocr_extra *e, const char *greedy)
{
    float c = e->conf_min * 1000.0f + 0.5f;
    if (!greedy[0]) return 1000u;
    return (uint16_t)(c <= 0.0f ? 0u : c >= 1000.0f ? 1000u : (unsigned)c);
}

/* A line's length in the recognizer's terms: its width at the input height of
 * 32 pixels, which its CTC frames (and so its time) follow. */
static uint32_t line_length(const VocrLine *ln)
{
    return (uint32_t)ln->w * 32u / (ln->h ? ln->h : 1u) + 1u;
}

/* One capture of the region: lines, then (weights) each line recognized. */
static int recognize(const VjoPlatform *p, const char *dir, const VjoVocrModel *mi,
                     const VjoGreySource *im, VjoVocrResident *res, unsigned char *ws, const Layout *l,
                     Stream *s, char *text, size_t cap, VjoVocrStats *st)
{
    const unsigned w = im->width;
    const VjoVocrPolicy *pol = s->pol;
    VocrLinesRows region = {w, im->height, VOCR_LINES_GREY, stream_row, s};
    VocrLineSet set;
    vocr_model *model = (vocr_model *)(void *)(ws + l->structs_off);
    vocr_ctx *ctx = (vocr_ctx *)(void *)(ws + l->structs_off + ALIGN16(sizeof(vocr_model)));
    unsigned char *x0 = ws + l->total; /* the LM extras (l->lm) */
    unsigned char *c0 = x0 + vjo_vocr_lm_job_bytes(mi->name); /* the cascade's (l->cascade) */
    LmLayout x;
    CascadeLayout cx;
    size_t used = 0, rused = 0;
    uint32_t t0, prim_us = 0, casc_ratio = 0, len_done = 0, len_left = 0;
    unsigned casc_conf = pol->cascade_conf ? pol->cascade_conf : VJO_VOCR_CASCADE_CONF;
    int rc, loaded = 0, lm_on = 0, casc_on = l->cascade, casc_ready = 0;

    lm_layout(&x);
    memset(&cx, 0, sizeof(cx));
    if (casc_on) {
        cascade_layout(pol->cascade, &cx);
        /* its time per line from the job's own, by the shapes' cost (then by what it measured) */
        casc_ratio = model_cost(pol->cascade) * 256u / model_cost(mi);
    }
    text[0] = '\0';
    if (pol->raw && pol->raw_cap) pol->raw[0] = '\0';
    st->lines = st->truncated = st->dark = st->frames = st->recognized = 0;
    st->blank = st->lm_lines = st->lm_changed = 0;
    st->lm_us = 0;
    if (l->lm) { /* this try's LM, not an earlier try's */
        st->lm = NULL;
        st->lm_rc = 0;
    }
    st->cascade_lines = st->cascade_changed = st->cascade_skipped = st->cascade_frames = st->cascade_blanked = 0;
    st->cascade_us = 0;
    st->refine_boxes = st->refine_lines = st->refine_split = st->refine_capped = 0;
    st->refine_rows = 0;
    st->refine_us = 0;
    if (l->refine_bytes) st->refine_rc = 0;
    if (l->cascade) {
        st->cascade = NULL;
        st->cascade_rc = 0;
    }
    t0 = clock_us(p);
    rc = vocr_find_lines_rows(&region, (int)mi->finder_mode, ws + l->scratch_off, l->finder, &set);
    if (rc == VOCR_LINES_E_SOURCE) return stream_error(s, VJO_E_SOURCE);
    if (rc) return rc == VOCR_LINES_E_ARENA ? VJO_E_OOM : VJO_E_OCR_REGION;
    /* A region inside one strip was read once: read it again, so that a screen
     * that changed while it was read is seen (the strips of a taller region are
     * all read twice by the finder's two passes already). */
    if (s->strip_rows >= im->height && stream_fetch(s, 0) < 0) return stream_error(s, VJO_E_SOURCE);
    st->find_us = clock_us(p) - t0;
    st->lines = set.count;
    st->truncated = set.truncated;
    for (unsigned i = 0; i < set.count; i++) {
        st->dark += set.lines[i].dark ? 1u : 0u;
        len_left += line_length(&set.lines[i]);
    }
    if (!set.count) return VJO_OK; /* nothing to read: the model file is not opened */
    if (cancelled(im)) return VJO_E_CANCELLED;
    /* Nothing is recognized before the rows have held still for settle_us. */
    if (settle_on(s) && settle_verify(s) < 0) return stream_error(s, VJO_E_SOURCE);

    /* With the blank-line check, the weights are read at the first line that
     * is not skipped: a box of blank lines opens no model file. */
    if (!pol->blank) {
        rc = load_model(p, dir, mi, im, res, ws, l, st);
        if (rc) return rc;
        loaded = 1;
        if (l->lm && pol->lm) {
            rc = lm_setup(p, dir, im, pol, x0, model, st);
            if (rc == VJO_E_CANCELLED || rc == VJO_E_OCR_UNAVAILABLE) return rc;
            lm_on = !rc;
        }
    }

    t0 = clock_us(p);
    for (unsigned i = 0, li = 0; i < set.count; i++) {
        const VocrLine *box = &set.lines[i];
        const unsigned char *box_pixels;
        size_t box_stride = w;
        VocrLineSet parts;
        len_left -= line_length(box);
        if (cancelled(im)) return VJO_E_CANCELLED;
        box_pixels = line_pixels(s, box, &box_stride);
        if (s->err) return s->err;
        if (!box_pixels) continue; /* taller than a strip and no line buffer: left out */
        parts.count = 1;
        parts.truncated = 0;
        parts.lines[0] = *box;
        if (l->refine_bytes) {
            /* The box refined on the pixels the job holds for it: its strip (the
             * box made strip-relative), or a tall line's own block. Its lines may
             * take the job to VJO_VOCR_MAX_LINES, one kept for each box still to
             * come; a split past that is not made, and the box is refined without
             * it (its main text band), as #83's whole-set refinement does. */
            int tall = box->h > s->strip_rows;
            VocrLinesImage bim = {tall ? box_pixels : s->strip, tall ? box->w : w, tall ? box->h : s->rows,
                                  box_stride, VOCR_LINES_GREY};
            unsigned ox = tall ? box->x : 0, oy = tall ? box->y : s->y0, flags = pol->refine, room;
            unsigned long rows = 0;
            VocrLineSet one;
            uint32_t r0 = clock_us(p);
            room = VJO_VOCR_MAX_LINES - (li < VJO_VOCR_MAX_LINES ? li : VJO_VOCR_MAX_LINES);
            room = room > set.count - i - 1u ? room - (set.count - i - 1u) : 1u;
            for (;;) {
                one.count = 1;
                one.truncated = 0;
                one.lines[0] = *box;
                one.lines[0].x -= ox;
                one.lines[0].y -= oy;
                rc = vocr_refine_lines(&bim, flags, ws + l->refine_off, l->refine_bytes, &one, &rows);
                st->refine_rows += rows;
                if (rc || one.count <= room || !(flags & VOCR_REFINE_SPLIT)) break;
                flags &= ~VOCR_REFINE_SPLIT;
                st->refine_capped++;
            }
            if (rc && !st->refine_rc) st->refine_rc = rc;
            if (!rc) { /* 1 to room lines */
                parts = one;
                for (unsigned k = 0; k < parts.count; k++) {
                    parts.lines[k].x += ox;
                    parts.lines[k].y += oy;
                }
                st->refine_boxes++;
                st->refine_lines += parts.count;
                st->refine_split += parts.count > 1;
            }
            st->refine_us += clock_us(p) - r0;
        }
        /* the box's estimate of the time still to read, now by its parts */
        for (unsigned k = 0; k < parts.count; k++) len_left += line_length(&parts.lines[k]);
        for (unsigned k = 0; k < parts.count; k++, li++) {
        const VocrLine *ln = &parts.lines[k];
        const unsigned char *pixels = box_pixels + (size_t)(ln->y - box->y) * box_stride + (ln->x - box->x);
        size_t stride = box_stride;
        vocr_image img;
        vocr_extra extra;
        VjoVocrInk ink = {0, 0, 0, 0, 0, 0};
        size_t start = used ? used + 1 : 0, n;
        uint32_t line_t0, line_us, line_len = line_length(ln);
        uint16_t flags;
        char *greedy;
        int blank = 0;
        len_left -= line_len;
        if (cancelled(im)) return VJO_E_CANCELLED;
        if (start + 1 >= cap) return VJO_E_TOO_LARGE;
        if (pol->blank || pol->line) {
            uint32_t b0 = clock_us(p);
            blank = vjo_vocr_ink(pixels, stride, ln->w, ln->h, &ink);
            if (pol->blank_edges) blank = ink.edges < pol->blank_edges;
            blank = blank && pol->blank;
            st->blank_us += clock_us(p) - b0;
        }
        if (blank) {
            if (st->blank < VJO_VOCR_BLANK_LOGGED) {
                VjoVocrBlankStat *b = &st->blank_line[st->blank];
                b->index = (uint16_t)li;
                b->width = (uint16_t)ln->w;
                b->height = (uint16_t)ln->h;
                b->x = (uint16_t)ln->x;
                b->y = (uint16_t)ln->y;
                b->ink = ink;
            }
            st->blank++;
            if (pol->line) {
                VjoVocrLineInfo info = {li, ln->x, ln->y, ln->w, ln->h, ln->dark, pixels, stride, ink, 1, "", 0, "",
                                        VJO_VOCR_LINE_CONF_NONE};
                pol->line(pol->ud, &info);
            }
            continue;
        }
        if (!loaded) {
            uint32_t r0 = clock_us(p);
            rc = load_model(p, dir, mi, im, res, ws, l, st);
            if (rc) return rc;
            loaded = 1;
            if (l->lm && pol->lm) {
                rc = lm_setup(p, dir, im, pol, x0, model, st);
                if (rc == VJO_E_CANCELLED || rc == VJO_E_OCR_UNAVAILABLE) return rc;
                lm_on = !rc;
            }
            t0 += clock_us(p) - r0; /* recognize_us leaves the load out */
        }
        img.pixels = pixels;
        img.width = ln->w;
        img.height = ln->h;
        img.stride = stride;
        img.format = VOCR_GREY8 | (ln->dark ? VOCR_INVERT : 0);
        memset(&extra, 0, sizeof(extra));
        extra.cancel = run_cancel;
        extra.user = (void *)im;
        if (l->lm) { /* the top K of every frame, for the decoder, and the line's confidence */
            extra.topk = VJO_VOCR_LM_TOPK;
            extra.topk_logit = (float *)(void *)(x0 + x.topk_logit);
            extra.topk_class = (uint16_t *)(void *)(x0 + x.topk_class);
            extra.blank_logit = (float *)(void *)(x0 + x.blank);
            extra.cap_frames = VJO_VOCR_LM_FRAMES;
        }
        line_t0 = clock_us(p);
        rc = vocr_run(ctx, &img, text + start, cap - start, &extra);
        if (rc != VOCR_OK) return run_error(rc);
        flags = VJO_VOCR_LINE_CONF_NONE;
        greedy = text + start;
        if (l->lm) {
            size_t gn = strlen(text + start);
            flags = line_conf(&extra, text + start);
            if (gn < LM_LINE_TEXT) { /* a class a frame, 4 bytes at most: always; kept for the log */
                greedy = (char *)(x0 + x.line);
                memcpy(greedy, text + start, gn + 1);
            }
            if (lm_on && gn && greedy != text + start) { /* a line read as empty stays empty */
                uint32_t d0 = clock_us(p);
                rc = vocr_lm_decode((vocr_lm_ctx *)(void *)(x0 + x.ctx), &extra, text + start, cap - start, NULL);
                st->lm_us += clock_us(p) - d0;
                if (rc == VOCR_E_CANCELLED) return VJO_E_CANCELLED;
                if (rc == VOCR_OK) {
                    flags |= VJO_VOCR_LINE_LM;
                    st->lm_lines++;
                    st->lm_changed += strcmp(greedy, text + start) != 0;
                } else { /* longer than the room left, or refused: the greedy line stands */
                    memcpy(text + start, greedy, gn + 1);
                    if (rc != VOCR_E_TEXT) {
                        st->lm_rc = VJO_E_OCR_INFERENCE;
                        lm_on = 0;
                    }
                }
            }
        }
        line_us = clock_us(p) - line_t0; /* the job's model: its run and decode */
        {   /* the primary's time per length, for the estimate of the lines still to read */
            prim_us += line_us;
            len_done += line_len;
            /* The cascade: a doubtful line read again by the larger model, whose
             * reading is then shown, while the job stays within its time ceiling. */
            if (casc_on && (flags & VJO_VOCR_LINE_CONF) < casc_conf && greedy != text + start) {
                uint64_t est = (uint64_t)line_us * casc_ratio / 256u +
                               (uint64_t)prim_us * len_left / (len_done ? len_done : 1u);
                if ((uint64_t)(clock_us(p) - t0) + est > pol->cascade_us) {
                    st->cascade_skipped++;
                } else {
                    if (!casc_ready) {
                        uint32_t r0 = clock_us(p);
                        rc = cascade_setup(p, dir, im, pol, c0, lm_on ? (const vocr_lm *)(void *)(x0 + x.lm) : NULL,
                                           st);
                        if (rc == VJO_E_CANCELLED || rc == VJO_E_OCR_UNAVAILABLE) return rc;
                        casc_ready = !rc;
                        casc_on = casc_ready;
                        t0 += clock_us(p) - r0; /* its load is not recognition time either */
                    }
                    if (casc_ready) {
                        uint32_t c_t0 = clock_us(p), c_us;
                        char *cg = (char *)(c0 + cx.greedy), *ct = cg;
                        vocr_extra e2;
                        memset(&e2, 0, sizeof(e2));
                        e2.cancel = run_cancel;
                        e2.user = (void *)im;
                        e2.topk = VJO_VOCR_LM_TOPK;
                        e2.topk_logit = (float *)(void *)(x0 + x.topk_logit);
                        e2.topk_class = (uint16_t *)(void *)(x0 + x.topk_class);
                        e2.blank_logit = (float *)(void *)(x0 + x.blank);
                        e2.cap_frames = VJO_VOCR_LM_FRAMES;
                        rc = vocr_run((vocr_ctx *)(void *)(c0 + cx.ctx), &img, cg, LM_LINE_TEXT, &e2);
                        if (rc == VOCR_E_CANCELLED) return VJO_E_CANCELLED;
                        if (rc == VOCR_OK) st->cascade_frames += e2.frames;
                        if (rc == VOCR_OK && lm_on && cg[0]) {
                            rc = vocr_lm_decode((vocr_lm_ctx *)(void *)(c0 + cx.lm_ctx), &e2, (char *)(c0 + cx.text),
                                                LM_LINE_TEXT, NULL);
                            if (rc == VOCR_E_CANCELLED) return VJO_E_CANCELLED;
                            if (rc == VOCR_OK) ct = (char *)(c0 + cx.text);
                            rc = VOCR_OK; /* its greedy reading, when the decode does not fit */
                        }
                        if (rc == VOCR_OK) {
                            size_t cn = strlen(ct);
                            if (start + cn < cap) { /* else the primary's reading stays */
                                st->cascade_changed += strcmp(ct, text + start) != 0;
                                st->cascade_blanked += !cn && text[start];
                                memcpy(text + start, ct, cn + 1);
                                flags = (uint16_t)((flags & ~VJO_VOCR_LINE_CONF) | line_conf(&e2, cg) |
                                                   VJO_VOCR_LINE_CASCADE);
                                st->cascade_lines++;
                            }
                        } else if (rc != VOCR_E_TEXT) {
                            st->cascade_rc = VJO_E_OCR_INFERENCE;
                            casc_on = 0;
                        }
                        c_us = clock_us(p) - c_t0;
                        st->cascade_us += c_us;
                        /* what this console takes for the larger model, if more than the estimate */
                        if (line_us && (uint64_t)c_us * 256u / line_us > casc_ratio)
                            casc_ratio = (uint32_t)((uint64_t)c_us * 256u / line_us);
                    }
                }
            }
        }
        if (strcmp(greedy, text + start)) flags |= VJO_VOCR_LINE_CHANGED;
        st->frames += extra.frames;
        if (st->recognized < VJO_VOCR_MAX_LINES) {
            VjoVocrLineStat *ls = &st->line[st->recognized++];
            ls->width = (uint16_t)ln->w;
            ls->height = (uint16_t)ln->h;
            ls->frames = (uint16_t)(extra.frames > 0xFFFFu ? 0xFFFFu : extra.frames);
            ls->flags = flags;
            ls->us = line_us; /* without a re-read: frames and time are the job's model's */
        }
        if (pol->line) {
            VjoVocrLineInfo info = {li, ln->x, ln->y, ln->w, ln->h, ln->dark, pixels, stride, ink, 0,
                                    text + start, clock_us(p) - line_t0, greedy, flags};
            pol->line(pol->ud, &info);
        }
        n = strlen(text + start);
        if (!n) { /* nothing read on this line: no separator either */
            text[used] = '\0';
            continue;
        }
        if (start) text[used] = '\n';
        used = start + n;
        if (pol->raw && pol->raw_cap) { /* its greedy text, laid out as the text */
            size_t rstart = rused ? rused + 1 : 0, gn = strlen(greedy);
            if (rstart + gn < pol->raw_cap) {
                memmove(pol->raw + rstart, greedy, gn + 1);
                if (rstart) pol->raw[rused] = '\n';
                rused = rstart + gn;
            }
        }
        } /* the box's parts */
    }
    st->recognize_us = clock_us(p) - t0 - st->cascade_us; /* the re-reads are cascade_us */
    return VJO_OK;
}

static const VjoVocrPolicy no_policy;

/* one_shot: the weights occupy the front of workspace for this job only. */
static int run_job(const VjoPlatform *platform, const char *model_dir, const char *model,
                   const VjoGreySource *image, VjoVocrResident *res, void *workspace,
                   size_t workspace_size, void *capture, size_t capture_size, int one_shot,
                   const VjoVocrPolicy *policy, char *text, size_t text_cap, VjoVocrStats *stats)
{
    const VjoVocrModel *mi;
    Layout l;
    Stream st;
    size_t front, need_capture;
    unsigned strip_rows;
    int rc = VJO_OK;
    if (!platform || !model_dir || !model || !image || !res || !text || !text_cap || !stats)
        return VJO_E_SOURCE;
    text[0] = '\0';
    memset(stats, 0, sizeof(*stats));
    mi = vjo_vocr_model(model);
    if (!mi) return VJO_E_OCR_MODEL;
    if (!image->strip || layout(mi, image->width, image->height, &l) < 0) return VJO_E_OCR_REGION;
    front = one_shot ? ALIGN16(mi->bytes) : 0;
    strip_rows = vjo_vocr_strip_rows(image->width, image->height);
    need_capture = vjo_vocr_capture_bytes(image->width, image->height);
    if (policy && policy->refine) {
        /* The refinement's arena ends the job's own workspace; without room the
         * boxes are read as found. */
        size_t rb = vjo_vocr_refine_bytes(image->width, image->height);
        if (workspace && rb && workspace_size >= front + l.total + rb) {
            l.refine_off = l.total;
            l.refine_bytes = rb;
            l.total += rb;
        } else {
            stats->refine_rc = VJO_E_OOM;
        }
    }
    {
        /* The LM extras (the top-K, for the LM and for each line's confidence)
         * follow the job's workspace, and the cascade's follow them; without
         * room the job reads greedily, or without the cascade. */
        int want_lm = policy && policy->lm && policy->lm_res;
        int want_cascade = policy && policy->cascade && policy->cascade_res && policy->cascade_us &&
                           policy->cascade != mi;
        size_t with_lm = front + l.total + vjo_vocr_lm_job_bytes(model);
        size_t with_cascade = with_lm + (want_cascade ? vjo_vocr_cascade_job_bytes(policy->cascade->name) : 0);
        if ((want_lm || want_cascade) && workspace && workspace_size >= with_lm) l.lm = 1;
        else if (want_lm) stats->lm_rc = VJO_E_OOM;
        if (want_cascade && l.lm && workspace_size >= with_cascade) l.cascade = 1;
        else if (want_cascade) stats->cascade_rc = VJO_E_OOM;
    }
    stats->workspace = front + l.total + (l.lm ? vjo_vocr_lm_job_bytes(model) : 0) +
                       (l.cascade ? vjo_vocr_cascade_job_bytes(policy->cascade->name) : 0);
    stats->capture = need_capture;
    stats->model_bytes = mi->bytes;
    stats->scratch_bytes = l.scratch;
    stats->width = image->width;
    stats->height = image->height;
    stats->strip_rows = strip_rows;
    if (cancelled(image)) return VJO_E_CANCELLED;
    if (!workspace || ((uintptr_t)workspace & 15u) || workspace_size < front + l.total ||
        !capture || capture_size < need_capture) return VJO_E_OOM;
    memset(&st, 0, sizeof(st));
    st.src = image;
    st.p = platform;
    st.pol = policy ? policy : &no_policy;
    st.st = stats;
    st.w = image->width;
    st.h = image->height;
    st.strip_rows = strip_rows;
    st.strip = (unsigned char *)capture;
    st.slots = vjo_vocr_state_slots(st.w, st.h);
    st.sums = (uint32_t *)(void *)((unsigned char *)capture + ALIGN4((size_t)strip_rows * st.w));
    st.ring = (uint16_t *)(void *)(st.sums + st.h);
    st.seen = (unsigned char *)(st.ring + (size_t)st.slots * st.h);
    st.cyc = st.seen + BIT_BYTES(st.h);
    /* An icon is a few dozen rows; a looping background may be more. */
    st.cycle_cap = VJO_VOCR_CYCLE_ROWS;
    st.learn = st.pol->settle_us && platform->now_us;
    st.settle_left = st.pol->settle_max_us ? st.pol->settle_max_us : VJO_VOCR_SETTLE_MAX_US;
    /* The screen may change while it is read (the player advanced the text): then
     * the region is captured again, up to VJO_VOCR_TRIES times. Without the
     * stable-text wait from scratch; with it, after waiting for the rows to hold
     * still (and before the first, with settle_first). */
    for (unsigned attempt = 1; attempt <= VJO_VOCR_TRIES; attempt++) {
        int wait = settle_on(&st) && (attempt > 1 || st.pol->settle_first);
        if (!wait || attempt == 1) bits_clear(&st);
        st.rows = st.y0 = st.first_next = 0;
        st.err = 0;
        st.held = st.verified = 0;
        st.cover_next = 0;
        stats->tries = attempt;
        stats->region_sum = VJO_FNV_INIT;
        if (wait && settle_wait(&st) < 0) {
            rc = stream_error(&st, VJO_E_SOURCE);
            break;
        }
        rc = recognize(platform, model_dir, mi, image, res, (unsigned char *)workspace + front, &l, &st,
                       text, text_cap, stats);
        if (rc != VJO_E_SCREEN_CHANGED || cancelled(image)) break;
    }
    if (rc) text[0] = '\0'; /* never publish a partial recognition */
    return rc;
}

int vjo_vocr_ocr_resident(const VjoPlatform *platform, const char *model_dir, const char *model,
                          const VjoGreySource *image, VjoVocrResident *resident,
                          void *workspace, size_t workspace_size, void *capture, size_t capture_size,
                          char *text, size_t text_cap, VjoVocrStats *stats)
{
    return run_job(platform, model_dir, model, image, resident, workspace, workspace_size, capture,
                   capture_size, 0, NULL, text, text_cap, stats);
}

int vjo_vocr_ocr_policy(const VjoPlatform *platform, const char *model_dir, const char *model,
                        const VjoGreySource *image, VjoVocrResident *resident,
                        void *workspace, size_t workspace_size, void *capture, size_t capture_size,
                        const VjoVocrPolicy *policy, char *text, size_t text_cap, VjoVocrStats *stats)
{
    return run_job(platform, model_dir, model, image, resident, workspace, workspace_size, capture,
                   capture_size, 0, policy, text, text_cap, stats);
}

static void *one_shot_alloc(void *ud, size_t bytes)
{
    (void)bytes;
    return ud; /* the front of the caller's workspace, sized by vjo_vocr_workspace_bytes */
}

int vjo_vocr_ocr(const VjoPlatform *platform, const char *model_dir, const char *model,
                 const VjoGreySource *image, void *workspace, size_t workspace_size,
                 void *capture, size_t capture_size, char *text, size_t text_cap, VjoVocrStats *stats)
{
    VjoVocrResident one;
    memset(&one, 0, sizeof(one));
    one.alloc = one_shot_alloc;
    one.ud = workspace;
    return run_job(platform, model_dir, model, image, &one, workspace, workspace_size, capture,
                   capture_size, 1, NULL, text, text_cap, stats);
}

/* ---- the blank-line check ---- */

/* The grey level at or below which `part` per mille of the n pixels lie
 * (n <= VJO_VOCR_INK_PIXELS: 32-bit arithmetic, no 64-bit division helper). */
static unsigned percentile(const uint32_t *hist, uint32_t n, unsigned part)
{
    uint32_t want = (n * part + 999u) / 1000u, acc = 0;
    unsigned v;
    if (!want) want = 1;
    for (v = 0; v < 255u; v++) {
        acc += hist[v];
        if (acc >= want) break;
    }
    return v;
}

static unsigned grey_step(unsigned a, unsigned b)
{
    return a > b ? a - b : b - a;
}

/* floor(n * 10000 / total) for n <= 2 total, total <= VJO_VOCR_INK_PIXELS, in
 * 32-bit steps of 100 (each remainder times 100 stays under 2^32). */
static uint32_t per_10000(uint32_t n, uint32_t total)
{
    uint32_t q = n / total, r = n % total, a, b;
    a = r * 100u / total;
    r = r * 100u % total;
    b = r * 100u / total;
    return q * 10000u + a * 100u + b;
}

int vjo_vocr_ink(const unsigned char *pixels, size_t stride, unsigned w, unsigned h, VjoVocrInk *out)
{
    uint32_t hist[256], total, edges = 0;
    unsigned lo, hi, e;
    memset(out, 0, sizeof(*out));
    if (!pixels || !w || !h || w > VJO_OCR_MAX_WIDTH || h > 0xFFFFu) return 0; /* unknown: never blank */
    total = (uint32_t)w * h;
    if (total > VJO_VOCR_INK_PIXELS) return 0;
    memset(hist, 0, sizeof(hist));
    for (unsigned y = 0; y < h; y++) {
        const unsigned char *r = pixels + (size_t)y * stride;
        for (unsigned x = 0; x < w; x++) hist[r[x]]++;
    }
    lo = percentile(hist, total, 20u);
    hi = percentile(hist, total, 980u);
    e = (hi - lo) / VJO_VOCR_EDGE_DIV < VJO_VOCR_EDGE_MIN ? VJO_VOCR_EDGE_MIN : (hi - lo) / VJO_VOCR_EDGE_DIV;
    /* An edge that scaling or blur softened rises over more than one pixel: the
     * step counts when the grey level gets there within VJO_VOCR_INK_SPAN pixels. */
    for (unsigned y = 0; y < h; y++) {
        const unsigned char *r = pixels + (size_t)y * stride;
        for (unsigned x = 0; x < w; x++) {
            unsigned v = r[x], across = 0, down = 0;
            for (unsigned s = 1; s <= VJO_VOCR_INK_SPAN && !(across && down); s++) {
                if (!across && x + s < w && grey_step(v, r[x + s]) >= e) across = 1;
                if (!down && y + s < h && grey_step(v, r[(size_t)s * stride + x]) >= e) down = 1;
            }
            edges += across + down;
        }
    }
    out->lo = (uint8_t)lo;
    out->hi = (uint8_t)hi;
    out->edge = (uint8_t)e;
    out->edges = (uint16_t)per_10000(edges, total); /* at most 2 per pixel: 20,000 */
    return vjo_vocr_blank(out);
}

int vjo_vocr_blank(const VjoVocrInk *ink)
{
    return ink->edges < VJO_VOCR_BLANK_EDGES;
}

/* ---- an RGBA image in memory as a grey source (host tools and tests) ---- */
static int rgba_strip(void *ud, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags)
{
    const VjoRgbaImage *img = (const VjoRgbaImage *)ud;
    (void)flags;
    if (y >= img->height || n > img->height - y) return VJO_E_SOURCE;
    for (unsigned r = 0; r < n; r++) {
        const unsigned char *p = img->pixels + (size_t)(y + r) * img->stride;
        unsigned char *d = dst + (size_t)r * stride;
        for (unsigned x = 0; x < img->width; x++, p += 4)
            d[x] = (unsigned char)vjo_grey(p[0], p[1], p[2]);
    }
    return (int)n;
}

void vjo_grey_source_rgba(const VjoRgbaImage *img, VjoGreySource *out)
{
    memset(out, 0, sizeof(*out));
    out->ud = (void *)img;
    out->width = img->width;
    out->height = img->height;
    out->strip = rgba_strip;
}
