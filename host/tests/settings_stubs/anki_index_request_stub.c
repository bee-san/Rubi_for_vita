/* settings/anki_index_request.c for the app-loop suites (test_settings_app.c, test_settings_games.c): no
 * Anki for Vita, so the start writes nothing and their file scripts stay as they were. The request
 * itself is tested in host/tests/test_settings_anki_index.c. */
#include <stdio.h>

#include "../../../settings/anki_index_request.h"

int vjo_anki_index_request(VjoArena *scratch, char *msg, size_t cap)
{
    (void)scratch;
    snprintf(msg, cap, "Anki for Vita: not installed (or no interface version 1)");
    return VJO_ANKI_REQUEST_NO_APP;
}
