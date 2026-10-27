/* Sentence readings (#311, core/sentence_reading.h): the readings the Rubi app writes over a card's sentence.
 *
 * Without a dictionary: a word's reading placed on its text (vjo_word_rubies: inflections, 来る, 良い, 何, 々 and
 * ヶ, a reading that does not fit), the field's HTML (vjo_furigana_html), the names' romanized readings. With the
 * bundled dictionary (VJO_PLAINTEXT_DIR, as host/tests/test_word_lookup.cpp has it; skipped without it unless
 * VJO_REQUIRE_PLAINTEXT is set): golden sentences (each reading checked by hand; a word whose top entries
 * disagree gets none), a card's field with the mined word's own reading in <b> over a line the game wrapped, the
 * longest sentence in its scratch, and a dictionary that cannot be read. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "acutest.h"
#include "local_dict.h"
#include "platform_posix.h"
#include "sentence_reading.h"

static unsigned char mem[VJO_READINGS_SCRATCH + 64 * 1024] __attribute__((aligned(16)));
static unsigned char out_mem[256 * 1024] __attribute__((aligned(16)));
static uint64_t session_mem[8 * 1024 / 8];

/* rubies as "base[reading] ..." of text */
static const char *show(const char *text, const VjoRuby *r, int n)
{
    static char buf[2048];
    size_t used = 0;
    buf[0] = '\0';
    for (int i = 0; i < n; i++)
        used += (size_t)snprintf(buf + used, sizeof(buf) - used, "%s%.*s[%.*s]", i ? " " : "",
                                 (int)(r[i].end - r[i].start), text + r[i].start, (int)r[i].reading_len, r[i].reading);
    return buf;
}

static void word_readings_are_placed_on_the_text(void)
{
    static const struct {
        const char *surface, *after, *spelling, *reading, *want;
    } cases[] = {
        {"食べました", "", "食べる", "たべる", "食[た]"},
        {"行って", "", "行く", "いく", "行[い]"},
        {"行って", "", "行く", "ゆく", "行[い]"},        /* 行く read ゆく: its past and て forms are いっ */
        {"行く", "", "行く", "ゆく", "行[ゆ]"},
        {"読まなかった", "", "読む", "よむ", "読[よ]"},
        {"来ない", "", "来る", "くる", "来[こ]"},        /* 来る as conjugated */
        {"来ます", "", "来る", "くる", "来[き]"},
        {"来た", "", "来る", "くる", "来[き]"},
        {"来なさい", "", "来る", "くる", "来[き]"},
        {"来れば", "", "来る", "くる", "来[く]"},
        {"来い", "", "来る", "くる", "来[こ]"},
        {"来る", "", "来る", "くる", "来[く]"},
        {"良かった", "", "良い", "よい", "良[よ]"},
        {"良かった", "", "良い", "いい", "良[よ]"},      /* いい's past is よかった */
        {"勉強", "した", "勉強", "べんきょう", "勉強[べんきょう]"},
        {"時々", "", "時々", "ときどき", "時々[ときどき]"}, /* 々 and ヶ are kanji here */
        {"一ヶ月", "", "一ヶ月", "いっかげつ", "一ヶ月[いっかげつ]"},
        {"物の怪", "", "物の怪", "もののけ", "物の怪[もののけ]"}, /* two ways to split it: read whole */
        {"何", "でだよ", "何", "なに", "何[なん]"},      /* 何 before で, だ, の...: なん */
        {"何", "を", "何", "なに", "何[なに]"},
        {"何", "も", "何", "なに", "何[なに]"},
        {"ゲーム", "", "ゲーム", "げーむ", ""},          /* no kanji, no reading */
        {"お茶", "", "お茶", "おちゃ", "茶[ちゃ]"},
        {"見せる", "", "見せる", "ミセル", "見[ミ]"},    /* a reading in katakana, compared as hiragana */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        VjoRuby r[8];
        int n = vjo_word_rubies(cases[i].surface, strlen(cases[i].surface), cases[i].after, strlen(cases[i].after),
                                cases[i].spelling, cases[i].reading, r, 8);
        TEST_CHECK_(!strcmp(show(cases[i].surface, r, n), cases[i].want), "%s (%s %s): '%s', want '%s'",
                    cases[i].surface, cases[i].spelling, cases[i].reading, show(cases[i].surface, r, n), cases[i].want);
    }
    /* none: a reading that does not fit the text, a name's (no kana), 何 before a counter, a word too long */
    {
        static const struct {
            const char *surface, *after, *spelling, *reading;
        } none[] = {
            {"食べる", "", "食べる", "たべり"},
            {"朋也", "", "朋也", "朋也"},
            {"何", "人", "何", "なに"},
            {"上手", "", "上手", "じょうず?"},
        };
        for (unsigned i = 0; i < sizeof(none) / sizeof(none[0]); i++) {
            VjoRuby r[8];
            TEST_CHECK_(vjo_word_rubies(none[i].surface, strlen(none[i].surface), none[i].after, strlen(none[i].after),
                                        none[i].spelling, none[i].reading, r, 8) == 0,
                        "%s: a reading", none[i].surface);
        }
    }
}

