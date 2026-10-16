/* Which line breaks of recognized texts the overlay joins (core/textjoin.h)
 * and which words its lookup then finds across them (tools/cross_line_eval.py,
 * docs/touch-lookup.md "Words split over two lines"). One text per stdin
 * line, its lines separated by an escaped "\n" ("\\" for a backslash); one
 * output line per text, one item per break, space separated:
 *
 *   J:-          J 1 when the break joins its lines, else 0; no word crosses it
 *   1:S-E:WORD   the Shell's lookup (vjo_overlay_lookup_at, the release
 *                defaults) segmenting the line before it from its start,
 *                longest match first, finds WORD (the entry's spelling) over
 *                code points S..E of the text, the break among them
 *
 *   vjo-cross-lines DIR FILENAMES < texts.txt
 */
#include <stdio.h>
#include <string.h>
#include "client.h"
#include "local_dict.h"
#include "platform_posix.h"
#include "textjoin.h"
#include "utf.h"

static size_t cp_index(const char *s, size_t byte)
{
    size_t i = 0, n = 0, len = strlen(s);
    while (i < byte && i < len) {
        vjo_utf8_next(s, len, &i);
        n++;
    }
    return n;
}

int main(int argc, char **argv)
{
    static uint8_t mem[384 * 1024];
    static char in[2 * VJO_LOCAL_MAX_TEXT + 2], text[VJO_LOCAL_MAX_TEXT + 1];
    VjoConfig cfg;
    PosixPlatform pp = {0};
    VjoPlatform p;
    if (argc != 3) {
        fprintf(stderr, "usage: vjo-cross-lines DIR FILENAMES < texts.txt\n");
        return 2;
    }
    vjo_config_defaults(&cfg);
    cfg.dictionary = VJO_DICT_LOCAL;
    if (strlen(argv[1]) >= sizeof(cfg.local_dictionary_dir) || strlen(argv[2]) >= sizeof(cfg.local_dictionaries))
        return 2;
    strcpy(cfg.local_dictionary_dir, argv[1]);
    strcpy(cfg.local_dictionaries, argv[2]);
    posix_platform_init(&pp, &p);
    while (fgets(in, sizeof(in), stdin)) {
        size_t n = 0, len, line = 0;
        int first = 1;
        for (size_t i = 0; in[i] && in[i] != '\n' && in[i] != '\r' && n < VJO_LOCAL_MAX_TEXT; i++) {
            if (in[i] == '\\' && (in[i + 1] == 'n' || in[i + 1] == '\\'))
                text[n++] = in[++i] == 'n' ? '\n' : '\\';
            else
                text[n++] = in[i];
        }
        text[n] = 0;
        len = n;
        for (size_t nl = 0; nl < len; nl++) {
            size_t at, cross_start = 0, cross_end = 0;
            const char *word = NULL;
            if (text[nl] != '\n')
                continue;
            /* the line before the break, from its start: each word the lookup shows, then the next */
            for (at = line; at < nl;) {
                VjoArena a;
                VjoOverlayData d = {0};
                size_t next = at;
                vjo_arena_init(&a, mem, sizeof(mem));
                d.ocr_text = d.filtered = d.sentence = text;
                if (vjo_overlay_lookup_at(&a, &p, &cfg, at, &d)) {
                    fprintf(stderr, "lookup failed: %s\n", vjo_err_text(&a, d.failed_stage, &d.err));
                    return 1;
                }
                if (d.list.n_entries && (size_t)d.list.entries[0].hl_end > at)
                    next = (size_t)d.list.entries[0].hl_end;
                else
                    vjo_utf8_next(text, len, &next);
                if (next > nl) {
                    static char spelling[256];
                    snprintf(spelling, sizeof(spelling), "%s", d.list.entries[0].vocab->spelling);
                    for (char *c = spelling; *c; c++)
                        if (*c == ' ' || *c == '\n' || *c == '\t')
                            *c = '_';
                    word = spelling;
                    cross_start = at;
                    cross_end = next;
                    break;
                }
                at = next;
            }
            printf("%s%d:", first ? "" : " ", vjo_text_joins(text, len, nl));
            if (word)
                printf("%zu-%zu:%s", cp_index(text, cross_start), cp_index(text, cross_end), word);
            else
                printf("-");
            first = 0;
            line = nl + 1;
        }
        printf("\n");
    }
    return 0;
}
