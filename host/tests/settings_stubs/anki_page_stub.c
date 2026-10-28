/* The Anki page (settings/anki_page.c) is not what the Settings harness
 * drives: it brings the card queue and the export, which have their own
 * suite (apkg: settings/anki_export.c, core/apkg.c). Here the page does
 * nothing and is never busy, so the harness runs the real main.c's other
 * pages. It only records the left stick's moves main.c gives it
 * (test_settings_games.c). The app's log.txt lines are the real writer's
 * (settings/applog.c), on the card's files. */
#include "../../../settings/anki_page.h"

int vjo_anki_stub_frames, vjo_anki_stub_shows, vjo_anki_stub_hold_cancel_to;
uint32_t vjo_anki_stub_pressed;
void vjo_anki_page_show(VjoArena *scratch) { (void)scratch; vjo_anki_stub_shows++; }
void vjo_anki_page_frame(const VjoPageUi *ui, uint32_t pressed, int touch_x, int touch_y, VjoArena *scratch)
{
    (void)touch_x; (void)touch_y; (void)scratch;
    vjo_anki_stub_frames++;
    vjo_anki_stub_pressed |= pressed;
    (void)ui;
}
int vjo_anki_stub_rows; /* the stick's moves, summed */
void vjo_anki_page_stick(int move) { vjo_anki_stub_rows += move; }
int vjo_anki_page_busy(void) { return 0; }
int vjo_anki_page_holds_cancel(void) { return vjo_anki_stub_frames < vjo_anki_stub_hold_cancel_to; }
void vjo_anki_page_quit(void) {}
