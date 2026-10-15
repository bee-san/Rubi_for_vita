/* game_ocr/net_game.c, the game process's network, with SceSysmodule and
 * libnet failure-injected: the worker loads SCE_SYSMODULE_NET only when the
 * game has not and never unloads it, starts libnet only when the game's is not
 * up and terminates only its own, and bounds a connect by its timeout and the
 * job's cancellation (docs/shell-slim.md section 3). */
#include "acutest.h"
#include <psp2/host_stubs.h>
#include "../../game_ocr/net_game.c"

static uint64_t clock_now;
uint64_t sceKernelGetProcessTimeWide(void) { return clock_now; }
int sceKernelDelayThread(unsigned us) { clock_now += us; return 0; }

/* ---- SceSysmodule ---- */
static int net_loaded, load_calls, unload_calls, load_error;
int sceSysmoduleIsLoaded(SceSysmoduleModuleId id)
{ TEST_CHECK(id == SCE_SYSMODULE_NET); return net_loaded ? SCE_SYSMODULE_LOADED : SCE_SYSMODULE_ERROR_UNLOADED; }
int sceSysmoduleLoadModule(SceSysmoduleModuleId id)
{ TEST_CHECK(id == SCE_SYSMODULE_NET); load_calls++; if (load_error) return load_error; net_loaded = 1; return 0; }
int sceSysmoduleUnloadModule(SceSysmoduleModuleId id) { unload_calls++; net_loaded = 0; return 0; }

/* ---- libnet ---- */
static int libnet_up, init_calls, term_calls, term_error, init_error;
static void *init_memory; static int init_size;
int sceNetInit(SceNetInitParam *param)
{
    init_calls++;
    TEST_CHECK(net_loaded);
    if (libnet_up) return (int)SCE_NET_ERROR_EBUSY;
    if (init_error) return init_error;
    init_memory = param->memory; init_size = param->size;
    TEST_CHECK(param->flags == 0);
    libnet_up = 1;
    return 0;
}
int sceNetTerm(void) { term_calls++; if (term_error) return term_error; libnet_up = 0; return 0; }

static int sockets, closes, open_sockets, connect_calls, connect_busy, connect_error, resolves, resolve_error;
static int pton_ip, opt_rcv, opt_snd, nbio_on, nbio_off;
static int cancel_at_connect;
int sceNetSocket(const char *name, int domain, int type, int protocol)
{ TEST_CHECK(libnet_up && domain == SCE_NET_AF_INET && type == SCE_NET_SOCK_STREAM); sockets++; open_sockets++; return 40 + sockets; }
int sceNetSocketClose(int s) { closes++; open_sockets--; return 0; }
int sceNetSetsockopt(int s, int level, int optname, const void *optval, unsigned int optlen)
{
    int v = *(const int *)optval;
    TEST_CHECK(level == SCE_NET_SOL_SOCKET && optlen == sizeof(int));
    if (optname == SCE_NET_SO_RCVTIMEO) opt_rcv = v;
    if (optname == SCE_NET_SO_SNDTIMEO) opt_snd = v;
    if (optname == SCE_NET_SO_NBIO) { if (v) nbio_on++; else nbio_off++; }
    return 0;
}
int sceNetConnect(int s, const SceNetSockaddr *name, unsigned int namelen)
{
    const SceNetSockaddrIn *sin = (const SceNetSockaddrIn *)name;
    TEST_CHECK(namelen == sizeof(SceNetSockaddrIn) && sin->sin_family == SCE_NET_AF_INET && sin->sin_port == 443);
    TEST_CHECK(sin->sin_addr.s_addr == 0x0100007Fu);
    connect_calls++;
    if (connect_calls == 1) return (int)SCE_NET_ERROR_EINPROGRESS;
    if (connect_error) return connect_error;
    if (connect_calls <= 1 + connect_busy) return (int)SCE_NET_ERROR_EALREADY;
    return (int)SCE_NET_ERROR_EISCONN;
}
/* reads and writes: the first `stall` calls run out their slice (EAGAIN), the rest move the bytes */
static int recv_stall, send_stall, recv_calls, send_calls, last_fd;
int sceNetRecv(int s, void *buf, unsigned int len, int flags)
{
    recv_calls++; last_fd = s;
    if (recv_stall > 0) { recv_stall--; clock_now += VJO_GAME_NET_SLICE_US; return (int)SCE_NET_ERROR_EAGAIN; }
    memset(buf, 'r', len); return (int)len;
}
int sceNetSend(int s, const void *msg, unsigned int len, int flags)
{
    send_calls++; last_fd = s;
    if (send_stall > 0) { send_stall--; clock_now += VJO_GAME_NET_SLICE_US; return (int)SCE_NET_ERROR_EAGAIN; }
    return (int)len;
}
int sceNetInetPton(int af, const char *src, void *dst)
{
    if (!strcmp(src, "127.0.0.1")) { ((SceNetInAddr *)dst)->s_addr = 0x0100007Fu; pton_ip++; return 1; }
    return 0;
}
unsigned short int sceNetHtons(unsigned short int v) { return v; } /* the test's byte order is the wire's */
int sceNetResolverCreate(const char *name, SceNetResolverParam *param, int flags) { TEST_CHECK(libnet_up); return 7; }
int sceNetResolverStartNtoa(int rid, const char *host, SceNetInAddr *addr, int timeout, int retry, int flags)
{
    resolves++;
    TEST_CHECK(rid == 7 && timeout > 0 && timeout <= RESOLVE_US && retry == 0);
    if (resolve_error) return resolve_error;
    addr->s_addr = 0x0100007Fu;
    return 0;
}
int sceNetResolverDestroy(int rid) { return 0; }

