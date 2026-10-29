#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "acutest.h"
#include "client.h"
#include "config.h"
#include "config_golden.h" /* #418: the parse's golden file */
#include "../include/vjo_game_ocr.h" /* VJO_GAME_OCR_VERTICAL_*: ocr_vertical's values (API 47) */
#include "entries.h"
#include "foreground.h"
#include "furigana.h"
#include "json.h"
#include "styled.h"
#include "vjo_vocr_sizes.h"
#include "regions.h"
#include "render.h"
#include "replay.h"
#include "scene.h"
#include "stick_cursor.h"
#include "textfilter.h"
#include "triggers.h"
#include "utf.h"

static uint8_t g_mem[4u << 20];
static VjoArena A;

static void setup(void)
{
    /* Garbage-fill like a reused arena on the Vita, so code that relies on
     * zeroed memory fails here too. */
    memset(g_mem, 0xA5, sizeof(g_mem));
    vjo_arena_init(&A, g_mem, sizeof(g_mem));
}

/* ---------- pb ---------- */

static void test_buf_interleaved(void)
{
    /* Two buffers growing in turn, with a plain allocation in between: the
     * one that is no longer last moves instead of overwriting the other. */
    VjoBuf x, y;
    char *p;
    setup();
    vjo_buf_init(&x, &A);
    vjo_buf_init(&y, &A);
    for (int i = 0; i < 300; i++) {
        vjo_buf_putc(&x, (char)('a' + i % 26));
        vjo_buf_putc(&y, (char)('A' + i % 26));
        if (i == 150)
            TEST_CHECK(vjo_arena_alloc(&A, 10) != NULL);
    }
    TEST_ASSERT(!x.oom && !y.oom && x.len == 300 && y.len == 300);
    for (int i = 0; i < 300; i++)
        if (x.data[i] != 'a' + i % 26 || y.data[i] != 'A' + i % 26)
            TEST_CHECK_(0, "byte %d", i);
    p = vjo_buf_cstr(&x);
    TEST_CHECK(p && strlen(p) == 300);
}

/* ---------- text ---------- */

static void test_filter_lines(void)
{
    setup();
    TEST_CHECK(!strcmp(vjo_filter_lines(&A, "Score 100\r\nこんにちは\nHP 20/20\n漢字abc\n", 1),
                       "こんにちは\n漢字abc"));
    TEST_CHECK(!strcmp(vjo_filter_lines(&A, "abc\ndef", 1), ""));
    TEST_CHECK(!strcmp(vjo_filter_lines(&A, "abc\nかな", 0), "abc\nかな"));
    /* Full-width punctuation alone (U+3000-303F) is not Japanese */
    TEST_CHECK(!strcmp(vjo_filter_lines(&A, "「」\nア", 1), "ア"));
    TEST_CHECK(!strcmp(vjo_strip_newlines(&A, "a\r\nb\nc\rd\r\r\n"), "abc\rd\r"));
}

static void test_utf(void)
{
    char s[] = "  \t x y \n ";
    size_t i = 0;
    const char *e = "😀a";
    TEST_CHECK(!strcmp(vjo_java_trim(s), "x y"));
    TEST_CHECK(vjo_utf8_next(e, strlen(e), &i) == 0x1F600 && i == 4);
    TEST_CHECK(vjo_utf8_next("\xe3\x81", 2, &(size_t){0}) == 0xFFFD);
}

static void test_utf_character_cursor(void)
{
    const char *text = "猫😀\n文\r?";
    const size_t positions[] = {0, 3, 7, 8, 11, 12};
    const uint32_t starts[] = {0, 1, 3, 4, 5, 5};
    const uint32_t units[] = {1, 2, 1, 1, 0, 1};
    size_t len = strlen(text), cursor = 0;
    uint32_t start, count;
    for (size_t i = 0; i < sizeof(positions) / sizeof(*positions); i++) {
        TEST_CHECK(cursor == positions[i]);
        TEST_CHECK(vjo_utf8_cursor_range(text, len, cursor, &start, &count) == (units[i] != 0));
        TEST_CHECK(start == starts[i] && count == units[i]);
        cursor = vjo_utf8_cursor_step(text, len, cursor, 1);
    }
    TEST_CHECK(cursor == 12); /* End clamps to the last selectable character. */
    for (size_t i = sizeof(positions) / sizeof(*positions); i--;) {
        TEST_CHECK(cursor == positions[i]);
        cursor = vjo_utf8_cursor_step(text, len, cursor, -1);
    }
    TEST_CHECK(cursor == 0);
    TEST_CHECK(vjo_utf8_cursor_step(text, len, 1, 0) == 0);
    TEST_CHECK(vjo_utf8_cursor_step(text, len, 4, 0) == 3);
    TEST_CHECK(vjo_utf8_cursor_step(text, len, 999, 0) == 12);
    TEST_CHECK(vjo_utf8_cursor_range(text, len, 4, &start, &count) && start == 1 && count == 2);
    TEST_CHECK(vjo_utf8_cursor_step("", 0, 999, 1) == 0);
    TEST_CHECK(!vjo_utf8_cursor_range("", 0, 0, &start, &count) && start == 0 && count == 0);

    /* Malformed leading and truncated bytes remain individual U+FFFD
     * characters in both navigation and the actual styled header. */
    text = "\xff\xe3\x81";
    len = 3;
    cursor = 0;
    for (size_t i = 0; i < len; i++) {
        TEST_CHECK(cursor == i);
        TEST_CHECK(vjo_utf8_cursor_range(text, len, cursor, &start, &count) && start == i && count == 1);
        cursor = vjo_utf8_cursor_step(text, len, cursor, 1);
    }
    TEST_CHECK(vjo_utf8_cursor_step(text, len, 2, -1) == 1);
    TEST_CHECK(vjo_utf8_cursor_step(text, len, 1, -1) == 0);
}

/* ---------- json ---------- */

static void test_json(void)
{
    setup();
    {
        VjoJson j;
        const char *js = "[\"a\\u732b\\n\",12]";
        TEST_ASSERT(vjo_json_parse(&A, js, strlen(js), &j) == 0);
        TEST_CHECK(!strcmp(vjo_json_str(&A, &j, 1), "a猫\n") && !strcmp(vjo_json_str(&A, &j, 2), "12"));
    }
    {
        VjoJson j;
        long v;
        const char *js = "[999999999,1000000000,-12,99999999999999999999999,3.7]";
        TEST_ASSERT(vjo_json_parse(&A, js, strlen(js), &j) == 0);
        TEST_CHECK(vjo_json_int(&j, 1, &v) == 0 && v == 999999999);
        TEST_CHECK(vjo_json_int(&j, 2, &v) == -1);
        TEST_CHECK(vjo_json_int(&j, 3, &v) == 0 && v == -12);
        TEST_CHECK(vjo_json_int(&j, 4, &v) == -1);
        TEST_CHECK(vjo_json_int(&j, 5, &v) == 0 && v == 3);
    }
    {
        VjoJson j;
        const char *js = "{\"surrogates\":\"\\u732b \\ud83d\\ude00\"}";
        TEST_ASSERT(vjo_json_parse(&A, js, strlen(js), &j) == 0);
        TEST_CHECK(!strcmp(vjo_json_str(&A, &j, vjo_json_obj_get(&j, 0, "surrogates")), "猫 😀"));
    }
}

/* #528 (Maudit-mem-anki-queue-3-6): strict jsmn takes an object's string without a colon ({"a" "b"}) as one of its
 * members, so the object's size is more than its pairs, and vjo_json_obj_get, walking that size in pairs, read past
 * the token array for a key the object does not have (ASan: a heap-buffer-overflow after the tokens of the audit's
 * {"version" "4","x":1}). Here each object's tokens end where a page no read may touch begins, and every key of every
 * object in it is looked up: each lookup stays within the tokens, and gives -1 or a token of the object (at most
 * the token count, which every accessor takes as none). The Rubi app's export reads such records (a hand edit). */
static void test_json_object_lookups_stay_in_the_tokens(void)
{
    static const char *const objects[] = {
        "{\"version\" \"4\",\"x\":1}", /* the audit's record */
        "{\"a\" \"b\"}", "{\"a\" \"b\" \"c\" \"d\"}", "{\"x\":1,\"a\"}", "{\"a\" \"version\":5}", "{\"x\":\"a\" \"b\"}",
        "{\"more\":[{\"dictionary\" \"d\",\"meanings\":[]},{\"a\" \"b\"}],\"time\" \"1\"}",
        "{\"version\":4,\"spelling\":\"x\",\"more\":[{\"dictionary\":\"d\"}]}", /* (well formed: the same as before) */
    };
    static const char *const keys[] = {"version", "x", "a", "b", "c", "d", "time", "more", "dictionary", "meanings",
                                       "spelling", ""};
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    uint8_t *map = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TEST_ASSERT(map != MAP_FAILED);
    TEST_ASSERT(mprotect(map + page, page, PROT_NONE) == 0);
    for (unsigned o = 0; o < sizeof(objects) / sizeof(objects[0]); o++) {
        const char *js = objects[o];
        size_t bytes;
        VjoArena edge;
        VjoJson j;
        setup();
        TEST_ASSERT(vjo_json_parse(&A, js, strlen(js), &j) == 0);
        bytes = sizeof(jsmntok_t) * (size_t)j.n;
        TEST_ASSERT(bytes <= page);
        vjo_arena_init(&edge, map + page - bytes, bytes); /* the tokens, then the page no read may touch */
        TEST_ASSERT(vjo_json_parse(&edge, js, strlen(js), &j) == 0 && (uint8_t *)(j.t + j.n) == map + page);
        for (int obj = 0; obj < j.n; obj++)
            for (unsigned k = 0; j.t[obj].type == JSMN_OBJECT && k < sizeof(keys) / sizeof(keys[0]); k++) {
                int v = vjo_json_obj_get(&j, obj, keys[k]);
                TEST_CHECK_(v == -1 || (v > obj && v <= j.n), "%s, object %d, \"%s\": %d of %d tokens", js, obj,
                            keys[k], v, j.n);
            }
        if (o == 0) { /* the audit's record: no "time", and "x" is 1 */
            long x = 0;
            TEST_CHECK(vjo_json_obj_get(&j, 0, "time") == -1);
            TEST_CHECK(vjo_json_int(&j, vjo_json_obj_get(&j, 0, "x"), &x) == 0 && x == 1);
        }
        if (o == sizeof(objects) / sizeof(objects[0]) - 1) { /* a well-formed one */
            long v = 0;
            TEST_CHECK(vjo_json_int(&j, vjo_json_obj_get(&j, 0, "version"), &v) == 0 && v == 4);
            TEST_CHECK(vjo_json_obj_get(&j, 0, "more") == 6 && vjo_json_obj_get(&j, 7, "dictionary") == 9 &&
                       vjo_json_obj_get(&j, 0, "time") == -1);
        }
    }
    TEST_CHECK(munmap(map, 2 * page) == 0);
}

/* ---------- dictionary results (as the local lookups make them) ---------- */

typedef struct {
    const char *spelling, *reading;
    int rank;
    const char *meanings[3];
} TestWord;

typedef struct {
    int vocab, pos16, len16;
} TestToken;

/* A VjoDictResult of n words and m tokens, in A. */
static void test_result(VjoDictResult *r, const TestWord *w, int n, const TestToken *t, int m)
{
    memset(r, 0, sizeof(*r));
    r->vocab = vjo_arena_zalloc(&A, sizeof(VjoVocab) * (size_t)(n ? n : 1));
    r->tokens = vjo_arena_zalloc(&A, sizeof(VjoToken) * (size_t)(m ? m : 1));
    TEST_ASSERT(r->vocab && r->tokens);
    for (int i = 0; i < n; i++) {
        VjoVocab *v = &r->vocab[i];
        v->spelling = w[i].spelling;
        v->reading = w[i].reading;
        v->rank = w[i].rank;
        v->meanings = (const char **)w[i].meanings;
        while (v->n_meanings < 3 && w[i].meanings[v->n_meanings])
            v->n_meanings++;
    }
    for (int i = 0; i < m; i++)
        r->tokens[i] = (VjoToken){t[i].vocab, t[i].pos16, t[i].len16};
    r->n_vocab = n;
    r->n_tokens = m;
}

/* Readings for a word as written, from its dictionary form's ruby (cases
 * from real responses of jiten.moe, a removed online dictionary, whose
 * queued cards still carry them: core/anki.c ExpressionFurigana). */
static void test_furigana_align(void)
{
    static const char *const cases[][3] = {
        {"雨が降っていた", "雨[あめ]が降[ふ]る", " 雨[あめ]が 降[ふ]っていた"},
        {"読んでいました", "読[よ]む", " 読[よ]んでいました"},
        {"学校", "学[がっ]校[こう]", " 学[がっ] 校[こう]"},
        {"三日", "三日[みっか]", " 三日[みっか]"},
        {"人々", "人々[ひとびと]", " 人々[ひとびと]"},
        {"一人で", "一人[ひとり]で", " 一人[ひとり]で"},
        {"お前", "お前[まえ]", "お 前[まえ]"},
        {"大人しく", "大人[おとな]しい", " 大人[おとな]しく"},
        {"旅立った", "旅[たび]立[だ]つ", " 旅[たび] 立[だ]った"},
        {"来た", "来[く]る", " 来[き]た"},
        {"来ます", "来[く]る", " 来[き]ます"},
        {"来ない", "来[く]る", " 来[こ]ない"},
        {"来られる", "来[く]る", " 来[こ]られる"},
        {"来れば", "来[く]る", " 来[く]れば"},
        {"来る", "来[く]る", " 来[く]る"},
        {"来い", "来[こ]い", " 来[こ]い"},
        {"来年", "来年[らいねん]", " 来年[らいねん]"},
        {"良かった", "良[い]い", " 良[よ]かった"}, /* 良い read いい: only the plain form */
        {"良い", "良[い]い", " 良[い]い"},
        {"良く", "良[よ]い", " 良[よ]く"},
        {"一人で", "一[ひと]人[り]で", " 一[ひと] 人[り]で"}, /* a word's own furigana (jpdb's tokens) */
        /* one token for 来て下さい: 下 has no reading in the ruby, an
         * auxiliary's is used */
        {"来て下さい", "来[く]る", " 来[き]て 下[くだ]さい"},
        {"待って下さい", "待[ま]つ", " 待[ま]って 下[くだ]さい"},
        {"帰って来た", "帰[かえ]る", " 帰[かえ]って 来[き]た"},
        {"走って行った", "走[はし]る", " 走[はし]って 行[い]った"},
        {"食べて見て", "食[た]べる", " 食[た]べて 見[み]て"},
        {"教えて頂けます", "教[おし]える", " 教[おし]えて 頂[いただ]けます"},
        {"忘れて仕舞った", "忘[わす]れる", " 忘[わす]れて 仕舞[しま]った"},
        {"食べ過ぎた", "食[た]べる", " 食[た]べ 過[す]ぎた"},
        {"見る", "見[み]る", " 見[み]る"},
        {"下を", "下[した]", " 下[した]を"},
        {"行った", "行[ゆ]く", " 行[い]った"}, /* 行く read ゆく */
        {"行く", "行[ゆ]く", " 行[ゆ]く"},
        {"行った", "行[い]く", " 行[い]った"},
        {"好かった", "好[い]い", " 好[よ]かった"},
        {"好い", "好[い]い", " 好[い]い"},
        {"上下", "上下[じょうげ]", " 上下[じょうげ]"},
        {"下手", "下手[へた]", " 下手[へた]"},
        {"下さる", "下[くだ]さる", " 下[くだ]さる"},
        {"手下さえ", "手[て]", " 手[て]下さえ"}, /* an auxiliary only as one */
        {"お待ち下さい", "お待[ま]ち", "お 待[ま]ち 下[くだ]さい"},
        {"見て行く", "見[み]る", " 見[み]て 行[い]く"},
        {"置いて見る", "置[お]く", " 置[お]いて 見[み]る"},
        {"ので", "ので", "ので"},
    };
    setup();
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *f = vjo_furigana_align(&A, cases[i][0], strlen(cases[i][0]), cases[i][1]);
        TEST_CHECK(f && !strcmp(f, cases[i][2]));
        TEST_MSG("%s %s: %s", cases[i][0], cases[i][1], f ? f : "(null)");
    }
    TEST_CHECK(!strcmp(vjo_furigana_align(&A, "猫", strlen("猫"), NULL), "猫"));
    TEST_CHECK(!strcmp(vjo_furigana_align(&A, "降っ\nた", strlen("降っ\nた"), "降[ふ]る"), " 降[ふ]っ\nた"));
    /* a line break of the OCR inside a base: read across it, after it in Anki's */
    TEST_CHECK(!strcmp(vjo_furigana_align(&A, "今\n日は", strlen("今\n日は"), "今日[きょう]"), " 今日[きょう]\nは"));
    TEST_CHECK(!strcmp(vjo_furigana_align(&A, "出\r\n来た", strlen("出\r\n来た"), "出来[でき]る"), " 出来[でき]\r\nた"));
    TEST_CHECK(!strcmp(vjo_furigana_align(&A, "学\n校", strlen("学\n校"), "学[がっ]校[こう]"), " 学[がっ]\n 校[こう]"));
}

/* The readings as ranges of the word, a line break inside one included. */
static void test_furigana_rubies(void)
{
    VjoRuby r[8];
    const char *s = "雨が降\nっ\n今\n日";
    int n = vjo_furigana_rubies(s, strlen(s), "雨[あめ]が降[ふ]る今日[きょう]", r, 8);
    TEST_ASSERT(n == 3);
    TEST_CHECK(r[0].start == 0 && r[0].end == 3 && r[0].reading_len == 6 && !memcmp(r[0].reading, "あめ", 6));
    TEST_CHECK(r[1].start == 6 && r[1].end == 9 && !memcmp(r[1].reading, "ふ", 3));
    TEST_CHECK(r[2].start == 14 && r[2].end == 21 && !memcmp(r[2].reading, "きょう", 9));
    TEST_CHECK(vjo_furigana_rubies(s, strlen(s), "雨[あめ]が降[ふ]る今日[きょう]", r, 2) == 2);
    TEST_CHECK(vjo_furigana_rubies("猫", strlen("猫"), NULL, r, 8) == 0);
}

static void test_entries(void)
{
    VjoDictResult r;
    VjoEntryList l;
    static const TestWord words[] = {{"が", "が", 50, {"indicates subject"}},
                                     {"猫", "ねこ", 1500, {"cat", "feline"}},
                                     {"好き", "好き", VJO_NO_RANK, {"liked"}}};
    static const TestToken tokens[] = {{1, 0, 1}, {0, 1, 1}, {2, 2, 2}};
    static const TestWord twice[] = {{"猫", "ねこ", 1500, {"cat"}}, {"と", "と", 20, {"and"}},
                                     {"犬", "いぬ", 900, {"dog"}}};
    static const TestToken twice_tokens[] = {{0, 0, 1}, {1, 1, 1}, {0, 2, 1}};
    setup();
    test_result(&r, words, 3, tokens, 3);
    /* the lookup's text "猫が好き" came from filtered "猫が\n好き" */
    TEST_ASSERT(vjo_entries_build(&A, "猫が\n好き", &r, &l) == 0);
    TEST_CHECK(!strcmp(l.header, "猫が\n好き"));
    /* every word gets an entry, in text order; a ranked one its badge, an
     * unranked one none without a frequency list */
    TEST_CHECK(l.n_entries == 3);
    TEST_CHECK(!strcmp(l.entries[0].text, "猫 (ねこ) #1500\ncat\nfeline"));
    TEST_CHECK(!strcmp(l.entries[1].text, "が #50\nindicates subject"));
    TEST_CHECK(!strcmp(l.entries[2].text, "好き\nliked"));
    TEST_CHECK(!strcmp(vjo_render_highlight(&A, &l, 0), "【猫】が\n好き"));
    TEST_CHECK(!strcmp(vjo_render_highlight(&A, &l, 1), "猫【が】\n好き"));
    TEST_CHECK(!strcmp(vjo_render_highlight(&A, &l, 2), "猫が\n【好き】"));
    TEST_CHECK(!strcmp(vjo_entries_body(&A, &l),
                       "猫 (ねこ) #1500\ncat\nfeline\n\nが #50\nindicates subject\n\n好き\nliked"));
    /* with a frequency list (a v3 dictionary's), an unranked word says so */
    r.source = "Jiten";
    TEST_ASSERT(vjo_entries_build(&A, "猫が\n好き", &r, &l) == 0);
    TEST_CHECK(!strcmp(l.source, "Jiten") && !strcmp(l.entries[0].text, "猫 (ねこ) #1500\ncat\nfeline"));
    TEST_CHECK(!strcmp(l.entries[2].text, "好き unranked\nliked"));
    {
        char badge[VJO_RANK_BADGE_MAX];
        TEST_CHECK(!strcmp(vjo_rank_badge(badge, 99999, NULL), " #99999")); /* a rank, as in Jiten's list */
        TEST_CHECK(!strcmp(vjo_rank_badge(badge, 16777215, "Jiten"), " #16777215"));
        TEST_CHECK(!strcmp(vjo_rank_badge(badge, VJO_NO_RANK, "Jiten"), " unranked"));
        TEST_CHECK(!strcmp(vjo_rank_badge(badge, -5, NULL), ""));
    }

    /* a word used twice: an entry for each, in text order; a word without
     * a token comes last */
    test_result(&r, twice, 3, twice_tokens, 3);
    TEST_ASSERT(vjo_entries_build(&A, "猫と猫", &r, &l) == 0);
    TEST_CHECK(l.n_entries == 4);
    TEST_CHECK(!strcmp(vjo_render_highlight(&A, &l, 0), "【猫】と猫"));
    TEST_CHECK(!strcmp(vjo_render_highlight(&A, &l, 1), "猫【と】猫"));
    TEST_CHECK(!strcmp(vjo_render_highlight(&A, &l, 2), "猫と【猫】"));
    TEST_CHECK(l.entries[2].vocab == l.entries[0].vocab);
    TEST_CHECK(!strcmp(l.entries[3].text, "犬 (いぬ) #900\ndog") && l.entries[3].hl_start == -1);
}

