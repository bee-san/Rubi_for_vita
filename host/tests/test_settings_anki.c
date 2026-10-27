/* The Rubi app's Anki page itself (settings/anki_page.c; #163 c3), on the host. The page draws
 * through main.c's helpers (VjoAnkiUi), recorded here; its five rows (API 32 added Trim black bars), which
 * scroll a row at a time, are checked for their layout and clipping, Up/Down and the left stick,
 * Left/Right, Enter and touch on each, each settings row's save into a
 * real config.ini (settings/preferences.c, read back by the app and by the Shell's own loader,
 * shell/files.c), and its export through the real settings/anki_export.c on a temporary memory
 * card, on the page's worker thread, which these stubs start when a test says (a join runs a
 * thread not yet run). The harnesses that drive main.c (test_settings_app.c, test_settings_games.c)
 * keep settings_stubs/anki_page_stub.c, so that they never run an export.
 *
 * Text widths here are the settings harness's stub metrics (Latin 10, other characters 18 units
 * per 18-unit em), not the firmware's PVF fonts. Each test runs in its own process (acutest's
 * default): the page keeps its state in statics, as on the console. */
#define main acutest_main
#include "acutest.h"
#undef main
#include "test_tmpdir.h"
#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/cpu.h>
#include <psp2/rtc.h>
#include <vita2d.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../../core/anki_queue.h"
#include "../../settings/anki_export.h"
#include "../../settings/anki_page.h"
#include "../../settings/app_prefs.h"
#include "../../settings/cards.h"
#include "../../settings/cards_page.h"
#include "../../settings/preferences.h"
#include "settings_stubs/search_dialog_stub.h"
#include <psp2/ime_dialog.h>

void vjo_config_load(VjoConfig *cfg, VjoArena *scratch); /* shell/files.c: what the overlay reads */

/* settings/anki_page.c's layout: eight rows (#314 added Duplicates and Known words, #315 Cards) in a column that
 * scrolls a row at a time in the room from y 112 to 412, then the 3-line status box above main.c's controls line. With
 * the first row in view, rows 0-3 are whole and row 4 is cut at the room's bottom; with row 4 selected, rows 1-4 are
 * whole and row 5 cut; with row 5, rows 2-5 and row 6 cut; with row 6, rows 3-6; with row 7, rows 5-7. */
#define ROWS 8
#define DUPES 3  /* Duplicates (#314): three badges, Tag, Skip and Keep, from x 440, 160 apart, 152 wide */
#define CARDS 4  /* Cards: N queued (#315): opens the Cards page */
#define KNOWN 5  /* Known words (#314 stage C): Import */
#define EXPORT 6 /* Export cards */
#define ALL 7    /* Export all cards */
static const int row_offset[ROWS] = {0, 72, 144, 216, 288, 360, 440, 520},
                 row_height[ROWS] = {68, 68, 68, 68, 68, 76, 76, 76};
#define VIEW_Y 112
#define VIEW_END 412
/* Row r's top on screen with row `top` first in view. */
static int row_top_at(int r, int top) { return VIEW_Y + row_offset[r] - row_offset[top]; }
#define FOOTER_Y 416
#define FOOTER_H 92
#define TABS_END 100       /* main.c: TAB_Y 52 + TAB_H 48 */
#define CONTROLS_TOP 511   /* main.c draws the controls line's 24 px text at baseline 535 */
#define CARD_X 24          /* main.c's focus_card: 24..936 */
#define CARD_END 936
#define WHITE 1u
#define DIM 2u
#define ACCENT 3u
#define OFF 4u
#define CARD 5u

/* ---------------- the memory card: real files under a temporary directory ---------------- */

static int native(int n) { return n < 0 ? (int)(0x80010000u | (unsigned)errno) : n; }
static const char *fail_open; /* an open of a path containing this fails (EIO) */
static int fail_rename, renames, writes_opened;
int sceIoOpen(const char *p, int flags, unsigned mode)
{
    if (fail_open && strstr(p, fail_open))
        return (int)0x80010005u;
    if (flags & (O_WRONLY | O_RDWR))
        writes_opened++;
    return native(open(p, flags, mode));
}
int sceIoClose(int fd) { return native(close(fd)); }
int sceIoRead(int fd, void *p, size_t n) { return native((int)read(fd, p, n)); }
int sceIoWrite(int fd, const void *p, size_t n) { return native((int)write(fd, p, n)); }
int sceIoPread(int fd, void *p, size_t n, SceOff off) { return native((int)pread(fd, p, n, off)); }
int sceIoPwrite(int fd, const void *p, size_t n, SceOff off) { return native((int)pwrite(fd, p, n, off)); }
SceOff sceIoLseek(int fd, SceOff off, int whence) { return lseek(fd, off, whence); }
int sceIoMkdir(const char *p, unsigned mode) { return native(mkdir(p, mode)); }
static void date_of(time_t t, SceDateTime *d)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    *d = (SceDateTime){(unsigned short)(tm.tm_year + 1900), (unsigned short)(tm.tm_mon + 1), (unsigned short)tm.tm_mday,
                       (unsigned short)tm.tm_hour, (unsigned short)tm.tm_min, (unsigned short)tm.tm_sec, 0};
}
int sceIoGetstat(const char *p, SceIoStat *st)
{
    struct stat s;
    if (stat(p, &s))
        return native(-1);
    st->st_mode = s.st_mode;
    st->st_size = s.st_size;
    date_of(s.st_ctim.tv_sec, &st->st_ctime);
    date_of(s.st_atim.tv_sec, &st->st_atime);
    date_of(s.st_mtim.tv_sec, &st->st_mtime);
    return 0;
}
int sceIoRename(const char *a, const char *b)
{
    if (fail_rename)
        return (int)0x80010005u;
    renames++;
    return native(rename(a, b));
}
int sceIoRemove(const char *p) { return native(unlink(p)); }
int sceIoSync(const char *p, unsigned flags) { (void)p; (void)flags; return 0; }
int sceIoSyncByFd(int fd, int flag) { (void)flag; return native(fsync(fd)); }
static DIR *dirs[4];
static char dir_paths[4][256];
static const char *fail_dopen; /* a directory that cannot be read */
int sceIoDopen(const char *p)
{
    if (fail_dopen && !strcmp(p, fail_dopen))
        return (int)0x80010005u;
    for (int i = 0; i < 4; i++)
        if (!dirs[i]) {
            if (!(dirs[i] = opendir(p)))
                return native(-1);
            snprintf(dir_paths[i], sizeof(dir_paths[i]), "%s", p);
            return 100 + i;
        }
    return -1;
}
int sceIoDread(int fd, SceIoDirent *ent)
{
    struct dirent *e = readdir(dirs[fd - 100]);
    char path[600];
    if (!e)
        return 0;
    snprintf(ent->d_name, sizeof(ent->d_name), "%s", e->d_name);
    snprintf(path, sizeof(path), "%s/%s", dir_paths[fd - 100], e->d_name);
    TEST_ASSERT(sceIoGetstat(path, &ent->d_stat) == 0);
    return 1;
}
int sceIoDclose(int fd)
{
    int rc = closedir(dirs[fd - 100]);
    dirs[fd - 100] = NULL;
    return rc;
}
/* 2026-10-12 09:30:05 on the console */
int sceRtcGetCurrentClockLocalTime(SceDateTime *t)
{
    *t = (SceDateTime){2026, 10, 12, 9, 30, 5, 0};
    return 0;
}
int sceRtcGetCurrentTick(SceRtcTick *t)
{
    t->tick = 62135596800000000ull + 1791790205ull * 1000000ull;
    return 0;
}

uint64_t sceKernelGetProcessTimeWide(void) { return 0; } /* (the page's log lines time its words and exports) */

/* ---------------- the page's thread, run when the test says ---------------- */

static int (*thread_entry)(SceSize, void *), (*pending)(SceSize, void *);
static int threads_created, threads_deleted, thread_priority, thread_affinity, fail_create;
static unsigned thread_stack;
SceUID sceKernelCreateMutex(const char *n, unsigned int a, int c, void *o) { (void)n; (void)a; (void)c; (void)o; return 0x100; }
int sceKernelLockMutex(SceUID u, int c, void *t) { (void)u; (void)c; (void)t; return 0; }
int sceKernelUnlockMutex(SceUID u, int c) { (void)u; (void)c; return 0; }
SceUID sceKernelCreateThread(const char *n, int (*e)(SceSize, void *), int p, unsigned int s, unsigned int a, int af, void *o)
{
    (void)n; (void)a; (void)o;
    if (fail_create)
        return (int)0x80020001u;
    thread_entry = e;
    thread_priority = p;
    thread_stack = s;
    thread_affinity = af;
    return 0x200 + ++threads_created;
}
int sceKernelStartThread(SceUID u, SceSize s, void *a) { (void)u; (void)s; (void)a; pending = thread_entry; return 0; }
/* The worker runs now: an export from its first step to its last. */
static void run_worker(void)
{
    int (*e)(SceSize, void *) = pending;
    pending = NULL;
    if (e)
        e(0, NULL);
}
int sceKernelWaitThreadEnd(SceUID u, int *s, void *t) { (void)u; (void)s; (void)t; run_worker(); return 0; }
int sceKernelDeleteThread(SceUID u) { (void)u; threads_deleted++; return 0; }
int sceKernelDeleteMutex(SceUID u) { (void)u; return 0; }

/* ---------------- the app's log and its display ---------------- */

static char logs[8192];
static int log_enabled = -1;
void vjo_log_enable(int on) { log_enabled = on; }
void vjo_log(const char *fmt, ...)
{
    va_list ap;
    size_t n = strlen(logs);
    va_start(ap, fmt);
    vsnprintf(logs + n, sizeof(logs) - n - 1, fmt, ap);
    va_end(ap);
    n = strlen(logs);
    if (n + 1 < sizeof(logs)) {
        logs[n] = '\n';
        logs[n + 1] = '\0';
    }
}
static int frames_begun, frames_shown;
/* the clipping the page sets around a row cut at the room's bottom, as each drawing call sees it */
static int clipping, clip_x0, clip_y0, clip_x1, clip_y1;
void vita2d_enable_clipping(void) { clipping = 1; }
void vita2d_disable_clipping(void) { clipping = 0; }
void vita2d_set_clip_rectangle(int x0, int y0, int x1, int y1) { clip_x0 = x0, clip_y0 = y0, clip_x1 = x1, clip_y1 = y1; }
void vita2d_start_drawing(void) { frames_begun++; }
void vita2d_clear_screen(void) {}
void vita2d_end_drawing(void) {}
void vita2d_swap_buffers(void) { frames_shown++; }

/* ---------------- what a frame draws (main.c's helpers, recorded) ---------------- */

/* clip: the clipping on (its rectangle's bottom; 0: none) when it was drawn */
typedef struct { int x, y, width; unsigned color; float px; int lines, clip; char s[480]; } Text; /* width 0: a single line */
typedef struct { int y, height, focus, clip; } Card;
typedef struct { int x, y, width, height, active, clip; char label[32]; } Badge;
typedef struct { int x, y, width, height; unsigned color; } Rect;
static struct {
    Text text[96];
    Card card[8];
    Badge badge[8];
    Rect rect[64];
    int texts, cards, badges, rects, headers, footers;
    int footer_y, footer_h, footer_text_y, footer_lines, footer_error, footer_clip;
    char footer[480], controls[160];
} drawn;
/* The Anki page clips only a row cut at the room's bottom, to the room; the Cards page clips its own rows and
 * card. Clips other than the room's are counted, and a frame the Anki page drew may have none. */
static int other_clips;
static int clip_now(void)
{
    if (!clipping) return 0;
    if (!(clip_x0 == 24 && clip_x1 == 936 && clip_y1 == VIEW_END))
        other_clips++;
    return clip_y1;
}
void vita2d_draw_rectangle(float x, float y, float w, float h, unsigned int color)
{
    if (drawn.rects < 64) drawn.rect[drawn.rects++] = (Rect){(int)x, (int)y, (int)w, (int)h, color};
}

static void d_text(int x, int y, unsigned color, float px, const char *s)
{
    if (drawn.texts < 96)
        drawn.text[drawn.texts++] = (Text){x, y, 0, color, px, 1, clip_now(), ""},
        snprintf(drawn.text[drawn.texts - 1].s, 480, "%s", s);
}
static void d_wrapped(int x, int y, int width, unsigned color, float px, int line_height, int lines, const char *s)
{
    (void)line_height;
    if (drawn.texts < 96)
        drawn.text[drawn.texts++] = (Text){x, y, width, color, px, lines, clip_now(), ""},
        snprintf(drawn.text[drawn.texts - 1].s, 480, "%s", s);
}
static void d_card(int y, int height, int focus)
{
    if (drawn.cards < 8)
        drawn.card[drawn.cards++] = (Card){y, height, focus, clip_now()};
}
static void d_badge(int x, int y, int width, int height, const char *label, int active)
{
    if (drawn.badges < 8) {
        drawn.badge[drawn.badges] = (Badge){x, y, width, height, active, clip_now(), ""};
        snprintf(drawn.badge[drawn.badges++].label, 32, "%s", label);
    }
}
static void d_footer(int y, int height, int text_y, int lines, const char *message, int error, const char *controls)
{
    drawn.footers++;
    drawn.footer_y = y, drawn.footer_h = height, drawn.footer_text_y = text_y, drawn.footer_lines = lines;
    drawn.footer_error = error;
    drawn.footer_clip = clipping;
    snprintf(drawn.footer, sizeof(drawn.footer), "%s", message);
    snprintf(drawn.controls, sizeof(drawn.controls), "%s", controls);
}
static void d_header(void) { drawn.headers++; }

/* A string's width in the stub metrics: Latin 10/18 of the size, other characters 18/18. */
static int width_of(const char *s, float px)
{
    float w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p < 0x80)
            w += px * 10 / 18;
        else if ((*p & 0xC0) == 0xC0)
            w += px;
    return (int)(w + 0.5f);
}

