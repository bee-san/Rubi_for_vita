/* Anki: base64, note fields, AnkiConnect requests and replies (over a fake
 * network), word audio, the anki_* settings. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "acutest.h"
#include "anki.h"
#include "base64.h"
#include "config.h"
#include "replay.h"

static uint8_t g_mem[4u << 20];
static VjoArena A;

static void setup(void)
{
    /* Garbage-fill like a reused arena on the Vita. */
    memset(g_mem, 0xA5, sizeof(g_mem));
    vjo_arena_init(&A, g_mem, sizeof(g_mem));
}

static void test_base64(void)
{
    static const char *const vec[][2] = {{"", ""},        {"f", "Zg=="},        {"fo", "Zm8="},
                                         {"foo", "Zm9v"}, {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="},
                                         {"foobar", "Zm9vYmFy"}};
    uint8_t data[1000];
    char one[1400], chunked[1400];
    size_t o = 0;
    for (unsigned i = 0; i < sizeof(vec) / sizeof(vec[0]); i++) {
        char out[16] = {0};
        size_t n = strlen(vec[i][0]);
        TEST_CHECK(vjo_base64_len(n) == strlen(vec[i][1]));
        vjo_base64_encode((const uint8_t *)vec[i][0], n, out);
        TEST_CHECK(!strcmp(out, vec[i][1]));
        TEST_MSG("'%s' -> '%s'", vec[i][0], out);
    }
    /* chunks of a multiple of 3 bytes, then the rest: same as one shot */
    for (int i = 0; i < 1000; i++)
        data[i] = (uint8_t)(i * 37 + 11);
    vjo_base64_encode(data, sizeof(data), one);
    for (size_t off = 0; off < sizeof(data); off += 99) {
        size_t n = sizeof(data) - off < 99 ? sizeof(data) - off : 99;
        vjo_base64_encode(data + off, n, chunked + o);
        o += vjo_base64_len(n);
    }
    TEST_CHECK(o == vjo_base64_len(sizeof(data)) && !memcmp(one, chunked, o));
}

static void test_anki_fields(void)
{
    VjoBuf b;
    char *s;
#define BUF(expr) (setup(), vjo_buf_init(&b, &A), (expr), s = vjo_buf_cstr(&b))
    /* the word in bold, escaped, line breaks as <br>, CR dropped */
    BUF(vjo_anki_sentence(&b, "猫が\r\n好き<3&\"", 0, 3));
    TEST_CHECK(!strcmp(s, "<b>猫</b>が<br>好き&lt;3&amp;&quot;"));
    BUF(vjo_anki_sentence(&b, "猫が好き", 6, 12));
    TEST_CHECK(!strcmp(s, "猫が<b>好き</b>"));
    BUF(vjo_anki_sentence(&b, "猫が\n好き", 3, 7)); /* across a line break */
    TEST_CHECK(!strcmp(s, "猫<b>が<br></b>好き"));
    BUF(vjo_anki_sentence(&b, "𠮷野家", 0, 4)); /* 4-byte code point */
    TEST_CHECK(!strcmp(s, "<b>𠮷</b>野家"));
    BUF(vjo_anki_sentence(&b, "猫", -1, -1));
    TEST_CHECK(!strcmp(s, "猫"));
    BUF(vjo_anki_sentence(&b, "猫", 0, 99)); /* out of range: no bold */
    TEST_CHECK(!strcmp(s, "猫"));

    /* Yomitan's {furigana-plain}, which Kiku's install guide asks for: each
     * kanji run takes its part of the reading; a space before a base that
     * follows other text, so Kiku's parser (and Anki's filter) read only that
     * run: お茶[おちゃ] would put おちゃ over 茶 in Kiku */
    {
        static const char *const cases[][3] = {
            {"言葉", "ことば", "言葉[ことば]"},
            {"食べる", "たべる", "食[た]べる"},
            {"お茶", "おちゃ", "お 茶[ちゃ]"},
            {"ご飯", "ごはん", "ご 飯[はん]"},
            {"お母さん", "おかあさん", "お 母[かあ]さん"},
            {"真っ白", "まっしろ", "真[ま]っ 白[しろ]"},
            {"見張り番", "みはりばん", "見張[みは]り 番[ばん]"},
            {"取り扱い", "とりあつかい", "取[と]り 扱[あつか]い"},
            {"大人しい", "おとなしい", "大人[おとな]しい"},
            {"時々", "ときどき", "時々[ときどき]"},
            {"ブラック企業", "ぶらっくきぎょう", "ブラック 企業[きぎょう]"}, /* katakana as hiragana */
            {"一ヶ月", "いっかげつ", " 一ヶ月[いっかげつ]"},             /* ヶ: kana to Kiku */
            /* the kana do not line up, or line up two ways: the whole word, after a space */
            {"今日は", "きょうわ", " 今日は[きょうわ]"},
            {"物の怪", "もののけ", " 物の怪[もののけ]"},   /* も|のけ or もの|け */
            {"物の哀れ", "もののあわれ", " 物の哀れ[もののあわれ]"},
            {"3日", "みっか", " 3日[みっか]"},
            {"ある", "ある", "ある"},
            {"テレビ", "てれび", "テレビ"}, /* no kanji */
            {"猫", "", "猫"},
            {"a<b&猫", "a<b&ねこ", "a&lt;b&amp; 猫[ねこ]"},
        };
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            BUF(vjo_anki_furigana(&b, cases[i][0], cases[i][1]));
            TEST_CHECK(!strcmp(s, cases[i][2]));
            TEST_MSG("%s %s -> '%s', want '%s'", cases[i][0], cases[i][1], s, cases[i][2]);
        }
        /* longer than the distribution handles, or that never lines up: the whole word */
        char word[200] = "", reading[300] = "";
        for (int i = 0; i < 49; i++)
            strcat(word, "茶");
        for (int i = 0; i < 49; i++)
            strcat(reading, "ちゃ");
        BUF(vjo_anki_furigana(&b, word, reading));
        TEST_CHECK(s[0] != ' ' && strstr(s, "茶[ちゃちゃ") != NULL);
        /* 16 runs of kanji between の over a reading of 47 の that never ends in the
         * word's x: no fit, given up within the step bound */
        strcpy(word, "");
        strcpy(reading, "");
        for (int i = 0; i < 15; i++)
            strcat(word, "茶の");
        strcat(word, "茶x");
        for (int i = 0; i < 47; i++)
            strcat(reading, "の");
        BUF(vjo_anki_furigana(&b, word, reading));
        TEST_CHECK(s[0] == ' ' && strstr(s, "茶x[のの") != NULL);
        TEST_MSG("%.40s", s);
    }

#undef BUF
    {
        /* a local definition's first line names its dictionary (tools/convert_dictionary.py) */
        char name[16];
        const char *t = vjo_anki_meaning_source("[JMdict Plain]\nto eat\nto bite", name, sizeof(name));
        TEST_CHECK(!strcmp(name, "JMdict Plain") && !strcmp(t, "to eat\nto bite"));
        t = vjo_anki_meaning_source("[Jitendex.org [2026-05-05]]\nx", name, sizeof(name)); /* brackets in it, cut */
        TEST_CHECK(!strcmp(name, "Jitendex.org [2") && !strcmp(t, "x"));
        t = vjo_anki_meaning_source("[大辞林第四版]\nx", name, 8); /* cut on a character */
        TEST_CHECK(!strcmp(name, "大辞") && !strcmp(t, "x"));
        static const char *const plain[] = {"to eat", "[x] y", "[x]", "[]\nz", "[x\n]y", "x\n[y]\n"};
        for (unsigned i = 0; i < sizeof(plain) / sizeof(plain[0]); i++) {
            TEST_CHECK(vjo_anki_meaning_source(plain[i], name, sizeof(name)) == plain[i] && !name[0]);
            TEST_MSG("%s", plain[i]);
        }
    }
    {
        /* MainDefinition and Glossary as Yomitan writes them, which Kiku pages by dictionary */
        static const char *const daijirin[] = {"[Daijirin4 Plain]\nA room; a <chamber>.\nAlso a stable."};
        static const char *const jmdict[] = {"[JMdict Plain]\nroom", "chamber"};
        static const char *const jitendex[] = {"room \"heya\"", "chamber"};
        VjoAnkiGloss more[2] = {{NULL, "部屋", "へや", (const char **)jmdict, 2},
                                {"Jitendex.org", "部屋", "へや", (const char **)jitendex, 2}};
        VjoAnkiNote n = {.spelling = "部屋", .reading = "へや", .meanings = (const char **)daijirin, .n_meanings = 1,
                         .sentence = "", .rank = VJO_NO_RANK};
        setup();
        TEST_CHECK(!strcmp(vjo_anki_field_value(&A, VJO_KIKU_MAIN_DEFINITION, &n),
                           "<div style=\"text-align: left;\" class=\"yomitan-glossary\"><ol>"
                           "<li data-dictionary=\"Daijirin4 Plain\"><i>(Daijirin4 Plain)</i> <span>A room; a "
                           "&lt;chamber&gt;.<br>Also a stable.</span></li></ol></div>"));
        TEST_MSG("%s", vjo_anki_field_value(&A, VJO_KIKU_MAIN_DEFINITION, &n));
        TEST_CHECK(!strcmp(vjo_anki_field_value(&A, VJO_KIKU_GLOSSARY, &n), "")); /* one dictionary: none */
        n.more = more;
        n.n_more = 2;
        TEST_CHECK(!strcmp(vjo_anki_field_value(&A, VJO_KIKU_GLOSSARY, &n),
                           "<div style=\"text-align: left;\" class=\"yomitan-glossary\"><ol>"
                           "<li data-dictionary=\"Daijirin4 Plain\"><i>(Daijirin4 Plain)</i> <span>A room; a "
                           "&lt;chamber&gt;.<br>Also a stable.</span></li>"
                           "<li data-dictionary=\"JMdict Plain\"><i>(JMdict Plain)</i> <span><ul><li>room</li>"
                           "<li>chamber</li></ul></span></li>"
                           "<li data-dictionary=\"Jitendex.org\"><i>(Jitendex.org)</i> <span><ul><li>room &quot;heya&quot;"
                           "</li><li>chamber</li></ul></span></li></ol></div>"));
        TEST_MSG("%s", vjo_anki_field_value(&A, VJO_KIKU_GLOSSARY, &n));
        /* a name of the record's own wins; none at all: no label */
        n.dictionary = "Daijirin <4>";
        TEST_CHECK(strstr(vjo_anki_field_value(&A, VJO_KIKU_MAIN_DEFINITION, &n),
                          "<li data-dictionary=\"Daijirin &lt;4&gt;\"><i>(Daijirin &lt;4&gt;)</i> <span>A room;") != NULL);
        n.meanings = (const char **)jitendex;
        n.n_meanings = 2;
        n.dictionary = NULL;
        TEST_CHECK(!strcmp(vjo_anki_field_value(&A, VJO_KIKU_MAIN_DEFINITION, &n),
                           "<div style=\"text-align: left;\" class=\"yomitan-glossary\"><ol><li><span><ul><li>"
                           "room &quot;heya&quot;</li><li>chamber</li></ul></span></li></ol></div>"));
        n.n_meanings = 0;
        TEST_CHECK(!strcmp(vjo_anki_field_value(&A, VJO_KIKU_MAIN_DEFINITION, &n), ""));
        /* the dictionary's ruby in Anki's syntax (a queued record of a removed online
         * dictionary's lookup has one), else the whole word's reading */
        n.spelling = "取り扱い";
        n.reading = "とりあつかい";
        TEST_CHECK(!strcmp(vjo_anki_field_value(&A, VJO_KIKU_EXPRESSION_FURIGANA, &n), "取[と]り 扱[あつか]い"));
        n.furigana = "取[と]り扱[あつか]い";
        TEST_CHECK(!strcmp(vjo_anki_field_value(&A, VJO_KIKU_EXPRESSION_FURIGANA, &n), "取[と]り 扱[あつか]い"));
        TEST_MSG("%s", vjo_anki_field_value(&A, VJO_KIKU_EXPRESSION_FURIGANA, &n));
    }

    {
        char name[40];
        vjo_anki_media_name(name, sizeof(name), 1791158400007ull, "jpg");
        TEST_CHECK(!strcmp(name, "vitajp_1791158400007.jpg"));
    }
    {
        uint32_t c[300];
        int n = vjo_anki_scan_candidates(0xC0A8011Eu, 0xFFFFFF00u, c, 300); /* 192.168.1.30/24 */
        TEST_CHECK(n == 253 && c[0] == 0xC0A80101u && c[252] == 0xC0A801FEu);
        for (int i = 0; i < n; i++)
            TEST_CHECK(c[i] != 0xC0A8011Eu);
        n = vjo_anki_scan_candidates(0x0A000105u, 0xFFFF0000u, c, 300); /* a /16: its /24 only */
        TEST_CHECK(n == 253 && c[0] == 0x0A000101u);
        n = vjo_anki_scan_candidates(0xC0A80182u, 0xFFFFFF80u, c, 300); /* a /25 */
        TEST_CHECK(n == 125 && c[0] == 0xC0A80181u && c[124] == 0xC0A801FEu);
        TEST_CHECK(vjo_anki_scan_candidates(0xC0A8011Eu, 0xFFFFFF00u, c, 10) == 10);
    }
}

