#include "textjoin.h"

#include <stdint.h>
#include <string.h>

#include "utf.h"

static int space(uint32_t c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == 0x3000;
}

/* U+3008..301B pairs its brackets: 〈《「『【〔〖〘〚 open at even code points
 * and close at the odd one after (〒 and 〓, 3012 and 3013, are no brackets). */
static int closes(uint32_t c)
{
    return c == 0x3002 || c == 0xFF0E || c == 0xFF61 || c == '.' || c == '!' || c == '?' || c == 0xFF01 ||
           c == 0xFF1F || c == ')' || c == ']' || c == '}' || c == 0xFF09 || c == 0xFF3D || c == 0xFF5D ||
           c == 0x301F || (c >= 0x3009 && c <= 0x301B && (c & 1) && c != 0x3013);
}

static int opens(uint32_t c)
{
    return c == '(' || c == '[' || c == '{' || c == 0xFF08 || c == 0xFF3B || c == 0xFF5B || c == 0x301D ||
           (c >= 0x3008 && c <= 0x301A && !(c & 1) && c != 0x3012);
}

typedef struct {
    size_t chars;         /* code points from the first character to the last, spaces between them included */
    uint32_t first, last; /* 0: an empty line */
} Line;

/* The line starting at byte `from`, up to its '\n' or the text's end. */
static void line_at(const char *s, size_t len, size_t from, Line *l)
{
    size_t gap = 0;
    l->chars = 0;
    l->first = l->last = 0;
    while (from < len && s[from] != '\n') {
        uint32_t c = vjo_utf8_next(s, len, &from);
        if (space(c)) {
            gap += l->chars != 0; /* counted only once a character follows */
            continue;
        }
        l->chars += gap + 1;
        gap = 0;
        if (!l->first)
            l->first = c;
        l->last = c;
    }
}

static size_t longest(const char *s, size_t len)
{
    size_t max = 0;
    for (size_t i = 0;; i++) {
        Line l;
        line_at(s, len, i, &l);
        if (l.chars > max)
            max = l.chars;
        while (i < len && s[i] != '\n')
            i++;
        if (i >= len)
            return max;
    }
}

/* nl: a '\n'; max: the longest line of the text as recognized */
static int joins(const char *s, size_t len, size_t nl, size_t max)
{
    size_t start = nl;
    Line a, b;
    while (start && s[start - 1] != '\n')
        start--;
    line_at(s, nl, start, &a);
    line_at(s, len, nl + 1, &b);
    return a.chars >= VJO_JOIN_MIN_CHARS && a.chars + VJO_JOIN_SLACK >= max && !closes(a.last) && b.first &&
           !opens(b.first);
}

int vjo_text_joins(const char *s, size_t len, size_t at)
{
    if (at + 1 < len && s[at] == '\r')
        at++;
    return at < len && s[at] == '\n' && joins(s, len, at, longest(s, len));
}

size_t vjo_text_join(char *s, int *offs, int n)
{
    size_t len = strlen(s), max = longest(s, len), i = len;
    /* From the end, so that a removed break moves only text after the ones still to decide;
     * each decides on its own two lines, which no later removal changes, and the text's
     * longest line as recognized. */
    while (i-- > 0) {
        size_t b;
        if (s[i] != '\n' || !joins(s, len, i, max))
            continue;
        b = i && s[i - 1] == '\r' ? i - 1 : i;
        memmove(s + b, s + i + 1, len - i); /* the NUL too */
        len -= i + 1 - b;
        for (int k = 0; k < n; k++)
            if (offs[k] > (int)b)
                offs[k] = offs[k] > (int)i ? offs[k] - (int)(i + 1 - b) : (int)b;
        i = b;
    }
    return len;
}
