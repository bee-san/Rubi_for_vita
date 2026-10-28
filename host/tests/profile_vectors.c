/* Runs unchanged against the built-in catalogue and the shipped external mode.
 * The Python suite compares every field and decoded output, title by title. */
#include "../../core/hook_profile.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t *p; unsigned n, reads, bytes; int short_read; } File;
static uint32_t u32(const uint8_t *p)
{ return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int read_at(void *ud, void *dst, unsigned n, uint32_t off)
{
    File *f = ud; f->reads++; f->bytes += n;
    if (off > f->n || n > f->n - off || f->short_read) return 0;
    memcpy(dst, f->p + off, n); return (int)n;
}
static void hex(const void *data, size_t n)
{
    const uint8_t *p = data;
    while (n--) printf("%02x", *p++);
    putchar('\n');
}
static void render(const VjoLunaSite *s, const VjoPlaceVerified *v)
{
    static const char *const text[] = {
        "今日はいい天気ですね。", "【名前】#n#本文\\n続き^終わり", "#Pos[0,42]#Speed[5]本文#n次",
        "#Ruby[漢字,かんじ]本文", "<br>本文<br>次", "＊Ａ＊Ｂ、＊Ｂさん。", "$t名前@本文$d$[漢字$/かんじ$]",
        "@vvoice.@r本文@p500.@r次", "c1.名前rvvoice.本文w200.bよみ.<漢字>", "㌫㍊㊤㊥㊦｢ｿﾛｿﾛ､行ｶﾅｷｬ｣",
        "!! !r !v !b !k !y !z !u ![ !] !| !< !> !. u9829.", "r本文\\n次\\d#STAND\\pose#@mA0b1C2@0\\u$",
        "<K><P><N><F:B>本文", "｜漢字《かんじ》と漢字《かんじ》", "#Name[1]#Name[2]#Name[3]本文",
        "<a>(｢)本文<ruby>(2,ﾖﾐ)漢字", "[n][xFF][w123]本文", "文字×本文㌻続き", "ASCII only", "　本文　\n"
    };
    char out[1536];
    for (unsigned i = 0; i < sizeof(text) / sizeof(text[0]); i++) {
        size_t n = v ? vjo_luna_text_filter(NULL, v->filter, v->encoding, (const uint8_t *)text[i],
                                           (unsigned)strlen(text[i]), VJO_HOOK_ENC_UTF8, out, sizeof(out))
                     : vjo_luna_text(s, (const uint8_t *)text[i], (unsigned)strlen(text[i]),
                                     VJO_HOOK_ENC_UTF8, out, sizeof(out));
        hex(out, n);
    }
}
int main(int argc, char **argv)
{
    FILE *fd;
    File f = {0};
    VjoHookProfile p;
    unsigned count;
    assert(argc == 2);
    fd = fopen(argv[1], "rb"); assert(fd);
    assert(!fseek(fd, 0, SEEK_END)); long size = ftell(fd); assert(size > 0);
    f.n = (unsigned)size; f.p = malloc(f.n); assert(f.p);
    rewind(fd); assert(fread(f.p, 1, f.n, fd) == f.n); fclose(fd);
    count = u32(f.p + 8);
    for (unsigned i = 0; i < count; i++) {
        const char *title = (const char *)f.p + 16 + 20 * i;
        const VjoLunaSite *s; const VjoPlaceVerified *v;
        f.reads = f.bytes = 0;
        int n = vjo_hook_profile_read(read_at, &f, f.n, title, p.raw, sizeof(p.raw));
        assert(n > 0 && f.reads <= 11 && f.bytes <= 16 + 20 * 9 + VJO_PROFILE_BYTES);
        assert(vjo_hook_profile_parse(&p, title, (unsigned)n));
        unsigned nl = vjo_luna_sites(title, &s), nv = vjo_place_verified(title, &v);
        printf("%.9s %u %u\n", title, nl, nv);
        for (unsigned j = 0; j < nl; j++) {
            printf("S %u %d %u %u %u %u %u %u %u %d\n", s[j].addr, s[j].padding, s[j].reg, s[j].encoding,
                   s[j].flags, s[j].func, s[j].filter, s[j].ported, s[j].version, vjo_luna_ported(s + j));
            render(s + j, NULL);
        }
        for (unsigned j = 0; j < nv; j++) {
            printf("V %s %u %u %d %u %u %u %u %u %u %u\n", v[j].title, v[j].module_nid, v[j].off,
                   v[j].padding, v[j].thumb, v[j].reg, v[j].encoding, v[j].sig_len, v[j].filter,
                   v[j].read, v[j].app_ver);
            hex(v[j].sig, 16); render(NULL, v + j);
        }
#ifdef VJO_EXTERNAL_HOOK_PROFILES
        assert(vjo_luna_sites(title, NULL) == nl);
        assert(!vjo_luna_sites(NULL, NULL));
        assert(!vjo_luna_sites("TEST00000", &s));
        assert(!vjo_place_verified("TEST00000", &v));
        uint8_t valid[VJO_PROFILE_BYTES]; memcpy(valid, p.raw, (unsigned)n);
        for (unsigned cut = 0; cut < (unsigned)n; cut++) {
            memcpy(p.raw, valid, (unsigned)n);
            assert(!vjo_hook_profile_parse(&p, title, cut));
            assert(!vjo_hook_profile_current());
        }
        for (unsigned byte = 0; byte < (unsigned)n; byte++) {
            memcpy(p.raw, valid, (unsigned)n); p.raw[byte] ^= 0x80;
            assert(!vjo_hook_profile_parse(&p, title, (unsigned)n));
        }
        /* Structural refusals after a valid checksum, beyond corruption detection. */
        memcpy(p.raw, valid, (unsigned)n);
        assert(vjo_hook_profile_parse(&p, title, (unsigned)n));
        if (p.nf) {
            unsigned filters = p.filters, ops = p.ops, end = p.strings;
            unsigned positions[] = {filters + 2, ops + 4, end - 8};
            for (unsigned k = 0; k < 3; k++) {
                memcpy(p.raw, valid, (unsigned)n);
                if (k < 2) p.raw[positions[k]] = p.raw[positions[k] + 1] = 0xff;
                else p.raw[positions[k]] = OP_BETWEEN; /* last op must terminate */
                uint32_t sum = vjo_hook_profile_checksum(p.raw + 72, (unsigned)n - 72);
                for (unsigned b = 0; b < 4; b++) p.raw[68 + b] = (uint8_t)(sum >> (8 * b));
                assert(!vjo_hook_profile_parse(&p, title, (unsigned)n));
            }
        }
        memcpy(p.raw, valid, (unsigned)n);
        assert(!vjo_hook_profile_parse(&p, "TEST00000", (unsigned)n));
        assert(vjo_hook_profile_parse(&p, title, (unsigned)n));
#endif
    }
    assert(!vjo_hook_profile_read(read_at, &f, f.n, "TEST00000", p.raw, sizeof(p.raw)));
    f.short_read = 1;
    assert(vjo_hook_profile_read(read_at, &f, f.n, "PCSG00415", p.raw, sizeof(p.raw)) < 0);
    f.short_read = 0;
    for (unsigned n = 0; n < 16; n++)
        assert(vjo_hook_profile_read(read_at, &f, n, "PCSG00415", p.raw, sizeof(p.raw)) < 0);
    f.p[8] = 0xff; f.p[9] = 0xff;
    assert(vjo_hook_profile_read(read_at, &f, f.n, "PCSG00415", p.raw, sizeof(p.raw)) < 0);
    free(f.p);
    return 0;
}
