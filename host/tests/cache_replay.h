/* cache_replay.h - the line cache's own operations, replayed through its layout before #418.
 *
 * Before #418 (cleanup P4) each VjoVocrCacheLine held its greedy text in a buffer of its own, VJO_VOCR_CACHE_TEXT
 * bytes, the shown text's copy on every line that neither the LM nor the cascade changed; now an entry keeps it after
 * the shown text's NUL, only where it is another text (core/vocr_ocr.h). A suite that includes this file and calls
 * cache_replay_install() has every lookup and every entry its jobs make (core/vocr_ocr.c's VjoVocrCacheTrace, host
 * builds only) made again in a copy of the cache with the old layout and main b55c5f5's cache_find and cache_put, and
 * after each of them checks that the two hold the same: the same lines with the same texts, greedy texts, flags,
 * frames and lengths, the same least recently used, the same hits and misses. A cache a test changed between two
 * operations (emptied, an entry edited, another cache's copy) is taken as it is before the next one (synced).
 *
 * The one rule that differs is counted, not failed: a line whose greedy text is another, whose two texts each fit an
 * old buffer but do not fit one entry together, which the old layout kept and this one leaves out (left_out; the job
 * counts it in VjoVocrStats.cache_long). */
#ifndef CACHE_REPLAY_H
#define CACHE_REPLAY_H

#include <stdio.h>
#include <string.h>

#include "../../core/vocr_ocr.h"

typedef struct {
    uint64_t key;
    const VjoVocrModel *model;
    uint32_t used;
    uint16_t frames, len;
    uint16_t flags;
    uint16_t greedy_len;
    uint16_t need;
    uint16_t pad;
    char text[VJO_VOCR_CACHE_TEXT];
    char greedy[VJO_VOCR_CACHE_TEXT];
} OldCacheLine;
typedef struct {
    OldCacheLine line[VJO_VOCR_CACHE_LINES];
    uint32_t tick, hits, misses;
} OldCache;

/* main b55c5f5's core/vocr_ocr.c cache_find and cache_put, on the old layout. */
static OldCacheLine *old_cache_find(OldCache *c, const VjoVocrModel *m, uint64_t key)
{
    for (unsigned i = 0; i < VJO_VOCR_CACHE_LINES; i++)
        if (c->line[i].key == key && c->line[i].model == m) return &c->line[i];
    return NULL;
}
static void old_cache_put(OldCache *c, const VjoVocrModel *m, uint64_t key, const char *text, size_t len,
                          const char *greedy, size_t greedy_len, unsigned frames, uint16_t flags, size_t need)
{
    OldCacheLine *e = old_cache_find(c, m, key);
    if (len >= VJO_VOCR_CACHE_TEXT || greedy_len >= VJO_VOCR_CACHE_TEXT || need >= 0xFFFFu) return;
    if (!e) { /* an empty entry, else the least recently used */
        e = &c->line[0];
        for (unsigned i = 1; i < VJO_VOCR_CACHE_LINES; i++)
            if (!c->line[i].key || (e->key && c->line[i].used < e->used)) e = &c->line[i];
    }
    e->key = key;
    e->model = m;
    e->used = ++c->tick;
    e->frames = (uint16_t)(frames > 0xFFFFu ? 0xFFFFu : frames);
    e->len = (uint16_t)len;
    e->flags = flags;
    e->greedy_len = (uint16_t)greedy_len;
    e->need = (uint16_t)need;
    e->pad = 0;
    memcpy(e->text, text, len);
    e->text[len] = '\0';
    memcpy(e->greedy, greedy, greedy_len);
    e->greedy[greedy_len] = '\0';
}

static struct {
    const VjoVocrCache *cache; /* the cache the copy follows */
    OldCache old;
    unsigned lookups, hits, puts, synced, left_out, mismatches;
} replay;

