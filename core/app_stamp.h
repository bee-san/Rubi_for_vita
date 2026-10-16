/* The Overlay app's version stamp: which app the overlay plugins came with,
 * and which app is installed (#101: test.1's app ran beside test.2's plugins).
 *
 * One line, written at build time by tools/app_stamp.py from the app's
 * eboot.bin and include/vjo_api.h:
 *
 *     vjoapp1 api=21 build=0123456789abcdef
 *
 * api is VJO_API_VERSION of the tree the app was built from, build the first
 * 16 hex digits of the SHA-256 of its eboot.bin. The same bytes go into the
 * app's VPK as vjo_app.txt (installed as ux0:app/VJOSET001/vjo_app.txt, the
 * app's own app0:vjo_app.txt) and into the release as
 * ux0:data/VitaJPOverlay/vjo_app.txt, which the installer uploads with the
 * plugins: the record of the app those plugins came with. The installer
 * uploads that app's VPK to ux0:data/JPOverlaySettings.vpk at the same time,
 * so installing that VPK always makes the two match.
 *
 * Nothing here allocates; the Shell reads both files into stack buffers at a
 * game start (shell/worker.c check_app). */
#ifndef VJO_APP_STAMP_H
#define VJO_APP_STAMP_H

#define VJO_APP_STAMP_INSTALLED "ux0:app/VJOSET001/vjo_app.txt" /* the installed app's */
#define VJO_APP_EBOOT_INSTALLED "ux0:app/VJOSET001/eboot.bin"   /* the installed app itself */
#define VJO_APP_STAMP_PACKAGE "ux0:data/VitaJPOverlay/vjo_app.txt" /* the plugins' record */
#define VJO_APP_STAMP_OWN "app0:vjo_app.txt" /* the running app's own, in the app */
#define VJO_APP_STAMP_MAX 64 /* a stamp's line and its NUL fit */

/* What the plugins' in-game notice says when the installed app is not the
 * one they came with: the VPK the installer put beside the record; or, for an
 * app of a later API than the plugins, the newer package's installer. */
#define VJO_APP_NOTICE "Overlay app is out of date: reinstall ux0:data/JPOverlaySettings.vpk in VitaShell."
#define VJO_APP_NOTICE_NEWER "Overlay app is newer than the overlay: run install-vita.sh from its package, then reboot."

enum {
    VJO_APP_OK = 0,       /* the installed app's stamp is the plugins' record */
    VJO_APP_UNKNOWN = 1,  /* no record, no app installed, or a file that cannot be read */
    VJO_APP_MISMATCH = 2, /* another stamp, or an app without one (from before the stamps) */
};

/* The installed app against the plugins' record. installed_rc is the result
 * of reading the installed app's stamp (>= 0, or the SDK's error: 0x80010002
 * when it is not there), installed its first line; app_present: the app's
 * eboot.bin is there; package_rc and package the same for the record. Texts
 * are NUL-terminated and only read when their rc is >= 0. An app without a
 * stamp is a mismatch; no app at all, or no record, is unknown: the README
 * lets the app be deleted, and an older package brings no record. */
int vjo_app_state(int installed_rc, const char *installed, int app_present, int package_rc, const char *package);

/* api= of a stamp line, or -1 when the line is not a stamp. */
int vjo_app_stamp_api(const char *line);

#endif
