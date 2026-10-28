#include "app_prefs.h"

#include <stdio.h>
#include <string.h>

#include "preferences.h"

#define BACKUP VJO_APP_PREFS_PATH ".bak"
#define VALUE_MAX 32 /* a value's name, NUL included */

typedef struct {
    const char *key;             /* lowercase: a save compares it with each line's key, lowercased */
    size_t field;                /* offsetof(VjoAppPrefs, ...): an int, the value's index */
    const char *const *values;   /* the values' names, NULL-terminated */
} Row;

static const char *const duplicates[] = {"tag", "skip", "keep", NULL}; /* VJO_DUPLICATES_*'s order */

static const char *const card_types[] = {"word", "sentence", "word_sentence", "click", NULL}; /* VJO_CARD_*'s */
static const char *const off_on[] = {"off", "on", NULL};

static const Row rows[] = {
    {"anki_duplicates", offsetof(VjoAppPrefs, anki_duplicates), duplicates},
    {"anki_card_type", offsetof(VjoAppPrefs, anki_card_type), card_types},
    {"anki_game_tag", offsetof(VjoAppPrefs, anki_game_tag), off_on},
    {"anki_sentence_readings", offsetof(VjoAppPrefs, anki_sentence_readings), off_on},
};
#define ROWS ((int)(sizeof(rows) / sizeof(rows[0])))
/* The keys of settings the app had and has no more: a line of one is ignored as any unknown key is, and a save drops
 * it. stats_daily_goal: the Stats tab's Daily goal (#312), removed with the tab by the owner (2026-10-28). */
static const char *const removed[] = {"stats_daily_goal", NULL};

