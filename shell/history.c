#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/rtc.h> /* the streak's day */
#if VJO_DEBUG
#include <psp2/kernel/processmgr.h> /* the total's count time, in its log line */
#endif

#include "history.h"
#include "shell.h"
#include "../core/port.h"
#include "../core/streak.h"
#include "../core/textnorm.h"

static const char HISTORY_PATH[] VJO_DATA_TABLE(reading_history_path) = VJO_DATA_DIR "/history.csv";

/* The writer's strings, as its header and offsets: in the data segment (core/port.h), whose last
 * page has room. They pay for the characters column's count in the normal Shell's full code pages
 * (docs/ocr-history.md). The file's path is not among them: its one copy is the code segment's string,
 * which the line-history reader (vjo_history_load) has too, so the append's open and rollback share it
 * (a data-segment copy of it, 35 B, paid for the line_source column's cells: docs/memory-ledger.md). */
static const char id_format[] VJO_DATA_TABLE(history_id_format) = "vjo-%llu-%llu";
static const char time_format[] VJO_DATA_TABLE(history_time_format) = ",%llu.%06u,";
static const char number_format[] VJO_DATA_TABLE(history_number_format) = ",%llu";
static const char line_format[] VJO_DATA_TABLE(history_line_format) = "%s%u:%u:%u:%u:%u";
/* Where the line came from (VjoGameOcrResult.text_from: 0 OCR, else a native text hook's stream): for
 * each, its engine cell and, after that cell's NUL, its line_source cell with the record's end. The writer
 * keeps one pointer into this table for the record (HistoryWriter.cells), so the two cells always agree.
 * In the data segment too, and the pointer in the writer's frame: the normal Shell's code stays the same
 * size (docs/ocr-history.md#line-source). */
#define OCR_ENGINE ",\"vocr\","
#define OCR_SOURCE ",\"ocr\"\n"
#define HOOK_ENGINE ",\"hook\","
#define HOOK_SOURCE ",\"hook\"\n"
static const char source_cells[] VJO_DATA_TABLE(history_source_cells) =
    OCR_ENGINE "\0" OCR_SOURCE "\0" HOOK_ENGINE "\0" HOOK_SOURCE;
#define HOOK_CELLS (sizeof(OCR_ENGINE) + sizeof(OCR_SOURCE)) /* a hook's cells, after OCR's */
#define LINE_SOURCE sizeof(OCR_ENGINE)                        /* the line_source cell, after the engine's */
_Static_assert(sizeof(HOOK_ENGINE) == LINE_SOURCE, "both sources' line_source cells at one offset");

/* Only fingerprints survive a call. No text or file-size-dependent memory. */
static uint64_t previous_text;
static uint32_t previous_game, previous_length;
/* API 49: the newest record's place in the file, its time, and the last append's: a line that grew since (a hook
 * that gives its text as it types: the text it was, then more, each part read at once) replaces that record, at the
 * same time, instead of adding one. A typed line's parts come a fraction of a second apart; a reader's next line,
 * even one that starts with the last (a page that collects its lines), comes later. */
static SceOff previous_start, previous_end;
static uint64_t previous_us, previous_append_us;
#define GROWN_GAP_US 750000ull

/* The reading total (docs/ocr-history.md#total-characters-read), which the overlay's Triangle view shows:
 * the characters of the lines of history.csv's first `covered` bytes. The writer adds each record's as it
 * appends it, when the record is the next one after them; what it did not add (the file's lines before
 * this Rubi, or those appended while the total was being counted or unknown) the control thread counts
 * from the file, a window at a time, once a Triangle press asks for it (vjo_history_total_step).
 * history_total.bin keeps the total between restarts, so a count reads only what was appended since it
 * was saved. The view lock guards chars, covered and state: the writer (VjoNet), the count (the control
 * thread) and the view (the paf thread) take it. Offsets are 32-bit: a history.csv of 4 GiB or more (some
 * ten million lines) has no total ("unavailable"). */
