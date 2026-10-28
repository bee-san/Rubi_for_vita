/* The native keyboard (sceImeDialog) in the Rubi app's own process, bounded: the Games search, and the Look up
 * page's words and sentences (#313). */
#ifndef VJO_SETTINGS_SEARCH_H
#define VJO_SETTINGS_SEARCH_H
#include <stddef.h>
#include <stdint.h>
#define VJO_SEARCH_CHARS 64
#define VJO_SEARCH_BYTES (4 * VJO_SEARCH_CHARS + 1)
/* The longest text a keyboard takes: the Look up page's, a word or a sentence of up to 128 characters. */
#define VJO_IME_CHARS 128
#define VJO_IME_BYTES (4 * VJO_IME_CHARS + 1)
/* Substring of a title or ID: ASCII case-insensitive, Japanese kept as UTF-8. */
int vjo_search_matches(const char *title, const char *id, const char *query);
/* The keyboard, titled title (UTF-16, NUL-terminated), for at most max_chars UTF-16 units (1 to VJO_IME_CHARS),
 * starting from text (UTF-8, cap bytes: at least 4 * max_chars + 1). `languages`: 0 keeps the system's keyboards
 * (the Games search, as before); else every language's keyboard is offered (VitaShell's ime_dialog.c), so Japanese
 * can be typed on a console set to another language. 1 accepted (text: what was typed, trimmed; empty is allowed),
 * 0 cancelled, <0 an SDK error; cancel and error leave text unchanged. draw renders one frame and updates the
 * native dialog, without handling input. */
int vjo_ime_dialog(const uint16_t *title, int max_chars, int languages, char *text, size_t cap, void (*draw)(void));
/* The Games search: vjo_ime_dialog titled "Search games", 64 characters, the system's keyboards. */
int vjo_search_dialog(char query[VJO_SEARCH_BYTES], void (*draw)(void));
#endif
