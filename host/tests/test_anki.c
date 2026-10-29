/* Anki cards: the HTML of the Lapis fields a card fills (core/anki.c), a card
 * made from the shown lookup (vjo_anki_note_from_entry), and the removed
 * anki_* settings. The export of whole notes is host/tests/test_apkg.c. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "acutest.h"
#include "anki.h"
#include "config.h"
#include "word_key.h"

static uint8_t g_mem[4u << 20];
static VjoArena A;

static void setup(void)
{
    /* Garbage-fill like a reused arena on the Vita. */
    memset(g_mem, 0xA5, sizeof(g_mem));
    vjo_arena_init(&A, g_mem, sizeof(g_mem));
}

static void test_anki_sentence_and_furigana(void)
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

    /* Yomitan's {furigana-plain}, which Lapis's README asks for: each kanji run
     * takes its part of the reading; a space before a base that follows other
     * text, so Anki's furigana filter reads only that run: お茶[おちゃ] would put
     * おちゃ over お茶 */
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
            {"一ヶ月", "いっかげつ", " 一ヶ月[いっかげつ]"},             /* ヶ: kana to a ruby parser */
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
}

/* MainDefinition and Glossary as Yomitan writes them, which Lapis pages by
 * dictionary (data-dictionary), hiding in Glossary those MainDefinition has. */
