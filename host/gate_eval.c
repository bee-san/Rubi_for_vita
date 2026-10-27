/* The text-presence gate on many regions, as the game worker's job runs it (docs/vita-vn-ocr.md, "Text-presence
 * gate"; tools/gate_eval.py): core/vocr_ocr.c's policy with Rubi's finder, the gate in boxes mode at its pinned
 * operating point, and the worker's defaults (Normal, the 1 MB LM, the cascade to Large within 2 s, PSP-only
 * refinement, a box's short last line, the counter re-decode, the head gate and the two-level head), one job a
 * region from stdin.
 *
 *   vjo-gate-eval MODEL_DIR GATE 0|1 < regions.bin > results.jsonl
 *     MODEL_DIR  where the pinned files are; a directory holding only VJO_VOCR_GATE_NAME stops each job after the
 *                gate (its recognizer cannot open): the finder and the gate alone, fast
 *     GATE       1: the gate on (ocr_text_gate = on); 0: off
 *     regions.bin: "VJGE" then per region: u32 width, u32 height, u32 flags (bit 0: a PSP game's, refined),
 *                width x height RGB bytes (made grey here as the kernel does, (77 R + 150 G + 29 B) >> 8).
 *
 * One JSON line per region: the job's rc, the finder's lines (before the gate), the lines it kept, the gate's
 * score, whether it decided, its time, and with the recognizer the text and CTC frames. Host execution. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform_posix.h"
#include "vocr_ocr.h"
#include "../include/vjo_game_ocr.h"
#include "../include/vjo_vocr_sizes.h"

typedef struct { const unsigned char *grey; unsigned w, h; } Region;

static int strip(void *ud, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags)
{
    const Region *r = ud;
    (void)flags;
    if (y > r->h || n > r->h - y) return VJO_E_SOURCE;
    for (unsigned i = 0; i < n; i++) memcpy(dst + (size_t)i * stride, r->grey + (size_t)(y + i) * r->w, r->w);
    return (int)n;
}

static void *line_mem;
static void *line_buffer(void *ud, size_t bytes)
{
    (void)ud;
    if (!line_mem) line_mem = malloc(VJO_VOCR_LINE_MAX_BYTES);
    return bytes <= VJO_VOCR_LINE_MAX_BYTES ? line_mem : NULL;
}

/* one block each for the weights, the LM and the cascade's model, kept between regions as the worker keeps them */
static void *mem[3];
static void *alloc_n(int k, size_t bytes)
{
    if (mem[k] || posix_memalign(&mem[k], 64, bytes)) return NULL;
    return mem[k];
}
static int release_n(int k) { free(mem[k]); mem[k] = NULL; return 0; }
static void *w_alloc(void *ud, size_t b) { (void)ud; return alloc_n(0, b); }
static int w_release(void *ud) { (void)ud; return release_n(0); }
static void *l_alloc(void *ud, size_t b) { (void)ud; return alloc_n(1, b); }
static int l_release(void *ud) { (void)ud; return release_n(1); }
static void *c_alloc(void *ud, size_t b) { (void)ud; return alloc_n(2, b); }
static int c_release(void *ud) { (void)ud; return release_n(2); }

