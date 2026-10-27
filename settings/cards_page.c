#include "cards_page.h"

#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vita2d.h>

#include "card_picture.h"
#include "cards.h"
#include "search.h"
#include "../core/errors.h"
#include "../core/linebreak.h"
#include "../core/stick_scroll.h"
#include "../core/utf.h"
/* As in main.c: no vita2d_fini in the system application. */
#pragma GCC poison vita2d_fini

/* ---------------- layout ---------------- */

/* The list: a bar (the two lists, the duplicates filter, where the page is), six cards a page, a
 * two-line status box above main.c's controls line (baseline 535). */
#define BAR_Y 112
#define BAR_H 36
#define ROWS_Y 156
#define ROW_H 44
#define ROW_PITCH 48
#define LIST_ROWS 6
#define LIST_FOOTER_Y 448
#define LIST_FOOTER_H 60
/* A card: its word, reading, game and date, its sentence and definition at the left, its picture at
 * the right, the actions below, the status box. */
#define VIEW_Y 112
#define VIEW_H 280
#define COL_X 42
#define COL_W 448
#define WORD_Y 148
#define META_Y 176
#define SENTENCE_Y 210
#define SENTENCE_LH 28
#define SENTENCE_ROWS 5
#define DEFINITION_Y 352
#define DEFINITION_LH 25
#define DEFINITION_ROWS 2
#define PIC_X 508
#define PIC_Y 120
#define PIC_W 416
#define PIC_H 234
#define ACTION_Y 400
#define ACTION_H 36
#define CARD_FOOTER_Y 444
#define CARD_FOOTER_H 64
#define WINDOWS 64       /* windows of VJO_CARDS_MAX cards: 128,000 cards */
#define EDIT_UNITS 2048  /* the keyboard's most (SCE_IME_DIALOG_MAX_TEXT_LENGTH) */
#define ROW_TEXT 512     /* a drawn row's bytes */

enum { VIEW_LIST, VIEW_CARD };
enum { ACT_DELETE, ACT_FIX, ACT_DROP, ACTIONS };
static const struct {
    int x, w;
    const char *label;
} actions[ACTIONS] = {{24, 160, "Delete"}, {192, 236, "Fix sentence"}, {436, 236, "Drop picture"}};
enum { JOB_NONE, JOB_LIST, JOB_LOAD, JOB_DELETE, JOB_FIX, JOB_DROP, JOB_EMPTY };
enum { PIC_NONE, PIC_SHOWN, PIC_MISSING, PIC_UNREADABLE, PIC_NO_MEMORY };

/* ---------------- state (main thread; a job's fields are the worker's while it runs) ---------------- */

static int open_, view, which, stick_move;
static VjoCardRow table[VJO_CARDS_MAX];
static VjoCardList list;
static int listed;                    /* the table is which's window `window` */
static int window;
static VjoCardRow bounds[WINDOWS];    /* window k's cards are older than bounds[k] (k >= 1) */
static int visible[VJO_CARDS_MAX], count, selected, offset, dup_only;
static int totals[2] = {-1, -1};      /* queued and exported cards, as last counted */
static int deleted_cards = -1;        /* cards in deleted/ */
static char note[300];
static int note_error;
static int armed = -1;                /* the action waiting for its second press, ACTIONS: Empty deleted */
static int action;                    /* the card view's chosen action */

/* the card shown, and the one a job loads (they swap when it ends) */
static VjoCardView views[2];
static int shown;
static vita2d_texture *picture;       /* views[shown]'s */
static int picture_state;
typedef struct {
    int start, bytes, ellipsis;
} Row;
typedef struct {
    int n;
    Row r[8];
} Rows;
static Rows sentence_rows, definition_rows;
static int laid_out;

/* the job: what the main thread asks, what the worker leaves */
static int job, job_which, job_rc, job_unsettled, job_removed, job_picture_rc, job_deleted;
static uint8_t job_id[16];
static char job_sentence[VJO_CARD_SENTENCE_MAX + 4]; /* (the keyboard's 2,048 units are at most 6,144 bytes) */
static const VjoCardRow *job_bound;
static VjoCardChange job_change;
static VjoCardsHeld job_held;         /* the recovery's held cards (#315's audit): marked in the list */
static VjoCardPixels job_pixels;
static int after_job;                 /* after_id is to be selected (and opened, in the card view) once a list is read */
static uint8_t after_id[16];
static int first_read;                /* the page's first list read: Cancel during it closes the page */
static int read_failed;               /* the last list read failed (not stopped) */
static int select_last;               /* a list read going back a window: its last card selected */

static const VjoAnkiUi *ui_now;       /* the frame's helpers, for the keyboard's frames */

/* ---------------- the worker's side ---------------- */

