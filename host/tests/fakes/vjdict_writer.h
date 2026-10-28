/* Small dictionary files for the host suites: the bytes core/local_dict.c reads, written as
 * tools/convert_dictionary.py writes them, so a suite can look words up without the bundled dictionary.
 *
 * vjdict2_one() writes a VJDICT2 file of one key with one entry into bytes (cap at least 8192 + the
 * entry's): the 64-byte header (its own name, when title is not NULL, at page 0's bytes 192..255), one
 * hash page with one slot, the key, its posting and the payload (spelling, reading, meaning, each with
 * its NUL). It returns the file's size. The little-endian writers and the key's hash (FNV-1a, then
 * murmur3's finaliser) are the converter's: a test's copy, so that a reader that hashed otherwise would
 * not find the key. */
#ifndef VJO_TEST_VJDICT_WRITER_H
#define VJO_TEST_VJDICT_WRITER_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline void vjdict_put16(uint8_t *p, uint16_t x) { p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8); }
static inline void vjdict_put32(uint8_t *p, uint32_t x) { vjdict_put16(p, (uint16_t)x); vjdict_put16(p + 2, (uint16_t)(x >> 16)); }
static inline void vjdict_put64(uint8_t *p, uint64_t x) { vjdict_put32(p, (uint32_t)x); vjdict_put32(p + 4, (uint32_t)(x >> 32)); }
static inline uint64_t vjdict_key_hash(const char *s)
{
    uint64_t h = UINT64_C(14695981039346656037);
    for (; *s; s++) h = (h ^ (uint8_t)*s) * UINT64_C(1099511628211);
    h ^= h >> 33; h *= UINT64_C(0xff51afd7ed558ccd);
    h ^= h >> 33; h *= UINT64_C(0xc4ceb9fe1a85ec53);
    return h ^ (h >> 33);
}

/* One key with one entry (VJDICT2: header, one hash slot, the key, its posting, the payload); 0 when it does not fit. */
static inline size_t vjdict2_one(uint8_t *bytes, size_t cap, const char *key, const char *spelling,
                                 const char *reading, const char *meaning, const char *title)
{
    size_t off = 8192, key_len = strlen(key), posting, payload, need = 0;
    const char *parts[] = {spelling, reading, meaning};
    for (int i = 0; i < 3; i++) need += strlen(parts[i]) + 1;
    if (cap < off + key_len + 16 + need || (title && strlen(title) > 63)) return 0;
    memset(bytes, 0, cap);
    memcpy(bytes, "VJDICT2\0", 8);
    vjdict_put32(bytes + 8, 2); vjdict_put32(bytes + 12, 64);
    vjdict_put32(bytes + 16, 1); vjdict_put32(bytes + 20, 24);
    vjdict_put64(bytes + 24, 4096); vjdict_put64(bytes + 32, off);
    vjdict_put64(bytes + 48, 1); vjdict_put64(bytes + 56, 1);
    if (title) memcpy(bytes + 192, title, strlen(title));
    memcpy(bytes + 4096, "VJHP", 4);
    vjdict_put64(bytes + 4112, vjdict_key_hash(key)); vjdict_put64(bytes + 4120, off);
    vjdict_put32(bytes + 4128, 1); vjdict_put16(bytes + 4132, (uint16_t)key_len);
    memcpy(bytes + off, key, key_len); off += key_len;
    posting = off; off += 16;
    payload = off;
    for (int i = 0; i < 3; i++) {
        size_t n = strlen(parts[i]) + 1;
        memcpy(bytes + off, parts[i], n); off += n;
    }
    vjdict_put64(bytes + posting, payload); vjdict_put32(bytes + posting + 8, (uint32_t)(off - payload));
    vjdict_put32(bytes + 4100, 1);
    vjdict_put64(bytes + 40, off);
    return off;
}
#endif
