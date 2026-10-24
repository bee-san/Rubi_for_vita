/* The Shell's integer division (shell/divide.c: __aeabi_uidiv, __aeabi_uidivmod, __aeabi_idiv,
 * __aeabi_idivmod, in place of libgcc's) against what C's division means, checked without dividing: q is
 * the quotient of n by d when q * d <= n < (q + 1) * d (in 64 bits), and a signed quotient is rounded
 * toward zero, its remainder n - q * d smaller than d and of n's sign. On the host and, in the ARM lane
 * (tools/arm_host_tests.sh), on the Vita's CPU, where these functions are the program's own division. */
#include "acutest.h"
#include <stdint.h>
#include <string.h>

#include "../../shell/divide.c"

static unsigned failures;

static int unsigned_ok(unsigned n, unsigned d)
{
    unsigned q = __aeabi_uidiv(n, d);
    uint64_t qd = (uint64_t)q * d, both = __aeabi_uidivmod(n, d);
    int ok = d ? qd <= n && (uint64_t)n - qd < d : q == 0;
    ok = ok && (unsigned)both == q && (unsigned)(both >> 32) == (d ? (unsigned)((uint64_t)n - qd) : n);
    if (!ok && failures++ < 10)
        TEST_MSG("%u / %u: %u (remainder %u)", n, d, q, (unsigned)(both >> 32));
    return ok;
}

static int signed_ok(int n, int d)
{
    int q = __aeabi_idiv(n, d);
    uint64_t both = __aeabi_idivmod(n, d);
    int64_t r = (int64_t)n - (int64_t)q * d, ad = d < 0 ? -(int64_t)d : d;
    int ok;
    if (!d)
        ok = q == 0;
    else if (n == INT32_MIN && d == -1) /* the one quotient past int's range: wraps, as libgcc's */
        ok = q == INT32_MIN && (int)(uint32_t)(both >> 32) == 0;
    else
        ok = (r < 0 ? -r : r) < ad && (r == 0 || (r < 0) == (n < 0)) && (int)(uint32_t)(both >> 32) == (int)r;
    ok = ok && (int)(uint32_t)both == q;
    if (!ok && failures++ < 10)
        TEST_MSG("%d / %d: %d (remainder %d)", n, d, q, (int)(uint32_t)(both >> 32));
    return ok;
}

/* xorshift: the same pairs on every run and machine */
static uint32_t next(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

static void test_small_operands_all(void)
{
    unsigned bad = 0;
    failures = 0;
    for (unsigned n = 0; n < 1024; n++)
        for (unsigned d = 0; d < 1024; d++)
            bad += !unsigned_ok(n, d) + !signed_ok((int)n - 512, (int)d - 512);
    TEST_CHECK(bad == 0);
}

static void test_edges(void)
{
    static const uint32_t v[] = {0, 1, 2, 3, 7, 10, 255, 256, 960, 544, 1000, 62500, 65535, 65536, 0x7FFFFFFE, 0x7FFFFFFF,
                                 0x80000000u, 0x80000001u, 0xFFFFFFFEu, 0xFFFFFFFFu, 0x55555555u, 0xAAAAAAAAu};
    unsigned bad = 0;
    failures = 0;
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++)
        for (size_t j = 0; j < sizeof(v) / sizeof(v[0]); j++)
            for (int k = -1; k <= 1; k++) {
                uint32_t n = v[i] + (uint32_t)k, d = v[j];
                bad += !unsigned_ok(n, d) + !signed_ok((int)n, (int)d) + !signed_ok((int)n, (int)(0u - d));
            }
    TEST_CHECK(bad == 0);
    TEST_CHECK(__aeabi_uidiv(7, 0) == 0 && __aeabi_idiv(-7, 0) == 0); /* a zero divisor: 0 */
    TEST_CHECK(__aeabi_idiv(-7, 2) == -3 && __aeabi_idiv(7, -2) == -3 && __aeabi_idiv(-7, -2) == 3);
    TEST_CHECK(__aeabi_idiv(INT32_MIN, -1) == INT32_MIN && __aeabi_idiv(INT32_MIN, 1) == INT32_MIN);
}

/* Pairs of every width: n and d each a random number of 1 to 32 bits */
static void test_random_widths(void)
{
    uint32_t s = 0x9E3779B9u;
    unsigned bad = 0;
    failures = 0;
    for (int i = 0; i < 2000000; i++) {
        uint32_t n = next(&s) >> (next(&s) & 31), d = next(&s) >> (next(&s) & 31);
        bad += !unsigned_ok(n, d) + !signed_ok((int)n, (int)d);
    }
    TEST_CHECK(bad == 0);
}

/* The Shell's own divisions (core/region_select.c): the region's normalised corners, and the stick's speed */
static void test_the_shells_divisions(void)
{
    unsigned bad = 0;
    failures = 0;
    for (unsigned v = 0; v <= 960; v++)
        bad += !unsigned_ok(v * 65536u + 959u, 960) + !unsigned_ok(v * 65536u, 960) +
               !unsigned_ok(v * 65536u + 543u, 544) + !signed_ok((int)(v * 65536u + 959u), 960);
    for (uint32_t x = 0; x < 0xFFFFFFFFu - 4099; x += 4099)
        bad += !unsigned_ok(x, 62500);
    TEST_CHECK(bad == 0);
}

TEST_LIST = {
    {"every quotient and remainder of operands up to 1023 (signed: -512 to 511)", test_small_operands_all},
    {"powers of two, the type's ends, zero and the sign's edges", test_edges},
    {"2,000,000 pairs of random widths", test_random_widths},
    {"the Shell's divisions: the region's corners and the stick's speed", test_the_shells_divisions},
    {NULL, NULL}
};
