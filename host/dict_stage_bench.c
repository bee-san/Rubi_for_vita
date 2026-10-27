/* vjo-dict-stage-bench: the Shell's cursor lookup (vjo_overlay_lookup_at) at every tap of many
 * lines, each stage timed and every file read attributed, over a taps corpus
 * (tools/yomitan_check/build_corpus.py's corpus.tsv: "ID\tTEXT\tTAPS", TAPS the code point
 * offsets to tap, comma-separated). Set up as shell/worker.c sets a cursor lookup up: one
 * 64 KiB block with the dictionary session at its start (kept across lines, as an open overlay
 * keeps it) and the result arena after it, the line's text generation for the session's word
 * locators, and the configured Dictionaries shown (RESULTS), whose stacked entries come with the
 * word. The lookups are the console's code; the file reads are the host's (warm OS cache: host
 * times measure CPU; the console's cost per read comes from its logs, tools/dict_stage_report.py).
 *
 *   vjo-dict-stage-bench DIR FILENAMES RESULTS cursor|prepared|word|character ROUNDS < corpus.tsv > taps.jsonl
 *
 * cursor: each tap is a cursor lookup with nothing prepared (a d-pad step or a tap right after
 * the text appeared). prepared: Auto's lookahead located the word first (vjo_local_prepare_at,
 * its reads counted apart as background work), as it does for the eight words after the shown
 * one while the worker is idle; the lookup then runs on the located word. word, character: the
 * open overlay's taps in order with the worker's lookahead between them in that highlight mode
 * (lookahead() below: this tree's, or main's before #360 when built against it), the taps' own
 * reads and, as prepare reads, the background ones before each.
 *
 * Round 0 counts (core/local_dict.c built with VJO_LOCAL_STATS): reads by what they fetch, bytes,
 * distinct 4 KiB pages, file opens, the candidates generated/searched/found, postings ranked from
 * heads or hints, the arena peak, and the host nanoseconds by stage: deinflection, search (filter,
 * index, key group, ranking), the shown entry's read, the stacked entries' reads, and the popup
 * text's build (core/styled.c vjo_styled_entry, as shell/overlay.cpp builds it each frame it shows
 * a new entry). Rounds 1.. time the whole lookup again (median printed as us) and fail if a tap
 * shows something else. One JSON line per tap, then a summary line. */
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "client.h"
#include "local_dict.h"
#include "platform_posix.h"
#include "port.h"
#include "styled.h"
#include "text_rows.h"
#include "utf.h"

#define RESULT_BLOCK (64 * 1024) /* shell/worker.c LOCAL_RESULT_ARENA_SIZE */
#define MAX_PAGES 512

typedef struct {
    VjoFile inner;
    int active;
} TrackedFile;

static TrackedFile files[VJO_LOCAL_MAX_DICTS];
static uint64_t n_reads, n_bytes, n_opens;
static uint64_t pages[MAX_PAGES];
static unsigned n_pages, pages_overflow;
static int track_pages = 1;
static int (*inner_open)(void *, const char *, VjoFile *);

static void note_page(int file, uint64_t page)
{
    uint64_t key = ((uint64_t)file << 56) | page;
    for (unsigned i = 0; i < n_pages; i++)
        if (pages[i] == key) return;
    if (n_pages < MAX_PAGES) pages[n_pages++] = key;
    else pages_overflow++;
}

static int tracked_read(void *ctx, uint64_t off, void *dst, size_t n)
{
    TrackedFile *f = ctx;
    n_reads++;
    n_bytes += n;
    for (uint64_t p = off >> 12; track_pages && n && p <= (off + n - 1) >> 12; p++) note_page((int)(f - files), p);
    return f->inner.read(f->inner.ctx, off, dst, n);
}

static void tracked_close(void *ctx)
{
    TrackedFile *f = ctx;
    f->inner.close(f->inner.ctx);
    f->active = 0;
}

