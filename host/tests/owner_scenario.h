/* The owner scenario (#528), the Shell's part: Rubi's Paf requests replayed against the Paf heap the owner's console
 * logged (tools/owner_scenario.txt, made from his log.txt by tools/owner_scenario.py --extract; docs/owner-scenario.md).
 * Included by host/tests/test_shell_worker.c in its Paf build (VJO_PAF_ALLOC), after its stubs, whose Paf heap answers
 * through the Shell's own guard (shell/paf_guard.h) while scenario_on is set.
 *
 * The heap: 5,242,880 B (VJO_PAF_HEAP). At each event the log showed FREE bytes free while 0.10.1 held HELD bytes
 * (the ledger's current; the plugin's 8 KiB at a refusal), so others held VJO_PAF_HEAP - FREE - HELD: that stays
 * until the next event, and this build's requests see what is left beside what it holds itself. Rubi's own steps are
 * its own code: the overlay page's text arena (shell/overlay_mem.h, as overlay.cpp's open_page and close_page take
 * and give it), the lookup's result arena (shell/worker.c mem_alloc, and the worker's own RESULT_MEM_* steps for
 * when it goes back: a job's start and run, a game's exit, the idle rule) and an Anki save's arena (shell/anki.c's
 * request, VJO_BUDGET_PAF_ANKI, through the same heap).
 * The page's own Paf, which PageOpen takes outside Rubi's ledger, is SCENARIO_PAGE_PAF while it is open.
 *
 * Events, one a line: boot|start|exit FREE HELD (at boot the Shell's start step: shell/overlay_mem.h
 * vjo_overlay_mem_boot, #528; at a start or an exit no game is in front: on_game_exit's step and the idle rule),
 * press FREE HELD LOOKUP (the toggle: the page opens, the cursor's word is looked up, and it closes; LOOKUP 1: 0.10.1's
 * log has the press's own job asking for its arena, so the press runs a job: its start and run steps first), lookup
 * FREE HELD (a background read's lookup: its job's start and run steps, then the arena after OCR), anki FREE HELD (a
 * press that saved a card). silent = refused - told: refusals no open overlay showed. Each scenario prints one
 * `owner-scenario shell NAME: key=value ...` line; tools/owner_scenario.py
 * holds them against tools/owner_scenario.json. The checks here hold for any policy: every refusal is logged, the
 * overlay that did not open says so, and a scenario gives back all it took but the overlay's text block, which is
 * the plugin's from its start (#528). */
#ifndef OWNER_SCENARIO_FILE
#define OWNER_SCENARIO_FILE VJO_SOURCE_DIR "/tools/owner_scenario.txt"
#endif

typedef struct {
    unsigned presses, not_opened, page_failed, lookups, refused, told, anki_ok, anki_refused;
    uint32_t peak, held;
} ScenarioCounts;

static uint8_t *scenario_ui; /* the open page's text arena: overlay.cpp's s_ui_mem */

/* The page itself (overlay.cpp open_page's PageOpen): Paf's own blocks for its widgets, outside Rubi's ledger, while
 * it is open: 94,800 B in the median of the 81 opens of the owner's logs where Rubi allocated again before the page
 * closed, 103,296 B the most (Mmem-paf-3, reports/mem/findings.md) [measured, log]: VJO_PAF_PAGE_MAX. The scenario
 * takes the most: PageOpen fails when the heap has less free (overlay.cpp then logs it), which the page check before
 * it (shell/paf_guard.h vjo_paf_page_fits, with SceShell's margin) should never let happen. */
#define SCENARIO_PAGE_PAF VJO_PAF_PAGE_MAX

static int64_t scenario_free(void)
{
    int64_t avail = (int64_t)VJO_PAF_HEAP - scenario_others -
                    (int64_t)vjo_ledger_pool_current(&g_shell_ledger, VJO_LEDGER_POOL_SHELL_PAF);
    return avail > 0 ? avail : 0;
}

static const char *scenario_last_log(void) { return log_count ? log_lines[(log_count - 1u) % 32u] : ""; }

static void scenario_level(uint32_t free_logged, uint32_t held_logged)
{
    scenario_others = (int64_t)VJO_PAF_HEAP - (int64_t)free_logged - (int64_t)held_logged;
    if (scenario_others < 0) scenario_others = 0;
}

/* No game in front (it left, or the next has not started): the control thread's idle rule (shell/worker.c
 * control_tick), as the worker sees it then: no title, no job, the overlay closed. */
