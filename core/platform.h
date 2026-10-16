/* Platform services for the core: random-access files (the local dictionaries,
 * the OCR models) and a monotonic clock. The overlay is offline: there are no
 * sockets, no TLS and no HTTP (the last network use, Google Lens, was removed). */
#ifndef VJO_PLATFORM_H
#define VJO_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "errors.h"
#include "file.h"

typedef struct {
    void *ud;
    /* Opens a file for reading: 0, or -1 when it cannot be opened. */
    int (*file_open)(void *ud, const char *path, VjoFile *out);
    /* optional: a monotonic clock in microseconds (stage times in the log) */
    uint64_t (*now_us)(void *ud);
} VjoPlatform;

typedef struct {
    int rc;             /* VJO_OK or VJO_E_* */
    const char *detail; /* a message for the user, may be NULL */
    int dict;           /* VJO_DICT_* for VJO_STAGE_DICT errors */
} VjoErr;

#endif
