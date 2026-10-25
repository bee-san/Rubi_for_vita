#include "history_lines.h"

#include <string.h>

#include "port.h"
#include "reading_chars.h"
#include "textfilter.h"
#include "utf.h"

/* After the line: time, platform, title ID, engine. Respect quoted commas and doubled quotes.
 * Old records still supply text even if they have no readable engine. */
static uint8_t record_origin(const char *p, const char *end)
{
    for (int field = 0; field < 3; field++) {
        int quoted = 0;
        for (; p < end; p++) {
            if (*p == '"') {
                if (quoted && p + 1 < end && p[1] == '"') { p++; continue; }
                quoted = !quoted;
            } else if (*p == ',' && !quoted) { p++; break; }
        }
    }
    if (end - p >= 7 && !memcmp(p, "\"vocr\",", 7)) return VJO_LINE_OCR;
    if (end - p >= 7 && !memcmp(p, "\"hook\",", 7)) return VJO_LINE_HOOK;
    return VJO_LINE_UNKNOWN;
}

/* The line of the record that starts at p (its '"') and ends before end, when the record is game
 * uuid's (its first field, the closing quote and comma included; any game's when ulen is 0): its
 * fourth field, unescaped in place and NUL-terminated; NULL otherwise, and when the record is cut
 * before the time that follows that field (its first digit). shell/history.c quotes each of the first
 * four fields, doubles a '"' inside one and writes the time, unquoted, after them. */
