/* The bundled-dictionary lookup at every Japanese letter of the real VOCR
 * lines (host/fixtures/word_lookup/vocr_lines.tsv), one result line each:
 * the cursor, rc, the entry's spelling and reading, its source span and a
 * hash of its definition. tools/arm_host_tests.sh builds it for the Vita's
 * CPU (32-bit ARMv7, qemu-arm) and natively, and requires identical output:
 * the ranking, its offsets and its spans do not depend on pointer width.
 * Only core/local_dict.c, utf.c and arena.c are linked; files are stdio.
 *
 *   word_lookup_parity DICT_DIR vocr_lines.tsv */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "local_dict.h"
#include "utf.h"

static uint8_t arena_mem[128 * 1024];

static int file_read(void *ctx, uint64_t off, void *dst, size_t n)
{
    FILE *f = ctx;
    if (fseeko(f, (off_t)off, SEEK_SET) || fread(dst, 1, n, f) != n) return -1;
    return 0;
}
static void file_close(void *ctx) { fclose(ctx); }
static int file_open(void *ud, const char *path, VjoFile *out)
{
    FILE *f = fopen(path, "rb");
    (void)ud;
    if (!f || fseeko(f, 0, SEEK_END)) { if (f) fclose(f); return -1; }
    *out = (VjoFile){f, (uint64_t)ftello(f), file_read, file_close, 0};
    return 0;
}

static uint32_t fnv(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

int main(int argc, char **argv)
{
    static char line[16384], text[16384];
    VjoConfig cfg;
    VjoPlatform p;
    FILE *in;
    unsigned lookups = 0, entries = 0;
    if (argc != 3 || !(in = fopen(argv[2], "r"))) {
        fprintf(stderr, "usage: word_lookup_parity DICT_DIR vocr_lines.tsv\n");
        return 2;
    }
    memset(&cfg, 0, sizeof(cfg));
    memset(&p, 0, sizeof(p));

    snprintf(cfg.local_dictionary_dir, sizeof(cfg.local_dictionary_dir), "%s", argv[1]);
    snprintf(cfg.local_dictionaries, sizeof(cfg.local_dictionaries), "plaintext.vjdict");
    p.file_open = file_open;
    while (fgets(line, sizeof(line), in)) {
        char *t = line, *o = text;
        size_t len, cursor;
        if (line[0] == '#') continue;
        for (int tabs = 0; tabs < 5 && t; tabs++) t = strchr(t, '\t') ? strchr(t, '\t') + 1 : NULL;
        if (!t) continue;
        for (; *t && *t != '\n' && *t != '\r'; t++) {
            if (t[0] == '\\' && t[1] == 'n') { *o++ = '\n'; t++; }
            else *o++ = *t;
        }
        *o = 0;
        len = strlen(text);
        for (cursor = vjo_utf8_japanese_cursor_step(text, len, 0, 0); cursor < len;) {
            VjoArena a;
            VjoDictResult r;
            VjoErr e;
            size_t next;
            int rc;
            vjo_arena_init(&a, arena_mem, sizeof(arena_mem));
            rc = vjo_local_lookup_at(&a, &p, &cfg, text, cursor, &r, &e);
            lookups++;
            if (!rc && r.n_vocab == 1) {
                entries++;
                printf("%zu rc=0 %s|%s pos16=%d len16=%d def=%08x\n", cursor, r.vocab[0].spelling,
                       r.vocab[0].reading, r.tokens[0].pos16, r.tokens[0].len16, (unsigned)fnv(r.vocab[0].meanings[0]));
            } else {
                printf("%zu rc=%d none\n", cursor, rc);
            }
            if ((rc || !r.n_vocab) && a.used) { fprintf(stderr, "arena not released\n"); return 1; }
            next = vjo_utf8_cursor_step(text, len, cursor, 1);
            next = next > cursor ? vjo_utf8_japanese_cursor_step(text, len, next, 0) : len;
            if (next <= cursor) break;
            cursor = next;
        }
    }
    fclose(in);
    fprintf(stderr, "%u lookups, %u entries\n", lookups, entries);
    return 0;
}
