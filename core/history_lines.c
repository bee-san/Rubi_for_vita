#include "history_lines.h"

#include <string.h>

#include "textfilter.h"
#include "utf.h"

/* The line of the record that starts at p (its '"') and ends before end, when the record is game
 * uuid's (its first field, the closing quote and comma included): its fourth field, unescaped in place and NUL-terminated; NULL otherwise, and when the
 * record is cut before the field after that one. shell/history.c quotes each of the first four
 * fields and doubles a '"' inside one. */
static char *record_line(char *p, char *end, const char *uuid, size_t ulen)
{
    if ((size_t)(end - p) < ulen + 2 || memcmp(p + 1, uuid, ulen))
        return NULL;
    p += ulen + 1; /* the second field's opening quote */
    for (int field = 2;; field++) {
        char *q = p + 1, *o = q;
        if (p >= end || *p != '"')
            return NULL;
        for (;;) {
            if (q >= end)
                return NULL;
            if (*q == '"') {
                if (q + 1 < end && q[1] == '"') {
                    *o++ = '"';
                    q += 2;
                    continue;
                }
                break; /* the closing quote */
            }
            *o++ = *q++;
        }
        if (q + 1 >= end || q[1] != ',')
            return NULL; /* each of these fields has another after it */
        if (field == 4) {
            *o = '\0';
            return p + 1;
        }
        p = q + 2;
    }
}

/* Keeps line as h's next earlier line, as the overlay shows a text (filtered and trimmed), copied
 * into `lines` at its own length: -1 when `lines` is full. *top: where `lines` began, until the
 * current text's newest record is read (then SEEN): a line newer than it (a refresh's that was
 * dropped as the visit opened) is no earlier line, so the lines kept before it go. */
#define SEEN ((size_t)-1)
static int keep(VjoHistory *h, VjoArena *lines, const char *line, int filter, size_t *top)
{
    size_t mark = vjo_arena_mark(lines), n;
    char *f = vjo_filter_lines(lines, line, filter), *kept;
    const char *t = f ? vjo_java_trim(f) : NULL;
    vjo_arena_release(lines, mark); /* t is read, and moved down, before anything is written there */
    if (!t)
        return -1;
    if (*top != SEEN && !strcmp(t, h->line[0])) {
        vjo_arena_release(lines, *top);
        *top = SEEN;
        memset(h->line + 1, 0, sizeof(h->line[0]) * (size_t)h->n); /* past the last kept, NULL (history_show) */
        h->n = 0;
    }
    if (!*t || !strcmp(t, h->line[h->n]))
        return 0; /* nothing shown, or the line after it again */
    n = strlen(t) + 1;
    kept = (char *)vjo_arena_alloc(lines, n); /* where the filter's copy began: at or below t */
    memmove(kept, t, n);
    h->line[++h->n] = kept;
    return 1;
}

void vjo_history_scan(VjoHistory *h, VjoArena *lines, char *buf, size_t len, int whole, const char *uuid, int filter)
{
    size_t ulen = strlen(uuid), end = len, i = len, low = whole ? 0 : 1, top = vjo_arena_mark(lines);
    while (h->n < VJO_HISTORY_LINES && i-- > low) {
        char *line;
        /* A record starts with '"' after a newline (or as the file starts), and the next byte is
         * neither '"' nor ',': inside a field every '"' is doubled, and a field's closing '"' is
         * followed by ',' or a newline (the header line starts with "uuid,"). */
        if (buf[i] != '"' || (i ? buf[i - 1] != '\n' : !whole) || i + 1 >= len || buf[i + 1] == '"' ||
            buf[i + 1] == ',')
            continue;
        line = record_line(buf + i, buf + end, uuid, ulen);
        end = i;
        if (line && keep(h, lines, line, filter, &top) < 0)
            break;
    }
}
