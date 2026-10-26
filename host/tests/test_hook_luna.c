/* LunaTranslator's Vita table and its ports (core/hook_luna.c, core/hook_re.c;
 * docs/hook-titles.md): the table (counts, every title's sites one slice, the lookups,
 * CLANNAD absent, the version gate), vjo_luna_capture on a fake memory image (the
 * default read, padding, DATA_INDIRECT, USING_CHAR, refused reads, the cap, every ported
 * hookfunc, the reader's call bound), every ported filter on examples made from Luna's
 * own patterns and comments, and the regex subset against Python's re
 * (host/tests/hook_re_vectors.inc, tools/gen_hook_re_vectors.py). Pure C: also on the
 * Vita's CPU under qemu-arm (tools/arm_host_tests.sh). */
#include <stdio.h>
#include <string.h>

#include "acutest.h"
#include "hook_luna.h"
#include "hook_re.h"

#include "hook_luna_table.inc" /* the test's own copy of the table, the enums */
#include "hook_re_vectors.inc"

/* ---- helpers ---- */

static const VjoLunaSite *site(const char *id, uint32_t addr)
{
    const VjoLunaSite *s = NULL;
    unsigned n = vjo_luna_sites(id, &s);
    for (unsigned i = 0; i < n; i++)
        if (s[i].addr == addr) return &s[i];
    TEST_CHECK_(0, "no site %s 0x%08X", id, (unsigned)addr);
    return NULL;
}

/* UTF-8 -> CP932 through the decoder itself: the first cell that decodes to a character */
static uint16_t rev932[0x10000];
static void build_rev932(void)
{
    static int done;
    if (done) return;
    done = 1;
    for (unsigned lead = 0x81; lead <= 0xFC; lead++)
        for (unsigned trail = 0x40; trail <= 0xFC; trail++) {
            uint8_t src[2] = {(uint8_t)lead, (uint8_t)trail};
            char out[8];
            const unsigned char *u = (const unsigned char *)out;
            if ((lead >= 0xA0 && lead < 0xE0) || trail == 0x7F) continue;
            if (vjo_hook_decode(VJO_HOOK_ENC_CP932, src, 2, out, sizeof out, NULL) == 3) {
                unsigned cp = (u[0] & 0x0Fu) << 12 | (u[1] & 0x3Fu) << 6 | (u[2] & 0x3Fu);
                if (!rev932[cp]) rev932[cp] = (uint16_t)(lead << 8 | trail);
            } else if (vjo_hook_decode(VJO_HOOK_ENC_CP932, src, 2, out, sizeof out, NULL) == 2) {
                unsigned cp = (u[0] & 0x1Fu) << 6 | (u[1] & 0x3Fu);
                if (!rev932[cp]) rev932[cp] = (uint16_t)(lead << 8 | trail);
            }
        }
}

static uint32_t next_cp(const char *s, size_t *i)
{
    const unsigned char *u = (const unsigned char *)s + *i;
    if (u[0] < 0x80) return *i += 1, u[0];
    if (u[0] < 0xE0) return *i += 2, (uint32_t)(u[0] & 0x1F) << 6 | (u[1] & 0x3F);
    if (u[0] < 0xF0) return *i += 3, (uint32_t)(u[0] & 0x0F) << 12 | (uint32_t)(u[1] & 0x3F) << 6 | (u[2] & 0x3F);
    *i += 4;
    return (uint32_t)(u[0] & 0x07) << 18 | (uint32_t)(u[1] & 0x3F) << 12 | (uint32_t)(u[2] & 0x3F) << 6 | (u[3] & 0x3F);
}

/* utf8 in the site's encoding (what the game would hold) into raw; returns the bytes */
static unsigned encode(unsigned enc, const char *utf8, uint8_t *raw, unsigned cap)
{
    size_t i = 0, len = strlen(utf8);
    unsigned w = 0;
    build_rev932();
    while (i < len && w + 4 <= cap) {
        uint32_t cp = next_cp(utf8, &i);
        if (enc == VJO_HOOK_ENC_UTF8) {
            size_t start = i;
            (void)start;
        }
        if (enc == VJO_HOOK_ENC_UTF16LE) {
            if (cp >= 0x10000) {
                uint32_t v = cp - 0x10000, hi = 0xD800 + (v >> 10), lo = 0xDC00 + (v & 0x3FF);
                raw[w++] = (uint8_t)hi, raw[w++] = (uint8_t)(hi >> 8), raw[w++] = (uint8_t)lo, raw[w++] = (uint8_t)(lo >> 8);
            } else {
                raw[w++] = (uint8_t)cp, raw[w++] = (uint8_t)(cp >> 8);
            }
        } else if (enc == VJO_HOOK_ENC_CP932) {
            if (cp < 0x80) raw[w++] = (uint8_t)cp;
            else if (cp >= 0xFF61 && cp <= 0xFF9F) raw[w++] = (uint8_t)(0xA1 + cp - 0xFF61);
            else if (cp < 0x10000 && rev932[cp]) raw[w++] = (uint8_t)(rev932[cp] >> 8), raw[w++] = (uint8_t)rev932[cp];
            else if (cp == VJO_HOOK_CP932_A0) raw[w++] = 0xA0; /* the decoder's single byte 0xA0 */
            else TEST_CHECK_(0, "U+%04X is not in CP932", (unsigned)cp);
        }
    }
    if (enc == VJO_HOOK_ENC_UTF8) {
        memcpy(raw, utf8, len < cap ? len : cap);
        w = (unsigned)(len < cap ? len : cap);
    }
    return w;
}

/* the text of a hit of s holding utf8 (in the site's encoding) */
static void filters_to(const char *id, uint32_t addr, const char *in, const char *want)
{
    uint8_t raw[1024];
    char out[1024];
    const VjoLunaSite *s = site(id, addr);
    unsigned n;
    size_t got;
    if (!s) return;
    TEST_CHECK_(vjo_luna_ported(s), "%s 0x%08X ported", id, (unsigned)addr);
    n = encode(s->encoding, in, raw, sizeof raw);
    got = vjo_luna_text(s, raw, n, s->encoding, out, sizeof out);
    TEST_CHECK_(got == strlen(want) && !strcmp(out, want), "%s 0x%08X: [%s]", id, (unsigned)addr, in);
    TEST_MSG("want [%s]", want);
    TEST_MSG("got  [%s]", out);
}

/* ---- the fake memory ---- */

typedef struct {
    uint32_t base;
    uint8_t *data;
    unsigned size;
} Blk;

typedef struct {
    Blk b[4];
    unsigned nb, calls, refuse;
} Mem;

/* VjoHookRead over the blocks: unit 1/2 stops after the first NUL unit (copied, counted) and
 * at the block's end; unit 0 copies exactly max or nothing. */
static unsigned rd(void *ud, uint32_t addr, void *dst, unsigned max, unsigned unit)
{
    Mem *m = ud;
    m->calls++;
    if (m->refuse) return 0;
    for (unsigned i = 0; i < m->nb; i++) {
        const Blk *b = &m->b[i];
        if (addr >= b->base && addr - b->base < b->size) {
            unsigned avail = b->size - (addr - b->base), n = 0;
            const uint8_t *src = b->data + (addr - b->base);
            if (!unit) {
                if (max > avail) return 0;
                memcpy(dst, src, max);
                return max;
            }
            if (max > avail) max = avail;
            while (n + unit <= max) {
                int nul = unit == 1 ? !src[n] : !(src[n] | src[n + 1]);
                memcpy((uint8_t *)dst + n, src + n, unit);
                n += unit;
                if (nul) break;
            }
            return n;
        }
    }
    return 0;
}

static uint8_t ram[0x2000];
#define RAM 0x81000000u

static Mem mem(void)
{
    Mem m;
    memset(&m, 0, sizeof m);
    m.b[0].base = RAM;
    m.b[0].data = ram;
    m.b[0].size = sizeof ram;
    m.nb = 1;
    return m;
}

static void put_le32(uint32_t at, uint32_t v)
{
    ram[at - RAM] = (uint8_t)v;
    ram[at - RAM + 1] = (uint8_t)(v >> 8);
    ram[at - RAM + 2] = (uint8_t)(v >> 16);
    ram[at - RAM + 3] = (uint8_t)(v >> 24);
}

/* the bytes at addr, then (unless n is given) a NUL */
static void put_bytes(uint32_t at, const void *p, unsigned n) { memcpy(ram + (at - RAM), p, n); }

static unsigned put_text(uint32_t at, unsigned enc, const char *utf8)
{
    uint8_t raw[512];
    unsigned n = encode(enc, utf8, raw, sizeof raw);
    put_bytes(at, raw, n);
    ram[at - RAM + n] = 0;
    if (enc == VJO_HOOK_ENC_UTF16LE) ram[at - RAM + n + 1] = 0;
    return n;
}

/* captures a hit of s with regs and decodes it through vjo_luna_text */
static size_t hit(const VjoLunaSite *s, const uint32_t *regs, Mem *m, char *out, size_t cap)
{
    uint8_t raw[512];
    unsigned enc = 99, n;
    if (!s) return 0;
    n = vjo_luna_capture(s, regs, rd, m, raw, sizeof raw, &enc);
    TEST_CHECK(enc == s->encoding);
    return vjo_luna_text(s, raw, n, enc, out, cap);
}

