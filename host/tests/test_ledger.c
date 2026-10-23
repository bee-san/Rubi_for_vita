/* The memory ledger's counters (core/ledger.c) and the static budget table
 * (include/vjo_ledger_budget.h) against the code it describes. */
#include "acutest.h"
#include "../../include/vjo_game_ocr.h"
#include <string.h>
#include "../../core/ledger.h"
#include "../../include/vjo_ledger_budget.h"
#include "../../include/vjo_vocr_sizes.h"
#include "../../core/styled.h"
#include "test_tier_pins.h"
#include "../../core/charmap.h" /* API 37: VJO_CMAP_MAX_BYTES (plain C, no OCR) */
#ifndef VJO_STRIP_MAX
#define VJO_STRIP_MAX 4096 /* shell/shell.h */
#endif
#ifdef VJO_WITH_VOCR
#include "../../core/vocr_ocr.h"
#endif

enum { A = 0, B, C, N };
static const VjoLedgerItemDef defs[N] = {
    [A] = {"a", VJO_LEDGER_POOL_SHELL_PAGES},
    [B] = {"b", VJO_LEDGER_POOL_SHELL_PAGES},
    [C] = {"c", VJO_LEDGER_POOL_KERNEL},
};

static void test_add_sub_and_peaks(void)
{
    VjoLedger l;
    vjo_ledger_init(&l, defs, N);
    TEST_CHECK(l.n == N && vjo_ledger_pool_current(&l, VJO_LEDGER_POOL_SHELL_PAGES) == 0);
    vjo_ledger_add(&l, A, 100);
    vjo_ledger_add(&l, B, 50);
    vjo_ledger_add(&l, C, 7);
    TEST_CHECK(vjo_ledger_current(&l, A) == 100 && vjo_ledger_current(&l, B) == 50 && vjo_ledger_current(&l, C) == 7);
    TEST_CHECK(vjo_ledger_pool_current(&l, VJO_LEDGER_POOL_SHELL_PAGES) == 150);
    TEST_CHECK(vjo_ledger_pool_peak(&l, VJO_LEDGER_POOL_SHELL_PAGES) == 150);
    TEST_CHECK(vjo_ledger_pool_current(&l, VJO_LEDGER_POOL_KERNEL) == 7);
    vjo_ledger_sub(&l, A, 60);
    TEST_CHECK(vjo_ledger_current(&l, A) == 40 && vjo_ledger_peak(&l, A) == 100);
    TEST_CHECK(vjo_ledger_pool_current(&l, VJO_LEDGER_POOL_SHELL_PAGES) == 90 &&
               vjo_ledger_pool_peak(&l, VJO_LEDGER_POOL_SHELL_PAGES) == 150);
    vjo_ledger_add(&l, A, 200); /* a new peak */
    TEST_CHECK(vjo_ledger_current(&l, A) == 240 && vjo_ledger_peak(&l, A) == 240);
    TEST_CHECK(vjo_ledger_pool_peak(&l, VJO_LEDGER_POOL_SHELL_PAGES) == 290);
    /* never below zero, even when a release is larger than what is held */
    vjo_ledger_sub(&l, A, 1000);
    TEST_CHECK(vjo_ledger_current(&l, A) == 0 && vjo_ledger_pool_current(&l, VJO_LEDGER_POOL_SHELL_PAGES) == 50);
    /* zero bytes, unknown items and pools are ignored */
    vjo_ledger_add(&l, B, 0);
    vjo_ledger_add(&l, -1, 5);
    vjo_ledger_add(&l, N, 5);
    vjo_ledger_sub(&l, 99, 5);
    TEST_CHECK(vjo_ledger_current(&l, B) == 50 && vjo_ledger_current(&l, -1) == 0 && vjo_ledger_current(&l, N) == 0);
    TEST_CHECK(vjo_ledger_pool_current(&l, VJO_LEDGER_POOLS) == 0 && vjo_ledger_pool_current(&l, -1) == 0);
    TEST_CHECK(vjo_ledger_pool_current(&l, VJO_LEDGER_POOL_SHELL_PAGES) == 50);
    /* an uninitialised ledger (defs NULL) ignores everything */
    memset(&l, 0, sizeof(l));
    vjo_ledger_add(&l, A, 5);
    TEST_CHECK(vjo_ledger_current(&l, A) == 0 && !vjo_ledger_known(&l, A));
    /* more items than slots: the extra ones are ignored */
    VjoLedgerItemDef many[VJO_LEDGER_MAX_ITEMS + 3] = {{"x", 0}};
    for (int i = 0; i < VJO_LEDGER_MAX_ITEMS + 3; i++) { many[i].name = "x"; many[i].pool = 0; }
    vjo_ledger_init(&l, many, VJO_LEDGER_MAX_ITEMS + 3);
    TEST_CHECK(l.n == VJO_LEDGER_MAX_ITEMS);
    vjo_ledger_add(&l, VJO_LEDGER_MAX_ITEMS, 5);
    TEST_CHECK(vjo_ledger_pool_current(&l, 0) == 0);
    /* a bad pool in a definition is ignored too */
    VjoLedgerItemDef bad[1] = {{"bad", VJO_LEDGER_POOLS}};
    vjo_ledger_init(&l, bad, 1);
    vjo_ledger_add(&l, 0, 5);
    TEST_CHECK(vjo_ledger_current(&l, 0) == 0);
}

