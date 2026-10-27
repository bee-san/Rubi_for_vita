/* The Look up page (settings/lookup_page.c, #313) is not what the Games and OCR-page harnesses drive: it has its own
 * suite (host/tests/test_settings_lookup.c, on a temporary card with a dictionary and a history.csv). Here it draws
 * its frame's header only (the tabs, for those suites' checks) and holds nothing. */
#include "../../../settings/lookup_page.h"
#include "../../../core/utf.h"
#include <vita2d.h>

int vjo_lookup_stub_shows, vjo_lookup_stub_leaves; /* the page shown and left, as main.c tells it */
/* With a count set, each frame also draws that many characters it never drew before (from U+4E00 on), as the page
 * draws the kanji of entries and earlier lines: main.c's glyph atlas count (font_reload). */
int vjo_lookup_stub_glyphs;
void vjo_lookup_page_show(VjoArena *scratch) { (void)scratch; vjo_lookup_stub_shows++; }
int vjo_lookup_page_frame(const VjoLookupUi *ui, const VjoLookupInput *in)
{
    (void)in;
    static uint32_t next = 0x4E00;
    vita2d_start_drawing();
    vita2d_clear_screen();
    ui->header();
    for (int i = 0; i < vjo_lookup_stub_glyphs; i++) {
        char s[8];
        s[vjo_utf8_encode(next++, s)] = '\0';
        ui->text(42, 200, ui->white, 24, s);
    }
    vita2d_end_drawing();
    vita2d_swap_buffers();
    return 0;
}
int vjo_lookup_page_holds_cancel(void) { return 0; }
void vjo_lookup_page_leave(void) { vjo_lookup_stub_leaves++; }
void vjo_lookup_page_quit(void) {}
