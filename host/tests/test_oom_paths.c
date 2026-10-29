/* Out-of-memory paths (#528, mem-crash; the design of the lookups' part is audit-mem-crash-1's): each arena an
 * operation of the Shell or the Rubi app takes is made to run out at every block it gives, one at a time (core/arena.c's
 * host-only vjo_test_arena_cap: the arena then holds only the bytes before that block's end, as a smaller one would),
 * and again at every block a cut run gives (until no new one). Every step of every run must fail with an error, give
 * the full run's result, or give it with only an omission it marks (a lookup's more_omitted, which the overlay shows
 * as "ask for a specific reading", keeping an ordered subset of the full step's other entries). None may crash: the
 * suite runs under the host build's checks, under ASan/UBSan when the build has them, and on the A9 (the ARM lane).
 *
 * Operations, on the real VOCR lines of host/fixtures/word_lookup in config.c's default dictionaries
 * (plaintext.vjdict and vndb-names.vjdict, those the directory holds):
 *   - the Shell's lookups in its 64 KiB result block after the dictionaries' session, in shell/worker.c's order:
 *     run_job's vjo_overlay_lookup_at at the cursor, prepare_words' vjo_local_prepare_at on the same arena (up to 8
 *     word steps, stopping at the first failure, as it does), then run_cursor_job at each of the line's first 6
 *     characters with every other dictionary's page and back (vjo_overlay_lookup_page);
 *   - the header and body the overlay draws in its 20 KiB text arena (core/styled.h);
 *   - an Anki card's record (vjo_anki_note_from_entry, vjo_queue_encode, vjo_queue_decode, vjo_lapis_fields) in the
 *     Shell's Anki arena; the Shell's own card path (shell/anki.c's save) is squeezed in test_shell_anki.c;
 *   - the Rubi app's sentence readings in their scratch (vjo_sentence_furigana, VJO_READINGS_SCRATCH);
 *   - the line history's scan of a history.csv with the current text's record, config.c's default filter
 *     (vjo_history_scan), which must keep the newest lines that fit, without a gap.
 *
 * VJO_PLAINTEXT_DIR names the dictionaries' directory. Without it, or without any of the dictionaries in it, the
 * dictionaries' operations are skipped (exit 77) unless VJO_REQUIRE_PLAINTEXT is set, which makes that, and a fixture
 * without a line, a failure. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define main acutest_main
#include "acutest.h"
#undef main
#include "anki.h"
#include "anki_queue.h"
#include "arena.h"
#include "client.h"
#include "config.h"
#include "history_lines.h"
#include "lapis.h"
#include "local_dict.h"
#include "platform_posix.h"
#include "sentence_reading.h"
#include "styled.h"
#include "text_rows.h"
#include "utf.h"

extern const void *vjo_test_arena_base;
extern size_t vjo_test_arena_cap, vjo_test_arena_ends[4096];
extern unsigned vjo_test_arena_n_ends;

/* ---- the squeeze ---- */

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    if (!h) h = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * UINT64_C(1099511628211);
    return h;
}
static uint64_t fstr(uint64_t h, const char *s) { return s ? fnv(h, s, strlen(s) + 1) : fnv(h, "\x01", 1); }
static uint64_t fint(uint64_t h, long v) { return fnv(h, &v, sizeof(v)); }

/* One run's results, a step to a slot (the same slot in the full run and in a cut one). A lookup's entries: the
 * header, its page, the entries' text and highlights; all: those and every other entry (more[]) whole. */
#define MAX_STEPS 80
#define MAX_MORE 160
enum { STEP_LIST, STEP_PREPARE, STEP_PAGE, STEP_PLAIN };
typedef struct {
    int used, kind, rc, omitted, n_more;
    uint64_t entries, all;
    uint64_t more[MAX_MORE];
} Step;
typedef struct {
    Step s[MAX_STEPS];
    int n;
} Run;
static Step *slot(Run *r, int k, int kind, int rc)
{
    Step *s;
    if (k >= MAX_STEPS) return NULL;
    s = &r->s[k];
    if (k >= r->n) r->n = k + 1;
    memset(s, 0, sizeof(*s));
    s->used = 1;
    s->kind = kind;
    s->rc = rc;
    return s;
}
/* An operation without steps of its own: one step, rc and what it made (h). */
static void add_plain(Run *r, int rc, uint64_t h)
{
    Step *s = slot(r, 0, STEP_PLAIN, rc);
    if (s && !rc) s->entries = s->all = h;
}

