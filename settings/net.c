#include "net.h"
#include "../shell/shell.h" /* vjo_log: anki_page.c */

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/rng.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/rtc.h>
#include <psp2/sysmodule.h>
#include <stdint.h>
#include <string.h>

#define NET_POOL (256 * 1024) /* SceNet's memory, in this app (static) */
#define NETCTL_ERROR_NOT_TERMINATED 0x80412102u /* sceNetCtlInit: already initialized */
#define NET_TIMEOUT_US (20 * 1000 * 1000)
#define CONNECT_POLL_US 20000

static unsigned char net_pool[NET_POOL] __attribute__((aligned(16)));
static int net_up;
static SceUID cancel_lock = -1;
static int cancel_fd = -1, cancel_rid = -1; /* the socket and the resolver in use */
static volatile int cancelled;

int vjo_app_net_init(void)
{
    SceNetInitParam param;
    int rc;
    if (net_up)
        return 0;
    if ((rc = sceSysmoduleLoadModule(SCE_SYSMODULE_NET)) < 0)
        return rc;
    memset(&param, 0, sizeof(param));
    param.memory = net_pool;
    param.size = sizeof(net_pool);
    rc = sceNetInit(&param);
    if (rc < 0 && (unsigned)rc != SCE_NET_ERROR_EBUSY) /* (already up) */
        return rc;
    if ((rc = sceNetCtlInit()) < 0 && (unsigned)rc != NETCTL_ERROR_NOT_TERMINATED)
        return rc;
    if (cancel_lock < 0)
        cancel_lock = sceKernelCreateMutex("VjoAppNetCancel", 0, 0, NULL);
    net_up = 1;
    return 0;
}

void vjo_app_net_term(void)
{
    if (!net_up)
        return;
    sceNetCtlTerm();
    sceNetTerm();
    sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
    net_up = 0;
}

int vjo_app_net_connected(void)
{
    int state = 0;
    return net_up && sceNetCtlInetGetState(&state) >= 0 && state == SCE_NETCTL_STATE_CONNECTED;
}

/* ---- cancelling: the socket in use is aborted under the lock ---- */

void vjo_app_net_cancel(void)
{
    cancelled = 1; /* also before the network is up: every later connect fails */
    if (cancel_lock < 0)
        return;
    sceKernelLockMutex(cancel_lock, 1, NULL);
    if (cancel_fd >= 0)
        sceNetSocketAbort(cancel_fd, 0);
    if (cancel_rid >= 0)
        sceNetResolverAbort(cancel_rid, 0);
    sceKernelUnlockMutex(cancel_lock, 1);
}

void vjo_app_net_clear(void)
{
    cancelled = 0;
}

int vjo_app_net_cancelled(void)
{
    return cancelled;
}

static int track(int fd)
{
    int rc = 0;
    sceKernelLockMutex(cancel_lock, 1, NULL);
    if (cancelled)
        rc = -1;
    else
        cancel_fd = fd;
    sceKernelUnlockMutex(cancel_lock, 1);
    return rc;
}

static void close_tracked(int fd)
{
    sceKernelLockMutex(cancel_lock, 1, NULL);
    if (cancel_fd == fd)
        cancel_fd = -1;
    sceNetSocketClose(fd);
    sceKernelUnlockMutex(cancel_lock, 1);
}

/* ---- sockets ---- */

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

static void ipv4_addr(SceNetSockaddrIn *sin, uint32_t ip, int port)
{
    memset(sin, 0, sizeof(*sin));
    sin->sin_len = sizeof(*sin);
    sin->sin_family = SCE_NET_AF_INET;
    sin->sin_port = sceNetHtons((unsigned short)port);
    sin->sin_addr.s_addr = sceNetHtonl(ip);
}

/* Non-blocking connects: 1 = connected, 0 = in progress, < 0 = failed. */
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

/* A host name (an IP address needs none); vjo_app_net_cancel aborts it. */
static int resolve(const char *host, SceNetInAddr *addr, int timeout_us)
{
    int rid = sceNetResolverCreate("VjoAppResolver", NULL, 0), ret = -1;
    if (rid < 0)
        return rid;
    sceKernelLockMutex(cancel_lock, 1, NULL);
    cancel_rid = cancelled ? -1 : rid;
    sceKernelUnlockMutex(cancel_lock, 1);
    if (cancel_rid >= 0)
        ret = sceNetResolverStartNtoa(rid, host, addr, timeout_us > 0 ? timeout_us : 5000000, timeout_us > 0 ? 0 : 2,
                                      0);
    sceKernelLockMutex(cancel_lock, 1, NULL);
    cancel_rid = -1;
    sceNetResolverDestroy(rid);
    sceKernelUnlockMutex(cancel_lock, 1);
    return ret;
}

