/* The debug build's read-only PSP probe, its pure-C half (core/psp_probe.h). No libc call: it builds
 * in the worker (-nostdlib) and on the host alike. */
#include "psp_probe.h"

#include "hook_luna_psp.inc"

_Static_assert(sizeof(vjo_luna_psp_prefix) == VJO_LUNA_PSP_PREFIXES * 4u + 1u, "4 letters per prefix");
_Static_assert(VJO_LUNA_PSP_SITES_PER_TITLE_MAX <= VJO_PSP_SITES_MAX, "every site of a title is read");

int vjo_psp_block_ok(const VjoPspBlock *b)
{
    if (!b || b->uid < 0 || b->info_rc < 0 || !b->size) return 0;
    if (b->addr - b->base >= b->size) return 0; /* holds its own address (an address below base wraps) */
    return (b->access & VJO_PSP_ACCESS_R) != 0;
}

int vjo_psp_inside(const VjoPspBlock *blocks, unsigned n_blocks, uint32_t addr, uint32_t n)
{
    if (!blocks || !n || addr + (n - 1u) < addr) return 0;
    for (unsigned i = 0; i < n_blocks; i++) {
        const VjoPspBlock *b = &blocks[i];
        /* [addr, addr + n) within [base, base + size), written so that nothing overflows */
        if (vjo_psp_block_ok(b) && addr - b->base < b->size && n <= b->size - (addr - b->base)) return 1;
    }
    return 0;
}

static int is_id(const char *s)
{
    for (unsigned i = 0; i < 9; i++) {
        char c = s[i];
        if (i < 4 ? c < 'A' || c > 'Z' : c < '0' || c > '9') return 0; /* a NUL fails both */
    }
    return 1;
}

int vjo_psp_title_id(const uint8_t raw[VJO_PSP_TITLEID_BYTES], char id[10])
{
    id[0] = 0;
    if (!raw || !is_id((const char *)raw) || raw[9]) return 0;
    for (unsigned i = 0; i < 9; i++) id[i] = (char)raw[i];
    id[9] = 0;
    return 1;
}

/* A title ID (is_id) packed as core/hook_luna_psp.inc packs them; 0xFFFFFFFF when its prefix is none of
 * the table's (no title has that value: a prefix index is below 8) */
static uint32_t pack(const char *s)
{
    uint32_t number = 0;
    for (unsigned i = 4; i < 9; i++) number = number * 10u + (uint32_t)(s[i] - '0');
    for (unsigned p = 0; p < VJO_LUNA_PSP_PREFIXES; p++) {
        const char *q = &vjo_luna_psp_prefix[p * 4u];
        if (q[0] == s[0] && q[1] == s[1] && q[2] == s[2] && q[3] == s[3])
            return (uint32_t)p << VJO_LUNA_PSP_NUMBER_BITS | number;
    }
    return 0xFFFFFFFFu;
}

unsigned vjo_psp_luna_sites(const char *title_id, uint32_t *addr, unsigned cap, unsigned *total)
{
    unsigned lo = 0, hi = VJO_LUNA_PSP_TITLES, first = 0, n, k = VJO_LUNA_PSP_TITLES;
    uint32_t want;
    if (total) *total = 0;
    if (!title_id || !is_id(title_id)) return 0;
    want = pack(title_id);
    while (lo < hi) { /* the packed IDs are sorted */
        unsigned mid = lo + (hi - lo) / 2u;
        if (vjo_luna_psp_ids[mid] == want) {
            k = mid;
            break;
        }
        if (want < vjo_luna_psp_ids[mid]) hi = mid;
        else lo = mid + 1u;
    }
    if (k == VJO_LUNA_PSP_TITLES) return 0; /* not listed */
    for (unsigned i = 0; i < k; i++) first += vjo_luna_psp_count[i];
    n = vjo_luna_psp_count[k];
    if (total) *total = n;
    if (!addr) cap = 0;
    if (cap > VJO_PSP_SITES_MAX) cap = VJO_PSP_SITES_MAX;
    if (n > cap) n = cap;
    for (unsigned i = 0; i < n; i++) addr[i] = vjo_luna_psp_addr[first + i];
    return n;
}

/* ---- the log's lines: a bounded writer ---- */

typedef struct {
    char *out;
    size_t cap, n; /* n < cap whenever cap > 0: the NUL always fits */
} Line;

static void put_c(Line *l, char c)
{
    if (l->n + 1u < l->cap) l->out[l->n++] = c;
}

static void put_s(Line *l, const char *s)
{
    while (*s) put_c(l, *s++);
}

static void put_hex(Line *l, uint32_t v)
{
    static const char digits[] = "0123456789ABCDEF";
    put_s(l, "0x");
    for (int shift = 28; shift >= 0; shift -= 4) put_c(l, digits[(v >> shift) & 15u]);
}

static void put_word(Line *l, uint32_t v) /* 8 hex digits, no 0x */
{
    static const char digits[] = "0123456789ABCDEF";
    for (int shift = 28; shift >= 0; shift -= 4) put_c(l, digits[(v >> shift) & 15u]);
}

