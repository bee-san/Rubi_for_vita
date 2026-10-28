/* The native hooks' stream slots (core/hook_stream.h). Pure C. */
#include "hook_stream.h"

#include <string.h> /* memcpy, job time only: the worker's libc_shim, sceClibMemcpy */

#include "fbgrey.h" /* vjo_fnv1a, the tree's one FNV-1a (#418) */

/* A test's seam (host/tests/test_hook_stream_seams.c): another thread's step at a point of replace() (scan: before slot
 * i's loads; chosen: the scan chose slot i, before its try-lock; taking: slot i's lock held and every check passed,
 * before the new caller's key; replacing: slot i is the new caller's, before its text), run on the test's own thread.
 * Nothing in the worker. */
#ifndef VJO_STREAM_SEAM
#define VJO_STREAM_SEAM(point, i) ((void)0)
#endif

/* No libc in the hook path: a plain byte loop the compiler may not turn into a memcpy call (the
 * worker builds with -fno-builtin; the volatile store keeps it a loop on the host as well). */
static void copy_bytes(uint8_t *dst, const uint8_t *src, unsigned n)
{
    for (unsigned i = 0; i < n; i++) ((volatile uint8_t *)dst)[i] = src[i];
}

/* n bytes at src as s's latest text; s's lock is held. fresh: a new caller's first text, new whatever s held. 1 new
 * text, 0 identical (a hit only). seq is also read without the lock by replace()'s scan and the worker's hook watch
 * (game_ocr/hooks.c vjo_hooks_seq), n by the watch, so both are stored atomically (#366), and seq once, never 0
 * ("never had text"), even as the clock wraps. seq is published with release order, last: a thread that reads it
 * (acquire) sees everything stored before it, the slot's key above all when a new caller took the slot (replace()'s
 * check for this caller's slot elsewhere). */
static int store(VjoStreamSlot *s, VjoStreamClock *c, const uint8_t *src, unsigned n, unsigned encoding, int fresh)
{
    uint32_t h = vjo_fnv1a(VJO_FNV_INIT, src, n), seq;
    s->hits++;
    if (!fresh && s->seq && s->n == n && s->hash == h && s->encoding == encoding) {
        return 0;
    }
    copy_bytes(s->raw, src, n);
    __atomic_store_n(&s->n, n, __ATOMIC_RELAXED);
    s->hash = h;
    s->encoding = encoding;
    seq = __atomic_add_fetch(&c->clock, 1u, __ATOMIC_RELAXED);
    if (!seq) seq = __atomic_add_fetch(&c->clock, 1u, __ATOMIC_RELAXED); /* 0 means "never" */
    __atomic_store_n(&s->seq, seq, __ATOMIC_RELEASE);
    return 1;
}

static int drop(VjoStreamClock *c, int no_slot)
{
    __atomic_add_fetch(&c->dropped, 1u, __ATOMIC_RELAXED);
    if (no_slot) __atomic_add_fetch(&c->call_dropped, 1u, __ATOMIC_RELAXED);
    return -1;
}

int vjo_stream_offer(VjoStreamSlot *s, VjoStreamClock *c, const uint8_t *src, unsigned n, unsigned encoding)
{
    int r;
    if (!n || n > VJO_STREAM_RAW || !vjo_stream_try_lock(s)) {
        return drop(c, 0);
    }
    r = store(s, c, src, n, encoding, 0);
    vjo_stream_unlock(s);
    return r;
}

