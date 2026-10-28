/* The native hooks' stream slots (core/hook_stream.c): new text, identical copies, busy slots, the
 * CALL source's caller claim, and game threads writing while the job thread reads. */
#define _POSIX_C_SOURCE 200809L /* pthread barriers, clock_gettime */
#include "acutest.h"

#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <time.h>

#include "../../core/fbgrey.h" /* vjo_fnv1a: the slots' hash */
#include "../../core/hook_stream.h"

static void test_offer_identical_new(void)
{
    static VjoStreamSlot s;
    static VjoStreamSnap out;
    VjoStreamClock c = {0, 0, 0, 0};
    memset(&s, 0, sizeof(s));
    TEST_CHECK(vjo_stream_read(&s, 1u, 0u, &out) == 0); /* no text yet */
    TEST_CHECK(vjo_stream_offer(&s, &c, (const uint8_t *)"abcd", 4, 3) == 1);
    TEST_CHECK(s.seq == 1 && s.hits == 1 && s.n == 4 && s.encoding == 3);
    TEST_CHECK(s.hash == vjo_fnv1a(VJO_FNV_INIT, (const uint8_t *)"abcd", 4));
    TEST_CHECK(vjo_stream_offer(&s, &c, (const uint8_t *)"abcd", 4, 3) == 0);
    TEST_CHECK(s.seq == 1 && s.hits == 2);
    TEST_CHECK(vjo_stream_offer(&s, &c, (const uint8_t *)"abcd", 4, 1) == 1); /* another encoding is new */
    TEST_CHECK(s.seq == 2 && s.hits == 3);
    TEST_CHECK(vjo_stream_offer(&s, &c, (const uint8_t *)"abcde", 5, 1) == 1);
    TEST_CHECK(s.seq == 3 && c.clock == 3 && c.dropped == 0);
    TEST_CHECK(vjo_stream_read(&s, 1u, 0u, &out) == 1);
    TEST_CHECK(out.seq == 3 && out.hits == 4 && out.n == 5 && !memcmp(out.raw, "abcde", 5) && out.encoding == 1);
    TEST_CHECK(s.lock == 0);
}

static void test_bounds_busy(void)
{
    static VjoStreamSlot s;
    static VjoStreamSnap out;
    static uint8_t big[VJO_STREAM_RAW + 1];
    VjoStreamClock c = {0, 0, 0, 0};
    memset(&s, 0, sizeof(s));
    memset(big, 'x', sizeof(big));
    TEST_CHECK(vjo_stream_offer(&s, &c, big, 0, 1) == -1);
    TEST_CHECK(vjo_stream_offer(&s, &c, big, VJO_STREAM_RAW + 1, 1) == -1);
    TEST_CHECK(c.dropped == 2 && s.seq == 0 && s.hits == 0);
    TEST_CHECK(vjo_stream_offer(&s, &c, big, VJO_STREAM_RAW, 1) == 1 && s.n == VJO_STREAM_RAW);
    TEST_CHECK(vjo_stream_try_lock(&s) == 1); /* the job holds it: a game thread's hit is skipped */
    TEST_CHECK(vjo_stream_try_lock(&s) == 0);
    TEST_CHECK(vjo_stream_offer(&s, &c, (const uint8_t *)"zzzz", 4, 1) == -1 && c.dropped == 3);
    TEST_CHECK(vjo_stream_read(&s, 1u, 0u, &out) == 0); /* and the job skips a slot a game thread holds */
    vjo_stream_unlock(&s);
    TEST_CHECK(vjo_stream_read(&s, 1u, 0u, &out) == 1 && out.n == VJO_STREAM_RAW);
}

/* Clock 0 is "never": a wrapped clock skips it. */
static void test_clock_wrap(void)
{
    static VjoStreamSlot s;
    VjoStreamClock c = {0xFFFFFFFFu, 0, 0, 0};
    memset(&s, 0, sizeof(s));
    TEST_CHECK(vjo_stream_offer(&s, &c, (const uint8_t *)"abcd", 4, 1) == 1);
    TEST_CHECK(s.seq == 1 && c.clock == 1);
}

