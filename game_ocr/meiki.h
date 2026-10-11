#ifndef VJO_MEIKI_GAME_H
#define VJO_MEIKI_GAME_H
#include "../core/local_ocr.h"
#include "../include/vjo_game_ocr.h"
#define VJO_MEIKI_PREPROCESS_BYTES 385024u
#define VJO_MEIKI_OWNED_BYTES 21356544u
#define VJO_MEIKI_MODULE_ALLOWANCE (2u * 1024u * 1024u)
#define VJO_MEIKI_GAME_RESERVE (4u * 1024u * 1024u)
#define VJO_MEIKI_REQUIRED_BYTES 27648000u
int vjo_meiki_game_owned(void);
int vjo_meiki_game_cleanup(void);
int vjo_meiki_game_run(const VjoPlatform *, const char *, const VjoOcrImage *, VjoGameOcrResult *);
#endif
