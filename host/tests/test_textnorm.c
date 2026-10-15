/* core/textnorm.c: each rule, what it must leave alone, lines, invalid bytes,
 * and that the text never grows (it is normalised in place). */
#include "acutest.h"
#include "textnorm.h"
#include <stdio.h>
#include <string.h>

static char buf[4096];

/* Normalises `in` and checks it reads `want`; returns the changes. */
static unsigned check(const char *in, const char *want, VjoTextNorm *k)
{
    unsigned n;
    snprintf(buf, sizeof(buf), "%s", in);
    n = vjo_text_normalize(buf, k);
    TEST_CHECK(!strcmp(buf, want));
    TEST_MSG("in:   %s\nwant: %s\ngot:  %s", in, want, buf);
    TEST_CHECK(strlen(buf) <= strlen(in));
    return n;
}

static void test_width(void)
{
    VjoTextNorm k;
    TEST_CHECK(check("本当！？　ＯＫ、１２３円", "本当!?　OK、123円", &k) == 7 && k.width == 7);
    TEST_CHECK(check("「ね～」", "「ね~」", &k) == 1 && k.width == 1);
    TEST_CHECK(check("（笑）＆［注］", "(笑)&[注]", &k) == 5 && k.width == 5);
    /* NFKC's range only: Japanese marks and the ideographic space stay */
    TEST_CHECK(check("、。「」『』・ー　", "、。「」『』・ー　", &k) == 0);
}

static void test_tildes_and_dashes(void)
{
    VjoTextNorm k;
    TEST_CHECK(check("えへへ〜∼", "えへへ~~", &k) == 2 && k.tilde == 2);
    TEST_CHECK(check("待って——それは─━‐−", "待って――それは――――", &k) == 6 && k.dash == 6);
    TEST_CHECK(check("Tシャツ-3", "Tシャツ-3", &k) == 0); /* the ASCII hyphen stays */
    TEST_CHECK(check("――", "――", &k) == 0);
}

static void test_ellipses(void)
{
    VjoTextNorm k;
    TEST_CHECK(check("そんな...", "そんな…", &k) == 1 && k.ellipsis == 1);
    TEST_CHECK(check("えっ．．．．．．", "えっ……", &k) == 8 && k.ellipsis == 2 && k.width == 6);
    TEST_CHECK(check("あ....", "あ….", &k) == 1);
    TEST_CHECK(check("1.5", "1.5", &k) == 0);
    TEST_CHECK(check("・・・・・・だ", "……だ", &k) == 2 && k.ellipsis == 2);
    TEST_CHECK(check("え・・", "え…", &k) == 1);
    TEST_CHECK(check("アデル・フラメ", "アデル・フラメ", &k) == 0); /* a lone ・ in a name stays */
    TEST_CHECK(check("……", "……", &k) == 0);
}

static void test_long_vowel_and_kanji_one(void)
{
    VjoTextNorm k;
    /* after katakana, before katakana, a mark or the end */
    TEST_CHECK(check("コ一ヒ一", "コーヒー", &k) == 2 && k.long_vowel == 2);
    TEST_CHECK(check("スーパ一!", "スーパー!", &k) == 1);
    TEST_CHECK(check("ゲ一一ム", "ゲーーム", &k) == 2); /* a run */
    TEST_CHECK(check("セ一ブ", "セーブ", &k) == 1);
    /* after hiragana, before what ends an elongation */
    TEST_CHECK(check("だよ一っ!」", "だよーっ!」", &k) == 1);
    TEST_CHECK(check("こまで一」", "こまでー」", &k) == 1);
    TEST_CHECK(check("すご一っ", "すごーっ", &k) == 1);
    TEST_CHECK(check("ね一~", "ねー~", &k) == 1);
    /* the number one stays: before a kanji or a hiragana, at the start, after a kanji */
    TEST_CHECK(check("コーヒー一杯", "コーヒー一杯", &k) == 0);
    TEST_CHECK(check("ケーキ一つ", "ケーキ一つ", &k) == 0);
    TEST_CHECK(check("もう一つ、他", "もう一つ、他", &k) == 0);
    TEST_CHECK(check("一人で", "一人で", &k) == 0);
    TEST_CHECK(check("たった一人", "たった一人", &k) == 0);
    TEST_CHECK(check("統一。", "統一。", &k) == 0);
    TEST_CHECK(check("嫌そ一な", "嫌そ一な", &k) == 0); /* hira 一 hira is ambiguous ("もう一つ"): left */
    /* the counters ヶ ヵ ゖ follow the number, never a long vowel */
    TEST_CHECK(check("あと一ヶ月", "あと一ヶ月", &k) == 0);
    TEST_CHECK(check("この一ヵ所", "この一ヵ所", &k) == 0);
    TEST_CHECK(check("ハネムーン一ヶ月", "ハネムーン一ヶ月", &k) == 0);
    TEST_CHECK(check("ハネムーン一ヵ月", "ハネムーン一ヵ月", &k) == 0);
    TEST_CHECK(check("あと一ゖ月", "あと一ゖ月", &k) == 0);
    TEST_CHECK(check("ア一ッ", "アーッ", &k) == 1); /* a small kana that is not a counter */
    /* after a hiragana at the end of a line it stays: a wrapped line ends mid-word */
    TEST_CHECK(check("すご一", "すご一", &k) == 0);
    TEST_CHECK(check("もう一", "もう一", &k) == 0);
    TEST_CHECK(check("ただ一\n人", "ただ一\n人", &k) == 0);
    TEST_CHECK(check("の一\nね一!", "の一\nねー!", &k) == 1);
    /* ー between two kanji is 一 */
    TEST_CHECK(check("玄ー郎", "玄一郎", &k) == 1 && k.kanji_one == 1);
    TEST_CHECK(check("統ー的", "統一的", &k) == 1);
    TEST_CHECK(check("超ーかわいい", "超ーかわいい", &k) == 0); /* before a kana it stays */
    TEST_CHECK(check("ラーメン", "ラーメン", &k) == 0);
}