static int app_connect(void *ud, const char *host, int port, int timeout_us, int io_timeout_us, VjoConn *out)
{
    SceNetSockaddrIn sin;
    SceNetInAddr addr;
    int fd, ret, timeout = io_timeout_us > 0 ? io_timeout_us : NET_TIMEOUT_US, off = 0;
    int64_t end;
    (void)ud;
    if (cancelled)
        return VJO_E_CANCELLED;
    if (!vjo_app_net_connected())
        return VJO_E_NET;
    if (sceNetInetPton(SCE_NET_AF_INET, host, &addr) != 1 && resolve(host, &addr, timeout_us) < 0)
        return VJO_E_NET;
    ipv4_addr(&sin, sceNetNtohl(addr.s_addr), port);
    if ((fd = sceNetSocket("VjoAppSocket", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0)) < 0)
        return VJO_E_NET;
    if (track(fd) < 0) {
        sceNetSocketClose(fd);
        return VJO_E_CANCELLED;
    }
    sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &timeout, sizeof(timeout));
    sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO, &timeout, sizeof(timeout));
    end = (int64_t)sceKernelGetProcessTimeWide() + (timeout_us > 0 ? timeout_us : NET_TIMEOUT_US);
    ret = connect_start(fd, &sin);
    while (ret == 0 && !cancelled && (int64_t)sceKernelGetProcessTimeWide() < end) {
        sceKernelDelayThread(CONNECT_POLL_US);
        ret = connect_poll(fd, &sin);
    }
    sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &off, sizeof(off));
    if (ret != 1) {
        close_tracked(fd);
        return cancelled ? VJO_E_CANCELLED : VJO_E_NET;
    }
    out->ctx = (void *)(intptr_t)fd;
    out->send = sock_send;
    out->recv = sock_recv;
    return VJO_OK;
}

static void app_disconnect(void *ud, VjoConn *c)
{
    (void)ud;
    close_tracked((int)(intptr_t)c->ctx);
}

static void app_random(void *ud, void *buf, size_t n)
{
    unsigned char *p = buf;
    (void)ud;
    while (n) { /* at most 64 bytes per call */
        size_t k = n > 64 ? 64 : n;
        sceKernelGetRandomNumber(p, k);
        p += k;
        n -= k;
    }
}

static uint64_t app_time(void *ud)
{
    SceRtcTick t;
    (void)ud;
    if (sceRtcGetCurrentTick(&t) < 0)
        return 0;
    return t.tick / 1000000ull - 62135596800ull; /* microseconds since 0001-01-01 UTC */
}

static uint64_t app_now_us(void *ud)
{
    (void)ud;
    return (uint64_t)sceKernelGetProcessTimeWide();
}

void vjo_app_net_platform(VjoPlatform *p)
{
    memset(p, 0, sizeof(*p));
    p->connect = app_connect;
    p->disconnect = app_disconnect;
    p->random = app_random;
    p->unix_time = app_time;
    p->now_us = app_now_us;
}

/* ---- the LAN ---- */

static uint32_t netctl_ipv4(int code)
{
    SceNetCtlInfo info;
    SceNetInAddr a;
    memset(&info, 0, sizeof(info));
    if (sceNetCtlInetGetInfo(code, &info) < 0)
        return 0;
    if (sceNetInetPton(SCE_NET_AF_INET, code == SCE_NETCTL_INFO_GET_NETMASK ? info.netmask : info.ip_address, &a) != 1)
        return 0;
    return sceNetNtohl(a.s_addr);
}

int vjo_app_net_local_ipv4(void *ud, uint32_t *ip, uint32_t *mask)
{
    (void)ud;
    if (!vjo_app_net_connected())
        return -1;
    *ip = netctl_ipv4(SCE_NETCTL_INFO_GET_IP_ADDRESS);
    *mask = netctl_ipv4(SCE_NETCTL_INFO_GET_NETMASK);
    return *ip ? 0 : -1;
}

int vjo_app_net_probe(void *ud, const uint32_t *ips, int n, int port, int window_us, uint8_t *open)
{
    int fds[VJO_ANKI_PROBE_MAX], pending = 0, failed = 0, last = 0;
    int64_t end;
    (void)ud;
    if (n > VJO_ANKI_PROBE_MAX)
        n = VJO_ANKI_PROBE_MAX;
    for (int i = 0; i < n; i++) {
        SceNetSockaddrIn sin;
        int st;
        open[i] = 0;
        if ((fds[i] = sceNetSocket("VjoAppProbe", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0)) < 0) {
            failed++; /* that host is not tried: said in log.txt, not taken for "closed" silently */
            last = fds[i];
            continue;
        }
        ipv4_addr(&sin, ips[i], port);
        st = connect_start(fds[i], &sin);
        if (st == 1)
            open[i] = 1;
        else if (st == 0)
            pending++;
    }
    end = (int64_t)sceKernelGetProcessTimeWide() + window_us;
    while (pending > 0 && !cancelled && (int64_t)sceKernelGetProcessTimeWide() < end) {
        sceKernelDelayThread(CONNECT_POLL_US);
        for (int i = 0; i < n; i++) {
            SceNetSockaddrIn sin;
            int st;
            if (fds[i] < 0 || open[i])
                continue;
            ipv4_addr(&sin, ips[i], port);
            if ((st = connect_poll(fds[i], &sin)) == 0)
                continue;
            open[i] = st == 1;
            pending--;
            if (st < 0) {
                sceNetSocketClose(fds[i]);
                fds[i] = -1;
            }
        }
    }
    for (int i = 0; i < n; i++)
        if (fds[i] >= 0)
            sceNetSocketClose(fds[i]);
    if (failed)
        vjo_log("anki: search: %d of %d sockets refused (0x%08X); those hosts were not tried", failed, n,
                (unsigned)last);
    return n;
}
