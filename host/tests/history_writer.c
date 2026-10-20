/* The real history writer (shell/history.c) and its character count
 * (core/reading_chars.h) on the host, for tools/reading_stats.py's tests
 * (tools/tests/test_reading_stats.py):
 *
 *   vjo-history-writer FILE < records   appends each record to FILE as the Shell does
 *   vjo-history-writer --count < texts  prints each text's characters, one line each
 *
 * Records and texts end with a NUL byte. A record is TITLE_ID, NAME, TIME_US and
 * TEXT separated by 0x1F; its text is normalised in place as the game worker
 * normalises what it returns (vjo_text_normalize_keep: the recognizer's text after
 * the NUL, for raw_line), and the other OCR facts are fixed. One line per record
 * on stdout: vjo_history_append's result (1 appended, 0 blank or the text just
 * written, negative an error). */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

static const char *path;

static int native(int rc) { return rc < 0 ? -errno : rc; }
int sceIoOpen(const char *file, int flags, unsigned mode)
{
    (void)file;
    return native(open(path, flags, mode));
}
SceOff sceIoLseek(int fd, SceOff off, int whence) { return lseek(fd, off, whence); }
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

#include "../../shell/history.c"

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
    if (argc != 2) {
        fprintf(stderr, "usage: vjo-history-writer FILE < records | --count < texts\n");
        return 2;
    }
    if (!strcmp(argv[1], "--count")) {
        while (more) {
            size_t n = next_item(item, sizeof(item), &more);
            if (n || more) printf("%u\n", (unsigned)vjo_reading_characters(item, n));
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
        memset(&result, 0, sizeof(result));
        snprintf(result.text, sizeof(result.text), "%s", field[3]);
        vjo_text_normalize_keep(result.text, sizeof(result.text), NULL, NULL);
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
