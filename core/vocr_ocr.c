/* vita-vn-ocr backend (vocr_ocr.h): region strips -> vocr_find_lines_rows ->
 * verified weights (resident between jobs, or one-shot) -> vocr_run per line,
 * straight from the strip that holds it. No copy of the whole region is made.
 * The runtime (vocr.c, vocr_lines.c, vocr_lines_rows.c) is bee-san/vita-vn-ocr's,
 * at the commit cmake/VocrOCR.cmake pins. */
#include "vocr_ocr.h"
#include "../include/vjo_vocr_sizes.h"

#include <bearssl_hash.h>
#include <string.h>

#include "fbgrey.h"
#include "vocr.h"
#include "vocr_lines.h"
#include "vocr_lines_rows.h"

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

/* Job workspace: [finder, then recognizer arena | vocr_model, vocr_ctx]. The
 * one-shot form puts the weights in front of it. The region's pixels are not in
 * it: they live one strip at a time in the capture block. */
typedef struct {
    size_t scratch_off, structs_off, total;
    size_t scratch, finder;
} Layout;

static int region_ok(unsigned w, unsigned h)
{
    return w && h && w <= VJO_OCR_MAX_WIDTH && h <= VJO_OCR_MAX_HEIGHT;
}

static int layout(const VjoVocrModel *m, unsigned w, unsigned h, Layout *l)
{
    if (!m || !region_ok(w, h)) return -1;
    l->finder = vocr_lines_arena_bytes(w, h);
    l->scratch = ALIGN16(l->finder > m->arena_bytes ? l->finder : m->arena_bytes);
    l->scratch_off = 0;
    l->structs_off = l->scratch;
    l->total = l->structs_off + ALIGN16(sizeof(vocr_model)) + ALIGN16(sizeof(vocr_ctx));
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

#define ALIGN4(x) (((x) + (size_t)3) & ~(size_t)3)

unsigned vjo_vocr_strip_rows(unsigned width, unsigned height)
{
    unsigned rows;
    if (!region_ok(width, height)) return 0;
    rows = VJO_VOCR_STRIP_BYTES / width;
    if (rows > VJO_VOCR_STRIP_MAX_ROWS) rows = VJO_VOCR_STRIP_MAX_ROWS;
    if (rows > height) rows = height;
    return rows ? rows : 1u;
}

/* Capture block: [strip: rows x width | row checksums: 4 x height | seen: 1 bit per row]. */
size_t vjo_vocr_capture_bytes(unsigned width, unsigned height)
{
    unsigned rows = vjo_vocr_strip_rows(width, height);
    if (!rows) return 0;
    return ALIGN4((size_t)rows * width) + (size_t)height * 4u + ALIGN4(((size_t)height + 7u) / 8u);
}

static int cancelled(const VjoGreySource *im)
{
    return im->cancelled && im->cancelled(im->ud);
}

static int run_cancel(void *user)
{
    return cancelled((const VjoGreySource *)user);
}

static uint32_t clock_us(const VjoPlatform *p)
{
    return p->now_us ? (uint32_t)p->now_us(p->ud) : 0;
}

/* The region, a strip at a time, for the finder and the recognizer. Every row
 * keeps the checksum it had when first read; a later read that differs means
 * the screen changed under us (err = VJO_E_SCREEN_CHANGED). */
typedef struct {
    const VjoGreySource *src;
    const VjoPlatform *p;
    VjoVocrStats *st;
    unsigned char *strip;   /* rows [y0, y0 + rows) of the region, width bytes each */
    uint32_t *sums;         /* per region row, valid where seen */
    unsigned char *seen;    /* 1 bit per region row */
    unsigned w, h, strip_rows, y0, rows;
    unsigned first_next;    /* the first pass's next row (h once it is in) */
    int err;
} Stream;

/* Checks (or records) the checksum of region row y, whose grey bytes are at p. */
static void stream_check(Stream *s, unsigned y, const unsigned char *p, int first)
{
    uint32_t sum = vjo_fnv1a(VJO_FNV_INIT, p, s->w);
    unsigned char bit = (unsigned char)(1u << (y & 7u));
    if (s->seen[y >> 3] & bit) {
        if (s->sums[y] != sum) {
            s->st->tears++;
            s->err = VJO_E_SCREEN_CHANGED;
        }
        return;
    }
    s->seen[y >> 3] |= bit;
    s->sums[y] = sum;
    if (first) s->st->region_sum = vjo_fnv1a(s->st->region_sum, (const unsigned char *)&sum, 4);
}

/* Rows [y, y + n) through the source into dst (stride w), checked. */
static int stream_read(Stream *s, unsigned y, unsigned n, unsigned char *dst)
{
    unsigned flags = 0;
    uint32_t t0;
    int got;
    if (s->err) return -1;
    if (cancelled(s->src)) { s->err = VJO_E_CANCELLED; return -1; }
    if (y == s->first_next) {
        flags = VJO_STRIP_FIRST | (y + n == s->h ? VJO_STRIP_LAST : 0u);
    }
    t0 = clock_us(s->p);
    got = s->src->strip(s->src->ud, y, n, dst, s->w, flags);
    s->st->capture_us += clock_us(s->p) - t0;
    s->st->strips++;
    if (got != (int)n) {
        s->err = got < 0 ? got : VJO_E_SOURCE;
        return -1;
    }
    if (flags) s->first_next = y + n;
    for (unsigned r = 0; r < n; r++) stream_check(s, y + r, dst + (size_t)r * s->w, flags != 0);
    return s->err ? -1 : 0;
}

/* The strip from row y: during the first pass the strips follow each other
 * from row 0 (the kernel builds the capture's scene signature from them, and
 * the last one publishes it), otherwise it is moved up to stay inside the region. */
static int stream_fetch(Stream *s, unsigned y)
{
    unsigned n = s->strip_rows;
    if (y == s->first_next && s->first_next < s->h) {
        if (n > s->h - y) n = s->h - y;
    } else if (y + n > s->h) {
        y = s->h - n;
    }
    s->rows = 0;
    if (stream_read(s, y, n, s->strip) < 0) return -1;
    s->y0 = y;
    s->rows = n;
    return 0;
}

static const unsigned char *stream_row(void *user, unsigned y)
{
    Stream *s = (Stream *)user;
    if (s->err || y >= s->h) return NULL;
    if (!s->rows || y < s->y0 || y >= s->y0 + s->rows) {
        /* rows run in order within a pass: the strip starts at the row asked for,
         * except during the first pass, whose strips must follow each other */
        if (stream_fetch(s, y) < 0) return NULL;
    }
    return s->strip + (size_t)(y - s->y0) * s->w;
}

/* rc for a finder or row failure: the source's own error first. */
static int stream_error(const Stream *s, int fallback)
{
    return s->err ? s->err : fallback;
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
                         const VjoGreySource *im)
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
                            VjoVocrResident *r, const VjoGreySource *im, VjoVocrStats *st)
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