static void run_job(VjoArena *a, const VjoExportUi *eui)
{
    size_t len;
    const uint8_t *jpeg;
    job_unsettled = 0;
    if (job != JOB_LOAD)
        job_unsettled = vjo_cards_recover(a, &job_held);
    switch (job) {
    case JOB_LIST:
        job_rc = vjo_cards_list(a, job_which, job_bound, &list, eui);
        job_deleted = vjo_cards_deleted_files();
        for (int i = 0; !job_rc && job_which == VJO_CARDS_QUEUED && i < list.n; i++)
            if (vjo_cards_is_held(&job_held, table[i].id))
                table[i].flags |= VJO_CARD_HELD;
        break;
    case JOB_LOAD:
        job_rc = vjo_cards_load(a, job_which, job_id, &views[!shown]);
        job_picture_rc = 1;
        memset(&job_pixels, 0, sizeof(job_pixels));
        if (!job_rc && (views[!shown].flags & VJO_CARD_PICTURE)) {
            /* the pixels stay in the arena until the main thread copies them (no job runs meanwhile) */
            if (!(jpeg = vjo_cards_picture(a, job_which, job_id, &len)))
                job_picture_rc = VJO_E_LOCAL_IO;
            else
                job_picture_rc = vjo_card_picture_decode(a, jpeg, len, PIC_W, PIC_H, &job_pixels);
        }
        break;
    case JOB_DELETE:
        job_rc = vjo_cards_delete(a, job_id);
        job_deleted = vjo_cards_deleted_files();
        break;
    case JOB_FIX:
        job_rc = vjo_cards_fix_sentence(a, job_id, job_sentence, &job_change);
        job_deleted = vjo_cards_deleted_files();
        break;
    case JOB_DROP:
        job_rc = vjo_cards_drop_picture(a, job_id, &job_change);
        job_deleted = vjo_cards_deleted_files();
        break;
    case JOB_EMPTY:
        job_rc = vjo_cards_empty_deleted(a, &job_removed);
        job_deleted = vjo_cards_deleted_files();
        break;
    default:
        break;
    }
}

static int start(int kind, const char *progress)
{
    int rc;
    job = kind;
    if ((rc = vjo_anki_job_start(run_job, progress)) < 0) {
        job = JOB_NONE;
        snprintf(note, sizeof(note), "Could not start (0x%08X).", (unsigned)rc);
        note_error = 1;
        return rc;
    }
    armed = -1;
    return 0;
}

int vjo_cards_page_job_running(void) { return job != JOB_NONE; }

/* ---------------- the list ---------------- */

static void rebuild(void)
{
    count = 0;
    if (listed)
        for (int i = 0; i < list.n; i++)
            if (!dup_only || (table[i].flags & VJO_CARD_DUPLICATE))
                visible[count++] = i;
    if (selected >= count)
        selected = count ? count - 1 : 0;
    if (selected < 0)
        selected = 0;
    offset = selected / LIST_ROWS * LIST_ROWS;
}

static int row_of_id(const uint8_t id[16])
{
    for (int i = 0; i < list.n; i++)
        if (!memcmp(table[i].id, id, 16))
            return i;
    return -1;
}

/* The shown row of table row t, or -1. */
static int visible_of(int t)
{
    for (int i = 0; i < count; i++)
        if (visible[i] == t)
            return i;
    return -1;
}

static void read_list(int k)
{
    if (k < 0 || k >= WINDOWS)
        return;
    window = k;
    list.rows = table;
    list.cap = VJO_CARDS_MAX;
    job_which = which;
    job_bound = k ? &bounds[k] : NULL;
    listed = 0;
    count = 0;
    start(JOB_LIST, "Reading the cards…");
}

static void open_card(int t)
{
    if (t < 0 || t >= list.n)
        return;
    memcpy(job_id, table[t].id, 16);
    job_which = which;
    start(JOB_LOAD, "Opening the card…");
}

void vjo_cards_page_open(void)
{
    VjoExportCounts c;
    open_ = 1;
    view = VIEW_LIST;
    which = VJO_CARDS_QUEUED;
    selected = offset = dup_only = 0;
    note[0] = '\0';
    note_error = 0;
    after_job = -1;
    first_read = 1;
    select_last = 0;
    vjo_export_count(&c);
    totals[0] = c.queued;
    totals[1] = c.exported;
    read_list(0);
}

int vjo_cards_page_is_open(void) { return open_; }

void vjo_cards_page_reread(void)
{
    if (!open_ || job != JOB_NONE || !listed)
        return;
    if (count) { /* the card selected (or shown) selected again, in its place now */
        memcpy(after_id, table[visible[selected]].id, 16);
        after_job = 1;
    }
    read_list(window);
}

void vjo_cards_page_stick(int move) { stick_move = move; }

static void free_picture(void)
{
    if (picture) {
        vita2d_wait_rendering_done(); /* the frames that drew it are done */
        vita2d_free_texture(picture);
        picture = NULL;
    }
}

static void close_page(void)
{
    free_picture();
    open_ = 0;
    listed = 0;
    view = VIEW_LIST;
    armed = -1;
}

void vjo_cards_page_quit(void) { free_picture(); }

/* The decoded pixels into a texture, which the GPU reads from main memory. */
static vita2d_texture *texture_of(const VjoCardPixels *p)
{
    vita2d_texture *t = vita2d_create_empty_texture_format(p->width, p->height, SCE_GXM_TEXTURE_FORMAT_U8U8U8_BGR);
    uint8_t *dst;
    unsigned stride;
    /* (its two stale fields, libvita2d #28, are cleared by the link's wrapper: settings/vita2d_textures.c) */
    if (!t)
        return NULL;
    dst = (uint8_t *)vita2d_texture_get_datap(t);
    stride = vita2d_texture_get_stride(t);
    for (unsigned y = 0; y < p->height; y++)
        memcpy(dst + (size_t)y * stride, p->rgb + (size_t)y * p->stride, (size_t)p->width * 3u);
    return t;
}

static const char *word_of(const VjoCardRow *r) { return r->word[0] ? r->word : "(no word)"; }

/* What a change left, in the status box. */
static void say(int error, const char *fmt, const char *word)
{
    snprintf(note, sizeof(note), fmt, word);
    vjo_utf8_trim_partial(note);
    note_error = error;
}

static void unsettled_note(void)
{
    if (job_unsettled)
        snprintf(note, sizeof(note), "A change that stopped could not be finished (see log.txt): its card stays "
                 "queued, marked, and it is tried again before the next list and export.");
    note_error = note_error || job_unsettled;
}