static void test_anki_glossaries(void)
{
    static const char *const daijirin[] = {"[Daijirin4 Plain]\nA room; a <chamber>.\nAlso a stable."};
    static const char *const jmdict[] = {"[JMdict Plain]\nroom", "chamber"};
    static const char *const jitendex[] = {"room \"heya\"", "chamber"};
    VjoAnkiGloss more[2] = {{NULL, "部屋", "へや", (const char **)jmdict, 2},
                            {"Jitendex.org", "部屋", "へや", (const char **)jitendex, 2}};
    VjoAnkiNote n = {.spelling = "部屋", .reading = "へや", .meanings = (const char **)daijirin, .n_meanings = 1,
                     .sentence = "", .rank = VJO_NO_RANK};
    setup();
    TEST_CHECK(!strcmp(vjo_anki_field_value(&A, VJO_LAPIS_MAIN_DEFINITION, &n),
                       "<div style=\"text-align: left;\" class=\"yomitan-glossary\"><ol>"
                       "<li data-dictionary=\"Daijirin4 Plain\"><i>(Daijirin4 Plain)</i> <span>A room; a "
                       "&lt;chamber&gt;.<br>Also a stable.</span></li></ol></div>"));
    TEST_MSG("%s", vjo_anki_field_value(&A, VJO_LAPIS_MAIN_DEFINITION, &n));
    TEST_CHECK(!strcmp(vjo_anki_field_value(&A, VJO_LAPIS_GLOSSARY, &n), "")); /* one dictionary: none */
    n.more = more;
    n.n_more = 2;
    TEST_CHECK(!strcmp(vjo_anki_field_value(&A, VJO_LAPIS_GLOSSARY, &n),
                       "<div style=\"text-align: left;\" class=\"yomitan-glossary\"><ol>"
                       "<li data-dictionary=\"Daijirin4 Plain\"><i>(Daijirin4 Plain)</i> <span>A room; a "
                       "&lt;chamber&gt;.<br>Also a stable.</span></li>"
                       "<li data-dictionary=\"JMdict Plain\"><i>(JMdict Plain)</i> <span><ul><li>room</li>"
                       "<li>chamber</li></ul></span></li>"
                       "<li data-dictionary=\"Jitendex.org\"><i>(Jitendex.org)</i> <span><ul><li>room &quot;heya&quot;"
                       "</li><li>chamber</li></ul></span></li></ol></div>"));
    TEST_MSG("%s", vjo_anki_field_value(&A, VJO_LAPIS_GLOSSARY, &n));
    /* a name of the record's own wins; none at all: no label */
    n.dictionary = "Daijirin <4>";
    TEST_CHECK(strstr(vjo_anki_field_value(&A, VJO_LAPIS_MAIN_DEFINITION, &n),
                      "<li data-dictionary=\"Daijirin &lt;4&gt;\"><i>(Daijirin &lt;4&gt;)</i> <span>A room;") != NULL);
    n.meanings = (const char **)jitendex;
    n.n_meanings = 2;
    n.dictionary = NULL;
    TEST_CHECK(!strcmp(vjo_anki_field_value(&A, VJO_LAPIS_MAIN_DEFINITION, &n),
                       "<div style=\"text-align: left;\" class=\"yomitan-glossary\"><ol><li><span><ul><li>"
                       "room &quot;heya&quot;</li><li>chamber</li></ul></span></li></ol></div>"));
    n.n_meanings = 0;
    TEST_CHECK(!strcmp(vjo_anki_field_value(&A, VJO_LAPIS_MAIN_DEFINITION, &n), ""));
    /* the dictionary's ruby in Anki's syntax (a queued record of an earlier
     * build's online lookup has one), else the whole word's reading */
    n.spelling = "取り扱い";
    n.reading = "とりあつかい";
    TEST_CHECK(!strcmp(vjo_anki_field_value(&A, VJO_LAPIS_EXPRESSION_FURIGANA, &n), "取[と]り 扱[あつか]い"));
    n.furigana = "取[と]り扱[あつか]い";
    TEST_CHECK(!strcmp(vjo_anki_field_value(&A, VJO_LAPIS_EXPRESSION_FURIGANA, &n), "取[と]り 扱[あつか]い"));
    TEST_MSG("%s", vjo_anki_field_value(&A, VJO_LAPIS_EXPRESSION_FURIGANA, &n));
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

/* Every Lapis field of a full card, as Yomitan fills it for the fields Lapis's
 * README lists; the ones a card has nothing for stay empty. */
static void test_anki_lapis_fields(void)
{
    static const char *const neko[] = {"[JMdict]\ncat \"neko\"", "a\\b <&>\nc"}, *const noun[] = {"n"};
    VjoAnkiNote n = {.spelling = "猫", .reading = "ねこ", .rank = 1500, .meanings = (const char **)neko,
                     .n_meanings = 2, .sentence = "猫が\n好き", .hl_start = 0, .hl_end = 3,
                     .sentence_furigana = "猫[ねこ]が\n 好[す]き", .sf_hl_start = 0, .sf_hl_end = 11,
                     .source = "BCCWJ", .pitch = {1}, .n_pitch = 1, .pos = (const char **)noun, .n_pos = 1,
                     .misc = "Persona 4 <Golden> (2026-10-11 09:05)"};
    static const struct {
        int field;
        const char *want;
    } want[] = {
        {VJO_LAPIS_EXPRESSION, "猫"},
        {VJO_LAPIS_EXPRESSION_FURIGANA, "猫[ねこ]"},
        {VJO_LAPIS_EXPRESSION_READING, "ねこ"},
        {VJO_LAPIS_EXPRESSION_AUDIO, ""},
        {VJO_LAPIS_SELECTION_TEXT, ""},
        {VJO_LAPIS_MAIN_DEFINITION, "<div style=\"text-align: left;\" class=\"yomitan-glossary\"><ol>"
                                    "<li data-dictionary=\"JMdict\"><i>(JMdict)</i> <span><ul><li>cat &quot;neko&quot;"
                                    "</li><li>a\\b &lt;&amp;&gt;<br>c</li></ul></span></li></ol></div>"},
        {VJO_LAPIS_DEFINITION_PICTURE, ""},
        {VJO_LAPIS_SENTENCE, "<b>猫</b>が<br>好き"},
        {VJO_LAPIS_SENTENCE_FURIGANA, "<b>猫[ねこ]</b>が<br> 好[す]き"},
        {VJO_LAPIS_SENTENCE_AUDIO, ""},
        {VJO_LAPIS_PICTURE, ""}, /* core/lapis.c names the card's screenshot */
        {VJO_LAPIS_GLOSSARY, ""},
        {VJO_LAPIS_HINT, ""},
        {VJO_LAPIS_IS_WORD_AND_SENTENCE_CARD, ""},
        {VJO_LAPIS_IS_CLICK_CARD, ""},
        {VJO_LAPIS_IS_SENTENCE_CARD, ""},
        {VJO_LAPIS_IS_AUDIO_CARD, ""},
        {VJO_LAPIS_PITCH_POSITION, "<span style=\"display:inline;\"><span>[</span><span>1</span><span>]</span></span>"},
        {VJO_LAPIS_PITCH_CATEGORIES, "atamadaka"},
        {VJO_LAPIS_FREQUENCY, "<ul style=\"text-align: left;\"><li>BCCWJ: 1500</li></ul>"},
        {VJO_LAPIS_FREQ_SORT, "1500"},
        {VJO_LAPIS_MISC_INFO, "Persona 4 &lt;Golden&gt; (2026-10-11 09:05)"},
    };
    setup();
    TEST_CHECK(sizeof(want) / sizeof(want[0]) == VJO_LAPIS_FIELDS);
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        const char *v = vjo_anki_field_value(&A, want[i].field, &n);
        TEST_CHECK(v && !strcmp(v, want[i].want));
        TEST_MSG("field %d: '%s', want '%s'", want[i].field, v, want[i].want);
    }
    /* no frequency: FreqSort 9999999, as Yomitan writes {frequency-harmonic-rank}, and no Frequency */
    n.rank = VJO_NO_RANK;
    TEST_CHECK(!strcmp(vjo_anki_field_value(&A, VJO_LAPIS_FREQ_SORT, &n), "9999999"));
    TEST_CHECK(!strcmp(vjo_anki_field_value(&A, VJO_LAPIS_FREQUENCY, &n), ""));
    /* a rank without a source: FreqSort only */
    n.rank = 7;
    n.source = NULL;
    TEST_CHECK(!strcmp(vjo_anki_field_value(&A, VJO_LAPIS_FREQ_SORT, &n), "7") &&
               !strcmp(vjo_anki_field_value(&A, VJO_LAPIS_FREQUENCY, &n), ""));
    /* SentenceFurigana with no readings beyond the sentence's: empty, so Lapis
     * shows {{furigana:Sentence}} (its README asks for it empty); none at all: empty */
    n.sentence_furigana = n.sentence;
    n.sf_hl_start = n.hl_start;
    n.sf_hl_end = n.hl_end;
    TEST_CHECK(!strcmp(vjo_anki_field_value(&A, VJO_LAPIS_SENTENCE_FURIGANA, &n), ""));
    n.sentence_furigana = NULL;
    TEST_CHECK(!strcmp(vjo_anki_field_value(&A, VJO_LAPIS_SENTENCE_FURIGANA, &n), ""));
    /* no pitch: no PitchPosition, no categories; no MiscInfo */
    n.n_pitch = 0;
    n.misc = NULL;
    TEST_CHECK(!strcmp(vjo_anki_field_value(&A, VJO_LAPIS_PITCH_POSITION, &n), "") &&
               !strcmp(vjo_anki_field_value(&A, VJO_LAPIS_PITCH_CATEGORIES, &n), "") &&
               !strcmp(vjo_anki_field_value(&A, VJO_LAPIS_MISC_INFO, &n), ""));
}

