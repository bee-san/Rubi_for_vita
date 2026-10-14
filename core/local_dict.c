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
    uint8_t depth; /* deinflection steps from the text (in the former padding) */
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


/* One key's postings are in source order. In the bundled Daijirin4 that is
 * reading order, not use: 私 lists あたい first, する 剃る, から 空. Only one
 * entry is shown, so the postings are ranked (docs/local-dictionaries.md):
 * a common reading of a frequent word (the table below), then an entry
 * spelled exactly as the text is written (kana text: the kana word, such as
 * the particle から or the verb する; inflected text: the dictionary form),
 * then for katakana text one written so in hiragana (って for ッて), then one with a reading (Daijirin4's kanji-character articles have none),
 * then the longer article. Equal ranks keep source order. */
#define RANK_PREFERRED (1u << 23)
#define RANK_WRITTEN (1u << 22)
#define RANK_KANA (1u << 21) /* katakana text, an entry written so in hiragana */
#define RANK_READING (1u << 20)
#define HEAD_BYTES 160u /* a payload's spelling (<= 32 characters) and reading */

/* Key, then the reading (or, for kana keys, the spelling) its frequent sense
 * uses; checked to exist in the bundled dictionary. Ends with an empty key. */
static const char preferred_readings[] =
    "私\0わたし\0僕\0ぼく\0彼\0かれ\0何\0なに\0今日\0きょう\0一\0いち\0日\0ひ\0火\0ひ\0水\0みず\0"
    "上\0うえ\0下\0した\0中\0なか\0後\0あと\0間\0あいだ\0方\0ほう\0様\0さま\0次\0つぎ\0別\0べつ\0"
    "母\0はは\0兄\0あに\0弟\0おとうと\0大人\0おとな\0学生\0がくせい\0体\0からだ\0頭\0あたま\0腕\0うで\0"
    "空\0そら\0金\0かね\0朝\0あさ\0秋\0あき\0夏\0なつ\0風\0かぜ\0木\0き\0道\0みち\0字\0じ\0飯\0めし\0"
    "同じ\0おなじ\0本気\0ほんき\0大切\0たいせつ\0何処\0どこ\0入る\0はいる\0守る\0まもる\0怒る\0おこる\0"
    "こう\0斯う\0そう\0然う\0どう\0如何\0ああ\0嗚呼\0くん\0君\0よう\0様\0いい\0良い\0"
    "ここ\0此処\0そこ\0其処\0あそこ\0彼処\0どこ\0何処\0これ\0此れ\0それ\0其れ\0どれ\0何れ\0"
    "たち\0達\0ことにする\0事にする\0私達\0わたしたち\0致す\0いたす\0立場\0たちば\0形相\0ぎょうそう\0"
    "米\0こめ\0神\0かみ\0服\0ふく\0羽\0はね\0和\0わ\0性\0せい\0室\0しつ\0美女\0びじょ\0小路\0こうじ\0";

typedef struct {
    const char *written;   /* the text as written: the source span, or a dictionary form */
    size_t written_len;
    /* The normalized form, when the text is written otherwise (katakana or
     * full width): an entry spelled so in hiragana also counts as written. */
    const Candidate *key;
    const char *preferred; /* preferred_readings' value for the key, or NULL */
    /* Strict: only an entry written so (or preferred) is taken; the best
     * other one goes to *fallback, once, for when no candidate has one. */
    int strict, fell_back;
    Match *fallback;
    uint32_t score; /* the taken posting's rank */
} Query;

static const char *preferred_for(const char *key)
{
    for (const char *p = preferred_readings; *p; p += strlen(p) + 1) {
        const char *value = p + strlen(p) + 1;
        if (!strcmp(p, key)) return value;
        p = value;
    }
    return NULL;
}

static int is_duplicate(const Workspace *w, int di, const Record *r)
{
    for (int k = 0; k < w->n_matches; k++)
        if (w->matches[k].dict == di && w->matches[k].record.entry_off == r->entry_off) return 1;
    return 0;
}