void vjo_cards_page_job_done(void)
{
    int kind = job, t;
    job = JOB_NONE;
    if (kind == JOB_LIST) {
        if (job_rc == VJO_E_CANCELLED) {
            if (first_read) { /* back where the page was opened from */
                close_page();
                return;
            }
            free_picture();
            view = VIEW_LIST;
            after_job = -1;
            select_last = 0;
            snprintf(note, sizeof(note), "Stopped. Start reads the list again.");
            note_error = 0;
            read_failed = 0;
            return;
        }
        first_read = 0;
        listed = job_rc == 0;
        read_failed = !listed;
        totals[which] = listed ? list.total : -1;
        deleted_cards = job_deleted;
        if (!listed) {
            snprintf(note, sizeof(note), "Cannot read %s (see log.txt).",
                     which == VJO_CARDS_EXPORTED ? VJO_QUEUE_EXPORTED_DIR : VJO_QUEUE_DIR);
            note_error = 1;
        }
        unsettled_note();
        rebuild();
        if (select_last) { /* back a window: at its end, where the newer window began */
            select_last = 0;
            selected = count ? count - 1 : 0;
            offset = selected / LIST_ROWS * LIST_ROWS;
        }
        if (after_job >= 0) { /* a changed card: shown again in its new place */
            int at = row_of_id(after_id);
            after_job = -1;
            if (at >= 0 && (t = visible_of(at)) >= 0) {
                selected = t;
                offset = selected / LIST_ROWS * LIST_ROWS;
                if (view == VIEW_CARD)
                    open_card(at);
            } else {
                view = VIEW_LIST;
                free_picture();
            }
        }
        return;
    }
    if (kind == JOB_LOAD) {
        if (job_rc) {
            snprintf(note, sizeof(note), "This card cannot be read now (see log.txt): exported or deleted meanwhile? "
                     "Start reads the list again.");
            note_error = 1;
            return;
        }
        free_picture();
        shown = !shown;
        laid_out = 0;
        view = VIEW_CARD;
        action = views[shown].which == VJO_CARDS_QUEUED && !(views[shown].flags & VJO_CARD_UNREADABLE) ? ACT_FIX
                                                                                                       : ACT_DELETE;
        picture_state = PIC_NONE;
        if (views[shown].flags & VJO_CARD_PICTURE) {
            if (job_picture_rc == 0 && (picture = texture_of(&job_pixels)) != NULL)
                picture_state = PIC_SHOWN;
            else
                picture_state = job_picture_rc == VJO_E_LOCAL_IO ? PIC_MISSING
                              : job_picture_rc == VJO_E_OOM || job_picture_rc == 0 ? PIC_NO_MEMORY
                                                                                     : PIC_UNREADABLE;
        }
        return;
    }
    deleted_cards = job_deleted;
    if (kind == JOB_EMPTY) {
        if (job_rc)
            snprintf(note, sizeof(note), "Removed %d files; some could not be removed (see log.txt).", job_removed);
        else
            snprintf(note, sizeof(note), "Removed %d file%s from anki_queue/deleted/ for good.", job_removed,
                     job_removed == 1 ? "" : "s");
        note_error = job_rc != 0;
        unsettled_note();
        return;
    }
    t = row_of_id(job_id);
    if (kind == JOB_DELETE) {
        if (job_rc == VJO_CARDS_E_PENDING) {
            say(1, "%s is the fixed card of a change that has not finished (see log.txt): it is tried again first; "
                   "delete it after.", t >= 0 ? word_of(&table[t]) : "The card");
            return;
        }
        if (job_rc) {
            say(1, "%s was not deleted (see log.txt): exported or deleted meanwhile? Start reads the list again.",
                t >= 0 ? word_of(&table[t]) : "The card");
            return;
        }
        say(0, "Deleted %s. It is in anki_queue/deleted/ until Empty deleted (Triangle on the list).",
            t >= 0 ? word_of(&table[t]) : "the card");
        unsettled_note();
        if (t >= 0) {
            int was = visible_of(t);
            memmove(&table[t], &table[t + 1], sizeof(VjoCardRow) * (size_t)(list.n - t - 1));
            list.n--;
            if (totals[0] > 0)
                totals[0]--;
            list.total = list.total > 0 ? list.total - 1 : 0;
            rebuild();
            if (was >= 0)
                selected = was < count ? was : count - 1;
            if (selected < 0)
                selected = 0;
            offset = selected / LIST_ROWS * LIST_ROWS;
        }
        free_picture();
        view = VIEW_LIST;
        if (!list.n && window > 0) { /* (the window's last card: the newer window, at its end) */
            select_last = 1;
            read_list(window - 1);
        }
        return;
    }
    /* Fix sentence, Drop picture */
    if (job_rc) {
        const char *w = t >= 0 ? word_of(&table[t]) : "the card";
        if (job_rc == VJO_E_TOO_LARGE)
            say(1, "Nothing changed: %s would be over 32 KiB of text.", w);
        else if (job_change.held)
            say(1, "Nothing changed: an earlier change of %s has not finished (see log.txt); the export leaves it "
                   "queued. Delete it if its fixed card is listed too.", w);
        else if (job_change.pending)
            say(1, "Nothing changed: %s is the fixed card of a change that has not finished (see log.txt); it is "
                   "tried again first.", w);
        else if (job_change.exported)
            say(1, "Nothing changed: %s was exported already (its note may be in Anki, and a changed card would be a "
                   "second note). Delete removes this queued copy.", w);
        else if (job_rc == VJO_E_PARSE && kind == JOB_FIX && !job_sentence[0])
            say(1, "Nothing changed: a card needs a sentence (%s).", w);
        else
            say(1, "Nothing changed for %s (see log.txt). Start reads the list again.", w);
        return;
    }
    if (job_change.done == VJO_CARDS_UNCHANGED) {
        say(0, "Nothing to change: %s is as it was.", t >= 0 ? word_of(&table[t]) : "the card");
        return;
    }
    if (kind == JOB_FIX)
        say(0, job_change.picture_lost ? "Fixed. Its picture was missing or damaged, so the fixed card has none (%s)."
               : job_change.word_found ? "Fixed the sentence of %s; the old card is in anki_queue/deleted/."
                                       : "Fixed the sentence of %s. The word is not in it now, so it has no bold.",
            t >= 0 ? word_of(&table[t]) : "the card");
    else
        say(0, "Dropped the picture of %s; the old card and its picture are in anki_queue/deleted/.",
            t >= 0 ? word_of(&table[t]) : "the card");
    if (job_change.done == VJO_CARDS_EXISTED) {
        size_t n = strlen(note);
        snprintf(note + n, sizeof(note) - n, " The new card was there already.");
    }
    if (job_change.old_left) {
        size_t n = strlen(note);
        snprintf(note + n, sizeof(note) - n, " The old card could not be moved yet (see log.txt).");
        note_error = 1;
    } else if (job_change.unsynced) {
        size_t n = strlen(note);
        snprintf(note + n, sizeof(note) - n, " The memory card did not confirm the write (see log.txt).");
        note_error = 1;
    }
    unsettled_note();
    /* read the list again (the old card left it, the new one has its place): then the new one opens */
    memcpy(after_id, job_change.id, 16);
    after_job = 1;
    read_list(window);
}

