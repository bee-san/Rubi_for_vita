/* The Rubi app's Games page through its actual input and draw loop
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
#define main acutest_main
#include "acutest.h"
#undef main
#define TEST_ONLY
#else
/* Evaluated in every build (assert would vanish with NDEBUG). */
#define TEST_ASSERT(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); _exit(2); } } while (0)
#define TEST_ONLY __attribute__((unused)) /* the tests' helpers, unused by the benchmark */
#endif
#include "test_tmpdir.h"
#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/devctl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/registrymgr.h>
#include <psp2/touch.h>
#include <vita2d.h>
#include "../../core/buttons.h"
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../../core/games.h"
#include "../../settings/catalog.h"
#include "../../settings/anki_page.h" /* the app's log (settings/applog.c) */
#include "../../core/pspbubble.h"
#include "settings_png_fixture.h"
#include "settings_wrap_reference.h"
#include "settings_stubs/search_dialog_stub.h"
#include <psp2/ime_dialog.h>

int vjo_settings_main(void); /* settings/main.c's main(), renamed for the harness */

/* settings/main.c's Games page layout. */
#define ROWS 5
#define GAMES_Y 174
#define GAME_H 40
#define GAME_PITCH 44
#define HINT_Y 535
#define DEFAULT_BADGE_Y (112 + 8 + 38 / 2 + 9) /* badge(174, DEFAULT_Y + 8, 108, 38): its label's baseline */
#define STATUS_Y 467
#define SELECTED RGBA8(35, 56, 70, 255)
#define ACCENT RGBA8(91, 220, 207, 255)
#define CDRAM_BLOCK (256 * 1024)

/* ---- what a frame showed and cost ---- */
#define MAX_FRAMES 4200
typedef struct {
    char title[ROWS][2][128], id[ROWS][16], icon[ROWS][16]; /* icon: the texture's game, "JP", or "" */
    unsigned tag[ROWS];                                      /* a PSP card's tag: its color, 0 for none */
    char hook[ROWS][16];                                     /* API 31: a hook entry's tag ("Hook", "Luna hook") */
    char badge[ROWS][4], default_badge[4];                   /* each card's ON/OFF/--, the Default row's */
    char hints[128], counter[24], status[2][128], pages[32];
    int counter_x;                                           /* where the counter starts */
    char loaded[16];                                         /* the game whose icon this frame loaded (the last) */
    int focus;                                               /* the focused card, -1 */
    int bar_y;                                               /* the focus bar's y (any page), -1 */
    char bar_title[48];                                      /* that card's title (x 42, 30 below its top), "" */
    int anki_rows;                                           /* the stick's moves given to the Anki page so far */
    int loads, frees, waits, icon_opens, icon_reads, scans, glyphs, measures, live;
    long ns;
} Frame;
typedef struct {
    int frames, exited, main_rc, searches, search_terms;
    int budget_size, budget_calls; /* sceAppMgrGetBudgetInfo's info->size, and its calls */
    /* GPU rules broken, and other totals */
    int free_while_drawing, free_in_flight, stale_fields, drawn_after_free, double_free, pool_empty;
    int loads, live_at_exit, max_live, decode_errors;
    long max_cdram;
    long startup_ns;
    unsigned short attempts[MAX_FRAMES]; /* icon0.png opens tried, by game number (VJOTnnnnn) */
    int description_writes;              /* opens for writing in ux0:data/VitaJPOverlay/psp (settings/psp.c) */
    int log_appends;                     /* appends to log.txt (settings/applog.c) */
    Frame shown[MAX_FRAMES];
} Session;
static Session *session;

/* ---- storage: the SDK's file calls over real files ---- */
static int native(int n) { return n < 0 ? (int)(0x80010000u | (unsigned)errno) : n; }
static Frame cur;
static int icon_fd[1024];
/* A test's faults: ux0:app that cannot be listed. Set before run(). */
static int app_dir_fails;
/* The card's free space for sceIoDevctl (-1: the call fails). */
static long long card_free = 1LL << 30;

/* A root sceIoDopen refuses with dir_fails_rc (a game card's gro0:app, #138),
 * a listing whose sceIoDread fails with EIO after this many entries (-1: never),
 * and safe mode: VitaShell's check (sceIoDevctl("ux0:", 0x3001, NULL, ...))
 * answers 0x80010030. card() clears them. */
