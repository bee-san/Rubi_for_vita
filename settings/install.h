/* The Rubi app's install engine (docs/one-vpk-install.md): the overlay's files from the app's own
 * folder (its payload), the taiHEN configurations (core/taicfg.c), config.ini, and the records. The
 * scan reads only; a run writes, in the design's order, and stops at the first failure with the
 * card as the result says (every file old or new; a configuration missing only between the two
 * renames of its swap, which the next scan finds as "interrupted"). Nothing here restarts the Vita
 * or retries by itself. The Install page (settings/install_page.c) runs it on its own thread. */
#ifndef VJO_SETTINGS_INSTALL_H
#define VJO_SETTINGS_INSTALL_H
#include <stddef.h>
#include <stdint.h>
#include "../core/boot_guard.h"
#include "../core/payload.h"
#include "../core/taicfg.h"

#define VJO_TAI_UX0 "ux0:tai/config.txt"
#define VJO_TAI_UR0 "ur0:tai/config.txt"

enum {
    VJO_INSTALL_INSTALLED = 0, /* this app's overlay, every file and line in place */
    VJO_INSTALL_NOT_INSTALLED, /* no configuration loads the overlay */
    VJO_INSTALL_UPDATE,        /* another build's overlay, or one with no installed.txt */
    VJO_INSTALL_REPAIR,        /* this build (installed.txt), but a file, a line or the plugins' record is not its own */
    VJO_INSTALL_CANNOT,        /* Unsafe Homebrew off, a damaged package, a configuration the editor refuses */
    VJO_INSTALL_NO_PAYLOAD,    /* the zip's app: no overlay files in it (install-vita.sh installs them) */
    VJO_INSTALL_INTERRUPTED    /* a config.txt.vjo-new or .vjo-old left by a stopped swap, beside a config.txt or none */
};
/* FINISH puts a stopped swap's file back (the current config.txt, if any, kept as a dated backup); KEEP
 * keeps the current config.txt and the leftovers as dated backups. Neither removes a file (C4). */
enum { VJO_INSTALL_ACT_INSTALL = 1, VJO_INSTALL_ACT_UNINSTALL = 2, VJO_INSTALL_ACT_FINISH = 4, VJO_INSTALL_ACT_KEEP = 8 };
/* What a config.txt beside a leftover is now. */
enum { VJO_TAI_NOW_OTHER = 0, VJO_TAI_NOW_STOCK = 1, VJO_TAI_NOW_OURS = 2 };

/* A plan has at most VJO_INSTALL_LINES lines: a longer one shows fewer of its exact config.txt and
 * config.ini lines (the rest counted), and its closing lines (the restart and the recovery) always fit. */
#define VJO_INSTALL_LINES 40
#define VJO_INSTALL_DIFF 8 /* exact config.txt lines a plan shows per file (fewer when it is long) */
#define VJO_INSTALL_INI_CHANGES 12 /* config.ini changes a plan lists (fewer when it is long) */
#define VJO_INSTALL_LINE 320

/* What the app is (core/payload.h): the page passes its compiled values; the host tests vary them. */
typedef VjoPayloadBuild VjoInstallBuild;

typedef struct {
    const char *path;          /* VJO_TAI_UX0 or VJO_TAI_UR0 */
    int exists;
    int interrupted;           /* missing, with a .vjo-new (2) or only a .vjo-old (1) beside it */
    int leftover;              /* exists, with a .vjo-old (1) and/or a .vjo-new (2) beside it */
    int swap_files;            /* a .vjo-old (1) and/or a .vjo-new (2) beside it, whether config.txt exists or not */
    int now_kind;              /* VJO_TAI_NOW_*: HENkaku's default (h-encore rewrites it when both are missing) */
    int use_new;               /* FINISH would put .vjo-new back: the editor's result (an install's or an uninstall's,
                                * one that changes the file) of .vjo-old, or, with no .vjo-old (the swap never
                                * started), of the current config.txt byte for byte; else .vjo-old */
    int finish_ok;             /* FINISH has a whole file to put back: use_new, or a .vjo-old */
    int stopped_run;           /* the run whose .vjo-new it is (VJO_INSTALL_ACT_INSTALL or _UNINSTALL), 0 when it
                                * cannot be told (an empty file, or a start both runs would write) */
    int refused;               /* the editor refuses an install (why) */
    char why[224];
    int install_changes, uninstall_changes;
    VjoTaiReport install;      /* the install's report (what it adds, removes, comments out) */
    int has_kernel;            /* a kernel line of ours that is not a comment */
    int loads_ours;            /* taiHEN loads the kernel plugin and the Shell from it now */
    uint8_t sha256[32];        /* its text's, so that a run knows the plan is still the card's */
    /* the exact lines an install (0) or an uninstall (1) adds (+) or removes (-), in file order: a minimal
     * line diff (vjo_install_diff; AGENTS.md rule 7: the plan shows the exact edits); more than
     * VJO_INSTALL_DIFF are counted */
    char diff[2][VJO_INSTALL_DIFF][VJO_TAI_LINE_MAX + 4];
    int diff_count[2], diff_more[2];
} VjoInstallConfig;