static void test_anki_pitch(void)
{
    static const char *const noun[] = {"n"}, *const verb[] = {"v5m", "vt"}, *const suru[] = {"n", "vs"},
                             *const adj[] = {"adj-i"}, *const adj_ix[] = {"adj-ix"};
    static const struct {
        const char *reading;
        int pitch[VJO_MAX_PITCH], n_pitch;
        const char *const *pos;
        int n_pos;
        const char *want;
    } cases[] = {
        {"きのう", {2}, 1, noun, 1, "nakadaka"},
        {"あした", {0, 3}, 2, noun, 1, "heiban,odaka"},
        {"きゃく", {2}, 1, noun, 1, "odaka"}, /* きゃ is one mora */
        {"いえ", {1, 1}, 2, noun, 1, "atamadaka"},
        {"よむ", {1}, 1, verb, 2, "kifuku"},
        {"きわどい", {3}, 1, adj, 1, "kifuku"},
        {"いい", {1}, 1, adj_ix, 1, "kifuku"},
        {"べんきょう", {0, 2}, 2, suru, 2, "heiban,nakadaka"}, /* a noun with する: not a verb */
        {"ねこ", {0}, 0, noun, 1, ""},
    };
    VjoBuf b;
    setup();
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        vjo_buf_init(&b, &A);
        vjo_anki_pitch_categories(&b, cases[i].reading, cases[i].pitch, cases[i].n_pitch, cases[i].pos,
                                  cases[i].n_pos);
        TEST_CHECK(!strcmp(vjo_buf_cstr(&b), cases[i].want));
        TEST_MSG("%s: %s", cases[i].reading, vjo_buf_cstr(&b));
    }
    {
        char misc[64];
        VjoAnkiDate when = {2026, 1, 2, 3, 4};
        vjo_anki_misc_info(misc, sizeof(misc), "", &when);
        TEST_CHECK(!strcmp(misc, "2026-01-02 03:04"));
        vjo_anki_misc_info(misc, sizeof(misc), "ペルソナ", NULL); /* no clock: no date */
        TEST_CHECK(!strcmp(misc, "ペルソナ"));
        vjo_anki_misc_info(misc, 8, "ペルソナ", NULL); /* cut on a character: "ペル" is 6 bytes */
        TEST_CHECK(!strcmp(misc, "ペル"));
    }
}

