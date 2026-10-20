/* overlay_lookups: the overlay's own lookups for the promo's lines, as JSON (one object per line).
 *
 * For each stdin line (a dialogue text; "\n" written as the two characters \n) it runs the code the
 * Shell runs in local cursor mode, unchanged (shell/worker.c, shell/overlay.cpp):
 *   vjo_text_normalize            the game worker's normalisation of the recognized text
 *   vjo_overlay_ocr_text          recognized text -> filtered text and the header's sentence
 *   vjo_utf8_japanese_cursor_step the overlay's first cursor (Japanese-only selection, the film's setting)
 *   vjo_overlay_lookup_at         the lookup at a cursor (an arrow key or a tap)
 *   vjo_text_row_key(dx = +1)     Right as the overlay's arrows move (shell/overlay.cpp): along the
 *                                 header's displayed row, wrapping at its ends (#87), over the rows
 *                                 of main's own renderer double (host/tests/layout_double.h: a
 *                                 920 px header, 27 px Japanese cells, a line per '\n')
 *   vjo_styled_entry              the popup's entry exactly as the body draws it: its text, colour and
 *                                 size per span (the default font sizes and frequency badge)
 * and prints the sentence, "rows" (the header's displayed rows), "walk": Right from the first cursor,
 * one step per press (its VJO_ROW_* result; a lookup at each new cursor; it stops when Right stays or
 * comes back to a cursor it has been at), and "taps": the lookup at each Japanese character, as a tap
 * on it looks up (shell/overlay.cpp input_header_touch: the tapped character is the cursor).
 * Offsets are given in bytes, code points and UTF-16 units.
 *
 *   overlay_lookups DICT_DIR DICT_FILES < lines.txt > lookups.jsonl
 *
 * Built against a Rubi checkout's host libraries (capture/run.sh):
 *   cc -O1 -DVJO_HOST=1 -I core -I host -I host/tests -I include overlay_lookups.c core/textnorm.c \
 *      build/host/libvjo_host.a build/host/libvjo_core.a -lm -o overlay_lookups
 * (from promo-assets' src/promo_lookup.c, for main after #153's kanji lines, #156's hint and #87's rows) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "config.h"
#include "layout_double.h"
#include "entries.h"
#include "platform_posix.h"
#include "styled.h"
#include "text_rows.h"
#include "textnorm.h"
#include "utf.h"

static void json_str_n(const char *s, size_t n)
{
    putchar('"');
    for (size_t i = 0; s && i < n && s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\')
            printf("\\%c", c);
        else if (c == '\n')
            fputs("\\n", stdout);
        else if (c == '\r')
            fputs("\\r", stdout);
        else if (c == '\t')
            fputs("\\t", stdout);
        else if (c < 0x20)
            printf("\\u%04x", c);
        else
            putchar(c);
    }
    putchar('"');
}

static void json_str(const char *s)
{
    json_str_n(s, s ? strlen(s) : 0);
}

static size_t cp_index(const char *s, size_t byte)
{
    size_t i = 0, n = 0, len = strlen(s);
    while (i < byte && i < len) {
        vjo_utf8_next(s, len, &i);
        n++;
    }
    return n;
}

/* UTF-16 units [start, start + len) of t -> UTF-8, into out (cap bytes). */
static void utf16_to_utf8(const uint16_t *t, uint32_t start, uint32_t len, char *out, size_t cap)
{
    size_t o = 0;
    for (uint32_t i = start; i < start + len && o + 5 < cap; i++) {
        uint32_t cp = t[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < start + len) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (t[i + 1] - 0xDC00);
            i++;
        }
        if (cp < 0x80)
            out[o++] = (char)cp;
        else if (cp < 0x800) {
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out[o++] = (char)(0xE0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | (cp >> 18));
            out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[o] = 0;
}

/* One lookup at cursor: prints the JSON object; returns the match end (bytes) or 0. */
static size_t lookup(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg, VjoOverlayData *d, size_t cursor)
{
    size_t mark = vjo_arena_mark(a), end = 0;
    const char *s = d->sentence;
    int rc = vjo_overlay_lookup_at(a, p, cfg, cursor, d);
    printf("{\"cursor_byte\":%zu,\"cursor_cp\":%zu,\"cursor_u16\":%u", cursor, cp_index(s, cursor),
           vjo_utf16_index(s, cursor));
    if (rc) {
        printf(",\"error\":");
        json_str(vjo_err_text(a, d->failed_stage, &d->err));
    } else if (!d->list.n_entries) {
        printf(",\"entry\":null");
    } else {
        const VjoEntry *e = &d->list.entries[0];
        const VjoVocab *v = e->vocab;
        char badge[VJO_RANK_BADGE_MAX];
        static char span[16384];
        VjoStyled body;
        end = (size_t)e->hl_end;
        printf(",\"hl_byte\":[%d,%d],\"hl_cp\":[%zu,%zu],\"hl_u16\":[%u,%u],\"matched\":", e->hl_start, e->hl_end,
               cp_index(s, (size_t)e->hl_start), cp_index(s, (size_t)e->hl_end), vjo_utf16_index(s, (size_t)e->hl_start),
               vjo_utf16_index(s, (size_t)e->hl_end));
        json_str_n(s + e->hl_start, (size_t)(e->hl_end - e->hl_start));
        printf(",\"spelling\":");
        json_str(v->spelling);
        printf(",\"reading\":");
        json_str(v->reading);
        printf(",\"dictionary\":");
        json_str(v->dictionary ? v->dictionary : "");
        printf(",\"rank\":%d,\"badge\":", v->rank);
        json_str(vjo_rank_badge(badge, v->rank, d->list.source));
        printf(",\"meanings\":[");
        for (int i = 0; i < v->n_meanings; i++) {
            if (i)
                putchar(',');
            json_str(v->meanings[i]);
        }
        printf("]");
        /* the popup's entry, as overlay.cpp render() draws it: vjo_styled_entry with the default
         * font sizes (vjo_font_px of font_size_ja / font_size_en), no in-Anki mark (AnkiConnect is
         * gone), the frequency badge as configured (frequency_badge = on by default) */
        if (vjo_styled_init(&body, a, VJO_BODY_UNITS, VJO_BODY_SPANS) == 0) {
            vjo_styled_entry(&body, &d->list, 0, vjo_font_px(cfg->font_size_ja), vjo_font_px(cfg->font_size_en), 0,
                             cfg->frequency_badge);
            printf(",\"entry_truncated\":%d,\"entry_spans\":[", body.truncated);
            for (int i = 0; i < body.n_spans; i++) {
                const VjoSpan *sp = &body.spans[i];
                utf16_to_utf8(body.text, sp->start, sp->len, span, sizeof(span));
                printf("%s{\"text\":", i ? "," : "");
                json_str(span);
                printf(",\"rgb\":\"#%06x\",\"px\":%d}", (unsigned)sp->rgb, sp->px);
            }
            printf("]");
        }
    }
    putchar('}');
    vjo_arena_release(a, mark);
    return end;
}

int main(int argc, char **argv)
{
    static uint8_t mem[4u << 20];
    static char line[8192], text[8192], raw[8192];
    VjoConfig cfg;
    PosixPlatform pp = {0};
    VjoPlatform p;
    if (argc != 3) {
        fprintf(stderr, "usage: overlay_lookups DIR FILENAMES < lines.txt\n");
        return 2;
    }
    vjo_config_defaults(&cfg);
    cfg.japanese_only_selection = 1; /* the film's: the default until 2026-10-20 (OFF now) */
    snprintf(cfg.local_dictionary_dir, sizeof(cfg.local_dictionary_dir), "%s", argv[1]);
    snprintf(cfg.local_dictionaries, sizeof(cfg.local_dictionaries), "%s", argv[2]);
    posix_platform_init(&pp, &p);
    while (fgets(line, sizeof(line), stdin)) {
        size_t n = strcspn(line, "\r\n"), j = 0;
        VjoArena a;
        VjoOverlayData d;
        const char *s;
        size_t len, cursor;
        line[n] = 0;
        if (!n)
            continue;
        for (size_t i = 0; i < n; i++) { /* "\n" (two characters) -> a newline */
            if (line[i] == '\\' && line[i + 1] == 'n') {
                text[j++] = '\n';
                i++;
            } else
                text[j++] = line[i];
        }
        text[j] = 0;
        memcpy(raw, text, j + 1);
        vjo_text_normalize(text, NULL); /* as the game worker returns it (game_ocr/main.c) */
        vjo_arena_init(&a, mem, sizeof(mem));
        if (vjo_overlay_ocr_text(&a, &cfg, text, &d)) {
            fprintf(stderr, "filter failed: %s\n", line);
            return 1;
        }
        s = d.sentence;
        len = strlen(s);
        printf("{\"input\":");
        json_str(raw);
        printf(",\"normalized\":");
        json_str(text);
        printf(",\"sentence\":");
        json_str(s);
        {
            static Layout lay;
            int ja = vjo_font_px(cfg.font_size_ja);
            VjoTextRows rows;
            VjoRowNav nav;
            size_t seen[64];
            int n_seen = 0, result = VJO_ROW_MOVED;
            layout_text(&lay, s, 920.0f, (float)ja, ja / 2.0f); /* shell/overlay.cpp BOX_W; the header's size */
            rows = layout_rows(&lay);
            printf(",\"font_px\":[%d,%d],\"frequency_badge\":%d,\"rows\":%u,\"walk\":[", ja,
                   vjo_font_px(cfg.font_size_en), cfg.frequency_badge, (unsigned)lay.lines);
            memset(&nav, 0, sizeof(nav));
            cursor = vjo_utf8_japanese_cursor_step(s, len, 0, 0);
            for (int k = 0; cursor < len && k < 64; k++) {
                size_t end, next = cursor;
                int again = 0;
                for (int i = 0; i < n_seen; i++)
                    again |= seen[i] == cursor;
                if (k)
                    printf(",");
                printf("{\"key\":\"%s\",\"result\":\"%s\",\"lookup\":", k ? "Right" : "open",
                       !k ? "open" : result == VJO_ROW_MOVED ? "moved" : result == VJO_ROW_WRAPPED ? "wrapped"
                       : result == VJO_ROW_STAY ? "stay" : "unavailable");
                if (k && (result == VJO_ROW_STAY || again)) { /* the cursor stays, or comes back: no new lookup */
                    printf("null}");
                    break;
                }
                seen[n_seen++] = cursor;
                end = lookup(&a, &p, &cfg, &d, cursor);
                printf("}");
#ifdef VJO_ROW_KEY_STICK /* #173 added the left stick's flag: 0, the d-pad's Right (capture/run.sh sets it) */
                result = vjo_text_row_key(&nav, s, len, &rows, cursor, end, +1, 0, cfg.japanese_only_selection, 0, &next);
#else
                result = vjo_text_row_key(&nav, s, len, &rows, cursor, end, +1, 0, cfg.japanese_only_selection, &next);
#endif
                cursor = next;
            }
        }
        printf("],\"taps\":[");
        {
            size_t i = 0;
            int first = 1;
            while (i < len) {
                size_t at = i;
                uint32_t cp = vjo_utf8_next(s, len, &i);
                if (!vjo_is_japanese_letter(cp))
                    continue;
                printf("%s", first ? "" : ",");
                first = 0;
                lookup(&a, &p, &cfg, &d, at);
            }
        }
        printf("]}\n");
        fflush(stdout);
    }
    return 0;
}
