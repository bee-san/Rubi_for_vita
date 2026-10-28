#include "lookup_page.h"

#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/rtc.h>
#include <stdio.h>
#include <string.h>
#include <vita2d.h>

#include "app_io.h" /* VJO_APP_RTC_UNIX_EPOCH */
#include "app_queue.h"
#include "applog.h"
#include "lookup.h"
#include "lookup_history.h"
#include "preferences.h"
#include "search.h"
#include "../core/entries.h"
#include "../core/linebreak.h"
#include "../core/local_dict.h"
#include "../core/port.h"
#include "../core/stick_cursor.h"
#include "../core/stick_scroll.h"
#include "../core/utf.h"
/* As in main.c: no vita2d_fini in the system application. */
#pragma GCC poison vita2d_fini

/* The layout, below the tabs (main.c: y 52 to 100) and above the controls line main.c draws at y 535. */
#define MODE_Y 112        /* the row of the two modes, and what the page shows on its right */
#define MODE_H 40
#define SEARCH_X 24
#define SEARCH_W 196
#define LINES_X 228
#define LINES_W 236
#define INFO_X 480
#define INFO_W 456
#define TEXT_Y 160        /* the text: a card of two rows */
#define TEXT_H 74
#define TEXT_ROWS 2
#define TEXT_PX 26
#define TEXT_LINE 32
#define TEXT_BASE 30      /* the first row's baseline in the card */
#define ENTRY_Y 240       /* the entry: a card to the status line */
#define ENTRY_END 464
#define STATUS_Y 468
#define STATUS_H 40
#define LIST_Y 160        /* Earlier lines: a column of lines */
#define LIST_ROW 43
#define LIST_H 40
#define LIST_ROWS 7
#define BODY_X 42         /* the entry's text, from the card's left padding to the scroll bar */
#define BODY_W 872
#define SCROLL_X 922
#define SCROLL_W 6
#define JA_PX 26          /* headwords, readings, kanji: the overlay's font_size_ja 18, at the app's scale */
#define EN_PX 22          /* definitions */
#define ROW_BYTES 508     /* a drawn row's bytes at most (settings/text_layout.h VJO_TEXT_ROW_BYTES) */

/* All text, including styled dictionary rows, follows the current app palette. */
#define HIGHLIGHT (s_ui->highlight)
#define READING (s_ui->reading)
#define RANK (s_ui->rank)
#define DEFINITION (s_ui->definition)

enum { MODE_SEARCH, MODE_LINES };

static const VjoPageUi *s_ui;
static VjoConfig cfg;
static int mode = MODE_SEARCH, line_open, configured;
static char status[200];
static int status_error;

/* ---------------- widths: each character's, measured once (main.c's font) ----------------
 * libvita2d's width of a text is the sum of its characters' advances (each truncated to whole pixels) and the
 * kerning between them (vita2d_pvf.c a8f15ab generic_pvf_draw_text): the sum of the characters' own widths, less
 * kerning. A row is cut by that sum, then checked once against the whole row's width, so a measured row never runs
 * past its room; the rows of a long entry cost a measure per new character instead of one per prefix. */
#define WIDTH_SLOTS 2048
static struct { uint32_t key; int16_t width; } widths[WIDTH_SLOTS];
static int width_entries;

static int char_width(uint32_t cp, int px)
{
    uint32_t key = ((cp << 6) | (uint32_t)(px & 63)) + 1, slot = (key * 2654435761u) >> 21;
    char buf[8];
    int n, w;
    if (cp == '\n' || cp == '\r')
        return 0;
    for (int probe = 0; probe < 16; probe++, slot = (slot + 1) % WIDTH_SLOTS) {
        if (widths[slot].key == key)
            return widths[slot].width;
        if (!widths[slot].key)
            break;
    }
    n = vjo_utf8_encode(cp, buf);
    buf[n] = '\0';
    w = s_ui->text_width((float)px, buf);
    if (w <= 0 && cp > ' ')
        return w; /* (a glyph its font could not hold: measured again next time, not kept) */
    if (width_entries >= WIDTH_SLOTS * 3 / 4) { /* full: forgotten, measured again as they come */
        memset(widths, 0, sizeof(widths));
        width_entries = 0;
        slot = (key * 2654435761u) >> 21;
    }
    while (widths[slot].key)
        slot = (slot + 1) % WIDTH_SLOTS;
    widths[slot].key = key;
    widths[slot].width = (int16_t)w;
    width_entries++;
    return w;
}

static int measure(const char *s, size_t n, int px)
{
    size_t i = 0;
    int w = 0;
    while (i < n)
        w += char_width(vjo_utf8_next(s, n, &i), px);
    return w;
}

/* The exact width of s[0, n) (one measure of the whole). */
static int exact(const char *s, size_t n, int px)
{
    char buf[ROW_BYTES + 4];
    if (n > ROW_BYTES)
        n = ROW_BYTES;
    memcpy(buf, s, n);
    buf[n] = '\0';
    return s_ui->text_width((float)px, buf);
}

