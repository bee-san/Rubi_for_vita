/* A binary max-heap over an array: the sift, the heap's making and the in-place heapsort that the Rubi app's export
 * (settings/anki_export.c: the words, the known words' keys, the list's oldest cards) and the package's order
 * (core/apkg.c) each had a copy of (core/heap.c). less(a, b, ctx) is the order: the largest is on top. The steps
 * are those copies' own, so an array comes out the same, ties too: an element moves down only past a child
 * less(it, child), and of two children the right one only when less(left, right). In place, with no recursion and
 * no allocation. */
#ifndef VJO_HEAP_H
#define VJO_HEAP_H
#include <stddef.h>

typedef int (*VjoHeapLess)(const void *a, const void *b, void *ctx);

/* Element i (of size bytes) moved down to its place among the first n. */
void vjo_heap_down(void *base, int n, int i, size_t size, VjoHeapLess less, void *ctx);
/* The first n elements made a heap: the largest at 0. */
void vjo_heap_make(void *base, int n, size_t size, VjoHeapLess less, void *ctx);
/* The first n elements in ascending order (heapsort). */
void vjo_heap_sort(void *base, int n, size_t size, VjoHeapLess less, void *ctx);

#endif
