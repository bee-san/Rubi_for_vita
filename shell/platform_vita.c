/* VjoPlatform for SceShell: SceNet sockets (SceShell has already initialized
 * the network stack), resolver DNS, RTC time and the kernel RNG. Finding
 * Anki on the LAN is the Overlay app's (settings/net.c), not SceShell's. */
#include <psp2/kernel/clib.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/rng.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/rtc.h>
#include <paf/std/stdlib.h>

#include "shell.h"

#define NET_TIMEOUT_US (20 * 1000 * 1000)
#define CONNECT_POLL_US 20000

static int sock_send(void *ctx, const void *p, size_t n)
{
    int r = sceNetSend((int)(intptr_t)ctx, p, n, 0);
    return r > 0 ? r : -1;
}

static int sock_recv(void *ctx, void *p, size_t n)
{
    int r = sceNetRecv((int)(intptr_t)ctx, p, n, 0);
    return r >= 0 ? r : -1;
}

/* DNS: 5 s per try, 2 retries; within timeout_us (one try) when it is set. */
static int resolve(const char *host, SceNetInAddr *addr, int timeout_us)
{
    int rid, ret, wait = 5 * 1000 * 1000, retries = 2;
    if (timeout_us > 0) {
        wait = timeout_us < wait ? timeout_us : wait;
        retries = 0;
    }
    rid = sceNetResolverCreate("VjoResolver", NULL, 0);
    if (rid < 0)
        return rid;
    ret = sceNetResolverStartNtoa(rid, host, addr, wait, retries, 0);
    sceNetResolverDestroy(rid);
    return ret;
}

static void ipv4_addr(SceNetSockaddrIn *sin, uint32_t ip, int port)
{
    sceClibMemset(sin, 0, sizeof(*sin));
    sin->sin_len = sizeof(*sin);
    sin->sin_family = SCE_NET_AF_INET;
    sin->sin_port = sceNetHtons((unsigned short)port);
    sin->sin_addr.s_addr = sceNetHtonl(ip);
}

/* Non-blocking connects (SceNet has no connect timeout option): start one,
 * then repeat it, which reports EISCONN once the first one is done (or its
 * error). Both return 1 = connected, 0 = in progress, < 0 = failed. */
static int connect_start(int fd, const SceNetSockaddrIn *sin)
{
    int on = 1, ret;
    sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &on, sizeof(on));
    ret = sceNetConnect(fd, (const SceNetSockaddr *)sin, sizeof(*sin));
    return ret == 0 ? 1 : (unsigned)ret == SCE_NET_ERROR_EINPROGRESS ? 0 : ret;
}

static int connect_poll(int fd, const SceNetSockaddrIn *sin)
{
    int ret = sceNetConnect(fd, (const SceNetSockaddr *)sin, sizeof(*sin));
    if (ret == 0 || (unsigned)ret == SCE_NET_ERROR_EISCONN)
        return 1;
    if ((unsigned)ret == SCE_NET_ERROR_EALREADY || (unsigned)ret == SCE_NET_ERROR_EINPROGRESS)
        return 0;
    return ret;
}

static int connect_timed(int fd, const SceNetSockaddrIn *sin, int timeout_us, const VjoNetCancel *nc)
{
    int off = 0, ret = connect_start(fd, sin);
    int64_t end = (int64_t)sceKernelGetProcessTimeWide() + timeout_us;
    while (ret == 0) {
        if ((int64_t)sceKernelGetProcessTimeWide() >= end) {
            ret = SCE_NET_ERROR_ETIMEDOUT;
            break;
        }
        if (nc && nc->cancelled) {
            ret = SCE_NET_ERROR_EINTR;
            break;
        }
        sceKernelDelayThread(CONNECT_POLL_US);
        ret = connect_poll(fd, sin);
    }
    sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &off, sizeof(off));
    return ret > 0 ? 0 : ret;
}