/* A card of the shown lookup: the entry's word, meanings, frequency, part of
 * speech and pitch copied out of the result memory, the whole text and the
 * word's place in it. */
static void test_anki_note_from_entry(void)
{
    static const char *const ga[] = {"indicates subject"}, *const neko[] = {"cat \"neko\"", "a\\b <&>\nc"};
    static const char *const suki[] = {"liked"}, *const prt[] = {"prt"}, *const noun[] = {"n"};
    static const char *const na[] = {"adj-na", "n"};
    VjoDictResult r = {0};
    VjoEntryList l;
    VjoAnkiNote n;
    VjoVocab *v;
    VjoToken *t;
    int cat = -1;
    setup();
    v = vjo_arena_zalloc(&A, 3 * sizeof(*v));
    t = vjo_arena_zalloc(&A, 3 * sizeof(*t));
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
    TEST_ASSERT(vjo_entries_build(&A, "猫が\n好き", &r, &l) == 0);
    for (int i = 0; i < l.n_entries; i++)
        if (!strcmp(l.entries[i].vocab->spelling, "猫"))
            cat = i;
    TEST_ASSERT(cat >= 0);
    TEST_ASSERT(vjo_anki_note_from_entry(&A, &l, cat, &n) == 0);
    TEST_CHECK(!strcmp(n.spelling, "猫") && !strcmp(n.reading, "ねこ") && n.rank == 1500 && n.n_meanings == 2 &&
               !strcmp(n.meanings[1], "a\\b <&>\nc") && n.n_pos == 1 && !strcmp(n.pos[0], "n") && n.n_pitch == 1 &&
               n.pitch[0] == 1 && !strcmp(n.source, "BCCWJ"));
    TEST_CHECK(!strcmp(n.sentence, "猫が\n好き") && n.hl_start == 0 && n.hl_end == 3);
    TEST_CHECK(!strcmp(vjo_anki_field_value(&A, VJO_LAPIS_SENTENCE, &n), "<b>猫</b>が<br>好き"));
    TEST_CHECK(!strcmp(vjo_anki_field_value(&A, VJO_LAPIS_FREQUENCY, &n),
                       "<ul style=\"text-align: left;\"><li>BCCWJ: 1500</li></ul>"));
    /* the copies are the card's own: the result memory may go (#528: its frequency source too, which a lookup during
     * the Shell's save wrote over) */
    TEST_CHECK(n.spelling != v[1].spelling && n.meanings[0] != v[1].meanings[0] && n.source != r.source);
    /* the dictionary's name: none here (no title, no "[name]" line) */
    TEST_CHECK(!n.dictionary);
    /* its file's title (Bee's Ultimate Dictionary names itself in page 0), copied */
    v[1].dictionary = "Bee's Ultimate Dictionary";
    TEST_ASSERT(vjo_anki_note_from_entry(&A, &l, cat, &n) == 0);
    TEST_CHECK(n.dictionary && !strcmp(n.dictionary, "Bee's Ultimate Dictionary") && n.dictionary != v[1].dictionary);
    /* else the name a definition's first line gives (the converter's "[title]") */
    {
        static const char *const labelled[] = {"[JMdict Plain]\ncat"};
        v[1].dictionary = NULL;
        v[1].meanings = (const char **)labelled;
        v[1].n_meanings = 1;
        TEST_ASSERT(vjo_anki_note_from_entry(&A, &l, cat, &n) == 0);
        TEST_CHECK(n.dictionary && !strcmp(n.dictionary, "JMdict Plain"));
        /* the title wins over a label line */
        v[1].dictionary = "Titled";
        TEST_ASSERT(vjo_anki_note_from_entry(&A, &l, cat, &n) == 0);
        TEST_CHECK(n.dictionary && !strcmp(n.dictionary, "Titled"));
    }
    TEST_CHECK(vjo_anki_note_from_entry(&A, &l, l.n_entries, &n) == -1);
    /* no frequency list: none (null in its record), never "" */
    l.source = NULL;
    TEST_ASSERT(vjo_anki_note_from_entry(&A, &l, cat, &n) == 0);
    TEST_CHECK(!n.source);
}