/* ---- the table ---- */

static void table_counts(void)
{
    unsigned total = 0, ported = 0, known = 0, titles_ported = 0;
    TEST_CHECK(VJO_LUNA_TITLES == 182u && VJO_LUNA_SITES == 306u);
    for (unsigned t = 0; t < VJO_LUNA_TITLES; t++) {
        const VjoLunaSite *s = NULL;
        const char *id = vjo_luna_title(t);
        unsigned n = vjo_luna_sites(id, &s);
        TEST_CHECK_(id && strlen(id) == 9 && n && s, "title %u", t);
        if (!s) continue;
        /* the slice is exactly the title's sites, as the test's own copy of the table has them */
        for (unsigned i = 0; i < n; i++) {
            TEST_CHECK_(s[i].title == t, "%s site %u", id, i);
            ported += s[i].ported;
        }
        {
            unsigned first = 0;
            while (first < VJO_LUNA_SITES && vjo_luna_table[first].title != t) first++;
            TEST_CHECK_(first + n <= VJO_LUNA_SITES && !memcmp(s, &vjo_luna_table[first], n * sizeof *s), "%s", id);
        }
        TEST_CHECK_(!strcmp(id, vjo_luna_ids[t]), "%s", id);
        if (t) TEST_CHECK_(strcmp(vjo_luna_title(t - 1), id) < 0, "%s sorted", id);
        known += vjo_luna_title_known(id);
        {
            /* a title with a ported site (audit-hooks-text N2) */
            unsigned any = 0;
            for (unsigned i = 0; i < n; i++) any |= (unsigned)vjo_luna_ported(&s[i]);
            TEST_CHECK_(vjo_luna_title_ported(id) == (int)any, "%s: has a ported site %u", id, any);
            titles_ported += any;
        }
        total += n;
    }
    TEST_CHECK_(total == VJO_LUNA_SITES, "%u sites", total);
    TEST_CHECK_(ported == 294, "%u ported", ported);
    TEST_CHECK(known == VJO_LUNA_TITLES);
    /* the 7 titles whose every site is unported (docs/guide.md, docs/native-hooks.md): no tag, OCR only in auto */
    TEST_CHECK_(titles_ported == VJO_LUNA_TITLES - 7u, "%u of %u titles have a ported site", titles_ported,
                (unsigned)VJO_LUNA_TITLES);
    {
        static const char *const none[] = {"PCSB00985", "PCSG00291", "PCSG00451", "PCSG00595",
                                           "PCSG00663", "PCSG01081", "PCSG01152"};
        for (unsigned i = 0; i < 7; i++)
            TEST_CHECK_(vjo_luna_title_known(none[i]) && !vjo_luna_title_ported(none[i]), "%s", none[i]);
    }
    TEST_CHECK(vjo_luna_title_ported("PCSG00543") && vjo_luna_title_ported("PCSG00917"));
    TEST_CHECK(!vjo_luna_title_ported("PCSG00415") && !vjo_luna_title_ported("") && !vjo_luna_title_ported(NULL));
    TEST_CHECK(!vjo_luna_title(VJO_LUNA_TITLES) && !strcmp(vjo_luna_title(0), "PCSB00985"));
    /* every ported site has every part this build ports; every refused one is refused */
    for (unsigned i = 0; i < VJO_LUNA_SITES; i++)
        TEST_CHECK_(vjo_luna_ported(&vjo_luna_table[i]) == vjo_luna_table[i].ported, "site %u (%s 0x%08X)", i,
                    vjo_luna_ids[vjo_luna_table[i].title], (unsigned)vjo_luna_table[i].addr);
}

static void lookups(void)
{
    const VjoLunaSite *s = (const VjoLunaSite *)1;
    TEST_CHECK(vjo_luna_sites("PCSG00543", &s) == 1 && s && s->addr == 0x80014426u && s->reg == 0xB &&
               s->encoding == VJO_HOOK_ENC_UTF16LE && (s->flags & VJO_LUNA_F_FULL_STRING) && s->ported);
    /* Luna lists two games under PCSG00917 (Starry Sky Autumn, Wand of Fortune R): 6 + 4 sites */
    TEST_CHECK(vjo_luna_sites("PCSG00917", &s) == 10);
    TEST_CHECK(vjo_luna_sites("PCSG00574", &s) == 1 && s->addr == 0x80017628u); /* a multi-ID entry */
    TEST_CHECK(vjo_luna_sites("PCSG00618", &s) == 2);
    /* CLANNAD is Rubi's own entry, not Luna's */
    TEST_CHECK(!vjo_luna_title_known("PCSG00415") && vjo_luna_sites("PCSG00415", &s) == 0 && s == NULL);
    TEST_CHECK(!vjo_luna_title_known("PCSG0054") && !vjo_luna_title_known("pcsg00543"));
    TEST_CHECK(!vjo_luna_title_known("") && !vjo_luna_title_known(NULL) && vjo_luna_sites(NULL, NULL) == 0);
    TEST_CHECK(vjo_luna_title_known("PCSG00543\0junk") && vjo_luna_title_known("PCSB00985"));
    TEST_CHECK(vjo_luna_sites("PCSG00543", NULL) == 1);
    /* the address space: segment 0 at VJO_LUNA_SEG0, the lowest site 0x8000854A */
    for (unsigned i = 0; i < VJO_LUNA_SITES; i++) TEST_CHECK(vjo_luna_table[i].addr >= VJO_LUNA_SEG0 + 0x4000u);
    /* refused: USING_CHAR, a hook-spawning hookfunc, the shadowed same-address entry, alternation filters */
    TEST_CHECK(!site("PCSG00663", 0x80BB4F70u)->ported && !site("PCSG01011", 0x82535242u)->ported);
    TEST_CHECK(!site("PCSG00595", 0x8009730Eu)->ported && !site("PCSG00611", 0x8003F554u)->ported);
    TEST_CHECK(site("PCSG01221", 0x8003F554u)->ported && site("PCSG00611", 0x800B9DEAu)->ported);
    TEST_CHECK(!site("PCSG00451", 0x810789EEu)->ported && !site("PCSG01152", 0x82303AB0u)->ported);
    TEST_CHECK(!site("PCSG00291", 0x80070658u)->ported && !site("PCSB00985", 0x8004D360u)->ported);
    /* the duplicate (address, title) left out: PCSG00917's 0x800345f0 keeps the first entry's (CP932) */
    TEST_CHECK(site("PCSG00917", 0x800345F0u)->encoding == VJO_HOOK_ENC_CP932);
    TEST_CHECK(site("PCSG00917", 0x8002CC56u)->padding == 8);
}

static void version_gate(void)
{
    const VjoLunaSite *all = NULL, *v100, *v101, *none;
    unsigned n = vjo_luna_sites("PCSG00708", &all);
    TEST_CHECK(n == 2);
    none = site("PCSG00708", 0x81558104u);
    v101 = site("PCSG00708", 0x81558264u);
    TEST_CHECK(none->version == 0 && v101->version == 0x0101);
    TEST_CHECK(vjo_luna_version_ok(all, n, v101, 0x0101) && !vjo_luna_version_ok(all, n, none, 0x0101));
    TEST_CHECK(!vjo_luna_version_ok(all, n, v101, 0x0100) && vjo_luna_version_ok(all, n, none, 0x0100));
    TEST_CHECK(!vjo_luna_version_ok(all, n, v101, 0) && vjo_luna_version_ok(all, n, none, 0));
    /* a version comment holds for the block's later entries: Starry Sky Autumn 1.00 x3, 1.01 x3 */
    n = vjo_luna_sites("PCSG00917", &all);
    v100 = site("PCSG00917", 0x800345F0u);
    v101 = site("PCSG00917", 0x8002CC72u);
    TEST_CHECK(v100->version == 0x0100 && v101->version == 0x0101);
    TEST_CHECK(site("PCSG00917", 0x80081378u)->version == 0x0101); /* Wand of Fortune R's block */
    TEST_CHECK(vjo_luna_version_ok(all, n, v100, 0x0100) && !vjo_luna_version_ok(all, n, v100, 0x0101));
    TEST_CHECK(!vjo_luna_version_ok(all, n, v100, 0x0102) && !vjo_luna_version_ok(all, n, v100, 0));
    /* three versions, one unnamed (Rear pheles: 1.00, 1.02 and the other's) */
    n = vjo_luna_sites("PCSG00663", &all);
    none = site("PCSG00663", 0x80BB4F70u);
    TEST_CHECK(n == 3 && vjo_luna_version_ok(all, n, none, 0x0101) && !vjo_luna_version_ok(all, n, none, 0x0102));
    TEST_CHECK(vjo_luna_version_ok(all, n, site("PCSG00663", 0x80848520u), 0x0102));
    /* a title without versions: any version, and an unread one */
    n = vjo_luna_sites("PCSG00543", &all);
    TEST_CHECK(vjo_luna_version_ok(all, n, all, 0x0100) && vjo_luna_version_ok(all, n, all, 0));
    TEST_CHECK(!vjo_luna_version_ok(all, n, NULL, 0x0100));
    /* the sites that name a version: only 1.00, 1.01 and 1.02 */
    for (unsigned i = 0; i < VJO_LUNA_SITES; i++) {
        unsigned v = vjo_luna_table[i].version;
        TEST_CHECK(v == 0 || v == 0x0100 || v == 0x0101 || v == 0x0102);
    }
}

