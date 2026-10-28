/* The Rubi app's Look up page itself (settings/lookup_page.c, #313), on the host: its frames drawn through main.c's
 * helpers (VjoLookupUi), recorded here, over a temporary memory card with real files: config.ini (the app's own
 * loader), two small dictionaries (the bundled ones' names: a word, and the word as a character's name), history.csv
 * as the Shell writes it and the Anki queue. The keyboard is the SDK boundary of settings_stubs/search_dialog_stub.c.
 * Checked: Search (the keyboard, the entry and the stacked one, the cursor by word, by the stick and by a tap, the
 * scroll), a card from a typed text and from an earlier line (the records on the card), Earlier lines (the newest
 * game first, Square, a line opened and Cancel back), the modes by L/R and by touch, what each state's hints say
 * with either button assignment, no history yet, and the dictionaries' files closed when the page is left.
 *
 * Text widths are the settings harness's stub metrics (Latin 10, other characters 18 units per 18-unit em), not the
 * firmware's PVF fonts. Each test runs in its own process (acutest's default): the page keeps its state in statics,
 * as on the console. */
#define _GNU_SOURCE /* memmem */
#define main acutest_main
#include "acutest.h"
#undef main
#include "test_tmpdir.h"
#include <psp2/ctrl.h>
#include <psp2/ime_dialog.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
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
#include "settings_stubs/search_dialog_stub.h"
#include "../../core/anki_queue.h"
#include "../../settings/lookup_page.h"
#include "../../settings/preferences.h"
#include "../../core/utf.h"

/* ---------------- the memory card (fakes/sceio_posix.h) ---------------- */

#define SCEIO_OWN (SCEIO_OPEN | SCEIO_CLOSE)
#include "fakes/sceio_posix.h"
#include "fakes/vjdict_writer.h"
static int open_files;
int sceIoOpen(const char *p, int flags, unsigned mode)
{
    int fd = sceio_open(p, flags, mode);
    if (fd >= 0)
        open_files++;
    return fd;
}
int sceIoClose(int fd)
{
    open_files--;
    return sceio_close(fd);
}

/* The console's clock: 2026-10-27 15:04:05 UTC, nine hours ahead locally (Japan). */
#define NOW_UNIX 1793113445ull
#define LOCAL_OFFSET_US (9ull * 3600 * 1000000)
static void date_of(uint64_t unix_us, SceDateTime *d)
{
    time_t t = (time_t)(unix_us / 1000000);
    struct tm tm;
    gmtime_r(&t, &tm);
    *d = (SceDateTime){(unsigned short)(tm.tm_year + 1900), (unsigned short)(tm.tm_mon + 1), (unsigned short)tm.tm_mday,
                       (unsigned short)tm.tm_hour, (unsigned short)tm.tm_min, (unsigned short)tm.tm_sec,
                       (unsigned)(unix_us % 1000000)};
}
int sceRtcGetCurrentTick(SceRtcTick *t) { t->tick = 62135596800000000ull + NOW_UNIX * 1000000ull; return 0; }
int sceRtcGetCurrentClockLocalTime(SceDateTime *t) { date_of(NOW_UNIX * 1000000ull + LOCAL_OFFSET_US, t); return 0; }
int sceRtcConvertUtcToLocalTime(const SceRtcTick *utc, SceRtcTick *local)
{
    local->tick = utc->tick + LOCAL_OFFSET_US;
    return 0;
}
int sceRtcConvertLocalTimeToUtc(const SceRtcTick *local, SceRtcTick *utc) { utc->tick = local->tick - LOCAL_OFFSET_US; return 0; }
int sceRtcSetTick(SceDateTime *d, const SceRtcTick *t)
{
    if (t->tick < 62135596800000000ull)
        return -1;
    date_of(t->tick - 62135596800000000ull, d);
    return 0;
}
int sceRtcGetTick(const SceDateTime *d, SceRtcTick *t) { (void)d; t->tick = 0; return -1; }
static uint64_t now;
uint64_t sceKernelGetProcessTimeWide(void) { return now; }
int sceDisplayWaitVblankStart(void) { return 0; }

/* ---------------- the app's log ---------------- */

static char logs[8192];
void vjo_log(const char *fmt, ...)
{
    va_list ap;
    size_t n = strlen(logs);
    va_start(ap, fmt);
    vsnprintf(logs + n, sizeof(logs) - n - 1, fmt, ap);
    va_end(ap);
    n = strlen(logs);
    if (n + 1 < sizeof(logs))
        logs[n] = '\n', logs[n + 1] = '\0';
}
void vjo_log_enable(int on) { (void)on; }

/* ---------------- what a frame draws ---------------- */

#define WHITE 1u
#define DIM 2u
#define ACCENT 3u
#define OFF 4u
#define TRACK 5u
#define SELECTED 6u
#define HIGHLIGHT RGBA8(0xFF, 0xD2, 0x4A, 255)
#define READING RGBA8(0x9F, 0xD0, 0xFF, 255)
#define DEFINITION RGBA8(0xB8, 0xC0, 0xCC, 255)
typedef struct { int x, y; unsigned color; float px; int clipped; char s[600]; } Text;
typedef struct { int x, y, w, h; unsigned color; } Rect;
typedef struct { int y, h, focus; } Card;
typedef struct { int x, y, w, h, active; char label[64]; } Badge;
static struct {
    Text text[160];
    Rect rect[32];
    Card card[16];
    Badge badge[8];
    int texts, rects, cards, badges, headers, footers;
    char footer[256], controls[200];
    int footer_error;
} drawn;
static int frames_begun, clipping;
void vita2d_start_drawing(void) { memset(&drawn, 0, sizeof(drawn)); frames_begun++; }
void vita2d_clear_screen(void) {}
void vita2d_end_drawing(void) {}
void vita2d_swap_buffers(void) {}