VjoStreamSlot *vjo_stream_caller(VjoStreamSlot *slots, unsigned n, uint32_t key)
{
    if (!key) return NULL;
    for (unsigned i = 0; i < n; i++) {
        uint32_t k = __atomic_load_n(&slots[i].key, __ATOMIC_ACQUIRE);
        if (k == key) return &slots[i];
        if (!k) {
            uint32_t expected = 0;
            if (__atomic_compare_exchange_n(&slots[i].key, &expected, key, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                return &slots[i];
            if (expected == key) return &slots[i]; /* another thread claimed it for the same caller */
        }
    }
    return NULL;
}

/* Every slot holds another caller: key takes the least recently written one that is not kept (a slot that never had
 * text first, then the earliest seq), under its try-lock. The scan reads the slots' seqs without their locks while
 * game threads write them, and a slot's seq only moves on, so the choice is checked under the lock (#366): unchanged,
 * the slot is still the least recently written (every other seq is at or after what the scan read); written since the
 * scan read it (its caller's new text, or another new caller that took it), it is not, and the hit is dropped, as for
 * a busy slot. Seqs compare in the clock's serial order, as the job compares them (core/hook_select.c,
 * game_ocr/main.c: within 2^31 new texts of each other), and the clock is not read: a scan that read it first (before
 * #366) gave a slot written during the scan, whose seq came after that reading, an age of about 2^32, and the newest
 * stream lost its slot. A busy oldest slot drops the hit rather than giving the next oldest away: two threads of one
 * caller that chose the same slot contend for its lock, and one that chose another slot finds the first one's key
 * under its own lock (below). Passing busy slots over gave callers with several threads two slots, one keeping a
 * stale text (#366's audit, F2: 223 to 3,880 times in 20 runs of the host's hammer test, against none); the cost of
 * not doing so is a new caller's wait while the oldest slot stays busy and the other callers never pause. The scan
 * reads each seq with acquire order (store()'s release): a slot whose new seq it sees, it sees with the key stored
 * before it, so the other thread's slot is found below on a CPU that reorders memory accesses (the A9) too. */
static int replace(VjoStreamSlot *slots, unsigned count, VjoStreamClock *c, uint32_t key, const uint8_t *src,
                   unsigned n, unsigned encoding)
{
    VjoStreamSlot *v = NULL;
    uint32_t vseq = 0;
    int r;
    for (unsigned i = 0; i < count; i++) {
        uint32_t seq;
        VJO_STREAM_SEAM(scan, i);
        if (__atomic_load_n(&slots[i].kept, __ATOMIC_ACQUIRE)) continue;
        seq = __atomic_load_n(&slots[i].seq, __ATOMIC_ACQUIRE);
        if (!v || (vseq && (!seq || (int32_t)(seq - vseq) < 0))) {
            v = &slots[i];
            vseq = seq;
        }
    }
    if (!v) return drop(c, 1); /* every slot kept */
    VJO_STREAM_SEAM(chosen, (unsigned)(v - slots));
    if (!vjo_stream_try_lock(v)) return drop(c, 1); /* the oldest busy: no wait */
    if (__atomic_load_n(&v->kept, __ATOMIC_ACQUIRE)) { /* kept since it was chosen */
        vjo_stream_unlock(v);
        return drop(c, 1);
    }
    for (unsigned i = 0; i < count; i++) /* another thread of this caller took a slot meanwhile: one is enough */
        if (&slots[i] != v && __atomic_load_n(&slots[i].key, __ATOMIC_ACQUIRE) == key) {
            vjo_stream_unlock(v);
            return drop(c, 1);
        }
    if (__atomic_load_n(&v->key, __ATOMIC_RELAXED) == key) { /* this caller's other thread took it first: its own */
        r = store(v, c, src, n, encoding, 0);
    } else if (v->seq != vseq) { /* written since the scan read it: no longer the least recently written */
        vjo_stream_unlock(v);
        return drop(c, 1);
    } else {
        VJO_STREAM_SEAM(taking, (unsigned)(v - slots)); /* every check passed, the slot's lock held, the old key */
        __atomic_store_n(&v->key, key, __ATOMIC_RELEASE);
        v->hits = 0; /* a new caller's stream: its text is new (fresh), its hits its own */
        __atomic_add_fetch(&c->call_replaced, 1u, __ATOMIC_RELAXED);
        VJO_STREAM_SEAM(replacing, (unsigned)(v - slots)); /* the old text's seq until store() gives the new one */
        r = store(v, c, src, n, encoding, 1);
    }
    vjo_stream_unlock(v);
    return r;
}

int vjo_stream_offer_caller(VjoStreamSlot *slots, unsigned count, VjoStreamClock *c, uint32_t key,
                            const uint8_t *src, unsigned n, unsigned encoding)
{
    VjoStreamSlot *s;
    int r;
    if (!key || !n || n > VJO_STREAM_RAW) return drop(c, 0);
    s = vjo_stream_caller(slots, count, key);
    if (!s) return replace(slots, count, c, key, src, n, encoding);
    if (!vjo_stream_try_lock(s)) return drop(c, 0);
    if (__atomic_load_n(&s->key, __ATOMIC_RELAXED) != key) { /* given to another caller since it was found */
        vjo_stream_unlock(s);
        return drop(c, 1);
    }
    r = store(s, c, src, n, encoding, 0);
    vjo_stream_unlock(s);
    return r;
}

void vjo_stream_keep(VjoStreamSlot *s)
{
    __atomic_store_n(&s->kept, 1u, __ATOMIC_RELEASE);
}

int vjo_stream_read(VjoStreamSlot *s, uint32_t id, uint32_t call_base, VjoStreamSnap *out)
{
    uint32_t key;
    if (!vjo_stream_try_lock(s)) return 0;
    if (!s->seq || !s->n || s->n > VJO_STREAM_RAW) {
        vjo_stream_unlock(s);
        return 0;
    }
    key = __atomic_load_n(&s->key, __ATOMIC_RELAXED);
    out->id = id;
    out->kind = s->kind;
    out->seq = s->seq;
    out->hits = s->hits;
    out->key = s->kind == VJO_STREAM_KIND_SITE ? key - 1u : key - call_base;
    out->encoding = s->encoding;
    out->n = s->n;
    out->site = s->site;
    memcpy(out->raw, s->raw, s->n);
    vjo_stream_unlock(s);
    return 1;
}