static void test_anki_config(void)
{
    VjoConfig c;
    /* AnkiConnect's address and the word audio source went with AnkiConnect,
     * and the field mapping before them: all ignored without a warning, in any
     * case, as an older config.ini has them; anki_mining stays */
    const char *ini = "anki_host = auto ; LAN\n"
                      "anki_audio_url = https://w.dev/audio/list?term={term}&reading={reading}&apiKey=a;b\n"
                      "ANKI_HOST = 192.168.1.5:8766\n"
                      "anki_deck = Mining #1 ; main\n"
                      "anki_note_type = Lapis;v2\n"
                      "anki_tags = a b\n"
                      "anki_field_picture =\n"
                      "Anki_Field_Word = Word\n"
                      "anki_mining = on\n";
    vjo_config_defaults(&c);
    TEST_CHECK(!c.anki_mining);
    vjo_config_parse(&c, ini, strlen(ini));
    TEST_CHECK(c.n_warnings == 0 && c.anki_mining == 1);
    TEST_MSG("%d: %s", c.n_warnings, c.warnings[0]);
    {
        static const char *const anki[] = {"anki_host", "Anki_Audio_URL", "anki_field_x"}; /* core/config.c's list */
        for (unsigned i = 0; i < sizeof(anki) / sizeof(anki[0]); i++) {
            const char *r = vjo_config_removed(anki[i], strlen(anki[i]));
            TEST_CHECK_(r && VJO_REMOVED_KIND(r) == VJO_REMOVED_ANKI, "%s", anki[i]);
        }
        TEST_CHECK(!vjo_config_removed("anki_mining", strlen("anki_mining")));
    }
    /* also silent where a setting's line would warn: no '=' (the line would be logged, and an
     * audio URL can carry a key), a value too long for any setting, a line too long to read */
    {
        static char big[3000];
        int k = snprintf(big, sizeof(big), "anki_audio_url https://w.dev/a?apiKey=SECRET1\n"
                                           "ANKI_HOST 10.0.0.2\nanki_field_word Word\nanki_audio_url = https://w.dev/b?k=");
        memset(big + k, 'x', 600);
        k += 600;
        k += snprintf(big + k, sizeof(big) - (size_t)k, "\nanki_audio_url = ");
        memset(big + k, 'y', 1500);
        k += 1500;
        snprintf(big + k, sizeof(big) - (size_t)k, "\nanki_mining = on\n");
        vjo_config_defaults(&c);
        vjo_config_parse(&c, big, strlen(big));
        TEST_CHECK(c.n_warnings == 0 && c.anki_mining == 1);
        TEST_MSG("%d: %s", c.n_warnings, c.warnings[0]);
        /* a setting that is not removed still warns there */
        const char *other = "local_dictionary_dir\nocr_mode on_press\n";
        vjo_config_defaults(&c);
        vjo_config_parse(&c, other, strlen(other));
        TEST_CHECK(c.n_warnings == 2 && !strcmp(c.warnings[0], "ignored line 1 (no '=')"));
        TEST_MSG("%d: %s", c.n_warnings, c.warnings[0]);
    }
    /* only those names: a near miss is still an unknown setting */
    ini = "anki_decks = x\nanki_field = x\nanki_tag = x\nanki_hosts = x\n";
    vjo_config_defaults(&c);
    vjo_config_parse(&c, ini, strlen(ini));
    TEST_CHECK(c.n_warnings == 4 && strstr(c.warnings[1], "unknown setting 'anki_field'") &&
               strstr(c.warnings[3], "unknown setting 'anki_hosts'"));
    TEST_MSG("%s", c.warnings[3]);
    ini = "anki_mining = maybe\n";
    vjo_config_defaults(&c);
    vjo_config_parse(&c, ini, strlen(ini));
    TEST_CHECK(c.n_warnings == 1 && !c.anki_mining && strstr(c.warnings[0], "anki_mining: invalid value"));
    /* the template the overlay writes has no AnkiConnect settings */
    TEST_CHECK(strstr(vjo_config_default_text(), "anki_mining = off") && !strstr(vjo_config_default_text(), "anki_host") &&
               !strstr(vjo_config_default_text(), "anki_audio_url") && !strstr(vjo_config_default_text(), "AnkiConnect"));
}

