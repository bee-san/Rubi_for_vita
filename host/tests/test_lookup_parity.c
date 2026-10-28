/* The Look up page's lookups (settings/lookup.c, #313) are the overlay's: on the real VOCR lines of
 * host/fixtures/word_lookup (test_word_lookup.cpp's golden lines) with the bundled dictionary, the app's engine and
 * the overlay's own pipeline (core/client.c vjo_overlay_ocr_text, vjo_overlay_lookup_at over the Shell's memory
 * layout, the host's file platform) take the same sentence, move their cursor word by word to the same places
 * (core/text_rows.h, as the overlay's Right does), and show the same entry at each: the same headword, reading,
 * frequency, conjugation, definitions, highlighted span and stacked dictionaries, in each highlight and cursor
 * mode. A card made there records that entry, with the line as its sentence.
 *
 * VJO_PLAINTEXT_DIR names the directory holding plaintext.vjdict (CMake prepares it); without it the test is
 * skipped (exit 77) unless VJO_REQUIRE_PLAINTEXT is set. */
#define main acutest_main
#include "acutest.h"
#undef main
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <psp2/io/fcntl.h>
#include <psp2/rtc.h>
#include "../../settings/lookup.h"
#include "anki_queue.h"
#include "client.h"
#include "local_dict.h"
#include "platform_posix.h"
#include "text_rows.h"
#include "utf.h"

#ifndef VJO_WORD_FIXTURES
#define VJO_WORD_FIXTURES "host/fixtures/word_lookup"
#endif

/* ---------------- the Vita's file API on the host's files (the app's platform reads through it) ---------------- */
#include "fakes/sceio_posix.h"
uint64_t sceKernelGetProcessTimeWide(void) { return 1; }
/* (a card's facts: the clock is not what this suite compares) */
int sceRtcGetCurrentTick(SceRtcTick *t) { t->tick = 62135596800000000ull + 1792000000ull * 1000000ull; return 0; }
int sceRtcGetCurrentClockLocalTime(SceDateTime *t) { memset(t, 0, sizeof(*t)); return -1; }
int sceRtcConvertUtcToLocalTime(const SceRtcTick *utc, SceRtcTick *local) { *local = *utc; return 0; }
int sceRtcSetTick(SceDateTime *d, const SceRtcTick *t) { (void)t; memset(d, 0, sizeof(*d)); return -1; }
/* settings/app_queue.c is not in this suite: a card is only made, never saved */
int vjo_app_queue_save(VjoArena *a, const VjoAnkiNote *note, char id[VJO_QUEUE_ID_SIZE]) { return -1; }
void vjo_log(const char *fmt, ...) { (void)fmt; }

/* ---------------- the lines ---------------- */

#define LINES 64
static char line_text[LINES][2048];
static int lines;

static void load(void)
{
    char path[1024], buf[8192];
    FILE *f;
    if (lines)
        return;
    snprintf(path, sizeof(path), "%s/vocr_lines.tsv", VJO_WORD_FIXTURES);
    TEST_ASSERT_((f = fopen(path, "r")) != NULL, "%s", path);
    while (lines < LINES && fgets(buf, sizeof(buf), f)) {
        char *col = buf, *out = line_text[lines];
        size_t n = 0;
        if (buf[0] == '#' || buf[0] == '\n')
            continue;
        for (int k = 0; k < 5 && col; k++)
            col = strchr(col, '\t') ? strchr(col, '\t') + 1 : NULL;
        TEST_ASSERT(col != NULL);
        for (; *col && *col != '\n' && *col != '\r' && n + 1 < sizeof(line_text[0]); col++) {
            if (col[0] == '\\' && col[1] == 'n') {
                out[n++] = '\n';
                col++;
            } else {
                out[n++] = *col;
            }
        }
        out[n] = '\0';
        lines++;
    }
    fclose(f);
    TEST_ASSERT(lines > 40);
    /* and lines with rows that have no Japanese (a speaker's romanized name, a menu's Latin): the overlay leaves those
     * rows out (non_japanese_filter = lines, the default), so an earlier line's text has none of them either */
    static const char *const latin_rows[] = {"Tomoya\n「猫が好きだ」", "Hello world\n昨日すき焼きを食べました\nEND",
                                             "ABC 123\n勉強しておかなきゃ"};
    for (unsigned i = 0; i < sizeof(latin_rows) / sizeof(latin_rows[0]) && lines < LINES; i++)
        snprintf(line_text[lines++], sizeof(line_text[0]), "%s", latin_rows[i]);
}

