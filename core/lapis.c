#include "lapis.h"

#include <string.h>

#include "lapis_notetype.inc"

#define NAME(i, name) name,
const char *const vjo_lapis_field[VJO_LAPIS_FIELDS] = {VJO_LAPIS_FIELD_NAMES(NAME)};
#undef NAME
typedef char vjo_lapis_field_count_check[VJO_LAPIS_FIELD_COUNT == VJO_LAPIS_FIELDS ? 1 : -1];

const long long vjo_lapis_notetype_id = VJO_LAPIS_NOTETYPE_ID;
const long long vjo_lapis_deck_id = VJO_LAPIS_DECK_ID;
const char vjo_lapis_deck_name[] = VJO_LAPIS_DECK_NAME;
const char vjo_lapis_tag[] = "vita-jp-overlay";
const char *const vjo_lapis_notetype = vjo_lapis_notetype_json;

char *vjo_lapis_field_value(VjoArena *a, int f, const VjoAnkiNote *n, const char *picture)
{
    if (f == VJO_LAPIS_PICTURE) {
        VjoBuf b;
        vjo_buf_init(&b, a);
        if (picture) { /* a name the exporter made: vitajp_<32 hex digits>.jpg */
            vjo_buf_puts(&b, "<img src=\"");
            vjo_buf_puts(&b, picture);
            vjo_buf_puts(&b, "\">");
        }
        return vjo_buf_cstr(&b);
    }
    return vjo_anki_field_value(a, f, n);
}

char *vjo_lapis_fields(VjoArena *a, const VjoAnkiNote *n, const char *picture, size_t *len)
{
    VjoBuf b;
    char *v[VJO_LAPIS_FIELDS];
    for (int f = 0; f < VJO_LAPIS_FIELDS; f++)
        if (!(v[f] = vjo_lapis_field_value(a, f, n, picture)))
            return NULL;
    vjo_buf_init(&b, a);
    for (int f = 0; f < VJO_LAPIS_FIELDS; f++) {
        size_t at;
        if (f)
            vjo_buf_putc(&b, 0x1F);
        at = b.len;
        vjo_buf_puts(&b, v[f]);
        /* A separator inside a value (a dictionary's text can hold any
         * character) would give the note a field too many: a space instead. */
        for (size_t i = at; !b.oom && i < b.len; i++)
            if (b.data[i] == 0x1F)
                b.data[i] = ' ';
    }
    if (b.oom)
        return NULL;
    *len = b.len;
    return vjo_buf_cstr(&b);
}
