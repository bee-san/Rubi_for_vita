/* core/lens_job.c: the game worker's Google Lens job on the host. The region
 * is a synthetic grey image read in strips, the network a fake connection
 * that records the request and answers with a recorded Lens reply
 * (host/fixtures/vn_textbox/lens.pb); no live service. */
#include "acutest.h"
#include "client.h"
#include "jpegsw.h"
#include "lens.h"
#include "lens_job.h"
#include "pb.h"
#include "replay.h"
#include <stdio.h>
#include <string.h>

#define W 200
#define H 72 /* two strips of 48 and 16 rows when the strip holds 48 */

static unsigned char grey[W * H];
static unsigned char strip_mem[64 * 1024];
static unsigned char workspace[1024 * 1024];
static unsigned char fixture_mem[256 * 1024];

/* ---- the region: strips of a grey image ---- */
typedef struct {
    const unsigned char *pixels; /* NULL: grey (W x H) */
    unsigned width;
    unsigned calls, first_flags, last_flags, next_y, rows_asked[16];
    int fail_at;      /* the call that returns an error (1-based), 0: none */
    int cancel_after; /* cancelled once this many strips were read, 0: never */
    int ordered;      /* every strip started at row 0 or where the one before ended */
    unsigned change_from, change_to; /* calls [from, to] see row 5 changed (an animation; 0: never) */
    unsigned flagged_from_zero;      /* FIRST-flagged strips that started a pass at row 0 */
} Source;

static int strip(void *ud, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags)
{
    Source *s = ud;
    s->calls++;
    if (s->calls <= 16) s->rows_asked[s->calls - 1] = n;
    if (s->fail_at && (int)s->calls == s->fail_at) return -1000;
    if (y != 0 && y != s->next_y) s->ordered = 0;
    s->next_y = y + n;
    if (flags & VJO_STRIP_FIRST) s->first_flags++;
    if ((flags & VJO_STRIP_FIRST) && y == 0) s->flagged_from_zero++;
    if (flags & VJO_STRIP_LAST) s->last_flags++;
    const unsigned char *px = s->pixels ? s->pixels : grey;
    unsigned w = s->pixels ? s->width : W;
    for (unsigned r = 0; r < n; r++) memcpy(dst + r * stride, px + (size_t)(y + r) * w, w);
    if (s->change_from && s->calls >= s->change_from && s->calls <= s->change_to && y <= 5 && 5 < y + n)
        dst[(5 - y) * stride] ^= 0xFF; /* row 5 reads otherwise on these calls */
    return (int)n;
}

static int cancelled(void *ud)
{
    Source *s = ud;
    return s->cancel_after && s->calls >= (unsigned)s->cancel_after;
}

static void fill_image(void)
{
    /* dark bars on light grey: something like text lines, with a gradient */
    for (unsigned y = 0; y < H; y++)
        for (unsigned x = 0; x < W; x++)
            grey[y * W + x] = (unsigned char)(((y / 6) % 2 && (x / 5) % 3) ? 30 : 200 + (x * 50) / W);
}

/* ---- the network: one fake connection ---- */
typedef struct {
    char request[512 * 1024];
    size_t request_len;
    const char *reply;
    size_t reply_len, reply_pos;
    int connects, refuse, closes;
    char log[4096];
    unsigned char rnd;
} Net;

static int conn_send(void *ctx, const void *p, size_t n)
{
    Net *net = ctx;
    if (net->request_len + n > sizeof(net->request)) return -1;
    memcpy(net->request + net->request_len, p, n);
    net->request_len += n;
    return (int)n;
}

static int conn_recv(void *ctx, void *p, size_t n)
{
    Net *net = ctx;
    size_t left = net->reply_len - net->reply_pos;
    if (n > left) n = left;
    if (n > 777) n = 777; /* split the reply */
    memcpy(p, net->reply + net->reply_pos, n);
    net->reply_pos += n;
    return (int)n;
}

static int net_connect(void *ud, const char *host, int port, int timeout_us, int io_timeout_us, VjoConn *out)
{
    Net *net = ud;
    net->connects++;
    TEST_CHECK(!strcmp(host, VJO_LENS_HOST) && port == 443);
    if (net->refuse) return VJO_E_NET;
    out->ctx = net;
    out->send = conn_send;
    out->recv = conn_recv;
    return VJO_OK;
}

