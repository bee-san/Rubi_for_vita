/* Build-time exporter. The worker never links this catalogue. */
#include "../core/hook_profile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require(int ok) { if (!ok) { fputs("hook profile exceeds its format bounds\n", stderr); exit(1); } }
static void w16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { w16(p, v); w16(p + 2, v >> 16); }
static uint8_t strings[VJO_PROFILE_BYTES];
static unsigned ns;
static const LunaOp *programs[512];

static unsigned string_add(const char *s)
{
    unsigned n;
    if (!s || !*s) return 0;
    n = (unsigned)strlen(s) + 1;
    for (unsigned i = 0; i < ns; i++)
        if (n <= ns - i && !memcmp(strings + i, s, n)) return i;
    require(n <= sizeof(strings) - ns);
    memcpy(strings + ns, s, n); ns += n;
    return ns - n;
}
static void program_add(unsigned id)
{
    const LunaOp *ops;
    if (!id) return;
    require(id < 512);
    if (programs[id]) return;
    ops = vjo_hook_profile_export_ops(id);
    if (!ops) return; /* unported/unused postprocessor */
    programs[id] = ops;
    for (unsigned i = 0; ; i++) {
        require(i < 256);
        if (ops[i].op == OP_END) break;
        if (ops[i].op == OP_CALL) program_add(ops[i].arg);
    }
}

static unsigned profile(const char *title, uint8_t out[VJO_PROFILE_BYTES])
{
    const VjoLunaSite *sites;
    const VjoPlaceVerified *verified;
    unsigned nl = vjo_luna_sites(title, &sites), nv = vjo_place_verified(title, &verified), nf = 0, no = 0, at = 72;
    require(nl <= VJO_PROFILE_LUNA_MAX && nv <= VJO_PROFILE_VERIFIED_MAX);
    memset(programs, 0, sizeof(programs)); memset(out, 0, VJO_PROFILE_BYTES); ns = 1; strings[0] = 0;
    for (unsigned i = 0; i < nl; i++) if (vjo_luna_ported(&sites[i])) {
        program_add(sites[i].filter); program_add(VJO_PROFILE_POST + sites[i].func);
    }
    for (unsigned i = 0; i < nv; i++) program_add(verified[i].filter);
    for (unsigned id = 1; id < 512; id++) if (programs[id]) {
        nf++;
        for (const LunaOp *o = programs[id]; ; o++) { no++; if (o->op == OP_END) break; }
    }
    require(72 + 18 * nl + 36 * nv + 4 * nf + 8 * no <= VJO_PROFILE_BYTES);
    memcpy(out, "vjohook\1", 8); memcpy(out + 8, title, 9);
    require(vjo_hook_profile_revision(out + 20));
    w16(out + 56, nl); w16(out + 58, nv); w16(out + 60, nf); w16(out + 62, no);
    for (unsigned i = 0; i < nl; i++, at += 18) {
        const VjoLunaSite *s = &sites[i]; uint8_t *r = out + at;
        w32(r, s->addr); w32(r + 4, (uint32_t)s->padding);
        r[8] = s->reg; r[9] = s->encoding; r[10] = s->flags; r[11] = s->func;
        r[12] = s->filter; r[13] = s->ported; w16(r + 14, s->version);
    }
    for (unsigned i = 0; i < nv; i++, at += 36) {
        const VjoPlaceVerified *v = &verified[i]; uint8_t *r = out + at;
        w32(r, v->module_nid); w32(r + 4, v->off); w32(r + 8, (uint32_t)v->padding);
        r[12] = v->thumb; r[13] = v->reg; r[14] = v->encoding; r[15] = v->sig_len;
        memcpy(r + 16, v->sig, 16); r[32] = v->filter; r[33] = v->read; w16(r + 34, v->app_ver);
    }
    unsigned op_at = at + 4 * nf, op_n = 0;
    for (unsigned id = 1; id < 512; id++) if (programs[id]) {
        w16(out + at, id); w16(out + at + 2, op_n); at += 4;
        for (const LunaOp *o = programs[id]; ; o++, op_n++) {
            uint8_t *r = out + op_at + 8 * op_n;
            r[0] = o->op; r[1] = o->enc; r[2] = o->arg;
            w16(r + 4, string_add(o->a)); w16(r + 6, string_add(o->b));
            if (o->op == OP_END) { op_n++; break; }
        }
    }
    at = op_at + 8 * no; require(ns <= VJO_PROFILE_BYTES - at);
    memcpy(out + at, strings, ns); at += ns;
    w16(out + 64, ns); w32(out + 52, at);
    w32(out + 68, vjo_hook_profile_checksum(out + 72, at - 72));
    return at;
}

static int cmp(const void *a, const void *b) { return strcmp(a, b); }
int main(int argc, char **argv)
{
    char titles[512][12] = {{0}};
    uint8_t row[20], blob[VJO_PROFILE_BYTES], header[16] = "vjohkdb1";
    const VjoPlaceVerified *v;
    unsigned n = 0, max = 0, offset;
    FILE *f;
    require(argc == 2);
    for (unsigned i = 0; vjo_luna_title(i); i++) {
        require(n < 512); memcpy(titles[n++], vjo_luna_title(i), 9);
    }
    unsigned nv = vjo_place_verified_all(&v);
    for (unsigned i = 0; i < nv; i++) {
        unsigned j = 0; while (j < n && strcmp(titles[j], v[i].title)) j++;
        if (j == n) { require(n < 512); memcpy(titles[n++], v[i].title, 9); }
    }
    qsort(titles, n, sizeof(titles[0]), cmp);
    w32(header + 8, n); f = fopen(argv[1], "wb"); require(f != NULL);
    require(fwrite(header, 1, 16, f) == 16); offset = 16 + 20 * n;
    for (unsigned i = 0; i < n; i++) {
        unsigned size = profile(titles[i], blob); if (size > max) max = size;
        memset(row, 0, sizeof(row)); memcpy(row, titles[i], 9); w32(row + 12, offset); w32(row + 16, size);
        require(fwrite(row, 1, sizeof(row), f) == sizeof(row)); offset += size;
    }
    for (unsigned i = 0; i < n; i++) {
        unsigned size = profile(titles[i], blob); require(fwrite(blob, 1, size, f) == size);
    }
    require(fclose(f) == 0);
    printf("hook profiles: %u games, largest %u B, archive %u B\n", n, max, offset);
    return 0;
}
