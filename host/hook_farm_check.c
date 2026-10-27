/* Rubi's text for hook bytes a farm recorded (tools/hook_farm.py check): each stdin line
 *   TITLE <tab> OFFSET <tab> APP_VER <tab> RAW [<tab> FARM]
 * (the verified entry's segment-0 offset and the game's APP_VER, both hex; RAW the bytes the farm read at
 * the entry's register, hex: a cell record's cells for VJO_PLACE_READ_CELLS; FARM, hex of UTF-8, the farm's
 * text) gives one stdout line
 *   STATUS <tab> TEXT <tab> SHOWN <tab> FARM_SHOWN
 * with TEXT (hex of UTF-8) what the game worker would show for that hit: the capture as game_ocr/hooks.c
 * site_hit makes it (at most VJO_HOOK_RAW_BYTES, up to the first NUL unit of the entry's encoding, or the
 * entry's own read), then vjo_hooks_text's path (the entry's filter, or the Luna site's at its address, or
 * the decoder; then vjo_hook_clean). STATUS: ok, noentry (no verified entry of TITLE at OFFSET), empty (no
 * text: the filter dropped it) or cut (the capture was cut at VJO_HOOK_RAW_BYTES). SHOWN and FARM_SHOWN are
 * TEXT and FARM normalised as the worker normalises every text it shows (core/textnorm.h, every rule:
 * full-width ASCII as ASCII, the dashes, the ellipses...), so a form the overlay never shows compares equal.
 *
 * The same functions the worker links (core/hook_place.c, core/hook_luna.c, core/hook_re.c,
 * core/hook_text.c, core/textnorm.c), built for the host. A development tool: host/tests/test_hooks_engine.c runs the
 * worker's own capture on the committed excerpts (host/tests/fixtures/hook_farm.inc). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/hook_luna.h"
#include "../core/hook_place.h"
#include "../core/hook_text.h"
#include "../core/textnorm.h"

#define RAW_BYTES 512u   /* VJO_HOOK_RAW_BYTES (game_ocr/hooks.h) */
#define TEXT_CAP 4096u   /* the shown text's room (game_ocr/main.c hook_show) is larger than any hit's text */

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t unhex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (s[0] && s[1] && n < cap) {
        int hi = hexval(s[0]), lo = hexval(s[1]);
        if (hi < 0 || lo < 0) break;
        out[n++] = (uint8_t)(hi << 4 | lo);
        s += 2;
    }
    return n;
}

int main(void)
{
    static char line[1u << 16];
    static uint8_t mem[1u << 15];
    static char out[TEXT_CAP], farm[TEXT_CAP];
    while (fgets(line, sizeof line, stdin)) {
        char *f[5] = {0}, *p = line;
        const VjoPlaceVerified *ver = NULL, *all;
        const VjoLunaSite *sites, *same;
        unsigned nv, nl, unit, appv, off, n, cut = 0;
        size_t m, len;
        for (unsigned i = 0; i < 5 && p; i++) {
            f[i] = p;
            p = strpbrk(p, "\t\n");
            if (p) *p++ = 0;
        }
        if (!f[3]) continue;
        off = (unsigned)strtoul(f[1], NULL, 16);
        appv = (unsigned)strtoul(f[2], NULL, 16);
        nv = vjo_place_verified(f[0], &all);
        for (unsigned i = 0; i < nv; i++)
            if (all[i].off == off) ver = &all[i];
        if (!ver) {
            printf("noentry\t\t\t\n");
            continue;
        }
        m = unhex(f[3], mem, sizeof mem);
        unit = ver->encoding == VJO_HOOK_ENC_UTF16LE ? 2u : 1u;
        if (ver->read == VJO_PLACE_READ_CELLS) { /* the farm read the record's count of cells: these */
            unsigned cells = (unsigned)(m / 2u);
            if (cells > RAW_BYTES / 2u) cells = RAW_BYTES / 2u, cut = 1;
            n = vjo_place_cells(mem, cells);
        } else { /* site_hit: the string at the register + padding, at most RAW_BYTES, after its first NUL unit not */
            n = 0;
            while (n + unit <= m && n + unit <= RAW_BYTES) {
                if (mem[n] == 0 && (unit == 1 || mem[n + 1] == 0)) break;
                n += unit;
            }
            if (n + unit > RAW_BYTES && n < m) cut = 1;
        }
        nl = vjo_luna_sites(f[0], &sites);
        /* no segment-0 dump here: segment 0 at the farm's 0x81000000 as large as an address can say, so Luna's
         * VJO_LUNA_SEG0 reading decides, as it did before vjo_luna_offset (no farm entry is at an address that only
         * the other reading places) */
        same = vjo_luna_site_at(sites, nl, ver->off, (uint16_t)appv, 0x81000000u, 0xFFFFFFFFu);
        if (ver->filter)
            len = vjo_luna_text_filter(same, ver->filter, ver->encoding, mem, n, ver->encoding, out, sizeof out);
        else if (same)
            len = vjo_luna_text(same, mem, n, ver->encoding, out, sizeof out);
        else
            len = vjo_hook_decode(ver->encoding, mem, n, out, sizeof out, NULL);
        len = vjo_hook_clean(out, len);
        printf("%s\t", !len ? "empty" : cut ? "cut" : "ok");
        for (size_t i = 0; i < len; i++) printf("%02x", (unsigned char)out[i]);
        vjo_text_normalize(out, NULL);
        printf("\t");
        for (size_t i = 0; out[i]; i++) printf("%02x", (unsigned char)out[i]);
        m = f[4] ? unhex(f[4], (uint8_t *)farm, sizeof farm - 1) : 0;
        farm[m] = 0;
        vjo_text_normalize(farm, NULL);
        printf("\t");
        for (size_t i = 0; farm[i]; i++) printf("%02x", (unsigned char)farm[i]);
        printf("\n");
    }
    return 0;
}
