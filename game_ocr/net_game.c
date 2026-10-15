/* The game process's network for the worker's online jobs: net_game.h. */
#include "net_game.h"

#include <stdarg.h>

#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/rng.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/rtc.h>
#include <psp2/sysmodule.h>

#include "../include/vjo_import.h"

#ifdef VJO_HOST
#define IMPORT_READY(fn) 1
#else
#define IMPORT_READY(fn) VJO_IMPORT_READY(fn)
#endif

#define CONNECT_POLL_US 20000
#ifdef VJO_HOST
/* Host tests: HTTP over the fake socket without TLS (core/net.h plain_http). The
 * Vita build has no way to skip TLS. */
int vjo_game_net_plain_http;
#endif
#define RESOLVE_US (5 * 1000 * 1000) /* one try, as the Shell's resolve() within a timeout */

static void note(VjoGameNet *n, const char *fmt, ...)
{
    char line[192];
    va_list ap;
    int len;
    va_start(ap, fmt);
    len = sceClibVsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (len < 0)
        return;
    if ((size_t)len >= sizeof(line))
        len = (int)sizeof(line) - 1;
    if (n->log_used + (unsigned)len + 2 > sizeof(n->log)) {
        n->log_dropped++;
        return;
    }
    sceClibMemcpy(n->log + n->log_used, line, (size_t)len);
    n->log_used += (unsigned)len;
    n->log[n->log_used++] = '\n';
    n->log[n->log_used] = '\0';
}

/* The libnet calls a job makes, all bound once SCE_SYSMODULE_NET is in. */
static int net_imports_ready(void)
{
    return IMPORT_READY(sceNetInit) && IMPORT_READY(sceNetTerm) && IMPORT_READY(sceNetSocket) &&
           IMPORT_READY(sceNetConnect) && IMPORT_READY(sceNetSend) && IMPORT_READY(sceNetRecv) &&
           IMPORT_READY(sceNetSetsockopt) && IMPORT_READY(sceNetSocketClose) && IMPORT_READY(sceNetInetPton) &&
           IMPORT_READY(sceNetHtons) && IMPORT_READY(sceNetResolverCreate) && IMPORT_READY(sceNetResolverStartNtoa) &&
           IMPORT_READY(sceNetResolverDestroy);
}

int vjo_game_net_open(VjoGameNet *n, void *pool, size_t pool_bytes, int (*cancelled)(void *ud), void *ud)
{
    SceNetInitParam param;
    int rc;
    sceClibMemset(n, 0, sizeof(*n));
    n->cancelled = cancelled;
    n->cancel_ud = ud;
    n->fd = -1;
    if (!IMPORT_READY(sceSysmoduleIsLoaded) || !IMPORT_READY(sceSysmoduleLoadModule)) {
        n->sysmodule = VJO_GNET_FAILED;
        n->sysmodule_rc = -1;
        note(n, "net: SceSysmodule is not available in this game");
        return VJO_E_NET;
    }
    rc = sceSysmoduleIsLoaded(SCE_SYSMODULE_NET);
    if (rc == SCE_SYSMODULE_LOADED) {
        n->sysmodule = VJO_GNET_GAME;
    } else {
        rc = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
        n->sysmodule = rc == 0 ? VJO_GNET_OURS : VJO_GNET_FAILED;
    }
    n->sysmodule_rc = rc;
    if (n->sysmodule == VJO_GNET_FAILED) {
        note(n, "net: loading SCE_SYSMODULE_NET failed 0x%08X", (unsigned)rc);
        return VJO_E_NET;
    }
    if (!net_imports_ready()) {
        n->libnet = VJO_GNET_FAILED;
        n->init_rc = -1;
        note(n, "net: libnet's imports are not bound in this game");
        return VJO_E_NET;
    }
    if (!pool || pool_bytes < VJO_GAME_NET_POOL) {
        n->libnet = VJO_GNET_FAILED;
        n->init_rc = -1;
        return VJO_E_NET;
    }
    sceClibMemset(&param, 0, sizeof(param));
    param.memory = pool;
    param.size = (int)pool_bytes;
    param.flags = 0;
    rc = sceNetInit(&param);
    n->init_rc = rc;
    if (rc == 0) {
        n->libnet = VJO_GNET_OURS;
        n->up = 1;
    } else if ((unsigned)rc == SCE_NET_ERROR_EBUSY) {
        n->libnet = VJO_GNET_GAME; /* the game's libnet: used, never terminated */
    } else {
        n->libnet = VJO_GNET_FAILED;
        note(n, "net: sceNetInit failed 0x%08X", (unsigned)rc);
        return VJO_E_NET;
    }
    return VJO_OK;
}