/* The converter's key normalization: katakana as hiragana, full-width ASCII
 * as ASCII (source spans keep their own lengths). */
static uint32_t normalize(uint32_t cp)
{
    if (cp >= 0x30a1 && cp <= 0x30f6) cp -= 0x60;
    if (cp >= 0xff01 && cp <= 0xff5e) cp -= 0xfee0;
    return cp;
}

static int normalized_equal(const char *s, size_t n, const Candidate *key)
{
    size_t i = 0, k = 0;
    while (i < n) {
        char enc[4];
        int m = vjo_utf8_encode(normalize(vjo_utf8_next(s, n, &i)), enc);
        if (k + (size_t)m > key->len || memcmp(key->key + k, enc, (size_t)m)) return 0;
        k += (size_t)m;
    }
    return k == key->len;
}

/* Reads the payload's spelling and reading (one bounded head) to rank it. */
static int rank_posting(Workspace *w, Dictionary *d, const Record *r, const Query *q, uint32_t *score)
{
    char head[HEAD_BYTES];
    size_t n = r->entry_len < HEAD_BYTES ? r->entry_len : HEAD_BYTES, spelling;
    const char *reading;
    int rc = cached_read(w, d, r->entry_off, head, n);
    *score = r->entry_len;
    if (rc) return rc;
    if (!(reading = memchr(head, 0, n))) return VJO_OK; /* longer than any written text */
    spelling = (size_t)(reading - head);
    reading++;
    if (spelling == q->written_len && !memcmp(head, q->written, spelling)) *score |= RANK_WRITTEN | RANK_KANA;
    else if (q->key && normalized_equal(head, spelling, q->key)) *score |= RANK_KANA;
    if (reading < head + n && *reading) *score |= RANK_READING;
    if (q->preferred) {
        size_t m = strlen(q->preferred) + 1;
        if ((spelling + 1 == m && !memcmp(head, q->preferred, m)) ||
            ((size_t)(head + n - reading) >= m && !memcmp(reading, q->preferred, m)))
            *score |= RANK_PREFERRED;
    }
    return VJO_OK;
}

/* Posting j of a key: v2, the group's 16-byte postings; v1, index record
 * group + j (group is the key's first record). */
static int posting_at(Workspace *w, Dictionary *d, uint64_t group, uint16_t key_len, uint32_t j, Record *r)
{
    uint8_t post[16];
    int rc;
    if (d->version == 1) {
        char key[VJO_LOCAL_MAX_KEY + 1];
        return record_at(w, d, (uint32_t)group + j, r, key);
    }
    if ((rc = cached_read(w, d, group + key_len + j * 16, post, sizeof(post)))) return rc;
    r->entry_off = u64(post); r->entry_len = u32(post + 8); r->rules = u16(post + 12);
    r->key_off = group; r->key_len = key_len;
    if (u16(post + 14) || (r->rules & ~RULE_MASK) || r->entry_off < d->data ||
        r->entry_len < 4 || r->entry_len > VJO_LOCAL_MAX_ENTRY ||
        !range_ok(d, r->entry_off, r->entry_len)) return VJO_E_LOCAL_FORMAT;
    return VJO_OK;
}

/* The best-ranked usable posting first; then, unless strict, the others in
 * source order, up to limit. */
