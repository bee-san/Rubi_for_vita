/* MAGES/5pb. SC3 script text (core/hook_sc3.c, docs/native-hooks.md "SC3 text"): the span a hit keeps (through the
 * 0xFF end token, past the 0x00 and 0xFF bytes its glyphs and controls hold), the decoder (names, line breaks, ruby's
 * base, unknown glyphs dropped, the cap), the committed charsets (third_party/sc3-charsets) against their pins and
 * Rubi's verified entries, and the farm's own pages through both (host/tests/fixtures/hook_sc3_farm.inc). Pure C:
 * also on the Vita's CPU under qemu-arm (tools/arm_host_tests.sh). */
#include <stdio.h>
#include <string.h>

#include "acutest.h"
#include "bearssl_hash.h"
#include "hook_place.h"
#include "hook_sc3.h"
#include "hook_text.h"

#include "fixtures/hook_sc3_farm.inc"

#ifndef VJO_SOURCE_DIR
#error "VJO_SOURCE_DIR: the checkout, for its third_party/sc3-charsets"
#endif

typedef struct {
    const char *title;
    const char *raw; /* the bytes the farm's hook read at r0, through the end token */
    unsigned n;
    const char *text; /* the farm's verified text, ruby's base only */
} FarmRow;
static const FarmRow farm_rows[] = {HOOK_SC3_FARM_ROWS};
_Static_assert(sizeof farm_rows / sizeof farm_rows[0] == HOOK_SC3_FARM_FIXTURES, "every excerpt");

static uint8_t files[3][VJO_SC3_MAX_BYTES];
static const char *const titles[3] = {"PCSG00146", "PCSG00352", "PCSG00555"};

/* The committed charset file of title (its bytes in files[]), its size; 0 when missing. */
static size_t charset(const char *title, const uint8_t **out)
{
    char path[512];
    FILE *f;
    size_t n;
    unsigned k = 0;
    while (k < 3 && strcmp(titles[k], title)) k++;
    TEST_ASSERT_(k < 3, "%s: one of the SC3 titles", title);
    snprintf(path, sizeof path, "%s/third_party/sc3-charsets/%s.sc3", VJO_SOURCE_DIR, title);
    f = fopen(path, "rb");
    TEST_ASSERT_(f != NULL, "%s", path);
    n = fread(files[k], 1, sizeof files[k], f);
    TEST_CHECK(fgetc(f) == EOF);
    fclose(f);
    *out = files[k];
    return n;
}

static unsigned get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

/* Each committed file is its pin's (size and SHA-256) and names its title, the module NID of Rubi's verified entry
 * and its glyph count; every SC3 entry has a pin and each pin an SC3 entry: the r0 of a function's entry, its bytes
 * read through the end token, decoded with no filter. */
static void charsets_and_entries(void)
{
    const VjoSc3Pin *pins;
    const VjoPlaceVerified *all;
    unsigned np = vjo_sc3_pins(&pins), nv = vjo_place_verified_all(&all), sc3 = 0;
    TEST_CHECK(np == 3);
    for (unsigned i = 0; i < np; i++) {
        const VjoPlaceVerified *v;
        const uint8_t *file;
        br_sha256_context hash;
        uint8_t digest[32];
        size_t n = charset(pins[i].title, &file);
        unsigned nver = vjo_place_verified(pins[i].title, &v);
        TEST_CHECK_(vjo_sc3_pin(pins[i].title) == &pins[i], "%s", pins[i].title);
        TEST_CHECK_(n == pins[i].bytes && n >= VJO_SC3_HEADER && n <= VJO_SC3_MAX_BYTES, "%s: %u B", pins[i].title,
                    (unsigned)n);
        br_sha256_init(&hash);
        br_sha256_update(&hash, file, n);
        br_sha256_out(&hash, digest);
        TEST_CHECK_(!memcmp(digest, pins[i].sha256, 32), "%s: the pin's SHA-256", pins[i].title);
        TEST_CHECK(!memcmp(file, "vjosc3\0\1", 8) && !memcmp(file + 8, pins[i].title, 9) && !file[17] && !file[18] &&
                   !file[19] && !get32(file + 28));
        TEST_CHECK_(get32(file + 24) == (n - VJO_SC3_HEADER) / 2u, "%s: its glyph count", pins[i].title);
        TEST_ASSERT_(nver == 1, "%s: one verified entry", pins[i].title);
        TEST_CHECK_(get32(file + 20) == v->module_nid, "%s: the entry's module NID %08X", pins[i].title,
                    (unsigned)v->module_nid);
    }
    for (unsigned i = 0; i < nv; i++) {
        if (all[i].encoding != VJO_HOOK_ENC_SC3 && all[i].read != VJO_PLACE_READ_SC3) continue;
        sc3++;
        TEST_CHECK_(all[i].encoding == VJO_HOOK_ENC_SC3 && all[i].read == VJO_PLACE_READ_SC3 && !all[i].filter &&
                    all[i].reg <= 12 && vjo_sc3_pin(all[i].title), "%s: SC3 read, encoding, no filter, a pin",
                    all[i].title);
    }
    TEST_CHECK(sc3 == np);
    TEST_CHECK(!vjo_sc3_pin("PCSG0014") && !vjo_sc3_pin("PCSG001460") && !vjo_sc3_pin("PCSG00415") &&
               !vjo_sc3_pin(NULL));
}

