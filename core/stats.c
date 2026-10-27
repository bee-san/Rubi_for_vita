#include "stats.h"

#include <limits.h>
#include <string.h>

#include "history_read.h"

uint32_t vjo_stats_check(const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 16777619u;
    return h;
}

void vjo_stats_reset(VjoStats *s, int32_t tz_minutes, uint32_t rule)
{
    memset(s, 0, sizeof(*s));
    s->tz_minutes = tz_minutes;
    s->rule = rule;
    s->open = -1;
}

int32_t vjo_stats_day_in(uint64_t us, int32_t tz_minutes)
{
    int64_t t;
    if (!us || us > (uint64_t)INT64_MAX / 2)
        return INT32_MIN;
    t = (int64_t)us + (int64_t)tz_minutes * 60000000ll;
    /* floor division: a time before 1970 in this zone is day -1 */
    return (int32_t)(t >= 0 ? t / VJO_STATS_DAY_US : -((-t + VJO_STATS_DAY_US - 1) / VJO_STATS_DAY_US));
}

int32_t vjo_stats_day_of(const VjoStats *s, uint64_t us) { return vjo_stats_day_in(us, s->tz_minutes); }

/* ---- games ---- */

/* The game of a uuid (len bytes, hash: record_line's FNV-1a of them), added when it is new (name: its records'
 * name, or a card's game); -1 when the table is full. A uuid longer than the field is told apart by its hash. */
static int game_index(VjoStats *s, const char *uuid, size_t len, uint32_t hash)
{
    size_t keep = len < VJO_STATS_UUID - 1 ? len : VJO_STATS_UUID - 1;
    if (s->open >= 0 && s->game[s->open].time.last_game == hash && !strncmp(s->game[s->open].uuid, uuid, keep) &&
        !s->game[s->open].uuid[keep])
        return s->open;
    for (uint32_t g = 0; g < s->games; g++)
        if (s->game[g].time.last_game == hash && !strncmp(s->game[g].uuid, uuid, keep) && !s->game[g].uuid[keep])
            return (int)g;
    if (s->games == VJO_STATS_GAMES)
        return -1;
    VjoStatsGame *g = &s->game[s->games];
    memset(g, 0, sizeof(*g));
    memcpy(g->uuid, uuid, keep);
    g->time.last_game = hash; /* (vjo_reading_time_add sets it to the same: each game's time sees its own records) */
    g->first_day = g->last_day = INT32_MIN;
    return (int)s->games++;
}

/* A name of at most VJO_STATS_NAME - 1 bytes, cut at a character's start. */
static void set_name(char *out, const char *name)
{
    size_t n = strlen(name);
    if (n > VJO_STATS_NAME - 1) {
        n = VJO_STATS_NAME - 1;
        while (n && ((unsigned char)name[n] & 0xC0) == 0x80)
            n--;
    }
    memcpy(out, name, n);
    out[n] = '\0';
}

/* ---- (game, day) rollups, sorted by day, then game ---- */

static int day_before(const VjoStatsDay *e, int32_t day, uint16_t game)
{
    return e->day < day || (e->day == day && e->game < game);
}

/* The rollup of game g's day, added when it is new and `add`; -1 when it is not there (or has no room). */
static int day_slot(VjoStats *s, int g, int32_t day, int add)
{
    uint32_t lo = 0, hi = s->days;
    if (day == INT32_MIN)
        return -1;
    /* (the newest day is at the end: most records are its, and a new one goes after every entry) */
    if (hi && day_before(&s->day[hi - 1], day, (uint16_t)g))
        lo = hi;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (day_before(&s->day[mid], day, (uint16_t)g))
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < s->days && s->day[lo].day == day && s->day[lo].game == g)
        return (int)lo;
    if (!add)
        return -1;
    if (s->days == VJO_STATS_DAYS) {
        /* full (decades of days): the oldest rollup goes, so that today and the days the tab shows still count; a day
         * older than every one kept is not added */
        if (!lo)
            return -1;
        memmove(&s->day[0], &s->day[1], (size_t)(lo - 1) * sizeof(s->day[0]));
        s->days--;
        s->days_full++;
        lo--;
        memset(&s->day[lo], 0, sizeof(s->day[0]));
        s->day[lo].day = day;
        s->day[lo].game = (uint16_t)g;
        s->days++;
        return (int)lo;
    }
    memmove(&s->day[lo + 1], &s->day[lo], (s->days - lo) * sizeof(s->day[0]));
    memset(&s->day[lo], 0, sizeof(s->day[0]));
    s->day[lo].day = day;
    s->day[lo].game = (uint16_t)g;
    s->days++;
    return (int)lo;
}

