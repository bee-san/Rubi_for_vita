/* Generated, small VJDICT1/2 fixtures exercise the production file callbacks,
 * deinflector and selected-entry formatter without copyrighted dictionary data. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "acutest.h"
#include "anki.h"
#include "lapis.h"
#include "anki_queue.h"
#include "client.h"
#include "local_dict.h"
#include "utf.h"

typedef struct {
    const char *key, *spelling, *reading, *meaning;
    uint16_t rules;
    uint32_t rank; /* v3 fixtures: the payload's frequency word (0 = unranked) */
} Row;
typedef struct {
    const char *name;
    uint8_t bytes[32768];
    size_t size;
    uint64_t payload[24];
    uint32_t payload_size[24];
    int n_payload, active;
} Fixture;
static Fixture fixtures[VJO_LOCAL_MAX_DICTS];
static uint8_t arena_mem[64 * 1024];
static int opened, closed, reads, payload_reads, fail_read, opens_by[VJO_LOCAL_MAX_DICTS];
static size_t read_bytes;
static int payload_touches; /* reads starting at a payload, of any length */

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
/* v3: the frequency word first (bits 24-31 set when the rank's high byte
 * says how it matched: the reader must ignore them). */
static size_t add_payload(Fixture *f, const Row *r, size_t off, int version)
{
    size_t start = off;
    const char *parts[] = {r->spelling, r->reading, r->meaning};
    if (version == 3) {
        TEST_ASSERT(off + 4 <= sizeof(f->bytes));
        put32(f->bytes + off, r->rank); off += 4;
    }
    for (int i = 0; i < 3; i++) {
        size_t n = strlen(parts[i]) + 1;
        TEST_ASSERT(off + n <= sizeof(f->bytes));
        memcpy(f->bytes + off, parts[i], n); off += n;
    }
    f->payload[f->n_payload] = start;
    f->payload_size[f->n_payload++] = (uint32_t)(off - start);
    return off;
}
static const char *fixture_title = "Jiten"; /* v3 header's frequency list */
/* v2/v3: the dictionary's own name at page 0 bytes 192..255 (NULL: none, all zero) */
static const char *fixture_dict_title;
/* The longest name a v3 header holds: with a maximum entry it still fits the
 * selected entry's budget (VJO_LOCAL_SELECTED_BUDGET). */
static const char longest_title[] = "A frequency list with the longest name a v3 header holds: 63 B!";
typedef char LongestTitleIs63[(sizeof(longest_title) == 64) ? 1 : -1];
static const char longest_dict_title[] = "The longest dictionary title a page-0 field holds, 63 byte long";
typedef char LongestDictTitleIs63[(sizeof(longest_dict_title) == 64) ? 1 : -1];
/* Formats 1 (sorted index), 2 (hash pages) and 3 (2 with the frequency list's
 * name and a frequency word per payload; a posting's spare u16 is set, as a
 * later index may use it, and must be ignored). */
static void build(Fixture *f, const char *name, const Row *rows, int n, int version)
{
    int order[24];
    size_t off;
    memset(f, 0, sizeof(*f)); f->name = name;
    TEST_ASSERT(n > 0 && n <= 24);
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
    memcpy(f->bytes, version == 1 ? "VJDICT1\0" : version == 2 ? "VJDICT2\0" : "VJDICT3\0", 8);
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
            payload = off; off = add_payload(f, r, off, version);
            put64(record + 8, payload); put16(record + 18, r->rules);
            put32(record + 20, (uint32_t)(off - payload));
        }
    } else {
        unsigned groups = 0;
        off = 8192;
        put64(f->bytes + 24, 4096); put64(f->bytes + 32, off);
        put64(f->bytes + 48, 1); put64(f->bytes + 56, 1);
        if (version == 3) memcpy(f->bytes + 64, fixture_title, strlen(fixture_title));
        if (fixture_dict_title) memcpy(f->bytes + 192, fixture_dict_title, strlen(fixture_dict_title));
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
                off = add_payload(f, &rows[order[j]], off, version);
                put64(post, payload); put32(post + 8, (uint32_t)(off - payload));
                put16(post + 12, rows[order[j]].rules);
                if (version == 3) put16(post + 14, 0xa5a5);
            }
            groups++; i = end;
        }
        put32(f->bytes + 4100, groups);
    }
    f->size = off; put64(f->bytes + 40, off);
}
/* The converter's key filter (tools/convert_dictionary.py build_key_filter) over rows'
 * keys with a given directory: a key's block is the last start at or below its first
 * two code points. fill 0: the keys' Bloom bits; 1: every bit; 2: none (corrupt). */
static uint32_t prefix_of(const char *key)
{
    size_t len = strlen(key), i = 0;
    uint32_t c0 = vjo_utf8_next(key, len, &i), c1 = i < len ? vjo_utf8_next(key, len, &i) : 0;
    return (c0 < 0xffff ? c0 : 0xffff) << 16 | (c1 < 0xffff ? c1 : 0xffff);
}
static void add_filter(Fixture *f, const Row *rows, int n, const uint32_t *starts, int blocks, int fill)
{
    size_t off = (f->size + 4095) / 4096 * 4096;
    TEST_ASSERT(blocks > 0 && off + (size_t)blocks * 4096 <= sizeof(f->bytes));
    memset(f->bytes + f->size, 0, off + (size_t)blocks * 4096 - f->size);
    if (fill == 1) memset(f->bytes + off, 0xff, (size_t)blocks * 4096);
    for (int i = 0; fill == 0 && i < n; i++) {
        uint32_t prefix = prefix_of(rows[i].key);
        uint64_t h = key_hash(rows[i].key);
        uint32_t h1 = (uint32_t)h, h2 = (uint32_t)(h >> 32) | 1u;
        int b = 0;
        while (b + 1 < blocks && starts[b + 1] <= prefix) b++;
        for (uint32_t k = 0; k < 7; k++) {
            uint32_t bit = (h1 + k * h2) & 32767u;
            f->bytes[off + (size_t)b * 4096 + (bit >> 3)] |= (uint8_t)(1u << (bit & 7));
        }
    }
    memcpy(f->bytes + 128, "VJKF", 4); put32(f->bytes + 132, 1);
    put32(f->bytes + 136, (uint32_t)blocks); put32(f->bytes + 140, 7);
    put64(f->bytes + 144, off); put32(f->bytes + 152, (uint32_t)n); put32(f->bytes + 156, 0);
    for (int b = 0; b < blocks; b++) put32(f->bytes + 256 + b * 4, starts[b]);
    f->size = off + (size_t)blocks * 4096;
    put64(f->bytes + 40, f->size);
}
/* The converter's posting hints (tools/convert_dictionary.py posting_hint) into a
 * built v3 fixture: every posting's u16, from its payload, and the VJPH descriptor. */
