#include "word_key.h"

#include <string.h>

#include "bearssl.h"

/* Feeds s, folded (word_key.h), into the hash; the number of bytes fed. */
static size_t fold(br_sha256_context *h, const char *s)
{
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    size_t fed = 0;
    while (*p) {
        /* a three-byte sequence of U+3000-U+3FFF (E3 xx xx) or U+F000-U+FFFF (EF xx xx) */
        if ((p[0] == 0xE3 || p[0] == 0xEF) && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            unsigned cp = (unsigned)(p[0] & 0x0F) << 12 | (unsigned)(p[1] & 0x3F) << 6 | (p[2] & 0x3F);
            unsigned char out[3];
            if (cp == 0x30FB || cp == 0xFF65) { /* ・ and ･: left out */
                p += 3;
                continue;
            }
            if ((cp >= 0x30A1 && cp <= 0x30F6) || cp == 0x30FD || cp == 0x30FE)
                cp -= 0x60; /* katakana as hiragana: ァ-ヶ to ぁ-ゖ, ヽヾ to ゝゞ */
            out[0] = (unsigned char)(0xE0 | cp >> 12);
            out[1] = (unsigned char)(0x80 | (cp >> 6 & 0x3F));
            out[2] = (unsigned char)(0x80 | (cp & 0x3F));
            br_sha256_update(h, out, 3);
            fed += 3;
            p += 3;
            continue;
        }
        br_sha256_update(h, p, 1);
        fed++;
        p++;
    }
    return fed;
}

uint64_t vjo_word_key(const char *spelling, const char *reading)
{
    br_sha256_context h, probe;
    unsigned char d[32];
    uint64_t key = 0;
    br_sha256_init(&h);
    fold(&h, spelling);
    br_sha256_update(&h, "\t", 1);
    /* a missing reading (or one of middle dots only) is the spelling's */
    probe = h;
    if (!fold(&probe, reading))
        fold(&h, spelling);
    else
        h = probe;
    br_sha256_out(&h, d);
    for (int i = 0; i < 8; i++)
        key = key << 8 | d[i];
    return key;
}

uint64_t vjo_word_spelling_key(const char *spelling)
{
    br_sha256_context h;
    unsigned char d[32];
    uint64_t key = 0;
    br_sha256_init(&h);
    fold(&h, spelling);
    br_sha256_out(&h, d);
    for (int i = 0; i < 8; i++)
        key = key << 8 | d[i];
    return key;
}

void vjo_word_key_hex(uint64_t key, char out[VJO_WORD_KEY_HEX])
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 15; i >= 0; i--, key >>= 4)
        out[i] = digits[key & 15];
    out[16] = '\0';
}
