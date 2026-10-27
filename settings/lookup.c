#include "lookup.h"

#include <psp2/rtc.h>
#include <stdio.h>
#include <string.h>

#include "app_platform.h"
#include "app_queue.h"
#include "../core/anki.h"
#include "../core/local_dict.h"
#include "../core/text_rows.h"
#include "../core/textfilter.h"
#include "../core/utf.h"

/* The memory, all static (the app's own code does not malloc: settings/sysapp.c):
 * - the lookup block, as the Shell's (shell/worker.c LOCAL_RESULT_ARENA_SIZE): the dictionary session at its start,
 *   the lookup's arena (its workspace, the entry and the stacked ones) after it;
 * - the text's filter and the error's text;
 * - a card (shell/anki.c ANKI_TEXT_MEM): its note, its other entries, its two records' encodings. */
#define BLOCK_BYTES (64u * 1024u)
#define SCRATCH_BYTES (48u * 1024u)
#define CARD_BYTES (128u * 1024u)
typedef char vjo_lookup_block_fits[VJO_LOCAL_RESULT_BUDGET == BLOCK_BYTES ? 1 : -1];
static uint8_t block[BLOCK_BYTES] __attribute__((aligned(16)));
static uint8_t scratch_mem[SCRATCH_BYTES] __attribute__((aligned(16)));
static uint8_t card_mem[CARD_BYTES] __attribute__((aligned(16)));
static VjoArena arena, scratch, card;
static VjoPlatform plat;
static int ready;
static VjoConfig cfg;

static char text[VJO_LOOKUP_TEXT_MAX + 1];
static size_t text_len, cursor, looked_up = (size_t)-1; /* looked_up: the cursor the result is for */
static uint32_t generation;
static VjoOverlayData data;
static char error[256];
static VjoRowNav nav;

size_t vjo_lookup_static_bytes(void)
{
    return sizeof(block) + sizeof(scratch_mem) + sizeof(card_mem) + sizeof(text);
}

static void setup(void)
{
    size_t session = vjo_local_session_size();
    if (ready)
        return;
    vjo_app_platform(&plat);
    memset(block, 0, session);
    plat.local_session = (VjoLocalSession *)(void *)block;
    vjo_arena_init(&arena, block + session, sizeof(block) - session);
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    vjo_arena_init(&card, card_mem, sizeof(card_mem));
    vjo_config_defaults(&cfg);
    ready = 1;
}

void vjo_lookup_configure(const VjoConfig *c)
{
    setup();
    if (strcmp(c->local_dictionary_dir, cfg.local_dictionary_dir) || strcmp(c->local_dictionaries, cfg.local_dictionaries))
        vjo_local_session_close(plat.local_session); /* other files: opened again at the next lookup */
    cfg = *c;
    looked_up = (size_t)-1; /* another count of dictionaries or another filter: look up again */
}

const VjoConfig *vjo_lookup_config(void)
{
    setup();
    return &cfg;
}

void vjo_lookup_close(void)
{
    if (ready)
        vjo_local_session_close(plat.local_session);
}

const char *vjo_lookup_text(void) { return text; }
size_t vjo_lookup_text_len(void) { return text_len; }
size_t vjo_lookup_cursor(void) { return cursor; }
const char *vjo_lookup_error(void) { return error; }

static void clear_result(void)
{
    memset(&data, 0, sizeof(data));
    data.ocr_text = data.filtered = data.sentence = text;
    data.list.header = text;
    error[0] = '\0';
}

int vjo_lookup_fresh(void) { return looked_up == cursor; }

const VjoOverlayData *vjo_lookup_result(void)
{
    static VjoOverlayData none;
    if (looked_up != cursor) { /* the stick moved the cursor: no entry for it yet */
        memset(&none, 0, sizeof(none));
        none.sentence = none.ocr_text = none.filtered = none.list.header = text;
        return &none;
    }
    return &data;
}