/* A dictionary result for "猫が\n好き" with tricky glosses, and its words'
 * frequency, part of speech and pitch (fields a dictionary result may fill;
 * its frequency list is "BCCWJ"). */
static void anki_list(VjoEntryList *l)
{
    static const char *const ga[] = {"indicates subject"}, *const neko[] = {"cat \"neko\"", "a\\b <&>\nc"};
    static const char *const suki[] = {"liked"}, *const prt[] = {"prt"}, *const noun[] = {"n"};
    static const char *const na[] = {"adj-na", "n"};
    VjoDictResult r = {0};
    VjoVocab *v = vjo_arena_zalloc(&A, 3 * sizeof(*v));
    VjoToken *t = vjo_arena_zalloc(&A, 3 * sizeof(*t));
    TEST_ASSERT(v && t);
    v[0] = (VjoVocab){.spelling = "が", .reading = "が", .rank = 50, .meanings = (const char **)ga, .n_meanings = 1,
                      .pos = (const char **)prt, .n_pos = 1, .pitch = {0}, .n_pitch = 1};
    v[1] = (VjoVocab){.spelling = "猫", .reading = "ねこ", .rank = 1500, .meanings = (const char **)neko,
                      .n_meanings = 2, .pos = (const char **)noun, .n_pos = 1, .pitch = {1}, .n_pitch = 1};
    v[2] = (VjoVocab){.spelling = "好き", .reading = "すき", .rank = VJO_NO_RANK, .meanings = (const char **)suki,
                      .n_meanings = 1, .pos = (const char **)na, .n_pos = 2, .pitch = {2, 1}, .n_pitch = 2};
    t[0] = (VjoToken){1, 0, 1};
    t[1] = (VjoToken){0, 1, 1};
    t[2] = (VjoToken){2, 2, 2};
    r.vocab = v;
    r.n_vocab = 3;
    r.tokens = t;
    r.n_tokens = 3;
    r.source = "BCCWJ";
    TEST_ASSERT(vjo_entries_build(&A, "猫が\n好き", &r, l) == 0);
}

/* The one target: Kiku notes in the Kiku deck, duplicates checked there. */
#define OPTS                                                                                                  \
    "\"options\":{\"allowDuplicate\":false,\"duplicateScope\":\"deck\",\"duplicateScopeOptions\":"            \
    "{\"deckName\":\"Kiku\",\"checkChildren\":false,\"checkAllModels\":false}}"
#define TARGET "\"deckName\":\"Kiku\",\"modelName\":\"Kiku\""

/* A local lookup's card (the shipped default): one entry, no frequency, pitch or
 * part of speech; SentenceFurigana is the sentence as it is, the word in bold,
 * as Sentence (no dictionary gives readings per word); ExpressionFurigana is
 * the word's reading. Without a highlight nothing is in bold. */
static void test_anki_local_card(void)
{
    static const char *const meanings[] = {"[JMdict Plain]\nto come"};
    VjoDictResult r = {0};
    VjoEntryList l;
    VjoAnkiNote n;
    VjoAnkiMedia none = {0};
    VjoAnkiBody b;
    setup();
    r.vocab = vjo_arena_zalloc(&A, sizeof(VjoVocab));
    r.tokens = vjo_arena_zalloc(&A, sizeof(VjoToken));
    TEST_ASSERT(r.vocab && r.tokens);
    r.vocab[0] = (VjoVocab){.spelling = "来る", .reading = "くる", .rank = VJO_NO_RANK,
                            .meanings = (const char **)meanings, .n_meanings = 1};
    r.tokens[0] = (VjoToken){0, 2, 1};
    r.n_vocab = r.n_tokens = 1;
    TEST_ASSERT(vjo_entries_build(&A, "彼は\n来なかった。", &r, &l) == 0);
    TEST_ASSERT(vjo_anki_note_from_entry(&A, &l, 0, &n) == 0);
    TEST_CHECK(!n.furigana && !n.source && !n.n_pitch && !n.n_pos && n.rank == VJO_NO_RANK);
    TEST_CHECK(n.sentence_furigana && !strcmp(n.sentence_furigana, "彼は\n来なかった。") &&
               n.sf_hl_start == n.hl_start && n.sf_hl_end == n.hl_end && n.hl_start == 7 && n.hl_end == 10);
    TEST_ASSERT(vjo_anki_add_request(&A, &n, &none, &b) == 0);
    TEST_CHECK(strstr(b.prefix, "\"Sentence\":\"彼は<br><b>来</b>なかった。\",\"SentenceFurigana\":\"彼は<br><b>来</b>"
                                "なかった。\"") != NULL);
    TEST_CHECK(strstr(b.prefix, "\"ExpressionFurigana\":\"来[く]る\"") != NULL);
    TEST_CHECK(!strstr(b.prefix, "\"Frequency\"") && !strstr(b.prefix, "\"FreqSort\"") &&
               !strstr(b.prefix, "\"PitchPosition\"") && !strstr(b.prefix, "\"PitchCategories\""));
    TEST_MSG("%s", b.prefix);
    /* a word without a highlight: nothing in bold */
    l.entries[0].hl_start = l.entries[0].hl_end = -1;
    TEST_ASSERT(vjo_anki_note_from_entry(&A, &l, 0, &n) == 0);
    TEST_CHECK(n.sf_hl_start < 0 && n.sf_hl_end < 0 && !strcmp(n.sentence_furigana, "彼は\n来なかった。"));
}

/* A request takes the same bytes wherever it is built: shell/anki.c's save checks with a dry run
 * in its own arena that △ can send the card, and the send builds it in another, emptier one. A
 * VjoBuf grows by doubling where the arena has room and to the exact size near its end, so each
 * field's value and the request are cut to their text: the bytes used never depend on the room
 * (a card the save said fits was refused by the send when VjoConfig shrank; host test
 * what_cross_saves_triangle_sends_whatever_the_dictionaries). */
static void test_anki_request_bytes_do_not_depend_on_the_arena(void)
{
    static uint8_t room[512 * 1024];
    static char text[4][5001];
    const char *meanings[1] = {text[0]}, *more_meanings[3][1] = {{text[1]}, {text[2]}, {text[3]}};
    VjoAnkiGloss more[3];
    VjoAnkiNote n = {.spelling = "好き", .reading = "すき", .rank = 1200, .meanings = meanings, .n_meanings = 1,
                     .sentence = "猫が\n好き", .hl_start = 10, .hl_end = 16, .sentence_furigana = "猫が\n好き",
                     .sf_hl_start = 10, .sf_hl_end = 16, .source = "BCCWJ", .dictionary = "Daijirin"};
    VjoAnkiMedia none = {0};
    VjoAnkiBody b;
    VjoArena a;
    size_t need;
    for (int k = 0; k < 4; k++) { /* escaping grows it: & < > " and line breaks */
        for (int i = 0; i < 5000; i++)
            text[k][i] = "\"&<\n>xy"[(i + k) % 7];
        text[k][5000] = '\0';
    }
    for (int k = 0; k < 3; k++)
        more[k] = (VjoAnkiGloss){"JMdict", "好き", "すき", more_meanings[k], 1};
    n.more = more;
    n.n_more = 3;
    vjo_arena_init(&a, room, sizeof(room));
    TEST_ASSERT(vjo_anki_add_request(&a, &n, &none, &b) == 0);
    need = a.used;
    TEST_CHECK(b.prefix_len > 40000 && need < sizeof(room) / 2);
    TEST_MSG("request %u B, %u B used", (unsigned)b.prefix_len, (unsigned)need);
    for (size_t size = need; size <= sizeof(room); size += 3001) {
        vjo_arena_init(&a, room, size);
        int ok = vjo_anki_add_request(&a, &n, &none, &b) == 0;
        TEST_CHECK_(ok && a.used == need, "arena of %u B: %s, %u B used (%u in the others)", (unsigned)size,
                    ok ? "built" : "refused", (unsigned)a.used, (unsigned)need);
        if (!ok || a.used != need)
            break;
    }
    vjo_arena_init(&a, room, need - 1);
    TEST_CHECK(vjo_anki_add_request(&a, &n, &none, &b) < 0);
}

