/* The kernel's half of the stage benchmark: the real kernel/capture.c and
 * kernel/scene.c (the display hook's grey rows and region signature), with
 * the SDK's user/kernel copies as plain memcpy, as the hook runs them inside
 * the game's own process (host/tests/parity_kernel.c does the same).
 *
 * Host: built with host/tests/vita_stubs, where those copies are functions.
 * Cortex-A9 lane: built by the Vita compiler against the real VitaSDK headers
 * (whose ksceKernelMemcpy* are macros for ksceKernelCopyFromUser/ToUser) and
 * linked with the kernel plugin's own capture.c.obj and scene.c.obj. The
 * firmware's copy routines are unknown code: memcpy stands in for them and is
 * reported as such. */
#include <string.h>

#ifdef VJO_HOST
#include <psp2kern/host_stubs.h>
#endif
#include "../../kernel/vjo_kernel.h"
#include "stage_bench.h"

VjoKernelState g;
#ifdef VJO_HOST
unsigned syscall_depth;
int ksceKernelMemcpyUserToKernel(void *dst, const void *src, SceSize n) { memcpy(dst, src, n); return 0; }
int ksceKernelMemcpyKernelToUser(void *dst, const void *src, SceSize n) { memcpy(dst, src, n); return 0; }
#else
int ksceKernelCopyFromUser(void *dst, const void *src, SceSize len) { memcpy(dst, src, len); return 0; }
int ksceKernelCopyToUser(void *dst, const void *src, SceSize len) { memcpy(dst, src, len); return 0; }
#endif
void klog(const char *fmt, ...) { (void)fmt; }

int bench_grey_rows(uintptr_t base, uint32_t pitch, uint32_t fmt, uint32_t fb_w, uint32_t fb_h, uint32_t x0,
                    uint32_t y0, uint32_t w, uint32_t h, uint32_t y, uint32_t n, unsigned char *dst,
                    uint32_t stride, int sig)
{
    OcrRowsJob job = {x0, y0, w, h, fb_w, fb_h, y, n, stride, (uintptr_t)dst, NULL, sig};
    return capture_grey_rows(base, pitch, fmt, fb_w, fb_h, &job);
}

void bench_capture_sig_begin(uint32_t w, uint32_t h)
{
    capture_sig_begin(w, h);
}

int bench_region_sig(uintptr_t base, uint32_t pitch, uint32_t fmt, uint32_t fb_w, uint32_t fb_h, uint16_t rx,
                     uint16_t ry, uint16_t rw, uint16_t rh, void *sig)
{
    g.region.x = rx;
    g.region.y = ry;
    g.region.w = rw;
    g.region.h = rh;
    return region_sig_hook(base, pitch, fmt, fb_w, fb_h, (SceneSig *)sig);
}

uint32_t bench_capture_sig_hash(void)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < SCENE_CELLS; i++) h = (h ^ g.capture_sig.hash[i]) * 16777619u;
    return h;
}
