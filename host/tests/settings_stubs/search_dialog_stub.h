#ifndef VJO_TEST_SEARCH_DIALOG_H
#define VJO_TEST_SEARCH_DIALOG_H
#include <stdint.h>
/* The keyboard's reply (what the user typed) and the dialog's last parameters, as the app gave them. */
extern uint16_t vjo_test_ime_reply[129], vjo_test_ime_initial[129], vjo_test_ime_title[64];
extern int vjo_test_ime_button, vjo_test_ime_init_rc, vjo_test_ime_term_rc, vjo_test_ime_result_rc;
extern int vjo_test_ime_calls, vjo_test_ime_terms, vjo_test_ime_updates, vjo_test_ime_loads, vjo_test_ime_unloads;
extern int vjo_test_ime_loaded, vjo_test_ime_load_rc;
extern unsigned vjo_test_ime_max;          /* maxTextLength */
extern uint64_t vjo_test_ime_languages;    /* supportedLanguages */
extern int vjo_test_ime_forced;            /* languagesForced */
/* option (line breaks), textBoxMode and dialogMode of the last keyboard */
extern unsigned vjo_test_ime_option, vjo_test_ime_textbox, vjo_test_ime_mode;
/* Called once per keyboard, before its reply is read (NULL: none): a harness sets the reply there. */
extern void (*vjo_test_ime_on_open)(void);
#endif