/* A character whose first measure gives 0, as from a font whose glyph atlas is full (libvita2d skips the glyph). */
static const char *zero_once;
static int stub_width(float px, const char *s)
{
    if (zero_once && !strcmp(s, zero_once)) {
        zero_once = NULL;
        return 0;
    }
    int units = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p < 0x80) units += 10;
        else if ((*p & 0xC0) != 0x80) units += 18;
    return (int)(units * px / 18.0f + 0.5f);
}
static void d_text(int x, int y, unsigned color, float px, const char *s)
{
    if (drawn.texts < 160) {
        Text *t = &drawn.text[drawn.texts++];
        *t = (Text){x, y, color, px, clipping, ""};
        snprintf(t->s, sizeof(t->s), "%s", s);
    }
}
static void d_wrapped(int x, int y, int width, unsigned color, float px, int line_height, int lines, const char *s)
{
    (void)width; (void)line_height; (void)lines;
    d_text(x, y, color, px, s);
}
static void d_rect(int x, int y, int w, int h, unsigned color)
{
    if (drawn.rects < 32) drawn.rect[drawn.rects++] = (Rect){x, y, w, h, color};
}
static void d_card(int y, int h, int focus)
{
    if (drawn.cards < 16) drawn.card[drawn.cards++] = (Card){y, h, focus};
}
static void d_badge(int x, int y, int w, int h, const char *label, int active)
{
    if (drawn.badges < 8) {
        drawn.badge[drawn.badges] = (Badge){x, y, w, h, active, ""};
        snprintf(drawn.badge[drawn.badges++].label, 64, "%s", label);
    }
}
static void d_footer(int y, int height, int text_y, int lines, const char *message, int error, const char *controls)
{
    (void)y; (void)height; (void)text_y; (void)lines;
    drawn.footers++;
    snprintf(drawn.footer, sizeof(drawn.footer), "%s", message);
    snprintf(drawn.controls, sizeof(drawn.controls), "%s", controls);
    drawn.footer_error = error;
}
static void d_header(void) { drawn.headers++; }
static void d_clip(int x0, int y0, int x1, int y1) { (void)y0; (void)y1; clipping = x1 > x0; }
static VjoButtons buttons;
static const VjoLookupUi ui = {d_text, stub_width, d_wrapped, d_rect, d_card, d_badge, d_footer, d_header, d_clip,
                               WHITE, DIM, ACCENT, OFF, TRACK, SELECTED, &buttons};

static const Text *shown(const char *s)
{
    for (int i = 0; i < drawn.texts; i++)
        if (!strcmp(drawn.text[i].s, s)) return &drawn.text[i];
    return NULL;
}
static const Text *shown_with(const char *s)
{
    for (int i = 0; i < drawn.texts; i++)
        if (strstr(drawn.text[i].s, s)) return &drawn.text[i];
    return NULL;
}
static const Badge *badge_named(const char *label)
{
    for (int i = 0; i < drawn.badges; i++)
        if (!strcmp(drawn.badge[i].label, label)) return &drawn.badge[i];
    return NULL;
}

/* ---------------- frames ---------------- */

static unsigned char scratch_mem[64 * 1024];
static VjoArena scratch;
static uint32_t held;
static int frame_full(uint32_t pressed, uint8_t lx, uint8_t ry, int x, int y, int stick_rows)
{
    VjoLookupInput in;
    now += 33333; /* 30 Hz */
    held |= pressed;
    in = (VjoLookupInput){pressed, held, lx, 128, 128, ry, now, x, y, stick_rows};
    int rc = vjo_lookup_page_frame(&ui, &in);
    held &= ~pressed; /* let go after the frame, unless a test holds it */
    return rc;
}
static int frame(uint32_t pressed) { return frame_full(pressed, 128, 128, -1, -1, 0); }
static int tap(int x, int y) { return frame_full(0, 128, 128, x, y, 0); }
static void show(void)
{
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    vjo_lookup_page_show(&scratch);
}
/* Frames until the page's scan of history.csv is done (the list's status says so). */
static void settle(void)
{
    for (int i = 0; i < 200; i++) {
        frame(0);
        if (!strstr(drawn.footer, "Reading history.csv")) return;
    }
    TEST_CHECK_(0, "history.csv's scan does not end");
}

/* The keyboard's answer: what the user types (UTF-8, as UTF-16 units), Enter. */
static void keyboard_types(const char *s)
{
    size_t i = 0, n = strlen(s);
    int k = 0;
    memset(vjo_test_ime_reply, 0, sizeof(vjo_test_ime_reply));
    while (i < n && k < 128) {
        uint32_t cp = vjo_utf8_next(s, n, &i);
        vjo_test_ime_reply[k++] = (uint16_t)cp; /* (BMP only here) */
    }
    vjo_test_ime_button = SCE_IME_DIALOG_BUTTON_ENTER;
}

/* ---------------- the card's files: dictionaries, history.csv, the queue ---------------- */

#define DICT_DIR "ux0:data/VitaJPOverlay/dictionaries"
/* A VJDICT2 file of one key with one entry (fakes/vjdict_writer.h), its own name in page 0. */
static void put_dictionary(const char *name, const char *key, const char *spelling, const char *reading,
                           const char *meaning, const char *title)
{
    static uint8_t bytes[16384];
    size_t off = vjdict2_one(bytes, sizeof(bytes), key, spelling, reading, meaning, title);
    char path[256];
    FILE *f;
    TEST_ASSERT(off > 0);
    snprintf(path, sizeof(path), DICT_DIR "/%s", name);
    TEST_ASSERT((f = fopen(path, "wb")) != NULL);
    TEST_ASSERT(fwrite(bytes, 1, off, f) == off && fclose(f) == 0);
}

