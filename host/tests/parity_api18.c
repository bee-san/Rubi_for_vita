/* The API 18 pipeline as it ran before #74, for the capture parity test:
 * - the kernel's crop and row conversion, verbatim from 8228c3c
 *   kernel/capture.c (capture_compute_crop, convert_row, read_row): the
 *   region copied as A8B8G8R8 rows into the kernel's capture buffer;
 * - the game worker reading those rows (vjoReadRaw) into its own region-sized
 *   grey plane and recognizing it: core/vocr_ocr.c of 8228c3c, kept verbatim in
 *   host/tests/api18/ (sha256 checked by tools/tests/test_capture_parity.py).
 * Only the runtime's inference (vocr_load/arena/init/run) is replaced, by a
 * recorder of what vocr_run is given; the line finder is the real one. */
#define vjo_vocr_job_bytes api18_vjo_vocr_job_bytes
#define vjo_vocr_model api18_vjo_vocr_model
#define vjo_vocr_models api18_vjo_vocr_models
#define vjo_vocr_ocr api18_vjo_vocr_ocr
#define vjo_vocr_ocr_resident api18_vjo_vocr_ocr_resident
#define vjo_vocr_resident_drop api18_vjo_vocr_resident_drop
#define vjo_vocr_workspace_bytes api18_vjo_vocr_workspace_bytes
#define vocr_load api18_spy_load
#define vocr_arena_bytes api18_spy_arena_bytes
#define vocr_init api18_spy_init
#define vocr_run api18_spy_run
#include "api18/vocr_ocr.c"

#include <stdlib.h>
#include "parity.h"

/* ---- 8228c3c kernel/capture.c, verbatim apart from the region argument ---- */
#define FMT_A8B8G8R8    0x00000000u
#define FMT_A2B10G10R10 0x60800000u
#define FMT_BGRA5551    0x50000000u /* 16 bpp, R in the low bits (udcd-uvc) */
#define VJO_MAX_W 960u
#define VJO_MAX_H 544u

static uint32_t fmt_bpp(uint32_t fmt)
{
    return fmt == FMT_BGRA5551 ? 2 : 4;
}

static void capture_compute_crop(uint16_t rx, uint16_t ry, uint16_t rw, uint16_t rh, uint32_t fb_w, uint32_t fb_h,
                                 uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h)
{
    struct { uint16_t x, y, w, h; } r = {rx, ry, rw, rh};
    uint32_t cx, cy, cw, ch;
    if (fb_w > VJO_MAX_W)
        fb_w = VJO_MAX_W;
    if (fb_h > VJO_MAX_H)
        fb_h = VJO_MAX_H;
    if (r.w == 0 || r.h == 0) {
        *x = 0;
        *y = 0;
        *w = fb_w;
        *h = fb_h;
        return;
    }
    cx = (uint32_t)r.x * fb_w >> 16;
    cy = (uint32_t)r.y * fb_h >> 16;
    cw = ((uint32_t)r.w * fb_w + 65535) >> 16;
    ch = ((uint32_t)r.h * fb_h + 65535) >> 16;
    /* Lens needs some context around tiny selections. */
    if (cw < 32)
        cw = 32;
    if (ch < 32)
        ch = 32;
    if (cx + cw > fb_w)
        cx = cw > fb_w ? 0 : fb_w - cw;
    if (cy + ch > fb_h)
        cy = ch > fb_h ? 0 : fb_h - ch;
    if (cw > fb_w)
        cw = fb_w;
    if (ch > fb_h)
        ch = fb_h;
    cx &= ~1u;
    *x = cx;
    *y = cy;
    *w = cw;
    *h = ch;
}

/* Converts one row of n pixels (source format) to A8B8G8R8 at dst. */
static void convert_row(uint8_t *dst, const uint8_t *src, uint32_t n, uint32_t fmt)
{
    uint32_t *d = (uint32_t *)dst;
    if (fmt == FMT_A2B10G10R10) {
        const uint32_t *s = (const uint32_t *)src;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t v = s[i];
            uint32_t r = (v >> 2) & 0xFF, gg = (v >> 12) & 0xFF, b = (v >> 22) & 0xFF;
            d[i] = 0xFF000000u | (b << 16) | (gg << 8) | r;
        }
    } else if (fmt == FMT_BGRA5551) {
        const uint16_t *s = (const uint16_t *)src;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t v = s[i];
            uint32_t r = (v & 0x1F) << 3, gg = ((v >> 5) & 0x1F) << 3, b = ((v >> 10) & 0x1F) << 3;
            d[i] = 0xFF000000u | (b << 16) | (gg << 8) | r;
        }
    }
}