static void test_entries_surrogates_and_trim(void)
{
    VjoDictResult r;
    VjoEntryList l;
    static const TestWord cat[] = {{"猫", "ねこ", 900, {NULL}}}, two[] = {{"猫", "ねこ", 900, {NULL}},
                                                                     {"犬", "いぬ", 800, {NULL}}};
    /* "😀猫" : 猫 is at UTF-16 index 2 */
    static const TestToken at2[] = {{0, 2, 1}}, at4[] = {{0, 4, 1}};
    static const TestToken past[] = {{0, 4, 999999999}, {1, 999999999, 5}};
    setup();
    test_result(&r, cat, 1, at2, 1);
    TEST_ASSERT(vjo_entries_build(&A, "  😀猫 ", &r, &l) == 0);
    /* index 2 is the emoji's high surrogate: the highlight covers whole
     * code points (lookups index the untrimmed text, 猫 is at index 4) */
    TEST_CHECK(l.entries[0].hl_start == 0 && l.entries[0].hl_end == 4);
    TEST_CHECK(!strcmp(vjo_render_highlight(&A, &l, 0), "【😀】猫"));
    test_result(&r, cat, 1, at4, 1);
    TEST_ASSERT(vjo_entries_build(&A, "  😀猫 ", &r, &l) == 0);
    TEST_CHECK(!strcmp(l.header, "😀猫"));
    TEST_CHECK(!strcmp(vjo_render_highlight(&A, &l, 0), "😀【猫】"));
    TEST_CHECK(!strcmp(l.entries[0].text, "猫 (ねこ) #900"));

    /* server positions past the text or huge lengths are clamped */
    test_result(&r, two, 2, past, 2);
    TEST_ASSERT(vjo_entries_build(&A, "  😀猫 ", &r, &l) == 0);
    TEST_CHECK(!strcmp(vjo_render_highlight(&A, &l, 0), "😀【猫】"));
    TEST_CHECK(l.entries[1].hl_start == -1);
    /* pos + len would overflow int (beyond what the JSON parsers accept):
     * the end saturates */
    test_result(&r, cat, 1, at4, 1);
    r.tokens[0].len16 = 0x7fffffff;
    TEST_ASSERT(vjo_entries_build(&A, "  😀猫 ", &r, &l) == 0);
    TEST_CHECK(!strcmp(vjo_render_highlight(&A, &l, 0), "😀【猫】"));
}

/* ---------- kernel foreground tracking ---------- */

enum { SHELL = 0x100, GAME = 0x200, APP = 0x300, APP2 = 0x400, SHELL2 = 0x500 };

static int fg_is(const VjoForeground *f, int game_pid, int game_active, int prev)
{
    return f->game_pid == game_pid && f->game_active == game_active && f->prev == prev;
}

/* GAME in the foreground and confirmed by the shell. */
static void fg_game_running(VjoForeground *f)
{
    uint32_t iev;
    memset(f, 0, sizeof(*f));
    f->shell_pid = SHELL;
    TEST_CHECK(fg_process_event(f, SHELL, PROCEV_RESUME) == 0); /* the shell never is the game */
    TEST_CHECK(fg_process_event(f, GAME, PROCEV_STARTUP) == IEV_GAME_START);
    TEST_CHECK(fg_is(f, GAME, 0, 0));
    TEST_CHECK(fg_set_game_active(f, GAME, VJO_GAME, &iev) == 0 && iev == IEV_ACTIVATE);
    TEST_CHECK(fg_is(f, GAME, VJO_GAME, 0));
    TEST_CHECK(fg_process_event(f, GAME, PROCEV_RESUME) == 0); /* already the foreground */
}

static void test_foreground(void)
{
    VjoForeground f;
    uint32_t iev;

    /* A non-game starts over the game; the shell says it is not a game, so
     * the game comes back (inactive until the shell re-classifies it). */
    fg_game_running(&f);
    TEST_CHECK(fg_process_event(&f, APP, PROCEV_STARTUP) == IEV_GAME_START);
    TEST_CHECK(fg_is(&f, APP, 0, GAME));
    TEST_CHECK(fg_set_game_active(&f, GAME, VJO_GAME, &iev) < 0 && iev == 0); /* not the foreground */
    TEST_CHECK(fg_is(&f, APP, 0, GAME));
    TEST_CHECK(fg_set_game_active(&f, APP, VJO_GAME_NONE, &iev) == 0 && iev == IEV_GAME_START);
    TEST_CHECK(fg_is(&f, GAME, 0, 0));
    TEST_CHECK(fg_set_game_active(&f, GAME, VJO_GAME, &iev) == 0 && iev == IEV_ACTIVATE);
    TEST_CHECK(fg_is(&f, GAME, VJO_GAME, 0));

    /* The newcomer exits before it is classified: back to the game. */
    fg_game_running(&f);
    TEST_CHECK(fg_process_event(&f, APP, PROCEV_STARTUP) == IEV_GAME_START);
    TEST_CHECK(fg_process_event(&f, APP, PROCEV_EXIT) == (IEV_GAME_EXIT | IEV_GAME_START));
    TEST_CHECK(fg_is(&f, GAME, 0, 0));

    /* The game suspends while behind the newcomer: it is forgotten, the
     * newcomer's non-game verdict changes nothing, and the game's resume
     * makes it the foreground again. */
    fg_game_running(&f);
    TEST_CHECK(fg_process_event(&f, APP, PROCEV_STARTUP) == IEV_GAME_START);
    TEST_CHECK(fg_process_event(&f, GAME, PROCEV_SUSPEND) == 0);
    TEST_CHECK(fg_is(&f, APP, 0, 0));
    TEST_CHECK(fg_set_game_active(&f, APP, VJO_GAME_NONE, &iev) == 0 && iev == 0);
    TEST_CHECK(fg_is(&f, APP, 0, 0));
    TEST_CHECK(fg_process_event(&f, GAME, PROCEV_RESUME) == IEV_GAME_START);
    TEST_CHECK(fg_is(&f, GAME, 0, 0));
    TEST_CHECK(fg_process_event(&f, APP, PROCEV_EXIT) == 0);
    TEST_CHECK(fg_is(&f, GAME, 0, 0));

    /* A second newcomer arrives before the first is classified: the game
     * stays behind both, the first's late verdict is refused, and the
     * second's brings the game back. */
    fg_game_running(&f);
    TEST_CHECK(fg_process_event(&f, APP, PROCEV_STARTUP) == IEV_GAME_START);
    TEST_CHECK(fg_process_event(&f, APP2, PROCEV_STARTUP) == IEV_GAME_START);
    TEST_CHECK(fg_is(&f, APP2, 0, GAME));
    TEST_CHECK(fg_set_game_active(&f, APP, VJO_GAME_NONE, &iev) < 0 && iev == 0);
    TEST_CHECK(fg_is(&f, APP2, 0, GAME));
    TEST_CHECK(fg_set_game_active(&f, APP2, VJO_GAME_NONE, &iev) == 0 && iev == IEV_GAME_START);
    TEST_CHECK(fg_is(&f, GAME, 0, 0));
    TEST_CHECK(fg_process_event(&f, APP, PROCEV_EXIT) == 0);
    TEST_CHECK(fg_is(&f, GAME, 0, 0));

    /* SceShell restarts: the old one exits, the new one starts before it
     * registers (so it is a newcomer over the game), registers, and
     * classifies itself as not a game. */
    fg_game_running(&f);
    TEST_CHECK(fg_process_event(&f, SHELL, PROCEV_EXIT) == 0);
    TEST_CHECK(fg_is(&f, GAME, VJO_GAME, 0));
    TEST_CHECK(fg_process_event(&f, SHELL2, PROCEV_STARTUP) == IEV_GAME_START);
    TEST_CHECK(fg_is(&f, SHELL2, 0, GAME));
    f.shell_pid = SHELL2; /* vjoRegisterShell */
    TEST_CHECK(fg_set_game_active(&f, SHELL2, VJO_GAME_NONE, &iev) == 0 && iev == IEV_GAME_START);
    TEST_CHECK(fg_is(&f, GAME, 0, 0));
    TEST_CHECK(fg_process_event(&f, SHELL2, PROCEV_RESUME) == 0);
    TEST_CHECK(fg_set_game_active(&f, GAME, VJO_GAME, &iev) == 0 && iev == IEV_ACTIVATE);
    TEST_CHECK(fg_is(&f, GAME, VJO_GAME, 0));

    /* A static framebuffer game keeps its mode until a system app opens over
     * it: it comes back inactive, to be re-classified by the shell. */
    fg_game_running(&f);
    TEST_CHECK(fg_set_game_active(&f, GAME, VJO_GAME_STATIC_FB, &iev) == 0 && iev == IEV_ACTIVATE);
    TEST_CHECK(fg_is(&f, GAME, VJO_GAME_STATIC_FB, 0));
    TEST_CHECK(fg_process_event(&f, APP, PROCEV_STARTUP) == IEV_GAME_START);
    TEST_CHECK(fg_is(&f, APP, 0, GAME));
    TEST_CHECK(fg_set_game_active(&f, APP, VJO_GAME_NONE, &iev) == 0 && iev == IEV_GAME_START);
    TEST_CHECK(fg_is(&f, GAME, 0, 0));
    TEST_CHECK(fg_set_game_active(&f, GAME, VJO_GAME_STATIC_FB, &iev) == 0 && iev == IEV_ACTIVATE);
    TEST_CHECK(fg_process_event(&f, GAME, PROCEV_EXIT) == IEV_GAME_EXIT);
    TEST_CHECK(fg_is(&f, 0, 0, 0));

    /* The game exits with nothing behind it; pid 0 is never the foreground. */
    fg_game_running(&f);
    TEST_CHECK(fg_process_event(&f, GAME, PROCEV_EXIT) == IEV_GAME_EXIT);
    TEST_CHECK(fg_is(&f, 0, 0, 0));
    TEST_CHECK(fg_set_game_active(&f, 0, VJO_GAME, &iev) < 0);
}

/* ---------- config / regions ---------- */

static void test_japanese_character_cursor(void)
{
    const char *text = "「ABC 123 한글、猫。ひｶー𠮷！犬」";
    const char *letters[] = {"猫", "ひ", "ｶ", "ー", "𠮷", "犬"};
    size_t len = strlen(text), positions[6];
    for (size_t i = 0; i < 6; i++) positions[i] = (size_t)(strstr(text, letters[i]) - text);
    size_t cursor = vjo_utf8_japanese_cursor_step(text, len, 0, 0);
    for (size_t i = 0; i < 6; i++) {
        TEST_CHECK(cursor == positions[i]);
        cursor = vjo_utf8_japanese_cursor_step(text, len, cursor, 1);
    }
    TEST_CHECK(cursor == positions[5]);
    for (size_t i = 6; i-- > 0;) {
        TEST_CHECK(cursor == positions[i]);
        cursor = vjo_utf8_japanese_cursor_step(text, len, cursor, -1);
    }
    TEST_CHECK(cursor == positions[0]);
    TEST_CHECK(vjo_utf8_japanese_cursor_step(text, len, positions[4] + 1, 0) == positions[4]);
    TEST_CHECK(vjo_utf8_japanese_cursor_step(text, len, len + 99, 0) == positions[5]);
    TEST_CHECK(vjo_utf8_cursor_step(text, len, 0, 0) == 0); /* OFF retains punctuation selection. */
    TEST_CHECK(vjo_is_japanese_letter(0x3005) && vjo_is_japanese_letter(0x3099));
    TEST_CHECK(vjo_is_japanese_letter(0x3400) && vjo_is_japanese_letter(0x9FFF));
    TEST_CHECK(vjo_is_japanese_letter(0x1B000) && vjo_is_japanese_letter(0x20000));
    const uint32_t excluded[] = {'A', '5', 0xFF15, 0x3001, 0x300C, 0x30FB, 0xFF65,
                                  0xAC00, 0x1100, 0x3105, 0x1F600, 0xFFFD, 0x110000};
    for (size_t i = 0; i < sizeof(excluded) / sizeof(excluded[0]); i++)
        TEST_CHECK(!vjo_is_japanese_letter(excluded[i]));
    text = "ABC 123 한글・「」😀"; len = strlen(text);
    for (int direction = -1; direction <= 1; direction++)
        TEST_CHECK(vjo_utf8_japanese_cursor_step(text, len, 0, direction) == len);
    TEST_CHECK(vjo_utf8_japanese_cursor_step("", 0, 99, 1) == 0);
    text = "\x80" "猫\xE3\x81"; len = strlen(text);
    TEST_CHECK(vjo_utf8_japanese_cursor_step(text, len, 0, 0) == 1);
    TEST_CHECK(vjo_utf8_japanese_cursor_step(text, len, 1, 1) == 1);
    text = "\xF0\x83\x81\x82" "A"; len = strlen(text); /* Overlong U+3042. */
    TEST_CHECK(vjo_utf8_japanese_cursor_step(text, len, 0, 0) == len);
    TEST_CHECK(vjo_utf8_cursor_step(text, len, 0, 1) == 4); /* Legacy behavior stays unchanged. */
    text = "\xF0\x83\x81\x82" "猫"; len = strlen(text);
    TEST_CHECK(vjo_utf8_japanese_cursor_step(text, len, 0, 0) == 4);
    TEST_CHECK(vjo_utf8_japanese_cursor_step(text, len, 4, -1) == 4);
    uint32_t start, units;
    text = "ABC𠮷"; len = strlen(text);
    cursor = vjo_utf8_japanese_cursor_step(text, len, 0, 0);
    TEST_CHECK(vjo_utf8_cursor_range(text, len, cursor, &start, &units) && start == 3 && units == 2);
}

