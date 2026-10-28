/* SDK boundary only: the actual UTF conversion, match and dialog lifecycle are settings/search.c. */
#include "search_dialog_stub.h"
#include <psp2/ime_dialog.h>
#include <psp2/sysmodule.h>
#include <string.h>
uint16_t vjo_test_ime_reply[129], vjo_test_ime_initial[129], vjo_test_ime_title[64];
int vjo_test_ime_button = SCE_IME_DIALOG_BUTTON_CLOSE, vjo_test_ime_init_rc, vjo_test_ime_term_rc, vjo_test_ime_result_rc;
int vjo_test_ime_calls, vjo_test_ime_terms, vjo_test_ime_updates, vjo_test_ime_loads, vjo_test_ime_unloads;
int vjo_test_ime_loaded, vjo_test_ime_load_rc;
unsigned vjo_test_ime_max;
uint64_t vjo_test_ime_languages;
int vjo_test_ime_forced;
unsigned vjo_test_ime_option, vjo_test_ime_textbox, vjo_test_ime_mode;
void (*vjo_test_ime_on_open)(void);
static uint16_t *input;
static int polls;
int sceSysmoduleIsLoaded(unsigned short id) { (void)id; return vjo_test_ime_loaded ? 0 : -1; }
int sceSysmoduleLoadModule(unsigned short id) { (void)id; vjo_test_ime_loads++; return vjo_test_ime_load_rc; }
int sceSysmoduleUnloadModule(unsigned short id) { (void)id; vjo_test_ime_unloads++; return 0; }
int sceImeDialogInit(const SceImeDialogParam *p)
{
    unsigned n = 0;
    vjo_test_ime_calls++;
    polls = 0;
    input = p->inputTextBuffer;
    vjo_test_ime_max = p->maxTextLength;
    vjo_test_ime_languages = p->supportedLanguages;
    vjo_test_ime_forced = p->languagesForced;
    vjo_test_ime_option = p->option;
    vjo_test_ime_textbox = p->textBoxMode;
    vjo_test_ime_mode = p->dialogMode;
    memset(vjo_test_ime_initial, 0, sizeof(vjo_test_ime_initial));
    for (; n <= p->maxTextLength && n < 129; n++) /* the initial text and its NUL: never past the app's buffer */
        if (!(vjo_test_ime_initial[n] = p->initialText[n])) break;
    memset(vjo_test_ime_title, 0, sizeof(vjo_test_ime_title));
    for (n = 0; n + 1 < 64 && p->title && p->title[n]; n++)
        vjo_test_ime_title[n] = p->title[n];
    if (vjo_test_ime_on_open) vjo_test_ime_on_open();
    /* the SDK's range (SCE_IME_DIALOG_MAX_TEXT_LENGTH, 2,048): the Look up page's 128 */
    return p->maxTextLength >= 1 && p->maxTextLength <= 2048 ? vjo_test_ime_init_rc : -1;
}
SceCommonDialogStatus sceImeDialogGetStatus(void)
{ return polls++ ? SCE_COMMON_DIALOG_STATUS_FINISHED : SCE_COMMON_DIALOG_STATUS_RUNNING; }
int sceImeDialogGetResult(SceImeDialogResult *r)
{
    /* the reply as the keyboard writes it: at most maxTextLength units and a NUL */
    unsigned n = 0;
    for (; n < vjo_test_ime_max && n < 128 && vjo_test_ime_reply[n]; n++) input[n] = vjo_test_ime_reply[n];
    input[n] = 0;
    r->button = vjo_test_ime_button;
    return vjo_test_ime_result_rc;
}
int sceImeDialogTerm(void) { vjo_test_ime_terms++; return vjo_test_ime_term_rc; }
int vita2d_common_dialog_update(void) { vjo_test_ime_updates++; return 0; }
