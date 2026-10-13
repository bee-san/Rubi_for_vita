/* Boot-safety guard on the host: the record (core/boot_guard.c) and the real
 * kernel-side code (kernel/boot.c) with ur0:data/VitaJPOverlay/boot.txt
 * replaced by an in-memory file and the clock by a counter. Each simulated
 * boot runs the same calls the console makes, in the same order: kernel
 * module_start (boot_init), the Shell's marks, the worker's ticks, then a
 * "power cut" at some point, which is simply the next boot_init. */
#include "acutest.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <psp2kern/host_stubs.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include "../../core/boot_guard.h"
#include "../../include/vjo_api.h"

/* kernel/boot.c's interface (kernel/vjo_kernel.h pulls in the whole kernel
 * state; only these are needed here) */
int boot_init(int api);
int boot_mode(void);
int boot_shell_seen(void);
int boot_mark(int stage);
void boot_mark_inert(void);
void boot_fault(int fault);
void boot_info(VjoBootInfo *out);
void boot_tick(void);
void boot_test_reset(void);
void klog(const char *fmt, ...);

/* ---- the simulated ur0 file and clock ---- */
static char file[512];
static int file_len = -1;       /* -1: no file (ENOENT) */
static int open_fail_rc;        /* every open fails with this (0: no failure) */
static int write_fail_rc;       /* every write fails with this */
static int short_write;         /* writes report one byte fewer */
static unsigned opens, writes, mkdirs;
static uint64_t clock_us = 1000000;
static char log_text[8192];
static int log_len;
static int fd_open, fd_flags;

void klog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (log_len < (int)sizeof(log_text) - 1)
        log_len += vsnprintf(log_text + log_len, sizeof(log_text) - (size_t)log_len, fmt, ap);
    va_end(ap);
    if (log_len < (int)sizeof(log_text) - 1)
        log_text[log_len++] = '\n';
}
uint64_t ksceKernelGetSystemTimeWide(void) { return clock_us; }
int ksceIoMkdir(const char *dir, unsigned mode) { mkdirs++; return 0; }
SceUID ksceIoOpen(const char *path, int flags, unsigned mode)
{
    opens++;
    TEST_CHECK(!strcmp(path, "ur0:data/VitaJPOverlay/boot.txt"));
    TEST_CHECK(!fd_open);
    if (open_fail_rc) return open_fail_rc;
    if (flags == SCE_O_RDONLY && file_len < 0) return (int)0x80010002; /* ENOENT */
    if (flags & SCE_O_TRUNC) file_len = 0;
    fd_open = 1; fd_flags = flags;
    return 7;
}
int ksceIoRead(SceUID fd, void *data, SceSize size)
{
    TEST_CHECK(fd == 7 && fd_open && fd_flags == SCE_O_RDONLY);
    int n = file_len < (int)size ? file_len : (int)size;
    memcpy(data, file, (size_t)n);
    return n;
}
int ksceIoWrite(SceUID fd, const void *data, SceSize size)
{
    writes++;
    TEST_CHECK(fd == 7 && fd_open && (fd_flags & SCE_O_WRONLY));
    if (write_fail_rc) return write_fail_rc;
    TEST_CHECK(file_len + (int)size <= (int)sizeof(file));
    memcpy(file + file_len, data, size);
    file_len += (int)size;
    return (int)size - (short_write ? 1 : 0);
}
int ksceIoClose(SceUID fd) { TEST_CHECK(fd == 7 && fd_open); fd_open = 0; return 0; }

static void reset_world(void)
{
    file_len = -1;
    open_fail_rc = write_fail_rc = short_write = 0;
    opens = writes = mkdirs = 0;
    clock_us = 1000000;
    log_len = 0;
    log_text[0] = 0;
    boot_test_reset();
}

/* One boot's kernel start: the "power cut" of the previous one. */
static int reboot(void)
{
    boot_test_reset();
    clock_us += 1000000; /* the console was off for a while; the clock restarts anyway */
    return boot_init(VJO_API_VERSION);
}

