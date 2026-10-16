#include "replay.h"

#include <stdio.h>
#include <string.h>

char *vjo_read_file(VjoArena *a, const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *p;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    p = (char *)vjo_arena_alloc(a, (size_t)n + 1);
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n)
        p = NULL;
    fclose(f);
    if (!p)
        return NULL;
    p[n] = '\0';
    if (len)
        *len = (size_t)n;
    return p;
}