static const char *dir_fails;
static int dir_fails_rc, dread_fails_after = -1, safe_mode;
static const char *dread_fails;
static int is_icon(const char *p)
{
    size_t n = strlen(p);
    return n >= 9 && !strcmp(p + n - 9, "icon0.png");
}
/* Appends to log.txt (opens with O_APPEND): in this process, and in a run's. */
static int log_appends;
static int is_log(const char *p, int flags)
{
    size_t n = strlen(p);
    return (flags & O_APPEND) && n >= 7 && !strcmp(p + n - 7, "log.txt");
}
int sceIoOpen(const char *p, int flags, unsigned mode)
{
    int fd = native(open(p, flags, mode)), game;
    if (fd >= 0 && fd < 1024) {
        icon_fd[fd] = is_icon(p);
    }
    if ((flags & (O_WRONLY | O_RDWR)) && strstr(p, "VitaJPOverlay/psp/")) session->description_writes++;
    if (is_log(p, flags)) { log_appends++; if (session) session->log_appends++; }
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
{
    struct stat st;
    size_t n = strlen(p);
    int rc;
    if (n && p[n - 1] == ':') { /* a device: there when the card has a folder of it (gro0:app for a game card) */
        char sub[64];
        snprintf(sub, sizeof(sub), "%sapp", p);
        rc = stat(sub, &st);
        if (rc) { snprintf(sub, sizeof(sub), "%sdata", p); rc = stat(sub, &st); }
        if (rc) return (int)0x80010013u; /* ENODEV: modelled, not a console's answer */
    } else rc = stat(p, &st);
    if (!rc) { out->st_mode = st.st_mode; out->st_size = st.st_size; }
    return native(rc);
}
/* The threads the app's log sees (settings/applog.c): the main thread's, or a
 * test's other one (the Anki worker's). */
static SceUID thread_id = 0x40010003;
SceUID sceKernelGetThreadId(void) { return thread_id; }
int sceIoRename(const char *a, const char *b) { return native(rename(a, b)); }
int sceIoChstat(const char *p, SceIoStat *st, int bits)
{
    if (bits != SCE_CST_SIZE) return (int)0x80010016u;
    return native(truncate(p, st->st_size));
}
int sceIoRemove(const char *p) { return native(unlink(p)); }
int sceIoSync(const char *p, unsigned flags) { (void)p; (void)flags; return 0; }
static DIR *dirs[8];
static char dir_path[8][64];
static int dir_reads[8]; /* entries each listing has given */
int sceIoDopen(const char *p)
{
    if (app_dir_fails && !strcmp(p, "ux0:app")) return (int)0x80010013u;
    if (dir_fails && !strcmp(p, dir_fails)) return dir_fails_rc;
    if (!strcmp(p, "ux0:app")) cur.scans++;
    for (int i = 0; i < 8; i++)
        if (!dirs[i]) {
            if (!(dirs[i] = opendir(p))) return native(-1);
            snprintf(dir_path[i], sizeof(dir_path[i]), "%s", p);
            dir_reads[i] = 0;
            return 100 + i;
        }
    return (int)0x80010018u;
}
int sceIoDread(int d, SceIoDirent *e)
{
    DIR *dir = d >= 100 && d < 108 ? dirs[d - 100] : NULL;
    struct dirent *de;
    if (!dir) return (int)0x80010009u;
    if (dread_fails && !strcmp(dir_path[d - 100], dread_fails) && dir_reads[d - 100] == dread_fails_after)
        return (int)0x80010005u; /* EIO */
    while ((de = readdir(dir)) && de->d_name[0] == '.') {}
    if (!de) return 0;
    char path[400];
    struct stat st;
    memset(e, 0, sizeof(*e));
    snprintf(path, sizeof(path), "%s/%s", dir_path[d - 100], de->d_name);
    if (!stat(path, &st)) e->d_stat.st_mode = st.st_mode;
    snprintf(e->d_name, sizeof(e->d_name), "%s", de->d_name);
    dir_reads[d - 100]++;
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
/* The frame interval the clock advances by: 60 Hz unless a test sets another
 * (card() sets it back). */
static uint64_t frame_us = 16667u;
uint64_t sceKernelGetProcessTimeWide(void) { return (uint64_t)frame * frame_us; }
int sceKernelExitProcess(int status) { session->exited = status; return 0; }
/* settings/sysapp.c's heap, and the budget the harness reports (main.c logs both;
 * budget_rc < 0: refused, as for an ordinary app). */
int _newlib_heap_size_user = 8 * 1024 * 1024;
static int budget_rc;
int sceAppMgrGetBudgetInfo(SceAppMgrBudgetInfo *info)
{
    session->budget_size = info->size;
    session->budget_calls++;
    if (budget_rc < 0) return budget_rc;
    info->app_mode = 2;
    info->total_user_rw_mem = 0xC800u * 1024u;
    info->free_user_rw = 0xC800u * 1024u - 23u * 1024u * 1024u - (unsigned)frame * 4096u;
    return 0;
}
int sceIoDevctl(const char *dev, unsigned int cmd, void *in, int inlen, void *out, int outlen)
{
    (void)in; (void)inlen;
    if (safe_mode && !strcmp(dev, "ux0:") && !out) return (int)0x80010030u; /* EUNSUP: VitaShell's safe mode */
    if (strcmp(dev, "ur0:") && strcmp(dev, "ux0:")) return (int)0x80010013u;
    if (cmd != 0x3001 || !out || outlen < (int)sizeof(SceIoDevInfo) || card_free < 0) return (int)0x80010016u;
    SceIoDevInfo *info = out;
    memset(info, 0, sizeof(*info));
    info->max_size = 1LL << 34;
    info->free_size = card_free;
    info->cluster_size = 32768;
    return 0;
}
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
/* Buttons-only sampling, the default, reports the sticks at rest (kernel/input.c's
 * note); the app asks for analog. */
static int sampling_mode = SCE_CTRL_MODE_DIGITAL;
int sceCtrlSetSamplingMode(int mode) { int old = sampling_mode; sampling_mode = mode; return old; }
/* A test's failed read (e.g. 0x80340020): the SDK leaves the caller's pad as it was. */
static int peek_error;
/* The Anki page stub's record (settings_stubs/anki_page_stub.c). */
extern int vjo_anki_stub_rows;
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
    cur.focus = cur.bar_y = -1;
    clock_gettime(CLOCK_MONOTONIC, &frame_start);
    if (peek_error) return peek_error;
    pad->buttons = frame < script_frames && frame < MAX_FRAMES - 1 ? script[frame].buttons : CANCEL;
    pad->lx = pad->rx = pad->ry = 128;
    pad->ly = (unsigned char)(128 + (frame < script_frames && frame < MAX_FRAMES - 1 && sampling_mode != SCE_CTRL_MODE_DIGITAL
                                     ? script[frame].stick : 0));
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
    session->searches = vjo_test_ime_calls;
    session->search_terms = vjo_test_ime_terms;
    cur.ns = since(&frame_start);
    cur.live = live_textures();
    cur.anki_rows = vjo_anki_stub_rows;
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
        if ((int)x == 38 && (int)y == GAMES_Y + r * GAME_PITCH + 4 &&
            (int)(x_scale * t->width + 0.5f) == 32 && (int)(y_scale * t->height + 0.5f) == 32)
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
    if ((int)x == 24 && (int)w == 4 && color == ACCENT) cur.bar_y = (int)y; /* focus_card's left edge */
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
    (void)font; (void)scale;
    for (int r = 0; r < ROWS; r++) {
        int top = GAMES_Y + r * GAME_PITCH;
        if (x == 90 && y == top + 21) copy(cur.title[r][0], sizeof(cur.title[r][0]), text);
        else if (x == 90 && y == top + 45) copy(cur.title[r][1], sizeof(cur.title[r][1]), text);
        else if (y == top + 36 && x == 260 && !strcmp(text, "PSP")) cur.tag[r] = color;
        else if (y == top + 36 && x == 260 && (!strcmp(text, "Hook") || !strcmp(text, "Luna hook")))
            copy(cur.hook[r], sizeof(cur.hook[r]), text);
        else if (y == top + 36 && x == 90) copy(cur.id[r], sizeof(cur.id[r]), text);
        else if (y == top + 25 && !strcmp(text, "JP")) copy(cur.icon[r], sizeof(cur.icon[r]), "JP");
        else if (y == top + 29 && x > 788) copy(cur.badge[r], sizeof(cur.badge[r]), text); /* badge(788, top + 7, 128, 38) */
    }
    if (y == DEFAULT_BADGE_Y && x > 174 && x < 282) copy(cur.default_badge, sizeof(cur.default_badge), text);
    if (y == HINT_Y) {
        if (x == 24) copy(cur.hints, sizeof(cur.hints), text);
        else { copy(cur.counter, sizeof(cur.counter), text); cur.counter_x = x; }
    }
    if (x == 40 && (y == STATUS_Y || y == STATUS_Y + 28)) copy(cur.status[(y - STATUS_Y) / 28], sizeof(cur.status[0]), text);
    if (y == 422 && !strncmp(text, "Page ", 5)) copy(cur.pages, sizeof(cur.pages), text);
    if (x == 42 && cur.bar_y >= 0 && y == cur.bar_y + 30) copy(cur.bar_title, sizeof(cur.bar_title), text);
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
static char card_dir[PATH_MAX];
/* (test_tmpdir.h's: no link followed, no truncated path walked) */
static void remove_tree(const char *path) { vjo_test_remove_tree(path); }
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
/* One game in root (ux0:app, gro0:app for a game card, ux0:patch for an
 * update): its param.sfo and, as g->icon says, its icon0.png. */
static void install_in(const char *root, const Game *g, int seed)
{
    static uint8_t png[400 * 1024];
    char path[160];
    size_t size = 0;
    mkdir(root, 0777);
    snprintf(path, sizeof(path), "%s/%s", root, g->id); mkdir(path, 0777);
    snprintf(path, sizeof(path), "%s/%s/sce_sys", root, g->id); mkdir(path, 0777);
    snprintf(path, sizeof(path), "%s/%s/sce_sys/param.sfo", root, g->id);
    put_sfo(path, g->title);
    snprintf(path, sizeof(path), "%s/%s/sce_sys/icon0.png", root, g->id);
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
static void install(const Game *g, int seed) { install_in("ux0:app", g, seed); }
/* n games (the catalog adds Adrenaline's shared entry), their icons as kind(i) says. */
static void card(int n, int (*kind)(int))
{
    if (!card_dir[0]) {
        TEST_ASSERT(vjo_test_tmpdir_new(card_dir, sizeof(card_dir)) != NULL);
        atexit(remove_card);
    }
    TEST_ASSERT(chdir("/") == 0);
    remove_tree(card_dir);
    TEST_ASSERT(mkdir(card_dir, 0777) == 0 && chdir(card_dir) == 0);
    mkdir("ux0:data", 0777); mkdir("ux0:data/VitaJPOverlay", 0777); mkdir("ux0:app", 0777);
    game_count = n;
    frame_hook = NULL;
    frame_us = 16667u;
    budget_rc = 0;
    dir_fails = dread_fails = NULL;
    dread_fails_after = -1;
    safe_mode = 0;
    for (int i = 0; i < n; i++) {
        Game *g = &games[i];
        snprintf(g->id, sizeof(g->id), "VJOT%05d", i);
        make_title(i, g->title, sizeof(g->title));
        g->icon = kind ? kind(i) : ICON_OK;
        install(g, i);
    }
    /* settings/catalog.c's synthetic entry, which has no icon */
    snprintf(games[n].id, sizeof(games[n].id), "NPXS10028");
    snprintf(games[n].title, sizeof(games[n].title), VJO_CATALOG_PSP_TITLE);
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
/* One frame of buttons with the left stick pushed. */
TEST_ONLY static void with_stick(unsigned buttons, int stick) { if (at < MAX_FRAMES - 2) script[at++] = (Input){buttons, 0, 0, 0, stick}; }

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
    TEST_CHECK_(session->max_live <= 5 && session->max_cdram <= 5L * CDRAM_BLOCK, "at most 5 icons: %d, %ld B",
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
            ref_wrapped_text(90, GAMES_Y + r * GAME_PITCH + 21, 650, 22, 24, 1, g->title);
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

/* The current page is the entire icon window. */
static int in_window(int f, int index)
{
    int top = top_of(f);
    return top >= 0 && index >= top && index < top + ROWS;
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
/* Current-page icons load in row order, skipping cached or failed images. */
static int loads_in_order(int from, int to, int direction)
{
    (void)direction;
    int top = top_of(to), want[ROWS], w = 0, last = -1, bad = 0;
    for (int r = 0; r < ROWS; r++) want[w++] = top + r;
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
    /* Only the five cards on this page are loaded. */
    TEST_CHECK(session->shown[0].scans == 1 && sum_loads(0, frames) <= ROWS && max_loads(0, frames) <= 1);
    /* One load per frame: after five frames every loadable card has its icon. */
    for (int f = ROWS - 1; f < frames; f++)
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

static int current_page_loads_only(int frames)
{
    int bad = 0, stable = 0;
    for (int f = 0; f < frames; f++) {
        const Frame *shown = &session->shown[f];
        stable = f && top_of(f) == top_of(f - 1) ? stable + 1 : 0;
        if (shown->loaded[0]) {
            int found = 0;
            for (int r = 0; r < ROWS; r++) found |= !strcmp(shown->loaded, shown->id[r]);
            bad += !found;
        }
        if (stable >= ROWS)
            for (int r = 0; r < ROWS; r++) {
                const Game *g = shown->id[r][0] ? game_of(shown->id[r]) : NULL;
                if (g && (g->icon == ICON_OK || g->icon == ICON_RGBA || g->icon == ICON_SMALL))
                    bad += strcmp(shown->icon[r], g->id) != 0;
            }
    }
    return bad == 0;
}

static void scrolling_loads_one_icon_a_frame_and_never_reloads_the_window(void)
{
    card(60, kind_mixed);
    spec_list();
    scenario_scroll();
    int frames = run(at), deepest = 0;
    check_rules();
    TEST_CHECK(check_cards(frames, NULL) == 0);
    TEST_CHECK(max_loads(0, frames) <= 1 && current_page_loads_only(frames));
    for (int f = 0; f < frames; f++) {
        TEST_CHECK(top_of(f) % ROWS == 0);
        if (top_of(f) > deepest) deepest = top_of(f);
    }
    TEST_CHECK(deepest >= 25 && top_of(frames - 1) == 0 && selected_of(frames - 1) == 0);
    TEST_CHECK(window_never_reloaded(frames));
}

/* Crossing a page boundary and coming back loads only those two pages. */
static void returning_to_a_page_reloads_only_that_page(void)
{
    card(60, NULL);
    spec_list();
    at = 0;
    hold(0, 15);
    for (int i = 0; i < 12; i++) tap(DOWN, 8);
    hold(0, 20);
    int mark = at - 1;
    for (int i = 0; i < 3; i++) tap(DOWN, 8);
    for (int i = 0; i < 5; i++) tap(UP, 8);
    hold(0, 10);
    int frames = run(at);
    check_rules();
    TEST_CHECK(check_cards(frames, NULL) == 0 && current_page_loads_only(frames));
    TEST_CHECK(top_of(mark) == top_of(frames - 1) && top_of(mark) > ROWS);
    TEST_CHECK(sum_loads(mark, frames) == 2 * ROWS); /* one new page and the page revisited */
    TEST_CHECK(window_never_reloaded(frames));
}

static void page_jumps_keep_the_row_and_load_only_that_page(void)
{
    card(60, NULL);
    spec_list();
    scenario_pages();
    int frames = run(at);
    check_rules();
    TEST_CHECK(check_cards(frames, NULL) == 0);
    TEST_CHECK(max_loads(0, frames) <= 1);
    /* Each R advances five entries. The new page fills lazily, one image per frame. */
    int f = 20, top = top_of(19), row = selected_of(19) - top, placeholders = 0;
    TEST_CHECK(top == 0 && row == 0);
    for (int i = 0; i < 8; i++, f += 16) {
        TEST_CHECK_(top_of(f) == top + ROWS * (i + 1), "R %d: top %d", i, top_of(f));
        TEST_CHECK(selected_of(f) - top_of(f) == row);
    }
    for (int i = 0; i < 8; i++, f += 16) TEST_CHECK_(top_of(f) == top + ROWS * (7 - i), "L %d: top %d", i, top_of(f));
    /* R held: pages repeat until the last page, then the last game is selected. */
    TEST_CHECK(top_of(frames - 1) == (list_count - 1) / ROWS * ROWS && selected_of(frames - 1) == list_count - 1);
    for (int g = 20; g < frames; g++)
        for (int r = 0; r < ROWS; r++) placeholders += !strcmp(session->shown[g].icon[r], "JP") &&
                                                       strcmp(session->shown[g].id[r], "NPXS10028");
    TEST_CHECK(placeholders > 0 && current_page_loads_only(frames));
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
    TEST_CHECK_(top_of(a) == 5 && selected_of(a) == 6, "a: %d %d", top_of(a), selected_of(a));
    TEST_CHECK_(top_of(b) == 5 && selected_of(b) == 7, "b: %d %d", top_of(b), selected_of(b));
    TEST_CHECK_(top_of(c) == 5 && selected_of(c) == 7, "c: %d %d", top_of(c), selected_of(c));
    TEST_CHECK_(top_of(d) == 0 && selected_of(d) == 2, "d: %d %d", top_of(d), selected_of(d));
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
        int top = stops[k] / ROWS * ROWS;
        TEST_CHECK(top_of(when[k]) == top);
    }
    TEST_CHECK(selected_of(when[n + 1]) == list_count - 1);
    /* A letter jump loads only its current page, in row order. */
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

/* Refresh loads only the current page, whichever direction led there. */
static void refresh_loads_only_the_current_page(void)
{
    static const unsigned keys[] = {UP, SCE_CTRL_LTRIGGER, SCE_CTRL_LEFT};
    static const char *const names[] = {"Up", "L", "Left"};
    for (int k = 0; k < 3; k++) {
        card(60, NULL);
        spec_list();
        at = 0;
        hold(0, 15);
        for (int i = 0; i < 6; i++) tap(SCE_CTRL_RTRIGGER, 12); /* down six five-game pages */
        tap(keys[k], 12);                                        /* one move up */
        tap(SCE_CTRL_START, 14);                                 /* the reload */
        int start = at - 15, frames = run(at), top = top_of(start);
        check_rules();
        TEST_CHECK(frames == at);
        TEST_CHECK_(top >= ROWS && top + ROWS <= list_count && top < 6 * ROWS, "%s: top %d", names[k], top);
        TEST_CHECK_(sum_loads(start, start + 3 * ROWS) == ROWS, "%s: %d loads", names[k],
                    sum_loads(start, start + 3 * ROWS));
        TEST_CHECK_(loads_in_order(start, start + 3 * ROWS - 1, -1), "%s", names[k]);
    }
    /* The same bound holds after a move down. */
    card(60, NULL);
    spec_list();
    at = 0;
    hold(0, 15);
    for (int i = 0; i < 6; i++) tap(SCE_CTRL_RTRIGGER, 12);
    tap(SCE_CTRL_START, 14);
    int start = at - 15;
    run(at);
    TEST_CHECK(loads_in_order(start, start + 3 * ROWS - 1, 1) && sum_loads(start, start + 3 * ROWS) == ROWS);
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
    TEST_CHECK_(selected_of(q - 1) == 5 && selected_of(q) == 5 + ROWS && top_of(q) == top_of(q - 1) + ROWS,
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

/* At 30 Hz (the app's likely rate as a system application: frames of 33,333 us)
 * the stick moves the list as many rows by the same time as at 60 Hz
 * (core/stick_scroll.h): the push's row, the same 166.7 ms pause, then two rows
 * a frame. */
static void stick_scrolls_at_the_same_speed_at_30_hz(void)
{
    int sel60[80], sel30[40];
    for (int pass = 0; pass < 2; pass++) {
        card(200, NULL);
        spec_list();
        int n = pass ? 40 : 80; /* 1.33 s either way */
        if (pass) frame_us = 33333u;
        at = 0;
        hold(0, 10);
        int p = at;
        hold_stick(127, n);
        hold(0, 10);
        int frames = run(at);
        TEST_CHECK(frames == at);
        for (int k = 0; k < n; k++)
            (pass ? sel30 : sel60)[k] = selected_of(p + k);
    }
    /* frame k at 30 Hz is frame 2k at 60 Hz: the same time, the same row */
    int bad = 0;
    for (int k = 0; k < 40; k++)
        if (sel30[k] != sel60[2 * k] && bad++ < 5)
            TEST_MSG("%d ms: row %d at 30 Hz, %d at 60 Hz", k * 33333 / 1000, sel30[k], sel60[2 * k]);
    TEST_CHECK(bad == 0);
    TEST_CHECK_(sel30[0] == 1 && sel30[4] == 1 && sel30[5] == 2 && sel30[6] == 4 && sel30[39] == 70,
                "the push %d, 133 ms %d, 167 ms %d, 200 ms %d, 1.3 s %d", sel30[0], sel30[4], sel30[5], sel30[6],
                sel30[39]);
}

/* log.txt's lines (the Anki page stub writes main.c's vjo_log as the page does)
 * but the catalog's, which every scan writes (its own tests below). */
static int log_lines(char lines[][256], int cap)
{
    FILE *f = fopen("ux0:data/VitaJPOverlay/log.txt", "r");
    char line[1024];
    int n = 0;
    if (!f) return -1;
    while (n < cap && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        if (strncmp(line, "Rubi app: games: ", 17)) snprintf(lines[n++], 256, "%.255s", line);
    }
    fclose(f);
    return n;
}

/* The app's version in its start line (settings/main.c VJO_APP_VERSION): the host
 * build has no VJO_VERSION_STRING ("dev"); the debug build adds "debug". */
#if VJO_DEBUG
#define APP_VERSION_WORDS "vdev debug"
#else
#define APP_VERSION_WORDS "vdev"
#endif

/* At start, one line of the budget the system gives the app and its heap
 * (settings/sysapp.c's 8 MiB): sceAppMgrGetBudgetInfo with the SDK's 0x88-byte
 * structure. Refused (an ordinary app), the line says so. log_file = off: none. */
static void the_start_line_names_the_budget_and_the_heap(void)
{
    char lines[8][256];
    card(5, NULL);
    at = 0;
    hold(0, 5);
    TEST_CHECK(run(at) == at);
    TEST_CHECK_(session->budget_size == 0x88 && session->budget_calls == 1, "info->size %d, %d calls",
                session->budget_size, session->budget_calls);
    TEST_CHECK(log_lines(lines, 8) == 1);
    TEST_CHECK_(!strcmp(lines[0], "Rubi app: start: " APP_VERSION_WORDS "; budget mode 2: main memory 28311552 "
                                  "of 52428800 B free, phycont 0 of 0 B, CDRAM 0 of 0 B; heap 8388608 B"), "%s", lines[0]);
    card(5, NULL);
    budget_rc = (int)0x80800001;
    at = 0;
    hold(0, 70);
    TEST_CHECK(run(at) == at);
    TEST_CHECK(log_lines(lines, 8) == (VJO_DEBUG ? 2 : 1)); /* the frame interval: the debug build's only */
    TEST_CHECK_(!strcmp(lines[0], "Rubi app: start: " APP_VERSION_WORDS "; no system-application budget "
                                  "(sceAppMgrGetBudgetInfo 0x80800001); heap 8388608 B"), "%s", lines[0]);
#if VJO_DEBUG
    TEST_CHECK_(!strcmp(lines[1], "Rubi app: frames: 60 in 1000020 us, 16667 us a frame (shortest 16667, "
                                  "longest 16667)"), "%s", lines[1]);
#endif
    card(5, NULL);
    put_file("ux0:data/VitaJPOverlay/config.ini", "log_file = off\n", 15);
    at = 0;
    hold(0, 70);
    TEST_CHECK(run(at) == at);
    TEST_CHECK_(log_lines(lines, 8) == -1, "log_file = off: no log.txt");
}

#if VJO_DEBUG /* the debug build's own (docs/release-variants.md) */
/* The frame interval goes to log.txt once, over the first 60 frames, with the
 * main memory then free: 16,667 us at 60 Hz, 33,333 us at 30 Hz. */
static void the_frame_interval_is_logged_once(void)
{
    static const unsigned rates[] = {16667u, 33333u};
    for (int i = 0; i < 2; i++) {
        char lines[8][256], want[256];
        card(30, NULL);
        frame_us = rates[i];
        at = 0;
        hold(0, 200);
        TEST_CHECK(run(at) == at);
        TEST_CHECK(log_lines(lines, 8) == 2);
        snprintf(want, sizeof(want), "Rubi app: frames: 60 in %u us, %u us a frame (shortest %u, longest %u); "
                 "budget mode 2: main memory 28065792 of 52428800 B free, phycont 0 of 0 B, CDRAM 0 of 0 B",
                 60 * rates[i], rates[i], rates[i], rates[i]);
        TEST_CHECK_(!strcmp(lines[1], want), "%u us frames: %s", rates[i], lines[1]);
        TEST_CHECK_(session->budget_calls == 2, "%d budget queries", session->budget_calls);
    }
}
#endif

/* The list keys act on the Games page only: on the OCR & lookup, Anki and Install
 * pages Up/Down/L/R leave the list where it was. */
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
    tap(SCE_CTRL_SELECT, 8); /* Install (docs/one-vpk-install.md) */
    tap(DOWN, 8);
    tap(SCE_CTRL_RTRIGGER, 8);
    hold(0, 8);
    tap(SCE_CTRL_SELECT, 8); /* Games again */
    int after = at - 1, frames = run(at);
    TEST_CHECK(frames == at);
    TEST_CHECK_(selected_of(before) == 1 && selected_of(after) == 1 && top_of(after) == 0, "selection %d -> %d, top %d",
                selected_of(before), selected_of(after), top_of(after));
}

/* The left stick stops at the first and last card of the OCR & lookup page, as
 * VitaShell's lists do (core/stick_scroll.h): held down, the focus only goes
 * down and stays on the last card; held up, it only goes up and stays on the
 * first. The d-pad still wraps round there. */
/* The OCR & lookup page's focused card in frame f: its place among the page's
 * cards (settings/main.c PREF_*), by its title, or -1. */
#define OCR_CARDS 10
static int ocr_card(const Frame *f)
{
    static const char *const titles[OCR_CARDS] = {"Model size", "Read text", "Speed boost while reading (444 MHz)",
                                                  "Japanese characters only", "Highlight", "Dictionaries shown",
                                                  "Text source", /* API 31 */
                                                  "Trusted hooks replace OCR", "Skip OCR on screens without text",
                                                  "Vertical text"}; /* API 43, API 47 */
    for (int i = 0; i < OCR_CARDS; i++)
        if (!strcmp(f->bar_title, titles[i])) return i;
    return -1;
}
static void stick_stops_at_the_first_and_last_card(void)
{
    card(30, NULL);
    spec_list();
    at = 0;
    hold(0, 10);
    tap(SCE_CTRL_SELECT, 8); /* OCR & lookup */
    int first_at = at - 1, down = at;
    hold_stick(127, 30);
    hold(0, 4);
    int up = at;
    hold_stick(-128, 30);
    hold(0, 4);
    int keys = at;
    tap(UP, 8);   /* from the first card */
    tap(DOWN, 8); /* from the last */
    int frames = run(at);
    TEST_CHECK(frames == at);
    const Frame *f = session->shown;
    /* by the focused card's place in the page (the page scrolls, so its bar's y can go up as the focus goes down) */
    int first = ocr_card(&f[first_at]), last = ocr_card(&f[up - 1]);
    TEST_CHECK_(first == 0 && last == OCR_CARDS - 1, "first card %d, last %d", first, last);
    TEST_CHECK_(ocr_card(&f[down]) == first + 1, "the push moves one card: %d", ocr_card(&f[down]));
    for (int i = down + 1; i < down + 10; i++) /* nine quiet frames */
        TEST_CHECK_(ocr_card(&f[i]) == ocr_card(&f[down]), "frame %d: %d", i - down, ocr_card(&f[i]));
    for (int i = down; i < up; i++) /* never back to the top */
        TEST_CHECK_(ocr_card(&f[i]) >= ocr_card(&f[i - 1]), "down, frame %d: %d after %d", i - down, ocr_card(&f[i]),
                    ocr_card(&f[i - 1]));
    for (int i = down + 20; i < up; i++) /* runs to the last card and stays */
        TEST_CHECK_(ocr_card(&f[i]) == last, "down, frame %d: %d", i - down, ocr_card(&f[i]));
    for (int i = up; i < keys; i++)
        TEST_CHECK_(ocr_card(&f[i]) <= ocr_card(&f[i - 1]), "up, frame %d: %d after %d", i - up, ocr_card(&f[i]),
                    ocr_card(&f[i - 1]));
    for (int i = up + 20; i < keys; i++)
        TEST_CHECK_(ocr_card(&f[i]) == first && f[i].bar_y == 112, "up, frame %d: %d", i - up, ocr_card(&f[i]));
    TEST_CHECK_(ocr_card(&f[keys]) == last, "the d-pad's Up wraps to the last card: %d", ocr_card(&f[keys]));
    TEST_CHECK_(ocr_card(&f[keys + 9]) == first, "the d-pad's Down wraps to the first: %d", ocr_card(&f[keys + 9]));
}

/* settings/main.c's OCR_Y: the OCR & lookup page's first card, focused when the app starts. */
#define OCR_FIRST_Y 112
/* A stick still pushed through a page change moves nothing until it is let
 * go, as a held button is no new press: not on the Select frame and not
 * after it. A new push moves again. The stick reaches the Anki page
 * (anki_page.c stops it at its ends), and never the Games list from another
 * page. */
static void a_page_change_latches_the_stick(void)
{
    card(30, NULL);
    spec_list();
    at = 0;
    hold(0, 10);
    hold_stick(127, 15); /* Games: a row, nine quiet frames, then five */
    int games = at - 1;
    with_stick(SCE_CTRL_SELECT, 127); /* OCR & lookup, the stick still down */
    int ocr = at - 1;
    hold_stick(127, 30);
    int ocr_held = at - 1;
    hold(0, 4);
    hold_stick(127, 1); /* let go, then pushed: a card */
    int ocr_push = at - 1;
    hold(0, 4);
    with_stick(SCE_CTRL_SELECT, 127); /* Anki, the stick still down */
    int anki = at - 1;
    hold_stick(127, 30);
    hold(0, 4);
    int anki_rest = at - 1;
    hold_stick(127, 1);
    hold(0, 4);
    int anki_push = at - 1;
    hold_stick(-128, 1); /* pushed up */
    hold(0, 4);
    int up_push = at - 1;
    with_stick(SCE_CTRL_SELECT, -128); /* Install, the stick up */
    with_stick(0, -128);
    with_stick(SCE_CTRL_SELECT, -128); /* Games again, the stick still up */
    hold_stick(-128, 30);
    int games_held = at - 1;
    hold(0, 4);
    int frames = run(at);
    TEST_CHECK(frames == at);
    const Frame *f = session->shown;
    TEST_CHECK_(selected_of(games) == 6, "Games, the stick held 15 frames: %d", selected_of(games));
    for (int i = ocr; i <= ocr_held; i++)
        TEST_CHECK_(f[i].bar_y == OCR_FIRST_Y, "OCR & lookup, frame %d after Select: %d", i - ocr, f[i].bar_y);
    TEST_CHECK_(f[ocr_push].bar_y > OCR_FIRST_Y, "a new push moves a card: %d", f[ocr_push].bar_y);
    TEST_CHECK_(f[anki_rest].anki_rows == f[anki - 1].anki_rows, "Anki: %d rows from the stick held through Select",
                f[anki_rest].anki_rows - f[anki - 1].anki_rows);
    TEST_CHECK_(f[anki_push].anki_rows == f[anki_rest].anki_rows + 1, "Anki: a push gives %d",
                f[anki_push].anki_rows - f[anki_rest].anki_rows);
    TEST_CHECK_(f[up_push].anki_rows == f[anki_push].anki_rows - 1, "Anki: a push up gives %d",
                f[up_push].anki_rows - f[anki_push].anki_rows);
    TEST_CHECK_(selected_of(games_held) == 6, "Games, back with the stick held up: %d", selected_of(games_held));
}

/* A touched tab changes the page as Select does: a stick still pushed moves
 * nothing on the new page until it is let go. */
static void a_touched_tab_latches_the_stick(void)
{
    card(30, NULL);
    spec_list();
    at = 0;
    hold(0, 10);
    hold_stick(127, 15);
    script[at++] = (Input){0, 1, 24 + 304 + 100, 52 + 20, 127}; /* the OCR & lookup tab, the stick still down */
    int tab = at - 1;
    hold_stick(127, 30);
    int held = at - 1;
    hold(0, 4);
    int frames = run(at);
    TEST_CHECK(frames == at);
    const Frame *f = session->shown;
    for (int i = tab; i <= held; i++)
        TEST_CHECK_(f[i].bar_y == OCR_FIRST_Y, "OCR & lookup, frame %d after the tab: %d", i - tab, f[i].bar_y);
}

/* The d-pad acts first in a frame: a press while the stick's repeat is also
 * due moves one row, not two; with the d-pad held, the stick moves again. The
 * same on the OCR & lookup page, where the stick is held up at the first card. */
static void the_dpad_acts_first_in_a_frame(void)
{
    card(30, NULL);
    spec_list();
    at = 0;
    hold(0, 10);
    hold_stick(127, 12); /* rows on frames 1, 11 and 12 */
    int before = at - 1;
    with_stick(DOWN, 127); /* both due */
    int both = at - 1;
    with_stick(DOWN, 127); /* the d-pad held, its repeat 350 ms away: the stick's row */
    int held = at - 1;
    hold(0, 4);
    tap(SCE_CTRL_SELECT, 4); /* OCR & lookup, its first card */
    hold_stick(-128, 12);    /* up at the first card: it stays */
    int ocr_before = at - 1;
    with_stick(DOWN, -128); /* the d-pad's Down and the stick's up both due: Down */
    int ocr_both = at - 1;
    with_stick(DOWN, -128); /* the d-pad held: the stick's up */
    int ocr_held = at - 1;
    hold(0, 4);
    int frames = run(at);
    TEST_CHECK(frames == at);
    const Frame *f = session->shown;
    TEST_CHECK_(selected_of(before) == 3, "%d", selected_of(before));
    TEST_CHECK_(selected_of(both) == 4 && selected_of(held) == 5, "both due: %d, then %d", selected_of(both),
                selected_of(held));
    TEST_CHECK_(f[ocr_before].bar_y == OCR_FIRST_Y && f[ocr_both].bar_y > OCR_FIRST_Y &&
                f[ocr_held].bar_y == OCR_FIRST_Y, "OCR & lookup: %d, %d, %d", f[ocr_before].bar_y, f[ocr_both].bar_y,
                f[ocr_held].bar_y);
}

/* A failed read (sceCtrlPeekBufferPositive below 1) is no buttons and the
 * stick at rest: the app's zeroed pad would read as the stick pushed up. */
static int failing_from, failing_to;
static void failing_reads(int f) { peek_error = f >= failing_from && f < failing_to ? (int)0x80340020 : 0; }
static void a_failed_read_moves_nothing(void)
{
    card(30, NULL);
    spec_list();
    at = 0;
    hold(0, 10);
    for (int i = 0; i < 10; i++) tap(DOWN, 1);
    int before = at - 1;
    failing_from = at;
    hold(0, 40);
    failing_to = at;
    hold(0, 4);
    frame_hook = failing_reads;
    int frames = run(at);
    TEST_CHECK(frames == at);
    TEST_CHECK_(selected_of(before) == 10 && selected_of(at - 1) == 10, "%d, then %d after 40 failed reads",
                selected_of(before), selected_of(at - 1));
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

/* Title layouts are paid once when their page is first shown, never for off-page titles. */
TEST_ONLY static int kind_none(int i) { (void)i; return ICON_NONE; }
static long layout_glyphs(const char *title)
{
    counted = 0;
    ref_measure = counting_width;
    ref_wrapped_text(90, 0, 650, 22, 24, 1, title);
    return counted;
}
static void titles_are_laid_out_only_for_the_current_page(void)
{
    card(60, kind_none);
    spec_list();
    at = 0;
    hold(0, 30);
    for (int i = 0; i < ROWS; i++) tap(DOWN, 10);
    int change = at - 11;
    hold(0, 10);
    int frames = run(at);
    TEST_CHECK(frames == at);
    long idle = session->shown[25].glyphs;
    for (int f = 1; f < 30; f++) TEST_CHECK(session->shown[f].glyphs == idle);
    TEST_CHECK(top_of(change - 1) == 0 && top_of(change) == ROWS);
    long work = 0;
    for (int r = 0; r < ROWS; r++) work += layout_glyphs(list[ROWS + r].title);
    TEST_CHECK(session->shown[change].glyphs >= idle + work - 4);
    TEST_CHECK(session->shown[change + 1].glyphs <= idle + 4);
    TEST_CHECK(current_page_loads_only(frames));
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
    /* when it applies, true with no game running as beside one */
    TEST_CHECK_(!strcmp(session->shown[TOGGLE_FRAMES - 1].status[0], "Saved. It applies when the game next starts or "
                "resumes.") && !session->shown[TOGGLE_FRAMES - 1].status[1][0], "%s | %s",
                session->shown[TOGGLE_FRAMES - 1].status[0], session->shown[TOGGLE_FRAMES - 1].status[1]);
    /* The filters show other lists: Enabled lacks the two games, Disabled is them. */
    int enabled = TOGGLE_FRAMES + 15, disabled = enabled + 16, all = disabled + 16;
    TEST_CHECK(selected_of(enabled) == 2 && top_of(enabled) == 2);
    TEST_CHECK(!strcmp(session->shown[disabled].id[0], list[0].id) && !strcmp(session->shown[disabled].id[1], list[1].id)
               && !session->shown[disabled].id[2][0]);
    TEST_CHECK(top_of(all) == 0);
    TEST_CHECK(max_loads(0, frames) <= 1 && visible_first(frames));
    TEST_CHECK_(sum_loads(TOGGLE_FRAMES, frames) <= 2 * ROWS, "%d loads through the filters", sum_loads(TOGGLE_FRAMES, frames));
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
    int reach = deepest + ROWS - 1, broken_tried = 0;
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
    snprintf(games[game_count].title, sizeof(games[0].title), VJO_CATALOG_PSP_TITLE);
    games[game_count].icon = ICON_NONE;
    spec_list();
    int target = 0;
    for (int i = ROWS; i < list_count - 1; i++)
        if (!loadable_kind(before[i].icon) && loadable_kind(list[i].icon)) { target = i; break; }
    TEST_ASSERT(target >= ROWS); /* pick a page that actually exercises the stale failure flag */
    frame_hook = install_new_game;
    at = 0;
    hold(0, 20);
    for (int i = 0; i < target; i++) tap(DOWN, 1);
    hold(0, at < NEW_GAME_FRAME + 15 ? NEW_GAME_FRAME + 15 - at : 15);
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
    /* Start empties the cache after the GPU wait, then refills only this page, one per frame. */
    TEST_CHECK(session->shown[start].waits >= 1 && max_loads(0, frames) <= 1);
    int loadable = 0, top = top_of(start);
    for (int i = top; i < top + ROWS; i++)
        loadable += i >= 0 && i < list_count && (list[i].icon == ICON_OK || list[i].icon == ICON_RGBA ||
                                                 list[i].icon == ICON_SMALL);
    TEST_CHECK_(sum_loads(start, frames) == loadable, "%d loads after Start, %d loadable icons in the window",
                sum_loads(start, frames), loadable);
    TEST_CHECK(top >= ROWS && loads_in_order(start, start + 3 * ROWS + 2, 1));
    /* The new game sorts first, so each later entry moved down one place. The case
     * that per-place state (failures, layouts) must not survive: a place in the
     * window whose old entry's icon was broken, and was tried, now holds a good one. */
    int moved = 0;
    for (int i = top; i < top + ROWS; i++)
        moved += i >= 1 && i < list_count && !strcmp(list[0].id, "VJOT09999") && !loadable_kind(before[i].icon) &&
                 loadable_kind(list[i].icon);
    TEST_CHECK_(moved >= 1, "no window place went from a broken icon to a good one (top %d)", top);
    /* From the rescan on, every card shows its own game's rows and icon. */
    int bad = 0;
    ref_measure = stub_width;
    for (int f = start; f < frames; f++)
        for (int r = 0; r < ROWS; r++) {
            const Frame *sh = &session->shown[f];
            const Game *g = sh->id[r][0] ? game_of(sh->id[r]) : NULL;
            if (!g) continue;
            ref_wrapped_text(90, 0, 650, 22, 24, 1, g->title);
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
    TEST_CHECK(deepest == (list_count - 1) / ROWS * ROWS && top_of(frames - 1) == 0);
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
    script[at++] = (Input){0, 1, 400, GAMES_Y + GAME_PITCH + 20, 0}; /* the second card */
    hold(0, 15);
    int frames = run(at);
    check_rules();
    TEST_CHECK(check_cards(frames, NULL) == 0);
    TEST_CHECK(selected_of(frames - 1) == 1);
    TEST_CHECK(!strncmp(session->shown[frames - 1].status[0], "Saved.", 6));
    TEST_CHECK(sum_loads(16, frames) == 0);
}

/* ---- Adrenaline bubbles (settings/psp.c, core/psptarget.c, core/taicfg.c) ---- */

static void put_le32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
/* A PARAM.SFO with TITLE and DISC_ID; stale: TITLE's length field left at 1,
 * as Adrenaline Bubble Manager before 01fd277 wrote a bubble's. */
static size_t psp_sfo(uint8_t *sfo, const char *title, const char *disc, int stale)
{
    uint32_t tn = (uint32_t)strlen(title) + 1, dn = (uint32_t)strlen(disc) + 1;
    memset(sfo, 0, 512);
    memcpy(sfo, "\0PSF", 4); put_le32(sfo + 4, 0x101);
    put_le32(sfo + 8, 52); put_le32(sfo + 12, 68); put_le32(sfo + 16, 2); /* keys at 52, values at 68 */
    sfo[22] = 4; sfo[23] = 2; put_le32(sfo + 24, dn); put_le32(sfo + 28, 16); put_le32(sfo + 32, 0); /* DISC_ID */
    sfo[36] = 8; sfo[38] = 4; sfo[39] = 2; put_le32(sfo + 40, stale ? 1 : tn); put_le32(sfo + 44, 128);
    put_le32(sfo + 48, 16); /* TITLE */
    memcpy(sfo + 52, "DISC_ID\0TITLE\0", 14);
    memcpy(sfo + 68, disc, dn);
    memcpy(sfo + 84, title, tn);
    return 68 + 16 + 128;
}
static void dir_rec(uint8_t *at, uint32_t lba, uint32_t size, int dir, const char *name)
{
    size_t n = strlen(name) ? strlen(name) : 1;
    at[0] = (uint8_t)(33 + n + !(n & 1));
    put_le32(at + 2, lba); put_le32(at + 10, size);
    at[25] = dir ? 2 : 0;
    at[32] = (uint8_t)n;
    memcpy(at + 33, name, strlen(name));
}
/* A synthetic UMD image: the volume descriptor, the root, /PSP_GAME, PARAM.SFO. */
static void put_iso(const char *path, const char *title, const char *disc)
{
    static uint8_t img[21 * 2048];
    uint8_t sfo[512];
    size_t n = psp_sfo(sfo, title, disc, 0);
    memset(img, 0, sizeof(img));
    uint8_t *pvd = img + 16 * 2048;
    pvd[0] = 1; memcpy(pvd + 1, "CD001", 5); pvd[6] = 1; pvd[128] = 0; pvd[129] = 8; /* 2048-byte blocks */
    dir_rec(pvd + 156, 17, 2048, 1, "");
    dir_rec(img + 17 * 2048, 18, 2048, 1, "PSP_GAME");
    dir_rec(img + 18 * 2048, 19, (uint32_t)n, 0, "PARAM.SFO;1");
    memcpy(img + 19 * 2048, sfo, n);
    put_file(path, img, sizeof(img));
}
static void put_pbp(const char *path, const char *title, const char *disc)
{
    uint8_t pbp[0x28 + 512] = {0}, sfo[512];
    size_t n = psp_sfo(sfo, title, disc, 0);
    memcpy(pbp, "\0PBP", 4); put_le32(pbp + 4, 0x10000);
    put_le32(pbp + 8, 0x28);
    for (int i = 1; i < 8; i++) put_le32(pbp + 8 + 4 * i, (uint32_t)(0x28 + n));
    memcpy(pbp + 0x28, sfo, n);
    put_file(path, pbp, 0x28 + n);
}
/* ux0:app/<id>: its own param.sfo and, for a bubble, data/boot.bin (the
 * magic, the target's path at 0x40) or data/boot.inf (inf). */
static void put_app(const char *id, const char *own_title, int stale, const char *target, int inf)
{
    char p[160];
    uint8_t sfo[512], boot[0x140] = {0};
    snprintf(p, sizeof(p), "ux0:app/%s", id); mkdir(p, 0777);
    snprintf(p, sizeof(p), "ux0:app/%s/sce_sys", id); mkdir(p, 0777);
    snprintf(p, sizeof(p), "ux0:app/%s/sce_sys/param.sfo", id);
    put_file(p, sfo, psp_sfo(sfo, own_title, id, stale));
    if (!target) return;
    snprintf(p, sizeof(p), "ux0:app/%s/data", id); mkdir(p, 0777);
    if (inf) {
        char text[300];
        int n = snprintf(text, sizeof(text), "# synthetic\r\nPATH=ms0:%s\r\n", target);
        snprintf(p, sizeof(p), "ux0:app/%s/data/boot.inf", id);
        put_file(p, text, (size_t)n);
        return;
    }
    memcpy(boot, "ABB", 4);
    boot[0x0C] = 1;
    memcpy(boot + 0x40, target, strlen(target));
    snprintf(p, sizeof(p), "ux0:app/%s/data/boot.bin", id);
    put_file(p, boot, sizeof(boot));
}
static const char *const tai_base = "*KERNEL\nur0:tai/VitaJPOverlay_Kernel.skprx\n*main\nur0:tai/VitaJPOverlay_Shell.suprx\n"
                                    "*PCSG00415\nur0:tai/VitaJPOverlay_OCR.suprx\n*PSPEMUCFW\nur0:tai/VitaJPOverlay_OCR.suprx\n";
static char *slurp(const char *path, size_t *len)
{
    static char text[8][16384];
    static int next;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *t = text[next++ % 8];
    size_t n = fread(t, 1, sizeof(text[0]) - 1, f);
    fclose(f);
    t[n] = '\0';
    if (len) *len = n;
    return t;
}
static void add_game(const char *id, const char *title)
{
    games[game_count + 1] = games[game_count]; /* the shared PSP row stays last */
    snprintf(games[game_count].id, sizeof(games[0].id), "%s", id);
    snprintf(games[game_count].title, sizeof(games[0].title), "%s", title);
    games[game_count].icon = ICON_NONE;
    game_count++;
}
/* Four Vita games, three bubbles (an ISO, an EBOOT.PBP, a missing target), an
 * old boot.inf bubble, Adrenaline (registered) and ur0's taiHEN configuration. */
static void bubble_card(void)
{
    card(4, NULL);
    mkdir("ur0:tai", 0777);
    put_file("ur0:tai/config.txt", tai_base, strlen(tai_base));
    mkdir("ux0:pspemu", 0777); mkdir("ux0:pspemu/ISO", 0777); mkdir("ux0:pspemu/PSP", 0777);
    mkdir("ux0:pspemu/PSP/GAME", 0777); mkdir("ux0:pspemu/PSP/GAME/NPJH90001", 0777);
    put_app("ULJS00378", "Old Bubble Name", 1, "ux0:pspemu/ISO/Synthetic.iso", 0);
    put_iso("ux0:pspemu/ISO/Synthetic.iso", "合成テスト PSP", "ULJS00378");
    add_game("ULJS00378", "合成テスト PSP");
    put_app("PSPEMU005", "Bubble Five", 0, "ux0:pspemu/PSP/GAME/NPJH90001/EBOOT.PBP", 0);
    put_pbp("ux0:pspemu/PSP/GAME/NPJH90001/EBOOT.PBP", "Synthetic PBP Game", "NPJH90001");
    add_game("PSPEMU005", "Synthetic PBP Game");
    put_app("PSPEMU006", "Unreadable Six", 1, "ux0:pspemu/ISO/MISSING.ISO", 0);
    add_game("PSPEMU006", "Unreadable Six");
    put_app("OLDINF001", "Old Inf Bubble", 0, "/ISO/OLD.ISO", 1);
    put_iso("ux0:pspemu/ISO/OLD.ISO", "Synthetic Old Game", "ULJM05001");
    add_game("OLDINF001", "Synthetic Old Game");
    put_app("PSPEMUCFW", "Adrenaline", 0, NULL, 0);
    add_game("PSPEMUCFW", "Adrenaline");
    spec_list();
}
/* The app's lines in the card's log.txt (the Anki page stub's vjo_log, as
 * settings/anki_page.c writes them: "Rubi app: " and the text). */
#define APP_LOG "ux0:data/VitaJPOverlay/log.txt"
static const char *app_logged(const char *needle)
{
    static char line[512];
    FILE *f = fopen(APP_LOG, "r");
    const char *found = NULL;
    while (f && !found && fgets(line, sizeof(line), f))
        if (strstr(line, needle)) found = line;
    if (f) fclose(f);
    return found;
}
/* The frame's card for id: its row, or -1. */
static int card_of(int f, const char *id)
{
    for (int r = 0; r < ROWS; r++) if (!strcmp(session->shown[f].id[r], id)) return r;
    return -1;
}
static void goto_row(const char *id)
{
    int index = list_index(id);
    TEST_ASSERT(index >= 0);
    for (int i = 0; i < index; i++) tap(DOWN, 3);
    hold(0, 4);
}

static void bubbles_are_named_tagged_and_described(void)
{
    bubble_card();
    at = 0;
    hold(0, 20);
    goto_row("ULJS00378");
    int frames = run(at), last = frames - 1;
    check_rules();
    /* Every card shows its game's title: a bubble's target's, else its own
     * (a stale SFO length read), Adrenaline's, the shared PSP row's. */
    TEST_CHECK(check_cards(frames, NULL) == 0);
    int r = card_of(last, "ULJS00378");
    TEST_ASSERT(r >= 0);
    TEST_CHECK(session->shown[last].tag[r] == ACCENT);
    for (int f = 0; f < frames; f++)
        for (int k = 0; k < ROWS; k++) {
            const char *id = session->shown[f].id[k];
            if (!id[0]) continue;
            int psp = !strcmp(id, "ULJS00378") || !strncmp(id, "PSPEMU", 6) || !strcmp(id, "OLDINF001") ||
                      !strcmp(id, "NPXS10028");
            TEST_CHECK_(!!session->shown[f].tag[k] == psp, "frame %d %s tag 0x%08X", f, id, session->shown[f].tag[k]);
            /* the tag never dims: the kernel loads the OCR worker into every game that is ON (#138) */
            if (session->shown[f].tag[k]) TEST_CHECK_(session->shown[f].tag[k] == ACCENT, "frame %d %s", f, id);
        }
    /* Enter toggles a bubble as any game; nothing asks for a registration. */
    TEST_CHECK(!strcmp(session->shown[last].hints, "×: toggle   L/R: page   Left/Right: letter   ○: exit"));
    TEST_MSG("%s", session->shown[last].hints);
    char both[300];
    snprintf(both, sizeof(both), "%s %s", session->shown[25].status[0], session->shown[25].status[1]);
    TEST_CHECK(!strstr(both, "registered") && strstr(both, "Choices save automatically") != NULL);
    TEST_MSG("%s", both);
    /* The Shell's descriptions: disc ID, title, target. */
    TEST_CHECK(!strcmp(slurp("ux0:data/VitaJPOverlay/psp/ULJS00378.txt", NULL),
                       "ULJS00378\n合成テスト PSP\nux0:pspemu/ISO/Synthetic.iso\n"));
    TEST_CHECK(!strcmp(slurp("ux0:data/VitaJPOverlay/psp/PSPEMU005.txt", NULL),
                       "NPJH90001\nSynthetic PBP Game\nux0:pspemu/PSP/GAME/NPJH90001/EBOOT.PBP\n"));
    TEST_CHECK(!strcmp(slurp("ux0:data/VitaJPOverlay/psp/PSPEMU006.txt", NULL), "\nUnreadable Six\nux0:pspemu/ISO/MISSING.ISO\n"));
    TEST_CHECK(!strcmp(slurp("ux0:data/VitaJPOverlay/psp/OLDINF001.txt", NULL),
                       "ULJM05001\nSynthetic Old Game\nux0:pspemu/ISO/OLD.ISO\n"));
    TEST_CHECK(!slurp("ux0:data/VitaJPOverlay/psp/PSPEMUCFW.txt", NULL) && !slurp("ux0:data/VitaJPOverlay/psp/NPXS10028.txt", NULL));
    /* The log names each bubble, its target and what it found there. */
    TEST_CHECK(app_logged("games: bubble ULJS00378 (boot.bin): ux0:pspemu/ISO/Synthetic.iso: ISO, disc ULJS00378, "
                          "\"合成テスト PSP\" in ") != NULL);
    TEST_CHECK(app_logged("games: bubble PSPEMU005 (boot.bin): ux0:pspemu/PSP/GAME/NPJH90001/EBOOT.PBP: PBP, disc NPJH90001") != NULL);
    TEST_CHECK(app_logged("games: bubble PSPEMU006 (boot.bin): ux0:pspemu/ISO/MISSING.ISO: unreadable, disc ?") != NULL);
    TEST_CHECK(app_logged("games: bubble OLDINF001 (boot.inf): ux0:pspemu/ISO/OLD.ISO: ISO, disc ULJM05001") != NULL);
    TEST_CHECK(app_logged("games: PSPEMUCFW: Adrenaline") != NULL);
    const char *all = slurp(APP_LOG, NULL);
    TEST_MSG("log.txt:\n%s", all ? all : "(none)");
    /* The first run wrote the four descriptions; a second rewrites none
     * (unchanged) and logs no bubble line (a console append costs 12-46 ms). */
    TEST_CHECK_(session->description_writes == 4, "%d description writes", session->description_writes);
    unlink(APP_LOG);
    at = 0;
    hold(0, 5);
    run(at);
    TEST_CHECK_(session->description_writes == 0, "%d description writes", session->description_writes);
    TEST_CHECK(!app_logged("games: bubble") && app_logged("games: PSPEMUCFW: Adrenaline"));
    TEST_CHECK(slurp("ux0:data/VitaJPOverlay/psp/ULJS00378.txt", NULL) != NULL);
}

/* Enter on a bubble turns it OFF and ON as any game: games.ini changes, and no taiHEN configuration
 * is touched or copied (the kernel plugin loads the worker into a game that is ON, #138). */
static void enter_toggles_a_bubble_and_writes_no_configuration(void)
{
    bubble_card();
    mkdir("ux0:tai", 0777);
    put_file("ux0:tai/config.txt", "*main\r\nux0:tai/x.suprx\r\n", 24);
    at = 0;
    hold(0, 20);
    goto_row("ULJS00378");
    int off_at = at;
    tap(SCE_CTRL_CROSS, 6); /* OFF */
    int on_at = at;
    tap(SCE_CTRL_CROSS, 6); /* ON */
    hold(0, 4);
    int frames = run(at), last = frames - 1;
    check_rules();
    TEST_CHECK(!strcmp(slurp("ur0:tai/config.txt", NULL), tai_base));
    TEST_CHECK(!strcmp(slurp("ux0:tai/config.txt", NULL), "*main\r\nux0:tai/x.suprx\r\n"));
    TEST_CHECK(!slurp("ur0:tai/config.txt.vjo-app-bak", NULL) && !slurp("ux0:tai/config.txt.vjo-app-bak", NULL));
    TEST_CHECK(!strncmp(session->shown[off_at + 2].status[0], "Saved.", 6));
    TEST_CHECK(!strncmp(session->shown[on_at + 2].status[0], "Saved.", 6));
    const char *ini = slurp("ux0:data/VitaJPOverlay/games.ini", NULL);
    TEST_CHECK(ini && strstr(ini, "ULJS00378 = enabled"));
    TEST_MSG("%s", ini);
    TEST_CHECK(!app_logged("taiHEN:"));
    TEST_CHECK(card_of(last, "ULJS00378") >= 0 && session->shown[last].tag[card_of(last, "ULJS00378")] == ACCENT);
}

/* AdrBubbleBooter reads a boot.bin of exactly 0x140 bytes as its layout
 * (the path at 0x40) and any other size as the older one (at 0x20): one of
 * 0x150 bytes is the older layout, so the app reads one byte past 0x140. */
static void a_longer_boot_bin_is_the_older_layout(void)
{
    uint8_t boot[0x150] = {0};
    bubble_card();
    put_app("PSPEMU007", "Bubble Seven", 0, "ux0:pspemu/ISO/UNUSED.ISO", 0);
    memcpy(boot, "ABB", 4);
    memcpy(boot + 0x20, "ux0:pspemu/ISO/OLD.ISO", 22);
    put_file("ux0:app/PSPEMU007/data/boot.bin", boot, sizeof(boot));
    add_game("PSPEMU007", "Synthetic Old Game");
    spec_list();
    at = 0;
    hold(0, 5);
    run(at);
    TEST_CHECK(!strcmp(slurp("ux0:data/VitaJPOverlay/psp/PSPEMU007.txt", NULL), "ULJM05001\nSynthetic Old Game\nux0:pspemu/ISO/OLD.ISO\n"));
}

/* Start reads the catalog again into the same rows: a row that was a bubble
 * keeps nothing of it when a Vita game takes its place (kind, a disc ID).
 * Here every PSP app goes and Vita games
 * come, so each of them lands where an old row was. */
static void remove_tree_at(const char *path) { remove_tree(path); }
static void bubbles_become_games(int f)
{
    if (f != 40) return;
    static const char *const gone[] = {"ULJS00378", "PSPEMU005", "PSPEMU006", "OLDINF001", "PSPEMUCFW"};
    char p[64];
    for (unsigned i = 0; i < sizeof(gone) / sizeof(gone[0]); i++) {
        snprintf(p, sizeof(p), "ux0:app/%s", gone[i]);
        remove_tree_at(p);
    }
    for (int i = 4; i < 9; i++) {
        Game g;
        memset(&g, 0, sizeof(g));
        snprintf(g.id, sizeof(g.id), "VJOT%05d", i);
        make_title(i, g.title, sizeof(g.title));
        g.icon = ICON_NONE;
        install(&g, i);
    }
}
static void a_rescan_keeps_nothing_of_old_rows(void)
{
    bubble_card();
    at = 0;
    hold(0, 20);
    goto_row("ULJS00378");
    tap(SCE_CTRL_CROSS, 6); /* turned OFF: the row's choice changes */
    hold(0, 40 - at > 0 ? 40 - at : 1);
    int start = at;
    tap(SCE_CTRL_START, 10);
    for (int i = 0; i < 12; i++) tap(DOWN, 3);
    hold(0, 4);
    frame_hook = bubbles_become_games;
    int frames = run(at);
    frame_hook = NULL;
    int vita_cards = 0, tagged = 0;
    for (int f = start + 2; f < frames; f++)
        for (int r = 0; r < ROWS; r++) {
            const char *id = session->shown[f].id[r];
            if (!strncmp(id, "VJOT", 4)) {
                vita_cards++;
                if (session->shown[f].tag[r]) tagged++;
            }
            if (!strcmp(id, "NPXS10028")) TEST_CHECK_(session->shown[f].tag[r] == ACCENT, "frame %d", f);
            TEST_CHECK_(strcmp(id, "PSPEMUCFW"), "frame %d: %s still listed", f, id);
        }
    TEST_CHECK_(vita_cards > 9 && !tagged, "%d Vita cards, %d tagged", vita_cards, tagged);
    /* The status is no stale registration message, and the gone bubbles' descriptions went. */
    TEST_CHECK(!slurp("ux0:data/VitaJPOverlay/psp/ULJS00378.txt", NULL) && !slurp("ux0:data/VitaJPOverlay/psp/PSPEMU005.txt", NULL));
}

/* A bubble whose own SFO has a title too long for the description beside a
 * long target path (only a hand-made SFO has one): its description is still
 * written, with the title cut at a whole character. */
static void a_long_title_is_cut_to_fit(void)
{
    char title[256], target[256], p[300];
    bubble_card();
    memset(title, 0, sizeof(title));
    for (int i = 0; i < 85; i++) memcpy(title + 3 * i, "\xE3\x81\x82", 3); /* 85 x あ, 255 B */
    snprintf(target, sizeof(target), "ux0:pspemu/ISO/");
    memset(target + 15, 'L', 240); /* 255 B, the most a boot.bin holds: 513 B with the title, over 512 */
    target[255] = '\0';
    put_app("PSPEMU009", "x", 0, target, 0); /* the target does not exist: its own title is used */
    snprintf(p, sizeof(p), "ux0:app/PSPEMU009/sce_sys/param.sfo");
    put_sfo(p, title);
    add_game("PSPEMU009", title);
    spec_list();
    at = 0;
    hold(0, 5);
    run(at);
    size_t n;
    const char *d = slurp("ux0:data/VitaJPOverlay/psp/PSPEMU009.txt", &n);
    TEST_ASSERT(d != NULL);
    const char *t = strchr(d, '\n') + 1, *e = strchr(t, '\n');
    TEST_ASSERT(e != NULL);
    TEST_CHECK_(n < VJO_PSP_INFO_MAX && (size_t)(e - t) % 3 == 0 && e - t > 0 && !strncmp(t, title, (size_t)(e - t)) &&
                !strncmp(e + 1, target, strlen(target)) && e[1 + strlen(target)] == '\n', "%zu B: %.*s", n, (int)(e - t), t);
}

/* Folders named ALL or KERNEL (taiHEN's reserved sections) or anything that
 * is not a title ID, even with a bubble's files, are listed as they are, never
 * as bubbles: Enter only turns them ON or OFF. */
static void reserved_names_are_listed_as_they_are(void)
{
    static const char *const ids[] = {"ALL", "KERNEL", "PSPEMU01"};
    bubble_card();
    put_app("ALL", "All Folder", 0, "/ISO/OLD.ISO", 1);
    add_game("ALL", "All Folder");
    put_app("KERNEL", "Kernel Folder", 0, "ux0:pspemu/ISO/OLD.ISO", 0);
    add_game("KERNEL", "Kernel Folder");
    put_app("PSPEMU01", "Not Quite", 0, "ux0:pspemu/ISO/OLD.ISO", 0);
    add_game("PSPEMU01", "Not Quite");
    spec_list();
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        put_file("ur0:tai/config.txt", tai_base, strlen(tai_base));
        at = 0;
        hold(0, 20);
        goto_row(ids[i]);
        int before = at - 1, tried = at;
        tap(SCE_CTRL_CROSS, 6);
        int frames = run(at);
        TEST_CHECK_(!strcmp(slurp("ur0:tai/config.txt", NULL), tai_base), "%s", ids[i]);
        TEST_CHECK_(!slurp("ur0:tai/config.txt.vjo-app-bak", NULL), "%s", ids[i]);
        TEST_CHECK_(!strncmp(session->shown[tried + 2].status[0], "Saved.", 6), "%s: %s", ids[i],
                    session->shown[tried + 2].status[0]);
        int r = card_of(before, ids[i]), r2 = card_of(frames - 1, ids[i]);
        TEST_CHECK_(r >= 0 && r2 >= 0 && !session->shown[before].tag[r] && !session->shown[frames - 1].tag[r2],
                    "%s tagged", ids[i]);
        char file[96];
        snprintf(file, sizeof(file), "ux0:data/VitaJPOverlay/psp/%s.txt", ids[i]);
        TEST_CHECK_(!slurp(file, NULL), "%s described", ids[i]);
    }
}

/* A description tells the Shell an ID is a bubble's: those of IDs that are
 * no bubble's now go at the scan (a deleted bubble, an ID used by a Vita app),
 * the bubbles' stay, and other files are left alone. */
static void stale_descriptions_are_removed(void)
{
    bubble_card();
    mkdir("ux0:data/VitaJPOverlay/psp", 0777);
    put_file("ux0:data/VitaJPOverlay/psp/GONE00001.txt", "ULJS00001\nGone\nux0:pspemu/ISO/GONE.ISO\n", 38);
    put_file("ux0:data/VitaJPOverlay/psp/VJOT00001.txt", "\nNot a bubble\nux0:x\n", 21);
    put_file("ux0:data/VitaJPOverlay/psp/notes.md", "kept", 4);
    /* ux0:app not listed: no bubble is known, so none is removed. */
    app_dir_fails = 1;
    at = 0;
    hold(0, 5);
    run(at);
    app_dir_fails = 0;
    TEST_CHECK(slurp("ux0:data/VitaJPOverlay/psp/GONE00001.txt", NULL) && slurp("ux0:data/VitaJPOverlay/psp/VJOT00001.txt", NULL));
    at = 0;
    hold(0, 5);
    run(at);
    TEST_CHECK(!slurp("ux0:data/VitaJPOverlay/psp/GONE00001.txt", NULL) && !slurp("ux0:data/VitaJPOverlay/psp/VJOT00001.txt", NULL));
    TEST_CHECK(slurp("ux0:data/VitaJPOverlay/psp/notes.md", NULL) && slurp("ux0:data/VitaJPOverlay/psp/ULJS00378.txt", NULL) &&
               slurp("ux0:data/VitaJPOverlay/psp/OLDINF001.txt", NULL));
    TEST_CHECK(app_logged("games: ux0:data/VitaJPOverlay/psp/GONE00001.txt removed: no bubble has that ID now") != NULL);
}

/* ---- the catalog's log, a game card, and titles in no folder (#138) ---- */

/* The log line, without "Rubi app: " and its line break, that starts with
 * prefix (the first such), or NULL. */
static const char *logged_line(const char *prefix)
{
    static char line[1024];
    FILE *f = fopen(APP_LOG, "r");
    const char *found = NULL;
    size_t n = strlen(prefix);
    while (f && !found && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        if (!strncmp(line, "Rubi app: ", 10) && !strncmp(line + 10, prefix, n)) found = line + 10;
    }
    if (f) fclose(f);
    return found;
}
/* How many log lines start with "Rubi app: " and prefix. */
static int logged_lines(const char *prefix)
{
    char line[1024];
    FILE *f = fopen(APP_LOG, "r");
    int n = 0;
    while (f && fgets(line, sizeof(line), f))
        n += !strncmp(line, "Rubi app: ", 10) && !strncmp(line + 10, prefix, strlen(prefix));
    if (f) fclose(f);
    return n;
}
#define CHECK_LINE(prefix, want)                                                                    \
    do {                                                                                            \
        const char *got_ = logged_line(prefix);                                                     \
        TEST_CHECK_(got_ && !strcmp(got_, want), "%s\n  got:  %s\n  want: %s", prefix, got_ ? got_ : "(none)", want); \
    } while (0)

/* A game only on the game card (gro0:app; CLANNAD may be one, #138) is listed
 * with the card's title and icon; one on both cards once, from ux0:app; a file
 * on the card is skipped. Each root's line says what it read, the rows' line
 * what was listed, and safe mode is off (VitaShell's check answers EINVAL). */
static void a_cartridge_only_title_is_listed(void)
{
    card(6, NULL);
    Game cart = {"PCSG00415", "Synthetic Card Game", ICON_OK};
    install_in("gro0:app", &cart, 415);
    install_in("gro0:app", &games[1], 1); /* on the memory card too */
    put_file("gro0:app/readme.txt", "x", 1);
    add_game(cart.id, cart.title);
    games[game_count - 1].icon = ICON_OK;
    spec_list();
    at = 0;
    hold(0, 20);
    goto_row("PCSG00415");
    hold(0, 12);
    int frames = run(at), last = frames - 1;
    check_rules();
    TEST_CHECK(check_cards(frames, NULL) == 0);
    int r = card_of(last, "PCSG00415");
    TEST_ASSERT(r >= 0);
    const Frame *f = &session->shown[last];
    TEST_CHECK_(!strcmp(f->title[r][0], "Synthetic Card Game") && !strcmp(f->icon[r], "PCSG00415") &&
                !strcmp(f->badge[r], "ON") && !f->tag[r], "'%s' icon '%s' badge '%s' tag 0x%08X", f->title[r][0],
                f->icon[r], f->badge[r], f->tag[r]);
    /* once: one card in any frame */
    for (int k = 0; k < frames; k++) {
        int seen = 0;
        for (int q = 0; q < ROWS; q++) seen += !strcmp(session->shown[k].id[q], games[1].id);
        TEST_CHECK_(seen <= 1, "frame %d: %s on %d cards", k, games[1].id, seen);
    }
    /* API 31: CLANNAD's row has Rubi's own hook entry; no other row a hook tag, the PSP row none either */
    for (int k = 0; k < frames; k++)
        for (int q = 0; q < ROWS; q++) {
            const char *id = session->shown[k].id[q];
            if (!id[0]) continue;
            TEST_CHECK_(!strcmp(session->shown[k].hook[q], !strcmp(id, "PCSG00415") ? "Hook" : ""),
                        "frame %d %s: hook tag '%s'", k, id, session->shown[k].hook[q]);
        }
    CHECK_LINE("games: ux0:app", "games: ux0:app: 6 entries: 6 listed, 0 already listed, 0 skipped");
    CHECK_LINE("games: ur0:app", "games: ur0:app: not opened: 0x80010002 (ENOENT); ur0: 0x80010013 (ENODEV)");
    CHECK_LINE("games: gro0:app", "games: gro0:app: 3 entries: 1 listed, 1 already listed, 1 skipped; skipped: "
                                  "readme.txt (not a folder)");
    CHECK_LINE("games: 8 rows", "games: 8 rows (the list holds 512): 7 from the folders, the shared PSP row, 0 only in "
                                "games.ini; safe mode no (ux0: devctl 0x80010016)");
    /* a scan writes four lines: each root's and the rows' */
    TEST_CHECK_(logged_lines("games: ") == 4, "%d lines", logged_lines("games: "));
}

/* gro0:app refused (as a game card might be to the app in #138's run): its
 * line names the error, and the titles with a choice of their own in
 * games.ini that no folder had are listed, with what they are: an update's
 * title and icon when ux0:patch has one, else the ID. Toggled, the choice is
 * saved and its row stays; a rule that is not a game's (NPXS10001) gets none. */
static void titles_with_a_choice_but_in_no_folder_are_listed(void)
{
    static const char ini[] = "# Rubi per-game settings\ndefault = enabled\nPCSG00415 = enabled\nPCSB00792 = disabled\n"
                              "NPXS10001 = enabled\nVJOT00002 = disabled\n";
    card(5, NULL);
    dir_fails = "gro0:app";
    dir_fails_rc = (int)0x80010013u;
    Game cart = {"PCSG00415", "Synthetic Card Game", ICON_OK};
    install_in("gro0:app", &cart, 415); /* on the card, which the app cannot list */
    Game update = {"PCSB00792", "Synthetic Patched Game", ICON_OK};
    install_in("ux0:patch", &update, 792);
    put_file("ux0:data/VitaJPOverlay/games.ini", ini, sizeof(ini) - 1);
    add_game("PCSG00415", "PCSG00415" VJO_CATALOG_MISSING_NOTE);
    add_game("PCSB00792", "Synthetic Patched Game" VJO_CATALOG_MISSING_NOTE);
    games[game_count - 1].icon = ICON_OK; /* the update's */
    spec_list();
    at = 0;
    hold(0, 20);
    goto_row("PCSG00415");
    tap(ENTER, 10);
    int toggled = at - 1;
    int frames = run(at);
    check_rules();
    TEST_CHECK(check_cards(frames, NULL) == 0);
    for (int k = 0; k < frames; k++)
        for (int q = 0; q < ROWS; q++) TEST_CHECK_(strcmp(session->shown[k].id[q], "NPXS10001"), "frame %d", k);
    int r = card_of(toggled, "PCSG00415");
    TEST_ASSERT(r >= 0);
    TEST_CHECK_(!strcmp(session->shown[toggled].badge[r], "OFF") && !session->shown[toggled].tag[r] &&
                !strcmp(session->shown[toggled].icon[r], "JP"), "badge '%s'", session->shown[toggled].badge[r]);
    TEST_CHECK(!strncmp(session->shown[toggled].status[0], "Saved.", 6));
    const char *ini2 = slurp("ux0:data/VitaJPOverlay/games.ini", NULL);
    TEST_CHECK(ini2 && strstr(ini2, "\nPCSG00415 = disabled\n") && strstr(ini2, "\nPCSB00792 = disabled\n"));
    CHECK_LINE("games: gro0:app", "games: gro0:app: not opened: 0x80010013 (ENODEV); gro0: is there");
    CHECK_LINE("games: 8 rows", "games: 8 rows (the list holds 512): 5 from the folders, the shared PSP row, 2 only in "
                                "games.ini; safe mode no (ux0: devctl 0x80010016); only in games.ini: "
                                "PCSG00415 = enabled, PCSB00792 = disabled");
    /* The next start: still listed, OFF, by its saved choice. */
    unlink(APP_LOG);
    at = 0;
    hold(0, 20);
    goto_row("PCSG00415");
    frames = run(at);
    r = card_of(frames - 1, "PCSG00415");
    TEST_CHECK_(r >= 0 && !strcmp(session->shown[frames - 1].badge[r], "OFF"), "after a restart: card %d", r);
    TEST_CHECK(logged_line("games: 8 rows") && strstr(logged_line("games: 8 rows"), "PCSG00415 = disabled"));
}

/* What a root holds that is no game's is skipped, and its line says why:
 * VitaShell, the system's and the Rubi app's own, names that are not title
 * IDs, taiHEN's name for SceShell, files. Names past 24 bytes are cut; past the
 * line's room the rest are counted, and the line fits one vjo_log line. A game
 * whose param.sfo gives no title is listed by its ID, and the line names it. */
static void skipped_folders_are_logged_with_why(void)
{
    static const char *const odd[] = {"VITASHELL", "NPXS10001", "VJOSET001", "Not An ID", "mainmenu",
                                      "A_FOLDER_NAME_LONGER_THAN_24_BYTES"};
    char p[96];
    card(3, NULL);
    for (unsigned i = 0; i < sizeof(odd) / sizeof(odd[0]); i++) {
        snprintf(p, sizeof(p), "ux0:app/%s", odd[i]);
        mkdir(p, 0777);
    }
    put_file("ux0:app/notes.txt", "x", 1);
    spec_list();
    at = 0;
    hold(0, 5);
    int frames = run(at);
    TEST_CHECK(check_cards(frames, NULL) == 0);
    const char *line = logged_line("games: ux0:app");
    TEST_ASSERT(line != NULL);
    static const char *const why[] = {"VITASHELL (VitaShell)", "NPXS10001 (a system app)", "VJOSET001 (this app)",
                                      "Not An ID (not a title ID)", "mainmenu (SceShell's name)",
                                      "A_FOLDER_NAME_LONGER_THA... (not a title ID)", "notes.txt (not a folder)"};
    static const char head[] = "games: ux0:app: 10 entries: 3 listed, 0 already listed, 7 skipped; skipped: ";
    TEST_CHECK_(!strncmp(line, head, sizeof(head) - 1) && !strstr(line, "more"), "%s", line);
    for (unsigned i = 0; i < sizeof(why) / sizeof(why[0]); i++) TEST_CHECK_(strstr(line, why[i]) != NULL, "%s: %s", why[i], line);
    /* 40 system folders: as many named as fit, the rest counted */
    card(3, NULL);
    for (int i = 0; i < 40; i++) {
        snprintf(p, sizeof(p), "ux0:app/NPXS%05d", 10100 + i);
        mkdir(p, 0777);
    }
    at = 0;
    hold(0, 5);
    run(at);
    line = logged_line("games: ux0:app");
    TEST_ASSERT(line != NULL);
    int named = 0;
    for (const char *s = strstr(line, "(a system app)"); s; s = strstr(s + 1, "(a system app)")) named++;
    char tail[48];
    snprintf(tail, sizeof(tail), " and %d more", 40 - named);
    TEST_CHECK_(named >= 3 && named < 40 && strlen(line) >= strlen(tail) && !strcmp(line + strlen(line) - strlen(tail), tail) &&
                strlen("Rubi app: ") + strlen(line) + 1 <= 300, "%d named, %zu B: %s", named, strlen(line), line);
    /* no TITLE in its param.sfo: listed by its ID, and named */
    card(3, NULL);
    put_file("ux0:app/VJOT00002/sce_sys/param.sfo", "not an SFO", 10);
    snprintf(games[2].title, sizeof(games[2].title), "%s", games[2].id);
    spec_list();
    at = 0;
    hold(0, 5);
    frames = run(at);
    TEST_CHECK(check_cards(frames, NULL) == 0);
    CHECK_LINE("games: ux0:app", "games: ux0:app: 3 entries: 3 listed, 0 already listed, 0 skipped, 1 listed under its "
                                 "ID (no title read); under its ID: VJOT00002");
    /* 40 games without a title and 5 skipped folders: every count, the skipped names
     * and reasons first, whole; the IDs after them as many as fit; no label cut */
    card(40, NULL);
    for (int i = 0; i < 40; i++) {
        snprintf(p, sizeof(p), "ux0:app/VJOT%05d/sce_sys/param.sfo", i);
        put_file(p, "not an SFO", 10);
    }
    static const char *const five[] = {"NPXS10001", "NPXS10002", "VITASHELL", "VJOSET001", "mainmenu"};
    for (unsigned i = 0; i < 5; i++) {
        snprintf(p, sizeof(p), "ux0:app/%s", five[i]);
        mkdir(p, 0777);
    }
    for (int i = 0; i < 40; i++) snprintf(games[i].title, sizeof(games[i].title), "%s", games[i].id);
    spec_list();
    at = 0;
    hold(0, 5);
    run(at);
    line = logged_line("games: ux0:app");
    TEST_ASSERT(line != NULL);
    static const char head2[] = "games: ux0:app: 45 entries: 40 listed, 0 already listed, 5 skipped, 40 listed under its "
                                "ID (no title read); skipped: ";
    TEST_CHECK_(!strncmp(line, head2, sizeof(head2) - 1), "%s", line);
    static const char *const why5[] = {"NPXS10001 (a system app)", "NPXS10002 (a system app)", "VITASHELL (VitaShell)",
                                       "VJOSET001 (this app)", "mainmenu (SceShell's name)"};
    for (unsigned i = 0; i < 5; i++) TEST_CHECK_(strstr(line, why5[i]) != NULL, "%s: %s", why5[i], line);
    const char *ids = strstr(line, "; under its ID: ");
    size_t n = strlen(line);
    TEST_CHECK_(n + 14 <= 300 && (!ids || ids > strstr(line, "; skipped: ")), "%zu B: %s", n, line);
    TEST_CHECK_(ids && n > 5 && !strcmp(line + n - 5, " more"), "%s", line); /* not all 40 IDs fit */
    /* 30 long skipped names fill the line before the IDs (the audit's A11): the skipped
     * section is counted where it runs out, the IDs' section is dropped whole, nothing is cut */
    card(40, NULL);
    for (int i = 0; i < 40; i++) {
        snprintf(p, sizeof(p), "ux0:app/VJOT%05d/sce_sys/param.sfo", i);
        put_file(p, "not an SFO", 10);
    }
    for (int i = 0; i < 30; i++) {
        snprintf(p, sizeof(p), "ux0:app/A_FOLDER_NAME_LONGER_THAN_24_BYTES_%02d", i);
        mkdir(p, 0777);
    }
    for (int i = 0; i < 40; i++) snprintf(games[i].title, sizeof(games[i].title), "%s", games[i].id);
    spec_list();
    at = 0;
    hold(0, 5);
    run(at);
    line = logged_line("games: ux0:app");
    TEST_ASSERT(line != NULL);
    static const char head3[] = "games: ux0:app: 70 entries: 40 listed, 0 already listed, 30 skipped, 40 listed under its "
                                "ID (no title read); skipped: A_FOLDER_NAME_LONGER_THA... (not a title ID)";
    TEST_CHECK_(!strncmp(line, head3, sizeof(head3) - 1), "%s", line);
    n = strlen(line);
    /* the skipped section ends with its own count; no IDs' label; within one log line */
    TEST_CHECK_(n + 14 <= 300 && n > 5 && !strcmp(line + n - 5, " more") && !strstr(line, "under its ID:"), "%zu B: %s", n, line);
    /* every item is whole: a name and its reason, then ", " or " and N more" */
    int items = 0, broken = 0;
    for (const char *q = strstr(line, "; skipped: ") + 11; *q; ) {
        const char *close = strstr(q, " (not a title ID)");
        if (!close || strncmp(q, "A_FOLDER_NAME_LONGER_THA...", 27)) { broken++; break; }
        items++;
        q = close + 17;
        if (!strncmp(q, ", ", 2)) q += 2;
        else { if (strncmp(q, " and ", 5)) broken++; break; }
    }
    TEST_CHECK_(items >= 3 && !broken, "%d items, broken %d: %s", items, broken, line);
    /* a name past 24 bytes is cut at a whole character: never inside one */
    card(1, NULL);
    mkdir("ux0:app/A\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86\xE3\x81\x88\xE3\x81\x8A\xE3\x81\x8B\xE3\x81\x8D\xE3\x81\x8F\xE3\x81\x91", 0777);
    spec_list();
    at = 0;
    hold(0, 5);
    run(at);
    CHECK_LINE("games: ux0:app", "games: ux0:app: 2 entries: 1 listed, 0 already listed, 1 skipped; skipped: "
                                 "A\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86\xE3\x81\x88\xE3\x81\x8A\xE3\x81\x8B\xE3\x81\x8D... (not a title ID)");
}

/* A listing that fails part way (EIO) says so, and ux0:app's, incomplete,
 * decides no stale bubble description: they go only after a whole listing. */
static void a_listing_that_stops_is_logged_and_prunes_nothing(void)
{
    bubble_card();
    mkdir("ux0:data/VitaJPOverlay/psp", 0777);
    put_file("ux0:data/VitaJPOverlay/psp/GONE00001.txt", "ULJS00001\nGone\nux0:pspemu/ISO/GONE.ISO\n", 38);
    dread_fails = "ux0:app";
    dread_fails_after = 2;
    at = 0;
    hold(0, 5);
    run(at);
    const char *line = logged_line("games: ux0:app");
    TEST_CHECK_(line && !strncmp(line, "games: ux0:app: 2 entries: ", strlen("games: ux0:app: 2 entries: ")) &&
                strstr(line, "; the listing stopped at 0x80010005 (EIO)"), "%s", line ? line : "(none)");
    TEST_CHECK(slurp("ux0:data/VitaJPOverlay/psp/GONE00001.txt", NULL) != NULL);
    dread_fails = NULL;
    at = 0;
    hold(0, 5);
    run(at);
    TEST_CHECK(!slurp("ux0:data/VitaJPOverlay/psp/GONE00001.txt", NULL));
}

/* More titles than the list holds: each root's line and the rows' line say how
 * many were left out; VitaShell's safe-mode check, when it says so, is logged. */
static void the_rows_line_counts_what_was_left_out_and_safe_mode(void)
{
    card(520, NULL);
    safe_mode = 1;
    at = 0;
    hold(0, 5);
    run(at);
    CHECK_LINE("games: ux0:app", "games: ux0:app: 520 entries: 512 listed, 0 already listed, 0 skipped, 8 left out: "
                                 "the list holds 512");
    CHECK_LINE("games: 512 rows", "games: 512 rows (the list holds 512): 512 from the folders, 0 only in games.ini, 8 left "
                                  "out; safe mode yes (ux0: devctl 0x80010030)");
}

/* A scan's lines go out in one append (settings/applog.c), after the start
 * line's: an append cost the console 12-46 ms in #81's timestamps. */
static void a_scan_logs_in_one_append(void)
{
    char lines[8][256];
    card(5, NULL);
    at = 0;
    hold(0, 5);
    TEST_CHECK(run(at) == at);
    TEST_CHECK_(session->log_appends == 2, "%d appends", session->log_appends);
    TEST_CHECK(log_lines(lines, 8) == 1 && logged_lines("games: ") == 4);
    /* Start scans again: one more append */
    card(5, NULL);
    at = 0;
    hold(0, 5);
    tap(SCE_CTRL_START, 5);
    TEST_CHECK(run(at) == at);
    TEST_CHECK_(session->log_appends == 3 && logged_lines("games: ") == 8, "%d appends, %d lines", session->log_appends,
                logged_lines("games: "));
}

/* The batch, directly: its lines in order in one append at its end; another
 * thread's line (the Anki worker's, during an export) goes out at once, never
 * into the batch; a batch fuller than its buffer goes out in pieces, in order,
 * losing nothing; with log_file off nothing is written. */
static void the_log_batch_keeps_order_and_threads_apart(void)
{
    char want[8192], got[8192];
    size_t used = 0;
    card(1, NULL);
    unlink(APP_LOG);
    vjo_log_enable(1);
    log_appends = 0;
    vjo_app_log_batch_begin();
    vjo_log("first");
    thread_id = 0x40010099; /* the worker */
    vjo_log("worker");
    vjo_app_log_batch_end();    /* not its batch: nothing */
    thread_id = 0x40010003;
    vjo_log("second");
    TEST_CHECK_(log_appends == 1, "%d appends before the end", log_appends);
    vjo_app_log_batch_end();
    TEST_CHECK_(log_appends == 2, "%d appends", log_appends);
    const char *text = slurp(APP_LOG, NULL);
    TEST_CHECK_(text && !strcmp(text, "Rubi app: worker\nRubi app: first\nRubi app: second\n"), "%s", text ? text : "");
    /* 30 lines of 200 B: more than the batch holds */
    unlink(APP_LOG);
    log_appends = 0;
    vjo_app_log_batch_begin();
    for (int i = 0; i < 30; i++) {
        char body[190];
        memset(body, 'a' + i % 26, sizeof(body) - 1);
        body[sizeof(body) - 1] = '\0';
        vjo_log("%02d %s", i, body);
        used += (size_t)snprintf(want + used, sizeof(want) - used, "Rubi app: %02d %s\n", i, body);
    }
    vjo_app_log_batch_end();
    size_t n;
    text = slurp(APP_LOG, &n);
    snprintf(got, sizeof(got), "%s", text ? text : "");
    TEST_CHECK_(n == used && !strcmp(got, want), "%zu B, want %zu", n, used);
    TEST_CHECK_(log_appends >= 3 && log_appends <= 4, "%d appends for %zu B", log_appends, used);
    /* off: nothing, batch or not */
    unlink(APP_LOG);
    vjo_log_enable(0);
    vjo_app_log_batch_begin();
    vjo_log("hidden");
    vjo_app_log_batch_end();
    vjo_log("hidden too");
    TEST_CHECK(!slurp(APP_LOG, NULL));
}

static int shell_sees(const char *path, const char *id, int psp);
/* A PSP game's ID in no folder (PSPEMUCFW, a disc ID) follows the shared PSP
 * row once its own rule is gone, as the Shell and the kernel's loader read it
 * (psp = 1): after disable all and Default ON with NPXS10028 OFF, both rows
 * show OFF, and both carry the PSP tag (the audit's A5, with the tag). */
static void missing_psp_rows_follow_the_shared_row(void)
{
    static const char ini[] = "# Rubi per-game settings\ndefault = enabled\nNPXS10028 = disabled\nPSPEMUCFW = enabled\n"
                              "ULJS00378 = enabled\n";
    card(3, NULL);
    put_file("ux0:data/VitaJPOverlay/games.ini", ini, sizeof(ini) - 1);
    add_game("PSPEMUCFW", "PSPEMUCFW" VJO_CATALOG_MISSING_NOTE);
    add_game("ULJS00378", "ULJS00378" VJO_CATALOG_MISSING_NOTE);
    spec_list();
    at = 0;
    hold(0, 10);
    int before = at - 1;
    tap(SCE_CTRL_TRIANGLE, 10);                      /* disable all */
    script[at++] = (Input){0, 1, 100, 112 + 20, 0}; /* the Default row: OFF -> ON */
    hold(0, 10);
    goto_row("PSPEMUCFW");
    int at_cfw = at - 1;
    int idx_cfw = list_index("PSPEMUCFW"), idx_disc = list_index("ULJS00378");
    for (int i = idx_cfw; i < idx_disc; i++) tap(DOWN, 3);
    hold(0, 4);
    int at_disc = at - 1;
    int frames = run(at);
    check_rules();
    TEST_CHECK(check_cards(frames, NULL) == 0);
    const char *now = slurp("ux0:data/VitaJPOverlay/games.ini", NULL);
    TEST_CHECK_(now && !strcmp(now, "# Rubi per-game settings\ndefault = enabled\nNPXS10028 = disabled\n"), "%s",
                now ? now : "");
    TEST_CHECK(!shell_sees("ux0:data/VitaJPOverlay/games.ini", "PSPEMUCFW", 1) &&
               !shell_sees("ux0:data/VitaJPOverlay/games.ini", "ULJS00378", 1));
    int r = card_of(at_cfw, "PSPEMUCFW"), d = card_of(at_disc, "ULJS00378");
    TEST_CHECK_(r >= 0 && !strcmp(session->shown[at_cfw].badge[r], "OFF"), "PSPEMUCFW card %d", r);
    TEST_CHECK_(d >= 0 && !strcmp(session->shown[at_disc].badge[d], "OFF"), "ULJS00378 card %d", d);
    /* the tag: a PSP game's row, before and after (accent: no registration to offer) */
    TEST_CHECK_(r >= 0 && session->shown[at_cfw].tag[r] == ACCENT, "PSPEMUCFW tag 0x%08X", r >= 0 ? session->shown[at_cfw].tag[r] : 0);
    TEST_CHECK_(d >= 0 && session->shown[at_disc].tag[d] == ACCENT, "ULJS00378 tag 0x%08X", d >= 0 ? session->shown[at_disc].tag[d] : 0);
    int b = card_of(before, "PSPEMUCFW");
    TEST_CHECK_(b < 0 || session->shown[before].tag[b] == ACCENT, "before: tag 0x%08X", session->shown[before].tag[b]);
}

/* log.txt's 1 MiB cap (settings/applog.c): a scan's batch writes each of its
 * lines that still fits, in order, as the writer did line by line; the file
 * never passes the cap and ends at a whole line. Checked against the lines an
 * uncapped run writes, from several distances to the cap. */
static void the_log_cap_keeps_whole_lines_as_before(void)
{
    static char fill[1024 * 1024];
    char lines[16][1024];
    size_t len[16];
    int count = 0;
    card(5, NULL);
    at = 0;
    hold(0, 5);
    run(at);
    FILE *f = fopen(APP_LOG, "r");
    TEST_ASSERT(f != NULL);
    while (count < 16 && fgets(lines[count], sizeof(lines[0]), f)) { len[count] = strlen(lines[count]); count++; }
    fclose(f);
    TEST_ASSERT(count >= 5); /* the start line and the scan's four */
    static const size_t room[] = {0, 1, 60, 150, 250, 400, 600, 6000};
    for (unsigned k = 0; k < sizeof(room) / sizeof(room[0]); k++) {
        size_t pre = 1024 * 1024 - room[k];
        card(5, NULL);
        memset(fill, 'x', pre);
        for (size_t i = 99; i < pre; i += 100) fill[i] = '\n';
        fill[pre - 1] = '\n';
        put_file(APP_LOG, fill, pre);
        at = 0;
        hold(0, 5);
        run(at);
        /* expected: each line kept when it fits at its turn */
        char want[16 * 1024];
        size_t want_n = 0, size = pre;
        for (int i = 0; i < count; i++)
            if (size + len[i] <= 1024 * 1024) { memcpy(want + want_n, lines[i], len[i]); want_n += len[i]; size += len[i]; }
        /* the file: its size, and what follows the prefill (slurp holds 16 KiB) */
        struct stat st;
        char tail[16 * 1024];
        TEST_ASSERT(stat(APP_LOG, &st) == 0);
        size_t n = (size_t)st.st_size, got = 0;
        FILE *g = fopen(APP_LOG, "rb");
        TEST_ASSERT(g != NULL && fseek(g, (long)pre - 1, SEEK_SET) == 0);
        got = fread(tail, 1, sizeof(tail), g);
        fclose(g);
        TEST_CHECK_(n <= 1024 * 1024 && n == pre + want_n && got == want_n + 1 && tail[0] == '\n' &&
                    !memcmp(tail + 1, want, want_n), "room %zu: %zu B, want %zu", room[k], n, pre + want_n);
    }
    /* Batches written directly (the audit's A6 cases): lines of exact sizes, "Rubi app: "
     * and the line break included. */
    static const struct { size_t room, len[4]; int n; const char *keep; } cases[] = {
        {100, {280}, 1, ""},                 /* one line longer than the room: nothing */
        {120, {200, 50, 250, 40}, 4, "0101"}, /* the 50 and the 40 still fit after a longer one */
        {77, {77}, 1, "1"},                  /* exactly the room: the file ends at the cap */
        {120, {200, 120}, 2, "01"},          /* after a dropped line, one of exactly the room is kept */
    };
    vjo_log_enable(1);
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        size_t pre = 1024 * 1024 - cases[c].room, want_n = 0;
        char body[4][400], want[2048];
        card(1, NULL);
        memset(fill, 'x', pre);
        for (size_t i = 99; i < pre; i += 100) fill[i] = '\n';
        fill[pre - 1] = '\n';
        put_file(APP_LOG, fill, pre);
        vjo_app_log_batch_begin();
        for (int i = 0; i < cases[c].n; i++) {
            memset(body[i], 'a' + i, cases[c].len[i] - 14);
            body[i][cases[c].len[i] - 14] = '\0';
            vjo_log("%s", body[i]);
            if (cases[c].keep[i] == '1')
                want_n += (size_t)snprintf(want + want_n, sizeof(want) - want_n, "Rubi app: %s\n", body[i]);
        }
        vjo_app_log_batch_end();
        struct stat st;
        char tail[2048];
        TEST_ASSERT(stat(APP_LOG, &st) == 0);
        FILE *g = fopen(APP_LOG, "rb");
        TEST_ASSERT(g != NULL && fseek(g, (long)pre, SEEK_SET) == 0);
        size_t got = fread(tail, 1, sizeof(tail), g);
        fclose(g);
        TEST_CHECK_((size_t)st.st_size == pre + want_n && got == want_n && !memcmp(tail, want, want_n),
                    "case %u: %lld B, want %zu", c, (long long)st.st_size, pre + want_n);
    }
    /* a file already past the cap: nothing more, in a batch or alone */
    card(1, NULL);
    memset(fill, 'x', sizeof(fill));
    fill[sizeof(fill) - 1] = '\n';
    put_file(APP_LOG, fill, sizeof(fill));
    FILE *g = fopen(APP_LOG, "ab");
    TEST_ASSERT(g != NULL && fputs("past the cap\n", g) >= 0 && fclose(g) == 0);
    vjo_app_log_batch_begin();
    vjo_log("in a batch");
    vjo_app_log_batch_end();
    vjo_log("alone");
    struct stat st;
    TEST_ASSERT(stat(APP_LOG, &st) == 0);
    TEST_CHECK_((size_t)st.st_size == sizeof(fill) + 13, "%lld B", (long long)st.st_size);
}

/* ---- Default: disable all (#138) ---- */

/* games.ini as the Shell reads it: path's rules for id (a PSP game: psp). */
static int shell_sees(const char *path, const char *id, int psp)
{
    size_t n;
    const char *text = slurp(path, &n);
    VjoGames g;
    TEST_ASSERT(text && vjo_games_parse(&g, text, n) == 0);
    return vjo_games_enabled_as(&g, id, psp);
}
/* games.ini copied at given frames, while the app runs (the child's frame hook). */
static int snap_frame[2];
static void snap_games_ini(int f)
{
    for (int i = 0; i < 2; i++) {
        if (f != snap_frame[i]) continue;
        size_t n;
        const char *text = slurp("ux0:data/VitaJPOverlay/games.ini", &n);
        char name[48];
        snprintf(name, sizeof(name), "games.snap%d", i);
        if (text) put_file(name, text, n);
    }
}

/* Default ON -> OFF (Triangle) turns every game OFF, the ones ON by a choice
 * of their own too: a listed game, a title in no folder, the shared PSP row,
 * Adrenaline; the games turned OFF one by one stay OFF. The Shell sees every
 * game OFF. A title in no folder keeps its row until the next scan and can be
 * turned ON alone. OFF -> ON (the Default row touched) turns on every game but
 * those turned OFF, and the status names them. */
static void disable_all_turns_off_the_games_on_by_their_own_choice(void)
{
    static const char ini[] = "# Rubi per-game settings\ndefault = enabled\nVJOT00001 = enabled\nVJOT00002 = disabled\n"
                              "PCSG00415 = enabled\nNPXS10028 = enabled\nPSPEMUCFW = enabled\n";
    static const char *const vita[] = {"VJOT00000", "VJOT00001", "VJOT00002", "VJOT00003", "VJOT00004", "VJOT00005",
                                       "PCSG00415", "PCSB00792"};
    static const char *const psp[] = {"NPXS10028", "PSPEMUCFW", "PSPEMU005", "ULJS00378"};
    char both[300];
    card(6, NULL);
    put_file("ux0:data/VitaJPOverlay/games.ini", ini, sizeof(ini) - 1);
    add_game("PCSG00415", "PCSG00415" VJO_CATALOG_MISSING_NOTE);
    add_game("PSPEMUCFW", "PSPEMUCFW" VJO_CATALOG_MISSING_NOTE); /* Adrenaline, not installed */
    spec_list();
    at = 0;
    hold(0, 20);
    int before = at - 1;
    tap(SCE_CTRL_TRIANGLE, 10);
    int off = at - 1;
    snap_frame[0] = at;
    goto_row("PCSG00415");
    tap(ENTER, 10);
    int picked = at - 1;
    snap_frame[1] = at;
    script[at++] = (Input){0, 1, 100, 112 + 20, 0}; /* the Default row: OFF -> ON */
    hold(0, 10);
    frame_hook = snap_games_ini;
    int frames = run(at), last = frames - 1;
    frame_hook = NULL;
    check_rules();
    TEST_CHECK(check_cards(frames, NULL) == 0);
    /* before: VJOT00001 ON, VJOT00002 OFF, the Default ON */
    TEST_CHECK(!strcmp(session->shown[before].default_badge, "ON"));
    TEST_CHECK(!strcmp(session->shown[off].default_badge, "OFF") && !strcmp(session->shown[last].default_badge, "ON"));
    /* every card of the first page OFF after Triangle */
    for (int q = 0; q < ROWS; q++)
        TEST_CHECK_(!session->shown[off].id[q][0] || !strcmp(session->shown[off].badge[q], "OFF"), "%s: %s",
                    session->shown[off].id[q], session->shown[off].badge[q]);
    snprintf(both, sizeof(both), "%s %s", session->shown[off].status[0], session->shown[off].status[1]);
    TEST_CHECK_(strstr(both, "Saved: every game is OFF from its next start or resume. Turned OFF too: ") != NULL, "%s",
                both);
    TEST_CHECK_(strstr(both, games[1].title) != NULL || strstr(both, "...") != NULL, "%s", both);
    /* the file: no "= enabled" rule, the "= disabled" one kept, the default OFF */
    const char *snap = slurp("games.snap0", NULL);
    TEST_CHECK_(snap && !strcmp(snap, "# Rubi per-game settings\ndefault = disabled\nVJOT00002 = disabled\n"), "%s",
                snap ? snap : "(none)");
    for (unsigned i = 0; i < sizeof(vita) / sizeof(vita[0]); i++)
        TEST_CHECK_(!shell_sees("games.snap0", vita[i], 0), "%s ON", vita[i]);
    for (unsigned i = 0; i < sizeof(psp) / sizeof(psp[0]); i++)
        TEST_CHECK_(!shell_sees("games.snap0", psp[i], 1), "%s ON", psp[i]);
    TEST_CHECK(!shell_sees("games.snap0", "GAME", 0)); /* the Shell's fallback without a title ID */
    /* the title in no folder: still listed, turned ON alone */
    int r = card_of(picked, "PCSG00415");
    TEST_CHECK_(r >= 0 && !strcmp(session->shown[picked].badge[r], "ON"), "card %d", r);
    snap = slurp("games.snap1", NULL);
    TEST_CHECK_(snap && !strcmp(snap, "# Rubi per-game settings\ndefault = disabled\nVJOT00002 = disabled\n"
                                      "PCSG00415 = enabled\n"), "%s", snap ? snap : "(none)");
    TEST_CHECK(shell_sees("games.snap1", "PCSG00415", 0) && !shell_sees("games.snap1", "VJOT00001", 0));
    /* Default ON again: every game ON but VJOT00002, which the status names */
    const char *now = slurp("ux0:data/VitaJPOverlay/games.ini", NULL);
    TEST_CHECK_(now && !strcmp(now, "# Rubi per-game settings\ndefault = enabled\nVJOT00002 = disabled\n"
                                    "PCSG00415 = enabled\n"), "%s", now ? now : "(none)");
    for (unsigned i = 0; i < sizeof(vita) / sizeof(vita[0]); i++)
        TEST_CHECK_(shell_sees("ux0:data/VitaJPOverlay/games.ini", vita[i], 0) == !!strcmp(vita[i], "VJOT00002"), "%s", vita[i]);
    for (unsigned i = 0; i < sizeof(psp) / sizeof(psp[0]); i++)
        TEST_CHECK_(shell_sees("ux0:data/VitaJPOverlay/games.ini", psp[i], 1), "%s OFF", psp[i]);
    snprintf(both, sizeof(both), "%s %s", session->shown[last].status[0], session->shown[last].status[1]);
    char want[320];
    snprintf(want, sizeof(want), "Saved: every game but 1 is ON from its next start or resume. Still OFF: %s.",
             games[2].title);
    TEST_CHECK_(!strcmp(both, want), "%s\n  want: %s", both, want);
    TEST_MSG("off: %s | on: %s", session->shown[off].status[0], both);
    /* log.txt: each change and the rules it removed or kept */
    CHECK_LINE("games: Default OFF", "games: Default OFF, 4 own ON choices removed: VJOT00001, PCSG00415, NPXS10028, PSPEMUCFW");
    CHECK_LINE("games: Default ON", "games: Default ON, 1 own OFF choice kept: VJOT00002");
}

/* The Default row's two statuses with no choice of its own on the card: when they apply, true
 * with or without a game running. */
static void default_changes_say_when_they_apply(void)
{
    static const char ini[] = "# Rubi per-game settings\ndefault = enabled\n";
    char both[300];
    card(3, NULL);
    put_file("ux0:data/VitaJPOverlay/games.ini", ini, sizeof(ini) - 1);
    spec_list();
    at = 0;
    hold(0, 10);
    tap(SCE_CTRL_TRIANGLE, 10);                      /* ON -> OFF: no game ON by its own choice */
    int off = at - 1;
    script[at++] = (Input){0, 1, 100, 112 + 20, 0}; /* the Default row: OFF -> ON, none kept OFF */
    hold(0, 10);
    int frames = run(at), last = frames - 1;
    for (int k = 0; k < 2; k++) {
        int f = k ? last : off;
        snprintf(both, sizeof(both), "%s%s%s", session->shown[f].status[0], session->shown[f].status[1][0] ? " " : "",
                 session->shown[f].status[1]);
        const char *want = k ? "Saved: every game is ON from its next start or resume."
                             : "Saved: every game is OFF from its next start or resume. Turn ON the ones you want.";
        TEST_CHECK_(!strcmp(both, want), "%s\n  want: %s", both, want);
    }
}

/* A Default change whose save fails changes nothing: the choices on screen and
 * in games.ini stay, and the status says so (save()'s message). */
static void a_failed_disable_all_changes_nothing(void)
{
    static const char ini[] = "# Rubi per-game settings\ndefault = enabled\nVJOT00001 = enabled\n";
    card(4, NULL);
    put_file("ux0:data/VitaJPOverlay/games.ini", ini, sizeof(ini) - 1);
    mkdir("ux0:data/VitaJPOverlay/games.ini.tmp", 0777); /* the save's temporary file cannot be written */
    spec_list();
    at = 0;
    hold(0, 20);
    tap(SCE_CTRL_TRIANGLE, 10);
    int frames = run(at), last = frames - 1;
    rmdir("ux0:data/VitaJPOverlay/games.ini.tmp");
    TEST_CHECK(!strcmp(session->shown[last].default_badge, "ON"));
    int r = card_of(last, "VJOT00001");
    TEST_CHECK(r >= 0 && !strcmp(session->shown[last].badge[r], "ON"));
    TEST_CHECK_(!strncmp(session->shown[last].status[0], "Save not confirmed.", 19), "%s", session->shown[last].status[0]);
    TEST_CHECK(!strcmp(slurp("ux0:data/VitaJPOverlay/games.ini", NULL), ini));
}

/* Search uses the real keyboard lifecycle and match code, with the SDK result supplied here. */
static void reply_ascii(const char *query)
{
    memset(vjo_test_ime_reply, 0, sizeof(vjo_test_ime_reply));
    for (int i = 0; query[i] && i < 64; i++) vjo_test_ime_reply[i] = (unsigned char)query[i];
    vjo_test_ime_button = SCE_IME_DIALOG_BUTTON_ENTER;
}

static void search_finds_an_off_page_id_and_clear_restores_the_list(void)
{
    card(60, NULL);
    spec_list();
    reply_ascii("vjot00059");
    at = 0;
    hold(0, 15);
    tap(SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER, 12);
    int found = at - 1;
    tap(ENTER, 8);
    script[at++] = (Input){0, 1, 490, 416, 0}; /* Clear, then the first five games again */
    hold(0, 10);
    int frames = run(at), last = frames - 1;
    check_rules();
    TEST_CHECK(session->searches == 1 && session->search_terms == 1);
    TEST_CHECK(!strcmp(session->shown[found].id[0], "VJOT00059") && !session->shown[found].id[1][0]);
    TEST_CHECK(!strcmp(session->shown[found].counter, "1/1") && !strcmp(session->shown[found].pages, "Page 1/1"));
    TEST_CHECK(top_of(last) == 0 && !strcmp(session->shown[last].pages, "Page 1/13"));
    TEST_CHECK(strstr(slurp("ux0:data/VitaJPOverlay/games.ini", NULL), "VJOT00059 = disabled") != NULL);
    int scans = 0;
    for (int f = 0; f < frames; f++) scans += session->shown[f].scans;
    TEST_CHECK(scans == 1); /* querying never rescans or reloads every icon */
}

static void search_matches_japanese_and_empty_results_remain_searchable(void)
{
    card(60, NULL);
    spec_list();
    memset(vjo_test_ime_reply, 0, sizeof(vjo_test_ime_reply));
    vjo_test_ime_reply[0] = 0x30B2; vjo_test_ime_reply[1] = 0x30FC; vjo_test_ime_reply[2] = 0x30E0; /* ゲーム */
    vjo_test_ime_button = SCE_IME_DIALOG_BUTTON_ENTER;
    at = 0; hold(0, 12);
    script[at++] = (Input){0, 1, 240, 416, 0}; /* touch the search field */
    hold(0, 12);
    int frames = run(at);
    TEST_CHECK(session->searches == 1 && !strcmp(session->shown[frames - 1].id[0], "VJOT00059"));

    card(60, NULL);
    reply_ascii("game that does not exist");
    at = 0; hold(0, 12);
    tap(SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER, 12);
    tap(ENTER, 8); /* no result: no setting is changed */
    int empty = at - 1;
    script[at++] = (Input){0, 1, 490, 416, 0}; /* Clear works with zero results */
    hold(0, 10);
    frames = run(at);
    TEST_CHECK(!session->shown[empty].id[0][0] && !strcmp(session->shown[empty].pages, "Page 0/0"));
    TEST_CHECK(!slurp("ux0:data/VitaJPOverlay/games.ini", NULL));
    TEST_CHECK(top_of(frames - 1) == 0 && !strcmp(session->shown[frames - 1].pages, "Page 1/13"));
}

static void search_cancel_and_filtering_preserve_choices(void)
{
    card(60, NULL); spec_list();
    vjo_test_ime_button = SCE_IME_DIALOG_BUTTON_CLOSE;
    at = 0; hold(0, 15);
    tap(SCE_CTRL_RTRIGGER, 10);
    int before = at - 1;
    tap(SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER, 12);
    int frames = run(at);
    TEST_CHECK(session->searches == 1 && top_of(frames - 1) == top_of(before) && selected_of(frames - 1) == selected_of(before));
    TEST_CHECK(!slurp("ux0:data/VitaJPOverlay/games.ini", NULL));

    card(60, NULL); spec_list(); reply_ascii("vjot00059");
    at = 0; hold(0, 15);
    tap(SCE_CTRL_SQUARE, 8); /* Enabled */
    tap(SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER, 12);
    int found = at - 1;
    tap(ENTER, 8); /* OFF: leaves this result set, and clamps selection */
    frames = run(at);
    TEST_CHECK(!strcmp(session->shown[found].id[0], "VJOT00059"));
    TEST_CHECK(!session->shown[frames - 1].id[0][0] && !strcmp(session->shown[frames - 1].pages, "Page 0/0"));
    TEST_CHECK(strstr(slurp("ux0:data/VitaJPOverlay/games.ini", NULL), "VJOT00059 = disabled") != NULL);
}

static void touch_pages_have_five_entries_and_a_partial_last_page(void)
{
    card(7, NULL); spec_list();
    at = 0; hold(0, 12);
    script[at++] = (Input){0, 1, 860, 416, 0};
    hold(0, 10);
    int last_page = at - 1;
    script[at++] = (Input){0, 1, 600, 416, 0};
    hold(0, 10);
    int frames = run(at);
    TEST_CHECK(top_of(last_page) == ROWS && !strcmp(session->shown[last_page].pages, "Page 2/2"));
    TEST_CHECK(session->shown[last_page].id[2][0] && !session->shown[last_page].id[3][0]);
    TEST_CHECK(top_of(frames - 1) == 0 && session->shown[frames - 1].id[4][0]);
    TEST_CHECK(current_page_loads_only(frames));
    check_rules();
}

TEST_LIST = {
    {"search_finds_an_off_page_id_and_clear_restores_the_list", search_finds_an_off_page_id_and_clear_restores_the_list},
    {"search_matches_japanese_and_empty_results_remain_searchable", search_matches_japanese_and_empty_results_remain_searchable},
    {"search_cancel_and_filtering_preserve_choices", search_cancel_and_filtering_preserve_choices},
    {"touch_pages_have_five_entries_and_a_partial_last_page", touch_pages_have_five_entries_and_a_partial_last_page},
    {"idle_window_fills_then_costs_nothing", idle_window_fills_then_costs_nothing},
    {"scrolling_loads_one_icon_a_frame_and_never_reloads_the_window",
     scrolling_loads_one_icon_a_frame_and_never_reloads_the_window},
    {"returning_to_a_page_reloads_only_that_page", returning_to_a_page_reloads_only_that_page},
    {"page_jumps_keep_the_row_and_load_only_that_page", page_jumps_keep_the_row_and_load_only_that_page},
    {"page_jumps_at_the_ends", page_jumps_at_the_ends},
    {"letter_jumps_go_to_the_next_initial", letter_jumps_go_to_the_next_initial},
    {"refresh_loads_only_the_current_page", refresh_loads_only_the_current_page},
    {"held_keys_repeat_and_new_presses_act", held_keys_repeat_and_new_presses_act},
    {"list_keys_only_on_the_games_page", list_keys_only_on_the_games_page},
    {"stick_scrolls_like_vitashell", stick_scrolls_like_vitashell},
    {"stick_scrolls_at_the_same_speed_at_30_hz", stick_scrolls_at_the_same_speed_at_30_hz},
    {"the_start_line_names_the_budget_and_the_heap", the_start_line_names_the_budget_and_the_heap},
#if VJO_DEBUG
    {"the_frame_interval_is_logged_once", the_frame_interval_is_logged_once},
#endif
    {"default_changes_say_when_they_apply", default_changes_say_when_they_apply},
    {"stick_stops_at_the_first_and_last_card", stick_stops_at_the_first_and_last_card},
    {"a_page_change_latches_the_stick", a_page_change_latches_the_stick},
    {"a_touched_tab_latches_the_stick", a_touched_tab_latches_the_stick},
    {"the_dpad_acts_first_in_a_frame", the_dpad_acts_first_in_a_frame},
    {"a_failed_read_moves_nothing", a_failed_read_moves_nothing},
    {"hints_leave_room_for_the_counter", hints_leave_room_for_the_counter},
    {"titles_are_laid_out_only_for_the_current_page", titles_are_laid_out_only_for_the_current_page},
    {"toggles_and_filters_keep_icons", toggles_and_filters_keep_icons},
    {"circle_console_toggles_with_circle", circle_console_toggles_with_circle},
    {"broken_icons_show_the_placeholder_once_tried", broken_icons_show_the_placeholder_once_tried},
    {"rescan_only_on_start", rescan_only_on_start},
    {"many_games_bounded", many_games_bounded},
    {"touch_toggles_the_touched_card", touch_toggles_the_touched_card},
    {"bubbles_are_named_tagged_and_described", bubbles_are_named_tagged_and_described},
    {"enter_toggles_a_bubble_and_writes_no_configuration", enter_toggles_a_bubble_and_writes_no_configuration},
    {"a_longer_boot_bin_is_the_older_layout", a_longer_boot_bin_is_the_older_layout},
    {"a_rescan_keeps_nothing_of_old_rows", a_rescan_keeps_nothing_of_old_rows},
    {"a_long_title_is_cut_to_fit", a_long_title_is_cut_to_fit},
    {"reserved_names_are_listed_as_they_are", reserved_names_are_listed_as_they_are},
    {"stale_descriptions_are_removed", stale_descriptions_are_removed},
    {"a_cartridge_only_title_is_listed", a_cartridge_only_title_is_listed},
    {"titles_with_a_choice_but_in_no_folder_are_listed", titles_with_a_choice_but_in_no_folder_are_listed},
    {"skipped_folders_are_logged_with_why", skipped_folders_are_logged_with_why},
    {"a_listing_that_stops_is_logged_and_prunes_nothing", a_listing_that_stops_is_logged_and_prunes_nothing},
    {"the_rows_line_counts_what_was_left_out_and_safe_mode", the_rows_line_counts_what_was_left_out_and_safe_mode},
    {"disable_all_turns_off_the_games_on_by_their_own_choice", disable_all_turns_off_the_games_on_by_their_own_choice},
    {"a_failed_disable_all_changes_nothing", a_failed_disable_all_changes_nothing},
    {"a_scan_logs_in_one_append", a_scan_logs_in_one_append},
    {"the_log_batch_keeps_order_and_threads_apart", the_log_batch_keeps_order_and_threads_apart},
    {"missing_psp_rows_follow_the_shared_row", missing_psp_rows_follow_the_shared_row},
    {"the_log_cap_keeps_whole_lines_as_before", the_log_cap_keeps_whole_lines_as_before},
    {NULL, NULL}
};

int main(int argc, char **argv)
{
    if (!vjo_test_tmpdir("settings-games")) return 2; /* each test's card in it, all removed at the end */
    return acutest_main(argc, argv);
}

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
    if (!vjo_test_tmpdir("settings-games")) return 2;
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