static int take_postings(Workspace *w, int di, const Candidate *c, Query *q, int limit,
                         uint64_t group, uint16_t key_len, uint32_t n)
{
    Dictionary *d = &w->dicts[di];
    Record r, top, other;
    uint32_t best = n, best_score = 0, other_score = 0, score;
    int rc, any = 0;
    for (uint32_t j = 0; j < n; j++) {
        if ((rc = posting_at(w, d, group, key_len, j, &r))) return rc;
        if ((c->rules && !(c->rules & r.rules)) || is_duplicate(w, di, &r)) continue;
        if ((rc = rank_posting(w, d, &r, q, &score))) return rc;
        if (q->strict && !(score & (RANK_PREFERRED | RANK_KANA))) {
            if (!any || score > other_score) { other = r; other_score = score; any = 1; }
            continue;
        }
        if (best == n || score > best_score) { best = j; best_score = score; top = r; }
    }
    if (best == n && any && q->fallback && !q->fell_back) {
        q->fallback->dict = di;
        q->fallback->record = other;
        q->fell_back = 1;
    }
    if (best == n || w->n_matches >= limit) return VJO_OK;
    q->score = best_score;
    w->matches[w->n_matches].dict = di;
    w->matches[w->n_matches++].record = top;
    for (uint32_t j = 0; j < n && !q->strict && w->n_matches < limit; j++) {
        if (j == best) continue;
        if ((rc = posting_at(w, d, group, key_len, j, &r))) return rc;
        if ((c->rules && !(c->rules & r.rules)) || is_duplicate(w, di, &r)) continue;
        w->matches[w->n_matches].dict = di;
        w->matches[w->n_matches++].record = r;
    }
    return VJO_OK;
}

static int find_matches_v1(Workspace *w, int di, const Candidate *c, Query *q, int limit)
{
    Dictionary *d = &w->dicts[di];
    uint32_t lo = 0, hi = d->count, n = 0;
    Record r;
    char key[VJO_LOCAL_MAX_KEY + 1];
    int rc;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if ((rc = record_at(w, d, mid, &r, key))) return rc;
        if (strcmp(key, c->key) < 0) lo = mid + 1; else hi = mid;
    }
    /* Bound duplicate-heavy dictionaries as well as result storage. */
    for (; lo + n < d->count && n < MAX_HOMONYMS; n++) {
        if ((rc = record_at(w, d, lo + n, &r, key))) return rc;
        if (strcmp(key, c->key)) break;
    }
    return n ? take_postings(w, di, c, q, limit, lo, 0, n) : VJO_OK;
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


static int find_matches_v2(Workspace *w, int di, const Candidate *c, Query *q, int limit)
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
            return take_postings(w, di, c, q, limit, group, key_len, n);
        }
        if (!next) break;
        page = next;
    }
    /* Cache only proven absent hashes, never a filtered/duplicate result. A
     * collision cannot create a false negative: any equal hash prevents caching. */
    if (!hash_present) { w->misses[miss].hash = hash; w->misses[miss].dict = (unsigned)di + 1; }
    return VJO_OK;
}

/* Candidate c (0: the text itself, written as the raw span; otherwise a
 * deinflected dictionary form, written as its key) in dictionary di. */
static int find_matches(Workspace *w, int di, int c, const char *raw, size_t raw_len, Query *q, int limit)
{
    const Candidate *cand = &w->candidates[c];
    q->written = c ? cand->key : raw;
    q->written_len = c ? cand->len : raw_len;
    q->key = !c && (raw_len != cand->len || memcmp(raw, cand->key, raw_len)) ? cand : NULL;
    q->preferred = preferred_for(cand->key);
    return w->dicts[di].version == 2 ? find_matches_v2(w, di, cand, q, limit) : find_matches_v1(w, di, cand, q, limit);
}

