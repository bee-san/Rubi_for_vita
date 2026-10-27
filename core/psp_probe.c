/* The debug build's read-only PSP probe, its pure-C half (core/psp_probe.h). No libc call: it builds
 * in the worker (-nostdlib) and on the host alike. */
#include "psp_probe.h"

#include "hook_luna_psp.inc"

_Static_assert(sizeof(vjo_luna_psp_prefix) == VJO_LUNA_PSP_PREFIXES * 4u + 1u, "4 letters per prefix");
_Static_assert(VJO_LUNA_PSP_SITES_PER_TITLE_MAX <= VJO_PSP_SITES_MAX, "every site of a title is read");
_Static_assert(VJO_PSP_READ_BYTES % VJO_PSP_PAGE == 0 && VJO_PSP_READ_BYTES % 16u == 0, "whole pages");

#define TITLE_ADDR (VJO_PSP_VIEW_ADRENALINE + VJO_PSP_TITLEID_OFFSET)
#define ALL_BLOCKS ((1u << VJO_PSP_BLOCKS) - 1u)

int vjo_psp_block_ok(const VjoPspBlock *b)
{
    if (!b || (b->uid < 0 && b->uid != VJO_PSP_INVALID_UID) || b->info_rc != 0 || !b->size) return 0;
    if (b->size - 1u > 0xFFFFFFFFu - b->base) return 0; /* reject a wrapping mapping */
    if (b->addr - b->base >= b->size) return 0; /* holds its own address (an address below base wraps) */
    return (b->access & VJO_PSP_ACCESS_R) != 0;
}

int vjo_psp_holder(const VjoPspBlock *blocks, unsigned n_blocks, uint32_t addr, uint32_t n)
{
    if (!blocks || !n || addr + (n - 1u) < addr) return -1;
    for (unsigned i = 0; i < n_blocks; i++) {
        const VjoPspBlock *b = &blocks[i];
        /* [addr, addr + n) within [base, base + size), written so that nothing overflows */
        if (vjo_psp_block_ok(b) && addr - b->base < b->size && n <= b->size - (addr - b->base)) return (int)i;
    }
    return -1;
}

int vjo_psp_inside(const VjoPspBlock *blocks, unsigned n_blocks, uint32_t addr, uint32_t n)
{
    return vjo_psp_holder(blocks, n_blocks, addr, n) >= 0;
}