/* The bytes of s[0, len) that the next row of `width` pixels takes (settings/text_layout.c fit_line's rule: the
 * widest part that fits, back to its last space or the last place a line may break when the text goes on:
 * core/linebreak.h), never inside a character; at least one character, so a row always advances. */
static size_t fit(const char *s, size_t len, int width, int px)
{
    size_t end = 0, space = 0, i = 0, b;
    int w = 0;
    while (i < len) {
        size_t begin = i;
        uint32_t cp = vjo_utf8_next(s, len, &i);
        if (cp == '\n' || i > ROW_BYTES)
            break;
        w += char_width(cp, px);
        if (w > width)
            break;
        end = i;
        if (cp == ' ')
            space = begin;
    }
    if (end < len && s[end] != '\n') {
        b = vjo_break_last8(s, len, 0, end);
        b -= b && s[b - 1] == ' ';
        if (b > space)
            space = b;
        if (space)
            end = space;
    }
    /* kerning made it wider than the sum: a character less (rare) */
    while (end && exact(s, end, px) > width) {
        size_t k = end;
        while (k && ((unsigned char)s[k - 1] & 0xC0) == 0x80)
            k--;
        end = k ? k - 1 : 0;
    }
    if (!end && len && s[0] != '\n') /* one character wider than the room: it takes the row alone */
        vjo_utf8_next(s, len, &end);
    return end;
}

/* ---------------- the entry's rows ---------------- */

typedef struct {
    const char *s;
    uint16_t bytes, x;  /* its text, and its place in the row */
    uint8_t px;
    unsigned color;
} Piece;
#define PIECES 5
typedef struct {
    Piece piece[PIECES];
    uint8_t pieces, height;
} Row;
#define BODY_ROWS 400
static Row body[BODY_ROWS];
static int body_rows, body_top, body_cut; /* body_cut: the entry had more rows than BODY_ROWS */
static char pool[4096];                   /* the strings the rows are made of besides the entry's own */
static size_t pool_used;

static const char *pool_put(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p;
    if (pool_used + n > sizeof(pool))
        return "";
    p = pool + pool_used;
    memcpy(p, s, n);
    pool_used += n;
    return p;
}

static int line_height(int px) { return px >= JA_PX ? 32 : 26; }

static Row *new_row(int height)
{
    Row *r;
    if (body_rows >= BODY_ROWS) {
        body_cut = 1;
        return NULL;
    }
    r = &body[body_rows++];
    memset(r, 0, sizeof(*r));
    r->height = (uint8_t)height;
    return r;
}

static void add_piece(Row *r, const char *s, size_t n, int px, unsigned color, int x)
{
    if (!r || r->pieces >= PIECES || !n)
        return;
    r->piece[r->pieces++] = (Piece){s, (uint16_t)n, (uint16_t)x, (uint8_t)px, color};
    if (line_height(px) > r->height)
        r->height = (uint8_t)line_height(px);
}

static void blank(void) { new_row(12); }

/* s[0, n) in rows of the body's width, its line breaks kept; the first row after `indent` pixels (a piece before
 * it, already in `first`). */
static void wrap(const char *s, size_t n, int px, unsigned color, Row *first, int indent)
{
    size_t at = 0;
    for (int row = 0; at < n; row++) {
        Row *r;
        int x = row == 0 && first ? indent : 0;
        size_t take;
        while (at < n && s[at] == ' ')
            at++; /* (a row starts after the spaces it was cut at, as settings/text_layout.c's) */
        if (at >= n || !(r = row == 0 && first ? first : new_row(line_height(px))))
            return;
        take = fit(s + at, n - at, BODY_W - x, px);
        add_piece(r, s + at, take, px, color, x); /* (none for an empty line: its row stays empty) */
        at += take;
        if (at < n && s[at] == '\n')
            at++;
        else if (!take)
            return;
    }
}

#include "../core/conjugation.inc"

/* A vocabulary's rows, as the overlay's body has them (core/styled.c vjo_styled_entry): its headword and reading
 * and, for the shown entry (e), the frequency badge and the conjugation line; then its meanings, the last one's
 * kanji lines ("漢=meaning", each kanji at the headword's size) after a blank row. */
