/* The Rubi app's Stats tab (#312): reading statistics from history.csv (docs/ocr-history.md), per game and
 * per local calendar day, counted with the Triangle view's own reading-time rule (core/history_lines.c
 * vjo_reading_time_add: config.ini reading_time, docs/reading-time.md), so a game's numbers here are the
 * Triangle view's for that game: today in this game, and this game's total.
 *
 * App only, pure C: no I/O, no allocation, no clock (the page, settings/stats_page.c, reads the files and the
 * clock and passes the bytes and the time zone in). One VjoStats holds everything the cache
 * (ux0:data/VitaJPOverlay/stats_cache.bin) keeps: an append to history.csv is counted from where the last
 * count stopped, and a count that was stopped goes on from where it was.
 *
 * How a record counts (as the Shell's count, vjo_history_count_scope with each game as its scope):
 * - its line's characters (vjo_line_characters) go to its game and to its game's day (the local day of its
 *   time; a record with an invalid time counts for the game's total only);
 * - the reading time credited to a line (when the next record of its game comes) goes to the line's own day:
 *   a line read across midnight is the day's it began on, as tools/reading_stats.py has it (the Triangle
 *   view's "today" never sees such a line: today has no line from yesterday);
 * - another game's record, a damaged record or an invalid time break the line's interval, as in the Shell.
 * The pace each game's lines are capped by is its own (VjoReadingTime.pace), as the Shell keeps it. */
#ifndef VJO_STATS_H
#define VJO_STATS_H

#include <stddef.h>
#include <stdint.h>

#include "history_lines.h"

#define VJO_STATS_GAMES 512    /* games (history.csv uuids, and the games of cards) */
#define VJO_STATS_DAYS 16384   /* (game, day) rollups: 44 years of one game a day */
#define VJO_STATS_CARDS 16384  /* cards whose game is known (the card cache) */
#define VJO_STATS_NAME 128     /* a game's name, as history.csv's name column (VjoHistoryRecord.name) */
#define VJO_STATS_UUID 24      /* "vita-PCSG00415", as VjoReadingScope.uuid */
#define VJO_STATS_NO_GAME 0xFFFFu /* a card whose record names no game (versions 1-2 have none) */
#define VJO_STATS_UNREAD 0xFFFEu  /* a card whose record could not be read: read again at the next count */
#define VJO_STATS_DAY_US 86400000000ll

typedef struct {
    uint64_t chars;       /* characters of the records */
    uint64_t active_us;   /* reading time credited to the lines */
    uint64_t timed_chars; /* characters of the lines that were credited a time (characters/hour's numerator) */
} VjoStatsCount;

typedef struct {
    int32_t day;   /* local calendar days since 1970-01-01 */
    uint16_t game; /* index in VjoStats.game */
    uint16_t pad;
    VjoStatsCount n;
} VjoStatsDay;

typedef struct {
    char uuid[VJO_STATS_UUID];   /* history.csv's first column: platform and title ID */
    char name[VJO_STATS_NAME];   /* the newest record's name */
    uint64_t chars;              /* every record's, also those with an invalid time */
    uint64_t lines;              /* records */
    VjoReadingTime time;         /* its reading time (active_us, timed_chars), the open line and the pace */
    int32_t first_day, last_day; /* the days of its first and newest records with a valid time (INT32_MIN: none) */
} VjoStatsGame;

typedef struct {
    uint8_t id[16]; /* the card's ID (core/anki_queue.h: 32 hex digits) */
    uint16_t game;  /* its game's index, VJO_STATS_NO_GAME (its record has none) or VJO_STATS_UNREAD */
    uint16_t pad;
} VjoStatsCard;

typedef struct {
    /* what the counts are of */
    uint64_t covered;    /* the bytes of history.csv counted: the next record to count starts here */
    uint64_t history_bytes; /* history.csv's size when it was counted */
    int32_t tz_minutes;  /* the local time's offset from UTC the days were counted with */
    uint32_t rule;       /* vjo_reading_rule the times were counted with */
    uint32_t tail_len, tail_check; /* FNV-1a of history.csv's last tail_len (<= 4,096) bytes before `covered` */
    uint32_t head_len, head_check; /* and of its first head_len (<= 4,096) bytes: the header and the first records */
    int32_t open;        /* the game whose newest line is open (its next record not counted yet), or -1 */
    uint32_t games, days, cards;
    uint64_t records, damaged, untimed; /* records counted; damaged ones skipped; with an invalid time */
    uint64_t dropped;    /* records of games past VJO_STATS_GAMES (their characters are in `other`) */
    uint64_t days_full;  /* rollups the full day table dropped (VJO_STATS_DAYS: the oldest go), and records older */
    uint64_t other_chars; /* those records' characters */
    uint32_t cards_unknown, cards_full; /* cards with no game; past VJO_STATS_CARDS */
    VjoStatsGame game[VJO_STATS_GAMES];
    VjoStatsDay day[VJO_STATS_DAYS]; /* sorted by day, then game */
    VjoStatsCard card[VJO_STATS_CARDS]; /* sorted by id */
} VjoStats;

/* Empty, for a count from history.csv's first byte with this time zone and rule. */
void vjo_stats_reset(VjoStats *s, int32_t tz_minutes, uint32_t rule);