/* The farm's pages: the span of the hook's read (its 512 bytes there hold more after the end token) is the
 * string's, a read without its end token keeps nothing, and the decoder gives the farm's text. */
static void farm_excerpts(void)
{
    unsigned seen[3] = {0};
    for (unsigned k = 0; k < HOOK_SC3_FARM_FIXTURES; k++) {
        const FarmRow *row = &farm_rows[k];
        const uint8_t *file;
        size_t n = charset(row->title, &file);
        uint8_t raw[VJO_SC3_MAX_BYTES];
        char out[1024];
        size_t len;
        for (unsigned t = 0; t < 3; t++) seen[t] += !strcmp(titles[t], row->title);
        TEST_ASSERT(row->n >= 2 && row->n + 64u <= sizeof raw && (uint8_t)row->raw[row->n - 1] == 0xFF);
        memcpy(raw, row->raw, row->n);
        memset(raw + row->n, 0x00, 32); /* the next string's bytes, or zeros */
        memcpy(raw + row->n + 32, "\x01\x82\xD2\x02\xFF", 5);
        TEST_CHECK_(vjo_sc3_span(raw, row->n + 37) == row->n, "%s excerpt %u: its span", row->title, k);
        TEST_CHECK(vjo_sc3_span(raw, row->n - 1) == 0);
        len = vjo_sc3_decode(file + VJO_SC3_HEADER, (unsigned)(n - VJO_SC3_HEADER) / 2u, raw, row->n + 37, out,
                             sizeof out);
        TEST_CHECK_(len == strlen(row->text) && !strcmp(out, row->text), "%s excerpt %u", row->title, k);
        TEST_MSG("got  [%s]", out);
        TEST_MSG("want [%s]", row->text);
    }
    for (unsigned t = 0; t < 3; t++) TEST_CHECK_(seen[t] >= 4, "%s: %u excerpts", titles[t], seen[t]);
}

/* A table of glyphs 0..5 and 0x100, 0x1FF: 0 'A', 1 'B' (U+3042 あ), 2 none, 3 a surrogate, 4 'é', 5 ' ', ... */
static uint8_t table[2 * 0x200];
static void set_glyph(unsigned g, unsigned cp)
{
    table[2 * g] = (uint8_t)cp;
    table[2 * g + 1] = (uint8_t)(cp >> 8);
}
static void make_table(void)
{
    memset(table, 0, sizeof table);
    set_glyph(0, 'A');
    set_glyph(1, 0x3042);   /* あ */
    set_glyph(3, 0xD800);   /* a surrogate: never written */
    set_glyph(4, 0xE9);     /* é, two bytes */
    set_glyph(0x100, 0x6F22); /* 漢: a glyph whose low byte is 0x00 */
    set_glyph(0x1FF, 0x5B57); /* 字: one whose low byte is 0xFF */
}

static size_t dec(const char *src, size_t n, char *out, size_t cap)
{
    return vjo_sc3_decode(table, 0x200, (const uint8_t *)src, n, out, cap);
}

