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
#endif
