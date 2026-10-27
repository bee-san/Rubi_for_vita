#include "history_read.h"

#include <string.h>

/* core/history_lines.c record_starts, with its callers' low bound (a window that is not whole starts at buf[1]). */
int vjo_history_record_start(const char *buf, size_t i, size_t len, int whole)
{
    if (i >= len || (!whole && !i))
        return 0;
    return buf[i] == '"' && (!i || buf[i - 1] == '\n') && i + 1 < len && buf[i + 1] != '"' && buf[i + 1] != ',';
}

size_t vjo_history_next_start(const char *buf, size_t from, size_t len, int whole)
{
    for (size_t i = from || whole ? from : 1; i < len; i++)
        if (buf[i] == '"' && vjo_history_record_start(buf, i, len, whole)) /* (most bytes: no call) */
            return i;
    return len;
}

size_t vjo_history_prev_start(const char *buf, size_t before, size_t len, int whole)
{
    size_t low = whole ? 0 : 1;
    if (before > len)
        before = len;
    for (size_t i = before; i-- > low;)
        if (buf[i] == '"' && vjo_history_record_start(buf, i, len, whole))
            return i;
    return len;
}

/* A quoted cell from p (its '"'), unescaped in place up to its closing quote: the closing quote's position, or
 * NULL when the cell does not end before `end`. *out and *len: the cell's text and its length, NUL-terminated (the
 * NUL goes where the unescaped text ends: at the closing quote or before it). */
static char *quoted(char *p, char *end, char **out, uint32_t *len)
{
    char *q = p + 1, *o = q;
    for (;;) {
        if (q >= end)
            return NULL;
        if (*q == '"') {
            if (q + 1 < end && q[1] == '"') {
                *o++ = '"';
                q += 2;
                continue;
            }
            break;
        }
        *o++ = *q++;
    }
    *o = '\0';
    *out = p + 1;
    *len = (uint32_t)(o - (p + 1));
    return q;
}

/* The time as record_line reads it (c: the cell's first byte, a digit). */
static uint64_t time_us(const char *c, const char *end)
{
    uint64_t us = 0;
    int digits = 0;
    while (c < end && *c >= '0' && *c <= '9' && digits++ < 12)
        us = us * 10 + (uint64_t)(*c++ - '0');
    if (c < end && *c++ == '.' && digits <= 12) {
        for (digits = 0; digits < 6 && c < end && *c >= '0' && *c <= '9'; digits++)
            us = us * 10 + (uint64_t)(*c++ - '0');
        if (digits != 6 || c >= end || *c != ',')
            us = 0;
    } else {
        us = 0;
    }
    return us;
}

int vjo_history_row(char *p, char *end, int want, VjoHistoryRow *row)
{
    memset(row, 0, sizeof(*row));
    if (want < 4)
        want = 4;
    if (want > VJO_HISTORY_CELLS)
        want = VJO_HISTORY_CELLS;
    if (!p || !end || end - p < 2)
        return -1;
    /* record_line: the first four cells quoted, each followed by ',', the fourth's ',' by the time's first digit */
    for (int k = 0; k < 4; k++) {
        char *q;
        if (p >= end || *p != '"' || !(q = quoted(p, end, &row->cell[k], &row->len[k])) || q + 1 >= end ||
            q[1] != ',' || (k == 3 && (q + 2 >= end || q[2] < '0' || q[2] > '9'))) {
            memset(row, 0, sizeof(*row));
            return -1;
        }
        p = q + 2;
    }
    row->uuid = row->cell[0];
    row->id = row->cell[1];
    row->name = row->cell[2];
    row->line = row->cell[3];
    row->time_us = time_us(p, end);
    row->cells = 4;
    /* The cells after them, quoted or bare, to the record's line break. A cell that the window's end cuts (no
     * closing quote, or no separator after a bare one) is not kept: nothing is written at or past `end`. */
    while (row->cells < want && p < end) {
        char *q, sep;
        if (*p == '"') {
            if (!(q = quoted(p, end, &row->cell[row->cells], &row->len[row->cells])))
                break;
            sep = q + 1 < end ? q[1] : '\0';
            q++;
        } else {
            for (q = p; q < end && *q != ',' && *q != '\n' && *q != '\r'; q++)
                ;
            if (q >= end)
                break;
            sep = *q;
            *q = '\0';
            row->cell[row->cells] = p;
            row->len[row->cells] = (uint32_t)(q - p);
        }
        row->cells++;
        if (sep != ',')
            break; /* the record's end (or a malformed cell after a quote) */
        p = q + 1;
    }
    return 0;
}

uint32_t vjo_history_game(const char *uuid, size_t len)
{
    uint32_t game = 2166136261u;
    for (size_t i = 0; i < len; i++)
        game = (game ^ (unsigned char)uuid[i]) * 16777619u;
    return game;
}

int vjo_history_column(const char *header, size_t len, const char *name)
{
    size_t i = 0, n = strlen(name);
    int column = 0;
    if (len >= 3 && !memcmp(header, "\xEF\xBB\xBF", 3))
        i = 3; /* a byte-order mark an editor added */
    while (i < len && header[i] != '\n' && header[i] != '\r') {
        size_t start = i;
        while (i < len && header[i] != ',' && header[i] != '\n' && header[i] != '\r')
            i++;
        if (i - start == n && !memcmp(header + start, name, n))
            return i < len ? column : -1; /* a name the buffer's end cuts may be a longer one: not known */
        if (i < len && header[i] == ',')
            i++;
        column++;
    }
    return -1;
}
