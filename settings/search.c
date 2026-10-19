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

static void to_utf16(const char *text, uint16_t out[VJO_SEARCH_CHARS + 1])
{
    size_t at = 0, len = strlen(text);
    int n = 0;
    while (at < len) {
        uint32_t cp = vjo_utf8_next(text, len, &at);
        if (n + vjo_utf16_units(cp) > VJO_SEARCH_CHARS) break;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out[n++] = (uint16_t)(0xD800 + (cp >> 10));
            out[n++] = (uint16_t)(0xDC00 + (cp & 1023));
        } else out[n++] = (uint16_t)cp;
    }
    out[n] = 0;
}

static void from_utf16(const uint16_t *text, char out[VJO_SEARCH_BYTES])
{
    size_t used = 0;
    for (int i = 0; i < VJO_SEARCH_CHARS && text[i]; i++) {
        uint32_t cp = text[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < VJO_SEARCH_CHARS &&
            text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + text[++i] - 0xDC00;
        } else if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
        used += (size_t)vjo_utf8_encode(cp, out + used);
    }
    out[used] = 0;
}

int vjo_search_dialog(char query[VJO_SEARCH_BYTES], void (*draw)(void))
{
    uint16_t initial[VJO_SEARCH_CHARS + 1] = {0}, input[VJO_SEARCH_CHARS + 1] = {0};
    static const uint16_t title[] = {'S','e','a','r','c','h',' ','g','a','m','e','s',0};
    SceImeDialogParam param;
    SceImeDialogResult result;
    int own_module = sceSysmoduleIsLoaded(SCE_SYSMODULE_IME) < 0, rc;
    if (own_module && (rc = sceSysmoduleLoadModule(SCE_SYSMODULE_IME)) < 0) return rc;
    to_utf16(query, initial);
    sceImeDialogParamInit(&param);
    param.type = SCE_IME_TYPE_DEFAULT;
    param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
    param.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    param.title = title;
    param.maxTextLength = VJO_SEARCH_CHARS;
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
                from_utf16(input, query);
                char *trimmed = vjo_java_trim(query);
                memmove(query, trimmed, strlen(trimmed) + 1);
            }
        }
    }
    if (own_module) sceSysmoduleUnloadModule(SCE_SYSMODULE_IME);
    return rc;
}
