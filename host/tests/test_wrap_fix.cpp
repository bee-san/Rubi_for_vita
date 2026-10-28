/* The re-wrap of the overlay's paf texts (shell/wrap_fix.h, the owner's
 * 2026-10-16 item 4: break between phrases, not anywhere) on a model of paf's layout:
 * greedy lines in a box, Japanese broken at any character, Latin at its last
 * space, '\n' a hard break, a layout that can be missing or one SetString
 * behind. It shows what the overlay's header, popup and strip get: every line
 * paf started inside a word is started earlier, at a place core/linebreak.h
 * allows, and a word wider than the box keeps paf's break. The real layout is
 * the console's (its glyph widths); this is not a substitute for it. Also the
 * header's unit map, vjo_styled_insert's spans and the strip's own units
 * (vjo_styled_units, the room for its breaks). */
#include <string.h>
#include <algorithm>
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

/* The subtitle strip's text (shell/strip.cpp): VJO_STRIP_MAX (shell/shell.h), its bytes of UTF-8 and its
 * units, in a buffer of VJO_STRIP_MAX + 1 + VJO_WRAP_BREAKS units, as strip.cpp's s_strip_u. */
#define STRIP_MAX 4096u
#define STRIP_UNITS (STRIP_MAX + 1 + VJO_WRAP_BREAKS)

/* What the strip's text was until cleanup P1 (#418): the text styled in an arena of cap units and one span
 * (vjo_styled_init, vjo_styled_puts), its units with their NUL, and truncated. */
struct StyledRef {
    std::vector<uint16_t> units;
    int truncated;
};
static StyledRef styled_ref(const char *s, uint32_t cap)
{
    std::vector<uint8_t> mem((cap + 16) * sizeof(uint16_t) + 256);
    VjoArena a;
    VjoStyled st;
    vjo_arena_init(&a, mem.data(), mem.size());
    StyledRef r;
    if (vjo_styled_init(&st, &a, cap, 1) != 0) {
        TEST_CHECK_(0, "the reference's arena is too small for %u units", cap);
        r.truncated = -1;
        return r;
    }
    vjo_styled_puts(&st, s, VJO_RGB_TEXT, 27);
    r.units.assign(st.text, st.text + st.len + 1);
    r.truncated = st.truncated;
    return r;
}

/* Whether n units end at a whole character of s, as vjo_utf8_next decodes it (CR has no unit). */
static bool at_character_end(const std::string &s, uint32_t n)
{
    size_t i = 0;
    uint32_t u = 0;
    while (u < n && i < s.size()) {
        uint32_t cp = vjo_utf8_next(s.c_str(), s.size(), &i);
        if (cp != '\r')
            u += (uint32_t)vjo_utf16_units(cp);
    }
    return u == n;
}

/* vjo_styled_units into the strip's buffer, against the styled text: the same units and NUL, the same
 * count, the same cut (at a whole character: never half a surrogate pair), and nothing written after the
 * NUL, so the breaks' room after it stays free. Returns 0 on a difference (said, with the case). */
static int strip_units_match(const std::string &s, uint32_t cap, const char *what)
{
    std::vector<uint16_t> buf(cap + 1 + VJO_WRAP_BREAKS, 0xA5A5);
    uint32_t n = vjo_styled_units(buf.data(), cap, s.c_str());
    StyledRef ref = styled_ref(s.c_str(), cap), whole = styled_ref(s.c_str(), (uint32_t)s.size() * 2 + 2);
    int ok = ref.truncated >= 0 && n + 1 == ref.units.size() && !memcmp(buf.data(), ref.units.data(), (n + 1) * 2);
    for (size_t k = n + 1; ok && k < buf.size(); k++)
        ok = buf[k] == 0xA5A5;
    /* cut where the styled text says it is, after a whole character of the whole text's units */
    ok = ok && ref.truncated == (n + 1 < whole.units.size()) && !whole.truncated &&
         !memcmp(buf.data(), whole.units.data(), n * 2) && at_character_end(s, n);
    if (!ok)
        TEST_CHECK_(ok, "%s: %zu bytes, cap %u: %u units, the styled text %zu (truncated %d)", what, s.size(), cap, n,
                    ref.units.size() - 1, ref.truncated);
    return ok;
}

/* Random UTF-8, mostly not: valid sequences of every length (surrogates and U+110000.. too, which
 * vjo_utf8_next decodes), overlong ones, lone and cut lead bytes, stray continuation bytes, CR, LF;
 * never a NUL. */
static uint32_t rng_state = 0x2545F491u;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static std::string random_utf8(size_t bytes)
{
    std::string s;
    while (s.size() < bytes) {
        uint32_t kind = rng() % 12, cp;
        char enc[4];
        switch (kind) {
        case 0: s += (char)(0x20 + rng() % 0x5F); break;                     /* ASCII */
        case 1: s += rng() & 1 ? '\r' : '\n'; break;
        case 2: cp = 0x80 + rng() % 0x780; s.append(enc, (size_t)vjo_utf8_encode(cp, enc)); break;
        case 3: case 4: cp = 0x800 + rng() % 0xF800; s.append(enc, (size_t)vjo_utf8_encode(cp, enc)); break;
        case 5: case 6: cp = 0x10000 + rng() % 0x100000; s.append(enc, (size_t)vjo_utf8_encode(cp, enc)); break;
        case 7: /* past U+10FFFF, to U+1FFFFF: a lead F4-F7 */
            cp = 0x110000 + rng() % 0xF0000;
            s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F));
            s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F));
            break;
        case 8: s += (char)(0xC0 | (rng() & 1)); s += (char)(0x80 | (rng() & 0x3F)); break; /* overlong */
        case 9: s += (char)(0x80 + rng() % 0x40); break;                      /* a stray continuation */
        case 10: s += (char)(0xC2 + rng() % 0x3E); break;                     /* a lead alone (or cut) */
        default: s += (char)(1 + rng() % 0xFF); break;                       /* any byte but NUL */
        }
    }
    return s;
}

