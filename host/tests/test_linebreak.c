/* Where the overlay's own wrapping may break a line (core/linebreak.h, the
 * owner's 2026-10-16 item 4: never inside a word, Japanese line-breaking
 * rules). Synthetic texts; each case marks with | every place a line may
 * break. Also the UTF-16 and UTF-8 walks against each other. Pure C: also
 * on the Vita's CPU under qemu-arm (tools/arm_host_tests.sh). */
#include <stdio.h>
#include <string.h>
#include "acutest.h"
#include "linebreak.h"
#include "utf.h"

/* s with '|' before every character a line may start with */
static void marked(const char *s, char *out, size_t cap)
{
    VjoBreakState st = {0};
    size_t len = strlen(s), i = 0, o = 0;
    while (i < len && o + 8 < cap) {
        size_t at = i;
        uint32_t c = vjo_utf8_next(s, len, &i);
        if (vjo_break_next(&st, c))
            out[o++] = '|';
        memcpy(out + o, s + at, i - at);
        o += i - at;
    }
    out[o] = 0;
}

static void check(const char *want)
{
    char plain[512], got[600];
    size_t n = 0;
    for (const char *p = want; *p && n + 1 < sizeof(plain); p++)
        if (*p != '|')
            plain[n++] = *p;
    plain[n] = 0;
    marked(plain, got, sizeof(got));
    TEST_CHECK_(!strcmp(got, want), "%s", plain);
    TEST_MSG("want %s", want);
    TEST_MSG("got  %s", got);
}

/* Japanese: between phrases, where kana gives way to kanji or katakana, and at punctuation. */
static void japanese_breaks_between_phrases(void)
{
    check("彼女は|学校へ|行った。|今日は|雨だ");
    check("あの|ジュースは|美味しいって|クラスで|評判でさ");
    check("思い出せなかった");             /* one hiragana between kanji: one word (思い出す) */
    check("この|人はいい|加減だ");
    check("食べるかな？");               /* inside a kana run: nowhere */
    check("お茶を|飲む");                 /* お|茶: a prefix stays */
    check("振り返る時に");                /* 振り|返る (and 返る|時): one hiragana between kanji, no particle */
    check("猫の|手も|借りたい");          /* の is a particle: 猫の|手 */
    check("あなた達がご飯を|食べた");     /* 達 stays with the word before, ご with 飯 */
    check("あなた達のおばあ様");          /* inside a kana run: nowhere */
    check("６年生の|教室");               /* digits stay with their kanji */
    check("ああ、|そうか。|分かった");
}

/* Kinsoku: no line starts with a closing mark, a small kana, ー or a pause,
 * none ends with an opening mark; marks of a kind stay together. */
static void kinsoku(void)
{
    check("「そうだね」|と|言った");      /* と: a particle between them */
    check("君が|「好きだ」|と");
    check("待って……|今の|何？");
    check("ちょっと|待って！|本当に!?|嘘でしょ");
    check("あっ、ゃだ");                  /* っ and ゃ never start a line */
    check("コーヒー、|ください");
    check("“quoted”|text");
    check("（注）|本文");
    check("――|そして");
    check("〜です~ね");                   /* 〜 (the text's ~) goes with the letter before */
}

/* Latin text breaks at spaces only; numbers and words stay whole. */
static void latin_breaks_at_spaces(void)
{
    check("The |language |used, |e.g. |gakko-o |(school).");
    check("1,000 |people");
    check("word  |two");
    check("ABC123日本");  /* no break where Latin meets Japanese, but at a space */
    check("ABC123 |日本");
    check("田中さんは|先生です |= |\"Tanaka |is |a |teacher.\"");
}

/* The text's own line breaks: never a place to break (they are breaks already),
 * and a line's first character after one is no place either. */
static void hard_breaks(void)
{
    check("彼女は\n学校へ");
    check("あいう \nかきく");
}

/* vjo_break_last16 and vjo_break_last8 give the same places, a surrogate
 * pair is one character, and the look-back stops at a hard break. */
static void last_break_in_utf16_and_utf8(void)
{
    static const char *texts[] = {"彼女は学校へ行った。今日は雨だ", "あの𠮷野家のジュースは美味しい",
                                  "word words more words", "振り返る時に猫の手も借りたい", "食べ\nるかな", ""};
    for (unsigned t = 0; t < sizeof(texts) / sizeof(texts[0]); t++) {
        uint16_t u[128];
        size_t b8[128], len = strlen(texts[t]), i = 0;
        uint32_t n = 0;
        while (i < len) { /* each character's UTF-16 start and UTF-8 start */
            uint32_t c;
            b8[n] = i;
            c = vjo_utf8_next(texts[t], len, &i);
            if (c >= 0x10000) {
                u[n++] = (uint16_t)(0xD800 | ((c - 0x10000) >> 10));
                b8[n] = b8[n - 1];
                u[n++] = (uint16_t)(0xDC00 | ((c - 0x10000) & 0x3FF));
            } else
                u[n++] = (uint16_t)c;
        }
        b8[n] = len;
        for (uint32_t from = 0; from < n; from++)
            for (uint32_t to = from; to < n; to++) {
                uint32_t b16 = vjo_break_last16(u, n, from, to);
                size_t b = vjo_break_last8(texts[t], len, b8[from], b8[to]);
                if ((u[to] & 0xFC00) == 0xDC00 || (u[from] & 0xFC00) == 0xDC00)
                    continue; /* not a character's start */
                TEST_CHECK_((b16 ? b8[b16] : 0) == b, "text %u from %u to %u: %u / %zu", t, from, to, b16, b);
                TEST_CHECK(!b16 || (b16 > from && b16 <= to));
            }
        TEST_CHECK(vjo_break_last16(u, n, 0, n) == 0 && vjo_break_last8(texts[t], len, 0, len) == 0);
    }
    /* the look-back: "は|学" from the line start; after a hard break the state starts afresh */
    {
        const uint16_t u[] = {0x5F7C, 0x5973, 0x306F, 0x5B66, 0x6821}; /* 彼女は学校 */
        const uint16_t v[] = {0x306F, '\n', 0x5B66, 0x6821};           /* は\n学校 */
        TEST_CHECK(vjo_break_last16(u, 5, 0, 4) == 3);
        TEST_CHECK(vjo_break_last16(u, 5, 2, 4) == 3); /* the line starts at は: は before it counts */
        TEST_CHECK(vjo_break_last16(u, 5, 3, 4) == 0); /* 学 starts the line: no place after it in 学校 */
        TEST_CHECK(vjo_break_last16(v, 4, 2, 3) == 0);
    }
}

TEST_LIST = {
    {"japanese_breaks_between_phrases", japanese_breaks_between_phrases},
    {"kinsoku", kinsoku},
    {"latin_breaks_at_spaces", latin_breaks_at_spaces},
    {"hard_breaks", hard_breaks},
    {"last_break_in_utf16_and_utf8", last_break_in_utf16_and_utf8},
    {NULL, NULL},
};
