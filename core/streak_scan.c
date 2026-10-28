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
    if (seed->day == VJO_STREAK_NONE) {
        seed->day = live->day;
        seed->current = live->current;
    } else {
        /* each record's run (current days in a row ending at its day) is days with a line: two runs that touch or
         * overlap are one run, from the earlier start to the later end; else the later one is the current one */
        int64_t e1 = seed->day, s1 = e1 - seed->current + 1, e2 = live->day, s2 = e2 - live->current + 1;
        int64_t end = e1 > e2 ? e1 : e2, start;
        if ((s1 > s2 ? s1 : s2) <= (e1 < e2 ? e1 : e2) + 1)
            start = s1 < s2 ? s1 : s2;
        else
            start = e1 > e2 ? s1 : s2;
        seed->day = (int32_t)end;
        seed->current = (uint16_t)(end - start + 1 > 0xFFFF ? 0xFFFF : end - start + 1);
    }
    if (seed->current > longest)
        longest = seed->current;
    seed->longest = (uint16_t)longest;
}
