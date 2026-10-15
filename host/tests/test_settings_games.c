/* The Overlay app's Games page through its actual input and draw loop
 * (settings/main.c) and its actual catalog (settings/catalog.c), on the host,
 * over synthetic installed games on a temporary memory card: ux0:app/ID/
 * sce_sys/param.sfo with a TITLE and an icon0.png each (generated titles and
 * patterns; no real game names or icons). The SDK boundary (vita2d, the
 * controller, the touch screen, the display) is scripted and records, per
 * frame, what each game card showed and what the frame cost: icon loads,
 * icon file reads, glyphs measured by vita2d_pvf_text_width, GPU waits, and
 * the frame's host CPU time. Icon loads decode the file as libvita2d's
 * libpng loader does (settings_png_fixture.h: inflate with zlib when built
 * with it, unfilter, palette expansion into a cleared texture), so they cost
 * a decode; vita2d_load_PNG_file reads its file in libpng's pieces.
 *
 * libvita2d's GPU rules are checked on every texture: it may be freed only
 * after the GPU was waited for since it was last drawn (libvita2d #28), never
 * while a frame is being drawn, and never drawn once freed. A loaded texture
 * comes with gxm_rtgt and depth_UID set as libvita2d's uncleared malloc may
 * leave them; freeing one the app has not cleared would destroy a render
 * target or free a block that is not the texture's, and is counted.
 *
 * Built twice: vjo-settings-games-tests (ctest settings_games, the tests
 * below) and vjo-settings-bench (VJO_SETTINGS_BENCH: each scenario's frame
 * costs as a table; with VJO_SETTINGS_BENCH_MAIN, another settings/main.c's).
 * Text widths are stub metrics (an ASCII character 10 units, any other 18,
 * per 18-unit em), not the firmware's PVF fonts; host times are this
 * machine's, not the Vita's. */
#ifndef VJO_SETTINGS_BENCH
#include "acutest.h"
#define TEST_ONLY
#else
/* Evaluated in every build (assert would vanish with NDEBUG). */
#define TEST_ASSERT(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); _exit(2); } } while (0)
#define TEST_ONLY __attribute__((unused)) /* the tests' helpers, unused by the benchmark */
#endif
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/registrymgr.h>
#include <psp2/touch.h>
#include <vita2d.h>
#include "../../core/buttons.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../../core/games.h"
#include "settings_png_fixture.h"
#include "settings_wrap_reference.h"

int vjo_settings_main(void); /* settings/main.c's main(), renamed for the harness */

/* settings/main.c's Games page layout. */
#define ROWS 3
#define GAMES_Y 174
#define GAME_H 80
#define GAME_PITCH 88
#define HINT_Y 535
#define STATUS_Y 467
#define SELECTED RGBA8(35, 56, 70, 255)
#define CDRAM_BLOCK (256 * 1024)

/* ---- what a frame showed and cost ---- */
#define MAX_FRAMES 4200
typedef struct {
    char title[ROWS][2][128], id[ROWS][16], icon[ROWS][16]; /* icon: the texture's game, "JP", or "" */
    char hints[128], counter[24], status[2][128];
    int counter_x;                                           /* where the counter starts */
    char loaded[16];                                         /* the game whose icon this frame loaded (the last) */
    int focus;                                               /* the focused card, -1 */
    int loads, frees, waits, icon_opens, icon_reads, scans, glyphs, measures, live;
    long ns;
} Frame;
typedef struct {
    int frames, exited, main_rc;
    /* GPU rules broken, and other totals */
    int free_while_drawing, free_in_flight, stale_fields, drawn_after_free, double_free, pool_empty;
    int loads, live_at_exit, max_live, decode_errors;
    long max_cdram;
    long startup_ns;
    unsigned short attempts[MAX_FRAMES]; /* icon0.png opens tried, by game number (VJOTnnnnn) */
    Frame shown[MAX_FRAMES];
} Session;
static Session *session;

/* ---- storage: the SDK's file calls over real files ---- */
static int native(int n) { return n < 0 ? (int)(0x80010000u | (unsigned)errno) : n; }
static Frame cur;
static int icon_fd[1024];
static int is_icon(const char *p)
{
    size_t n = strlen(p);
    return n >= 9 && !strcmp(p + n - 9, "icon0.png");
}
int sceIoOpen(const char *p, int flags, unsigned mode)
{
    int fd = native(open(p, flags, mode)), game;
    if (fd >= 0 && fd < 1024) icon_fd[fd] = is_icon(p);
    if (is_icon(p)) {
        cur.icon_opens++;
        if (sscanf(p, "ux0:app/VJOT%5d", &game) == 1 && game >= 0 && game < MAX_FRAMES) session->attempts[game]++;
    }
    return fd;
}
int sceIoClose(int fd) { if (fd >= 0 && fd < 1024) icon_fd[fd] = 0; return native(close(fd)); }
int sceIoRead(int fd, void *p, size_t len)
{
    if (fd >= 0 && fd < 1024 && icon_fd[fd]) cur.icon_reads++;
    return native((int)read(fd, p, len));
}
int sceIoWrite(int fd, const void *p, size_t len) { return native((int)write(fd, p, len)); }
SceOff sceIoLseek(int fd, SceOff off, int whence) { return lseek(fd, off, whence); }
int sceIoMkdir(const char *p, unsigned mode) { return native(mkdir(p, mode)); }
int sceIoGetstat(const char *p, SceIoStat *out)
{ struct stat st; int rc = stat(p, &st); if (!rc) out->st_mode = st.st_mode; return native(rc); }
int sceIoRename(const char *a, const char *b) { return native(rename(a, b)); }
int sceIoRemove(const char *p) { return native(unlink(p)); }
int sceIoSync(const char *p, unsigned flags) { (void)p; (void)flags; return 0; }
static DIR *dirs[8];
static char dir_path[8][64];
int sceIoDopen(const char *p)
{
    if (!strcmp(p, "ux0:app")) cur.scans++;
    for (int i = 0; i < 8; i++)
        if (!dirs[i]) {
            if (!(dirs[i] = opendir(p))) return native(-1);
            snprintf(dir_path[i], sizeof(dir_path[i]), "%s", p);
            return 100 + i;
        }
    return (int)0x80010018u;
}
int sceIoDread(int d, SceIoDirent *e)
{
    DIR *dir = d >= 100 && d < 108 ? dirs[d - 100] : NULL;
    struct dirent *de;
    if (!dir) return (int)0x80010009u;
    while ((de = readdir(dir)) && de->d_name[0] == '.') {}
    if (!de) return 0;
    char path[400];
    struct stat st;
    memset(e, 0, sizeof(*e));
    snprintf(path, sizeof(path), "%s/%s", dir_path[d - 100], de->d_name);
    if (!stat(path, &st)) e->d_stat.st_mode = st.st_mode;
    snprintf(e->d_name, sizeof(e->d_name), "%s", de->d_name);
    return 1;
}
int sceIoDclose(int d)
{
    if (d < 100 || d >= 108 || !dirs[d - 100]) return (int)0x80010009u;
    closedir(dirs[d - 100]);
    dirs[d - 100] = NULL;
    return 0;
}

/* ---- the scripted controller, touch screen and display ---- */
typedef struct { unsigned buttons; int touch, x, y, stick; } Input; /* stick: the left stick's y from its centre */
static Input script[MAX_FRAMES];
static int script_frames, frame;
/* Called at the start of each frame (the app's process): a test changes the card there. */
static void (*frame_hook)(int frame);
static struct timespec frame_start, process_start;
static long since(const struct timespec *t)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - t->tv_sec) * 1000000000L + (now.tv_nsec - t->tv_nsec);
}
uint64_t sceKernelGetProcessTimeWide(void) { return (uint64_t)frame * 16667u; }
int sceKernelExitProcess(int status) { session->exited = status; return 0; }
/* The console's /CONFIG/SYSTEM/button_assign: × enters unless a test sets ○. */
static int button_assign = VJO_BUTTON_ASSIGN_CROSS;
int sceRegMgrGetKeyInt(const char *category, const char *name, int *buf)
{
    if (strcmp(category, "/CONFIG/SYSTEM") || strcmp(name, "button_assign")) return (int)0x800D0003;
    *buf = button_assign;
    return 0;
}
#define ENTER (vjo_buttons(button_assign).enter)
#define CANCEL (vjo_buttons(button_assign).cancel)
int sceCtrlSetSamplingMode(int mode) { (void)mode; return 0; }
int sceTouchSetSamplingState(int port, int state) { (void)port; (void)state; return 0; }
static int live_textures(void);
/* The loop reads the pad first: a frame starts here. */
int sceCtrlPeekBufferPositive(int port, SceCtrlData *pad, int count)
{
    (void)port; (void)count;
    if (!frame) session->startup_ns = since(&process_start);
    if (frame_hook) frame_hook(frame);
    int scans = cur.scans; /* the scan before the first frame belongs to it */
    memset(&cur, 0, sizeof(cur));
    cur.scans = frame ? 0 : scans;
    cur.focus = -1;
    clock_gettime(CLOCK_MONOTONIC, &frame_start);
    pad->buttons = frame < script_frames && frame < MAX_FRAMES - 1 ? script[frame].buttons : CANCEL;
    pad->lx = pad->rx = pad->ry = 128;
    pad->ly = (unsigned char)(128 + (frame < script_frames && frame < MAX_FRAMES - 1 ? script[frame].stick : 0));
    return 1;
}
int sceTouchPeek(int port, SceTouchData *data, int count)
{
    (void)port; (void)count;
    memset(data, 0, sizeof(*data));
    if (frame < script_frames && script[frame].touch) {
        data->reportNum = 1;
        data->report[0].x = (uint16_t)(script[frame].x * 2);
        data->report[0].y = (uint16_t)(script[frame].y * 2);
    }
    return 1;
}
int sceDisplayWaitVblankStart(void)
{
    cur.ns = since(&frame_start);
    cur.live = live_textures();
    if (frame < MAX_FRAMES) session->shown[frame] = cur;
    frame++;
    return 0;
}

