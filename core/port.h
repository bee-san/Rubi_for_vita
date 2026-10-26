/* Portability shims for the core. On the Vita the SceShell plugin is linked
 * without newlib, so formatted output goes through sceClib. The remaining
 * libc functions the core uses (memcpy, memmove, memset, memcmp, strlen,
 * strncmp, strchr) are provided by shell/libc_shim.c there. */
#ifndef VJO_PORT_H
#define VJO_PORT_H

#include <stdarg.h>
#include <stddef.h>

#if defined(VJO_KERNEL)
/* The kernel plugin (core/games.c, for kernel/loader.c): SceSysclibForDriver's
 * own (psp2kern/kernel/sysclib.h). */
int snprintf(char *dst, unsigned int max, const char *fmt, ...);
int vsnprintf(char *dst, unsigned int max, const char *fmt, va_list arg);
#define vjo_vsnprintf vsnprintf
#define vjo_snprintf snprintf
#elif defined(__vita__) && !defined(VJO_HOST) && !defined(VJO_STANDALONE)
int sceClibVsnprintf(char *dst, size_t dst_max_size, const char *fmt, va_list args);
int sceClibSnprintf(char *dst, size_t dst_max_size, const char *fmt, ...);
#define vjo_vsnprintf sceClibVsnprintf
#define vjo_snprintf sceClibSnprintf
#else
#include <stdio.h>
#define vjo_vsnprintf vsnprintf
#define vjo_snprintf snprintf
#endif

/* A read-only table that the Vita build keeps in its data segment instead of
 * its code segment: the Shell's code pages are full in the normal build, and
 * the last page of its data segment has room, so the module's pages stay as
 * they are (docs/memory-ledger.md, "Tables in the data segment"). The table
 * stays const (never written); on the host it is ordinary read-only data
 * (.rodata; a table of pointers in a position-independent build, as Ubuntu's
 * GCC makes by default, is .data.rel.ro, read-only once relocated). */
#if defined(__vita__)
#define VJO_DATA_TABLE(name) __attribute__((section(".data.vjo." #name)))
#else
#define VJO_DATA_TABLE(name)
#endif

#endif
