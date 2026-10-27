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
 * A save cut short leaves the backup, which the Rubi app reads and its next save puts back. */
int vjo_line_fix_file_read(VjoGames *games, VjoArena *scratch);
/* The Rubi app's, as vjo_games_file_load: 0 the file (or neither file: no rows), 1 its backup, -1 unreadable. */
int vjo_line_fix_file_load(VjoGames *games, VjoArena *scratch);
/* As vjo_games_file_save, with vjo_line_fix_format. */
int vjo_line_fix_file_save(const VjoGames *games, VjoArena *scratch);
#endif
