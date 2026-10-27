/* The kernel's capture code (kernel/capture.c) with failure-injecting SDK boundaries: the job's
 * crop of the region, the game OCR worker's rows (API 19: grey; API 23, a shot: colour) and the
 * first pass's signature. API 24 removed the kernel's full-frame capture buffer (VjoCapture,
 * vjoRequestCapture, vjoReadRaw) and its tests; its crop and pixel-format checks are below, on
 * the paths that still use them. */
#include "acutest.h"
#include <psp2kern/host_stubs.h>
#include "../../kernel/vjo_kernel.h"
#include "../../kernel/capture.c"
#include "../../core/region_select.h"

VjoKernelState g;
unsigned syscall_depth;
static unsigned game_copies;
static int game_copy_failure;
int ksceKernelMemcpyUserToKernel(void *dst, const void *src, SceSize n) {
    game_copies++;
    if (game_copy_failure) return -1;
    memcpy(dst, src, n); return 0;
}
static void setup(void) {
    memset(&g, 0, sizeof(g));
    g.game_pid = 7; g.game_active = VJO_GAME; g.shell_pid = 3;
    game_copies = 0; game_copy_failure = 0;
}

/* The job's crop of the saved region (capture_crop_of; vjoOcrSubmit takes it in the game's last
 * frame, region_sig_hook in each sampled one): a dialogue box on a 960 x 544 frame, a region of
 * a small frame, the whole frame for an empty region, a frame over 960 x 544 cut to it, and a
 * region under the 32 px minimum grown to it inside the frame, x even. The first two were the
 * full-frame capture's checks until API 24. */
static void region_crops(void)
{
    uint32_t x, y, w, h;
    const VjoRect box = {6000, 45000, 51500, 17500}, part = {16384, 32768, 32768, 32768};
    const VjoRect none = {0, 0, 0, 0}, tiny = {65000, 65000, 100, 100};
    capture_crop_of(&box, 960, 544, &x, &y, &w, &h);
    TEST_CHECK(x == 86 && y == 373 && w == 755 && h == 146);
    TEST_MSG("box: %u,%u %ux%u", x, y, w, h);
    capture_crop_of(&part, 64, 64, &x, &y, &w, &h);
    TEST_CHECK(x == 16 && y == 32 && w == 32 && h == 32);
    capture_crop_of(&none, 960, 544, &x, &y, &w, &h);
    TEST_CHECK(x == 0 && y == 0 && w == 960 && h == 544);
    capture_crop_of(&none, 1280, 720, &x, &y, &w, &h);
    TEST_CHECK(x == 0 && y == 0 && w == VJO_MAX_W && h == VJO_MAX_H);
    capture_crop_of(&tiny, 960, 544, &x, &y, &w, &h);
    TEST_CHECK(x == 928 && y == 512 && w == 32 && h == 32);
    TEST_MSG("tiny: %u,%u %ux%u", x, y, w, h);
    setup(); g.region = box; /* capture_compute_crop: the same for the saved region */
    capture_compute_crop(960, 544, &x, &y, &w, &h);
    TEST_CHECK(x == 86 && y == 373 && w == 755 && h == 146);
}

/* A box drawn with the region selector (core/region_select.c, the Shell) and
 * saved as a VjoRect is cropped by this code as exactly that box on a
 * 960 x 544 frame, its x rounded down to even as always: every position and
 * every size from the selector's 32 px minimum, both axes. So the size the
 * selector shows is the size the game reads. */
static void drawn_box_is_the_captured_crop(void)
{
    uint32_t x, y, w, h, cx, cy, cw, ch;
    for (x = 0; x < 960; x++)
        for (w = VJO_RSEL_MIN; w <= 960 - x; w++) {
            VjoRect r = vjo_rsel_rect((int)x, 0, (int)w, 544);
            capture_crop_of(&r, 960, 544, &cx, &cy, &cw, &ch);
            if (cx != (x & ~1u) || cw != w || cy != 0 || ch != 544) {
                TEST_CHECK_(0, "box x %u w %u cropped as %u,%u %ux%u", x, w, cx, cy, cw, ch);
                return;
            }
        }
    for (y = 0; y < 544; y++)
        for (h = VJO_RSEL_MIN; h <= 544 - y; h++) {
            VjoRect r = vjo_rsel_rect(0, (int)y, 960, (int)h);
            capture_crop_of(&r, 960, 544, &cx, &cy, &cw, &ch);
            if (cy != y || ch != h || cx != 0 || cw != 960) {
                TEST_CHECK_(0, "box y %u h %u cropped as %u,%u %ux%u", y, h, cx, cy, cw, ch);
                return;
            }
        }
}

