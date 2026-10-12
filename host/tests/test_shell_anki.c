/* Actual Shell AnkiConnect sender (shell/anki.c) and its screenshot path
 * (shell/capture.c, the real JPEG encoder over a synthetic 960x544 game
 * frame). Only Vita threads, locks, the Paf heap, the kernel capture and
 * sockets are substituted. A started card thread runs to completion inside
 * sceKernelStartThread (or later, with defer_start), as the real thread
 * does before the control thread joins it. Lines starting "measure:" are
 * the arena peaks quoted in docs/anki.md. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "acutest.h"
#include "anki.h"
#include "base64.h"
#include "replay.h"
#define VJO_TEST_MUTEX_STUBS 1
#define VJO_TEST_THREAD_STUBS 1
#define VJO_TEST_CREATE_STUBS 1
#include <psp2/host_stubs.h>
#include <psp2/rtc.h>
#include "../../shell/shell.h"

/* ---------------- kernel capture of the game frame ---------------- */

#define FRAME_W 960
#define FRAME_H 544
static uint8_t frame[FRAME_W * FRAME_H * 4];
static VjoState cap_state;
static int capture_rc; /* < 0: vjoRequestCapture fails with it */
static unsigned captures, rows_read, discards;
static uint32_t capture_flags;
static uint64_t test_now;

uint64_t sceKernelGetProcessTimeWide(void) { return test_now += 1000; }
int sceKernelSetEventFlag(SceUID uid, unsigned bits) { return 0; }
int vjoGetState(VjoState *out) { *out = cap_state; return 0; }
int vjoRequestCapture(uint32_t flags)
{
    if (flags == VJO_CAPTURE_DISCARD) {
        discards++;
        return 0;
    }
    capture_flags = flags;
    if (capture_rc < 0)
        return capture_rc;
    captures++;
    cap_state.size = sizeof(cap_state);
    cap_state.done_seq = captures;
    cap_state.width = FRAME_W;
    cap_state.height = FRAME_H;
    cap_state.raw_stride = FRAME_W * 4;
    cap_state.capture_result = 0;
    return (int)captures;
}
int vjoWaitEvent(uint32_t mask, uint32_t *bits, uint32_t timeout) { *bits = 0; return 0; }
int vjoReadRaw(uint32_t row, uint32_t n, void *dst)
{
    if (row >= FRAME_H)
        return -1;
    if (n > FRAME_H - row)
        n = FRAME_H - row;
    memcpy(dst, frame + (size_t)row * FRAME_W * 4, (size_t)n * FRAME_W * 4);
    rows_read += n;
    return (int)n;
}
void vjo_log(const char *fmt, ...) {}

/* capture.c and anki.c both have a static now_us. */
#define now_us capture_now_us
#include "../../shell/capture.c"
#undef now_us

/* ---------------- threads, locks, view ---------------- */

static int held_box, held_view, mutex_deletes, fail_mutex;
static int threads_created, threads_started, threads_joined, threads_deleted;
static int fail_create, fail_start, fail_wait, defer_start;
static int (*thread_entry)(SceSize, void *);
VjoView g_view;

SceUID sceKernelCreateMutex(const char *name, unsigned int attr, int count, void *opt)
{
    if (fail_mutex)
        return -1;
    return !strcmp(name, "VjoAnkiBox") ? 91 : !strcmp(name, "VjoNetCancel") ? 92 : 93;
}
int sceKernelDeleteMutex(SceUID uid) { mutex_deletes++; return 0; }
/* Never the view inside the mailbox or the reverse: a lock order that the
 * paf thread (view) and the card (both) could deadlock on. */
int sceKernelLockMutex(SceUID uid, int count, void *timeout)
{
    if (uid == 91) { TEST_CHECK(!held_box && !held_view); held_box++; }
    if (uid == 81) { TEST_CHECK(!held_view && !held_box); held_view++; }
    return 0;
}
int sceKernelUnlockMutex(SceUID uid, int count)
{
    if (uid == 91) { TEST_CHECK(held_box == 1); held_box--; }
    if (uid == 81) { TEST_CHECK(held_view == 1); held_view--; }
    return 0;
}
void vjo_view_lock(void) { sceKernelLockMutex(81, 1, NULL); }
void vjo_view_unlock(void) { sceKernelUnlockMutex(81, 1); }