/* ---- the scan ---- */

/* The open line's interval ends without a time (another game's record, a damaged one): as the Shell's
 * vjo_reading_scope_add does for a game that is not its scope's. */
static void break_open(VjoStats *s)
{
    if (s->open >= 0) {
        s->game[s->open].time.last_us = 0;
        s->game[s->open].time.last_chars = 0;
    }
}

/* The record [p, end): counted, or damaged (skipped, and the interval broken, as in the Shell's count). 0 only for
 * the file's newest record when it is damaged (`newest`): it waits for the next count, which knows more. */
static int count_record(VjoStats *s, char *p, char *end, int newest)
{
    VjoHistoryRow row;
    if (vjo_history_row(p, end, 4, &row) < 0) {
        /* A file can end in a line break inside a record's text while the record is being appended (the writer
         * writes it a few hundred bytes at a time): that record is not damaged, only not whole yet. Once a record
         * follows it, it is what it is. */
        if (newest)
            return 0;
        /* the Shell's count: a damaged record breaks the interval (vjo_history_count_scope) */
        s->damaged++;
        if (s->open >= 0)
            s->game[s->open].time.last_us = 0;
        return 1;
    }
    /* the game's hash and the line's characters by their cells' lengths, as the Shell's count has them */
    uint32_t hash = vjo_history_game(row.uuid, row.len[0]);
    uint32_t chars = vjo_line_characters(row.line, row.len[3]);
    int g = game_index(s, row.uuid, row.len[0], hash);
    s->records++;
    if (g < 0) { /* more games than the table holds: their characters count in All games only */
        s->dropped++;
        s->other_chars += chars;
        break_open(s);
        s->open = -1;
        return 1;
    }
    if (s->open != g)
        break_open(s);
    VjoStatsGame *G = &s->game[g];
    uint64_t prev = G->time.last_us;
    uint32_t prev_chars = G->time.last_chars;
    uint32_t credit = vjo_reading_time_add(&G->time, row.time_us, hash, chars);
    if (credit) { /* the earlier line's time, on its own day */
        int k = day_slot(s, g, vjo_stats_day_of(s, prev), 0);
        if (k >= 0) {
            s->day[k].n.active_us += credit;
            s->day[k].n.timed_chars += prev_chars;
        }
    }
    G->chars += chars;
    G->lines++;
    if (!row.time_us) {
        s->untimed++;
    } else {
        int32_t d = vjo_stats_day_of(s, row.time_us);
        int k = day_slot(s, g, d, 1);
        if (k >= 0)
            s->day[k].n.chars += chars;
        else
            s->days_full++;
        if (G->first_day == INT32_MIN || d < G->first_day)
            G->first_day = d;
        if (G->last_day == INT32_MIN || d > G->last_day)
            G->last_day = d;
    }
    if (row.name[0])
        set_name(G->name, row.name);
    s->open = g;
    return 1;
}

uint64_t vjo_stats_scan(VjoStats *s, char *buf, size_t len, uint64_t base, int end)
{
    int whole = base == 0, waits = 0;
    size_t first = whole ? 0 : 1, start = len, i;
    uint64_t next;
    for (i = first; i <= len; i++) {
        if (i < len && (buf[i] != '"' || !vjo_history_record_start(buf, i, len, whole)))
            continue; /* (most bytes) */
        if (start < len && (i < len || end == VJO_STATS_SCAN_END))
            waits = !count_record(s, buf + start, buf + i, i == len);
        if (i < len)
            start = i;
    }
    /* A '"' as the window's last byte, at a line's start, may start a record: whether it does is its next byte's,
     * which the window does not hold. The next window starts there, never after it (a window of the file's end that
     * ends in a line break has no such byte). */
    size_t keep = len > first && buf[len - 1] == '"' && (len - 1 ? buf[len - 2] == '\n' : whole) ? len - 1 : len;
    if (waits)
        next = base + start; /* the file's newest record, damaged as it is now: the next count reads it again */
    else if (start == len || end == VJO_STATS_SCAN_END)
        next = base + keep; /* every record counted (or none starts here) */
    else if (start > first || end == VJO_STATS_SCAN_CUT)
        next = base + start; /* the newest waits: its end is not read yet */
    else { /* a record longer than the window: not one the writer makes; skipped as damaged, as the Shell does */
        s->damaged++;
        if (s->open >= 0)
            s->game[s->open].time.last_us = 0;
        next = base + keep;
    }
    s->covered = next;
    return next;
}

