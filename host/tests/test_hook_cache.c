#include "acutest.h"
#include "hook_cache.h"
#include <stdio.h>
#include <string.h>

static VjoHookCacheKey key(unsigned game)
{
    VjoHookCacheKey k = {0};
    snprintf(k.title, sizeof(k.title), "PCSG%05u", game);
    memset(k.build, 0xA1, sizeof(k.build));
    memset(k.definition, 0xB2, sizeof(k.definition));
    k.mode = 2;
    return k;
}

static VjoHookLearned learned(unsigned interval)
{
    VjoHookLearned v = {1, 0x12345, 2, interval, 0};
    return v;
}

static void roundtrip_and_no_rewrite(void)
{
    uint8_t data[VJO_HOOK_CACHE_BYTES], before[VJO_HOOK_CACHE_BYTES];
    VjoHookCacheKey k = key(1);
    VjoHookLearned v = learned(8), out;
    memset(data, 0xFF, sizeof(data));
    TEST_CHECK(vjo_hook_cache_put(data, 0, &k, &v) == 1);
    TEST_CHECK(vjo_hook_cache_get(data, sizeof(data), &k, &out) == 1);
    TEST_CHECK(!memcmp(&out, &v, sizeof(v)));
    memcpy(before, data, sizeof(data));
    TEST_CHECK(vjo_hook_cache_put(data, sizeof(data), &k, &v) == 0);
    TEST_CHECK(!memcmp(before, data, sizeof(data)));
    v.interval = 32;
    TEST_CHECK(vjo_hook_cache_put(data, sizeof(data), &k, &v) == 1);
    TEST_CHECK(vjo_hook_cache_get(data, sizeof(data), &k, &out) == 1 && out.interval == 32);
    v.site = 0; v.key = 0x76543; v.encoding = 3;
    TEST_CHECK(vjo_hook_cache_put(data, sizeof(data), &k, &v) == 1);
    TEST_CHECK(vjo_hook_cache_get(data, sizeof(data), &k, &out) == 1 && !memcmp(&v, &out, sizeof(v)));
    TEST_CHECK(vjo_hook_cache_put(data, sizeof(data), &k, NULL) == 1);
    TEST_CHECK(vjo_hook_cache_get(data, sizeof(data), &k, &out) == 0);
    TEST_CHECK(vjo_hook_cache_put(data, sizeof(data), &k, NULL) == 0);
}

static void invalidation(void)
{
    uint8_t data[VJO_HOOK_CACHE_BYTES] = {0};
    VjoHookCacheKey k = key(1), other;
    VjoHookLearned v = learned(32), out;
    TEST_CHECK(vjo_hook_cache_put(data, 0, &k, &v) == 1);
    other = key(2);
    TEST_CHECK(!vjo_hook_cache_get(data, sizeof(data), &other, &out));
    other = k; other.build[31] ^= 1;
    TEST_CHECK(!vjo_hook_cache_get(data, sizeof(data), &other, &out));
    other = k; other.definition[31] ^= 1;
    TEST_CHECK(!vjo_hook_cache_get(data, sizeof(data), &other, &out));
    other = k; other.mode ^= 0x100;
    TEST_CHECK(!vjo_hook_cache_get(data, sizeof(data), &other, &out));
    TEST_CHECK(vjo_hook_cache_put(data, sizeof(data), &other, &v) == 1);
    TEST_CHECK(!vjo_hook_cache_get(data, sizeof(data), &k, &out));
    TEST_CHECK(vjo_hook_cache_get(data, sizeof(data), &other, &out));
    /* An unknown executable/revision must never produce reusable confidence. */
    memset(other.build, 0, 32);
    TEST_CHECK(vjo_hook_cache_put(data, sizeof(data), &other, &v) == -1);
    other = k; memset(other.definition, 0, 32);
    TEST_CHECK(vjo_hook_cache_put(data, sizeof(data), &other, &v) == -1);
}

