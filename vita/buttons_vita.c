#include "buttons_vita.h"
#include <psp2/registrymgr.h>

VjoButtons vjo_buttons_read(void)
{
    int assign = -1;
    if (sceRegMgrGetKeyInt("/CONFIG/SYSTEM", "button_assign", &assign) < 0)
        assign = -1;
    return vjo_buttons(assign);
}
