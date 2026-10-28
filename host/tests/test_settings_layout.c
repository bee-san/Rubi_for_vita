/* The Rubi app's text layouts and icon helpers, on their own:
 * - settings/text_layout.c lays rows out exactly as main.c's wrapped_text
 *   drew them before it (the reference below is that code, verbatim but for
 *   drawing into a list and, since 2026-10-16, ending a row where a line may
 *   break, core/linebreak.h), over generated text and several measure rules;
 *   Latin text without such marks breaks as before, Japanese never inside a
 *   word;
 * - its memo returns the same layouts and keeps a bounded number;
 * - settings/icons.c's PNG check lets only files whose every chunk lies
 *   inside the buffer through (libvita2d's buffer reader has no length), at
 *   every truncation and byte flip of a real icon file, each checked in a
 *   copy that ends at an unmapped page, so an over-read faults without a
 *   sanitizer; its file read is bounded; the icon cache evicts the least
 *   recently used icon outside the window and never one in it;
 * - settings/letter.c files each title under its letter, against a gojuon
 *   table written here, and orders the catalog so that each letter is one run.
 * Built for the host (ctest settings_layout) and, as a pure-C suite, for the
 * Vita's CPU under qemu-arm (tools/arm_host_tests.sh). */
#define main acutest_main
#include "acutest.h"
#undef main
#include "test_tmpdir.h"
#include <psp2/io/fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../core/utf.h"
#include "../../settings/icons.h"
#include "../../settings/letter.h"
#include "../../settings/text_layout.h"
#include "settings_png_fixture.h"
#include "settings_wrap_reference.h"

/* ---- the file calls, over real files (vjo_icon_read; fakes/sceio_posix.h) ---- */
#define SCEIO_OWN SCEIO_READ
#include "fakes/sceio_posix.h"
static int short_reads; /* sceIoRead returns at most this many bytes when set */
int sceIoRead(int fd, void *p, size_t len)
{
    if (short_reads && len > (size_t)short_reads) len = (size_t)short_reads;
    return sceio_read(fd, p, len);
}

/* ---- measure rules ---- */
static int measured; /* glyphs measured */
/* The settings harness's: 10 units an ASCII character, 18 any other, per 18-unit em. */
static int stub_width(float pixels, const char *s)
{
    int units = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p < 0x80) units += 10, measured++;
        else if ((*p & 0xC0) != 0x80) units += 18, measured++;
    return (int)(units * (pixels / 18.0f) + 0.5f);
}
/* Proportional widths with kerning: a string is not the sum of its parts. */
static int kerned_width(float pixels, const char *s)
{
    int w = 0;
    unsigned prev = 0;
    for (size_t i = 0, n = strlen(s); i < n;) {
        uint32_t cp = vjo_utf8_next(s, n, &i);
        w += cp < 0x80 ? (int)(4 + cp % 9) : cp < 0x3000 ? 13 : 19;
        if ((prev == 'A' && cp == 'V') || (prev == 'T' && cp == 'o')) w -= 3;
        prev = cp;
    }
    return (int)(w * pixels / 24.0f);
}
/* Any deterministic rule will do: a hash of the text. */
static int hashed_width(float pixels, const char *s)
{
    uint32_t h = 2166136261u;
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return (int)(n * pixels / 2.5f) + (int)(h % 23) - 11;
}
static VjoTextMeasure measure_rule;

/* ---- generated text ---- */
static uint32_t rng = 12345;
static uint32_t next(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }
static size_t put(char *out, size_t at, size_t cap, const char *piece)
{
    size_t n = strlen(piece);
    if (at + n >= cap) return at;
    memcpy(out + at, piece, n);
    return at + n;
}
/* Words, runs of spaces, CR/LF, kana and kanji, 4-byte characters, lone and
 * truncated UTF-8 bytes, and words longer than any row. */
static void gen_text(char *out, size_t cap)
{
    static const char *pieces[] = {
        "a", "Wo", "AV", "To", "Synthetic", "title", "edition", "-", ":", "1/2", "...", "…",
        " ", " ", " ", "  ", "   ", "\r", "\n", "\r\n", "\n\n",
        "あ", "テスト", "の", "漢字", "長い", "タイトル", "ー", "・", "　", "〜",
        "\xF0\x9F\x8E\xAE", "\xF0\xA0\x80\x8B", "\x80", "\xC3", "\xE3\x81", "\xFF",
        "Supercalifragilisticexpialidocious", "とてもとてもとてもとてもとてもとても長い単語"};
    size_t n = next() % 40, at = 0;
    if (next() % 6 == 0) n = 60 + next() % 120; /* long: past two rows, past 508 bytes */
    for (size_t i = 0; i < n; i++) at = put(out, at, cap, pieces[next() % (sizeof(pieces) / sizeof(pieces[0]))]);
    out[at] = 0;
}

static int same_as_reference(const char *s, int width, float pixels, int lines, int *rows_out)
{
    VjoTextLayout layout;
    char row[512];
    ref_measure = measure_rule;
    ref_wrapped_text(0, 100, width, pixels, 30, lines, s);
    vjo_text_layout(s, width, pixels, lines, measure_rule, &layout);
    *rows_out = layout.rows;
    if (ref_rows != layout.rows) return 0;
    for (int r = 0; r < layout.rows; r++) {
        vjo_text_layout_row(s, &layout, r, row);
        if (ref_row[r].y != 100 + r * 30 || strcmp(ref_row[r].text, row)) return 0;
    }
    return 1;
}

