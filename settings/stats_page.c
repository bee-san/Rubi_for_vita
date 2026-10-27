#include "stats_page.h"

#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>
#include <stdio.h>
#include <string.h>
#include <vita2d.h>

#include "anki_page.h" /* vjo_log */
#include "app_prefs.h"
#include "preferences.h"
#include "../core/anki_queue.h"
#include "../core/history_read.h"
#include "../core/json.h"
#include "../core/stats.h"
#include "../core/stick_scroll.h"
/* As in main.c: no vita2d_fini in the system application. */
#pragma GCC poison vita2d_fini

#define MISSING ((int)0x80010002u)
#define RTC_UNIX_EPOCH 62135596800000000ull /* SceRtcTick's microseconds from 0001-01-01 to 1970-01-01 */
#define WORKER_STACK (64 * 1024)
#define WINDOW (64 * 1024)          /* history.csv is read 64 KiB (and the byte before) at a time */
#define WORK_MEM (1024 * 1024)      /* the cache's bytes, a window, or the card list and one card's record */
/* The cache's checks of history.csv: its first bytes (the header, which every Rubi file shares, and the first
 * records, whose IDs hold their times) and the last bytes counted. */
#define CHECK_HEAD 1024u
#define CHECK_TAIL 256u
_Static_assert(sizeof(VjoStats) - offsetof(VjoStats, game) + 256 <= WORK_MEM, "the largest cache fits the work memory");
_Static_assert(WINDOW + 1 <= WORK_MEM, "a window fits the work memory");
_Static_assert(VJO_STATS_CARDS * sizeof(VjoStatsCard) + VJO_QUEUE_TEXT_MAX + 1 + 256 * 1024 <= WORK_MEM,
               "the card list, a record and its JSON tokens fit the work memory");

/* The page's rows, top to bottom: Today, Daily goal, Last 14 days, This year, All games, then each game. They are
 * more than the room between the tabs and the status line: the column scrolls a row at a time, the selected row
 * always whole, as the OCR page's cards do. */
enum { ROW_TODAY, ROW_GOAL, ROW_DAYS, ROW_YEAR, ROW_ALL, ROW_GAMES };
#define VIEW_Y 112
#define VIEW_END 466
#define ROW_GAP 4
#define FOOTER_Y 470
#define FOOTER_H 30
#define SCROLL_X 942
#define SCROLL_W 6
#define TODAY_H 104
#define GOAL_H 68
#define DAYS_H 150
#define YEAR_H 172
#define GAME_H 92
/* the 14 days' bars */
#define BARS_X 42
#define BAR_PITCH 62
#define BAR_W 46
#define BARS_DY 46 /* the chart's top, from the row's */
#define BARS_H 70
/* the year's heatmap: weeks from Sunday, 53 columns of 7 days */
#define HEAT_X 72
#define HEAT_DY 42
#define HEAT_CELL 12
#define HEAT_PITCH 14
/* the goal's badge */
#define GOAL_X 500
#define GOAL_W 400

/* ---------------- the count (its thread) ---------------- */

static VjoStats stats;                               /* the worker's while it runs, the page's after */
static unsigned char work[WORK_MEM] __attribute__((aligned(16)));
static SceUID lock = -1, worker = -1;
static int running, finished, cancelled, count_rule;
static char progress[160], result[240];
static int result_error;
static char note[160]; /* the page's own line (a save, a start that failed): the main thread's */
static int note_error;

static void set_progress(const char *msg)
{
    sceKernelLockMutex(lock, 1, NULL);
    snprintf(progress, sizeof(progress), "%s", msg);
    sceKernelUnlockMutex(lock, 1);
}

static int stopped(void)
{
    int c;
    sceKernelLockMutex(lock, 1, NULL);
    c = cancelled;
    sceKernelUnlockMutex(lock, 1);
    return c;
}

/* The local time's offset from UTC now, in minutes (the Vita's time zone and daylight saving setting), and the
 * time now (Unix microseconds). */
static int32_t local_offset(uint64_t *now_us)
{
    SceRtcTick utc, local;
    int64_t d;
    *now_us = 0;
    if (sceRtcGetCurrentTick(&utc) < 0 || utc.tick < RTC_UNIX_EPOCH)
        return 0;
    *now_us = utc.tick - RTC_UNIX_EPOCH;
    if (sceRtcConvertUtcToLocalTime(&utc, &local) < 0)
        return 0;
    d = (int64_t)(local.tick - utc.tick);
    return (int32_t)((d >= 0 ? d + 30000000 : d - 30000000) / 60000000);
}

/* n bytes at off of fd into buf (short reads again): 0, or the SDK's error (-1: the file ended first). */
static int read_at(SceUID fd, void *buf, size_t n, uint64_t off)
{
    size_t got = 0;
    while (got < n) {
        int r = sceIoPread(fd, (char *)buf + got, n - got, (SceOff)(off + got));
        if (r <= 0)
            return r < 0 ? r : -1;
        got += (size_t)r;
    }
    return 0;
}

/* stats_cache.bin into stats, or stats reset: why not (VJO_STATS_CACHE_*), 1 when there is none. */
static int load_cache(int32_t tz, uint32_t rule)
{
    SceUID fd = sceIoOpen(VJO_STATS_CACHE_PATH, SCE_O_RDONLY, 0);
    SceOff size;
    int rc = VJO_STATS_CACHE_DAMAGED;
    if (fd < 0) {
        vjo_stats_reset(&stats, tz, rule);
        return fd == MISSING ? 1 : VJO_STATS_CACHE_DAMAGED;
    }
    size = sceIoLseek(fd, 0, SCE_SEEK_END);
    if (size > 0 && size <= (SceOff)sizeof(work) && !read_at(fd, work, (size_t)size, 0))
        rc = vjo_stats_cache_read(&stats, work, (size_t)size, tz, rule);
    else
        vjo_stats_reset(&stats, tz, rule);
    sceIoClose(fd);
    return rc;
}