/* ---- hook time ---- */

static void capture_default(void)
{
    Mem m = mem();
    uint32_t regs[16] = {0};
    char out[256];
    const VjoLunaSite *s;
    VjoLunaSite x;
    uint8_t raw[64];
    unsigned enc;
    memset(ram, 0xEE, sizeof ram);
    /* PCSG00543: UTF-16 at r11 */
    put_text(RAM + 0x100, VJO_HOOK_ENC_UTF16LE, "冴えない彼女");
    regs[11] = RAM + 0x100;
    TEST_CHECK(hit(site("PCSG00543", 0x80014426u), regs, &m, out, sizeof out) && !strcmp(out, "冴えない彼女"));
    /* padding: PCSG01001 reads r0 + 8 (UTF-8) */
    put_text(RAM + 0x208, VJO_HOOK_ENC_UTF8, "サイド@@キックス");
    regs[0] = RAM + 0x200;
    TEST_CHECK(hit(site("PCSG01001", 0x80020C78u), regs, &m, out, sizeof out) && !strcmp(out, "サイドキックス"));
    /* CP932 at r5 (PCSG00789, no filter) */
    put_text(RAM + 0x300, VJO_HOOK_ENC_CP932, "オメルタ");
    regs[5] = RAM + 0x300;
    s = site("PCSG00789", 0x800BB456u);
    TEST_CHECK(hit(s, regs, &m, out, sizeof out) == 12 && !strcmp(out, "オメルタ"));
    /* DATA_INDIRECT: the register points at the text's pointer */
    x = *s;
    x.flags = VJO_LUNA_F_DATA_INDIRECT;
    x.padding = 2;
    put_le32(RAM + 0x400, RAM + 0x300 - 2);
    regs[5] = RAM + 0x400;
    TEST_CHECK(hit(&x, regs, &m, out, sizeof out) && !strcmp(out, "オメルタ"));
    /* a pointer the reader cannot read, or that points nowhere */
    regs[5] = 0x10;
    TEST_CHECK(vjo_luna_capture(&x, regs, rd, &m, raw, sizeof raw, &enc) == 0);
    put_le32(RAM + 0x400, 0x20);
    regs[5] = RAM + 0x400;
    TEST_CHECK(vjo_luna_capture(&x, regs, rd, &m, raw, sizeof raw, &enc) == 0);
    /* a reader that refuses everything */
    regs[5] = RAM + 0x300;
    m.refuse = 1;
    TEST_CHECK(vjo_luna_capture(s, regs, rd, &m, raw, sizeof raw, &enc) == 0);
    m.refuse = 0;
    /* the cap: the text cut at cap bytes (and the decoder drops the half character) */
    TEST_CHECK(vjo_luna_capture(s, regs, rd, &m, raw, 7, &enc) == 7);
    TEST_CHECK(vjo_luna_text(s, raw, 7, enc, out, sizeof out) == 9 && !strcmp(out, "オメル"));
    TEST_CHECK(vjo_luna_capture(s, regs, rd, &m, raw, 3, &enc) == 0); /* below the minimum */
    /* a string that runs into the end of memory: what is there */
    put_bytes(RAM + sizeof ram - 4, "\x83\x41\x83\x43", 4);
    regs[5] = RAM + sizeof ram - 4;
    TEST_CHECK(vjo_luna_capture(s, regs, rd, &m, raw, sizeof raw, &enc) == 4);
    /* an empty string: no text */
    ram[0x500] = 0;
    regs[5] = RAM + 0x500;
    TEST_CHECK(vjo_luna_capture(s, regs, rd, &m, raw, sizeof raw, &enc) == 0);
    /* UTF-16 and an odd cap: whole units */
    regs[11] = RAM + 0x100;
    TEST_CHECK(vjo_luna_capture(site("PCSG00543", 0x80014426u), regs, rd, &m, raw, 9, &enc) == 8);
    /* USING_CHAR (refused in the table: Luna joins characters across hits): one character */
    s = site("PCSG00663", 0x80BB4F70u);
    regs[1] = RAM + 0x100;
    TEST_CHECK(vjo_luna_capture(s, regs, rd, &m, raw, sizeof raw, &enc) == 0 && !vjo_luna_ported(s));
    x = *s;
    x.ported = 1;
    TEST_CHECK(vjo_luna_capture(&x, regs, rd, &m, raw, sizeof raw, &enc) == 2 && raw[0] == 0xB4 && raw[1] == 0x51); /* 冴 U+51B4 */
    x.flags = VJO_LUNA_F_USING_CHAR;
    x.encoding = VJO_HOOK_ENC_CP932;
    regs[1] = 0x41A082u; /* the register itself: lead 0x82, trail 0xA0 */
    TEST_CHECK(vjo_luna_capture(&x, regs, rd, &m, raw, sizeof raw, &enc) == 2 && raw[0] == 0x82 && raw[1] == 0xA0);
    regs[1] = 0x41;
    TEST_CHECK(vjo_luna_capture(&x, regs, rd, &m, raw, sizeof raw, &enc) == 1 && raw[0] == 0x41);
    regs[1] = 0;
    TEST_CHECK(vjo_luna_capture(&x, regs, rd, &m, raw, sizeof raw, &enc) == 0);
    /* nothing without a site, registers, a reader or a buffer */
    TEST_CHECK(vjo_luna_capture(NULL, regs, rd, &m, raw, sizeof raw, &enc) == 0 && enc == VJO_HOOK_ENC_AUTO);
    TEST_CHECK(vjo_luna_capture(&x, NULL, rd, &m, raw, sizeof raw, NULL) == 0);
    TEST_CHECK(vjo_luna_capture(&x, regs, NULL, &m, raw, sizeof raw, NULL) == 0);
    TEST_CHECK(vjo_luna_capture(&x, regs, rd, &m, NULL, sizeof raw, NULL) == 0);
}

