/* API 31: core/hook_titles.c, the Rubi app's Games tag: Rubi's own entry (CLANNAD), Luna's ported table
 * when core/hook_luna.c is in the build (hooks/luna-port), nothing else. */
#include "acutest.h"
#include <string.h>

#include "../../core/hook_luna.h"
#include "../../core/hook_titles.h"

static void test_rubi_entry_first(void)
{
    TEST_CHECK(vjo_hook_title_listed("PCSG00415") == VJO_HOOK_TITLE_RUBI); /* CLANNAD */
    TEST_CHECK(vjo_hook_title_listed("PCSG0041") == VJO_HOOK_TITLE_NONE);   /* a prefix is not the ID */
    TEST_CHECK(vjo_hook_title_listed("PCSG004150") == VJO_HOOK_TITLE_NONE);
    TEST_CHECK(vjo_hook_title_listed("pcsg00415") == VJO_HOOK_TITLE_NONE);  /* IDs are upper case */
}

static void test_none(void)
{
    TEST_CHECK(vjo_hook_title_listed(NULL) == VJO_HOOK_TITLE_NONE);
    TEST_CHECK(vjo_hook_title_listed("") == VJO_HOOK_TITLE_NONE);
    TEST_CHECK(vjo_hook_title_listed("ZZZZ99999") == VJO_HOOK_TITLE_NONE);
    TEST_CHECK(vjo_hook_title_listed("NPXS10028") == VJO_HOOK_TITLE_NONE); /* Adrenaline: no hooks in this version */
}

static void test_luna_table(void)
{
#if VJO_HOOK_LUNA
    /* every title the table lists, Rubi's own first */
    unsigned n = 0;
    for (unsigned i = 0; vjo_luna_title(i); i++, n++) {
        const char *id = vjo_luna_title(i);
        int want = !strcmp(id, "PCSG00415") ? VJO_HOOK_TITLE_RUBI : VJO_HOOK_TITLE_LUNA;
        TEST_CHECK_(vjo_hook_title_listed(id) == want, "%s", id);
    }
    TEST_CHECK(n > 0);
#else
    TEST_SKIP("core/hook_luna.c is not in this build (hooks/luna-port)");
#endif
}

TEST_LIST = {
    {"Rubi's own entry, exactly", test_rubi_entry_first},
    {"no entry", test_none},
    {"Luna's table", test_luna_table},
    {NULL, NULL},
};
