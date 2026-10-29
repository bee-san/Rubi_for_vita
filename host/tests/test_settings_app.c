/* The Rubi app's OCR card, its Model size row (API 21), through its
 * actual input and draw loop (settings/main.c), on the host. The SDK boundary (vita2d, controller, touch
 * screen, display) is scripted and records what each frame draws; config.ini,
 * games.ini and the model files are real files in a temporary directory, read
 * and written by the app's own settings/preferences.c, settings/model_size.c and
 * vita/games_storage.c. Text widths come from stub metrics (Latin 10, other
 * characters 18 units per 18-unit em), not the firmware's PVF fonts.
 *
 * Built twice: with this build's model metadata (core/vocr_models.c), and with
 * VJO_VOCR_MODEL_INFO_TEST naming a later package's table, where generation
 * VOCR6 is pinned for two sizes and VOCR5 kept for the third: the app then names
 * each size's model unchanged. */
#define main acutest_main
#include "acutest.h"
#undef main
#include "test_tmpdir.h"
#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/registrymgr.h>
#include <psp2/touch.h>
#include <vita2d.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../../core/vocr_models.h"
#include "../../core/utf.h"
#include "../../core/config.h" /* VJO_VERTICAL_* (API 47) */
#include "../../include/vjo_vocr_sizes.h"
#include "../../core/app_stamp.h"
#include "../../include/vjo_api.h"
#include "../../settings/app_check.h"
#include "../../settings/app_prefs.h"
#include "../../settings/catalog.h"
#include "../../settings/model_size.h"
#include "../../settings/preferences.h"

int vjo_settings_main(void); /* settings/main.c's main(), renamed for the harness */

/* settings/main.c's layout: the OCR card, the local engine's three Model size
 * buttons (VOCR is the only engine: the overlay is offline), then the card's
 * status line; below it the Read text card (ocr_mode), its two choices, and the
 * Speed boost card (ocr_boost_clock); then the three lookup cards, scrolled
 * into the room above the status line a card at a time. */
#define OCR_Y 112
#define OCR_H 94
#define SIZE_X 232
#define SIZE_Y (OCR_Y + 6)
#define SIZE_H 54
#define SIZE_W 220
#define SIZE_PITCH 233
#define SIZE_NAME_Y (SIZE_Y + 24)
#define SIZE_DETAIL_Y (SIZE_Y + 47)
#define STATUS_Y (OCR_Y + 85)
#define READ_Y (OCR_Y + OCR_H + 4) /* the card after the OCR card */
#define READ_H 102
#define READ_BUTTON_Y (READ_Y + 42)
#define READ_BUTTON_W 433
#define READ_BUTTON_H 54
#define READ_BUTTON_X(k) (42 + (k) * 445)
#define BOOST_Y (READ_Y + READ_H + 4)
#define CARD_H 68
#define ROOM_END 422 /* the cards' room ends above the status line */
#define ACCENT RGBA8(255, 210, 74, 255)
#define SELECTED RGBA8(43, 47, 49, 255)
#define CARD RGBA8(25, 27, 29, 255)
#define WHITE RGBA8(240, 242, 244, 255)
#define BG_COLOR RGBA8(15, 16, 17, 255)
#define DIM RGBA8(184, 187, 191, 255)
#define OFF RGBA8(244, 160, 167, 255)
#define MODEL_DIR "ux0:data/VitaJPOverlay/ocr"

/* ---- storage: the SDK's file calls over real files (fakes/sceio_posix.h) ---- */
#define SCEIO_OWN (SCEIO_OPEN | SCEIO_CLOSE | SCEIO_RENAME)
#include "fakes/sceio_posix.h"
static const char *close_fails; /* a file whose sceIoClose reports an error */
static int close_fails_fd = -1;
static int app_rename_fails;
int sceIoOpen(const char *p, int flags, unsigned mode)
{
    int fd = sceio_open(p, flags, mode);
    size_t n = strlen(p), m = close_fails ? strlen(close_fails) : 0;
    if (fd >= 0 && m && n >= m && !strcmp(p + n - m, close_fails)) close_fails_fd = fd;
    return fd;
}
int sceIoClose(int fd)
{
    int rc = sceio_close(fd);
    if (fd != close_fails_fd) return rc;
    close_fails_fd = -1;
    return (int)0x80010005u; /* EIO */
}
int sceIoRename(const char *from, const char *to)
{
    if (app_rename_fails && !strcmp(from, VJO_APP_PREFS_PATH ".tmp")) return -1;
    return sceio_rename(from, to);
}
SceUID sceKernelGetThreadId(void) { return 0x40010003; } /* settings/applog.c's batch owner */

/* No installed games: the Games page is not what these tests drive. */
int vjo_catalog_scan(VjoInstalledGame *out, int capacity, int *truncated, const VjoGames *rules)
{ (void)out; (void)capacity; (void)rules; *truncated = 0; return 0; }
/* No PSP game to register either (settings/psp.c, test_settings_games.c). */
#include "../../settings/psp.h"
int vjo_psp_register(VjoInstalledGame *g, char *msg, size_t cap) { (void)g; snprintf(msg, cap, "-"); return -1; }

/* ---- the scripted controller, touch screen and display ---- */
typedef struct { unsigned buttons; int touch, x, y; } Input; /* x, y in screen pixels */
#define MAX_FRAMES 64
/* Each text and rectangle with the clipping it was drawn under: clipped, and
 * the clip rectangle's top and bottom (vita2d_set_clip_rectangle's y_min, y_max). */
typedef struct { int x, y; unsigned color; float scale; char text[128]; int clipped, clip_top, clip_bottom, clip_left, clip_right; } Text;
typedef struct { int x, y, w, h; unsigned color; int clipped, clip_top, clip_bottom, clip_left, clip_right; } Rect;
typedef struct {
    unsigned clear;
    Text text[160]; int texts;
    Rect rect[512]; int rects;
    char backend[16]; /* config.ini's ocr_backend value after this frame ("" without the line) */
    char tier[16];    /* its vocr_tier value */
    char mode[16];    /* its ocr_mode value */
    char boost[16];   /* its ocr_boost_clock value */
    char highlight[16]; /* its highlight_mode value */
    char japanese[16]; /* its japanese_only_selection value */
    char source[16];    /* API 31: its text_source value */
    char gate[16];      /* API 43: its ocr_text_gate value */
    char trust[16];     /* API 43: its hook_trust value */
    char vertical[16];  /* API 47: its ocr_vertical value */
    char pause[16];     /* API 55: its ocr_pause value */
    int model_line;   /* it still has an older build's vocr_model or vocr_fallback_model line */
    char region[96];  /* #358: region.ini's text after this frame ("-" without the file) */
} Frame;
/* Each run is one process of the app, as on the console: settings/main.c keeps
 * its state in statics, so it runs in a forked child, and the frames it shows
 * come back through shared memory. */
typedef struct { int frames, exited, main_rc, finis, font_loads, font_frees, font_free_in_frame, font_free_unwaited,
                 dead_font_uses, most_glyphs, fonts_both, most_fonts;
                 int anki_shows, anki_frames, install_shows, install_frames; uint32_t anki_pressed, install_pressed;
                 int lookup_shows, lookup_leaves, lookup_frames; uint32_t lookup_pressed[16], lookup_held[16];
                 Frame shown[MAX_FRAMES]; } Session;
/* settings_stubs/lookup_page_stub.c's records and controls (#313: main.c's Look up wiring) */
extern int vjo_lookup_stub_shows, vjo_lookup_stub_leaves, vjo_lookup_stub_frames, vjo_lookup_stub_keyboard_at,
           vjo_lookup_stub_hold_cancel_to;
extern uint32_t vjo_lookup_stub_pressed[16], vjo_lookup_stub_held[16];
extern int vjo_install_stub_open, vjo_install_stub_busy_to, vjo_install_stub_frames, vjo_install_stub_shows;
extern uint32_t vjo_install_stub_pressed;
extern int vjo_anki_stub_frames, vjo_anki_stub_shows, vjo_anki_stub_hold_cancel_to;
extern uint32_t vjo_anki_stub_pressed;
static Session *session;
static Frame *shown;
static const Input *script;
static int script_frames, frame, exited = -1;
static int finis;      /* vita2d_fini calls in this process */
static int font_fails; /* vita2d_load_default_pvf returns NULL */
static Frame drawing;

uint64_t sceKernelGetProcessTimeWide(void) { return (uint64_t)frame * 16667u; }
int sceKernelExitProcess(int status) { exited = status; return 0; }
/* settings/sysapp.c's heap and a system application's budget (main.c logs both at start). */
int _newlib_heap_size_user = 8 * 1024 * 1024;
int sceAppMgrGetBudgetInfo(SceAppMgrBudgetInfo *info)
{
    if (info->size != (int)sizeof(*info)) return (int)0x80800001;
    info->total_user_rw_mem = 50u << 20;
    info->free_user_rw = 30u << 20;
    return 0;
}
/* /CONFIG/SYSTEM/button_assign: 1, × enters (the scripts press × and leave with ○). */
int sceRegMgrGetKeyInt(const char *category, const char *name, int *buf)
{
    (void)category; (void)name;
    *buf = 1;
    return 0;
}
int sceCtrlSetSamplingMode(int mode) { (void)mode; return 0; }
int sceTouchSetSamplingState(int port, int state) { (void)port; (void)state; return 0; }
int sceCtrlPeekBufferPositive(int port, SceCtrlData *pad, int count)
{
    (void)port; (void)count;
    pad->buttons = frame < script_frames && frame < MAX_FRAMES - 1 ? script[frame].buttons : (frame - script_frames) % 2 ? 0 : SCE_CTRL_CIRCLE;
    pad->lx = pad->ly = pad->rx = pad->ry = 128; /* the sticks at rest */
    return 1;
}
int sceTouchPeek(int port, SceTouchData *data, int count)
{
    (void)port; (void)count;
    if (frame < script_frames && script[frame].touch) {
        data->reportNum = 1;
        data->report[0].x = (uint16_t)(script[frame].x * 2);
        data->report[0].y = (uint16_t)(script[frame].y * 2);
    }
    return 1;
}

/* config.ini's value of key (its first line), or "" */
static void saved_value(const char *key, char out[16])
{
    char text[8192], line_key[48];
    out[0] = 0;
    FILE *f = fopen(VJO_PREFERENCES_PATH, "rb");
    if (!f) return;
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    snprintf(line_key, sizeof(line_key), "%s = ", key);
    const char *value = !strncmp(text, line_key, strlen(line_key)) ? text + strlen(line_key) : NULL;
    snprintf(line_key, sizeof(line_key), "\n%s = ", key);
    const char *line = value ? NULL : strstr(text, line_key);
    if (line) value = line + strlen(line_key);
    if (value) sscanf(value, "%15[a-z0-9_]", out);
}
static void saved_backend(char out[16]) { saved_value("ocr_backend", out); }
static int has_model_line(void)
{
    char text[8192];
    FILE *f = fopen(VJO_PREFERENCES_PATH, "rb");
    if (!f) return 0;
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    return strstr(text, "vocr_model") != NULL || strstr(text, "vocr_fallback_model") != NULL;
}
int sceDisplayWaitVblankStart(void)
{
    if (frame < MAX_FRAMES) {
        shown[frame] = drawing;
        saved_backend(shown[frame].backend);
        saved_value("vocr_tier", shown[frame].tier);
        saved_value("ocr_mode", shown[frame].mode);
        saved_value("ocr_boost_clock", shown[frame].boost);
        saved_value("highlight_mode", shown[frame].highlight);
        saved_value("japanese_only_selection", shown[frame].japanese);
        saved_value("text_source", shown[frame].source);
        saved_value("ocr_text_gate", shown[frame].gate);
        saved_value("hook_trust", shown[frame].trust);
        saved_value("ocr_vertical", shown[frame].vertical);
        saved_value("ocr_pause", shown[frame].pause);
        shown[frame].model_line = has_model_line();
        {
            FILE *f = fopen(VJO_PREFERENCES_REGION_PATH, "rb");
            size_t n = f ? fread(shown[frame].region, 1, sizeof(shown[frame].region) - 1, f) : 0;
            if (f) fclose(f);
            shown[frame].region[n] = 0;
            if (!f) snprintf(shown[frame].region, sizeof(shown[frame].region), "-");
        }
    }
    frame++;
    return 0;
}

int vita2d_init(void) { return 0; }
int vita2d_fini(void) { finis++; return 0; }
static unsigned clear_colour;
void vita2d_set_clear_color(unsigned int color) { clear_colour = color; }
/* The fonts: each load a font of its own, alive until freed; a font that drew is freed only between frames, after a
 * wait for the GPU (settings/main.c font_tick: one that only measured, whose atlas the GPU never reads, at any time),
 * and none is drawn or measured with once freed; the most alive at once (their atlases: 256 KiB each). */
#define FONTS 96
static int font_live[FONTS], font_loads, font_frees, font_free_in_frame, font_free_unwaited, dead_font_uses,
           in_frame, waited, fonts_alive, most_fonts;
/* Each font's distinct characters, as libvita2d's atlas would hold them (its glyph table), the most any font had,
 * and the fonts that drew and those that measured. */