static void test_anki_requests(void)
{
    VjoEntryList l;
    VjoAnkiNote n;
    VjoAnkiMedia m = {(const uint8_t *)"jpeg", 4, "vitajp_1.jpg", NULL, NULL};
    VjoAnkiBody b;
    char *s, misc[96];
    setup();
    anki_list(&l);

    s = vjo_anki_can_add_request(&A, &l, 99);
    TEST_CHECK(s && !strcmp(s, "{\"action\":\"canAddNotesWithErrorDetail\",\"version\":6,\"params\":{\"notes\":["
                               "{" TARGET ",\"fields\":{\"Expression\":\"猫\"}," OPTS "},"
                               "{" TARGET ",\"fields\":{\"Expression\":\"が\"}," OPTS "},"
                               "{" TARGET ",\"fields\":{\"Expression\":\"好き\"}," OPTS "}]}}"));
    TEST_MSG("%s", s);
    s = vjo_anki_can_add_request(&A, &l, 1); /* the first n only */
    TEST_CHECK(s && strstr(s, "猫") && !strstr(s, "が"));

    TEST_ASSERT(vjo_anki_note_from_entry(&A, &l, 0, &n) == 0);
    TEST_CHECK(vjo_anki_note_from_entry(&A, &l, 3, &n) < 0);
    {
        VjoAnkiDate when = {2026, 10, 11, 9, 5};
        vjo_anki_misc_info(misc, sizeof(misc), "Persona 4 <Golden>", &when);
    }
    n.misc = misc;
    n.dictionary = "Jitendex.org";
    TEST_ASSERT(vjo_anki_add_request(&A, &n, &m, &b) == 0);
    /* Kiku v2.1.0's field names, in its order, the media fields named empty */
    TEST_CHECK(!strcmp(b.prefix,
                       "{\"action\":\"addNote\",\"version\":6,\"params\":{\"note\":{" TARGET ",\"fields\":{"
                       "\"Expression\":\"猫\",\"ExpressionFurigana\":\"猫[ねこ]\",\"ExpressionReading\":\"ねこ\","
                       "\"MainDefinition\":\"<div style=\\\"text-align: left;\\\" class=\\\"yomitan-glossary\\\"><ol>"
                       "<li data-dictionary=\\\"Jitendex.org\\\"><i>(Jitendex.org)</i> <span><ul><li>cat &quot;neko&quot;</li>"
                       "<li>a\\\\b &lt;&amp;&gt;<br>c</li></ul></span></li></ol></div>\","
                       "\"Sentence\":\"<b>猫</b>が<br>好き\",\"SentenceFurigana\":\"<b>猫</b>が<br>好き\","
                       "\"Picture\":\"\","
                       "\"PitchPosition\":\"<span style=\\\"display:inline;\\\"><span>[</span><span>1</span>"
                       "<span>]</span></span>\",\"PitchCategories\":\"atamadaka\","
                       "\"Frequency\":\"<ul style=\\\"text-align: left;\\\"><li>BCCWJ: 1500</li></ul>\","
                       "\"FreqSort\":\"1500\","
                       "\"MiscInfo\":\"Persona 4 &lt;Golden&gt; (2026-10-11 09:05)\"},"
                       "\"tags\":[\"vita-jp-overlay\"]," OPTS ","
                       "\"picture\":[{\"filename\":\"vitajp_1.jpg\",\"fields\":[\"Picture\"],\"data\":\""));
    TEST_MSG("%s", b.prefix);
    TEST_CHECK(b.prefix_len == strlen(b.prefix) && !strcmp(b.suffix, "\"}]}}}") && b.suffix_len == 6);
    TEST_CHECK(b.picture == m.picture && b.picture_len == 4);

    /* the word's audio, for Anki to download, before the picture; the queue's tag */
    m.audio_url = "https://example.com/a \"b\".opus";
    m.audio_name = "vitajp_1.opus";
    n.queue_tag = "vita_queue_00ff";
    TEST_ASSERT(vjo_anki_add_request(&A, &n, &m, &b) == 0);
    /* media fields are named, empty: the rewritten AnkiConnect fills only named ones */
    TEST_CHECK(strstr(b.prefix, "\"ExpressionReading\":\"ねこ\",\"ExpressionAudio\":\"\",\"MainDefinition\"") &&
               strstr(b.prefix, "\"Picture\":\"\",\"PitchPosition\""));
    TEST_CHECK(strstr(b.prefix, "\"tags\":[\"vita-jp-overlay\",\"vita_queue_00ff\"]," OPTS
                                ",\"audio\":[{\"filename\":\"vitajp_1.opus\",\"fields\":[\"ExpressionAudio\"],"
                                "\"url\":\"https://example.com/a \\\"b\\\".opus\"}],"
                                "\"picture\":[{\"filename\":\"vitajp_1.jpg\"") != NULL);
    TEST_MSG("%s", b.prefix);

    /* nothing to fill is left out: no rank, no meanings, no picture or audio */
    m = (VjoAnkiMedia){0};
    TEST_ASSERT(vjo_anki_note_from_entry(&A, &l, 2, &n) == 0);
    n.meanings = NULL;
    n.n_meanings = 0;
    TEST_ASSERT(vjo_anki_add_request(&A, &n, &m, &b) == 0);
    TEST_CHECK(!strcmp(b.prefix, "{\"action\":\"addNote\",\"version\":6,\"params\":{\"note\":{" TARGET
                                 ",\"fields\":{\"Expression\":\"好き\",\"ExpressionFurigana\":\"好[す]き\","
                                 "\"ExpressionReading\":\"すき\",\"Sentence\":\"猫が<br><b>好き</b>\","
                                 "\"SentenceFurigana\":\"猫が<br><b>好き</b>\",\"PitchPosition\":\"<ol>"
                                 "<li><span style=\\\"display:inline;\\\"><span>[</span><span>2</span><span>]</span></span></li>"
                                 "<li><span style=\\\"display:inline;\\\"><span>[</span><span>1</span><span>]</span></span></li>"
                                 "</ol>\",\"PitchCategories\":\"odaka,atamadaka\"},\"tags\":[\"vita-jp-overlay\"]," OPTS "}}}"));
    TEST_MSG("%s", b.prefix);
    TEST_CHECK(b.suffix_len == 0 && b.picture_len == 0);
}

/* ---------- AnkiConnect over a fake network ---------- */

/* A VjoPlatform that serves canned HTTP responses in turn (NULL = nothing
 * listening) and records what was sent. */
typedef struct {
    const char *responses[4];
    int n_responses, next;
    VjoMemConn conn;
    char sent[4][8192];
    int port, timeout_us, io_timeout_us;
} FakeNet;

static int fake_connect(void *ud, const char *host, int port, int timeout_us, int io_timeout_us, VjoConn *out)
{
    FakeNet *f = (FakeNet *)ud;
    int k = f->next;
    (void)host;
    if (k >= f->n_responses || !f->responses[k])
        return VJO_E_NET;
    f->next++;
    f->port = port;
    f->timeout_us = timeout_us;
    f->io_timeout_us = io_timeout_us;
    memset(&f->conn, 0, sizeof(f->conn));
    f->conn.in = f->responses[k];
    f->conn.len = strlen(f->responses[k]);
    f->conn.out = f->sent[k];
    f->conn.out_cap = sizeof(f->sent[k]) - 1;
    vjo_memconn_init(&f->conn, out);
    return VJO_OK;
}

