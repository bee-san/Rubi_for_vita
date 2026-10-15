/* The Games page's icons (settings/main.c): reading an icon file, the check
 * that keeps libvita2d's PNG buffer reader inside it, and the cache of
 * loaded textures by title ID. */
#ifndef VJO_SETTINGS_ICONS_H
#define VJO_SETTINGS_ICONS_H
#include <stddef.h>
#include "../core/games.h"

/* An icon file is at most 256 KiB and its image at most 256 x 256 pixels, so
 * its texture is at most 256 KiB (a 128 x 128 icon's is 64 KiB), in main
 * memory: the app is a system application without CDRAM (settings/sysapp.c).
 * A larger or broken file shows the placeholder. */
#define VJO_ICON_FILE_MAX (256 * 1024)
#define VJO_ICON_SIDE_MAX 256
/* Bytes after the file that vjo_icon_read clears; its buffer has
 * VJO_ICON_FILE_MAX + VJO_ICON_FILE_PAD bytes. */
#define VJO_ICON_FILE_PAD 16

enum {
    VJO_ICON_OK = 0,
    VJO_ICON_ABSENT = -1,     /* no file, or it cannot be read */
    VJO_ICON_TOO_BIG = -2,    /* more than VJO_ICON_FILE_MAX bytes */
    VJO_ICON_NOT_PNG = -3,    /* no PNG signature */
    VJO_ICON_BAD_HEADER = -4, /* the first chunk is not a 13-byte IHDR */
    VJO_ICON_BAD_SIZE = -5,   /* 0 or more than VJO_ICON_SIDE_MAX pixels a side */
    VJO_ICON_TRUNCATED = -6,  /* a chunk runs past the file's end, or no IEND */
    VJO_ICON_BAD_CRC = -7,    /* a critical chunk's CRC does not match */
    VJO_ICON_NO_IMAGE = -8,   /* IEND before any IDAT */
    VJO_ICON_BAD_CHUNK = -9,  /* a chunk type libpng refuses (not four letters, third lower case) */
    VJO_ICON_BAD_ORDER = -10  /* IDATs not one run, or a palette image's IDAT before its PLTE */
};

/* Reads path into buf and clears the VJO_ICON_FILE_PAD bytes after it: the
 * file's size, or VJO_ICON_ABSENT / VJO_ICON_TOO_BIG. */
long vjo_icon_read(const char *path, unsigned char *buf);

/* VJO_ICON_OK when png[0, size) is a PNG whose chunks, from IHDR (first)
 * through one run of IDATs to IEND, all lie inside it, have names libpng
 * accepts, and, for critical chunks, valid CRCs, and whose image is at most
 * VJO_ICON_SIDE_MAX pixels a side (written to *width and *height when not
 * NULL). vita2d_load_PNG_buffer has no length: libpng reads each chunk where
 * its header says, so only a buffer that passed this check may be given to
 * it. The CRC and order rules also keep a damaged file from reaching libpng's
 * image decoding, where a failure costs a texture: libvita2d's loader does not
 * free it. An ancillary chunk's CRC is not checked: libpng only warns on it.
 * Image data whose CRCs hold but whose deflate stream is broken still fails
 * there. */
int vjo_icon_png_check(const unsigned char *png, size_t size, unsigned *width, unsigned *height);

/* Loaded icon textures by title ID; the least recently used goes first. */
#define VJO_ICON_SLOTS 9
typedef struct {
    char id[VJO_TITLE_ID_SIZE];
    void *texture; /* NULL: a free slot */
    unsigned used; /* the cache's clock at the last use */
} VjoIconSlot;
typedef struct {
    VjoIconSlot slot[VJO_ICON_SLOTS];
    unsigned clock;
} VjoIconCache;

/* id's texture, marked as used now, or NULL. */
void *vjo_icon_cache_get(VjoIconCache *cache, const char *id);
int vjo_icon_cache_count(const VjoIconCache *cache);
/* Empties the least recently used slot whose ID is none of keep[0, n) and
 * returns its texture, for the caller to free once the GPU is done with it;
 * NULL when every filled slot is kept. */
void *vjo_icon_cache_evict(VjoIconCache *cache, const char *const *keep, int n);
/* Puts id's texture in a free slot (id not cached): 0, or -1 when full. */
int vjo_icon_cache_put(VjoIconCache *cache, const char *id, void *texture);
/* Empties every slot, handing each texture to release. */
void vjo_icon_cache_clear(VjoIconCache *cache, void (*release)(void *texture));
#endif
