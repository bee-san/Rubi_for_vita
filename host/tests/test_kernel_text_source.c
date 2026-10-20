/* API 31: kernel/text_source.c's syscalls, linked from production: vjoSetTextSource only from the
 * registered Shell and only VJO_TEXT_SOURCE_*, vjoGetTextSource from anyone, OCR until the Shell set a
 * source and in a safe boot whatever it set, and (audit-hooks F1) OCR to every process the loader did not
 * start the worker in as ON (text_source_started). Only the SDK boundary is substituted. */
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
void text_source_started(SceUID pid, int on);
#include "../../kernel/text_source.c"

unsigned syscall_depth;
static SceUID caller;
static int mode = VJO_BOOT_NORMAL;
static unsigned log_calls;
static char last_log[192];

#define SHELL 0x40010001
#define GAME 0x40010003 /* a game's process, its worker started by the loader as ON */

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
    memset(on_pid, 0, sizeof(on_pid));
    on_next = 0;
    g.shell_pid = 0;
    caller = 0;
    mode = VJO_BOOT_NORMAL;
    log_calls = 0;
    last_log[0] = 0;
}

/* What process pid's worker reads (its module_start's call). */
static int read_as(SceUID pid)
{
    SceUID keep = caller;
    int v;
    caller = pid;
    v = vjoGetTextSource();
    caller = keep;
    return v;
}

/* Before any Shell sets one: OCR (no hooks), for every caller. */
static void test_ocr_until_set(void)
{
    reset();
    text_source_started(GAME, 1);
    TEST_CHECK(vjoGetTextSource() == (int)VJO_TEXT_SOURCE_OCR);
    TEST_CHECK(read_as(GAME) == (int)VJO_TEXT_SOURCE_OCR);
    g.shell_pid = SHELL;
    TEST_CHECK(read_as(GAME) == (int)VJO_TEXT_SOURCE_OCR);
    TEST_CHECK(log_calls == 0);
}

/* Only the registered Shell sets it: before registration nobody, then not a game or another process. */
static void test_shell_only(void)
{
    reset();
    text_source_started(GAME, 1);
    caller = SHELL;
    TEST_CHECK(vjoSetTextSource(VJO_TEXT_SOURCE_AUTO) == VJO_ERR_PERM); /* no Shell registered */
    g.shell_pid = SHELL;
    caller = GAME;
    TEST_CHECK(vjoSetTextSource(VJO_TEXT_SOURCE_HOOKS) == VJO_ERR_PERM); /* a game */
    caller = 0;
    TEST_CHECK(vjoSetTextSource(VJO_TEXT_SOURCE_HOOKS) == VJO_ERR_PERM); /* the kernel's own (pid 0) */
    TEST_CHECK(read_as(GAME) == (int)VJO_TEXT_SOURCE_OCR);
    caller = SHELL;
    TEST_CHECK(vjoSetTextSource(VJO_TEXT_SOURCE_HOOKS) == 0);
    TEST_CHECK(read_as(GAME) == (int)VJO_TEXT_SOURCE_HOOKS); /* the game worker reads what the Shell set */
    /* A Shell that registered again (SceShell restarted): the old process is refused, the value stays. */
    g.shell_pid = 0x40010005;
    caller = SHELL;
    TEST_CHECK(vjoSetTextSource(VJO_TEXT_SOURCE_OCR) == VJO_ERR_PERM);
    TEST_CHECK(read_as(GAME) == (int)VJO_TEXT_SOURCE_HOOKS);
}

