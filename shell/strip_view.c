/* The subtitles strip's view model (VjoView.strip_*), written by the
 * control thread (worker.c) and drawn by strip.cpp. */
#include <psp2/kernel/clib.h>

#include "shell.h"

static int s_on, s_font_ja, s_font_en;

void vjo_strip_configure(int on, const VjoConfig *cfg)
{
    s_on = on;
    s_font_ja = cfg->font_size_ja;
    s_font_en = cfg->font_size_en;
}

/* Every change bumps strip_version (under the view lock). */
static void bump(void)
{
    __atomic_store_n(&g_view.strip_on, s_on, __ATOMIC_RELEASE); /* also read unlocked */
    g_view.font_size_ja = s_font_ja;
    g_view.font_size_en = s_font_en;
    g_view.strip_version++;
}

void vjo_strip_show(const char *text, int kind, int busy)
{
    size_t n;
    if (!text) /* an error whose text had no room (vjo_err_text's NULL, #528): an empty strip, not a fault in SceShell */
        text = "";
    n = sceClibStrnlen(text, sizeof(g_view.strip_text) - 1);
    if (n == sizeof(g_view.strip_text) - 1)
        while (n && ((uint8_t)text[n] & 0xC0) == 0x80)
            n--;
    vjo_view_lock();
    if (kind != g_view.strip_kind || sceClibStrncmp(g_view.strip_text, text, n) || g_view.strip_text[n]) {
        sceClibMemcpy(g_view.strip_text, text, n);
        g_view.strip_text[n] = '\0';
        g_view.strip_kind = kind;
        g_view.strip_text_seq++;
    }
    g_view.strip_busy = busy;
    bump();
    vjo_view_unlock();
}

void vjo_strip_set_busy(int busy)
{
    vjo_view_lock();
    g_view.strip_busy = busy;
    bump();
    vjo_view_unlock();
}
