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
#define ACCENT RGBA8(91, 220, 207, 255)
#define SELECTED RGBA8(35, 56, 70, 255)
#define CARD RGBA8(21, 31, 44, 255)
#define WHITE RGBA8(238, 244, 255, 255)
#define BG_COLOR RGBA8(9, 15, 23, 255)
#define DIM RGBA8(193, 207, 222, 255)
#define OFF RGBA8(244, 160, 167, 255)
#define MODEL_DIR "ux0:data/VitaJPOverlay/ocr"

/* ---- storage: the SDK's file calls over real files (fakes/sceio_posix.h) ---- */
#define SCEIO_OWN (SCEIO_OPEN | SCEIO_CLOSE)
#include "fakes/sceio_posix.h"
static const char *close_fails; /* a file whose sceIoClose reports an error */
static int close_fails_fd = -1;
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
typedef struct { int x, y; unsigned color; float scale; char text[128]; int clipped, clip_top, clip_bottom; } Text;
typedef struct { int x, y, w, h; unsigned color; int clipped, clip_top, clip_bottom; } Rect;
typedef struct {
    Text text[160]; int texts;
    Rect rect[96]; int rects;
    char backend[16]; /* config.ini's ocr_backend value after this frame ("" without the line) */
    char tier[16];    /* its vocr_tier value */
    char mode[16];    /* its ocr_mode value */
    char boost[16];   /* its ocr_boost_clock value */
    char highlight[16]; /* its highlight_mode value */
    char results[16];   /* its local_dictionary_results value */
    char source[16];    /* API 31: its text_source value */
    char gate[16];      /* API 43: its ocr_text_gate value */
    char trust[16];     /* API 43: its hook_trust value */
    char vertical[16];  /* API 47: its ocr_vertical value */
    char ontext[16];    /* API 46: its overlay_on_text value */
    char pause[16];     /* API 55: its ocr_pause value */
    int model_line;   /* it still has an older build's vocr_model or vocr_fallback_model line */
    char region[96];  /* #358: region.ini's text after this frame ("-" without the file) */
} Frame;
/* Each run is one process of the app, as on the console: settings/main.c keeps
 * its state in statics, so it runs in a forked child, and the frames it shows
 * come back through shared memory. */
typedef struct { int frames, exited, main_rc, finis, font_loads, font_frees, font_free_in_frame, font_free_unwaited,
                 dead_font_uses, most_glyphs, fonts_both, most_fonts;
                 int lookup_shows, lookup_leaves, lookup_frames; uint32_t lookup_pressed[16], lookup_held[16];
                 Frame shown[MAX_FRAMES]; } Session;
/* settings_stubs/lookup_page_stub.c's records and controls (#313: main.c's Look up wiring) */
extern int vjo_lookup_stub_shows, vjo_lookup_stub_leaves, vjo_lookup_stub_frames, vjo_lookup_stub_keyboard_at,
           vjo_lookup_stub_hold_cancel_to;
extern uint32_t vjo_lookup_stub_pressed[16], vjo_lookup_stub_held[16];
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
    pad->buttons = frame < script_frames && frame < MAX_FRAMES - 1 ? script[frame].buttons : SCE_CTRL_CIRCLE;
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
        saved_value("local_dictionary_results", shown[frame].results);
        saved_value("text_source", shown[frame].source);
        saved_value("ocr_text_gate", shown[frame].gate);
        saved_value("hook_trust", shown[frame].trust);
        saved_value("ocr_vertical", shown[frame].vertical);
        saved_value("overlay_on_text", shown[frame].ontext);
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
void vita2d_set_clear_color(unsigned int color) { (void)color; }
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
void vita2d_clear_screen(void) {}
void vita2d_wait_rendering_done(void) { waited = 1; }
/* libvita2d keeps the last clip rectangle and applies it while clipping is on. */
static int clipping, clip_top, clip_bottom;
void vita2d_enable_clipping(void) { clipping = 1; }
void vita2d_disable_clipping(void) { clipping = 0; }
void vita2d_set_clip_rectangle(int x_min, int y_min, int x_max, int y_max)
{ (void)x_min; (void)x_max; clip_top = y_min; clip_bottom = y_max; }
void vita2d_draw_rectangle(float x, float y, float w, float h, unsigned int color)
{
    if (drawing.rects < (int)(sizeof(drawing.rect) / sizeof(drawing.rect[0])))
        drawing.rect[drawing.rects++] = (Rect){(int)x, (int)y, (int)w, (int)h, color, clipping, clip_top, clip_bottom};
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
    return session->frames;
}
static int run(const Input *inputs, int n) { return run_to(inputs, n, 0); }

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
#define OCR_PAGE PRESS(SCE_CTRL_SELECT) /* Games -> OCR & lookup, the page after the first */
#define CARD_TITLE TAP(100, 140)  /* the size row's title: focus only */

static const char *const SIZES[3] = {"Small", "Normal", "Large"};
/* config.ini's ocr_backend after an app save: none. The engine is no setting
 * (VOCR is the only one), so a save drops the line (core/config.h
 * vjo_config_removed_line). */
#define SAVED_BACKEND ""

#ifndef VJO_VOCR_MODEL_INFO_TEST
#define CARDS 13 /* the page's cards, top to bottom, by their titles */
static const char *const CARD_NAMES[CARDS] = {"Model size", "Read text", "Speed boost while reading (444 MHz)",
                                              "Japanese characters only", "Highlight", "Dictionaries shown",
                                              "Text source", /* API 31 */
                                              "Trusted hooks replace OCR", "Skip OCR on screens without text", /* API 43 */
                                              "Vertical text", /* API 47 */
                                              "Suggest a text box", /* #358 */
                                              "Overlay on the game's text", /* API 46 (test/combined) */
                                              "Pause the game to read"}; /* #355, the last */
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
        if (r->y < OCR_Y || (r->x == 24 && r->w == 912 && r->y == ROOM_END + 4)) continue; /* the header; the status box */
        if (r->clipped && r->clip_bottom < bottom) bottom = r->clip_bottom;
        TEST_CHECK_(bottom <= ROOM_END || r->y >= bottom, "frame %d: a %dx%d rectangle at %d,%d reaches %d", f, r->w,
                    r->h, r->x, r->y, bottom);
    }
    for (int i = 0; i < shown[f].texts; i++) {
        const Text *t = &shown[f].text[i];
        int bottom = t->y + 8; /* a baseline's descenders */
        if (t->y < OCR_Y || (t->x == 40 && t->y == ROOM_END + 4 + 22) || t->y == 535) continue; /* status line, hints */
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
    TEST_ASSERT(run(in, 2) == 2); /* each scripted frame drawn; Circle then exits */
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
    TEST_CHECK(text_at(1, READ_Y + 30, 0, 960) && !strcmp(text_at(1, READ_Y + 30, 0, 960)->text, "Read text"));
    TEST_CHECK(text_at(1, BOOST_Y + 30, 0, 960) &&
               !strcmp(text_at(1, BOOST_Y + 30, 0, 960)->text, "Speed boost while reading (444 MHz)"));
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
    TEST_ASSERT(run(in, 2 + 2 * N) == 2 + 2 * N);
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
    run(keys, (int)(sizeof(keys) / sizeof(keys[0])));
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
    run(in, (int)(sizeof(in) / sizeof(in[0])));
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
        run(in, (int)(sizeof(in) / sizeof(in[0])));
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
        run(in, 2);
        check_status(1, older[i].status, DIM);
        check_sizes(1, older[i].tier, SIZES, DETAILS);
        TEST_MSG("%s", older[i].ini);
    }
    /* the chosen size's file missing, or not its release size (the Large blob a byte short) */
    card("vocr_tier = large\n");
    remove_model(VJO_VOCR_TIER3_NAME);
    run(in, 2);
    check_status(1, "VOCR6-L missing. Reinstall the package.", OFF);
    put_model(VJO_VOCR_TIER3_NAME, VJO_VOCR_TIER3_BYTES - 1);
    run(in, 2);
    check_status(1, "VOCR6-L missing. Reinstall the package.", OFF);
    /* nothing at or below the chosen size: the chosen size's model, a red line */
    remove_model(VJO_VOCR_TIER2_NAME);
    remove_model(VJO_VOCR_TIER1_NAME);
    run(in, 2);
    check_status(1, "VOCR6-L missing. Reinstall the package.", OFF);
    /* a larger size's file missing does not matter to a smaller choice, unless the smaller size has
     * the same model (a size that kept the model of the size above it) */
    card("vocr_tier = normal\n");
    remove_model(VJO_VOCR_TIER3_NAME);
    run(in, 2);
    if (strcmp(VJO_VOCR_TIER2_NAME, VJO_VOCR_TIER3_NAME)) check_status(1, NORMAL_STATUS, DIM);
    else check_status(1, "VOCR6-N missing. Reinstall the package.", OFF);
    /* Normal's file missing: Normal named missing */
    card(NULL);
    remove_model(VJO_VOCR_TIER2_NAME);
    run(in, 2);
    check_status(1, "VOCR6-N missing. Reinstall the package.", OFF);
    /* a model file that cannot be closed again is not counted as found */
    card(NULL);
    close_fails = VJO_VOCR_TIER2_NAME;
    run(in, 2);
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
    run(in, 2);
    check_status(1, NORMAL_STATUS, DIM);
    check_no_fallback_hint(2);
}

/* Read text and Speed boost while reading, on a first run (no config.ini: the
 * defaults, auto and on): below the Model size card, each choice with its line,
 * a lookup card peeking at the bottom, cut at the room's end; looking saves nothing. */
