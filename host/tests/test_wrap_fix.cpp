/* The re-wrap of the overlay's paf texts (shell/wrap_fix.h, the owner's
 * 2026-10-16 item 4: never break inside a word) on a model of paf's layout:
 * greedy lines in a box, Japanese broken at any character, Latin at its last
 * space, '\n' a hard break, a layout that can be missing or one SetString
 * behind. It shows what the overlay's header, popup and strip get: every line
 * paf started inside a word is started earlier, at a place core/linebreak.h
 * allows, and a word wider than the box keeps paf's break. The real layout is
 * the console's (its glyph widths); this is not a substitute for it. Also the
 * header's unit map and vjo_styled_insert's spans. */
#include <string.h>
#include <vector>
#include "acutest.h"
#include "../../shell/wrap_fix.h"
extern "C" {
#include "styled.h"
#include "utf.h"
}

struct Info {
    float x;
    uint32_t line;
};

struct Model {
    std::vector<uint16_t> text;
    std::vector<uint32_t> line;
    std::vector<float> x;
    float box = 16 * 27.0f;
    int not_ready = 0;          /* calls to answer with "not laid out" */
    bool lag = false;           /* the layout is the previous text's */
    std::vector<uint32_t> old_line;
    int queries = 0;

    static float width(uint16_t u) { return u < 0x80 ? 15.0f : 27.0f; }
    void set(const uint16_t *u, uint32_t n)
    {
        old_line = line;
        text.assign(u, u + n);
        line.assign(n, 0);
        x.assign(n, 0.0f);
        uint32_t l = 0, start = 0; /* this line's first unit */
        float pos = 0.0f;
        for (uint32_t i = 0; i < n; i++) {
            if (text[i] != '\n' && pos + width(text[i]) > box && pos > 0.0f) {
                uint32_t b = i;
                if (text[i] < 0x80 && text[i] != ' ') /* a Latin word moves down whole, from its space */
                    for (uint32_t k = i; k > start; k--)
                        if (text[k - 1] == ' ') {
                            b = k;
                            break;
                        }
                l++;
                pos = 0.0f;
                for (uint32_t k = b; k < i; k++) {
                    line[k] = l;
                    x[k] = pos;
                    pos += width(text[k]);
                }
                start = b;
            }
            line[i] = l;
            x[i] = pos;
            if (text[i] == '\n') {
                l++;
                pos = 0.0f;
                start = i + 1;
            } else
                pos += width(text[i]);
        }
    }
    uint32_t lines() const { return text.empty() ? 0 : line.back() + 1 + (text.back() == '\n'); }
    int GetLineCount(uint32_t &n)
    {
        if (not_ready > 0) {
            not_ready--;
            return -1;
        }
        n = lag && !old_line.empty() ? old_line.back() + 1 : lines();
        return 0;
    }
    int GetCharInfo(uint32_t i, Info &ci)
    {
        const std::vector<uint32_t> &l = lag && !old_line.empty() ? old_line : line;
        queries++;
        if (i >= l.size())
            return -1;
        ci.line = l[i];
        ci.x = x.size() > i ? x[i] : 0.0f;
        return 0;
    }
};

static std::vector<uint16_t> u16(const char *s)
{
    std::vector<uint16_t> u;
    size_t len = strlen(s), i = 0;
    while (i < len) {
        uint32_t c = vjo_utf8_next(s, len, &i);
        if (c >= 0x10000) {
            u.push_back((uint16_t)(0xD800 | ((c - 0x10000) >> 10)));
            u.push_back((uint16_t)(0xDC00 | ((c - 0x10000) & 0x3FF)));
        } else
            u.push_back((uint16_t)c);
    }
    return u;
}

/* Whether a soft line of the model starts where core/linebreak.h allows (or a hard break, or a space). */
static bool line_start_allowed(const std::vector<uint16_t> &u, uint32_t s)
{
    if (u[s - 1] == '\n' || u[s] == ' ')
        return true;
    return vjo_break_last16(u.data(), (uint32_t)u.size(), s - 1, s) == s || /* the rule allows s */
           false;
}

