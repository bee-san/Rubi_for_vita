/* The Overlay app's lines in log.txt (settings/anki_page.h): "overlay app: "
 * and the text, written while config.ini's log_file is on, and only below the
 * Shell's 1 MiB rotation. Each line is one append, opened and closed at once,
 * so a crash keeps every line before it; a batch (vjo_app_log_batch_begin/end)
 * keeps its lines in memory and writes them in one append, as the Shell's does
 * for a job's lines (#100): an append cost the console 12-46 ms in #81's
 * self-test timestamps, and a scan of the Games list logs four lines. */
#include "anki_page.h"
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LOG_PATH "ux0:data/VitaJPOverlay/log.txt"
#define LOG_CAP (1024 * 1024) /* the Shell rotates log.txt; the app only appends below this */
#define LINE_MAX 300          /* a line, "overlay app: " and the line break included */
#define BATCH_MAX 2048        /* a batch: about seven lines of the most a line holds */

static int log_on;
/* The open batch: its lines and the thread that opened it (0: none). The app
 * logs from its main thread and, during an export, from the Anki worker
 * (settings/anki_export.c): another thread's line is written at once, never
 * into the batch. A batch is opened and closed in one function of its thread,
 * which waits for nothing in between (settings/catalog.c's scan). */
static char batch[BATCH_MAX];
static size_t batch_used;
static SceUID batch_thread;

void vjo_log_enable(int on) { log_on = on; }

/* text's whole lines (n bytes, each ending in a line break) to log.txt in one
 * append: each line that still fits below the cap, in order, as the writer
 * did line by line before batches (a line that does not fit is dropped, a
 * shorter one after it may still fit). The kept lines are moved together in
 * text itself. */
static void append(char *text, size_t n)
{
    SceIoStat st;
    SceUID fd;
    if (!n) return;
    memset(&st, 0, sizeof(st));
    if (sceIoGetstat(LOG_PATH, &st) >= 0 && st.st_size + (SceOff)n > LOG_CAP) {
        SceOff size = st.st_size;
        size_t kept = 0;
        for (size_t at = 0; at < n;) {
            const char *end = memchr(text + at, '\n', n - at);
            size_t len = (end ? (size_t)(end - (text + at)) + 1 : n - at);
            if (size + (SceOff)len <= LOG_CAP) {
                memmove(text + kept, text + at, len);
                kept += len;
                size += (SceOff)len;
            }
            at += len;
        }
        n = kept;
        if (!n) return;
    }
    if ((fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666)) >= 0) {
        sceIoWrite(fd, text, (SceSize)n);
        sceIoClose(fd);
    }
}

void vjo_app_log_batch_begin(void)
{
    batch_used = 0;
    batch_thread = sceKernelGetThreadId();
}

void vjo_app_log_batch_end(void)
{
    if (batch_thread != sceKernelGetThreadId()) return;
    append(batch, batch_used);
    batch_used = 0;
    batch_thread = 0;
}

void vjo_log(const char *fmt, ...)
{
    char line[LINE_MAX];
    va_list ap;
    int n = snprintf(line, sizeof(line), "overlay app: ");
    if (!log_on || n < 0)
        return;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
    va_end(ap);
    n = (int)strlen(line);
    line[n++] = '\n';
    if (batch_thread && batch_thread == sceKernelGetThreadId()) {
        if (batch_used + (size_t)n > sizeof(batch)) { /* full: what it holds goes out first */
            append(batch, batch_used);
            batch_used = 0;
        }
        memcpy(batch + batch_used, line, (size_t)n);
        batch_used += (size_t)n;
        return;
    }
    append(line, (size_t)n);
}
