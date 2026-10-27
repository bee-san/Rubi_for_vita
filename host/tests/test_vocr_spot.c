/* API 53: the text spotter as a job's finder (core/vocr_ocr.c, VjoVocrPolicy.spot), with the committed random VTSP
 * version-4 blob (host/fixtures/spotter; its pins test_spot_pins.h) and a synthetic recognizer pin with a 4 KiB arena
 * (test_spot_models.h): its weights' load and refusals (vjo_vocr_spot_load); the lines a job reads on fixture frames
 * against the two runtime calls made directly (vocr_textspot6_find on the whole frame in memory, its own dark flags,
 * then vocr_textspot7_post on the region in memory with its horizontal boxes in its order: textspot7's decode D, at
 * most 16); the region's assignment (a box whose centre is outside the region is not read); the frame read at its
 * blob's scale; every frame row checked, those in the region against the region's capture; GRAD when it cannot run
 * or refuses; no refinement, gate or short-line probe beside it; the sizes. No recognizer weights are read: every
 * found line is skipped as blank, so the job hands each line's crop to the line callback without recognizing it.
 * Random weights find no text: this makes no accuracy or timing claim. */
#include "acutest.h"
#include "vocr_ocr.h"
#include "vocr.h"
#include "vocr_lines.h"
#include "vocr_lines_refine.h"
#include "vocr_textspot6.h"
#include "vocr_textspot7.h"
#include "../../include/vjo_vocr_sizes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef VJO_SOURCE_DIR
#error "VJO_SOURCE_DIR: the source tree (its host/fixtures)"
#endif
#define ALIGN16(x) (((x) + (size_t)15) & ~(size_t)15)

/* ---- the fixtures: the blob and the frames ---- */
static unsigned char blob[VJO_VOCR_SPOT_BYTES];
static void load_blob(void)
{
    FILE *f = fopen(VJO_SOURCE_DIR "/host/fixtures/spotter/" VJO_VOCR_SPOT_NAME, "rb");
    TEST_ASSERT(f != NULL);
    TEST_ASSERT(fread(blob, 1, sizeof(blob), f) == sizeof(blob) && fgetc(f) == EOF);
    fclose(f);
}

#define TB_W 852u
#define TB_H 135u
#define MAX_W 960u
#define MAX_H 544u
static unsigned char textbox[TB_W * TB_H];
static unsigned char grey[MAX_W * MAX_H];
static unsigned frame_w, frame_h;
static void load_textbox(void)
{
    char head[16];
    FILE *f = fopen(VJO_SOURCE_DIR "/host/fixtures/short_line/textbox.pgm", "rb");
    TEST_ASSERT(f != NULL);
    TEST_ASSERT(fread(head, 1, 15, f) == 15 && !memcmp(head, "P5\n852 135\n255\n", 15));
    TEST_ASSERT(fread(textbox, 1, sizeof(textbox), f) == sizeof(textbox));
    fclose(f);
}
/* A w x h frame: a pattern, with the text box tiled over the rectangle (bx, by, bw, bh), inverted (dark text on
 * light) or not. */
static void make_frame(unsigned w, unsigned h, unsigned bx, unsigned by, unsigned bw, unsigned bh, int invert)
{
    TEST_ASSERT(w <= MAX_W && h <= MAX_H && bx + bw <= w && by + bh <= h);
    frame_w = w;
    frame_h = h;
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) grey[y * w + x] = (unsigned char)((x * 3u + y * 5u) & 0x3Fu);
    for (unsigned y = 0; y < bh; y++)
        for (unsigned x = 0; x < bw; x++) {
            unsigned char v = textbox[(y % TB_H) * TB_W + x % TB_W];
            grey[(by + y) * w + bx + x] = invert ? (unsigned char)(255u - v) : v;
        }
}

/* ---- the card: the spotter's file (the blob, perhaps changed), every other file missing ---- */
static struct {
    int missing, size_delta, corrupt, cancel;
    uint64_t stamp;
    unsigned opens, spot_opens, reads, other_opens;
} card;
static int card_read(void *ctx, uint64_t off, void *dst, size_t n)
{
    (void)ctx;
    if (off > VJO_VOCR_SPOT_BYTES || n > VJO_VOCR_SPOT_BYTES - off) return -1;
    memcpy(dst, blob + off, n);
    if (card.corrupt && off <= 100 && 100 < off + n) ((unsigned char *)dst)[100 - off] ^= 1;
    card.reads++;
    return 0;
}
static void card_close(void *ctx) { (void)ctx; }
static int card_open(void *ud, const char *path, VjoFile *out)
{
    const char *name = strrchr(path, '/');
    (void)ud;
    name = name ? name + 1 : path;
    card.opens++;
    if (strcmp(name, VJO_VOCR_SPOT_NAME)) {
        card.other_opens++;
        return -1;
    }
    card.spot_opens++;
    if (card.missing) return -1;
    *out = (VjoFile){NULL, (uint64_t)((int64_t)VJO_VOCR_SPOT_BYTES + card.size_delta), card_read, card_close,
                     card.stamp};
    return 0;
}
static uint64_t clock_now;
static uint64_t now(void *ud)
{
    (void)ud;
    return clock_now += 7;
}
static VjoPlatform platform = {NULL, card_open, now, NULL, 0};
static int cancel_test(void *ud)
{
    (void)ud;
    return card.cancel;
}

