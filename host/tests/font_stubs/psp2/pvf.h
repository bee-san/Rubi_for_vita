#ifndef VJO_TEST_PVF_H
#define VJO_TEST_PVF_H
#include <stdint.h>
typedef uint32_t ScePvfError;
typedef void *ScePvfFontId;
ScePvfError scePvfSetCharSize(ScePvfFontId font, float h, float v);
#endif