static int d_text_width(float px, const char *s) { return width_of(s, px); }

static VjoButtons buttons;
static const VjoAnkiUi ui = {d_text, d_wrapped, d_card, d_badge, d_footer, d_header, WHITE, DIM, ACCENT, OFF, CARD,
                             &buttons, d_text_width};

/* ---------------- the Cards page's picture: textures, recorded ---------------- */

static int textures_made, textures_live, texture_draws;
static vita2d_texture *last_texture;
static float texture_x, texture_y, texture_sx, texture_sy;
vita2d_texture *vita2d_create_empty_texture_format(unsigned int w, unsigned int h, unsigned int format)
{
    vita2d_texture *t = calloc(1, sizeof(*t));
    TEST_ASSERT(t != NULL && format == SCE_GXM_TEXTURE_FORMAT_U8U8U8_BGR && w <= 512 && h <= 288);
    t->width = w;
    t->height = h;
    t->pixels = calloc((size_t)((w + 7) & ~7u) * 3u * h, 1);
    t->live = 1;
    /* as the app gets it on the Vita: through the link's wrapper (settings/vita2d_textures.c, its own suite
     * settings_textures), which clears libvita2d's two stale fields: gxm_rtgt and depth_UID stay 0 (calloc) */
    textures_made++;
    textures_live++;
    return t;
}
void *vita2d_texture_get_datap(const vita2d_texture *t) { return t->pixels; }
unsigned int vita2d_texture_get_stride(const vita2d_texture *t) { return ((t->width + 7) & ~7u) * 3u; }
unsigned int vita2d_texture_get_width(const vita2d_texture *t) { return t->width; }
unsigned int vita2d_texture_get_height(const vita2d_texture *t) { return t->height; }
void vita2d_draw_texture_scale(const vita2d_texture *t, float x, float y, float sx, float sy)
{
    TEST_CHECK(t->live);
    ((vita2d_texture *)t)->in_flight = 1;
    last_texture = (vita2d_texture *)t;
    texture_draws++;
    texture_x = x, texture_y = y, texture_sx = sx, texture_sy = sy;
}
void vita2d_wait_rendering_done(void)
{
    if (last_texture)
        last_texture->in_flight = 0;
}
void vita2d_free_texture(vita2d_texture *t)
{
    TEST_CHECK_(t->gxm_rtgt == NULL && t->depth_UID == 0, "a texture freed with stale fields");
    TEST_CHECK_(!t->in_flight, "a texture freed while the GPU may draw it");
    t->live = 0;
    textures_live--;
    if (last_texture == t)
        last_texture = NULL;
    free(t->pixels);
    free(t);
}
int sceDisplayWaitVblankStart(void) { return 0; }
static unsigned char scratch_mem[64 * 1024];
static VjoArena scratch;

static int dialog_ran; /* the last frame's return: the keyboard ran in it */
static void touch(uint32_t pressed, int x, int y)
{
    memset(&drawn, 0, sizeof(drawn));
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    other_clips = 0;
    dialog_ran = vjo_anki_page_frame(&ui, pressed, x, y, &scratch);
    /* (a frame is one page's: the Cards page's while it is open after it, else the Anki page's) */
    TEST_CHECK_(vjo_cards_page_is_open() || !other_clips, "the Anki page clipped %d times outside its room", other_clips);
}
static void frame(uint32_t pressed) { touch(pressed, -1, -1); }
static void show(void)
{
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    vjo_anki_page_show(&scratch);
}
/* config.ini as another save of the app left it. */
static int save_now(int mining, int screenshot)
{
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    return vjo_preferences_save_anki(mining, screenshot, 1, &scratch);
}

/* The first row the frame drew: the one whose title is at the room's top (y 112 + 30). */
static int first_row(void)
{
    static const char *const titles[ROWS] = {"Card mining", "Screenshots", "Trim black bars", "Duplicates",
                                             "Cards", "Known words", "Export cards (", "Export all cards"};
    for (int i = 0; i < drawn.texts; i++)
        if (drawn.text[i].x == 42 && drawn.text[i].y == VIEW_Y + 30)
            for (int r = 0; r < ROWS; r++)
                if (!strncmp(drawn.text[i].s, titles[r], strlen(titles[r])))
                    return r;
    return -1;
}
/* The rows a frame draws with row `top` first: those that start inside the room. */
static int rows_drawn(int top)
{
    int n = 0;
    for (int r = top; r < ROWS && row_top_at(r, top) < VIEW_END; r++)
        n++;
    return n;
}
static int badges_of(int r) { return r == DUPES ? 3 : 1; }
/* The row drawn focused (-1: none, -2: more than one). */
static int focused(void)
{
    int f = -1;
    for (int i = 0; i < drawn.cards; i++)
        if (drawn.card[i].focus)
            f = f == -1 ? first_row() + i : -2;
    return f;
}
/* Row r's badge k (each row drawn has badges_of(r), in row order); an empty one when it was not drawn. */
static const Badge *badge_k(int r, int k)
{
    static const Badge none = {0, 0, 0, 0, 0, 0, "(none)"};
    int i = k, top = first_row();
    if (top < 0 || r < top || k >= badges_of(r))
        return &none;
    for (int q = top; q < r; q++)
        i += badges_of(q);
    return i < drawn.badges ? &drawn.badge[i] : &none;
}
static const Badge *badge(int r) { return badge_k(r, 0); }
static const Text *text_starting(const char *prefix)
{
    for (int i = 0; i < drawn.texts; i++)
        if (!strncmp(drawn.text[i].s, prefix, strlen(prefix)))
            return &drawn.text[i];
    return NULL;
}

/* ---------------- the card's files ---------------- */

static int there(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}
static int read_file(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f)
        return -1;
    n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = '\0';
    return (int)n;
}
static int count_lines(const char *text, const char *prefix)
{
    int n = 0;
    for (const char *p = text; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
        n += !strncmp(p, prefix, strlen(prefix));
    return n;
}
static int files_in(const char *dir, const char *suffix)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;
    if (!d)
        return 0;
    while ((e = readdir(d)))
        if (strlen(e->d_name) > strlen(suffix) && !strcmp(e->d_name + strlen(e->d_name) - strlen(suffix), suffix))
            n++;
    closedir(d);
    return n;
}
static void reset(int assign)
{
    /* an empty ux0:data/VitaJPOverlay in the suite's directory, removed and made in-process (no shell) */
    char data[PATH_MAX], overlay[PATH_MAX];
    TEST_ASSERT(snprintf(data, sizeof(data), "%s/ux0:data", vjo_test_tmpdir_path) < (int)sizeof(data));
    TEST_ASSERT(snprintf(overlay, sizeof(overlay), "%s/VitaJPOverlay", data) < (int)sizeof(overlay));
    TEST_ASSERT(vjo_test_remove_tree(data) == 0);
    TEST_ASSERT(mkdir(data, 0777) == 0 && mkdir(overlay, 0777) == 0);
    fail_open = fail_dopen = NULL;
    fail_rename = fail_create = renames = writes_opened = 0;
    logs[0] = '\0';
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    buttons = vjo_buttons(assign);
}

/* A card in the queue as the overlay's Enter saves it (version 4), with its screenshot or not, from line `sentence`
 * (NULL: the word alone) a minute after the first (`minute`); its ID. */
static const char *queue_line(const char *spelling, const char *sentence, int minute, int picture);
static const char *queue_card(const char *spelling, int picture) { return queue_line(spelling, NULL, 0, picture); }
static const char *queue_line(const char *spelling, const char *sentence, int minute, int picture)
{
    static char id[VJO_QUEUE_ID_SIZE];
    static unsigned char mem[64 * 1024];
    const char *m[1] = {"a meaning"}, *pos[1] = {"n"};
    char path[256];
    VjoArena a;
    VjoAnkiNote n;
    char *json;
    FILE *f;
    vjo_arena_init(&a, mem, sizeof(mem));
    memset(&n, 0, sizeof(n));
    n.spelling = spelling;
    n.reading = "よみ";
    n.rank = VJO_NO_RANK;
    n.meanings = m;
    n.n_meanings = 1;
    n.sentence = sentence ? sentence : spelling;
    n.hl_start = 0;
    n.hl_end = (int)strlen(spelling);
    n.sf_hl_start = n.sf_hl_end = -1;
    n.pos = pos;
    n.n_pos = 1;
    n.misc = "SOME GAME (2026-10-12 09:30)";
    n.dictionary = "JMdict";
    n.game = "SOME GAME";
    n.title_id = "PCSG00000";
    n.platform = "vita";
    n.unix_time = 1791790200 + 60 * minute;
    n.ocr_engine = "vocr";
    n.ocr_model = "VOCR3_H15_w8.vocr";
    TEST_ASSERT((json = vjo_queue_encode(&a, &n, picture)) != NULL);
    vjo_queue_id(json, strlen(json), id);
    mkdir(VJO_QUEUE_DIR, 0777);
    snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s.json", id);
    TEST_ASSERT((f = fopen(path, "wb")) != NULL);
    TEST_ASSERT(fwrite(json, 1, strlen(json), f) == strlen(json));
    fclose(f);
    if (picture) {
        static const unsigned char jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 1, 2, 3, 4, 5, 6, 7, 8, 0xFF, 0xD9};
        snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s.jpg", id);
        TEST_ASSERT((f = fopen(path, "wb")) != NULL);
        TEST_ASSERT(fwrite(jpeg, 1, sizeof(jpeg), f) == sizeof(jpeg));
        fclose(f);
    }
    return id;
}

/* config.ini's three Anki lines as written, and as the app and the Shell load them back. */
static void check_saved3(int mining, int screenshot, int trim)
{
    char text[VJO_CONFIG_TEXT_MAX + 1], want[64];
    static unsigned char shell_mem[24 * 1024];
    VjoArena shell;
    VjoConfig cfg;
    TEST_ASSERT(read_file(VJO_PREFERENCES_PATH, text, sizeof(text)) > 0);
    snprintf(want, sizeof(want), "anki_mining = %s\n", mining ? "on" : "off");
    TEST_CHECK_(count_lines(text, "anki_mining") == 1 && strstr(text, want), "config.ini: want %s", want);
    snprintf(want, sizeof(want), "anki_screenshot = %s\n", screenshot ? "on" : "off");
    TEST_CHECK_(count_lines(text, "anki_screenshot =") == 1 && strstr(text, want), "config.ini: want %s", want);
    snprintf(want, sizeof(want), "anki_screenshot_trim_bars = %s\n", trim ? "on" : "off");
    TEST_CHECK_(count_lines(text, "anki_screenshot_trim_bars") == 1 && strstr(text, want), "config.ini: want %s", want);
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    TEST_ASSERT(vjo_preferences_load(&cfg, &scratch) == 0);
    TEST_CHECK(cfg.anki_mining == mining && cfg.anki_screenshot == screenshot && cfg.anki_screenshot_trim_bars == trim);
    vjo_arena_init(&shell, shell_mem, sizeof(shell_mem));
    vjo_config_load(&cfg, &shell);
    TEST_CHECK_(cfg.anki_mining == mining && cfg.anki_screenshot == screenshot && cfg.anki_screenshot_trim_bars == trim &&
                    !cfg.n_warnings,
                "the Shell loads anki_mining %d, anki_screenshot %d, anki_screenshot_trim_bars %d (%d warnings)",
                cfg.anki_mining, cfg.anki_screenshot, cfg.anki_screenshot_trim_bars, cfg.n_warnings);
}
static void check_saved(int mining, int screenshot) { check_saved3(mining, screenshot, 1); }
/* The settings rows' badges show the saved states, and the page's note says it was saved. */
static void check_shown3(int mining, int screenshot, int trim)
{
    frame(0);
    TEST_CHECK_(!strcmp(badge(0)->label, mining ? "ON" : "OFF") && badge(0)->active == mining,
                "Card mining shows %s", badge(0)->label);
    TEST_CHECK_(!strcmp(badge(1)->label, screenshot ? "ON" : "OFF") && badge(1)->active == screenshot,
                "Screenshots shows %s", badge(1)->label);
    TEST_CHECK_(!strcmp(badge(2)->label, trim ? "ON" : "OFF") && badge(2)->active == trim,
                "Trim black bars shows %s", badge(2)->label);
    TEST_CHECK_(!strcmp(drawn.footer, "Saved. The overlay uses it when it next opens.") && !drawn.footer_error,
                "the note: %s", drawn.footer);
}
static void check_shown(int mining, int screenshot) { check_shown3(mining, screenshot, 1); }

/* ---------------- tests ---------------- */

/* The rows drawn with row `top` first in view: each where the column puts it, none over another, its
 * badges and texts inside it; a row cut at the room's bottom drawn clipped there, its card, texts and
 * badges; the others and the footer not clipped; a bar right of the rows where the view is in the
 * column (524 px of rows in a 300 px room). */
