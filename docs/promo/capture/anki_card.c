/* anki_card: the promo's mined cards through a Rubi checkout's own code, into a real .apkg.
 *
 * For each card (a sentence and the cursor of its word, in code points):
 *   vjo_text_normalize, vjo_overlay_ocr_text, vjo_overlay_lookup_at   the overlay's lookup (as overlay_lookups.c)
 *   vjo_anki_note_from_entry + card_facts' values                     the Shell's note (shell/anki.c do_save,
 *                                                                     card_facts: the game, its title ID and
 *                                                                     platform, MiscInfo, the time to the minute,
 *                                                                     the engine and the Normal size's model)
 *   vjo_queue_encode(picture) + its ID, and <id>.jpg beside it        the Shell's record with its screenshot, as
 *                                                                     vjo_queue_save commits it (shell/anki_queue.c)
 * then the Rubi app's export, vjo_export_run (settings/anki_export.c, core/apkg.c, core/lapis.c), on real
 * files: the Vita file API mapped onto POSIX by host/tests/test_apkg.c (included, its main renamed), the
 * memory card being WORKDIR/ux0:data/... . The screenshot is the game worker's JPEG of the game's frame
 * (host vjo-jpeg: core/jpegsw.c at the worker's quality, 70; capture/run.sh makes it).
 *
 *   anki_card DICT_DIR DICT_FILES WORKDIR JPEG "SENTENCE" CURSOR_CP ["SENTENCE" CURSOR_CP ...]
 *
 * Prints one JSON object: the cards' IDs and the export's result line (rc, notes, media, bytes, name).
 * (from promo-assets' src/promo_anki.c, for main after #150's screenshots and #153's dictionary title) */
#include "test_apkg_lib.c"

#include "client.h"
#include "platform_posix.h"
#include "textnorm.h"
#include "utf.h"
#include "vjo_vocr_sizes.h"

static uint8_t lookup_mem[4u << 20];

static size_t byte_of_cp(const char *s, size_t cp)
{
    size_t i = 0, n = 0, len = strlen(s);
    while (n < cp && i < len) {
        vjo_utf8_next(s, len, &i);
        n++;
    }
    return i;
}

static void json_out(const char *s)
{
    for (; s && *s; s++)
        if (*s == '"' || *s == '\\')
            printf("\\%c", *s);
        else if (*s == '\n')
            printf("\\n");
        else
            putchar(*s);
}

int main(int argc, char **argv)
{
    VjoConfig cfg;
    PosixPlatform pp = {0};
    VjoPlatform p;
    VjoExportResult r;
    char msg[300];
    const char *picture;
    if (argc < 7 || (argc - 5) % 2) {
        fprintf(stderr, "usage: anki_card DICT_DIR DICT_FILES WORKDIR JPEG SENTENCE CURSOR_CP ...\n");
        return 2;
    }
    vjo_config_defaults(&cfg);
    snprintf(cfg.local_dictionary_dir, sizeof(cfg.local_dictionary_dir), "%s", argv[1]);
    snprintf(cfg.local_dictionaries, sizeof(cfg.local_dictionaries), "%s", argv[2]);
    posix_platform_init(&pp, &p);
    picture = argv[4];
    if (chdir(argv[3]) < 0)
        return 2;
    mkdir("ux0:data", 0777);
    mkdir("ux0:data/VitaJPOverlay", 0777);
    mkdir(VJO_EXPORT_QUEUE_DIR, 0777);
    memset(&io, 0, sizeof(io));
    io.write_after = -1;
    io.fd = -1;
    /* the console's clock (MiscInfo, the package's name): 2026-10-16 19:42:10 local */
    clock_local = (SceDateTime){2026, 10, 16, 19, 42, 10, 0};
    printf("{\"cards\":[");
    for (int argi = 5, k = 0; argi + 1 < argc; argi += 2, k++) {
        static char text[4096];
        VjoArena la;
        VjoOverlayData d;
        VjoAnkiNote note;
        char misc[160], jpg[256], cmd[700];
        const char *json, *id;
        VjoAnkiDate when = {2026, 10, 16, 19, 42 + k};
        snprintf(text, sizeof(text), "%s", argv[argi]);
        vjo_text_normalize(text, NULL); /* as the game worker returns it */
        vjo_arena_init(&la, lookup_mem, sizeof(lookup_mem));
        if (vjo_overlay_ocr_text(&la, &cfg, text, &d) ||
            vjo_overlay_lookup_at(&la, &p, &cfg, byte_of_cp(d.sentence, (size_t)atoi(argv[argi + 1])), &d) ||
            d.list.n_entries != 1) {
            fprintf(stderr, "no entry at %s in %s\n", argv[argi + 1], argv[argi]);
            return 1;
        }
        vjo_arena_init(&A, mem, sizeof(mem));
        if (vjo_anki_note_from_entry(&A, &d.list, 0, &note)) {
            fprintf(stderr, "no note\n");
            return 1;
        }
        /* card_facts (shell/anki.c) for the promo's game, as the Shell knows it after a recognition with
         * the Normal size (the default): worker.c vjo_anki_set_ocr_model(vjo_vocr_tier_model(ran)) */
        note.game = "夕凪ステップ";
        note.title_id = "PROMO0001";
        note.platform = "vita";
        note.ocr_model = vjo_vocr_tier_model(VJO_VOCR_TIER_DEFAULT);
        vjo_anki_misc_info(misc, sizeof(misc), note.game, &when);
        note.misc = misc;
        note.ocr_engine = "vocr";
        note.unix_time = (int64_t)1792179720 + 60 * k; /* 2026-10-16 19:42 UTC + k minutes, to the minute */
        if (!(json = vjo_queue_encode(&A, &note, 1))) {
            fprintf(stderr, "record too large\n");
            return 1;
        }
        id = queue_json(json, 0); /* <id>.json, its ID from its own bytes (vjo_queue_id) */
        snprintf(jpg, sizeof(jpg), "%s/%s.jpg", VJO_EXPORT_QUEUE_DIR, id);
        snprintf(cmd, sizeof(cmd), "cp '%s' '%s'", picture, jpg);
        if (system(cmd) != 0)
            return 1;
        printf("%s{\"id\":\"%s\",\"spelling\":\"", k ? "," : "", id);
        json_out(note.spelling);
        printf("\",\"dictionary\":\"");
        json_out(note.dictionary);
        printf("\",\"ocr_model\":\"");
        json_out(note.ocr_model);
        printf("\",\"record\":\"%s/%s.json\"}", VJO_EXPORT_QUEUE_DIR, id);
    }
    vjo_arena_init(&A, mem, sizeof(mem));
    vjo_export_run(&A, 0, NULL, &ui, &r, msg, sizeof(msg));
    printf("],\"rc\":%d,\"notes\":%d,\"media\":%d,\"bytes\":%u,\"path\":\"%s\",\"name\":\"%s\",\"message\":\"", r.rc,
           r.cards, r.media, (unsigned)r.bytes, r.path, r.name);
    json_out(msg);
    printf("\",\"log\":\"");
    json_out(logs);
    printf("\"}\n");
    return r.rc ? 1 : 0;
}