static int tracked_open(void *ud, const char *path, VjoFile *out)
{
    for (int i = 0; i < VJO_LOCAL_MAX_DICTS; i++) {
        TrackedFile *f = &files[i];
        if (f->active) continue;
        if (inner_open(ud, path, &f->inner)) return -1;
        f->active = 1;
        n_opens++;
        *out = (VjoFile){f, f->inner.size, tracked_read, tracked_close, f->inner.stamp};
        return 0;
    }
    return -1;
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const unsigned char *s = p;
    while (n--) h = (h ^ *s++) * UINT64_C(1099511628211);
    return h;
}

static uint64_t fnv_str(uint64_t h, const char *s)
{
    return s ? fnv(h, s, strlen(s) + 1) : fnv(h, "", 1);
}

/* What the popup shows: rc, the entry's text, its highlight and conjugation, the stacked entries. */
static uint64_t digest(int rc, const VjoOverlayData *d)
{
    uint64_t h = UINT64_C(14695981039346656037);
    h = fnv(h, &rc, sizeof(rc));
    h = fnv(h, &d->list.n_entries, sizeof(d->list.n_entries));
    for (int i = 0; i < d->list.n_entries; i++) {
        const VjoEntry *e = &d->list.entries[i];
        int pos[3] = {e->pos16, e->hl_start, e->hl_end};
        h = fnv_str(h, e->text);
        h = fnv(h, pos, sizeof(pos));
        h = fnv(h, &e->conjugation, sizeof(e->conjugation));
    }
    h = fnv(h, &d->list.n_more, sizeof(d->list.n_more));
    for (int i = 0; i < d->list.n_more; i++) {
        h = fnv_str(h, d->list.more[i]->spelling);
        h = fnv_str(h, d->list.more[i]->reading);
        for (int m = 0; m < d->list.more[i]->n_meanings; m++) h = fnv_str(h, d->list.more[i]->meanings[m]);
    }
    return h;
}

typedef struct {
    char *id;
    size_t cursor;
    int tap;
    int rc, entries, more, chars;
    uint64_t digest, opens, reads, bytes, pages, arena_peak, entry_bytes;
    uint64_t prepare_reads, prepare_bytes, ns_total, ns_styled, styled_units;
    int prepare_rc;
    VjoLocalStats stats;
    double *us;
} Query;

static Query *queries;
static size_t n_queries, cap_queries;

