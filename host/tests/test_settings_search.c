#include "acutest.h"
#include "../../settings/search.h"
#include "settings_stubs/search_dialog_stub.h"
#include <psp2/ime_dialog.h>
#include <vita2d.h>
#include <string.h>
#include "../../core/utf.h"

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

/* The Games search keeps its keyboard: 64 characters, the system's keyboards. */
static void the_games_search_keeps_its_keyboard(void)
{
    char query[VJO_SEARCH_BYTES] = "";
    vjo_test_ime_button = SCE_IME_DIALOG_BUTTON_CLOSE;
    TEST_CHECK(vjo_search_dialog(query, frame) == 0);
    TEST_CHECK(vjo_test_ime_max == 64 && vjo_test_ime_languages == 0 && vjo_test_ime_forced == 0);
    static const uint16_t title[] = {'S','e','a','r','c','h',' ','g','a','m','e','s',0};
    TEST_CHECK(!memcmp(vjo_test_ime_title, title, sizeof(title)));
}

/* #313's Look up keyboard: 128 characters, every language's keyboard offered (VitaShell's 0x1FFFF, forced); its
 * whole reply taken (a pair at its end too), the initial text cut at 128 units without splitting a pair. Bad
 * arguments open nothing. */
static void the_look_up_keyboard(void)
{
    static const uint16_t title[] = {'L','o','o','k',' ','u','p',0};
    char text[VJO_IME_BYTES], small[64];
    int calls = vjo_test_ime_calls;
    /* 127 kanji and a surrogate pair: 129 units, cut before the pair */
    size_t n = 0;
    for (int i = 0; i < 127; i++) n += (size_t)vjo_utf8_encode(0x4E00 + (uint32_t)i, text + n);
    n += (size_t)vjo_utf8_encode(0x20BB7, text + n);
    text[n] = 0;
    vjo_test_ime_button = SCE_IME_DIALOG_BUTTON_CLOSE;
    TEST_CHECK(vjo_ime_dialog(title, VJO_IME_CHARS, 1, text, sizeof(text), frame) == 0);
    TEST_CHECK(vjo_test_ime_max == 128 && vjo_test_ime_languages == 0x1FFFF && vjo_test_ime_forced == 1);
    TEST_CHECK(vjo_test_ime_initial[126] == 0x4E7E && vjo_test_ime_initial[127] == 0); /* no half pair */
    TEST_CHECK(!memcmp(vjo_test_ime_title, title, sizeof(title)) && strlen(text) == n); /* cancelled: unchanged */
    /* the reply: 126 kanji and a pair, 128 units, all of it */
    for (int i = 0; i < 126; i++) vjo_test_ime_reply[i] = (uint16_t)(0x4E00 + i);
    vjo_test_ime_reply[126] = 0xD842, vjo_test_ime_reply[127] = 0xDFB7, vjo_test_ime_reply[128] = 0;
    vjo_test_ime_button = SCE_IME_DIALOG_BUTTON_ENTER;
    TEST_CHECK(vjo_ime_dialog(title, VJO_IME_CHARS, 1, text, sizeof(text), frame) == 1);
    TEST_CHECK(strlen(text) == 126 * 3 + 4 && !strcmp(text + 126 * 3, "𠮷"));
    /* bad arguments: nothing opened, the text as it was */
    TEST_CHECK(vjo_ime_dialog(title, 0, 1, text, sizeof(text), frame) == -1);
    TEST_CHECK(vjo_ime_dialog(title, VJO_IME_CHARS + 1, 1, text, sizeof(text), frame) == -1);
    TEST_CHECK(vjo_ime_dialog(title, 64, 1, small, sizeof(small), frame) == -1);
    TEST_CHECK(vjo_test_ime_calls == calls + 2);
}

TEST_LIST = {
    {"the_games_search_keeps_its_keyboard", the_games_search_keeps_its_keyboard},
    {"the_look_up_keyboard", the_look_up_keyboard},
    {"title_and_id_matching", title_and_id_matching},
    {"native_keyboard_preserves_unicode_and_balances_its_module", native_keyboard_preserves_unicode_and_balances_its_module},
    {"cancel_and_sdk_errors_keep_the_query", cancel_and_sdk_errors_keep_the_query},
    {NULL, NULL}
};
