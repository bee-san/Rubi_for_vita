/* The tiny weights and LM blobs that host/tests/test_vocr_parallel.c and test_vocr_vertical.c build and pin
 * themselves (no release files): vita-vn-ocr runtime/tests/test_vocr.c's builder, height 8, stride 4. The suites'
 * pins are SHA-256 of what it builds, so it must stay byte for byte as it is. And the directory the job reads the
 * built files from, model_dir, with write_file() writing one there and giving its SHA-256. Include it once, after
 * acutest.h and bearssl_hash.h. */
#ifndef VJO_TEST_VOCR_BLOB_H
#define VJO_TEST_VOCR_BLOB_H

/* ------------------------------------------------------------ the tiny blobs
 * (vita-vn-ocr runtime/tests/test_vocr.c's builder: height 8, stride 4) */
static unsigned char blob[1 << 16] __attribute__((aligned(16)));
static size_t blob_pos;
static uint32_t lcg = 12345u;
enum { CLASSES = 12 };

static float frand(void)
{
    lcg = lcg * 1664525u + 1013904223u;
    return (float)((lcg >> 8) & 0xFFFF) / 32768.0f - 1.0f;
}
static void wr16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void wr32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static uint32_t put(const void *data, size_t n)
{
    uint32_t off = (uint32_t)blob_pos;
    memcpy(blob + blob_pos, data, n);
    blob_pos = (blob_pos + n + 15u) & ~(size_t)15u;
    return off;
}
static uint32_t put_floats(unsigned n, float scale)
{
    uint32_t off = (uint32_t)blob_pos;
    for (unsigned i = 0; i < n; i++) {
        float v = frand() * scale;
        memcpy(blob + blob_pos + 4u * i, &v, sizeof v);
    }
    blob_pos = (blob_pos + 4u * n + 15u) & ~(size_t)15u;
    return off;
}
static uint32_t crc32_ref(const unsigned char *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int b = 0; b < 8; b++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}
static void op_rec(unsigned i, unsigned type, unsigned flags, unsigned kh, unsigned kw, unsigned sh, unsigned sw,
                   unsigned ph, unsigned pw, unsigned cin, unsigned cout, unsigned hin, unsigned hout, uint32_t w_off,
                   uint32_t w_bytes, uint32_t b_off, uint32_t s_off, unsigned k, unsigned cout_pad)
{
    unsigned char *r = blob + 64 + 64 * i;
    memset(r, 0, 64);
    r[0] = (unsigned char)type; r[1] = (unsigned char)flags; r[2] = (unsigned char)kh; r[3] = (unsigned char)kw;
    r[4] = (unsigned char)sh; r[5] = (unsigned char)sw; r[6] = (unsigned char)ph; r[7] = (unsigned char)pw;
    wr16(r + 8, cin); wr16(r + 10, cout); wr16(r + 12, hin); wr16(r + 14, hout);
    wr32(r + 16, w_off); wr32(r + 20, w_bytes); wr32(r + 24, b_off); wr32(r + 28, s_off);
    wr16(r + 32, k); wr16(r + 34, cout_pad);
}
static void mat_op(unsigned i, unsigned type, unsigned flags, unsigned kh, unsigned cin, unsigned cout, unsigned hin,
                   unsigned hout)
{
    unsigned k = kh * cin, cout_pad = (cout + 7u) & ~7u, n = cout_pad * k;
    uint32_t w_off = (uint32_t)blob_pos, s_off, b_off = 0;
    for (unsigned j = 0; j < n; j++) blob[blob_pos + j] = (unsigned char)(signed char)(int)(frand() * 127.0f);
    blob_pos = (blob_pos + n + 15u) & ~(size_t)15u;
    s_off = (uint32_t)blob_pos;
    for (unsigned j = 0; j < cout_pad; j++) {
        float sc = 0.01f + 0.004f * (float)(j % 5);
        memcpy(blob + blob_pos + 4u * j, &sc, sizeof sc);
    }
    blob_pos = (blob_pos + 4u * cout_pad + 15u) & ~(size_t)15u;
    if (flags & 4) b_off = put_floats(cout_pad, 0.1f);
    op_rec(i, type, flags | 8, kh, 1, 1, 1, 0, 0, cin, cout, hin, hout, w_off, n, b_off, s_off, k, cout_pad);
}
/* classes: 0 blank, 1-11 'a'..'k' (the tiny LM knows a-d) */
static size_t build_blob(uint32_t seed)
{
    unsigned char vocab[2 * (CLASSES + 1) + CLASSES];
    unsigned pos = 0, n_ops = 7;
    uint32_t off;
    memset(blob, 0, sizeof blob);
    blob_pos = 64 + 64 * n_ops;
    lcg = seed;
    off = put_floats(9 * 8, 0.6f);
    op_rec(0, 1, 1 | 4, 3, 3, 2, 2, 1, 1, 1, 8, 8, 4, off, 9 * 8 * 4, put_floats(8, 0.1f), 0, 9, 8);
    off = put_floats(9 * 8, 0.6f);
    op_rec(1, 2, 1 | 4, 3, 3, 2, 2, 1, 1, 8, 8, 4, 2, off, 9 * 8 * 4, put_floats(8, 0.1f), 0, 9, 8);
    mat_op(2, 3, 1 | 4, 1, 8, 16, 2, 2);
    mat_op(3, 3, 1 | 4 | 16, 2, 16, 16, 2, 1);
    off = put_floats(5 * 16, 0.6f);
    op_rec(4, 2, 1 | 4, 1, 5, 1, 1, 0, 2, 16, 16, 1, 1, off, 5 * 16 * 4, put_floats(16, 0.1f), 0, 5, 16);
    mat_op(5, 3, 1 | 2 | 4, 1, 16, 16, 1, 1);
    mat_op(6, 4, 4, 1, 16, CLASSES, 1, 1);
    wr16(vocab, 0);
    wr16(vocab + 2, 0);
    for (unsigned i = 1; i < CLASSES; i++) {
        vocab[2 * (CLASSES + 1) + pos++] = (unsigned char)('a' + i - 1);
        wr16(vocab + 2 * (i + 1), pos);
    }
    off = put(vocab, 2 * (CLASSES + 1) + pos);
    memcpy(blob, "VOCR", 4);
    wr16(blob + 4, 1); wr16(blob + 6, 64); wr32(blob + 8, (uint32_t)blob_pos); wr32(blob + 12, 1u);
    wr16(blob + 16, 8); wr16(blob + 18, 256); wr16(blob + 20, 4); wr16(blob + 22, n_ops);
    wr32(blob + 24, CLASSES); wr32(blob + 28, 64); wr32(blob + 32, off); wr32(blob + 36, pos);
    wr32(blob + 40, crc32_ref(blob + 64, blob_pos - 64));
    return blob_pos;
}

