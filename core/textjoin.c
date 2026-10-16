#include "textjoin.h"

int vjo_text_joins(const char *text, size_t len, size_t at)
{
    (void)text;
    (void)len;
    (void)at;
    return 0;
}

size_t vjo_text_join(char *text, int *offs, int n)
{
    size_t len = 0;
    (void)offs;
    (void)n;
    while (text[len])
        len++;
    return len;
}
