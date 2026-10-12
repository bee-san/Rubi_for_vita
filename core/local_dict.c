#include "local_dict.h"

#include <string.h>
#include "port.h"
#include "utf.h"

#define HEADER_SIZE 64u
#define INDEX_SIZE 24u
#define PAGE_SIZE 4096u
#define MAX_CANDIDATES 128
#define MAX_SCAN_CHARS 32
#define MAX_HOMONYMS 64
#define RULE_MASK 127u
#define CACHE_PAGES 4
#define MISS_SLOTS 64
#define HASH_PAGE_RECORDS 170u
#define MAX_HASH_CHAIN 64u

typedef struct {
    const char *in, *out;
    uint8_t in_len, out_len;
    uint16_t rules_in, rules_out;
} DeinflectRule;
#include "deinflect.inc"

typedef struct {
    char key[VJO_LOCAL_MAX_KEY + 1];
    uint16_t rules, len;
} Candidate;

typedef struct {
    VjoFile file;
    uint32_t count, version, buckets;
    uint64_t data;
} Dictionary;

typedef struct {
    Dictionary *dict;
    uint64_t off;
    size_t len;
    uint32_t used;
    uint8_t bytes[PAGE_SIZE];
} Page;

typedef struct {
    uint64_t key_off, entry_off;
    uint32_t entry_len;
    uint16_t key_len, rules;
} Record;

typedef struct {
    int dict;
    uint16_t units; /* selected lookups: matched source span (in former padding) */
    Record record;
} Match;

typedef struct {
    Dictionary dicts[VJO_LOCAL_MAX_DICTS];
    Page pages[CACHE_PAGES];
    struct { uint64_t hash; unsigned dict; } misses[MISS_SLOTS];
    uint32_t clock;
    Candidate candidates[MAX_CANDIDATES];
    Match matches[VJO_LOCAL_MAX_MATCHES];
    uint64_t seen_off[VJO_LOCAL_MAX_RESULTS];
    int seen_dict[VJO_LOCAL_MAX_RESULTS];
    int n_dicts, n_matches;
} Workspace;

typedef char WorkspaceFits[(sizeof(Workspace) <= VJO_LOCAL_SCRATCH_BUDGET) ? 1 : -1];
typedef char MatchKeepsItsSize[(sizeof(Match) == 8 + sizeof(Record)) ? 1 : -1];

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)u16(p) | (uint32_t)u16(p + 2) << 16; }
static uint64_t u64(const uint8_t *p) { return (uint64_t)u32(p) | (uint64_t)u32(p + 4) << 32; }

/* One filename at a time, bounded even for malformed config. */
static int next_name(const char **cursor, char name[64])
{
    const char *s = *cursor;
    size_t n = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return 0;
    while (*s && *s != ',') {
        if (n >= 63) return -1;
        name[n++] = *s++;
    }
    while (n && (name[n - 1] == ' ' || name[n - 1] == '\t')) n--;
    name[n] = 0;
    if (n < 8 || name[0] == '.' || strstr(name, "..") || strcmp(name + n - 7, ".vjdict")) return -1;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return -1;
    }
    if (*s == ',') {
        s++;
        const char *tail = s;
        while (*tail == ' ' || *tail == '\t') tail++;
        if (!*tail) return -1;
    }
    *cursor = s;
    return 1;
}

int vjo_local_names_count(const char *names)
{
    char name[64];
    int n = 0, rc;
    if (!names) return 0;
    while ((rc = next_name(&names, name)) > 0)
        if (++n > VJO_LOCAL_MAX_DICTS) return 0;
    return rc == 0 ? n : 0;
}

int vjo_local_names_valid(const char *names)
{
    return vjo_local_names_count(names) > 0;
}

/* The index-th configured name; 0 when absent or the list is invalid. */
static int nth_name(const char *names, int index, char name[64])
{
    if (index < 0 || index >= vjo_local_names_count(names)) return 0;
    for (int i = 0; i <= index; i++)
        if (next_name(&names, name) <= 0) return 0;
    return 1;
}

static int range_ok(Dictionary *d, uint64_t off, size_t n)
{
    return off <= d->file.size && n <= d->file.size - off;
}

