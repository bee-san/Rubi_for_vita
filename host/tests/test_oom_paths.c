/* Out-of-memory paths (#528, mem-crash): each arena an operation of the Shell or the Rubi app takes is made to run
 * out at every block it gives, one at a time (core/arena.c's host-only vjo_test_arena_cap: the arena then holds only
 * the bytes before that block's end, as a smaller one would). Every run must return, with an error or with its full
 * result; none may crash (the suite runs under the host build's checks, and under ASan/UBSan when the build has
 * them) or answer OK with a result the full arena would not have given, but for an omission the result marks (a
 * lookup's more_omitted, which the overlay shows as "ask for a specific reading").
 *
 * Operations, on the real VOCR lines of host/fixtures/word_lookup in the bundled dictionary: the Shell's lookups in
 * its 64 KiB result block after the dictionaries' session (vjo_local_prepare_at, vjo_overlay_lookup_at at each of a
 * line's first characters, vjo_overlay_lookup_page), the header and body the overlay draws in its 20 KiB text arena
 * (core/styled.h), an Anki card's record in the Shell's 128 KiB Anki arena (vjo_anki_note_from_entry,
 * vjo_queue_encode, then vjo_queue_decode and vjo_lapis_fields as the Rubi app reads it), the app's sentence
 * readings in their scratch (vjo_sentence_furigana, VJO_READINGS_SCRATCH); and the line history's scan of a
 * history.csv (vjo_history_scan), which must keep the newest lines that fit.
 *
 * VJO_PLAINTEXT_DIR names the directory holding plaintext.vjdict (the dictionary's operations are skipped without
 * it, as the word lookup suite's are, unless VJO_REQUIRE_PLAINTEXT is set). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "acutest.h"
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
#include "utf.h"

extern const void *vjo_test_arena_base;
extern size_t vjo_test_arena_cap, vjo_test_arena_ends[4096];
extern unsigned vjo_test_arena_n_ends;

/* ---- the squeeze ---- */

/* What a run made: h all of it; core the part no degradation may change (a lookup's entries); degraded: the run
 * says it left something out (a lookup's more_omitted: "ask for a specific reading"). */
typedef struct {
    uint64_t h, core;
    int degraded;
} Digest;
static void fnv(uint64_t *h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    if (!*h) *h = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < n; i++) *h = (*h ^ b[i]) * UINT64_C(1099511628211);
}
static void dig(Digest *d, const void *p, size_t n)
{
    fnv(&d->h, p, n);
    fnv(&d->core, p, n);
}
static void dig_str(Digest *d, const char *s)
{
    static const char none[] = "\x01(null)";
    if (s) dig(d, s, strlen(s) + 1);
    else dig(d, none, sizeof(none));
}
static void dig_int(Digest *d, long v) { dig(d, &v, sizeof(v)); }

/* One run of an operation from the start, its arena at mem (size bytes); its rc, and what it made into d. */
typedef int (*Op)(void *ud, uint8_t *mem, size_t size, Digest *d);

typedef struct {
    unsigned runs, failed, ok_same, ok_degraded, ok_differs;
} Squeeze;
static int trace_lists; /* VJO_OOM_TRACE: what the full run and a cut one made, where they differ */

static int cmp_size(const void *a, const void *b)
{
    size_t x = *(const size_t *)a, y = *(const size_t *)b;
    return x < y ? -1 : x > y;
}

/* Runs op with the whole arena, then once for each block end the full run recorded, the arena cut just before
 * it. The full run must succeed; every cut run must return an error, the full run's result, or that result with
 * only a marked omission (Digest.degraded, the core unchanged). VJO_OOM_TRACE=1 prints both runs of a mismatch. */