static void check_layout(int top)
{
    int badges = 0;
    for (int r = top; r < top + rows_drawn(top); r++)
        badges += badges_of(r);
    TEST_ASSERT(drawn.cards == rows_drawn(top) && drawn.badges == badges && drawn.headers == 1 && drawn.footers == 1);
    TEST_ASSERT(first_row() == top);
    for (int i = 0; i < drawn.cards; i++) {
        int r = top + i, y = row_top_at(r, top), whole = y + row_height[r] <= VIEW_END;
        const Card *c = &drawn.card[i];
        TEST_CHECK_(c->y == y && c->height == row_height[r], "row %d at %d+%d", r, c->y, c->height);
        TEST_CHECK_(i ? c->y >= drawn.card[i - 1].y + drawn.card[i - 1].height : c->y >= TABS_END,
                    "row %d at %d overlaps what is above it", r, c->y);
        TEST_CHECK_(c->y < VIEW_END && c->clip == (whole ? 0 : VIEW_END), "row %d (first %d): whole %d, clip %d", r,
                    top, whole, c->clip);
        /* its badges inside it, right of its title, none over another */
        for (int k = 0; k < badges_of(r); k++) {
            const Badge *b = badge_k(r, k);
            TEST_CHECK_(b->y >= c->y && b->y + b->height <= c->y + c->height && b->x >= CARD_X + 4 &&
                            b->x + b->width <= CARD_END - 4 && b->clip == c->clip, "row %d's badge %d outside its card",
                        r, k);
            TEST_CHECK_(!k || b->x >= badge_k(r, k - 1)->x + badge_k(r, k - 1)->width, "row %d's badges overlap", r);
        }
    }
    /* every text inside exactly one row, from its cap height to its descent, clipped as its row is */
    for (int i = 0; i < drawn.texts; i++) {
        const Text *t = &drawn.text[i];
        int t_top = t->y - (int)(t->px * 0.8f), bottom = t->y + (int)(t->px * 0.25f), in = 0, clip = -1;
        for (int k = 0; k < drawn.cards; k++)
            if (t_top >= drawn.card[k].y && bottom <= drawn.card[k].y + drawn.card[k].height)
                in++, clip = drawn.card[k].clip;
        TEST_CHECK_(in == 1 && t->clip == clip && t->x >= CARD_X + 4 && (!t->width || t->x + t->width <= CARD_END - 4),
                    "'%s' at %d,%d is not inside one row (clip %d)", t->s, t->x, t->y, t->clip);
        TEST_CHECK_(bottom <= VIEW_END || t->clip == VIEW_END, "'%s' at %d,%d below the room, not clipped", t->s, t->x, t->y);
        /* nothing runs into a badge or a text on its line: a title or count ends before it (stub metrics) */
        for (int k = 0; k < drawn.badges; k++) {
            const Badge *b = &drawn.badge[k];
            if (!t->width && t_top < b->y + b->height && bottom > b->y && t->x < b->x)
                TEST_CHECK_(t->x + width_of(t->s, t->px) <= b->x, "'%s' runs into the badge '%s'", t->s, b->label);
        }
        for (int k = 0; k < drawn.texts; k++) {
            const Text *u = &drawn.text[k];
            int u_top = u->y - (int)(u->px * 0.8f), u_bottom = u->y + (int)(u->px * 0.25f);
            if (k != i && !t->width && t_top < u_bottom && bottom > u_top && t->x < u->x)
                TEST_CHECK_(t->x + width_of(t->s, t->px) <= u->x, "'%s' runs into '%s'", t->s, u->s);
        }
    }
    /* the bar: its track the room's height, its thumb where the view is */
    int all_rows = row_offset[ROWS - 1] + row_height[ROWS - 1], room = VIEW_END - VIEW_Y, from = row_offset[top];
    int shown = all_rows - from < room ? all_rows - from : room;
    TEST_ASSERT(drawn.rects == 2);
    TEST_CHECK(drawn.rect[0].x == 942 && drawn.rect[0].y == VIEW_Y && drawn.rect[0].width == 6 &&
               drawn.rect[0].height == room && drawn.rect[0].color == CARD);
    TEST_CHECK_(drawn.rect[1].x == 942 && drawn.rect[1].y == VIEW_Y + room * from / all_rows && drawn.rect[1].width == 6 &&
                    drawn.rect[1].height == room * shown / all_rows && drawn.rect[1].color == DIM,
                "thumb at %d+%d", drawn.rect[1].y, drawn.rect[1].height);
    TEST_CHECK(drawn.rect[1].y >= VIEW_Y && drawn.rect[1].y + drawn.rect[1].height <= VIEW_END);
    /* the footer: below the room, three 28 px lines inside it, above the controls line; not clipped */
    TEST_CHECK(drawn.footer_y == FOOTER_Y && drawn.footer_h == FOOTER_H && drawn.footer_lines == 3 && !drawn.footer_clip);
    TEST_CHECK(drawn.footer_y >= VIEW_END && drawn.footer_y + drawn.footer_h <= CONTROLS_TOP);
    TEST_CHECK(drawn.footer_text_y - 24 >= drawn.footer_y + 2 &&
               drawn.footer_text_y + (drawn.footer_lines - 1) * 28 + 6 <= drawn.footer_y + drawn.footer_h);
}

/* Seven rows below the tabs (API 32 added Trim black bars, #314 Duplicates and Known words), more than their room:
 * with the first row selected rows 0-3 are whole and row 4 is cut at the room's bottom; with the last, rows 4-6
 * (check_layout);
 * the footer below them and above main.c's controls line; one row focused; the hints name the console's
 * buttons. */
static void the_rows_and_the_footer_do_not_overlap(void)
{
    reset(VJO_BUTTON_ASSIGN_CROSS);
    queue_card("猫", 1);
    show();
    TEST_CHECK(log_enabled == 1); /* config.ini's log_file (on by default), set when the page is shown */
    frame(0);
    TEST_CHECK(frames_begun == 1 && frames_shown == 1);
    check_layout(0);
    TEST_CHECK(focused() == 0 && !clipping);
    TEST_CHECK(!strcmp(drawn.controls, "Up/Down: choose   ×: select   Select: page   ○: exit"));
    TEST_CHECK(strstr(drawn.footer, "ux0:data/VitaJPOverlay/anki/") && !drawn.footer_error);
    /* the rows' texts and states: mining off, screenshots on and their bars trimmed by default, one card queued */
    TEST_CHECK(text_starting("Card mining") && text_starting("Screenshots") && text_starting("Trim black bars") &&
               text_starting("Duplicates"));
    TEST_CHECK(text_starting("× in the overlay saves the word") && text_starting("Cuts black borders from the picture") &&
               text_starting("A word you mined before gets Anki's duplicate tag."));
    TEST_CHECK(!strcmp(badge(0)->label, "OFF") && !strcmp(badge(1)->label, "ON") && !strcmp(badge(2)->label, "ON") &&
               badge(2)->active && !strcmp(badge(CARDS)->label, "Open"));
    /* #314: Duplicates, its three choices, Tag (the default) lit */
    TEST_CHECK(!strcmp(badge_k(DUPES, 0)->label, "Tag") && badge_k(DUPES, 0)->active &&
               !strcmp(badge_k(DUPES, 1)->label, "Skip") && !badge_k(DUPES, 1)->active &&
               !strcmp(badge_k(DUPES, 2)->label, "Keep") && !badge_k(DUPES, 2)->active);
    for (int k = 0; k < 3; k++)
        TEST_CHECK(badge_k(DUPES, k)->x == 440 + 160 * k && badge_k(DUPES, k)->width == 152);
    TEST_CHECK(42 + width_of("Duplicates", 28) <= 440);
    /* the widest counts still fit before their badges (both at x 760; "already mined" up to 4 digits) */
    for (int k = 0; k < 3; k++) {
        const char *s = k == 2 ? "20000 cards in all" : k ? "Export cards (9999 queued, 9999 already mined)"
                                                          : "Export cards (20000 queued)";
        int x = k == 2 ? 500 : 42, px = k == 2 ? 24 : 28;
        TEST_CHECK_(x + width_of(s, (float)px) <= 760, "'%s' runs into its badge", s);
    }
    /* the last row selected: the view five rows down, every row it shows whole; back at the top as it was */
    frame(SCE_CTRL_UP);
    TEST_CHECK(focused() == ALL);
    check_layout(5);
    TEST_CHECK(text_starting("Export all cards") && text_starting("1 card in all") &&
               !strcmp(badge(ALL)->label, "Export all") && !strcmp(badge(KNOWN)->label, "Import") &&
               text_starting("Imports known_words.txt (tools/known_words.py makes it).") &&
               text_starting("Export cards (1 queued)") && !strcmp(badge(EXPORT)->label, "Export"));
    frame(SCE_CTRL_UP); /* rows 6 and 5: the same view */
    TEST_CHECK(focused() == EXPORT);
    check_layout(5);
    frame(SCE_CTRL_UP);
    TEST_CHECK(focused() == KNOWN);
    check_layout(5);
    frame(SCE_CTRL_UP); /* #315's row 4: one row up, row 7 cut */
    TEST_CHECK(focused() == CARDS);
    check_layout(4);
    TEST_CHECK(text_starting("Cards: 1 queued") && !strcmp(badge(CARDS)->label, "Open") && !badge(CARDS)->active &&
               text_starting("See each card; delete it, fix its sentence or drop its picture."));
    TEST_CHECK(42 + width_of("Cards: 20000 queued", 28) <= badge(CARDS)->x);
    for (int i = 0; i < 4; i++)
        frame(SCE_CTRL_DOWN);
    TEST_CHECK(focused() == 0);
    check_layout(0);
    /* from above: row 4 brings the view one row down (row 5 cut), row 5 two (row 6 cut), row 6 three (rows 3-6) */
    for (int i = 0; i < 4; i++)
        frame(SCE_CTRL_DOWN);
    TEST_CHECK(focused() == CARDS);
    check_layout(1);
    frame(SCE_CTRL_DOWN);
    TEST_CHECK(focused() == KNOWN);
    check_layout(2);
    frame(SCE_CTRL_DOWN);
    TEST_CHECK(focused() == EXPORT);
    check_layout(3);
    for (int i = 0; i < 2; i++)
        frame(SCE_CTRL_DOWN);
    TEST_CHECK(focused() == 0);
    /* the Japanese button assignment: ○ enters */
    reset(VJO_BUTTON_ASSIGN_CIRCLE);
    show();
    frame(0);
    TEST_CHECK(!strcmp(drawn.controls, "Up/Down: choose   ○: select   Select: page   ×: exit"));
    TEST_CHECK(text_starting("○ in the overlay saves the word"));
}

/* Up/Down go round the eight rows; the left stick moves one row a step and stops at the first and
 * the last; in a frame with both, the d-pad's press wins. The view follows the selection by the fewest
 * rows (row 4 brings rows 1-4 in, row 5 rows 2-5, row 6 rows 3-6, row 7 rows 5-7, row 4 from below rows 4-7, row 0
 * rows 0-3). Moving saves nothing and opens nothing. */
static void up_down_and_the_stick_choose_a_row(void)
{
    static const struct { uint32_t pressed; int stick, row, top; } steps[] = {
        {SCE_CTRL_DOWN, 0, 1, 0}, {SCE_CTRL_DOWN, 0, 2, 0}, {SCE_CTRL_DOWN, 0, 3, 0}, {SCE_CTRL_DOWN, 0, 4, 1},
        {SCE_CTRL_DOWN, 0, 5, 2}, {SCE_CTRL_DOWN, 0, 6, 3}, {SCE_CTRL_DOWN, 0, 7, 5}, {SCE_CTRL_DOWN, 0, 0, 0},
        {SCE_CTRL_UP, 0, 7, 5},   {SCE_CTRL_UP, 0, 6, 5},   {0, 1, 7, 5},             {0, 1, 7, 5},
        {0, -1, 6, 5},            {SCE_CTRL_UP, 0, 5, 5},   {0, -1, 4, 4},            {0, -1, 3, 3},
        {0, -1, 2, 2},            {0, -5, 0, 0},            {0, -1, 0, 0},            {SCE_CTRL_DOWN, 1, 1, 0},
        {SCE_CTRL_UP, 2, 0, 0},   {0, 2, 2, 0},             {SCE_CTRL_UP, -1, 1, 0},  {0, 0, 1, 0},
        {0, 9, 7, 5},             {0, -2, 5, 5},
    };
    reset(VJO_BUTTON_ASSIGN_CROSS);
    show();
    frame(0);
    TEST_CHECK(focused() == 0);
    for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        vjo_anki_page_stick(steps[i].stick);
        frame(steps[i].pressed);
        TEST_CHECK_(focused() == steps[i].row && first_row() == steps[i].top, "step %u: row %d focused (first %d), want %d (%d)",
                    i, focused(), first_row(), steps[i].row, steps[i].top);
    }
    TEST_CHECK(!there(VJO_PREFERENCES_PATH) && writes_opened == 0 && threads_created == 0);
}

/* Enter on a settings row turns it the other way; Left/Right turn it OFF/ON, and do nothing when
 * it is that already; on the export rows they do nothing. Each save writes that row's key, and the
 * other rows' as they were, where the app and the Shell read them. */