#define FONT_GLYPHS 4096
static uint32_t font_glyph[FONTS][FONT_GLYPHS];
static int font_glyphs[FONTS], font_drew[FONTS], font_measured[FONTS], most_glyphs;
void vita2d_start_drawing(void) { memset(&drawing, 0, sizeof(drawing)); in_frame = 1; waited = 0; }
void vita2d_end_drawing(void) { in_frame = 0; }
static void font_use(vita2d_pvf *font)
{
    int i = (int)((int *)font - font_live);
    if (i < 0 || i >= FONTS || !font_live[i]) dead_font_uses++;
}
static void font_holds(vita2d_pvf *font, const char *text, int drawn)
{
    int f = (int)((int *)font - font_live);
    size_t n = strlen(text), i = 0;
    if (f < 0 || f >= FONTS) return;
    (drawn ? font_drew : font_measured)[f] = 1;
    while (i < n) {
        uint32_t cp = vjo_utf8_next(text, n, &i);
        int k = 0;
        if (cp == '\n') continue; /* (libvita2d never adds it) */
        while (k < font_glyphs[f] && font_glyph[f][k] != cp) k++;
        if (k == font_glyphs[f] && k < FONT_GLYPHS) font_glyph[f][font_glyphs[f]++] = cp;
        if (font_glyphs[f] > most_glyphs) most_glyphs = font_glyphs[f];
    }
}
void vita2d_swap_buffers(void) {}
void vita2d_clear_screen(void) { drawing.clear = clear_colour; }
void vita2d_wait_rendering_done(void) { waited = 1; }
/* libvita2d keeps the last clip rectangle and applies it while clipping is on. */
static int clipping, clip_top, clip_bottom, clip_left, clip_right;
void vita2d_enable_clipping(void) { clipping = 1; }
void vita2d_disable_clipping(void) { clipping = 0; }
void vita2d_set_clip_rectangle(int x_min, int y_min, int x_max, int y_max)
{ clip_left = x_min; clip_right = x_max; clip_top = y_min; clip_bottom = y_max; }
void vita2d_draw_rectangle(float x, float y, float w, float h, unsigned int color)
{
    if (drawing.rects < (int)(sizeof(drawing.rect) / sizeof(drawing.rect[0])))
        drawing.rect[drawing.rects++] = (Rect){(int)x, (int)y, (int)w, (int)h, color, clipping, clip_top, clip_bottom, clip_left, clip_right};
}
vita2d_pvf *vita2d_load_default_pvf(void)
{
    if (font_fails || font_loads >= FONTS) return NULL;
    font_live[font_loads] = 1;
    if (++fonts_alive > most_fonts) most_fonts = fonts_alive;
    return (vita2d_pvf *)&font_live[font_loads++];
}
void vita2d_free_pvf(vita2d_pvf *font)
{
    int f = (int)((int *)font - font_live);
    font_use(font);
    font_live[f] = 0;
    fonts_alive--;
    font_frees++;
    font_free_in_frame += in_frame && font_drew[f];
    font_free_unwaited += !waited && font_drew[f];
}
/* Stub metrics: 10 units for an ASCII character, 18 for any other, at scale 1. */
int vita2d_pvf_text_width(vita2d_pvf *font, float scale, const char *text)
{
    font_use(font);
    font_holds(font, text, 0);
    int units = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if (*p < 0x80) units += 10;
        else if ((*p & 0xC0) != 0x80) units += 18;
    return (int)(units * scale + 0.5f);
}
int vita2d_pvf_draw_text(vita2d_pvf *font, int x, int y, unsigned int color, float scale, const char *text)
{
    font_use(font);
    font_holds(font, text, 1);
    if (drawing.texts < (int)(sizeof(drawing.text) / sizeof(drawing.text[0]))) {
        Text *t = &drawing.text[drawing.texts++];
        t->x = x; t->y = y; t->color = color; t->scale = scale;
        t->clipped = clipping; t->clip_top = clip_top; t->clip_bottom = clip_bottom;
        t->clip_left = clip_left; t->clip_right = clip_right;
        snprintf(t->text, sizeof(t->text), "%s", text);
    }
    return 0;
}
vita2d_texture *vita2d_load_PNG_file(const char *filename) { (void)filename; return NULL; }
vita2d_texture *vita2d_load_PNG_buffer(const void *buffer) { (void)buffer; return NULL; }
void vita2d_free_texture(vita2d_texture *texture) { (void)texture; }
unsigned int vita2d_texture_get_width(const vita2d_texture *texture) { (void)texture; return 64; }
unsigned int vita2d_texture_get_height(const vita2d_texture *texture) { (void)texture; return 64; }
void vita2d_draw_texture_scale(const vita2d_texture *texture, float x, float y, float x_scale, float y_scale)
{ (void)texture; (void)x; (void)y; (void)x_scale; (void)y_scale; }

/* ---- fixtures ---- */
static void put_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    TEST_ASSERT(f != NULL);
    TEST_ASSERT(fwrite(text, 1, strlen(text), f) == strlen(text));
    TEST_ASSERT(fclose(f) == 0);
}
/* A model file of `bytes` (sparse: only its size matters to the app). */
static void put_model(const char *name, long bytes)
{
    char path[256];
    snprintf(path, sizeof(path), MODEL_DIR "/%s", name);
    FILE *f = fopen(path, "wb");
    TEST_ASSERT(f != NULL);
    TEST_ASSERT(ftruncate(fileno(f), bytes) == 0);
    TEST_ASSERT(fclose(f) == 0);
}
static void remove_model(const char *name)
{
    char path[256];
    snprintf(path, sizeof(path), MODEL_DIR "/%s", name);
    unlink(path);
}
/* The temporary memory card of this test's process, removed when it ends (the
 * app's own forked processes leave with _exit, so only the test removes it), in
 * the suite's directory, which goes with whatever is left in it when the suite
 * ends (test_tmpdir.h). */
static char card_dir[PATH_MAX];
/* (test_tmpdir.h's: no link followed, no truncated path walked) */
static void remove_tree(const char *path) { vjo_test_remove_tree(path); }
static void remove_card(void)
{
    if (card_dir[0] && chdir("/") == 0) remove_tree(card_dir);
}
/* A fresh memory card: the default models present, config.ini as given (NULL: none). */
static void card(const char *config)
{
    static int initialized;
    if (!initialized) {
        TEST_ASSERT(vjo_test_tmpdir_new(card_dir, sizeof(card_dir)) != NULL);
        atexit(remove_card);
        TEST_ASSERT(chdir(card_dir) == 0);
        initialized = 1;
    }
    mkdir("ux0:data", 0777);
    mkdir("ux0:data/VitaJPOverlay", 0777);
    mkdir(MODEL_DIR, 0777);
    unlink(VJO_PREFERENCES_PATH);
    unlink(VJO_PREFERENCES_PATH ".bak");
    unlink(VJO_PREFERENCES_PATH ".tmp");
    unlink(VJO_PREFERENCES_REGION_PATH); /* #358: no region.ini (Suggest a text box ON) */
    unlink(VJO_PREFERENCES_REGION_PATH ".tmp");
    unlink(VJO_PREFERENCES_REGION_PATH ".bak");
    unlink(VJO_APP_PREFS_PATH);
    unlink(VJO_APP_PREFS_PATH ".bak");
    unlink(VJO_APP_PREFS_PATH ".tmp");
    app_rename_fails = 0;
    vjo_install_stub_open = vjo_install_stub_busy_to = vjo_anki_stub_hold_cancel_to = 0;
    unsigned n;
    const VjoVocrModelInfo *info = vjo_vocr_model_infos(&n);
    for (unsigned i = 0; i < n; i++) put_model(info[i].name, (long)info[i].bytes);
    if (config) put_file(VJO_PREFERENCES_PATH, config);
}
/* Runs the app, in a new process, over the inputs (then Circle); returns the
 * number of frames it showed. The app must end its process with code (main
 * returning the same), and never through vita2d_fini (settings/main.c: in a
 * system application it clears a display buffer libvita2d never set). */