static char *record_line(char *p, char *end, const char *uuid, size_t ulen, uint8_t *origin, VjoReadingTime *time, VjoReadingScope *scope)
{
    if ((size_t)(end - p) < ulen + 2 || memcmp(p + 1, uuid, ulen))
        return NULL;
    uint32_t game = 2166136261u;
    for (int field = 1;; field++) {
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
        if (field == 1 && time)
            for (char *c = p + 1; c < o; c++) game = (game ^ (unsigned char)*c) * 16777619u;
        if (q + 1 >= end || q[1] != ',')
            return NULL; /* each of these fields has another after it */
        if (field == 4) {
            /* and the line's field the time: a record a power loss cut, run into the next boot's
             * append, has another field's '"' there (audit-line-history F1) */
            if (q + 2 >= end || q[2] < '0' || q[2] > '9')
                return NULL;
            *o = '\0';
            if (origin) *origin = record_origin(q + 2, end);
            if (time) {
                /* Rubi writes Unix seconds and exactly six fractional digits. Invalid/missing
                 * times break the interval, but do not hide the line's character count. */
                char *c = q + 2;
                uint64_t us = 0;
                int digits = 0;
                while (c < end && *c >= '0' && *c <= '9' && digits++ < 12) us = us * 10 + *c++ - '0';
                if (c < end && *c++ == '.' && digits <= 12) {
                    for (digits = 0; digits < 6 && c < end && *c >= '0' && *c <= '9'; digits++)
                        us = us * 10 + *c++ - '0';
                    if (digits != 6 || c >= end || *c != ',') us = 0;
                } else us = 0;
                vjo_reading_scope_add(time, scope, us, game, vjo_line_characters(p + 1, (size_t)(o - p - 1)));
            }
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
static int keep(VjoHistory *h, VjoArena *lines, const char *line, uint8_t origin, int filter, size_t *top)
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
    h->origin[h->n] = origin;
    return 1;
}

/* Whether a record starts at buf[i]: a '"' after a newline (or as the file starts: buf[0] is read only
 * when the window is the whole file, the callers' `low`), and the next byte is neither '"' nor ',':
 * inside a field every '"' is doubled, and a field's closing '"' is followed by ',' or a newline (the
 * header line starts with "uuid,"). */
static int record_starts(const char *buf, size_t i, size_t len)
{
    return buf[i] == '"' && (!i || buf[i - 1] == '\n') && i + 1 < len && buf[i + 1] != '"' && buf[i + 1] != ',';
}

void vjo_history_scan(VjoHistory *h, VjoArena *lines, char *buf, size_t len, int whole, const char *uuid, int filter)
{
    size_t ulen = strlen(uuid), end = len, i = len, low = whole ? 0 : 1, top = vjo_arena_mark(lines);
    while (h->n < VJO_HISTORY_LINES && i-- > low) {
        char *line;
        uint8_t origin = VJO_LINE_UNKNOWN;
        if (buf[i] != '"' || !record_starts(buf, i, len)) /* (most bytes: no call) */
            continue;
        line = record_line(buf + i, buf + end, uuid, ulen, &origin, NULL, NULL);
        end = i;
        if (line && keep(h, lines, line, origin, filter, &top) < 0)
            break;
    }
}

uint32_t vjo_line_characters(const char *s, size_t len)
{
    return vjo_reading_characters(s, len);
}

void vjo_reading_time_add(VjoReadingTime *t, uint64_t us, uint32_t game, uint32_t chars)
{
    uint64_t gap = us - t->last_us;
    if (t->last_us && us && game == t->last_game && gap && gap <= 15000000) {
        t->active_us += gap;
        t->timed_chars += t->last_chars;
    }
    t->last_us = us;
    t->last_game = game;
    t->last_chars = chars;
}

void vjo_reading_scope_add(VjoReadingTime *t, VjoReadingScope *scope,
                           uint64_t us, uint32_t game, uint32_t chars)
{
    uint32_t selected = 2166136261u;
    if (scope && scope->uuid[0]) {
        for (const char *p = scope->uuid; *p; p++) selected = (selected ^ (unsigned char)*p) * 16777619u;
        if (game != selected) { t->last_us = 0; t->last_chars = 0; return; }
    }
    uint64_t before = t->active_us, timed = t->timed_chars, prev = t->last_us;
    vjo_reading_time_add(t, us, game, chars);
    if (scope && us >= scope->start_us && us < scope->end_us) {
        scope->chars += chars;
        /* Midnight breaks the daily interval, rather than crediting yesterday's line today. */
        if (prev >= scope->start_us) {
            scope->active_us += t->active_us - before;
            scope->timed_chars += t->timed_chars - timed;
        }
    }
}

uint32_t vjo_history_count_scope(char *buf, size_t len, int whole, int cut, size_t *first, size_t *last,
                           VjoReadingTime *time, VjoReadingScope *scope)
{
    uint32_t n = 0;
    size_t start = len;
    *first = *last = len;
    /* Forward order preserves the previous line's time across windows. A record is parsed only
     * after its end is known; a cut newest record waits for the next window, as before. */
    for (size_t i = (size_t)!whole; i <= len; i++) {
        if (i < len && (buf[i] != '"' || !record_starts(buf, i, len))) continue;
        if (start < len && (i < len || !cut)) {
            char *line = record_line(buf + start, buf + i, "", 0, NULL, time, scope);
            if (line) n += time ? time->last_chars : vjo_line_characters(line, strlen(line));
            else if (time) time->last_us = 0; /* damaged records break the interval */
        }
        if (i < len) {
            if (*first == len) *first = i;
            *last = start = i;
        }
    }
    return n;
}

uint32_t vjo_history_count(char *buf, size_t len, int whole, int cut, size_t *first, size_t *last,
                           VjoReadingTime *time)
{
    return vjo_history_count_scope(buf, len, whole, cut, first, last, time, NULL);
}

uint32_t vjo_history_chars(char *buf, size_t len, int whole, int cut, size_t *first, size_t *last)
{
    return vjo_history_count(buf, len, whole, cut, first, last, NULL);
}

char *vjo_history_number(char *buf, size_t cap, uint64_t n)
{
    static const char format[] VJO_DATA_TABLE(history_total_number) = "%llu"; /* (the Shell's code pages are full) */
    int len = vjo_snprintf(buf, cap, format, (unsigned long long)n), commas = (len - 1) / 3, digits = 0;
    buf[len + commas] = '\0'; /* a comma between every three digits, from the right: 1,234,567 */
    for (int i = len - 1; commas; i--) {
        buf[i + commas] = buf[i];
        if (++digits == 3) {
            buf[i + --commas] = ',';
            digits = 0;
        }
    }
    return buf;
}