typedef struct {
    VjoInstallBuild build;
    int state, actions;
    char title[128];
    char reason[512];          /* cannot, no payload, interrupted: why and what to do */
    int unsafe_off;            /* Enable Unsafe Homebrew is off */
    int has_payload;
    VjoPayload payload;
    int has_installed;         /* installed.txt read */
    VjoInstalled installed;
    VjoInstallConfig cfg[2];   /* ux0's, ur0's */
    unsigned char file_ok[VJO_PAYLOAD_COUNT]; /* plugins: size and SHA-256; data: size */
    int files_ok, plugins_ok;
    int dict_in_app;           /* the dictionary is still in the app folder */
    int dict_index;            /* its payload index */
    int record_ok;             /* the plugins' record (vjo_app.txt) is this app's stamp */
    char own_stamp[64];
    int config_ini;            /* config.ini: 0 kept as it is, 1 patched (lines listed), 2 written from the template,
                                * 3 written from its backup config.ini.bak (missing beside it; over this build) */
    char config_changes[VJO_INSTALL_INI_CHANGES][120]; /* config.ini's changed lines, for the plan */
    int config_change_count;
    int config_change_more;    /* changed lines past VJO_INSTALL_INI_CHANGES (counted, not listed) */
    char stamp[20];            /* the backups' date: YYYYMMDD-HHMMSS (local time) */
    char before_overlay[2][128];/* each folder's newest backup without our kernel line ("" none) */
    char storage[40];          /* a storage plugin in ur0's *KERNEL: taiHEN reads ur0's at boot */
    /* #528: this boot's record of the boot guard (ur0:data/VitaJPOverlay/boot.txt, core/boot_guard.h). safe_mode: it
     * says safe, so the overlay, its hooks and OCR are off this boot; boot: the record (the failed normal boots in a
     * row, the previous boot's stage and fault). From VJO_BOOT_STICKY_FAILURES on, safe mode stays until an install
     * or Check and repair resets the record (vjo_install_run); below it, the next restart tries the overlay again. */
    int safe_mode;
    VjoBootRecord boot;
} VjoInstallScan;

/* Reads the card into s (writes nothing). */
void vjo_install_scan(VjoInstallScan *s, const VjoInstallBuild *build);

/* #528: what the page says while the boot guard holds the overlay in safe mode (s->safe_mode): why, and what ends
 * it. Its length, 0 when s is not in safe mode. */
int vjo_install_safe_note(const VjoInstallScan *s, char *out, size_t cap);

/* The name the page gives the action that installs in this state (Install, Update, Repair, Check and repair): the
 * page's badge and the safe-mode note's (audit-mem-crash-2 F4: one mapping). */
const char *vjo_install_action_name(int state);

/* The plan of action, one entry per line (the page wraps them): their count (at most max). */
int vjo_install_plan(const VjoInstallScan *s, int action, char lines[][VJO_INSTALL_LINE], int max);

/* The lines that make old (old_len bytes) new, in file order, each given to emit with its sign ('-' a
 * line of old, '+' one of new; its text without its line end: CRLF, CR or LF): a minimal line diff,
 * the fewest such lines. Of the minimal ones it keeps the most lines that do not name the overlay,
 * then shows a removal before an addition. A changed stretch of more than 1,024 lines, or 256 changed
 * lines or more, gets a greedy walk instead (still the edit, not always the shortest). The plan's
 * exact lines and log.txt's (code audit note 1). */
void vjo_install_diff(const char *old, int old_len, const char *neu, int new_len,
                      void (*emit)(void *ctx, char sign, const char *s, int n), void *ctx);

typedef struct {
    /* step: what is being done; done/total: bytes of the step (0/0 when it has no size) */
    void (*progress)(void *ud, const char *step, uint64_t done, uint64_t total);
    void *ud;
} VjoInstallUi;

typedef struct {
    int rc;                    /* 0 done, < 0 stopped */
    int written;               /* anything was changed on the card */
    char lines[VJO_INSTALL_LINES][VJO_INSTALL_LINE];
    int count;
} VjoInstallResult;

/* Runs action for s (a fresh scan of the same card: it scans again and stops when the state moved). */
int vjo_install_run(VjoInstallScan *s, int action, const VjoInstallUi *ui, VjoInstallResult *r);

/* The recovery text, the same on the page and in the README (docs/one-vpk-install.md, "Recovery"). */
#define VJO_INSTALL_RECOVERY_BOOT                                                                          \
    "If the Vita does not start: hold L while you turn it on (h-encore or h-encore2: open its menu, "     \
    "holding R as h-encore starts, and choose Exit while holding L), then in VitaShell put # before the " \
    "two VitaJPOverlay lines of ur0:tai/config.txt and ux0:tai/config.txt, or choose Uninstall here."
#define VJO_INSTALL_RECOVERY_GAME "If a game does not start: turn it OFF on the Games page, or hold L while you start it."
#endif
