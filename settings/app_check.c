/* The Rubi app against the overlay plugins (app_check.h). */
#include "app_check.h"

#include <psp2/io/fcntl.h>
#include <stdio.h>
#include <string.h>

#include "../core/app_stamp.h"
#include "../include/vjo_api.h"

int vjo_app_running_api(const char *status)
{
    const char *p = strstr(status, "shell API ");
    int v = 0, digits = 0;
    if (!p || memchr(status, '\n', (size_t)(p - status)))
        return -1; /* not on the first line */
    for (p += 10; *p >= '0' && *p <= '9' && digits < 6; p++, digits++)
        v = v * 10 + (*p - '0');
    return digits ? v : -1;
}

int vjo_app_banner_text(int app_api, const char *own, const char *package, const char *status, char *out,
                        size_t cap)
{
    return vjo_app_banner_text_for(app_api, own, package, status, 0, out, cap);
}

int vjo_app_banner_text_for(int app_api, const char *own, const char *package, const char *status, int has_payload,
                            char *out, size_t cap)
{
    int p = package ? vjo_app_stamp_api(package) : -1, r = status ? vjo_app_running_api(status) : -1;
    int mine = own && vjo_app_stamp_api(own) >= 0;
    out[0] = '\0';
    /* The plugins installed on the card came with another app (their record). */
    if (p >= 0 && (mine ? strcmp(own, package) != 0 : p != app_api)) {
        if (has_payload) {
            snprintf(out, cap, p > app_api ? "The installed overlay is newer than this app's: its Install page puts this "
                     "app's back, or install the newer VPK." : p < app_api ? "The installed overlay is older than this app: "
                     "its Install page updates it." : "The installed overlay is another build than this app's: see the "
                     "Install page.");
            return 1;
        }
        if (p > app_api)
            snprintf(out, cap, "This Rubi app is older than the installed overlay: install "
                     "ux0:data/Rubi.vpk in VitaShell.");
        else if (p < app_api)
            snprintf(out, cap, "The installed overlay is older than this app: run install-vita.sh from "
                     "this app's package, then reboot.");
        else
            snprintf(out, cap, "This Rubi app is not the installed overlay's: install "
                     "ux0:data/Rubi.vpk in VitaShell.");
        return 1;
    }
    /* The Shell plugin that last started (status.txt, rewritten at each boot that loads it) is another API's. */
    if (r >= 0 && r != app_api) {
        if (has_payload && !(r < app_api && mine && p == app_api)) {
            snprintf(out, cap, "The overlay that last started (API %d) is not this app's (API %d): see the Install page, "
                     "then restart the Vita.", r, app_api);
            return 1;
        }
        if (r > app_api)
            snprintf(out, cap, "This Rubi app (API %d) is older than the overlay that last started (API %d): "
                     "install ux0:data/Rubi.vpk in VitaShell.", app_api, r);
        else if (mine && p == app_api)
            snprintf(out, cap, "The overlay that last started (API %d) is older than this app: reboot the Vita "
                     "to load the one installed with it.", r);
        else
            snprintf(out, cap, "The overlay that last started (API %d) is older than this app (API %d): run "
                     "install-vita.sh from this app's package, then reboot.", r, app_api);
        return 1;
    }
    return 0;
}

/* The first line of path into buf, or NULL. */
static const char *first_line(const char *path, char *buf, int cap)
{
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    int used = 0, n = 0;
    if (fd < 0)
        return NULL;
    while (used < cap - 1 && (n = sceIoRead(fd, buf + used, (SceSize)(cap - 1 - used))) > 0)
        used += n;
    sceIoClose(fd);
    if (n < 0 || !used)
        return NULL;
    buf[used] = '\0';
    buf[strcspn(buf, "\r\n")] = '\0';
    return buf;
}

int vjo_app_banner(char *out, size_t cap, int has_payload)
{
    char own[VJO_APP_STAMP_MAX], package[VJO_APP_STAMP_MAX], status[96];
    /* This app is VJOSET001: when its app0: is not readable, its installed copy is the same file. */
    const char *mine = first_line(VJO_APP_STAMP_OWN, own, sizeof(own));
    if (!mine)
        mine = first_line(VJO_APP_STAMP_INSTALLED, own, sizeof(own));
    return vjo_app_banner_text_for(VJO_API_VERSION, mine, first_line(VJO_APP_STAMP_PACKAGE, package, sizeof(package)),
                                   first_line(VJO_APP_STATUS_PATH, status, sizeof(status)), has_payload, out, cap);
}