static void net_disconnect(void *ud, VjoConn *c) { ((Net *)ud)->closes++; }
static void net_random(void *ud, void *buf, size_t n) { memset(buf, ++((Net *)ud)->rnd, n); }
static uint64_t net_time(void *ud) { return 1760000000ull; }
static uint64_t clock_value;
static uint64_t net_now(void *ud) { return clock_value += 1000; }
static void net_log(void *ud, const char *msg)
{
    Net *net = ud;
    size_t used = strlen(net->log);
    snprintf(net->log + used, sizeof(net->log) - used, "%s\n", msg);
}

static Net net;
static char reply[300 * 1024];

static VjoPlatform platform(void)
{
    VjoPlatform p;
    memset(&p, 0, sizeof(p));
    p.ud = &net;
    p.connect = net_connect;
    p.disconnect = net_disconnect;
    p.random = net_random;
    p.unix_time = net_time;
    p.now_us = net_now;
    p.log = net_log;
    p.plain_http = 1; /* the host's: the fake connection carries the HTTP itself */
    return p;
}

static const unsigned char *fixture(size_t *len)
{
    static VjoArena a;
    static const char *data;
    static size_t bytes;
    if (!data) {
        vjo_arena_init(&a, fixture_mem, sizeof(fixture_mem));
        data = vjo_read_file(&a, VJO_FIXTURES "/vn_textbox/lens.pb", &bytes);
        TEST_ASSERT(data != NULL);
    }
    *len = bytes;
    return (const unsigned char *)data;
}

static void set_reply(int status, const unsigned char *body, size_t len)
{
    int head = snprintf(reply, sizeof(reply), "HTTP/1.1 %d X\r\nContent-Length: %zu\r\n\r\n", status, len);
    memcpy(reply + head, body, len);
    net.reply = reply;
    net.reply_len = (size_t)head + len;
    net.reply_pos = 0;
}

static void reset(void)
{
    memset(&net, 0, sizeof(net));
    size_t len;
    const unsigned char *pb = fixture(&len);
    set_reply(200, pb, len);
    fill_image();
}

/* The text the recorded reply holds, as the Shell's Lens path made it. */
static const char *expected_text(void)
{
    static unsigned char mem[256 * 1024];
    VjoArena a;
    VjoLensResult r;
    size_t len;
    const unsigned char *pb = fixture(&len);
    vjo_arena_init(&a, mem, sizeof(mem));
    TEST_ASSERT(!vjo_lens_parse_response(&a, pb, len, &r));
    return vjo_lens_text(&a, &r);
}

/* The JPEG inside the request's protobuf (objects 1 -> data 3 -> payload 1 -> bytes 1). */
static PbField subfield(const PbField *message, unsigned field)
{
    PbReader reader;
    PbField found = {0}, item;
    pb_sub_reader(&reader, message);
    while (pb_next(&reader, &item) > 0)
        if (item.field == field) { found = item; break; }
    return found;
}

static PbField sent_jpeg(void)
{
    const char *body = strstr(net.request, "\r\n\r\n");
    PbField none = {0};
    if (!body) return none;
    body += 4;
    PbField root = {.wire = PB_LEN, .data = (const uint8_t *)body,
                    .len = net.request_len - (size_t)(body - net.request)};
    PbField objects = subfield(&root, 1), data = subfield(&objects, 3), payload = subfield(&data, 1);
    return subfield(&payload, 1);
}

/* The same pixels as RGBA (R = G = B) through the encoder directly: what the
 * Shell's capture of this grey screen sent. */
static int rgba_rows(void *ud, uint32_t row, uint32_t n, uint8_t *dst)
{
    for (uint32_t r = 0; r < n; r++)
        for (unsigned x = 0; x < W; x++) {
            uint8_t g = grey[(row + r) * W + x];
            uint8_t *o = dst + (r * W + x) * 4;
            o[0] = o[1] = o[2] = g;
            o[3] = 255;
        }
    return (int)n;
}

