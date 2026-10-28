/* The Rubi app seeds the overlay's reading streak once (core/streak.h, core/streak_scan.h): when
 * ux0:data/VitaJPOverlay/reading-streak.bin is missing, damaged or not seeded yet, a thread of its own (CPU3, as every
 * thread of the app) reads history.csv once, a window at a time, replays its records' days through the streak's rule,
 * keeps what the Shell added meanwhile (vjo_streak_merge) and writes the record, seeded. The Stats tab's cache of the
 * 0.9.8 test builds (stats_cache.bin, read by nothing now) is removed then. history.csv is only read. A seed the app's
 * exit stops writes nothing: the next start seeds again. */
#ifndef VJO_SETTINGS_STREAK_SEED_H
#define VJO_SETTINGS_STREAK_SEED_H

/* At the app's start: the seed's thread, when the record wants it. */
void vjo_streak_seed_start(void);
/* Leaving the app: a seed still running stops (writing nothing), and its thread is joined. */
void vjo_streak_seed_quit(void);

#endif
