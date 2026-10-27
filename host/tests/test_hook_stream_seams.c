/* The CALL slots' replacement (core/hook_stream.c, replace()) at each point where another thread can act, one
 * interleaving at a time (#366). This suite includes core/hook_stream.c with VJO_STREAM_SEAM defined, and at a seam
 * (the scan about to read slot i; the scan's choice made, before its try-lock; the slot's lock held and every check
 * passed, before the new caller's key; the slot the new caller's, before its text) it runs one step of another thread
 * on this one: a caller's store, another caller's replacement, the job keeping, reading or watching a slot. Single-
 * threaded and exact: each interleaving happens on every run, where the threaded races of test_hook_stream.c hit a
 * window a few loads wide only now and then. The worker builds the seams as nothing. */
#include "acutest.h"

#include <string.h>

static void seam_scan(unsigned i);
static void seam_chosen(unsigned i);
static void seam_taking(unsigned i);
static void seam_replacing(unsigned i);
#define VJO_STREAM_SEAM(point, i) seam_##point(i)
#include "../../core/hook_stream.c"

#define KEY_A 0xA0u
#define KEY_B 0xB0u
#define KEY_C 0xC0u /* the new caller whose hit the tests follow */
#define KEY_D 0xD0u

enum { AT_SCAN = 1, AT_CHOSEN, AT_TAKING, AT_REPLACING };
static struct {
    int point;          /* AT_SCAN: before the scan reads slot `at`; AT_CHOSEN: the scan chose slot `at`; AT_TAKING:
                           slot `at`'s lock held, every check passed, before the new caller's key; AT_REPLACING: slot
                           `at` is the new caller's, before its text */
    unsigned at;
    void (*step)(void); /* the other thread's step; NULL once it has run */
    unsigned ran;       /* steps run: a test checks its interleaving happened */
} seam;

static void seam_run(int point, unsigned i)
{
    void (*step)(void) = seam.step;
    if (!step || seam.point != point || seam.at != i) return;
    seam.step = NULL; /* once: the step's own offers pass the seams untouched */
    seam.ran++;
    step();
}
static void seam_scan(unsigned i) { seam_run(AT_SCAN, i); }
static void seam_chosen(unsigned i) { seam_run(AT_CHOSEN, i); }
static void seam_taking(unsigned i) { seam_run(AT_TAKING, i); }
static void seam_replacing(unsigned i) { seam_run(AT_REPLACING, i); }

static VjoStreamSlot slots[2];
static VjoStreamClock c;

static int offer(uint32_t key, const char *text)
{
    return vjo_stream_offer_caller(slots, 2, &c, key, (const uint8_t *)text, (unsigned)strlen(text), 0);
}

static void reset(uint32_t clock)
{
    memset(slots, 0, sizeof(slots));
    memset(&c, 0, sizeof(c));
    memset(&seam, 0, sizeof(seam));
    c.clock = clock;
}

/* A, then B, store a text: both slots held, A's the least recently written (seq clock + 1, B's clock + 2). */
static void setup(uint32_t clock)
{
    reset(clock);
    TEST_ASSERT(offer(KEY_A, "aaaa") == 1 && slots[0].key == KEY_A);
    TEST_ASSERT(offer(KEY_B, "bbbb") == 1 && slots[1].key == KEY_B);
}

static void arm(int point, unsigned at, void (*step)(void))
{
    seam.point = point;
    seam.at = at;
    seam.step = step;
}

/* The other thread's steps */
static void a_writes(void) { TEST_CHECK(offer(KEY_A, "aaa2") == 1); }
static void b_writes(void) { TEST_CHECK(offer(KEY_B, "bbb2") == 1); }
static void d_takes_a(void) { TEST_CHECK(offer(KEY_D, "dddd") == 1 && slots[0].key == KEY_D); }
static void c_takes_a(void) { TEST_CHECK(offer(KEY_C, "cccc") == 1 && slots[0].key == KEY_C); } /* C's other thread */
/* The job keeps A's slot as game_ocr/hooks.c vjo_hooks_keep does (an agreeing stream, or a pin): under its try-lock;
 * busy, not now */