static const char TOTAL_PATH[] VJO_DATA_TABLE(reading_legacy_cache_path) = VJO_DATA_DIR "/history_total.bin";
enum { TOTAL_UNREAD, TOTAL_COUNTING, TOTAL_KNOWN, TOTAL_FAILED };
static struct {
    VjoReadingScope scope, scan_scope;
    int requested;
    uint32_t streak; /* the Triangle view's streak (vjo_history_streak): the current streak as of the day it was read,
                      * and the longest in the high 16 bits (in what was the padding before saved_us) */
    uint64_t saved_us;
    VjoReadingTime time, scan_time; /* committed prefix and the count in progress */
    uint64_t chars, found;            /* chars: the lines' in [0, covered); found: the count's in [covered, at) */
    uint32_t covered, at, top, saved; /* the count: from covered (at: its next window) to top, the file's end */
    SceUID fd;                        /* history.csv while a count reads it (0: closed) */
    uint32_t seq, top_seq;            /* seq: odd while the writer appends a record; top_seq: when top was read */
    int state, read, clean;           /* TOTAL_*; read: history_total.bin was read; clean: no append ran as top
                                       * was read */
    uint32_t streak_seq;              /* `streak`'s stores (vjo_history_streak); 0: not read since the Shell started */
#if VJO_DEBUG
    uint32_t windows, bytes; /* the count's windows and bytes read, and when it started: its log line */
    int64_t started;
#endif
} total;
/* history_total.bin / reading-<game>.bin: the total, the bytes of history.csv it is the total of, and a check
 * that a torn or foreign file does not have, nor one counted with the other reading_time rule: the check
 * starts from the rule (VJO_READING_GAPS_15S's start is 0.9.7's, whose files it counted with that rule, so
 * they stay valid with it; adaptive's differs, and a 0.9.7 file is counted again, from history.csv's start) */
typedef struct {
    uint64_t chars;
    uint32_t covered, check;
    VjoReadingTime time;
    VjoReadingScope scope;
} TotalFile;
_Static_assert(sizeof(TotalFile) == 112, "reading cache layout");
static uint32_t total_file_check(const TotalFile *f)
{
    uint32_t check = 0x33544d44u + vjo_reading_rule;
    const unsigned char *p = (const unsigned char *)f;
    for (size_t i = 0; i < sizeof(*f); i++)
        if (i < offsetof(TotalFile, check) || i >= offsetof(TotalFile, time))
            check = (check * 16777619u) ^ p[i];
    return check;
}

typedef struct {
    SceUID fd;
    int error;
    unsigned used, written;
    const char *cells; /* this record's in source_cells: its engine cell, then its line_source cell */
    char bytes[192];
} HistoryWriter;

static void flush(HistoryWriter *w)
{
    unsigned off = 0;
    while (!w->error && off < w->used) {
        int n = sceIoWrite(w->fd, w->bytes + off, w->used - off);
        if (n <= 0 || (unsigned)n > w->used - off)
            w->error = n < 0 ? n : -1;
        else
            off += (unsigned)n;
    }
    w->written += off; /* the record's bytes (the total's covered: vjo_history_append) */
    w->used = 0;
}

static void put(HistoryWriter *w, char c)
{
    if (w->error) return;
    if (w->used == sizeof(w->bytes)) flush(w);
    if (!w->error) w->bytes[w->used++] = c;
}

static void literal(HistoryWriter *w, const char *s)
{
    while (*s && !w->error) put(w, *s++);
}

static void field(HistoryWriter *w, const char *s, size_t cap)
{
    put(w, '"');
    if (s) for (size_t i = 0; i < cap && s[i] && !w->error; i++) {
        if (s[i] == '"') put(w, '"');
        put(w, s[i]);
    }
    put(w, '"');
}

static void number(HistoryWriter *w, uint64_t n)
{
    char text[24];
    sceClibSnprintf(text, sizeof(text), number_format, (unsigned long long)n);
    literal(w, text);
}

void vjo_history_reset(void)
{
    previous_text = 0;
    previous_game = previous_length = 0;
}