static void enter_and_left_right_save_each_row_s_key(void)
{
    reset(VJO_BUTTON_ASSIGN_CROSS);
    show(); /* no config.ini yet: mining off, screenshots on (the defaults) */
    frame(0);
    TEST_CHECK(!strcmp(badge(0)->label, "OFF") && !strcmp(badge(1)->label, "ON"));
    frame(SCE_CTRL_CROSS); /* row 0 */
    check_saved(1, 1);
    check_shown(1, 1);
    frame(SCE_CTRL_CROSS);
    check_saved(0, 1);
    check_shown(0, 1);
    frame(SCE_CTRL_RIGHT);
    check_saved(1, 1);
    renames = 0;
    frame(SCE_CTRL_RIGHT); /* ON already: nothing written */
    TEST_CHECK(renames == 0);
    frame(SCE_CTRL_LEFT);
    check_saved(0, 1);
    renames = 0;
    frame(SCE_CTRL_LEFT);
    TEST_CHECK(renames == 0);
    frame(SCE_CTRL_DOWN); /* row 1 */
    frame(SCE_CTRL_CROSS);
    check_saved(0, 0);
    check_shown(0, 0);
    frame(SCE_CTRL_RIGHT);
    check_saved(0, 1);
    renames = 0;
    frame(SCE_CTRL_RIGHT);
    TEST_CHECK(renames == 0);
    frame(SCE_CTRL_LEFT);
    check_saved(0, 0);
    frame(SCE_CTRL_UP); /* row 0 again: its own key only */
    frame(SCE_CTRL_RIGHT);
    check_saved(1, 0);
    check_shown(1, 0);
    /* API 32: row 2, the screenshots' black bars: trimmed by default, kept, trimmed again */
    frame(SCE_CTRL_DOWN);
    frame(SCE_CTRL_DOWN);
    TEST_CHECK(focused() == 2);
    frame(SCE_CTRL_CROSS);
    check_saved3(1, 0, 0);
    check_shown3(1, 0, 0);
    frame(SCE_CTRL_RIGHT);
    check_saved3(1, 0, 1);
    check_shown3(1, 0, 1);
    renames = 0;
    frame(SCE_CTRL_RIGHT);
    TEST_CHECK(renames == 0);
    frame(SCE_CTRL_LEFT);
    check_saved3(1, 0, 0);
    renames = 0;
    frame(SCE_CTRL_LEFT);
    TEST_CHECK(renames == 0);
    frame(SCE_CTRL_UP); /* row 1 keeps row 2's choice */
    frame(SCE_CTRL_RIGHT);
    check_saved3(1, 1, 0);
    frame(SCE_CTRL_LEFT);
    check_saved3(1, 0, 0);
    frame(SCE_CTRL_DOWN);
    frame(SCE_CTRL_CROSS);
    check_saved3(1, 0, 1);
    /* the action rows (past Duplicates, #314; the Cards row, #315): Left/Right open, start and save nothing */
    frame(SCE_CTRL_DOWN);
    TEST_CHECK(focused() == DUPES);
    for (int r = CARDS; r <= ALL; r++) {
        frame(SCE_CTRL_DOWN);
        TEST_CHECK(focused() == r);
        renames = 0;
        frame(SCE_CTRL_LEFT);
        frame(SCE_CTRL_RIGHT);
        TEST_CHECK(renames == 0 && threads_created == 0 && !vjo_anki_page_busy() && !vjo_cards_page_is_open());
    }
    check_saved(1, 0);
    /* with ○ as enter (a Japanese console), × is no Enter */
    reset(VJO_BUTTON_ASSIGN_CIRCLE);
    show();
    frame(SCE_CTRL_DOWN); /* from row 7 round to row 0 */
    TEST_CHECK(focused() == 0);
    frame(SCE_CTRL_CROSS);
    TEST_CHECK(!there(VJO_PREFERENCES_PATH));
    frame(SCE_CTRL_CIRCLE);
    check_saved(1, 1);
}

/* A touch inside a row in view chooses it and acts as Enter does, from its top edge to the line above
 * its bottom; on the row cut at the room's bottom it only chooses it (the view then brings it in);
 * between the rows, outside the cards' width, above them, below the room and on the footer, nothing. */
static void touch_chooses_and_acts_on_each_row(void)
{
    reset(VJO_BUTTON_ASSIGN_CROSS);
    queue_card("猫", 1);
    show();
    frame(0);
    /* the settings rows: each touch turns the row the other way */
    touch(0, 480, row_top_at(0, 0));
    TEST_CHECK(focused() == 0);
    check_saved(1, 1);
    touch(0, 935, row_top_at(0, 0) + row_height[0] - 1);
    check_saved(0, 1);
    touch(0, 24, row_top_at(1, 0));
    TEST_CHECK(focused() == 1);
    check_saved(0, 0);
    touch(0, 480, row_top_at(1, 0) + row_height[1] - 1);
    check_saved(0, 1);
    touch(0, 480, row_top_at(2, 0)); /* API 32: the black bars */
    TEST_CHECK(focused() == 2);
    check_saved3(0, 1, 0);
    touch(0, 935, row_top_at(2, 0) + row_height[2] - 1);
    check_saved3(0, 1, 1);
    /* nothing: the gaps, the sides, above, below the room, the footer */
    static const int miss[][2] = {{480, 180}, {480, 183}, {480, 252}, {480, 255}, {480, 324}, {480, 327},
                                  {480, 396}, {480, 399}, {480, 412}, {480, 450}, {480, 111}, {480, 60},
                                  {23, 140}, {936, 140}, {23, 300}, {936, 360}};
    renames = 0;
    for (unsigned i = 0; i < sizeof(miss) / sizeof(miss[0]); i++) {
        touch(0, miss[i][0], miss[i][1]);
        TEST_CHECK_(focused() == 2 && first_row() == 0 && renames == 0 && threads_created == 0, "a touch at %d,%d acted",
                    miss[i][0], miss[i][1]);
    }
    /* row 4 (Cards, #315), cut at the room's bottom (its 12 px from y 400): chosen, nothing opened; the view brings it
     * in; then row 5 (Known words), cut below it (12 px from y 400), and row 6 (Export cards, 4 px from y 408), the
     * same */
    touch(0, 480, row_top_at(CARDS, 0) + 1);
    TEST_CHECK(focused() == CARDS && first_row() == 1 && threads_created == 0 && !vjo_cards_page_is_open());
    touch(0, 480, row_top_at(KNOWN, 1) + 1);
    TEST_CHECK(focused() == KNOWN && first_row() == 2 && threads_created == 0 && !vjo_anki_page_busy());
    touch(0, 480, row_top_at(EXPORT, 2) + 1);
    TEST_CHECK(focused() == EXPORT && first_row() == 3 && threads_created == 0 && !vjo_anki_page_busy());
    /* the export rows, row 6 with rows 3-6 in view, then (Down to row 7) rows 5-7: a touch at each one's top and
     * bottom edges starts that export */
    for (int k = 0; k < 4; k++) {
        static const char *const result[4] = {"to 2026-10-12_093005-1card.apkg.", "No cards queued",
                                              "to 2026-10-12_093005-all-1card.apkg.",
                                              "to 2026-10-12_093005-all-1card-2.apkg."};
        int r = k < 2 ? EXPORT : ALL, top = k < 2 ? 3 : 5;
        int y = k % 2 ? row_top_at(r, top) + row_height[r] - 1 : row_top_at(r, top);
        if (k == 2) { /* row 7, below the view: Down brings it in */
            frame(SCE_CTRL_DOWN);
            TEST_CHECK(focused() == ALL && first_row() == 5 && threads_created == 2 && !vjo_anki_page_busy());
        }
        touch(0, k % 2 ? 935 : 480, y);
        TEST_CHECK_(focused() == r && vjo_anki_page_busy() && threads_created == k + 1,
                    "a touch at y %d did not start row %d's export", y, r);
        run_worker();
        frame(0);
        TEST_CHECK_(!vjo_anki_page_busy() && strstr(drawn.footer, result[k]), "row %d at y %d: %s", r, y, drawn.footer);
    }
    TEST_CHECK(threads_deleted == 4);
}

/* Enter on an export row runs the export on the page's worker thread (64 KiB, the system core):
 * meanwhile the page says so, its rows act on nothing, and Cancel stops it; once it ends the page
 * shows the result and counts the cards again. */
static void an_export_runs_on_the_worker_and_the_page_follows_it(void)
{
    char before[VJO_CONFIG_TEXT_MAX + 1], after[VJO_CONFIG_TEXT_MAX + 1];
    reset(VJO_BUTTON_ASSIGN_CROSS);
    queue_card("猫", 1);
    queue_card("犬", 0);
    show();
    frame(0);
    for (int i = 0; i < 6; i++)
        frame(SCE_CTRL_DOWN); /* row 6 */
    TEST_CHECK(text_starting("Export cards (2 queued)"));
    frame(SCE_CTRL_CROSS);
    TEST_CHECK(vjo_anki_page_busy() && threads_created == 1 && thread_stack == 64 * 1024 &&
               thread_affinity == SCE_KERNEL_CPU_MASK_SYSTEM && thread_priority == 0x10000100);
    frame(0);
    TEST_CHECK(!strcmp(badge(EXPORT)->label, "Exporting…") && badge(EXPORT)->active &&
               !strcmp(badge(KNOWN)->label, "Import") && !badge(KNOWN)->active);
    TEST_CHECK(text_starting("Reading the cards…") && text_starting("Imports known_words.txt") &&
               !strcmp(drawn.controls, "○: stop   Select: page"));
    /* busy: no save, no second export, from Enter, Left/Right or a touch */
    TEST_ASSERT(save_now(0, 1) == 0);
    TEST_ASSERT(read_file(VJO_PREFERENCES_PATH, before, sizeof(before)) > 0);
    for (int i = 0; i < 6; i++)
        frame(SCE_CTRL_UP);
    TEST_CHECK(focused() == 0); /* (the selection still moves) */
    frame(SCE_CTRL_CROSS);
    frame(SCE_CTRL_RIGHT);
    touch(0, 480, row_top_at(1, 0) + 10);
    touch(0, 480, row_top_at(2, 0) + 10);
    touch(0, 480, row_top_at(3, 0) + 10);
    TEST_ASSERT(read_file(VJO_PREFERENCES_PATH, after, sizeof(after)) > 0);
    TEST_CHECK(!strcmp(before, after) && threads_created == 1 && vjo_anki_page_busy() && !vjo_cards_page_is_open());
    TEST_CHECK(vjo_anki_page_holds_cancel());
    /* the export ends: the page joins it and counts again */
    run_worker();
    frame(0);
    TEST_CHECK(!vjo_anki_page_busy() && threads_deleted == 1);
    TEST_CHECK(strstr(drawn.footer, "Exported 2 cards (") && strstr(drawn.footer, "to 2026-10-12_093005-2cards.apkg.") &&
               strstr(drawn.footer, "Copy it from ux0:data/VitaJPOverlay/anki/ by VitaShell's FTP or USB") &&
               !drawn.footer_error);
    TEST_MSG("%s", drawn.footer);
    TEST_CHECK(there(VJO_EXPORT_DIR "/2026-10-12_093005-2cards.apkg") && files_in(VJO_QUEUE_DIR, ".json") == 0 &&
               files_in(VJO_QUEUE_EXPORTED_DIR, ".json") == 2 && files_in(VJO_QUEUE_EXPORTED_DIR, ".jpg") == 1);
    TEST_CHECK(strstr(logs, "anki: export (queued) rc=0: 2 cards, 2 moved, 0 kept, 0 unreadable, 0 too large, 0 left, "
                            "0 stopped, 0 swept, ") != NULL);
    TEST_MSG("%s", logs);
    /* Export all cards: the two again (the last row, by the stick) */
    vjo_anki_page_stick(4);
    frame(0);
    TEST_CHECK(focused() == ALL && first_row() == 5 && text_starting("2 cards in all"));
    TEST_CHECK(text_starting("Export cards (0 queued)") && !strcmp(badge(EXPORT)->label, "Export") &&
               !badge(EXPORT)->active);
    frame(SCE_CTRL_CROSS);
    frame(0);
    TEST_CHECK(!strcmp(badge(ALL)->label, "Exporting…") && badge(ALL)->active && !strcmp(badge(EXPORT)->label, "Export") &&
               !badge(EXPORT)->active);
    /* the progress on its own row only; the last export's result is not the footer while this one runs */
    TEST_CHECK(text_starting("Reading the cards…") && text_starting("Writes a new .apkg"));
    TEST_CHECK_(!strstr(drawn.footer, "Exported") && strstr(drawn.footer, "Copy the .apkg from"), "busy: %s", drawn.footer);
    run_worker();
    frame(0);
    TEST_CHECK(strstr(drawn.footer, "Exported 2 cards (") && strstr(drawn.footer, "to 2026-10-12_093005-all-2cards.apkg."));
    TEST_MSG("%s", drawn.footer);
    /* Cancel: stopped, nothing changed */
    queue_card("鳥", 0);
    show();
    frame(SCE_CTRL_UP);
    frame(SCE_CTRL_CROSS); /* row 6 */
    TEST_CHECK(focused() == EXPORT && vjo_anki_page_busy());
    frame(SCE_CTRL_CIRCLE);
    run_worker();
    frame(0);
    TEST_CHECK(!vjo_anki_page_busy() && !strcmp(drawn.footer, "Stopped. Nothing was changed.") && drawn.footer_error);
    TEST_MSG("%s", drawn.footer);
    TEST_CHECK(files_in(VJO_QUEUE_DIR, ".json") == 1 && files_in(VJO_EXPORT_DIR, ".apkg") == 2);
    /* Cancel with nothing running does nothing */
    frame(SCE_CTRL_CIRCLE);
    TEST_CHECK(!vjo_anki_page_busy() && threads_created == 3);
    /* leaving the app joins a running export, cancelled: the queue as it was */
    frame(SCE_CTRL_CROSS);
    TEST_CHECK(vjo_anki_page_busy() && threads_created == 4);
    vjo_anki_page_quit();
    TEST_CHECK(!vjo_anki_page_busy() && !pending && threads_deleted == 4);
    TEST_CHECK(files_in(VJO_QUEUE_DIR, ".json") == 1 && files_in(VJO_EXPORT_DIR, ".apkg") == 2);
    /* with ○ as enter, ○ during an export does not stop it, and × does */
    buttons = vjo_buttons(VJO_BUTTON_ASSIGN_CIRCLE);
    frame(SCE_CTRL_CIRCLE);
    TEST_CHECK(focused() == EXPORT && vjo_anki_page_busy() && threads_created == 5);
    frame(0);
    TEST_CHECK(!strcmp(drawn.controls, "×: stop   Select: page"));
    frame(SCE_CTRL_CIRCLE);
    run_worker();
    frame(0);
    TEST_CHECK(!vjo_anki_page_busy() && strstr(drawn.footer, "Exported 1 card (") && !drawn.footer_error);
    TEST_MSG("%s", drawn.footer);
    TEST_CHECK(files_in(VJO_QUEUE_DIR, ".json") == 0 && files_in(VJO_EXPORT_DIR, ".apkg") == 3);
    queue_card("魚", 0);
    show();
    frame(SCE_CTRL_CIRCLE);
    TEST_CHECK(vjo_anki_page_busy() && threads_created == 6);
    frame(SCE_CTRL_CROSS);
    run_worker();
    frame(0);
    TEST_CHECK(!vjo_anki_page_busy() && !strcmp(drawn.footer, "Stopped. Nothing was changed.") && drawn.footer_error);
    TEST_CHECK(files_in(VJO_QUEUE_DIR, ".json") == 1 && files_in(VJO_EXPORT_DIR, ".apkg") == 3);
}

