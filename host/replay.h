/* Host test helper: a whole file into an arena. */
#ifndef VJO_REPLAY_H
#define VJO_REPLAY_H

#include "client.h"

/* Reads a whole file into the arena (NUL-terminated). */
char *vjo_read_file(VjoArena *a, const char *path, size_t *len);

#endif