/* Runs the overlay's loop: settle, fix, put the '\n' in, set again; returns the breaks put in. */
static int rewrap(Model &m, std::vector<uint16_t> &u, int frames = 200)
{
    VjoWrap w;
    int put = 0;
    m.set(u.data(), (uint32_t)u.size());
    vjo_wrap_arm(&w);
    while (frames-- > 0) {
        uint32_t b = vjo_wrap_step<Info>(&w, &m, u.data(), (uint32_t)u.size(), 64);
        if (!b)
            continue;
        u.insert(u.begin() + b, (uint16_t)'\n');
        vjo_wrap_put(&w, b);
        m.set(u.data(), (uint32_t)u.size());
        put++;
    }
    return put;
}

static void soft_lines_move_to_word_starts(void)
{
    static const struct { const char *text; int breaks; } cases[] = {
        /* paf breaks after 16 characters, inside 食べる: the line ends before 食べる instead */
        {"あいうえおかきくけこさしすせそ食べるかなと思った", 1},
        /* the recognized lines, each wider than the box: two fixes (…図書館で|本を, …私は|明日も) */
        {"彼女は学校へ行ったあと図書館で本を読んでいた\n今日は雨だったので家にいた私は明日も晴れると思う", 2},
        /* Latin: paf already breaks at spaces */
        {"The language used by the Japanese people since ancient times and the official", 0},
        /* a katakana word wider than the box keeps paf's break */
        {"エンサイクロペディアブリタニカエンサイクロペディア", 0}};
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        Model m;
        std::vector<uint16_t> u = u16(cases[c].text);
        TEST_CASE(cases[c].text);
        TEST_CHECK(rewrap(m, u) == cases[c].breaks);
        /* every soft line now starts where a line may break, but the one long word's */
        for (uint32_t i = 1; i < u.size(); i++)
            if (m.line[i] > m.line[i - 1] && u[i - 1] != '\n' && c != 3)
                TEST_CHECK_(line_start_allowed(u, i), "line at %u", i);
    }
    /* the first case's lines: the box's 16, then 食べるかなと思った */
    {
        Model m;
        std::vector<uint16_t> u = u16("あいうえおかきくけこさしすせそ食べるかなと思った");
        rewrap(m, u);
        TEST_CHECK(u[15] == '\n' && u[16] == 0x98DF); /* 食 starts the second line */
    }
}

/* The layout lagging a SetString, or missing for a few frames, is waited for. */
static void stale_or_missing_layouts_are_waited_for(void)
{
    Model m;
    VjoWrap w;
    std::vector<uint16_t> u = u16("あいうえおかきくけこさしすせそ食べるかなと思った");
    m.set(u.data(), (uint32_t)u.size());
    vjo_wrap_arm(&w);
    m.not_ready = 3;
    uint32_t b = 0;
    for (int f = 0; f < 6 && !b; f++)
        b = vjo_wrap_step<Info>(&w, &m, u.data(), (uint32_t)u.size(), 64);
    TEST_CHECK(b == 15);
    u.insert(u.begin() + b, (uint16_t)'\n');
    vjo_wrap_put(&w, b);
    m.set(u.data(), (uint32_t)u.size());
    m.lag = true; /* still the text before the '\n': one unit short */
    TEST_CHECK(vjo_wrap_fix<Info>(&m, u.data(), (uint32_t)u.size()) == VJO_WRAP_WAIT);
    m.lag = false;
    TEST_CHECK(vjo_wrap_fix<Info>(&m, u.data(), (uint32_t)u.size()) == 0 && w.last == 15);
    /* a layout that never comes: the step gives up after VJO_WRAP_TRIES frames */
    Model never;
    never.set(u.data(), (uint32_t)u.size());
    never.not_ready = 1000;
    vjo_wrap_arm(&w);
    int f = 0;
    while (w.frames && f < 100) {
        TEST_CHECK(vjo_wrap_step<Info>(&w, &never, u.data(), (uint32_t)u.size(), 64) == 0);
        f++;
    }
    TEST_CHECK(!w.frames && f == VJO_WRAP_SETTLE + VJO_WRAP_TRIES);
    /* a text that needs a break: none without room, none once VJO_WRAP_BREAKS are in */
    std::vector<uint16_t> v = u16("あいうえおかきくけこさしすせそ食べるかなと思った");
    Model fresh;
    fresh.set(v.data(), (uint32_t)v.size());
    vjo_wrap_arm(&w);
    w.frames = 1;
    TEST_CHECK(vjo_wrap_step<Info>(&w, &fresh, v.data(), (uint32_t)v.size(), 0) == 0);
    vjo_wrap_arm(&w);
    w.frames = 1;
    w.count = VJO_WRAP_BREAKS;
    TEST_CHECK(vjo_wrap_step<Info>(&w, &fresh, v.data(), (uint32_t)v.size(), 64) == 0);
    vjo_wrap_arm(&w);
    w.frames = 1;
    TEST_CHECK(vjo_wrap_step<Info>(&w, &fresh, v.data(), (uint32_t)v.size(), 64) == 15);
}