static void test_config(void)
{
    VjoConfig c;
    const char *ini = "\xEF\xBB\xBF; comment\n"
                      "dictionary = Local\n"
                      "hachidori_host = 192.168.1.20\n"
                      "jpdb_api_key = abc123 ; my key\n"
                      "jiten_api_key = ak_xyz\n"
                      "frequency_filter=500\n"
                      "non_japanese_filter = none\r\n"
                      "font_size_ja = 99\n"
                      "font_size_en = 10\n"
                      "toggle_button = Select+R\n"
                      "subtitle_button = select\n"
                      "ocr_mode = on_press\n"
                      "log_host = 192.168.1.5\n"
                      "log_file = on\n"
                      "bogus = 1\n";
    vjo_config_defaults(&c);
    TEST_CHECK(c.font_size_ja == 18 && c.font_size_en == 14 &&
               c.toggle_button == VJO_TRIGGER_L_R && c.japanese_only_selection == 0); /* OFF since 2026-10-20 */
    vjo_config_parse(&c, ini, strlen(ini));
    TEST_CHECK(!strcmp(VJO_DICT_NAME, "Local dictionaries"));
    TEST_CHECK(c.non_japanese_filter == VJO_FILTER_NONE);
    TEST_CHECK(c.font_size_ja == 18); /* invalid -> default */
    TEST_CHECK(c.font_size_en == 10);
    TEST_CHECK(c.toggle_button == VJO_TRIGGER_SELECT_R && c.subtitle_button == VJO_TRIGGER_SELECT);
    TEST_CHECK(c.ocr_mode == VJO_OCR_ON_PRESS);
    TEST_CHECK(c.log_file == 1);
    /* font_size_ja and bogus; the removed settings (the dictionary choice, frequency_filter, hachidori_host, the API
     * keys, the UDP log's log_host) are silent (#418) */
    TEST_CHECK(c.n_warnings == 2 && !strcmp(c.warnings[1], "unknown setting 'bogus'"));
    TEST_MSG("warnings: %d, the second '%s'", c.n_warnings, c.n_warnings > 1 ? c.warnings[1] : "");
    /* an empty log_host (the old template's line) says nothing: the app's save and the installer drop it */
    vjo_config_defaults(&c);
    vjo_config_parse(&c, "log_host =\nLOG_HOST=\n", strlen("log_host =\nLOG_HOST=\n"));
    TEST_CHECK(c.n_warnings == 0);
    /* jpdb.io and jiten.moe, the removed online dictionaries, and the removed
     * Hachidori relay: local, without a warning (#418: the dictionary choice is a
     * removed setting), in any case; the online ones' API keys (and api_key,
     * jpdb's older name) are ignored without one too, so no warning shows a
     * key's line, and so is the relay's hachidori_host. */
    {
        static const char *const removed[] = {
            "dictionary = jpdb\n", "dictionary = JITEN ; old\n", "Dictionary = Jpdb\r\n",
            "dictionary = hachidori\nhachidori_host = 192.168.1.20:19633\n",
            "hachidori_host = anki.local ; LAN\nDICTIONARY = Hachidori\n",
            "dictionary = hachidori\nhachidori_host = http://not a host\n"};
        VjoConfig d;
        vjo_config_defaults(&d);
        for (size_t i = 0; i < sizeof(removed) / sizeof(removed[0]); i++) {
            vjo_config_defaults(&c);
            vjo_config_parse(&c, removed[i], strlen(removed[i]));
            TEST_CHECK_(!memcmp(&c, &d, sizeof(c)), "%s: %d warnings", removed[i], c.n_warnings);
        }
        /* also a typo's line (the key glued to the name, "secret-8 ; x = y" read as the key),
         * and one too long to read, which would otherwise warn "value too long" */
        static char keys[2048];
        int k = snprintf(keys, sizeof(keys), "dictionary = local\njpdb_api_key = secret-1\njiten_api_key = secret-2 ; mine\n"
                         "api_key = secret-3\nJPDB_API_KEY=secret-4\njiten_api_key secret-5\n"
                         "jpdb_api_key: secret-6\n  Jiten_Api_Key\tsecret-7\njpdb_api_key secret-8 ; x = y\n"
                         "jpdb_api_keysecret-9\napi_keys = secret-10\njpdb_api_key = ");
        memset(keys + k, 'S', 1500);
        memcpy(keys + k + 1500, "\n", 2);
        vjo_config_defaults(&c);
        vjo_config_parse(&c, keys, strlen(keys));
        TEST_CHECK(c.n_warnings == 0);
        TEST_MSG("%d warnings, first: %s", c.n_warnings, c.n_warnings ? c.warnings[0] : "");
        /* nothing of them is kept anywhere: every byte is the defaults' (audit N6, M2) */
        {
            VjoConfig d;
            vjo_config_defaults(&d);
            TEST_CHECK(!memcmp(&c, &d, sizeof(c)));
        }
        /* another line without '=' is named by its number (never quoted), another unknown key by its name */
        const char *others = "jpdb_api_key_old = x\nbogus line\nlens_api_key = y\n";
        vjo_config_defaults(&c);
        vjo_config_parse(&c, others, strlen(others));
        TEST_CHECK(c.n_warnings == 2 && !strcmp(c.warnings[0], "ignored line 2 (no '=')") &&
                   !strcmp(c.warnings[1], "unknown setting 'lens_api_key'"));
        /* local in any case, without a warning; another value is invalid and stays local (the line is read as on
         * main: only a removed dictionary's value lost its warning, #418's audit F1) */
        static const char *const values[] = {"dictionary = LOCAL\n", "dictionary = hachidorix\n",
                                             "dictionary = relay\n", "dictionary =\n"};
        for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
            vjo_config_defaults(&c);
            vjo_config_parse(&c, values[i], strlen(values[i]));
            TEST_CHECK(c.n_warnings == (i ? 1 : 0));
            TEST_CHECK(!i || strstr(c.warnings[0], "dictionary: invalid value") != NULL);
            TEST_MSG("%s", values[i]);
        }
        /* a dictionary line without '=' is a malformed line, warned by its number as any other */
        vjo_config_defaults(&c);
        vjo_config_parse(&c, "dictionary\n", 11);
        TEST_CHECK(c.n_warnings == 1 && !strcmp(c.warnings[0], "ignored line 1 (no '=')"));
        /* The ID the self-test reports (config dictionary=) does not move. */
        TEST_CHECK(VJO_DICT_LOCAL == 3);
    }
    /* The lines the Rubi app's saves rewrite (core/config.h vjo_config_removed_line). */
    {
        static const char *const drop[] = {"jpdb_api_key = x\n", "  JITEN_API_KEY=x", "api_key = x\r\n",
                                           "jiten_api_key x\n", "jpdb_api_key: x\n", "jpdb_api_key =\n",
                                           "jpdb_api_keyx\n", "jpdb_api_key_old = x\n", "api_keys = x\n",
                                           "\tJiten_Api_Key_typo: x",
                                           /* the removed Anki settings (AnkiConnect's, the field mapping) */
                                           "anki_host = jpdb\n", "ANKI_HOST=10.0.0.2", "anki_audio_url = https://a/?k=1\n",
                                           "anki_audio_url https://a/?k=1\n", "anki_deck = Mining\n",
                                           "Anki_Field_Word = Word\n",
                                           /* a removed name alone on its line (audit-overlay-apkg-b D5) */
                                           "anki_host\n", "anki_audio_url\n", "ANKI_TAGS", "anki_deck\r\n"};
        static const char *const replace[] = {"dictionary = jpdb\n", "Dictionary=JITEN ; old\r\n",
                                              "  dictionary =\tjpdb", "dictionary = jiten # x\n",
                                              "dictionary = hachidori\n", "DICTIONARY = Hachidori ; relay\r\n",
                                              "dictionary=HACHIDORI"};
        static const char *const keep[] = {"dictionary = local\n", "dictionary = hachidorix\n", "dictionary = jpdbx\n",
                                           "hachidori_host = 192.168.1.20\n", "dictionary = hachidori-relay\n",
                                           "; dictionary = jpdb\n", "dictionaryx = jpdb\n", "lens_api_key = x\n",
                                           "ocr_backendx = lens\n", "; ocr_backend = lens\n", "ocr_backend lens\n",
                                           "my_api_key = x\n", "anki_mining = on\n", "anki_hosts = x\n",
                                           "anki_field = x\n", "anki_hosts\n", "anki_mining\n", "jpdb_api_ke = x\n",
                                           "; jpdb_api_key = x\n", "", "\n", "dictionary\n", "dictionary = \n",
                                           "log_hosts = x\n", "; log_host = 10.0.0.1\n", "log_host 10.0.0.1\n",
                                           "log_file = on\n"};
        const char *r;
        for (size_t i = 0; i < sizeof(drop) / sizeof(drop[0]); i++) {
            TEST_CHECK(vjo_config_removed_line(drop[i], strlen(drop[i]), &r) == VJO_LINE_DROP && !r);
            TEST_MSG("%s", drop[i]);
        }
        for (size_t i = 0; i < sizeof(replace) / sizeof(replace[0]); i++) {
            TEST_CHECK(vjo_config_removed_line(replace[i], strlen(replace[i]), &r) == VJO_LINE_REPLACE &&
                       r && !strcmp(r, "dictionary = local\n"));
            TEST_MSG("%s", replace[i]);
        }
        for (size_t i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) {
            TEST_CHECK(vjo_config_removed_line(keep[i], strlen(keep[i]), &r) == VJO_LINE_KEEP && !r);
            TEST_MSG("%s", keep[i]);
        }
        /* The removed engine choice and UDP log: dropped whatever their value, and not secrets. */
        static const char *const setting[] = {"ocr_backend = lens\n", "ocr_backend = vocr\n", "OCR_BACKEND=Lens\r\n",
                                              "  ocr_backend =\n", "ocr_backend = vocr2 ; old\n", "ocr_backend\t= meiki",
                                              "log_host =\n", "log_host = 192.168.1.5\n", "LOG_HOST=10.0.0.1 ; pc\r\n",
                                              "\tlog_host\t= x"};
        for (size_t i = 0; i < sizeof(setting) / sizeof(setting[0]); i++) {
            TEST_CHECK(vjo_config_removed_line(setting[i], strlen(setting[i]), &r) == VJO_LINE_DROP_SETTING && !r);
            TEST_MSG("%s", setting[i]);
        }
    }
    /* Japanese characters only (OFF by default): on and off, in either letter case */
    vjo_config_defaults(&c);
    const char *selection_on = "japanese_only_selection = ON\nocr_backend = VOCR\n";
    vjo_config_parse(&c, selection_on, strlen(selection_on));
    TEST_CHECK(c.japanese_only_selection == 1 && c.n_warnings == 0);
    const char *selection_off = "japanese_only_selection = Off\n";
    vjo_config_parse(&c, selection_off, strlen(selection_off));
    TEST_CHECK(!c.japanese_only_selection && c.n_warnings == 0);
    /* Anki mining is off unless anki_mining says on; an older file's anki_host
     * (AnkiConnect's, removed) is ignored without a warning */
    vjo_config_defaults(&c);
    TEST_CHECK(!c.anki_mining);
    const char *mining = "anki_host = 192.168.1.23\nanki_mining = ON\n";
    vjo_config_parse(&c, mining, strlen(mining));
    TEST_CHECK(c.anki_mining == 1 && c.n_warnings == 0);
    mining = "anki_mining = off\n";
    vjo_config_parse(&c, mining, strlen(mining));
    TEST_CHECK(!c.anki_mining && c.n_warnings == 0);
    mining = "anki_mining = yes\n";
    vjo_config_parse(&c, mining, strlen(mining));
    TEST_CHECK(!c.anki_mining && c.n_warnings == 1);
    /* One engine, vocr: the engine choice is gone (core/config.h), a removed setting: its line is ignored without a
     * warning whatever it names (vocr, Google Lens, the VOCR2 and Meiki trials, ncnn of the host tools, anything else;
     * #418), in any case, and nothing else changes. */
    {
        static const char *const removed[] = {
            "ocr_backend = lens\n", "ocr_backend = Lens\n", "ocr_backend = vocr2\n", "ocr_backend = VOCR2\n",
            "ocr_backend = Meiki\n", "OCR_BACKEND = meiki ; old\n", "ocr_backend = ncnn\n", "ocr_backend = NCNN\n",
            "ocr_backend = vocr3\n", "ocr_backend = google\n", "ocr_backend =\n", "ocr_backend = vocr22\n",
            "ocr_backend\n", "ocr_backend lens\n"};
        VjoConfig d;
        vjo_config_defaults(&d);
        for (size_t i = 0; i < sizeof(removed) / sizeof(removed[0]); i++) {
            vjo_config_defaults(&c);
            vjo_config_parse(&c, removed[i], strlen(removed[i]));
            TEST_CHECK_(!memcmp(&c, &d, sizeof(c)), "%s: %d warnings", removed[i], c.n_warnings);
        }
        const char *engines = "ocr_backend = VOCR\nvocr_tier = large\n";
        vjo_config_defaults(&c);
        vjo_config_parse(&c, engines, strlen(engines));
        TEST_CHECK(!c.n_warnings && c.vocr_tier == VJO_VOCR_TIER_LARGE);
    }
    /* an invalid value keeps what was there: the default (OFF), or a line's ON before it */
    vjo_config_defaults(&c);
    const char *invalid_selection = "japanese_only_selection = maybe\n";
    vjo_config_parse(&c, invalid_selection, strlen(invalid_selection));
    TEST_CHECK(!c.japanese_only_selection && c.n_warnings == 1);
    vjo_config_defaults(&c);
    invalid_selection = "japanese_only_selection = on\njapanese_only_selection = maybe\n";
    vjo_config_parse(&c, invalid_selection, strlen(invalid_selection));
    TEST_CHECK(c.japanese_only_selection == 1 && c.n_warnings == 1);
    /* The stable-text wait (on by default) and the blank-line check (off by
     * default), each in its range. */
    vjo_config_defaults(&c);
    TEST_CHECK(c.ocr_settle_ms == 150 && c.ocr_settle_max_ms == 2000 && c.ocr_skip_blank_lines == 0);
    const char *settle = "ocr_settle_ms = 0\nocr_settle_max_ms = 5000\nocr_skip_blank_lines = ON\n";
    vjo_config_parse(&c, settle, strlen(settle));
    TEST_CHECK(!c.ocr_settle_ms && c.ocr_settle_max_ms == 5000 && c.ocr_skip_blank_lines == 1 && c.n_warnings == 0);
    settle = "ocr_skip_blank_lines = off\n";
    vjo_config_parse(&c, settle, strlen(settle));
    TEST_CHECK(!c.ocr_skip_blank_lines && c.n_warnings == 0);
    vjo_config_defaults(&c);
    settle = "ocr_settle_ms = 1001\nocr_settle_max_ms = 99\nocr_skip_blank_lines = maybe\nocr_settle_ms = -5\n";
    vjo_config_parse(&c, settle, strlen(settle));
    TEST_CHECK(c.ocr_settle_ms == 150 && c.ocr_settle_max_ms == 2000 && c.ocr_skip_blank_lines == 0 &&
               c.n_warnings == 4);
    TEST_CHECK(strstr(c.warnings[0], "ocr_settle_ms") && strstr(c.warnings[1], "ocr_settle_max_ms"));
    vjo_config_defaults(&c);
    settle = "ocr_settle_ms = 1000\nocr_settle_max_ms = 10000\n";
    vjo_config_parse(&c, settle, strlen(settle));
    TEST_CHECK(c.ocr_settle_ms == 1000 && c.ocr_settle_max_ms == 10000 && c.n_warnings == 0);
    /* The press's old limit, ocr_settle_press_ms, is read by nothing since #413 (opening the overlay reads without the
     * stability wait), and still checked as before: 0 or 100-10,000 ms, else warned; it changes nothing (#418). */
    static const struct { const char *val; int warns; } press[] = {
        {"0", 0}, {"100", 0}, {"10000", 0}, {"1", 1}, {"24", 1}, {"99", 1}, {"10001", 1}, {"-5", 1}, {"soon", 1}};
    for (unsigned i = 0; i < sizeof(press) / sizeof(press[0]); i++) {
        char line[64];
        VjoConfig d;
        vjo_config_defaults(&c);
        vjo_config_defaults(&d);
        snprintf(line, sizeof(line), "ocr_settle_press_ms = %s\n", press[i].val);
        vjo_config_parse(&c, line, strlen(line));
        TEST_CHECK(c.n_warnings == press[i].warns);
        TEST_MSG("ocr_settle_press_ms = %s: %d warning(s)", press[i].val, c.n_warnings);
        if (press[i].warns) TEST_CHECK(strstr(c.warnings[0], "ocr_settle_press_ms") != NULL);
        c.n_warnings = 0;
        memset(c.warnings, 0, sizeof(c.warnings));
        TEST_CHECK(!memcmp(&c, &d, sizeof(c))); /* nothing else changed */
    }
    /* The left stick's lookup (#138, core/stick_cursor.h): once the cursor has rested
     * 200 ms by default; 0 (as soon as the stick stops) to 1,000 ms; else the default, warned. */
    vjo_config_defaults(&c);
    TEST_CHECK(c.stick_lookup_ms == 200 && c.stick_lookup_ms == VJO_STICK_CURSOR_REST_MS);
    static const struct { const char *val; int want, warns; } rest[] = {
        {"0", 0, 0}, {"150", 150, 0}, {"250", 250, 0}, {"1000", 1000, 0}, {"1001", 200, 1}, {"-5", 200, 1},
        {"soon", 200, 1}, {"", 200, 1}, {"20 ms", 200, 1}};
    for (unsigned i = 0; i < sizeof(rest) / sizeof(rest[0]); i++) {
        char line[64];
        vjo_config_defaults(&c);
        snprintf(line, sizeof(line), "Stick_Lookup_Ms = %s\n", rest[i].val);
        vjo_config_parse(&c, line, strlen(line));
        TEST_CHECK(c.stick_lookup_ms == rest[i].want && c.n_warnings == rest[i].warns);
        TEST_MSG("stick_lookup_ms = %s: %d, %d warning(s)", rest[i].val, c.stick_lookup_ms, c.n_warnings);
        if (rest[i].warns) TEST_CHECK(strstr(c.warnings[0], "Stick_Lookup_Ms") != NULL); /* named as written */
    }
    /* The beam + LM decoder's largest LM (1000k by default, tiers 1-4, off) and the
     * confidence cascade's time ceiling (2 s by default, 0 = off, at most 3,750 ms). */
    vjo_config_defaults(&c);
    TEST_CHECK(c.ocr_lm == 2 && c.ocr_cascade_ms == 2000 && c.ocr_refine_lines == 2);
    const char *refine = "ocr_refine_lines = ON\n";
    vjo_config_parse(&c, refine, strlen(refine));
    TEST_CHECK(c.ocr_refine_lines == 1 && c.n_warnings == 0);
    refine = "ocr_refine_lines = off\nocr_refine_lines = sometimes\n";
    vjo_config_parse(&c, refine, strlen(refine));
    TEST_CHECK(c.ocr_refine_lines == 0 && c.n_warnings == 1);
    /* API 26: the text-presence gate, off by default (docs/vita-vn-ocr.md "Text-presence gate": API 43's measurement
     * on the hooked pages kept it so) */
    vjo_config_defaults(&c);
    TEST_CHECK(c.ocr_text_gate == 0 && VJO_TEXT_GATE_DEFAULT == 0);
    const char *gate = "ocr_text_gate = ON\n";
    vjo_config_parse(&c, gate, strlen(gate));
    TEST_CHECK(c.ocr_text_gate == 1 && c.n_warnings == 0);
    gate = "ocr_text_gate = off\nocr_text_gate = maybe\n";
    vjo_config_parse(&c, gate, strlen(gate));
    TEST_CHECK(c.ocr_text_gate == 0 && c.n_warnings == 1);
    /* API 55: the pause for the frame (docs/game-pause.md), off by default; one row of the off/on switches' table
     * (core/config.c switches[]), its own setting, in any case; an unknown value keeps the value, warned */
    vjo_config_defaults(&c);
    TEST_CHECK(c.ocr_pause == 0);
    const char *pause = "OCR_Pause = On\n";
    vjo_config_parse(&c, pause, strlen(pause));
    TEST_CHECK(c.ocr_pause == 1 && c.ocr_text_gate == 0 && c.n_warnings == 0);
    pause = "ocr_text_gate = on\nocr_pause = sometimes\n";
    vjo_config_parse(&c, pause, strlen(pause));
    TEST_CHECK(c.ocr_pause == 1 && c.ocr_text_gate == 1 && c.n_warnings == 1);
    pause = "ocr_pause = off\nocr_text_gate = off\n";
    vjo_config_parse(&c, pause, strlen(pause));
    TEST_CHECK(c.ocr_pause == 0 && c.ocr_text_gate == 0);
    pause = "ocr_pauses = on\n"; /* not the key */
    vjo_config_defaults(&c);
    vjo_config_parse(&c, pause, strlen(pause));
    TEST_CHECK(c.ocr_pause == 0);
    /* API 42: the head gate and the two-level head, ocr_head_gate's VJO_HEAD_* bits: on (both) by default
     * (docs/vita-vn-ocr.md "Head gate", "Two-level head"), gate or two_level alone, off neither, in any case; an
     * unknown value keeps the value, warned */
    vjo_config_defaults(&c);
    TEST_CHECK(c.ocr_head_gate == (VJO_HEAD_GATE | VJO_HEAD_TWO_LEVEL) && VJO_HEAD_GATE_DEFAULT == 3);
    {
        static const struct { const char *text; int bits; } heads[] = {
            {"ocr_head_gate = OFF\n", 0}, {"ocr_head_gate = gate\n", VJO_HEAD_GATE},
            {"ocr_head_gate = Two_Level\n", VJO_HEAD_TWO_LEVEL}, {"ocr_head_gate = on\n", VJO_HEAD_GATE | VJO_HEAD_TWO_LEVEL}};
        for (unsigned i = 0; i < sizeof(heads) / sizeof(heads[0]); i++) {
            vjo_config_defaults(&c);
            if (i == 3) c.ocr_head_gate = 0;
            vjo_config_parse(&c, heads[i].text, strlen(heads[i].text));
            TEST_CHECK_(c.ocr_head_gate == heads[i].bits && c.n_warnings == 0, "%s", heads[i].text);
        }
    }
    vjo_config_defaults(&c);
    const char *hg = "ocr_head_gate = gate\nocr_head_gate = sometimes\nocr_two_level = off\n";
    vjo_config_parse(&c, hg, strlen(hg));
    TEST_CHECK(c.ocr_head_gate == VJO_HEAD_GATE && c.n_warnings == 2); /* no ocr_two_level key: unknown, warned */
    TEST_CHECK(strstr(vjo_config_default_text(), "\nocr_head_gate = on\n") != NULL);
    /* API 50: the line finder's fixes, on by default (vita-vn-ocr #129's measured default); off; junk; all */
    vjo_config_defaults(&c);
    TEST_CHECK(c.ocr_line_fix == 1 && VJO_LINE_FIX_DEFAULT == 1);
    static const struct { const char *text; int want, warnings; } fixes[] = {
        {"ocr_line_fix = off\n", 0, 0}, {"ocr_line_fix = ON\n", 1, 0}, {"ocr_line_fix = junk\n", 2, 0},
        {"ocr_line_fix = all\n", 3, 0}, {"ocr_line_fix = all\nocr_line_fix = valley\n", 3, 1}};
    for (unsigned k = 0; k < sizeof(fixes) / sizeof(fixes[0]); k++) {
        vjo_config_parse(&c, fixes[k].text, strlen(fixes[k].text));
        TEST_CHECK(c.ocr_line_fix == fixes[k].want && c.n_warnings == fixes[k].warnings);
        TEST_MSG("%s: %d, %d warnings", fixes[k].text, c.ocr_line_fix, c.n_warnings);
    }
    /* API 54: the finder, the spotter by default (docs/vita-vn-ocr.md "The spotter finder"), and the spotter's
     * refinement, off by default. Any case; an unknown or empty value keeps the one before, warned with the key's name. */
    vjo_config_defaults(&c);
    TEST_CHECK(c.ocr_finder == VJO_FINDER_SPOTTER && VJO_FINDER_DEFAULT == VJO_FINDER_SPOTTER && !c.ocr_spotter_refine &&
               !VJO_SPOTTER_REFINE_DEFAULT);
    static const struct { const char *text, *key; int finder, refine, warnings; } finders[] = {
        {"ocr_finder = spotter\n", NULL, VJO_FINDER_SPOTTER, 0, 0},
        {"OCR_Finder = SPOTTER\nocr_spotter_refine = On\n", NULL, VJO_FINDER_SPOTTER, 1, 0},
        {"ocr_finder = spotter\nocr_finder = grad\n", NULL, VJO_FINDER_GRAD, 0, 0},
        {"ocr_finder = spotter ; vita-vn-ocr's text spotter\n", NULL, VJO_FINDER_SPOTTER, 0, 0},
        {"ocr_finder = spotter\nocr_finder = textdet\n", "ocr_finder", VJO_FINDER_SPOTTER, 0, 1},
        {"ocr_finder =\n", "ocr_finder", VJO_FINDER_SPOTTER, 0, 1},
        {"ocr_finder = grad\n", NULL, VJO_FINDER_GRAD, 0, 0},
        {"ocr_spotter_refine = on\nocr_spotter_refine = maybe\n", "ocr_spotter_refine", VJO_FINDER_SPOTTER, 1, 1},
        {"ocr_spotter_refine = on\nocr_spotter_refine = OFF\n", NULL, VJO_FINDER_SPOTTER, 0, 0}};
    for (unsigned i = 0; i < sizeof(finders) / sizeof(finders[0]); i++) {
        vjo_config_defaults(&c);
        vjo_config_parse(&c, finders[i].text, strlen(finders[i].text));
        TEST_CHECK_(c.ocr_finder == finders[i].finder && c.ocr_spotter_refine == finders[i].refine &&
                    c.n_warnings == finders[i].warnings && !c.ocr_text_gate, "%s", finders[i].text);
        if (finders[i].key) TEST_CHECK(strstr(c.warnings[0], finders[i].key) != NULL); /* named, never the value */
    }
    /* the template names both, with their defaults (docs/config.example.ini is the same text) */
    TEST_CHECK(strstr(vjo_config_default_text(), "\nocr_finder = " VJO_FINDER_DEFAULT_TEXT "\n") != NULL &&
               strstr(vjo_config_default_text(), "\nocr_spotter_refine = " VJO_SPOTTER_REFINE_DEFAULT_TEXT "\n") != NULL);
    /* API 31, audit-hooks F3: the hooks' table sites, on by default; off makes the Shell send
     * VJO_TEXT_SOURCE_NO_SITES (only the string copies hooked). Any case; an unknown value is on, warned. */
    vjo_config_defaults(&c);
    TEST_CHECK(c.hook_sites == 1);
    static const struct { const char *text; int sites, warnings; } sites[] = {
        {"hook_sites = off\n", 0, 0}, {"Hook_Sites = OFF\n", 0, 0}, {"hook_sites = off\nhook_sites = On\n", 1, 0},
        {"hook_sites = off ; the string copies alone\n", 0, 0}, {"hook_sites = off\nhook_sites = maybe\n", 1, 1},
        {"hook_sites = off\nhook_sites =\n", 1, 1}, {"hook_sites = 0\n", 1, 1}, {"hook_sites = no\n", 1, 1}};
    for (unsigned i = 0; i < sizeof(sites) / sizeof(sites[0]); i++) {
        vjo_config_defaults(&c);
        vjo_config_parse(&c, sites[i].text, strlen(sites[i].text));
        TEST_CHECK_(c.hook_sites == sites[i].sites && c.n_warnings == sites[i].warnings, "%s", sites[i].text);
        if (sites[i].warnings) TEST_CHECK(strstr(c.warnings[0], "hook_sites") != NULL); /* named, never the value */
    }
    /* API 43, hook_trust: on (1) by default, any case, an unknown or empty value on with a warning naming it;
     * it and hook_sites share their parse, each setting its own field */
    vjo_config_defaults(&c);
    TEST_CHECK(c.hook_trust == 1);
    static const struct { const char *text; int trust, sites, warnings; } trust[] = {
        {"hook_trust = off\n", 0, 1, 0}, {"Hook_Trust = OFF\n", 0, 1, 0}, {"HOOK_TRUST = off\nhook_sites = off\n", 0, 0, 0},
        {"hook_sites = off\nhook_trust = on\n", 1, 0, 0}, {"hook_trust = off\nhook_trust = maybe\n", 1, 1, 1},
        {"hook_trust = off\nhook_trust =\n", 1, 1, 1}, {"hook_trust = off ; 0.9.7's checks\n", 0, 1, 0},
        {"hook_trusted = off\n", 1, 1, 1}};
    for (unsigned i = 0; i < sizeof(trust) / sizeof(trust[0]); i++) {
        vjo_config_defaults(&c);
        vjo_config_parse(&c, trust[i].text, strlen(trust[i].text));
        TEST_CHECK_(c.hook_trust == trust[i].trust && c.hook_sites == trust[i].sites &&
                    c.n_warnings == trust[i].warnings, "%s", trust[i].text);
        if (trust[i].warnings) TEST_CHECK(strstr(c.warnings[0], "hook_trust") != NULL);
    }
    /* The reading statistics' time rule (docs/ocr-history.md#reading-time): adaptive (0) by default, 15s (1)
     * Rubi 0.9.7's. Any case; an unknown or empty value is adaptive, warned with the key's name. */
    vjo_config_defaults(&c);
    TEST_CHECK(c.reading_time == 0);
    static const struct { const char *text; int rule, warnings; } rules[] = {
        {"reading_time = 15s\n", 1, 0}, {"Reading_Time = 15S\n", 1, 0}, {"reading_time = adaptive\n", 0, 0},
        {"reading_time = ADAPTIVE\n", 0, 0}, {"reading_time = 15s ; Rubi 0.9.7's\n", 1, 0},
        {"reading_time = 15s\nreading_time = 30s\n", 0, 1}, {"reading_time = 15s\nreading_time =\n", 0, 1},
        {"reading_time = gsm\n", 0, 1}, {"reading_time = 15\n", 0, 1}};
    for (unsigned i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) {
        vjo_config_defaults(&c);
        vjo_config_parse(&c, rules[i].text, strlen(rules[i].text));
        TEST_CHECK_(c.reading_time == rules[i].rule && c.n_warnings == rules[i].warnings, "%s", rules[i].text);
        if (rules[i].warnings) TEST_CHECK(strstr(c.warnings[0], "reading_time") != NULL);
    }
    vjo_config_defaults(&c);
    const char *lm = "ocr_lm = 1000K\nocr_cascade_ms = 0\n";
    vjo_config_parse(&c, lm, strlen(lm));
    TEST_CHECK(c.ocr_lm == 2 && c.ocr_cascade_ms == 0 && c.n_warnings == 0);
    lm = "ocr_lm = off\nocr_cascade_ms = 3750\n";
    vjo_config_parse(&c, lm, strlen(lm));
    TEST_CHECK(c.ocr_lm == 0 && c.ocr_cascade_ms == 3750 && c.n_warnings == 0);
    vjo_config_defaults(&c);
    lm = "ocr_lm = 8000k\nocr_cascade_ms = 3751\nocr_cascade_ms = -1\n";
    vjo_config_parse(&c, lm, strlen(lm));
    TEST_CHECK(c.ocr_lm == 2 && c.ocr_cascade_ms == 2000 && c.n_warnings == 3);
    TEST_MSG("%u warnings", c.n_warnings);
    /* API 22: lines read at once (1-3, default 3) and the 444 MHz clock (default on) */
    vjo_config_defaults(&c);
    /* the defaults: ocr_threads = 3 (two helpers), ocr_boost_clock = on (the owner, 2026-10-15) */
    TEST_CHECK(c.ocr_helpers == 2 && c.ocr_boost_clock == 1);
    const char *speed = "ocr_threads = 1\nocr_boost_clock = OFF\n";
    vjo_config_parse(&c, speed, strlen(speed));
    TEST_CHECK(c.ocr_helpers == 0 && c.ocr_boost_clock == 0 && c.n_warnings == 0); /* an explicit off is kept */
    speed = "ocr_threads = 2\nocr_threads = 0\nocr_threads = 4\nocr_threads = x\nocr_boost_clock = on\n"
            "ocr_boost_clock = maybe\n";
    vjo_config_parse(&c, speed, strlen(speed));
    TEST_CHECK(c.ocr_helpers == 1 && c.ocr_boost_clock == 1 && c.n_warnings == 4); /* one warning each, kept 2 */
    TEST_MSG("helpers %d clock %d, %u warnings", c.ocr_helpers, c.ocr_boost_clock, c.n_warnings);
    /* vita-vn-ocr #99 (API 47): vertical text, VJO_VERTICAL_DEFAULT by default; off | on | auto in any case, stored
     * as the request's field (core/config.h VJO_VERTICAL_*); anything else keeps the value with a warning that
     * names the setting, not the value */
    vjo_config_defaults(&c);
    TEST_CHECK(c.ocr_vertical == VJO_VERTICAL_DEFAULT);
    TEST_CHECK(VJO_VERTICAL_OFF == 0 && VJO_VERTICAL_ON == (int)VJO_GAME_OCR_VERTICAL_ON &&
               VJO_VERTICAL_AUTO == (int)VJO_GAME_OCR_VERTICAL_AUTO);
    const char *vertical = "ocr_vertical = ON\n";
    vjo_config_parse(&c, vertical, strlen(vertical));
    TEST_CHECK(c.ocr_vertical == VJO_VERTICAL_ON && c.n_warnings == 0);
    vertical = "ocr_vertical = yes\nOCR_Vertical = 1\n";
    vjo_config_parse(&c, vertical, strlen(vertical));
    TEST_CHECK(c.ocr_vertical == VJO_VERTICAL_ON && c.n_warnings == 2 && strstr(c.warnings[0], "ocr_vertical: invalid value") &&
               !strstr(c.warnings[0], "yes") && strstr(c.warnings[1], "OCR_Vertical"));
    vertical = "ocr_vertical = off\n";
    vjo_config_parse(&c, vertical, strlen(vertical));
    TEST_CHECK(c.ocr_vertical == VJO_VERTICAL_OFF && c.n_warnings == 2);
    vertical = "ocr_vertical = Auto\n";
    vjo_config_parse(&c, vertical, strlen(vertical));
    TEST_CHECK(c.ocr_vertical == VJO_VERTICAL_AUTO && c.n_warnings == 2);
    vjo_config_defaults(&c);
    vertical = "ocr_vertical = on\nocr_boost_clock = off\n"; /* neighbours in the parser, not one field */
    vjo_config_parse(&c, vertical, strlen(vertical));
    TEST_CHECK(c.ocr_vertical == VJO_VERTICAL_ON && c.ocr_boost_clock == 0 && !c.n_warnings);
    /* #358, ocr_wait_hint: on (1) by default, on | off in any case; anything else keeps the value with a warning naming
     * the setting; it and ocr_boost_clock share their parse, each setting its own field */
    vjo_config_defaults(&c);
    TEST_CHECK(c.ocr_wait_hint == 1);
    static const struct { const char *text; int hint, clock, warnings; } hint[] = {
        {"ocr_wait_hint = off\n", 0, 1, 0}, {"OCR_Wait_Hint = OFF\n", 0, 1, 0},
        {"ocr_wait_hint = off\nocr_boost_clock = off\n", 0, 0, 0}, {"ocr_boost_clock = off\nocr_wait_hint = on\n", 1, 0, 0},
        {"ocr_wait_hint = off\nocr_wait_hint = 2\n", 0, 1, 1}, {"ocr_wait_hint = maybe\n", 1, 1, 1},
        {"ocr_wait_hint =\n", 1, 1, 1}, {"ocr_wait_hint = off ; Recognizing only\n", 0, 1, 0},
        {"ocr_wait_hints = off\n", 1, 1, 1}, {"ocr_wait = off\n", 1, 1, 1}};
    for (unsigned i = 0; i < sizeof(hint) / sizeof(hint[0]); i++) {
        vjo_config_defaults(&c);
        vjo_config_parse(&c, hint[i].text, strlen(hint[i].text));
        TEST_CHECK_(c.ocr_wait_hint == hint[i].hint && c.ocr_boost_clock == hint[i].clock &&
                    c.n_warnings == hint[i].warnings, "%s", hint[i].text);
        if (hint[i].warnings) /* named, never the value */
            TEST_CHECK(strstr(c.warnings[0], "ocr_wait") != NULL && !strstr(c.warnings[0], "maybe"));
    }

    /* the default file parses cleanly to the defaults */
    {
        VjoConfig d;
        const char *t = vjo_config_default_text();
        vjo_config_defaults(&d);
        vjo_config_parse(&d, t, strlen(t));
        TEST_CHECK(d.n_warnings == 0);
        TEST_CHECK(d.ocr_mode == VJO_OCR_AUTO &&
                   d.font_size_ja == 18 && d.font_size_en == 14 &&
                   d.toggle_button == VJO_TRIGGER_L_R && d.subtitle_button == VJO_TRIGGER_SELECT_R);
        TEST_CHECK(!d.anki_mining && strstr(t, "\nanki_mining = off\n") && !strstr(t, "anki_host"));
        /* no trace of the removed online dictionaries (jpdb.io, jiten.moe) or their keys, or of
         * the removed Hachidori relay (hachidori_host, its port) */
        TEST_CHECK(!strstr(t, "jpdb") && !strstr(t, "jiten") && !strstr(t, "api_key") && !strstr(t, "furigana"));
        TEST_CHECK(!strstr(t, "achidori") && !strstr(t, "relay") && !strstr(t, "19633") &&
                   strstr(t, "\ndictionary = local\n"));
        TEST_CHECK(d.ocr_settle_ms == 150 && d.ocr_settle_max_ms == 2000 && d.ocr_skip_blank_lines == 0);
        TEST_CHECK(d.ocr_refine_lines == 2 && strstr(t, "\nocr_refine_lines = psp\n"));
        TEST_CHECK(d.ocr_lm == 2 && d.ocr_cascade_ms == 2000 && strstr(t, "\nocr_lm = 1000k\n") &&
                   strstr(t, "\nocr_cascade_ms = 2000\n"));
        TEST_CHECK(d.stick_lookup_ms == 200 && strstr(t, "\nstick_lookup_ms = 200\n"));
        TEST_CHECK(d.hook_sites == 1 && strstr(t, "\nhook_sites = on\n")); /* audit-hooks F3 */
        TEST_CHECK(d.hook_trust == 1 && strstr(t, "\nhook_trust = on\n")); /* API 43 */
        TEST_CHECK(d.ocr_wait_hint == 1 && strstr(t, "\nocr_wait_hint = on\n")); /* #358 */
        TEST_CHECK(d.ocr_pause == 0 && strstr(t, "\nocr_pause = off\n")); /* API 55 */
        TEST_CHECK(d.reading_time == 0 && strstr(t, "\nreading_time = adaptive\n"));
        /* Japanese characters only: OFF in both (the owner, 2026-10-20; ON before) */
        TEST_CHECK(d.japanese_only_selection == 0 && strstr(t, "\njapanese_only_selection = off\n") &&
                   !strstr(t, "japanese_only_selection = on"));
        TEST_CHECK(d.ocr_vertical == VJO_VERTICAL_DEFAULT &&
                   strstr(t, "\nocr_vertical = " VJO_VERTICAL_DEFAULT_TEXT "\n"));
    }

    /* the same button for both: the toggle keeps it */
    {
        const char *same = "toggle_button = select+r\n";
        vjo_config_defaults(&c);
        vjo_config_parse(&c, same, strlen(same));
        TEST_CHECK(c.toggle_button == VJO_TRIGGER_SELECT_R && c.subtitle_button == VJO_TRIGGER_SELECT_L &&
                   c.n_warnings == 1);
        same = "toggle_button = start\nsubtitle_button = start\n";
        vjo_config_defaults(&c);
        vjo_config_parse(&c, same, strlen(same));
        TEST_CHECK(c.toggle_button == VJO_TRIGGER_START && c.subtitle_button == VJO_TRIGGER_SELECT_R &&
                   c.n_warnings == 1);
    }


    /* settings renamed/removed since earlier releases, as in tools/migrate_config.py */
    {
        const char *old = "api_key = k1\nfont_size = 20\nhw_jpeg = on\nfrequency_filter = 3\ncombo_delay_ms = 0\n";
        vjo_config_defaults(&c);
        vjo_config_parse(&c, old, strlen(old));
        TEST_CHECK(c.n_warnings == 0 && c.font_size_ja == 18);
    }

    /* docs/config.example.ini is the default file (the Rubi app's first save) */
    {
        size_t len;
        const char *t = vjo_config_default_text();
        char *doc;
        setup();
        doc = vjo_read_file(&A, VJO_FIXTURES "/../../docs/config.example.ini", &len);
        TEST_ASSERT(doc != NULL);
        TEST_CHECK(len == strlen(t) && !memcmp(doc, t, len));
        TEST_MSG("docs/config.example.ini differs from vjo_config_default_text()");
    }
}