/* A field value takes the same bytes wherever it is built (#117's buf_done in core/anki.c): a
 * VjoBuf grows by doubling where the arena has room and to the exact size near its end, so each
 * value is cut to its text. The export (core/lapis.c) builds them card after card in one arena. */
static void test_anki_field_bytes_do_not_depend_on_the_arena(void)
{
    static uint8_t room[256 * 1024];
    static char text[3][5001];
    const char *meanings[1] = {text[0]}, *more_meanings[2][1] = {{text[1]}, {text[2]}};
    VjoAnkiGloss more[2];
    VjoAnkiNote n = {.spelling = "好き", .reading = "すき", .rank = 1200, .meanings = meanings, .n_meanings = 1,
                     .sentence = "猫が\n好き", .hl_start = 7, .hl_end = 13, .source = "BCCWJ", .dictionary = "Daijirin"};
    VjoArena a;
    for (int k = 0; k < 3; k++) { /* escaping grows them: & < > " and line breaks */
        for (int i = 0; i < 5000; i++)
            text[k][i] = "\"&<\n>xy"[(i + k) % 7];
        text[k][5000] = '\0';
    }
    for (int k = 0; k < 2; k++)
        more[k] = (VjoAnkiGloss){"JMdict", "好き", "すき", more_meanings[k], 1};
    n.more = more;
    n.n_more = 2;
    for (int f = 0; f < VJO_LAPIS_FIELDS; f++) {
        size_t need;
        vjo_arena_init(&a, room, sizeof(room));
        TEST_ASSERT(vjo_anki_field_value(&a, f, &n) != NULL);
        need = a.used;
        for (size_t size = need; size <= sizeof(room); size += 2003) {
            const char *v;
            vjo_arena_init(&a, room, size);
            v = vjo_anki_field_value(&a, f, &n);
            TEST_CHECK_(v && a.used == need, "field %d, arena of %u B: %s, %u B used (%u in the others)", f,
                        (unsigned)size, v ? "built" : "refused", (unsigned)a.used, (unsigned)need);
            if (!v || a.used != need)
                break;
        }
    }
}