/* One run of an operation from the start, its arena at mem (size bytes), its steps into r. */
typedef void (*Op)(void *ud, uint8_t *mem, size_t size, Run *r);

typedef struct {
    unsigned runs, steps_failed, steps_same, steps_marked, steps_other, runs_other, new_cuts;
} Squeeze;
static int trace_runs; /* VJO_OOM_TRACE=1: each cut run that answers OK with another result, slot by slot */

static int cmp_size(const void *a, const void *b)
{
    size_t x = *(const size_t *)a, y = *(const size_t *)b;
    return x < y ? -1 : x > y;
}

/* A cut run's step against the full run's: refused, the same, or only marked omissions (the same entries, more_omitted
 * set, and its other entries in the full step's, in their order). */
static int judge(const Step *f, const Step *c, Squeeze *q)
{
    if (c->rc) { q->steps_failed++; return 1; }
    if (f->rc) { q->steps_other++; return 0; } /* the full run failed there, the cut one did not */
    if (c->all == f->all) { q->steps_same++; return 1; }
    if (c->kind != STEP_PREPARE && c->kind != STEP_PLAIN && c->omitted && c->entries == f->entries &&
        c->n_more <= f->n_more) {
        int j = 0;
        for (int i = 0; i < c->n_more && i < MAX_MORE; i++) {
            while (j < f->n_more && j < MAX_MORE && f->more[j] != c->more[i]) j++;
            if (j >= f->n_more || j >= MAX_MORE) { q->steps_other++; return 0; }
            j++;
        }
        q->steps_marked++;
        return 1;
    }
    q->steps_other++;
    return 0;
}

#define MAX_CUTS 16384
#define MAX_CUT_BYTES (256 * 1024)
/* Runs op with the whole arena, then with the arena cut just before each block end a run recorded: the full run's,
 * then each cut run's own, until no new one. The full run's steps must all succeed. */