static int cached_read(Workspace *w, Dictionary *d, uint64_t off, void *dst, size_t n)
{
    uint8_t *out = dst;
    if (!range_ok(d, off, n)) return VJO_E_LOCAL_FORMAT;
    while (n) {
        uint64_t base = off & ~(uint64_t)(PAGE_SIZE - 1);
        size_t pos = (size_t)(off - base), take;
        Page *page = NULL, *victim = &w->pages[0];
        for (int i = 0; i < CACHE_PAGES; i++) {
            Page *p = &w->pages[i];
            if (p->dict == d && p->off == base) { page = p; break; }
            if (!p->dict || (victim->dict && p->used < victim->used)) victim = p;
        }
        if (!page) {
            page = victim;
            uint64_t remaining = d->file.size - base;
            page->dict = NULL;
            page->off = base;
            page->len = remaining < PAGE_SIZE ? (size_t)remaining : PAGE_SIZE;
            if (d->file.read(d->file.ctx, base, page->bytes, page->len)) return VJO_E_LOCAL_IO;
            page->dict = d;
        }
        page->used = ++w->clock;
        take = page->len - pos;
        if (take > n) take = n;
        memcpy(out, page->bytes + pos, take);
        off += take; out += take; n -= take;
    }
    return VJO_OK;
}

static int open_file(const VjoPlatform *p, const VjoConfig *cfg, const char *name, Dictionary *d, VjoErr *err)
{
    char path[256];
    int n = vjo_snprintf(path, sizeof(path), "%s/%s", cfg->local_dictionary_dir, name);
    if (n < 0 || (size_t)n >= sizeof(path) || p->file_open(p->ud, path, &d->file)) {
        err->detail = "Cannot open a configured dictionary; check local_dictionary_dir and local_dictionaries";
        return VJO_E_LOCAL_IO;
    }
    return VJO_OK;
}

/* Reads and validates only the 64-byte header of an opened file. */
static int check_header(Dictionary *d)
{
    uint8_t h[HEADER_SIZE];
    if (d->file.size < HEADER_SIZE) return VJO_E_LOCAL_FORMAT;
    if (!d->file.read || !d->file.close || d->file.read(d->file.ctx, 0, h, sizeof(h))) return VJO_E_LOCAL_IO;
    d->version = u32(h + 8);
    d->count = u32(h + 16);
    d->data = u64(h + 32);
    if (u32(h + 12) != HEADER_SIZE || u32(h + 20) != INDEX_SIZE ||
        d->data > d->file.size || u64(h + 40) != d->file.size || !d->count ||
        d->file.size > INT64_MAX) return VJO_E_LOCAL_FORMAT;
    if (d->version == 1) {
        if (memcmp(h, "VJDICT1\0", 8) || u64(h + 24) != HEADER_SIZE ||
            d->data != HEADER_SIZE + (uint64_t)d->count * INDEX_SIZE ||
            u64(h + 48) || u64(h + 56)) return VJO_E_LOCAL_FORMAT;
    } else if (d->version == 2) {
        uint64_t buckets = u64(h + 48), pages = u64(h + 56);
        if (memcmp(h, "VJDICT2\0", 8) || u64(h + 24) != PAGE_SIZE ||
            !buckets || buckets > UINT32_MAX || (buckets & (buckets - 1)) ||
            pages < buckets || pages > UINT32_MAX || d->data != PAGE_SIZE + pages * PAGE_SIZE)
            return VJO_E_LOCAL_FORMAT;
        d->buckets = (uint32_t)buckets;
    } else return VJO_E_LOCAL_FORMAT;
    return VJO_OK;
}

static int open_dictionaries(Workspace *w, const VjoPlatform *p, const VjoConfig *cfg, VjoErr *err)
{
    const char *names = cfg->local_dictionaries;
    char name[64];
    if (!p->file_open || !vjo_local_names_valid(names) || !cfg->local_dictionary_dir[0]) {
        err->detail = "Set local_dictionaries to up to 8 .vjdict filenames in config.ini";
        return VJO_E_LOCAL_IO;
    }
    while (next_name(&names, name) > 0) {
        Dictionary *d = &w->dicts[w->n_dicts];
        int rc = open_file(p, cfg, name, d, err);
        if (rc) return rc;
        w->n_dicts++; /* close even if header validation fails */
        if ((rc = check_header(d))) return rc;
    }
    return VJO_OK;
}