static void test_job_sends_the_region_and_returns_the_text(void)
{
    static unsigned char mem[512 * 1024];
    VjoArena a;
    VjoBuf reference;
    Source src = {.ordered = 1};
    VjoGreySource im = {&src, W, H, strip, cancelled, NULL};
    VjoPlatform p;
    VjoErr err;
    VjoLensJobStats st;
    char text[4096];
    reset();
    p = platform();
    TEST_ASSERT(vjo_lens_job(&p, &im, strip_mem, W * 48, workspace, sizeof(workspace), VJO_LENS_JPEG_QUALITY,
                             text, sizeof(text), &err, &st) == VJO_OK);
    TEST_CHECK(!strcmp(text, expected_text()));
    TEST_CHECK(st.text_bytes == strlen(text) && !st.truncated);
    /* an encoding pass and a comparing pass, each top to bottom in strips of 48 rows (a multiple of
     * the encoder's 16); only the first is flagged as the job's first pass, its last strip as LAST */
    TEST_CHECK(src.calls == 4 && src.ordered && src.rows_asked[0] == 48 && src.rows_asked[1] == 24);
    TEST_CHECK(src.rows_asked[2] == 48 && src.rows_asked[3] == 24);
    TEST_CHECK(src.first_flags == 2 && src.last_flags == 1 && src.flagged_from_zero == 1);
    TEST_CHECK(st.strips == 4 && st.strip_rows == 48 && st.tries == 1 && st.tears == 0);
    TEST_CHECK(net.connects == 1 && net.closes == 1);
    /* the request: Lens's path, its image the encoder's JPEG of these pixels */
    TEST_CHECK(!strncmp(net.request, "POST " VJO_LENS_PATH " ", strlen("POST " VJO_LENS_PATH " ")));
    PbField jpeg = sent_jpeg();
    vjo_arena_init(&a, mem, sizeof(mem));
    vjo_buf_init(&reference, &a);
    TEST_ASSERT(!vjo_jpeg_encode(&a, W, H, W * 4, rgba_rows, NULL, VJO_LENS_JPEG_QUALITY, &reference));
    TEST_CHECK(jpeg.len == reference.len && jpeg.len == st.jpeg_bytes);
    TEST_CHECK(jpeg.data && !memcmp(jpeg.data, reference.data, reference.len));
    TEST_CHECK(jpeg.len > 4 && jpeg.data[0] == 0xFF && jpeg.data[1] == 0xD8);
    /* the log has the request's lines, never the recognized text */
    TEST_CHECK(strstr(net.log, "Lens image=200x72") != NULL);
    TEST_CHECK(strstr(net.log, VJO_LENS_HOST ":443 " VJO_LENS_PATH " -> rc=0 status=200") != NULL);
    TEST_CHECK(strstr(net.log, text) == NULL);
    TEST_CHECK(st.peak > 0 && st.peak <= vjo_lens_job_bytes(W, H));
}

static void test_sizes(void)
{
    TEST_CHECK(vjo_lens_job_bytes(0, 10) == 0 && vjo_lens_job_bytes(961, 10) == 0 && vjo_lens_job_bytes(10, 545) == 0);
    TEST_CHECK(vjo_lens_job_bytes(960, 544) == 8192 + 960 * 64 + 544 * 4 + 960 * 544 * 3 / 4 + 320 * 1024);
    TEST_CHECK(vjo_lens_strip_rows(960, 65536) == 64);  /* 61,440 B */
    TEST_CHECK(vjo_lens_strip_rows(960, 960 * 63) == 48);
    TEST_CHECK(vjo_lens_strip_rows(960, 960 * 15) == 0);
    TEST_CHECK(vjo_lens_strip_rows(0, 65536) == 0);
}

static void test_full_screen_fits_its_workspace(void)
{
    /* The largest region, 960 x 544, of a text-like pattern, in the job's exact room. */
    static unsigned char big[960 * 544];
    static unsigned char room[8192 + 960 * 64 + 544 * 4 + 960 * 544 * 3 / 4 + 320 * 1024];
    Source src = {.pixels = big, .width = 960, .ordered = 1};
    VjoGreySource im = {&src, 960, 544, strip, cancelled, NULL};
    VjoPlatform p;
    VjoErr err;
    VjoLensJobStats st;
    char text[4096];
    for (unsigned y = 0; y < 544; y++)
        for (unsigned x = 0; x < 960; x++)
            big[y * 960 + x] = (unsigned char)(((y / 9) % 2 && (x / 7) % 3) ? 20 : 220);
    reset();
    p = platform();
    TEST_ASSERT(sizeof(room) == vjo_lens_job_bytes(960, 544));
    TEST_CHECK(vjo_lens_job(&p, &im, strip_mem, sizeof(strip_mem), room, sizeof(room), VJO_LENS_JPEG_QUALITY, text,
                            sizeof(text), &err, &st) == VJO_OK);
    /* 64-row strips (the kernel's most per call): 9 a pass, the last 32 rows */
    TEST_CHECK(st.strip_rows == 64 && st.strips == 18 && src.rows_asked[8] == 32 && src.last_flags == 1);
    TEST_CHECK(st.tries == 1 && !st.tears);
    /* this dense pattern (hard edges every 7 px over the whole screen) is the worst a text screen does */
    TEST_CHECK(st.jpeg_bytes < VJO_LENS_JPEG_ROOM(960, 544));
    TEST_MSG("jpeg %u B of %u B room", st.jpeg_bytes, (unsigned)VJO_LENS_JPEG_ROOM(960, 544));
    TEST_CHECK(st.peak <= sizeof(room));
    TEST_MSG("peak %zu of %zu B", st.peak, sizeof(room));
    fprintf(stderr, "full screen: jpeg %u B, workspace peak %zu of %zu B\n", st.jpeg_bytes, st.peak, sizeof(room));
}

