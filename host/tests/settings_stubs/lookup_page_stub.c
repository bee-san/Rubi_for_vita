/* The Look up page (settings/lookup_page.c, #313) is not what the Games and OCR-page harnesses drive: it has its own
 * suite (host/tests/test_settings_lookup.c, on a temporary card with a dictionary and a history.csv). Here it draws
 * its frame's header only (the tabs, for those suites' checks) and holds nothing. */
#include "../../../settings/lookup_page.h"
#include "../../../core/utf.h"
#include <vita2d.h>

int vjo_lookup_stub_shows, vjo_lookup_stub_leaves; /* the page shown and left, as main.c tells it */
/* With a count set, each frame also draws (glyphs) and measures (measures: one a call, or, negative, in one string)
 * that many characters it never drew or measured before, from U+4E00 and U+3400 on, as the page draws and lays out the kanji of entries and earlier
 * lines: main.c's glyph atlases (glyphs_for, measured_for, font_tick). */
int vjo_lookup_stub_glyphs, vjo_lookup_stub_measures;
void vjo_lookup_page_show(VjoArena *scratch) { (void)scratch; vjo_lookup_stub_shows++; }
int vjo_lookup_page_frame(const VjoLookupUi *ui, const VjoLookupInput *in)
{
    static uint32_t next_drawn = 0x4E00, next_measured = 0x3400;
    static char measured[4 * 2048 + 1];
    size_t n = 0;
    (void)in;
    /* measures > 0: that many single characters, one a call (as the page's char_width); < 0: one string of that many */
    for (int i = 0; i < vjo_lookup_stub_measures; i++) {
        char s[8];
        s[vjo_utf8_encode(next_measured++, s)] = '\0';
        ui->text_width(24, s);
    }
    for (int i = 0; i < -vjo_lookup_stub_measures && n + 4 < sizeof(measured); i++)
        n += (size_t)vjo_utf8_encode(next_measured++, measured + n);
    measured[n] = '\0';
    if (n)
        ui->text_width(24, measured);
    vita2d_start_drawing();
    vita2d_clear_screen();
    ui->header();
    for (int i = 0; i < vjo_lookup_stub_glyphs; i++) {
        char s[8];
        s[vjo_utf8_encode(next_drawn++, s)] = '\0';
        ui->text(42, 200, ui->white, 24, s);
    }
    vita2d_end_drawing();
    vita2d_swap_buffers();
    return 0;
}
int vjo_lookup_page_holds_cancel(void) { return 0; }
void vjo_lookup_page_leave(void) { vjo_lookup_stub_leaves++; }
void vjo_lookup_page_quit(void) {}