static void vocab_rows(const VjoVocab *v, const VjoEntry *e, const char *source)
{
    Row *r = new_row(line_height(JA_PX));
    int x;
    if (!r)
        return;
    add_piece(r, v->spelling, strlen(v->spelling), JA_PX, HIGHLIGHT, 0);
    x = measure(v->spelling, strlen(v->spelling), JA_PX);
    if (strcmp(v->spelling, v->reading)) {
        char reading[160];
        snprintf(reading, sizeof(reading), " (%s)", v->reading);
        vjo_utf8_trim_partial(reading); /* (a reading too long for it: cut on a character) */
        const char *p = pool_put(reading);
        add_piece(r, p, strlen(p), JA_PX, READING, x);
        x += measure(p, strlen(p), JA_PX);
    }
    if (e && cfg.frequency_badge) {
        char badge[VJO_RANK_BADGE_MAX];
        const char *p = pool_put(vjo_rank_badge(badge, v->rank, source));
        add_piece(r, p, strlen(p), EN_PX, RANK, x);
    }
    if (e && e->conjugation) {
        char line[256];
        size_t used = (size_t)snprintf(line, sizeof(line), "Conjugation: ");
        for (uint32_t reasons = e->conjugation; reasons && used < sizeof(line); reasons >>= 6) {
            unsigned reason = reasons & 63u;
            used += (size_t)snprintf(line + used, sizeof(line) - used, "%s%s",
                reason && reason < sizeof(conjugation_offsets) / sizeof(*conjugation_offsets)
                    ? conjugation_labels + conjugation_offsets[reason] : "…", reasons >> 6 ? " → " : "");
        }
        vjo_utf8_trim_partial(line);
        const char *p = pool_put(line);
        wrap(p, strlen(p), EN_PX, READING, NULL, 0);
    }
    for (int i = 0; i < v->n_meanings; i++) {
        const char *t = v->meanings[i];
        size_t len = strlen(t), k = i == v->n_meanings - 1 ? vjo_kanji_block(t) : len, d = k;
        while (k < len && d && t[d - 1] == '\n')
            d--; /* the definition ends before the newline(s) its kanji lines follow */
        if (d)
            wrap(t, d, EN_PX, DEFINITION, NULL, 0);
        if (k < len && d)
            blank();
        while (k < len) { /* "漢=meaning, meaning": the kanji at the headword's size, its meanings after it */
            const char *eq = strchr(t + k, '='), *nl = strchr(eq, '\n');
            size_t end = nl ? (size_t)(nl - t) : len;
            Row *kr = new_row(line_height(JA_PX));
            int kw = measure(t + k, (size_t)(eq - (t + k)), JA_PX);
            add_piece(kr, t + k, (size_t)(eq - (t + k)), JA_PX, s_ui->white, 0);
            wrap(eq, (size_t)(t + end - eq), EN_PX, DEFINITION, kr, kw);
            k = nl ? end + 1 : len;
        }
    }
}

/* The body: the entry and, below it, the other dictionaries' entries for the word; or why there is none. */
static void build_body(void)
{
    const VjoOverlayData *d = vjo_lookup_result();
    const VjoEntryList *l = &d->list;
    const char *why = NULL;
    unsigned why_color = s_ui->dim;
    body_rows = body_top = body_cut = 0;
    pool_used = 0;
    if (vjo_lookup_error()[0]) {
        why = vjo_lookup_error();
        why_color = s_ui->off;
    } else if (!vjo_lookup_text_len()) {
        why = mode == MODE_SEARCH
            ? "Type a word or a sentence (Triangle) to look it up with the overlay's dictionaries. Enter saves "
              "its entry as an Anki card, without a picture."
            : "";
    } else if (!vjo_local_names_count(cfg.local_dictionaries)) {
        why = "No dictionary: set local_dictionaries in config.ini first.";
        why_color = s_ui->off;
    } else if (vjo_lookup_cursor() >= vjo_lookup_text_len()) {
        why = "No word here.";
    } else if (!l->n_entries) {
        why = "No entry for this word.";
    }
    if (why || !l->n_entries) {
        if (why && *why)
            wrap(why, strlen(why), EN_PX, why_color, NULL, 0);
        return;
    }
    vocab_rows(l->entries[0].vocab, &l->entries[0], l->source);
    for (int i = 0; i < l->n_more; i++) {
        blank();
        vocab_rows(l->more[i], NULL, NULL); /* its definition names its dictionary ("[title]") */
    }
    if (body_cut && body_rows) { /* more rows than BODY_ROWS: the last one says so */
        Row *r = &body[body_rows - 1];
        memset(r, 0, sizeof(*r));
        r->height = (uint8_t)line_height(EN_PX);
        add_piece(r, "…", strlen("…"), EN_PX, s_ui->dim, 0);
    }
}

/* The rows' room in the card, and the last top that still fills it. */
#define BODY_TOP_Y (ENTRY_Y + 6)
#define BODY_END (ENTRY_END - 6)
static int body_last_top(void)
{
    int h = 0, top = body_rows;
    while (top > 0 && h + body[top - 1].height <= BODY_END - BODY_TOP_Y)
        h += body[--top].height;
    return top;
}

static void scroll_body(int rows)
{
    int last = body_last_top();
    body_top += rows;
    if (body_top > last)
        body_top = last;
    if (body_top < 0)
        body_top = 0;
}

/* ---------------- the text's rows ---------------- */

#define TEXT_MAX_ROWS 160
static struct { size_t start, bytes; } text_row[TEXT_MAX_ROWS];
static int text_rows, text_top;

static void build_text_rows(void)
{
    const char *s = vjo_lookup_text();
    size_t len = vjo_lookup_text_len(), at = 0;
    text_rows = text_top = 0;
    while (at < len && text_rows < TEXT_MAX_ROWS) {
        size_t take = fit(s + at, len - at, BODY_W, TEXT_PX);
        text_row[text_rows].start = at;
        text_row[text_rows].bytes = take;
        text_rows++;
        at += take;
        if (at < len && s[at] == '\n')
            at++;
        else if (!take)
            break;
    }
}

