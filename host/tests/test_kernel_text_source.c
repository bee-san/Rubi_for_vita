/* API 31: kernel/text_source.c's syscalls, linked from production: vjoSetTextSource only from the
 * registered Shell and only VJO_TEXT_SOURCE_*, vjoGetTextSource from anyone, OCR until the Shell set a
 * source and in a safe boot whatever it set. Only the SDK boundary is substituted. */
#include "acutest.h"
#include <stdarg.h>
#include <stdio.h>
#include <psp2kern/host_stubs.h>
#include "../../include/vjo_api.h"
#include "../../core/boot_guard.h"

/* Keep the kernel's other state out of this harness: the syscalls read g.shell_pid only. */
#define VJO_KERNEL_H
static struct {
    volatile SceUID shell_pid;
} g;
void klog(const char *fmt, ...);
int boot_mode(void);
#include "../../kernel/text_source.c"

unsigned syscall_depth;
static SceUID caller;
static int mode = VJO_BOOT_NORMAL;
static unsigned log_calls;
static char last_log[192];

void klog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof(last_log), fmt, ap);
    va_end(ap);
    log_calls++;
}
SceUID ksceKernelGetProcessId(void) { return caller; }
int boot_mode(void) { return mode; }

static void reset(void)
{
    text_source = 0; /* the module's .bss as it loads */
    g.shell_pid = 0;
    caller = 0;
    mode = VJO_BOOT_NORMAL;
    log_calls = 0;
    last_log[0] = 0;
}

/* Before any Shell sets one: OCR (no hooks), for every caller. */
static void test_ocr_until_set(void)
{
    reset();
    TEST_CHECK(vjoGetTextSource() == (int)VJO_TEXT_SOURCE_OCR);
    caller = 0x40010003; /* a game's process */
    TEST_CHECK(vjoGetTextSource() == (int)VJO_TEXT_SOURCE_OCR);
    g.shell_pid = 0x40010001;
    TEST_CHECK(vjoGetTextSource() == (int)VJO_TEXT_SOURCE_OCR);
    TEST_CHECK(log_calls == 0);
}

/* Only the registered Shell sets it: before registration nobody, then not a game or another process. */
static void test_shell_only(void)
{
    reset();
    caller = 0x40010001;
    TEST_CHECK(vjoSetTextSource(VJO_TEXT_SOURCE_AUTO) == VJO_ERR_PERM); /* no Shell registered */
    g.shell_pid = 0x40010001;
    caller = 0x40010003;
    TEST_CHECK(vjoSetTextSource(VJO_TEXT_SOURCE_HOOKS) == VJO_ERR_PERM); /* a game */
    caller = 0;
    TEST_CHECK(vjoSetTextSource(VJO_TEXT_SOURCE_HOOKS) == VJO_ERR_PERM); /* the kernel's own (pid 0) */
    TEST_CHECK(vjoGetTextSource() == (int)VJO_TEXT_SOURCE_OCR);
    caller = 0x40010001;
    TEST_CHECK(vjoSetTextSource(VJO_TEXT_SOURCE_HOOKS) == 0);
    caller = 0x40010003; /* the game worker reads what the Shell set */
    TEST_CHECK(vjoGetTextSource() == (int)VJO_TEXT_SOURCE_HOOKS);
    /* A Shell that registered again (SceShell restarted): the old process is refused, the value stays. */
    g.shell_pid = 0x40010005;
    caller = 0x40010001;
    TEST_CHECK(vjoSetTextSource(VJO_TEXT_SOURCE_OCR) == VJO_ERR_PERM);
    TEST_CHECK(vjoGetTextSource() == (int)VJO_TEXT_SOURCE_HOOKS);
}