static Squeeze squeeze(const char *what, Op op, void *ud, uint8_t *mem, size_t size)
{
    static size_t cuts[MAX_CUTS];
    static unsigned char seen[MAX_CUT_BYTES / 8];
    static Run full, cut;
    Squeeze q = {0, 0, 0, 0, 0, 0, 0};
    unsigned n = 0, head = 0;
    TEST_ASSERT(size <= MAX_CUT_BYTES);
    vjo_test_arena_base = mem;
    vjo_test_arena_cap = (size_t)-1;
    vjo_test_arena_n_ends = 0;
    memset(&full, 0, sizeof(full));
    op(ud, mem, size, &full);
    for (int k = 0; k < full.n; k++)
        TEST_CHECK_(!full.s[k].used || !full.s[k].rc, "%s: the full run's step %d succeeds (rc %d)", what, k, full.s[k].rc);
    TEST_CHECK_(vjo_test_arena_n_ends < 4096, "%s: %u block ends (the record holds 4,096)", what, vjo_test_arena_n_ends);
    memset(seen, 0, sizeof(seen));
    for (unsigned i = 0; i < vjo_test_arena_n_ends; i++) {
        size_t c = vjo_test_arena_ends[i] - 1;
        if (c < MAX_CUT_BYTES && !(seen[c >> 3] & (1u << (c & 7))) && n < MAX_CUTS) {
            seen[c >> 3] |= (unsigned char)(1u << (c & 7));
            cuts[n++] = c;
        }
    }
    qsort(cuts, n, sizeof(cuts[0]), cmp_size);
    while (head < n) {
        size_t c = cuts[head++];
        int other = 0;
        memset(&cut, 0, sizeof(cut));
        vjo_test_arena_cap = c;
        vjo_test_arena_n_ends = 0;
        op(ud, mem, size, &cut);
        q.runs++;
        for (int k = 0; k < cut.n; k++) {
            if (!cut.s[k].used) continue;
            if (!full.s[k].used || cut.s[k].kind != full.s[k].kind) {
                if (cut.s[k].rc) q.steps_failed++; /* a step the full run did not take, refused */
                else { q.steps_other++; other = 1; }
                continue;
            }
            if (!judge(&full.s[k], &cut.s[k], &q)) {
                other = 1;
                if (trace_runs)
                    printf("  %s, cut at %zu: step %d (kind %d): rc %d of %d, entries %s, other entries %d of %d, "
                           "omitted %d of %d\n", what, c, k, cut.s[k].kind, cut.s[k].rc, full.s[k].rc,
                           cut.s[k].entries == full.s[k].entries ? "the same" : "DIFFER", cut.s[k].n_more,
                           full.s[k].n_more, cut.s[k].omitted, full.s[k].omitted);
            }
        }
        if (other) {
            q.runs_other++;
            TEST_CHECK_(0, "%s: arena cut at %zu answered OK with another result", what, c);
        }
        for (unsigned i = 0; i < vjo_test_arena_n_ends; i++) { /* the cut run's own blocks: more cuts */
            size_t e = vjo_test_arena_ends[i] - 1;
            if (e < MAX_CUT_BYTES && !(seen[e >> 3] & (1u << (e & 7))) && n < MAX_CUTS) {
                seen[e >> 3] |= (unsigned char)(1u << (e & 7));
                cuts[n++] = e;
                q.new_cuts++;
            }
        }
    }
    vjo_test_arena_base = NULL;
    vjo_test_arena_cap = (size_t)-1;
    return q;
}

/* ---- the dictionaries and the real VOCR lines ---- */

static const char *dict_dir;
static char dicts[128];   /* config.c's default local_dictionaries, those dict_dir holds */
static char lines[64][1024];
static int n_lines, lines_loaded;

static void load_lines(void)
{
    char path[1024], row[8192];
    FILE *f;
    if (lines_loaded++) return; /* once: every test reads the same lines */
    snprintf(path, sizeof(path), "%s/vocr_lines.tsv", VJO_WORD_FIXTURES);
    if (!(f = fopen(path, "rb"))) return;
    while (n_lines < (int)(sizeof(lines) / sizeof(lines[0])) && fgets(row, sizeof(row), f)) {
        char *col = row, *out = lines[n_lines];
        size_t k = 0;
        if (row[0] == '#' || row[0] == '\n') continue;
        for (int c = 0; c < 5 && col; c++) col = strchr(col, '\t') ? strchr(col, '\t') + 1 : NULL;
        if (!col) continue;
        for (size_t i = 0; col[i] && col[i] != '\n' && col[i] != '\r' && k + 1 < sizeof(lines[0]); i++) {
            if (col[i] == '\\' && col[i + 1] == 'n') { out[k++] = '\n'; i++; }
            else out[k++] = col[i];
        }
        out[k] = '\0';
        n_lines++;
    }
    fclose(f);
}

/* Which of config.c's default dictionaries dict_dir holds (into dicts), and whether plaintext.vjdict is one; the
 * lines loaded. 1 when the dictionaries' tests can run. */
static int find_dictionaries(int *plaintext)
{
    VjoConfig d;
    char names[sizeof(d.local_dictionaries)], *name, *save = NULL;
    dicts[0] = '\0';
    *plaintext = 0;
    vjo_config_defaults(&d);
    snprintf(names, sizeof(names), "%s", d.local_dictionaries);
    if ((dict_dir = getenv("VJO_PLAINTEXT_DIR")))
        for (name = strtok_r(names, ",", &save); name; name = strtok_r(NULL, ",", &save)) {
            char path[1024];
            FILE *f;
            snprintf(path, sizeof(path), "%s/%s", dict_dir, name);
            if (!(f = fopen(path, "rb"))) continue;
            fclose(f);
            *plaintext |= !strcmp(name, "plaintext.vjdict");
            snprintf(dicts + strlen(dicts), sizeof(dicts) - strlen(dicts), "%s%s", dicts[0] ? "," : "", name);
        }
    load_lines();
    return dicts[0] && n_lines > 0;
}