static int run_to(const Input *inputs, int n, int code)
{
    if (!session) {
        session = mmap(NULL, sizeof(*session), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        TEST_ASSERT(session != MAP_FAILED);
        shown = session->shown;
    }
    memset(session, 0, sizeof(*session));
    session->main_rc = session->exited = -1;
    script = inputs; script_frames = n;
    fflush(stdout); fflush(stderr);
    pid_t pid = fork();
    TEST_ASSERT(pid >= 0);
    if (pid == 0) {
        frame = 0; exited = -1; finis = 0;
        session->main_rc = vjo_settings_main();
        session->frames = frame;
        session->exited = exited;
        session->finis = finis;
        session->font_loads = font_loads;
        session->font_frees = font_frees;
        session->font_free_in_frame = font_free_in_frame;
        session->font_free_unwaited = font_free_unwaited;
        session->dead_font_uses = dead_font_uses;
        session->most_glyphs = most_glyphs;
        session->most_fonts = most_fonts;
        session->anki_shows = vjo_anki_stub_shows;
        session->anki_frames = vjo_anki_stub_frames;
        session->anki_pressed = vjo_anki_stub_pressed;
        session->install_shows = vjo_install_stub_shows;
        session->install_frames = vjo_install_stub_frames;
        session->install_pressed = vjo_install_stub_pressed;
        session->lookup_shows = vjo_lookup_stub_shows;
        session->lookup_leaves = vjo_lookup_stub_leaves;
        session->lookup_frames = vjo_lookup_stub_frames;
        memcpy(session->lookup_pressed, vjo_lookup_stub_pressed, sizeof(session->lookup_pressed));
        memcpy(session->lookup_held, vjo_lookup_stub_held, sizeof(session->lookup_held));
        for (int k = 0; k < FONTS; k++) session->fonts_both += font_drew[k] && font_measured[k];
        _exit(0);
    }
    int status = 0;
    TEST_ASSERT(waitpid(pid, &status, 0) == pid);
    TEST_CHECK_(WIFEXITED(status) && !WEXITSTATUS(status), "the app's process ended with status %d", status);
    TEST_CHECK_(session->main_rc == code && session->exited == code, "main returned %d, exit code %d, expected %d",
                session->main_rc, session->exited, code);
    TEST_CHECK_(session->finis == 0, "vita2d_fini ran %d times", session->finis);
    /* Automatic Back/exit cleanup is outside the scripted frames being tested. */
    return session->frames < n ? session->frames : n;
}
static int run(const Input *inputs, int n) { return run_to(inputs, n, 0); }
/* Model component tests enter the real Advanced page through Reading. Hide only those two setup frames from
 * their recorded steps; navigation itself is exercised separately by settings_navigation. */
static int run_model(const Input *inputs, int n)
{
    Input expanded[MAX_FRAMES];
    TEST_ASSERT(n >= 2 && n + 2 <= MAX_FRAMES);
    memcpy(expanded, inputs, 2 * sizeof(*inputs));
    expanded[2] = (Input){0, 1, 820, 28};
    expanded[3] = (Input){0, 0, 0, 0};
    memcpy(expanded + 4, inputs + 2, (size_t)(n - 2) * sizeof(*inputs));
    int frames = run(expanded, n + 2);
    TEST_ASSERT(frames == n + 2);
    memmove(shown, shown + 2, (size_t)n * sizeof(*shown));
    return n;
}


/* What frame f showed. */
static const Text *text_at(int f, int y, int x_min, int x_max)
{
    for (int i = 0; i < shown[f].texts; i++) {
        const Text *t = &shown[f].text[i];
        if (t->y == y && t->x >= x_min && t->x < x_max) return t;
    }
    return NULL;
}
static int shows(int f, const char *needle)
{
    for (int i = 0; i < shown[f].texts; i++)
        if (strstr(shown[f].text[i].text, needle)) return 1;
    return 0;
}
typedef struct { int count; Rect rect[4]; const Text *label[4], *detail[4]; } Buttons;
/* Frame f's buttons of one row: filled rectangles of the row's height (an
 * outline's sides are 2 pixels wide), with the text in them. */
static Buttons row_buttons(int f, int y, int h, int w, int label_y, int detail_y)
{
    Buttons b = {0};
    for (int i = 0; i < shown[f].rects; i++) {
        const Rect *r = &shown[f].rect[i];
        if (r->y != y || r->h != h || r->w != w) continue;
        if (b.count < 4) {
            b.rect[b.count] = *r;
            b.label[b.count] = text_at(f, label_y, r->x, r->x + r->w);
            b.detail[b.count] = detail_y ? text_at(f, detail_y, r->x, r->x + r->w) : NULL;
        }
        b.count++;
    }
    return b;
}
static Buttons size_buttons(int f) { return row_buttons(f, SIZE_Y, SIZE_H, SIZE_W, SIZE_NAME_Y, SIZE_DETAIL_Y); }
static int width(const Text *t)
{
    return vita2d_pvf_text_width(NULL, t->scale, t->text);
}
/* Frame f's OCR card: no engine choice (VOCR is the only engine), its status line. */
static void check_status(int f, const char *status, unsigned status_color)
{
    const Text *s = text_at(f, STATUS_Y, 0, 960);
    TEST_CHECK_(s && !strcmp(s->text, status) && s->color == status_color, "frame %d: status '%s' colour %08X", f,
                s ? s->text : "(none)", s ? s->color : 0);
    if (s) TEST_CHECK(s->x + width(s) <= 912); /* within the card's text width (stub metrics) */
    TEST_CHECK_(!shows(f, "Google Lens") && !shows(f, "OCR engine") && !shows(f, "Wi-Fi"),
                "frame %d: an engine choice or Lens is shown", f);
}
/* Frame f's Model size row (API 21), the OCR card's only row: its title, the
 * three sizes in order with their estimates (names[i], details[i]), the chosen
 * one (1-3; 0: none) filled. */
static void check_sizes(int f, int chosen, const char *const names[3], const char *const details[3])
{
    Buttons b = size_buttons(f);
    const Text *title = text_at(f, SIZE_NAME_Y, 0, SIZE_X), *sub = text_at(f, SIZE_DETAIL_Y, 0, SIZE_X);
    TEST_CHECK_(b.count == 3, "frame %d: %d size buttons", f, b.count);
    TEST_CHECK_(title && !strcmp(title->text, "Model size") && title->x + width(title) < SIZE_X - 20,
                "frame %d: the row's title", f);
    TEST_CHECK(sub && !strcmp(sub->text, "estimated") && sub->x + width(sub) < SIZE_X - 40);
    if (b.count != 3) return;
    for (int i = 0; i < 3; i++) {
        int active = chosen == i + 1;
        TEST_CHECK_(b.rect[i].x == SIZE_X + i * SIZE_PITCH && b.rect[i].x + b.rect[i].w <= 918,
                    "frame %d: size %d at %d", f, i, b.rect[i].x);
        TEST_CHECK_(b.rect[i].color == (active ? ACCENT : SELECTED), "frame %d: size %d colour %08X", f, i,
                    b.rect[i].color);
        TEST_CHECK_(b.label[i] && !strcmp(b.label[i]->text, names[i]) &&
                    b.label[i]->color == (active ? BG_COLOR : WHITE),
                    "frame %d: size %d label '%s'", f, i, b.label[i] ? b.label[i]->text : "(none)");
        TEST_CHECK_(b.detail[i] && !strcmp(b.detail[i]->text, details[i]), "frame %d: size %d detail '%s'", f, i,
                    b.detail[i] ? b.detail[i]->text : "(none)");
        for (int k = 0; k < 2; k++) { /* centred in the button, inside it */
            const Text *t = k ? b.detail[i] : b.label[i];
            if (t) TEST_CHECK(t->x == b.rect[i].x + (b.rect[i].w - width(t)) / 2 && width(t) <= SIZE_W - 8);
        }
    }
    /* no outlined (muted) size: that was the row under Google Lens */
    int outline = 0;
    for (int i = 0; i < shown[f].rects; i++) {
        const Rect *r = &shown[f].rect[i];
        if (r->color == ACCENT && r->x >= SIZE_X && r->y >= SIZE_Y && r->y < SIZE_Y + SIZE_H &&
            (r->w == 2 || r->h == 2))
            outline++;
    }
    TEST_CHECK_(outline == 0, "frame %d: %d outline sides", f, outline);
}
/* No frame says anything about running a smaller size (owner 2026-10-15 04:54Z). */
static void check_no_fallback_hint(int frames)
{
    static const char *const hints[] = {"fallback", "falls back", "fall back", "smaller", "uses VOCR", "instead",
                                        "not fit", "too little", "short on memory"};
    for (int f = 0; f < frames && f < MAX_FRAMES; f++)
        for (unsigned i = 0; i < sizeof(hints) / sizeof(hints[0]); i++)
            TEST_CHECK_(!shows(f, hints[i]), "frame %d shows '%s'", f, hints[i]);
}

#define TAP(x, y) {0, 1, (x), (y)}, {0, 0, 0, 0}
#define PRESS(button) {(button), 0, 0, 0}, {0, 0, 0, 0}
#define OCR_PAGE TAP(200, 200) /* Settings -> Reading & dictionaries */
#define ADVANCED_PAGE OCR_PAGE, TAP(820, 28)
#define CARD_TITLE TAP(100, 140)  /* the size row's title: focus only */

static const char *const SIZES[3] = {"Small", "Normal", "Large"};
/* config.ini's ocr_backend after an app save: none. The engine is no setting
 * (VOCR is the only one), so a save drops the line (core/config.h
 * vjo_config_removed_line). */
#define SAVED_BACKEND ""

#ifndef VJO_VOCR_MODEL_INFO_TEST
#define CARDS 12 /* the page's cards, top to bottom, by their titles */
static const char *const CARD_NAMES[CARDS] = {"Model size", "Read text", "Speed boost while reading (444 MHz)",
                                              "Japanese characters only", "Selection mode",
                                              "Text source", /* API 31 */
                                              "Trusted hooks replace OCR", "Skip OCR on screens without text", /* API 43 */
                                              "Vertical text", /* API 47 */
                                              "Suggest a text box", /* #358 */
                                              "Pause the game to read", "Advanced reading"};
/* Frame f's card i: its top (its title is drawn 30 below it, at x 42), or -1 when not shown. */
static int card_top(int f, int i)
{
    for (int k = 0; k < shown[f].texts; k++)
        if (shown[f].text[k].x == 42 && !strcmp(shown[f].text[k].text, CARD_NAMES[i])) return shown[f].text[k].y - 30;
    return -1;
}
/* Frame f's focused card (whose focus bar is drawn at its left edge), or -1. */
static int focused_card(int f)
{
    for (int i = 0; i < shown[f].rects; i++) {
        const Rect *r = &shown[f].rect[i];
        if (r->x == 24 && r->w == 4 && r->color == ACCENT)
            for (int c = 0; c < CARDS; c++)
                if (card_top(f, c) == r->y) return c;
    }
    return -1;
}
/* Frame f draws nothing of a card into the status line or the hints below the
 * cards' room: whatever reaches past ROOM_END is clipped at it (the status box,
 * its line and the hints are the footer's own). */
static void check_room(int f)
{
    for (int i = 0; i < shown[f].rects; i++) {
        const Rect *r = &shown[f].rect[i];
        int bottom = r->y + r->h;
        if ((r->x == 0 && r->w == 960) || (r->x < 112 && r->y >= 504) || r->y < OCR_Y || (r->x == 24 && r->w == 912 && r->y == ROOM_END + 4)) continue; /* the header; the status box */
        if (r->clipped && r->clip_bottom < bottom) bottom = r->clip_bottom;
        TEST_CHECK_(bottom <= ROOM_END || r->y >= bottom, "frame %d: a %dx%d rectangle at %d,%d reaches %d", f, r->w,
                    r->h, r->x, r->y, bottom);
    }
    for (int i = 0; i < shown[f].texts; i++) {
        const Text *t = &shown[f].text[i];
        int bottom = t->y + 8; /* a baseline's descenders */
        if ((t->x < 112 && t->y >= 510) || t->y < OCR_Y || (t->x == 40 && t->y == ROOM_END + 4 + 22) || t->y == 535) continue; /* status line, hints */
        if (t->clipped && t->clip_bottom < bottom) bottom = t->clip_bottom;
        TEST_CHECK_(bottom <= ROOM_END || t->y - 24 >= bottom, "frame %d: '%s' at %d,%d drawn below the room", f,
                    t->text, t->x, t->y);
    }
}
/* Frame f's Read text card at top y: its title, its two choices (Automatically,
 * then the press), each with its line; `chosen` (VJO_OCR_*, -1: none) filled. */
static void check_read(int f, int y, int chosen, const char *press, const char *verb)
{
    char ready[64], waits[64];
    const char *names[2] = {"Automatically (when it changes)", press}, *lines[2] = {ready, waits};
    snprintf(ready, sizeof(ready), "The popup is ready when you %s", verb);
    snprintf(waits, sizeof(waits), "Nothing runs until you %s", verb);
    const Text *title = text_at(f, y + 30, 0, 960);
    TEST_CHECK_(title && !strcmp(title->text, "Read text") && title->x == 42, "frame %d: the Read text title", f);
    Buttons b = row_buttons(f, y + 42, READ_BUTTON_H, READ_BUTTON_W, y + 42 + 24, y + 42 + 47);
    TEST_CHECK_(b.count == 2, "frame %d: %d Read text choices", f, b.count);
    if (b.count != 2) return;
    for (int k = 0; k < 2; k++) {
        int active = chosen == (k ? VJO_OCR_ON_PRESS : VJO_OCR_AUTO);
        TEST_CHECK_(b.rect[k].x == READ_BUTTON_X(k) && b.rect[k].x + b.rect[k].w <= 920, "frame %d: choice %d at %d", f,
                    k, b.rect[k].x);
        TEST_CHECK_(b.rect[k].color == (active ? ACCENT : SELECTED), "frame %d: choice %d colour %08X", f, k,
                    b.rect[k].color);
        TEST_CHECK_(b.label[k] && !strcmp(b.label[k]->text, names[k]) && b.label[k]->color == (active ? BG_COLOR : WHITE),
                    "frame %d: choice %d '%s'", f, k, b.label[k] ? b.label[k]->text : "(none)");
        TEST_CHECK_(b.detail[k] && !strcmp(b.detail[k]->text, lines[k]) && b.detail[k]->color == (active ? BG_COLOR : DIM),
                    "frame %d: choice %d line '%s'", f, k, b.detail[k] ? b.detail[k]->text : "(none)");
        for (int m = 0; m < 2; m++) { /* centred in its button, inside it */
            const Text *t = m ? b.detail[k] : b.label[k];
            if (t) TEST_CHECK(t->x == b.rect[k].x + (b.rect[k].w - width(t)) / 2 && width(t) <= READ_BUTTON_W - 8);
        }
    }
}
/* Frame f's Speed boost card at top y: its title, its ON/OFF badge (label, filled when on), its help. */
static void check_boost(int f, int y, const char *label, int on)
{
    const Text *title = text_at(f, y + 30, 0, 960), *help = text_at(f, y + 60, 0, 960), *badge = text_at(f, y + 32, 812, 920);
    TEST_CHECK_(title && !strcmp(title->text, "Speed boost while reading (444 MHz)") && title->x + width(title) < 812 - 16,
                "frame %d: the Speed boost title", f);
    TEST_CHECK_(badge && !strcmp(badge->text, label), "frame %d: the badge says '%s'", f, badge ? badge->text : "(none)");
    TEST_CHECK_(help && !strcmp(help->text, "ON reads faster: 444 MHz only while text is read.") &&
                help->x + width(help) <= 912 - 100, "frame %d: the Speed boost help", f);
    int filled = 0;
    for (int i = 0; i < shown[f].rects; i++) {
        const Rect *r = &shown[f].rect[i];
        if (r->x == 812 && r->y == y + 6 && r->w == 108 && r->h == 34) filled = r->color == ACCENT ? 1 : 2;
    }
    TEST_CHECK_(filled == (on ? 1 : 2), "frame %d: the badge %s", f, on ? "is not filled" : "is filled");
}

static const char *const NORMAL_STATUS = "Normal: default, balanced. VOCR6-N found.";
static const char *const SMALL_STATUS = "Small: fastest. VOCR6-S found.";
static const char *const LARGE_STATUS = "Large: most accurate. VOCR6-L found.";
/* This package's estimates (core/vocr_models.c): VOCR6-S, VOCR6-N, VOCR6-L (v0.4.1-vocr6.2). */
static const char *const DETAILS[3] = {"0.6 s, 1.0 MB", "0.8 s, 1.4 MB", "1.9 s, 2.8 MB"};

/* This package: one engine, VOCR; the OCR card is its Model size, Normal chosen,
 * each size with its estimates from the models' metadata, and the status line
 * naming the size's model (its generation in the name, VOCR6-N). No engine
 * choice and nothing of Google Lens: the overlay is offline (owner 2026-10-15). */
static void test_one_engine_its_model_size_on_top(void)
{
    card(NULL); /* first run: no config.ini, the defaults */
    static const Input in[] = {OCR_PAGE};
    TEST_ASSERT(run_model(in, 2) == 2); /* each scripted frame drawn; Circle then exits */
    for (int f = 0; f < 2; f++) {
        check_status(f, NORMAL_STATUS, DIM);
        check_sizes(f, VJO_VOCR_TIER_NORMAL, SIZES, DETAILS);
        TEST_CHECK(!shows(f, "VOCR2") && !shows(f, "Custom engine"));
    }
    check_no_fallback_hint(2);
    TEST_CHECK(!shown[1].backend[0] && !shown[1].tier[0]); /* looking saves nothing */
    TEST_CHECK(access(VJO_PREFERENCES_PATH, F_OK) != 0);
    /* the card is the page's first and focused: its bar spans the whole card */
    int bar = 0;
    for (int i = 0; i < shown[1].rects; i++) {
        const Rect *r = &shown[1].rect[i];
        if (r->x == 24 && r->w == 4 && r->color == ACCENT) bar += r->y == OCR_Y && r->h == OCR_H;
    }
    TEST_CHECK(bar == 1);
    /* below it, how text is read: the Read text card, then the Speed boost card; a lookup card peeks */
    TEST_CHECK(shows(1, "Speed boost while reading") && shows(1, "Text source"));
    TEST_CHECK(!shows(1, "Read text"));
}

/* API 21: the Model size. Touch a size to choose and save it (its edges and the
 * gaps between them only focus the row); the controller starts on it, Left and
 * Right step through the sizes, Enter (× here) goes round them, Down and Up leave
 * the row and come back. The status line names the size's model: VOCR6-L for
 * Large. */
static void test_model_size_touch_keys_and_save(void)
{
    /* Each edge tap comes when it would change the saved size were that edge a pixel off: an
     * inside pixel from another size, an outside one with a neighbour chosen. Small spans x 232-451,
     * Normal 465-684, Large 698-917, each y 118-171. */
    static const struct { int x, y; const char *tier; } taps[] = {
        {451, 171, "small"},  /* Small, bottom-right pixel (from Normal) */
        {465, 118, "normal"}, /* Normal, top-left pixel */
        {452, 150, "normal"}, /* the gap right of Small */
        {698, 118, "large"},  /* Large, top-left pixel */
        {464, 150, "large"},  /* the gap left of Normal */
        {684, 171, "normal"}, /* Normal, bottom-right pixel */
        {697, 150, "normal"}, /* the gap left of Large */
        {917, 171, "large"},  /* Large, bottom-right pixel */
        {685, 150, "large"},  /* the gap right of Normal */
        {232, 118, "small"},  /* Small, top-left pixel */
        {918, 150, "small"},  /* right of Large */
        {500, 117, "small"},  /* the row above the buttons: focus only */
        {500, 172, "small"},  /* below them, the status line: focus only */
        {300, 171, "small"},
        {500, 150, "normal"},
        {231, 150, "normal"}, /* left of Small: the row's title */
        {100, 150, "normal"},
        {451, 118, "small"},  /* Small, top-right pixel */
    };
    enum { N = sizeof(taps) / sizeof(taps[0]) };
    static Input in[2 + 2 * N] = {OCR_PAGE};
    card("ocr_backend = vocr\nvocr_model = H15_w8.vocr\nvocr_fallback_model = FL10_w8.vocr\nfont_size_ja = 22\n");
    for (int i = 0; i < N; i++) in[2 + 2 * i] = (Input){0, 1, taps[i].x, taps[i].y};
    TEST_ASSERT(run_model(in, 2 + 2 * N) == 2 + 2 * N);
    /* an older build's vocr_model = H15_w8.vocr shows as Normal until a choice; the first save
     * writes vocr_tier and drops the vocr_model and vocr_fallback_model lines */
    check_sizes(1, VJO_VOCR_TIER_NORMAL, SIZES, DETAILS);
    TEST_CHECK(!shown[1].tier[0] && shown[1].model_line);
    static const char *const status[] = {"", SMALL_STATUS, NORMAL_STATUS, LARGE_STATUS};
    for (int i = 0; i < N; i++) {
        int f = 2 + 2 * i, t = vjo_vocr_tier_parse(taps[i].tier);
        TEST_CHECK_(!strcmp(shown[f].tier, taps[i].tier), "tap %d at %d,%d: saved '%s', want %s", i, taps[i].x,
                    taps[i].y, shown[f].tier, taps[i].tier);
        TEST_CHECK_(!shown[f].model_line, "tap %d: an older model line is left", i);
        TEST_CHECK_(!strcmp(shown[f].backend, SAVED_BACKEND), "tap %d: ocr_backend '%s'", i, shown[f].backend);
        check_status(f + 1, status[t], DIM);
        check_sizes(f + 1, t, SIZES, DETAILS);
    }
    check_no_fallback_hint(2 + 2 * N);
    /* every other setting survived the saves */
    FILE *file = fopen(VJO_PREFERENCES_PATH, "rb");
    TEST_ASSERT(file != NULL);
    char text[4096];
    size_t n = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[n] = 0;
    TEST_CHECK(strstr(text, "font_size_ja = 22\n") && strstr(text, "vocr_tier = small\n"));
    /* the controller: the size row first, Left/Right step, Enter goes round, Down/Up leave and come back */
    card("vocr_tier = small\nocr_mode = on_press\n");
    static const Input keys[] = {OCR_PAGE, PRESS(SCE_CTRL_LEFT), PRESS(SCE_CTRL_RIGHT),
                                 PRESS(SCE_CTRL_RIGHT), PRESS(SCE_CTRL_RIGHT), PRESS(SCE_CTRL_CROSS),
                                 PRESS(SCE_CTRL_CROSS), PRESS(SCE_CTRL_LEFT), PRESS(SCE_CTRL_DOWN),
                                 PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_RIGHT)};
    run_model(keys, (int)(sizeof(keys) / sizeof(keys[0])));
    static const struct { int frame; const char *tier; } want[] = {
        {2, "small"},   /* Left at Small: nothing smaller, nothing saved */
        {4, "normal"}, {6, "large"},
        {8, "large"},   /* Right at Large: nothing larger */
        {10, "small"},  /* Enter: round to Small */
        {12, "normal"}, {14, "small"},
        {16, "small"},  /* Down: the next card */
        {18, "small"},  /* Up: the size row again */
        {20, "normal"}, /* Right there steps the size */
    };
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        int f = want[i].frame;
        TEST_CHECK_(!strcmp(shown[f].tier, want[i].tier), "frame %d: saved %s, want %s", f, shown[f].tier,
                    want[i].tier);
    }
    TEST_CHECK(!strcmp(shown[2].backend, "")); /* nothing saved before the first step */
    check_sizes(7, VJO_VOCR_TIER_LARGE, SIZES, DETAILS);
    check_status(7, LARGE_STATUS, DIM);
    check_sizes(21, VJO_VOCR_TIER_NORMAL, SIZES, DETAILS);
    check_status(21, NORMAL_STATUS, DIM);
    /* the focus bar: beside the card while its row is selected, on the next card after Down */
    int bar_card = 0, bar_next = 0;
    for (int i = 0; i < shown[3].rects; i++) {
        const Rect *r = &shown[3].rect[i];
        if (r->x == 24 && r->w == 4 && r->color == ACCENT) bar_card += r->y == OCR_Y;
    }
    for (int i = 0; i < shown[17].rects; i++) {
        const Rect *r = &shown[17].rect[i];
        if (r->x == 24 && r->w == 4 && r->color == ACCENT) {
            bar_next += r->y == READ_Y;
            bar_card += r->y == OCR_Y;
        }
    }
    TEST_CHECK(bar_card == 1 && bar_next == 1);
}

