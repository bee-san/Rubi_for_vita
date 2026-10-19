/* A test-side writer of Anki for Vita's duplicate index (AFVINDEX version 1, core/anki_index.h), written
 * from Anki for Vita's docs/duplicate-index.md for Rubi's tests: the index of notes that each hold one
 * plain word, a form and its reading ("" or NULL for a note without one). A note holding a plain word
 * gives Anki for Vita the keys the overlay computes for it (its normalisation of the word HTML-escaped in
 * a field is the overlay's of the plain word), so the keys here are vjo_anki_index_keys'. Not the app's
 * builder (that is Anki for Vita's): the buckets' cap, the counts and the check, enough to make every
 * file the reader may meet. test_anki_index.c checks it against the document's worked example. */
#ifndef VJO_TEST_AFV_INDEX_WRITER_H
#define VJO_TEST_AFV_INDEX_WRITER_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "anki_index.h"
#include "bearssl.h"

typedef struct {
    const char *form, *reading;
} AfvWord;

/* Rubi's default request's hash (Anki for Vita's docs/duplicate-index.md: {"v": 1, "notetypes": [],
 * "key": "Expression", "reading": "ExpressionReading"}). */
#define AFV_RUBI_REQUEST_HASH 0x050ce21040448110ull

static void afv_put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void afv_put64(uint8_t *p, uint64_t v)
{
    afv_put32(p, (uint32_t)v);
    afv_put32(p + 4, (uint32_t)(v >> 32));
}

static int afv_cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static uint32_t afv_bucket(uint64_t key, uint32_t bits)
{
    return bits ? (uint32_t)(key >> (64 - bits)) : 0;
}

/* The index of n notes into a new buffer (*size its bytes; free() it): source and built in the
 * header's times, the request's hash and the default names. */
static uint8_t *afv_write(const AfvWord *w, int n, uint64_t request_hash, uint64_t source_ms, uint64_t built_ms,
                          size_t *size)
{
    uint64_t *keys = malloc(sizeof(uint64_t) * (size_t)(n ? n : 1));
    uint32_t keyed = 0, kept = 0, bits = 0, largest = 0, over_bucket = 0, run = 0, cur = 0;
    uint8_t *out, *hdr;
    size_t at;
    if (!keys)
        return NULL;
    for (int i = 0; i < n; i++) {
        VjoAnkiIndexKeys k;
        if (vjo_anki_index_keys(w[i].form, w[i].reading, &k))
            continue;
        keys[keyed++] = k.has_pair ? k.pair : k.form;
    }
    while (bits < VJO_ANKI_INDEX_MAX_BITS && keyed > (16u << bits))
        bits++;
    qsort(keys, keyed, sizeof(keys[0]), afv_cmp);
    /* distinct keys, each bucket's 64 smallest (compacted in place) */
    for (uint32_t i = 0; i < keyed; i++) {
        uint32_t b = afv_bucket(keys[i], bits);
        if (i && keys[i] == keys[i - 1])
            continue;
        if (!kept || b != cur) {
            cur = b;
            run = 0;
        }
        if (run == VJO_ANKI_INDEX_MAX_BUCKET) {
            over_bucket++;
            continue;
        }
        keys[kept++] = keys[i];
        if (++run > largest)
            largest = run;
    }
    *size = VJO_ANKI_INDEX_HEADER + 4u * ((1u << bits) + 1) + 8u * kept;
    if (!(out = calloc(1, *size))) {
        free(keys);
        return NULL;
    }
    hdr = out;
    memcpy(hdr, "AFVINDEX", 8);
    afv_put32(hdr + 8, 1);
    afv_put32(hdr + 12, VJO_ANKI_INDEX_HEADER);
    afv_put32(hdr + 16, kept);
    afv_put32(hdr + 20, bits);
    afv_put32(hdr + 24, largest);
    afv_put32(hdr + 28, 1); /* every key considered */
    afv_put64(hdr + 32, source_ms);
    afv_put64(hdr + 40, built_ms);
    afv_put32(hdr + 48, (uint32_t)n);
    afv_put32(hdr + 52, keyed);
    afv_put32(hdr + 56, 0);
    afv_put32(hdr + 60, over_bucket);
    afv_put64(hdr + 64, request_hash);
    memcpy(hdr + 72, "Expression", 10);
    memcpy(hdr + 96, "ExpressionReading", 17);
    {
        br_sha256_context c;
        uint8_t d[32], zero = 0;
        br_sha256_init(&c);
        br_sha256_update(&c, &zero, 1);
        br_sha256_update(&c, hdr, 124);
        br_sha256_out(&c, d);
        afv_put32(hdr + 124, (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3]);
    }
    /* the directory, then the keys */
    at = VJO_ANKI_INDEX_HEADER;
    for (uint32_t b = 0, i = 0; b <= (1u << bits); b++) {
        while (i < kept && afv_bucket(keys[i], bits) < b)
            i++;
        afv_put32(out + at + 4 * b, i);
    }
    at += 4u * ((1u << bits) + 1);
    for (uint32_t i = 0; i < kept; i++)
        afv_put64(out + at + 8 * i, keys[i]);
    free(keys);
    return out;
}

#endif
