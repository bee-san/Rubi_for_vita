/* Run the actual kernel file worker on a host thread. A blocked disk call
 * must leave its caller free; start failures must return every ledger byte. */
#include "acutest.h"
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include "../../kernel/io_worker.c"

VjoKernelState g;
static pthread_t thread;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static unsigned events;
static int entered_disk, hold_disk, fail_stage, event_deletes, thread_deletes, joined;
static int (*entry)(SceSize, void *);

void klog(const char *fmt, ...) { (void)fmt; }
void game_ocr_model_tick(void)
{
    pthread_mutex_lock(&mutex);
    entered_disk = 1;
    pthread_cond_broadcast(&cond);
    while (hold_disk) pthread_cond_wait(&cond, &mutex);
    pthread_mutex_unlock(&mutex);
}
void boot_tick(void) {}
void kernel_file_flush(void) {}
SceUID ksceKernelCreateEventFlag(const char *name, unsigned attr, unsigned bits, void *opt)
{ (void)name; (void)attr; (void)bits; (void)opt; return fail_stage == 1 ? -1 : 41; }
int ksceKernelDeleteEventFlag(SceUID uid) { TEST_CHECK(uid == 41); event_deletes++; return 0; }
int ksceKernelSetEventFlag(SceUID uid, unsigned bits)
{
    TEST_CHECK(uid == 41);
    pthread_mutex_lock(&mutex);
    events |= bits;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&mutex);
    return 0;
}
int ksceKernelWaitEventFlag(SceUID uid, unsigned bits, unsigned flags, unsigned *out, SceUInt *timeout)
{
    (void)uid; (void)flags;
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += (long)*timeout * 1000;
    until.tv_sec += until.tv_nsec / 1000000000;
    until.tv_nsec %= 1000000000;
    pthread_mutex_lock(&mutex);
    while (!(events & bits))
        if (pthread_cond_timedwait(&cond, &mutex, &until)) break;
    *out = events & bits;
    events &= ~bits;
    pthread_mutex_unlock(&mutex);
    return 0;
}
SceUID ksceKernelCreateThread(const char *name, int (*fn)(SceSize, void *), int priority,
                              SceSize stack, unsigned attr, int affinity, void *opt)
{
    TEST_CHECK(!strcmp(name, "VjoFile") && stack == VJO_STACK_KERNEL_IO && priority == 0x60 && !affinity);
    (void)attr; (void)opt; entry = fn;
    return fail_stage == 2 ? -1 : 42;
}
static void *run(void *unused) { (void)unused; entry(0, NULL); return NULL; }
int ksceKernelStartThread(SceUID uid, SceSize args, void *argp)
{ TEST_CHECK(uid == 42); (void)args; (void)argp; return fail_stage == 3 ? -1 : pthread_create(&thread, NULL, run, NULL); }
int ksceKernelWaitThreadEnd(SceUID uid, int *status, void *timeout)
{ TEST_CHECK(uid == 42); (void)status; (void)timeout; joined++; return pthread_join(thread, NULL); }
int ksceKernelDeleteThread(SceUID uid) { TEST_CHECK(uid == 42); thread_deletes++; return 0; }

static void reset(void)
{
    memset(&g, 0, sizeof(g));
    events = 0; entered_disk = hold_disk = fail_stage = event_deletes = thread_deletes = joined = 0;
    kledger_init();
}
static void disk_does_not_block_the_caller(void)
{
    reset(); hold_disk = 1;
    TEST_ASSERT(kernel_io_start() == 0);
    TEST_CHECK(g_kernel_ledger.current[VJO_KL_IO_STACK] == VJO_BUDGET_KERNEL_IO_STACK);
    ksceKernelSetEventFlag(g.ioevf, IEV_MODEL);
    int observed = 0;
    for (int i = 0; i < 1000 && !observed; i++) {
        pthread_mutex_lock(&mutex); observed = entered_disk; pthread_mutex_unlock(&mutex);
        if (!observed) usleep(1000);
    }
    TEST_CHECK(observed);
    /* Start returned while the actual worker remains blocked on another thread. */
    TEST_CHECK(!pthread_equal(pthread_self(), thread) && !joined);
    pthread_mutex_lock(&mutex); hold_disk = 0; pthread_cond_broadcast(&cond); pthread_mutex_unlock(&mutex);
    kernel_io_stop();
    TEST_CHECK(joined == 1 && event_deletes == 1 && thread_deletes == 1 && !g.ioevf);
    TEST_CHECK(!g_kernel_ledger.current[VJO_KL_IO_STACK]);
}
static void failed_starts_return_the_stack(void)
{
    for (int stage = 1; stage <= 3; stage++) {
        reset(); fail_stage = stage;
        TEST_CHECK(kernel_io_start() < 0);
        TEST_CHECK(!g_kernel_ledger.current[VJO_KL_IO_STACK]);
        TEST_CHECK(event_deletes == (stage >= 2) && thread_deletes == (stage == 3));
        kernel_io_stop();
        TEST_CHECK(!joined);
    }
}
TEST_LIST = {{"blocked disk I/O leaves its caller free and joins before deletion", disk_does_not_block_the_caller},
             {"event, thread and start failures leak no stack", failed_starts_return_the_stack}, {NULL, NULL}};
