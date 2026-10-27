/* The Rubi app's VjoPlatform (core/platform.h): the local dictionaries' files through sceIo and the process
 * clock, so that the app can look words up with the overlay's own code (core/local_dict.c, core/client.c): the Look
 * up page (#313) and the export's sentence readings (#311). Modelled on shell/platform_vita.c, without its counters.
 *
 * Stateless: it fills p and keeps nothing. p->local_session stays NULL; a caller that wants the dictionary
 * session's open files and caches between lookups sets its own (core/local_dict.h vjo_local_session_size), in its
 * own memory, used by one thread at a time. Each opened file is one sceIo descriptor until its close. */
#ifndef VJO_SETTINGS_APP_PLATFORM_H
#define VJO_SETTINGS_APP_PLATFORM_H
#include "../core/platform.h"

void vjo_app_platform(VjoPlatform *p);

#endif