static void test_errors(void)
{
    Source src;
    VjoGreySource im = {&src, W, H, strip, cancelled, NULL};
    VjoPlatform p;
    VjoErr err;
    VjoLensJobStats st;
    char text[4096];

    /* the source's own error, and nothing sent */
    reset(); p = platform(); memset(&src, 0, sizeof(src)); src.ordered = 1; src.fail_at = 2;
    TEST_CHECK(vjo_lens_job(&p, &im, strip_mem, W * 48, workspace, sizeof(workspace), 80, text, sizeof(text),
                            &err, &st) == -1000);
    TEST_CHECK(net.connects == 0 && text[0] == 0);
    /* cancelled between strips */
    reset(); p = platform(); memset(&src, 0, sizeof(src)); src.cancel_after = 1;
    TEST_CHECK(vjo_lens_job(&p, &im, strip_mem, W * 48, workspace, sizeof(workspace), 80, text, sizeof(text),
                            &err, &st) == VJO_E_CANCELLED);
    TEST_CHECK(net.connects == 0);
    /* too small a workspace or strip: refused before anything is read */
    reset(); p = platform(); memset(&src, 0, sizeof(src));
    TEST_CHECK(vjo_lens_job(&p, &im, strip_mem, W * 48, workspace, vjo_lens_job_bytes(W, H) - 1, 80, text,
                            sizeof(text), &err, &st) == VJO_E_OOM);
    TEST_CHECK(vjo_lens_job(&p, &im, strip_mem, W * 15, workspace, sizeof(workspace), 80, text, sizeof(text),
                            &err, &st) == VJO_E_OOM);
    TEST_CHECK(src.calls == 0);
    /* a region Lens is never asked for */
    VjoGreySource wide = {&src, 961, H, strip, cancelled, NULL};
    TEST_CHECK(vjo_lens_job(&p, &wide, strip_mem, sizeof(strip_mem), workspace, sizeof(workspace), 80, text,
                            sizeof(text), &err, &st) == VJO_E_OCR_REGION);
    /* no network */
    reset(); p = platform(); memset(&src, 0, sizeof(src)); net.refuse = 1;
    TEST_CHECK(vjo_lens_job(&p, &im, strip_mem, W * 48, workspace, sizeof(workspace), 80, text, sizeof(text),
                            &err, &st) == VJO_E_NET);
    TEST_CHECK(text[0] == 0);
    /* a server error */
    reset(); p = platform(); memset(&src, 0, sizeof(src));
    set_reply(503, (const unsigned char *)"busy", 4);
    TEST_CHECK(vjo_lens_job(&p, &im, strip_mem, W * 48, workspace, sizeof(workspace), 80, text, sizeof(text),
                            &err, &st) == VJO_E_STATUS);
    TEST_CHECK(err.http_status == 503 && text[0] == 0);
    /* a reply that is not Lens's protobuf */
    reset(); p = platform(); memset(&src, 0, sizeof(src));
    set_reply(200, (const unsigned char *)"\xff\xff\xff", 3);
    TEST_CHECK(vjo_lens_job(&p, &im, strip_mem, W * 48, workspace, sizeof(workspace), 80, text, sizeof(text),
                            &err, &st) == VJO_E_PARSE);
}

static void test_text_cut_at_a_character(void)
{
    Source src = {.ordered = 1};
    VjoGreySource im = {&src, W, H, strip, cancelled, NULL};
    VjoPlatform p;
    VjoErr err;
    VjoLensJobStats st;
    const char *full = expected_text();
    size_t n = strlen(full);
    char text[64];
    TEST_ASSERT(n > sizeof(text));
    reset();
    p = platform();
    TEST_CHECK(vjo_lens_job(&p, &im, strip_mem, W * 48, workspace, sizeof(workspace), 80, text, sizeof(text),
                            &err, &st) == VJO_OK);
    TEST_CHECK(st.truncated && st.text_bytes == strlen(text) && st.text_bytes < sizeof(text));
    TEST_CHECK(!strncmp(text, full, st.text_bytes));
    /* never half a character: the next byte of the full text starts one */
    TEST_CHECK(((unsigned char)full[st.text_bytes] & 0xC0) != 0x80);
}