int vjo_history_append(const VjoHistoryRecord *r)
{
    static const char header[] VJO_DATA_TABLE(history_header) =
        "uuid,given_identifier,name,line,time,platform,title_id,engine,model,raw_line,"
        "scene,region_seq,monotonic_us,elapsed_us,job_seq,region_x,region_y,width,height,"
        "fb_width,fb_height,fb_pitch,fb_format,lines,recognized,frames,find_us,load_us,"
        "recognize_us,capture_us,captures,tears,region_sum,source,capture_memory,"
        "capture_bytes,strip_rows,workspace_bytes,resident_bytes,fallback,line_metrics,characters,line_source\n";
#define OFFSET(member) offsetof(VjoGameOcrResult, member)
    static const uint16_t offsets[] VJO_DATA_TABLE(history_offsets) = {
        OFFSET(seq), OFFSET(region_x), OFFSET(region_y), OFFSET(width), OFFSET(height),
        OFFSET(fb_width), OFFSET(fb_height), OFFSET(fb_pitch), OFFSET(fb_format),
        OFFSET(lines), OFFSET(recognized), OFFSET(frames), OFFSET(find_us), OFFSET(load_us),
        OFFSET(recognize_us), OFFSET(capture_us), OFFSET(captures), OFFSET(tears), OFFSET(region_sum),
        OFFSET(source), OFFSET(capture_memory), OFFSET(capture_bytes), OFFSET(strip_rows),
        OFFSET(workspace_bytes), OFFSET(resident_bytes), OFFSET(fallback)
    };
#undef OFFSET
    const VjoGameOcrResult *o;
    uint64_t hash = 14695981039346656037ull, prefix = 0, us;
    uint32_t game = 2166136261u;
    size_t len = 0;
    int visible = 0, rc, closed, grown;
    uint32_t chars;
    SceOff start;
    SceIoStat st;
    HistoryWriter w = {.fd = -1};
    char text[72];
    if (!r || !(o = r->result) || o->rc || !r->time_us || !r->title_id[0] ||
        sceClibStrnlen(r->title_id, sizeof(r->title_id)) == sizeof(r->title_id)) return -1;
    while (len < sizeof(o->text) && o->text[len]) {
        unsigned char c = (unsigned char)o->text[len++];
        hash = (hash ^ c) * 1099511628211ull;
        visible |= c > ' ';
        if (len == previous_length) prefix = hash; /* the text's first part, as long as the newest record's */
    }
    if (len == sizeof(o->text)) return -1;
    if (!visible) { vjo_history_reset(); return 0; }
    for (const char *p = r->platform ? r->platform : "vita"; *p; p++)
        game = (game ^ (unsigned char)*p) * 16777619u;
    game = (game ^ '-') * 16777619u;
    for (size_t i = 0; i < sizeof(r->title_id) && r->title_id[i]; i++)
        game = (game ^ (unsigned char)r->title_id[i]) * 16777619u;
    if (previous_text == hash && previous_length == len && previous_game == game) return 0;
    grown = previous_text && prefix == previous_text && previous_game == game && len > previous_length &&
            r->time_us - previous_append_us < GROWN_GAP_US; /* (unsigned: an earlier time is no growth) */
    w.fd = sceIoOpen(HISTORY_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (w.fd < 0) return w.fd;
    start = sceIoLseek(w.fd, 0, SCE_SEEK_END);
    if (start < 0) { sceIoClose(w.fd); return (int)start; }
    total.seq++; /* odd until the record is whole or gone: a count's end of the file is not read meanwhile */
    __sync_synchronize();
    /* #338's audit F3: the newest record goes (this one takes its place, its time and its ID) only once the file is
     * open, inside that window, and not while a count reads the file (total.fd: the control thread's, read here only
     * to leave it alone); the end is read after it, which is where the next write goes whether the console's
     * SCE_O_APPEND appends at each write or sets the position at the open */
    sceClibMemset(&st, 0, sizeof(st));
    st.st_size = previous_start;
    if (grown && start == previous_end && !total.fd && sceIoChstat(HISTORY_PATH, &st, SCE_CST_SIZE) >= 0)
        start = sceIoLseek(w.fd, 0, SCE_SEEK_END);
    else
        grown = 0;
    us = grown ? previous_us : r->time_us;
    if (!start) literal(&w, header);
    put(&w, '"');
    literal(&w, r->platform ? r->platform : "vita");
    put(&w, '-');
    literal(&w, r->title_id);
    literal(&w, "\",\"");
    sceClibSnprintf(text, sizeof(text), id_format, (unsigned long long)us, (unsigned long long)start);
    literal(&w, text);
    literal(&w, "\",");
    field(&w, r->name[0] ? r->name : r->title_id, r->name[0] ? sizeof(r->name) : sizeof(r->title_id));
    put(&w, ',');
    field(&w, o->text, len);
    sceClibSnprintf(text, sizeof(text), time_format, (unsigned long long)(us / 1000000ull),
                    (unsigned)(us % 1000000ull));
    literal(&w, text);
    field(&w, r->platform, 16);
    put(&w, ','); field(&w, r->title_id, sizeof(r->title_id));
    w.cells = source_cells + (o->text_from ? HOOK_CELLS : 0);
    literal(&w, w.cells); /* the engine: vocr, or hook for the line a native text hook read (API 31) */
    field(&w, r->model, VJO_GAME_OCR_MODEL_BYTES);
    put(&w, ','); field(&w, vjo_text_raw(o->text, sizeof(o->text)), sizeof(o->text) - len - 1);
    number(&w, r->scene); number(&w, r->region_seq);
    number(&w, r->monotonic_us); number(&w, r->elapsed_us);
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
        number(&w, *(const uint32_t *)((const char *)o + offsets[i]));
    /* Per-line crop, CTC frames, flags (confidence and LM), recognition us.
     * A quoted field keeps the nested records independent of CSV columns. */
    literal(&w, ",\"");
    for (uint32_t i = 0; i < o->recognized && i < VJO_GAME_OCR_LINES; i++) {
        const VjoGameOcrLine *line = &o->line[i];
        sceClibSnprintf(text, sizeof(text), line_format, i ? "|" : "",
                        line->width, line->height, line->frames, line->flags, (unsigned)line->us);
        literal(&w, text);
    }
    put(&w, '"');
    /* The line's characters as a reading tracker counts them (core/reading_chars.h, which the total's
     * count uses too: vjo_line_characters), then where the line
     * came from (hook or ocr) and the record's end, from the engine's table: appended columns, so every
     * earlier column keeps its position (docs/ocr-history.md). */
    chars = vjo_line_characters(o->text, len);
    number(&w, chars);
    literal(&w, w.cells + LINE_SOURCE);
    flush(&w);
    rc = w.error;
    if (!rc) rc = sceIoSyncByFd(w.fd, 0);
    closed = sceIoClose(w.fd);
    if (!rc && closed < 0) rc = closed;
    if (rc < 0) { /* back to where this record began (a grown line's: where its earlier record began) */
        st.st_size = start;
        sceIoChstat(HISTORY_PATH, &st, SCE_CST_SIZE);
    } else {
        SceOff end = start + w.written, was_end = previous_end;
        previous_text = hash;
        previous_game = game;
        previous_length = (uint32_t)len;
        previous_start = start;
        previous_end = end;
        previous_us = us;
        previous_append_us = r->time_us;
        /* the total: this record's characters, when it is the next one after the total's bytes */
        vjo_view_lock();
        if (total.state == TOTAL_KNOWN) {
            if (grown && total.covered == was_end && !(end >> 32)) { /* the line it replaces was counted: its growth */
                total.chars += vjo_reading_scope_grow(&total.time, &total.scope, us, chars);
                total.covered = (uint32_t)end;
                g_view.version++;
            } else if (total.covered == start && !(end >> 32)) {
                vjo_reading_scope_add(&total.time, &total.scope, us, game, chars);
                total.chars += total.time.last_chars;
                if (total.scope.end_us && us >= total.scope.end_us) total.requested = 1;
                total.covered = (uint32_t)end;
                g_view.version++;
            } else { /* the file is not the one the total is of, or has more (or 4 GiB): the control thread
                      * counts it */
                total.state = TOTAL_COUNTING;
            }
        }
        vjo_view_unlock();
        rc = 1;
    }
    __sync_synchronize();
    total.seq++;
    if (rc == 1 && chars)
        vjo_history_streak(1); /* the reading streak: this line's day (core/streak.h) */
    return rc;
}

int vjo_history_load(VjoHistory *h, VjoArena *lines, const char *uuid, int filter, VjoArena *scratch)
{
    size_t mark = vjo_arena_mark(scratch), n = 0, got = 0;
    /* the window and the byte before it, which says whether the window's first byte starts a record */
    char *buf = vjo_arena_alloc(scratch, VJO_HISTORY_WINDOW + 1);
    SceUID fd = buf ? sceIoOpen(HISTORY_PATH, SCE_O_RDONLY, 0) : -1;
    SceOff end = fd < 0 ? fd : sceIoLseek(fd, 0, SCE_SEEK_END), from = end > VJO_HISTORY_WINDOW ? end - VJO_HISTORY_WINDOW - 1 : 0;
    if (end > 0 && sceIoLseek(fd, from, SCE_SEEK_SET) == from)
        for (n = (size_t)(end - from); got < n;) { /* a short read: the file shrank (an append rolled back) */
            int r = sceIoRead(fd, buf + got, n - got);
            if (r <= 0)
                break;
            got += (size_t)r;
        }
    if (fd >= 0)
        sceIoClose(fd);
    vjo_history_scan(h, lines, buf, got, !from, uuid, filter);
    vjo_arena_release(scratch, mark);
    return end < 0 ? (int)end : h->n;
}

/* ---- the total (docs/ocr-history.md#total-characters-read) ---- */

/* One bounded cache per game, with the full UUID also checked inside the file; the streak's is reading-streak.bin
 * (core/streak.h VJO_STREAK_FILE), which no game's ID makes. */
static const char *cache_path(char *buf, size_t cap, const char *name)
{
    static const char format[] VJO_DATA_TABLE(history_cache_path) = VJO_DATA_DIR "/reading-%s.bin";
    if (!name[0]) return TOTAL_PATH;
    sceClibSnprintf(buf, cap, format, name);
    return buf;
}

/* The reading streak (core/streak.h), today's day on the Vita's clock: days since 1970-01-01 of its local time. */
static int32_t streak_today(void)
{
    SceDateTime date;
    SceRtcTick tick;
    if (sceRtcGetCurrentClockLocalTime(&date) < 0 || sceRtcGetTick(&date, &tick) < 0)
        return VJO_STREAK_NONE;
    return (int32_t)(tick.tick / VJO_STREAK_DAY_US) - 719162; /* (SceRtcTick counts from 0001-01-01) */
}

__attribute__((noinline)) void vjo_history_streak(int add)
{
    char path[64];
    VjoStreak s;
    uint32_t seq = total.streak_seq; /* the stores before this read (the writer's: VjoNet; the view's: VjoControl) */
    int32_t today = streak_today();
    SceUID fd = sceIoOpen(cache_path(path, sizeof(path), VJO_STREAK_NAME), SCE_O_RDONLY, 0);
    int n = fd < 0 ? fd : sceIoRead(fd, &s, sizeof(s));
    if (fd >= 0)
        sceIoClose(fd);
    if (n < 0 && n != (int)0x80010002)
        return; /* there, but not readable now: never written over (the next line tries again) */
    if (n != (int)sizeof(s) || s.magic != VJO_STREAK_MAGIC)
        vjo_streak_init(&s); /* none yet, or not one this writes: a new record (core/streak.c's rules take any fields) */
    if (add && vjo_streak_add(&s, today))
        vjo_file_write(path, &s, sizeof(s));
    uint32_t shown = vjo_streak_current(&s, today) | (uint32_t)s.longest << 16;
    vjo_view_lock();
    if (add || seq == total.streak_seq) { /* a view's read, made while the writer wrote, never undoes the writer's */
        /* drawn again only when it changed, or at its first store (an open overlay's redraw resets its scroll) */
        g_view.version += total.streak != shown || !total.streak_seq;
        total.streak = shown;
        total.streak_seq++;
    }
    vjo_view_unlock();
}

void vjo_history_streak_text(char *buf, size_t cap)
{
    uint32_t current = total.streak & 0xFFFFu;
    *buf = '\0'; /* not read yet (the view's first frames after a press since the Shell started): no line, not 0 */
    if (total.streak_seq)
        sceClibSnprintf(buf, cap, "\n\nStreak: current %u day%s · longest %u", (unsigned)current, current == 1 ? "" : "s",
                    (unsigned)(total.streak >> 16));
}

static void total_save(uint64_t chars, uint32_t covered, const VjoReadingTime *time, const VjoReadingScope *scope)
{
    TotalFile f = {chars, covered, 0, *time, *scope};
    char path[80];
    f.check = total_file_check(&f);
    if (vjo_file_write(cache_path(path, sizeof(path), total.scope.uuid), &f, sizeof(f)) >= 0) {
        total.saved = covered;
        total.saved_us = time->last_us;
    }
}

/* The count's end, or its failure: the state the view shows, which it draws again */
static void total_set(int state)
{
    vjo_view_lock();
    total.state = state;
    g_view.version++;
    vjo_view_unlock();
}

void vjo_history_total_want(void)
{
    int ask;
    vjo_view_lock();
    total.requested = 1;
    ask = total.state == TOTAL_UNREAD || total.state == TOTAL_FAILED;
    if (ask)
        total.state = TOTAL_COUNTING;
    vjo_view_unlock();
    vjoNotifyShell();
}

const char *vjo_history_total_text(char *buf, size_t cap)
{
    static const char counting[] VJO_DATA_TABLE(history_total_counting) = "counting…";
    static const char unavailable[] VJO_DATA_TABLE(history_total_unavailable) = "unavailable";
    if (total.state != TOTAL_KNOWN)
        return total.state == TOTAL_FAILED ? unavailable : counting;
    return vjo_history_number(buf, cap, total.chars);
}

void vjo_history_total_summary(char *buf, size_t cap)
{
    static const char format[] VJO_DATA_TABLE(history_summary_format) =
        "Total characters read: %s\nCharacters/hour: %s\nTotal time played: %llu:%02u:%02u (active)\n";
    static const char pending[] VJO_DATA_TABLE(history_summary_pending) = "Reading statistics: %s\n";
    char chars[32], speed[32];
    if (total.state != TOTAL_KNOWN) {
        sceClibSnprintf(buf, cap, pending, vjo_history_total_text(chars, sizeof(chars)));
        return;
    }
    uint64_t seconds = total.time.active_us / 1000000;
    const char *rate = "-";
    if (total.time.active_us)
        rate = vjo_history_number(speed, sizeof(speed),
            total.time.timed_chars * 3600000000ull / total.time.active_us);
    sceClibSnprintf(buf, cap, format, vjo_history_total_text(chars, sizeof(chars)), rate,
        (unsigned long long)(seconds / 3600), (unsigned)(seconds / 60 % 60), (unsigned)(seconds % 60));
}

void vjo_history_stats_block(char *buf, size_t cap, int today)
{
    static const char format[] VJO_DATA_TABLE(history_stats_format) = "%s characters\n%llu h %02u min reading · %s chars/hour";
    static const char minutes[] VJO_DATA_TABLE(history_stats_minutes) = "%s characters\n%u min reading · %s chars/hour";
    char chars[32], speed[32];
    if (total.state != TOTAL_KNOWN) {
        sceClibSnprintf(buf, cap, "%s", vjo_history_total_text(chars, sizeof(chars)));
        return;
    }
    uint64_t active = today ? total.scope.active_us : total.time.active_us;
    uint64_t timed = today ? total.scope.timed_chars : total.time.timed_chars;
    uint64_t mins = active / 60000000;
    static const char no_rate[] VJO_DATA_TABLE(stats_no_rate) = "—";
    const char *rate = active ? vjo_history_number(speed, sizeof(speed), timed * 3600000000ull / active) : no_rate;
    vjo_history_number(chars, sizeof(chars), today ? total.scope.chars : total.chars);
    if (mins < 60) sceClibSnprintf(buf, cap, minutes, chars, (unsigned)mins, rate);
    else sceClibSnprintf(buf, cap, format, chars, (unsigned long long)(mins / 60), (unsigned)(mins % 60), rate);
}

__attribute__((noinline)) int vjo_history_total_step(VjoArena *scratch)
{
    size_t mark = vjo_arena_mark(scratch), n = 0, first, last;
    uint32_t from, to, base, covered;
    uint64_t chars;
    VjoReadingTime time;
    VjoReadingScope scope;
    char *buf;
    int cut, known;
    vjo_view_lock();
    int requested = total.requested;
    total.requested = 0;
    vjo_view_unlock();
    if (requested) {
        VjoReadingScope context = {0};
        vjo_history_streak(0); /* the view's streak, as the file has it now (the Rubi app may have seeded it) */
        if (!vjo_history_context(&context)) { vjo_history_total_stop(); total_set(TOTAL_FAILED); return 0; }
        if (sceClibStrcmp(context.uuid, total.scope.uuid) || context.start_us != total.scope.start_us ||
             context.end_us != total.scope.end_us) {
            vjo_history_total_keep();
            vjo_history_total_stop();
            vjo_view_lock();
            total.scope = context;
            total.chars = total.covered = total.saved = 0;
            sceClibMemset(&total.time, 0, sizeof(total.time));
            total.read = 0;
            total.state = TOTAL_COUNTING;
            vjo_view_unlock();
        }
    }
    if (total.state != TOTAL_COUNTING) {
        /* Checkpoint in the control thread, at most once per minute of recorded time.
         * A dialogue append itself never adds a cache write. */
        vjo_view_lock();
        int checkpoint = total.state == TOTAL_KNOWN && total.covered != total.saved &&
            total.time.last_us > total.saved_us && total.time.last_us - total.saved_us >= 60000000;
        vjo_view_unlock();
        if (checkpoint) vjo_history_total_keep();
        return 0;
    }
    if (!total.read) { /* the first count since the Shell started: from the total history_total.bin keeps */
        size_t len = 0;
        char path[80];
        const char *bytes = vjo_file_read(scratch, cache_path(path, sizeof(path), total.scope.uuid), &len);
        TotalFile f;
        if (bytes && len == sizeof(f)) {
            sceClibMemcpy(&f, bytes, sizeof(f)); /* (the scratch's alignment is its array's) */
            if (f.check == total_file_check(&f) && !sceClibStrcmp(f.scope.uuid, total.scope.uuid) &&
                f.scope.start_us == total.scope.start_us && f.scope.end_us == total.scope.end_us) {
                total.scope = f.scope;
                total.time = f.time;
                total.chars = f.chars;
                total.covered = total.saved = f.covered;
                total.saved_us = f.time.last_us;
            }
        }
        vjo_arena_release(scratch, mark);
        total.read = 1;
    }
    if (!total.fd) { /* a count's start: history.csv's end, read while no record is being appended */
        uint32_t seq = total.seq;
        SceOff top;
        __sync_synchronize();
        total.fd = sceIoOpen(HISTORY_PATH, SCE_O_RDONLY, 0);
        top = total.fd >= 0 ? sceIoLseek(total.fd, 0, SCE_SEEK_END) : total.fd == (SceUID)0x80010002 ? 0 : -1;
        __sync_synchronize();
        total.clean = !(seq & 1) && total.seq == seq;
        total.top_seq = seq;
        if ((uint64_t)top >> 32) /* an error, or 4 GiB or more */
            goto failed;
        total.top = (uint32_t)top;
        if (total.covered > total.top) { /* a shorter file than the total is of: another one */
            total.chars = 0, total.covered = 0;
            total.scope.chars = total.scope.active_us = total.scope.timed_chars = 0;
            sceClibMemset(&total.time, 0, sizeof(total.time));
        }
        total.scan_scope = total.scope;
        total.scan_time = total.time;
        total.at = total.covered;
        total.found = 0;
#if VJO_DEBUG
        total.windows = total.bytes = 0;
        total.started = sceKernelGetProcessTimeWide();
#endif
    }
    from = total.at;
    to = total.top - from > VJO_HISTORY_WINDOW ? from + VJO_HISTORY_WINDOW : total.top;
    base = from ? from - 1 : 0; /* the window and the byte before it */
    buf = vjo_arena_alloc(scratch, VJO_HISTORY_WINDOW + 1);
    if (!buf || (to > base && sceIoLseek(total.fd, base, SCE_SEEK_SET) != base))
        goto failed;
    while (n < to - base) {
        int r = sceIoRead(total.fd, buf + n, to - base - n);
        if (r <= 0)
            goto failed;
        n += (size_t)r;
    }
    /* the newest record of a window that the file's end does not end, of one read while a record was
     * appended, or of a file that ends in no line break (a record a power loss cut: the next boot's
     * record runs on from it, and a count of the two takes the cut one only, as a fresh count does), is
     * left for the next window or count */
    cut = to < total.top || !total.clean || (n && buf[n - 1] != '\n');
    total.found += vjo_history_count_scope(buf, n, !from, cut, &first, &last, &total.scan_time, &total.scan_scope);
#if VJO_DEBUG
    total.windows++;
    total.bytes += (uint32_t)n;
#endif
    vjo_arena_release(scratch, mark);
    if (from && from == total.covered && from < total.top && first != 1) {
        /* no record starts where the total's bytes end: history.csv is not the file it is the total of */
        total.chars = total.found = 0;
        total.scope.chars = total.scope.active_us = total.scope.timed_chars = 0;
        total.covered = total.at = 0;
        sceClibMemset(&total.time, 0, sizeof(total.time));
        total.scan_scope = total.scope;
        total.scan_time = total.time;
        return 1;
    }
    if (to < total.top) {
        total.at = last < n && base + last > from ? base + (uint32_t)last : to;
        return 1;
    }
    if (total.fd > 0)
        sceIoClose(total.fd);
    total.fd = 0;
    vjo_view_lock();
    chars = total.chars += total.found;
    scope = total.scope = total.scan_scope;
    time = total.time = total.scan_time;
    /* a cut window's total ends where its newest record starts, or, with none, where it starts */
    covered = total.covered = !cut ? to : last < n ? base + (uint32_t)last : from;
    /* an append since the end was read: another count, from there, at the next wakeup (the append is
     * done by then: no count waits for it here) */
    known = total.clean && total.seq == total.top_seq;
    if (known)
        total.state = TOTAL_KNOWN;
    g_view.version++;
    vjo_view_unlock();
#if VJO_DEBUG
    vjo_log("history total: %llu characters in %u B of history.csv (%u B read in %u windows, %u ms)%s",
            (unsigned long long)chars, (unsigned)covered, (unsigned)total.bytes, (unsigned)total.windows,
            (unsigned)((sceKernelGetProcessTimeWide() - total.started) / 1000), known ? "" : "; again");
#endif
    if (known && covered != total.saved)
        total_save(chars, covered, &time, &scope);
    return 0;
failed:
    vjo_arena_release(scratch, mark);
    if (total.fd > 0)
        sceIoClose(total.fd);
    total.fd = 0;
    total_set(TOTAL_FAILED);
    return 0;
}

void vjo_history_total_keep(void)
{
    uint64_t chars;
    uint32_t covered;
    VjoReadingTime time;
    VjoReadingScope scope;
    int keep;
    vjo_view_lock();
    chars = total.chars;
    time = total.time;
    scope = total.scope;
    covered = total.covered;
    keep = total.state == TOTAL_KNOWN && covered != total.saved;
    vjo_view_unlock();
    if (keep)
        total_save(chars, covered, &time, &scope);
}

void vjo_history_total_stop(void)
{
    if (total.fd > 0) /* a count in progress keeps history.csv open between its windows */
        sceIoClose(total.fd);
    total.fd = 0;
}
