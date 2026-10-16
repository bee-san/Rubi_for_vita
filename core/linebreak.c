#include "linebreak.h"

#include "port.h"
#include "utf.h"

/* Character classes, in this order: the ones up to C_SYMBOL start a phrase. */
enum { C_START, C_NL, C_SPACE, C_OPEN, C_CLOSE, C_SYMBOL, C_WITH, C_HIRA, C_KATA, C_KANJI, C_LATIN };

#define N(t) ((int)(sizeof(t) / sizeof((t)[0])))
/* Besides the bracket pairs of U+3008..301B (odd ones close) and ASCII ! ?
 * (the recognized text writes ！？ so, core/textnorm.h): the marks no line
 * starts with (、。〟・ dashes, pauses, ’” and the full-width and half-width
 * forms), the ones no line ends with, small kana (katakana as their
 * hiragana) and the marks that go with the letter before them (ー 々 〜 ゝゞ
 * ヽヾ ゛゜ 〃 ～ and half-width ｰﾞﾟ; ASCII ~, the text's 〜). */
static const uint16_t closing[] VJO_DATA_TABLE(closing) = {0x3001, 0x3002, 0x301F, 0x30FB, 0x2014, 0x2015, 0x2019, 0x201D, 0x2025, 0x2026,
                                   0xFF01, 0xFF09, 0xFF0C, 0xFF0E, 0xFF1A, 0xFF1B, 0xFF1F, 0xFF3D, 0xFF5D, 0xFF61,
                                   0xFF63, 0xFF64, 0xFF65, '!', '?'};
static const uint16_t opening[] VJO_DATA_TABLE(opening) = {0x301D, 0x2018, 0x201C, 0xFF08, 0xFF3B, 0xFF5B, 0xFF62};
static const uint16_t with[] VJO_DATA_TABLE(with) = {0x3041, 0x3043, 0x3045, 0x3047, 0x3049, 0x3063, 0x3083, 0x3085, 0x3087, 0x308E,
                                0x3095, 0x3096, 0x30FC, 0x3005, 0x301C, 0x309D, 0x309E, 0x30FD, 0x30FE, 0x309B,
                                0x309C, 0x3003, 0xFF5E, 0xFF70, 0xFF9E, 0xFF9F, '~'};
/* One hiragana between kanji that is a particle (the next kanji may start a
 * line): の を が に は と で も へ や か; kanji suffixes: 様 達 君 殿 氏 等 共. */
static const uint16_t particles[] VJO_DATA_TABLE(particles) = {0x306E, 0x3092, 0x304C, 0x306B, 0x306F, 0x3068, 0x3067, 0x3082, 0x3078, 0x3084,
                                     0x304B};
static const uint16_t suffixes[] VJO_DATA_TABLE(suffixes) = {0x69D8, 0x9054, 0x541B, 0x6BBF, 0x6C0F, 0x7B49, 0x5171};

static int in(const uint16_t *t, int n, uint32_t c)
{
    while (n--)
        if (t[n] == c)
            return 1;
    return 0;
}

/* Below U+10000, by range: each starts at its code point and ends at the next
 * one's (C_OPEN here: U+3008..301B's brackets, odd ones closing, but 〒 〓). */
static const uint16_t from[] VJO_DATA_TABLE(from) = {0x0000, 0x0021, 0x2000, 0x3000, 0x3001, 0x3006, 0x3008, 0x301C,
                                0x3041, 0x30A0, 0x3100, 0x3400, 0xA000, 0xAC00, 0xD7B0, 0xF900, 0xFB00,
                                0xFF10, 0xFF5B, 0xFF66, 0xFFA0};
static const uint8_t range[] VJO_DATA_TABLE(range) = {C_SPACE, C_LATIN, C_SYMBOL, C_SPACE, C_SYMBOL, C_KANJI, C_OPEN,
                                C_SYMBOL, C_HIRA, C_KATA, C_SYMBOL, C_KANJI, C_SYMBOL, C_LATIN, C_SYMBOL,
                                C_KANJI, C_SYMBOL, C_LATIN, C_SYMBOL, C_KATA, C_SYMBOL};

