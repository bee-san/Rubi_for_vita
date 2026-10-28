/* The config.ini golden files (#418, cleanup package P2): the parser's, the Rubi app's saves' and its install's output
 * for the fixtures host/tests/fixtures/config-*.ini, docs/config.example.ini and every record of
 * host/tests/fixtures/config-lines.txt, compared with a golden file taken before a change, so that the change's diff
 * of the golden file is the whole of its behaviour change. Each record is one line that holds only what differs from
 * a reference (the defaults' parse, the input with a save's own appended lines, an empty file's plan); a record that
 * differs in nothing is counted, not listed, so a change to it still shows (its line appears). VJO_UPDATE_GOLDEN=1 in
 * the environment writes the golden file instead of comparing (then review its diff). Included by test_core.c (the
 * parse), test_preferences.c (the saves) and test_settings_install.c (the install). */
#ifndef VJO_TEST_CONFIG_GOLDEN_H
#define VJO_TEST_CONFIG_GOLDEN_H

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

/* Every VjoConfig field, the strings first, then the numbers in the struct's order: the dump prints each one. A field
 * the lists miss fails the size check below, so a new setting is added here with its parse. */
#define VJO_GOLDEN_STR_FIELDS(X) X(local_dictionary_dir) X(local_dictionaries) X(ocr_model_dir)
#define VJO_GOLDEN_INT_FIELDS(X)                                                                                     \
    X(dictionary) X(local_dictionary_results) X(non_japanese_filter) X(japanese_only_selection) X(highlight_mode)    \
    X(frequency_badge) X(font_size_ja) X(font_size_en) X(toggle_button) X(subtitle_button) X(ocr_mode)              \
    X(text_source) X(vocr_tier) X(vocr_tier_explicit) X(ocr_settle_ms) X(ocr_settle_max_ms) X(ocr_skip_blank_lines) \
    X(ocr_lm) X(ocr_cascade_ms) X(ocr_refine_lines) X(log_file) X(anki_mining) X(ocr_helpers) X(ocr_boost_clock)  \
    X(ocr_settle_press_ms) X(anki_screenshot) X(stick_lookup_ms) X(ocr_text_gate) X(hook_sites)                    \
    X(anki_screenshot_trim_bars) X(reading_time) X(ocr_head_gate) X(hook_trust) X(ocr_vertical) X(ocr_line_fix)     \
    X(ocr_wait_hint) X(names_first) X(ocr_finder) X(ocr_spotter_refine) X(ocr_pause)
#define VJO_GOLDEN_SIZE(f) +sizeof(((VjoConfig *)0)->f)
_Static_assert(sizeof(VjoConfig) == 0 VJO_GOLDEN_STR_FIELDS(VJO_GOLDEN_SIZE) VJO_GOLDEN_INT_FIELDS(VJO_GOLDEN_SIZE) +
                                        sizeof(((VjoConfig *)0)->warnings) + sizeof(((VjoConfig *)0)->n_warnings),
               "a VjoConfig field that host/tests/config_golden.h does not dump");

/* Each test file uses some of these helpers. */
#define GOLDEN_FN static __attribute__((unused))

/* A growing text: the dump. */
typedef struct {
    char *p;
    size_t n, cap;
} GoldenOut;

GOLDEN_FN void golden_put(GoldenOut *o, const char *s, size_t n)
{
    if (o->n + n + 1 > o->cap) {
        size_t cap = (o->cap ? o->cap * 2 : 1 << 16) + n;
        char *p = (char *)realloc(o->p, cap);
        if (!p) abort();
        o->p = p;
        o->cap = cap;
    }
    memcpy(o->p + o->n, s, n);
    o->n += n;
    o->p[o->n] = '\0';
}

GOLDEN_FN void golden_puts(GoldenOut *o, const char *s) { golden_put(o, s, strlen(s)); }

GOLDEN_FN void golden_printf(GoldenOut *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
GOLDEN_FN void golden_printf(GoldenOut *o, const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof(line)) abort();
    golden_put(o, line, (size_t)n);
}

/* The n bytes at s as one printable line: \n, \r, \t, \\ and \xHH for every other byte outside printable ASCII
 * (UTF-8 too, so the golden file is ASCII and a BOM shows), and for a space at the end. */