/* ---- cards ---- */

int vjo_stats_card_id(const char *hex, uint8_t id[16])
{
    for (int i = 0; i < 32; i++) {
        char c = hex[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0)
            return -1;
        if (i & 1)
            id[i / 2] = (uint8_t)(id[i / 2] | v);
        else
            id[i / 2] = (uint8_t)(v << 4);
    }
    return hex[32] ? -1 : 0;
}

static int card_cmp(const VjoStatsCard *a, const VjoStatsCard *b) { return memcmp(a->id, b->id, 16); }

uint32_t vjo_stats_cards_sort(VjoStatsCard *c, uint32_t n)
{
    uint32_t kept = 0;
    /* a heap sort: no allocation, no recursion, n log n at any n */
    for (uint32_t start = n / 2; start-- > 0;) {
        for (uint32_t root = start, child; (child = 2 * root + 1) < n; root = child) {
            if (child + 1 < n && card_cmp(&c[child], &c[child + 1]) < 0)
                child++;
            if (card_cmp(&c[root], &c[child]) >= 0)
                break;
            VjoStatsCard t = c[root];
            c[root] = c[child];
            c[child] = t;
        }
    }
    for (uint32_t end = n; end-- > 1;) {
        VjoStatsCard t = c[0];
        c[0] = c[end];
        c[end] = t;
        for (uint32_t root = 0, child; (child = 2 * root + 1) < end; root = child) {
            if (child + 1 < end && card_cmp(&c[child], &c[child + 1]) < 0)
                child++;
            if (card_cmp(&c[root], &c[child]) >= 0)
                break;
            t = c[root];
            c[root] = c[child];
            c[child] = t;
        }
    }
    for (uint32_t k = 0; k < n; k++)
        if (!kept || card_cmp(&c[kept - 1], &c[k]))
            c[kept++] = c[k];
    return kept;
}

int vjo_stats_card_find(const VjoStats *s, const uint8_t id[16])
{
    uint32_t lo = 0, hi = s->cards;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int c = memcmp(s->card[mid].id, id, 16);
        if (!c)
            return (int)mid;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return -1;
}

uint16_t vjo_stats_card_game(VjoStats *s, const char *platform, const char *title_id)
{
    char uuid[2 * VJO_STATS_UUID];
    size_t p = strlen(platform), t = strlen(title_id);
    int g;
    if (!p || !t || p + 1 + t >= sizeof(uuid))
        return VJO_STATS_NO_GAME;
    memcpy(uuid, platform, p);
    uuid[p] = '-';
    memcpy(uuid + p + 1, title_id, t + 1);
    g = game_index(s, uuid, p + 1 + t, vjo_history_game(uuid, p + 1 + t));
    return g < 0 ? VJO_STATS_NO_GAME : (uint16_t)g;
}

/* ---- what the tab shows ---- */

static void add_count(VjoStatsCount *to, const VjoStatsCount *n)
{
    to->chars += n->chars;
    to->active_us += n->active_us;
    to->timed_chars += n->timed_chars;
}

