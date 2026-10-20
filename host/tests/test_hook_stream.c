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
    VjoStreamClock c = {0, 0};
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
    VjoStreamClock c = {0, 0};
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
    VjoStreamClock c = {0xFFFFFFFFu, 0};
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
    {"concurrent", test_concurrent},
    {NULL, NULL},
};
