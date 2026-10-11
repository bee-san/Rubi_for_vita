#ifndef VJO_GAMES_TEST_CTRL_H
#define VJO_GAMES_TEST_CTRL_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef int SceUID;
typedef unsigned SceSize;
typedef struct {
    uint64_t timeStamp;
    uint32_t buttons;
    uint8_t lx, ly, rx, ry;
    uint8_t up, right, down, left, lt, rt, l1, r1;
    uint8_t triangle, circle, cross, square;
    uint8_t reserved[4];
} SceCtrlData;
#define SCE_CTRL_SELECT 0x0001u
#define SCE_CTRL_START 0x0008u
#define SCE_CTRL_LTRIGGER 0x0100u
#define SCE_CTRL_RTRIGGER 0x0200u
#define SCE_CTRL_L1 0x0400u
#define SCE_CTRL_R1 0x0800u
#define SCE_CTRL_UP 0x0010u
#define SCE_CTRL_RIGHT 0x0020u
#define SCE_CTRL_DOWN 0x0040u
#define SCE_CTRL_LEFT 0x0080u
#define SCE_CTRL_TRIANGLE 0x1000u
#define SCE_CTRL_CIRCLE 0x2000u
#define SCE_CTRL_CROSS 0x4000u
#define SCE_CTRL_SQUARE 0x8000u
#define SCE_CTRL_MODE_DIGITAL 0
#define SCE_CTRL_MODE_ANALOG 1
int ksceCtrlGetSamplingMode(int *);
int ksceCtrlSetSamplingMode(int);
int ksceCtrlPeekBufferPositive(int, SceCtrlData *, int);
SceUID ksceKernelGetProcessId(void);
SceUID ksceKernelGetThreadId(void);
uint64_t ksceKernelGetSystemTimeWide(void);
int ksceKernelMemcpyUserToKernel(void *, const void *, SceSize);
int ksceKernelMemcpyKernelToUser(void *, const void *, SceSize);
int ksceKernelSetEventFlag(SceUID, unsigned);
#endif
