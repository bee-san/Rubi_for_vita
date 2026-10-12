#ifndef VJO_TEST_RTC_H
#define VJO_TEST_RTC_H
#include "host_stubs.h"
typedef struct { uint64_t tick; } SceRtcTick;
typedef struct {
    unsigned short year, month, day, hour, minute, second;
    unsigned int microsecond;
} SceDateTime;
int sceRtcGetCurrentTick(SceRtcTick *tick);
int sceRtcGetCurrentClockLocalTime(SceDateTime *time);
#endif
