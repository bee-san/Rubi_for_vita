/* Anki for Vita's duplicate index as the overlay reads it (core/anki_index.h, core/anki_index_find.c):
 * the worked example of Anki for Vita's docs/duplicate-index.md byte for byte, the keys of plain words,
 * every word of a large index found and absent ones not, at most three reads, the previous index
 * (rubi.idx.old) when there is no rubi.idx, and every damaged file "unknown" without a read out of
 * bounds. Pure C over BearSSL's SHA-256: CMake also builds it with AddressSanitizer and
 * UndefinedBehaviorSanitizer (vjo-anki-index-asan-tests), and tools/arm_host_tests.sh runs it on the
 * Cortex-A9 under qemu-arm. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "acutest.h"
#include "afv_index_writer.h"
#include "anki_index.h"
#include "bearssl.h"

/* ---------------- files in memory, their reads counted ---------------- */

typedef struct {
    const char *path;
    const uint8_t *data;
    size_t size;
} MemFile;

static MemFile files[2];
static int opens, failed_opens, closes, reads;
static size_t read_bytes, max_read;

static int mem_read(void *ctx, uint64_t off, void *dst, size_t n)
{
    const MemFile *m = ctx;
    reads++;
    read_bytes += n;
    if (n > max_read)
        max_read = n;
    if (off > m->size || n > m->size - (size_t)off)
        return -1;
    if (n)
        memcpy(dst, m->data + off, n);
    return 0;
}

static void mem_close(void *ctx)
{
    (void)ctx;
    closes++;
}

static int mem_open(void *ud, const char *path, VjoFile *f)
{
    (void)ud;
    for (int i = 0; i < 2; i++)
        if (files[i].path && !strcmp(files[i].path, path)) {
            opens++;
            *f = (VjoFile){&files[i], files[i].size, mem_read, mem_close, 0};
            return 0;
        }
    failed_opens++;
    return -1;
}

static const VjoPlatform plat = {NULL, mem_open, NULL, NULL, 0};

static void counters_reset(void)
{
    opens = failed_opens = closes = reads = 0;
    read_bytes = max_read = 0;
}

/* Only rubi.idx (data NULL: none). */
static void use_index(const uint8_t *data, size_t size)
{
    memset(files, 0, sizeof(files));
    if (data)
        files[0] = (MemFile){VJO_ANKI_INDEX_PATH, data, size};
    counters_reset();
}

static int lookup(const char *form, const char *reading)
{
    VjoAnkiIndexKeys k;
    int rc = vjo_anki_index_keys(form, reading, &k);
    return rc ? rc : vjo_anki_index_lookup(&plat, VJO_ANKI_INDEX_PATH, VJO_ANKI_INDEX_OLD_PATH, &k);
}

/* BE64(SHA-256(kind || a (|| 0x1F || b))) */
static uint64_t ref_key(uint8_t kind, const char *a, const char *b)
{
    br_sha256_context c;
    uint8_t d[32];
    uint64_t v = 0;
    br_sha256_init(&c);
    br_sha256_update(&c, &kind, 1);
    br_sha256_update(&c, a, strlen(a));
    if (b) {
        br_sha256_update(&c, "\x1f", 1);
        br_sha256_update(&c, b, strlen(b));
    }
    br_sha256_out(&c, d);
    for (int i = 0; i < 8; i++)
        v = v << 8 | d[i];
    return v;
}

/* ---------------- the document's worked example ---------------- */

/* Anki for Vita's docs/duplicate-index.md, "Worked example": Rubi's request, the notes <b>猫</b> / ネコ
 * and 犬 / (empty), col.mod 1792400000000, built at 1792400001000: 152 bytes. */
static const uint8_t worked_example[152] = {
    0x41, 0x46, 0x56, 0x49, 0x4e, 0x44, 0x45, 0x58, 0x01, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x84, 0x5d, 0x53, 0xa1, 0x01, 0x00, 0x00, 0xe8, 0x87, 0x5d, 0x53, 0xa1, 0x01, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x10, 0x81, 0x44, 0x40, 0x10, 0xe2, 0x0c, 0x05, 0x45, 0x78, 0x70, 0x72, 0x65, 0x73, 0x73, 0x69,
    0x6f, 0x6e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x45, 0x78, 0x70, 0x72, 0x65, 0x73, 0x73, 0x69, 0x6f, 0x6e, 0x52, 0x65, 0x61, 0x64, 0x69, 0x6e,
    0x67, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9e, 0x41, 0x97, 0xf2,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xd5, 0x04, 0x52, 0x5b, 0xfe, 0xcc, 0x9c, 0x71,
    0x28, 0x58, 0x38, 0xbf, 0x5d, 0xfe, 0x6e, 0xc2};