/* A tap in the card's top band (y OCR_Y to OCR_SIZE_Y - 1, above the size buttons) focuses
 * the card and chooses nothing, wherever the focus was before (audit-overlay-shell-slim-1 M12). */
static int focus_bar_at(int f, int y)
{
    int n = 0;
    for (int i = 0; i < shown[f].rects; i++) {
        const Rect *r = &shown[f].rect[i];
        n += r->x == 24 && r->w == 4 && r->color == ACCENT && r->y == y;
    }
    return n;
}
static void test_the_cards_top_band_focuses_it(void)
{
    card("vocr_tier = small\nfont_size_ja = 22\n");
    static const Input in[] = {OCR_PAGE, PRESS(SCE_CTRL_DOWN), TAP(300, OCR_Y + 2), PRESS(SCE_CTRL_RIGHT)};
    run_model(in, (int)(sizeof(in) / sizeof(in[0])));
    TEST_CHECK_(focus_bar_at(3, READ_Y) == 1 && !focus_bar_at(3, OCR_Y), "frame 3: the next card focused");
    TEST_CHECK_(focus_bar_at(5, OCR_Y) == 1 && !focus_bar_at(5, READ_Y), "frame 5: the tap focused the card");
    TEST_CHECK_(!strcmp(shown[5].tier, "small"), "the tap chose a size: '%s'", shown[5].tier); /* still the file's */
    TEST_CHECK_(!strcmp(shown[6].tier, "normal"), "Right then steps the size: '%s'", shown[6].tier);
}

/* A config.ini that chose an engine (vocr, or a removed one: Google Lens since
 * the overlay went offline, VOCR2, Meiki, ncnn) shows the Model size as for any
 * other (it loads as vocr), and the app's next save drops its line. */
static void test_engine_line_shows_the_size_and_next_save_drops_it(void)
{
    static const char *const removed[] = {"vocr", "lens", "vocr2", "meiki", "ncnn"};
    for (unsigned i = 0; i < sizeof(removed) / sizeof(removed[0]); i++) {
        char config[128];
        snprintf(config, sizeof(config), "; mine\nocr_backend = %s\nfont_size_ja = 22\n", removed[i]);
        card(config);
        static const Input in[] = {OCR_PAGE, CARD_TITLE, PRESS(SCE_CTRL_RIGHT)};
        run_model(in, (int)(sizeof(in) / sizeof(in[0])));
        check_status(1, NORMAL_STATUS, DIM);
        check_sizes(1, VJO_VOCR_TIER_NORMAL, SIZES, DETAILS);
        TEST_CHECK_(!strcmp(shown[3].backend, removed[i]), "%s: unchanged until a choice", removed[i]);
        TEST_CHECK_(!strcmp(shown[4].backend, SAVED_BACKEND) && !strcmp(shown[4].tier, "large"),
                    "%s: left as '%s'", removed[i], shown[4].backend);
        for (int f = 0; f < 6; f++) TEST_CHECK(!shows(f, "VOCR2") && !shows(f, "Lens"));
    }
}

/* The status line names the chosen size's model when its file is there at its
 * release size, or reports it missing in red, never what runs instead (a job
 * leaves a missing tier out and runs the next smaller one found). */
static void test_status_follows_the_configured_models(void)
{
    static const Input in[] = {OCR_PAGE};
    /* an older build's vocr_model, in each family: its size */
    static const struct { const char *ini, *status; int tier; } older[] = {
        {"vocr_model = H15_w8.vocr\nvocr_fallback_model = FL10_w8.vocr\n", NORMAL_STATUS, 2},
        {"vocr_model = FL10_w8.vocr\n", SMALL_STATUS, 1},
        {"vocr_model = VOCR3_FL10_w8.vocr\nvocr_fallback_model = none\n", SMALL_STATUS, 1},
        {"vocr_model = VOCR3_H15_w8.vocr\n", NORMAL_STATUS, 2},
        {"vocr_model = F20_w8.vocr\n", LARGE_STATUS, 3},
        {"vocr_model = VOCR4_W20_w8.vocr\n", LARGE_STATUS, 3},
        {"vocr_model = VOCR9_X_w8.vocr\n", NORMAL_STATUS, 2}, /* not a known shape: the default */
        {"vocr_tier = small\nvocr_model = VOCR4_W20_w8.vocr\n", SMALL_STATUS, 1},
    };
    for (unsigned i = 0; i < sizeof(older) / sizeof(older[0]); i++) {
        card(older[i].ini);
        run_model(in, 2);
        check_status(1, older[i].status, DIM);
        check_sizes(1, older[i].tier, SIZES, DETAILS);
        TEST_MSG("%s", older[i].ini);
    }
    /* the chosen size's file missing, or not its release size (the Large blob a byte short) */
    card("vocr_tier = large\n");
    remove_model(VJO_VOCR_TIER3_NAME);
    run_model(in, 2);
    check_status(1, "VOCR6-L missing. Reinstall the package.", OFF);
    put_model(VJO_VOCR_TIER3_NAME, VJO_VOCR_TIER3_BYTES - 1);
    run_model(in, 2);
    check_status(1, "VOCR6-L missing. Reinstall the package.", OFF);
    /* nothing at or below the chosen size: the chosen size's model, a red line */
    remove_model(VJO_VOCR_TIER2_NAME);
    remove_model(VJO_VOCR_TIER1_NAME);
    run_model(in, 2);
    check_status(1, "VOCR6-L missing. Reinstall the package.", OFF);
    /* a larger size's file missing does not matter to a smaller choice, unless the smaller size has
     * the same model (a size that kept the model of the size above it) */
    card("vocr_tier = normal\n");
    remove_model(VJO_VOCR_TIER3_NAME);
    run_model(in, 2);
    if (strcmp(VJO_VOCR_TIER2_NAME, VJO_VOCR_TIER3_NAME)) check_status(1, NORMAL_STATUS, DIM);
    else check_status(1, "VOCR6-N missing. Reinstall the package.", OFF);
    /* Normal's file missing: Normal named missing */
    card(NULL);
    remove_model(VJO_VOCR_TIER2_NAME);
    run_model(in, 2);
    check_status(1, "VOCR6-N missing. Reinstall the package.", OFF);
    /* a model file that cannot be closed again is not counted as found */
    card(NULL);
    close_fails = VJO_VOCR_TIER2_NAME;
    run_model(in, 2);
    close_fails = NULL;
    check_status(1, "VOCR6-N missing. Reinstall the package.", OFF);
    /* the earlier generations' files that are no size's any more: their absence changes nothing */
    card(NULL);
    {
        static const char *const earlier[] = {VJO_VOCR3_H15_NAME, VJO_VOCR3_FL10_NAME, VJO_VOCR4_W20_NAME};
        for (unsigned i = 0; i < sizeof(earlier) / sizeof(earlier[0]); i++)
            if (strcmp(earlier[i], VJO_VOCR_TIER1_NAME) && strcmp(earlier[i], VJO_VOCR_TIER2_NAME) &&
                strcmp(earlier[i], VJO_VOCR_TIER3_NAME))
                remove_model(earlier[i]);
    }
    run_model(in, 2);
    check_status(1, NORMAL_STATUS, DIM);
    check_no_fallback_hint(2);
}

/* Read text and Speed boost while reading, on a first run (no config.ini: the
 * defaults, auto and on): below the Model size card, each choice with its line,
 * a lookup card peeking at the bottom, cut at the room's end; looking saves nothing. */
static void test_read_text_and_speed_boost_defaults(void)
{
    card(NULL);
    static const Input basic[] = {OCR_PAGE};
    run(basic, sizeof(basic) / sizeof(basic[0]));
    check_read(1, OCR_Y, VJO_OCR_AUTO, "Only when I press L + R", "press");
    TEST_CHECK(shows(1, "Selection mode") && shows(1, "Japanese characters only"));
    TEST_CHECK(!shows(1, "Model size") && !shows(1, "444 MHz") && !shows(1, "Trusted hooks"));
    TEST_CHECK(focused_card(1) == 1 && access(VJO_PREFERENCES_PATH, F_OK) != 0);
    static const Input advanced[] = {ADVANCED_PAGE};
    run(advanced, sizeof(advanced) / sizeof(advanced[0]));
    check_sizes(3, VJO_VOCR_TIER_NORMAL, SIZES, DETAILS);
    check_boost(3, OCR_Y + OCR_H + 4, "ON", 1);
}

/* Read text by the controller (Down to it; Right the press, Left automatically,
 * Enter in turn) and by touch (a choice takes it; its edges are exact; the
 * title row, the gap and the margin only focus), saved as ocr_mode; Speed
 * boost by Enter, Left (OFF), Right (ON) and a touch anywhere on its card, saved
 * as ocr_boost_clock; every other line of config.ini kept. */
static void test_read_text_and_speed_boost_by_keys_and_touch(void)
{
    card("; keep\nocr_mode = auto\nocr_boost_clock = on\nfont_size_en = 16\n");
    static const Input basic[] = {OCR_PAGE, PRESS(SCE_CTRL_RIGHT), PRESS(SCE_CTRL_LEFT), TAP(700, 174), TAP(150, 174)};
    run(basic, sizeof(basic) / sizeof(basic[0]));
    TEST_CHECK(!strcmp(shown[2].mode, "on_press") && !strcmp(shown[4].mode, "auto"));
    TEST_CHECK(!strcmp(shown[6].mode, "on_press") && !strcmp(shown[8].mode, "auto"));
    TEST_CHECK(!strcmp(shown[8].boost, "on"));
    static const Input advanced[] = {ADVANCED_PAGE, PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_LEFT),
        PRESS(SCE_CTRL_RIGHT), PRESS(SCE_CTRL_CROSS), TAP(850, 230)};
    run(advanced, sizeof(advanced) / sizeof(advanced[0]));
    TEST_CHECK(!strcmp(shown[6].boost, "off") && !strcmp(shown[8].boost, "on"));
    TEST_CHECK(!strcmp(shown[10].boost, "off") && !strcmp(shown[12].boost, "on"));
    TEST_CHECK(!strcmp(shown[12].mode, "auto"));
    char value[32]; saved_value("font_size_en", value); TEST_CHECK(!strcmp(value, "16"));
}

/* The page scrolls a card at a time: Down from the Model size to the last
 * card brings each card all into view, with the fewest cards scrolled, and Up
 * back; Up from the first card wraps to the last. A card cut at the bottom is
 * only selected by a touch (no change), then a touch on it acts. Nothing of a
 * card is ever drawn into the status line. */