static VjoBootInfo info(void)
{
    VjoBootInfo i;
    boot_info(&i);
    return i;
}

static void tick_for(uint64_t us)
{
    uint64_t end = clock_us + us;
    while (clock_us < end) {
        clock_us += 16000;
        boot_tick();
    }
}

/* The Shell's part of a healthy normal boot: module_start, worker, a minute. */
static void shell_runs_fine(void)
{
    TEST_CHECK(boot_mark(VJO_BOOT_STAGE_SHELL) == 0);
    tick_for(2000000);
    TEST_CHECK(boot_mark(VJO_BOOT_STAGE_WORKER) == 0);
    tick_for(61000000);
    TEST_CHECK(boot_mark(VJO_BOOT_STAGE_OK) == 0);
    tick_for(100000);
}

/* ---------------- the record ---------------- */

static void test_format_and_parse_round_trip(void)
{
    VjoBootRecord r = {12, VJO_BOOT_SAFE, VJO_BOOT_STAGE_WORKER, 1, VJO_BOOT_FAULT_SHELL_EXIT, 2, 17,
                       VJO_BOOT_NORMAL, VJO_BOOT_STAGE_OK, 0, 1};
    VjoBootRecord back;
    char text[VJO_BOOT_RECORD_MAX];
    int n = vjo_boot_format(&r, text, sizeof(text));
    TEST_CHECK(n > 0 && text[n] == 0 && text[n - 1] == '\n');
    TEST_CHECK_(!strcmp(text, "vjoboot1 seq=12 mode=1 stage=3 inert=1 fault=1 failures=2 api=17 prev=0/4/0/1\n"), "%s", text);
    TEST_CHECK(vjo_boot_parse(text, n, &back) == 0);
    TEST_CHECK(!memcmp(&back, &r, sizeof(r)));
    /* the largest record fits the fixed buffer */
    VjoBootRecord big = {VJO_BOOT_MAX_SEQ, 1, VJO_BOOT_STAGE_OK, 1, 9, VJO_BOOT_MAX_FAILURES, 9999, 1, VJO_BOOT_STAGE_OK, 1, 9};
    n = vjo_boot_format(&big, text, sizeof(text));
    TEST_CHECK(n > 0 && n < VJO_BOOT_RECORD_MAX - 1);
    TEST_CHECK(vjo_boot_format(&big, text, n) == -1);     /* no room for the NUL */
    TEST_CHECK(vjo_boot_format(&big, text, 8) == -1);
    /* out-of-range fields are clamped on the way out */
    VjoBootRecord odd = {5, 7, 42, -3, 99, 1000, -1, 2, 9, 5, 12};
    n = vjo_boot_format(&odd, text, sizeof(text));
    TEST_CHECK_(!strcmp(text, "vjoboot1 seq=5 mode=1 stage=4 inert=0 fault=9 failures=99 api=0 prev=1/4/1/9\n"), "%s", text);
}

