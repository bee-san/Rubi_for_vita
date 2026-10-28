/* The native hooks' text decoder (core/hook_text.c): UTF-8, UTF-16LE with surrogates,
 * CP932 with the NEC and IBM extension rows (every cell against the generator's mapping,
 * tools/gen_cp932.py), AUTO, NUL handling, malformed input (dropped, never copied, never
 * read past n) and the cap (the text ends at its last whole character). Pure C: also on the
 * Vita's CPU under qemu-arm (tools/arm_host_tests.sh). */
#include <stdio.h>
#include <string.h>

#include "acutest.h"
#include "bearssl_hash.h"
#include "hook_text.h"
#include "reading_chars.h"

#include "hook_cp932.inc" /* VJO_CP932_MAP_SHA256 (the table itself is hook_text.c's) */

static size_t dec(unsigned enc, const char *src, size_t n, char *out, size_t cap, unsigned *used)
{
    return vjo_hook_decode(enc, (const uint8_t *)src, n, out, cap, used);
}

/* decodes and compares; n is the source's length */
#define DECODES(enc, src, n, want)                                                                 \
    do {                                                                                           \
        char out_[256];                                                                            \
        size_t got_ = dec(enc, src, n, out_, sizeof out_, NULL);                                   \
        TEST_CHECK_(got_ == strlen(want) && !strcmp(out_, want), "%s", #src);                      \
        TEST_MSG("want [%s] (%u), got [%s] (%u)", want, (unsigned)strlen(want), out_, (unsigned)got_); \
    } while (0)

static void utf8_valid(void)
{
    DECODES(VJO_HOOK_ENC_UTF8, "abc", 3, "abc");
    DECODES(VJO_HOOK_ENC_UTF8, "日本語のテキスト", 24, "日本語のテキスト");
    DECODES(VJO_HOOK_ENC_UTF8, "\xf0\x9f\x98\x80!", 5, "\xf0\x9f\x98\x80!");       /* U+1F600 */
    DECODES(VJO_HOOK_ENC_UTF8, "\xf4\x8f\xbf\xbf", 4, "\xf4\x8f\xbf\xbf");         /* U+10FFFF */
    DECODES(VJO_HOOK_ENC_UTF8, "a\nb\x1b", 4, "a\nb\x1b");                          /* controls kept */
    DECODES(VJO_HOOK_ENC_UTF8, "", 0, "");
}

static void utf8_malformed(void)
{
    DECODES(VJO_HOOK_ENC_UTF8, "a\xc0\xafz", 4, "az");             /* overlong / */
    DECODES(VJO_HOOK_ENC_UTF8, "a\xe0\x80\xafz", 5, "az");         /* overlong, 3 bytes */
    DECODES(VJO_HOOK_ENC_UTF8, "a\xed\xa0\x80z", 5, "az");         /* a surrogate */
    DECODES(VJO_HOOK_ENC_UTF8, "a\xf4\x90\x80\x80z", 6, "az");     /* above U+10FFFF */
    DECODES(VJO_HOOK_ENC_UTF8, "a\xf5\x80\x80\x80z", 6, "az");
    DECODES(VJO_HOOK_ENC_UTF8, "\x80\xbf\xff\xfe", 4, "");         /* stray continuations, FE FF */
    DECODES(VJO_HOOK_ENC_UTF8, "あ\xe3\x81", 5, "あ");             /* cut off at n */
    DECODES(VJO_HOOK_ENC_UTF8, "\xe3\x81" "A", 3, "A");            /* not continued */
    DECODES(VJO_HOOK_ENC_UTF8, "\xe3\x81\xe3\x81\x82", 5, "あ");   /* a restart inside a sequence */
}

static void utf16_cases(void)
{
    static const char hello[] = "\x42\x30\x44\x30\x46\x30"; /* あいう */
    DECODES(VJO_HOOK_ENC_UTF16LE, hello, 6, "あいう");
    DECODES(VJO_HOOK_ENC_UTF16LE, "\x3d\xd8\x00\xde", 4, "\xf0\x9f\x98\x80"); /* a surrogate pair */
    DECODES(VJO_HOOK_ENC_UTF16LE, "\x3d\xd8" "A\0", 4, "A");                  /* a lone high one */
    DECODES(VJO_HOOK_ENC_UTF16LE, "\x3d\xd8", 2, "");                         /* cut off at n */
    DECODES(VJO_HOOK_ENC_UTF16LE, "\x00\xde" "B\0", 4, "B");                  /* a lone low one */
    DECODES(VJO_HOOK_ENC_UTF16LE, "\x3d\xd8\x3d\xd8\x00\xde", 6, "\xf0\x9f\x98\x80");
    DECODES(VJO_HOOK_ENC_UTF16LE, "A\0B", 3, "A");                            /* an odd last byte */
    DECODES(VJO_HOOK_ENC_UTF16LE, "A\0\0\0B\0", 6, "A");                      /* ends at the NUL unit */
    DECODES(VJO_HOOK_ENC_UTF16LE, "\0A", 2, "\xe4\x84\x80");                  /* U+4100: not a NUL unit */
    DECODES(VJO_HOOK_ENC_UTF16LE, "\x0a\x00\xff\xfe", 4, "\n\xef\xbb\xbf");   /* U+FEFF kept */
}

static void cp932_cases(void)
{
    DECODES(VJO_HOOK_ENC_CP932, "\x82\xa0\x8a\xbf\x8e\x9a", 6, "あ漢字");
    DECODES(VJO_HOOK_ENC_CP932, "\x81\x40\x81\x79\x81\x7a", 6, "　【】");
    DECODES(VJO_HOOK_ENC_CP932, "\xb1\xb2\xdf\xa1", 4, "ｱｲﾟ｡");             /* half-width katakana */
    DECODES(VJO_HOOK_ENC_CP932, "\x5c\x7e", 2, "\\~");                        /* CP932, not JIS-Roman */
    DECODES(VJO_HOOK_ENC_CP932, "\x87\x40\x87\x54\x87\x82\x87\x9c", 8, "①Ⅰ№∪"); /* NEC row 13 */
    DECODES(VJO_HOOK_ENC_CP932, "\xed\x40\xee\xec\xee\xef\xee\xfc", 8, "纊黑ⅰ＂"); /* NEC-selected IBM */
    DECODES(VJO_HOOK_ENC_CP932, "\xfa\x40\xfa\x5c\xfc\x4b", 6, "ⅰ纊黑");       /* IBM extensions */
    DECODES(VJO_HOOK_ENC_CP932, "\x88\x9f\x98\x72\x98\x9f\xea\xa4", 8, "亜腕弌熙"); /* JIS levels 1 and 2 */
    DECODES(VJO_HOOK_ENC_CP932, "A\x85\x40" "B", 4, "AB");      /* a cell without a character: both bytes */
    DECODES(VJO_HOOK_ENC_CP932, "A\xf0\x40" "B", 4, "AB");      /* the user-defined rows: no glyph */
    DECODES(VJO_HOOK_ENC_CP932, "A\x82\x0a" "B", 4, "A\nB");    /* a lead without a trail: the lead alone */
    DECODES(VJO_HOOK_ENC_CP932, "A\x82\x7f" "B", 4, "A\x7f" "B");
    DECODES(VJO_HOOK_ENC_CP932, "A\x82", 2, "A");               /* cut off at n */
    DECODES(VJO_HOOK_ENC_CP932, "A\x82\0\x82\xa0", 5, "A");     /* the NUL after a lead ends it */
    DECODES(VJO_HOOK_ENC_CP932, "\x80\xfd\xfe\xff" "C", 5, "C");
    /* 0xA0, which Shift_JIS leaves undefined: dropped, as before #316, except in VJO_HOOK_ENC_CP932_A0 (CP932 with
     * VJO_HOOK_DECODE_A0: a filter that makes it what its game draws, core/hook_luna.c A0_BLANK; #316's audit F2),
     * Windows' cp932's U+F8F0. As a trail byte it is a character's (あ, 82 A0) either way. The flag with another
     * encoding is no encoding: no text. */
    DECODES(VJO_HOOK_ENC_CP932, "A\xa0" "B\x81\x49\xa0", 6, "AB！");
    DECODES(VJO_HOOK_ENC_CP932_A0, "A\xa0" "B\x81\x49\xa0", 6, "A\xef\xa3\xb0" "B！\xef\xa3\xb0");
    DECODES(VJO_HOOK_ENC_CP932, "\x82\xa0\xa0", 3, "あ");
    DECODES(VJO_HOOK_ENC_CP932_A0, "\x82\xa0\xa0", 3, "あ\xef\xa3\xb0");
    TEST_CHECK(VJO_HOOK_ENC_CP932_A0 == (VJO_HOOK_ENC_CP932 | VJO_HOOK_DECODE_A0));
    DECODES(VJO_HOOK_ENC_UTF8 | VJO_HOOK_DECODE_A0, "A\xa0" "B", 3, "");
    DECODES(VJO_HOOK_ENC_UTF16LE | VJO_HOOK_DECODE_A0, "\xa0\x00", 2, "");
    {
        char out[16];
        unsigned used = 99;
        TEST_CHECK(dec(VJO_HOOK_ENC_AUTO, "A\xa0", 2, out, sizeof out, &used) == 1 && used == VJO_HOOK_ENC_CP932 &&
                   !strcmp(out, "A")); /* AUTO: CP932, its 0xA0 dropped */
        TEST_CHECK(dec(VJO_HOOK_ENC_CP932_A0, "A\xa0", 2, out, sizeof out, &used) == 4 &&
                   used == VJO_HOOK_ENC_CP932_A0);
    }
    DECODES(VJO_HOOK_ENC_CP932, "\x1b\xb4", 2, "\x1bｴ");
}

/* Every cell of the table against tools/gen_cp932.py's mapping (its SHA-256). */
static void cp932_every_cell(void)
{
    static const unsigned char want_hex[] = VJO_CP932_MAP_SHA256;
    unsigned char got[32], le[2];
    br_sha256_context h;
    unsigned cells = 0, mapped = 0;
    char hex[65];
    br_sha256_init(&h);
    for (unsigned lead = 0x81; lead <= 0xFC; lead++) {
        if (lead >= 0xA0 && lead < 0xE0) continue;
        for (unsigned trail = 0x40; trail <= 0xFC; trail++) {
            uint8_t src[2] = {(uint8_t)lead, (uint8_t)trail};
            char out[8];
            uint32_t cp = 0;
            size_t n;
            if (trail == 0x7F) continue;
            n = vjo_hook_decode(VJO_HOOK_ENC_CP932, src, 2, out, sizeof out, NULL);
            if (n) { /* one character, UTF-8 */
                const unsigned char *u = (const unsigned char *)out;
                TEST_CHECK_(n == 3 || n == 2, "cell %02X%02X: %u bytes", lead, trail, (unsigned)n);
                cp = n == 2 ? (uint32_t)(u[0] & 0x1F) << 6 | (u[1] & 0x3F)
                            : (uint32_t)(u[0] & 0x0F) << 12 | (uint32_t)(u[1] & 0x3F) << 6 | (u[2] & 0x3F);
                mapped++;
            }
            le[0] = (unsigned char)cp;
            le[1] = (unsigned char)(cp >> 8);
            br_sha256_update(&h, le, 2);
            cells++;
        }
    }
    br_sha256_out(&h, got);
    for (unsigned i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", got[i]);
    TEST_CHECK(cells == VJO_CP932_CELLS);
    TEST_CHECK_(!memcmp(hex, want_hex, 64), "mapping SHA-256 %s", hex);
    TEST_MSG("want %s", (const char *)want_hex);
    TEST_CHECK_(mapped == 7724, "%u characters", mapped);
}

/* The compact table's own shape (tools/gen_cp932.py): segments in order, inside the cells,
 * their pools and checkpoints inside the arrays. */
static void cp932_table_shape(void)
{
    unsigned prev_end = 0, kinds[3] = {0, 0, 0};
    for (unsigned i = 0; i < VJO_CP932_SEGS; i++) {
        unsigned start = vjo_cp932_seg[i][0], len = vjo_cp932_seg[i][1] & 0xFFFu, kind = vjo_cp932_seg[i][1] >> 12;
        unsigned at = vjo_cp932_seg[i][2];
        TEST_CHECK_(start >= prev_end && len && start + len <= VJO_CP932_CELLS, "segment %u", i);
        TEST_CHECK_(kind <= 2, "segment %u kind %u", i, kind);
        if (kind == 0) TEST_CHECK_(at + len <= sizeof vjo_cp932_raw / sizeof vjo_cp932_raw[0], "segment %u", i);
        if (kind == 2) {
            unsigned cks = (len + VJO_CP932_CK - 1) / VJO_CP932_CK;
            TEST_CHECK_(at + cks <= sizeof vjo_cp932_ck / sizeof vjo_cp932_ck[0], "segment %u", i);
            TEST_CHECK(vjo_cp932_ck[at][0] < sizeof vjo_cp932_steps);
        }
        if (kind <= 2) kinds[kind]++;
        prev_end = start + len;
    }
    TEST_CHECK(kinds[0] && kinds[1] && kinds[2]);
    TEST_MSG("raw %u, run %u, delta %u segments", kinds[0], kinds[1], kinds[2]);
}

static void auto_detection(void)
{
    char out[64];
    unsigned used = 99;
    dec(VJO_HOOK_ENC_AUTO, "日本", 6, out, sizeof out, &used);
    TEST_CHECK(used == VJO_HOOK_ENC_UTF8 && !strcmp(out, "日本"));
    dec(VJO_HOOK_ENC_AUTO, "\x93\xfa\x96\x7b", 4, out, sizeof out, &used); /* 日本 in CP932 */
    TEST_CHECK(used == VJO_HOOK_ENC_CP932 && !strcmp(out, "日本"));
    dec(VJO_HOOK_ENC_AUTO, "plain", 5, out, sizeof out, &used); /* no byte >= 0x80: CP932 (the same text) */
    TEST_CHECK(used == VJO_HOOK_ENC_CP932 && !strcmp(out, "plain"));
    dec(VJO_HOOK_ENC_AUTO, "日本\xe8\xaa", 8, out, sizeof out, &used); /* UTF-8 the cap cut off */
    TEST_CHECK(used == VJO_HOOK_ENC_UTF8 && !strcmp(out, "日本"));
    dec(VJO_HOOK_ENC_AUTO, "日本\0\x93\xfa", 9, out, sizeof out, &used); /* judged up to the NUL */
    TEST_CHECK(used == VJO_HOOK_ENC_UTF8 && !strcmp(out, "日本"));
    dec(VJO_HOOK_ENC_AUTO, "", 0, out, sizeof out, &used);
    TEST_CHECK(used == VJO_HOOK_ENC_CP932 && !out[0]);
    dec(77, "abc", 3, out, sizeof out, &used); /* an unknown encoding: nothing */
    TEST_CHECK(!out[0]);
}

static void nul_and_cap(void)
{
    char out[16];
    memset(out, 'x', sizeof out);
    TEST_CHECK(dec(VJO_HOOK_ENC_UTF8, "ab\0cd", 5, out, sizeof out, NULL) == 2 && !strcmp(out, "ab"));
    /* "あいう" is 9 bytes: cap 10 fits it, 9 keeps two characters, 4 one, 3 none, 1 none */
    TEST_CHECK(dec(VJO_HOOK_ENC_UTF8, "あいう", 9, out, 10, NULL) == 9);
    TEST_CHECK(dec(VJO_HOOK_ENC_UTF8, "あいう", 9, out, 9, NULL) == 6 && !strcmp(out, "あい"));
    TEST_CHECK(dec(VJO_HOOK_ENC_CP932, "\x82\xa0\x82\xa2", 4, out, 4, NULL) == 3 && !strcmp(out, "あ"));
    TEST_CHECK(dec(VJO_HOOK_ENC_UTF16LE, "\x42\x30\x44\x30", 4, out, 3, NULL) == 0 && !out[0]);
    TEST_CHECK(dec(VJO_HOOK_ENC_UTF8, "a", 1, out, 1, NULL) == 0 && !out[0]);
    TEST_CHECK(vjo_hook_decode(VJO_HOOK_ENC_UTF8, (const uint8_t *)"a", 1, out, 0, NULL) == 0);
    TEST_CHECK(vjo_hook_decode(VJO_HOOK_ENC_UTF8, NULL, 5, out, sizeof out, NULL) == 0 && !out[0]);
    TEST_CHECK(vjo_hook_decode(VJO_HOOK_ENC_UTF8, (const uint8_t *)"a", 1, NULL, 4, NULL) == 0);
}

/* No input reads past n: a sequence that would continue past n stops at n (the guard
 * bytes after it would make it whole). */
static void never_past_n(void)
{
    static const char buf[] = "\xe3\x81\x82\xe3\x81\x82";
    char out[32];
    for (size_t n = 0; n <= 6; n++) {
        size_t got = dec(VJO_HOOK_ENC_UTF8, buf, n, out, sizeof out, NULL);
        TEST_CHECK_(got == n / 3 * 3, "n %u", (unsigned)n);
    }
    TEST_CHECK(dec(VJO_HOOK_ENC_CP932, "\x82\xa0\x82\xa0", 3, out, sizeof out, NULL) == 3);
    TEST_CHECK(dec(VJO_HOOK_ENC_UTF16LE, "\x3d\xd8\x00\xde", 3, out, sizeof out, NULL) == 0);
}

/* A pseudo-random walk over every encoding: never more than cap - 1 bytes, always
 * NUL-terminated, always well-formed UTF-8, and the same result twice. */
static void fuzz_well_formed(void)
{
    uint32_t s = 0x12345678;
    for (unsigned round = 0; round < 20000; round++) {
        uint8_t src[48];
        char out[40], again[40];
        unsigned n, enc, cap;
        size_t got, i = 0;
        for (unsigned k = 0; k < sizeof src; k++) {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            src[k] = (uint8_t)(round & 1 ? s : (s % 3 ? 0x80 + s % 0x7D : s));
        }
        n = s % sizeof src;
        enc = (s >> 8) % 4;
        cap = 1 + (s >> 12) % sizeof out;
        got = vjo_hook_decode(enc, src, n, out, cap, NULL);
        TEST_CHECK(got < cap && out[got] == 0 && strlen(out) == got);
        TEST_CHECK(vjo_hook_decode(enc, src, n, again, cap, NULL) == got && !memcmp(out, again, got));
        while (i < got) { /* well-formed: lead and continuation counts */
            unsigned b = (unsigned char)out[i], len = b < 0x80 ? 1 : b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : 2;
            TEST_CHECK(b < 0x80 || (b >= 0xC2 && b <= 0xF4));
            for (unsigned k = 1; k < len; k++) TEST_CHECK(i + k < got && ((unsigned char)out[i + k] & 0xC0) == 0x80);
            i += len;
        }
    }
}

static void clean_controls(void)
{
    static const struct { const char *raw, *want; } cases[] = {
        {"\\s156「ばーーーっっ!」", "「ばーーーっっ!」"},
        {"\\s0猫\\w123犬\\s999999999999鳥\\k", "猫犬鳥"},
        {"猫\\n犬\\r\\n鳥\\t魚\\c\\p\\f", "猫\n犬\r\n鳥\t魚"},
        {"猫\x01\x1b\x7f犬\n\r\t鳥", "猫犬\n\r\t鳥"},
        {"「ABC123、猫ー々ｱ」", "「ABC123、猫ー々ｱ」"},
        {"\\q123 \\new \\path \\s \\w \\s猫 \\c123", "\\q123 \\new \\path \\s \\w \\s猫 \\c123"},
        {"\\\\s156猫\\\\n犬\\", "\\\\s156猫\\\\n犬\\"},
        {"\\s156\\w1\\k\\c", ""},
        {"", ""},
        /* private-use code points stay, U+F8F0 too (#316's audit F2: no path but the filters that map it makes one) */
        {"猫\xef\xa3\xb0犬\xef\xa3\xb0", "猫\xef\xa3\xb0犬\xef\xa3\xb0"},
        {"猫\xef\xa3\xb1犬\xef\xa3", "猫\xef\xa3\xb1犬\xef\xa3"},
        /* マブラヴ オルタネイティヴ PCSG00777, a verified entry with no filter: its message's \x03 (a reveal pause) and
         * \x01 (the click wait that ends it) are controls (the psandvita farm's page 3, as its reviewer read it) */
        {"【武】「…\x03…\x03オレの…\x03…\x03部屋、\x03か？」\x01", "【武】「……オレの……部屋、か？」"},
    };
    for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        char text[256];
        strcpy(text, cases[k].raw);
        size_t n = vjo_hook_clean(text, strlen(text));
        TEST_CHECK_(n == strlen(cases[k].want) && !strcmp(text, cases[k].want),
                    "[%s] -> [%s], want [%s]", cases[k].raw, text, cases[k].want);
    }
    /* The plain path (the CALL source, a site with neither filter nor Luna site): a CP932 0xA0 before a command's
     * letter leaves the command a command, as before #316 (#316's audit: A\<A0>w30.B gave A\w30.B there) */
    {
        char text[64];
        size_t n = dec(VJO_HOOK_ENC_CP932, "A\\\xa0w30.B", 8, text, sizeof text, NULL);
        n = vjo_hook_clean(text, n);
        TEST_CHECK_(n == 3 && !strcmp(text, "A.B"), "got [%s]", text);
    }
    /* The same count history.csv saves: the command's letter and digits are not reading. */
    char text[] = "\\s156「ばーーーっっ!」\\w10\\k";
    size_t n = vjo_hook_clean(text, strlen(text));
    TEST_CHECK(vjo_reading_characters(text, n) == 6);
}

static void clean_bounds(void)
{
    const char *raw = "\\s156猫\\n犬\\k\\";
    for (size_t n = 0; n <= strlen(raw); n++) {
        char text[64];
        memset(text, 0x55, sizeof(text));
        memcpy(text, raw, n); /* no NUL in the readable input: n is the bound */
        size_t got = vjo_hook_clean(text, n);
        TEST_CHECK(got <= n && text[got] == 0);
        for (size_t j = n + 1; j < sizeof(text); j++) TEST_CHECK(text[j] == 0x55);
    }
    char text[] = "猫\0\\s156";
    TEST_CHECK(vjo_hook_clean(text, sizeof(text) - 1) == 3 && !strcmp(text, "猫"));
    TEST_CHECK(vjo_hook_clean(NULL, 10) == 0);
}

static void japanese_count(void)
{
    const char *s = "「こんにちは」、世界！ABC々ー㌔ｱ漢\xf0\xa0\x80\x8b";
    TEST_CHECK(vjo_hook_japanese(s, strlen(s)) == 16); /* not ABC, not ㌔ (CJK compatibility) */
    TEST_CHECK(vjo_hook_japanese("hello, world", 12) == 0);
    TEST_CHECK(vjo_hook_japanese("\xe3\x81", 2) == 0); /* cut off: none */
    TEST_CHECK(vjo_hook_japanese("あ\xff" "い", 7) == 2);
    TEST_CHECK(vjo_hook_japanese(NULL, 4) == 0);
}

static void maybe_text(void)
{
    TEST_CHECK(vjo_hook_maybe_text((const uint8_t *)"\x82\xa0", 2));
    TEST_CHECK(!vjo_hook_maybe_text((const uint8_t *)"abcdefgh\x82", 9)); /* only the first 8 */
    TEST_CHECK(vjo_hook_maybe_text((const uint8_t *)"abcdefg\x81", 8));
    TEST_CHECK(!vjo_hook_maybe_text((const uint8_t *)"\x80", 1));
    TEST_CHECK(!vjo_hook_maybe_text((const uint8_t *)"", 0));
}

TEST_LIST = {
    {"utf8_valid", utf8_valid},
    {"utf8_malformed", utf8_malformed},
    {"utf16_cases", utf16_cases},
    {"cp932_cases", cp932_cases},
    {"cp932_every_cell", cp932_every_cell},
    {"cp932_table_shape", cp932_table_shape},
    {"auto_detection", auto_detection},
    {"nul_and_cap", nul_and_cap},
    {"never_past_n", never_past_n},
    {"fuzz_well_formed", fuzz_well_formed},
    {"clean_controls", clean_controls},
    {"clean_bounds", clean_bounds},
    {"japanese_count", japanese_count},
    {"maybe_text", maybe_text},
    {NULL, NULL},
};
