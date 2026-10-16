#include "app_stamp.h"

#include <string.h>

#define VJO_IO_NOENT ((int)0x80010002u) /* SCE_ERROR_ERRNO_ENOENT */

int vjo_app_state(int installed_rc, const char *installed, int app_present, int package_rc, const char *package)
{
    if (package_rc < 0 || !*package)
        return VJO_APP_UNKNOWN; /* nothing says which app the plugins came with */
    if (installed_rc >= 0)
        return strcmp(installed, package) ? VJO_APP_MISMATCH : VJO_APP_OK;
    /* No stamp beside an installed app: one from before the stamps (test.2's and older, #101). */
    return installed_rc == VJO_IO_NOENT && app_present ? VJO_APP_MISMATCH : VJO_APP_UNKNOWN;
}

int vjo_app_stamp_api(const char *line)
{
    const char *p;
    int v = 0, digits = 0;
    if (strncmp(line, "vjoapp1 ", 8) || !(p = strstr(line, " api=")))
        return -1;
    for (p += 5; *p >= '0' && *p <= '9' && digits < 6; p++, digits++)
        v = v * 10 + (*p - '0');
    return digits && (*p == ' ' || !*p) ? v : -1;
}