static void test_read_text_and_speed_boost_defaults(void)
{
    card(NULL);
    static const Input in[] = {OCR_PAGE};
    TEST_ASSERT(run(in, 2) == 2);
    for (int f = 0; f < 2; f++) {
        check_read(f, READ_Y, VJO_OCR_AUTO, "Only when I press L + R", "press");
        check_boost(f, BOOST_Y, "ON", 1);
        check_room(f);
        TEST_CHECK_(card_top(f, 3) == BOOST_Y + CARD_H + 4, "frame %d: the next card peeks at %d", f, card_top(f, 3));
    }
    TEST_CHECK(focused_card(1) == 0 && access(VJO_PREFERENCES_PATH, F_OK) != 0);
    TEST_CHECK(!shown[1].mode[0] && !shown[1].boost[0]);
}

/* Read text by the controller (Down to it; Right the press, Left automatically,
 * Enter in turn) and by touch (a choice takes it; its edges are exact; the
 * title row, the gap and the margin only focus), saved as ocr_mode; Speed
 * boost by Enter, Left (OFF), Right (ON) and a touch anywhere on its card, saved
 * as ocr_boost_clock; every other line of config.ini kept. */
static void test_read_text_and_speed_boost_by_keys_and_touch(void)
{
    card("; mine\nocr_backend = vocr\nocr_mode = auto\nfont_size_ja = 22\nvocr_tier = small\n");
    static const Input keys[] = {OCR_PAGE, PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_RIGHT), PRESS(SCE_CTRL_RIGHT),
                                 PRESS(SCE_CTRL_LEFT), PRESS(SCE_CTRL_CROSS), PRESS(SCE_CTRL_CROSS),
                                 PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_CROSS), PRESS(SCE_CTRL_LEFT),
                                 PRESS(SCE_CTRL_RIGHT), PRESS(SCE_CTRL_LEFT), PRESS(SCE_CTRL_UP)};
    run(keys, (int)(sizeof(keys) / sizeof(keys[0])));
    static const struct { int frame; const char *mode, *boost; } want[] = {
        {2, "auto", ""},            /* Down: Read text, nothing saved */
        {4, "on_press", "on"},      /* Right: the press (saved with the boost the file had: none, on) */
        {6, "on_press", "on"},      /* Right again: no change */
        {8, "auto", "on"},          /* Left: automatically */
        {10, "on_press", "on"},     /* Enter: in turn */
        {12, "auto", "on"},
        {14, "auto", "on"},         /* Down: Speed boost */
        {16, "auto", "off"},        /* Enter: OFF */
        {18, "auto", "off"},        /* Left: OFF already */
        {20, "auto", "on"},         /* Right: ON */
        {22, "auto", "off"},        /* Left: OFF */
        {24, "auto", "off"},        /* Up: Read text again */
    };
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        int f = want[i].frame;
        TEST_CHECK_(!strcmp(shown[f].mode, want[i].mode), "frame %d: ocr_mode '%s', want %s", f, shown[f].mode,
                    want[i].mode);
        TEST_CHECK_(!strcmp(shown[f].boost, want[i].boost), "frame %d: ocr_boost_clock '%s', want '%s'", f,
                    shown[f].boost, want[i].boost);
        TEST_CHECK_(!strcmp(shown[f].tier, "small") && !strcmp(shown[f].backend, i < 1 ? "vocr" : SAVED_BACKEND),
                    "frame %d: vocr_tier '%s', ocr_backend '%s'", f, shown[f].tier, shown[f].backend);
        check_room(f);
    }
    check_read(5, READ_Y, VJO_OCR_ON_PRESS, "Only when I press L + R", "press");
    check_read(9, READ_Y, VJO_OCR_AUTO, "Only when I press L + R", "press");
    check_boost(17, BOOST_Y, "OFF", 0);
    check_boost(21, BOOST_Y, "ON", 1);
    TEST_CHECK(focused_card(3) == 1 && focused_card(15) == 2 && focused_card(25) == 1);
    {
        FILE *file = fopen(VJO_PREFERENCES_PATH, "rb");
        char text[4096];
        TEST_ASSERT(file != NULL);
        size_t n = fread(text, 1, sizeof(text) - 1, file);
        fclose(file);
        text[n] = 0;
        TEST_CHECK(!strcmp(text, "; mine\nocr_mode = auto\nfont_size_ja = 22\nvocr_tier = small\nocr_boost_clock = off\n"));
        TEST_MSG("%s", text);
    }
    /* touch: each edge tap comes when it would change the saved choice were that edge a pixel off.
     * Automatically spans x 42-474, the press 487-919, both y READ_BUTTON_Y to +53. */
    static const struct { int x, y; const char *mode; } taps[] = {
        {474, READ_BUTTON_Y, "auto"},                       /* Automatically, top-right pixel (from the press) */
        {487, READ_BUTTON_Y, "on_press"},                   /* the press, top-left pixel */
        {42, READ_BUTTON_Y + READ_BUTTON_H - 1, "auto"},    /* Automatically, bottom-left pixel */
        {919, READ_BUTTON_Y + READ_BUTTON_H - 1, "on_press"}, /* the press, bottom-right pixel */
        {475, READ_BUTTON_Y + 20, "on_press"},              /* the gap: focus only */
        {300, READ_BUTTON_Y + 20, "auto"},
        {486, READ_BUTTON_Y + 20, "auto"},                  /* the gap's other side */
        {920, READ_BUTTON_Y + 20, "auto"},                  /* right of the press */
        {600, READ_BUTTON_Y + 20, "on_press"},
        {41, READ_BUTTON_Y + 20, "on_press"},               /* left of Automatically */
        {300, READ_BUTTON_Y - 1, "on_press"},               /* above the choices: the title row */
        {300, READ_BUTTON_Y + READ_BUTTON_H, "on_press"},   /* below them: the card's margin */
        {300, READ_Y + 20, "on_press"},                     /* the title */
    };
    enum { N = sizeof(taps) / sizeof(taps[0]), BOOST_TAPS = 3 };
    static Input touch[2 + 2 * N + 2 * BOOST_TAPS] = {OCR_PAGE};
    card("ocr_mode = on_press\nfont_size_ja = 22\n");
    for (int i = 0; i < N; i++) touch[2 + 2 * i] = (Input){0, 1, taps[i].x, taps[i].y};
    /* then the Speed boost card: a tap anywhere on it switches it (none in the file: on) */
    static const int boost_taps[BOOST_TAPS][2] = {{500, BOOST_Y + 30}, {100, BOOST_Y + 60}, {900, BOOST_Y + 2}};
    static const char *const boost_after[BOOST_TAPS] = {"off", "on", "off"};
    for (int i = 0; i < BOOST_TAPS; i++) touch[2 + 2 * N + 2 * i] = (Input){0, 1, boost_taps[i][0], boost_taps[i][1]};
    TEST_ASSERT(run(touch, 2 + 2 * N + 2 * BOOST_TAPS) == 2 + 2 * N + 2 * BOOST_TAPS);
    for (int i = 0; i < N; i++) {
        int f = 2 + 2 * i;
        TEST_CHECK_(!strcmp(shown[f].mode, taps[i].mode), "tap %d at %d,%d: ocr_mode '%s', want %s", i, taps[i].x,
                    taps[i].y, shown[f].mode, taps[i].mode);
        TEST_CHECK_(focused_card(f) == 1, "tap %d: Read text focused (%d)", i, focused_card(f));
    }
    for (int i = 0; i < BOOST_TAPS; i++) {
        int f = 2 + 2 * N + 2 * i;
        TEST_CHECK_(!strcmp(shown[f].boost, boost_after[i]) && focused_card(f) == 2 && !strcmp(shown[f].mode, "on_press"),
                    "Speed boost tap %d: '%s', want %s", i, shown[f].boost, boost_after[i]);
    }
}

/* The page scrolls a card at a time: Down from the Model size to the last
 * card brings each card all into view, with the fewest cards scrolled, and Up
 * back; Up from the first card wraps to the last. A card cut at the bottom is
 * only selected by a touch (no change), then a touch on it acts. Nothing of a
 * card is ever drawn into the status line. */