/* Line ln's grey pixels and their stride: from the strip (stride s->w), read
 * again from its top row if the strip does not hold the whole line; a line
 * taller than a strip goes through the strip a strip at a time (each row checked
 * as always) and only its own columns are kept, in the source's line buffer
 * (stride ln->w: ln->w x ln->h bytes). NULL: the line is left out (tall, no
 * buffer) or the read failed (s->err). */
static const unsigned char *line_pixels(Stream *s, const VocrLine *ln, size_t *stride)
{
    if (ln->h <= s->strip_rows) {
        if (!s->rows || ln->y < s->y0 || ln->y + ln->h > s->y0 + s->rows) {
            if (stream_fetch(s, ln->y) < 0) return NULL;
        }
        *stride = s->w;
        return s->strip + (size_t)(ln->y - s->y0) * s->w + ln->x;
    }
    {
        size_t bytes = (size_t)ln->w * ln->h;
        unsigned char *buf = bytes <= VJO_VOCR_LINE_MAX_BYTES && s->src->line_buffer
                             ? (unsigned char *)s->src->line_buffer(s->src->ud, bytes) : NULL;
        s->st->tall++;
        if (!buf) {
            s->st->tall_skipped++;
            return NULL;
        }
        if (bytes > s->st->line_bytes) s->st->line_bytes = bytes;
        for (unsigned y = 0; y < ln->h;) {
            unsigned n = ln->h - y < s->strip_rows ? ln->h - y : s->strip_rows;
            s->rows = 0;
            if (stream_read(s, ln->y + y, n, s->strip) < 0) return NULL;
            for (unsigned r = 0; r < n; r++)
                memcpy(buf + (size_t)(y + r) * ln->w, s->strip + (size_t)r * s->w + ln->x, ln->w);
            s->y0 = ln->y + y; /* the strip now holds these rows */
            s->rows = n;
            y += n;
        }
        *stride = ln->w;
        return buf;
    }
}

