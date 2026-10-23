/* The real history writer (shell/history.c) and its character count
 * (core/reading_chars.h) on the host, for tools/reading_stats.py's tests
 * (tools/tests/test_reading_stats.py):
 *
 *   vjo-history-writer FILE < records   appends each record to FILE as the Shell does
 *   vjo-history-writer --count < texts  prints each text's characters, one line each
 *   vjo-history-writer --total FILE [N] prints the Shell's reading total of FILE, as a Triangle press
 *                                       has the control thread count it (docs/ocr-history.md#total-characters-read),
 *                                       window by window, with nothing kept from before: the characters,
 *                                       the bytes they are of, the windows read and the count's time in ns
 *                                       (the median of N counts, 1 by default; the host's, not a console's)
 *
 * Records and texts end with a NUL byte. A record is TITLE_ID, NAME, TIME_US and
 * TEXT separated by 0x1F, and optionally a fifth field after another 0x1F: the
 * result's text_from (0 OCR, the default; 1..20 the native hook stream whose text
 * it is: docs/ocr-history.md#line-source); its text is normalised in place as the game worker
 * normalises what it returns (vjo_text_normalize_keep: the recognizer's text after
 * the NUL, for raw_line), and the other OCR facts are fixed. One line per record
 * on stdout: vjo_history_append's result (1 appended, 0 blank or the text just
 * written, negative an error). */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

static const char *path;

static int native(int rc) { return rc < 0 ? -errno : rc; }
int sceIoOpen(const char *file, int flags, unsigned mode)
{
    (void)file;
    int fd = open(path, flags, mode);
    return fd < 0 && errno == ENOENT ? (int)0x80010002 : native(fd); /* no file: the SDK's error */
}
SceOff sceIoLseek(int fd, SceOff off, int whence) { return lseek(fd, off, whence); }
int sceIoRead(int fd, void *p, size_t n) { return native((int)read(fd, p, n)); }
int sceIoWrite(int fd, const void *p, size_t n) { return native((int)write(fd, p, n)); }
int sceIoSyncByFd(int fd, int flags)
{
    (void)flags;
    return native(fsync(fd));
}
int sceIoClose(int fd) { return native(close(fd)); }
int sceIoChstat(const char *file, SceIoStat *st, int flags)
{
    (void)file; (void)flags;
    return native(truncate(path, st->st_size));
}

int vjoNotifyShell(void); /* the kernel's (include/vjo_api.h declares it for the Vita build only) */
#include <stdarg.h>
#include <psp2/kernel/processmgr.h>
/* the debug build's count line (history.c): its clock, and the log, which this program does not keep */
uint64_t sceKernelGetProcessTimeWide(void) { return 0; }
void vjo_log(const char *fmt, ...) { (void)fmt; }

#include "../../shell/history.c"

/* The total's: the view's lock, the Shell's wakeup, and no history_total.bin */
VjoView g_view;
void vjo_view_lock(void) {}
void vjo_view_unlock(void) {}
int vjoNotifyShell(void) { return 0; }
char *vjo_file_read(VjoArena *a, const char *file, size_t *len) { (void)a; (void)file; (void)len; return NULL; }
int vjo_file_write(const char *file, const void *data, size_t len) { (void)file; (void)data; return (int)len; }
int vjo_file_exists(const char *file) { (void)file; return 0; }

/* The next NUL-terminated item of stdin into buf; 0 at the end. */
static size_t next_item(char *buf, size_t cap, int *more)
{
    size_t n = 0;
    int c;
    while ((c = getchar()) != EOF && c != 0)
        if (n + 1 < cap) buf[n++] = (char)c;
    buf[n] = 0;
    *more = c != EOF;
    return n;
}

