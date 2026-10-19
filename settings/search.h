/* Games search: a bounded native keyboard in the Rubi app's own process. */
#ifndef VJO_SETTINGS_SEARCH_H
#define VJO_SETTINGS_SEARCH_H
#define VJO_SEARCH_CHARS 64
#define VJO_SEARCH_BYTES (4 * VJO_SEARCH_CHARS + 1)
/* Substring of a title or ID: ASCII case-insensitive, Japanese kept as UTF-8. */
int vjo_search_matches(const char *title, const char *id, const char *query);
/* 1 accepted (empty clears), 0 cancelled, <0 SDK error. Cancel/error leaves query unchanged.
 * draw renders one frame and updates the native dialog, without handling game-list input. */
int vjo_search_dialog(char query[VJO_SEARCH_BYTES], void (*draw)(void));
#endif
