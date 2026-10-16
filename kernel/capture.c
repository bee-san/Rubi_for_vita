/* Region capture and region signatures (scene.h), both inside the display
 * hook, and the game OCR worker's grey rows (API 19). The shell reads the raw
 * rows of a JPEG capture and encodes the JPEG itself. */
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/sysmem.h>

#include "../core/fbgrey.h"
#include "vjo_kernel.h"

/* Two rows of scratch for every copy below. They are only used from on_frame,
 * which runs for one caller at a time (display.c frame_busy): a JPEG capture,
 * a region signature and a worker's rows run one after the other, never at
 * once, so they share these instead of holding three pairs in .bss. */
static uint32_t scratch_row32[VJO_MAX_W], scratch_tmp32[VJO_MAX_W]; /* word-aligned rows */
#define scratch_row ((uint8_t *)scratch_row32)
#define scratch_tmp ((uint8_t *)scratch_tmp32)
/* The capture's signature accumulator: a JPEG region capture or the OCR job's
 * first pass, which cannot be pending together (game_ocr_capture_busy_locked). */
static SceneAcc capture_acc;

void capture_compute_crop(uint32_t fb_w, uint32_t fb_h, uint32_t *x, uint32_t *y, uint32_t *w,
                          uint32_t *h)
{
    capture_crop_of(&g.region, fb_w, fb_h, x, y, w, h);
}

void capture_crop_of(const VjoRect *region, uint32_t fb_w, uint32_t fb_h, uint32_t *x, uint32_t *y,
                     uint32_t *w, uint32_t *h)
{
    VjoRect r = *region;
    uint32_t cx, cy, cw, ch;
    if (fb_w > VJO_MAX_W)
        fb_w = VJO_MAX_W;
    if (fb_h > VJO_MAX_H)
        fb_h = VJO_MAX_H;
    if (r.w == 0 || r.h == 0) {
        *x = 0;
        *y = 0;
        *w = fb_w;
        *h = fb_h;
        return;
    }
    cx = (uint32_t)r.x * fb_w >> 16;
    cy = (uint32_t)r.y * fb_h >> 16;
    cw = ((uint32_t)r.w * fb_w + 65535) >> 16;
    ch = ((uint32_t)r.h * fb_h + 65535) >> 16;
    /* A capture's minimum size, 32 x 32 (the region selector's smallest box). */
    if (cw < 32)
        cw = 32;
    if (ch < 32)
        ch = 32;
    if (cx + cw > fb_w)
        cx = cw > fb_w ? 0 : fb_w - cw;
    if (cy + ch > fb_h)
        cy = ch > fb_h ? 0 : fb_h - ch;
    if (cw > fb_w)
        cw = fb_w;
    if (ch > fb_h)
        ch = fb_h;
    cx &= ~1u;
    *x = cx;
    *y = cy;
    *w = cw;
    *h = ch;
}

/* Runs in the game's context (display hook): its framebuffer is user memory
 * of the current process. Same-process copies only, as PSVshell/reVita do
 * in this hook; reading another process' framebuffer from the worker
 * crashed the console. */
static int copy_from_game(void *dst, uintptr_t src, uint32_t len)
{
    return ksceKernelMemcpyUserToKernel(dst, (const void *)src, len);
}

/* Reads n pixels at src as A8B8G8R8 into dst; tmp holds a converted
 * format's source row. */
static int read_row(uint8_t *dst, uint8_t *tmp, uintptr_t src, uint32_t n, uint32_t fmt)
{
    if (fmt == VJO_FB_A8B8G8R8)
        return copy_from_game(dst, src, n * 4);
    if (copy_from_game(tmp, src, n * vjo_fb_bpp(fmt)) < 0)
        return -1;
    vjo_fb_to_abgr((uint32_t *)dst, tmp, n, fmt);
    return 0;
}

/* Copies the region (or, for a VJO_CAPTURE_FULL request, the whole frame)
 * of a game frame into g.raw (A8B8G8R8, raw_stride bytes per row), and the
 * region's signature into g.capture_sig. Game context only (display or pad
 * hook, display.c). */
int capture_copy(uintptr_t base, uint32_t pitch, uint32_t fmt, uint32_t w, uint32_t h)
{
    uint32_t x, y, cw, ch, bpp, stride;
    int full = g.capture_full;

    if (!g.raw)
        return VJO_ERR_NO_MEMORY;
    bpp = vjo_fb_bpp(fmt);
    if (!bpp)
        return VJO_ERR_FORMAT;
    w = w > VJO_MAX_W ? VJO_MAX_W : w;
    h = h > VJO_MAX_H ? VJO_MAX_H : h;
    x = g.capture_x;
    y = g.capture_y;
    cw = g.crop_w;
    ch = g.crop_h;
    if (w != g.capture_fb_w || h != g.capture_fb_h || !cw || !ch ||
        pitch < w || x > w || cw > w - x || y > h || ch > h - y ||
        cw * ch * 4 > g.raw_capacity)
        return VJO_ERR_ARG;
    if (!full) scene_acc_begin(&capture_acc, &g.capture_sig, cw, ch);

    stride = g.raw_stride = cw * 4;

    for (uint32_t row = 0; row < ch; row++) {
        uint8_t *dst = g.raw + row * stride;
        if (read_row(dst, scratch_tmp, base + ((y + row) * pitch + x) * bpp, cw, fmt) < 0)
            return VJO_ERR_COPY;
        if (!full)
            scene_acc_row(&capture_acc, row, (const uint32_t *)dst);
    }
    return 0;
}

