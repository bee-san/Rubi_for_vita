#include "streak.h"

#include <string.h>

/* Days are small numbers (a day since 1970 of a year 1 to 9999 is within 3,000,000 of 0) and VJO_STREAK_NONE is
 * INT32_MIN, so a difference of two days taken in 32 bits is exact, and the difference between NONE and a day is
 * never -1, 0 or 1: the rules below need no 64-bit arithmetic (the Shell's code pages have little room). */

void vjo_streak_init(VjoStreak *s)
{
    memset(s, 0, sizeof(*s));
    s->magic = VJO_STREAK_MAGIC;
    s->day = VJO_STREAK_NONE;
}

int vjo_streak_valid(const VjoStreak *s)
{
    if (s->magic != VJO_STREAK_MAGIC)
        return 0;
    if (s->day == VJO_STREAK_NONE)
        return !s->current && !s->longest;
    return s->current >= 1 && s->current <= s->longest;
}

int vjo_streak_add(VjoStreak *s, int32_t day)
{
    uint32_t gap = (uint32_t)day - (uint32_t)s->day;
    if (day == VJO_STREAK_NONE || gap + 1u <= 1u)
        return 0; /* counted already: the same day, or the day before it (a time zone moved west) */
    /* the next day: one more; else the first day, a gap or a clock set back by days: a new streak, 1 */
    s->current = gap == 1u ? (uint16_t)(s->current + (s->current < 0xFFFFu)) : (uint16_t)1u;
    if (s->current > s->longest)
        s->longest = s->current;
    s->day = day;
    return 1;
}

uint32_t vjo_streak_current(const VjoStreak *s, int32_t today)
{
    /* the newest day is today, yesterday or (a time zone moved west) tomorrow; NONE on either side is never */
    return (uint32_t)today - (uint32_t)s->day + 1u <= 2u ? s->current : 0;
}

int32_t vjo_streak_day(uint64_t us, int32_t tz_minutes)
{
    int64_t t;
    if (!us || us > (uint64_t)INT64_MAX / 2)
        return VJO_STREAK_NONE;
    t = (int64_t)us + (int64_t)tz_minutes * 60000000ll;
    /* floor division: a local time before 1970 is day -1 */
    t = t >= 0 ? t / (int64_t)VJO_STREAK_DAY_US : -((-t + (int64_t)VJO_STREAK_DAY_US - 1) / (int64_t)VJO_STREAK_DAY_US);
    return t > -100000000 && t < 100000000 ? (int32_t)t : VJO_STREAK_NONE;
}
