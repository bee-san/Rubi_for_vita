#include "psptarget.h"
#include <string.h>

static uint32_t le16(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le64(const unsigned char *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

const char *vjo_psp_format_name(int format)
{
    switch (format) {
    case VJO_PSP_PBP: return "PBP";
    case VJO_PSP_ISO: return "ISO";
    case VJO_PSP_CSO: return "CSO";
    case VJO_PSP_ZSO: return "ZSO";
    default: return "?";
    }
}

static int file_read(const VjoPspFile *f, uint64_t off, void *buf, size_t n)
{
    if (off > f->size || n > f->size - off) return VJO_PSP_E_READ;
    return f->read(f->ctx, off, buf, n) < 0 ? VJO_PSP_E_READ : 0;
}

long vjo_lz4_block(const unsigned char *in, size_t n, unsigned char *out, size_t cap)
{
    size_t ip = 0, op = 0;
    while (ip < n) {
        unsigned token = in[ip++];
        size_t lit = token >> 4;
        if (lit == 15) {
            unsigned b;
            do {
                if (ip >= n) return -1;
                b = in[ip++];
                lit += b;
            } while (b == 255);
        }
        if (lit > n - ip || lit > cap - op) return -1;
        memcpy(out + op, in + ip, lit);
        ip += lit;
        op += lit;
        if (op == cap || ip >= n) break; /* full, or the last literals */
        if (n - ip < 2) return -1;
        size_t back = in[ip] | (size_t)in[ip + 1] << 8, match = token & 15;
        ip += 2;
        if (!back || back > op) return -1;
        if (match == 15) {
            unsigned b;
            do {
                if (ip >= n) return -1;
                b = in[ip++];
                match += b;
            } while (b == 255);
        }
        match += 4;
        if (match > cap - op) return -1;
        for (size_t i = 0; i < match; i++, op++) out[op] = out[op - back];
    }
    return (long)op;
}

/* The image's bytes: the file itself (ISO) or its blocks (CSO, ZSO). */
typedef struct {
    const VjoPspFile *f;
    VjoPspWork *w;
    int format, version;
    uint64_t total;       /* the uncompressed image's size */
    uint32_t block_size, blocks, shift;
    int64_t cached;       /* the block in w->block, or -1 */
} Image;

static int load_block(Image *im, uint32_t b)
{
    unsigned char idx[8];
    if (im->cached == (int64_t)b) return 0;
    im->cached = -1;
    if (file_read(im->f, 0x18 + (uint64_t)b * 4, idx, 8)) return VJO_PSP_E_READ;
    uint32_t a = le32(idx), z = le32(idx + 4);
    uint64_t start = (uint64_t)(a & 0x7FFFFFFFu) << im->shift, end = (uint64_t)(z & 0x7FFFFFFFu) << im->shift;
    uint64_t left = im->total - (uint64_t)b * im->block_size;
    size_t want = left < im->block_size ? (size_t)left : im->block_size;
    if (end < start || end > im->f->size) return VJO_PSP_E_IMAGE;
    uint64_t stored_size = end - start;
    int stored, lz4;
    if (im->format == VJO_PSP_ZSO) stored = a >> 31, lz4 = 1;
    else if (im->version >= 2) stored = stored_size >= im->block_size, lz4 = a >> 31;
    else stored = a >> 31, lz4 = 0;
    if (stored) {
        /* A stored block holds the block's bytes (and maybe its padding). */
        if (stored_size < want) return VJO_PSP_E_IMAGE;
        if (file_read(im->f, start, im->w->block, want)) return VJO_PSP_E_READ;
    } else {
        size_t n = stored_size > sizeof(im->w->packed) ? sizeof(im->w->packed) : (size_t)stored_size;
        long got;
        if (!n) return VJO_PSP_E_IMAGE;
        if (file_read(im->f, start, im->w->packed, n)) return VJO_PSP_E_READ;
        if (lz4) got = vjo_lz4_block(im->w->packed, n, im->w->block, want);
        else if (!im->f->inflate) return VJO_PSP_E_CODEC;
        else got = im->f->inflate(im->f->ctx, im->w->packed, n, im->w->block, want);
        if (got != (long)want) return VJO_PSP_E_CODEC;
    }
    im->cached = b;
    return 0;
}

static int image_read(Image *im, uint64_t off, void *buf, size_t n)
{
    unsigned char *out = buf;
    if (im->format == VJO_PSP_ISO) return file_read(im->f, off, buf, n);
    if (off > im->total || n > im->total - off) return VJO_PSP_E_IMAGE;
    while (n) {
        uint32_t b = (uint32_t)(off / im->block_size), in = (uint32_t)(off % im->block_size);
        int rc = load_block(im, b);
        if (rc) return rc;
        size_t take = im->block_size - in;
        if (take > n) take = n;
        memcpy(out, im->w->block + in, take);
        out += take;
        off += take;
        n -= take;
    }
    return 0;
}

/* name (len bytes, ISO 9660 d-characters, maybe ";1") is want, ignoring case. */
static int same_name(const unsigned char *name, size_t len, const char *want)
{
    size_t n = 0;
    while (n < len && name[n] != ';') n++;
    if (n && name[n - 1] == '.') n--; /* "PSP_GAME." */
    if (n != strlen(want)) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = name[i];
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        if (c != (unsigned char)want[i]) return 0;
    }
    return 1;
}

/* Finds want (a directory when dir) in the directory at sector lba of size
 * bytes: its sector and size. */
static int find_entry(Image *im, uint32_t lba, uint32_t size, const char *want, int dir, uint32_t *at, uint32_t *bytes)
{
    unsigned char *s = im->w->sector;
    if (size > VJO_PSP_DIR_MAX) size = VJO_PSP_DIR_MAX;
    for (uint32_t done = 0; done < size; done += VJO_PSP_SECTOR, lba++) {
        if (image_read(im, (uint64_t)lba * VJO_PSP_SECTOR, s, VJO_PSP_SECTOR)) return VJO_PSP_E_IMAGE;
        for (uint32_t pos = 0; pos + 34 <= VJO_PSP_SECTOR && pos < size - done;) {
            uint32_t len = s[pos];
            if (!len) break; /* the rest of the sector is padding */
            if (len < 34 || pos + len > VJO_PSP_SECTOR || 33u + s[pos + 32] > len) return VJO_PSP_E_IMAGE;
            if (!!(s[pos + 25] & 2) == !!dir && same_name(s + pos + 33, s[pos + 32], want)) {
                *at = le32(s + pos + 2);
                *bytes = le32(s + pos + 10);
                return 0;
            }
            pos += len;
        }
    }
    return VJO_PSP_E_NO_SFO;
}

static int iso_sfo(Image *im, unsigned char *sfo, size_t cap, size_t *len)
{
    unsigned char *pvd = im->w->sector;
    uint32_t lba, size;
    int rc = image_read(im, 16u * VJO_PSP_SECTOR, pvd, VJO_PSP_SECTOR);
    if (rc) return rc == VJO_PSP_E_READ ? VJO_PSP_E_FORMAT : rc;
    if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) || le16(pvd + 128) != VJO_PSP_SECTOR) return VJO_PSP_E_FORMAT;
    lba = le32(pvd + 156 + 2);
    size = le32(pvd + 156 + 10);
    if ((rc = find_entry(im, lba, size, "PSP_GAME", 1, &lba, &size))) return rc;
    if ((rc = find_entry(im, lba, size, "PARAM.SFO", 0, &lba, &size))) return rc;
    if (!size || size > cap) return VJO_PSP_E_NO_SFO;
    if ((rc = image_read(im, (uint64_t)lba * VJO_PSP_SECTOR, sfo, size))) return rc;
    *len = size;
    return 0;
}