/* ---- vita2d: textures under the GPU's rules ---- */
#define POOL 6000
static vita2d_texture pool[POOL];
static int pool_used, drawing, font_dummy;
static void copy(char *dst, size_t cap, const char *s) { snprintf(dst, cap, "%s", s); }
static int live_textures(void)
{
    int n = 0;
    for (int i = 0; i < pool_used; i++) n += pool[i].live;
    return n;
}
static long cdram_bytes(void)
{
    long n = 0;
    for (int i = 0; i < pool_used; i++)
        if (pool[i].live) {
            long size = (long)((pool[i].width + 7) & ~7u) * pool[i].height * 4;
            n += (size + CDRAM_BLOCK - 1) / CDRAM_BLOCK * CDRAM_BLOCK; /* libvita2d's CDRAM rounding */
        }
    return n;
}
/* A decoded texture, as libvita2d returns it: its two render-target fields uncleared. */
static vita2d_texture *new_texture(const uint8_t *png, size_t size)
{
    if (pool_used == POOL) { session->pool_empty++; return NULL; }
    vita2d_texture *t = &pool[pool_used];
    unsigned w = fx_get32(png + 16), h = fx_get32(png + 20);
    memset(t, 0, sizeof(*t));
    t->pixels = malloc((size_t)w * h * 4);
    if (!t->pixels || fx_png_decode(png, size, t->pixels, t->id, sizeof(t->id))) {
        free(t->pixels);
        session->decode_errors++;
        return NULL;
    }
    pool_used++;
    t->width = w; t->height = h;
    t->gxm_rtgt = (void *)(uintptr_t)0xDEADBEEFu;
    t->depth_UID = 0x5EED;
    t->live = 1;
    copy(cur.loaded, sizeof(cur.loaded), t->id);
    cur.loads++;
    session->loads++;
    if (live_textures() > session->max_live) session->max_live = live_textures();
    if (cdram_bytes() > session->max_cdram) session->max_cdram = cdram_bytes();
    return t;
}
vita2d_texture *vita2d_load_PNG_buffer(const void *buffer)
{
    /* libvita2d's buffer reader has no length: it reads chunks as their headers say. */
    return new_texture(buffer, (size_t)1 << 30);
}
/* libvita2d's file loader with libpng's reads: the signature, then each chunk's
 * header, data (IDAT in pieces of at most 8 KiB) and CRC, to the last IDAT. */
vita2d_texture *vita2d_load_PNG_file(const char *filename)
{
    static uint8_t file[512 * 1024];
    int fd = sceIoOpen(filename, O_RDONLY, 0);
    size_t at = 8, last_idat = 0;
    if (fd < 0) return NULL;
    if (sceIoRead(fd, file, 8) != 8) { sceIoClose(fd); return NULL; }
    for (;;) {
        if (at + 12 > sizeof(file) || sceIoRead(fd, file + at, 8) != 8) break;
        uint32_t n = fx_get32(file + at);
        int idat = !memcmp(file + at + 4, "IDAT", 4);
        if (n > sizeof(file) - at - 12) break;
        for (uint32_t done = 0; done < n;) {
            uint32_t piece = idat && n - done > 8192 ? 8192 : n - done;
            if (sceIoRead(fd, file + at + 8 + done, piece) != (int)piece) { sceIoClose(fd); return NULL; }
            done += piece;
        }
        if (sceIoRead(fd, file + at + 8 + n, 4) != 4) break;
        at += 12 + n;
        if (idat) {
            uint8_t next[8]; /* libpng reads past the last IDAT only to find more image data */
            last_idat = at;
            if (pread(fd, next, 8, (off_t)at) != 8 || memcmp(next + 4, "IDAT", 4)) break;
        }
    }
    sceIoClose(fd);
    if (!last_idat) return NULL;
    memcpy(file + last_idat, "\0\0\0\0IEND\xAE\x42\x60\x82", 12);
    return new_texture(file, last_idat + 12);
}
void vita2d_free_texture(vita2d_texture *t)
{
    if (!t) return;
    cur.frees++;
    if (!t->live) { session->double_free++; return; }
    if (drawing) session->free_while_drawing++;
    if (t->in_flight) session->free_in_flight++;
    if (t->gxm_rtgt || t->depth_UID) session->stale_fields++;
    t->live = 0;
    free(t->pixels);
    t->pixels = NULL;
}
unsigned int vita2d_texture_get_width(const vita2d_texture *t) { return t->width; }
unsigned int vita2d_texture_get_height(const vita2d_texture *t) { return t->height; }
void vita2d_draw_texture_scale(const vita2d_texture *texture, float x, float y, float x_scale, float y_scale)
{
    vita2d_texture *t = (vita2d_texture *)texture;
    if (!t->live) session->drawn_after_free++;
    t->in_flight = 1;
    for (int r = 0; r < ROWS; r++)
        if ((int)x == 38 && (int)y == GAMES_Y + r * GAME_PITCH + 8 &&
            (int)(x_scale * t->width + 0.5f) == 64 && (int)(y_scale * t->height + 0.5f) == 64)
            copy(cur.icon[r], sizeof(cur.icon[r]), t->id);
}
void vita2d_wait_rendering_done(void)
{
    cur.waits++;
    for (int i = 0; i < pool_used; i++) pool[i].in_flight = 0;
}
int vita2d_init(void) { return 0; }
int vita2d_fini(void) { return 0; }
void vita2d_set_clear_color(unsigned int color) { (void)color; }
void vita2d_start_drawing(void) { drawing = 1; }
void vita2d_end_drawing(void) { drawing = 0; }
void vita2d_swap_buffers(void) {}
void vita2d_clear_screen(void) {}
void vita2d_enable_clipping(void) {}
void vita2d_disable_clipping(void) {}
void vita2d_set_clip_rectangle(int x_min, int y_min, int x_max, int y_max)
{ (void)x_min; (void)y_min; (void)x_max; (void)y_max; }
void vita2d_draw_rectangle(float x, float y, float w, float h, unsigned int color)
{
    for (int r = 0; r < ROWS; r++)
        if ((int)x == 24 && (int)w == 912 && (int)h == GAME_H && (int)y == GAMES_Y + r * GAME_PITCH && color == SELECTED)
            cur.focus = r;
}
vita2d_pvf *vita2d_load_default_pvf(void) { return (vita2d_pvf *)&font_dummy; }
void vita2d_free_pvf(vita2d_pvf *font) { (void)font; }
/* Stub metrics: 10 units for an ASCII character (a test may widen them), 18 for
 * any other, at scale 1. The app calibrates its em from full-width glyphs, so
 * wider ASCII makes Latin text wider on screen. */
static int ascii_units = 10;
static int stub_units(const char *text, int *glyphs)
{
    int units = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if (*p < 0x80) units += ascii_units, (*glyphs)++;
        else if ((*p & 0xC0) != 0x80) units += 18, (*glyphs)++;
    return units;
}
int vita2d_pvf_text_width(vita2d_pvf *font, float scale, const char *text)
{
    (void)font;
    cur.measures++;
    return (int)(stub_units(text, &cur.glyphs) * scale + 0.5f);
}
/* What each card shows, by where main.c draws it. */
int vita2d_pvf_draw_text(vita2d_pvf *font, int x, int y, unsigned int color, float scale, const char *text)
{
    (void)font; (void)color; (void)scale;
    for (int r = 0; r < ROWS; r++) {
        int top = GAMES_Y + r * GAME_PITCH;
        if (x == 120 && y == top + 30) copy(cur.title[r][0], sizeof(cur.title[r][0]), text);
        else if (x == 120 && y == top + 62) copy(cur.title[r][1], sizeof(cur.title[r][1]), text);
        else if (y == top + 70 && x > 700) copy(cur.id[r], sizeof(cur.id[r]), text);
        else if (y == top + 47 && !strcmp(text, "JP")) copy(cur.icon[r], sizeof(cur.icon[r]), "JP");
    }
    if (y == HINT_Y) {
        if (x == 24) copy(cur.hints, sizeof(cur.hints), text);
        else { copy(cur.counter, sizeof(cur.counter), text); cur.counter_x = x; }
    }
    if (x == 40 && (y == STATUS_Y || y == STATUS_Y + 28)) copy(cur.status[(y - STATUS_Y) / 28], sizeof(cur.status[0]), text);
    return 0;
}
TEST_ONLY static int stub_width(float pixels, const char *s)
{
    int glyphs = 0; /* the reference's own measuring is not the app's */
    return (int)(stub_units(s, &glyphs) * (pixels / 18.0f) + 0.5f);
}
/* The same widths, counting the glyphs measured, as the app's measure does. */
static long counted;
TEST_ONLY static int counting_width(float pixels, const char *s)
{
    int glyphs = 0;
    int w = (int)(stub_units(s, &glyphs) * (pixels / 18.0f) + 0.5f);
    counted += glyphs;
    return w;
}

/* ---- the memory card: synthetic installed games ---- */
enum { ICON_OK, ICON_NONE, ICON_DAMAGED, ICON_HUGE, ICON_NOT_PNG, ICON_RGBA, ICON_SMALL };
typedef struct { char id[VJO_TITLE_ID_SIZE]; char title[200]; int icon; } Game;
#define MAX_GAMES 520
static Game games[MAX_GAMES + 1];
static int game_count;
/* Synthetic titles: Latin initials A-Z, some lower case, kana of several
 * gojuon rows (dakuten included), kanji, digits and symbols, short and long,
 * and the edges of the letters: ゔ, ヴ and ヵ (filed under あ, ア and カ but
 * byte-sorted after ん and ン), '{' and '~' (after 'z'), half-width kana and a
 * byte that is not UTF-8. */
