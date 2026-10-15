/* The overlay page's hold on SceShell's default front touch region (#104).
 *
 * SceTouch gives a touch to the frontmost process whose active region holds
 * it (SDK Touch Overview, "Ownership of Touch Data"). SceShell keeps its
 * default region inactive while a game is in front, so the game gets the
 * panel, and active while its own LiveArea is. The open overlay activates it
 * (paf ActivateDefaultRegion) to read taps over the game, and has to give it
 * back in the state SceShell expects for whatever is in front when it closes:
 *   - its game is still in front (closed by Cancel, L + R, a saved region...),
 *     or a process started over it: deactivate, the state it had over the game;
 *   - the game left the foreground and SceShell is in front (PS, the game
 *     exited): no write at all. SceShell's own switch to the LiveArea owns the
 *     region then; deactivating it after that switch is #104 (the LiveArea's
 *     touch dead until SceShell next activates it).
 * Two signals say the game left, and either one is enough:
 *   - the control thread's hand-back (shell/worker.c close_overlay_for,
 *     published as VjoView.touch_handback with the close): VJO_TOUCH_LEAVE when
 *     the game's exit closed the overlay, or reached it before the page closed;
 *   - the kernel's foreground as the page closes (vjoGetState, read by the
 *     page): none in front. It changes in the process event hook itself, so it
 *     is current even when the control thread has not handled the exit yet
 *     (Cancel, then PS before the page's next frame).
 * Plain C, so the host tests run the same rules (host/tests/test_touch_hold.c);
 * shell/overlay.cpp makes the writes. */
#ifndef VJO_TOUCH_HOLD_H
#define VJO_TOUCH_HOLD_H

#include <stdint.h>

/* VjoView.touch_handback: the control thread's view of what is in front. */
enum {
    VJO_TOUCH_RELEASE = 0, /* the game is in front: deactivate */
    VJO_TOUCH_LEAVE = 1,   /* SceShell is in front: leave the region to it, no write */
};

/* The write the page makes next; VJO_TOUCH_LEFT: the hold ends without one. */
enum { VJO_TOUCH_NO_WRITE = 0, VJO_TOUCH_ACTIVATE = 1, VJO_TOUCH_DEACTIVATE = 2, VJO_TOUCH_LEFT = 3 };

typedef struct {
    int held; /* the page activated the region and has not given it back */
} VjoTouchHold;

/* The page opens (or its region selector starts): activate unless held. */
static inline int vjo_touch_hold(const VjoTouchHold *t)
{
    return t->held ? VJO_TOUCH_NO_WRITE : VJO_TOUCH_ACTIVATE;
}

/* The page closes, with the control thread's handback, and vjoGetState's
 * result and VjoState.game_pid read as it closes (a failed read says nothing).
 * Returns the write to make; VJO_TOUCH_LEFT ends the hold without one. */
static inline int vjo_touch_release(VjoTouchHold *t, int handback, int state_rc, int32_t game_pid)
{
    if (!t->held)
        return VJO_TOUCH_NO_WRITE; /* never activated, or refused: nothing to give back */
    if (handback == VJO_TOUCH_LEAVE || (state_rc >= 0 && game_pid == 0)) {
        t->held = 0;
        return VJO_TOUCH_LEFT;
    }
    return VJO_TOUCH_DEACTIVATE;
}

/* A write's result (ActivateDefaultRegion's rc). A refused write changes
 * nothing: a refused activation is not held (taps are not read), a refused
 * deactivation still is. */
static inline void vjo_touch_written(VjoTouchHold *t, int action, int rc)
{
    if (rc >= 0 && (action == VJO_TOUCH_ACTIVATE || action == VJO_TOUCH_DEACTIVATE))
        t->held = action == VJO_TOUCH_ACTIVATE;
}

#endif