static void test_the_page_scrolls_a_card_at_a_time(void)
{
    card("vocr_tier = small\njapanese_only_selection = on\n");
    static const Input in[] = {OCR_PAGE, PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_DOWN),
                               PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP),
                               PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP)};
    int frames = run(in, (int)(sizeof(in) / sizeof(in[0])));
    TEST_ASSERT(frames == (int)(sizeof(in) / sizeof(in[0])));
    /* the first card in view after each key: Down to Japanese characters only scrolls one card
     * (Read text on top), to Highlight one more (Speed boost on top), and the last card fits so */
    static const struct { int frame, focus, top; } want[] = {
        {1, 0, 0}, {3, 1, 0}, {5, 2, 0}, {7, 3, 1}, {9, 4, 2}, {11, 5, 2},
        {13, 4, 2}, {15, 3, 2}, {17, 2, 2}, {19, 1, 1}, {21, 0, 0},
        {23, 12, 9}, /* Up from the first: the last (test/combined: the drafts' cards after main's) */
    };
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        int f = want[i].frame, top = card_top(f, want[i].top), mine = card_top(f, want[i].focus);
        TEST_CHECK_(focused_card(f) == want[i].focus, "frame %d: card %d focused, want %d", f, focused_card(f),
                    want[i].focus);
        TEST_CHECK_(top == OCR_Y, "frame %d: card %d at %d, want it first", f, want[i].top, top);
        for (int c = 0; c < want[i].top; c++) /* the cards above the view are not drawn */
            TEST_CHECK_(card_top(f, c) < 0, "frame %d: card %d drawn above the view", f, c);
        int h = want[i].focus == 0 ? OCR_H : want[i].focus == 1 ? READ_H : CARD_H;
        TEST_CHECK_(mine >= OCR_Y && mine + h <= ROOM_END, "frame %d: the focused card at %d..%d", f, mine, mine + h);
        check_room(f);
        /* the scroll bar's thumb: where the view is in the 992-pixel column (310 of it shown; API 31's
         * Text source card made it 488 + 4 + 68, API 43's two switches 560 + 2 x (4 + 68), API 47's Vertical text
         * 704 + 4 + 68, #358's Suggest a text box 776 + 4 + 68) */
        static const int offset[] = {0, 98, 204, 276, 348, 420, 492, 564, 636, 708};
        int thumb = 0;
        for (int k = 0; k < shown[f].rects; k++)
            if (shown[f].rect[k].x == 942 && shown[f].rect[k].color == DIM)
                thumb = shown[f].rect[k].y == OCR_Y + 310 * offset[want[i].top] / 992 &&
                        shown[f].rect[k].h == 310 * (992 - offset[want[i].top] < 310 ? 992 - offset[want[i].top] : 310) / 992;
        TEST_CHECK_(thumb, "frame %d: the scroll bar's thumb", f);
    }
    /* the size buttons' place is Read text's title row once the page has scrolled: a tap there
     * selects Read text and chooses nothing */
    static const Input scrolled[] = {OCR_PAGE, PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_DOWN),
                                     TAP(300, 150), TAP(500, 400), TAP(300, 300)};
    card("vocr_tier = small\njapanese_only_selection = on\nhighlight_mode = word\n");
    frames = run(scrolled, (int)(sizeof(scrolled) / sizeof(scrolled[0])));
    TEST_ASSERT(frames == (int)(sizeof(scrolled) / sizeof(scrolled[0])));
    TEST_CHECK_(focused_card(9) == 1 && !strcmp(shown[9].tier, "small") && card_top(9, 0) < 0 &&
                card_top(9, 1) == OCR_Y, "frame 9: card %d focused, Read text at %d", focused_card(9), card_top(9, 1));
    /* Highlight, cut at the bottom (its top at 362, the room's end 422): selected and scrolled
     * into view (Speed boost now first), not switched */
    TEST_CHECK_(focused_card(10) == 4 && card_top(10, 2) == OCR_Y && card_top(10, 4) + CARD_H <= ROOM_END &&
                !strcmp(shown[10].highlight, "word"), "frame 10: card %d focused, highlight '%s'", focused_card(10),
                shown[10].highlight);
    /* a touch on it, all in view now, switches it */
    TEST_CHECK_(card_top(12, 4) == 256 && !strcmp(shown[12].highlight, "character") && focused_card(12) == 4,
                "frame 12: highlight '%s'", shown[12].highlight);
    for (int f = 0; f < frames; f++) check_room(f);
}

/* The lookup cards' choices once the page has scrolled (Speed boost first: Highlight at y 256, Dictionaries shown
 * at 328; audit N8): drawn there, and touched there, at their badges' exact edges. A touch elsewhere on Highlight
 * switches it, as before the page scrolled; on Dictionaries shown only a number chooses. */
#define HL_Y 256
#define DICT_Y 328
static int rect_at(int f, int x, int y, int w, int h)
{
    for (int i = 0; i < shown[f].rects; i++) {
        const Rect *r = &shown[f].rect[i];
        if (r->x == x && r->y == y && r->w == w && r->h == h) return 1;
    }
    return 0;
}
static void test_scrolled_cards_keep_their_touch_targets(void)
{
    card("highlight_mode = word\nlocal_dictionaries = a.vjdict,b.vjdict,c.vjdict\nlocal_dictionary_results = 1\n");
    static const struct { int x, y; const char *highlight, *results; } taps[] = {
        {724, HL_Y + 6, "character", "1"},   /* Character, top-left pixel */
        {707, HL_Y + 39, "word", "1"},       /* Whole word, bottom-right pixel */
        {600, HL_Y + 20, "word", "1"},       /* Whole word again: chosen, it stays (a toggle would switch it) */
        {919, HL_Y + 39, "character", "1"},  /* Character, bottom-right pixel */
        {800, HL_Y + 20, "character", "1"},  /* Character again: it stays */
        {532, HL_Y + 6, "word", "1"},        /* Whole word, top-left pixel */
        {300, HL_Y + 30, "character", "1"},  /* the title: switches */
        {715, HL_Y + 20, "word", "1"},       /* between the badges: switches */
        {636, DICT_Y + 6, "word", "3"},      /* 3, top-left pixel */
        {679, DICT_Y + 39, "word", "3"},     /* 3, bottom-right pixel */
        {680, DICT_Y + 20, "word", "3"},     /* the gap after 3: focus only */
        {684, DICT_Y + 20, "word", "4"},     /* 4, left edge */
        {919, DICT_Y + 20, "word", "8"},     /* 8, right edge */
        {920, DICT_Y + 20, "word", "8"},     /* right of 8: focus only */
        {540, DICT_Y + 5, "word", "8"},      /* above the numbers: focus only */
        {540, DICT_Y + 40, "word", "8"},     /* below them: focus only */
        {540, DICT_Y + 6, "word", "1"},      /* 1, top-left pixel */
    };
    enum { N = sizeof(taps) / sizeof(taps[0]), KEYS = 10 };
    static Input in[KEYS + 2 * N] = {OCR_PAGE, PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_DOWN),
                                     PRESS(SCE_CTRL_DOWN)}; /* Highlight focused: Speed boost on top */
    for (int i = 0; i < N; i++) in[KEYS + 2 * i] = (Input){0, 1, taps[i].x, taps[i].y};
    TEST_ASSERT(run(in, KEYS + 2 * N) == KEYS + 2 * N);
    TEST_CHECK_(card_top(9, 2) == OCR_Y && card_top(9, 4) == HL_Y && card_top(9, 5) == DICT_Y,
                "frame 9: Speed boost at %d, Highlight at %d, Dictionaries at %d", card_top(9, 2), card_top(9, 4),
                card_top(9, 5));
    TEST_CHECK(rect_at(9, 532, HL_Y + 6, 176, 34) && rect_at(9, 724, HL_Y + 6, 196, 34)); /* the badges, there */
    for (int k = 0; k < 8; k++) TEST_CHECK_(rect_at(9, 540 + k * 48, DICT_Y + 6, 44, 34), "number %d", k + 1);
    for (int i = 0; i < N; i++) {
        int f = KEYS + 2 * i;
        TEST_CHECK_(!strcmp(shown[f].highlight, taps[i].highlight) && !strcmp(shown[f].results, taps[i].results),
                    "tap %d at %d,%d: highlight '%s', results '%s'; want %s, %s", i, taps[i].x, taps[i].y,
                    shown[f].highlight, shown[f].results, taps[i].highlight, taps[i].results);
        TEST_CHECK_(focused_card(f) == (taps[i].y < DICT_Y ? 4 : 5) && card_top(f, 2) == OCR_Y,
                    "tap %d: card %d focused, Speed boost at %d", i, focused_card(f), card_top(f, 2));
        check_room(f);
    }
}

/* #418 (P10, audit-rubi-cleanup-10 F11): Dictionaries shown by its buttons (settings/main.c choose_preference): Enter
 * takes the next count, 8 then 1; Left and Right the count beside it, never past 1 or 8. */
static void test_dictionaries_shown_by_its_buttons(void)
{
    static const struct { uint32_t button; const char *results; } steps[] = {
        {SCE_CTRL_CROSS, "2"}, {SCE_CTRL_RIGHT, "3"}, {SCE_CTRL_LEFT, "2"}, {SCE_CTRL_LEFT, "1"},
        {SCE_CTRL_LEFT, "1"}, /* not past 1 */
        {SCE_CTRL_RIGHT, "2"}, {SCE_CTRL_RIGHT, "3"}, {SCE_CTRL_RIGHT, "4"}, {SCE_CTRL_RIGHT, "5"},
        {SCE_CTRL_RIGHT, "6"}, {SCE_CTRL_RIGHT, "7"}, {SCE_CTRL_RIGHT, "8"},
        {SCE_CTRL_RIGHT, "8"}, /* not past 8 */
        {SCE_CTRL_CROSS, "1"}, /* round: 8, then 1 */
    };
    enum { N = sizeof(steps) / sizeof(steps[0]), KEYS = 12 }; /* the page, then Down to Dictionaries shown */
    static Input in[KEYS + 2 * N] = {OCR_PAGE, PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_DOWN),
                                     PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_DOWN)};
    card("local_dictionaries = a.vjdict,b.vjdict,c.vjdict\nlocal_dictionary_results = 1\n");
    for (int i = 0; i < N; i++) in[KEYS + 2 * i] = (Input){steps[i].button, 0, 0, 0};
    TEST_ASSERT(run(in, KEYS + 2 * N) == KEYS + 2 * N);
    for (int i = 0; i < N; i++) {
        int f = KEYS + 2 * i;
        TEST_CHECK_(focused_card(f) == 5 && !strcmp(shown[f].results, steps[i].results),
                    "press %d (0x%x): card %d focused, results '%s'; want %s", i, (unsigned)steps[i].button,
                    focused_card(f), shown[f].results, steps[i].results);
    }
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
        check_read(1, READ_Y, strstr(triggers[i].ini, "on_press") ? VJO_OCR_ON_PRESS : VJO_OCR_AUTO, triggers[i].press,
                   triggers[i].verb);
        TEST_MSG("%s", triggers[i].ini);
    }
}

