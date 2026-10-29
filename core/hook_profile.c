#include "hook_profile.h"
#include <string.h>

static VjoHookProfile *current;
static unsigned u16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }
static uint32_t u32(const uint8_t *p) { return u16(p) | (uint32_t)u16(p + 2) << 16; }

/* Corruption check, not an authenticity claim. Profiles are also tied to this
 * build's definition revision; placement still checks the game's NID/bytes. */
uint32_t vjo_hook_profile_checksum(const uint8_t *p, unsigned n)
{
    uint32_t h = 2166136261u;
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}

int vjo_hook_profile_revision(uint8_t out[32])
{
#ifdef VJO_HOOK_DEFINITION_SHA256
    const char *s = VJO_HOOK_DEFINITION_SHA256;
    for (unsigned i = 0; i < 32; i++) {
        unsigned v = 0;
        for (unsigned j = 0; j < 2; j++) {
            unsigned c = (unsigned char)s[2 * i + j];
            if (c >= '0' && c <= '9') c -= '0';
            else if (c >= 'a' && c <= 'f') c = c - 'a' + 10;
            else return 0;
            v = (v << 4) | c;
        }
        out[i] = (uint8_t)v;
    }
    return s[64] == 0;
#else
    (void)out;
    return 0;
#endif
}

const VjoHookProfile *vjo_hook_profile_current(void) { return current; }
void vjo_hook_profile_clear(void) { current = NULL; }

static unsigned filter_at(const VjoHookProfile *p, unsigned id)
{
    for (unsigned i = 0; i < p->nf; i++) {
        const uint8_t *r = p->raw + p->filters + 4 * i;
        if (u16(r) == id) return p->ops + 8 * u16(r + 2);
    }
    return 0;
}
unsigned vjo_hook_profile_filter(unsigned id) { return current ? filter_at(current, id) : 0; }

int vjo_hook_profile_op(unsigned offset, LunaOp *out)
{
    const uint8_t *r;
    if (!current || offset < current->ops || offset >= current->strings || (offset - current->ops) % 8) return 0;
    r = current->raw + offset;
    out->op = r[0]; out->enc = r[1]; out->arg = r[2];
    out->a = (const char *)current->raw + current->strings + u16(r + 4);
    out->b = (const char *)current->raw + current->strings + u16(r + 6);
    return 1;
}

int vjo_hook_profile_parse(VjoHookProfile *p, const char *title, unsigned n)
{
    uint8_t revision[32];
    const uint8_t *r = p->raw;
    unsigned at, ns;
    current = NULL;
    p->nl = p->nv = p->nf = p->no = 0;
    if (!title || strlen(title) != 9 || n < VJO_PROFILE_HEADER || n > sizeof(p->raw) ||
        memcmp(r, "vjohook\1", 8) || memcmp(r + 8, title, 9) || r[17] || r[18] || r[19] ||
        !vjo_hook_profile_revision(revision) || memcmp(r + 20, revision, 32) || u32(r + 52) != n ||
        r[66] || r[67] || u32(r + 68) != vjo_hook_profile_checksum(r + 72, n - 72)) return 0;
    p->nl = u16(r + 56); p->nv = u16(r + 58); p->nf = u16(r + 60); p->no = u16(r + 62); ns = u16(r + 64);
    if (p->nl > VJO_PROFILE_LUNA_MAX || p->nv > VJO_PROFILE_VERIFIED_MAX || !ns ||
        p->nf > 512 || p->no > VJO_PROFILE_BYTES / 8) goto bad;
    at = VJO_PROFILE_HEADER;
    p->filters = at + 18 * p->nl + 36 * p->nv;
    p->ops = p->filters + 4 * p->nf;
    p->strings = p->ops + 8 * p->no;
    if (p->strings + ns != n || r[p->strings] || r[n - 1]) goto bad;
    for (unsigned i = 0; i < p->nf; i++) {
        const uint8_t *f = r + p->filters + 4 * i;
        unsigned id = u16(f), start = u16(f + 2), end = i + 1 < p->nf ? u16(f + 6) : p->no;
        if (!id || id >= 512 || (i && id <= u16(f - 4)) || start >= end || end > p->no ||
            (!i && start) || r[p->ops + 8 * (end - 1)] != OP_END) goto bad;
    }
    if ((!p->nf) != (!p->no)) goto bad;
    for (unsigned i = 0; i < p->no; i++) {
        const uint8_t *o = r + p->ops + 8 * i;
        unsigned a = u16(o + 4), b = u16(o + 6);
        if (o[0] > OP_BETWEEN || o[3] || a >= ns || b >= ns ||
            !memchr(r + p->strings + a, 0, ns - a) || !memchr(r + p->strings + b, 0, ns - b) ||
            (o[0] == OP_CALL && !filter_at(p, o[2]))) goto bad;
    }
    for (unsigned i = 0; i < p->nl; i++, at += 18) {
        VjoLunaSite *s = &p->luna[i]; const uint8_t *x = r + at;
        memset(s, 0, sizeof(*s));
        s->addr = u32(x); s->padding = (int32_t)u32(x + 4);
        s->reg = x[8]; s->encoding = x[9]; s->flags = x[10]; s->func = x[11];
        s->filter = x[12]; s->ported = x[13]; s->version = (uint16_t)u16(x + 14);
        if (x[16] || x[17] || s->reg > 15 || s->ported > 1 ||
            (s->ported && (s->encoding < VJO_HOOK_ENC_UTF8 || s->encoding > VJO_HOOK_ENC_CP932 ||
             (s->filter && !filter_at(p, s->filter))))) goto bad;
    }
    for (unsigned i = 0; i < p->nv; i++, at += 36) {
        VjoPlaceVerified *v = &p->verified[i]; const uint8_t *x = r + at;
        memset(v, 0, sizeof(*v)); v->title = (const char *)r + 8;
        v->module_nid = u32(x); v->off = u32(x + 4); v->padding = (int32_t)u32(x + 8);
        v->thumb = x[12]; v->reg = x[13]; v->encoding = x[14]; v->sig_len = x[15];
        memcpy(v->sig, x + 16, 16); v->filter = x[32]; v->read = x[33]; v->app_ver = (uint16_t)u16(x + 34);
        if (v->thumb > 1 || v->reg > 15 || v->sig_len < 8 || v->sig_len > 16 ||
            v->read > VJO_PLACE_READ_DELTARUNE || !v->encoding ||
            (v->read == VJO_PLACE_READ_DELTARUNE && v->encoding != VJO_HOOK_ENC_UTF8) ||
            (v->filter && !filter_at(p, v->filter))) goto bad;
    }
    current = p;
    return 1;
bad:
    p->nl = p->nv = p->nf = p->no = 0;
    return 0;
}