/* Every mode under VJO_TEXT_SOURCE_COUNT, nothing else; a refused one leaves the value as it was. */
static void test_modes(void)
{
    reset();
    g.shell_pid = caller = 0x40010001;
    for (uint32_t m = 0; m < VJO_TEXT_SOURCE_COUNT; m++) {
        TEST_CHECK(vjoSetTextSource(m) == 0);
        TEST_CHECK(vjoGetTextSource() == (int)m);
    }
    TEST_CHECK(vjoSetTextSource(VJO_TEXT_SOURCE_AUTO) == 0);
    const uint32_t bad[] = {VJO_TEXT_SOURCE_COUNT, 4u, 0xFFu, 0x80000000u, 0xFFFFFFFFu};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_CHECK(vjoSetTextSource(bad[i]) == VJO_ERR_ARG);
        TEST_MSG("mode 0x%X", bad[i]);
        TEST_CHECK(vjoGetTextSource() == (int)VJO_TEXT_SOURCE_AUTO);
    }
    TEST_CHECK(VJO_TEXT_SOURCE_OCR == 0u && VJO_TEXT_SOURCE_AUTO == 1u && VJO_TEXT_SOURCE_HOOKS == 2u &&
               VJO_TEXT_SOURCE_COUNT == 3u);
}

/* A change is logged once; the Shell's every config read with the same value logs nothing. */
static void test_log_on_change(void)
{
    reset();
    g.shell_pid = caller = 0x40010001;
    TEST_CHECK(vjoSetTextSource(VJO_TEXT_SOURCE_OCR) == 0);
    TEST_CHECK(log_calls == 0); /* OCR already */
    TEST_CHECK(vjoSetTextSource(VJO_TEXT_SOURCE_AUTO) == 0);
    TEST_CHECK(log_calls == 1 && !strcmp(last_log, "text source 1"));
    for (int i = 0; i < 5; i++)
        TEST_CHECK(vjoSetTextSource(VJO_TEXT_SOURCE_AUTO) == 0);
    TEST_CHECK(log_calls == 1);
    TEST_CHECK(vjoSetTextSource(VJO_TEXT_SOURCE_HOOKS) == 0);
    TEST_CHECK(log_calls == 2 && !strcmp(last_log, "text source 2"));
    TEST_CHECK(vjoSetTextSource(7u) == VJO_ERR_ARG && log_calls == 2);
}

/* A safe boot: every reader gets OCR, so no worker installs a hook, whatever the Shell set. */
static void test_safe_boot_reads_ocr(void)
{
    reset();
    g.shell_pid = caller = 0x40010001;
    TEST_CHECK(vjoSetTextSource(VJO_TEXT_SOURCE_HOOKS) == 0);
    mode = VJO_BOOT_SAFE;
    TEST_CHECK(vjoGetTextSource() == (int)VJO_TEXT_SOURCE_OCR);
    caller = 0x40010003;
    TEST_CHECK(vjoGetTextSource() == (int)VJO_TEXT_SOURCE_OCR);
    mode = VJO_BOOT_NORMAL;
    TEST_CHECK(vjoGetTextSource() == (int)VJO_TEXT_SOURCE_HOOKS);
}

/* The syscalls touch no user memory and take no lock: neither needs the syscall frame. */
static void test_no_syscall_frame(void)
{
    reset();
    g.shell_pid = caller = 0x40010001;
    syscall_depth = 0;
    TEST_CHECK(vjoSetTextSource(VJO_TEXT_SOURCE_AUTO) == 0);
    TEST_CHECK(vjoGetTextSource() == (int)VJO_TEXT_SOURCE_AUTO);
    TEST_CHECK(syscall_depth == 0);
    TEST_CHECK(sizeof(text_source) == 4); /* one word of the kernel's .bss */
}

TEST_LIST = {
    {"ocr_until_set", test_ocr_until_set},
    {"shell_only", test_shell_only},
    {"modes", test_modes},
    {"log_on_change", test_log_on_change},
    {"safe_boot_reads_ocr", test_safe_boot_reads_ocr},
    {"no_syscall_frame", test_no_syscall_frame},
    {NULL, NULL},
};
