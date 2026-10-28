/* The real overlay lookup (vjo_overlay_lookup_at, as a tap on a word's first
 * character) for many words in one process: one word per stdin line, one
 * output line per word, "spelling\treading\trank\tlen16", or "-" when no entry
 * starts there. For measurements over corpora of words (tools/frequency_eval.py);
 * the lookups are the Shell's, the file reads the host's.
 *
 *   vjo-word-forms DIR FILENAMES < words.txt > shown.tsv
 */
#include <stdio.h>
#include <string.h>
#include "client.h"
#include "local_dict.h"
#include "platform_posix.h"
#include "utf.h"

int main(int argc, char **argv)
{
    static uint8_t mem[384 * 1024];
    static char line[VJO_LOCAL_MAX_TEXT + 2];
    VjoConfig cfg;
    PosixPlatform pp = {0};
    VjoPlatform p;
    if (argc != 3) {
        fprintf(stderr, "usage: vjo-word-forms DIR FILENAMES < words.txt\n");
        return 2;
    }
    vjo_config_defaults(&cfg);
    if (strlen(argv[1]) >= sizeof(cfg.local_dictionary_dir) || strlen(argv[2]) >= sizeof(cfg.local_dictionaries))
        return 2;
    strcpy(cfg.local_dictionary_dir, argv[1]);
    strcpy(cfg.local_dictionaries, argv[2]);
    posix_platform_init(&pp, &p);
    while (fgets(line, sizeof(line), stdin)) {
        size_t n = strcspn(line, "\r\n");
        VjoArena a;
        VjoOverlayData d = {0};
        line[n] = 0;
        vjo_arena_init(&a, mem, sizeof(mem));
        d.ocr_text = d.filtered = d.sentence = line;
        if (!n || vjo_overlay_lookup_at(&a, &p, &cfg, 0, &d)) {
            if (n && d.err.rc) {
                fprintf(stderr, "%s: %s\n", line, vjo_err_text(&a, d.failed_stage, &d.err));
                return 1;
            }
            puts("-");
            continue;
        }
        if (!d.list.n_entries) {
            puts("-");
            continue;
        }
        const VjoEntry *e = &d.list.entries[0];
        int len16 = 0;
        for (size_t i = (size_t)e->hl_start; i < (size_t)e->hl_end;)
            len16 += vjo_utf16_units(vjo_utf8_next(line, n, &i));
        printf("%s\t%s\t%d\t%d\n", e->vocab->spelling, e->vocab->reading, e->vocab->rank, len16);
    }
    return 0;
}