static void the_field_is_ankis_furigana_syntax(void)
{
    /* readings on 学校 and 友達; the word in <b>; a bracket of the text's own is no reading */
    static const char text[] = "今日は学校へ[行]って\n<友達>&";
    VjoRuby r[2] = {{9, 15, "がっこう", 12}, {31, 37, "ともだち", 12}};
    VjoBuf b;
    VjoArena a;
    vjo_arena_init(&a, out_mem, sizeof(out_mem));
    vjo_buf_init(&b, &a);
    vjo_furigana_html(&b, text, strlen(text), r, 2, 9, 15);
    TEST_CHECK_(!strcmp(vjo_buf_cstr(&b), "今日は<b>学校[がっこう]</b>へ&#91;行&#93;って<br>&lt; 友達[ともだち]&gt;&amp;"),
                "%s", vjo_buf_cstr(&b));
    /* a reading over the highlight's edge is left out (the <b> stays whole), and none over a line break */
    {
        VjoRuby over[2] = {{6, 15, "はがっこう", 15}, {0, 9, "きょうは", 12}};
        vjo_buf_init(&b, &a);
        vjo_furigana_html(&b, text, 15, over + 1, 1, 9, 15);
        TEST_CHECK_(!strcmp(vjo_buf_cstr(&b), "今日は[きょうは]<b>学校</b>"), "%s", vjo_buf_cstr(&b));
        vjo_buf_init(&b, &a);
        vjo_furigana_html(&b, text, 15, over, 1, 9, 15);
        TEST_CHECK_(!strcmp(vjo_buf_cstr(&b), "今日は<b>学校</b>"), "%s", vjo_buf_cstr(&b));
    }
}

#ifdef VJO_HOST
/* VNDB's romanized names against a word's reading (the names rule). */
static void names_are_read_from_their_romaji(void)
{
    static const struct {
        const char *romaji, *kana;
    } cases[] = {
        {"Komine", "こみね"},   {"Sachi", "さち"},     {"Ootani", "おおたに"}, {"Ryuudou", "りゅうどう"},
        {"Isshu", "いっしゅ"},  {"Kazuki", "かずき"},  {"Shin'ichi", "しんいち"}, {"Kanade", "かなで"},
        {"sensei", "せんせい"}, {"Matchi", "まっち"},  {"Kyouko", "きょうこ"},
    };
    char out[64];
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        TEST_CHECK_(vjo_reading_romaji_kana(cases[i].romaji, strlen(cases[i].romaji), out, sizeof(out)) > 0 &&
                        !strcmp(out, cases[i].kana),
                    "%s: %s", cases[i].romaji, out);
    TEST_CHECK(vjo_reading_romaji_kana("Alfred", 6, out, sizeof(out)) < 0); /* l: no Japanese reading */
    /* 一樹 as a name is Kazuki: the word's いちじゅ disagrees; 幸 is Sachi, as the word さち */
    TEST_CHECK(vjo_reading_name_disagrees("Haizawa Kazuki (灰沢 一樹)\nGame [PSP]\nhttps://vndb.org/c1", "一樹", 6,
                                          "いちじゅ"));
    TEST_CHECK(!vjo_reading_name_disagrees("Komine Sachi (小嶺 幸)\nGame [PS Vita]\nhttps://vndb.org/c576", "幸", 3,
                                           "さち"));
    /* an alias (no part written so) says nothing; one character of several reading so is enough */
    TEST_CHECK(!vjo_reading_name_disagrees("Fujimi Ryuudou (富士見 流動)\nGame\nurl", "会長", 6, "かいちょう"));
    TEST_CHECK(!vjo_reading_name_disagrees("Haji-sensei (土師 先生)\nGame\nurl", "先生", 6, "せんせい"));
    TEST_CHECK(!vjo_reading_name_disagrees("A Kazuki (甲 一樹)\nG\nu\nB Ichiju (乙 一樹)\nG\nu", "一樹", 6, "いちじゅ"));
}
#endif