static void scenario_no_game(void)
{
    title_id[0] = '\0';
    job_running = 0;
    RESULT_MEM_IDLE();
}

/* A user's job, neither a refresh nor the self-test's (the normal build has no self-test: job_selftest is 0 there). */
static void scenario_user_job(void)
{
    job_refresh = 0;
#if VJO_DEBUG
    job_selftest = 0;
#endif
}

static void scenario_between_jobs(ScenarioCounts *c)
{
    uint32_t held = vjo_ledger_pool_current(&g_shell_ledger, VJO_LEDGER_POOL_SHELL_PAF);
    if (held > c->held) c->held = held;
}

/* The lookup's result arena, as a job asks for it (shell/worker.c mem_alloc): counted, and its refusal logged. */
static int scenario_arena(ScenarioCounts *c)
{
    if (has_result_memory()) return 0; /* the arena the worker holds serves the lookup */
    c->lookups++;
    if (mem_alloc() == 0) return 0;
    c->refused++;
    TEST_CHECK_(strstr(scenario_last_log(), "result workspace Paf allocation failed") != NULL, "refusal logged: %s",
                scenario_last_log());
    return -1;
}

static void scenario_press(ScenarioCounts *c, int lookup_logged, int anki)
{
    uint8_t *ui = vjo_overlay_mem_open(&scenario_ui); /* overlay.cpp open_page */
    int page = 0;
    c->presses++;
    if (!ui) {
        /* no text block (its refusal is logged), or no room for the page beside SceShell's margin (the page check's
         * guard line): the user sees nothing, as when PageOpen fails */
        int page_room = strstr(scenario_last_log(), "overlay page Paf reserve guard") != NULL;
        c->not_opened++;
        c->page_failed += page_room;
        TEST_CHECK_(page_room || strstr(scenario_last_log(), "overlay not opened") != NULL, "not opened, logged: %s",
                    scenario_last_log());
    } else if (scenario_free() < SCENARIO_PAGE_PAF) { /* PageOpen refused: open_page's own failure path */
        vjo_log("overlay page failed to open");
        vjo_overlay_mem_close(&scenario_ui);
        ui = NULL;
        c->not_opened++;
        c->page_failed++;
    } else {
        scenario_others += SCENARIO_PAGE_PAF; /* the page's widgets, while it is open */
        page = 1;
    }
    /* The cursor's word: an open page always asks; one that did not open asked too when 0.10.1's log has its arena's
     * refusal beside it (the job started before the page's close came). Such a press ran a job: the worker's own
     * steps give the arena back at its start and run (a press without one looks up on the shown text) */
    if (lookup_logged) {
        scenario_user_job();
        RESULT_MEM_JOB_START();
        RESULT_MEM_JOB_RUN();
    }
    if ((ui || lookup_logged) && scenario_arena(c) < 0 && ui)
        c->told++; /* run_cursor_job: the open page shows OOM_SHELL_RESULTS ("... too low for dictionary results") */
    if (anki && ui) { /* Enter in the open page: shell/anki.c's arena for the card (ANKI_TEXT_MEM) */
        void *a = vjo_shell_heap_alloc_for(VJO_SL_PAF_ANKI, VJO_BUDGET_PAF_ANKI);
        if (a) {
            c->anki_ok++;
            vjo_shell_heap_free(VJO_SL_PAF_ANKI, a, VJO_BUDGET_PAF_ANKI);
        } else {
            c->anki_refused++;
        }
    }
    if (page) scenario_others -= SCENARIO_PAGE_PAF;
    if (ui) vjo_overlay_mem_close(&scenario_ui); /* close_page */
}

static void scenario_print(const char *name, const ScenarioCounts *c)
{
    fprintf(stderr, "owner-scenario shell %s: presses=%u not_opened=%u page_failed=%u lookups=%u refused=%u told=%u "
                    "silent=%u anki_ok=%u anki_refused=%u peak=%u held=%u\n", name, c->presses, c->not_opened,
            c->page_failed, c->lookups, c->refused, c->told, c->refused - c->told, c->anki_ok, c->anki_refused,
            (unsigned)c->peak, (unsigned)c->held);
}

