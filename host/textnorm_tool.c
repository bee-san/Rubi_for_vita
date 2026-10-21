/* core/textnorm.c on standard input: each input line is normalised as the
 * overlay normalises recognized text and printed, followed by a tab and its
 * changes (width tilde dash ellipsis long_vowel kanji_one counter garbage); a
 * line the border rule drops prints as an empty text. For tools/textnorm_eval.py. */
#include <stdio.h>
#include <string.h>
#include "textnorm.h"

int main(void)
{
    static char line[16384];
    while (fgets(line, sizeof(line), stdin)) {
        size_t n = strlen(line);
        VjoTextNorm k;
        if (n && line[n - 1] == '\n') line[--n] = 0;
        vjo_text_normalize(line, &k);
        printf("%s\t%u %u %u %u %u %u %u %u\n", line, k.width, k.tilde, k.dash, k.ellipsis, k.long_vowel, k.kanji_one,
               k.counter, k.garbage);
    }
    return 0;
}
