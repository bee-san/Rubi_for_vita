/* The PARAM.SFO of the PSP game an Adrenaline bubble boots, read from its
 * target: an EBOOT.PBP (its first section), or a UMD image, plain (ISO 9660,
 * /PSP_GAME/PARAM.SFO) or block-compressed: CSO v1 (deflate), CSO v2 (deflate
 * or LZ4) and ZSO (LZ4), as maxcso's README_CSO.md and README_ZSO.md define
 * them. DAX and other formats are refused. Every read is bounded: the PBP
 * header, the ISO's volume descriptor, two directories of at most
 * VJO_PSP_DIR_MAX bytes and the SFO, through at most one compressed block at a
 * time. For the Rubi app (never SceShell) and the host tests. */
#ifndef VJO_PSPTARGET_H
#define VJO_PSPTARGET_H
#include <stddef.h>
#include <stdint.h>

#define VJO_PSP_SECTOR 2048u
#define VJO_PSP_SFO_MAX (64u * 1024u)
#define VJO_PSP_DIR_MAX (64u * 1024u)     /* a directory's bytes looked at, 32 sectors */
#define VJO_PSP_BLOCK_MAX (32u * 1024u)   /* the largest compressed-image block accepted */

typedef struct {
    void *ctx;
    uint64_t size; /* the file's size */
    /* Exactly n bytes at off into buf: 0, or < 0 (error, short read). */
    int (*read)(void *ctx, uint64_t off, void *buf, size_t n);
    /* Raw deflate (no zlib header) of in[0, n) into out[0, cap): the bytes
     * written, or < 0. NULL: deflate blocks fail (VJO_PSP_E_CODEC). */
    long (*inflate)(void *ctx, const void *in, size_t n, void *out, size_t cap);
} VjoPspFile;

/* The work memory one call needs (the app keeps one, static). */
typedef struct {
    unsigned char packed[2 * VJO_PSP_BLOCK_MAX]; /* a compressed block as stored */
    unsigned char block[VJO_PSP_BLOCK_MAX];      /* the decompressed block */
    unsigned char sector[VJO_PSP_SECTOR];
} VjoPspWork;

enum { VJO_PSP_PBP = 1, VJO_PSP_ISO, VJO_PSP_CSO, VJO_PSP_ZSO };
enum {
    VJO_PSP_E_READ = -1,     /* a read failed or the file is too short */
    VJO_PSP_E_FORMAT = -2,   /* not a format this reads (DAX, a damaged header) */
    VJO_PSP_E_IMAGE = -3,    /* a damaged image: index, block, directory */
    VJO_PSP_E_NO_SFO = -4,   /* no /PSP_GAME/PARAM.SFO, or one too large */
    VJO_PSP_E_CODEC = -5     /* a block that does not decompress */
};
/* The format's name for the logs ("PBP", "ISO", "CSO", "ZSO"; "?" otherwise). */
const char *vjo_psp_format_name(int format);

/* Reads the target's PARAM.SFO into sfo[0, cap): the format (> 0) with its
 * length in *len, or VJO_PSP_E_*. */
int vjo_psp_param_sfo(const VjoPspFile *f, VjoPspWork *w, unsigned char *sfo, size_t cap, size_t *len);

/* An LZ4 block (the raw block format, no frame) of in[0, n) into out[0, cap):
 * the bytes written once cap is full or the input ends, or -1 for a damaged
 * block. Bytes after the block's last literals are ignored (a ZSO block may
 * be padded to its index alignment). */
long vjo_lz4_block(const unsigned char *in, size_t n, unsigned char *out, size_t cap);
#endif