/* #358: the text box suggested on first use, the screen's bottom rows drawn from the start (vjo_rsel_preset) and saved
 * as they are, is cropped as those rows: on the 960 x 544 screen, and on a PSP game's own 480 x 272 frame, half of
 * them (the game worker's band there has an even number of rows on the screen: core/region_suggest.c
 * vjo_region_rows, host/tests/test_region_suggest.c), every height from the selector's minimum. A band under the
 * capture's 32 rows of that frame is grown upward to them, as any region is (capture_crop_of). */
static void suggested_band_is_the_captured_rows(void)
{
    for (uint32_t rows = VJO_RSEL_MIN; rows <= VJO_RSEL_H; rows++) {
        VjoRegionSelect s;
        int bx, by, bw, bh;
        uint32_t x, y, w, h;
        vjo_rsel_begin(&s, 0);
        TEST_ASSERT(vjo_rsel_preset(&s, rows) && vjo_rsel_box(&s, &bx, &by, &bw, &bh));
        VjoRect r = vjo_rsel_rect(bx, by, bw, bh);
        capture_crop_of(&r, 960, 544, &x, &y, &w, &h);
        if (x || w != 960 || y != 544 - rows || h != rows) {
            TEST_CHECK_(0, "%u rows cropped as %u,%u %ux%u on the screen", rows, x, y, w, h);
            return;
        }
        capture_crop_of(&r, 480, 272, &x, &y, &w, &h);
        const uint32_t psp = rows / 2 < 32 ? 32 : rows / 2;
        if (!(rows & 1) && (x || w != 480 || y != 272 - psp || h != psp)) {
            TEST_CHECK_(0, "%u rows cropped as %u,%u %ux%u on a PSP frame", rows, x, y, w, h);
            return;
        }
    }
}

/* The OCR worker's grey rows (API 19): each format expanded to 8 bits as the
 * colour rows expand it (read_row), then (77 R + 150 G + 29 B) >> 8; a frame of another
 * size or format is refused; a failed copy into the worker is reported. */