/* ---------------- with the bundled dictionary ---------------- */

static const char *dict_dir;

static int setup(VjoPlatform *p, VjoConfig *cfg, PosixPlatform *pp)
{
    char path[1024];
    FILE *f;
    if (!(dict_dir = getenv("VJO_PLAINTEXT_DIR")))
        return 0;
    snprintf(path, sizeof(path), "%s/plaintext.vjdict", dict_dir);
    if (!(f = fopen(path, "rb")))
        return 0;
    fclose(f);
    vjo_config_defaults(cfg);
    snprintf(cfg->local_dictionary_dir, sizeof(cfg->local_dictionary_dir), "%s", dict_dir);
    snprintf(cfg->local_dictionaries, sizeof(cfg->local_dictionaries), "plaintext.vjdict");
    posix_platform_init(pp, p);
    memset(session_mem, 0, sizeof(session_mem));
    p->local_session = (VjoLocalSession *)session_mem;
    return 1;
}

static int no_dictionary(void)
{
    if (getenv("VJO_REQUIRE_PLAINTEXT")) {
        TEST_CHECK_(0, "plaintext.vjdict required (VJO_PLAINTEXT_DIR)");
        return 1;
    }
    TEST_MSG("no plaintext.vjdict (VJO_PLAINTEXT_DIR): skipped");
    return 1;
}

/* Each sentence's readings, every one of them checked by hand; a word whose readings depend on its context
 * gets none (今日: きょう, こんにち; 明日: あした, あす; 三人: さんにん, みたり; 方 alone: ほう, かた). */
