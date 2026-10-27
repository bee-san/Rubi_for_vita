/* Text a native hook copied out of a game, decoded to UTF-8 (core/hook_text.h).
 *
 * Every loop advances by at least one byte of input per step and stops at n, at
 * the first NUL unit or when the next character does not fit: no input makes it
 * read past n, write past cap or loop. Self-contained (its own UTF-8 encoder, no
 * libc): the game worker links it as is, and so do the host tests. */
#include "hook_text.h"

#include "hook_cp932.inc"

/* The UTF-8 bytes of cp (<= U+10FFFF, not a surrogate) into o (4 bytes). */
static unsigned utf8_put(uint32_t cp, char *o)
{
    if (cp < 0x80) {
        o[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        o[0] = (char)(0xC0 | cp >> 6);
        o[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | cp >> 12);
        o[1] = (char)(0x80 | (cp >> 6 & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | cp >> 18);
    o[1] = (char)(0x80 | (cp >> 12 & 0x3F));
    o[2] = (char)(0x80 | (cp >> 6 & 0x3F));
    o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* One well-formed UTF-8 sequence at s[0..n) (Unicode table 3-7: no overlong form,
 * no surrogate, nothing above U+10FFFF): its length, the code point in *cp; 0 when
 * the sequence is not well formed, -1 when it is a well-formed start cut off by n. */
static int utf8_get(const uint8_t *s, size_t n, uint32_t *cp)
{
    unsigned b = s[0], len, lo = 0x80, hi = 0xBF;
    uint32_t v;
    if (b < 0x80) {
        *cp = b;
        return 1;
    }
    if (b >= 0xC2 && b <= 0xDF) {
        len = 2;
        v = b & 0x1F;
    } else if (b >= 0xE0 && b <= 0xEF) {
        len = 3;
        v = b & 0x0F;
        if (b == 0xE0) lo = 0xA0;
        if (b == 0xED) hi = 0x9F;
    } else if (b >= 0xF0 && b <= 0xF4) {
        len = 4;
        v = b & 0x07;
        if (b == 0xF0) lo = 0x90;
        if (b == 0xF4) hi = 0x8F;
    } else {
        return 0;
    }
    for (unsigned k = 1; k < len; k++) {
        unsigned c;
        if (k >= n) return -1;
        c = s[k];
        if (c < lo || c > hi) return 0;
        lo = 0x80;
        hi = 0xBF;
        v = v << 6 | (c & 0x3F);
    }
    *cp = v;
    return (int)len;
}

/* The code point of the CP932 double-byte cell (lead, trail), 0 for none. */
static uint32_t cp932_pair(unsigned lead, unsigned trail)
{
    unsigned i, lo = 0, hi = VJO_CP932_SEGS, k, len;
    const uint16_t *seg;
    if (lead >= 0x81 && lead <= 0x9F) lead -= 0x81;
    else if (lead >= 0xE0 && lead <= 0xFC) lead -= 0xE0 - 31;
    else return 0;
    if (trail >= 0x40 && trail <= 0x7E) trail -= 0x40;
    else if (trail >= 0x80 && trail <= 0xFC) trail -= 0x80 - 63;
    else return 0;
    i = lead * 188 + trail;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (vjo_cp932_seg[mid][0] <= i) lo = mid + 1;
        else hi = mid;
    }
    if (!lo) return 0;
    seg = vjo_cp932_seg[lo - 1];
    k = i - seg[0];
    len = seg[1] & 0xFFFu;
    if (k >= len) return 0;
    switch (seg[1] >> 12) {
    case 1: /* RUN */
        return (uint32_t)seg[2] + k;
    case 0: /* RAW */
        return vjo_cp932_raw[seg[2] + k];
    default: { /* DELTA: from the checkpoint, at most VJO_CP932_CK - 1 steps */
        const uint16_t *ck = vjo_cp932_ck[seg[2] + k / VJO_CP932_CK];
        unsigned off = ck[0];
        uint32_t v = ck[1];
        for (unsigned j = k % VJO_CP932_CK; j; j--) {
            unsigned b = vjo_cp932_steps[off];
            if (b == 0x80) {
                v = vjo_cp932_steps[off + 1] | (uint32_t)vjo_cp932_steps[off + 2] << 8;
                off += 3;
            } else {
                v = (v + (uint32_t)(int32_t)(int8_t)b) & 0xFFFFu;
                off += 1;
            }
        }
        return v;
    }
    }
}

/* AUTO: well-formed UTF-8 up to the first NUL (a sequence cut off by n at the end
 * allowed: the copy's cap cut it) with at least one byte >= 0x80. */
static int looks_utf8(const uint8_t *s, size_t n)
{
    int high = 0;
    size_t i = 0;
    while (i < n && s[i]) {
        uint32_t cp;
        int k = utf8_get(s + i, n - i, &cp);
        if (k < 0) break; /* cut off at the end */
        if (k == 0) return 0;
        if (cp >= 0x80) high = 1;
        i += (size_t)k;
    }
    return high;
}

size_t vjo_hook_decode(unsigned encoding, const uint8_t *src, size_t n, char *dst, size_t cap,
                       unsigned *used)
{
    size_t i = 0, w = 0;
    if (!src) n = 0;
    if (encoding == VJO_HOOK_ENC_AUTO) encoding = looks_utf8(src, n) ? VJO_HOOK_ENC_UTF8 : VJO_HOOK_ENC_CP932;
    if (used) *used = encoding;
    if (!dst || !cap) return 0;
    if (encoding != VJO_HOOK_ENC_UTF8 && encoding != VJO_HOOK_ENC_UTF16LE && encoding != VJO_HOOK_ENC_CP932 &&
        encoding != VJO_HOOK_ENC_CP932_A0) n = 0;
    while (i < n) {
        uint32_t cp = 0;
        size_t step = 1;
        if (encoding == VJO_HOOK_ENC_UTF16LE) {
            uint32_t u;
            if (n - i < 2) break; /* an odd last byte */
            u = src[i] | (uint32_t)src[i + 1] << 8;
            step = 2;
            if (!u) break;
            if (u >= 0xD800 && u <= 0xDBFF) {
                uint32_t lo;
                if (n - i >= 4 && (lo = src[i + 2] | (uint32_t)src[i + 3] << 8) >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                    step = 4;
                }
                /* else: a lone high surrogate, dropped */
            } else if (u < 0xDC00 || u > 0xDFFF) {
                cp = u; /* (a lone low surrogate is dropped) */
            }
        } else if (encoding == VJO_HOOK_ENC_UTF8) {
            int k;
            if (!src[i]) break;
            k = utf8_get(src + i, n - i, &cp);
            if (k > 0) step = (size_t)k;
            else cp = 0; /* not well formed, or cut off by n: dropped a byte at a time */
        } else { /* CP932 */
            unsigned b = src[i];
            if (!b) break;
            if (b < 0x80) {
                cp = b;
            } else if (b >= 0xA1 && b <= 0xDF) {
                cp = 0xFF61 + (b - 0xA1); /* half-width katakana */
            } else if ((b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC)) {
                unsigned t = i + 1 < n ? src[i + 1] : 0;
                if ((t >= 0x40 && t <= 0x7E) || (t >= 0x80 && t <= 0xFC)) {
                    cp = cp932_pair(b, t); /* 0: a cell without a character, both bytes dropped */
                    step = 2;
                }
                /* else: a lead byte without its trail byte, dropped alone */
            } else if (b == 0xA0) { /* Windows' cp932's U+F8F0 in VJO_HOOK_ENC_CP932_A0, for its filter; else dropped */
                cp = encoding == VJO_HOOK_ENC_CP932 ? 0u : VJO_HOOK_CP932_A0;
            }
            /* 0x80, 0xA0 (Shift_JIS leaves it undefined) but in VJO_HOOK_ENC_CP932_A0, and 0xFD-0xFF: dropped */
        }
        if (cp) {
            char o[4];
            unsigned len = utf8_put(cp, o);
            if (len > cap - 1 - w) break; /* ends at the last whole character */
            for (unsigned k = 0; k < len; k++) dst[w++] = o[k];
        }
        i += step;
    }
    dst[w] = 0;
    return w;
}

static int ascii_word(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

size_t vjo_hook_clean(char *text, size_t n)
{
    size_t i = 0, w = 0;
    if (!text) return 0;
    while (i < n && text[i]) {
        unsigned char c = (unsigned char)text[i++];
        if (c == '\\' && i < n && text[i]) {
            unsigned char cmd = (unsigned char)text[i];
            if (cmd == '\\') { /* literal pair: do not treat its second slash as a command */
                text[w++] = (char)c;
                text[w++] = text[i++];
                continue;
            }
            if ((cmd == 's' || cmd == 'w') && i + 1 < n &&
                text[i + 1] >= '0' && text[i + 1] <= '9') {
                i++;
                do { i++; } while (i < n && text[i] >= '0' && text[i] <= '9');
                continue;
            }
            /* Do not eat the beginning of a literal word such as \new or \path. */
            if (i + 1 == n || !ascii_word((unsigned char)text[i + 1])) {
                if (cmd == 'n' || cmd == 'r' || cmd == 't') {
                    i++;
                    c = cmd == 'n' ? '\n' : cmd == 'r' ? '\r' : '\t';
                } else if (cmd == 'c' || cmd == 'k' || cmd == 'p' || cmd == 'f') {
                    i++;
                    continue;
                }
            }
        }
        if ((c < 0x20 && c != '\n' && c != '\r' && c != '\t') || c == 0x7F) continue;
        text[w++] = (char)c;
    }
    text[w] = 0;
    return w;
}

unsigned vjo_hook_japanese(const char *utf8, size_t n)
{
    const uint8_t *s = (const uint8_t *)utf8;
    unsigned count = 0;
    size_t i = 0;
    if (!s) return 0;
    while (i < n) {
        uint32_t cp;
        int k = utf8_get(s + i, n - i, &cp);
        if (k <= 0) {
            i++;
            continue;
        }
        i += (size_t)k;
        if ((cp >= 0x3000 && cp <= 0x30FF) ||   /* CJK symbols and punctuation, hiragana, katakana */
            (cp >= 0x31F0 && cp <= 0x31FF) ||   /* katakana phonetic extensions */
            (cp >= 0x3400 && cp <= 0x4DBF) ||   /* CJK extension A */
            (cp >= 0x4E00 && cp <= 0x9FFF) ||   /* CJK unified ideographs */
            (cp >= 0xF900 && cp <= 0xFAFF) ||   /* CJK compatibility ideographs */
            (cp >= 0xFF00 && cp <= 0xFFEF) ||   /* half-width and full-width forms */
            (cp >= 0x20000 && cp <= 0x2FFFF))   /* CJK extensions B-F, compatibility supplement */
            count++;
    }
    return count;
}