/* The first rollup of `day` or a later one. */
static uint32_t first_of_day(const VjoStats *s, int32_t day)
{
    uint32_t lo = 0, hi = s->days;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (s->day[mid].day < day)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

void vjo_stats_days_range(const VjoStats *s, int32_t first, int n, VjoStatsCount *out)
{
    memset(out, 0, sizeof(*out) * (size_t)(n > 0 ? n : 0));
    for (uint32_t k = first_of_day(s, first); k < s->days && s->day[k].day < (int64_t)first + n; k++)
        add_count(&out[s->day[k].day - first], &s->day[k].n);
}

VjoStatsCount vjo_stats_game_day(const VjoStats *s, int g, int32_t day)
{
    VjoStatsCount n = {0, 0, 0};
    for (uint32_t k = first_of_day(s, day); k < s->days && s->day[k].day == day; k++)
        if (s->day[k].game == g)
            n = s->day[k].n;
    return n;
}

VjoStatsCount vjo_stats_game_total(const VjoStats *s, int g)
{
    VjoStatsCount n = {s->game[g].chars, s->game[g].time.active_us, s->game[g].time.timed_chars};
    return n;
}

VjoStatsCount vjo_stats_all(const VjoStats *s, uint64_t *lines)
{
    VjoStatsCount n = {s->other_chars, 0, 0};
    uint64_t l = s->dropped;
    for (uint32_t g = 0; g < s->games; g++) {
        VjoStatsCount t = vjo_stats_game_total(s, (int)g);
        add_count(&n, &t);
        l += s->game[g].lines;
    }
    if (lines)
        *lines = l;
    return n;
}

uint32_t vjo_stats_cards_per_game(const VjoStats *s, uint32_t *cards)
{
    memset(cards, 0, sizeof(*cards) * VJO_STATS_GAMES);
    for (uint32_t k = 0; k < s->cards; k++)
        if (s->card[k].game < VJO_STATS_GAMES)
            cards[s->card[k].game]++;
    return s->cards + s->cards_full;
}

int vjo_stats_game_order(const VjoStats *s, uint16_t *order)
{
    int n = 0;
    for (uint32_t g = 0; g < s->games; g++) {
        const VjoStatsGame *G = &s->game[g];
        int at = n;
        /* insertion: newest day first, then the most characters, then the name */
        while (at > 0) {
            const VjoStatsGame *o = &s->game[order[at - 1]];
            int later = G->last_day != o->last_day ? G->last_day > o->last_day
                      : G->chars != o->chars ? G->chars > o->chars : strcmp(G->name, o->name) < 0;
            if (!later)
                break;
            order[at] = order[at - 1];
            at--;
        }
        order[at] = (uint16_t)g;
        n++;
    }
    return n;
}

/* ---- the goal and the streak ---- */

static const uint32_t goal_chars[VJO_GOALS] = {0, 2000, 5000, 10000, 20000, 50000, 0, 0, 0, 0};
static const uint32_t goal_minutes[VJO_GOALS] = {0, 0, 0, 0, 0, 0, 15, 30, 60, 120};

uint32_t vjo_stats_goal_chars(int goal) { return goal > 0 && goal < VJO_GOALS ? goal_chars[goal] : 0; }
uint32_t vjo_stats_goal_minutes(int goal) { return goal > 0 && goal < VJO_GOALS ? goal_minutes[goal] : 0; }

int vjo_stats_goal_met(const VjoStatsCount *day, int goal)
{
    uint32_t chars = vjo_stats_goal_chars(goal), minutes = vjo_stats_goal_minutes(goal);
    if (chars)
        return day->chars >= chars;
    if (minutes)
        return day->active_us >= (uint64_t)minutes * 60000000ull;
    return day->chars > 0;
}

uint32_t vjo_stats_goal_progress(const VjoStatsCount *day, int goal)
{
    uint32_t chars = vjo_stats_goal_chars(goal), minutes = vjo_stats_goal_minutes(goal);
    uint64_t p = chars ? day->chars * 1000ull / chars
               : minutes ? day->active_us / 60000ull / minutes : 0; /* (us / 60,000 x 1,000 / (minutes x 1,000)) */
    return p > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)p;
}

