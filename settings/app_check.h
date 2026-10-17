/* The Rubi app against the overlay plugins (core/app_stamp.h): the red
 * banner under the app's header when they do not match (#101: test.1's app
 * ran beside test.2's plugins, showed VOCR2, had no Anki page and wrote
 * ocr_backend = vocr2). */
#ifndef VJO_SETTINGS_APP_CHECK_H
#define VJO_SETTINGS_APP_CHECK_H

#include <stddef.h>

#define VJO_APP_STATUS_PATH "ux0:data/VitaJPOverlay/status.txt" /* the Shell's, rewritten at each boot that loads it */

/* The API of the Shell plugin that last started, from status.txt's text
 * ("bootstrap: shell API 21 entered" is its first line since API 13), or -1. */
int vjo_app_running_api(const char *status);

/* The banner for this app (API app_api; own: its stamp, app0:vjo_app.txt),
 * the plugins' record (package, ux0:data/VitaJPOverlay/vjo_app.txt) and the
 * Shell that last started (status.txt's text); NULL text = not readable.
 * Writes the banner into out and returns 1, or returns 0 with out = "" when
 * nothing is known not to match. */
int vjo_app_banner_text(int app_api, const char *own, const char *package, const char *status, char *out,
                        size_t cap);
/* The same for an app that carries the overlay (its payload, docs/one-vpk-install.md): the banner
 * names its Install page instead of install-vita.sh or a VPK in ux0:data. */
int vjo_app_banner_text_for(int app_api, const char *own, const char *package, const char *status, int has_payload,
                            char *out, size_t cap);

/* The same from the files on the memory card. */
int vjo_app_banner(char *out, size_t cap, int has_payload);

#endif