static unsigned golden_parse_quiet; /* records whose parse is the defaults' with no warning */
static void golden_parse_record(const char *record, size_t n, void *arg)
{
    GoldenOut *o = (GoldenOut *)arg;
    VjoConfig d, c;
    size_t mark = o->n;
    vjo_config_defaults(&d);
    vjo_config_defaults(&c);
    vjo_config_parse(&c, record, n);
    golden_puts(o, "> ");
    golden_escape_short(o, record, n);
    golden_puts(o, " =>");
    if (golden_dump_config_line(o, &c, &d)) {
        golden_puts(o, "\n");
    } else { /* nothing: counted, not listed (a change to it adds its line) */
        o->n = mark;
        o->p[mark] = '\0';
        golden_parse_quiet++;
    }
}

/* #418 (cleanup package P2): every VjoConfig field and warning the parser makes of the fixtures, the template and
 * each record of host/tests/fixtures/config-lines.txt (alone, over the defaults), against
 * host/tests/fixtures/config-parse.golden: the defaults' fields once, then one line a record with the fields that
 * differ from them and its warnings (config_golden.h); a change to the parser shows as that file's diff. */
static void test_config_golden(void)
{
    static const struct { const char *path, *name; } files[] = {
        {"/../tests/fixtures/config-old-app-test2.ini", "host/tests/fixtures/config-old-app-test2.ini"},
        {"/../tests/fixtures/config-test3-owner.ini", "host/tests/fixtures/config-test3-owner.ini"},
        {"/../../docs/config.example.ini", "docs/config.example.ini"}};
    GoldenOut o = {0};
    VjoConfig c, d;
    char path[512], why[GOLDEN_WHY];
    golden_puts(&o, "# The parse of the config fixtures (host/tests/test_core.c config_golden; VJO_UPDATE_GOLDEN=1 "
                    "rewrites this file): the defaults, then each file and record with the fields that differ\n");
    vjo_config_defaults(&d);
    golden_puts(&o, "= the defaults\n");
    golden_dump_config(&o, &d, NULL);
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        size_t len;
        snprintf(path, sizeof(path), "%s%s", VJO_FIXTURES, files[i].path);
        char *text = golden_read(path, &len);
        TEST_ASSERT_(text != NULL, "%s", path);
        vjo_config_defaults(&c);
        vjo_config_parse(&c, text, len);
        golden_printf(&o, "= %s =>", files[i].name);
        golden_dump_config_line(&o, &c, &d);
        golden_puts(&o, "\n");
        free(text);
    }
    golden_parse_quiet = 0;
    snprintf(path, sizeof(path), "%s/../tests/fixtures/config-lines.txt", VJO_FIXTURES);
    TEST_CHECK(golden_records(path, golden_parse_record, &o) > 300);
    golden_long_records(golden_parse_record, &o);
    golden_printf(&o, "= %u records parse as the defaults, with no warning\n", golden_parse_quiet);
    snprintf(path, sizeof(path), "%s/../tests/fixtures/config-parse.golden", VJO_FIXTURES);
    int rc = golden_check(&o, path, why, sizeof(why));
    TEST_CHECK_(rc >= 0, "%s", why);
    free(o.p);
}

static void test_highlight_config(void)
{
    VjoConfig c;
    vjo_config_defaults(&c);
    TEST_CHECK(c.highlight_mode == VJO_HIGHLIGHT_WORD);
    /* Existing saved configs gain word highlighting without changing choices. */
    const char old[] = "ocr_backend = vocr\njapanese_only_selection = on\n"; /* ON: not the default */
    vjo_config_parse(&c, old, sizeof(old) - 1);
    TEST_CHECK(c.highlight_mode == VJO_HIGHLIGHT_WORD && c.n_warnings == 0);
    TEST_CHECK(c.japanese_only_selection == 1);
    const char character[] = "HIGHLIGHT_MODE = CHARACTER\n";
    vjo_config_parse(&c, character, sizeof(character) - 1);
    TEST_CHECK(c.highlight_mode == VJO_HIGHLIGHT_CHARACTER && c.n_warnings == 0);
    const char word[] = "highlight_mode = Word\n";
    vjo_config_parse(&c, word, sizeof(word) - 1);
    TEST_CHECK(c.highlight_mode == VJO_HIGHLIGHT_WORD && c.n_warnings == 0);
    const char invalid[] = "highlight_mode = sentence\n";
    vjo_config_parse(&c, invalid, sizeof(invalid) - 1);
    TEST_CHECK(c.highlight_mode == VJO_HIGHLIGHT_WORD && c.n_warnings == 1);
    TEST_CHECK(strstr(c.warnings[0], "highlight_mode") != NULL);
}

static void test_frequency_badge_config(void)
{
    VjoConfig c;
    vjo_config_defaults(&c);
    TEST_CHECK(c.frequency_badge == 1);
    /* A saved config without the key keeps the badge on. */
    const char old[] = "highlight_mode = word\n";
    vjo_config_parse(&c, old, sizeof(old) - 1);
    TEST_CHECK(c.frequency_badge == 1 && c.n_warnings == 0);
    const char off[] = "FREQUENCY_BADGE = OFF\n";
    vjo_config_parse(&c, off, sizeof(off) - 1);
    TEST_CHECK(c.frequency_badge == 0 && c.n_warnings == 0);
    const char on[] = "frequency_badge = on\n";
    vjo_config_parse(&c, on, sizeof(on) - 1);
    TEST_CHECK(c.frequency_badge == 1 && c.n_warnings == 0);
    const char invalid[] = "frequency_badge = maybe\n";
    vjo_config_parse(&c, off, sizeof(off) - 1);
    vjo_config_parse(&c, invalid, sizeof(invalid) - 1);
    TEST_CHECK(c.frequency_badge == 0 && c.n_warnings == 1); /* an invalid value keeps the setting */
    TEST_CHECK(strstr(c.warnings[0], "frequency_badge") != NULL);
    vjo_config_parse(&c, on, sizeof(on) - 1);
    vjo_config_parse(&c, invalid, sizeof(invalid) - 1);
    TEST_CHECK(c.frequency_badge == 1 && c.n_warnings == 2); /* either setting: on stays on */
    TEST_CHECK(strstr(c.warnings[1], "frequency_badge") != NULL);
    /* The written template carries it, on. */
    vjo_config_defaults(&c);
    c.frequency_badge = 0;
    vjo_config_parse(&c, vjo_config_default_text(), strlen(vjo_config_default_text()));
    TEST_CHECK(c.frequency_badge == 1 && c.n_warnings == 0);
    TEST_CHECK(strstr(vjo_config_default_text(), "\nfrequency_badge = on\n") != NULL);
}

static void test_regions(void)
{
#define P(s, t, g) vjo_region_parse(s, strlen(s), t, g)
    VjoRect r = {100, 200, 30000, 40000}, g;
    char line[64], *t;
    int n = vjo_region_format(line, sizeof(line), VJO_REGION_FALLBACK, &r);
    TEST_CHECK(n > 0 && !strcmp(line, "region = 100,200,30000,40000\n"));
    TEST_CHECK(vjo_region_parse(line, (size_t)n, NULL, &g) == VJO_REGION_ALL && !memcmp(&g, &r, sizeof(r)));
    TEST_CHECK(vjo_region_format(line, 10, VJO_REGION_FALLBACK, &r) == -1);
    TEST_CHECK(P(" region=1, 2 ,3,4\r\n", NULL, &g) == VJO_REGION_ALL && g.x == 1 && g.h == 4);
    TEST_CHECK(P("region = 1,2,0,3\n", NULL, &g) == 0);        /* zero size */
    TEST_CHECK(P("region = 60000,2,6000,3\n", NULL, &g) == 0); /* off screen */
    TEST_CHECK(P("region = 1,2,3\n", NULL, &g) == 0);
    TEST_CHECK(P("region = 1,2,3,4x\n", NULL, &g) == 0);
    TEST_CHECK(P("PCSG00123 = 1,2,3,4\n", NULL, &g) == 0);
    TEST_CHECK(P("", "PCSG00123", &g) == 0);

    /* per game: the game's line wins, wherever it is; else the fallback */
    {
        const char *ini = "region = 1,1,1,1\nPCSG00123 = 2,2,2,2\nPCSG0012 = 3,3,3,3\n";
        TEST_CHECK(P(ini, "PCSG00123", &g) == VJO_REGION_GAME && g.x == 2);
        TEST_CHECK(P(ini, "PCSG0012", &g) == VJO_REGION_GAME && g.x == 3);
        TEST_CHECK(P(ini, "PCSB00001", &g) == VJO_REGION_ALL && g.x == 1);
        TEST_CHECK(P("PCSG00123 = 2,2,0,2\nregion = 1,1,1,1\n", "PCSG00123", &g) == VJO_REGION_ALL);
        TEST_CHECK(P("region = 1,1,1,1\nPCSG00123 =  full \r\n", "PCSG00123", &g) == VJO_REGION_GAME && g.w == 0);
        TEST_CHECK(P("PCSG00123 = fullx\n", "PCSG00123", &g) == 0);
        TEST_CHECK(P("region = 1,1,1,1", "PCSG00123", &g) == VJO_REGION_ALL); /* no final newline */
    }

    /* update: replace, add, remove; other lines kept */
    setup();
    {
        VjoRect q = {5, 6, 7, 8};
        const char *ini = "region = 1,1,1,1\n; note\nPCSG00123 = 2,2,2,2";
        t = vjo_region_update(&A, ini, strlen(ini), "PCSG00123", &q);
        TEST_CHECK(t && !strcmp(t, "region = 1,1,1,1\n; note\nPCSG00123 = 5,6,7,8\n"));
        t = vjo_region_update(&A, ini, strlen(ini), "PCSB00001", &q);
        TEST_CHECK(t && !strcmp(t, "region = 1,1,1,1\n; note\nPCSG00123 = 2,2,2,2\nPCSB00001 = 5,6,7,8\n"));
        q.w = 0; /* full screen */
        t = vjo_region_update(&A, ini, strlen(ini), "PCSG00123", &q);
        TEST_CHECK(t && !strcmp(t, "region = 1,1,1,1\n; note\nPCSG00123 = full\n"));
        q.w = 7;
        t = vjo_region_update(&A, "", 0, "PCSG00123", &q);
        TEST_CHECK(t && !strcmp(t, "PCSG00123 = 5,6,7,8\n"));
    }

    /* #358, drop: the Rubi app's Suggest a text box ON takes the line for every game out (every one of them), the
     * games' own lines and the rest kept; a text without it unchanged but for its last newline and blank lines */
    {
        const char *ini = "region = full\n; note\nPCSG00123 = 2,2,2,2\n\n region =1,1,1,1\r\nregions = 1,1,1,1\n"
                          "PCSG00124 = full";
        t = vjo_region_drop(&A, ini, strlen(ini), VJO_REGION_FALLBACK);
        TEST_CHECK(t && !strcmp(t, "; note\nPCSG00123 = 2,2,2,2\nregions = 1,1,1,1\nPCSG00124 = full\n"));
        TEST_CHECK(t && P(t, "PCSB00001", &g) == 0 && P(t, "PCSG00124", &g) == VJO_REGION_GAME);
        t = vjo_region_drop(&A, "", 0, VJO_REGION_FALLBACK);
        TEST_CHECK(t && !strcmp(t, ""));
        t = vjo_region_drop(&A, "PCSG00123 = full", 16, VJO_REGION_FALLBACK);
        TEST_CHECK(t && !strcmp(t, "PCSG00123 = full\n"));
        /* OFF and ON again: the file as it was */
        VjoRect full = {0, 0, 0, 0};
        const char *games = "PCSG00123 = 2,2,2,2\n";
        t = vjo_region_update(&A, games, strlen(games), VJO_REGION_FALLBACK, &full);
        TEST_CHECK(t && !strcmp(t, "PCSG00123 = 2,2,2,2\nregion = full\n") && P(t, "PCSB00001", &g) == VJO_REGION_ALL &&
                   g.w == 0);
        t = vjo_region_drop(&A, t, strlen(t), VJO_REGION_FALLBACK);
        TEST_CHECK(t && !strcmp(t, games));
    }
#undef P
}