/* In a test: the dictionaries (main runs the dictionaries' tests only when find_dictionaries found them). */
static int have_dictionaries(void)
{
    int plaintext;
    return find_dictionaries(&plaintext);
}

/* The Shell's result block (shell/worker.c mem_alloc: 64 KiB, LOCAL_RESULT_ARENA_SIZE): the dictionaries' session
 * first, the lookups' arena after it. */
static uint8_t result_mem[64 * 1024] __attribute__((aligned(16)));
static size_t session_bytes;
static VjoConfig cfg;
static VjoPlatform plat;
static PosixPlatform pp;
static uint32_t generation;
static void setup(void)
{
    vjo_config_defaults(&cfg);
    snprintf(cfg.local_dictionary_dir, sizeof(cfg.local_dictionary_dir), "%s", dict_dir);
    snprintf(cfg.local_dictionaries, sizeof(cfg.local_dictionaries), "%s", dicts);
    posix_platform_init(&pp, &plat);
    session_bytes = (vjo_local_session_size() + 7u) & ~(size_t)7u;
}
static void session_fresh(void)
{
    if (plat.local_session) vjo_local_session_close(plat.local_session);
    memset(result_mem, 0, session_bytes);
    plat.local_session = (VjoLocalSession *)result_mem;
}
static void session_end(void)
{
    if (plat.local_session) vjo_local_session_close(plat.local_session);
    plat.local_session = NULL;
}

static uint64_t vocab_hash(const VjoVocab *v)
{
    uint64_t h = fstr(0, v->spelling);
    h = fstr(h, v->reading);
    h = fstr(h, v->dictionary);
    h = fint(h, v->n_meanings);
    for (int i = 0; i < v->n_meanings; i++) h = fstr(h, v->meanings[i]);
    return h;
}
/* A lookup's step: its entries, and every other entry whole. */
static void add_list(Run *r, int k, int kind, int rc, const VjoOverlayData *o)
{
    const VjoEntryList *l = &o->list;
    Step *s = slot(r, k, kind, rc);
    if (!s || rc) return;
    s->entries = fstr(0, l->header);
    s->entries = fint(s->entries, vjo_overlay_page_count(o));
    s->entries = fint(s->entries, o->page);
    s->entries = fint(s->entries, l->n_entries);
    for (int i = 0; i < l->n_entries; i++) {
        s->entries = fstr(s->entries, l->entries[i].text);
        s->entries = fint(s->entries, l->entries[i].hl_start);
        s->entries = fint(s->entries, l->entries[i].hl_end);
    }
    s->omitted = l->more_omitted;
    s->n_more = l->n_more;
    s->all = fint(s->entries, l->n_more);
    for (int i = 0; i < l->n_more && i < MAX_MORE; i++) {
        s->more[i] = l->more[i] ? vocab_hash(l->more[i]) : 1;
        s->all = fnv(s->all, &s->more[i], sizeof(s->more[i]));
    }
    s->all = fint(s->all, l->more_omitted);
}
static void add_prepare(Run *r, int k, int rc, size_t end)
{
    Step *s = slot(r, k, STEP_PREPARE, rc);
    if (s && !rc) s->entries = s->all = fint(0, (long)end);
}
static void shown_text(VjoOverlayData *o, const char *text)
{
    memset(o, 0, sizeof(*o));
    o->ocr_text = o->filtered = o->sentence = text;
    o->list.header = text;
}

/* A line's lookups in shell/worker.c's order (see the top), the session fresh as after mem_alloc. Slots: 0 run_job's
 * lookup, 1-8 the preparations, 10 + 10k cursor job k, 11 + 10k + j its j-th page turn. */