/* 猫's entry: twelve numbered senses (more than the entry card's rows), then its kanji line */
#define CAT_MEANING "[Bee's Ultimate Dictionary]\n1. cat\n2. a cat-like animal\n3. shamisen (from its cat skin)\n" \
    "4. geisha\n5. (Kansai) a hot-water bottle\n6. sense six\n7. sense seven\n8. sense eight\n9. sense nine\n" \
    "10. sense ten\n11. sense eleven\n12. sense twelve\n\n猫=cat, feline"
static void reset(int assign)
{
    char data[PATH_MAX];
    TEST_ASSERT(snprintf(data, sizeof(data), "%s/ux0:data", vjo_test_tmpdir_path) < (int)sizeof(data));
    TEST_ASSERT(vjo_test_remove_tree(data) == 0);
    TEST_ASSERT(mkdir("ux0:data", 0777) == 0 && mkdir("ux0:data/VitaJPOverlay", 0777) == 0 && mkdir(DICT_DIR, 0777) == 0);
    /* the defaults' two files (config.ini none: local_dictionaries = plaintext.vjdict,vndb-names.vjdict, 2 shown) */
    put_dictionary("plaintext.vjdict", "猫", "猫", "ねこ", CAT_MEANING, "Bee's Ultimate Dictionary");
    put_dictionary("vndb-names.vjdict", "猫", "猫", "ねこ", "[VNDB names]\nNeko, a character", "VNDB names");
    buttons = vjo_buttons(assign);
    logs[0] = '\0';
    vjo_test_ime_calls = 0;
    vjo_test_ime_button = SCE_IME_DIALOG_BUTTON_CLOSE;
    held = 0;
}

/* history.csv as shell/history.c writes it: the first five columns, then platform, title ID, engine, model and more. */
static FILE *csv;
static void field(const char *s)
{
    fputc('"', csv);
    for (; *s; s++) {
        if (*s == '"') fputc('"', csv);
        fputc(*s, csv);
    }
    fputc('"', csv);
}
static void history_line(const char *game, const char *line, uint64_t unix_s)
{
    int clannad = !strcmp(game, "CLANNAD");
    const char *platform = clannad ? "vita" : "psp", *tid = clannad ? "PCSG00415" : "ULJM05000";
    char uuid[32];
    if (!csv) {
        TEST_ASSERT((csv = fopen("ux0:data/VitaJPOverlay/history.csv", "wb")) != NULL);
        fputs("uuid,given_identifier,name,line,time,platform,title_id,engine,model,raw_line\n", csv);
    }
    snprintf(uuid, sizeof(uuid), "%s-%s", platform, tid);
    field(uuid);
    fputs(",\"vjo-1-0\",", csv);
    field(game);
    fputc(',', csv);
    field(line);
    fprintf(csv, ",%llu.000000,", (unsigned long long)unix_s);
    field(platform);
    fputc(',', csv);
    field(tid);
    fputs(",\"vocr\",\"VOCR5_N_w8.vocr\",", csv);
    field(line);
    fputc('\n', csv);
}
static void history_done(void)
{
    TEST_ASSERT(fclose(csv) == 0);
    csv = NULL;
}

static int starts(const char *s, const char *prefix) { return s && !strncmp(s, prefix, strlen(prefix)); }

/* The queue's records, decoded: their number, and (out) the one whose sentence is that (NULL: any), decoded once the
 * directory is read (its order is the file system's: tmpfs's differs from a disk's), with misc and game pointing at
 * those keys in its JSON (the game and the OCR facts are in the file, not in the note the export reads). */
static int queued(VjoAnkiNote *out, const char *sentence)
{
    static uint8_t mem[256 * 1024], keep[256 * 1024];
    static char raw[VJO_QUEUE_TEXT_MAX + 1];
    VjoArena a;
    DIR *d = opendir(VJO_QUEUE_DIR);
    struct dirent *e;
    size_t raw_len = 0;
    int n = 0, found = 0, picture = 1;
    if (!d) return 0;
    while ((e = readdir(d))) {
        char path[512], json[VJO_QUEUE_TEXT_MAX + 1];
        FILE *f;
        size_t len;
        VjoAnkiNote note;
        if (strlen(e->d_name) != 37 || strcmp(e->d_name + 32, ".json")) continue;
        n++;
        snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s", e->d_name);
        TEST_ASSERT((f = fopen(path, "rb")) != NULL);
        len = fread(json, 1, sizeof(json) - 1, f);
        fclose(f);
        json[len] = '\0';
        vjo_arena_init(&a, mem, sizeof(mem));
        picture = 1;
        TEST_ASSERT(vjo_queue_decode(&a, json, len, &note, &picture) == 4 && !picture);
        TEST_CHECK(strstr(json, "\"picture\":false") != NULL);
        if (out && !found && (!sentence || !strcmp(note.sentence, sentence))) {
            memcpy(raw, json, len + 1);
            raw_len = len;
            found = 1;
        }
    }
    closedir(d);
    if (out && found) {
        vjo_arena_init(&a, keep, sizeof(keep));
        TEST_ASSERT(vjo_queue_decode(&a, raw, raw_len, out, &picture) == 4);
        out->misc = strstr(raw, "\"misc\":");
        out->game = strstr(raw, "\"game\":");
    }
    TEST_CHECK_(!out || found, "no queued card has the sentence '%s'", sentence ? sentence : "(any)");
    return n;
}

/* ---------------- tests ---------------- */

