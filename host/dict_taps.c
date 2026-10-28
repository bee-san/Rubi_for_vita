/* vjo-dict-taps: the Shell's tap lookup (vjo_overlay_lookup_at) at chosen characters of many lines,
 * set up as shell/worker.c sets it up: one 64 KiB block (LOCAL_RESULT_ARENA_SIZE) with the
 * dictionary session at its start and the result arena after it, the line's text generation for
 * the session's word locators, and the configured Dictionaries shown (local_dictionary_results) whose
 * stacked entries come with the word. For comparisons with other lookups over corpora
 * (tools/yomitan_check); the lookups are the Shell's, the file reads the host's.
 *
 *   vjo-dict-taps DIR FILENAMES [RESULTS] < lines.tsv > taps.tsv
 *
 * Each input line is "ID\tTEXT\tTAPS", TAPS the code point offsets in TEXT to tap, comma-separated.
 * The text goes through vjo_overlay_ocr_text first, as recognized or hooked text does; a line whose
 * filtered sentence is not TEXT itself is reported once, as "ID\t-1\tfiltered". Each tap prints
 * "ID\tTAP\tLEN\tSPELLING\tREADING\tRANK\tREASONS\tDICTIONARY\tMORE": LEN the code points the
 * highlight covers (0 and the rest empty: no entry), REASONS the conjugation's labels from the
 * base form (core/conjugation.inc, as the popup shows them), comma-separated, DICTIONARY
 * the entry's file's title, MORE the stacked entries as "SPELLING|READING|DICTIONARY", ';'-joined.
 * A failed lookup prints "ID\tTAP\terror\tMESSAGE". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "client.h"
#include "local_dict.h"
#include "platform_posix.h"
#include "port.h"
#include "utf.h"
#include "conjugation.inc"

#define RESULT_BLOCK (64 * 1024) /* shell/worker.c LOCAL_RESULT_ARENA_SIZE */

static uint64_t block64[RESULT_BLOCK / sizeof(uint64_t)]; /* aligned as the Shell's PAF block */
#define block ((uint8_t *)block64)
static char line[3 * VJO_LOCAL_MAX_TEXT];
static char sentence[VJO_LOCAL_MAX_TEXT + 1];

static void field(const char *s)
{
    for (; s && *s; s++) putchar(*s == '\t' || *s == '\n' || *s == '|' || *s == ';' ? ' ' : *s);
}

int main(int argc, char **argv)
{
    static uint8_t textmem[64 * 1024];
    VjoConfig cfg;
    PosixPlatform pp = {0};
    VjoPlatform p;
    uint32_t generation = 0;
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: vjo-dict-taps DIR FILENAMES [RESULTS] < lines.tsv\n");
        return 2;
    }
    vjo_config_defaults(&cfg);
    if (strlen(argv[1]) >= sizeof(cfg.local_dictionary_dir) || strlen(argv[2]) >= sizeof(cfg.local_dictionaries))
        return 2;
    strcpy(cfg.local_dictionary_dir, argv[1]);
    strcpy(cfg.local_dictionaries, argv[2]);

    if (vjo_local_names_count(cfg.local_dictionaries) < 1 || vjo_local_names_count(cfg.local_dictionaries) > VJO_LOCAL_MAX_DICTS) return 2;
    posix_platform_init(&pp, &p);
    if (vjo_local_session_size() > RESULT_BLOCK / 2) return 2;
    memset(block, 0, vjo_local_session_size());
    p.local_session = (VjoLocalSession *)block;
    while (fgets(line, sizeof(line), stdin)) {
        char *id = line, *text, *taps, *end;
        VjoArena ta;
        VjoOverlayData d;
        size_t n;
        line[strcspn(line, "\r\n")] = 0;
        if (!(text = strchr(id, '\t')) || !(taps = strchr(text + 1, '\t'))) continue;
        *text++ = 0;
        *taps++ = 0;
        vjo_arena_init(&ta, textmem, sizeof(textmem));
        if (strlen(text) > VJO_LOCAL_MAX_TEXT || vjo_overlay_ocr_text(&ta, &cfg, text, &d) || !d.sentence ||
            strcmp(d.sentence, text)) {
            printf("%s\t-1\tfiltered\n", id);
            continue;
        }
        n = strlen(text);
        memcpy(sentence, text, n + 1);
        if (!++generation) generation = 1; /* the worker's job_text_generation: a new text */
        p.local_text_generation = generation;
        for (char *t = taps; *t; t = *end ? end + 1 : end) {
            long tap = strtol(t, &end, 10);
            size_t cursor = 0;
            VjoArena a;
            VjoOverlayData out;
            if (end == t || tap < 0) break;
            for (long k = 0; k < tap && cursor < n; k++) vjo_utf8_next(sentence, n, &cursor);
            if (cursor >= n) continue;
            /* run_cursor_job: the published text, the arena after the session, reset */
            memset(&out, 0, sizeof(out));
            out.ocr_text = out.filtered = out.sentence = sentence;
            out.list.header = sentence;
            vjo_arena_init(&a, block + vjo_local_session_size(), RESULT_BLOCK - vjo_local_session_size());
            if (vjo_overlay_lookup_at(&a, &p, &cfg, cursor, &out)) {
                printf("%s\t%ld\terror\t%s\n", id, tap, vjo_err_text(&a, out.failed_stage, &out.err));
                continue;
            }
            printf("%s\t%ld\t", id, tap);
            if (!out.list.n_entries) {
                printf("0\t\t\t\t\t\t\n");
                continue;
            }
            const VjoEntry *e = &out.list.entries[0];
            int chars = 0;
            for (size_t i = (size_t)e->hl_start; i < (size_t)e->hl_end; chars++) vjo_utf8_next(sentence, n, &i);
            printf("%d\t", chars);
            field(e->vocab->spelling);
            putchar('\t');
            field(e->vocab->reading);
            printf("\t%d\t", e->vocab->rank);
            for (uint32_t r = e->conjugation, first = 1; r; r >>= 6, first = 0) {
                unsigned reason = r & 63u;
                printf("%s%s", first ? "" : ",", reason && reason < sizeof(conjugation_offsets) / sizeof(*conjugation_offsets)
                    ? conjugation_labels + conjugation_offsets[reason] : "…");
            }
            putchar('\t');
            field(e->vocab->dictionary);
            putchar('\t');
            for (int i = 0; i < out.list.n_more; i++) {
                if (i) putchar(';');
                field(out.list.more[i]->spelling);
                putchar('|');
                field(out.list.more[i]->reading);
                putchar('|');
                field(out.list.more[i]->dictionary);
            }
            putchar('\n');
        }
    }
    return 0;
}