static void op_lookup(void *ud, uint8_t *mem, size_t size, Run *r)
{
    const char *text = (const char *)ud;
    size_t len = strlen(text), cur = 0, end = 0;
    VjoArena a;
    VjoOverlayData o;
    int rc;
    session_fresh();
    vjo_arena_init(&a, mem, size);
    plat.local_text_generation = ++generation;
    /* run_job: the cursor's lookup after OCR (byte 0: japanese_only_selection is off by default) */
    shown_text(&o, text);
    rc = vjo_overlay_lookup_at(&a, &plat, &cfg, 0, &o);
    add_list(r, 0, STEP_LIST, rc, &o);
    /* prepare_words: on the same arena (the shown result stays), until one fails */
    for (int k = 0; k < 8 && cur < len; k++) {
        VjoWordNavigation nav;
        size_t next;
        int row;
        rc = vjo_local_prepare_at(&a, &plat, &cfg, text, cur, &end);
        add_prepare(r, 1 + k, rc, end);
        if (rc) break;
        memset(&nav, 0, sizeof(nav));
        next = vjo_word_step(&nav, text, len, cur, end, 1, cfg.japanese_only_selection, NULL, 0, &row);
        if (next <= cur) break;
        cur = next;
    }
    /* run_cursor_job at the first 6 characters, and its page branch: every other page, then page 0 again */
    for (size_t at = 0, k = 0; at < len && k < 6; k++) {
        size_t next = at;
        vjo_arena_reset(&a);
        shown_text(&o, text);
        rc = vjo_overlay_lookup_at(&a, &plat, &cfg, at, &o);
        add_list(r, 10 + 10 * (int)k, STEP_LIST, rc, &o);
        if (!rc && o.pages && o.pages->count > 1)
            for (int pg = 1; pg <= o.pages->count && pg < 9; pg++) {
                rc = vjo_overlay_lookup_page(&a, &plat, &cfg, pg % o.pages->count, &o);
                add_list(r, 10 + 10 * (int)k + pg, STEP_PAGE, rc, &o);
                if (rc) break;
            }
        (void)vjo_utf8_next(text, len, &next);
        if (next <= at) break;
        at = next;
    }
}

/* The overlay's texts of a lookup's result: the header and the body, in its UI arena's units and spans
 * (shell/overlay.cpp: VJO_BODY_UNITS, VJO_BODY_SPANS). Arena: the UI's. */
static VjoOverlayData shown;
static void op_styled(void *ud, uint8_t *mem, size_t size, Run *r)
{
    VjoArena a;
    VjoStyled h, b;
    uint64_t d;
    (void)ud;
    vjo_arena_init(&a, mem, size);
    if (vjo_styled_init(&h, &a, 1024, VJO_HEADER_SPANS) < 0 || vjo_styled_init(&b, &a, VJO_BODY_UNITS, VJO_BODY_SPANS) < 0) {
        add_plain(r, VJO_E_OOM, 0);
        return;
    }
    vjo_styled_header(&h, &shown.list, 18);
    vjo_styled_entry(&b, &shown.list, 0, 18, 15, 0, 1);
    d = fnv(0, h.text, h.len * 2);
    d = fnv(d, b.text, b.len * 2);
    d = fint(fint(d, h.n_spans), b.n_spans);
    add_plain(r, VJO_OK, d);
}

/* An Anki card's record from the shown entry (the core's calls shell/anki.c makes: its save itself is squeezed in
 * test_shell_anki.c), read back and made the package's Lapis fields, as the Rubi app's export does. */