static void test_caller_claim(void)
{
    static VjoStreamSlot slots[3];
    memset(slots, 0, sizeof(slots));
    TEST_CHECK(vjo_stream_caller(slots, 3, 0) == NULL);
    TEST_CHECK(vjo_stream_caller(slots, 3, 0x5910Fu) == &slots[0]);
    TEST_CHECK(vjo_stream_caller(slots, 3, 0x1234u) == &slots[1]);
    TEST_CHECK(vjo_stream_caller(slots, 3, 0x5910Fu) == &slots[0]); /* a stream keeps its slot */
    TEST_CHECK(vjo_stream_caller(slots, 3, 0x9999u) == &slots[2]);
    TEST_CHECK(vjo_stream_caller(slots, 3, 0x7777u) == NULL);      /* all held by other callers */
    TEST_CHECK(vjo_stream_caller(slots, 3, 0x1234u) == &slots[1]);
    TEST_CHECK(slots[0].key == 0x5910Fu && slots[1].key == 0x1234u && slots[2].key == 0x9999u);
}

static int offer_from(VjoStreamSlot *slots, unsigned count, VjoStreamClock *c, uint32_t key, const char *text)
{
    return vjo_stream_offer_caller(slots, count, c, key, (const uint8_t *)text, (unsigned)strlen(text), 0);
}

/* audit-hooks F2: callers claim free slots; with every slot held, a new caller takes the least recently
 * written one that is not kept, under its try-lock (busy: dropped, never waiting), and that slot's key and
 * text become the new caller's; a kept slot is never taken; the counts. */
static void test_caller_replacement(void)
{
    static VjoStreamSlot slots[3];
    static VjoStreamSnap out;
    VjoStreamClock c = {0, 0, 0, 0};
    memset(slots, 0, sizeof(slots));
    TEST_CHECK(offer_from(slots, 3, &c, 0, "abcd") == -1 && c.dropped == 1 && !c.call_dropped); /* no key */
    TEST_CHECK(vjo_stream_offer_caller(slots, 3, &c, 0xA0u, (const uint8_t *)"abcd", 0, 0) == -1);
    TEST_CHECK(offer_from(slots, 3, &c, 0xA0u, "aaaa") == 1 && slots[0].key == 0xA0u && slots[0].seq == 1);
    TEST_CHECK(offer_from(slots, 3, &c, 0xB0u, "bbbb") == 1 && slots[1].key == 0xB0u && slots[1].seq == 2);
    TEST_CHECK(offer_from(slots, 3, &c, 0xC0u, "cccc") == 1 && slots[2].key == 0xC0u && slots[2].seq == 3);
    TEST_CHECK(offer_from(slots, 3, &c, 0xA0u, "aaa2") == 1 && slots[0].seq == 4); /* A wrote last: B is oldest */
    TEST_CHECK(offer_from(slots, 3, &c, 0xA0u, "aaa2") == 0 && slots[0].hits == 3);  /* identical: a hit only */
    TEST_CHECK(!c.call_dropped && !c.call_replaced);
    /* D: every slot is held, B's (seq 2) is the least recently written */
    TEST_CHECK(offer_from(slots, 3, &c, 0xD0u, "dddd") == 1 && c.call_replaced == 1);
    TEST_CHECK(slots[1].key == 0xD0u && slots[1].seq == 5 && slots[1].hits == 1 && !memcmp(slots[1].raw, "dddd", 4));
    TEST_CHECK(vjo_stream_read(&slots[1], 1u, 0u, &out) == 1 && out.key == 0xD0u && !memcmp(out.raw, "dddd", 4));
    TEST_CHECK(slots[1].lock == 0);
    /* D keeps writing into its slot; B, whose slot it was, now needs one: C's (seq 3) is the oldest */
    TEST_CHECK(offer_from(slots, 3, &c, 0xD0u, "ddd2") == 1 && slots[1].seq == 6 && c.call_replaced == 1);
    TEST_CHECK(offer_from(slots, 3, &c, 0xB0u, "bbb2") == 1 && slots[2].key == 0xB0u && c.call_replaced == 2);
    /* keep A's and D's: the oldest that is not kept is B's (the only one), whatever the others' age */
    vjo_stream_keep(&slots[0]);
    vjo_stream_keep(&slots[1]);
    TEST_CHECK(offer_from(slots, 3, &c, 0xE0u, "eeee") == 1 && slots[2].key == 0xE0u && c.call_replaced == 3);
    TEST_CHECK(slots[0].key == 0xA0u && slots[1].key == 0xD0u);
    /* E's slot busy (the job reads it): F is dropped, not waiting, and nothing changes */
    TEST_CHECK(vjo_stream_try_lock(&slots[2]) == 1);
    TEST_CHECK(offer_from(slots, 3, &c, 0xF0u, "ffff") == -1 && c.call_dropped == 1 && c.dropped == 3);
    TEST_CHECK(slots[2].key == 0xE0u && !memcmp(slots[2].raw, "eeee", 4));
    vjo_stream_unlock(&slots[2]);
    /* every slot kept: a new caller is dropped */
    vjo_stream_keep(&slots[2]);
    TEST_CHECK(offer_from(slots, 3, &c, 0xF0u, "ffff") == -1 && c.call_dropped == 2 && c.call_replaced == 3);
    TEST_CHECK(slots[0].key == 0xA0u && slots[1].key == 0xD0u && slots[2].key == 0xE0u);
    /* the kept callers still write their own slots */
    TEST_CHECK(offer_from(slots, 3, &c, 0xE0u, "eee2") == 1 && !memcmp(slots[2].raw, "eee2", 4));
    TEST_CHECK(c.call_dropped == 2 && c.dropped == 4);
}