/* Search: Triangle opens the keyboard (128 characters, every keyboard); the typed text is looked up at its first
 * character, the entry and, below it, the other dictionary's entry for the word, as the overlay shows them. */
static void search_by_the_keyboard(void)
{
    reset(VJO_BUTTON_ASSIGN_CROSS);
    show();
    TEST_ASSERT(frame(0) == 0);
    TEST_CHECK(drawn.headers == 1 && badge_named("Search")->active && !badge_named("Earlier lines")->active);
    TEST_CHECK(shown("Triangle, or a tap here: type a word or a sentence.") != NULL);
    TEST_CHECK(shown_with("Type a word or a sentence (Triangle)") != NULL);
    TEST_CHECK(!strcmp(drawn.controls, "Triangle or ×: type   L/R: earlier lines   Select: page   ○: exit"));
    keyboard_types("猫が好き");
    TEST_CHECK(frame(SCE_CTRL_TRIANGLE) == 1); /* the keyboard ran in the frame */
    TEST_CHECK(vjo_test_ime_calls == 1 && vjo_test_ime_max == 128 && vjo_test_ime_languages == 0x1FFFF &&
               vjo_test_ime_forced == 1 && vjo_test_ime_updates >= 1);
    static const uint16_t title[] = {'L','o','o','k',' ','u','p',' ','a',' ','w','o','r','d',' ','o','r',' ','a',' ',
                                     's','e','n','t','e','n','c','e',0};
    TEST_CHECK(!memcmp(vjo_test_ime_title, title, sizeof(title)));
    frame(0);
    /* the text: 猫 highlighted (the matched word) and underlined, the rest white */
    const Text *word = shown("猫"), *rest = shown("が好き");
    TEST_ASSERT(word && rest);
    TEST_CHECK(word->color == HIGHLIGHT && word->x == 42 && word->px == 26 && rest->color == WHITE &&
               rest->x == 42 + stub_width(26, "猫") && rest->y == word->y);
    /* the entry: the headword and its reading, the senses, the kanji line; the other dictionary's below */
    int heads = 0;
    for (int i = 0; i < drawn.texts; i++)
        heads += !strcmp(drawn.text[i].s, "猫") && drawn.text[i].color == HIGHLIGHT && drawn.text[i].y > word->y;
    TEST_CHECK_(heads >= 1, "%d headwords", heads);
    const Text *reading = shown(" (ねこ)"), *sense = shown("1. cat"), *label = shown("[Bee's Ultimate Dictionary]");
    TEST_CHECK(reading && reading->color == READING && sense && sense->color == DEFINITION && sense->px == 22 && label);
    TEST_CHECK(!strcmp(drawn.controls, "Triangle: type   Left/Right: word   ×: save card   L/R: lines   ○: exit"));
    TEST_CHECK(!drawn.footer[0] && !drawn.footer_error);
    /* the entry is longer than its card: a bar right of it */
    int bars = 0;
    for (int i = 0; i < drawn.rects; i++)
        bars += drawn.rect[i].x == 922 && drawn.rect[i].color == TRACK;
    TEST_CHECK(bars == 1);
    /* scrolled to its end: the kanji line and the name dictionary's entry */
    for (int i = 0; i < 30; i++)
        frame(SCE_CTRL_DOWN);
    const Text *kanji = NULL;
    for (int i = 0; i < drawn.texts; i++)
        if (!strcmp(drawn.text[i].s, "猫") && drawn.text[i].color == WHITE) kanji = &drawn.text[i];
    const Text *meanings = shown("=cat, feline"), *name = shown("Neko, a character"), *names = shown("[VNDB names]");
    TEST_CHECK(kanji && meanings && meanings->y == kanji->y && meanings->x == 42 + stub_width(26, "猫") &&
               meanings->color == DEFINITION && name && names);
    TEST_CHECK(!shown("1. cat")); /* scrolled out */
    /* never past the last rows: more Down changes nothing */
    int y = name ? name->y : 0;
    frame(SCE_CTRL_DOWN);
    TEST_CHECK(shown("Neko, a character") && shown("Neko, a character")->y == y);
    /* the right stick scrolls back up */
    for (int i = 0; i < 40; i++)
        frame_full(0, 128, 0, -1, -1, 0);
    TEST_CHECK(shown("1. cat") != NULL);
}

/* Left/Right go by the matched word, the stick a character (its word looked up once it rests), a tap to the
 * character touched; the highlight is the word, or the character where nothing matches. */
static void the_cursor_by_word_stick_and_tap(void)
{
    reset(VJO_BUTTON_ASSIGN_CROSS);
    show();
    keyboard_types("猫が好き");
    frame(SCE_CTRL_TRIANGLE);
    frame(SCE_CTRL_RIGHT); /* past 猫: が */
    TEST_CHECK(shown("が") && shown("が")->color == HIGHLIGHT && shown("猫") && shown("猫")->color == WHITE);
    TEST_CHECK(shown("No entry for this word.") != NULL);
    frame(SCE_CTRL_RIGHT); /* 好 */
    TEST_CHECK(shown("好") && shown("好")->color == HIGHLIGHT);
    frame(SCE_CTRL_LEFT); /* back along the way Right came */
    frame(SCE_CTRL_LEFT);
    TEST_CHECK(shown("猫") && shown("猫")->color == HIGHLIGHT && shown(" (ねこ)"));
    /* the stick right: a character at once, the entry kept until it rests (200 ms: config.ini stick_lookup_ms) */
    frame_full(0, 255, 128, -1, -1, 0);
    TEST_CHECK(shown("が") && shown("が")->color == HIGHLIGHT && shown(" (ねこ)")); /* not looked up yet */
    for (int i = 0; i < 8; i++)
        frame(0);
    TEST_CHECK(shown("No entry for this word.") && !shown(" (ねこ)"));
    /* a tap on 猫 */
    tap(42 + 5, 160 + 20);
    TEST_CHECK(shown("猫") && shown("猫")->color == HIGHLIGHT && shown(" (ねこ)"));
    /* a tap past the row's end: its last character */
    tap(800, 160 + 20);
    TEST_CHECK(shown("き") && shown("き")->color == HIGHLIGHT);
}

