/* Anki for Vita's duplicate index, read (core/anki_index.h): a word's keys and their lookup, on
 * Enter (shell/anki.c do_save). SceShell code: it allocates nothing and writes nothing; a lookup is
 * one open (two when there is no rubi.idx), three reads and a close, with one 512-byte buffer on the
 * stack. Built for size (vita/CMakeLists.txt): it runs once per Enter. */
#include "anki_index.h"

#include "bearssl.h"

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "the index's words are read as native integers: a little-endian CPU (the Vita's, the hosts')"
#endif

#define MAGIC0 0x49564641u /* "AFVI" */
#define MAGIC1 0x5845444Eu /* "NDEX" */
#define CHECK_AT 124u      /* the header's check */

static void start(br_sha256_context *c, uint8_t kind)
{
    br_sha256_init(c);
    br_sha256_update(c, &kind, 1);
}

/* The digest's first 8 bytes, big-endian. */
static uint64_t digest(br_sha256_context *c)
{
    union {
        uint64_t q[4];
        uint8_t b[32];
    } d;
    br_sha256_out(c, d.b);
    return __builtin_bswap64(d.q[0]);
}

/* Feeds s to the hash normalised (core/anki_index.h): returns the bytes fed, or -1 at an ASCII '['. */
static int feed(br_sha256_context *c, const char *s, int kana)
{
    const uint8_t *p = (const uint8_t *)s;
    int n = 0, space = 0;
    uint8_t u[3];
    for (uint32_t cp; (cp = *p) != 0;) {
        size_t len = 1;
        if (cp == '[')
            return -1;
        /* U+00A0 is read as U+3000, the other whitespace character: a byte 0xA0 alone (the second of
         * "à", C3 A0) is not one. A 3-byte sequence is read whole; any other byte is copied alone (no
         * lead byte is ever another sequence's continuation byte). */
        if (cp == 0xC2 && p[1] == 0xA0) {
            cp = 0x3000;
            len = 2;
        } else if ((cp & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            cp = (cp & 0x0F) << 12 | (uint32_t)(p[1] & 0x3F) << 6 | (p[2] & 0x3Fu);
            len = 3;
        }
        p += len;
        if (cp == ' ' || cp - 9 < 5 || cp == 0x3000) {
            space = n > 0;
            continue;
        }
        if (space) {
            br_sha256_update(c, " ", 1);
            n++;
            space = 0;
        }
        if (cp - 0xFF01 <= 0xFF5E - 0xFF01) {
            cp -= 0xFEE0;
            len = 1;
        } else if (kana && (cp - 0x30A1 <= 0x30F6 - 0x30A1 || cp - 0x30FD <= 1)) {
            cp -= 0x60;
        }
        if (len == 3) {
            u[0] = (uint8_t)(0xE0 | cp >> 12);
            u[1] = (uint8_t)(0x80 | (cp >> 6 & 0x3F));
            u[2] = (uint8_t)(0x80 | (cp & 0x3F));
        } else {
            u[0] = (uint8_t)cp;
        }
        br_sha256_update(c, u, len);
        n += (int)len;
    }
    return n;
}

int vjo_anki_index_keys(const char *form, const char *reading, VjoAnkiIndexKeys *k)
{
    br_sha256_context c;
    k->has_pair = 0;
    start(&c, 1);
    if (!form || feed(&c, form, 0) <= 0)
        return VJO_ANKI_INDEX_NOKEY;
    k->form = digest(&c);
    if (reading) {
        start(&c, 2);
        feed(&c, form, 0);
        br_sha256_update(&c, "\x1f", 1);
        if (feed(&c, reading, 1) > 0) {
            k->pair = (k->form & 0xFFFFFFFF00000000ull) | (digest(&c) >> 32);
            k->has_pair = 1;
        }
    }
    return 0;
}

/* A key's bucket: its top `bits` bits (bits 0..16; none, bucket 0, for 0). */
static uint32_t bucket_of(uint64_t key, uint32_t bits)
{
    return ((uint32_t)(key >> 32) >> 1) >> (31 - bits);
}

int vjo_anki_index_lookup(const VjoPlatform *p, const char *path, const char *old_path, const VjoAnkiIndexKeys *k)
{
    union { /* the header, then the bucket */
        uint64_t q[VJO_ANKI_INDEX_MAX_BUCKET];
        uint32_t w[2 * VJO_ANKI_INDEX_MAX_BUCKET];
    } u;
    uint32_t dir[2], bits, n, b, keys_at, found = 0;
    br_sha256_context c;
    VjoFile f;
    int rc = VJO_ANKI_INDEX_BAD;
    if (!p->file_open || (p->file_open(p->ud, path, &f) != 0 && p->file_open(p->ud, old_path, &f) != 0))
        return VJO_ANKI_INDEX_MISSING;
    if (f.size < VJO_ANKI_INDEX_HEADER || f.read(f.ctx, 0, u.q, VJO_ANKI_INDEX_HEADER) != 0)
        goto out;
    start(&c, 0);
    br_sha256_update(&c, u.q, CHECK_AT);
    n = u.w[4];     /* N */
    bits = u.w[5];  /* B */
    if (u.w[0] != MAGIC0 || u.w[1] != MAGIC1 || u.w[2] != 1 || u.w[3] != VJO_ANKI_INDEX_HEADER ||
        bits > VJO_ANKI_INDEX_MAX_BITS || n > VJO_ANKI_INDEX_MAX_KEYS || u.w[6] > VJO_ANKI_INDEX_MAX_BUCKET ||
        (uint32_t)(digest(&c) >> 32) != u.w[CHECK_AT / 4])
        goto out;
    keys_at = VJO_ANKI_INDEX_HEADER + 4 * ((1u << bits) + 1);
    if (f.size != keys_at + 8 * (uint64_t)n)
        goto out;
    b = bucket_of(k->form, bits);
    if (f.read(f.ctx, VJO_ANKI_INDEX_HEADER + 4 * b, dir, sizeof(dir)) != 0 || dir[0] > dir[1] || dir[1] > n ||
        (n = dir[1] - dir[0]) > u.w[6])
        goto out;
    if (n && f.read(f.ctx, keys_at + 8 * (uint64_t)dir[0], u.q, 8 * n) != 0)
        goto out;
    /* The whole bucket is checked (every key in it, ascending) before it answers. */
    for (uint32_t i = 0; i < n; i++) {
        uint64_t key = u.q[i];
        if (bucket_of(key, bits) != b || (i && key <= u.q[i - 1]))
            goto out;
        if (k->has_pair && key == k->pair)
            found |= 2;
        else if (key == k->form)
            found |= 1;
    }
    rc = found & 2 ? VJO_ANKI_INDEX_PAIR : (int)found;
out:
    f.close(f.ctx);
    return rc;
}