int vjo_lookup_set_text(const char *t, int typed)
{
    char *f, *trimmed;
    size_t n;
    setup();
    vjo_arena_reset(&scratch);
    f = typed ? vjo_arena_strndup(&scratch, t, strlen(t)) : vjo_filter_lines(&scratch, t, cfg.non_japanese_filter);
    trimmed = f ? vjo_java_trim(f) : NULL;
    n = trimmed ? strlen(trimmed) : 0;
    if (n > VJO_LOOKUP_TEXT_MAX) { /* (a history.csv line is at most 4095 bytes, a typed text 512) */
        n = VJO_LOOKUP_TEXT_MAX;
        while (n && ((unsigned char)trimmed[n] & 0xC0) == 0x80)
            n--; /* on a character */
    }
    if (n)
        memcpy(text, trimmed, n);
    text[n] = '\0';
    text_len = n;
    vjo_arena_reset(&scratch);
    if (!++generation) /* the session's locator cache keys on it: never 0 */
        generation = 1;
    memset(&nav, 0, sizeof(nav));
    cursor = 0;
    looked_up = (size_t)-1;
    clear_result();
    vjo_arena_reset(&arena);
    if (!n)
        return -1;
    /* the overlay's first selection (shell/worker.c): the first Japanese letter, or with Japanese characters only
     * OFF the text's start */
    vjo_lookup_at(cfg.japanese_only_selection ? vjo_utf8_japanese_cursor_step(text, text_len, 0, 0) : 0);
    return 0;
}

int vjo_lookup_at(size_t at)
{
    int rc;
    setup();
    if (at > text_len)
        at = text_len;
    if (at < text_len)
        at = vjo_utf8_cursor_step(text, text_len, at, 0);
    cursor = at;
    looked_up = at;
    clear_result();
    vjo_arena_reset(&arena); /* the previous entry: the session before the arena keeps its files */
    if (at >= text_len)
        return VJO_OK; /* past the last selectable character: nothing to look up */
    plat.local_text_generation = generation;
    rc = vjo_overlay_lookup_at(&arena, &plat, &cfg, at, &data);
    if (rc) {
        size_t mark = vjo_arena_mark(&scratch);
        snprintf(error, sizeof(error), "%s", vjo_err_text(&scratch, data.failed_stage, &data.err));
        vjo_utf8_trim_partial(error);
        vjo_arena_release(&scratch, mark);
    }
    return rc;
}

int vjo_lookup_step(int dx, int stick)
{
    size_t to = cursor, matched = 0;
    const VjoEntryList *l = &data.list;
    if (!text_len || !dx)
        return 0;
    /* Whole word: Right skips the shown word (the overlay's s_match_end); Character mode, a pending lookup and the
     * stick a character */
    if (!stick && cfg.highlight_mode == VJO_HIGHLIGHT_WORD && looked_up == cursor && l->n_entries &&
        l->entries[0].hl_start == (int)cursor && l->entries[0].hl_end > (int)cursor)
        matched = (size_t)l->entries[0].hl_end;
    vjo_text_row_key(&nav, text, text_len, NULL, cursor, matched, dx, 0, cfg.japanese_only_selection, stick, &to);
    if (to == cursor)
        return 0;
    cursor = to; /* looked up by the caller: at once for the d-pad, once the stick rests */
    return 1;
}

void vjo_lookup_highlight(size_t *start, size_t *end)
{
    const VjoEntryList *l = &data.list;
    *start = *end = cursor;
    if (cursor >= text_len)
        return;
    if (cfg.highlight_mode == VJO_HIGHLIGHT_WORD && looked_up == cursor && l->n_entries && l->entries[0].hl_start >= 0 &&
        l->entries[0].hl_end > l->entries[0].hl_start) {
        *start = (size_t)l->entries[0].hl_start;
        *end = (size_t)l->entries[0].hl_end;
        return;
    }
    vjo_utf8_next(text, text_len, end); /* the cursor's character */
}

/* ---------------- a card ---------------- */

static const char *keep(const char *s)
{
    return s && *s ? vjo_arena_strndup(&card, s, strlen(s)) : NULL;
}

/* The local date and time of a UTC time (microseconds since 1970), as the Vita's clock is set. */
static int local_date(uint64_t utc_us, VjoAnkiDate *d)
{
    SceRtcTick utc, local;
    SceDateTime t;
    memset(&t, 0, sizeof(t));
    utc.tick = 62135596800000000ull + utc_us; /* microseconds since 0001-01-01 */
    if (sceRtcConvertUtcToLocalTime(&utc, &local) < 0 || sceRtcSetTick(&t, &local) < 0 || !t.year)
        return 0;
    *d = (VjoAnkiDate){t.year, t.month, t.day, t.hour, t.minute};
    return 1;
}

/* Does the note, as it is now, still make a record (at most VJO_QUEUE_TEXT_MAX, in the card arena)? */
static int fits(const VjoAnkiNote *note)
{
    size_t mark = vjo_arena_mark(&card);
    int ok = vjo_queue_encode(&card, note, 0) != NULL;
    vjo_arena_release(&card, mark);
    return ok;
}

