/* End-to-end requests: Lens OCR over HTTPS and the dictionary lookup (the
 * local .vjdict files), plus the overlay pipeline that ties them together
 * (same code on host and Vita). */
#ifndef VJO_CLIENT_H
#define VJO_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "config.h"
#include "entries.h"
#include "dict.h"
#include "lens.h"
#include "net.h"


typedef struct {
    void *ud;
    /* Copies len bytes at offset off; returns 0 or < 0 on failure. */
    int (*read)(void *ud, uint32_t off, void *dst, uint32_t len);
    uint32_t size;
    uint32_t width, height;
} VjoJpegSource;


/* Stages for VjoOverlayData.failed_stage */
enum { VJO_STAGE_NONE = 0, VJO_STAGE_OCR = 1, VJO_STAGE_DICT = 2 }; /* OCR, then dictionary */

/* Disk locators of the local dictionaries' entries at one cursor (local_dict.h). */
typedef struct VjoLocalPages VjoLocalPages;

typedef struct {
    const char *ocr_text;   /* recognized UTF-8 text */
    const char *filtered;   /* after non_japanese_filter */
    const char *sentence;   /* filtered, trimmed: the header's text (and the subtitle) */
    VjoEntryList list;      /* header + entries */
    int failed_stage;
    VjoErr err;
    /* Local cursor lookups with local_dictionary_results > 1: the page
     * locators, allocated first in the lookup's arena (NULL otherwise), and
     * the page whose entry `list` holds. */
    VjoLocalPages *pages;
    int page;
} VjoOverlayData;

#define VJO_LENS_MAX_RESPONSE (256u * 1024u)

int vjo_lens_ocr(VjoArena *a, const VjoPlatform *p, const VjoJpegSource *src,
                 VjoLensResult *res, const char **text, VjoErr *err);

/* Looks up text in the local .vjdict files (core/local_dict.h), the only
 * dictionaries: no lookup uses the network. */
int vjo_dict_lookup(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg, const char *text,
                    VjoDictResult *res, VjoErr *err);

/* A recognition in two phases (the sentence is ready before the lookup):
 * vjo_overlay_ocr: JPEG -> Lens -> filter, sets ocr_text, filtered and
 * sentence; vjo_overlay_lookup: dictionary -> entries (list), after a
 * successful vjo_overlay_ocr. Both return VJO_OK or the error, also in
 * out->err with out->failed_stage. */
int vjo_overlay_ocr(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                    const VjoJpegSource *src, VjoOverlayData *out);
int vjo_overlay_lookup(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg, VjoOverlayData *out);
/* Local-only lookup at a raw UTF-8 byte cursor in the existing trimmed
 * sentence. Preserves OCR text/header and returns at most one entry, with
 * highlight bytes relative to sentence (including its remaining newlines).
 * The caller releases the preceding lookup's arena before another cursor.
 * With cfg->local_dictionary_results > 1, out->pages also locates the other
 * dictionaries' entries; the shown entry is page 0. */
int vjo_overlay_lookup_at(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                          size_t cursor_byte, VjoOverlayData *out);
/* Replaces the entry of a successful vjo_overlay_lookup_at in the same arena
 * (out->pages set) with page `page`: releases the previous entry, keeps the
 * locators, and reads only that dictionary's payload. On failure the page
 * shows no entry and the locators remain for another page. */
int vjo_overlay_lookup_page(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                            int page, VjoOverlayData *out);
/* Dictionary pages available for out's shown local result (its entry count
 * without locators). */
int vjo_overlay_page_count(const VjoOverlayData *out);
/* Already recognized text -> filter/sentence, without a dictionary request.
 * text must remain valid for the lifetime of out (normally in a). */
int vjo_overlay_ocr_text(VjoArena *a, const VjoConfig *cfg, const char *text, VjoOverlayData *out);

/* OCR text -> filter -> vjo_overlay_lookup. */
int vjo_overlay_from_text(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                          const char *ocr_text, VjoOverlayData *out);

/* vjo_overlay_ocr, then vjo_overlay_lookup. */
int vjo_overlay_from_jpeg(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                          const VjoJpegSource *src, VjoOverlayData *out);

/* Opens the Lens connection a recognition will use ahead of it, when p keeps
 * connections (p->pool), the connect giving up after timeout_us. */
void vjo_overlay_warm(const VjoPlatform *p, int timeout_us);

/* Short user-facing description of an error. */
const char *vjo_err_text(VjoArena *a, int stage, const VjoErr *err);

#endif
