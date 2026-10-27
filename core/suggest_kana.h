/* The kana and kanji of a recognized line's text (#358: the text box suggested on first use, core/region_suggest.h),
 * which core/vocr_ocr.c counts as it places a line. Its own header: core/vocr_ocr.c takes no SDK or kernel header
 * (include/vjo_api.h's syscalls need the SDK's types unless VJO_HOST), as the ARM lane's capture parity build has it. */
#ifndef VJO_SUGGEST_KANA_H
#define VJO_SUGGEST_KANA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The kana and kanji of a UTF-8 text, as the Shell's line filter counts Japanese (core/utf.h
 * vjo_is_japanese_char: U+3040-30FF and U+4E00-9FAF), at most 65535. core/region_suggest.c. */
uint16_t vjo_suggest_kana(const char *s);

#ifdef __cplusplus
}
#endif

#endif
