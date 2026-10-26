/* The recognized text's lines as one running text (core/textjoin.h, owner
 * 2026-10-16 item 4): which line breaks join their lines for the lookup and
 * the Anki sentence, and the in-place join with the offsets that follow it.
 * The texts are synthetic, shaped like the real text boxes the rule was
 * measured on (tools/cross_line_eval.py, docs/touch-lookup.md). */
#include <stdio.h>
#include <string.h>
#include "acutest.h"
#include "textjoin.h"

static int joins_at(const char *text, const char *mark)
{
    const char *at = strstr(text, mark);
    TEST_ASSERT(at != NULL);
    return vjo_text_joins(text, strlen(text), (size_t)(at - text));
}

/* the game's own wrap: a full line, then the rest of the sentence */
static void full_line_joins_the_next(void)
{
    const char *t = "あいうえおかきくけこさし食べ\nるかな？";
    TEST_CHECK(joins_at(t, "\n"));
    /* "\r\n" too, at its '\r' or its '\n' */
    t = "あいうえおかきくけこさし食べ\r\nるかな？";
    TEST_CHECK(joins_at(t, "\r"));
    TEST_CHECK(joins_at(t, "\n"));
    /* the first and second of three: two full lines, then the end */
    t = "あいうえおかきくけこさしすせ\nそたちつてとなにぬねのはひふ\nへほ";
    TEST_CHECK(joins_at(t, "せ\n") == 0); /* a character's own byte is not a break */
    TEST_CHECK(vjo_text_joins(t, strlen(t), (size_t)(strstr(t, "せ\n") - t) + 3));
    TEST_CHECK(vjo_text_joins(t, strlen(t), (size_t)(strstr(t, "ふ\n") - t) + 3));
}

/* A speaker's name, a short line broken by hand: no join. */
static void short_lines_keep_their_break(void)
{
    /* a name line before the dialogue, with or without its brackets */
    TEST_CHECK(!joins_at("健太\nそうだな、お前の言うとおりかもしれない", "\n"));
    TEST_CHECK(!joins_at("【ミナミ】\n分かったよ。それなら明日の朝にもう一度", "\n"));
    TEST_CHECK(!joins_at("健太\n「それでも俺は、最後まであきらめずに走り", "\n"));
    /* fewer than VJO_JOIN_MIN_CHARS characters: no wrapped line is that short */
    TEST_CHECK(!joins_at("ふうん、そうか\nまた明日ね", "\n"));
    TEST_CHECK(joins_at("ふうん、そうなの\nまた明日ね", "\n")); /* 8: full */
    /* shorter than the longest line by more than VJO_JOIN_SLACK */
    TEST_CHECK(!joins_at("今日の練習でも\nいい音が出ていたと思うよ！", "\n"));    /* 7 of 13 */
    TEST_CHECK(!joins_at("今日の練習でもさ\nいい音が出てたと思うよ！", "\n"));    /* 8 of 12 */
    TEST_CHECK(!joins_at("今日の練習でもさあ\nいい音が出てたと思うよ！", "\n"));  /* 9 of 12 */
    TEST_CHECK(joins_at("今日の練習でもさああ\nいい音が出てたと思うよ！", "\n")); /* 10 of 12 */
    /* the longest line may come later */
    TEST_CHECK(!joins_at("あいうえおかきくけ\nこ\nさしすせそたちつてとなにぬね", "け\n"));
}