/* ---- the resident's block ---- */
static unsigned char block_mem[64 * 1024] __attribute__((aligned(64)));
static struct {
    int live, fail, misalign, release_fail;
    unsigned allocs, releases;
    size_t bytes;
} blk;
static void *blk_alloc(void *ud, size_t bytes)
{
    (void)ud;
    blk.allocs++;
    TEST_CHECK(!blk.live); /* never two blocks */
    if (blk.fail || bytes + 16 > sizeof(block_mem)) return NULL;
    blk.live = 1;
    blk.bytes = bytes;
    return block_mem + (blk.misalign ? 8 : 0);
}
static int blk_release(void *ud)
{
    (void)ud;
    blk.releases++;
    if (blk.release_fail) return -1;
    blk.live = 0;
    return 0;
}
static VjoVocrSpotResident res;
static void reset(void)
{
    memset(&card, 0, sizeof(card));
    card.stamp = 0x1234;
    memset(&blk, 0, sizeof(blk));
    memset(&res, 0, sizeof(res));
    res.alloc = blk_alloc;
    res.release = blk_release;
}
static int load(unsigned *read)
{
    VjoGreySource im;
    memset(&im, 0, sizeof(im));
    im.cancelled = cancel_test;
    return vjo_vocr_spot_load(&platform, "card", &res, &im, read);
}

/* ---- the two runtime calls, made directly on the frame and the region in memory ---- */
static unsigned char runtime_arena[512 * 1024], post_arena[16 * 1024];
static VocrTextspot6Set runtime_set;
static VocrT7Box frame_boxes[VOCR_T7_MAX_BOXES], post_boxes[VOCR_T7_MAX_BOXES];
static unsigned n_frame_boxes, n_post;
/* The spotter on the whole frame (flags 0: its blob's scale and its dark flags), its horizontal boxes in its order
 * (vita-vn-ocr eval/textspot7.py frame_boxes_of), then textspot7's post-pass with the region (rx, ry, rw, rh) and
 * the pinned steps: the region's lines, at most 16 (*more: past 16). */
static unsigned runtime_lines(unsigned rx, unsigned ry, unsigned rw, unsigned rh, VocrLine *lines, unsigned *more)
{
    vocr_textspot6 m;
    VocrLinesImage im = {grey, frame_w, frame_h, frame_w, VOCR_LINES_GREY};
    VocrLinesImage region = {grey + (size_t)ry * frame_w + rx, rw, rh, frame_w, VOCR_LINES_GREY};
    size_t a;
    unsigned n = 0;
    TEST_ASSERT(vocr_textspot6_load(&m, blob, sizeof(blob)) == VOCR_LINES_OK);
    a = vocr_textspot6_frame_arena_bytes(&m, frame_w, frame_h);
    TEST_ASSERT(a && a <= sizeof(runtime_arena));
    TEST_ASSERT(vocr_textspot6_find(&m, &im, 0, runtime_arena, a, &runtime_set) == VOCR_LINES_OK);
    n_frame_boxes = 0;
    for (unsigned i = 0; i < runtime_set.count; i++) {
        const VocrTextspot6Box *b = &runtime_set.boxes[i];
        if (!b->vertical) frame_boxes[n_frame_boxes++] = (VocrT7Box){b->x, b->y, b->w, b->h, b->conf, b->dark};
    }
    TEST_ASSERT(vocr_textspot7_arena_bytes_steps(rw, rh, VJO_VOCR_SPOT_POST) <= sizeof(post_arena));
    TEST_ASSERT(vocr_textspot7_post(frame_boxes, n_frame_boxes, (int)rx, (int)ry, (int)(rx + rw), (int)(ry + rh),
                                    &region, VJO_VOCR_SPOT_POST, post_arena, sizeof(post_arena), post_boxes,
                                    &n_post) == VOCR_T7_OK);
    *more = n_post > VJO_VOCR_MAX_LINES;
    for (unsigned i = 0; i < n_post && n < VJO_VOCR_MAX_LINES; i++, n++)
        lines[n] = (VocrLine){(unsigned)post_boxes[i].x, (unsigned)post_boxes[i].y, (unsigned)post_boxes[i].w,
                              (unsigned)post_boxes[i].h, post_boxes[i].dark};
    return n;
}

/* ---- the sources: the region's and the frame's, strips of the frame in memory ---- */
typedef struct {
    unsigned x0, y0, w, h;   /* the rectangle of the frame they read */
    unsigned strips, rows;   /* strips and rows read */
    int fail_at;             /* > 0: the strip that reads this row (1-based) fails */
    int change_at;           /* > 0: from the read of this many strips on, the rectangle's first row reads changed */
    unsigned first_flags;    /* strips with VJO_STRIP_FIRST */
} Src;
static int src_strip(void *ud, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags)
{
    Src *s = ud;
    TEST_ASSERT(y + n <= s->h);
    s->strips++;
    s->rows += n;
    if (flags & VJO_STRIP_FIRST) s->first_flags++;
    if (s->fail_at > 0 && y < (unsigned)s->fail_at && (unsigned)s->fail_at <= y + n) return VJO_E_SOURCE;
    for (unsigned r = 0; r < n; r++) {
        memcpy(dst + r * stride, grey + (size_t)(s->y0 + y + r) * frame_w + s->x0, s->w);
        if (s->change_at > 0 && s->strips >= (unsigned)s->change_at && y + r == 0) dst[r * stride] ^= 0x55u;
    }
    return (int)n;
}

