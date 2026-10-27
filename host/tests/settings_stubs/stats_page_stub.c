/* The Stats page (settings/stats_page.c, #312) is not what the Games and OCR-page harnesses drive: it has its own
 * suite (host/tests/test_settings_stats.c, on a temporary card with a history.csv). Here it counts nothing and draws
 * the header only, so the tab bar can be checked on it; it records how often it was shown and the stick's moves. */
#include <vita2d.h>
#include "../../../settings/stats_page.h"

int vjo_stats_stub_shown, vjo_stats_stub_rows;
void vjo_stats_page_show(VjoArena *scratch) { (void)scratch; vjo_stats_stub_shown++; }
void vjo_stats_page_frame(const VjoStatsUi *ui, uint32_t pressed, int touch_x, int touch_y, VjoArena *scratch)
{
    (void)pressed; (void)touch_x; (void)touch_y; (void)scratch;
    vita2d_start_drawing(); /* a whole frame, as the page draws its own */
    vita2d_clear_screen();
    ui->header();
    vita2d_end_drawing();
    vita2d_swap_buffers();
}
void vjo_stats_page_stick(int move) { vjo_stats_stub_rows += move; }
void vjo_stats_page_quit(void) {}
