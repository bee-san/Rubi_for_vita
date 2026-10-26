/* Adaptive native-hook validation, pure C with no allocation or platform calls. See
 * docs/native-hooks.md. All state belongs to the game worker, never SceShell. */
#ifndef VJO_HOOK_SELECT_H
#define VJO_HOOK_SELECT_H

#include <stddef.h>
#include <stdint.h>

#define VJO_HOOK_AGREE 700u        /* similarity (0..1000) at or above which a stream agrees with OCR */
#define VJO_HOOK_MIN_CHARS 4u      /* fewer characters (after normalising) never establish OCR agreement */
#define VJO_HOOK_LOCK_AGREEMENTS 2u
#define VJO_HOOK_UNLOCK_MISSES 2u
#define VJO_HOOK_INTERVAL_MIN 8u
#define VJO_HOOK_INTERVAL_MAX 32u
/* API 43 (VJO_GAME_OCR_S_TRUST, config.ini hook_trust): a learned regular source is trusted once its interval
 * reaches this, that is after VJO_HOOK_LOCK_AGREEMENTS agreements and one scheduled check per doubling: 8 -> 16
 * after its first check, 16 -> 32 after its second, so 32 is four OCR agreements on four distinct lines
 * (docs/native-hooks.md, "Trusted hooks": why four), and once an agreement at that interval scored at least
 * VJO_HOOK_TRUST_SCORE. One of the cache's intervals (8, 16 or 32), so a saved source keeps its level. */
#ifndef VJO_HOOK_TRUST_INTERVAL
#define VJO_HOOK_TRUST_INTERVAL 32u
#endif
typedef char vjo_hook_trust_interval_check[VJO_HOOK_TRUST_INTERVAL == 8u || VJO_HOOK_TRUST_INTERVAL == 16u ||
                                           VJO_HOOK_TRUST_INTERVAL == 32u ? 1 : -1];
/* The similarity (0..1000) an agreement at VJO_HOOK_TRUST_INTERVAL needs to grant trust (VjoHookSelect.strong): a
 * source with the screen's line agrees at 1000 or close to it (OCR's misreads aside), one with a line the screen's
 * text only contains (the line before, part of the box) at 700-850 (docs/native-hooks.md: measured). */
#define VJO_HOOK_TRUST_SCORE 900u

/* Candidate provenance in the existing site word; no extra per-stream storage. */
#define VJO_HOOK_CAND_SITE 1u
#define VJO_HOOK_CAND_LUNA 2u /* only a successfully installed Luna table site */

/* One stream's decoded text at job time (game_ocr/hooks.h VjoHookSnap after vjo_hooks_text). */
typedef struct {
    uint32_t id;      /* stream ID, 1..VJO_HOOK_STREAMS */
    uint32_t seq;     /* the stream's seq: changes at each new text */
    uint32_t site;    /* VJO_HOOK_CAND_* flags; 0: a CALL caller (a tie goes to a site) */
    const char *text; /* UTF-8 */
    size_t n;         /* bytes of text */
    uint32_t key;     /* module-relative site/caller offset, never an absolute address */
    uint32_t encoding; /* decoding identity; definition/filter changes also invalidate the cache revision */
    uint32_t verified; /* 1: a Rubi verified table site (core/hook_place.c), checked against the game's bytes */
} VjoHookCand;

/* The only learned data that may be persisted. A slot ID, runtime address, dialogue or partial
 * comparison history never belongs in a cache. The worker scopes this to the exact executable and
 * hook-definition revision. site is the boolean !!VjoHookCand.site, not its provenance flags. */
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
    /* API 43: 1 once the locked source agreed at VJO_HOOK_TRUST_INTERVAL with at least VJO_HOOK_TRUST_SCORE (a
     * scheduled check, or a restored source's startup confirmation); 0 again at a disagreement. Never persisted. */
    uint32_t strong;
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

/* Before recognition: a pinned stream with text, or AUTO's newest nonempty Luna site, answers without
 * OCR. Luna does not consult or change learned AUTO state, including for short or repeated text.
 * Otherwise a learned AUTO source with fresh comparable text and no confirmation/check due may
 * answer without OCR. The first interval permits seven
 * hook-only readings, then checks the eighth. Unchanged text still takes the existing OCR path but
 * does not advance the cadence. A missing/short source uses OCR and requires confirmation on return.
 * A successful job must still capture the region; rollback state when capture/job delivery fails. */
uint32_t vjo_hook_select_before(VjoHookSelect *st, uint32_t pick, const VjoHookCand *c, unsigned n,
                                uint32_t *flags);

/* API 43, a trusted hook (VJO_GAME_OCR_S_TRUST, config.ini hook_trust): called before vjo_hook_select_before,
 * with the automatic pick only. A trusted source with text answers without OCR, as Luna's sites do without the
 * request's bit: first Luna's sites and Rubi's verified sites (c->verified: module NID and every recorded byte
 * checked as the game started), the newest text among them (a tie: the lower ID); else the learned regular source
 * once trusted (interval >= VJO_HOOK_TRUST_INTERVAL, strong, no pending disagreement) while a slot holds it (the same
 * kind, key and encoding). Short and unchanged texts answer too; no check falls due. Flags: VJO_HOOK_R_TRUSTED |
 * VJO_HOOK_R_NO_OCR, and VJO_HOOK_R_SITE for a site, VJO_HOOK_R_LOCKED for the learned source. Changes nothing
 * but st->locked (the learned source's runtime slot, as vjo_hook_select_before records it). 0 (no trusted source
 * has text, or another pick): the caller goes on to vjo_hook_select_before, so OCR resumes by itself. */
uint32_t vjo_hook_select_trusted(VjoHookSelect *st, uint32_t pick, const VjoHookCand *c, unsigned n,
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