static void fake_disconnect(void *ud, VjoConn *c)
{
    FakeNet *f = (FakeNet *)ud;
    (void)c;
    f->sent[f->next - 1][f->conn.out_len] = '\0';
}

static void fake_net(FakeNet *f, VjoPlatform *p, int n, ...)
{
    va_list ap;
    memset(f, 0, sizeof(*f));
    memset(p, 0, sizeof(*p));
    p->ud = f;
    p->connect = fake_connect;
    p->disconnect = fake_disconnect;
    va_start(ap, n);
    for (int i = 0; i < n; i++)
        f->responses[i] = va_arg(ap, const char *);
    va_end(ap);
    f->n_responses = n;
}

/* A 200 response with a JSON body (one of 4 static buffers). */
static const char *ok(const char *json)
{
    static char bufs[4][1024];
    static int k;
    char *b = bufs[k++ % 4];
    snprintf(b, sizeof(bufs[0]), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %lu\r\n\r\n%s",
             (unsigned long)strlen(json), json);
    return b;
}

#define DUPLICATE "cannot create note because it is a duplicate"

static void test_anki_probe(void)
{
    FakeNet f;
    VjoPlatform p;
    VjoErr err;
    setup();
    /* plain HTTP to the port, Content-Length, a connect timeout */
    fake_net(&f, &p, 1, ok("{\"result\": 6, \"error\": null}"));
    TEST_CHECK(vjo_anki_probe(&A, &p, "192.168.1.23", 8765, &err) == VJO_OK);
    TEST_CHECK(f.port == 8765 && f.timeout_us > 0);
    TEST_CHECK(!strcmp(f.sent[0], "POST / HTTP/1.1\r\nHost: 192.168.1.23:8765\r\nContent-Type: application/json\r\n"
                                  "Content-Length: 32\r\nConnection: close\r\n\r\n"
                                  "{\"action\":\"version\",\"version\":6}"));
    TEST_MSG("%s", f.sent[0]);
    /* something else on the port; then nothing listening */
    fake_net(&f, &p, 3, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi", ok("{\"result\": 6}"),
             ok("[6]"));
    TEST_CHECK(vjo_anki_probe(&A, &p, "h", 8765, &err) == VJO_E_PARSE);
    TEST_CHECK(vjo_anki_probe(&A, &p, "h", 8765, &err) == VJO_E_PARSE); /* no "error" */
    TEST_CHECK(vjo_anki_probe(&A, &p, "h", 8765, &err) == VJO_E_PARSE);
    TEST_CHECK(vjo_anki_probe(&A, &p, "h", 8765, &err) == VJO_E_NET);
    TEST_CHECK(!strcmp(vjo_anki_err_text(&A, &err), "Anki offline"));
}

static void test_anki_check(void)
{
    FakeNet f;
    VjoPlatform p;
    VjoEntryList l;
    VjoErr err;
    uint8_t marks[4];
    const char *req;
    setup();
    anki_list(&l);
    req = vjo_anki_can_add_request(&A, &l, 3);
    TEST_ASSERT(req != NULL);
    /* only a duplicate is marked: a missing deck (even one named after
     * duplicates) or note type is not */
    fake_net(&f, &p, 1,
             ok("{\"result\": [{\"canAdd\": false, \"error\": \"" DUPLICATE "\"}, {\"canAdd\": true}, "
                "{\"canAdd\": false, \"error\": \"deck was not found: Duplicates\"}], \"error\": null}"));
    memset(marks, 7, sizeof(marks));
    TEST_CHECK(vjo_anki_check(&A, &p, "h", 8765, req, marks, 3, &err) == VJO_OK);
    TEST_CHECK(marks[0] == 1 && marks[1] == 0 && marks[2] == 0 && marks[3] == 7);
    TEST_CHECK(strstr(f.sent[0], "\"Expression\":\"が\"") != NULL);
    /* the rewritten AnkiConnect (2026) words a duplicate differently */
    fake_net(&f, &p, 1,
             ok("{\"result\": [{\"canAdd\": false, \"error\": \"A matching note exists.\"}, {\"canAdd\": true}, "
                "{\"canAdd\": false, \"error\": \"Deck not found: {'name': 'Duplicates'}\"}], \"error\": null}"));
    TEST_CHECK(vjo_anki_check(&A, &p, "h", 8765, req, marks, 3, &err) == VJO_OK);
    TEST_CHECK(marks[0] == 1 && marks[1] == 0 && marks[2] == 0);
    /* malformed: the wrong count, a non-boolean canAdd, an old AnkiConnect */
    fake_net(&f, &p, 3, ok("{\"result\": [{\"canAdd\": true}], \"error\": null}"),
             ok("{\"result\": [{\"canAdd\": \"false\"}, {\"canAdd\": true}, {\"canAdd\": true}], \"error\": null}"),
             ok("{\"result\": null, \"error\": \"unsupported action\"}"));
    TEST_CHECK(vjo_anki_check(&A, &p, "h", 8765, req, marks, 3, &err) == VJO_E_PARSE);
    TEST_CHECK(vjo_anki_check(&A, &p, "h", 8765, req, marks, 3, &err) == VJO_E_PARSE);
    TEST_CHECK(vjo_anki_check(&A, &p, "h", 8765, req, marks, 3, &err) == VJO_E_ANKI);
    TEST_CHECK(!strcmp(vjo_anki_err_text(&A, &err), "Anki: unsupported action"));
}