static void test_parse_tolerates_damage_and_rejects_garbage(void)
{
    VjoBootRecord r;
    /* CRLF, trailing junk, unknown keys, a second line */
    const char *t = "vjoboot1 seq=3 mode=0 colour=blue stage=2 inert=0 fault=0 failures=1 api=17 prev=1/4/1/0 tail\r\nvjoboot1 seq=9 mode=1\n";
    TEST_CHECK(vjo_boot_parse(t, (int)strlen(t), &r) == 0);
    TEST_CHECK(r.seq == 3 && r.mode == 0 && r.stage == 2 && r.failures == 1 && r.api == 17);
    TEST_CHECK(r.prev_mode == 1 && r.prev_stage == 4 && r.prev_inert == 1 && r.prev_fault == 0);
    /* truncated anywhere after seq and mode: still a record, missing fields zero */
    t = "vjoboot1 seq=4 mode=1 stage=";
    TEST_CHECK(vjo_boot_parse(t, (int)strlen(t), &r) == 0 && r.seq == 4 && r.mode == 1 && r.stage == 0);
    t = "vjoboot1 seq=4 mode=1 prev=1/";
    TEST_CHECK(vjo_boot_parse(t, (int)strlen(t), &r) == 0 && r.prev_mode == 1 && r.prev_stage == 0);
    /* oversized numbers saturate at their ranges */
    t = "vjoboot1 seq=99999999999 mode=5 stage=77 inert=3 fault=12 failures=12345 api=123456789 prev=7/7/7/7";
    TEST_CHECK(vjo_boot_parse(t, (int)strlen(t), &r) == 0);
    TEST_CHECK(r.seq == VJO_BOOT_MAX_SEQ && r.mode == 1 && r.stage == VJO_BOOT_STAGE_OK && r.inert == 1 &&
               r.fault == 9 && r.failures == VJO_BOOT_MAX_FAILURES && r.api == 9999 && r.prev_stage == 4);
    /* not records: wrong magic, empty, no seq, seq 0, no mode, NULL, binary junk */
    const char *bad[] = {"", "vjoboot", "vjoboot2 seq=1 mode=0", "vjoboot1 mode=0", "vjoboot1 seq=0 mode=0",
                         "vjoboot1 seq=1", "vjoboot1 seq=x mode=0", "\x00\xff\xfe", "ok"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        r.seq = 77;
        TEST_CHECK_(vjo_boot_parse(bad[i], (int)strlen(bad[i]) + (i == 7 ? 3 : 0), &r) == -1, "%u", i);
        TEST_CHECK(r.seq == 0); /* zeroed */
    }
    TEST_CHECK(vjo_boot_parse(NULL, 0, &r) == -1);
    TEST_CHECK(vjo_boot_parse("vjoboot1 seq=1 mode=0", 3, &r) == -1); /* len respected */
}

