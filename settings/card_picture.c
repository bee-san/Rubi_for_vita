#include "card_picture.h"

#include <setjmp.h>
#include <stdio.h> /* (jpeglib.h needs FILE) */
#include <string.h>

#include <jpeglib.h>

#include "../core/errors.h"

void vjo_log(const char *fmt, ...); /* the app's log.txt lines (settings/applog.c) */

/* libjpeg's errors come back here instead of ending the process (its default error_exit). */
typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf back;
    int logged;
} Errors;

static void on_error(j_common_ptr c)
{
    Errors *e = (Errors *)c->err;
    char msg[JMSG_LENGTH_MAX];
    c->err->format_message(c, msg);
    vjo_log("cards: picture: %s", msg);
    longjmp(e->back, 1);
}

/* A warning (corrupt data libjpeg decodes past): the first one in log.txt. */
static void on_message(j_common_ptr c)
{
    Errors *e = (Errors *)c->err;
    char msg[JMSG_LENGTH_MAX];
    if (e->logged++)
        return;
    c->err->format_message(c, msg);
    vjo_log("cards: picture: %s", msg);
}

int vjo_card_picture_decode(VjoArena *a, const uint8_t *jpeg, size_t len, unsigned box_w, unsigned box_h,
                            VjoCardPixels *out)
{
    struct jpeg_decompress_struct d;
    Errors err;
    const size_t mark = vjo_arena_mark(a);
    volatile int rc = VJO_E_PARSE;
    unsigned m, w, h, stride;
    uint8_t *rgb;
    memset(out, 0, sizeof(*out));
    memset(&d, 0, sizeof(d));
    memset(&err, 0, sizeof(err));
    d.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = on_error;
    err.pub.output_message = on_message;
    if (setjmp(err.back)) {
        /* (d.mem is NULL until jpeg_create_decompress has made it: destroying is then a no-op) */
        jpeg_destroy_decompress(&d);
        vjo_arena_release(a, mark);
        memset(out, 0, sizeof(*out));
        return rc;
    }
    jpeg_create_decompress(&d);
    /* A progressive (or multi-scan) file keeps its whole coefficient arrays: at most this much, else
     * libjpeg's error (no backing store), as for a size too large to show. A baseline one, as every
     * screenshot Rubi takes is (core/jpegsw.c), keeps no such arrays. */
    d.mem->max_memory_to_use = VJO_CARD_PICTURE_JPEG_MEM;
    jpeg_mem_src(&d, (const unsigned char *)jpeg, (unsigned long)len);
    if (jpeg_read_header(&d, TRUE) != JPEG_HEADER_OK)
        longjmp(err.back, 1);
    d.out_color_space = JCS_RGB; /* a grey picture too: three bytes a pixel, the texture's format */
    for (m = 1; m <= 8; m++) {
        d.scale_num = m;
        d.scale_denom = 8;
        jpeg_calc_output_dimensions(&d);
        if (d.output_width >= box_w || d.output_height >= box_h)
            break;
    }
    if (m > 8)
        m = 8; /* smaller than the box at full size: drawn larger */
    for (;;) {
        d.scale_num = m;
        d.scale_denom = 8;
        jpeg_calc_output_dimensions(&d);
        if ((d.output_width <= VJO_CARD_PICTURE_W && d.output_height <= VJO_CARD_PICTURE_H) || m == 1)
            break;
        m--;
    }
    w = d.output_width;
    h = d.output_height;
    if (!w || !h || w > VJO_CARD_PICTURE_W || h > VJO_CARD_PICTURE_H) {
        vjo_log("cards: picture: %ux%u, too large to show", (unsigned)d.image_width, (unsigned)d.image_height);
        rc = VJO_E_TOO_LARGE;
        longjmp(err.back, 1);
    }
    stride = ((w + 7u) & ~7u) * 3u;
    if (!(rgb = (uint8_t *)vjo_arena_alloc(a, (size_t)stride * h))) {
        rc = VJO_E_OOM;
        longjmp(err.back, 1);
    }
    jpeg_start_decompress(&d);
    if (d.output_width != w || d.output_height != h || d.output_components != 3)
        longjmp(err.back, 1);
    while (d.output_scanline < h) {
        JSAMPROW row = rgb + (size_t)d.output_scanline * stride;
        if (jpeg_read_scanlines(&d, &row, 1) != 1)
            longjmp(err.back, 1); /* (a source in memory never suspends) */
    }
    jpeg_finish_decompress(&d);
    jpeg_destroy_decompress(&d);
    out->rgb = rgb;
    out->width = w;
    out->height = h;
    out->stride = stride;
    return 0;
}
