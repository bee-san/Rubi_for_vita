/* Generated, small VJDICT1/2 fixtures exercise the production file callbacks,
 * deinflector and selected-entry formatter without copyrighted dictionary data. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "acutest.h"
#include "client.h"
#include "local_dict.h"

typedef struct {
    const char *key, *spelling, *reading, *meaning;
    uint16_t rules;
} Row;
typedef struct {
    const char *name;
    uint8_t bytes[32768];
    size_t size;
    uint64_t payload[16];
    uint32_t payload_size[16];
    int n_payload, active;
} Fixture;
static Fixture fixtures[2];
static uint8_t arena_mem[64 * 1024];
static int opened, closed, reads, payload_reads, fail_read;

static void put16(uint8_t *p, uint16_t x) { p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8); }
static void put32(uint8_t *p, uint32_t x) { put16(p, (uint16_t)x); put16(p + 2, (uint16_t)(x >> 16)); }
static void put64(uint8_t *p, uint64_t x) { put32(p, (uint32_t)x); put32(p + 4, (uint32_t)(x >> 32)); }
static uint64_t key_hash(const char *s)
{
    uint64_t h = UINT64_C(14695981039346656037);
    for (; *s; s++) h = (h ^ (uint8_t)*s) * UINT64_C(1099511628211);
    h ^= h >> 33; h *= UINT64_C(0xff51afd7ed558ccd);
    h ^= h >> 33; h *= UINT64_C(0xc4ceb9fe1a85ec53);
    return h ^ (h >> 33);
}
static size_t add_payload(Fixture *f, const Row *r, size_t off)
{
    size_t start = off;
    const char *parts[] = {r->spelling, r->reading, r->meaning};
    for (int i = 0; i < 3; i++) {
        size_t n = strlen(parts[i]) + 1;
        TEST_ASSERT(off + n <= sizeof(f->bytes));
        memcpy(f->bytes + off, parts[i], n); off += n;
    }
    f->payload[f->n_payload] = start;
    f->payload_size[f->n_payload++] = (uint32_t)(off - start);
    return off;
}
static void build(Fixture *f, const char *name, const Row *rows, int n, int version)
{
    int order[16];
    size_t off;
    memset(f, 0, sizeof(*f)); f->name = name;
    TEST_ASSERT(n > 0 && n <= 16);
    /* Stable sorting preserves first posting priority for identical keys. */
    for (int i = 0; i < n; i++) {
        int j = i;
        order[i] = i;
        while (j > 0) {
            const char *a = rows[order[j - 1]].key, *b = rows[order[j]].key;
            int greater = version == 1 ? strcmp(a, b) > 0 :
                key_hash(a) > key_hash(b) || (key_hash(a) == key_hash(b) && strcmp(a, b) > 0);
            if (!greater) break;
            int tmp = order[j - 1]; order[j - 1] = order[j]; order[j] = tmp; j--;
        }
    }
    memcpy(f->bytes, version == 1 ? "VJDICT1\0" : "VJDICT2\0", 8);
    put32(f->bytes + 8, (uint32_t)version); put32(f->bytes + 12, 64);
    put32(f->bytes + 16, (uint32_t)n); put32(f->bytes + 20, 24);
    if (version == 1) {
        off = 64 + (size_t)n * 24;
        put64(f->bytes + 24, 64); put64(f->bytes + 32, off);
        for (int i = 0; i < n; i++) {
            const Row *r = &rows[order[i]];
            uint8_t *record = f->bytes + 64 + i * 24;
            size_t key_len = strlen(r->key), payload;
            put64(record, off); put16(record + 16, (uint16_t)key_len);
            memcpy(f->bytes + off, r->key, key_len); off += key_len;
            payload = off; off = add_payload(f, r, off);
            put64(record + 8, payload); put16(record + 18, r->rules);
            put32(record + 20, (uint32_t)(off - payload));
        }
    } else {
        unsigned groups = 0;
        off = 8192;
        put64(f->bytes + 24, 4096); put64(f->bytes + 32, off);
        put64(f->bytes + 48, 1); put64(f->bytes + 56, 1);
        memcpy(f->bytes + 4096, "VJHP", 4);
        for (int i = 0; i < n;) {
            int end = i + 1;
            const Row *r = &rows[order[i]];
            size_t key_len = strlen(r->key), group = off, posting;
            uint16_t rules = r->rules;
            uint8_t *slot = f->bytes + 4096 + 16 + groups * 24;
            while (end < n && !strcmp(r->key, rows[order[end]].key)) {
                rules |= rows[order[end]].rules; end++;
            }
            put64(slot, key_hash(r->key)); put64(slot + 8, group);
            put32(slot + 16, (uint32_t)(end - i));
            put16(slot + 20, (uint16_t)key_len); put16(slot + 22, rules);
            memcpy(f->bytes + off, r->key, key_len); off += key_len;
            posting = off; off += (size_t)(end - i) * 16;
            for (int j = i; j < end; j++) {
                uint8_t *post = f->bytes + posting + (j - i) * 16;
                size_t payload = off;
                off = add_payload(f, &rows[order[j]], off);
                put64(post, payload); put32(post + 8, (uint32_t)(off - payload));
                put16(post + 12, rows[order[j]].rules);
            }
            groups++; i = end;
        }
        put32(f->bytes + 4100, groups);
    }
    f->size = off; put64(f->bytes + 40, off);
}
static int read_file(void *ctx, uint64_t off, void *dst, size_t n)
{
    Fixture *f = ctx;
    reads++;
    TEST_CHECK(f->active > 0);
    if (fail_read == reads) return -1;
    if (off > f->size || n > f->size - (size_t)off) return -1;
    for (int i = 0; i < f->n_payload; i++)
        if (off == f->payload[i] && n == f->payload_size[i]) payload_reads++;
    memcpy(dst, f->bytes + (size_t)off, n); return 0;
}
static void close_file(void *ctx)
{
    Fixture *f = ctx;
    TEST_CHECK(f->active > 0); f->active--; closed++;
}
static int open_file(void *ud, const char *path, VjoFile *out)
{
    (void)ud;
    const char *name = strrchr(path, '/'); name = name ? name + 1 : path;
    for (int i = 0; i < 2; i++) if (fixtures[i].name && !strcmp(name, fixtures[i].name)) {
        Fixture *f = &fixtures[i];
        f->active++; opened++;
        *out = (VjoFile){f, f->size, read_file, close_file}; return 0;
    }
    return -1;
}
static void setup(VjoArena *a, VjoPlatform *p, VjoConfig *cfg)
{
    memset(fixtures, 0, sizeof(fixtures));
    opened = closed = reads = payload_reads = fail_read = 0;
    vjo_arena_init(a, arena_mem, sizeof(arena_mem));
    memset(p, 0, sizeof(*p)); p->file_open = open_file;
    vjo_config_defaults(cfg); cfg->dictionary = VJO_DICT_LOCAL;
    strcpy(cfg->local_dictionary_dir, "fixture"); strcpy(cfg->local_dictionaries, "main.vjdict");
}
static int lookup(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                   const char *text, size_t cursor, VjoDictResult *r, VjoErr *e)
{
    int rc = vjo_local_lookup_at(a, p, cfg, text, cursor, r, e);
    TEST_CHECK(opened == closed);
    TEST_CHECK(r->n_vocab >= 0 && r->n_vocab <= 1 && r->n_tokens == r->n_vocab);
    TEST_CHECK(e->rc == rc && e->dict == VJO_DICT_LOCAL);
    return rc;
}
static void longest_and_first_posting(void)
{
    const Row rows[] = {{"猫", "猫", "ねこ", "cat", 0},
        {"猫舌", "猫舌", "ねこじた", "first long meaning", 0},
        {"猫舌", "猫舌", "ねこじた", "other long meaning", 0}};
    for (int version = 1; version <= 2; version++) {
        VjoArena a; VjoPlatform p; VjoConfig cfg; VjoDictResult r; VjoErr e;
        setup(&a, &p, &cfg); build(&fixtures[0], "main.vjdict", rows, 3, version);
        TEST_ASSERT(!lookup(&a, &p, &cfg, "猫舌。猫", 0, &r, &e));
        TEST_ASSERT(r.n_vocab == 1);
        TEST_CHECK(!strcmp(r.vocab[0].meanings[0], "first long meaning"));
        TEST_CHECK(r.tokens[0].pos16 == 0 && r.tokens[0].len16 == 2);
        TEST_CHECK(payload_reads == 1 && a.peak <= 64 * 1024);
    }
}
static void inflection_and_dictionary_priority(void)
{
    const Row first[] = {{"食べる", "食べる", "たべる", "first inflection", 1},
        {"猫", "猫", "ねこ", "first dictionary", 0}};
    const Row second[] = {{"食べた", "食べた", "たべた", "exact later dictionary", 0},
        {"猫", "猫", "ねこ", "second dictionary", 0}};
    for (int version = 1; version <= 2; version++) {
        VjoArena a; VjoPlatform p; VjoConfig cfg; VjoDictResult r; VjoErr e;
        setup(&a, &p, &cfg); build(&fixtures[0], "main.vjdict", first, 2, version);
        TEST_ASSERT(!lookup(&a, &p, &cfg, "食べませんでした。", 0, &r, &e));
        TEST_ASSERT(r.n_vocab == 1);
        TEST_CHECK(!strcmp(r.vocab[0].spelling, "食べる") && r.tokens[0].len16 == 8);
        build(&fixtures[1], "extra.vjdict", second, 2, version);
        strcpy(cfg.local_dictionaries, "main.vjdict,extra.vjdict");
        vjo_arena_reset(&a);
        TEST_ASSERT(!lookup(&a, &p, &cfg, "食べた", 0, &r, &e));
        TEST_ASSERT(r.n_vocab == 1);
        TEST_CHECK(!strcmp(r.vocab[0].meanings[0], "exact later dictionary"));
        vjo_arena_reset(&a);
        TEST_ASSERT(!lookup(&a, &p, &cfg, "猫", 0, &r, &e));
        TEST_CHECK(!strcmp(r.vocab[0].meanings[0], "first dictionary"));
    }
}
static void cursor_boundary_and_no_later_word(void)
{
    const Row rows[] = {{"猫", "猫", "ねこ", "cat", 0}, {"犬", "犬", "いぬ", "dog", 0}};
    VjoArena a; VjoPlatform p; VjoConfig cfg; VjoDictResult r; VjoErr e;
    setup(&a, &p, &cfg); build(&fixtures[0], "main.vjdict", rows, 2, 2);
    TEST_ASSERT(!lookup(&a, &p, &cfg, "X猫", 0, &r, &e)); TEST_CHECK(r.n_vocab == 0);
    TEST_ASSERT(!lookup(&a, &p, &cfg, "X猫", 1, &r, &e)); TEST_CHECK(r.n_vocab == 1);
    vjo_arena_reset(&a);
    TEST_ASSERT(!lookup(&a, &p, &cfg, "\x80" "猫", 1, &r, &e));
    TEST_CHECK(r.n_vocab == 1 && r.tokens[0].pos16 == 1);
    vjo_arena_reset(&a);
    int before = opened;
    TEST_CHECK(lookup(&a, &p, &cfg, "😀猫", 1, &r, &e) == VJO_E_PARSE);
    TEST_CHECK(lookup(&a, &p, &cfg, "😀猫", 2, &r, &e) == VJO_E_PARSE);
    TEST_CHECK(lookup(&a, &p, &cfg, "😀猫", 3, &r, &e) == VJO_E_PARSE);
    TEST_CHECK(lookup(&a, &p, &cfg, "猫", 4, &r, &e) == VJO_E_PARSE);
    TEST_ASSERT(!lookup(&a, &p, &cfg, "猫", 3, &r, &e));
    TEST_ASSERT(!lookup(&a, &p, &cfg, "。猫", 0, &r, &e));
    TEST_ASSERT(!lookup(&a, &p, &cfg, "\n猫", 0, &r, &e));
    TEST_CHECK(opened == before && a.used == 0);
}
static void raw_newline_and_supplementary_highlights(void)
{
    const Row rows[] = {{"猫犬", "猫犬", "", "must not cross newline", 0},
        {"猫", "猫", "ねこ", "cat", 0}, {"犬", "犬", "いぬ", "dog", 0},
        {"𠮷猫", "𠮷猫", "", "supplementary term", 0}};
    for (int version = 1; version <= 2; version++) {
        VjoArena a; VjoPlatform p; VjoConfig cfg; VjoOverlayData out;
        setup(&a, &p, &cfg); build(&fixtures[0], "main.vjdict", rows, 4, version);
        const char *text = "😀\r\n猫\n犬。𠮷猫";
        out = (VjoOverlayData){.ocr_text = text, .filtered = text, .sentence = text};
        TEST_ASSERT(!vjo_overlay_lookup_at(&a, &p, &cfg, 6, &out));
        TEST_ASSERT(out.list.n_entries == 1);
        TEST_CHECK(out.list.header == text && out.ocr_text == text && out.filtered == text);
        TEST_CHECK(out.list.entries[0].pos16 == 4);
        TEST_CHECK(out.list.entries[0].hl_start == 6 && out.list.entries[0].hl_end == 9);
        vjo_arena_reset(&a);
        TEST_ASSERT(!vjo_overlay_lookup_at(&a, &p, &cfg, 10, &out));
        TEST_CHECK(out.list.entries[0].pos16 == 6);
        TEST_CHECK(out.list.entries[0].hl_start == 10 && out.list.entries[0].hl_end == 13);
        vjo_arena_reset(&a);
        TEST_ASSERT(!vjo_overlay_lookup_at(&a, &p, &cfg, 16, &out));
        TEST_CHECK(out.list.entries[0].pos16 == 8);
        TEST_CHECK(out.list.entries[0].hl_start == 16 && out.list.entries[0].hl_end == 23);
        TEST_CHECK(opened == closed);
    }
}
static void punctuation_and_read_counts_do_not_scan_remainder(void)
{
    const Row rows[] = {{"猫", "猫", "ねこ", "cat", 0}, {"犬", "犬", "いぬ", "dog", 0},
        {"猫犬", "猫犬", "", "longer", 0}};
    for (int version = 1; version <= 2; version++) {
        VjoArena a; VjoPlatform p; VjoConfig cfg; VjoDictResult r; VjoErr e;
        char long_text[4096];
        setup(&a, &p, &cfg); build(&fixtures[0], "main.vjdict", rows, 3, version);
        TEST_ASSERT(!lookup(&a, &p, &cfg, "猫。", 0, &r, &e));
        int short_reads = reads;
        memcpy(long_text, "猫。", 6); memset(long_text + 6, 'X', sizeof(long_text) - 7);
        long_text[sizeof(long_text) - 1] = 0;
        vjo_arena_reset(&a); reads = payload_reads = 0;
        TEST_ASSERT(!lookup(&a, &p, &cfg, long_text, 0, &r, &e));
        TEST_CHECK(reads == short_reads && payload_reads == 1);
        TEST_CHECK(r.tokens[0].len16 == 1);
        vjo_arena_reset(&a);
        TEST_ASSERT(!lookup(&a, &p, &cfg, "猫\n犬", 0, &r, &e));
        TEST_CHECK(r.tokens[0].len16 == 1);
    }
}
static void largest_entry_and_input_fit_64k(void)
{
    char meaning[8192], text[4096];
    /* 猫 + NUL, empty reading + NUL, meaning + NUL totals exactly 8192. */
    memset(meaning, 'M', 8186); meaning[8186] = 0;
    memcpy(text, "猫。", 6); memset(text + 6, 'X', sizeof(text) - 7); text[4095] = 0;
    const Row row = {"猫", "猫", "", meaning, 0};
    for (int version = 1; version <= 2; version++) {
        VjoArena a; VjoPlatform p; VjoConfig cfg; VjoOverlayData out;
        setup(&a, &p, &cfg); build(&fixtures[0], "main.vjdict", &row, 1, version);
        TEST_ASSERT(fixtures[0].payload_size[0] == VJO_LOCAL_MAX_ENTRY);
        out = (VjoOverlayData){.ocr_text = text, .filtered = text, .sentence = text};
        TEST_ASSERT(!vjo_overlay_lookup_at(&a, &p, &cfg, 0, &out));
        TEST_ASSERT(out.list.n_entries == 1);
        TEST_CHECK(strlen(out.list.entries[0].vocab->meanings[0]) == 8186);
        TEST_CHECK(strstr(out.list.entries[0].text, meaning) != NULL);
        size_t static_peak = a.peak;
        vjo_arena_reset(&a); a.peak = 0; cfg.non_japanese_filter = 0;
        TEST_ASSERT(!vjo_overlay_ocr_text(&a, &cfg, text, &out));
        TEST_ASSERT(strlen(out.sentence) == 4095);
        const char *sentence = out.sentence;
        TEST_ASSERT(!vjo_overlay_lookup_at(&a, &p, &cfg, 0, &out));
        TEST_CHECK(out.list.header == sentence && out.list.n_entries == 1);
        TEST_CHECK(strstr(out.list.entries[0].text, meaning) != NULL);
        TEST_CHECK(a.peak <= sizeof(arena_mem) && opened == closed);
        TEST_MSG("version=%d static_peak=%zu copied_text_peak=%zu final=%zu",
                  version, static_peak, a.peak, a.used);
        printf("  VJDICT%d selected peak: static=%zu copied4095=%zu final=%zu\n",
               version, static_peak, a.peak, a.used);
    }
}
static void repeated_lookups_oom_and_io_recovery(void)
{
    const Row row = {"猫", "猫", "ねこ", "cat", 0};
    VjoArena a; VjoPlatform p; VjoConfig cfg; VjoOverlayData out;
    setup(&a, &p, &cfg); build(&fixtures[0], "main.vjdict", &row, 1, 2);
    TEST_ASSERT(!vjo_overlay_ocr_text(&a, &cfg, "猫。X", &out));
    const char *header = out.sentence;
    size_t mark = a.used;
    for (int i = 0; i < 100; i++) {
        vjo_arena_release(&a, mark);
        TEST_ASSERT(!vjo_overlay_lookup_at(&a, &p, &cfg, 0, &out));
        TEST_CHECK(out.list.n_entries == 1 && out.list.header == header);
        vjo_arena_release(&a, mark);
        TEST_ASSERT(!vjo_overlay_lookup_at(&a, &p, &cfg, 6, &out));
        TEST_CHECK(!out.list.n_entries && out.list.header == header && a.used == mark);
    }
    size_t normal_size = a.size;
    a.size = mark + 128;
    TEST_CHECK(vjo_overlay_lookup_at(&a, &p, &cfg, 0, &out) == VJO_E_OOM);
    TEST_CHECK(a.used == mark && out.failed_stage == VJO_STAGE_DICT && out.err.dict == VJO_DICT_LOCAL);
    TEST_CHECK(out.list.header == header && out.list.entries == NULL && !out.list.n_entries);
    a.size = normal_size; fail_read = reads + 1;
    TEST_CHECK(vjo_overlay_lookup_at(&a, &p, &cfg, 0, &out) == VJO_E_LOCAL_IO);
    TEST_CHECK(a.used == mark && out.list.header == header && opened == closed);
    fail_read = 0;
    TEST_ASSERT(!vjo_overlay_lookup_at(&a, &p, &cfg, 0, &out));
    TEST_CHECK(out.failed_stage == VJO_STAGE_NONE && !out.err.rc && out.list.n_entries == 1);
    vjo_arena_release(&a, mark);
    strcpy(cfg.local_dictionaries, "main.vjdict,missing.vjdict");
    TEST_CHECK(vjo_overlay_lookup_at(&a, &p, &cfg, 0, &out) == VJO_E_LOCAL_IO);
    TEST_CHECK(opened == closed && a.used == mark && !out.list.n_entries);
    strcpy(cfg.local_dictionaries, "main.vjdict");
    fixtures[0].bytes[0] = 'X';
    TEST_CHECK(vjo_overlay_lookup_at(&a, &p, &cfg, 0, &out) == VJO_E_LOCAL_FORMAT);
    TEST_CHECK(opened == closed && a.used == mark && !out.list.n_entries);
}
static void normalized_source_span_and_invalid_input(void)
{
    const Row rows[] = {{"ねこ", "猫", "ねこ", "normalized cat", 0},
        {"abc", "abc", "", "normalized latin", 0}};
    VjoArena a; VjoPlatform p; VjoConfig cfg; VjoDictResult r; VjoErr e;
    char too_long[4098];
    setup(&a, &p, &cfg); build(&fixtures[0], "main.vjdict", rows, 2, 2);
    TEST_ASSERT(!lookup(&a, &p, &cfg, "ネコ", 0, &r, &e));
    TEST_CHECK(r.n_vocab == 1 && r.tokens[0].len16 == 2);
    vjo_arena_reset(&a);
    TEST_ASSERT(!lookup(&a, &p, &cfg, "ａｂｃ", 0, &r, &e));
    TEST_CHECK(r.n_vocab == 1 && r.tokens[0].len16 == 3);
    vjo_arena_reset(&a); memset(too_long, 'X', sizeof(too_long) - 1); too_long[4097] = 0;
    int before = opened;
    TEST_CHECK(lookup(&a, &p, &cfg, too_long, 0, &r, &e) == VJO_E_TOO_LARGE);
    TEST_CHECK(opened == before && a.used == 0);
    VjoOverlayData out = {.sentence = "猫"};
    cfg.dictionary = VJO_DICT_JPDB;
    TEST_CHECK(vjo_overlay_lookup_at(&a, &p, &cfg, 0, &out) == VJO_E_PARSE);
    TEST_CHECK(out.failed_stage == VJO_STAGE_DICT && out.list.header == out.sentence);
}

