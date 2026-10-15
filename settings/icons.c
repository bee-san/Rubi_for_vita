#include "icons.h"
#include <psp2/io/fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

long vjo_icon_read(const char *path, unsigned char *buf)
{
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return VJO_ICON_ABSENT;
    long used = 0;
    for (;;) {
        /* One byte more than the limit tells a file that is too big. */
        int n = sceIoRead(fd, buf + used, (size_t)(VJO_ICON_FILE_MAX + 1 - used));
        if (n < 0) {
            sceIoClose(fd);
            return VJO_ICON_ABSENT;
        }
        if (!n) break;
        used += n;
        if (used > VJO_ICON_FILE_MAX) {
            sceIoClose(fd);
            return VJO_ICON_TOO_BIG;
        }
    }
    sceIoClose(fd);
    memset(buf + used, 0, VJO_ICON_FILE_PAD);
    return used;
}

static uint32_t be32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* PNG's CRC-32 (ISO 3309, as zlib's crc32). */
static uint32_t chunk_crc(const unsigned char *p, size_t n)
{
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

int vjo_icon_png_check(const unsigned char *png, size_t size, unsigned *width, unsigned *height)
{
    static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (size < 8 || memcmp(png, signature, 8)) return VJO_ICON_NOT_PNG;
    size_t at = 8;
    int first = 1, image = 0;
    for (;;) {
        /* length, type, data, CRC: the data's length is checked against what is left */
        if (size - at < 12) return VJO_ICON_TRUNCATED;
        uint32_t length = be32(png + at);
        if (length > size - at - 12) return VJO_ICON_TRUNCATED;
        const unsigned char *type = png + at + 4;
        if (chunk_crc(type, 4 + (size_t)length) != be32(type + 4 + length)) return VJO_ICON_BAD_CRC;
        if (first) {
            if (memcmp(type, "IHDR", 4) || length != 13) return VJO_ICON_BAD_HEADER;
            uint32_t w = be32(type + 4), h = be32(type + 8);
            if (!w || !h || w > VJO_ICON_SIDE_MAX || h > VJO_ICON_SIDE_MAX) return VJO_ICON_BAD_SIZE;
            if (width) *width = w;
            if (height) *height = h;
            first = 0;
        } else if (!memcmp(type, "IDAT", 4)) {
            image = 1;
        } else if (!memcmp(type, "IEND", 4)) {
            return image ? VJO_ICON_OK : VJO_ICON_NO_IMAGE;
        }
        at += 12 + (size_t)length;
    }
}

void *vjo_icon_cache_get(VjoIconCache *cache, const char *id)
{
    for (int i = 0; i < VJO_ICON_SLOTS; i++) {
        VjoIconSlot *s = &cache->slot[i];
        if (s->texture && !strcmp(s->id, id)) {
            s->used = ++cache->clock;
            return s->texture;
        }
    }
    return NULL;
}

int vjo_icon_cache_count(const VjoIconCache *cache)
{
    int n = 0;
    for (int i = 0; i < VJO_ICON_SLOTS; i++) n += cache->slot[i].texture != NULL;
    return n;
}

void *vjo_icon_cache_evict(VjoIconCache *cache, const char *const *keep, int n)
{
    VjoIconSlot *victim = NULL;
    for (int i = 0; i < VJO_ICON_SLOTS; i++) {
        VjoIconSlot *s = &cache->slot[i];
        int kept = 0;
        if (!s->texture) continue;
        for (int k = 0; k < n && !kept; k++) kept = !strcmp(s->id, keep[k]);
        if (!kept && (!victim || s->used < victim->used)) victim = s;
    }
    if (!victim) return NULL;
    void *texture = victim->texture;
    memset(victim, 0, sizeof(*victim));
    return texture;
}

int vjo_icon_cache_put(VjoIconCache *cache, const char *id, void *texture)
{
    for (int i = 0; i < VJO_ICON_SLOTS; i++) {
        VjoIconSlot *s = &cache->slot[i];
        if (s->texture) continue;
        snprintf(s->id, sizeof(s->id), "%s", id);
        s->texture = texture;
        s->used = ++cache->clock;
        return 0;
    }
    return -1;
}

void vjo_icon_cache_clear(VjoIconCache *cache, void (*release)(void *texture))
{
    for (int i = 0; i < VJO_ICON_SLOTS; i++) {
        VjoIconSlot *s = &cache->slot[i];
        if (s->texture) release(s->texture);
        memset(s, 0, sizeof(*s));
    }
}