static Squeeze squeeze(const char *what, Op op, void *ud, uint8_t *mem, size_t size)
{
    static size_t ends[4096];
    Squeeze q = {0, 0, 0, 0, 0};
    Digest full = {0};
    unsigned n = 0;
    int rc;
    vjo_test_arena_base = mem;
    vjo_test_arena_cap = (size_t)-1;
    vjo_test_arena_n_ends = 0;
    rc = op(ud, mem, size, &full);
    TEST_CHECK_(rc == VJO_OK, "%s: the full run succeeds (rc %d)", what, rc);
    memcpy(ends, vjo_test_arena_ends, vjo_test_arena_n_ends * sizeof(ends[0]));
    qsort(ends, vjo_test_arena_n_ends, sizeof(ends[0]), cmp_size);
    for (unsigned i = 0; i < vjo_test_arena_n_ends; i++)
        if (!n || ends[n - 1] != ends[i]) ends[n++] = ends[i];
    for (unsigned i = 0; i < n; i++) {
        Digest d = {0};
        vjo_test_arena_cap = ends[i] - 1;
        rc = op(ud, mem, size, &d);
        q.runs++;
        if (rc != VJO_OK) q.failed++;
        else if (d.h == full.h) q.ok_same++;
        else if (d.degraded && d.core == full.core) q.ok_degraded++;
        else {
            if (getenv("VJO_OOM_TRACE")) {
                Digest x = {0};
                trace_lists = 1;
                printf("  %s: full run\n", what);
                vjo_test_arena_cap = (size_t)-1;
                op(ud, mem, size, &x);
                printf("  %s: cut at %zu\n", what, ends[i] - 1);
                vjo_test_arena_cap = ends[i] - 1;
                op(ud, mem, size, &x);
                trace_lists = 0;
            }
            q.ok_differs++;
            TEST_CHECK_(0, "%s: arena cut at %zu of %zu B answered OK with another result", what, ends[i] - 1,
                        ends[n - 1]);
        }
    }
    vjo_test_arena_base = NULL;
    vjo_test_arena_cap = (size_t)-1;
    return q;
}

/* ---- the bundled dictionary and real VOCR lines ---- */

static const char *dict_dir;
static char lines[64][1024];
static int n_lines;

static int load_lines(void)
{
    char path[1024], row[8192];
    FILE *f;
    snprintf(path, sizeof(path), "%s/vocr_lines.tsv", VJO_WORD_FIXTURES);
    if (!(f = fopen(path, "rb"))) return 0;
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
    return n_lines;
}

static int have_dictionary(void)
{
    char path[1024];
    FILE *f;
    if (!dict_dir && !(dict_dir = getenv("VJO_PLAINTEXT_DIR"))) goto none;
    snprintf(path, sizeof(path), "%s/plaintext.vjdict", dict_dir);
    if (!(f = fopen(path, "rb"))) goto none;
    fclose(f);
    return load_lines() > 0;
none:
    if (getenv("VJO_REQUIRE_PLAINTEXT")) {
        fprintf(stderr, "plaintext.vjdict required but missing\n");
        exit(1);
    }
    return 0;
}

/* The Shell's result block (shell/worker.c mem_alloc: 64 KiB, LOCAL_RESULT_ARENA_SIZE): the dictionaries' session
 * first, the lookups' arena after it. */
static uint8_t result_mem[64 * 1024];
static size_t session_bytes;
static VjoConfig cfg;
static VjoPlatform plat;
static PosixPlatform pp;
static void setup(void)
{
    vjo_config_defaults(&cfg);
    snprintf(cfg.local_dictionary_dir, sizeof(cfg.local_dictionary_dir), "%s", dict_dir);
    snprintf(cfg.local_dictionaries, sizeof(cfg.local_dictionaries), "plaintext.vjdict");
    posix_platform_init(&pp, &plat);
    session_bytes = (vjo_local_session_size() + 7u) & ~(size_t)7u;
}

static void dig_list(Digest *d, const VjoEntryList *l)
{
    if (trace_lists)
        printf("    list: %d entries, %d more, omitted %d, first %.60s\n", l->n_entries, l->n_more, l->more_omitted,
               l->n_entries ? l->entries[0].text : "-");
    dig_str(d, l->header);
    dig_int(d, l->n_entries);
    for (int i = 0; i < l->n_entries; i++) {
        dig_str(d, l->entries[i].text);
        dig_int(d, l->entries[i].hl_start);
        dig_int(d, l->entries[i].hl_end);
    }
    fnv(&d->h, &l->n_more, sizeof(l->n_more));
    for (int i = 0; i < l->n_more; i++)
        if (l->more[i]) {
            fnv(&d->h, l->more[i]->spelling, strlen(l->more[i]->spelling));
            fnv(&d->h, &l->more[i]->n_meanings, sizeof(l->more[i]->n_meanings));
        }
    fnv(&d->h, &l->more_omitted, sizeof(l->more_omitted));
    if (l->more_omitted) d->degraded = 1;
}