static void every_file_read_failure_closes_and_rolls_back(void)
{
    const Row row = {"猫", "猫", "ねこ", "cat", 0};
    for (int version = 1; version <= 2; version++) {
        VjoArena a; VjoPlatform p; VjoConfig cfg; VjoOverlayData out;
        setup(&a, &p, &cfg); build(&fixtures[0], "main.vjdict", &row, 1, version);
        TEST_ASSERT(!vjo_overlay_ocr_text(&a, &cfg, "猫。", &out));
        const char *header = out.sentence;
        size_t mark = a.used;
        TEST_ASSERT(!vjo_overlay_lookup_at(&a, &p, &cfg, 0, &out));
        int total = reads;
        TEST_CHECK(payload_reads == 1);
        for (int i = 1; i <= total; i++) {
            vjo_arena_release(&a, mark); reads = 0; fail_read = i;
            TEST_CHECK(vjo_overlay_lookup_at(&a, &p, &cfg, 0, &out) == VJO_E_LOCAL_IO);
            TEST_CHECK(opened == closed && fixtures[0].active == 0 && a.used == mark);
            TEST_CHECK(out.list.header == header && !out.list.entries && !out.list.n_entries);
            TEST_CHECK(out.failed_stage == VJO_STAGE_DICT && out.err.dict == VJO_DICT_LOCAL);
        }
        fail_read = 0;
        TEST_ASSERT(!vjo_overlay_lookup_at(&a, &p, &cfg, 0, &out));
        TEST_CHECK(out.list.n_entries == 1 && out.failed_stage == VJO_STAGE_NONE);
    }
}

TEST_LIST = {
    {"longest_and_first_posting", longest_and_first_posting},
    {"inflection_and_dictionary_priority", inflection_and_dictionary_priority},
    {"cursor_boundary_and_no_later_word", cursor_boundary_and_no_later_word},
    {"raw_newline_and_supplementary_highlights", raw_newline_and_supplementary_highlights},
    {"punctuation_and_read_counts_do_not_scan_remainder", punctuation_and_read_counts_do_not_scan_remainder},
    {"largest_entry_and_input_fit_64k", largest_entry_and_input_fit_64k},
    {"repeated_lookups_oom_and_io_recovery", repeated_lookups_oom_and_io_recovery},
    {"normalized_source_span_and_invalid_input", normalized_source_span_and_invalid_input},
    {"every_file_read_failure_closes_and_rolls_back", every_file_read_failure_closes_and_rolls_back},
    {NULL, NULL}
};
