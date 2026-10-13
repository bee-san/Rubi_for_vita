/* Boot-safety guard, SceShell side. The kernel decided this boot's mode
 * before SceShell existed (kernel/boot.c); here the plugin asks for it,
 * unloads itself in safe mode with a note in status.txt, and reports the
 * stages it reaches so the next boot knows how far this one got. Every call
 * is a weak syscall import: an older kernel without them means normal mode
 * and no marks, which vjo_worker_start's version check then refuses.
 *
 * Only the constants of core/boot_guard.h are used here: the record's
 * parser and formatter stay out of the Shell module (every page of it is
 * SceShell's). */
#include <psp2/kernel/clib.h>

#include "../core/boot_guard.h"
#include "../include/vjo_import.h"
#include "shell.h"

#ifdef VJO_HOST
#define IMPORT_READY(fn) 1
#else
#define IMPORT_READY(fn) VJO_IMPORT_READY(fn)
#endif

static const char *stage_name(int stage)
{
    static const char *const names[] = {"none", "kernel", "shell", "worker", "ok"};
    return stage >= 0 && stage <= VJO_BOOT_STAGE_OK ? names[stage] : "?";
}

static const char *mode_name(int mode)
{
    return mode == VJO_BOOT_SAFE ? "safe" : "normal";
}

static int info_ready(VjoBootInfo *info)
{
    sceClibMemset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    if (!IMPORT_READY(vjoBootInfo))
        return 0;
    return vjoBootInfo(info) == 0 && info->size == sizeof(*info);
}

int vjo_guard_mark(int stage)
{
    int rc;
    if (!IMPORT_READY(vjoBootMark))
        return -1;
    rc = vjoBootMark(stage);
    if (rc < 0)
        vjo_log("boot guard: mark %s refused 0x%08X", stage_name(stage), rc);
    return rc;
}

/* The note a safe boot leaves in status.txt (tools/install_ftp.sh --status). */
static void safe_mode_note(const VjoBootInfo *i)
{
    vjo_log("BOOT GUARD: SAFE MODE, boot %u. No hooks, no overlay, no OCR this boot.", (unsigned)i->seq);
    vjo_log("boot guard: boot %u (%s) reached stage %s%s, %d failed normal boot%s in a row.",
            (unsigned)i->seq - 1, mode_name(i->prev_mode), stage_name(i->prev_stage),
            i->prev_fault ? ", then SceShell exited" : " and never reached ok",
            i->failures, i->failures == 1 ? "" : "s");
    if (i->failures < VJO_BOOT_STICKY_FAILURES)
        vjo_log("boot guard: reboot to try the overlay once more; a second failure keeps safe mode.");
    else
        vjo_log("boot guard: safe mode stays until a new install, or ur0:data/VitaJPOverlay/boot.txt is deleted.");
    vjo_log("boot guard: send status.txt and kernel.txt (install_ftp.sh --status). No boot at all: hold L at power-on, then install_ftp.sh --uninstall.");
    if (i->write_status)
        vjo_log("boot guard: boot.txt not writable (0x%08X): this boot cannot be remembered.", i->write_status);
}

int vjo_guard_safe_mode(void)
{
    VjoBootInfo info;
    if (!info_ready(&info)) {
        vjo_log("boot guard: no kernel record (old kernel plugin?)");
        return 0;
    }
    if (info.mode != VJO_BOOT_SAFE) {
        vjo_log("boot guard: boot %u normal (previous %s/%s%s%s, failures %d, write 0x%08X)",
                (unsigned)info.seq, mode_name(info.prev_mode), stage_name(info.prev_stage),
                info.prev_inert ? " inert" : "", info.prev_fault ? " fault" : "", info.failures,
                info.write_status);
        return 0;
    }
    safe_mode_note(&info);
    vjo_guard_mark(0); /* inert: this boot is complete as far as we are concerned */
    return 1;
}