/* The table's worst cases are what the budget check enforces; the
 * arithmetic in the header must hold for the C compiler too. */
static void test_budget_table_is_consistent(void)
{
    TEST_CHECK(VJO_WORST_SHELL_PAGES_PERMANENT == VJO_BUDGET_SHELL_MODULE_PAGES + VJO_BUDGET_WORKER_IN_SHELL_PAGES +
                                                  VJO_BUDGET_SHELL_NET_STACK + VJO_BUDGET_SHELL_CTL_STACK);
    TEST_CHECK(VJO_WORST_SHELL_PAGES <= VJO_LIMIT_SHELL_PAGES);
    TEST_CHECK(VJO_WORST_SHELL_PAF <= VJO_LIMIT_SHELL_PAF);
    /* the overlay's text arena holds what overlay.cpp allocates from it:
     * header 4096 units + VJO_HEADER_SPANS (5: an earlier line's dimmed
     * neighbours, line history) spans, body VJO_BODY_UNITS (4096) units +
     * VJO_BODY_SPANS (96) spans, 8-byte aligned */
    {
        uint32_t text = (2u * (4096u + 1u) + 7u) & ~7u, span = (uint32_t)sizeof(VjoSpan);
        uint32_t body = (2u * (VJO_BODY_UNITS + 1u) + 7u) & ~7u;
        uint32_t need = text + ((span * VJO_HEADER_SPANS + 7u) & ~7u) + body + ((span * VJO_BODY_SPANS + 7u) & ~7u);
        /* API 37: and the cursor's copy of the text with its char map (shell/overlay.cpp s_cursor_text) */
        TEST_CHECK_(need == 18016u && need + VJO_HEADER_RANGE_BYTES + VJO_STRIP_MAX <= VJO_BUDGET_PAF_UI, "%u", need);
        TEST_CHECK(VJO_STRIP_MAX >= VJO_CMAP_MAX_BYTES + 1u); /* a char map fits after an empty text, at least */
    }
    TEST_CHECK(VJO_WORST_KERNEL <= VJO_LIMIT_KERNEL);
    TEST_CHECK(VJO_WORST_GAME_JOB <= VJO_LIMIT_GAME_JOB);
    TEST_CHECK(VJO_BUDGET_GAME_WEIGHTS <= VJO_LIMIT_GAME_WEIGHTS && VJO_BUDGET_GAME_WORKSPACE <= VJO_LIMIT_GAME_WORKSPACE);
    /* page-rounded figures are page-rounded */
    TEST_CHECK(VJO_BUDGET_SHELL_MODULE_PAGES % VJO_LEDGER_PAGE == 0 && VJO_BUDGET_WORKER_IN_SHELL_PAGES % VJO_LEDGER_PAGE == 0 &&
               VJO_BUDGET_KERNEL_MODULE_PAGES % VJO_LEDGER_PAGE == 0 && VJO_BUDGET_GAME_WEIGHTS % VJO_LEDGER_PAGE == 0 &&
               VJO_BUDGET_GAME_WORKSPACE % VJO_LEDGER_PAGE == 0 && VJO_BUDGET_KERNEL_OCR_STRIP % VJO_LEDGER_PAGE == 0);
    /* the game weights and cascade budgets are the largest accepted model, the Large size's, page-rounded */
    TEST_CHECK(VJO_BUDGET_GAME_WEIGHTS == (VJO_VOCR_MAX_BYTES + VJO_LEDGER_PAGE - 1) / VJO_LEDGER_PAGE * VJO_LEDGER_PAGE);
    TEST_CHECK(VJO_BUDGET_GAME_CASCADE == VJO_BUDGET_GAME_WEIGHTS && VJO_VOCR_MAX_BYTES == VJO_VOCR_TIER3_BYTES);
    /* each size's model at least the one below's; a size may keep the model of the size below it */
    TEST_CHECK(VJO_VOCR_TIER1_BYTES <= VJO_VOCR_TIER2_BYTES && VJO_VOCR_TIER2_BYTES <= VJO_VOCR_TIER3_BYTES);
    TEST_CHECK(VJO_VOCR_TIER1_BYTES != VJO_VOCR_TIER2_BYTES || !strcmp(VJO_VOCR_TIER1_NAME, VJO_VOCR_TIER2_NAME));
    TEST_CHECK(VJO_VOCR_TIER2_BYTES != VJO_VOCR_TIER3_BYTES || !strcmp(VJO_VOCR_TIER2_NAME, VJO_VOCR_TIER3_NAME));
    /* the Large size's release file (test_tier_pins.h; VOCR5-L's 2,473,696 B: 2,473,984 B), page-rounded, under
     * the 2.5 MB weights limit (the owner's ceiling) */
    TEST_CHECK(VJO_BUDGET_GAME_WEIGHTS == (test_tier_pins[2].bytes + VJO_LEDGER_PAGE - 1) / VJO_LEDGER_PAGE * VJO_LEDGER_PAGE &&
               VJO_LIMIT_GAME_WEIGHTS == 2502656u &&
               VJO_LIMIT_GAME_WEIGHTS == (2500000u + VJO_LEDGER_PAGE - 1) / VJO_LEDGER_PAGE * VJO_LEDGER_PAGE);
    /* the PAF limit is the measured free heap minus the runtime reserve */
    TEST_CHECK(VJO_LIMIT_SHELL_PAF == VJO_PAF_FREE_MEASURED - VJO_PAF_RESERVE && VJO_PAF_FREE_MEASURED < VJO_PAF_HEAP);
}