/* ---------------- input ---------------- */

static void move_to(int i)
{
    if (!count)
        return;
    selected = i < 0 ? 0 : i >= count ? count - 1 : i;
    offset = selected / LIST_ROWS * LIST_ROWS;
}

/* L/R: the page before or after, as the Games page's; past the first or last page of a window of
 * VJO_CARDS_MAX cards, the newer or older window. */
static void page_jump(int direction)
{
    int last = count ? (count - 1) / LIST_ROWS * LIST_ROWS : 0, top = offset + direction * LIST_ROWS;
    if (direction > 0 && offset == last && listed && list.older > 0 && !dup_only && window + 1 < WINDOWS) {
        bounds[window + 1] = table[list.n - 1];
        selected = offset = 0;
        read_list(window + 1);
        return;
    }
    if (direction < 0 && offset == 0 && window > 0 && !dup_only) {
        selected = offset = 0;
        select_last = 1;
        read_list(window - 1);
        return;
    }
    if (!count)
        return;
    if (top > last)
        top = last;
    if (top < 0)
        top = 0;
    if (direction > 0 ? top <= offset : top >= offset) {
        selected = direction > 0 ? count - 1 : 0;
        return;
    }
    selected += top - offset;
    if (selected >= count)
        selected = count - 1;
    offset = top;
}

static int editable(void)
{
    const VjoCardView *v = &views[shown];
    return v->which == VJO_CARDS_QUEUED;
}

static int action_ok(int a)
{
    const VjoCardView *v = &views[shown];
    if (!editable())
        return 0;
    if (a == ACT_DELETE)
        return 1;
    if (v->flags & VJO_CARD_UNREADABLE)
        return 0;
    return a == ACT_FIX || (v->flags & VJO_CARD_PICTURE);
}

/* The keyboard's frames: the page behind it, which takes no input meanwhile. */
static void draw_behind_keyboard(void)
{
    vita2d_start_drawing();
    vita2d_clear_screen();
    ui_now->header();
    ui_now->text(COL_X, 148, ui_now->white, 24, "Fix the sentence");
    ui_now->wrapped(COL_X, 184, 870, ui_now->dim, 22, 26, 2,
                    "The word is found again in the new text; if it is not in it, the card has no bold. The card "
                    "gets a new ID: its old record goes to anki_queue/deleted/.");
    vita2d_end_drawing();
    vita2d_common_dialog_update();
    vita2d_swap_buffers();
    sceDisplayWaitVblankStart();
}

/* UTF-16 units of s. */
static unsigned units_of(const char *s)
{
    size_t n = strlen(s), at = 0;
    unsigned u = 0;
    while (at < n)
        u += (unsigned)vjo_utf16_units(vjo_utf8_next(s, n, &at));
    return u;
}

/* Fix sentence: the keyboard with the sentence, then the change on the worker. 1: the keyboard ran. */
static int fix_sentence(void)
{
    static uint16_t initial[EDIT_UNITS + 1], input[EDIT_UNITS + 1];
    static const uint16_t title[] = {'F', 'i', 'x', ' ', 't', 'h', 'e', ' ', 's', 'e', 'n', 't', 'e', 'n', 'c', 'e', 0};
    const VjoCardView *v = &views[shown];
    int rc;
    if (v->sentence_cut || units_of(v->sentence) > EDIT_UNITS) {
        snprintf(note, sizeof(note), "This sentence is too long for the keyboard: 2,048 UTF-16 units at most (a "
                                     "character each, two for one beyond U+FFFF such as an emoji).");
        note_error = 1;
        return 0;
    }
    snprintf(job_sentence, sizeof(job_sentence), "%s", v->sentence);
    rc = vjo_text_dialog(title, v->sentence, job_sentence, sizeof(job_sentence), EDIT_UNITS, 1, initial, input,
                         draw_behind_keyboard);
    if (rc < 0) {
        snprintf(note, sizeof(note), "Could not open the keyboard (0x%08X). Nothing changed.", (unsigned)rc);
        note_error = 1;
        vjo_log("cards: keyboard failed 0x%08X", (unsigned)rc);
        return 1;
    }
    if (rc == 0) {
        snprintf(note, sizeof(note), "Cancelled: nothing changed.");
        note_error = 0;
        return 1;
    }
    if (!strcmp(job_sentence, v->sentence)) {
        snprintf(note, sizeof(note), "Nothing to change: the sentence is as it was.");
        note_error = 0;
        return 1;
    }
    memcpy(job_id, v->id, 16);
    start(JOB_FIX, "Saving the fixed card…");
    return 1;
}