static void job_keeps_a(void)
{
    if (!vjo_stream_try_lock(&slots[0])) return;
    vjo_stream_keep(&slots[0]);
    vjo_stream_unlock(&slots[0]);
}
static void job_locks_a(void) { TEST_CHECK(vjo_stream_try_lock(&slots[0]) == 1); } /* the job reading it */
static int c2_rc; /* the result of the hit of C's other thread */
static void c2_offers(void) { c2_rc = offer(KEY_C, "ccc2"); }
static void job_keeps_a_then_c2_offers(void)
{
    job_keeps_a();
    c2_offers();
}
static void a_writes_c_takes_b(void)
{
    a_writes();
    TEST_CHECK(offer(KEY_C, "cccc") == 1 && slots[1].key == KEY_C);
}
/* The worker's hook watch on A's slot, as game_ocr/hooks.c vjo_hooks_seq reads it: without the lock */
static void watch_reads_a(void)
{
    uint32_t n = __atomic_load_n(&slots[0].n, __ATOMIC_RELAXED), seq = __atomic_load_n(&slots[0].seq, __ATOMIC_ACQUIRE);
    TEST_CHECK_(seq == 1 && n == 4, "the watch sees the slot's last text: seq %u (1), %u bytes (4)", (unsigned)seq,
                (unsigned)n);
}

static int locks_free(void) { return slots[0].lock == 0 && slots[1].lock == 0; }

/* No step: C takes A's slot, the least recently written (the seams change nothing by themselves). */
static void test_no_step(void)
{
    setup(0);
    TEST_CHECK(offer(KEY_C, "cccc") == 1 && seam.ran == 0);
    TEST_CHECK(slots[0].key == KEY_C && slots[0].seq == 3 && slots[0].hits == 1 && !memcmp(slots[0].raw, "cccc", 4));
    TEST_CHECK(slots[1].key == KEY_B && c.call_replaced == 1 && c.call_dropped == 0 && locks_free());
}

/* #366's first finding, as it happens: B stores while C's scan runs, before the scan reads A's slot, and before it
 * reads B's. B's seq is then after the clock's value as the scan began: main read the clock once, there, so B's age
 * wrapped to about 2^32 and C took B's slot, the newest. C takes A's. */
static void test_scan_meets_a_store(void)
{
    for (unsigned at = 0; at < 2; at++) {
        TEST_CASE_("B stores before the scan reads slot %u", at);
        setup(0);
        arm(AT_SCAN, at, b_writes);
        TEST_CHECK(offer(KEY_C, "cccc") == 1 && seam.ran == 1);
        TEST_CHECK_(slots[0].key == KEY_C && slots[1].key == KEY_B && !memcmp(slots[1].raw, "bbb2", 4),
                    "C takes A's slot: slot 0 key %#x, slot 1 key %#x", (unsigned)slots[0].key,
                    (unsigned)slots[1].key);
        TEST_CHECK(c.call_replaced == 1 && c.call_dropped == 0 && locks_free());
    }
}

/* The same across the clock's wrap: A at 0xFFFFFFFE, B at 0xFFFFFFFF, and B's store during the scan takes 1 (0 is
 * skipped). In the clock's serial order B's 1 is the newest, so C takes A's slot. */
static void test_scan_meets_a_store_across_wrap(void)
{
    setup(0xFFFFFFFDu);
    TEST_ASSERT(slots[0].seq == 0xFFFFFFFEu && slots[1].seq == 0xFFFFFFFFu);
    arm(AT_SCAN, 0, b_writes);
    TEST_CHECK(offer(KEY_C, "cccc") == 1 && seam.ran == 1);
    TEST_CHECK_(slots[0].key == KEY_C && slots[0].seq == 2 && slots[1].key == KEY_B && slots[1].seq == 1,
                "C takes A's slot: slot 0 key %#x, slot 1 key %#x", (unsigned)slots[0].key, (unsigned)slots[1].key);
    TEST_CHECK(c.call_replaced == 1 && locks_free());
}

/* The scan chose A's slot (the least recently written), and A stores before C takes the slot's lock: A's is now the
 * newest. Under the lock C finds A's seq changed and drops its hit (call_dropped), as when the slot is busy; main
 * took A's slot. Also with A's store as the scan reads B's slot, after it read A's. C's next hit takes B's slot, the
 * least recently written by then. */