SceUID sceKernelCreateThread(const char *name, int (*entry)(SceSize, void *), int priority, unsigned int stack,
                             unsigned int attr, int affinity, void *opt)
{
    TEST_CHECK(!strcmp(name, "VjoAnki") && stack == 0x10000);
    if (fail_create)
        return -1;
    thread_entry = entry;
    return 70 + ++threads_created;
}
int sceKernelStartThread(SceUID uid, SceSize size, void *args)
{
    if (fail_start)
        return -1;
    threads_started++;
    if (!defer_start)
        thread_entry(0, NULL);
    return 0;
}
int sceKernelWaitThreadEnd(SceUID uid, int *status, void *timeout)
{
    if (fail_wait)
        return -1;
    threads_joined++;
    return 0;
}
int sceKernelDeleteThread(SceUID uid) { threads_deleted++; return 0; }
int sceRtcGetCurrentTick(SceRtcTick *t) { t->tick = 62135596800000000ull + 1791158400007000ull; return 0; }
static int clock_fails;
int sceRtcGetCurrentClockLocalTime(SceDateTime *t)
{
    if (clock_fails)
        return -1;
    *t = (SceDateTime){2026, 10, 12, 9, 30, 5, 0};
    return 0;
}

/* ---------------- Paf heap ---------------- */

static uint8_t heap[512 * 1024] __attribute__((aligned(16)));
static int heap_live, heap_attempts, heap_allocs, heap_frees, fail_heap;
static size_t heap_last;
void *vjo_shell_heap_alloc_for(const char *what, size_t bytes)
{
    TEST_CHECK(!strcmp(what, "Anki") && !heap_live);
    heap_attempts++;
    heap_last = bytes;
    if (fail_heap || bytes > sizeof(heap))
        return NULL;
    heap_allocs++;
    heap_live = 1;
    memset(heap, 0xA5, bytes); /* reused memory is never zeroed */
    return heap;
}
void vjo_shell_heap_free(void *p)
{
    TEST_CHECK(p == heap && heap_live);
    heap_live = 0;
    heap_frees++;
}

/* ---------------- files, LAN and sockets ---------------- */

static char host_file[128];
static int host_file_writes;
char *vjo_file_read(VjoArena *a, const char *path, size_t *len)
{
    TEST_CHECK(!strcmp(path, VJO_ANKI_HOST_PATH));
    if (!host_file[0])
        return NULL;
    *len = strlen(host_file);
    return vjo_arena_strndup(a, host_file, *len);
}
int vjo_file_write(const char *path, const void *data, size_t len)
{
    TEST_CHECK(!strcmp(path, VJO_ANKI_HOST_PATH) && len < sizeof(host_file));
    memcpy(host_file, data, len);
    host_file[len] = '\0';
    host_file_writes++;
    return 0;
}

static unsigned probe_batches, cancels;
static uint32_t anki_ip = 0xC0A80117u; /* 192.168.1.23 answers on 8765 */
int vjo_net_local_ipv4(uint32_t *ip, uint32_t *mask)
{
    *ip = 0xC0A8011Eu; /* 192.168.1.30/24 */
    *mask = 0xFFFFFF00u;
    return 0;
}
int vjo_net_probe_port(const uint32_t *ips, int n, int port, int window_us, uint8_t *open)
{
    if (n > VJO_NET_PROBE_MAX)
        n = VJO_NET_PROBE_MAX;
    probe_batches++;
    for (int i = 0; i < n; i++)
        open[i] = ips[i] == anki_ip && port == VJO_ANKI_PORT;
    return n;
}
int vjo_net_cancel_init(VjoNetCancel *c)
{
    c->fd = -1;
    c->cancelled = 0;
    c->lock = sceKernelCreateMutex("VjoNetCancel", 0, 0, NULL);
    return c->lock < 0 ? -1 : 0;
}
void vjo_net_cancel(VjoNetCancel *c) { c->cancelled = 1; cancels++; }
void vjo_net_cancel_clear(VjoNetCancel *c) { c->cancelled = 0; }

/* AnkiConnect (and an audio source) on the LAN: canned HTTP responses in
 * turn, NULL = nothing listening; what each connection sent is kept. */
#define MAX_CONNS 6
static struct {
    const char *responses[MAX_CONNS];
    int n, next, stop_at; /* stop_at: connection on which vjo_anki_stop begins */
    VjoMemConn conn;
    char host[MAX_CONNS][64];
    int port[MAX_CONNS];
} net;
static char sent[MAX_CONNS][640 * 1024];

static void simulate_stop_begins(void);
static int fake_connect(void *ud, const char *host, int port, int timeout_us, int io_timeout_us, VjoConn *out)
{
    VjoNetCancel *c = (VjoNetCancel *)ud;
    int k = net.next;
    if (k == net.stop_at)
        simulate_stop_begins();
    if (c && c->cancelled)
        return VJO_E_CANCELLED;
    if (k >= net.n)
        return VJO_E_NET;
    net.next++; /* a refused connection takes its turn too */
    if (!net.responses[k])
        return VJO_E_NET;
    snprintf(net.host[k], sizeof(net.host[k]), "%s", host);
    net.port[k] = port;
    memset(&net.conn, 0, sizeof(net.conn));
    net.conn.in = net.responses[k];
    net.conn.len = strlen(net.responses[k]);
    net.conn.out = sent[k];
    net.conn.out_cap = sizeof(sent[k]) - 1;
    vjo_memconn_init(&net.conn, out);
    return VJO_OK;
}
static void fake_disconnect(void *ud, VjoConn *c) { sent[net.next - 1][net.conn.out_len] = '\0'; }
void vjo_platform_vita(VjoPlatform *p, VjoNetCancel *cancel)
{
    memset(p, 0, sizeof(*p));
    p->ud = cancel;
    p->connect = fake_connect;
    p->disconnect = fake_disconnect;
}