static const char *confirm_text(int a)
{
    static char text[200];
    if (a == ACT_DELETE)
        return "Press again to delete this card: it goes to anki_queue/deleted/ (Empty deleted removes it for good).";
    if (a == ACT_DROP)
        return "Press again to drop the picture: the card keeps its text, the old one goes to anki_queue/deleted/.";
    snprintf(text, sizeof(text), "Press Triangle again to remove the %d deleted card%s for good.", deleted_cards,
             deleted_cards == 1 ? "" : "s");
    return text;
}

/* An action on the card shown; Delete and Drop picture ask for a second press. 1: the keyboard ran. */
static int act(int a)
{
    if (!action_ok(a))
        return 0;
    if (a == ACT_FIX)
        return fix_sentence();
    if (armed != a) {
        armed = a;
        snprintf(note, sizeof(note), "%s", confirm_text(a));
        note_error = 0;
        return 0;
    }
    armed = -1;
    memcpy(job_id, views[shown].id, 16);
    start(a == ACT_DELETE ? JOB_DELETE : JOB_DROP, a == ACT_DELETE ? "Deleting the card…" : "Dropping the picture…");
    return 0;
}

static void switch_list(int to)
{
    if (to == which)
        return;
    which = to;
    selected = offset = 0;
    armed = -1;
    note[0] = '\0';
    note_error = 0;
    read_list(0);
}

/* The card before or after the one shown, in the list's order. */
static void step_card(int direction)
{
    int t = row_of_id(views[shown].id), at = t >= 0 ? visible_of(t) : -1;
    if (at < 0 || at + direction < 0 || at + direction >= count)
        return;
    move_to(at + direction);
    open_card(visible[selected]);
}

static int list_input(uint32_t pressed, int stick, int touch_x, int touch_y, const VjoButtons *b)
{
    if (pressed & b->cancel) {
        close_page();
        return 0;
    }
    if (pressed & SCE_CTRL_START) {
        note[0] = '\0';
        if (listed)
            vjo_cards_page_reread();
        else
            read_list(window);
        return 0;
    }
    if (pressed & SCE_CTRL_LEFT)
        switch_list(VJO_CARDS_QUEUED);
    else if (pressed & SCE_CTRL_RIGHT)
        switch_list(VJO_CARDS_EXPORTED);
    if (job != JOB_NONE)
        return 0;
    if (pressed & SCE_CTRL_UP)
        move_to(selected - 1);
    else if (pressed & SCE_CTRL_DOWN)
        move_to(selected + 1);
    else if (pressed & SCE_CTRL_LTRIGGER)
        page_jump(-1);
    else if (pressed & SCE_CTRL_RTRIGGER)
        page_jump(1);
    else if (stick && count) {
        selected = vjo_stick_step(selected, count, stick);
        offset = selected / LIST_ROWS * LIST_ROWS;
    }
    if (job != JOB_NONE)
        return 0;
    if (pressed & SCE_CTRL_SQUARE) {
        dup_only = !dup_only;
        selected = offset = 0;
        rebuild();
    }
    if (pressed & SCE_CTRL_TRIANGLE) {
        if (deleted_cards <= 0) {
            snprintf(note, sizeof(note), "anki_queue/deleted/ is empty.");
            note_error = 0;
        } else if (armed != ACTIONS) {
            armed = ACTIONS;
            snprintf(note, sizeof(note), "%s", confirm_text(ACTIONS));
            note_error = 0;
        } else {
            start(JOB_EMPTY, "Emptying anki_queue/deleted/…");
        }
    } else if (pressed && !(pressed & SCE_CTRL_TRIANGLE) && armed == ACTIONS) {
        armed = -1;
        note[0] = '\0';
    }
    if ((pressed & b->enter) && count)
        open_card(visible[selected]);
    if (job != JOB_NONE)
        return 0;
    /* a touch: the list buttons in the bar, or a card (chosen and opened, as Enter) */
    if (touch_y >= BAR_Y && touch_y < BAR_Y + BAR_H) {
        if (touch_x >= 24 && touch_x < 224)
            switch_list(VJO_CARDS_QUEUED);
        else if (touch_x >= 232 && touch_x < 452)
            switch_list(VJO_CARDS_EXPORTED);
        else if (touch_x >= 460 && touch_x < 660) {
            dup_only = !dup_only;
            selected = offset = 0;
            rebuild();
        }
    } else if (touch_y >= ROWS_Y && touch_x >= 24 && touch_x < 936 && (touch_y - ROWS_Y) % ROW_PITCH < ROW_H) {
        int r = (touch_y - ROWS_Y) / ROW_PITCH;
        if (r < LIST_ROWS && offset + r < count) {
            selected = offset + r;
            open_card(visible[selected]);
        }
    }
    return 0;
}

