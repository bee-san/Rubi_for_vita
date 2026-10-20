/* The Shell's side of native text hooks (API 31, docs/native-hooks.md): what △ in the overlay picks next,
 * and the overlay's compact source label. Plain C, no allocation, no platform calls: the Shell's control
 * and network threads call it, the host tests link it as is.
 *
 * pick is VjoGameOcrRequest.hook_pick (include/vjo_game_ocr.h): VJO_HOOK_PICK_AUTO, a stream ID 1..
 * VJO_HOOK_STREAMS (pinned) or VJO_HOOK_PICK_OCR. streams is a result's hook_streams: bit (id - 1) for each
 * stream ID that holds text. */
#ifndef VJO_HOOK_PICK_H
#define VJO_HOOK_PICK_H

#include <stddef.h>
#include <stdint.h>

#define VJO_HOOK_LABEL_BYTES 28u /* the longest label, "OCR, Hook 20/20 empty", and its NUL fit */

#ifdef __cplusplus
extern "C" { /* shell/shell.h, which overlay.cpp includes, includes it */
#endif

/* △'s next pick: automatic -> each stream that holds text, by ID -> OCR only -> automatic. A pinned
 * stream goes on to the next one with text after it (OCR only after the last); with no stream holding
 * text automatic goes to OCR only. An unknown pick goes back to automatic. */
uint32_t vjo_hook_pick_next(uint32_t pick, uint32_t streams);

/* The label for pick (installed: the worker hooked something in this game). With have_result 0 (no
 * result for this pick yet: △ was just pressed) it names the pick: "Automatic", "Hook 2/5 pinned",
 * "OCR only". With a result it names where text[] came from: "OCR", "Hook 2/5", "Hook 2/5 locked",
 * "Hook 2/5 pinned", "OCR, Hook 2/5 empty" (a pinned stream without text: OCR answered), "OCR only";
 * k/n: the stream's place among the streams with text, and their number. "" when nothing is hooked in
 * this game and nothing is pinned: no label, the overlay's layout as without hooks. Returns the bytes
 * before the NUL; out always ends with one (cap > 0). */
size_t vjo_hook_label(char *out, size_t cap, uint32_t pick, int installed, int have_result, uint32_t text_from,
                      uint32_t flags, uint32_t streams);

#ifdef __cplusplus
}
#endif

#endif