static unsigned char grey_out[64 * 32];
static int user_copy_failure;
int ksceKernelMemcpyKernelToUser(void *dst, const void *src, SceSize n)
{ if (user_copy_failure) return -1; memcpy(dst, src, n); return 0; }
static void ocr_grey_rows_every_format(void)
{
    static uint16_t fb16[40 * 32];
    static uint32_t fb32[40 * 32];
    for (unsigned i = 0; i < 40 * 32; i++) {
        fb16[i] = (uint16_t)(i * 2654435761u >> 16);
        fb32[i] = i * 2654435761u;
    }
    for (int f = 0; f < 3; f++) {
        uint32_t fmt = f == 0 ? VJO_FB_A8B8G8R8 : f == 1 ? VJO_FB_A2B10G10R10 : VJO_FB_A1B5G5R5;
        uintptr_t base = f == 2 ? (uintptr_t)fb16 : (uintptr_t)fb32;
        OcrRowsJob job = {4, 2, 32, 28, 40, 32, 3, 20, 64, (uintptr_t)grey_out, NULL, 0, 0};
        memset(grey_out, 0xEE, sizeof(grey_out));
        TEST_CHECK(capture_grey_rows(base, 40, fmt, 40, 32, &job) == 20);
        for (unsigned r = 0; r < 20; r++)
            for (unsigned x = 0; x < 32; x++) {
                unsigned idx = (2 + 3 + r) * 40 + 4 + x;
                uint32_t abgr;
                vjo_fb_to_abgr(&abgr, f == 2 ? (const void *)&fb16[idx] : (const void *)&fb32[idx], 1, fmt);
                TEST_CHECK(grey_out[r * 64 + x] == vjo_grey(abgr & 0xFF, (abgr >> 8) & 0xFF, (abgr >> 16) & 0xFF));
            }
        TEST_CHECK(grey_out[32] == 0xEE); /* width bytes only */
        if (f == 2) { /* 5-bit channels expand by << 3, as the colour rows do */
            uint32_t abgr; static const uint16_t white[2] = {0x7FFF, 0x7FFF}; /* 4 bytes: any format's pixel */
            vjo_fb_to_abgr(&abgr, white, 1, fmt);
            TEST_CHECK((abgr & 0xFFFFFF) == 0xF8F8F8);
        }
    }
    OcrRowsJob job = {4, 2, 32, 28, 40, 32, 3, 20, 64, (uintptr_t)grey_out, NULL, 0, 0};
    TEST_CHECK(capture_grey_rows((uintptr_t)fb32, 40, 0x12345678u, 40, 32, &job) == VJO_ERR_FORMAT);
    TEST_CHECK(capture_grey_rows((uintptr_t)fb32, 40, VJO_FB_A8B8G8R8, 48, 32, &job) == VJO_ERR_FORMAT);
    TEST_CHECK(capture_grey_rows(0, 40, VJO_FB_A8B8G8R8, 40, 32, &job) == VJO_ERR_COPY);
    job.n = 30; /* past the region */
    TEST_CHECK(capture_grey_rows((uintptr_t)fb32, 40, VJO_FB_A8B8G8R8, 40, 32, &job) == VJO_ERR_FORMAT);
    job.n = 20; user_copy_failure = 1;
    TEST_CHECK(capture_grey_rows((uintptr_t)fb32, 40, VJO_FB_A8B8G8R8, 40, 32, &job) == VJO_ERR_COPY);
    user_copy_failure = 0;
    /* the frame's geometry against the job's, as the full-frame capture's copy was checked until API 24: a
     * pitch below the frame's width, and a region past the frame's right or bottom edge, read nothing */
    TEST_CHECK(capture_grey_rows((uintptr_t)fb32, 39, VJO_FB_A8B8G8R8, 40, 32, &job) == VJO_ERR_FORMAT);
    OcrRowsJob past = job; past.x0 = 10; /* 10 + 32 > 40 */
    TEST_CHECK(capture_grey_rows((uintptr_t)fb32, 40, VJO_FB_A8B8G8R8, 40, 32, &past) == VJO_ERR_FORMAT);
    past = job; past.y0 = 5; /* 5 + 28 > 32 */
    TEST_CHECK(capture_grey_rows((uintptr_t)fb32, 40, VJO_FB_A8B8G8R8, 40, 32, &past) == VJO_ERR_FORMAT);
    unsigned char kstrip[32 * 20];
    OcrRowsJob kjob = {4, 2, 32, 28, 40, 32, 3, 20, 32, 0, kstrip, 0, 0};
    TEST_CHECK(capture_grey_rows((uintptr_t)fb32, 40, VJO_FB_A8B8G8R8, 40, 32, &kjob) == 20);
    memset(grey_out, 0, sizeof(grey_out));
    TEST_CHECK(capture_grey_rows((uintptr_t)fb32, 40, VJO_FB_A8B8G8R8, 40, 32, &job) == 20);
    for (unsigned r = 0; r < 20; r++) TEST_CHECK(!memcmp(kstrip + r * 32, grey_out + r * 64, 32));
}

/* API 23's colour rows (a shot, OcrRowsJob.rgba): each format expanded to A8B8G8R8 words by
 * read_row, as the full-frame capture expanded it until API 24 (10-bit channels >> 2, 5-bit
 * << 3, alpha 0xFF; A8B8G8R8 as it is), w * 4 bytes a row at the job's stride, into the
 * worker's memory or a kernel strip; a failed read of the frame or copy out is a copy error. */
