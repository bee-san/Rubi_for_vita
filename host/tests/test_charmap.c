/* The char map (core/charmap.h, core/charmap_build.h; API 37, docs/overlay-on-text.md): the block's format, the
 * game worker's writer against the Shell's sentence (core/textfilter.c + vjo_java_trim), the hand-over into the
 * Shell's text buffer, frame-to-screen transforms (a 960 x 544 frame, PSP's 480 x 272 at 2x, an Adrenaline frame),
 * hit-testing, a word's rectangles, Up/Down across lines and the popup's place. No model, no file. */
#include "acutest.h"

#include <stdio.h>
#include <string.h>

#include "../../core/arena.h"
#include "../../core/charmap.h"
#include "../../core/charmap_build.h"
#include "../../core/textfilter.h"
#include "../../core/utf.h"

#define CAP 4096

static VjoCmapScratch scratch; /* the worker lends its per-line box buffer (game_ocr/main.c) */

/* A recognized line of n characters from text (code points), each `pitch` px wide from x0, at rows y0..y1. */
typedef struct {
    uint16_t cp[128];
    uint16_t a[128], b[128];
} Line;

static VjoCmapSource line_of(Line *L, const char *text, unsigned x0, unsigned pitch, unsigned y0, unsigned y1)
{
    VjoCmapSource s;
    size_t i = 0, len = strlen(text);
    unsigned n = 0;
    while (i < len && n < 128) {
        L->cp[n] = (uint16_t)vjo_utf8_next(text, len, &i);
        L->a[n] = (uint16_t)(x0 + n * pitch);
        L->b[n] = (uint16_t)(x0 + (n + 1) * pitch);
        n++;
    }
    s.cp = L->cp;
    s.a = L->a;
    s.b = L->b;
    s.n = n;
    s.x = 0;
    s.y0 = y0;
    s.y1 = y1;
    return s;
}

/* The worker's result buffer: text, NUL, raw, NUL, zeros. */
static void result_text(char *buf, const char *text, const char *raw)
{
    memset(buf, 0, CAP);
    strcpy(buf, text);
    strcpy(buf + strlen(text) + 1, raw);
}

/* The Shell's sentence of a worker text (shell/worker.c run_vocr). */
static size_t shell_sentence(const char *text, int filter, char *out)
{
    static unsigned char mem[3 * CAP];
    VjoArena a;
    char *f, *t;
    vjo_arena_init(&a, mem, sizeof(mem));
    f = vjo_filter_lines(&a, text, filter ? VJO_FILTER_LINES : VJO_FILTER_NONE);
    TEST_ASSERT(f != NULL);
    t = vjo_java_trim(f);
    strcpy(out, t);
    return strlen(t);
}

static void test_sentence_matches_the_shell(void)
{
    static const char *const texts[] = {
        "「こんにちは」", "  名前\n「今日はいい天気だね」\nAuto Skip Log\n", "abc\ndef", "\nあ\n\n い \n",
        "Menu\r\n選択肢\r\nEnd", "   ", "ーニーーニ\n本文です", "a\n\nb\nか\n", "\t\tテスト\t ",
    };
    for (unsigned t = 0; t < sizeof(texts) / sizeof(texts[0]); t++)
        for (int filter = 0; filter < 2; filter++) {
            char shell[CAP];
            unsigned char kept[VJO_CMAP_LINES];
            size_t lead, len, n = shell_sentence(texts[t], filter, shell);
            uint32_t fnv;
            vjo_cmap_sentence(texts[t], filter, kept, &lead, &len, &fnv);
            TEST_CHECK_(len == n, "text %u filter %d: %zu vs %zu", t, filter, len, n);
            TEST_CHECK_(fnv == vjo_cmap_fnv(shell, n), "text %u filter %d", t, filter);
        }
}