static int rng_calls; static size_t rng_largest;
int sceKernelGetRandomNumber(void *out, SceSize size) { rng_calls++; if (size > rng_largest) rng_largest = size; memset(out, 0x5a, size); return 0; }
static int rtc_error;
int sceRtcGetCurrentTick(SceRtcTick *t) { if (rtc_error) return rtc_error; t->tick = (62135596800ull + 1760000000ull) * 1000000ull; return 0; }

static int cancel_flag;
static int cancelled(void *ud) { (void)ud; return cancel_flag || (cancel_at_connect && connect_calls >= cancel_at_connect); }

static unsigned char pool[VJO_GAME_NET_POOL];

static void reset(void)
{
    net_loaded = load_calls = unload_calls = load_error = 0;
    libnet_up = init_calls = term_calls = term_error = init_error = 0;
    init_memory = NULL; init_size = 0;
    sockets = closes = open_sockets = connect_calls = connect_busy = connect_error = resolves = resolve_error = 0;
    pton_ip = opt_rcv = opt_snd = nbio_on = nbio_off = cancel_at_connect = cancel_flag = 0;
    recv_stall = send_stall = recv_calls = send_calls = last_fd = 0;
    rng_calls = 0; rng_largest = 0; rtc_error = 0;
    clock_now = 1000;
}

static void test_worker_starts_only_what_the_game_has_not(void)
{
    VjoGameNet n;
    /* a game without network: the worker loads NET and starts libnet on its pool, and ends libnet */
    reset();
    TEST_CHECK(vjo_game_net_open(&n, pool, sizeof(pool), cancelled, NULL) == VJO_OK);
    TEST_CHECK(n.sysmodule == VJO_GNET_OURS && n.libnet == VJO_GNET_OURS && load_calls == 1 && init_calls == 1);
    TEST_CHECK(init_memory == pool && init_size == (int)sizeof(pool));
    TEST_CHECK(vjo_game_net_close(&n) == 0 && term_calls == 1 && !libnet_up);
    TEST_CHECK(unload_calls == 0 && net_loaded); /* never unloaded: the game may have loaded it meanwhile */
    /* the next job: NET is there now (ours, kept), libnet is started and ended again */
    TEST_CHECK(vjo_game_net_open(&n, pool, sizeof(pool), cancelled, NULL) == VJO_OK);
    TEST_CHECK(n.sysmodule == VJO_GNET_GAME && load_calls == 1 && n.libnet == VJO_GNET_OURS);
    TEST_CHECK(vjo_game_net_close(&n) == 0 && term_calls == 2);
    /* a game with its own network up: used, never terminated */
    reset();
    net_loaded = libnet_up = 1;
    TEST_CHECK(vjo_game_net_open(&n, pool, sizeof(pool), cancelled, NULL) == VJO_OK);
    TEST_CHECK(n.sysmodule == VJO_GNET_GAME && n.libnet == VJO_GNET_GAME && load_calls == 0);
    TEST_CHECK((unsigned)n.init_rc == SCE_NET_ERROR_EBUSY);
    TEST_CHECK(vjo_game_net_close(&n) == 0 && term_calls == 0 && libnet_up);
    TEST_CHECK(unload_calls == 0);
}

