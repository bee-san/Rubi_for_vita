#include "utf.h"

#include <string.h>

uint32_t vjo_utf8_next(const char *str, size_t len, size_t *i)
{
    const uint8_t *s = (const uint8_t *)str;
    size_t p = *i;
    uint32_t c = s[p], cp;
    int n;

    if (c < 0x80) {
        *i = p + 1;
        return c;
    } else if ((c & 0xE0) == 0xC0) {
        n = 1;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        n = 2;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        n = 3;
        cp = c & 0x07;
    } else {
        *i = p + 1;
        return 0xFFFD;
    }
    if (p + (size_t)n >= len) {
        *i = p + 1;
        return 0xFFFD;
    }
    for (int k = 1; k <= n; k++) {
        uint8_t cc = s[p + (size_t)k];
        if ((cc & 0xC0) != 0x80) {
            *i = p + 1;
            return 0xFFFD;
        }
        cp = (cp << 6) | (cc & 0x3F);
    }
    *i = p + 1 + (size_t)n;
    return cp;
}

int vjo_utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

size_t vjo_utf8_cursor_step(const char *s, size_t len, size_t offset, int direction)
{
    size_t current = 0, previous = 0, next = 0;
    if (!s || !len)
        return 0;
    for (;;) {
        next = current;
        vjo_utf8_next(s, len, &next);
        if (offset < next || next == len)
            break;
        previous = current;
        current = next;
    }
    if (direction < 0)
        return previous;
    return direction > 0 && next < len ? next : current;
}

int vjo_utf8_cursor_range(const char *s, size_t len, size_t offset, uint32_t *start, uint32_t *units)
{
    size_t i = 0, selected;
    uint32_t cp;
    *start = *units = 0;
    if (!s || !len)
        return 0;
    selected = vjo_utf8_cursor_step(s, len, offset, 0);
    while (i < selected) {
        cp = vjo_utf8_next(s, len, &i);
        if (cp != '\r')
            *start += (uint32_t)vjo_utf16_units(cp);
    }
    cp = vjo_utf8_next(s, len, &i);
    if (cp == '\r')
        return 0;
    *units = (uint32_t)vjo_utf16_units(cp);
    return 1;
}

/* Unicode 17.0.0 Scripts/ScriptExtensions Han, Hiragana and Katakana,
 * intersected with letter/letter-number categories; Japanese combining
 * dakuten/handakuten are also retained. Punctuation is deliberately excluded.
 * Inspiration: ikatyang-collab/cjk-regex@3703c1322ad52ba20f9706752f3098181ef3e15b
 * uses script/category intersections; its Hangul/Bopomofo are not included.
 * https://www.unicode.org/Public/17.0.0/ucd/{Scripts,ScriptExtensions,UnicodeData}.txt
 * SHA256: 9f5e50d3abaee7d6ce09480f325c706f485ae3240912527e651954d2d6b035bf
 *         ec2107e58825a1586acee8e0911ce18260394ac8b87e535ca325f1ccbeb06bc6
 *         2e1efc1dcb59c575eedf5ccae60f95229f706ee6d031835247d843c11d96470c */
static const struct { uint32_t first, last; } japanese_letters[] = {
    {0x3005u, 0x3007u},
    {0x3021u, 0x3029u},
    {0x3031u, 0x3035u},
    {0x3038u, 0x303Cu},
    {0x3041u, 0x3096u},
    {0x3099u, 0x309Au},
    {0x309Du, 0x309Fu},
    {0x30A1u, 0x30FAu},
    {0x30FCu, 0x30FFu},
    {0x31F0u, 0x31FFu},
    {0x3400u, 0x4DBFu},
    {0x4E00u, 0x9FFFu},
    {0xF900u, 0xFA6Du},
    {0xFA70u, 0xFAD9u},
    {0xFF66u, 0xFF9Fu},
    {0x16FE3u, 0x16FE3u},
    {0x16FF2u, 0x16FF6u},
    {0x1AFF0u, 0x1AFF3u},
    {0x1AFF5u, 0x1AFFBu},
    {0x1AFFDu, 0x1AFFEu},
    {0x1B000u, 0x1B122u},
    {0x1B132u, 0x1B132u},
    {0x1B150u, 0x1B152u},
    {0x1B155u, 0x1B155u},
    {0x1B164u, 0x1B167u},
    {0x20000u, 0x2A6DFu},
    {0x2A700u, 0x2B81Du},
    {0x2B820u, 0x2CEADu},
    {0x2CEB0u, 0x2EBE0u},
    {0x2EBF0u, 0x2EE5Du},
    {0x2F800u, 0x2FA1Du},
    {0x30000u, 0x3134Au},
    {0x31350u, 0x33479u}
};

int vjo_is_japanese_letter(uint32_t cp)
{
    size_t lo = 0, hi = sizeof(japanese_letters) / sizeof(japanese_letters[0]);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cp < japanese_letters[mid].first) hi = mid;
        else if (cp > japanese_letters[mid].last) lo = mid + 1;
        else return 1;
    }
    return 0;
}

size_t vjo_utf8_japanese_cursor_step(const char *s, size_t len, size_t offset, int direction)
{
    size_t previous = len, current = len, next = len, i = 0;
    if (!s || !len) return len;
    size_t selected = vjo_utf8_cursor_step(s, len, offset, 0);
    while (i < len) {
        size_t begin = i;
        uint32_t cp = vjo_utf8_next(s, len, &i);
        if (!vjo_is_japanese_letter(cp)) continue;
        /* The compatibility decoder also accepts overlong sequences. This
         * opt-in selector must not turn malformed bytes into Japanese. */
        size_t canonical = cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        if (i - begin != canonical) continue;
        if (begin < selected) previous = begin;
        else if (begin == selected) current = begin;
        else if (next == len) next = begin;
    }
    if (direction < 0 && previous < len) return previous;
    if (direction > 0 && next < len) return next;
    if (current < len) return current;
    return next < len ? next : previous;
}

char *vjo_java_trim(char *s)
{
    size_t n = strlen(s);
    while (n && (uint8_t)s[n - 1] <= 0x20)
        s[--n] = '\0';
    while (*s && (uint8_t)*s <= 0x20)
        s++;
    return s;
}

static char ascii_lower(char c)
{
    return (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
}

int vjo_ieq_prefix(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++)
        if (ascii_lower(*s) != ascii_lower(*prefix))
            return 0;
    return 1;
}

int vjo_starts_withn(const char *s, size_t n, const char *prefix)
{
    size_t k = strlen(prefix);
    return n >= k && !memcmp(s, prefix, k);
}

void vjo_utf8_trim_partial(char *s)
{
    size_t n = strlen(s), start = n, need;
    unsigned char lead;
    while (start > 0 && ((unsigned char)s[start - 1] & 0xC0) == 0x80)
        start--;
    if (start == 0)
        return;
    lead = (unsigned char)s[start - 1];
    need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (n - (start - 1) < need)
        s[start - 1] = '\0';
}

int vjo_ieq(const char *a, const char *b)
{
    return vjo_ieq_prefix(a, b) && strlen(a) == strlen(b);
}