static void test_the_documents_worked_example(void)
{
    static const AfvWord notes[] = {{"猫", "ネコ"}, {"犬", ""}};
    size_t size;
    uint8_t *mine;
    use_index(worked_example, sizeof(worked_example));
    TEST_CHECK(lookup("猫", "ねこ") == VJO_ANKI_INDEX_PAIR);
    TEST_CHECK(lookup("猫", "ネコ") == VJO_ANKI_INDEX_PAIR); /* the reading's katakana folded */
    TEST_CHECK(lookup("犬", "いぬ") == VJO_ANKI_INDEX_FORM);
    TEST_CHECK(lookup("犬", NULL) == VJO_ANKI_INDEX_FORM);
    TEST_CHECK(lookup("猫", "びょう") == VJO_ANKI_INDEX_NONE);
    TEST_CHECK(lookup("猫", NULL) == VJO_ANKI_INDEX_NONE); /* 猫's note has a reading: only the pair is there */
    TEST_CHECK(lookup("鳥", "とり") == VJO_ANKI_INDEX_NONE);
    /* the tests' writer makes the same file (afv_index_writer.h) */
    mine = afv_write(notes, 2, AFV_RUBI_REQUEST_HASH, 1792400000000ull, 1792400001000ull, &size);
    TEST_ASSERT(mine != NULL);
    TEST_CHECK(size == sizeof(worked_example) && !memcmp(mine, worked_example, size));
    free(mine);
}

static void test_known_keys(void)
{
    VjoAnkiIndexKeys k;
    TEST_ASSERT(vjo_anki_index_keys("猫", "ねこ", &k) == 0 && k.has_pair);
    TEST_CHECK(k.form == 0xc26efe5d3e6897eeull && k.pair == 0xc26efe5dbf385828ull); /* the document's */
    TEST_CHECK(k.form == ref_key(1, "猫", NULL));
    TEST_CHECK((uint32_t)k.pair == (uint32_t)(ref_key(2, "猫", "ねこ") >> 32));
}

/* A plain word's keys: those of the normalised bytes (the document's "A client's keys of a word"). */
static void test_plain_words(void)
{
    static const struct {
        const char *form, *reading, *nform, *nreading; /* nreading NULL: no pair key */
    } cases[] = {
        {"猫", "ねこ", "猫", "ねこ"},
        {"  猫\t", " ね こ ", "猫", "ね こ"},
        {"\xC2\xA0猫\xE3\x80\x80犬\n", NULL, "猫 犬", NULL},
        {"ＡＢＣ！～", "ＡＢＣ", "ABC!~", "ABC"},
        {"コーヒー", "コーヒー", "コーヒー", "こーひー"},          /* katakana folded in the reading only */
        {"ヴァイオリン", "ヴァイオリン", "ヴァイオリン", "ゔぁいおりん"},
        {"ヽヾ", "ヽヾヷ", "ヽヾ", "ゝゞヷ"},
        {"à la", "à", "à la", "à"},                           /* C3 A0: its 0xA0 is no space */
        {"ムム", "ムム", "ムム", "むむ"},                      /* E3 83 A0 */
        {"猫", "", "猫", NULL},
        {"猫", "  ", "猫", NULL},
        {"猫", "ね[こ]", "猫", NULL},                          /* a reading with '[': no pair */
        {"R&B", "あ", "R&B", "あ"},                            /* plain: the field holds it as R&amp;B */
        {"\xE3\x83", "\xE3\x83", "\xE3\x83", "\xE3\x83"},      /* a cut sequence, byte for byte */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        VjoAnkiIndexKeys k;
        TEST_ASSERT_(vjo_anki_index_keys(cases[i].form, cases[i].reading, &k) == 0, "%s", cases[i].form);
        TEST_CHECK_(k.form == ref_key(1, cases[i].nform, NULL), "%s: form", cases[i].form);
        TEST_CHECK_(k.has_pair == (cases[i].nreading != NULL), "%s: pair", cases[i].form);
        if (cases[i].nreading)
            TEST_CHECK_(k.pair == ((k.form & 0xFFFFFFFF00000000ull) | ref_key(2, cases[i].nform, cases[i].nreading) >> 32),
                        "%s: pair key", cases[i].form);
    }
    {
        VjoAnkiIndexKeys k;
        TEST_CHECK(vjo_anki_index_keys("漢字[かんじ]", "かんじ", &k) == VJO_ANKI_INDEX_NOKEY); /* furigana */
        TEST_CHECK(vjo_anki_index_keys("", "x", &k) == VJO_ANKI_INDEX_NOKEY);
        TEST_CHECK(vjo_anki_index_keys(" \xE3\x80\x80 ", NULL, &k) == VJO_ANKI_INDEX_NOKEY);
        TEST_CHECK(vjo_anki_index_keys(NULL, NULL, &k) == VJO_ANKI_INDEX_NOKEY);
    }
}

