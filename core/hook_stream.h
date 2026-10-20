/* The native hooks' stream slots (docs/native-hooks.md): static, fixed-size, written by game threads
 * inside hooked functions and read by the worker's job thread. Pure C (GCC atomic builtins), no
 * allocation, no platform calls, no waiting: every access takes the slot's try-lock, and a busy
 * slot is skipped (a hit is then dropped and counted; a job reads it again at the next job). */
#ifndef VJO_HOOK_STREAM_H
#define VJO_HOOK_STREAM_H

#include <stddef.h>
#include <stdint.h>

#define VJO_STREAM_RAW 512u /* = VJO_HOOK_RAW_BYTES (game_ocr/hooks.h) */

typedef struct {
    uint32_t lock;     /* 0 free, 1 held (a game thread writing, or the job reading) */
    uint32_t key;      /* 0 unused; a site's segment-0 offset + 1, or a CALL caller's return address */
    uint32_t kind;     /* VJO_HOOK_KIND_* */
    uint32_t seq;      /* the streams' clock at this slot's last new text; 0: never had text */
    uint32_t hits;     /* hits stored or identical, since the game started */
    uint32_t hash;     /* FNV-1a of raw[0..n) */
    uint32_t encoding; /* VJO_HOOK_ENC_* of raw */
    uint32_t n;        /* bytes in raw */
    const void *site;  /* a table site's VjoLunaSite, or NULL */
    uint8_t raw[VJO_STREAM_RAW];
} VjoStreamSlot;

typedef struct {
    uint32_t clock;   /* the last seq handed out */
    uint32_t dropped; /* hits skipped: their slot was busy, or no CALL slot was free */
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
 * else the first unused one, claimed for key for the rest of the game session (a stream's ID never
 * changes). NULL when all n hold other callers. */
VjoStreamSlot *vjo_stream_caller(VjoStreamSlot *slots, unsigned n, uint32_t key);

/* Job time: copies the slot under its try-lock. 1 copied; 0 empty (no text yet) or busy. */
int vjo_stream_read(VjoStreamSlot *s, VjoStreamSlot *out);

#endif