static void put_dec(Line *l, uint32_t v)
{
    char tmp[10];
    unsigned k = 0;
    do {
        tmp[k++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (k) put_c(l, tmp[--k]);
}

static Line begin(char *out, size_t cap)
{
    Line l = {out, out ? cap : 0, 0};
    put_s(&l, "psp probe ");
    put_dec(&l, VJO_PSP_LOG_FORMAT);
    put_s(&l, ": ");
    return l;
}

static size_t finish(Line *l)
{
    put_c(l, '\n');
    if (l->cap) l->out[l->n] = 0;
    return l->n;
}

size_t vjo_psp_line_start(char *out, size_t cap, uint32_t text_source)
{
    Line l = begin(out, cap);
    put_s(&l, "start: text source ");
    put_dec(&l, text_source);
    return finish(&l);
}

size_t vjo_psp_line_module(char *out, size_t cap, int32_t rc, uint32_t nid)
{
    Line l = begin(out, cap);
    put_s(&l, "ScePspemu: rc ");
    put_hex(&l, (uint32_t)rc);
    put_s(&l, " nid ");
    put_hex(&l, nid);
    return finish(&l);
}

size_t vjo_psp_line_block(char *out, size_t cap, const VjoPspBlock *b)
{
    static const VjoPspBlock none = {0, -1, -1, 0, 0, 0, 0, 0};
    Line l = begin(out, cap);
    if (!b) b = &none;
    put_s(&l, "block ");
    put_hex(&l, b->addr);
    put_s(&l, ": uid ");
    put_hex(&l, (uint32_t)b->uid);
    put_s(&l, " info ");
    put_hex(&l, (uint32_t)b->info_rc);
    put_s(&l, " base ");
    put_hex(&l, b->base);
    put_s(&l, " size ");
    put_hex(&l, b->size);
    put_s(&l, " memtype ");
    put_hex(&l, b->memtype);
    put_s(&l, " access ");
    put_hex(&l, b->access);
    put_s(&l, " type ");
    put_hex(&l, b->type);
    put_s(&l, vjo_psp_block_ok(b) ? " ok" : " bad");
    return finish(&l);
}

size_t vjo_psp_line_titleid(char *out, size_t cap, uint32_t vita_addr, int read, const uint8_t *raw)
{
    Line l = begin(out, cap);
    put_s(&l, "titleid ");
    put_hex(&l, vita_addr);
    if (!read || !raw) {
        put_s(&l, ": outside the blocks");
        return finish(&l);
    }
    put_s(&l, ": ");
    for (unsigned i = 0; i < VJO_PSP_TITLEID_BYTES; i++) {
        static const char digits[] = "0123456789ABCDEF";
        put_c(&l, digits[raw[i] >> 4]);
        put_c(&l, digits[raw[i] & 15u]);
    }
    put_s(&l, " \"");
    for (unsigned i = 0; i < VJO_PSP_TITLEID_BYTES && raw[i]; i++) /* up to the first NUL */
        put_c(&l, raw[i] >= 0x20 && raw[i] < 0x7F && raw[i] != '"' && raw[i] != '\\' ? (char)raw[i] : '.');
    put_c(&l, '"');
    return finish(&l);
}

size_t vjo_psp_line_sites(char *out, size_t cap, const char *id, unsigned total, unsigned read)
{
    Line l = begin(out, cap);
    put_s(&l, "luna ");
    for (unsigned i = 0; id && i < 9 && id[i]; i++) /* at most an ID's 9 characters */
        put_c(&l, id[i] > 0x20 && id[i] < 0x7F ? id[i] : '.');
    if (!total) {
        put_s(&l, ": not listed");
        return finish(&l);
    }
    put_s(&l, ": ");
    put_dec(&l, total);
    put_s(&l, " sites, ");
    put_dec(&l, read);
    put_s(&l, " read");
    return finish(&l);
}

size_t vjo_psp_line_site(char *out, size_t cap, uint32_t psp_addr, uint32_t vita_addr,
                         const uint32_t words[VJO_PSP_SITE_WORDS], unsigned mask)
{
    Line l = begin(out, cap);
    put_s(&l, "site ");
    put_hex(&l, psp_addr);
    put_s(&l, " at ");
    put_hex(&l, vita_addr);
    put_c(&l, ':');
    for (unsigned k = 0; k < VJO_PSP_SITE_WORDS; k++) {
        put_c(&l, ' ');
        if (words && (mask >> k & 1u)) put_word(&l, words[k]);
        else put_s(&l, "--------");
    }
    return finish(&l);
}

size_t vjo_psp_line_end(char *out, size_t cap, const char *why)
{
    Line l = begin(out, cap);
    if (why) {
        put_s(&l, "stop: ");
        for (unsigned i = 0; why[i] && i < 96u; i++) put_c(&l, why[i]); /* a short reason */
    } else {
        put_s(&l, "done");
    }
    return finish(&l);
}