static void layout_matches_wrapped_text(void)
{
    static const VjoTextMeasure rules[] = {stub_width, kerned_width, hashed_width};
    static const int widths[] = {-5, 0, 1, 9, 40, 90, 300, 500, 620, 772, 870, 880, 912, 4000};
    static const float sizes[] = {22, 24, 26, 28, 32};
    char s[2048];
    long cases = 0, rows = 0, ellipses = 0, mismatches = 0;
    for (int rule = 0; rule < 3; rule++) {
        measure_rule = rules[rule];
        for (int i = 0; i < 4000; i++) {
            gen_text(s, sizeof(s));
            for (int w = 0; w < (int)(sizeof(widths) / sizeof(widths[0])); w++) {
                int lines = 1 + (int)(next() % VJO_TEXT_ROWS), n = 0;
                float px = sizes[next() % 5];
                if (!same_as_reference(s, widths[w], px, lines, &n)) {
                    if (mismatches++ < 5) TEST_MSG("rule %d width %d px %.0f lines %d: \"%s\"", rule, widths[w], px, lines, s);
                }
                VjoTextLayout l;
                vjo_text_layout(s, widths[w], px, lines, measure_rule, &l);
                for (int r = 0; r < l.rows; r++) ellipses += l.row[r].ellipsis;
                rows += n;
                cases++;
            }
        }
    }
    TEST_CHECK_(mismatches == 0, "%ld of %ld layouts differ from wrapped_text's rows", mismatches, cases);
    TEST_CHECK(rows > cases && ellipses > cases / 20); /* the cases wrap and cut, not only fit */
    TEST_MSG("%ld layouts, %ld rows, %ld cut with \"...\"", cases, rows, ellipses);
}

/* Latin text (letters, digits, ASCII marks and spaces) gets main's rows:
 * the old rule, at the last space that fits. */
static void latin_text_breaks_as_before(void)
{
    static const char *pieces[] = {"a", "Wo", "AV", "To", "Synthetic", "title", "edition", "-", ":", "1/2",
                                   "...", " ", " ", "  ", "\r", "\n", "Supercalifragilisticexpialidocious"};
    char s[1024];
    long cases = 0, bad = 0;
    measure_rule = stub_width;
    for (int i = 0; i < 3000; i++) {
        size_t at = 0, n = 1 + next() % 60;
        for (size_t k = 0; k < n; k++) at = put(s, at, sizeof(s), pieces[next() % (sizeof(pieces) / sizeof(pieces[0]))]);
        s[at] = 0;
        for (int w = 40; w <= 900; w += 215) {
            int rows;
            ref_breaks = 0;
            bad += !same_as_reference(s, w, 24, 1 + (int)(next() % VJO_TEXT_ROWS), &rows);
            cases++;
        }
    }
    ref_breaks = 1;
    TEST_CHECK_(bad == 0, "%ld of %ld Latin layouts differ from main's rows", bad, cases);
}

/* A Japanese title's first row ends where a phrase starts, never inside a
 * word (the owner's 2026-10-16 item 4); a word wider than the row still breaks. */
static void japanese_rows_end_between_words(void)
{
    static const struct { const char *title, *row0, *row1; int width; } cases[] = {
        /* 11 glyphs fit: テスト用のとても長 would cut 長い; the row ends before it */
        {"テスト用のとても長い日本語のタイトル", "テスト用のとても", "長い日本語のタイトル", 11 * 18},
        /* before 場合, rather than inside it; and after 、 when that is the last place */
        {"二行に、収まらない場合は省略", "二行に、収まらない", "場合は省略", 10 * 18},
        {"二行に、収まらない", "二行に、", "収まらない", 8 * 18},
        /* never before 。 or 」 (they stay with the word before) */
        {"「そうだね」と言った", "「そうだね」", "と言った", 6 * 18},
        /* a katakana word wider than the row: cut at its last glyph that fits */
        {"エンサイクロペディアブリタニカ", "エンサイクロペディ", "アブリタニカ", 9 * 18}};
    measure_rule = stub_width;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        VjoTextLayout l;
        char row[512];
        vjo_text_layout(cases[i].title, cases[i].width, 18, 2, stub_width, &l);
        TEST_CASE(cases[i].title);
        TEST_ASSERT(l.rows == 2);
        vjo_text_layout_row(cases[i].title, &l, 0, row);
        TEST_CHECK_(!strcmp(row, cases[i].row0), "row 0: %s", row);
        vjo_text_layout_row(cases[i].title, &l, 1, row);
        TEST_CHECK_(!strcmp(row, cases[i].row1), "row 1: %s", row);
    }
}

/* Titles as the Games page draws them: 620 px, 26 px text, two rows. */
static void layout_game_titles(void)
{
    static const char *titles[] = {
        "", " ", "A", "Short title", "A synthetic title that is long enough to need two rows of the card, and more",
        "テスト用のとても長い日本語のタイトルで、二行に収まらない場合は最後に省略記号が付きます",
        "Word\nbreak", "Trailing spaces    ", "    Leading spaces", "\r\n\r\n", "OneVeryLongWordWithoutAnySpaceToBreakAtAtAllEvenOnTheSecondRow"};
    measure_rule = stub_width;
    for (int i = 0; i < (int)(sizeof(titles) / sizeof(titles[0])); i++) {
        int n;
        TEST_CHECK_(same_as_reference(titles[i], 620, 26, 2, &n), "title %d", i);
    }
    /* Long leading whitespace: row offsets are 32-bit. */
    static char spaces[70000 + 8];
    memset(spaces, ' ', 70000);
    memcpy(spaces + 70000, "abc", 4);
    int n;
    TEST_CHECK(same_as_reference(spaces, 620, 26, 2, &n) && n == 1);
}

static int same_layout(const VjoTextLayout *a, const VjoTextLayout *b)
{
    if (a->rows != b->rows) return 0;
    for (int r = 0; r < a->rows; r++)
        if (a->row[r].start != b->row[r].start || a->row[r].bytes != b->row[r].bytes ||
            a->row[r].ellipsis != b->row[r].ellipsis) return 0;
    return 1;
}