int vjo_psp_param_sfo(const VjoPspFile *f, VjoPspWork *w, unsigned char *sfo, size_t cap, size_t *len)
{
    unsigned char h[0x28];
    Image im = {f, w, 0, 0, 0, 0, 0, 0, -1};
    int rc;
    if (!f || !f->read || !w || !sfo || !len) return VJO_PSP_E_READ;
    *len = 0;
    if (file_read(f, 0, h, f->size < sizeof(h) ? (size_t)f->size : sizeof(h)) || f->size < 0x18) return VJO_PSP_E_READ;
    if (!memcmp(h, "\0PBP", 4)) {
        uint32_t at, end;
        if (f->size < sizeof(h)) return VJO_PSP_E_READ;
        at = le32(h + 8);
        end = le32(h + 12);
        if (at < sizeof(h) || end <= at || end - at > cap || end > f->size) return VJO_PSP_E_NO_SFO;
        if (file_read(f, at, sfo, end - at)) return VJO_PSP_E_READ;
        *len = end - at;
        return VJO_PSP_PBP;
    }
    if (!memcmp(h, "CISO", 4) || !memcmp(h, "ZISO", 4)) {
        im.format = h[0] == 'C' ? VJO_PSP_CSO : VJO_PSP_ZSO;
        im.total = le64(h + 8);
        im.block_size = le32(h + 16);
        im.version = h[20];
        im.shift = h[21];
        if (im.block_size < VJO_PSP_SECTOR || im.block_size > VJO_PSP_BLOCK_MAX ||
            im.block_size % VJO_PSP_SECTOR || im.shift > 16 || !im.total || (im.format == VJO_PSP_CSO && im.version > 2))
            return VJO_PSP_E_FORMAT;
        /* ceil(total / block) without total + block - 1 wrapping round */
        uint64_t blocks = im.total / im.block_size + (im.total % im.block_size != 0);
        if (blocks >= 0x40000000u || 0x18 + (blocks + 1) * 4 > f->size) return VJO_PSP_E_FORMAT;
        im.blocks = (uint32_t)blocks;
    } else {
        im.format = VJO_PSP_ISO;
        im.total = f->size;
    }
    rc = iso_sfo(&im, sfo, cap, len);
    return rc ? rc : im.format;
}