/* A slot that never had text is the least recently written; the age counts across the clock's wrap. */
static void test_replacement_age(void)
{
    static VjoStreamSlot slots[3];
    VjoStreamClock c = {0xFFFFFFF0u, 0, 0, 0};
    memset(slots, 0, sizeof(slots));
    TEST_CHECK(offer_from(slots, 3, &c, 0xA0u, "aaaa") == 1 && slots[0].seq == 0xFFFFFFF1u);
    TEST_CHECK(offer_from(slots, 3, &c, 0xB0u, "bbbb") == 1);
    slots[2].key = 0xC0u; /* claimed, its first text dropped: seq 0 */
    TEST_CHECK(offer_from(slots, 3, &c, 0xD0u, "dddd") == 1 && slots[2].key == 0xD0u);
    c.clock = 0xFFFFFFFFu;
    TEST_CHECK(offer_from(slots, 3, &c, 0xB0u, "bbb2") == 1 && slots[1].seq == 1); /* B's seq wrapped past 0 */
    /* A (0xFFFFFFF1) is older than B (1) and D (0xFFFFFFF3) across the wrap */
    TEST_CHECK(offer_from(slots, 3, &c, 0xE0u, "eeee") == 1 && slots[0].key == 0xE0u && c.call_replaced == 2);
}

/* A new caller whose first text is the very one its slot held (another caller's copy of the same bytes) still stores
 * it as new: its stream's first text, with a new seq and its own hits (#366: no longer by storing seq 0 first). */
static void test_replacement_same_text(void)
{
    static VjoStreamSlot slots[1];
    VjoStreamClock c = {0, 0, 0, 0};
    memset(slots, 0, sizeof(slots));
    TEST_CHECK(offer_from(slots, 1, &c, 0xA0u, "same") == 1 && offer_from(slots, 1, &c, 0xA0u, "same") == 0);
    TEST_CHECK(slots[0].seq == 1 && slots[0].hits == 2);
    TEST_CHECK(offer_from(slots, 1, &c, 0xB0u, "same") == 1);
    TEST_CHECK(slots[0].key == 0xB0u && slots[0].seq == 2 && slots[0].hits == 1 && c.call_replaced == 1);
    TEST_CHECK(offer_from(slots, 1, &c, 0xB0u, "same") == 0 && slots[0].seq == 2 && slots[0].hits == 2);
}

