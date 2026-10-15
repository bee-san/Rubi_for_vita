/* settings/main.c's fit_line and wrapped_text as they were before
 * settings/text_layout.c (main 7ed16d8), verbatim but for measuring with
 * ref_measure and "drawing" into ref_row[]: the reference that the cached
 * layouts are tested against (test_settings_layout.c, test_settings_games.c).
 * Host-only. */
#ifndef VJO_TEST_SETTINGS_WRAP_REFERENCE_H
#define VJO_TEST_SETTINGS_WRAP_REFERENCE_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../core/utf.h"

static int (*ref_measure)(float pixels, const char *s);
typedef struct { int x, y; char text[512]; } RefRow;
static RefRow ref_row[16];
static int ref_rows;
static int ref_text_width(float pixels, const char *s) { return ref_measure(pixels, s); }
static void ref_text(int x, int y, unsigned color, float pixels, const char *s)
{
    (void)color; (void)pixels;
    if (ref_rows < 16) {
        ref_row[ref_rows].x = x;
        ref_row[ref_rows].y = y;
        snprintf(ref_row[ref_rows].text, sizeof(ref_row[0].text), "%s", s);
    }
    ref_rows++;
}

#define text_width ref_text_width
#define text ref_text
#define fit_line ref_fit_line
#define wrapped_text ref_wrapped_text_draw
/* ---- verbatim from main 7ed16d8 settings/main.c ---- */
/* A line ends only at a decoded UTF-8 boundary. PVF advances, rather than
 * byte counts, decide how many Japanese/Latin glyphs fit in the card. */
static size_t fit_line(const char *s, float pixels, int width, char out[512])
{
    size_t len = strlen(s), end = 0, space = 0;
    for (size_t i = 0; i < len;) {
        size_t begin = i;
        uint32_t cp = vjo_utf8_next(s, len, &i);
        if (cp == '\n' || i > 508) break; /* Reserve the final "..." and NUL. */
        memcpy(out, s, i);
        out[i] = 0;
        if (text_width(pixels, out) > width) break;
        end = i;
        if (cp == ' ') space = begin;
    }
    if (s[end] && s[end] != '\n' && space) end = space;
    memcpy(out, s, end);
    out[end] = 0;
    return end;
}

static void wrapped_text(int x, int y, int width, unsigned color,
                         float pixels, int line_height, int lines, const char *s)
{
    char line[512];
    for (int row = 0; row < lines && *s; row++) {
        while (*s == ' ' || *s == '\r') s++;
        size_t end = fit_line(s, pixels, width, line);
        const char *remaining = s + end;
        while (*remaining == ' ' || *remaining == '\r' || *remaining == '\n') remaining++;
        if (row == lines - 1 && *remaining) {
            end = fit_line(s, pixels, width - text_width(pixels, "..."), line);
            strcat(line, "...");
        }
        text(x, y + row * line_height, color, pixels, line);
        if (!end && *s != '\n') break; /* Even one glyph can exceed a tiny region. */
        s += end;
        if (*s == '\n') s++;
    }
}
/* ---- end of the verbatim part ---- */
#undef text_width
#undef text
#undef fit_line
#undef wrapped_text

/* The rows the old wrapped_text drew for s, into ref_row[0, ref_rows). */
static inline void ref_wrapped_text(int x, int y, int width, float pixels, int line_height, int lines, const char *s)
{
    ref_rows = 0;
    ref_wrapped_text_draw(x, y, width, 0, pixels, line_height, lines, s);
}
#endif