#include "../../shell/anki.c"

/* What vjo_anki_stop does on another thread before it joins the card. */
static void simulate_stop_begins(void)
{
    sceKernelLockMutex(box_lock, 1, NULL);
    started = 0;
    sceKernelUnlockMutex(box_lock, 1);
    vjo_net_cancel(&cancel);
}

/* ---------------- fixtures ---------------- */

static const char *ok(const char *json)
{
    static char bufs[MAX_CONNS][1024];
    static int k;
    char *b = bufs[k++ % MAX_CONNS];
    snprintf(b, sizeof(bufs[0]), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %lu\r\n\r\n%s",
             (unsigned long)strlen(json), json);
    return b;
}
#define NOTE_ID "{\"result\": 1700000000001, \"error\": null}"

static void lan(int n, ...)
{
    va_list ap;
    memset(&net, 0, sizeof(net));
    net.stop_at = -1;
    va_start(ap, n);
    for (int i = 0; i < n; i++)
        net.responses[i] = va_arg(ap, const char *);
    va_end(ap);
    net.n = n;
    for (int i = 0; i < MAX_CONNS; i++)
        sent[i][0] = '\0';
}

/* A shown local lookup: "猫が好き" with 猫 and 好き looked up. */
static const char *neko_meanings[] = {"cat", "shamisen \"neko\""};
static const char *suki_meanings[] = {"liked", "favourite"};
static VjoVocab neko = {.spelling = "猫", .reading = "ねこ", .rank = 1500, .meanings = neko_meanings, .n_meanings = 2};
static VjoVocab suki = {.spelling = "好き", .reading = "すき", .rank = VJO_NO_RANK, .meanings = suki_meanings,
                        .n_meanings = 2};
static VjoEntry entries[3];
static VjoEntryList list;
static VjoConfig cfg;

/* A dialogue-box game frame: smooth background, a dark box with rows of
 * glyph-like strokes. noise = 1: every pixel random (worst case). */
static void make_frame(int noise)
{
    uint32_t r = 12345;
    for (int y = 0; y < FRAME_H; y++) {
        for (int x = 0; x < FRAME_W; x++) {
            uint8_t *p = frame + ((size_t)y * FRAME_W + x) * 4;
            r = r * 1103515245u + 12345u;
            if (noise) {
                p[0] = (uint8_t)(r >> 24);
                p[1] = (uint8_t)(r >> 16);
                p[2] = (uint8_t)(r >> 8);
            } else if (y > 380 && y < 530 && x > 20 && x < 940) {
                int gx = (x - 40) % 26, gy = (y - 400) % 34, glyph = ((x - 40) / 26 * 7 + (y - 400) / 34 * 13) % 5;
                int ink = y >= 400 && gx < 22 && gy < 24 && glyph && ((gx * glyph + gy) % 6 < 2 || gy % 9 == glyph);
                p[0] = p[1] = p[2] = ink ? 240 : 24;
            } else {
                p[0] = (uint8_t)(90 + x / 8);
                p[1] = (uint8_t)(60 + y / 5);
                p[2] = (uint8_t)(140 + (x + y) / 16);
            }
            p[3] = 255;
        }
    }
}

static void reset(void)
{
    memset(&g_view, 0, sizeof(g_view));
    memset(&cap_state, 0, sizeof(cap_state));
    capture_rc = 0;
    captures = rows_read = discards = 0;
    held_box = held_view = mutex_deletes = fail_mutex = 0;
    threads_created = threads_started = threads_joined = threads_deleted = 0;
    fail_create = fail_start = fail_wait = defer_start = 0;
    heap_live = heap_attempts = heap_allocs = heap_frees = fail_heap = 0;
    host_file[0] = '\0';
    host_file_writes = 0;
    probe_batches = cancels = 0;
    saved_loaded = scanned = 0;
    saved_host[0] = '\0';
    lan(0);
    /* the Shell starts the mailbox before its first config load */
    TEST_ASSERT(vjo_anki_start() == 0);
    TEST_ASSERT(capture_lock < 0 ? vjo_capture_init() == 0 : 1);
    entries[0] = (VjoEntry){.vocab = &neko, .pos16 = 0, .hl_start = 0, .hl_end = 3, .text = "猫 (ねこ)"};
    entries[1] = (VjoEntry){.vocab = &suki, .pos16 = 2, .hl_start = 6, .hl_end = 12, .text = "好き (すき)"};
    entries[2] = (VjoEntry){.vocab = &neko, .pos16 = 4, .hl_start = 12, .hl_end = 15, .text = "猫 (ねこ)"};
    list = (VjoEntryList){.header = "猫が好き猫", .entries = entries, .n_entries = 3};
    g_view.open = 1;
    g_view.list = &list;
    g_view.list_seq = 5;
    vjo_config_defaults(&cfg);
    snprintf(cfg.anki_host, sizeof(cfg.anki_host), "192.168.1.23");
    snprintf(cfg.anki_deck, sizeof(cfg.anki_deck), "Japanese::Mining");
    TEST_ASSERT(vjo_anki_configure(&cfg) == 1);
    vjo_anki_set_game("CLANNAD");
    clock_fails = 0;
    make_frame(0);
}

