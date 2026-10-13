/* Boot-safety guard: the record kept in ur0:data/VitaJPOverlay/boot.txt and
 * the decision that turns a failed boot into a safe one. Plain C with no SDK
 * dependency, so the host tests run the same code as the kernel plugin.
 *
 * One record describes one boot. The kernel plugin reads the previous boot's
 * record first thing in module_start, decides this boot's mode, and writes
 * the new record before it installs anything. The Shell plugin marks the
 * stages it reaches; the kernel worker thread writes each change back.
 *
 *   mode     normal: hooks and the Shell plugin run; safe: the kernel installs
 *            no hooks and the Shell and game plugins unload themselves at once
 *   stage    the furthest point this boot reached (monotonic)
 *   inert    the Shell plugin unloaded itself on purpose (safe mode, a missing
 *            RCO, a kernel API mismatch): a complete boot for the guard
 *   fault    the kernel saw SceShell exit while our plugin was loaded in it
 *   failures normal boots in a row that did not complete
 *
 * A boot is complete when it reached VJO_BOOT_STAGE_OK or was inert, and no
 * fault was seen. After an incomplete normal boot the next boot is safe. A
 * complete safe boot is followed by one more normal attempt while fewer than
 * VJO_BOOT_STICKY_FAILURES normal boots failed in a row; from then on safe
 * mode stays until the record is reset (the installer deletes it; so does
 * removing the file by hand). */
#ifndef VJO_BOOT_GUARD_H
#define VJO_BOOT_GUARD_H

#include <stdint.h>

#define VJO_BOOT_NORMAL 0
#define VJO_BOOT_SAFE   1

/* Stages in order; vjo_boot_mark keeps the furthest one. */
#define VJO_BOOT_STAGE_NONE   0
#define VJO_BOOT_STAGE_KERNEL 1 /* the kernel plugin's module_start ran */
#define VJO_BOOT_STAGE_SHELL  2 /* the Shell plugin's module_start ran in SceShell */
#define VJO_BOOT_STAGE_WORKER 3 /* the Shell plugin loaded its Paf plugin and started its threads */
#define VJO_BOOT_STAGE_OK     4 /* the Shell plugin's control thread was still alive a minute later */

/* Faults the kernel can observe. */
#define VJO_BOOT_FAULT_NONE       0
#define VJO_BOOT_FAULT_SHELL_EXIT 1 /* the registered SceShell process exited */

#define VJO_BOOT_STICKY_FAILURES 2
#define VJO_BOOT_MAX_FAILURES    99
#define VJO_BOOT_MAX_SEQ         999999u

/* Longest record vjo_boot_format writes, including the newline and NUL. */
#define VJO_BOOT_RECORD_MAX 160

typedef struct {
    uint32_t seq;       /* boot number, 1-based, wraps at VJO_BOOT_MAX_SEQ */
    int mode;           /* VJO_BOOT_* of this boot */
    int stage;          /* VJO_BOOT_STAGE_* reached in this boot */
    int inert;          /* 1: the Shell plugin unloaded itself (see above) */
    int fault;          /* VJO_BOOT_FAULT_* seen in this boot */
    int failures;       /* consecutive incomplete normal boots, this one excluded */
    int api;            /* VJO_API_VERSION of the plugins that wrote it */
    int prev_mode;      /* the previous boot, as read when this one started */
    int prev_stage;
    int prev_inert;
    int prev_fault;
} VjoBootRecord;

/* 1 if record r describes a boot that completed (reached OK or was inert,
 * without a fault). */
int vjo_boot_complete(const VjoBootRecord *r);

/* The mode of the boot that follows prev (NULL: no readable record, i.e.
 * the first boot after an install or a reset), written into next with
 * stage KERNEL. Pure: the same inputs always give the same record. */
void vjo_boot_decide(const VjoBootRecord *prev, int api, VjoBootRecord *next);

/* Records stage if it is further than the one recorded. Returns 1 if the
 * record changed. */
int vjo_boot_mark(VjoBootRecord *r, int stage);

/* One line, "vjoboot1 seq=.. mode=.. stage=.. inert=.. fault=.. failures=..
 * api=.. prev=mode/stage/inert/fault\n", NUL-terminated. Returns its length
 * without the NUL, or -1 if cap is too small (nothing written). */
int vjo_boot_format(const VjoBootRecord *r, char *out, int cap);

/* Parses what vjo_boot_format wrote (CRLF and trailing junk tolerated,
 * unknown keys ignored, values clamped to their ranges). Returns 0 and fills
 * out, or -1 for anything that is not a record (wrong magic, missing seq or
 * mode): the caller then treats the boot as the first one. */
int vjo_boot_parse(const char *text, int len, VjoBootRecord *out);

/* Names for logs and status.txt. */
const char *vjo_boot_mode_name(int mode);
const char *vjo_boot_stage_name(int stage);

#endif