static void test_chosen_then_written(void)
{
    static const struct {
        int point;
        unsigned at;
    } when[] = {{AT_CHOSEN, 0}, {AT_SCAN, 1}};
    for (unsigned k = 0; k < 2; k++) {
        TEST_CASE(k ? "A stores as the scan reads B's slot" : "A stores after the scan chose its slot");
        setup(0);
        arm(when[k].point, when[k].at, a_writes);
        TEST_CHECK(offer(KEY_C, "cccc") == -1 && seam.ran == 1);
        TEST_CHECK_(slots[0].key == KEY_A && !memcmp(slots[0].raw, "aaa2", 4) && slots[0].seq == 3,
                    "A keeps its slot: slot 0 key %#x", (unsigned)slots[0].key);
        TEST_CHECK(slots[1].key == KEY_B && !memcmp(slots[1].raw, "bbbb", 4));
        TEST_CHECK(c.call_replaced == 0 && c.call_dropped == 1 && c.dropped == 1 && locks_free());
        TEST_CHECK(offer(KEY_C, "cccc") == 1 && slots[1].key == KEY_C && slots[0].key == KEY_A);
        TEST_CHECK(c.call_replaced == 1);
    }
}

/* The scan chose A's slot, and D, another new caller, takes it before C's try-lock: D's stream is the newest. C drops
 * its hit; main took the slot from D at once (D lost the slot it had just taken: call_replaced 2). */
static void test_chosen_then_replaced(void)
{
    setup(0);
    arm(AT_CHOSEN, 0, d_takes_a);
    TEST_CHECK(offer(KEY_C, "cccc") == -1 && seam.ran == 1);
    TEST_CHECK_(slots[0].key == KEY_D && !memcmp(slots[0].raw, "dddd", 4) && slots[0].hits == 1,
                "D keeps the slot it took: slot 0 key %#x", (unsigned)slots[0].key);
    TEST_CHECK(c.call_replaced == 1 && c.call_dropped == 1 && locks_free());
    TEST_CHECK(offer(KEY_C, "cccc") == 1 && slots[1].key == KEY_C && slots[0].key == KEY_D && c.call_replaced == 2);
}

/* B claimed its slot, but its first text was dropped: seq 0, never had text, so the least recently written. The scan
 * chose it, and B's first text lands before C's try-lock. C drops its hit; main took B's slot, by then the newest. */
static void test_chosen_then_first_text(void)
{
    reset(0);
    TEST_ASSERT(offer(KEY_A, "aaaa") == 1 && slots[0].seq == 1);
    TEST_ASSERT(vjo_stream_caller(slots, 2, KEY_B) == &slots[1] && slots[1].seq == 0);
    arm(AT_CHOSEN, 1, b_writes);
    TEST_CHECK(offer(KEY_C, "cccc") == -1 && seam.ran == 1);
    TEST_CHECK_(slots[1].key == KEY_B && slots[1].seq == 2 && !memcmp(slots[1].raw, "bbb2", 4),
                "B keeps its slot: slot 1 key %#x", (unsigned)slots[1].key);
    TEST_CHECK(c.call_replaced == 0 && c.call_dropped == 1 && locks_free());
    TEST_CHECK(offer(KEY_C, "cccc") == 1 && slots[0].key == KEY_C && slots[1].key == KEY_B);
}

/* As before #366: the scan chose A's slot, and another thread of C's caller takes it first. C's hit is then stored in
 * C's own slot (here identical: a hit only), not dropped. */
static void test_chosen_then_same_caller(void)
{
    setup(0);
    arm(AT_CHOSEN, 0, c_takes_a);
    TEST_CHECK(offer(KEY_C, "cccc") == 0 && seam.ran == 1);
    TEST_CHECK(slots[0].key == KEY_C && slots[0].hits == 2 && slots[1].key == KEY_B);
    TEST_CHECK(c.call_replaced == 1 && c.call_dropped == 0 && locks_free());
}