/* Game context (display or pad hook): signature of the region of the
 * frame, reading only the sampled rows. Same-process copies only, as
 * PSVshell/reVita do in the display hook; reading another process'
 * framebuffer from the worker crashed the console. */
int region_sig_hook(uintptr_t base, uint32_t pitch, uint32_t fmt, uint32_t w, uint32_t h, SceneSig *out)
{
    static SceneAcc acc;
    uint32_t x, y, cw, ch, bpp;

    if (!base)
        return VJO_ERR_COPY;
    bpp = vjo_fb_bpp(fmt);
    if (!bpp)
        return VJO_ERR_FORMAT;
    capture_compute_crop(w, h, &x, &y, &cw, &ch);
    if (!cw || !ch)
        return VJO_ERR_FORMAT;
    scene_acc_begin(&acc, out, cw, ch);
    for (uint32_t r = 0; r < ch; r = scene_acc_next_row(&acc)) {
        if (read_row(scratch_row, scratch_tmp, base + ((y + r) * pitch + x) * bpp, cw, fmt) < 0)
            return VJO_ERR_COPY;
        scene_acc_row(&acc, r, (const uint32_t *)scratch_row);
    }
    return 0;
}

void capture_sig_begin(uint32_t w, uint32_t h)
{
    scene_acc_begin(&capture_acc, &g.capture_sig, w, h);
}

/* Game context (display or pad hook, display.c): rows [y, y + n) of the OCR
 * job's region (x0, y0, w, h in a frame of fb_w x fb_h) of the frame at base,
 * as grey bytes, row r at dst + r * stride: user memory of the current process
 * (dst_user) or a kernel strip (dst_kernel). sig: the rows also continue the
 * capture's signature (g.capture_sig, capture_sig_begin). rgba (API 23, a shot):
 * the rows as A8B8G8R8 words instead, 4 bytes a pixel. No allocation, no
 * lock; the job's geometry was checked when it was armed. */
int capture_grey_rows(uintptr_t base, uint32_t pitch, uint32_t fmt, uint32_t fb_w, uint32_t fb_h,
                      const OcrRowsJob *job)
{
    uint32_t bpp = vjo_fb_bpp(fmt);
    if (!base)
        return VJO_ERR_COPY;
    if (!bpp)
        return VJO_ERR_FORMAT;
    if (fb_w != job->fb_w || fb_h != job->fb_h || pitch < fb_w || !job->w || job->w > VJO_MAX_W ||
        job->x0 > fb_w || job->w > fb_w - job->x0 || job->y0 > fb_h || job->h > fb_h - job->y0 ||
        !job->n || job->y > job->h || job->n > job->h - job->y)
        return VJO_ERR_FORMAT; /* the frame is not the one the job was armed for */
    for (uint32_t r = 0; r < job->n; r++) {
        uint32_t row = job->y + r;
        uintptr_t src = base + ((job->y0 + row) * pitch + job->x0) * bpp;
        if (job->rgba) {
            /* API 23, a shot: the row in colour (A8B8G8R8, read_row's expansion), 4 bytes a pixel */
            if (read_row(scratch_row, scratch_tmp, src, job->w, fmt) < 0)
                return VJO_ERR_COPY;
            if (job->dst_kernel)
                memcpy(job->dst_kernel + r * job->stride, scratch_row, job->w * 4);
            else if (ksceKernelMemcpyKernelToUser((void *)(job->dst_user + r * job->stride), scratch_row,
                                                  job->w * 4) < 0)
                return VJO_ERR_COPY;
            continue;
        }
        if (fmt == VJO_FB_A1B5G5R5 && !(job->sig && row == scene_acc_next_row(&capture_acc))) {
            /* The PSP emulator's 16-bit rows straight to grey, one pass (core/fbgrey.h);
             * a row the capture's signature samples hashes A8B8G8R8 words below. */
            if (copy_from_game(scratch_row, src, job->w * 2) < 0)
                return VJO_ERR_COPY;
            vjo_a1b5g5r5_to_grey(scratch_tmp, (const uint16_t *)scratch_row, job->w);
        } else {
            if (read_row(scratch_row, scratch_tmp, src, job->w, fmt) < 0)
                return VJO_ERR_COPY;
            if (job->sig)
                scene_acc_row(&capture_acc, row, (const uint32_t *)scratch_row);
            vjo_abgr_to_grey(scratch_tmp, (const uint32_t *)scratch_row, job->w);
        }
        if (job->dst_kernel)
            memcpy(job->dst_kernel + r * job->stride, scratch_tmp, job->w);
        else if (ksceKernelMemcpyKernelToUser((void *)(job->dst_user + r * job->stride), scratch_tmp, job->w) < 0)
            return VJO_ERR_COPY;
    }
    return (int)job->n;
}