/* ---- cancelling (VjoNetCancel) ----
 * The socket is closed under the lock, so an abort never reaches a socket
 * number another thread got since. */

int vjo_net_cancel_init(VjoNetCancel *c)
{
    c->fd = -1;
    c->cancelled = 0;
    c->lock = sceKernelCreateMutex("VjoNetCancel", 0, 0, NULL);
    return c->lock < 0 ? -1 : 0;
}

void vjo_net_cancel(VjoNetCancel *c)
{
    sceKernelLockMutex(c->lock, 1, NULL);
    c->cancelled = 1;
    if (c->fd >= 0)
        sceNetSocketAbort(c->fd, 0);
    sceKernelUnlockMutex(c->lock, 1);
}

void vjo_net_cancel_clear(VjoNetCancel *c)
{
    sceKernelLockMutex(c->lock, 1, NULL);
    c->cancelled = 0;
    sceKernelUnlockMutex(c->lock, 1);
}

/* fd is the socket in use; -1 if cancelled meanwhile. */
static int cancel_track(VjoNetCancel *c, int fd)
{
    int ret = 0;
    if (!c)
        return 0;
    sceKernelLockMutex(c->lock, 1, NULL);
    if (c->cancelled)
        ret = -1;
    else
        c->fd = fd;
    sceKernelUnlockMutex(c->lock, 1);
    return ret;
}

static void close_tracked(VjoNetCancel *c, int fd)
{
    if (c)
        sceKernelLockMutex(c->lock, 1, NULL);
    if (c && c->fd == fd)
        c->fd = -1;
    sceNetSocketClose(fd);
    if (c)
        sceKernelUnlockMutex(c->lock, 1);
}

static int vita_connect(void *ud, const char *host, int port, int timeout_us, int io_timeout_us, VjoConn *out)
{
    SceNetSockaddrIn sin;
    SceNetInAddr addr;
    VjoNetCancel *nc = (VjoNetCancel *)ud;
    int state = 0, fd, ret, timeout = io_timeout_us > 0 ? io_timeout_us : NET_TIMEOUT_US;

    if (nc && nc->cancelled)
        return VJO_E_CANCELLED;
    if (sceNetCtlInetGetState(&state) < 0 || state != SCE_NETCTL_STATE_CONNECTED) {
        vjo_log("net: not connected (state %d)", state);
        return VJO_E_NET;
    }
    /* An IP address (Anki on the LAN) needs no DNS. */
    if (sceNetInetPton(SCE_NET_AF_INET, host, &addr) != 1 && (ret = resolve(host, &addr, timeout_us)) < 0) {
        vjo_log("net: resolve %s failed 0x%08X", host, ret);
        return VJO_E_NET;
    }
    ipv4_addr(&sin, sceNetNtohl(addr.s_addr), port);
    fd = sceNetSocket("VjoSocket", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (fd < 0) {
        vjo_log("net: socket failed 0x%08X", fd);
        return VJO_E_NET;
    }
    if (cancel_track(nc, fd) < 0) {
        sceNetSocketClose(fd);
        return VJO_E_CANCELLED;
    }
    sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &timeout, sizeof(timeout));
    sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (timeout_us > 0)
        ret = connect_timed(fd, &sin, timeout_us, nc);
    else
        ret = sceNetConnect(fd, (SceNetSockaddr *)&sin, sizeof(sin));
    if (ret < 0) {
        int cancelled = nc && nc->cancelled;
        if (!cancelled)
            vjo_log("net: connect %s failed 0x%08X", host, ret);
        close_tracked(nc, fd);
        return cancelled ? VJO_E_CANCELLED : VJO_E_NET;
    }
    out->ctx = (void *)(intptr_t)fd;
    out->send = sock_send;
    out->recv = sock_recv;
    return VJO_OK;
}

static void vita_disconnect(void *ud, VjoConn *c)
{
    close_tracked((VjoNetCancel *)ud, (int)(intptr_t)c->ctx);
}