/* ---------------- a large index ---------------- */

static const char *const kanji[] = {"猫", "犬", "日", "本", "語", "学", "生", "先", "年", "食", "飲", "見", "行",
                                    "来", "話", "書", "読", "手", "目", "耳", "山", "川", "空", "海", "雨", "雪",
                                    "春", "夏", "秋", "冬", "朝", "夜", "時", "間", "人", "子", "女", "男"};
static const char *const hira[] = {"あ", "い", "う", "え", "お", "か", "き", "く", "け", "こ", "さ", "し", "す",
                                   "せ", "そ", "た", "ち", "つ", "て", "と", "な", "に", "ぬ", "ね", "の", "は"};

static void make_word(unsigned i, char *form, char *reading, size_t cap)
{
    unsigned v = i;
    form[0] = reading[0] = '\0';
    do {
        strncat(form, kanji[v % 38], cap - strlen(form) - 1);
        strncat(reading, hira[(v + i) % 26], cap - strlen(reading) - 1);
        strncat(reading, hira[(v / 7 + i) % 26], cap - strlen(reading) - 1);
    } while (v /= 38);
}

static void test_every_word_found_absent_ones_not(void)
{
    enum { N = 20000 };
    AfvWord *w = calloc(N, sizeof(AfvWord));
    char (*forms)[96] = calloc(N, 96), (*readings)[96] = calloc(N, 96);
    size_t size;
    uint8_t *idx;
    unsigned wrong = 0, false_pos = 0;
    TEST_ASSERT(w && forms && readings);
    for (unsigned i = 0; i < N; i++) {
        make_word(i, forms[i], readings[i], 96);
        w[i] = (AfvWord){forms[i], i % 4 == 3 ? "" : readings[i]}; /* every fourth note without a reading */
    }
    TEST_ASSERT((idx = afv_write(w, N, AFV_RUBI_REQUEST_HASH, 0, 0, &size)) != NULL);
    use_index(idx, size);
    for (unsigned i = 0; i < N; i++) {
        int rc = lookup(forms[i], readings[i]);
        wrong += rc != (i % 4 == 3 ? VJO_ANKI_INDEX_FORM : VJO_ANKI_INDEX_PAIR);
        rc = lookup(forms[i], "べつ"); /* another reading: only a note without one matches */
        wrong += rc != (i % 4 == 3 ? VJO_ANKI_INDEX_FORM : VJO_ANKI_INDEX_NONE);
    }
    TEST_CHECK(wrong == 0);
    TEST_MSG("%u wrong", wrong);
    for (unsigned i = 0; i < 300000; i++) {
        char form[120], reading[96];
        make_word(N + i, form, reading, 96);
        strncat(form, "x", sizeof(form) - strlen(form) - 1); /* never a note's */
        false_pos += lookup(form, reading) > 0;
    }
    TEST_CHECK(false_pos == 0);
    /* one lookup: one open, three reads, at most 128 + 8 + 512 B */
    counters_reset();
    TEST_CHECK(lookup(forms[8], readings[8]) == VJO_ANKI_INDEX_PAIR);
    TEST_CHECK(opens == 1 && failed_opens == 0 && closes == 1 && reads == 3 && read_bytes <= 648 && max_read <= 512);
    printf("measure: %u notes: %zu B; a lookup %d reads, %zu B; 300,000 absent words: %u found\n", N, size, reads,
           read_bytes, false_pos);
    free(idx);
    free(w);
    free(forms);
    free(readings);
}

/* No rubi.idx: rubi.idx.old (a publish stopped between its renames); neither: missing. A damaged
 * rubi.idx is not replaced by the .old (the file is there: it is the index). */
static void test_the_previous_index(void)
{
    counters_reset();
    memset(files, 0, sizeof(files));
    TEST_CHECK(lookup("猫", "ねこ") == VJO_ANKI_INDEX_MISSING && failed_opens == 2 && closes == 0);
    files[1] = (MemFile){VJO_ANKI_INDEX_OLD_PATH, worked_example, sizeof(worked_example)};
    counters_reset();
    TEST_CHECK(lookup("猫", "ねこ") == VJO_ANKI_INDEX_PAIR && failed_opens == 1 && opens == 1 && closes == 1);
    files[0] = (MemFile){VJO_ANKI_INDEX_PATH, worked_example, 100};
    counters_reset();
    TEST_CHECK(lookup("猫", "ねこ") == VJO_ANKI_INDEX_BAD && opens == 1 && closes == 1);
}

