/* Hidden OCR history. One caller (VjoNet), no heap, thread or retained text. */
#ifndef VJO_HISTORY_H
#define VJO_HISTORY_H

#include <stdint.h>
#include "../include/vjo_game_ocr.h"

typedef struct {
    char name[128];
    char title_id[12];
    const char *platform;
    const char *model;
    const VjoGameOcrResult *result;
    uint64_t time_us;       /* Unix UTC microseconds, captured before publishing the result. */
    uint64_t monotonic_us;  /* SceShell process time at the start of the job. */
    uint32_t scene, region_seq, elapsed_us;
} VjoHistoryRecord;

/* 1 appended, 0 empty/adjacent duplicate, negative on error. Files grow only
 * on disk. The result and its text must remain valid until this returns.
 * Ordinary I/O failures roll back this append when the filesystem permits;
 * power loss during a write may leave an incomplete final CSV record. */
int vjo_history_append(const VjoHistoryRecord *record);
/* Forget only the previous fingerprint; does not touch the history file. */
void vjo_history_reset(void);

#endif