int vjo_game_net_close(VjoGameNet *n)
{
    if (n->fd >= 0) { /* a request that ended without closing its socket */
        sceNetSocketClose(n->fd);
        n->fd = -1;
    }
    if (!n->up)
        return 0;
    n->term_rc = sceNetTerm();
    if (n->term_rc < 0) {
        note(n, "net: sceNetTerm failed 0x%08X: its pool is kept", (unsigned)n->term_rc);
        return -1;
    }
    n->up = 0;
    return 0;
}

/* ---- the platform ---- */

static int job_cancelled(const VjoGameNet *n)
{
    return n->cancelled && n->cancelled(n->cancel_ud);
}

/* A slice ran out with nothing sent or received: wait on, unless the job was
 * cancelled or io_us passed. */
static int wait_on(const VjoGameNet *n, int r, int *waited)
{
    if ((unsigned)r != SCE_NET_ERROR_EAGAIN || job_cancelled(n))
        return 0;
    *waited += VJO_GAME_NET_SLICE_US;
    return *waited < n->io_us;
}

static int sock_send(void *ctx, const void *p, size_t len)
{
    VjoGameNet *n = (VjoGameNet *)ctx;
    int r, waited = 0;
    while ((r = sceNetSend(n->fd, p, len, 0)) <= 0 && wait_on(n, r, &waited)) {
    }
    return r > 0 ? r : -1;
}

static int sock_recv(void *ctx, void *p, size_t len)
{
    VjoGameNet *n = (VjoGameNet *)ctx;
    int r, waited = 0;
    while ((r = sceNetRecv(n->fd, p, len, 0)) < 0 && wait_on(n, r, &waited)) {
    }
    return r >= 0 ? r : -1;
}

static int resolve(VjoGameNet *n, const char *host, SceNetInAddr *addr, int timeout_us)
{
    int rid, ret, wait = RESOLVE_US;
    if (timeout_us > 0 && timeout_us < wait)
        wait = timeout_us;
    rid = sceNetResolverCreate("VjoGameResolver", NULL, 0);
    if (rid < 0)
        return rid;
    ret = sceNetResolverStartNtoa(rid, host, addr, wait, 0, 0);
    sceNetResolverDestroy(rid);
    (void)n;
    return ret;
}

/* A connect bounded by timeout_us and the job's cancellation (SceNet has no
 * connect timeout): non-blocking, repeated until it reports EISCONN. */
static int connect_timed(VjoGameNet *n, int fd, const SceNetSockaddrIn *sin, int timeout_us)
{
    int on = 1, off = 0, ret;
    int64_t end = (int64_t)sceKernelGetProcessTimeWide() + timeout_us;
    sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &on, sizeof(on));
    ret = sceNetConnect(fd, (const SceNetSockaddr *)sin, sizeof(*sin));
    if (ret == 0 || (unsigned)ret == SCE_NET_ERROR_EISCONN) {
        ret = 0;
    } else if ((unsigned)ret == SCE_NET_ERROR_EINPROGRESS) {
        ret = 0;
        for (;;) {
            int r = sceNetConnect(fd, (const SceNetSockaddr *)sin, sizeof(*sin));
            if (r == 0 || (unsigned)r == SCE_NET_ERROR_EISCONN) {
                ret = 0;
                break;
            }
            if ((unsigned)r != SCE_NET_ERROR_EALREADY && (unsigned)r != SCE_NET_ERROR_EINPROGRESS) {
                ret = r;
                break;
            }
            if ((int64_t)sceKernelGetProcessTimeWide() >= end) {
                ret = (int)SCE_NET_ERROR_ETIMEDOUT;
                break;
            }
            if (job_cancelled(n)) {
                ret = (int)SCE_NET_ERROR_EINTR;
                break;
            }
            sceKernelDelayThread(CONNECT_POLL_US);
        }
    }
    sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &off, sizeof(off));
    return ret;
}

