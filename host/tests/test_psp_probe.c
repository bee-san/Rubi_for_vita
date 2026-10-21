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

/* Packed title ID k of the table -> its 9 characters and a NUL. */
static void unpack(unsigned k, char id[10])
{
    uint32_t v = vjo_luna_psp_ids[k], p = v >> VJO_LUNA_PSP_NUMBER_BITS, number = v & ((1u << VJO_LUNA_PSP_NUMBER_BITS) - 1u);
    TEST_ASSERT(p < VJO_LUNA_PSP_PREFIXES && number <= 99999u);
    memcpy(id, &vjo_luna_psp_prefix[p * 4u], 4);
    for (int d = 8; d >= 4; d--) { /* 5 digits, leading zeros kept */
        id[d] = (char)('0' + number % 10u);
        number /= 10u;
    }
    id[9] = 0;
}

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
    /* the prefixes the report counted (ULJM 235, NPJH 78, ULJS 39, UCJS 1, ULUS 1), as the packing names them */
    TEST_CHECK(VJO_LUNA_PSP_PREFIXES == 5u && strlen(vjo_luna_psp_prefix) == 20u);
    TEST_CHECK(!memcmp(&vjo_luna_psp_prefix[VJO_LUNA_PSP_NPJH * 4u], "NPJH", 4));
    TEST_CHECK(!memcmp(&vjo_luna_psp_prefix[VJO_LUNA_PSP_UCJS * 4u], "UCJS", 4));
    TEST_CHECK(!memcmp(&vjo_luna_psp_prefix[VJO_LUNA_PSP_ULJM * 4u], "ULJM", 4));
    TEST_CHECK(!memcmp(&vjo_luna_psp_prefix[VJO_LUNA_PSP_ULJS * 4u], "ULJS", 4));
    TEST_CHECK(!memcmp(&vjo_luna_psp_prefix[VJO_LUNA_PSP_ULUS * 4u], "ULUS", 4));
    {
        unsigned per[5] = {0};
        for (unsigned i = 0; i < VJO_LUNA_PSP_TITLES; i++) per[vjo_luna_psp_ids[i] >> VJO_LUNA_PSP_NUMBER_BITS]++;
        TEST_CHECK(per[VJO_LUNA_PSP_ULJM] == 235u && per[VJO_LUNA_PSP_NPJH] == 78u && per[VJO_LUNA_PSP_ULJS] == 39u &&
                   per[VJO_LUNA_PSP_UCJS] == 1u && per[VJO_LUNA_PSP_ULUS] == 1u);
    }
    TEST_CHECK(VJO_LUNA_PSP_ID(ULJM, 5889) == (2u << 17 | 5889u)); /* ULJM05889 */
}