#define DECODES(src, want)                                                                         \
    do {                                                                                           \
        char out_[128];                                                                            \
        size_t got_ = dec(src, sizeof(src) - 1, out_, sizeof out_);                                \
        TEST_CHECK_(got_ == strlen(want) && !strcmp(out_, want), "%s", #src);                      \
        TEST_MSG("want [%s], got [%s]", want, out_);                                               \
    } while (0)

#define SPAN(src) vjo_sc3_span((const uint8_t *)(src), sizeof(src) - 1)

static void span_tokens(void)
{
    /* glyphs whose second byte is 0x00 or 0xFF, a 0x00 line break: the end is the first 0xFF token */
    TEST_CHECK(SPAN("\x81\x00\x00\x81\xFF\xFF" "\x80\x01\xFF") == 6);
    TEST_CHECK(SPAN("\xFF") == 1);
    /* two-byte arguments that hold 0xFF and 0x00 */
    TEST_CHECK(SPAN("\x11\xFF\x00\x12\x00\xFF\x0C\xFF\xFF\x13\x80\xFF\x80\x01\xFF") == 15);
    /* expressions: two-byte tokens, and longer ones (bit 7: 2 + ((t & 0x60) >> 5) bytes), to their 0x00 */
    TEST_CHECK(SPAN("\x04\x28\xFF\x45\x14\x00\x80\x00\xFF") == 9);
    TEST_CHECK(SPAN("\x15\x80\xFF\xA0\xFF\xFF\xC0\xFF\xFF\xFF\xE0\xFF\xFF\xFF\xFF\x00\xFF") == 17);
    /* none: no end token, or a token cut by n */
    TEST_CHECK(SPAN("\x01\x80\x00\x02\x00") == 0);
    TEST_CHECK(SPAN("\x80") == 0 && SPAN("\x11\xFF") == 0 && SPAN("\x04\x28\xFF") == 0 &&
               SPAN("\x04\xE0\xFF\x00") == 0);
    TEST_CHECK(SPAN("") == 0 && vjo_sc3_span(NULL, 8) == 0);
    /* n bounds every read: a string whose end token is past n has none */
    TEST_CHECK(vjo_sc3_span((const uint8_t *)"\x80\x01\xFF", 2) == 0);
}

static void decode_rules(void)
{
    make_table();
    /* a name on its own line, a 0x00 line break, glyphs whose low byte is 0x00 or 0xFF */
    DECODES("\x01\x80\x01\x02\x80\x00\x00\x81\x00\x81\xFF\xFF", "あ\nA\n漢字");
    /* a 0x02 without a name is no line; other controls draw nothing, their arguments and expressions included */
    DECODES("\x02\x80\x00\x03\x08\x11\x80\x01\x04\x80\x01\x00\x80\x04\x1E\xFF", "Aé");
    /* ruby: the base, not the reading */
    DECODES("\x80\x00\x09\x81\x00\x0A\x80\x01\x80\x01\x0B\x80\x04\xFF", "A漢é");
    /* a glyph the table cannot name (0, past its count, a surrogate) is dropped, never guessed */
    DECODES("\x80\x02\x80\x00\x80\x03\xFE\xFF\x80\x01\xFF", "Aあ");
    /* the end token ends the text; a token cut by n ends it too */
    DECODES("\x80\x00\xFF\x80\x01", "A");
    DECODES("\x80\x00\x80", "A");
    DECODES("\x80\x00\x04\x28", "A");
    /* the cap: whole characters only, and the NUL always written */
    {
        char out[8];
        memset(out, 'x', sizeof out);
        TEST_CHECK(dec("\x80\x01\x80\x01\x80\x01\xFF", 7, out, 7) == 6 && !strcmp(out, "ああ"));
        TEST_CHECK(dec("\x80\x01\xFF", 3, out, 3) == 0 && !out[0]);
        TEST_CHECK(dec("\x80\x00\xFF", 3, out, 1) == 0 && !out[0]);
        TEST_CHECK(dec("\x80\x00\xFF", 3, NULL, 8) == 0 && dec("\x80\x00\xFF", 3, out, 0) == 0);
        TEST_CHECK(vjo_sc3_decode(table, 0x200, NULL, 4, out, sizeof out) == 0 && !out[0]);
    }
    /* no table: no glyph at all, the controls' line breaks only */
    {
        char out[16];
        TEST_CHECK(vjo_sc3_decode(NULL, 0, (const uint8_t *)"\x80\x00\x00\x80\x01\xFF", 6, out, sizeof out) == 1 &&
                   !strcmp(out, "\n"));
    }
}

TEST_LIST = {
    {"the committed charsets: their pins, headers and Rubi's SC3 entries", charsets_and_entries},
    {"the farm's pages: the span of a 512-byte read, the decoded text", farm_excerpts},
    {"span: through the end token, past 0x00 and 0xFF in glyphs, arguments and expressions", span_tokens},
    {"decode: names, line breaks, ruby's base, unknown glyphs dropped, the cap", decode_rules},
    {NULL, NULL},
};