static uint32_t colour_out[20 * 33], colour_kstrip[20 * 32];
static void ocr_rows_colour_every_format(void)
{
    static uint32_t fb32[40 * 32];
    static uint16_t fb16[40 * 32];
    for (int f = 0; f < 3; f++) {
        uint32_t fmt = f == 0 ? VJO_FB_A8B8G8R8 : f == 1 ? VJO_FB_A2B10G10R10 : VJO_FB_A1B5G5R5;
        for (unsigned i = 0; i < 40 * 32; i++) {
            fb32[i] = f == 0 ? i * 2654435761u : (17u << 2) | (31u << 12) | (63u << 22);
            fb16[i] = 0x801F;
        }
        uintptr_t base = f == 2 ? (uintptr_t)fb16 : (uintptr_t)fb32;
        /* rows 3-22 of the region at 4,2 (32 x 28) of a 40 x 32 frame, 33 words apart */
        OcrRowsJob job = {4, 2, 32, 28, 40, 32, 3, 20, 33 * 4, (uintptr_t)colour_out, NULL, 0, 1};
        OcrRowsJob kjob = {4, 2, 32, 28, 40, 32, 3, 20, 32 * 4, 0, (uint8_t *)colour_kstrip, 0, 1};
        unsigned bad = 0;
        setup();
        memset(colour_out, 0xEE, sizeof(colour_out));
        memset(colour_kstrip, 0xEE, sizeof(colour_kstrip));
        TEST_CHECK(capture_grey_rows(base, 40, fmt, 40, 32, &job) == 20 && game_copies == 20);
        TEST_CHECK(capture_grey_rows(base, 40, fmt, 40, 32, &kjob) == 20);
        for (unsigned r = 0; r < 20; r++) {
            for (unsigned x = 0; x < 32; x++) {
                uint32_t want = f == 0 ? fb32[(2 + 3 + r) * 40 + 4 + x] : f == 1 ? 0xFF3F1F11u : 0xFF0000F8u;
                bad += colour_out[r * 33 + x] != want || colour_kstrip[r * 32 + x] != want;
            }
            bad += colour_out[r * 33 + 32] != 0xEEEEEEEEu; /* w * 4 bytes only */
        }
        TEST_CHECK(bad == 0);
        TEST_MSG("format 0x%08X: %u words wrong", (unsigned)fmt, bad);
        game_copy_failure = 1;
        TEST_CHECK(capture_grey_rows(base, 40, fmt, 40, 32, &job) == VJO_ERR_COPY);
        game_copy_failure = 0; user_copy_failure = 1;
        TEST_CHECK(capture_grey_rows(base, 40, fmt, 40, 32, &job) == VJO_ERR_COPY);
        user_copy_failure = 0;
        TEST_CHECK(capture_grey_rows(base, 40, fmt, 40, 31, &job) == VJO_ERR_FORMAT); /* another frame */
    }
}

/* The OCR job's first pass (sig = 1) must leave g.capture_sig exactly the
 * signature of the region's A8B8G8R8 words in every format, while its grey
 * bytes stay right: the signature decides whether a job's text belongs to the
 * screen on show (game_ocr.c scene_match, shell/worker.c shows_screen), and the
 * A1B5G5R5 one-pass rows must never skip a row the signature samples
 * (capture_grey_rows). The reference runs scene.c's own accumulator over the
 * expanded region (vjo_fb_to_abgr), every row in order. Adapted from
 * audit-overlay-perf-2's test_sig_rows.c (PR #83 audit, blocker B1): with the
 * fast path's condition reduced to the format, off by one, or the slow path not
 * feeding the signature, these fail. */
#define SIG_FW 960u
#define SIG_FH 544u
static uint32_t sig_fb32[SIG_FW * SIG_FH];
static uint16_t sig_fb16[SIG_FW * SIG_FH];
static uint32_t sig_abgr_row[SIG_FW];
static unsigned char sig_grey[64 * SIG_FW];