static void test_the_page_scrolls_a_card_at_a_time(void)
{
    card(NULL);
    static const Input basic[] = {OCR_PAGE, PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_DOWN),
        PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_DOWN)};
    int frames = run(basic, sizeof(basic) / sizeof(basic[0]));
    static const int want[] = {1, 3, 4, 11, 4, 3, 1, 11, 1};
    for (unsigned k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
        int f = 1 + 2 * (int)k;
        TEST_CHECK_(focused_card(f) == want[k], "step %u: focused %d", k, focused_card(f));
        TEST_CHECK(card_top(f, 1) == OCR_Y && card_top(f, 11) + 58 <= ROOM_END);
        check_room(f);
    }
    TEST_CHECK(frames == sizeof(basic) / sizeof(basic[0]));
    static const Input advanced[] = {ADVANCED_PAGE, PRESS(SCE_CTRL_UP)};
    run(advanced, sizeof(advanced) / sizeof(advanced[0]));
    TEST_CHECK(focused_card(5) == 10 && card_top(5, 7) == OCR_Y);
    TEST_CHECK(card_top(5, 10) + CARD_H <= ROOM_END && card_top(5, 0) < 0);
    for (int f = 3; f < 6; f++) check_room(f);
}

/* The lookup cards' choices once the page has scrolled (Speed boost first: Highlight at y 256, Dictionaries shown
 * at 328; audit N8): drawn there, and touched there, at their badges' exact edges. A touch elsewhere on Highlight
 * switches it, as before the page scrolled; on Dictionaries shown only a number chooses. */
static void test_scrolled_cards_keep_their_touch_targets(void)
{
    card("highlight_mode = word\njapanese_only_selection = off\n");
    static const Input in[] = {OCR_PAGE, TAP(850, 310), TAP(600, 310), TAP(850, 240), TAP(850, 240)};
    run(in, sizeof(in) / sizeof(in[0]));
    TEST_CHECK(!strcmp(shown[2].highlight, "character") && !strcmp(shown[4].highlight, "word"));
    TEST_CHECK(!strcmp(shown[6].japanese, "on") && !strcmp(shown[8].japanese, "off"));
    TEST_CHECK(focused_card(3) == 4 && focused_card(7) == 3);
}

/* #418 (P10, audit-rubi-cleanup-10 F11): Dictionaries shown by its buttons (settings/main.c choose_preference): Enter
 * takes the next count, 8 then 1; Left and Right the count beside it, never past 1 or 8. */
static void test_removed_dictionary_count_has_no_control(void)
{
    static const Input in[] = {OCR_PAGE, PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_DOWN),
        PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_RIGHT)};
    card("local_dictionaries = a.vjdict,b.vjdict,c.vjdict\nlocal_dictionary_results = 1\n");
    TEST_ASSERT(run(in, sizeof(in) / sizeof(in[0])) == sizeof(in) / sizeof(in[0]));
    TEST_CHECK(focused_card(sizeof(in) / sizeof(in[0]) - 1) != 5);
    for (int f = 0; f < (int)(sizeof(in) / sizeof(in[0])); f++)
        for (int t = 0; t < shown[f].texts; t++)
            TEST_CHECK(strcmp(shown[f].text[t].text, "Dictionaries shown"));
}

/* The press choice names what opens the overlay (config.ini toggle_button). */
static void test_read_text_names_the_buttons_that_open_the_overlay(void)
{
    static const struct { const char *ini, *press, *verb; } triggers[] = {
        {"toggle_button = select\n", "Only when I press Select", "press"},
        {"toggle_button = start\n", "Only when I press Start", "press"},
        {"toggle_button = select+l\n", "Only when I press Select + L", "press"},
        {"toggle_button = select+r\nsubtitle_button = select+l\n", "Only when I press Select + R", "press"},
        {"toggle_button = rear_double_tap\n", "Only when I double-tap the back", "tap"},
        {"toggle_button = l+r\nocr_mode = on_press\n", "Only when I press L + R", "press"},
        {"toggle_button = nonsense\n", "Only when I press L + R", "press"},
    };
    static const Input in[] = {OCR_PAGE};
    for (unsigned i = 0; i < sizeof(triggers) / sizeof(triggers[0]); i++) {
        card(triggers[i].ini);
        run(in, 2);
        check_read(1, OCR_Y, strstr(triggers[i].ini, "on_press") ? VJO_OCR_ON_PRESS : VJO_OCR_AUTO, triggers[i].press,
                   triggers[i].verb);
        TEST_MSG("%s", triggers[i].ini);
    }
}

/* An unreadable config.ini: nothing is selected and nothing can be saved. */
static void test_unreadable_config_selects_and_saves_nothing(void)
{
    card(NULL);
    TEST_ASSERT(mkdir(VJO_PREFERENCES_PATH, 0777) == 0);
    static const Input basic[] = {OCR_PAGE, PRESS(SCE_CTRL_RIGHT), PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_CROSS)};
    run(basic, sizeof(basic) / sizeof(basic[0]));
    check_read(1, OCR_Y, -1, "Only when I press L + R", "press");
    TEST_CHECK(shows(7, "Cannot read config.ini"));
    static const Input advanced[] = {ADVANCED_PAGE, PRESS(SCE_CTRL_RIGHT), PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_CROSS)};
    run(advanced, sizeof(advanced) / sizeof(advanced[0]));
    check_sizes(3, 0, SIZES, DETAILS);
    TEST_CHECK(shows(9, "Cannot read config.ini"));
    struct stat st; TEST_ASSERT(stat(VJO_PREFERENCES_PATH, &st) == 0); TEST_CHECK(S_ISDIR(st.st_mode));
}

/* A saved choice says when it applies: true with no game running as beside one (the app
 * opens from the LiveArea either way). */
static void test_a_saved_choice_says_when_it_applies(void)
{
    card(NULL);
    static const Input in[] = {OCR_PAGE, TAP(SIZE_X + 2 * SIZE_PITCH + 20, SIZE_Y + 20)}; /* Large */
    run_model(in, (int)(sizeof(in) / sizeof(in[0])));
    char tier[16];
    saved_value("vocr_tier", tier);
    TEST_CHECK_(!strcmp(tier, "large"), "vocr_tier = %s", tier);
    const Text *st = text_at(3, ROOM_END + 4 + 22, 40, 41); /* the page's status line */
    TEST_CHECK_(st && !strcmp(st->text, "Saved. It applies when the overlay next opens.") && st->color == WHITE,
                "the status line: '%s'", st ? st->text : "(none)");
}

/* API 31: Text source (Up from the first card reaches the last, API 55's Pause card; five more Ups reach it): Right
 * and Left step within its
 * three choices (Automatic, Every game, OCR only: auto, hooks, ocr), Enter takes the next one (OCR only
 * to Automatic), a touch on a choice takes it and elsewhere on the card only focuses. Each change is
 * config.ini's text_source, every other line kept, and the status line says it applies when a game next
 * starts; a file without the line shows Automatic and writes nothing until a change. */
static void test_text_source_by_keys_and_touch(void)
{
    card("text_source = hooks\n");
    static const Input in[] = {ADVANCED_PAGE, PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_LEFT),
        PRESS(SCE_CTRL_RIGHT), PRESS(SCE_CTRL_RIGHT), PRESS(SCE_CTRL_LEFT), PRESS(SCE_CTRL_CROSS),
        TAP(600, 302), TAP(820, 302)};
    run(in, sizeof(in) / sizeof(in[0]));
    TEST_CHECK(!strcmp(shown[8].source, "hooks")); /* looking at the legacy alias does not rewrite it */
    TEST_CHECK(!strcmp(shown[10].source, "ocr") && !strcmp(shown[12].source, "ocr"));
    TEST_CHECK(!strcmp(shown[14].source, "auto") && !strcmp(shown[16].source, "ocr"));
    TEST_CHECK(!strcmp(shown[18].source, "auto") && !strcmp(shown[20].source, "ocr"));
    TEST_CHECK(focused_card(7) == 5 && shows(11, "Saved. It applies when a game next starts."));
    for (int f = 3; f < 22; f++) { TEST_CHECK(!shows(f, "Every game")); check_room(f); }
}

/* API 43: the two OCR-skipping switches, above the page's last three cards (Up four times from the first card reaches
 * Skip OCR on screens without text, one more Trusted hooks replace OCR). Enter switches the focused one, Left is OFF and Right
 * ON, a touch anywhere on a card in view switches it; each change saves ocr_text_gate and hook_trust together,
 * every other line kept, and the status line says it applies when the overlay next opens. A file without the lines
 * shows the defaults (trusted hooks ON, the text gate OFF: docs/vita-vn-ocr.md, its measurement) and writes nothing
 * until a change. */
static void test_ocr_skip_switches_by_keys_and_touch(void)
{
    card(NULL);
    static const Input in[] = {ADVANCED_PAGE, PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_DOWN),
        PRESS(SCE_CTRL_LEFT), PRESS(SCE_CTRL_RIGHT), PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_CROSS),
        PRESS(SCE_CTRL_LEFT), TAP(850, 276), TAP(850, 348)};
    run(in, sizeof(in) / sizeof(in[0]));
    TEST_CHECK(!strcmp(shown[10].trust, "off") && !strcmp(shown[12].trust, "on"));
    TEST_CHECK(!strcmp(shown[16].gate, "on") && !strcmp(shown[18].gate, "off"));
    TEST_CHECK(!strcmp(shown[20].trust, "off") && !strcmp(shown[22].gate, "on"));
    TEST_CHECK(focused_card(9) == 6 && focused_card(15) == 7);
}

/* API 47: Vertical text, the third card from the end (Up three times from the first card reaches it): Right and Left step within its three
 * choices (OFF, Automatic, ON: config.ini ocr_vertical off, auto, on), Enter takes the next one (ON to OFF), a touch
 * on a choice takes it and elsewhere on the card only focuses. Each change is config.ini's ocr_vertical, every other
 * line kept, and the status line says it applies when the overlay next opens; a file without the line shows the
 * default (VJO_VERTICAL_DEFAULT, Automatic) and writes nothing until a change. */
static void test_vertical_text_by_keys_and_touch(void)
{
    card(NULL);
    static const Input in[] = {ADVANCED_PAGE, PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP),
        PRESS(SCE_CTRL_LEFT), PRESS(SCE_CTRL_CROSS), PRESS(SCE_CTRL_RIGHT), PRESS(SCE_CTRL_CROSS),
        TAP(700, 205), TAP(850, 205)};
    run(in, sizeof(in) / sizeof(in[0]));
    TEST_CHECK(!strcmp(shown[10].vertical, "off") && !strcmp(shown[12].vertical, "auto") &&
               !strcmp(shown[14].vertical, "on") && !strcmp(shown[16].vertical, "off"));
    TEST_CHECK(!strcmp(shown[18].vertical, "auto") && !strcmp(shown[20].vertical, "on"));
    TEST_CHECK(focused_card(9) == 8);
}

/* API 55: Pause the game to read, the last card (Up from the first card reaches it): Enter switches it, Right is ON
 * and Left OFF, a touch anywhere on it switches it; each change is config.ini's ocr_pause, every other line kept, and
 * the status line says it applies when the overlay next opens. A file without the line shows OFF (the default) and
 * writes nothing until a change. */
static void test_pause_card_by_keys_and_touch(void)
{
    card(NULL);
    static const Input in[] = {ADVANCED_PAGE, PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_CROSS), PRESS(SCE_CTRL_LEFT),
        PRESS(SCE_CTRL_RIGHT), TAP(850, 348)};
    run(in, sizeof(in) / sizeof(in[0]));
    TEST_CHECK(focused_card(5) == 10 && !shown[5].pause[0]); /* OFF without a saved line */
    TEST_CHECK(!strcmp(shown[6].pause, "on") && !strcmp(shown[8].pause, "off") &&
               !strcmp(shown[10].pause, "on") && !strcmp(shown[12].pause, "off"));
}

/* #358: Suggest a text box, the card above the last (Up twice from the first card reaches it), kept in region.ini, the Shell's file
 * (settings/preferences.h): ON without its line for every game (the default: no file), OFF with "region = full".
 * Enter switches it, Left is OFF and Right ON, a touch on the card switches it; each change rewrites region.ini with
 * every game's own line kept, config.ini untouched, and the status line says when it applies. An old install's box
 * for every game shows OFF; ON drops it. */
static void test_suggest_a_text_box_saved_in_region_ini(void)
{
    card(NULL);
    static const Input in[] = {ADVANCED_PAGE, PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_LEFT),
        PRESS(SCE_CTRL_RIGHT), TAP(850, 276)};
    run(in, sizeof(in) / sizeof(in[0]));
    TEST_CHECK(focused_card(7) == 9 && shows(7, "Suggest a text box"));
    TEST_CHECK(shows(9, "Saved. It applies when the game next starts or resumes."));
    TEST_CHECK(access(VJO_PREFERENCES_PATH, F_OK) != 0);
    unsigned char mem[8192]; VjoArena a; vjo_arena_init(&a, mem, sizeof(mem));
    TEST_CHECK(vjo_preferences_suggest(&a) == 0);
}

/* model_size.c on its own: the card of a parsed config.ini, its status line, the sizes' lines. */
static void test_model_size_card_and_size_lines(void)
{
    /* every config.ini engine value, Google Lens's too, shows the local engine's card */
    static const char *const values[] = {"vocr", "lens", "vocr2", "meiki", "ncnn", "other"};
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        char ini[64], line[120];
        VjoConfig cfg;
        VjoModelSizeCard c;
        vjo_config_defaults(&cfg);
        snprintf(ini, sizeof(ini), "ocr_backend = %s\n", values[i]);
        vjo_config_parse(&cfg, ini, strlen(ini));
        card(NULL);
        vjo_model_size_card(&cfg, &c);
        TEST_CHECK_(c.tier == VJO_VOCR_TIER_NORMAL && c.model && c.found[VJO_VOCR_TIER_NORMAL], "%s", values[i]);
        TEST_CHECK_(vjo_model_size_status(&c, line, sizeof(line)) == 0 && !strcmp(line, NORMAL_STATUS), "%s: %s",
                    values[i], line);
    }
    /* each size's line from its model's metadata; none outside the three */
    char line[32];
    for (int t = 1; t <= 3; t++) {
        TEST_CHECK(vjo_model_size_detail(t, line, sizeof(line)) > 0 && !strcmp(line, DETAILS[t - 1]));
        TEST_MSG("tier %d: %s", t, line);
    }
    TEST_CHECK(vjo_model_size_detail(0, line, sizeof(line)) == 0 && !line[0]);
    TEST_CHECK(vjo_model_size_detail(4, line, sizeof(line)) == 0 && !line[0]);
}