/* ---------- styled text (ui::Text) ---------- */

static void test_styled(void)
{
    VjoDictResult r;
    VjoEntryList l;
    VjoStyled st;
    uint32_t a, n;
    static const TestWord words[] = {{"猫", "ねこ", 1500, {"cat <pet>", "A&B"}}, {"好き", "すき", 900, {"liked"}}};
    static const TestToken tokens[] = {{0, 0, 1}, {1, 2, 2}};
    setup();
    test_result(&r, words, 2, tokens, 2);
    TEST_ASSERT(vjo_entries_build(&A, "猫が\r\n好き", &r, &l) == 0);

    /* header: one span, \r dropped; highlight ranges in UTF-16 units */
    TEST_ASSERT(vjo_styled_init(&st, &A, 64, 4) == 0);
    vjo_styled_header(&st, &l, 21);
    TEST_CHECK(st.len == 5 && st.text[0] == 0x732B && st.text[2] == '\n' && st.n_spans == 1);
    TEST_CHECK(vjo_entry_header_range(&l, 0, &a, &n) && a == 0 && n == 1);
    TEST_CHECK(vjo_entry_header_range(&l, 1, &a, &n) && a == 3 && n == 2);
    TEST_CHECK(!vjo_entry_header_range(&l, 2, &a, &n));

    /* body: headword+reading at ja px, badge+meanings at en px */
    TEST_ASSERT(vjo_styled_init(&st, &A, 256, 16) == 0);
    vjo_styled_entry(&st, &l, 0, 21, 18, 0, 1);
    TEST_CHECK(st.n_spans == 9); /* 猫 ( ねこ ) #rank \n meaning \n meaning */
    TEST_CHECK(st.spans[0].len == 1 && st.spans[0].px == 21 && st.spans[0].rgb == VJO_RGB_HIGHLIGHT);
    TEST_CHECK(st.spans[2].rgb == VJO_RGB_READING && st.spans[2].px == 21);
    TEST_CHECK(st.spans[4].px == 18 && st.spans[4].rgb == VJO_RGB_RANK && st.spans[4].len == 6); /* " #1500" */
    TEST_CHECK(st.text[st.spans[4].start + 1] == '#' && st.text[st.spans[4].start + 2] == '1');
    TEST_CHECK(st.spans[6].px == 18 && st.spans[6].len == 9);           /* "cat <pet>" unescaped */
    /* in Anki: a green √ after the badge */
    TEST_ASSERT(vjo_styled_init(&st, &A, 256, 16) == 0);
    vjo_styled_entry(&st, &l, 0, 21, 18, 1, 1);
    TEST_CHECK(st.n_spans == 10 && st.spans[5].rgb == VJO_RGB_OK && st.spans[5].len == 2 &&
               st.text[st.spans[5].start + 1] == 0x221A);
    /* frequency_badge = off: no badge, the meanings follow the reading */
    TEST_ASSERT(vjo_styled_init(&st, &A, 256, 16) == 0);
    vjo_styled_entry(&st, &l, 0, 21, 18, 0, 0);
    TEST_CHECK(st.n_spans == 8 && st.spans[4].rgb == VJO_RGB_DIM && st.spans[5].len == 9);
    for (int i = 0; i < st.n_spans; i++)
        TEST_CHECK(st.spans[i].rgb != VJO_RGB_RANK);
    /* an unranked word: " unranked" with a frequency list, nothing without one */
    r.vocab[0].rank = VJO_NO_RANK;
    TEST_ASSERT(vjo_styled_init(&st, &A, 256, 16) == 0);
    vjo_styled_entry(&st, &l, 0, 21, 18, 0, 1);
    TEST_CHECK(st.n_spans == 8);
    l.source = "Jiten";
    TEST_ASSERT(vjo_styled_init(&st, &A, 256, 16) == 0);
    vjo_styled_entry(&st, &l, 0, 21, 18, 0, 1);
    TEST_CHECK(st.n_spans == 9 && st.spans[4].rgb == VJO_RGB_RANK && st.spans[4].len == 9); /* " unranked" */
    TEST_CHECK(st.text[st.spans[4].start + 1] == 'u');
    l.source = NULL;
    r.vocab[0].rank = 1500;

    /* another dictionary's entry for the word (a character's name) below it: a blank line, its headword
     * and reading, its definition; no badge, no mark */
    {
        static const char *name_meanings[] = {"[VNDB] Furukawa Nagisa"};
        static VjoVocab name = {"渚", "なぎさ", 7, name_meanings, 1};
        static const VjoVocab *more[] = {&name};
        static char longdef[600];
        uint16_t want[] = {'\n', '\n', 0x6E1A, ' ', '('};
        uint32_t at;
        l.more = more;
        l.n_more = 1;
        TEST_ASSERT(vjo_styled_init(&st, &A, 256, 32) == 0);
        vjo_styled_entry(&st, &l, 0, 21, 18, 1, 1);
        for (at = 0; at + 5 <= st.len && memcmp(&st.text[at], want, sizeof(want)); at++) {}
        TEST_CHECK(at + 5 <= st.len);
        TEST_CHECK(st.text[st.len - 1] == 'a' && st.n_spans == 17); /* the entry's 10, then \n\n, 渚 ( なぎさ ) and \n + "Nagisa" */
        TEST_MSG("last %04X, %d spans", st.text[st.len - 1], st.n_spans);
        /* a definition longer than the body is clipped, so the name below still shows whole */
        memset(longdef, 'x', sizeof(longdef) - 1);
        const char **was = r.vocab[0].meanings, *clipped[] = {"cat <pet>", longdef};
        r.vocab[0].meanings = clipped;
        TEST_ASSERT(vjo_styled_init(&st, &A, 256, 32) == 0);
        vjo_styled_entry(&st, &l, 0, 21, 18, 0, 1);
        TEST_CHECK(!st.truncated && st.text[st.len - 1] == 'a');
        r.vocab[0].meanings = was;
        /* only the shown entry of a list stacks them */
        TEST_ASSERT(vjo_styled_init(&st, &A, 256, 32) == 0);
        vjo_styled_entry(&st, &l, 1, 21, 18, 0, 1);
        TEST_CHECK(st.text[st.len - 1] == 'd'); /* "liked" */
        l.more = NULL;
        l.n_more = 0;
    }

    /* surrogate pairs and truncation */
    TEST_ASSERT(vjo_styled_init(&st, &A, 3, 4) == 0);
    vjo_styled_puts(&st, "😀ab", 0, 10);
    TEST_CHECK(st.len == 3 && st.text[0] == 0xD83D && st.text[1] == 0xDE00 && st.truncated);
    TEST_CHECK(vjo_utf16_index("😀猫", 4) == 2 && vjo_utf16_index("a\r\nb", 3) == 2);
    TEST_CHECK(vjo_font_px(15) == 23 && vjo_font_px(8) == 12);
}

/* Line history's header (docs/line-history.md, the owner's example of 2026-10-20): the earlier line
 * ４７７８万 in white between the last row of the line before it (昭和５２) and the first row of the
 * line after it (昨日すき焼きを食べました), both dimmed; the text's units start after the line before. */
static void test_styled_history_header(void)
{
    VjoStyled st;
    uint32_t lead;
    setup();
    TEST_ASSERT(vjo_styled_init(&st, &A, 4096, VJO_HEADER_SPANS) == 0);
    lead = vjo_styled_history(&st, "昭和５２", "４７７８万", "昨日すき焼きを食べました", 27);
    TEST_CHECK(lead == 5 && st.n_spans == 5 && st.len == 5 + 5 + 1 + 12 && !st.truncated);
    TEST_CHECK(st.spans[0].start == 0 && st.spans[0].len == 4 && st.spans[0].rgb == VJO_RGB_RANK); /* 昭和５２ */
    TEST_CHECK(st.spans[1].len == 1 && st.text[4] == '\n' && st.spans[1].rgb == VJO_RGB_RANK);
    TEST_CHECK(st.spans[2].start == lead && st.spans[2].len == 5 && st.spans[2].rgb == VJO_RGB_TEXT); /* the focus */
    TEST_CHECK(st.text[lead] == 0xFF14 && st.text[lead + 4] == 0x4E07);                          /* ４ ... 万 */
    TEST_CHECK(st.spans[3].len == 1 && st.text[10] == '\n' && st.spans[4].len == 12 && st.spans[4].rgb == VJO_RGB_RANK);
    for (int i = 0; i < st.n_spans; i++)
        TEST_CHECK(st.spans[i].px == 27);
    /* the current text (no neighbours): the text alone, in one span, as before line history */
    TEST_ASSERT(vjo_styled_init(&st, &A, 4096, VJO_HEADER_SPANS) == 0);
    TEST_CHECK(vjo_styled_history(&st, NULL, "猫が\r\n好き", NULL, 21) == 0 && st.n_spans == 1 && st.len == 5);
    TEST_ASSERT(vjo_styled_init(&st, &A, 4096, VJO_HEADER_SPANS) == 0);
    TEST_CHECK(vjo_styled_history(&st, "", "猫", "", 21) == 0 && st.n_spans == 1 && st.len == 1);
    /* the oldest line kept: no line before it; the newest: the current text after it */
    TEST_ASSERT(vjo_styled_init(&st, &A, 4096, VJO_HEADER_SPANS) == 0);
    TEST_CHECK(vjo_styled_history(&st, NULL, "犬", "猫", 21) == 0 && st.n_spans == 3 && st.text[1] == '\n');
    /* a textbox's rows: of the line before its last row, of the line after its first */
    TEST_ASSERT(vjo_styled_init(&st, &A, 4096, VJO_HEADER_SPANS) == 0);
    lead = vjo_styled_history(&st, "一行目\n二行目", "今の\n行", "三行目\n四行目", 21);
    TEST_CHECK(lead == 4 && st.text[0] == 0x4E8C && st.len == 4 + 4 + 4 && st.text[lead + 2] == '\n'); /* 二行目 */
    TEST_CHECK(st.text[lead + 5] == 0x4E09 && st.spans[4].len == 3);                                  /* 三行目 */
    /* a row longer than any textbox's: the line before is cut at 128 units (whole characters), the text
     * keeps its own units; a surrogate pair is never split */
    {
        static char row[3 * 300 + 1], text[4 * 1000 + 1];
        for (int i = 0; i < 300; i++)
            memcpy(row + 3 * i, "あ", 3);
        for (int i = 0; i < 1000; i++)
            memcpy(text + 4 * i, "😀", 4);
        TEST_ASSERT(vjo_styled_init(&st, &A, 4096, VJO_HEADER_SPANS) == 0);
        lead = vjo_styled_history(&st, row, text, row, 21);
        TEST_CHECK(lead == 129 && st.spans[0].len == 128 && st.cap == 4096);
        TEST_CHECK(st.spans[2].len == 2000 && st.text[lead + 1999] == 0xDE00);   /* the whole text */
        TEST_CHECK(st.spans[4].len == 300 && st.len == 129 + 2000 + 1 + 300);    /* the line after, its whole row */
        /* a line before of surrogate pairs: cut at 128 units, the pair whole */
        TEST_ASSERT(vjo_styled_init(&st, &A, 4096, VJO_HEADER_SPANS) == 0);
        lead = vjo_styled_history(&st, text, "猫", NULL, 21);
        TEST_CHECK(lead == 129 && st.spans[0].len == 128 && st.text[127] == 0xDE00);
    }
}

/* Bee's Ultimate Dictionary's kanji lines: the trailing lines "漢=meaning",
 * a kanji (by whole code point), '=' and the meanings. */
static void test_kanji_block(void)
{
    static const struct { const char *text; int block; } cases[] = {
        {"a tree\n木=tree, wood", 7},
        {"Japanese\n日=day\n本=book\n語=word", 9},
        {"木=tree, wood", 0},                     /* no definition: all lines */
        {"Topic particle", -1},                   /* a kana word's: none */
        {"木=tree\na tree", -1},                  /* not at the end */
        {"a tree\n木=tree\n", -1},                /* a last empty line */
        {"a tree\n木=", -1},                      /* no meaning */
        {"a tree\n木:tree", -1},                  /* no '=' */
        {"under a tree\nの=of\n下=below", 20},    /* の is not a kanji: 下 only */
        {"a restaurant chain\n𠮷=lucky\n野=field\n家=house", 19}, /* beyond the BMP */
        {"famous\n\xEF\xA9\x9F=author\n名=name", 7}, /* U+FA5F, a compatibility ideograph */
        {"a word\n\xE3\x90\x80=x\n名=name", 7},   /* U+3400, CJK Extension A */
        {"repeat\n々=again\n名=name", 17},        /* 々 is no ideograph: 名 only */
        {"kana\nア=a", -1},                        /* nor kana */
        {"tea\n\xE8=x\n茶=tea", 8},               /* a malformed byte is no kanji: 茶 only */
        {"", -1},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t want = cases[i].block < 0 ? strlen(cases[i].text) : (size_t)cases[i].block;
        TEST_CHECK_(vjo_kanji_block(cases[i].text) == want, "case %zu: %zu, expected %zu", i,
                    vjo_kanji_block(cases[i].text), want);
    }
}

/* The UTF-16 text of st from unit `from`, as UTF-8 (BMP only), for comparisons. */
static const char *styled_utf8(const VjoStyled *st, uint32_t from, char *out, size_t cap)
{
    size_t n = 0;
    for (uint32_t i = from; i < st->len && n + 4 < cap; i++)
        n += (size_t)vjo_utf8_encode(st->text[i], out + n);
    out[n] = 0;
    return out;
}

/* The popup's kanji lines and its fitting (core/styled.c entry_text). */
static void test_styled_kanji_lines_and_fitting(void)
{
    VjoDictResult r;
    VjoEntryList l;
    VjoStyled st;
    char buf[4096];
    static char long_def[2048], summary[200], text[4096];
    static const char *nihongo[] = {"Japanese\n日=day\n本=book\n語=word", NULL};
    static const TestWord words[] = {{"日本語", "にほんご", 4372, {NULL}}};
    static const TestToken tokens[] = {{0, 0, 3}};
    TestWord w = words[0];
    setup();
    w.meanings[0] = nihongo[0];
    test_result(&r, &w, 1, tokens, 1);
    TEST_ASSERT(vjo_entries_build(&A, "日本語", &r, &l) == 0);

    /* a blank line after the definition, then the lines as written, each kanji
     * at ja px in the text colour */
    TEST_ASSERT(vjo_styled_init(&st, &A, 256, 32) == 0);
    vjo_styled_entry(&st, &l, 0, 27, 21, 0, 1);
    TEST_CHECK(!strcmp(styled_utf8(&st, 0, buf, sizeof(buf)),
                       "日本語 (にほんご) #4372\nJapanese\n\n日=day\n本=book\n語=word"));
    /* head 5; the newline, the definition and the blank line before the lines; 2 a line */
    TEST_CHECK_(st.n_spans == 14, "%d spans", st.n_spans);
    for (int k = 0; k < 3; k++) {
        const VjoSpan *kanji = &st.spans[8 + 2 * k], *meaning = &st.spans[9 + 2 * k];
        TEST_CHECK(kanji->len == 1 && kanji->px == 27 && kanji->rgb == VJO_RGB_TEXT);
        TEST_CHECK(st.text[kanji->start] == (k == 0 ? 0x65E5 : k == 1 ? 0x672C : 0x8A9E));
        TEST_CHECK(meaning->px == 21 && meaning->rgb == VJO_RGB_DIM && st.text[meaning->start] == '=');
        /* each line's newline ends its meanings' span; the definition's ends its own */
        TEST_CHECK(k == 2 || st.text[meaning->start + meaning->len - 1] == '\n');
    }
    TEST_CHECK(st.spans[7].px == 21 && st.spans[7].len == 2 && st.text[st.spans[7].start] == '\n' &&
               st.text[st.spans[7].start + 1] == '\n');

    /* too few spans left: the text stays whole (its last spans unstyled), marked truncated */
    TEST_ASSERT(vjo_styled_init(&st, &A, 256, 10) == 0);
    vjo_styled_entry(&st, &l, 0, 27, 21, 0, 1);
    TEST_CHECK(!strcmp(styled_utf8(&st, 0, buf, sizeof(buf)),
                       "日本語 (にほんご) #4372\nJapanese\n\n日=day\n本=book\n語=word"));
    TEST_CHECK(st.n_spans == 10 && st.truncated);
    /* kanji lines without a definition */
    {
        static const char *only[] = {"日=day\n本=book", NULL};
        TestWord ow = words[0];
        ow.meanings[0] = only[0];
        test_result(&r, &ow, 1, tokens, 1);
        TEST_ASSERT(vjo_entries_build(&A, "日本語", &r, &l) == 0);
        TEST_ASSERT(vjo_styled_init(&st, &A, 256, 32) == 0);
        vjo_styled_entry(&st, &l, 0, 27, 21, 0, 0);
        TEST_CHECK(!strcmp(styled_utf8(&st, 0, buf, sizeof(buf)), "日本語 (にほんご)\n日=day\n本=book"));
        TEST_CHECK(st.n_spans == 4 + 1 + 4 && st.spans[5].px == 27 && st.spans[7].px == 27);
    }

    /* An entry longer than the room, as the bundle orders an appended grammar
     * point: the summary, the article, a kanji line. The definition is clipped
     * at its end and marked; the summary (its start) and the kanji line stay
     * whole, and the room after the entry too. */
    memset(long_def, 'd', 1500); long_def[1500] = 0;
    memset(summary, 's', 150); summary[150] = 0;
    snprintf(text, sizeof(text), "%s\n%s\n日=day", summary, long_def);
    static const char *long_meanings[2];
    long_meanings[0] = text;
    static const TestWord long_word = {"日", "ひ", 0, {NULL}};
    TestWord lw = long_word;
    lw.meanings[0] = long_meanings[0];
    test_result(&r, &lw, 1, tokens, 1);
    TEST_ASSERT(vjo_entries_build(&A, "日", &r, &l) == 0);
    TEST_ASSERT(vjo_styled_init(&st, &A, 600, 32) == 0);
    st.cap -= 100; /* overlay.cpp: the room kept for what follows */
    vjo_styled_entry(&st, &l, 0, 27, 21, 0, 0);
    st.cap += 100;
    /* within the room (600 - 100), ASCII using it to the unit: bytes = units */
    TEST_CHECK_(!st.truncated && st.len == 500, "%u units", st.len);
    styled_utf8(&st, 0, buf, sizeof(buf));
    {
        /* the mark is the definition's: dim, at en px */
        int marks = 0;
        for (int i = 0; i < st.n_spans; i++)
            if (st.text[st.spans[i].start] == 0x2026) {
                marks++;
                TEST_CHECK(st.spans[i].len == 1 && st.spans[i].px == 21 && st.spans[i].rgb == VJO_RGB_DIM);
            }
        TEST_CHECK(marks == 1);
    }
    {
        char want_head[256];
        snprintf(want_head, sizeof(want_head), "日 (ひ)\n%s\nddd", summary);
        TEST_CHECK(!strncmp(buf, want_head, strlen(want_head)));
        TEST_CHECK(!strcmp(buf + strlen(buf) - strlen("d…\n\n日=day"), "d…\n\n日=day"));
    }
    /* a longer one: still the definition's start, then the mark */
    snprintf(text, sizeof(text), "first line\n%s%s\n日=day", long_def, summary);
    test_result(&r, &lw, 1, tokens, 1);
    TEST_ASSERT(vjo_entries_build(&A, "日", &r, &l) == 0);
    TEST_ASSERT(vjo_styled_init(&st, &A, 600, 32) == 0);
    vjo_styled_entry(&st, &l, 0, 27, 21, 0, 0);
    styled_utf8(&st, 0, buf, sizeof(buf));
    TEST_CHECK_(!st.truncated && st.len == 600, "%u units", st.len);
    TEST_CHECK(!strncmp(buf, "日 (ひ)\nfirst line\nddd", strlen("日 (ひ)\nfirst line\nddd")) &&
               !strcmp(buf + strlen(buf) - strlen("d…\n\n日=day"), "d…\n\n日=day"));
    /* an entry that fits is shown whole, unmarked: the cap is not reached */
    snprintf(text, sizeof(text), "short\n%s\n日=day", summary);
    test_result(&r, &lw, 1, tokens, 1);
    TEST_ASSERT(vjo_entries_build(&A, "日", &r, &l) == 0);
    TEST_ASSERT(vjo_styled_init(&st, &A, 600, 32) == 0);
    vjo_styled_entry(&st, &l, 0, 27, 21, 0, 0);
    styled_utf8(&st, 0, buf, sizeof(buf));
    TEST_CHECK(!st.truncated && !strstr(buf, "…") && strstr(buf, summary) && strstr(buf, "\n\n日=day"));
    /* a Japanese definition (3 bytes a character) is clipped at a character
     * boundary: no partial character, every unit kept one of its own */
    {
        static char kana[3 * 400 + 32];
        static const char *kana_meanings[1];
        TestWord kw = long_word;
        size_t o = 0;
        for (int i = 0; i < 400; i++) { memcpy(kana + o, "あ", 3); o += 3; }
        memcpy(kana + o, "\n日=day", sizeof("\n日=day"));
        kana_meanings[0] = kana;
        kw.meanings[0] = kana_meanings[0];
        for (uint32_t cap = 120; cap <= 130; cap++) {
            test_result(&r, &kw, 1, tokens, 1);
            TEST_ASSERT(vjo_entries_build(&A, "日", &r, &l) == 0);
            TEST_ASSERT(vjo_styled_init(&st, &A, cap, 32) == 0);
            vjo_styled_entry(&st, &l, 0, 27, 21, 0, 0);
            int bad = 0, kept = 0;
            for (uint32_t i = 0; i < st.len; i++) {
                bad += st.text[i] == 0xFFFD;
                kept += st.text[i] == 0x3042;
            }
            styled_utf8(&st, 0, buf, sizeof(buf));
            TEST_CHECK_(!bad && !st.truncated && kept > 0 &&
                            !strcmp(buf + strlen(buf) - strlen("あ…\n\n日=day"), "あ…\n\n日=day"),
                        "cap %u: %d replacement characters, %d kept: %s", cap, bad, kept, buf);
        }
    }
    /* a text with an empty line of its own before the kanji lines: still one blank line */
    snprintf(text, sizeof(text), "short\n\n日=day");
    test_result(&r, &lw, 1, tokens, 1);
    TEST_ASSERT(vjo_entries_build(&A, "日", &r, &l) == 0);
    TEST_ASSERT(vjo_styled_init(&st, &A, 600, 32) == 0);
    vjo_styled_entry(&st, &l, 0, 27, 21, 0, 0);
    TEST_CHECK(!strcmp(styled_utf8(&st, 0, buf, sizeof(buf)), "日 (ひ)\nshort\n\n日=day"));
    /* a text without kanji lines is shown as written, a last newline included */
    snprintf(text, sizeof(text), "a tree\n");
    test_result(&r, &lw, 1, tokens, 1);
    TEST_ASSERT(vjo_entries_build(&A, "日", &r, &l) == 0);
    TEST_ASSERT(vjo_styled_init(&st, &A, 600, 32) == 0);
    vjo_styled_entry(&st, &l, 0, 27, 21, 0, 0);
    TEST_CHECK(!strcmp(styled_utf8(&st, 0, buf, sizeof(buf)), "日 (ひ)\na tree\n"));
    /* kanji lines longer than the whole body: the definition gets no room (none
     * past the cap: the room is floored at 0), marked, then the lines to the cap */
    {
        size_t o = (size_t)snprintf(text, sizeof(text), "%.100s", long_def);
        for (int i = 0; i < 40; i++)
            o += (size_t)snprintf(text + o, sizeof(text) - o, "\n日=day");
        test_result(&r, &lw, 1, tokens, 1);
        TEST_ASSERT(vjo_entries_build(&A, "日", &r, &l) == 0);
        TEST_ASSERT(vjo_styled_init(&st, &A, 64, 96) == 0);
        vjo_styled_entry(&st, &l, 0, 27, 21, 0, 0);
        styled_utf8(&st, 0, buf, sizeof(buf));
        TEST_CHECK_(st.truncated && st.len <= 64 && !strncmp(buf, "日 (ひ)\n…\n\n日=day\n日=day", strlen("日 (ひ)\n…\n\n日=day\n日=day")),
                    "%u units: %s", st.len, buf);
    }
    /* a body too small even for the kanji lines: cut at the cap, never past it
     * (the list again from lw: kw's meanings went out of scope with its block) */
    test_result(&r, &lw, 1, tokens, 1);
    TEST_ASSERT(vjo_entries_build(&A, "日", &r, &l) == 0);
    TEST_ASSERT(vjo_styled_init(&st, &A, 12, 32) == 0);
    vjo_styled_entry(&st, &l, 0, 27, 21, 0, 0);
    TEST_CHECK(st.truncated && st.len <= 12);
}

