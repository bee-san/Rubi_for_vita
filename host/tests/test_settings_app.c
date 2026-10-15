/* The Overlay app's OCR engine card through its actual input and draw loop
 * (settings/main.c), on the host. The SDK boundary (vita2d, controller, touch
 * screen, display) is scripted and records what each frame draws; config.ini,
 * games.ini and the model files are real files in a temporary directory, read
 * and written by the app's own settings/preferences.c, settings/engine.c and
 * vita/games_storage.c. Text widths come from stub metrics (Latin 10, other
 * characters 18 units per 18-unit em), not the firmware's PVF fonts.
 *
 * Built twice: with this build's model metadata (core/vocr_models.c), and with
 * VJO_VOCR_MODEL_INFO_TEST naming a later package's table, where generation
 * VOCR5 is pinned: the app then names the local engine VOCR5 unchanged. */
#include "acutest.h"
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
#include "../../include/vjo_vocr_sizes.h"
#include "../../settings/catalog.h"
#include "../../settings/engine.h"
#include "../../settings/preferences.h"

int vjo_settings_main(void); /* settings/main.c's main(), renamed for the harness */

/* settings/main.c's layout: the OCR card and its two engine buttons. */
#define OCR_Y 112
#define BUTTON_Y (OCR_Y + 42)
#define BUTTON_H 48
#define BUTTON_TEXT_Y (BUTTON_Y + BUTTON_H / 2 + 9)
#define HELP_Y (OCR_Y + 111)
#define STATUS_Y (OCR_Y + 137)
#define ACCENT RGBA8(91, 220, 207, 255)
#define SELECTED RGBA8(35, 56, 70, 255)
#define DIM RGBA8(193, 207, 222, 255)
#define OFF RGBA8(244, 160, 167, 255)
#define MODEL_DIR "ux0:data/VitaJPOverlay/ocr"

/* ---- storage: the SDK's file calls over real files ---- */
static int native(int n) { return n < 0 ? (int)(0x80010000u | (unsigned)errno) : n; }
static const char *close_fails; /* a file whose sceIoClose reports an error */
static int close_fails_fd = -1;
int sceIoOpen(const char *p, int flags, unsigned mode)
{
    int fd = native(open(p, flags, mode));
    size_t n = strlen(p), m = close_fails ? strlen(close_fails) : 0;
    if (fd >= 0 && m && n >= m && !strcmp(p + n - m, close_fails)) close_fails_fd = fd;
    return fd;
}
int sceIoClose(int fd)
{
    int rc = native(close(fd));
    if (fd != close_fails_fd) return rc;
    close_fails_fd = -1;
    return (int)0x80010005u; /* EIO */
}
int sceIoRead(int fd, void *p, size_t len) { return native((int)read(fd, p, len)); }
int sceIoWrite(int fd, const void *p, size_t len) { return native((int)write(fd, p, len)); }
SceOff sceIoLseek(int fd, SceOff off, int whence) { return lseek(fd, off, whence); }
int sceIoMkdir(const char *p, unsigned mode) { return native(mkdir(p, mode)); }
int sceIoGetstat(const char *p, SceIoStat *out)
{ struct stat st; int rc = stat(p, &st); if (!rc) out->st_mode = st.st_mode; return native(rc); }
int sceIoRename(const char *a, const char *b) { return native(rename(a, b)); }
int sceIoRemove(const char *p) { return native(unlink(p)); }
int sceIoSync(const char *p, unsigned flags) { (void)p; (void)flags; return 0; }

/* No installed games: the Games page is not what these tests drive. */
int vjo_catalog_scan(VjoInstalledGame *out, int capacity, int *truncated)
{ (void)out; (void)capacity; *truncated = 0; return 0; }