/* The cache's checks of history.csv (fd, size bytes): the bytes it counted are still the file's first, and where it
 * stopped is its end or a record's start (as the Shell checks its own count: shell/history.c vjo_history_total_step). */
static int same_history(SceUID fd, uint64_t size)
{
    char at[3];
    if (!stats.covered)
        return 1;
    if (size < stats.covered || stats.history_bytes < stats.covered)
        return 0; /* a shorter file than the one counted: another one */
    /* (with a single byte after it, whether a record starts there is not known yet: the scan waits for the next) */
    if (size - stats.covered >= 2 && (read_at(fd, at, 3, stats.covered - 1) || !vjo_history_record_start(at, 1, 3, 0)))
        return 0;
    if (read_at(fd, work, stats.head_len, 0) || vjo_stats_check(work, stats.head_len) != stats.head_check)
        return 0;
    if (read_at(fd, work, stats.tail_len, stats.covered - stats.tail_len) ||
        vjo_stats_check(work, stats.tail_len) != stats.tail_check)
        return 0;
    return 1;
}

/* After a count: the checks of the bytes it counted, for the next one. 0, or -1 when they could not be read: then the
 * counts are shown but not cached (the next count starts from the cache kept on the card). */
static int set_checks(SceUID fd, uint64_t size)
{
    uint32_t head = stats.covered < CHECK_HEAD ? (uint32_t)stats.covered : CHECK_HEAD;
    uint32_t tail = stats.covered < CHECK_TAIL ? (uint32_t)stats.covered : CHECK_TAIL;
    if (read_at(fd, work, head, 0))
        return -1;
    stats.head_check = vjo_stats_check(work, head);
    if (read_at(fd, work, tail, stats.covered - tail))
        return -1;
    stats.tail_check = vjo_stats_check(work, tail);
    stats.head_len = head;
    stats.tail_len = tail;
    stats.history_bytes = size;
    return 0;
}

/* history.csv counted from where the cache stopped: its bytes, or -1 (the error in *err). *cacheable: the counts can be
 * cached (their checks were read). */
static int64_t count_history(uint64_t *from, int *err, int *records_before, int *cacheable)
{
    SceUID fd = sceIoOpen(VJO_STATS_HISTORY_PATH, SCE_O_RDONLY, 0);
    SceOff end;
    uint64_t size, pos;
    char msg[96];
    *err = 0;
    *cacheable = 1;
    *records_before = (int)stats.records;
    if (fd < 0) {
        if (fd != MISSING) {
            *err = fd;
            return -1;
        }
        /* no history.csv (Read text: Only when I press L + R keeps none) or none any more: nothing counted from it;
         * the cards' games stay, unless some were the old file's */
        if (stats.covered)
            vjo_stats_reset(&stats, stats.tz_minutes, stats.rule);
        *from = 0;
        return 0;
    }
    end = sceIoLseek(fd, 0, SCE_SEEK_END);
    if (end < 0) {
        *err = (int)end;
        sceIoClose(fd);
        return -1;
    }
    size = (uint64_t)end;
    if (!same_history(fd, size)) {
        vjo_log("stats: history.csv is not the one counted (%llu B, the cache's %llu of %llu): counting it again",
                (unsigned long long)size, (unsigned long long)stats.covered, (unsigned long long)stats.history_bytes);
        vjo_stats_reset(&stats, stats.tz_minutes, stats.rule); /* (and the cards: their games are read again) */
    }
    *from = pos = stats.covered;
    *records_before = (int)stats.records;
    while (pos < size && !stopped()) {
        uint64_t base = pos ? pos - 1 : 0, to = size - pos > WINDOW ? pos + WINDOW : size;
        int how = VJO_STATS_SCAN_MORE, rc = read_at(fd, work, (size_t)(to - base), base);
        if (rc) {
            *err = rc;
            break;
        }
        if (to == size)
            how = work[to - base - 1] == '\n' ? VJO_STATS_SCAN_END : VJO_STATS_SCAN_CUT;
        uint64_t next = vjo_stats_scan(&stats, (char *)work, (size_t)(to - base), base, how);
        if (next <= pos)
            break; /* (the newest record waits for its end) */
        pos = next;
        snprintf(msg, sizeof(msg), "Counting history.csv: %llu of %llu KB…", (unsigned long long)(pos / 1024),
                 (unsigned long long)(size / 1024));
        set_progress(msg);
    }
    *cacheable = set_checks(fd, size) == 0;
    sceIoClose(fd);
    return *err ? -1 : (int64_t)size;
}

/* The card files of one queue directory (anki_queue/ or its exported/) into list. */
static void list_cards(const char *dir, int archive, VjoStatsCard *list, uint32_t *n, uint32_t *full)
{
    SceUID d = sceIoDopen(dir);
    SceIoDirent ent;
    if (d < 0)
        return;
    for (;;) {
        memset(&ent, 0, sizeof(ent));
        if (sceIoDread(d, &ent) <= 0)
            break;
        if (!SCE_S_ISREG(ent.d_stat.st_mode) || strlen(ent.d_name) != 37 || strcmp(ent.d_name + 32, ".json"))
            continue;
        ent.d_name[32] = '\0';
        if (*n == VJO_STATS_CARDS) {
            (*full)++;
            continue;
        }
        if (vjo_stats_card_id(ent.d_name, list[*n].id) < 0)
            continue;
        list[*n].game = VJO_STATS_NO_GAME;
        list[*n].pad = (uint16_t)archive;
        (*n)++;
    }
    sceIoDclose(d);
}

/* A card's game, from its record's platform and title ID (core/anki_queue.h; docs/anki.md): VJO_STATS_NO_GAME for a
 * record that names none, VJO_STATS_UNREAD for one that cannot be read (gone, damaged, being written). */