/* Counts the records that start in the window buf[0, len) of history.csv (buf is changed: cells are unescaped
 * in place). `base` is the file offset of buf[0]: the window is read with the byte before it (buf[0], which says
 * whether its first byte starts a record) unless base is 0, the file's first byte. A record is counted once its
 * end is known: the next record's start, or, with VJO_STATS_SCAN_END, the window's end. The newest record of a
 * window is otherwise left for the next window (VJO_STATS_SCAN_MORE: the file goes on), or for the next count
 * (VJO_STATS_SCAN_CUT: the window ends at the file's end, which ends in no line break: a record being appended, or
 * one a power loss cut). With VJO_STATS_SCAN_END the file's newest record, when it is damaged, waits for the next
 * count too: an append in progress can end in a line break inside the record's text. A record longer than a whole
 * window is skipped as damaged, as the Shell's count skips one longer than its own. Returns the file offset the
 * next window starts at (s->covered then). */
enum { VJO_STATS_SCAN_MORE, VJO_STATS_SCAN_END, VJO_STATS_SCAN_CUT };
uint64_t vjo_stats_scan(VjoStats *s, char *buf, size_t len, uint64_t base, int end);

/* ---- cards (the Anki queue's records, anki_queue/ and anki_queue/exported/) ---- */

/* A card's ID from its 32 lowercase hex digits (a queue file's name): 0, or -1. */
int vjo_stats_card_id(const char *hex, uint8_t id[16]);
/* Sorts n cards by ID and drops repeats (a card in the queue and the archive both): the cards left. */
uint32_t vjo_stats_cards_sort(VjoStatsCard *c, uint32_t n);
/* The entry of card `id` in s->card, or -1. */
int vjo_stats_card_find(const VjoStats *s, const uint8_t id[16]);
/* The game of a card's platform and title ID (history.csv's uuid "vita-PCSG00415" is "vita" and "PCSG00415"), added
 * when it has no record yet; VJO_STATS_NO_GAME when either is empty or the table is full. */
uint16_t vjo_stats_card_game(VjoStats *s, const char *platform, const char *title_id);

/* The local day of a Unix time (microseconds), in the counts' time zone; INT32_MIN for an invalid time (0). */
int32_t vjo_stats_day_of(const VjoStats *s, uint64_t us);
/* The same in a time zone tz_minutes east of UTC. */
int32_t vjo_stats_day_in(uint64_t us, int32_t tz_minutes);
/* A FNV-1a 32 of n bytes, as the checks of the cache use. */
uint32_t vjo_stats_check(const void *p, size_t n);

/* ---- what the tab shows ---- */

/* All games' counts of `n` consecutive days from `first` into out[0, n). */
void vjo_stats_days_range(const VjoStats *s, int32_t first, int n, VjoStatsCount *out);
/* Game g's counts of one day. */
VjoStatsCount vjo_stats_game_day(const VjoStats *s, int g, int32_t day);
/* Every game's total (also the characters of games past the table), and its records. */
VjoStatsCount vjo_stats_all(const VjoStats *s, uint64_t *lines);
/* Game g's total. */
VjoStatsCount vjo_stats_game_total(const VjoStats *s, int g);
/* The cards of each game into cards[VJO_STATS_GAMES]; returns all cards (also those with no game). */
uint32_t vjo_stats_cards_per_game(const VjoStats *s, uint32_t *cards);
/* The games with any record or card, newest day first (then the most characters, then the name), into order;
 * returns how many. */
int vjo_stats_game_order(const VjoStats *s, uint16_t *order);

/* The daily goal (app.ini stats_daily_goal): off, characters or minutes. */
enum {
    VJO_GOAL_OFF,
    VJO_GOAL_2K, VJO_GOAL_5K, VJO_GOAL_10K, VJO_GOAL_20K, VJO_GOAL_50K,
    VJO_GOAL_15MIN, VJO_GOAL_30MIN, VJO_GOAL_60MIN, VJO_GOAL_120MIN,
    VJO_GOALS
};
/* The goal's characters (0: not a characters goal) and minutes (0: not a minutes goal). */
uint32_t vjo_stats_goal_chars(int goal);
uint32_t vjo_stats_goal_minutes(int goal);
/* Whether a day counts for the streak: it met the goal, or with no goal it had any reading (a character). */
int vjo_stats_goal_met(const VjoStatsCount *day, int goal);
/* The goal's progress in a day, in thousandths (1000: met; more past it; 0 with no goal). */
uint32_t vjo_stats_goal_progress(const VjoStatsCount *day, int goal);
/* The streak: *current, the days in a row that count, ending today, or ending yesterday while today does not
 * count yet (GameSentenceMiner's calculate_streak); *best, the most days in a row over the whole history. */
void vjo_stats_streak(const VjoStats *s, int32_t today, int goal, int *current, int *best);

/* ---- calendar (proleptic Gregorian, days since 1970-01-01) ---- */
void vjo_stats_civil(int32_t day, int *year, int *month, int *mday);
int32_t vjo_stats_days_from_civil(int year, int month, int mday);
int vjo_stats_weekday(int32_t day); /* 0 Sunday .. 6 Saturday */

/* ---- the cache (stats_cache.bin) ---- */

/* Its size for s. */
size_t vjo_stats_cache_size(const VjoStats *s);
/* s as the cache's bytes into out (cap >= vjo_stats_cache_size): the bytes, or 0. */
size_t vjo_stats_cache_write(const VjoStats *s, unsigned char *out, size_t cap);
enum { VJO_STATS_CACHE_OK, VJO_STATS_CACHE_DAMAGED = -1, VJO_STATS_CACHE_VERSION = -2,
       VJO_STATS_CACHE_RULE = -3, VJO_STATS_CACHE_ZONE = -4 };
/* The cache's bytes into s, when they are whole, this version's and counted with this time zone and rule:
 * VJO_STATS_CACHE_OK; else why not, with s reset for a count from the start. */
int vjo_stats_cache_read(VjoStats *s, const unsigned char *in, size_t len, int32_t tz_minutes, uint32_t rule);

#endif
