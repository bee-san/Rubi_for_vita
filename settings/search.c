#include "search.h"
#include "../core/utf.h"
#include <psp2/ime_dialog.h>
#include <psp2/sysmodule.h>
#include <string.h>

static int contains(const char *text, const char *query)
{
    for (; *text; text++)
        if (vjo_ieq_prefix(text, query)) return 1;
    return !*query;
}

int vjo_search_matches(const char *title, const char *id, const char *query)
{ return contains(title, query) || contains(id, query); }

/* text as at most max UTF-16 units (a character that would not fit whole is left out), NUL-ended. */
static void to_utf16(const char *text, uint16_t *out, unsigned max)
{
    size_t at = 0, len = strlen(text);
    unsigned n = 0;
    while (at < len) {
        uint32_t cp = vjo_utf8_next(text, len, &at);
        if (n + (unsigned)vjo_utf16_units(cp) > max) break;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out[n++] = (uint16_t)(0xD800 + (cp >> 10));
            out[n++] = (uint16_t)(0xDC00 + (cp & 1023));
        } else out[n++] = (uint16_t)cp;
    }
    out[n] = 0;
}

/* At most max units of text as UTF-8 into out (cap bytes, NUL-ended; a character that would not fit
 * is left out, with the rest). */
static void from_utf16(const uint16_t *text, unsigned max, char *out, size_t cap)
{
    size_t used = 0;
    for (unsigned i = 0; i < max && text[i]; i++) {
        uint32_t cp = text[i];
        char bytes[4];
        int n;
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < max &&
            text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + text[++i] - 0xDC00;
        } else if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
        n = vjo_utf8_encode(cp, bytes);
        if (used + (size_t)n + 1 > cap) break;
        memcpy(out + used, bytes, (size_t)n);
        used += (size_t)n;
    }
    out[used] = 0;
}

int vjo_ime_dialog(const uint16_t *title, int max_chars, int languages, char *text, size_t cap, void (*draw)(void))
{
    uint16_t initial[VJO_IME_CHARS + 1] = {0}, input[VJO_IME_CHARS + 1] = {0};
    char typed[VJO_IME_BYTES];
    SceImeDialogParam param;
    SceImeDialogResult result;
    int own_module, rc;
    if (max_chars < 1 || max_chars > VJO_IME_CHARS || cap < (size_t)(4 * max_chars + 1))
        return -1;
    own_module = sceSysmoduleIsLoaded(SCE_SYSMODULE_IME) < 0;
    if (own_module && (rc = sceSysmoduleLoadModule(SCE_SYSMODULE_IME)) < 0) return rc;
    to_utf16(text, initial, (unsigned)max_chars);
    sceImeDialogParamInit(&param);
    if (languages) {
        /* every keyboard, whatever the system settings enable (VitaShell's ime_dialog.c does the same) */
        param.supportedLanguages = 0x0001FFFF;
        param.languagesForced = SCE_TRUE;
    }
    param.type = SCE_IME_TYPE_DEFAULT;
    param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
    param.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    param.title = title;
    param.maxTextLength = (SceUInt32)max_chars;
    param.initialText = initial;
    param.inputTextBuffer = input;
    rc = sceImeDialogInit(&param);
    if (rc >= 0) {
        SceCommonDialogStatus state;
        while ((state = sceImeDialogGetStatus()) == SCE_COMMON_DIALOG_STATUS_RUNNING) draw();
        memset(&result, 0, sizeof(result));
        rc = state == SCE_COMMON_DIALOG_STATUS_FINISHED ? sceImeDialogGetResult(&result) : -1;
        if (rc >= 0 && result.result < 0) rc = result.result;
        int ended = sceImeDialogTerm();
        if (rc >= 0 && ended < 0) rc = ended;
        if (rc >= 0) {
            rc = result.button == SCE_IME_DIALOG_BUTTON_ENTER;
            if (rc) {
                from_utf16(input, (unsigned)max_chars, typed, sizeof(typed));
                char *trimmed = vjo_java_trim(typed);
                memmove(text, trimmed, strlen(trimmed) + 1);
            }
        }
    }
    if (own_module) sceSysmoduleUnloadModule(SCE_SYSMODULE_IME);
    return rc;
}

int vjo_text_dialog(const uint16_t *title, const char *initial, char *out, size_t cap, unsigned max_units,
                    int multiline, uint16_t *initial_buf, uint16_t *input_buf, void (*draw)(void))
{
    SceImeDialogParam param;
    SceImeDialogResult result;
    int own_module = sceSysmoduleIsLoaded(SCE_SYSMODULE_IME) < 0, rc;
    if (own_module && (rc = sceSysmoduleLoadModule(SCE_SYSMODULE_IME)) < 0) return rc;
    to_utf16(initial, initial_buf, max_units);
    memset(input_buf, 0, sizeof(uint16_t) * (max_units + 1));
    sceImeDialogParamInit(&param);
    /* Japanese to edit: every keyboard, whatever the system settings enable (as vjo_ime_dialog's languages) */
    param.supportedLanguages = 0x0001FFFF;
    param.languagesForced = SCE_TRUE;
    param.type = SCE_IME_TYPE_DEFAULT;
    /* Several lines (the Cards page's sentence) as VitaShell's text editor opens them (VitaShell ime_dialog.c:
     * initImeDialog): a cancel button, the default text box; there Enter types a line break, and the dialog's
     * done button comes back as CLOSE (updateImeDialog). One line, as the games search always had it. */
    if (multiline) param.option = SCE_IME_OPTION_MULTILINE;
    param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
    param.textBoxMode = multiline ? SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT : SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    param.title = title;
    param.maxTextLength = max_units;
    param.initialText = initial_buf;
    param.inputTextBuffer = input_buf;
    rc = sceImeDialogInit(&param);
    if (rc >= 0) {
        SceCommonDialogStatus state;
        while ((state = sceImeDialogGetStatus()) == SCE_COMMON_DIALOG_STATUS_RUNNING) draw();
        memset(&result, 0, sizeof(result));
        rc = state == SCE_COMMON_DIALOG_STATUS_FINISHED ? sceImeDialogGetResult(&result) : -1;
        if (rc >= 0 && result.result < 0) rc = result.result;
        int ended = sceImeDialogTerm();
        if (rc >= 0 && ended < 0) rc = ended;
        if (rc >= 0) {
            rc = result.button == (multiline ? SCE_IME_DIALOG_BUTTON_CLOSE : SCE_IME_DIALOG_BUTTON_ENTER);
            if (rc) from_utf16(input_buf, max_units, out, cap);
        }
    }
    if (own_module) sceSysmoduleUnloadModule(SCE_SYSMODULE_IME);
    return rc;
}

int vjo_search_dialog(char query[VJO_SEARCH_BYTES], void (*draw)(void))
{
    static const uint16_t title[] = {'S','e','a','r','c','h',' ','g','a','m','e','s',0};
    return vjo_ime_dialog(title, VJO_SEARCH_CHARS, 0, query, VJO_SEARCH_BYTES, draw);
}
