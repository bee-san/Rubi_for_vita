/* Global C++ allocation for the game worker and the host tools that run
 * PP-OCRv6: while a job's heap is active, ncnn's objects live in it (and are
 * all destroyed before the job ends); otherwise core/ocr_heap.c refuses on the
 * Vita and uses the host C++ runtime's malloc on the host. Never linked into
 * SceShell, whose global operators belong to Paf. */
#include <stddef.h>
#include "ocr_heap.h"

void *operator new(size_t size) { return vjo_ocr_new(size); }
void *operator new[](size_t size) { return vjo_ocr_new(size); }
void operator delete(void *ptr) noexcept { vjo_ocr_delete(ptr); }
void operator delete[](void *ptr) noexcept { vjo_ocr_delete(ptr); }
void operator delete(void *ptr, size_t) noexcept { vjo_ocr_delete(ptr); }
void operator delete[](void *ptr, size_t) noexcept { vjo_ocr_delete(ptr); }

#ifndef VJO_HOST
/* No C++ runtime in the worker: a pure virtual call is a programming error.
 * Spin rather than return into undefined state. */
extern "C" void __cxa_pure_virtual(void) { for (;;) {} }
#endif
