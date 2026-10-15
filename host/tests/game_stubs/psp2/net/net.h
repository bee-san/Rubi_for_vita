/* Host stand-in for VitaSDK's psp2/net/net.h: what game_ocr/net_game.c uses
 * (values as VitaSDK's psp2common/net.h; the test defines the functions). */
#ifndef VJO_GAME_TEST_NET_H
#define VJO_GAME_TEST_NET_H
#include <psp2/host_stubs.h>
#define SCE_NET_AF_INET 2
#define SCE_NET_SOCK_STREAM 1
#define SCE_NET_SOL_SOCKET 0xFFFF
#define SCE_NET_SO_SNDTIMEO 0x1005
#define SCE_NET_SO_RCVTIMEO 0x1006
#define SCE_NET_SO_NBIO 0x1100
#define SCE_NET_ERROR_EINTR 0x80410104u
#define SCE_NET_ERROR_EBUSY 0x80410110u
#define SCE_NET_ERROR_EAGAIN 0x80410123u
#define SCE_NET_ERROR_EINPROGRESS 0x80410124u
#define SCE_NET_ERROR_EALREADY 0x80410125u
#define SCE_NET_ERROR_EISCONN 0x80410138u
#define SCE_NET_ERROR_ETIMEDOUT 0x8041013Cu
typedef struct SceNetInAddr { unsigned int s_addr; } SceNetInAddr;
typedef struct SceNetSockaddr { unsigned char sa_len, sa_family; char sa_data[14]; } SceNetSockaddr;
typedef struct SceNetSockaddrIn {
    unsigned char sin_len, sin_family;
    unsigned short sin_port;
    SceNetInAddr sin_addr;
    unsigned short sin_vport;
    char sin_zero[6];
} SceNetSockaddrIn;
typedef struct SceNetInitParam { void *memory; int size, flags; } SceNetInitParam;
typedef struct SceNetResolverParam { void *a, *b, *c; } SceNetResolverParam;
int sceNetInit(SceNetInitParam *param);
int sceNetTerm(void);
int sceNetSocket(const char *name, int domain, int type, int protocol);
int sceNetConnect(int s, const SceNetSockaddr *name, unsigned int namelen);
int sceNetRecv(int s, void *buf, unsigned int len, int flags);
int sceNetSend(int s, const void *msg, unsigned int len, int flags);
int sceNetSetsockopt(int s, int level, int optname, const void *optval, unsigned int optlen);
int sceNetSocketClose(int s);
int sceNetInetPton(int af, const char *src, void *dst);
unsigned short int sceNetHtons(unsigned short int host16);
int sceNetResolverCreate(const char *name, SceNetResolverParam *param, int flags);
int sceNetResolverStartNtoa(int rid, const char *hostname, SceNetInAddr *addr, int timeout, int retry, int flags);
int sceNetResolverDestroy(int rid);
#endif
