/* A card's picture for the Cards page (settings/cards_page.c): its JPEG (anki_queue/<id>.jpg, at most
 * VJO_QUEUE_PICTURE_MAX bytes) decoded by libjpeg (libjpeg-turbo, the library libvita2d links), scaled
 * while it decodes (M/8) to the smallest size that covers the page's box, into RGB rows the page
 * copies into a texture.
 *
 * Not libvita2d's vita2d_load_JPEG_buffer: its libjpeg error handler is the default one, which ends
 * the process on a damaged file (libvita2d a8f15ab vita2d_image_jpeg.c: jpeg_std_error, no setjmp),
 * and it decodes at full size. Here an error returns to the caller (setjmp), and the app keeps the
 * card, without its picture. libjpeg's working memory comes from newlib's heap (settings/sysapp.c,
 * 8 MiB) and goes back to it before this returns; the pixels are in the caller's arena. */
#ifndef VJO_SETTINGS_CARD_PICTURE_H
#define VJO_SETTINGS_CARD_PICTURE_H
#include <stddef.h>
#include <stdint.h>
#include "../core/arena.h"

/* The most pixels a decoded picture has, whatever its box: its texture is at most
 * 512 × 288 × 3 = 442,368 B. */
#define VJO_CARD_PICTURE_W 512u
#define VJO_CARD_PICTURE_H 288u
/* The most of newlib's heap libjpeg's coefficient arrays take for a progressive file (card_picture.c). */
#define VJO_CARD_PICTURE_JPEG_MEM (4L * 1024 * 1024)

typedef struct {
    uint8_t *rgb;           /* height rows of stride bytes: R, G, B for each of width pixels */
    unsigned width, height;
    unsigned stride;        /* (width rounded up to 8) × 3: a vita2d texture's row (U8U8U8_BGR) */
} VjoCardPixels;

/* jpeg (len bytes) into out, its pixels in a: scaled by the smallest M/8 (M 1-8) whose size reaches
 * box_w or box_h, then by less while it is over VJO_CARD_PICTURE_W × _H. 0, or VJO_E_PARSE (not a
 * JPEG libjpeg decodes: its message in log.txt), VJO_E_TOO_LARGE, VJO_E_OOM (nothing kept in a). */
int vjo_card_picture_decode(VjoArena *a, const uint8_t *jpeg, size_t len, unsigned box_w, unsigned box_h,
                            VjoCardPixels *out);
#endif