static int game_connect(void *ud, const char *host, int port, int timeout_us, int io_timeout_us, VjoConn *out)
{
    VjoGameNet *n = (VjoGameNet *)ud;
    SceNetInAddr addr;
    SceNetSockaddrIn sin;
    int fd, ret, slice = VJO_GAME_NET_SLICE_US;
    if (job_cancelled(n))
        return VJO_E_CANCELLED;
    if (n->fd >= 0) /* one connection a job: core/net.c without a pool closes each before the next */
        return VJO_E_NET;
    n->io_us = io_timeout_us > 0 ? io_timeout_us : VJO_GAME_NET_IO_US;
    if (sceNetInetPton(SCE_NET_AF_INET, host, &addr) != 1 &&
        (ret = resolve(n, host, &addr, timeout_us > 0 ? timeout_us : RESOLVE_US)) < 0) {
        note(n, "net: resolve %s failed 0x%08X", host, (unsigned)ret);
        return VJO_E_NET;
    }
    sceClibMemset(&sin, 0, sizeof(sin));
    sin.sin_len = sizeof(sin);
    sin.sin_family = SCE_NET_AF_INET;
    sin.sin_port = sceNetHtons((unsigned short)port);
    sin.sin_addr = addr; /* network order already */
    fd = sceNetSocket("VjoGameSocket", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (fd < 0) {
        note(n, "net: socket failed 0x%08X", (unsigned)fd);
        return VJO_E_NET;
    }
    sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &slice, sizeof(slice));
    sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO, &slice, sizeof(slice));
    ret = connect_timed(n, fd, &sin, timeout_us > 0 ? timeout_us : n->io_us);
    if (ret < 0) {
        int cancelled = job_cancelled(n);
        if (!cancelled)
            note(n, "net: connect %s failed 0x%08X", host, (unsigned)ret);
        sceNetSocketClose(fd);
        return cancelled ? VJO_E_CANCELLED : VJO_E_NET;
    }
    n->fd = fd;
    out->ctx = n;
    out->send = sock_send;
    out->recv = sock_recv;
    return VJO_OK;
}

static void game_disconnect(void *ud, VjoConn *c)
{
    VjoGameNet *n = (VjoGameNet *)ud;
    (void)c;
    if (n->fd >= 0)
        sceNetSocketClose(n->fd);
    n->fd = -1;
}

static void game_random(void *ud, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    (void)ud;
    while (len) { /* at most 64 bytes per call */
        size_t k = len > 64 ? 64 : len;
        sceKernelGetRandomNumber(p, k);
        p += k;
        len -= k;
    }
}

static uint64_t game_time(void *ud)
{
    VjoGameNet *n = (VjoGameNet *)ud;
    SceRtcTick t;
    int ret = IMPORT_READY(sceRtcGetCurrentTick) ? sceRtcGetCurrentTick(&t) : -1;
    if (ret < 0) {
        /* 1970: certificate validation then fails with a TLS error */
        note(n, "rtc: no time (0x%08X)", (unsigned)ret);
        return 0;
    }
    return t.tick / 1000000ull - 62135596800ull; /* microseconds since 0001-01-01 -> Unix seconds */
}

static uint64_t game_now_us(void *ud)
{
    (void)ud;
    return (uint64_t)sceKernelGetProcessTimeWide();
}

static void game_log(void *ud, const char *msg)
{
    note((VjoGameNet *)ud, "%s", msg);
}

void vjo_game_net_platform(VjoGameNet *n, VjoPlatform *p)
{
    sceClibMemset(p, 0, sizeof(*p));
    p->ud = n;
    p->connect = game_connect;
    p->disconnect = game_disconnect;
    p->random = game_random;
    p->unix_time = game_time;
    p->now_us = game_now_us;
    p->log = game_log;
#ifdef VJO_HOST
    p->plain_http = vjo_game_net_plain_http;
#endif
}