/* Enter saves the shown entry as a card: the overlay's record, without a picture; a typed text longer than the
 * word is its sentence, a typed word alone has none; no game, MiscInfo the date it was saved. The same card again
 * is queued already. Where nothing matches, Enter says so; with no text, it opens the keyboard. */
static void a_card_from_a_typed_text(void)
{
    VjoAnkiNote note;
    reset(VJO_BUTTON_ASSIGN_CROSS);
    show();
    keyboard_types("猫が好き");
    frame(SCE_CTRL_TRIANGLE);
    frame(SCE_CTRL_CROSS);
    TEST_CHECK(!strcmp(drawn.footer, "Saved for Anki, without a picture: export it on the Anki page.") &&
               !drawn.footer_error);
    TEST_ASSERT(queued(&note, NULL) == 1);
    TEST_CHECK(!strcmp(note.spelling, "猫") && !strcmp(note.reading, "ねこ") && !strcmp(note.sentence, "猫が好き") &&
               note.hl_start == 0 && note.hl_end == 3 && note.n_more == 1 && !strcmp(note.more[0].dictionary, "VNDB names"));
    TEST_CHECK(starts(note.misc, "\"misc\":\"2026-10-28 00:04\"")); /* the local date it was saved */
    TEST_CHECK(starts(note.game, "\"game\":null,\"title_id\":null,\"platform\":null,\"ocr_engine\":null"));
    TEST_CHECK(strstr(logs, "look up: card from a typed text rc=0") && !strstr(logs, "猫"));
    frame(SCE_CTRL_CROSS);
    TEST_CHECK(!strcmp(drawn.footer, "Already queued") && queued(NULL, NULL) == 1);
    /* the word alone */
    keyboard_types("猫");
    frame(SCE_CTRL_TRIANGLE);
    frame(SCE_CTRL_CROSS);
    TEST_ASSERT(queued(&note, "") == 2);
    TEST_CHECK(!strcmp(note.sentence, "") && note.hl_start == -1);
    /* on が nothing matches: Enter says so, saves nothing and opens no keyboard */
    keyboard_types("猫が");
    frame(SCE_CTRL_TRIANGLE);
    frame(SCE_CTRL_RIGHT);
    int calls = vjo_test_ime_calls;
    TEST_CHECK(frame(SCE_CTRL_CROSS) == 0 && vjo_test_ime_calls == calls && queued(NULL, NULL) == 2);
    TEST_CHECK(!strcmp(drawn.footer, "No entry here to save.") && !drawn.footer_error);
    /* a keyboard cancelled: the text stays */
    vjo_test_ime_button = SCE_IME_DIALOG_BUTTON_CLOSE;
    TEST_CHECK(frame(SCE_CTRL_TRIANGLE) == 1 && vjo_test_ime_calls == calls + 1);
    frame(0);
    TEST_CHECK(shown("猫") && shown("が"));
    /* with no text, Enter is the keyboard */
    keyboard_types("");
    frame(SCE_CTRL_TRIANGLE);
    TEST_CHECK(frame(SCE_CTRL_CROSS) == 1 && vjo_test_ime_calls == calls + 3);
    /* an empty text accepted: nothing to look up */
    keyboard_types("");
    frame(SCE_CTRL_TRIANGLE);
    frame(0);
    TEST_CHECK(shown("Triangle, or a tap here: type a word or a sentence.") != NULL);
    /* the queue cannot be written (its place is a file): said, and nothing saved */
    TEST_ASSERT(vjo_test_remove_tree(VJO_QUEUE_DIR) == 0);
    FILE *f = fopen(VJO_QUEUE_DIR, "wb");
    TEST_ASSERT(f && !fclose(f));
    keyboard_types("猫");
    frame(SCE_CTRL_TRIANGLE);
    frame(SCE_CTRL_CROSS);
    TEST_CHECK(!strcmp(drawn.footer, "Card not saved: cannot write ux0:data/VitaJPOverlay/anki_queue (space?)") &&
               drawn.footer_error);
}

/* Earlier lines: the newest game's lines, newest first, with their local times; Square the next game; Enter opens a
 * line with the word cursor; Enter saves its word with the line, its game and its date; Cancel goes back. */