static int is_id(const char *s)
{
    for (unsigned i = 0; i < 9; i++) {
        char c = s[i];
        if (i < 4 ? c < 'A' || c > 'Z' : c < '0' || c > '9') return 0; /* a NUL fails both */
    }
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

/* ---- the MIPS site classifier (MIPS32 encodings; Allegrex's VFPU branches are COP2's BC2 form) ---- */

#define OP(w) ((w) >> 26)
#define RS(w) (((w) >> 21) & 31u)
#define RT(w) (((w) >> 16) & 31u)
#define FUNCT(w) ((w) & 63u)

int vjo_mips_is_cti(uint32_t w)
{
    switch (OP(w)) {
    case 0x00: return FUNCT(w) == 0x08u || FUNCT(w) == 0x09u; /* SPECIAL: jr, jalr */
    case 0x01: return (RT(w) & 0x0Cu) == 0;                    /* REGIMM rt 0-3, 16-19: bltz ... bgezall */
    case 0x02: case 0x03:                                      /* j, jal */
    case 0x04: case 0x05: case 0x06: case 0x07:                /* beq, bne, blez, bgtz */
    case 0x14: case 0x15: case 0x16: case 0x17: return 1;      /* beql, bnel, blezl, bgtzl */
    case 0x10: case 0x11: case 0x12: return RS(w) == 0x08u;    /* COPz rs BC: bc1* (FPU), bc2* (bvf, bvt);
                                                                  * bc0*, invalid on Allegrex, to be safe */
    default: return 0;
    }
}

int vjo_mips_is_call(uint32_t w)
{
    return OP(w) == 0x03u || (OP(w) == 0x00u && FUNCT(w) == 0x09u) || (OP(w) == 0x01u && (RT(w) & 0x1Cu) == 0x10u);
}

int vjo_mips_unsafe(uint32_t w)
{
    if (vjo_mips_is_cti(w)) return 1;
    if (OP(w) == 0x00u && (FUNCT(w) == 0x0Cu || FUNCT(w) == 0x0Du)) return 1; /* syscall, break */
    return OP(w) == 0x10u || OP(w) == 0x30u || OP(w) == 0x38u;               /* COP0 (eret ...), ll, sc */
}

int vjo_mips_is_frame(uint32_t w)
{
    return (w & 0xFFFF0000u) == 0x27BD0000u && (w & 0x8000u) != 0; /* addiu sp, sp, imm < 0 */
}

unsigned vjo_psp_classify(const uint32_t words[VJO_PSP_SITE_WORDS], unsigned mask)
{
    if (!words || (mask & 0xFu) != 0xFu) return VJO_PSP_CLASS_UNREAD;
    if (!(words[0] | words[1] | words[2] | words[3])) return VJO_PSP_CLASS_ZERO;
    if (vjo_mips_is_cti(words[1])) return VJO_PSP_CLASS_DELAY;
    if (vjo_mips_unsafe(words[2]) || vjo_mips_unsafe(words[3])) return VJO_PSP_CLASS_BRANCH;
    if (vjo_mips_is_frame(words[2])) return VJO_PSP_CLASS_ENTRY;
    if (vjo_mips_is_call(words[0])) return VJO_PSP_CLASS_RETURN;
    return VJO_PSP_CLASS_OTHER;
}

const char *vjo_psp_class_name(unsigned cls)
{
    static const char *const names[VJO_PSP_CLASSES] = {"unread", "zero", "delay", "branch", "entry", "return",
                                                       "other"};
    return cls < VJO_PSP_CLASSES ? names[cls] : "?";
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

static void put_word(Line *l, uint32_t v) /* 8 hex digits, no 0x */
{
    static const char digits[] = "0123456789ABCDEF";
    for (int shift = 28; shift >= 0; shift -= 4) put_c(l, digits[(v >> shift) & 15u]);
}

static void put_hex(Line *l, uint32_t v)
{
    put_s(l, "0x");
    put_word(l, v);
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

static void put_sdec(Line *l, int32_t v)
{
    if (v < 0) put_c(l, '-');
    put_dec(l, v < 0 ? 0u - (uint32_t)v : (uint32_t)v);
}

/* " <name> <decimal>" */
static void put_kv(Line *l, const char *name, uint32_t v)
{
    put_c(l, ' ');
    put_s(l, name);
    put_c(l, ' ');
    put_dec(l, v);
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

size_t vjo_psp_line_process(char *out, size_t cap, const char *title, uint32_t word)
{
    Line l = begin(out, cap);
    put_s(&l, "process ");
    for (unsigned i = 0; title && i < 12 && title[i]; i++) /* a title ID's 9 characters, at most 12 */
        put_c(&l, title[i] > 0x20 && title[i] < 0x7F ? title[i] : '.');
    put_s(&l, ", text source ");
    put_dec(&l, word);
    return finish(&l);
}

size_t vjo_psp_line_start(char *out, size_t cap, unsigned pass, uint32_t job_seq)
{
    Line l = begin(out, cap);
    put_s(&l, "start: pass ");
    put_dec(&l, pass);
    put_s(&l, " after job ");
    put_dec(&l, job_seq);
    return finish(&l);
}

size_t vjo_psp_line_module(char *out, size_t cap, const VjoPspModule *m)
{
    static const VjoPspModule none = {-1, 0, 0, 0, 0, 0};
    Line l = begin(out, cap);
    if (!m) m = &none;
    put_s(&l, "ScePspemu: rc ");
    put_hex(&l, (uint32_t)m->rc);
    put_s(&l, " nid ");
    put_hex(&l, m->nid);
    put_s(&l, " modid ");
    put_hex(&l, (uint32_t)m->modid);
    put_s(&l, " info ");
    put_hex(&l, (uint32_t)m->info_rc);
    put_s(&l, " seg0 ");
    put_hex(&l, m->seg0);
    put_s(&l, " size ");
    put_hex(&l, m->seg0_size);
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
    put_s(&l, b->memtype == VJO_PSP_MEMTYPE_CACHED ? " (cached)" : b->memtype == VJO_PSP_MEMTYPE_UNCACHED ? " (uncached)"
                                                                                                       : " (?)");
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
    put_c(&l, ' ');
    put_s(&l, vjo_psp_class_name(vjo_psp_classify(words, mask)));
    return finish(&l);
}

static uint32_t fnv1a(const uint8_t *p, uint32_t n)
{
    uint32_t h = 0x811C9DC5u;
    for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x01000193u;
    return h;
}

size_t vjo_psp_line_profile(char *out, size_t cap, const char *id, const VjoPspProfile *pp,
                            const uint32_t words[VJO_PSP_CODE_WORDS], unsigned mask, const uint8_t *buf, uint32_t n)
{
    Line l = begin(out, cap);
    unsigned same = words && (mask & 0xFu) == 0xFu;
    uint32_t k = 0;
    put_s(&l, "rubi ");
    for (unsigned i = 0; id && i < 9 && id[i]; i++) put_c(&l, id[i] > 0x20 && id[i] < 0x7F ? id[i] : '.');
    if (!pp) {
        put_s(&l, ": no profile");
        return finish(&l);
    }
    put_s(&l, ": site ");
    put_hex(&l, pp->site);
    put_s(&l, " at ");
    put_hex(&l, vjo_psp_view(pp->site));
    put_c(&l, ':');
    for (unsigned i = 0; i < VJO_PSP_CODE_WORDS; i++) {
        put_c(&l, ' ');
        if (words && (mask >> i & 1u)) put_word(&l, words[i]);
        else put_s(&l, "--------");
        if (!words || !(mask >> i & 1u) || words[i] != pp->code[i]) same = 0;
    }
    put_s(&l, (mask & 0xFu) != 0xFu ? " unread" : same ? " the farm's" : " not the farm's");
    put_s(&l, ", buffer ");
    put_hex(&l, pp->buffer);
    put_s(&l, ": ");
    if (!buf) {
        put_s(&l, "unread");
        return finish(&l);
    }
    put_dec(&l, n);
    put_s(&l, " bytes, ");
    while (k < n && buf[k]) k++;
    if (k == n) {
        put_s(&l, "no NUL");
        return finish(&l);
    }
    put_s(&l, "NUL at ");
    put_dec(&l, k);
    put_s(&l, ", hash ");
    put_hex(&l, fnv1a(buf, k));
    return finish(&l);
}

size_t vjo_psp_line_classes(char *out, size_t cap, const unsigned *count)
{
    /* the report's four first (entry, return, branch, delay), then the rest */
    static const unsigned order[VJO_PSP_CLASSES] = {VJO_PSP_CLASS_ENTRY, VJO_PSP_CLASS_RETURN, VJO_PSP_CLASS_BRANCH,
                                                    VJO_PSP_CLASS_DELAY, VJO_PSP_CLASS_OTHER, VJO_PSP_CLASS_ZERO,
                                                    VJO_PSP_CLASS_UNREAD};
    Line l = begin(out, cap);
    put_s(&l, "classes:");
    for (unsigned i = 0; i < VJO_PSP_CLASSES; i++) put_kv(&l, vjo_psp_class_name(order[i]), count ? count[order[i]] : 0u);
    return finish(&l);
}

static void put_read_head(Line *l, uint32_t addr, uint32_t n)
{
    put_s(l, "read ");
    put_hex(l, addr);
    put_s(l, ": ");
    put_hex(l, n);
    put_s(l, " bytes");
}

size_t vjo_psp_line_read_check(char *out, size_t cap, uint32_t addr, uint32_t n, int outside, unsigned pages,
                               uint32_t bad_page, int32_t bad_uid, int32_t block_uid, uint32_t check_us)
{
    Line l = begin(out, cap);
    put_read_head(&l, addr, n);
    if (outside) {
        put_s(&l, ": outside the blocks");
    } else if (pages < n / VJO_PSP_PAGE) {
        put_s(&l, ": page ");
        put_hex(&l, bad_page);
        put_s(&l, " in block ");
        put_hex(&l, (uint32_t)bad_uid);
        put_s(&l, ", not ");
        put_hex(&l, (uint32_t)block_uid);
        put_s(&l, ": not read");
    } else {
        put_s(&l, ", ");
        put_dec(&l, pages);
        put_s(&l, " pages in readable mapping, uid ");
        put_hex(&l, (uint32_t)block_uid);
        put_s(&l, " (");
        put_dec(&l, check_us);
        put_s(&l, " us): reading");
    }
    return finish(&l);
}

size_t vjo_psp_line_read_done(char *out, size_t cap, uint32_t addr, uint32_t n, uint32_t read_us, uint32_t sum)
{
    Line l = begin(out, cap);
    put_read_head(&l, addr, n);
    put_s(&l, " in ");
    put_dec(&l, read_us);
    put_s(&l, " us, sum ");
    put_hex(&l, sum);
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

/* "job: no result" (the runner writes no job line without one) */
static size_t no_result(Line *l)
{
    put_s(l, "job: no result");
    return finish(l);
}

size_t vjo_psp_line_job_capture(char *out, size_t cap, const VjoGameOcrResult *r, uint32_t flags, uint32_t speed)
{
    Line l = begin(out, cap);
    if (!r) return no_result(&l);
    put_s(&l, "job ");
    put_dec(&l, r->seq);
    put_s(&l, ": rc ");
    put_sdec(&l, r->rc);
    put_s(&l, ", flags ");
    put_hex(&l, flags);
    put_s(&l, " speed ");
    put_hex(&l, speed);
    put_s(&l, ", capture src ");
    put_dec(&l, r->source);
    put_kv(&l, "mem", r->capture_memory);
    put_kv(&l, "block", r->capture_bytes);
    put_s(&l, ", fb ");
    put_dec(&l, r->fb_width);
    put_c(&l, 'x');
    put_dec(&l, r->fb_height);
    put_kv(&l, "pitch", r->fb_pitch);
    put_s(&l, " fmt ");
    put_hex(&l, r->fb_format);
    put_s(&l, ", region ");
    put_dec(&l, r->width);
    put_c(&l, 'x');
    put_dec(&l, r->height);
    put_s(&l, " at ");
    put_dec(&l, r->region_x);
    put_c(&l, ',');
    put_dec(&l, r->region_y);
    put_s(&l, ",");
    put_kv(&l, "rows", r->strip_rows);
    put_kv(&l, "strips", r->captures);
    put_kv(&l, "tears", r->tears);
    put_kv(&l, "in", r->capture_us);
    put_s(&l, " us, sum ");
    put_hex(&l, r->region_sum);
    return finish(&l);
}

const char *vjo_psp_text_name(unsigned state)
{
    static const char *const names[VJO_PSP_TEXT_STATES] = {"off", "no view", "no title", "unlisted", "code differs",
                                                           "no NUL", "unstable", "empty", "ok"};
    return state < VJO_PSP_TEXT_STATES ? names[state] : "?";
}

size_t vjo_psp_line_job_ocr(char *out, size_t cap, const VjoGameOcrResult *r, uint32_t job_us, uint32_t psp_text)
{
    static const char *const weights[] = {"none", "read", "resident"};
    uint32_t chars = 0, hash = 0x811C9DC5u; /* FNV-1a */
    Line l = begin(out, cap);
    if (!r) return no_result(&l);
    for (size_t i = 0; i < sizeof(r->text) && r->text[i]; i++) {
        uint8_t c = (uint8_t)r->text[i];
        chars += (c & 0xC0u) != 0x80u; /* a UTF-8 character's first byte */
        hash = (hash ^ c) * 0x01000193u;
    }
    put_s(&l, "job ");
    put_dec(&l, r->seq);
    put_s(&l, " ocr:");
    put_kv(&l, "lines", r->lines);
    put_kv(&l, "rec", r->recognized);
    put_kv(&l, "frames", r->frames);
    put_s(&l, ", us");
    put_kv(&l, "find", r->find_us);
    put_kv(&l, "load", r->load_us);
    put_kv(&l, "rec", r->recognize_us);
    put_kv(&l, "job", job_us);
    put_s(&l, ", free ");
    put_sdec(&l, r->free_user);
    put_s(&l, " need ");
    put_dec(&l, r->need_workspace);
    put_c(&l, '+');
    put_dec(&l, r->need_weights);
    put_kv(&l, "reserve", r->reserve_bytes);
    put_kv(&l, "fallback", r->fallback);
    put_s(&l, ", weights ");
    put_s(&l, r->weights < 3u ? weights[r->weights] : "?");
    put_kv(&l, "resident", r->resident_bytes);
    put_s(&l, ", from "); /* the stream whose text it is: 0 OCR, 1 PSP text (core/psp_text.h) */
    put_dec(&l, r->text_from);
    put_s(&l, " flags ");
    put_hex(&l, r->hook_flags);
    put_kv(&l, "score", r->hook_score);
    put_s(&l, ", psp ");
    put_s(&l, vjo_psp_text_name(psp_text));
    put_s(&l, ", chars ");
    put_dec(&l, chars);
    put_s(&l, " hash ");
    put_hex(&l, hash);
    return finish(&l);
}

size_t vjo_psp_line_jobs_cap(char *out, size_t cap, unsigned jobs)
{
    Line l = begin(out, cap);
    put_s(&l, "jobs: ");
    put_dec(&l, jobs);
    put_s(&l, ", the most per process: no more job lines");
    return finish(&l);
}

/* ---- the runner ---- */

static void say(const VjoPspOps *o, const char *line, size_t n)
{
    if (o->put && n) o->put(o->ud, line, n);
}

static uint64_t now(const VjoPspOps *o)
{
    return o->now_us ? o->now_us(o->ud) : 0u;
}

static uint32_t elapsed(uint64_t from, uint64_t to)
{
    uint64_t d = to > from ? to - from : 0u;
    return d > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)d;
}

/* Step 1's lookups at the three views: the blocks, and what a pass compares (no read of PSP RAM). */
static void lookup(const VjoPspOps *o, VjoPspBlock blocks[VJO_PSP_BLOCKS], VjoPspSeen *seen)
{
    static const uint32_t views[VJO_PSP_BLOCKS] = {VJO_PSP_VIEW_RAM, VJO_PSP_VIEW_USER, VJO_PSP_VIEW_ADRENALINE};
    VjoPspSeen none = {0};
    *seen = none;
    for (unsigned i = 0; i < VJO_PSP_BLOCKS; i++) {
        VjoPspBlock *b = &blocks[i], empty = {views[i], -1, -1, 0, 0, 0, 0, 0};
        *b = empty;
        if (o->block) o->block(o->ud, views[i], b);
        b->addr = views[i]; /* the address asked about, whatever the op wrote there */
        if (vjo_psp_block_ok(b)) seen->ok |= 1u << i;
        seen->uid[i] = b->uid;
        seen->base[i] = b->base;
        seen->size[i] = b->size;
    }
}

/* The 12 title ID bytes, read when all three blocks passed and a block holds them all. */
static int title_readable(const VjoPspBlock *blocks, const VjoPspSeen *seen)
{
    return seen->ok == ALL_BLOCKS && vjo_psp_inside(blocks, VJO_PSP_BLOCKS, TITLE_ADDR, VJO_PSP_TITLEID_BYTES);
}

static int same(const VjoPspSeen *a, const VjoPspSeen *b)
{
    if (a->ok != b->ok || a->title_read != b->title_read) return 0;
    for (unsigned i = 0; i < VJO_PSP_BLOCKS; i++)
        if (a->uid[i] != b->uid[i] || a->base[i] != b->base[i] || a->size[i] != b->size[i]) return 0;
    for (unsigned i = 0; a->title_read && i < VJO_PSP_TITLEID_BYTES; i++)
        if (a->title[i] != b->title[i]) return 0;
    return 1;
}

/* Step 4: the 1 MiB at VJO_PSP_READ_ADDR, when a block holds it all and each of its pages' lookups names that
 * block; the lookups, then the read, timed apart, the first line out before the read begins. */
static void timed_read(const VjoPspOps *o, const VjoPspBlock *blocks)
{
    const uint32_t addr = VJO_PSP_READ_ADDR, n = VJO_PSP_READ_BYTES;
    char line[VJO_PSP_LINE_MAX];
    int h = vjo_psp_holder(blocks, VJO_PSP_BLOCKS, addr, n);
    unsigned pages = 0;
    uint64_t t0, t1, t2;
    uint32_t sum;
    if (h < 0 || !o->sum || !o->find) {
        say(o, line, vjo_psp_line_read_check(line, sizeof(line), addr, n, 1, 0, 0, 0, 0, 0));
        return;
    }
    t0 = now(o);
    for (; pages < n / VJO_PSP_PAGE; pages++) {
        uint32_t page = addr + pages * VJO_PSP_PAGE;
        int32_t uid = o->find(o->ud, page);
        if (uid != blocks[h].uid) {
            say(o, line, vjo_psp_line_read_check(line, sizeof(line), addr, n, 0, pages, page, uid, blocks[h].uid, 0));
            return;
        }
        if (uid == VJO_PSP_INVALID_UID) {
            VjoPspBlock b = {page, -1, -1, 0, 0, 0, 0, 0};
            if (o->block) o->block(o->ud, page, &b);
            b.addr = page;
            /* Shared mappings may expose no user UID. Identical error codes are not a page check:
             * require fresh readable info, the complete page, and the original mapping's identity. */
            if (!vjo_psp_inside(&b, 1, page, VJO_PSP_PAGE) || b.uid != uid ||
                b.base != blocks[h].base || b.size != blocks[h].size || b.type != blocks[h].type ||
                b.memtype != blocks[h].memtype || b.access != blocks[h].access) {
                say(o, line, vjo_psp_line_block(line, sizeof(line), &b));
                say(o, line, vjo_psp_line_end(line, sizeof(line), "page mapping changed or unreadable: not read"));
                return;
            }
        }
    }
    t1 = now(o);
    say(o, line, vjo_psp_line_read_check(line, sizeof(line), addr, n, 0, pages, 0, 0, blocks[h].uid, elapsed(t0, t1)));
    t1 = now(o); /* the line's write is not the read's time */
    sum = o->sum(o->ud, addr, n);
    t2 = now(o);
    say(o, line, vjo_psp_line_read_done(line, sizeof(line), addr, n, elapsed(t1, t2), sum));
}

/* Step 3b: the profile's line, its site's words and buffer read behind the same checks as every other read. */
static void profile_step(const VjoPspOps *o, const VjoPspBlock *blocks, const char *id)
{
    const VjoPspProfile *pp = vjo_psp_text_profile(id);
    char line[VJO_PSP_LINE_MAX];
    uint8_t buf[VJO_PSP_TEXT_MAX];
    uint32_t words[VJO_PSP_CODE_WORDS] = {0}, at;
    unsigned mask = 0;
    int read = 0;
    if (!pp) return; /* a game without a profile: no line */
    for (unsigned k = 0; k < VJO_PSP_CODE_WORDS && o->word; k++) {
        uint32_t a = vjo_psp_view(pp->site) + 4u * k;
        if (!vjo_psp_inside(blocks, VJO_PSP_BLOCKS, a, 4)) continue;
        words[k] = o->word(o->ud, a);
        mask |= 1u << k;
    }
    at = vjo_psp_view(pp->buffer);
    if (o->bytes && vjo_psp_inside(blocks, VJO_PSP_BLOCKS, at, sizeof buf)) {
        o->bytes(o->ud, at, buf, sizeof buf);
        read = 1;
    }
    say(o, line, vjo_psp_line_profile(line, sizeof(line), id, pp, words, mask, read ? buf : NULL, sizeof buf));
}

/* One pass, steps 1-4, each line put before the next read. seen: this job's lookups, and its title bytes when
 * seen->title_read (a pass after the first; the first reads them after the block lines are out). */
static void pass(VjoPspProbe *p, const VjoPspOps *o, const VjoPspBlock *blocks, VjoPspSeen *seen, uint32_t seq)
{
    char line[VJO_PSP_LINE_MAX], id[10];
    VjoPspModule m = {-1, 0, 0, 0, 0, 0};
    uint32_t sites[VJO_PSP_SITES_MAX];
    unsigned total = 0, n, count[VJO_PSP_CLASSES] = {0};
    p->passes++;
    say(o, line, vjo_psp_line_start(line, sizeof(line), p->passes, seq));
    /* 1. which ScePspemu build this is, and the view's three blocks */
    if (o->module) o->module(o->ud, &m);
    say(o, line, vjo_psp_line_module(line, sizeof(line), &m));
    for (unsigned i = 0; i < VJO_PSP_BLOCKS; i++) say(o, line, vjo_psp_line_block(line, sizeof(line), &blocks[i]));
    if (seen->ok != ALL_BLOCKS) {
        p->seen = *seen;
        say(o, line, vjo_psp_line_end(line, sizeof(line), "a block is missing or not readable"));
        return;
    }
    /* 2. the running game's ID, as Adrenaline's PSP side wrote it */
    if (!title_readable(blocks, seen) || !o->bytes) {
        p->seen = *seen;
        say(o, line, vjo_psp_line_titleid(line, sizeof(line), TITLE_ADDR, 0, NULL));
        say(o, line, vjo_psp_line_end(line, sizeof(line), "the title ID is outside the blocks"));
        return;
    }
    if (!seen->title_read) {
        o->bytes(o->ud, TITLE_ADDR, seen->title, VJO_PSP_TITLEID_BYTES);
        seen->title_read = 1;
    }
    p->seen = *seen;
    say(o, line, vjo_psp_line_titleid(line, sizeof(line), TITLE_ADDR, 1, seen->title));
    /* 3. Luna's sites of that ID: four words around each and their class (an ID that is not one is "not listed") */
    vjo_psp_title_id(seen->title, id);
    n = vjo_psp_luna_sites(id, sites, VJO_PSP_SITES_MAX, &total);
    say(o, line, vjo_psp_line_sites(line, sizeof(line), id, total, n));
    for (unsigned s = 0; s < n; s++) {
        uint32_t target = vjo_psp_vita_addr(sites[s]), words[VJO_PSP_SITE_WORDS] = {0};
        unsigned mask = 0;
        for (unsigned k = 0; k < VJO_PSP_SITE_WORDS && o->word; k++) {
            uint32_t a = target - 8u + 4u * k;
            if (!vjo_psp_inside(blocks, VJO_PSP_BLOCKS, a, 4)) continue;
            words[k] = o->word(o->ud, a); /* aligned: Luna's addresses are */
            mask |= 1u << k;
        }
        count[vjo_psp_classify(words, mask)]++;
        say(o, line, vjo_psp_line_site(line, sizeof(line), sites[s], target, words, mask));
    }
    if (n) say(o, line, vjo_psp_line_classes(line, sizeof(line), count));
    /* 3b. Rubi's PSP text profile of that ID (core/psp_text.h): the four words at its site, and its page buffer's
     * length and hash (never its text), each read only inside a mapping the lookups found */
    profile_step(o, blocks, id);
    /* 4. one timed read of 1 MiB, once per process */
    if (!p->timed) {
        p->timed = 1;
        timed_read(o, blocks);
    }
    say(o, line, vjo_psp_line_end(line, sizeof(line), NULL));
}

void vjo_psp_probe_job(VjoPspProbe *p, const VjoPspOps *o, const VjoGameOcrResult *r, uint32_t flags,
                       uint32_t speed, uint32_t job_us, uint32_t psp_text)
{
    char line[VJO_PSP_LINE_MAX];
    uint32_t seq = r ? r->seq : 0u;
    if (!p || !o) return;
    p->jobs++;
    if (p->passes < VJO_PSP_PASSES_MAX) {
        VjoPspBlock blocks[VJO_PSP_BLOCKS];
        VjoPspSeen seen;
        lookup(o, blocks, &seen);
        if (!p->passes) {
            pass(p, o, blocks, &seen, seq);
        } else {
            if (title_readable(blocks, &seen) && o->bytes) {
                o->bytes(o->ud, TITLE_ADDR, seen.title, VJO_PSP_TITLEID_BYTES);
                seen.title_read = 1;
            }
            if (!same(&seen, &p->seen)) pass(p, o, blocks, &seen, seq);
        }
    }
    if (!r) return;
    if (p->job_lines < VJO_PSP_JOB_LINES_MAX) {
        p->job_lines++;
        say(o, line, vjo_psp_line_job_capture(line, sizeof(line), r, flags, speed));
        say(o, line, vjo_psp_line_job_ocr(line, sizeof(line), r, job_us, psp_text));
    } else if (p->job_lines == VJO_PSP_JOB_LINES_MAX) {
        p->job_lines++;
        say(o, line, vjo_psp_line_jobs_cap(line, sizeof(line), VJO_PSP_JOB_LINES_MAX));
    }
}
