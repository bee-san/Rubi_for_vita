/* The overlay's hold on SceShell's default front touch region (#104).
 *
 * SceTouch gives a touch to the frontmost process whose active region holds
 * it (SDK Touch Overview, "Ownership of Touch Data"). SceShell keeps its
 * default region inactive while a game is in front, so the game gets the
 * panel, and active while its own LiveArea is. The open overlay activates it
 * (paf ActivateDefaultRegion) to read taps over the game, and has to give it
 * back in the state SceShell expects for whatever is in front when it closes:
 *   - its game is still in front (closed by Cancel, L + R, a saved region...):
 *     deactivate, the state it had over the game;
 *   - the game left the foreground and SceShell is in front (PS, the game
 *     exited): no write at all. SceShell's own switch to the LiveArea owns the
 *     region then; deactivating it after that switch is #104 (the LiveArea's
 *     touch dead until SceShell next activates it).
 * The control thread decides which (shell/worker.c close_overlay, published as
 * VjoView.touch_handback); the paf page makes the write (shell/overlay.cpp).
 * Plain C, so the host tests run the same rules (host/tests/test_touch_hold.c). */
#ifndef VJO_TOUCH_HOLD_H
#define VJO_TOUCH_HOLD_H

/* VjoView.touch_handback: what closing the page does with the region. */
enum {
    VJO_TOUCH_RELEASE = 0, /* the game is in front: deactivate */
    VJO_TOUCH_LEAVE = 1,   /* SceShell is in front: leave the region to it, no write */
};

/* The write the page makes next. */
enum { VJO_TOUCH_NO_WRITE = 0, VJO_TOUCH_ACTIVATE = 1, VJO_TOUCH_DEACTIVATE = 2 };

typedef struct {
    int held; /* the page activated the region and has not given it back */
} VjoTouchHold;

/* The page opens (or its region selector starts): activate unless held. */
static inline int vjo_touch_hold(const VjoTouchHold *t)
{
    return t->held ? VJO_TOUCH_NO_WRITE : VJO_TOUCH_ACTIVATE;
}

/* The page closes with the control thread's handback. Returns the write to
 * make; for VJO_TOUCH_LEAVE the hold ends here, without one. */
static inline int vjo_touch_release(VjoTouchHold *t, int handback)
{
    if (!t->held)
        return VJO_TOUCH_NO_WRITE; /* never activated, or refused: nothing to give back */
    if (handback == VJO_TOUCH_LEAVE) {
        t->held = 0;
        return VJO_TOUCH_NO_WRITE;
    }
    return VJO_TOUCH_DEACTIVATE;
}

/* The write's result (ActivateDefaultRegion's rc). A refused write changes
 * nothing: a refused activation is not held (taps are not read), a refused
 * deactivation still is. */
static inline void vjo_touch_written(VjoTouchHold *t, int action, int rc)
{
    if (rc >= 0 && action != VJO_TOUCH_NO_WRITE)
        t->held = action == VJO_TOUCH_ACTIVATE;
}

#endif