static int record_at(Workspace *w, Dictionary *d, uint32_t i, Record *r, char *key)
{
    uint8_t bytes[INDEX_SIZE];
    int rc = cached_read(w, d, HEADER_SIZE + (uint64_t)i * INDEX_SIZE, bytes, sizeof(bytes));
    if (rc) return rc;
    r->key_off = u64(bytes); r->entry_off = u64(bytes + 8);
    r->key_len = u16(bytes + 16); r->rules = u16(bytes + 18); r->entry_len = u32(bytes + 20);
    if (!r->key_len || r->key_len > VJO_LOCAL_MAX_KEY || (r->rules & ~RULE_MASK) ||
        r->key_off < d->data || r->entry_off < d->data || r->entry_len < 4 ||
        r->entry_len > VJO_LOCAL_MAX_ENTRY || !range_ok(d, r->entry_off, r->entry_len)) return VJO_E_LOCAL_FORMAT;
    rc = cached_read(w, d, r->key_off, key, r->key_len);
    if (rc) return rc;
    if (memchr(key, 0, r->key_len)) return VJO_E_LOCAL_FORMAT;
    key[r->key_len] = 0;
    return VJO_OK;
}

static int find_matches_v1(Workspace *w, int di, const Candidate *c, int limit)
{
    Dictionary *d = &w->dicts[di];
    uint32_t lo = 0, hi = d->count;
    Record r;
    char key[VJO_LOCAL_MAX_KEY + 1];
    int rc;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if ((rc = record_at(w, d, mid, &r, key))) return rc;
        if (strcmp(key, c->key) < 0) lo = mid + 1; else hi = mid;
    }
    /* Bound duplicate-heavy dictionaries as well as result storage. */
    for (unsigned scanned = 0; lo < d->count && scanned < MAX_HOMONYMS; lo++, scanned++) {
        int duplicate = 0;
        if ((rc = record_at(w, d, lo, &r, key))) return rc;
        if (strcmp(key, c->key)) break;
        if (c->rules && !(c->rules & r.rules)) continue;
        for (int j = 0; j < w->n_matches; j++)
            if (w->matches[j].dict == di && w->matches[j].record.entry_off == r.entry_off) duplicate = 1;
        if (duplicate) continue;
        if (w->n_matches >= limit) break;
        w->matches[w->n_matches].dict = di;
        w->matches[w->n_matches++].record = r;
        if (w->n_matches >= limit) break; /* never read a row that cannot be kept */
    }
    return VJO_OK;
}

/* Stable FNV-1a-64 hash with an avalanche finalizer; mirrored by the converter.
 * Hashes route to a bucket only: full keys are always compared before a hit. */
static uint64_t key_hash(const char *key)
{
    uint64_t h = UINT64_C(14695981039346656037);
    for (; *key; key++) h = (h ^ (uint8_t)*key) * UINT64_C(1099511628211);
    h ^= h >> 33; h *= UINT64_C(0xff51afd7ed558ccd);
    h ^= h >> 33; h *= UINT64_C(0xc4ceb9fe1a85ec53);
    return h ^ (h >> 33);
}