/* Ends the test's mailbox; every card's memory and thread are gone. */
static void finish(void)
{
    vjo_anki_reap();
    TEST_CHECK(vjo_anki_stop() == 0);
    TEST_CHECK(!heap_live && heap_allocs == heap_frees);
    TEST_CHECK(threads_created == threads_deleted);
    TEST_CHECK(!held_box && !held_view);
}

static const char *body_of(int k)
{
    const char *b = strstr(sent[k], "\r\n\r\n");
    return b ? b + 4 : "";
}

/* ---------------- tests ---------------- */

static void test_off_by_default_allocates_nothing(void)
{
    VjoConfig d;
    reset();
    vjo_config_defaults(&d);
    TEST_CHECK(!d.anki_host[0]);
    TEST_CHECK(vjo_anki_configure(&d) == 0);
    lan(1, ok(NOTE_ID));
    vjo_anki_post_add(5, 0);
    TEST_CHECK(threads_created == 0 && heap_attempts == 0 && captures == 0 && net.next == 0);
    TEST_CHECK(!g_view.anki_status[0] && g_view.anki_version == 0);
    finish();
}

static void test_cross_sends_note_with_screenshot(void)
{
    static const char post[] = "POST / HTTP/1.1\r\nHost: 192.168.1.23:8765\r\nContent-Type: application/json\r\n";
    static const char note[] =
        "{\"action\":\"addNote\",\"version\":6,\"params\":{\"note\":{\"deckName\":\"Japanese::Mining\","
        "\"modelName\":\"Lapis\",\"fields\":{\"Expression\":\"猫\",\"ExpressionReading\":\"ねこ\","
        "\"ExpressionFurigana\":\"猫[ねこ]\",\"MainDefinition\":\"<ol><li>cat</li><li>shamisen "
        "&quot;neko&quot;</li></ol>\",\"Sentence\":\"<b>猫</b>が好き猫\",\"Picture\":\"\",\"FreqSort\":\"1500\","
        "\"SentenceFurigana\":\"<b>猫</b>が好き猫\",\"MiscInfo\":\"CLANNAD (2026-10-12 09:30)\"},"
        "\"tags\":[\"vita-jp-overlay\"]";
    char name[96], cl[64], soi[8] = {0};
    const char *body, *data, *end;
    reset();
    lan(1, ok(NOTE_ID));
    vjo_anki_post_add(5, 0);
    TEST_CHECK(threads_created == 1 && threads_started == 1);
    /* one addNote to anki_host's AnkiConnect, plain HTTP on the LAN */
    TEST_CHECK(net.next == 1 && !strcmp(net.host[0], "192.168.1.23") && net.port[0] == VJO_ANKI_PORT);
    TEST_CHECK(!strncmp(sent[0], post, strlen(post)));
    body = body_of(0);
    snprintf(cl, sizeof(cl), "Content-Length: %lu\r\n", (unsigned long)strlen(body));
    TEST_CHECK(strstr(sent[0], cl) != NULL);
    TEST_CHECK(!strncmp(body, note, strlen(note)));
    TEST_MSG("%.400s", body);
    /* the whole game frame (one full single-pass capture, every row read
     * once), streamed as base64: a JPEG within the bound */
    TEST_CHECK(captures == 1 && capture_flags == (VJO_CAPTURE_FULL | VJO_CAPTURE_ONCE) && rows_read == FRAME_H);
    snprintf(name, sizeof(name), "\"picture\":[{\"filename\":\"vitajp_1791158400007.jpg\",\"fields\":[\"Picture\"]");
    TEST_CHECK(strstr(body, name) != NULL);
    data = strstr(body, "\"data\":\"");
    TEST_ASSERT(data != NULL);
    data += 8;
    end = strchr(data, '"');
    TEST_ASSERT(end && !strcmp(end, "\"}]}}}"));
    TEST_CHECK((size_t)(end - data) % 4 == 0 && (size_t)(end - data) <= vjo_base64_len(VJO_ANKI_PICTURE_MAX));
    vjo_base64_encode((const uint8_t *)"\xFF\xD8\xFF", 3, soi);
    TEST_CHECK(!strncmp(data, soi, 4)); /* starts with the JPEG SOI marker */
    /* confirmation and the √ for every occurrence of the word */
    TEST_CHECK(!strcmp(g_view.anki_status, "Added to Anki") && g_view.anki_status_seq == 5 &&
               g_view.anki_status_kind == VJO_ANKI_STATUS_DIM);
    TEST_CHECK(g_view.anki_marks_seq == 5 && g_view.anki_mark[0] && !g_view.anki_mark[1] && g_view.anki_mark[2]);
    /* bounded, freed on completion, recognized text untouched */
    TEST_CHECK(heap_allocs == 1 && heap_frees == 1 && !heap_live && heap_last == ANKI_PICTURE_MEM);
    TEST_CHECK(g_view.list == &list && !strcmp(list.header, "猫が好き猫"));
    printf("measure: picture card: arena %u bytes, peak %u, JPEG %u bytes, request %u bytes\n",
           (unsigned)heap_last, (unsigned)last_peak, (unsigned)((end - data) / 4 * 3),
           (unsigned)strlen(sent[0]));
    TEST_CHECK(last_peak > 0 && last_peak <= ANKI_PICTURE_MEM);
    /* the control thread joins the finished card: no thread stays */
    TEST_CHECK(threads_deleted == 0);
    vjo_anki_reap();
    TEST_CHECK(threads_joined == 1 && threads_deleted == 1);
    vjo_anki_reap();
    TEST_CHECK(threads_joined == 1);
    finish();
}