/* ---------------- #314: Duplicates ---------------- */

static const char *const dupes_names[3] = {"tag", "skip", "keep"};
static const char *const dupes_help[3] = {"A word you mined before gets Anki's duplicate tag.",
                                          "A word you mined before stays queued, not exported.",
                                          "Every card is exported, with no duplicate check."};

/* app.ini holds Duplicates = value, once, as the app loads it back; the row shows it lit, with its help. */
static void check_dupes(int value)
{
    char text[4096], want[48];
    VjoAppPrefs prefs;
    snprintf(want, sizeof(want), "anki_duplicates = %s\n", dupes_names[value]);
    TEST_ASSERT(read_file(VJO_APP_PREFS_PATH, text, sizeof(text)) > 0);
    TEST_CHECK_(strstr(text, want) && count_lines(text, "anki_duplicates") == 1, "app.ini: %s, want %s", text, want);
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    TEST_CHECK(vjo_app_prefs_load(&prefs, &scratch) == 0 && prefs.anki_duplicates == value);
    frame(0);
    for (int k = 0; k < 3; k++)
        TEST_CHECK_(badge_k(DUPES, k)->active == (k == value), "choice %d lit %d, want %d", k, badge_k(DUPES, k)->active,
                    value);
    TEST_CHECK(text_starting(dupes_help[value]) != NULL);
}

/* The Duplicates row (#314): Enter takes the next choice, round; Left/Right the one to the left or right, and nothing
 * at the ends; a touch on a choice takes it, elsewhere on the row only selects it. Each choice is saved in app.ini,
 * which the overlay never reads, at once. */
static void the_duplicates_row_saves_app_ini(void)
{
    const int y = row_top_at(DUPES, 0);
    reset(VJO_BUTTON_ASSIGN_CROSS);
    show();
    for (int i = 0; i < 3; i++)
        frame(SCE_CTRL_DOWN);
    TEST_CHECK(focused() == DUPES && badge_k(DUPES, 0)->active && !there(VJO_APP_PREFS_PATH));
    frame(SCE_CTRL_CROSS);
    check_dupes(VJO_DUPLICATES_SKIP);
    TEST_CHECK(!strcmp(drawn.footer, "Saved. The next export uses it.") && !drawn.footer_error);
    frame(SCE_CTRL_CROSS);
    check_dupes(VJO_DUPLICATES_KEEP);
    frame(SCE_CTRL_CROSS);
    check_dupes(VJO_DUPLICATES_TAG);
    renames = 0;
    frame(SCE_CTRL_LEFT); /* Tag is the first: nothing */
    TEST_CHECK(renames == 0);
    frame(SCE_CTRL_RIGHT);
    check_dupes(VJO_DUPLICATES_SKIP);
    frame(SCE_CTRL_RIGHT);
    check_dupes(VJO_DUPLICATES_KEEP);
    renames = 0;
    frame(SCE_CTRL_RIGHT); /* Keep is the last */
    TEST_CHECK(renames == 0);
    frame(SCE_CTRL_LEFT);
    check_dupes(VJO_DUPLICATES_SKIP);
    /* touches: each choice, from its edges; between them, on the title or the help: only the selection */
    frame(SCE_CTRL_UP);
    TEST_CHECK(focused() == 2);
    touch(0, 440, y + 6);
    TEST_CHECK(focused() == DUPES);
    check_dupes(VJO_DUPLICATES_TAG);
    touch(0, 440 + 2 * 160 + 151, y + 39);
    check_dupes(VJO_DUPLICATES_KEEP);
    touch(0, 440 + 160 + 75, y + 20);
    check_dupes(VJO_DUPLICATES_SKIP);
    renames = 0;
    static const int miss[][2] = {{440 + 152, 20}, {440 + 160 + 159, 20}, {439, 20}, {440 + 3 * 160, 20},
                                  {100, 20}, {600, 5}, {600, 40}, {600, 60}};
    for (unsigned i = 0; i < sizeof(miss) / sizeof(miss[0]); i++) {
        frame(SCE_CTRL_UP);
        touch(0, miss[i][0], y + miss[i][1]);
        TEST_CHECK_(focused() == DUPES && renames == 0, "a touch at %d,+%d saved", miss[i][0], miss[i][1]);
    }
    check_dupes(VJO_DUPLICATES_SKIP);
    TEST_CHECK(threads_created == 0);
    /* what else app.ini holds stays as it is */
    {
        FILE *f = fopen(VJO_APP_PREFS_PATH, "ab");
        TEST_ASSERT(f && fputs("; mine\nfuture_key = 1\n", f) >= 0);
        fclose(f);
    }
    frame(SCE_CTRL_LEFT);
    check_dupes(VJO_DUPLICATES_TAG);
    {
        char text[4096];
        TEST_ASSERT(read_file(VJO_APP_PREFS_PATH, text, sizeof(text)) > 0);
        TEST_CHECK(strstr(text, "; mine\nfuture_key = 1\n") != NULL);
    }
    TEST_CHECK(!there(VJO_PREFERENCES_PATH)); /* config.ini, the overlay's, never */
}

/* An unreadable app.ini: no choice lit, the row says why, and nothing is written; a save that does not complete says
 * so and shows the file's choice; the overlay's own rows still save. */
static void an_unreadable_app_ini_is_never_written(void)
{
    char before[4096], after[4096];
    reset(VJO_BUTTON_ASSIGN_CROSS);
    show();
    for (int i = 0; i < 3; i++)
        frame(SCE_CTRL_DOWN);
    frame(SCE_CTRL_RIGHT);
    check_dupes(VJO_DUPLICATES_SKIP);
    TEST_ASSERT(read_file(VJO_APP_PREFS_PATH, before, sizeof(before)) > 0);
    fail_open = "app.ini";
    show();
    frame(0);
    for (int k = 0; k < 3; k++)
        TEST_CHECK(!badge_k(DUPES, k)->active);
    TEST_CHECK(text_starting("Cannot read app.ini.") != NULL);
    writes_opened = renames = 0;
    frame(SCE_CTRL_CROSS);
    TEST_CHECK(!strcmp(drawn.footer, "Cannot read app.ini. The choice has not been changed.") && drawn.footer_error);
    frame(SCE_CTRL_LEFT);
    frame(SCE_CTRL_RIGHT);
    touch(0, 450, row_top_at(DUPES, 0) + 10);
    TEST_CHECK(writes_opened == 0 && renames == 0);
    fail_open = NULL;
    TEST_ASSERT(read_file(VJO_APP_PREFS_PATH, after, sizeof(after)) > 0);
    TEST_CHECK(!strcmp(before, after));
    /* (config.ini's rows are not app.ini's) */
    frame(SCE_CTRL_UP);
    frame(SCE_CTRL_CROSS);
    check_saved3(0, 1, 0);
    /* a save that does not complete: said, and the choice shown is the file's */
    show();
    frame(SCE_CTRL_DOWN);
    fail_rename = 1;
    frame(SCE_CTRL_RIGHT);
    fail_rename = 0;
    TEST_CHECK(!strcmp(drawn.footer, "Save not confirmed. Check the choice shown, then retry.") && drawn.footer_error);
    check_dupes(VJO_DUPLICATES_SKIP);
}

/* The export row counts the queue's words mined before (an earlier card has them, queued or exported): "N queued, M
 * already mined"; the export follows Duplicates (Tag tags them, Skip leaves them queued), and the count follows the
 * export. The page reads at most 64 cards' words when it is shown: past them it shows the count only once it has
 * read the rest (an export reads every one). */
static void the_export_row_counts_words_mined_before(void)
{
    char name[32];
    reset(VJO_BUTTON_ASSIGN_CROSS);
    queue_line("猫", "猫が好き", 0, 0);
    queue_line("猫", "猫を見た", 1, 0);
    queue_line("犬", "犬だ", 2, 0);
    show();
    for (int i = 0; i < 6; i++)
        frame(SCE_CTRL_DOWN);
    TEST_CHECK(text_starting("Export cards (3 queued, 1 already mined)") != NULL);
    frame(SCE_CTRL_CROSS); /* Tag: the second 猫 tagged */
    run_worker();
    frame(0);
    TEST_CHECK(strstr(drawn.footer, "Exported 3 cards (") && strstr(drawn.footer, " 1 tagged duplicate (mined before)."));
    TEST_MSG("%s", drawn.footer);
    TEST_CHECK(text_starting("Export cards (0 queued)") != NULL);
    /* a word of an exported card: Skip leaves it queued, and counted */
    queue_line("猫", "猫だ", 3, 0);
    queue_line("鳥", "鳥だ", 4, 0);
    show();
    frame(0);
    TEST_CHECK(text_starting("Export cards (2 queued, 1 already mined)") != NULL);
    frame(SCE_CTRL_UP);
    frame(SCE_CTRL_UP);
    frame(SCE_CTRL_UP);
    frame(SCE_CTRL_RIGHT); /* Duplicates: Skip */
    check_dupes(VJO_DUPLICATES_SKIP);
    frame(SCE_CTRL_DOWN);
    frame(SCE_CTRL_DOWN);
    frame(SCE_CTRL_DOWN);
    frame(0);
    TEST_CHECK(focused() == EXPORT && first_row() == 3);
    frame(SCE_CTRL_CROSS);
    run_worker();
    frame(0);
    TEST_CHECK(strstr(drawn.footer, "Exported 1 card (") && strstr(drawn.footer, " 1 already mined stays queued (Duplicates: Skip)."));
    TEST_MSG("%s", drawn.footer);
    TEST_CHECK(text_starting("Export cards (1 queued, 1 already mined)") && files_in(VJO_QUEUE_DIR, ".json") == 1);
    frame(SCE_CTRL_CROSS);
    run_worker();
    frame(0);
    TEST_CHECK(!strcmp(drawn.footer, "No new cards: the 1 queued is a word already mined, and stays queued "
                                     "(Duplicates: Skip).") && !drawn.footer_error);
    TEST_MSG("%s", drawn.footer);
    TEST_CHECK(text_starting("Export cards (1 queued, 1 already mined)") != NULL);
    TEST_CHECK(strstr(logs, "1 duplicates (skip)") != NULL);
    TEST_MSG("%s", logs);
    /* 65 new cards beside the 猫: the page reads 64 when shown, so no count until it has read the rest */
    for (int i = 0; i < 65; i++) {
        snprintf(name, sizeof(name), "語%d", i);
        queue_line(name, name, 10 + i, 0);
    }
    show();
    frame(0);
    TEST_CHECK(text_starting("Export cards (66 queued)") && !text_starting("Export cards (66 queued,"));
    show();
    frame(0);
    TEST_CHECK(text_starting("Export cards (66 queued, 1 already mined)") != NULL);
    TEST_CHECK(threads_created == 3);
}

/* audit-rubi-rm4 P1, P2, P10: while an export runs, the Duplicates row saves nothing, by Enter, Left/Right or a touch on
 * any of its choices, and the export row shows no "already mined" count (the job is changing the queue). */
static void a_busy_page_changes_no_choice(void)
{
    reset(VJO_BUTTON_ASSIGN_CROSS);
    queue_line("猫", "猫が好き", 0, 0);
    queue_line("猫", "猫を見た", 1, 0);
    queue_line("犬", "犬だ", 2, 0);
    show();
    for (int i = 0; i < EXPORT; i++)
        frame(SCE_CTRL_DOWN);
    TEST_CHECK(focused() == EXPORT && text_starting("Export cards (3 queued, 1 already mined)"));
    frame(SCE_CTRL_CROSS);
    TEST_ASSERT(vjo_anki_page_busy());
    frame(0);
    TEST_CHECK(text_starting("Export cards (3 queued)") && !text_starting("Export cards (3 queued, 1"));
    for (int i = 0; i < EXPORT - DUPES; i++)
        frame(SCE_CTRL_UP);
    TEST_CHECK(focused() == DUPES);
    writes_opened = renames = 0;
    frame(SCE_CTRL_RIGHT);
    frame(SCE_CTRL_LEFT);
    frame(SCE_CTRL_CROSS);
    frame(0);
    for (int k = 1; k < 3; k++)
        touch(0, 440 + k * 160 + 20, row_top_at(DUPES, first_row()) + 20);
    TEST_CHECK(writes_opened == 0 && renames == 0 && !there(VJO_APP_PREFS_PATH));
    TEST_CHECK(badge_k(DUPES, 0)->active && !badge_k(DUPES, 1)->active && !badge_k(DUPES, 2)->active);
    run_worker();
    frame(0);
    TEST_CHECK(!vjo_anki_page_busy() && strstr(drawn.footer, " 1 tagged duplicate (mined before)."));
    /* idle again: the same touch takes Skip */
    touch(0, 440 + 160 + 20, row_top_at(DUPES, first_row()) + 20);
    check_dupes(VJO_DUPLICATES_SKIP);
}

/* audit-rubi-rm4 P7, P9, N9: an export that could not check its duplicates (too many words for memory: here a cache
 * whose count does not fit) or its known words (a damaged index) is shown as an error, as the app's other incomplete
 * results are; the Known words row says a damaged index cannot be read, rather than its count. */
