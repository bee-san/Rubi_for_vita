/* The API 19 pipeline for the capture parity test: the kernel's crop
 * (capture_crop_of) and the display hook's grey rows (capture_grey_rows, real
 * kernel/capture.c, linked from parity_kernel.c) written into the game
 * worker's strip, a strip per call, read by the current core/vocr_ocr.c. As in
 * parity_api18.c only the runtime's inference is replaced by the recorder. */
#define vocr_load api19_spy_load
#define vocr_arena_bytes api19_spy_arena_bytes
#define vocr_init api19_spy_init
#define vocr_run api19_spy_run
/* API 18's finder (8228c3c: GRAD), so only the capture differs (core/vocr_ocr.c VJO_VOCR_FINDER) */
#define VJO_VOCR_FINDER VOCR_LINES_GRAD
#include "../../core/vocr_ocr.c"

#include <stdlib.h>
#include "parity.h"


typedef struct {
    const ParityFrame *f;
    uint32_t x, y, w, h;
    unsigned char *line;    /* the tall-line block (game_ocr/main.c line_buffer) */
    size_t line_bytes;
} Api19Capture;

/* vjoOcrCaptureRows on the hook rung: the display hook copies region rows
 * [y, y + n) of this frame as grey into dst (the worker's capture block). */
static int api19_strip(void *ud, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags)
{
    const Api19Capture *c = ud;
    ParityRowsJob job = {c->x, c->y, c->w, c->h, c->f->fb_w, c->f->fb_h, y, n, (uint32_t)stride,
                         (uintptr_t)dst, NULL, 0, 0};
    (void)flags;
    return parity_grey_rows((uintptr_t)c->f->pixels, c->f->pitch, c->f->fmt, c->f->fb_w, c->f->fb_h, &job);
}

static void *api19_line(void *ud, size_t bytes)
{
    Api19Capture *c = ud;
    if (bytes > c->line_bytes) {
        free(c->line);
        c->line = malloc(bytes);
        c->line_bytes = c->line ? bytes : 0;
    }
    return c->line;
}

static ParityRun *current;
int api19_spy_load(vocr_model *m, const void *blob, size_t bytes)
{
    (void)blob; (void)bytes;
    memset(m, 0, sizeof(*m));
    return VOCR_OK;
}
size_t api19_spy_arena_bytes(const vocr_model *m, const vocr_opts *o) { (void)m; (void)o; return 64; }
int api19_spy_init(vocr_ctx *c, const vocr_model *m, const vocr_opts *o, void *arena, size_t bytes)
{ (void)c; (void)m; (void)o; (void)arena; (void)bytes; return VOCR_OK; }
int api19_spy_run(vocr_ctx *c, const vocr_image *img, char *text, size_t cap, vocr_extra *extra)
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

int parity_api19(const ParityFrame *f, const char *model, ParityRun *out)
{
    static unsigned char weights[16] __attribute__((aligned(16)));
    const VjoVocrModel *mi = vjo_vocr_model(model);
    struct { uint16_t x, y, w, h; } region = {f->rx, f->ry, f->rw, f->rh};
    Api19Capture cap;
    void *ws = NULL, *block = NULL;
    size_t bytes, cap_bytes;
    VjoPlatform p;
    VjoVocrResident res;
    VjoVocrStats st;
    char text[VJO_OCR_TEXT_CAP];
    parity_reset(out);
    if (!mi) return out->rc = VJO_E_OCR_MODEL;
    memset(&cap, 0, sizeof(cap));
    cap.f = f;
    parity_crop_of(region.x, region.y, region.w, region.h, f->fb_w, f->fb_h, &cap.x, &cap.y, &cap.w, &cap.h);
    out->crop_x = cap.x; out->crop_y = cap.y; out->crop_w = cap.w; out->crop_h = cap.h;
    bytes = vjo_vocr_job_bytes(model, cap.w, cap.h);
    cap_bytes = vjo_vocr_capture_bytes(cap.w, cap.h);
    if (!bytes || !cap_bytes || posix_memalign(&ws, 64, bytes) || posix_memalign(&block, 64, cap_bytes)) {
        free(ws);
        return out->rc = VJO_E_OOM;
    }
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
        VjoGreySource src = {&cap, cap.w, cap.h, api19_strip, NULL, api19_line};
        current = out;
        out->rc = vjo_vocr_ocr_resident(&p, "ux0:data/VitaJPOverlay/ocr", model, &src, &res, ws, bytes, block,
                                        cap_bytes, text, sizeof(text), &st);
        current = NULL;
        out->tall = st.tall;
    }
    free(cap.line);
    free(block);
    free(ws);
    return out->rc;
}
