#ifndef VJO_GAME_WORKER_H
#define VJO_GAME_WORKER_H
#include "../include/vjo_game_ocr.h"
int vjo_game_ocr_run_request(const VjoGameOcrRequest *, VjoGameOcrResult *);
int vjo_game_ocr_worker_start(void);
int vjo_game_ocr_worker_stop(void);
#endif
