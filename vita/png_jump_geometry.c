/* Build-only evidence for the linked native PNG runtime gate. */
#include <stddef.h>
#include "pngpriv.h"

const unsigned int vjo_png_jump_geometry[] = {
    sizeof(((png_struct *)0)->jmp_buf_local),
    offsetof(png_struct, longjmp_fn),
    offsetof(png_struct, jmp_buf_ptr),
    offsetof(png_struct, jmp_buf_size)
};