static void session_fresh(void)
{
    if (plat.local_session) vjo_local_session_close(plat.local_session);
    memset(result_mem, 0, session_bytes);
    plat.local_session = (VjoLocalSession *)result_mem;
}

/* A line's lookups as the Shell runs them after OCR (shell/worker.c run_job, run_cursor_job, net_job): the word
 * prepared at the line's start, then the cursor at each of its first characters, and the next dictionary's page
 * when there is one. The session starts empty at each run, as after mem_alloc. */
static int op_lookup(void *ud, uint8_t *mem, size_t size, Digest *d)
{
    const char *text = (const char *)ud;
    size_t len = strlen(text), end = 0;
    VjoArena a;
    VjoOverlayData o;
    int rc;
    session_fresh();
    vjo_arena_init(&a, mem, size);
    if ((rc = vjo_local_prepare_at(&a, &plat, &cfg, text, 0, &end)) < 0) return rc;
    dig_int(d, rc);
    dig_int(d, (long)end);
    for (size_t at = 0, k = 0; at < len && k < 6; k++) {
        size_t next = at;
        vjo_arena_reset(&a);
        memset(&o, 0, sizeof(o));
        o.ocr_text = o.filtered = o.sentence = text;
        o.list.header = text;
        if ((rc = vjo_overlay_lookup_at(&a, &plat, &cfg, at, &o)) != VJO_OK) return rc;
        dig_list(d, &o.list);
        if (vjo_overlay_page_count(&o) > 1) {
            if ((rc = vjo_overlay_lookup_page(&a, &plat, &cfg, 1, &o)) != VJO_OK) return rc;
            dig_list(d, &o.list);
        }
        (void)vjo_utf8_next(text, len, &next);
        if (next <= at) break;
        at = next;
    }
    return VJO_OK;
}

/* The overlay's texts of a lookup's result: the header and the body, in its UI arena's units and spans
 * (shell/overlay.cpp: VJO_BODY_UNITS, VJO_BODY_SPANS). Arena: the UI's. */
static VjoOverlayData shown;
static int op_styled(void *ud, uint8_t *mem, size_t size, Digest *d)
{
    VjoArena a;
    VjoStyled h, b;
    (void)ud;
    vjo_arena_init(&a, mem, size);
    if (vjo_styled_init(&h, &a, 1024, VJO_HEADER_SPANS) < 0) return VJO_E_OOM;
    if (vjo_styled_init(&b, &a, VJO_BODY_UNITS, VJO_BODY_SPANS) < 0) return VJO_E_OOM;
    vjo_styled_header(&h, &shown.list, 18);
    vjo_styled_entry(&b, &shown.list, 0, 18, 15, 0, 1);
    dig(d, h.text, h.len * 2);
    dig(d, b.text, b.len * 2);
    dig_int(d, h.n_spans);
    dig_int(d, b.n_spans);
    return VJO_OK;
}

/* An Anki card from the shown entry, as shell/anki.c makes it (its 128 KiB arena), its queue record read back as the
 * Rubi app reads it, and its Lapis fields (settings/anki_export.c). */
