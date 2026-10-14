/* Production region capture code with failure-injecting SDK boundaries. */
#include "acutest.h"
#include <psp2kern/host_stubs.h>
#include "../../kernel/vjo_kernel.h"
#include "../../kernel/buffers.c"
#include "../../kernel/capture.c"
#include "../../kernel/capture_request.c"
#include "../../core/region_select.h"

int game_ocr_capture_busy_locked(void) { return 0; }
VjoKernelState g;
unsigned syscall_depth;
static unsigned capture_allocations, capture_frees, capture_bytes, capture_copies;
static int allocation_failure, capture_base_failure, capture_base_null, capture_copy_failure;
static uint32_t capture_storage[VJO_MAX_W * VJO_MAX_H + 8];
static uint32_t frame_pixels[VJO_MAX_W * VJO_MAX_H];
void klog(const char *fmt, ...) {}
uint64_t ksceKernelGetSystemTimeWide(void) { return 1000000; }
int ksceKernelLockMutex(SceUID u, int c, void *t) { return 0; }
int ksceKernelUnlockMutex(SceUID u, int c) { return 0; }
int ksceKernelGetMemBlockBase(SceUID uid, void **base) {
    TEST_ASSERT(uid == 22);
    *base = capture_base_null ? NULL : (void *)(capture_storage + 4);
    return capture_base_failure ? -1 : 0;
}
int ksceKernelMemcpyUserToKernel(void *dst, const void *src, SceSize n) {
    capture_copies++;
    if (capture_copy_failure) return -1;
    memcpy(dst, src, n); return 0;
}
SceUID ksceKernelAllocMemBlock(const char *n, unsigned int type, SceSize bytes, void *o) {
    TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW);
    TEST_CHECK(bytes && bytes <= VJO_MAX_W * VJO_MAX_H * 4 && !(bytes & 4095));
    memset(capture_storage, 0xA5, sizeof(capture_storage));
    capture_allocations++; capture_bytes = bytes; return allocation_failure ? -1 : 22;
}
int ksceKernelFreeMemBlock(SceUID uid) { TEST_CHECK(uid == 22); capture_frees++; return 0; }
static void setup(void) {
    memset(&g, 0, sizeof(g));
    g.game_pid = 7; g.game_active = VJO_GAME; g.shell_pid = 3;
    capture_allocations = capture_frees = capture_bytes = capture_copies = 0;
    allocation_failure = capture_base_failure = capture_base_null = capture_copy_failure = 0;
    memset(capture_storage, 0xA5, sizeof(capture_storage));
}
static void capture_lazy_single_pass(void) {
    setup();
    TEST_CHECK(!capture_allocations && !g.raw);
    TEST_ASSERT(capture_request(VJO_CAPTURE_ONCE) == 1 && capture_bytes == RAW_SIZE);
    TEST_CHECK(capture_request(0) == VJO_ERR_BUSY && capture_allocations == 1);
    g.capture_state = CAPTURE_IDLE; g.raw_valid = 1;
    buffers_read_done(0, 256);
    TEST_CHECK(!capture_frees && g.raw && g.raw_valid);
    buffers_read_done(256, 288);
    TEST_CHECK(capture_frees == 1 && !g.raw && !g.raw_valid && g.alloc_status == VJO_ALLOC_NONE);
    TEST_ASSERT(capture_request(VJO_CAPTURE_FULL) == 2 && g.capture_full);
    g.capture_state = CAPTURE_IDLE; g.raw_valid = 1;
    buffers_read_done(0, 544);
    TEST_CHECK(capture_frees == 2 && !g.raw && !g.raw_valid);
    allocation_failure = 1;
    TEST_CHECK(capture_request(0) == VJO_ERR_NO_MEMORY && g.capture_state == CAPTURE_IDLE);
}
static void capture_guards(void)
{
    for (unsigned i = 0; i < 4; i++) {
        TEST_CHECK(capture_storage[i] == 0xA5A5A5A5u);
        TEST_CHECK(capture_storage[4 + capture_bytes / 4 + i] == 0xA5A5A5A5u);
    }
}

