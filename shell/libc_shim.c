/* SceShell plugins run without newlib: route the few libc functions the core
 * and BearSSL use to SceLibKernel's sceClib.
 *
 * Most are ARM code, the state of the stubs they branch to: a Thumb wrapper's
 * tail call to an ARM stub goes through a 16-byte interworking veneer the
 * linker adds (__sceClibMemcpy_from_thumb, ...), one a function, where an ARM
 * one branches to the stub directly, in the same 4 bytes. Their Thumb callers
 * reach them by BLX, as they reach the stubs themselves: no veneer, no byte
 * more (the Shell's 9 veneers, 144 B of its text; 7 of the game worker's,
 * 112 B: docs/memory-ledger.md). memset stays Thumb: a Thumb function
 * tail-calls it, and an ARM memset would only move its veneer to that call. */
#include <psp2/kernel/clib.h>
#include <stddef.h>

#define ARM_CODE __attribute__((target("arm")))

ARM_CODE void *memcpy(void *d, const void *s, size_t n) { return sceClibMemcpy(d, s, n); }
ARM_CODE void *memmove(void *d, const void *s, size_t n) { return sceClibMemmove(d, s, n); }
void *memset(void *d, int c, size_t n) { return sceClibMemset(d, c, n); }
ARM_CODE int memcmp(const void *a, const void *b, size_t n) { return sceClibMemcmp(a, b, n); }
ARM_CODE void *memchr(const void *s, int c, size_t n) { return sceClibMemchr(s, c, n); }
ARM_CODE size_t strlen(const char *s) { return sceClibStrnlen(s, 0x7fffffff); }
ARM_CODE int strcmp(const char *a, const char *b) { return sceClibStrcmp(a, b); }
ARM_CODE int strncmp(const char *a, const char *b, size_t n) { return sceClibStrncmp(a, b, n); }
ARM_CODE char *strchr(const char *s, int c) { return sceClibStrchr(s, c); }
ARM_CODE char *strstr(const char *h, const char *n) { return sceClibStrstr(h, n); }
int abs(int n) { return n < 0 ? -n : n; }
