/* Host test helpers: a whole file into an arena, and an in-memory
 * connection (the HTTP clients' tests). */
#ifndef VJO_REPLAY_H
#define VJO_REPLAY_H

#include "client.h"

/* Reads a whole file into the arena (NUL-terminated). */
char *vjo_read_file(VjoArena *a, const char *path, size_t *len);

/* In-memory VjoConn: recv serves `in` (at most `step` bytes per call when
 * step > 0) and then returns end_rc (0 = EOF); send appends to `out` (up to
 * out_cap bytes; discarded when out is NULL). */
typedef struct {
    const char *in;
    size_t pos, len, step;
    char *out;
    size_t out_cap, out_len;
    int end_rc;
} VjoMemConn;

void vjo_memconn_init(VjoMemConn *m, VjoConn *c);

#endif