static int find_matches_v2(Workspace *w, int di, const Candidate *c, int limit)
{
    Dictionary *d = &w->dicts[di];
    uint64_t hash = key_hash(c->key), page = PAGE_SIZE + (hash & (d->buckets - 1)) * PAGE_SIZE;
    unsigned miss = (unsigned)(hash >> 32) & (MISS_SLOTS - 1);
    int rc, hash_present = 0;
    if (w->misses[miss].dict == (unsigned)di + 1 && w->misses[miss].hash == hash) return VJO_OK;
    for (unsigned chain = 0; ; chain++) {
        uint8_t header[16], slot[24];
        uint32_t count, lo = 0, hi;
        uint64_t next;
        if (chain >= MAX_HASH_CHAIN || page < PAGE_SIZE || page >= d->data || page % PAGE_SIZE)
            return VJO_E_LOCAL_FORMAT;
        if ((rc = cached_read(w, d, page, header, sizeof(header)))) return rc;
        count = u32(header + 4); next = u64(header + 8);
        if (memcmp(header, "VJHP", 4) || count > HASH_PAGE_RECORDS ||
            (next && (next <= page || next < PAGE_SIZE + (uint64_t)d->buckets * PAGE_SIZE ||
                      next >= d->data || next % PAGE_SIZE))) return VJO_E_LOCAL_FORMAT;
        hi = count;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            if ((rc = cached_read(w, d, page + 16 + mid * 24, slot, sizeof(slot)))) return rc;
            if (u64(slot) < hash) lo = mid + 1; else hi = mid;
        }
        for (; lo < count; lo++) {
            uint64_t group;
            uint32_t n;
            uint16_t key_len, rules;
            char key[VJO_LOCAL_MAX_KEY + 1];
            if ((rc = cached_read(w, d, page + 16 + lo * 24, slot, sizeof(slot)))) return rc;
            if (u64(slot) != hash) break;
            hash_present = 1;
            group = u64(slot + 8); n = u32(slot + 16);
            key_len = u16(slot + 20); rules = u16(slot + 22);
            if (!key_len || key_len > VJO_LOCAL_MAX_KEY || !n || n > d->count ||
                (rules & ~RULE_MASK) || group < d->data || !range_ok(d, group, key_len) ||
                (uint64_t)n * 16 > d->file.size - group - key_len) return VJO_E_LOCAL_FORMAT;
            if (key_len != c->len || (c->rules && !(c->rules & rules))) continue;
            if ((rc = cached_read(w, d, group, key, key_len))) return rc;
            if (memcmp(key, c->key, key_len)) continue;
            if (n > MAX_HOMONYMS) n = MAX_HOMONYMS;
            for (uint32_t j = 0; j < n && w->n_matches < limit; j++) {
                uint8_t post[16];
                Record r;
                int duplicate = 0;
                if ((rc = cached_read(w, d, group + key_len + j * 16, post, sizeof(post)))) return rc;
                r.entry_off = u64(post); r.entry_len = u32(post + 8); r.rules = u16(post + 12);
                r.key_off = group; r.key_len = key_len;
                if (u16(post + 14) || (r.rules & ~RULE_MASK) || r.entry_off < d->data ||
                    r.entry_len < 4 || r.entry_len > VJO_LOCAL_MAX_ENTRY ||
                    !range_ok(d, r.entry_off, r.entry_len)) return VJO_E_LOCAL_FORMAT;
                if (c->rules && !(c->rules & r.rules)) continue;
                for (int k = 0; k < w->n_matches; k++)
                    if (w->matches[k].dict == di && w->matches[k].record.entry_off == r.entry_off) duplicate = 1;
                if (duplicate) continue;
                w->matches[w->n_matches].dict = di;
                w->matches[w->n_matches++].record = r;
            }
            return VJO_OK;
        }
        if (!next) break;
        page = next;
    }
    /* Cache only proven absent hashes, never a filtered/duplicate result. A
     * collision cannot create a false negative: any equal hash prevents caching. */
    if (!hash_present) { w->misses[miss].hash = hash; w->misses[miss].dict = (unsigned)di + 1; }
    return VJO_OK;
}

static int find_matches(Workspace *w, int di, const Candidate *c, int limit)
{
    return w->dicts[di].version == 2 ? find_matches_v2(w, di, c, limit) : find_matches_v1(w, di, c, limit);
}