/* A sentence's end or a quotation's: no join (the next line starts anew). */
static void sentence_ends_and_brackets_keep_their_break(void)
{
    static const char *const ends[] = {"。", "．", ".", "！", "？", "!", "?", "」", "』", "）", ")", "】", "〕", "〉",
                                       "》", "］", "〟", "｡"};
    static const char *const opens[] = {"「", "『", "（", "(", "【", "〔", "〈", "《", "［", "〝"};
    static const char *const goes_on[] = {"、", "…", "ー", "っ", "〜", "~", "・"};
    char t[128];
    for (unsigned i = 0; i < sizeof(ends) / sizeof(ends[0]); i++) {
        snprintf(t, sizeof(t), "あいうえおかきくけこ%s\nさしすせそ", ends[i]);
        TEST_CHECK_(!joins_at(t, "\n"), "%s", ends[i]);
    }
    for (unsigned i = 0; i < sizeof(opens) / sizeof(opens[0]); i++) {
        snprintf(t, sizeof(t), "あいうえおかきくけこ\n%sさしすせそ", opens[i]);
        TEST_CHECK_(!joins_at(t, "\n"), "%s", opens[i]);
    }
    /* a comma, a pause, a long vowel or a small tsu go on */
    for (unsigned i = 0; i < sizeof(goes_on) / sizeof(goes_on[0]); i++) {
        snprintf(t, sizeof(t), "あいうえおかきくけこ%s\nさしすせそ", goes_on[i]);
        TEST_CHECK_(joins_at(t, "\n"), "%s", goes_on[i]);
    }
    /* 〒 and 〓 are not brackets */
    TEST_CHECK(joins_at("あいうえおかきくけこ〓\nさしすせそ", "\n"));
    TEST_CHECK(joins_at("あいうえおかきくけこ\n〒さしすせそ", "\n"));
}

/* Spaces at a line's ends count for nothing; empty lines and the text's ends never join. */
static void spaces_ends_and_empty_lines(void)
{
    TEST_CHECK(joins_at("  あいうえおかきく  \nけこ", "\n"));
    TEST_CHECK(!joins_at("あいうえおかきくけこ。 \nさしすせそ", "\n")); /* a space after 。 ends nothing */
    TEST_CHECK(!joins_at("  あいうえおかき  \nけこ", "\n"));
    TEST_CHECK(joins_at("あいう　えおかき\nけこ", "\n")); /* a space between characters counts */
    TEST_CHECK(!joins_at("あいうえおかきくけこ\n\nさし", "こ\n"));
    TEST_CHECK(!joins_at("あいうえおかきくけこ\n   ", "\n"));
    TEST_CHECK(!joins_at("あいうえおかきくけこ\n", "\n"));
    TEST_CHECK(!joins_at("\nあいうえおかきくけこ", "\n"));
    TEST_CHECK(!vjo_text_joins("あいうえおかきくけこ\r", strlen("あいうえおかきくけこ\r"),
                               strlen("あいうえおかきくけこ")));
    TEST_CHECK(!vjo_text_joins("abc", 3, 3) && !vjo_text_joins("abc", 3, 1) && !vjo_text_joins("", 0, 0));
}

/* A half-width character is half as wide: a line with ASCII in it is as full
 * as the others with fewer characters, and 15 ASCII letters are no full line. */
static void half_width_characters_count_half(void)
{
    /* 18 full-width characters each, the first one's six ASCII among them as three */
    const char *t = "ABC-12日の午前に届いた古い手紙の写し\nそれを読み返して、ひとり考え込む人が\nいた。";
    TEST_CHECK(vjo_text_joins(t, strlen(t), (size_t)(strstr(t, "人が\n") - t) + 6));
    TEST_CHECK(!joins_at("abcdefghijklmno\npqrstuvwxyz", "\n"));    /* 7.5 full-width characters */
    TEST_CHECK(joins_at("abcdefghijklmnop\nqrstuvwxyz", "\n"));     /* 8 */
    TEST_CHECK(joins_at("ｱｲｳｴｵｶｷｸｹｺｻｼｽｾｿﾀ\nﾁﾂﾃﾄ", "\n"));       /* half-width katakana: 8 */
    TEST_CHECK(!joins_at("ｱｲｳｴｵｶｷｸｹｺｻｼｽｾｿ\nﾁﾂﾃﾄ", "\n"));
}