void vjo_stats_streak(const VjoStats *s, int32_t today, int goal, int *current, int *best)
{
    int run = 0, longest = 0, today_met = 0, yesterday_run = 0;
    int64_t last = INT64_MIN; /* the last day that counted */
    uint32_t k = 0;
    while (k < s->days) {
        int32_t day = s->day[k].day;
        VjoStatsCount n = {0, 0, 0};
        for (; k < s->days && s->day[k].day == day; k++)
            add_count(&n, &s->day[k].n);
        if (day > today)
            continue; /* a day after today (the clock set back): not part of a streak to today */
        if (!vjo_stats_goal_met(&n, goal))
            continue;
        run = (int64_t)day == last + 1 ? run + 1 : 1;
        last = day;
        if (run > longest)
            longest = run;
        if (day == today)
            today_met = run;
        else if (day == today - 1)
            yesterday_run = run;
    }
    if (current)
        *current = today_met ? today_met : yesterday_run;
    if (best)
        *best = longest;
}

/* ---- calendar: Howard Hinnant's days_from_civil / civil_from_days ---- */

int32_t vjo_stats_days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (int32_t)(era * 146097 + (int)doe - 719468);
}

void vjo_stats_civil(int32_t z0, int *year, int *month, int *mday)
{
    int64_t z = (int64_t)z0 + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    unsigned mp = (5u * doy + 2u) / 153u;
    unsigned d = doy - (153u * mp + 2u) / 5u + 1u;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    *year = (int)(y + (m <= 2));
    *month = (int)m;
    *mday = (int)d;
}

int vjo_stats_weekday(int32_t day)
{
    /* 1970-01-01 was a Thursday (4) */
    int w = (int)(((int64_t)day + 4) % 7);
    return w < 0 ? w + 7 : w;
}

/* ---- the cache ---- */

/* stats_cache.bin: this header, then the games, the (game, day) rollups and the cards as VjoStats holds them
 * (the Vita's layout: the cache is the app's own, never copied elsewhere); every field and table is checked as it
 * is read. Version 1. */
#define CACHE_MAGIC "RubiStat"
#define CACHE_VERSION 1u
typedef struct {
    char magic[8];
    uint32_t version, size;           /* this layout's version; the file's bytes */
    uint32_t check;                   /* FNV-1a of every byte after this field */
    uint32_t sizes;                   /* the tables' entry sizes, packed: a layout another build made is refused */
    int32_t tz_minutes;
    uint32_t rule;
    uint64_t covered, history_bytes;
    uint32_t tail_len, tail_check, head_len, head_check;
    int32_t open;
    uint32_t games, days, cards;
    uint32_t cards_unknown, cards_full;
    uint64_t records, damaged, untimed, dropped, days_full, other_chars;
} CacheHeader;
#define CACHE_SIZES ((uint32_t)(sizeof(VjoStatsGame) << 16 | sizeof(VjoStatsDay) << 8 | sizeof(VjoStatsCard)))
_Static_assert(sizeof(VjoStatsGame) < 65536 && sizeof(VjoStatsDay) < 256 && sizeof(VjoStatsCard) < 256,
               "the cache's packed entry sizes");
_Static_assert(sizeof(VjoReadingTime) == 32, "VjoReadingTime as the Shell's cache keeps it");

size_t vjo_stats_cache_size(const VjoStats *s)
{
    return sizeof(CacheHeader) + s->games * sizeof(VjoStatsGame) + s->days * sizeof(VjoStatsDay) +
           s->cards * sizeof(VjoStatsCard);
}

size_t vjo_stats_cache_write(const VjoStats *s, unsigned char *out, size_t cap)
{
    size_t n = vjo_stats_cache_size(s), at = sizeof(CacheHeader);
    CacheHeader h;
    if (cap < n || n > 0xFFFFFFFFu)
        return 0;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, CACHE_MAGIC, 8);
    h.version = CACHE_VERSION;
    h.size = (uint32_t)n;
    h.sizes = CACHE_SIZES;
    h.tz_minutes = s->tz_minutes;
    h.rule = s->rule;
    h.covered = s->covered;
    h.history_bytes = s->history_bytes;
    h.tail_len = s->tail_len;
    h.tail_check = s->tail_check;
    h.head_len = s->head_len;
    h.head_check = s->head_check;
    h.open = s->open;
    h.games = s->games;
    h.days = s->days;
    h.cards = s->cards;
    h.cards_unknown = s->cards_unknown;
    h.cards_full = s->cards_full;
    h.records = s->records;
    h.damaged = s->damaged;
    h.untimed = s->untimed;
    h.dropped = s->dropped;
    h.days_full = s->days_full;
    h.other_chars = s->other_chars;
    memcpy(out + at, s->game, s->games * sizeof(VjoStatsGame));
    at += s->games * sizeof(VjoStatsGame);
    memcpy(out + at, s->day, s->days * sizeof(VjoStatsDay));
    at += s->days * sizeof(VjoStatsDay);
    memcpy(out + at, s->card, s->cards * sizeof(VjoStatsCard));
    memcpy(out, &h, sizeof(h));
    h.check = vjo_stats_check(out + offsetof(CacheHeader, sizes), n - offsetof(CacheHeader, sizes));
    memcpy(out + offsetof(CacheHeader, check), &h.check, sizeof(h.check));
    return n;
}