static void memo_reuses_layouts(void)
{
    static VjoTextMemo memo;
    VjoTextLayout scratch, fresh;
    measure_rule = stub_width;
    vjo_text_memo_clear(&memo);
    const char *status = "Choices save automatically. Turn OFF games that need L+R.";
    measured = 0;
    const VjoTextLayout *a = vjo_text_memo_layout(&memo, status, 880, 24, 2, stub_width, &scratch);
    int first = measured;
    measured = 0;
    const VjoTextLayout *b = vjo_text_memo_layout(&memo, status, 880, 24, 2, stub_width, &scratch);
    TEST_CHECK(first > 100 && measured == 0 && a == b && memo.hits == 1 && memo.misses == 1);
    vjo_text_layout(status, 880, 24, 2, stub_width, &fresh);
    TEST_CHECK(same_layout(a, &fresh));
    /* Each key part makes another entry. */
    TEST_CHECK(vjo_text_memo_layout(&memo, status, 879, 24, 2, stub_width, &scratch) != a);
    TEST_CHECK(vjo_text_memo_layout(&memo, status, 880, 26, 2, stub_width, &scratch) != a);
    TEST_CHECK(vjo_text_memo_layout(&memo, status, 880, 24, 1, stub_width, &scratch) != a);
    TEST_CHECK(memo.misses == 4);
    char copy[128];
    snprintf(copy, sizeof(copy), "%s", status); /* the same text elsewhere: a hit */
    TEST_CHECK(vjo_text_memo_layout(&memo, copy, 880, 24, 2, stub_width, &scratch) == a && memo.hits == 2);
    /* Bounded: 16 entries, the least recently used goes. */
    vjo_text_memo_clear(&memo);
    char texts[40][32];
    for (int i = 0; i < 40; i++) {
        snprintf(texts[i], sizeof(texts[i]), "text %d", i);
        vjo_text_memo_layout(&memo, texts[i], 300, 24, 1, stub_width, &scratch);
    }
    TEST_CHECK(memo.misses == 40 && memo.hits == 0);
    vjo_text_memo_layout(&memo, texts[39], 300, 24, 1, stub_width, &scratch);
    vjo_text_memo_layout(&memo, texts[24], 300, 24, 1, stub_width, &scratch);
    TEST_CHECK(memo.hits == 2);
    vjo_text_memo_layout(&memo, texts[23], 300, 24, 1, stub_width, &scratch);
    TEST_CHECK(memo.misses == 41);
    /* Too long to keep: laid out into scratch each time, still right. */
    char long_text[400];
    memset(long_text, 'x', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = 0;
    const VjoTextLayout *l = vjo_text_memo_layout(&memo, long_text, 300, 24, 2, stub_width, &scratch);
    vjo_text_layout(long_text, 300, 24, 2, stub_width, &fresh);
    TEST_CHECK(l == &scratch && same_layout(l, &fresh));
    /* The edge: 255 bytes are kept, 256 are laid out into scratch each time. */
    char edge[VJO_TEXT_MEMO_TEXT + 1];
    for (int len = VJO_TEXT_MEMO_TEXT - 1; len <= VJO_TEXT_MEMO_TEXT; len++) {
        memset(edge, 'w', (size_t)len);
        edge[len] = 0;
        vjo_text_memo_clear(&memo);
        const VjoTextLayout *e1 = vjo_text_memo_layout(&memo, edge, 300, 24, 2, stub_width, &scratch);
        const VjoTextLayout *e2 = vjo_text_memo_layout(&memo, edge, 300, 24, 2, stub_width, &scratch);
        vjo_text_layout(edge, 300, 24, 2, stub_width, &fresh);
        if (len < VJO_TEXT_MEMO_TEXT) TEST_CHECK_(e1 == e2 && e1 != &scratch && memo.hits == 1, "%d bytes kept", len);
        else TEST_CHECK_(e1 == &scratch && e2 == &scratch && memo.hits == 0 && memo.misses == 2, "%d bytes", len);
        TEST_CHECK(same_layout(e2, &fresh));
    }
    /* lines above VJO_TEXT_ROWS are clamped, never past the row array */
    l = vjo_text_memo_layout(&memo, "a b c d e f g h i j k l m n o p", 10, 24, 9, stub_width, &scratch);
    TEST_CHECK(l->rows <= VJO_TEXT_ROWS);
}

/* ---- the titles' letters and order (settings/letter.c) ---- */
/* The gojuon rows, from the kana chart (not from letter.c): each row's
 * hiragana, small and voiced ones included, and the letter it is filed under.
 * ゔ is a voiced う, ゕ and ゖ small か and け. */
static uint32_t chart_letter(uint32_t c)
{
    static const struct { const char *members; uint32_t head; } rows[] = {
        {"ぁあぃいぅうぇえぉおゔ", 0x3042}, {"かがきぎくぐけげこごゕゖ", 0x304B}, {"さざしじすずせぜそぞ", 0x3055},
        {"ただちぢっつづてでとど", 0x305F}, {"なにぬねの", 0x306A}, {"はばぱひびぴふぶぷへべぺほぼぽ", 0x306F},
        {"まみむめも", 0x307E}, {"ゃやゅゆょよ", 0x3084}, {"らりるれろ", 0x3089}, {"ゎわゐゑをん", 0x308F}};
    int katakana = c >= 0x30A1 && c <= 0x30F6;
    uint32_t k = katakana ? c - 0x60 : c;
    for (unsigned r = 0; r < sizeof(rows) / sizeof(rows[0]); r++)
        for (size_t i = 0, n = strlen(rows[r].members); i < n;)
            if (vjo_utf8_next(rows[r].members, n, &i) == k) return rows[r].head + (katakana ? 0x60 : 0);
    if (c < 0x80) return c >= 'a' && c <= 'z' ? c - 0x20 : c >= 'A' && c <= 'Z' ? c : '#';
    return c;
}

static void title_letters(void)
{
    static const struct { const char *title; uint32_t letter; } cases[] = {
        {"abc", 'A'}, {"Zed", 'Z'}, {"3D Room", '#'}, {"{Curly}", '#'}, {"~", '#'}, {" lead", '#'}, {"", '#'},
        {"が", 0x304B}, {"ご", 0x304B}, {"ゔ", 0x3042}, {"ゕ", 0x304B}, {"ゖ", 0x304B}, {"ぁ", 0x3042}, {"ん", 0x308F},
        {"ガ", 0x30AB}, {"ヴ", 0x30A2}, {"ヵ", 0x30AB}, {"ヶ", 0x30AB}, {"ン", 0x30EF}, {"ヷ", 0x30F7}, {"ー", 0x30FC},
        {"ゝ", 0x309D}, {"ｶ", 0xFF76}, {"漢", 0x6F22}, {"é", 0xE9}, {"\xF0\x9F\x8E\xAE", 0x1F3AE},
        {"\xFF Broken", 0xFFFD}, {"\xE3\x81", 0xFFFD}, {"\xC3", 0xFFFD}};
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        TEST_CHECK_(vjo_title_letter(cases[i].title) == cases[i].letter, "case %u: U+%04X, want U+%04X", i,
                    (unsigned)vjo_title_letter(cases[i].title), (unsigned)cases[i].letter);
    /* Every kana and ASCII character as a title's first, against the chart. */
    int bad = 0;
    for (uint32_t c = 1; c < 0x3100; c++) {
        if (c >= 0x80 && c < 0x3040) continue;
        char t[8];
        int n = vjo_utf8_encode(c, t);
        t[n] = 'x';
        t[n + 1] = 0;
        if (vjo_title_letter(t) != chart_letter(c) && bad++ < 5)
            TEST_MSG("U+%04X: U+%04X, the chart U+%04X", (unsigned)c, (unsigned)vjo_title_letter(t),
                     (unsigned)chart_letter(c));
    }
    TEST_CHECK(bad == 0);
}

typedef struct { char title[64], id[12]; } Entry;
static int entry_order(const void *a, const void *b)
{
    const Entry *x = a, *y = b;
    return vjo_title_order(x->title, x->id, y->title, y->id);
}
static int sign(int v) { return (v > 0) - (v < 0); }

static void title_order(void)
{
    static const char *const starts[] = {
        "a", "A", "b", "z", "Z", "0", "8", "[", "{", "~", " ", "あ", "お", "ゔ", "か", "が", "ゕ", "ゖ", "ん", "ア",
        "ヴ", "カ", "ガ", "ヵ", "ヶ", "ン", "ヷ", "ー", "ｶ", "漢", "空", "é", "\xF0\x9F\x8E\xAE", "\xFF", "\xC3"};
    static const char *const tails[] = {"", "x", "X", "bc", "Bc", "の旅", " 2", "ab", "AB"};
    static Entry e[3000];
    int n = 0;
    for (unsigned s = 0; s < sizeof(starts) / sizeof(starts[0]); s++)
        for (unsigned t = 0; t < sizeof(tails) / sizeof(tails[0]); t++)
            for (int k = 0; k < 3 && n < 3000; k++, n++) {
                snprintf(e[n].title, sizeof(e[n].title), "%s%s", starts[s], tails[t]);
                snprintf(e[n].id, sizeof(e[n].id), "VJOT%05d", (int)(next() % 100000));
            }
    for (int i = n - 1; i > 0; i--) { /* shuffled */
        int j = (int)(next() % (unsigned)(i + 1));
        Entry tmp = e[i]; e[i] = e[j]; e[j] = tmp;
    }
    qsort(e, (size_t)n, sizeof(e[0]), entry_order);
    /* Each letter is one run, and the letters go up: '#', A-Z, then by code point. */
    int runs = 1, bad = 0;
    for (int i = 1; i < n; i++) {
        uint32_t a = vjo_title_letter(e[i - 1].title), b = vjo_title_letter(e[i].title);
        runs += a != b;
        if (a > b && bad++ < 5) TEST_MSG("U+%04X before U+%04X", (unsigned)a, (unsigned)b);
    }
    TEST_CHECK(bad == 0);
    int letters = 0;
    for (int i = 0; i < n; i++) {
        int seen = 0;
        for (int j = 0; j < i && !seen; j++) seen = vjo_title_letter(e[j].title) == vjo_title_letter(e[i].title);
        letters += !seen;
    }
    TEST_CHECK_(runs == letters, "%d runs, %d letters", runs, letters);
    /* A total order: antisymmetric, transitive, an entry only equal to itself. */
    for (int k = 0; k < 20000; k++) {
        const Entry *a = &e[next() % (unsigned)n], *b = &e[next() % (unsigned)n], *c = &e[next() % (unsigned)n];
        int ab = sign(entry_order(a, b)), bc = sign(entry_order(b, c)), ac = sign(entry_order(a, c));
        TEST_CHECK(ab == -sign(entry_order(b, a)));
        if (ab <= 0 && bc <= 0) TEST_CHECK(ac <= 0);
        if (!ab) TEST_CHECK(!strcmp(a->title, b->title) && !strcmp(a->id, b->id));
    }
    /* Case: letters fold, then a capital comes first; equal titles go by ID. */
    /* ("aC" after "Abd": c after b, whatever the case; a prefix before its longer titles) */
    static const Entry cased[] = {{"abc", "B"}, {"aBc", "A"}, {"ABC", "D"}, {"Abc", "C"}, {"abc", "A"},
                                  {"ab", "Z"}, {"Abd", "E"}, {"echo", "F"}, {"Fish", "G"}, {"Echo", "H"},
                                  {"aC", "J"}, {"Ab", "I"}};
    static const char *const want[] = {"Ab", "ab", "ABC", "Abc", "aBc", "abc", "abc", "Abd", "aC", "Echo", "echo",
                                       "Fish"};
    static const char *const want_id[] = {"I", "Z", "D", "C", "A", "A", "B", "E", "J", "H", "F", "G"};
    enum { CASED = sizeof(cased) / sizeof(cased[0]) };
    Entry c[CASED];
    memcpy(c, cased, sizeof(c));
    qsort(c, CASED, sizeof(c[0]), entry_order);
    for (int i = 0; i < CASED; i++)
        TEST_CHECK_(!strcmp(c[i].title, want[i]) && !strcmp(c[i].id, want_id[i]), "%d: %s %s, want %s %s", i,
                    c[i].title, c[i].id, want[i], want_id[i]);
}

/* ---- the PNG check ---- */
/* A 128 x 128 icon as installed games have, and a 24 x 24 one for the
 * exhaustive loops (every truncation, every byte), which cost its size squared. */
static uint8_t icon[64 * 1024], small[4096];
static size_t icon_size, small_size;
static void make_icon(void)
{
    if (!icon_size) icon_size = fx_png(icon, sizeof(icon), 128, 128, 1, 7, "PCSG00001");
    if (!small_size) small_size = fx_png(small, sizeof(small), 24, 24, 1, 5, "PCSG00002");
    TEST_ASSERT(icon_size > 100 && small_size > 100);
}
/* A chunk type the PNG spec allows (four ASCII letters, the third upper case) whose
 * first letter is lower case: ancillary. Written from the spec, not from icons.c. */
static int spec_ancillary_name(const uint8_t *t)
{
    for (int i = 0; i < 4; i++)
        if (!((t[i] >= 'A' && t[i] <= 'Z') || (t[i] >= 'a' && t[i] <= 'z'))) return 0;
    return t[2] >= 'A' && t[2] <= 'Z' && t[0] >= 'a' && t[0] <= 'z';
}
/* The check on a copy that ends where an unmapped page begins: a read past the
 * end faults in any build, not only under ASan (CI runs none). */
static int check_copy(const uint8_t *p, size_t n, unsigned side)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE), data = (n + page - 1) / page * page;
    uint8_t *map = mmap(NULL, data + page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TEST_ASSERT(map != MAP_FAILED);
    TEST_ASSERT(mprotect(map + data, page, PROT_NONE) == 0);
    uint8_t *copy = map + data - n;
    if (n) memcpy(copy, p, n);
    unsigned w = 0, h = 0;
    int rc = vjo_icon_png_check(copy, n, &w, &h);
    if (rc == VJO_ICON_OK) TEST_CHECK(w == side && h == side);
    munmap(map, data + page);
    return rc;
}

