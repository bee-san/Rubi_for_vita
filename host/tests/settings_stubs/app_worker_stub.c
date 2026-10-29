/* Deterministic worker scheduling for main.c's scripted draw-loop harnesses. The job is held
 * until a later poll, or until normal exit joins it. The real worker's SDK/thread handling is
 * exercised by settings_anki and settings_install. */
#include "../../../settings/app_worker.h"
#include <assert.h>
#include <stddef.h>

int vjo_app_worker_stub_delay, vjo_app_worker_stub_fail_start;
static int (*pending)(SceSize, void *);

int vjo_app_worker_prepare(VjoAppWorker *w) { w->lock = 1; return 0; }
void vjo_app_worker_lock(VjoAppWorker *w) { (void)w; }
void vjo_app_worker_unlock(VjoAppWorker *w) { (void)w; }
int vjo_app_worker_start(VjoAppWorker *w, const char *name, int (*entry)(SceSize, void *))
{
    (void)name;
    assert(!w->running && !pending);
    if (vjo_app_worker_stub_fail_start) return -1;
    pending = entry;
    w->running = 1;
    w->finished = w->cancelled = 0;
    return 0;
}
void vjo_app_worker_join(VjoAppWorker *w)
{
    if (!w->running) return;
    assert(pending);
    int (*entry)(SceSize, void *) = pending;
    pending = NULL;
    entry(0, NULL);
    assert(w->finished);
    w->running = 0;
}
int vjo_app_worker_reap(VjoAppWorker *w)
{
    if (!w->running) return 0;
    if (vjo_app_worker_stub_delay > 0) { vjo_app_worker_stub_delay--; return 0; }
    vjo_app_worker_join(w);
    return 1;
}
void vjo_app_worker_cancel(VjoAppWorker *w) { w->cancelled = 1; }
int vjo_app_worker_stopped(VjoAppWorker *w) { return w->cancelled; }
