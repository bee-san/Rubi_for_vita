/* Host-only SceAppMgr boundary for the Settings app harness (shadows
 * vita_stubs'): the budget query main.c logs at start, with the SDK's
 * structure (psp2/appmgr.h, 0x88 bytes). The harness answers it. */
#ifndef VJO_TEST_SETTINGS_APPMGR_H
#define VJO_TEST_SETTINGS_APPMGR_H
#include <psp2/host_stubs.h>

typedef struct SceAppMgrBudgetInfo {
    int size; /* must be 0x88 */
    int app_mode;
    int unk0;
    unsigned int total_user_rw_mem;
    unsigned int free_user_rw;
    int extra_mem_allowed;
    int unk1;
    unsigned int total_extra_mem;
    unsigned int free_extra_mem;
    int unk2[2];
    unsigned int total_phycont_mem;
    unsigned int free_phycont_mem;
    int unk3[10];
    unsigned int total_cdram_mem;
    unsigned int free_cdram_mem;
    int reserved[9];
} SceAppMgrBudgetInfo;
typedef char vjo_test_budget_info_is_0x88[sizeof(SceAppMgrBudgetInfo) == 0x88 ? 1 : -1];

int sceAppMgrGetBudgetInfo(SceAppMgrBudgetInfo *info);

#endif
