#include "textjoin.h"

#include <stdint.h>
#include <string.h>

#include "utf.h"

/* A sentence's end or a closing bracket: 。．｡ . ! ? ！？ ) ] } ）］｝〟, and the
 * U+3008..301B pairs 〉》」』】〕〗〙〛 (odd); the opening ones ( [ { （［｛〝
 * and 〈《「『【〔〖〘〚 (even). 〒 and 〓 (3012, 3013) are no brackets. */
static const uint16_t ends[] = {0x3002, 0xFF0E, 0xFF61, '.', '!', '?', 0xFF01, 0xFF1F,
                                ')', ']', '}', 0xFF09, 0xFF3D, 0xFF5D, 0x301F};
static const uint16_t starts[] = {'(', '[', '{', 0xFF08, 0xFF3B, 0xFF5B, 0x301D};

static int mark(uint32_t c, const uint16_t *t, int n, uint32_t odd)
{
    while (n--)
        if (t[n] == c)
            return 1;
    return c >= 0x3008 && c <= 0x301B && (c & 1) == odd && (c >> 1) != (0x3012 >> 1);
}

/* The text's lines in one pass: the longest one's characters, and for the
 * line ending at the '\n' at byte nl, its characters and last character, and
 * the next line's first. Spaces at a line's ends are not counted. */
static int joins(const char *s, size_t len, size_t nl)
{
    size_t i = 0, chars = 0, gap = 0, max = 0, before = 0;
    uint32_t last = 0, prev_last = 0, next_first = 0;
    int line = 0; /* 0: up to the break, 1: the line after it, 2: later */
    for (;;) {
        uint32_t c = i < len ? vjo_utf8_next(s, len, &i) : '\n';
        if (c == '\n') {
            if (chars > max)
                max = chars;
            if (line == 0 && i == nl + 1) { /* the line before the break */
                before = chars;
                prev_last = last;
                line = 1;
            } else if (line == 1)
                line = 2;
            if (i >= len)
                break;
            chars = gap = 0;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == 0x3000) {
            gap += chars != 0; /* counted only once a character follows */
            continue;
        }
        if (line == 1 && !next_first)
            next_first = c;
        chars += gap + 1;
        gap = 0;
        last = c;
    }
    return before >= VJO_JOIN_MIN_CHARS && before + VJO_JOIN_SLACK >= max && next_first &&
           !mark(prev_last, ends, sizeof(ends) / sizeof(ends[0]), 1) &&
           !mark(next_first, starts, sizeof(starts) / sizeof(starts[0]), 0);
}

int vjo_text_joins(const char *s, size_t len, size_t at)
{
    if (at + 1 < len && s[at] == '\r')
        at++;
    return at < len && s[at] == '\n' && joins(s, len, at);
}

size_t vjo_text_join(char *s, int *offs, int n)
{
    size_t len = strlen(s), i = len, orig = len;
    /* From the end, so that a removed break moves only text after the ones
     * still to decide. Each break is decided on the text as recognized: the
     * rule reads the text's longest line, so the breaks are found first. */
    char keep[VJO_JOIN_MAX_BREAKS];
    size_t at[VJO_JOIN_MAX_BREAKS];
    int k = 0;
    for (size_t j = 0; j < orig && k < VJO_JOIN_MAX_BREAKS; j++)
        if (s[j] == '\n') {
            at[k] = j;
            keep[k++] = (char)!joins(s, orig, j);
        }
    while (k-- > 0) {
        size_t b;
        if (keep[k])
            continue;
        i = at[k];
        b = i && s[i - 1] == '\r' ? i - 1 : i;
        memmove(s + b, s + i + 1, len - i); /* the NUL too */
        len -= i + 1 - b;
        for (int m = 0; m < n; m++)
            if (offs[m] > (int)b)
                offs[m] = offs[m] > (int)i ? offs[m] - (int)(i + 1 - b) : (int)b;
    }
    return len;
}