static void capture_region_allocation(void)
{
    setup();
    g.fb_w = 960; g.fb_h = 544;
    g.region = (VjoRect){6000, 45000, 51500, 17500};
    TEST_ASSERT(capture_request(0) == 1);
    TEST_CHECK(g.crop_w == 755 && g.crop_h == 146);
    TEST_CHECK(capture_bytes == 442368 && g.raw_capacity == capture_bytes);
    TEST_CHECK(capture_bytes < RAW_SIZE / 4);
    unsigned count = capture_allocations;
    TEST_CHECK(buffers_alloc(RAW_SIZE) < 0 && capture_allocations == count && !capture_frees);
    g.capture_state = CAPTURE_IDLE;
    TEST_ASSERT(capture_request(VJO_CAPTURE_FULL) == 2);
    TEST_CHECK(g.crop_w == 960 && g.crop_h == 544 && g.capture_x == 0 && g.capture_y == 0);
    TEST_CHECK(capture_allocations == 2 && capture_frees == 1 && g.raw_capacity == RAW_SIZE);
    g.capture_state = CAPTURE_IDLE;
    TEST_ASSERT(capture_request(0) == 3);
    TEST_CHECK(capture_allocations == 3 && capture_frees == 2 && g.raw_capacity == 442368);
    g.capture_state = CAPTURE_IDLE;
    TEST_ASSERT(capture_request(0) == 4);
    TEST_CHECK(capture_allocations == 3 && capture_frees == 2); /* exact-size reuse */
    capture_guards();
}

static void capture_multi_pass(void)
{
    setup(); g.fb_w = g.fb_h = 32;
    for (unsigned i = 0; i < 32 * 32; i++) frame_pixels[i] = 0xFF000000u | i;
    TEST_ASSERT(capture_request(0) == 1 && !g.capture_once);
    TEST_ASSERT(capture_copy((uintptr_t)frame_pixels, 32, VJO_FB_A8B8G8R8, 32, 32) == 0);
    g.capture_state = CAPTURE_IDLE; g.raw_valid = 1;
    /* Local OCR finds lines by reading the entire image, then rereads each
     * detected line. The first final-row read must not invalidate that image. */
    buffers_read_done(0, 32);
    TEST_ASSERT(g.raw && g.raw_valid && !capture_frees);
    TEST_CHECK(g.raw && g.raw_valid && !memcmp(g.raw, frame_pixels, 32 * 32 * 4));
    buffers_read_done(8, 24);
    TEST_CHECK(g.raw && g.raw_valid && !capture_frees);
    buffers_free();
    TEST_CHECK(capture_frees == 1 && !g.raw && !g.raw_capacity);
}

static void capture_allocation_failures(void)
{
    for (int failure = 0; failure < 3; failure++) {
        setup();
        allocation_failure = failure == 0;
        capture_base_failure = failure == 1;
        capture_base_null = failure == 2;
        TEST_CHECK(capture_request(0) == VJO_ERR_NO_MEMORY);
        TEST_CHECK(g.capture_state == CAPTURE_IDLE && !g.mem_uid && !g.raw && !g.raw_capacity);
        TEST_CHECK(g.alloc_status == VJO_ALLOC_FAIL && !g.raw_valid && !g.capture_seq);
        TEST_CHECK(capture_allocations == 1 && capture_frees == (unsigned)(failure != 0));
        allocation_failure = capture_base_failure = capture_base_null = 0;
        TEST_CHECK(capture_request(0) == 1 && g.raw_capacity == RAW_SIZE);
    }
    setup();
    TEST_CHECK(buffers_alloc(0) < 0 && buffers_alloc(RAW_SIZE + 1) < 0 && !capture_allocations);
    TEST_CHECK(buffers_alloc(1) == 0 && capture_bytes == 4096);
    g.raw_valid = 1;
    allocation_failure = 1;
    TEST_CHECK(buffers_alloc(8192) < 0);
    TEST_CHECK(capture_frees == 1 && !g.raw && !g.raw_capacity && !g.raw_valid);
    TEST_CHECK(g.alloc_status == VJO_ALLOC_FAIL);
}