/* One capture of the region: lines, then (weights) each line recognized. */
static int recognize(const VjoPlatform *p, const char *dir, const VjoVocrModel *mi,
                     const VjoGreySource *im, VjoVocrResident *res, unsigned char *ws, const Layout *l,
                     Stream *s, char *text, size_t cap, VjoVocrStats *st)
{
    const unsigned w = im->width;
    VocrLinesRows region = {w, im->height, VOCR_LINES_GREY, stream_row, s};
    VocrLineSet set;
    vocr_model *model = (vocr_model *)(void *)(ws + l->structs_off);
    vocr_ctx *ctx = (vocr_ctx *)(void *)(ws + l->structs_off + ALIGN16(sizeof(vocr_model)));
    vocr_opts opts;
    size_t used = 0;
    uint32_t t0;
    int rc;

    text[0] = '\0';
    st->lines = st->truncated = st->dark = st->frames = st->recognized = 0;
    t0 = clock_us(p);
    rc = vocr_find_lines_rows(&region, (int)mi->finder_mode, ws + l->scratch_off, l->finder, &set);
    if (rc == VOCR_LINES_E_SOURCE) return stream_error(s, VJO_E_SOURCE);
    if (rc) return rc == VOCR_LINES_E_ARENA ? VJO_E_OOM : VJO_E_OCR_REGION;
    /* A region inside one strip was read once: read it again, so that a screen
     * that changed while it was read is seen (the strips of a taller region are
     * all read twice by the finder's two passes already). */
    if (s->strip_rows >= im->height && stream_fetch(s, 0) < 0) return stream_error(s, VJO_E_SOURCE);
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
        const unsigned char *pixels;
        size_t stride = w;
        vocr_image img;
        vocr_extra extra;
        size_t start = used ? used + 1 : 0, n;
        uint32_t line_t0;
        if (cancelled(im)) return VJO_E_CANCELLED;
        if (start + 1 >= cap) return VJO_E_TOO_LARGE;
        pixels = line_pixels(s, ln, &stride);
        if (s->err) return s->err;
        if (!pixels) continue; /* taller than a strip and no line buffer: left out */
        img.pixels = pixels;
        img.width = ln->w;
        img.height = ln->h;
        img.stride = stride;
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
                   const VjoGreySource *image, VjoVocrResident *res, void *workspace,
                   size_t workspace_size, void *capture, size_t capture_size, int one_shot,
                   char *text, size_t text_cap, VjoVocrStats *stats)
{
    const VjoVocrModel *mi;
    Layout l;
    Stream st;
    size_t front, need_capture;
    unsigned strip_rows;
    int rc = VJO_OK;
    if (!platform || !model_dir || !model || !image || !res || !text || !text_cap || !stats)
        return VJO_E_SOURCE;
    text[0] = '\0';
    memset(stats, 0, sizeof(*stats));
    mi = vjo_vocr_model(model);
    if (!mi) return VJO_E_OCR_MODEL;
    if (!image->strip || layout(mi, image->width, image->height, &l) < 0) return VJO_E_OCR_REGION;
    front = one_shot ? ALIGN16(mi->bytes) : 0;
    strip_rows = vjo_vocr_strip_rows(image->width, image->height);
    need_capture = vjo_vocr_capture_bytes(image->width, image->height);
    stats->workspace = front + l.total;
    stats->capture = need_capture;
    stats->model_bytes = mi->bytes;
    stats->scratch_bytes = l.scratch;
    stats->width = image->width;
    stats->height = image->height;
    stats->strip_rows = strip_rows;
    if (cancelled(image)) return VJO_E_CANCELLED;
    if (!workspace || ((uintptr_t)workspace & 15u) || workspace_size < front + l.total ||
        !capture || capture_size < need_capture) return VJO_E_OOM;
    memset(&st, 0, sizeof(st));
    st.src = image;
    st.p = platform;
    st.st = stats;
    st.w = image->width;
    st.h = image->height;
    st.strip_rows = strip_rows;
    st.strip = (unsigned char *)capture;
    st.sums = (uint32_t *)(void *)((unsigned char *)capture + ALIGN4((size_t)strip_rows * st.w));
    st.seen = (unsigned char *)(st.sums + st.h);
    /* The screen may change while it is read (the player advanced the text): then
     * the region is captured again from scratch, up to VJO_VOCR_TRIES times. */
    for (unsigned attempt = 1; attempt <= VJO_VOCR_TRIES; attempt++) {
        memset(st.seen, 0, ((size_t)st.h + 7u) / 8u);
        st.rows = st.y0 = st.first_next = 0;
        st.err = 0;
        stats->tries = attempt;
        stats->region_sum = VJO_FNV_INIT;
        rc = recognize(platform, model_dir, mi, image, res, (unsigned char *)workspace + front, &l, &st,
                       text, text_cap, stats);
        if (rc != VJO_E_SCREEN_CHANGED || cancelled(image)) break;
    }
    if (rc) text[0] = '\0'; /* never publish a partial recognition */
    return rc;
}

