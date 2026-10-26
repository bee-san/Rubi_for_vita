/* API 31: core/hook_titles.c, the Rubi app's Games tag: Rubi's own entries (the worker's verified table,
 * core/hook_place.c), a title with a ported site in Luna's table when core/hook_luna.c is in the build
 * (hooks/luna-port), nothing else. */
#include "acutest.h"
#include <string.h>

#include "../../core/hook_luna.h"
#include "../../core/hook_place.h"
#include "../../core/hook_titles.h"

static void test_rubi_entry_first(void)
{
    TEST_CHECK(vjo_hook_title_listed("PCSG00415") == VJO_HOOK_TITLE_RUBI); /* CLANNAD */
    TEST_CHECK(vjo_hook_title_listed("PCSG0041") == VJO_HOOK_TITLE_NONE);   /* a prefix is not the ID */
    TEST_CHECK(vjo_hook_title_listed("PCSG004150") == VJO_HOOK_TITLE_NONE);
    TEST_CHECK(vjo_hook_title_listed("pcsg00415") == VJO_HOOK_TITLE_NONE);  /* IDs are upper case */
}

/* Every title of the worker's verified table is tagged as Rubi's own, before Luna's table: the entries
 * #273 and #293-#296 added were missing from the tag's former copy of the list (CLANNAD alone), so 10 of
 * them had no tag and 17 a Luna one. */
static void test_every_verified_entry(void)
{
    const VjoPlaceVerified *all;
    unsigned n = vjo_place_verified_all(&all);
#if VJO_HOOK_LUNA
    unsigned only_rubi = 0;
#endif
    TEST_CHECK(n >= 28u);
    for (unsigned i = 0; i < n; i++) {
        TEST_CHECK_(vjo_hook_title_listed(all[i].title) == VJO_HOOK_TITLE_RUBI, "%s", all[i].title);
#if VJO_HOOK_LUNA
        only_rubi += !vjo_luna_title_known(all[i].title);
#endif
    }
    /* ones Rubi alone covers, and ones also in Luna's table (Rubi's tag wins) */
    TEST_CHECK(vjo_hook_title_listed("PCSG00007") == VJO_HOOK_TITLE_RUBI); /* リトルバスターズ！ CE */
    TEST_CHECK(vjo_hook_title_listed("PCSG00206") == VJO_HOOK_TITLE_RUBI); /* グリザイアの果実 */
    TEST_CHECK(vjo_hook_title_listed("PCSG00553") == VJO_HOOK_TITLE_RUBI); /* 剣が君 for V */
    TEST_CHECK(vjo_hook_title_listed("PCSG01198") == VJO_HOOK_TITLE_RUBI); /* アマツツミ */
    TEST_CHECK(vjo_hook_title_listed("PCSG00469") == VJO_HOOK_TITLE_RUBI); /* CLOCK ZERO ExTime */
    TEST_CHECK(vjo_hook_title_listed("PCSG00628") == VJO_HOOK_TITLE_RUBI); /* ALIA's CARNIVAL! サクラメント */
    TEST_CHECK(vjo_hook_title_listed("PCSG00380") == VJO_HOOK_TITLE_RUBI); /* 真 流行り神 */
#if VJO_HOOK_LUNA
    TEST_CHECK_(only_rubi == 13u, "%u verified titles are not in Luna's table (CLANNAD and 12 of the farm's)",
                only_rubi);
    TEST_CHECK(vjo_luna_title_ported("PCSG01198") && vjo_luna_title_ported("PCSG00469"));
#endif
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
    /* every title the table lists with a ported site, Rubi's own first; one whose every site is unported has
     * no tag (audit-hooks-text N2: the 7 of docs/guide.md) */
    static const char *const unported[] = {"PCSB00985", "PCSG00291", "PCSG00451", "PCSG00595",
                                           "PCSG00663", "PCSG01081", "PCSG01152"};
    unsigned n = 0, tagged = 0;
    for (unsigned i = 0; vjo_luna_title(i); i++, n++) {
        const char *id = vjo_luna_title(i);
        int want = vjo_place_title_verified(id) ? VJO_HOOK_TITLE_RUBI
                   : vjo_luna_title_ported(id)  ? VJO_HOOK_TITLE_LUNA
                                                : VJO_HOOK_TITLE_NONE;
        TEST_CHECK_(vjo_hook_title_listed(id) == want, "%s", id);
        tagged += want != VJO_HOOK_TITLE_NONE;
    }
    TEST_CHECK(n > 0 && tagged == n - 7u);
    for (unsigned i = 0; i < 7; i++)
        TEST_CHECK_(vjo_luna_title_known(unported[i]) && vjo_hook_title_listed(unported[i]) == VJO_HOOK_TITLE_NONE,
                    "%s: no tag", unported[i]);
    TEST_CHECK(vjo_hook_title_listed("PCSG00543") == VJO_HOOK_TITLE_LUNA);
#else
    TEST_SKIP("core/hook_luna.c is not in this build (hooks/luna-port)");
#endif
}

TEST_LIST = {
    {"Rubi's own entry, exactly", test_rubi_entry_first},
    {"every verified entry is Rubi's own", test_every_verified_entry},
    {"no entry", test_none},
    {"Luna's table", test_luna_table},
    {NULL, NULL},
};
