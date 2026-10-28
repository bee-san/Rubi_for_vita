/* The Rubi app's worker threads (settings/anki_page.c, install_page.c): one job at a time on a thread
 * of its own, which the main thread starts, asks to stop and joins once the job says it has finished. Each page keeps
 * its job's own state (its progress text, its result) and writes it under the worker's lock, with finished. */
#ifndef VJO_SETTINGS_APP_WORKER_H
#define VJO_SETTINGS_APP_WORKER_H
#include <psp2/kernel/threadmgr.h>

/* A worker thread's stack: a page's job runs in it (tools/check_settings_self.py counts two of them). */
#define VJO_APP_WORKER_STACK (64 * 1024)

typedef struct {
    const char *lock_name; /* its mutex's name */
    SceUID lock;           /* made at the first start (vjo_app_worker_prepare); -1 until then */
    SceUID thread;         /* the job's thread while it runs; -1 when none */
    int running;           /* a thread was started and is not joined yet (the main thread's) */
    int finished;          /* the job is done: set by the job, under the lock */
    int cancelled;         /* the user stopped it: set by the main thread, under the lock */
} VjoAppWorker;
#define VJO_APP_WORKER_INIT(name) {(name), -1, -1, 0, 0, 0}

/* The lock, made the first time (lock_name): 0, or the SDK's error (no lock: lock stays -1, and nothing starts). */
int vjo_app_worker_prepare(VjoAppWorker *w);
void vjo_app_worker_lock(VjoAppWorker *w);
void vjo_app_worker_unlock(VjoAppWorker *w);
/* entry on a new thread called name, on CPU3 at a system priority as the main thread (settings/sysapp.c), with
 * VJO_APP_WORKER_STACK of stack; finished and cancelled cleared first. The lock is prepared and no job runs. 0, or the
 * SDK's error (then no thread is left and none runs). */
int vjo_app_worker_start(VjoAppWorker *w, const char *name, int (*entry)(SceSize args, void *argp));
/* The job's check (on its thread): cancelled, read under the lock. */
int vjo_app_worker_stopped(VjoAppWorker *w);
/* Asks a running job to stop: cancelled, under the lock. Nothing when none runs. */
void vjo_app_worker_cancel(VjoAppWorker *w);
/* The job has said it finished (read under the lock): its thread joined and deleted, 1. 0 when none runs or it has not
 * finished. */
int vjo_app_worker_reap(VjoAppWorker *w);
/* Waits for a running job's end, then deletes its thread (leaving the app; vjo_app_worker_cancel first stops it). */
void vjo_app_worker_join(VjoAppWorker *w);

#endif
