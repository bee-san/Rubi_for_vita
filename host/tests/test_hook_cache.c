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
    VjoHookLearned v = {1, 0x12345, 2, interval};
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

TEST_LIST = {
    {"roundtrip_and_no_rewrite", roundtrip_and_no_rewrite},
    {"invalidation", invalidation},
    {"bounded_eviction", bounded_eviction},
    {"corrupt_and_truncated", corrupt_and_truncated},
    {NULL, NULL}
};