/* The small icon's chunks, by type: its offset in the file (0 when absent). */
static size_t chunk_at(const uint8_t *png, size_t size, const char *type)
{
    for (size_t at = 8; at + 12 <= size; at += 12 + (size_t)fx_get32(png + at))
        if (!memcmp(png + at + 4, type, 4)) return at;
    return 0;
}

/* CRCs: an ancillary chunk's may be wrong (libpng warns and loads), a critical one's not. */
static void png_check_crc_rules(void)
{
    uint8_t png[8192];
    make_icon();
    size_t text = chunk_at(small, small_size, "tEXt"), plte = chunk_at(small, small_size, "PLTE"),
           idat = chunk_at(small, small_size, "IDAT");
    TEST_ASSERT(text && plte && idat);
    memcpy(png, small, small_size);
    png[text + 8 + fx_get32(small + text) + 3] ^= 1; /* tEXt's CRC */
    TEST_CHECK(vjo_icon_png_check(png, small_size, NULL, NULL) == VJO_ICON_OK);
    png[text + 9] ^= 0x40; /* and its text */
    TEST_CHECK(vjo_icon_png_check(png, small_size, NULL, NULL) == VJO_ICON_OK);
    memcpy(png, small, small_size);
    png[plte + 8 + fx_get32(small + plte) + 3] ^= 1; /* PLTE's CRC */
    TEST_CHECK(vjo_icon_png_check(png, small_size, NULL, NULL) == VJO_ICON_BAD_CRC);
    memcpy(png, small, small_size);
    png[idat + 20] ^= 4; /* IDAT's data */
    TEST_CHECK(vjo_icon_png_check(png, small_size, NULL, NULL) == VJO_ICON_BAD_CRC);
    memcpy(png, small, small_size);
    png[small_size - 1] ^= 1; /* IEND's CRC */
    TEST_CHECK(vjo_icon_png_check(png, small_size, NULL, NULL) == VJO_ICON_BAD_CRC);
}