static void json_text(const char *s)
{
    putchar('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

static int read_u32(uint32_t *v) { return fread(v, 4, 1, stdin) == 1; }

int main(int argc, char **argv)
{
    static VjoVocrGateResident gate_res;
    static char text[VJO_GAME_OCR_TEXT_BYTES], raw[VJO_GAME_OCR_TEXT_BYTES];
    const char *model = VJO_VOCR_TIER2_NAME, *cascade_name = VJO_VOCR_TIER3_NAME;
    char magic[4];
    if (argc != 3) {
        fprintf(stderr, "usage: vjo-gate-eval MODEL_DIR 0|1 < regions.bin\n");
        return 2;
    }
    int gate = atoi(argv[2]);
    if (fread(magic, 1, 4, stdin) != 4 || memcmp(magic, "VJGE", 4)) return 2;
    VjoPlatform platform;
    PosixPlatform pp = {0};
    posix_platform_init(&pp, &platform);
    VjoVocrResident res = {w_alloc, w_release, NULL, NULL, NULL, 0, 0, 0};
    VjoVocrLmResident lres = {l_alloc, l_release, NULL, NULL, NULL, 0, 0, 0};
    VjoVocrResident cres = {c_alloc, c_release, NULL, NULL, NULL, 0, 0, 0};
    const VjoVocrLm *lm = vjo_vocr_lm_tier(2);
    const VjoVocrModel *casc = vjo_vocr_model(cascade_name);
    for (unsigned index = 0;; index++) {
        uint32_t w, h, flags;
        if (!read_u32(&w)) break;
        if (!read_u32(&h) || !read_u32(&flags) || !w || !h || w > VJO_OCR_MAX_WIDTH || h > VJO_OCR_MAX_HEIGHT) return 2;
        unsigned char *grey = malloc((size_t)w * h), *rgb = malloc((size_t)w * h * 3u);
        if (!grey || !rgb || fread(rgb, 1, (size_t)w * h * 3u, stdin) != (size_t)w * h * 3u) return 2;
        for (size_t i = 0; i < (size_t)w * h; i++) /* the kernel's grey (core/fbgrey.h) */
            grey[i] = (unsigned char)((77u * rgb[3 * i] + 150u * rgb[3 * i + 1] + 29u * rgb[3 * i + 2]) >> 8);
        free(rgb);
        Region region = {grey, w, h};
        VjoGreySource src = {&region, w, h, strip, NULL, line_buffer};
        unsigned refine = flags & 1u ? VJO_VOCR_REFINE_ALL : 0u;
        /* the worker's workspace for this job (game_ocr/main.c): the job's, the refinement's, the LM and cascade
         * extras; the greedy text after them */
        size_t job = vjo_vocr_job_bytes(model, w, h) + (refine ? vjo_vocr_refine_bytes(w, h) : 0) +
                     vjo_vocr_lm_job_bytes(model) + vjo_vocr_cascade_job_bytes(cascade_name);
        size_t cap = vjo_vocr_capture_bytes(w, h);
        void *ws = NULL, *capture = malloc(cap ? cap : 1);
        if (!cap || !job || !capture || posix_memalign(&ws, 64, job)) {
            printf("{\"index\": %u, \"rc\": %d, \"unsupported\": 1}\n", index, VJO_E_OCR_REGION);
            free(grey); free(capture); free(ws);
            continue;
        }
        VjoVocrPolicy policy = {0};
        policy.lm = lm;
        policy.lm_res = &lres;
        policy.cascade = casc;
        policy.cascade_res = &cres;
        policy.cascade_us = 2000000u;
        policy.refine = refine;
        policy.short_lines = 1;
        policy.counter_tau = VJO_VOCR_COUNTER_TAU;
        policy.counter_open = VJO_VOCR_COUNTER_OPEN;
        policy.raw = raw;
        policy.raw_cap = sizeof(raw);
        policy.gate = gate;
        policy.gate_res = &gate_res;
        policy.head_gate = policy.two_level = 1; /* ocr_head_gate = on, the default */
        VjoVocrStats st;
        int rc = vjo_vocr_ocr_policy(&platform, argv[1], model, &src, &res, ws, job, capture, cap, &policy, text,
                                     sizeof(text), &st);
        unsigned found = st.gate_stopped ? st.gate_stopped : st.lines;
        printf("{\"index\": %u, \"rc\": %d, \"found\": %u, \"lines\": %u, \"gate_ran\": %u, \"gate_score\": %d, "
               "\"gate_stopped\": %u, \"gate_rc\": %d, \"gate_us\": %u, \"find_us\": %u, \"recognized\": %u, "
               "\"frames\": %u, \"recognize_us\": %u, \"text\": ",
               index, rc, found, st.lines, st.gate_ran, st.gate_score, st.gate_stopped, st.gate_rc,
               (unsigned)st.gate_us, (unsigned)st.find_us, st.recognized, st.frames, (unsigned)st.recognize_us);
        json_text(rc ? "" : text);
        printf("}\n");
        fflush(stdout);
        free(grey); free(capture); free(ws);
    }
    vjo_vocr_resident_drop(&res);
    vjo_vocr_resident_drop(&cres);
    vjo_vocr_lm_drop(&lres);
    free(line_mem);
    return 0;
}
