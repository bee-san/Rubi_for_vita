/* The console's enter and cancel buttons. ○ and × trade places with the system
 * setting /CONFIG/SYSTEM/button_assign: 0 makes ○ enter (the default on
 * Japanese consoles), 1 makes × enter (the default elsewhere), and HENkaku can
 * change it on any console. Applications get the same value from
 * SCE_SYSTEM_PARAM_ID_ENTER_BUTTON. Code asks for enter and cancel, never for
 * ○ or ×; vita/buttons_vita.c reads the setting.
 *
 * Plain values, no SDK calls: the host tests use it as the Vita build does. */
#ifndef VJO_BUTTONS_H
#define VJO_BUTTONS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VJO_BUTTON_CIRCLE 0x2000u /* SCE_CTRL_CIRCLE (psp2common/ctrl.h) */
#define VJO_BUTTON_CROSS  0x4000u /* SCE_CTRL_CROSS */
#define VJO_BUTTON_ASSIGN_CIRCLE 0 /* button_assign: ○ enters */
#define VJO_BUTTON_ASSIGN_CROSS  1 /* button_assign: × enters */

typedef struct {
    uint32_t enter, cancel;                 /* SCE_CTRL_* bits */
    const char *enter_glyph, *cancel_glyph; /* UTF-8, for hints and messages */
} VjoButtons;

/* The buttons for a button_assign value. Any other value, such as a failed
 * read's error, makes × enter: the setting of most consoles. */
static inline VjoButtons vjo_buttons(int button_assign)
{
    VjoButtons b;
    int circle = button_assign == VJO_BUTTON_ASSIGN_CIRCLE;
    b.enter = circle ? VJO_BUTTON_CIRCLE : VJO_BUTTON_CROSS;
    b.cancel = circle ? VJO_BUTTON_CROSS : VJO_BUTTON_CIRCLE;
    b.enter_glyph = circle ? "\xE2\x97\x8B" : "\xC3\x97";  /* ○ : × */
    b.cancel_glyph = circle ? "\xC3\x97" : "\xE2\x97\x8B"; /* × : ○ */
    return b;
}

#ifdef __cplusplus
}
#endif

#endif