static void capture_hookfuncs(void)
{
    Mem m = mem();
    uint32_t regs[16] = {0};
    char out[256];
    unsigned n;
    memset(ram, 0xEE, sizeof ram);
    /* PCSG01124 (下天の華): 0xAA strings and escapes */
    {
        uint8_t img[64];
        unsigned w = 0;
        img[w++] = 0xAA;
        w += encode(VJO_HOOK_ENC_CP932, "テキスト", img + w, 32);
        img[w++] = 0;
        img[w++] = 0xAA;
        memcpy(img + w, "\x1b\xb4\x00\x01\x00\x02\x00", 7); /* an escape with NULs in it */
        w += 7;
        img[w++] = 0xAA;
        memcpy(img + w, "\x1b\xb5\x00", 3);
        w += 3;
        img[w++] = 0xAA;
        w += encode(VJO_HOOK_ENC_CP932, "続き\n　", img + w, 32);
        img[w++] = 0;
        img[w++] = 0x00; /* not 0xAA: the end */
        put_bytes(RAM + 0x100, img, w);
        regs[0] = RAM + 0x100;
        TEST_CHECK(hit(site("PCSG00921", 0x80129BE0u), regs, &m, out, sizeof out) && !strcmp(out, "テキスト続き"));
        TEST_MSG("got [%s]", out);
        ram[0x100] = 0x41; /* no 0xAA first: nothing */
        TEST_CHECK(hit(site("PCSG00921", 0x80129BE0u), regs, &m, out, sizeof out) == 0);
    }
    /* ReadU16TextAndLenDW (神凪ノ杜): the units at +0xC, their count at +8; NUL units removed */
    put_le32(RAM + 0x208, 5);
    n = put_text(RAM + 0x20C, VJO_HOOK_ENC_UTF16LE, "神凪");
    ram[0x20C + n] = 0, ram[0x20C + n + 1] = 0; /* a NUL unit inside the count */
    put_text(RAM + 0x20C + n + 2, VJO_HOOK_ENC_UTF16LE, "ノ杜あ"); /* the count stops before あ */
    regs[0] = RAM + 0x200;
    TEST_CHECK(hit(site("PCSG01268", 0x828BB50Cu), regs, &m, out, sizeof out) && !strcmp(out, "神凪ノ杜"));
    TEST_MSG("got [%s]", out);
    put_le32(RAM + 0x208, 0x7FFFFFFFu); /* a count past the cap: the cap */
    TEST_CHECK(hit(site("PCSG01268", 0x828BB50Cu), regs, &m, out, sizeof out) > 0);
    /* PCSG01046 (嘘月シャングリラ): the same from r0, <br> removed */
    put_le32(RAM + 0x308, 7);
    put_text(RAM + 0x30C, VJO_HOOK_ENC_UTF16LE, "嘘<br>月");
    regs[0] = RAM + 0x300;
    regs[4] = 0;
    TEST_CHECK(hit(site("PCSG01046", 0x81E1F5C8u), regs, &m, out, sizeof out) && !strcmp(out, "嘘月"));
    /* TPCSG00903 (New Game!): UTF-8 at r0 + 0x1C, its length at r0 + 0x14; \n a space */
    n = put_text(RAM + 0x41C, VJO_HOOK_ENC_UTF8, "がんばるぞい\\n次");
    put_le32(RAM + 0x414, n - 3);
    regs[0] = RAM + 0x400;
    TEST_CHECK(hit(site("PCSG00903", 0x8012674Cu), regs, &m, out, sizeof out) && !strcmp(out, "がんばるぞい "));
    TEST_MSG("got [%s]", out);
    /* PCSG01235 (ノラと皇女と野良猫ハート2): the string 3 bytes before r11's pointer */
    put_text(RAM + 0x500, VJO_HOOK_ENC_UTF8, "ノラ");
    regs[11] = RAM + 0x503;
    TEST_CHECK(hit(site("PCSG01235", 0x80027620u), regs, &m, out, sizeof out) && !strcmp(out, "ノラ"));
    /* PCSG00992 (遙かなる時空の中で3): the strings at r1 up to an empty one, newlines removed */
    {
        unsigned at = 0x600;
        at += put_text(RAM + at, VJO_HOOK_ENC_CP932, "一行目\n") + 1;
        at += put_text(RAM + at, VJO_HOOK_ENC_CP932, "二行目") + 1;
        ram[at] = 0;
        put_text(RAM + at + 1, VJO_HOOK_ENC_CP932, "後");
        regs[1] = RAM + 0x600;
        TEST_CHECK(hit(site("PCSG00992", 0x800A927Eu), regs, &m, out, sizeof out) && !strcmp(out, "一行目二行目"));
    }
    /* PCSG00912 (Un:BIRTHDAY SONG): 47 FF FF in r1's 0x50 bytes, strings chained by 48 FF FF 47 FF FF */
    {
        uint8_t img[96];
        unsigned w = 0;
        memset(img, 0x11, sizeof img);
        w = 6;
        memcpy(img + w, "\x47\xff\xff\x01\x02", 5);
        w += 5;
        w += encode(VJO_HOOK_ENC_CP932, "愛を", img + w, 32);
        img[w++] = 0;
        memcpy(img + w, "\x48\xff\xff\x47\xff\xff\x03\x04", 8);
        w += 8;
        w += encode(VJO_HOOK_ENC_CP932, "唄う", img + w, 32);
        img[w++] = 0;
        img[w++] = 0x10; /* not 48 FF FF: the end */
        put_bytes(RAM + 0x700, img, w);
        regs[1] = RAM + 0x700;
        TEST_CHECK(hit(site("PCSG00912", 0x80038538u), regs, &m, out, sizeof out) && !strcmp(out, "愛を唄う"));
        TEST_MSG("got [%s]", out);
        regs[1] = RAM + 0x706; /* the pattern at offset 0: Luna's SearchPattern says none */
        TEST_CHECK(hit(site("PCSG00912", 0x80038538u), regs, &m, out, sizeof out) == 0);
        memset(ram + 0x700, 0x11, 0x60); /* no pattern */
        regs[1] = RAM + 0x700;
        TEST_CHECK(hit(site("PCSG00912", 0x80038538u), regs, &m, out, sizeof out) == 0);
    }
    /* PCSG01011 (空蝉の廻): r7 inside a string: from its start; \n removed, cut after × */
    ram[0x7FF] = 0;
    n = put_text(RAM + 0x800, VJO_HOOK_ENC_UTF8, "名前×台詞\n続き");
    regs[7] = RAM + 0x800 + 9;
    TEST_CHECK(hit(site("PCSG01011", 0x801AE35Au), regs, &m, out, sizeof out) && !strcmp(out, "台詞続き"));
    TEST_MSG("got [%s]", out);
    memset(ram + 0x900, 'a', 0x400); /* no NUL within the cap before r7: none */
    regs[7] = RAM + 0xB00;
    TEST_CHECK(hit(site("PCSG01011", 0x801AE35Au), regs, &m, out, sizeof out) == 0);
    regs[7] = RAM + 1; /* the start of memory before it: as far back as it goes, then none */
    memset(ram, 'b', 16);
    TEST_CHECK(hit(site("PCSG01011", 0x801AE35Au), regs, &m, out, sizeof out) == 0);
    /* PCSG00914 (うたプリ Repeat LOVE): strings at r1 + 0x20, each one's final \n dropped; ㌫㍊ the name */
    {
        unsigned at = 0xD20;
        at += put_text(RAM + at, VJO_HOOK_ENC_CP932, "㌫㍊\n") + 1;
        at += put_text(RAM + at, VJO_HOOK_ENC_CP932, "さん㌻") + 1;
        ram[at] = 0;
        regs[1] = RAM + 0xD00;
        TEST_CHECK(hit(site("PCSG00914", 0x8005A534u), regs, &m, out, sizeof out) && !strcmp(out, "七海春歌さん"));
        TEST_MSG("got [%s]", out);
    }
    /* PCSG00769 (逢魔が刻): back over the strings around r3, then all of them */
    {
        unsigned at = 0xE00;
        ram[at++] = 0;
        ram[at++] = 0;
        at += put_text(RAM + at, VJO_HOOK_ENC_CP932, "㊤始まり　") + 1;
        at += put_text(RAM + at, VJO_HOOK_ENC_CP932, "中\n") + 1;
        n = at;
        at += put_text(RAM + at, VJO_HOOK_ENC_CP932, "終わり㊦") + 1;
        ram[at] = 0;
        regs[3] = RAM + n + 2;
        TEST_CHECK(hit(site("PCSG00769", 0x8003CA08u), regs, &m, out, sizeof out) && !strcmp(out, "―始まり中終わり―"));
        TEST_MSG("got [%s]", out);
        /* a one-byte string before: Luna would loop for ever; the walk stops */
        memset(ram + 0xF00, 0, 0x40);
        ram[0xF10] = 'x';
        put_text(RAM + 0xF12, VJO_HOOK_ENC_CP932, "後");
        regs[3] = RAM + 0xF13;
        TEST_CHECK(hit(site("PCSG00769", 0x8003CA08u), regs, &m, out, sizeof out) == 0 || strlen(out) < 64);
    }
    /* not ported: PCSG00595's hookfunc spawns a hook */
    {
        uint8_t raw[64];
        unsigned enc;
        regs[0] = RAM + 0x100;
        TEST_CHECK(vjo_luna_capture(site("PCSG00595", 0x8009730Eu), regs, rd, &m, raw, sizeof raw, &enc) == 0);
        TEST_CHECK(vjo_luna_text(site("PCSG00595", 0x8009730Eu), (const uint8_t *)"a", 1, enc, out, sizeof out) == 0);
    }
}

/* The reader is called at most VJO_LUNA_READS times for the strings of one hit (plus the
 * hookfunc's few pointer and length reads), whatever memory holds. */
static void capture_bounded(void)
{
    Mem m = mem();
    uint32_t regs[16] = {0};
    uint8_t raw[512];
    unsigned enc, n;
    for (unsigned i = 0; i < 0x400; i += 2) ram[i] = 'a', ram[i + 1] = 0; /* "a" "a" "a"... */
    regs[1] = RAM;
    m.calls = 0;
    n = vjo_luna_capture(site("PCSG00992", 0x800A927Eu), regs, rd, &m, raw, sizeof raw, &enc);
    TEST_CHECK_(m.calls <= VJO_LUNA_READS && n == VJO_LUNA_READS, "%u calls, %u bytes", m.calls, n);
    regs[3] = RAM + 0x101;
    m.calls = 0;
    vjo_luna_capture(site("PCSG00769", 0x8003CA08u), regs, rd, &m, raw, sizeof raw, &enc);
    TEST_CHECK_(m.calls <= 2 * VJO_LUNA_READS + 10, "%u calls", m.calls);
    regs[7] = RAM + 0x101;
    m.calls = 0;
    vjo_luna_capture(site("PCSG01011", 0x801AE35Au), regs, rd, &m, raw, sizeof raw, &enc);
    TEST_CHECK_(m.calls <= 12, "%u calls", m.calls);
}

/* ---- job time: every ported filter ---- */