static void unchecked_exports_are_errors(void)
{
    FILE *f;
    reset(VJO_BUTTON_ASSIGN_CROSS);
    queue_line("猫", "猫が好き", 0, 0);
    /* a cache of 100,000 words (2,400,000 B of them: more than the export's 2 MiB) */
    TEST_ASSERT((f = fopen(VJO_EXPORT_DATA_DIR "/anki_words.bin", "wb")) != NULL);
    {
        static const unsigned char head[16] = {'R', 'u', 'b', 'i', 'W', 'd', '1', '\n', 0x00, 0x01, 0x86, 0xA0};
        TEST_ASSERT(fwrite(head, 1, 16, f) == 16 && ftruncate(fileno(f), 16 + 100000 * 20) == 0);
    }
    fclose(f);
    show();
    for (int i = 0; i < EXPORT; i++)
        frame(SCE_CTRL_DOWN);
    TEST_CHECK(focused() == EXPORT);
    frame(SCE_CTRL_CROSS);
    run_worker();
    frame(0);
    TEST_CHECK(strstr(drawn.footer, "Exported 1 card (") &&
               strstr(drawn.footer, " Not checked for duplicates: too many cards for memory.") && drawn.footer_error);
    TEST_MSG("%s", drawn.footer);
    TEST_ASSERT(unlink(VJO_EXPORT_DATA_DIR "/anki_words.bin") == 0);
    /* a damaged index of known words */
    TEST_ASSERT((f = fopen(VJO_EXPORT_DATA_DIR "/anki_known.bin", "wb")) != NULL);
    fputs("RubiKn1\n\0\0\0\1\0\0\0\0abcdefgh", f); /* one key, its checksum wrong */
    fclose(f);
    queue_line("犬", "犬だ", 1, 0);
    show();
    frame(0);
    TEST_CHECK(focused() == EXPORT && text_starting("The known words cannot be read: Import again.") != NULL);
    frame(SCE_CTRL_CROSS);
    run_worker();
    frame(0);
    TEST_CHECK(strstr(drawn.footer, "Exported 1 card (") && strstr(drawn.footer, " Not checked for known words") &&
               drawn.footer_error);
    TEST_MSG("%s", drawn.footer);
}

/* #314 stage C, the Known words row: Enter (or a touch) imports known_words.txt on the page's worker, the row says
 * so meanwhile, then the result and the count; Left/Right do nothing there; the next export tags the known words'
 * cards known. */
static void the_known_words_row_imports_the_file(void)
{
    FILE *f;
    reset(VJO_BUTTON_ASSIGN_CROSS);
    show();
    for (int i = 0; i < 5; i++)
        frame(SCE_CTRL_DOWN);
    TEST_CHECK(focused() == KNOWN && !strcmp(badge(KNOWN)->label, "Import") && !badge(KNOWN)->active &&
               text_starting("Imports known_words.txt (tools/known_words.py makes it)."));
    frame(SCE_CTRL_LEFT);
    frame(SCE_CTRL_RIGHT);
    TEST_CHECK(threads_created == 0 && !vjo_anki_page_busy());
    /* no file: said, nothing else */
    frame(SCE_CTRL_CROSS);
    TEST_CHECK(vjo_anki_page_busy() && threads_created == 1);
    frame(0);
    TEST_CHECK(!strcmp(badge(KNOWN)->label, "Importing…") && badge(KNOWN)->active &&
               text_starting("Reading known_words.txt…") && !strcmp(badge(EXPORT)->label, "Export"));
    run_worker();
    frame(0);
    TEST_CHECK(!vjo_anki_page_busy() && strstr(drawn.footer, "No known_words.txt in ux0:data/VitaJPOverlay/") &&
               !drawn.footer_error);
    TEST_MSG("%s", drawn.footer);
    /* a file: imported, counted, its card tagged by the next export */
    TEST_ASSERT((f = fopen(VJO_EXPORT_DATA_DIR "/known_words.txt", "wb")) != NULL);
    fputs("\xEF\xBB\xBF# from Anki\r\n猫\tよみ\r\n犬\r\n\tのみ\r\n", f);
    fclose(f);
    touch(0, 480, row_top_at(KNOWN, 2) + 10);
    TEST_CHECK(vjo_anki_page_busy() && threads_created == 2);
    run_worker();
    frame(0);
    TEST_CHECK(strstr(drawn.footer, "Imported 2 known words from known_words.txt: their cards get the tag known. 1 line "
                                    "is not a word (see log.txt).") && drawn.footer_error);
    TEST_MSG("%s", drawn.footer);
    TEST_CHECK(text_starting("2 known words: their cards get the tag known.") != NULL);
    TEST_CHECK(strstr(logs, "anki: known words import rc=0: 2 words, 2 keys, 1 not words, 0 over") != NULL);
    TEST_MSG("%s", logs);
    queue_line("猫", "猫が好き", 0, 0);
    queue_line("鳥", "鳥だ", 1, 0);
    show();
    frame(SCE_CTRL_DOWN);
    TEST_CHECK(focused() == EXPORT);
    frame(SCE_CTRL_CROSS);
    run_worker();
    frame(0);
    TEST_CHECK(strstr(drawn.footer, "Exported 2 cards (") && strstr(drawn.footer, " 1 tagged known (known_words.txt)."));
    TEST_MSG("%s", drawn.footer);
    TEST_CHECK(strstr(logs, "0 duplicates (tag), 1 known, ") != NULL);
    /* the file removed: Import clears them */
    TEST_ASSERT(unlink(VJO_EXPORT_DATA_DIR "/known_words.txt") == 0);
    frame(SCE_CTRL_UP);
    frame(SCE_CTRL_CROSS);
    run_worker();
    frame(0);
    TEST_CHECK(strstr(drawn.footer, "No known_words.txt") && text_starting("Imports known_words.txt"));
}

/* An unreadable config.ini is never written: its rows show "--" and say why; a save that does not
 * complete says so; a queue that cannot be read is shown as such; a thread that cannot start says so. */
static void a_missing_screenshot_is_reported_after_export(void)
{
    char path[256];
    reset(VJO_BUTTON_ASSIGN_CROSS);
    snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s.jpg", queue_card("猫", 1));
    TEST_ASSERT(unlink(path) == 0);
    show();
    for (int i = 0; i < 6; i++)
        frame(SCE_CTRL_DOWN);
    touch(0, 480, row_top_at(EXPORT, 3) + 10);
    run_worker();
    frame(0);
    TEST_CHECK(!vjo_anki_page_busy() && text_starting("Export cards (0 queued)"));
    TEST_CHECK(strstr(drawn.footer, "Exported 1 card (") &&
               strstr(drawn.footer, "1 exported without a screenshot") && drawn.footer_error);
    TEST_MSG("%s", drawn.footer);
    TEST_CHECK(strstr(logs, "1 without screenshots") != NULL);
}

static void failures_are_shown_and_change_nothing(void)
{
    reset(VJO_BUTTON_ASSIGN_CROSS);
    TEST_ASSERT(save_now(1, 1) == 0);
    fail_open = "config.ini";
    show();
    frame(0);
    TEST_CHECK(!strcmp(badge(0)->label, "--") && !badge(0)->active && !strcmp(badge(1)->label, "--") &&
               !badge(1)->active && !strcmp(badge(2)->label, "--") && !badge(2)->active);
    writes_opened = renames = 0;
    frame(SCE_CTRL_CROSS);
    TEST_CHECK(!strcmp(drawn.footer, "Cannot read config.ini. Choices have not been changed.") && drawn.footer_error);
    frame(SCE_CTRL_DOWN);
    frame(SCE_CTRL_RIGHT);
    frame(SCE_CTRL_LEFT);
    frame(SCE_CTRL_DOWN); /* API 32: the black bars' row too */
    frame(SCE_CTRL_CROSS);
    frame(SCE_CTRL_LEFT);
    TEST_CHECK(writes_opened == 0 && renames == 0);
    fail_open = NULL;
    check_saved(1, 1);
    /* a save that does not complete: said, and the choice shown is the file's */
    show();
    frame(SCE_CTRL_UP);
    frame(SCE_CTRL_UP);
    fail_rename = 1;
    frame(SCE_CTRL_CROSS);
    fail_rename = 0;
    TEST_CHECK(!strcmp(drawn.footer, "Save not confirmed. Check the choices shown, then retry.") && drawn.footer_error);
    frame(0);
    TEST_CHECK(!strcmp(badge(0)->label, "ON"));
    check_saved(1, 1);
    /* the queue unreadable */
    queue_card("猫", 0);
    fail_dopen = VJO_QUEUE_DIR;
    show();
    frame(0);
    frame(0); /* #315: the Cards row says so too (row 4, cut at the room's bottom) */
    {
        const Text *c = text_starting("Cards (queue unreadable)");
        TEST_CHECK(c && c->color == OFF);
    }
    frame(SCE_CTRL_UP); /* round to row 7: both export rows in view */
    TEST_CHECK(focused() == ALL);
    const Text *t = text_starting("Export cards (queue unreadable)");
    TEST_CHECK(t && t->color == OFF && text_starting("Unreadable"));
    fail_dopen = NULL;
    /* the worker cannot start */
    fail_create = 1;
    frame(SCE_CTRL_UP);
    TEST_CHECK(focused() == EXPORT);
    frame(SCE_CTRL_CROSS);
    TEST_CHECK(!vjo_anki_page_busy() && !strcmp(drawn.footer, "Could not start the export (0x80020001).") &&
               drawn.footer_error);
    TEST_MSG("%s", drawn.footer);
    fail_create = 0;
    TEST_CHECK(files_in(VJO_QUEUE_DIR, ".json") == 1);
}

/* ---------------- the Cards page (#315) ---------------- */

extern int vjo_test_picture_decodes, vjo_test_picture_fail; /* settings_stubs/card_picture_stub.c */

/* A card as Enter saves it (version 4), with what the Cards page shows of it; its ID. */
static const char *queue_full(const char *spelling, const char *sentence, int hl_start, int hl_end, int picture,
                              int64_t time)
{
    static char id[VJO_QUEUE_ID_SIZE];
    static unsigned char mem[64 * 1024];
    const char *m[2] = {"[JMdict Plain]\na meaning", "a second meaning"}, *pos[1] = {"n"};
    char path[256];
    VjoArena a;
    VjoAnkiNote n;
    char *json;
    FILE *f;
    vjo_arena_init(&a, mem, sizeof(mem));
    memset(&n, 0, sizeof(n));
    n.spelling = spelling;
    n.reading = "よみ";
    n.rank = VJO_NO_RANK;
    n.meanings = m;
    n.n_meanings = 2;
    n.sentence = sentence;
    n.hl_start = hl_start;
    n.hl_end = hl_end;
    n.sentence_furigana = sentence;
    n.sf_hl_start = hl_start;
    n.sf_hl_end = hl_end;
    n.pos = pos;
    n.n_pos = 1;
    n.misc = "SOME GAME (2026-10-12 09:30)";
    n.game = "SOME GAME";
    n.title_id = "PCSG00000";
    n.platform = "vita";
    n.unix_time = time;
    n.ocr_engine = "vocr";
    n.ocr_model = "VOCR5_N_w8g2.vocr";
    TEST_ASSERT((json = vjo_queue_encode(&a, &n, picture)) != NULL);
    vjo_queue_id(json, strlen(json), id);
    mkdir(VJO_QUEUE_DIR, 0777);
    snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s.json", id);
    TEST_ASSERT((f = fopen(path, "wb")) != NULL);
    TEST_ASSERT(fwrite(json, 1, strlen(json), f) == strlen(json));
    fclose(f);
    if (picture) {
        static const unsigned char jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 9, 8, 7, 6, 5, 4, 3, 2, 0xFF, 0xD9};
        snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s.jpg", id);
        TEST_ASSERT((f = fopen(path, "wb")) != NULL);
        TEST_ASSERT(fwrite(jpeg, 1, sizeof(jpeg), f) == sizeof(jpeg));
        fclose(f);
    }
    return id;
}

/* A text drawn that holds s / one drawn at x, y. */
static const Text *text_has(const char *s)
{
    for (int i = 0; i < drawn.texts; i++)
        if (strstr(drawn.text[i].s, s))
            return &drawn.text[i];
    return NULL;
}
static const Badge *badge_named(const char *label)
{
    for (int i = 0; i < drawn.badges; i++)
        if (!strcmp(drawn.badge[i].label, label))
            return &drawn.badge[i];
    return NULL;
}
/* The card view's sentence as drawn: its rows' texts (24 px, at x 42.., y 210..350) in order, rows joined by
 * a line break. */
static const char *sentence_drawn(void)
{
    static char out[2048];
    int used = 0, last_y = -1;
    out[0] = '\0';
    for (int y = 210; y < 350; y += 28)
        for (int i = 0; i < drawn.texts; i++) /* (drawn left to right) */
            if (drawn.text[i].y == y && drawn.text[i].px == 24 && drawn.text[i].x >= 42 && drawn.text[i].x < 490) {
                used += snprintf(out + used, sizeof(out) - (size_t)used, "%s%s", last_y >= 0 && last_y != y ? "\n" : "",
                                 drawn.text[i].s);
                last_y = y;
            }
    return out;
}
/* The list's cards drawn (44 px high), and the one focused (-1: none). */
static int list_rows(void)
{
    int n = 0;
    for (int i = 0; i < drawn.cards; i++)
        n += drawn.card[i].height == 44;
    return n;
}
static int list_focused(void)
{
    for (int i = 0, k = 0; i < drawn.cards; i++)
        if (drawn.card[i].height == 44) {
            if (drawn.card[i].focus)
                return k;
            k++;
        }
    return -1;
}
/* The word of the list's row k drawn: the 22 px text at x 42 on its line. */
static const char *row_word(int k)
{
    int y = 156 + k * 48 + 21;
    for (int i = 0; i < drawn.texts; i++)
        if (drawn.text[i].x == 42 && drawn.text[i].y == y && drawn.text[i].px == 22)
            return drawn.text[i].s;
    return "";
}
/* From the Anki page (its first row selected), the Cards page opened and its list read. */
static void open_cards(void)
{
    int before = threads_created;
    for (int k = 0; k < CARDS; k++)
        frame(SCE_CTRL_DOWN);
    TEST_ASSERT(focused() == CARDS);
    frame(SCE_CTRL_CROSS);
    TEST_ASSERT(vjo_cards_page_is_open() && vjo_anki_page_busy() && threads_created == before + 1);
    run_worker();
    frame(0);
    TEST_ASSERT(!vjo_anki_page_busy());
}
/* The job a press started, run, and the frames after it until the worker is idle (a change reads the
 * list again, then opens the changed card). */
