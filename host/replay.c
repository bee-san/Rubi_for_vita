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

static int mem_send(void *ctx, const void *p, size_t n)
{
    VjoMemConn *m = (VjoMemConn *)ctx;
    if (m->out) {
        size_t k = m->out_cap - m->out_len < n ? m->out_cap - m->out_len : n;
        memcpy(m->out + m->out_len, p, k);
        m->out_len += k;
    }
    return (int)n;
}

static int mem_recv(void *ctx, void *p, size_t n)
{
    VjoMemConn *m = (VjoMemConn *)ctx;
    size_t k = m->len - m->pos;
    if (k > n)
        k = n;
    if (m->step && k > m->step)
        k = m->step;
    if (!k)
        return m->end_rc;
    memcpy(p, m->in + m->pos, k);
    m->pos += k;
    return (int)k;
}

void vjo_memconn_init(VjoMemConn *m, VjoConn *c)
{
    c->ctx = m;
    c->send = mem_send;
    c->recv = mem_recv;
}