/* The cache as the old layout held it: each entry's greedy text in its own buffer. */
static void replay_sync(const VjoVocrCache *c)
{
    memset(&replay.old, 0, sizeof(replay.old));
    replay.cache = c;
    replay.old.tick = c->tick;
    replay.old.hits = c->hits;
    replay.old.misses = c->misses;
    for (unsigned i = 0; i < VJO_VOCR_CACHE_LINES; i++) {
        const VjoVocrCacheLine *n = &c->line[i];
        OldCacheLine *o = &replay.old.line[i];
        o->key = n->key;
        o->model = n->model;
        o->used = n->used;
        if (!n->key) continue;
        o->frames = n->frames;
        o->len = n->len;
        o->flags = n->flags;
        o->greedy_len = n->greedy_len;
        o->need = n->need;
        memcpy(o->text, n->text, (size_t)n->len + 1u < sizeof(o->text) ? (size_t)n->len + 1u : sizeof(o->text));
        if ((size_t)n->greedy_at + n->greedy_len < sizeof(n->text))
            memcpy(o->greedy, n->text + n->greedy_at, (size_t)n->greedy_len + 1u);
    }
}

/* Whether c holds what the old layout's copy holds; what differs first in what (64 bytes). */
static int replay_same(const VjoVocrCache *c, char *what, size_t cap)
{
    if (c->tick != replay.old.tick || c->hits != replay.old.hits || c->misses != replay.old.misses) {
        snprintf(what, cap, "tick %u/%u hits %u/%u misses %u/%u", (unsigned)c->tick, (unsigned)replay.old.tick,
                 (unsigned)c->hits, (unsigned)replay.old.hits, (unsigned)c->misses, (unsigned)replay.old.misses);
        return 0;
    }
    for (unsigned i = 0; i < VJO_VOCR_CACHE_LINES; i++) {
        const VjoVocrCacheLine *n = &c->line[i];
        const OldCacheLine *o = &replay.old.line[i];
        if (n->key != o->key || n->model != o->model || n->used != o->used) {
            snprintf(what, cap, "entry %u: key, model or use", i);
            return 0;
        }
        if (!n->key) continue;
        if (n->frames != o->frames || n->len != o->len || n->flags != o->flags || n->greedy_len != o->greedy_len ||
            n->need != o->need || memcmp(n->text, o->text, (size_t)n->len + 1u) ||
            (size_t)n->greedy_at + n->greedy_len >= sizeof(n->text) ||
            memcmp(n->text + n->greedy_at, o->greedy, (size_t)n->greedy_len + 1u)) {
            snprintf(what, cap, "entry %u: its texts or figures", i);
            return 0;
        }
    }
    return 1;
}

static void replay_trace(int after, const VjoVocrCache *c, const VjoVocrModel *m, uint64_t key, const char *text,
                         size_t len, const char *greedy, size_t greedy_len, unsigned frames, unsigned flags,
                         size_t need)
{
    char what[64];
    if (!after) {
        if (c != replay.cache || !replay_same(c, what, sizeof(what))) {
            replay_sync(c);
            replay.synced++;
        }
        if (!text) { /* a lookup: the job's line hits its entry, or misses */
            OldCacheLine *h = old_cache_find(&replay.old, m, key);
            replay.lookups++;
            if (h) {
                h->used = ++replay.old.tick;
                replay.old.hits++;
                replay.hits++;
            } else {
                replay.old.misses++;
            }
        } else { /* an entry offered once the job succeeded */
            replay.puts++;
            old_cache_put(&replay.old, m, key, text, len, greedy, greedy_len, frames, (uint16_t)flags, need);
        }
        return;
    }
    if (replay_same(c, what, sizeof(what))) return;
    if (text && greedy != text && len < VJO_VOCR_CACHE_TEXT && greedy_len < VJO_VOCR_CACHE_TEXT &&
        len + 1u + greedy_len >= VJO_VOCR_CACHE_TEXT && need < 0xFFFFu) {
        replay.left_out++; /* the one rule that differs (above) */
    } else {
        replay.mismatches++;
        TEST_CHECK_(0, "the line cache after a %s differs from the old layout's: %s", text ? "new entry" : "lookup",
                    what);
    }
    replay_sync(c);
}

/* From now on every cache operation of the suite's jobs is replayed and checked (counters from 0). */
static void cache_replay_install(void)
{
    memset(&replay, 0, sizeof(replay));
    vjo_vocr_cache_trace = replay_trace;
}

#endif
