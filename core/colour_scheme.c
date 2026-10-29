#include "colour_scheme.h"
#include "config.h"
#include "styled.h"
#include "utf.h"
#include "port.h"
#include <string.h>

const char *const vjo_colour_scheme_names[] VJO_DATA_TABLE(colour_scheme_names) = {"default", "mint", "paper", NULL};

__attribute__((noinline)) int vjo_colour_scheme_parse(const char *text, size_t len)
{
    int scheme = VJO_THEME_DEFAULT;
    if (!text || memchr(text, 0, len)) return scheme;
    for (size_t i = 0; i < len;) {
        size_t at = i, n, s, e, k;
        while (i < len && text[i] != '\n') i++;
        n = i++ - at;
        const char *line = text + at, *eq;
        s = vjo_config_line_start(line, n);
        if (s == n || line[s] == ';' || line[s] == '#' || !(eq = memchr(line + s, '=', n - s))) continue;
        k = (size_t)(eq - line);
        while (k > s && (line[k-1] == ' ' || line[k-1] == '\t' || line[k-1] == '\r')) k--;
        if (k - s != 13 || !vjo_ieq_prefix(line + s, "colour_scheme")) continue;
        s = (size_t)(eq - line) + 1;
        while (s < n && (line[s] == ' ' || line[s] == '\t' || line[s] == '\r')) s++;
        for (e = s; e < n; e++)
            if ((line[e] == ';' || line[e] == '#') && e > s && (line[e-1] == ' ' || line[e-1] == '\t' || line[e-1] == '\r')) break;
        while (e > s && (line[e-1] == ' ' || line[e-1] == '\t' || line[e-1] == '\r')) e--;
        for (int j = 0; j < VJO_THEMES; j++)
            if (e - s == strlen(vjo_colour_scheme_names[j]) && vjo_ieq_prefix(line + s, vjo_colour_scheme_names[j])) scheme = j;
    }
    return scheme;
}

/* One remapper, including under LTO: not a copy at every semantic text role. */
__attribute__((noinline)) uint32_t vjo_colour_scheme_rgb(int scheme, uint32_t rgb)
{
    if (scheme == VJO_THEME_MINT && rgb == VJO_RGB_HIGHLIGHT) return VJO_MINT_ACCENT;
    if (scheme == VJO_THEME_PAPER) {
        static const uint32_t roles[][2] VJO_DATA_TABLE(colour_scheme_paper) = {
            {0, VJO_PAPER_BG}, {0x08080Au, VJO_PAPER_BG},
            {VJO_RGB_TEXT, VJO_PAPER_TEXT}, {VJO_RGB_HIGHLIGHT, VJO_PAPER_HIGHLIGHT},
            {VJO_RGB_READING, VJO_PAPER_READING}, {VJO_RGB_RANK, VJO_PAPER_DIM},
            {VJO_RGB_DIM, VJO_PAPER_DEFINITION}, {VJO_RGB_ERROR, VJO_PAPER_ERROR},
            {VJO_RGB_OK, VJO_PAPER_ACCENT}
        };
        for (unsigned i = 0; i < sizeof(roles)/sizeof(roles[0]); i++)
            if (rgb == roles[i][0]) return roles[i][1];
    }
    return rgb;
}
