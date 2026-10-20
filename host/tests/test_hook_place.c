/* The native hooks' site check, Rubi's verified entry and the CALL prefilter (core/hook_place.c), on
 * byte fixtures the VitaSDK assembler made (tools/gen_hook_place_fixtures.sh). */
#include "acutest.h"

#include <stdio.h>
#include <string.h>

#include "../../core/hook_place.h"
#include "../../core/hook_text.h"

typedef struct {
    int thumb, ok, entry;
    uint8_t b[12];
    unsigned n;
    const char *src;
} Form;

static const Form forms[] = {
#include "fixtures/hook_place_forms.inc"
};
#define FORMS_N (sizeof(forms) / sizeof(forms[0]))

#define SEG_BASE 0x81000000u
#define CODE_N 256u

/* Padding the allow-list accepts: Thumb MOV r8, r8 (0x46C0), ARM MOV r0, r0 (0xE1A00000). */
static void pad(uint8_t *code, int thumb)
{
    for (unsigned i = 0; i < CODE_N; i += thumb ? 2u : 4u) {
        if (thumb) {
            code[i] = 0xC0;
            code[i + 1] = 0x46;
        } else {
            code[i] = 0x00;
            code[i + 1] = 0x00;
            code[i + 2] = 0xA0;
            code[i + 3] = 0xE1;
        }
    }
}

/* A site at off: the entry (Thumb PUSH {r4, lr} / ARM PUSH {r4, lr}), padding, the form at off + at. */
static void site(uint8_t *code, int thumb, unsigned off, unsigned at, const Form *f)
{
    pad(code, thumb);
    if (thumb) {
        code[off] = 0x10;
        code[off + 1] = 0xB5;
    } else {
        code[off] = 0x10;
        code[off + 1] = 0x40;
        code[off + 2] = 0x2D;
        code[off + 3] = 0xE9;
    }
    if (f) memcpy(code + off + at, f->b, f->n);
}

static void test_fixtures_present(void)
{
    unsigned t_ok = 0, t_no = 0, a_ok = 0, a_no = 0;
    for (unsigned i = 0; i < FORMS_N; i++) {
        const Form *f = &forms[i];
        if (f->thumb) f->ok ? t_ok++ : t_no++;
        else f->ok ? a_ok++ : a_no++;
    }
    TEST_CHECK(t_ok >= 30 && t_no >= 40 && a_ok >= 15 && a_no >= 25);
    TEST_MSG("thumb %u ok %u no, arm %u ok %u no", t_ok, t_no, a_ok, a_no);
}

/* A form as the site's first instruction: a function entry that saves LR, or refused (ENTRY). */
static void test_entries(void)
{
    uint8_t code[CODE_N];
    for (unsigned i = 0; i < FORMS_N; i++) {
        const Form *f = &forms[i];
        if (!f->entry) continue;
        for (unsigned off = 0x40; off <= (f->thumb ? 0x42u : 0x40u); off += 2) {
            int want = f->ok ? VJO_PLACE_OK : VJO_PLACE_ENTRY;
            int got;
            pad(code, f->thumb);
            memcpy(code + off, f->b, f->n);
            got = vjo_place_check(code, CODE_N, SEG_BASE, off, f->thumb);
            TEST_CHECK_(got == want, "%s entry '%s' at +%#x: %s, want %s", f->thumb ? "thumb" : "arm", f->src,
                        off, vjo_place_reason(got), vjo_place_reason(want));
            if (f->ok) {
                int isa = vjo_place_isa(code + off, CODE_N - off, SEG_BASE + off);
                TEST_CHECK_(isa == f->thumb, "isa of '%s' at +%#x: %d", f->src, off, isa);
            }
        }
    }
}

/* A form inside the bytes the hook overwrites: right after the entry, and as the last instruction
 * that starts in them (8 bytes at an address = 0 mod 4; 12 for Thumb at 2 mod 4). */
