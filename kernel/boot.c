/* Boot-safety guard, kernel side (the decision itself is core/boot_guard.c).
 *
 * boot_init runs first thing in module_start, before any hook, thread or
 * memory block exists: it reads the previous boot's record from
 * ur0:data/VitaJPOverlay/boot.txt, decides this boot's mode and writes this
 * boot's record. ur0 (the internal eMMC) is the only storage that is final
 * this early: taiHEN itself reads ur0:tai/config.txt before it loads us,
 * while ux0 can still be remapped (SD2Vita). The Shell plugin then marks the
 * stages it reaches through vjoBootMark; the kernel worker thread writes each
 * change back from its own context, never from a syscall.
 *
 * Every I/O failure fails open: the boot continues in normal mode and the
 * failure is logged. The guard narrows the damage a bad build can do; the
 * hold-L skip of taiHEN stays the last resort. */
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/threadmgr.h>

#include "../core/boot_guard.h"
#include "vjo_kernel.h"

#define BOOT_DIR  "ur0:data/VitaJPOverlay"
#define BOOT_FILE BOOT_DIR "/boot.txt"
/* The Shell's "ok" mark is believed only after this much kernel uptime: the
 * Shell counts its own stable minute, the kernel checks it. */
#define BOOT_OK_MIN_US (55LL * 1000 * 1000)
/* In safe mode our plugins hold nothing, so the device staying up this long
 * completes the boot even if the Shell plugin never reported in. */
#define BOOT_SAFE_OK_US (60LL * 1000 * 1000)

static VjoBootRecord rec;
static int have_rec;
static volatile int dirty;
static int write_status;      /* the last write's result (0 ok) */
static int64_t start_us;
static int shell_seen;        /* the Shell plugin marked a stage this boot */

static int read_record(char *buf, int cap)
{
    SceUID fd = ksceIoOpen(BOOT_FILE, SCE_O_RDONLY, 0);
    int n;
    if (fd < 0)
        return fd;
    n = ksceIoRead(fd, buf, (SceSize)cap);
    ksceIoClose(fd);
    return n;
}

static int write_record(void)
{
    char buf[VJO_BOOT_RECORD_MAX];
    SceUID fd;
    int n = vjo_boot_format(&rec, buf, sizeof(buf)), w;
    if (n < 0)
        return VJO_ERR_FORMAT;
    ksceIoMkdir("ur0:data", 0777);
    ksceIoMkdir(BOOT_DIR, 0777);
    fd = ksceIoOpen(BOOT_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0)
        return fd;
    w = ksceIoWrite(fd, buf, (SceSize)n);
    ksceIoClose(fd);
    return w == n ? 0 : (w < 0 ? w : VJO_ERR_COPY);
}

int boot_init(int api)
{
    char buf[VJO_BOOT_RECORD_MAX + 64];
    VjoBootRecord prev;
    const VjoBootRecord *p = NULL;
    int n = read_record(buf, sizeof(buf) - 1);
    start_us = ksceKernelGetSystemTimeWide();
    if (n > 0) {
        buf[n] = 0;
        if (vjo_boot_parse(buf, n, &prev) == 0)
            p = &prev;
        else
            klog("boot guard: unreadable record (%d bytes): first boot", n);
    } else if (n == 0) {
        klog("boot guard: empty record (reset by the installer): first boot");
    } else if (n != (int)0x80010002) { /* ENOENT: a fresh install */
        klog("boot guard: no record (0x%08X): first boot", n);
    }
    vjo_boot_decide(p, api, &rec);
    have_rec = 1;
    write_status = write_record();
    klog("boot guard: boot %u %s (previous: %s, stage %s%s%s, failures %d) write 0x%08X",
         rec.seq, vjo_boot_mode_name(rec.mode),
         p ? vjo_boot_mode_name(rec.prev_mode) : "none", vjo_boot_stage_name(rec.prev_stage),
         rec.prev_inert ? ", inert" : "", rec.prev_fault ? ", fault" : "", rec.failures, write_status);
    return rec.mode;
}

int boot_mode(void)
{
    return have_rec ? rec.mode : VJO_BOOT_NORMAL;
}

int boot_shell_seen(void)
{
    return shell_seen;
}

/* The registered Shell marks a stage (vjoBootMark). Returns 0, or VJO_ERR_*
 * for a stage the kernel does not believe yet. */
int boot_mark(int stage)
{
    if (!have_rec)
        return VJO_ERR_BUSY;
    if (stage < VJO_BOOT_STAGE_SHELL || stage > VJO_BOOT_STAGE_OK)
        return VJO_ERR_ARG;
    shell_seen = 1;
    if (stage == VJO_BOOT_STAGE_OK && ksceKernelGetSystemTimeWide() - start_us < BOOT_OK_MIN_US)
        return VJO_ERR_BUSY;
    if (vjo_boot_mark(&rec, stage)) {
        klog("boot guard: stage %s", vjo_boot_stage_name(stage));
        dirty = 1;
    }
    return 0;
}

/* The Shell plugin unloaded itself on purpose (safe mode, missing RCO, API
 * mismatch): this boot cannot be ours to break. */
void boot_mark_inert(void)
{
    if (!have_rec)
        return;
    shell_seen = 1;
    if (!rec.inert) {
        rec.inert = 1;
        klog("boot guard: shell inert");
        dirty = 1;
    }
}

void boot_fault(int fault)
{
    if (!have_rec || rec.fault == fault)
        return;
    rec.fault = fault;
    klog("boot guard: fault %d", fault);
    dirty = 1;
}

void boot_info(VjoBootInfo *out)
{
    memset(out, 0, sizeof(*out));
    out->size = sizeof(*out);
    if (!have_rec)
        return;
    out->seq = rec.seq;
    out->mode = rec.mode;
    out->stage = rec.stage;
    out->inert = rec.inert;
    out->fault = rec.fault;
    out->failures = rec.failures;
    out->prev_mode = rec.prev_mode;
    out->prev_stage = rec.prev_stage;
    out->prev_inert = rec.prev_inert;
    out->prev_fault = rec.prev_fault;
    out->write_status = write_status;
}

/* Worker thread, each tick: completes a safe boot by uptime and writes
 * pending changes. One write per change, so a crash loses at most the
 * latest mark. */
void boot_tick(void)
{
    if (!have_rec)
        return;
    if (rec.mode == VJO_BOOT_SAFE && rec.stage < VJO_BOOT_STAGE_OK &&
        ksceKernelGetSystemTimeWide() - start_us >= BOOT_SAFE_OK_US) {
        vjo_boot_mark(&rec, VJO_BOOT_STAGE_OK);
        klog("boot guard: safe boot up for a minute: ok");
        dirty = 1;
    }
    if (dirty) {
        dirty = 0;
        write_status = write_record();
        if (write_status < 0)
            klog("boot guard: write failed 0x%08X", write_status);
    }
}

#ifdef VJO_HOST
/* The host test simulates a reboot: the kernel's statics start over, the
 * file (its stub) survives. */
void boot_test_reset(void)
{
    memset(&rec, 0, sizeof(rec));
    have_rec = dirty = write_status = shell_seen = 0;
    start_us = 0;
}
#endif