static void earlier_lines_and_a_card_from_one(void)
{
    VjoAnkiNote note;
    reset(VJO_BUTTON_ASSIGN_CROSS);
    history_line("AIR", "空を見た", 1793050000);
    history_line("CLANNAD", "猫がいた", 1793080000);          /* 2026-10-27 05:46:40 UTC: 14:46 here */
    history_line("CLANNAD", "朋也\n「猫が鳴いた」", 1793090000); /* 17:33 */
    history_line("CLANNAD", "English only", 1793090100);   /* not shown: no Japanese (non_japanese_filter) */
    history_done();
    show();
    frame(0);
    frame(SCE_CTRL_RTRIGGER);
    TEST_CHECK(badge_named("Earlier lines")->active && !badge_named("Search")->active);
    settle();
    TEST_CHECK(shown("朋也 「猫が鳴いた」") && shown("猫がいた") && !shown("English only") && !shown("空を見た"));
    TEST_CHECK(shown("10-27 17:33") && shown("10-27 14:46") && shown("CLANNAD  (Square: game)"));
    TEST_CHECK(drawn.cards >= 2 && drawn.card[0].focus && !drawn.card[1].focus);
    TEST_CHECK(!strcmp(drawn.footer, "2 lines, newest first: × opens one."));
    TEST_CHECK(!strcmp(drawn.controls, "Up/Down: line   ×: open   Square: game   L/R: search   ○: exit"));
    TEST_CHECK(!vjo_lookup_page_holds_cancel());
    /* the other game, and back */
    frame(SCE_CTRL_SQUARE);
    settle();
    TEST_CHECK(shown("空を見た") && !shown("猫がいた") && shown("AIR  (Square: game)"));
    frame(SCE_CTRL_SQUARE);
    settle();
    /* the second line, opened */
    frame(SCE_CTRL_DOWN);
    TEST_CHECK(drawn.card[1].focus);
    frame(SCE_CTRL_CROSS);
    TEST_CHECK(vjo_lookup_page_holds_cancel() && shown("CLANNAD  10-27 14:46"));
    TEST_CHECK(shown("猫") && shown("猫")->color == HIGHLIGHT && shown("がいた") && shown(" (ねこ)"));
    TEST_CHECK(!strcmp(drawn.controls, "Left/Right: word   Up/Down: scroll   ×: save card   ○: back"));
    frame(SCE_CTRL_CROSS);
    TEST_CHECK(!strcmp(drawn.footer, "Saved for Anki, without a picture: export it on the Anki page."));
    TEST_ASSERT(queued(&note, "猫がいた") == 1);
    TEST_CHECK(note.hl_start == 0 && note.hl_end == 3 && starts(note.misc, "\"misc\":\"CLANNAD (2026-10-27 14:46)\""));
    TEST_CHECK(starts(note.game, "\"game\":\"CLANNAD\",\"title_id\":\"PCSG00415\",\"platform\":\"vita\",\"ocr_engine\":"
                                 "\"vocr\",\"ocr_model\":\"VOCR5_N_w8.vocr\""));
    TEST_CHECK(strstr(logs, "look up: card from an earlier line rc=0") != NULL);
    /* L/R to Search and back: the line is still open */
    frame(SCE_CTRL_LTRIGGER);
    TEST_CHECK(badge_named("Search")->active && !vjo_lookup_page_holds_cancel());
    frame(SCE_CTRL_RTRIGGER);
    TEST_CHECK(vjo_lookup_page_holds_cancel() && shown("がいた"));
    /* Cancel: the list again, the line it was on still selected */
    frame(SCE_CTRL_CIRCLE);
    TEST_CHECK(!vjo_lookup_page_holds_cancel() && shown("猫がいた") && drawn.card[1].focus);
    /* the first line, a multi-line one: its line break kept in the text, a space in the list */
    frame(SCE_CTRL_UP);
    frame(SCE_CTRL_CROSS);
    TEST_CHECK(shown("朋") && shown("朋")->color == HIGHLIGHT && shown("也") && shown("「猫が鳴いた」") &&
               shown("「猫が鳴いた」")->y == shown("朋")->y + 32); /* its second row */
}

/* The modes by touch, a line opened by a tap, the game by a tap on its name. */
static void touches(void)
{
    reset(VJO_BUTTON_ASSIGN_CROSS);
    history_line("AIR", "空を見た", 1793050000);
    history_line("CLANNAD", "猫がいた", 1793080000);
    history_done();
    show();
    frame(0);
    tap(300, 112 + 20); /* Earlier lines */
    TEST_CHECK(badge_named("Earlier lines")->active);
    settle();
    tap(700, 112 + 20); /* the game's name: the next game */
    settle();
    TEST_CHECK(shown("空を見た") && shown("AIR  (Square: game)"));
    tap(700, 112 + 20);
    settle();
    tap(400, 160 + 20); /* the first line */
    TEST_CHECK(vjo_lookup_page_holds_cancel() && shown("がいた"));
    tap(300, 112 + 20); /* its mode's button: back to the list */
    TEST_CHECK(!vjo_lookup_page_holds_cancel() && shown("猫がいた"));
    tap(100, 112 + 20); /* Search */
    TEST_CHECK(badge_named("Search")->active);
    tap(400, 160 + 20); /* the empty text: the keyboard */
    TEST_CHECK(vjo_test_ime_calls == 1);
}

/* No history.csv yet: the page says what records it; Start reads it again once there is one. And the Japanese
 * button assignment (○ enters): the hints name it. */
static void no_history_yet_and_the_buttons_named(void)
{
    reset(VJO_BUTTON_ASSIGN_CIRCLE);
    show();
    frame(0);
    TEST_CHECK(!strcmp(drawn.controls, "Triangle or ○: type   L/R: earlier lines   Select: page   ×: exit"));
    frame(SCE_CTRL_RTRIGGER);
    settle();
    TEST_CHECK(shown_with("No earlier lines yet: Rubi keeps the lines it reads with Read text: Automatically") != NULL);
    history_line("CLANNAD", "猫がいた", 1793080000);
    history_done();
    frame(SCE_CTRL_START);
    settle();
    TEST_CHECK(shown("猫がいた") && !strcmp(drawn.controls, "Up/Down: line   ○: open   Square: game   L/R: search   ×: exit"));
    frame(SCE_CTRL_CIRCLE); /* ○ opens */
    TEST_CHECK(vjo_lookup_page_holds_cancel() &&
               !strcmp(drawn.controls, "Left/Right: word   Up/Down: scroll   ○: save card   ×: back"));
    frame(SCE_CTRL_CROSS); /* × goes back */
    TEST_CHECK(!vjo_lookup_page_holds_cancel());
}