static uint16_t card_game(const VjoStatsCard *c, VjoArena *a)
{
    static const char hex[] = "0123456789abcdef";
    char name[40], path[128];
    uint16_t game = VJO_STATS_UNREAD;
    for (int i = 0; i < 16; i++) {
        name[2 * i] = hex[c->id[i] >> 4];
        name[2 * i + 1] = hex[c->id[i] & 15];
    }
    name[32] = 0;
    for (int tries = 0; tries < 2 && game == VJO_STATS_UNREAD; tries++) {
        /* (an export may have moved it from the queue to the archive since the list was read) */
        int archive = c->pad ^ tries;
        snprintf(path, sizeof(path), "%s/%s.json", archive ? VJO_QUEUE_EXPORTED_DIR : VJO_QUEUE_DIR, name);
        SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
        if (fd < 0)
            continue;
        size_t mark = vjo_arena_mark(a);
        SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
        char *text = size > 0 && size <= VJO_QUEUE_TEXT_MAX ? vjo_arena_alloc(a, (size_t)size + 1) : NULL;
        VjoJson j;
        if (text && !read_at(fd, text, (size_t)size, 0) && !vjo_json_parse(a, text, (size_t)size, &j) &&
            vjo_json_is_type(&j, 0, JSMN_OBJECT)) {
            int p = vjo_json_obj_get(&j, 0, "platform"), t = vjo_json_obj_get(&j, 0, "title_id");
            char *platform = p >= 0 && vjo_json_is_type(&j, p, JSMN_STRING) ? vjo_json_str(a, &j, p) : NULL;
            char *title = t >= 0 && vjo_json_is_type(&j, t, JSMN_STRING) ? vjo_json_str(a, &j, t) : NULL;
            game = platform && title ? vjo_stats_card_game(&stats, platform, title) : VJO_STATS_NO_GAME;
            tries = 2; /* read: whatever it says */
        } else if (text)
            tries = 2; /* there, but damaged: read again at the next count */
        sceIoClose(fd);
        vjo_arena_release(a, mark);
    }
    return game;
}

/* The cards: every card file's ID, its game from the cache's list, or read from its record when it is new.
 * Returns the records read. */
static int count_cards(void)
{
    VjoStatsCard *list = (VjoStatsCard *)work;
    uint32_t n = 0, full = 0, unknown = 0;
    int read = 0;
    VjoArena a;
    char msg[96];
    list_cards(VJO_QUEUE_DIR, 0, list, &n, &full);
    list_cards(VJO_QUEUE_EXPORTED_DIR, 1, list, &n, &full);
    n = vjo_stats_cards_sort(list, n);
    vjo_arena_init(&a, work + VJO_STATS_CARDS * sizeof(VjoStatsCard), sizeof(work) - VJO_STATS_CARDS * sizeof(VjoStatsCard));
    for (uint32_t k = 0; k < n && !stopped(); k++) {
        int at = vjo_stats_card_find(&stats, list[k].id);
        if (at >= 0 && stats.card[at].game != VJO_STATS_UNREAD) {
            list[k].game = stats.card[at].game;
        } else { /* a new card, or one whose record could not be read (read again: it may be whole now) */
            list[k].game = card_game(&list[k], &a);
            if (++read % 50 == 0) {
                snprintf(msg, sizeof(msg), "Reading cards: %u of %u…", (unsigned)k + 1, (unsigned)n);
                set_progress(msg);
            }
        }
        unknown += list[k].game >= VJO_STATS_UNREAD;
    }
    if (stopped())
        return read; /* the cache keeps its last list */
    for (uint32_t k = 0; k < n; k++)
        list[k].pad = 0;
    memcpy(stats.card, list, n * sizeof(list[0]));
    stats.cards = n;
    stats.cards_full = full;
    stats.cards_unknown = unknown;
    return read;
}

/* stats into stats_cache.bin: a temporary file, synced, then in place of the old one. */
static int save_cache(void)
{
    size_t n = vjo_stats_cache_write(&stats, work, sizeof(work)), used = 0;
    SceUID fd;
    int rc = -1;
    if (!n)
        return -1;
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir("ux0:data/VitaJPOverlay", 0777);
    fd = sceIoOpen(VJO_STATS_CACHE_PATH ".tmp", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0)
        return fd;
    while (used < n) {
        int w = sceIoWrite(fd, work + used, n - used);
        if (w <= 0)
            break;
        used += (size_t)w;
    }
    if (used == n && sceIoSyncByFd(fd, 0) >= 0)
        rc = 0;
    if (sceIoClose(fd) < 0)
        rc = -1;
    if (!rc) {
        sceIoRemove(VJO_STATS_CACHE_PATH);
        rc = sceIoRename(VJO_STATS_CACHE_PATH ".tmp", VJO_STATS_CACHE_PATH) < 0 ? -1 : 0;
    }
    if (rc)
        sceIoRemove(VJO_STATS_CACHE_PATH ".tmp");
    return rc;
}