static int deinflect(Candidate *c, const char *key)
{
    int count = 1;
    size_t len = strlen(key);
    memcpy(c[0].key, key, len + 1); c[0].len = (uint16_t)len; c[0].rules = 0; c[0].depth = 0;
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
            c[count].depth = (uint8_t)(c[i].depth + 1);
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
            k = vjo_utf8_encode(normalize(cp), enc);
            if (bytes + (size_t)k > VJO_LOCAL_MAX_KEY) break;
            memcpy(key + bytes, enc, (size_t)k); bytes += (size_t)k;
            count++; ends[count] = scan; lengths[count] = bytes; units[count] = u;
        }
        for (int k = count; k > 0 && !matched; k--) {
            int candidates;
            Query q = {0};
            key[lengths[k]] = 0;
            candidates = deinflect(w->candidates, key);
            w->n_matches = 0;
            /* Exact matches in all dictionaries precede deinflected matches;
             * each key's best-ranked posting comes first. */
            for (int c = 0; c < candidates && w->n_matches < VJO_LOCAL_MAX_MATCHES; c++)
                for (int d = 0; d < w->n_dicts && w->n_matches < VJO_LOCAL_MAX_MATCHES; d++)
                    if ((rc = find_matches(w, d, c, text + off, ends[k] - off, &q, VJO_LOCAL_MAX_MATCHES)))
                        goto done;
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
    size_t lengths[MAX_SCAN_CHARS + 1], ends[MAX_SCAN_CHARS + 1];
    int units[MAX_SCAN_CHARS + 1], pos16 = 0, count = 0, source_units = 0, complete = 0;
    unsigned resolved = 0;
    char key[VJO_LOCAL_MAX_KEY + 1];
    Match fallback[VJO_LOCAL_MAX_DICTS], chosen[VJO_LOCAL_MAX_DICTS];
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
        n = vjo_utf8_encode(normalize(cp), enc);
        if (bytes + (size_t)n > VJO_LOCAL_MAX_KEY) break;
        memcpy(key + bytes, enc, (size_t)n); bytes += (size_t)n;
        count++; lengths[count] = bytes; units[count] = source_units; ends[count] = off;
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
    /* Longest source span first. At one span, each dictionary's best entry
     * written as the text is (or a preferred reading): the text itself
     * before any inflection, then the higher rank, so kana text finds its
     * kana word or its inflection (した: する, なってる: なる) before a
     * homograph written in kanji (下). An exact hit in a later dictionary
     * beats an inflected hit in an earlier one; otherwise configured order.
     * A dictionary without such an entry takes the best homograph of its
     * first matching candidate. Each dictionary keeps only its best match;
     * the first one overall is the single result. Only page 0's payload is
     * read: other pages are located, never loaded. */
    for (int k = count; k > 0 && !complete; k--) {
        int candidates;
        unsigned fell_back = 0, found = 0, exact = 0;
        key[lengths[k]] = 0;
        candidates = deinflect(w->candidates, key);
        /* Phase 0: the text itself in each dictionary; phase 1: its inflections.
         * With one result, the first dictionary that has an entry ends it. */
        for (int phase = 0; phase < 2; phase++)
            for (int d = 0; d < w->n_dicts && !(limit == 1 && found); d++) {
            uint32_t top = 0;
            int depth = 0;
            if (((resolved | found) >> d) & 1) continue;
            for (int c = phase; c < (phase ? candidates : 1); c++) {
                int before = w->n_matches;
                Query q = {0};
                /* Candidates come in breadth-first order: fewer inflections first. */
                if (((found >> d) & 1) && w->candidates[c].depth > depth) break;
                q.strict = 1;
                q.fallback = &fallback[d];
                q.fell_back = (fell_back >> d) & 1;
                if ((rc = find_matches(w, d, c, text + cursor_byte, ends[k] - cursor_byte, &q, before + 1)))
                    goto done;
                fell_back |= (unsigned)q.fell_back << d;
                if (w->n_matches == before) continue;
                w->n_matches = before; /* held in chosen[d] until the span's order is known */
                if (((found >> d) & 1) && q.score <= top) continue;
                chosen[d] = w->matches[before];
                top = q.score;
                depth = w->candidates[c].depth;
                found |= 1u << d;
                exact |= (unsigned)!c << d;
            }
        }
        for (int pass = 0; pass < 3 && !complete; pass++)
            for (int d = 0; d < w->n_dicts && !complete; d++) {
                unsigned bit = 1u << d;
                const Match *m = &fallback[d];
                if (resolved & bit) continue;
                if (pass < 2) {
                    if (!(found & bit) || ((exact & bit) != 0) != (pass == 0)) continue;
                    m = &chosen[d];
                } else if (!(fell_back & bit)) continue;
                resolved |= bit;
                w->matches[w->n_matches] = *m;
                w->matches[w->n_matches++].units = (uint16_t)units[k];
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