int main(int argc, char **argv)
{
    static char item[2 * VJO_GAME_OCR_TEXT_BYTES];
    static VjoGameOcrResult result;
    int more = 1;
    if ((argc == 3 || argc == 4) && !strcmp(argv[1], "--total")) {
        static uint8_t mem[24 * 1024] __attribute__((aligned(8)));
        static uint64_t ns[101];
        VjoArena scratch;
        unsigned windows = 0, runs = argc == 4 ? (unsigned)strtoul(argv[3], NULL, 10) : 1;
        if (!runs || runs > 101) { fprintf(stderr, "vjo-history-writer: 1 to 101 counts\n"); return 2; }
        path = argv[2];
        for (unsigned k = 0; k < runs; k++) {
            struct timespec t0, t1;
            memset(&total, 0, sizeof(total)); /* the Shell's start: nothing kept (no history_total.bin) */
            windows = 1;
            vjo_arena_init(&scratch, mem, sizeof(mem));
            clock_gettime(CLOCK_MONOTONIC, &t0);
            vjo_history_total_want();
            while ((vjo_history_total_step(&scratch) || total.state == TOTAL_COUNTING) && windows < 1000000) windows++;
            clock_gettime(CLOCK_MONOTONIC, &t1);
            ns[k] = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000u + (uint64_t)t1.tv_nsec - (uint64_t)t0.tv_nsec;
            if (total.state != TOTAL_KNOWN) { fprintf(stderr, "vjo-history-writer: no total (%d)\n", total.state); return 1; }
        }
        for (unsigned a = 1; a < runs; a++) /* the median */
            for (unsigned b = a; b && ns[b - 1] > ns[b]; b--) { uint64_t t = ns[b]; ns[b] = ns[b - 1]; ns[b - 1] = t; }
        printf("%llu %u %u %llu\n", (unsigned long long)total.chars, (unsigned)total.covered, windows,
               (unsigned long long)ns[runs / 2]);
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: vjo-history-writer FILE < records | --count < texts | --total FILE [N]\n");
        return 2;
    }
    if (!strcmp(argv[1], "--count")) {
        while (more) {
            size_t n = next_item(item, sizeof(item), &more);
            if (n || more) printf("%u\n", (unsigned)vjo_line_characters(item, n));
        }
        return 0;
    }
    path = argv[1];
    while (more) {
        char *field[4] = {item, NULL, NULL, NULL};
        size_t n = next_item(item, sizeof(item), &more);
        if (!n && !more) break;
        for (int f = 1; f < 4; f++) {
            char *sep = field[f - 1] ? strchr(field[f - 1], 0x1F) : NULL;
            if (!sep) { fprintf(stderr, "vjo-history-writer: a record needs 4 fields\n"); return 2; }
            *sep = 0;
            field[f] = sep + 1;
        }
        VjoHistoryRecord r = {0};
        char *from = strchr(field[3], 0x1F);
        memset(&result, 0, sizeof(result));
        if (from) { /* the text's source: a hook stream's ID, which a hook's answer also flags */
            *from++ = 0;
            result.text_from = (uint32_t)strtoul(from, NULL, 10);
            if (result.text_from)
                result.hook_flags = VJO_HOOK_R_AGREED | VJO_HOOK_R_LOCKED |
                                    (result.text_from <= VJO_HOOK_SITE_STREAMS ? VJO_HOOK_R_SITE : 0u);
        }
        snprintf(result.text, sizeof(result.text), "%s", field[3]);
        vjo_text_normalize_keep(result.text, sizeof(result.text), NULL, 0, NULL);
        result.seq = 1; result.width = 900; result.height = 125; result.fb_width = 960; result.fb_height = 544;
        result.lines = result.recognized = 1;
        result.line[0] = (VjoGameOcrLine){800, 30, 50, 950, 300000};
        snprintf(r.title_id, sizeof(r.title_id), "%.11s", field[0]);
        snprintf(r.name, sizeof(r.name), "%.127s", field[1]);
        r.platform = "vita"; r.model = "VOCR5_N_w8.vocr"; r.result = &result;
        r.time_us = strtoull(field[2], NULL, 10);
        printf("%d\n", vjo_history_append(&r));
    }
    return 0;
}
