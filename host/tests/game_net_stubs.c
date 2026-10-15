/* Default SDK stand-ins for game_ocr/net_game.c in host tests of the game
 * worker that run no Lens job (test_game_vocr.c and the others that include
 * game_ocr/main.c): SceSysmodule cannot load the network, so a Lens request
 * there fails cleanly with VJO_E_NET. Every one is weak: a test that runs Lens
 * jobs (test_game_lens.c, test_game_net.c) defines its own. */
#include <psp2/host_stubs.h>
#include <psp2/kernel/rng.h>
#include <psp2/net/net.h>
#include <psp2/rtc.h>
#include <psp2/sysmodule.h>

#define WEAK __attribute__((weak))

WEAK int sceSysmoduleIsLoaded(SceSysmoduleModuleId id) { (void)id; return SCE_SYSMODULE_ERROR_UNLOADED; }
WEAK int sceSysmoduleLoadModule(SceSysmoduleModuleId id) { (void)id; return (int)0x805A10FFu; }
WEAK int sceSysmoduleUnloadModule(SceSysmoduleModuleId id) { (void)id; return SCE_SYSMODULE_ERROR_UNLOADED; }
WEAK int sceNetInit(SceNetInitParam *param) { (void)param; return -1; }
WEAK int sceNetTerm(void) { return 0; }
WEAK int sceNetSocket(const char *name, int domain, int type, int protocol)
{ (void)name; (void)domain; (void)type; (void)protocol; return -1; }
WEAK int sceNetConnect(int s, const SceNetSockaddr *name, unsigned int namelen) { (void)s; (void)name; (void)namelen; return -1; }
WEAK int sceNetRecv(int s, void *buf, unsigned int len, int flags) { (void)s; (void)buf; (void)len; (void)flags; return -1; }
WEAK int sceNetSend(int s, const void *msg, unsigned int len, int flags) { (void)s; (void)msg; (void)len; (void)flags; return -1; }
WEAK int sceNetSetsockopt(int s, int level, int optname, const void *optval, unsigned int optlen)
{ (void)s; (void)level; (void)optname; (void)optval; (void)optlen; return 0; }
WEAK int sceNetSocketClose(int s) { (void)s; return 0; }
WEAK int sceNetInetPton(int af, const char *src, void *dst) { (void)af; (void)src; (void)dst; return 0; }
WEAK unsigned short int sceNetHtons(unsigned short int v) { return (unsigned short)(v << 8 | v >> 8); }
WEAK int sceNetResolverCreate(const char *name, SceNetResolverParam *param, int flags) { (void)name; (void)param; (void)flags; return -1; }
WEAK int sceNetResolverStartNtoa(int rid, const char *host, SceNetInAddr *addr, int timeout, int retry, int flags)
{ (void)rid; (void)host; (void)addr; (void)timeout; (void)retry; (void)flags; return -1; }
WEAK int sceNetResolverDestroy(int rid) { (void)rid; return 0; }
WEAK int sceRtcGetCurrentTick(SceRtcTick *tick) { (void)tick; return -1; }
WEAK int sceKernelGetRandomNumber(void *output, SceSize size) { memset(output, 0, size); return 0; }
