/* PSP text (core/psp_text.h, docs/native-hooks.md "PSP text (Adrenaline)"): its profiles against the farm's own
 * records (hooked-b2/psp/<title>/hook/rubi-hook.json: the site's address and its 16 signature bytes as the
 * decrypted EBOOT has them, the buffer every event's register held), the poll on a fake PSP RAM whose every read
 * is recorded and checked, and the two engines' filters on synthetic lines carrying the farm's codes. No game
 * text is used: the lines are made up. */
#include "acutest.h"

#include <stdio.h>
#include <string.h>

#include "../../core/hook_luna.h"
#include "../../core/hook_text.h"
#include "../../core/psp_text.h"

/* ---- the profiles against the farm's records ---- */

static uint32_t le32(const char *hex, unsigned word) /* word k of a hex byte string, little-endian */
{
    unsigned b[4];
    TEST_ASSERT(sscanf(hex + 8u * word, "%2x%2x%2x%2x", &b[0], &b[1], &b[2], &b[3]) == 4);
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static const struct {
    const char *psn, *umd, *signature; /* rubi-hook.json's title, its EBOOT dump's disc ID, its signature */
    uint32_t address, buffer;          /* rubi-hook.json's address; the events' register value (pages.jsonl.zst) */
    unsigned filter;
} farm[] = {
    {"NPJH50382", "ULJM05467", "0e0023960c0024969e08023c001c0300", 0x8898920u, 0x089DA830u, VJO_RF_5PB},
    {"NPJH50132", "ULJM05350", "b0febd272c00bfaf2800beaf2400b7af", 0x8942070u, 0x08BA8100u, VJO_RF_KID},
};

static void profiles_are_the_farms(void)
{
    for (unsigned i = 0; i < sizeof farm / sizeof farm[0]; i++) {
        const VjoPspProfile *p = vjo_psp_text_profile(farm[i].psn);
        TEST_ASSERT_(p != NULL, "%s", farm[i].psn);
        TEST_CHECK(vjo_psp_text_profile(farm[i].umd) == p); /* either ID */
        TEST_CHECK(p->site == farm[i].address && p->buffer == farm[i].buffer && p->filter == farm[i].filter);
        TEST_CHECK(p->encoding == VJO_HOOK_ENC_CP932);
        for (unsigned k = 0; k < VJO_PSP_CODE_WORDS; k++)
            TEST_CHECK_(p->code[k] == le32(farm[i].signature, k), "%s word %u", farm[i].psn, k);
        /* the site and the buffer are in the PSP's user RAM (0x08800000-0x0A000000): the 32 MiB view mapping
         * the owner's console reported (Vita 0x70000000, #248) holds both */
        TEST_CHECK(p->site >= 0x08800000u && p->site < 0x0A000000u && p->buffer >= 0x08800000u &&
                   p->buffer + VJO_PSP_TEXT_MAX <= 0x0A000000u);
        TEST_CHECK(vjo_psp_view(p->buffer) >= 0x70000000u && vjo_psp_view(p->buffer) + VJO_PSP_TEXT_MAX <= 0x72000000u);
    }
    /* Memories Off 6's site loads the buffer's high half: lui v0, 0x089E (0x089DA830 = 0x089E0000 - 0x57D0) */
    {
        const VjoPspProfile *p = vjo_psp_text_profile("ULJM05467");
        TEST_CHECK(p->code[2] == (0x3C020000u | ((p->buffer + 0x8000u) >> 16)));
    }
    /* not an ID, or an ID without a profile */
    TEST_CHECK(!vjo_psp_text_profile(NULL) && !vjo_psp_text_profile("") && !vjo_psp_text_profile("ULJM0546") &&
               !vjo_psp_text_profile("uljm05467") && !vjo_psp_text_profile("ULJS00339") &&
               !vjo_psp_text_profile("ULJM05889"));
    /* Adrenaline's view (CONVERT_ADDRESS), and SceAdrenaline.titleid's place in it */
    TEST_CHECK(vjo_psp_view(0x08800000u) == 0x70800000u && vjo_psp_view(0x88800000u) == 0x70800000u);
    TEST_CHECK(vjo_psp_view(VJO_PSP_TITLEID_ADDR) == 0x73CDE098u);
}

static void title_ids(void)
{
    char id[10];
    TEST_CHECK(vjo_psp_title_id((const uint8_t *)"ULJM05467\0\0", id) && !strcmp(id, "ULJM05467"));
    TEST_CHECK(vjo_psp_title_id((const uint8_t *)"NPJH50382\0\0", id) && !strcmp(id, "NPJH50382"));
    TEST_CHECK(!vjo_psp_title_id((const uint8_t *)"\0\0\0\0\0\0\0\0\0\0\0", id) && !id[0]); /* the XMB */
    TEST_CHECK(!vjo_psp_title_id((const uint8_t *)"ULJM-0546\0\0", id) && !id[0]);
    TEST_CHECK(!vjo_psp_title_id((const uint8_t *)"ULJM054670\0", id)); /* no NUL after 9 */
    TEST_CHECK(!vjo_psp_title_id(NULL, id) && !id[0]);
}

/* ---- the poll on a fake PSP RAM ---- */

#define RAM_BASE 0x08800000u
#define RAM_SIZE 0x01800000u /* PSP 0x08800000-0x0A000000, as the console's view mapping covers it */
static uint8_t ram[RAM_SIZE];
static uint8_t adrenaline[0x2000]; /* SceAdrenaline, PSP 0xABCDE000 (a mapping of its own) */
static uint32_t ram_end = RAM_BASE + RAM_SIZE; /* a test shortens the mapping */
static int no_adrenaline;
static unsigned reads;
static struct {
    uint32_t addr;
    unsigned max, unit;
} read_log[16];
static void (*after_read)(unsigned k); /* the game writing between two reads */

static unsigned fake_read(void *ud, uint32_t psp, void *dst, unsigned max, unsigned unit)
{
    const uint8_t *src = NULL;
    uint32_t end = 0, n = 0;
    TEST_CHECK(ud == (void *)&reads);
    if (reads < 16) read_log[reads] = (__typeof__(read_log[0])){psp, max, unit};
    reads++;
    if (!no_adrenaline && psp >= VJO_PSP_ADRENALINE_ADDR && psp < VJO_PSP_ADRENALINE_ADDR + sizeof adrenaline) {
        src = adrenaline + (psp - VJO_PSP_ADRENALINE_ADDR);
        end = VJO_PSP_ADRENALINE_ADDR + sizeof adrenaline;
    } else if (psp >= RAM_BASE && psp < ram_end) {
        src = ram + (psp - RAM_BASE);
        end = ram_end;
    }
    if (!src || !max) return 0;
    if (end - psp < max) { /* read_game: never past the mapping's end */
        if (!unit) return 0;
        max = end - psp;
    }
    for (; n < max; n++) {
        ((uint8_t *)dst)[n] = src[n];
        if (unit == 1 && !src[n]) {
            n++;
            break;
        }
    }
    if (after_read) after_read(reads);
    return n;
}

static const VjoPspTextOps ops = {&reads, fake_read};

static void game(const char *id, const char *signature, uint32_t site)
{
    memset(ram, 0, sizeof ram);
    memset(adrenaline, 0, sizeof adrenaline);
    ram_end = RAM_BASE + RAM_SIZE;
    no_adrenaline = 0;
    after_read = NULL;
    reads = 0;
    if (id) memcpy(adrenaline + 0x98, id, strlen(id) + 1u);
    for (unsigned k = 0; signature && k < VJO_PSP_CODE_WORDS; k++) {
        uint32_t w = le32(signature, k);
        memcpy(ram + (site - RAM_BASE) + 4u * k, &w, 4);
    }
}

static void page(uint32_t buffer, const void *bytes, size_t n)
{
    memcpy(ram + (buffer - RAM_BASE), bytes, n);
}

static uint8_t *const mo6_page = ram + (0x089DA830u - RAM_BASE);
static void writes_the_page(unsigned k)
{
    if (k == 3) mo6_page[1] ^= 0x01; /* after the first read of the buffer (the 3rd read), before the 4th */
}

static void poll_reads_only_what_it_must(void)
{
    const char *const sig = farm[0].signature, *const line = "\x82\xA0\x82\xA2%K%P"; /* あい%K%P */
    const VjoPspProfile *p = (const VjoPspProfile *)1;
    uint8_t out[VJO_PSP_TEXT_MAX + 8];
    unsigned n = 99;
    /* the game of a profile, its code, its page: the title ID, the site's 16 bytes, the page twice */
    game("ULJM05467", sig, 0x08898920u);
    page(0x089DA830u, line, strlen(line) + 1u);
    TEST_CHECK(vjo_psp_text_poll(&ops, &p, out, sizeof out, &n) == VJO_PSP_TEXT_OK);
    TEST_CHECK(p == vjo_psp_text_profile("NPJH50382") && n == strlen(line) && !memcmp(out, line, n));
    TEST_ASSERT(reads == 4);
    TEST_CHECK(read_log[0].addr == VJO_PSP_TITLEID_ADDR && read_log[0].max == 12 && read_log[0].unit == 0);
    TEST_CHECK(read_log[1].addr == 0x08898920u && read_log[1].max == 16 && read_log[1].unit == 0);
    TEST_CHECK(read_log[2].addr == 0x089DA830u && read_log[2].max == VJO_PSP_TEXT_MAX && read_log[2].unit == 1);
    TEST_CHECK(read_log[3].addr == 0x089DA830u && read_log[3].max == VJO_PSP_TEXT_MAX && read_log[3].unit == 1);
    /* its PSN ID too; the other profile's game */
    game("NPJH50382", sig, 0x08898920u);
    page(0x089DA830u, line, strlen(line) + 1u);
    TEST_CHECK(vjo_psp_text_poll(&ops, &p, out, sizeof out, &n) == VJO_PSP_TEXT_OK && p->site == 0x08898920u);
    game("ULJM05350", farm[1].signature, 0x08942070u);
    page(0x08BA8100u, line, strlen(line) + 1u);
    TEST_CHECK(vjo_psp_text_poll(&ops, &p, out, sizeof out, &n) == VJO_PSP_TEXT_OK && p->site == 0x08942070u);

    /* SceAdrenaline unreadable: nothing else is read */
    game("ULJM05467", sig, 0x08898920u);
    no_adrenaline = 1;
    TEST_CHECK(vjo_psp_text_poll(&ops, &p, out, sizeof out, &n) == VJO_PSP_TEXT_NO_VIEW && !p && !n && reads == 1);
    /* the XMB (no ID), and a game without a profile: only the title ID */
    game(NULL, sig, 0x08898920u);
    TEST_CHECK(vjo_psp_text_poll(&ops, &p, out, sizeof out, &n) == VJO_PSP_TEXT_NO_TITLE && !p && reads == 1);
    game("ULJS00339", sig, 0x08898920u);
    TEST_CHECK(vjo_psp_text_poll(&ops, &p, out, sizeof out, &n) == VJO_PSP_TEXT_UNLISTED && !p && reads == 1);
    /* another build of the game: one word differs; the page is never read */
    game("ULJM05467", sig, 0x08898920u);
    page(0x089DA830u, line, strlen(line) + 1u);
    ram[0x08898920u - RAM_BASE + 9] ^= 0x40;
    TEST_CHECK(vjo_psp_text_poll(&ops, &p, out, sizeof out, &n) == VJO_PSP_TEXT_CODE && !p && !n && reads == 2);
    /* nothing loaded there yet (all zero) */
    game("ULJM05467", NULL, 0x08898920u);
    TEST_CHECK(vjo_psp_text_poll(&ops, &p, out, sizeof out, &n) == VJO_PSP_TEXT_CODE && reads == 2);

    /* the page: no NUL in 512 bytes; cut by the mapping's end; written between the two reads; empty */
    game("ULJM05467", sig, 0x08898920u);
    memset(mo6_page, 'A', VJO_PSP_TEXT_MAX + 4u);
    TEST_CHECK(vjo_psp_text_poll(&ops, &p, out, sizeof out, &n) == VJO_PSP_TEXT_UNENDED && p && !n && reads == 3);
    TEST_CHECK(read_log[2].max == VJO_PSP_TEXT_MAX); /* never more than VJO_PSP_TEXT_MAX, whatever cap */
    mo6_page[VJO_PSP_TEXT_MAX - 1u] = 0; /* 511 bytes and the NUL: whole */
    TEST_CHECK(vjo_psp_text_poll(&ops, &p, out, sizeof out, &n) == VJO_PSP_TEXT_OK && n == VJO_PSP_TEXT_MAX - 1u);
    game("ULJM05467", sig, 0x08898920u);
    memset(mo6_page, 'A', 64);
    ram_end = 0x089DA830u + 32u; /* the mapping ends inside the page */
    TEST_CHECK(vjo_psp_text_poll(&ops, &p, out, sizeof out, &n) == VJO_PSP_TEXT_UNENDED && !n);
    game("ULJM05467", sig, 0x08898920u);
    page(0x089DA830u, line, strlen(line) + 1u);
    after_read = writes_the_page;
    TEST_CHECK(vjo_psp_text_poll(&ops, &p, out, sizeof out, &n) == VJO_PSP_TEXT_UNSTABLE && p && !n && reads == 4);
    after_read = NULL;
    TEST_CHECK(vjo_psp_text_poll(&ops, &p, out, sizeof out, &n) == VJO_PSP_TEXT_OK); /* the next poll takes it */
    game("ULJM05467", sig, 0x08898920u);
    TEST_CHECK(vjo_psp_text_poll(&ops, &p, out, sizeof out, &n) == VJO_PSP_TEXT_EMPTY && p && !n);
    page(0x089DA830u, "A", 2);
    TEST_CHECK(vjo_psp_text_poll(&ops, &p, out, sizeof out, &n) == VJO_PSP_TEXT_EMPTY && !n);
    page(0x089DA830u, "AB", 3);
    TEST_CHECK(vjo_psp_text_poll(&ops, &p, out, sizeof out, &n) == VJO_PSP_TEXT_OK && n == 2);

    /* a small cap reads no more than it; no ops, no room: nothing read */
    game("ULJM05467", sig, 0x08898920u);
    page(0x089DA830u, line, strlen(line) + 1u);
    TEST_CHECK(vjo_psp_text_poll(&ops, &p, out, 4, &n) == VJO_PSP_TEXT_UNENDED && read_log[2].max == 4);
    reads = 0;
    TEST_CHECK(vjo_psp_text_poll(NULL, &p, out, sizeof out, &n) == VJO_PSP_TEXT_OFF && !p && !reads);
    TEST_CHECK(vjo_psp_text_poll(&ops, &p, out, 1, &n) == VJO_PSP_TEXT_OFF && !reads);
    TEST_CHECK(vjo_psp_text_poll(&ops, &p, NULL, sizeof out, &n) == VJO_PSP_TEXT_OFF && !reads);
    TEST_CHECK(vjo_psp_text_poll(&ops, NULL, out, sizeof out, NULL) == VJO_PSP_TEXT_OK); /* NULL outs are fine */
}

/* ---- the engines' filters (core/hook_luna.c VJO_RF_5PB, VJO_RF_KID), on made-up lines ---- */

static void filtered(unsigned filter, unsigned encoding, const char *raw, size_t n, const char *want)
{
    char out[1024];
    size_t got = vjo_luna_text_filter(NULL, filter, VJO_HOOK_ENC_CP932, (const uint8_t *)raw, (unsigned)n, encoding,
                                      out, sizeof out);
    TEST_CHECK_(got == strlen(want) && !strcmp(out, want), "%s -> %s (want %s)", raw, out, want);
}
#define F8(f, raw, want) filtered(f, VJO_HOOK_ENC_UTF8, raw, strlen(raw), want)

static void engine_filters(void)
{
    TEST_CHECK(vjo_luna_filter_known(VJO_RF_5PB) && vjo_luna_filter_known(VJO_RF_KID));
    /* 5pb.: %K, %P and %p draw nothing, nor ∬; %N a line break; {base}[reading] its base; a name stays */
    F8(VJO_RF_5PB, "【名前】「こんにちは」%K%P", "【名前】「こんにちは」");
    F8(VJO_RF_5PB, "「それなら……。%p」", "「それなら……。」");
    F8(VJO_RF_5PB, "「えっ」∬∬∬∬%K%P", "「えっ」");
    F8(VJO_RF_5PB, "一行目%N二行目%K%P", "一行目\n二行目");
    F8(VJO_RF_5PB, "{漢字}[かんじ]を読む%K%P", "漢字を読む");
    F8(VJO_RF_5PB, "%N%N%N下に寄せた行%K%P", "下に寄せた行");
    F8(VJO_RF_5PB, "%K%P", "");
    /* the CP932 bytes as the buffer holds them: 【名前】「こんにちは」∬∬%K%P */
    filtered(VJO_RF_5PB, VJO_HOOK_ENC_CP932,
             "\x81\x79\x96\xBC\x91\x4F\x81\x7A\x81\x75\x82\xB1\x82\xF1\x82\xC9\x82\xBF\x82\xCD\x81\x76\x81\xE8\x81\xE8"
             "%K%P", 30, "【名前】「こんにちは」");
    /* KID: %N a line break; %CXXXX, %LC, %FS, %FE draw nothing; ⑳ the narrow space in a name */
    F8(VJO_RF_KID, "%LC%CFFFF%FS見出しの文字%FE", "見出しの文字");
    F8(VJO_RF_KID, "山田⑳太郎「おはよう」", "山田 太郎「おはよう」");
    F8(VJO_RF_KID, "一行目%N二行目", "一行目\n二行目");
    F8(VJO_RF_KID, "%C00FF色の付いた文字", "色の付いた文字");
    filtered(VJO_RF_KID, VJO_HOOK_ENC_CP932, "\x8E\x52\x93\x63\x87\x53\x91\xBE\x98\x59\x25\x4E\x8E\x9F\x82\xCC\x8D\x73",
             18, "山田 太郎\n次の行");
}

TEST_LIST = {
    {"the profiles are the farm's records (address, signature, buffer, IDs)", profiles_are_the_farms},
    {"SceAdrenaline.titleid -> a game ID", title_ids},
    {"the poll reads only the title ID, the site and the page, behind every check", poll_reads_only_what_it_must},
    {"the 5pb. and KID filters on the farm's codes", engine_filters},
    {NULL, NULL}
};