/* kernel/triggers.c (pad masks as SCE_CTRL_*) */
#define B_SELECT 0x1u
#define B_L 0x100u
#define B_R 0x200u

static void test_trigger_edges(void)
{
    TrigEdge sel = {0, 0}, selr = {0, 0}, lr = {0, 0};
    const uint32_t s = B_SELECT, sr = B_SELECT | B_R;
    /* no overlap: fires on press, once */
    TEST_CHECK(trig_edge(&lr, B_L | B_R, sr, B_L) == 0);
    TEST_CHECK(trig_edge(&lr, B_L | B_R, sr, B_L | B_R) == 1);
    TEST_CHECK(trig_edge(&lr, B_L | B_R, sr, B_L | B_R) == 0);
    TEST_CHECK(trig_edge(&lr, B_L | B_R, sr, 0) == 0);
    /* select alone: the subset fires on release */
    TEST_CHECK(trig_edge(&sel, s, sr, s) == 0);
    TEST_CHECK(trig_edge(&sel, s, sr, 0) == 1);
    /* select, then R: only select+r fires (on press) */
    TEST_CHECK(trig_edge(&sel, s, sr, s) == 0 && trig_edge(&selr, sr, s, s) == 0);
    TEST_CHECK(trig_edge(&sel, s, sr, sr) == 0 && trig_edge(&selr, sr, s, sr) == 1);
    TEST_CHECK(trig_edge(&sel, s, sr, s) == 0 && trig_edge(&selr, sr, s, s) == 0);
    TEST_CHECK(trig_edge(&sel, s, sr, 0) == 0 && trig_edge(&selr, sr, s, 0) == 0);
    /* R, then select */
    TEST_CHECK(trig_edge(&sel, s, sr, B_R) == 0 && trig_edge(&selr, sr, s, B_R) == 0);
    TEST_CHECK(trig_edge(&sel, s, sr, sr) == 0 && trig_edge(&selr, sr, s, sr) == 1);
    TEST_CHECK(trig_edge(&sel, s, sr, 0) == 0 && trig_edge(&selr, sr, s, 0) == 0);
    /* the next select tap fires again */
    TEST_CHECK(trig_edge(&sel, s, sr, s) == 0 && trig_edge(&sel, s, sr, 0) == 1);
    /* no buttons (rear double tap) never fires */
    TEST_CHECK(trig_edge(&lr, 0, sr, 0xFFFFu) == 0);
}

static void test_trigger_filter(void)
{
    TrigConfig c = {{B_L | B_R, B_SELECT | B_R}, {0, 0}, 0};
    TrigHold h[TRIG_COUNT];
    int64_t t = 1000000;

    /* no delay: hidden only while the whole combo is held */
    TEST_CHECK(trig_filter(&c, NULL, B_L, t) == B_L);
    TEST_CHECK(trig_filter(&c, NULL, B_L | B_R | 0x4000u, t) == 0x4000u);
    c.single[TRIG_SUBTITLE] = 1;
    c.mask[TRIG_SUBTITLE] = B_SELECT;
    TEST_CHECK(trig_filter(&c, NULL, B_SELECT | 0x4000u, t) == 0x4000u);

    /* delay 50 ms: l+r pressed L first never shows L */
    c.mask[TRIG_SUBTITLE] = B_SELECT | B_R;
    c.single[TRIG_SUBTITLE] = 0;
    c.delay_us = 50000;
    memset(h, 0, sizeof(h));
    TEST_CHECK(trig_filter(&c, h, B_L, t) == 0);
    TEST_CHECK(trig_filter(&c, h, B_L, t + 16000) == 0);
    TEST_CHECK(trig_filter(&c, h, B_L | B_R, t + 32000) == 0);
    TEST_CHECK(trig_filter(&c, h, B_L, t + 200000) == 0); /* held over: still hidden */
    TEST_CHECK(trig_filter(&c, h, 0, t + 216000) == 0);
    TEST_CHECK(trig_filter(&c, h, 0, t + 232000) == 0);  /* no replay */

    /* L held alone: reaches the game after the delay */
    t += 1000000;
    TEST_CHECK(trig_filter(&c, h, B_L, t) == 0);
    TEST_CHECK(trig_filter(&c, h, B_L, t + 49000) == 0);
    TEST_CHECK(trig_filter(&c, h, B_L, t + 50000) == B_L);
    TEST_CHECK(trig_filter(&c, h, 0, t + 66000) == 0);

    /* a short L tap is replayed after its release, for at least two frames */
    t += 1000000;
    TEST_CHECK(trig_filter(&c, h, B_L, t) == 0);
    TEST_CHECK(trig_filter(&c, h, 0, t + 16000) == B_L);
    TEST_CHECK(trig_filter(&c, h, 0, t + 60000) == B_L);
    TEST_CHECK(trig_filter(&c, h, 0, t + 16000 + 50000) == 0);

    /* select+r (the other trigger) shares R: l+r does not replay it */
    t += 1000000;
    TEST_CHECK(trig_filter(&c, h, B_SELECT, t) == 0);
    TEST_CHECK(trig_filter(&c, h, B_SELECT | B_R, t + 16000) == 0);
    TEST_CHECK(trig_filter(&c, h, 0, t + 32000) == 0);
    TEST_CHECK(trig_filter(&c, h, 0, t + 48000) == 0);
    /* nor when R is let go first */
    t += 1000000;
    TEST_CHECK(trig_filter(&c, h, B_R, t) == 0);
    TEST_CHECK(trig_filter(&c, h, B_SELECT | B_R, t + 16000) == 0);
    TEST_CHECK(trig_filter(&c, h, B_SELECT, t + 32000) == 0);
    TEST_CHECK(trig_filter(&c, h, 0, t + 48000) == 0);
    TEST_CHECK(trig_filter(&c, h, 0, t + 64000) == 0);
}

/* ---- change detection (kernel/scene.c) ---- */

#define FRAME_MAX_W 960
#define FRAME_MAX_H 544
static uint32_t frame[FRAME_MAX_W * FRAME_MAX_H];
static uint32_t frame_w, frame_h;

#define DARK  0xFF202020u
#define WHITE 0xFFFFFFFFu
#define GLYPH 24
#define TICK  133000 /* a signature every 8 frames at 60 fps */
#define NO_INPUT 0   /* no player input (long before the tests' clock) */

static void frame_new(uint32_t w, uint32_t h)
{
    frame_w = w;
    frame_h = h;
    for (uint32_t i = 0; i < w * h; i++)
        frame[i] = DARK;
}

static void frame_rect(uint32_t x, uint32_t y, uint32_t rw, uint32_t rh, uint32_t color)
{
    for (uint32_t r = y; r < y + rh; r++)
        for (uint32_t c = x; c < x + rw; c++)
            frame[r * frame_w + c] = color;
}

/* A made-up glyph: about a third of its pixels inked, by seed. Glyphs of
 * different seeds have the same ink density, so a line swap keeps each
 * cell's brightness and only the pixels differ. */
static void frame_glyph(uint32_t x, uint32_t y, uint32_t seed)
{
    uint32_t v = seed * 2654435761u + 1;
    for (uint32_t r = 0; r < GLYPH; r++)
        for (uint32_t c = 0; c < GLYPH; c++) {
            v = v * 1103515245u + 12345u;
            frame[(y + r) * frame_w + x + c] = (v >> 16) % 3 == 0 ? WHITE : DARK;
        }
}

static void frame_text(uint32_t x, uint32_t y, uint32_t seed, int n)
{
    for (int i = 0; i < n; i++)
        frame_glyph(x + (uint32_t)i * GLYPH, y, seed + (uint32_t)i);
}

static SceneSig frame_sig(void)
{
    static SceneAcc acc;
    SceneSig sig;
    scene_acc_begin(&acc, &sig, frame_w, frame_h);
    for (uint32_t y = 0; y < frame_h; y++)
        scene_acc_row(&acc, y, &frame[y * frame_w]); /* unsampled rows are skipped */
    return sig;
}

/* Feeds sig until the screen settles; returns the ticks it took (0: never
 * within max). */
static int settle(SceneTracker *t, const SceneSig *sig, int64_t *now, int max)
{
    for (int i = 1; i <= max; i++)
        if (scene_update(t, sig, *now += TICK, NO_INPUT, NO_INPUT) == SCENE_SETTLED)
            return i;
    return 0;
}

static void test_scene_grid(void)
{
    SceneAcc acc;
    SceneSig sig;
    memset(&acc, 0, sizeof(acc));
    scene_acc_begin(&acc, &sig, 960, 544);
    TEST_CHECK(acc.cols == 16 && acc.ystep == 16 && acc.xstep == 4);
    scene_acc_begin(&acc, &sig, 900, 120); /* a text box: 32x4 */
    TEST_CHECK(acc.cols == 32 && acc.y0[4] == 120);
    scene_acc_begin(&acc, &sig, 32, 32);
    TEST_CHECK(acc.ystep == 1 && acc.xstep == 1);
    /* every grid row gets a sampled row */
    for (uint32_t h = 32; h <= 544; h += 7) {
        scene_acc_begin(&acc, &sig, 960, h);
        for (uint32_t r = 0; acc.y0[r] < h; r++)
            TEST_CHECK(acc.y0[r + 1] - acc.y0[r] >= acc.ystep);
    }
}

static void test_scene_changes(void)
{
    SceneTracker t;
    SceneSig a, b, sig;
    int64_t now = 1000000;
    uint32_t id;
    memset(&t, 0, sizeof(t));

    /* the first frame is a new screen; it settles once, after the window */
    frame_new(960, 544);
    frame_text(40, 420, 100, 12);
    a = frame_sig();
    TEST_CHECK(scene_update(&t, &a, now, NO_INPUT, NO_INPUT) == 0 && t.id == 1 && !t.stable);
    TEST_CHECK(settle(&t, &a, &now, 10) == 3);
    TEST_CHECK(t.stable && scene_unsettled_us(&t, now) == 0);
    TEST_CHECK(scene_update(&t, &a, now += TICK, NO_INPUT, NO_INPUT) == 0);

    /* another line of the same length and ink: a new screen, and a capture
     * of the previous line (taken just before) does not match it */
    frame_new(960, 544);
    frame_text(40, 420, 200, 12);
    b = frame_sig();
    TEST_CHECK(scene_update(&t, &b, now += TICK, NO_INPUT, NO_INPUT) == 0 && t.id == 2 && !t.stable);
    TEST_CHECK(scene_match(&t, &a) == 0 && scene_match(&t, &b) == 2);

    /* a two-glyph reply replaced by another one, a line per 0.4 s */
    frame_new(960, 544);
    frame_text(40, 420, 300, 2);
    sig = frame_sig();
    scene_update(&t, &sig, now += 400000, NO_INPUT, NO_INPUT);
    id = t.id;
    frame_new(960, 544);
    frame_text(40, 420, 400, 2);
    sig = frame_sig();
    TEST_CHECK(scene_update(&t, &sig, now += 400000, NO_INPUT, NO_INPUT) == 0 && t.id == id + 1);

    /* a different grid (resolution or region): a new screen */
    frame_new(480, 272);
    sig = frame_sig();
    TEST_CHECK(scene_match(&t, &sig) == 0);
    TEST_CHECK(scene_update(&t, &sig, now += TICK, NO_INPUT, NO_INPUT) == 0 && t.id == id + 2);

    /* a reset keeps the numbering: an old number never matches again */
    id = t.id;
    scene_reset(&t);
    TEST_CHECK(scene_match(&t, &sig) == 0);
    TEST_CHECK(scene_update(&t, &sig, now += TICK, NO_INPUT, NO_INPUT) == 0 && t.id == id + 1);
}

/* A text box (900x120: 32x4 cells of about 28x30 px). */
#define ARROW_X 846 /* inside one cell */
#define ARROW_Y 92

static SceneSig box_with_line(uint32_t seed, uint32_t arrow)
{
    frame_new(900, 120);
    frame_text(20, 10, seed, 20);
    if (arrow)
        frame_rect(ARROW_X, ARROW_Y, 20, 20, arrow);
    return frame_sig();
}

/* Feeds an icon cycling through n states (one per `every` signatures)
 * for `ticks` signatures; returns how often the screen settled. */
static int run_icon(SceneTracker *t, const SceneSig *states, int n, int every, int ticks, int64_t *now)
{
    int settled = 0;
    for (int i = 0; i < ticks; i++)
        settled += scene_update(t, &states[i / every % n], *now += TICK, NO_INPUT, NO_INPUT) == SCENE_SETTLED;
    return settled;
}

/* The "next" arrow blinks (about 0.5 s on, 0.5 s off). Its first
 * appearance is a new state, which settles like any change; once it goes
 * back to a recent state, settling waits until it has blinked for
 * SCENE_ANIM_SPAN_US and is masked, then the screen settles for good; a
 * capture with it on or off is the screen. */
static void test_scene_blinking(void)
{
    SceneTracker t;
    SceneSig st[2] = {box_with_line(100, WHITE), box_with_line(100, 0)};
    int64_t now = 1000000;
    uint32_t id;
    memset(&t, 0, sizeof(t));

    scene_update(&t, &st[1], now, NO_INPUT, NO_INPUT);
    TEST_CHECK(settle(&t, &st[1], &now, 10) == 3);
    TEST_CHECK(run_icon(&t, st, 2, 4, 24, &now) == 2); /* 3.2 s */
    id = t.id;
    TEST_CHECK(t.stable && t.masked == 1);
    TEST_CHECK(run_icon(&t, st, 2, 4, 40, &now) == 0 && t.id == id);
    TEST_CHECK(scene_match(&t, &st[0]) == id && scene_match(&t, &st[1]) == id);

    /* a slow blink (1 s on, 1 s off) is masked too */
    memset(&t, 0, sizeof(t));
    scene_update(&t, &st[1], now, NO_INPUT, NO_INPUT);
    TEST_CHECK(run_icon(&t, st, 2, 8, 40, &now) == 2 && t.masked == 1);
}

/* An icon with many states (bobbing, color cycle) changes every signature. */
static void test_scene_animated_icon(void)
{
    static const uint32_t colors[6] = {0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u,
                                       0xFF00FFFFu, 0xFFFF00FFu, 0xFFFFFF00u};
    SceneTracker t;
    SceneSig st[6];
    int64_t now = 1000000;
    uint32_t id;
    memset(&t, 0, sizeof(t));

    for (int k = 0; k < 6; k++)
        st[k] = box_with_line(100, colors[k]);
    TEST_CHECK(run_icon(&t, st, 6, 1, 24, &now) == 1); /* revisits from the 7th state */
    id = t.id;
    TEST_CHECK(run_icon(&t, st, 6, 1, 40, &now) == 0 && t.id == id && t.masked == 1);

    /* the next line under the still blinking icon: a change */
    {
        SceneSig next[6];
        for (int k = 0; k < 6; k++) {
            next[k] = box_with_line(200, colors[k]);
        }
        TEST_CHECK(scene_update(&t, &next[0], now += TICK, NO_INPUT, NO_INPUT) == 0 && t.id == id + 1);
    }
}

/* The icon stops in another state than the screen's first frame: once its
 * mask ends, that is a change (whatever the mask hid is seen). */
static void test_scene_mask_expires(void)
{
    SceneTracker t;
    SceneSig st[2] = {box_with_line(100, WHITE), box_with_line(100, 0)};
    int64_t now = 1000000;
    uint32_t id;
    int ticks = 0;
    memset(&t, 0, sizeof(t));

    scene_update(&t, &st[0], now, NO_INPUT, NO_INPUT);
    run_icon(&t, st, 2, 4, 30, &now); /* ends on st[1] */
    id = t.id;
    TEST_CHECK(t.masked == 1 && scene_match(&t, &st[1]) == id);
    while (t.id == id && ticks++ < 30)
        scene_update(&t, &st[1], now += TICK, NO_INPUT, NO_INPUT);
    TEST_CHECK(t.id == id + 1 && t.masked == 0);
    TEST_CHECK((int64_t)ticks * TICK <= SCENE_ANIM_GAP_US + 2 * TICK);
}

/* An animated background: too many cells to mask, so nothing is masked
 * (it would hide the text) and it never settles. */
static void test_scene_animated_background(void)
{
    SceneTracker t;
    SceneSig st[3];
    int64_t now = 1000000;
    memset(&t, 0, sizeof(t));

    for (int k = 0; k < 3; k++) {
        frame_new(960, 544);
        frame_rect(0, 0, 960, 200, 0xFF000000u + (uint32_t)k * 40);
        frame_text(40, 420, 100, 12);
        st[k] = frame_sig();
    }
    TEST_CHECK(run_icon(&t, st, 3, 1, 60, &now) == 0);
    TEST_CHECK(t.masked == 0 && scene_unsettled_us(&t, now) >= 59 * TICK);
}

