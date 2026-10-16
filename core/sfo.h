#ifndef VJO_SFO_H
#define VJO_SFO_H
#include <stddef.h>
/* Copy a NUL-terminated UTF-8 string from a bounded PARAM.SFO image. The
 * string ends at a NUL within the entry's length, or, when its length field
 * is stale, within its capacity (older Adrenaline Bubble Manager bubbles). */
int vjo_sfo_string(const void *data, size_t len, const char *key, char *out, size_t cap);
#endif