int vjo_stats_cache_read(VjoStats *s, const unsigned char *in, size_t len, int32_t tz_minutes, uint32_t rule)
{
    CacheHeader h;
    int why = VJO_STATS_CACHE_DAMAGED;
    vjo_stats_reset(s, tz_minutes, rule);
    if (!in || len < sizeof(h))
        return why;
    memcpy(&h, in, sizeof(h));
    if (memcmp(h.magic, CACHE_MAGIC, 8))
        return why;
    if (h.version != CACHE_VERSION || h.sizes != CACHE_SIZES)
        return VJO_STATS_CACHE_VERSION;
    if (h.size != len || h.check != vjo_stats_check(in + offsetof(CacheHeader, sizes), len - offsetof(CacheHeader, sizes)))
        return why;
    if (h.games > VJO_STATS_GAMES || h.days > VJO_STATS_DAYS || h.cards > VJO_STATS_CARDS ||
        len != sizeof(h) + h.games * sizeof(VjoStatsGame) + h.days * sizeof(VjoStatsDay) + h.cards * sizeof(VjoStatsCard) ||
        h.open < -1 || h.open >= (int32_t)h.games || h.tail_len > 4096 || h.head_len > 4096 || h.tail_len > h.covered ||
        h.head_len > h.covered ||
        h.covered > h.history_bytes)
        return why;
    if (h.rule != rule)
        return VJO_STATS_CACHE_RULE;
    if (h.tz_minutes != tz_minutes)
        return VJO_STATS_CACHE_ZONE;
    s->covered = h.covered;
    s->history_bytes = h.history_bytes;
    s->tail_len = h.tail_len;
    s->tail_check = h.tail_check;
    s->head_len = h.head_len;
    s->head_check = h.head_check;
    s->open = h.open;
    s->games = h.games;
    s->days = h.days;
    s->cards = h.cards;
    s->cards_unknown = h.cards_unknown;
    s->cards_full = h.cards_full;
    s->records = h.records;
    s->damaged = h.damaged;
    s->untimed = h.untimed;
    s->dropped = h.dropped;
    s->days_full = h.days_full;
    s->other_chars = h.other_chars;
    size_t at = sizeof(h);
    memcpy(s->game, in + at, h.games * sizeof(VjoStatsGame));
    at += h.games * sizeof(VjoStatsGame);
    memcpy(s->day, in + at, h.days * sizeof(VjoStatsDay));
    at += h.days * sizeof(VjoStatsDay);
    memcpy(s->card, in + at, h.cards * sizeof(VjoStatsCard));
    /* the tables as the scan keeps them: names and uuids terminated, rollups in order and of known games */
    for (uint32_t g = 0; g < s->games; g++)
        if (memchr(s->game[g].uuid, 0, VJO_STATS_UUID) == NULL || memchr(s->game[g].name, 0, VJO_STATS_NAME) == NULL)
            goto damaged;
    for (uint32_t k = 0; k < s->days; k++)
        if (s->day[k].game >= s->games || (k && !day_before(&s->day[k - 1], s->day[k].day, s->day[k].game)))
            goto damaged;
    for (uint32_t k = 0; k < s->cards; k++)
        if ((s->card[k].game < VJO_STATS_UNREAD && s->card[k].game >= s->games) ||
            (k && memcmp(s->card[k - 1].id, s->card[k].id, 16) >= 0))
            goto damaged;
    return VJO_STATS_CACHE_OK;
damaged:
    vjo_stats_reset(s, tz_minutes, rule);
    return why;
}
