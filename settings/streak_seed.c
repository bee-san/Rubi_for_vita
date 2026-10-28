#include "streak_seed.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>
#include <stdio.h>
#include <string.h>

#include "anki_page.h" /* vjo_log */
#include "../core/streak_scan.h"

#define DATA_DIR "ux0:data/VitaJPOverlay"
#define STREAK_PATH DATA_DIR "/" VJO_STREAK_FILE
#define STREAK_TEMP STREAK_PATH ".new" /* (not the Shell's .tmp: shell/files.c vjo_file_write) */
#define HISTORY_PATH DATA_DIR "/history.csv"
#define STATS_CACHE_PATH DATA_DIR "/stats_cache.bin" /* the Stats tab's (0.9.8's test builds): read by nothing now */
#define MISSING ((int)0x80010002u)
#define RTC_UNIX_EPOCH 62135596800000000ull /* SceRtcTick's microseconds from 0001-01-01 to 1970-01-01 */
#define WINDOW (64 * 1024)                  /* history.csv is read 64 KiB (and the byte before) at a time */
#define SEED_STACK (16 * 1024)

static char window[WINDOW + 1];
static SceUID thread = -1;
static volatile int stop;

/* The local time's offset from UTC now, in minutes (the Vita's time zone and daylight saving setting). */
static int32_t local_offset(void)
{
    SceRtcTick utc, local;
    int64_t d;
    if (sceRtcGetCurrentTick(&utc) < 0 || utc.tick < RTC_UNIX_EPOCH || sceRtcConvertUtcToLocalTime(&utc, &local) < 0)
        return 0;
    d = (int64_t)(local.tick - utc.tick);
    return (int32_t)((d >= 0 ? d + 30000000 : d - 30000000) / 60000000);
}

/* The record as the Shell reads it (shell/history.c vjo_history_streak), or 0 when there is none or it is not one. */
static int read_record(VjoStreak *s)
{
    SceUID fd = sceIoOpen(STREAK_PATH, SCE_O_RDONLY, 0);
    int n = fd < 0 ? fd : sceIoRead(fd, s, sizeof(*s));
    if (fd >= 0)
        sceIoClose(fd);
    return n == (int)sizeof(*s) && vjo_streak_valid(s);
}

/* As the Shell writes it (shell/files.c vjo_file_write: a temporary file of its own name, then a rename), synced. 0,
 * or the error. */
static int write_record(const VjoStreak *s)
{
    SceUID fd = sceIoOpen(STREAK_TEMP, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    int n, rc;
    if (fd < 0)
        return fd;
    n = sceIoWrite(fd, s, sizeof(*s));
    rc = sceIoClose(fd);
    if (n != (int)sizeof(*s))
        return n < 0 ? n : -1;
    if (rc < 0 || (rc = sceIoSync("ux0:", 0)) < 0)
        return rc;
    sceIoRemove(STREAK_PATH);
    if ((rc = sceIoRename(STREAK_TEMP, STREAK_PATH)) < 0)
        return rc;
    return sceIoSync("ux0:", 0);
}

/* n bytes at off of fd into buf (short reads again): 0, or the SDK's error (-1: the file ended first). */
static int read_at(SceUID fd, void *buf, size_t n, uint64_t off)
{
    size_t got = 0;
    while (got < n) {
        int r = sceIoPread(fd, (char *)buf + got, n - got, (SceOff)(off + got));
        if (r <= 0)
            return r < 0 ? r : -1;
        got += (size_t)r;
    }
    return 0;
}

/* history.csv's days into s: 0, 1 when stopped, or the SDK's error. *bytes: the bytes read. */
static int count_history(VjoStreak *s, int32_t tz, uint64_t *bytes)
{
    SceUID fd = sceIoOpen(HISTORY_PATH, SCE_O_RDONLY, 0);
    SceOff end;
    uint64_t size, pos = 0;
    int rc = 0;
    *bytes = 0;
    if (fd < 0)
        return fd == MISSING ? 0 : fd; /* none: nothing read yet (Read text: Only when I press L + R keeps none) */
    end = sceIoLseek(fd, 0, SCE_SEEK_END);
    if (end < 0) {
        sceIoClose(fd);
        return (int)end;
    }
    size = (uint64_t)end;
    while (pos < size) {
        uint64_t base = pos ? pos - 1 : 0, to = size - pos > WINDOW ? pos + WINDOW : size, next;
        size_t n = (size_t)(to - base);
        if (stop) {
            rc = 1;
            break;
        }
        if ((rc = read_at(fd, window, n, base)) != 0)
            break;
        /* the file's end in a line break: its newest record is whole (one without it may still be being written) */
        next = base + vjo_streak_scan(s, window, n, !base, to == size && window[n - 1] == '\n', tz);
        if (to < size && next >= to)
            next = to - 1; /* no record starts in it: its last byte may start one (read with the byte after it) */
        if (next <= pos) {
            if (to == size)
                break; /* the newest record, not whole yet: the Shell adds its day if it is today's */
            next = to - 1; /* a record longer than a window: none the writer makes (damaged): skipped, but not the
                            * window's last byte, where the next record may start */
        }
        pos = next;
    }
    sceIoClose(fd);
    *bytes = pos;
    return rc;
}

static int seed_main(SceSize args, void *argp)
{
    VjoStreak seed, live;
    int have = read_record(&live), rc;
    int32_t tz = local_offset();
    uint64_t bytes;
    (void)args;
    (void)argp;
    if (have && live.seeded == VJO_STREAK_SEEDED)
        return 0;
    vjo_streak_init(&seed);
    rc = count_history(&seed, tz, &bytes);
    if (rc) {
        if (rc < 0)
            vjo_log("streak: history.csv could not be read (0x%08X): seeded at the next start", (unsigned)rc);
        return 0;
    }
    /* what the Shell added while the app had not seeded it (read again: a line may have come meanwhile) */
    have = read_record(&live);
    vjo_streak_merge(&seed, have ? &live : NULL);
    seed.seeded = VJO_STREAK_SEEDED;
    if ((rc = write_record(&seed)) < 0) {
        vjo_log("streak: %s could not be written (0x%08X): seeded at the next start", STREAK_PATH, (unsigned)rc);
        return 0;
    }
    rc = sceIoRemove(STATS_CACHE_PATH);
    vjo_log("streak: seeded from %llu B of history.csv (UTC%+d min): current %u, longest %u, last day %d%s",
            (unsigned long long)bytes, (int)tz, (unsigned)seed.current, (unsigned)seed.longest, (int)seed.day,
            rc >= 0 ? "; stats_cache.bin removed" : "");
    return 0;
}

void vjo_streak_seed_start(void)
{
    VjoStreak s;
    int rc;
    if (thread >= 0 || (read_record(&s) && s.seeded == VJO_STREAK_SEEDED))
        return; /* (seeded: nothing to do, and no thread) */
    stop = 0;
    thread = sceKernelCreateThread("VjoStreakSeed", seed_main, 0x10000100, SEED_STACK, 0, SCE_KERNEL_CPU_MASK_SYSTEM,
                                   NULL);
    rc = thread < 0 ? thread : sceKernelStartThread(thread, 0, NULL);
    if (rc < 0) {
        if (thread >= 0)
            sceKernelDeleteThread(thread);
        thread = -1;
        vjo_log("streak: the seed could not start (0x%08X): seeded at the next start", (unsigned)rc);
    }
}

void vjo_streak_seed_quit(void)
{
    if (thread < 0)
        return;
    stop = 1;
    sceKernelWaitThreadEnd(thread, NULL, NULL);
    sceKernelDeleteThread(thread);
    thread = -1;
}