static void test_build_and_read_back(void)
{
    char buf[CAP], shell[CAP];
    Line l0, l1;
    VjoCmapSource src[2];
    VjoCmap m;
    size_t n, len;
    /* a name line (no Japanese? it has: a kanji name), a dialogue line, a UI line the filter drops */
    result_text(buf, "真奈\n「そうだね」\nAuto", "真奈\n「そうだね」\nAuto");
    src[0] = line_of(&l0, "真奈", 100, 20, 400, 424);
    src[1] = line_of(&l1, "「そうだね」", 100, 22, 430, 456);
    n = vjo_cmap_build(buf, CAP, 1, src, 2, 960, 544, &scratch);
    TEST_CHECK(n > 0);
    len = shell_sentence(buf, 1, shell);
    TEST_CHECK(strcmp(shell, "真奈\n「そうだね」") == 0);
    /* the Shell takes it after its own sentence */
    TEST_CHECK(vjo_cmap_carry(shell, CAP, len, buf, CAP) == n);
    TEST_ASSERT(vjo_cmap_find(shell, CAP, len, &m));
    TEST_CHECK(m.lines == 2 && m.chars == 2 + 6 && m.fb_w == 960 && m.fb_h == 544);
    {
        uint32_t off;
        unsigned line, x0, x1, y0, y1;
        vjo_cmap_get(&m, 0, &off, &line, &x0, &x1);
        TEST_CHECK(off == 0 && line == 0 && x0 == 100 && x1 == 120);
        vjo_cmap_get(&m, 2, &off, &line, &x0, &x1); /* 「: after "真奈\n" (3 + 3 + 1 bytes) */
        TEST_CHECK(off == 7 && line == 1 && x0 == 100 && x1 == 122);
        vjo_cmap_line(&m, 1, &y0, &y1);
        TEST_CHECK(y0 == 430 && y1 == 456);
        TEST_CHECK(vjo_cmap_index(&m, 10) == 3); /* そ */
        TEST_CHECK(vjo_cmap_index(&m, 11) == -1); /* inside そ */
    }
    /* another sentence: the block is refused */
    {
        char other[CAP];
        memset(other, 0, CAP);
        strcpy(other, "真奈\n「そうだな」");
        TEST_CHECK(vjo_cmap_carry(other, CAP, strlen(other), buf, CAP) == 0);
        TEST_CHECK(!vjo_cmap_find(other, CAP, strlen(other), &m) && m.p == NULL);
    }
}

static void test_lm_edits_and_inserted_characters(void)
{
    char buf[CAP], shell[CAP];
    Line l0;
    VjoCmapSource src;
    VjoCmap m;
    size_t len;
    /* the recognizer read そうだね; the LM made it そーだね and the normalisation added nothing; then an
     * inserted character (a hook's 、) has a zero-width cell at its neighbour */
    result_text(buf, "そーだ、ね", "そうだね");
    src = line_of(&l0, "そうだね", 0, 30, 10, 40);
    TEST_ASSERT(vjo_cmap_build(buf, CAP, 1, &src, 1, 960, 544, &scratch) > 0);
    len = shell_sentence(buf, 1, shell);
    TEST_ASSERT(vjo_cmap_carry(shell, CAP, len, buf, CAP) > 0);
    TEST_ASSERT(vjo_cmap_find(shell, CAP, len, &m));
    TEST_CHECK(m.chars == 5);
    {
        uint32_t off;
        unsigned line, x0, x1;
        vjo_cmap_get(&m, 1, &off, &line, &x0, &x1); /* ー on う's cell */
        TEST_CHECK(off == 3 && x0 == 30 && x1 == 60);
        vjo_cmap_get(&m, 3, &off, &line, &x0, &x1); /* the inserted 、: zero width at だ's end */
        TEST_CHECK(off == 9 && x0 == 90 && x1 == 90);
        vjo_cmap_get(&m, 4, &off, &line, &x0, &x1); /* ね */
        TEST_CHECK(x0 == 90 && x1 == 120);
    }
    /* a zero-width cell holds no tap */
    TEST_CHECK(vjo_cmap_hit(&m, 90, 20) == 4);
}

