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

/* The total of history.csv's characters (docs/ocr-history.md#total-characters-read), which the overlay's
 * Triangle view shows. The writer adds each record's (vjo_history_append); the rest is counted from the
 * file once a view asks for it. */
/* The Triangle view opened (paf thread, no lock held): the total is counted unless it is known. */
void vjo_history_total_want(void);
/* Control-thread context: current game's identity and today's local-midnight UTC bounds. */
int vjo_history_context(VjoReadingScope *scope);
/* Under view lock: block 0 today, 1 game total; separate styles are applied by the overlay. */
void vjo_history_stats_block(char *buf, size_t cap, int today);
/* Under the view lock: the total as the view shows it, written into buf, or "counting…" or "unavailable". */
const char *vjo_history_total_text(char *buf, size_t cap);
/* Under the view lock: all three reading statistics, or the count state. */
void vjo_history_total_summary(char *buf, size_t cap);
/* The reading streak (core/streak.h, ux0:data/VitaJPOverlay/reading-streak.bin), no lock held: add, after the writer
 * appended a line with a character (VjoNet), adds today to the record; else (the control thread, as the Triangle
 * view asks for its statistics) it is only read. Either way the view's streak is set from it. */
void vjo_history_streak(int add);
/* Under the view lock: the Triangle view's streak line, "\n\nStreak: current 3 days · longest 12"; "" until the record
 * has been read since the Shell started (a line recorded, or the view's request). */
void vjo_history_streak_text(char *buf, size_t cap);
/* Control thread: handles a request with only the checked 112-byte cache and streak. A valid cached prefix
 * is immediately displayable. Only with background set (closed overlay, idle worker, paced by the caller)
 * may it scan one 24 KiB CSV window or checkpoint. 1 means more windows remain; never wake immediately
 * for these. A completed count and game exit persist the total; incremental checkpoints are 30 minutes apart. */
int vjo_history_total_step(VjoArena *scratch, int background);
/* Control thread, at a game's exit: history_total.bin gets the total if the writer added to it. */
void vjo_history_total_keep(void);
/* The Shell's stop, its threads joined: a count's history.csv is closed. */
void vjo_history_total_stop(void);

#endif