static void test_decision_table(void)
{
    VjoBootRecord prev, next;
    /* no record: first boot, normal */
    vjo_boot_decide(NULL, 17, &next);
    TEST_CHECK(next.seq == 1 && next.mode == VJO_BOOT_NORMAL && next.stage == VJO_BOOT_STAGE_KERNEL &&
               next.failures == 0 && next.api == 17 && !next.inert && !next.fault);
    /* a complete normal boot: normal again, failures reset */
    memset(&prev, 0, sizeof(prev));
    prev.seq = 5; prev.mode = VJO_BOOT_NORMAL; prev.stage = VJO_BOOT_STAGE_OK; prev.failures = 1;
    vjo_boot_decide(&prev, 17, &next);
    TEST_CHECK(next.seq == 6 && next.mode == VJO_BOOT_NORMAL && next.failures == 0 &&
               next.prev_mode == VJO_BOOT_NORMAL && next.prev_stage == VJO_BOOT_STAGE_OK);
    /* an inert normal boot (RCO missing) counts as complete */
    prev.stage = VJO_BOOT_STAGE_SHELL; prev.inert = 1;
    vjo_boot_decide(&prev, 17, &next);
    TEST_CHECK(next.mode == VJO_BOOT_NORMAL && next.failures == 0 && next.prev_inert == 1);
    /* an incomplete normal boot at each stage: safe, failures + 1 */
    for (int stage = VJO_BOOT_STAGE_NONE; stage < VJO_BOOT_STAGE_OK; stage++) {
        prev.stage = stage; prev.inert = 0; prev.failures = 0;
        vjo_boot_decide(&prev, 17, &next);
        TEST_CHECK_(next.mode == VJO_BOOT_SAFE && next.failures == 1, "stage %d", stage);
    }
    /* a faulted normal boot that had reached ok: safe */
    prev.stage = VJO_BOOT_STAGE_OK; prev.fault = VJO_BOOT_FAULT_SHELL_EXIT;
    vjo_boot_decide(&prev, 17, &next);
    TEST_CHECK(next.mode == VJO_BOOT_SAFE && next.failures == 1 && next.prev_fault == VJO_BOOT_FAULT_SHELL_EXIT);
    /* failures saturate */
    prev.failures = VJO_BOOT_MAX_FAILURES;
    vjo_boot_decide(&prev, 17, &next);
    TEST_CHECK(next.failures == VJO_BOOT_MAX_FAILURES);
    /* a complete safe boot after one failure: one more normal try */
    memset(&prev, 0, sizeof(prev));
    prev.seq = 8; prev.mode = VJO_BOOT_SAFE; prev.stage = VJO_BOOT_STAGE_OK; prev.failures = 1;
    vjo_boot_decide(&prev, 17, &next);
    TEST_CHECK(next.mode == VJO_BOOT_NORMAL && next.failures == 1);
    prev.stage = VJO_BOOT_STAGE_KERNEL; prev.inert = 1; /* inert Shell completes it too */
    vjo_boot_decide(&prev, 17, &next);
    TEST_CHECK(next.mode == VJO_BOOT_NORMAL && next.failures == 1);
    /* a complete safe boot after two failures: safe stays (sticky) */
    prev.failures = VJO_BOOT_STICKY_FAILURES;
    vjo_boot_decide(&prev, 17, &next);
    TEST_CHECK(next.mode == VJO_BOOT_SAFE && next.failures == VJO_BOOT_STICKY_FAILURES);
    /* an incomplete safe boot (power cut within the minute): safe again, failures unchanged */
    prev.failures = 1; prev.inert = 0; prev.stage = VJO_BOOT_STAGE_KERNEL;
    vjo_boot_decide(&prev, 17, &next);
    TEST_CHECK(next.mode == VJO_BOOT_SAFE && next.failures == 1);
    /* seq wraps */
    prev.seq = VJO_BOOT_MAX_SEQ;
    vjo_boot_decide(&prev, 17, &next);
    TEST_CHECK(next.seq == 1);
    /* pure: same input, same output */
    VjoBootRecord again;
    vjo_boot_decide(&prev, 17, &again);
    TEST_CHECK(!memcmp(&again, &next, sizeof(next)));
    /* marks only move forward, and never past ok */
    VjoBootRecord r = {1, 0, VJO_BOOT_STAGE_SHELL, 0, 0, 0, 17, 0, 0, 0, 0};
    TEST_CHECK(!vjo_boot_mark(&r, VJO_BOOT_STAGE_KERNEL) && r.stage == VJO_BOOT_STAGE_SHELL);
    TEST_CHECK(!vjo_boot_mark(&r, VJO_BOOT_STAGE_SHELL));
    TEST_CHECK(vjo_boot_mark(&r, VJO_BOOT_STAGE_OK) && r.stage == VJO_BOOT_STAGE_OK);
    TEST_CHECK(!vjo_boot_mark(&r, VJO_BOOT_STAGE_OK + 1) && r.stage == VJO_BOOT_STAGE_OK);
    TEST_CHECK(!strcmp(vjo_boot_stage_name(VJO_BOOT_STAGE_WORKER), "worker") && !strcmp(vjo_boot_mode_name(1), "safe"));
}

/* ---------------- the kernel code, boot after boot ---------------- */

static void test_first_boot_writes_the_record_before_anything_else(void)
{
    reset_world();
    TEST_CHECK(boot_init(VJO_API_VERSION) == VJO_BOOT_NORMAL);
    TEST_CHECK(opens == 2 && writes == 1 && mkdirs == 2); /* read (ENOENT), then write */
    TEST_CHECK_(file_len > 0 && !strncmp(file, "vjoboot1 seq=1 mode=0 stage=1 inert=0 fault=0 failures=0 api=17 prev=0/0/0/0\n", (size_t)file_len), "%.*s", file_len, file);
    VjoBootInfo i = info();
    TEST_CHECK(i.size == sizeof(i) && i.seq == 1 && i.mode == VJO_BOOT_NORMAL && i.stage == VJO_BOOT_STAGE_KERNEL &&
               i.failures == 0 && i.write_status == 0);
    TEST_CHECK(!boot_shell_seen());
    TEST_CHECK(strstr(log_text, "boot 1 normal (previous: none") != NULL);
}

