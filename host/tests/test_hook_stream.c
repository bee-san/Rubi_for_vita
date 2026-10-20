/* The native hooks' stream slots (core/hook_stream.c): new text, identical copies, busy slots, the
 * CALL source's caller claim, and game threads writing while the job thread reads. */
#include "acutest.h"

#include <pthread.h>
#include <string.h>

#include "../../core/hook_place.h"
#include "../../core/hook_stream.h"

static void test_offer_identical_new(void)
{
    static VjoStreamSlot s;
    static VjoStreamSlot out;
    VjoStreamClock c = {0, 0, 0, 0};
    memset(&s, 0, sizeof(s));
    TEST_CHECK(vjo_stream_read(&s, &out) == 0); /* no text yet */
    TEST_CHECK(vjo_stream_offer(&s, &c, (const uint8_t *)"abcd", 4, 3) == 1);
    TEST_CHECK(s.seq == 1 && s.hits == 1 && s.n == 4 && s.encoding == 3);
    TEST_CHECK(s.hash == vjo_place_hash((const uint8_t *)"abcd", 4));
    TEST_CHECK(vjo_stream_offer(&s, &c, (const uint8_t *)"abcd", 4, 3) == 0);
    TEST_CHECK(s.seq == 1 && s.hits == 2);
    TEST_CHECK(vjo_stream_offer(&s, &c, (const uint8_t *)"abcd", 4, 1) == 1); /* another encoding is new */
    TEST_CHECK(s.seq == 2 && s.hits == 3);
    TEST_CHECK(vjo_stream_offer(&s, &c, (const uint8_t *)"abcde", 5, 1) == 1);
    TEST_CHECK(s.seq == 3 && c.clock == 3 && c.dropped == 0);
    TEST_CHECK(vjo_stream_read(&s, &out) == 1);
    TEST_CHECK(out.seq == 3 && out.hits == 4 && out.n == 5 && !memcmp(out.raw, "abcde", 5) && out.lock == 0);
    TEST_CHECK(s.lock == 0);
}