/* Every title, through the probe's lookup, gives its sites from the table, in order. */
static void test_every_title(void)
{
    unsigned first = 0;
    for (unsigned i = 0; i < VJO_LUNA_PSP_TITLES; i++) {
        char id[10], before[10];
        uint32_t got[VJO_PSP_SITES_MAX];
        unsigned total = 99, n;
        unpack(i, id);
        if (i) {
            unpack(i - 1u, before);
            TEST_CHECK(vjo_luna_psp_ids[i - 1u] < vjo_luna_psp_ids[i] && strcmp(before, id) < 0); /* sorted, no repeat */
        }
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

/* FNV-1a over a NUL-terminated string, written apart from core/psp_probe.c's */
static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

/* A job's result as a PSP game's OCR job could have it. */
static VjoGameOcrResult *job_result(uint32_t seq)
{
    static VjoGameOcrResult r;
    memset(&r, 0, sizeof(r));
    r.size = sizeof(r);
    r.seq = seq;
    r.source = 1;
    r.capture_memory = 2;
    r.capture_bytes = 12288;
    r.fb_width = 960;
    r.fb_height = 544;
    r.fb_pitch = 960;
    r.width = 480;
    r.height = 136;
    r.region_y = 408;
    r.strip_rows = 16;
    r.captures = 9;
    r.capture_us = 150000;
    r.region_sum = 0x1234ABCDu;
    r.lines = 3;
    r.recognized = 3;
    r.frames = 205;
    r.find_us = 100000;
    r.recognize_us = 900000;
    r.free_user = 12345678;
    r.need_workspace = 4096000;
    r.reserve_bytes = 8388608;
    r.weights = 2;
    r.resident_bytes = 1310720;
    strcpy(r.text, "\xE7\x8C\xAB\xE3\x81\xA7\xE3\x81\x99"); /* 猫です: 3 characters, 9 bytes */
    return &r;
}

static void test_lines(void)
{
    char buf[VJO_PSP_LINE_MAX], want[VJO_PSP_LINE_MAX];
    uint32_t words[4] = {0x03E00008u, 0x00000000u, 0x27BDFFE0u, 0xAFBF001Cu}; /* jr ra; nop | addiu sp, sp, -32; sw ra */
    unsigned count[VJO_PSP_CLASSES] = {1, 2, 3, 4, 5, 6, 7};
    const VjoPspModule m = {0, 0x3F75D4D3u, 0x40010003, 0, 0x81000000u, 0x00200000u};
    VjoPspBlock b = block(0x70800000u, 0x70000000u, 0x04000000u, 6);
    VjoGameOcrResult *r = job_result(7);
    static const uint8_t raw[12] = {'U', 'L', 'J', 'M', '0', '5', '8', '8', '9', 0, 0, 0};
    static const uint8_t odd[12] = {'"', '\\', 0x01, 0x7F, 'A', 0xFF, 'z', ' ', 0, 'Q', 0, 0};
    line_is(vjo_psp_line_process(buf, sizeof(buf), "PSPEMUCFW"), buf, "psp probe 2: process PSPEMUCFW\n");
    line_is(vjo_psp_line_process(buf, sizeof(buf), "AB\ncd 0123456789"), buf, "psp probe 2: process AB.cd.012345\n");
    line_is(vjo_psp_line_process(buf, sizeof(buf), NULL), buf, "psp probe 2: process \n");
    line_is(vjo_psp_line_start(buf, sizeof(buf), 1, 7), buf, "psp probe 2: start: pass 1 after job 7\n");
    line_is(vjo_psp_line_module(buf, sizeof(buf), &m), buf,
            "psp probe 2: ScePspemu: rc 0x00000000 nid 0x3F75D4D3 modid 0x40010003 info 0x00000000 seg0 0x81000000 "
            "size 0x00200000\n");
    line_is(vjo_psp_line_module(buf, sizeof(buf), NULL), buf,
            "psp probe 2: ScePspemu: rc 0xFFFFFFFF nid 0x00000000 modid 0x00000000 info 0x00000000 seg0 0x00000000 "
            "size 0x00000000\n");
    line_is(vjo_psp_line_block(buf, sizeof(buf), &b), buf,
            "psp probe 2: block 0x70800000: uid 0x00001234 info 0x00000000 base 0x70000000 size 0x04000000 "
            "memtype 0x000000D0 (cached) access 0x00000006 type 0x0C20D060 ok\n");
    b.memtype = 0x80;
    line_is(vjo_psp_line_block(buf, sizeof(buf), &b), buf,
            "psp probe 2: block 0x70800000: uid 0x00001234 info 0x00000000 base 0x70000000 size 0x04000000 "
            "memtype 0x00000080 (uncached) access 0x00000006 type 0x0C20D060 ok\n");
    b.uid = -1;
    b.info_rc = (int32_t)0x80020001u;
    b.base = b.size = b.memtype = b.access = b.type = 0;
    line_is(vjo_psp_line_block(buf, sizeof(buf), &b), buf,
            "psp probe 2: block 0x70800000: uid 0xFFFFFFFF info 0x80020001 base 0x00000000 size 0x00000000 "
            "memtype 0x00000000 (?) access 0x00000000 type 0x00000000 bad\n");
    line_is(vjo_psp_line_titleid(buf, sizeof(buf), 0x73CDE098u, 1, raw), buf,
            "psp probe 2: titleid 0x73CDE098: 554C4A4D3035383839000000 \"ULJM05889\"\n");
    line_is(vjo_psp_line_titleid(buf, sizeof(buf), 0x73CDE098u, 1, odd), buf,
            "psp probe 2: titleid 0x73CDE098: 225C017F41FF7A2000510000 \"....A.z \"\n");
    line_is(vjo_psp_line_titleid(buf, sizeof(buf), 0x73CDE098u, 0, raw), buf,
            "psp probe 2: titleid 0x73CDE098: outside the blocks\n");
    {
        /* 12 bytes without a NUL, and printable bytes after them: only the 12 are read */
        static const uint8_t longer[16] = {'W', 'W', 'W', 'W', 'W', 'W', 'W', 'W', 'W', 'W', 'W', 'W', 'X', 'Y', 'Z', 0};
        line_is(vjo_psp_line_titleid(buf, sizeof(buf), 0x73CDE098u, 1, longer), buf,
                "psp probe 2: titleid 0x73CDE098: 575757575757575757575757 \"WWWWWWWWWWWW\"\n");
    }
    line_is(vjo_psp_line_sites(buf, sizeof(buf), "ULJM05889", 1, 1), buf, "psp probe 2: luna ULJM05889: 1 sites, 1 read\n");
    line_is(vjo_psp_line_sites(buf, sizeof(buf), "ULJM05334", 0, 0), buf, "psp probe 2: luna ULJM05334: not listed\n");
    line_is(vjo_psp_line_sites(buf, sizeof(buf), "", 0, 0), buf, "psp probe 2: luna : not listed\n");
    line_is(vjo_psp_line_sites(buf, sizeof(buf), "AB\ncd 0123456789", 0, 0), buf, "psp probe 2: luna AB.cd.012: not listed\n");
    line_is(vjo_psp_line_site(buf, sizeof(buf), 0x08868AB8u, 0x70868AB8u, words, 0xFu), buf,
            "psp probe 2: site 0x08868AB8 at 0x70868AB8: 03E00008 00000000 27BDFFE0 AFBF001C entry\n");
    line_is(vjo_psp_line_site(buf, sizeof(buf), 0x08868AB8u, 0x70868AB8u, words, 0x6u), buf,
            "psp probe 2: site 0x08868AB8 at 0x70868AB8: -------- 00000000 27BDFFE0 -------- unread\n");
    line_is(vjo_psp_line_site(buf, sizeof(buf), 0x08868AB8u, 0x70868AB8u, NULL, 0xFu), buf,
            "psp probe 2: site 0x08868AB8 at 0x70868AB8: -------- -------- -------- -------- unread\n");
    line_is(vjo_psp_line_classes(buf, sizeof(buf), count), buf,
            "psp probe 2: classes: entry 5 return 6 branch 4 delay 3 other 7 zero 2 unread 1\n");
    line_is(vjo_psp_line_classes(buf, sizeof(buf), NULL), buf,
            "psp probe 2: classes: entry 0 return 0 branch 0 delay 0 other 0 zero 0 unread 0\n");
    line_is(vjo_psp_line_read_check(buf, sizeof(buf), 0x70800000u, 0x100000u, 0, 256, 0, 0, 0x10001, 1234), buf,
            "psp probe 2: read 0x70800000: 0x00100000 bytes, 256 pages in block 0x00010001 (1234 us): reading\n");
    line_is(vjo_psp_line_read_check(buf, sizeof(buf), 0x70800000u, 0x100000u, 0, 128, 0x70880000u, 0x20002, 0x10001, 9),
            buf, "psp probe 2: read 0x70800000: 0x00100000 bytes: page 0x70880000 in block 0x00020002, not 0x00010001: "
                 "not read\n");
    line_is(vjo_psp_line_read_check(buf, sizeof(buf), 0x70800000u, 0x100000u, 1, 0, 0, 0, 0, 0), buf,
            "psp probe 2: read 0x70800000: 0x00100000 bytes: outside the blocks\n");
    line_is(vjo_psp_line_read_done(buf, sizeof(buf), 0x70800000u, 0x100000u, 5678, 0xDEADBEEFu), buf,
            "psp probe 2: read 0x70800000: 0x00100000 bytes in 5678 us, sum 0xDEADBEEF\n");
    line_is(vjo_psp_line_end(buf, sizeof(buf), "a block is missing or not readable"), buf,
            "psp probe 2: stop: a block is missing or not readable\n");
    line_is(vjo_psp_line_end(buf, sizeof(buf), NULL), buf, "psp probe 2: done\n");
    {
        /* a reason is cut at 96 characters */
        char why[200];
        memset(why, 'r', sizeof(why) - 1);
        why[sizeof(why) - 1] = 0;
        TEST_CHECK(vjo_psp_line_end(buf, sizeof(buf), why) == strlen("psp probe 2: stop: ") + 96u + 1u);
    }
    {
        /* the text's characters and hash stop at the end of its buffer, whatever follows it */
        static struct { VjoGameOcrResult r; char tail[16]; } big;
        char want_chars[64];
        memcpy(&big.r, r, sizeof(*r));
        memset(big.r.text, 'x', sizeof(big.r.text)); /* no NUL in it */
        memset(big.tail, 'y', sizeof(big.tail));
        vjo_psp_line_job_ocr(buf, sizeof(buf), &big.r, 0);
        snprintf(want_chars, sizeof(want_chars), ", chars %u hash ", (unsigned)sizeof(big.r.text));
        TEST_CHECK(strstr(buf, want_chars) != NULL);
        TEST_MSG("%s", buf);
    }
    line_is(vjo_psp_line_job_capture(buf, sizeof(buf), r, 0x10, 0x86), buf,
            "psp probe 2: job 7: rc 0, flags 0x00000010 speed 0x00000086, capture src 1 mem 2 block 12288, fb 960x544 "
            "pitch 960 fmt 0x00000000, region 480x136 at 0,408, rows 16 strips 9 tears 0 in 150000 us, sum 0x1234ABCD\n");
    snprintf(want, sizeof(want),
             "psp probe 2: job 7 ocr: lines 3 rec 3 frames 205, us find 100000 load 0 rec 900000 job 1200000, free "
             "12345678 need 4096000+0 reserve 8388608 fallback 0, weights resident resident 1310720, from 0, chars 3 "
             "hash 0x%08X\n", (unsigned)fnv1a(r->text));
    line_is(vjo_psp_line_job_ocr(buf, sizeof(buf), r, 1200000), buf, want);
    r->rc = -2147352567;
    r->free_user = -1;
    r->weights = 7;
    r->text[0] = 0;
    line_is(vjo_psp_line_job_capture(buf, sizeof(buf), r, 0, 0x80), buf,
            "psp probe 2: job 7: rc -2147352567, flags 0x00000000 speed 0x00000080, capture src 1 mem 2 block 12288, fb "
            "960x544 pitch 960 fmt 0x00000000, region 480x136 at 0,408, rows 16 strips 9 tears 0 in 150000 us, sum "
            "0x1234ABCD\n");
    line_is(vjo_psp_line_job_ocr(buf, sizeof(buf), r, 0), buf,
            "psp probe 2: job 7 ocr: lines 3 rec 3 frames 205, us find 100000 load 0 rec 900000 job 0, free -1 need "
            "4096000+0 reserve 8388608 fallback 0, weights ? resident 1310720, from 0, chars 0 hash 0x811C9DC5\n");
    r->weights = 3; /* one past the names: "?" too */
    TEST_CHECK(vjo_psp_line_job_ocr(buf, sizeof(buf), r, 0) && strstr(buf, ", weights ? resident ") != NULL);
    line_is(vjo_psp_line_job_capture(buf, sizeof(buf), NULL, 0, 0), buf, "psp probe 2: job: no result\n");
    line_is(vjo_psp_line_job_ocr(buf, sizeof(buf), NULL, 0), buf, "psp probe 2: job: no result\n");
    line_is(vjo_psp_line_jobs_cap(buf, sizeof(buf), 1000), buf,
            "psp probe 2: jobs: 1000, the most per process: no more job lines\n");
}

/* Each kind of line at its longest. */
#define KINDS 15u
static size_t longest(unsigned kind, char *out, size_t cap)
{
    static char reason[200];
    static VjoGameOcrResult r;
    static const VjoPspModule m = {-2147483647 - 1, 0xFFFFFFFFu, -2147483647 - 1, -2147483647 - 1, 0xFFFFFFFFu,
                                   0xFFFFFFFFu};
    VjoPspBlock b = {0xFFFFFFFFu, -1, -1, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    uint8_t raw[12];
    uint32_t words[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    unsigned count[VJO_PSP_CLASSES];
    memset(raw, 'W', sizeof(raw)); /* no NUL: the printable part is all 12 bytes */
    memset(reason, 'r', sizeof(reason) - 1);
    for (unsigned i = 0; i < VJO_PSP_CLASSES; i++) count[i] = 0xFFFFFFFFu;
    memset(&r, 0xFF, sizeof(r)); /* every number at its widest, */
    r.rc = r.free_user = -2147483647 - 1;
    r.weights = 2;                                /* the longest weights name ("resident"), */
    memset(r.text, 'x', sizeof(r.text));          /* and text up to the end of the buffer, no NUL */
    switch (kind) {
    case 0: return vjo_psp_line_process(out, cap, "WWWWWWWWWWWWWWWWWWWW");
    case 1: return vjo_psp_line_start(out, cap, 0xFFFFFFFFu, 0xFFFFFFFFu);
    case 2: return vjo_psp_line_module(out, cap, &m);
    case 3: return vjo_psp_line_block(out, cap, &b);
    case 4: return vjo_psp_line_titleid(out, cap, 0xFFFFFFFFu, 1, raw);
    case 5: return vjo_psp_line_sites(out, cap, "ZZZZ99999 and longer", 0xFFFFFFFFu, 0xFFFFFFFFu);
    case 6: return vjo_psp_line_site(out, cap, 0xFFFFFFFFu, 0xFFFFFFFFu, words, 0xFu);
    case 7: return vjo_psp_line_classes(out, cap, count);
    case 8: return vjo_psp_line_read_check(out, cap, 0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0xFFFFFFFFu, 0, 0, -1, 0xFFFFFFFFu);
    case 9: return vjo_psp_line_read_check(out, cap, 0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0, 0xFFFFFFFFu, -1, -1, 0);
    case 10: return vjo_psp_line_read_done(out, cap, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu);
    case 11: return vjo_psp_line_end(out, cap, reason);
    case 12: return vjo_psp_line_job_capture(out, cap, &r, 0xFFFFFFFFu, 0xFFFFFFFFu);
    case 13: return vjo_psp_line_job_ocr(out, cap, &r, 0xFFFFFFFFu);
    default: return vjo_psp_line_jobs_cap(out, cap, 0xFFFFFFFFu);
    }
}

static void test_line_bounds(void)
{
    for (unsigned kind = 0; kind < KINDS; kind++) {
        char full[1024];
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

static void test_holder(void)
{
    VjoPspBlock b[3];
    b[0] = block(0x70000000u, 0x70000000u, 0x00800000u, 4); /* 0x70000000-0x707FFFFF */
    b[1] = block(0x70800000u, 0x70800000u, 0x03800000u, 4); /* 0x70800000-0x73FFFFFF */
    b[2] = block(0x73CDE000u, 0x73CDE000u, 0x1000u, 4);
    TEST_CHECK(vjo_psp_holder(b, 3, 0x70000000u, 4) == 0);
    TEST_CHECK(vjo_psp_holder(b, 3, 0x707FFFFCu, 4) == 0);
    TEST_CHECK(vjo_psp_holder(b, 3, 0x707FFFFEu, 4) == -1); /* across two blocks: neither holds it all */
    TEST_CHECK(vjo_psp_holder(b, 3, 0x70800000u, VJO_PSP_READ_BYTES) == 1);
    TEST_CHECK(vjo_psp_holder(b, 3, 0x73CDE098u, VJO_PSP_TITLEID_BYTES) == 1); /* the first that holds it */
    TEST_CHECK(vjo_psp_holder(b + 2, 1, 0x73CDE098u, VJO_PSP_TITLEID_BYTES) == 0);
    TEST_CHECK(vjo_psp_holder(b, 3, 0x74000000u, 4) == -1);
    b[1].access = 2; /* not readable: never a holder */
    TEST_CHECK(vjo_psp_holder(b, 3, 0x70800000u, 4) == -1 && vjo_psp_holder(b, 3, 0x73CDE098u, 4) == 2);
    TEST_CHECK(vjo_psp_holder(NULL, 3, 0x70000000u, 4) == -1 && vjo_psp_holder(b, 3, 0x70000000u, 0) == -1);
}

/* ---- the MIPS classifier, on synthetic code ---- */

enum { ZERO = 0, V0 = 2, A0 = 4, T0 = 8, S0 = 16, SP = 29, RA = 31 };
#define NOP 0x00000000u
#define J(t) (0x08000000u | ((t) >> 2 & 0x03FFFFFFu))
#define JAL(t) (0x0C000000u | ((t) >> 2 & 0x03FFFFFFu))
#define SPECIAL(rs, rt, rd, funct) ((uint32_t)(rs) << 21 | (uint32_t)(rt) << 16 | (uint32_t)(rd) << 11 | (funct))
#define JR(rs) SPECIAL(rs, 0, 0, 0x08u)
#define JALR(rd, rs) SPECIAL(rs, 0, rd, 0x09u)
#define ADDU(rd, rs, rt) SPECIAL(rs, rt, rd, 0x21u)
#define I(op, rs, rt, imm) ((uint32_t)(op) << 26 | (uint32_t)(rs) << 21 | (uint32_t)(rt) << 16 | ((uint32_t)(imm) & 0xFFFFu))
#define REGIMM(rs, rt, off) I(0x01, rs, rt, off)
#define ADDIU(rt, rs, imm) I(0x09, rs, rt, imm)
#define LW(rt, off, base) I(0x23, base, rt, off)
#define SW(rt, off, base) I(0x2B, base, rt, off)
#define LUI(rt, imm) I(0x0F, 0, rt, imm)
#define COP(z, rs, rt, rest) (0x40000000u | (uint32_t)(z) << 26 | (uint32_t)(rs) << 21 | (uint32_t)(rt) << 16 | (rest))

static void test_mips(void)
{
    /* every instruction with a delay slot (MIPS32 and Allegrex's VFPU branches, COP2's BC form) */
    const uint32_t cti[] = {
        J(0x08804100u), JAL(0x08804100u), JR(RA), JR(T0), JALR(RA, T0),
        I(0x04, A0, ZERO, 4), I(0x05, A0, V0, -4), I(0x06, A0, 0, 2), I(0x07, A0, 0, 2),  /* beq bne blez bgtz */
        I(0x14, A0, ZERO, 4), I(0x15, A0, V0, -4), I(0x16, A0, 0, 2), I(0x17, A0, 0, 2),  /* their likely forms */
        REGIMM(A0, 0x00, 3), REGIMM(A0, 0x01, 3), REGIMM(A0, 0x02, 3), REGIMM(A0, 0x03, 3), /* bltz bgez (l) */
        REGIMM(A0, 0x10, 3), REGIMM(A0, 0x11, 3), REGIMM(A0, 0x12, 3), REGIMM(A0, 0x13, 3), /* ...al, ...all */
        COP(1, 0x08, 0, 5), COP(1, 0x08, 1, 5), COP(1, 0x08, 2, 5), COP(1, 0x08, 3, 5),     /* bc1f bc1t (l) */
        COP(2, 0x08, 0, 5), COP(2, 0x08, 1, 5), COP(2, 0x08, 3, 5),                          /* bvf bvt bvtl */
        COP(0, 0x08, 0, 5), /* COP0 rs 8: MIPS I's bc0f (mftc0 on MIPS32 MT, invalid on Allegrex): a branch, to be safe */
    };
    /* and what is not: arithmetic, loads, stores, traps, syscall, eret, coprocessor moves and arithmetic */
    const uint32_t plain[] = {
        NOP, ADDIU(SP, SP, -32), LW(RA, 28, SP), SW(RA, 28, SP), LUI(V0, 0x0889), ADDU(A0, V0, ZERO),
        SPECIAL(A0, V0, 0, 0x34u) /* teq */, REGIMM(A0, 0x08, 3) /* tgei */, REGIMM(A0, 0x0C, 3) /* teqi */,
        REGIMM(A0, 0x04, 3) /* reserved */, SPECIAL(A0, V0, V0, 0x0Au) /* movz */,
        0x0000000Cu /* syscall */, 0x0000000Du /* break */, 0x42000018u /* eret */, COP(0, 0x00, V0, 12 << 11) /* mfc0 */,
        COP(1, 0x10, 0, 0) /* add.s */, COP(2, 0x00, V0, 0) /* COP2, not BC */, I(0x30, SP, V0, 0) /* ll */,
        I(0x38, SP, V0, 0) /* sc */, I(0x31, SP, 0, 0) /* lwc1 */, I(0x39, SP, 0, 0) /* swc1 */,
        I(0x08, SP, SP, -32) /* addi */,
    };
    for (unsigned i = 0; i < sizeof(cti) / sizeof(cti[0]); i++) {
        TEST_CHECK(vjo_mips_is_cti(cti[i]) && vjo_mips_unsafe(cti[i]) && !vjo_mips_is_frame(cti[i]));
        TEST_MSG("cti %u: %08X", i, (unsigned)cti[i]);
    }
    for (unsigned i = 0; i < sizeof(plain) / sizeof(plain[0]); i++) {
        TEST_CHECK(!vjo_mips_is_cti(plain[i]) && !vjo_mips_is_call(plain[i]));
        TEST_MSG("plain %u: %08X", i, (unsigned)plain[i]);
    }
    /* calls: what returns to its own address + 8 */
    TEST_CHECK(vjo_mips_is_call(JAL(0x08804100u)) && vjo_mips_is_call(JALR(RA, T0)) && vjo_mips_is_call(REGIMM(A0, 0x10, 3)) &&
               vjo_mips_is_call(REGIMM(A0, 0x11, 3)) && vjo_mips_is_call(REGIMM(A0, 0x12, 3)) &&
               vjo_mips_is_call(REGIMM(A0, 0x13, 3)));
    TEST_CHECK(!vjo_mips_is_call(J(0x08804100u)) && !vjo_mips_is_call(JR(RA)) && !vjo_mips_is_call(I(0x04, A0, 0, 4)) &&
               !vjo_mips_is_call(REGIMM(A0, 0x00, 3)) && !vjo_mips_is_call(REGIMM(A0, 0x03, 3)) &&
               !vjo_mips_is_call(COP(1, 0x08, 1, 5)));
    /* what a two-word patch must not overwrite, besides the branches: syscall, break, COP0, ll, sc */
    TEST_CHECK(vjo_mips_unsafe(0x0000000Cu) && vjo_mips_unsafe(0x0001F40Cu) && vjo_mips_unsafe(0x0000000Du) &&
               vjo_mips_unsafe(0x42000018u) && vjo_mips_unsafe(COP(0, 0x00, V0, 12 << 11)) &&
               vjo_mips_unsafe(COP(0, 0x04, V0, 12 << 11)) && vjo_mips_unsafe(I(0x30, SP, V0, 0)) &&
               vjo_mips_unsafe(I(0x38, SP, V0, 0)));
    TEST_CHECK(!vjo_mips_unsafe(NOP) && !vjo_mips_unsafe(ADDIU(SP, SP, -32)) && !vjo_mips_unsafe(SW(RA, 28, SP)) &&
               !vjo_mips_unsafe(LW(RA, 28, SP)) && !vjo_mips_unsafe(LUI(V0, 1)) && !vjo_mips_unsafe(I(0x31, SP, 0, 0)) &&
               !vjo_mips_unsafe(I(0x39, SP, 0, 0)) && !vjo_mips_unsafe(SPECIAL(A0, V0, 0, 0x34u)) &&
               !vjo_mips_unsafe(COP(1, 0x10, 0, 0)) && !vjo_mips_unsafe(COP(2, 0x00, V0, 0)));
    /* a frame: addiu sp, sp, -N with N > 0, nothing else */
    TEST_CHECK(vjo_mips_is_frame(0x27BDFFE0u) && vjo_mips_is_frame(ADDIU(SP, SP, -1)) &&
               vjo_mips_is_frame(ADDIU(SP, SP, -32768)));
    TEST_CHECK(!vjo_mips_is_frame(ADDIU(SP, SP, 32)) && !vjo_mips_is_frame(ADDIU(SP, SP, 0)) &&
               !vjo_mips_is_frame(ADDIU(V0, SP, -32)) && !vjo_mips_is_frame(ADDIU(SP, V0, -32)) &&
               !vjo_mips_is_frame(I(0x08, SP, SP, -32)) && !vjo_mips_is_frame(SW(RA, -4, SP)));
}

static unsigned classify4(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3)
{
    const uint32_t w[4] = {w0, w1, w2, w3};
    return vjo_psp_classify(w, 0xFu);
}

static void test_classify(void)
{
    const uint32_t entry[4] = {JR(RA), NOP, ADDIU(SP, SP, -32), SW(RA, 28, SP)};
    /* a function's first instruction, after the one before it returned */
    TEST_CHECK(classify4(JR(RA), NOP, ADDIU(SP, SP, -32), SW(RA, 28, SP)) == VJO_PSP_CLASS_ENTRY);
    TEST_CHECK(classify4(LW(RA, 28, SP), ADDIU(SP, SP, 32), ADDIU(SP, SP, -48), SW(S0, 16, SP)) == VJO_PSP_CLASS_ENTRY);
    /* where a call returns: jal or jalr two words before the target, its delay slot between */
    TEST_CHECK(classify4(JAL(0x08804100u), NOP, ADDU(A0, V0, ZERO), LW(RA, 28, SP)) == VJO_PSP_CLASS_RETURN);
    TEST_CHECK(classify4(JALR(RA, T0), ADDU(A0, S0, ZERO), LW(V0, 0, S0), NOP) == VJO_PSP_CLASS_RETURN);
    TEST_CHECK(classify4(REGIMM(A0, 0x11, 3), NOP, LW(V0, 0, S0), NOP) == VJO_PSP_CLASS_RETURN); /* bgezal */
    /* a frame after a call is an entry: the frame check comes first */
    TEST_CHECK(classify4(JAL(0x08804100u), NOP, ADDIU(SP, SP, -16), SW(RA, 12, SP)) == VJO_PSP_CLASS_ENTRY);
    /* the target in a branch's delay slot: whatever follows */
    TEST_CHECK(classify4(LW(V0, 0, S0), I(0x04, V0, ZERO, 8), ADDIU(SP, SP, -32), SW(RA, 28, SP)) == VJO_PSP_CLASS_DELAY);
    TEST_CHECK(classify4(LW(V0, 0, S0), JR(RA), NOP, ADDIU(SP, SP, -32)) == VJO_PSP_CLASS_DELAY);
    TEST_CHECK(classify4(JAL(0x08804100u), JAL(0x08804200u), NOP, NOP) == VJO_PSP_CLASS_DELAY); /* before return */
    TEST_CHECK(classify4(NOP, COP(2, 0x08, 1, 5), NOP, NOP) == VJO_PSP_CLASS_DELAY);                /* bvt */
    /* a delay slot whose own words are unsafe too: delay, the check before branch */
    TEST_CHECK(classify4(NOP, JR(RA), JAL(0x08804100u), NOP) == VJO_PSP_CLASS_DELAY);
    TEST_CHECK(classify4(NOP, I(0x05, V0, ZERO, 4), NOP, 0x0000000Cu) == VJO_PSP_CLASS_DELAY);
    /* a patch would overwrite a branch, syscall, eret, ll or sc (the target's word or the next one) */
    TEST_CHECK(classify4(NOP, NOP, JR(RA), NOP) == VJO_PSP_CLASS_BRANCH);
    TEST_CHECK(classify4(NOP, NOP, ADDIU(SP, SP, -32), JAL(0x08804100u)) == VJO_PSP_CLASS_BRANCH); /* before entry */
    TEST_CHECK(classify4(JAL(0x08804100u), NOP, NOP, I(0x05, V0, ZERO, 4)) == VJO_PSP_CLASS_BRANCH); /* before return */
    TEST_CHECK(classify4(NOP, NOP, 0x0000000Cu, NOP) == VJO_PSP_CLASS_BRANCH);
    TEST_CHECK(classify4(NOP, NOP, NOP, 0x42000018u) == VJO_PSP_CLASS_BRANCH);
    TEST_CHECK(classify4(NOP, NOP, I(0x30, SP, V0, 0), NOP) == VJO_PSP_CLASS_BRANCH);
    TEST_CHECK(classify4(NOP, NOP, NOP, I(0x38, SP, V0, 0)) == VJO_PSP_CLASS_BRANCH);
    /* a block start mid-function that passes the checks: neither */
    TEST_CHECK(classify4(LUI(V0, 0x0889), ADDIU(V0, V0, 0x1234), LW(A0, 0, V0), SW(A0, 4, SP)) == VJO_PSP_CLASS_OTHER);
    TEST_CHECK(classify4(J(0x08804100u), NOP, NOP, NOP) == VJO_PSP_CLASS_OTHER); /* j two words back is no call */
    /* nothing there, or not all of it read */
    TEST_CHECK(classify4(0, 0, 0, 0) == VJO_PSP_CLASS_ZERO);
    TEST_CHECK(classify4(0, 0, 0, 1) == VJO_PSP_CLASS_OTHER);
    for (unsigned mask = 0; mask < 15u; mask++) {
        static const uint32_t zeros[4] = {0, 0, 0, 0};
        TEST_CHECK(vjo_psp_classify(entry, mask) == VJO_PSP_CLASS_UNREAD);
        TEST_CHECK(vjo_psp_classify(zeros, mask) == VJO_PSP_CLASS_UNREAD); /* unread before zero */
        TEST_MSG("mask %u", mask);
    }
    TEST_CHECK(vjo_psp_classify(entry, 0xFFu) == VJO_PSP_CLASS_ENTRY); /* bits past the four: ignored */
    TEST_CHECK(vjo_psp_classify(NULL, 0xFu) == VJO_PSP_CLASS_UNREAD);
    {
        static const char *const names[] = {"unread", "zero", "delay", "branch", "entry", "return", "other"};
        TEST_CHECK(sizeof(names) / sizeof(names[0]) == VJO_PSP_CLASSES);
        for (unsigned c = 0; c < VJO_PSP_CLASSES; c++) TEST_CHECK(!strcmp(vjo_psp_class_name(c), names[c]));
        TEST_CHECK(!strcmp(vjo_psp_class_name(VJO_PSP_CLASSES), "?") && !strcmp(vjo_psp_class_name(~0u), "?"));
    }
}

/* ---- the runner, on a fake RAM image ----
 * The image backs two windows of Vita addresses: 2 MiB of the user partition at 0x70800000 (the timed read's
 * 1 MiB and every site this test uses) and SceAdrenaline's page at 0x73CDE000. The fake's lookups answer from
 * its blocks, which may claim more than the image (as one 64 MiB view would): a read the runner makes outside
 * the image, or outside every readable block, is counted in bad_reads, which every case checks is 0. */

#define USER_BASE 0x70800000u
#define USER_BYTES (2u << 20)
#define ADR_BASE 0x73CDE000u
#define ADR_BYTES 0x1000u
#define NOT_FOUND ((int32_t)0x80020006u) /* a negative error, as a failed lookup gives */
#define VIEW_UID 0x10001

static uint8_t user_ram[USER_BYTES], adr_ram[ADR_BYTES];
static char fake_log[1u << 20];

typedef struct {
    VjoPspBlock blk[3];
    unsigned n_blk;
    uint32_t odd_page; /* find(odd_page) answers odd_uid (0: none) */
    int32_t odd_uid;
    VjoPspModule module;
    unsigned block_calls, find_calls, reads, bad_reads, sum_calls, lines;
    unsigned read_after[64], n_read_after; /* the lines out before each read */
    uint64_t t;
    size_t log_n;
} Fake;
static Fake F;

static uint8_t *image(uint32_t addr, uint32_t n)
{
    if (addr - USER_BASE < USER_BYTES && n <= USER_BYTES - (addr - USER_BASE)) return user_ram + (addr - USER_BASE);
    if (addr - ADR_BASE < ADR_BYTES && n <= ADR_BYTES - (addr - ADR_BASE)) return adr_ram + (addr - ADR_BASE);
    return NULL;
}

/* The fake's own check, apart from core/psp_probe.c's: a readable block of the fake holds the range, and so does
 * the image. */
static uint8_t *checked(uint32_t addr, uint32_t n)
{
    int held = 0;
    for (unsigned i = 0; i < F.n_blk; i++) {
        const VjoPspBlock *b = &F.blk[i];
        if ((b->access & 4u) && addr >= b->base && (uint64_t)addr + n <= (uint64_t)b->base + b->size) held = 1;
    }
    F.reads++;
    if (F.n_read_after < 64) F.read_after[F.n_read_after++] = F.lines;
    if (!held || !image(addr, n)) {
        F.bad_reads++;
        return NULL;
    }
    return image(addr, n);
}

static const VjoPspBlock *fake_holder(uint32_t addr)
{
    for (unsigned i = 0; i < F.n_blk; i++)
        if (addr - F.blk[i].base < F.blk[i].size) return &F.blk[i];
    return NULL;
}

static void f_module(void *ud, VjoPspModule *m) { (void)ud; *m = F.module; }

static void f_block(void *ud, uint32_t addr, VjoPspBlock *out)
{
    const VjoPspBlock *b = fake_holder(addr);
    (void)ud;
    F.block_calls++;
    if (b) {
        *out = *b;
    } else {
        VjoPspBlock none = {0, NOT_FOUND, NOT_FOUND, 0, 0, 0, 0, 0};
        *out = none;
    }
    out->addr = addr;
}

static int32_t f_find(void *ud, uint32_t addr)
{
    const VjoPspBlock *b = fake_holder(addr);
    (void)ud;
    F.find_calls++;
    if (F.odd_uid && addr == F.odd_page) return F.odd_uid;
    return b ? b->uid : NOT_FOUND;
}

static uint32_t f_word(void *ud, uint32_t addr)
{
    const uint8_t *p = checked(addr, 4);
    (void)ud;
    return p ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24 : 0u;
}

static void f_bytes(void *ud, uint32_t addr, uint8_t *out, uint32_t n)
{
    const uint8_t *p = checked(addr, n);
    (void)ud;
    if (p) memcpy(out, p, n);
    else memset(out, 0, n);
}

static uint32_t sum_words(const uint8_t *p, uint32_t n)
{
    uint32_t s = 0;
    for (uint32_t i = 0; i + 4u <= n; i += 4u)
        s += (uint32_t)p[i] | (uint32_t)p[i + 1u] << 8 | (uint32_t)p[i + 2u] << 16 | (uint32_t)p[i + 3u] << 24;
    return s;
}

static uint32_t f_sum(void *ud, uint32_t addr, uint32_t n)
{
    const uint8_t *p = checked(addr, n);
    (void)ud;
    F.sum_calls++;
    return p ? sum_words(p, n) : 0u;
}

static uint64_t f_now(void *ud) { (void)ud; return F.t += 7u; }

static void f_put(void *ud, const char *line, size_t n)
{
    (void)ud;
    TEST_CHECK(n > 0 && line[n - 1] == '\n' && strlen(line) == n); /* one whole line, NUL after it */
    if (F.log_n + n < sizeof(fake_log)) {
        memcpy(fake_log + F.log_n, line, n);
        F.log_n += n;
        fake_log[F.log_n] = 0;
    }
    F.lines++;
}

static const VjoPspOps fake_ops = {NULL, f_module, f_block, f_find, f_word, f_bytes, f_sum, f_now, f_put};

static VjoPspBlock view_block(uint32_t base, uint32_t size, int32_t uid)
{
    VjoPspBlock b = {0, uid, 0, base, size, 0xD0, 6, 0x0C20D060};
    return b;
}

/* One block for the whole 64 MiB view, ScePspemu of 3.65-3.70, all RAM 0 and no title. */
static void fake_reset(void)
{
    const VjoPspModule m = {0, 0x3F75D4D3u, 0x40010003, 0, 0x81000000u, 0x00200000u};
    memset(&F, 0, sizeof(F));
    memset(user_ram, 0, sizeof(user_ram));
    memset(adr_ram, 0, sizeof(adr_ram));
    fake_log[0] = 0;
    F.blk[0] = view_block(0x70000000u, 0x04000000u, VIEW_UID);
    F.n_blk = 1;
    F.module = m;
}

static void set_title(const char *id)
{
    memset(adr_ram + VJO_PSP_TITLEID_OFFSET, 0, VJO_PSP_TITLEID_BYTES);
    memcpy(adr_ram + VJO_PSP_TITLEID_OFFSET, id, strlen(id));
}

static void put_word(uint32_t vita, uint32_t w)
{
    uint8_t *p = image(vita, 4);
    TEST_ASSERT(p != NULL);
    p[0] = (uint8_t)w;
    p[1] = (uint8_t)(w >> 8);
    p[2] = (uint8_t)(w >> 16);
    p[3] = (uint8_t)(w >> 24);
}

/* The four words around the Vita address of Luna's site psp. */
static void put_site(uint32_t psp, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3)
{
    uint32_t v = vjo_psp_vita_addr(psp);
    put_word(v - 8u, w0);
    put_word(v - 4u, w1);
    put_word(v, w2);
    put_word(v + 4u, w3);
}

/* Line k of the fake's log, without its newline ("" past the end). */
static const char *log_line(unsigned k)
{
    static char out[VJO_PSP_LINE_MAX];
    const char *p = fake_log;
    for (; k && *p; k--) {
        const char *e = strchr(p, '\n');
        p = e ? e + 1 : p + strlen(p);
    }
    {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n >= sizeof(out)) n = sizeof(out) - 1u;
        memcpy(out, p, n);
        out[n] = 0;
    }
    return out;
}

static int starts(const char *s, const char *prefix)
{
    return !strncmp(s, prefix, strlen(prefix));
}

static unsigned log_count(const char *needle)
{
    unsigned n = 0;
    for (const char *p = fake_log; (p = strstr(p, needle)) != NULL; p += strlen(needle)) n++;
    return n;
}

static void lines_are(unsigned first, const char *const *want, unsigned n)
{
    for (unsigned k = 0; k < n; k++) {
        TEST_CHECK(!strcmp(log_line(first + k), want[k]));
        TEST_MSG("line %u\ngot  [%s]\nwant [%s]", first + k, log_line(first + k), want[k]);
    }
}

static const char *const blocks_ok[3] = {
    "psp probe 2: block 0x70000000: uid 0x00010001 info 0x00000000 base 0x70000000 size 0x04000000 memtype "
    "0x000000D0 (cached) access 0x00000006 type 0x0C20D060 ok",
    "psp probe 2: block 0x70800000: uid 0x00010001 info 0x00000000 base 0x70000000 size 0x04000000 memtype "
    "0x000000D0 (cached) access 0x00000006 type 0x0C20D060 ok",
    "psp probe 2: block 0x73CDE000: uid 0x00010001 info 0x00000000 base 0x70000000 size 0x04000000 memtype "
    "0x000000D0 (cached) access 0x00000006 type 0x0C20D060 ok",
};
#define MODULE_LINE                                                                                                \
    "psp probe 2: ScePspemu: rc 0x00000000 nid 0x3F75D4D3 modid 0x40010003 info 0x00000000 seg0 0x81000000 size "  \
    "0x00200000"

/* The probe's first console title: ましろ色シンフォニー, one site, a function's entry there; the 1 MiB read. */
static void test_run_listed(void)
{
    VjoPspProbe p;
    char want_sum[VJO_PSP_LINE_MAX], job[2][VJO_PSP_LINE_MAX];
    fake_reset();
    memset(&p, 0, sizeof(p));
    set_title("ULJM05889");
    for (uint32_t i = 0; i < VJO_PSP_READ_BYTES / 4u; i++) put_word(USER_BASE + 4u * i, i * 2654435761u);
    put_site(0x08868AB8u, JR(RA), NOP, ADDIU(SP, SP, -32), SW(RA, 28, SP));
    snprintf(want_sum, sizeof(want_sum), "psp probe 2: read 0x70800000: 0x00100000 bytes in 7 us, sum 0x%08X",
             (unsigned)sum_words(user_ram, VJO_PSP_READ_BYTES));
    vjo_psp_probe_job(&p, &fake_ops, job_result(7), 0x10, 0x86, 1200000);
    vjo_psp_line_job_capture(job[0], sizeof(job[0]), job_result(7), 0x10, 0x86);
    vjo_psp_line_job_ocr(job[1], sizeof(job[1]), job_result(7), 1200000);
    job[0][strlen(job[0]) - 1] = job[1][strlen(job[1]) - 1] = 0;
    {
        const char *const want[] = {
            "psp probe 2: start: pass 1 after job 7",
            MODULE_LINE,
            blocks_ok[0], blocks_ok[1], blocks_ok[2],
            "psp probe 2: titleid 0x73CDE098: 554C4A4D3035383839000000 \"ULJM05889\"",
            "psp probe 2: luna ULJM05889: 1 sites, 1 read",
            "psp probe 2: site 0x08868AB8 at 0x70868AB8: 03E00008 00000000 27BDFFE0 AFBF001C entry",
            "psp probe 2: classes: entry 1 return 0 branch 0 delay 0 other 0 zero 0 unread 0",
            "psp probe 2: read 0x70800000: 0x00100000 bytes, 256 pages in block 0x00010001 (7 us): reading",
            want_sum,
            "psp probe 2: done",
            job[0], job[1],
        };
        lines_are(0, want, 14);
        TEST_CHECK(F.lines == 14);
    }
    /* each read after the line of the step before it: the title after the blocks, the site's four words after
     * the luna line, the 1 MiB after its pages' line */
    TEST_CHECK(F.reads == 6 && F.bad_reads == 0 && F.sum_calls == 1 && F.find_calls == 256 && F.block_calls == 3);
    TEST_CHECK(F.n_read_after == 6 && F.read_after[0] == 5 && F.read_after[1] == 7 && F.read_after[4] == 7 &&
               F.read_after[5] == 10);
    TEST_CHECK(p.passes == 1 && p.jobs == 1 && p.job_lines == 1 && p.timed);
    /* the next job, nothing changed: its lookups and the title, read again, and its two lines only */
    vjo_psp_probe_job(&p, &fake_ops, job_result(8), 0, 0x80, 1000);
    TEST_CHECK(F.lines == 16 && starts(log_line(14), "psp probe 2: job 8: rc 0, flags 0x00000000 speed 0x00000080,") &&
               starts(log_line(15), "psp probe 2: job 8 ocr: "));
    TEST_CHECK(p.passes == 1 && F.reads == 7 && F.read_after[6] == 14 && F.bad_reads == 0 && F.sum_calls == 1 &&
               F.find_calls == 256 && F.block_calls == 6);
    /* no result (a job the worker had none for): the lookups, no line */
    vjo_psp_probe_job(&p, &fake_ops, NULL, 0, 0x80, 0);
    TEST_CHECK(F.lines == 16 && p.jobs == 3 && p.job_lines == 2);
}

/* Adrenaline's XMB, then a game from it, in the same process: a second pass, with no second timed read. */
static void test_run_xmb_then_game(void)
{
    VjoPspProbe p;
    fake_reset();
    memset(&p, 0, sizeof(p));
    vjo_psp_probe_job(&p, &fake_ops, job_result(1), 0, 0x80, 0);
    TEST_CHECK(!strcmp(log_line(5), "psp probe 2: titleid 0x73CDE098: 000000000000000000000000 \"\"") &&
               !strcmp(log_line(6), "psp probe 2: luna : not listed") && starts(log_line(7), "psp probe 2: read ") &&
               !strcmp(log_line(9), "psp probe 2: done"));
    TEST_CHECK(F.sum_calls == 1 && F.bad_reads == 0 && p.passes == 1);
    vjo_psp_probe_job(&p, &fake_ops, job_result(2), 0, 0x80, 0); /* the XMB still */
    TEST_CHECK(p.passes == 1 && log_count("start: pass") == 1);
    set_title("ULJM05889");
    put_site(0x08868AB8u, JAL(0x08804100u), NOP, ADDU(A0, V0, ZERO), LW(RA, 28, SP));
    vjo_psp_probe_job(&p, &fake_ops, job_result(3), 0, 0x80, 0);
    TEST_CHECK(p.passes == 2 && log_count("start: pass") == 2 && log_count("start: pass 2 after job 3\n") == 1);
    TEST_CHECK(log_count("site 0x08868AB8 at 0x70868AB8: 0E201040 00000000 00402021 8FBF001C return\n") == 1 &&
               log_count("classes: entry 0 return 1 ") == 1);
    TEST_CHECK(F.sum_calls == 1 && log_count(": reading\n") == 1 && log_count("psp probe 2: done\n") == 2);
    /* reads: job 1 the title and the 1 MiB, job 2 the title, job 3 the title (once) and the site's 4 words */
    TEST_CHECK(F.reads == 2u + 1u + 5u && F.bad_reads == 0);
}

/* A block missing: the pass stops after the block lines and reads nothing; a mapping that appears later is a
 * new pass. */
static void test_run_missing_block(void)
{
    VjoPspProbe p;
    fake_reset();
    memset(&p, 0, sizeof(p));
    set_title("ULJM05889");
    F.blk[0].size = 0x03000000u; /* ends at 0x73000000: no block at 0x73CDE000 */
    vjo_psp_probe_job(&p, &fake_ops, job_result(1), 0, 0x80, 0);
    {
        const char *const want[] = {
            "psp probe 2: start: pass 1 after job 1",
            MODULE_LINE,
            "psp probe 2: block 0x70000000: uid 0x00010001 info 0x00000000 base 0x70000000 size 0x03000000 memtype "
            "0x000000D0 (cached) access 0x00000006 type 0x0C20D060 ok",
            "psp probe 2: block 0x70800000: uid 0x00010001 info 0x00000000 base 0x70000000 size 0x03000000 memtype "
            "0x000000D0 (cached) access 0x00000006 type 0x0C20D060 ok",
            "psp probe 2: block 0x73CDE000: uid 0x80020006 info 0x80020006 base 0x00000000 size 0x00000000 memtype "
            "0x00000000 (?) access 0x00000000 type 0x00000000 bad",
            "psp probe 2: stop: a block is missing or not readable",
        };
        lines_are(0, want, 6);
    }
    TEST_CHECK(F.reads == 0 && F.sum_calls == 0 && F.find_calls == 0 && F.lines == 8 && !p.timed);
    vjo_psp_probe_job(&p, &fake_ops, job_result(2), 0, 0x80, 0); /* the same: no pass, nothing read */
    TEST_CHECK(F.reads == 0 && p.passes == 1 && F.lines == 10);
    F.blk[0].size = 0x04000000u;
    vjo_psp_probe_job(&p, &fake_ops, job_result(3), 0, 0x80, 0);
    TEST_CHECK(p.passes == 2 && log_count("start: pass 2 after job 3\n") == 1 && log_count("luna ULJM05889: 1 sites") == 1 &&
               F.sum_calls == 1 && p.timed && F.bad_reads == 0);
    /* a block that is there but not readable: the same stop */
    fake_reset();
    memset(&p, 0, sizeof(p));
    F.blk[0].access = 2;
    vjo_psp_probe_job(&p, &fake_ops, job_result(1), 0, 0x80, 0);
    TEST_CHECK(log_count(" bad\n") == 3 && log_count("stop: a block is missing or not readable\n") == 1 && F.reads == 0);
}

/* The three blocks found, but none holds the title ID's 12 bytes: no read at all. */
static void test_run_title_outside(void)
{
    VjoPspProbe p;
    fake_reset();
    memset(&p, 0, sizeof(p));
    set_title("ULJM05889");
    F.blk[0] = view_block(0x70000000u, 0x03CDE000u, VIEW_UID);
    F.blk[1] = view_block(ADR_BASE, VJO_PSP_TITLEID_OFFSET, 0x10002); /* holds 0x73CDE000, ends at the title */
    F.n_blk = 2;
    vjo_psp_probe_job(&p, &fake_ops, job_result(1), 0, 0x80, 0);
    TEST_CHECK(!strcmp(log_line(5), "psp probe 2: titleid 0x73CDE098: outside the blocks") &&
               !strcmp(log_line(6), "psp probe 2: stop: the title ID is outside the blocks"));
    TEST_CHECK(log_count(" ok\n") == 3 && F.reads == 0 && F.sum_calls == 0 && !p.timed);
}

/* A block that ends at the site: the two words inside it are read, the two past it are not; the 1 MiB is not
 * inside a block either. */
static void test_run_site_outside(void)
{
    VjoPspProbe p;
    fake_reset();
    memset(&p, 0, sizeof(p));
    set_title("ULJM05889");
    put_site(0x08868AB8u, JR(RA), NOP, ADDIU(SP, SP, -32), SW(RA, 28, SP));
    F.blk[0] = view_block(0x70000000u, 0x00868AB8u, VIEW_UID); /* ends at the target */
    F.blk[1] = view_block(ADR_BASE, ADR_BYTES, 0x10002);
    F.n_blk = 2;
    vjo_psp_probe_job(&p, &fake_ops, job_result(1), 0, 0x80, 0);
    TEST_CHECK(log_count("site 0x08868AB8 at 0x70868AB8: 03E00008 00000000 -------- -------- unread\n") == 1 &&
               log_count("classes: entry 0 return 0 branch 0 delay 0 other 0 zero 0 unread 1\n") == 1 &&
               log_count("read 0x70800000: 0x00100000 bytes: outside the blocks\n") == 1 &&
               log_count("psp probe 2: done\n") == 1);
    TEST_CHECK(F.reads == 3 && F.bad_reads == 0 && F.sum_calls == 0 && F.find_calls == 0 && p.timed);
}

/* Blocks that end inside a word or inside the title ID: what straddles the end is not read. */
static void test_run_partial_ends(void)
{
    VjoPspProbe p;
    fake_reset();
    memset(&p, 0, sizeof(p));
    set_title("ULJM05889");
    put_site(0x08868AB8u, JR(RA), NOP, ADDIU(SP, SP, -32), SW(RA, 28, SP));
    F.blk[0] = view_block(0x70000000u, 0x00868ABAu, VIEW_UID); /* ends 2 bytes into the target's word */
    F.blk[1] = view_block(ADR_BASE, ADR_BYTES, 0x10002);
    F.n_blk = 2;
    vjo_psp_probe_job(&p, &fake_ops, job_result(1), 0, 0x80, 0);
    TEST_CHECK(log_count("site 0x08868AB8 at 0x70868AB8: 03E00008 00000000 -------- -------- unread\n") == 1);
    TEST_CHECK(F.reads == 3 && F.bad_reads == 0);
    /* the title's block ends 6 bytes into it: not read, by a pass or between passes */
    fake_reset();
    memset(&p, 0, sizeof(p));
    set_title("ULJM05889");
    F.blk[0] = view_block(0x70000000u, 0x03CDE000u, VIEW_UID);
    F.blk[1] = view_block(ADR_BASE, VJO_PSP_TITLEID_OFFSET + 6u, 0x10002);
    F.n_blk = 2;
    vjo_psp_probe_job(&p, &fake_ops, job_result(1), 0, 0x80, 0);
    vjo_psp_probe_job(&p, &fake_ops, job_result(2), 0, 0x80, 0);
    TEST_CHECK(log_count("titleid 0x73CDE098: outside the blocks\n") == 1 && F.reads == 0 && F.bad_reads == 0);
}

/* The user partition in a block of its own, the view's start in another: the sites and the 1 MiB are read in
 * the second (each read checked against every block found, the 1 MiB's pages against the block that holds it). */
static void test_run_two_blocks(void)
{
    VjoPspProbe p;
    fake_reset();
    memset(&p, 0, sizeof(p));
    set_title("ULJM05889");
    put_site(0x08868AB8u, JR(RA), NOP, ADDIU(SP, SP, -32), SW(RA, 28, SP));
    F.blk[0] = view_block(0x70000000u, 0x00800000u, VIEW_UID);  /* PSP 0x08000000-0x087FFFFF */
    F.blk[1] = view_block(USER_BASE, 0x03800000u, 0x10002);     /* PSP 0x08800000-0x0BFFFFFF */
    F.n_blk = 2;
    vjo_psp_probe_job(&p, &fake_ops, job_result(1), 0, 0x80, 0);
    TEST_CHECK(log_count("site 0x08868AB8 at 0x70868AB8: 03E00008 00000000 27BDFFE0 AFBF001C entry\n") == 1);
    TEST_CHECK(log_count("read 0x70800000: 0x00100000 bytes, 256 pages in block 0x00010002 (7 us): reading\n") == 1 &&
               F.sum_calls == 1 && F.find_calls == 256 && F.bad_reads == 0);
}

/* Each of what a pass compares, changed alone, gives a new pass: a block's UID, base, size, readability; the
 * title. Nothing changed gives none. The reads between passes: the title only. */
static void test_run_changes(void)
{
    VjoPspProbe p;
    unsigned reads;
    fake_reset();
    memset(&p, 0, sizeof(p));
    set_title("ULJM05889");
    vjo_psp_probe_job(&p, &fake_ops, job_result(1), 0, 0x80, 0);
    TEST_CHECK(p.passes == 1);
    reads = F.reads;
    vjo_psp_probe_job(&p, &fake_ops, job_result(2), 0, 0x80, 0);
    TEST_CHECK(p.passes == 1 && F.reads == reads + 1u); /* the title, read again */
    F.blk[0].uid = 0x10009;
    vjo_psp_probe_job(&p, &fake_ops, job_result(3), 0, 0x80, 0);
    TEST_CHECK(p.passes == 2);
    F.blk[0].base = 0x6FFFF000u;
    F.blk[0].size = 0x04001000u;
    vjo_psp_probe_job(&p, &fake_ops, job_result(4), 0, 0x80, 0);
    TEST_CHECK(p.passes == 3);
    F.blk[0].size = 0x04002000u;
    vjo_psp_probe_job(&p, &fake_ops, job_result(5), 0, 0x80, 0);
    TEST_CHECK(p.passes == 4);
    reads = F.reads;
    F.blk[0].access = 2; /* not readable: a pass that stops, and no read between */
    vjo_psp_probe_job(&p, &fake_ops, job_result(6), 0, 0x80, 0);
    TEST_CHECK(p.passes == 5 && F.reads == reads && log_count("stop: a block is missing or not readable\n") == 1);
    vjo_psp_probe_job(&p, &fake_ops, job_result(7), 0, 0x80, 0);
    TEST_CHECK(p.passes == 5 && F.reads == reads);
    F.blk[0].access = 6;
    vjo_psp_probe_job(&p, &fake_ops, job_result(8), 0, 0x80, 0);
    TEST_CHECK(p.passes == 6 && F.reads == reads + 1u + 4u); /* the title once (between passes), the site's words */
    set_title("ULJM05795");
    vjo_psp_probe_job(&p, &fake_ops, job_result(9), 0, 0x80, 0);
    TEST_CHECK(p.passes == 7 && log_count("luna ULJM05795: 1 sites, 1 read\n") == 1 && F.bad_reads == 0);
    /* only which block fails changes (a block per view; the title unread either way): a new pass */
    fake_reset();
    memset(&p, 0, sizeof(p));
    F.blk[0] = view_block(0x70000000u, 0x00800000u, VIEW_UID);
    F.blk[1] = view_block(USER_BASE, 0x03000000u, 0x10002);
    F.blk[2] = view_block(ADR_BASE, ADR_BYTES, 0x10003);
    F.n_blk = 3;
    F.blk[2].info_rc = -1; /* found, but its info refused: not ok */
    vjo_psp_probe_job(&p, &fake_ops, job_result(1), 0, 0x80, 0);
    TEST_CHECK(p.passes == 1 && log_count(" bad\n") == 1);
    F.blk[2].info_rc = 0;
    F.blk[1].info_rc = -1;
    vjo_psp_probe_job(&p, &fake_ops, job_result(2), 0, 0x80, 0);
    TEST_CHECK(p.passes == 2 && log_count(" bad\n") == 2 && F.reads == 0);
}

/* A block op that writes another address than the one asked: the lines name the address asked. */
static void f_block_wrong_addr(void *ud, uint32_t addr, VjoPspBlock *out)
{
    f_block(ud, addr, out);
    out->addr = 0;
}

static void test_run_block_addr(void)
{
    const VjoPspOps ops = {NULL, f_module, f_block_wrong_addr, f_find, f_word, f_bytes, f_sum, f_now, f_put};
    VjoPspProbe p;
    fake_reset();
    memset(&p, 0, sizeof(p));
    vjo_psp_probe_job(&p, &ops, job_result(1), 0, 0x80, 0);
    TEST_CHECK(!strcmp(log_line(2), blocks_ok[0]) && !strcmp(log_line(3), blocks_ok[1]) &&
               !strcmp(log_line(4), blocks_ok[2]) && F.bad_reads == 0);
}

/* A page of the 1 MiB whose lookup names another block: the read is not made, and not tried again. */
static void test_run_odd_page(void)
{
    VjoPspProbe p;
    fake_reset();
    memset(&p, 0, sizeof(p));
    F.odd_page = 0x70880000u;
    F.odd_uid = 0x20002;
    vjo_psp_probe_job(&p, &fake_ops, job_result(1), 0, 0x80, 0);
    TEST_CHECK(log_count("read 0x70800000: 0x00100000 bytes: page 0x70880000 in block 0x00020002, not 0x00010001: "
                         "not read\n") == 1);
    TEST_CHECK(F.sum_calls == 0 && F.find_calls == 129 && F.bad_reads == 0 && p.timed);
    set_title("ULJM05889"); /* a new pass: no timed read in it */
    vjo_psp_probe_job(&p, &fake_ops, job_result(2), 0, 0x80, 0);
    TEST_CHECK(p.passes == 2 && F.find_calls == 129 && F.sum_calls == 0 && log_count("read 0x70800000") == 1);
}

/* The most sites a title has (6, NPJH50619), one of each class, in the table's order. */
static void test_run_classes(void)
{
    VjoPspProbe p;
    fake_reset();
    memset(&p, 0, sizeof(p));
    set_title("NPJH50619");
    put_site(0x08952CFCu, JR(RA), NOP, ADDIU(SP, SP, -32), SW(RA, 28, SP));        /* entry */
    put_site(0x0884AAD4u, JAL(0x08804100u), NOP, ADDU(A0, V0, ZERO), LW(RA, 28, SP)); /* return */
    put_site(0x0882E1B0u, ADDIU(SP, SP, -32), LW(V0, 0, S0), JR(RA), NOP);          /* branch */
    put_site(0x088BB108u, LW(V0, 0, S0), JAL(0x08804100u), ADDIU(A0, A0, 1), NOP);  /* delay */
    put_site(0x089526A0u, LUI(V0, 0x0889), ADDIU(V0, V0, 4), LW(A0, 0, V0), SW(A0, 4, SP)); /* other */
    /* 0x088BCEF8: left 0, zero */
    vjo_psp_probe_job(&p, &fake_ops, job_result(1), 0, 0x80, 0);
    {
        const char *const want[] = {
            "psp probe 2: luna NPJH50619: 6 sites, 6 read",
            "psp probe 2: site 0x08952CFC at 0x70952CFC: 03E00008 00000000 27BDFFE0 AFBF001C entry",
            "psp probe 2: site 0x0884AAD4 at 0x7084AAD4: 0E201040 00000000 00402021 8FBF001C return",
            "psp probe 2: site 0x0882E1B0 at 0x7082E1B0: 27BDFFE0 8E020000 03E00008 00000000 branch",
            "psp probe 2: site 0x088BB108 at 0x708BB108: 8E020000 0E201040 24840001 00000000 delay",
            "psp probe 2: site 0x089526A0 at 0x709526A0: 3C020889 24420004 8C440000 AFA40004 other",
            "psp probe 2: site 0x088BCEF8 at 0x708BCEF8: 00000000 00000000 00000000 00000000 zero",
            "psp probe 2: classes: entry 1 return 1 branch 1 delay 1 other 1 zero 1 unread 0",
        };
        lines_are(6, want, 8);
    }
    TEST_CHECK(F.reads == 1 + 24 + 1 && F.bad_reads == 0);
}

/* A title that keeps changing: VJO_PSP_PASSES_MAX passes, then no lookup and no read at all, job lines still. */
static void test_run_pass_cap(void)
{
    VjoPspProbe p;
    unsigned calls = 0, reads = 0;
    fake_reset();
    memset(&p, 0, sizeof(p));
    for (unsigned j = 1; j <= 2u * VJO_PSP_PASSES_MAX; j++) {
        set_title(j % 2u ? "ULJM05889" : "ULJM05795");
        vjo_psp_probe_job(&p, &fake_ops, job_result(j), 0, 0x80, 0);
        if (j == VJO_PSP_PASSES_MAX) {
            calls = F.block_calls;
            reads = F.reads;
        }
    }
    TEST_CHECK(p.passes == VJO_PSP_PASSES_MAX && log_count("start: pass") == VJO_PSP_PASSES_MAX);
    TEST_CHECK(F.block_calls == calls && F.reads == reads && F.sum_calls == 1 && F.bad_reads == 0);
    TEST_CHECK(log_count(" ocr: ") == 2u * VJO_PSP_PASSES_MAX && p.job_lines == 2u * VJO_PSP_PASSES_MAX);
}

/* At most VJO_PSP_JOB_LINES_MAX jobs' lines, then one line that says so, then nothing. */
static void test_run_job_cap(void)
{
    VjoPspProbe p;
    fake_reset();
    memset(&p, 0, sizeof(p));
    for (unsigned j = 1; j <= VJO_PSP_JOB_LINES_MAX + 5u; j++) vjo_psp_probe_job(&p, &fake_ops, job_result(j), 0, 0x80, 0);
    TEST_CHECK(log_count(" ocr: ") == VJO_PSP_JOB_LINES_MAX && log_count("job 1000 ocr: ") == 1 &&
               log_count("job 1001") == 0);
    TEST_CHECK(log_count("psp probe 2: jobs: 1000, the most per process: no more job lines\n") == 1);
    TEST_CHECK(p.jobs == VJO_PSP_JOB_LINES_MAX + 5u && F.bad_reads == 0);
}

/* No state, no ops, or ops without reads: nothing is read, nothing breaks. */
static void test_run_null(void)
{
    static const VjoPspOps put_only = {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, f_put};
    static const VjoPspOps none = {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
    VjoPspProbe p;
    fake_reset();
    memset(&p, 0, sizeof(p));
    vjo_psp_probe_job(NULL, &fake_ops, job_result(1), 0, 0x80, 0);
    vjo_psp_probe_job(&p, NULL, job_result(1), 0, 0x80, 0);
    TEST_CHECK(F.lines == 0 && F.block_calls == 0 && p.jobs == 0);
    vjo_psp_probe_job(&p, &none, job_result(1), 0, 0x80, 0);
    TEST_CHECK(p.jobs == 1 && p.passes == 1);
    memset(&p, 0, sizeof(p));
    vjo_psp_probe_job(&p, &put_only, NULL, 0, 0x80, 0);
    TEST_CHECK(!strcmp(log_line(0), "psp probe 2: start: pass 1 after job 0") &&
               !strcmp(log_line(1), "psp probe 2: ScePspemu: rc 0xFFFFFFFF nid 0x00000000 modid 0x00000000 info "
                                    "0x00000000 seg0 0x00000000 size 0x00000000") &&
               log_count(" bad\n") == 3 && log_count("stop: a block is missing or not readable\n") == 1 && F.lines == 6);
    /* the blocks there, but no read ops: the title is not read, and the pass says so */
    fake_reset();
    memset(&p, 0, sizeof(p));
    {
        const VjoPspOps no_reads = {NULL, f_module, f_block, f_find, NULL, NULL, NULL, NULL, f_put};
        vjo_psp_probe_job(&p, &no_reads, job_result(1), 0, 0x80, 0);
    }
    TEST_CHECK(log_count("titleid 0x73CDE098: outside the blocks\n") == 1 && F.reads == 0);
}

TEST_LIST = {
    {"counts", test_counts},
    {"every_title", test_every_title},
    {"known_entries", test_known_entries},
    {"not_listed", test_not_listed},
    {"cap", test_cap},
    {"vita_addr", test_vita_addr},
    {"blocks", test_blocks},
    {"holder", test_holder},
    {"title_id", test_title_id},
    {"lines", test_lines},
    {"line_bounds", test_line_bounds},
    {"mips", test_mips},
    {"classify", test_classify},
    {"run_listed", test_run_listed},
    {"run_xmb_then_game", test_run_xmb_then_game},
    {"run_missing_block", test_run_missing_block},
    {"run_title_outside", test_run_title_outside},
    {"run_site_outside", test_run_site_outside},
    {"run_partial_ends", test_run_partial_ends},
    {"run_two_blocks", test_run_two_blocks},
    {"run_changes", test_run_changes},
    {"run_block_addr", test_run_block_addr},
    {"run_odd_page", test_run_odd_page},
    {"run_classes", test_run_classes},
    {"run_pass_cap", test_run_pass_cap},
    {"run_job_cap", test_run_job_cap},
    {"run_null", test_run_null},
    {NULL, NULL},
};
