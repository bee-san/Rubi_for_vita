/* SDK boundary only: the actual UTF conversion, match and dialog lifecycle are settings/search.c. */
#include "search_dialog_stub.h"
#include <psp2/ime_dialog.h>
#include <psp2/sysmodule.h>
#include <string.h>
uint16_t vjo_test_ime_reply[65], vjo_test_ime_initial[65];
int vjo_test_ime_button = SCE_IME_DIALOG_BUTTON_CLOSE, vjo_test_ime_init_rc, vjo_test_ime_term_rc, vjo_test_ime_result_rc;
int vjo_test_ime_calls, vjo_test_ime_terms, vjo_test_ime_updates, vjo_test_ime_loads, vjo_test_ime_unloads;
int vjo_test_ime_loaded, vjo_test_ime_load_rc;
static uint16_t *input;
static int polls;
int sceSysmoduleIsLoaded(unsigned short id) { (void)id; return vjo_test_ime_loaded ? 0 : -1; }
int sceSysmoduleLoadModule(unsigned short id) { (void)id; vjo_test_ime_loads++; return vjo_test_ime_load_rc; }
int sceSysmoduleUnloadModule(unsigned short id) { (void)id; vjo_test_ime_unloads++; return 0; }
int sceImeDialogInit(const SceImeDialogParam *p)
{
    vjo_test_ime_calls++;
    polls = 0;
    input = p->inputTextBuffer;
    memcpy(vjo_test_ime_initial, p->initialText, sizeof(vjo_test_ime_initial));
    return p->maxTextLength == 64 ? vjo_test_ime_init_rc : -1;
}
SceCommonDialogStatus sceImeDialogGetStatus(void)
{ return polls++ ? SCE_COMMON_DIALOG_STATUS_FINISHED : SCE_COMMON_DIALOG_STATUS_RUNNING; }
int sceImeDialogGetResult(SceImeDialogResult *r)
{
    memcpy(input, vjo_test_ime_reply, sizeof(vjo_test_ime_reply));
    r->button = vjo_test_ime_button;
    return vjo_test_ime_result_rc;
}
int sceImeDialogTerm(void) { vjo_test_ime_terms++; return vjo_test_ime_term_rc; }
int vita2d_common_dialog_update(void) { vjo_test_ime_updates++; return 0; }