/* ---------------- the overlay's side: the Shell's one 64 KiB block, session first ---------------- */

static uint8_t shell_mem[64 * 1024] __attribute__((aligned(16)));
static VjoArena shell_arena;
static VjoPlatform shell_p;
static PosixPlatform posix;
static char shell_text[VJO_LOCAL_MAX_TEXT + 1];

static void shell_init(void)
{
    size_t session = vjo_local_session_size();
    posix_platform_init(&posix, &shell_p);
    memset(shell_mem, 0, session);
    shell_p.local_session = (VjoLocalSession *)(void *)shell_mem;
    vjo_arena_init(&shell_arena, shell_mem + session, sizeof(shell_mem) - session);
}

/* The overlay's sentence of a recognized text (vjo_overlay_ocr_text: filtered, trimmed), kept as the Shell keeps
 * its local text. */
static void shell_sentence(const VjoConfig *cfg, const char *text)
{
    VjoOverlayData d;
    vjo_arena_reset(&shell_arena);
    TEST_ASSERT(vjo_overlay_ocr_text(&shell_arena, cfg, text, &d) == VJO_OK);
    snprintf(shell_text, sizeof(shell_text), "%s", d.sentence);
}

static int shell_lookup(const VjoConfig *cfg, size_t cursor, VjoOverlayData *d)
{
    memset(d, 0, sizeof(*d));
    d->ocr_text = d->filtered = d->sentence = d->list.header = shell_text;
    vjo_arena_reset(&shell_arena);
    return vjo_overlay_lookup_at(&shell_arena, &shell_p, cfg, cursor, d);
}

static int same_vocab(const VjoVocab *a, const VjoVocab *b)
{
    if (strcmp(a->spelling, b->spelling) || strcmp(a->reading, b->reading) || a->rank != b->rank ||
        a->n_meanings != b->n_meanings || a->n_pos != b->n_pos || a->n_pitch != b->n_pitch ||
        strcmp(a->dictionary ? a->dictionary : "", b->dictionary ? b->dictionary : ""))
        return 0;
    for (int i = 0; i < a->n_meanings; i++)
        if (strcmp(a->meanings[i], b->meanings[i]))
            return 0;
    for (int i = 0; i < a->n_pos; i++)
        if (strcmp(a->pos[i], b->pos[i]))
            return 0;
    return 1;
}

