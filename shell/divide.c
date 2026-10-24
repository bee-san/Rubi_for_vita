/* The Shell's 32-bit integer division: the ARM run-time ABI's helpers, which GCC calls for `/` and `%`
 * on the Cortex-A9 (it has no divide instruction). libgcc's are unrolled for speed: 1,312 B of the
 * normal Shell's code pages, where the Shell divides in three places that are not hot (the region
 * selector's rectangle, once a region is saved, and its stick cursor's speed, twice a frame while the
 * stick moves it; the debug build's job log too). A bit-at-a-time loop from the quotient's highest bit
 * does it in 90 B of the normal Shell (106 B in the debug build, whose job log also takes a remainder:
 * docs/memory-ledger.md, "The reading total"), at 145 instructions a division against libgcc's 93.
 * Linked into the Shell only: the game worker's OCR divides in its loops and keeps libgcc's. A zero
 * divisor gives 0 (no call divides by zero; C leaves it undefined). host/tests/test_shell_divide.c checks
 * this source against what C's division means, on the host and in the ARM lane. */
#include <stdint.h>

unsigned __aeabi_uidiv(unsigned n, unsigned d)
{
    unsigned q = 0;
    int shift;
    if (d > n || !d)
        return 0;
    shift = __builtin_clz(d) - __builtin_clz(n); /* d << shift has n's highest bit */
    d <<= shift;
    do {
        q <<= 1;
        if (n >= d) {
            n -= d;
            q |= 1;
        }
        d >>= 1;
    } while (shift--);
    return q;
}

/* The quotient in r0 and the remainder in r1: one 64-bit value (AAPCS: its low word in r0) */
uint64_t __aeabi_uidivmod(unsigned n, unsigned d)
{
    unsigned q = __aeabi_uidiv(n, d);
    return (uint64_t)(n - q * d) << 32 | q;
}

int __aeabi_idiv(int n, int d)
{
    unsigned q = __aeabi_uidiv(n < 0 ? 0u - (unsigned)n : (unsigned)n, d < 0 ? 0u - (unsigned)d : (unsigned)d);
    return (int)((n ^ d) < 0 ? 0u - q : q); /* rounded toward zero, as C's */
}

uint64_t __aeabi_idivmod(int n, int d)
{
    unsigned q = (unsigned)__aeabi_idiv(n, d);
    return (uint64_t)((unsigned)n - q * (unsigned)d) << 32 | q;
}