static void test_mapping_port_audio_and_text_arena(void)
{
    static const char get[] = "GET /?term=%E5%A5%BD%E3%81%8D&reading=%E3%81%99%E3%81%8D HTTP/1.1\r\n";
    const char *body;
    reset();
    snprintf(cfg.anki_host, sizeof(cfg.anki_host), "my-pc.local:9000");
    snprintf(cfg.anki_note_type, sizeof(cfg.anki_note_type), "Japanese (recognition)");
    snprintf(cfg.anki_tags, sizeof(cfg.anki_tags), "vita  vn");
    snprintf(cfg.anki_field[VJO_ANKI_WORD], sizeof(cfg.anki_field[0]), "Front");
    snprintf(cfg.anki_field[VJO_ANKI_DEFINITION], sizeof(cfg.anki_field[0]), "Back");
    cfg.anki_field[VJO_ANKI_READING][0] = cfg.anki_field[VJO_ANKI_FURIGANA][0] = '\0';
    cfg.anki_field[VJO_ANKI_SENTENCE][0] = cfg.anki_field[VJO_ANKI_FREQUENCY][0] = '\0';
    cfg.anki_field[VJO_ANKI_PICTURE][0] = cfg.anki_field[VJO_ANKI_SENTENCE_FURIGANA][0] = '\0';
    cfg.anki_field[VJO_ANKI_PITCH_POSITION][0] = cfg.anki_field[VJO_ANKI_PITCH_CATEGORIES][0] = '\0';
    cfg.anki_field[VJO_ANKI_FREQUENCY_LIST][0] = '\0';
    snprintf(cfg.anki_field[VJO_ANKI_MISC_INFO], sizeof(cfg.anki_field[0]), "Source");
    clock_fails = 1; /* MiscInfo without a date rather than a wrong one */
    snprintf(cfg.anki_audio_url, sizeof(cfg.anki_audio_url), "http://192.168.1.23:5050/?term={term}&reading={reading}");
    TEST_ASSERT(vjo_anki_configure(&cfg) == 1);
    lan(2, ok("{\"type\":\"audioSourceList\",\"audioSources\":[{\"name\":\"a\",\"url\":\"http://192.168.1.23:5050/su.opus\"}]}"),
        ok(NOTE_ID));
    vjo_anki_post_add(5, 1);
    /* the word's audio source, then AnkiConnect on its port */
    TEST_CHECK(net.next == 2 && net.port[0] == 5050 && net.port[1] == 9000 && !strcmp(net.host[1], "my-pc.local"));
    TEST_CHECK(!strncmp(sent[0], get, strlen(get)));
    TEST_MSG("%s", sent[0]);
    body = body_of(1);
    TEST_CHECK(strstr(sent[1], "Host: my-pc.local:9000\r\n") != NULL);
    TEST_CHECK(strstr(body, "\"modelName\":\"Japanese (recognition)\",\"fields\":{\"Front\":\"好き\","
                            "\"Back\":\"<ol><li>liked</li><li>favourite</li></ol>\",\"ExpressionAudio\":\"\","
                            "\"Source\":\"CLANNAD\"},\"tags\":[\"vita\",\"vn\"]") != NULL);
    TEST_CHECK(strstr(body, "\"audio\":[{\"filename\":\"vitajp_1791158400007.opus\",\"fields\":[\"ExpressionAudio\"],"
                            "\"url\":\"http://192.168.1.23:5050/su.opus\"}]") != NULL);
    TEST_MSG("%s", body);
    TEST_CHECK(!strstr(body, "\"picture\"") && captures == 0); /* no picture field: no capture */
    TEST_CHECK(!strcmp(g_view.anki_status, "Added to Anki") && g_view.anki_mark[1] && !g_view.anki_mark[0]);
    TEST_CHECK(heap_last == ANKI_TEXT_MEM && heap_frees == 1);
    printf("measure: text card with audio: arena %u bytes, peak %u\n", (unsigned)heap_last, (unsigned)last_peak);
    finish();
}