/* An unreadable config.ini: nothing is selected and nothing can be saved. */
static void test_unreadable_config_selects_and_saves_nothing(void)
{
    card(NULL);
    put_file(VJO_PREFERENCES_PATH, "ocr_backend = lens\nvocr_tier = large\n");
    TEST_ASSERT(chmod(VJO_PREFERENCES_PATH, 0) == 0);
    if (access(VJO_PREFERENCES_PATH, R_OK) == 0) { /* running as root: permissions do not apply */
        chmod(VJO_PREFERENCES_PATH, 0644);
        TEST_SKIP("the file stays readable (root)");
        return;
    }
    static const Input in[] = {OCR_PAGE, TAP(300, 150), PRESS(SCE_CTRL_RIGHT)};
    run(in, (int)(sizeof(in) / sizeof(in[0])));
    Buttons s = size_buttons(5);
    TEST_CHECK(s.count == 3 && s.rect[0].color == SELECTED && s.rect[1].color == SELECTED && s.rect[2].color == SELECTED);
    const Text *st = text_at(5, STATUS_Y, 0, 960), *title = text_at(5, SIZE_NAME_Y, 0, SIZE_X);
    TEST_CHECK(st && !strcmp(st->text, "Cannot read config.ini. Repair it before changing settings.") &&
               st->color == OFF);
    TEST_CHECK(title && title->color == DIM); /* the row's title dimmed: nothing to choose */
    /* Read text and Speed boost show no choice either, and keys and touches save nothing */
    static const Input reading[] = {OCR_PAGE, PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_RIGHT), PRESS(SCE_CTRL_CROSS),
                                    TAP(600, READ_BUTTON_Y + 20), PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_CROSS),
                                    PRESS(SCE_CTRL_RIGHT), TAP(500, BOOST_Y + 30)};
    run(reading, (int)(sizeof(reading) / sizeof(reading[0])));
    for (int f = 1; f < (int)(sizeof(reading) / sizeof(reading[0])); f++) {
        check_read(f, READ_Y, -1, "Only when I press L + R", "press");
        const Text *read_title = text_at(f, READ_Y + 30, 0, 960), *badge = text_at(f, BOOST_Y + 32, 812, 920);
        TEST_CHECK_(read_title && read_title->color == DIM && badge && !strcmp(badge->text, "--"),
                    "frame %d: Read text's title %08X, the Speed boost badge '%s'", f, read_title ? read_title->color : 0,
                    badge ? badge->text : "(none)");
    }
    const Text *why = text_at(17, ROOM_END + 4 + 22, 40, 41); /* the status line (a cut card's help is drawn there, clipped) */
    TEST_CHECK_(why && !strcmp(why->text, "Cannot read config.ini. Choices have not been changed.") && why->color == OFF,
                "the status line: '%s'", why ? why->text : "(none)");
    chmod(VJO_PREFERENCES_PATH, 0644);
    char backend[16], tier[16];
    saved_backend(backend);
    saved_value("vocr_tier", tier);
    TEST_CHECK(!strcmp(backend, "lens") && !strcmp(tier, "large")); /* the file was never written */
}

/* A saved choice says when it applies: true with no game running as beside one (the app
 * opens from the LiveArea either way). */
static void test_a_saved_choice_says_when_it_applies(void)
{
    card(NULL);
    static const Input in[] = {OCR_PAGE, TAP(SIZE_X + 2 * SIZE_PITCH + 20, SIZE_Y + 20)}; /* Large */
    run(in, (int)(sizeof(in) / sizeof(in[0])));
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
    card("; mine\nvocr_tier = small\n");
    static const Input in[] = {OCR_PAGE, PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP),
                               PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_RIGHT), PRESS(SCE_CTRL_RIGHT),
                               PRESS(SCE_CTRL_RIGHT), PRESS(SCE_CTRL_CROSS), PRESS(SCE_CTRL_LEFT),
                               PRESS(SCE_CTRL_CROSS), PRESS(SCE_CTRL_LEFT)};
    int frames = run(in, (int)(sizeof(in) / sizeof(in[0])));
    TEST_ASSERT(frames == (int)(sizeof(in) / sizeof(in[0])));
    static const struct { int frame; const char *source; } want[] = {
        {14, ""},     /* Up five times: Text source, nothing saved */
        {16, "hooks"},/* Right: Every game */
        {18, "ocr"},  /* Right: OCR only */
        {20, "ocr"},  /* Right again: no change */
        {22, "auto"}, /* Enter: the next, round to Automatic */
        {24, "auto"}, /* Left: Automatic already */
        {26, "hooks"},/* Enter */
        {28, "auto"}, /* Left */
    };
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        int f = want[i].frame;
        TEST_CHECK_(!strcmp(shown[f].source, want[i].source), "frame %d: text_source '%s', want '%s'", f,
                    shown[f].source, want[i].source);
        TEST_CHECK_(focused_card(f) == 6 && !strcmp(shown[f].tier, "small"), "frame %d: card %d", f, focused_card(f));
        check_room(f);
    }
    const Text *st = text_at(16, ROOM_END + 4 + 22, 40, 41);
    TEST_CHECK_(st && !strcmp(st->text, "Saved. It applies when a game next starts."), "the status line: '%s'",
                st ? st->text : "(none)");
    {
        FILE *file = fopen(VJO_PREFERENCES_PATH, "rb");
        char text[4096];
        TEST_ASSERT(file != NULL);
        size_t n = fread(text, 1, sizeof(text) - 1, file);
        fclose(file);
        text[n] = 0;
        TEST_CHECK(!strcmp(text, "; mine\nvocr_tier = small\ntext_source = auto\n"));
        TEST_MSG("%s", text);
    }
    /* touch, once the card is in view (Up wraps to the last card, four more reach it): each choice, then the title
     * row and the help line (focus only). The choices span x 440-591, 600-751 and 760-911, y from the card's top +
     * 6 to + 39. */
    card("text_source = ocr\n");
    static const Input taps[] = {OCR_PAGE, PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP),
                                 PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), TAP(500, 0), TAP(700, 0), TAP(911, 0),
                                 TAP(595, 0), TAP(300, 0), TAP(450, 0)};
    Input script_taps[sizeof(taps) / sizeof(taps[0])];
    memcpy(script_taps, taps, sizeof(taps));
    /* the card's top once in view: Text source first (the column's 492, since API 47's Vertical text card and
     * #358's Suggest a text box, the last, scrolled past it) */
    const int top = OCR_Y + 492 - 492;
    for (unsigned i = 16; i < sizeof(taps) / sizeof(taps[0]); i += 2) script_taps[i].y = top + 6 + 16;
    script_taps[24].y = top + 30; /* the title row: focus only */
    frames = run(script_taps, (int)(sizeof(script_taps) / sizeof(script_taps[0])));
    TEST_ASSERT(frames == (int)(sizeof(script_taps) / sizeof(script_taps[0])));
    TEST_CHECK_(card_top(14, 6) == top, "Text source at %d, want %d", card_top(14, 6), top);
    static const char *const after[] = {"auto", "hooks", "ocr", "ocr", "ocr", "auto"};
    for (unsigned i = 0; i < 6; i++)
        TEST_CHECK_(!strcmp(shown[16 + 2 * i].source, after[i]) && focused_card(16 + 2 * i) == 6,
                    "tap %u: text_source '%s', want %s", i, shown[16 + 2 * i].source, after[i]);
}

/* API 43: the two OCR-skipping switches, above the page's last three cards (Up four times from the first card reaches
 * Skip OCR on screens without text, one more Trusted hooks replace OCR). Enter switches the focused one, Left is OFF and Right
 * ON, a touch anywhere on a card in view switches it; each change saves ocr_text_gate and hook_trust together,
 * every other line kept, and the status line says it applies when the overlay next opens. A file without the lines
 * shows the defaults (trusted hooks ON, the text gate OFF: docs/vita-vn-ocr.md, its measurement) and writes nothing
 * until a change. */
