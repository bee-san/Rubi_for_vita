#include "streak_scan.h"

#include "history_lines.h"
#include "history_read.h"

size_t vjo_streak_scan(VjoStreak *s, char *buf, size_t len, int whole, int last, int32_t tz_minutes)
{
    size_t start = vjo_history_next_start(buf, 0, len, whole);
    while (start < len) {
        size_t next = vjo_history_next_start(buf, start + 1, len, whole);
        VjoHistoryRow row;
        if (next == len && !last)
            break; /* its end is past the window: the next window starts with it */
        if (!vjo_history_row(buf + start, buf + next, 4, &row) && row.time_us &&
            vjo_line_characters(row.line, row.len[3]))
            vjo_streak_add(s, vjo_streak_day(row.time_us, tz_minutes));
        start = next;
    }
    return start;
}

void vjo_streak_merge(VjoStreak *seed, const VjoStreak *live)
{
    uint32_t longest;
    if (!live || !vjo_streak_valid(live) || live->day == VJO_STREAK_NONE)
        return;
    longest = live->longest > seed->longest ? live->longest : seed->longest;
    if (seed->day == VJO_STREAK_NONE || (int64_t)live->day > seed->day) {
        /* history.csv lost the newest lines: the Shell's newest day, its streak joined to the seed's when it began
         * the day after the seed's newest day (a run of live->current days ending at live->day) */
        uint32_t current = live->current;
        if (seed->day != VJO_STREAK_NONE && (int64_t)live->day - live->current == seed->day)
            current += seed->current;
        seed->day = live->day;
        seed->current = (uint16_t)(current > 0xFFFFu ? 0xFFFFu : current);
        if (seed->current > longest)
            longest = seed->current;
    }
    seed->longest = (uint16_t)longest;
}