/* The row holding byte `at` (the last one for the text's end). */
static int row_of(size_t at)
{
    for (int r = 0; r < text_rows; r++)
        if (at < text_row[r].start + text_row[r].bytes || (r + 1 < text_rows && at < text_row[r + 1].start))
            return r;
    return text_rows ? text_rows - 1 : 0;
}

/* The cursor's row in view. */
static void show_cursor_row(void)
{
    int r = row_of(vjo_lookup_cursor());
    if (r < text_top)
        text_top = r;
    if (r >= text_top + TEXT_ROWS)
        text_top = r - TEXT_ROWS + 1;
}

/* After a lookup or a move: the cursor's row shown, the body laid out again. */
static void looked(void)
{
    show_cursor_row();
    build_body();
}

/* ---------------- the two texts: the typed one and the open line ---------------- */

static char search_raw[VJO_IME_BYTES];
static size_t search_cursor;
static char line_raw[VJO_LOOKUP_TEXT_MAX + 1];
static size_t line_cursor;
static VjoLookupSource line_source;
static int line_index = -1;
static int panel = -1; /* the text the lookups have: MODE_SEARCH, MODE_LINES (the open line), -1 none */

/* The panel the lookups have is let go: its cursor kept, to be looked up again when it comes back. */
static void leave_panel(void)
{
    if (panel == MODE_SEARCH)
        search_cursor = vjo_lookup_cursor();
    else if (panel == MODE_LINES)
        line_cursor = vjo_lookup_cursor();
    panel = -1;
}

static void use_panel(int which)
{
    size_t keep;
    if (panel == which)
        return;
    leave_panel();
    panel = which;
    keep = which == MODE_SEARCH ? search_cursor : line_cursor;
    vjo_lookup_set_text(which == MODE_SEARCH ? search_raw : line_raw, which == MODE_SEARCH);
    if (keep && keep != vjo_lookup_cursor() && keep < vjo_lookup_text_len())
        vjo_lookup_at(keep);
    build_text_rows();
    looked();
}

/* ---------------- the keyboard ---------------- */

static void draw_page(void);

static void keyboard_frame(void)
{
    vita2d_start_drawing();
    vita2d_clear_screen();
    draw_page();
    vita2d_end_drawing();
    vita2d_common_dialog_update();
    vita2d_swap_buffers();
    sceDisplayWaitVblankStart();
}

/* The keyboard for the search: 1 when it ran (accepted or not). */
static int type_text(void)
{
    static const uint16_t title[] = {'L', 'o', 'o', 'k', ' ', 'u', 'p', ' ', 'a', ' ', 'w', 'o', 'r', 'd', ' ',
                                     'o', 'r', ' ', 'a', ' ', 's', 'e', 'n', 't', 'e', 'n', 'c', 'e', 0};
    char typed[VJO_IME_BYTES];
    int rc;
    snprintf(typed, sizeof(typed), "%s", search_raw);
    rc = vjo_ime_dialog(title, VJO_IME_CHARS, 1, typed, sizeof(typed), keyboard_frame);
    if (rc < 0) {
        snprintf(status, sizeof(status), "Could not open the keyboard (0x%08X).", (unsigned)rc);
        status_error = 1;
        vjo_log("look up: keyboard failed 0x%08X", (unsigned)rc);
    } else if (rc > 0) {
        snprintf(search_raw, sizeof(search_raw), "%s", typed);
        search_cursor = 0;
        panel = -1; /* the new text */
        use_panel(MODE_SEARCH);
        status[0] = '\0';
        status_error = 0;
    }
    return 1;
}

/* ---------------- Earlier lines ---------------- */

static int list_selected, list_top;

/* "10-27 14:32", the line's local time, or "" without one. */
static void line_time(uint64_t us, char *out, size_t cap)
{
    SceRtcTick utc, local;
    SceDateTime t;
    out[0] = '\0';
    memset(&t, 0, sizeof(t));
    utc.tick = VJO_APP_RTC_UNIX_EPOCH + us;
    if (us && sceRtcConvertUtcToLocalTime(&utc, &local) >= 0 && sceRtcSetTick(&t, &local) >= 0 && t.year)
        snprintf(out, cap, "%02u-%02u %02u:%02u", t.month, t.day, t.hour, t.minute);
}

static void list_show(int i)
{
    const VjoLookupLine *lines;
    int n = vjo_lookup_history_lines(&lines);
    if (i >= n)
        i = n - 1;
    if (i < 0)
        i = 0;
    list_selected = i;
    if (i < list_top)
        list_top = i;
    if (i >= list_top + LIST_ROWS)
        list_top = i - LIST_ROWS + 1;
}

static void open_line(int i)
{
    if (vjo_lookup_history_line(i, line_raw, sizeof(line_raw), &line_source) < 0) {
        snprintf(status, sizeof(status), "history.csv changed: it is read again.");
        status_error = 1;
        vjo_lookup_history_open(cfg.non_japanese_filter);
        return;
    }
    leave_panel(); /* (the Search text's cursor is kept for L back to it) */
    line_index = i;
    line_cursor = 0;
    line_open = 1;
    use_panel(MODE_LINES);
    status[0] = '\0';
    status_error = 0;
}