static void test_ocr_skip_switches_by_keys_and_touch(void)
{
    card("; mine\nvocr_tier = small\n");
    static const Input in[] = {OCR_PAGE, PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP),
                               PRESS(SCE_CTRL_CROSS), PRESS(SCE_CTRL_RIGHT), PRESS(SCE_CTRL_LEFT), PRESS(SCE_CTRL_UP),
                               PRESS(SCE_CTRL_LEFT), PRESS(SCE_CTRL_CROSS), PRESS(SCE_CTRL_DOWN)};
    int frames = run(in, (int)(sizeof(in) / sizeof(in[0])));
    TEST_ASSERT(frames == (int)(sizeof(in) / sizeof(in[0])));
    static const struct { int frame, focus; const char *gate, *trust; } want[] = {
        {10, 8, "", ""},        /* Up three times: Skip OCR on screens without text, OFF by default, nothing saved */
        {12, 8, "on", "on"},    /* Enter: ON (both keys saved) */
        {14, 8, "on", "on"},   /* Right: ON already */
        {16, 8, "off", "on"},  /* Left: OFF */
        {18, 7, "off", "on"},  /* Up: Trusted hooks replace OCR, ON by default, nothing changes */
        {20, 7, "off", "off"}, /* Left: OFF */
        {22, 7, "off", "on"},  /* Enter: ON */
        {24, 8, "off", "on"},  /* Down: back to Skip OCR */
    };
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        int f = want[i].frame;
        TEST_CHECK_(focused_card(f) == want[i].focus && !strcmp(shown[f].gate, want[i].gate) &&
                    !strcmp(shown[f].trust, want[i].trust) && !strcmp(shown[f].tier, "small"),
                    "frame %d: card %d, ocr_text_gate '%s', hook_trust '%s'", f, focused_card(f), shown[f].gate,
                    shown[f].trust);
        check_room(f);
    }
    /* the badges say so: OFF on frame 6's Skip OCR card, ON on frame 8's */
    const Text *b2 = text_at(10, card_top(10, 8) + 32, 812, 920), *b4 = text_at(12, card_top(12, 8) + 32, 812, 920);
    TEST_CHECK_(b2 && !strcmp(b2->text, "OFF") && b4 && !strcmp(b4->text, "ON"), "badges '%s', '%s'",
                b2 ? b2->text : "(none)", b4 ? b4->text : "(none)");
    const Text *st = text_at(12, ROOM_END + 4 + 22, 40, 41);
    TEST_CHECK_(st && !strcmp(st->text, "Saved. It applies when the overlay next opens."), "the status line: '%s'",
                st ? st->text : "(none)");
    {
        FILE *file = fopen(VJO_PREFERENCES_PATH, "rb");
        char text[4096];
        TEST_ASSERT(file != NULL);
        size_t n = fread(text, 1, sizeof(text) - 1, file);
        fclose(file);
        text[n] = 0;
        TEST_CHECK(!strcmp(text, "; mine\nvocr_tier = small\nocr_text_gate = off\nhook_trust = on\n"));
        TEST_MSG("%s", text);
    }
    /* a touch on each card in view switches it (the trust card first once Up has wrapped to #358's Suggest a text
     * box: the column's 564; the gate card 636 down it) */
    card("ocr_text_gate = off\nhook_trust = off\n");
    static const Input taps[] = {OCR_PAGE, PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), TAP(300, OCR_Y + 636 - 564 + 30),
                                 TAP(860, OCR_Y + 564 - 564 + 20), TAP(500, OCR_Y + 636 - 564 + 60)};
    frames = run(taps, (int)(sizeof(taps) / sizeof(taps[0])));
    TEST_ASSERT(frames == (int)(sizeof(taps) / sizeof(taps[0])));
    TEST_CHECK_(card_top(12, 8) == OCR_Y + 636 - 564 && card_top(12, 7) == OCR_Y + 564 - 564, "cards at %d, %d",
                card_top(12, 7), card_top(12, 8));
    TEST_CHECK_(!strcmp(shown[14].gate, "on") && !strcmp(shown[14].trust, "off") && focused_card(14) == 8,
                "tap 1: '%s', '%s'", shown[14].gate, shown[14].trust);
    TEST_CHECK_(!strcmp(shown[16].gate, "on") && !strcmp(shown[16].trust, "on") && focused_card(16) == 7,
                "tap 2: '%s', '%s'", shown[16].gate, shown[16].trust);
    TEST_CHECK_(!strcmp(shown[18].gate, "off") && !strcmp(shown[18].trust, "on") && focused_card(18) == 8,
                "tap 3: '%s', '%s'", shown[18].gate, shown[18].trust);
    for (int f = 0; f < frames; f++) check_room(f);
}

/* API 47: Vertical text, the third card from the end (Up three times from the first card reaches it): Right and Left step within its three
 * choices (OFF, Automatic, ON: config.ini ocr_vertical off, auto, on), Enter takes the next one (ON to OFF), a touch
 * on a choice takes it and elsewhere on the card only focuses. Each change is config.ini's ocr_vertical, every other
 * line kept, and the status line says it applies when the overlay next opens; a file without the line shows the
 * default (VJO_VERTICAL_DEFAULT, Automatic) and writes nothing until a change. */
static void test_vertical_text_by_keys_and_touch(void)
{
    card("; mine\nvocr_tier = small\n");
    static const Input in[] = {OCR_PAGE, PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_RIGHT),
                               PRESS(SCE_CTRL_RIGHT), PRESS(SCE_CTRL_CROSS), PRESS(SCE_CTRL_CROSS), PRESS(SCE_CTRL_LEFT),
                               PRESS(SCE_CTRL_LEFT), PRESS(SCE_CTRL_CROSS)};
    int frames = run(in, (int)(sizeof(in) / sizeof(in[0])));
    TEST_ASSERT(frames == (int)(sizeof(in) / sizeof(in[0])));
    TEST_CHECK(VJO_VERTICAL_DEFAULT == VJO_VERTICAL_AUTO); /* the walk below starts at Automatic */
    static const struct { int frame; const char *mode; } want[] = {
        {8, ""},      /* Up, Up: Vertical text, nothing saved (Automatic, the default) */
        {10, "on"},    /* Right: ON */
        {12, "on"},    /* Right again: no change */
        {14, "off"},  /* Enter: the next, round to OFF */
        {16, "auto"}, /* Enter: Automatic */
        {18, "off"},  /* Left: OFF */
        {20, "off"},  /* Left again: no change */
        {22, "auto"}, /* Enter */
    };
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        int f = want[i].frame;
        TEST_CHECK_(!strcmp(shown[f].vertical, want[i].mode), "frame %d: ocr_vertical '%s', want '%s'", f,
                    shown[f].vertical, want[i].mode);
        TEST_CHECK_(focused_card(f) == 9 && !strcmp(shown[f].tier, "small"), "frame %d: card %d", f, focused_card(f));
        check_room(f);
    }
    TEST_CHECK(shows(8, "Columns where the text box holds vertical text.") &&
               shows(10, "Columns, right to left, unless the text is in rows.") && shows(14, "Rows only: horizontal text, as before."));
    const Text *st = text_at(10, ROOM_END + 4 + 22, 40, 41);
    TEST_CHECK_(st && !strcmp(st->text, "Saved. It applies when the overlay next opens."), "the status line: '%s'",
                st ? st->text : "(none)");
    {
        FILE *file = fopen(VJO_PREFERENCES_PATH, "rb");
        char text[4096];
        TEST_ASSERT(file != NULL);
        size_t n = fread(text, 1, sizeof(text) - 1, file);
        fclose(file);
        text[n] = 0;
        TEST_CHECK(!strcmp(text, "; mine\nvocr_tier = small\nocr_vertical = auto\n"));
        TEST_MSG("%s", text);
    }
    /* touch, once the card is in view (Up wraps to the last card, which brings it into view): each choice, then the
     * title row and the help line (focus only). The choices span x 440-591, 600-751 and 760-911, y from the card's
     * top + 6 to + 39. */
    card("ocr_vertical = on\n");
    static const Input taps[] = {OCR_PAGE, PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), TAP(500, 0), TAP(700, 0), TAP(911, 0), TAP(595, 0),
                                 TAP(300, 0), TAP(450, 0)};
    Input script_taps[sizeof(taps) / sizeof(taps[0])];
    memcpy(script_taps, taps, sizeof(taps));
    /* the card's top once in view: Trusted hooks first (the column's 564, #358's Suggest a text box the last),
     * Vertical text 708 down it */
    const int top = OCR_Y + 708 - 708;
    for (unsigned i = 8; i < sizeof(taps) / sizeof(taps[0]); i += 2) script_taps[i].y = top + 6 + 16;
    script_taps[16].y = top + 30; /* the title row: focus only */
    frames = run(script_taps, (int)(sizeof(script_taps) / sizeof(script_taps[0])));
    TEST_ASSERT(frames == (int)(sizeof(script_taps) / sizeof(script_taps[0])));
    TEST_CHECK_(card_top(6, 9) == top && top + CARD_H <= ROOM_END, "Vertical text at %d, want %d", card_top(6, 9), top);
    static const char *const after[] = {"off", "auto", "on", "on", "on", "off"};
    for (unsigned i = 0; i < 6; i++)
        TEST_CHECK_(!strcmp(shown[8 + 2 * i].vertical, after[i]) && focused_card(8 + 2 * i) == 9,
                    "tap %u: ocr_vertical '%s', want %s", i, shown[8 + 2 * i].vertical, after[i]);
}

/* API 55: Pause the game to read, the last card (Up from the first card reaches it): Enter switches it, Right is ON
 * and Left OFF, a touch anywhere on it switches it; each change is config.ini's ocr_pause, every other line kept, and
 * the status line says it applies when the overlay next opens. A file without the line shows OFF (the default) and
 * writes nothing until a change. */
