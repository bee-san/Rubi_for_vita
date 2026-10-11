/* Bounded text records; one JSON object per line in the durable queue. */
#ifndef VJO_ANKI_QUEUE_H
#define VJO_ANKI_QUEUE_H
#include "anki.h"
#define VJO_QUEUE_ID_SIZE 33
#define VJO_QUEUE_TEXT_MAX (32u * 1024u)
#define VJO_QUEUE_FILE_MAX (16u * 1024u * 1024u)
/* The source title ID is captured before the foreground can change. */
char *vjo_queue_encode(VjoArena *a, const VjoAnkiNote *note,
                       const VjoConfig *config, const char *source_id);
void vjo_queue_id(const char *json, size_t len, char id[VJO_QUEUE_ID_SIZE]);
int vjo_queue_valid_id(const char *id);
#endif
