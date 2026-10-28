/* Keep the real kernel implementations behind the host's retail-return model.
 * Renaming is test-only: native exports and library NIDs remain unchanged. */
#define vjoOcrRegister test_kernel_register
#define vjoOcrSubmit test_kernel_submit
#define vjoOcrTake test_kernel_take
#define vjoOcrCaptureRows test_kernel_capture_rows
#define vjoOcrCancelled test_kernel_cancelled
#define vjoOcrComplete test_kernel_complete
#define vjoOcrRead test_kernel_read
#define vjoOcrCancel test_kernel_cancel
#define vjoOcrModelOpen test_kernel_model_open
#define vjoOcrModelRead test_kernel_model_read
#define vjoOcrModelClose test_kernel_model_close
#define vjoOcrShotWrite test_kernel_shot_write
#define vjoOcrShotSave test_kernel_shot_save
#define vjoOcrHookCache test_kernel_hook_cache
#include "../../kernel/game_ocr.c"

/* An SC3 charset's open (VJO_GAME_OCR_OPEN_SC3) asks the loader for the caller's title ID: this harness has no
 * loader, so that open is refused, as with a loader that is off. */
__attribute__((weak)) int loader_title(SceUID pid, char *title, SceSize len)
{
    (void)pid; (void)title; (void)len;
    return -1;
}

/* kernel/log.c's clock in ms (a shot's stage lines), which this harness does not link. */
__attribute__((weak)) unsigned klog_ms(void)
{
    return (unsigned)(ksceKernelGetSystemTimeWide() / 1000);
}

/* The model-only and allocation harnesses never write a screenshot. They still
 * link the whole broker; the screenshot harness overrides this with its real
 * file truncation. Fail immediately if another harness unexpectedly reaches it. */
#include <stdlib.h>
__attribute__((weak)) int ksceIoChstatByFd(SceUID fd, SceIoStat *st, int bits)
{
    (void)fd; (void)st; (void)bits;
    abort();
}