static int worker_main(SceSize args, void *argp)
{
    static const char *const why_text[] = {"read", "damaged", "of another version", "of another reading_time",
                                           "of another time zone"};
    static const char *const save_text[] = {"saved", "unchanged", "not saved"};
    uint64_t now, from = 0, t0 = sceKernelGetProcessTimeWide(), t1, t2;
    int err = 0, before = 0, read_cards, save = 1, cacheable;
    char msg[240];
    (void)args;
    (void)argp;
    vjo_reading_rule = (uint32_t)count_rule; /* the Shell's rule, as config.ini sets it (core/history_lines.h) */
    int32_t tz = local_offset(&now);
    int why = load_cache(tz, (uint32_t)count_rule);
    uint64_t covered = stats.covered;
    uint32_t cards_before = stats.cards;
    set_progress("Counting history.csv…");
    int64_t bytes = count_history(&from, &err, &before, &cacheable);
    t1 = sceKernelGetProcessTimeWide();
    read_cards = count_cards();
    t2 = sceKernelGetProcessTimeWide();
    if (!cacheable)
        save = 2;
    else if (why || stats.covered != covered || read_cards || stats.cards != cards_before)
        save = save_cache() < 0 ? 2 : 0;
    vjo_log("stats: history.csv %lld B, %llu B counted from %llu in %u ms (%llu records; in all %llu, %llu damaged, "
            "%llu without a time); %u cards, %d read, in %u ms; the cache %s, then %s%s",
            (long long)bytes, (unsigned long long)(stats.covered - from), (unsigned long long)from,
            (unsigned)((t1 - t0) / 1000), (unsigned long long)stats.records - (unsigned long long)before,
            (unsigned long long)stats.records, (unsigned long long)stats.damaged, (unsigned long long)stats.untimed,
            (unsigned)stats.cards, read_cards, (unsigned)((t2 - t1) / 1000),
            why == 1 ? "absent" : why_text[-why < 5 ? -why : 1], save_text[save], stopped() ? "; stopped" : "");
    if (bytes < 0)
        snprintf(msg, sizeof(msg), "Cannot read history.csv (0x%08X). The numbers are from the last count.", (unsigned)err);
    else if (!bytes && !stats.cards)
        snprintf(msg, sizeof(msg), "No reading yet: with Read text Automatically, Rubi keeps the lines it reads in "
                 "history.csv.");
    else
        snprintf(msg, sizeof(msg), "Counted %llu lines of history.csv and %u cards. Start: count again.",
                 (unsigned long long)stats.records, (unsigned)stats.cards);
    sceKernelLockMutex(lock, 1, NULL);
    snprintf(result, sizeof(result), "%s", msg);
    result_error = bytes < 0;
    progress[0] = '\0';
    finished = 1;
    sceKernelUnlockMutex(lock, 1);
    return 0;
}

static void start(void)
{
    int rc;
    if (running)
        return;
    if (lock < 0 && (lock = sceKernelCreateMutex("VjoStatsPage", 0, 0, NULL)) < 0) {
        snprintf(note, sizeof(note), "Could not start the count (0x%08X).", (unsigned)lock);
        note_error = 1;
        lock = -1;
        return;
    }
    finished = cancelled = 0;
    snprintf(progress, sizeof(progress), "Counting history.csv…");
    /* CPU3 at a system priority, as the main thread (settings/sysapp.c) */
    worker = sceKernelCreateThread("VjoStatsCount", worker_main, 0x10000100, WORKER_STACK, 0,
                                   SCE_KERNEL_CPU_MASK_SYSTEM, NULL);
    rc = worker < 0 ? worker : sceKernelStartThread(worker, 0, NULL);
    if (rc < 0) {
        if (worker >= 0)
            sceKernelDeleteThread(worker);
        worker = -1;
        snprintf(result, sizeof(result), "Could not start the count (0x%08X).", (unsigned)rc);
        result_error = 1;
        return;
    }
    running = 1;
}

/* ---------------- what the page shows (the main thread's, built after a count) ---------------- */

typedef struct {
    char name[VJO_STATS_NAME];
    int game; /* its index in stats.game */
    VjoStatsCount total, today;
    uint64_t lines;
    uint32_t cards;
    int32_t last_day;
} GameRow;
static GameRow game_rows[VJO_STATS_GAMES];
static int games_shown, counted, view_dirty;
static VjoStatsCount all_total, today_all, last14[14], year_days[366];
static uint64_t all_lines;
static uint32_t all_cards;
static int32_t today = INT32_MIN, year_first, view_tz;
static int year_len, view_year, streak_now, streak_best;

static VjoConfig cfg;
static int goal, goal_writable = 1, selected, top_row, stick_move, day14 = 13;
static int32_t year_day = INT32_MIN;

/* Today, in the counts' time zone (until the first count: the Vita's now); 1970-01-01 when the clock cannot be read. */
static int32_t day_now(void)
{
    uint64_t now;
    int32_t tz = local_offset(&now), day = vjo_stats_day_in(now, counted ? view_tz : tz);
    return day == INT32_MIN ? 0 : day;
}

/* The days around today and the streak (stats is the page's: no count runs). */
static void build_days(void)
{
    int y, m, d;
    today = day_now();
    vjo_stats_days_range(&stats, today - 13, 14, last14);
    today_all = last14[13];
    vjo_stats_civil(today, &y, &m, &d);
    view_year = y;
    year_first = vjo_stats_days_from_civil(y, 1, 1);
    year_len = vjo_stats_days_from_civil(y + 1, 1, 1) - year_first;
    vjo_stats_days_range(&stats, year_first, year_len, year_days);
    vjo_stats_streak(&stats, today, goal, &streak_now, &streak_best);
    if (year_day == INT32_MIN || year_day > today || year_day < year_first)
        year_day = today;
    for (int k = 0; k < games_shown; k++)
        game_rows[k].today = vjo_stats_game_day(&stats, game_rows[k].game, today);
}

static void build_view(void)
{
    uint32_t cards[VJO_STATS_GAMES];
    uint16_t order[VJO_STATS_GAMES];
    int n;
    all_cards = vjo_stats_cards_per_game(&stats, cards);
    n = vjo_stats_game_order(&stats, order);
    games_shown = 0;
    for (int k = 0; k < n; k++) {
        const VjoStatsGame *g = &stats.game[order[k]];
        GameRow *r = &game_rows[games_shown];
        if (!g->lines && !cards[order[k]])
            continue; /* (a game a card named once, whose cards have gone since) */
        games_shown++;
        snprintf(r->name, sizeof(r->name), "%s", g->name[0] ? g->name : g->uuid);
        r->game = order[k];
        r->total = vjo_stats_game_total(&stats, order[k]);
        r->lines = g->lines;
        r->cards = cards[order[k]];
        r->last_day = g->last_day;
    }
    all_total = vjo_stats_all(&stats, &all_lines);
    view_tz = stats.tz_minutes;
    counted = 1;
    build_days();
    view_dirty = 0;
}