/* ---------------- damaged files ---------------- */

typedef struct {
    char form[96], reading[96];
    int want;
    VjoAnkiIndexKeys keys;
} Word;
static Word words[300];

static int safe(const uint8_t *data, size_t size, int *bad)
{
    int ok = 1;
    *bad = 0;
    use_index(data, size);
    for (unsigned i = 0; i < 300; i++) {
        int rc;
        counters_reset();
        rc = vjo_anki_index_lookup(&plat, VJO_ANKI_INDEX_PATH, VJO_ANKI_INDEX_OLD_PATH, &words[i].keys);
        ok &= rc == VJO_ANKI_INDEX_BAD || rc == VJO_ANKI_INDEX_NONE || rc == words[i].want;
        *bad += rc == VJO_ANKI_INDEX_BAD;
        ok &= reads <= 3 && read_bytes <= 648 && opens == 1 && closes == 1;
    }
    return ok;
}

static uint64_t rng = 88172645463325252ull;
static uint64_t next_rand(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static void test_damaged_files(void)
{
    AfvWord w[250];
    size_t size;
    uint8_t *good, *copy;
    int bad;
    for (unsigned i = 0; i < 300; i++) {
        make_word(i < 250 ? i : 100000 + i, words[i].form, words[i].reading, 96);
        if (i < 250)
            w[i] = (AfvWord){words[i].form, i % 4 == 3 ? "" : words[i].reading};
        words[i].want = i >= 250 ? VJO_ANKI_INDEX_NONE : i % 4 == 3 ? VJO_ANKI_INDEX_FORM : VJO_ANKI_INDEX_PAIR;
        TEST_ASSERT(vjo_anki_index_keys(words[i].form, words[i].reading, &words[i].keys) == 0);
    }
    TEST_ASSERT((good = afv_write(w, 250, AFV_RUBI_REQUEST_HASH, 0, 0, &size)) != NULL);
    TEST_ASSERT((copy = malloc(size + 64)) != NULL);
    TEST_CHECK(safe(good, size, &bad) && bad == 0);
    /* every truncation, and a byte too many */
    for (size_t n = 0; n <= size + 1; n++) {
        if (n == size)
            continue;
        memcpy(copy, good, size);
        copy[size] = 0;
        TEST_CHECK_(safe(copy, n, &bad) && bad == 300, "%zu bytes: %d unknown", n, bad);
    }
    /* every byte of the header (magic, version, sizes, counts, bits, the check) */
    for (size_t at = 0; at < VJO_ANKI_INDEX_HEADER; at++) {
        memcpy(copy, good, size);
        copy[at] ^= 0x01;
        /* bytes the reader does not use (times, counts, names, the request) change the check */
        TEST_CHECK_(safe(copy, size, &bad) && bad == 300, "header byte %zu: %d unknown", at, bad);
    }
    /* every bit of the directory and the keys: never a false match */
    for (size_t at = VJO_ANKI_INDEX_HEADER; at < size; at++)
        for (int bit = 0; bit < 8; bit++) {
            memcpy(copy, good, size);
            copy[at] ^= (uint8_t)(1u << bit);
            TEST_CHECK_(safe(copy, size, &bad), "byte %zu bit %d", at, bit);
        }
    /* random damage, random files, a good header over random bytes */
    for (int t = 0; t < 3000; t++) {
        size_t n = size;
        memcpy(copy, good, size);
        if (t % 3 == 0) {
            for (int i = 0, k = 1 + (int)(next_rand() % 8); i < k; i++)
                copy[next_rand() % size] ^= (uint8_t)(1 + next_rand() % 255);
        } else if (t % 3 == 1) {
            for (size_t i = 0; i < size; i++)
                copy[i] = (uint8_t)next_rand();
            n = next_rand() % (size + 64);
        } else {
            for (size_t i = VJO_ANKI_INDEX_HEADER; i < size; i++)
                copy[i] = (uint8_t)next_rand();
        }
        TEST_CHECK_(safe(copy, n, &bad), "random damage %d", t);
    }
    free(copy);
    free(good);
}

TEST_LIST = {
    {"the_documents_worked_example", test_the_documents_worked_example},
    {"known_keys", test_known_keys},
    {"plain_words", test_plain_words},
    {"every_word_found_absent_ones_not", test_every_word_found_absent_ones_not},
    {"the_previous_index_when_there_is_no_rubi_idx", test_the_previous_index},
    {"damaged_files_are_unknown_never_read_out_of_bounds", test_damaged_files},
    {NULL, NULL}};