static void test_windows(void)
{
    uint8_t code[CODE_N];
    for (unsigned i = 0; i < FORMS_N; i++) {
        const Form *f = &forms[i];
        if (f->entry) continue;
        unsigned offs[2] = {0x40, 0x42};
        for (unsigned k = 0; k < (f->thumb ? 2u : 1u); k++) {
            unsigned off = offs[k], patch = vjo_place_patch_bytes(SEG_BASE + off, f->thumb);
            unsigned first = f->thumb ? 2u : 4u, last = patch - (f->thumb ? 2u : 4u);
            int want = f->ok ? VJO_PLACE_OK : VJO_PLACE_WINDOW;
            for (unsigned at = first; at <= last; at += last - first ? last - first : 1u) {
                int got;
                site(code, f->thumb, off, at, f);
                got = vjo_place_check(code, CODE_N, SEG_BASE, off, f->thumb);
                TEST_CHECK_(got == want, "%s '%s' at +%#x, window byte %u of %u: %s, want %s",
                            f->thumb ? "thumb" : "arm", f->src, off, at, patch, vjo_place_reason(got),
                            vjo_place_reason(want));
                if (last == first) break;
            }
        }
    }
}

/* What starts past the overwritten bytes is not looked at: the window's size decides. */
static void test_window_size(void)
{
    uint8_t code[CODE_N];
    const Form *bl = NULL, *ldr_arm = NULL;
    for (unsigned i = 0; i < FORMS_N; i++) {
        if (forms[i].thumb && !forms[i].entry && !strcmp(forms[i].src, "bl .+8")) bl = &forms[i];
        if (!forms[i].thumb && !forms[i].entry && !strcmp(forms[i].src, "ldr r0, [pc, #8]")) ldr_arm = &forms[i];
    }
    TEST_ASSERT(bl && ldr_arm);
    /* Thumb, BL at byte 8: outside an 8-byte patch (address 0 mod 4), inside a 12-byte one. */
    site(code, 1, 0x40, 8, bl);
    TEST_CHECK(vjo_place_check(code, CODE_N, SEG_BASE, 0x40, 1) == VJO_PLACE_OK);
    site(code, 1, 0x42, 8, bl);
    TEST_CHECK(vjo_place_check(code, CODE_N, SEG_BASE, 0x42, 1) == VJO_PLACE_WINDOW);
    /* Thumb, BL at byte 12 of a 12-byte patch: past it. */
    site(code, 1, 0x42, 12, bl);
    TEST_CHECK(vjo_place_check(code, CODE_N, SEG_BASE, 0x42, 1) == VJO_PLACE_OK);
    /* ARM: the patch is two words; a literal load in the third is not moved. */
    site(code, 0, 0x40, 8, ldr_arm);
    TEST_CHECK(vjo_place_check(code, CODE_N, SEG_BASE, 0x40, 0) == VJO_PLACE_OK);
    site(code, 0, 0x40, 4, ldr_arm);
    TEST_CHECK(vjo_place_check(code, CODE_N, SEG_BASE, 0x40, 0) == VJO_PLACE_WINDOW);
    TEST_CHECK(vjo_place_patch_bytes(SEG_BASE + 0x40, 1) == 8 && vjo_place_patch_bytes(SEG_BASE + 0x42, 1) == 12);
    TEST_CHECK(vjo_place_patch_bytes(SEG_BASE + 0x42, 0) == 8);
}

/* Outside segment 0's code, or too close to its end for taiHEN's 16 saved bytes; misaligned. */
static void test_range_align(void)
{
    uint8_t code[CODE_N];
    site(code, 1, 0x40, 0, NULL);
    TEST_CHECK(vjo_place_check(code, CODE_N, SEG_BASE, 0x40, 1) == VJO_PLACE_OK);
    TEST_CHECK(vjo_place_check(code, CODE_N, SEG_BASE, CODE_N, 1) == VJO_PLACE_RANGE);
    TEST_CHECK(vjo_place_check(code, CODE_N, SEG_BASE, 0xFFFFFFF0u, 1) == VJO_PLACE_RANGE);
    site(code, 1, CODE_N - 16, 0, NULL);
    TEST_CHECK(vjo_place_check(code, CODE_N, SEG_BASE, CODE_N - 16, 1) == VJO_PLACE_OK);
    site(code, 1, CODE_N - 14, 0, NULL);
    TEST_CHECK(vjo_place_check(code, CODE_N, SEG_BASE, CODE_N - 14, 1) == VJO_PLACE_RANGE);
    TEST_CHECK(vjo_place_check(code, 0, SEG_BASE, 0, 1) == VJO_PLACE_RANGE);
    site(code, 1, 0x40, 0, NULL);
    TEST_CHECK(vjo_place_check(code, CODE_N, SEG_BASE, 0x41, 1) == VJO_PLACE_ALIGN);
    site(code, 0, 0x40, 0, NULL);
    TEST_CHECK(vjo_place_check(code, CODE_N, SEG_BASE, 0x40, 0) == VJO_PLACE_OK);
    TEST_CHECK(vjo_place_check(code, CODE_N, SEG_BASE, 0x42, 0) == VJO_PLACE_ALIGN);
    TEST_CHECK(vjo_place_check(code, CODE_N, SEG_BASE + 2, 0x40, 0) == VJO_PLACE_ALIGN);
}

