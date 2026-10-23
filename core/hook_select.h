/* Adaptive native-hook validation, pure C with no allocation or platform calls. See
 * docs/native-hooks.md. All state belongs to the game worker, never SceShell. */
#ifndef VJO_HOOK_SELECT_H
#define VJO_HOOK_SELECT_H

#include <stddef.h>
#include <stdint.h>

#define VJO_HOOK_AGREE 700u        /* similarity (0..1000) at or above which a stream agrees with OCR */
#define VJO_HOOK_MIN_CHARS 4u      /* fewer characters (after normalising) never agree, nor answer a job */
#define VJO_HOOK_LOCK_AGREEMENTS 2u
#define VJO_HOOK_UNLOCK_MISSES 2u
#define VJO_HOOK_INTERVAL_MIN 8u
#define VJO_HOOK_INTERVAL_MAX 32u

/* One stream's decoded text at job time (game_ocr/hooks.h VjoHookSnap after vjo_hooks_text). */
typedef struct {
    uint32_t id;      /* stream ID, 1..VJO_HOOK_STREAMS */
    uint32_t seq;     /* the stream's seq: changes at each new text */
    uint32_t site;    /* 1: a table site, 0: a CALL caller (a tie goes to the site) */
    const char *text; /* UTF-8 */
    size_t n;         /* bytes of text */
    uint32_t key;     /* module-relative site/caller offset, never an absolute address */
    uint32_t encoding; /* decoding identity; definition/filter changes also invalidate the cache revision */
    uint32_t verified; /* 1: a Rubi verified table site (core/hook_place.c), checked against the game's bytes */
} VjoHookCand;

/* The only learned data that may be persisted. A slot ID, runtime address, dialogue or partial
 * comparison history never belongs in a cache. The worker scopes this to the exact executable and
 * hook-definition revision. site is the same boolean as VjoHookCand.site. */
typedef struct {
    uint32_t site, key, encoding, interval;
} VjoHookLearned;

/* One game's state, owned by its worker thread. Zero initially; copy before a job and restore the
 * copy on failure. Content fingerprints avoid treating background writes or reopened dialogue as
 * fresh readings. They contain no dialogue and are never persisted. interval != 0 means learned,
 * including a restored source not yet mapped to its current runtime slot (locked == 0). */
typedef struct {
    uint32_t locked, locked_seq, streak_id, streak, misses, unchecked, locked_key;
    uint32_t locked_site, locked_encoding, interval, confirm;
    uint32_t streak_key, streak_site, streak_encoding;
    uint64_t shown_hash, streak_hash, compare_hook_hash, compare_ocr_hash;
} VjoHookSelect;

/* Restoring clears transient state and requires one OCR confirmation. Bad data clears the state and
 * returns 0. Export includes provisional learning so an inconclusive startup never deletes a cache.
 * With no learned source export clears *out and returns 0. */
int vjo_hook_select_restore(VjoHookSelect *st, const VjoHookLearned *saved);
int vjo_hook_select_learned(const VjoHookSelect *st, VjoHookLearned *out);

/* Similarity, 0..1000, of a stream's text h to OCR's text o, after normalising both (full-width forms
 * folded, whitespace and punctuation dropped): the best match of h within o (o may hold a speaker's
 * name or a second line that h lacks), scaled by how much of o h covers. Fewer than VJO_HOOK_MIN_CHARS
 * characters in h (or in o): 0. */
unsigned vjo_hook_similarity(const char *o, size_t on, const char *h, size_t hn);

/* Before recognition: a pinned stream with text, or a learned AUTO source with fresh comparable
 * text and no confirmation/check due, may answer without OCR. The first interval permits seven
 * hook-only readings, then checks the eighth. Unchanged text still takes the existing OCR path but
 * does not advance the cadence. A missing/short source uses OCR and requires confirmation on return.
 * A successful job must still capture the region; rollback state when capture/job delivery fails. */
uint32_t vjo_hook_select_before(VjoHookSelect *st, uint32_t pick, const VjoHookCand *c, unsigned n,
                                uint32_t *flags);

/* After recognition: keep OCR on an inconclusive or rejected comparison; return an agreeing hook
 * when present. Initial learning needs two distinct normalized lines from the same stable source.
 * A scheduled successful check doubles the interval up to32; startup/missing-text confirmation does
 * not. One comparable disagreement retains the source and checks the next new line; a match resets
 * to8, two disagreements unlock. Inconclusive or duplicate comparisons neither add nor clear misses.
 * Pinned/OCR-only calls never change AUTO state. Equal scores prefer the streak, a site, newest seq. */
uint32_t vjo_hook_select_after(VjoHookSelect *st, uint32_t pick, const char *ocr, size_t on,
                               const VjoHookCand *c, unsigned n, unsigned *score, uint32_t *flags);

/* After OCR was refused for memory, so no stream can ever agree with it (a game that leaves less than
 * the smallest model's reserve free): with the automatic pick, the newest candidate from a Rubi verified
 * table site (c->verified) with at least VJO_HOOK_MIN_CHARS characters answers unconfirmed (flags:
 * VJO_HOOK_R_UNCONFIRMED | VJO_HOOK_R_NO_OCR | VJO_HOOK_R_SITE). Luna's sites and CALL callers never do:
 * only a site whose bytes Rubi checked. A pinned or OCR-only pick, or no such candidate: 0. Never changes
 * the automatic pick's state (nothing is learned without OCR). */
uint32_t vjo_hook_select_no_memory(uint32_t pick, const VjoHookCand *c, unsigned n, uint32_t *flags);

#endif