/* ---- a job ---- */
static VocrLine got[VJO_VOCR_MAX_LINES + 1];
static unsigned n_got, bad_pixels, region_x, region_y;
static void record(void *ud, const VjoVocrLineInfo *info)
{
    (void)ud;
    if (n_got <= VJO_VOCR_MAX_LINES) got[n_got] = (VocrLine){info->x, info->y, info->w, info->h, info->dark};
    n_got++;
    /* the crop the recognizer would read is the frame's own pixels there (region coordinates) */
    for (unsigned y = 0; y < info->h; y++)
        if (memcmp(info->pixels + (size_t)y * info->stride,
                   grey + (size_t)(region_y + info->y + y) * frame_w + region_x + info->x, info->w)) {
            bad_pixels++;
            break;
        }
}
static unsigned char line_mem[MAX_W * MAX_H];
static void *line_buffer(void *ud, size_t bytes)
{
    (void)ud;
    return bytes <= sizeof(line_mem) ? line_mem : NULL;
}
static unsigned char ws_mem[2 * 1024 * 1024] __attribute__((aligned(64)));
static unsigned char capture[96 * 1024];
static Src region_src, frame_src;
static VjoGreySource region_im, frame_im;
/* The region (x, y, w, h) of the frame: its source and the frame's (none when the region is the frame). */
static VjoVocrPolicy policy(unsigned x, unsigned y, unsigned w, unsigned h)
{
    VjoVocrPolicy p;
    memset(&p, 0, sizeof(p));
    p.blank = 1;
    p.blank_edges = 20000; /* every crop blank: nothing recognized, no recognizer file opened */
    p.line = record;
    p.spot = 1;
    p.spot_res = &res;
    region_x = x;
    region_y = y;
    region_src = (Src){x, y, w, h, 0, 0, 0, 0, 0};
    region_im = (VjoGreySource){&region_src, w, h, src_strip, NULL, line_buffer};
    frame_src = (Src){0, 0, frame_w, frame_h, 0, 0, 0, 0, 0};
    frame_im = (VjoGreySource){&frame_src, frame_w, frame_h, src_strip, NULL, NULL};
    if (x || y || w != frame_w || h != frame_h) {
        p.spot_frame = &frame_im;
        p.spot_x = x;
        p.spot_y = y;
    }
    return p;
}
static size_t spot_need(const VjoVocrPolicy *pol)
{
    return vjo_vocr_job_bytes_spot("FL10_w8.vocr", region_im.width, region_im.height,
                                   vjo_vocr_spot_bytes(&res, region_im.width, region_im.height,
                                                       pol->spot_frame ? frame_w : 0, pol->spot_frame ? frame_h : 0));
}
/* The job on the region, its workspace `workspace` bytes (0: the spotter's own, vjo_vocr_job_bytes_spot). */
static int job(const VjoVocrPolicy *pol, size_t workspace, VjoVocrStats *st)
{
    VjoVocrResident model;
    char text[256];
    size_t need = workspace ? workspace : spot_need(pol);
    if (pol->refine) need += vjo_vocr_refine_bytes(region_im.width, region_im.height);
    TEST_ASSERT(need && need <= sizeof(ws_mem));
    TEST_ASSERT(vjo_vocr_capture_bytes(region_im.width, region_im.height) <= sizeof(capture));
    memset(&model, 0, sizeof(model));
    n_got = bad_pixels = 0;
    return vjo_vocr_ocr_policy(&platform, "card", "FL10_w8.vocr", &region_im, &model, ws_mem, need, capture,
                               vjo_vocr_capture_bytes(region_im.width, region_im.height), pol, text, sizeof(text), st);
}
static int same_lines(const VocrLine *a, unsigned na, const VocrLine *b, unsigned nb)
{
    if (na != nb) return 0;
    for (unsigned i = 0; i < na; i++)
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].w != b[i].w || a[i].h != b[i].h || a[i].dark != b[i].dark)
            return 0;
    return 1;
}

/* ---- tests ---- */
static void the_fixture_is_the_pinned_blob(void)
{
    vocr_textspot6 m;
    load_blob();
    TEST_CHECK(vocr_textspot6_load(&m, blob, sizeof(blob)) == VOCR_LINES_OK);
    TEST_CHECK(m.version == VOCR_TEXTSPOT6_VERSION_TAG && m.form == VOCR_TEXTSPOT6_FORM_TRUNK && m.tags == 4);
    TEST_CHECK(m.half_above == 480 && !m.half_above_h && m.blob_bytes == VJO_VOCR_SPOT_BYTES);
    /* its arena pin: the runtime's for a 960 x 544 frame, the most any frame up to it needs */
    TEST_CHECK(vocr_textspot6_arena_bytes(&m, 960, 544) == VJO_VOCR_SPOT_ARENA);
    TEST_CHECK(vocr_textspot6_frame_arena_bytes(&m, 960, 544) == VJO_VOCR_SPOT_ARENA);
    /* the pick's decode, D: join10, then extend */
    TEST_CHECK(VJO_VOCR_SPOT_POST == (VOCR_T7_JOIN10 | VOCR_T7_EXTEND));
    /* the block: the weights, then their parsed form, each 16-byte aligned */
    TEST_CHECK(vjo_vocr_spot_block_bytes() == ALIGN16((size_t)VJO_VOCR_SPOT_BYTES) + ALIGN16(sizeof(vocr_textspot6)));
}

static void the_weights_are_read_checked_and_kept(void)
{
    unsigned read = 7;
    load_blob();
    reset();
    TEST_CHECK(load(&read) == VJO_OK && read == 1 && res.verified && blk.live && blk.allocs == 1);
    TEST_CHECK(blk.bytes == vjo_vocr_spot_block_bytes() && res.capacity == blk.bytes);
    unsigned reads = card.reads;
    /* the file unchanged (size and stamp): opened, not read again */
    for (unsigned i = 0; i < 5; i++) TEST_CHECK(load(&read) == VJO_OK && read == 0 && res.verified);
    TEST_CHECK(card.reads == reads && card.spot_opens == 6 && blk.allocs == 1);
    /* replaced (another stamp): read again into the same block */
    card.stamp++;
    TEST_CHECK(load(&read) == VJO_OK && read == 1 && card.reads > reads && blk.allocs == 1 && !blk.releases);
    /* an unknown stamp (0): read every time */
    card.stamp = 0;
    TEST_CHECK(load(&read) == VJO_OK && read == 1);
    TEST_CHECK(load(&read) == VJO_OK && read == 1);
    TEST_CHECK(vjo_vocr_spot_drop(&res) == 0 && !blk.live && !res.verified && !res.data);
}

