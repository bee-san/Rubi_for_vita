/* The characters of a recognized text on the screen: the char map (API 46, docs/overlay-on-text.md, #309).
 *
 * The game worker places every character of the text it returns where it is on the screen (vita-vn-ocr #114:
 * runtime/vocr_charbox.c, from the CTC frame each character fires at, mapped through the line's resize), and
 * writes the places as a small block after the text and its recognizer's copy in VjoGameOcrResult.text, whose
 * last bytes are zero otherwise (core/textnorm.h): no struct grows. The block is keyed to the SENTENCE the Shell
 * shows, the text filtered as non_japanese_filter says and trimmed (core/textfilter.h, vjo_java_trim), so the
 * Shell looks nothing up: it checks the block against its sentence (length and FNV-1a) and keeps it after the
 * sentence's NUL (shell/worker.c local_text), where the open overlay reads it.
 *
 * The block (little-endian 16-bit words at any byte offset):
 *   w0 'C' | 'M' << 8; w1 VJO_CMAP_VERSION | lines << 8; w2 chars; w3 the sentence's length in bytes;
 *   w4, w5 its FNV-1a (low, high); w6, w7 the width and height of the space the boxes are in (the
 *   screen, VJO_SCREEN_W x VJO_SCREEN_H, in what the worker writes for the Shell: it maps the game's
 *   frame to the screen, so the Shell scales nothing);
 *   then per character (chars of them, in sentence order): the byte offset of its first byte in the sentence,
 *   x0, x1 (its cell along its line; the cells of a line follow each other). A character the recognizer did not
 *   read itself (the LM, the text normalisation or a hook text put it there) has x0 == x1, at its neighbour;
 *   then per line (lines of them, top to bottom, a row of the screen each): y0, y1 (its rows), the index of its
 *   first character, their count. A text line the game wrapped over several rows has a line for each row.
 * Characters the block does not hold (a line it could not place, a sentence longer than the block's room) have
 * no box: the overlay falls back to its popup for them.
 *
 * Plain C99, no allocation. The Shell's part (find, carry, get, line, index, hit, span, vertical, popup) is small
 * and cold: it runs on a new text, a key press or a tap while the overlay is open. */
#ifndef VJO_CHARMAP_H
#define VJO_CHARMAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VJO_CMAP_VERSION 2u
#define VJO_CMAP_LINES 16u                 /* the line finder's most (VJO_VOCR_MAX_LINES) */
#define VJO_CMAP_CHARS 512u
#define VJO_CMAP_HEADER 16u
#define VJO_CMAP_MAX_BYTES (VJO_CMAP_HEADER + 6u * VJO_CMAP_CHARS + 8u * VJO_CMAP_LINES) /* 3,216 */
#define VJO_SCREEN_W 960
#define VJO_SCREEN_H 544
/* A tap this far (screen px, x and y distances added) outside every cell still takes the nearest character. */
#define VJO_CMAP_SLACK 12

typedef struct {
    const unsigned char *p;   /* the block, NULL: none */
    unsigned lines, chars;
} VjoCmap;

typedef struct {
    int x, y, w, h;           /* screen px, origin top left */
} VjoCmapRect;

/* FNV-1a 32 of n bytes. */
uint32_t vjo_cmap_fnv(const char *s, size_t n);

/* The block after a sentence of len bytes in buf (cap bytes): it starts at buf + len + 1. 1 with *m set
 * when it is there, whole, and made for exactly this sentence (length and FNV-1a), else 0 (m->p NULL). */
int vjo_cmap_find(const char *buf, size_t cap, size_t len, VjoCmap *m);
/* Its size in bytes (a found block). */
size_t vjo_cmap_bytes(const VjoCmap *m);
/* The Shell's copy: the block the game worker wrote in src (src_cap bytes: its text, NUL, the recognizer's copy,
 * NUL, the block) into dst after the sentence of len bytes there (dst_cap bytes), when it is made for exactly that
 * sentence and fits; else dst holds none after its NUL (zeroed). Returns the bytes copied. */
size_t vjo_cmap_carry(char *dst, size_t dst_cap, size_t len, const char *src, size_t src_cap);

/* Character k's offset in the sentence, its line and its cell along it. */
void vjo_cmap_get(const VjoCmap *m, unsigned k, uint32_t *off, unsigned *line, unsigned *x0, unsigned *x1);
/* Line i's rows, and its first character and their count. */
void vjo_cmap_line(const VjoCmap *m, unsigned i, unsigned *y0, unsigned *y1, unsigned *first, unsigned *count);
/* The character that starts at byte `off` of the sentence, or -1. */
int vjo_cmap_index(const VjoCmap *m, uint32_t off);

/* The character whose cell holds the screen point (x, y), else the nearest one within VJO_CMAP_SLACK (a cell of
 * zero width holds nothing), or -1. */
int vjo_cmap_hit(const VjoCmap *m, int x, int y);
/* The screen rectangles of the characters starting at sentence bytes [start, end): one per line they
 * are on, at most max of them, in order. Returns their count (0: none of them has a box). */
int vjo_cmap_span(const VjoCmap *m, uint32_t start, uint32_t end, VjoCmapRect *r, int max);
/* Up (dir -1) or Down (dir 1) from character k: the character of the nearest line above or below whose
 * cell's centre is closest to k's, or -1 (no line there). */
int vjo_cmap_vertical(const VjoCmap *m, int k, int dir);
/* Where a popup of height h goes for a word at `word` (screen px): its top. Below all the map's lines when it fits
 * there with gap px between, else above them all (so it covers none of the game's text), else below the word, else
 * above it, else at the screen's edge farther from the word. */
int vjo_cmap_popup_top(const VjoCmap *m, const VjoCmapRect *word, int h, int gap);

/* ---- the game worker's part: writing a block ---- */
typedef struct {
    unsigned char *p;         /* the block's first byte */
    size_t cap;               /* the room for it */
    unsigned lines, chars, max_chars;
    uint16_t line[VJO_CMAP_LINES][4]; /* y0, y1, first, count: written after the characters by vjo_cmap_end */
    int failed;               /* a line or a character did not fit: the block stays as far as it got */
} VjoCmapWriter;

/* Starts a block at buf + at (cap bytes after it) for a sentence of len bytes with FNV-1a fnv, its boxes in a
 * space of w x h. 0, or -1 when not even the header fits. */
int vjo_cmap_begin(VjoCmapWriter *wr, char *buf, size_t cap, size_t at, size_t len, uint32_t fnv, unsigned w,
                   unsigned h);
/* A new line (y0 <= y1): the characters added after it, until the next line, are its. */
int vjo_cmap_add_line(VjoCmapWriter *wr, unsigned y0, unsigned y1);
/* The next character: its sentence offset (increasing) and its cell (x0 <= x1). -1 when it does not fit. */
int vjo_cmap_add_char(VjoCmapWriter *wr, uint32_t off, unsigned x0, unsigned x1);
/* Ends the block: its line table and counts written; lines that got no character are dropped. Returns its
 * bytes, or 0 when it holds no character (then it is zeroed: no block). */
size_t vjo_cmap_end(VjoCmapWriter *wr);

#ifdef __cplusplus
}
#endif
#endif