static int cls(uint32_t c)
{
    int i = N(from) - 1;
    if (c == '\n')
        return C_NL;
    if (c == '\r')
        return C_LATIN; /* no place to break (main's rows ignored it; the Shell's texts have none) */
    if (in(closing, N(closing), c))
        return C_CLOSE;
    if (in(opening, N(opening), c))
        return C_OPEN;
    if (in(with, N(with), c >= 0x30A1 && c <= 0x30F6 ? c - 0x60 : c)) /* ァ ッ ャ ヵ ヶ as ぁ っ ゃ ゕ ゖ */
        return C_WITH;
    if (c > 0xFFFF)
        return c >= 0x20000 ? C_KANJI : C_SYMBOL;
    while (from[i] > c)
        i--;
    if (range[i] == C_OPEN && (c & 1))
        return (c >> 1) == (0x3012 >> 1) ? C_SYMBOL : C_CLOSE;
    return range[i] == C_OPEN && c == 0x3012 ? C_SYMBOL : range[i];
}

int vjo_break_next(VjoBreakState *st, uint32_t c)
{
    int o = cls(c), q = o, p = st->prev, own = st->own, ok;
    if (own <= C_NL || own == C_OPEN || o <= C_SPACE || o == C_CLOSE || o == C_WITH)
        ok = 0; /* a line's first character; never after an opening mark or before a closing one */
    else if (own <= C_SYMBOL || o <= C_SYMBOL)
        ok = 1; /* after a space, a closing mark or a symbol; before an opening mark or a symbol */
    else
        ok = p == C_HIRA && (o == C_KATA || o == C_KANJI) &&
             st->cp != 0x304A && st->cp != 0x3054 &&                                            /* お|茶, ご|飯 */
             !(st->run == 1 && st->before == C_KANJI && !in(particles, N(particles), st->cp)) && /* 振り|返る */
             !in(suffixes, N(suffixes), c);                                                     /* あなた|達 */
    if (o == C_WITH) {
        st->run += p == C_HIRA && st->run < 3;
        q = p >= C_WITH ? p : C_SYMBOL; /* ー っ 々 go with their letter; alone, a symbol */
    } else if (o != C_HIRA)
        st->run = 0;
    else if (p == C_HIRA)
        st->run += st->run < 3;
    else {
        st->run = 1;
        st->before = (uint8_t)p;
    }
    st->own = (uint8_t)o;
    st->prev = (uint8_t)q;
    st->cp = c;
    return ok;
}

uint32_t vjo_break_last16(const uint16_t *u, uint32_t n, uint32_t from, uint32_t to)
{
    VjoBreakState st = {0};
    uint32_t last = 0, i = from;
    if (to >= n)
        return 0;
    /* The rules look back three letters at most: eight units before the line
     * (none past a hard break) settle the walk's state. */
    for (int k = 0; k < 8 && i && u[i - 1] != '\n'; k++)
        i--;
    while (i <= to) {
        uint32_t c = u[i], w = 1;
        if ((c & 0xFC00) == 0xD800 && i + 1 < n && (u[i + 1] & 0xFC00) == 0xDC00) {
            c = 0x10000 + ((c - 0xD800) << 10) + (u[i + 1] - 0xDC00u);
            w = 2;
        }
        if (vjo_break_next(&st, c) && i > from)
            last = i;
        i += w;
    }
    return last;
}

size_t vjo_break_last8(const char *s, size_t len, size_t from, size_t to)
{
    VjoBreakState st = {0};
    size_t last = 0, i = from;
    if (to >= len)
        return 0;
    for (int k = 0; k < 8 && i && s[i - 1] != '\n'; k++)
        while (i && (s[--i] & 0xC0) == 0x80) {
        }
    while (i <= to) {
        size_t at = i;
        if (vjo_break_next(&st, vjo_utf8_next(s, len, &i)) && at > from)
            last = at;
    }
    return last;
}
