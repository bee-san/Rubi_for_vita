/* The reading streak's seed (core/streak.h), the Rubi app's only (settings/streak_seed.c): history.csv's records
 * replayed through the streak's rule, so a streak read before the record existed carries over. Pure C: no I/O, no
 * clock, no allocation; the app reads the file a window at a time (core/history_read.h has the windows' rules). */
#ifndef VJO_STREAK_SCAN_H
#define VJO_STREAK_SCAN_H

#include <stddef.h>
#include <stdint.h>

#include "streak.h"

/* The records that start in the window buf[0, len) of history.csv (buf[0] is the byte before the window unless
 * `whole`: the window starts at the file's first byte) added to s in the file's order, each on its local day in a time
 * zone tz_minutes east of UTC: a record counts as the Shell's writer counts a line it appends (shell/history.c), when it
 * is whole (core/history_read.h vjo_history_row), has a valid time and its line a character (vjo_line_characters). A
 * record is read once its end is known: the next record's start, or the window's end when `last` (the window ends
 * where the file does, in a line break). Returns the offset in buf where the first record not read starts (the next
 * window starts there), or len when every record was read. buf is changed: cells are unescaped in place. */
size_t vjo_streak_scan(VjoStreak *s, char *buf, size_t len, int whole, int last, int32_t tz_minutes);

/* The seed of history.csv (seed) and the record the Shell kept meanwhile (live: the lines it added before the app
 * seeded it), one record into seed: history.csv has every line the Shell counted, so seed is the record unless the
 * file lost its newest lines (moved away, cut). Then live's newest day stands, and its streak goes on from seed's when
 * it started the day after seed's newest day; the longest is the longest of both. live may be NULL or not valid. */
void vjo_streak_merge(VjoStreak *seed, const VjoStreak *live);

#endif