static void test_failures_are_reported(void)
{
    VjoGameNet n;
    reset();
    load_error = (int)0x805A10FFu;
    TEST_CHECK(vjo_game_net_open(&n, pool, sizeof(pool), cancelled, NULL) == VJO_E_NET);
    TEST_CHECK(n.sysmodule == VJO_GNET_FAILED && n.sysmodule_rc == load_error && init_calls == 0);
    TEST_CHECK(strstr(n.log, "loading SCE_SYSMODULE_NET failed 0x805A10FF") != NULL);
    TEST_CHECK(vjo_game_net_close(&n) == 0 && term_calls == 0);
    reset();
    init_error = (int)0x80410106u;
    TEST_CHECK(vjo_game_net_open(&n, pool, sizeof(pool), cancelled, NULL) == VJO_E_NET);
    TEST_CHECK(n.libnet == VJO_GNET_FAILED && strstr(n.log, "sceNetInit failed 0x80410106") != NULL);
    TEST_CHECK(vjo_game_net_close(&n) == 0 && term_calls == 0);
    /* too small a pool: libnet is not started on it */
    reset();
    TEST_CHECK(vjo_game_net_open(&n, pool, sizeof(pool) - 1, cancelled, NULL) == VJO_E_NET && init_calls == 0);
    /* a failed sceNetTerm keeps the pool: the caller must not free it */
    reset();
    TEST_CHECK(vjo_game_net_open(&n, pool, sizeof(pool), cancelled, NULL) == VJO_OK);
    term_error = (int)0x80410101u;
    TEST_CHECK(vjo_game_net_close(&n) < 0 && n.libnet == VJO_GNET_OURS && n.up);
    TEST_CHECK(strstr(n.log, "sceNetTerm failed 0x80410101: its pool is kept") != NULL);
    term_error = 0;
    TEST_CHECK(vjo_game_net_close(&n) == 0 && !n.up && n.libnet == VJO_GNET_OURS); /* a retry succeeds; the route stays */
}