static int deinflect(Candidate *c, const char *key)
{
    int count = 1;
    size_t len = strlen(key);
    memcpy(c[0].key, key, len + 1); c[0].len = (uint16_t)len; c[0].rules = 0;
    for (int i = 0; i < count; i++) {
        unsigned suffix = (uint8_t)c[i].key[c[i].len - 1];
        for (unsigned r = deinflect_offsets[suffix]; r < deinflect_offsets[suffix + 1]; r++) {
            const DeinflectRule *rule = &deinflect_rules[r];
            size_t stem, n;
            char next[VJO_LOCAL_MAX_KEY + 1];
            int duplicate = 0;
            if ((c[i].rules && !(c[i].rules & rule->rules_in)) || c[i].len < rule->in_len) continue;
            stem = c[i].len - rule->in_len;
            if (memcmp(c[i].key + stem, rule->in, rule->in_len)) continue;
            n = stem + rule->out_len;
            if (!n || n > VJO_LOCAL_MAX_KEY) continue;
            memcpy(next, c[i].key, stem); memcpy(next + stem, rule->out, rule->out_len); next[n] = 0;
            for (int j = 0; j < count; j++)
                if (c[j].rules == rule->rules_out && !strcmp(next, c[j].key)) duplicate = 1;
            if (duplicate) continue;
            if (count >= MAX_CANDIDATES) return count;
            memcpy(c[count].key, next, n + 1);
            c[count].rules = rule->rules_out; c[count++].len = (uint16_t)n;
        }
    }
    return count;
}

static int boundary(uint32_t cp)
{
    return cp <= 0x20 || cp == 0x3000 || cp == 0x3001 || cp == 0x3002 ||
           (cp >= 0x3008 && cp <= 0x3011) || cp == 0xff01 || cp == 0xff1f ||
           cp == '!' || cp == '?' || cp == ',' || cp == '.';
}

/* Reads one payload, three NUL-terminated strings (spelling, reading and
 * plain-text definition), into out. The definition is displayed verbatim. */
static int read_entry(VjoArena *out, const Dictionary *d, uint64_t off, uint32_t len, VjoVocab *v)
{
    char *payload = vjo_arena_alloc(out, len), *reading, *meaning;
    size_t left;
    if (!payload) return VJO_E_TOO_LARGE;
    if (d->file.read(d->file.ctx, off, payload, len)) return VJO_E_LOCAL_IO;
    if (!payload[0] || payload[len - 1]) return VJO_E_LOCAL_FORMAT;
    reading = memchr(payload, 0, len);
    if (!reading) return VJO_E_LOCAL_FORMAT;
    reading++;
    left = len - (size_t)(reading - payload);
    meaning = memchr(reading, 0, left);
    if (!meaning || meaning + 1 >= payload + len) return VJO_E_LOCAL_FORMAT;
    meaning++;
    if (memchr(meaning, 0, len - (size_t)(meaning - payload) - 1)) return VJO_E_LOCAL_FORMAT;
    v->spelling = payload; v->reading = *reading ? reading : payload; v->rank = VJO_NO_RANK;
    v->meanings = vjo_arena_alloc(out, sizeof(char *));
    if (!v->meanings) return VJO_E_TOO_LARGE;
    v->meanings[0] = meaning; v->n_meanings = 1;
    return VJO_OK;
}

static int append_match(VjoArena *a, Workspace *w, const Match *m, int pos16, int len16, VjoDictResult *res)
{
    int n = res->n_vocab, rc;
    /* Repeated words reuse vocabulary, but keep every token. */
    for (int i = 0; i < n; i++) {
        if (w->seen_dict[i] == m->dict && w->seen_off[i] == m->record.entry_off) {
            if (res->n_tokens >= VJO_LOCAL_MAX_RESULTS) return VJO_E_TOO_LARGE;
            res->tokens[res->n_tokens++] = (VjoToken){.vocab = i, .pos16 = pos16, .len16 = len16};
            return VJO_OK;
        }
    }
    if (n >= VJO_LOCAL_MAX_RESULTS || res->n_tokens >= VJO_LOCAL_MAX_RESULTS) return VJO_E_TOO_LARGE;
    if ((rc = read_entry(a, &w->dicts[m->dict], m->record.entry_off, m->record.entry_len, &res->vocab[n])))
        return rc;
    w->seen_dict[n] = m->dict; w->seen_off[n] = m->record.entry_off;
    res->tokens[res->n_tokens++] = (VjoToken){.vocab = n, .pos16 = pos16, .len16 = len16};
    res->n_vocab++;
    return VJO_OK;
}

