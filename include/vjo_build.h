/* The build variant (docs/release-variants.md).
 *
 * VJO_DEBUG 1 is the debug build, the test builds' own: every log line, the
 * self-test, the timing and trace lines, the game worker's step log. VJO_DEBUG 0
 * is the normal build, the release's main download: errors, warnings and a short
 * log only; the rest is compiled out, its format strings included. Both read the
 * same config.ini, models and dictionaries.
 *
 * CMake sets it for every target (-DVJO_DEBUG=ON, the default, or OFF). A file
 * compiled without it is the debug build, so a stray build never loses its
 * diagnostics. include/vjo_api.h includes this header, so every plugin, core and
 * app source sees the same value. */
#ifndef VJO_BUILD_H
#define VJO_BUILD_H

#ifndef VJO_DEBUG
#define VJO_DEBUG 1
#endif

/* The variant's name in log lines and the Overlay app's version. */
#if VJO_DEBUG
#define VJO_VARIANT "debug"
#else
#define VJO_VARIANT "normal"
#endif

/* A debug-build line: vjo_dlog for the Shell's log (shell/log_vita.c vjo_log), klog_d for
 * kernel.txt (kernel/log.c klog). The debug build logs it as the logger does; the normal build
 * compiles it out, its format string and its arguments' evaluation included (the dead call keeps
 * them type-checked). Its arguments must have no side effects. Here rather than in shell.h and
 * kernel/vjo_kernel.h so that the host tests' substitutes for those headers have them too. */
#if VJO_DEBUG
#define vjo_dlog(...) vjo_log(__VA_ARGS__)
#define klog_d(...) klog(__VA_ARGS__)
#else
#define vjo_dlog(...) do { if (0) vjo_log(__VA_ARGS__); } while (0)
#define klog_d(...) do { if (0) klog(__VA_ARGS__); } while (0)
#endif

#endif
