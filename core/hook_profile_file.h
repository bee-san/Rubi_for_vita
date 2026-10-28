/* Indexed on-disk hook catalogue. Reads only the index rows needed to locate
 * one title and that title's blob. No allocation or retained file handle. */
#ifndef VJO_HOOK_PROFILE_FILE_H
#define VJO_HOOK_PROFILE_FILE_H
#include <stdint.h>
#define VJO_PROFILE_BYTES 1024u
#define VJO_PROFILE_DB_PATH "ux0:data/VitaJPOverlay/hooks/profiles.vjhp"
#define VJO_PROFILE_DB_HEADER 16u
#define VJO_PROFILE_DB_ROW 20u
#define VJO_PROFILE_DB_TITLES_MAX 4096u
typedef int (*VjoProfileRead)(void *ud, void *dst, unsigned bytes, uint32_t offset);
/* Size on success, 0 for no matching title, -1 for a malformed or short file. */
int vjo_hook_profile_read(VjoProfileRead read, void *ud, uint32_t size, const char *title, void *dst, unsigned cap);
#endif