/* One configuration over every line: the same sentence, cursor walk and entries. */
static void walk(const VjoConfig *cfg, int *lookups, int *entries, int *stacked)
{
    vjo_lookup_configure(cfg);
    shell_init();
    for (int i = 0; i < lines; i++) {
        VjoRowNav nav;
        size_t cursor, len;
        int set = vjo_lookup_set_text(line_text[i], 0);
        shell_sentence(cfg, line_text[i]);
        len = strlen(shell_text);
        TEST_CHECK_(!strcmp(vjo_lookup_text(), shell_text) && vjo_lookup_text_len() == len, "line %d: '%s' / '%s'", i,
                    vjo_lookup_text(), shell_text);
        if (set < 0) {
            TEST_CHECK(!len);
            continue;
        }
        memset(&nav, 0, sizeof(nav));
        /* the overlay's first selection (shell/worker.c) */
        cursor = cfg->japanese_only_selection ? vjo_utf8_japanese_cursor_step(shell_text, len, 0, 0) : 0;
        for (int guard = 0; cursor < len; guard++) {
            VjoOverlayData d;
            const VjoOverlayData *a = vjo_lookup_result();
            size_t matched = 0, next = cursor;
            int rc = shell_lookup(cfg, cursor, &d);
            TEST_ASSERT_(guard < 600, "line %d: the walk does not end", i);
            TEST_CHECK_(vjo_lookup_cursor() == cursor && vjo_lookup_fresh(), "line %d: the app's cursor %zu, the overlay's %zu",
                        i, vjo_lookup_cursor(), cursor);
            (*lookups)++;
            TEST_CHECK_(rc == VJO_OK && !vjo_lookup_error()[0] && a->list.n_entries == d.list.n_entries &&
                            a->list.n_more == d.list.n_more,
                        "line %d at %zu: rc %d, '%s', %d/%d entries, %d/%d stacked", i, cursor, rc, vjo_lookup_error(),
                        a->list.n_entries, d.list.n_entries, a->list.n_more, d.list.n_more);
            if (a->list.n_entries && d.list.n_entries) {
                const VjoEntry *x = &a->list.entries[0], *y = &d.list.entries[0];
                size_t hs, he;
                (*entries)++;
                TEST_CHECK_(same_vocab(x->vocab, y->vocab) && x->hl_start == y->hl_start && x->hl_end == y->hl_end &&
                                x->conjugation == y->conjugation && !strcmp(x->text, y->text) &&
                                !strcmp(a->list.source ? a->list.source : "", d.list.source ? d.list.source : ""),
                            "line %d at %zu: %s / %s", i, cursor, x->vocab->spelling, y->vocab->spelling);
                for (int k = 0; k < a->list.n_more && k < d.list.n_more; k++, (*stacked)++)
                    TEST_CHECK(same_vocab(a->list.more[k], d.list.more[k]));
                /* the highlight: the overlay's (overlay.cpp header_highlight) */
                vjo_lookup_highlight(&hs, &he);
                if (cfg->highlight_mode == VJO_HIGHLIGHT_WORD) {
                    TEST_CHECK(hs == (size_t)y->hl_start && he == (size_t)y->hl_end);
                } else { /* the cursor's character */
                    size_t e = cursor;
                    vjo_utf8_next(shell_text, len, &e);
                    TEST_CHECK(hs == cursor && he == e);
                }
                if (cfg->highlight_mode == VJO_HIGHLIGHT_WORD && y->hl_start == (int)cursor)
                    matched = (size_t)y->hl_end;
                /* the card made there: that entry, the line as its sentence, the line's game */
                if (*entries % 7 == 1) {
                    VjoLookupSource src;
                    memset(&src, 0, sizeof(src));
                    VjoAnkiNote note, want;
                    char misc[160];
                    snprintf(src.game, sizeof(src.game), "SOME GAME");
                    snprintf(src.title_id, sizeof(src.title_id), "PCSG00000");
                    snprintf(src.engine, sizeof(src.engine), "vocr");
                    TEST_ASSERT(vjo_lookup_note(&src, &note, misc, sizeof(misc)) == 0);
                    static uint8_t nm[64 * 1024];
                    VjoArena na;
                    vjo_arena_init(&na, nm, sizeof(nm));
                    TEST_ASSERT(vjo_anki_note_from_entry(&na, &d.list, 0, &want) == 0);
                    TEST_CHECK(!strcmp(note.spelling, want.spelling) && !strcmp(note.reading, want.reading) &&
                               !strcmp(note.sentence, want.sentence) && note.hl_start == want.hl_start &&
                               note.hl_end == want.hl_end && note.rank == want.rank &&
                               note.n_meanings == want.n_meanings && note.n_more <= d.list.n_more &&
                               note.n_more <= VJO_ANKI_MAX_MORE);
                    for (int k = 0; k < note.n_more; k++) {
                        TEST_CHECK(!strcmp(note.more[k].spelling, d.list.more[k]->spelling));
                        TEST_CHECK(!strcmp(note.more[k].reading, d.list.more[k]->reading));
                        TEST_CHECK(!strcmp(note.more[k].meanings[0], d.list.more[k]->meanings[0]));
                    }
                    TEST_CHECK(!strcmp(note.game, "SOME GAME") && !strcmp(note.platform, "vita") &&
                               !strcmp(note.ocr_engine, "vocr") && !strcmp(note.misc, "SOME GAME"));
                }
            }
            /* Right, as the overlay's d-pad moves it (overlay.cpp: vjo_text_row_key without the layout) */
            vjo_text_row_key(&nav, shell_text, len, NULL, cursor, matched, 1, 0, cfg->japanese_only_selection, 0, &next);
            int moved = vjo_lookup_step(1, 0);
            TEST_CHECK_(moved == (next != cursor) && vjo_lookup_cursor() == next, "line %d: Right from %zu: %zu / %zu", i,
                        cursor, vjo_lookup_cursor(), next);
            if (next <= cursor)
                break; /* the last word (a one-row text: Right goes round to its first) */
            cursor = next;
            vjo_lookup_at(cursor);
        }
    }
}

static const char *dir;