/* Thumb vs ARM: an address with bit 0 or 1 set must hold a Thumb entry; else the bytes decide. */
static void test_isa(void)
{
    static const uint8_t t16[] = {0x10, 0xB5, 0xC0, 0x46}, t32[] = {0x2D, 0xE9, 0xF0, 0x4F},
                         arm[] = {0x10, 0x40, 0x2D, 0xE9}, none[] = {0x70, 0x47, 0x00, 0xBF};
    TEST_CHECK(vjo_place_isa(t16, 4, SEG_BASE + 0x100) == 1);
    TEST_CHECK(vjo_place_isa(t16, 4, SEG_BASE + 0x101) == 1);
    TEST_CHECK(vjo_place_isa(t16, 4, SEG_BASE + 0x102) == 1);
    TEST_CHECK(vjo_place_isa(t32, 4, SEG_BASE + 0x100) == 1);
    TEST_CHECK(vjo_place_isa(arm, 4, SEG_BASE + 0x100) == 0);
    TEST_CHECK(vjo_place_isa(arm, 4, SEG_BASE + 0x101) == -1);
    TEST_CHECK(vjo_place_isa(arm, 4, SEG_BASE + 0x102) == -1);
    TEST_CHECK(vjo_place_isa(none, 4, SEG_BASE + 0x100) == -1);
    TEST_CHECK(vjo_place_isa(t16, 3, SEG_BASE + 0x100) == -1);
}

/* Rubi's own CLANNAD entry: title, module NID and bytes exactly as recorded. */
static void test_verified(void)
{
    static const uint8_t sig[8] = {0xAB, 0xF8, 0x2C, 0x23, 0x14, 0xF5, 0x53, 0x70};
    static uint8_t code[0x30000];
    const VjoPlaceVerified *v;
    TEST_CHECK(vjo_place_verified("PCSG00415", &v) == 1 && v);
    TEST_CHECK(v->module_nid == 0xC5187D60u && v->off == 0x2CA70u && v->thumb == 1 && v->reg == 6);
    TEST_CHECK(v->encoding == VJO_HOOK_ENC_CP932 && v->sig_len == 8 && v->padding == 0);
    TEST_CHECK(vjo_place_title_verified("PCSG00415") == 1);
    TEST_CHECK(vjo_place_title_verified("PCSG00416") == 0);
    TEST_CHECK(vjo_place_verified("PCSG004150", &v) == 0 && !v);
    TEST_CHECK(vjo_place_verified("PCSG0041", &v) == 0);
    TEST_CHECK(vjo_place_verified(NULL, &v) == 0);
    vjo_place_verified("PCSG00415", &v);
    memset(code, 0, sizeof(code));
    memcpy(code + 0x2CA70, sig, sizeof(sig));
    TEST_CHECK(vjo_place_check_verified(v, code, sizeof(code), SEG_BASE, 0xC5187D60u) == VJO_PLACE_OK);
    TEST_CHECK(vjo_place_check_verified(v, code, sizeof(code), SEG_BASE, 0xC5187D61u) == VJO_PLACE_SIGNATURE);
    code[0x2CA77] ^= 1;
    TEST_CHECK(vjo_place_check_verified(v, code, sizeof(code), SEG_BASE, 0xC5187D60u) == VJO_PLACE_SIGNATURE);
    code[0x2CA77] ^= 1;
    TEST_CHECK(vjo_place_check_verified(v, code, 0x2CA70 + 15, SEG_BASE, 0xC5187D60u) == VJO_PLACE_RANGE);
    TEST_CHECK(vjo_place_check_verified(v, code, 0x2CA70, SEG_BASE, 0xC5187D60u) == VJO_PLACE_RANGE);
    /* The same bytes at an address = 2 mod 4 would need a 12-byte patch: 8 recorded bytes are too few. */
    TEST_CHECK(vjo_place_check_verified(v, code, sizeof(code), SEG_BASE + 2, 0xC5187D60u) == VJO_PLACE_ALIGN);
}