/* The image data is one run of IDATs, after a palette image's PLTE. */
static void png_check_chunk_order(void)
{
    uint8_t png[8192];
    make_icon();
    size_t plte = chunk_at(small, small_size, "PLTE"), idat = chunk_at(small, small_size, "IDAT"),
           iend = chunk_at(small, small_size, "IEND");
    uint32_t n = fx_get32(small + idat);
    const uint8_t *data = small + idat + 8;
    TEST_ASSERT(plte && idat && iend && n > 20);
    /* IDAT split in two, back to back: one run. */
    size_t at = idat;
    memcpy(png, small, idat);
    at += fx_chunk(png + at, "IDAT", data, 10);
    at += fx_chunk(png + at, "IDAT", data + 10, n - 10);
    memcpy(png + at, small + iend, small_size - iend);
    size_t split = at + small_size - iend;
    TEST_CHECK(vjo_icon_png_check(png, split, NULL, NULL) == VJO_ICON_OK);
    /* a chunk between the two halves: refused (libpng would fail mid-image) */
    at = idat;
    at += fx_chunk(png + at, "IDAT", data, 10);
    at += fx_chunk(png + at, "tIME", (const uint8_t *)"\x07\xea\x0a\x0f\0\0\0", 7);
    at += fx_chunk(png + at, "IDAT", data + 10, n - 10);
    memcpy(png + at, small + iend, small_size - iend);
    TEST_CHECK(vjo_icon_png_check(png, at + small_size - iend, NULL, NULL) == VJO_ICON_BAD_ORDER);
    /* an ancillary chunk after the image data, before IEND: fine (libvita2d never reads it) */
    at = idat + 12 + n;
    memcpy(png, small, at);
    at += fx_chunk(png + at, "tIME", (const uint8_t *)"\x07\xea\x0a\x0f\0\0\0", 7);
    memcpy(png + at, small + iend, small_size - iend);
    TEST_CHECK(vjo_icon_png_check(png, at + small_size - iend, NULL, NULL) == VJO_ICON_OK);
    /* a palette image's IDAT before its PLTE */
    at = plte;
    memcpy(png, small, plte);
    memcpy(png + at, small + idat, 12 + n);
    at += 12 + n;
    memcpy(png + at, small + plte, idat - plte);
    at += idat - plte;
    memcpy(png + at, small + iend, small_size - iend);
    TEST_CHECK(vjo_icon_png_check(png, at + small_size - iend, NULL, NULL) == VJO_ICON_BAD_ORDER);
    /* an RGBA image has no PLTE */
    uint8_t rgba[80 * 1024];
    size_t size = fx_png(rgba, sizeof(rgba), 16, 16, 0, 2, NULL);
    TEST_CHECK(size && !chunk_at(rgba, size, "PLTE") && vjo_icon_png_check(rgba, size, NULL, NULL) == VJO_ICON_OK);
}

