#ifndef VJO_PLATFORM_POSIX_H
#define VJO_PLATFORM_POSIX_H

#include "client.h"

typedef struct {
    int verbose;
} PosixPlatform;

/* Files (read-only, pread) and the monotonic clock, for the host CLI, tools and tests. */
void posix_platform_init(PosixPlatform *pp, VjoPlatform *p);

#endif
