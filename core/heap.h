/* A binary max-heap over an array: the sift, the heap's making and the in-place heapsort that the Rubi app's export
 * (settings/anki_export.c: the words, the known words' keys, the list's oldest cards) and the package's order
 * (core/apkg.c) each had a copy of. less(a, b, ctx) is the
 * order: the largest is on top. The steps are those copies' own, so an array comes out the same, ties too: an
 * element moves down only past a child less(it, child), and of two children the right one only when
 * less(left, right). In place, with no recursion and no allocation. Static inline: each caller's comparison and
 * element size are inlined into its own copy. */
#ifndef VJO_HEAP_H
#define VJO_HEAP_H
#include <stddef.h>
#include <string.h>

typedef int (*VjoHeapLess)(const void *a, const void *b, void *ctx);

/* Elements a and b (size bytes each) swapped, at most 128 bytes at a time. */
static inline void vjo_heap_swap(unsigned char *a, unsigned char *b, size_t size)
{
    unsigned char t[128];
    while (size) {
        size_t n = size < sizeof(t) ? size : sizeof(t);
        memcpy(t, a, n);
        memcpy(a, b, n);
        memcpy(b, t, n);
        a += n;
        b += n;
        size -= n;
    }
}

/* Element i moved down to its place among the first n. */
static inline void vjo_heap_down(void *base, int n, int i, size_t size, VjoHeapLess less, void *ctx)
{
    unsigned char *e = (unsigned char *)base;
    for (;;) {
        int k = 2 * i + 1;
        if (k >= n)
            return;
        if (k + 1 < n && less(e + (size_t)k * size, e + (size_t)(k + 1) * size, ctx))
            k++;
        if (!less(e + (size_t)i * size, e + (size_t)k * size, ctx))
            return;
        vjo_heap_swap(e + (size_t)i * size, e + (size_t)k * size, size);
        i = k;
    }
}

/* The first n elements made a heap: the largest at 0. */
static inline void vjo_heap_make(void *base, int n, size_t size, VjoHeapLess less, void *ctx)
{
    for (int i = n / 2 - 1; i >= 0; i--)
        vjo_heap_down(base, n, i, size, less, ctx);
}

/* The first n elements in ascending order (heapsort). */
static inline void vjo_heap_sort(void *base, int n, size_t size, VjoHeapLess less, void *ctx)
{
    unsigned char *e = (unsigned char *)base;
    vjo_heap_make(base, n, size, less, ctx);
    for (int end = n - 1; end > 0; end--) {
        vjo_heap_swap(e, e + (size_t)end * size, size);
        vjo_heap_down(base, end, 0, size, less, ctx);
    }
}

#endif
