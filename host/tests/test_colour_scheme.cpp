#include "acutest.h"
#include "../../core/styled.h"
#include "../../settings/themes.h"
#include "../../shell/colour_widgets.h"
#include <cstring>

struct Color { uint32_t rgb; float alpha; };
static Color color(uint32_t rgb, float alpha) { return {rgb, alpha}; }
struct Widget { Color tint; int calls = 0; void SetColor(Color c) { tint = c; calls++; } };
struct Page {
    Widget panel, band, strip, separator;
    Widget *FindChild(const char *s) {
        if (!strcmp(s, "vjo_panel")) return &panel;
        if (!strcmp(s, "vjo_band_mid")) return &band;
        if (!strcmp(s, "vjo_strip")) return &strip;
        if (!strcmp(s, "vjo_separator")) return &separator;
        return nullptr; /* a strip page lacks overlay widgets and vice versa */
    }
};
static uint32_t rgb(uint32_t packed) { return ((packed & 255) << 16) | (packed & 0xFF00) | ((packed >> 16) & 255); }
static void paper_surfaces_and_text(void)
{
    Page p;
    vjo_colour_widgets(&p, VJO_THEME_PAPER, color);
    TEST_CHECK(p.panel.calls == 1 && p.panel.tint.rgb == rgb(vjo_app_theme(VJO_THEME_PAPER)->bg));
    TEST_CHECK(p.panel.tint.alpha == 1 && p.strip.tint.alpha == 1 && p.band.tint.alpha == 1);
    TEST_CHECK(p.separator.tint.alpha == 0.3f);
    const VjoAppTheme *a = vjo_app_theme(VJO_THEME_PAPER);
    const uint32_t from[] = {VJO_RGB_TEXT, VJO_RGB_HIGHLIGHT, VJO_RGB_READING, VJO_RGB_RANK, VJO_RGB_DIM, VJO_RGB_ERROR, VJO_RGB_OK};
    const uint32_t to[] = {a->text, a->highlight, a->reading, a->rank, a->definition, a->off, a->accent};
    for (unsigned i = 0; i < sizeof(from)/sizeof(from[0]); i++) TEST_CHECK(vjo_colour_scheme_rgb(VJO_THEME_PAPER, from[i]) == rgb(to[i]));
    TEST_CHECK(p.separator.tint.rgb == rgb(a->text));
}
static void mint_and_default(void)
{
    Page p;
    vjo_colour_widgets(&p, VJO_THEME_DEFAULT, color);
    TEST_CHECK(!p.panel.calls && !p.strip.calls); /* preserve the RCO's original alpha and surfaces */
    vjo_colour_widgets(&p, VJO_THEME_MINT, color);
    TEST_CHECK(!p.panel.calls && !p.strip.calls); /* Mint keeps the resource's dark surfaces too. */
    TEST_CHECK(vjo_colour_scheme_rgb(VJO_THEME_MINT, VJO_RGB_HIGHLIGHT) == rgb(vjo_app_theme(VJO_THEME_MINT)->highlight));
    TEST_CHECK(vjo_colour_scheme_rgb(VJO_THEME_DEFAULT, VJO_RGB_TEXT) == VJO_RGB_TEXT);
    TEST_CHECK(vjo_colour_scheme_rgb(-1, VJO_RGB_ERROR) == VJO_RGB_ERROR);
    TEST_CHECK(vjo_colour_scheme_rgb(VJO_THEME_PAPER, 0x123456) == 0x123456);
}
static void preference_grammar(void)
{
    const struct { const char *text; int want; } cases[] = {
        {"", 0}, {"colour_scheme = mint\n", 1},
        {"\xEF\xBB\xBF\tCOLOUR_SCHEME = PaPeR ; saved\r\nanki_card_type = word\n", 2},
        {"colour_scheme = paper\ncolour_scheme = invalid\n", 2},
        {"colour_scheme = paper\ncolour_scheme = default\n", 0},
        {"; colour_scheme = paper\ncolour_scheme_extra=mint\n", 0},
        {"colour_scheme=paper#bad\n", 0}, {"colour_scheme=mint # comment", 1}, {"colour_scheme=paper\r# comment", 2}
    };
    for (auto &c : cases) TEST_CHECK(vjo_colour_scheme_parse(c.text, strlen(c.text)) == c.want);
    TEST_CHECK(vjo_colour_scheme_parse("colour_scheme=paper\0x", 21) == 0);
    TEST_CHECK(vjo_colour_scheme_parse("colour_scheme=paperX", 19) == VJO_THEME_PAPER); /* length-bounded, no NUL */
    TEST_CHECK(vjo_colour_scheme_parse(nullptr, 0) == VJO_THEME_DEFAULT);
}
TEST_LIST = {
    {"paper surfaces, subtitle opacity and shared text roles", paper_surfaces_and_text},
    {"mint highlight and unchanged default", mint_and_default},
    {"shared preference grammar", preference_grammar},
    {nullptr, nullptr}
};