/* A kept connection is usable if the server has neither closed it nor sent
 * anything (an alert before closing) since the last reply. It is then the
 * socket a cancel aborts. */
static int vita_acquire(void *ud, VjoConn *c)
{
    int fd = (int)(intptr_t)c->ctx;
    uint8_t b;
    if ((unsigned)sceNetRecv(fd, &b, 1, SCE_NET_MSG_PEEK | SCE_NET_MSG_DONTWAIT) != SCE_NET_ERROR_EWOULDBLOCK)
        return VJO_E_NET;
    return cancel_track((VjoNetCancel *)ud, fd) < 0 ? VJO_E_CANCELLED : VJO_OK;
}

/* An idle kept connection is not the socket in use. */
static void vita_release(void *ud, VjoConn *c)
{
    VjoNetCancel *nc = (VjoNetCancel *)ud;
    if (!nc)
        return;
    sceKernelLockMutex(nc->lock, 1, NULL);
    if (nc->fd == (int)(intptr_t)c->ctx)
        nc->fd = -1;
    sceKernelUnlockMutex(nc->lock, 1);
}

static void vita_random(void *ud, void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    (void)ud;
    /* sceKernelGetRandomNumber returns at most 64 bytes per call. */
    while (n) {
        size_t k = n > 64 ? 64 : n;
        sceKernelGetRandomNumber(p, k);
        p += k;
        n -= k;
    }
}

static uint64_t vita_time(void *ud)
{
    SceRtcTick t;
    int ret;
    (void)ud;
    ret = sceRtcGetCurrentTick(&t); /* microseconds since 0001-01-01 UTC */
    if (ret < 0) {
        /* 1970: certificate validation then fails with a TLS error. */
        vjo_log("rtc: sceRtcGetCurrentTick failed 0x%08X", ret);
        return 0;
    }
    return t.tick / 1000000ull - 62135596800ull;
}

static uint64_t vita_now_us(void *ud)
{
    (void)ud;
    return (uint64_t)sceKernelGetProcessTimeWide();
}

static void vita_log(void *ud, const char *msg)
{
    (void)ud;
    vjo_log("%s", msg);
}

static int local_read(void *ctx, uint64_t off, void *dst, size_t n)
{
    SceUID fd = (SceUID)(intptr_t)ctx;
    uint8_t *out = dst;
    vjo_dict_reads++; /* the lookup log's read count (worker.c) */
    vjo_dict_read_bytes += (uint32_t)n;
    if (off > INT64_MAX || sceIoLseek(fd, (SceOff)off, SCE_SEEK_SET) != (SceOff)off) return -1;
    while (n) {
        int got = sceIoRead(fd, out, (SceSize)n);
        if (got <= 0) return -1;
        out += got; n -= (size_t)got;
    }
    return 0;
}

static void local_close(void *ctx) { sceIoClose((SceUID)(intptr_t)ctx); }

static int local_open(void *ud, const char *path, VjoFile *out)
{
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    SceOff size;
    (void)ud;
    if (fd < 0) return -1;
    size = sceIoLseek(fd, 0, SCE_SEEK_END);
    if (size < 0) { sceIoClose(fd); return -1; }
    out->ctx = (void *)(intptr_t)fd; out->size = (uint64_t)size;
    out->read = local_read; out->close = local_close;
    return 0;
}

void vjo_platform_vita(VjoPlatform *p, VjoNetCancel *cancel)
{
    sceClibMemset(p, 0, sizeof(*p));
    p->ud = cancel;
    p->connect = vita_connect;
    p->file_open = local_open;
    p->disconnect = vita_disconnect;
    p->random = vita_random;
    p->unix_time = vita_time;
    p->log = vita_log;
    p->now_us = vita_now_us;
    p->acquire = vita_acquire;
    p->release = vita_release;
}
