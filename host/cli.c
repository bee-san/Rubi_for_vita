/* vjo-cli: runs Rubi's lookup pipeline on the Mac, on text (the
 * game worker recognizes the Vita's screen; on a computer vjo-ocr does).
 *
 *   vjo-cli --text "日本語" [--dict local] [--config config.ini]
 *           [--filter lines|none] [--nav] [-v]
 *   vjo-cli --print-default-config */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "platform_posix.h"
#include "render.h"
#include "replay.h"

static void usage(void)
{
    fprintf(stderr,
            "usage: vjo-cli --text TEXT [--dict local] [--config FILE]\n"
            "               [--local-dir DIR] [--local-dicts main.vjdict,extra.vjdict]\n"
            "               [--filter lines|none] [--nav] [--stats] [-v]\n"
            );
    exit(2);
}

int main(int argc, char **argv)
{
    static uint8_t mem[8u << 20], filemem[8u << 20];
    VjoArena a, fa; /* fa holds input files so a.peak is what the Vita needs */
    VjoConfig cfg;
    PosixPlatform pp = {0};
    VjoPlatform plat;
    VjoOverlayData d;
    const char *text = NULL, *config = NULL, *dict = NULL;
    const char *local_dir = NULL, *local_dicts = NULL;
    int nav = 0, stats = 0, rc;

    memset(mem, 0xA5, sizeof(mem)); /* like a reused arena on the Vita */
    vjo_arena_init(&a, mem, sizeof(mem));
    vjo_arena_init(&fa, filemem, sizeof(filemem));
    vjo_config_defaults(&cfg);
    for (int i = 1; i < argc; i++) {
        const char *s = argv[i];
        int more = i + 1 < argc;
        if (!strcmp(s, "--config") && more)
            config = argv[++i];
        else if (!strcmp(s, "--dict") && more)
            dict = argv[++i];
        else if (!strcmp(s, "--local-dir") && more)
            local_dir = argv[++i];
        else if (!strcmp(s, "--local-dicts") && more)
            local_dicts = argv[++i];
        else if (!strcmp(s, "--filter") && more)
            cfg.non_japanese_filter = strcmp(argv[++i], "none") ? 1 : 0;
        else if (!strcmp(s, "--text") && more)
            text = argv[++i];
        else if (!strcmp(s, "--nav"))
            nav = 1;
        else if (!strcmp(s, "--stats"))
            stats = 1;
        else if (!strcmp(s, "-v"))
            pp.verbose = 1;
        else if (!strcmp(s, "--print-default-config")) {
            fputs(vjo_config_default_text(), stdout);
            return 0;
        }
        else
            usage();
    }
    if (config) {
        size_t len;
        char *ini = vjo_read_file(&fa, config, &len);
        if (!ini) {
            fprintf(stderr, "cannot read %s\n", config);
            return 1;
        }
        vjo_config_parse(&cfg, ini, len);
        for (int i = 0; i < cfg.n_warnings; i++)
            fprintf(stderr, "config: %s\n", cfg.warnings[i]);
    }
    if (dict && strcmp(dict, "local")) /* the local dictionaries are the only ones */
        usage();
    if (local_dir) {
        if (strlen(local_dir) >= sizeof(cfg.local_dictionary_dir)) usage();
        snprintf(cfg.local_dictionary_dir, sizeof(cfg.local_dictionary_dir), "%s", local_dir);
    }
    if (local_dicts) {
        if (strlen(local_dicts) >= sizeof(cfg.local_dictionaries)) usage();
        snprintf(cfg.local_dictionaries, sizeof(cfg.local_dictionaries), "%s", local_dicts);
    }

    posix_platform_init(&pp, &plat);
    memset(&d, 0, sizeof(d));

    if (!text)
        usage();
    vjo_overlay_from_text(&a, &plat, &cfg, text, &d);

    rc = d.err.rc ? 1 : 0;
    fputs(vjo_render_overlay(&a, &d), stdout);
    if (nav) {
        printf("\n[navigation]\n");
        for (int i = 0; i < d.list.n_entries; i++)
            printf("%d: %s\n", i + 1, vjo_render_highlight(&a, &d.list, i));
    }
    if (stats)
        fprintf(stderr, "arena peak: %lu bytes\n", (unsigned long)a.peak);
    return rc;
}
