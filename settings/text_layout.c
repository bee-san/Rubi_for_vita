#include "text_layout.h"
#include <string.h>
#include "../core/linebreak.h"
#include "../core/utf.h"

/* A line ends only at a decoded UTF-8 boundary. PVF advances, rather than
 * byte counts, decide how many Japanese/Latin glyphs fit in the card.
 * (settings/main.c's fit_line but for the measure callback and where a row
 * ends.) When the text goes on, the row ends at its last space that fits, as
 * before, or later where a line may break (core/linebreak.h: after
 * punctuation, where a Japanese phrase starts; never inside a word), before a
 * space there: so Latin text breaks as before and Japanese no longer
 * anywhere. With neither in the row (one word wider than it), at its last glyph. */
static size_t fit_line(const char *s, float pixels, int width, char out[512], VjoTextMeasure measure)
{
    size_t len = strlen(s), end = 0, space = 0, b;
    for (size_t i = 0; i < len;) {
        size_t begin = i;
        uint32_t cp = vjo_utf8_next(s, len, &i);
        if (cp == '\n' || i > VJO_TEXT_ROW_BYTES) break; /* Reserve the final "..." and NUL. */
        memcpy(out, s, i);
        out[i] = 0;
        if (measure(pixels, out) > width) break;
        end = i;
        if (cp == ' ') space = begin;
    }
    if (s[end] && s[end] != '\n') {
        b = vjo_break_last8(s, len, 0, end);
        b -= b && s[b - 1] == ' ';
        if (b > space) space = b;
        if (space) end = space;
    }
    memcpy(out, s, end);
    out[end] = 0;
    return end;
}

/* wrapped_text's loop, recording each row instead of drawing it. */
void vjo_text_layout(const char *s, int width, float pixels, int lines, VjoTextMeasure measure,
                     VjoTextLayout *out)
{
    char line[512];
    const char *source = s;
    if (lines > VJO_TEXT_ROWS) lines = VJO_TEXT_ROWS;
    memset(out, 0, sizeof(*out));
    for (int row = 0; row < lines && *s; row++) {
        while (*s == ' ' || *s == '\r') s++;
        size_t end = fit_line(s, pixels, width, line, measure);
        const char *remaining = s + end;
        int ellipsis = 0;
        while (*remaining == ' ' || *remaining == '\r' || *remaining == '\n') remaining++;
        if (row == lines - 1 && *remaining) {
            end = fit_line(s, pixels, width - measure(pixels, "..."), line, measure);
            ellipsis = 1;
        }
        VjoTextRow *r = &out->row[out->rows++];
        r->start = (unsigned)(s - source);
        r->bytes = (unsigned short)end;
        r->ellipsis = (unsigned char)ellipsis;
        if (!end && *s != '\n') break; /* Even one glyph can exceed a tiny region. */
        s += end;
        if (*s == '\n') s++;
    }
}

void vjo_text_layout_row(const char *s, const VjoTextLayout *layout, int r, char out[512])
{
    const VjoTextRow *row = &layout->row[r];
    memcpy(out, s + row->start, row->bytes);
    memcpy(out + row->bytes, row->ellipsis ? "..." : "", row->ellipsis ? 4 : 1);
}

static uint32_t fnv1a(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

const VjoTextLayout *vjo_text_memo_layout(VjoTextMemo *memo, const char *s, int width, float pixels, int lines,
                                          VjoTextMeasure measure, VjoTextLayout *scratch)
{
    size_t bytes = strlen(s);
    if (lines > VJO_TEXT_ROWS) lines = VJO_TEXT_ROWS;
    if (bytes >= VJO_TEXT_MEMO_TEXT) {
        memo->misses++;
        vjo_text_layout(s, width, pixels, lines, measure, scratch);
        return scratch;
    }
    uint32_t hash = fnv1a(s, bytes);
    VjoTextMemoEntry *oldest = &memo->entry[0];
    for (int i = 0; i < VJO_TEXT_MEMO; i++) {
        VjoTextMemoEntry *e = &memo->entry[i];
        if (e->used && e->hash == hash && e->bytes == bytes && e->width == width && e->lines == lines &&
            e->pixels == pixels && !memcmp(e->text, s, bytes)) {
            e->used = ++memo->clock;
            memo->hits++;
            return &e->layout;
        }
        if (e->used < oldest->used) oldest = e;
    }
    memo->misses++;
    oldest->hash = hash;
    oldest->bytes = (unsigned short)bytes;
    oldest->width = width;
    oldest->lines = (unsigned char)lines;
    oldest->pixels = pixels;
    memcpy(oldest->text, s, bytes + 1);
    vjo_text_layout(oldest->text, width, pixels, lines, measure, &oldest->layout);
    oldest->used = ++memo->clock;
    return &oldest->layout;
}

void vjo_text_memo_clear(VjoTextMemo *memo)
{
    memset(memo, 0, sizeof(*memo));
}
