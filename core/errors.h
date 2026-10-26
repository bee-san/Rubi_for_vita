/* Result codes shared by the core, the Shell and the Rubi app. Numbers are
 * never reused, so an older build's log still reads unambiguously
 * (tools/analyse_selftest.py RC_NAMES keeps their old names). */
#ifndef VJO_ERRORS_H
#define VJO_ERRORS_H

enum {
    VJO_OK = 0,
    /* -100 to -102 were the network's (VJO_E_NET, VJO_E_TLS, VJO_E_HTTP): the
     * overlay is offline, the sockets, TLS and HTTP client were removed */
    VJO_E_OOM = -103,       /* arena exhausted */
    VJO_E_PARSE = -104,     /* malformed JSON / package data */
    VJO_E_TOO_LARGE = -105, /* a result or text over its limit */
    /* -106 was VJO_E_STATUS, a non-2xx HTTP status (removed with the network) */
    /* -107 was VJO_E_NO_KEY, the removed online dictionaries' missing API key */
    VJO_E_SOURCE = -108,    /* the capture source could not be read */
    /* -109 to -111 were AnkiConnect's (removed) */
    /* -112 was VJO_E_NO_HOST, the removed Hachidori relay's unset or invalid host */
    VJO_E_LOCAL_IO = -113, /* cannot open/read a local dictionary */
    VJO_E_LOCAL_FORMAT = -114, /* incompatible/corrupt .vjdict */
    VJO_E_OCR_MODEL = -115, /* missing/incomplete/unexpected local OCR weights */
    VJO_E_OCR_REGION = -116, /* unsupported/oversized dialogue region */
    VJO_E_OCR_UNAVAILABLE = -117, /* build has no local OCR */
    VJO_E_CANCELLED = -118,
    VJO_E_OCR_INFERENCE = -119,
    VJO_E_SCREEN_CHANGED = -120, /* the screen changed while the game read it, on every try */
    /* API 44 (config.ini ocr_vertical = auto): the region's rows looked like vertical text, so nothing was
     * read (core/vocr_ocr.h VjoVocrPolicy.vertical_check); the game worker reads its columns next. It never
     * leaves the worker. */
    VJO_E_VERTICAL = -121,
};

#endif