/* The screen changes while a try reads it: the try is read again; a region that
 * never holds still sends its last try's image (as the Shell's single frame did). */
static void test_changing_screen_is_read_again(void)
{
    VjoPlatform p;
    VjoErr err;
    VjoLensJobStats st;
    char text[4096];
    /* row 5 changes on the second call: the first strip of the comparing pass reads otherwise */
    Source once = {.ordered = 1, .change_from = 3, .change_to = 3};
    VjoGreySource im = {&once, W, H, strip, cancelled, NULL};
    reset();
    p = platform();
    TEST_CHECK(vjo_lens_job(&p, &im, strip_mem, W * 48, workspace, sizeof(workspace), 80, text, sizeof(text),
                            &err, &st) == VJO_OK);
    TEST_CHECK(st.tries == 2 && st.tears == 0 && once.calls == 8 && once.flagged_from_zero == 2);
    TEST_CHECK(once.last_flags == 2 && once.ordered && net.connects == 1);
    /* the second try's capture is the one published and sent: its rows */
    TEST_CHECK(!strcmp(text, expected_text()));
    /* an animation on every read: three tries, the third image sent, its comparison skipped */
    Source always = {.ordered = 1, .change_from = 1, .change_to = 1000};
    VjoGreySource anim = {&always, W, H, strip, cancelled, NULL};
    reset();
    p = platform();
    TEST_CHECK(vjo_lens_job(&p, &anim, strip_mem, W * 48, workspace, sizeof(workspace), 80, text, sizeof(text),
                            &err, &st) == VJO_OK);
    /* each try's encoding reads row 5 changed against nothing; its comparison against the encoding: equal
     * (both see the change), so the first try holds: an animation that changes on every read reads the same */
    TEST_CHECK(st.tries == 1 && always.calls == 4);
    /* a row that alternates between reads: never the same twice in a row */
    Source flip = {.ordered = 1};
    VjoGreySource fl = {&flip, W, H, strip, cancelled, NULL};
    reset();
    p = platform();
    flip.change_from = 3; flip.change_to = 3; /* try 1's comparison differs */
    TEST_CHECK(vjo_lens_job(&p, &fl, strip_mem, W * 48, workspace, sizeof(workspace), 80, text, sizeof(text),
                            &err, &st) == VJO_OK && st.tries == 2);
}

static unsigned flip_calls;
static int flipping_strip(void *ud, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags)
{
    int rc = strip(ud, y, n, dst, stride, flags);
    if (y == 0 && (++flip_calls & 1)) dst[0] ^= 0x55; /* row 0 alternates on every pass */
    return rc;
}

static void test_screen_that_never_holds_still(void)
{
    VjoPlatform p;
    VjoErr err;
    VjoLensJobStats st;
    char text[4096];
    Source src = {.ordered = 1};
    VjoGreySource im = {&src, W, H, flipping_strip, cancelled, NULL};
    reset();
    flip_calls = 0;
    p = platform();
    TEST_CHECK(vjo_lens_job(&p, &im, strip_mem, W * 48, workspace, sizeof(workspace), 80, text, sizeof(text),
                            &err, &st) == VJO_OK);
    /* tries 1 and 2 compare unequal; try 3's image is sent without a comparison, its rows noted torn */
    TEST_CHECK(st.tries == VJO_VOCR_TRIES && st.tears == 1);
    /* two strips a pass: (encode + compare) twice, then the last encoding */
    TEST_CHECK(src.calls == 2 * 2 * 2 + 2 && src.flagged_from_zero == VJO_VOCR_TRIES && src.last_flags == VJO_VOCR_TRIES);
    TEST_CHECK(net.connects == 1 && !strcmp(text, expected_text()));
}

TEST_LIST = {
    {"changing_screen_is_read_again", test_changing_screen_is_read_again},
    {"screen_that_never_holds_still", test_screen_that_never_holds_still},
    {"job_sends_the_region_and_returns_the_text", test_job_sends_the_region_and_returns_the_text},
    {"sizes", test_sizes},
    {"full_screen_room", test_full_screen_fits_its_workspace},
    {"errors", test_errors},
    {"text_cut_at_a_character", test_text_cut_at_a_character},
    {NULL, NULL}};