static void a_missing_wrong_or_short_file_is_refused(void)
{
    unsigned read = 7;
    load_blob();
    /* missing: nothing held */
    reset();
    card.missing = 1;
    TEST_CHECK(load(&read) == VJO_E_OCR_MODEL && !read && !res.verified && !blk.allocs);
    /* the wrong size: never this pin */
    reset();
    card.size_delta = 1;
    TEST_CHECK(load(&read) == VJO_E_OCR_MODEL && !res.verified && !blk.allocs);
    card.size_delta = -1;
    TEST_CHECK(load(&read) == VJO_E_OCR_MODEL && !res.verified && !blk.allocs);
    /* the right size, another hash: read, refused, the block released (memory only for verified weights) */
    reset();
    card.corrupt = 1;
    TEST_CHECK(load(&read) == VJO_E_OCR_MODEL && !read && !res.verified && !res.data && blk.allocs == 1 && !blk.live);
    /* verified, then the file changes to a wrong one: released */
    reset();
    TEST_CHECK(load(&read) == VJO_OK && blk.live);
    card.stamp++;
    card.corrupt = 1;
    TEST_CHECK(load(&read) == VJO_E_OCR_MODEL && !res.verified && !blk.live);
    /* verified, then the file is gone: released */
    reset();
    TEST_CHECK(load(&read) == VJO_OK && blk.live);
    card.missing = 1;
    TEST_CHECK(load(&read) == VJO_E_OCR_MODEL && !res.verified && !blk.live);
}

static void no_memory_or_a_cancellation_is_not_a_crash(void)
{
    unsigned read = 7;
    load_blob();
    /* no block: the job goes on without the spotter */
    reset();
    blk.fail = 1;
    TEST_CHECK(load(&read) == VJO_E_OOM && !res.verified && !res.data);
    /* a block not 16-byte aligned is given back */
    reset();
    blk.misalign = 1;
    TEST_CHECK(load(&read) == VJO_E_OOM && !res.verified && !res.data && !blk.live);
    /* a block that cannot be given back: the worker stops taking jobs (VJO_E_OCR_UNAVAILABLE) */
    reset();
    TEST_CHECK(load(&read) == VJO_OK);
    card.missing = 1;
    blk.release_fail = 1;
    TEST_CHECK(load(&read) == VJO_E_OCR_UNAVAILABLE && !res.verified);
    blk.release_fail = 0;
    TEST_CHECK(vjo_vocr_spot_drop(&res) == 0 && !blk.live);
    /* cancelled while its file is opened: proves nothing about the file, verified weights stay */
    reset();
    TEST_CHECK(load(&read) == VJO_OK);
    card.missing = 1;
    card.cancel = 1;
    TEST_CHECK(load(&read) == VJO_E_CANCELLED && res.verified && blk.live);
    /* bad arguments */
    TEST_CHECK(vjo_vocr_spot_load(NULL, "card", &res, NULL, &read) == VJO_E_SOURCE);
    TEST_CHECK(vjo_vocr_spot_load(&platform, "card", NULL, NULL, &read) == VJO_E_SOURCE);
    TEST_CHECK(vjo_vocr_spot_drop(NULL) == 0);
}

/* The job's lines are the post-pass of the runtime's own boxes, bit for bit: the spotter on the whole frame (its
 * dark flags), its horizontal boxes in its order, vocr_textspot7_post on the region with the pinned steps, at most
 * 16; every crop the frame's own pixels. Regions of one strip and of several, inside the frame, at its edges, the
 * whole frame; light and dark text; a PSP-sized frame read at its own size. */
static void the_job_reads_the_post_pass_of_the_runtimes_boxes(void)
{
    static const struct { unsigned fw, fh, bx, by, bw, bh, rx, ry, rw, rh; int invert; } cases[] = {
        {960, 544, 54, 380, 852, 135, 54, 380, 852, 135, 0},  /* a dialogue box at the bottom, drawn on it */
        {960, 544, 54, 380, 852, 135, 40, 370, 880, 160, 0},  /* drawn with a margin */
        {960, 544, 54, 380, 852, 135, 54, 380, 852, 60, 1},   /* its first line only, dark text on light */
        {960, 544, 0, 0, 960, 544, 0, 0, 960, 544, 0},        /* the whole frame: its own stream */
        {960, 544, 0, 0, 960, 544, 100, 50, 700, 300, 0},     /* tiled text, a region of several strips */
        {960, 544, 54, 380, 852, 135, 0, 400, 960, 144, 0},   /* the frame's whole width, to its bottom */
        {960, 544, 54, 380, 852, 135, 500, 420, 120, 40, 1},  /* a small one: one strip */
        {480, 272, 0, 120, 480, 135, 10, 120, 460, 135, 0},   /* a PSP-sized frame, read at its own size */
    };
    load_blob();
    load_textbox();
    for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        VjoVocrPolicy pol;
        VjoVocrStats st;
        VocrLine want[VJO_VOCR_MAX_LINES];
        unsigned more, n;
        reset();
        make_frame(cases[k].fw, cases[k].fh, cases[k].bx, cases[k].by, cases[k].bw, cases[k].bh, cases[k].invert);
        TEST_ASSERT(load(NULL) == VJO_OK);
        n = runtime_lines(cases[k].rx, cases[k].ry, cases[k].rw, cases[k].rh, want, &more);
        pol = policy(cases[k].rx, cases[k].ry, cases[k].rw, cases[k].rh);
        TEST_CHECK_(job(&pol, 0, &st) == VJO_OK, "case %u", k);
        TEST_CHECK_(st.spot_ran == 1 && !st.spot_rc && st.lines == n && st.truncated == more && n_got == n,
                    "case %u: %u lines (%u read), the runtime's %u%s", k, st.lines, n_got, n, more ? "+" : "");
        TEST_CHECK_(same_lines(got, n_got, want, n), "case %u: the runtime's post-pass", k);
        TEST_CHECK_(!bad_pixels, "case %u: every crop the frame's own", k);
        TEST_CHECK(st.spot_boxes == runtime_set.count && st.spot_truncated == runtime_set.truncated);
        TEST_CHECK(st.spot_bytes == vjo_vocr_spot_bytes(&res, cases[k].rw, cases[k].rh, frame_w, frame_h) &&
                   st.spot_us > 0);
        TEST_CHECK(st.blank == n && !st.recognized && !card.other_opens); /* no recognizer weights opened */
        unsigned dark = 0;
        for (unsigned i = 0; i < n; i++) dark += want[i].dark ? 1u : 0u;
        TEST_CHECK(st.dark == dark);
        /* the region's first pass is the capture (its strips flagged first); the frame read through its own
         * stream, a whole pass at least, none of its strips a first pass */
        if (pol.spot_frame) {
            TEST_CHECK_(frame_src.rows >= frame_h && !frame_src.first_flags && st.spot_strips == frame_src.strips,
                        "case %u: %u frame rows in %u strips", k, frame_src.rows, frame_src.strips);
            TEST_CHECK(region_src.first_flags > 0);
        } else {
            TEST_CHECK(!frame_src.strips && !st.spot_strips && region_src.first_flags > 0);
        }
        if (k == 2) TEST_CHECK_(dark > 0, "dark text: %u dark lines", dark);
    }
}