static void test_healthy_boots_stay_normal(void)
{
    reset_world();
    for (int b = 1; b <= 3; b++) {
        TEST_CHECK(reboot() == VJO_BOOT_NORMAL);
        shell_runs_fine();
        VjoBootInfo i = info();
        TEST_CHECK_(i.seq == (uint32_t)b && i.stage == VJO_BOOT_STAGE_OK && i.failures == 0, "boot %d", b);
        TEST_CHECK(boot_shell_seen());
        TEST_CHECK(strstr(file, "stage=4") != NULL); /* written back by the worker */
    }
    /* each mark is written once, nothing is rewritten while idle */
    unsigned before = writes;
    tick_for(10000000);
    TEST_CHECK(writes == before);
}

static void test_crash_before_ok_makes_the_next_boot_safe_then_one_retry(void)
{
    reset_world();
    TEST_CHECK(reboot() == VJO_BOOT_NORMAL);
    shell_runs_fine();
    /* boot 2: SceShell dies during the Shell plugin's start (#59's second boot) */
    TEST_CHECK(reboot() == VJO_BOOT_NORMAL);
    TEST_CHECK(boot_mark(VJO_BOOT_STAGE_SHELL) == 0);
    tick_for(500000);
    /* power cut: boot 3 is safe */
    TEST_CHECK(reboot() == VJO_BOOT_SAFE);
    VjoBootInfo i = info();
    TEST_CHECK(i.seq == 3 && i.failures == 1 && i.prev_mode == VJO_BOOT_NORMAL &&
               i.prev_stage == VJO_BOOT_STAGE_SHELL && !i.prev_inert && !i.prev_fault);
    TEST_CHECK(strstr(file, "mode=1") != NULL);
    /* the Shell unloads itself and says so */
    TEST_CHECK(boot_mark(0) == VJO_ERR_ARG); /* 0 is not a stage: the syscall maps it to inert */
    boot_mark_inert();
    TEST_CHECK(boot_shell_seen());
    tick_for(100000);
    TEST_CHECK(strstr(file, "inert=1") != NULL);
    /* boot 4: the retry, normal, failures still 1 */
    TEST_CHECK(reboot() == VJO_BOOT_NORMAL);
    i = info();
    TEST_CHECK(i.seq == 4 && i.failures == 1 && i.prev_mode == VJO_BOOT_SAFE && i.prev_inert);
    shell_runs_fine();
    /* boot 5: all forgiven */
    TEST_CHECK(reboot() == VJO_BOOT_NORMAL);
    TEST_CHECK(info().failures == 0);
}

static void test_two_failures_in_a_row_stick_to_safe_until_the_record_is_reset(void)
{
    reset_world();
    TEST_CHECK(reboot() == VJO_BOOT_NORMAL);
    boot_mark(VJO_BOOT_STAGE_SHELL);
    boot_mark(VJO_BOOT_STAGE_WORKER);
    tick_for(3000000);                     /* crash at 3 s */
    TEST_CHECK(reboot() == VJO_BOOT_SAFE); /* failures 1 */
    boot_mark_inert();
    tick_for(100000);
    TEST_CHECK(reboot() == VJO_BOOT_NORMAL); /* the retry */
    boot_mark(VJO_BOOT_STAGE_SHELL);
    tick_for(3000000);                       /* crashes again */
    TEST_CHECK(reboot() == VJO_BOOT_SAFE);
    TEST_CHECK(info().failures == 2);
    boot_mark_inert();
    tick_for(100000);
    for (int b = 0; b < 5; b++) {
        TEST_CHECK_(reboot() == VJO_BOOT_SAFE, "boot %d", b); /* sticky */
        TEST_CHECK(info().failures == 2);
        boot_mark_inert();
        tick_for(100000);
    }
    /* the installer empties the record (a new build), or the user deletes
     * it: a fresh start either way */
    file_len = 0;
    TEST_CHECK(reboot() == VJO_BOOT_NORMAL);
    TEST_CHECK(info().seq == 1 && info().failures == 0);
    TEST_CHECK(strstr(log_text, "empty record (reset by the installer)") != NULL);
    tick_for(1000000);
    TEST_CHECK(reboot() == VJO_BOOT_SAFE);
    boot_mark_inert();
    tick_for(100000);
    file_len = -1;
    TEST_CHECK(reboot() == VJO_BOOT_NORMAL);
    TEST_CHECK(info().seq == 1 && info().failures == 0);
}