static void test_pause_card_by_keys_and_touch(void)
{
    card("; mine\nvocr_tier = small\n");
    static const Input in[] = {OCR_PAGE, PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_CROSS), PRESS(SCE_CTRL_RIGHT),
                               PRESS(SCE_CTRL_LEFT), PRESS(SCE_CTRL_LEFT), PRESS(SCE_CTRL_CROSS)};
    int frames = run(in, (int)(sizeof(in) / sizeof(in[0])));
    TEST_ASSERT(frames == (int)(sizeof(in) / sizeof(in[0])));
    static const struct { int frame; const char *pause; } want[] = {
        {2, ""},     /* Up: Pause the game to read, OFF by default, nothing saved */
        {4, "on"},   /* Enter: ON */
        {6, "on"},   /* Right: ON already */
        {8, "off"},  /* Left: OFF */
        {10, "off"}, /* Left again: no change */
        {12, "on"},  /* Enter: ON */
    };
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        int f = want[i].frame;
        TEST_CHECK_(focused_card(f) == 12 && !strcmp(shown[f].pause, want[i].pause) && !strcmp(shown[f].tier, "small"),
                    "frame %d: card %d, ocr_pause '%s', want '%s'", f, focused_card(f), shown[f].pause, want[i].pause);
        check_room(f);
    }
    const Text *off = text_at(2, card_top(2, 12) + 32, 812, 920), *on = text_at(4, card_top(4, 12) + 32, 812, 920);
    TEST_CHECK_(off && !strcmp(off->text, "OFF") && on && !strcmp(on->text, "ON"), "badges '%s', '%s'",
                off ? off->text : "(none)", on ? on->text : "(none)");
    TEST_CHECK(shows(2, "ON: a brief pause, so an icon can't spoil a read. Draw a box."));
    const Text *st = text_at(4, ROOM_END + 4 + 22, 40, 41);
    TEST_CHECK_(st && !strcmp(st->text, "Saved. It applies when the overlay next opens."), "the status line: '%s'",
                st ? st->text : "(none)");
    {
        FILE *file = fopen(VJO_PREFERENCES_PATH, "rb");
        char text[4096];
        TEST_ASSERT(file != NULL);
        size_t n = fread(text, 1, sizeof(text) - 1, file);
        fclose(file);
        text[n] = 0;
        TEST_CHECK(!strcmp(text, "; mine\nvocr_tier = small\nocr_pause = on\n"));
        TEST_MSG("%s", text);
    }
    /* a touch anywhere on the card, once in view (Up wraps to it: Skip OCR first, the column's 636; the card 852 down
     * it), switches it */
    card("ocr_pause = on\n");
    const int top = OCR_Y + 924 - 708;
    static Input taps[] = {OCR_PAGE, PRESS(SCE_CTRL_UP), TAP(300, 0), TAP(860, 0)};
    taps[4].y = top + 30;
    taps[6].y = top + 20;
    frames = run(taps, (int)(sizeof(taps) / sizeof(taps[0])));
    TEST_ASSERT(frames == (int)(sizeof(taps) / sizeof(taps[0])));
    TEST_CHECK_(card_top(2, 12) == top && top + CARD_H <= ROOM_END, "Pause card at %d, want %d", card_top(2, 12), top);
    TEST_CHECK_(!strcmp(shown[4].pause, "off") && focused_card(4) == 12, "tap 1: '%s'", shown[4].pause);
    TEST_CHECK_(!strcmp(shown[6].pause, "on") && focused_card(6) == 12, "tap 2: '%s'", shown[6].pause);
    for (int f = 0; f < frames; f++) check_room(f);
}

/* API 46: Overlay on the game's text, the card before the last (Up twice from the first card reaches it): Enter
 * toggles it, Right turns it on and Left off, a touch on the card toggles it. Each change is config.ini's
 * overlay_on_text, every other line kept, and the status line says it applies when the overlay next opens; a file
 * without the line shows OFF and writes nothing until a change. */
static void test_overlay_on_text_by_keys_and_touch(void)
{
    card("; mine\nvocr_tier = small\n");
    static const Input in[] = {OCR_PAGE, PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_CROSS),
                               PRESS(SCE_CTRL_RIGHT), PRESS(SCE_CTRL_LEFT), PRESS(SCE_CTRL_LEFT), PRESS(SCE_CTRL_CROSS)};
    int frames = run(in, (int)(sizeof(in) / sizeof(in[0])));
    TEST_ASSERT(frames == (int)(sizeof(in) / sizeof(in[0])));
    static const struct { int frame; const char *value; } want[] = {
        {4, ""},     /* Up, Up: the card, nothing saved */
        {6, "on"},   /* Enter: on */
        {8, "on"},   /* Right: on already */
        {10, "off"}, /* Left: off */
        {12, "off"}, /* Left again: no change */
        {14, "on"},  /* Enter */
    };
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        int f = want[i].frame;
        TEST_CHECK_(!strcmp(shown[f].ontext, want[i].value), "frame %d: overlay_on_text '%s', want '%s'", f,
                    shown[f].ontext, want[i].value);
        TEST_CHECK_(focused_card(f) == 11 && !strcmp(shown[f].tier, "small"), "frame %d: card %d", f, focused_card(f));
        check_room(f);
    }
    const Text *off = text_at(4, card_top(4, 11) + 32, 812, 920), *on = text_at(6, card_top(6, 11) + 32, 812, 920);
    TEST_CHECK_(off && !strcmp(off->text, "OFF") && on && !strcmp(on->text, "ON"), "badges '%s', '%s'",
                off ? off->text : "(none)", on ? on->text : "(none)");
    TEST_CHECK(shows(4, "Overlay on the game's text"));
    const Text *st = text_at(6, ROOM_END + 4 + 22, 40, 41);
    TEST_CHECK_(st && !strcmp(st->text, "Saved. It applies when the overlay next opens."), "the status line: '%s'",
                st ? st->text : "(none)");
    {
        FILE *file = fopen(VJO_PREFERENCES_PATH, "rb");
        char text[4096];
        TEST_ASSERT(file != NULL);
        size_t n = fread(text, 1, sizeof(text) - 1, file);
        fclose(file);
        text[n] = 0;
        TEST_CHECK(!strcmp(text, "; mine\nvocr_tier = small\noverlay_on_text = on\n"));
        TEST_MSG("%s", text);
    }
    /* a touch on the card, once in view (Up wraps to the last card, one more reaches this one: Skip OCR first, the
     * column's 636; this card 780 down it), toggles it */
    card("overlay_on_text = on\n");
    static const Input taps[] = {OCR_PAGE, PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), TAP(600, 0), TAP(600, 0)};
    Input script_taps[sizeof(taps) / sizeof(taps[0])];
    memcpy(script_taps, taps, sizeof(taps));
    const int top = OCR_Y + 852 - 708;
    script_taps[6].y = script_taps[8].y = top + 34; /* TAP is two entries: the touch, then the release */
    frames = run(script_taps, (int)(sizeof(script_taps) / sizeof(script_taps[0])));
    TEST_ASSERT(frames == (int)(sizeof(script_taps) / sizeof(script_taps[0])));
    TEST_CHECK_(card_top(4, 11) == top && top + CARD_H <= ROOM_END, "the card at %d, want %d", card_top(4, 11), top);
    TEST_CHECK_(!strcmp(shown[4].ontext, "on") && !strcmp(shown[6].ontext, "off") && !strcmp(shown[8].ontext, "on"),
                "taps: %s %s %s", shown[4].ontext, shown[6].ontext, shown[8].ontext);
    for (int f = 0; f < frames; f++) check_room(f);
}

/* #358: Suggest a text box, the card above the last (Up twice from the first card reaches it), kept in region.ini, the Shell's file
 * (settings/preferences.h): ON without its line for every game (the default: no file), OFF with "region = full".
 * Enter switches it, Left is OFF and Right ON, a touch on the card switches it; each change rewrites region.ini with
 * every game's own line kept, config.ini untouched, and the status line says when it applies. An old install's box
 * for every game shows OFF; ON drops it. */