/* The worker is done: join it, then show what it counted. */
static void reap(void)
{
    int done;
    if (!running)
        return;
    sceKernelLockMutex(lock, 1, NULL);
    done = finished;
    sceKernelUnlockMutex(lock, 1);
    if (!done)
        return;
    sceKernelWaitThreadEnd(worker, NULL, NULL);
    sceKernelDeleteThread(worker);
    worker = -1;
    running = 0;
    build_view();
}

void vjo_stats_page_show(VjoArena *scratch)
{
    VjoAppPrefs prefs;
    /* config.ini's reading_time (the Triangle view's rule: an unreadable file is the Shell's defaults' adaptive);
     * the log switch stays main.c's and the Anki page's (an export may be running) */
    if (vjo_preferences_load(&cfg, scratch) < 0)
        cfg.reading_time = VJO_READING_ADAPTIVE;
    goal_writable = vjo_app_prefs_load(&prefs, scratch) >= 0;
    goal = prefs.stats_daily_goal;
    reap(); /* a count done while another page was shown: its numbers, then a new count */
    note[0] = '\0';
    if (!running) {
        if (!counted)
            build_days(); /* the days until the first count is done: today's, and nothing in them */
        count_rule = cfg.reading_time; /* VJO_READING_* */
        start();
    }
    view_dirty = 1;
}

void vjo_stats_page_stick(int move) { stick_move = move; }

void vjo_stats_page_quit(void)
{
    if (!running)
        return;
    sceKernelLockMutex(lock, 1, NULL);
    cancelled = 1;
    sceKernelUnlockMutex(lock, 1);
    sceKernelWaitThreadEnd(worker, NULL, NULL);
    sceKernelDeleteThread(worker);
    worker = -1;
    running = 0;
}

/* ---------------- input ---------------- */

static int rows_total(void) { return ROW_GAMES + games_shown; }
static int row_height(int i)
{
    return i == ROW_TODAY ? TODAY_H : i == ROW_GOAL ? GOAL_H : i == ROW_DAYS ? DAYS_H : i == ROW_YEAR ? YEAR_H : GAME_H;
}
static int row_offset(int i)
{
    int y = 0;
    for (int k = 0; k < i; k++)
        y += row_height(k) + ROW_GAP;
    return y;
}
static int row_y(int i) { return VIEW_Y + row_offset(i) - row_offset(top_row); }
static int row_whole(int i) { return i >= top_row && row_y(i) + row_height(i) <= VIEW_END; }
static void row_show(int i)
{
    if (i < top_row)
        top_row = i;
    while (top_row < i && row_y(i) + row_height(i) > VIEW_END)
        top_row++;
}

static void set_goal(int g, VjoArena *scratch)
{
    static const char *const keys[] = {"stats_daily_goal"};
    int values[1] = {g};
    if (g < 0 || g >= VJO_GOALS || g == goal)
        return;
    if (!goal_writable) {
        snprintf(note, sizeof(note), "Cannot read app.ini. The goal has not been changed.");
        note_error = 1;
        return;
    }
    int rc = vjo_app_prefs_save(keys, values, 1, scratch);
    VjoAppPrefs prefs;
    goal_writable = vjo_app_prefs_load(&prefs, scratch) >= 0;
    goal = prefs.stats_daily_goal;
    note_error = rc < 0 || goal != g;
    snprintf(note, sizeof(note), "%s", note_error ? "Save not confirmed. Check the goal shown, then retry."
                                                  : "Saved in app.ini.");
    if (running)
        view_dirty = 1; /* the streak is counted again once the count is done */
    else
        vjo_stats_streak(&stats, today, goal, &streak_now, &streak_best);
}

static void move_day(int delta)
{
    if (selected == ROW_DAYS) {
        day14 += delta;
        day14 = day14 < 0 ? 0 : day14 > 13 ? 13 : day14;
    } else if (selected == ROW_YEAR) {
        year_day += delta;
        if (year_day < year_first)
            year_day = year_first;
        if (year_day > today)
            year_day = today;
    }
}

/* ---------------- drawing ---------------- */

static void number(char *out, uint64_t n) { vjo_history_number(out, 32, n); }

/* A time as the Triangle view writes it: 45 min, 1 h 23 min. */
static void duration(char *out, size_t cap, uint64_t us)
{
    uint64_t minutes = us / 60000000ull;
    if (minutes < 60)
        snprintf(out, cap, "%u min", (unsigned)minutes);
    else
        snprintf(out, cap, "%llu h %02u min", (unsigned long long)(minutes / 60), (unsigned)(minutes % 60));
}