static void scenario_end(const char *name, ScenarioCounts *c, ScenarioCounts *all, uint32_t start_held)
{
    /* #528: the page's text block is the plugin's from the Shell's start (shell/overlay_mem.h): it is all Rubi may
     * still hold beside the plugin's own, once no game runs */
    uint32_t kept = scenario_ui ? VJO_OVERLAY_MEM_BYTES : 0;
    c->peak = scenario_peak;
    scenario_no_game(); /* the boot ends with no game in front: the idle rule */
    TEST_CHECK_(vjo_ledger_pool_current(&g_shell_ledger, VJO_LEDGER_POOL_SHELL_PAF) == start_held + kept,
                "%s: everything but the kept text block given back (%u held, %u at the start, %u kept)", name,
                (unsigned)vjo_ledger_pool_current(&g_shell_ledger, VJO_LEDGER_POOL_SHELL_PAF), (unsigned)start_held,
                (unsigned)kept);
    if (scenario_ui) { /* the next scenario is another boot: its Shell starts afresh */
        vjo_shell_heap_free(VJO_SL_PAF_UI, scenario_ui, VJO_OVERLAY_MEM_BYTES);
        scenario_ui = NULL;
    }
    scenario_print(name, c);
    all->presses += c->presses; all->not_opened += c->not_opened; all->page_failed += c->page_failed;
    all->lookups += c->lookups;
    all->refused += c->refused; all->told += c->told; all->anki_ok += c->anki_ok; all->anki_refused += c->anki_refused;
    if (c->peak > all->peak) all->peak = c->peak;
    if (c->held > all->held) all->held = c->held;
}

static void test_owner_scenario_shell(void)
{
    FILE *f = fopen(OWNER_SCENARIO_FILE, "r");
    char line[256], name[96] = "", all_name[] = "all";
    ScenarioCounts c, all;
    uint32_t start_held = 0;
    unsigned scenarios = 0;
    int in_scenario = 0;
    TEST_ASSERT_(f != NULL, "%s", OWNER_SCENARIO_FILE);
    memset(&all, 0, sizeof(all));
    memset(&c, 0, sizeof(c));
    while (fgets(line, sizeof(line), f)) {
        char kind[16];
        unsigned a = 0, b = 0, d = 0;
        int n = sscanf(line, "%15s %u %u %u", kind, &a, &b, &d);
        if (n < 1 || kind[0] == '#') continue;
        if (!strcmp(kind, "scenario")) {
            if (in_scenario) scenario_end(name, &c, &all, start_held);
            TEST_ASSERT(sscanf(line, "scenario %95s", name) == 1);
            reset();
            vjo_ledger_shell_init(); /* the plugin's own 8 KiB, as at the Shell's start */
            scenario_on = 1;
            scenario_others = 0;
            scenario_peak = start_held = vjo_ledger_pool_current(&g_shell_ledger, VJO_LEDGER_POOL_SHELL_PAF);
            memset(&c, 0, sizeof(c));
            in_scenario = 1;
            scenarios++;
            continue;
        }
        TEST_ASSERT_(in_scenario && n >= 3, "%s", line);
        scenario_level(a, b);
        if (!strcmp(kind, "boot")) {
            /* the Shell's start (shell/worker.c vjo_worker_start's end, overlay.cpp vjo_overlay_boot): the overlay's
             * text block, kept from now on (#528; 0.10.1 took no Paf here beside the plugin's) */
            vjo_overlay_mem_boot(&scenario_ui);
            scenario_between_jobs(&c);
        } else if (!strcmp(kind, "exit")) {
            /* the game left the front: on_game_exit's step, then the idle rule */
            job_running = 0;
            RESULT_MEM_GAME_EXIT();
            scenario_no_game();
            scenario_between_jobs(&c);
        } else if (!strcmp(kind, "start")) {
            /* no game ran since the last exit (the idle rule), then this one is in front */
            scenario_no_game();
            memcpy(title_id, "PCSG00415", 10);
            scenario_between_jobs(&c);
        } else if (!strcmp(kind, "press")) {
            TEST_ASSERT(n == 4);
            scenario_press(&c, (int)d, 0);
        } else if (!strcmp(kind, "anki")) {
            scenario_press(&c, 1, 1);
        } else if (!strcmp(kind, "lookup")) {
            /* a background read (not a refresh): its job's start and run steps, then the arena after OCR */
            scenario_user_job();
            RESULT_MEM_JOB_START();
            RESULT_MEM_JOB_RUN();
            scenario_arena(&c);
        } else {
            TEST_CHECK_(0, "unknown event: %s", line);
        }
    }
    fclose(f);
    if (in_scenario) scenario_end(name, &c, &all, start_held);
    scenario_on = 0;
    scenario_others = 0;
    scenario_print(all_name, &all);
    TEST_CHECK_(scenarios > 0, "no scenario in %s", OWNER_SCENARIO_FILE);
}