static void lookups_equal_the_overlays(void)
{
    int lookups = 0, entries = 0, stacked = 0;
    VjoConfig cfg;
    load();
    for (int variant = 0; variant < 3; variant++) {
        vjo_config_defaults(&cfg); /* the release defaults: word highlight, Japanese characters only OFF */
        snprintf(cfg.local_dictionary_dir, sizeof(cfg.local_dictionary_dir), "%s", dir);
        snprintf(cfg.local_dictionaries, sizeof(cfg.local_dictionaries), "plaintext.vjdict");

        if (variant == 1) { /* the other dictionaries' entries stacked (the same file twice: its word again) */
            snprintf(cfg.local_dictionaries, sizeof(cfg.local_dictionaries), "plaintext.vjdict,plaintext.vjdict");

        } else if (variant == 2) { /* Character highlight, Japanese characters only */
            cfg.highlight_mode = VJO_HIGHLIGHT_CHARACTER;
            cfg.japanese_only_selection = 1;
        }
        walk(&cfg, &lookups, &entries, &stacked);
    }
    vjo_lookup_close();
    vjo_local_session_close(shell_p.local_session);
    TEST_CHECK_(lookups > 800 && entries > 600 && stacked > 100, "%d lookups, %d entries, %d stacked", lookups, entries,
                stacked);
    /* (a line's Latin rows left out: the walk compared the texts; here, that it saw them) */
    VjoConfig check;
    vjo_config_defaults(&check);
    vjo_lookup_configure(&check);
    TEST_CHECK(vjo_lookup_set_text("Tomoya\n「猫が好きだ」", 0) == 0 && !strcmp(vjo_lookup_text(), "「猫が好きだ」"));
    TEST_CHECK(vjo_lookup_set_text("Tomoya\n「猫が好きだ」", 1) == 0 && !strcmp(vjo_lookup_text(), "Tomoya\n「猫が好きだ」"));
    TEST_MSG("%d lookups, %d entries, %d stacked", lookups, entries, stacked);
}

/* A typed text is the card's sentence only when it is longer than the word. */
static void a_typed_word_alone_has_no_sentence(void)
{
    VjoConfig cfg;
    VjoLookupSource typed;
    VjoAnkiNote note;
    char misc[160];
    memset(&typed, 0, sizeof(typed));
    typed.typed = 1;
    vjo_config_defaults(&cfg);
    snprintf(cfg.local_dictionary_dir, sizeof(cfg.local_dictionary_dir), "%s", dir);
    snprintf(cfg.local_dictionaries, sizeof(cfg.local_dictionaries), "plaintext.vjdict");
    vjo_lookup_configure(&cfg);
    TEST_ASSERT(vjo_lookup_set_text("  食べた  ", 1) == 0);
    TEST_ASSERT(vjo_lookup_result()->list.n_entries == 1);
    TEST_ASSERT(vjo_lookup_note(&typed, &note, misc, sizeof(misc)) == 0);
    TEST_CHECK(!strcmp(note.spelling, "食べる") && !strcmp(note.sentence, "") && note.hl_start == -1 && !note.game &&
               !note.title_id && !note.platform && !note.ocr_engine);
    TEST_CHECK(!strcmp(note.misc, "")); /* the clock unreadable here: no date rather than a wrong one */
    TEST_ASSERT(vjo_lookup_set_text("パンを食べた", 1) == 0);
    TEST_ASSERT(vjo_lookup_at(strlen("パンを")) == VJO_OK && vjo_lookup_result()->list.n_entries == 1);
    TEST_ASSERT(vjo_lookup_note(&typed, &note, misc, sizeof(misc)) == 0);
    TEST_CHECK(!strcmp(note.sentence, "パンを食べた") && note.hl_start == (int)strlen("パンを"));
    /* a typed text is not filtered: Latin stays, as typed */
    TEST_CHECK(vjo_lookup_set_text("abc", 1) == 0 && !strcmp(vjo_lookup_text(), "abc"));
    TEST_CHECK(vjo_lookup_set_text("   ", 1) == -1 && !vjo_lookup_text_len());
    vjo_lookup_close();
}

TEST_LIST = {
    {"the app's lookups are the overlay's, on the golden lines", lookups_equal_the_overlays},
    {"a typed word alone has no sentence", a_typed_word_alone_has_no_sentence},
    {NULL, NULL},
};

int main(int argc, char **argv)
{
    dir = getenv("VJO_PLAINTEXT_DIR");
    if (!dir || !*dir) {
        printf("VJO_PLAINTEXT_DIR is not set: no bundled dictionary, skipped\n");
        return getenv("VJO_REQUIRE_PLAINTEXT") ? 1 : 77;
    }
    return acutest_main(argc, argv);
}