static void test_anki_add(void)
{
    FakeNet f;
    VjoPlatform p;
    VjoEntryList l;
    VjoAnkiNote n;
    VjoErr err;
    VjoAnkiMedia m = {0}, none = {0};
    uint8_t jpeg[100];
    char b64[200] = {0};
    const char *body;

    setup();
    anki_list(&l);
    TEST_ASSERT(vjo_anki_note_from_entry(&A, &l, 0, &n) == 0);
    for (int i = 0; i < 100; i++)
        jpeg[i] = (uint8_t)(255 - i);
    vjo_base64_encode(jpeg, sizeof(jpeg), b64);

    /* one addNote, the picture streamed as base64 with an exact length */
    fake_net(&f, &p, 1, ok("{\"result\": 1700000000001, \"error\": null}"));
    m.picture = jpeg;
    m.picture_len = sizeof(jpeg);
    m.picture_name = "vitajp_1.jpg";
    TEST_CHECK(vjo_anki_add(&A, &p, "h", 8765, &n, &m, &err) == VJO_OK);
    TEST_CHECK(f.next == 1);
    body = strstr(f.sent[0], "\r\n\r\n");
    TEST_ASSERT(body != NULL);
    body += 4;
    {
        char cl[64];
        const char *data = strstr(body, "\"data\":\"");
        snprintf(cl, sizeof(cl), "Content-Length: %lu\r\n", (unsigned long)strlen(body));
        TEST_CHECK(strstr(f.sent[0], cl) != NULL);
        TEST_MSG("%s", f.sent[0]);
        TEST_ASSERT(data != NULL);
        data += 8;
        TEST_CHECK(!strncmp(data, b64, strlen(b64)) && !strcmp(data + strlen(b64), "\"}]}}}"));
        TEST_CHECK(!strncmp(body, "{\"action\":\"addNote\"", 19));
    }

    /* a new deck: created, then the note again */
    fake_net(&f, &p, 3, ok("{\"result\": null, \"error\": \"deck was not found: Kiku\"}"),
             ok("{\"result\": 1, \"error\": null}"), ok("{\"result\": 2, \"error\": null}"));
    TEST_CHECK(vjo_anki_add(&A, &p, "h", 8765, &n, &none, &err) == VJO_OK);
    TEST_CHECK(f.next == 3 && strstr(f.sent[2], "\"action\":\"addNote\""));
    TEST_CHECK(strstr(f.sent[1], "{\"action\":\"createDeck\",\"version\":6,\"params\":{\"deck\":\"Kiku\"}}"));
    TEST_CHECK(!strstr(f.sent[2], "\"picture\"")); /* no JPEG: no picture */

    /* AnkiConnect refusals and their messages */
#define REFUSED(error, rc, text)                                                                               \
    do {                                                                                                       \
        fake_net(&f, &p, 1, ok("{\"result\": null, \"error\": \"" error "\"}"));                             \
        TEST_CHECK(vjo_anki_add(&A, &p, "h", 8765, &n, &none, &err) == (rc));                   \
        TEST_CHECK(!strcmp(vjo_anki_err_text(&A, &err), text));                                                \
        TEST_MSG("%s -> %s", error, vjo_anki_err_text(&A, &err));                                              \
    } while (0)
    REFUSED(DUPLICATE, VJO_E_ANKI_DUPLICATE, "Already in Anki");
#define NO_KIKU "Kiku note type not found in Anki: import Kiku (docs/anki.md)"
    REFUSED("model was not found: Kiku", VJO_E_ANKI, NO_KIKU);
    /* a note type named after duplicates is not one */
    REFUSED("model was not found: duplicate-kiku", VJO_E_ANKI, NO_KIKU);
    REFUSED("cannot create note because it is empty", VJO_E_ANKI, "Anki: cannot create note because it is empty");
    REFUSED("collection is not available", VJO_E_ANKI, "Anki: collection is not available");
    REFUSED("Notetype not found: {'name': 'Kiku'}", VJO_E_ANKI, NO_KIKU);
#undef REFUSED

    /* The rewritten AnkiConnect (2026): a new deck is "Deck not found" */
    fake_net(&f, &p, 3, ok("{\"result\": null, \"error\": \"Deck not found: {'name': 'Kiku'}\"}"),
             ok("{\"result\": 1, \"error\": null}"), ok("{\"result\": 2, \"error\": null}"));
    TEST_CHECK(vjo_anki_add(&A, &p, "h", 8765, &n, &none, &err) == VJO_OK);
    TEST_CHECK(f.next == 3 && strstr(f.sent[1], "\"action\":\"createDeck\""));
    /* ... and a refused note names no reason: asked with canAddNotesWithErrorDetail */
#define VALIDATION "{\"result\": null, \"error\": \"One or more notes failed validation.\"}"
    fake_net(&f, &p, 2, ok(VALIDATION),
             ok("{\"result\": [{\"canAdd\": false, \"error\": \"A matching note exists.\"}], \"error\": null}"));
    TEST_CHECK(vjo_anki_add(&A, &p, "h", 8765, &n, &none, &err) == VJO_E_ANKI_DUPLICATE);
    TEST_CHECK(f.next == 2 && strstr(f.sent[1], "\"action\":\"canAddNotesWithErrorDetail\""));
    TEST_CHECK(!strcmp(vjo_anki_err_text(&A, &err), "Already in Anki"));
    fake_net(&f, &p, 2, ok(VALIDATION),
             ok("{\"result\": [{\"canAdd\": false, \"error\": \"Anki field validation: emptyFirstField\"}], "
                "\"error\": null}"));
    TEST_CHECK(vjo_anki_add(&A, &p, "h", 8765, &n, &none, &err) == VJO_E_ANKI);
    TEST_CHECK(!strcmp(vjo_anki_err_text(&A, &err), "Anki: Anki field validation: emptyFirstField"));
    fake_net(&f, &p, 1, ok(VALIDATION)); /* the question fails too: the refusal as it is */
    TEST_CHECK(vjo_anki_add(&A, &p, "h", 8765, &n, &none, &err) == VJO_E_ANKI);
    TEST_CHECK(!strcmp(vjo_anki_err_text(&A, &err), "Anki: One or more notes failed validation."));
#undef VALIDATION

    fake_net(&f, &p, 1, "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n");
    TEST_CHECK(vjo_anki_add(&A, &p, "h", 8765, &n, &none, &err) == VJO_E_STATUS);
    TEST_CHECK(!strcmp(vjo_anki_err_text(&A, &err), "Anki refused the request (HTTP 403)"));
    err.rc = VJO_E_NOT_FOUND;
    TEST_CHECK(!strcmp(vjo_anki_err_text(&A, &err), "Anki not found: test the connection in the Overlay app"));
    err.rc = VJO_OK;
    TEST_CHECK(!strcmp(vjo_anki_err_text(&A, &err), ""));

    /* a saved host is searched for again unless AnkiConnect itself answered */
    TEST_CHECK(vjo_anki_host_stale(VJO_E_NET) && vjo_anki_host_stale(VJO_E_PARSE) &&
               vjo_anki_host_stale(VJO_E_HTTP) && vjo_anki_host_stale(VJO_E_STATUS));
    TEST_CHECK(!vjo_anki_host_stale(VJO_OK) && !vjo_anki_host_stale(VJO_E_ANKI) &&
               !vjo_anki_host_stale(VJO_E_ANKI_DUPLICATE) && !vjo_anki_host_stale(VJO_E_OOM));
}

/* ---------- word audio ---------- */

#define AUDIO_LIST                                                                                            \
    "{\"type\":\"audioSourceList\",\"audioSources\":["                                                           \
    "{\"name\":\"NHK16 ネコ＼ [1]\",\"url\":\"https://raw.example.com/x/audio/1.opus\"},"                         \
    "{\"name\":\"TTS 1\",\"url\":\"https://audio.example.workers.dev/tts/a+b/c=\"}]}"

static void test_anki_audio_parts(void)
{
    const char *url;
    VjoConfig cfg;
    setup();

    vjo_config_defaults(&cfg);
    TEST_CHECK(!vjo_anki_audio_enabled(&cfg)); /* no URL by default */
    snprintf(cfg.anki_audio_url, sizeof(cfg.anki_audio_url), "https://h/?t={term}");
    TEST_CHECK(vjo_anki_audio_enabled(&cfg));

    /* the template with {term} and {reading} percent-encoded (no reading: the term) */
    TEST_CHECK(!strcmp(vjo_anki_audio_path(&A, "/audio/list?term={term}&reading={reading}&apiKey=k", "猫", "ねこ"),
                       "/audio/list?term=%E7%8C%AB&reading=%E3%81%AD%E3%81%93&apiKey=k"));
    TEST_CHECK(!strcmp(vjo_anki_audio_path(&A, "?t={term}&r={reading}&l={language}&x={other}", "a&b c", ""),
                       "/?t=a%26b%20c&r=a%26b%20c&l=ja&x={other}"));
    TEST_CHECK(!strcmp(vjo_anki_audio_path(&A, "?t={term}&r={reading}", "a", NULL), "/?t=a&r=a"));
    TEST_CHECK(!strcmp(vjo_anki_audio_path(&A, "", "a", "b"), "/"));

    /* the first http(s) URL; entries without one are skipped */
    TEST_CHECK(vjo_anki_audio_parse(&A, AUDIO_LIST, strlen(AUDIO_LIST), &url) == VJO_OK && url &&
               !strcmp(url, "https://raw.example.com/x/audio/1.opus"));
#define PARSE(json, rc) (vjo_anki_audio_parse(&A, json, strlen(json), &url) == (rc))
    TEST_CHECK(PARSE("{\"audioSources\":[{\"name\":\"x\"},\"y\",3,{\"url\":\"data:audio/mp3;base64,AA\"},"
                     "{\"url\":7},{\"url\":\"http://h/2.ogg\"}]}",
                     VJO_OK) &&
               url && !strcmp(url, "http://h/2.ogg"));
    TEST_CHECK(PARSE("{\"type\":\"audioSourceList\",\"audioSources\":[]}", VJO_OK) && !url);
    TEST_CHECK(PARSE("{\"audioSources\":[{\"url\":\"file:///etc/passwd\"}]}", VJO_OK) && !url);
    TEST_CHECK(PARSE("{\"error\":\"x\"}", VJO_E_PARSE) && !url);
    TEST_CHECK(PARSE("{\"audioSources\":{}}", VJO_E_PARSE) && PARSE("[]", VJO_E_PARSE) &&
               PARSE("<html>", VJO_E_PARSE));
#undef PARSE

    /* a known audio extension of the path, else mp3 (text-to-speech) */
#define EXT(u, e)                                                                                             \
    do {                                                                                                      \
        TEST_CHECK(!strcmp(vjo_anki_audio_ext(u), e));                                                        \
        TEST_MSG("%s -> %s", u, vjo_anki_audio_ext(u));                                                       \
    } while (0)
    EXT("https://h/x/audio/1.opus", "opus");
    EXT("https://h/a.b/1.OGG?x=y.wav#z.m4a", "ogg");
    EXT("https://audio.example.workers.dev/tts/a+b/c=", "mp3");
    EXT("https://audio.example.com", "mp3");     /* the host's dot */
    EXT("https://audio.example.com?f=a.opus", "mp3"); /* the query's */
    EXT("https://h/tts.php?t=x", "mp3");
    EXT("https://h/x.opus/1", "mp3");
    EXT("https://h/1.opus2", "mp3");
    EXT("h/1.wav", "wav");
#undef EXT
}