/* AnkiConnect's refusals and an unreachable computer: a status line only. */
static void test_failures_leave_the_overlay(void)
{
    static const struct {
        int n;
        const char *a, *b, *c;
        const char *status;
        int kind, mark;
    } cases[] = {
        {0, NULL, NULL, NULL, "Anki offline", VJO_ANKI_STATUS_DIM, 0},
        {1, "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n", NULL, NULL, "Anki refused the request (HTTP 403)",
         VJO_ANKI_STATUS_ERROR, 0},
        {1, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi", NULL, NULL,
         "Anki: unexpected response (-104). Is AnkiConnect running?", VJO_ANKI_STATUS_ERROR, 0},
        {1, "{\"result\": null, \"error\": \"model was not found: Lapis\"}", NULL, NULL,
         "Note type \"Lapis\" not found in Anki (anki_note_type)", VJO_ANKI_STATUS_ERROR, 0},
        {1, "{\"result\": null, \"error\": \"cannot create note because it is a duplicate\"}", NULL, NULL,
         "Already in Anki", VJO_ANKI_STATUS_DIM, 1},
        /* a new deck is created, then the note added */
        {3, "{\"result\": null, \"error\": \"deck was not found: Japanese::Mining\"}", "{\"result\": 1, \"error\": null}",
         NOTE_ID, "Added to Anki", VJO_ANKI_STATUS_DIM, 1},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        reset();
        lan(cases[i].n, cases[i].a && cases[i].a[0] == '{' ? ok(cases[i].a) : cases[i].a,
            cases[i].b ? ok(cases[i].b) : NULL, cases[i].c ? ok(cases[i].c) : NULL);
        g_view.status_is_error = 0;
        snprintf(g_view.status, sizeof(g_view.status), "kept");
        vjo_anki_post_add(5, 0);
        TEST_CHECK(!strcmp(g_view.anki_status, cases[i].status) && g_view.anki_status_kind == cases[i].kind);
        TEST_MSG("case %u: %s", i, g_view.anki_status);
        TEST_CHECK(!!g_view.anki_mark[0] == cases[i].mark);
        TEST_CHECK(g_view.list == &list && !strcmp(g_view.status, "kept") && !heap_live && heap_frees == 1);
        if (i == 5)
            TEST_CHECK(strstr(body_of(1), "{\"action\":\"createDeck\",\"version\":6,\"params\":{\"deck\":\"Japanese::Mining\"}}") &&
                       strstr(body_of(2), "\"action\":\"addNote\""));
        /* × works again after any result */
        TEST_CHECK(!want_add);
        finish();
    }
}