int vjo_local_lookup(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                     const char *text, VjoDictResult *res, VjoErr *err)
{
    size_t mark = vjo_arena_mark(a), len = strlen(text), off = 0;
    VjoArena output;
    Workspace *w;
    void *outmem;
    int rc = VJO_OK, pos16 = 0;
    memset(res, 0, sizeof(*res));
    if (len > VJO_LOCAL_MAX_TEXT) return err->rc = VJO_E_TOO_LARGE;
    outmem = vjo_arena_alloc(a, VJO_LOCAL_RESULT_BUDGET);
    w = vjo_arena_zalloc(a, sizeof(*w));
    if (!outmem || !w) { vjo_arena_release(a, mark); return err->rc = VJO_E_OOM; }
    vjo_arena_init(&output, outmem, VJO_LOCAL_RESULT_BUDGET);
    res->vocab = vjo_arena_zalloc(&output, VJO_LOCAL_MAX_RESULTS * sizeof(VjoVocab));
    res->tokens = vjo_arena_zalloc(&output, VJO_LOCAL_MAX_RESULTS * sizeof(VjoToken));
    if ((rc = open_dictionaries(w, p, cfg, err))) goto done;
    while (off < len) {
        char key[VJO_LOCAL_MAX_KEY + 1];
        size_t ends[MAX_SCAN_CHARS + 1], lengths[MAX_SCAN_CHARS + 1], scan = off, bytes = 0;
        int units[MAX_SCAN_CHARS + 1], count = 0, u = 0, matched = 0;
        while (scan < len && count < MAX_SCAN_CHARS) {
            uint32_t cp = vjo_utf8_next(text, len, &scan);
            char enc[4];
            int k;
            if (boundary(cp)) break;
            u += vjo_utf16_units(cp);
            /* Same normalization as the converter; source lengths stay intact. */
            if (cp >= 0x30a1 && cp <= 0x30f6) cp -= 0x60;
            if (cp >= 0xff01 && cp <= 0xff5e) cp -= 0xfee0;
            k = vjo_utf8_encode(cp, enc);
            if (bytes + (size_t)k > VJO_LOCAL_MAX_KEY) break;
            memcpy(key + bytes, enc, (size_t)k); bytes += (size_t)k;
            count++; ends[count] = scan; lengths[count] = bytes; units[count] = u;
        }
        for (int k = count; k > 0 && !matched; k--) {
            int candidates;
            key[lengths[k]] = 0;
            candidates = deinflect(w->candidates, key);
            w->n_matches = 0;
            /* Exact matches in all dictionaries precede deinflected matches. */
            for (int c = 0; c < candidates && w->n_matches < VJO_LOCAL_MAX_MATCHES; c++)
                for (int d = 0; d < w->n_dicts && w->n_matches < VJO_LOCAL_MAX_MATCHES; d++)
                    if ((rc = find_matches(w, d, &w->candidates[c], VJO_LOCAL_MAX_MATCHES))) goto done;
            if (!w->n_matches) continue;
            for (int j = 0; j < w->n_matches; j++)
                if ((rc = append_match(&output, w, &w->matches[j], pos16, units[k], res))) goto done;
            off = ends[k]; pos16 += units[k]; matched = 1;
        }
        if (!matched) pos16 += vjo_utf16_units(vjo_utf8_next(text, len, &off));
    }
done:
    for (int i = 0; i < w->n_dicts; i++)
        if (w->dicts[i].file.close) w->dicts[i].file.close(w->dicts[i].file.ctx);
    if (rc) {
        memset(res, 0, sizeof(*res));
        vjo_arena_release(a, mark);
    } else {
        /* The scratch workspace and unused result capacity do not stay resident. */
        vjo_arena_release(a, (size_t)((uint8_t *)outmem - a->base) + output.used);
    }
    return err->rc = rc;
}

/* Pages are final when the overall best match is known and every dictionary
 * that may take one of the remaining limit - 1 places (configured order) is
 * resolved. limit 1 stops at the first match, exactly as a single lookup. */
static int pages_complete(const Workspace *w, unsigned resolved, int limit)
{
    int best, shown = 1;
    if (!w->n_matches) return 0;
    best = w->matches[0].dict;
    for (int d = 0; d < w->n_dicts && shown < limit; d++) {
        if (d == best) continue;
        if (!(resolved & (1u << d))) return 0; /* may still match a shorter span */
        shown++;
    }
    return 1;
}