/* Going back to the previous screen is a change, however small: a menu
 * cursor moved to item B and back to A must not keep B's number. It settles
 * once it is clear that this is no animation. */
static void test_scene_revert(void)
{
    SceneTracker t;
    SceneSig st[2];
    int64_t now = 1000000;
    uint32_t id;
    int ticks;
    memset(&t, 0, sizeof(t));

    for (uint32_t k = 0; k < 2; k++) {
        frame_new(960, 544);
        frame_text(40, 420, 100 + k * 100, 2);
        st[k] = frame_sig();
    }
    scene_update(&t, &st[0], now, NO_INPUT, NO_INPUT);
    TEST_CHECK(settle(&t, &st[0], &now, 10) == 3);
    scene_update(&t, &st[1], now += TICK, NO_INPUT, NO_INPUT);
    TEST_CHECK(settle(&t, &st[1], &now, 10) == 3);
    id = t.id;
    TEST_CHECK(scene_update(&t, &st[0], now += TICK, NO_INPUT, NO_INPUT) == 0 && t.id == id + 1);
    TEST_CHECK(scene_match(&t, &st[0]) == id + 1 && scene_match(&t, &st[1]) == 0);
    /* an icon-sized revert waits until it is clear it is no blink */
    ticks = settle(&t, &st[0], &now, 20);
    TEST_CHECK(ticks > 3 && (int64_t)ticks * TICK <= SCENE_ANIM_GAP_US + 2 * TICK);

    /* moved back and forth for seconds: that is masked like an icon, but
     * once it stops, the mask ends and that is a change */
    run_icon(&t, st, 2, 3, 30, &now); /* ends on st[1] */
    id = t.id;
    TEST_CHECK(t.masked == 2); /* the two glyphs' cells */
    ticks = 0;
    while (t.id == id && ticks++ < 30)
        scene_update(&t, &st[1], now += TICK, NO_INPUT, NO_INPUT);
    TEST_CHECK(t.id == id + 1 && scene_match(&t, &st[1]) == t.id && scene_match(&t, &st[0]) == 0);
}

/* A menu scrolled up and down for seconds, a press per item: changes the
 * player causes are never animation, so stopping on an item is a change at
 * once and a capture taken while scrolling does not match it. Item texts
 * of n glyphs (1 glyph: one cell, like an icon). */
static void menu_scroll(uint32_t glyphs)
{
    SceneTracker t;
    SceneSig item[4];
    int64_t now = 1000000;
    uint32_t id;
    memset(&t, 0, sizeof(t));

    for (uint32_t k = 0; k < 4; k++) {
        frame_new(960, 544);
        frame_text(40, 420, 100 + k * 50, (int)glyphs);
        item[k] = frame_sig();
    }
    scene_update(&t, &item[0], now, NO_INPUT, NO_INPUT);
    for (int step = 0; step < 12; step++) { /* 0.4 s per item, 4.8 s */
        int k = step % 6 < 3 ? step % 6 : 6 - step % 6; /* 0 1 2 3 2 1 0 ... */
        int64_t pressed = now + TICK / 2;
        for (int i = 0; i < 3; i++) {
            scene_update(&t, &item[k], now += TICK, pressed, pressed);
            TEST_CHECK(t.masked == 0);
        }
    }
    id = t.id; /* scrolling ended on item 1: stop on item 3 */
    TEST_CHECK(scene_match(&t, &item[1]) == id);
    now += TICK;
    TEST_CHECK(scene_update(&t, &item[3], now, now - TICK / 2, now - TICK / 2) == 0 && t.id == id + 1);
    TEST_CHECK(scene_match(&t, &item[3]) == t.id && scene_match(&t, &item[1]) == 0);

    /* wiggled 3, 2, 3, 2 right after: each step is a change */
    for (int i = 0; i < 4; i++) {
        id = t.id;
        now += 3 * TICK;
        TEST_CHECK(scene_update(&t, &item[i % 2 ? 3 : 2], now, now - TICK / 2, now - TICK / 2) == 0);
        TEST_CHECK(t.id == id + 1 && t.masked == 0);
    }
}

static void test_scene_menu_scroll(void)
{
    menu_scroll(12); /* descriptions */
    menu_scroll(1);  /* one-glyph names: 剣, 槍, 斧, 弓 */
}

/* The documented gap: a menu cycled with no input recorded (it advances by
 * itself, or reacts later than SCENE_INPUT_US) is masked like an icon, and
 * the stop is seen once the mask ends. */
static void test_scene_menu_without_input(void)
{
    SceneTracker t;
    SceneSig item[2];
    int64_t now = 1000000;
    uint32_t id;
    int ticks = 0;
    memset(&t, 0, sizeof(t));

    for (uint32_t k = 0; k < 2; k++) {
        frame_new(960, 544);
        frame_text(10, 420, 100 + k * 50, 1); /* inside one cell */
        item[k] = frame_sig();
    }
    scene_update(&t, &item[0], now, NO_INPUT, NO_INPUT);
    run_icon(&t, item, 2, 3, 30, &now); /* ends on item 1 */
    id = t.id;
    TEST_CHECK(t.masked == 1 && scene_match(&t, &item[0]) == id);
    while (t.id == id && ticks++ < 30)
        scene_update(&t, &item[1], now += TICK, NO_INPUT, NO_INPUT);
    TEST_CHECK(t.id == id + 1 && (int64_t)ticks * TICK <= SCENE_ANIM_GAP_US + 2 * TICK);
    TEST_CHECK(scene_match(&t, &item[0]) == 0);
}

/* A held button (no edge after the press) keeps a masked icon masked; a
 * press or release ends the masks (a change), and the icon is masked again
 * once it cycles. */
static void test_scene_input_and_masks(void)
{
    SceneTracker t;
    SceneSig st[2] = {box_with_line(100, WHITE), box_with_line(100, 0)};
    int64_t now = 1000000, edge;
    uint32_t id;
    int settled = 0;
    memset(&t, 0, sizeof(t));

    scene_update(&t, &st[1], now, NO_INPUT, NO_INPUT);
    run_icon(&t, st, 2, 4, 24, &now);
    id = t.id;
    TEST_CHECK(t.masked == 1 && t.stable);
    edge = now + TICK / 2; /* pressed, then held for 5 s */
    for (int i = 0; i < 40; i++) {
        now += TICK;
        settled += scene_update(&t, &st[i / 4 % 2], now, now - edge <= 1500000 ? now : edge + 1500000, edge) == SCENE_SETTLED;
        if (i == 0)
            TEST_CHECK(t.id == id + 1 && t.masked == 0); /* the press */
    }
    /* the blinks in the held-input window are the player's: a few settles
     * until it is masked again */
    id = t.id;
    TEST_CHECK(t.masked == 1 && t.stable && settled <= 5);
    settled = 0;
    for (int i = 0; i < 24; i++)
        settled += scene_update(&t, &st[i / 4 % 2], now += TICK, edge + 1500000, edge) == SCENE_SETTLED;
    TEST_CHECK(t.id == id && settled == 0);
}

/* A short wrapping menu scrolled by a held button for 5 s (auto-repeat
 * past the held-input window: masked like an icon), then tapped back: each
 * tap is seen, and so is the release without taps. */
static void test_scene_held_scroll(void)
{
    SceneTracker t;
    SceneSig item[4];
    int64_t now = 1000000, edge;
    int k = 0;
    memset(&t, 0, sizeof(t));

    for (uint32_t n = 0; n < 4; n++) {
        frame_new(960, 544);
        frame_text(10, 420, 100 + n * 50, 1);
        item[n] = frame_sig();
    }
    scene_update(&t, &item[0], now, NO_INPUT, NO_INPUT);
    edge = now + TICK / 2;
    for (int i = 0; i < 38; i++) { /* an item per 2 signatures */
        if (i % 2 == 0)
            k = (k + 1) % 4;
        now += TICK;
        scene_update(&t, &item[k], now, now - edge <= 1500000 ? now : edge + 1500000, edge);
    }
    TEST_CHECK(t.masked == 1); /* the documented held-scroll residual */
    for (int tap = 0; tap < 2; tap++) { /* released, then tapped back twice */
        uint32_t id = t.id;
        edge = now + TICK / 2;
        k = (k + 3) % 4;
        now += 2 * TICK;
        scene_update(&t, &item[k], now, edge, edge);
        TEST_CHECK(t.id > id && t.masked == 0);
        TEST_CHECK(scene_match(&t, &item[k]) == t.id && scene_match(&t, &item[(k + 1) % 4]) == 0);
    }
}

/* Two icons blinking at different rates (the "next" arrow and an AUTO
 * mark): both are masked and the screen settles for good. */
static void test_scene_two_icons(void)
{
    SceneTracker t;
    SceneSig st[4];
    int64_t now = 1000000;
    int settled = 0;
    memset(&t, 0, sizeof(t));

    for (int k = 0; k < 4; k++) {
        frame_new(900, 120);
        frame_text(20, 10, 100, 20);
        if (k & 1)
            frame_rect(ARROW_X, ARROW_Y, 20, 20, WHITE);
        if (k & 2)
            frame_rect(2, 92, 20, 20, WHITE); /* inside the first cell */
        st[k] = frame_sig();
    }
    for (int i = 0; i < 40; i++) /* the arrow every 4 signatures, AUTO every 6 */
        settled += scene_update(&t, &st[(i / 4 % 2) | (i / 6 % 2) << 1], now += TICK, NO_INPUT, NO_INPUT) == SCENE_SETTLED;
    TEST_CHECK(t.masked == 2 && t.stable && settled <= 3);
    settled = 0;
    for (int i = 40; i < 120; i++)
        settled += scene_update(&t, &st[(i / 4 % 2) | (i / 6 % 2) << 1], now += TICK, NO_INPUT, NO_INPUT) == SCENE_SETTLED;
    TEST_CHECK(settled == 0 && t.masked == 2);
}

/* A line replaced by a shorter one: its tail goes back to empty cells
 * (states seen before), which is no animation: it settles as usual. */
static void test_scene_shorter_line(void)
{
    SceneTracker t;
    SceneSig line_long, line_short;
    int64_t now = 1000000;
    memset(&t, 0, sizeof(t));

    frame_new(960, 544);
    line_long = frame_sig();
    scene_update(&t, &line_long, now, NO_INPUT, NO_INPUT);
    frame_text(40, 420, 100, 12);
    line_long = frame_sig();
    scene_update(&t, &line_long, now += TICK, NO_INPUT, NO_INPUT);
    TEST_CHECK(settle(&t, &line_long, &now, 10) == 3);
    frame_new(960, 544);
    frame_text(40, 420, 200, 3);
    line_short = frame_sig();
    scene_update(&t, &line_short, now += TICK, NO_INPUT, NO_INPUT);
    TEST_CHECK(settle(&t, &line_short, &now, 10) == 3);
}

/* The next line's arrow, soon after the last one's mask ended (it was
 * hidden while the line was typed): masked as soon as it cycles. */
static void test_scene_warm_mask(void)
{
    SceneTracker t;
    SceneSig st[2] = {box_with_line(100, WHITE), box_with_line(100, 0)};
    SceneSig next[2] = {box_with_line(200, WHITE), box_with_line(200, 0)};
    int64_t now = 1000000;
    int settled = 0, ticks = 0;
    memset(&t, 0, sizeof(t));

    scene_update(&t, &st[1], now, NO_INPUT, NO_INPUT);
    run_icon(&t, st, 2, 4, 24, &now);
    TEST_CHECK(t.masked == 1);
    while (t.masked && ticks++ < 30) /* the next line, typed for 2 s */
        scene_update(&t, &next[1], now += TICK, NO_INPUT, NO_INPUT);
    run_icon(&t, &next[1], 1, 1, 13, &now);
    settled = run_icon(&t, next, 2, 4, 14, &now); /* on, off, on, off */
    TEST_CHECK(t.masked == 1 && t.stable);
    TEST_CHECK(settled <= 1);
    TEST_CHECK(run_icon(&t, next, 2, 4, 16, &now) == 0);
}

/* Short lines (two glyphs) faded in and out over 4 signatures each, shown
 * 0.8 s: every line is a new screen that settles, and a capture of one
 * never matches another. (Once their cells are masked as cycling, a line
 * is seen when it has held SCENE_STABLE_US.) */
static void test_scene_fades(void)
{
    static const uint32_t levels[4] = {0xFF404040u, 0xFF808080u, 0xFFC0C0C0u, WHITE};
    SceneTracker t;
    SceneSig sig, prev;
    int64_t now = 1000000;
    memset(&t, 0, sizeof(t));

    frame_new(960, 544);
    sig = prev = frame_sig();
    scene_update(&t, &sig, now, NO_INPUT, NO_INPUT);
    for (uint32_t line = 0; line < 8; line++) {
        for (int step = 0; step < 4; step++) { /* fade in */
            frame_new(960, 544);
            frame_text(40, 420, 100 + line * 10, 2);
            for (uint32_t p = 0; p < 960 * 544; p++)
                if (frame[p] == WHITE)
                    frame[p] = levels[step];
            sig = frame_sig();
            scene_update(&t, &sig, now += TICK, NO_INPUT, NO_INPUT);
        }
        /* masked as cycling from the third line: held SCENE_STABLE_US, then settled */
        TEST_CHECK(settle(&t, &sig, &now, 8) == (line < 2 ? 3 : 6));
        TEST_CHECK(scene_match(&t, &sig) == t.id && scene_match(&t, &prev) == 0);
        prev = sig;
        for (int step = 3; step >= 0; step--) { /* fade out */
            frame_new(960, 544);
            frame_text(40, 420, 100 + line * 10, 2);
            for (uint32_t p = 0; p < 960 * 544; p++)
                if (frame[p] == WHITE)
                    frame[p] = step ? levels[step - 1] : DARK;
            sig = frame_sig();
            scene_update(&t, &sig, now += TICK, NO_INPUT, NO_INPUT);
        }
    }
}

/* Typewriter text: each glyph is a change, a pause shorter than the window
 * does not settle it, and it settles a window after the last glyph. */
static void test_scene_typewriter(void)
{
    SceneTracker t;
    SceneSig sig;
    int64_t now = 1000000, started;
    memset(&t, 0, sizeof(t));

    frame_new(900, 120);
    sig = frame_sig();
    scene_update(&t, &sig, now, NO_INPUT, NO_INPUT);
    TEST_CHECK(settle(&t, &sig, &now, 10) == 3);
    started = now + TICK;
    for (int i = 0; i < 16; i++) {
        uint32_t id = t.id;
        frame_glyph(20 + (uint32_t)i * GLYPH, 10, 100 + (uint32_t)i);
        sig = frame_sig();
        TEST_CHECK(scene_update(&t, &sig, now += TICK, NO_INPUT, NO_INPUT) == 0 && t.id == id + 1);
        if (i == 7) /* a pause after a comma: 2 ticks < SCENE_STABLE_US */
            TEST_CHECK(settle(&t, &sig, &now, 2) == 0);
    }
    TEST_CHECK(scene_unsettled_us(&t, now) == now - started);
    TEST_CHECK(settle(&t, &sig, &now, 10) == 3);
}

/* A VN text box (a PSP game's, 458x64) whose "next" mark spins after the
 * last glyph: 8 frames (widths of a turn, the back shaded), 4 game frames
 * each, sampled every 9, in other cells on every line. The first line
 * settles once the mark is masked (SCENE_ANIM_SPAN_US); later lines
 * sooner, a mask being recent; none changes again until the next press,
 * so a result taken when it settles stays current. */
static void spinning_mark(uint32_t x, uint32_t y, uint32_t f)
{
    static const uint32_t width[8] = {14, 12, 8, 5, 2, 5, 8, 12};
    uint32_t ph = f / 4 % 8, w = width[ph];
    frame_rect(x + (15 - w) / 2, y, w, 15, 0xFFC060FFu);
    if (ph > 4)
        frame_rect(x + (15 - w) / 2, y + 3, w / 2, 4, 0xFF9040C0u);
    else
        frame_rect(x + 7, y + 3, w / 2, 4, 0xFFE0A0FFu);
}

/* The mark read while the game draws the next frame: its rows from `split`
 * down are still the previous frame's. */
static void torn_mark(uint32_t x, uint32_t y, uint32_t f, uint32_t split)
{
    static uint32_t top[15][16];
    spinning_mark(x, y, f);
    for (uint32_t r = 0; r < split; r++)
        for (uint32_t c = 0; c < 16; c++)
            top[r][c] = frame[(y + r) * frame_w + x + c];
    frame_rect(x, y, 16, 15, DARK);
    spinning_mark(x, y, f - 4);
    for (uint32_t r = 0; r < split; r++)
        for (uint32_t c = 0; c < 16; c++)
            frame[(y + r) * frame_w + x + c] = top[r][c];
}

static void test_scene_spinning_mark(void)
{
    SceneTracker t;
    SceneSig sig;
    int64_t now = 100000000; /* not warm at the start */
    uint32_t f = 0;
    memset(&t, 0, sizeof(t));

    for (uint32_t line = 0; line < 8; line++) {
        int64_t press = now += TICK;
        uint32_t n = 6 + line * 5 % 12, settled = 0, id = 0;
        for (uint32_t i = 1; i <= n; i++, now += TICK) { /* typed, a glyph per signature */
            frame_new(458, 64);
            frame_text(4, 4 + line % 2 * 30, 100 * line, (int)i);
            sig = frame_sig();
            scene_update(&t, &sig, now, press, press);
        }
        for (uint32_t k = 1; k <= 45; k++, now += TICK, f += 9) { /* read for 6 s */
            frame_new(458, 64);
            frame_text(4, 4 + line % 2 * 30, 100 * line, (int)n);
            spinning_mark(6 + n * GLYPH, 8 + line % 2 * 30, f);
            sig = frame_sig();
            if (scene_update(&t, &sig, now, press, press) == SCENE_SETTLED) {
                TEST_CHECK(!settled);
                settled = k;
                id = t.id;
            }
        }
        TEST_CHECK(settled && settled * TICK < (line ? SCENE_ANIM_SPAN_US : SCENE_ANIM_SPAN_US + 1000000));
        TEST_CHECK(line || settled * TICK >= SCENE_ANIM_SPAN_US);
        TEST_MSG("line %u settled after %u signatures", line, settled);
        TEST_CHECK(t.id == id && t.stable && t.masked >= 1);
    }
}

/* The spinning mark on a game whose frame is read while it draws (the PSP
 * emulator): a third of the reads mix two frames, at any row, so the mark
 * keeps showing states its cells have not seen. Once masked, it stays
 * masked: the line settles once and keeps its scene. */
static void test_scene_torn_mark(void)
{
    SceneTracker t;
    SceneSig sig;
    int64_t now = 100000000;
    uint32_t f = 0, v = 1;
    memset(&t, 0, sizeof(t));

    for (uint32_t line = 0; line < 6; line++) {
        int64_t press = now += TICK;
        uint32_t n = 8 + line * 3, settles = 0, id = 0;
        for (uint32_t k = 1; k <= 60; k++, now += TICK, f += 8) { /* 8 s */
            frame_new(458, 64);
            frame_text(4, 4, 100 * line, (int)n);
            v = v * 1103515245u + 12345u;
            if (v >> 16 & 1 && (v >> 17) % 3 == 0)
                torn_mark(6 + n * GLYPH, 8, f, 1 + (v >> 20) % 14);
            else
                spinning_mark(6 + n * GLYPH, 8, f);
            sig = frame_sig();
            if (scene_update(&t, &sig, now, press, press) == SCENE_SETTLED) {
                settles++;
                id = t.id;
            }
        }
        TEST_CHECK(settles == 1 && t.id == id && t.masked >= 1);
        TEST_MSG("line %u: settled %u times, masked %d", line, settles, t.masked);
    }
}

/* The spinning mark, read whole or torn: the screen is quiet (worth
 * capturing) well before it settles, never while a line is typed. A
 * capture taken once quiet shows the screen from then on, through the
 * mask and the settle; one of the line still being typed never does. */
static void test_scene_quiet_mark(void)
{
    SceneTracker t;
    SceneSig sig, cap, part;
    int64_t now = 100000000, cap_us = 0, part_us = 0;
    uint32_t f = 0, v = 1;
    memset(&t, 0, sizeof(t));

    for (uint32_t line = 0; line < 8; line++) {
        int64_t press = now += TICK;
        uint32_t n = 6 + line * 5 % 12, quiet = 0, settled = 0, typing_quiet = 0, lost = 0;
        int torn = line >= 4;
        for (uint32_t i = 1; i <= n; i++, now += TICK) {
            frame_new(458, 64);
            frame_text(4, 4 + line % 2 * 30, 100 * line, (int)i);
            sig = frame_sig();
            typing_quiet += scene_update(&t, &sig, now, press, press) == SCENE_QUIET;
            if (i == 1 && line) /* the last line's capture */
                TEST_CHECK(scene_match_since(&t, &cap, cap_us) == 0);
            if (i == n - 1) {
                part = sig;
                part_us = now;
            }
        }
        for (uint32_t k = 1; k <= 45; k++, now += TICK, f += 9) {
            int r;
            frame_new(458, 64);
            frame_text(4, 4 + line % 2 * 30, 100 * line, (int)n);
            v = v * 1103515245u + 12345u;
            if (torn && v >> 16 & 1 && (v >> 17) % 3 == 0)
                torn_mark(6 + n * GLYPH, 8 + line % 2 * 30, f, 1 + (v >> 20) % 14);
            else
                spinning_mark(6 + n * GLYPH, 8 + line % 2 * 30, f);
            sig = frame_sig();
            r = scene_update(&t, &sig, now, press, press);
            if (r == 2 && !quiet) {
                quiet = k;
                cap = sig;
                cap_us = now;
            }
            if (r == 1 && !settled)
                settled = k;
            if (quiet && k > quiet)
                lost += scene_match_since(&t, &cap, cap_us) != t.id;
            TEST_CHECK(scene_match_since(&t, &part, part_us) == 0);
        }
        TEST_CHECK(!typing_quiet && quiet && settled && quiet < settled);
        TEST_MSG("line %u: quiet after %u signatures, settled after %u, quiet while typed %u", line, quiet,
                 settled, typing_quiet);
        TEST_CHECK(quiet <= 8); /* a torn read can delay it */
        TEST_CHECK(!lost);
        TEST_MSG("line %u: the capture did not show the screen %u times", line, lost);
    }
}

