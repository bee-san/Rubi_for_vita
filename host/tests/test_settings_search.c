#include "acutest.h"
#include "../../settings/search.h"
#include "settings_stubs/search_dialog_stub.h"
#include <psp2/ime_dialog.h>
#include <vita2d.h>
#include <string.h>

static void frame(void) { vita2d_common_dialog_update(); }

static void title_and_id_matching(void)
{
    TEST_CHECK(vjo_search_matches("CLANNAD", "PCSG00415", "clann"));
    TEST_CHECK(vjo_search_matches("CLANNAD", "PCSG00415", "pcsg00415"));
    TEST_CHECK(vjo_search_matches("蒼の彼方のフォーリズム", "PCSG00633", "彼方"));
    TEST_CHECK(!vjo_search_matches("CLANNAD", "PCSG00415", "AIR"));
    TEST_CHECK(vjo_search_matches("CLANNAD", "PCSG00415", ""));
}

static void native_keyboard_preserves_unicode_and_balances_its_module(void)
{
    char query[VJO_SEARCH_BYTES] = "彼方𠮷";
    const uint16_t answer[] = {' ',0x65E5,0x672C,0xD842,0xDFB7,' ',0};
    memcpy(vjo_test_ime_reply, answer, sizeof(answer));
    vjo_test_ime_button = SCE_IME_DIALOG_BUTTON_ENTER;
    TEST_CHECK(vjo_search_dialog(query, frame) == 1);
    TEST_CHECK(!strcmp(query, "日本𠮷"));
    TEST_CHECK(vjo_test_ime_initial[0] == 0x5F7C && vjo_test_ime_initial[1] == 0x65B9 &&
               vjo_test_ime_initial[2] == 0xD842 && vjo_test_ime_initial[3] == 0xDFB7);
    TEST_CHECK(vjo_test_ime_loads == 1 && vjo_test_ime_unloads == 1 && vjo_test_ime_terms == 1 && vjo_test_ime_updates == 1);
}

static void cancel_and_sdk_errors_keep_the_query(void)
{
    char query[VJO_SEARCH_BYTES] = "CLANNAD";
    TEST_CHECK(vjo_search_dialog(query, frame) == 0 && !strcmp(query, "CLANNAD"));
    vjo_test_ime_init_rc = -123;
    TEST_CHECK(vjo_search_dialog(query, frame) == -123 && !strcmp(query, "CLANNAD"));
    TEST_CHECK(vjo_test_ime_calls == 2 && vjo_test_ime_terms == 1 && vjo_test_ime_unloads == 2);
    vjo_test_ime_init_rc = 0;
    vjo_test_ime_button = SCE_IME_DIALOG_BUTTON_ENTER;
    vjo_test_ime_term_rc = -124;
    TEST_CHECK(vjo_search_dialog(query, frame) == -124 && !strcmp(query, "CLANNAD"));
    vjo_test_ime_loaded = 1;
    vjo_test_ime_term_rc = 0;
    TEST_CHECK(vjo_search_dialog(query, frame) == 1 && !query[0]); /* accepting empty clears */
    TEST_CHECK(vjo_test_ime_loads == 3 && vjo_test_ime_unloads == 3); /* borrowed module stays loaded */
}

TEST_LIST = {
    {"title_and_id_matching", title_and_id_matching},
    {"native_keyboard_preserves_unicode_and_balances_its_module", native_keyboard_preserves_unicode_and_balances_its_module},
    {"cancel_and_sdk_errors_keep_the_query", cancel_and_sdk_errors_keep_the_query},
    {NULL, NULL}
};
