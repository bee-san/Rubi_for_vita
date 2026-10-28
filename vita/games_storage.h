#ifndef VJO_GAMES_STORAGE_H
#define VJO_GAMES_STORAGE_H
#include "../core/arena.h"
#include "../core/games.h"
#define VJO_GAMES_DIR "ux0:data/VitaJPOverlay"
#define VJO_GAMES_PATH VJO_GAMES_DIR "/games.ini"
/* The previous good copy; the kernel's reader (kernel/loader.c) reads it too. */
#define VJO_GAMES_BACKUP VJO_GAMES_PATH ".bak"
/* 0 = primary/missing; 1 = recovered backup; -1 = unreadable/corrupt. */
int vjo_games_file_load(VjoGames *games, VjoArena *scratch);
/* Checked writes, sync, backup and rename; retains a recovery copy. */
int vjo_games_file_save(const VjoGames *games, VjoArena *scratch);

/* #333: each game's own ocr_line_fix (core/games.h vjo_line_fix_parse), written by the Rubi app's game page and
 * read by the Shell as the game starts or resumes (shell/game.c). Older builds never read it. */
#define VJO_LINE_FIX_PATH VJO_GAMES_DIR "/ocr_line_fix.ini"
#define VJO_LINE_FIX_BACKUP VJO_LINE_FIX_PATH ".bak"
/* The Shell's read: the file alone, 0 or -1 (missing, unreadable or corrupt: then *games may hold part of it).
 * A save cut short leaves the backup, which the Rubi app reads and puts back as it starts (settings/main.c). */
int vjo_line_fix_file_read(VjoGames *games, VjoArena *scratch);
/* The Rubi app's: 0 the file (or neither file: no rows); 1 its backup, the file missing (a save cut short: the
 * caller saves the rows back, so the Shell reads them); -1 the file there but unreadable, or only a backup that
 * cannot be read either (no rows: nothing may be written over it). */
int vjo_line_fix_file_load(VjoGames *games, VjoArena *scratch);
/* As vjo_games_file_save, with vjo_line_fix_format. */
int vjo_line_fix_file_save(const VjoGames *games, VjoArena *scratch);

/* The file I/O the Shell's two readers share (shell/files.c too, #418): the whole of the open file fd, at most max
 * bytes, into the arena with a NUL after it (its length in *len), or NULL: too large, a seek that failed, an
 * allocation refused, a read that failed or fell short. The caller closes fd. And whether a path is there
 * (sceIoGetstat succeeds) or missing (it says ENOENT, the SDK's encoded value). */
char *vjo_io_read_fd(int fd, VjoArena *a, size_t max, size_t *len);
int vjo_io_exists(const char *path);
int vjo_io_missing(const char *path);
#endif