static void make_title(int i, char *out, size_t cap)
{
    static const char *const latin[] = {"Amber", "Bright", "Cobalt", "Distant", "Echo", "Frozen", "Golden",
        "Hidden", "Iron", "Jade", "Kinetic", "Lunar", "Midnight", "Neon", "Orbit", "Paper", "Quiet", "Rapid",
        "Silent", "Tidal", "Upper", "Velvet", "Winter", "Xenon", "Yellow", "Zero"};
    static const char *const nouns[] = {"Archive", "Garden", "Signal", "Harbor", "Lantern", "Circuit"};
    static const char *const kana[] = {"あおいそらの", "がっこうの", "かぜの", "さくらの", "ざわめきの", "たいようの",
        "なつの", "ぱずるの", "はるかな", "まよなかの", "やまの", "らせんの", "わすれた",
        "アーカイブ", "カタログ", "ガラスの", "サウンド", "タイム", "ハーモニー", "マップ", "ワールド"};
    static const char *const kanji[] = {"空の物語", "海の記憶", "星の欠片", "夢の続き", "森の歌"};
    const char *tail = i % 7 == 3 ? " - Synthetic Long Edition With Extra Subtitle Words For Wrapping Twice" :
                       i % 11 == 5 ? "　〜とても長い日本語の副題がついたテスト用の題名で二行を超える〜" : "";
    switch (i % 10) {
    case 0: case 1: case 2: case 3:
        snprintf(out, cap, "%s %s %d%s", latin[(i * 7) % 26], nouns[i % 6], i, tail);
        break;
    case 4: /* lower case first: listed with its letter */
        snprintf(out, cap, "%s %s %d%s", latin[(i * 3) % 26], nouns[(i + 1) % 6], i, tail);
        out[0] = (char)(out[0] - 'A' + 'a');
        break;
    case 5: case 6: case 7: /* each kana in turn: row-mates such as か/が and カ/ガ by 60 games */
        snprintf(out, cap, "%s%s %d%s", kana[(i / 10 * 3 + i % 10 - 5) % 21], i % 2 ? "ものがたり" : "きおく", i, tail);
        break;
    case 8:
        snprintf(out, cap, "%s %d%s", kanji[i % 5], i, tail);
        break;
    default: { /* the first six by 60 games */
#ifndef VJO_SETTINGS_BENCH
        static const char *const edges[] = {"ゔぁいおりんの", "ヴァイオリン", "ヵ月の旅", "{Curly} Brace", "8-bit Quest",
            "ｶﾀｶﾅ ゲーム", "~Tilde Tale", "[Test] Bracket", "3D Room", "ゖの歌", "ヶ所めぐり", "\xFF Broken Byte"};
        snprintf(out, cap, "%s %d%s", edges[i / 10 % 12], i, tail);
#else /* the benchmark keeps the titles its published tables were measured with */
        snprintf(out, cap, "%s %d%s", i % 3 == 0 ? "3D Room" : i % 3 == 1 ? "[Test] Bracket" : "8-bit Quest", i, tail);
#endif
        break;
    }
    }
}
static char card_dir[64];
static void remove_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st)) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            char child[512];
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            if (snprintf(child, sizeof(child), "%s/%s", path, e->d_name) < (int)sizeof(child)) remove_tree(child);
        }
        if (d) closedir(d);
        rmdir(path);
    } else unlink(path);
}
static void remove_card(void) { if (card_dir[0] && chdir("/") == 0) remove_tree(card_dir); }
static void put_file(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    TEST_ASSERT(f != NULL);
    TEST_ASSERT(fwrite(data, 1, n, f) == n);
    TEST_ASSERT(fclose(f) == 0);
}
/* A PARAM.SFO with one key, TITLE. */
static void put_sfo(const char *path, const char *title)
{
    uint8_t sfo[512] = {0};
    uint32_t n = (uint32_t)strlen(title) + 1, cap = (n + 3) & ~3u;
    sfo[0] = 0; sfo[1] = 'P'; sfo[2] = 'S'; sfo[3] = 'F';
    sfo[4] = 1; sfo[5] = 1;
    sfo[8] = 36; sfo[12] = 44; sfo[16] = 1;              /* keys at 36, values at 44, one entry */
    sfo[20] = 0; sfo[22] = 0x04; sfo[23] = 0x02;          /* key 0, UTF-8 string */
    memcpy(sfo + 24, &n, 4); memcpy(sfo + 28, &cap, 4); /* little-endian on this host */
    memcpy(sfo + 36, "TITLE", 6);
    memcpy(sfo + 44, title, n);
    put_file(path, sfo, 44 + cap);
}
/* One game on the card: its param.sfo and, as g->icon says, its icon0.png. */
static void install(const Game *g, int seed)
{
    static uint8_t png[400 * 1024];
    char path[160];
    size_t size = 0;
    snprintf(path, sizeof(path), "ux0:app/%s", g->id); mkdir(path, 0777);
    snprintf(path, sizeof(path), "ux0:app/%s/sce_sys", g->id); mkdir(path, 0777);
    snprintf(path, sizeof(path), "ux0:app/%s/sce_sys/param.sfo", g->id);
    put_sfo(path, g->title);
    snprintf(path, sizeof(path), "ux0:app/%s/sce_sys/icon0.png", g->id);
    switch (g->icon) {
    case ICON_NONE: return;
    case ICON_NOT_PNG: put_file(path, "GIF89a not a PNG", 16); return;
    case ICON_HUGE: size = fx_png(png, sizeof(png), 300, 300, 1, (unsigned)seed, g->id); break;
    case ICON_RGBA: size = fx_png(png, sizeof(png), 128, 128, 0, (unsigned)seed, g->id); break;
    case ICON_SMALL: size = fx_png(png, sizeof(png), 64, 64, 1, (unsigned)seed, g->id); break;
    default: size = fx_png(png, sizeof(png), 128, 128, 1, (unsigned)seed, g->id); break;
    }
    TEST_ASSERT(size > 0);
    if (g->icon == ICON_DAMAGED) png[size / 2] ^= 0x10; /* inside IDAT: its CRC fails */
    put_file(path, png, size);
}
/* n games (the catalog adds Adrenaline's shared entry), their icons as kind(i) says. */
static void card(int n, int (*kind)(int))
{
    if (!card_dir[0]) {
        char d[] = "/tmp/vjo-settings-games-XXXXXX";
        TEST_ASSERT(mkdtemp(d) != NULL);
        snprintf(card_dir, sizeof(card_dir), "%s", d);
        atexit(remove_card);
    }
    TEST_ASSERT(chdir("/") == 0);
    remove_tree(card_dir);
    TEST_ASSERT(mkdir(card_dir, 0777) == 0 && chdir(card_dir) == 0);
    mkdir("ux0:data", 0777); mkdir("ux0:data/VitaJPOverlay", 0777); mkdir("ux0:app", 0777);
    game_count = n;
    frame_hook = NULL;
    for (int i = 0; i < n; i++) {
        Game *g = &games[i];
        snprintf(g->id, sizeof(g->id), "VJOT%05d", i);
        make_title(i, g->title, sizeof(g->title));
        g->icon = kind ? kind(i) : ICON_OK;
        install(g, i);
    }
    /* settings/catalog.c's synthetic entry, which has no icon */
    snprintf(games[n].id, sizeof(games[n].id), "NPXS10028");
    snprintf(games[n].title, sizeof(games[n].title), "Adrenaline / PSP games (shared setting)");
    games[n].icon = ICON_NONE;
}

/* ---- the list as the spec orders and groups it (the test's own reference) ---- */
/* The letter a title is listed under: ASCII letters by letter, other ASCII
 * together, kana by gojuon row (hiragana and katakana apart), else itself. */
static uint32_t spec_group(const char *title)
{
    static const struct { uint32_t first, last, head; } rows[] = {
        {0x3041, 0x304A, 0x3042}, {0x304B, 0x3054, 0x304B}, {0x3055, 0x305E, 0x3055}, {0x305F, 0x3069, 0x305F},
        {0x306A, 0x306E, 0x306A}, {0x306F, 0x307D, 0x306F}, {0x307E, 0x3082, 0x307E}, {0x3083, 0x3088, 0x3084},
        {0x3089, 0x308D, 0x3089}, {0x308E, 0x3093, 0x308F}, {0x3094, 0x3094, 0x3042}, {0x3095, 0x3096, 0x304B}};
    size_t at = 0;
    uint32_t c = vjo_utf8_next(title, strlen(title), &at);
    if (c < 0x80) return (c | 0x20) >= 'a' && (c | 0x20) <= 'z' ? (c & ~0x20u) : '#';
    int katakana = c >= 0x30A1 && c <= 0x30F6;
    uint32_t k = katakana ? c - 0x60 : c;
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
        if (k >= rows[i].first && k <= rows[i].last) return rows[i].head + (katakana ? 0x60 : 0);
    return c;
}
/* By letter, then alphabetically, capital and small letters together, then by bytes and ID. */
static int fold(unsigned char c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; }
static int spec_order(const void *a, const void *b)
{
    const Game *ga = a, *gb = b;
    uint32_t la = spec_group(ga->title), lb = spec_group(gb->title);
    if (la != lb) return la < lb ? -1 : 1;
    const unsigned char *x = (const unsigned char *)ga->title, *y = (const unsigned char *)gb->title;
    while (*x && fold(*x) == fold(*y)) x++, y++;
    int n = fold(*x) - fold(*y);
    if (!n) n = strcmp(ga->title, gb->title);
    return n ? n : strcmp(ga->id, gb->id);
}
static Game list[MAX_GAMES + 1];
static int list_count;
TEST_ONLY static void spec_list(void)
{
    list_count = game_count + 1;
    memcpy(list, games, sizeof(Game) * (size_t)list_count);
    qsort(list, (size_t)list_count, sizeof(Game), spec_order);
}
static int list_index(const char *id)
{
    for (int i = 0; i < list_count; i++) if (!strcmp(list[i].id, id)) return i;
    return -1;
}
TEST_ONLY static const Game *game_of(const char *id)
{
    int i = list_index(id);
    return i < 0 ? NULL : &list[i];
}