static void next_game(void)
{
    const VjoLookupGame *games;
    int n = vjo_lookup_history_games(&games), g = vjo_lookup_history_game();
    if (n < 2)
        return;
    vjo_lookup_history_select((g + 1) % n);
    list_selected = list_top = 0;
}

/* ---------------- input ---------------- */

static VjoStickCursor stick;   /* the left stick over the text: a character a step, its word looked up at rest */
static VjoStickScroll rstick;  /* the right stick: the entry's rows */
static uint64_t repeat_at;     /* a held d-pad key's next step */

/* A d-pad key just pressed, or held long enough to repeat (the Games page's 350 ms, then 90 ms). */
static uint32_t repeating(const VjoLookupInput *in, uint32_t keys)
{
    uint32_t fresh = in->pressed & keys, held = in->held & keys;
    if (fresh) {
        repeat_at = in->now_us + 350000;
        return fresh;
    }
    if (held && in->now_us >= repeat_at) {
        repeat_at = in->now_us + 90000;
        return held;
    }
    return 0;
}

/* The stick's word, when the stick has moved the cursor and its lookup is still to come. */
static void catch_up(void)
{
    if (!vjo_lookup_fresh()) {
        stick.due = 0;
        vjo_lookup_at(vjo_lookup_cursor());
        looked();
    }
}

static void mine(void)
{
    VjoLookupSource typed;
    catch_up();
    memset(&typed, 0, sizeof(typed));
    typed.typed = 1;
    int rc = vjo_lookup_mine(panel == MODE_LINES ? &line_source : &typed, status, sizeof(status));
    status_error = rc < 0 && rc != VJO_APP_QUEUE_FAILED - 10;
    vjo_log("look up: card from %s rc=%d", panel == MODE_LINES ? "an earlier line" : "a typed text", rc);
}

/* The text and its entry: the cursor, the scroll, a card. */
static void text_input(const VjoLookupInput *in)
{
    uint32_t arrows = repeating(in, SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_UP | SCE_CTRL_DOWN);
    int dx = arrows & SCE_CTRL_LEFT ? -1 : arrows & SCE_CTRL_RIGHT ? 1 : 0;
    int st = vjo_stick_cursor(&stick, in->lx, in->ly, in->now_us), rows;
    stick.rest_us = (uint32_t)cfg.stick_lookup_ms * 1000u;
    if (dx) {
        size_t was = vjo_lookup_cursor();
        stick.due = 0; /* an arrow takes the frame (core/stick_cursor.h) */
        if (vjo_lookup_step(dx, 0)) {
            /* Round the text's ends only as the overlay goes round them (core/text_rows.h): a text on one displayed
             * row, and a fresh press (a held key stops there). The step has no layout (the line breaks stand for
             * the rows), so a text the page wraps into more rows stops here. */
            int round = dx > 0 ? vjo_lookup_cursor() < was : vjo_lookup_cursor() > was;
            if (round && (!(in->pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) || text_rows > 1))
                vjo_lookup_at(was);
            else
                vjo_lookup_at(vjo_lookup_cursor());
            looked();
        }
    } else if (st == 1 || st == -1) {
        int moved = vjo_lookup_step(st, 1);
        vjo_stick_cursor_moved(&stick, moved, in->now_us);
        if (moved)
            show_cursor_row(); /* the entry stays until the cursor rests and its word is looked up */
    } else if (st) {
        scroll_body(st / 2); /* the stick's up and down: the entry, as Up/Down */
    }
    if (vjo_stick_cursor_lookup(&stick, in->now_us)) {
        vjo_lookup_at(vjo_lookup_cursor());
        looked();
    }
    if (arrows & SCE_CTRL_UP)
        scroll_body(-1);
    if (arrows & SCE_CTRL_DOWN)
        scroll_body(1);
    if ((rows = vjo_stick_scroll(&rstick, in->ry, in->now_us)))
        scroll_body(rows);
}

/* A touch on the text: the character there. */
static void touch_text(int x, int y)
{
    const char *s = vjo_lookup_text();
    int r = text_top + (y - TEXT_Y - (TEXT_BASE - TEXT_LINE + 6)) / TEXT_LINE;
    size_t at, end, i;
    int w = BODY_X;
    if (r < text_top || r >= text_rows || r >= text_top + TEXT_ROWS)
        return;
    at = text_row[r].start;
    end = at + text_row[r].bytes;
    for (i = at; i < end;) {
        size_t begin = i;
        int cw = char_width(vjo_utf8_next(s, end, &i), TEXT_PX);
        if (x < w + cw) {
            at = begin;
            break;
        }
        w += cw;
        at = i;
    }
    if (at >= end && end > text_row[r].start) /* past the row's end: its last character */
        at = vjo_utf8_cursor_step(s, end, end - 1, 0);
    stick.due = 0;
    vjo_lookup_at(at);
    looked();
}