static void run_jobs(void)
{
    for (int k = 0; k < 4 && vjo_anki_page_busy(); k++) {
        run_worker();
        frame(0);
    }
    TEST_ASSERT(!vjo_anki_page_busy());
}
static int in_dir(const char *dir, const char *hex, const char *ext)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.%s", dir, hex, ext);
    return there(path);
}

/* The Cards row opens the queue on the worker (64 KiB, CPU3), newest first: each card's word, reading, game
 * and date, a camera for a card with a picture; the lists' buttons and where the page is; Up/Down and the
 * stick; Cancel goes back to the Anki page, never out of the app. */
static void the_cards_row_opens_the_queue_newest_first(void)
{
    reset(VJO_BUTTON_ASSIGN_CROSS);
    queue_full("猫", "猫が好き", 0, 3, 1, 1791790200);
    queue_full("犬", "犬だ", 0, 3, 0, 1791790300);
    queue_full("鳥", "鳥だ", 0, 3, 0, 1791790100);
    show();
    frame(0);
    for (int k = 0; k < CARDS; k++)
        frame(SCE_CTRL_DOWN);
    /* (the Anki page writes its word index when shown, #314: the Cards page itself writes nothing) */
    int writes = writes_opened;
    frame(SCE_CTRL_CROSS);
    TEST_ASSERT(vjo_cards_page_is_open() && vjo_anki_page_busy() && vjo_anki_page_holds_cancel());
    TEST_CHECK(threads_created == 1 && thread_stack == 64 * 1024 && thread_affinity == SCE_KERNEL_CPU_MASK_SYSTEM &&
               thread_priority == 0x10000100);
    /* drawn at once: reading */
    TEST_CHECK(drawn.headers == 1 && text_has("Reading the cards…") && !strcmp(drawn.controls, "○: stop   Select: page"));
    run_worker();
    frame(0);
    TEST_ASSERT(list_rows() == 3 && list_focused() == 0);
    TEST_CHECK(!strcmp(row_word(0), "犬") && !strcmp(row_word(1), "猫") && !strcmp(row_word(2), "鳥"));
    TEST_CHECK(text_has("SOME GAME") && text_has("2026-10-12 09:30") && text_has("よみ"));
    TEST_CHECK(badge_named("Queued (3)") && badge_named("Queued (3)")->active && badge_named("Exported (0)") &&
               !badge_named("Exported (0)")->active && badge_named("Duplicates") && text_has("1-3 of 3"));
    TEST_CHECK(!strcmp(drawn.controls, "Up/Down: card   L/R: page   Left/Right: list   ×: open   ○: back"));
    TEST_CHECK(strstr(drawn.footer, "Square: duplicates only") && strstr(drawn.footer, "Triangle: empty deleted (0 cards)"));
    /* the camera: three rectangles at x 892 on 猫's row only */
    {
        int on_cat = 0, elsewhere = 0;
        for (int i = 0; i < drawn.rects; i++)
            if (drawn.rect[i].x >= 892 && drawn.rect[i].x < 920 && drawn.rect[i].color == ACCENT) {
                if (drawn.rect[i].y >= 156 + 48 && drawn.rect[i].y < 156 + 96)
                    on_cat++;
                else
                    elsewhere++;
            }
        TEST_CHECK_(on_cat >= 2 && !elsewhere, "camera: %d on 猫's row, %d elsewhere", on_cat, elsewhere);
    }
    frame(SCE_CTRL_DOWN);
    TEST_CHECK(list_focused() == 1);
    vjo_anki_page_stick(5);
    frame(0);
    TEST_CHECK(list_focused() == 2);
    frame(SCE_CTRL_UP);
    TEST_CHECK(list_focused() == 1);
    TEST_CHECK_(writes_opened == writes, "the Cards page opened %d files for writing", writes_opened - writes);
    /* Cancel: the Anki page again, drawn in that frame, its count read again; the app keeps running */
    frame(SCE_CTRL_CIRCLE);
    TEST_CHECK(!vjo_cards_page_is_open() && !vjo_anki_page_holds_cancel() && focused() == CARDS &&
               text_starting("Cards: 3 queued"));
    TEST_CHECK(threads_created == 1 && threads_deleted == 1);
}

/* Enter opens a card on the worker: its word, reading, game and date, its sentence with the word in the
 * accent colour, its definition's first lines, its picture as a texture drawn in its box; the actions; L/R
 * the card after or before it; Cancel the list again, the texture freed once the GPU is done with it. */
static void a_card_shows_its_sentence_definition_and_picture(void)
{
    reset(VJO_BUTTON_ASSIGN_CROSS);
    queue_full("猫", "白い猫が好き", 6, 9, 1, 1791790200);
    queue_full("犬", "犬だ", 0, 3, 0, 1791790100);
    show();
    frame(0);
    open_cards();
    frame(SCE_CTRL_CROSS); /* 猫, the newest */
    TEST_ASSERT(vjo_anki_page_busy());
    run_worker();
    frame(0);
    TEST_CHECK(text_has("猫") && text_has("よみ") && text_has("SOME GAME · 2026-10-12 09:30"));
    {
        const Text *before = text_has("白い"), *word = NULL, *after = text_has("が好き");
        for (int i = 0; i < drawn.texts; i++)
            if (!strcmp(drawn.text[i].s, "猫") && drawn.text[i].y == 210)
                word = &drawn.text[i];
        TEST_CHECK(before && word && after && before->color == WHITE && word->color == ACCENT && after->color == WHITE);
        TEST_CHECK(before && word && after && word->x == before->x + width_of("白い", 24) &&
                   after->x == word->x + width_of("猫", 24));
    }
    TEST_CHECK(text_has("a meaning") && text_has("a second meaning"));
    TEST_CHECK(textures_live == 1 && texture_draws > 0 && texture_x == 508 && texture_y == 120 && texture_sx == 1.0f &&
               vjo_test_picture_decodes == 1);
    TEST_CHECK(badge_named("Delete") && badge_named("Fix sentence") && badge_named("Fix sentence")->active &&
               badge_named("Drop picture") && !badge_named("Drop picture")->active);
    TEST_CHECK(!strcmp(drawn.controls, "L/R: card   Left/Right: action   ×: do it   ○: back"));
    frame(SCE_CTRL_RIGHT);
    TEST_CHECK(badge_named("Drop picture")->active);
    frame(SCE_CTRL_RIGHT);
    TEST_CHECK(badge_named("Delete")->active);
    /* R: the card after it (older), which has no picture: its texture goes, none comes */
    frame(SCE_CTRL_RTRIGGER);
    run_worker();
    frame(0);
    TEST_CHECK(!strcmp(sentence_drawn(), "犬だ") && textures_live == 0 && text_has("No picture") && !badge_named("Drop picture") &&
               badge_named("Delete") && badge_named("Fix sentence"));
    frame(SCE_CTRL_RTRIGGER); /* the last: nothing */
    TEST_CHECK(!vjo_anki_page_busy());
    frame(SCE_CTRL_LTRIGGER);
    run_worker();
    frame(0);
    TEST_CHECK(!strcmp(sentence_drawn(), "白い猫が好き") && textures_live == 1);
    frame(SCE_CTRL_CIRCLE);
    TEST_CHECK(list_rows() == 2 && textures_live == 0 && vjo_cards_page_is_open());
}

/* Delete and Drop picture take a second press (any other button between them asks again); Delete moves the
 * card to deleted/ and the list goes on without it; Drop picture makes the new card, which opens. */
static void delete_and_drop_picture_ask_for_a_second_press(void)
{
    char a[VJO_QUEUE_ID_SIZE], c[VJO_QUEUE_ID_SIZE];
    int threads;
    reset(VJO_BUTTON_ASSIGN_CROSS);
    snprintf(a, sizeof(a), "%s", queue_full("猫", "猫が好き", 0, 3, 1, 1791790300));
    queue_full("犬", "犬だ", 0, 3, 0, 1791790200);
    snprintf(c, sizeof(c), "%s", queue_full("鳥", "鳥だ", 0, 3, 1, 1791790100));
    show();
    frame(0);
    open_cards();
    frame(SCE_CTRL_CROSS);
    run_worker();
    frame(0);
    frame(SCE_CTRL_LEFT); /* Fix sentence -> Delete */
    TEST_CHECK(badge_named("Delete")->active);
    threads = threads_created;
    frame(SCE_CTRL_CROSS);
    TEST_CHECK(strstr(drawn.footer, "Press again to delete this card") && threads_created == threads);
    frame(SCE_CTRL_RIGHT); /* another button: asked again */
    frame(SCE_CTRL_LEFT);
    frame(SCE_CTRL_CROSS);
    TEST_CHECK(strstr(drawn.footer, "Press again to delete this card") && threads_created == threads);
    frame(SCE_CTRL_CROSS);
    TEST_ASSERT(threads_created == threads + 1);
    run_jobs();
    TEST_CHECK(list_rows() == 2 && !strcmp(row_word(0), "犬") && strstr(drawn.footer, "Deleted 猫."));
    TEST_MSG("%s", drawn.footer);
    TEST_CHECK(!in_dir(VJO_QUEUE_DIR, a, "json") && in_dir(VJO_CARDS_DELETED_DIR, a, "json") &&
               in_dir(VJO_CARDS_DELETED_DIR, a, "jpg"));
    TEST_CHECK(text_has("empty deleted (1 card)") || strstr(drawn.footer, "Deleted"));
    /* Drop picture of 鳥 */
    frame(SCE_CTRL_DOWN);
    frame(SCE_CTRL_CROSS);
    run_worker();
    frame(0);
    TEST_CHECK(!strcmp(sentence_drawn(), "鳥だ") && textures_live == 1);
    frame(SCE_CTRL_RIGHT); /* Fix sentence -> Drop picture */
    TEST_CHECK(badge_named("Drop picture")->active);
    frame(SCE_CTRL_CROSS);
    TEST_CHECK(strstr(drawn.footer, "Press again to drop the picture"));
    frame(SCE_CTRL_CROSS);
    run_jobs();
    TEST_CHECK(strstr(drawn.footer, "Dropped the picture of 鳥") && !strcmp(sentence_drawn(), "鳥だ") && text_has("No picture") &&
               !badge_named("Drop picture") && textures_live == 0);
    TEST_MSG("%s", drawn.footer);
    TEST_CHECK(!in_dir(VJO_QUEUE_DIR, c, "json") && in_dir(VJO_CARDS_DELETED_DIR, c, "jpg") &&
               files_in(VJO_QUEUE_DIR, ".json") == 2 && files_in(VJO_QUEUE_DIR, ".jpg") == 0);
}

static void utf16(uint16_t *out, const char *s)
{
    size_t n = strlen(s), at = 0;
    int k = 0;
    while (at < n) {
        unsigned char c = (unsigned char)s[at];
        uint32_t cp = c < 0x80 ? c : c < 0xE0 ? c & 0x1F : c < 0xF0 ? c & 0x0F : c & 0x07;
        int more = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
        at++;
        while (more--)
            cp = cp << 6 | ((unsigned char)s[at++] & 0x3F);
        out[k++] = (uint16_t)cp;
    }
    out[k] = 0;
}

/* Fix sentence opens the keyboard (line breaks allowed, 2,048 characters, as VitaShell's text editor opens it:
 * a cancel button, the default text box) with the sentence; its done button (CLOSE in a multi-line keyboard:
 * Enter types a line break there) saves the fixed card on the worker, which opens with the word found again;
 * its cancel (NONE), or the same text, changes nothing; a sentence without the word says it keeps no bold. The
 * keyboard's frame tells main.c (its closing press is no new one). */
static void fix_sentence_through_the_keyboard(void)
{
    uint16_t want[65];
    char old[VJO_QUEUE_ID_SIZE];
    int threads;
    reset(VJO_BUTTON_ASSIGN_CROSS);
    snprintf(old, sizeof(old), "%s", queue_full("猫", "猫が好き", 0, 3, 1, 1791790200));
    show();
    frame(0);
    open_cards();
    frame(SCE_CTRL_CROSS);
    run_worker();
    frame(0);
    TEST_ASSERT(badge_named("Fix sentence") && badge_named("Fix sentence")->active);
    /* cancelled in the keyboard (Enter there is a line break, never the end) */
    for (int k = 0; k < 2; k++) {
        vjo_test_ime_button = k ? SCE_IME_DIALOG_BUTTON_ENTER : SCE_IME_DIALOG_BUTTON_NONE;
        utf16(vjo_test_ime_reply, "白い猫");
        threads = threads_created;
        frame(SCE_CTRL_CROSS);
        TEST_CHECK(dialog_ran && vjo_test_ime_calls == k + 1 && vjo_test_ime_option == SCE_IME_OPTION_MULTILINE &&
                   vjo_test_ime_max == 2048 && vjo_test_ime_languages == 0x1FFFF && vjo_test_ime_forced &&
                   vjo_test_ime_textbox == SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT &&
                   vjo_test_ime_mode == SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL && threads_created == threads);
    }
    utf16(want, "猫が好き");
    TEST_CHECK(!memcmp(vjo_test_ime_initial, want, sizeof(uint16_t) * 5));
    frame(0);
    TEST_CHECK(strstr(drawn.footer, "Cancelled: nothing changed.") && files_in(VJO_QUEUE_DIR, ".json") == 1);
    /* the same text */
    vjo_test_ime_button = SCE_IME_DIALOG_BUTTON_CLOSE;
    utf16(vjo_test_ime_reply, "猫が好き");
    frame(SCE_CTRL_CROSS);
    frame(0);
    TEST_CHECK(strstr(drawn.footer, "Nothing to change") && threads_created == threads);
    /* fixed */
    utf16(vjo_test_ime_reply, "白い猫が好きだ");
    frame(SCE_CTRL_CROSS);
    TEST_CHECK(dialog_ran && threads_created == threads + 1);
    run_jobs();
    TEST_CHECK(strstr(drawn.footer, "Fixed the sentence of 猫") && !strcmp(sentence_drawn(), "白い猫が好きだ") &&
               textures_live == 1);
    TEST_MSG("%s", drawn.footer);
    TEST_CHECK(!in_dir(VJO_QUEUE_DIR, old, "json") && in_dir(VJO_CARDS_DELETED_DIR, old, "json") &&
               files_in(VJO_QUEUE_DIR, ".json") == 1 && files_in(VJO_QUEUE_DIR, ".jpg") == 1);
    {
        const Text *word = NULL;
        for (int i = 0; i < drawn.texts; i++)
            if (!strcmp(drawn.text[i].s, "猫") && drawn.text[i].y == 210)
                word = &drawn.text[i];
        TEST_CHECK(word && word->color == ACCENT);
    }
    /* the word gone */
    utf16(vjo_test_ime_reply, "犬が好きだ");
    frame(SCE_CTRL_CROSS);
    run_jobs();
    TEST_CHECK(strstr(drawn.footer, "The word is not in it now, so it has no bold."));
    TEST_MSG("%s", drawn.footer);
    for (int i = 0; i < drawn.texts; i++)
        TEST_CHECK_(drawn.text[i].color != ACCENT || drawn.text[i].y != 210, "'%s' still marked", drawn.text[i].s);
    /* the keyboard that cannot open */
    vjo_test_ime_init_rc = (int)0x80102001u;
    frame(SCE_CTRL_CROSS);
    frame(0);
    TEST_CHECK(strstr(drawn.footer, "Could not open the keyboard (0x80102001). Nothing changed."));
    vjo_test_ime_init_rc = 0;
}