static void strip_units_are_the_styled_text(void)
{
    std::vector<uint16_t> buf(STRIP_UNITS, 0xA5A5);
    /* by hand: CR omitted, a pair for U+1F600, U+FFFD for each byte of a cut sequence */
    TEST_CHECK(vjo_styled_units(buf.data(), STRIP_MAX, "") == 0 && buf[0] == 0 && buf[1] == 0xA5A5);
    TEST_CHECK(vjo_styled_units(buf.data(), STRIP_MAX, "猫\r\n犬") == 3 && buf[0] == 0x732B && buf[1] == '\n' &&
               buf[2] == 0x72AC && buf[3] == 0);
    TEST_CHECK(vjo_styled_units(buf.data(), STRIP_MAX, "\xF0\x9F\x98\x80") == 2 && buf[0] == 0xD83D &&
               buf[1] == 0xDE00 && buf[2] == 0);
    TEST_CHECK(vjo_styled_units(buf.data(), STRIP_MAX, "a\xE3\x81") == 3 && buf[1] == 0xFFFD && buf[2] == 0xFFFD);
    TEST_CHECK(vjo_styled_units(buf.data(), 1, "\xF0\x9F\x98\x80") == 0 && buf[0] == 0); /* no room for the pair */
    /* at the cap: the pair that would pass it is left out whole */
    std::string edge(STRIP_MAX - 1, 'a');
    edge += "\xF0\x9F\x98\x80" "b";
    std::fill(buf.begin(), buf.end(), 0xA5A5);
    TEST_CHECK(vjo_styled_units(buf.data(), STRIP_MAX, edge.c_str()) == STRIP_MAX - 1 && buf[STRIP_MAX - 1] == 0);
    for (size_t k = STRIP_MAX; k < buf.size(); k++)
        TEST_CHECK_(buf[k] == 0xA5A5, "unit %zu written", k);
    strip_units_match(edge, STRIP_MAX, "a pair at the cap");
    edge.assign(STRIP_MAX - 2, 'a');
    edge += "\xF0\x9F\x98\x80" "b";
    TEST_CHECK(vjo_styled_units(buf.data(), STRIP_MAX, edge.c_str()) == STRIP_MAX && buf[STRIP_MAX - 1] == 0xDE00 &&
               buf[STRIP_MAX] == 0);
    strip_units_match(edge, STRIP_MAX, "a pair that ends at the cap");
    /* the longest strip texts (VjoView.strip_text: VJO_STRIP_MAX bytes with its NUL) fit whole, the room
     * for every break kept: all pairs, all one-byte, all three-byte */
    std::string pairs, ones(STRIP_MAX - 1, 'x'), threes;
    while (pairs.size() + 4 < STRIP_MAX) pairs += "\xF0\xA0\xAE\x9F"; /* 𠮟 */
    while (threes.size() + 3 < STRIP_MAX) threes += "\xE3\x81\x82";   /* あ */
    for (const std::string *t : {&pairs, &ones, &threes}) {
        std::fill(buf.begin(), buf.end(), 0xA5A5);
        uint32_t n = vjo_styled_units(buf.data(), STRIP_MAX, t->c_str());
        TEST_CHECK(n <= t->size() && n < STRIP_MAX);
        strip_units_match(*t, STRIP_MAX, "a longest strip text");
    }
    /* random UTF-8: the strip's cap, texts of every length up to past it, and small caps for many cuts */
    int failures = 0, cuts = 0;
    for (int i = 0; i < 4000 && failures < 8; i++) {
        size_t bytes = i < 2000 ? rng() % (STRIP_MAX + 2048) : rng() % 96;
        uint32_t cap = i < 2000 ? STRIP_MAX : rng() % 48;
        std::string s = random_utf8(bytes);
        if (!strip_units_match(s, cap, "random"))
            failures++;
        cuts += styled_ref(s.c_str(), cap).truncated == 1;
        if (s.size() < STRIP_MAX) { /* any strip text fits whole */
            std::fill(buf.begin(), buf.end(), 0xA5A5);
            uint32_t n = vjo_styled_units(buf.data(), STRIP_MAX, s.c_str());
            TEST_CHECK_(n <= s.size() && styled_ref(s.c_str(), STRIP_MAX).truncated == 0, "%zu bytes: %u units",
                        s.size(), n);
        }
    }
    TEST_CHECK(failures == 0);
    TEST_CHECK_(cuts > 1000, "%d of the random texts cut", cuts);
}

TEST_LIST = {
    {"soft_lines_move_to_word_starts", soft_lines_move_to_word_starts},
    {"stale_or_missing_layouts_are_waited_for", stale_or_missing_layouts_are_waited_for},
    {"header_units_map_to_the_shown_ones", header_units_map_to_the_shown_ones},
    {"styled_insert_keeps_the_spans", styled_insert_keeps_the_spans},
    {"strip_units_are_the_styled_text", strip_units_are_the_styled_text},
    {NULL, NULL},
};