static uint32_t fixture_freq_key(uint32_t word)
{
    uint32_t rank = word & 0xffffffu, e = 0, m;
    if (!rank) return 0;
    while (rank >> (e + 1)) e++;
    m = e >= 7 ? (rank >> (e - 7)) & 127u : (rank << (7 - e)) & 127u;
    return (word & (1u << 24) ? 0u : 4096u) + 4095u - (e * 128u + m);
}
static int normalizes_to(const uint8_t *s, size_t n, const uint8_t *key, size_t key_len)
{
    size_t i = 0, k = 0;
    while (i < n) {
        char enc[4];
        uint32_t cp = vjo_utf8_next((const char *)s, n, &i);
        int m;
        if (cp >= 0x30a1 && cp <= 0x30f6) cp -= 0x60;
        if (cp >= 0xff01 && cp <= 0xff5e) cp -= 0xfee0;
        m = vjo_utf8_encode(cp, enc);
        if (k + (size_t)m > key_len || memcmp(key + k, enc, (size_t)m)) return 0;
        k += (size_t)m;
    }
    return k == key_len;
}
static void add_hints(Fixture *f)
{
    uint32_t count = (uint32_t)(f->bytes[4100] | f->bytes[4101] << 8);
    TEST_ASSERT(!memcmp(f->bytes, "VJDICT3", 7));
    for (uint32_t slot = 0; slot < count; slot++) {
        const uint8_t *rec = f->bytes + 4096 + 16 + slot * 24;
        size_t group = (size_t)(rec[8] | rec[9] << 8 | rec[10] << 16), key_len = (size_t)(rec[20] | rec[21] << 8);
        uint32_t postings = (uint32_t)(rec[16] | rec[17] << 8);
        for (uint32_t j = 0; j < postings; j++) {
            uint8_t *post = f->bytes + group + key_len + j * 16;
            size_t entry = (size_t)(post[0] | post[1] << 8 | post[2] << 16);
            size_t len = (size_t)(post[8] | post[9] << 8), n = (len < 164 ? len : 164) - 4, sp;
            const uint8_t *head = f->bytes + entry + 4, *nul = memchr(head, 0, n);
            uint32_t word = (uint32_t)(f->bytes[entry] | f->bytes[entry + 1] << 8 | f->bytes[entry + 2] << 16 |
                                       (uint32_t)f->bytes[entry + 3] << 24);
            uint16_t hint = (uint16_t)(fixture_freq_key(word) << 3);
            if (nul) {
                sp = (size_t)(nul - head);
                if (sp == key_len && !memcmp(head, f->bytes + group, key_len)) hint |= 1;
                if (normalizes_to(head, sp, f->bytes + group, key_len)) hint |= 2;
                if (sp + 1 < n && head[sp + 1]) hint |= 4;
            }
            put16(post + 14, hint);
        }
    }
    memcpy(f->bytes + 160, "VJPH", 4);
    put32(f->bytes + 164, 1);
}
static int read_file(void *ctx, uint64_t off, void *dst, size_t n)
{
    Fixture *f = ctx;
    reads++;
    TEST_CHECK(f->active > 0);
    if (fail_read == reads) return -1;
    if (off > f->size || n > f->size - (size_t)off) return -1;
    read_bytes += n;
    for (int i = 0; i < f->n_payload; i++) {
        if (off == f->payload[i] && n == f->payload_size[i]) payload_reads++;
        if (off == f->payload[i]) payload_touches++;
    }
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
    for (int i = 0; i < VJO_LOCAL_MAX_DICTS; i++) if (fixtures[i].name && !strcmp(name, fixtures[i].name)) {
        Fixture *f = &fixtures[i];
        f->active++; opened++; opens_by[i]++;
        *out = (VjoFile){f, f->size, read_file, close_file, 0}; return 0;
    }
    return -1;
}
static void setup(VjoArena *a, VjoPlatform *p, VjoConfig *cfg)
{
    memset(fixtures, 0, sizeof(fixtures));
    memset(opens_by, 0, sizeof(opens_by));
    opened = closed = reads = payload_reads = fail_read = 0;
    read_bytes = 0;
    fixture_title = "Jiten";
    fixture_dict_title = NULL;
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
    const Row rows[] = {{"猫", "猫", "ねこ", "cat", 0, 0},
        {"猫舌", "猫舌", "ねこじた", "first long meaning", 0, 0},
        {"猫舌", "猫舌", "ねこじた", "other long meaning", 0, 0}};
    for (int version = 1; version <= 3; version++) {
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
    const Row first[] = {{"食べる", "食べる", "たべる", "first inflection", 1, 0},
        {"猫", "猫", "ねこ", "first dictionary", 0, 0}};
    const Row second[] = {{"食べた", "食べた", "たべた", "exact later dictionary", 0, 0},
        {"猫", "猫", "ねこ", "second dictionary", 0, 0}};
    for (int version = 1; version <= 3; version++) {
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
    const Row rows[] = {{"猫", "猫", "ねこ", "cat", 0, 0}, {"犬", "犬", "いぬ", "dog", 0, 0}};
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
    const Row rows[] = {{"猫犬", "猫犬", "", "must not cross newline", 0, 0},
        {"猫", "猫", "ねこ", "cat", 0, 0}, {"犬", "犬", "いぬ", "dog", 0, 0},
        {"𠮷猫", "𠮷猫", "", "supplementary term", 0, 0}};
    for (int version = 1; version <= 3; version++) {
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
    const Row rows[] = {{"猫", "猫", "ねこ", "cat", 0, 0}, {"犬", "犬", "いぬ", "dog", 0, 0},
        {"猫犬", "猫犬", "", "longer", 0, 0}};
    for (int version = 1; version <= 3; version++) {
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
    /* 猫 + NUL, empty reading + NUL, meaning + NUL totals exactly 8192, in v3
     * with the 4-byte frequency word before them. */
    memcpy(text, "猫。", 6); memset(text + 6, 'X', sizeof(text) - 7); text[4095] = 0;
    for (int version = 1; version <= 3; version++) {
        size_t meaning_len = version == 3 ? 8182 : 8186;
        memset(meaning, 'M', meaning_len); meaning[meaning_len] = 0;
        const Row row = {"猫", "猫", "", meaning, 0, 1536};
        VjoArena a; VjoPlatform p; VjoConfig cfg; VjoOverlayData out;
        setup(&a, &p, &cfg); fixture_title = longest_title; fixture_dict_title = longest_dict_title;
        build(&fixtures[0], "main.vjdict", &row, 1, version);
        TEST_ASSERT(fixtures[0].payload_size[0] == VJO_LOCAL_MAX_ENTRY);
        out = (VjoOverlayData){.ocr_text = text, .filtered = text, .sentence = text};
        TEST_ASSERT(!vjo_overlay_lookup_at(&a, &p, &cfg, 0, &out));
        TEST_ASSERT(out.list.n_entries == 1);
        TEST_CHECK(strlen(out.list.entries[0].vocab->meanings[0]) == meaning_len);
        TEST_CHECK(strstr(out.list.entries[0].text, meaning) != NULL);
        /* v3: the rank and its list's name fit the selected entry's budget too */
        TEST_CHECK(out.list.entries[0].vocab->rank == (version == 3 ? 1536 : VJO_NO_RANK));
        TEST_CHECK(version == 3 ? out.list.source && !strcmp(out.list.source, longest_title) : !out.list.source);
        /* v2/v3: and the longest dictionary name, as its file's title */
        TEST_CHECK(version >= 2 ? out.list.entries[0].vocab->dictionary &&
                                      !strcmp(out.list.entries[0].vocab->dictionary, longest_dict_title)
                                : !out.list.entries[0].vocab->dictionary);
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
    const Row row = {"猫", "猫", "ねこ", "cat", 0, 0};
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
    const Row rows[] = {{"ねこ", "猫", "ねこ", "normalized cat", 0, 0},
        {"abc", "abc", "", "normalized latin", 0, 0}};
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
    VjoOverlayData out = {.sentence = NULL}; /* no recognized sentence: nothing to look up */
    TEST_CHECK(vjo_overlay_lookup_at(&a, &p, &cfg, 0, &out) == VJO_E_PARSE);
    TEST_CHECK(out.failed_stage == VJO_STAGE_DICT && out.err.dict == VJO_DICT_LOCAL && a.used == 0);
}

static void every_file_read_failure_closes_and_rolls_back(void)
{
    const Row row = {"猫", "猫", "ねこ", "cat", 0, 0};
    for (int version = 1; version <= 3; version++) {
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

/* ---- several dictionaries: one entry resident, the others located ---- */

static const char *const page_names[VJO_LOCAL_MAX_DICTS] = {
    "d0.vjdict", "d1.vjdict", "d2.vjdict", "d3.vjdict", "d4.vjdict", "d5.vjdict", "d6.vjdict", "d7.vjdict"};

static void set_names(VjoConfig *cfg, int n)
{
    cfg->local_dictionaries[0] = 0;
    for (int i = 0; i < n; i++) {
        if (i) strcat(cfg->local_dictionaries, ",");
        strcat(cfg->local_dictionaries, page_names[i]);
    }
}

static int pages_lookup(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg, const char *text,
                        size_t cursor, int limit, VjoLocalPages *pages, VjoDictResult *r, VjoErr *e)
{
    int rc = vjo_local_lookup_pages(a, p, cfg, text, cursor, limit, pages, r, e);
    TEST_CHECK(opened == closed && e->rc == rc && e->dict == VJO_DICT_LOCAL);
    TEST_CHECK(pages->count >= 0 && pages->count <= limit && (pages->count > 0) == (r->n_vocab == 1));
    return rc;
}

static void pages_follow_best_match_then_priority(void)
{
    /* d2 has the longest span, so it is the single best result. d1 has only
     * an inflected match and d3 has none; they keep configured order. */
    const Row d0[] = {{"猫", "猫", "ねこ", "d0 cat", 0, 0}};
    const Row d1[] = {{"猫", "猫", "ねこ", "d1 cat", 0, 0}, {"猫舌", "猫舌", "", "d1 other", 0, 0}};
    const Row d2[] = {{"猫舌", "猫舌", "ねこじた", "d2 long", 0, 0}};
    const Row d3[] = {{"犬", "犬", "いぬ", "d3 dog", 0, 0}};
    for (int version = 1; version <= 3; version++) {
        VjoArena a; VjoPlatform p; VjoConfig cfg; VjoDictResult r, single; VjoErr e; VjoLocalPages pages;
        setup(&a, &p, &cfg); set_names(&cfg, 4);
        build(&fixtures[0], page_names[0], d0, 1, version);
        build(&fixtures[1], page_names[1], d1, 2, version);
        build(&fixtures[2], page_names[2], d2, 1, version);
        build(&fixtures[3], page_names[3], d3, 1, version);
        /* d1 also has 猫舌 and comes first: it is the best match, d2 second. */
        TEST_ASSERT(!vjo_local_lookup_at(&a, &p, &cfg, "猫舌。", 0, &single, &e));
        TEST_ASSERT(single.n_vocab == 1 && !strcmp(single.vocab[0].meanings[0], "d1 other"));
        vjo_arena_reset(&a); payload_reads = 0;
        TEST_ASSERT(!pages_lookup(&a, &p, &cfg, "猫舌。", 0, 8, &pages, &r, &e));
        TEST_CHECK(payload_reads == 1); /* only the shown entry is read */
        TEST_ASSERT(pages.count == 3 && r.n_vocab == 1);
        TEST_CHECK(!strcmp(r.vocab[0].meanings[0], "d1 other") && r.tokens[0].len16 == 2);
        TEST_CHECK(pages.page[0].dict == 1 && pages.page[0].len16 == 2);
        TEST_CHECK(pages.page[1].dict == 0 && pages.page[1].len16 == 1); /* only 猫 in d0 */
        TEST_CHECK(pages.page[2].dict == 2 && pages.page[2].len16 == 2);
        TEST_CHECK(pages.cursor_byte == 0 && pages.pos16 == 0);
        /* A limit keeps the first pages only. */
        vjo_arena_reset(&a);
        TEST_ASSERT(!pages_lookup(&a, &p, &cfg, "猫舌。", 0, 2, &pages, &r, &e));
        TEST_CHECK(pages.count == 2 && pages.page[1].dict == 0);
        /* Each page reads exactly its own payload. */
        const char *expect[] = {"d1 other", "d0 cat"};
        for (int page = 0; page < 2; page++) {
            VjoDictResult one;
            size_t used = a.used;
            payload_reads = 0; memset(opens_by, 0, sizeof(opens_by));
            TEST_ASSERT(!vjo_local_read_page(&a, &p, &cfg, &pages, page, &one, &e));
            TEST_CHECK(one.n_vocab == 1 && !strcmp(one.vocab[0].meanings[0], expect[page]));
            TEST_CHECK(one.tokens[0].len16 == pages.page[page].len16 && one.tokens[0].pos16 == 0);
            TEST_CHECK(payload_reads == 1 && opens_by[pages.page[page].dict] == 1 && opened == closed);
            int others = 0;
            for (int i = 0; i < VJO_LOCAL_MAX_DICTS; i++) if (i != pages.page[page].dict) others += opens_by[i];
            TEST_CHECK(others == 0);
            TEST_CHECK(a.used - used <= VJO_LOCAL_SELECTED_BUDGET);
        }
    }
}

static void exact_later_dictionary_precedes_inflected_page(void)
{
    const Row d0[] = {{"食べる", "食べる", "たべる", "d0 inflected", 1, 0}};
    const Row d1[] = {{"食べた", "食べた", "たべた", "d1 exact", 0, 0}};
    VjoArena a; VjoPlatform p; VjoConfig cfg; VjoDictResult r; VjoErr e; VjoLocalPages pages;
    setup(&a, &p, &cfg); set_names(&cfg, 2);
    build(&fixtures[0], page_names[0], d0, 1, 2);
    build(&fixtures[1], page_names[1], d1, 1, 2);
    TEST_ASSERT(!pages_lookup(&a, &p, &cfg, "食べた", 0, 8, &pages, &r, &e));
    TEST_ASSERT(pages.count == 2);
    TEST_CHECK(!strcmp(r.vocab[0].meanings[0], "d1 exact"));
    TEST_CHECK(pages.page[0].dict == 1 && pages.page[1].dict == 0);
    TEST_CHECK(pages.page[0].len16 == 3 && pages.page[1].len16 == 3);
}

/* One key's postings are ranked, not taken in file order (generated rows in
 * the bundled Daijirin4's order, both formats): kana text shows the entry
 * spelled in kana, inflected kana its dictionary form before a homograph, a
 * listed common reading wins, an entry with a reading beats a reading-less
 * character article, then the longer article; katakana text matches its
 * hiragana spelling. Synthetic rows, not dictionary content. */
static void postings_are_ranked_by_how_the_text_is_written(void)
{
    const Row rows[] = {
        {"から", "空", "から", "empty", 0, 0}, {"から", "殻", "から", "shell, much longer article", 0, 0},
        {"から", "から", "", "particle: from", 0, 0},
        {"した", "下", "した", "below", 0, 0}, {"する", "剃る", "する", "to shave", 2, 0},
        {"する", "する", "", "to do", 4, 0},
        {"わたし", "私", "わたし", "I", 0, 0}, {"私", "私", "あたい", "I (casual)", 0, 0},
        {"私", "私", "わたし", "I", 0, 0}, {"私", "私", "わっし", "I (old), the longest article of the three", 0, 0},
        {"人", "人", "", "the character", 0, 0}, {"人", "人", "ひと", "person", 0, 0},
        {"って", "って", "", "quotative", 0, 0}};
    for (int version = 1; version <= 3; version++) {
        VjoArena a; VjoPlatform p; VjoConfig cfg; VjoDictResult r; VjoErr e;
        static const struct { const char *text, *meaning; int len16; } cases[] = {
            {"から", "particle: from", 2}, {"した", "to do", 2}, {"私", "I", 1}, {"人", "person", 1},
            {"ッて", "quotative", 2}, {"するの", "to do", 2}};
        setup(&a, &p, &cfg);
        build(&fixtures[0], "main.vjdict", rows, (int)(sizeof(rows) / sizeof(rows[0])), version);
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            vjo_arena_init(&a, arena_mem, sizeof(arena_mem));
            TEST_CASE_("v%d %s", version, cases[i].text);
            TEST_ASSERT(!lookup(&a, &p, &cfg, cases[i].text, 0, &r, &e) && r.n_vocab == 1);
            TEST_CHECK_(!strcmp(r.vocab[0].meanings[0], cases[i].meaning), "got %s", r.vocab[0].meanings[0]);
            TEST_CHECK(r.tokens[0].len16 == cases[i].len16);
        }
    }
}

/* Katakana text: the entry spelled exactly so (RANK_WRITTEN) beats one spelled
 * so in hiragana, which beats a homograph with a longer article (RANK_KANA);
 * mixed script and full width ASCII; half-width katakana is not normalized,
 * as before. The cases are audit-word-lookup's (its adversarial.c
 * a4_kana_mixes_and_fullwidth): they fail with RANK_WRITTEN or the katakana
 * match removed. Synthetic rows, not dictionary content. */
static void katakana_text_prefers_its_spelling_then_hiragana(void)
{
    const Row rows[] = {
        {"じゅーす", "汁酢", "じゅーす", "kanji homograph, the longest article of all of these", 0, 0},
        {"じゅーす", "じゅーす", "", "hiragana spelled, longer article", 0, 0},
        {"じゅーす", "ジュース", "", "katakana", 0, 0},
        {"って", "偶て", "って", "kanji homograph with a much longer article", 0, 0},
        {"って", "って", "", "quotative", 0, 0},
        {"ok", "OK", "", "ascii", 0, 0}, {"ok", "ＯＫ", "", "full width", 0, 0},
        {"OK", "OK", "", "ascii upper", 0, 0}, {"OK", "ＯＫ", "", "full width upper", 0, 0}};
    static const struct { const char *text, *meaning; } cases[] = {
        {"ジュース", "katakana"}, {"じゅーす", "hiragana spelled, longer article"},
        {"ジューす", "hiragana spelled, longer article"}, /* mixed: both equal as kana, the longer article */
        {"ッて", "quotative"}, {"って", "quotative"}, {"ッテ", "quotative"},
        {"ＯＫ", "full width upper"}, {"OK", "ascii upper"}};
    for (int version = 1; version <= 3; version++) {
        VjoArena a; VjoPlatform p; VjoConfig cfg; VjoDictResult r; VjoErr e;
        setup(&a, &p, &cfg);
        build(&fixtures[0], "main.vjdict", rows, (int)(sizeof(rows) / sizeof(rows[0])), version);
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            vjo_arena_init(&a, arena_mem, sizeof(arena_mem));
            TEST_CASE_("v%d %s", version, cases[i].text);
            TEST_ASSERT(!lookup(&a, &p, &cfg, cases[i].text, 0, &r, &e) && r.n_vocab == 1);
            TEST_CHECK_(!strcmp(r.vocab[0].meanings[0], cases[i].meaning), "got %s", r.vocab[0].meanings[0]);
        }
        vjo_arena_init(&a, arena_mem, sizeof(arena_mem));
        TEST_CHECK(!lookup(&a, &p, &cfg, "\xef\xbd\xbc\xef\xbe\x9e\xef\xbd\xad\xef\xbd\xb0\xef\xbd\xbd", 0, &r, &e) &&
                   r.n_vocab == 0); /* ｼﾞｭｰｽ */
    }
}

/* With several dictionaries, an entry written as the text is in a later one
 * beats a homograph in an earlier one; the homograph is still a page. */
static void written_entry_in_later_dictionary_precedes_homograph(void)
{
    const Row d0[] = {{"から", "空", "から", "d0 homograph", 0, 0}};
    const Row d1[] = {{"から", "から", "", "d1 written", 0, 0}};
    VjoArena a; VjoPlatform p; VjoConfig cfg; VjoDictResult r; VjoErr e; VjoLocalPages pages;
    setup(&a, &p, &cfg); set_names(&cfg, 2);
    build(&fixtures[0], page_names[0], d0, 1, 2);
    build(&fixtures[1], page_names[1], d1, 1, 2);
    TEST_ASSERT(!pages_lookup(&a, &p, &cfg, "から", 0, 1, &pages, &r, &e) && r.n_vocab == 1);
    TEST_CHECK(!strcmp(r.vocab[0].meanings[0], "d1 written"));
    vjo_arena_init(&a, arena_mem, sizeof(arena_mem));
    TEST_ASSERT(!pages_lookup(&a, &p, &cfg, "から", 0, 8, &pages, &r, &e) && pages.count == 2);
    TEST_CHECK(pages.page[0].dict == 1 && pages.page[1].dict == 0);
}

static void single_result_reads_are_unchanged(void)
{
    /* Limit 1 is the previous single lookup: once d0 matches, d1's index is
     * never searched. Another limit searches d1 but never reads its payload. */
    const Row row = {"猫", "猫", "ねこ", "cat", 0, 0};
    VjoArena a; VjoPlatform p; VjoConfig cfg; VjoDictResult r; VjoErr e; VjoLocalPages pages;
    setup(&a, &p, &cfg); set_names(&cfg, 2);
    build(&fixtures[0], page_names[0], &row, 1, 2);
    build(&fixtures[1], page_names[1], &row, 1, 2);
    TEST_ASSERT(!vjo_local_lookup_at(&a, &p, &cfg, "猫", 0, &r, &e));
    int single_reads = reads;
    size_t single_peak = a.peak, single_used = a.used;
    vjo_arena_init(&a, arena_mem, sizeof(arena_mem)); reads = payload_reads = 0;
    TEST_ASSERT(!pages_lookup(&a, &p, &cfg, "猫", 0, 1, &pages, &r, &e));
    TEST_CHECK(reads == single_reads && pages.count == 1 && a.peak == single_peak && a.used == single_used);
    vjo_arena_init(&a, arena_mem, sizeof(arena_mem)); reads = payload_reads = 0;
    TEST_ASSERT(!pages_lookup(&a, &p, &cfg, "猫", 0, 2, &pages, &r, &e));
    TEST_CHECK(pages.count == 2 && reads > single_reads && payload_reads == 1);
    TEST_CHECK(a.peak == single_peak && a.used == single_used); /* locators are the caller's */
    printf("  two dictionaries, limit 1: %d reads; limit 2: %d reads; peak %zu both\n",
           single_reads, reads, a.peak);
}

/* First byte of an entry's definition ("spelling rank\nmeaning"). */
static char meaning_byte(const VjoOverlayData *out)
{
    const char *nl = strchr(out->list.entries[0].text, '\n');
    return nl ? nl[1] : 0;
}

static void overlay_pages_stream_through_one_arena(void)
{
    /* Eight maximum-size definitions are never resident together: paging
     * releases the shown entry and reads only the next page's payload. */
    static char meanings[VJO_LOCAL_MAX_DICTS][8192];
    Row rows[VJO_LOCAL_MAX_DICTS];
    VjoArena a; VjoPlatform p; VjoConfig cfg; VjoOverlayData out;
    setup(&a, &p, &cfg); set_names(&cfg, 8); fixture_title = longest_title;
    for (int i = 0; i < VJO_LOCAL_MAX_DICTS; i++) {
        /* Formats 3, 2, 1, 3, ...: page 0 (v3) holds the most, its list's name too. */
        int version = 3 - i % 3;
        size_t len = version == 3 ? 8182 : 8186;
        memset(meanings[i], 'a' + i, len); meanings[i][len] = 0;
        rows[i] = (Row){"猫", "猫", "", meanings[i], 0, version == 3 ? 100u + (uint32_t)i : 0};
        build(&fixtures[i], page_names[i], &rows[i], 1, version);
        TEST_ASSERT(fixtures[i].payload_size[0] == VJO_LOCAL_MAX_ENTRY);
    }
    const char *text = "猫が好き";
    out = (VjoOverlayData){.ocr_text = text, .filtered = text, .sentence = text};
    cfg.local_dictionary_results = 1;
    TEST_ASSERT(!vjo_overlay_lookup_at(&a, &p, &cfg, 0, &out));
    TEST_CHECK(!out.pages && vjo_overlay_page_count(&out) == 1);
    size_t single_peak = a.peak, single_used = a.used;
    TEST_CHECK(vjo_overlay_lookup_page(&a, &p, &cfg, 0, &out) == VJO_E_PARSE); /* nothing located */
    vjo_arena_init(&a, arena_mem, sizeof(arena_mem));
    cfg.local_dictionary_results = 8; payload_reads = 0;
    out = (VjoOverlayData){.ocr_text = text, .filtered = text, .sentence = text};
    TEST_ASSERT(!vjo_overlay_lookup_at(&a, &p, &cfg, 0, &out));
    TEST_ASSERT(out.pages && vjo_overlay_page_count(&out) == 8 && out.page == 0);
    TEST_CHECK(payload_reads == 1 && out.list.n_entries == 1 && meaning_byte(&out) == 'a');
    TEST_CHECK((uint8_t *)out.pages == a.base); /* locators first; the entry after them */
    size_t resident = a.used, peak = a.peak;
    TEST_CHECK(resident <= single_used + sizeof(VjoLocalPages) + 8);
    TEST_CHECK(peak <= single_peak + sizeof(VjoLocalPages) + 8 && peak <= sizeof(arena_mem));
    for (int round = 0; round < 25; round++) {
        for (int page = 0; page < 8; page++) {
            int target = round % 2 ? 7 - page : page;
            payload_reads = 0; memset(opens_by, 0, sizeof(opens_by));
            TEST_ASSERT(!vjo_overlay_lookup_page(&a, &p, &cfg, target, &out));
            TEST_CHECK(out.page == target && out.list.n_entries == 1 && out.list.header == text);
            TEST_CHECK(meaning_byte(&out) == 'a' + target);
            TEST_CHECK(out.list.entries[0].vocab->rank == (int)rows[target].rank);
            TEST_CHECK(target % 3 ? !out.list.source : out.list.source && !strcmp(out.list.source, longest_title));
            TEST_CHECK(out.list.entries[0].hl_start == 0 && out.list.entries[0].hl_end == 3);
            TEST_CHECK(payload_reads == 1 && opens_by[target] == 1 && opened == closed);
            TEST_CHECK(a.used <= resident); /* one entry resident, never more */
        }
    }
    TEST_CHECK(a.peak <= peak); /* page reads need no lookup scratch */
    printf("  8 x 8 KiB definitions: single peak %zu resident %zu; paged peak %zu resident %zu\n",
           single_peak, single_used, a.peak, resident);
}

static void page_failures_keep_header_and_locators(void)
{
    const Row d0[] = {{"猫", "猫", "ねこ", "d0 cat", 0, 0}};
    const Row d1[] = {{"猫", "猫", "ねこ", "d1 cat", 0, 0}};
    VjoArena a; VjoPlatform p; VjoConfig cfg; VjoOverlayData out;
    setup(&a, &p, &cfg); set_names(&cfg, 2); cfg.local_dictionary_results = 2;
    build(&fixtures[0], page_names[0], d0, 1, 2);
    build(&fixtures[1], page_names[1], d1, 1, 1);
    TEST_ASSERT(!vjo_overlay_ocr_text(&a, &cfg, "猫。", &out));
    const char *header = out.sentence;
    size_t mark = a.used;
    TEST_ASSERT(!vjo_overlay_lookup_at(&a, &p, &cfg, 0, &out));
    TEST_ASSERT(out.pages && out.pages->count == 2);
    VjoLocalPages *pages = out.pages;
    size_t keep = (size_t)((uint8_t *)(pages + 1) - a.base);
    /* Every read of the page fails cleanly, then the page loads. */
    reads = 0;
    TEST_ASSERT(!vjo_overlay_lookup_page(&a, &p, &cfg, 1, &out));
    int total = reads;
    for (int i = 1; i <= total; i++) {
        reads = 0; fail_read = i;
        TEST_CHECK(vjo_overlay_lookup_page(&a, &p, &cfg, 1, &out) == VJO_E_LOCAL_IO);
        TEST_CHECK(out.pages == pages && out.page == 1 && !out.list.n_entries && !out.list.entries);
        TEST_CHECK(out.list.header == header && out.failed_stage == VJO_STAGE_DICT && a.used == keep);
        TEST_CHECK(opened == closed && fixtures[1].active == 0);
    }
    fail_read = 0;
    TEST_ASSERT(!vjo_overlay_lookup_page(&a, &p, &cfg, 1, &out));
    TEST_CHECK(out.failed_stage == VJO_STAGE_NONE && strstr(out.list.entries[0].text, "d1 cat"));
    /* A refused page allocation keeps the recognized header. */
    size_t normal = a.size;
    a.size = keep + 64;
    TEST_CHECK(vjo_overlay_lookup_page(&a, &p, &cfg, 0, &out) == VJO_E_OOM);
    TEST_CHECK(out.list.header == header && !out.list.n_entries && a.used == keep && out.pages == pages);
    a.size = normal;
    TEST_ASSERT(!vjo_overlay_lookup_page(&a, &p, &cfg, 0, &out));
    TEST_CHECK(strstr(out.list.entries[0].text, "d0 cat") != NULL);
    /* A replaced file (other size) is reported, never read at stale offsets. */
    fixtures[1].size += 1; put64(fixtures[1].bytes + 40, fixtures[1].size);
    payload_reads = 0;
    TEST_CHECK(vjo_overlay_lookup_page(&a, &p, &cfg, 1, &out) == VJO_E_LOCAL_IO);
    TEST_CHECK(out.err.detail && strstr(out.err.detail, "changed") && !payload_reads && opened == closed);
    /* Invalid pages and a shortened dictionary list never read. */
    TEST_CHECK(vjo_overlay_lookup_page(&a, &p, &cfg, 2, &out) == VJO_E_PARSE);
    TEST_CHECK(vjo_overlay_lookup_page(&a, &p, &cfg, -1, &out) == VJO_E_PARSE);
    strcpy(cfg.local_dictionaries, page_names[0]);
    TEST_CHECK(vjo_overlay_lookup_page(&a, &p, &cfg, 1, &out) == VJO_E_LOCAL_IO);
    TEST_CHECK(out.list.header == header && a.used == keep && opened == closed);
    /* An OOM while locating keeps nothing; the next cursor works. */
    set_names(&cfg, 2); fixtures[1].size -= 1; put64(fixtures[1].bytes + 40, fixtures[1].size);
    vjo_arena_release(&a, mark);
    a.size = mark + sizeof(VjoLocalPages) + 64;
    TEST_CHECK(vjo_overlay_lookup_at(&a, &p, &cfg, 0, &out) == VJO_E_OOM);
    TEST_CHECK(!out.pages && a.used == mark && out.list.header == header && !out.list.n_entries);
    a.size = mark + 32;
    TEST_CHECK(vjo_overlay_lookup_at(&a, &p, &cfg, 0, &out) == VJO_E_OOM && !out.pages && a.used == mark);
    a.size = normal;
    TEST_ASSERT(!vjo_overlay_lookup_at(&a, &p, &cfg, 0, &out));
    TEST_CHECK(out.pages && out.pages->count == 2);
    /* No entry at the cursor: no locators stay allocated. */
    vjo_arena_release(&a, mark);
    TEST_ASSERT(!vjo_overlay_lookup_at(&a, &p, &cfg, 3, &out));
    TEST_CHECK(!out.pages && !out.list.n_entries && a.used == mark && vjo_overlay_page_count(&out) == 0);
}

static void eight_dictionaries_only_matching_pages(void)
{
    const Row cat = {"猫", "猫", "ねこ", "cat", 0, 0}, dog = {"犬", "犬", "いぬ", "dog", 0, 0};
    VjoArena a; VjoPlatform p; VjoConfig cfg; VjoDictResult r; VjoErr e; VjoLocalPages pages;
    setup(&a, &p, &cfg); set_names(&cfg, 8);
    for (int i = 0; i < 8; i++) build(&fixtures[i], page_names[i], i == 1 || i == 4 || i == 6 ? &cat : &dog, 1, 2);
    TEST_ASSERT(!pages_lookup(&a, &p, &cfg, "猫", 0, 8, &pages, &r, &e));
    TEST_ASSERT(pages.count == 3);
    TEST_CHECK(pages.page[0].dict == 1 && pages.page[1].dict == 4 && pages.page[2].dict == 6);
    for (int i = 0; i < 8; i++) TEST_CHECK(opens_by[i] == 1); /* every enabled file, once */
    vjo_arena_reset(&a);
    TEST_ASSERT(!pages_lookup(&a, &p, &cfg, "犬", 0, 3, &pages, &r, &e));
    TEST_CHECK(pages.count == 3 && pages.page[0].dict == 0 && pages.page[1].dict == 2 && pages.page[2].dict == 3);
    vjo_arena_reset(&a);
    int before = opened;
    TEST_ASSERT(!pages_lookup(&a, &p, &cfg, "。", 0, 8, &pages, &r, &e));
    TEST_CHECK(!pages.count && opened == before && !a.used);
}

/* ---- format 3: a frequency rank in every payload ---- */

/* The rank and the list's name come with the entry: no extra read, the name
 * in the result arena, unranked as VJO_NO_RANK; v2 has neither. */
static void v3_rank_and_source_come_with_the_entry(void)
{
    const Row rows[] = {{"猫", "猫", "ねこ", "cat", 0, 1536}, {"犬", "犬", "いぬ", "dog", 0, 0},
                        {"あんとわーぬ", "アントワーヌ", "", "a name", 0, 99999}};
    int reads_v2 = 0;
    size_t bytes_v2 = 0;
    for (int version = 2; version <= 3; version++) {
        VjoArena a; VjoPlatform p; VjoConfig cfg; VjoDictResult r; VjoErr e; VjoOverlayData out;
        setup(&a, &p, &cfg); build(&fixtures[0], "main.vjdict", rows, 3, version);
        TEST_ASSERT(!lookup(&a, &p, &cfg, "猫", 0, &r, &e) && r.n_vocab == 1);
        TEST_CHECK(r.vocab[0].rank == (version == 3 ? 1536 : VJO_NO_RANK));
        TEST_CHECK(!strcmp(r.vocab[0].spelling, "猫") && !strcmp(r.vocab[0].reading, "ねこ"));
        TEST_CHECK(!strcmp(r.vocab[0].meanings[0], "cat") && payload_reads == 1);
        if (version == 2) {
            TEST_CHECK(!r.source);
            reads_v2 = reads; bytes_v2 = read_bytes;
        } else {
            TEST_ASSERT(r.source && !strcmp(r.source, "Jiten"));
            TEST_CHECK((const uint8_t *)r.source >= a.base && (const uint8_t *)r.source < a.base + a.used);
            /* the same reads as v2: the name is in the header's read, the
             * rank in the head's and the payload's (4 more bytes each) */
            TEST_CHECK(reads == reads_v2);
            TEST_CHECK_(read_bytes <= bytes_v2 + 64 + 4 * 2, "%zu vs %zu", read_bytes, bytes_v2);
        }
        vjo_arena_reset(&a);
        TEST_ASSERT(!lookup(&a, &p, &cfg, "犬", 0, &r, &e) && r.n_vocab == 1);
        TEST_CHECK(r.vocab[0].rank == VJO_NO_RANK && !strcmp(r.vocab[0].meanings[0], "dog"));
        TEST_CHECK(version == 3 ? r.source && !strcmp(r.source, "Jiten") : !r.source);
        vjo_arena_reset(&a);
        TEST_ASSERT(!lookup(&a, &p, &cfg, "アントワーヌ", 0, &r, &e) && r.n_vocab == 1);
        TEST_CHECK(r.vocab[0].rank == (version == 3 ? 99999 : VJO_NO_RANK)); /* a real rank */
        /* the overlay's entry: the badge and the list's source */
        vjo_arena_reset(&a);
        out = (VjoOverlayData){.ocr_text = "猫と犬", .filtered = "猫と犬", .sentence = "猫と犬"};
        TEST_ASSERT(!vjo_overlay_lookup_at(&a, &p, &cfg, 0, &out) && out.list.n_entries == 1);
        TEST_CHECK(!strcmp(out.list.entries[0].text, version == 3 ? "猫 (ねこ) #1536\ncat" : "猫 (ねこ)\ncat"));
        TEST_CHECK(version == 3 ? !strcmp(out.list.source, "Jiten") : !out.list.source);
        vjo_arena_reset(&a);
        TEST_ASSERT(!vjo_overlay_lookup_at(&a, &p, &cfg, 6, &out) && out.list.n_entries == 1);
        TEST_CHECK(!strcmp(out.list.entries[0].text, version == 3 ? "犬 (いぬ) unranked\ndog" : "犬 (いぬ)\ndog"));
    }
    /* A located page reads its own dictionary's rank and name. */
    {
        const Row d0[] = {{"猫", "猫", "ねこ", "d0 cat", 0, 0}};
        const Row d1[] = {{"猫", "猫", "ねこ", "d1 cat", 0, 777}};
        VjoArena a; VjoPlatform p; VjoConfig cfg; VjoDictResult r, one; VjoErr e; VjoLocalPages pages;
        setup(&a, &p, &cfg); set_names(&cfg, 2);
        build(&fixtures[0], page_names[0], d0, 1, 2);
        fixture_title = "Other list";
        build(&fixtures[1], page_names[1], d1, 1, 3);
        TEST_ASSERT(!pages_lookup(&a, &p, &cfg, "猫", 0, 2, &pages, &r, &e) && pages.count == 2);
        TEST_CHECK(!r.source && r.vocab[0].rank == VJO_NO_RANK); /* page 0: the v2 file */
        TEST_ASSERT(!vjo_local_read_page(&a, &p, &cfg, &pages, 1, &one, &e));
        TEST_CHECK(one.vocab[0].rank == 777 && one.source && !strcmp(one.source, "Other list"));
        TEST_CHECK(!strcmp(one.vocab[0].meanings[0], "d1 cat"));
    }
}

/* The frequency decides between postings the text ranks alike: after an
 * entry written as the text is, a listed common reading and one with a
 * reading, and before the longer article and source order. Synthetic rows. */
static void frequency_breaks_ties_after_how_the_text_is_written(void)
{
    const Row rows[] = {
        /* equal flags: the more frequent wins over a longer article first in the file */
        {"かいとう", "解答", "かいとう", "answer, the longest article of the three", 0, 6000},
        {"かいとう", "回答", "かいとう", "reply", 0, 3000},
        {"かいとう", "怪盗", "かいとう", "phantom thief", 0, 0},
        /* written as the text beats a more frequent homograph */
        {"から", "空", "から", "empty, a frequent homograph", 0, 39553},
        {"から", "から", "", "particle: from", 0, 0},
        /* the preferred-readings table beats the frequency */
        {"私", "私", "あたし", "I (casual)", 0, 1678},
        {"私", "私", "わたし", "I", 0, 32000},
        /* an entry with a reading beats a more frequent reading-less one */
        {"人", "人", "", "the character", 0, 65},
        {"人", "人", "ひと", "person", 0, 9000},
        /* exact below rank 256, whatever the articles' lengths (a key in
         * neither built-in table) */
        {"きかん", "期間", "きかん", "period, a long article for its rank", 0, 201},
        {"きかん", "機関", "きかん", "agency", 0, 200},
        /* within 1/128 of an octave the ranks tie: the longer article */
        {"りょう", "量", "りょう", "amount", 0, 100100},
        {"りょう", "寮", "りょう", "dormitory, the longer article", 0, 100097},
        /* a ranked entry before an unranked one */
        {"もと", "元", "もと", "origin", 0, 0},
        {"もと", "本", "もと", "basis", 0, 400000},
        /* a kanji spelling's own rank before a borrowed one (the word's rank
         * in kana, bit 24): Jiten has 先 read さき at 416, さっき in kana at 221 */
        {"先", "先", "さっき", "a while ago", 0, 1u << 24 | 221u},
        {"先", "先", "さき", "ahead", 0, 416},
        /* two borrowed ranks compare as ranks */
        {"甘", "甘", "うま", "tasty", 0, 1u << 24 | 420u},
        {"甘", "甘", "あま", "sweet, a longer article", 0, 1u << 24 | 17420u},
        /* dictionary forms at the same inflection depth (かった: かう, かる, かつ,
         * one step each): the more frequent, where the longer article decided */
        {"かう", "かう", "", "to buy", 2, 500},
        {"かる", "かる", "", "to cut, an article longer than the other two", 2, 80000},
        {"かつ", "かつ", "", "to win", 2, 0}};
    static const struct { const char *text, *v2, *v3; } cases[] = {
        {"かいとう", "answer, the longest article of the three", "reply"},
        {"から", "particle: from", "particle: from"},
        {"私", "I", "I"},
        {"人", "person", "person"},
        {"きかん", "period, a long article for its rank", "agency"},
        {"りょう", "dormitory, the longer article", "dormitory, the longer article"},
        {"もと", "origin", "basis"},
        {"先", "a while ago", "ahead"},
        {"甘", "sweet, a longer article", "tasty"},
        {"かった", "to cut, an article longer than the other two", "to buy"},
        {"かう", "to buy", "to buy"}}; /* the text itself still before its inflections */
    for (int version = 2; version <= 3; version++) {
        VjoArena a; VjoPlatform p; VjoConfig cfg; VjoDictResult r; VjoErr e;
        setup(&a, &p, &cfg);
        build(&fixtures[0], "main.vjdict", rows, (int)(sizeof(rows) / sizeof(rows[0])), version);
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            vjo_arena_init(&a, arena_mem, sizeof(arena_mem));
            TEST_CASE_("v%d %s", version, cases[i].text);
            TEST_ASSERT(!lookup(&a, &p, &cfg, cases[i].text, 0, &r, &e) && r.n_vocab == 1);
            TEST_CHECK_(!strcmp(r.vocab[0].meanings[0], version == 3 ? cases[i].v3 : cases[i].v2),
                        "got %s", r.vocab[0].meanings[0]);
        }
    }
}

/* usual_spellings (local_dict.c): in a file with ranks, a key's listed
 * spelling beats a lower rank (やめる: 止める "stop" over 辞める "resign");
 * without ranks the longer article decides, as before. It only orders one
 * key's entries: the text's own entry (無くして) still comes before an
 * inflection's usual spelling (なくす: 無くす), as on main (audit-dict-freq-b). */
static void usual_spellings_order_one_keys_entries(void)
{
    const Row rows[] = {
        {"やめる", "辞める", "やめる", "to resign, an article longer than to stop's", 1, 1674},
        {"やめる", "止める", "やめる", "to stop", 1, 86863},
        {"なくして", "無くして", "なくして", "without", 0, 91039},
        {"なくす", "亡くす", "なくす", "to lose (a person)", 2, 6000},
        {"なくす", "無くす", "なくす", "to lose", 2, 8899}};
    static const struct { const char *text, *v2, *v3; } cases[] = {
        {"やめる", "to resign, an article longer than to stop's", "to stop"},
        {"やめて", "to resign, an article longer than to stop's", "to stop"},
        {"なくして", "without", "without"},
        {"なくした", "to lose (a person)", "to lose"}};
    for (int version = 2; version <= 3; version++) {
        VjoArena a; VjoPlatform p; VjoConfig cfg; VjoDictResult r; VjoErr e;
        setup(&a, &p, &cfg);
        build(&fixtures[0], "main.vjdict", rows, (int)(sizeof(rows) / sizeof(rows[0])), version);
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            vjo_arena_init(&a, arena_mem, sizeof(arena_mem));
            TEST_CASE_("v%d %s", version, cases[i].text);
            TEST_ASSERT(!lookup(&a, &p, &cfg, cases[i].text, 0, &r, &e) && r.n_vocab == 1);
            TEST_CHECK_(!strcmp(r.vocab[0].meanings[0], version == 3 ? cases[i].v3 : cases[i].v2),
                        "got %s", r.vocab[0].meanings[0]);
        }
    }
}

/* A v3 header's list name must be 1-63 bytes of text, NUL-padded; payloads
 * hold the frequency word and three strings; v2 still refuses a set spare
 * posting field, v3 ignores it (and the word's top byte). */
static void v3_format_is_checked(void)
{
    const Row row = {"猫", "猫", "ねこ", "cat", 0, 0xff000000u | 42u};
    VjoArena a; VjoPlatform p; VjoConfig cfg; VjoDictResult r; VjoErr e;
    setup(&a, &p, &cfg); build(&fixtures[0], "main.vjdict", &row, 1, 3);
    TEST_ASSERT(!lookup(&a, &p, &cfg, "猫", 0, &r, &e) && r.vocab[0].rank == 42);
    static const struct { size_t off; uint8_t value; } bad[] = {
        {64, 0},            /* an empty name */
        {64 + 6, 'x'},      /* text in the padding after the name's NUL (64 + 5)... */
        {64 + 2, 0x1f},     /* a control character */
        {64 + 63, 'x'},     /* ...or none at all, with every byte text */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        setup(&a, &p, &cfg); build(&fixtures[0], "main.vjdict", &row, 1, 3);
        if (i == 3) memset(fixtures[0].bytes + 64, 'x', 64);
        else fixtures[0].bytes[bad[i].off] = bad[i].value;
        TEST_CASE_("bad name %zu", i);
        TEST_CHECK(lookup(&a, &p, &cfg, "猫", 0, &r, &e) == VJO_E_LOCAL_FORMAT && !r.n_vocab);
        TEST_CHECK(opened == closed && a.used == 0);
    }
    /* the longest name, 63 bytes */
    setup(&a, &p, &cfg);
    fixture_title = "123456789012345678901234567890123456789012345678901234567890123";
    build(&fixtures[0], "main.vjdict", &row, 1, 3);
    TEST_ASSERT(!lookup(&a, &p, &cfg, "猫", 0, &r, &e) && r.source && strlen(r.source) == 63);
    /* a payload too short for the word and three strings */
    {
        const Row tiny = {"a", "a", "", "", 0, 7};
        setup(&a, &p, &cfg); build(&fixtures[0], "main.vjdict", &tiny, 1, 3);
        TEST_ASSERT(fixtures[0].payload_size[0] == 8);
        TEST_CHECK(!lookup(&a, &p, &cfg, "a", 0, &r, &e) && r.n_vocab == 1 && r.vocab[0].rank == 7);
        /* posting lengths 4-7: shorter than any v3 payload (4 would leave
         * nothing after the frequency word) */
        uint8_t *post = fixtures[0].bytes + 8192 + 1;
        for (uint32_t bad_len = 4; bad_len < 8; bad_len++) {
            put32(post + 8, bad_len);
            vjo_arena_reset(&a);
            TEST_CASE_("posting length %u", (unsigned)bad_len);
            TEST_CHECK(lookup(&a, &p, &cfg, "a", 0, &r, &e) == VJO_E_LOCAL_FORMAT && !r.n_vocab);
        }
    }
    /* the spare posting field: ignored in v3 (the builder sets it), refused in v2 */
    setup(&a, &p, &cfg); build(&fixtures[0], "main.vjdict", &row, 1, 2);
    TEST_ASSERT(!lookup(&a, &p, &cfg, "猫", 0, &r, &e));
    put16(fixtures[0].bytes + 8192 + strlen("猫") + 14, 1);
    vjo_arena_reset(&a);
    TEST_CHECK(lookup(&a, &p, &cfg, "猫", 0, &r, &e) == VJO_E_LOCAL_FORMAT);
    /* a v2 magic with version 3, or the other way round */
    setup(&a, &p, &cfg); build(&fixtures[0], "main.vjdict", &row, 1, 3);
    memcpy(fixtures[0].bytes, "VJDICT2\0", 8);
    TEST_CHECK(lookup(&a, &p, &cfg, "猫", 0, &r, &e) == VJO_E_LOCAL_FORMAT);
    setup(&a, &p, &cfg); build(&fixtures[0], "main.vjdict", &row, 1, 2);
    put32(fixtures[0].bytes + 8, 3);
    TEST_CHECK(lookup(&a, &p, &cfg, "猫", 0, &r, &e) == VJO_E_LOCAL_FORMAT);
}

/* The rank reaches the card: the note (core/anki.c), its queue record and
 * the Lapis fields the .apkg export writes (core/lapis.c), with the list's
 * name as the frequency source; an unranked entry records 0, leaves
 * Frequency empty and sorts last (FreqSort 9999999, as Yomitan's). */
static void v3_rank_reaches_the_anki_card(void)
{
    const Row rows[] = {{"猫", "猫", "ねこ", "cat", 0, 1536}, {"犬", "犬", "いぬ", "dog", 0, 0}};
    static const struct { const char *text; int rank; const char *record, *sort, *freq; } cases[] = {
        {"猫が", 1536, ",\"rank\":1536,", "1536", "<ul style=\"text-align: left;\"><li>Jiten: 1536</li></ul>"},
        {"犬が", VJO_NO_RANK, ",\"rank\":0,", "9999999", ""}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        VjoArena a; VjoPlatform p; VjoConfig cfg; VjoOverlayData out; VjoAnkiNote note, back;
        char *json, *sort, *freq;
        int picture;
        setup(&a, &p, &cfg); build(&fixtures[0], "main.vjdict", rows, 2, 3);
        out = (VjoOverlayData){.ocr_text = cases[i].text, .filtered = cases[i].text, .sentence = cases[i].text};
        TEST_CASE(cases[i].text);
        TEST_ASSERT(!vjo_overlay_lookup_at(&a, &p, &cfg, 0, &out) && out.list.n_entries == 1);
        TEST_ASSERT(!vjo_anki_note_from_entry(&a, &out.list, 0, &note));
        TEST_CHECK(note.rank == cases[i].rank && note.source && !strcmp(note.source, "Jiten"));
        json = vjo_queue_encode(&a, &note, 0);
        TEST_ASSERT(json != NULL);
        TEST_CHECK(strstr(json, "{\"version\":4,") == json && strstr(json, cases[i].record) &&
                   strstr(json, ",\"source\":\"Jiten\","));
        TEST_MSG("%s", json);
        TEST_CHECK(vjo_queue_decode(&a, json, strlen(json), &back, &picture) == 4 && back.rank == cases[i].rank);
        sort = vjo_lapis_field_value(&a, VJO_LAPIS_FREQ_SORT, &back, NULL);
        freq = vjo_lapis_field_value(&a, VJO_LAPIS_FREQUENCY, &back, NULL);
        TEST_CHECK(sort && !strcmp(sort, cases[i].sort));
        TEST_CHECK(freq && !strcmp(freq, cases[i].freq));
        TEST_MSG("%s / %s", sort ? sort : "(null)", freq ? freq : "(null)");
    }
}

/* The whole-text lookup (the host CLI's, vjo_overlay_lookup) can mix
 * dictionaries: it names a frequency list only when every entry's file has
 * it, so a v2 file's entry shows no "unranked" and a ranked one its rank. */
static void whole_text_lookup_names_a_list_every_entry_has(void)
{
    const Row d0[] = {{"猫", "猫", "ねこ", "cat", 0, 0}};
    const Row d1[] = {{"犬", "犬", "いぬ", "dog", 0, 900}, {"鳥", "鳥", "とり", "bird", 0, 0}};
    static const struct { const char *text; int v2; const char *source, *first, *second; } cases[] = {
        {"猫犬", 1, NULL, "猫 (ねこ)\ncat", "犬 (いぬ) #900\ndog"},
        {"犬鳥", 0, "Jiten", "犬 (いぬ) #900\ndog", "鳥 (とり) unranked\nbird"}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        /* that path's 64 KiB result budget and its scratch, beside the cursor's */
        static uint8_t big[VJO_LOCAL_RESULT_BUDGET + VJO_LOCAL_SCRATCH_BUDGET + 16384];
        VjoArena a; VjoPlatform p; VjoConfig cfg; VjoDictResult r; VjoErr e; VjoEntryList l;
        setup(&a, &p, &cfg); set_names(&cfg, 2);
        vjo_arena_init(&a, big, sizeof(big));
        build(&fixtures[0], page_names[0], d0, 1, 2);
        build(&fixtures[1], page_names[1], d1, 2, 3);
        TEST_CASE(cases[i].text);
        memset(&e, 0, sizeof(e));
        TEST_ASSERT_(!vjo_local_lookup(&a, &p, &cfg, cases[i].text, &r, &e) && r.n_vocab == 2, "rc %d", e.rc);
        TEST_CHECK(cases[i].source ? r.source && !strcmp(r.source, cases[i].source) : !r.source);
        TEST_ASSERT(vjo_entries_build(&a, cases[i].text, &r, &l) == 0 && l.n_entries == 2);
        TEST_CHECK_(!strcmp(l.entries[0].text, cases[i].first), "got %s", l.entries[0].text);
        TEST_CHECK_(!strcmp(l.entries[1].text, cases[i].second), "got %s", l.entries[1].text);
        TEST_CHECK(opened == closed);
    }
}

/* A located page that is too short for any payload (v3: the frequency word and
 * three strings, 8 B; v2: 4 B) is refused before its payload is read: a page
 * locator is the caller's memory, and read_entry must never see such a length. */
/* Page 0 bytes 192..255 of a v2 or v3 file: the dictionary's own name
 * (bee-san/plaintext-dicts writes "Bee's Ultimate Dictionary"), read with the
 * header in its one read and given to every entry of that file, copied into
 * the result after it, and to a located page. A file without
 * one names none; v1 has index records there; a malformed name refuses the
 * file, as a malformed frequency list name does. */
static void dictionary_title_names_its_entries(void)
{
    static const char title[] = "Bee's Ultimate Dictionary";
    const Row rows[] = {{"木", "木", "き", "a tree\n木=tree, wood", 0, 1459},
                        {"日本語", "日本語", "にほんご", "Japanese\n日=day\n本=book\n語=word", 0, 4372}};
    for (int version = 2; version <= 3; version++) {
        int plain_reads = 0;
        for (int titled = 0; titled <= 1; titled++) {
            VjoArena a; VjoPlatform p; VjoConfig cfg; VjoDictResult r; VjoErr e;
            setup(&a, &p, &cfg); fixture_dict_title = titled ? title : NULL;
            build(&fixtures[0], "main.vjdict", rows, 2, version);
            TEST_CASE_("v%d %s", version, titled ? "titled" : "untitled");
            TEST_ASSERT(!lookup(&a, &p, &cfg, "木", 0, &r, &e) && r.n_vocab == 1);
            if (!titled) {
                TEST_CHECK(!r.vocab[0].dictionary);
                plain_reads = reads;
            } else {
                TEST_ASSERT(r.vocab[0].dictionary && !strcmp(r.vocab[0].dictionary, title));
                TEST_CHECK((const uint8_t *)r.vocab[0].dictionary >= a.base &&
                           (const uint8_t *)r.vocab[0].dictionary < a.base + a.used); /* in the result */
                TEST_CHECK(reads == plain_reads); /* the header's own read */
            }
            /* the whole-text lookup (its 64 KiB result budget and scratch):
             * each entry names it */
            static uint8_t big[VJO_LOCAL_RESULT_BUDGET + VJO_LOCAL_SCRATCH_BUDGET + 16384];
            vjo_arena_init(&a, big, sizeof(big));
            TEST_ASSERT_(!vjo_local_lookup(&a, &p, &cfg, "木と日本語", &r, &e) && r.n_vocab == 2, "rc %d", e.rc);
            TEST_CHECK(titled ? r.vocab[0].dictionary && r.vocab[1].dictionary &&
                                    !strcmp(r.vocab[0].dictionary, title) && !strcmp(r.vocab[1].dictionary, title)
                              : !r.vocab[0].dictionary && !r.vocab[1].dictionary);
            TEST_CHECK(opened == closed);
        }
    }
    /* v1: 8 index records from byte 64 cover bytes 192..255; no name is read there */
    {
        static const char *const keys[] = {"あ", "い", "う", "え", "お", "か", "き", "く"};
        Row many[8];
        VjoArena a; VjoPlatform p; VjoConfig cfg; VjoDictResult r; VjoErr e;
        for (int i = 0; i < 8; i++) many[i] = (Row){keys[i], keys[i], "", "kana", 0, 0};
        setup(&a, &p, &cfg); build(&fixtures[0], "main.vjdict", many, 8, 1);
        TEST_ASSERT(fixtures[0].bytes[192] || fixtures[0].bytes[200]);
        TEST_ASSERT(!lookup(&a, &p, &cfg, "く", 0, &r, &e) && r.n_vocab == 1 && !r.vocab[0].dictionary);
    }
    /* a located page names its own file's title, or none */
    {
        const Row d0[] = {{"猫", "猫", "ねこ", "d0 cat", 0, 0}};
        const Row d1[] = {{"猫", "猫", "ねこ", "d1 cat", 0, 0}};
        const Row d2[] = {{"猫", "猫", "ねこ", "d2 cat", 0, 5}};
        const char *names[] = {"First", NULL, "Third"};
        VjoArena a; VjoPlatform p; VjoConfig cfg; VjoDictResult r, one; VjoErr e; VjoLocalPages pages;
        setup(&a, &p, &cfg); set_names(&cfg, 3);
        fixture_dict_title = names[0]; build(&fixtures[0], page_names[0], d0, 1, 3);
        fixture_dict_title = names[1]; build(&fixtures[1], page_names[1], d1, 1, 2);
        fixture_dict_title = names[2]; build(&fixtures[2], page_names[2], d2, 1, 3);
        TEST_ASSERT(!pages_lookup(&a, &p, &cfg, "猫", 0, 3, &pages, &r, &e) && pages.count == 3);
        TEST_CHECK(r.vocab[0].dictionary && !strcmp(r.vocab[0].dictionary, "First"));
        for (int page = 0; page < 3; page++) {
            int d = pages.page[page].dict;
            TEST_ASSERT(!vjo_local_read_page(&a, &p, &cfg, &pages, page, &one, &e));
            TEST_CHECK_(names[d] ? one.vocab[0].dictionary && !strcmp(one.vocab[0].dictionary, names[d])
                                 : !one.vocab[0].dictionary, "page %d (file %d)", page, d);
        }
    }
    /* malformed: text after its NUL padding, a control character or DEL, no NUL at all */
    static const struct { size_t off; uint8_t value; } bad[] = {
        {192 + 5, 'x'}, {192 + 1, 0x1f}, {192 + 2, 0x7f}, {192 + 63, 'x'},
    };
    for (int version = 2; version <= 3; version++)
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            VjoArena a; VjoPlatform p; VjoConfig cfg; VjoDictResult r; VjoErr e;
            setup(&a, &p, &cfg); fixture_dict_title = "abc";
            build(&fixtures[0], "main.vjdict", rows, 2, version);
            if (i == 3) memset(fixtures[0].bytes + 192, 'x', 64);
            else fixtures[0].bytes[bad[i].off] = bad[i].value;
            TEST_CASE_("v%d bad title %zu", version, i);
            TEST_CHECK(lookup(&a, &p, &cfg, "木", 0, &r, &e) == VJO_E_LOCAL_FORMAT && !r.n_vocab);
            TEST_CHECK(opened == closed && a.used == 0);
        }
    /* vjo_local_title (the Overlay app's card): page 0 of 256 bytes or more, a v2/v3
     * magic and its own version, else no name */
    {
        VjoArena a; VjoPlatform p; VjoConfig cfg;
        char out[VJO_LOCAL_TITLE_SIZE];
        setup(&a, &p, &cfg); fixture_dict_title = title;
        build(&fixtures[0], "main.vjdict", rows, 2, 3);
        uint8_t page0[256];
        memcpy(page0, fixtures[0].bytes, sizeof(page0));
        TEST_CHECK(vjo_local_title(page0, sizeof(page0), out) == 1 && !strcmp(out, title));
        TEST_CHECK(vjo_local_title(page0, 255, out) == 0 && !out[0]); /* a short read */
        TEST_CHECK(vjo_local_title(page0, 64, out) == 0 && !out[0]);
        page0[8] = 2; /* VJDICT3 that says version 2 */
        TEST_CHECK(vjo_local_title(page0, sizeof(page0), out) == 0 && !out[0]);
        page0[8] = 3; page0[6] = '4'; /* an unknown version */
        TEST_CHECK(vjo_local_title(page0, sizeof(page0), out) == 0 && !out[0]);
        fixture_dict_title = NULL;
    }
}

static void read_page_refuses_short_locators(void)
{
    const Row row = {"猫", "猫", "ねこ", "cat", 0, 1536};
    for (int version = 2; version <= 3; version++) {
        VjoArena a; VjoPlatform p; VjoConfig cfg; VjoDictResult r, one; VjoErr e; VjoLocalPages pages;
        uint32_t shortest = version == 3 ? 8 : 4;
        setup(&a, &p, &cfg); set_names(&cfg, 2);
        build(&fixtures[0], page_names[0], &row, 1, version);
        build(&fixtures[1], page_names[1], &row, 1, version);
        TEST_ASSERT(!pages_lookup(&a, &p, &cfg, "猫", 0, 2, &pages, &r, &e) && pages.count == 2);
        size_t used = a.used;
        for (uint32_t len = 0; len < shortest; len++) {
            VjoLocalPages bad = pages;
            bad.page[1].entry_len = len;
            payload_touches = 0;
            TEST_CASE_("v%d length %u", version, (unsigned)len);
            TEST_CHECK(vjo_local_read_page(&a, &p, &cfg, &bad, 1, &one, &e) == VJO_E_LOCAL_FORMAT);
            TEST_CHECK(!one.n_vocab && !payload_touches && a.used == used && opened == closed);
        }
        /* the real length still reads */
        TEST_ASSERT(!vjo_local_read_page(&a, &p, &cfg, &pages, 1, &one, &e) && one.n_vocab == 1);
        TEST_CHECK(one.vocab[0].rank == (version == 3 ? 1536 : VJO_NO_RANK) && !strcmp(one.vocab[0].meanings[0], "cat"));
    }
}

/* One lookup's shown entry as text (spelling, reading, meaning, span), and its reads. */
static int shown(const char *name, const char *text, size_t cursor, char *out, size_t cap)
{
    VjoArena a; VjoPlatform p; VjoConfig cfg; VjoDictResult r; VjoErr e;
    int before = reads;
    vjo_arena_init(&a, arena_mem, sizeof(arena_mem));
    memset(&p, 0, sizeof(p)); p.file_open = open_file;
    vjo_config_defaults(&cfg); cfg.dictionary = VJO_DICT_LOCAL;
    strcpy(cfg.local_dictionary_dir, "fixture"); strcpy(cfg.local_dictionaries, name);
    payload_reads = 0;
    if (lookup(&a, &p, &cfg, text, cursor, &r, &e)) snprintf(out, cap, "rc %d", e.rc);
    else if (!r.n_vocab) snprintf(out, cap, "none");
    else snprintf(out, cap, "%s|%s|%s|%d", r.vocab[0].spelling, r.vocab[0].reading, r.vocab[0].meanings[0],
                  r.tokens[0].len16);
    return reads - before;
}

/* Every shown entry is the one the plain file shows, in format 2 and in format 3
 * (the bundle's). This fixture's index is one page, which the page cache keeps,
 * so the filter can only add its own block's read; the reads it saves on larger
 * indexes are measured by the converter's tests and tools/dict_lookup_bench.py. */
static void key_filter_shows_what_the_plain_file_shows(void)
{
    const Row rows[] = {{"食べる", "食べる", "たべる", "eat", 1, 0}, {"たべる", "食べる", "たべる", "eat", 1, 0},
        {"猫", "猫", "ねこ", "cat", 0, 1536}, {"猫舌", "猫舌", "ねこじた", "cat tongue", 0, 0},
        {"する", "する", "", "do", 4, 0}, {"見る", "見る", "みる", "see", 1, 0}};
    const char *texts[] = {"食べませんでした。", "たべたくなかった", "猫舌です", "猫", "見ない", "しました", "霻霻霻"};
    const uint32_t one[] = {0};
    char plain[256], filtered[256];
    VjoArena a; VjoPlatform p; VjoConfig cfg;
    for (int version = 2; version <= 3; version++) {
        setup(&a, &p, &cfg);
        build(&fixtures[0], "plain.vjdict", rows, 6, version);
        build(&fixtures[1], "main.vjdict", rows, 6, version);
        add_filter(&fixtures[1], rows, 6, one, 1, 0);
        for (size_t t = 0; t < sizeof(texts) / sizeof(texts[0]); t++) {
            int plain_reads = shown("plain.vjdict", texts[t], 0, plain, sizeof(plain));
            int filtered_reads = shown("main.vjdict", texts[t], 0, filtered, sizeof(filtered));
            TEST_CHECK_(!strcmp(plain, filtered), "v%d %s: %s vs %s", version, texts[t], plain, filtered);
            TEST_CHECK_(filtered_reads <= plain_reads + 1, "v%d %s: %d reads, plain %d", version, texts[t],
                        filtered_reads, plain_reads);
        }
        TEST_CHECK(opened == closed);
    }
}

/* No false negative: a candidate whose prefix is in another block (した -> する), a
 * one-character candidate and a full filter all search the index as before, in
 * format 2 and in format 3. */
static void key_filter_never_hides_a_key(void)
{
    /* 𠮟る and 😀😀: code points beyond the BMP clamp to 0xffff in the prefix
     * (😀😀's prefix, 0xffffffff, is above every block's range: never filtered) */
    const Row rows[] = {{"する", "する", "", "do", 4, 0}, {"した", "下", "した", "below", 0, 0},
        {"猫", "猫", "ねこ", "cat", 0, 1536}, {"手", "手", "て", "hand", 0, 0},
        {"\xf0\xa0\xae\x9fる", "\xf0\xa0\xae\x9fる", "しかる", "scold", 2, 0},
        {"\xf0\x9f\x98\x80\xf0\x9f\x98\x80", "\xf0\x9f\x98\x80\xf0\x9f\x98\x80", "", "two smileys", 0, 0}};
    uint32_t split[2] = {0, prefix_of("す")};
    char plain[256], filtered[256];
    VjoArena a; VjoPlatform p; VjoConfig cfg;
    for (int version = 2; version <= 3; version++) {
        setup(&a, &p, &cfg);
        build(&fixtures[0], "plain.vjdict", rows, 6, version);
        for (int fill = 0; fill < 3; fill++) {
            const char *texts[] = {"しました", "した", "猫", "手を", "\xf0\xa0\xae\x9fった", "\xf0\x9f\x98\x80\xf0\x9f\x98\x80!"};
            build(&fixtures[1], "main.vjdict", rows, 6, version);
            add_filter(&fixtures[1], rows, 6, split, 2, fill);
            for (size_t t = 0; t < sizeof(texts) / sizeof(texts[0]); t++) {
                shown("plain.vjdict", texts[t], 0, plain, sizeof(plain));
                shown("main.vjdict", texts[t], 0, filtered, sizeof(filtered));
                if (fill == 2 && t == 4) {
                    /* A zeroed (corrupt) block hides the keys of its range, 𠮟る here: the
                     * proof that this format's lookups consult the filter at all. */
                    TEST_CHECK_(!strcmp(filtered, "none"), "v%d %s: %s, not hidden by a zeroed block", version,
                                texts[t], filtered);
                    continue;
                }
                TEST_CHECK_(!strcmp(plain, filtered), "v%d fill %d %s: %s vs %s", version, fill, texts[t], plain, filtered);
            }
        }
        /* しました: its own block holds no する, which is found in the other block's range by the index. */
        shown("main.vjdict", "しました", 0, filtered, sizeof(filtered));
        TEST_CHECK_(!strncmp(filtered, "する|", strlen("する|")), "v%d %s", version, filtered);
    }
}

/* The descriptor and directory are checked when the file is opened, in format 2
 * and in format 3; other bytes there (no magic) mean no filter. */
static void key_filter_descriptor_is_validated(void)
{
    const Row rows[] = {{"猫舌", "猫舌", "ねこじた", "cat tongue", 0, 0}, {"猫", "猫", "ねこ", "cat", 0, 1536}};
    const uint32_t two[] = {0, prefix_of("猫")};
    char out[256], want[32];
    VjoArena a; VjoPlatform p; VjoConfig cfg;
    struct { size_t at; int width; uint64_t value; } bad[] = {
        {132, 4, 2}, {136, 4, 0}, {136, 4, 481}, {140, 4, 0}, {140, 4, 17}, {144, 8, 8193},
        {144, 8, 4096}, {136, 4, 1}};
    snprintf(want, sizeof(want), "rc %d", VJO_E_LOCAL_FORMAT);
    for (int version = 2; version <= 3; version++) {
        setup(&a, &p, &cfg);
        build(&fixtures[0], "main.vjdict", rows, 2, version);
        add_filter(&fixtures[0], rows, 2, two, 2, 0);
        shown("main.vjdict", "猫舌", 0, out, sizeof(out));
        TEST_CHECK_(!strcmp(out, "猫舌|ねこじた|cat tongue|2"), "v%d %s", version, out);
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            Fixture saved = fixtures[0];
            uint8_t *at = fixtures[0].bytes + bad[i].at;
            if (bad[i].width == 4) put32(at, (uint32_t)bad[i].value); else put64(at, bad[i].value);
            shown("main.vjdict", "猫舌", 0, out, sizeof(out));
            TEST_CHECK_(!strcmp(out, want), "v%d field at %zu = %llu: %s", version, bad[i].at,
                        (unsigned long long)bad[i].value, out);
            fixtures[0] = saved;
        }
        {
            /* An offset past the file whose size difference wraps to exactly 900
             * blocks (a page multiple at or above the data): refused. */
            Fixture saved = fixtures[0];
            put32(fixtures[0].bytes + 136, 900);
            put64(fixtures[0].bytes + 144, (uint64_t)fixtures[0].size - UINT64_C(900) * 4096);
            shown("main.vjdict", "猫舌", 0, out, sizeof(out));
            TEST_CHECK_(!strcmp(out, want), "v%d wrapped offset: %s", version, out);
            fixtures[0] = saved;
        }
        memcpy(fixtures[0].bytes + 128, "XXXX", 4); /* no magic: the blocks are just more data */
        shown("main.vjdict", "猫舌", 0, out, sizeof(out));
        TEST_CHECK_(!strcmp(out, "猫舌|ねこじた|cat tongue|2"), "v%d %s", version, out);
    }
}

/* Posting hints rank every posting exactly as its payload's head does (#127's
 * tie-break cases, katakana text, hiragana text with a more frequent katakana
 * spelling, a preferred reading, a usual spelling), with fewer reads; without
 * the descriptor the u16 is ignored (0xa5a5 in the fixture). */
static void posting_hints_rank_as_the_head_does(void)
{
    const Row rows[] = {
        {"かいとう", "解答", "かいとう", "answer, the longest article of the three", 0, 6000},
        {"かいとう", "回答", "かいとう", "reply", 0, 3000},
        {"かいとう", "怪盗", "かいとう", "phantom thief", 0, 0},
        {"から", "空", "から", "empty, a frequent homograph", 0, 39553},
        {"から", "から", "", "particle: from", 0, 0},
        {"私", "私", "あたし", "I (casual)", 0, 1678},
        {"私", "私", "わたし", "I", 0, 32000},
        {"人", "人", "", "the character", 0, 65},
        {"人", "人", "ひと", "person", 0, 9000},
        {"せい", "性", "せい", "nature, a long article for its rank", 0, 201},
        {"せい", "所為", "せい", "because of", 0, 200},
        {"もと", "元", "もと", "origin", 0, 0},
        {"もと", "本", "もと", "basis", 0, 400000},
        {"先", "先", "さっき", "a while ago", 0, 1u << 24 | 221u},
        {"先", "先", "さき", "ahead", 0, 416},
        {"かう", "かう", "", "to buy", 2, 500},
        {"かる", "かる", "", "to cut, an article longer than the other two", 2, 80000},
        /* katakana text: a frequent hiragana spelling, a rare katakana one (written
         * as the text is: it wins, which only its head can tell) and a kanji one */
        {"ねこ", "ねこ", "", "hiragana spelling", 0, 50},
        {"ねこ", "ネコ", "", "katakana spelling", 0, 90000},
        {"ねこ", "猫", "ねこ", "kanji spelling", 0, 10},
        /* hiragana text: its own spelling wins over a more frequent katakana one,
         * which only normalizes to the key (HINT_SPELLING, not HINT_NORMALIZED) */
        {"いぬ", "イヌ", "", "katakana spelling, more frequent", 0, 40},
        {"いぬ", "いぬ", "", "hiragana spelling", 0, 9000},
        /* a usual spelling (usual_spellings: やめる 止める) beats a more frequent one */
        {"やめる", "辞める", "やめる", "to quit", 1, 500},
        {"やめる", "止める", "やめる", "to stop", 1, 2000}};
    const char *texts[] = {"かいとう", "から", "私", "人", "せい", "もと", "先", "かった", "かう", "ネコ", "ねこ", "猫",
                           "いぬ", "イヌ", "やめる"};
    for (size_t t = 0; t < sizeof(texts) / sizeof(texts[0]); t++) {
        char plain[256], hinted[256];
        int plain_reads, hinted_reads;
        VjoArena a; VjoPlatform p; VjoConfig cfg;
        setup(&a, &p, &cfg);
        build(&fixtures[0], "plain.vjdict", rows, (int)(sizeof(rows) / sizeof(rows[0])), 3);
        build(&fixtures[1], "main.vjdict", rows, (int)(sizeof(rows) / sizeof(rows[0])), 3);
        add_hints(&fixtures[1]);
        TEST_CASE(texts[t]);
        plain_reads = shown("plain.vjdict", texts[t], 0, plain, sizeof(plain));
        hinted_reads = shown("main.vjdict", texts[t], 0, hinted, sizeof(hinted));
        TEST_CHECK_(!strcmp(plain, hinted), "%s vs %s", plain, hinted);
        TEST_CHECK_(hinted_reads <= plain_reads, "%d reads, %d without hints", hinted_reads, plain_reads);
        if (!strcmp(texts[t], "いぬ")) /* written as the text is, before the more frequent イヌ */
            TEST_CHECK_(!strncmp(hinted, "いぬ|", strlen("いぬ|")), "%s", hinted);
        if (!strcmp(texts[t], "イヌ"))
            TEST_CHECK_(!strncmp(hinted, "イヌ|", strlen("イヌ|")), "%s", hinted);
        /* a wrong magic: the u16 is ignored, as #127's reader does */
        memcpy(fixtures[1].bytes + 160, "XXXX", 4);
        shown("main.vjdict", texts[t], 0, hinted, sizeof(hinted));
        TEST_CHECK_(!strcmp(plain, hinted), "no descriptor: %s vs %s", plain, hinted);
    }
    {
        /* a descriptor of another version is refused */
        VjoArena a; VjoPlatform p; VjoConfig cfg;
        char out[256], want[32];
        setup(&a, &p, &cfg);
        build(&fixtures[0], "main.vjdict", rows, (int)(sizeof(rows) / sizeof(rows[0])), 3);
        add_hints(&fixtures[0]);
        put32(fixtures[0].bytes + 164, 2);
        shown("main.vjdict", "から", 0, out, sizeof(out));
        snprintf(want, sizeof(want), "rc %d", VJO_E_LOCAL_FORMAT);
        TEST_CHECK_(!strcmp(out, want), "%s", out);
    }
}

TEST_LIST = {
    {"posting_hints_rank_as_the_head_does", posting_hints_rank_as_the_head_does},
    {"key_filter_shows_what_the_plain_file_shows", key_filter_shows_what_the_plain_file_shows},
    {"key_filter_never_hides_a_key", key_filter_never_hides_a_key},
    {"key_filter_descriptor_is_validated", key_filter_descriptor_is_validated},
    {"read_page_refuses_short_locators", read_page_refuses_short_locators},
    {"dictionary_title_names_its_entries", dictionary_title_names_its_entries},
    {"whole_text_lookup_names_a_list_every_entry_has", whole_text_lookup_names_a_list_every_entry_has},
    {"v3_rank_reaches_the_anki_card", v3_rank_reaches_the_anki_card},
    {"v3_rank_and_source_come_with_the_entry", v3_rank_and_source_come_with_the_entry},
    {"frequency_breaks_ties_after_how_the_text_is_written", frequency_breaks_ties_after_how_the_text_is_written},
    {"usual_spellings_order_one_keys_entries", usual_spellings_order_one_keys_entries},
    {"v3_format_is_checked", v3_format_is_checked},
    {"pages_follow_best_match_then_priority", pages_follow_best_match_then_priority},
    {"exact_later_dictionary_precedes_inflected_page", exact_later_dictionary_precedes_inflected_page},
    {"postings_are_ranked_by_how_the_text_is_written", postings_are_ranked_by_how_the_text_is_written},
    {"written_entry_in_later_dictionary_precedes_homograph", written_entry_in_later_dictionary_precedes_homograph},
    {"katakana_text_prefers_its_spelling_then_hiragana", katakana_text_prefers_its_spelling_then_hiragana},
    {"single_result_reads_are_unchanged", single_result_reads_are_unchanged},
    {"overlay_pages_stream_through_one_arena", overlay_pages_stream_through_one_arena},
    {"page_failures_keep_header_and_locators", page_failures_keep_header_and_locators},
    {"eight_dictionaries_only_matching_pages", eight_dictionaries_only_matching_pages},
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