static void capture_discard(void)
{
    setup(); g.fb_w = g.fb_h = 32;
    TEST_CHECK(capture_request(VJO_CAPTURE_DISCARD) == VJO_ERR_ARG && !capture_allocations);
    TEST_ASSERT(capture_request(VJO_CAPTURE_ONCE) == 1);
    uint8_t *raw = g.raw;
    for (int state = CAPTURE_PENDING; state <= CAPTURE_COPIED; state++) {
        g.capture_state = state;
        TEST_CHECK(capture_request(VJO_CAPTURE_DISCARD) == VJO_ERR_BUSY);
        TEST_CHECK(g.raw == raw && !capture_frees && g.capture_state == state);
    }
    g.capture_state = CAPTURE_IDLE; g.raw_valid = 1;
    const uint32_t invalid[] = {VJO_CAPTURE_DISCARD | VJO_CAPTURE_ONCE,
        VJO_CAPTURE_DISCARD | VJO_CAPTURE_FULL, 8, UINT32_MAX};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        TEST_CHECK(capture_request(invalid[i]) == VJO_ERR_ARG);
        TEST_CHECK(g.raw == raw && g.raw_valid && !capture_frees && capture_allocations == 1);
    }
    /* A failed JPEG may have read some bands but never its final one. */
    buffers_read_done(0, 16);
    TEST_ASSERT(g.raw_valid && !capture_frees);
    TEST_CHECK(capture_request(VJO_CAPTURE_DISCARD) == 0);
    TEST_CHECK(capture_frees == 1 && !g.raw && !g.raw_valid && !g.raw_capacity);
    TEST_CHECK(!g.capture_once && !g.capture_full && g.capture_seq == 1);
    TEST_CHECK(capture_request(VJO_CAPTURE_DISCARD) == VJO_ERR_ARG && capture_frees == 1);
    TEST_ASSERT(capture_request(VJO_CAPTURE_FULL) == 2);
    g.capture_state = CAPTURE_IDLE; /* a completed copy failure has no valid image */
    TEST_CHECK(capture_request(VJO_CAPTURE_DISCARD) == 0 && capture_frees == 2);

    TEST_ASSERT(capture_request(0) == 3); /* local OCR remains owned across passes */
    g.capture_state = CAPTURE_IDLE; g.raw_valid = 1; raw = g.raw;
    TEST_CHECK(capture_request(VJO_CAPTURE_DISCARD) == VJO_ERR_ARG);
    TEST_CHECK(g.raw == raw && g.raw_valid && g.raw_capacity == 4096 && capture_frees == 2);
    buffers_read_done(0, 32);
    TEST_CHECK(g.raw == raw && g.raw_valid && capture_frees == 2);
}