/* A response bigger than ok()'s buffers. */
static const char *big_response(size_t body_len)
{
    static char buf[64 * 1024];
    int n = snprintf(buf, sizeof(buf), "HTTP/1.1 200 OK\r\nContent-Length: %lu\r\n\r\n", (unsigned long)body_len);
    TEST_ASSERT(n > 0 && (size_t)n + body_len < sizeof(buf));
    memset(buf + n, ' ', body_len);
    buf[n + body_len] = '\0';
    return buf;
}

static void test_anki_find_audio(void)
{
    FakeNet f;
    VjoPlatform p;
    VjoConfig cfg;
    VjoEntryList l;
    VjoAnkiNote n;
    VjoAnkiMedia m;
    VjoErr err;
    size_t mark;
    char long_term[200 * 3 + 1];
    const uint64_t now = 1791158400007ull;
    setup();
    vjo_config_defaults(&cfg);
    anki_list(&l);
    TEST_ASSERT(vjo_anki_note_from_entry(&A, &l, 0, &n) == 0);
#define FIND() vjo_anki_find_audio(&A, &p, &cfg, &n, now, &m, &err)
#define WORKER "https://my-audio.example.workers.dev/audio/list?term={term}&reading={reading}&apiKey=k"
    snprintf(cfg.anki_audio_url, sizeof(cfg.anki_audio_url), WORKER);

    /* a GET to the URL's host (HTTPS: 443, short timeouts); only the URL
     * and its file name stay in the arena */
    fake_net(&f, &p, 1, ok(AUDIO_LIST));
    p.plain_http = 1;
    mark = vjo_arena_mark(&A);
    TEST_CHECK(FIND() == VJO_OK && m.audio_url && !strcmp(m.audio_url, "https://raw.example.com/x/audio/1.opus") &&
               !strcmp(m.audio_name, "vitajp_1791158400007.opus"));
    TEST_CHECK(vjo_arena_mark(&A) - mark < 128);
    TEST_CHECK(f.port == 443 && f.timeout_us > 0 && f.io_timeout_us > 0);
    TEST_CHECK(!strcmp(f.sent[0], "GET /audio/list?term=%E7%8C%AB&reading=%E3%81%AD%E3%81%93&apiKey=k HTTP/1.1\r\n"
                                  "Host: my-audio.example.workers.dev\r\nConnection: close\r\n\r\n"));
    TEST_MSG("%s", f.sent[0]);

    /* plain HTTP on another port: the port in Host; a chunked reply */
    snprintf(cfg.anki_audio_url, sizeof(cfg.anki_audio_url), "http://192.168.1.5:5050?term={term}");
    fake_net(&f, &p, 1,
             "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
             "10\r\n{\"audioSources\":\r\n1b\r\n[{\"url\":\"http://h/a.mp3\"}]}\r\n0\r\n\r\n");
    TEST_CHECK(FIND() == VJO_OK && m.audio_url && !strcmp(m.audio_url, "http://h/a.mp3") && f.port == 5050);
    TEST_CHECK(!strcmp(f.sent[0], "GET /?term=%E7%8C%AB HTTP/1.1\r\nHost: 192.168.1.5:5050\r\n"
                                  "Connection: close\r\n\r\n"));
    TEST_MSG("%s", f.sent[0]);

    /* no recording; then errors, with nothing left on the note or in the arena */
    snprintf(cfg.anki_audio_url, sizeof(cfg.anki_audio_url), WORKER);
    fake_net(&f, &p, 4, ok("{\"audioSources\":[]}"), "HTTP/1.1 401 No\r\nContent-Length: 0\r\n\r\n",
             ok("<html>"), NULL);
    p.plain_http = 1;
    mark = vjo_arena_mark(&A);
    TEST_CHECK(FIND() == VJO_OK && !m.audio_url && !m.audio_name);
    TEST_CHECK(!strcmp(vjo_anki_audio_err_text(&A, &err), "no recording"));
    TEST_CHECK(FIND() == VJO_E_STATUS && !m.audio_url && !strcmp(vjo_anki_audio_err_text(&A, &err), "HTTP 401"));
    TEST_CHECK(FIND() == VJO_E_PARSE && !m.audio_url &&
               !strcmp(vjo_anki_audio_err_text(&A, &err), "not an audio source list"));
    TEST_CHECK(FIND() == VJO_E_NET && !m.audio_url &&
               !strcmp(vjo_anki_audio_err_text(&A, &err), "audio source offline"));
    vjo_arena_release(&A, mark); /* (the error texts) */

    /* a reply over the limit; a term too long for the request line */
    fake_net(&f, &p, 1, big_response(VJO_ANKI_AUDIO_MAX_RESPONSE + 1));
    p.plain_http = 1;
    TEST_CHECK(FIND() == VJO_E_TOO_LARGE && !m.audio_url &&
               !strcmp(vjo_anki_audio_err_text(&A, &err), "reply too large"));
    for (int i = 0; i < 200; i++)
        memcpy(long_term + i * 3, "猫", 3);
    long_term[200 * 3] = '\0';
    n.spelling = long_term;
    fake_net(&f, &p, 1, ok(AUDIO_LIST));
    p.plain_http = 1;
    TEST_CHECK(FIND() == VJO_E_HTTP && !m.audio_url);

    /* an invalid URL (validated at load, so only by hand) */
    snprintf(cfg.anki_audio_url, sizeof(cfg.anki_audio_url), "ftp://x/{term}");
    TEST_CHECK(FIND() == VJO_E_PARSE && !m.audio_url);
#undef WORKER
#undef FIND
}

/* ---------- settings ---------- */

