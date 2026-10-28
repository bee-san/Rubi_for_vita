#include "heap.h"

#include <string.h>

/* Elements a and b (size bytes each) swapped, at most 128 bytes at a time. */
static void swap(unsigned char *a, unsigned char *b, size_t size)
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

void vjo_heap_down(void *base, int n, int i, size_t size, VjoHeapLess less, void *ctx)
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
        swap(e + (size_t)i * size, e + (size_t)k * size, size);
        i = k;
    }
}

void vjo_heap_make(void *base, int n, size_t size, VjoHeapLess less, void *ctx)
{
    for (int i = n / 2 - 1; i >= 0; i--)
        vjo_heap_down(base, n, i, size, less, ctx);
}

void vjo_heap_sort(void *base, int n, size_t size, VjoHeapLess less, void *ctx)
{
    unsigned char *e = (unsigned char *)base;
    vjo_heap_make(base, n, size, less, ctx);
    for (int end = n - 1; end > 0; end--) {
        swap(e, e + (size_t)end * size, size);
        vjo_heap_down(base, end, 0, size, less, ctx);
    }
}
