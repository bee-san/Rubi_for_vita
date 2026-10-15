/* Google Lens as a job of the game worker: core/lens_job.h. */
#include "lens_job.h"

#include <string.h>

#include "client.h" /* vjo_lens_ocr, VjoJpegSource */
#include "fbgrey.h" /* vjo_fnv1a */
#include "jpegsw.h"
#include "utf.h"

#define BAND_ROWS 16u  /* the encoder's MCU band (core/jpegsw.c) */
#define ENC_ROOM 8192u /* the encoder's state before its band: its Huffman and quantisation tables */

static size_t sums_bytes(unsigned height)
{
    return ((size_t)height * sizeof(uint32_t) + 7u) & ~(size_t)7u;
}

size_t vjo_lens_job_bytes(unsigned width, unsigned height)
{
    if (!width || !height || width > VJO_OCR_MAX_WIDTH || height > VJO_OCR_MAX_HEIGHT)
        return 0;
    return ENC_ROOM + (size_t)width * 4u * BAND_ROWS + sums_bytes(height) + VJO_LENS_JPEG_ROOM(width, height) +
           VJO_LENS_NET_ROOM;
}

unsigned vjo_lens_strip_rows(unsigned width, size_t strip_bytes)
{
    unsigned rows = VJO_VOCR_STRIP_MAX_ROWS;
    if (!width)
        return 0;
    while (rows >= BAND_ROWS && (size_t)rows * width > strip_bytes)
        rows -= BAND_ROWS;
    return rows >= BAND_ROWS ? rows : 0;
}

static uint64_t clock_us(const VjoPlatform *p)
{
    return p->now_us ? p->now_us(p->ud) : 0;
}

/* The region's strips, in order from row 0, for one pass. The encoding pass
 * flags its strips as the job's first pass and records each row's checksum;
 * the comparing pass flags nothing and counts the rows that read otherwise. */
typedef struct {
    const VjoGreySource *im;
    const VjoPlatform *p;
    unsigned char *strip;
    uint32_t *sums;
    unsigned rows;  /* rows per strip */
    unsigned y0, n; /* the strip held: region rows [y0, y0 + n) */
    int encoding;   /* this pass's strips are flagged and recorded */
    int err;        /* the source's error, VJO_E_CANCELLED, or 0 */
    VjoLensJobStats *st;
} Pass;

static int pass_fetch(Pass *s, unsigned y)
{
    unsigned w = s->im->width, m = s->im->height - y < s->rows ? s->im->height - y : s->rows, flags = 0;
    uint64_t t0;
    int rc;
    if (s->im->cancelled && s->im->cancelled(s->im->ud)) {
        s->err = VJO_E_CANCELLED;
        return -1;
    }
    if (s->encoding)
        flags = VJO_STRIP_FIRST | (y + m == s->im->height ? VJO_STRIP_LAST : 0u);
    t0 = clock_us(s->p);
    rc = s->im->strip(s->im->ud, y, m, s->strip, w, flags);
    s->st->capture_us += (uint32_t)(clock_us(s->p) - t0);
    s->st->strips++;
    if (rc != (int)m) {
        s->err = rc < 0 ? rc : VJO_E_SOURCE;
        return -1;
    }
    s->y0 = y;
    s->n = m;
    for (unsigned r = 0; r < m; r++) {
        uint32_t sum = vjo_fnv1a(VJO_FNV_INIT, s->strip + (size_t)r * w, w);
        if (s->encoding)
            s->sums[y + r] = sum;
        else if (s->sums[y + r] != sum)
            s->st->tears++;
    }
    return 0;
}

/* The encoder's rows: each band of 16 rows is in one strip (a strip is a
 * multiple of 16 rows), the next strip is read when a band starts past the one
 * held, and grey becomes R = G = B (the JPEG's luma is the grey, its chroma flat). */
static int band_rows(void *ud, uint32_t row, uint32_t n, uint8_t *dst)
{
    Pass *s = (Pass *)ud;
    unsigned w = s->im->width;
    if ((!s->n || row < s->y0 || row + n > s->y0 + s->n) && pass_fetch(s, row) < 0)
        return -1;
    for (uint32_t i = 0; i < n; i++) {
        const unsigned char *g = s->strip + (size_t)(row - s->y0 + i) * w;
        uint8_t *o = dst + (size_t)i * w * 4u;
        for (unsigned x = 0; x < w; x++, o += 4) {
            o[0] = o[1] = o[2] = g[x];
            o[3] = 255;
        }
    }
    return (int)n;
}