static void filters_cp932(void)
{
    filters_to("PCSG01157", 0x800608F0u, "一行目\n二行目", "一行目二行目");          /* NewLineCharFilterA */
    filters_to("PCSG00401", 0x8000C1C8u, "【　名前　】「台詞」", "【名前】「台詞」"); /* PCSG00401 */
    filters_to("PCSG00766", 0x80073862u, "#Ruby[漢字,かんじ]を読む#n　次の行#Speed[10]", "漢字を読む次の行");
    filters_to("PCSG00766", 0x80073862u, "　", "　"); /* Luna compares with the UTF-8 form: no match here */
    filters_to("PCSG00664", 0x80035C34u, "あ　[n]　い", "あい");
    filters_to("PCSG00477", 0x8003A49Au, "一#n　二#Wait10]三", "一二三");
    filters_to("PCSG00826", 0x800579EEu, "一#n　二", "一二");
    filters_to("PCSG00062", 0x8119ADDAu, "[本/ほん]を<b>読む</b>\\n次", "本を読む\n次");
    filters_to("PCSG00509", 0x80016C76u, "あ^い^", "あい"); /* FPCSG00852 */
    filters_to("PCSG00402", 0x800879DEu, "　一#n　二#Cmd[x]三#End", "一二三");
    filters_to("PCSG01267", 0x800482CEu, " 一 #n 二 #Wait[10]三", "一二三"); /* FPCSG00815 */
    filters_to("PCSG01023", 0x8002E176u, "%Nあ　　い%P%K<br>%CF11F終\n", "あ　　い終");
    filters_to("PCSG01023", 0x8002E176u, "%N　　台詞", "台詞");
    filters_to("PCSG01282", 0x8000F00Eu, "@abc一行\n\n二行#注#3", "一行 二行");
    filters_to("PCSG00448", 0x800718F8u, " 一 #n#n二#Speed[1.5]三#Pos[1,2]", "一二三");
    filters_to("PCSG00751", 0x80070E30u, "一 @a二＄三", "一二三");
    filters_to("PCSG00696", 0x8008BD1Au, "㌔あ㍉", "あ");
    filters_to("PCSG00410", 0x8004E44Au, "#Speed[1.5]#Pos[1,2]台詞④#n②⑥⑪名⑫⑤①", "台詞!? !!。【名】、・・・");
    filters_to("PCSG00405", 0x8001BFF2u, "一 二\t三", "一二三");
    filters_to("PCSG00776", 0x80126E7Eu, " \x02台詞　 ", "台詞");
    filters_to("PCSG00510", 0x8003542Eu, "%N一%N二", "一二"); /* FPCSG00912 */
    filters_to("PCSG00401", 0x8000BAD4u, " 一\\c二\\n三", "一二三"); /* FPCSG00401: its \c literal */
    filters_to("PCSG00530", 0x8002CF8Eu, "一#n二", "一二");
    filters_to("PCSG01204", 0x80063E54u, "一\n@1r二@-1r", "一二");
    filters_to("PCSG01167", 0x80029AB2u, "<よみ>ポスト|後", "ポスト後"); /* ポ is 83 7C: Luna's | would cut it */
    filters_to("PCSG00472", 0x8007300Eu, "えゞ　#n#n#Cmd[1,2]ゝ", "え!?!!");
    filters_to("PCSG00272", 0x8103416Cu, "名前#n#台詞", "【名前】#台詞");
    filters_to("PCSG00272", 0x8103416Cu, "#n#台詞", "#台詞");
    filters_to("PCSG00172", 0x810A2486u, "app0:d:d台詞u_@", "台詞");
    filters_to("PCSG00789", 0x800BC462u, "#Name一　二", "一　二"); /* F010088B01A8FC000 on CP932 */
    filters_to("PCSG01250", 0x80013ED6u, "<text 本文#n/ruby:漢&かん/字>", "本文漢字");
    filters_to("PCSG01250", 0x80013ED6u, "台詞", "");
    filters_to("PCSG01250", 0x80013ED6u, "<text", ""); /* shorter than the cut: substr throws in Luna */
    /* PCSG01250_N (ex = 1): Luna's s.substr(7, s.size() - 9) keeps [7, size - 2) of the CP932 bytes
       (audit-hooks-text F1: the port cut one byte more and lost the name's last character) */
    filters_to("PCSG01250", 0x80013EE6u, "<name=\"名前\">", "名前");
    filters_to("PCSG01250", 0x80013EE6u, "<name=\"ナナ\">", "ナナ");
    filters_to("PCSG01250", 0x80013EE6u, "<name x名前yz>", "名前y");
    filters_to("PCSG01250", 0x80013EE6u, "台詞", ""); /* no <name: cleared */
    filters_to("PCSG01318", 0x800154CCu, "<name 名前>", "名前");
    filters_to("PCSG01202", 0x80022C9Cu, "/ruby:漢&かん/字", "漢字");
    filters_to("PCSG00431", 0x80019DFAu, "一#n　二#n三", "一二三");
    filters_to("PCSG00431", 0x800338AAu, "一#n　二", "一　二"); /* FPCSG00855 on CP932: #n only */
    filters_to("PCSG01178", 0x80043D1Au, "一#n二  三", "一二三");
    filters_to("PCSG00535", 0x80060E58u, "一\\f二\\n三", "一二\n三");
    filters_to("PCSG00535", 0x80083A94u, "同じ", "同じ");
    filters_to("PCSG01249", 0x80030CAAu, "一[n]二", "一二");
    filters_to("PCSG00370", 0x80053BD0u, "一\n　二▼＞\n", "一二");
    filters_to("PCSG01289", 0x800647D0u, "ｱｲｳ?$t漢字@と$[字$/じ$]", "あいう？漢字と字");
    filters_to("PCSG01198", 0x800785C8u, "`漢字@ｶﾞ", "漢字か！");
    filters_to("PCSG00585", 0x80036616u, "$[漢$/かん$]$C[ffffff]白@$C[]\n", "漢白");
    filters_to("PCSG01254", 0x8007D250u, "ｺﾝﾆﾁﾊ@wait_1.5", "こんにちは");
    filters_to("PCSG01196", 0x8003D5B6u, "<a>一^二<b>", "一\n二");
    filters_to("PCSG00780", 0x8006DF02u, "^<漢,かん>字", "漢字");
    filters_to("PCSG01151", 0x8001B7AAu, "^一　二", "一二");
    filters_to("PCSG00991", 0x8007AA94u, "#r漢|かん#字@r字@1\\c\\n", "漢字字");
    filters_to("PCSG01092", 0x80C5A6EAu, "【名@w前】　", "【名前】"); /* PCSG01144_1 */
    filters_to("PCSG01092", 0x80C5A6EAu, "台詞", "");
    filters_to("PCSG01092", 0x80020EA0u, "一\\n二\\c", "一二"); /* PCSG01144 */
    filters_to("PCSG00449", 0x80015842u, "【名前】@", "【名前】");
    filters_to("PCSG00449", 0x80015842u, "台詞@", "台詞"); /* kept: Luna's endWith(@) refills the buffer */
    filters_to("PCSG00449", 0x80015842u, "台詞", "");
    filters_to("PCSG01197", 0x8000DF58u, "#r漢|かん#@w字", "漢字");
    filters_to("PCSG01247", 0x80010C96u, "一\\n二", "一二");
    filters_to("PCSG01203", 0x8004C0CAu, "漢#Ruby[-1,かん]字", "漢字");
    filters_to("PCSG00992", 0x800A927Eu, "一\n二", "一二"); /* the hookfunc's own \n removal */
    filters_to("PCSG00914", 0x8005A534u, "㌫㍊㌻だ", "七海春歌だ");
    filters_to("PCSG00769", 0x8003CA08u, "㊤一　\n二㊦", "―一二―");
    /* commonfilter: a CP932 hit that is ▽ or ▼ alone */
    filters_to("PCSG00789", 0x800BB456u, "▼", "");
    filters_to("PCSG00789", 0x800BB456u, "▽", "");
    filters_to("PCSG00789", 0x800BB456u, "▼▼", "▼▼");
}