/* #366: the least recently written slot is the one with the earliest seq, whatever the clock read as the new
 * caller's scan began. A caller that stores while the scan runs gets a seq after that reading: main read the clock
 * once, before the scan, so that slot's age (clock - seq) wrapped to about 2^32 and the newest stream lost its slot.
 * The state such a scan saw, built by hand as the issue built it: B stored 1, 2 and 1,000 times after the clock was
 * read, and once across the clock's wrap; then two controls, B's last text before the reading, and A's text 2^31 - 1
 * new texts before B's (the serial order's range, which the job's comparisons assume as well). Every time, C takes
 * A's slot (seq 1, the least recently written), and B keeps its own. Single-threaded: the same result on every run
 * (host/tests/test_hook_stream_seams.c runs such stores at the scan's own points). */
static void test_replacement_newer_than_clock(void)
{
    static const struct {
        uint32_t clock, a, b;
    } st[] = {
        {10, 1, 11}, {10, 1, 12}, {10, 1, 1010},
        {0xFFFFFFFFu, 0xFFFFFFF0u, 1}, /* B's seq 1 follows 0xFFFFFFFF (0 is skipped) */
        {10, 1, 10},                   /* the control */
        {0x80000000u, 1, 0x80000000u}, /* and A 2^31 - 1 new texts before B: the serial order's range */
    };
    static VjoStreamSlot slots[2];
    for (unsigned k = 0; k < sizeof(st) / sizeof(st[0]); k++) {
        VjoStreamClock c = {0, 0, 0, 0};
        memset(slots, 0, sizeof(slots));
        TEST_CHECK(offer_from(slots, 2, &c, 0xA0u, "aaaa") == 1 && offer_from(slots, 2, &c, 0xB0u, "bbbb") == 1);
        slots[0].seq = st[k].a;
        slots[1].seq = st[k].b;
        c.clock = st[k].clock;
        TEST_CHECK(offer_from(slots, 2, &c, 0xC0u, "cccc") == 1);
        TEST_CHECK_(slots[0].key == 0xC0u && !memcmp(slots[0].raw, "cccc", 4) && slots[1].key == 0xB0u &&
                        slots[1].seq == st[k].b && !memcmp(slots[1].raw, "bbbb", 4) && c.call_replaced == 1,
                    "clock %#x, A seq %#x, B seq %#x: C takes A's slot (slot 0 key %#x, slot 1 key %#x)",
                    (unsigned)st[k].clock, (unsigned)st[k].a, (unsigned)st[k].b, (unsigned)slots[0].key,
                    (unsigned)slots[1].key);
    }
}

/* The two races below (#344): game threads and the job (the test's own thread) start together, at a
 * barrier, and the game threads go on, RACE_ROUNDS hits each at least, until the job has seen what the
 * race's checks need (the race is covered). With a fixed count of rounds a check could pass having tested
 * nothing (the job's tries all made before a game thread began) or fail with nothing wrong: offers never
 * wait, so while one thread held the oldest slot's lock off the CPU, every other caller's rounds could all
 * be dropped, and no slot changed hands. Past its RACE_ROUNDS a game thread yields before each hit (the job
 * every 1024 passes), so a lock holder off the CPU soon gets back on it, even on one CPU. A race ends
 * uncovered only once RACE_DEADLINE_MS have passed, every game thread has made RACE_YIELDS yielding hits,
 * and the job has then read on for RACE_YIELDS passes: a slow or starved run only takes longer. With a
 * working lock, a race stays uncovered only if a lock holder is kept off the CPU all that while; a lost
 * unlock, or slots that never change hands, leave it uncovered. A failed check prints the race's counters. */
#define RACE_ROUNDS 200000     /* the more hits, the likelier a race in the code is caught (#365's audit) */
#define RACE_YIELDS 20000      /* yielding hits of every game thread, then job passes, before it may end uncovered */
#define RACE_DEADLINE_MS 10000 /* and not before this */
static unsigned race_stop, race_done, race_far; /* atomics: the job ended the race; the game threads that have
                                                   stopped; those that have made RACE_YIELDS yielding hits */
static unsigned race_wait;            /* the job's passes since the race could end uncovered */
static unsigned race_covered;         /* the race ended covered */
static long race_ended, race_elapsed; /* ms from the barrier: to the race's end, and to the last thread's */
static struct timespec race_t0;

static long race_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long)(now.tv_sec - race_t0.tv_sec) * 1000 + (now.tv_nsec - race_t0.tv_nsec) / 1000000;
}

