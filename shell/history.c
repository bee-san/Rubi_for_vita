#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>

#include "history.h"
#include "shell.h"
#include "../core/port.h"
#include "../core/textnorm.h"

#define HISTORY_PATH VJO_DATA_DIR "/history.csv"

/* Only fingerprints survive a call. No text or file-size-dependent memory. */
static uint64_t previous_text;
static uint32_t previous_game, previous_length;

typedef struct {
    SceUID fd;
    int error;
    unsigned used;
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
    sceClibSnprintf(text, sizeof(text), ",%llu", (unsigned long long)n);
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
        "capture_bytes,strip_rows,workspace_bytes,resident_bytes,fallback,line_metrics\n";
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
    uint64_t hash = 14695981039346656037ull;
    uint32_t game = 2166136261u;
    size_t len = 0;
    int visible = 0, rc, closed;
    SceOff start;
    HistoryWriter w = {.fd = -1};
    char text[72];
    if (!r || !(o = r->result) || o->rc || !r->time_us || !r->title_id[0] ||
        sceClibStrnlen(r->title_id, sizeof(r->title_id)) == sizeof(r->title_id)) return -1;
    while (len < sizeof(o->text) && o->text[len]) {
        unsigned char c = (unsigned char)o->text[len++];
        hash = (hash ^ c) * 1099511628211ull;
        visible |= c > ' ';
    }
    if (len == sizeof(o->text)) return -1;
    if (!visible) { vjo_history_reset(); return 0; }
    for (size_t i = 0; i < sizeof(r->title_id) && r->title_id[i]; i++)
        game = (game ^ (unsigned char)r->title_id[i]) * 16777619u;
    if (r->platform) for (const char *p = r->platform; *p; p++)
        game = (game ^ (unsigned char)*p) * 16777619u;
    if (previous_text == hash && previous_length == len && previous_game == game) return 0;
    w.fd = sceIoOpen(HISTORY_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (w.fd < 0) return w.fd;
    start = sceIoLseek(w.fd, 0, SCE_SEEK_END);
    if (start < 0) { sceIoClose(w.fd); return (int)start; }
    if (!start) literal(&w, header);
    put(&w, '"');
    literal(&w, r->platform ? r->platform : "vita");
    put(&w, '-');
    literal(&w, r->title_id);
    literal(&w, "\",\"");
    sceClibSnprintf(text, sizeof(text), "vjo-%llu-%llu", (unsigned long long)r->time_us,
                    (unsigned long long)start);
    literal(&w, text);
    literal(&w, "\",");
    field(&w, r->name[0] ? r->name : r->title_id, r->name[0] ? sizeof(r->name) : sizeof(r->title_id));
    put(&w, ',');
    field(&w, o->text, len);
    sceClibSnprintf(text, sizeof(text), ",%llu.%06u,", (unsigned long long)(r->time_us / 1000000ull),
                    (unsigned)(r->time_us % 1000000ull));
    literal(&w, text);
    field(&w, r->platform, 16);
    put(&w, ','); field(&w, r->title_id, sizeof(r->title_id));
    literal(&w, ",\"vocr\","); field(&w, r->model, VJO_GAME_OCR_MODEL_BYTES);
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
        sceClibSnprintf(text, sizeof(text), "%s%u:%u:%u:%u:%u", i ? "|" : "",
                        line->width, line->height, line->frames, line->flags, (unsigned)line->us);
        literal(&w, text);
    }
    put(&w, '"');
    put(&w, '\n');
    flush(&w);
    rc = w.error;
    if (!rc) rc = sceIoSyncByFd(w.fd, 0);
    closed = sceIoClose(w.fd);
    if (!rc && closed < 0) rc = closed;
    if (rc < 0) {
        SceIoStat st;
        sceClibMemset(&st, 0, sizeof(st));
        st.st_size = start;
        sceIoChstat(HISTORY_PATH, &st, SCE_CST_SIZE);
        return rc;
    }
    previous_text = hash;
    previous_game = game;
    previous_length = (uint32_t)len;
    return 1;
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