/* A rule never looks across a line break; the break stays. */
static void test_lines(void)
{
    VjoTextNorm k;
    TEST_CHECK(check("コ一\n一人", "コー\n一人", &k) == 1);
    TEST_CHECK(check("カ\n一ド", "カ\n一ド", &k) == 0);
    TEST_CHECK(check("統\nー的", "統\nー的", &k) == 0);
    TEST_CHECK(check("あ..\n.い", "あ..\n.い", &k) == 0);
    TEST_CHECK(check("", "", &k) == 0);
    TEST_CHECK(vjo_text_normalize(NULL, &k) == 0 && !k.width);
    TEST_CHECK(vjo_text_normalize(buf, NULL) == 0); /* no counts wanted */
}

/* Invalid bytes are kept; a cut sequence at the end too. */
static void test_invalid_bytes(void)
{
    VjoTextNorm k;
    TEST_CHECK(check("\xFF！\xE3\x81", "\xFF!\xE3\x81", &k) == 1);
    TEST_CHECK(check("\xE4\xB8", "\xE4\xB8", &k) == 0);
    TEST_CHECK(check("コ\xE4\xB8\x80\xFF", "コー\xFF", &k) == 1);
}

/* The longest text the worker returns: never grows, never reads past it. */
static void test_full_buffer(void)
{
    VjoTextNorm k;
    size_t n = 0;
    while (n + 3 < sizeof(buf) - 1) {
        memcpy(buf + n, "\xEF\xBC\x81", 3); /* ！ */
        n += 3;
    }
    buf[n] = 0;
    TEST_CHECK(vjo_text_normalize(buf, &k) == n / 3 && strlen(buf) == n / 3 && k.width == n / 3);
}

/* The worker's form: the normalised text, then the text as it was after its
 * NUL; a copy that does not fit is cut at a character; no room, no copy; every
 * byte after it zero (the kernel checks the last). */
static void test_keep_raw(void)
{
    static char b[64];
    VjoTextNorm k;
    memset(b, 'x', sizeof(b));
    strcpy(b, "コ一ヒ一！");
    TEST_CHECK(vjo_text_normalize_keep(b, sizeof(b), NULL, &k) == 3 && k.long_vowel == 2 && k.width == 1);
    TEST_CHECK(!strcmp(b, "コーヒー!") && !strcmp(vjo_text_raw(b, sizeof(b)), "コ一ヒ一！"));
    for (size_t i = strlen(b) + 1 + strlen(vjo_text_raw(b, sizeof(b))); i < sizeof(b); i++) TEST_CHECK(!b[i]);
    /* 30 bytes of text in a 48-byte buffer: 16 bytes of copy room, cut to 15 (5 kana) */
    static char c[48];
    memset(c, 'x', sizeof(c));
    strcpy(c, "あいうえおかきくけこ");
    TEST_CHECK(vjo_text_normalize_keep(c, sizeof(c), NULL, NULL) == 0);
    TEST_CHECK(!strcmp(c, "あいうえおかきくけこ") && !strcmp(vjo_text_raw(c, sizeof(c)), "あいうえお") && !c[47]);
    /* the text fills the buffer: normalised, no copy, the tail zeroed */
    static char d[8];
    memcpy(d, "ab\xEF\xBC\x81" "cd", 7); d[7] = 0; /* ab！cd */
    TEST_CHECK(vjo_text_normalize_keep(d, sizeof(d), NULL, NULL) == 1 && !strcmp(d, "ab!cd") && !*vjo_text_raw(d, sizeof(d)));
    TEST_CHECK(!d[5] && !d[6] && !d[7]);
    /* an empty text keeps an empty copy */
    static char e[4] = {0, 'z', 'z', 'z'};
    TEST_CHECK(vjo_text_normalize_keep(e, sizeof(e), NULL, NULL) == 0 && !e[0] && !*vjo_text_raw(e, sizeof(e)) && !e[3]);
    TEST_CHECK(vjo_text_normalize_keep(NULL, 10, NULL, &k) == 0 && !k.width);
    /* another raw text (the greedy one, with the LM): kept, cut to the room left */
    static char f[32], g[24];
    memset(f, 'x', sizeof(f));
    strcpy(f, "ぼくの！");
    TEST_CHECK(vjo_text_normalize_keep(f, sizeof(f), "ばくの！？", NULL) == 1 && !strcmp(f, "ぼくの!"));
    TEST_CHECK(!strcmp(vjo_text_raw(f, sizeof(f)), "ばくの！？") && !f[31]);
    memset(g, 'x', sizeof(g));
    strcpy(g, "ぼくの！");
    TEST_CHECK(vjo_text_normalize_keep(g, sizeof(g), "ばくの！？", NULL) == 1 && !strcmp(g, "ぼくの!"));
    TEST_CHECK(!strcmp(vjo_text_raw(g, sizeof(g)), "ばくの") && !g[23]); /* 10 bytes of room: cut at a character */
}

TEST_LIST = {
    {"keep_the_raw_text_after_the_normalised_one", test_keep_raw},
    {"width_folds_as_nfkc", test_width},
    {"tildes_and_dashes", test_tildes_and_dashes},
    {"ellipses", test_ellipses},
    {"long_vowel_or_kanji_one_by_context", test_long_vowel_and_kanji_one},
    {"lines_are_separate", test_lines},
    {"invalid_bytes_are_kept", test_invalid_bytes},
    {"a_full_buffer_never_grows", test_full_buffer},
    {NULL, NULL}
};
