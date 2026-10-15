/* The Anki page (settings/anki_page.c) is not what the Settings harness
 * drives: it brings the app's SceNet, AnkiConnect and the card queue, and its
 * logic (core/anki_app.c) has its own suite, anki_app. Here the page does
 * nothing and is never modal or busy, so the harness runs the real main.c's
 * OCR & lookup page. */
#include "../../../settings/anki_page.h"

void vjo_anki_page_show(VjoArena *scratch) { (void)scratch; }
void vjo_anki_page_frame(const VjoAnkiUi *ui, uint32_t pressed, int touch_x, int touch_y, VjoArena *scratch)
{
    (void)ui; (void)pressed; (void)touch_x; (void)touch_y; (void)scratch;
}
int vjo_anki_page_modal(void) { return 0; }
int vjo_anki_page_busy(void) { return 0; }
void vjo_anki_page_quit(void) {}
