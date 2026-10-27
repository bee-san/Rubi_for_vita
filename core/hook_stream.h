/* The native hooks' stream slots (docs/native-hooks.md): static, fixed-size, written by game threads
 * inside hooked functions and read by the worker's job thread. Pure C (GCC atomic builtins), no
 * allocation, no platform calls, no waiting: a text is stored and copied out under the slot's
 * try-lock, and a busy slot is skipped (a hit is then dropped and counted; a job reads it again at the
 * next job). The fields also used without the lock are atomics: key (a CALL caller's claim and search),
 * kept (vjo_stream_keep), seq and n (the replacement's scan, the worker's hook watch: #366). */
#ifndef VJO_HOOK_STREAM_H
#define VJO_HOOK_STREAM_H

#include <stddef.h>
#include <stdint.h>

#define VJO_STREAM_RAW 512u /* = VJO_HOOK_RAW_BYTES (game_ocr/hooks.h) */

typedef struct {
    uint32_t lock;     /* 0 free, 1 held (a game thread writing, or the job reading) */
    uint32_t key;      /* 0 unused; a site's segment-0 offset + 1, or a CALL caller's return address */
    uint32_t kind;     /* VJO_HOOK_KIND_* */
    uint32_t seq;      /* the streams' clock at this slot's last new text; 0: never had text (once it has, never 0) */
    uint32_t hits;     /* hits stored or identical, since the game started (since its caller took it) */
    uint32_t hash;     /* FNV-1a of raw[0..n) */
    uint32_t encoding; /* VJO_HOOK_ENC_* of raw */
    uint32_t n;        /* bytes in raw */
    uint32_t kept;     /* 1: a CALL slot no other caller may take (vjo_stream_keep) */
    const void *site;  /* a table site's VjoLunaSite, or NULL */
    uint8_t raw[VJO_STREAM_RAW];
} VjoStreamSlot;

typedef struct {
    uint32_t clock;         /* the last seq handed out */
    uint32_t dropped;       /* hits skipped: their slot was busy, or the CALL caller had no slot */
    uint32_t call_dropped;  /* of those, CALL hits whose caller found every slot held by others and took none */
    uint32_t call_replaced; /* CALL callers that took another caller's slot (every slot was held) */
} VjoStreamClock;

/* The slot's try-lock: 1 taken, 0 busy. Never waits. */
static inline int vjo_stream_try_lock(VjoStreamSlot *s)
{
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(&s->lock, &expected, 1u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static inline void vjo_stream_unlock(VjoStreamSlot *s) { __atomic_store_n(&s->lock, 0u, __ATOMIC_RELEASE); }

/* Hook time: n bytes at src (1..VJO_STREAM_RAW; the caller has bounded and prefiltered them) as the
 * slot's latest text. An identical text (same n, same hash) only counts a hit. 1 stored as new text,
 * 0 identical, -1 skipped (the slot is busy, or n out of range). */
int vjo_stream_offer(VjoStreamSlot *s, VjoStreamClock *c, const uint8_t *src, unsigned n, unsigned encoding);

/* Hook time, CALL source: the slot of caller key (nonzero) among n CALL slots: the one holding key,
 * else the first unused one, claimed for key. NULL when all n hold other callers. */
VjoStreamSlot *vjo_stream_caller(VjoStreamSlot *slots, unsigned n, uint32_t key);

/* Hook time, CALL source (audit-hooks F2): n bytes at src from caller key (nonzero) into its slot among
 * the count CALL slots, as vjo_stream_offer: the slot holding key, else the first unused one (claimed for
 * key), else, when every slot holds another caller, the least recently written one (the earliest seq in the
 * clock's serial order; one that never had text first) that is not kept: under its try-lock its key becomes
 * key, its hits and text key's (a new seq), and call_replaced counts it. Never waits: that slot busy, or every
 * slot kept, drops the hit (call_dropped). Under the slot's lock the choice is checked again: kept since, or
 * written since the scan read it (its caller's new text, another new caller that took it: no longer the least
 * recently written, #366), the hit is dropped too (call_dropped); and the key is checked again, so a hit of a
 * caller whose slot was just given to another one is dropped too (call_dropped), never written into the
 * other's stream, and a caller whose other thread took a slot meanwhile takes no second one.
 * 1 new text, 0 identical, -1 dropped. */
int vjo_stream_offer_caller(VjoStreamSlot *slots, unsigned count, VjoStreamClock *c, uint32_t key,
                            const uint8_t *src, unsigned n, unsigned encoding);

/* Job time: no other caller may take s from now on (the stream agreed with OCR, or the Shell pinned it). Call it
 * with s's try-lock held (game_ocr/hooks.c vjo_hooks_keep): a new caller taking s holds that lock, so a keep that
 * landed after its checks could let a thread of the same caller pass s over as kept and take a second slot (#366's
 * audit-b, F3). */
void vjo_stream_keep(VjoStreamSlot *s);

/* Job time: copies the slot under its try-lock. 1 copied; 0 empty (no text yet) or busy. */
int vjo_stream_read(VjoStreamSlot *s, VjoStreamSlot *out);

#endif
