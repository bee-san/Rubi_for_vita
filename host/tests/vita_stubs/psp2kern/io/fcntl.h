#include <psp2kern/host_stubs.h>
#include <stdio.h>
#define SCE_O_RDONLY 0
#define SCE_SEEK_END SEEK_END
SceUID ksceIoOpen(const char *, int, unsigned);
SceOff ksceIoLseek(SceUID, SceOff, int);
int ksceIoPread(SceUID, void *, SceSize, SceOff);
int ksceIoClose(SceUID);