static void filters_utf8(void)
{
    filters_to("PCSG00787", 0x810286C8u, "一\n二", "一二"); /* NewLineCharFilterA */
    filters_to("PCSG01008", 0x8002FF90u, "　", "");     /* PCSG00766: a UTF-8 site's lone 　 */
    filters_to("PCSG01008", 0x8002FF90u, "#Ruby[字,じ]#n　次", "字次");
    filters_to("PCSG00944", 0x80039656u, "一\n　　二", "一二"); /* FPCSG01066 */
    filters_to("PCSG00020", 0x810DE344u, "<red>赤い<end>", "赤い");
    filters_to("PCSG01063", 0x8009B496u, "一\n　二\n三", "一二三");
    filters_to("PCSG00815", 0x80035948u, " 一 #n 二 #Wait[10]三", "一二三");
    filters_to("PCSG00935", 0x800644F6u, "台詞#n#Pos[0,1]名前", "【名前】台詞");
    filters_to("PCSG00935", 0x800259DCu, "#Ruby[字,じ]", "字");
    filters_to("PCSG00216", 0x81003DB0u, "[x]本\\n", "[x]本\\n"); /* FPCSG00839 on UTF-8: nothing */
    filters_to("PCSG00592", 0x8000AE7Eu, "#Color[2]赤#Color[0]", "赤");
    filters_to("PCSG00592", 0x8000AC70u, "一　二", "一二"); /* PCSG00833 */
    filters_to("PCSG01208", 0x80035B80u, "一#n　二#n三", "一二三");
    filters_to("PCSG01150", 0x800A0264u, "#Color[1]一#n　二", "一二");
    filters_to("PCSG01260", 0x801B1FC0u, "一@n二", "一二");
    filters_to("PCSG01260", 0x801B28C8u, "【名前】台詞", "【名前】");
    filters_to("PCSG01260", 0x801B28C8u, "台詞", "");
    filters_to("PCSG01260", 0x801B28C8u, "【名前", "");
    filters_to("PCSG01068", 0x8005426Cu, "一\\\\二\\三。!", "一\n二三。");
    /* PCSG00917 on UTF-8 bytes, as in Luna: た ends in 9F, a byte of ？, so its 　 stays */
    filters_to("PCSG00917", 0x8008128Au, "た　い　。　#n", "た　い。　");
    filters_to("PCSG00917", 0x8008128Au, "　始め", "始め");
    filters_to("PCSG00938", 0x8006C986u, "い　#Wait[10]止まれ!", "い止まれ");
    filters_to("PCSG00468", 0x80043538u, "一\\n　二\\k[三*注]×", "一二三");
    filters_to("PCSG00808", 0x8003542Au, "  一 #n 二 #Wait[10]三 #End  ", "一二三");
    filters_to("PCSG01075", 0x80035F44u, "  一 #n 二 #Wait[10]三 #End  ", "一二三");
    filters_to("PCSG01110", 0x80015BCCu, "#Name一　二", "一二");
    filters_to("PCSG01036", 0x8007838Cu, "一#n二", "一二");
    filters_to("PCSG01325", 0x80322DDAu, "一@wait_1*二", "一二");
    filters_to("PCSG00855", 0x80037600u, "一#n　二#Pos[1]三", "一二三");
    filters_to("PCSG00855", 0x801A2ADAu, "ПΧΔΛ《》∫∨∴∵Ц#n", "【、。っ（）「」『』】"); /* Cyrillic П Ц */
    filters_to("PCSG00654", 0x80059946u, "一\\二$", "一二");
    filters_to("PCSG00787", 0x8001D862u, "一\\二", "一二");
    filters_to("PCSG01114", 0x80029E5Cu, "@一{注釈}二{x", "一二{x");
    /* PCSG00482: Luna's StringFilter(u8"▼\f") removes ▼ only before a form feed (bytes E2 96 BC 0C), then
       CharFilter('\n') (audit-hooks-text F2: the port removed every ▼ and left the \f) */
    filters_to("PCSG00482", 0x80086798u, "一▼\f二\n三▼\f", "一二三");
    filters_to("PCSG00482", 0x80086798u, "一▼\n二", "一▼二");
    filters_to("PCSG00482", 0x8005F0C4u, "▼一\f二▼", "▼一\f二▼"); /* ▼ and \f apart: both kept */
    filters_to("PCSG01314", 0x8031BFA2u, "@r漢@かん@字@wait1", "漢字");
    filters_to("PCSG00745", 0x8000C2D4u, "一\\n二", "一二"); /* PCSG01247 on UTF-8 */
    filters_to("PCSG01180", 0x8006808Eu, "一\\n二,注", "一 二 ");
    filters_to("PCSG01162", 0x8000D3A4u, "一#n二", "一二"); /* PCSG00530 on UTF-8 */
    filters_to("PCSG01011", 0x801AE35Au, "名前×台詞\n続き", "台詞続き");
    filters_to("PCSG00866", 0x80030250u, "▼", "▼"); /* commonfilter: CP932 hits only */
}

static void filters_utf16(void)
{
    filters_to("PCSG00502", 0x800E954Eu, "あ\nい", "あい"); /* NewLineCharFilterW */
    filters_to("PCSG00708", 0x81558104u, "\\テ$キ", "テキ");
    filters_to("PCSG00543", 0x80014426u, "同じ", "同じ");
    filters_to("PCSG01049", 0x81F38A40u, "一<br>　<br>二　<br>　三", "一\n二三");
    filters_to("PCSG00829", 0x80009722u, "[x]y本文\\k%C#0a1f;強調%r\\n", "y本文強調"); /* FPCSG00839 */
    filters_to("PCSG00829", 0x800086C0u, "一\\n　二\\n[三,1]", "一二");
    filters_to("PCSG00706", 0x80C1F270u, "一<br>二", "一二"); /* FPCSG00706 */
    filters_to("PCSG01034", 0x8276B2CEu, "一\n二", "一二");
    filters_to("PCSG00367", 0x8004D4A0u, "\\一#二", "一二");
    filters_to("PCSG00433", 0x80086CB8u, "<CLT 5>強調<CLT>\n", "強調");
    filters_to("PCSG00615", 0x800714BCu, "一＿二", "一二");
    filters_to("PCSG01002", 0x8232D5C0u, "一\n<b>二</b>", "一二");
    filters_to("PCSG01046", 0x81E1F5C8u, "一<br>二", "一二");
    filters_to("PCSG00689", 0x8097D494u, "ToLOVEる", "ToLOVEる");
    filters_to("PCSG00591", 0x800151A4u, "\U0001F600字", "\U0001F600字"); /* a surrogate pair */
}

/* The ops this build has but no table site uses (all_ascii_FilterW is the refused
 * USING_CHAR sites' filter) and the text's limits. */
static void filter_ops_and_limits(void)
{
    VjoLunaSite x = *site("PCSG00543", 0x80014426u);
    uint8_t raw[4000];
    char out[2048];
    unsigned n;
    x.filter = VJO_LF_ALL_ASCII;
    n = encode(x.encoding, "abc", raw, sizeof raw);
    TEST_CHECK(vjo_luna_text(&x, raw, n, x.encoding, out, sizeof out) == 0);
    n = encode(x.encoding, "aあ", raw, sizeof raw);
    TEST_CHECK(vjo_luna_text(&x, raw, n, x.encoding, out, sizeof out) == 4 && !strcmp(out, "aあ"));
    /* the katakana map, as the table's copy has it */
    TEST_CHECK(vjo_luna_kana[0xFF71 - 0xFF61] == 0x3042 && vjo_luna_kana_extra[0][0] == '?');
    /* a refused filter (PCSG00451): no text, whatever comes */
    x.filter = VJO_LF_PCSG00451;
    TEST_CHECK(!vjo_luna_ported(&x) && vjo_luna_text(&x, raw, n, x.encoding, out, sizeof out) == 0);
    /* a text longer than VJO_LUNA_TEXT_MAX: cut at its last whole character first */
    x = *site("PCSG00789", 0x800BB456u);
    for (n = 0; n + 2 <= 3000; n += 2) raw[n] = 0x82, raw[n + 1] = 0xA0; /* あ x 1500 */
    TEST_CHECK(vjo_luna_text(&x, raw, n, x.encoding, out, sizeof out) == (VJO_LUNA_TEXT_MAX - 1) / 3 * 3);
    TEST_CHECK(strlen(out) % 3 == 0);
    /* a small out: the last whole character */
    TEST_CHECK(vjo_luna_text(&x, raw, 6, x.encoding, out, 4) == 3 && !strcmp(out, "あ"));
    TEST_CHECK(vjo_luna_text(&x, raw, 6, x.encoding, out, 1) == 0 && !out[0]);
    TEST_CHECK(vjo_luna_text(&x, raw, 6, x.encoding, NULL, 8) == 0);
    TEST_CHECK(vjo_luna_text(NULL, raw, 6, x.encoding, out, sizeof out) == 0);
    /* a filter's output growing past the buffer: cut, still whole characters (① -> ・・・ x 3) */
    x = *site("PCSG00410", 0x8004E44Au);
    for (n = 0; n + 2 <= 1000; n += 2) raw[n] = 0x87, raw[n + 1] = 0x40; /* ① x 500 */
    TEST_CHECK(vjo_luna_text(&x, raw, n, x.encoding, out, 600) == 597);
    TEST_CHECK(!strncmp(out, "・・・・", 12));
}

/* ---- the regex subset ---- */

static void regex_vectors(void)
{
    unsigned bad = 0;
    for (unsigned i = 0; i < VJO_RE_VECTORS; i++) {
        const char *pat = vjo_re_pats[vjo_re_vectors[i].pat][0], *rep = vjo_re_pats[vjo_re_vectors[i].pat][1];
        const char *in = vjo_re_inputs[vjo_re_vectors[i].in];
        const char *want = vjo_re_vectors[i].want ? vjo_re_vectors[i].want : in;
        unsigned flags = vjo_re_vectors[i].bytes ? VJO_RE_BYTES : 0;
        unsigned long budget = 1000000;
        char out[2048];
        if (rep) {
            int r = vjo_re_sub(pat, flags, in, strlen(in), rep, out, sizeof out, &budget);
            if (r != (int)strlen(want) || strcmp(out, want)) {
                if (bad++ < 12) {
                    TEST_CHECK_(0, "vector %u: /%s/%s -> %s", i, pat, flags ? " (bytes)" : "", rep);
                    TEST_MSG("in   [%s]", in);
                    TEST_MSG("want [%s]", want);
                    TEST_MSG("got  [%s] (%d)", out, r);
                }
            }
        } else {
            long m[4];
            int f = vjo_re_search(pat, flags, in, strlen(in), 1, m, &budget);
            if (f != vjo_re_vectors[i].found ||
                (f && memcmp(m, vjo_re_vectors[i].span, sizeof m))) {
                if (bad++ < 12) {
                    TEST_CHECK_(0, "search vector %u: /%s/ in [%s]: %d %ld %ld %ld %ld", i, pat, in, f, m[0], m[1],
                                m[2], m[3]);
                }
            }
        }
    }
    TEST_CHECK_(!bad, "%u of %u vectors differ from Python's re", bad, (unsigned)VJO_RE_VECTORS);
}