static void add_page(VjoLocalPages *pages, const Workspace *w, int match)
{
    const Match *m = &w->matches[match];
    VjoLocalPage *pg;
    if (!pages) return; /* one result: nothing to locate */
    pg = &pages->page[pages->count++];
    pg->entry_off = m->record.entry_off;
    pg->entry_len = m->record.entry_len;
    pg->file_size = w->dicts[m->dict].file.size;
    pg->len16 = m->units;
    pg->dict = (uint8_t)m->dict;
    pg->reserved = 0;
}

int vjo_local_lookup_pages(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                           const char *text, size_t cursor_byte, int limit,
                           VjoLocalPages *pages, VjoDictResult *res, VjoErr *err)
{
    size_t mark = vjo_arena_mark(a), len = strlen(text), off = 0, bytes = 0;
    size_t lengths[MAX_SCAN_CHARS + 1];
    int units[MAX_SCAN_CHARS + 1], pos16 = 0, count = 0, source_units = 0, complete = 0;
    unsigned resolved = 0;
    char key[VJO_LOCAL_MAX_KEY + 1];
    VjoArena output;
    Workspace *w;
    void *outmem;
    int rc = VJO_OK;

    memset(res, 0, sizeof(*res));
    memset(err, 0, sizeof(*err));
    err->dict = VJO_DICT_LOCAL;
    if (limit < 1 || !pages) limit = 1;
    if (limit > VJO_LOCAL_MAX_DICTS) limit = VJO_LOCAL_MAX_DICTS;
    if (pages) memset(pages, 0, sizeof(*pages));
    if (len > VJO_LOCAL_MAX_TEXT) return err->rc = VJO_E_TOO_LARGE;
    if (cursor_byte > len) return err->rc = VJO_E_PARSE;
    while (off < cursor_byte)
        pos16 += vjo_utf16_units(vjo_utf8_next(text, len, &off));
    if (off != cursor_byte) return err->rc = VJO_E_PARSE;
    if (pages) {
        pages->cursor_byte = (uint32_t)cursor_byte;
        pages->pos16 = pos16;
    }

    /* Only inspect the prefix at the selected character. Source UTF-16
     * lengths remain unchanged by kana/fullwidth search normalization. */
    while (off < len && count < MAX_SCAN_CHARS) {
        uint32_t cp = vjo_utf8_next(text, len, &off);
        char enc[4];
        int n;
        if (boundary(cp)) break;
        source_units += vjo_utf16_units(cp);
        if (cp >= 0x30a1 && cp <= 0x30f6) cp -= 0x60;
        if (cp >= 0xff01 && cp <= 0xff5e) cp -= 0xfee0;
        n = vjo_utf8_encode(cp, enc);
        if (bytes + (size_t)n > VJO_LOCAL_MAX_KEY) break;
        memcpy(key + bytes, enc, (size_t)n); bytes += (size_t)n;
        count++; lengths[count] = bytes; units[count] = source_units;
    }
    if (!count) return VJO_OK; /* End/punctuation: no files or allocations. */

    outmem = vjo_arena_alloc(a, VJO_LOCAL_SELECTED_BUDGET);
    w = vjo_arena_zalloc(a, sizeof(*w));
    if (!outmem || !w) {
        vjo_arena_release(a, mark);
        return err->rc = VJO_E_OOM;
    }
    vjo_arena_init(&output, outmem, VJO_LOCAL_SELECTED_BUDGET);
    res->vocab = vjo_arena_zalloc(&output, sizeof(VjoVocab));
    res->tokens = vjo_arena_zalloc(&output, sizeof(VjoToken));
    if ((rc = open_dictionaries(w, p, cfg, err))) goto done;
    /* An exact hit in a later dictionary beats an inflected hit in an earlier
     * dictionary. Within the same candidate, first dictionary and first
     * posting win. Each dictionary keeps only its first (best) match; the
     * first one found overall is the single result. Only page 0's payload is
     * read: other pages are located, never loaded. */
    for (int k = count; k > 0 && !complete; k--) {
        int candidates;
        key[lengths[k]] = 0;
        candidates = deinflect(w->candidates, key);
        for (int c = 0; c < candidates && !complete; c++)
            for (int d = 0; d < w->n_dicts && !complete; d++) {
                int before = w->n_matches;
                if (resolved & (1u << d)) continue;
                if ((rc = find_matches(w, d, &w->candidates[c], before + 1))) goto done;
                if (w->n_matches == before) continue;
                resolved |= 1u << d;
                w->matches[before].units = (uint16_t)units[k];
                complete = pages_complete(w, resolved, limit);
            }
    }
    if (w->n_matches) {
        int shown = 1;
        add_page(pages, w, 0);
        for (int d = 0; d < w->n_dicts && shown < limit; d++) {
            if (d == w->matches[0].dict) continue;
            for (int i = 1; i < w->n_matches; i++)
                if (w->matches[i].dict == d) { add_page(pages, w, i); shown++; break; }
        }
        rc = append_match(&output, w, &w->matches[0], pos16, w->matches[0].units, res);
    }
done:
    for (int i = 0; i < w->n_dicts; i++)
        if (w->dicts[i].file.close) w->dicts[i].file.close(w->dicts[i].file.ctx);
    if (rc || !res->n_vocab) {
        memset(res, 0, sizeof(*res));
        if (pages) pages->count = 0;
        vjo_arena_release(a, mark);
    } else {
        vjo_arena_release(a, (size_t)((uint8_t *)outmem - a->base) + output.used);
    }
    return err->rc = rc;
}