/* ------------------------------------------------------------ the tiny LM
 * (vita-vn-ocr runtime/tests/test_vocr_lm.c's: order 3; tokens 0 <unk>, 1 <s>,
 * 2 </s>, 3-6 a b c d; bigrams (<s> a) (a b) (a c) (b c); trigram (a b c)) */
static unsigned char lmb[4096] __attribute__((aligned(16)));
static size_t lpos;
static uint32_t lput(const void *d, size_t n)
{
    uint32_t off = (uint32_t)lpos;
    memcpy(lmb + lpos, d, n);
    lpos = (lpos + n + 15u) & ~(size_t)15u;
    return off;
}
static void lrow(unsigned k, uint32_t n, uint32_t ot, uint32_t op, uint32_t ob, uint32_t oc, uint32_t ocp, uint32_t ocb)
{
    unsigned char *r = lmb + 64 + 32 * (k - 1);
    wr32(r, n); wr32(r + 4, ot); wr32(r + 8, op); wr32(r + 12, ob); wr32(r + 16, oc); wr32(r + 20, ocp); wr32(r + 24, ocb);
}
static size_t build_lm(void)
{
    static const uint32_t cps[4] = {0x61, 0x62, 0x63, 0x64};
    static const uint8_t p1[7] = {10, 20, 4, 2, 3, 3, 5}, b1[7] = {0, 3, 0, 1, 2, 0, 0};
    static const uint32_t c1[8] = {0, 0, 1, 1, 3, 4, 4, 4};
    static const uint16_t t2[4] = {3, 4, 5, 5};
    static const uint8_t p2[4] = {1, 2, 3, 1}, b2[4] = {0, 1, 0, 0};
    static const uint32_t c2[5] = {0, 0, 1, 1, 1};
    static const uint16_t t3[1] = {5};
    static const uint8_t p3[1] = {7};
    int16_t cb1[256], cbb1[256], cb2[256], cbb2[256], cb3[256];
    uint32_t o[16];
    memset(lmb, 0, sizeof lmb);
    for (unsigned i = 0; i < 256; i++) {
        cb1[i] = (int16_t)(-64 * (int)i);
        cb2[i] = (int16_t)(-50 * (int)i);
        cb3[i] = (int16_t)(-10 * (int)i);
        cbb1[i] = cbb2[i] = 0;
    }
    cbb1[1] = -100; cbb1[2] = -200; cbb1[3] = 50; cbb2[1] = -300;
    lpos = 64 + 6 * 32;
    o[0] = lput(p1, 7); o[1] = lput(b1, 7); o[2] = lput(c1, sizeof c1); o[3] = lput(cb1, sizeof cb1);
    o[4] = lput(cbb1, sizeof cbb1); o[5] = lput(t2, sizeof t2); o[6] = lput(p2, 4); o[7] = lput(b2, 4);
    o[8] = lput(c2, sizeof c2); o[9] = lput(cb2, sizeof cb2); o[10] = lput(cbb2, sizeof cbb2); o[11] = lput(t3, sizeof t3);
    o[12] = lput(p3, 1); o[13] = lput(cb3, sizeof cb3); o[14] = lput(cps, sizeof cps);
    lrow(1, 7, 0, o[0], o[1], o[2], o[3], o[4]);
    lrow(2, 4, o[5], o[6], o[7], o[8], o[9], o[10]);
    lrow(3, 1, o[11], o[12], 0, 0, o[13], 0);
    memcpy(lmb, "VOLM", 4);
    wr32(lmb + 4, 1); wr32(lmb + 8, (uint32_t)lpos); wr32(lmb + 12, 3); wr32(lmb + 16, 7); wr32(lmb + 28, o[14]);
    wr32(lmb + 24, crc32_ref(lmb + 64, lpos - 64));
    return lpos;
}

static char model_dir[256]; /* the product joins it and a name in 256 bytes (core/vocr_ocr.c join_path) */

static void write_file(const char *name, const void *data, size_t n, unsigned char sha[32])
{
    char path[300];
    br_sha256_context h;
    FILE *f;
    snprintf(path, sizeof path, "%s/%s", model_dir, name);
    f = fopen(path, "wb");
    TEST_ASSERT(f != NULL);
    TEST_ASSERT(fwrite(data, 1, n, f) == n);
    fclose(f);
    br_sha256_init(&h);
    br_sha256_update(&h, data, n);
    br_sha256_out(&h, sha);
}
#endif