static void date_text(char *out, size_t cap, int32_t day, int with_year)
{
    static const char *const week[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const month[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int y, m, d;
    if (day == INT32_MIN) {
        snprintf(out, cap, "-");
        return;
    }
    vjo_stats_civil(day, &y, &m, &d);
    if (with_year)
        snprintf(out, cap, "%s %d %s %d", week[vjo_stats_weekday(day)], d, month[m - 1], y);
    else
        snprintf(out, cap, "%s %d %s", week[vjo_stats_weekday(day)], d, month[m - 1]);
}

/* "1,234 characters, 12 min, 6,170/hour": characters an hour as the Triangle view counts them, the timed lines'
 * characters over their time, and none with no time. (ASCII separators: the app's one font, the system's default
 * PVF, has not been checked on a console for the overlay's "·" and "—".) */
static void summary(char *out, size_t cap, const VjoStatsCount *n)
{
    char chars[32], time[32], speed[32];
    number(chars, n->chars);
    duration(time, sizeof(time), n->active_us);
    if (!n->active_us) {
        snprintf(out, cap, "%s characters, %s", chars, time);
        return;
    }
    number(speed, n->timed_chars * 3600000000ull / n->active_us);
    snprintf(out, cap, "%s characters, %s, %s/hour", chars, time, speed);
}

static unsigned blend(unsigned a, unsigned b, int t) /* t/100 of b over a (RGBA8, little-endian) */
{
    unsigned out = 0xFF000000u;
    for (int shift = 0; shift < 24; shift += 8) {
        int x = (int)(a >> shift & 0xFF), y = (int)(b >> shift & 0xFF);
        out |= (unsigned)(x + (y - x) * t / 100) << shift;
    }
    return out;
}

/* An empty bar's or day's colour: the other card colour, so it shows on the row whether focused or not (ui->track
 * is an unfocused card's colour and ui->selected a focused one's; draw_row passes the row's in bg). */
static unsigned slot_of(const VjoStatsUi *ui, unsigned bg) { return bg == ui->selected ? ui->track : ui->selected; }

static void draw_today(const VjoStatsUi *ui, int y, unsigned bg)
{
    char line[160], amount[48], done[32];
    uint32_t goal_chars = vjo_stats_goal_chars(goal), goal_minutes = vjo_stats_goal_minutes(goal);
    ui->text(42, y + 30, ui->white, 28, "Today");
    if (counted)
        summary(line, sizeof(line), &today_all);
    else if (running)
        snprintf(line, sizeof(line), "Counting history.csv…");
    else /* (the count could not start: the status line says why) */
        snprintf(line, sizeof(line), "Not counted yet. Start: count");
    ui->text(150, y + 30, counted ? ui->white : ui->dim, 24, line);
    /* the goal's bar */
    uint32_t p = vjo_stats_goal_progress(&today_all, goal);
    int met = goal && vjo_stats_goal_met(&today_all, goal);
    ui->rect(42, y + 46, 560, 14, slot_of(ui, bg));
    if (goal)
        ui->rect(42, y + 46, (int)(560 * (p > 1000 ? 1000 : p) / 1000), 14, met ? ui->accent : ui->white);
    number(done, goal_chars);
    if (goal_chars)
        snprintf(amount, sizeof(amount), "%s characters", done);
    else
        snprintf(amount, sizeof(amount), "%u minutes", (unsigned)goal_minutes);
    if (!goal)
        snprintf(line, sizeof(line), "No daily goal: choose one below");
    else if (met)
        snprintf(line, sizeof(line), "Goal met: %s", amount);
    else
        snprintf(line, sizeof(line), "%u %% of %s", (unsigned)(p / 10), amount);
    ui->text(616, y + 59, met ? ui->accent : ui->dim, 22, line);
    if (!goal)
        snprintf(line, sizeof(line), "Streak: %d day%s with reading, best %d", streak_now, streak_now == 1 ? "" : "s",
                 streak_best);
    else
        snprintf(line, sizeof(line), "Streak: %d day%s at the goal, best %d", streak_now, streak_now == 1 ? "" : "s",
                 streak_best);
    ui->text(42, y + 92, ui->white, 24, line);
}

static void draw_goal(const VjoStatsUi *ui, int y)
{
    char label[48], n[32], help[120];
    uint32_t chars = vjo_stats_goal_chars(goal), minutes = vjo_stats_goal_minutes(goal);
    ui->text(42, y + 30, goal_writable ? ui->white : ui->dim, 28, "Daily goal");
    number(n, chars);
    if (chars)
        snprintf(label, sizeof(label), "%s characters", n);
    else if (minutes)
        snprintf(label, sizeof(label), "%u minutes", (unsigned)minutes);
    else
        snprintf(label, sizeof(label), "Off");
    ui->text(GOAL_X - 26, y + 31, ui->dim, 26, "<");
    ui->badge(GOAL_X, y + 6, GOAL_W, 34, goal_writable ? label : "--", goal_writable && goal);
    ui->text(GOAL_X + GOAL_W + 12, y + 31, ui->dim, 26, ">");
    if (!goal_writable)
        snprintf(help, sizeof(help), "Cannot read app.ini.");
    else if (!goal)
        snprintf(help, sizeof(help), "Off: a day with any reading counts for the streak.");
    else
        snprintf(help, sizeof(help), "A day at the goal counts for the streak. Left/Right: change.");
    ui->text(42, y + 60, ui->dim, 22, help);
}

static void draw_days(const VjoStatsUi *ui, int y, int focus, unsigned bg)
{
    char line[160], when[32], label[8];
    uint64_t top = 1;
    uint32_t goal_chars = vjo_stats_goal_chars(goal);
    for (int k = 0; k < 14; k++)
        if (last14[k].chars > top)
            top = last14[k].chars;
    if (goal_chars > top)
        top = goal_chars;
    ui->text(42, y + 30, ui->white, 28, "Last 14 days");
    date_text(when, sizeof(when), today - 13 + day14, 0);
    summary(line, sizeof(line), &last14[day14]);
    char detail[200];
    snprintf(detail, sizeof(detail), "%s: %s", when, line);
    ui->wrapped(250, y + 30, 670, focus ? ui->white : ui->dim, 22, 24, 1, detail);
    for (int k = 0; k < 14; k++) {
        int x = BARS_X + k * BAR_PITCH, h = (int)(last14[k].chars * BARS_H / top);
        int met = vjo_stats_goal_met(&last14[k], goal);
        if (last14[k].chars && h < 2)
            h = 2;
        ui->rect(x, y + BARS_DY, BAR_W, BARS_H, slot_of(ui, bg));
        if (h)
            ui->rect(x, y + BARS_DY + BARS_H - h, BAR_W, h, met ? ui->accent : ui->white);
        int d, m, yy;
        vjo_stats_civil(today - 13 + k, &yy, &m, &d);
        snprintf(label, sizeof(label), "%d", d);
        ui->text(x + (BAR_W - ui->text_width(18, label)) / 2, y + BARS_DY + BARS_H + 20,
                 k == day14 ? ui->white : ui->dim, 18, label);
        if (k == day14)
            ui->rect(x, y + BARS_DY + BARS_H + 26, BAR_W, 3, ui->accent);
    }
    if (goal_chars) /* the goal's line across the chart */
        ui->rect(BARS_X, y + BARS_DY + BARS_H - (int)(goal_chars * BARS_H / top), 14 * BAR_PITCH - (BAR_PITCH - BAR_W), 2,
                 ui->off);
}

static void draw_year(const VjoStatsUi *ui, int y, int focus, unsigned bg)
{
    static const char *const months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char title[16], when[32], detail[120], chars[32];
    uint64_t top = 0;
    int32_t first_sunday = year_first - vjo_stats_weekday(year_first);
    for (int k = 0; k < year_len; k++)
        if (year_days[k].chars > top)
            top = year_days[k].chars;
    snprintf(title, sizeof(title), "%d", view_year);
    ui->text(42, y + 30, ui->white, 28, title);
    date_text(when, sizeof(when), year_day, 0);
    number(chars, year_day >= year_first && year_day < year_first + year_len ? year_days[year_day - year_first].chars : 0);
    snprintf(detail, sizeof(detail), "%s: %s characters", when, chars);
    ui->text(130, y + 30, focus ? ui->white : ui->dim, 22, detail);
    ui->text(42, y + HEAT_DY + HEAT_PITCH * 1 + 11, ui->dim, 16, "M");
    ui->text(42, y + HEAT_DY + HEAT_PITCH * 3 + 11, ui->dim, 16, "W");
    ui->text(42, y + HEAT_DY + HEAT_PITCH * 5 + 11, ui->dim, 16, "F");
    for (int k = 0; k < year_len; k++) {
        int32_t day = year_first + k;
        int col = (day - first_sunday) / 7, row = vjo_stats_weekday(day);
        int x = HEAT_X + col * HEAT_PITCH, cy = y + HEAT_DY + row * HEAT_PITCH;
        uint64_t c = year_days[k].chars;
        /* GameSentenceMiner's four levels: a quarter of the year's best day each */
        int level = !c || !top ? 0 : c * 4 <= top ? 1 : c * 2 <= top ? 2 : c * 4 <= top * 3 ? 3 : 4;
        static const int shade[5] = {0, 35, 55, 78, 100};
        unsigned slot = slot_of(ui, bg);
        /* a day read: from the empty colour to the accent; a day still to come: halfway to the card's own colour */
        unsigned color = level ? blend(slot, ui->accent, shade[level]) : day > today ? blend(bg, slot, 40) : slot;
        ui->rect(x, cy, HEAT_CELL, HEAT_CELL, color);
        if (day == year_day) {
            ui->rect(x - 2, cy - 2, HEAT_CELL + 4, 2, ui->white);
            ui->rect(x - 2, cy + HEAT_CELL, HEAT_CELL + 4, 2, ui->white);
            ui->rect(x - 2, cy, 2, HEAT_CELL, ui->white);
            ui->rect(x + HEAT_CELL, cy, 2, HEAT_CELL, ui->white);
        }
        int m, d, yy;
        vjo_stats_civil(day, &yy, &m, &d);
        if (d == 1)
            ui->text(x, y + HEAT_DY + 7 * HEAT_PITCH + 18, ui->dim, 16, months[m - 1]);
    }
}

static void per_10k(char *out, size_t cap, uint32_t cards, uint64_t chars)
{
    if (!chars)
        snprintf(out, cap, "%u card%s", (unsigned)cards, cards == 1 ? "" : "s");
    else {
        uint64_t tenths = (uint64_t)cards * 100000ull / chars; /* cards per 10,000 characters, in tenths */
        snprintf(out, cap, "%u card%s, %llu.%llu per 10,000 characters", (unsigned)cards, cards == 1 ? "" : "s",
                 (unsigned long long)(tenths / 10), (unsigned long long)(tenths % 10));
    }
}

static void draw_all(const VjoStatsUi *ui, int y)
{
    char line[200], cards[96], lines[32];
    ui->text(42, y + 28, ui->white, 26, "All games");
    summary(line, sizeof(line), &all_total);
    ui->text(42, y + 56, ui->white, 22, line);
    per_10k(cards, sizeof(cards), all_cards, all_total.chars);
    number(lines, all_lines);
    snprintf(line, sizeof(line), "%s; %s lines", cards, lines);
    ui->text(42, y + 82, ui->dim, 20, line);
}

static void draw_game(const VjoStatsUi *ui, int y, const GameRow *r)
{
    char line[320], when[48], text[200], cards[96];
    ui->wrapped(42, y + 26, 540, ui->white, 24, 26, 1, r->name);
    if (r->last_day == today)
        snprintf(when, sizeof(when), "read today");
    else if (r->last_day == INT32_MIN)
        snprintf(when, sizeof(when), "no lines read");
    else {
        char d[32];
        date_text(d, sizeof(d), r->last_day, r->last_day < year_first);
        snprintf(when, sizeof(when), "last read %s", d);
    }
    ui->text(918 - ui->text_width(20, when), y + 26, ui->dim, 20, when);
    if (r->today.chars || r->today.active_us) {
        summary(text, sizeof(text), &r->today);
        snprintf(line, sizeof(line), "Today: %s", text);
    } else
        snprintf(line, sizeof(line), "Today: nothing yet");
    ui->text(42, y + 54, ui->white, 20, line);
    summary(text, sizeof(text), &r->total);
    per_10k(cards, sizeof(cards), r->cards, r->total.chars);
    snprintf(line, sizeof(line), "In all: %s; %s", text, cards);
    ui->wrapped(42, y + 80, 880, ui->dim, 20, 22, 1, line);
}

static void draw_row(const VjoStatsUi *ui, int i, int y)
{
    int focus = selected == i;
    unsigned bg = focus ? ui->selected : ui->track; /* the card's colour (main.c's focus_card) */
    ui->card(y, row_height(i), focus);
    switch (i) {
    case ROW_TODAY: draw_today(ui, y, bg); break;
    case ROW_GOAL: draw_goal(ui, y); break;
    case ROW_DAYS: draw_days(ui, y, focus, bg); break;
    case ROW_YEAR: draw_year(ui, y, focus, bg); break;
    case ROW_ALL: draw_all(ui, y); break;
    default: draw_game(ui, y, &game_rows[i - ROW_GAMES]); break;
    }
}

/* A touch at x, y in the rows' room: the row is chosen; on a row in view, the goal's badge sides change it and a
 * bar or a day of the heatmap is chosen. */
static void touch(int x, int y, VjoArena *scratch)
{
    for (int r = top_row; r < rows_total() && row_y(r) < VIEW_END; r++) {
        int ry = row_y(r), dy = y - ry;
        if (dy < 0 || dy >= row_height(r))
            continue;
        selected = r;
        if (!row_whole(r))
            return;
        if (r == ROW_GOAL && dy >= 6 && dy < 40 && x >= GOAL_X - 30 && x < GOAL_X + GOAL_W + 30)
            set_goal(goal + (x >= GOAL_X + GOAL_W / 2 ? 1 : -1), scratch);
        else if (r == ROW_DAYS && dy >= BARS_DY && dy < BARS_DY + BARS_H + 30 && x >= BARS_X &&
                 (x - BARS_X) / BAR_PITCH < 14)
            day14 = (x - BARS_X) / BAR_PITCH;
        else if (r == ROW_YEAR && dy >= HEAT_DY && dy < HEAT_DY + 7 * HEAT_PITCH && x >= HEAT_X) {
            int32_t first_sunday = year_first - vjo_stats_weekday(year_first);
            int32_t day = first_sunday + (x - HEAT_X) / HEAT_PITCH * 7 + (dy - HEAT_DY) / HEAT_PITCH;
            if (day >= year_first && day < year_first + year_len && day <= today)
                year_day = day;
        }
        return;
    }
}

void vjo_stats_page_frame(const VjoStatsUi *ui, uint32_t pressed, int touch_x, int touch_y, VjoArena *scratch)
{
    char status[240], controls[120];
    int stick = stick_move, err = 0, rows;
    stick_move = 0;
    reap();
    if (!running && (view_dirty || (counted && day_now() != today))) {
        if (counted)
            build_days(); /* a new day, or the goal changed while the count ran */
        view_dirty = 0;
    }
    rows = rows_total();
    if (pressed & SCE_CTRL_UP)
        selected = (selected + rows - 1) % rows;
    if (pressed & SCE_CTRL_DOWN)
        selected = (selected + 1) % rows;
    if (!(pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN)))
        selected = vjo_stick_step(selected, rows, stick);
    if (selected >= rows)
        selected = rows - 1;
    if (selected == ROW_GOAL) {
        /* Left/Right step within the choices; Enter goes round them */
        if (pressed & SCE_CTRL_LEFT)
            set_goal(goal - 1, scratch);
        if (pressed & SCE_CTRL_RIGHT)
            set_goal(goal + 1, scratch);
        if (pressed & ui->buttons->enter)
            set_goal((goal + 1) % VJO_GOALS, scratch);
    } else {
        if (pressed & SCE_CTRL_LEFT)
            move_day(-1);
        if (pressed & SCE_CTRL_RIGHT)
            move_day(1);
        if (pressed & SCE_CTRL_LTRIGGER)
            move_day(-7);
        if (pressed & SCE_CTRL_RTRIGGER)
            move_day(7);
    }
    if ((pressed & SCE_CTRL_START) && !running) {
        note[0] = '\0';
        count_rule = cfg.reading_time;
        start();
    }
    if (touch_y >= VIEW_Y && touch_y < VIEW_END && touch_x >= 24 && touch_x < 936)
        touch(touch_x, touch_y, scratch);
    status[0] = '\0';
    if (lock >= 0) {
        sceKernelLockMutex(lock, 1, NULL);
        snprintf(status, sizeof(status), "%s", running && progress[0] ? progress : result);
        err = !running && result_error;
        sceKernelUnlockMutex(lock, 1);
    }
    if (note[0]) {
        snprintf(status, sizeof(status), "%s", note);
        err = note_error;
    }
    if (selected == ROW_GOAL)
        snprintf(controls, sizeof(controls), "Up/Down: row   Left/Right: goal   Start: count again   %s: exit",
                 ui->buttons->cancel_glyph);
    else if (selected == ROW_DAYS || selected == ROW_YEAR)
        snprintf(controls, sizeof(controls), "Up/Down: row   Left/Right: day   L/R: week   %s: exit",
                 ui->buttons->cancel_glyph);
    else
        snprintf(controls, sizeof(controls), "Up/Down: row   Start: count again   Select: page   %s: exit",
                 ui->buttons->cancel_glyph);

    vita2d_start_drawing();
    vita2d_clear_screen();
    ui->header();
    row_show(selected);
    for (int i = top_row; i < rows && row_y(i) < VIEW_END; i++) {
        int y = row_y(i), whole = row_whole(i);
        if (!whole) { /* cut at the bottom of the room: its card and its text */
            vita2d_enable_clipping();
            vita2d_set_clip_rectangle(24, y, 936, VIEW_END);
        }
        draw_row(ui, i, y);
        if (!whole)
            vita2d_disable_clipping();
    }
    { /* where the view is in the column: a thin bar right of the rows */
        int room = VIEW_END - VIEW_Y, all = row_offset(rows - 1) + row_height(rows - 1);
        int from = row_offset(top_row), shown = all - from < room ? all - from : room;
        if (all > room) {
            ui->rect(SCROLL_X, VIEW_Y, SCROLL_W, room, ui->track);
            ui->rect(SCROLL_X, VIEW_Y + room * from / all, SCROLL_W, room * shown / all, ui->dim);
        }
    }
    ui->footer(FOOTER_Y, FOOTER_H, FOOTER_Y + 22, 1, status, err, controls);
    vita2d_end_drawing();
    vita2d_swap_buffers();
}