GOLDEN_FN void golden_escape(GoldenOut *o, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '\n') golden_puts(o, "\\n");
        else if (ch == '\r') golden_puts(o, "\\r");
        else if (ch == '\t') golden_puts(o, "\\t");
        else if (ch == '\\') golden_puts(o, "\\\\");
        else if (ch == ' ' && i + 1 == n) golden_puts(o, "\\x20"); /* no trailing space in a golden file */
        else if (ch < 0x20 || ch >= 0x7F) golden_printf(o, "\\x%02X", ch);
        else golden_put(o, (const char *)&ch, 1);
    }
}

/* Every field of c, or (base not NULL) those that differ from base, then the warnings: "  name=value" lines. */
GOLDEN_FN void golden_dump_config(GoldenOut *o, const VjoConfig *c, const VjoConfig *base)
{
#define VJO_GOLDEN_STR(f)                                                                                          \
    if (!base || strcmp(c->f, base->f)) {                                                                          \
        golden_puts(o, "  " #f "=\"");                                                                             \
        golden_escape(o, c->f, strlen(c->f));                                                                      \
        golden_puts(o, "\"\n");                                                                                    \
    }
#define VJO_GOLDEN_INT(f) \
    if (!base || c->f != base->f) golden_printf(o, "  " #f "=%d\n", c->f);
    VJO_GOLDEN_STR_FIELDS(VJO_GOLDEN_STR)
    VJO_GOLDEN_INT_FIELDS(VJO_GOLDEN_INT)
#undef VJO_GOLDEN_STR
#undef VJO_GOLDEN_INT
    if (!base || c->n_warnings) golden_printf(o, "  n_warnings=%d\n", c->n_warnings);
    for (int i = 0; i < c->n_warnings && i < VJO_CONFIG_MAX_WARNINGS; i++) {
        golden_puts(o, "  warning: ");
        golden_escape(o, c->warnings[i], strnlen(c->warnings[i], sizeof(c->warnings[i])));
        golden_puts(o, "\n");
    }
}

/* The fields of c that differ from base and its warnings, on one line: " name=value" and " | w: text" (nothing when c
 * is base's parse with no warning). Returns whether it wrote anything. */
GOLDEN_FN int golden_dump_config_line(GoldenOut *o, const VjoConfig *c, const VjoConfig *base)
{
    size_t start = o->n;
#define VJO_GOLDEN_STR(f)                                                                                          \
    if (strcmp(c->f, base->f)) {                                                                                   \
        golden_puts(o, " " #f "=\"");                                                                              \
        golden_escape(o, c->f, strlen(c->f));                                                                      \
        golden_puts(o, "\"");                                                                                      \
    }
#define VJO_GOLDEN_INT(f) \
    if (c->f != base->f) golden_printf(o, " " #f "=%d", c->f);
    VJO_GOLDEN_STR_FIELDS(VJO_GOLDEN_STR)
    VJO_GOLDEN_INT_FIELDS(VJO_GOLDEN_INT)
#undef VJO_GOLDEN_STR
#undef VJO_GOLDEN_INT
    for (int i = 0; i < c->n_warnings && i < VJO_CONFIG_MAX_WARNINGS; i++) {
        golden_puts(o, " | w: ");
        golden_escape(o, c->warnings[i], strnlen(c->warnings[i], sizeof(c->warnings[i])));
    }
    if (c->n_warnings > VJO_CONFIG_MAX_WARNINGS) golden_printf(o, " | n_warnings=%d", c->n_warnings);
    return o->n != start;
}

/* A file's bytes (NUL-terminated, malloc'd), or NULL. */
GOLDEN_FN char *golden_read(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *p = NULL;
    size_t n = 0, cap = 0;
    if (!f) return NULL;
    for (;;) {
        if (n + 4096 + 1 > cap) {
            cap = cap * 2 + 4096 + 1;
            char *q = (char *)realloc(p, cap);
            if (!q) abort();
            p = q;
        }
        size_t got = fread(p + n, 1, 4096, f);
        n += got;
        if (got < 4096) break;
    }
    fclose(f);
    p[n] = '\0';
    *len = n;
    return p;
}

/* The records of host/tests/fixtures/config-lines.txt: one a line, its \n, \r, \t, \\ and \xHH unescaped (a record
 * can hold several config.ini lines); empty lines and "//" comments are no records. Calls each(record, n, arg). */
GOLDEN_FN int golden_records(const char *path, void (*each)(const char *record, size_t n, void *arg), void *arg)
{
    size_t len;
    char *text = golden_read(path, &len);
    if (!text) return -1;
    int count = 0;
    char *record = (char *)malloc(len + 1);
    if (!record) abort();
    for (size_t i = 0; i < len;) {
        size_t e = i, n = 0;
        while (e < len && text[e] != '\n') e++;
        if (e > i && !(e - i >= 2 && text[i] == '/' && text[i + 1] == '/')) {
            for (size_t k = i; k < e; k++) {
                char ch = text[k];
                if (ch == '\\' && k + 1 < e) {
                    char x = text[++k];
                    if (x == 'n') ch = '\n';
                    else if (x == 'r') ch = '\r';
                    else if (x == 't') ch = '\t';
                    else if (x == 'x' && k + 2 < e) {
                        char hex[3] = {text[k + 1], text[k + 2], 0};
                        ch = (char)strtol(hex, NULL, 16);
                        k += 2;
                    } else ch = x;
                }
                record[n++] = ch;
            }
            each(record, n, arg);
            count++;
        }
        i = e + 1;
    }
    free(record);
    free(text);
    return count;
}

/* Records longer than the parser's buffers (a line 1,023 bytes, a key 63, a value 511, a path 159), made here: each
 * one to each(record, n, arg). */
GOLDEN_FN void golden_long_records(void (*each)(const char *record, size_t n, void *arg), void *arg)
{
    static const struct { const char *head; char fill; size_t n; } longer[] = {
        {"jpdb_api_key = ", 'S', 1500}, {"font_size_ja = ", '9', 1500}, {"frequency_filter = ", 'x', 600},
        {"anki_audio_url = https://", 'x', 1500}, {"dictionary = ", 'x', 600}, {"ocr_backend = ", 'x', 600},
        {"log_host = ", 'x', 600}, {"vocr_model = ", 'x', 600}, {"bogus = ", 'x', 1500},
        {"local_dictionaries = ", 'a', 510}, {"local_dictionaries = ", 'a', 511}, {"local_dictionaries = ", 'a', 512},
        {"local_dictionary_dir = ux0:", 'd', 155}, {"local_dictionary_dir = ux0:", 'd', 156},
        {"local_dictionary_dir = ux0:", 'd', 157}, {"ocr_model_dir = ux0:", 'd', 156},
        {"", 'k', 63}, {"", 'k', 64}, {"", 'k', 70}, {"", 'k', 1100}};
    static char record[2048];
    for (size_t i = 0; i < sizeof(longer) / sizeof(longer[0]); i++) {
        size_t h = strlen(longer[i].head);
        memcpy(record, longer[i].head, h);
        memset(record + h, longer[i].fill, longer[i].n);
        size_t n = h + longer[i].n;
        if (!h) n += (size_t)snprintf(record + n, sizeof(record) - n, " = 1"); /* a key of that many bytes */
        each(record, n, arg);
    }
}

/* FNV-1a, 64 bits: a long output's fingerprint in a golden file. */
GOLDEN_FN unsigned long long golden_hash(const char *s, size_t n)
{
    unsigned long long h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 0x100000001b3ull;
    return h;
}

/* golden_escape, but data longer than 240 bytes as its first 80 bytes, its length and its hash (the long records). */
#define VJO_GOLDEN_LONG 240
GOLDEN_FN void golden_escape_short(GoldenOut *o, const char *s, size_t n)
{
    if (n <= VJO_GOLDEN_LONG) {
        golden_escape(o, s, n);
        return;
    }
    golden_escape(o, s, 80);
    golden_printf(o, "...[%zu bytes, fnv1a %016llx]", n, golden_hash(s, n));
}

/* The lines of a (an, the input) that b (bn, a save's output) drops, as "    - <line>", and those it adds, as
 * "    + <line>", each with its line number in its own text: a save keeps every other line in its order, so this walk
 * (a line kept when it is the next line of b, else dropped when no later line of b is it, else b's lines up to it
 * are added) is the whole difference, and b can be rebuilt from a and it. Each line with its '\n', escaped. */
GOLDEN_FN size_t golden_next_line(const char *s, size_t n, size_t at)
{
    while (at < n && s[at] != '\n') at++;
    return at < n ? at + 1 : n;
}
GOLDEN_FN void golden_line_diff_as(GoldenOut *o, const char *a, size_t an, const char *b, size_t bn, int inline_)
{
    size_t i = 0, j = 0;
    unsigned ai = 1, bj = 1;
    while (i < an || j < bn) {
        size_t ie = golden_next_line(a, an, i), je = golden_next_line(b, bn, j);
        if (i < an && j < bn && ie - i == je - j && !memcmp(a + i, b + j, ie - i)) {
            i = ie, j = je, ai++, bj++;
            continue;
        }
        int later = 0;
        for (size_t k = j, ke; i < an && k < bn; k = ke) { /* is a's line a later line of b? */
            ke = golden_next_line(b, bn, k);
            if (ke - k == ie - i && !memcmp(b + k, a + i, ie - i)) { later = 1; break; }
        }
        if (i < an && !later) {
            golden_printf(o, inline_ ? " -%u " : "    -%u ", ai);
            golden_escape_short(o, a + i, ie - i);
            if (!inline_) golden_puts(o, "\n");
            i = ie, ai++;
        } else {
            golden_printf(o, inline_ ? " +%u " : "    +%u ", bj);
            golden_escape_short(o, b + j, je - j);
            if (!inline_) golden_puts(o, "\n");
            j = je, bj++;
        }
    }
}
/* One entry a line (the real files' saves, the empty file's plan). */
GOLDEN_FN void golden_line_diff(GoldenOut *o, const char *a, size_t an, const char *b, size_t bn)
{
    golden_line_diff_as(o, a, an, b, bn, 0);
}
/* All on the line being written (a record's). */
GOLDEN_FN void golden_line_diff_inline(GoldenOut *o, const char *a, size_t an, const char *b, size_t bn)
{
    golden_line_diff_as(o, a, an, b, bn, 1);
}

/* The line of text (len bytes) that starts at or before byte at: into out, cut to fit. */
GOLDEN_FN void golden_line_at(const char *text, size_t len, size_t at, char *out, size_t cap)
{
    size_t b = at < len ? at : len, e;
    while (b > 0 && text[b - 1] != '\n') b--;
    for (e = b; e < len && text[e] != '\n'; e++) {}
    snprintf(out, cap, "%.*s", (int)(e - b), text + b);
}

/* 0 when out equals the golden file at path; with VJO_UPDATE_GOLDEN set, writes it there instead (1). On a difference
 * (-1), why (GOLDEN_WHY bytes) is the first differing line: its number, the golden file's text and this run's. */
#define GOLDEN_WHY 1200
GOLDEN_FN int golden_check(const GoldenOut *o, const char *path, char *why, size_t cap)
{
    const char *update = getenv("VJO_UPDATE_GOLDEN");
    if (update && *update) {
        FILE *f = fopen(path, "wb");
        if (!f || fwrite(o->p, 1, o->n, f) != o->n) abort();
        fclose(f);
        return 1;
    }
    size_t len = 0, i = 0;
    char *want = golden_read(path, &len);
    if (want && len == o->n && !memcmp(want, o->p, len)) {
        free(want);
        return 0;
    }
    int no = 1;
    for (; want && i < len && i < o->n && want[i] == o->p[i]; i++)
        if (want[i] == '\n') no++;
    char was[300] = "(no golden file)", now[300];
    if (want) golden_line_at(want, len, i, was, sizeof(was));
    golden_line_at(o->p, o->n, i, now, sizeof(now));
    snprintf(why, cap, "%.400s, line %d: golden \"%.300s\", now \"%.300s\" (VJO_UPDATE_GOLDEN=1 rewrites it)", path, no,
             was, now);
    free(want);
    return -1;
}

#endif