static void join_removes_the_breaks_and_moves_offsets(void)
{
    char t[256] = "健太\n「それでも俺は、最後まであきらめずに走り続けなが\nらも、ずっと前を見ていたんだ」";
    const char *b = strstr(t, "なが\n");
    int offs[5] = {(int)(b - t), (int)(b - t) + 6, (int)(b - t) + 7, -1, 3};
    size_t len = vjo_text_join(t, offs, 5);
    TEST_CHECK(len == strlen(t));
    TEST_CHECK(!strcmp(t, "健太\n「それでも俺は、最後まであきらめずに走り続けながらも、ずっと前を見ていたんだ」"));
    /* the word's start stays; the break's own byte and the one after it both name ら, where the break was */
    TEST_CHECK(offs[0] == (int)(strstr(t, "ながら") - t));
    TEST_CHECK(offs[1] == (int)(strstr(t, "らも") - t) && offs[2] == offs[1]);
    TEST_CHECK(offs[3] == -1 && offs[4] == 3); /* none, and one before every joined break */
    /* "\r\n" is one break; two in a row, each decided on the text before either went */
    char u[256] = "あいうえおかきくけこさし\r\nすせそたちつてとなにぬね\nのは";
    int o[2] = {(int)strlen("あいうえおかきくけこさし\r"), (int)strlen(u)};
    TEST_CHECK(vjo_text_join(u, o, 2) == strlen("あいうえおかきくけこさしすせそたちつてとなにぬねのは"));
    TEST_CHECK(!strcmp(u, "あいうえおかきくけこさしすせそたちつてとなにぬねのは"));
    TEST_CHECK(o[0] == (int)strlen("あいうえおかきくけこさし") && o[1] == (int)strlen(u));
    /* nothing to join: unchanged */
    char v[64] = "猫が\n好き";
    int w[1] = {7};
    TEST_CHECK(vjo_text_join(v, w, 1) == strlen("猫が\n好き") && !strcmp(v, "猫が\n好き") && w[0] == 7);
    char e[1] = "";
    TEST_CHECK(vjo_text_join(e, NULL, 0) == 0);
    /* VJO_JOIN_MAX_BREAKS: the first 32 breaks are decided, later ones stay (more lines than a
     * screen's text box holds) */
    {
        static char many[64 * 40];
        size_t at = 0, left;
        for (int k = 0; k < 34; k++) {
            memcpy(many + at, "あいうえおかきくけこ\n", 31);
            at += 31;
        }
        memcpy(many + at, "さしすせそ", 16);
        TEST_CHECK(vjo_text_join(many, NULL, 0) == at + 15 - 32);
        left = 0;
        for (const char *c = many; *c; c++)
            left += *c == '\n';
        TEST_CHECK_(left == 2, "%zu breaks left", left);
    }
}

/* Vertical text (vita-vn-ocr #99, config.ini ocr_vertical): the game worker returns a region's columns
 * right to left, one per line, so the rule reads them as it reads rows. A vertical box wraps its columns at
 * one height, so a full column joins the next as a full row does (a word across the break is looked up
 * whole); short columns (a manga balloon's, under VJO_JOIN_MIN_CHARS) keep their breaks, as short rows do. */
static int break_joins(const char *text, const char *mark) /* the break that ends `mark` */
{
    const char *at = strstr(text, mark);
    TEST_ASSERT(at != NULL && mark[0] && mark[strlen(mark) - 1] == '\n');
    return vjo_text_joins(text, strlen(text), (size_t)(at - text) + strlen(mark) - 1);
}

static void columns_join_as_lines_do(void)
{
    char t[256] = "神子殿、この京の都には今\nも多くの怨霊がさまよって\nいるのです。\nしかし";
    int offs[1] = {0};
    TEST_CHECK(break_joins(t, "今\n") && break_joins(t, "て\n") && !break_joins(t, "。\n"));
    offs[0] = (int)(strstr(t, "今") - t);
    TEST_CHECK(vjo_text_join(t, offs, 1) == strlen(t) &&
               !strcmp(t, "神子殿、この京の都には今も多くの怨霊がさまよっているのです。\nしかし") &&
               !strncmp(t + offs[0], "今も", strlen("今も")));
    /* a balloon's short columns */
    TEST_CHECK(!break_joins("そんな\nバカな\nことが…", "そんな\n") && !break_joins("そんな\nバカな\nことが…", "カな\n"));
}

TEST_LIST = {
    {"full_line_joins_the_next", full_line_joins_the_next},
    {"short_lines_keep_their_break", short_lines_keep_their_break},
    {"sentence_ends_and_brackets_keep_their_break", sentence_ends_and_brackets_keep_their_break},
    {"spaces_ends_and_empty_lines", spaces_ends_and_empty_lines},
    {"half_width_characters_count_half", half_width_characters_count_half},
    {"join_removes_the_breaks_and_moves_offsets", join_removes_the_breaks_and_moves_offsets},
    {"columns_join_as_lines_do", columns_join_as_lines_do},
    {NULL, NULL},
};