static void golden_sentences(void)
{
    static const struct {
        const char *text, *want;
    } cases[] = {
        {"パンを食べました。", "パンを 食[た]べました。"},
        {"学校へ行って、本を読まなかった。", "学校[がっこう]へ 行[い]って、 本[ほん]を 読[よ]まなかった。"},
        {"明日は来ないの？", "明日は 来[こ]ないの？"},
        {"また来ます。", "また 来[き]ます。"},
        {"早く来なさい！", "早[はや]く 来[き]なさい！"},
        {"昨日は勉強した。", "昨日は 勉強[べんきょう]した。"},
        {"時々、人々が集まる。", "時々[ときどき]、 人々[ひとびと]が 集[あつ]まる。"},
        {"一ヶ月前に始めた。", "一ヶ月[いっかげつ]前に 始[はじ]めた。"},
        {"三人で食べた。", "三人で 食[た]べた。"},
        {"５分だけ待って。", "５分[ごふん]だけ 待[ま]って。"},
        {"ゲームをしたい。", "ゲームをしたい。"},
        {"今日は晴れ。", "今日は 晴[は]れ。"},
        {"何でだよ！", "何[なん]でだよ！"},
        {"何を言ってるの？", "何[なに]を言ってるの？"},
        {"彼女は本当に優しい。", "彼女[かのじょ]は 本当[ほんとう]に 優[やさ]しい。"},
        {"大丈夫ですか？", "大丈夫[だいじょうぶ]ですか？"},
        {"先生、おはようございます。", "先生[せんせい]、おはようございます。"},
    };
    VjoPlatform p;
    VjoConfig cfg;
    PosixPlatform pp = {0};
    if (!setup(&p, &cfg, &pp) && no_dictionary())
        return;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        VjoArena keep, scratch, a;
        VjoRuby r[64];
        VjoReadingStats st;
        VjoBuf b;
        int n;
        memset(&st, 0, sizeof(st));
        vjo_arena_init(&keep, out_mem, sizeof(out_mem) / 2);
        vjo_arena_init(&scratch, mem, sizeof(mem));
        n = vjo_sentence_rubies(&keep, &scratch, &p, &cfg, NULL, cases[i].text, 0, strlen(cases[i].text), r, 64, &st);
        TEST_ASSERT(n >= 0);
        vjo_arena_init(&a, out_mem + sizeof(out_mem) / 2, sizeof(out_mem) / 2);
        vjo_buf_init(&b, &a);
        vjo_furigana_html(&b, cases[i].text, strlen(cases[i].text), r, n, -1, -1);
        TEST_CHECK_(!strcmp(vjo_buf_cstr(&b), cases[i].want), "'%s'\n  got  '%s'\n  want '%s'", cases[i].text,
                    vjo_buf_cstr(&b), cases[i].want);
        TEST_CHECK(scratch.used == 0 && st.failed == 0); /* the scratch is all given back */
    }
    vjo_local_session_close(p.local_session);
}

/* A card's SentenceFurigana: its sentence joined as its Sentence (the line the game wrapped inside 一緒 is one
 * line), the mined word in <b> with the card's own reading, the others from the dictionary. */
static void a_cards_field_over_a_wrapped_line(void)
{
    VjoPlatform p;
    VjoConfig cfg;
    PosixPlatform pp = {0};
    VjoAnkiNote note;
    VjoArena a, scratch;
    VjoReadingStats st;
    char *html;
    int rc;
    /* the first line as wide as the widest, not ending a sentence: the break joins (core/textjoin.h) */
    static const char sentence[] = "わたしたちはいつまでも一\n緒に勉強していたかった。";
    if (!setup(&p, &cfg, &pp) && no_dictionary())
        return;
    memset(&note, 0, sizeof(note));
    memset(&st, 0, sizeof(st));
    note.spelling = "勉強";
    note.reading = "べんきょう";
    note.sentence = sentence;
    note.hl_start = (int)(strstr(sentence, "勉強") - sentence);
    note.hl_end = note.hl_start + 6;
    note.sf_hl_start = note.sf_hl_end = -1;
    vjo_arena_init(&a, out_mem, sizeof(out_mem));
    vjo_arena_init(&scratch, mem, sizeof(mem));
    html = vjo_sentence_furigana(&a, &scratch, &p, &cfg, NULL, &note, &st, &rc);
    TEST_ASSERT(html != NULL);
    TEST_CHECK_(!strcmp(html, "わたしたちはいつまでも 一緒[いっしょ]に<b>勉強[べんきょう]</b>していたかった。"), "%s", html);
    TEST_CHECK(st.lookups > 0 && st.read >= 1 && scratch.used == 0);
    /* a sentence without kanji: "" (Lapis then shows the Sentence) */
    note.sentence = "ねえ、どうしたの？";
    note.hl_start = 0;
    note.hl_end = 3;
    note.spelling = note.reading = "ねえ";
    html = vjo_sentence_furigana(&a, &scratch, &p, &cfg, NULL, &note, &st, &rc);
    TEST_CHECK(html && !strcmp(html, "") && rc == 0);
    vjo_local_session_close(p.local_session);
}

/* The longest sentence a record holds (VJO_LOCAL_MAX_TEXT bytes of kanji words and kana) in exactly
 * VJO_READINGS_SCRATCH of scratch: no VJO_E_OOM, the scratch given back. */