/* The CALL prefilter: Japanese lines pass, binary structs and Latin text do not. */
static void test_prefilter(void)
{
    /* CP932 「こんにちは」 and 渚 */
    static const uint8_t cp932[] = {0x81, 0x75, 0x82, 0xB1, 0x82, 0xF1, 0x82, 0xC9, 0x82, 0xBF, 0x82, 0xCD, 0x81, 0x76, 0};
    static const uint8_t cp932_kanji[] = {0x93, 0x48, 0x81, 0x41, 0x8D, 0xA1, 0x93, 0xFA, 0};
    /* UTF-8 こんにちは */
    static const uint8_t utf8[] = {0xE3, 0x81, 0x93, 0xE3, 0x82, 0x93, 0xE3, 0x81, 0xAB, 0xE3, 0x81, 0xA1, 0};
    /* a struct of pointers 0x81xxxxxx and small integers */
    static const uint8_t ptrs[] = {0x10, 0x20, 0x05, 0x81, 0x00, 0x00, 0x00, 0x00, 0x44, 0x33, 0x02, 0x81, 1, 0, 0, 0};
    static const uint8_t ptrs2[] = {0x82, 0xA0, 0x05, 0x81, 0x82, 0xA4, 0x05, 0x81, 0x82, 0xA6, 0x05, 0x81, 0x82, 0xA8, 0x05, 0x81};
    static const uint8_t latin[] = "Hello, world";
    static const uint8_t one_kana[] = {'A', 'B', 0x82, 0xA0, 'C', 0};
    static const uint8_t halfwidth[] = {0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0};
    /* 16 bytes of CP932 text whose 16th byte is a lead: cut by the limit, still text */
    static const uint8_t cut[] = {0x82, 0xA0, 0x82, 0xA2, 0x82, 0xA4, 0x82, 0xA6, 0x82, 0xA8, 0x82, 0xA9,
                                  0x82, 0xAB, 0x41, 0x82, 0xAD, 0};
    static const uint8_t bad_utf8[] = {0xE3, 0x81, 0x41, 0xE3, 0x82, 0x93, 0};
    TEST_CHECK(vjo_place_text_prefilter(cp932, sizeof(cp932)) == 1);
    TEST_CHECK(vjo_place_text_prefilter(cp932_kanji, sizeof(cp932_kanji)) == 1);
    TEST_CHECK(vjo_place_text_prefilter(utf8, sizeof(utf8)) == 1);
    TEST_CHECK(vjo_place_text_prefilter(ptrs, sizeof(ptrs)) == 0);
    TEST_CHECK(vjo_place_text_prefilter(ptrs2, sizeof(ptrs2)) == 0);
    TEST_CHECK(vjo_place_text_prefilter(latin, sizeof(latin)) == 0);
    TEST_CHECK(vjo_place_text_prefilter(one_kana, sizeof(one_kana)) == 0);
    TEST_CHECK(vjo_place_text_prefilter(halfwidth, sizeof(halfwidth)) == 0);
    TEST_CHECK(vjo_place_text_prefilter(cut, sizeof(cut)) == 1);
    TEST_CHECK(vjo_place_text_prefilter(bad_utf8, sizeof(bad_utf8)) == 0);
    TEST_CHECK(vjo_place_text_prefilter(cp932, 3) == 0);
    TEST_CHECK(vjo_place_text_prefilter(NULL, 16) == 0);
    /* a NUL within the first 4 bytes: too short */
    TEST_CHECK(vjo_place_text_prefilter((const uint8_t *)"\x82\xA0\x00\x82\xA2\x82\xA4", 7) == 0);
}

static void test_reasons(void)
{
    TEST_CHECK(!strcmp(vjo_place_reason(VJO_PLACE_OK), "ok"));
    TEST_CHECK(!strcmp(vjo_place_reason(VJO_PLACE_WINDOW), "window"));
    TEST_CHECK(!strcmp(vjo_place_reason(VJO_PLACE_TAI), "tai"));
    TEST_CHECK(!strcmp(vjo_place_reason(-1), "?") && !strcmp(vjo_place_reason(VJO_PLACE_COUNT), "?"));
    TEST_CHECK(vjo_place_hash((const uint8_t *)"", 0) == 2166136261u);
    TEST_CHECK(vjo_place_hash((const uint8_t *)"a", 1) == 0xE40C292Cu);
}

TEST_LIST = {
    {"fixtures_present", test_fixtures_present},
    {"entries", test_entries},
    {"windows", test_windows},
    {"window_size", test_window_size},
    {"range_align", test_range_align},
    {"isa", test_isa},
    {"verified", test_verified},
    {"prefilter", test_prefilter},
    {"reasons", test_reasons},
    {NULL, NULL},
};
