/* The native hooks' stream slots (core/hook_stream.h). Pure C. */
#include "hook_stream.h"

#include "hook_place.h"

/* No libc in the hook path: a plain byte loop the compiler may not turn into a memcpy call (the
 * worker builds with -fno-builtin; the volatile store keeps it a loop on the host as well). */
static void copy_bytes(uint8_t *dst, const uint8_t *src, unsigned n)
{
    for (unsigned i = 0; i < n; i++) ((volatile uint8_t *)dst)[i] = src[i];
}

int vjo_stream_offer(VjoStreamSlot *s, VjoStreamClock *c, const uint8_t *src, unsigned n, unsigned encoding)
{
    uint32_t h;
    if (!n || n > VJO_STREAM_RAW || !vjo_stream_try_lock(s)) {
        __atomic_add_fetch(&c->dropped, 1u, __ATOMIC_RELAXED);
        return -1;
    }
    h = vjo_place_hash(src, n);
    s->hits++;
    if (s->seq && s->n == n && s->hash == h && s->encoding == encoding) {
        vjo_stream_unlock(s);
        return 0;
    }
    copy_bytes(s->raw, src, n);
    s->n = n;
    s->hash = h;
    s->encoding = encoding;
    s->seq = __atomic_add_fetch(&c->clock, 1u, __ATOMIC_RELAXED);
    if (!s->seq) s->seq = __atomic_add_fetch(&c->clock, 1u, __ATOMIC_RELAXED); /* 0 means "never" */
    vjo_stream_unlock(s);
    return 1;
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

int vjo_stream_read(VjoStreamSlot *s, VjoStreamSlot *out)
{
    if (!vjo_stream_try_lock(s)) return 0;
    if (!s->seq || !s->n || s->n > VJO_STREAM_RAW) {
        vjo_stream_unlock(s);
        return 0;
    }
    out->lock = 0;
    out->key = __atomic_load_n(&s->key, __ATOMIC_RELAXED);
    out->kind = s->kind;
    out->seq = s->seq;
    out->hits = s->hits;
    out->hash = s->hash;
    out->encoding = s->encoding;
    out->n = s->n;
    out->site = s->site;
    copy_bytes(out->raw, s->raw, s->n);
    vjo_stream_unlock(s);
    return 1;
}
