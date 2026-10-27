#include "lookup_history.h"

#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <string.h>

#include "../core/arena.h"
#include "../core/history_read.h"
#include "../core/textfilter.h"
#include "../core/utf.h"

void vjo_log(const char *fmt, ...); /* the app's log.txt lines (settings/applog.c) */

#define MISSING ((int)0x80010002u) /* ENOENT: nothing recorded yet */

static char window[VJO_LOOKUP_WINDOW + 1];     /* a window, the byte before it first */
static uint8_t filter_mem[16u * 1024u];        /* a line's filtered copy (a line has at most 4,095 bytes) */
static VjoLookupGame games[VJO_LOOKUP_GAMES];
static int n_games;
static VjoLookupLine lines[VJO_LOOKUP_LINES];
static int n_lines;
static char want[sizeof(games[0].uuid)];       /* the game whose lines are read: chosen, or the newest record's */
static int chosen;                             /* want is the user's choice (vjo_lookup_history_select) */
static uint64_t last_hash;                     /* the line kept last (the next newer one), as the scan's keeps it */
static int have_last;
static int state = VJO_LOOKUP_HISTORY_NONE, error_rc, filter_mode = -1;
static uint64_t size, next, stop; /* the file's size; the next window's end (the oldest record start found); the end */
static int games_done;            /* a pass has read [stop, size): every game there is listed */

int vjo_lookup_history_state(void) { return state; }
int vjo_lookup_history_error(void) { return error_rc; }
uint64_t vjo_lookup_history_size(void) { return size; }
uint64_t vjo_lookup_history_read(void) { return size - next; }

int vjo_lookup_history_games(const VjoLookupGame **out)
{
    *out = games;
    return n_games;
}

int vjo_lookup_history_lines(const VjoLookupLine **out)
{
    *out = lines;
    return n_lines;
}

static int find_game(const char *uuid)
{
    for (int i = 0; i < n_games; i++)
        if (!strcmp(games[i].uuid, uuid))
            return i;
    return -1;
}

int vjo_lookup_history_game(void) { return want[0] ? find_game(want) : -1; }

/* s's first n bytes into out (cap bytes), cut on a character. */
static void copy_cut(char *out, size_t cap, const char *s, size_t n)
{
    if (n > cap - 1) {
        n = cap - 1;
        while (n && ((unsigned char)s[n] & 0xC0) == 0x80)
            n--;
    }
    memcpy(out, s, n);
    out[n] = '\0';
}

/* The lines' pass again from the file's end (the games found stay). */
static void restart_lines(void)
{
    n_lines = 0;
    have_last = 0;
    next = size;
    state = size ? VJO_LOOKUP_HISTORY_READING : VJO_LOOKUP_HISTORY_NONE;
}

void vjo_lookup_history_open(int filter)
{
    SceUID fd = sceIoOpen(VJO_LOOKUP_HISTORY_PATH, SCE_O_RDONLY, 0);
    SceOff end = fd < 0 ? fd : sceIoLseek(fd, 0, SCE_SEEK_END);
    if (fd >= 0)
        sceIoClose(fd);
    if (end >= 0 && (uint64_t)end == size && filter == filter_mode &&
        (state == VJO_LOOKUP_HISTORY_READING || state == VJO_LOOKUP_HISTORY_READ))
        return; /* the same file: the scan done or under way stands */
    n_games = 0;
    games_done = 0;
    if (!chosen)
        want[0] = '\0'; /* the newest record's game, which may be another one now */
    filter_mode = filter;
    error_rc = end < 0 ? (int)end : 0;
    size = end < 0 ? 0 : (uint64_t)end;
    stop = size > VJO_LOOKUP_SCAN_MAX ? size - VJO_LOOKUP_SCAN_MAX : 0;
    restart_lines(); /* (want kept: the game chosen, when the file still has it) */
    if (end < 0) {
        state = (int)end == MISSING ? VJO_LOOKUP_HISTORY_NONE : VJO_LOOKUP_HISTORY_FAILED;
        if (state == VJO_LOOKUP_HISTORY_FAILED)
            vjo_log("look up: %s cannot be read 0x%08X", VJO_LOOKUP_HISTORY_PATH, (unsigned)end);
    }
}