static uint8_t anki_mem[128 * 1024];
static int op_anki(void *ud, uint8_t *mem, size_t size, Digest *d)
{
    VjoArena a;
    VjoAnkiNote note, back;
    char *json, *fields;
    size_t len = 0;
    int picture = 0, rc;
    (void)ud;
    vjo_arena_init(&a, mem, size);
    memset(&note, 0, sizeof(note));
    if ((rc = vjo_anki_note_from_entry(&a, &shown.list, 0, &note)) != 0) return rc < 0 ? rc : VJO_E_OOM;
    if (!(json = vjo_queue_encode(&a, &note, 0))) return VJO_E_OOM;
    dig_str(d, json);
    memset(&back, 0, sizeof(back));
    if (vjo_queue_decode(&a, json, strlen(json), &back, &picture) < 0) return VJO_E_OOM;
    if (!(fields = vjo_lapis_fields(&a, &back, NULL, NULL, &len))) return VJO_E_OOM;
    dig(d, fields, len);
    dig_int(d, picture);
    return VJO_OK;
}

/* The export's sentence readings of a card (settings/anki_export.c: its readings' scratch, VJO_READINGS_SCRATCH;
 * the furigana in the package's arena). Arena: the scratch. */
static uint8_t readings_mem[VJO_READINGS_SCRATCH];
static uint8_t keep_mem[64 * 1024];
static VjoAnkiNote card;
static int op_readings(void *ud, uint8_t *mem, size_t size, Digest *d)
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
        return rc ? rc : VJO_E_OOM;
    if (trace_lists)
        printf("    readings rc %d, read %u unplaced %u unchecked %u failed %u: %.120s\n", rc, st.read, st.unplaced,
               st.unchecked, st.failed, html);
    dig_str(d, html);
    return VJO_OK;
}

static void lookups(void)
{
    unsigned runs = 0, failed = 0, same = 0, degraded = 0, lines_done = 0;
    if (!have_dictionary()) {
        printf("SKIP: no plaintext.vjdict (set VJO_PLAINTEXT_DIR)\n");
        return;
    }
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
        runs += q.runs; failed += q.failed; same += q.ok_same; degraded += q.ok_degraded; lines_done++;
    }
    if (plat.local_session) vjo_local_session_close(plat.local_session);
    plat.local_session = NULL;
    printf("\n  lookups: %u lines, %u cut arenas: %u refused, %u gave the full result, %u the entries with the other "
           "readings left out (marked)\n", lines_done, runs, failed, same, degraded);
}

static void cards(void)
{
    unsigned runs[3] = {0}, failed[3] = {0}, lines_done = 0;
    if (!have_dictionary()) {
        printf("SKIP: no plaintext.vjdict (set VJO_PLAINTEXT_DIR)\n");
        return;
    }
    setup();
    for (int i = 0; i < n_lines; i++) {
        VjoArena a;
        Squeeze q;
        char what[64];
        static uint8_t lookup_mem[128 * 1024];
        vjo_arena_init(&a, lookup_mem, sizeof(lookup_mem));
        memset(&shown, 0, sizeof(shown));
        if (vjo_overlay_from_text(&a, &plat, &cfg, lines[i], &shown) != VJO_OK || shown.list.n_entries < 1) continue;
        lines_done++;
        snprintf(what, sizeof(what), "overlay texts of line %d", i);
        q = squeeze(what, op_styled, NULL, anki_mem, 20 * 1024);
        runs[0] += q.runs; failed[0] += q.failed;
        snprintf(what, sizeof(what), "Anki card of line %d", i);
        q = squeeze(what, op_anki, NULL, anki_mem, sizeof(anki_mem));
        runs[1] += q.runs; failed[1] += q.failed;
        {
            static uint8_t card_mem[32 * 1024]; /* the card's strings: apart from the readings' arenas */
            VjoArena c;
            vjo_arena_init(&c, card_mem, sizeof(card_mem));
            memset(&card, 0, sizeof(card));
            if (vjo_anki_note_from_entry(&c, &shown.list, 0, &card) != 0) continue;
            snprintf(what, sizeof(what), "sentence readings of line %d", i);
            q = squeeze(what, op_readings, NULL, readings_mem, sizeof(readings_mem));
            runs[2] += q.runs; failed[2] += q.failed;
        }
    }
    printf("\n  %u lines: overlay texts %u cut arenas (%u refused), Anki cards %u (%u), sentence readings %u (%u)\n",
           lines_done, runs[0], failed[0], runs[1], failed[1], runs[2], failed[2]);
}