static void test_anki_config(void)
{
    VjoConfig c;
    char host[64];
    int port = 0;
    /* ';' and '#' kept in the audio URL, inline comments allowed after
     * anki_host; the field mapping's settings, which a fixed Kiku note
     * replaced, are ignored without a warning (in any case) */
    const char *ini = "anki_host = auto ; LAN\n"
                      "anki_deck = Mining #1 ; main\n"
                      "anki_note_type = Lapis;v2\n"
                      "anki_tags = a b\n"
                      "anki_field_picture =\n"
                      "anki_field_audio =\n"
                      "anki_audio_url = https://w.dev/audio/list?term={term}&reading={reading}&apiKey=a;b\n"
                      "Anki_Field_Word = Word\n"
                      "ANKI_DECK = x\n";
    vjo_config_defaults(&c);
    TEST_CHECK(!c.anki_host[0] && !c.anki_audio_url[0] && !c.anki_mining);
    vjo_config_parse(&c, ini, strlen(ini));
    TEST_CHECK(c.n_warnings == 0);
    TEST_MSG("%d: %s", c.n_warnings, c.warnings[0]);
    TEST_CHECK(!strcmp(c.anki_host, "auto"));
    TEST_CHECK(!strcmp(c.anki_audio_url, "https://w.dev/audio/list?term={term}&reading={reading}&apiKey=a;b"));
    TEST_MSG("%s", c.anki_audio_url);
    /* only those names: a near miss is still an unknown setting */
    ini = "anki_decks = x\nanki_field = x\nanki_tag = x\n";
    vjo_config_defaults(&c);
    vjo_config_parse(&c, ini, strlen(ini));
    TEST_CHECK(c.n_warnings == 3 && strstr(c.warnings[1], "unknown setting 'anki_field'"));
    TEST_MSG("%s", c.warnings[1]);

    ini = "anki_host = 192.168.1.5:99999\nanki_deck =\nanki_host2 = x\n";
    vjo_config_defaults(&c);
    vjo_config_parse(&c, ini, strlen(ini));
    TEST_CHECK(c.n_warnings == 2 && !c.anki_host[0]);
    TEST_CHECK(!strcmp(c.warnings[0], "anki_host: invalid value '192.168.1.5:99999' (empty, auto, or IP[:port])"));
    TEST_MSG("%s", c.warnings[0]);
    /* anki_audio_url: one warning for each invalid value, and no audio */
    {
        static const char *const bad[] = {
            "ftp://x/{term}", "https://", "https:///?t={term}", "https://h:0/{term}", "https://h/{term} b",
            "https://h/{term}#x", "https://h/?t=%s", "https://h/{term}\x7f", "https://h/{term}\xe7\x8c\xab",
            "https://h_h/{term}", "http//h/{term}",
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            char line[256];
            snprintf(line, sizeof(line), "anki_audio_url = %s\n", bad[i]);
            vjo_config_defaults(&c);
            vjo_config_parse(&c, line, strlen(line));
            TEST_CHECK(c.n_warnings == 1 && !c.anki_audio_url[0]);
            TEST_MSG("%s", bad[i]);
        }
        TEST_CHECK(strstr(c.warnings[0], "anki_audio_url: invalid value") == c.warnings[0]);
        TEST_MSG("%s", c.warnings[0]);
    }
    /* a host of up to 127 bytes, a value of up to 255 */
    {
        char line[600], fill[300];
        memset(fill, 'h', sizeof(fill));
        fill[127] = '\0';
        snprintf(line, sizeof(line), "anki_audio_url = https://%s/?t={term}\n", fill);
        vjo_config_defaults(&c);
        vjo_config_parse(&c, line, strlen(line));
        TEST_CHECK(c.n_warnings == 0 && strlen(c.anki_audio_url) == strlen("https:///?t={term}") + 127);
        fill[127] = 'h';
        fill[128] = '\0';
        snprintf(line, sizeof(line), "anki_audio_url = https://%s/?t={term}\n", fill);
        vjo_config_defaults(&c);
        vjo_config_parse(&c, line, strlen(line));
        TEST_CHECK(c.n_warnings == 1 && !c.anki_audio_url[0]);

        memset(fill, 'k', sizeof(fill));
        fill[255 - strlen("https://h/?t={term}&k=")] = '\0';
        snprintf(line, sizeof(line), "anki_audio_url = https://h/?t={term}&k=%s\n", fill);
        vjo_config_defaults(&c);
        vjo_config_parse(&c, line, strlen(line));
        TEST_CHECK(c.n_warnings == 0 && strlen(c.anki_audio_url) == 255);
        /* longer: a warning, not a URL cut short (and so a wrong API key) */
        snprintf(line, sizeof(line), "anki_audio_url = https://h/?t={term}&k=%sk\n", fill);
        vjo_config_defaults(&c);
        vjo_config_parse(&c, line, strlen(line));
        TEST_CHECK(c.n_warnings == 1 && !c.anki_audio_url[0] && strstr(c.warnings[0], "value too long"));
        /* a long inline comment after a short value: only the comment is cut */
        {
            static char comment[2000];
            memset(comment, 'x', sizeof(comment));
            memcpy(comment, "anki_host = auto ; ", 19);
            comment[sizeof(comment) - 2] = '\n';
            comment[sizeof(comment) - 1] = '\0';
            vjo_config_defaults(&c);
            vjo_config_parse(&c, comment, strlen(comment));
            TEST_CHECK(c.n_warnings == 0 && !strcmp(c.anki_host, "auto"));
        }
        /* a line longer than the line buffer: the same */
        {
            static char longline[2000];
            memset(longline, 'k', sizeof(longline));
            memcpy(longline, "anki_audio_url = https://h/?t={term}&k=", 39);
            longline[sizeof(longline) - 2] = '\n';
            longline[sizeof(longline) - 1] = '\0';
            vjo_config_defaults(&c);
            vjo_config_parse(&c, longline, strlen(longline));
            TEST_CHECK(c.n_warnings == 1 && !c.anki_audio_url[0] && strstr(c.warnings[0], "value too long"));
        }
    }
    ini = "anki_host = 192.168.1.5:8766\n";
    vjo_config_defaults(&c);
    vjo_config_parse(&c, ini, strlen(ini));
    TEST_CHECK(c.n_warnings == 0 && !strcmp(c.anki_host, "192.168.1.5:8766"));

    TEST_CHECK(vjo_anki_endpoint("", host, sizeof(host), &port) == VJO_ANKI_OFF);
    TEST_CHECK(vjo_anki_endpoint("Auto", host, sizeof(host), &port) == VJO_ANKI_AUTO);
    TEST_CHECK(vjo_anki_endpoint("192.168.1.23", host, sizeof(host), &port) == VJO_ANKI_MANUAL &&
               !strcmp(host, "192.168.1.23") && port == 8765);
    TEST_CHECK(vjo_anki_endpoint("my-pc.local:9000", host, sizeof(host), &port) == VJO_ANKI_MANUAL &&
               !strcmp(host, "my-pc.local") && port == 9000);
    TEST_CHECK(vjo_anki_endpoint("1.2.3.4:", host, sizeof(host), &port) < 0);
    TEST_CHECK(vjo_anki_endpoint("1.2.3.4:70000", host, sizeof(host), &port) < 0);
    TEST_CHECK(vjo_anki_endpoint("1.2.3.4:0", host, sizeof(host), &port) < 0);
    TEST_CHECK(vjo_anki_endpoint("http://x", host, sizeof(host), &port) < 0);
    TEST_CHECK(vjo_anki_endpoint(":8765", host, sizeof(host), &port) < 0);
    TEST_CHECK(vjo_anki_endpoint("a b", host, sizeof(host), &port) < 0);
}

TEST_LIST = {
    {"base64", test_base64},
    {"anki_fields", test_anki_fields},
    {"anki_requests", test_anki_requests},
    {"anki_pitch", test_anki_pitch},
    {"anki_local_card", test_anki_local_card},
    {"anki_request_bytes_do_not_depend_on_the_arena", test_anki_request_bytes_do_not_depend_on_the_arena},
    {"anki_probe", test_anki_probe},
    {"anki_check", test_anki_check},
    {"anki_add", test_anki_add},
    {"anki_audio_parts", test_anki_audio_parts},
    {"anki_find_audio", test_anki_find_audio},
    {"anki_config", test_anki_config},
    {NULL, NULL},
};
