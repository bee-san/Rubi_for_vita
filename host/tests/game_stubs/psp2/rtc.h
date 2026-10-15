/* Host stand-in for VitaSDK's psp2/rtc.h (the test defines the function). */
#ifndef VJO_GAME_TEST_RTC_H
#define VJO_GAME_TEST_RTC_H
#include <psp2/host_stubs.h>
typedef struct { uint64_t tick; } SceRtcTick;
int sceRtcGetCurrentTick(SceRtcTick *tick);
#endif
