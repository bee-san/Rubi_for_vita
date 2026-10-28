/* The streak's seed (settings/streak_seed.c) is not what the Games and OCR-page harnesses drive: it has its own suite
 * (host/tests/test_settings_streak.c, on a temporary card with a history.csv). Here it reads nothing; it records the
 * app's start and quit (which stops and joins a seed still running). */
#include "../../../settings/streak_seed.h"

int vjo_streak_seed_stub_starts, vjo_streak_seed_stub_quits;
void vjo_streak_seed_start(void) { vjo_streak_seed_stub_starts++; }
void vjo_streak_seed_quit(void) { vjo_streak_seed_stub_quits++; }