static void header_units_map_to_the_shown_ones(void)
{
    uint32_t brk[VJO_WRAP_BREAKS];
    int n = 0;
    /* breaks put in before shown units 10, then 4, then 20 (shown after the first two) */
    vjo_wrap_add(brk, &n, 10);
    vjo_wrap_add(brk, &n, 4);
    vjo_wrap_add(brk, &n, 20);
    /* as the text's units: before 4, 10 and 18 (shown: 0-3, \n, 4-9, \n, 10-17, \n, 18) */
    TEST_CHECK(n == 3 && brk[0] == 4 && brk[1] == 10 && brk[2] == 18);
    TEST_CHECK(vjo_wrap_shown(brk, n, 0) == 0 && vjo_wrap_shown(brk, n, 3) == 3);
    TEST_CHECK(vjo_wrap_shown(brk, n, 4) == 5 && vjo_wrap_shown(brk, n, 9) == 10);
    TEST_CHECK(vjo_wrap_shown(brk, n, 10) == 12 && vjo_wrap_shown(brk, n, 17) == 19);
    TEST_CHECK(vjo_wrap_shown(brk, n, 18) == 21);
    TEST_CHECK(vjo_wrap_shown(brk, 0, 18) == 18);
    /* one before the last break (shown 19 is text unit 17, before the break at 18) */
    vjo_wrap_add(brk, &n, 19);
    TEST_CHECK(n == 4 && brk[0] == 4 && brk[1] == 10 && brk[2] == 17 && brk[3] == 18);
    TEST_CHECK(vjo_wrap_shown(brk, n, 16) == 18 && vjo_wrap_shown(brk, n, 17) == 20 && vjo_wrap_shown(brk, n, 18) == 22);
}

static void styled_insert_keeps_the_spans(void)
{
    static uint8_t mem[4096];
    VjoArena a;
    VjoStyled st;
    vjo_arena_init(&a, mem, sizeof(mem));
    TEST_ASSERT(vjo_styled_init(&st, &a, 12, 4) == 0);
    vjo_styled_puts(&st, "ab", 1, 10);   /* [0, 2) */
    vjo_styled_puts(&st, "cdef", 2, 10); /* [2, 6) */
    vjo_styled_puts(&st, "g", 3, 10);    /* [6, 7) */
    TEST_CHECK(vjo_styled_insert(&st, 4, '\n'));  /* inside "cdef" */
    TEST_CHECK(st.len == 8 && st.text[4] == '\n' && st.text[5] == 'e' && st.text[8] == 0);
    TEST_CHECK(st.spans[0].start == 0 && st.spans[0].len == 2 && st.spans[1].start == 2 && st.spans[1].len == 5);
    TEST_CHECK(st.spans[2].start == 7 && st.spans[2].len == 1);
    TEST_CHECK(vjo_styled_insert(&st, 2, '\n'));  /* at a span's start: the span before it takes it */
    TEST_CHECK(st.spans[0].len == 3 && st.spans[1].start == 3 && st.spans[2].start == 8);
    TEST_CHECK(!vjo_styled_insert(&st, 0, '\n') && !vjo_styled_insert(&st, st.len + 1, '\n'));
    while (st.len < st.cap)
        TEST_CHECK(vjo_styled_insert(&st, st.len, ' '));
    TEST_CHECK(!vjo_styled_insert(&st, 1, '\n') && st.len == 12);
}

TEST_LIST = {
    {"soft_lines_move_to_word_starts", soft_lines_move_to_word_starts},
    {"stale_or_missing_layouts_are_waited_for", stale_or_missing_layouts_are_waited_for},
    {"header_units_map_to_the_shown_ones", header_units_map_to_the_shown_ones},
    {"styled_insert_keeps_the_spans", styled_insert_keeps_the_spans},
    {NULL, NULL},
};