/* textspot7's region rule: a frame box is the region's only when its centre lies in it (then clipped to it), so a
 * box that crosses into the region with its centre outside gives no line; the frame's other boxes cost no read. */
static void a_box_whose_centre_is_outside_the_region_is_not_read(void)
{
    VjoVocrPolicy pol;
    VjoVocrStats st;
    VocrLine want[VJO_VOCR_MAX_LINES];
    unsigned more, n, found = 0;
    load_blob();
    load_textbox();
    reset();
    make_frame(960, 544, 0, 0, 960, 544, 0);
    TEST_ASSERT(load(NULL) == VJO_OK);
    /* the region: a strip left of a horizontal box, reaching a quarter into it (the random blob's boxes are small) */
    unsigned rx = 0, ry = 0, rw = 0, rh = 0;
    (void)runtime_lines(0, 0, 960, 544, want, &more);
    for (unsigned i = 0; i < n_frame_boxes && !found; i++) {
        const VocrT7Box *b = &frame_boxes[i];
        if (b->w < 16 || b->h < 8) continue;
        /* a region ending a quarter into the box: the box crosses into it, its centre stays outside */
        rx = b->x > 16 ? (unsigned)b->x - 16 : 0;
        ry = (unsigned)b->y;
        rw = (unsigned)b->x + (unsigned)b->w / 4 - rx;
        rh = (unsigned)b->h;
        found = rw >= 8;
    }
    TEST_ASSERT(found);
    n = runtime_lines(rx, ry, rw, rh, want, &more);
    pol = policy(rx, ry, rw, rh);
    TEST_CHECK(job(&pol, 0, &st) == VJO_OK && st.spot_ran && same_lines(got, n_got, want, n));
    /* every line read is a kept box's: no line is the clip of a box whose centre lies outside the region */
    for (unsigned i = 0; i < n_frame_boxes; i++) {
        const VocrT7Box *b = &frame_boxes[i];
        const int cx = 2 * b->x + b->w, cy = 2 * b->y + b->h;
        const int inside = 2 * (int)rx <= cx && cx < 2 * (int)(rx + rw) && 2 * (int)ry <= cy && cy < 2 * (int)(ry + rh);
        const int crosses = b->x < (int)(rx + rw) && b->x + b->w > (int)rx && b->y < (int)(ry + rh) &&
                            b->y + b->h > (int)ry;
        if (!crosses || inside) continue;
        found = 2;
        for (unsigned k = 0; k < n_got; k++) {
            const int x0 = b->x > (int)rx ? b->x - (int)rx : 0, y0 = b->y > (int)ry ? b->y - (int)ry : 0;
            TEST_CHECK_(!(got[k].x == (unsigned)x0 && got[k].y == (unsigned)y0), "box %u read", i);
        }
    }
    TEST_CHECK_(found == 2, "a box crosses into the region with its centre outside");
    /* a frame box crossing the region's edge with its centre outside: not in the spotter's kept set, so its rows
     * were never read for a dark flag (the job reads only the region's boxes' rows past the frame's pass) */
    TEST_CHECK(frame_src.rows < 2u * frame_h);
}

/* The frame is read at its blob's scale, with no SCALE flag: halved when wider than its half_above (every 960 x 544
 * frame), at its own size when not (480 x 272): the two differ on this frame, and the job follows each. */