static void png_check_accepts_icons(void)
{
    uint8_t png[300 * 1024];
    unsigned w, h;
    make_icon();
    TEST_CHECK(check_copy(icon, icon_size, 128) == VJO_ICON_OK);
    TEST_CHECK(check_copy(small, small_size, 24) == VJO_ICON_OK);
    size_t n = fx_png(png, sizeof(png), 256, 256, 0, 3, NULL); /* the largest allowed, RGBA */
    TEST_CHECK(n && vjo_icon_png_check(png, n, &w, &h) == VJO_ICON_OK && w == 256 && h == 256);
    n = fx_png(png, sizeof(png), 1, 1, 1, 3, NULL);
    TEST_CHECK(n && vjo_icon_png_check(png, n, &w, &h) == VJO_ICON_OK && w == 1 && h == 1);
    /* Bytes after IEND are not read. */
    memcpy(png, icon, icon_size);
    memset(png + icon_size, 0xA5, 32);
    TEST_CHECK(vjo_icon_png_check(png, icon_size + 32, NULL, NULL) == VJO_ICON_OK);
}

static void png_check_refuses_damage(void)
{
    uint8_t png[300 * 1024];
    make_icon();
    long refused = 0;
    /* Every truncation: never OK (IEND is the last 12 bytes), never past the end (ASan). */
    for (size_t n = 0; n < small_size; n++) {
        int rc = check_copy(small, n, 24);
        TEST_CHECK_(rc != VJO_ICON_OK, "truncated to %zu bytes", n);
        refused += rc != VJO_ICON_OK;
    }
    for (size_t n = 0; n < icon_size; n += 97) TEST_CHECK(check_copy(icon, n, 128) != VJO_ICON_OK);
    /* Every single-bit change. The oracle, from the PNG spec and libpng's defaults: a
     * change inside an ancillary chunk's data or CRC still loads (libpng warns on the
     * CRC), as does one that leaves an ancillary chunk's type another valid ancillary
     * type (libpng skips unknown ancillary chunks); any other change is refused. */
    memcpy(png, small, small_size);
    int accepted = 0;
    for (size_t i = 0; i < small_size; i++) {
        uint8_t keep = png[i];
        png[i] ^= (uint8_t)(1u << (i % 8));
        int rc = check_copy(png, small_size, 24), want_ok = 0;
        for (size_t at = 8; at + 12 <= small_size;) { /* the original file's chunks */
            uint32_t n = fx_get32(small + at);
            int ancillary = small[at + 4] & 0x20;
            if (ancillary && i >= at + 8 && i < at + 12 + n) want_ok = 1;
            if (ancillary && i >= at + 4 && i < at + 8) want_ok = spec_ancillary_name(png + at + 4);
            at += 12 + (size_t)n;
        }
        png[i] = keep;
        TEST_CHECK_((rc == VJO_ICON_OK) == want_ok, "byte %zu changed: %d", i, rc);
        refused += rc != VJO_ICON_OK;
        accepted += rc == VJO_ICON_OK;
    }
    TEST_CHECK(accepted > 8 && refused > (long)small_size); /* both kinds were met */
    TEST_MSG("%ld damaged files refused", refused);
    /* Lengths that would wrap a 32-bit size_t, with valid CRCs over what is there. */
    static const uint32_t lengths[] = {0x7FFFFFFFu, 0x80000000u, 0xFFFFFFF0u, 0xFFFFFFFFu, 0x10000u};
    for (int k = 0; k < 5; k++) {
        memcpy(png, icon, 33); /* signature and IHDR */
        fx_be32(png + 33, lengths[k]);
        memcpy(png + 37, "tEXt", 4);
        TEST_CHECK(vjo_icon_png_check(png, 33 + 12 + 16, NULL, NULL) == VJO_ICON_TRUNCATED);
    }
    /* Sizes, header and order. */
    size_t n = fx_png(png, sizeof(png), 257, 4, 1, 1, NULL);
    TEST_CHECK(n && vjo_icon_png_check(png, n, NULL, NULL) == VJO_ICON_BAD_SIZE);
    n = fx_png(png, sizeof(png), 4, 257, 1, 1, NULL);
    TEST_CHECK(n && vjo_icon_png_check(png, n, NULL, NULL) == VJO_ICON_BAD_SIZE);
    memcpy(png, icon, icon_size);
    fx_be32(png + 16, 0);
    fx_chunk(png + 8, "IHDR", png + 16, 13);
    TEST_CHECK(vjo_icon_png_check(png, icon_size, NULL, NULL) == VJO_ICON_BAD_SIZE);
    uint8_t first[64];
    size_t at = 8;
    memcpy(first, icon, 8);
    at += fx_chunk(first + at, "tEXt", (const uint8_t *)"Comment\0x", 9);
    TEST_CHECK(vjo_icon_png_check(first, at, NULL, NULL) == VJO_ICON_BAD_HEADER);
    /* A 13-byte first chunk that is not IHDR, even with plausible "sizes", then an image. */
    static const uint8_t fake[13] = {0, 0, 0, 16, 0, 0, 0, 16, 8, 3, 0, 0, 0};
    size_t ihdr_end = 33;
    at = 8 + fx_chunk(png + 8, "tEXt", fake, 13);
    memcpy(png + at, small + ihdr_end, small_size - ihdr_end);
    TEST_CHECK(vjo_icon_png_check(png, at + small_size - ihdr_end, NULL, NULL) == VJO_ICON_BAD_HEADER);
    /* IHDR, then IEND: no image data. */
    memcpy(first, icon, 33);
    at = 33 + fx_chunk(first + 33, "IEND", NULL, 0);
    TEST_CHECK(vjo_icon_png_check(first, at, NULL, NULL) == VJO_ICON_NO_IMAGE);
    /* No IEND at all. */
    TEST_CHECK(vjo_icon_png_check(icon, icon_size - 12, NULL, NULL) == VJO_ICON_TRUNCATED);
    /* Chunk names libpng refuses: a digit, the third letter lower case. */
    memcpy(png, small, 33);
    at = 33 + fx_chunk(png + 33, "t3Xt", (const uint8_t *)"x", 1);
    memcpy(png + at, small + 33, small_size - 33);
    TEST_CHECK(vjo_icon_png_check(png, at + small_size - 33, NULL, NULL) == VJO_ICON_BAD_CHUNK);
    at = 33 + fx_chunk(png + 33, "tExt", (const uint8_t *)"x", 1);
    memcpy(png + at, small + 33, small_size - 33);
    TEST_CHECK(vjo_icon_png_check(png, at + small_size - 33, NULL, NULL) == VJO_ICON_BAD_CHUNK);
    TEST_CHECK(vjo_icon_png_check((const uint8_t *)"GIF89a....", 10, NULL, NULL) == VJO_ICON_NOT_PNG);
}