/* ---- the scripted controller, touch screen and display ---- */
typedef struct { unsigned buttons; int touch, x, y; } Input; /* x, y in screen pixels */
#define MAX_FRAMES 48
typedef struct { int x, y; unsigned color; float scale; char text[128]; } Text;
typedef struct { int x, y, w, h; unsigned color; } Rect;
typedef struct {
    Text text[160]; int texts;
    Rect rect[96]; int rects;
    char backend[16]; /* config.ini's ocr_backend value after this frame */
} Frame;
/* Each run is one process of the app, as on the console: settings/main.c keeps
 * its state in statics, so it runs in a forked child, and the frames it shows
 * come back through shared memory. */
typedef struct { int frames, exited, main_rc, finis; Frame shown[MAX_FRAMES]; } Session;
static Session *session;
static Frame *shown;
static const Input *script;
static int script_frames, frame, exited = -1;
static int finis;      /* vita2d_fini calls in this process */
static int font_fails; /* vita2d_load_default_pvf returns NULL */
static Frame drawing;
static int font_dummy;

uint64_t sceKernelGetProcessTimeWide(void) { return (uint64_t)frame * 16667u; }
int sceKernelExitProcess(int status) { exited = status; return 0; }
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

/* config.ini's ocr_backend value, or "" */
static void saved_backend(char out[16])
{
    char text[8192];
    out[0] = 0;
    FILE *f = fopen(VJO_PREFERENCES_PATH, "rb");
    if (!f) return;
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    const char *value = !strncmp(text, "ocr_backend = ", 14) ? text + 14 : NULL;
    const char *line = value ? NULL : strstr(text, "\nocr_backend = ");
    if (line) value = line + 15;
    if (value) sscanf(value, "%15[a-z0-9]", out);
}
int sceDisplayWaitVblankStart(void)
{
    if (frame < MAX_FRAMES) {
        shown[frame] = drawing;
        saved_backend(shown[frame].backend);
    }
    frame++;
    return 0;
}

