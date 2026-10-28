/* The real kernel side of the capture parity test: kernel/capture.c (crop and
 * the display hook's grey rows) with the SDK copies as plain memcpy, as the
 * hook runs them in the game's own process. */
#include <psp2kern/host_stubs.h>
#include "../../kernel/vjo_kernel.h"
#include "parity.h"

VjoKernelState g;
unsigned syscall_depth;
void klog(const char *fmt, ...) { (void)fmt; }
int ksceKernelMemcpyUserToKernel(void *dst, const void *src, SceSize n) { memcpy(dst, src, n); return 0; }
int ksceKernelMemcpyKernelToUser(void *dst, const void *src, SceSize n) { memcpy(dst, src, n); return 0; }

/* The job's layout is OcrRowsJob's (kernel/vjo_kernel.h). */
typedef char parity_job_layout[sizeof(ParityRowsJob) == sizeof(OcrRowsJob) ? 1 : -1];

void parity_crop_of(uint16_t rx, uint16_t ry, uint16_t rw, uint16_t rh, uint32_t fb_w, uint32_t fb_h, uint32_t *x,
                    uint32_t *y, uint32_t *w, uint32_t *h)
{
    VjoRect r = {rx, ry, rw, rh};
    capture_crop_of(&r, fb_w, fb_h, x, y, w, h);
}

int parity_grey_rows(uintptr_t base, uint32_t pitch, uint32_t fmt, uint32_t fb_w, uint32_t fb_h,
                     const ParityRowsJob *j)
{
    OcrRowsJob job = {j->x0, j->y0, j->w, j->h, j->fb_w, j->fb_h, j->y, j->n, j->stride, j->dst_user,
                      j->dst_kernel, j->sig, j->rgba, j->frame};
    return capture_grey_rows(base, pitch, fmt, fb_w, fb_h, &job);
}

void parity_sig_begin(uint32_t w, uint32_t h)
{
    capture_sig_begin(w, h);
}