/* The harness's decoder (settings_png_fixture.h) gives back the image the
 * fixture encoded, through its Sub/Up filters, the palette and the tag. */
static void fixture_round_trip(void)
{
    static uint8_t png[80 * 1024], rgba[128 * 128 * 4];
    char id[16];
    for (int palette = 0; palette < 2; palette++) {
        size_t n = fx_png(png, sizeof(png), 128, 128, palette, 11, "VJOT00042");
        TEST_ASSERT(n > 0);
        TEST_CHECK(vjo_icon_png_check(png, n, NULL, NULL) == VJO_ICON_OK);
        TEST_CHECK(fx_png_decode(png, n, rgba, id, sizeof(id)) == 0 && !strcmp(id, "VJOT00042"));
        int same = 1;
        for (unsigned y = 0; y < 128; y++)
            for (unsigned x = 0; x < 128; x++)
                for (unsigned c = 0; c < 3; c++) {
                    uint8_t v = (uint8_t)((x * 7u + y * 3u + 11u * 13u + (palette ? 0 : c * 41u)) ^ ((x * y) >> 4) ^ ((x ^ 11u) & 15u));
                    uint8_t want = palette ? (uint8_t)(c == 0 ? v : c == 1 ? 255 - v : v * 5 + 11) : v;
                    same &= rgba[(y * 128 + x) * 4 + c] == want;
                }
        TEST_CHECK_(same, "palette %d", palette);
    }
}

static char dir[PATH_MAX];
static void in_temp_dir(void)
{
    if (dir[0]) return;
    TEST_ASSERT(vjo_test_tmpdir_new(dir, sizeof(dir)) != NULL);
    TEST_ASSERT(chdir(dir) == 0);
}
static void put_file(const char *path, const uint8_t *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    TEST_ASSERT(f != NULL);
    TEST_ASSERT(fwrite(data, 1, n, f) == n);
    TEST_ASSERT(fclose(f) == 0);
}