static void bounded_eviction(void)
{
    uint8_t data[VJO_HOOK_CACHE_BYTES] = {0};
    VjoHookLearned v = learned(16), out;
    for (unsigned i = 1; i <= VJO_HOOK_CACHE_ENTRIES; i++) {
        VjoHookCacheKey k = key(i);
        TEST_CHECK(vjo_hook_cache_put(data, sizeof(data), &k, &v) == 1);
    }
    /* Changing an old title refreshes its place; merely reopening it does not write. */
    VjoHookCacheKey first = key(1), second = key(2), newest = key(33);
    v.interval = 32;
    TEST_CHECK(vjo_hook_cache_put(data, sizeof(data), &first, &v) == 1);
    TEST_CHECK(vjo_hook_cache_put(data, sizeof(data), &newest, &v) == 1);
    TEST_CHECK(vjo_hook_cache_get(data, sizeof(data), &first, &out));
    TEST_CHECK(!vjo_hook_cache_get(data, sizeof(data), &second, &out));
    for (unsigned i = 3; i <= 33; i++) {
        VjoHookCacheKey k = key(i);
        TEST_CHECK(vjo_hook_cache_get(data, sizeof(data), &k, &out));
    }
}

static void corrupt_and_truncated(void)
{
    uint8_t data[VJO_HOOK_CACHE_BYTES] = {0}, broken[VJO_HOOK_CACHE_BYTES];
    VjoHookCacheKey k = key(1);
    VjoHookLearned v = learned(32), out;
    TEST_CHECK(!vjo_hook_cache_get(NULL, 0, &k, &out));
    TEST_CHECK(!vjo_hook_cache_get(data, 0, &k, &out));
    TEST_CHECK(vjo_hook_cache_put(data, 0, &k, &v) == 1);
    for (size_t n = 0; n < sizeof(data); n++) TEST_CHECK(!vjo_hook_cache_get(data, n, &k, &out));
    TEST_CHECK(!vjo_hook_cache_get(data, sizeof(data) + 1, &k, &out));
    /* A changed byte in any header or payload position fails closed. */
    for (size_t i = 0; i < sizeof(data); i++) {
        memcpy(broken, data, sizeof(data)); broken[i] ^= 0x80;
        TEST_CHECK(!vjo_hook_cache_get(broken, sizeof(broken), &k, &out));
    }
    memset(data, 0x73, sizeof(data));
    TEST_CHECK(vjo_hook_cache_put(data, sizeof(data), &k, &v) == 1);
    TEST_CHECK(vjo_hook_cache_get(data, sizeof(data), &k, &out));
    v.interval = 64;
    TEST_CHECK(vjo_hook_cache_put(data, sizeof(data), &k, &v) == -1);
    v = learned(8); v.site = 2;
    TEST_CHECK(vjo_hook_cache_put(data, sizeof(data), &k, &v) == -1);
}