static int holds_text(void) { return mode == MODE_SEARCH || line_open; }

static void switch_mode(int to)
{
    mode = to;
    status[0] = '\0';
    status_error = 0;
    if (mode == MODE_SEARCH)
        use_panel(MODE_SEARCH);
    else if (line_open)
        use_panel(MODE_LINES);
    vjo_stick_cursor_latch(&stick);
}

static int input(const VjoLookupInput *in)
{
    int x = in->touch_x, y = in->touch_y;
    if (in->pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) {
        switch_mode(mode == MODE_SEARCH ? MODE_LINES : MODE_SEARCH);
        return 0;
    }
    if (y >= MODE_Y && y < MODE_Y + MODE_H) { /* the modes; the game */
        if (x >= SEARCH_X && x < SEARCH_X + SEARCH_W)
            switch_mode(MODE_SEARCH);
        else if (x >= LINES_X && x < LINES_X + LINES_W) {
            if (mode == MODE_LINES && line_open)
                line_open = 0; /* the open mode's button: back to the list */
            switch_mode(MODE_LINES);
        } else if (x >= INFO_X && mode == MODE_LINES && !line_open)
            next_game();
        return 0;
    }
    if (mode == MODE_SEARCH) {
        int on_text = y >= TEXT_Y && y < TEXT_Y + TEXT_H && x >= 24 && x < 936;
        if (in->pressed & s_ui->buttons->enter) {
            /* Enter: a card of the entry shown (none there: it says so); with no text yet, the keyboard */
            if (!vjo_lookup_text_len())
                return type_text();
            mine();
            return 0;
        }
        if ((in->pressed & SCE_CTRL_TRIANGLE) || (on_text && !vjo_lookup_text_len()))
            return type_text();
        if (on_text)
            touch_text(x, y);
        text_input(in);
        return 0;
    }
    if (line_open) {
        if (in->pressed & s_ui->buttons->cancel) {
            line_cursor = vjo_lookup_cursor();
            line_open = 0;
            status[0] = '\0';
            status_error = 0;
            return 0;
        }
        if (in->pressed & s_ui->buttons->enter) {
            mine();
            return 0;
        }
        if (y >= TEXT_Y && y < TEXT_Y + TEXT_H && x >= 24 && x < 936)
            touch_text(x, y);
        text_input(in);
        return 0;
    }
    /* the list */
    {
        const VjoLookupLine *lines;
        int n = vjo_lookup_history_lines(&lines);
        uint32_t keys = repeating(in, SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT);
        if (keys & SCE_CTRL_UP)
            list_show(list_selected - 1);
        else if (keys & SCE_CTRL_DOWN)
            list_show(list_selected + 1);
        else if (keys & SCE_CTRL_LEFT)
            list_show(list_selected - LIST_ROWS);
        else if (keys & SCE_CTRL_RIGHT)
            list_show(list_selected + LIST_ROWS);
        else if (in->stick_rows)
            list_show(vjo_stick_step(list_selected, n, in->stick_rows));
        if (in->pressed & SCE_CTRL_SQUARE)
            next_game();
        if (in->pressed & SCE_CTRL_START)
            vjo_lookup_history_open(cfg.non_japanese_filter);
        if ((in->pressed & s_ui->buttons->enter) && n)
            open_line(list_selected);
        else if (x >= 24 && x < 936 && y >= LIST_Y && y < LIST_Y + LIST_ROWS * LIST_ROW &&
                 (y - LIST_Y) % LIST_ROW < LIST_H && list_top + (y - LIST_Y) / LIST_ROW < n) {
            list_selected = list_top + (y - LIST_Y) / LIST_ROW;
            open_line(list_selected);
        }
    }
    return 0;
}

/* ---------------- drawing ---------------- */

/* s cut to `width` at px with "…" when it is wider, into out. */
static void cut(const char *s, int width, int px, char *out, size_t cap)
{
    size_t len = strlen(s), i = 0, keep = 0;
    int w = 0, dots = measure("…", strlen("…"), px);
    if (measure(s, len, px) <= width) {
        snprintf(out, cap, "%s", s);
        return;
    }
    while (i < len) {
        w += char_width(vjo_utf8_next(s, len, &i), px);
        if (w + dots > width)
            break;
        keep = i;
    }
    if (keep > cap - 4)
        keep = cap - 4;
    while (keep && ((unsigned char)s[keep] & 0xC0) == 0x80)
        keep--;
    memcpy(out, s, keep);
    snprintf(out + keep, cap - keep, "…");
}

/* A piece of text at x, its bytes copied to a terminated string. */
static void draw_bytes(int x, int y, unsigned color, int px, const char *s, size_t n)
{
    char buf[ROW_BYTES + 4];
    if (n > ROW_BYTES)
        n = ROW_BYTES;
    memcpy(buf, s, n);
    buf[n] = '\0';
    s_ui->text(x, y, color, (float)px, buf);
}