static void test_bounds_busy(void)
{
    static VjoStreamSlot s;
    static VjoStreamSlot out;
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
    TEST_CHECK(vjo_stream_read(&s, &out) == 0); /* and the job skips a slot a game thread holds */
    vjo_stream_unlock(&s);
    TEST_CHECK(vjo_stream_read(&s, &out) == 1 && out.n == VJO_STREAM_RAW);
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
    static VjoStreamSlot slots[3], out;
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
    TEST_CHECK(vjo_stream_read(&slots[1], &out) == 1 && out.key == 0xD0u && !memcmp(out.raw, "dddd", 4));
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

/* A hit of a caller whose slot was given to another caller meanwhile (found by key, then replaced before
 * it took the lock) is dropped, never written into the new caller's stream: game threads racing for slots. */
#define RACERS 6
#define RACE_ROUNDS 20000
static VjoStreamSlot race_slots[2];
static VjoStreamClock race_clock;
static void *racer(void *arg)
{
    unsigned w = (unsigned)(uintptr_t)arg;
    uint8_t buf[8];
    for (unsigned i = 0; i < RACE_ROUNDS; i++) {
        uint32_t key = 0x100u + w; /* one caller per thread: its text names it */
        memset(buf, (int)(0x41 + w), sizeof(buf));
        buf[7] = (uint8_t)i;
        vjo_stream_offer_caller(race_slots, 2, &race_clock, key, buf, sizeof(buf), 0);
    }
    return NULL;
}
static void test_replacement_race(void)
{
    pthread_t t[RACERS];
    static VjoStreamSlot out;
    unsigned reads = 0, wrong = 0;
    memset(race_slots, 0, sizeof(race_slots));
    memset(&race_clock, 0, sizeof(race_clock));
    for (unsigned w = 0; w < RACERS; w++) pthread_create(&t[w], NULL, racer, (void *)(uintptr_t)w);
    for (unsigned i = 0; i < RACE_ROUNDS; i++)
        for (unsigned k = 0; k < 2; k++) {
            if (!vjo_stream_read(&race_slots[k], &out)) continue;
            reads++;
            /* the text is its key's caller's, whole */
            if (out.key < 0x100u || out.key >= 0x100u + RACERS || out.n != 8) { wrong++; continue; }
            for (unsigned b = 0; b < 7; b++)
                if (out.raw[b] != (uint8_t)(0x41 + (out.key - 0x100u))) { wrong++; break; }
        }
    for (unsigned w = 0; w < RACERS; w++) pthread_join(t[w], NULL);
    TEST_CHECK(wrong == 0);
    TEST_MSG("%u of %u reads hold another caller's text", wrong, reads);
    TEST_CHECK(race_clock.call_replaced > 0 && race_slots[0].lock == 0 && race_slots[1].lock == 0);
    TEST_CHECK(race_clock.call_dropped <= race_clock.dropped);
}

/* Game threads offering whole texts (every byte one value, the length from it) while the job
 * thread reads: a read is never torn, and every hit is stored, identical or dropped. */
#define WRITERS 4
#define ROUNDS 20000
static VjoStreamSlot shared;
static VjoStreamClock shared_clock;
static unsigned results[WRITERS][3];

static void *writer(void *arg)
{
    unsigned w = (unsigned)(uintptr_t)arg;
    uint8_t buf[64];
    for (unsigned i = 0; i < ROUNDS; i++) {
        uint8_t v = (uint8_t)(1 + (w * 7 + i) % 50);
        unsigned n = 4u + v;
        int r;
        memset(buf, v, n);
        r = vjo_stream_offer(&shared, &shared_clock, buf, n, 1);
        results[w][r + 1]++;
    }
    return NULL;
}

static void test_concurrent(void)
{
    pthread_t t[WRITERS];
    static VjoStreamSlot out;
    unsigned reads = 0, torn = 0;
    memset(&shared, 0, sizeof(shared));
    memset(&shared_clock, 0, sizeof(shared_clock));
    memset(results, 0, sizeof(results));
    for (unsigned w = 0; w < WRITERS; w++) pthread_create(&t[w], NULL, writer, (void *)(uintptr_t)w);
    for (unsigned i = 0; i < ROUNDS; i++) {
        if (!vjo_stream_read(&shared, &out)) continue;
        reads++;
        if (out.n < 5 || out.n > 54 || out.n != 4u + out.raw[0] || out.hash != vjo_place_hash(out.raw, out.n)) {
            torn++;
            continue;
        }
        for (unsigned k = 1; k < out.n; k++)
            if (out.raw[k] != out.raw[0]) {
                torn++;
                break;
            }
    }
    for (unsigned w = 0; w < WRITERS; w++) pthread_join(t[w], NULL);
    TEST_CHECK(torn == 0);
    TEST_MSG("%u of %u reads torn", torn, reads);
    {
        unsigned stored = 0, same = 0, dropped = 0;
        for (unsigned w = 0; w < WRITERS; w++) {
            dropped += results[w][0];
            same += results[w][1];
            stored += results[w][2];
        }
        TEST_CHECK(stored + same + dropped == WRITERS * ROUNDS);
        TEST_CHECK(dropped == shared_clock.dropped);
        TEST_CHECK(stored == shared_clock.clock);
        TEST_CHECK(shared.hits == stored + same);
        TEST_CHECK(shared.lock == 0);
    }
}

TEST_LIST = {
    {"offer_identical_new", test_offer_identical_new},
    {"bounds_busy", test_bounds_busy},
    {"clock_wrap", test_clock_wrap},
    {"caller_claim", test_caller_claim},
    {"caller_replacement", test_caller_replacement},
    {"replacement_age", test_replacement_age},
    {"replacement_race", test_replacement_race},
    {"concurrent", test_concurrent},
    {NULL, NULL},
};