static void sig_first_pass(uint32_t fmt, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t strip)
{
    static SceneAcc ref_acc;
    static SceneSig ref;
    const void *base = fmt == VJO_FB_A1B5G5R5 ? (const void *)sig_fb16 : (const void *)sig_fb32;
    unsigned bpp = vjo_fb_bpp(fmt), bad_grey = 0;
    memset(&ref_acc, 0, sizeof(ref_acc));
    scene_acc_begin(&ref_acc, &ref, w, h);
    for (uint32_t r = 0; r < h; r++) {
        vjo_fb_to_abgr(sig_abgr_row, (const uint8_t *)base + ((size_t)(y0 + r) * SIG_FW + x0) * bpp, w, fmt);
        scene_acc_row(&ref_acc, r, sig_abgr_row);
    }
    /* the kernel: a first pass in strips, as vjoOcrCaptureRows arms it */
    memset(&g.capture_sig, 0x5A, sizeof(g.capture_sig));
    capture_sig_begin(w, h);
    for (uint32_t y = 0; y < h; y += strip) {
        uint32_t n = h - y < strip ? h - y : strip;
        OcrRowsJob job = {x0, y0, w, h, SIG_FW, SIG_FH, y, n, w, (uintptr_t)sig_grey, NULL, 1, 0};
        TEST_ASSERT(capture_grey_rows((uintptr_t)base, SIG_FW, fmt, SIG_FW, SIG_FH, &job) == (int)n);
        for (uint32_t r = 0; r < n; r++) {
            vjo_fb_to_abgr(sig_abgr_row, (const uint8_t *)base + ((size_t)(y0 + y + r) * SIG_FW + x0) * bpp, w, fmt);
            for (uint32_t x = 0; x < w; x++) {
                uint32_t v = sig_abgr_row[x];
                bad_grey += sig_grey[r * w + x] != vjo_grey(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF);
            }
        }
    }
    TEST_CHECK(bad_grey == 0);
    TEST_CHECK(g.capture_sig.w == ref.w && g.capture_sig.h == ref.h && g.capture_sig.cols == ref.cols);
    TEST_CHECK(!memcmp(g.capture_sig.hash, ref.hash, sizeof(ref.hash)));
    TEST_MSG("fmt 0x%08X region %u,%u %ux%u strips of %u: %u grey mismatches, ystep %u", (unsigned)fmt,
             (unsigned)x0, (unsigned)y0, (unsigned)w, (unsigned)h, (unsigned)strip, bad_grey,
             (unsigned)capture_acc.ystep);
}

static void sig_frames(void)
{
    uint32_t s = 0x83u;
    setup();
    for (unsigned i = 0; i < SIG_FW * SIG_FH; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        sig_fb32[i] = s;
        sig_fb16[i] = (uint16_t)(s >> 9);
    }
}

static const uint32_t sig_formats[] = {VJO_FB_A8B8G8R8, VJO_FB_A1B5G5R5, VJO_FB_A2B10G10R10};

static void ocr_first_pass_signature_every_format(void)
{
    sig_frames();
    for (unsigned f = 0; f < 3; f++) {
        sig_first_pass(sig_formats[f], 4, 2, 32, 28, 5);         /* thin: every row sampled */
        sig_first_pass(sig_formats[f], 0, 0, SIG_FW, SIG_FH, 64); /* full frame: every 16th row sampled */
        sig_first_pass(sig_formats[f], 32, 362, 896, 156, 64);    /* a dialogue box */
        sig_first_pass(sig_formats[f], 32, 362, 896, 156, 5);     /* sampled rows at every strip offset */
    }
}

/* 400 random regions (even x, any size up to the frame), strip heights 1-64. */
static void ocr_first_pass_signature_random_geometry(void)
{
    uint32_t s = 0x2545F491u;
#define SIG_RND(n) (s ^= s << 13, s ^= s >> 17, s ^= s << 5, s % (n))
    sig_frames();
    for (unsigned it = 0; it < 400; it++) {
        uint32_t w = 1 + SIG_RND(SIG_FW), h = 1 + SIG_RND(SIG_FH);
        uint32_t x0 = SIG_RND(SIG_FW - w + 1) & ~1u, y0 = SIG_RND(SIG_FH - h + 1), strip = 1 + SIG_RND(64);
        sig_first_pass(sig_formats[it % 3], x0, y0, w, h, strip);
    }
#undef SIG_RND
}

TEST_LIST = {
    {"the job's crop of the region: a box, a small frame, the whole frame, the 32 px minimum", region_crops},
    {"OCR grey rows: every format, refusals, worker copy and kernel strip", ocr_grey_rows_every_format},
    {"a shot's colour rows: every format expanded, worker copy and kernel strip, refusals", ocr_rows_colour_every_format},
    {"OCR first pass: g.capture_sig is the expanded region's signature, every format", ocr_first_pass_signature_every_format},
    {"OCR first pass: random regions and strip heights", ocr_first_pass_signature_random_geometry},
    {"a box drawn with the region selector is the captured crop", drawn_box_is_the_captured_crop},
    {"a suggested band is the captured rows (#358)", suggested_band_is_the_captured_rows},
    {NULL, NULL}
};