static void the_frame_is_read_at_its_blobs_scale(void)
{
    vocr_textspot6 m;
    load_blob();
    load_textbox();
    TEST_ASSERT(vocr_textspot6_load(&m, blob, sizeof(blob)) == VOCR_LINES_OK && m.half_above == 480);
    for (unsigned big = 0; big <= 1; big++) {
        VjoVocrPolicy pol;
        VjoVocrStats st;
        VocrLine want[VJO_VOCR_MAX_LINES];
        unsigned more, n;
        reset();
        TEST_ASSERT(load(NULL) == VJO_OK);
        make_frame(big ? 960 : 480, big ? 544 : 272, 0, 0, big ? 960 : 480, big ? 544 : 272, 0);
        n = runtime_lines(16, 16, frame_w - 32, frame_h - 32, want, &more);
        pol = policy(16, 16, frame_w - 32, frame_h - 32);
        TEST_CHECK(job(&pol, 0, &st) == VJO_OK && st.spot_ran && same_lines(got, n_got, want, n));
        /* the frame's arena at its scale is the job's: the pin's for 960 x 544, less for 480 x 272 */
        size_t a = vocr_textspot6_frame_arena_bytes(&m, frame_w, frame_h);
        TEST_CHECK(big ? a == VJO_VOCR_SPOT_ARENA : a < VJO_VOCR_SPOT_ARENA);
        unsigned s1 = 0;
        if (!big) {
            /* halving this 480 x 272 frame (SCALE2) finds other boxes than its own scale: the rule decided */
            static VocrTextspot6Set halved;
            VocrLinesImage im = {grey, frame_w, frame_h, frame_w, VOCR_LINES_GREY};
            size_t a2 = vocr_textspot6_frame_arena_bytes_flags(&m, frame_w, frame_h, VOCR_TEXTSPOT6_SCALE2);
            TEST_ASSERT(a2 && a2 <= sizeof(runtime_arena));
            TEST_ASSERT(vocr_textspot6_find(&m, &im, VOCR_TEXTSPOT6_SCALE2, runtime_arena, a2, &halved) == VOCR_LINES_OK);
            s1 = halved.count != runtime_set.count ||
                 memcmp(halved.boxes, runtime_set.boxes, runtime_set.count * sizeof(halved.boxes[0]));
            TEST_CHECK(s1);
        }
    }
}

/* Every frame row is checked as the region's are: a frame row inside the region that reads otherwise than the
 * region's capture is a change of the screen (VJO_E_SCREEN_CHANGED after the tries), as a row the frame reads twice
 * differently is; a row that cannot be read is the job's error (no GRAD reading after it). */
static void the_frames_rows_are_checked_against_the_regions(void)
{
    VjoVocrPolicy pol;
    VjoVocrStats st;
    int rc;
    load_blob();
    load_textbox();
    make_frame(960, 544, 54, 380, 852, 135, 0);
    /* the region's capture holds its first row otherwise than the frame's read of it (its source reads that row
     * changed from its first strip on): a change between the capture and the spotter's frame, every try */
    reset();
    TEST_ASSERT(load(NULL) == VJO_OK);
    pol = policy(54, 380, 852, 135);
    region_src.change_at = 1;
    rc = job(&pol, 0, &st);
    TEST_CHECK_(rc == VJO_E_SCREEN_CHANGED && st.tries == VJO_VOCR_TRIES && st.tears >= VJO_VOCR_TRIES,
                "rc %d after %u tries, %u tears", rc, st.tries, st.tears);
    TEST_CHECK(!st.spot_ran && !n_got && frame_src.strips > 0);
    /* a frame row that cannot be read: the job's error, not GRAD's reading */
    reset();
    TEST_ASSERT(load(NULL) == VJO_OK);
    pol = policy(54, 380, 852, 135);
    frame_src.fail_at = 200;
    rc = job(&pol, 0, &st);
    TEST_CHECK_(rc == VJO_E_SOURCE && !n_got && !st.spot_ran && !st.spot_rc, "rc %d", rc);
    /* the frame read again unchanged: the same job reads its lines */
    pol = policy(54, 380, 852, 135);
    TEST_CHECK(job(&pol, 0, &st) == VJO_OK && st.spot_ran && st.tries == 1 && !st.tears);
}

/* GRAD's path reads the try whenever the spotter cannot run or refuses, exactly as a job without it: no verified
 * weights, no room for its part (a workspace of GRAD's size), a region outside the frame it is said to be cut from,
 * a refusal of the runtime's (an unloaded model), and, with ocr_vertical = auto, a region the spotter saw only
 * columns in. */
static void a_spotter_that_cannot_run_or_refuses_reads_with_grad(void)
{
    VjoVocrPolicy pol, grad;
    VjoVocrStats st;
    VocrLine want[VJO_VOCR_MAX_LINES + 1];
    unsigned n;
    load_blob();
    load_textbox();
    make_frame(960, 544, 54, 380, 852, 135, 0);
    /* GRAD's own lines (no spotter) */
    reset();
    grad = policy(54, 380, 852, 135);
    grad.spot = 0;
    TEST_CHECK(job(&grad, vjo_vocr_job_bytes("FL10_w8.vocr", 852, 135), &st) == VJO_OK);
    TEST_CHECK(!st.spot_ran && !st.spot_rc && n_got > 0 && !frame_src.strips);
    n = n_got <= VJO_VOCR_MAX_LINES ? n_got : VJO_VOCR_MAX_LINES;
    memcpy(want, got, n * sizeof(want[0]));
    /* no verified weights */
    pol = policy(54, 380, 852, 135);
    TEST_CHECK(job(&pol, vjo_vocr_job_bytes("FL10_w8.vocr", 852, 135), &st) == VJO_OK);
    TEST_CHECK(!st.spot_ran && st.spot_rc == VJO_E_OCR_MODEL && same_lines(got, n_got, want, n) && !st.spot_bytes);
    TEST_CHECK(!frame_src.strips);
    /* verified, but GRAD's workspace: no room for the spotter's part (its arena is over this pin's 4 KiB) */
    TEST_ASSERT(load(NULL) == VJO_OK);
    pol = policy(54, 380, 852, 135);
    TEST_CHECK(spot_need(&pol) > vjo_vocr_job_bytes("FL10_w8.vocr", 852, 135));
    TEST_CHECK(job(&pol, vjo_vocr_job_bytes("FL10_w8.vocr", 852, 135), &st) == VJO_OK);
    TEST_CHECK(!st.spot_ran && st.spot_rc == VJO_E_OOM && same_lines(got, n_got, want, n) && !frame_src.strips);
    /* a region the frame cannot hold where it is said to be */
    pol = policy(54, 380, 852, 135);
    pol.spot_x = 200;
    TEST_CHECK(job(&pol, 0, &st) == VJO_OK && !st.spot_ran && st.spot_rc == VJO_E_OOM);
    TEST_CHECK(same_lines(got, n_got, want, n) && !frame_src.strips);
    /* a parsed form the runtime does not take (unloaded): no arena it can size, GRAD reads the try */
    pol = policy(54, 380, 852, 135);
    vocr_textspot6 *parsed = (vocr_textspot6 *)(void *)(res.data + ALIGN16((size_t)VJO_VOCR_SPOT_BYTES));
    parsed->loaded = 0;
    TEST_CHECK(job(&pol, 0, &st) == VJO_OK && !st.spot_ran && st.spot_rc == VJO_E_OOM && !frame_src.strips);
    TEST_CHECK(same_lines(got, n_got, want, n));
    TEST_ASSERT(vocr_textspot6_load(parsed, res.data, VJO_VOCR_SPOT_BYTES) == VOCR_LINES_OK);
    /* with its room it runs, and reads other lines than GRAD */
    pol = policy(54, 380, 852, 135);
    TEST_CHECK(job(&pol, 0, &st) == VJO_OK && st.spot_ran && !st.spot_rc);
    TEST_CHECK(!same_lines(got, n_got, want, n));
}

