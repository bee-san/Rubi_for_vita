/* Blocking storage stays off the controller's 16 ms poll. This thread owns
 * broker I/O, boot-record writes and log flushing, all in kernel context. It
 * shares the existing staging buffer and takes no image buffer or heap. */
#include "vjo_kernel.h"
#include "game_ocr.h"
#include "../include/vjo_ledger_budget.h"

static SceUID io_thread = -1;
static volatile int io_run;

static int file_worker(SceSize args, void *argp)
{
    (void)args; (void)argp;
    while (__atomic_load_n(&io_run, __ATOMIC_ACQUIRE)) {
        unsigned bits = 0;
        SceUInt timeout = 16000;
        ksceKernelWaitEventFlag(g.ioevf, IEV_MODEL | IEV_QUIT,
                                SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &bits, &timeout);
        if (bits & IEV_QUIT) break;
        game_ocr_model_tick();
        boot_tick();
        kernel_file_flush();
    }
    return 0;
}

/* Ledger wrapper: one fixed kernel stack, including refused starts. */
int kernel_io_start(void)
{
    g.ioevf = ksceKernelCreateEventFlag("VjoFileEvents", 0, 0, NULL);
    if (g.ioevf < 0) return g.ioevf;
    __atomic_store_n(&io_run, 1, __ATOMIC_RELEASE);
    /* Storage has no controller deadline. Keep it below the input worker's
     * 0x3C priority and let the kernel choose its CPU instead of pinning CPU0. */
    io_thread = ksceKernelCreateThread("VjoFile", file_worker, 0x60, VJO_STACK_KERNEL_IO, 0, 0, NULL);
    if (io_thread >= 0) {
        vjo_ledger_add(&g_kernel_ledger, VJO_KL_IO_STACK, VJO_BUDGET_KERNEL_IO_STACK);
        int rc = ksceKernelStartThread(io_thread, 0, NULL);
        if (rc >= 0) return 0;
        ksceKernelDeleteThread(io_thread);
        vjo_ledger_sub(&g_kernel_ledger, VJO_KL_IO_STACK, VJO_BUDGET_KERNEL_IO_STACK);
    }
    io_thread = -1;
    __atomic_store_n(&io_run, 0, __ATOMIC_RELEASE);
    ksceKernelDeleteEventFlag(g.ioevf);
    g.ioevf = 0;
    return -1;
}

void kernel_io_stop(void)
{
    if (io_thread < 0) return;
    __atomic_store_n(&io_run, 0, __ATOMIC_RELEASE);
    ksceKernelSetEventFlag(g.ioevf, IEV_QUIT);
    ksceKernelWaitThreadEnd(io_thread, NULL, NULL);
    ksceKernelDeleteThread(io_thread);
    vjo_ledger_sub(&g_kernel_ledger, VJO_KL_IO_STACK, VJO_BUDGET_KERNEL_IO_STACK);
    io_thread = -1;
    ksceKernelDeleteEventFlag(g.ioevf);
    g.ioevf = 0;
}
