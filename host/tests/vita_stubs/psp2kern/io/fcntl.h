#include <psp2kern/host_stubs.h>
#include <stdio.h>
#ifndef VJO_TEST_KERN_FCNTL_H
#define VJO_TEST_KERN_FCNTL_H
#define SCE_O_RDONLY 0
#define SCE_O_WRONLY 2
#define SCE_O_CREAT 0x200
#define SCE_O_TRUNC 0x400
#define SCE_O_APPEND 0x100
#define SCE_SEEK_END SEEK_END
#define SCE_SEEK_SET SEEK_SET
SceUID ksceIoOpen(const char *, int, unsigned);
SceOff ksceIoLseek(SceUID, SceOff, int);
int ksceIoPread(SceUID, void *, SceSize, SceOff);
int ksceIoRead(SceUID, void *, SceSize);
int ksceIoWrite(SceUID, const void *, SceSize);
int ksceIoClose(SceUID);
/* API 23: a shot's JPEG (kernel/game_ocr.c) */
int ksceIoPwrite(SceUID, const void *, SceSize, SceOff);
int ksceIoRemove(const char *);
int ksceIoRename(const char *, const char *);
int ksceIoSync(const char *, unsigned int);
#endif