/* The file's checksum after a test changed a byte (core/hook_cache.c's: CRC-32 of the records, at +12) */
static void reseal(uint8_t *data)
{
    uint32_t crc = ~0u;
    for (unsigned i = 16; i < VJO_HOOK_CACHE_BYTES; i++) {
        crc ^= data[i];
        for (unsigned b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    crc = ~crc;
    for (unsigned i = 0; i < 4; i++) data[12 + i] = (uint8_t)(crc >> (8 * i));
}

/* #338's audit F1: a record keeps whether its source was saved trusted (bit 1 of its site word) apart from its
 * interval: the same interval with the bit changed is a change (one write), the bit round-trips with either kind of
 * source, and a site word past the two bits, or a trust value other than 0 and 1, is refused. */
static void trusted_bit(void)
{
    uint8_t data[VJO_HOOK_CACHE_BYTES] = {0}, before[VJO_HOOK_CACHE_BYTES];
    VjoHookCacheKey k = key(1);
    VjoHookLearned v = learned(32), out;
    for (unsigned site = 0; site < 2; site++) {
        v.site = site;
        v.trusted = 1;
        TEST_CHECK(vjo_hook_cache_put(data, sizeof(data), &k, &v) == 1);
        TEST_CHECK(vjo_hook_cache_get(data, sizeof(data), &k, &out) == 1 && !memcmp(&out, &v, sizeof(v)));
        TEST_CHECK(out.site == site && out.trusted == 1 && out.interval == 32);
        memcpy(before, data, sizeof(data));
        TEST_CHECK(vjo_hook_cache_put(data, sizeof(data), &k, &v) == 0 && !memcmp(before, data, sizeof(data)));
        v.trusted = 0;
        TEST_CHECK(vjo_hook_cache_put(data, sizeof(data), &k, &v) == 1);
        TEST_CHECK(vjo_hook_cache_get(data, sizeof(data), &k, &out) == 1 && out.site == site && !out.trusted);
    }
    v.trusted = 2;
    TEST_CHECK(vjo_hook_cache_put(data, sizeof(data), &k, &v) == -1);
    /* the record's site word as a file could hold it (the first record's, at 16 + 80, with the file's checksum):
     * 3 is a trusted site, 4 (no build writes it) makes the file invalid, as 2 did before the bit */
    TEST_CHECK(vjo_hook_cache_get(data, sizeof(data), &k, &out) == 1 && data[16 + 80] == 1); /* the loop's last */
    data[16 + 80] = 3;
    reseal(data);
    TEST_CHECK(vjo_hook_cache_get(data, sizeof(data), &k, &out) == 1 && out.site == 1 && out.trusted == 1);
    data[16 + 80] = 4;
    reseal(data);
    TEST_CHECK(!vjo_hook_cache_get(data, sizeof(data), &k, &out));
}

/* A key's title is the strict title ID (core/games.h vjo_title_id_strict) in its 12 bytes, zero-padded: anything
 * else is an invalid key, never stored, never found. */
static void key_titles(void)
{
    static const char *const bad[] = {"pcsg00146", "PCSG0014", "PCSG001466", "PCSG0014/", "PCSG\xC0" "0146", ""};
    uint8_t data[VJO_HOOK_CACHE_BYTES] = {0};
    VjoHookLearned v = learned(16), out;
    VjoHookCacheKey k = key(146);
    TEST_CHECK(vjo_hook_cache_put(data, 0, &k, &v) == 1 && vjo_hook_cache_get(data, sizeof(data), &k, &out) == 1);
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        VjoHookCacheKey b = key(146);
        memset(b.title, 0, sizeof(b.title));
        memcpy(b.title, bad[i], strlen(bad[i]) < sizeof(b.title) ? strlen(bad[i]) : sizeof(b.title));
        TEST_CHECK_(vjo_hook_cache_put(data, sizeof(data), &b, &v) == -1, "%s", bad[i]);
        TEST_CHECK_(vjo_hook_cache_get(data, sizeof(data), &b, &out) == 0, "%s", bad[i]);
    }
    for (unsigned at = 10; at < 12; at++) { /* the padding after the NUL */
        VjoHookCacheKey b = key(146);
        b.title[at] = 'X';
        TEST_CHECK_(vjo_hook_cache_put(data, sizeof(data), &b, &v) == -1, "padding byte %u", at);
    }
    TEST_CHECK(vjo_hook_cache_get(data, sizeof(data), &k, &out) == 1); /* the good one is still there */
}

TEST_LIST = {
    {"key_titles", key_titles},
    {"trusted_bit", trusted_bit},
    {"roundtrip_and_no_rewrite", roundtrip_and_no_rewrite},
    {"invalidation", invalidation},
    {"bounded_eviction", bounded_eviction},
    {"corrupt_and_truncated", corrupt_and_truncated},
    {NULL, NULL}
};
