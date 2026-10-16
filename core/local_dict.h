/* Disk-backed Yomitan term dictionaries; see docs/local-dictionaries.md. */
#ifndef VJO_LOCAL_DICT_H
#define VJO_LOCAL_DICT_H
#include "client.h"

#define VJO_LOCAL_MAX_DICTS 8
#define VJO_LOCAL_MAX_KEY 192
#define VJO_LOCAL_MAX_ENTRY 8192
#define VJO_LOCAL_MAX_TEXT 4096
#define VJO_LOCAL_MAX_RESULTS 128
#define VJO_LOCAL_MAX_MATCHES 8
#define VJO_LOCAL_RESULT_BUDGET (64u * 1024u)
#define VJO_LOCAL_SCRATCH_BUDGET (48u * 1024u)
/* One maximum-size payload plus its vocabulary, token and alignment. */
#define VJO_LOCAL_SELECTED_BUDGET (VJO_LOCAL_MAX_ENTRY + 256u)

#ifdef VJO_LOCAL_STATS
/* Host benchmark counters (host/dict_bench.c, tools/dict_lookup_bench.py),
 * compiled only into the benchmark: every other build has none of this code.
 * Reads are counted where local_dict.c reads the file, by what they fetch. */
enum { VJO_READ_HEADER, VJO_READ_INDEX, VJO_READ_GROUP, VJO_READ_HEAD, VJO_READ_PAYLOAD,
       VJO_READ_FILTER, VJO_READ_KINDS };
typedef struct {
    uint64_t spans;        /* source spans tried, longest first */
    uint64_t candidates;   /* deinflect() output: the span itself and its deinflections */
    uint64_t probes;       /* candidate x dictionary searches */
    uint64_t filtered;     /* probes a key filter answered (absent) without an index read */
    uint64_t miss_cached;  /* probes the per-request absent-hash cache answered */
    /* v2: probes whose hash is in the index (rule-compatible or not); v1: not counted */
    uint64_t hash_present;
    /* v2: probes that found the full key with fitting rules; v1: the key is in the
     * index, whatever its rules (take_postings filters them) */
    uint64_t found;
    uint64_t ranked;       /* postings ranked by their payload's head (a read unless cached) */
    uint64_t hinted;       /* postings ranked by their posting hints, without the head */
    uint64_t absent_reads; /* index and group reads of probes that found no key */
    uint64_t reads[VJO_READ_KINDS], bytes[VJO_READ_KINDS];
} VjoLocalStats;
extern VjoLocalStats vjo_local_stats;
/* Optional: every candidate search, its key and how it ended
 * (VJO_PROBE_*), for designing key filters offline. */
enum { VJO_PROBE_ABSENT, VJO_PROBE_CACHED, VJO_PROBE_FILTERED, VJO_PROBE_HASH, VJO_PROBE_FOUND };
extern void (*vjo_local_trace)(int dict, int cand, int depth, const char *key, int outcome);
#endif

/* Validate comma-separated, relative .vjdict filenames (no directory traversal). */
int vjo_local_names_valid(const char *names);
/* Number of valid names (1..8), or 0 when the list is invalid. */
int vjo_local_names_count(const char *names);
int vjo_local_lookup(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                     const char *text, VjoDictResult *res, VjoErr *err);

/* Zero or one best match starting at cursor_byte in raw text. The cursor must
 * be a decoder codepoint boundary; invalid offsets return VJO_E_PARSE.
 * Malformed bytes keep the existing one-byte replacement behavior. Searches
 * at most 32 characters, stopping at punctuation/whitespace, without scanning
 * later words. Longest source span wins. At that span, an entry written as
 * the text is (kana text: the kana word; inflected text: the dictionary form;
 * or a listed common reading) wins over a homograph, the text itself before
 * its inflections (fewer steps first), then dictionary order; a key's
 * postings are ranked (local_dict.c, docs/local-dictionaries.md). Token
 * positions include raw newlines and use UTF-16 units. */
int vjo_local_lookup_at(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                        const char *text, size_t cursor_byte,
                        VjoDictResult *res, VjoErr *err);

/* Where one dictionary's selected entry is on storage. Paging between
 * dictionaries keeps these 24-byte locators, never several definitions. */
typedef struct {
    uint64_t entry_off; /* payload offset in its dictionary */
    uint64_t file_size; /* that file's size when located: detects replacement */
    uint32_t entry_len;
    uint16_t len16;     /* matched source span from the cursor, UTF-16 units */
    uint8_t dict;       /* index in local_dictionaries */
    uint8_t reserved;
} VjoLocalPage;

struct VjoLocalPages { /* typedef in client.h */
    VjoLocalPage page[VJO_LOCAL_MAX_DICTS];
    uint32_t cursor_byte; /* the raw-text cursor every page starts at */
    int pos16;            /* its UTF-16 position */
    int count;
};

/* vjo_local_lookup_at for up to `limit` (1..8) dictionaries. Page 0 is exactly
 * vjo_local_lookup_at's single result, also read into res. Later pages are the
 * best match (longest span, then as above) of each other dictionary that has
 * one, in local_dictionaries order; only their locators
 * are kept. limit 1 performs the same reads as vjo_local_lookup_at, and pages
 * may then be NULL (nothing to locate). */
int vjo_local_lookup_pages(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                           const char *text, size_t cursor_byte, int limit,
                           VjoLocalPages *pages, VjoDictResult *res, VjoErr *err);

/* Reads one located page's entry into `a` (one vocabulary, one token), opening
 * only that dictionary and checking its header and size again. Uses no page
 * cache or deinflection scratch: at most VJO_LOCAL_SELECTED_BUDGET bytes. */
int vjo_local_read_page(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                        const VjoLocalPages *pages, int page,
                        VjoDictResult *res, VjoErr *err);

#ifdef VJO_HOST
/* Host tests: the built-in common-reading table and the ranked files' usual
 * spellings, key then value, each NUL terminated, ending with an empty key
 * (local_dict.c). Not in the Vita build. */
const char *vjo_local_preferred_table(void);
const char *vjo_local_usual_table(void);
#endif
#endif
