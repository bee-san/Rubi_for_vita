/* The debug build's read-only PSP probe, its pure-C half (core/psp_probe.c): Luna's PSP table as the
 * probe looks it up (every title against core/hook_luna_psp.inc, and entries copied by hand from
 * third_party/LunaTranslator/ppsspp_1.cpp), the bounds of the probe's reads, the title ID, and the
 * log's lines (exact text, the longest each can be, and every cut short of it on a heap buffer of
 * exactly that size, so ASan sees any byte written past it). */
#include "acutest.h"

#include <stdlib.h>
#include <string.h>

#include "../../core/psp_probe.h"
#include "../../core/hook_luna_psp.inc" /* this test's own copy, to walk every title */

static void test_counts(void)
{
    unsigned sum = 0;
    /* reports/hooks-psp.md section 3: 415 entries, 354 IDs; 13 entries name several (431 sites) */
    TEST_CHECK(VJO_LUNA_PSP_ENTRIES == 415u);
    TEST_CHECK(VJO_LUNA_PSP_TITLES == 354u);
    TEST_CHECK(VJO_LUNA_PSP_SITES == 431u);
    TEST_CHECK(VJO_LUNA_PSP_SITES_PER_TITLE_MAX == 6u);
    for (unsigned i = 0; i < VJO_LUNA_PSP_TITLES; i++) {
        TEST_CHECK(vjo_luna_psp_count[i] >= 1u && vjo_luna_psp_count[i] <= VJO_LUNA_PSP_SITES_PER_TITLE_MAX);
        sum += vjo_luna_psp_count[i];
    }
    TEST_CHECK(sum == VJO_LUNA_PSP_SITES);
    TEST_CHECK(strlen(vjo_luna_psp_ids) == VJO_LUNA_PSP_TITLES * 9u);
}

/* Every title, through the probe's lookup, gives its sites from the table, in order. */
static void test_every_title(void)
{
    unsigned first = 0;
    for (unsigned i = 0; i < VJO_LUNA_PSP_TITLES; i++) {
        char id[10];
        uint32_t got[VJO_PSP_SITES_MAX];
        unsigned total = 99, n;
        memcpy(id, &vjo_luna_psp_ids[i * 9u], 9);
        id[9] = 0;
        if (i) TEST_CHECK(memcmp(&vjo_luna_psp_ids[(i - 1u) * 9u], id, 9) < 0); /* sorted, no repeat */
        n = vjo_psp_luna_sites(id, got, VJO_PSP_SITES_MAX, &total);
        TEST_CHECK(n == vjo_luna_psp_count[i] && total == n);
        TEST_MSG("%s: %u sites, total %u", id, n, total);
        for (unsigned k = 0; k < n; k++) {
            TEST_CHECK(got[k] == vjo_luna_psp_addr[first + k]);
            TEST_CHECK(got[k] % 4u == 0 && got[k] >= 0x08804000u && got[k] < 0x0C000000u);
        }
        first += vjo_luna_psp_count[i];
    }
    TEST_CHECK(first == VJO_LUNA_PSP_SITES);
}

typedef struct {
    const char *id;
    unsigned n;
    uint32_t addr[6];
} Known;

/* Copied by hand from ppsspp_1.cpp at the pin (line numbers there), not from the generator's output. */
static const Known known[] = {
    {"ULJM05889", 1, {0x8868AB8}},                     /* :1843 ましろ色シンフォニー, the probe's first console title */
    {"ULJM05795", 1, {0x8836918}},                     /* :2347 */
    {"ULJS00273", 1, {0x8808764}},                     /* :1704, the table's first entry */
    {"ULJS00339", 1, {0x0886775c}},                    /* :1790 */
    {"NPJH50505", 1, {0x8958490}},                     /* :1821 */
    {"NPJH50654", 1, {0x89DB26C}},                     /* :1712, one entry with two IDs */
    {"NPJH50655", 1, {0x89DB26C}},                     /* :1712 */
    {"ULJM06302", 2, {0x880a744, 0x8804094}},          /* :1906-1907, two entries with three IDs; the lowest address */
    {"ULJM06303", 2, {0x880a744, 0x8804094}},
    {"ULJM06307", 2, {0x880a744, 0x8804094}},
    {"ULJM06025", 1, {0x8B0449C}},                     /* :2085, the highest address */
    {"NPJH50619", 6, {0x8952cfc, 0x884aad4, 0x882e1b0, 0x88bb108, 0x89526a0, 0x88bcef8}}, /* :1814-1819 */
};