static void test_safe_boot_completes_by_uptime_without_a_shell_plugin(void)
{
    reset_world();
    TEST_CHECK(reboot() == VJO_BOOT_NORMAL);
    tick_for(1000000);
    TEST_CHECK(reboot() == VJO_BOOT_SAFE);
    /* no Shell plugin reports in; the kernel completes the boot after a minute */
    tick_for(59000000);
    TEST_CHECK(info().stage == VJO_BOOT_STAGE_KERNEL);
    tick_for(2000000);
    TEST_CHECK(info().stage == VJO_BOOT_STAGE_OK);
    TEST_CHECK(strstr(file, "stage=4") != NULL);
    TEST_CHECK(reboot() == VJO_BOOT_NORMAL); /* one retry */
    /* a power cut inside the safe boot's minute: safe again, no extra failure */
    tick_for(1000000);
    TEST_CHECK(reboot() == VJO_BOOT_SAFE && info().failures == 2);
    tick_for(10000000);
    TEST_CHECK(reboot() == VJO_BOOT_SAFE && info().failures == 2);
}

static void test_shell_exit_after_ok_is_a_fault(void)
{
    reset_world();
    TEST_CHECK(reboot() == VJO_BOOT_NORMAL);
    shell_runs_fine();
    /* the game launch takes SceShell down (#59's first session) */
    boot_fault(VJO_BOOT_FAULT_SHELL_EXIT);
    tick_for(100000);
    TEST_CHECK(strstr(file, "fault=1") != NULL && strstr(file, "stage=4") != NULL);
    TEST_CHECK(reboot() == VJO_BOOT_SAFE);
    VjoBootInfo i = info();
    TEST_CHECK(i.prev_fault == VJO_BOOT_FAULT_SHELL_EXIT && i.prev_stage == VJO_BOOT_STAGE_OK && i.failures == 1);
}

static void test_ok_is_not_believed_early_and_marks_only_move_forward(void)
{
    reset_world();
    TEST_CHECK(reboot() == VJO_BOOT_NORMAL);
    TEST_CHECK(boot_mark(VJO_BOOT_STAGE_SHELL) == 0);
    TEST_CHECK(boot_mark(VJO_BOOT_STAGE_OK) == VJO_ERR_BUSY); /* 1 s of uptime */
    TEST_CHECK(info().stage == VJO_BOOT_STAGE_SHELL);
    tick_for(56000000);
    TEST_CHECK(boot_mark(VJO_BOOT_STAGE_OK) == 0);
    TEST_CHECK(boot_mark(VJO_BOOT_STAGE_WORKER) == 0 && info().stage == VJO_BOOT_STAGE_OK); /* no going back */
    TEST_CHECK(boot_mark(VJO_BOOT_STAGE_KERNEL) == VJO_ERR_ARG);
    TEST_CHECK(boot_mark(VJO_BOOT_STAGE_OK + 1) == VJO_ERR_ARG);
    /* before boot_init nothing is recorded */
    boot_test_reset();
    TEST_CHECK(boot_mark(VJO_BOOT_STAGE_SHELL) == VJO_ERR_BUSY);
    TEST_CHECK(boot_mode() == VJO_BOOT_NORMAL);
    VjoBootInfo i = info();
    TEST_CHECK(i.size == sizeof(i) && i.seq == 0);
}

