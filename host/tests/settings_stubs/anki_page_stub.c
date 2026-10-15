/* The Anki page (settings/anki_page.c) is not what the Settings harness
 * drives: it brings the app's SceNet, AnkiConnect and the card queue, and its
 * logic (core/anki_app.c) has its own suite, anki_app. Here the page does
 * nothing and is never busy, so the harness runs the real main.c's OCR &
 * lookup page. It only records the left stick's moves main.c gives it, and
 * is modal while a test opens its keyboard (test_settings_games.c). Its log
 * writes what main.c logs to the card's log.txt, as the page's own does, while
 * main.c has it on. */
#include "../../../settings/anki_page.h"

#include <stdarg.h>
#include <stdio.h>

void vjo_anki_page_show(VjoArena *scratch) { (void)scratch; }
void vjo_anki_page_frame(const VjoAnkiUi *ui, uint32_t pressed, int touch_x, int touch_y, VjoArena *scratch)
{
    (void)ui; (void)pressed; (void)touch_x; (void)touch_y; (void)scratch;
}
int vjo_anki_stub_rows, vjo_anki_stub_modal; /* the stick's moves, summed; the keyboard open */
void vjo_anki_page_stick(int move) { vjo_anki_stub_rows += move; }
int vjo_anki_page_modal(void) { return vjo_anki_stub_modal; }
int vjo_anki_page_busy(void) { return 0; }
void vjo_anki_page_quit(void) {}

static int log_on;
void vjo_log_enable(int on) { log_on = on; }
void vjo_log(const char *fmt, ...)
{
    FILE *f;
    va_list ap;
    if (!log_on || !(f = fopen("ux0:data/VitaJPOverlay/log.txt", "a")))
        return;
    fputs("overlay app: ", f);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}
