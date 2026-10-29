/* Shared theme IDs and palette roles; packed colors here are RGB (not vita2d's ABGR). */
#ifndef VJO_COLOUR_SCHEME_H
#define VJO_COLOUR_SCHEME_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { VJO_THEME_DEFAULT, VJO_THEME_MINT, VJO_THEME_PAPER, VJO_THEMES };
#define VJO_MINT_ACCENT 0x5BDCCFu
#define VJO_PAPER_BG 0xF7F2E8u
#define VJO_PAPER_TEXT 0x302D28u
#define VJO_PAPER_DIM 0x665F53u
#define VJO_PAPER_ACCENT 0x386A59u
#define VJO_PAPER_HIGHLIGHT 0x80550Du
#define VJO_PAPER_READING 0x305E83u
#define VJO_PAPER_DEFINITION 0x514B43u
#define VJO_PAPER_ERROR 0xA13B48u
extern const char *const vjo_colour_scheme_names[];
/* app.ini rules: case-insensitive, BOM, CRLF, inline comments, last valid duplicate wins. */
int vjo_colour_scheme_parse(const char *text, size_t len);
/* Styled text retains its semantic RGB roles. Only the PAF presentation remaps them. */
uint32_t vjo_colour_scheme_rgb(int scheme, uint32_t rgb);
#ifdef __cplusplus
}
#endif
#endif