int vjo_vocr_ocr_resident(const VjoPlatform *platform, const char *model_dir, const char *model,
                          const VjoGreySource *image, VjoVocrResident *resident,
                          void *workspace, size_t workspace_size, void *capture, size_t capture_size,
                          char *text, size_t text_cap, VjoVocrStats *stats)
{
    return run_job(platform, model_dir, model, image, resident, workspace, workspace_size, capture,
                   capture_size, 0, text, text_cap, stats);
}

static void *one_shot_alloc(void *ud, size_t bytes)
{
    (void)bytes;
    return ud; /* the front of the caller's workspace, sized by vjo_vocr_workspace_bytes */
}

int vjo_vocr_ocr(const VjoPlatform *platform, const char *model_dir, const char *model,
                 const VjoGreySource *image, void *workspace, size_t workspace_size,
                 void *capture, size_t capture_size, char *text, size_t text_cap, VjoVocrStats *stats)
{
    VjoVocrResident one;
    memset(&one, 0, sizeof(one));
    one.alloc = one_shot_alloc;
    one.ud = workspace;
    return run_job(platform, model_dir, model, image, &one, workspace, workspace_size, capture,
                   capture_size, 1, text, text_cap, stats);
}

/* ---- an RGBA image in memory as a grey source (host tools and tests) ---- */
static int rgba_strip(void *ud, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags)
{
    const VjoRgbaImage *img = (const VjoRgbaImage *)ud;
    (void)flags;
    if (y >= img->height || n > img->height - y) return VJO_E_SOURCE;
    for (unsigned r = 0; r < n; r++) {
        const unsigned char *p = img->pixels + (size_t)(y + r) * img->stride;
        unsigned char *d = dst + (size_t)r * stride;
        for (unsigned x = 0; x < img->width; x++, p += 4)
            d[x] = (unsigned char)vjo_grey(p[0], p[1], p[2]);
    }
    return (int)n;
}

void vjo_grey_source_rgba(const VjoRgbaImage *img, VjoGreySource *out)
{
    memset(out, 0, sizeof(*out));
    out->ud = (void *)img;
    out->width = img->width;
    out->height = img->height;
    out->strip = rgba_strip;
}