int vita2d_init(void) { return 0; }
int vita2d_fini(void) { finis++; return 0; }
void vita2d_set_clear_color(unsigned int color) { (void)color; }
void vita2d_start_drawing(void) { memset(&drawing, 0, sizeof(drawing)); }
void vita2d_end_drawing(void) {}
void vita2d_swap_buffers(void) {}
void vita2d_clear_screen(void) {}
void vita2d_wait_rendering_done(void) {}
void vita2d_enable_clipping(void) {}
void vita2d_disable_clipping(void) {}
void vita2d_set_clip_rectangle(int x_min, int y_min, int x_max, int y_max)
{ (void)x_min; (void)y_min; (void)x_max; (void)y_max; }
void vita2d_draw_rectangle(float x, float y, float w, float h, unsigned int color)
{
    if (drawing.rects < (int)(sizeof(drawing.rect) / sizeof(drawing.rect[0])))
        drawing.rect[drawing.rects++] = (Rect){(int)x, (int)y, (int)w, (int)h, color};
}
vita2d_pvf *vita2d_load_default_pvf(void) { return font_fails ? NULL : (vita2d_pvf *)&font_dummy; }
void vita2d_free_pvf(vita2d_pvf *font) { (void)font; }
/* Stub metrics: 10 units for an ASCII character, 18 for any other, at scale 1. */
int vita2d_pvf_text_width(vita2d_pvf *font, float scale, const char *text)
{
    (void)font;
    int units = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if (*p < 0x80) units += 10;
        else if ((*p & 0xC0) != 0x80) units += 18;
    return (int)(units * scale + 0.5f);
}
int vita2d_pvf_draw_text(vita2d_pvf *font, int x, int y, unsigned int color, float scale, const char *text)
{
    (void)font;
    if (drawing.texts < (int)(sizeof(drawing.text) / sizeof(drawing.text[0]))) {
        Text *t = &drawing.text[drawing.texts++];
        t->x = x; t->y = y; t->color = color; t->scale = scale;
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
 * app's own forked processes leave with _exit, so only the test removes it). */
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
static void remove_card(void)
{
    if (card_dir[0] && chdir("/") == 0) remove_tree(card_dir);
}
/* A fresh memory card: the default models present, config.ini as given (NULL: none). */
static void card(const char *config)
{
    static int initialized;
    if (!initialized) {
        char dir[] = "/tmp/vjo-settings-app-XXXXXX";
        TEST_ASSERT(mkdtemp(dir) != NULL);
        snprintf(card_dir, sizeof(card_dir), "%s", dir);
        atexit(remove_card);
        TEST_ASSERT(chdir(dir) == 0);
        initialized = 1;
    }
    mkdir("ux0:data", 0777);
    mkdir("ux0:data/VitaJPOverlay", 0777);
    mkdir(MODEL_DIR, 0777);
    unlink(VJO_PREFERENCES_PATH);
    unlink(VJO_PREFERENCES_PATH ".bak");
    unlink(VJO_PREFERENCES_PATH ".tmp");
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
typedef struct { int count; Rect rect[4]; const Text *label[4]; } Buttons;
static Buttons engine_buttons(int f)
{
    Buttons b = {0};
    for (int i = 0; i < shown[f].rects; i++) {
        const Rect *r = &shown[f].rect[i];
        if (r->y != BUTTON_Y || r->h != BUTTON_H) continue;
        if (b.count < 4) {
            b.rect[b.count] = *r;
            b.label[b.count] = text_at(f, BUTTON_TEXT_Y, r->x, r->x + r->w);
        }
        b.count++;
    }
    return b;
}
static int width(const Text *t)
{
    return vita2d_pvf_text_width(NULL, t->scale, t->text);
}
/* Frame f's engine card: two buttons, local (label, active?) and Lens, then help and status. */
static void check_card(int f, const char *local, int lens_active, const char *help, const char *status,
                       unsigned status_color)
{
    Buttons b = engine_buttons(f);
    TEST_CHECK_(b.count == 2, "frame %d: %d engine buttons", f, b.count);
    if (b.count != 2) return;
    TEST_CHECK(b.rect[0].x == 42 && b.rect[0].w == 432 && b.rect[1].x == 486 && b.rect[1].w == 432);
    TEST_CHECK(b.rect[1].x + b.rect[1].w <= 932 - 14); /* inside the card, as the tab row's edges */
    TEST_CHECK_(b.label[0] && !strcmp(b.label[0]->text, local), "frame %d: local button '%s'", f,
                b.label[0] ? b.label[0]->text : "(none)");
    TEST_CHECK_(b.label[1] && !strcmp(b.label[1]->text, "Google Lens"), "frame %d: second button '%s'", f,
                b.label[1] ? b.label[1]->text : "(none)");
    TEST_CHECK_(b.rect[0].color == (lens_active ? SELECTED : ACCENT) &&
                b.rect[1].color == (lens_active ? ACCENT : SELECTED), "frame %d: active button", f);
    for (int i = 0; i < 2; i++) /* each label is centred in its button */
        if (b.label[i]) TEST_CHECK(b.label[i]->x == b.rect[i].x + (b.rect[i].w - width(b.label[i])) / 2);
    const Text *h = text_at(f, HELP_Y, 0, 960), *s = text_at(f, STATUS_Y, 0, 960);
    TEST_CHECK_(h && !strcmp(h->text, help), "frame %d: help '%s'", f, h ? h->text : "(none)");
    TEST_CHECK_(s && !strcmp(s->text, status) && s->color == status_color, "frame %d: status '%s' colour %08X", f,
                s ? s->text : "(none)", s ? s->color : 0);
    if (h) TEST_CHECK(h->x + width(h) <= 912); /* within the card's text width (stub metrics) */
    if (s) TEST_CHECK(s->x + width(s) <= 912);
}

#define TAP(x, y) {0, 1, (x), (y)}, {0, 0, 0, 0}
#define PRESS(button) {(button), 0, 0, 0}, {0, 0, 0, 0}
#define OCR_PAGE PRESS(SCE_CTRL_SELECT) /* Games -> OCR & lookup, the page after the first */
#define LOCAL_BUTTON TAP(200, 170)
#define LENS_BUTTON TAP(700, 170)
#define CARD_TITLE TAP(300, 130)  /* the card outside its buttons: focus only */

static const char *const LOCAL_HELP = "Local OCR: dialogue region, automatic or manual.";

#ifndef VJO_VOCR_MODEL_INFO_TEST
static int shows(int f, const char *needle)
{
    for (int i = 0; i < shown[f].texts; i++)
        if (strstr(shown[f].text[i].text, needle)) return 1;
    return 0;
}
static const char *const LENS_HELP = "Wi-Fi required. Sends the selected image to Google.";
static const char *const LENS_STATUS = "No model file needed. Uses your chosen dictionary.";

/* This package: exactly two engines, VOCR3 (its default model's generation) and Google Lens. */
static void test_two_engines_local_named_after_its_generation(void)
{
    card(NULL); /* first run: no config.ini, the defaults */
    static const Input in[] = {OCR_PAGE};
    TEST_ASSERT(run(in, 2) == 2); /* each scripted frame drawn; Circle then exits */
    check_card(0, "VOCR3", 0, LOCAL_HELP, "VOCR3-H15 found. Game OCR worker required.", DIM);
    check_card(1, "VOCR3", 0, LOCAL_HELP, "VOCR3-H15 found. Game OCR worker required.", DIM);
    for (int f = 0; f < 2; f++) TEST_CHECK(!shows(f, "VOCR2") && !shows(f, "Custom engine"));
    TEST_CHECK(!shown[1].backend[0]); /* looking saves nothing */
    TEST_CHECK(access(VJO_PREFERENCES_PATH, F_OK) != 0);
}

/* Touching a button chooses that engine and saves it; the card follows. Every
 * button edge and the gaps around them: each tap comes where a hit on the wrong
 * side would change the saved engine. */
static void test_touch_chooses_and_saves_each_engine(void)
{
    static const struct { int x, y; const char *backend; } taps[] = {
        {700, 202, "vocr"},  /* the row below the buttons */
        {700, 153, "vocr"},  /* the row above them */
        {485, 170, "vocr"},  /* the gap, beside Google Lens */
        {918, 170, "vocr"},  /* right of Google Lens */
        {486, 154, "lens"},  /* Google Lens, top-left pixel */
        {474, 170, "lens"},  /* the gap, beside the local engine */
        {41, 170, "lens"},   /* left of the local engine */
        {200, 153, "lens"},
        {200, 202, "lens"},
        {473, 201, "vocr"},  /* the local engine, bottom-right pixel */
        {917, 201, "lens"},  /* Google Lens, bottom-right pixel */
        {917, 170, "lens"},  /* the selected engine again */
        {42, 154, "vocr"},   /* the local engine, top-left pixel */
        {300, 130, "vocr"},  /* the card's title: focus only */
        {700, 170, "lens"},
        {700, 170, "lens"},
        {200, 170, "vocr"},
    };
    enum { N = sizeof(taps) / sizeof(taps[0]) };
    static Input in[2 + 2 * N] = {OCR_PAGE};
    card("ocr_backend = vocr\nocr_mode = on_press\nfont_size_ja = 22\n");
    for (int i = 0; i < N; i++) in[2 + 2 * i] = (Input){0, 1, taps[i].x, taps[i].y};
    TEST_ASSERT(run(in, 2 + 2 * N) == 2 + 2 * N);
    for (int i = 0; i < N; i++) {
        int f = 2 + 2 * i, lens = !strcmp(taps[i].backend, "lens");
        TEST_CHECK_(!strcmp(shown[f].backend, taps[i].backend), "tap %d at %d,%d: saved %s, want %s", i,
                    taps[i].x, taps[i].y, shown[f].backend, taps[i].backend);
        check_card(f + 1, "VOCR3", lens, lens ? LENS_HELP : LOCAL_HELP,
                   lens ? LENS_STATUS : "VOCR3-H15 found. Game OCR worker required.", DIM);
    }
    /* every other setting survived the saves */
    FILE *f = fopen(VJO_PREFERENCES_PATH, "rb");
    TEST_ASSERT(f != NULL);
    char text[4096];
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    TEST_CHECK(strstr(text, "ocr_mode = on_press\n") && strstr(text, "font_size_ja = 22\n"));
}

/* Controller: Left chooses the local engine, Right Google Lens, Cross the other one. */
static void test_controller_left_right_choose_and_cross_toggles(void)
{
    card("ocr_backend = lens\n");
    static const Input in[] = {OCR_PAGE, CARD_TITLE, PRESS(SCE_CTRL_LEFT), PRESS(SCE_CTRL_LEFT),
                               PRESS(SCE_CTRL_RIGHT), PRESS(SCE_CTRL_RIGHT), PRESS(SCE_CTRL_CROSS),
                               PRESS(SCE_CTRL_CROSS), PRESS(SCE_CTRL_DOWN), PRESS(SCE_CTRL_UP),
                               PRESS(SCE_CTRL_CROSS)};
    run(in, (int)(sizeof(in) / sizeof(in[0])));
    check_card(3, "VOCR3", 1, LENS_HELP, LENS_STATUS, DIM);
    static const struct { int frame; const char *backend; } want[] = {
        {4, "vocr"}, {6, "vocr"}, {8, "lens"}, {10, "lens"}, {12, "vocr"}, {14, "lens"},
        {16, "lens"}, {18, "lens"}, {20, "vocr"}, /* Down and Up leave the card and come back */
    };
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        int f = want[i].frame, lens = !strcmp(want[i].backend, "lens");
        TEST_CHECK_(!strcmp(shown[f].backend, want[i].backend), "frame %d: saved %s, want %s", f,
                    shown[f].backend, want[i].backend);
        check_card(f, "VOCR3", lens, lens ? LENS_HELP : LOCAL_HELP,
                   lens ? LENS_STATUS : "VOCR3-H15 found. Game OCR worker required.", DIM);
    }
}

/* A config.ini that chose a removed engine shows the local engine selected
 * (it loads as vocr), and the app's next save writes vocr in its place. */
static void test_removed_engine_shows_local_and_next_save_rewrites_it(void)
{
    static const char *const removed[] = {"vocr2", "meiki", "ncnn"};
    for (unsigned i = 0; i < 3; i++) {
        char config[128];
        snprintf(config, sizeof(config), "; mine\nocr_backend = %s\nfont_size_ja = 22\n", removed[i]);
        card(config);
        static const Input in[] = {OCR_PAGE, CARD_TITLE, PRESS(SCE_CTRL_LEFT)};
        run(in, (int)(sizeof(in) / sizeof(in[0])));
        check_card(1, "VOCR3", 0, LOCAL_HELP, "VOCR3-H15 found. Game OCR worker required.", DIM);
        TEST_CHECK_(!strcmp(shown[3].backend, removed[i]), "%s: unchanged until a choice", removed[i]);
        TEST_CHECK_(!strcmp(shown[4].backend, "vocr"), "%s: rewritten as '%s'", removed[i], shown[4].backend);
        for (int f = 0; f < 6; f++) TEST_CHECK(!shows(f, "VOCR2"));
    }
}

/* The local engine is named after the generation of the model the game worker
 * would load; the line under it reports that file, or what is missing. */
static void test_label_and_status_follow_the_configured_models(void)
{
    static const Input in[] = {OCR_PAGE};
    card("vocr_model = H15_w8.vocr\nvocr_fallback_model = FL10_w8.vocr\n");
    run(in, 2);
    check_card(1, "VOCR1", 0, LOCAL_HELP, "VOCR1-H15 found. Game OCR worker required.", DIM);
    /* the configured model names the engine, not its fallback of another generation */
    card("vocr_model = H15_w8.vocr\n");
    run(in, 2);
    check_card(1, "VOCR1", 0, LOCAL_HELP, "VOCR1-H15 found. Game OCR worker required.", DIM);
    /* until its file is missing or not its release size: the worker refuses it and
     * runs the fallback, which then names the engine */
    remove_model("H15_w8.vocr");
    run(in, 2);
    check_card(1, "VOCR3", 0, LOCAL_HELP, "VOCR1-H15 missing; uses VOCR3-FL10. Game OCR worker required.", DIM);
    put_model("H15_w8.vocr", VJO_VOCR_H15_BYTES + 1);
    run(in, 2);
    check_card(1, "VOCR3", 0, LOCAL_HELP, "VOCR1-H15 missing; uses VOCR3-FL10. Game OCR worker required.", DIM);
    /* with the fallback's file missing too nothing runs: the configured model's name, a red line */
    remove_model(VJO_VOCR3_FL10_NAME);
    run(in, 2);
    check_card(1, "VOCR1", 0, LOCAL_HELP, "VOCR1-H15 missing. Game OCR worker required.", OFF);
    /* a model file that cannot be closed again is not counted as found */
    card(NULL);
    close_fails = VJO_VOCR3_H15_NAME;
    run(in, 2);
    close_fails = NULL;
    check_card(1, "VOCR3", 0, LOCAL_HELP, "VOCR3-H15 missing; uses VOCR3-FL10. Game OCR worker required.", DIM);
    /* a model this build does not run: refused, the fallback runs and names the engine */
    card("vocr_model = VOCR9_X_w8.vocr\n");
    run(in, 2);
    check_card(1, "VOCR3", 0, LOCAL_HELP, "vocr_model is not in this build; uses VOCR3-FL10.", OFF);
    /* the fallback's file missing as well: nothing runs, and the button keeps the fallback's name */
    remove_model(VJO_VOCR3_FL10_NAME);
    run(in, 2);
    check_card(1, "VOCR3", 0, LOCAL_HELP, "vocr_model is not in this build. Set it in config.ini.", OFF);
    card("vocr_model = VOCR9_X_w8.vocr\nvocr_fallback_model = none\n");
    run(in, 2);
    check_card(1, "VOCR", 0, LOCAL_HELP, "vocr_model is not in this build. Set it in config.ini.", OFF);
    /* the confidence cascade's VOCR4-W20 also runs as the job's own model (API 20) */
    card("vocr_model = VOCR4_W20_w8.vocr\n");
    run(in, 2);
    check_card(1, "VOCR4", 0, LOCAL_HELP, "VOCR4-W20 found. Game OCR worker required.", DIM);
    /* the model's file missing, or not its release size: the fallback runs */
    card(NULL);
    remove_model(VJO_VOCR3_H15_NAME);
    run(in, 2);
    check_card(1, "VOCR3", 0, LOCAL_HELP, "VOCR3-H15 missing; uses VOCR3-FL10. Game OCR worker required.", DIM);
    card(NULL);
    put_model(VJO_VOCR3_H15_NAME, VJO_VOCR_H15_BYTES - 1);
    run(in, 2);
    check_card(1, "VOCR3", 0, LOCAL_HELP, "VOCR3-H15 missing; uses VOCR3-FL10. Game OCR worker required.", DIM);
    /* both missing, or no fallback configured */
    remove_model(VJO_VOCR3_FL10_NAME);
    run(in, 2);
    check_card(1, "VOCR3", 0, LOCAL_HELP, "VOCR3-H15 missing. Game OCR worker required.", OFF);
    card("vocr_fallback_model = none\n");
    remove_model(VJO_VOCR3_H15_NAME);
    run(in, 2);
    check_card(1, "VOCR3", 0, LOCAL_HELP, "VOCR3-H15 missing. Game OCR worker required.", OFF);
    /* a fallback naming the model itself is no fallback */
    card("vocr_fallback_model = VOCR3_H15_w8.vocr\n");
    remove_model(VJO_VOCR3_H15_NAME);
    run(in, 2);
    check_card(1, "VOCR3", 0, LOCAL_HELP, "VOCR3-H15 missing. Game OCR worker required.", OFF);
    /* Google Lens needs no model file, whatever is missing */
    card("ocr_backend = lens\n");
    remove_model(VJO_VOCR3_H15_NAME);
    remove_model(VJO_VOCR3_FL10_NAME);
    run(in, 2);
    check_card(1, "VOCR3", 1, LENS_HELP, LENS_STATUS, DIM);
}

/* An unreadable config.ini: nothing is selected and nothing can be saved. */
static void test_unreadable_config_selects_and_saves_nothing(void)
{
    card(NULL);
    put_file(VJO_PREFERENCES_PATH, "ocr_backend = lens\n");
    TEST_ASSERT(chmod(VJO_PREFERENCES_PATH, 0) == 0);
    if (access(VJO_PREFERENCES_PATH, R_OK) == 0) { /* running as root: permissions do not apply */
        chmod(VJO_PREFERENCES_PATH, 0644);
        TEST_SKIP("the file stays readable (root)");
        return;
    }
    static const Input in[] = {OCR_PAGE, LOCAL_BUTTON};
    run(in, (int)(sizeof(in) / sizeof(in[0])));
    Buttons b = engine_buttons(3);
    TEST_CHECK(b.count == 2 && b.rect[0].color == SELECTED && b.rect[1].color == SELECTED);
    const Text *h = text_at(3, HELP_Y, 0, 960), *s = text_at(3, STATUS_Y, 0, 960);
    TEST_CHECK(h && !strcmp(h->text, "Cannot read the saved OCR choice."));
    TEST_CHECK(s && !strcmp(s->text, "Repair config.ini before changing settings.") && s->color == OFF);
    chmod(VJO_PREFERENCES_PATH, 0644);
    char backend[16];
    saved_backend(backend);
    TEST_CHECK(!strcmp(backend, "lens")); /* the file was never written */
}

/* engine.c on its own: the button order and the backends a parsed file can hold. */
static void test_engine_buttons_and_backends(void)
{
    TEST_CHECK(vjo_engine_backend(0) == VJO_OCR_VOCR && vjo_engine_backend(1) == VJO_OCR_LENS);
    TEST_CHECK(vjo_engine_index(VJO_OCR_VOCR) == 0 && vjo_engine_index(VJO_OCR_LENS) == 1);
    TEST_CHECK(vjo_engine_backend(vjo_engine_index(VJO_OCR_LENS)) == VJO_OCR_LENS);
    TEST_CHECK(vjo_engine_backend(vjo_engine_index(VJO_OCR_VOCR)) == VJO_OCR_VOCR);
    VjoEngineCard c;
    memset(&c, 0, sizeof(c));
    strcpy(c.local, "VOCR3");
    TEST_CHECK(!strcmp(vjo_engine_name(&c, 0), "VOCR3") && !strcmp(vjo_engine_name(&c, 1), "Google Lens"));
    TEST_CHECK(!strcmp(vjo_engine_description(0), LOCAL_HELP) && !strcmp(vjo_engine_description(1), LENS_HELP));
    /* every config.ini engine value lands on one of the two buttons */
    static const char *const values[] = {"vocr", "lens", "vocr2", "meiki", "ncnn", "other"};
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        char ini[64];
        VjoConfig cfg;
        vjo_config_defaults(&cfg);
        snprintf(ini, sizeof(ini), "ocr_backend = %s\n", values[i]);
        vjo_config_parse(&cfg, ini, strlen(ini));
        TEST_CHECK_(cfg.ocr_backend == vjo_engine_backend(vjo_engine_index(cfg.ocr_backend)), "%s", values[i]);
        TEST_CHECK_(vjo_engine_index(cfg.ocr_backend) == !strcmp(values[i], "lens"), "%s", values[i]);
    }
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

TEST_LIST = {
    {"two_engines_the_local_one_named_VOCR3_after_its_model_generation", test_two_engines_local_named_after_its_generation},
    {"exit_ends_the_process_without_vita2d_fini", test_exit_ends_the_process_without_vita2d_fini},
    {"touching_a_button_chooses_and_saves_that_engine_gaps_only_focus", test_touch_chooses_and_saves_each_engine},
    {"left_right_choose_local_or_lens_cross_toggles", test_controller_left_right_choose_and_cross_toggles},
    {"removed_engine_VOCR2_Meiki_ncnn_shows_local_and_next_save_writes_vocr", test_removed_engine_shows_local_and_next_save_rewrites_it},
    {"label_and_status_follow_the_configured_models_and_files", test_label_and_status_follow_the_configured_models},
    {"unreadable_config_selects_and_saves_nothing", test_unreadable_config_selects_and_saves_nothing},
    {"engine_buttons_and_backends", test_engine_buttons_and_backends},
    {NULL, NULL}
};
#else
/* A later package (test table): VOCR5 pinned and made the default. */
static void test_later_generation_is_named_from_its_metadata(void)
{
    static const Input in[] = {OCR_PAGE};
    card("vocr_model = VOCR5_N_w8.vocr\nvocr_fallback_model = VOCR5_S_w8.vocr\n");
    run(in, 2);
    check_card(1, "VOCR5", 0, LOCAL_HELP, "VOCR5-N found. Game OCR worker required.", DIM);
    remove_model("VOCR5_N_w8.vocr");
    run(in, 2);
    check_card(1, "VOCR5", 0, LOCAL_HELP, "VOCR5-N missing; uses VOCR5-S. Game OCR worker required.", DIM);
    /* the same app with an older config.ini still names that file's generation */
    card(NULL);
    run(in, 2);
    check_card(1, "VOCR3", 0, LOCAL_HELP, "VOCR3-H15 found. Game OCR worker required.", DIM);
}
/* VOCR5 the default with an older generation's fallback: while the VOCR5 file is
 * missing or only partly uploaded, the game worker runs VOCR3-FL10, and the
 * button names VOCR3, not the VOCR5 it refuses. */
static void test_mixed_generations_name_the_model_that_loads(void)
{
    static const Input in[] = {OCR_PAGE};
    card("vocr_model = VOCR5_N_w8.vocr\nvocr_fallback_model = VOCR3_FL10_w8.vocr\n");
    run(in, 2);
    check_card(1, "VOCR5", 0, LOCAL_HELP, "VOCR5-N found. Game OCR worker required.", DIM);
    remove_model("VOCR5_N_w8.vocr");
    run(in, 2);
    check_card(1, "VOCR3", 0, LOCAL_HELP, "VOCR5-N missing; uses VOCR3-FL10. Game OCR worker required.", DIM);
    put_model("VOCR5_N_w8.vocr", 3000000 - 32768); /* one 32 KiB piece short */
    run(in, 2);
    check_card(1, "VOCR3", 0, LOCAL_HELP, "VOCR5-N missing; uses VOCR3-FL10. Game OCR worker required.", DIM);
    /* neither file there: nothing runs; the configured model's name and a red line */
    remove_model(VJO_VOCR3_FL10_NAME);
    run(in, 2);
    check_card(1, "VOCR5", 0, LOCAL_HELP, "VOCR5-N missing. Game OCR worker required.", OFF);
    /* the VOCR5 file back: it runs again, whatever its fallback */
    put_model("VOCR5_N_w8.vocr", 3000000);
    run(in, 2);
    check_card(1, "VOCR5", 0, LOCAL_HELP, "VOCR5-N found. Game OCR worker required.", DIM);
}
TEST_LIST = {
    {"a_later_generation_VOCR5_is_named_from_its_pinned_metadata_without_app_changes", test_later_generation_is_named_from_its_metadata},
    {"VOCR5_missing_with_a_VOCR3_fallback_names_VOCR3_the_model_that_loads", test_mixed_generations_name_the_model_that_loads},
    {NULL, NULL}
};
#endif
