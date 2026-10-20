/* OCR history (docs/ocr-history.md): VjoNet appends Read text: Automatically's lines to
 * history.csv, with no heap, thread or retained text; the control thread reads this game's
 * lines back from the file's end when the overlay's Up asks for earlier lines
 * (docs/line-history.md). */
#ifndef VJO_HISTORY_H
#define VJO_HISTORY_H

#include <stdint.h>
#include "../core/history_lines.h"
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

/* Line history: game uuid's earlier lines (the file's first column with its closing quote and the comma
 * after it, "vita-PCSG00415\",": core/history_lines.h) from the end of history.csv into h (h->line[0]
 * set: the current text), newest first, copied into `lines` (core/history_lines.h vjo_history_scan;
 * filter: config non_japanese_filter). One read of the file's last VJO_HISTORY_WINDOW bytes and the byte
 * before them into scratch, which is released again; an append in progress on VjoNet only makes the
 * last record incomplete, and it is skipped. Returns h->n, or the SDK's error when the file cannot be
 * read (0x80010002: there is none yet). */
int vjo_history_load(VjoHistory *h, VjoArena *lines, const char *uuid, int filter, VjoArena *scratch);

#endif