static Query *new_query(void)
{
    if (n_queries == cap_queries) {
        cap_queries = cap_queries ? cap_queries * 2 : 4096;
        queries = realloc(queries, cap_queries * sizeof(*queries));
        if (!queries) exit(3);
    }
    memset(&queries[n_queries], 0, sizeof(*queries));
    return &queries[n_queries++];
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

typedef struct {
    char *id, *text;
    size_t *cursors;
    int *taps, n_taps;
} Line;

static Line *lines;
static int n_lines, cap_lines;

static uint64_t block64[RESULT_BLOCK / sizeof(uint64_t)]; /* aligned as the Shell's PAF block */
#define block ((uint8_t *)block64)
static char inbuf[3 * VJO_LOCAL_MAX_TEXT];
static uint8_t textmem[64 * 1024];
static uint8_t uimem[2 * VJO_BODY_UNITS * 2 + 8192]; /* the overlay's body: units and spans */

/* The popup's body text for the shown entry, as shell/overlay.cpp render() builds it (the
 * entry's part: headword, badge, conjugation line, definition, kanji lines, stacked entries). */
static uint64_t styled_build(const VjoOverlayData *d, uint64_t *units)
{
    VjoArena ui;
    VjoStyled body;
    uint64_t t0 = now_ns();
    vjo_arena_init(&ui, uimem, sizeof(uimem));
    *units = 0;
    if (vjo_styled_init(&body, &ui, VJO_BODY_UNITS, VJO_BODY_SPANS) == 0) {
        vjo_styled_entry(&body, &d->list, 0, 28, 20, 0, 1); /* the defaults' pixel sizes, badge on */
        *units = body.len;
    }
    return now_ns() - t0;
}

/* One tap, as shell/worker.c run_cursor_job runs it. prepare: Auto's lookahead first. */
static int run_tap(VjoPlatform *p, const VjoConfig *cfg, const Line *ln, int t, int prepare, Query *q,
                   VjoOverlayData *out, uint64_t *ns_total)
{
    VjoArena a;
    uint64_t t0;
    int rc;
    memset(out, 0, sizeof(*out));
    out->ocr_text = out->filtered = out->sentence = ln->text;
    out->list.header = ln->text;
    vjo_arena_init(&a, block + vjo_local_session_size(), RESULT_BLOCK - vjo_local_session_size());
    if (prepare) {
        size_t end;
        int prc;
        n_reads = n_bytes = 0;
        memset(&vjo_local_stats, 0, sizeof(vjo_local_stats));
        /* worker.c: the lookahead works in the arena after the shown entry; here the arena is
         * empty, as after a tap whose entry was released (the locators, not the entry, are kept). */
        prc = vjo_local_prepare_at(&a, p, cfg, ln->text, ln->cursors[t], &end);
        if (q) {
            q->prepare_rc = prc;
            q->prepare_reads = n_reads;
            q->prepare_bytes = n_bytes;
        }
        vjo_arena_reset(&a);
    }
    n_reads = n_bytes = n_opens = 0;
    n_pages = pages_overflow = 0;
    memset(&vjo_local_stats, 0, sizeof(vjo_local_stats));
    t0 = now_ns();
    rc = vjo_overlay_lookup_at(&a, p, cfg, ln->cursors[t], out);
    *ns_total = now_ns() - t0;
    if (q) {
        q->rc = rc;
        q->opens = n_opens;
        q->reads = n_reads;
        q->bytes = n_bytes;
        q->pages = n_pages + pages_overflow;
        q->arena_peak = a.peak + vjo_local_session_size();
        q->stats = vjo_local_stats;
    }
    return rc;
}

/* The lookahead modes (word, character): the open overlay's lookups with shell/worker.c's lookahead
 * (prepare_words) between them, each pass run to its end (the idle time it needs is assumed): the line's
 * first word looked up on opening (byte 0) and its pass, then each tap's lookup and the pass from it.
 * A tap's prepare reads are the background reads of the pass before it. Built with this tree's
 * core/local_dict.h (VJO_LOCAL_LOCATED, #360): shell/worker.c's pass (Right's words, then each character
 * before the cursor, then the rest, a located position passed; VJO_LOCAL_LOCATED units, a unit a job and a
 * unit a step past a located position);
 * built against main before #360 (no VJO_LOCAL_LOCATED): its pass, Whole word only, the cursor's word
 * and the next seven Right reaches, and nothing in Character mode. */
static uint64_t bg_reads, bg_bytes;
static int bg_prepare(VjoPlatform *p, const VjoConfig *cfg, const char *text, size_t at, size_t *end)
{
    VjoArena a;
    uint64_t r = n_reads, b = n_bytes;
    int rc;
    vjo_arena_init(&a, block + vjo_local_session_size(), RESULT_BLOCK - vjo_local_session_size());
    rc = vjo_local_prepare_at(&a, p, cfg, text, at, end);
    bg_reads += n_reads - r;
    bg_bytes += n_bytes - b;
    return rc;
}
static void lookahead(VjoPlatform *p, const VjoConfig *cfg, const char *text, size_t cursor, int word)
{
    size_t len = strlen(text), at = cursor, end = cursor, next;
#ifdef VJO_LOCAL_LOCATED
    /* shell/worker.c prepare_words' loop: a unit a job, a unit a step past a located position */
    int phase = 0, left = VJO_LOCAL_LOCATED;
    while (left-- > 0) {
        if (!vjo_local_located(p, cfg, text, at, &end)) {
            if (bg_prepare(p, cfg, text, at, &end)) break; /* a failure ends the pass (worker.c on_job_done) */
            continue;                                       /* the worker's next tick: at, located */
        }
        next = at;
        vjo_text_row_step(text, len, NULL, at, phase == 1 ? at : !phase && word && end > at ? end : at + 1,
                          phase == 1 ? -1 : 1, cfg->japanese_only_selection, 0, &next);
        if (next != at) at = next;
        else if (++phase > 2) break;
        else at = cursor;
    }
#else
    for (int left = 8; word && left > 0 && at < len; left--) {
        VjoWordNavigation nav = {{0}, 0};
        int row;
        bg_prepare(p, cfg, text, at, &end);
        next = vjo_word_step(&nav, text, len, at, end, 1, cfg->japanese_only_selection, NULL, 0, &row);
        if (next <= at) break;
        at = next;
    }
#endif
}
/* The line's first word, looked up as the overlay opens on it (not a tap: no counts), then its pass. */
static void open_line(VjoPlatform *p, const VjoConfig *cfg, const Line *ln, int word)
{
    VjoArena a;
    VjoOverlayData d;
    memset(&d, 0, sizeof(d));
    d.ocr_text = d.filtered = d.sentence = ln->text;
    d.list.header = ln->text;
    vjo_arena_init(&a, block + vjo_local_session_size(), RESULT_BLOCK - vjo_local_session_size());
    vjo_overlay_lookup_at(&a, p, cfg, 0, &d);
    bg_reads = bg_bytes = 0;
    lookahead(p, cfg, ln->text, 0, word);
}

int main(int argc, char **argv)
{
    VjoConfig cfg;
    PosixPlatform pp = {0};
    VjoPlatform p;
    uint32_t generation = 0;
    int rounds, prepare, ahead = 0, word = 0;
    if (argc != 6 || (rounds = atoi(argv[5])) < 1 || rounds > 1000) {
        fprintf(stderr, "usage: vjo-dict-stage-bench DIR FILENAMES RESULTS cursor|prepared|word|character ROUNDS"
                        " < corpus.tsv\n");
        return 2;
    }
    if (!strcmp(argv[4], "cursor")) prepare = 0;
    else if (!strcmp(argv[4], "prepared")) prepare = 1;
    else if (!strcmp(argv[4], "word")) prepare = 0, ahead = 1, word = 1;
    else if (!strcmp(argv[4], "character")) prepare = 0, ahead = 1, word = 0;
    else { fprintf(stderr, "unknown mode %s\n", argv[4]); return 2; }
    vjo_config_defaults(&cfg);
    cfg.dictionary = VJO_DICT_LOCAL;
    if (strlen(argv[1]) >= sizeof(cfg.local_dictionary_dir) || strlen(argv[2]) >= sizeof(cfg.local_dictionaries))
        return 2;
    strcpy(cfg.local_dictionary_dir, argv[1]);
    strcpy(cfg.local_dictionaries, argv[2]);
    cfg.local_dictionary_results = atoi(argv[3]);
    if (cfg.local_dictionary_results < 1 || cfg.local_dictionary_results > VJO_LOCAL_MAX_DICTS) return 2;
    posix_platform_init(&pp, &p);
    inner_open = p.file_open;
    p.file_open = tracked_open;
    if (vjo_local_session_size() > RESULT_BLOCK / 2) return 2;
    memset(block, 0, vjo_local_session_size());
    p.local_session = (VjoLocalSession *)block;

    /* The lines and their taps, each line's text as the Shell keeps it (vjo_overlay_ocr_text). */
    while (fgets(inbuf, sizeof(inbuf), stdin)) {
        char *id = inbuf, *text, *taps, *end;
        VjoArena ta;
        VjoOverlayData d;
        Line *ln;
        int cap = 16;
        inbuf[strcspn(inbuf, "\r\n")] = 0;
        if (!(text = strchr(id, '\t')) || !(taps = strchr(text + 1, '\t'))) continue;
        *text++ = 0;
        *taps++ = 0;
        vjo_arena_init(&ta, textmem, sizeof(textmem));
        if (strlen(text) > VJO_LOCAL_MAX_TEXT || vjo_overlay_ocr_text(&ta, &cfg, text, &d) || !d.sentence ||
            strcmp(d.sentence, text))
            continue; /* a filtered line: not the corpus's text (vjo-dict-taps reports it) */
        if (n_lines == cap_lines) {
            cap_lines = cap_lines ? cap_lines * 2 : 1024;
            lines = realloc(lines, cap_lines * sizeof(*lines));
            if (!lines) return 3;
        }
        ln = &lines[n_lines++];
        ln->id = strdup(id);
        ln->text = strdup(text);
        ln->taps = malloc(cap * sizeof(int));
        ln->cursors = malloc(cap * sizeof(size_t));
        ln->n_taps = 0;
        for (char *t = taps; *t; t = *end ? end + 1 : end) {
            long tap = strtol(t, &end, 10);
            size_t cursor = 0, n = strlen(text);
            if (end == t || tap < 0) break;
            for (long k = 0; k < tap && cursor < n; k++) vjo_utf8_next(text, n, &cursor);
            if (cursor >= n) continue;
            if (ln->n_taps == cap) {
                cap *= 2;
                ln->taps = realloc(ln->taps, cap * sizeof(int));
                ln->cursors = realloc(ln->cursors, cap * sizeof(size_t));
                if (!ln->taps || !ln->cursors) return 3;
            }
            ln->taps[ln->n_taps] = (int)tap;
            ln->cursors[ln->n_taps++] = cursor;
        }
    }
    if (!n_lines) { fprintf(stderr, "no lines\n"); return 2; }

    /* Round 0: the taps, their results and counts. */
    for (int i = 0; i < n_lines; i++) {
        Line *ln = &lines[i];
        if (!++generation) generation = 1; /* the worker's job_text_generation: a new text */
        p.local_text_generation = generation;
        if (ahead) open_line(&p, &cfg, ln, word);
        for (int t = 0; t < ln->n_taps; t++) {
            Query *q = new_query();
            VjoOverlayData d;
            uint64_t ns;
            q->id = ln->id;
            q->tap = ln->taps[t];
            q->cursor = ln->cursors[t];
            run_tap(&p, &cfg, ln, t, prepare, q, &d, &ns);
            if (ahead) { /* the pass before this tap (the one from it follows its result: it reuses the arena) */
                q->prepare_reads = bg_reads;
                q->prepare_bytes = bg_bytes;
                bg_reads = bg_bytes = 0;
            }
            q->ns_total = ns;
            q->digest = digest(q->rc, &d);
            q->entries = d.list.n_entries;
            q->more = d.list.n_more;
            if (q->entries) {
                const VjoEntry *e = &d.list.entries[0];
                size_t n = strlen(ln->text);
                for (size_t k = (size_t)e->hl_start; k < (size_t)e->hl_end && k < n; q->chars++) vjo_utf8_next(ln->text, n, &k);
                q->entry_bytes = strlen(e->text);
                q->ns_styled = styled_build(&d, &q->styled_units);
            }
            if (ahead) lookahead(&p, &cfg, ln->text, ln->cursors[t], word);
        }
    }

    /* Rounds 1..: the same taps, timed; a changed result fails. */
    track_pages = 0;
    for (size_t k = 0; k < n_queries; k++)
        queries[k].us = calloc((size_t)rounds, sizeof(double));
    for (int r = 1; r < rounds; r++) {
        size_t k = 0;
        vjo_local_session_close(p.local_session);
        memset(block, 0, vjo_local_session_size());
        generation = 0;
        for (int i = 0; i < n_lines; i++) {
            Line *ln = &lines[i];
            if (!++generation) generation = 1;
            p.local_text_generation = generation;
            if (ahead) open_line(&p, &cfg, ln, word);
            for (int t = 0; t < ln->n_taps; t++, k++) {
                Query *q = &queries[k];
                VjoOverlayData d;
                uint64_t ns;
                int rc = run_tap(&p, &cfg, ln, t, prepare, NULL, &d, &ns);
                q->us[r - 1] = (double)ns / 1000.0;
                if (digest(rc, &d) != q->digest) {
                    fprintf(stderr, "%s at %d: result changed between rounds\n", ln->id, ln->taps[t]);
                    return 1;
                }
                if (ahead) lookahead(&p, &cfg, ln->text, ln->cursors[t], word);
            }
        }
    }
    vjo_local_session_close(p.local_session);

    for (size_t k = 0; k < n_queries; k++) {
        Query *q = &queries[k];
        const VjoLocalStats *st = &q->stats;
        double us = (double)q->ns_total / 1000.0;
        if (rounds > 1) {
            qsort(q->us, (size_t)(rounds - 1), sizeof(double), cmp_double);
            us = q->us[(rounds - 1) / 2];
        }
        printf("{\"id\":\"%s\",\"tap\":%d,\"cursor\":%zu,\"rc\":%d,\"entries\":%d,\"more\":%d,\"chars\":%d,"
               "\"digest\":\"%016" PRIx64 "\",\"us\":%.3f,\"us_round0\":%.3f,\"us_styled\":%.3f,\"styled_units\":%" PRIu64
               ",\"entry_bytes\":%" PRIu64 ",\"opens\":%" PRIu64 ",\"reads\":%" PRIu64 ",\"bytes\":%" PRIu64
               ",\"pages\":%" PRIu64 ",\"arena_peak\":%" PRIu64
               ",\"spans\":%" PRIu64 ",\"candidates\":%" PRIu64 ",\"probes\":%" PRIu64 ",\"filtered\":%" PRIu64
               ",\"miss_cached\":%" PRIu64 ",\"hash_present\":%" PRIu64 ",\"found\":%" PRIu64 ",\"ranked\":%" PRIu64
               ",\"hinted\":%" PRIu64 ",\"absent_reads\":%" PRIu64
               ",\"reads_by\":{\"header\":%" PRIu64 ",\"index\":%" PRIu64 ",\"group\":%" PRIu64 ",\"head\":%" PRIu64
               ",\"payload\":%" PRIu64 ",\"filter\":%" PRIu64 "},\"bytes_by\":{\"header\":%" PRIu64
               ",\"index\":%" PRIu64 ",\"group\":%" PRIu64 ",\"head\":%" PRIu64 ",\"payload\":%" PRIu64
               ",\"filter\":%" PRIu64 "},\"ns_by\":{\"deinflect\":%" PRIu64 ",\"search\":%" PRIu64 ",\"rank\":%" PRIu64
               ",\"entry\":%" PRIu64 ",\"page\":%" PRIu64 "},\"prepare\":{\"rc\":%d,\"reads\":%" PRIu64 ",\"bytes\":%" PRIu64 "}}\n",
               q->id, q->tap, q->cursor, q->rc, q->entries, q->more, q->chars, q->digest, us,
               (double)q->ns_total / 1000.0, (double)q->ns_styled / 1000.0, q->styled_units, q->entry_bytes,
               q->opens, q->reads, q->bytes, q->pages, q->arena_peak, st->spans, st->candidates,
               st->probes, st->filtered, st->miss_cached, st->hash_present, st->found, st->ranked, st->hinted, st->absent_reads,
               st->reads[VJO_READ_HEADER], st->reads[VJO_READ_INDEX], st->reads[VJO_READ_GROUP],
               st->reads[VJO_READ_HEAD], st->reads[VJO_READ_PAYLOAD], st->reads[VJO_READ_FILTER],
               st->bytes[VJO_READ_HEADER], st->bytes[VJO_READ_INDEX], st->bytes[VJO_READ_GROUP],
               st->bytes[VJO_READ_HEAD], st->bytes[VJO_READ_PAYLOAD], st->bytes[VJO_READ_FILTER],
               st->ns[VJO_NS_DEINFLECT], st->ns[VJO_NS_SEARCH], st->ns[VJO_NS_RANK], st->ns[VJO_NS_ENTRY], st->ns[VJO_NS_PAGE],
               q->prepare_rc, q->prepare_reads, q->prepare_bytes);
    }
    printf("{\"summary\":{\"lines\":%d,\"taps\":%zu,\"mode\":\"%s\",\"results\":%d,\"dictionaries\":\"%s\",\"rounds\":%d,"
           "\"block_bytes\":%u,\"session_bytes\":%zu}}\n",
           n_lines, n_queries, argv[4], cfg.local_dictionary_results, argv[2], rounds, (unsigned)RESULT_BLOCK,
           vjo_local_session_size());
    for (size_t k = 0; k < n_queries; k++) free(queries[k].us);
    free(queries);
    for (int i = 0; i < n_lines; i++) {
        free(lines[i].id);
        free(lines[i].text);
        free(lines[i].taps);
        free(lines[i].cursors);
    }
    free(lines);
    return 0;
}