/* ocr_vertical = auto (vertical_check): where the spotter's boxes centred in the region are all vertical columns,
 * GRAD's path reads the try (its check may read the region's columns); one horizontal box keeps the spotter's
 * lines. Without the check the spotter's empty set stands. */
static void columns_the_spotter_saw_go_to_grads_check(void)
{
    VjoVocrPolicy pol;
    VjoVocrStats st;
    VocrLine want[VJO_VOCR_MAX_LINES];
    unsigned more, found = 0, rx = 0, ry = 0, rw = 0, rh = 0;
    load_blob();
    load_textbox();
    reset();
    make_frame(960, 544, 0, 0, 960, 544, 0);
    TEST_ASSERT(load(NULL) == VJO_OK);
    (void)runtime_lines(0, 0, 960, 544, want, &more);
    /* a region around a vertical box that holds no horizontal box's centre */
    for (unsigned i = 0; i < runtime_set.count && !found; i++) {
        const VocrTextspot6Box *b = &runtime_set.boxes[i];
        if (!b->vertical) continue;
        rx = b->x;
        ry = b->y;
        rw = b->w < 32 ? 32 : b->w;
        rh = b->h < 32 ? 32 : b->h;
        if (rx + rw > 960 || ry + rh > 544) continue;
        found = 1;
        for (unsigned k = 0; k < runtime_set.count; k++) {
            const VocrTextspot6Box *o = &runtime_set.boxes[k];
            const unsigned cx = 2u * o->x + o->w, cy = 2u * o->y + o->h;
            if (!o->vertical && 2 * rx <= cx && cx < 2 * (rx + rw) && 2 * ry <= cy && cy < 2 * (ry + rh)) found = 0;
        }
    }
    if (!found) {
        TEST_MSG("no vertical box alone in a region on this frame (random weights)");
        TEST_SKIP("no such region");
        return;
    }
    TEST_CHECK(runtime_lines(rx, ry, rw, rh, want, &more) == 0);
    /* without the check: the spotter's set stands (no line) */
    pol = policy(rx, ry, rw, rh);
    TEST_CHECK(job(&pol, 0, &st) == VJO_OK && st.spot_ran && !n_got);
    /* with it: GRAD's path, whose rows (or columns: the caller's) read the region */
    pol = policy(rx, ry, rw, rh);
    pol.vertical_check = 1;
    int rc = job(&pol, 0, &st);
    TEST_CHECK_((rc == VJO_OK || rc == VJO_E_VERTICAL) && !st.spot_ran && st.spot_rc == VJO_E_VERTICAL, "rc %d", rc);
}

/* GRAD's refinement, short-line probe and text gate are GRAD's: a try whose spotter finds the lines reads them as
 * its post-pass gave them, runs no gate and adds no line. */
static void the_refinement_gate_and_probe_are_grads(void)
{
    VjoVocrPolicy pol;
    VjoVocrStats st;
    VocrLine want[VJO_VOCR_MAX_LINES];
    unsigned more, n;
    VjoVocrGateResident gate;
    load_blob();
    load_textbox();
    make_frame(960, 544, 54, 380, 852, 135, 0);
    reset();
    TEST_ASSERT(load(NULL) == VJO_OK);
    n = runtime_lines(54, 380, 852, 135, want, &more);
    TEST_ASSERT(n > 0);
    /* GRAD's refinement on: the spotter's boxes as the post-pass gave them */
    pol = policy(54, 380, 852, 135);
    pol.refine = VJO_VOCR_REFINE_ALL;
    TEST_CHECK(job(&pol, 0, &st) == VJO_OK && st.spot_ran && !st.refine_boxes && same_lines(got, n_got, want, n));
    /* the gate and the probe asked for: neither runs beside the spotter */
    memset(&gate, 0, sizeof(gate));
    pol = policy(54, 380, 852, 135);
    pol.gate = 1;
    pol.gate_res = &gate;
    pol.short_lines = 1;
    TEST_CHECK(job(&pol, 0, &st) == VJO_OK && st.spot_ran && same_lines(got, n_got, want, n));
    TEST_CHECK(!st.gate_ran && !st.gate_stopped);
}

/* The sizes: the spotter's part is its arena for the frame, its result, the post-pass's boxes and arena, and the
 * frame's strip and rows' state unless the region is the frame; it grows the scratch only where it is the larger
 * part, never past vjo_vocr_spot_bytes_max(). */
