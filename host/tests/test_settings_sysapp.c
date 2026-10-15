/* settings/sysapp.c on the host: the system application's process values and
 * its sceKernelCreateThread wrapper (the link's --wrap, vita/CMakeLists.txt),
 * which puts every thread the app creates on CPU3 at a system priority
 * whatever the call asks (audit-pr109's M5: the Anki worker's mask set to 0
 * built and packaged). tools/check_settings_self.py checks that the build's
 * calls go through it; this checks what it passes on. */
#include "acutest.h"
#include "../../settings/sysapp.c"

static struct {
    int calls;
    const char *name;
    SceKernelThreadEntry entry;
    int priority, affinity;
    SceSize stack;
    SceUInt attr;
    const SceKernelThreadOptParam *option;
} got;

SceUID __real_sceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int priority, SceSize stack,
                                    SceUInt attr, int affinity, const SceKernelThreadOptParam *option)
{
    got.calls++;
    got.name = name;
    got.entry = entry;
    got.priority = priority;
    got.stack = stack;
    got.attr = attr;
    got.affinity = affinity;
    got.option = option;
    return 0x40010000 + got.calls;
}

static int worker(SceSize args, void *argp) { (void)args; (void)argp; return 0; }

static void the_process_values(void)
{
    TEST_CHECK(sceUserMainThreadCpuAffinityMask == SCE_KERNEL_CPU_MASK_SYSTEM);
    TEST_CHECK(sceUserMainThreadPriority == 0x10000100);
    TEST_CHECK(_newlib_heap_size_user == 8 * 1024 * 1024);
}

/* Any CPU mask asked for, CPU3 given; everything else passed on as it is. */
static void every_thread_runs_on_cpu3(void)
{
    static const int masks[] = {0, SCE_KERNEL_CPU_MASK_USER_0, SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2,
                                SCE_KERNEL_CPU_MASK_USER_ALL, SCE_KERNEL_CPU_MASK_SYSTEM, -1};
    SceKernelThreadOptParam option = {sizeof(option), 0};
    for (unsigned i = 0; i < sizeof(masks) / sizeof(masks[0]); i++) {
        memset(&got, 0, sizeof(got));
        SceUID uid = __wrap_sceKernelCreateThread("VjoAnkiApp", worker, 0x10000100, 128 * 1024, 0x80000000u, masks[i],
                                                  &option);
        TEST_CHECK_(got.calls == 1 && got.affinity == SCE_KERNEL_CPU_MASK_SYSTEM, "mask 0x%x: 0x%x", masks[i],
                    got.affinity);
        TEST_CHECK(uid == 0x40010001);
        TEST_CHECK(!strcmp(got.name, "VjoAnkiApp") && got.entry == worker && got.stack == 128 * 1024 &&
                   got.attr == 0x80000000u && got.option == &option);
    }
}

/* An application priority (below 0x10000000: 0 is the caller's, 64-191 a
 * game's) becomes the main thread's; a system one, the default with an
 * offset, is kept. */
static void application_priorities_become_the_main_threads(void)
{
    static const int asked[] = {0, 64, 0xA0, 191, 0x0FFFFFFF, 0x10000100, 0x100000E1, 0x10000120};
    static const int given[] = {0x10000100, 0x10000100, 0x10000100, 0x10000100, 0x10000100, 0x10000100, 0x100000E1,
                                0x10000120};
    for (unsigned i = 0; i < sizeof(asked) / sizeof(asked[0]); i++) {
        memset(&got, 0, sizeof(got));
        __wrap_sceKernelCreateThread("t", worker, asked[i], 0x1000, 0, SCE_KERNEL_CPU_MASK_SYSTEM, NULL);
        TEST_CHECK_(got.priority == given[i], "asked 0x%x: 0x%x, want 0x%x", asked[i], got.priority, given[i]);
    }
}

TEST_LIST = {
    {"the process values: CPU3, 0x10000100, an 8 MiB heap", the_process_values},
    {"every thread runs on CPU3", every_thread_runs_on_cpu3},
    {"application priorities become the main thread's", application_priorities_become_the_main_threads},
    {NULL, NULL}
};