/* n game threads running fn, and the job, start together at b. One barrier per race: if a thread cannot be
 * made, those already waiting at it stay out of the next race. */
static void race_start(pthread_barrier_t *b, pthread_t *t, unsigned n, void *(*fn)(void *))
{
    race_stop = race_done = race_far = race_wait = race_covered = 0;
    race_ended = race_elapsed = 0;
    TEST_ASSERT(pthread_barrier_init(b, NULL, n + 1) == 0);
    for (unsigned w = 0; w < n; w++) TEST_ASSERT(pthread_create(&t[w], NULL, fn, (void *)(uintptr_t)w) == 0);
    pthread_barrier_wait(b);
    clock_gettime(CLOCK_MONOTONIC, &race_t0);
}

/* A game thread's, before its hit i: 1 to make it (its first RACE_ROUNDS, then, each after a yield, until
 * the job ends the race). */
static int race_on(unsigned i)
{
    if (i < RACE_ROUNDS) return 1;
    if (__atomic_load_n(&race_stop, __ATOMIC_ACQUIRE)) return 0;
    if (i == RACE_ROUNDS + RACE_YIELDS) __atomic_add_fetch(&race_far, 1u, __ATOMIC_RELEASE);
    sched_yield();
    return 1;
}

static void race_end(int covered)
{
    race_covered = covered != 0;
    race_ended = race_ms();
    __atomic_store_n(&race_stop, 1u, __ATOMIC_RELEASE);
}

/* The job's, after each pass of its reads: 1 to read on. The race ends once it is covered; uncovered, it
 * ends RACE_YIELDS passes after the job found the deadline past and every game thread far (it looks every
 * 1024 passes, when it yields). The job then reads on until every game thread has stopped. */
static int race_job(unsigned pass, int covered, unsigned n)
{
    if (__atomic_load_n(&race_stop, __ATOMIC_RELAXED)) {
        if (__atomic_load_n(&race_done, __ATOMIC_ACQUIRE) == n) return 0;
    } else if (covered) {
        race_end(1);
    } else if (race_wait) {
        if (++race_wait > RACE_YIELDS) race_end(0);
    } else if (pass % 1024 == 0 && __atomic_load_n(&race_far, __ATOMIC_ACQUIRE) == n &&
               race_ms() >= RACE_DEADLINE_MS) {
        race_wait = 1;
    }
    if (pass % 1024 == 0) sched_yield();
    return 1;
}

static void race_join(pthread_barrier_t *b, pthread_t *t, unsigned n)
{
    for (unsigned w = 0; w < n; w++) pthread_join(t[w], NULL);
    pthread_barrier_destroy(b);
    race_elapsed = race_ms();
}

/* A hit's result, as a column of a thread's counts: dropped, identical, stored; any other value is 4. */
static unsigned race_result(int r)
{
    return r >= -1 && r <= 1 ? (unsigned)(r + 1) : 4u;
}

/* A hit of a caller whose slot was given to another caller meanwhile (found by key, then replaced before
 * it took the lock) is dropped, never written into the new caller's stream: game threads racing for slots,
 * six callers for two. Each caller offers one text of its own, again and again, so a stored (new) text is
 * a caller taking a slot: one of the two first claims, or a replacement (call_replaced). A hit written into
 * another caller's stream stores a new text besides (and so does that caller's next hit), counted whether
 * or not the job reads the slot in time. The job also reads each slot's seq and length without its lock, as the
 * worker's hook watch does (game_ocr/hooks.c vjo_hooks_seq): once it has seen text there, a slot never reads as
 * empty (seq 0), which a replacement's plain `seq = 0` before its new text made it (#366). Covered: every caller has
 * had text stored (two slots to claim: at least four callers took theirs from another one) and the job has read
 * text. */
#define RACERS 6
static pthread_barrier_t race_barrier;
static VjoStreamSlot race_slots[2];
static VjoStreamClock race_clock;
static unsigned race_had_text;        /* atomic: a bit per caller whose text has been stored */
static unsigned race_hits[RACERS][5]; /* each caller's hits dropped, identical, stored, made, other results */
static unsigned race_reads, race_tries, race_wrong;
static VjoStreamSnap race_first_wrong; /* the first read with another caller's text */
static unsigned race_watch_had[2];     /* the job's reads without the lock: the slot had text (seq not 0) */
static unsigned race_watch_reads, race_watch_empty; /* those reads, and those of a slot that had text with seq 0 */