/* The app leaves by ending its process, never through vita2d_fini: as a system
 * application its libvita2d leaves the third display buffer unset, and
 * vita2d_fini clears it (settings/main.c). Both ways out: Cancel (code 0)
 * and a font that does not load (code 1, before the first frame). Every other
 * run checks the same through run_to. */
static void test_exit_ends_the_process_without_vita2d_fini(void)
{
    static const Input in[] = {OCR_PAGE};
    card(NULL);
    TEST_CHECK(run(in, 2) >= 2);
    font_fails = 1;
    TEST_CHECK(run_to(in, 2, 1) == 0);
    font_fails = 0;
}

/* ---- the Rubi app against the overlay plugins (settings/app_check.c) ---- */
#define BANNER_RED OFF
static void stamp(char *out, size_t cap, int api, const char *build)
{
    snprintf(out, cap, "vjoapp1 api=%d build=%s", api, build);
}
/* The banner's two rows (settings/main.c: baselines 24 and 46), joined; "" when none is drawn. */
static void banner_drawn(int f, char *out, size_t cap)
{
    const Text *a = text_at(f, 24, 0, 960), *b = text_at(f, 46, 0, 960);
    int red = 0;
    for (int i = 0; i < shown[f].rects; i++)
        red |= shown[f].rect[i].y == 4 && shown[f].rect[i].h == 46 && shown[f].rect[i].w == 960 &&
               shown[f].rect[i].color == BANNER_RED;
    snprintf(out, cap, "%s%s%s", red && a ? a->text : "", red && a && b ? " " : "", red && b ? b->text : "");
}
static void app_card(const char *own, const char *record, const char *status)
{
    card(NULL);
    unlink(VJO_APP_STAMP_OWN); unlink(VJO_APP_STAMP_PACKAGE); unlink(VJO_APP_STATUS_PATH);
    if (own) put_file(VJO_APP_STAMP_OWN, own);
    if (record) put_file(VJO_APP_STAMP_PACKAGE, record);
    if (status) put_file(VJO_APP_STATUS_PATH, status);
}

static void test_banner_text_says_which_side_is_older(void)
{
    char own[64], newer[64], older[64], other[64], out[192];
    const int api = VJO_API_VERSION;
    stamp(own, sizeof(own), api, "0123456789abcdef");
    stamp(newer, sizeof(newer), api + 1, "1111111111111111");
    stamp(older, sizeof(older), api - 1, "2222222222222222");
    stamp(other, sizeof(other), api, "3333333333333333");
    char st_same[64], st_newer[64], st_older[64];
    snprintf(st_same, sizeof(st_same), "bootstrap: shell API %d entered\nboot guard: boot 1 normal", api);
    snprintf(st_newer, sizeof(st_newer), "bootstrap: shell API %d entered", api + 1);
    snprintf(st_older, sizeof(st_older), "bootstrap: shell API %d entered", api - 1);
    /* nothing says otherwise: no banner */
    TEST_CHECK(!vjo_app_banner_text(api, own, own, st_same, out, sizeof(out)) && !out[0]);
    TEST_CHECK(!vjo_app_banner_text(api, NULL, NULL, NULL, out, sizeof(out)) && !out[0]);
    TEST_CHECK(!vjo_app_banner_text(api, own, NULL, NULL, out, sizeof(out)));
    TEST_CHECK(!vjo_app_banner_text(api, NULL, other, st_same, out, sizeof(out))); /* no stamp of its own: the API decides */
    /* the plugins installed came with another app */
    TEST_CHECK(vjo_app_banner_text(api, own, newer, st_same, out, sizeof(out)));
    TEST_CHECK(!strcmp(out, "This Rubi app is older than the installed overlay: install "
                            "ux0:data/Rubi.vpk in VitaShell."));
    TEST_CHECK(vjo_app_banner_text(api, own, older, st_older, out, sizeof(out)));
    TEST_CHECK(!strcmp(out, "The installed overlay is older than this app: run install-vita.sh from this app's "
                            "package, then reboot."));
    TEST_CHECK(vjo_app_banner_text(api, own, other, st_same, out, sizeof(out)));
    TEST_CHECK(!strcmp(out, "This Rubi app is not the installed overlay's: install ux0:data/Rubi.vpk "
                            "in VitaShell."));
    TEST_CHECK(vjo_app_banner_text(api, NULL, newer, NULL, out, sizeof(out)) && strstr(out, "is older than the installed"));
    /* the Shell plugin that last started (status.txt) is another API's */
    TEST_CHECK(vjo_app_banner_text(api, own, NULL, st_newer, out, sizeof(out)));
    char want[192];
    snprintf(want, sizeof(want), "This Rubi app (API %d) is older than the overlay that last started (API %d): install "
             "ux0:data/Rubi.vpk in VitaShell.", api, api + 1);
    TEST_CHECK(!strcmp(out, want));
    TEST_CHECK(vjo_app_banner_text(api, own, own, st_older, out, sizeof(out)));
    snprintf(want, sizeof(want), "The overlay that last started (API %d) is older than this app: reboot the Vita to load the "
             "one installed with it.", api - 1);
    TEST_CHECK(!strcmp(out, want));
    TEST_CHECK(vjo_app_banner_text(api, own, NULL, st_older, out, sizeof(out)));
    snprintf(want, sizeof(want), "The overlay that last started (API %d) is older than this app (API %d): run install-vita.sh "
             "from this app's package, then reboot.", api - 1, api);
    TEST_CHECK(!strcmp(out, want));
    /* status.txt's first line only, and only that line's form */
    TEST_CHECK(vjo_app_running_api(st_same) == api && vjo_app_running_api("boot guard: x\nbootstrap: shell API 9 entered") == -1);
    TEST_CHECK(vjo_app_running_api("bootstrap: shell API x entered") == -1 && vjo_app_running_api("") == -1);
    TEST_CHECK(!vjo_app_banner_text(api, own, own, "boot guard: x\nbootstrap: shell API 1 entered", out, sizeof(out)));
    /* not a stamp: no answer from it */
    TEST_CHECK(!vjo_app_banner_text(api, own, "vjoapp1 garbage", st_same, out, sizeof(out)));
}

/* The app loop draws the banner over the title row on every page it shows, and
 * nothing else changes; with matching files the title is back. */
static void test_banner_is_drawn_on_every_page(void)
{
    char own[64], newer[64], status[64], text[192], want[192];
    static const Input in[] = {OCR_PAGE};
    stamp(own, sizeof(own), VJO_API_VERSION, "0123456789abcdef");
    stamp(newer, sizeof(newer), VJO_API_VERSION + 1, "1111111111111111");
    snprintf(status, sizeof(status), "bootstrap: shell API %d entered\n", VJO_API_VERSION + 1);
    app_card(own, newer, status); /* #101 the other way round: newer plugins, this app older */
    TEST_ASSERT(run(in, 2) == 2);
    vjo_app_banner_text(VJO_API_VERSION, own, newer, status, want, sizeof(want));
    for (int f = 0; f < 2; f++) {
        banner_drawn(f, text, sizeof(text));
        TEST_CHECK_(!strcmp(text, want), "frame %d: banner '%s'", f, text);
        TEST_CHECK(!text_at(f, 35, 0, 700)); /* the title row is the banner's */
    }
    TEST_CHECK(shows(1, "Read text") && shows(1, "Selection mode"));
    /* the files agree: the title is back and nothing is red */
    snprintf(status, sizeof(status), "bootstrap: shell API %d entered\n", VJO_API_VERSION);
    app_card(own, own, status);
    TEST_ASSERT(run(in, 2) == 2);
    for (int f = 0; f < 2; f++) {
        banner_drawn(f, text, sizeof(text));
        TEST_CHECK_(!text[0], "frame %d: banner '%s'", f, text);
        TEST_CHECK(shows(f, "Reading & dictionaries") && shows(f, "Back"));
    }
    /* app0: not readable: the installed copy of its stamp (ux0:app/VJOSET001) is the same file */
    char other[64];
    stamp(other, sizeof(other), VJO_API_VERSION, "3333333333333333");
    app_card(NULL, own, status);
    mkdir("ux0:app", 0777);
    mkdir("ux0:app/VJOSET001", 0777);
    put_file(VJO_APP_STAMP_INSTALLED, other);
    TEST_ASSERT(run(in, 2) == 2);
    banner_drawn(1, text, sizeof(text));
    TEST_CHECK_(!strcmp(text, "This Rubi app is not the installed overlay's: install ux0:data/Rubi.vpk "
                              "in VitaShell."), "banner '%s'", text);
    put_file(VJO_APP_STAMP_INSTALLED, own);
    TEST_ASSERT(run(in, 2) == 2);
    banner_drawn(1, text, sizeof(text));
    TEST_CHECK_(!text[0], "banner '%s'", text);
    unlink(VJO_APP_STAMP_INSTALLED);
    unlink(VJO_APP_STAMP_OWN); unlink(VJO_APP_STAMP_PACKAGE); unlink(VJO_APP_STATUS_PATH);
}

static void test_dictionaries_card_names_the_bundled_dictionary(void)
{
    static const Input in[] = {OCR_PAGE};
    card(NULL);
    TEST_ASSERT(run(in, 2) == 2);
    for (int f = 0; f < 2; f++)
        for (int i = 0; i < shown[f].texts; i++)
            TEST_CHECK(strcmp(shown[f].text[i].text, "Dictionaries shown"));
}

/* Two glossy destination controls from x 24 to at most 936, with centred labels and visible selection.
 * The rounded control's central rectangle supplies its full width and excludes its corner strips. */
#define TAB_Y 52
#define TAB_H 48
#define TABS 2
static const char *const TAB_LABELS[TABS] = {"Look up", "Settings"};
typedef struct { int n, x[TABS], w[TABS]; } Tabs;
static Tabs tabs_of(int f, int active)
{
    Tabs t = {0};
    for (int i = 0; i < shown[f].rects; i++) {
        const Rect *r = &shown[f].rect[i];
        if (r->y != TAB_Y + 8 || r->h != TAB_H - 16) continue;
        if (t.n < TABS) {
            t.x[t.n] = r->x;
            t.w[t.n] = r->w;
            TEST_CHECK_(r->color == (t.n == active ? SELECTED : CARD), "frame %d: tab %d colour %08X", f, t.n, r->color);
            const Text *l = text_at(f, TAB_Y + 33, r->x, r->x + r->w);
            TEST_CHECK_(l && !strcmp(l->text, TAB_LABELS[t.n]) && !l->clipped && l->x >= r->x + 10 &&
                            l->x + width(l) <= r->x + r->w - 10 && abs(2 * l->x + width(l) - (2 * r->x + r->w)) <= 1 &&
                            l->color == WHITE,
                        "frame %d: tab %d's label '%s'", f, t.n, l ? l->text : "(none)");
        }
        t.n++;
    }
    TEST_CHECK_(t.n == TABS, "frame %d: %d tabs", f, t.n);
    TEST_CHECK_(t.x[0] == 24 && t.x[TABS - 1] + t.w[TABS - 1] <= 936, "frame %d: the bar from %d to %d", f, t.x[0],
                t.x[TABS - 1] + t.w[TABS - 1]);
    for (int i = 1; i < TABS && i < t.n; i++)
        TEST_CHECK_(t.x[i] >= t.x[i - 1] + t.w[i - 1] + 8, "frame %d: tab %d at %d, after %d+%d", f, i, t.x[i],
                    t.x[i - 1], t.w[i - 1]);
    return t;
}
static void test_the_tab_bar(void)
{
    card(NULL);
    static const Input in[] = {{0, 0, 0, 0}, {0, 0, 0, 0}, TAP(200, 72), TAP(700, 72),
        OCR_PAGE, TAP(200, 72), PRESS(SCE_CTRL_SELECT), TAP(700, 72)};
    TEST_ASSERT(run(in, sizeof(in) / sizeof(in[0])) == sizeof(in) / sizeof(in[0]));
    tabs_of(0, 1); /* the app starts on Settings */
    tabs_of(2, 0); tabs_of(4, 1); tabs_of(6, 1); /* Reading is a Settings child */
    tabs_of(8, 0); tabs_of(10, 1); tabs_of(12, 1);
    TEST_CHECK(shows(0, "Games") && shows(0, "Reading & dictionaries") && shows(0, "Anki") &&
               shows(0, "Appearance") && shows(0, "Install & repair"));
    TEST_CHECK(shows(6, "Back") && shows(6, "Read text") && !shows(6, "Model size"));
    TEST_CHECK(shows(10, "Choose a category"));
}

/* libvita2d's glyph atlas keeps every glyph a font drew or measured and drops new ones once it is full: main.c draws
 * with one font and measures with another, counts the distinct characters each has had and, before a text that would
 * take one past 350, loads that one again (an empty atlas) and goes on with it; the replaced ones are freed between
 * frames, after a wait for the GPU (font_tick); log.txt says so. No font ever holds more than 350 characters, none
 * both draws and measures, and none is used once freed. The drawing font switches once a frame at most, the
 * measuring one as often as a frame's layout needs (1,000 new characters in one string: three switches). A page that
 * draws the same characters again (the Games page) never switches. */