/* Rubi's own filter f (VJO_RF_*) on utf8 held in encoding enc, with no Luna site: want */
static void rubi_to(unsigned f, unsigned enc, const char *in, const char *want)
{
    uint8_t raw[1024];
    char out[1024];
    unsigned n = encode(enc, in, raw, sizeof raw);
    size_t got = vjo_luna_text_filter(NULL, f, enc, raw, n, enc, out, sizeof out);
    TEST_CHECK_(got == strlen(want) && !strcmp(out, want), "filter 0x%02X enc %u: [%s]", f, enc, in);
    TEST_MSG("want [%s]", want);
    TEST_MSG("got  [%s]", out);
}

/* Rubi's filters (core/hook_luna.c, docs/native-hooks.md#rubis-filters), on examples from the farm's
 * hook bytes (bee-san/psandvita hooks/vita/<title>/RUBI-HOOK.md and hook-text-artifacts.jsonl). */
static void rubi_filters(void)
{
    const VjoLunaSite *s;
    uint8_t raw[256];
    char out[256];
    unsigned n;
    /* every Rubi filter is known and has ops; nothing between Luna's and Rubi's, nor after Rubi's */
    for (unsigned f = VJO_RF_BASE; f < VJO_RF_END; f++) TEST_CHECK_(vjo_luna_filter_known(f), "0x%02X", f);
    TEST_CHECK(vjo_luna_filter_known(VJO_LUNA_NONE) && vjo_luna_filter_known(VJO_LF_CARET));
    TEST_CHECK(!vjo_luna_filter_known(VJO_LF_COUNT) && !vjo_luna_filter_known(VJO_RF_BASE - 1u));
    TEST_CHECK(!vjo_luna_filter_known(VJO_RF_END) && !vjo_luna_filter_known(0x100u));
    TEST_CHECK(VJO_LF_COUNT <= VJO_RF_BASE);

    /* PROTOTYPE: PCSG00007 event 3 (a nameplate), PCSG00206 (an inline name, ruby), PCSG00620 events 80-81 */
    rubi_to(VJO_RF_PROTOTYPE, VJO_HOOK_ENC_CP932, "$t声$d｢ｷｮｰｽｹが帰ｯﾃｷﾀぞｰｯ！｣", "声\n「きょーすけが帰ってきたぞーっ！」");
    rubi_to(VJO_RF_PROTOTYPE, VJO_HOOK_ENC_CP932, "$t天音@｢$[周防$/すおう$]ﾉ話｣$d次", "天音「周防の話」\n次");
    rubi_to(VJO_RF_PROTOTYPE, VJO_HOOK_ENC_CP932, "\n　$A1$S120ﾃｽﾄ$S$w$C[ff0000]赤$C[]＿終", "てすと赤　終");
    rubi_to(VJO_RF_PROTOTYPE, VJO_HOOK_ENC_CP932, "$L1一般設定", "一般設定");
    /* IFENGINE: PCSG00448 event 1 and 54, PCSG00935 event 1, PCSG00410's #Pos */
    rubi_to(VJO_RF_IFENGINE, VJO_HOOK_ENC_CP932, "#Scale[1.0]一行目#n二行目#n", "一行目\n二行目");
    rubi_to(VJO_RF_IFENGINE, VJO_HOOK_ENC_UTF8, "#Ruby[漢字,かんじ]を#Color[7]読む#Color[0]#n", "漢字を読む");
    rubi_to(VJO_RF_IFENGINE, VJO_HOOK_ENC_CP932, "……#nLAGRANGE POINTの", "……\nLAGRANGE POINTの");
    rubi_to(VJO_RF_IFENGINE, VJO_HOOK_ENC_UTF8, "#Pos[10, 20]#Speed[5]台詞#Upper", "台詞");
    /* default names before the expander: PCSG00448 event 208, PCSG00469 sample 001345, PCSG00853 */
    rubi_to(VJO_RF_PCSG00448, VJO_HOOK_ENC_CP932, "担当する#n#Name[1]#Name[2]です。", "担当する\n片岡ユエです。");
    rubi_to(VJO_RF_PCSG00469, VJO_HOOK_ENC_UTF8, "私は#Name[1]。#n", "私は撫子。");
    rubi_to(VJO_RF_PCSG00853, VJO_HOOK_ENC_UTF8, "#Name[1]#Name[2]、#Name[3]#Color[3]！#n", "花宮かずは、カズハ！");
    /* <base,reading>: PCSG00648 sample 000005; PCSG00509 event 1011's name code and its ^ */
    rubi_to(VJO_RF_ANGLE_RUBY, VJO_HOOK_ENC_CP932, "…旅立って<一月,ひとつき>が経ちました。", "…旅立って一月が経ちました。");
    rubi_to(VJO_RF_PCSG00509, VJO_HOOK_ENC_CP932, "「どしたの、＊Ｂ？^ひょっとして<疲,つか>れちゃった？」",
            "「どしたの、鈴？\nひょっとして疲れちゃった？」");
    /* <br> (PCSG00807 sample 000002, PCSG00553's centred card), a lone backslash (PCSG00787 event 1) */
    rubi_to(VJO_RF_BR_NEWLINE, VJO_HOOK_ENC_UTF16LE, "　　鬼丸国綱<br>次の行<br>", "鬼丸国綱\n次の行");
    rubi_to(VJO_RF_BACKSLASH_NEWLINE, VJO_HOOK_ENC_UTF8, "一行目\\二行目\\", "一行目\n二行目");
    /* PCSG00450's voice cue, PCSG00817's codes, PCSG00876's @@ and $<digit> */
    rubi_to(VJO_RF_PCSG00450, VJO_HOOK_ENC_UTF16LE, "ダイチ「《SP QUE,1,0012》行くぞ」", "ダイチ「行くぞ」");
    rubi_to(VJO_RF_PCSG00817, VJO_HOOK_ENC_CP932, "[w024][h024]一行目[n][c00ff00]二行目[e]", "一行目\n二行目");
    rubi_to(VJO_RF_PCSG00876, VJO_HOOK_ENC_UTF8, "$4強調$0です@@次", "強調です\n次");
    /* PCSG01194 events 1 and 21: a name, a pause and a voice, half-width kana; narration from @r */
    rubi_to(VJO_RF_PCSG01194, VJO_HOOK_ENC_CP932, "男@r@p10.@vA_男a6_04.｢ぎｬﾞ｣", "男\n「ぎゃ！」");
    rubi_to(VJO_RF_PCSG01194, VJO_HOOK_ENC_CP932, "@rｿﾝﾅ･･私ﾊ･･ﾞ", "そんな……私は……！");
    /* PCSG00540: a named line with a voice, a pause and b ruby; narration with and without a voice */
    rubi_to(VJO_RF_PCSG00540, VJO_HOOK_ENC_CP932, "c12.大石rv01_0001.ｺﾚﾊw300.bき.<吉>だ･ﾞ", "大石\nこれは吉だ…！");
    rubi_to(VJO_RF_PCSG00540, VJO_HOOK_ENC_CP932, "rvzen01.ｿｳｶﾟ", "そうか？");
    rubi_to(VJO_RF_PCSG00540, VJO_HOOK_ENC_CP932, "r静かだ", "静かだ");
    /* a 0xA0 byte: the blank cell the game draws (farm page 82: 東京都　陵南学園。, 9 glyphs) */
    rubi_to(VJO_RF_PCSG00540, VJO_HOOK_ENC_CP932, "r東京都\xef\xa3\xb0陵南学園｡", "東京都　陵南学園。");
    rubi_to(VJO_RF_PROTOTYPE, VJO_HOOK_ENC_CP932, "$t猫@｢セブンズ･カオスﾅのニャ！｣", "猫「セブンズ・カオスなのニャ！」");
    /* PCSG00628: the voice after the name, a ruby with a line break before its base, waits, the 0xA0 blank,
     * !X literals (farm events 20, 148, 207) */
    rubi_to(VJO_RF_PCSG00628, VJO_HOOK_ENC_CP932, "月詠rvKY_tu_0417_012.｢･･ﾞﾟ\xef\xa3\xb0ﾅぜ｣|y", "月詠\n「……！？　なぜ」");
    rubi_to(VJO_RF_PCSG00628, VJO_HOOK_ENC_CP932, "rｿｼﾃ､bうたたね.r<転寝>ｦ[―――]k｡", "そして、\n転寝を―――。");
    rubi_to(VJO_RF_PCSG00628, VJO_HOOK_ENC_CP932, "r“bヴィジター.<!V!I!S!I!T!O!R>”", "“VISITOR”");
    /* main.snr's z<n>. (a size) and u<n>. (a code point), dropped; a voice ID's u after the name stays the voice's */
    rubi_to(VJO_RF_PCSG00628, VJO_HOOK_ENC_CP932, "堂元rv0428_03_Tu_0018.｢z140.ﾏﾃ!!u9829.｣", "堂元\n「まて！」");
    /* PCSG00380: the dash pieces, the player's default names (farm pages 37, 158), the page's last line break */
    rubi_to(VJO_RF_PCSG00380, VJO_HOOK_ENC_CP932, "㌫㍊も、\n「おい、㌫㊤㊥㊦」\n", "北條紗希も、\n「おい、北條―――」");

    /* in place of a Luna site's filter: PCSG00448's own drops the name the hook sees before the expander */
    s = site("PCSG00448", 0x800718F8u);
    if (s) {
        n = encode(VJO_HOOK_ENC_CP932, "#Name[2]です。#n", raw, sizeof raw);
        TEST_CHECK(vjo_luna_text(s, raw, n, VJO_HOOK_ENC_CP932, out, sizeof out) == strlen("です。") &&
                   !strcmp(out, "です。"));
        TEST_CHECK(vjo_luna_text_filter(s, VJO_RF_PCSG00448, VJO_HOOK_ENC_CP932, raw, n, VJO_HOOK_ENC_CP932, out,
                                        sizeof out) == strlen("ユエです。") && !strcmp(out, "ユエです。"));
        /* filter 0: none at all, not the site's */
        TEST_CHECK(vjo_luna_text_filter(s, VJO_LUNA_NONE, VJO_HOOK_ENC_CP932, raw, n, VJO_HOOK_ENC_CP932, out,
                                        sizeof out) == strlen("#Name[2]です。#n"));
    }
    /* a site's hookfunc clean-up still runs first (PCSG00914's name glyphs), then the Rubi filter */
    s = site("PCSG00914", 0x8005A534u);
    if (s) {
        n = encode(VJO_HOOK_ENC_CP932, "㌫と<漢,かん>", raw, sizeof raw);
        TEST_CHECK(vjo_luna_text_filter(s, VJO_RF_ANGLE_RUBY, VJO_HOOK_ENC_CP932, raw, n, VJO_HOOK_ENC_CP932, out,
                                        sizeof out) > 0 && !strcmp(out, "七海と漢"));
        TEST_MSG("got [%s]", out);
    }
    /* an unknown filter, a site that is not ported, no input or no buffer: no text */
    n = encode(VJO_HOOK_ENC_UTF8, "台詞", raw, sizeof raw);
    TEST_CHECK(vjo_luna_text_filter(NULL, VJO_RF_END, VJO_HOOK_ENC_UTF8, raw, n, VJO_HOOK_ENC_UTF8, out, sizeof out) ==
               0 && !out[0]);
    TEST_CHECK(vjo_luna_text_filter(NULL, 0x200u, VJO_HOOK_ENC_UTF8, raw, n, VJO_HOOK_ENC_UTF8, out, sizeof out) == 0);
    {
        VjoLunaSite off;
        memset(&off, 0, sizeof off);
        off.encoding = VJO_HOOK_ENC_UTF8;
        TEST_CHECK(vjo_luna_text_filter(&off, VJO_RF_ANGLE_RUBY, VJO_HOOK_ENC_UTF8, raw, n, VJO_HOOK_ENC_UTF8, out,
                                        sizeof out) == 0);
    }
    TEST_CHECK(vjo_luna_text_filter(NULL, VJO_RF_ANGLE_RUBY, VJO_HOOK_ENC_UTF8, NULL, n, VJO_HOOK_ENC_UTF8, out,
                                    sizeof out) == 0);
    TEST_CHECK(vjo_luna_text_filter(NULL, VJO_RF_ANGLE_RUBY, VJO_HOOK_ENC_UTF8, raw, n, VJO_HOOK_ENC_UTF8, NULL, 9) ==
               0);
    TEST_CHECK(vjo_luna_text_filter(NULL, VJO_LUNA_NONE, VJO_HOOK_ENC_UTF8, raw, n, VJO_HOOK_ENC_UTF8, out,
                                    sizeof out) == n && !strcmp(out, "台詞"));
}