/* The page left: the dictionaries' files closed (an install may replace them); shown again, opened as needed. And
 * a config.ini whose dictionaries changed is read when the page is shown. */
static void leaving_closes_the_dictionaries(void)
{
    reset(VJO_BUTTON_ASSIGN_CROSS);
    open_files = 0;
    show();
    keyboard_types("猫が好き");
    frame(SCE_CTRL_TRIANGLE);
    frame(0);
    TEST_CHECK_(open_files == 2, "%d files open", open_files); /* the session's two dictionaries */
    vjo_lookup_page_leave();
    TEST_CHECK(open_files == 0);
    /* one dictionary only now: no stacked entry */
    FILE *f = fopen(VJO_PREFERENCES_PATH, "wb");
    TEST_ASSERT(f && fputs("local_dictionaries = plaintext.vjdict\nlocal_dictionary_results = 1\n", f) >= 0 && !fclose(f));
    show();
    frame(0);
    for (int i = 0; i < 30; i++)
        frame(SCE_CTRL_DOWN);
    TEST_CHECK(shown("=cat, feline") && !shown("Neko, a character") && open_files == 1);
    vjo_lookup_page_quit();
    TEST_CHECK(open_files == 0);
}

/* Each mode keeps its text and cursor (the guide): Search's cursor stays where it was when an earlier line is opened
 * and L goes back to Search (the audit's F1), and the line's when L goes back to it. */
static void each_mode_keeps_its_cursor(void)
{
    reset(VJO_BUTTON_ASSIGN_CROSS);
    history_line("CLANNAD", "猫がいた", 1793080000);
    history_done();
    show();
    keyboard_types("猫が好き");
    frame(SCE_CTRL_TRIANGLE);
    frame(SCE_CTRL_RIGHT);
    frame(SCE_CTRL_RIGHT); /* 好 */
    TEST_ASSERT(shown("好") && shown("好")->color == HIGHLIGHT);
    frame(SCE_CTRL_RTRIGGER); /* Earlier lines: the list */
    settle();
    frame(SCE_CTRL_CROSS); /* the line opened */
    TEST_CHECK(shown("猫") && shown("猫")->color == HIGHLIGHT && shown("がいた"));
    frame(SCE_CTRL_RIGHT); /* its が */
    TEST_CHECK(shown("が") && shown("が")->color == HIGHLIGHT);
    frame(SCE_CTRL_LTRIGGER); /* back to Search: still on 好 */
    TEST_CHECK_(shown("好") && shown("好")->color == HIGHLIGHT, "Search's cursor: %s highlighted",
                shown("猫") && shown("猫")->color == HIGHLIGHT ? "猫" : "not 好");
    frame(SCE_CTRL_RTRIGGER); /* the line again: still on が */
    TEST_CHECK(shown("が") && shown("が")->color == HIGHLIGHT && shown("猫") && shown("猫")->color == WHITE);
}

/* An opened line is shown as the overlay shows a text: its rows without Japanese left out (non_japanese_filter, the
 * default), so the card's sentence is the dialogue (the audit's M6); a typed text is kept as typed. */
static void an_opened_line_is_filtered_as_the_overlay_shows_it(void)
{
    VjoAnkiNote note;
    reset(VJO_BUTTON_ASSIGN_CROSS);
    history_line("CLANNAD", "Tomoya\n「猫が鳴いた」", 1793080000);
    history_done();
    show();
    frame(SCE_CTRL_RTRIGGER);
    settle();
    TEST_CHECK(shown("「猫が鳴いた」") != NULL); /* the list's start: the line as shown */
    frame(SCE_CTRL_CROSS);
    TEST_CHECK(!shown_with("Tomoya") && shown("「") && shown("猫が鳴いた」"));
    frame(SCE_CTRL_RIGHT); /* 猫 */
    frame(SCE_CTRL_CROSS);
    TEST_ASSERT(queued(&note, "「猫が鳴いた」") == 1);
    /* typed: kept as typed */
    frame(SCE_CTRL_LTRIGGER);
    keyboard_types("Tomoya 猫");
    frame(SCE_CTRL_TRIANGLE);
    frame(0); /* (the keyboard's frames drew the page as it was) */
    TEST_CHECK(shown("T") && shown("T")->color == HIGHLIGHT && shown("omoya 猫")); /* (its pieces around the cursor) */
}

/* A line read again must be the same record: another game's of the same size and time at its place (the file
 * replaced) is no line (the audit's M12), and the list is read again. */
static void a_replaced_line_of_another_game_is_not_opened(void)
{
    reset(VJO_BUTTON_ASSIGN_CROSS);
    history_line("CLANNAD", "猫がいた", 1793080000);
    history_done();
    show();
    frame(SCE_CTRL_RTRIGGER);
    settle();
    /* the file replaced: the same bytes but the game's (vita-PCSG00415 -> vita-PCSG00416, same length, same time) */
    FILE *f = fopen("ux0:data/VitaJPOverlay/history.csv", "r+b");
    char buf[4096];
    TEST_ASSERT(f);
    size_t n = fread(buf, 1, sizeof(buf), f);
    char *at = memmem(buf, n, "vita-PCSG00415", 14);
    TEST_ASSERT(at && at == strstr(buf, "\n\"vita-PCSG00415") + 2);
    at[13] = '6';
    TEST_ASSERT(fseek(f, 0, SEEK_SET) == 0 && fwrite(buf, 1, n, f) == n && fclose(f) == 0);
    frame(SCE_CTRL_CROSS);
    TEST_CHECK(!vjo_lookup_page_holds_cancel() && !strcmp(drawn.footer, "history.csv changed: it is read again.") &&
               drawn.footer_error);
}