/* Every mode under VJO_TEXT_SOURCE_COUNT, nothing else; a refused one leaves the value as it was. */
static void test_modes(void)
{
    reset();
    text_source_started(GAME, 1);
    g.shell_pid = caller = SHELL;
    for (uint32_t m = 0; m < VJO_TEXT_SOURCE_COUNT; m++) {
        TEST_CHECK(vjoSetTextSource(m) == 0);
        TEST_CHECK(read_as(GAME) == (int)m);
    }
    TEST_CHECK(vjoSetTextSource(VJO_TEXT_SOURCE_AUTO) == 0);
    const uint32_t bad[] = {VJO_TEXT_SOURCE_COUNT, 4u, 0xFFu, 0x80000000u, 0xFFFFFFFFu};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_CHECK(vjoSetTextSource(bad[i]) == VJO_ERR_ARG);
        TEST_MSG("mode 0x%X", bad[i]);
        TEST_CHECK(read_as(GAME) == (int)VJO_TEXT_SOURCE_AUTO);
    }
    TEST_CHECK(VJO_TEXT_SOURCE_OCR == 0u && VJO_TEXT_SOURCE_AUTO == 1u && VJO_TEXT_SOURCE_HOOKS == 2u &&
               VJO_TEXT_SOURCE_COUNT == 3u);
}

/* A change is logged once; the Shell's every config read with the same value logs nothing. */
static void test_log_on_change(void)
{
    reset();
    g.shell_pid = caller = SHELL;
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
    text_source_started(GAME, 1);
    g.shell_pid = caller = SHELL;
    TEST_CHECK(vjoSetTextSource(VJO_TEXT_SOURCE_HOOKS) == 0);
    mode = VJO_BOOT_SAFE;
    TEST_CHECK(read_as(GAME) == (int)VJO_TEXT_SOURCE_OCR);
    TEST_CHECK(read_as(SHELL) == (int)VJO_TEXT_SOURCE_OCR);
    mode = VJO_BOOT_NORMAL;
    TEST_CHECK(read_as(GAME) == (int)VJO_TEXT_SOURCE_HOOKS);
}

/* audit-hooks F1: the Shell's value goes only to a process the loader started the worker in as ON. A
 * worker that a leftover *TITLEID line loads into a game that is OFF (a start recorded with on = 0, or
 * never recorded), the Shell, the kernel and any other process read OCR: no hooks there. */
static void test_only_processes_started_on(void)
{
    reset();
    g.shell_pid = caller = SHELL;
    TEST_CHECK(vjoSetTextSource(VJO_TEXT_SOURCE_HOOKS) == 0);
    TEST_CHECK(read_as(GAME) == (int)VJO_TEXT_SOURCE_OCR); /* not started by the loader */
    text_source_started(GAME, 1);
    TEST_CHECK(read_as(GAME) == (int)VJO_TEXT_SOURCE_HOOKS);
    text_source_started(0x40010007, 0); /* a game that is OFF: its leftover line's worker */
    TEST_CHECK(read_as(0x40010007) == (int)VJO_TEXT_SOURCE_OCR);
    TEST_CHECK(read_as(0x40010009) == (int)VJO_TEXT_SOURCE_OCR); /* never seen */
    TEST_CHECK(read_as(SHELL) == (int)VJO_TEXT_SOURCE_OCR);
    TEST_CHECK(read_as(GAME) == (int)VJO_TEXT_SOURCE_HOOKS); /* the ON game's untouched */
    /* No ID <= 0 is recorded, and none reads the value (the table's free slots are 0). */
    text_source_started(0, 1);
    text_source_started(-5, 1);
    TEST_CHECK(read_as(0) == (int)VJO_TEXT_SOURCE_OCR && read_as(-5) == (int)VJO_TEXT_SOURCE_OCR);
    for (unsigned i = 0; i < TEXT_SOURCE_PIDS; i++) TEST_CHECK(on_pid[i] == 0 || on_pid[i] == GAME);
    /* The same process ID at a start that is not ON (an ID used again, a game turned OFF): OCR from then. */
    text_source_started(GAME, 0);
    TEST_CHECK(read_as(GAME) == (int)VJO_TEXT_SOURCE_OCR);
    /* Started as ON twice: one entry. */
    text_source_started(GAME, 1);
    text_source_started(GAME, 1);
    unsigned n = 0;
    for (unsigned i = 0; i < TEXT_SOURCE_PIDS; i++) n += on_pid[i] == GAME;
    TEST_CHECK(n == 1 && read_as(GAME) == (int)VJO_TEXT_SOURCE_HOOKS);
    TEST_CHECK(syscall_depth == 0); /* no user memory: no syscall frame */
}

