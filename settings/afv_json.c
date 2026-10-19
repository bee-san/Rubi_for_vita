/* The interface's rules for a JSON file (afv_json.h). */
#include "afv_json.h"

#include <stdlib.h>
#include <string.h>

/* Valid UTF-8 (RFC 3629: no overlong form, no surrogate, nothing above U+10FFFF) and no NUL. */
static int utf8_valid(const unsigned char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned c = s[i], need, min, cp;
        if (c < 0x80) {
            if (!c)
                return 0;
            i++;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF)
            need = 1, min = 0x80, cp = c & 0x1F;
        else if (c >= 0xE0 && c <= 0xEF)
            need = 2, min = 0x800, cp = c & 0x0F;
        else if (c >= 0xF0 && c <= 0xF4)
            need = 3, min = 0x10000, cp = c & 0x07;
        else
            return 0;
        if (n - i <= need)
            return 0;
        for (unsigned k = 1; k <= need; k++) {
            if ((s[i + k] & 0xC0) != 0x80)
                return 0;
            cp = cp << 6 | (s[i + k] & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return 0;
        i += need + 1;
    }
    return 1;
}

/* Objects and arrays nested at most VJO_AFV_JSON_DEPTH levels (outside strings). */
static int depth_ok(const char *s, size_t n)
{
    int depth = 0, in_string = 0;
    for (size_t i = 0; i < n; i++) {
        if (in_string) {
            if (s[i] == '\\')
                i++;
            else if (s[i] == '"')
                in_string = 0;
        } else if (s[i] == '"') {
            in_string = 1;
        } else if (s[i] == '{' || s[i] == '[') {
            if (++depth > VJO_AFV_JSON_DEPTH)
                return 0;
        } else if (s[i] == '}' || s[i] == ']') {
            depth--;
        }
    }
    return 1;
}

static int hex4(const char *s, unsigned *v)
{
    *v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0)
            return -1;
        *v = *v << 4 | (unsigned)d;
    }
    return 0;
}

/* Every string's escapes: no \u0000, no lone surrogate (a high one not followed by a low one, a low
 * one not after a high one). */
static int escapes_ok(const VjoJson *j)
{
    for (int t = 0; t < j->n; t++) {
        const char *s = j->js + j->t[t].start;
        int n = j->t[t].end - j->t[t].start;
        if (j->t[t].type != JSMN_STRING)
            continue;
        for (int i = 0; i + 1 < n; i++) {
            unsigned u, lo;
            if (s[i] != '\\')
                continue;
            if (s[i + 1] != 'u') {
                i++;
                continue;
            }
            if (i + 5 >= n || hex4(s + i + 2, &u) < 0 || !u || (u >= 0xDC00 && u <= 0xDFFF))
                return 0;
            if (u >= 0xD800 && u <= 0xDBFF && (i + 11 >= n || s[i + 6] != '\\' || s[i + 7] != 'u' ||
                                               hex4(s + i + 8, &lo) < 0 || lo < 0xDC00 || lo > 0xDFFF))
                return 0;
            i += u >= 0xD800 && u <= 0xDBFF ? 11 : 5;
        }
    }
    return 1;
}

typedef struct {
    const char *s; /* the key, unescaped */
    size_t n;
} Key;

static int key_cmp(const void *x, const void *y)
{
    const Key *a = (const Key *)x, *b = (const Key *)y;
    if (a->n != b->n)
        return a->n < b->n ? -1 : 1;
    return memcmp(a->s, b->s, a->n);
}

/* No object gives a key twice (the keys compared unescaped). */
static int keys_unique(VjoArena *a, const VjoJson *j)
{
    for (int o = 0; o < j->n; o++) {
        size_t mark = vjo_arena_mark(a);
        int size = j->t[o].size, k, ok = 1;
        Key *keys;
        if (j->t[o].type != JSMN_OBJECT || size < 2)
            continue;
        if (!(keys = (Key *)vjo_arena_alloc(a, sizeof(Key) * (size_t)size)))
            return 0;
        k = o + 1;
        for (int i = 0; i < size; i++, k = vjo_json_skip(j, k + 1)) {
            if (!(keys[i].s = vjo_json_str(a, j, k))) {
                vjo_arena_release(a, mark);
                return 0;
            }
            keys[i].n = strlen(keys[i].s);
        }
        qsort(keys, (size_t)size, sizeof(Key), key_cmp);
        for (int i = 1; ok && i < size; i++)
            ok = key_cmp(&keys[i - 1], &keys[i]) != 0;
        vjo_arena_release(a, mark);
        if (!ok)
            return 0;
    }
    return 1;
}

int vjo_afv_json_strict(VjoArena *a, const char *json, size_t len, size_t max, VjoJson *j)
{
    size_t mark = vjo_arena_mark(a);
    if (!len || len > max || !utf8_valid((const unsigned char *)json, len) || !depth_ok(json, len) ||
        vjo_json_parse(a, json, len, j) < 0 || j->n < 1 || !vjo_json_is_type(j, 0, JSMN_OBJECT) ||
        vjo_json_skip(j, 0) != j->n || !escapes_ok(j) || !keys_unique(a, j)) {
        vjo_arena_release(a, mark);
        return -1;
    }
    return 0;
}

int vjo_afv_json_version(const VjoJson *j, int64_t *v)
{
    int t = vjo_json_obj_get(j, 0, "v"), n;
    const char *s;
    if (t < 0 || !vjo_json_is_type(j, t, JSMN_PRIMITIVE))
        return -1;
    s = j->js + j->t[t].start;
    n = j->t[t].end - j->t[t].start;
    if (n < 1 || n > 18)
        return -1;
    *v = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        *v = *v * 10 + (s[i] - '0');
    }
    return 0;
}
