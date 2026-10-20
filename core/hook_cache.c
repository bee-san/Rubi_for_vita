#include "hook_cache.h"
#include <string.h>

#define HEADER 16u
#define RECORD 96u

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (i * 8));
}

static uint32_t checksum(const uint8_t *data)
{
    uint32_t crc = ~0u;
    for (unsigned i = HEADER; i < VJO_HOOK_CACHE_BYTES; i++) {
        crc ^= data[i];
        for (unsigned b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static int interval_ok(uint32_t n) { return n == 8u || n == 16u || n == 32u; }

static int title_ok(const char *title)
{
    for (unsigned i = 0; i < 9; i++)
        if (!((title[i] >= 'A' && title[i] <= 'Z') || (title[i] >= '0' && title[i] <= '9'))) return 0;
    return !title[9] && !title[10] && !title[11];
}

static int digest_ok(const uint8_t *p)
{
    unsigned bits = 0;
    for (unsigned i = 0; i < 32; i++) bits |= p[i];
    return bits != 0;
}

static int key_ok(const VjoHookCacheKey *key)
{
    return key && title_ok(key->title) && digest_ok(key->build) && digest_ok(key->definition);
}

static int valid(const uint8_t *data, size_t n)
{
    if (!data || n != VJO_HOOK_CACHE_BYTES || memcmp(data, "VJHC", 4) || get32(data + 4) != 1u ||
        get32(data + 8) > VJO_HOOK_CACHE_ENTRIES || get32(data + 12) != checksum(data)) return 0;
    unsigned count = get32(data + 8);
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *r = data + HEADER + RECORD * i;
        if (!title_ok((const char *)r) || !digest_ok(r + 12) || !digest_ok(r + 44) ||
            get32(r + 80) > 1u || !interval_ok(get32(r + 92))) return 0;
        for (unsigned j = 0; j < i; j++)
            if (!memcmp(r, data + HEADER + RECORD * j, 12)) return 0;
    }
    /* Unused records must be empty, including after deletion/eviction. */
    for (unsigned i = HEADER + RECORD * count; i < VJO_HOOK_CACHE_BYTES; i++)
        if (data[i]) return 0;
    return 1;
}

static int same_key(const uint8_t *r, const VjoHookCacheKey *key)
{
    return !memcmp(r, key->title, 12) && !memcmp(r + 12, key->build, 32) &&
           !memcmp(r + 44, key->definition, 32) && get32(r + 76) == key->mode;
}

static int same_learning(const uint8_t *r, const VjoHookLearned *v)
{
    return get32(r + 80) == v->site && get32(r + 84) == v->key &&
           get32(r + 88) == v->encoding && get32(r + 92) == v->interval;
}

int vjo_hook_cache_get(const uint8_t *data, size_t n, const VjoHookCacheKey *key, VjoHookLearned *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!out || !key_ok(key) || !valid(data, n)) return 0;
    for (unsigned i = 0; i < get32(data + 8); i++) {
        const uint8_t *r = data + HEADER + RECORD * i;
        if (!same_key(r, key)) continue;
        out->site = get32(r + 80); out->key = get32(r + 84);
        out->encoding = get32(r + 88); out->interval = get32(r + 92);
        return 1;
    }
    return 0;
}

int vjo_hook_cache_put(uint8_t *data, size_t n, const VjoHookCacheKey *key, const VjoHookLearned *v)
{
    unsigned count, found;
    if (!data || !key_ok(key) || (v && (v->site > 1u || !interval_ok(v->interval)))) return -1;
    int was_valid = valid(data, n);
    if (!was_valid) memset(data, 0, VJO_HOOK_CACHE_BYTES);
    count = get32(data + 8);
    for (found = 0; found < count; found++) {
        uint8_t *r = data + HEADER + RECORD * found;
        if (memcmp(r, key->title, 12)) continue;
        if (v && same_key(r, key) && same_learning(r, v)) return 0;
        break;
    }
    if (!v && found == count) return 0;
    if (found < count) {
        memmove(data + HEADER + RECORD * found, data + HEADER + RECORD * (found + 1u),
                (count - found - 1u) * RECORD);
        count--;
    }
    if (v) {
        if (count == VJO_HOOK_CACHE_ENTRIES) count--;
        memmove(data + HEADER + RECORD, data + HEADER, count * RECORD);
        uint8_t *r = data + HEADER;
        memcpy(r, key->title, 12); memcpy(r + 12, key->build, 32); memcpy(r + 44, key->definition, 32);
        put32(r + 76, key->mode); put32(r + 80, v->site); put32(r + 84, v->key);
        put32(r + 88, v->encoding); put32(r + 92, v->interval);
        count++;
    }
    memset(data + HEADER + count * RECORD, 0, (VJO_HOOK_CACHE_ENTRIES - count) * RECORD);
    memcpy(data, "VJHC", 4); put32(data + 4, 1u); put32(data + 8, count); put32(data + 12, checksum(data));
    return 1;
}
