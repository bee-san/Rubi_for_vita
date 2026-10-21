/* The re-wrap of the overlay's paf texts (shell/wrap_fix.h, the owner's
 * 2026-10-16 item 4: break between phrases, not anywhere) on a model of paf's layout:
 * greedy lines in a box, Japanese broken at any character, Latin at its last
 * space, '\n' a hard break, a layout that can be missing or one SetString
 * behind. It shows what the overlay's header, popup and strip get: every line
 * paf started inside a word is started earlier, at a place core/linebreak.h
 * allows, and a word wider than the box keeps paf's break. The real layout is
 * the console's (its glyph widths); this is not a substitute for it. Also the
 * header's unit map and vjo_styled_insert's spans. */
#include <string.h>
#include <string>
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
    VjoWrap w = {}; /* keep 0: vjo_wrap_arm leaves it (audit-source-badge F2) */
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
    VjoWrap w = {}; /* keep 0 (audit-source-badge F2) */
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

/* The source badge before the current text (core/styled.c vjo_styled_history: "[HOOK]" or "[OCR]", the
 * owner's 2026-10-21 request): the re-wrap keeps its rules with it (every soft line starts where
 * core/linebreak.h allows), never breaks inside or right after the badge (VjoWrap.keep, as overlay.cpp sets
 * it), which stays whole at the first row's start with text after it, and the overlay's unit map
 * (overlay.cpp header_shown: the lead, then vjo_wrap_add's breaks) still finds each of the text's units, so
 * the cursor, taps and the highlight are on the text's characters. The first row has the badge's width less
 * room for the text: nothing else of the wrap changes. A text that opens with 「 may break before it
 * (core/linebreak.h): right after the badge that would leave the badge a row of its own, so that row keeps
 * paf's break, as a word wider than the box does. */
static void the_source_badge_keeps_the_wrap_rules(void)
{
    static const char *const texts[] = {
        "あいうえおかきくけこさしすせそ食べるかなと思った",
        "彼女は学校へ行ったあと図書館で本を読んでいた\n今日は雨だったので家にいた私は明日も晴れると思う",
        "また明日、一緒に見に行こう!", "エンサイクロペディアブリタニカエンサイクロペディア",
        "「あいうえおかきくけこさしすせそ」と言った"};
    /* the ASCII badges, and the icons (source_badge = icons: U+E1BB, U+E541), one unit each, which
     * core/linebreak.h takes as symbols, after which a line may break: keep alone holds them on the row */
    static const char *const badges[] = {"[HOOK]", "[OCR]", "\xEE\x86\xBB", "\xEE\x95\x81"};
    for (unsigned t = 0; t < sizeof(texts) / sizeof(texts[0]); t++)
        for (unsigned k = 0; k < sizeof(badges) / sizeof(badges[0]); k++) {
            std::string shown = std::string(badges[k]) + texts[t];
            std::vector<uint16_t> u = u16(shown.c_str()), text = u16(texts[t]), badge = u16(badges[k]);
            uint32_t lead = (uint32_t)badge.size(), brk[VJO_WRAP_BREAKS];
            int nbrk = 0;
            Model m;
            VjoWrap w = {};
            TEST_CASE(shown.c_str());
            m.set(u.data(), (uint32_t)u.size());
            vjo_wrap_arm(&w);
            w.keep = lead; /* overlay.cpp render: the current text's lead */
            for (int frames = 0; frames < 200; frames++) {
                uint32_t b = vjo_wrap_step<Info>(&w, &m, u.data(), (uint32_t)u.size(), 64);
                if (!b)
                    continue;
                TEST_CHECK_(b > lead, "a break at %u, in the badge's %u units or right after them", b, lead);
                u.insert(u.begin() + b, (uint16_t)'\n');
                vjo_wrap_put(&w, b);
                vjo_wrap_add(brk, &nbrk, b); /* as overlay.cpp wrap_frame records the header's */
                m.set(u.data(), (uint32_t)u.size());
            }
            for (uint32_t i = 0; i <= lead; i++) /* the badge, whole, and the text's first unit on the first row */
                TEST_CHECK(m.line[i] == 0 && (i == lead || u[i] == badge[i]));
            /* every soft line starts where a line may break, or keeps paf's break when its row has no such
             * place past the badge (one word wider than what is left of the box) */
            for (uint32_t i = lead + 1, row = 0; i < u.size(); i++)
                if (m.line[i] > m.line[i - 1]) {
                    if (u[i - 1] != '\n' && !line_start_allowed(u, i))
                        TEST_CHECK_(vjo_break_last16(u.data(), (uint32_t)u.size(), row, i) <= lead, "line at %u", i);
                    row = i;
                }
            for (uint32_t i = 0; i < text.size(); i++)
                TEST_CHECK_(u[vjo_wrap_shown(brk, nbrk, lead + i)] == text[i], "the text's unit %u", i);
        }
    /* keep is what holds the badge there: without it, the 「 text's first row would break right after it */
    std::string shown = std::string("[HOOK]") + texts[4];
    std::vector<uint16_t> u = u16(shown.c_str());
    Model m;
    m.set(u.data(), (uint32_t)u.size());
    TEST_CHECK(vjo_wrap_fix<Info>(&m, u.data(), (uint32_t)u.size()) == 6);
    TEST_CHECK(vjo_wrap_fix<Info>(&m, u.data(), (uint32_t)u.size(), 6) == 0);
    /* and an icon: the rules allow a break right after it (a symbol), here before the 「 text's first row;
     * keep 1 refuses it (audit-source-badge-b N12: before texts[0] the check could not fail) */
    std::vector<uint16_t> v = u16((std::string("\xEE\x86\xBB") + texts[4]).c_str());
    Model mv;
    mv.set(v.data(), (uint32_t)v.size());
    TEST_CHECK(vjo_break_last16(v.data(), (uint32_t)v.size(), 0, 1) == 1);
    TEST_CHECK(vjo_wrap_fix<Info>(&mv, v.data(), (uint32_t)v.size()) == 1);
    TEST_CHECK(vjo_wrap_fix<Info>(&mv, v.data(), (uint32_t)v.size(), 1) == 0);
}

TEST_LIST = {
    {"soft_lines_move_to_word_starts", soft_lines_move_to_word_starts},
    {"stale_or_missing_layouts_are_waited_for", stale_or_missing_layouts_are_waited_for},
    {"header_units_map_to_the_shown_ones", header_units_map_to_the_shown_ones},
    {"styled_insert_keeps_the_spans", styled_insert_keeps_the_spans},
    {"the_source_badge_keeps_the_wrap_rules", the_source_badge_keeps_the_wrap_rules},
    {NULL, NULL},
};