static int card_input(uint32_t pressed, int touch_x, int touch_y, const VjoButtons *b)
{
    int ran = 0;
    if (pressed & b->cancel) {
        free_picture();
        view = VIEW_LIST;
        armed = -1;
        note[0] = '\0';
        return 0;
    }
    if (pressed & SCE_CTRL_LTRIGGER)
        step_card(-1);
    else if (pressed & SCE_CTRL_RTRIGGER)
        step_card(1);
    if (job != JOB_NONE)
        return 0;
    if (pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) {
        int d = pressed & SCE_CTRL_LEFT ? -1 : 1;
        for (int k = 1; k <= ACTIONS; k++) {
            int a = ((action + d * k) % ACTIONS + ACTIONS) % ACTIONS;
            if (action_ok(a)) {
                action = a;
                break;
            }
        }
    }
    if (armed >= 0 && armed < ACTIONS && (pressed & ~b->enter) && !(pressed & b->enter)) {
        armed = -1; /* any other button: no second press */
        note[0] = '\0';
    }
    if ((pressed & b->enter) && action_ok(action))
        ran = act(action);
    if (ran || job != JOB_NONE)
        return ran;
    if (touch_y >= ACTION_Y && touch_y < ACTION_Y + ACTION_H)
        for (int a = 0; a < ACTIONS; a++)
            if (action_ok(a) && touch_x >= actions[a].x && touch_x < actions[a].x + actions[a].w) {
                if (armed >= 0 && armed != a)
                    armed = -1;
                action = a;
                ran |= act(a);
            }
    return ran;
}

/* ---------------- drawing ---------------- */

/* s in at most `lines` rows of `width` pixels: a row ends at a line break, else at the last place
 * that fits where a line may break (core/linebreak.h), else at its last character that fits; the last
 * row ends in "..." when text is left. Laid out once per card. */
static void lay_out(const VjoAnkiUi *ui, const char *s, int width, float px, int lines, Rows *out)
{
    char buf[ROW_TEXT + 4];
    size_t len = strlen(s), pos = 0;
    out->n = 0;
    while (out->n < lines && pos < len) {
        size_t end = pos, i = pos, b;
        int last = out->n == lines - 1, room = width;
        Row *r = &out->r[out->n++];
        for (int pass = 0; pass < 2; pass++) {
            end = i = pos;
            while (i < len) {
                size_t at = i;
                uint32_t cp = vjo_utf8_next(s, len, &i);
                if (cp == '\n' || i - pos > ROW_TEXT)
                    break;
                memcpy(buf, s + pos, i - pos);
                buf[i - pos] = '\0';
                if (ui->text_width(px, buf) > room)
                    break;
                end = i;
                (void)at;
            }
            if (end < len && s[end] != '\n' && end > pos) { /* the text goes on: at a place a line may break */
                b = vjo_break_last8(s, len, pos, end);
                if (b > pos)
                    end = b;
            }
            r->ellipsis = 0;
            if (!last || end >= len || s[end] == '\n' || pass)
                break;
            /* the last row with text after it: again, with room for "..." */
            {
                size_t rest = end;
                while (rest < len && (s[rest] == ' ' || s[rest] == '\n' || s[rest] == '\r'))
                    rest++;
                if (rest >= len)
                    break;
            }
            room = width - ui->text_width(px, "...");
            r->ellipsis = 1;
        }
        if (last && end < len) {
            size_t rest = end;
            while (rest < len && (s[rest] == ' ' || s[rest] == '\n' || s[rest] == '\r'))
                rest++;
            r->ellipsis = rest < len;
        }
        if (end == pos && s[pos] != '\n') { /* not even one character fits: no row */
            out->n--;
            break;
        }
        r->start = (int)pos;
        r->bytes = (int)(end - pos);
        pos = end;
        while (pos < len && s[pos] == '\r')
            pos++;
        if (pos < len && s[pos] == '\n')
            pos++;
        else
            while (pos < len && s[pos] == ' ')
                pos++;
    }
}

/* Row r of s at x, y: the bytes in [hl_start, hl_end) in the accent colour, the rest in color. */
static void draw_row(const VjoAnkiUi *ui, const char *s, const Row *r, int x, int y, float px, unsigned color,
                     int hl_start, int hl_end)
{
    char buf[ROW_TEXT + 4];
    /* the row before the word, the word's part in it, the rest (no word: the whole row is the first) */
    int cut[4] = {r->start, r->start + r->bytes, r->start + r->bytes, r->start + r->bytes};
    if (hl_start >= 0 && hl_end > hl_start) {
        cut[1] = hl_start < r->start ? r->start : hl_start > cut[3] ? cut[3] : hl_start;
        cut[2] = hl_end < cut[1] ? cut[1] : hl_end > cut[3] ? cut[3] : hl_end;
    }
    for (int k = 0; k < 3; k++) {
        int n = cut[k + 1] - cut[k];
        if (n <= 0)
            continue;
        memcpy(buf, s + cut[k], (size_t)n);
        buf[n] = '\0';
        ui->text(x, y, k == 1 ? ui->accent : color, px, buf);
        x += ui->text_width(px, buf);
    }
    if (r->ellipsis)
        ui->text(x, y, color, px, "...");
}

/* A small camera: the card has a picture. */
static void camera(const VjoAnkiUi *ui, int x, int y, unsigned color)
{
    vita2d_draw_rectangle(x + 6, y, 8, 4, color);
    vita2d_draw_rectangle(x, y + 4, 24, 14, color);
    vita2d_draw_rectangle(x + 9, y + 8, 6, 6, ui->track);
}