/* A card keeps the other dictionaries' entries while its record stays within 32 KiB (the overlay's rule): eight
 * dictionaries of 7 KB entries for the word, the card saved with fewer (the audit's M22). */
static void a_card_keeps_stacked_entries_while_it_fits(void)
{
    static char big[7001];
    VjoAnkiNote note;
    char names[512] = "", name[32];
    reset(VJO_BUTTON_ASSIGN_CROSS);
    for (int d = 0; d < 8; d++) {
        memset(big, 'a' + d, sizeof(big) - 1);
        memcpy(big, "[D]\n", 4);
        snprintf(name, sizeof(name), "d%d.vjdict", d);
        put_dictionary(name, "猫", "猫", "ねこ", big, name);
        snprintf(names + strlen(names), sizeof(names) - strlen(names), "%s%s", d ? "," : "", name);
    }
    FILE *f = fopen(VJO_PREFERENCES_PATH, "wb");
    TEST_ASSERT(f && fprintf(f, "local_dictionaries = %s\nlocal_dictionary_results = 8\n", names) > 0 && !fclose(f));
    show();
    keyboard_types("猫");
    frame(SCE_CTRL_TRIANGLE);
    frame(SCE_CTRL_CROSS);
    TEST_CHECK_(!strcmp(drawn.footer, "Saved for Anki, without a picture: export it on the Anki page."), "%s",
                drawn.footer);
    TEST_ASSERT(queued(&note, "") == 1);
    TEST_CHECK_(note.n_more >= 1 && note.n_more < 7, "%d other entries kept", note.n_more);
    struct stat st;
    char path[600];
    DIR *d = opendir(VJO_QUEUE_DIR);
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (strlen(e->d_name) == 37) {
            snprintf(path, sizeof(path), VJO_QUEUE_DIR "/%s", e->d_name);
            TEST_CHECK(stat(path, &st) == 0 && st.st_size <= VJO_QUEUE_TEXT_MAX && st.st_size > 7000 * 2);
        }
    if (d)
        closedir(d);
}

/* Left/Right go round the text's ends only on one displayed row, and for a fresh press (the overlay's rule): a typed
 * text wrapped into two rows stops at its last word (the audit's N4). */
static void the_ends_go_round_on_one_row_only(void)
{
    char longer[512] = "";
    reset(VJO_BUTTON_ASSIGN_CROSS);
    show();
    keyboard_types("猫が好き"); /* one row */
    frame(SCE_CTRL_TRIANGLE);
    frame(SCE_CTRL_LEFT); /* from the first word, a fresh Left: round to the last */
    TEST_CHECK(shown("き") && shown("き")->color == HIGHLIGHT);
    for (int i = 0; i < 60; i++) /* 60 kana: wider than a row of 872 px at 26 px (stub metrics) */
        strcat(longer, "あ");
    strcat(longer, "猫");
    keyboard_types(longer);
    frame(SCE_CTRL_TRIANGLE);
    frame(SCE_CTRL_LEFT); /* two rows: Left at the first character stays */
    const Text *first = NULL;
    for (int i = 0; i < drawn.texts; i++)
        if (drawn.text[i].color == HIGHLIGHT && drawn.text[i].y < 240) first = &drawn.text[i];
    TEST_CHECK_(first && first->x == 42 && !strcmp(first->s, "あ"), "highlighted: %s at %d", first ? first->s : "(none)",
                first ? first->x : -1);
}

/* A width a font could not give (0: its glyph atlas full) is not kept: the character is measured again, and the text
 * after it is placed by its real width (the audit's F2). */
static void a_zero_width_is_measured_again(void)
{
    reset(VJO_BUTTON_ASSIGN_CROSS);
    show();
    keyboard_types("猫が好き");
    zero_once = "好";
    frame(SCE_CTRL_TRIANGLE);
    frame(0);
    TEST_CHECK(zero_once == NULL); /* it was measured, and gave 0 */
    frame(SCE_CTRL_RIGHT);
    frame(SCE_CTRL_RIGHT); /* 好 highlighted: き after it, at its real place */
    const Text *ki = shown("き");
    TEST_CHECK_(ki && ki->x == 42 + stub_width(26, "猫が好"), "き at %d, not %d", ki ? ki->x : -1,
                42 + stub_width(26, "猫が好"));
}

TEST_LIST = {
    {"a zero width is measured again", a_zero_width_is_measured_again},
    {"each mode keeps its text and cursor", each_mode_keeps_its_cursor},
    {"an opened line is filtered as the overlay shows it", an_opened_line_is_filtered_as_the_overlay_shows_it},
    {"a replaced line of another game is not opened", a_replaced_line_of_another_game_is_not_opened},
    {"a card keeps stacked entries while it fits 32 KiB", a_card_keeps_stacked_entries_while_it_fits},
    {"the ends go round on one row only", the_ends_go_round_on_one_row_only},
    {"Search: the keyboard, the entry and the stacked one, the scroll", search_by_the_keyboard},
    {"the cursor by word, by the stick and by a tap", the_cursor_by_word_stick_and_tap},
    {"a card from a typed text", a_card_from_a_typed_text},
    {"Earlier lines, and a card from one", earlier_lines_and_a_card_from_one},
    {"touches", touches},
    {"no history yet, and the buttons named", no_history_yet_and_the_buttons_named},
    {"leaving the page closes the dictionaries", leaving_closes_the_dictionaries},
    {NULL, NULL},
};

int main(int argc, char **argv)
{
    const char *dir = vjo_test_tmpdir("settings-lookup");
    if (!dir || chdir(dir) < 0)
        return 2;
    return acutest_main(argc, argv);
}
