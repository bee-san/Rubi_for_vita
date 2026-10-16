/* The Install page (settings/install_page.c) is not what the Games and OCR-page harnesses drive: it
 * has its own suite (host/tests/test_settings_install.c, on a temporary card with a payload). Here it
 * says the overlay is installed, so the app opens on the Games page as before, and it draws the
 * header only when shown. */
#include "../../../settings/install_page.h"

int vjo_install_page_start(void) { return 0; }
void vjo_install_page_show(void) {}
void vjo_install_page_frame(const VjoInstallDraw *ui, uint32_t pressed, int touch_x, int touch_y)
{
    (void)pressed; (void)touch_x; (void)touch_y;
    ui->header();
}
void vjo_install_page_stick(int move) { (void)move; }
int vjo_install_page_busy(void) { return 0; }
void vjo_install_page_quit(void) {}
int vjo_install_page_has_payload(void) { return 0; }
int vjo_install_page_runs(void) { return 0; }
