#ifndef VJO_TEST_IME_DIALOG_H
#define VJO_TEST_IME_DIALOG_H
#include <stdint.h>
#include <string.h>
typedef uint32_t SceUInt32;
typedef uint64_t SceUInt64;
typedef int SceBool;
#ifndef SCE_TRUE
#define SCE_TRUE 1
#endif
typedef enum { SCE_COMMON_DIALOG_STATUS_NONE, SCE_COMMON_DIALOG_STATUS_RUNNING,
               SCE_COMMON_DIALOG_STATUS_FINISHED } SceCommonDialogStatus;
enum { SCE_IME_TYPE_DEFAULT = 0, SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL = 1,
       SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR = 2, SCE_IME_DIALOG_BUTTON_CLOSE = 1,
       SCE_IME_DIALOG_BUTTON_ENTER = 2 };
typedef struct {
    SceUInt64 supportedLanguages;
    SceBool languagesForced;
    uint32_t type, dialogMode, textBoxMode, maxTextLength;
    const uint16_t *title;
    uint16_t *initialText, *inputTextBuffer;
} SceImeDialogParam;
typedef struct { int result, button; } SceImeDialogResult;
static inline void sceImeDialogParamInit(SceImeDialogParam *p) { memset(p, 0, sizeof(*p)); }
int sceImeDialogInit(const SceImeDialogParam *p);
SceCommonDialogStatus sceImeDialogGetStatus(void);
int sceImeDialogGetResult(SceImeDialogResult *r);
int sceImeDialogTerm(void);
#endif
