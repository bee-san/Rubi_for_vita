/* Host boundary for shell/log_vita.c (host/tests/test_shell_log.c): an
 * in-memory file system, counted, and the few SceShell calls the logger makes.
 * The Vita build never includes these headers. */
#ifndef VJO_TEST_LOG_STUBS_H
#define VJO_TEST_LOG_STUBS_H
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int SceUID;
typedef unsigned int SceSize;
typedef int64_t SceOff;
typedef struct {
    unsigned st_mode;
    SceOff st_size;
} SceIoStat;

#define SCE_O_RDONLY 0x0001
#define SCE_O_WRONLY 0x0002
#define SCE_O_APPEND 0x0100
#define SCE_O_CREAT 0x0200
#define SCE_O_TRUNC 0x0400

SceUID sceIoOpen(const char *path, int flags, int mode);
int sceIoClose(SceUID fd);
int sceIoWrite(SceUID fd, const void *data, SceSize len);
int sceIoGetstat(const char *path, SceIoStat *st);
int sceIoRemove(const char *path);
int sceIoRename(const char *from, const char *to);
int sceIoMkdir(const char *path, int mode);

#define sceClibMemset memset
#define sceClibMemcpy memcpy
int sceClibVsnprintf(char *dst, SceSize cap, const char *fmt, va_list ap);

SceUID sceKernelCreateMutex(const char *name, unsigned attr, int count, void *opt);
int sceKernelDeleteMutex(SceUID uid);
int sceKernelLockMutex(SceUID uid, int count, unsigned *timeout);
int sceKernelUnlockMutex(SceUID uid, int count);
SceUID sceKernelGetThreadId(void);
uint64_t sceKernelGetProcessTimeWide(void);

typedef struct {
    uint32_t s_addr;
} SceNetInAddr;
typedef struct {
    uint8_t sin_len, sin_family;
    uint16_t sin_port;
    SceNetInAddr sin_addr;
    char sin_zero[8];
} SceNetSockaddrIn;
typedef struct {
    uint8_t sa_len, sa_family;
    char sa_data[14];
} SceNetSockaddr;
#define SCE_NET_AF_INET 2
#define SCE_NET_SOCK_DGRAM 2
int sceNetSocket(const char *name, int domain, int type, int protocol);
int sceNetSocketClose(int fd);
int sceNetSendto(int fd, const void *buf, unsigned len, int flags, const SceNetSockaddr *to, unsigned tolen);
uint16_t sceNetHtons(uint16_t v);
int sceNetInetPton(int af, const char *src, void *dst);
#endif