static void *racer(void *arg)
{
    unsigned w = (unsigned)(uintptr_t)arg;
    uint32_t key = 0x100u + w; /* one caller per thread: its text names it */
    uint8_t buf[8];
    unsigned i;
    memset(buf, (int)(0x41 + w), sizeof(buf));
    pthread_barrier_wait(&race_barrier);
    for (i = 0; race_on(i); i++) {
        int r = vjo_stream_offer_caller(race_slots, 2, &race_clock, key, buf, sizeof(buf), 0);
        if (r == 1 && !race_hits[w][2]) __atomic_fetch_or(&race_had_text, 1u << w, __ATOMIC_RELEASE);
        race_hits[w][race_result(r)]++;
    }
    race_hits[w][3] = i;
    __atomic_add_fetch(&race_done, 1u, __ATOMIC_RELEASE);
    return NULL;
}

/* The text is its key's caller's, whole. */
static int race_text_ok(const VjoStreamSnap *s)
{
    if (s->key < 0x100u || s->key >= 0x100u + RACERS || s->n != 8) return 0;
    for (unsigned b = 0; b < 8; b++)
        if (s->raw[b] != (uint8_t)(0x41 + (s->key - 0x100u))) return 0;
    return 1;
}

static void race_counters(void)
{
    unsigned stored = 0, dropped = 0;
    for (unsigned w = 0; w < RACERS; w++) {
        stored += race_hits[w][2];
        dropped += race_hits[w][0];
    }
    TEST_MSG("%s at %ld ms (the last thread stopped at %ld): call_replaced %u, call_dropped %u, dropped %u, "
             "clock %u", race_covered ? "covered" : "not covered", race_ended, race_elapsed,
             race_clock.call_replaced, race_clock.call_dropped, race_clock.dropped, race_clock.clock);
    TEST_MSG("the callers' hits: %u stored, %u dropped", stored, dropped);
    for (unsigned k = 0; k < 2; k++)
        TEST_MSG("slot %u: lock %u, key %#x, seq %u, hits %u", k, race_slots[k].lock, race_slots[k].key,
                 race_slots[k].seq, race_slots[k].hits);
    TEST_MSG("the job: %u reads of %u tries, %u with another caller's text", race_reads, race_tries, race_wrong);
    TEST_MSG("the job without the lock: %u reads, %u of a slot that had text with seq 0", race_watch_reads,
             race_watch_empty);
    if (race_wrong)
        TEST_MSG("the first: key %#x, %u bytes, %02x %02x .. %02x", race_first_wrong.key, race_first_wrong.n,
                 race_first_wrong.raw[0], race_first_wrong.raw[1], race_first_wrong.raw[7]);
    for (unsigned w = 0; w < RACERS; w++)
        TEST_MSG("caller %#x: %u hits: %u stored, %u identical, %u dropped, %u other", 0x100u + w,
                 race_hits[w][3], race_hits[w][2], race_hits[w][1], race_hits[w][0], race_hits[w][4]);
}