static void draw_bar(const VjoAnkiUi *ui)
{
    char label[48], where[48];
    for (int k = 0; k < 2; k++) {
        if (totals[k] >= 0)
            snprintf(label, sizeof(label), "%s (%d)", k ? "Exported" : "Queued", totals[k]);
        else
            snprintf(label, sizeof(label), "%s", k ? "Exported" : "Queued");
        ui->badge(k ? 232 : 24, BAR_Y, k ? 220 : 200, BAR_H, label, which == k);
    }
    ui->badge(460, BAR_Y, 200, BAR_H, "Duplicates", dup_only);
    if (listed && count) {
        int base = dup_only ? 0 : list.newer, last = offset + LIST_ROWS < count ? offset + LIST_ROWS : count;
        snprintf(where, sizeof(where), "%d-%d of %d", base + offset + 1, base + last,
                 dup_only ? count : list.newer + list.n + list.older);
        ui->text(676, BAR_Y + 25, ui->dim, 22, where);
    }
}

static void draw_list(const VjoAnkiUi *ui, const char *progress)
{
    char controls[160], hint[300];
    draw_bar(ui);
    if (job == JOB_LIST || !listed) {
        ui->wrapped(COL_X, ROWS_Y + 30, 870, job == JOB_LIST || !read_failed ? ui->white : ui->off, 24, 28, 2,
                    job == JOB_LIST ? progress
                    : read_failed   ? "The cards could not be read. Start tries again."
                                    : "Stopped. Start reads the list again.");
    } else if (!count) {
        ui->wrapped(COL_X, ROWS_Y + 30, 870, ui->white, 24, 28, 2,
                    dup_only ? "No card here is of a word mined before. Square shows them all."
                    : window > 0 ? "No cards in this part of the list now: L shows the newer ones."
                    : which == VJO_CARDS_EXPORTED ? "No exported cards yet."
                                                  : "No cards queued. Save words in the overlay with Card mining ON.");
    }
    for (int r = 0; listed && job != JOB_LIST && r < LIST_ROWS && offset + r < count; r++) {
        const VjoCardRow *row = &table[visible[offset + r]];
        int y = ROWS_Y + r * ROW_PITCH, x;
        ui->card(y, ROW_H, offset + r == selected);
        vita2d_enable_clipping();
        vita2d_set_clip_rectangle(COL_X, y + 2, 620, y + ROW_H - 2);
        ui->text(COL_X, y + 21, row->flags & VJO_CARD_UNREADABLE ? ui->off : ui->white, 22,
                 row->word[0] ? row->word : "(cannot be read)");
        x = COL_X + ui->text_width(22, row->word[0] ? row->word : "(cannot be read)") + 12;
        if (row->reading[0] && strcmp(row->reading, row->word))
            ui->text(x, y + 21, ui->dim, 18, row->reading);
        ui->text(COL_X, y + 38, ui->dim, 16,
                 row->game < list.n_games ? list.games[row->game] : "");
        vita2d_disable_clipping();
        ui->text(640, y + 21, ui->dim, 18, row->date);
        if (row->flags & VJO_CARD_HELD)
            ui->text(640, y + 38, ui->off, 16, "A change of it did not finish");
        else if (row->flags & VJO_CARD_UNREADABLE)
            ui->text(640, y + 38, ui->off, 16, "Not exportable");
        else if (row->flags & VJO_CARD_DUPLICATE)
            ui->text(640, y + 38, ui->accent, 16, "Mined before: tagged duplicate");
        if (row->flags & VJO_CARD_PICTURE)
            camera(ui, 892, y + 12, ui->accent);
    }
    if (job != JOB_NONE && job != JOB_LIST)
        snprintf(hint, sizeof(hint), "%s", progress); /* opening a card, emptying deleted/ */
    else if (note[0])
        snprintf(hint, sizeof(hint), "%s", note);
    else if (job == JOB_LIST)
        snprintf(hint, sizeof(hint), "%s", which == VJO_CARDS_EXPORTED ? "The exported cards, read only."
                                                                       : "The queued cards, newest first.");
    else if (which == VJO_CARDS_EXPORTED)
        snprintf(hint, sizeof(hint), "Exported cards are read only: their notes may be in Anki already, and an "
                 "edit would make a new note. Square: duplicates only.");
    else
        snprintf(hint, sizeof(hint), "Square: duplicates only   Triangle: empty deleted (%d card%s)   Start: read "
                 "again", deleted_cards > 0 ? deleted_cards : 0, deleted_cards == 1 ? "" : "s");
    if (job == JOB_LIST)
        snprintf(controls, sizeof(controls), "%s: stop   Select: page", ui->buttons->cancel_glyph);
    else
        snprintf(controls, sizeof(controls), "Up/Down: card   L/R: page   Left/Right: list   %s: open   %s: back",
                 ui->buttons->enter_glyph, ui->buttons->cancel_glyph);
    ui->footer(LIST_FOOTER_Y, LIST_FOOTER_H, LIST_FOOTER_Y + 24, 2, hint, job == JOB_NONE && note_error, controls);
}