static void a_longest_sentence_fits_its_scratch(void)
{
    static char text[VJO_LOCAL_MAX_TEXT + 1];
    static const char *const words[] = {"学校へ行って", "友達と", "一緒に", "勉強した", "時々", "本を読む"};
    VjoPlatform p;
    VjoConfig cfg;
    PosixPlatform pp = {0};
    VjoAnkiNote note;
    VjoArena a, scratch;
    VjoReadingStats st;
    size_t used = 0;
    char *html;
    int rc;
    if (!setup(&p, &cfg, &pp) && no_dictionary())
        return;
    for (int k = 0; used + 24 < VJO_LOCAL_MAX_TEXT; k++)
        used += (size_t)snprintf(text + used, sizeof(text) - used, "%s", words[k % 6]);
    memset(&note, 0, sizeof(note));
    memset(&st, 0, sizeof(st));
    note.spelling = "学校";
    note.reading = "がっこう";
    note.sentence = text;
    note.hl_start = 0;
    note.hl_end = 6;
    note.sf_hl_start = note.sf_hl_end = -1;
    vjo_arena_init(&a, out_mem, sizeof(out_mem));
    vjo_arena_init(&scratch, mem, VJO_READINGS_SCRATCH);
    html = vjo_sentence_furigana(&a, &scratch, &p, &cfg, NULL, &note, &st, &rc);
    TEST_CHECK_(html != NULL && rc == 0, "rc %d, peak %zu of %u", rc, scratch.peak, (unsigned)VJO_READINGS_SCRATCH);
    TEST_CHECK(scratch.used == 0 && scratch.peak <= VJO_READINGS_SCRATCH && st.read > 100);
    TEST_MSG("%zu bytes, %u words read, scratch peak %zu", used, st.read, scratch.peak);
    vjo_local_session_close(p.local_session);
}

/* Dictionaries that cannot be read: no field (NULL, VJO_E_LOCAL_IO): the export keeps the record's. */
static void a_dictionary_that_cannot_be_read(void)
{
    VjoPlatform p;
    VjoConfig cfg;
    PosixPlatform pp = {0};
    VjoAnkiNote note;
    VjoArena a, scratch;
    VjoReadingStats st;
    int rc = 0;
    vjo_config_defaults(&cfg);
    snprintf(cfg.local_dictionary_dir, sizeof(cfg.local_dictionary_dir), "no/such/directory");
    snprintf(cfg.local_dictionaries, sizeof(cfg.local_dictionaries), "plaintext.vjdict");
    posix_platform_init(&pp, &p);
    memset(&note, 0, sizeof(note));
    memset(&st, 0, sizeof(st));
    note.spelling = "学校";
    note.reading = "がっこう";
    note.sentence = "学校へ行った。";
    note.hl_start = 0;
    note.hl_end = 6;
    vjo_arena_init(&a, out_mem, sizeof(out_mem));
    vjo_arena_init(&scratch, mem, sizeof(mem));
    TEST_CHECK(vjo_sentence_furigana(&a, &scratch, &p, &cfg, NULL, &note, &st, &rc) == NULL);
    TEST_CHECK(rc == VJO_E_LOCAL_IO && st.failed == 1 && scratch.used == 0);
}

TEST_LIST = {
    {"word_readings_are_placed_on_the_text", word_readings_are_placed_on_the_text},
    {"the_field_is_ankis_furigana_syntax", the_field_is_ankis_furigana_syntax},
#ifdef VJO_HOST
    {"names_are_read_from_their_romaji", names_are_read_from_their_romaji},
#endif
    {"golden_sentences", golden_sentences},
    {"a_cards_field_over_a_wrapped_line", a_cards_field_over_a_wrapped_line},
    {"a_longest_sentence_fits_its_scratch", a_longest_sentence_fits_its_scratch},
    {"a_dictionary_that_cannot_be_read", a_dictionary_that_cannot_be_read},
    {NULL, NULL},
};