static void test_known_entries(void)
{
    for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        uint32_t got[VJO_PSP_SITES_MAX] = {0};
        unsigned total = 0, n = vjo_psp_luna_sites(known[i].id, got, VJO_PSP_SITES_MAX, &total);
        TEST_CHECK(n == known[i].n && total == known[i].n);
        TEST_MSG("%s: %u sites, %u expected", known[i].id, n, known[i].n);
        for (unsigned k = 0; k < known[i].n && k < n; k++) TEST_CHECK(got[k] == known[i].addr[k]);
    }
}

static void test_not_listed(void)
{
    /* MEMORIES OFF (a comment in Luna's table only), a Vita ID, and what is not an ID */
    static const char *const ids[] = {"ULJM05334", "PCSG00415", "ULJM0588", "uljm05889", "ULJM0588X", "ULJ M0588",
                                      "", "AAAA00000", "ZZZZ99999"};
    uint32_t got[VJO_PSP_SITES_MAX];
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        unsigned total = 7;
        TEST_CHECK(vjo_psp_luna_sites(ids[i], got, VJO_PSP_SITES_MAX, &total) == 0 && total == 0);
        TEST_MSG("%s", ids[i]);
    }
    TEST_CHECK(vjo_psp_luna_sites(NULL, got, VJO_PSP_SITES_MAX, NULL) == 0);
    /* only 9 characters are read: what follows them does not matter */
    TEST_CHECK(vjo_psp_luna_sites("ULJM05889 and more", got, VJO_PSP_SITES_MAX, NULL) == 1 && got[0] == 0x8868AB8u);
}

static void test_cap(void)
{
    uint32_t got[VJO_PSP_SITES_MAX + 4];
    unsigned total = 0;
    memset(got, 0xAB, sizeof(got));
    TEST_CHECK(vjo_psp_luna_sites("NPJH50619", got, 2, &total) == 2 && total == 6);
    TEST_CHECK(got[0] == 0x8952CFCu && got[1] == 0x884AAD4u && got[2] == 0xABABABABu);
    TEST_CHECK(vjo_psp_luna_sites("NPJH50619", got, 0, &total) == 0 && total == 6);
    TEST_CHECK(vjo_psp_luna_sites("NPJH50619", NULL, 8, &total) == 0 && total == 6);
    TEST_CHECK(vjo_psp_luna_sites("NPJH50619", got, 1000, &total) == 6 && total == 6);
    TEST_CHECK(got[6] == 0xABABABABu); /* nothing past the title's sites */
}

static void test_vita_addr(void)
{
    TEST_CHECK(vjo_psp_vita_addr(0x08000000u) == VJO_PSP_VIEW_RAM);
    TEST_CHECK(vjo_psp_vita_addr(0x08800000u) == VJO_PSP_VIEW_USER);
    TEST_CHECK(vjo_psp_vita_addr(0xABCDE000u) == VJO_PSP_VIEW_ADRENALINE);
    TEST_CHECK(vjo_psp_vita_addr(0xABCDE000u + VJO_PSP_TITLEID_OFFSET) == 0x73CDE098u);
    TEST_CHECK(vjo_psp_vita_addr(0x08868AB8u) == 0x70868AB8u);
    TEST_CHECK(vjo_psp_vita_addr(0x0BFFFFFFu) == 0x73FFFFFFu);
}

static VjoPspBlock block(uint32_t addr, uint32_t base, uint32_t size, uint32_t access)
{
    VjoPspBlock b = {addr, 0x1234, 0, base, size, 0xD0, access, 0x0C20D060};
    return b;
}