static void test_suggest_a_text_box_saved_in_region_ini(void)
{
    static const char game[] = "PCSG00415 = 1536,48188,62600,15059\n";
    card("; mine\nvocr_tier = small\n");
    put_file(VJO_PREFERENCES_REGION_PATH, game);
    static const Input in[] = {OCR_PAGE, PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_CROSS), PRESS(SCE_CTRL_LEFT),
                               PRESS(SCE_CTRL_RIGHT), PRESS(SCE_CTRL_RIGHT), PRESS(SCE_CTRL_LEFT)};
    int frames = run(in, (int)(sizeof(in) / sizeof(in[0])));
    TEST_ASSERT(frames == (int)(sizeof(in) / sizeof(in[0])));
    static const struct { int frame; const char *badge; int line; } want[] = {
        {6, "ON", 0},  /* Up: the last card, ON (no line for every game), nothing saved */
        {8, "OFF", 1}, /* Enter: OFF, "region = full" written */
        {10, "OFF", 1}, /* Left: OFF already */
        {12, "ON", 0},  /* Right: ON, the line dropped */
        {14, "ON", 0}, /* Right again: no change */
        {16, "OFF", 1},/* Left: OFF */
    };
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        int f = want[i].frame;
        const Text *b = text_at(f, card_top(f, 10) + 32, 812, 920);
        char expect[96];
        snprintf(expect, sizeof(expect), "%s%s", game, want[i].line ? "region = full\n" : "");
        TEST_CHECK_(focused_card(f) == 10 && b && !strcmp(b->text, want[i].badge), "frame %d: card %d, badge '%s'", f,
                    focused_card(f), b ? b->text : "(none)");
        TEST_CHECK_(!strcmp(shown[f].region, expect), "frame %d: region.ini '%s'", f, shown[f].region);
        TEST_CHECK_(!strcmp(shown[f].tier, "small") && !shown[f].vertical[0], "frame %d: config.ini changed", f);
        check_room(f);
    }
    TEST_CHECK(shows(6, "ON: a game without a box is offered the one Rubi finds."));
    const Text *st = text_at(8, ROOM_END + 4 + 22, 40, 41);
    TEST_CHECK_(st && !strcmp(st->text, "Saved. It applies when the game next starts or resumes."),
                "the status line: '%s'", st ? st->text : "(none)");
    /* no region.ini: ON, and ON again writes nothing; OFF creates it with the line alone; a touch switches it */
    card("vocr_tier = small\n");
    static const Input taps[] = {OCR_PAGE, PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_RIGHT), TAP(300, 0), TAP(860, 0)};
    Input script_taps[sizeof(taps) / sizeof(taps[0])];
    memcpy(script_taps, taps, sizeof(taps));
    /* the card's top once in view: Trusted hooks first (the column's 564), Suggest a text box 780 down it */
    const int top = OCR_Y + 780 - 708;
    script_taps[10].y = script_taps[12].y = top + 30;
    frames = run(script_taps, (int)(sizeof(script_taps) / sizeof(script_taps[0])));
    TEST_ASSERT(frames == (int)(sizeof(script_taps) / sizeof(script_taps[0])));
    TEST_CHECK_(card_top(6, 10) == top && top + CARD_H <= ROOM_END, "Suggest a text box at %d, want %d",
                card_top(6, 10), top);
    TEST_CHECK_(!strcmp(shown[8].region, "-"), "Right on ON: region.ini '%s'", shown[8].region);
    TEST_CHECK_(!strcmp(shown[10].region, "region = full\n") && focused_card(10) == 10, "tap 1: region.ini '%s'",
                shown[10].region);
    TEST_CHECK_(!strcmp(shown[12].region, ""), "tap 2: region.ini '%s'", shown[12].region);
    /* an old install's box for every game: OFF; ON drops it, the games' own lines kept */
    card("vocr_tier = small\n");
    put_file(VJO_PREFERENCES_REGION_PATH, "region = 0,40000,65535,25536\nPCSB00001 = full\n");
    static const Input old[] = {OCR_PAGE, PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_CROSS)};
    frames = run(old, (int)(sizeof(old) / sizeof(old[0])));
    TEST_ASSERT(frames == (int)(sizeof(old) / sizeof(old[0])));
    {
        const Text *b = text_at(6, card_top(6, 10) + 32, 812, 920);
        TEST_CHECK_(b && !strcmp(b->text, "OFF"), "an old box for every game: '%s'", b ? b->text : "(none)");
    }
    TEST_CHECK_(!strcmp(shown[8].region, "PCSB00001 = full\n"), "ON: region.ini '%s'", shown[8].region);
    /* a region.ini that cannot be read (its close fails, as a card error would): "--", nothing saved */
    card("vocr_tier = small\n");
    put_file(VJO_PREFERENCES_REGION_PATH, game);
    close_fails = "region.ini";
    static const Input locked[] = {OCR_PAGE, PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_UP), PRESS(SCE_CTRL_CROSS)};
    frames = run(locked, (int)(sizeof(locked) / sizeof(locked[0])));
    close_fails = NULL;
    TEST_ASSERT(frames == (int)(sizeof(locked) / sizeof(locked[0])));
    {
        const Text *b = text_at(8, card_top(8, 10) + 32, 812, 920);
        TEST_CHECK_(b && !strcmp(b->text, "--") && shows(8, "Cannot read region.ini."), "unreadable: '%s'",
                    b ? b->text : "(none)");
        TEST_CHECK_(!strcmp(shown[8].region, game), "unreadable: region.ini '%s'", shown[8].region);
    }
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
    check_status(1, NORMAL_STATUS, DIM); /* the OCR card below the banner is unchanged */
    /* the files agree: the title is back and nothing is red */
    snprintf(status, sizeof(status), "bootstrap: shell API %d entered\n", VJO_API_VERSION);
    app_card(own, own, status);
    TEST_ASSERT(run(in, 2) == 2);
    for (int f = 0; f < 2; f++) {
        banner_drawn(f, text, sizeof(text));
        TEST_CHECK_(!text[0], "frame %d: banner '%s'", f, text);
        const Text *title = text_at(f, 35, 0, 700);
        TEST_CHECK(title && !strcmp(title->text, "Rubi"));
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

/* The Dictionaries shown card's help line: drawn 30 px below its title
 * (settings/main.c: CARD_TITLE_Y 30, CARD_HELP_Y 60 within the card). */
static const Text *results_help(int f)
{
    for (int i = 0; i < shown[f].texts; i++)
        if (!strcmp(shown[f].text[i].text, "Dictionaries shown"))
            return text_at(f, shown[f].text[i].y + 30, 0, 960);
    return NULL;
}
#define DICT_DIR "ux0:data/VitaJPOverlay/dictionaries"
/* A dictionary's page 0 as the bundle writes it: magic, version, the frequency
 * list's name at 64 and, when given, the title at 192 (the rest of the file is
 * not read by the app). */
static void put_dictionary(const char *name, const char *magic, const char *title)
{
    char path[256];
    unsigned char page0[4096] = {0};
    memcpy(page0, magic, 8);
    page0[8] = (unsigned char)(magic[6] - '0');
    memcpy(page0 + 64, "Jiten", 5);
    if (title) memcpy(page0 + 192, title, strlen(title));
    snprintf(path, sizeof(path), DICT_DIR "/%s", name);
    mkdir(DICT_DIR, 0777);
    FILE *f = fopen(path, "wb");
    TEST_ASSERT(f != NULL);
    TEST_ASSERT(fwrite(page0, 1, sizeof(page0), f) == sizeof(page0));
    TEST_ASSERT(fclose(f) == 0);
}
/* The card names the one enabled dictionary by its file's own title, as the
 * bundled Bee's Ultimate Dictionary has one; without a readable title (none,
 * a version 1 file whose index fills those bytes, a malformed one, no file)
 * it counts the dictionaries as before. */
static void test_dictionaries_card_names_the_bundled_dictionary(void)
{
    /* the page scrolled to the card (#146's cards above it): Highlight focused, Dictionaries shown
     * drawn below it in the last frame */
    static const Input in[] = {OCR_PAGE, PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_DOWN),
                               PRESS(SCE_CTRL_DOWN)};
    enum { FRAMES = (int)(sizeof(in) / sizeof(in[0])), LAST = FRAMES - 1 };
    static const struct { const char *magic, *title, *line; } cases[] = {
        {"VJDICT3\0", "Bee's Ultimate Dictionary", "Best match only: Bee's Ultimate Dictionary."},
        {"VJDICT2\0", "Jitendex.org", "Best match only: Jitendex.org."},
        {"VJDICT3\0", NULL, "Best match only. 1 dictionary enabled."},
        {"VJDICT1\0", "index records", "Best match only. 1 dictionary enabled."},
        {"VJDICT3\0", "bad\x01title", "Best match only. 1 dictionary enabled."},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        card("local_dictionaries = plaintext.vjdict\n");
        put_dictionary("plaintext.vjdict", cases[i].magic, cases[i].title);
        TEST_CASE_("case %u", i);
        TEST_ASSERT(run(in, FRAMES) == FRAMES);
        const Text *t = results_help(LAST);
        TEST_CHECK_(t && !strcmp(t->text, cases[i].line), "'%s'", t ? t->text : "(none)");
    }
    /* no file: the count */
    card("local_dictionaries = plaintext.vjdict\n");
    unlink(DICT_DIR "/plaintext.vjdict");
    TEST_ASSERT(run(in, FRAMES) == FRAMES);
    const Text *t = results_help(LAST);
    TEST_CHECK_(t && !strcmp(t->text, "Best match only. 1 dictionary enabled."), "'%s'", t ? t->text : "(none)");
    /* one file with 2 shown (the default): nothing to page to, so best match only */
    card("local_dictionaries = plaintext.vjdict\nlocal_dictionary_results = 2\n");
    put_dictionary("plaintext.vjdict", "VJDICT3\0", "Bee's Ultimate Dictionary");
    TEST_ASSERT(run(in, FRAMES) == FRAMES);
    t = results_help(LAST);
    TEST_CHECK_(t && !strcmp(t->text, "Best match only: Bee's Ultimate Dictionary."), "'%s'",
                t ? t->text : "(none)");
    /* two files and 1 shown: the count */
    card("local_dictionaries = plaintext.vjdict,vndb-names.vjdict\nlocal_dictionary_results = 1\n");
    TEST_ASSERT(run(in, FRAMES) == FRAMES);
    t = results_help(LAST);
    TEST_CHECK_(t && !strcmp(t->text, "Best match only. 2 dictionaries enabled."), "'%s'", t ? t->text : "(none)");
    /* a file shorter than page 0's 256 bytes (cut inside its title): the count */
    card("local_dictionaries = plaintext.vjdict\n");
    put_dictionary("plaintext.vjdict", "VJDICT3\0", "Bee's Ultimate Dictionary");
    TEST_ASSERT(truncate(DICT_DIR "/plaintext.vjdict", 200) == 0);
    TEST_ASSERT(run(in, FRAMES) == FRAMES);
    t = results_help(LAST);
    TEST_CHECK_(t && !strcmp(t->text, "Best match only. 1 dictionary enabled."), "'%s'", t ? t->text : "(none)");
    /* The defaults: two bundled files and 2 shown, the paging line (it names none). */
    card(NULL);
    put_dictionary("plaintext.vjdict", "VJDICT3\0", "Bee's Ultimate Dictionary");
    TEST_ASSERT(run(in, FRAMES) == FRAMES);
    t = results_help(LAST);
    TEST_CHECK_(t && !strcmp(t->text, "Best match, then the others below it. 2 enabled."), "'%s'",
                t ? t->text : "(none)");
    TEST_CHECK(!shown[1].backend[0]); /* looking saves nothing */
}

/* The tab bar, laid out from its labels since the fifth tab (#313's Look up; #312's Stats came and went): five tabs
 * from x 24 to at most 936, in order, TAB_GAP apart, each at least its label's width with 10 px each side (the stub's
 * labels do not fit an equal share), each label centred in its tab on the old baseline and never cut, the page's own
 * filled. A touch on a tab shows its page; one in the gap between two tabs does nothing. */