/* ---- running the app: one process per run, as on the console ---- */
static int run(int n)
{
    if (!session) {
        session = mmap(NULL, sizeof(*session), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        TEST_ASSERT(session != MAP_FAILED);
    }
    memset(session, 0, sizeof(*session));
    session->main_rc = session->exited = -1;
    script_frames = n;
    fflush(stdout); fflush(stderr);
    pid_t pid = fork();
    TEST_ASSERT(pid >= 0);
    if (pid == 0) {
        frame = 0;
        clock_gettime(CLOCK_MONOTONIC, &process_start);
        session->main_rc = vjo_settings_main();
        session->frames = frame;
        session->live_at_exit = live_textures();
        _exit(0);
    }
    int status = 0;
    TEST_ASSERT(waitpid(pid, &status, 0) == pid);
    TEST_ASSERT(WIFEXITED(status) && !WEXITSTATUS(status));
    TEST_ASSERT(session->main_rc == 0 && session->exited == 0);
    return session->frames;
}
/* Script builders: n frames of buttons held (0: none); a tap is one frame down, one up. */
static int at;
static void hold(unsigned buttons, int n) { for (int i = 0; i < n && at < MAX_FRAMES - 2; i++) script[at++] = (Input){buttons, 0, 0, 0, 0}; }
static void tap(unsigned buttons, int gap) { hold(buttons, 1); hold(0, gap); }
/* The left stick held n frames, offset from its centre (-128 top .. 127 bottom). */
TEST_ONLY static void hold_stick(int stick, int n) { for (int i = 0; i < n && at < MAX_FRAMES - 2; i++) script[at++] = (Input){0, 0, 0, 0, stick}; }

/* ---- scenarios (shared by the tests and the benchmark) ---- */
#define DOWN SCE_CTRL_DOWN
#define UP SCE_CTRL_UP
TEST_ONLY static int kind_mixed(int i)
{
    return i == 4 ? ICON_NONE : i == 9 ? ICON_DAMAGED : i == 13 ? ICON_HUGE : i == 17 ? ICON_NOT_PNG :
           i % 23 == 21 ? ICON_RGBA : i % 19 == 18 ? ICON_SMALL : ICON_OK;
}
/* Idle: the window fills, then nothing changes. */
static void scenario_idle(void) { at = 0; hold(0, 120); }
/* Down held over about 30 rows, a pause, Up held back to the top. */
static void scenario_scroll(void) { at = 0; hold(0, 20); hold(DOWN, 200); hold(0, 20); hold(UP, 200); hold(0, 20); }
/* R pressed 8 times, then L 8 times; then R held. */
static void scenario_pages(void)
{
    at = 0;
    hold(0, 20);
    for (int i = 0; i < 8; i++) tap(SCE_CTRL_RTRIGGER, 15);
    for (int i = 0; i < 8; i++) tap(SCE_CTRL_LTRIGGER, 15);
    hold(SCE_CTRL_RTRIGGER, 150);
    hold(0, 20);
}
/* Enter on cards (each saves games.ini and rebuilds the list): the first and
 * second games off, the third off, on, off and on; then Square through the
 * Enabled, Disabled and All filters. */
#define TOGGLE_FRAMES (20 + 8 * 11)
static void scenario_toggle(void)
{
    at = 0;
    hold(0, 20);
    tap(ENTER, 10); tap(DOWN, 10); tap(ENTER, 10); tap(DOWN, 10);
    for (int i = 0; i < 4; i++) tap(ENTER, 10);
    for (int i = 0; i < 3; i++) tap(SCE_CTRL_SQUARE, 15);
    hold(0, 20);
}

/* ---- checks over a run ---- */
static int rules_kept(void)
{
    return !session->free_while_drawing && !session->free_in_flight && !session->stale_fields &&
           !session->drawn_after_free && !session->double_free && !session->pool_empty &&
           !session->decode_errors && !session->live_at_exit;
}

#ifndef VJO_SETTINGS_BENCH
static void check_rules(void)
{
    TEST_CHECK_(rules_kept(), "freed while drawing %d, in flight %d, stale fields %d, drawn after free %d, "
                "double free %d, pool empty %d, decode errors %d, alive at exit %d",
                session->free_while_drawing, session->free_in_flight, session->stale_fields,
                session->drawn_after_free, session->double_free, session->pool_empty, session->decode_errors,
                session->live_at_exit);
    TEST_CHECK_(session->max_live <= 9 && session->max_cdram <= 9L * CDRAM_BLOCK, "at most 9 icons: %d, %ld B",
                session->max_live, session->max_cdram);
}
/* Every card of every frame: its ID's game, its title rows as the old
 * wrapped_text laid them out, and only that game's icon or the placeholder. */
static int check_cards(int frames, int *placeholders_out)
{
    int bad = 0, placeholders = 0;
    ref_measure = stub_width;
    for (int f = 0; f < frames; f++) {
        const Frame *s = &session->shown[f];
        for (int r = 0; r < ROWS; r++) {
            if (!s->id[r][0]) continue;
            const Game *g = game_of(s->id[r]);
            if (!g) { if (bad++ < 5) TEST_MSG("frame %d card %d: unknown ID %s", f, r, s->id[r]); continue; }
            ref_wrapped_text(120, GAMES_Y + r * GAME_PITCH + 30, 620, 26, 32, 2, g->title);
            for (int k = 0; k < 2; k++) {
                const char *want = k < ref_rows ? ref_row[k].text : "";
                if (strcmp(s->title[r][k], want) && bad++ < 5)
                    TEST_MSG("frame %d card %d row %d: \"%s\", want \"%s\"", f, r, k, s->title[r][k], want);
            }
            int expect_icon = g->icon == ICON_OK || g->icon == ICON_RGBA || g->icon == ICON_SMALL;
            if (!strcmp(s->icon[r], "JP")) placeholders += expect_icon;
            else if (!expect_icon || strcmp(s->icon[r], g->id)) {
                if (bad++ < 5) TEST_MSG("frame %d card %d (%s): icon \"%s\"", f, r, g->id, s->icon[r]);
            }
        }
    }
    if (placeholders_out) *placeholders_out = placeholders;
    return bad;
}
static int max_loads(int from, int to)
{
    int m = 0;
    for (int f = from; f < to; f++) if (session->shown[f].loads > m) m = session->shown[f].loads;
    return m;
}
static int sum_loads(int from, int to)
{
    int n = 0;
    for (int f = from; f < to; f++) n += session->shown[f].loads;
    return n;
}
/* The list position each frame shows: the top card's and the focused card's indexes. */
static int top_of(int f) { return session->shown[f].id[0][0] ? list_index(session->shown[f].id[0]) : -1; }
static int selected_of(int f)
{
    const Frame *s = &session->shown[f];
    return s->focus >= 0 && s->id[s->focus][0] ? list_index(s->id[s->focus]) : -1;
}

/* The window main.c keeps icons for: the visible rows and a page above and below. */
static int in_window(int f, int index)
{
    int top = top_of(f);
    return top >= 0 && index >= top - ROWS && index < top + 2 * ROWS;
}
/* No icon is loaded again unless it left the window in between. */
static int window_never_reloaded(int frames)
{
    int bad = 0;
    for (int f = 0; f < frames; f++) {
        if (!session->shown[f].loaded[0]) continue;
        int index = list_index(session->shown[f].loaded), prev = -1;
        for (int g = f - 1; g >= 0 && prev < 0; g--)
            if (!strcmp(session->shown[g].loaded, session->shown[f].loaded)) prev = g;
        if (prev < 0) continue;
        int left = 0;
        for (int g = prev; g < f && !left; g++) left = !in_window(g, index);
        if (!left && bad++ < 5) TEST_MSG("frame %d reloads %s, loaded at %d and in the window since", f,
                                         session->shown[f].loaded, prev);
    }
    return bad == 0;
}
/* The loads of frames [from, to] go in main.c's order for a list last moved
 * down (direction 1) or up: the visible rows, the page that way, then the
 * other page, nearest first; an icon that is cached or broken is skipped. */
static int loads_in_order(int from, int to, int direction)
{
    int top = top_of(to), want[3 * ROWS], w = 0, last = -1, bad = 0;
    for (int r = 0; r < ROWS; r++) want[w++] = top + r;
    for (int side = 0; side < 2; side++)
        for (int r = 0; r < ROWS; r++)
            want[w++] = (side == 0) == (direction > 0) ? top + ROWS + r : top - 1 - r;
    for (int f = from; f <= to; f++) {
        if (!session->shown[f].loaded[0]) continue;
        int index = list_index(session->shown[f].loaded), pos = -1;
        for (int q = 0; q < w; q++) if (want[q] == index) pos = q;
        if ((pos < 0 || pos < last) && bad++ < 5)
            TEST_MSG("frame %d loads entry %d (top %d) out of order", f, index, top);
        if (pos > last) last = pos;
    }
    return bad == 0;
}
/* A frame that loads a row outside the view has every loadable visible icon already. */
static int visible_first(int frames)
{
    int bad = 0;
    for (int f = 0; f < frames; f++) {
        const Frame *s = &session->shown[f];
        int index = s->loaded[0] ? list_index(s->loaded) : -1, top = top_of(f);
        if (index < 0 || (index >= top && index < top + ROWS)) continue;
        for (int r = 0; r < ROWS; r++) {
            const Game *g = s->id[r][0] ? game_of(s->id[r]) : NULL;
            if (g && !strcmp(s->icon[r], "JP") && (g->icon == ICON_OK || g->icon == ICON_RGBA || g->icon == ICON_SMALL)
                && bad++ < 5)
                TEST_MSG("frame %d loads %s with card %d (%s) still a placeholder", f, s->loaded, r, g->id);
        }
    }
    return bad == 0;
}

static void idle_window_fills_then_costs_nothing(void)
{
    card(60, kind_mixed);
    spec_list();
    scenario_idle();
    int frames = run(at);
    TEST_CHECK(frames == at);
    check_rules();
    TEST_CHECK(check_cards(frames, NULL) == 0);
    /* The window: the three cards and the page below (nothing is above the top). */
    TEST_CHECK(session->shown[0].scans == 1 && sum_loads(0, frames) <= 6 && max_loads(0, frames) <= 1);
    /* The visible icons come first: one a frame, so from the third frame on every card has its icon. */
    for (int f = 2; f < frames; f++)
        for (int r = 0; r < ROWS; r++) TEST_CHECK_(strcmp(session->shown[f].icon[r], "JP") ||
            game_of(session->shown[f].id[r])->icon != ICON_OK, "frame %d card %d placeholder", f, r);
    /* Steady state: no loads, no reads, no layout; only short labels are measured. */
    int glyphs = 0;
    for (int f = 30; f < frames; f++) {
        const Frame *s = &session->shown[f];
        TEST_CHECK(!s->loads && !s->icon_opens && !s->icon_reads && !s->scans && !s->waits);
        if (s->glyphs > glyphs) glyphs = s->glyphs;
    }
    TEST_CHECK_(glyphs <= 120, "%d glyphs measured in an idle frame", glyphs);
    TEST_MSG("idle frame: at most %d glyphs measured; %d icon loads in all", glyphs, sum_loads(0, frames));
}

static void scrolling_loads_one_icon_a_frame_and_never_reloads_the_window(void)
{
    card(60, kind_mixed);
    spec_list();
    scenario_scroll();
    int frames = run(at), placeholders = 0;
    check_rules();
    TEST_CHECK(check_cards(frames, NULL) == 0);
    TEST_CHECK(max_loads(0, frames) <= 1);
    /* Once filled, scrolling at the repeat rate never shows a placeholder for a loadable icon. */
    for (int f = 20; f < frames; f++)
        for (int r = 0; r < ROWS; r++) {
            const Frame *s = &session->shown[f];
            const Game *g = s->id[r][0] ? game_of(s->id[r]) : NULL;
            if (g && !strcmp(s->icon[r], "JP") && (g->icon == ICON_OK || g->icon == ICON_RGBA || g->icon == ICON_SMALL))
                placeholders++;
        }
    TEST_CHECK_(placeholders == 0, "%d placeholders while scrolling", placeholders);
    /* It moved about 30 rows down and back to the top. */
    int deepest = 0;
    for (int f = 0; f < frames; f++) if (top_of(f) > deepest) deepest = top_of(f);
    TEST_CHECK(deepest >= 25 && top_of(frames - 1) == 0 && selected_of(frames - 1) == 0);
    /* Each step loads the one row entering the window: about one load a row each way. */
    int loads = sum_loads(0, frames);
    TEST_CHECK_(loads <= 2 * deepest + 3 * ROWS, "%d loads for %d rows down and back", loads, deepest);
    TEST_CHECK(window_never_reloaded(frames));
    TEST_MSG("%d frames, %d rows deep, %d icon loads", frames, deepest, loads);
}

/* Scroll the view 3 rows down and back: the visible cards' icons are never loaded again. */
static void scroll_back_keeps_visible_icons(void)
{
    card(60, NULL);
    spec_list();
    at = 0;
    hold(0, 15);
    for (int i = 0; i < 12; i++) tap(DOWN, 8); /* selection 12 on the bottom card: the view's top is 10 */
    hold(0, 20);
    int mark = at - 1; /* the last frame before the taps */
    for (int i = 0; i < 3; i++) tap(DOWN, 8); /* the view to 13 */
    for (int i = 0; i < 5; i++) tap(UP, 8);   /* the selection to 10, the view back to 10 */
    hold(0, 10);
    int frames = run(at);
    check_rules();
    TEST_CHECK(check_cards(frames, NULL) == 0);
    TEST_CHECK(top_of(mark) == top_of(frames - 1) && top_of(mark) > 3);
    /* After the mark: only rows entering the window's far edges load, never a visible one,
     * and no card shows the placeholder. */
    int loads = sum_loads(mark, frames);
    TEST_CHECK_(loads <= 6, "%d loads moving 3 rows and back", loads);
    for (int f = mark; f < frames; f++) {
        const Frame *s = &session->shown[f];
        int index = s->loaded[0] ? list_index(s->loaded) : -1;
        TEST_CHECK(index < 0 || index < top_of(f) || index >= top_of(f) + ROWS);
        for (int r = 0; r < ROWS; r++) TEST_CHECK(strcmp(s->icon[r], "JP"));
    }
    TEST_CHECK(window_never_reloaded(frames));
}

static void page_jumps_keep_the_row_and_prefetch(void)
{
    card(60, NULL);
    spec_list();
    scenario_pages();
    int frames = run(at);
    check_rules();
    TEST_CHECK(check_cards(frames, NULL) == 0);
    TEST_CHECK(max_loads(0, frames) <= 1);
    /* Each R moves the view a page, the selection keeps its row, and the new page was
     * already loaded: no placeholder after the first fill. */
    int f = 20, top = top_of(19), row = selected_of(19) - top, placeholders = 0;
    TEST_CHECK(top == 0 && row == 0);
    for (int i = 0; i < 8; i++, f += 16) {
        TEST_CHECK_(top_of(f) == top + 3 * (i + 1), "R %d: top %d", i, top_of(f));
        TEST_CHECK(selected_of(f) - top_of(f) == row);
    }
    for (int i = 0; i < 8; i++, f += 16) TEST_CHECK_(top_of(f) == top + 3 * (7 - i), "L %d: top %d", i, top_of(f));
    /* R held: pages repeat until the last page, then the last game is selected. */
    TEST_CHECK(top_of(frames - 1) == list_count - ROWS && selected_of(frames - 1) == list_count - 1);
    for (int g = 20; g < frames; g++)
        for (int r = 0; r < ROWS; r++) placeholders += !strcmp(session->shown[g].icon[r], "JP") &&
                                                       strcmp(session->shown[g].id[r], "NPXS10028");
    TEST_CHECK_(placeholders == 0, "%d placeholders while paging", placeholders);
    /* The hints name the new controls, and the counter is drawn apart. */
    TEST_CHECK(!strcmp(session->shown[25].hints, "×: toggle   L/R: page   Left/Right: letter   ○: exit"));
    char counter[24];
    snprintf(counter, sizeof(counter), "%d/%d", selected_of(25) + 1, list_count);
    TEST_CHECK(!strcmp(session->shown[25].counter, counter));
}

static void page_jumps_at_the_ends(void)
{
    card(7, NULL); /* 8 entries with Adrenaline's: 2 pages and 2 rows */
    spec_list();
    at = 0;
    hold(0, 12);
    tap(DOWN, 12);                   /* selection on row 1 */
    tap(SCE_CTRL_RTRIGGER, 12);      /* top 3, row 1 */
    int a = at - 1;
    tap(SCE_CTRL_RTRIGGER, 12);      /* top 5 (the last page), row 1 kept: 6 */
    int b = at - 1;
    tap(SCE_CTRL_RTRIGGER, 12);      /* cannot move: the last game */
    int c = at - 1;
    tap(SCE_CTRL_LTRIGGER, 12);      /* top 2, row 2 */
    int d = at - 1;
    tap(SCE_CTRL_LTRIGGER, 12); tap(SCE_CTRL_LTRIGGER, 12); /* top 0, then the first game */
    int e = at - 1;
    int frames = run(at);
    check_rules();
    TEST_CHECK(frames == at);
    TEST_CHECK_(top_of(a) == 3 && selected_of(a) == 4, "a: %d %d", top_of(a), selected_of(a));
    TEST_CHECK_(top_of(b) == 5 && selected_of(b) == 6, "b: %d %d", top_of(b), selected_of(b));
    TEST_CHECK_(top_of(c) == 5 && selected_of(c) == 7, "c: %d %d", top_of(c), selected_of(c));
    TEST_CHECK_(top_of(d) == 2 && selected_of(d) == 4, "d: %d %d", top_of(d), selected_of(d));
    TEST_CHECK_(top_of(e) == 0 && selected_of(e) == 0, "e: %d %d", top_of(e), selected_of(e));
    /* Fewer games than a page: L and R go to the first and last. */
    card(1, NULL);
    spec_list();
    at = 0;
    hold(0, 5); tap(SCE_CTRL_RTRIGGER, 5);
    a = at - 1;
    tap(SCE_CTRL_LTRIGGER, 5);
    b = at - 1;
    run(at);
    TEST_CHECK(top_of(a) == 0 && selected_of(a) == 1 && selected_of(b) == 0);
}

static void letter_jumps_go_to_the_next_initial(void)
{
    card(120, NULL); /* every edge initial of make_title */
    spec_list();
    /* The expected stops: from the top, Right goes to the first entry of the next group. */
    int stops[MAX_GAMES + 1], n = 0, i = 0;
    while (i < list_count - 1) {
        uint32_t g = spec_group(list[i].title);
        while (i < list_count && spec_group(list[i].title) == g) i++;
        if (i == list_count) i = list_count - 1;
        stops[n++] = i;
    }
    at = 0;
    hold(0, 12);
    int when[MAX_GAMES + 1];
    for (int k = 0; k < n + 2; k++) { tap(SCE_CTRL_RIGHT, 8); when[k] = at - 1; }
    /* Back with Left: to each group's first entry, from the last game's group. */
    int back_when[MAX_GAMES + 1], m = 0;
    for (int k = 0; k < n + 2; k++) { tap(SCE_CTRL_LEFT, 8); back_when[m++] = at - 1; }
    int frames = run(at);
    check_rules();
    TEST_CHECK(check_cards(frames, NULL) == 0);
    TEST_CHECK(max_loads(0, frames) <= 1);
    for (int k = 0; k < n; k++) {
        TEST_CHECK_(selected_of(when[k]) == stops[k], "Right %d: %d, want %d", k, selected_of(when[k]), stops[k]);
        /* shown at the top, unless near the end */
        int top = stops[k] < list_count - ROWS ? stops[k] : list_count - ROWS;
        TEST_CHECK(top_of(when[k]) == top);
    }
    TEST_CHECK(selected_of(when[n + 1]) == list_count - 1);
    /* After a jump right, the loads go: the visible rows, the page below, the page
     * above, nearest first, each as it is needed. */
    int order_ok = 1;
    for (int k = 0; k < n; k++) order_ok &= loads_in_order(when[k] - 8, when[k], 1);
    TEST_CHECK(order_ok);
    TEST_CHECK(visible_first(frames));
    int sel = list_count - 1;
    for (int k = 0; k < m; k++) {
        uint32_t g = spec_group(list[sel].title);
        int first = sel;
        while (first > 0 && spec_group(list[first - 1].title) == g) first--;
        if (first == sel && sel > 0) {
            g = spec_group(list[--first].title);
            while (first > 0 && spec_group(list[first - 1].title) == g) first--;
        }
        TEST_CHECK_(selected_of(back_when[k]) == first, "Left %d: %d, want %d", k, selected_of(back_when[k]), first);
        sel = first;
    }
    TEST_CHECK(selected_of(frames - 1) == 0);
    TEST_MSG("%d groups among %d entries", n + 1, list_count);
    /* Left from inside a letter goes to that letter's first game, not the previous letter's. */
    int first = -1; /* the first game of a letter with two or more games, not the list's first letter */
    for (int i = 1; i + 1 < list_count && first < 0; i++)
        if (spec_group(list[i].title) != spec_group(list[i - 1].title) &&
            spec_group(list[i + 1].title) == spec_group(list[i].title)) first = i;
    TEST_ASSERT(first > 0);
    at = 0;
    hold(0, 12);
    int k = 0;
    for (int i = 0; i < first; i = stops[k++]) tap(SCE_CTRL_RIGHT, 8); /* Right to that letter */
    int at_first = at - 1;
    tap(DOWN, 8);
    int inside = at - 1;
    tap(SCE_CTRL_LEFT, 8);
    int back = at - 1;
    tap(SCE_CTRL_LEFT, 8);
    int prev = at - 1;
    run(at);
    int prev_first = first - 1;
    while (prev_first > 0 && spec_group(list[prev_first - 1].title) == spec_group(list[first - 1].title)) prev_first--;
    TEST_CHECK_(selected_of(at_first) == first && selected_of(inside) == first + 1, "at %d, inside %d, want %d, %d",
                selected_of(at_first), selected_of(inside), first, first + 1);
    TEST_CHECK_(selected_of(back) == first, "Left inside: %d, want %d", selected_of(back), first);
    TEST_CHECK_(selected_of(prev) == prev_first, "Left again: %d, want %d", selected_of(prev), prev_first);
    /* Lower case is listed with its letter: every group is one run. */
    int runs = 1;
    for (int k = 1; k < list_count; k++) runs += spec_group(list[k].title) != spec_group(list[k - 1].title);
    uint32_t seen[MAX_GAMES + 1];
    int distinct = 0;
    for (int k = 0; k < list_count; k++) {
        uint32_t g = spec_group(list[k].title);
        int found = 0;
        for (int q = 0; q < distinct; q++) found |= seen[q] == g;
        if (!found) seen[distinct++] = g;
    }
    TEST_CHECK_(runs == distinct, "%d runs, %d groups", runs, distinct);
}

/* After a move up (Up at the top card, L, Left) the list's direction of travel is
 * up: Start's reload, which finds no icon cached, goes visible rows, then the page
 * above, then the page below, nearest first. */
static void moving_up_prefetches_the_page_above(void)
{
    static const unsigned keys[] = {UP, SCE_CTRL_LTRIGGER, SCE_CTRL_LEFT};
    static const char *const names[] = {"Up", "L", "Left"};
    for (int k = 0; k < 3; k++) {
        card(60, NULL);
        spec_list();
        at = 0;
        hold(0, 15);
        for (int i = 0; i < 6; i++) tap(SCE_CTRL_RTRIGGER, 12); /* down 6 pages: the view's top is 18 */
        tap(keys[k], 12);                                        /* one move up */
        tap(SCE_CTRL_START, 14);                                 /* the reload */
        int start = at - 15, frames = run(at), top = top_of(start);
        check_rules();
        TEST_CHECK(frames == at);
        TEST_CHECK_(top >= ROWS && top + 2 * ROWS <= list_count && top < 18, "%s: top %d", names[k], top);
        TEST_CHECK_(sum_loads(start, start + 3 * ROWS) == 3 * ROWS, "%s: %d loads", names[k],
                    sum_loads(start, start + 3 * ROWS));
        TEST_CHECK_(loads_in_order(start, start + 3 * ROWS - 1, -1), "%s", names[k]);
    }
    /* and after a move down, the page below first (the same reload) */
    card(60, NULL);
    spec_list();
    at = 0;
    hold(0, 15);
    for (int i = 0; i < 6; i++) tap(SCE_CTRL_RTRIGGER, 12);
    tap(SCE_CTRL_START, 14);
    int start = at - 15;
    run(at);
    TEST_CHECK(loads_in_order(start, start + 3 * ROWS - 1, 1) && sum_loads(start, start + 3 * ROWS) == 3 * ROWS);
}

/* A held list key repeats 350 ms after its press, then every 90 ms (the process
 * clock: 16,667 us a frame); a key pressed while another is held acts at once. */
static void held_keys_repeat_and_new_presses_act(void)
{
    card(60, NULL);
    spec_list();
    at = 0;
    hold(0, 10);
    int p = at;
    hold(DOWN, 40);
    int q = at;
    hold(DOWN | SCE_CTRL_RTRIGGER, 4); /* R pressed with Down still held */
    hold(0, 10);
    int frames = run(at);
    TEST_CHECK(frames == at);
    /* The expected selection, frame by frame, from the rule. */
    int sel = 0, bad = 0;
    uint64_t repeat = 0;
    for (int f = p; f < q + 4; f++) {
        uint64_t now = (uint64_t)f * 16667u;
        if (f == p) { sel++; repeat = now + 350000; }
        else if (f == q) { sel += ROWS; repeat = now + 350000; } /* a page; the row is kept */
        else if (now >= repeat) { sel++; repeat = now + 90000; } /* held together, Down repeats */
        if (selected_of(f) != sel && bad++ < 5) TEST_MSG("frame %d: selection %d, want %d", f - p, selected_of(f), sel);
    }
    TEST_CHECK(bad == 0);
    TEST_CHECK_(selected_of(q - 1) == 5 && selected_of(q) == 8 && top_of(q) == top_of(q - 1) + ROWS,
                "Down held 40 frames: %d; then R: %d", selected_of(q - 1), selected_of(q));
}

/* The left stick moves the selection as VitaShell's lists do (core/stick_scroll.h):
 * a row on the push, nine quiet frames, then a row every frame; a push under
 * half way does nothing; the d-pad acts first. */
static void stick_scrolls_like_vitashell(void)
{
    card(60, NULL);
    spec_list();
    at = 0;
    hold(0, 10);
    hold_stick(64, 20); /* 64 from the centre: not a push */
    int p = at;
    hold_stick(127, 40);
    hold(0, 10);
    int q = at;
    hold_stick(-128, 15);
    hold(0, 10);
    int r = at;
    hold(DOWN, 1);
    for (int i = 0; i < 4; i++) script[at++] = (Input){DOWN, 0, 0, 0, 127}; /* Down held with the stick */
    hold(0, 10);
    int frames = run(at);
    TEST_CHECK(frames == at);
    TEST_CHECK(selected_of(p - 1) == 0);
    int sel = 0, bad = 0;
    for (int f = p; f < p + 40; f++) {
        if (f == p || f >= p + 10) sel++;
        if (selected_of(f) != sel && bad++ < 5) TEST_MSG("frame %d: selection %d, want %d", f - p, selected_of(f), sel);
    }
    TEST_CHECK(bad == 0);
    TEST_CHECK_(selected_of(q - 1) == 31, "after 40 frames down: %d", selected_of(q - 1));
    TEST_CHECK_(selected_of(r - 1) == 25, "after 15 frames up: %d", selected_of(r - 1));
    /* Down pressed (one row), then held with the stick: the stick's push adds one,
     * the d-pad's repeat is not due for 350 ms */
    TEST_CHECK_(selected_of(r) == 26 && selected_of(r + 4) == 27, "%d, %d", selected_of(r), selected_of(r + 4));
}

/* The list keys act on the Games page only: on the OCR & lookup and Anki pages
 * Up/Down/L/R leave the list where it was. */
static void list_keys_only_on_the_games_page(void)
{
    card(30, NULL);
    spec_list();
    at = 0;
    hold(0, 10);
    tap(DOWN, 8);
    int before = at - 1;
    tap(SCE_CTRL_SELECT, 8); /* OCR & lookup */
    for (int i = 0; i < 3; i++) tap(DOWN, 8);
    tap(SCE_CTRL_RTRIGGER, 8);
    tap(SCE_CTRL_LTRIGGER, 8);
    hold(DOWN, 40);
    hold(0, 8);
    tap(SCE_CTRL_SELECT, 8); /* Anki */
    tap(DOWN, 8);
    tap(SCE_CTRL_RTRIGGER, 8);
    hold(UP, 30);
    hold(0, 8);
    tap(SCE_CTRL_SELECT, 8); /* Games again */
    int after = at - 1, frames = run(at);
    TEST_CHECK(frames == at);
    TEST_CHECK_(selected_of(before) == 1 && selected_of(after) == 1 && top_of(after) == 0, "selection %d -> %d, top %d",
                selected_of(before), selected_of(after), top_of(after));
}

/* The hints keep 140 px for the counter, which is right-aligned at the card edge:
 * with Latin glyphs wide enough that the hints need more than 912 - 140 px, they
 * are cut with "..." there and never reach the counter. */
static void hints_leave_room_for_the_counter(void)
{
    for (int wide = 0; wide < 2; wide++) {
        ascii_units = wide ? 12 : 10;
        card(511, NULL); /* 512 entries: the counter's widest, "512/512" */
        spec_list();
        at = 0;
        hold(0, 10);
        hold(SCE_CTRL_RTRIGGER, 1100); /* to the last game */
        hold(0, 10);
        int frames = run(at);
        TEST_CHECK(frames == at);
        const Frame *last = &session->shown[frames - 1];
        float px = 24.0f / 18.0f; /* text_scale(24): the em is 18 units, full-width */
        int hints_w = (int)(stub_units(last->hints, &(int){0}) * px + 0.5f);
        int counter_w = (int)(stub_units(last->counter, &(int){0}) * px + 0.5f);
        TEST_CHECK_(!strcmp(last->counter, "512/512") && last->counter_x + counter_w == 936,
                    "wide %d: counter '%s' at %d, %d px", wide, last->counter, last->counter_x, counter_w);
        TEST_CHECK_(hints_w <= 912 - 140 && 24 + hints_w < last->counter_x, "wide %d: hints %d px to %d, counter at %d",
                    wide, hints_w, 24 + hints_w, last->counter_x);
        size_t n = strlen(last->hints);
        int cut = n > 3 && !strcmp(last->hints + n - 3, "...");
        TEST_CHECK_(cut == wide, "wide %d: hints '%s'", wide, last->hints);
    }
    ascii_units = 10;
}

/* Titles of the window beyond the visible rows are laid out one a frame, when no
 * icon is to be loaded; a view move then lays out no title while it draws. */
TEST_ONLY static int kind_none(int i) { (void)i; return ICON_NONE; }
static long layout_glyphs(const char *title)
{
    counted = 0;
    ref_measure = counting_width;
    ref_wrapped_text(120, 0, 620, 26, 32, 2, title);
    return counted;
}
static void titles_are_laid_out_ahead_one_a_frame(void)
{
    card(60, kind_none); /* no icons: each window entry's attempt takes one frame, then fails */
    spec_list();
    at = 0;
    hold(0, 30);
    tap(DOWN, 10); tap(DOWN, 10); /* the selection to the third card: no view move */
    tap(DOWN, 10);                /* the view moves one row */
    int move = at - 11;
    int frames = run(at);
    TEST_CHECK(frames == at);
    long idle = session->shown[25].glyphs, bad = 0;
    /* frames 1-5: one icon attempt each (the visible titles were laid out by frame 0's draw) */
    for (int f = 1; f < 2 * ROWS; f++)
        if (session->shown[f].glyphs != idle && bad++ < 5) TEST_MSG("frame %d: %d glyphs, idle %ld", f, session->shown[f].glyphs, idle);
    /* frames 6-8: the page below's three titles, one a frame */
    for (int f = 2 * ROWS; f < 3 * ROWS; f++) {
        long want = idle + layout_glyphs(list[ROWS + f - 2 * ROWS].title);
        if (session->shown[f].glyphs != want && bad++ < 5) TEST_MSG("frame %d: %d glyphs, want %ld", f, session->shown[f].glyphs, want);
    }
    for (int f = 3 * ROWS; f < 30; f++)
        if (session->shown[f].glyphs != idle && bad++ < 5) TEST_MSG("frame %d: %d glyphs, idle %ld", f, session->shown[f].glyphs, idle);
    TEST_CHECK(bad == 0);
    /* The move's frame: one icon attempt (the row entering the window), and no title
     * layout (the counter's digits aside); the next frame lays out that row's title. */
    TEST_CHECK_(top_of(move) == 1 && top_of(move - 1) == 0, "top %d", top_of(move));
    long move_glyphs = session->shown[move].glyphs, next = session->shown[move + 1].glyphs;
    TEST_CHECK_(move_glyphs <= idle + 2, "the move's frame measured %ld glyphs, idle %ld", move_glyphs, idle);
    TEST_CHECK_(next - move_glyphs == layout_glyphs(list[2 * ROWS].title), "the next frame: %ld more glyphs, want %ld",
                next - move_glyphs, layout_glyphs(list[2 * ROWS].title));
}

static void toggles_and_filters_keep_icons(void)
{
    card(60, NULL);
    spec_list();
    scenario_toggle();
    int frames = run(at);
    check_rules();
    TEST_CHECK(check_cards(frames, NULL) == 0);
    /* Toggling saves games.ini and rebuilds the list; the icons in the window stay. */
    TEST_CHECK_(sum_loads(20, TOGGLE_FRAMES) == 0, "%d loads while toggling", sum_loads(20, TOGGLE_FRAMES));
    TEST_CHECK(!strncmp(session->shown[TOGGLE_FRAMES - 1].status[0], "Saved.", 6));
    /* The filters show other lists: Enabled lacks the two games, Disabled is them. */
    int enabled = TOGGLE_FRAMES + 15, disabled = enabled + 16, all = disabled + 16;
    TEST_CHECK(selected_of(enabled) == 2 && top_of(enabled) == 2);
    TEST_CHECK(!strcmp(session->shown[disabled].id[0], list[0].id) && !strcmp(session->shown[disabled].id[1], list[1].id)
               && !session->shown[disabled].id[2][0]);
    TEST_CHECK(top_of(all) == 0);
    TEST_CHECK(max_loads(0, frames) <= 1 && visible_first(frames));
    TEST_CHECK_(sum_loads(TOGGLE_FRAMES, frames) <= 2, "%d loads through the filters", sum_loads(TOGGLE_FRAMES, frames));
}

/* A console that enters with ○ (Japan's default): ○ toggles, × leaves, and the
 * hints say so; the same toggles as on a × console. */
static void circle_console_toggles_with_circle(void)
{
    button_assign = VJO_BUTTON_ASSIGN_CIRCLE;
    card(60, NULL);
    spec_list();
    scenario_toggle();
    int frames = run(at);
    check_rules();
    TEST_CHECK(!strncmp(session->shown[TOGGLE_FRAMES - 1].status[0], "Saved.", 6));
    int enabled = TOGGLE_FRAMES + 15, disabled = enabled + 16;
    TEST_CHECK(selected_of(enabled) == 2 && top_of(enabled) == 2);
    TEST_CHECK(!strcmp(session->shown[disabled].id[0], list[0].id) && !strcmp(session->shown[disabled].id[1], list[1].id)
               && !session->shown[disabled].id[2][0]);
    TEST_CHECK(!strcmp(session->shown[25].hints, "○: toggle   L/R: page   Left/Right: letter   ×: exit"));
    TEST_MSG("%s", session->shown[25].hints);
    TEST_CHECK(session->exited == 0 && frames < MAX_FRAMES - 1); /* × at the script's end left the app */
    button_assign = VJO_BUTTON_ASSIGN_CROSS;
}

static void broken_icons_show_the_placeholder_once_tried(void)
{
    card(60, kind_mixed);
    spec_list();
    scenario_scroll();
    int frames = run(at);
    check_rules();
    TEST_CHECK(check_cards(frames, NULL) == 0);
    /* Each broken or missing icon is tried once per scan, however often it is shown. */
    int broken_shown = 0;
    for (int f = 0; f < frames; f++)
        for (int r = 0; r < ROWS; r++) {
            const Frame *s = &session->shown[f];
            const Game *g = s->id[r][0] ? game_of(s->id[r]) : NULL;
            if (g && g->icon != ICON_OK && g->icon != ICON_RGBA && g->icon != ICON_SMALL) {
                TEST_CHECK(!strcmp(s->icon[r], "JP"));
                broken_shown++;
            }
        }
    TEST_CHECK(broken_shown > 100);
    /* In the window's reach, a broken or missing icon file was opened once, though the
     * scroll passed it twice; a good one once or twice (evicted going down, loaded again
     * coming back). None beyond the window's reach was opened. */
    int deepest = 0;
    for (int f = 0; f < frames; f++) if (top_of(f) > deepest) deepest = top_of(f);
    int reach = deepest + 2 * ROWS - 1, broken_tried = 0;
    for (int i = 0; i < game_count; i++) {
        int index = list_index(games[i].id), n = session->attempts[i];
        int broken = games[i].icon != ICON_OK && games[i].icon != ICON_RGBA && games[i].icon != ICON_SMALL;
        int ok = index > reach ? n == 0 : broken ? n == 1 : n == 1 || n == 2;
        TEST_CHECK_(ok, "%s (entry %d, kind %d, reach %d): %d opens", games[i].id, index, games[i].icon, reach, n);
        broken_tried += broken && index <= reach;
    }
    TEST_CHECK_(broken_tried >= 3, "%d broken icons in reach", broken_tried);
    TEST_CHECK(visible_first(frames) && window_never_reloaded(frames));
}

/* While the app runs, a game is installed that sorts first: the list moves by one
 * only when Start rescans, and then every card's title rows and icon are its own
 * (layouts and failures are kept by list position, icons by ID: a rescan drops all). */
#define NEW_GAME_FRAME 60
static void install_new_game(int f)
{
    if (f != NEW_GAME_FRAME) return;
    Game g = {"VJOT09999", "0 New Synthetic Game", ICON_OK};
    install(&g, 9999);
}
TEST_ONLY static int kind_every_third(int i) { return i % 3 == 1 ? ICON_DAMAGED : ICON_OK; }
static int loadable_kind(int kind) { return kind == ICON_OK || kind == ICON_RGBA || kind == ICON_SMALL; }
static void rescan_only_on_start(void)
{
    card(60, kind_every_third);
    Game before[MAX_GAMES + 1]; /* the list before the rescan */
    spec_list();
    memcpy(before, list, sizeof(Game) * (size_t)list_count);
    /* the list after the rescan: the new game, then Adrenaline's entry */
    snprintf(games[game_count].id, sizeof(games[0].id), "VJOT09999");
    snprintf(games[game_count].title, sizeof(games[0].title), "0 New Synthetic Game");
    games[game_count].icon = ICON_OK;
    game_count++;
    snprintf(games[game_count].id, sizeof(games[0].id), "NPXS10028");
    snprintf(games[game_count].title, sizeof(games[0].title), "Adrenaline / PSP games (shared setting)");
    games[game_count].icon = ICON_NONE;
    spec_list();
    frame_hook = install_new_game;
    at = 0;
    hold(0, 20);
    hold(DOWN, 60);  /* 8 rows down: the window has a page above and below */
    hold(0, 30);
    tap(SCE_CTRL_START, 40);
    int start = at - 41;
    hold(0, 10);
    int frames = run(at);
    check_rules();
    int scans = 0;
    for (int f = 0; f < frames; f++) scans += session->shown[f].scans;
    TEST_CHECK_(scans == 2, "%d catalog scans", scans); /* at start and on Start only */
    TEST_CHECK(session->shown[0].scans == 1 && session->shown[start].scans == 1);
    /* Before Start the new game is not listed. */
    for (int f = 0; f < start; f++)
        for (int r = 0; r < ROWS; r++) TEST_CHECK(strcmp(session->shown[f].id[r], "VJOT09999"));
    /* Start empties the cache after waiting for the GPU, then refills one a frame:
     * the visible rows, then the page below (the list last moved down), then above. */
    TEST_CHECK(session->shown[start].waits >= 1 && max_loads(0, frames) <= 1);
    int loadable = 0, top = top_of(start);
    for (int i = top - ROWS; i < top + 2 * ROWS; i++)
        loadable += i >= 0 && i < list_count && (list[i].icon == ICON_OK || list[i].icon == ICON_RGBA ||
                                                 list[i].icon == ICON_SMALL);
    TEST_CHECK_(sum_loads(start, frames) == loadable, "%d loads after Start, %d loadable icons in the window",
                sum_loads(start, frames), loadable);
    TEST_CHECK(top > ROWS && loads_in_order(start, start + 3 * ROWS + 2, 1));
    /* The new game sorts first, so each later entry moved down one place. The case
     * that per-place state (failures, layouts) must not survive: a place in the
     * window whose old entry's icon was broken, and was tried, now holds a good one. */
    int moved = 0;
    for (int i = top - ROWS; i < top + 2 * ROWS; i++)
        moved += i >= 1 && i < list_count && !strcmp(list[0].id, "VJOT09999") && !loadable_kind(before[i].icon) &&
                 loadable_kind(list[i].icon);
    TEST_CHECK_(moved >= 1, "no window place went from a broken icon to a good one");
    /* From the rescan on, every card shows its own game's rows and icon. */
    int bad = 0;
    ref_measure = stub_width;
    for (int f = start; f < frames; f++)
        for (int r = 0; r < ROWS; r++) {
            const Frame *sh = &session->shown[f];
            const Game *g = sh->id[r][0] ? game_of(sh->id[r]) : NULL;
            if (!g) continue;
            ref_wrapped_text(120, 0, 620, 26, 32, 2, g->title);
            if (strcmp(sh->title[r][0], ref_rows > 0 ? ref_row[0].text : "") ||
                strcmp(sh->title[r][1], ref_rows > 1 ? ref_row[1].text : "")) bad++;
            int loadable = g->icon == ICON_OK || g->icon == ICON_RGBA || g->icon == ICON_SMALL;
            if (f >= start + 3 * ROWS && loadable != !strcmp(sh->icon[r], g->id)) bad++;
        }
    TEST_CHECK_(bad == 0, "%d wrong cards after the rescan", bad);
}

static void many_games_bounded(void)
{
    card(511, NULL); /* with Adrenaline's entry: the 512 the catalog holds */
    spec_list();
    at = 0;
    hold(0, 15);
    hold(SCE_CTRL_RTRIGGER, 1100); /* 170 pages to the end, one every 6 frames */
    hold(0, 15);
    tap(SCE_CTRL_LEFT, 10); tap(SCE_CTRL_LEFT, 10);
    hold(SCE_CTRL_LTRIGGER, 1100);
    hold(0, 15);
    int frames = run(at);
    check_rules();
    TEST_CHECK(check_cards(frames, NULL) == 0);
    TEST_CHECK(max_loads(0, frames) <= 1);
    int deepest = 0;
    for (int f = 0; f < frames; f++) if (top_of(f) > deepest) deepest = top_of(f);
    TEST_CHECK(deepest == list_count - ROWS && top_of(frames - 1) == 0);
    TEST_MSG("%d entries, %d loads, at most %d icons (%ld B of CDRAM)", list_count, session->loads,
             session->max_live, session->max_cdram);
    /* More than 512: the first 512, and the message. */
    card(520, NULL);
    at = 0;
    hold(0, 5);
    frames = run(at);
    TEST_CHECK(!strcmp(session->shown[2].status[0], "Showing the first 512 installed titles."));
}

static void touch_toggles_the_touched_card(void)
{
    card(10, NULL);
    spec_list();
    at = 0;
    hold(0, 15);
    script[at++] = (Input){0, 1, 400, GAMES_Y + GAME_PITCH + 40, 0}; /* the second card */
    hold(0, 15);
    int frames = run(at);
    check_rules();
    TEST_CHECK(check_cards(frames, NULL) == 0);
    TEST_CHECK(selected_of(frames - 1) == 1);
    TEST_CHECK(!strncmp(session->shown[frames - 1].status[0], "Saved.", 6));
    TEST_CHECK(sum_loads(16, frames) == 0);
}

TEST_LIST = {
    {"idle_window_fills_then_costs_nothing", idle_window_fills_then_costs_nothing},
    {"scrolling_loads_one_icon_a_frame_and_never_reloads_the_window",
     scrolling_loads_one_icon_a_frame_and_never_reloads_the_window},
    {"scroll_back_keeps_visible_icons", scroll_back_keeps_visible_icons},
    {"page_jumps_keep_the_row_and_prefetch", page_jumps_keep_the_row_and_prefetch},
    {"page_jumps_at_the_ends", page_jumps_at_the_ends},
    {"letter_jumps_go_to_the_next_initial", letter_jumps_go_to_the_next_initial},
    {"moving_up_prefetches_the_page_above", moving_up_prefetches_the_page_above},
    {"held_keys_repeat_and_new_presses_act", held_keys_repeat_and_new_presses_act},
    {"list_keys_only_on_the_games_page", list_keys_only_on_the_games_page},
    {"stick_scrolls_like_vitashell", stick_scrolls_like_vitashell},
    {"hints_leave_room_for_the_counter", hints_leave_room_for_the_counter},
    {"titles_are_laid_out_ahead_one_a_frame", titles_are_laid_out_ahead_one_a_frame},
    {"toggles_and_filters_keep_icons", toggles_and_filters_keep_icons},
    {"circle_console_toggles_with_circle", circle_console_toggles_with_circle},
    {"broken_icons_show_the_placeholder_once_tried", broken_icons_show_the_placeholder_once_tried},
    {"rescan_only_on_start", rescan_only_on_start},
    {"many_games_bounded", many_games_bounded},
    {"touch_toggles_the_touched_card", touch_toggles_the_touched_card},
    {NULL, NULL}
};

#else /* VJO_SETTINGS_BENCH */
static int cmp_long(const void *a, const void *b)
{
    long x = *(const long *)a, y = *(const long *)b;
    return x < y ? -1 : x > y;
}
typedef struct { const char *name; void (*script)(void); int from; } Scenario;
typedef struct {
    int frames, loads, max_loads, reads, waits, max_live, kept, moves, move_loads;
    double mean, p50, p95, max, move_mean, move_max;
    long glyphs, max_glyphs, max_cdram;
    char rules[200];
} Stats;
static Stats stats_of(const Scenario *sc, int frames)
{
    static long ns[MAX_FRAMES];
    Stats st;
    long sum = 0, move_sum = 0;
    int n = 0;
    memset(&st, 0, sizeof(st));
    for (int f = sc->from; f < frames; f++) {
        const Frame *s = &session->shown[f];
        ns[n++] = s->ns;
        sum += s->ns;
        /* a frame that shows another first card: the view moved */
        if (strcmp(s->id[0], session->shown[f - 1].id[0])) {
            st.moves++;
            move_sum += s->ns;
            st.move_loads += s->loads;
            if (s->ns / 1e6 > st.move_max) st.move_max = s->ns / 1e6;
        }
        st.glyphs += s->glyphs;
        if (s->glyphs > st.max_glyphs) st.max_glyphs = s->glyphs;
        st.loads += s->loads;
        if (s->loads > st.max_loads) st.max_loads = s->loads;
        st.reads += s->icon_reads;
        st.waits += s->waits;
    }
    qsort(ns, (size_t)n, sizeof(ns[0]), cmp_long);
    st.frames = n;
    st.mean = sum / 1e6 / n;
    st.p50 = ns[n / 2] / 1e6;
    st.p95 = ns[n * 95 / 100] / 1e6;
    st.max = ns[n - 1] / 1e6;
    st.glyphs /= n;
    st.move_mean = st.moves ? move_sum / 1e6 / st.moves : 0;
    st.max_live = session->max_live;
    st.max_cdram = session->max_cdram;
    st.kept = rules_kept();
    snprintf(st.rules, sizeof(st.rules), "freed in flight %d, while drawing %d; uncleared fields %d; drawn after free %d; "
             "alive at exit %d", session->free_in_flight, session->free_while_drawing, session->stale_fields,
             session->drawn_after_free, session->live_at_exit);
    return st;
}
int main(int argc, char **argv)
{
    int games_n = argc > 1 ? atoi(argv[1]) : 100, repeats = argc > 2 ? atoi(argv[2]) : 5;
    const Scenario scenarios[] = {{"idle", scenario_idle, 30}, {"scroll", scenario_scroll, 20},
                                  {"pages", scenario_pages, 20}, {"toggle", scenario_toggle, 20}};
    double startup = 0;
    if (repeats < 1) repeats = 1;
    if (repeats > 15) repeats = 15;
    card(games_n, NULL);
    printf("%d games + Adrenaline's entry; icons 128x128 8-bit palette, %s; frames after the first %s; "
           "the run with the median mean of %d\n\n", games_n,
#ifdef VJO_TEST_ZLIB
           "zlib-compressed",
#else
           "stored blocks (no zlib)",
#endif
           "fill (idle: 30, others: 20)", repeats);
    printf("| scenario | frames | mean ms | p50 ms | p95 ms | max ms | view moves | ms a move (mean) | ms a move (max) | "
           "loads on a move | glyphs/frame | max glyphs | loads | max loads/frame | icon reads | GPU waits | max icons | "
           "max CDRAM KiB | GPU rules |\n");
    printf("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|\n");
    for (unsigned k = 0; k < sizeof(scenarios) / sizeof(scenarios[0]); k++) {
        Stats runs[15];
        for (int r = 0; r < repeats; r++) {
            scenarios[k].script();
            runs[r] = stats_of(&scenarios[k], run(at));
            if (!k && !r) startup = session->startup_ns / 1e6;
        }
        int median = 0;
        for (int r = 0; r < repeats; r++) {
            int below = 0;
            for (int q = 0; q < repeats; q++) below += runs[q].mean < runs[r].mean || (runs[q].mean == runs[r].mean && q < r);
            if (below == repeats / 2) median = r;
        }
        const Stats *st = &runs[median];
        printf("| %s | %d | %.4f | %.4f | %.4f | %.4f | %d | %.4f | %.4f | %.2f | %ld | %ld | %d | %d | %d | %d | %d | %ld | %s |\n",
               scenarios[k].name, st->frames, st->mean, st->p50, st->p95, st->max, st->moves, st->move_mean,
               st->move_max, st->moves ? (double)st->move_loads / st->moves : 0, st->glyphs, st->max_glyphs,
               st->loads, st->max_loads, st->reads, st->waits, st->max_live, st->max_cdram / 1024,
               st->kept ? "kept" : st->rules);
    }
    printf("\nstartup (the catalog scan of %d games, before the first frame): %.3f ms\n", games_n, startup);
    return 0;
}
#endif
