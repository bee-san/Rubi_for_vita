/* The overlay page's hold on SceShell's default front touch region
 * (shell/touch_hold.h, used by shell/overlay.cpp), against a model of the
 * region: one active flag per process region, the last write wins (SceTouch's
 * ksceTouchActivateRegion as shoui520/psp2's reimplementation has it), written
 * by SceShell's own transitions (LiveArea in front: active; a game in front:
 * inactive) and by the page. Who gets a touch: SceShell while the flag is set,
 * else the game in front, else nobody (the LiveArea's touch is dead).
 * The model is an assumption about SceShell, not a measurement: it is the one
 * that explains both the console's working header taps over a game and #104. */
#include "acutest.h"
#include "../../shell/touch_hold.h"

typedef struct {
    int active;       /* SceShell's default front region */
    int game_front;   /* the game is in front (its own region takes the rest) */
    unsigned writes;  /* the page's ActivateDefaultRegion calls */
    int refuse;       /* the next page write fails (rc < 0) */
} Panel;

enum { TOUCH_NOBODY = 0, TOUCH_SHELL, TOUCH_GAME };

static int who_touches(const Panel *p)
{
    if (p->active)
        return TOUCH_SHELL;
    return p->game_front ? TOUCH_GAME : TOUCH_NOBODY;
}

/* overlay.cpp touch_write */
static void page_write(Panel *p, VjoTouchHold *t, int action)
{
    int rc;
    if (action == VJO_TOUCH_NO_WRITE)
        return;
    p->writes++;
    rc = p->refuse ? -1 : 0;
    p->refuse = 0;
    if (rc >= 0)
        p->active = action == VJO_TOUCH_ACTIVATE;
    vjo_touch_written(t, action, rc);
}

static void page_open(Panel *p, VjoTouchHold *t) { page_write(p, t, vjo_touch_hold(t)); }
static void page_close(Panel *p, VjoTouchHold *t, int handback) { page_write(p, t, vjo_touch_release(t, handback)); }
/* The rule before #104: every close deactivated a held region. */
static void old_page_close(Panel *p, VjoTouchHold *t) { page_write(p, t, t->held ? VJO_TOUCH_DEACTIVATE : 0); }

/* SceShell's own switches */
static void shell_to_livearea(Panel *p) { p->game_front = 0; p->active = 1; }
static void shell_to_game(Panel *p) { p->game_front = 1; p->active = 0; }

static void game_in_front(Panel *p, VjoTouchHold *t)
{
    *t = (VjoTouchHold){0};
    *p = (Panel){0};
    shell_to_game(p);
    TEST_CHECK(who_touches(p) == TOUCH_GAME);
}

/* Closed in game (Cancel, L + R): the region is back as SceShell had it over the game. */
static void test_close_in_game_gives_the_panel_back_to_the_game(void)
{
    Panel p;
    VjoTouchHold t;
    game_in_front(&p, &t);
    page_open(&p, &t);
    TEST_CHECK(t.held && who_touches(&p) == TOUCH_SHELL && p.writes == 1);
    page_open(&p, &t); /* the region selector: already held, no second write */
    TEST_CHECK(p.writes == 1);
    page_close(&p, &t, VJO_TOUCH_RELEASE);
    TEST_CHECK(!t.held && who_touches(&p) == TOUCH_GAME && p.writes == 2);
}

/* open -> PS -> released. SceShell switches to the LiveArea before the page
 * closes (the console's order: test.1's log has "touch region off" after the
 * game's exit ledger lines) or after it. The old rule left the LiveArea
 * without touch in the first order (#104); the hand-back leaves the region to
 * SceShell in both. resume -> closed: SceShell's switch back to the game gives
 * it the panel, and the next open takes the region again. */
static void test_ps_leaves_the_livearea_its_touch_in_either_order(void)
{
    for (int shell_first = 0; shell_first < 2; shell_first++) {
        Panel p;
        VjoTouchHold t;
        game_in_front(&p, &t);
        page_open(&p, &t);
        if (shell_first)
            shell_to_livearea(&p);
        page_close(&p, &t, VJO_TOUCH_LEAVE);
        if (!shell_first)
            shell_to_livearea(&p);
        TEST_CHECK_(!t.held && p.writes == 1, "order %d", shell_first);
        TEST_CHECK_(who_touches(&p) == TOUCH_SHELL, "order %d: the LiveArea has its touch", shell_first);
        shell_to_game(&p); /* resume */
        TEST_CHECK(who_touches(&p) == TOUCH_GAME);
        page_open(&p, &t);
        TEST_CHECK(t.held && who_touches(&p) == TOUCH_SHELL && p.writes == 2);
        page_close(&p, &t, VJO_TOUCH_RELEASE);
        TEST_CHECK(who_touches(&p) == TOUCH_GAME);
    }
    /* The same sequence under the old rule: #104 reproduced in the model. */
    Panel p;
    VjoTouchHold t;
    game_in_front(&p, &t);
    page_open(&p, &t);
    shell_to_livearea(&p);
    old_page_close(&p, &t);
    TEST_CHECK(who_touches(&p) == TOUCH_NOBODY);
}

/* A refused activation is not held: no taps are read, and closing writes
 * nothing. A refused deactivation is still held: the next open assumes the
 * region is active and does not write again; the next close tries again. */
static void test_refused_writes(void)
{
    Panel p;
    VjoTouchHold t;
    game_in_front(&p, &t);
    p.refuse = 1;
    page_open(&p, &t);
    TEST_CHECK(!t.held && who_touches(&p) == TOUCH_GAME && p.writes == 1);
    page_close(&p, &t, VJO_TOUCH_RELEASE);
    page_close(&p, &t, VJO_TOUCH_LEAVE);
    TEST_CHECK(p.writes == 1 && !t.held);

    page_open(&p, &t);
    TEST_CHECK(t.held && p.writes == 2);
    p.refuse = 1;
    page_close(&p, &t, VJO_TOUCH_RELEASE);
    TEST_CHECK(t.held && p.writes == 3 && who_touches(&p) == TOUCH_SHELL);
    page_open(&p, &t);
    TEST_CHECK(p.writes == 3);
    page_close(&p, &t, VJO_TOUCH_RELEASE);
    TEST_CHECK(!t.held && p.writes == 4 && who_touches(&p) == TOUCH_GAME);
}

TEST_LIST = {
    {"close_in_game_gives_the_panel_back_to_the_game", test_close_in_game_gives_the_panel_back_to_the_game},
    {"ps_leaves_the_livearea_its_touch_in_either_order", test_ps_leaves_the_livearea_its_touch_in_either_order},
    {"refused_writes", test_refused_writes},
    {NULL, NULL}
};
