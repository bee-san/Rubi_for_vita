/* The one-VPK install's payload (docs/one-vpk-install.md, "Inside the VPK"): the files the Rubi
 * app installs from its own folder, compiled in. The app accepts a payload manifest only when its
 * file set is exactly this list (core/payload.c), so a damaged or edited manifest cannot name any
 * other file (games.ini, boot.txt, config.txt, another plugin). tools/package_release.py writes the
 * manifest from the same list (PAYLOAD_FILES); tools/tests/test_one_vpk.py checks the two agree.
 *
 * In the VPK each file sits under payload/<device>/<path>, as in the release zip
 * (payload/ur0/tai/VitaJPOverlay_Kernel.skprx); installed as ux0:app/VJOSET001/payload/... */
#ifndef VJO_PAYLOAD_H
#define VJO_PAYLOAD_H
#include "vjo_vocr_sizes.h"

#define VJO_PAYLOAD_APP_DIR "ux0:app/VJOSET001"          /* the app's own folder, as VitaShell installs it */
#define VJO_PAYLOAD_DIR VJO_PAYLOAD_APP_DIR "/payload"
#define VJO_PAYLOAD_MANIFEST VJO_PAYLOAD_DIR "/manifest.txt"
#define VJO_PAYLOAD_MANIFEST_MAX 8192                    /* 64 lines of at most 255 bytes, and more */
#define VJO_PAYLOAD_LINE_MAX 255
#define VJO_PAYLOAD_LINES_MAX 64
/* The record of the build installed (docs/one-vpk-install.md, "installed.txt"): written by the app's
 * Install and by install-vita.sh, read by the app's Install page. */
#define VJO_INSTALLED_RECORD "ux0:data/VitaJPOverlay/installed.txt"

enum { VJO_PAYLOAD_COPY = 0, VJO_PAYLOAD_MOVE = 1, VJO_PAYLOAD_TEMPLATE = 2 };

/* The destinations, in install order: the data first (the models, the dictionary), the plugins and
 * their RCO last (the shortest time with a new plugin beside an old one, which fails closed), then the
 * config template (written only when there is no config.ini). */
#define VJO_PAYLOAD_DATA "ux0:data/VitaJPOverlay/"
#define VJO_PAYLOAD_OCR VJO_PAYLOAD_DATA "ocr/"
#define VJO_PAYLOAD_FILE_LIST(X)                                                        \
    X(VJO_PAYLOAD_OCR VJO_VOCR_TIER1_NAME, VJO_PAYLOAD_COPY)                            \
    X(VJO_PAYLOAD_OCR VJO_VOCR_TIER2_NAME, VJO_PAYLOAD_COPY)                            \
    X(VJO_PAYLOAD_OCR VJO_VOCR_TIER3_NAME, VJO_PAYLOAD_COPY)                            \
    X(VJO_PAYLOAD_OCR VJO_VOCR_LM1_NAME, VJO_PAYLOAD_COPY)                              \
    X(VJO_PAYLOAD_OCR VJO_VOCR_LM2_NAME, VJO_PAYLOAD_COPY)                              \
    X(VJO_PAYLOAD_OCR VJO_VOCR_LM3_NAME, VJO_PAYLOAD_COPY)                              \
    X(VJO_PAYLOAD_OCR VJO_VOCR_LM4_NAME, VJO_PAYLOAD_COPY)                              \
    X(VJO_PAYLOAD_OCR "LM-ATTRIBUTION.md", VJO_PAYLOAD_COPY)                            \
    X(VJO_PAYLOAD_OCR "MODEL-ATTRIBUTION.md", VJO_PAYLOAD_COPY)                         \
    X(VJO_PAYLOAD_OCR VJO_VOCR_GATE_NAME, VJO_PAYLOAD_COPY)                             \
    X(VJO_PAYLOAD_OCR "GATE-ATTRIBUTION.md", VJO_PAYLOAD_COPY)                          \
    X(VJO_PAYLOAD_OCR VJO_VOCR_SPOT_NAME, VJO_PAYLOAD_COPY)                             \
    X(VJO_PAYLOAD_OCR "TEXTSPOT-ATTRIBUTION.md", VJO_PAYLOAD_COPY)                      \
    X(VJO_PAYLOAD_DATA "dictionaries/plaintext.vjdict", VJO_PAYLOAD_MOVE)               \
    X("ur0:tai/VitaJPOverlay_Kernel.skprx", VJO_PAYLOAD_COPY)                           \
    X("ur0:tai/VitaJPOverlay_Shell.suprx", VJO_PAYLOAD_COPY)                            \
    X("ur0:tai/VitaJPOverlay_OCR.suprx", VJO_PAYLOAD_COPY)                              \
    X("ur0:data/VitaJPOverlay/vitajpoverlay.rco", VJO_PAYLOAD_COPY)                     \
    X(VJO_PAYLOAD_DATA "config.ini", VJO_PAYLOAD_TEMPLATE)

#define VJO_PAYLOAD_COUNT_ONE(dest, kind) +1
enum { VJO_PAYLOAD_COUNT = 0 VJO_PAYLOAD_FILE_LIST(VJO_PAYLOAD_COUNT_ONE) };
#define VJO_PAYLOAD_KERNEL_INDEX 14 /* ur0:tai/VitaJPOverlay_Kernel.skprx: the first plugin */
#define VJO_PAYLOAD_PLUGINS 4      /* the three plugins and the RCO, hashed at every start */

/* The package's own settings (package_release.py --set, a test build's anki_mining = on) a payload
 * may carry, each "on" or "off". */
#define VJO_PAYLOAD_SET_KEYS {"anki_mining", "log_file"}
#define VJO_PAYLOAD_SETS_MAX 4
#endif