static uint8_t anki_mem[128 * 1024] __attribute__((aligned(16)));
static void op_anki(void *ud, uint8_t *mem, size_t size, Run *r)
{
    VjoArena a;
    VjoAnkiNote note, back;
    char *json, *fields;
    size_t len = 0;
    int picture = 0, rc;
    uint64_t d;
    (void)ud;
    vjo_arena_init(&a, mem, size);
    memset(&note, 0, sizeof(note));
    if ((rc = vjo_anki_note_from_entry(&a, &shown.list, 0, &note)) != 0) { add_plain(r, rc < 0 ? rc : VJO_E_OOM, 0); return; }
    if (!(json = vjo_queue_encode(&a, &note, 0))) { add_plain(r, VJO_E_OOM, 0); return; }
    d = fstr(0, json);
    memset(&back, 0, sizeof(back));
    if (vjo_queue_decode(&a, json, strlen(json), &back, &picture) < 0) { add_plain(r, VJO_E_OOM, 0); return; }
    if (!(fields = vjo_lapis_fields(&a, &back, NULL, NULL, &len))) { add_plain(r, VJO_E_OOM, 0); return; }
    add_plain(r, VJO_OK, fint(fnv(d, fields, len), picture));
}

/* The export's sentence readings of a card (settings/anki_export.c: its readings' scratch, VJO_READINGS_SCRATCH;
 * the furigana in the package's arena). Arena: the scratch. */
static uint8_t readings_mem[VJO_READINGS_SCRATCH] __attribute__((aligned(16)));
static uint8_t keep_mem[64 * 1024] __attribute__((aligned(16)));
static VjoAnkiNote card;
static void op_readings(void *ud, uint8_t *mem, size_t size, Run *r)
{
    VjoArena keep, scratch;
    VjoReadingRules rules;
    VjoReadingStats st;
    char *html;
    int rc = 0;
    (void)ud;
    vjo_arena_init(&keep, keep_mem, sizeof(keep_mem));
    vjo_arena_init(&scratch, mem, size);
    vjo_reading_rules_default(&rules);
    memset(&st, 0, sizeof(st));
    if (!(html = vjo_sentence_furigana(&keep, &scratch, &plat, &cfg, &rules, &card, &st, &rc)))
        add_plain(r, rc ? rc : VJO_E_OOM, 0);
    else
        add_plain(r, VJO_OK, fstr(0, html));
}

static void total(Squeeze *t, const Squeeze *q)
{
    t->runs += q->runs; t->steps_failed += q->steps_failed; t->steps_same += q->steps_same;
    t->steps_marked += q->steps_marked; t->steps_other += q->steps_other; t->runs_other += q->runs_other;
    t->new_cuts += q->new_cuts;
}

static void lookups(void)
{
    Squeeze all = {0, 0, 0, 0, 0, 0, 0};
    unsigned lines_done = 0;
    TEST_ASSERT(have_dictionaries());
    setup();
    for (int i = 0; i < n_lines; i++) {
        static uint8_t scratch[32 * 1024];
        static char sentence[4096];
        VjoArena a;
        VjoOverlayData o;
        Squeeze q;
        char what[64];
        vjo_arena_init(&a, scratch, sizeof(scratch));
        memset(&o, 0, sizeof(o));
        if (vjo_overlay_ocr_text(&a, &cfg, lines[i], &o) != VJO_OK || !o.sentence || !*o.sentence) continue;
        snprintf(sentence, sizeof(sentence), "%s", o.sentence);
        snprintf(what, sizeof(what), "lookups of line %d", i);
        q = squeeze(what, op_lookup, sentence, result_mem + session_bytes, sizeof(result_mem) - session_bytes);
        total(&all, &q);
        lines_done++;
    }
    session_end();
    printf("\n  lookups (%s): %u lines, %u cut arenas (%u from the cut runs' own blocks): steps %u refused, %u the "
           "full step's, %u with marked omissions, %u other\n", dicts, lines_done, all.runs, all.new_cuts,
           all.steps_failed, all.steps_same, all.steps_marked, all.steps_other);
}

