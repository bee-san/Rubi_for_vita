/* The Shell's and the Rubi app's file calls (vita/games_storage.c) over real
 * files (fakes/sceio_posix.h), for host/tests/test_kernel_loader.c's parity check:
 * the kernel's games.ini reader against the Shell's own on the same files. Reads
 * come back 13 bytes at a time, as test_games.c's, so every short-read loop runs. */
#define SCEIO_OWN SCEIO_READ
#include "fakes/sceio_posix.h"

int sceIoRead(int fd, void *p, size_t len) { return sceio_read(fd, p, len > 13 ? 13 : len); }