static void its_scratch_is_sized_exactly(void)
{
    vocr_textspot6 m;
    load_blob();
    reset();
    TEST_CHECK(!vjo_vocr_spot_bytes(&res, 852, 135, 960, 544)); /* nothing verified */
    TEST_ASSERT(load(NULL) == VJO_OK && vocr_textspot6_load(&m, blob, sizeof(blob)) == VOCR_LINES_OK);
    const size_t boxes = ALIGN16(VOCR_T7_MAX_BOXES * sizeof(VocrT7Box));
    for (unsigned w = 32; w <= 960; w += 116)
        for (unsigned h = 32; h <= 544; h += 64) {
            const size_t a = vocr_textspot6_frame_arena_bytes(&m, 960, 544);
            const size_t fixed = ALIGN16(a) + ALIGN16(sizeof(VocrTextspot6Set)) + 2 * boxes +
                                 ALIGN16(vocr_textspot7_rows_arena_bytes_steps(w, h, VJO_VOCR_SPOT_POST));
            const size_t frame = w == 960 && h == 544 ? 0 /* the region is the frame: its own stream */
                                 : ALIGN16((size_t)vjo_vocr_strip_rows(960, 544) * 960) +
                                       ALIGN16(4u * 544 + 2u * 2u * 544 + 2u * 68);
            size_t sb = vjo_vocr_spot_bytes(&res, w, h, 960, 544), grad = vjo_vocr_job_bytes("FL10_w8.vocr", w, h);
            size_t with = vjo_vocr_job_bytes_spot("FL10_w8.vocr", w, h, sb);
            TEST_CHECK_(sb == fixed + frame && sb <= vjo_vocr_spot_bytes_max(), "%ux%u: %zu", w, h, sb);
            TEST_CHECK(vjo_vocr_job_bytes_spot("FL10_w8.vocr", w, h, 0) == grad && with >= grad);
            /* the scratch is the larger of GRAD's and the spotter's: the job grows by the difference */
            size_t scratch = grad - ALIGN16(sizeof(vocr_model)) - ALIGN16(sizeof(vocr_ctx));
            TEST_CHECK(with == (ALIGN16(sb) > scratch ? grad + ALIGN16(sb) - scratch : grad));
        }
    /* the whole frame: no frame stream of its own (0 x 0 says the same) */
    TEST_CHECK(vjo_vocr_spot_bytes(&res, 960, 544, 960, 544) == vjo_vocr_spot_bytes(&res, 960, 544, 0, 0));
    TEST_CHECK(vjo_vocr_spot_bytes(&res, 960, 544, 960, 544) <
               vjo_vocr_spot_bytes(&res, 960, 543, 960, 544));
    /* a PSP-sized frame: its own arena and strip */
    TEST_CHECK(vjo_vocr_spot_bytes(&res, 400, 100, 480, 272) ==
               ALIGN16(vocr_textspot6_frame_arena_bytes(&m, 480, 272)) + ALIGN16(sizeof(VocrTextspot6Set)) +
                   2 * boxes + ALIGN16(3u * 400) + ALIGN16((size_t)vjo_vocr_strip_rows(480, 272) * 480) +
                   ALIGN16(4u * 272 + 2u * 2u * 272 + 2u * 34));
    /* refused: a region larger than its frame, a side over the job's, nothing verified */
    TEST_CHECK(!vjo_vocr_spot_bytes(&res, 500, 100, 480, 272) && !vjo_vocr_spot_bytes(&res, 100, 300, 480, 272));
    TEST_CHECK(!vjo_vocr_spot_bytes(&res, 0, 10, 0, 0) && !vjo_vocr_spot_bytes(&res, 961, 10, 0, 0));
    TEST_CHECK(!vjo_vocr_spot_bytes(&res, 100, 100, 961, 544));
    TEST_CHECK(!vjo_vocr_job_bytes_spot("no such model", 100, 100, 0));
    /* the ledger's worst case: the pin's arena, a region of the frame's width one row short of it */
    TEST_CHECK(vjo_vocr_spot_bytes_max() ==
               ALIGN16((size_t)VJO_VOCR_SPOT_ARENA) + ALIGN16(sizeof(VocrTextspot6Set)) + 2 * boxes +
                   ALIGN16(3u * 960) + ALIGN16((size_t)vjo_vocr_strip_rows(960, 544) * 960) +
                   ALIGN16(4u * 544 + 2u * 2u * 544 + 2u * 68));
    TEST_CHECK(vjo_vocr_spot_bytes_max() == vjo_vocr_spot_bytes(&res, 960, 543, 960, 544));
}

TEST_LIST = {
    {"the_fixture_is_the_pinned_blob", the_fixture_is_the_pinned_blob},
    {"the_weights_are_read_checked_and_kept", the_weights_are_read_checked_and_kept},
    {"a_missing_wrong_or_short_file_is_refused", a_missing_wrong_or_short_file_is_refused},
    {"no_memory_or_a_cancellation_is_not_a_crash", no_memory_or_a_cancellation_is_not_a_crash},
    {"the_job_reads_the_post_pass_of_the_runtimes_boxes", the_job_reads_the_post_pass_of_the_runtimes_boxes},
    {"a_box_whose_centre_is_outside_the_region_is_not_read", a_box_whose_centre_is_outside_the_region_is_not_read},
    {"the_frame_is_read_at_its_blobs_scale", the_frame_is_read_at_its_blobs_scale},
    {"the_frames_rows_are_checked_against_the_regions", the_frames_rows_are_checked_against_the_regions},
    {"a_spotter_that_cannot_run_or_refuses_reads_with_grad", a_spotter_that_cannot_run_or_refuses_reads_with_grad},
    {"columns_the_spotter_saw_go_to_grads_check", columns_the_spotter_saw_go_to_grads_check},
    {"the_refinement_gate_and_probe_are_grads", the_refinement_gate_and_probe_are_grads},
    {"its_scratch_is_sized_exactly", its_scratch_is_sized_exactly},
    {NULL, NULL}
};