static void draw_picture(const VjoAnkiUi *ui, const VjoCardView *v)
{
    static const char *const why[] = {"No picture", "", "The picture is missing or damaged",
                                      "The picture cannot be shown (log.txt)", "Not enough memory to show it"};
    if (picture_state == PIC_SHOWN && picture) {
        float w = (float)vita2d_texture_get_width(picture), h = (float)vita2d_texture_get_height(picture);
        float s = PIC_W / w < PIC_H / h ? PIC_W / w : PIC_H / h;
        vita2d_draw_texture_scale(picture, PIC_X + (PIC_W - w * s) / 2, PIC_Y + (PIC_H - h * s) / 2, s, s);
        return;
    }
    vita2d_draw_rectangle(PIC_X, PIC_Y, PIC_W, 2, ui->dim);
    vita2d_draw_rectangle(PIC_X, PIC_Y + PIC_H - 2, PIC_W, 2, ui->dim);
    vita2d_draw_rectangle(PIC_X, PIC_Y, 2, PIC_H, ui->dim);
    vita2d_draw_rectangle(PIC_X + PIC_W - 2, PIC_Y, 2, PIC_H, ui->dim);
    {
        const char *s = why[(v->flags & VJO_CARD_PICTURE) ? picture_state : PIC_NONE];
        int w = ui->text_width(22, s);
        ui->text(PIC_X + (PIC_W - w) / 2, PIC_Y + PIC_H / 2 + 8, picture_state == PIC_NONE ? ui->dim : ui->off, 22, s);
    }
}

static void draw_card_view(const VjoAnkiUi *ui, const char *progress)
{
    const VjoCardView *v = &views[shown];
    char line[400], controls[160];
    int x;
    if (!laid_out) {
        lay_out(ui, v->sentence, COL_W, 24, SENTENCE_ROWS, &sentence_rows);
        lay_out(ui, v->definition, COL_W, 22, DEFINITION_ROWS, &definition_rows);
        laid_out = 1;
    }
    ui->card(VIEW_Y, VIEW_H, 0);
    vita2d_enable_clipping();
    vita2d_set_clip_rectangle(COL_X, VIEW_Y + 2, COL_X + COL_W, VIEW_Y + VIEW_H - 2);
    ui->text(COL_X, WORD_Y, v->flags & VJO_CARD_UNREADABLE ? ui->off : ui->white, 32, v->word[0] ? v->word : "?");
    x = COL_X + ui->text_width(32, v->word[0] ? v->word : "?") + 16;
    if (v->reading[0] && strcmp(v->reading, v->word))
        ui->text(x, WORD_Y, ui->dim, 24, v->reading);
    if (v->flags & VJO_CARD_UNREADABLE)
        snprintf(line, sizeof(line), "%s", v->why == VJO_CARD_WHY_CHECKSUM
                                               ? "Not exportable: its file was edited or damaged."
                                               : "Not exportable: a damaged record, or a newer build's.");
    else
        snprintf(line, sizeof(line), "%s%s%s", v->game, v->game[0] && v->date[0] ? " · " : "", v->date);
    ui->text(COL_X, META_Y, v->flags & VJO_CARD_UNREADABLE ? ui->off : ui->dim, 20, line);
    for (int r = 0; r < sentence_rows.n; r++)
        draw_row(ui, v->sentence, &sentence_rows.r[r], COL_X, SENTENCE_Y + r * SENTENCE_LH, 24, ui->white,
                 v->hl_start, v->hl_end);
    for (int r = 0; r < definition_rows.n; r++)
        draw_row(ui, v->definition, &definition_rows.r[r], COL_X, DEFINITION_Y + r * DEFINITION_LH, 22, ui->dim, -1,
                 -1);
    vita2d_disable_clipping();
    draw_picture(ui, v);
    if (editable())
        for (int a = 0; a < ACTIONS; a++)
            if (action_ok(a))
                ui->badge(actions[a].x, ACTION_Y, actions[a].w, ACTION_H, actions[a].label,
                          a == action && job == JOB_NONE);
    if (!editable())
        ui->text(COL_X, ACTION_Y + 26, ui->dim, 20, "Exported: read only. Its note may be in Anki already.");
    snprintf(controls, sizeof(controls), "L/R: card   Left/Right: action   %s: do it   %s: back",
             ui->buttons->enter_glyph, ui->buttons->cancel_glyph);
    if (!editable())
        snprintf(controls, sizeof(controls), "L/R: card   %s: back", ui->buttons->cancel_glyph);
    ui->footer(CARD_FOOTER_Y, CARD_FOOTER_H, CARD_FOOTER_Y + 25, 2,
               job != JOB_NONE ? progress
               : note[0]        ? note
               : v->which == VJO_CARDS_QUEUED && !(v->flags & VJO_CARD_UNREADABLE)
                   ? "Fix sentence opens the keyboard. A changed card is a new card: the export makes it a new note."
                   : v->which == VJO_CARDS_QUEUED ? "This card is kept, not exported, until it is deleted."
                                                  : "The export took this card; Export all cards takes it again.",
               job == JOB_NONE && note_error, controls);
}

int vjo_cards_page_frame(const VjoAnkiUi *ui, uint32_t pressed, int touch_x, int touch_y)
{
    char progress[160];
    int stick = stick_move, ran = 0;
    stick_move = 0;
    ui_now = ui;
    if (job != JOB_NONE) {
        /* while it runs: cancel stops a list's reading; a card's opening and a change finish (they are short) */
        if ((pressed & ui->buttons->cancel) && job == JOB_LIST)
            vjo_anki_job_cancel();
    } else if (view == VIEW_LIST) {
        ran = list_input(pressed, stick, touch_x, touch_y, ui->buttons);
    } else {
        ran = card_input(pressed, touch_x, touch_y, ui->buttons);
    }
    vjo_anki_job_progress(progress, sizeof(progress));
    if (!open_)
        return ran; /* closed: the Anki page draws this frame */
    vita2d_start_drawing();
    vita2d_clear_screen();
    ui->header();
    if (view == VIEW_CARD)
        draw_card_view(ui, progress);
    else
        draw_list(ui, progress);
    vita2d_end_drawing();
    vita2d_swap_buffers();
    return ran;
}