int vjo_local_lookup_at(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                        const char *text, size_t cursor_byte,
                        VjoDictResult *res, VjoErr *err)
{
    return vjo_local_lookup_pages(a, p, cfg, text, cursor_byte, 1, NULL, res, err);
}

int vjo_local_read_page(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                        const VjoLocalPages *pages, int page,
                        VjoDictResult *res, VjoErr *err)
{
    size_t mark = vjo_arena_mark(a);
    const VjoLocalPage *pg;
    Dictionary d;
    VjoArena output;
    char name[64];
    void *outmem;
    int rc, opened = 0;

    memset(res, 0, sizeof(*res));
    memset(err, 0, sizeof(*err));
    err->dict = VJO_DICT_LOCAL;
    if (!pages || page < 0 || page >= pages->count || pages->count > VJO_LOCAL_MAX_DICTS)
        return err->rc = VJO_E_PARSE;
    pg = &pages->page[page];
    if (!p->file_open || !cfg->local_dictionary_dir[0] || !nth_name(cfg->local_dictionaries, pg->dict, name)) {
        err->detail = "Set local_dictionaries to up to 8 .vjdict filenames in config.ini";
        return err->rc = VJO_E_LOCAL_IO;
    }
    outmem = vjo_arena_alloc(a, VJO_LOCAL_SELECTED_BUDGET);
    if (!outmem) return err->rc = VJO_E_OOM;
    vjo_arena_init(&output, outmem, VJO_LOCAL_SELECTED_BUDGET);
    res->vocab = vjo_arena_zalloc(&output, sizeof(VjoVocab));
    res->tokens = vjo_arena_zalloc(&output, sizeof(VjoToken));
    memset(&d, 0, sizeof(d));
    rc = open_file(p, cfg, name, &d, err);
    opened = rc == VJO_OK;
    if (!rc) rc = check_header(&d);
    if (!rc && d.file.size != pg->file_size) {
        err->detail = "A dictionary file changed. Close and reopen the overlay.";
        rc = VJO_E_LOCAL_IO;
    }
    if (!rc && (pg->entry_off < d.data || pg->entry_len < 4 || pg->entry_len > VJO_LOCAL_MAX_ENTRY ||
                !range_ok(&d, pg->entry_off, pg->entry_len)))
        rc = VJO_E_LOCAL_FORMAT;
    if (!rc) rc = read_entry(&output, &d, pg->entry_off, pg->entry_len, &res->vocab[0]);
    if (opened && d.file.close) d.file.close(d.file.ctx);
    if (rc) {
        memset(res, 0, sizeof(*res));
        vjo_arena_release(a, mark);
        return err->rc = rc;
    }
    res->tokens[0] = (VjoToken){.vocab = 0, .pos16 = pages->pos16, .len16 = pg->len16};
    res->n_vocab = res->n_tokens = 1;
    vjo_arena_release(a, (size_t)((uint8_t *)outmem - a->base) + output.used);
    return VJO_OK;
}