static void draw_modes(void)
{
    char info[200];
    s_ui->badge(SEARCH_X, MODE_Y, SEARCH_W, MODE_H, "Search", mode == MODE_SEARCH);
    s_ui->badge(LINES_X, MODE_Y, LINES_W, MODE_H, "Earlier lines", mode == MODE_LINES);
    info[0] = '\0';
    if (mode == MODE_LINES) {
        const VjoLookupGame *games;
        int n = vjo_lookup_history_games(&games), g = vjo_lookup_history_game();
        if (line_open) {
            char when[24];
            line_time(line_source.time_us, when, sizeof(when));
            snprintf(info, sizeof(info), "%s%s%s", line_source.game, when[0] ? "  " : "", when);
        } else if (g >= 0) {
            snprintf(info, sizeof(info), "%s%s", games[g].name, n > 1 ? "  (Square: game)" : "");
        }
    }
    if (info[0]) {
        char shown[240];
        cut(info, INFO_W - 12, 24, shown, sizeof(shown));
        s_ui->text(INFO_X + 8, MODE_Y + 28, s_ui->white, 24, shown);
    }
}

static void draw_text(void)
{
    const char *s = vjo_lookup_text();
    size_t hs, he;
    s_ui->card(TEXT_Y, TEXT_H, 0);
    if (!vjo_lookup_text_len()) {
        s_ui->text(BODY_X, TEXT_Y + 46, s_ui->dim, 24,
                   mode == MODE_SEARCH ? "Triangle, or a tap here: type a word or a sentence." : "");
        return;
    }
    vjo_lookup_highlight(&hs, &he);
    for (int r = text_top; r < text_rows && r < text_top + TEXT_ROWS; r++) {
        size_t a = text_row[r].start, b = a + text_row[r].bytes;
        size_t ha = hs > a ? hs : a, hb = he < b ? he : b;
        int y = TEXT_Y + TEXT_BASE + (r - text_top) * TEXT_LINE, x = BODY_X;
        if (ha >= hb) { /* no highlight in this row */
            draw_bytes(x, y, s_ui->white, TEXT_PX, s + a, b - a);
            continue;
        }
        draw_bytes(x, y, s_ui->white, TEXT_PX, s + a, ha - a);
        x += measure(s + a, ha - a, TEXT_PX);
        draw_bytes(x, y, HIGHLIGHT, TEXT_PX, s + ha, hb - ha);
        s_ui->rect(x, y + 5, measure(s + ha, hb - ha, TEXT_PX), 2, HIGHLIGHT);
        x += measure(s + ha, hb - ha, TEXT_PX);
        draw_bytes(x, y, s_ui->white, TEXT_PX, s + hb, b - hb);
    }
    if (text_rows > TEXT_ROWS) { /* where the shown rows are in the text */
        vjo_page_scroll_bar(s_ui, SCROLL_X, TEXT_Y + 4, SCROLL_W, TEXT_H - 8, text_top, TEXT_ROWS, text_rows);
    }
}

static void draw_body(void)
{
    int y = BODY_TOP_Y, total = 0, before = 0, shown = 0;
    s_ui->card(ENTRY_Y, ENTRY_END - ENTRY_Y, 0);
    s_ui->clip(24, ENTRY_Y + 2, 936, ENTRY_END - 2);
    for (int i = body_top; i < body_rows && y + body[i].height <= BODY_END; i++) {
        const Row *r = &body[i];
        int base = y + r->height * 3 / 4 + 1;
        for (int k = 0; k < r->pieces; k++)
            draw_bytes(BODY_X + r->piece[k].x, base, r->piece[k].color, r->piece[k].px, r->piece[k].s,
                       r->piece[k].bytes);
        y += r->height;
        shown += r->height;
    }
    s_ui->clip(0, 0, 0, 0);
    for (int i = 0; i < body_rows; i++) {
        total += body[i].height;
        if (i < body_top)
            before += body[i].height;
    }
    if (total > BODY_END - BODY_TOP_Y) { /* more than the room: a bar where the view is */
        vjo_page_scroll_bar(s_ui, SCROLL_X, BODY_TOP_Y, SCROLL_W, BODY_END - BODY_TOP_Y, before, shown, total);
    }
}