static void test_blocks(void)
{
    VjoPspBlock b = block(0x70800000u, 0x70000000u, 0x04000000u, 6);
    VjoPspBlock two[2];
    TEST_CHECK(vjo_psp_block_ok(&b));
    b.access = 2; /* write only */
    TEST_CHECK(!vjo_psp_block_ok(&b));
    b = block(0x70800000u, 0x70000000u, 0x04000000u, 4);
    b.uid = -2147352575; /* 0x80020001-ish: not found */
    TEST_CHECK(!vjo_psp_block_ok(&b));
    b = block(0x70800000u, 0x70000000u, 0x04000000u, 4);
    b.info_rc = -1;
    TEST_CHECK(!vjo_psp_block_ok(&b));
    TEST_CHECK(!vjo_psp_block_ok(NULL));
    b = block(0x70800000u, 0x70000000u, 0, 4); /* empty */
    TEST_CHECK(!vjo_psp_block_ok(&b));
    b = block(0x6FFFFFFFu, 0x70000000u, 0x04000000u, 4); /* below the block */
    TEST_CHECK(!vjo_psp_block_ok(&b));
    b = block(0x74000000u, 0x70000000u, 0x04000000u, 4); /* just past it */
    TEST_CHECK(!vjo_psp_block_ok(&b));

    b = block(0x70000000u, 0x70000000u, 0x04000000u, 4);
    TEST_CHECK(vjo_psp_inside(&b, 1, 0x70000000u, 4));
    TEST_CHECK(vjo_psp_inside(&b, 1, 0x73FFFFFCu, 4));
    TEST_CHECK(!vjo_psp_inside(&b, 1, 0x73FFFFFDu, 4)); /* its last byte past the end */
    TEST_CHECK(!vjo_psp_inside(&b, 1, 0x6FFFFFFCu, 4));
    TEST_CHECK(!vjo_psp_inside(&b, 1, 0x70000000u, 0));
    TEST_CHECK(vjo_psp_inside(&b, 1, 0x70000000u, 0x04000000u));
    TEST_CHECK(!vjo_psp_inside(&b, 1, 0x70000000u, 0x04000001u));
    TEST_CHECK(!vjo_psp_inside(&b, 0, 0x70000000u, 4));
    TEST_CHECK(!vjo_psp_inside(NULL, 1, 0x70000000u, 4));
    /* a block at the top of the address space: no overflow either way */
    b = block(0xFFFFF000u, 0xFFFFF000u, 0x1000u, 4);
    TEST_CHECK(vjo_psp_inside(&b, 1, 0xFFFFFFFCu, 4));
    TEST_CHECK(!vjo_psp_inside(&b, 1, 0xFFFFFFFEu, 4)); /* wraps */
    TEST_CHECK(!vjo_psp_inside(&b, 1, 0xFFFFFFFFu, 0xFFFFFFFFu));
    /* a block that is not ok is never used; the second one is */
    two[0] = block(0x70000000u, 0x70000000u, 0x04000000u, 2);
    two[1] = block(0x73CDE000u, 0x73CDE000u, 0x2000u, 4);
    TEST_CHECK(!vjo_psp_inside(two, 2, 0x70800000u, 4));
    TEST_CHECK(vjo_psp_inside(two, 2, 0x73CDE098u, VJO_PSP_TITLEID_BYTES));
    TEST_CHECK(!vjo_psp_inside(two, 2, 0x73CDFFF8u, VJO_PSP_TITLEID_BYTES));
}

static void test_title_id(void)
{
    char id[10];
    static const uint8_t ok[12] = {'U', 'L', 'J', 'M', '0', '5', '8', '8', '9', 0, 0, 0};
    static const uint8_t junk_after[12] = {'N', 'P', 'J', 'H', '5', '0', '5', '0', '5', 0, 'x', 'y'};
    static const uint8_t no_nul[12] = {'U', 'L', 'J', 'M', '0', '5', '8', '8', '9', 'X', 0, 0};
    static const uint8_t xmb[12] = {'X', 'M', 'B', 0xE2, 0x84, 0xA2, 0, 0, 0, 0, 0, 0};
    static const uint8_t lower[12] = {'u', 'l', 'j', 'm', '0', '5', '8', '8', '9', 0, 0, 0};
    static const uint8_t zero[12] = {0};
    TEST_CHECK(vjo_psp_title_id(ok, id) == 1 && !strcmp(id, "ULJM05889"));
    TEST_CHECK(vjo_psp_title_id(junk_after, id) == 1 && !strcmp(id, "NPJH50505")); /* after the NUL: strcpy's leftovers */
    TEST_CHECK(vjo_psp_title_id(no_nul, id) == 0 && id[0] == 0);
    TEST_CHECK(vjo_psp_title_id(xmb, id) == 0 && id[0] == 0);
    TEST_CHECK(vjo_psp_title_id(lower, id) == 0 && id[0] == 0);
    TEST_CHECK(vjo_psp_title_id(zero, id) == 0 && id[0] == 0);
    TEST_CHECK(vjo_psp_title_id(NULL, id) == 0 && id[0] == 0);
}

