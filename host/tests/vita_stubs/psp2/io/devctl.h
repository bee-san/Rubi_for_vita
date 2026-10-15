/* Host-only device control (psp2/io/devctl.h): the free space of a device,
 * command 0x3001 as VitaShell reads it. The host suites that call it implement it. */
#ifndef VJO_TEST_DEVCTL_H
#define VJO_TEST_DEVCTL_H
#include <psp2/io/fcntl.h>
typedef struct {
    SceOff max_size;
    SceOff free_size;
    SceSize cluster_size;
    void *unk;
} SceIoDevInfo;
int sceIoDevctl(const char *dev, unsigned int cmd, void *indata, int inlen, void *outdata, int outlen);
#endif
