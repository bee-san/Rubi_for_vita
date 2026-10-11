#ifndef VJO_SETTINGS_ANKI_ACCOUNT_H
#define VJO_SETTINGS_ANKI_ACCOUNT_H
#include "../core/arena.h"
#define VJO_ANKI_ACCOUNT_PATH "ux0:data/VitaJPOverlay/anki_account.json"
typedef struct { char username[256], password[256]; } VjoAnkiAccount;
/* Plain device storage, separate from exported config/queue. No network here.
 * 0 primary/missing, 1 recovered backup, -1 unreadable. */
int vjo_anki_account_load(VjoAnkiAccount *account, VjoArena *scratch);
int vjo_anki_account_save(const VjoAnkiAccount *account, VjoArena *scratch);
int vjo_anki_account_forget(void);
#endif