static void test_unmatched_lines_get_no_boxes(void)
{
    char buf[CAP];
    Line l0;
    VjoCmapSource src;
    /* the text is nothing like what was recognized: no line is placed, no block */
    result_text(buf, "まったく別の文", "まったく別の文");
    src = line_of(&l0, "ABCDEFG", 0, 10, 0, 20);
    TEST_CHECK(vjo_cmap_build(buf, CAP, 1, &src, 1, 960, 544, &scratch) == 0);
    TEST_CHECK(buf[strlen(buf) + 1 + strlen(buf + strlen(buf) + 1) + 1] == 0);
}

static void test_transforms_and_hits(void)
{
    char buf[CAP], shell[CAP];
    Line l0;
    VjoCmapSource src;
    VjoCmap m;
    VjoCmapRect r[2];
    size_t len;
    /* a PSP frame (480 x 272, shown 2x on the Vita's screen): screen px are twice the frame's */
    result_text(buf, "あいう", "あいう");
    src = line_of(&l0, "あいう", 40, 12, 200, 216);
    TEST_ASSERT(vjo_cmap_build(buf, CAP, 1, &src, 1, 480, 272, &scratch) > 0);
    len = shell_sentence(buf, 1, shell);
    TEST_ASSERT(vjo_cmap_carry(shell, CAP, len, buf, CAP) > 0);
    TEST_ASSERT(vjo_cmap_find(shell, CAP, len, &m));
    TEST_CHECK(vjo_cmap_sx(&m, 40) == 80 && vjo_cmap_sy(&m, 200) == 400);
    TEST_CHECK(vjo_cmap_hit(&m, 80, 400) == 0);
    TEST_CHECK(vjo_cmap_hit(&m, 80 + 24, 431) == 1);
    TEST_CHECK(vjo_cmap_hit(&m, 80 + 24, 432) == -1); /* below the line */
    TEST_CHECK(vjo_cmap_hit(&m, 79, 410) == -1);
    TEST_CHECK(vjo_cmap_span(&m, 3, 9, r, 2) == 1);
    TEST_CHECK(r[0].x == 104 && r[0].w == 48 && r[0].y == 400 && r[0].h == 32);
    /* an Adrenaline frame is the Vita's screen (960 x 544): the boxes are the screen's own */
    result_text(buf, "あいう", "あいう");
    TEST_ASSERT(vjo_cmap_build(buf, CAP, 1, &src, 1, 960, 544, &scratch) > 0);
    len = shell_sentence(buf, 1, shell);
    TEST_ASSERT(vjo_cmap_carry(shell, CAP, len, buf, CAP) > 0);
    TEST_ASSERT(vjo_cmap_find(shell, CAP, len, &m));
    TEST_CHECK(vjo_cmap_sx(&m, 40) == 40 && vjo_cmap_hit(&m, 41, 201) == 0);
    /* a game rendering 720 x 408, scaled to the screen */
    result_text(buf, "あいう", "あいう");
    TEST_ASSERT(vjo_cmap_build(buf, CAP, 1, &src, 1, 720, 408, &scratch) > 0);
    len = shell_sentence(buf, 1, shell);
    TEST_ASSERT(vjo_cmap_carry(shell, CAP, len, buf, CAP) > 0);
    TEST_ASSERT(vjo_cmap_find(shell, CAP, len, &m));
    TEST_CHECK(vjo_cmap_sx(&m, 720) == 960 && vjo_cmap_sy(&m, 408) == 544 && vjo_cmap_sx(&m, 40) == 53);
}