static void line_is(size_t n, const char *buf, const char *want)
{
    TEST_CHECK(n == strlen(want) && !strcmp(buf, want));
    TEST_MSG("got  [%s]\nwant [%s]", buf, want);
}

static void test_lines(void)
{
    char buf[VJO_PSP_LINE_MAX];
    uint32_t words[4] = {0x27BDFFE0u, 0xAFBF001Cu, 0x00000000u, 0x3C028890u};
    VjoPspBlock b = block(0x70800000u, 0x70000000u, 0x04000000u, 6);
    static const uint8_t raw[12] = {'U', 'L', 'J', 'M', '0', '5', '8', '8', '9', 0, 0, 0};
    static const uint8_t odd[12] = {'"', '\\', 0x01, 0x7F, 'A', 0xFF, 'z', ' ', 0, 'Q', 0, 0};
    line_is(vjo_psp_line_start(buf, sizeof(buf), 1), buf, "psp probe 1: start: text source 1\n");
    line_is(vjo_psp_line_module(buf, sizeof(buf), 0, 0x3F75D4D3u), buf,
            "psp probe 1: ScePspemu: rc 0x00000000 nid 0x3F75D4D3\n");
    line_is(vjo_psp_line_module(buf, sizeof(buf), -2147352575, 0), buf,
            "psp probe 1: ScePspemu: rc 0x80020001 nid 0x00000000\n");
    line_is(vjo_psp_line_block(buf, sizeof(buf), &b), buf,
            "psp probe 1: block 0x70800000: uid 0x00001234 info 0x00000000 base 0x70000000 size 0x04000000 "
            "memtype 0x000000D0 access 0x00000006 type 0x0C20D060 ok\n");
    b.uid = -1;
    b.info_rc = (int32_t)0x80020001u;
    b.base = b.size = b.memtype = b.access = b.type = 0;
    line_is(vjo_psp_line_block(buf, sizeof(buf), &b), buf,
            "psp probe 1: block 0x70800000: uid 0xFFFFFFFF info 0x80020001 base 0x00000000 size 0x00000000 "
            "memtype 0x00000000 access 0x00000000 type 0x00000000 bad\n");
    line_is(vjo_psp_line_titleid(buf, sizeof(buf), 0x73CDE098u, 1, raw), buf,
            "psp probe 1: titleid 0x73CDE098: 554C4A4D3035383839000000 \"ULJM05889\"\n");
    line_is(vjo_psp_line_titleid(buf, sizeof(buf), 0x73CDE098u, 1, odd), buf,
            "psp probe 1: titleid 0x73CDE098: 225C017F41FF7A2000510000 \"....A.z \"\n");
    line_is(vjo_psp_line_titleid(buf, sizeof(buf), 0x73CDE098u, 0, raw), buf,
            "psp probe 1: titleid 0x73CDE098: outside the blocks\n");
    line_is(vjo_psp_line_sites(buf, sizeof(buf), "ULJM05889", 1, 1), buf, "psp probe 1: luna ULJM05889: 1 sites, 1 read\n");
    line_is(vjo_psp_line_sites(buf, sizeof(buf), "NPJH50619", 6, 6), buf, "psp probe 1: luna NPJH50619: 6 sites, 6 read\n");
    line_is(vjo_psp_line_sites(buf, sizeof(buf), "ULJM05334", 0, 0), buf, "psp probe 1: luna ULJM05334: not listed\n");
    line_is(vjo_psp_line_sites(buf, sizeof(buf), "", 0, 0), buf, "psp probe 1: luna : not listed\n");
    line_is(vjo_psp_line_sites(buf, sizeof(buf), "AB\ncd 0123456789", 0, 0), buf, "psp probe 1: luna AB.cd.012: not listed\n");
    line_is(vjo_psp_line_site(buf, sizeof(buf), 0x08868AB8u, 0x70868AB8u, words, 0xFu), buf,
            "psp probe 1: site 0x08868AB8 at 0x70868AB8: 27BDFFE0 AFBF001C 00000000 3C028890\n");
    line_is(vjo_psp_line_site(buf, sizeof(buf), 0x08868AB8u, 0x70868AB8u, words, 0x6u), buf,
            "psp probe 1: site 0x08868AB8 at 0x70868AB8: -------- AFBF001C 00000000 --------\n");
    line_is(vjo_psp_line_site(buf, sizeof(buf), 0x08868AB8u, 0x70868AB8u, NULL, 0xFu), buf,
            "psp probe 1: site 0x08868AB8 at 0x70868AB8: -------- -------- -------- --------\n");
    line_is(vjo_psp_line_end(buf, sizeof(buf), "a block is missing or not readable"), buf,
            "psp probe 1: stop: a block is missing or not readable\n");
    line_is(vjo_psp_line_end(buf, sizeof(buf), NULL), buf, "psp probe 1: done\n");
}

