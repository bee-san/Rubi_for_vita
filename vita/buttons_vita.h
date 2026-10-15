#ifndef VJO_BUTTONS_VITA_H
#define VJO_BUTTONS_VITA_H
#include "../core/buttons.h"
#ifdef __cplusplus
extern "C" {
#endif
/* The console's enter and cancel buttons, from the system setting
 * /CONFIG/SYSTEM/button_assign (core/buttons.h); × enters if it cannot be read. */
VjoButtons vjo_buttons_read(void);
#ifdef __cplusplus
}
#endif
#endif