static int *field(VjoAppPrefs *p, const Row *r)
{
    return (int *)((char *)p + r->field);
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* The n bytes at s are word, in any case. */
static int same(const char *s, size_t n, const char *word)
{
    size_t i = 0;
    for (; i < n && word[i]; i++)
        if (lower((unsigned char)s[i]) != (unsigned char)word[i])
            return 0;
    return i == n && !word[i];
}

static const Row *row_named(const char *s, size_t n)
{
    for (int i = 0; i < ROWS; i++)
        if (same(s, n, rows[i].key))
            return &rows[i];
    return NULL;
}

/* The index of the value named by the n bytes at s, or -1. */
static int value_index(const Row *r, const char *s, size_t n)
{
    for (int v = 0; r->values[v]; v++)
        if (same(s, n, r->values[v]))
            return v;
    return -1;
}

static int values_of(const Row *r)
{
    int n = 0;
    while (r->values[n])
        n++;
    return n;
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

void vjo_app_prefs_parse(VjoAppPrefs *p, const char *text, size_t len)
{
    size_t i = 0;
    while (i < len) {
        const char *line = text + i, *eq;
        size_t n = 0, s, k, v, e;
        const Row *r;
        int value;
        while (i + n < len && line[n] != '\n')
            n++;
        i += n + 1;
        s = vjo_config_line_start(line, n); /* after spaces, tabs, CRs and BOMs, as config.ini's lines */
        if (s == n || line[s] == ';' || line[s] == '#' || !(eq = memchr(line + s, '=', n - s)))
            continue;
        k = (size_t)(eq - line);
        while (k > s && is_space(line[k - 1]))
            k--;
        v = (size_t)(eq - line) + 1;
        while (v < n && is_space(line[v]))
            v++;
        /* the value ends at a comment behind a space, or at the line's end */
        for (e = v; e < n && !((line[e] == ';' || line[e] == '#') && e > v && is_space(line[e - 1])); e++)
            ;
        while (e > v && is_space(line[e - 1]))
            e--;
        if ((r = row_named(line + s, k - s)) && (value = value_index(r, line + v, e - v)) >= 0)
            *field(p, r) = value;
    }
}

/* text (len bytes, NUL-terminated) without the lines of removed keys, in place: its length now. */
static size_t drop_removed(char *text, size_t len)
{
    size_t i = 0, used = 0;
    if (len >= 3 && !memcmp(text, "\xEF\xBB\xBF", 3))
        i = used = 3; /* a BOM (Notepad) stays, as the patch keeps it */
    while (i < len) {
        const char *line = text + i, *eq;
        size_t n = 0, s, k;
        int drop = 0;
        while (i + n < len && line[n] != '\n')
            n++;
        if (i + n < len)
            n++; /* the line break */
        s = vjo_config_line_start(line, n);
        if (s < n && line[s] != ';' && line[s] != '#' && (eq = memchr(line + s, '=', n - s))) {
            k = (size_t)(eq - line);
            while (k > s && is_space(line[k - 1]))
                k--;
            for (int r = 0; removed[r] && !drop; r++)
                drop = same(line + s, k - s, removed[r]);
        }
        if (!drop) {
            memmove(text + used, line, n);
            used += n;
        }
        i += n;
    }
    text[used] = '\0';
    return used;
}

/* app.ini's text, else its backup's: 0 (*primary: app.ini's), 1 neither exists (*text ""), -1 unreadable. */
static int current_text(VjoArena *a, char **text, size_t *bytes, int *primary)
{
    int rc = vjo_prefs_read_file(VJO_APP_PREFS_PATH, a, text, bytes);
    *primary = rc == 0;
    if (rc != 1)
        return rc;
    rc = vjo_prefs_read_file(BACKUP, a, text, bytes);
    if (rc == 1) {
        *text = (char *)"";
        *bytes = 0;
    }
    return rc;
}

int vjo_app_prefs_load(VjoAppPrefs *p, VjoArena *a)
{
    size_t mark = vjo_arena_mark(a), bytes = 0;
    char *text = NULL;
    int primary, rc;
    vjo_app_prefs_defaults(p);
    rc = current_text(a, &text, &bytes, &primary);
    if (rc >= 0)
        vjo_app_prefs_parse(p, text, bytes);
    vjo_arena_release(a, mark);
    return rc < 0 ? -1 : rc == 0 && !primary ? 1 : 0;
}

int vjo_app_prefs_save(const char *const *keys, const int *values, int n, VjoArena *a)
{
    const char *k[8], *v[8];
    size_t mark, bytes = 0, cap;
    char *text = NULL, *next;
    int primary, len, rc = -1;
    if (n < 1 || n > 8)
        return -1;
    for (int i = 0; i < n; i++) {
        const Row *r = keys[i] ? row_named(keys[i], strlen(keys[i])) : NULL;
        if (!r || values[i] < 0 || values[i] >= values_of(r))
            return -1;
        for (int j = 0; j < i; j++)
            if (k[j] == r->key) /* the same setting twice */
                return -1;
        k[i] = r->key;
        v[i] = r->values[values[i]];
    }
    mark = vjo_arena_mark(a);
    if (current_text(a, &text, &bytes, &primary) < 0)
        goto done; /* never rewrite a file that could not be read */
    cap = bytes + bytes / 4 + (size_t)n * (40 + VALUE_MAX + 8) + 8;
    next = (char *)vjo_arena_alloc(a, cap);
    len = next ? vjo_ini_patch_keys(text, bytes, k, v, n, next, cap) : -1;
    if (len >= 0)
        len = (int)drop_removed(next, (size_t)len);
    if (len < 0 || len > VJO_PREFERENCES_TEXT_MAX)
        goto done;
    rc = vjo_prefs_publish_file(VJO_APP_PREFS_PATH, next, len, primary);
done:
    vjo_arena_release(a, mark);
    return rc;
}

const char *vjo_app_prefs_key(int i)
{
    return i >= 0 && i < ROWS ? rows[i].key : NULL;
}

const char *vjo_app_prefs_value(const char *key, int v)
{
    const Row *r = key ? row_named(key, strlen(key)) : NULL;
    return r && v >= 0 && v < values_of(r) ? r->values[v] : NULL;
}

int vjo_app_prefs_get(const VjoAppPrefs *p, const char *key)
{
    const Row *r = key ? row_named(key, strlen(key)) : NULL;
    return r ? *field((VjoAppPrefs *)p, r) : -1;
}