static void cards(void)
{
    Squeeze t[3] = {{0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0}};
    unsigned lines_done = 0;
    TEST_ASSERT(have_dictionaries());
    setup();
    for (int i = 0; i < n_lines; i++) {
        static uint8_t lookup_mem[256 * 1024] __attribute__((aligned(16))); /* a whole line's lookup: room for every line */
        static uint8_t card_mem[32 * 1024] __attribute__((aligned(16)));   /* the card's strings, apart from the cut arenas */
        VjoArena a, c;
        Squeeze q;
        char what[64];
        int rc;
        vjo_arena_init(&a, lookup_mem, sizeof(lookup_mem));
        memset(&shown, 0, sizeof(shown));
        rc = vjo_overlay_from_text(&a, &plat, &cfg, lines[i], &shown);
        TEST_CHECK_(rc == VJO_OK, "line %d's lookup for the card (rc %d, %zu of %zu B)", i, rc, a.peak, a.size);
        if (rc != VJO_OK || shown.list.n_entries < 1) continue; /* (a line with no word: nothing to show) */
        lines_done++;
        snprintf(what, sizeof(what), "overlay texts of line %d", i);
        q = squeeze(what, op_styled, NULL, anki_mem, 20 * 1024);
        total(&t[0], &q);
        snprintf(what, sizeof(what), "Anki card of line %d", i);
        q = squeeze(what, op_anki, NULL, anki_mem, sizeof(anki_mem));
        total(&t[1], &q);
        vjo_arena_init(&c, card_mem, sizeof(card_mem));
        memset(&card, 0, sizeof(card));
        if (vjo_anki_note_from_entry(&c, &shown.list, 0, &card) != 0) continue;
        snprintf(what, sizeof(what), "sentence readings of line %d", i);
        q = squeeze(what, op_readings, NULL, readings_mem, sizeof(readings_mem));
        total(&t[2], &q);
    }
    session_end();
    printf("\n  %u lines: overlay texts %u cut arenas (%u refused), Anki cards %u (%u), sentence readings %u (%u)\n",
           lines_done, t[0].runs, t[0].steps_failed, t[1].runs, t[1].steps_failed, t[2].runs, t[2].steps_failed);
}

/* ---- the line history (shell/worker.c history_open: its PAF block's arena) ---- */

static char history_csv[48 * 1024];
static size_t history_len;
static const char uuid[] = "vita-PCSG00415\",";
static const char current[] = "ここのジュース、美味しいって";
/* 160 records, every fifth another game's, every seventh's line not Japanese (the default filter drops it), the
 * newest of this game's the current text's own record (the usual case: the visit opens from it) */
static void make_history(void)
{
    size_t n = 0;
    int current_at = 159;
    while (current_at % 5 == 0 || current_at % 7 == 3) current_at--;
    n += (size_t)snprintf(history_csv + n, sizeof(history_csv) - n, "uuid,given_identifier,name,line,time\n");
    for (int i = 0; i < 160 && n + 512 < sizeof(history_csv); i++) {
        char line[256];
        if (i == current_at) snprintf(line, sizeof(line), "%s", current);
        else if (i % 7 == 3) snprintf(line, sizeof(line), "Choose an option %d", i);
        else if (i % 3) snprintf(line, sizeof(line), "「あ、\"\"そうなんだ\"\"」%d", i);
        else snprintf(line, sizeof(line), "  ここのジュース、美味しいって\nAuto-save %d\nクラスで評判でさ%d  ", i, i);
        n += (size_t)snprintf(history_csv + n, sizeof(history_csv) - n,
                              "\"vita-%s\",\"vjo-%d\",\"g\",\"%s\",%d.162344,vita,%s,vocr,m\n",
                              i % 5 ? "PCSG00415" : "PCSG00402", i, line, 1792235952 + i,
                              i % 5 ? "PCSG00415" : "PCSG00402");
    }
    history_len = n;
}

static VjoHistory hist;
static int op_history(uint8_t *mem, size_t size)
{
    static char buf[sizeof(history_csv)];
    VjoConfig d;
    VjoArena a;
    vjo_config_defaults(&d);
    memcpy(buf, history_csv, history_len); /* the scan unescapes in place */
    memset(&hist, 0, sizeof(hist));
    vjo_arena_init(&a, mem, size);
    if (!(hist.line[0] = vjo_arena_strndup(&a, current, strlen(current)))) return VJO_E_OOM;
    vjo_history_scan(&hist, &a, buf, history_len, 1, uuid, d.non_japanese_filter);
    return VJO_OK;
}

