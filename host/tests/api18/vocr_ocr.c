/* vita-vn-ocr backend (vocr_ocr.h): region -> grey -> vocr_find_lines ->
 * verified weights (resident between jobs, or one-shot) -> vocr_run per line.
 * The runtime (vocr.c, vocr_lines.c) is bee-san/vita-vn-ocr's, at the commit
 * cmake/VocrOCR.cmake pins. */
#include "vocr_ocr.h"
#include "../include/vjo_vocr_sizes.h"

#include <bearssl_hash.h>
#include <string.h>

#include "vocr.h"
#include "vocr_lines.h"

typedef char vjo_vocr_max_lines_matches_runtime[VJO_VOCR_MAX_LINES == VOCR_LINES_MAX ? 1 : -1];
/* The int8 assets of github.com/bee-san/vita-vn-ocr release v0.2.0
 * (docs/vita-vn-ocr.md). Arena bytes: vocr_arena_bytes() in W8I mode with the
 * default 4-frame tiles and 16-frame head batches, any line width. */
#ifdef VJO_VOCR_TEST_MODELS
/* Host lifecycle tests only (host/tests/test_game_resident.c): synthetic pins. */
#include VJO_VOCR_TEST_MODELS
#else
static const VjoVocrModel models[] = {
    {"H15_w8.vocr", VJO_VOCR_H15_BYTES,
     {0x31,0x6e,0xbc,0x37,0x7e,0xc3,0x45,0x20,0x44,0x09,0x91,0xc5,0x08,0x51,0x99,0xd8,
      0xe7,0x40,0x31,0xf0,0x1a,0x86,0x79,0x38,0xbe,0x90,0xa6,0x0c,0x1e,0x3f,0x03,0xcd},
     182944, VOCR_LINES_GRAD},
    {"FL10_w8.vocr", VJO_VOCR_FL10_BYTES,
     {0x50,0x0e,0x0b,0x84,0x2e,0x5f,0x0c,0xb3,0x26,0xf5,0x5e,0x7d,0x99,0x1a,0x52,0xab,
      0x66,0xdd,0x24,0x97,0x7c,0xcc,0x89,0xbd,0x18,0x82,0x28,0xae,0xdc,0x82,0x5e,0x84},
     124832, VOCR_LINES_GRAD},
    {"F20_w8.vocr", VJO_VOCR_F20_BYTES,
     {0xf8,0xc1,0xab,0xe4,0x74,0x3a,0x71,0xa7,0x42,0xe7,0x18,0x52,0x3f,0x42,0x06,0xf9,
      0x92,0xd3,0xbd,0x4f,0xb6,0x9d,0xe3,0x21,0x99,0x7b,0x2b,0xd6,0xf1,0x67,0x79,0xc0},
     241056, VOCR_LINES_GRAD},
#if VJO_VOCR2_ENABLED
    /* Experimental trained FL10-sized recognizer and native textbox finder.
     * The strict per-title guard failed; this backend is explicitly opt-in. */
    {VJO_VOCR2_ARTIFACT_NAME, VJO_VOCR2_BYTES,
     {0xd3,0xad,0xc5,0x2e,0x5c,0xf6,0x2b,0x54,0x8a,0xf0,0x4c,0x5a,0xb0,0x7a,0x2f,0x57,
      0xcf,0x04,0x19,0x38,0x6b,0xbd,0xf4,0x9b,0xa1,0x9c,0x97,0x03,0xbe,0x6e,0x17,0x66},
     124832, VOCR_LINES_TEXTBOX},
#endif
};
#endif /* VJO_VOCR_TEST_MODELS */
#define N_MODELS (sizeof(models) / sizeof(models[0]))

#define ALIGN16(x) (((x) + (size_t)15) & ~(size_t)15)
/* Weights are read and hashed in pieces of this size: the game worker's kernel
 * broker stages exactly one piece per request (VJO_GAME_OCR_MODEL_CHUNK). */
#define READ_CHUNK 32768u

const VjoVocrModel *vjo_vocr_models(unsigned *count)
{
    if (count) *count = (unsigned)N_MODELS;
    return models;
}

const VjoVocrModel *vjo_vocr_model(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < N_MODELS; i++)
        if (!strcmp(name, models[i].name)) return &models[i];
    return NULL;
}

/* Job workspace: [finder, then recognizer arena | vocr_model, vocr_ctx | grey region].
 * Before the finder runs, the arena holds the RGBA rows being copied. The
 * one-shot form puts the weights in front of it. */
typedef struct {
    size_t scratch_off, structs_off, grey_off, total;
    size_t scratch, finder;
} Layout;

