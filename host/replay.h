/* Host replay of recorded responses (vjo-cli --replay/--record and the
 * fixture tests): in-memory connection, fixture file names and a
 * VjoPlatform that serves a fixture dir. */
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

/* Recorded response file in a fixture dir for a request host: "lens.pb" for
 * Google Lens (--record writes it, the replay reads it), NULL for any other
 * host (not recorded; the replay refuses it like an offline network). */
const char *vjo_fixture_file(const char *host);

/* vjo_overlay_ocr (empty JPEG) on a VjoPlatform whose connect() serves dir's
 * recorded Lens response as a plain HTTP 200 response, or fails like an
 * offline network when there is none: the sentence, without a lookup. The
 * response is loaded into `files`. Returns vjo_overlay_ocr's result. */
int vjo_replay_ocr(VjoArena *a, VjoArena *files, const char *dir, const VjoConfig *cfg, VjoOverlayData *out);

#endif