static void test_io_failures_fail_open(void)
{
    /* the file cannot be opened at all: normal boot, failure logged */
    reset_world();
    open_fail_rc = (int)0x80010005;
    TEST_CHECK(boot_init(VJO_API_VERSION) == VJO_BOOT_NORMAL);
    TEST_CHECK(info().write_status == (int)0x80010005);
    TEST_CHECK(strstr(log_text, "no record (0x80010005)") != NULL);
    /* writes fail: still normal, the mode is still decided from the readable record */
    reset_world();
    TEST_CHECK(boot_init(VJO_API_VERSION) == VJO_BOOT_NORMAL);
    tick_for(1000000);
    write_fail_rc = (int)0x80010010;
    TEST_CHECK(reboot() == VJO_BOOT_SAFE); /* decided from the file */
    TEST_CHECK(info().write_status == (int)0x80010010);
    boot_mark_inert();
    tick_for(100000);
    TEST_CHECK(strstr(log_text, "write failed 0x80010010") != NULL);
    /* a short write is reported as such */
    reset_world();
    short_write = 1;
    TEST_CHECK(boot_init(VJO_API_VERSION) == VJO_BOOT_NORMAL);
    TEST_CHECK(info().write_status == VJO_ERR_COPY);
    /* garbage in the file: first boot */
    reset_world();
    file_len = (int)strlen("hello world\n");
    memcpy(file, "hello world\n", (size_t)file_len);
    TEST_CHECK(boot_init(VJO_API_VERSION) == VJO_BOOT_NORMAL);
    TEST_CHECK(info().seq == 1 && strstr(log_text, "unreadable record") != NULL);
    TEST_CHECK(!strncmp(file, "vjoboot1 seq=1 ", 15)); /* replaced */
    /* a record from a build with more fields: the known ones are used */
    reset_world();
    const char *future = "vjoboot1 seq=41 mode=0 stage=4 inert=0 fault=0 failures=0 api=99 prev=0/4/0/0 extra=1 more=2\n";
    file_len = (int)strlen(future);
    memcpy(file, future, (size_t)file_len);
    TEST_CHECK(boot_init(VJO_API_VERSION) == VJO_BOOT_NORMAL && info().seq == 42);
}

TEST_LIST = {
    {"record_format_and_parse_round_trip", test_format_and_parse_round_trip},
    {"record_parse_tolerates_damage_and_rejects_garbage", test_parse_tolerates_damage_and_rejects_garbage},
    {"decision_table", test_decision_table},
    {"kernel_first_boot_writes_the_record_before_anything_else", test_first_boot_writes_the_record_before_anything_else},
    {"kernel_healthy_boots_stay_normal", test_healthy_boots_stay_normal},
    {"kernel_crash_before_ok_makes_the_next_boot_safe_then_one_retry", test_crash_before_ok_makes_the_next_boot_safe_then_one_retry},
    {"kernel_two_failures_in_a_row_stick_to_safe_until_the_record_is_reset", test_two_failures_in_a_row_stick_to_safe_until_the_record_is_reset},
    {"kernel_safe_boot_completes_by_uptime_without_a_shell_plugin", test_safe_boot_completes_by_uptime_without_a_shell_plugin},
    {"kernel_shell_exit_after_ok_is_a_fault", test_shell_exit_after_ok_is_a_fault},
    {"kernel_ok_is_not_believed_early_and_marks_only_move_forward", test_ok_is_not_believed_early_and_marks_only_move_forward},
    {"kernel_io_failures_fail_open", test_io_failures_fail_open},
    {NULL, NULL}
};