static int layout(const VjoVocrModel *m, unsigned w, unsigned h, Layout *l)
{
    if (!m || !w || !h || w > VJO_OCR_MAX_WIDTH || h > VJO_OCR_MAX_HEIGHT) return -1;
    l->finder = vocr_lines_arena_bytes(w, h);
    l->scratch = ALIGN16(l->finder > m->arena_bytes ? l->finder : m->arena_bytes);
    l->scratch_off = 0;
    l->structs_off = l->scratch;
    l->grey_off = l->structs_off + ALIGN16(sizeof(vocr_model)) + ALIGN16(sizeof(vocr_ctx));
    l->total = l->grey_off + ALIGN16((size_t)w * h);
    return 0;
}

size_t vjo_vocr_job_bytes(const char *model, unsigned width, unsigned height)
{
    Layout l;
    return layout(vjo_vocr_model(model), width, height, &l) < 0 ? 0 : l.total;
}

size_t vjo_vocr_workspace_bytes(const char *model, unsigned width, unsigned height)
{
    const VjoVocrModel *m = vjo_vocr_model(model);
    Layout l;
    return layout(m, width, height, &l) < 0 ? 0 : ALIGN16(m->bytes) + l.total;
}

static int cancelled(const VjoOcrImage *im)
{
    return im->cancelled && im->cancelled(im->ud);
}

static int run_cancel(void *user)
{
    return cancelled((const VjoOcrImage *)user);
}

static uint32_t clock_us(const VjoPlatform *p)
{
    return p->now_us ? (uint32_t)p->now_us(p->ud) : 0;
}

/* All rows, once each and in order (the kernel frees a single-pass capture
 * after its last row), as grey = (77 R + 150 G + 29 B) >> 8: the luma both
 * the finder and the recognizer compute from RGBA, so this copy changes no
 * result. rgba holds whole rows (cap bytes). */
static int read_grey(const VjoOcrImage *im, unsigned char *rgba, size_t cap, unsigned char *grey)
{
    const unsigned w = im->width, batch = (unsigned)(cap / ((size_t)w * 4));
    if (!batch) return VJO_E_OOM;
    for (unsigned y = 0; y < im->height;) {
        unsigned n = im->height - y < batch ? im->height - y : batch;
        int got;
        if (cancelled(im)) return VJO_E_CANCELLED;
        got = im->rows(im->ud, y, n, rgba);
        if (got <= 0 || (unsigned)got > n) return VJO_E_SOURCE;
        for (unsigned r = 0; r < (unsigned)got; r++) {
            const unsigned char *s = rgba + (size_t)r * w * 4;
            unsigned char *d = grey + (size_t)(y + r) * w;
            for (unsigned x = 0; x < w; x++, s += 4)
                d[x] = (unsigned char)((77u * s[0] + 150u * s[1] + 29u * s[2]) >> 8);
        }
        y += (unsigned)got;
    }
    return VJO_OK;
}

int vjo_vocr_resident_drop(VjoVocrResident *r)
{
    if (!r) return 0;
    r->model = NULL;
    r->size = r->stamp = 0;
    if (!r->data) return 0;
    if (r->release && r->release(r->ud)) return -1;
    r->data = NULL;
    r->capacity = 0;
    return 0;
}

/* rc, after releasing the resident memory; VJO_E_OCR_UNAVAILABLE if that failed. */
static int drop_with(VjoVocrResident *r, int rc)
{
    return vjo_vocr_resident_drop(r) ? VJO_E_OCR_UNAVAILABLE : rc;
}

/* Reads the whole open file into dst and checks its SHA-256 while reading;
 * cancellation is checked between pieces. */
static int read_verified(const VjoFile *file, const VjoVocrModel *m, unsigned char *dst,
                         const VjoOcrImage *im)
{
    br_sha256_context hash;
    unsigned char digest[32];
    br_sha256_init(&hash);
    for (size_t off = 0; off < m->bytes;) {
        size_t n = m->bytes - off < READ_CHUNK ? m->bytes - off : READ_CHUNK;
        if (cancelled(im)) return VJO_E_CANCELLED;
        if (file->read(file->ctx, off, dst + off, n) < 0)
            return cancelled(im) ? VJO_E_CANCELLED : VJO_E_OCR_MODEL;
        br_sha256_update(&hash, dst + off, n);
        off += n;
    }
    br_sha256_out(&hash, digest);
    return memcmp(digest, m->sha256, sizeof(digest)) ? VJO_E_OCR_MODEL : VJO_OK;
}

/* Verified weights of m in r->data. The file is always opened (its size and
 * stamp decide); it is read and hashed again only if they changed, the stamp
 * is unknown, or no verified copy is held. */