/* ---- the line history (shell/worker.c history_open: its PAF block's arena) ---- */

static char history_csv[48 * 1024];
static size_t history_len;
static const char uuid[] = "vita-PCSG00415\",";
static void make_history(void)
{
    size_t n = 0;
    n += (size_t)snprintf(history_csv + n, sizeof(history_csv) - n, "uuid,given_identifier,name,line,time\n");
    for (int i = 0; i < 160 && n + 512 < sizeof(history_csv); i++)
        n += (size_t)snprintf(history_csv + n, sizeof(history_csv) - n,
                              "\"vita-%s\",\"vjo-%d\",\"g\",\"%s%d\",%d.162344,vita,%s,vocr,m\n",
                              i % 5 ? "PCSG00415" : "PCSG00402", i,
                              i % 3 ? "「あ、\"\"そうなんだ\"\"」" : "ここのジュース、美味しいって\nクラスで評判でさ", i,
                              1792235952 + i, i % 5 ? "PCSG00415" : "PCSG00402");
    history_len = n;
}

static VjoHistory hist;
static int op_history(void *ud, uint8_t *mem, size_t size, Digest *d)
{
    static char buf[sizeof(history_csv)];
    static const char current[] = "ここのジュース、美味しいって";
    VjoArena a;
    (void)ud;
    memcpy(buf, history_csv, history_len); /* the scan unescapes in place */
    memset(&hist, 0, sizeof(hist));
    vjo_arena_init(&a, mem, size);
    if (!(hist.line[0] = vjo_arena_strndup(&a, current, strlen(current)))) return VJO_E_OOM;
    vjo_history_scan(&hist, &a, buf, history_len, 1, uuid, 0);
    dig_int(d, hist.n);
    for (int i = 0; i <= hist.n; i++) dig_str(d, hist.line[i]);
    return VJO_OK;
}

/* A cut arena keeps the newest lines that fit (history_lines.h: "the lines kept are always the newest, without a
 * gap"): every run returns, and its lines are the full run's first ones. */
static void history(void)
{
    static uint8_t mem[16 * 1024];
    static char full[VJO_HISTORY_LINES + 2][512];
    static size_t ends[4096];
    unsigned n, runs = 0, fewer = 0;
    int full_n;
    Digest d = {0};
    make_history();
    vjo_test_arena_base = mem;
    vjo_test_arena_cap = (size_t)-1;
    vjo_test_arena_n_ends = 0;
    TEST_ASSERT(op_history(NULL, mem, sizeof(mem), &d) == VJO_OK);
    full_n = hist.n;
    TEST_CHECK_(full_n >= 8, "the full scan keeps %d earlier lines", full_n);
    for (int i = 0; i <= full_n; i++) snprintf(full[i], sizeof(full[i]), "%s", hist.line[i]);
    n = vjo_test_arena_n_ends;
    memcpy(ends, vjo_test_arena_ends, n * sizeof(ends[0]));
    for (unsigned i = 0; i < n; i++) {
        Digest e = {0};
        vjo_test_arena_cap = ends[i] - 1;
        runs++;
        if (op_history(NULL, mem, sizeof(mem), &e) != VJO_OK) continue;
        TEST_CHECK_(hist.n <= full_n, "cut at %zu: %d lines of %d", ends[i] - 1, hist.n, full_n);
        fewer += hist.n < full_n;
        for (int k = 1; k <= hist.n && k <= full_n; k++)
            TEST_CHECK_(hist.line[k] && !strcmp(hist.line[k], full[k]), "cut at %zu: line %d is the full scan's",
                        ends[i] - 1, k);
    }
    printf("\n  history: %d earlier lines; %u cut arenas, %u kept fewer, each the newest without a gap\n", full_n, runs,
           fewer);
    vjo_test_arena_base = NULL;
    vjo_test_arena_cap = (size_t)-1;
}

TEST_LIST = {
    {"lookups: every block of the result arena the first that does not fit", lookups},
    {"overlay texts, Anki cards and sentence readings with cut arenas", cards},
    {"line history with a cut arena", history},
    {NULL, NULL},
};