typedef struct {
    const uint8_t *data;
} Jpeg;

static int jpeg_read(void *ud, uint32_t off, void *dst, uint32_t len)
{
    memcpy(dst, ((Jpeg *)ud)->data + off, len);
    return 0;
}

int vjo_lens_job(const VjoPlatform *p, const VjoGreySource *image, unsigned char *strip, size_t strip_bytes,
                 void *workspace, size_t workspace_bytes, int quality, char *text, size_t text_cap,
                 VjoErr *err, VjoLensJobStats *st)
{
    unsigned w = image->width, h = image->height;
    size_t need = vjo_lens_job_bytes(w, h), scratch_bytes = ENC_ROOM + (size_t)w * 4u * BAND_ROWS, len, jpeg_mark;
    VjoArena scratch, main_arena;
    VjoBuf jb;
    Pass pass;
    Jpeg jpeg;
    VjoJpegSource src;
    VjoLensResult lens;
    const char *t = NULL;
    uint64_t t0;
    int rc;

    memset(err, 0, sizeof(*err));
    memset(st, 0, sizeof(*st));
    if (text_cap)
        text[0] = '\0';
    if (!need || !text_cap || !image->strip)
        return err->rc = VJO_E_OCR_REGION;
    if (workspace_bytes < need)
        return err->rc = VJO_E_OOM;
    st->strip_rows = vjo_lens_strip_rows(w, strip_bytes);
    if (!st->strip_rows)
        return err->rc = VJO_E_OOM;
    vjo_arena_init(&scratch, workspace, scratch_bytes);
    vjo_arena_init(&main_arena, (uint8_t *)workspace + scratch_bytes, workspace_bytes - scratch_bytes);
    memset(&pass, 0, sizeof(pass));
    pass.im = image;
    pass.p = p;
    pass.strip = strip;
    pass.rows = st->strip_rows;
    pass.st = st;
    pass.sums = (uint32_t *)vjo_arena_alloc(&main_arena, sums_bytes(h));
    jpeg_mark = vjo_arena_mark(&main_arena);
    if (!pass.sums)
        return err->rc = VJO_E_OOM;

    for (;;) {
        /* one try: an encoding pass, then a comparing pass */
        vjo_arena_reset(&scratch);
        vjo_arena_release(&main_arena, jpeg_mark);
        vjo_buf_init(&jb, &main_arena);
        pass.encoding = 1;
        pass.n = 0;
        pass.err = 0;
        st->tries++;
        uint32_t before = st->capture_us;
        t0 = clock_us(p);
        rc = vjo_jpeg_encode(&scratch, w, h, w * 4u, band_rows, &pass, quality, &jb);
        st->jpeg_us += (uint32_t)(clock_us(p) - t0) - (st->capture_us - before);
        if (scratch.peak > st->peak)
            st->peak = scratch.peak;
        if (rc < 0)
            return err->rc = pass.err ? pass.err : jb.oom ? VJO_E_OOM : VJO_E_SOURCE;
        if (st->tries >= VJO_VOCR_TRIES)
            break; /* the last try's image goes, whatever a comparison would say */
        pass.encoding = 0;
        st->tears = 0;
        for (unsigned y = 0; y < h; y += pass.rows)
            if (pass_fetch(&pass, y) < 0)
                return err->rc = pass.err;
        if (!st->tears)
            break;
    }
    st->region_sum = VJO_FNV_INIT;
    for (unsigned y = 0; y < h; y++)
        st->region_sum = vjo_fnv1a(st->region_sum, (const unsigned char *)&pass.sums[y], 4);
    st->jpeg_bytes = (uint32_t)jb.len;

    memset(&src, 0, sizeof(src));
    jpeg.data = jb.data;
    src.ud = &jpeg;
    src.read = jpeg_read;
    src.size = (uint32_t)jb.len;
    src.width = w;
    src.height = h;
    t0 = clock_us(p);
    rc = vjo_lens_ocr(&main_arena, p, &src, &lens, &t, err);
    st->request_us = (uint32_t)(clock_us(p) - t0);
    st->peak += main_arena.peak;
    if (rc)
        return err->rc = rc;
    len = strlen(t);
    if (len >= text_cap) { /* cut at a character, never inside one */
        len = text_cap - 1;
        st->truncated = 1;
    }
    memcpy(text, t, len);
    text[len] = '\0';
    if (st->truncated)
        vjo_utf8_trim_partial(text);
    st->text_bytes = (uint32_t)strlen(text);
    return VJO_OK;
}