static void draw_list(void)
{
    const VjoLookupLine *lines;
    int n = vjo_lookup_history_lines(&lines), state = vjo_lookup_history_state();
    for (int r = 0; r < LIST_ROWS && list_top + r < n; r++) {
        const VjoLookupLine *l = &lines[list_top + r];
        int y = LIST_Y + r * LIST_ROW;
        char when[24], shown[VJO_LOOKUP_PREVIEW + 8];
        s_ui->card(y, LIST_H, list_top + r == list_selected);
        line_time(l->time_us, when, sizeof(when));
        s_ui->text(BODY_X, y + 26, s_ui->dim, 20, when);
        cut(l->preview, 914 - 172, 24, shown, sizeof(shown));
        s_ui->text(172, y + 28, s_ui->white, 24, shown);
    }
    if (!n) {
        char why[200];
        if (state == VJO_LOOKUP_HISTORY_READING)
            snprintf(why, sizeof(why), "Reading history.csv…");
        else if (state == VJO_LOOKUP_HISTORY_FAILED)
            snprintf(why, sizeof(why), "Cannot read history.csv (0x%08X). Start: try again.",
                     (unsigned)vjo_lookup_history_error());
        else if (state == VJO_LOOKUP_HISTORY_NONE)
            snprintf(why, sizeof(why), "No earlier lines yet: Rubi keeps the lines it reads with Read text: "
                                       "Automatically, in history.csv.");
        else
            snprintf(why, sizeof(why), "No lines of this game.");
        s_ui->wrapped(BODY_X, LIST_Y + 34, 870, state == VJO_LOOKUP_HISTORY_FAILED ? s_ui->off : s_ui->white, 24, 30,
                      3, why);
    } else if (n > LIST_ROWS) {
        vjo_page_scroll_bar(s_ui, 942, LIST_Y, SCROLL_W, LIST_ROWS * LIST_ROW - (LIST_ROW - LIST_H), list_top,
                            LIST_ROWS, n);
    }
}

static void draw_page(void)
{
    char controls[160], line[200];
    const char *e = s_ui->buttons->enter_glyph, *c = s_ui->buttons->cancel_glyph;
    const char *message = status;
    int error = status_error;
    s_ui->header();
    draw_modes();
    if (holds_text()) {
        draw_text();
        draw_body();
    } else {
        draw_list();
    }
    if (mode == MODE_SEARCH && !vjo_lookup_text_len())
        snprintf(controls, sizeof(controls), "Triangle or %s: type   L/R: earlier lines   Select: settings   %s: exit", e, c);
    else if (mode == MODE_SEARCH)
        snprintf(controls, sizeof(controls), "Triangle: type   Left/Right: word   %s: save card   L/R: lines   %s: exit",
                 e, c);
    else if (line_open)
        snprintf(controls, sizeof(controls), "Left/Right: word   Up/Down: scroll   %s: save card   %s: back", e, c);
    else
        snprintf(controls, sizeof(controls), "Up/Down: line   %s: open   Square: game   L/R: search   %s: exit", e, c);
    if (!message[0] && mode == MODE_LINES && !line_open) {
        const VjoLookupGame *games;
        const VjoLookupLine *lines;
        int g = vjo_lookup_history_game(), n = vjo_lookup_history_lines(&lines);
        vjo_lookup_history_games(&games);
        if (vjo_lookup_history_state() == VJO_LOOKUP_HISTORY_READING)
            snprintf(line, sizeof(line), "Reading history.csv… %llu of %llu KB", 
                     (unsigned long long)(vjo_lookup_history_read() >> 10),
                     (unsigned long long)((vjo_lookup_history_size() < VJO_LOOKUP_SCAN_MAX ? vjo_lookup_history_size()
                                                                                         : VJO_LOOKUP_SCAN_MAX) >> 10));
        else if (g >= 0)
            snprintf(line, sizeof(line), "%d line%s, newest first: %s opens one.", n, n == 1 ? "" : "s", e);
        else
            line[0] = '\0';
        message = line;
    }
    s_ui->footer(STATUS_Y, STATUS_H, STATUS_Y + 28, 1, message, error, controls);
}

/* ---------------- the page ---------------- */

void vjo_lookup_page_show(VjoArena *scratch)
{
    VjoConfig loaded;
    if (vjo_preferences_load(&loaded, scratch) < 0) {
        if (!configured)
            vjo_config_defaults(&loaded);
        else
            loaded = cfg; /* config.ini unreadable now: the settings it had */
    }
    /* The text is looked up again at its cursor, with the settings as they are now: the dictionaries, their order
     * and count, names_first (#321), the filter, the cursor's rules (any of them may have changed meanwhile). */
    leave_panel();
    cfg = loaded;
    configured = 1;
    vjo_lookup_configure(&cfg);
    vjo_lookup_history_open(cfg.non_japanese_filter);
    vjo_stick_cursor_latch(&stick);
    vjo_stick_latch(&rstick);
}

int vjo_lookup_page_frame(const VjoPageUi *ui, const VjoLookupInput *in)
{
    int dialog;
    s_ui = ui;
    if (!configured)
        return 0; /* (main.c shows the page first) */
    if (panel < 0 && holds_text())
        use_panel(mode == MODE_SEARCH ? MODE_SEARCH : MODE_LINES);
    /* history.csv's scan, a part of it each frame (about 12 ms) while the list is shown */
    if (mode == MODE_LINES && !line_open)
        vjo_lookup_history_step(12000);
    dialog = input(in);
    if (dialog)
        return 1; /* the keyboard drew its own frames */
    vita2d_start_drawing();
    vita2d_clear_screen();
    draw_page();
    vita2d_end_drawing();
    vita2d_swap_buffers();
    return 0;
}

int vjo_lookup_page_holds_cancel(void) { return mode == MODE_LINES && line_open; }

void vjo_lookup_page_leave(void) { vjo_lookup_close(); }

void vjo_lookup_page_quit(void) { vjo_lookup_close(); }