static void select_game(int g, int by_user)
{
    snprintf(want, sizeof(want), "%s", games[g].uuid);
    chosen = by_user;
    restart_lines();
}

void vjo_lookup_history_select(int g)
{
    if (g >= 0 && g < n_games)
        select_game(g, 1);
}

static uint64_t fnv64(const char *s)
{
    uint64_t h = 14695981039346656037ull;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 1099511628211ull;
    return h;
}

/* One record of the window, newest first: its game listed, its line kept when it is the wanted game's. */
static void take(char *start, char *end, uint64_t offset)
{
    VjoHistoryRow row;
    VjoArena a;
    char *f, *t;
    uint64_t hash;
    int g;
    if (vjo_history_row(start, end, VJO_HISTORY_COL_MODEL + 1, &row) || !row.len[0] ||
        row.len[0] >= sizeof(games[0].uuid) || memchr(row.uuid, '\0', row.len[0]))
        return; /* damaged, or no game Rubi writes */
    if ((g = find_game(row.uuid)) < 0 && n_games < VJO_LOOKUP_GAMES) {
        g = n_games++;
        snprintf(games[g].uuid, sizeof(games[g].uuid), "%s", row.uuid);
        copy_cut(games[g].name, sizeof(games[g].name), row.len[2] ? row.name : row.uuid,
                 row.len[2] ? strlen(row.name) : strlen(row.uuid));
    }
    if (!want[0])
        snprintf(want, sizeof(want), "%s", row.uuid); /* the newest record's game */
    if (n_lines >= VJO_LOOKUP_LINES || strcmp(row.uuid, want))
        return;
    /* as the overlay shows it: filtered, trimmed; none empty, none the same as the one kept after it */
    vjo_arena_init(&a, filter_mem, sizeof(filter_mem));
    if (!(f = vjo_filter_lines(&a, row.line, filter_mode)) || !*(t = vjo_java_trim(f)))
        return;
    hash = fnv64(t);
    if (have_last && hash == last_hash)
        return;
    last_hash = hash;
    have_last = 1;
    VjoLookupLine *l = &lines[n_lines++];
    l->offset = offset;
    l->bytes = (uint32_t)(end - start);
    l->time_us = row.time_us;
    copy_cut(l->preview, sizeof(l->preview), t, strlen(t));
    for (char *p = l->preview; *p; p++)
        if (*p == '\n' || *p == '\r' || *p == '\t')
            *p = ' '; /* one row in the list */
}

/* The window that ends at `next`, read and walked back. */
static void one_window(void)
{
    uint64_t lo = next > VJO_LOOKUP_WINDOW ? next - VJO_LOOKUP_WINDOW : 0, base;
    size_t len, got = 0, oldest;
    int whole, fd;
    if (lo < stop)
        lo = stop;
    whole = lo == 0;
    base = whole ? 0 : lo - 1; /* the byte before the window says whether its first byte starts a record */
    len = (size_t)(next - base);
    if ((fd = sceIoOpen(VJO_LOOKUP_HISTORY_PATH, SCE_O_RDONLY, 0)) < 0) {
        error_rc = fd;
    } else {
        while (got < len) {
            int n = sceIoPread(fd, window + got, (SceSize)(len - got), (SceOff)(base + got));
            if (n <= 0)
                break;
            got += (size_t)n;
        }
        sceIoClose(fd);
        error_rc = got == len ? 0 : -1;
    }
    if (error_rc) { /* gone, or shorter: the page says so, and Start reads it again */
        state = VJO_LOOKUP_HISTORY_FAILED;
        vjo_log("look up: reading %s at %llu failed 0x%08X", VJO_LOOKUP_HISTORY_PATH, (unsigned long long)base,
                (unsigned)error_rc);
        return;
    }
    oldest = len;
    for (size_t e = len, s; (s = vjo_history_prev_start(window, e, len, whole)) < len; e = s) {
        take(window + s, window + e, base + s);
        oldest = s;
    }
    /* the next window ends at the oldest record found here (the record before it may start in the window before);
     * with none, these bytes are part of a longer one, left out */
    next = oldest < len ? base + oldest : lo;
    if (next <= stop || whole) {
        next = stop;
        games_done = 1;
        if (vjo_lookup_history_game() < 0 && n_games) { /* the chosen game is no longer there: the newest one's */
            select_game(0, 0);
            return;
        }
        state = n_games ? VJO_LOOKUP_HISTORY_READ : VJO_LOOKUP_HISTORY_NONE;
    } else if (games_done && n_lines >= VJO_LOOKUP_LINES) {
        state = VJO_LOOKUP_HISTORY_READ; /* every game listed already, and this one's lines found */
    }
}