static int resident_weights(const VjoPlatform *p, const char *dir, const VjoVocrModel *m,
                            VjoVocrResident *r, const VjoOcrImage *im, VjoVocrStats *st)
{
    char path[256];
    size_t dn = strlen(dir), fn = strlen(m->name);
    VjoFile file;
    int rc;
    /* Another model's weights: never hold two. */
    if (r->model != m && vjo_vocr_resident_drop(r)) return VJO_E_OCR_UNAVAILABLE;
    if (!dn || dn + 1 + fn >= sizeof(path)) return drop_with(r, VJO_E_OCR_MODEL);
    memcpy(path, dir, dn);
    path[dn] = '/';
    memcpy(path + dn + 1, m->name, fn + 1);
    memset(&file, 0, sizeof(file));
    if (!p->file_open || p->file_open(p->ud, path, &file) < 0)
        /* A cancelled check proves nothing about the file: keep verified weights. */
        return cancelled(im) ? VJO_E_CANCELLED : drop_with(r, VJO_E_OCR_MODEL);
    if (file.size != m->bytes || !file.read || !file.close) {
        if (file.close) file.close(file.ctx);
        return drop_with(r, VJO_E_OCR_MODEL);
    }
    if (r->model == m && r->data && file.stamp && file.stamp == r->stamp && file.size == r->size) {
        file.close(file.ctx);
        st->model_cached = 1;
        return VJO_OK;
    }
    /* From here until the hash matches, the memory holds no weights. */
    r->model = NULL;
    if (!r->data) {
        unsigned char *data = r->alloc ? (unsigned char *)r->alloc(r->ud, m->bytes) : NULL;
        if (!data) {
            file.close(file.ctx);
            return VJO_E_OOM;
        }
        r->data = data;
        r->capacity = m->bytes;
        if ((uintptr_t)data & 15u) {
            file.close(file.ctx);
            return drop_with(r, VJO_E_OOM);
        }
    }
    rc = read_verified(&file, m, r->data, im);
    file.close(file.ctx);
    if (rc) return drop_with(r, rc);
    r->model = m;
    r->size = file.size;
    r->stamp = file.stamp;
    st->model_loaded = 1;
    return VJO_OK;
}

static int run_error(int rc)
{
    switch (rc) {
    case VOCR_E_TEXT: return VJO_E_TOO_LARGE;
    case VOCR_E_CANCELLED: return VJO_E_CANCELLED;
    default: return VJO_E_OCR_INFERENCE;
    }
}

static int recognize(const VjoPlatform *p, const char *dir, const VjoVocrModel *mi,
                     const VjoOcrImage *im, VjoVocrResident *res, unsigned char *ws, const Layout *l,
                     char *text, size_t cap, VjoVocrStats *st)
{
    unsigned char *grey = ws + l->grey_off;
    const unsigned w = im->width;
    VocrLinesImage region = {grey, w, im->height, w, VOCR_LINES_GREY};
    VocrLineSet set;
    vocr_model *model = (vocr_model *)(void *)(ws + l->structs_off);
    vocr_ctx *ctx = (vocr_ctx *)(void *)(ws + l->structs_off + ALIGN16(sizeof(vocr_model)));
    vocr_opts opts;
    size_t used = 0;
    uint32_t t0;
    int rc;

    t0 = clock_us(p);
    rc = read_grey(im, ws + l->scratch_off, l->scratch, grey);
    if (rc) return rc;
    rc = vocr_find_lines(&region, mi->finder_mode, ws + l->scratch_off, l->finder, &set);
    if (rc) return rc == VOCR_LINES_E_ARENA ? VJO_E_OOM : VJO_E_OCR_REGION;
    st->find_us = clock_us(p) - t0;
    st->lines = set.count;
    st->truncated = set.truncated;
    for (unsigned i = 0; i < set.count; i++)
        st->dark += set.lines[i].dark ? 1u : 0u;
    if (!set.count) return VJO_OK; /* nothing to read: the model file is not opened */
    if (cancelled(im)) return VJO_E_CANCELLED;

    t0 = clock_us(p);
    rc = resident_weights(p, dir, mi, res, im, st);
    if (rc) return rc;
    /* Verified bytes the runtime refuses would fail every job: release them. */
    if (vocr_load(model, res->data, mi->bytes) != VOCR_OK) return drop_with(res, VJO_E_OCR_MODEL);
    memset(&opts, 0, sizeof(opts));
    opts.mode = VOCR_MODE_W8I;
    {
        size_t need = vocr_arena_bytes(model, &opts);
        if (!need || need > l->scratch) return drop_with(res, VJO_E_OCR_MODEL);
        if (vocr_init(ctx, model, &opts, ws + l->scratch_off, need) != VOCR_OK) return VJO_E_OCR_INFERENCE;
    }
    st->load_us = clock_us(p) - t0;

    t0 = clock_us(p);
    for (unsigned i = 0; i < set.count; i++) {
        const VocrLine *ln = &set.lines[i];
        vocr_image img;
        vocr_extra extra;
        size_t start = used ? used + 1 : 0, n;
        uint32_t line_t0;
        if (cancelled(im)) return VJO_E_CANCELLED;
        if (start + 1 >= cap) return VJO_E_TOO_LARGE;
        img.pixels = grey + (size_t)ln->y * w + ln->x;
        img.width = ln->w;
        img.height = ln->h;
        img.stride = w;
        img.format = VOCR_GREY8 | (ln->dark ? VOCR_INVERT : 0);
        memset(&extra, 0, sizeof(extra));
        extra.cancel = run_cancel;
        extra.user = (void *)im;
        line_t0 = clock_us(p);
        rc = vocr_run(ctx, &img, text + start, cap - start, &extra);
        if (rc != VOCR_OK) return run_error(rc);
        st->frames += extra.frames;
        if (st->recognized < VJO_VOCR_MAX_LINES) {
            VjoVocrLineStat *ls = &st->line[st->recognized++];
            ls->width = (uint16_t)ln->w;
            ls->height = (uint16_t)ln->h;
            ls->frames = (uint16_t)(extra.frames > 0xFFFFu ? 0xFFFFu : extra.frames);
            ls->us = clock_us(p) - line_t0;
        }
        n = strlen(text + start);
        if (!n) { /* nothing read on this line: no separator either */
            text[used] = '\0';
            continue;
        }
        if (start) text[used] = '\n';
        used = start + n;
    }
    st->recognize_us = clock_us(p) - t0;
    return VJO_OK;
}