#define TAB_Y 52
#define TAB_H 48
#define TABS 5
static const char *const TAB_LABELS[TABS] = {"Games", "OCR & lookup", "Anki", "Look up", "Install"};
typedef struct { int n, x[TABS], w[TABS]; } Tabs;
static Tabs tabs_of(int f, int active)
{
    Tabs t = {0};
    for (int i = 0; i < shown[f].rects; i++) {
        const Rect *r = &shown[f].rect[i];
        if (r->y != TAB_Y || r->h != TAB_H) continue;
        if (t.n < TABS) {
            t.x[t.n] = r->x;
            t.w[t.n] = r->w;
            TEST_CHECK_(r->color == (t.n == active ? ACCENT : CARD), "frame %d: tab %d colour %08X", f, t.n, r->color);
            const Text *l = text_at(f, TAB_Y + 33, r->x, r->x + r->w);
            TEST_CHECK_(l && !strcmp(l->text, TAB_LABELS[t.n]) && !l->clipped && l->x >= r->x + 10 &&
                            l->x + width(l) <= r->x + r->w - 10 && abs(2 * l->x + width(l) - (2 * r->x + r->w)) <= 1 &&
                            l->color == (t.n == active ? BG_COLOR : WHITE),
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
    Input in[2 * 9];
    int n = 0;
    card(NULL);
    /* the layout, on the Games page; then a touch on each tab's middle (the Anki page's stub draws nothing: not it),
     * and one in a gap */
    in[n++] = (Input){0, 0, 0, 0};
    in[n++] = (Input){0, 0, 0, 0};
    TEST_ASSERT(run(in, n) >= 2);
    Tabs t = tabs_of(0, 0);
    static const int order[] = {1, 3, 4, 0, 3}; /* OCR & lookup, Look up, Install, Games, Look up */
    n = 0;
    for (int k = 0; k < 5; k++) {
        in[n++] = (Input){0, 1, t.x[order[k]] + t.w[order[k]] / 2, TAB_Y + 20};
        in[n++] = (Input){0, 0, 0, 0};
    }
    in[n++] = (Input){0, 1, t.x[1] + t.w[1] + 4, TAB_Y + 20}; /* frame 10: the gap after OCR & lookup: nothing */
    in[n++] = (Input){0, 0, 0, 0};
    in[n++] = (Input){SCE_CTRL_SELECT, 0, 0, 0}; /* frame 12: Select from Look up: Install */
    in[n++] = (Input){0, 0, 0, 0};
    TEST_ASSERT(run(in, n) >= n);
    for (int k = 0; k < 5; k++)
        tabs_of(2 * k, order[k]);
    tabs_of(10, 3);
    tabs_of(12, 4);
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
    for (int i = 0; i < 3; i++) /* Select to the Look up page, whose stub draws and measures new characters */
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
                    strstr(log, "font: an empty glyph atlas to measure with after 350 characters (3 times)\n"),
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
#define TABS_TO_LOOKUP PRESS(SCE_CTRL_SELECT), PRESS(SCE_CTRL_SELECT), PRESS(SCE_CTRL_SELECT) /* Games -> Look up */
static void test_the_look_up_page_s_wiring(void)
{
    card(NULL);
    /* shown, left by Select (to Install), shown by its tab, left by a tab (Games) */
    {
        Input in[2 * 8];
        int n = 0;
        static const Input lead[] = {TABS_TO_LOOKUP, PRESS(SCE_CTRL_SELECT)};
        memcpy(in, lead, sizeof(lead));
        n = (int)(sizeof(lead) / sizeof(lead[0]));
        TEST_ASSERT(run(in, n) == n);
        TEST_CHECK_(session->lookup_shows == 1 && session->lookup_leaves == 1, "Select: shown %d, left %d",
                    session->lookup_shows, session->lookup_leaves);
        Tabs t = tabs_of(1, 1); /* (frame 1: OCR & lookup, after the first Select) */
        Input taps[] = {TAP(t.x[3] + t.w[3] / 2, TAB_Y + 20), TAP(t.x[0] + t.w[0] / 2, TAB_Y + 20),
                        TAP(t.x[3] + t.w[3] / 2, TAB_Y + 20), TAP(t.x[3] + t.w[3] / 2, TAB_Y + 20)};
        TEST_ASSERT(run(taps, 8) == 8);
        /* the second tap on its own tab while shown: no second show */
        TEST_CHECK_(session->lookup_shows == 2 && session->lookup_leaves == 1, "tabs: shown %d, left %d",
                    session->lookup_shows, session->lookup_leaves);
    }
    /* Cancel held by the page: Circle pressed on it (frame 6) does not close the app, and the page gets it; once the
     * page lets go, Circle (frame 8) closes the app */
    {
        static const Input in[] = {TABS_TO_LOOKUP, PRESS(SCE_CTRL_CIRCLE), PRESS(SCE_CTRL_CIRCLE), {0, 0, 0, 0}};
        vjo_lookup_stub_hold_cancel_to = 3; /* while fewer than three of its frames have run: frames 4, 5 and 6 */
        int frames = run(in, (int)(sizeof(in) / sizeof(in[0])));
        vjo_lookup_stub_hold_cancel_to = 0;
        TEST_CHECK_(frames == 8, "the app closed at frame %d, not 8", frames);
        TEST_CHECK_(session->lookup_frames == 4 && session->lookup_pressed[2] == SCE_CTRL_CIRCLE,
                    "%d frames of the page, its third's press %08X", session->lookup_frames, session->lookup_pressed[2]);
    }
    /* the keyboard (the page's second frame, frame 5, the keyboard's own): Circle down in the frame after it (6), as
     * the keyboard's closing press, is no press (the page sees it held, not pressed) and does not close the app */
    {
        static const Input in[] = {TABS_TO_LOOKUP, {SCE_CTRL_CIRCLE, 0, 0, 0}, {SCE_CTRL_CIRCLE, 0, 0, 0}, {0, 0, 0, 0},
                                   {0, 0, 0, 0}};
        vjo_lookup_stub_keyboard_at = 2;
        int frames = run(in, (int)(sizeof(in) / sizeof(in[0])));
        vjo_lookup_stub_keyboard_at = 0;
        TEST_CHECK_(frames == (int)(sizeof(in) / sizeof(in[0])), "the app closed at frame %d of %d", frames,
                    (int)(sizeof(in) / sizeof(in[0])));
        TEST_CHECK_(session->lookup_frames >= 3 && session->lookup_pressed[2] == 0 &&
                        session->lookup_held[2] == SCE_CTRL_CIRCLE,
                    "after the keyboard: pressed %08X, held %08X", session->lookup_pressed[2], session->lookup_held[2]);
    }
}

TEST_LIST = {
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
    {"overlay on the game's text by keys and touch, saved as overlay_on_text (API 46)", test_overlay_on_text_by_keys_and_touch},
    {"Pause the game to read by keys and touch, saved as ocr_pause (API 55)", test_pause_card_by_keys_and_touch},
    {"the OCR-skipping switches by keys and touch, saved as ocr_text_gate and hook_trust (API 43)",
     test_ocr_skip_switches_by_keys_and_touch},
    {"read_text_and_speed_boost_by_keys_and_touch_saved_as_ocr_mode_and_ocr_boost_clock",
     test_read_text_and_speed_boost_by_keys_and_touch},
    {"the_page_scrolls_a_card_at_a_time_and_never_draws_into_the_status_line", test_the_page_scrolls_a_card_at_a_time},
    {"read_text_names_the_buttons_that_open_the_overlay", test_read_text_names_the_buttons_that_open_the_overlay},
    {"scrolled_highlight_and_dictionaries_cards_keep_their_touch_targets", test_scrolled_cards_keep_their_touch_targets},
    {"dictionaries_shown_by_its_buttons", test_dictionaries_shown_by_its_buttons},
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
    run(in, 2);
    check_status(1, "Normal: default, balanced. VOCR6-N found.", DIM);
    check_sizes(1, VJO_VOCR_TIER_NORMAL, SIZES, DETAILS6);
    /* the Large size's model is the earlier generation's: the line names that one */
    card("vocr_tier = large\n");
    run(in, 2);
    check_status(1, "Large: most accurate. VOCR5-L found.", DIM);
    check_sizes(1, VJO_VOCR_TIER_LARGE, SIZES, DETAILS6);
    remove_model("VOCR5_L_w8.vocr");
    run(in, 2);
    check_status(1, "VOCR5-L missing. Reinstall the package.", OFF);
    check_no_fallback_hint(2); /* VOCR6-N would run: the card never says so */
    /* the same app with an older config.ini: its model's size, this package's model of it */
    card("vocr_model = VOCR3_H15_w8.vocr\nvocr_fallback_model = VOCR3_FL10_w8.vocr\n");
    run(in, 2);
    check_status(1, "Normal: default, balanced. VOCR6-N found.", DIM);
    check_sizes(1, VJO_VOCR_TIER_NORMAL, SIZES, DETAILS6);
    card("vocr_model = VOCR4_W20_w8.vocr\n");
    run(in, 2);
    check_status(1, "Large: most accurate. VOCR5-L found.", DIM);
}
/* Nothing of the chosen size or below on the card (a partly uploaded package):
 * nothing runs; the chosen size's model is named missing, never another one. */
static void test_mixed_generations_name_the_model_that_loads(void)
{
    static const Input in[] = {OCR_PAGE};
    card("vocr_tier = normal\n");
    remove_model("VOCR6_N_w8.vocr");
    run(in, 2);
    check_status(1, "VOCR6-N missing. Reinstall the package.", OFF); /* VOCR6-S loads */
    check_no_fallback_hint(2);
    put_model("VOCR6_N_w8.vocr", 1500000 - 32768); /* one 32 KiB piece short */
    remove_model("VOCR6_S_w8.vocr");
    run(in, 2);
    check_status(1, "VOCR6-N missing. Reinstall the package.", OFF);
    check_no_fallback_hint(2);
    /* its file back: it runs again */
    put_model("VOCR6_N_w8.vocr", 1500000);
    run(in, 2);
    check_status(1, "Normal: default, balanced. VOCR6-N found.", DIM);
    /* Large chosen, its VOCR5 file and Normal's missing: the chosen size's model named missing */
    card("vocr_tier = large\n");
    remove_model("VOCR5_L_w8.vocr");
    remove_model("VOCR6_N_w8.vocr");
    run(in, 2);
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
