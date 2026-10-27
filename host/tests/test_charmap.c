/* The char map (core/charmap.h, core/charmap_build.h; API 46, docs/overlay-on-text.md, #309): the block's format,
 * the game worker's writer against the Shell's sentence (core/textfilter.c + vjo_java_trim), the hand-over into
 * the Shell's text buffer, the frame-to-screen mapping (a 960 x 544 frame, PSP's 480 x 272 at 2x, a 720 x 408
 * frame), a hook's line the game wrapped over several rows, hit-testing with its slack, a word's rectangles, the
 * lines' underlines, Up/Down across lines and the popup's place. No model, no file. */
#include "acutest.h"

#include <stdio.h>
#include <string.h>

#include "../../core/arena.h"
#include "../../core/charmap.h"
#include "../../core/charmap_build.h"
#include "../../core/textfilter.h"
#include "../../core/utf.h"

#define CAP 4096

static VjoCmapScratch scratch; /* the worker's job block lends it (game_ocr/main.c) */

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
    memset(out, 0, CAP);
    strcpy(out, t);
    return strlen(t);
}

/* The worker's text, its map built (src lines from a fb_w x fb_h frame, to the screen), and the Shell's copy. */
static int shell_map(char *buf, char *shell, const VjoCmapSource *src, unsigned n, unsigned fb_w, unsigned fb_h,
                     VjoCmap *m)
{
    size_t len;
    if (!vjo_cmap_build(buf, CAP, 1, src, n, fb_w, fb_h, VJO_SCREEN_W, VJO_SCREEN_H, &scratch)) return 0;
    len = shell_sentence(buf, 1, shell);
    return vjo_cmap_carry(shell, CAP, len, buf, CAP) > 0 && vjo_cmap_find(shell, CAP, len, m);
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
    /* a name line (a kanji name), a dialogue line, a UI line the filter drops */
    result_text(buf, "真奈\n「そうだね」\nAuto", "真奈\n「そうだね」\nAuto");
    src[0] = line_of(&l0, "真奈", 100, 20, 400, 424);
    src[1] = line_of(&l1, "「そうだね」", 100, 22, 430, 456);
    n = vjo_cmap_build(buf, CAP, 1, src, 2, 960, 544, VJO_SCREEN_W, VJO_SCREEN_H, &scratch);
    TEST_CHECK(n == VJO_CMAP_HEADER + 6u * 8u + 8u * 2u);
    len = shell_sentence(buf, 1, shell);
    TEST_CHECK(strcmp(shell, "真奈\n「そうだね」") == 0);
    /* the Shell takes it after its own sentence */
    TEST_CHECK(vjo_cmap_carry(shell, CAP, len, buf, CAP) == n);
    TEST_ASSERT(vjo_cmap_find(shell, CAP, len, &m));
    TEST_CHECK(m.lines == 2 && m.chars == 2 + 6 && vjo_cmap_bytes(&m) == n);
    {
        uint32_t off;
        unsigned line, x0, x1, y0, y1, first, count;
        vjo_cmap_get(&m, 0, &off, &line, &x0, &x1);
        TEST_CHECK(off == 0 && line == 0 && x0 == 100 && x1 == 120);
        vjo_cmap_get(&m, 2, &off, &line, &x0, &x1); /* 「: after "真奈\n" (3 + 3 + 1 bytes) */
        TEST_CHECK(off == 7 && line == 1 && x0 == 100 && x1 == 122);
        vjo_cmap_line(&m, 1, &y0, &y1, &first, &count);
        TEST_CHECK(y0 == 430 && y1 == 456 && first == 2 && count == 6);
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
    /* the recognizer read そうだね; the LM made it そーだね; an inserted character (a hook's 、) gets a
     * zero-width cell after its neighbour */
    result_text(buf, "そーだ、ね", "そうだね");
    src = line_of(&l0, "そうだね", 0, 30, 10, 40);
    TEST_ASSERT(shell_map(buf, shell, &src, 1, 960, 544, &m));
    TEST_CHECK(m.chars == 5 && m.lines == 1);
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
    /* a zero-width cell holds no tap: the tap on its place is ね's */
    TEST_CHECK(vjo_cmap_hit(&m, 90, 20) == 4);
    /* an inserted character first: zero width at the next one's left edge, on its row */
    result_text(buf, "…そうだ", "そうだ");
    src = line_of(&l0, "そうだ", 40, 30, 10, 40);
    TEST_ASSERT(shell_map(buf, shell, &src, 1, 960, 544, &m));
    {
        uint32_t off;
        unsigned line, x0, x1;
        vjo_cmap_get(&m, 0, &off, &line, &x0, &x1);
        TEST_CHECK(m.chars == 4 && off == 0 && line == 0 && x0 == 40 && x1 == 40);
    }
}

static void test_unmatched_lines_get_no_boxes(void)
{
    char buf[CAP];
    Line l0;
    VjoCmapSource src;
    /* the text is nothing like what was recognized: no line is placed, no block */
    result_text(buf, "まったく別の文", "まったく別の文");
    src = line_of(&l0, "ABCDEFG", 0, 10, 0, 20);
    TEST_CHECK(vjo_cmap_build(buf, CAP, 1, &src, 1, 960, 544, VJO_SCREEN_W, VJO_SCREEN_H, &scratch) == 0);
    TEST_CHECK(buf[strlen(buf) + 1 + strlen(buf + strlen(buf) + 1) + 1] == 0);
}

/* A hook's text has no line breaks where the game wraps it: one text line over three recognized rows. Each
 * character is on the row it was read on, a line of the block each; a row OCR misread in part still holds the
 * characters that align. */
static void test_a_hook_line_wrapped_over_rows(void)
{
    char buf[CAP], shell[CAP];
    Line l0, l1, l2;
    VjoCmapSource src[3];
    VjoCmap m;
    VjoCmapRect r[4];
    const char *hook = "今日はいい天気だから、一緒に海まで歩いて行こうよ。";
    result_text(buf, hook, "今日はいい天気だから、\n一緒に海まで歩いて\n行こうよ。");
    src[0] = line_of(&l0, "今日はいい天気だから、", 60, 30, 380, 410);
    src[1] = line_of(&l1, "一緒に海まで歩いて", 60, 30, 416, 446);
    src[2] = line_of(&l2, "行こうよ。", 60, 30, 452, 482);
    TEST_ASSERT(shell_map(buf, shell, src, 3, 960, 544, &m));
    TEST_CHECK_(m.lines == 3 && m.chars == 25, "%u lines, %u characters", m.lines, m.chars);
    {
        uint32_t off;
        unsigned line, x0, x1;
        vjo_cmap_get(&m, 11, &off, &line, &x0, &x1); /* 一: the second row's first */
        TEST_CHECK(line == 1 && x0 == 60 && x1 == 90 && off == 33);
        vjo_cmap_get(&m, 24, &off, &line, &x0, &x1); /* 。 */
        TEST_CHECK(line == 2 && x0 == 180 && x1 == 210);
    }
    /* the lines' underlines: a rectangle a row */
    TEST_CHECK(vjo_cmap_span(&m, 0, (uint32_t)strlen(shell), r, 4) == 3);
    TEST_CHECK(r[0].x == 60 && r[0].w == 330 && r[0].y == 380 && r[0].h == 30);
    TEST_CHECK(r[2].x == 60 && r[2].w == 150 && r[2].y == 452);
    /* a word over the wrap (歩いて行こう: the second row's last three, the third's first three) */
    TEST_CHECK(vjo_cmap_span(&m, (uint32_t)(strstr(shell, "歩") - shell), (uint32_t)(strstr(shell, "よ") - shell), r,
                             2) == 2);
    TEST_CHECK(r[0].x == 240 && r[0].w == 90 && r[0].y == 416 && r[1].x == 60 && r[1].w == 90 && r[1].y == 452);
    /* OCR misread part of the second row: its other characters still sit there */
    src[1] = line_of(&l1, "一諸に海までルいて", 60, 30, 416, 446);
    result_text(buf, hook, "x");
    TEST_ASSERT(shell_map(buf, shell, src, 3, 960, 544, &m));
    TEST_CHECK(m.lines == 3 && m.chars == 25);
    /* a hook's text that is half someone else's: not placed */
    result_text(buf, "全然違う文章がここに入っていて海まで歩く", "x");
    TEST_CHECK(vjo_cmap_build(buf, CAP, 1, src, 3, 960, 544, VJO_SCREEN_W, VJO_SCREEN_H, &scratch) == 0);
}

static void test_frames_are_mapped_to_the_screen_and_hit(void)
{
    char buf[CAP], shell[CAP];
    Line l0;
    VjoCmapSource src;
    VjoCmap m;
    VjoCmapRect r[2];
    /* a PSP frame (480 x 272, shown 2x on the Vita's screen): the block is in screen px */
    result_text(buf, "あいう", "あいう");
    src = line_of(&l0, "あいう", 40, 12, 200, 216);
    TEST_ASSERT(shell_map(buf, shell, &src, 1, 480, 272, &m));
    TEST_CHECK(vjo_cmap_hit(&m, 80, 400) == 0);
    TEST_CHECK(vjo_cmap_hit(&m, 80 + 24, 431) == 1);
    TEST_CHECK(vjo_cmap_span(&m, 3, 9, r, 2) == 1);
    TEST_CHECK(r[0].x == 104 && r[0].w == 48 && r[0].y == 400 && r[0].h == 32);
    /* a tap just off the text still takes the nearest character, within VJO_CMAP_SLACK; further, nothing */
    TEST_CHECK(vjo_cmap_hit(&m, 80 + 24, 432 + 4) == 1);  /* 5 px below the row */
    TEST_CHECK(vjo_cmap_hit(&m, 79 - 6, 410) == 0);       /* 7 px left of the first cell */
    TEST_CHECK(vjo_cmap_hit(&m, 80 + 24, 432 + 12) == -1); /* 13 px below */
    TEST_CHECK(vjo_cmap_hit(&m, 80 - 13, 410) == -1);
    TEST_CHECK(vjo_cmap_hit(&m, 80 - 8, 400 - 8) == -1);  /* 8 + 8 px off a corner */
    /* an Adrenaline frame is the Vita's screen (960 x 544): the boxes are the frame's own */
    result_text(buf, "あいう", "あいう");
    TEST_ASSERT(shell_map(buf, shell, &src, 1, 960, 544, &m));
    TEST_CHECK(vjo_cmap_hit(&m, 41, 201) == 0 && vjo_cmap_span(&m, 0, 9, r, 2) == 1 && r[0].x == 40 && r[0].w == 36);
    /* a game rendering 720 x 408, scaled to the screen (x 4/3) */
    result_text(buf, "あいう", "あいう");
    TEST_ASSERT(shell_map(buf, shell, &src, 1, 720, 408, &m));
    TEST_CHECK(vjo_cmap_span(&m, 0, 9, r, 2) == 1 && r[0].x == 53 && r[0].w == 48 && r[0].y == 267 && r[0].h == 21);
}

static void test_spans_vertical_and_popup(void)
{
    char buf[CAP], shell[CAP];
    Line l0, l1;
    VjoCmapSource src[2];
    VjoCmap m;
    VjoCmapRect r[2], w;
    result_text(buf, "あいうえ\nかきくけ", "あいうえ\nかきくけ");
    src[0] = line_of(&l0, "あいうえ", 100, 25, 380, 410);
    src[1] = line_of(&l1, "かきくけ", 100, 25, 420, 450);
    TEST_ASSERT(shell_map(buf, shell, src, 2, 960, 544, &m));
    /* a word from え across the line break to か: two rectangles */
    TEST_CHECK(vjo_cmap_span(&m, 9, 16, r, 2) == 2);
    TEST_CHECK(r[0].x == 175 && r[0].w == 25 && r[0].y == 380 && r[1].x == 100 && r[1].w == 25 && r[1].y == 420);
    TEST_CHECK(vjo_cmap_span(&m, 9, 16, r, 1) == 1); /* at most max */
    /* Up / Down: the nearest character on the line above or below */
    TEST_CHECK(vjo_cmap_vertical(&m, 1, 1) == 5);  /* い -> き */
    TEST_CHECK(vjo_cmap_vertical(&m, 6, -1) == 2); /* く -> う */
    TEST_CHECK(vjo_cmap_vertical(&m, 1, -1) == -1 && vjo_cmap_vertical(&m, 5, 1) == -1);
    TEST_CHECK(vjo_cmap_vertical(&m, -1, 1) == -1 && vjo_cmap_vertical(&m, 1, 0) == -1);
    /* the popup goes where it covers none of the game's text: here (rows 380-450) above it all */
    w.x = 100; w.y = 380; w.w = 50; w.h = 30;
    TEST_CHECK(vjo_cmap_popup_top(&m, &w, 196, 6) == 380 - 6 - 196);
    /* without a map, or text over most of the screen: below a word with room under it, else above, else at the
     * far edge */
    {
        VjoCmap none = {0};
        w.y = 100;
        TEST_CHECK(vjo_cmap_popup_top(&none, &w, 196, 6) == 136);
        w.y = 380;
        TEST_CHECK(vjo_cmap_popup_top(&none, &w, 196, 6) == 380 - 6 - 196);
        w.y = 150; w.h = 300; /* no room either side, its middle in the lower half: the top edge */
        TEST_CHECK(vjo_cmap_popup_top(&none, &w, 196, 6) == 0);
        w.y = 100; w.h = 300; /* in the upper half: the bottom edge */
        TEST_CHECK(vjo_cmap_popup_top(&none, &w, 196, 6) == 544 - 196);
    }
    /* text at the top (rows 40-110): the popup below it all, not just below the word on its first line */
    result_text(buf, "あいうえ\nかきくけ", "あいうえ\nかきくけ");
    src[0] = line_of(&l0, "あいうえ", 100, 25, 40, 70);
    src[1] = line_of(&l1, "かきくけ", 100, 25, 80, 110);
    TEST_ASSERT(shell_map(buf, shell, src, 2, 960, 544, &m));
    w.x = 100; w.y = 40; w.w = 25; w.h = 30;
    TEST_CHECK(vjo_cmap_popup_top(&m, &w, 196, 6) == 116);
    /* text from the top to the foot of the screen: beside the word */
    src[1] = line_of(&l1, "かきくけ", 100, 25, 480, 510);
    TEST_ASSERT(shell_map(buf, shell, src, 2, 960, 544, &m));
    TEST_CHECK(vjo_cmap_popup_top(&m, &w, 196, 6) == 76);
}

static void test_corrupt_blocks_are_refused(void)
{
    char buf[CAP], shell[CAP];
    Line l0, l1;
    VjoCmapSource src[2];
    VjoCmap m;
    size_t len, n;
    result_text(buf, "あいう\nかき", "あいう\nかき");
    src[0] = line_of(&l0, "あいう", 0, 10, 0, 20);
    src[1] = line_of(&l1, "かき", 0, 10, 30, 50);
    n = vjo_cmap_build(buf, CAP, 1, src, 2, 960, 544, VJO_SCREEN_W, VJO_SCREEN_H, &scratch);
    TEST_ASSERT(n > 0);
    len = shell_sentence(buf, 1, shell);
    TEST_ASSERT(vjo_cmap_carry(shell, CAP, len, buf, CAP) == n);
    /* every single bit flipped: refused, or still a block whose every entry stays inside the sentence and its
     * lines tile its characters */
    for (size_t i = 0; i < n; i++)
        for (int bit = 0; bit < 8; bit++) {
            char copy[CAP];
            memcpy(copy, shell, CAP);
            copy[len + 1 + i] ^= (char)(1 << bit);
            if (vjo_cmap_find(copy, CAP, len, &m)) {
                unsigned total = 0;
                for (unsigned k = 0; k < m.chars; k++) {
                    uint32_t off;
                    unsigned line, x0, x1;
                    vjo_cmap_get(&m, k, &off, &line, &x0, &x1);
                    TEST_CHECK(off < len && line < m.lines && x0 <= x1);
                }
                for (unsigned l = 0; l < m.lines; l++) {
                    unsigned y0, y1, first, count;
                    vjo_cmap_line(&m, l, &y0, &y1, &first, &count);
                    TEST_CHECK(first == total && count && y0 <= y1);
                    total += count;
                }
                TEST_CHECK(total == m.chars);
            }
        }
    /* a block that would run past the buffer is refused */
    TEST_CHECK(!vjo_cmap_find(shell, len + 1 + n - 1, len, &m));
    /* too little room in the result: no block written; no scratch, no frame: none either */
    result_text(buf, "あいう", "あいう");
    TEST_CHECK(vjo_cmap_build(buf, 30, 1, src, 1, 960, 544, VJO_SCREEN_W, VJO_SCREEN_H, &scratch) == 0);
    TEST_CHECK(vjo_cmap_build(buf, CAP, 1, src, 1, 960, 544, VJO_SCREEN_W, VJO_SCREEN_H, NULL) == 0);
    TEST_CHECK(vjo_cmap_build(buf, CAP, 1, src, 1, 0, 544, VJO_SCREEN_W, VJO_SCREEN_H, &scratch) == 0);
}

/* The writer's own limits: a character after the last one that fits, a line past VJO_CMAP_LINES, a line that got
 * no character (dropped), offsets that do not increase (the reader refuses them). */
static void test_writer_limits(void)
{
    char buf[CAP], s[CAP];
    VjoCmapWriter w;
    VjoCmap m;
    memset(buf, 0, CAP);
    memset(s, 0, CAP);
    strcpy(s, "あいう");
    TEST_ASSERT(vjo_cmap_begin(&w, buf, CAP, 0, 9, vjo_cmap_fnv(s, 9), 960, 544) == 0);
    TEST_CHECK(vjo_cmap_add_char(&w, 0, 0, 10) < 0); /* before any line */
    w.failed = 0;
    TEST_CHECK(vjo_cmap_add_line(&w, 0, 20) == 0 && vjo_cmap_add_line(&w, 30, 50) == 0); /* the first stays empty */
    TEST_CHECK(vjo_cmap_add_char(&w, 0, 0, 10) == 0 && vjo_cmap_add_char(&w, 3, 10, 20) == 0);
    TEST_CHECK(vjo_cmap_add_line(&w, 60, 80) == 0 && vjo_cmap_add_line(&w, 90, 99) == 0);
    TEST_CHECK(vjo_cmap_add_char(&w, 6, 0, 10) == 0);
    TEST_CHECK(vjo_cmap_end(&w) == VJO_CMAP_HEADER + 3u * 6u + 2u * 8u);
    memcpy(s + 10, buf, 64);
    TEST_ASSERT(vjo_cmap_find(s, CAP, 9, &m));
    {
        unsigned y0, y1, first, count;
        vjo_cmap_line(&m, 1, &y0, &y1, &first, &count);
        TEST_CHECK(m.lines == 2 && y0 == 90 && first == 2 && count == 1);
    }
    TEST_ASSERT(vjo_cmap_begin(&w, buf, CAP, 0, 9, vjo_cmap_fnv(s, 9), 960, 544) == 0);
    for (unsigned i = 0; i < VJO_CMAP_LINES; i++) {
        TEST_CHECK(vjo_cmap_add_line(&w, i, i + 1) == 0);
        TEST_CHECK(vjo_cmap_add_char(&w, i < 3 ? i * 3 : 9 + i, 0, 1) == 0);
    }
    TEST_CHECK(vjo_cmap_add_line(&w, 99, 100) < 0 && w.failed);
    TEST_CHECK(vjo_cmap_end(&w) == VJO_CMAP_HEADER + VJO_CMAP_LINES * 6u + VJO_CMAP_LINES * 8u);
    memcpy(s + 10, buf, 512);
    TEST_CHECK(!vjo_cmap_find(s, CAP, 9, &m)); /* offsets past the sentence */
    /* no character at all: no block */
    TEST_ASSERT(vjo_cmap_begin(&w, buf, CAP, 0, 9, 0, 960, 544) == 0);
    TEST_CHECK(vjo_cmap_add_line(&w, 0, 1) == 0 && vjo_cmap_end(&w) == 0 && buf[0] == 0);
}

TEST_LIST = {
    {"sentence_matches_the_shell", test_sentence_matches_the_shell},
    {"build_and_read_back", test_build_and_read_back},
    {"lm_edits_and_inserted_characters", test_lm_edits_and_inserted_characters},
    {"unmatched_lines_get_no_boxes", test_unmatched_lines_get_no_boxes},
    {"a_hook_line_wrapped_over_rows", test_a_hook_line_wrapped_over_rows},
    {"frames_are_mapped_to_the_screen_and_hit", test_frames_are_mapped_to_the_screen_and_hit},
    {"spans_vertical_and_popup", test_spans_vertical_and_popup},
    {"corrupt_blocks_are_refused", test_corrupt_blocks_are_refused},
    {"writer_limits", test_writer_limits},
    {NULL, NULL}};