/* one_shot: the weights occupy the front of workspace for this job only. */
static int run_job(const VjoPlatform *platform, const char *model_dir, const char *model,
                   const VjoOcrImage *image, VjoVocrResident *res, void *workspace,
                   size_t workspace_size, int one_shot, char *text, size_t text_cap,
                   VjoVocrStats *stats)
{
    const VjoVocrModel *mi;
    Layout l;
    size_t front;
    int rc;
    if (!platform || !model_dir || !model || !image || !res || !text || !text_cap || !stats)
        return VJO_E_SOURCE;
    text[0] = '\0';
    memset(stats, 0, sizeof(*stats));
    mi = vjo_vocr_model(model);
    if (!mi) return VJO_E_OCR_MODEL;
    if (!image->rows || layout(mi, image->width, image->height, &l) < 0) return VJO_E_OCR_REGION;
    front = one_shot ? ALIGN16(mi->bytes) : 0;
    stats->workspace = front + l.total;
    stats->model_bytes = mi->bytes;
    stats->scratch_bytes = l.scratch;
    stats->region_bytes = (size_t)image->width * image->height;
    stats->width = image->width;
    stats->height = image->height;
    if (cancelled(image)) return VJO_E_CANCELLED;
    if (!workspace || ((uintptr_t)workspace & 15u) || workspace_size < front + l.total) return VJO_E_OOM;
    rc = recognize(platform, model_dir, mi, image, res, (unsigned char *)workspace + front, &l,
                   text, text_cap, stats);
    if (rc) text[0] = '\0'; /* never publish a partial recognition */
    return rc;
}

int vjo_vocr_ocr_resident(const VjoPlatform *platform, const char *model_dir, const char *model,
                          const VjoOcrImage *image, VjoVocrResident *resident,
                          void *workspace, size_t workspace_size,
                          char *text, size_t text_cap, VjoVocrStats *stats)
{
    return run_job(platform, model_dir, model, image, resident, workspace, workspace_size, 0,
                   text, text_cap, stats);
}

static void *one_shot_alloc(void *ud, size_t bytes)
{
    (void)bytes;
    return ud; /* the front of the caller's workspace, sized by vjo_vocr_workspace_bytes */
}

int vjo_vocr_ocr(const VjoPlatform *platform, const char *model_dir, const char *model,
                 const VjoOcrImage *image, void *workspace, size_t workspace_size,
                 char *text, size_t text_cap, VjoVocrStats *stats)
{
    VjoVocrResident one;
    memset(&one, 0, sizeof(one));
    one.alloc = one_shot_alloc;
    one.ud = workspace;
    return run_job(platform, model_dir, model, image, &one, workspace, workspace_size, 1,
                   text, text_cap, stats);
}