static int read_row(uint8_t *dst, uint8_t *tmp, uintptr_t src, uint32_t n, uint32_t fmt)
{
    if (fmt == FMT_A8B8G8R8) {
        memcpy(dst, (const void *)src, n * 4); /* copy_from_game */
        return 0;
    }
    memcpy(tmp, (const void *)src, n * fmt_bpp(fmt));
    convert_row(dst, tmp, n, fmt);
    return 0;
}
/* ---- end of 8228c3c kernel/capture.c ---- */

typedef struct {
    const ParityFrame *f;
    uint8_t *raw;           /* the kernel's capture buffer: crop_w x crop_h A8B8G8R8 */
    uint32_t w, h;
} Api18Capture;

/* vjoReadRaw: rows of the kernel's buffer, raw_stride = w * 4. */
static int api18_rows(void *ud, unsigned y, unsigned n, unsigned char *dst)
{
    const Api18Capture *c = ud;
    if (y >= c->h) return -1;
    if (n > c->h - y) n = c->h - y;
    memcpy(dst, c->raw + (size_t)y * c->w * 4, (size_t)n * c->w * 4);
    return (int)n;
}

static ParityRun *current;
int api18_spy_load(vocr_model *m, const void *blob, size_t bytes)
{
    (void)blob; (void)bytes;
    memset(m, 0, sizeof(*m));
    return VOCR_OK;
}
size_t api18_spy_arena_bytes(const vocr_model *m, const vocr_opts *o) { (void)m; (void)o; return 64; }
int api18_spy_init(vocr_ctx *c, const vocr_model *m, const vocr_opts *o, void *arena, size_t bytes)
{ (void)c; (void)m; (void)o; (void)arena; (void)bytes; return VOCR_OK; }
int api18_spy_run(vocr_ctx *c, const vocr_image *img, char *text, size_t cap, vocr_extra *extra)
{
    (void)c; (void)cap;
    parity_record(current, img);
    text[0] = '\0';
    if (extra) extra->frames = 0;
    return VOCR_OK;
}

static uint64_t expect_bytes;
static int fake_read(void *ctx, uint64_t off, void *dst, size_t n) { (void)ctx; (void)off; (void)dst; (void)n; return -1; }
static void fake_close(void *ctx) { (void)ctx; }
/* The weights file as the resident copy saw it: same size and stamp, so the
 * core keeps the (recorded, never executed) resident weights. */
static int fake_open(void *ud, const char *path, VjoFile *out)
{
    (void)ud; (void)path;
    out->ctx = NULL;
    out->size = expect_bytes;
    out->read = fake_read;
    out->close = fake_close;
    out->stamp = 1;
    return 0;
}

int parity_api18(const ParityFrame *f, const char *model, ParityRun *out)
{
    static unsigned char weights[16] __attribute__((aligned(16)));
    const VjoVocrModel *mi = vjo_vocr_model(model);
    uint32_t x, y, w, h, bpp = fmt_bpp(f->fmt);
    Api18Capture cap;
    uint8_t *tmp;
    void *ws = NULL;
    size_t bytes;
    VjoPlatform p;
    VjoVocrResident res;
    VjoVocrStats st;
    char text[VJO_OCR_TEXT_CAP];
    parity_reset(out);
    if (!mi) return out->rc = VJO_E_OCR_MODEL;
    capture_compute_crop(f->rx, f->ry, f->rw, f->rh, f->fb_w, f->fb_h, &x, &y, &w, &h);
    out->crop_x = x; out->crop_y = y; out->crop_w = w; out->crop_h = h;
    /* capture_copy: the region's rows into the kernel buffer */
    cap.f = f; cap.w = w; cap.h = h;
    cap.raw = malloc((size_t)w * h * 4);
    tmp = malloc((size_t)VJO_MAX_W * 4);
    if (!cap.raw || !tmp) { free(cap.raw); free(tmp); return out->rc = VJO_E_OOM; }
    for (uint32_t row = 0; row < h; row++)
        read_row(cap.raw + (size_t)row * w * 4, tmp,
                 (uintptr_t)f->pixels + ((size_t)(y + row) * f->pitch + x) * bpp, w, f->fmt);
    free(tmp);
    /* the game worker: the resident job with the weights already verified */
    bytes = vjo_vocr_job_bytes(model, w, h);
    if (!bytes || posix_memalign(&ws, 64, bytes)) { free(cap.raw); return out->rc = VJO_E_OOM; }
    memset(&p, 0, sizeof(p));
    p.file_open = fake_open;
    expect_bytes = mi->bytes;
    memset(&res, 0, sizeof(res));
    res.model = mi;
    res.data = weights;
    res.capacity = sizeof(weights);
    res.size = mi->bytes;
    res.stamp = 1;
    {
        VjoOcrImage im = {&cap, w, h, api18_rows, NULL};
        current = out;
        out->rc = vjo_vocr_ocr_resident(&p, "ux0:data/VitaJPOverlay/ocr", model, &im, &res, ws, bytes, text,
                                        sizeof(text), &st);
        current = NULL;
    }
    free(ws);
    free(cap.raw);
    return out->rc;
}