/* As before: kept after the scan chose it (the stream agreed with OCR): dropped; C's next hit takes B's slot. */
static void test_chosen_then_kept(void)
{
    setup(0);
    arm(AT_CHOSEN, 0, job_keeps_a);
    TEST_CHECK(offer(KEY_C, "cccc") == -1 && seam.ran == 1 && c.call_dropped == 1 && c.call_replaced == 0);
    TEST_CHECK(slots[0].key == KEY_A && slots[0].kept == 1 && locks_free());
    TEST_CHECK(offer(KEY_C, "cccc") == 1 && slots[1].key == KEY_C && slots[0].key == KEY_A);
}

/* As before: the job holds the chosen slot's lock (reading it): dropped, never waiting; once the job is done, C's next
 * hit takes it (still the least recently written). */
static void test_chosen_then_busy(void)
{
    setup(0);
    arm(AT_CHOSEN, 0, job_locks_a);
    TEST_CHECK(offer(KEY_C, "cccc") == -1 && seam.ran == 1 && c.call_dropped == 1);
    TEST_CHECK(slots[0].key == KEY_A && slots[0].lock == 1);
    vjo_stream_unlock(&slots[0]);
    TEST_CHECK(offer(KEY_C, "cccc") == 1 && slots[0].key == KEY_C && c.call_replaced == 1 && locks_free());
}

/* As before: A stores after the scan chose its slot, and another thread of C's caller then takes B's. Both checks under
 * the lock drop C's hit (A's seq changed; C has a slot already), and A keeps its slot. */
static void test_chosen_then_written_and_caller_elsewhere(void)
{
    setup(0);
    arm(AT_CHOSEN, 0, a_writes_c_takes_b);
    TEST_CHECK(offer(KEY_C, "cccc") == -1 && seam.ran == 1);
    TEST_CHECK(slots[0].key == KEY_A && slots[1].key == KEY_C && !memcmp(slots[0].raw, "aaa2", 4));
    TEST_CHECK(c.call_replaced == 1 && c.call_dropped == 1 && locks_free());
}

/* (audit-rubi-366 F3) C's other thread takes A's slot as C's scan begins, before it reads slot 0: the scan then chooses
 * B's slot, unchanged, so only the check for this caller's slot elsewhere drops the hit. One slot per caller. */
static void test_scan_then_caller_elsewhere(void)
{
    setup(0);
    arm(AT_SCAN, 0, c_takes_a);
    TEST_CHECK(offer(KEY_C, "cccc") == -1 && seam.ran == 1);
    TEST_CHECK_(slots[0].key == KEY_C && slots[1].key == KEY_B && !memcmp(slots[1].raw, "bbbb", 4),
                "B keeps its slot: slot 0 key %#x, slot 1 key %#x", (unsigned)slots[0].key, (unsigned)slots[1].key);
    TEST_CHECK(c.call_replaced == 1 && c.call_dropped == 1 && locks_free());
}

/* (audit-rubi-366 F2) A's slot, the least recently written, is busy as the scan reads it: the job reading it, or a
 * thread of C's own caller taking it, its lock held. C's hit is dropped, never waiting, and B's slot (the next least
 * recently written) is not taken: passing a busy slot over gave a caller with two threads two slots when its other
 * thread was the one holding A's lock (measured: #366's audit follow-up). Once A's lock is free, C's next hit takes A's
 * slot. This pins the rule, not the invariant (one slot per caller: taking_meets_my_other_thread); a rule that ends
 * the wait without a second slot would take B's here instead (#366's audit-b, N1). */
static void test_scan_meets_a_busy_slot(void)
{
    setup(0);
    arm(AT_SCAN, 0, job_locks_a);
    TEST_CHECK(offer(KEY_C, "cccc") == -1 && seam.ran == 1);
    TEST_CHECK_(slots[0].key == KEY_A && slots[1].key == KEY_B && !memcmp(slots[1].raw, "bbbb", 4),
                "nothing taken: slot 0 key %#x, slot 1 key %#x", (unsigned)slots[0].key, (unsigned)slots[1].key);
    TEST_CHECK(c.call_replaced == 0 && c.call_dropped == 1 && slots[0].lock == 1 && slots[1].lock == 0);
    vjo_stream_unlock(&slots[0]);
    TEST_CHECK(offer(KEY_C, "cccc") == 1 && slots[0].key == KEY_C && c.call_replaced == 1 && locks_free());
}

