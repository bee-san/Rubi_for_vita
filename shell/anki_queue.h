#ifndef VJO_SHELL_ANKI_QUEUE_H
#define VJO_SHELL_ANKI_QUEUE_H
#include "../core/anki_queue.h"
#define VJO_QUEUE_PATH "ux0:data/VitaJPOverlay/anki_queue.jsonl"
/* Serialized by the existing worker. Never allocates the whole queue.
 * count includes all saved records, including records later synchronized. */
int vjo_queue_count(void);
/* 0 saved, 1 already saved, -1 I/O/damaged queue, -2 full, -3 arena full.
 * Incomplete final writes are repaired before appending. A complete damaged
 * line is preserved and reported, never silently discarded. */
int vjo_queue_save(VjoArena *a, const VjoAnkiNote *note,
                   const VjoConfig *config, const char *source_id);
#endif
