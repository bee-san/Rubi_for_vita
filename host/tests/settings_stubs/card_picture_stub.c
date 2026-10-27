/* The Cards page's picture decoder (settings/card_picture.c, libjpeg) is not what the Anki page's suite
 * (test_settings_anki.c) drives: the host suites build without libjpeg, and test_card_picture.c tests the
 * decoder itself where libjpeg is. Here a "picture" is any file with a JPEG's start and end markers: it decodes
 * to the box's size in one grey (its bytes' sum), so the page's texture, its scale and its failures can be
 * checked; anything else is VJO_E_PARSE, as libjpeg's error would be. */
#include "../../../settings/card_picture.h"
#include <string.h>
#include "../../../core/errors.h"

int vjo_test_picture_decodes, vjo_test_picture_fail; /* calls; a decode that fails: 1 VJO_E_OOM, 2 VJO_E_PARSE */

int vjo_card_picture_decode(VjoArena *a, const uint8_t *jpeg, size_t len, unsigned box_w, unsigned box_h,
                            VjoCardPixels *out)
{
    unsigned sum = 0, w = box_w > VJO_CARD_PICTURE_W ? VJO_CARD_PICTURE_W : box_w;
    unsigned h = box_h > VJO_CARD_PICTURE_H ? VJO_CARD_PICTURE_H : box_h;
    memset(out, 0, sizeof(*out));
    vjo_test_picture_decodes++;
    if (vjo_test_picture_fail)
        return vjo_test_picture_fail == 1 ? VJO_E_OOM : VJO_E_PARSE;
    if (len < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8 || jpeg[len - 2] != 0xFF || jpeg[len - 1] != 0xD9)
        return VJO_E_PARSE;
    for (size_t i = 0; i < len; i++)
        sum += jpeg[i];
    out->stride = ((w + 7u) & ~7u) * 3u;
    if (!(out->rgb = (uint8_t *)vjo_arena_alloc(a, (size_t)out->stride * h)))
        return VJO_E_OOM;
    memset(out->rgb, (int)(sum & 0xFF), (size_t)out->stride * h);
    out->width = w;
    out->height = h;
    return 0;
}
