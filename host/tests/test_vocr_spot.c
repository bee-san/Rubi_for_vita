/* API 54: the text spotter as a job's finder (core/vocr_ocr.c, VjoVocrPolicy.spot), with the committed random VTSP
 * version-4 blob (host/fixtures/spotter; its pins test_spot_pins.h) and a synthetic recognizer pin with a 4 KiB arena
 * (test_spot_models.h): its weights' load and refusals (vjo_vocr_spot_load), the lines a job reads against the
 * runtime's own boxes on fixture frames (vocr_textspot6_find on the frame in memory: its horizontal boxes in its
 * order, with their dark flags, at most 16), the frame's scale, GRAD when it cannot run, its own refinement, no
 * gate or short-line probe beside it, and the sizes. No recognizer weights are read: every found line is skipped as
 * blank, so the job hands each line's crop to the line callback without recognizing it. Random weights find no
 * text: this makes no accuracy or timing claim. */
#include "acutest.h"
#include "vocr_ocr.h"
#include "vocr.h"
#include "vocr_lines.h"
#include "vocr_lines_refine.h"
#include "vocr_textspot6.h"
#include "../../include/vjo_vocr_sizes.h"
#include "../../core/fbgrey.h" /* vjo_grey: the counting source's rows */
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
static unsigned char grey[MAX_W * MAX_H], rgba[MAX_W * MAX_H * 4];
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
/* A w x h frame of the text box: tiled from (x0, y0), inverted (dark text on light) or not. */
static void make_frame(unsigned w, unsigned h, unsigned x0, unsigned y0, int invert)
{
    TEST_ASSERT(w <= MAX_W && h <= MAX_H);
    frame_w = w;
    frame_h = h;
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) {
            unsigned char v = textbox[((y + y0) % TB_H) * TB_W + (x + x0) % TB_W];
            if (invert) v = (unsigned char)(255u - v);
            grey[y * w + x] = v;
            rgba[(y * w + x) * 4] = rgba[(y * w + x) * 4 + 1] = rgba[(y * w + x) * 4 + 2] = v;
            rgba[(y * w + x) * 4 + 3] = 255;
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

/* ---- the runtime's own boxes on the frame in memory ---- */
static unsigned char runtime_arena[512 * 1024];
static VocrTextspot6Set runtime_set;
/* The runtime on the w x h frame at the scale flags: its horizontal boxes in its order, at most 16 (*more: past 16). */
static unsigned runtime_lines(unsigned flags, VocrLine *lines, unsigned *more)
{
    vocr_textspot6 m;
    VocrLinesImage im = {grey, frame_w, frame_h, frame_w, VOCR_LINES_GREY};
    size_t a;
    unsigned n = 0;
    TEST_ASSERT(vocr_textspot6_load(&m, blob, sizeof(blob)) == VOCR_LINES_OK);
    a = vocr_textspot6_arena_bytes_flags(&m, frame_w, frame_h, flags);
    TEST_ASSERT(a && a <= sizeof(runtime_arena));
    TEST_ASSERT(vocr_textspot6_find(&m, &im, flags, runtime_arena, a, &runtime_set) == VOCR_LINES_OK);
    *more = 0;
    for (unsigned i = 0; i < runtime_set.count; i++) {
        const VocrTextspot6Box *b = &runtime_set.boxes[i];
        if (b->vertical) continue;
        if (n == VJO_VOCR_MAX_LINES) {
            *more = 1;
            break;
        }
        lines[n++] = (VocrLine){b->x, b->y, b->w, b->h, b->dark};
    }
    return n;
}

/* ---- a job ---- */
static VocrLine got[VJO_VOCR_MAX_LINES + 1];
static unsigned n_got, bad_pixels;
static void record(void *ud, const VjoVocrLineInfo *info)
{
    (void)ud;
    if (n_got <= VJO_VOCR_MAX_LINES) got[n_got] = (VocrLine){info->x, info->y, info->w, info->h, info->dark};
    n_got++;
    /* the crop the recognizer would read is the frame's own pixels there */
    for (unsigned y = 0; y < info->h; y++)
        if (memcmp(info->pixels + (size_t)y * info->stride, grey + (size_t)(info->y + y) * frame_w + info->x, info->w)) {
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
static VjoVocrPolicy policy(void)
{
    VjoVocrPolicy p;
    memset(&p, 0, sizeof(p));
    p.blank = 1;
    p.blank_edges = 20000; /* every crop blank: nothing recognized, no recognizer file opened */
    p.line = record;
    p.spot = 1;
    p.spot_res = &res;
    return p;
}
/* The job on the frame, its workspace `workspace` bytes (0: the spotter's own, vjo_vocr_job_bytes_spot). */
static int job(const VjoVocrPolicy *pol, size_t workspace, VjoVocrStats *st)
{
    VjoRgbaImage img = {rgba, frame_w, frame_h, (size_t)frame_w * 4};
    VjoGreySource src;
    VjoVocrResident model;
    char text[256];
    size_t need = workspace ? workspace
                            : vjo_vocr_job_bytes_spot("FL10_w8.vocr", frame_w, frame_h,
                                                      vjo_vocr_spot_bytes(&res, frame_w, frame_h, pol->spot_frame_w,
                                                                          pol->spot_frame_h));
    if (pol->refine || pol->spot_refine) need += vjo_vocr_refine_bytes(frame_w, frame_h);
    TEST_ASSERT(need && need <= sizeof(ws_mem) && vjo_vocr_capture_bytes(frame_w, frame_h) <= sizeof(capture));
    vjo_grey_source_rgba(&img, &src);
    src.line_buffer = line_buffer;
    memset(&model, 0, sizeof(model));
    n_got = bad_pixels = 0;
    return vjo_vocr_ocr_policy(&platform, "card", "FL10_w8.vocr", &src, &model, ws_mem, need, capture,
                               vjo_vocr_capture_bytes(frame_w, frame_h), pol, text, sizeof(text), st);
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
    /* #418's audit F7 (#455's stated corner): an unverified block whose release failed is released again before the
     * next load takes a block, never read into: the next load frees it, then takes a fresh one, verified */
    reset();
    TEST_CHECK(load(&read) == VJO_OK && blk.live && blk.allocs == 1);
    card.missing = 1;
    blk.release_fail = 1;
    TEST_CHECK(load(&read) == VJO_E_OCR_UNAVAILABLE && !res.verified && res.data && blk.live);
    card.missing = 0;
    blk.release_fail = 0;
    {
        const unsigned releases = blk.releases;
        TEST_CHECK(load(&read) == VJO_OK && read == 1 && res.verified && blk.live);
        TEST_CHECK_(blk.releases == releases + 1u && blk.allocs == 2, "releases %u (+%u), allocs %u", blk.releases,
                    blk.releases - releases, blk.allocs);
    }
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

/* The job's lines are the runtime's own horizontal boxes, in its order, with their dark flags, at most 16, from the
 * frame's pixels: frames of one strip and of several, light and dark text, one with fewer boxes than the cap. */
static void the_job_reads_the_runtimes_boxes(void)
{
    static const struct { unsigned w, h, x0, y0; int invert; } frames[] = {
        {TB_W, TB_H, 0, 0, 0},       /* the text box: more boxes than the job keeps */
        {TB_W, TB_H, 0, 0, 1},       /* dark text on light: dark flags */
        {MAX_W, 405u, 40, 0, 0},     /* a tall region, several strips, tiled */
        {300u, 48u, 8, 20, 0},       /* a small one: one strip */
        {120u, 40u, 500, 60, 1},
    };
    load_blob();
    load_textbox();
    for (unsigned k = 0; k < sizeof(frames) / sizeof(frames[0]); k++) {
        VjoVocrPolicy pol = policy();
        VjoVocrStats st;
        VocrLine want[VJO_VOCR_MAX_LINES];
        unsigned more, n;
        reset();
        make_frame(frames[k].w, frames[k].h, frames[k].x0, frames[k].y0, frames[k].invert);
        TEST_ASSERT(load(NULL) == VJO_OK);
        n = runtime_lines(VOCR_TEXTSPOT6_SCALE2, want, &more);
        TEST_CHECK_(job(&pol, 0, &st) == VJO_OK, "frame %u", k);
        TEST_CHECK_(st.spot_ran == 1 && !st.spot_rc && st.lines == n && st.truncated == more && n_got == n,
                    "frame %u: %u lines (%u read), runtime %u%s", k, st.lines, n_got, n, more ? "+" : "");
        TEST_CHECK_(same_lines(got, n_got, want, n), "frame %u: the runtime's boxes", k);
        TEST_CHECK_(!bad_pixels, "frame %u: every crop the frame's own", k);
        TEST_CHECK(st.spot_boxes == runtime_set.count && st.spot_truncated == runtime_set.truncated);
        TEST_CHECK(st.spot_bytes == vjo_vocr_spot_bytes(&res, frame_w, frame_h, 0, 0) && st.spot_us > 0);
        TEST_CHECK(st.blank == n && !st.recognized && !card.other_opens); /* no recognizer weights opened */
        unsigned dark = 0;
        for (unsigned i = 0; i < n; i++) dark += want[i].dark ? 1u : 0u;
        TEST_CHECK(st.dark == dark);
        if (!k) TEST_CHECK_(more && n == VJO_VOCR_MAX_LINES, "the text box: past the cap (%u)", n);
        if (k == 1) TEST_CHECK_(dark > 0, "dark text: %u dark lines", dark);
        if (k >= 3) TEST_CHECK_(!more, "frame %u: under the cap", k);
    }
}

/* audit-vocr6-spotter2-rubi F2: a model of columns reads without the spotter, so its job with the spotter's part is
 * its job (the rotation buffer kept), whatever that part; a model of rows grows by what the part exceeds its scratch. */
static void a_model_of_columns_takes_no_part_of_the_spotters_scratch(void)
{
    const size_t big = vjo_vocr_spot_bytes_max();
    TEST_CHECK(vjo_vocr_job_bytes_spot(VJO_VOCR_VERTICAL_NAME, 960, 544, big) ==
               vjo_vocr_job_bytes(VJO_VOCR_VERTICAL_NAME, 960, 544));
    TEST_CHECK(vjo_vocr_job_bytes_spot(VJO_VOCR_VERTICAL_NAME, TB_W, TB_H, big) ==
               vjo_vocr_job_bytes(VJO_VOCR_VERTICAL_NAME, TB_W, TB_H));
    TEST_CHECK(vjo_vocr_job_bytes_spot("FL10_w8.vocr", 960, 544, big) > vjo_vocr_job_bytes("FL10_w8.vocr", 960, 544));
    TEST_CHECK(vjo_vocr_job_bytes_spot("FL10_w8.vocr", 960, 544, 0) == vjo_vocr_job_bytes("FL10_w8.vocr", 960, 544));
}

/* API 54: with ocr_vertical = auto (VjoVocrPolicy.vertical_check VJO_VOCR_CHECK_AUTO) the spotter's own box counts
 * decide whether its boxes are the lines or GRAD's rows go to auto's check: it hands over a region it calls vertical
 * more often than horizontal. With on (VJO_VOCR_CHECK_ON, #379) the spotter is not set up at all, whatever its boxes
 * (audit-vocr6-spotter2-rubi F1): on's job is GRAD's rows and on's check; off never hands over. Each frame is checked
 * against the runtime's own counts; the random blob's frames below give all three count cases (vita-vn-ocr's twin on
 * them: the text box 43 vertical / 21 horizontal boxes, the small inverted crop 0 / 1, the uniform grey 1 / 1). */
static void vertical_text_checks_hand_over_by_the_spotters_counts(void)
{
    static const struct { unsigned w, h, x0, y0; int invert, uniform; } frames[] = {
        {TB_W, TB_H, 0, 0, 0, 0},   /* mostly vertical boxes: auto hands over */
        {120u, 40u, 500, 60, 1, 0}, /* mostly horizontal: auto does not */
        {TB_W, TB_H, 0, 0, 0, 90},  /* as many of each: auto does not */
    };
    static const int checks[] = {0, VJO_VOCR_CHECK_AUTO, VJO_VOCR_CHECK_ON};
    unsigned seen_more_vertical = 0, seen_more_horizontal = 0, seen_tie = 0;
    load_blob();
    load_textbox();
    for (unsigned k = 0; k < sizeof(frames) / sizeof(frames[0]); k++) {
        unsigned vertical = 0, horizontal = 0;
        for (unsigned c = 0; c < sizeof(checks) / sizeof(checks[0]); c++) {
            VjoVocrPolicy pol = policy();
            VjoVocrStats st;
            VocrLine want[VJO_VOCR_MAX_LINES];
            unsigned more;
            reset();
            make_frame(frames[k].w, frames[k].h, frames[k].x0, frames[k].y0, frames[k].invert);
            if (frames[k].uniform) /* one grey value everywhere (grey = (77 + 150 + 29) v >> 8 = v) */
                for (size_t i = 0; i < (size_t)frame_w * frame_h; i++) {
                    grey[i] = (unsigned char)frames[k].uniform;
                    rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = (unsigned char)frames[k].uniform;
                    rgba[i * 4 + 3] = 255;
                }
            TEST_ASSERT(load(NULL) == VJO_OK);
            (void)runtime_lines(VOCR_TEXTSPOT6_SCALE2, want, &more);
            vertical = horizontal = 0;
            for (unsigned i = 0; i < runtime_set.count; i++) {
                if (runtime_set.boxes[i].vertical) vertical++;
                else horizontal++;
            }
            pol.vertical_check = checks[c];
            int rc = job(&pol, 0, &st);
            if (checks[c] == VJO_VOCR_CHECK_ON) /* no spotter: GRAD's rows and on's check (read, or columns next) */
                TEST_CHECK_(!st.spot_ran && !st.spot_rc && !st.spot_bytes && !st.spot_boxes &&
                                (rc == VJO_OK || rc == VJO_E_VERTICAL),
                            "frame %u on: rc %d spot_rc %d ran %u bytes %zu", k, rc, st.spot_rc, st.spot_ran,
                            st.spot_bytes);
            else if (checks[c] == VJO_VOCR_CHECK_AUTO && vertical > horizontal) /* GRAD's rows and auto's check */
                TEST_CHECK_(st.spot_rc == VJO_E_VERTICAL && !st.spot_ran && (rc == VJO_OK || rc == VJO_E_VERTICAL),
                            "frame %u auto (%u vertical, %u horizontal): rc %d spot_rc %d ran %u", k, vertical,
                            horizontal, rc, st.spot_rc, st.spot_ran);
            else
                TEST_CHECK_(rc == VJO_OK && st.spot_ran == 1 && !st.spot_rc,
                            "frame %u check %d (%u vertical, %u horizontal): rc %d spot_rc %d ran %u", k, checks[c],
                            vertical, horizontal, rc, st.spot_rc, st.spot_ran);
        }
        seen_more_vertical += vertical > horizontal;
        seen_more_horizontal += horizontal > vertical;
        seen_tie += vertical == horizontal;
    }
    TEST_CHECK_(seen_more_vertical == 1 && seen_more_horizontal == 1 && seen_tie == 1,
                "cases: more vertical %u, more horizontal %u, tie %u", seen_more_vertical, seen_more_horizontal,
                seen_tie);
}

/* The scale its blob's rule gives the frame the region is cut from: every region of a 960 x 544 frame halved
 * (SCALE2, the default), one of a 480 x 272 frame at its own size (SCALE1). */
static void the_region_is_read_at_its_frames_scale(void)
{
    static const struct { unsigned fw, fh, flags; } cases[] = {
        {0, 0, VOCR_TEXTSPOT6_SCALE2}, {960, 544, VOCR_TEXTSPOT6_SCALE2}, {720, 408, VOCR_TEXTSPOT6_SCALE2},
        {480, 272, VOCR_TEXTSPOT6_SCALE1},
    };
    load_blob();
    load_textbox();
    make_frame(400u, TB_H, 30, 0, 0);
    for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        VjoVocrPolicy pol = policy();
        VjoVocrStats st;
        VocrLine want[VJO_VOCR_MAX_LINES];
        unsigned more, n;
        reset();
        TEST_ASSERT(load(NULL) == VJO_OK);
        pol.spot_frame_w = cases[k].fw;
        pol.spot_frame_h = cases[k].fh;
        n = runtime_lines(cases[k].flags, want, &more);
        TEST_CHECK(job(&pol, 0, &st) == VJO_OK && st.spot_ran);
        TEST_CHECK_(same_lines(got, n_got, want, n), "frame %ux%u: the runtime's boxes at its scale", cases[k].fw,
                    cases[k].fh);
    }
    /* the two scales differ on this region (so the test can tell them apart) */
    VocrLine a[VJO_VOCR_MAX_LINES], b[VJO_VOCR_MAX_LINES];
    unsigned ma, mb, na = runtime_lines(VOCR_TEXTSPOT6_SCALE1, a, &ma), nb = runtime_lines(VOCR_TEXTSPOT6_SCALE2, b, &mb);
    TEST_CHECK(!same_lines(a, na, b, nb));
}

/* GRAD finds the lines when the spotter cannot run, exactly as a job without it: no verified weights, or no room
 * for its scratch (a workspace of GRAD's size). */
static void without_its_weights_or_room_the_job_reads_with_grad(void)
{
    VjoVocrPolicy pol, grad;
    VjoVocrStats st;
    VocrLine want[VJO_VOCR_MAX_LINES + 1];
    unsigned n;
    load_blob();
    load_textbox();
    make_frame(TB_W, TB_H, 0, 0, 0);
    /* GRAD's own lines (no spotter) */
    reset();
    grad = policy();
    grad.spot = 0;
    TEST_CHECK(job(&grad, vjo_vocr_job_bytes("FL10_w8.vocr", frame_w, frame_h), &st) == VJO_OK);
    TEST_CHECK(!st.spot_ran && !st.spot_rc && n_got > 0);
    n = n_got <= VJO_VOCR_MAX_LINES ? n_got : VJO_VOCR_MAX_LINES;
    memcpy(want, got, n * sizeof(want[0]));
    /* no verified weights */
    pol = policy();
    TEST_CHECK(job(&pol, vjo_vocr_job_bytes("FL10_w8.vocr", frame_w, frame_h), &st) == VJO_OK);
    TEST_CHECK(!st.spot_ran && st.spot_rc == VJO_E_OCR_MODEL && same_lines(got, n_got, want, n) && !st.spot_bytes);
    /* verified, but GRAD's workspace: no room for the spotter's scratch (its arena is over this pin's 4 KiB) */
    TEST_ASSERT(load(NULL) == VJO_OK);
    size_t spot = vjo_vocr_spot_bytes(&res, frame_w, frame_h, 0, 0);
    TEST_CHECK(vjo_vocr_job_bytes_spot("FL10_w8.vocr", frame_w, frame_h, spot) >
               vjo_vocr_job_bytes("FL10_w8.vocr", frame_w, frame_h));
    TEST_CHECK(job(&pol, vjo_vocr_job_bytes("FL10_w8.vocr", frame_w, frame_h), &st) == VJO_OK);
    TEST_CHECK(!st.spot_ran && st.spot_rc == VJO_E_OOM && same_lines(got, n_got, want, n));
    /* with its room it runs */
    TEST_CHECK(job(&pol, 0, &st) == VJO_OK && st.spot_ran && !st.spot_rc);
    TEST_CHECK(!same_lines(got, n_got, want, n));
}

/* Its boxes are refined by its own setting (spot_refine), never by GRAD's (refine), and the text gate and the
 * short-line probe are GRAD's: a job whose spotter finds the lines reads no gate weights and adds no line. */
static void its_refinement_is_its_own_and_the_gate_and_probe_are_grads(void)
{
    VjoVocrPolicy pol;
    VjoVocrStats st;
    VocrLine want[VJO_VOCR_MAX_LINES];
    unsigned more, n;
    VjoVocrGateResident gate;
    load_blob();
    load_textbox();
    make_frame(300u, 60u, 120, 10, 0);
    reset();
    TEST_ASSERT(load(NULL) == VJO_OK);
    n = runtime_lines(VOCR_TEXTSPOT6_SCALE2, want, &more);
    TEST_ASSERT(n > 0);
    /* GRAD's refinement on, the spotter's off: its boxes as it found them */
    pol = policy();
    pol.refine = VJO_VOCR_REFINE_ALL;
    TEST_CHECK(job(&pol, 0, &st) == VJO_OK && st.spot_ran && !st.refine_boxes && same_lines(got, n_got, want, n));
    /* the spotter's on: every box refined (into one line or more) */
    pol = policy();
    pol.spot_refine = VJO_VOCR_REFINE_ALL;
    TEST_CHECK(job(&pol, 0, &st) == VJO_OK && st.spot_ran && st.refine_boxes == n && st.refine_lines >= n);
    TEST_CHECK(!st.refine_rc && n_got == st.refine_lines && !bad_pixels);
    /* the gate and the probe asked for: neither runs beside the spotter */
    memset(&gate, 0, sizeof(gate));
    pol = policy();
    pol.gate = 1;
    pol.gate_res = &gate;
    pol.short_lines = 1;
    card.opens = 0;
    TEST_CHECK(job(&pol, 0, &st) == VJO_OK && st.spot_ran && same_lines(got, n_got, want, n));
    TEST_CHECK(!st.gate_ran && !st.gate_rc && !st.gate_read && card.opens == 0); /* no gate weights opened */
}

/* The sizes: the spotter's part is its arena at the frame's scale and its result; it grows the scratch only where it
 * is the larger part, and never past vjo_vocr_spot_bytes_max(). */
static void its_scratch_is_sized_exactly(void)
{
    vocr_textspot6 m;
    load_blob();
    reset();
    TEST_CHECK(!vjo_vocr_spot_bytes(&res, 852, 135, 0, 0)); /* nothing verified */
    TEST_ASSERT(load(NULL) == VJO_OK && vocr_textspot6_load(&m, blob, sizeof(blob)) == VOCR_LINES_OK);
    for (unsigned w = 8; w <= 960; w += 119)
        for (unsigned h = 8; h <= 544; h += 67) {
            size_t a = vocr_textspot6_arena_bytes_flags(&m, w, h, VOCR_TEXTSPOT6_SCALE2);
            size_t sb = vjo_vocr_spot_bytes(&res, w, h, 0, 0), grad = vjo_vocr_job_bytes("FL10_w8.vocr", w, h);
            size_t with = vjo_vocr_job_bytes_spot("FL10_w8.vocr", w, h, sb);
            TEST_CHECK_(a && sb == ALIGN16(a) + ALIGN16(sizeof(VocrTextspot6Set)) && sb <= vjo_vocr_spot_bytes_max(),
                        "%ux%u: %zu", w, h, sb);
            TEST_CHECK(vjo_vocr_job_bytes_spot("FL10_w8.vocr", w, h, 0) == grad && with >= grad);
            /* the scratch is the larger of GRAD's and the spotter's: the job grows by the difference */
            size_t scratch = grad - ALIGN16(sizeof(vocr_model)) - ALIGN16(sizeof(vocr_ctx));
            TEST_CHECK(with == (ALIGN16(sb) > scratch ? grad + ALIGN16(sb) - scratch : grad));
            TEST_CHECK(vjo_vocr_spot_bytes(&res, w, h, 480, 272) ==
                       ALIGN16(vocr_textspot6_arena_bytes_flags(&m, w, h, VOCR_TEXTSPOT6_SCALE1)) +
                           ALIGN16(sizeof(VocrTextspot6Set)));
        }
    TEST_CHECK(!vjo_vocr_spot_bytes(&res, 0, 10, 0, 0) && !vjo_vocr_spot_bytes(&res, 961, 10, 0, 0));
    TEST_CHECK(!vjo_vocr_job_bytes_spot("no such model", 100, 100, 0));
    /* the ledger's worst case: the protocol's arena ceiling and the result */
    TEST_CHECK(vjo_vocr_spot_bytes_max() == ALIGN16((size_t)VJO_VOCR_SPOT_ARENA_MAX) + ALIGN16(sizeof(VocrTextspot6Set)));
}

/* #545's audit, finding 6: with the stable-text wait off (and without the pause), every row of a region taller than a
 * strip is read again after the spotter's pass, before its lines are recognized, so a change in rows where it found no
 * box is seen: the job then reads the region again (a second try) and gives the new frame's boxes. Counted with a
 * source that switches to a changed frame right after the first pass and records every read row. */
static unsigned char rgba_b[MAX_W * MAX_H * 4];
static unsigned row_reads[MAX_H], first_pass_reads, rows_read_total;
static int switch_after_first; /* the frame changes once the first pass is in (the read with VJO_STRIP_LAST) */
static int switched;
static int counting_strip(void *ud, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags)
{
    const unsigned char *src = switched ? rgba_b : rgba;
    (void)ud;
    if (y >= frame_h || n > frame_h - y) return VJO_E_SOURCE;
    for (unsigned r = 0; r < n; r++) {
        const unsigned char *p = src + (size_t)(y + r) * frame_w * 4;
        for (unsigned x = 0; x < frame_w; x++, p += 4) dst[(size_t)r * stride + x] = (unsigned char)vjo_grey(p[0], p[1], p[2]);
        row_reads[y + r]++;
    }
    rows_read_total += n;
    if (flags & VJO_STRIP_FIRST) first_pass_reads++;
    if (switch_after_first && (flags & VJO_STRIP_LAST)) switched = 1;
    return (int)n;
}
static int counting_job(const VjoVocrPolicy *pol, VjoVocrStats *st)
{
    VjoGreySource src;
    VjoVocrResident model;
    char text[256];
    const size_t need = vjo_vocr_job_bytes_spot("FL10_w8.vocr", frame_w, frame_h,
                                                vjo_vocr_spot_bytes(&res, frame_w, frame_h, 0, 0));
    TEST_ASSERT(need && need <= sizeof(ws_mem) && vjo_vocr_capture_bytes(frame_w, frame_h) <= sizeof(capture));
    memset(&src, 0, sizeof(src));
    src.width = frame_w;
    src.height = frame_h;
    src.strip = counting_strip;
    src.line_buffer = line_buffer;
    memset(&model, 0, sizeof(model));
    memset(row_reads, 0, sizeof(row_reads));
    first_pass_reads = rows_read_total = 0;
    switched = 0;
    n_got = bad_pixels = 0;
    return vjo_vocr_ocr_policy(&platform, "card", "FL10_w8.vocr", &src, &model, ws_mem, need, capture,
                               vjo_vocr_capture_bytes(frame_w, frame_h), pol, text, sizeof(text), st);
}
static void without_the_wait_every_row_is_read_again(void)
{
    VjoVocrStats st;
    VjoVocrPolicy p;
    VocrLine want[VJO_VOCR_MAX_LINES];
    unsigned more = 0, n;
    load_blob();
    load_textbox();
    reset();
    TEST_ASSERT(load(NULL) == VJO_OK);
    p = policy();
    /* the textbox twice, 852 x 270: five strips of 64 rows (vjo_vocr_strip_rows) */
    make_frame(TB_W, 2u * TB_H, 0, 0, 0);
    TEST_ASSERT(vjo_vocr_strip_rows(frame_w, frame_h) < frame_h);
    switch_after_first = 0;
    TEST_ASSERT(counting_job(&p, &st) == VJO_OK);
    TEST_CHECK(st.spot_ran && st.tries == 1 && !st.settle_passes);
    for (unsigned y = 0; y < frame_h; y++)
        TEST_CHECK_(row_reads[y] >= 2, "row %u read %u time(s): the wait off, every row is read again", y, row_reads[y]);
    /* a change in every row once the first pass is in (the frame inverted): the second read sees it, the job reads
     * again and gives the changed frame's boxes, each crop its pixels */
    {
        static unsigned char plain[MAX_W * MAX_H * 4];
        memcpy(plain, rgba, (size_t)frame_w * frame_h * 4);
        make_frame(TB_W, 2u * TB_H, 0, 0, 1);     /* grey and rgba: the inverted frame, the one the job must give */
        memcpy(rgba_b, rgba, (size_t)frame_w * frame_h * 4);
        memcpy(rgba, plain, (size_t)frame_w * frame_h * 4); /* the job starts on the plain frame */
    }
    switch_after_first = 1;
    TEST_ASSERT(counting_job(&p, &st) == VJO_OK);
    TEST_CHECK_(st.tries == 2 && st.tears > 0, "%u tries, %u tears", st.tries, st.tears);
    memcpy(rgba, rgba_b, (size_t)frame_w * frame_h * 4); /* record() compares each crop with grey: the inverted frame */
    n = runtime_lines(VOCR_TEXTSPOT6_SCALE2, want, &more);
    TEST_CHECK_(same_lines(got, n_got, want, n) && !bad_pixels, "%u lines against the changed frame's %u, %u bad crops",
                n_got, n, bad_pixels);
}

/* #528: with the stable-text wait on, a spotter job whose finding ends before the wait's due time (settle_us after its
 * first pass began) sleeps until then and reads the region once (one pass that counts), not at once and again after
 * the sleep; a change during the sleep is seen by that pass (a second try, the changed frame's boxes). */
static unsigned sleeps;
static uint64_t slept_until;
static void test_sleep(uint32_t us)
{
    sleeps++;
    clock_now += us;
    slept_until = clock_now;
    if (switch_after_first == 2) switched = 1; /* the screen changes while the job sleeps */
}
static void the_wait_reads_one_pass_at_its_due_time(void)
{
    VjoVocrStats st;
    VjoVocrPolicy p;
    VocrLine want[VJO_VOCR_MAX_LINES];
    unsigned more = 0, n;
    load_blob();
    load_textbox();
    reset();
    TEST_ASSERT(load(NULL) == VJO_OK);
    p = policy();
    p.settle_us = 150000u;
    p.settle_max_us = 2000000u;
    p.sleep = test_sleep;
    make_frame(TB_W, 2u * TB_H, 0, 0, 0); /* five strips; the clock moves 7 us a reading: the finding ends early */
    switch_after_first = 0;
    sleeps = 0;
    TEST_ASSERT(counting_job(&p, &st) == VJO_OK);
    TEST_CHECK_(st.spot_ran && st.tries == 1 && st.settled && st.settle_passes == 1 && sleeps == 1,
                "spot %u, %u tries, settled %u, %u stable-text passes, %u sleeps", st.spot_ran, st.tries, st.settled,
                st.settle_passes, sleeps);
    for (unsigned y = 0; y < frame_h; y++) TEST_CHECK_(row_reads[y] >= 2, "row %u read %u time(s)", y, row_reads[y]);
    /* the screen changes during the sleep: the pass at the due time sees it, and the job gives the new frame */
    {
        static unsigned char plain[MAX_W * MAX_H * 4];
        memcpy(plain, rgba, (size_t)frame_w * frame_h * 4);
        make_frame(TB_W, 2u * TB_H, 0, 0, 1);
        memcpy(rgba_b, rgba, (size_t)frame_w * frame_h * 4);
        memcpy(rgba, plain, (size_t)frame_w * frame_h * 4);
    }
    switch_after_first = 2;
    sleeps = 0;
    TEST_ASSERT(counting_job(&p, &st) == VJO_OK);
    switch_after_first = 0;
    TEST_CHECK_(st.tries == 2 && st.tears > 0, "%u tries, %u tears", st.tries, st.tears);
    n = runtime_lines(VOCR_TEXTSPOT6_SCALE2, want, &more);
    TEST_CHECK_(same_lines(got, n_got, want, n) && !bad_pixels, "%u lines against the changed frame's %u, %u bad crops",
                n_got, n, bad_pixels);
}

TEST_LIST = {
    {"the_fixture_is_the_pinned_blob", the_fixture_is_the_pinned_blob},
    {"the_weights_are_read_checked_and_kept", the_weights_are_read_checked_and_kept},
    {"a_missing_wrong_or_short_file_is_refused", a_missing_wrong_or_short_file_is_refused},
    {"no_memory_or_a_cancellation_is_not_a_crash", no_memory_or_a_cancellation_is_not_a_crash},
    {"the_job_reads_the_runtimes_boxes", the_job_reads_the_runtimes_boxes},
    {"the_region_is_read_at_its_frames_scale", the_region_is_read_at_its_frames_scale},
    {"without_its_weights_or_room_the_job_reads_with_grad", without_its_weights_or_room_the_job_reads_with_grad},
    {"its_refinement_is_its_own_and_the_gate_and_probe_are_grads",
     its_refinement_is_its_own_and_the_gate_and_probe_are_grads},
    {"its_scratch_is_sized_exactly", its_scratch_is_sized_exactly},
    {"vertical_text_checks_hand_over_by_the_spotters_counts", vertical_text_checks_hand_over_by_the_spotters_counts},
    {"a_model_of_columns_takes_no_part_of_the_spotters_scratch", a_model_of_columns_takes_no_part_of_the_spotters_scratch},
    {"without_the_wait_every_row_is_read_again", without_the_wait_every_row_is_read_again},
    {"the_wait_reads_one_pass_at_its_due_time", the_wait_reads_one_pass_at_its_due_time},
    {NULL, NULL}
};