extern int vjo_lookup_stub_glyphs, vjo_lookup_stub_measures;
static void test_full_glyph_atlases_get_empty_ones(void)
{
    static const Input idle[] = {{0, 0, 0, 0}};
    Input in[2 * 4 + 12];
    int n = 0;
    card(NULL);
    TEST_ASSERT(run(idle, 1) >= 1);
    TEST_CHECK_(session->font_loads == 2 && session->font_frees == 2 && !session->fonts_both,
                "an idle app: %d loads, %d frees, %d fonts both drawing and measuring", session->font_loads,
                session->font_frees, session->fonts_both);
    for (int i = 0; i < 1; i++) /* Select opens Look up; the stub draws and measures new characters */
        in[n++] = (Input){SCE_CTRL_SELECT, 0, 0, 0}, in[n++] = (Input){0, 0, 0, 0};
    for (int i = 0; i < 12; i++)
        in[n++] = (Input){0, 0, 0, 0};
    vjo_lookup_stub_glyphs = 100;    /* 100 new characters drawn a frame: 1,400 in 14 frames */
    vjo_lookup_stub_measures = 1000; /* 1,000 new ones measured a frame, one a call as the page does: 14,000 */
    TEST_ASSERT(run(in, n) == n);
    vjo_lookup_stub_glyphs = vjo_lookup_stub_measures = 0;
    /* drawing: 1,400 characters, 350 a font: four switches at least, one a frame at most; measuring: 14,000, two or
     * three switches each frame */
    TEST_CHECK_(session->font_loads >= 2 + 4 + 39 && session->font_frees == session->font_loads,
                "%d loads, %d frees", session->font_loads, session->font_frees);
    TEST_CHECK_(!session->font_free_in_frame && !session->font_free_unwaited && !session->dead_font_uses,
                "%d frees in a frame, %d without a wait, %d uses of a freed font", session->font_free_in_frame,
                session->font_free_unwaited, session->dead_font_uses);
    TEST_CHECK_(!session->fonts_both, "%d fonts both drew and measured", session->fonts_both);
    TEST_CHECK_(session->most_glyphs <= 350, "a font held %d characters", session->most_glyphs);
    /* four atlases at most (tools/check_settings_self.py FIXED): the two fonts, a drawing font replaced this frame,
     * and for a moment the new measuring font before the one it replaces is freed */
    TEST_CHECK_(session->most_fonts >= 3 && session->most_fonts <= 4, "%d fonts alive at once", session->most_fonts);
    char log[8192] = "";
    FILE *f = fopen("ux0:data/VitaJPOverlay/log.txt", "rb");
    if (f) {
        log[fread(log, 1, sizeof(log) - 1, f)] = '\0';
        fclose(f);
    }
    /* a line a frame with switches, the counts so far: the drawing font's first, the measuring font's three at once */
    TEST_CHECK_(strstr(log, "font: an empty glyph atlas to draw with after 350 characters (1 time)\n") &&
                    strstr(log, "font: an empty glyph atlas to measure with after 350 characters ("),
                "log.txt: %s", log);
    /* rows of 30 new characters a string, as the page measures a row: no font past 350 */
    vjo_lookup_stub_glyphs = 30;
    vjo_lookup_stub_measures = -30;
    TEST_ASSERT(run(in, n) == n);
    vjo_lookup_stub_glyphs = vjo_lookup_stub_measures = 0;
    TEST_CHECK_(session->most_glyphs <= 350, "a font held %d characters", session->most_glyphs);
    TEST_CHECK(!session->dead_font_uses && !session->fonts_both && session->font_frees == session->font_loads);
    /* the Games page alone, the same characters frame after frame: the two fonts */
    static const Input games[24];
    TEST_ASSERT(run(games, 24) == 24);
    TEST_CHECK(session->font_loads == 2 && !session->dead_font_uses);
}

/* main.c's Look up wiring (#313; the audit's M16b, M20, M28, M29): the page is shown when Select or its tab brings it
 * and told when it is left (its dictionaries' files closed), whichever page comes next; Cancel is the page's while it
 * holds it (an open earlier line); and after its keyboard, the button that closed the keyboard, still down, is no new
 * press: here Circle, which would close the app. */
#define TABS_TO_LOOKUP PRESS(SCE_CTRL_SELECT) /* Settings -> Look up */
static void test_the_look_up_page_s_wiring(void)
{
    card(NULL);
    {
        static const Input in[] = {TABS_TO_LOOKUP, PRESS(SCE_CTRL_SELECT), TAP(200, 72), TAP(700, 72),
                                   TAP(200, 72), TAP(200, 72)};
        TEST_ASSERT(run(in, sizeof(in) / sizeof(in[0])) == sizeof(in) / sizeof(in[0]));
        TEST_CHECK(session->lookup_shows == 3 && session->lookup_leaves == 2);
    }
    {
        static const Input in[] = {TABS_TO_LOOKUP, PRESS(SCE_CTRL_CIRCLE), PRESS(SCE_CTRL_CIRCLE), {0, 0, 0, 0}};
        vjo_lookup_stub_hold_cancel_to = 3;
        int frames = run(in, sizeof(in) / sizeof(in[0]));
        vjo_lookup_stub_hold_cancel_to = 0;
        TEST_CHECK(frames == 4 && session->lookup_frames == 4 && session->lookup_pressed[2] == SCE_CTRL_CIRCLE);
    }
    {
        static const Input in[] = {TABS_TO_LOOKUP, {SCE_CTRL_CIRCLE, 0, 0, 0}, {SCE_CTRL_CIRCLE, 0, 0, 0},
                                   {0, 0, 0, 0}, {0, 0, 0, 0}};
        vjo_lookup_stub_keyboard_at = 2;
        int frames = run(in, sizeof(in) / sizeof(in[0]));
        vjo_lookup_stub_keyboard_at = 0;
        TEST_CHECK(frames == sizeof(in) / sizeof(in[0]));
        TEST_CHECK(session->lookup_frames >= 3 && session->lookup_pressed[2] == 0 &&
                   session->lookup_held[2] == SCE_CTRL_CIRCLE);
    }
}

/* Host preview records: real app rectangles, text positions, colours and clipping, with the harness's substitute
 * font metrics. tools/render_theme_previews.py turns these into explicitly labelled mock screenshots. */
static void json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fputc('\\', f);
        if (c < 32) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}
static void preview_record(int frame, const char *name)
{
    const char *out = getenv("VJO_THEME_PREVIEW_DIR");
    if (!out || !*out) return;
    char path[PATH_MAX];
    TEST_ASSERT(snprintf(path, sizeof(path), "%s/%s.json", out, name) < (int)sizeof(path));
    FILE *f = fopen(path, "wb");
    TEST_ASSERT(f != NULL);
    const Frame *v = &shown[frame];
    fprintf(f, "{\"clear\":%u,\"rects\":[", v->clear);
    for (int k = 0; k < v->rects; k++) {
        const Rect *r = &v->rect[k];
        fprintf(f, "%s[%d,%d,%d,%d,%u,%d,%d,%d,%d,%d]", k ? "," : "", r->x, r->y, r->w, r->h, r->color,
                r->clipped, r->clip_top, r->clip_bottom, r->clip_left, r->clip_right);
    }
    fprintf(f, "],\"texts\":[");
    for (int k = 0; k < v->texts; k++) {
        const Text *t = &v->text[k];
        fprintf(f, "%s[%d,%d,%u,%.6f,%d,%d,%d,%d,%d,", k ? "," : "", t->x, t->y, t->color, t->scale,
                t->clipped, t->clip_top, t->clip_bottom, t->clip_left, t->clip_right);
        json_string(f, t->text);
        fputc(']', f);
    }
    TEST_ASSERT(fprintf(f, "]}\n") > 0 && fclose(f) == 0);
}

static VjoAppPrefs app_choices(void)
{
    VjoAppPrefs p;
    unsigned char mem[64 * 1024];
    VjoArena a;
    vjo_arena_init(&a, mem, sizeof(mem));
    TEST_ASSERT(vjo_app_prefs_load(&p, &a) >= 0);
    return p;
}
#define APPEARANCE_PAGE TAP(200, 325)
extern int vjo_app_worker_stub_delay, vjo_app_worker_stub_fail_start;
static void test_theme_while_save_waits(void)
{
    card(NULL);
    put_file(VJO_APP_PREFS_PATH, "; keep\nanki_card_type = sentence\n");
    vjo_app_worker_stub_delay = 1000; /* storage does not finish during any scripted frame */
    static const Input keys[] = {APPEARANCE_PAGE, PRESS(SCE_CTRL_RIGHT), PRESS(SCE_CTRL_RIGHT),
        PRESS(SCE_CTRL_LEFT), PRESS(SCE_CTRL_RIGHT), PRESS(SCE_CTRL_SELECT)};
    run(keys, sizeof(keys) / sizeof(keys[0]));
    TEST_CHECK(shown[3].clear == vjo_app_theme(VJO_THEME_MINT)->bg && shows(3, "Saving colour scheme"));
    TEST_CHECK(shown[5].clear == vjo_app_theme(VJO_THEME_PAPER)->bg && shows(5, "Saving colour scheme"));
    TEST_CHECK(shown[7].clear == vjo_app_theme(VJO_THEME_MINT)->bg);
    TEST_CHECK(shown[9].clear == vjo_app_theme(VJO_THEME_PAPER)->bg);
    TEST_CHECK(shown[11].clear == vjo_app_theme(VJO_THEME_PAPER)->bg && shows(11, "Look up"));
    /* Exit waits for both the in-flight save and the latest choice; unrelated keys survive. */
    VjoAppPrefs p = app_choices();
    TEST_CHECK(p.colour_scheme == VJO_THEME_PAPER && p.anki_card_type == 1);
    vjo_app_worker_stub_delay = 0;
    /* A thread that cannot start rolls back and allows another choice. */
    vjo_app_worker_stub_fail_start = 1;
    static const Input choose[] = {APPEARANCE_PAGE, PRESS(SCE_CTRL_LEFT)};
    run(choose, sizeof(choose) / sizeof(choose[0]));
    TEST_CHECK(shown[3].clear == vjo_app_theme(VJO_THEME_PAPER)->bg && shows(3, "Could not start save"));
    TEST_CHECK(app_choices().colour_scheme == VJO_THEME_PAPER);
    vjo_app_worker_stub_fail_start = 0;
}

static void test_app_colour_schemes(void)
{
    static const Input keys[] = {APPEARANCE_PAGE, PRESS(SCE_CTRL_LEFT), PRESS(SCE_CTRL_RIGHT),
        PRESS(SCE_CTRL_RIGHT), PRESS(SCE_CTRL_RIGHT), PRESS(SCE_CTRL_CROSS), PRESS(SCE_CTRL_CROSS)};
    card(NULL);
    put_file(VJO_APP_PREFS_PATH, "; keep this\nanki_card_type = sentence\nfuture_key = yes\n");
    TEST_ASSERT(run(keys, sizeof(keys) / sizeof(keys[0])) == sizeof(keys) / sizeof(keys[0]));
    static const int frames[] = {1, 3, 5, 7, 9, 11, 13};
    static const int choices[] = {VJO_THEME_DEFAULT, VJO_THEME_DEFAULT, VJO_THEME_MINT, VJO_THEME_PAPER,
                                 VJO_THEME_PAPER, VJO_THEME_DEFAULT, VJO_THEME_MINT};
    for (int i = 0; i < 7; i++) {
        int f = frames[i];
        const VjoAppTheme *t = vjo_app_theme(choices[i]);
        TEST_CHECK(shown[f].clear == t->bg && shows(f, "Selected") && shows(f, "Restart your game"));
        const Text *selected_label = text_at(f, 112 + 245, 24 + choices[i] * 312, 24 + choices[i] * 312 + 288);
        TEST_CHECK(selected_label && !strcmp(selected_label->text, "Selected") && selected_label->color == t->bg);
        TEST_CHECK(shown[f].rect[0].color == t->accent); /* the shared header */
    }
    preview_record(1, "default-picker"); preview_record(5, "mint-picker"); preview_record(7, "paper-picker");
    VjoAppPrefs p = app_choices();
    TEST_CHECK(p.colour_scheme == VJO_THEME_MINT && p.anki_card_type == 1);
    char bytes[512];
    FILE *f = fopen(VJO_APP_PREFS_PATH, "rb");
    TEST_ASSERT(f != NULL);
    size_t n = fread(bytes, 1, sizeof(bytes) - 1, f); bytes[n] = 0; fclose(f);
    TEST_CHECK(strstr(bytes, "; keep this\n") && strstr(bytes, "future_key = yes\n"));
    /* A fresh process starts in the saved palette, on the Games page. */
    static const Input idle[] = {{0, 0, 0, 0}};
    run(idle, 1);
    TEST_CHECK(shown[0].clear == vjo_app_theme(VJO_THEME_MINT)->bg);
    TEST_CHECK(shown[0].rect[0].color == vjo_app_theme(VJO_THEME_MINT)->accent);
    /* Touch a palette card; touch gaps and outside the cards do nothing. */
    static const Input taps[] = {APPEARANCE_PAGE, TAP(24 + 2 * 312 + 120, 112 + 100),
        TAP(24 + 288 + 5, 112 + 100), TAP(959, 112 + 100), TAP(80, 420)};
    run(taps, sizeof(taps) / sizeof(taps[0]));
    TEST_CHECK(app_choices().colour_scheme == VJO_THEME_PAPER);
    for (int i = 2; i < 10; i++) TEST_CHECK(shown[i].clear == vjo_app_theme(VJO_THEME_PAPER)->bg);
    /* The same settings screen in each palette, after a fresh launch. */
    for (int i = 0; i < VJO_THEMES; i++) {
        char ini[64];
        snprintf(ini, sizeof(ini), "colour_scheme = %s\n", vjo_app_prefs_value("colour_scheme", i));
        put_file(VJO_APP_PREFS_PATH, ini);
        run(idle, 1);
        char home_name[48];
        snprintf(home_name, sizeof(home_name), "%s-home", vjo_app_prefs_value("colour_scheme", i));
        preview_record(0, home_name);
        static const Input settings[] = {OCR_PAGE};
        run(settings, sizeof(settings) / sizeof(settings[0]));
        TEST_CHECK(shown[1].clear == vjo_app_theme(i)->bg);
        preview_record(1, vjo_app_prefs_value("colour_scheme", i));
    }
    /* The app's palette works even if config.ini is unreadable. */
    card(NULL);
    TEST_ASSERT(mkdir(VJO_PREFERENCES_PATH, 0777) == 0);
    static const Input choose[] = {APPEARANCE_PAGE, PRESS(SCE_CTRL_RIGHT)};
    run(choose, sizeof(choose) / sizeof(choose[0]));
    TEST_CHECK(app_choices().colour_scheme == VJO_THEME_MINT);
    TEST_ASSERT(rmdir(VJO_PREFERENCES_PATH) == 0);
    /* A failed publication shows an error and keeps the saved palette. */
    card(NULL);
    put_file(VJO_APP_PREFS_PATH, "colour_scheme = mint\n");
    app_rename_fails = 1;
    run(choose, sizeof(choose) / sizeof(choose[0]));
    TEST_CHECK(shown[3].clear == vjo_app_theme(VJO_THEME_MINT)->bg);
    TEST_CHECK(shows(3, "Save not confirmed"));
    TEST_CHECK(app_choices().colour_scheme == VJO_THEME_MINT);
    /* An unreadable app.ini is never overwritten, and Start can retry once it is repaired. */
    card(NULL);
    TEST_ASSERT(mkdir(VJO_APP_PREFS_PATH, 0777) == 0);
    run(choose, sizeof(choose) / sizeof(choose[0]));
    TEST_CHECK(shows(3, "Cannot read app.ini") && shows(3, "Unavailable"));
    TEST_ASSERT(rmdir(VJO_APP_PREFS_PATH) == 0);
    put_file(VJO_APP_PREFS_PATH ".bak", "colour_scheme = paper\n");
    run(idle, 1);
    TEST_CHECK(shown[0].clear == vjo_app_theme(VJO_THEME_PAPER)->bg);
}