#ifdef VJO_WITH_VOCR
/* The workspace budget is the real layout code's figure for the largest job of
 * any pinned model at the largest region, with (API 20) the refinement's arena,
 * the LM decoder's extras, the greedy text the game worker keeps, the confidence
 * cascade's extras (game_ocr/main.c extra_job_bytes; cascade_of: a request by
 * name's VOCR4-W20 beside a smaller model, a request by tiers' next tier up), and
 * (API 22) the line slots of the most helpers a job takes. */
static size_t full_job(const char *model, const char *cascade)
{
    /* API 22: with the most helpers a job takes (ocr_threads 3), whose line slots end the workspace */
    return vjo_vocr_job_bytes(model, VJO_OCR_MAX_WIDTH, VJO_OCR_MAX_HEIGHT) +
           vjo_vocr_refine_bytes(VJO_OCR_MAX_WIDTH, VJO_OCR_MAX_HEIGHT) + vjo_vocr_lm_job_bytes(model) +
           VJO_GAME_OCR_TEXT_BYTES + (cascade ? vjo_vocr_cascade_job_bytes(cascade) : 0) +
           vjo_vocr_helpers_bytes(model, VJO_VOCR_HELPERS_MAX);
}
static void test_game_workspace_budget_matches_the_layout(void)
{
    static const char *const tiers[] = {VJO_VOCR_TIER1_NAME, VJO_VOCR_TIER2_NAME, VJO_VOCR_TIER3_NAME};
    unsigned n = 0;
    const VjoVocrModel *all = vjo_vocr_models(&n), *casc = vjo_vocr_model(VJO_VOCR_CASCADE_NAME);
    size_t largest = 0, casc_bytes = vjo_vocr_cascade_job_bytes(VJO_VOCR_CASCADE_NAME);
    const char *which = "", *which_casc = "";
    TEST_ASSERT(casc != NULL && casc_bytes > 0);
    TEST_CHECK(n >= 7); /* every model the game worker accepts: the six before VOCR5 by name, and the sizes' */
    for (unsigned i = 0; i < n; i++) {
        /* a request by name: VOCR4-W20 reads again beside a smaller model */
        const char *c = all[i].bytes < casc->bytes ? casc->name : NULL;
        size_t bytes = full_job(all[i].name, c);
        TEST_CHECK_(bytes > 0, "%s", all[i].name);
        TEST_CHECK(vjo_vocr_job_bytes_helpers(all[i].name, VJO_OCR_MAX_WIDTH, VJO_OCR_MAX_HEIGHT, VJO_VOCR_HELPERS_MAX) ==
                   vjo_vocr_job_bytes(all[i].name, VJO_OCR_MAX_WIDTH, VJO_OCR_MAX_HEIGHT) +
                       vjo_vocr_helpers_bytes(all[i].name, VJO_VOCR_HELPERS_MAX));
        if (bytes > largest) { largest = bytes; which = all[i].name; which_casc = c ? c : "-"; }
        /* every model's weights block, and as a cascade its block, within the two budgets */
        TEST_CHECK((all[i].bytes + VJO_LEDGER_PAGE - 1) / VJO_LEDGER_PAGE * VJO_LEDGER_PAGE <= VJO_BUDGET_GAME_WEIGHTS);
        TEST_CHECK((all[i].bytes + VJO_LEDGER_PAGE - 1) / VJO_LEDGER_PAGE * VJO_LEDGER_PAGE <= VJO_BUDGET_GAME_CASCADE);
    }
    /* a request by tiers: each size reads again with the next size up (Large with none), when that
     * size's model is larger (game_ocr/main.c cascade_of: a size that kept the model of the size
     * below it is no cascade) */
    for (unsigned t = 0; t < 3; t++) {
        const char *c = t + 1 < 3 && vjo_vocr_model(tiers[t + 1])->bytes > vjo_vocr_model(tiers[t])->bytes ? tiers[t + 1]
                                                                                                         : NULL;
        size_t bytes = full_job(tiers[t], c);
        TEST_CHECK_(bytes > 0, "%s", tiers[t]);
        if (bytes > largest) { largest = bytes; which = tiers[t]; which_casc = c ? c : "-"; }
    }
    TEST_CHECK_((largest + VJO_LEDGER_PAGE - 1) / VJO_LEDGER_PAGE * VJO_LEDGER_PAGE == VJO_BUDGET_GAME_WORKSPACE,
                "largest full-screen job %zu B (%s, cascade %s)", largest, which, which_casc);
    TEST_MSG("largest full-screen job %zu B: %s with the cascade %s", largest, which, which_casc);
}

#endif

TEST_LIST = {
    {"add_sub_and_peaks", test_add_sub_and_peaks},
    {"budget_table_is_consistent", test_budget_table_is_consistent},
#ifdef VJO_WITH_VOCR
    {"game_workspace_budget_matches_the_layout", test_game_workspace_budget_matches_the_layout},
#endif
    {NULL, NULL}
};