/* Each kind of line at its longest. */
static size_t longest(unsigned kind, char *out, size_t cap)
{
    static char reason[200];
    VjoPspBlock b = {0xFFFFFFFFu, -1, -1, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    uint8_t raw[12];
    uint32_t words[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    memset(raw, 'W', sizeof(raw)); /* no NUL: the printable part is all 12 bytes */
    memset(reason, 'r', sizeof(reason) - 1);
    switch (kind) {
    case 0: return vjo_psp_line_start(out, cap, 0xFFFFFFFFu);
    case 1: return vjo_psp_line_module(out, cap, -1, 0xFFFFFFFFu);
    case 2: return vjo_psp_line_block(out, cap, &b);
    case 3: return vjo_psp_line_titleid(out, cap, 0xFFFFFFFFu, 1, raw);
    case 4: return vjo_psp_line_sites(out, cap, "ZZZZ99999 and longer", 0xFFFFFFFFu, 0xFFFFFFFFu);
    case 5: return vjo_psp_line_site(out, cap, 0xFFFFFFFFu, 0xFFFFFFFFu, words, 0xFu);
    default: return vjo_psp_line_end(out, cap, reason);
    }
}

static void test_line_bounds(void)
{
    for (unsigned kind = 0; kind < 7; kind++) {
        char full[512];
        size_t n = longest(kind, full, sizeof(full));
        TEST_CHECK(n + 1u <= VJO_PSP_LINE_MAX && full[n - 1] == '\n' && strlen(full) == n);
        TEST_MSG("kind %u: %zu bytes: %s", kind, n, full);
        TEST_CHECK(longest(kind, NULL, 0) == 0);
        TEST_CHECK(longest(kind, NULL, 64) == 0); /* no buffer: nothing written */
        for (size_t cap = 0; cap <= n + 2u; cap++) {
            char *out = cap ? malloc(cap) : NULL; /* exactly cap bytes: ASan sees a byte past them */
            size_t got = longest(kind, out, cap);
            if (!cap) {
                TEST_CHECK(got == 0);
                continue;
            }
            TEST_CHECK(got == (n < cap ? n : cap - 1u) && out[got] == 0 && !memcmp(out, full, got));
            TEST_MSG("kind %u cap %zu: %zu", kind, cap, got);
            free(out);
        }
    }
}

TEST_LIST = {
    {"counts", test_counts},
    {"every_title", test_every_title},
    {"known_entries", test_known_entries},
    {"not_listed", test_not_listed},
    {"cap", test_cap},
    {"vita_addr", test_vita_addr},
    {"blocks", test_blocks},
    {"title_id", test_title_id},
    {"lines", test_lines},
    {"line_bounds", test_line_bounds},
    {NULL, NULL},
};