/* Typing never makes the screen quiet, however slow (a glyph every 3
 * signatures: within one spot for 400 ms, but only ever new states). */
static void test_scene_quiet_typing(void)
{
    SceneTracker t;
    SceneSig sig;
    int64_t now = 100000000;
    int quiet = 0;
    memset(&t, 0, sizeof(t));
    for (int i = 1; i <= 16 * 3; i++) {
        frame_new(458, 64);
        frame_text(4, 4, 7, (i + 2) / 3);
        sig = frame_sig();
        quiet += scene_update(&t, &sig, now += TICK, NO_INPUT, NO_INPUT) == SCENE_QUIET;
    }
    TEST_CHECK(!quiet);
}

/* A "next" mark blinking at a fixed spot, the line typed up to it (not
 * the player's doing after SCENE_INPUT_US): the glyphs next to the masked
 * mark are changes, the screen settles with the whole line only. */
static void test_scene_text_next_to_mark(void)
{
    SceneTracker t;
    SceneSig sig, settled_sig;
    int64_t now = 100000000, press;
    int settled_glyphs = 0;
    memset(&t, 0, sizeof(t));

    for (int k = 0; k < 45; k++, now += TICK) {
        frame_new(960, 544);
        frame_text(40, 420, 100, 30);
        if (k / 4 % 2)
            frame_rect(910, 490, 20, 20, WHITE);
        sig = frame_sig();
        scene_update(&t, &sig, now, NO_INPUT, NO_INPUT);
    }
    TEST_CHECK(t.masked >= 1);
    press = now;
    for (int i = 2; i <= 2 * 36 + 20; i++, now += TICK) { /* 36 glyphs, 2 signatures each */
        int g = i / 2 < 36 ? i / 2 : 36;
        frame_new(960, 544);
        frame_text(40, 470, 300, g);
        if (i / 4 % 2)
            frame_rect(910, 490, 20, 20, WHITE);
        sig = frame_sig();
        if (scene_update(&t, &sig, now, press, press) == SCENE_SETTLED) {
            settled_glyphs = g;
            settled_sig = sig;
        }
    }
    TEST_CHECK(settled_glyphs == 36);
    TEST_MSG("settled with %d glyphs", settled_glyphs);
    TEST_CHECK(scene_match(&t, &settled_sig) == t.id);
}

/* An icon two cells wide, blinking, then replaced by a glyph (no input):
 * the glyph is new content once it has held SCENE_STABLE_US. */
static void test_scene_content_in_icon(void)
{
    SceneTracker t;
    SceneSig sig;
    int64_t now = 100000000;
    uint32_t id;
    int ticks = 0;
    memset(&t, 0, sizeof(t));

    for (int k = 0; k < 40; k++, now += TICK) {
        frame_new(900, 120);
        frame_text(20, 10, 100, 20);
        if (k / 4 % 2)
            frame_rect(830, 92, 30, 20, WHITE);
        sig = frame_sig();
        scene_update(&t, &sig, now, NO_INPUT, NO_INPUT);
    }
    TEST_CHECK(t.masked >= 1 && t.stable);
    id = t.id;
    frame_new(900, 120);
    frame_text(20, 10, 100, 20);
    frame_glyph(830, 92, 999);
    sig = frame_sig();
    while (t.id == id && ticks++ < 20)
        scene_update(&t, &sig, now += TICK, NO_INPUT, NO_INPUT);
    TEST_CHECK(t.id != id && ticks * TICK <= SCENE_STABLE_US + 2 * TICK);
    TEST_MSG("seen after %d signatures", ticks);
    TEST_CHECK(settle(&t, &sig, &now, 10) && scene_match(&t, &sig) == t.id);
}

/* An icon of 12 frames in one cell, looping (a sparkle): masked once it
 * has looped, as an icon of a few frames; then the screen stays settled. */
/* API 41, a hooked line (scene_hook): the worker reports the next line as the game copies it, before the
 * box shows it. A new scene at once, which the box clearing and the line typing out keep, and which a
 * capture begun since is (one begun before is not, however alike it looks). Once the region settled, the
 * blinking "next" arrow keeps it; a menu over the box is another screen, as without a hook. */
static void test_scene_hooked_line(void)
{
    SceneTracker t;
    SceneSig line1 = box_with_line(100, 0), line2 = box_with_line(200, 0), arrow[2], blank, typing[4], menu;
    int64_t now = 1000000, hooked;
    uint32_t id;
    memset(&t, 0, sizeof(t));
    frame_new(900, 120);
    blank = frame_sig();
    for (int k = 0; k < 4; k++) {
        frame_new(900, 120);
        frame_text(20, 10, 200, 5 * (k + 1)); /* the same glyphs as line2, 5 more a step */
        typing[k] = frame_sig();
    }
    arrow[0] = box_with_line(200, WHITE);
    arrow[1] = line2;
    frame_new(900, 120);
    frame_rect(100, 20, 600, 80, WHITE);
    menu = frame_sig();

    scene_update(&t, &line1, now, NO_INPUT, NO_INPUT);
    TEST_CHECK(settle(&t, &line1, &now, 10) == 3);
    id = t.id;
    TEST_CHECK(!t.hook_us && !scene_hook_drawing(&t) && scene_capture(&t, &line1, now) == id);

    /* the hook reports line 2 while the box still shows line 1 */
    hooked = now += 1000;
    scene_hook(&t, hooked);
    TEST_CHECK(t.id == id + 1 && !t.stable && scene_hook_drawing(&t));
    id = t.id;
    TEST_CHECK(scene_capture(&t, &line1, hooked - 1) == 0);
    TEST_CHECK(scene_capture(&t, &line1, hooked) == id);
    /* the box clears and the line types out: the same scene, drawing */
    TEST_CHECK(scene_update(&t, &blank, now += TICK, NO_INPUT, NO_INPUT) == 0 && t.id == id);
    for (int k = 0; k < 4; k++)
        TEST_CHECK(scene_update(&t, &typing[k], now += TICK, NO_INPUT, NO_INPUT) == 0 && t.id == id &&
                   scene_hook_drawing(&t));
    TEST_CHECK(scene_unsettled_us(&t, now) > 0);
    /* it settles: drawn, still the same scene, and a capture begun since the line is still that scene's */
    TEST_CHECK(settle(&t, &line2, &now, 10) == 3 && t.id == id && t.stable && !scene_hook_drawing(&t));
    TEST_CHECK(scene_capture(&t, &typing[1], hooked + TICK) == id);
    /* the "next" arrow appears and blinks until it is masked: icon-sized, the same scene */
    run_icon(&t, arrow, 2, 4, 30, &now);
    TEST_CHECK(t.id == id && t.hook_us == hooked);
    /* a menu over the box: another screen, no longer the hooked line's */
    TEST_CHECK(scene_update(&t, &menu, now += TICK, NO_INPUT, NO_INPUT) == 0 && t.id == id + 1 && !t.hook_us);
    TEST_CHECK(scene_capture(&t, &line2, hooked) == 0 && scene_capture(&t, &menu, now) == id + 1);

    /* the next hooked line, then another before the first has drawn: each a new scene */
    TEST_CHECK(settle(&t, &menu, &now, 10) == 3);
    scene_hook(&t, now += 1000);
    id = t.id;
    TEST_CHECK(scene_update(&t, &blank, now += TICK, NO_INPUT, NO_INPUT) == 0 && t.id == id);
    hooked = now += 1000;
    scene_hook(&t, hooked);
    TEST_CHECK(t.id == id + 1 && scene_hook_drawing(&t));
    id = t.id;
    TEST_CHECK(scene_update(&t, &typing[0], now += TICK, NO_INPUT, NO_INPUT) == 0 && t.id == id);

    /* a region that never settles (new content behind the box at every signature): the scene holds for
     * SCENE_HOOK_DRAW_US, then such a change is another screen */
    for (int k = 0; scene_hook_drawing(&t) && k < 200; k++) {
        SceneSig sig;
        frame_new(900, 120);
        frame_text(20, 10, 300 + (uint32_t)k, 20);
        sig = frame_sig();
        TEST_CHECK(scene_update(&t, &sig, now += TICK, NO_INPUT, NO_INPUT) == 0);
        TEST_CHECK(t.id == (scene_hook_drawing(&t) ? id : id + 1));
    }
    TEST_CHECK(!t.hook_us && t.id == id + 1);
    TEST_CHECK(now - hooked >= SCENE_HOOK_DRAW_US && now - hooked < SCENE_HOOK_DRAW_US + TICK);

    /* a line reported but not drawn within SCENE_STABLE_US settles as drawn: its typing, once it
     * starts, is another screen (as without a hook) */
    TEST_CHECK(settle(&t, &blank, &now, 10) > 0);
    scene_hook(&t, now += 1000);
    id = t.id;
    TEST_CHECK(settle(&t, &blank, &now, 10) == 3 && t.id == id && !scene_hook_drawing(&t));
    TEST_CHECK(scene_update(&t, &typing[2], now += TICK, NO_INPUT, NO_INPUT) == 0 && t.id == id + 1);

    /* a reset (another region, the game in front again) forgets it */
    scene_hook(&t, now += 1000);
    TEST_CHECK(t.hook_us);
    scene_reset(&t);
    TEST_CHECK(!t.hook_us && !t.hook_drawn && !scene_hook_drawing(&t));
}

static void test_scene_many_frames(void)
{
    SceneTracker t;
    SceneSig sig;
    int64_t now = 100000000;
    uint32_t id = 0;
    int settled = 0;
    memset(&t, 0, sizeof(t));

    for (int k = 0; k < 60; k++, now += TICK) {
        frame_new(960, 544);
        frame_text(40, 420, 100, 12);
        frame_glyph(900, 480, 500 + (uint32_t)(k % 12));
        sig = frame_sig();
        if (scene_update(&t, &sig, now, NO_INPUT, NO_INPUT) == SCENE_SETTLED) {
            settled++;
            id = t.id;
        }
    }
    TEST_CHECK(settled == 1 && t.id == id && t.stable && t.masked >= 1);
    TEST_MSG("settled %d times", settled);
}

/* A config warning names a setting or a line's number, never a value (audit-pr180-b N3, the conductor 2026-10-17
 * 03:35Z): a warning is shown in the open overlay and written to log.txt, and a value can be a key or an address.
 * A BOM in the middle of a hand-joined file is no part of the next line's name: a removed API key behind one is
 * silent (it gave "ignored line '<BOM>jpdb_api_keySECRET…'"), and a setting behind one is read. */
static void test_config_warnings_name_no_value(void)
{
    /* cards of at most VJO_CONFIG_MAX_WARNINGS warnings each (a parse keeps that many) */
    static const struct { const char *ini, *want[VJO_CONFIG_MAX_WARNINGS]; } cards[] = {
        {"vocr_tier = normal\n"                                  /* 1 */
         "\xEF\xBB\xBF" "jpdb_api_keySECRETz9\n"                 /* 2: a removed key behind a BOM: silent */
         "bare_token_SECRETa\n"                                  /* 3: no '=' */
         "\xEF\xBB\xBF\xEF\xBB\xBF" "vocr_tier = large\n"         /* 4: behind two BOMs: read */
         "mystery key = SECRETb\n"                               /* 5: unknown, a space in its key */
         "ocr_lm = SECRETc\n"                                    /* 6 */
         "\xEF\xBB\xBF" "anki_audio_url https://example.invalid/?k=SECRETi\n" /* 7: a removed Anki line: silent */
         "font_size_ja = SECRETd\n",                             /* 8 */
         {"ignored line 3 (no '=')", "unknown setting 'mystery'", "ocr_lm: invalid value",
          "font_size_ja: invalid value (8-40)"}},
        {"local_dictionaries = SECRETe/../x.vjdict\n"
         "ocr_settle_ms = SECRETf\n"
         "vocr_model = SECRETg.vocr\n" /* a removed setting: silent (#418) */
         "abcdefghijklmnopqrstuvwxyz0123456789SECRETh = 1\n",
         {"local_dictionaries: invalid value (1-8 relative .vjdict filenames separated by commas)",
          "ocr_settle_ms: invalid value (ms, out of range)", "unknown setting 'abcdefghijklmnopqrstuvwxyz012345'"}},
        {"local_dictionary_dir = ux0:data/SECRETj\nlocal_dictionary_dir =\n", {"local_dictionary_dir: empty (ignored)"}},
        /* audit-pr185 B1: a value too long names its key cut as an unknown one is (the long values are added below) */
        {NULL, {"abcdefghijklmnopqrstuvwxyz012345: value too long", "mystery: value too long",
                "vocr_tier: value too long"}},
        /* audit-pr185 N1, O1 and O2: comments and a blank line count as lines; a space before a BOM, then a name */
        {"; c\n\n# c2\r\nbogus_SECRETo\n \xEF\xBB\xBF" "vocr_tier = large\n\t\xEF\xBB\xBF \xEF\xBB\xBF" "ocr_mode = on_press\n",
         {"ignored line 4 (no '=')"}},
    };
    /* card 3: values of 600 bytes and a line of 1,100, for the 'value too long' warnings */
    static char longs[4096];
    {
        int k = 0;
        k += snprintf(longs + k, sizeof(longs) - (size_t)k, "abcdefghijklmnopqrstuvwxyz0123456789SECRETh = ");
        memset(longs + k, 'S', 600); k += 600;
        k += snprintf(longs + k, sizeof(longs) - (size_t)k, "\nmystery key SECRETb = ");
        memset(longs + k, 'S', 1100); k += 1100; /* a line longer than the parser reads */
        k += snprintf(longs + k, sizeof(longs) - (size_t)k, "\nvocr_tier = ");
        memset(longs + k, 'S', 600); k += 600;
        longs[k++] = '\n';
        longs[k] = '\0';
    }
    for (unsigned k = 0; k < sizeof(cards) / sizeof(cards[0]); k++) {
        VjoConfig c;
        int want = 0;
        const char *ini = cards[k].ini ? cards[k].ini : longs;
        while (want < VJO_CONFIG_MAX_WARNINGS && cards[k].want[want]) want++;
        vjo_config_defaults(&c);
        vjo_config_parse(&c, ini, strlen(ini));
        TEST_CHECK_(c.n_warnings == want, "card %u: %d warnings, want %d", k, c.n_warnings, want);
        for (int i = 0; i < c.n_warnings; i++) {
            TEST_MSG("card %u, %d: %s", k, i, c.warnings[i]);
            TEST_CHECK_(!strstr(c.warnings[i], "SECRET") && !strstr(c.warnings[i], "\xEF\xBB\xBF") &&
                        !strstr(c.warnings[i], "example.invalid"), "card %u: warning %d quotes a value: %s", k, i,
                        c.warnings[i]);
            if (i < want)
                TEST_CHECK_(!strcmp(c.warnings[i], cards[k].want[i]), "card %u: [%s], want [%s]", k, c.warnings[i],
                            cards[k].want[i]);
        }
        if (k == 0)
            TEST_CHECK(c.vocr_tier == VJO_VOCR_TIER_LARGE); /* line 4, behind its BOMs */
        if (k == 2)
            TEST_CHECK(!strcmp(c.local_dictionary_dir, "ux0:data/SECRETj")); /* the empty line kept the earlier value */
        if (k == 4) /* behind a space and a BOM, and behind a tab, BOM, space, BOM: both read */
            TEST_CHECK(c.vocr_tier == VJO_VOCR_TIER_LARGE && c.ocr_mode == VJO_OCR_ON_PRESS);
    }
    /* where a line's name starts: spaces, tabs, CRs and BOMs in any order, never past the line's end */
    TEST_CHECK(vjo_config_line_start("", 0) == 0);
    TEST_CHECK(vjo_config_line_start("  ", 2) == 2);
    TEST_CHECK(vjo_config_line_start("\xEF\xBB\xBF", 2) == 0); /* a BOM cut by the line's end is no BOM */
    TEST_CHECK(vjo_config_line_start("\xEF\xBB", 2) == 0);
    TEST_CHECK(vjo_config_line_start("\xEF\xBB\xBF", 3) == 3);
    TEST_CHECK(vjo_config_line_start(" \xEF\xBB\xBF" "a", 5) == 4);
    TEST_CHECK(vjo_config_line_start("\xEF\xBB\xBF \t\r\xEF\xBB\xBF" "x", 10) == 9);
    TEST_CHECK(vjo_config_line_start("\xEF\xBB" "x", 3) == 0);
    /* every setting the template names, and a few it does not, with a value no setting accepts: its warning, if
     * any, never quotes it (one parse per key: a parse keeps four warnings) */
    static const char *const extra[] = {"vocr_model", "vocr_fallback_model", "ocr_backend", "dictionary", "toggle_button",
                                        "subtitle_button", "frequency_badge", "ocr_threads", "stick_lookup_ms", NULL};
    const char *tpl = vjo_config_default_text();
    int keys = 0, warned = 0;
    for (const char *l = tpl; *l;) {
        const char *e = strchr(l, '\n');
        size_t len = e ? (size_t)(e - l) : strlen(l);
        const char *eq = memchr(l, '=', len);
        char key[64];
        size_t kn = eq ? (size_t)(eq - l) : 0;
        while (kn && l[kn - 1] == ' ') kn--;
        if (*l != ';' && eq && kn && kn < sizeof(key)) {
            memcpy(key, l, kn);
            key[kn] = '\0';
            for (int pass = 0; pass < 2; pass++) {
                char ini[160];
                snprintf(ini, sizeof(ini), "%s = %s\n", key, pass ? "SECRETq q" : "SECRETq");
                VjoConfig c;
                vjo_config_defaults(&c);
                vjo_config_parse(&c, ini, strlen(ini));
                warned += c.n_warnings > 0;
                for (int i = 0; i < c.n_warnings; i++)
                    TEST_CHECK_(!strstr(c.warnings[i], "SECRET"), "%s: %s", key, c.warnings[i]);
            }
            keys++;
        }
        l += len + (e != NULL);
    }
    for (int x = 0; extra[x]; x++) {
        char ini[160];
        snprintf(ini, sizeof(ini), "%s = SECRETq\n", extra[x]);
        VjoConfig c;
        vjo_config_defaults(&c);
        vjo_config_parse(&c, ini, strlen(ini));
        warned += c.n_warnings > 0;
        for (int i = 0; i < c.n_warnings; i++)
            TEST_CHECK_(!strstr(c.warnings[i], "SECRET"), "%s: %s", extra[x], c.warnings[i]);
    }
    TEST_CHECK_(keys >= 20 && warned >= 20, "%d template keys, %d parses warned", keys, warned);
}

TEST_LIST = {
    {"buf_interleaved", test_buf_interleaved},
    {"config_warnings_name_no_value", test_config_warnings_name_no_value},
    {"filter_lines", test_filter_lines},
    {"utf", test_utf},
    {"utf_character_cursor", test_utf_character_cursor},
    {"japanese_character_cursor", test_japanese_character_cursor},
    {"json", test_json},
    {"json_object_lookups_stay_in_the_tokens", test_json_object_lookups_stay_in_the_tokens},
    {"furigana_align", test_furigana_align},
    {"furigana_rubies", test_furigana_rubies},
    {"entries", test_entries},
    {"entries_surrogates_and_trim", test_entries_surrogates_and_trim},
    {"foreground", test_foreground},
    {"trigger_edges", test_trigger_edges},
    {"trigger_filter", test_trigger_filter},
    {"scene_grid", test_scene_grid},
    {"scene_changes", test_scene_changes},
    {"scene_blinking", test_scene_blinking},
    {"scene_animated_icon", test_scene_animated_icon},
    {"scene_mask_expires", test_scene_mask_expires},
    {"scene_animated_background", test_scene_animated_background},
    {"scene_revert", test_scene_revert},
    {"scene_menu_scroll", test_scene_menu_scroll},
    {"scene_menu_without_input", test_scene_menu_without_input},
    {"scene_input_and_masks", test_scene_input_and_masks},
    {"scene_held_scroll", test_scene_held_scroll},
    {"scene_two_icons", test_scene_two_icons},
    {"scene_shorter_line", test_scene_shorter_line},
    {"scene_warm_mask", test_scene_warm_mask},
    {"scene_fades", test_scene_fades},
    {"scene_typewriter", test_scene_typewriter},
    {"scene_spinning_mark", test_scene_spinning_mark},
    {"scene_quiet_mark", test_scene_quiet_mark},
    {"scene_quiet_typing", test_scene_quiet_typing},
    {"scene_torn_mark", test_scene_torn_mark},
    {"scene_text_next_to_mark", test_scene_text_next_to_mark},
    {"scene_content_in_icon", test_scene_content_in_icon},
    {"scene_many_frames", test_scene_many_frames},
    {"scene_hooked_line", test_scene_hooked_line},
    {"config", test_config},
    {"config_golden", test_config_golden},
    {"highlight_config", test_highlight_config},
    {"frequency_badge_config", test_frequency_badge_config},
    {"regions", test_regions},
    {"styled", test_styled},
    {"styled_history_header", test_styled_history_header},
    {"kanji_block", test_kanji_block},
    {"styled_kanji_lines_and_fitting", test_styled_kanji_lines_and_fitting},
    {NULL, NULL},
};
