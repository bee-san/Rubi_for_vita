/* The Rubi app's graphics start as a system application (settings/sysapp_gfx.c): what the
 * link's wrappers of libvita2d's two calls into the system saw, for log.txt and the tests. */
#ifndef VJO_SETTINGS_SYSAPP_GFX_H
#define VJO_SETTINGS_SYSAPP_GFX_H
#include <stdint.h>

/* libvita2d a8f15ab's sceGxmVshInitialize flags (vita2d.c:240, the pinned libvita2d.a's
 * literal): SHARED_SYNC | SHAREDPB_OPEN, the system's shared parameter buffer. */
#define VJO_GFX_FLAGS_LIBVITA2D 0x0Au
/* The app's own parameter buffer in main memory, when that one is refused: PB_LPDDR |
 * SHARED_SYNC (a system application has no CDRAM; libvita2d's 2 MiB). */
#define VJO_GFX_FLAGS_OWN_PB 0x03u
/* The display's hand-over: the pause after each "not yet", the log.txt line, the end of the wait. */
#define VJO_GFX_RETRY_US 1000u
#define VJO_GFX_LOG_AFTER_US 1000000u
#define VJO_GFX_GIVE_UP_US 10000000u

typedef struct {
    int gxm_calls;               /* sceGxmVshInitialize calls (0: libvita2d took the ordinary path) */
    unsigned flags[2];           /* each call's flags */
    int rc[2];                   /* and its result */
    unsigned reads;              /* sceSharedFbGetInfo calls up to the hand-over */
    int fb_id, fb_rc, index;     /* the last of them: the framebuffer's id, the result, its buffer index */
    uint64_t first_us, ready_us; /* the first one; the hand-over */
    int ready;                   /* libvita2d has a framebuffer it can draw into */
    int waiting_logged;          /* the wait's log.txt line is written */
} VjoSysappGfx;

const VjoSysappGfx *vjo_sysapp_gfx(void);
#endif
