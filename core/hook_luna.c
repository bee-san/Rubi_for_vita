/* LunaTranslator's PS Vita hook table, its hookfuncs and its filters, ported
 * (core/hook_luna.h; the sources: third_party/LunaTranslator, 51884b6).
 *
 * Two halves. vjo_luna_capture runs at hook time, inside the game's function:
 * it only reads, through the caller's VjoHookRead, at most VJO_LUNA_READS times,
 * into the caller's buffer, with a few dozen bytes of locals and no other call.
 * vjo_luna_text runs at job time in the worker's thread: it decodes the raw
 * bytes (core/hook_text.c), applies the site's hookfunc's own clean-up, Luna's
 * commonfilter and the site's filter, all on UTF-8, with VJO_LUNA_TEXT_MAX bytes
 * of scratch and the regex engine's state (core/hook_re.c) on the stack and a
 * work budget. A filter step that fails (budget, a pattern outside the subset)
 * drops the hit: OCR answers then.
 *
 * The filters are op lists that keep Luna's code in order: its re::sub patterns
 * verbatim (CP932 byte escapes written as the characters they encode), its
 * strReplace/StringFilter/CharFilter strings. Luna runs a byte pattern on the
 * text's own bytes: on a UTF-8 site those are the bytes here (VJO_RE_BYTES, the
 * same result); on a CP932 site they were Shift-JIS bytes, matched here as
 * characters, which is the same result unless a pattern's ASCII delimiter is a
 * double-byte character's trail byte there (0x40-0x7E: @ [ \ ] ^ _ ` { | } ~ and
 * letters), where Luna's match would cut the character. Its wide patterns (UTF-16
 * sites, or text it decoded first) run on characters. docs/hook-titles.md lists
 * each filter's port and caveats. */
#include "hook_luna.h"
#include "port.h"

#include <string.h>

#include "hook_re.h"

#include "hook_luna_table.inc"

#define LUNA_TEXT_BUDGET 2000000ul /* matcher steps for one text (core/hook_re.h) */

/* ---------------- the table ---------------- */