int vjo_lookup_history_step(uint32_t budget_us)
{
    uint64_t t0 = sceKernelGetProcessTimeWide();
    while (state == VJO_LOOKUP_HISTORY_READING) {
        one_window();
        if (sceKernelGetProcessTimeWide() - t0 >= budget_us)
            break;
    }
    return state == VJO_LOOKUP_HISTORY_READING;
}

int vjo_lookup_history_line(int i, char *text, size_t cap, VjoLookupSource *src)
{
    const VjoLookupLine *l;
    VjoHistoryRow row;
    size_t got = 0;
    int fd, g;
    if (i < 0 || i >= n_lines || cap < 2)
        return -1;
    l = &lines[i];
    if (!l->bytes || l->bytes > VJO_LOOKUP_WINDOW || (fd = sceIoOpen(VJO_LOOKUP_HISTORY_PATH, SCE_O_RDONLY, 0)) < 0)
        return -1;
    while (got < l->bytes) {
        int n = sceIoPread(fd, window + got, (SceSize)(l->bytes - got), (SceOff)(l->offset + got));
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    sceIoClose(fd);
    if (got != l->bytes || !vjo_history_record_start(window, 0, got, 1) ||
        vjo_history_row(window, window + got, VJO_HISTORY_COL_MODEL + 1, &row) || strcmp(row.uuid, want))
        return -1; /* the file changed */
    copy_cut(text, cap, row.line, row.len[3]);
    memset(src, 0, sizeof(*src));
    g = find_game(want);
    copy_cut(src->game, sizeof(src->game), g >= 0 ? games[g].name : row.name, strlen(g >= 0 ? games[g].name : row.name));
    /* the platform and title ID: their columns, else the uuid's two parts (vita-PCSG00415) */
    const char *dash = strchr(row.uuid, '-');
    if (row.cells > VJO_HISTORY_COL_TITLE_ID && row.len[VJO_HISTORY_COL_TITLE_ID] &&
        row.len[VJO_HISTORY_COL_PLATFORM]) {
        copy_cut(src->platform, sizeof(src->platform), row.cell[VJO_HISTORY_COL_PLATFORM],
                 row.len[VJO_HISTORY_COL_PLATFORM]);
        copy_cut(src->title_id, sizeof(src->title_id), row.cell[VJO_HISTORY_COL_TITLE_ID],
                 row.len[VJO_HISTORY_COL_TITLE_ID]);
    } else if (dash) {
        copy_cut(src->platform, sizeof(src->platform), row.uuid, (size_t)(dash - row.uuid));
        copy_cut(src->title_id, sizeof(src->title_id), dash + 1, strlen(dash + 1));
    }
    if (row.cells > VJO_HISTORY_COL_ENGINE)
        copy_cut(src->engine, sizeof(src->engine), row.cell[VJO_HISTORY_COL_ENGINE], row.len[VJO_HISTORY_COL_ENGINE]);
    if (row.cells > VJO_HISTORY_COL_MODEL)
        copy_cut(src->model, sizeof(src->model), row.cell[VJO_HISTORY_COL_MODEL], row.len[VJO_HISTORY_COL_MODEL]);
    src->time_us = row.time_us;
    return 0;
}