static void regex_limits(void)
{
    char out[64];
    unsigned long budget = 1000;
    long m[4];
    /* outside the subset: refused, never guessed */
    static const char *const refused[] = {"\\c", "(?:a)", "a{2}", "(a)\\1", "a**", "(a", "a)", "*a",
                                          "\\b", "[a", "\\x41", "[z-a]", "\\q"};
    for (unsigned i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        budget = 1000;
        TEST_CHECK_(vjo_re_sub(refused[i], 0, "aa", 2, "", out, sizeof out, &budget) == VJO_RE_EPATTERN, "%s",
                    refused[i]);
        TEST_CHECK(!out[0]);
    }
    budget = 1000;
    TEST_CHECK(vjo_re_sub("(a)(b)", 0, "ab", 2, "$1$2", out, sizeof out, &budget) == VJO_RE_EPATTERN);
    TEST_CHECK(vjo_re_sub("(a)", 0, "ab", 2, "$2", out, sizeof out, &budget) == VJO_RE_EPATTERN);
    /* a pattern too long for the program */
    TEST_CHECK(vjo_re_sub("a|b|c|d|e|f|g|h|i|j|k|l|m|n|o|p|q|r|s|t", 0, "x", 1, "", out, sizeof out, &budget) ==
               VJO_RE_EPATTERN);
    /* the budget */
    budget = 50;
    TEST_CHECK(vjo_re_sub("(.*?)z", 0, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 48, "", out, sizeof out,
                          &budget) == VJO_RE_EBUDGET);
    TEST_CHECK(!out[0] && budget == 0);
    /* the output cut at a whole character */
    budget = 100000;
    TEST_CHECK(vjo_re_sub("x", 0, "あいう", 9, "", out, 8, &budget) == 6 && !strcmp(out, "あい"));
    TEST_CHECK(vjo_re_sub("あ", 0, "ああ", 6, "いいい", out, 8, &budget) == 6 && !strcmp(out, "いい"));
    /* $&, $$ and a literal $ */
    TEST_CHECK(vjo_re_sub("b+", 0, "abbc", 4, "[$&$$$x]", out, sizeof out, &budget) == 9 &&
               !strcmp(out, "a[bb$$x]c"));
    /* an unmatched group is empty */
    TEST_CHECK(vjo_re_sub("a(x)?b", 0, "ab", 2, "<$1>", out, sizeof out, &budget) == 2 && !strcmp(out, "<>"));
    /* the search API */
    TEST_CHECK(vjo_re_search("b(c+)", 0, "abccd", 5, 1, m, &budget) == 1 && m[0] == 1 && m[1] == 4 && m[2] == 2 &&
               m[3] == 4);
    TEST_CHECK(vjo_re_search("z", 0, "abc", 3, 0, m, &budget) == 0 && m[0] == -1);
    TEST_CHECK(vjo_re_search("b", 0, "abc", 3, 0, m, &budget) == 1 && m[2] == 1 && m[3] == 2);
    /* bytes against characters on UTF-8 */
    TEST_CHECK(vjo_re_sub(".", VJO_RE_BYTES, "あ", 3, "x", out, sizeof out, &budget) == 3 && !strcmp(out, "xxx"));
    TEST_CHECK(vjo_re_sub(".", 0, "あ", 3, "x", out, sizeof out, &budget) == 1 && !strcmp(out, "x"));
    TEST_CHECK(vjo_re_sub("あ*", VJO_RE_BYTES, "\xe3\x81\x82\x82", 4, "-", out, sizeof out, &budget) > 0);
    /* line terminators: '.' stops at \n and \r, and in characters at U+2028 */
    TEST_CHECK(vjo_re_sub("a.c", 0, "a\nc a\rc a\xe2\x80\xa8" "c abc", 17, "", out, sizeof out, &budget) == 14);
    TEST_CHECK(vjo_re_sub("a.+", VJO_RE_BYTES, "a\xe2\x80\xa8", 4, "", out, sizeof out, &budget) == 0);
    /* no input: empty matches only */
    TEST_CHECK(vjo_re_sub("x*", 0, NULL, 5, "-", out, sizeof out, &budget) == 1 && !strcmp(out, "-"));
    TEST_CHECK(vjo_re_sub("x", 0, "x", 1, "", NULL, 0, &budget) == VJO_RE_EPATTERN);
    budget = 0;
    TEST_CHECK(vjo_re_sub("x", 0, "x", 1, "", out, sizeof out, &budget) == VJO_RE_EBUDGET);
    TEST_CHECK(vjo_re_sub("x", 0, "x", 1, "", out, sizeof out, NULL) == 0); /* the default budget */
}

TEST_LIST = {
    {"table_counts", table_counts},
    {"lookups", lookups},
    {"version_gate", version_gate},
    {"capture_default", capture_default},
    {"capture_hookfuncs", capture_hookfuncs},
    {"capture_bounded", capture_bounded},
    {"filters_cp932", filters_cp932},
    {"filters_utf8", filters_utf8},
    {"filters_utf16", filters_utf16},
    {"filter_ops_and_limits", filter_ops_and_limits},
    {"regex_vectors", regex_vectors},
    {"regex_limits", regex_limits},
    {"rubi_filters", rubi_filters},
    {NULL, NULL},
};