static void capture_geometry_bounds(void)
{
    setup(); g.fb_w = g.fb_h = 64;
    g.region = (VjoRect){16384, 32768, 32768, 32768};
    TEST_ASSERT(capture_request(0) == 1);
    TEST_CHECK(g.crop_w == 32 && g.crop_h == 32 && capture_bytes == 4096);
    TEST_CHECK(g.capture_x == 16 && g.capture_y == 32);
    uint32_t capacity = g.raw_capacity;
    uintptr_t base = (uintptr_t)frame_pixels;
    capture_copies = 0;
    TEST_CHECK(capture_copy(base, 64, VJO_FB_A8B8G8R8, 63, 64) == VJO_ERR_ARG);
    TEST_CHECK(capture_copy(base, 64, VJO_FB_A8B8G8R8, 64, 63) == VJO_ERR_ARG);
    TEST_CHECK(capture_copy(base, 63, VJO_FB_A8B8G8R8, 64, 64) == VJO_ERR_ARG);
    g.raw_capacity = capacity - 1;
    TEST_CHECK(capture_copy(base, 64, VJO_FB_A8B8G8R8, 64, 64) == VJO_ERR_ARG);
    g.raw_capacity = capacity;
    g.capture_x = 64;
    TEST_CHECK(capture_copy(base, 64, VJO_FB_A8B8G8R8, 64, 64) == VJO_ERR_ARG);
    g.capture_x = 16;
    g.capture_y = 64;
    TEST_CHECK(capture_copy(base, 64, VJO_FB_A8B8G8R8, 64, 64) == VJO_ERR_ARG);
    g.capture_y = 32;
    TEST_CHECK(capture_copy(base, 64, 12345, 64, 64) == VJO_ERR_FORMAT);
    TEST_CHECK(!capture_copies);
    capture_guards();
    for (unsigned i = 0; i < 64 * 64; i++) frame_pixels[i] = 0xFF000000u | i;
    g.region = (VjoRect){0}; /* a later region change must not grow this request */
    TEST_ASSERT(capture_copy(base, 64, VJO_FB_A8B8G8R8, 64, 64) == 0);
    TEST_CHECK(g.raw_stride == 128 && capture_copies == 32);
    for (unsigned row = 0; row < 32; row++)
        TEST_CHECK(!memcmp(g.raw + row * 128, frame_pixels + (row + 32) * 64 + 16, 128));
    capture_guards();
    capture_copy_failure = 1; capture_copies = 0;
    TEST_CHECK(capture_copy(base, 64, VJO_FB_A8B8G8R8, 64, 64) == VJO_ERR_COPY && capture_copies == 1);
    capture_guards();
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

static void capture_format_conversion(void)
{
    for (int ten_bit = 0; ten_bit < 2; ten_bit++) {
        setup(); g.fb_w = g.fb_h = 32;
        TEST_ASSERT(capture_request(0) == 1);
        if (ten_bit) {
            for (unsigned i = 0; i < 32 * 32; i++)
                frame_pixels[i] = (17u << 2) | (31u << 12) | (63u << 22);
        } else {
            const uint16_t pixel = 0x801F;
            for (unsigned i = 0; i < 32 * 32; i++)
                memcpy((uint8_t *)frame_pixels + i * 2, &pixel, sizeof(pixel));
        }
        TEST_ASSERT(capture_copy((uintptr_t)frame_pixels, 32,
            ten_bit ? VJO_FB_A2B10G10R10 : VJO_FB_A1B5G5R5, 32, 32) == 0);
        for (unsigned i = 0; i < 32 * 32; i++)
            TEST_CHECK(((uint32_t *)g.raw)[i] == (ten_bit ? 0xFF3F1F11u : 0xFF0000F8u));
        capture_guards();
    }
}

/* The OCR worker's grey rows (API 19): each format expanded to 8 bits as the
 * JPEG capture expands it, then (77 R + 150 G + 29 B) >> 8; a frame of another
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
        OcrRowsJob job = {4, 2, 32, 28, 40, 32, 3, 20, 64, (uintptr_t)grey_out, NULL, 0};
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
        if (f == 2) { /* 5-bit channels expand by << 3, as the JPEG capture does */
            uint32_t abgr; static const uint16_t white[2] = {0x7FFF, 0x7FFF}; /* 4 bytes: any format's pixel */
            vjo_fb_to_abgr(&abgr, white, 1, fmt);
            TEST_CHECK((abgr & 0xFFFFFF) == 0xF8F8F8);
        }
    }
    OcrRowsJob job = {4, 2, 32, 28, 40, 32, 3, 20, 64, (uintptr_t)grey_out, NULL, 0};
    TEST_CHECK(capture_grey_rows((uintptr_t)fb32, 40, 0x12345678u, 40, 32, &job) == VJO_ERR_FORMAT);
    TEST_CHECK(capture_grey_rows((uintptr_t)fb32, 40, VJO_FB_A8B8G8R8, 48, 32, &job) == VJO_ERR_FORMAT);
    TEST_CHECK(capture_grey_rows(0, 40, VJO_FB_A8B8G8R8, 40, 32, &job) == VJO_ERR_COPY);
    job.n = 30; /* past the region */
    TEST_CHECK(capture_grey_rows((uintptr_t)fb32, 40, VJO_FB_A8B8G8R8, 40, 32, &job) == VJO_ERR_FORMAT);
    job.n = 20; user_copy_failure = 1;
    TEST_CHECK(capture_grey_rows((uintptr_t)fb32, 40, VJO_FB_A8B8G8R8, 40, 32, &job) == VJO_ERR_COPY);
    user_copy_failure = 0;
    unsigned char kstrip[32 * 20];
    OcrRowsJob kjob = {4, 2, 32, 28, 40, 32, 3, 20, 32, 0, kstrip, 0};
    TEST_CHECK(capture_grey_rows((uintptr_t)fb32, 40, VJO_FB_A8B8G8R8, 40, 32, &kjob) == 20);
    memset(grey_out, 0, sizeof(grey_out));
    TEST_CHECK(capture_grey_rows((uintptr_t)fb32, 40, VJO_FB_A8B8G8R8, 40, 32, &job) == 20);
    for (unsigned r = 0; r < 20; r++) TEST_CHECK(!memcmp(kstrip + r * 32, grey_out + r * 64, 32));
}

TEST_LIST = {
    {"OCR grey rows: every format, refusals, worker copy and kernel strip", ocr_grey_rows_every_format},
    {"lazy JPEG capture and final-row release", capture_lazy_single_pass},
    {"region capture allocation and resizing", capture_region_allocation},
    {"multi-pass local OCR capture lifetime", capture_multi_pass},
    {"capture allocation and base failures", capture_allocation_failures},
    {"failed JPEG discard and ownership protection", capture_discard},
    {"capture geometry bounds and frozen region", capture_geometry_bounds},
    {"a box drawn with the region selector is the captured crop", drawn_box_is_the_captured_crop},
    {"capture source pixel conversion", capture_format_conversion}, {NULL, NULL}
};