/* Real menu routes, no settings changes from opening a category; Back returns through the hierarchy. */
static void test_settings_navigation(void)
{
    card(NULL);
    static const Input keys[] = {PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_CROSS), PRESS(SCE_CTRL_CIRCLE),
        PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_CROSS), PRESS(SCE_CTRL_CIRCLE), PRESS(SCE_CTRL_DOWN),
        PRESS(SCE_CTRL_CROSS), PRESS(SCE_CTRL_CIRCLE), PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_CROSS),
        PRESS(SCE_CTRL_CIRCLE), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP),
        PRESS(SCE_CTRL_CROSS), PRESS(SCE_CTRL_CIRCLE)};
    TEST_CHECK(run(keys, sizeof(keys) / sizeof(keys[0])) == sizeof(keys) / sizeof(keys[0]));
    TEST_CHECK(shows(2, "Read text") && !shows(2, "Model size") && shows(4, "Choose a category"));
    TEST_CHECK(session->anki_shows == 1 && session->anki_pressed == 0);
    TEST_CHECK(shows(14, "Selected") && shows(16, "Choose a category"));
    TEST_CHECK(session->install_shows == 1 && session->install_pressed == 0);
    TEST_CHECK(shows(32, "Default:") && shows(34, "Choose a category"));
    TEST_CHECK(access(VJO_PREFERENCES_PATH, F_OK) != 0 && access(VJO_APP_PREFS_PATH, F_OK) != 0);
    /* Touch gaps do nothing; the basic screen keeps hook controls behind Advanced. */
    card(NULL);
    static const Input touch[] = {TAP(200, 172), TAP(479, 72), OCR_PAGE, PRESS(SCE_CTRL_UP),
        PRESS(SCE_CTRL_CROSS | SCE_CTRL_RIGHT), PRESS(SCE_CTRL_CIRCLE), TAP(40, 525), TAP(200, 325), TAP(40, 525)};
    run(touch, sizeof(touch) / sizeof(touch[0]));
    TEST_CHECK(shows(1, "Choose a category") && shows(3, "Choose a category"));
    TEST_CHECK(shows(5, "Read text") && !shows(5, "Model size"));
    TEST_CHECK(shows(9, "Text source") && !shown[9].source[0]); /* Enter+Right opens, never sets Hooks */
    TEST_CHECK(shows(11, "Read text") && !shows(11, "Trusted hooks"));
    TEST_CHECK(shows(13, "Choose a category") && shows(15, "Selected") && shows(17, "Choose a category"));
    preview_record(9, "reading-advanced");
    /* An incomplete installation still opens its page, and cannot be left while its worker runs. */
    card(NULL);
    vjo_install_stub_open = 1;
    vjo_install_stub_busy_to = 6;
    static const Input busy[] = {PRESS(SCE_CTRL_SELECT), TAP(40, 525), PRESS(SCE_CTRL_CIRCLE), PRESS(SCE_CTRL_CIRCLE)};
    run(busy, sizeof(busy) / sizeof(busy[0]));
    TEST_CHECK(session->lookup_shows == 0 && session->install_frames == 6 && shows(7, "Choose a category"));
    /* Cancel owned by an active Anki job is delivered to that job first. */
    card(NULL);
    vjo_anki_stub_hold_cancel_to = 4;
    static const Input anki[] = {TAP(200, 265), PRESS(SCE_CTRL_CIRCLE), PRESS(SCE_CTRL_CIRCLE)};
    run(anki, sizeof(anki) / sizeof(anki[0]));
    TEST_CHECK(session->anki_frames == 4 && (session->anki_pressed & SCE_CTRL_CIRCLE) && shows(5, "Choose a category"));
}

/* The walkthrough captures both scroll positions so every usual and advanced reading option is visible. */
static void test_settings_walkthrough(void)
{
    if (!getenv("VJO_THEME_PREVIEW_DIR")) return;
    card(NULL);
    static const Input home[] = {{0, 0, 0, 0}};
    run(home, 1); preview_record(0, "walk-home");
    static const Input basic[] = {OCR_PAGE};
    run(basic, sizeof(basic) / sizeof(basic[0])); preview_record(1, "walk-reading-1-usual");
    TEST_CHECK(shows(1, "Read text") && shows(1, "Selection mode") && !shows(1, "Model size"));
    static const Input advanced[] = {ADVANCED_PAGE, PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_DOWN),
        PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_DOWN)};
    run(advanced, sizeof(advanced) / sizeof(advanced[0]));
    preview_record(3, "walk-advanced-1-model"); preview_record(11, "walk-advanced-2-hooks");
    static const Input bottom[] = {ADVANCED_PAGE, PRESS(SCE_CTRL_UP)};
    run(bottom, sizeof(bottom) / sizeof(bottom[0])); preview_record(5, "walk-advanced-3-textbox");
    TEST_CHECK(focused_card(5) == 10 && card_top(5, 8) >= OCR_Y && card_top(5, 9) >= OCR_Y);
    static const Input appearance[] = {APPEARANCE_PAGE};
    run(appearance, sizeof(appearance) / sizeof(appearance[0])); preview_record(1, "walk-appearance");
}

TEST_LIST = {
    {"settings_walkthrough", test_settings_walkthrough},
    {"settings_navigation", test_settings_navigation},
    {"app_colour_schemes", test_app_colour_schemes},
    {"theme stays responsive during a held save", test_theme_while_save_waits},
    {"the_tab_bar_six_tabs_fit_their_labels_and_their_touches", test_the_tab_bar},
    {"the_look_up_page_s_wiring_show_leave_cancel_and_the_keyboard", test_the_look_up_page_s_wiring},
    {"full_glyph_atlases_get_empty_ones", test_full_glyph_atlases_get_empty_ones},
    {"one_engine_VOCR_its_model_size_on_top_no_lens", test_one_engine_its_model_size_on_top},
    {"a_tap_in_the_cards_top_band_focuses_it_and_chooses_nothing", test_the_cards_top_band_focuses_it},
    {"exit_ends_the_process_without_vita2d_fini", test_exit_ends_the_process_without_vita2d_fini},
    {"model_size_by_touch_and_keys_saved_as_vocr_tier_the_status_names_its_model", test_model_size_touch_keys_and_save},
    {"engine_line_vocr_Lens_VOCR2_Meiki_ncnn_shows_the_size_and_next_save_drops_it", test_engine_line_shows_the_size_and_next_save_drops_it},
    {"status_follows_the_configured_models_and_files", test_status_follows_the_configured_models},
    {"unreadable_config_selects_and_saves_nothing", test_unreadable_config_selects_and_saves_nothing},
    {"a_saved_choice_says_when_it_applies_with_or_without_a_game", test_a_saved_choice_says_when_it_applies},
    {"read_text_and_speed_boost_first_run_auto_and_on_below_the_model_size", test_read_text_and_speed_boost_defaults},
    {"text_source by keys and touch, saved as text_source (API 31)", test_text_source_by_keys_and_touch},
    {"Vertical text by keys and touch, saved as ocr_vertical (API 47)", test_vertical_text_by_keys_and_touch},
    {"Pause the game to read by keys and touch, saved as ocr_pause (API 55)", test_pause_card_by_keys_and_touch},
    {"the OCR-skipping switches by keys and touch, saved as ocr_text_gate and hook_trust (API 43)",
     test_ocr_skip_switches_by_keys_and_touch},
    {"read_text_and_speed_boost_by_keys_and_touch_saved_as_ocr_mode_and_ocr_boost_clock",
     test_read_text_and_speed_boost_by_keys_and_touch},
    {"the_page_scrolls_a_card_at_a_time_and_never_draws_into_the_status_line", test_the_page_scrolls_a_card_at_a_time},
    {"read_text_names_the_buttons_that_open_the_overlay", test_read_text_names_the_buttons_that_open_the_overlay},
    {"selection_cards_keep_their_touch_targets", test_scrolled_cards_keep_their_touch_targets},
    {"removed_dictionary_count_has_no_control", test_removed_dictionary_count_has_no_control},
    {"model_size_card_and_size_lines", test_model_size_card_and_size_lines},
    {"banner_text_says_which_side_is_older_and_what_to_do", test_banner_text_says_which_side_is_older},
    {"banner_is_drawn_over_the_title_on_every_page_or_not_at_all", test_banner_is_drawn_on_every_page},
    {"dictionaries_card_names_the_bundled_dictionary_by_its_title", test_dictionaries_card_names_the_bundled_dictionary},
    {"Suggest a text box, saved in region.ini (#358)", test_suggest_a_text_box_saved_in_region_ini},
    {NULL, NULL}
};
#else
/* A later package (test_vocr6_models.h): generation VOCR6's Small and Normal pinned as its
 * tiers, its Large kept from VOCR5. The status line names each size's own model. */
static const char *const DETAILS6[3] = {"0.9 s, 1.1 MB", "1.0 s, 1.8 MB", "2.6 s, 2.9 MB"};
static void test_later_generation_is_named_from_its_metadata(void)
{
    static const Input in[] = {OCR_PAGE};
    card(NULL);
    run_model(in, 2);
    check_status(1, "Normal: default, balanced. VOCR6-N found.", DIM);
    check_sizes(1, VJO_VOCR_TIER_NORMAL, SIZES, DETAILS6);
    /* the Large size's model is the earlier generation's: the line names that one */
    card("vocr_tier = large\n");
    run_model(in, 2);
    check_status(1, "Large: most accurate. VOCR5-L found.", DIM);
    check_sizes(1, VJO_VOCR_TIER_LARGE, SIZES, DETAILS6);
    remove_model("VOCR5_L_w8.vocr");
    run_model(in, 2);
    check_status(1, "VOCR5-L missing. Reinstall the package.", OFF);
    check_no_fallback_hint(2); /* VOCR6-N would run: the card never says so */
    /* the same app with an older config.ini: its model's size, this package's model of it */
    card("vocr_model = VOCR3_H15_w8.vocr\nvocr_fallback_model = VOCR3_FL10_w8.vocr\n");
    run_model(in, 2);
    check_status(1, "Normal: default, balanced. VOCR6-N found.", DIM);
    check_sizes(1, VJO_VOCR_TIER_NORMAL, SIZES, DETAILS6);
    card("vocr_model = VOCR4_W20_w8.vocr\n");
    run_model(in, 2);
    check_status(1, "Large: most accurate. VOCR5-L found.", DIM);
}
/* Nothing of the chosen size or below on the card (a partly uploaded package):
 * nothing runs; the chosen size's model is named missing, never another one. */
static void test_mixed_generations_name_the_model_that_loads(void)
{
    static const Input in[] = {OCR_PAGE};
    card("vocr_tier = normal\n");
    remove_model("VOCR6_N_w8.vocr");
    run_model(in, 2);
    check_status(1, "VOCR6-N missing. Reinstall the package.", OFF); /* VOCR6-S loads */
    check_no_fallback_hint(2);
    put_model("VOCR6_N_w8.vocr", 1500000 - 32768); /* one 32 KiB piece short */
    remove_model("VOCR6_S_w8.vocr");
    run_model(in, 2);
    check_status(1, "VOCR6-N missing. Reinstall the package.", OFF);
    check_no_fallback_hint(2);
    /* its file back: it runs again */
    put_model("VOCR6_N_w8.vocr", 1500000);
    run_model(in, 2);
    check_status(1, "Normal: default, balanced. VOCR6-N found.", DIM);
    /* Large chosen, its VOCR5 file and Normal's missing: the chosen size's model named missing */
    card("vocr_tier = large\n");
    remove_model("VOCR5_L_w8.vocr");
    remove_model("VOCR6_N_w8.vocr");
    run_model(in, 2);
    check_status(1, "VOCR5-L missing. Reinstall the package.", OFF);
    check_no_fallback_hint(2);
}
TEST_LIST = {
    {"a_later_generation_VOCR6_is_named_from_its_pinned_metadata_without_app_changes", test_later_generation_is_named_from_its_metadata},
    {"VOCR6_with_an_earlier_Large_names_the_size_s_model_missing_and_never_another", test_mixed_generations_name_the_model_that_loads},
    {NULL, NULL}
};
#endif

int main(int argc, char **argv)
{
    if (!vjo_test_tmpdir("settings-app")) return 2; /* each test's card in it, all removed at the end */
    return acutest_main(argc, argv);
}