/* (#366's audit-b, N1: F2's invariant) C holds A's lock, every check passed, about to take it, when C's other thread
 * offers: the other thread's hit is dropped (A busy), and C ends with A's slot only, B keeping its own. A scan that
 * passed busy slots over took B for the other thread here: a second slot for C. */
static void test_taking_meets_my_other_thread(void)
{
    setup(0);
    c2_rc = 99;
    arm(AT_TAKING, 0, c2_offers);
    TEST_CHECK(offer(KEY_C, "cccc") == 1 && seam.ran == 1);
    TEST_CHECK_(c2_rc == -1, "C's other thread: %d (-1, dropped)", c2_rc);
    TEST_CHECK_(slots[0].key == KEY_C && slots[1].key == KEY_B && !memcmp(slots[1].raw, "bbbb", 4),
                "one slot for C: slot 0 key %#x, slot 1 key %#x", (unsigned)slots[0].key, (unsigned)slots[1].key);
    TEST_CHECK(c.call_replaced == 1 && c.call_dropped == 1 && locks_free());
}

/* (#366's audit-b, F3) The job keeps A (a pin, or an agreeing stream) while C holds A's lock taking it: the keep takes
 * A's try-lock, finds it busy and keeps nothing now. C's other thread then offers: A is not kept, so it is still the
 * least recently written, busy: dropped. One slot for C. A keep without the lock landed here and let the other thread
 * pass A over as kept and take B. The next job's keep keeps A, now C's. */
static void test_taking_then_kept_meets_my_other_thread(void)
{
    setup(0);
    c2_rc = 99;
    arm(AT_TAKING, 0, job_keeps_a_then_c2_offers);
    TEST_CHECK(offer(KEY_C, "cccc") == 1 && seam.ran == 1);
    TEST_CHECK_(c2_rc == -1, "C's other thread: %d (-1, dropped)", c2_rc);
    TEST_CHECK_(slots[0].key == KEY_C && slots[1].key == KEY_B, "one slot for C: slot 0 key %#x, slot 1 key %#x",
                (unsigned)slots[0].key, (unsigned)slots[1].key);
    TEST_CHECK(slots[0].kept == 0 && c.call_replaced == 1 && locks_free());
    job_keeps_a();
    TEST_CHECK(slots[0].kept == 1 && slots[0].key == KEY_C);
}

/* (audit-rubi-366 N1) The worker's hook watch reads a slot without its lock while a new caller takes it: until the new
 * caller's text is stored it sees the old text's seq and length, never seq 0 ("never had text", which it skips), as a
 * replacement's seq = 0 before the new text made it (before #366); then the new text's. */
static void test_replacing_while_watched(void)
{
    setup(0);
    arm(AT_REPLACING, 0, watch_reads_a);
    TEST_CHECK(offer(KEY_C, "cccc") == 1 && seam.ran == 1);
    TEST_CHECK(slots[0].key == KEY_C && slots[0].seq == 3 && slots[0].n == 4 && c.call_replaced == 1 && locks_free());
}

TEST_LIST = {
    {"no_step", test_no_step},
    {"scan_meets_a_store", test_scan_meets_a_store},
    {"scan_meets_a_store_across_wrap", test_scan_meets_a_store_across_wrap},
    {"chosen_then_written", test_chosen_then_written},
    {"chosen_then_replaced", test_chosen_then_replaced},
    {"chosen_then_first_text", test_chosen_then_first_text},
    {"chosen_then_same_caller", test_chosen_then_same_caller},
    {"chosen_then_kept", test_chosen_then_kept},
    {"chosen_then_busy", test_chosen_then_busy},
    {"chosen_then_written_and_caller_elsewhere", test_chosen_then_written_and_caller_elsewhere},
    {"scan_then_caller_elsewhere", test_scan_then_caller_elsewhere},
    {"scan_meets_a_busy_slot", test_scan_meets_a_busy_slot},
    {"taking_meets_my_other_thread", test_taking_meets_my_other_thread},
    {"taking_then_kept_meets_my_other_thread", test_taking_then_kept_meets_my_other_thread},
    {"replacing_while_watched", test_replacing_while_watched},
    {NULL, NULL},
};