static void test_replacement_race(void)
{
    pthread_t t[RACERS];
    static VjoStreamSnap out;
    unsigned stored = 0, dropped = 0, other = 0, replaced, shown = 0;
    memset(race_slots, 0, sizeof(race_slots));
    memset(&race_clock, 0, sizeof(race_clock));
    memset(race_hits, 0, sizeof(race_hits));
    memset(&race_first_wrong, 0, sizeof(race_first_wrong));
    race_had_text = race_reads = race_tries = race_wrong = 0;
    race_watch_had[0] = race_watch_had[1] = race_watch_reads = race_watch_empty = 0;
    race_start(&race_barrier, t, RACERS, racer);
    for (unsigned pass = 1;; pass++) {
        int covered;
        for (unsigned k = 0; k < 2; k++) {
            /* vjo_hooks_seq's loads: the length (each text's is 8), then the seq */
            uint32_t n = __atomic_load_n(&race_slots[k].n, __ATOMIC_RELAXED);
            uint32_t seq = __atomic_load_n(&race_slots[k].seq, __ATOMIC_ACQUIRE);
            race_watch_reads++;
            if (!seq && race_watch_had[k]) race_watch_empty++;
            if (seq && n == 8) race_watch_had[k] = 1;
            race_tries++;
            if (!vjo_stream_read(&race_slots[k], 1u, 0u, &out)) continue;
            race_reads++;
            if (!race_text_ok(&out) && !race_wrong++) race_first_wrong = out;
        }
        covered = race_reads && __atomic_load_n(&race_had_text, __ATOMIC_ACQUIRE) == (1u << RACERS) - 1;
        if (!race_job(pass, covered, RACERS)) break;
    }
    race_join(&race_barrier, t, RACERS);
    for (unsigned w = 0; w < RACERS; w++) {
        dropped += race_hits[w][0];
        stored += race_hits[w][2];
        other += race_hits[w][4];
    }
    replaced = race_clock.call_replaced;
    /* the counters once, after the first failed check */
    if (!TEST_CHECK(race_covered) && !shown++) race_counters();
    if (!TEST_CHECK(race_wrong == 0) && !shown++) race_counters();
    if (!TEST_CHECK(race_slots[0].lock == 0 && race_slots[1].lock == 0) && !shown++) race_counters();
    if (!TEST_CHECK(replaced >= RACERS - 2) && !shown++) race_counters();
    /* every new text is a caller taking a slot: none was written into another caller's stream */
    if (!TEST_CHECK(replaced <= stored && stored <= replaced + 2) && !shown++) race_counters();
    if (!TEST_CHECK(race_clock.call_dropped <= race_clock.dropped) && !shown++) race_counters();
    if (!TEST_CHECK(stored == race_clock.clock && dropped == race_clock.dropped) && !shown++) race_counters();
    if (!TEST_CHECK(other == 0) && !shown++) race_counters();
    /* the hook watch never saw a slot that had text as empty (#366) */
    if (!TEST_CHECK(race_watch_empty == 0) && !shown++) race_counters();
}

/* Game threads offering whole texts (every byte one value, the length from it) into three slots, two
 * threads each, while the job thread reads them: a read is never torn, every hit is stored, identical or
 * dropped, and the slots' stores share one clock (as the worker's slots do, game_ocr/hooks.c) without losing
 * a tick. Covered: the job has read text from every slot. */
#define WRITERS 6 /* writer w writes slot w % 3 */
static pthread_barrier_t shared_barrier;
static VjoStreamSlot shared[3];
static VjoStreamClock shared_clock;
static unsigned results[WRITERS][5]; /* each writer's hits dropped, identical, stored, made, other results */
static unsigned shared_reads[3], shared_tries, shared_torn;

static void *writer(void *arg)
{
    unsigned w = (unsigned)(uintptr_t)arg;
    uint8_t buf[64];
    unsigned i;
    pthread_barrier_wait(&shared_barrier);
    for (i = 0; race_on(i); i++) {
        uint8_t v = (uint8_t)(1 + (w * 7 + i) % 50);
        unsigned n = 4u + v;
        memset(buf, v, n);
        results[w][race_result(vjo_stream_offer(&shared[w % 3], &shared_clock, buf, n, 1))]++;
    }
    results[w][3] = i;
    __atomic_add_fetch(&race_done, 1u, __ATOMIC_RELEASE);
    return NULL;
}

static void shared_counters(void)
{
    unsigned stored = 0, same = 0, dropped = 0;
    for (unsigned w = 0; w < WRITERS; w++) {
        dropped += results[w][0];
        same += results[w][1];
        stored += results[w][2];
    }
    TEST_MSG("%s at %ld ms (the last thread stopped at %ld): clock %u, dropped %u",
             race_covered ? "covered" : "not covered", race_ended, race_elapsed, shared_clock.clock,
             shared_clock.dropped);
    TEST_MSG("the writers' hits: %u stored, %u identical, %u dropped", stored, same, dropped);
    for (unsigned k = 0; k < 3; k++)
        TEST_MSG("slot %u: lock %u, seq %u, hits %u; the job's reads %u", k, shared[k].lock, shared[k].seq,
                 shared[k].hits, shared_reads[k]);
    TEST_MSG("the job: %u tries, %u torn", shared_tries, shared_torn);
    for (unsigned w = 0; w < WRITERS; w++)
        TEST_MSG("writer %u (slot %u): %u hits: %u stored, %u identical, %u dropped, %u other", w, w % 3,
                 results[w][3], results[w][2], results[w][1], results[w][0], results[w][4]);
}

