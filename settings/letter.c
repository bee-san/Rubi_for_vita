#include "letter.h"
#include <string.h>
#include "../core/utf.h"

/* (settings/main.c's initial() before this file, unchanged.) */
uint32_t vjo_title_letter(const char *title)
{
    static const uint16_t row_start[] = {0x3041, 0x304B, 0x3055, 0x305F, 0x306A, 0x306F,
                                         0x307E, 0x3083, 0x3089, 0x308E, 0x3094, 0x3095};
    static const uint16_t row_head[] = {0x3042, 0x304B, 0x3055, 0x305F, 0x306A, 0x306F,
                                        0x307E, 0x3084, 0x3089, 0x308F, 0x3042, 0x304B};
    size_t at = 0, len = strlen(title);
    uint32_t c = len ? vjo_utf8_next(title, len, &at) : 0;
    if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
    if (c < 0x80) return c >= 'A' && c <= 'Z' ? c : '#';
    uint32_t kana = c >= 0x30A1 && c <= 0x30F6 ? c - 0x60 : c; /* katakana as hiragana */
    if (kana < 0x3041 || kana > 0x3096) return c;
    int row = 0;
    while (row + 1 < (int)(sizeof(row_start) / sizeof(row_start[0])) && kana >= row_start[row + 1]) row++;
    return row_head[row] + (kana != c ? 0x60 : 0);
}

static int fold(unsigned char c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; }

int vjo_title_order(const char *title_a, const char *id_a, const char *title_b, const char *id_b)
{
    /* The letter first: ゔ and ヴ are filed under あ and ア but their bytes sort
     * after ん and ン, and '{' after 'z'; by bytes alone their letters would
     * come apart, and Left/Right would stop at them twice. */
    uint32_t la = vjo_title_letter(title_a), lb = vjo_title_letter(title_b);
    if (la != lb) return la < lb ? -1 : 1;
    const unsigned char *x = (const unsigned char *)title_a, *y = (const unsigned char *)title_b;
    while (*x && fold(*x) == fold(*y)) x++, y++;
    int n = fold(*x) - fold(*y);
    if (!n) n = strcmp(title_a, title_b);
    return n ? n : strcmp(id_a, id_b);
}