/* The table holds the last TEXT_SOURCE_PIDS starts that were ON: the next replaces the oldest. */
static void test_pid_table_wraps(void)
{
    reset();
    g.shell_pid = caller = SHELL;
    TEST_CHECK(vjoSetTextSource(VJO_TEXT_SOURCE_AUTO) == 0);
    TEST_CHECK(TEXT_SOURCE_PIDS == 4);
    for (SceUID p = 1; p <= 4; p++) text_source_started(0x40020000 + p, 1);
    for (SceUID p = 1; p <= 4; p++) TEST_CHECK(read_as(0x40020000 + p) == (int)VJO_TEXT_SOURCE_AUTO);
    text_source_started(0x40020005, 1); /* the fifth: the first goes */
    TEST_CHECK(read_as(0x40020001) == (int)VJO_TEXT_SOURCE_OCR);
    for (SceUID p = 2; p <= 5; p++) TEST_CHECK_(read_as(0x40020000 + p) == (int)VJO_TEXT_SOURCE_AUTO, "%d", (int)p);
    text_source_started(0x40020006, 1); /* the sixth: the second */
    TEST_CHECK(read_as(0x40020002) == (int)VJO_TEXT_SOURCE_OCR);
    for (SceUID p = 3; p <= 6; p++) TEST_CHECK_(read_as(0x40020000 + p) == (int)VJO_TEXT_SOURCE_AUTO, "%d", (int)p);
    /* The slot counter wraps with its 32 bits: still one slot of the four, each in turn. */
    reset();
    g.shell_pid = caller = SHELL;
    TEST_CHECK(vjoSetTextSource(VJO_TEXT_SOURCE_HOOKS) == 0);
    on_next = 0xFFFFFFFEu;
    for (SceUID p = 1; p <= 5; p++) text_source_started(0x40030000 + p, 1);
    TEST_CHECK(on_next == 3u);
    TEST_CHECK(read_as(0x40030001) == (int)VJO_TEXT_SOURCE_OCR); /* the fifth took its slot (0xFFFFFFFE % 4 = 2) */
    for (SceUID p = 2; p <= 5; p++) TEST_CHECK_(read_as(0x40030000 + p) == (int)VJO_TEXT_SOURCE_HOOKS, "%d", (int)p);
}

/* The syscalls touch no user memory and take no lock: neither needs the syscall frame. */
static void test_no_syscall_frame(void)
{
    reset();
    text_source_started(GAME, 1);
    g.shell_pid = caller = SHELL;
    syscall_depth = 0;
    TEST_CHECK(vjoSetTextSource(VJO_TEXT_SOURCE_AUTO) == 0);
    TEST_CHECK(read_as(GAME) == (int)VJO_TEXT_SOURCE_AUTO);
    TEST_CHECK(syscall_depth == 0);
    /* the kernel's .bss: the word, the four process IDs and the slot counter */
    TEST_CHECK(sizeof(text_source) + sizeof(on_pid) + sizeof(on_next) == 24);
}

TEST_LIST = {
    {"ocr_until_set", test_ocr_until_set},
    {"shell_only", test_shell_only},
    {"modes", test_modes},
    {"log_on_change", test_log_on_change},
    {"safe_boot_reads_ocr", test_safe_boot_reads_ocr},
    {"only_processes_started_on", test_only_processes_started_on},
    {"pid_table_wraps", test_pid_table_wraps},
    {"no_syscall_frame", test_no_syscall_frame},
    {NULL, NULL},
};
