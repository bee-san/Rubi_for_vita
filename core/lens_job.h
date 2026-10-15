/* Google Lens as a job of the game worker (docs/shell-slim.md): the region's
 * grey rows, read in strips, become a baseline JPEG that is posted to Lens;
 * the reply's text comes back. The rows come from a VjoGreySource (in the
 * game, the display hook's strips), the network from a VjoPlatform (the game's
 * libnet: game_ocr/net_game.c), every byte of memory from the caller: no
 * allocation, no file, no global state, the same code on the host.
 *
 * The Shell's Lens capture was one frame; strips are one frame each, so a
 * screen that changes while they are read could give a torn image. Each try
 * therefore reads the region twice: a pass that encodes (its strips flagged
 * VJO_STRIP_FIRST, the last VJO_STRIP_LAST, so the game's capture of it is
 * published) and a pass that only compares every row's checksum with it. A
 * change is tried again, at most VJO_VOCR_TRIES times; the last try's image is
 * sent whatever the comparison says (a region with an animation never holds
 * still, and the Shell's single frame never failed on one). */
#ifndef VJO_LENS_JOB_H
#define VJO_LENS_JOB_H

#include <stddef.h>
#include <stdint.h>

#include "net.h"
#include "vocr_ocr.h" /* VjoGreySource, VJO_STRIP_*, VJO_VOCR_TRIES */

/* The quality the Shell's Lens capture used (shell/worker.c JPEG_QUALITY). */
#define VJO_LENS_JPEG_QUALITY 80
/* The JPEG's room in the workspace: three quarters of a byte per pixel. A
 * dialogue box at quality 80 takes a tenth of that [measured: 409 x 104 ->
 * 10,070 B, docs/lens-performance.md], a full screen of dense text a half
 * (host/tests/test_lens_job.c); an image that does not fit is VJO_E_OOM, as the
 * Shell's encoder's was. */
#define VJO_LENS_JPEG_ROOM(w, h) ((size_t)(w) * (size_t)(h) * 3u / 4u)
/* The request and reply after the image: the TLS state and its buffer
 * (core/net.c single_request, ~25 KB), the request's 4 KiB chunk, the reply
 * (VJO_LENS_MAX_RESPONSE, 256 KiB) and its parse. */
#define VJO_LENS_NET_ROOM (320u * 1024u)

typedef struct {
    uint32_t tries;               /* encoding passes (1..VJO_VOCR_TRIES) */
    uint32_t tears;               /* rows the last comparison found changed: 0, the image held still; after
                                   * VJO_VOCR_TRIES tries the last image is not compared and may be torn */
    uint32_t strips, strip_rows;  /* strips read over every pass, rows in each */
    uint32_t capture_us;          /* waiting for and copying the strips */
    uint32_t jpeg_bytes, jpeg_us; /* the image sent, and its encodings (the strips' time excluded) */
    uint32_t request_us;          /* DNS, connect, TLS, upload and the reply */
    uint32_t region_sum;          /* FNV-1a over the sent image's row checksums (as VjoGameOcrResult.region_sum) */
    uint32_t text_bytes;          /* the text returned, without its NUL */
    int truncated;                /* the text did not fit text_cap: cut at a character */
    size_t peak;                  /* the workspace's high-water mark */
} VjoLensJobStats;

/* Workspace bytes for a job on a width x height region: the encoder's state
 * and 16-row RGBA band, the rows' checksums, the JPEG's room and
 * VJO_LENS_NET_ROOM; 0 for a size Lens is never asked for (0, or over 960 x 544). */
size_t vjo_lens_job_bytes(unsigned width, unsigned height);
/* Rows per strip for a region width, given a strip buffer of strip_bytes: a
 * multiple of 16 (the encoder's band) up to 64 (the kernel's most per call);
 * 0 if not even 16 rows fit. */
unsigned vjo_lens_strip_rows(unsigned width, size_t strip_bytes);

/* One job: reads image (see above) into strip; encodes it at `quality`; posts
 * it to Lens through p (its log gets the request's lines, never the text); and
 * writes the reply's text, NUL-terminated, into text. Returns VJO_OK or a
 * VJO_E_* code (the strip source's own error included, VJO_E_CANCELLED when
 * image->cancelled said so); err has the network's details. text is "" on
 * failure. */
int vjo_lens_job(const VjoPlatform *p, const VjoGreySource *image, unsigned char *strip, size_t strip_bytes,
                 void *workspace, size_t workspace_bytes, int quality, char *text, size_t text_cap,
                 VjoErr *err, VjoLensJobStats *st);

#endif
