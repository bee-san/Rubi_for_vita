/* The game process's network for the worker's online jobs (Google Lens).
 * docs/shell-slim.md section 3 has the evidence for running it here and the
 * rules that keep the game's own network intact:
 *   - SCE_SYSMODULE_NET is loaded only when the game has not loaded it, and it
 *     is never unloaded: it costs the game's budget nothing [SDK:
 *     libsysmodule-Reference, 'Module IDs'], and loading a loaded module also
 *     answers SCE_OK, so an unload could pull one the game loaded meanwhile;
 *   - libnet is initialized only when the game's is not up (SCE_NET_ERROR_EBUSY:
 *     the game's own is used and never terminated), on a pool the job gives,
 *     and terminated when the job ends, before that pool is freed;
 *   - no libnetctl: a resolver or connect failure says "no network".
 * Every unresolved weak import is checked before it is called (vjo_import.h). */
#ifndef VJO_GAME_NET_H
#define VJO_GAME_NET_H

#include <stddef.h>
#include <stdint.h>

#include "../core/net.h"

/* libnet's pool when the worker starts it: "32 KiB or more is recommended"
 * [SDK: libnet-Reference, 'SceNetInitParam']. */
#define VJO_GAME_NET_POOL 32768u
/* The platform's log lines kept for the Shell (each "line\n"). */
#define VJO_GAME_NET_LOG 640u
/* Reads and writes give up after this (each call, not the request), as the
 * Shell's did (shell/platform_vita.c NET_TIMEOUT_US)... */
#define VJO_GAME_NET_IO_US (20 * 1000 * 1000)
/* ...in slices of this, between which the job's cancellation is checked: the
 * Shell aborted a socket from its control thread (sceNetSocketAbort); here the
 * worker's own thread is the one waiting, so it looks itself. */
#define VJO_GAME_NET_SLICE_US (250 * 1000)

/* VjoGameNet.sysmodule and .libnet: how the job found them. */
enum {
    VJO_GNET_NONE = 0,   /* not looked at (the job ended first) */
    VJO_GNET_GAME = 1,   /* the game's, already there: used, never unloaded or terminated */
    VJO_GNET_OURS = 2,   /* the worker loaded or started it */
    VJO_GNET_FAILED = 3, /* unavailable: rc says why */
};

typedef struct {
    int sysmodule, sysmodule_rc; /* SCE_SYSMODULE_NET */
    int libnet, init_rc;         /* libnet */
    int up;                      /* the worker's libnet is up (vjo_game_net_close ends it) */
    int term_rc;                 /* sceNetTerm's, when libnet was ours */
    int (*cancelled)(void *ud);  /* the job's cancel check, polled while a connect, read or write waits */
    void *cancel_ud;
    int fd;                      /* the job's one socket (-1: none) */
    int io_us;                   /* its reads' and writes' limit */
    char log[VJO_GAME_NET_LOG];  /* the platform's lines, "line\n" each, NUL-terminated */
    unsigned log_used, log_dropped;
} VjoGameNet;

/* Readies the network for one job; pool (pool_bytes >= VJO_GAME_NET_POOL)
 * must stay valid until vjo_game_net_close returns 0. Returns VJO_OK, or
 * VJO_E_NET with n->sysmodule / n->libnet saying which part failed. */
int vjo_game_net_open(VjoGameNet *n, void *pool, size_t pool_bytes, int (*cancelled)(void *ud), void *ud);
/* The job's VjoPlatform: sockets with the resolver, the kernel's random
 * numbers, the RTC's time, now_us and the log buffer. No connection pool. */
void vjo_game_net_platform(VjoGameNet *n, VjoPlatform *p);
/* Ends the job's network: sceNetTerm if this job started libnet. Returns 0,
 * or < 0 when libnet may still use the pool (it must not be freed). */
int vjo_game_net_close(VjoGameNet *n);

#endif