int vjo_lookup_note(const VjoLookupSource *src, VjoAnkiNote *note, char *misc, size_t misc_cap)
{
    const VjoEntryList *l = &data.list;
    VjoAnkiDate when;
    SceDateTime now;
    SceRtcTick tick;
    VjoAnkiGloss *g;
    int dated;
    setup();
    if (looked_up != cursor || !l->n_entries)
        return -1;
    vjo_arena_reset(&card);
    if (vjo_anki_note_from_entry(&card, l, 0, note) < 0)
        return -1;
    /* A typed text is the card's sentence only when it is longer than the word: a word alone has none. */
    if (src->typed && note->hl_start <= 0 && note->hl_end >= (int)text_len) {
        note->sentence = note->sentence_furigana = "";
        note->hl_start = note->hl_end = note->sf_hl_start = note->sf_hl_end = -1;
    }
    if (!src->typed) {
        note->game = keep(src->game);
        note->title_id = keep(src->title_id);
        note->platform = note->title_id ? keep(src->platform[0] ? src->platform : "vita") : NULL;
        note->ocr_engine = keep(src->engine);
        note->ocr_model = !strcmp(src->engine, "vocr") ? keep(src->model) : NULL; /* a hook's line: no model read it */
    }
    /* MiscInfo: the game and the line's local date (none for a line without a time: no date rather than a wrong
     * one); a typed search the date it is saved */
    memset(&now, 0, sizeof(now));
    if (!src->typed) {
        dated = src->time_us && local_date(src->time_us, &when);
    } else {
        dated = sceRtcGetCurrentClockLocalTime(&now) >= 0 && now.year;
        when = (VjoAnkiDate){now.year, now.month, now.day, now.hour, now.minute};
    }
    vjo_anki_misc_info(misc, misc_cap, note->game, dated ? &when : NULL);
    note->misc = misc;
    /* when it is saved, to the minute (shell/anki.c card_facts): the same card saved again within it is that card */
    note->unix_time = sceRtcGetCurrentTick(&tick) >= 0 && tick.tick > 62135596800000000ull
        ? (int64_t)((tick.tick - 62135596800000000ull) / 60000000ull) * 60 : 0;
    /* the other dictionaries' entries for the word, as the overlay's card keeps them, while the record fits */
    if (l->n_more > 0 && (g = vjo_arena_zalloc(&card, sizeof(*g) * VJO_ANKI_MAX_MORE))) {
        note->more = g;
        for (int i = 0; i < l->n_more && note->n_more < VJO_ANKI_MAX_MORE; i++) {
            const VjoVocab *v = l->more[i];
            VjoAnkiGloss *k = &g[note->n_more];
            char name[96];
            size_t mark = vjo_arena_mark(&card);
            if (!v || !v->n_meanings)
                continue;
            k->spelling = v->spelling;
            k->reading = v->reading;
            k->meanings = v->meanings;
            k->n_meanings = v->n_meanings;
            k->dictionary = v->dictionary;
            if (!k->dictionary) {
                vjo_anki_meaning_source(v->meanings[0], name, sizeof(name));
                k->dictionary = keep(name);
            }
            note->n_more++;
            if (!fits(note)) {
                note->n_more--;
                vjo_arena_release(&card, mark);
                break;
            }
        }
    }
    return 0;
}

int vjo_lookup_mine(const VjoLookupSource *src, char *msg, size_t cap)
{
    VjoAnkiNote note;
    char misc[160];
    int rc;
    if (vjo_lookup_note(src, &note, misc, sizeof(misc)) < 0) {
        snprintf(msg, cap, "%s", looked_up == cursor ? "No entry here to save." : "Looking it up first: try again.");
        return VJO_APP_QUEUE_FAILED - 10;
    }
    rc = vjo_app_queue_save(&card, &note, NULL);
    switch (rc) {
    case VJO_APP_QUEUE_SAVED: snprintf(msg, cap, "Saved for Anki, without a picture: export it on the Anki page."); break;
    case VJO_APP_QUEUE_EXISTS: snprintf(msg, cap, "Already queued"); break;
    case VJO_APP_QUEUE_ARCHIVED: snprintf(msg, cap, "Already exported (in an .apkg)"); break;
    case VJO_APP_QUEUE_TOO_LARGE: snprintf(msg, cap, "Card not saved: over %u KiB of text", VJO_QUEUE_TEXT_MAX >> 10); break;
    default: snprintf(msg, cap, "Card not saved: cannot write ux0:data/VitaJPOverlay/anki_queue (space?)"); break;
    }
    vjo_arena_reset(&card);
    return rc;
}
