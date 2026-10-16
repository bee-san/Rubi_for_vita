/* The Overlay app's install engine (docs/one-vpk-install.md): the overlay's files from the app's own
 * folder (its payload), the taiHEN configurations (core/taicfg.c), config.ini, and the records. The
 * scan reads only; a run writes, in the design's order, and stops at the first failure with the
 * card as the result says (every file old or new; a configuration missing only between the two
 * renames of its swap, which the next scan finds as "interrupted"). Nothing here restarts the Vita
 * or retries by itself. The Install page (settings/install_page.c) runs it on its own thread. */
#ifndef VJO_SETTINGS_INSTALL_H
#define VJO_SETTINGS_INSTALL_H
#include <stddef.h>
#include <stdint.h>
#include "../core/payload.h"
#include "../core/taicfg.h"

#define VJO_TAI_UX0 "ux0:tai/config.txt"
#define VJO_TAI_UR0 "ur0:tai/config.txt"

enum {
    VJO_INSTALL_INSTALLED = 0, /* this app's overlay, every file and line in place */
    VJO_INSTALL_NOT_INSTALLED, /* no configuration loads the overlay */
    VJO_INSTALL_UPDATE,        /* another build's overlay (installed.txt, the plugins' record, a plugin) */
    VJO_INSTALL_REPAIR,        /* this build, but a file or a line is missing */
    VJO_INSTALL_CANNOT,        /* Unsafe Homebrew off, a damaged package, a configuration the editor refuses */
    VJO_INSTALL_NO_PAYLOAD,    /* the zip's app: no overlay files in it (install-vita.sh installs them) */
    VJO_INSTALL_INTERRUPTED    /* a config.txt.vjo-new or .vjo-old left by a stopped swap, beside a config.txt or none */
};
/* FINISH puts a stopped swap's file back (the current config.txt, if any, kept as a dated backup); KEEP
 * keeps the current config.txt and the leftovers as dated backups. Neither removes a file (C4). */
enum { VJO_INSTALL_ACT_INSTALL = 1, VJO_INSTALL_ACT_UNINSTALL = 2, VJO_INSTALL_ACT_FINISH = 4, VJO_INSTALL_ACT_KEEP = 8 };
/* What a config.txt beside a leftover is now. */
enum { VJO_TAI_NOW_OTHER = 0, VJO_TAI_NOW_STOCK = 1, VJO_TAI_NOW_OURS = 2 };

#define VJO_INSTALL_LINES 40
#define VJO_INSTALL_DIFF 8 /* exact config.txt lines a plan shows per file */
#define VJO_INSTALL_INI_CHANGES 12 /* config.ini changes a plan lists */
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
    int use_new;               /* FINISH would put .vjo-new back: the editor's result (an install's or an uninstall's)
                                * of .vjo-old, or, with no .vjo-old (the swap never started), of the current
                                * config.txt byte for byte; else .vjo-old */
    int finish_ok;             /* FINISH has a whole file to put back: use_new, or a .vjo-old */
    int refused;               /* the editor refuses an install (why) */
    char why[224];
    int install_changes, uninstall_changes;
    VjoTaiReport install;      /* the install's report (what it adds, removes, comments out) */
    int has_kernel;            /* a kernel line of ours that is not a comment */
    int loads_ours;            /* taiHEN loads the kernel plugin and the Shell from it now */
    uint8_t sha256[32];        /* its text's, so that a run knows the plan is still the card's */
    /* the exact lines an install (0) or an uninstall (1) adds (+) or removes (-), in file order
     * (AGENTS.md rule 7: the plan shows the exact edits); more than VJO_INSTALL_DIFF are counted */
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
    int config_ini;            /* config.ini: 0 kept as it is, 1 patched (lines listed), 2 written from the template */
    char config_changes[VJO_INSTALL_INI_CHANGES][120]; /* config.ini's changed lines, for the plan */
    int config_change_count;
    char stamp[20];            /* the backups' date: YYYYMMDD-HHMMSS (local time) */
    char before_overlay[2][128];/* each folder's newest backup without our kernel line ("" none) */
    char storage[40];          /* a storage plugin in ur0's *KERNEL: taiHEN reads ur0's at boot */
} VjoInstallScan;

/* Reads the card into s (writes nothing). */
void vjo_install_scan(VjoInstallScan *s, const VjoInstallBuild *build);

/* The plan of action, one entry per line (the page wraps them): their count. */
int vjo_install_plan(const VjoInstallScan *s, int action, char lines[][VJO_INSTALL_LINE], int max);

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
