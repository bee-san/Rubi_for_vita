#include "app_worker.h"

#include <psp2/kernel/cpu.h>
#include <stddef.h>

int vjo_app_worker_prepare(VjoAppWorker *w)
{
    if (w->lock < 0 && (w->lock = sceKernelCreateMutex(w->lock_name, 0, 0, NULL)) < 0) {
        int rc = w->lock;
        w->lock = -1;
        return rc;
    }
    return 0;
}

void vjo_app_worker_lock(VjoAppWorker *w)
{
    sceKernelLockMutex(w->lock, 1, NULL);
}

void vjo_app_worker_unlock(VjoAppWorker *w)
{
    sceKernelUnlockMutex(w->lock, 1);
}

int vjo_app_worker_start(VjoAppWorker *w, const char *name, int (*entry)(SceSize args, void *argp))
{
    int rc;
    w->finished = w->cancelled = 0;
    /* CPU3 at a system priority, as the main thread (settings/sysapp.c) */
    w->thread = sceKernelCreateThread(name, entry, 0x10000100, VJO_APP_WORKER_STACK, 0, SCE_KERNEL_CPU_MASK_SYSTEM,
                                      NULL);
    w->running = 1;
    rc = w->thread < 0 ? w->thread : sceKernelStartThread(w->thread, 0, NULL);
    if (rc < 0) {
        if (w->thread >= 0)
            sceKernelDeleteThread(w->thread);
        w->thread = -1;
        w->running = 0;
        return rc;
    }
    return 0;
}

int vjo_app_worker_stopped(VjoAppWorker *w)
{
    int c;
    sceKernelLockMutex(w->lock, 1, NULL);
    c = w->cancelled;
    sceKernelUnlockMutex(w->lock, 1);
    return c;
}

void vjo_app_worker_cancel(VjoAppWorker *w)
{
    if (!w->running)
        return;
    sceKernelLockMutex(w->lock, 1, NULL);
    w->cancelled = 1;
    sceKernelUnlockMutex(w->lock, 1);
}

int vjo_app_worker_reap(VjoAppWorker *w)
{
    int done;
    if (!w->running)
        return 0;
    sceKernelLockMutex(w->lock, 1, NULL);
    done = w->finished;
    sceKernelUnlockMutex(w->lock, 1);
    if (!done)
        return 0;
    vjo_app_worker_join(w);
    return 1;
}

void vjo_app_worker_join(VjoAppWorker *w)
{
    if (!w->running)
        return;
    sceKernelWaitThreadEnd(w->thread, NULL, NULL);
    sceKernelDeleteThread(w->thread);
    w->thread = -1;
    w->running = 0;
}