/* #314's word key (core/word_key.h): the SHA-256 of spelling TAB reading, folded, its first 64 bits. The vectors are
 * Python's hashlib on the folded strings (hashlib.sha256("猫\tねこ".encode()).hexdigest()[:16]): the Rubi app's
 * export and any later reader of the key must agree with them. */
static void test_word_key(void)
{
    static const struct { const char *spelling, *reading, *hex; } vec[] = {
        {"猫", "ねこ", "cd77b2639247e16b"},
        {"食べる", "たべる", "e128a777deb3b1dd"},
        {"コーヒー", "コーヒー", "007166c0dae67709"},       /* こーひー TAB こーひー: the long vowel mark as written */
        {"ジョン・スミス", "ジョン・スミス", "9b4ec991ebd188fe"}, /* じょんすみす TAB じょんすみす */
        {"ねこ", "", "4877fccd5a1e29f4"},                 /* no reading: the spelling's */
        {"方", "かた", "9819f44e00a6aba5"},
        {"方", "ほう", "afd1a4d8b1a2b7a7"},
    };
    char hex[VJO_WORD_KEY_HEX];
    for (unsigned i = 0; i < sizeof(vec) / sizeof(vec[0]); i++) {
        vjo_word_key_hex(vjo_word_key(vec[i].spelling, vec[i].reading), hex);
        TEST_CHECK_(!strcmp(hex, vec[i].hex), "%s %s: %s, want %s", vec[i].spelling, vec[i].reading, hex, vec[i].hex);
    }
    /* katakana as hiragana, in either string; every inflection is the record's one dictionary form */
    TEST_CHECK(vjo_word_key("猫", "ネコ") == vjo_word_key("猫", "ねこ"));
    TEST_CHECK(vjo_word_key("ネコ", "ネコ") == vjo_word_key("ねこ", "ねこ"));
    TEST_CHECK(vjo_word_key("ヴァイオリン", "ヴァイオリン") == vjo_word_key("ゔぁいおりん", "ゔぁいおりん"));
    TEST_CHECK(vjo_word_key("ヽヾ", "ヽヾ") == vjo_word_key("ゝゞ", "ゝゞ"));
    TEST_CHECK(vjo_word_key("ヵヶ", "ヵヶ") == vjo_word_key("ゕゖ", "ゕゖ")); /* the range's end (audit-rubi-rm4 W1) */
    TEST_CHECK(vjo_word_key("ヷ", "ヷ") != vjo_word_key("\xE3\x82\x97", "\xE3\x82\x97")); /* U+30F7 not to U+3097 */
    TEST_CHECK(vjo_word_key("ヷ", "ヷ") != vjo_word_key("わ", "わ")); /* ヷ-ヺ have no hiragana: as written */
    /* the long vowel mark is not a vowel */
    TEST_CHECK(vjo_word_key("コーヒー", "コーヒー") != vjo_word_key("コオヒイ", "コオヒイ"));
    TEST_CHECK(vjo_word_key("コーヒー", "コーヒー") == vjo_word_key("こーひー", "こーひー"));
    /* the middle dot, full or half width, in either string */
    TEST_CHECK(vjo_word_key("ジョン・スミス", "じょん・すみす") == vjo_word_key("ジョンスミス", "じょんすみす"));
    TEST_CHECK(vjo_word_key("ｼﾞｮﾝ", "じょん･すみす") == vjo_word_key("ｼﾞｮﾝ", "じょんすみす"));
    /* a missing reading, or one of dots only, is the spelling's */
    TEST_CHECK(vjo_word_key("ねこ", "") == vjo_word_key("ねこ", "ねこ"));
    TEST_CHECK(vjo_word_key("ネコ", NULL) == vjo_word_key("ねこ", "ねこ"));
    TEST_CHECK(vjo_word_key("ねこ", "・") == vjo_word_key("ねこ", "ねこ"));
    /* other words stay apart: the reading, the spelling, where the TAB falls */
    TEST_CHECK(vjo_word_key("見る", "みる") != vjo_word_key("観る", "みる"));
    TEST_CHECK(vjo_word_key("方", "かた") != vjo_word_key("方", "ほう"));
    TEST_CHECK(vjo_word_key("ab", "c") != vjo_word_key("a", "bc"));
    TEST_CHECK(vjo_word_key("猫", "ねこ") != vjo_word_key("猫", "ねこ "));
    /* bytes that are not UTF-8 as they are, a lone lead byte at the end too */
    TEST_CHECK(vjo_word_key("\xE3\x82", "x") != vjo_word_key("", "x"));
    TEST_CHECK(vjo_word_key("\xE3\x83\xBB", "x") == vjo_word_key("", "x")); /* ・ alone */
    /* the spelling alone (known words without a reading): the same folds, never a word's key */
    {
        static const struct { const char *spelling, *hex; } sp[] = {
            {"猫", "a810bf845a307382"}, {"食べる", "2b7ce3887f50f5f1"}, {"コーヒー", "3118c7e596aedaf8"},
            {"ジョン・スミス", "1611c782d42cad20"}, {"方", "4a5ff677e1dbd37c"},
        };
        for (unsigned i = 0; i < sizeof(sp) / sizeof(sp[0]); i++) {
            vjo_word_key_hex(vjo_word_spelling_key(sp[i].spelling), hex);
            TEST_CHECK_(!strcmp(hex, sp[i].hex), "%s: %s, want %s", sp[i].spelling, hex, sp[i].hex);
        }
        TEST_CHECK(vjo_word_spelling_key("ネコ") == vjo_word_spelling_key("ねこ"));
        TEST_CHECK(vjo_word_spelling_key("ねこ") != vjo_word_key("ねこ", "ねこ"));
        TEST_CHECK(vjo_word_spelling_key("ねこ") != vjo_word_key("ねこ", ""));
    }
    vjo_word_key_hex(0x0123456789abcdefull, hex);
    TEST_CHECK(!strcmp(hex, "0123456789abcdef"));
    vjo_word_key_hex(0, hex);
    TEST_CHECK(!strcmp(hex, "0000000000000000"));
}

TEST_LIST = {
    {"word_key", test_word_key},
    {"anki_sentence_and_furigana", test_anki_sentence_and_furigana},
    {"anki_glossaries", test_anki_glossaries},
    {"anki_pitch", test_anki_pitch},
    {"anki_lapis_fields", test_anki_lapis_fields},
    {"anki_note_from_entry", test_anki_note_from_entry},
    {"anki_config", test_anki_config},
    {"anki_field_bytes_do_not_depend_on_the_arena", test_anki_field_bytes_do_not_depend_on_the_arena},
    {NULL, NULL},
};