/* Screenshot problems never cost the card. */
static void test_screenshot_failures_still_add(void)
{
    reset();
    capture_rc = VJO_ERR_BUSY; /* the kernel's capture is taken */
    lan(1, ok(NOTE_ID));
    vjo_anki_post_add(5, 0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Added to Anki without a screenshot (the capture failed)"));
    TEST_CHECK(!strstr(body_of(0), "\"picture\"") && net.next == 1 && !heap_live);
    finish();

    /* a frame whose JPEG does not fit: dropped, and the arena still holds the note */
    reset();
    make_frame(1);
    lan(1, ok(NOTE_ID));
    vjo_anki_post_add(5, 0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Added to Anki without a screenshot (the screenshot was too large)"));
    TEST_MSG("%s", g_view.anki_status);
    TEST_CHECK(!strstr(body_of(0), "\"picture\"") && rows_read > 0 && !heap_live);
    printf("measure: noise frame (JPEG over the bound): peak %u of %u\n", (unsigned)last_peak, (unsigned)heap_last);
    TEST_CHECK(last_peak > 0 && last_peak <= ANKI_PICTURE_MEM);
    finish();

    /* a note whose request does not fit beside its screenshot: nothing was
     * sent, so it goes again without the screenshot */
    {
        static char long_meaning[100 * 1024];
        static const char *long_meanings[] = {long_meaning};
        static VjoVocab long_word = {.spelling = "長文", .reading = "ちょうぶん", .rank = VJO_NO_RANK,
                                     .meanings = long_meanings, .n_meanings = 1};
        reset();
        memset(long_meaning, 'y', sizeof(long_meaning) - 1);
        long_meaning[sizeof(long_meaning) - 1] = '\0';
        entries[0].vocab = &long_word;
        lan(1, ok(NOTE_ID));
        vjo_anki_post_add(5, 0);
        TEST_CHECK(!strcmp(g_view.anki_status, "Added to Anki without a screenshot (no memory for both)"));
        TEST_MSG("%s", g_view.anki_status);
        TEST_CHECK(net.next == 1 && captures == 1 && !strstr(body_of(0), "\"picture\"") &&
                   strstr(body_of(0), "yyyyyyyy</li></ol>") && !heap_live);
        printf("measure: 100 KiB definition with a screenshot: peak %u of %u\n", (unsigned)last_peak,
               (unsigned)heap_last);
        entries[0].vocab = &neko;
        finish();
    }
}

/* Out of memory: the recognized text and its lookup stay usable. */
static void test_oom_keeps_recognized_text(void)
{
    static char big[48 * 1024];
    static const char *big_meanings[] = {big};
    static VjoVocab huge = {.spelling = "長文", .reading = "ちょうぶん", .rank = VJO_NO_RANK, .meanings = big_meanings,
                            .n_meanings = 1};

    /* the Paf heap keeps its UI reserve: nothing is captured or sent */
    reset();
    fail_heap = 1;
    lan(1, ok(NOTE_ID));
    snprintf(g_view.status, sizeof(g_view.status), "kept");
    vjo_anki_post_add(5, 0);
    TEST_CHECK(heap_attempts == 1 && heap_allocs == 0 && captures == 0 && net.next == 0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Anki: not enough memory, card not sent. Close the overlay and try again.") &&
               g_view.anki_status_kind == VJO_ANKI_STATUS_ERROR);
    TEST_CHECK(g_view.list == &list && !strcmp(g_view.status, "kept") && !g_view.anki_mark[0] && !want_add);
    fail_heap = 0;
    vjo_anki_post_add(5, 0); /* and a retry works */
    TEST_CHECK(!strcmp(g_view.anki_status, "Added to Anki") && net.next == 1);
    finish();

    /* no thread for the card */
    reset();
    fail_create = 1;
    vjo_anki_post_add(5, 0);
    TEST_CHECK(!strcmp(g_view.anki_status, "Anki: not enough memory, card not sent. Close the overlay and try again."));
    TEST_CHECK(heap_attempts == 0 && !want_add && thread < 0);
    fail_create = 0;
    fail_start = 1;
    vjo_anki_post_add(5, 0);
    TEST_CHECK(threads_created == 1 && threads_deleted == 1 && !want_add && thread < 0 && heap_attempts == 0);
    fail_start = 0;
    finish();

    /* an entry larger than the text arena */
    reset();
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    entries[0].vocab = &huge;
    cfg.anki_field[VJO_ANKI_PICTURE][0] = '\0';
    TEST_ASSERT(vjo_anki_configure(&cfg) == 1);
    lan(1, ok(NOTE_ID));
    vjo_anki_post_add(5, 0);
    TEST_CHECK(net.next == 0 && !heap_live && heap_frees == 1);
    TEST_CHECK(strstr(g_view.anki_status, "not enough memory") != NULL &&
               g_view.anki_status_kind == VJO_ANKI_STATUS_ERROR);
    TEST_MSG("%s", g_view.anki_status);
    TEST_CHECK(g_view.list == &list);
    entries[0].vocab = &neko;
    finish();
}

static void test_cancellation(void)
{
    /* the list changed before the card ran: nothing is copied or sent */
    reset();
    defer_start = 1;
    lan(1, ok(NOTE_ID));
    vjo_anki_post_add(5, 0);
    TEST_CHECK(threads_started == 1 && want_add);
    /* a second × meanwhile is ignored, and says so */
    vjo_anki_post_add(5, 1);
    TEST_CHECK(threads_created == 1 && !strcmp(g_view.anki_status, "Anki: still sending the previous card"));
    g_view.list_seq = 6; /* another word shown */
    thread_entry(0, NULL);
    TEST_CHECK(net.next == 0 && captures == 0 && !heap_live && heap_allocs == 1 && !want_add);
    TEST_CHECK(g_view.anki_status_seq != 6);
    finish();

    /* the overlay closed before the card ran */
    reset();
    defer_start = 1;
    lan(1, ok(NOTE_ID));
    vjo_anki_post_add(5, 0);
    g_view.open = 0;
    thread_entry(0, NULL);
    TEST_CHECK(net.next == 0 && !heap_live);
    finish();

    /* stop while the note is being sent: the request is aborted, nothing is
     * published, the arena is freed and the thread joined */
    reset();
    lan(1, ok(NOTE_ID));
    net.stop_at = 0;
    vjo_anki_post_add(5, 0);
    TEST_CHECK(cancels == 1 && net.next == 0 && !heap_live && heap_frees == 1);
    TEST_CHECK(!strcmp(g_view.anki_status, "Adding to Anki…")); /* no result for a stopped card */
    TEST_CHECK(vjo_anki_stop() == 0 && threads_joined == 1 && threads_deleted == 1 && box_lock < 0);
    vjo_anki_post_add(5, 0); /* after stop: ignored */
    TEST_CHECK(threads_created == 1);

    /* a join that fails keeps the mailbox for a retry */
    reset();
    defer_start = 1;
    vjo_anki_post_add(5, 0);
    fail_wait = 1;
    TEST_CHECK(vjo_anki_stop() < 0 && box_lock >= 0 && threads_deleted == 0);
    fail_wait = 0;
    thread_entry(0, NULL); /* the card sees the stop and ends */
    TEST_CHECK(net.next == 0 && !heap_live);
    TEST_CHECK(vjo_anki_stop() == 0 && box_lock < 0 && threads_deleted == 1);
}

/* anki_host = auto: one LAN search, remembered; searched again only after
 * the remembered computer stops answering. */
static void test_auto_finds_anki_on_the_lan(void)
{
    reset();
    snprintf(cfg.anki_host, sizeof(cfg.anki_host), "auto");
    TEST_ASSERT(vjo_anki_configure(&cfg) == 1);
    lan(2, ok("{\"result\": 6, \"error\": null}"), ok(NOTE_ID));
    vjo_anki_post_add(5, 0);
    TEST_CHECK(probe_batches == 1 && net.next == 2 && !strcmp(net.host[0], "192.168.1.23"));
    TEST_CHECK(strstr(body_of(0), "\"action\":\"version\"") && strstr(body_of(1), "\"action\":\"addNote\""));
    TEST_CHECK(!strcmp(host_file, "192.168.1.23:8765\n") && !strcmp(g_view.anki_status, "Added to Anki"));
    vjo_anki_reap();
    /* the next card goes straight to the saved computer */
    lan(1, ok(NOTE_ID));
    g_view.list_seq = 6;
    vjo_anki_post_add(6, 1);
    TEST_CHECK(probe_batches == 1 && net.next == 1 && !strcmp(net.host[0], "192.168.1.23"));
    vjo_anki_reap();
    /* it moved: the saved one is offline, a new search finds it */
    anki_ip = 0xC0A80140u;
    lan(3, NULL, ok("{\"result\": 6, \"error\": null}"), ok(NOTE_ID));
    g_view.list_seq = 7;
    vjo_anki_post_add(7, 0);
    TEST_CHECK(probe_batches > 1 && !strcmp(host_file, "192.168.1.64:8765\n"));
    TEST_CHECK(!strcmp(g_view.anki_status, "Added to Anki"));
    TEST_MSG("%s / %s", host_file, g_view.anki_status);
    anki_ip = 0xC0A80117u;
    finish();
}

/* Many cards: one allocation and one thread each, all released. */
static void test_repeated_cards_release_everything(void)
{
    size_t peak = 0;
    reset();
    for (int i = 0; i < 50; i++) {
        lan(1, ok(i % 3 ? NOTE_ID : "{\"result\": null, \"error\": \"cannot create note because it is a duplicate\"}"));
        g_view.list_seq = 100 + (unsigned)i;
        vjo_anki_post_add(g_view.list_seq, i % 3);
        TEST_CHECK(net.next == 1 && !heap_live && !want_add);
        if (last_peak > peak)
            peak = last_peak;
        vjo_anki_reap();
    }
    TEST_CHECK(heap_allocs == 50 && heap_frees == 50 && threads_created == 50 && threads_deleted == 50);
    TEST_CHECK(captures == 50 && rows_read == 50u * FRAME_H && discards == 0);
    printf("measure: 50 picture cards: max peak %u of %u\n", (unsigned)peak, (unsigned)ANKI_PICTURE_MEM);
    finish();
}

TEST_LIST = {
    {"anki_off_by_default_starts_no_thread_and_allocates_nothing", test_off_by_default_allocates_nothing},
    {"cross_sends_configured_note_and_bounded_screenshot_then_frees", test_cross_sends_note_with_screenshot},
    {"field_mapping_port_audio_and_text_only_arena", test_mapping_port_audio_and_text_arena},
    {"ankiconnect_failures_only_change_the_anki_status", test_failures_leave_the_overlay},
    {"screenshot_failure_or_oversize_still_adds_the_note", test_screenshot_failures_still_add},
    {"oom_keeps_recognized_text_and_retry_works", test_oom_keeps_recognized_text},
    {"stale_list_close_and_stop_cancel_without_leaks", test_cancellation},
    {"auto_host_searches_lan_once_and_remembers", test_auto_finds_anki_on_the_lan},
    {"fifty_cards_release_memory_and_threads", test_repeated_cards_release_everything},
    {NULL, NULL},
};
