/* The reading streak (the owner, 2026-10-28: "Keep streak, but put that into the Overlay. Just longest vs current
 * streak"): the overlay's Triangle view shows "Streak: current 3 days · longest 12" (docs/ocr-history.md#streak).
 *
 * A day is the Vita's local calendar day, its clock and time zone as a line is recorded, in days since 1970-01-01. A
 * day counts when history.csv gets a line with a character (core/reading_chars.h) that day, in any game: Read text:
 * Automatically's lines, which are the ones history.csv keeps.
 *
 * The record is 16 bytes, ux0:data/VitaJPOverlay/reading-streak.bin (VJO_STREAK_FILE). The Shell's history writer
 * adds the day of the lines it appends, once a day (shell/history.c), and reads the record again for the Triangle
 * view: nothing in the game scans history.csv. The Rubi app seeds it once from history.csv (settings/streak_seed.c),
 * so a streak read before the record existed carries over.
 *
 * The rule (vjo_streak_add), for a line on `day` against the newest day counted:
 * - the same day, or the day before it (a time zone moved west, a clock set back by hours): nothing changes;
 * - the next day: one day more (a time zone moved east by hours lands there too);
 * - a later day (a gap), the first day, or a day more than one before it (the clock set back by days: a clock that
 *   was set in the future and is put right): the streak starts again from that day, at 1. The longest is kept.
 * The current streak as of today (vjo_streak_current) is the record's while its newest day is today or yesterday
 * (or tomorrow: the time zone moved west); else 0, a streak that has ended.
 *
 * Pure C: no I/O, no clock, no allocation. */
#ifndef VJO_STREAK_H
#define VJO_STREAK_H

#include <stdint.h>

/* in ux0:data/VitaJPOverlay (VJO_DATA_DIR), named as the Shell names its reading caches (reading-<name>.bin: a game's
 * is reading-vita-PCSG00415.bin), so the Shell builds its path with the format it has */
#define VJO_STREAK_NAME "streak"
#define VJO_STREAK_FILE "reading-" VJO_STREAK_NAME ".bin"
#define VJO_STREAK_MAGIC 0x31525352u        /* "RSR1" in the file's (little-endian) bytes */
#define VJO_STREAK_NONE INT32_MIN           /* no day counted yet */
#define VJO_STREAK_DAY_US 86400000000ull

typedef struct {
    uint32_t magic;   /* VJO_STREAK_MAGIC */
    int32_t day;      /* the newest day counted; VJO_STREAK_NONE */
    uint16_t current; /* the days in a row that end at `day` (0 with no day) */
    uint16_t longest; /* the most days in a row */
    uint32_t seeded;  /* VJO_STREAK_SEEDED once the Rubi app counted history.csv into it; the Shell keeps it */
} VjoStreak;
#define VJO_STREAK_SEEDED 1u

/* A new record: no day counted, not seeded. */
void vjo_streak_init(VjoStreak *s);
/* 1 when s holds a record as these functions write it: its magic, and a day with a current of at least 1 and no
 * more than the longest (or no day and no streak). */
int vjo_streak_valid(const VjoStreak *s);
/* A line read on `day` (VJO_STREAK_NONE: none, nothing changes): 1 when the record changed (it is to be written). */
int vjo_streak_add(VjoStreak *s, int32_t day);
/* The current streak as the Triangle view shows it on `today`: the record's while its newest day is within a day of
 * today, else 0. */
uint32_t vjo_streak_current(const VjoStreak *s, int32_t today);
/* The local day of a Unix time (microseconds, UTC) in a time zone tz_minutes east of UTC: VJO_STREAK_NONE for 0 (an
 * invalid time) or a time past the year 2^31 days. */
int32_t vjo_streak_day(uint64_t us, int32_t tz_minutes);

#endif