static void test_connect_resolves_waits_and_cancels(void)
{
    VjoGameNet n;
    VjoPlatform p;
    VjoConn c;
    char b[8];
    reset();
    TEST_ASSERT(vjo_game_net_open(&n, pool, sizeof(pool), cancelled, NULL) == VJO_OK);
    vjo_game_net_platform(&n, &p);
    /* a host name goes through the resolver; the connect polls until it is in */
    connect_busy = 3;
    TEST_CHECK(p.connect(p.ud, "lens.example", 443, 5000000, 0, &c) == VJO_OK);
    TEST_CHECK(resolves == 1 && connect_calls == 5 && nbio_on == 1 && nbio_off == 1);
    /* reads and writes wait in slices: a cancelled job is seen within one */
    TEST_CHECK(opt_rcv == VJO_GAME_NET_SLICE_US && opt_snd == VJO_GAME_NET_SLICE_US);
    TEST_CHECK(c.send(c.ctx, "abc", 3) == 3 && c.recv(c.ctx, b, sizeof(b)) == (int)sizeof(b));
    TEST_CHECK(last_fd == 41 && n.fd == 41);
    /* one connection a job: core/net.c closes each before the next */
    VjoConn c2;
    TEST_CHECK(p.connect(p.ud, "127.0.0.1", 443, 5000000, 0, &c2) == VJO_E_NET && sockets == 1);
    p.disconnect(p.ud, &c);
    TEST_CHECK(open_sockets == 0 && n.fd == -1);
    /* an address needs no resolver; the io limit is the request's when it has one */
    connect_calls = 0; connect_busy = 0;
    TEST_CHECK(p.connect(p.ud, "127.0.0.1", 443, 5000000, 3000000, &c) == VJO_OK);
    TEST_CHECK(resolves == 1 && pton_ip == 1 && n.io_us == 3000000);
    /* a slow reply: slices run out until it comes; one that never comes ends at the io limit */
    recv_stall = 5;
    TEST_CHECK(c.recv(c.ctx, b, sizeof(b)) == (int)sizeof(b) && recv_calls == 7);
    recv_stall = 1000; recv_calls = 0;
    TEST_CHECK(c.recv(c.ctx, b, sizeof(b)) == -1 && recv_calls == 3000000 / VJO_GAME_NET_SLICE_US);
    send_stall = 1000; send_calls = 0;
    TEST_CHECK(c.send(c.ctx, "abc", 3) == -1 && send_calls == 3000000 / VJO_GAME_NET_SLICE_US);
    /* the job is cancelled while a read waits: it ends after that slice */
    recv_stall = 1000; recv_calls = 0; cancel_flag = 1;
    TEST_CHECK(c.recv(c.ctx, b, sizeof(b)) == -1 && recv_calls == 1);
    cancel_flag = 0; recv_stall = send_stall = 0;
    /* another error ends it at once */
    p.disconnect(p.ud, &c);
    /* the resolver fails: no network, and the log says so */
    resolve_error = (int)0x804101E2u;
    TEST_CHECK(p.connect(p.ud, "lens.example", 443, 5000000, 0, &c) == VJO_E_NET);
    TEST_CHECK(strstr(n.log, "net: resolve lens.example failed 0x804101E2") != NULL && open_sockets == 0);
    resolve_error = 0;
    /* a connect that never completes ends at its timeout, the socket closed */
    connect_calls = 0; connect_busy = 1000000;
    TEST_CHECK(p.connect(p.ud, "127.0.0.1", 443, 200000, 0, &c) == VJO_E_NET);
    TEST_CHECK(open_sockets == 0 && strstr(n.log, "connect 127.0.0.1 failed 0x8041013C") != NULL);
    /* the job is cancelled while the connect waits: it ends at once, quietly */
    unsigned logged = n.log_used;
    connect_calls = 0; connect_busy = 1000000; cancel_at_connect = 3;
    TEST_CHECK(p.connect(p.ud, "127.0.0.1", 443, 5000000, 0, &c) == VJO_E_CANCELLED);
    TEST_CHECK(open_sockets == 0 && connect_calls == 3 && n.log_used == logged);
    cancel_at_connect = 0;
    /* cancelled before: no socket at all */
    cancel_flag = 1;
    int before = sockets;
    TEST_CHECK(p.connect(p.ud, "127.0.0.1", 443, 5000000, 0, &c) == VJO_E_CANCELLED && sockets == before);
    cancel_flag = 0;
    /* a socket left open by a failed request is closed with the network */
    connect_calls = 0; connect_busy = 0;
    TEST_CHECK(p.connect(p.ud, "127.0.0.1", 443, 5000000, 0, &c) == VJO_OK && open_sockets == 1);
    TEST_CHECK(vjo_game_net_close(&n) == 0 && open_sockets == 0 && n.fd == -1);
}

static void test_platform_services(void)
{
    VjoGameNet n;
    VjoPlatform p;
    unsigned char seed[150];
    reset();
    TEST_ASSERT(vjo_game_net_open(&n, pool, sizeof(pool), cancelled, NULL) == VJO_OK);
    vjo_game_net_platform(&n, &p);
    p.random(p.ud, seed, sizeof(seed));
    TEST_CHECK(rng_calls == 3 && rng_largest == 64 && seed[149] == 0x5a);
    TEST_CHECK(p.unix_time(p.ud) == 1760000000ull);
    rtc_error = (int)0x80251000u;
    TEST_CHECK(p.unix_time(p.ud) == 0 && strstr(n.log, "rtc: no time (0x80251000)") != NULL);
    clock_now = 123456;
    TEST_CHECK(p.now_us(p.ud) == 123456 && p.pool == NULL && p.file_open == NULL);
    /* the log keeps whole lines within its buffer, and counts what it drops */
    char line[150];
    memset(line, 'x', sizeof(line) - 1);
    line[sizeof(line) - 1] = 0;
    for (int i = 0; i < 10; i++) p.log(p.ud, line);
    TEST_CHECK(n.log_used < sizeof(n.log) && n.log[n.log_used] == 0 && n.log[n.log_used - 1] == '\n');
    TEST_CHECK(n.log_dropped > 0);
    TEST_CHECK(vjo_game_net_close(&n) == 0);
}

TEST_LIST = {
    {"worker_starts_only_what_the_game_has_not", test_worker_starts_only_what_the_game_has_not},
    {"failures_are_reported", test_failures_are_reported},
    {"connect_resolves_waits_and_cancels", test_connect_resolves_waits_and_cancels},
    {"platform_services", test_platform_services},
    {NULL, NULL}};