/* The exported cards: a list of their own (Left/Right), read only: a card opens with no action. The queue's
 * list says when it is empty. */
static void exported_cards_are_read_only(void)
{
    int threads;
    reset(VJO_BUTTON_ASSIGN_CROSS);
    queue_full("猫", "猫が好き", 0, 3, 1, 1791790200);
    show();
    for (int k = 0; k < EXPORT; k++)
        frame(SCE_CTRL_DOWN);
    frame(SCE_CTRL_CROSS); /* Export cards */
    run_worker();
    for (int k = CARDS; k < EXPORT; k++)
        frame(SCE_CTRL_UP); /* the Cards row (Known words between) */
    TEST_ASSERT(focused() == CARDS);
    frame(SCE_CTRL_CROSS);
    run_worker();
    frame(0);
    TEST_CHECK(list_rows() == 0 && text_has("No cards queued."));
    frame(SCE_CTRL_RIGHT);
    run_worker();
    frame(0);
    TEST_CHECK(list_rows() == 1 && badge_named("Exported (1)") && badge_named("Exported (1)")->active &&
               strstr(drawn.footer, "Exported cards are read only"));
    frame(SCE_CTRL_CROSS);
    run_worker();
    frame(0);
    TEST_CHECK(!strcmp(sentence_drawn(), "猫が好き") && !badge_named("Delete") && !badge_named("Fix sentence") &&
               text_has("Exported: read only.") && !strcmp(drawn.controls, "L/R: card   ○: back") && textures_live == 1);
    threads = threads_created;
    frame(SCE_CTRL_CROSS);
    frame(SCE_CTRL_LEFT);
    frame(SCE_CTRL_RIGHT);
    TEST_CHECK(threads_created == threads && !vjo_anki_page_busy() && vjo_test_ime_calls == 0);
    frame(SCE_CTRL_CIRCLE);
    frame(SCE_CTRL_LEFT);
    run_worker();
    frame(0);
    TEST_CHECK(badge_named("Queued (0)") && badge_named("Queued (0)")->active);
}

/* Square shows only the cards whose word another card has (marked in the list); Triangle, pressed twice, empties
 * deleted/ on the worker. */
static void the_duplicates_filter_and_empty_deleted(void)
{
    reset(VJO_BUTTON_ASSIGN_CROSS);
    queue_full("猫", "猫が好き", 0, 3, 0, 1791790100);
    queue_full("猫", "猫だ", 0, 3, 0, 1791790200);
    queue_full("犬", "犬だ", 0, 3, 0, 1791790300);
    show();
    frame(0);
    open_cards();
    TEST_CHECK(list_rows() == 3 && text_has("Same word as another card"));
    frame(SCE_CTRL_SQUARE);
    TEST_CHECK(list_rows() == 2 && badge_named("Duplicates")->active && !strcmp(row_word(0), "猫") &&
               !strcmp(row_word(1), "猫"));
    frame(SCE_CTRL_SQUARE);
    TEST_CHECK(list_rows() == 3);
    frame(SCE_CTRL_TRIANGLE);
    TEST_CHECK(strstr(drawn.footer, "anki_queue/deleted/ is empty."));
    /* 犬 deleted, then deleted/ emptied */
    frame(SCE_CTRL_CROSS);
    run_worker();
    frame(0);
    frame(SCE_CTRL_LEFT);
    frame(SCE_CTRL_CROSS);
    frame(SCE_CTRL_CROSS);
    run_jobs();
    TEST_CHECK(list_rows() == 2 && files_in(VJO_CARDS_DELETED_DIR, ".json") == 1);
    frame(SCE_CTRL_TRIANGLE);
    TEST_CHECK(strstr(drawn.footer, "Press Triangle again to remove the 1 deleted card for good.") && !vjo_anki_page_busy());
    frame(SCE_CTRL_TRIANGLE);
    run_jobs();
    TEST_CHECK(strstr(drawn.footer, "Removed 1 file from anki_queue/deleted/ for good.") &&
               files_in(VJO_CARDS_DELETED_DIR, ".json") == 0);
    TEST_MSG("%s", drawn.footer);
}

/* Six cards a page: L/R jump a page as on the Games page (at the last page R goes to the last card); Down
 * from a page's last card shows the next page. */
static void the_list_has_pages_of_six(void)
{
    reset(VJO_BUTTON_ASSIGN_CROSS);
    for (int i = 0; i < 13; i++) {
        char word[16];
        snprintf(word, sizeof(word), "語%02d", i);
        queue_full(word, word, 0, 5, 0, 1791790000 + i * 60);
    }
    show();
    frame(0);
    open_cards();
    TEST_CHECK(list_rows() == 6 && !strcmp(row_word(0), "語12") && text_has("1-6 of 13"));
    frame(SCE_CTRL_RTRIGGER);
    TEST_CHECK(list_rows() == 6 && list_focused() == 0 && !strcmp(row_word(0), "語06") && text_has("7-12 of 13"));
    frame(SCE_CTRL_RTRIGGER);
    TEST_CHECK(list_rows() == 1 && text_has("13-13 of 13"));
    frame(SCE_CTRL_LTRIGGER);
    frame(SCE_CTRL_LTRIGGER);
    TEST_CHECK(list_focused() == 0 && !strcmp(row_word(0), "語12"));
    for (int k = 0; k < 6; k++)
        frame(SCE_CTRL_DOWN);
    TEST_CHECK(list_focused() == 0 && !strcmp(row_word(0), "語06"));
}

/* Cancel while the page first reads the queue goes back to the Anki page; while it reads it again (Start, or the
 * Anki page shown again) it stops there and says so. Start keeps the card selected; a card deleted by another
 * way meanwhile (VitaShell) is gone from the list read again. */
static void cancel_and_start_while_the_list_is_read(void)
{
    char gone[VJO_QUEUE_ID_SIZE], path[256];
    reset(VJO_BUTTON_ASSIGN_CROSS);
    queue_full("猫", "猫だ", 0, 3, 0, 1791790300);
    snprintf(gone, sizeof(gone), "%s", queue_full("犬", "犬だ", 0, 3, 0, 1791790200));
    queue_full("鳥", "鳥だ", 0, 3, 0, 1791790100);
    show();
    frame(0);
    for (int k = 0; k < CARDS; k++)
        frame(SCE_CTRL_DOWN);
    frame(SCE_CTRL_CROSS);
    TEST_ASSERT(vjo_cards_page_is_open() && vjo_anki_page_busy());
    frame(SCE_CTRL_CIRCLE); /* the first read stopped: back */
    run_worker();
    frame(0);
    TEST_CHECK(!vjo_cards_page_is_open() && !vjo_anki_page_busy() && focused() == CARDS);
    frame(SCE_CTRL_CROSS);
    run_worker();
    frame(0);
    frame(SCE_CTRL_DOWN);
    frame(SCE_CTRL_DOWN);
    TEST_CHECK(list_focused() == 2 && !strcmp(row_word(2), "鳥"));
    /* removed by VitaShell meanwhile; Start reads again, 鳥 still selected */
    snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s.json", gone);
    TEST_ASSERT(unlink(path) == 0);
    frame(SCE_CTRL_START);
    TEST_CHECK(vjo_anki_page_busy());
    run_worker();
    frame(0);
    TEST_CHECK(list_rows() == 2 && list_focused() == 1 && !strcmp(row_word(1), "鳥") && badge_named("Queued (2)"));
    /* the Anki page shown again (Select back to it): read again too; Cancel then stops it, and the page stays */
    show();
    TEST_CHECK(vjo_anki_page_busy());
    frame(SCE_CTRL_CIRCLE);
    run_worker();
    frame(0);
    TEST_CHECK(vjo_cards_page_is_open() && !vjo_anki_page_busy() && text_has("Stopped. Start reads the list again."));
    frame(SCE_CTRL_START);
    run_worker();
    frame(0);
    TEST_CHECK(list_rows() == 2 && vjo_cards_page_is_open());
}

/* A picture that is missing or damaged, one the decoder refuses, or no memory for it: the card opens with its
 * text and says why it has no picture. */
static void a_picture_that_cannot_be_shown_is_said(void)
{
    char path[256];
    reset(VJO_BUTTON_ASSIGN_CROSS);
    snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s.jpg", queue_full("猫", "猫が好き", 0, 3, 1, 1791790200));
    show();
    frame(0);
    open_cards();
    {
        FILE *f = fopen(path, "wb");
        TEST_ASSERT(f && fwrite("garbage", 1, 7, f) == 7);
        fclose(f);
    }
    frame(SCE_CTRL_CROSS);
    run_worker();
    frame(0);
    TEST_CHECK(!strcmp(sentence_drawn(), "猫が好き") && text_has("The picture is missing or damaged") && textures_live == 0);
    frame(SCE_CTRL_CIRCLE);
    {
        static const unsigned char jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 1, 0xFF, 0xD9};
        FILE *f = fopen(path, "wb");
        TEST_ASSERT(f && fwrite(jpeg, 1, sizeof(jpeg), f) == sizeof(jpeg));
        fclose(f);
    }
    vjo_test_picture_fail = 2;
    frame(SCE_CTRL_CROSS);
    run_worker();
    frame(0);
    TEST_CHECK(text_has("The picture cannot be shown") && textures_live == 0);
    frame(SCE_CTRL_CIRCLE);
    vjo_test_picture_fail = 1;
    frame(SCE_CTRL_CROSS);
    run_worker();
    frame(0);
    TEST_CHECK(text_has("Not enough memory to show it") && textures_live == 0);
    vjo_test_picture_fail = 0;
    /* leaving the app with the page open: its texture goes */
    frame(SCE_CTRL_CIRCLE);
    frame(SCE_CTRL_CROSS);
    run_worker();
    frame(0);
    TEST_CHECK(textures_live == 1);
    vjo_anki_page_quit();
    TEST_CHECK(textures_live == 0);
}

TEST_LIST = {
    {"the_rows_and_the_footer_do_not_overlap", the_rows_and_the_footer_do_not_overlap},
    {"up_down_and_the_stick_choose_a_row", up_down_and_the_stick_choose_a_row},
    {"enter_and_left_right_save_each_row_s_key", enter_and_left_right_save_each_row_s_key},
    {"touch_chooses_and_acts_on_each_row", touch_chooses_and_acts_on_each_row},
    {"an_export_runs_on_the_worker_and_the_page_follows_it", an_export_runs_on_the_worker_and_the_page_follows_it},
    {"a_missing_screenshot_is_reported_after_export", a_missing_screenshot_is_reported_after_export},
    {"failures_are_shown_and_change_nothing", failures_are_shown_and_change_nothing},
    {"the_duplicates_row_saves_app_ini", the_duplicates_row_saves_app_ini},
    {"an_unreadable_app_ini_is_never_written", an_unreadable_app_ini_is_never_written},
    {"the_export_row_counts_words_mined_before", the_export_row_counts_words_mined_before},
    {"the_known_words_row_imports_the_file", the_known_words_row_imports_the_file},
    {"a_busy_page_changes_no_choice", a_busy_page_changes_no_choice},
    {"unchecked_exports_are_errors", unchecked_exports_are_errors},
    {"the_cards_row_opens_the_queue_newest_first", the_cards_row_opens_the_queue_newest_first},
    {"a_card_shows_its_sentence_definition_and_picture", a_card_shows_its_sentence_definition_and_picture},
    {"delete_and_drop_picture_ask_for_a_second_press", delete_and_drop_picture_ask_for_a_second_press},
    {"fix_sentence_through_the_keyboard", fix_sentence_through_the_keyboard},
    {"exported_cards_are_read_only", exported_cards_are_read_only},
    {"the_duplicates_filter_and_empty_deleted", the_duplicates_filter_and_empty_deleted},
    {"the_list_has_pages_of_six", the_list_has_pages_of_six},
    {"cancel_and_start_while_the_list_is_read", cancel_and_start_while_the_list_is_read},
    {"a_picture_that_cannot_be_shown_is_said", a_picture_that_cannot_be_shown_is_said},
    {NULL, NULL},
};

int main(int argc, char **argv)
{
    /* the memory card, removed when the suite ends (acutest runs each test in a child: test_tmpdir.h) */
    const char *dir = vjo_test_tmpdir("settings-anki");
    if (!dir || chdir(dir) < 0)
        return 2;
    return acutest_main(argc, argv);
}
