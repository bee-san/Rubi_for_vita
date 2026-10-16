/* The one-VPK install's payload manifest and the installed build's record (docs/one-vpk-install.md):
 * pure functions over bounded texts, shared by the Overlay app (settings/install.c) and the host
 * tests; tools/package_release.py writes the manifest and tools/install_ftp.sh the record with the
 * same rules (tools/tests/test_one_vpk.py runs both).
 *
 * payload/manifest.txt, LF line ends, printable ASCII, at most 64 lines of 255 bytes:
 *
 *     vjopayload 1
 *     version 0.8.0              the package's (a test build's: 0.8.0-test.4)
 *     app_version 0.8.0          the Overlay app's own (vita/CMakeLists.txt VJO_VERSION)
 *     variant normal             normal | debug (docs/release-variants.md)
 *     api 24                     VJO_API_VERSION
 *     worker kernel              kernel | lines (where the game OCR worker comes from)
 *     build 0123456789abcdef     the Overlay app's eboot.bin stamp (core/app_stamp.h)
 *     file DEST BYTES SHA256 copy|move
 *     template ux0:data/VitaJPOverlay/config.ini BYTES SHA256
 *     set anki_mining on
 *
 * installed.txt, one line:
 *
 *     vjoinst1 version=0.8.0 variant=normal api=24 build=0123456789abcdef manifest=<sha256 of manifest.txt> */
#ifndef VJO_PAYLOAD_PARSE_H
#define VJO_PAYLOAD_PARSE_H
#include <stddef.h>
#include <stdint.h>
#include "../include/vjo_payload.h"

typedef struct {
    const char *dest; /* the compiled list's (include/vjo_payload.h) */
    int kind;         /* VJO_PAYLOAD_COPY, _MOVE, _TEMPLATE */
    uint64_t bytes;
    uint8_t sha256[32];
} VjoPayloadFile;

/* What the app was built as: a payload must be exactly its own build's. */
typedef struct {
    int api;
    const char *app_version;
    const char *variant; /* "normal" or "debug" */
    int worker;          /* VJO_TAI_WORKER_KERNEL or VJO_TAI_WORKER_LINES (core/taicfg.h) */
} VjoPayloadBuild;

typedef struct {
    char version[40], app_version[40], variant[8], build[17];
    int api, worker;
    VjoPayloadFile file[VJO_PAYLOAD_COUNT];
    struct { char key[16], value[4]; } set[VJO_PAYLOAD_SETS_MAX];
    int sets;
    uint8_t manifest_sha256[32];
} VjoPayload;

/* The compiled list: each destination and kind, in install order. */
const char *vjo_payload_dest(int index);
int vjo_payload_kind(int index);
/* A destination's file inside the app folder (VJO_PAYLOAD_DIR "/ux0/data/..."), into out: 0, or -1. */
int vjo_payload_source(int index, char *out, size_t cap);

/* Parses text[0, len) as a payload manifest of the build own: 0, or -1 with why (what is wrong).
 * Every rule of the header is checked; any other text is refused. */
int vjo_payload_parse(const char *text, size_t len, const VjoPayloadBuild *own, VjoPayload *out, char *why,
                      size_t cap);

typedef struct {
    char version[40], variant[8], build[17];
    int api;
    uint8_t manifest_sha256[32];
} VjoInstalled;

/* installed.txt's line for p (with its LF): its length, or -1. */
int vjo_installed_format(char *out, size_t cap, const VjoPayload *p);
/* Parses an installed.txt line: 0, or -1 (an empty or another file: no record). */
int vjo_installed_parse(const char *text, size_t len, VjoInstalled *out);

/* Compares two versions (0.8.0, 0.8.0-test.4): < 0 when a is older, 0, > 0 newer. A pre-release
 * (-...) is older than its release; build metadata (+...) is ignored. */
int vjo_version_compare(const char *a, const char *b);

/* 64 lowercase hex digits of sha into out (65 bytes). */
void vjo_sha256_hex(const uint8_t sha[32], char out[65]);
#endif
