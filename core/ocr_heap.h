#ifndef VJO_OCR_HEAP_H
#define VJO_OCR_HEAP_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
int vjo_ocr_heap_begin(void *memory, size_t size);
size_t vjo_ocr_heap_end(void);
/* The highest heap offset the last job's allocations reached (after heap_end). */
size_t vjo_ocr_heap_extent(void);
int vjo_ocr_heap_failed(void);
int vjo_ocr_heap_owns(const void *ptr);
void *vjo_ncnn_malloc(size_t size);
void vjo_ncnn_free(void *ptr);
int vjo_ncnn_posix_memalign(void **ptr, size_t alignment, size_t size);
/* operator new/delete for executables that link core/ncnn_new.cpp. */
void *vjo_ocr_new(size_t size);
void vjo_ocr_delete(void *ptr);
#ifdef __cplusplus
}
#endif
#endif
