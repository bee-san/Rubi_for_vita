#ifndef VJO_KERNEL_GAME_OCR_H
#define VJO_KERNEL_GAME_OCR_H

#include "../include/vjo_game_ocr.h"
#include <psp2kern/types.h>

/* All *_locked helpers require g.lock. No helper takes g.game_lock. */
/* g.mevf bit: the kernel worker finished a model operation. */
#define MEV_DONE 0x1u
void game_ocr_init(void);
int game_ocr_capture_busy_locked(void);
int game_ocr_raw_caller_locked(SceUID pid);
int game_ocr_shutdown_locked(void);
void game_ocr_shell_changed_locked(void);
void game_ocr_foreground_changed_locked(void);
/* Called only by the existing kernel worker; disk I/O runs without g.lock. */
void game_ocr_model_tick(void);
/* Called with g.game_lock held; takes g.lock in this fixed order. */
void game_ocr_process_gone(SceUID pid);

#endif
