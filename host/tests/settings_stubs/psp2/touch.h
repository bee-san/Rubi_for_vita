/* Host-only SceTouch boundary for the Settings app harness. Front-panel
 * coordinates are 1920 x 1088, twice the screen's. */
#ifndef VJO_TEST_SETTINGS_TOUCH_H
#define VJO_TEST_SETTINGS_TOUCH_H
#include <stdint.h>
#define SCE_TOUCH_PORT_FRONT 0
#define SCE_TOUCH_SAMPLING_STATE_START 1
typedef struct {
    uint8_t id, force;
    uint16_t x, y;
    uint8_t reserved[8];
    uint16_t info;
} SceTouchReport;
typedef struct {
    uint64_t timeStamp;
    uint32_t status;
    uint32_t reportNum;
    SceTouchReport report[8];
} SceTouchData;
int sceTouchSetSamplingState(int port, int state);
int sceTouchPeek(int port, SceTouchData *data, int count);
#endif