/* the caller's ID against the table's (9 characters): stops at the first difference */
static int id_cmp(const char *a, const char *b)
{
    for (unsigned i = 0; i < 9; i++) {
        unsigned x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

static int title_index(const char *id)
{
    unsigned lo = 0, hi = VJO_LUNA_TITLES;
    if (!id) return -1;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        int c = id_cmp(id, vjo_luna_ids[mid]);
        if (!c) return (int)mid;
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    return -1;
}

unsigned vjo_luna_sites(const char *title_id, const VjoLunaSite **sites)
{
    int t = title_index(title_id);
    unsigned lo = 0, hi = VJO_LUNA_SITES, n = 0;
    if (sites) *sites = NULL;
    if (t < 0) return 0;
    while (lo < hi) { /* the title's first site: the sites are sorted by title */
        unsigned mid = (lo + hi) / 2;
        if (vjo_luna_table[mid].title < (unsigned)t) lo = mid + 1;
        else hi = mid;
    }
    while (lo + n < VJO_LUNA_SITES && vjo_luna_table[lo + n].title == (unsigned)t) n++;
    if (sites && n) *sites = &vjo_luna_table[lo];
    return n;
}

int vjo_luna_title_known(const char *title_id)
{
    return title_index(title_id) >= 0;
}

int vjo_luna_title_ported(const char *title_id)
{
    const VjoLunaSite *s;
    unsigned n = vjo_luna_sites(title_id, &s);
    /* the table's flag: vjo_luna_ported's answer for every site (test_hook_luna's table_counts), without
     * linking the filters into the Rubi app, which tags its Games page with this */
    for (unsigned i = 0; i < n; i++)
        if (s[i].ported) return 1;
    return 0;
}

const char *vjo_luna_title(unsigned index)
{
    return index < VJO_LUNA_TITLES ? vjo_luna_ids[index] : NULL;
}

int vjo_luna_version_ok(const VjoLunaSite *all, unsigned n, const VjoLunaSite *s, uint16_t app_ver)
{
    if (!s) return 0;
    if (s->version) return app_ver && s->version == app_ver;
    if (!app_ver) return 1;
    for (unsigned i = 0; all && i < n; i++)
        if (all[i].version == app_ver) return 0; /* the unnamed sites are another version's */
    return 1;
}

const VjoLunaSite *vjo_luna_site_at(const VjoLunaSite *all, unsigned n, uint32_t off, uint16_t app_ver)
{
    for (unsigned i = 0; all && i < n; i++) {
        const VjoLunaSite *s = &all[i];
        uint32_t a = s->addr & ~1u;
        if (a >= VJO_LUNA_SEG0 && a - VJO_LUNA_SEG0 == off && vjo_luna_ported(s) &&
            vjo_luna_version_ok(all, n, s, app_ver))
            return s;
    }
    return NULL;
}

/* ---------------- hook time ---------------- */

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The bytes before the first NUL unit in raw[0..k), k cut to whole units. */
static unsigned text_len(const uint8_t *raw, unsigned k, unsigned cap, unsigned unit)
{
    unsigned i = 0;
    if (k > cap) k = cap; /* a reader that says it copied more than it may */
    if (unit == 2) {
        k &= ~1u;
        while (i < k && (raw[i] | raw[i + 1])) i += 2;
    } else {
        while (i < k && raw[i]) i++;
    }
    return i;
}

/* The memory at addr into dst, max bytes at most, by successive reads (each stops
 * after a NUL unit or at the end of a memory block): at most VJO_LUNA_READS of
 * them; stops at an address it cannot read. */
static unsigned mirror(VjoHookRead rd, void *ud, uint32_t addr, uint8_t *dst, unsigned max, unsigned unit)
{
    unsigned got = 0;
    for (unsigned calls = 0; got < max && calls < VJO_LUNA_READS; calls++) {
        unsigned k = rd(ud, addr + got, dst + got, max - got, unit);
        if (!k || k > max - got) break;
        got += k;
    }
    return got;
}

/* Strings that follow each other (each after the previous one's NUL) up to an
 * empty one, joined in place; with drop_nl each one's final newline goes. */
static unsigned join_strings(uint8_t *raw, unsigned m, int drop_nl)
{
    unsigned i = 0, w = 0;
    while (i < m && raw[i]) {
        unsigned start = w;
        while (i < m && raw[i]) raw[w++] = raw[i++];
        if (drop_nl && w > start && raw[w - 1] == '\n') w--;
        i++;
    }
    return w;
}

/* USING_CHAR: the character in v's low bytes (Luna: CP932 one byte or a lead and
 * its trail, UTF-16 one unit, UTF-8 one sequence). */
static unsigned one_char(uint32_t v, unsigned enc, uint8_t *raw, unsigned cap)
{
    unsigned b = v & 0xFF, n;
    if (enc == VJO_HOOK_ENC_UTF16LE) n = 2;
    else if (enc == VJO_HOOK_ENC_CP932) n = (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC) ? 2 : 1;
    else n = b < 0x80 ? 1 : b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : 2;
    if (n > cap) return 0;
    for (unsigned i = 0; i < n; i++) raw[i] = (uint8_t)(v >> (8 * i));
    return text_len(raw, n, cap, enc == VJO_HOOK_ENC_UTF16LE ? 2 : 1) == n ? n : 0;
}

/* Luna's default read: the string at reg + padding, or at the pointer there with
 * DATA_INDIRECT; USING_CHAR: the character instead of a string. */
static unsigned cap_default(const VjoLunaSite *s, uint32_t p, VjoHookRead rd, void *ud, uint8_t *raw,
                            unsigned cap, unsigned unit)
{
    if (s->flags & VJO_LUNA_F_DATA_INDIRECT) {
        uint8_t w[4];
        if (rd(ud, p, w, 4, 0) != 4) {
            if (!(s->flags & VJO_LUNA_F_USING_CHAR) || rd(ud, p, w, 2, 0) != 2) return 0;
            w[2] = w[3] = 0;
        }
        p = le32(w);
    }
    p += (uint32_t)s->padding;
    if (s->flags & VJO_LUNA_F_USING_CHAR) return one_char(p, s->encoding, raw, cap);
    return text_len(raw, rd(ud, p, raw, cap, unit), cap, unit);
}

/* PCSG01124: 0xAA, then a string, again and again; after 0xAA, 1B B4 and 1B B6
 * skip 7 bytes and 1B B5 skips 3 (an escape and its parameters). */
static unsigned cap_pcsg01124(uint32_t p, VjoHookRead rd, void *ud, uint8_t *raw, unsigned cap)
{
    unsigned m = mirror(rd, ud, p, raw, cap, 1), i = 0, w = 0;
    while (i < m && raw[i] == 0xAA) {
        i++;
        if (i + 1 < m && raw[i] == 0x1B && (raw[i + 1] == 0xB4 || raw[i + 1] == 0xB6)) {
            i += 7;
            continue;
        }
        if (i + 1 < m && raw[i] == 0x1B && raw[i + 1] == 0xB5) {
            i += 3;
            continue;
        }
        while (i < m && raw[i]) raw[w++] = raw[i++];
        i++;
    }
    return w;
}

/* A text and its length: `units` text units of `unit` bytes at p + text, the
 * 32-bit count at p + count. Luna copies them all and LunaHost removes NUL
 * characters: so does this. */
static unsigned cap_counted(uint32_t p, uint32_t count, uint32_t text, unsigned unit, VjoHookRead rd, void *ud,
                            uint8_t *raw, unsigned cap)
{
    uint8_t w[4];
    uint32_t units;
    unsigned want, m, o = 0;
    if (rd(ud, p + count, w, 4, 0) != 4) return 0;
    units = le32(w);
    want = unit == 2 ? (units > cap >> 1 ? cap & ~1u : (unsigned)units << 1) : (units > cap ? cap : (unsigned)units);
    m = mirror(rd, ud, p + text, raw, want, unit);
    if (unit == 2) {
        m &= ~1u;
        for (unsigned i = 0; i < m; i += 2)
            if (raw[i] | raw[i + 1]) {
                raw[o++] = raw[i];
                raw[o++] = raw[i + 1];
            }
    } else {
        for (unsigned i = 0; i < m; i++)
            if (raw[i]) raw[o++] = raw[i];
    }
    return o;
}

/* PCSG00912: 47 FF FF in the 0x50 bytes at p (Luna's SearchPattern: offsets
 * 0..0x4C, and a hit at 0 reads as none), the text 5 bytes after it, then more
 * text while 48 FF FF and 47 FF FF (and 2 bytes) separate them. */
static unsigned cap_pcsg00912(uint32_t p, VjoHookRead rd, void *ud, uint8_t *raw, unsigned cap)
{
    unsigned m = mirror(rd, ud, p, raw, cap < 0x50 ? cap : 0x50, 1), k, i = 0, w = 0;
    for (k = 0; k < 0x4D && k + 3 <= m; k++)
        if (raw[k] == 0x47 && raw[k + 1] == 0xFF && raw[k + 2] == 0xFF) break;
    if (!k || k >= 0x4D || k + 3 > m) return 0;
    m = mirror(rd, ud, p + k + 5, raw, cap, 1);
    for (;;) {
        while (i < m && raw[i]) raw[w++] = raw[i++];
        i++;
        if (i + 3 > m || raw[i] != 0x48 || raw[i + 1] != 0xFF || raw[i + 2] != 0xFF) break;
        i += 3;
        if (i + 3 > m || raw[i] != 0x47 || raw[i + 1] != 0xFF || raw[i + 2] != 0xFF) break;
        i += 5;
    }
    return w;
}

/* The `back` bytes before p into raw (halved until a memory block holds them all:
 * at most 1 + log2(back) reads); returns how many. */
static unsigned before(uint32_t p, VjoHookRead rd, void *ud, uint8_t *raw, unsigned back)
{
    if (back > p) back = (unsigned)p;
    while (back && rd(ud, p - back, raw, back, 0) != back) back /= 2;
    return back;
}

/* PCSG01011: the string that holds p (r7), from the byte after the NUL before it;
 * no NUL within the cap bytes before p: none (the text would not fit). */
static unsigned cap_pcsg01011(uint32_t p, VjoHookRead rd, void *ud, uint8_t *raw, unsigned cap)
{
    unsigned back = before(p, rd, ud, raw, cap), k = back;
    while (k && raw[k - 1]) k--;
    if (!k) return 0;
    p -= back - k;
    return text_len(raw, rd(ud, p, raw, cap, 1), cap, 1);
}

/* PCSG00769: from p, back to a NUL, on to the first two bytes that are not NUL;
 * while the byte two before them is not a NUL (another string ends there), back
 * by two and again; then the strings from there up to an empty one, joined. The
 * walk runs on the bytes around p (cap / 2 before it); leaving them, or a step
 * that does not get further back (a one-byte string, where Luna's loop never
 * ends), gives no text. */
static unsigned cap_pcsg00769(uint32_t p, VjoHookRead rd, void *ud, uint8_t *raw, unsigned cap)
{
    unsigned back = before(p, rd, ud, raw, cap / 2), m, k = back, last = ~0u;
    m = back + mirror(rd, ud, p, raw + back, cap - back, 1);
    for (unsigned steps = 0; steps < 64; steps++) {
        while (k < m && raw[k]) {
            if (!k) return 0;
            k--;
        }
        if (k >= m) return 0;
        while (k + 1 < m && !(raw[k] && raw[k + 1])) k++;
        if (k + 1 >= m || k >= last) return 0;
        last = k;
        if (k < 2 || !raw[k - 2]) break;
        k -= 2;
    }
    if (k >= m) return 0;
    m = mirror(rd, ud, p - back + k, raw, cap, 1);
    return join_strings(raw, m, 0);
}

unsigned vjo_luna_capture(const VjoLunaSite *s, const uint32_t regs[16], VjoHookRead rd, void *ud,
                          uint8_t *raw, unsigned cap, unsigned *encoding)
{
    unsigned unit, m;
    uint32_t p;
    if (encoding) *encoding = s ? s->encoding : VJO_HOOK_ENC_AUTO;
    if (!s || !regs || !rd || !raw || cap < 4 || !s->ported || s->reg > 15) return 0;
    unit = s->encoding == VJO_HOOK_ENC_UTF16LE ? 2u : 1u;
    if (unit == 2) cap &= ~1u;
    p = regs[s->reg];
    switch (s->func) {
    case VJO_LH_NONE:
        return cap_default(s, p, rd, ud, raw, cap, unit);
    case VJO_LH_PCSG01124:
        return cap_pcsg01124(p, rd, ud, raw, cap);
    case VJO_LH_READU16LEN: /* ReadU16TextAndLenDW */
        return cap_counted(p, 8, 0xC, 2, rd, ud, raw, cap);
    case VJO_LH_PCSG01046: /* the same from r0 (its <br>: vjo_luna_text) */
        return cap_counted(regs[0], 8, 0xC, 2, rd, ud, raw, cap);
    case VJO_LH_TPCSG00903:
        return cap_counted(regs[0], 0x14, 0x1C, 1, rd, ud, raw, cap);
    case VJO_LH_PCSG01235:
        return text_len(raw, rd(ud, p - 3, raw, cap, 1), cap, 1);
    case VJO_LH_PCSG00992:
        m = mirror(rd, ud, regs[1], raw, cap, 1);
        return join_strings(raw, m, 0);
    case VJO_LH_PCSG00914:
        m = mirror(rd, ud, p + (uint32_t)s->padding, raw, cap, 1);
        return join_strings(raw, m, 1);
    case VJO_LH_PCSG00912:
        return cap_pcsg00912(regs[1], rd, ud, raw, cap);
    case VJO_LH_PCSG01011:
        return cap_pcsg01011(regs[7], rd, ud, raw, cap);
    case VJO_LH_PCSG00769:
        return cap_pcsg00769(p, rd, ud, raw, cap);
    default: /* PCSG00595, TPCSG00291, PCSB00985: not ported */
        return 0;
    }
}

/* ---------------- job time: the filters ---------------- */

enum {
    OP_END,
    OP_DEL,     /* a removed wherever it is (StringFilter, CharFilter, strReplace(s, a)) */
    OP_REP,     /* a replaced by b wherever it is (strReplace, StringCharReplacer) */
    OP_SUB,     /* re::sub(s, a, b) on a byte string: bytes on a UTF-8 site, characters on a CP932 one */
    OP_SUBW,    /* re::sub on a wide string: characters */
    OP_CALL,    /* another filter (arg) */
    OP_IFEQ,    /* a text equal to a: dropped */
    OP_PREFIX,  /* a text that does not start with a: dropped */
    OP_UPTO,    /* the text up to the end of its first a (none: dropped) */
    OP_AFTER,   /* the text after its first a (none: unchanged) */
    OP_CUT,     /* substr(from, size - from - drop) in CP932 bytes: arg = from | drop << 4 */
    OP_ENDDROP, /* a text that ends with a loses its last byte */
    OP_P00449,  /* PCSG00449's ending */
    OP_WRAP,    /* PCSG00272: a name before #n# into 【】 */
    OP_NAMEPOS, /* PCSG00935: the name after #n#Pos[..] (pattern a) to the front, in 【】 */
    OP_TRIM,    /* Trim(): iswspace at both ends */
    OP_KANA,    /* remapkatakana */
    OP_ASCII,   /* all_ascii: a text of ASCII only is dropped */
    OP_BETWEEN  /* StringFilterBetween(a, b) */
};

/* VjoLunaOp.enc: the site encodings an op runs for (0: all) */
#define E_U8 (1u << VJO_HOOK_ENC_UTF8)
#define E_U16 (1u << VJO_HOOK_ENC_UTF16LE)
#define E_SJ (1u << VJO_HOOK_ENC_CP932)

typedef struct {
    uint8_t op, enc, arg;
    const char *a, *b;
} LunaOp;

#define END {OP_END, 0, 0, NULL, NULL}
#define DEL(x) {OP_DEL, 0, 0, x, ""}
#define REP(x, y) {OP_REP, 0, 0, x, y}
#define SUB(p, r) {OP_SUB, 0, 0, p, r}
#define SUB_IF(e, p, r) {OP_SUB, e, 0, p, r}
#define SUBW(p, r) {OP_SUBW, 0, 0, p, r}
#define SUBW_IF(e, p, r) {OP_SUBW, e, 0, p, r}
#define DEL_IF(e, x) {OP_DEL, e, 0, x, ""}
#define CALL(f) {OP_CALL, 0, VJO_LF_##f, NULL, NULL}
#define IFEQ_IF(e, x) {OP_IFEQ, e, 0, x, NULL}
#define PREFIX(x) {OP_PREFIX, 0, 0, x, NULL}
#define UPTO(x) {OP_UPTO, 0, 0, x, NULL}
#define AFTER(x) {OP_AFTER, 0, 0, x, NULL}
#define CUT(from, drop) {OP_CUT, 0, (from) | (drop) << 4, NULL, NULL}
#define ENDDROP(x) {OP_ENDDROP, 0, 0, x, NULL}
#define SPECIAL(o) {o, 0, 0, NULL, NULL}
#define NAMEPOS(p) {OP_NAMEPOS, 0, 0, p, NULL}
#define BETWEEN(x, y) {OP_BETWEEN, 0, 0, x, y}

/* Each list: Luna's function's statements, in order (vita3k_1.cpp; its CP932
 * escapes in comments where a character stands for them). A duplicate check
 * (`static std::string last` and the like) is left out: Rubi reads the latest
 * text when a job asks for it. */
static const LunaOp f_NEWLINE[] = {DEL("\n"), END}; /* NewLineCharFilterA/W, PCSG01034 */
static const LunaOp f_IDENTITY[] = {END};           /* PCSG00543, PCSG00689, PCSG01284, PCSG00535_2 */
static const LunaOp f_ALL_ASCII[] = {SPECIAL(OP_ASCII), END};
static const LunaOp f_BACKSLASH[] = {DEL("\\"), END}; /* PCSG00787 */
static const LunaOp f_CARET[] = {DEL("^"), END};      /* FPCSG00852 (strAW, ^, fromWA) */
static const LunaOp f_FPCSG01124[] = {
    SUB("\x1b" "ｴ", ""), SUB("\x1b" "ｵ", ""), /* "\x1b\xb4", "\x1b\xb5": ESC and a half-width katakana byte */
    SUB("(　)*\\n*(　)*", ""),                /* R"((\x81\x40)*\n*(\x81\x40)*)" */
    END};
static const LunaOp f_FPCSG01023[] = {
    SUB("<br>", ""), SUB("%CF11F", ""), SUB("%CFFFF", ""), SUB("%P", ""), SUB("%K", ""),
    SUB("%N(　)*", ""), /* R"(%N(\x81\x40)*)" */
    SUB("\n", ""), END};
static const LunaOp f_FPCSG01282[] = {SUB("(\\n)+", " "), SUB("\\d$|^@[a-z]+|#.*?#|\\$", ""), END};
static const LunaOp f_FPCSG00410[] = {
    SUB("#[A-Za-z]+\\[(\\d*[.])?\\d+\\]", ""), SUB("#Pos\\[[\\s\\S]*?\\]", ""), SUB("#n", " "),
    REP("④", "!?"), REP("②", "!!"), REP("⑥", "。"), REP("⑪", "【"), REP("⑫", "】"), /* \x87\x43 ... */
    REP("⑤", "、"), REP("①", "・・・"), END};
static const LunaOp f_FPCSG00448[] = {
    SUB("[\\s]", ""), SUB("(#n)+", ""), SUB("#[A-Za-z]+\\[(\\d*[.])?\\d+\\]", ""), SUB("#Pos\\[[\\s\\S]*?\\]", ""),
    END};
static const LunaOp f_PCSG01203[] = {SUB("#Ruby\\[[-\\d]+,(.*?)\\]", ""), END};
static const LunaOp f_PCSG00401[] = {
    SUB("【(　)*(.*?)(　)*】", "【$2】"), /* R"(\x81\x79(\x81\x40)*(.*?)(\x81\x40)*\x81\x7a)", "\x81\x79$2\x81\x7a" */
    END};
static const LunaOp f_PCSG00766[] = {
    IFEQ_IF(E_U8, "　"), /* s == u8"　": the UTF-8 form, so only a UTF-8 site's text can equal it */
    SUB("#Ruby\\[(.*?),(.*?)\\]", "$1"),
    SUB("(　)*#n(　)*", ""), /* u8R"((　)*#n(　)*)" on UTF-8, R"((\x81\x40)*#n(\x81\x40)*)" on CP932 */
    SUB("#[A-Za-z]+\\[[\\d\\-,\\.]*\\]", ""), END};
static const LunaOp f_PCSG00935[] = {NAMEPOS("#n#Pos[[\\d\\-,\\.]*\\](.*)"), CALL(PCSG00766), END};
static const LunaOp f_FPCSG00903[] = {SUB("\\\\n", " "), END};
static const LunaOp f_PCSG00615[] = {DEL("＿"), END}; /* L"＿" */
static const LunaOp f_PCSG01001[] = {DEL("@@"), END};
static const LunaOp f_PCSG01247[] = {DEL("\\n"), END};
static const LunaOp f_PCSG01144[] = {DEL("\\n"), DEL("\\c"), END};
static const LunaOp f_FPCSG01180[] = {SUB("\\\\n", " "), SUB(",.*$", " "), END};
static const LunaOp f_PCSG01002[] = {DEL("\n"), SUBW("<.*?>", ""), END};
static const LunaOp f_FPCSG00839[] = {
    /* strW(): on a UTF-8 site Luna reads the bytes as UTF-16 units, where none of these can match */
    SUBW_IF(E_U16, "\\[[^\\]]+.", ""), SUBW_IF(E_U16, "\\\\k|\\\\x|%C|%B|%p-1;", ""),
    SUBW_IF(E_U16, "#[0-9a-fA-F]+;([^%#]+)(%r)?", "$1"), SUBW_IF(E_U16, "\\\\n", ""), END};
static const LunaOp f_FPCSG00751[] = {SUB("[\\s]", ""), SUB("@[a-z]", ""), DEL("＄"), /* \x81\x90 */ END};
static const LunaOp f_FPCSG00401[] = {
    SUB("[\\s]", ""),
    DEL("\\c"), /* re::sub(s, R"(\c)"): not valid ECMAScript (Luna throws and drops the hit); its literal \c */
    SUB("\\\\n", ""), END};
static const LunaOp f_PCSG00530[] = {DEL("#n"), END};
static const LunaOp f_PCSG00826[] = {DEL("#n"), DEL("　"), /* \x81\x40 */ END};
static const LunaOp f_PCSG00592[] = {SUB("#Color\\[\\d+\\]", ""), END};
static const LunaOp f_PCSG00833[] = {DEL("　"), END}; /* u8"　" */
static const LunaOp f_PCSG00855[] = {DEL("#n　"), DEL("#n"), END};
static const LunaOp f_PCSG01150[] = {CALL(PCSG00855), CALL(PCSG00592), END};
static const LunaOp f_PCSG01260_T[] = {DEL("@n"), END};
static const LunaOp f_PCSG01260[] = {PREFIX("【"), UPTO("】"), END};
static const LunaOp f_PCSG01204[] = {DEL("\n"), DEL("@1r"), DEL("@-1r"), END};
static const LunaOp f_PCSG01068[] = {REP("\\\\", "\n"), DEL("\\"), ENDDROP("。!"), END};
static const LunaOp f_PCSG00367[] = {DEL("\\"), DEL("#"), END};
static const LunaOp f_PCSG01167[] = {SUB("<(.*?)>(.*?)\\|", "$2"), END};
static const LunaOp f_PCSG00917[] = {SUB("([^。…？！])　", "$1"), SUB("^　", ""), SUB("#n", ""), END};
static const LunaOp f_PCSG00938[] = {
    SUB("([^。…？！])　", "$1"), SUB("^　", ""), SUB("#n", ""), SUB("#\\w+\\[\\d+\\]|!", ""), END};
static const LunaOp f_FPCSG00912[] = {DEL("%N"), END};
static const LunaOp f_FPCSG00706[] = {SUBW("<br>", ""), END};
static const LunaOp f_FPCSG00696[] = {DEL("㌔"), DEL("㍉"), /* \x87\x60, \x87\x5f */ END};
static const LunaOp f_PCSG00472[] = {
    REP("ゞ", "!?"), REP("ゝ", "!!"), DEL("　"), /* \x81\x55, \x81\x54, \x81\x40 */
    SUB("(#n)+", ""), SUB("#[A-Za-z]+\\[(\\d*[.,])?\\d+\\]", ""), END};
static const LunaOp f_PCSG00272[] = {SPECIAL(OP_WRAP), CALL(PCSG00472), END};
static const LunaOp f_FPCSG00405[] = {SUB("[\\s]", ""), END};
static const LunaOp f_PCSG00172[] = {
    /* strAW(); the U+FFFD in two of them are Windows' stand-in for bytes it cannot decode: the port drops
       such bytes, so those two never match */
    DEL("Mﾚ      　ｰ_Mﾚ  \xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd"), DEL("u_"), DEL("ata/data4.dat"), DEL(":data/data4.dat"),
    DEL("@"), DEL("\xef\xbf\xbd"), DEL("∥pp0:d"), DEL("app0:d:d"), END};
static const LunaOp f_PCSG00776[] = {DEL("\x02"), SPECIAL(OP_TRIM), END};
static const LunaOp f_FPCSG00468[] = {SUB("\\\\n(　)*|\\\\k", ""), SUB("\\[|\\*[^\\]]+]", ""), SUB("×", ""), END};
static const LunaOp f_FPCSG00808[] = {
    SUB("^\\s+|\\s+$", ""), SUB("\\s*(#n)*\\s*", ""), SUB("#\\w+(\\[.+?\\])?", ""), END};
static const LunaOp f_F010088B01A8FC000[] = {
    SUB("#\\w+(\\[.+?\\])?", ""),
    SUB_IF(E_U8, "　", ""), /* u8"　": never in CP932 text */
    END};
static const LunaOp f_PCSG01036[] = {DEL("#n"), END};
static const LunaOp f_PCSG00402[] = {
    SUB("(　)*(#n)*(　)*", ""), /* R"((\x81\x40)*(#n)*(\x81\x40)*)" */
    SUB("#\\w+(\\[.+?\\])?", ""), END};
static const LunaOp f_FPCSG00815[] = {SUB("\\s*(#n)*\\s*", ""), SUB("#\\w+(\\[.+?\\])?", ""), END};
static const LunaOp f_PCSG01250_T[] = {
    PREFIX("<text"), CUT(6, 1), SUB("/ruby:(.*?)&(.*?)/", "$1"), SUB("#n", ""), END};
static const LunaOp f_PCSG01250_N1[] = {PREFIX("<name"), CUT(7, 2), END}; /* PCSG01250_N<1>: substr(7, size - 9) */
static const LunaOp f_PCSG01250_N0[] = {PREFIX("<name"), CUT(6, 1), END}; /* PCSG01250_N<0> */
static const LunaOp f_PCSG01202[] = {SUB("/ruby:(.*?)&(.*?)/", "$1"), END};
static const LunaOp f_PCSG01325[] = {SUB("@[_\\*\\d\\w]*", ""), END};
static const LunaOp f_PCSG00431[] = {DEL("#n　"), DEL("#n"), END}; /* "#n\x81\x40" */
static const LunaOp f_FPCSG00855[] = {
    SUB_IF(E_U8, "#n(　)*", ""), /* u8R"(#n(　)*)" */
    SUB_IF(E_SJ, "#n", ""),      /* the same pattern on CP932 bytes: its UTF-8 　 never matches */
    SUB("#\\w.+?\\]", ""), END};
static const LunaOp f_PCSG01178[] = {CALL(FPCSG00855), DEL("  "), END};
static const LunaOp f_FPCSG00855_2[] = {
    REP("Χ", "、"), REP("Δ", "。"), REP("Λ", "っ"), REP("《", "（"), REP("》", "）"), REP("∫", "「"),
    REP("∨", "」"), REP("∴", "『"), REP("∵", "』"), REP("П", "【"), REP("Ц", "】"), CALL(FPCSG00855), END};
static const LunaOp f_FPCSG00477[] = {SUB("#n(　)*", ""), /* R"(#n(\x81\x40)*)" */ SUB("#\\w.+?\\]", ""), END};
static const LunaOp f_PCSG00654[] = {DEL("\\"), DEL("$"), END};
static const LunaOp f_PCSG00829[] = {DEL("\\n　"), DEL("\\n"), SUBW("\\[(.*?),\\d\\]", ""), END};
static const LunaOp f_PCSG00535_1[] = {DEL("\\f"), REP("\\n", "\n"), END};
static const LunaOp f_PCSG01114[] = {DEL("@"), BETWEEN("{", "}"), END};
static const LunaOp f_PCSG00482[] = {DEL("▼\f"), DEL("\n"), END}; /* u8"▼\f": E2 96 BC 0C, a page end */
static const LunaOp f_PCSG01249[] = {DEL("[n]"), END};
static const LunaOp f_PCSG00370[] = {DEL("\n　"), DEL("▼"), DEL("＞"), DEL("\n"), /* \x81\xa5, \x81\x84 */ END};
static const LunaOp f_PCSG01063[] = {DEL("\n　"), DEL("\n"), END};
static const LunaOp f_PCSG01289[] = {
    SPECIAL(OP_KANA), SUBW("\\$t(.*?)@", "$1"), SUBW("\\$\\[(.*?)\\$/(.*?)\\$\\]", "$1"), END};
static const LunaOp f_PCSG01198[] = {
    SPECIAL(OP_KANA), SUBW("`(.*?)@", "$1"), SUBW("\\$\\[(.*?)\\$/(.*?)\\$\\]", "$1"), END};
static const LunaOp f_PCSG00585[] = {
    SPECIAL(OP_KANA), SUBW("\\$\\[(.*?)\\$/(.*?)\\$\\]", "$1"), SUBW("\\$C\\[ffffff\\](.*?)@\\$C\\[\\]", "$1"),
    DEL("\n"), END};
static const LunaOp f_PCSG01254[] = {SPECIAL(OP_KANA), SUBW("@[a-zA-Z\\d_\\.]+", ""), END};
static const LunaOp f_PCSG01196[] = {REP("^", "\n"), SUBW("<(.*?)>(.*?)|", "$2"), END};
static const LunaOp f_PCSG00780[] = {DEL("^"), SUBW("<(.*?),(.*?)>", "$1"), END};
static const LunaOp f_PCSG01151[] = {DEL("^"), DEL("　"), END};
static const LunaOp f_PCSG00664[] = {SUB("(　)*\\[n\\](　)*", ""), /* R"((\x81\x40)*\[n\](\x81\x40)*)" */ END};
static const LunaOp f_FPCSG01066[] = {SUB("\n(　)*", ""), END};
static const LunaOp f_PCSG01314[] = {SUB("@r(.*?)@(.*?)@", "$1"), SUB("@[\\w\\d_]+", ""), END};
static const LunaOp f_F010052300F612000[] = {
    SUB("#r(.*?)\\|(.*?)#", "$1"), SUB("@r(.*?)@\\d", "$1"), DEL("\\c"), DEL("\\n"), END};
static const LunaOp f_PCSG01144_1[] = {DEL("@w"), DEL("@y"), DEL("　"), PREFIX("【"), END};
static const LunaOp f_PCSG00449[] = {DEL("@w"), DEL("　"), SPECIAL(OP_P00449), END};
static const LunaOp f_PCSG01197[] = {SUB("#r(.*?)\\|(.*?)#", "$1"), DEL("@w"), END};
static const LunaOp f_PCSG00433[] = {DEL("\n"), SUBW("<CLT \\d+>(.*?)<CLT>", "$1"), END};
static const LunaOp f_PCSG00020[] = {SUB("<\\w+?>", ""), END};
static const LunaOp f_PCSG00062[] = {
    SUBW("\\[(.*?)/(.*?)\\]", "$1"), SUBW("<[#A-Za-z0-9\\.\"_/]*>", ""), REP("\\n", "\n"), END};
static const LunaOp f_PCSG01049[] = {SUBW("<br>　*<br>", "\n"), SUBW("　*<br>　*", ""), END};
static const LunaOp f_PCSG00708[] = {DEL("\\"), DEL("$"), END};

static const LunaOp *const vjo_luna_filters[VJO_LF_COUNT] = {
    [VJO_LF_NEWLINE] = f_NEWLINE, [VJO_LF_IDENTITY] = f_IDENTITY, [VJO_LF_ALL_ASCII] = f_ALL_ASCII,
    [VJO_LF_BACKSLASH] = f_BACKSLASH, [VJO_LF_CARET] = f_CARET, [VJO_LF_FPCSG01124] = f_FPCSG01124,
    [VJO_LF_FPCSG01023] = f_FPCSG01023,
    [VJO_LF_FPCSG01282] = f_FPCSG01282, [VJO_LF_FPCSG00410] = f_FPCSG00410, [VJO_LF_FPCSG00448] = f_FPCSG00448,
    [VJO_LF_PCSG01203] = f_PCSG01203, [VJO_LF_PCSG00401] = f_PCSG00401, [VJO_LF_PCSG00766] = f_PCSG00766,
    [VJO_LF_PCSG00935] = f_PCSG00935, [VJO_LF_FPCSG00903] = f_FPCSG00903, [VJO_LF_PCSG00615] = f_PCSG00615,
    [VJO_LF_PCSG01001] = f_PCSG01001, [VJO_LF_PCSG01247] = f_PCSG01247, [VJO_LF_PCSG01144] = f_PCSG01144,
    [VJO_LF_FPCSG01180] = f_FPCSG01180, [VJO_LF_PCSG01002] = f_PCSG01002, [VJO_LF_FPCSG00839] = f_FPCSG00839,
    [VJO_LF_FPCSG00751] = f_FPCSG00751, [VJO_LF_FPCSG00401] = f_FPCSG00401, [VJO_LF_PCSG00530] = f_PCSG00530,
    [VJO_LF_PCSG00826] = f_PCSG00826, [VJO_LF_PCSG00592] = f_PCSG00592, [VJO_LF_PCSG00833] = f_PCSG00833,
    [VJO_LF_PCSG00855] = f_PCSG00855, [VJO_LF_PCSG01150] = f_PCSG01150, [VJO_LF_PCSG01260_T] = f_PCSG01260_T,
    [VJO_LF_PCSG01260] = f_PCSG01260, [VJO_LF_PCSG01204] = f_PCSG01204, [VJO_LF_PCSG01068] = f_PCSG01068,
    [VJO_LF_PCSG00367] = f_PCSG00367, [VJO_LF_PCSG01167] = f_PCSG01167, [VJO_LF_PCSG00917] = f_PCSG00917,
    [VJO_LF_PCSG00938] = f_PCSG00938, [VJO_LF_FPCSG00912] = f_FPCSG00912, [VJO_LF_FPCSG00706] = f_FPCSG00706,
    [VJO_LF_FPCSG00696] = f_FPCSG00696, [VJO_LF_PCSG00472] = f_PCSG00472, [VJO_LF_PCSG00272] = f_PCSG00272,
    [VJO_LF_FPCSG00405] = f_FPCSG00405, [VJO_LF_PCSG00172] = f_PCSG00172, [VJO_LF_PCSG00776] = f_PCSG00776,
    [VJO_LF_FPCSG00468] = f_FPCSG00468, [VJO_LF_FPCSG00808] = f_FPCSG00808,
    [VJO_LF_F010088B01A8FC000] = f_F010088B01A8FC000, [VJO_LF_PCSG01036] = f_PCSG01036,
    [VJO_LF_PCSG00402] = f_PCSG00402, [VJO_LF_FPCSG00815] = f_FPCSG00815, [VJO_LF_PCSG01250_T] = f_PCSG01250_T,
    [VJO_LF_PCSG01250_N1] = f_PCSG01250_N1, [VJO_LF_PCSG01250_N0] = f_PCSG01250_N0,
    [VJO_LF_PCSG01202] = f_PCSG01202, [VJO_LF_PCSG01325] = f_PCSG01325, [VJO_LF_PCSG00431] = f_PCSG00431,
    [VJO_LF_FPCSG00855] = f_FPCSG00855, [VJO_LF_PCSG01178] = f_PCSG01178, [VJO_LF_FPCSG00855_2] = f_FPCSG00855_2,
    [VJO_LF_FPCSG00477] = f_FPCSG00477, [VJO_LF_PCSG00654] = f_PCSG00654, [VJO_LF_PCSG00829] = f_PCSG00829,
    [VJO_LF_PCSG00535_1] = f_PCSG00535_1, [VJO_LF_PCSG01114] = f_PCSG01114, [VJO_LF_PCSG00482] = f_PCSG00482,
    [VJO_LF_PCSG01249] = f_PCSG01249, [VJO_LF_PCSG00370] = f_PCSG00370, [VJO_LF_PCSG01063] = f_PCSG01063,
    [VJO_LF_PCSG01289] = f_PCSG01289, [VJO_LF_PCSG01198] = f_PCSG01198, [VJO_LF_PCSG00585] = f_PCSG00585,
    [VJO_LF_PCSG01254] = f_PCSG01254, [VJO_LF_PCSG01196] = f_PCSG01196, [VJO_LF_PCSG00780] = f_PCSG00780,
    [VJO_LF_PCSG01151] = f_PCSG01151, [VJO_LF_PCSG00664] = f_PCSG00664, [VJO_LF_FPCSG01066] = f_FPCSG01066,
    [VJO_LF_PCSG01314] = f_PCSG01314, [VJO_LF_F010052300F612000] = f_F010052300F612000,
    [VJO_LF_PCSG01144_1] = f_PCSG01144_1, [VJO_LF_PCSG00449] = f_PCSG00449, [VJO_LF_PCSG01197] = f_PCSG01197,
    [VJO_LF_PCSG00433] = f_PCSG00433, [VJO_LF_PCSG00020] = f_PCSG00020, [VJO_LF_PCSG00062] = f_PCSG00062,
    [VJO_LF_PCSG01049] = f_PCSG01049, [VJO_LF_PCSG00708] = f_PCSG00708,
    /* PCSG00451, PCSG01152: not ported (NULL) */
};

/* A hookfunc's own clean-up after its read (Luna does it in the hookfunc, before
 * commonfilter and the site's filter). */
static const LunaOp p_PCSG01046[] = {DEL("<br>"), END};                       /* StringFilter(L"<br>") */
static const LunaOp p_PCSG00992[] = {DEL("\n"), END};                        /* strReplace(text, "\n") */
static const LunaOp p_PCSG01011[] = {DEL("\n"), AFTER("×"), END};            /* its duplicate check: left out */
static const LunaOp p_PCSG00914[] = {DEL("㌻"), REP("㌫", "七海"), REP("㍊", "春歌"), END}; /* \x87\x6e, \x87\x6c, \x87\x6d */
static const LunaOp p_PCSG00769[] = {REP("㊤", "―"), REP("㊥", "―"), REP("㊦", "―"), DEL("　"), DEL("\n"), END};
static const LunaOp *const vjo_luna_post[VJO_LH_COUNT] = {
    [VJO_LH_PCSG01046] = p_PCSG01046, [VJO_LH_PCSG00992] = p_PCSG00992, [VJO_LH_PCSG01011] = p_PCSG01011,
    [VJO_LH_PCSG00914] = p_PCSG00914, [VJO_LH_PCSG00769] = p_PCSG00769,
};

/* ---------------- Rubi's own filters (VjoPlaceVerified.filter) ---------------- */

/* Written from bee-san/psandvita's Vita3K runs (hooks/vita/<title>/RUBI-HOOK.md, hook-text-artifacts.jsonl):
 * each code below was seen in the hook's bytes there and compared with the native frame the game drew.
 * Proven in Vita3K only. Ruby keeps its base (the word the dictionary looks up), as Luna's filters do; a
 * line-break code becomes "\n" (what the game draws); a player-name code the hook sees before the game
 * expands it becomes the game's default name (Rubi cannot know a name the player typed: such a line then
 * differs from the screen by that name only). docs/native-hooks.md#rubis-filters has the evidence. */
_Static_assert(VJO_LF_COUNT <= VJO_RF_BASE, "Luna's filters and Rubi's share one byte");
#define RCALL(f) {OP_CALL, 0, VJO_RF_##f, NULL, NULL}

/* グリザイアの果実 PCSG00206, its spin-out PCSG00620 and リトルバスターズ！ Converted Edition PCSG00007 (the
 * PROTOTYPE engine, as Luna's PCSG01198/PCSG01289): plain hiragana and punctuation are half-width kana
 * bytes (OP_KANA: ｷｮｰｽｹ -> きょーすけ); $t<name>$d is a name in its own nameplate (PCSG00007 event 3),
 * $t<name>@ a name drawn inline (PCSG00206); $d a line break; $[base$/reading$] ruby (PCSG00206 event
 * 367); $L1 (config labels), $A1, $S<nnn>..$S, $w, $C[rrggbb] are not drawn (PCSG00620 event 81); ＿ is a
 * blank indent and leading line breaks place the text (PCSG00620 event 80). */
static const LunaOp r_PROTOTYPE[] = {
    SPECIAL(OP_KANA), SUB("\\$t([^$@\n]*)\\$d", "$1\n"), SUB("\\$t([^$@\n]*)@", "$1"), REP("$d", "\n"),
    SUB("\\$\\[(.*?)\\$/(.*?)\\$\\]", "$1"), SUB("\\$[LA]\\d|\\$S\\d\\d\\d|\\$S|\\$w|\\$C\\[[0-9A-Fa-f]*\\]", ""),
    REP("＿", "　"), SPECIAL(OP_TRIM), END};
/* The IF:Lib GAME-ENGINE 3.00 titles (Otomate, Rejet, Design Factory): #Ruby[base,reading] (PCSG00935
 * event 1); #Scale[1.0], #Color[7], #Speed[5], #Word[..], #Pos[x, y] and other #Tag[..] are not drawn
 * (PCSG00448 event 1, PCSG00853 sample 000001); #n is a line break, also before a Latin word (PCSG00448
 * event 54 #nLAGRANGE); a trailing #n ends every page and draws nothing. */
static const LunaOp r_IFENGINE[] = {
    SUB("#Ruby\\[([^,\\]]*),[^\\]]*\\]", "$1"), SUB("#[A-Z][A-Za-z]*(\\[[^\\]\n]*\\])?", ""), REP("#n", "\n"),
    SPECIAL(OP_TRIM), END};
/* <base,reading>: the base drawn, the reading above it (PCSG00648 sample 000005, PCSG00509 event 50). */
static const LunaOp r_ANGLE_RUBY[] = {SUB("<([^<>,\n]*),[^<>\n]*>", "$1"), END};
/* Rejet's Unity history strings (剣が君 for V PCSG00553, Dance with Devils PCSG00807): <br> is a line break
 * (PCSG00807 sample 000002); leading full-width spaces centre a line. */
static const LunaOp r_BR_NEWLINE[] = {REP("<br>", "\n"), SPECIAL(OP_TRIM), END};
/* 鏡界の白雪 PCSG00787: a lone backslash is the line break (event 1). */
static const LunaOp r_BACKSLASH_NEWLINE[] = {REP("\\", "\n"), SPECIAL(OP_TRIM), END};
/* PCSG00448: the hook is before the #Name expander; 片岡 ユエ, the default name, was drawn for #Name[1]#Name[2]
 * (event 208, sample 000206). */
static const LunaOp r_PCSG00448[] = {REP("#Name[1]", "片岡"), REP("#Name[2]", "ユエ"), RCALL(IFENGINE), END};
/* PCSG00450: 《SP QUE,1,NNNN》 voice-sync cues are never drawn (script.dat, 355 uses). */
static const LunaOp r_PCSG00450[] = {SUB("《SP [^》]*》", ""), END};
/* PCSG00469: #Name[1] is drawn as the default 撫子 (samples 000080, 000182, 001345). */
static const LunaOp r_PCSG00469[] = {REP("#Name[1]", "撫子"), RCALL(IFENGINE), END};
/* PCSG00509: ＊Ｂ is drawn as the heroine's default name 鈴 (event 1011, 67 approved pages hold it); ^ is
 * a line break (Luna's FPCSG00852 drops it); <base,reading> ruby (event 50). */
static const LunaOp r_PCSG00509[] = {REP("＊Ｂ", "鈴"), REP("^", "\n"), RCALL(ANGLE_RUBY), END};
/* PCSG00540 (ADV::MSGSET): c<id>.<nameplate>r[v<voice>.]<body>, narration r<body>; w<ms>. a timed pause;
 * b<reading>.<base> ruby; plain hiragana are half-width kana, and ･ ﾞ ﾟ are drawn … ！ ？ (events 1-5, 51,
 * 53, 55). The name goes on its own line. */
static const LunaOp r_PCSG00540[] = {
    SUB("^c\\d+\\.([^\n]*?)r", "$1\n"), SUB("^r", ""), SUB("^v[^.\n]*\\.", ""), SUB("\nv[^.\n]*\\.", "\n"),
    SUB("w\\d+\\.", ""), SUB("b[^.<>\n]*\\.<([^<>\n]*)>", "$1"), SPECIAL(OP_KANA), END};
/* PCSG00817: [n] is a line break; [wNNN] [hNNN] [cRRGGBB] [e] and the other [x] codes are not drawn
 * (session 458160a9, events 1-59, event 432). */
static const LunaOp r_PCSG00817[] = {REP("[n]", "\n"), SUB("\\[[a-z@][0-9a-fA-F]*\\]", ""), END};
/* PCSG00853: defaults 花宮 かずは and カズハ drawn for #Name[1], #Name[2], #Name[3] (samples 000078, 000120,
 * 000259, 000371, 000464, 000492). */
static const LunaOp r_PCSG00853[] = {
    REP("#Name[1]", "花宮"), REP("#Name[2]", "かずは"), REP("#Name[3]", "カズハ"), RCALL(IFENGINE), END};
/* PCSG00876: @@ is a line break, $4..$0 switch the style (scn op 0x10 records); %first and the other name
 * codes are expanded before the hook. */
static const LunaOp r_PCSG00876[] = {REP("@@", "\n"), SUB("\\$\\d", ""), END};
/* PCSG01194 (shin engine, @-markup build): <name>@r[@p<n>.][@v<voice>.]<body>, narration starts with @r; @r is a
 * line break, @p<n>. and @v<voice>. are not drawn; plain hiragana are half-width kana, ･ ﾞ ﾟ drawn … ！ ？
 * (events 1, 11, 21-23; main.snr: 10,909 @v, 2,242 @p). Luna lists the site with no filter ("too complex"). */
static const LunaOp r_PCSG01194[] = {
    SUB("@v[^.@\n]*\\.", ""), SUB("@p\\d+\\.", ""), REP("@r", "\n"), SPECIAL(OP_KANA), SPECIAL(OP_TRIM), END};

static const LunaOp *const vjo_rubi_filters[VJO_RF_END - VJO_RF_BASE] = {
    [VJO_RF_PROTOTYPE - VJO_RF_BASE] = r_PROTOTYPE, [VJO_RF_IFENGINE - VJO_RF_BASE] = r_IFENGINE,
    [VJO_RF_ANGLE_RUBY - VJO_RF_BASE] = r_ANGLE_RUBY, [VJO_RF_BR_NEWLINE - VJO_RF_BASE] = r_BR_NEWLINE,
    [VJO_RF_BACKSLASH_NEWLINE - VJO_RF_BASE] = r_BACKSLASH_NEWLINE, [VJO_RF_PCSG00448 - VJO_RF_BASE] = r_PCSG00448,
    [VJO_RF_PCSG00450 - VJO_RF_BASE] = r_PCSG00450, [VJO_RF_PCSG00469 - VJO_RF_BASE] = r_PCSG00469,
    [VJO_RF_PCSG00509 - VJO_RF_BASE] = r_PCSG00509, [VJO_RF_PCSG00540 - VJO_RF_BASE] = r_PCSG00540,
    [VJO_RF_PCSG00817 - VJO_RF_BASE] = r_PCSG00817, [VJO_RF_PCSG00853 - VJO_RF_BASE] = r_PCSG00853,
    [VJO_RF_PCSG00876 - VJO_RF_BASE] = r_PCSG00876, [VJO_RF_PCSG01194 - VJO_RF_BASE] = r_PCSG01194,
};

/* A filter's ops: Luna's (VJO_LF_*) or Rubi's (VJO_RF_*); NULL for none or an unknown one. */
static const LunaOp *filter_ops(unsigned f)
{
    if (f > VJO_LF_NONE && f < VJO_LF_COUNT) return vjo_luna_filters[f];
    if (f >= VJO_RF_BASE && f < VJO_RF_END) return vjo_rubi_filters[f - VJO_RF_BASE];
    return NULL;
}

/* ---------------- job time: the text and its ops ---------------- */

typedef struct {
    char *cur, *alt; /* the text, and the other buffer (cap bytes each) */
    size_t len, cap;
    unsigned enc; /* the site's encoding: what Luna's buffer held */
    unsigned long budget;
    int bad; /* a step failed: the hit gives no text */
} Txt;

static size_t u8len(const char *s, size_t n)
{
    unsigned b = (unsigned char)s[0];
    size_t k = b < 0xC0 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
    return k < n ? k : n;
}

/* Appends len bytes, or the whole UTF-8 characters of them that fit: 0 when cut. */
static int put(char *out, size_t cap, size_t *w, const char *src, size_t len)
{
    size_t room = cap - 1 - *w, k;
    if (len <= room) {
        memcpy(out + *w, src, len);
        *w += len;
        return 1;
    }
    k = room;
    while (k && ((unsigned char)src[k] & 0xC0) == 0x80) k--;
    memcpy(out + *w, src, k);
    *w += k;
    return 0;
}

static void done(Txt *t, size_t w)
{
    char *x = t->cur;
    t->alt[w] = 0;
    t->cur = t->alt;
    t->alt = x;
    t->len = w;
}

static int starts(const Txt *t, const char *a)
{
    size_t n = strlen(a);
    return t->len >= n && !memcmp(t->cur, a, n);
}

static int ends(const Txt *t, const char *a)
{
    size_t n = strlen(a);
    return t->len >= n && !memcmp(t->cur + t->len - n, a, n);
}

/* the first a at or after from, or -1 */
static long find(const Txt *t, size_t from, const char *a, size_t na)
{
    for (size_t i = from; na && i + na <= t->len; i++)
        if (!memcmp(t->cur + i, a, na)) return (long)i;
    return -1;
}

/* every a (from the left, not overlapping) replaced by b, after prefix pre */
static void op_rep(Txt *t, const char *a, const char *b, const char *pre)
{
    size_t na = strlen(a), nb = strlen(b), i = 0, w = 0;
    int ok = put(t->alt, t->cap, &w, pre, strlen(pre));
    if (!na) return;
    while (ok && i < t->len) {
        if (i + na <= t->len && !memcmp(t->cur + i, a, na)) {
            ok = put(t->alt, t->cap, &w, b, nb);
            i += na;
        } else {
            size_t k = u8len(t->cur + i, t->len - i);
            ok = put(t->alt, t->cap, &w, t->cur + i, k);
            i += k;
        }
    }
    done(t, w);
}

static void op_sub(Txt *t, const char *pat, const char *rep, unsigned flags)
{
    int r = vjo_re_sub(pat, flags, t->cur, t->len, rep, t->alt, t->cap, &t->budget);
    if (r < 0) {
        t->bad = 1;
        return;
    }
    done(t, (size_t)r);
}

/* One UTF-8 character at s (valid UTF-8: the decoder's output). */
static uint32_t u8get(const char *s, size_t n, size_t *len)
{
    const unsigned char *u = (const unsigned char *)s;
    size_t k = u8len(s, n);
    uint32_t v = k == 1 ? u[0] : k == 2 ? u[0] & 0x1Fu : k == 3 ? u[0] & 0x0Fu : u[0] & 0x07u;
    for (size_t i = 1; i < k; i++) v = v << 6 | (u[i] & 0x3Fu);
    *len = k;
    return v;
}

static size_t u8put(uint32_t cp, char *o)
{
    if (cp < 0x80) {
        o[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        o[0] = (char)(0xC0 | cp >> 6);
        o[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | cp >> 12);
        o[1] = (char)(0x80 | (cp >> 6 & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | cp >> 18);
    o[1] = (char)(0x80 | (cp >> 12 & 0x3F));
    o[2] = (char)(0x80 | (cp >> 6 & 0x3F));
    o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* remapkatakana: Luna's katakanaMap, character by character */
static void op_kana(Txt *t)
{
    size_t i = 0, w = 0;
    int ok = 1;
    while (ok && i < t->len) {
        size_t k;
        char o[4];
        uint32_t cp = u8get(t->cur + i, t->len - i, &k);
        if (cp >= 0xFF61 && cp <= 0xFF9F) cp = vjo_luna_kana[cp - 0xFF61];
        else
            for (unsigned e = 0; e < sizeof vjo_luna_kana_extra / sizeof vjo_luna_kana_extra[0]; e++)
                if (cp == vjo_luna_kana_extra[e][0]) {
                    cp = vjo_luna_kana_extra[e][1];
                    break;
                }
        ok = put(t->alt, t->cap, &w, o, u8put(cp, o));
        i += k;
    }
    done(t, w);
}

/* Trim(): Windows' iswspace at both ends; of the white space CP932 text can
 * hold (its only user, PCSG00776, is CP932): tab to CR, space and U+3000. */
static void op_trim(Txt *t)
{
    size_t a = 0, b = t->len;
    for (;;) {
        if (a < b && ((t->cur[a] >= 9 && t->cur[a] <= 13) || t->cur[a] == ' ')) a++;
        else if (b - a >= 3 && !memcmp(t->cur + a, "　", 3)) a += 3;
        else break;
    }
    for (;;) {
        if (b > a && ((t->cur[b - 1] >= 9 && t->cur[b - 1] <= 13) || t->cur[b - 1] == ' ')) b--;
        else if (b - a >= 3 && !memcmp(t->cur + b - 3, "　", 3)) b -= 3;
        else break;
    }
    memmove(t->cur, t->cur + a, b - a);
    t->len = b - a;
}

/* substr(from, size - from - drop) with Luna's CP932 byte counts: a text shorter
 * than from is dropped (substr throws); a count that wraps keeps the rest; a
 * character a cut falls inside goes too. */
static void op_cut(Txt *t, unsigned from, unsigned drop)
{
    size_t total = 0, i, at = 0, w = 0;
    for (i = 0; i < t->len;) {
        size_t k;
        uint32_t cp = u8get(t->cur + i, t->len - i, &k);
        total += cp < 0x80 || (cp >= 0xFF61 && cp <= 0xFF9F) ? 1 : 2;
        i += k;
    }
    if (total < from) {
        t->len = 0;
        return;
    }
    {
        size_t end = total >= from + drop ? total - drop : total;
        for (i = 0; i < t->len;) {
            size_t k, width;
            uint32_t cp = u8get(t->cur + i, t->len - i, &k);
            width = cp < 0x80 || (cp >= 0xFF61 && cp <= 0xFF9F) ? 1 : 2;
            if (at >= from && at + width <= end) {
                memmove(t->cur + w, t->cur + i, k);
                w += k;
            }
            at += width;
            i += k;
        }
    }
    t->len = w;
}

/* PCSG00935: re::search(s, pat); found: every copy of the match removed and
 * "【" + its group 1 + "】" in front */
static void op_namepos(Txt *t, const char *pat)
{
    long m[4];
    size_t w = 0, na, i = 0;
    int r = vjo_re_search(pat, t->enc == VJO_HOOK_ENC_UTF8 ? VJO_RE_BYTES : 0, t->cur, t->len, 1, m, &t->budget);
    int ok;
    if (r < 0) t->bad = 1;
    if (r <= 0) return;
    na = (size_t)(m[1] - m[0]);
    ok = put(t->alt, t->cap, &w, "【", 3);
    if (ok && m[2] >= 0) ok = put(t->alt, t->cap, &w, t->cur + m[2], (size_t)(m[3] - m[2]));
    if (ok) ok = put(t->alt, t->cap, &w, "】", 3);
    while (ok && i < t->len) {
        if (na && i + na <= t->len && !memcmp(t->cur + i, t->cur + m[0], na)) {
            i += na;
        } else {
            size_t k = u8len(t->cur + i, t->len - i);
            ok = put(t->alt, t->cap, &w, t->cur + i, k);
            i += k;
        }
    }
    done(t, w);
}

/* StringFilterBetween(a, b): from each a to the next b after it, both included */
static void op_between(Txt *t, const char *a, const char *b)
{
    size_t na = strlen(a), nb = strlen(b);
    long at = find(t, 0, a, na);
    while (at >= 0) {
        long end = find(t, (size_t)at + na, b, nb);
        size_t gone;
        if (end < 0) break;
        gone = (size_t)end + nb - (size_t)at;
        memmove(t->cur + at, t->cur + at + gone, t->len - (size_t)at - gone);
        t->len -= gone;
        at = find(t, (size_t)at, a, na);
    }
}

static void run_ops(Txt *t, const LunaOp *op, unsigned depth)
{
    for (; op && op->op != OP_END && !t->bad && t->len; op++) {
        long at;
        if (op->enc && !(op->enc & (1u << t->enc))) continue;
        switch (op->op) {
        case OP_DEL:
        case OP_REP:
            op_rep(t, op->a, op->b, "");
            break;
        case OP_SUB:
            op_sub(t, op->a, op->b, t->enc == VJO_HOOK_ENC_UTF8 ? VJO_RE_BYTES : 0);
            break;
        case OP_SUBW:
            op_sub(t, op->a, op->b, 0);
            break;
        case OP_CALL:
            if (depth < 3) run_ops(t, filter_ops(op->arg), depth + 1);
            break;
        case OP_IFEQ:
            if (t->len == strlen(op->a) && !memcmp(t->cur, op->a, t->len)) t->len = 0;
            break;
        case OP_PREFIX:
            if (!starts(t, op->a)) t->len = 0;
            break;
        case OP_UPTO:
            at = find(t, 0, op->a, strlen(op->a));
            t->len = at < 0 ? 0 : (size_t)at + strlen(op->a);
            break;
        case OP_AFTER:
            at = find(t, 0, op->a, strlen(op->a));
            if (at >= 0) {
                size_t from = (size_t)at + strlen(op->a);
                memmove(t->cur, t->cur + from, t->len - from);
                t->len -= from;
            }
            break;
        case OP_CUT:
            op_cut(t, op->arg & 15u, op->arg >> 4);
            break;
        case OP_ENDDROP:
            if (ends(t, op->a)) t->len--;
            break;
        case OP_P00449: /* if (!startWith(s, 【)) clear; if (endWith(s, "@")) from(s minus its last byte) */
            if (ends(t, "@")) t->len--;
            else if (!starts(t, "【")) t->len = 0;
            break;
        case OP_WRAP: /* if (!startWith(s, "#n#") && s.find("#n#") != npos): #n# -> 】#, 【 in front */
            if (!starts(t, "#n#") && find(t, 0, "#n#", 3) >= 0) op_rep(t, "#n#", "】#", "【");
            break;
        case OP_NAMEPOS:
            op_namepos(t, op->a);
            break;
        case OP_TRIM:
            op_trim(t);
            break;
        case OP_KANA:
            op_kana(t);
            break;
        case OP_ASCII: {
            size_t i = 0;
            while (i < t->len && !((unsigned char)t->cur[i] & 0x80)) i++;
            if (i == t->len) t->len = 0;
            break;
        }
        case OP_BETWEEN:
            op_between(t, op->a, op->b);
            break;
        default:
            t->bad = 1;
            break;
        }
    }
}

int vjo_luna_filter_known(unsigned filter)
{
    return filter == VJO_LF_NONE || filter_ops(filter) != NULL;
}

int vjo_luna_ported(const VjoLunaSite *s)
{
    return s && s->ported && s->func < VJO_LH_COUNT && s->filter < VJO_LF_COUNT &&
           (s->filter == VJO_LF_NONE || vjo_luna_filters[s->filter]);
}

/* The decoded text, s's hookfunc clean-up (s may be NULL), commonfilter, then filter's ops. */
static size_t text_with(const VjoLunaSite *s, unsigned filter, unsigned site_encoding, const uint8_t *raw,
                        unsigned n, unsigned encoding, char *out, size_t cap)
{
    char tmp[VJO_LUNA_TEXT_MAX];
    Txt t;
    t.cap = cap < sizeof tmp ? cap : sizeof tmp;
    t.cur = out;
    t.alt = tmp;
    t.enc = site_encoding;
    t.budget = LUNA_TEXT_BUDGET;
    t.bad = 0;
    t.len = vjo_hook_decode(encoding, raw, n, out, t.cap, NULL);
    if (s) run_ops(&t, vjo_luna_post[s->func], 0);
    /* commonfilter: a CP932 hit that is the one character ▽ or ▼ (2 bytes) */
    if (t.enc == VJO_HOOK_ENC_CP932 && t.len == 3 && (!memcmp(t.cur, "▽", 3) || !memcmp(t.cur, "▼", 3))) t.len = 0;
    if (filter) run_ops(&t, filter_ops(filter), 0);
    if (t.bad) t.len = 0;
    if (t.cur != out) memcpy(out, t.cur, t.len);
    out[t.len] = 0;
    return t.len;
}

size_t vjo_luna_text(const VjoLunaSite *s, const uint8_t *raw, unsigned n, unsigned encoding, char *out,
                     size_t cap)
{
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!s || !raw || !vjo_luna_ported(s)) return 0;
    return text_with(s, s->filter, s->encoding, raw, n, encoding, out, cap);
}

size_t vjo_luna_text_filter(const VjoLunaSite *s, unsigned filter, unsigned site_encoding, const uint8_t *raw,
                            unsigned n, unsigned encoding, char *out, size_t cap)
{
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!raw || (s && !vjo_luna_ported(s)) || !vjo_luna_filter_known(filter)) return 0;
    return text_with(s, filter, site_encoding, raw, n, encoding, out, cap);
}
