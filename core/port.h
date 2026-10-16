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

#endif