/* The text is whole: every byte the first's, the length from it (the job's copy has no hash: it reads none; the
 * slot's own is checked under its lock, slot_hash_ok). */
static int shared_text_ok(const VjoStreamSnap *s)
{
    if (s->n < 5 || s->n > 54 || s->n != 4u + s->raw[0]) return 0;
    for (unsigned k = 1; k < s->n; k++)
        if (s->raw[k] != s->raw[0]) return 0;
    return 1;
}

/* #418's audit F2: under the slot's own try-lock, as store() keeps it, its hash is its text's (FNV-1a of raw[0..n)): the
 * identical-text test (store: s->hash == h) must never see a hash of another text. 1 ok, 0 torn, -1 busy. */
static int slot_hash_ok(VjoStreamSlot *s)
{
    int ok;
    if (!vjo_stream_try_lock(s)) return -1;
    ok = !s->seq || s->hash == vjo_fnv1a(VJO_FNV_INIT, s->raw, s->n);
    vjo_stream_unlock(s);
    return ok;
}

static void test_concurrent(void)
{
    pthread_t t[WRITERS];
    static VjoStreamSnap out;
    unsigned stored = 0, same = 0, dropped = 0, other = 0, shown = 0;
    memset(shared, 0, sizeof(shared));
    memset(&shared_clock, 0, sizeof(shared_clock));
    memset(results, 0, sizeof(results));
    shared_reads[0] = shared_reads[1] = shared_reads[2] = shared_tries = shared_torn = 0;
    race_start(&shared_barrier, t, WRITERS, writer);
    for (unsigned pass = 1; race_job(pass, shared_reads[0] && shared_reads[1] && shared_reads[2], WRITERS); pass++)
        for (unsigned k = 0; k < 3; k++) {
            shared_tries++;
            if (!slot_hash_ok(&shared[k])) shared_torn++; /* -1 (busy) and 1 are fine */
            if (!vjo_stream_read(&shared[k], 1u, 0u, &out)) continue;
            shared_reads[k]++;
            if (!shared_text_ok(&out)) shared_torn++;
        }
    race_join(&shared_barrier, t, WRITERS);
    for (unsigned k = 0; k < 3; k++) /* and once the writers are done */
        if (slot_hash_ok(&shared[k]) != 1) shared_torn++;
    for (unsigned w = 0; w < WRITERS; w++) {
        dropped += results[w][0];
        same += results[w][1];
        stored += results[w][2];
        other += results[w][4];
    }
    if (!TEST_CHECK(race_covered) && !shown++) shared_counters();
    if (!TEST_CHECK(shared_torn == 0) && !shown++) shared_counters();
    if (!TEST_CHECK(other == 0) && !shown++) shared_counters();
    if (!TEST_CHECK(dropped == shared_clock.dropped) && !shown++) shared_counters();
    if (!TEST_CHECK(stored == shared_clock.clock) && !shown++) shared_counters(); /* no tick lost */
    if (!TEST_CHECK(shared[0].hits + shared[1].hits + shared[2].hits == stored + same) && !shown++)
        shared_counters();
    if (!TEST_CHECK(shared[0].lock == 0 && shared[1].lock == 0 && shared[2].lock == 0) && !shown++)
        shared_counters();
}

TEST_LIST = {
    {"offer_identical_new", test_offer_identical_new},
    {"bounds_busy", test_bounds_busy},
    {"clock_wrap", test_clock_wrap},
    {"caller_claim", test_caller_claim},
    {"caller_replacement", test_caller_replacement},
    {"replacement_age", test_replacement_age},
    {"replacement_same_text", test_replacement_same_text},
    {"replacement_newer_than_clock", test_replacement_newer_than_clock},
    {"replacement_race", test_replacement_race},
    {"concurrent", test_concurrent},
    {NULL, NULL},
};