/* A cut arena keeps the newest lines that fit (history_lines.h: "the lines kept are always the newest, without a
 * gap"): every run returns, and its lines are the full run's first ones. A record newer than the current text's
 * (a refresh the visit's opening dropped) is left out here: under a cut the scan can stop before it reaches the current
 * text's record and so keep it (audit-mem-crash-1 finding 7), which the Shell's 16 KiB block cannot reach (its peak:
 * the current text, that line and keep()'s copy, at most 3 x 4,095 B). */
static void history(void)
{
    static uint8_t mem[16 * 1024] __attribute__((aligned(16)));
    static char full[VJO_HISTORY_LINES + 2][512];
    static size_t ends[4096];
    unsigned n, runs = 0, fewer = 0;
    int full_n;
    make_history();
    vjo_test_arena_base = mem;
    vjo_test_arena_cap = (size_t)-1;
    vjo_test_arena_n_ends = 0;
    TEST_ASSERT(op_history(mem, sizeof(mem)) == VJO_OK);
    full_n = hist.n;
    TEST_CHECK_(full_n >= 8, "the full scan keeps %d earlier lines", full_n);
    for (int i = 0; i <= full_n; i++) snprintf(full[i], sizeof(full[i]), "%s", hist.line[i]);
    n = vjo_test_arena_n_ends;
    memcpy(ends, vjo_test_arena_ends, n * sizeof(ends[0]));
    qsort(ends, n, sizeof(ends[0]), cmp_size);
    for (unsigned i = 0; i < n; i++) {
        if (i && ends[i] == ends[i - 1]) continue;
        vjo_test_arena_cap = ends[i] - 1;
        runs++;
        if (op_history(mem, sizeof(mem)) != VJO_OK) continue;
        TEST_CHECK_(hist.n <= full_n, "cut at %zu: %d lines of %d", ends[i] - 1, hist.n, full_n);
        fewer += hist.n < full_n;
        for (int k = 1; k <= hist.n && k <= full_n; k++)
            TEST_CHECK_(hist.line[k] && !strcmp(hist.line[k], full[k]), "cut at %zu: line %d is the full scan's",
                        ends[i] - 1, k);
        for (int k = hist.n + 1; k < VJO_HISTORY_LINES + 2; k++)
            TEST_CHECK_(!hist.line[k], "cut at %zu: no line past the kept ones (%d)", ends[i] - 1, k);
    }
    printf("\n  history: %d earlier lines; %u cut arenas, %u kept fewer, each the newest without a gap\n", full_n, runs,
           fewer);
    vjo_test_arena_base = NULL;
    vjo_test_arena_cap = (size_t)-1;
}

TEST_LIST = {
    {"lookups in the Shell's order: every block of the result arena cut", lookups},
    {"overlay texts, Anki cards and sentence readings with cut arenas", cards},
    {"line history with a cut arena", history},
    {NULL, NULL},
};

/* Without a default dictionary only the history runs, and the suite exits 77 (ctest: skipped, SKIP_RETURN_CODE), not 0;
 * with VJO_REQUIRE_PLAINTEXT that, plaintext.vjdict missing or a fixture without a line, fails. */
int main(int argc, char **argv)
{
    int plaintext, found = find_dictionaries(&plaintext);
    if (getenv("VJO_REQUIRE_PLAINTEXT") && (!plaintext || !n_lines)) {
        fprintf(stderr, "VJO_REQUIRE_PLAINTEXT: %s\n", !plaintext ? "plaintext.vjdict missing" : "no line in the fixture");
        return 1;
    }
    if (!found && argc == 1) {
        char *only[] = {argv[0], (char *)"line history with a cut arena", NULL};
        int rc = acutest_main(2, only);
        printf("SKIP: no default dictionary in VJO_PLAINTEXT_DIR (%s): lookups, cards and readings not run\n",
               dict_dir ? dict_dir : "unset");
        return rc ? rc : 77;
    }
    return acutest_main(argc, argv);
}