static void icon_read_is_bounded(void)
{
    static uint8_t buf[VJO_ICON_FILE_MAX + VJO_ICON_FILE_PAD], big[VJO_ICON_FILE_MAX + 1];
    in_temp_dir();
    make_icon();
    put_file("icon.png", icon, icon_size);
    memset(buf, 0xEE, sizeof(buf));
    TEST_CHECK(vjo_icon_read("icon.png", buf) == (long)icon_size);
    TEST_CHECK(!memcmp(buf, icon, icon_size));
    int pad = 1;
    for (int i = 0; i < VJO_ICON_FILE_PAD; i++) pad &= buf[icon_size + i] == 0;
    TEST_CHECK(pad);
    short_reads = 1000; /* the card returns short reads: still whole */
    memset(buf, 0, sizeof(buf));
    TEST_CHECK(vjo_icon_read("icon.png", buf) == (long)icon_size && !memcmp(buf, icon, icon_size));
    short_reads = 0;
    TEST_CHECK(vjo_icon_read("missing.png", buf) == VJO_ICON_ABSENT);
    put_file("empty.png", big, 0);
    TEST_CHECK(vjo_icon_read("empty.png", buf) == 0);
    put_file("max.png", big, VJO_ICON_FILE_MAX);
    TEST_CHECK(vjo_icon_read("max.png", buf) == VJO_ICON_FILE_MAX);
    put_file("big.png", big, VJO_ICON_FILE_MAX + 1);
    TEST_CHECK(vjo_icon_read("big.png", buf) == VJO_ICON_TOO_BIG);
    mkdir("a-directory", 0777);
    TEST_CHECK(vjo_icon_read("a-directory", buf) == VJO_ICON_ABSENT);
    unlink("icon.png"); unlink("empty.png"); unlink("max.png"); unlink("big.png"); rmdir("a-directory");
    if (chdir("/") == 0) rmdir(dir);
    dir[0] = 0;
}

/* ---- the icon cache ---- */
static int released;
static void release(void *t) { (void)t; released++; }
static void icon_cache_lru(void)
{
    static VjoIconCache cache;
    static int tex[VJO_ICON_SLOTS + 2];
    char id[VJO_ICON_SLOTS + 2][VJO_TITLE_ID_SIZE];
    memset(&cache, 0, sizeof(cache));
    for (int i = 0; i < VJO_ICON_SLOTS + 2; i++) snprintf(id[i], sizeof(id[i]), "PCSG%05d", i);
    TEST_CHECK(vjo_icon_cache_get(&cache, id[0]) == NULL && vjo_icon_cache_count(&cache) == 0);
    for (int i = 0; i < VJO_ICON_SLOTS; i++) TEST_CHECK(vjo_icon_cache_put(&cache, id[i], &tex[i]) == 0);
    TEST_CHECK(vjo_icon_cache_count(&cache) == VJO_ICON_SLOTS);
    TEST_CHECK(vjo_icon_cache_put(&cache, id[VJO_ICON_SLOTS], &tex[VJO_ICON_SLOTS]) == -1);
    for (int i = 0; i < VJO_ICON_SLOTS; i++) TEST_CHECK(vjo_icon_cache_get(&cache, id[i]) == &tex[i]);
    /* Use 0 again: 1 is now the least recently used. */
    vjo_icon_cache_get(&cache, id[0]);
    TEST_CHECK(vjo_icon_cache_evict(&cache, NULL, 0) == &tex[1] && vjo_icon_cache_count(&cache) == VJO_ICON_SLOTS - 1);
    TEST_CHECK(vjo_icon_cache_get(&cache, id[1]) == NULL);
    /* Kept entries are never evicted: protect 2..4, leaving 0 as the only candidate. */
    const char *keep[] = {id[2], id[3], id[4]};
    TEST_CHECK(vjo_icon_cache_evict(&cache, keep, 3) == &tex[0]);
    /* Everything kept: nothing to evict. */
    const char *all[VJO_ICON_SLOTS];
    int n = 0;
    for (int i = 0; i < VJO_ICON_SLOTS; i++) if (vjo_icon_cache_get(&cache, id[i])) all[n++] = id[i];
    TEST_CHECK(n == VJO_ICON_SLOTS - 2 && vjo_icon_cache_evict(&cache, all, n) == NULL);
    /* An 11-character ID (the longest) is kept whole. */
    TEST_CHECK(vjo_icon_cache_put(&cache, "ABCDEFGHIJK", &tex[VJO_ICON_SLOTS]) == 0);
    TEST_CHECK(vjo_icon_cache_get(&cache, "ABCDEFGHIJK") == &tex[VJO_ICON_SLOTS]);
    TEST_CHECK(vjo_icon_cache_get(&cache, "ABCDEFGHIJ") == NULL);
    released = 0;
    vjo_icon_cache_clear(&cache, release);
    TEST_CHECK(released == VJO_ICON_SLOTS - 1 && vjo_icon_cache_count(&cache) == 0);
    TEST_CHECK(vjo_icon_cache_get(&cache, id[0]) == NULL);
}

TEST_LIST = {
    {"layout_matches_wrapped_text", layout_matches_wrapped_text},
    {"latin_text_breaks_as_before", latin_text_breaks_as_before},
    {"japanese_rows_end_between_words", japanese_rows_end_between_words},
    {"layout_game_titles", layout_game_titles},
    {"title_letters", title_letters},
    {"title_order", title_order},
    {"memo_reuses_layouts", memo_reuses_layouts},
    {"png_check_accepts_icons", png_check_accepts_icons},
    {"png_check_refuses_damage", png_check_refuses_damage},
    {"png_check_crc_rules", png_check_crc_rules},
    {"png_check_chunk_order", png_check_chunk_order},
    {"fixture_round_trip", fixture_round_trip},
    {"icon_read_is_bounded", icon_read_is_bounded},
    {"icon_cache_lru", icon_cache_lru},
    {NULL, NULL}
};

int main(int argc, char **argv)
{
    if (!vjo_test_tmpdir("settings-layout")) return 2; /* the icon files' directory in it, removed at the end */
    return acutest_main(argc, argv);
}