static void test_spans_vertical_and_popup(void)
{
    char buf[CAP], shell[CAP];
    Line l0, l1;
    VjoCmapSource src[2];
    VjoCmap m;
    VjoCmapRect r[2], w;
    size_t len;
    result_text(buf, "あいうえ\nかきくけ", "あいうえ\nかきくけ");
    src[0] = line_of(&l0, "あいうえ", 100, 25, 380, 410);
    src[1] = line_of(&l1, "かきくけ", 100, 25, 420, 450);
    TEST_ASSERT(vjo_cmap_build(buf, CAP, 1, src, 2, 960, 544, &scratch) > 0);
    len = shell_sentence(buf, 1, shell);
    TEST_ASSERT(vjo_cmap_carry(shell, CAP, len, buf, CAP) > 0);
    TEST_ASSERT(vjo_cmap_find(shell, CAP, len, &m));
    /* a word from え across the line break to か: two rectangles */
    TEST_CHECK(vjo_cmap_span(&m, 9, 16, r, 2) == 2);
    TEST_CHECK(r[0].x == 175 && r[0].w == 25 && r[0].y == 380 && r[1].x == 100 && r[1].w == 25 && r[1].y == 420);
    TEST_CHECK(vjo_cmap_span(&m, 9, 16, r, 1) == 1); /* at most max */
    /* Up / Down: the nearest character on the line above or below */
    TEST_CHECK(vjo_cmap_vertical(&m, 1, 1) == 5);  /* い -> き */
    TEST_CHECK(vjo_cmap_vertical(&m, 6, -1) == 2); /* く -> う */
    TEST_CHECK(vjo_cmap_vertical(&m, 1, -1) == -1 && vjo_cmap_vertical(&m, 5, 1) == -1);
    /* the popup goes below a word with room under it, else above, else at the far edge */
    w.x = 100; w.y = 100; w.w = 50; w.h = 30;
    TEST_CHECK(vjo_cmap_popup_top(&w, 196, 6) == 136);
    w.y = 380;
    TEST_CHECK(vjo_cmap_popup_top(&w, 196, 6) == 380 - 6 - 196);
    w.y = 150; w.h = 300;
    TEST_CHECK(vjo_cmap_popup_top(&w, 196, 6) == 544 - 196 || vjo_cmap_popup_top(&w, 196, 6) == 0);
}

static void test_corrupt_blocks_are_refused(void)
{
    char buf[CAP], shell[CAP];
    Line l0;
    VjoCmapSource src;
    VjoCmap m;
    size_t len, n;
    result_text(buf, "あいう", "あいう");
    src = line_of(&l0, "あいう", 0, 10, 0, 20);
    n = vjo_cmap_build(buf, CAP, 1, &src, 1, 960, 544, &scratch);
    TEST_ASSERT(n > 0);
    len = shell_sentence(buf, 1, shell);
    TEST_ASSERT(vjo_cmap_carry(shell, CAP, len, buf, CAP) == n);
    /* every single byte flipped: refused, or still a block whose every entry stays inside the sentence */
    for (size_t i = 0; i < n; i++)
        for (int bit = 0; bit < 8; bit++) {
            char copy[CAP];
            memcpy(copy, shell, CAP);
            copy[len + 1 + i] ^= (char)(1 << bit);
            if (vjo_cmap_find(copy, CAP, len, &m)) {
                for (unsigned k = 0; k < m.chars; k++) {
                    uint32_t off;
                    unsigned line, x0, x1;
                    vjo_cmap_get(&m, k, &off, &line, &x0, &x1);
                    TEST_CHECK(off < len && line < m.lines && x0 <= x1);
                }
            }
        }
    /* a block that would run past the buffer is refused */
    TEST_CHECK(!vjo_cmap_find(shell, len + 1 + n - 1, len, &m));
    /* too little room in the result: no block written; no scratch: none either */
    result_text(buf, "あいう", "あいう");
    TEST_CHECK(vjo_cmap_build(buf, 30, 1, &src, 1, 960, 544, &scratch) == 0);
    TEST_CHECK(vjo_cmap_build(buf, CAP, 1, &src, 1, 960, 544, NULL) == 0);
}

TEST_LIST = {
    {"sentence_matches_the_shell", test_sentence_matches_the_shell},
    {"build_and_read_back", test_build_and_read_back},
    {"lm_edits_and_inserted_characters", test_lm_edits_and_inserted_characters},
    {"unmatched_lines_get_no_boxes", test_unmatched_lines_get_no_boxes},
    {"transforms_and_hits", test_transforms_and_hits},
    {"spans_vertical_and_popup", test_spans_vertical_and_popup},
    {"corrupt_blocks_are_refused", test_corrupt_blocks_are_refused},
    {NULL, NULL}};
