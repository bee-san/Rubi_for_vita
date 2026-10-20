/* Which text a job shows: OCR's, or a native hook stream's (docs/native-hooks.md). Plain C, no
 * allocation, no platform calls; the game worker calls it at job time in its own thread, the host tests
 * link it as is. The risk it guards against is showing the wrong text (a name, a backlog line, a line the
 * game prepared ahead of the screen), so a stream's text is shown in the automatic pick only once OCR
 * agrees with it, and a stream answers without OCR only once it is locked (two agreements in a row), has
 * new text since the last job of at least VJO_HOOK_MIN_CHARS characters, and for at most
 * VJO_HOOK_UNCHECKED_MAX jobs in a row: the next job runs OCR, which must agree with it (audit-hooks-text
 * F3). So a locked stream that runs ahead of the screen, or turns to a name or a UI string, shows at most
 * VJO_HOOK_UNCHECKED_MAX wrong pages before OCR catches it.
 *
 * pick is VjoGameOcrRequest.hook_pick: VJO_HOOK_PICK_AUTO, a stream ID (pinned), VJO_HOOK_PICK_OCR. */
#ifndef VJO_HOOK_SELECT_H
#define VJO_HOOK_SELECT_H

#include <stddef.h>
#include <stdint.h>

#define VJO_HOOK_AGREE 700u        /* similarity (0..1000) at or above which a stream agrees with OCR */
#define VJO_HOOK_MIN_CHARS 4u      /* fewer characters (after normalising) never agree, nor answer a job */
#define VJO_HOOK_LOCK_AGREEMENTS 2u
#define VJO_HOOK_UNLOCK_MISSES 2u
#define VJO_HOOK_UNCHECKED_MAX 2u  /* jobs in a row a locked stream answers without OCR; the next runs OCR */

/* One stream's decoded text at job time (game_ocr/hooks.h VjoHookSnap after vjo_hooks_text). */
typedef struct {
    uint32_t id;      /* stream ID, 1..VJO_HOOK_STREAMS */
    uint32_t seq;     /* the stream's seq: changes at each new text */
    uint32_t site;    /* 1: a table site, 0: a CALL caller (a tie goes to the site) */
    const char *text; /* UTF-8 */
    size_t n;         /* bytes of text */
    uint32_t key;     /* VjoHookSnap.key: a site's offset, a caller's return address. Another key under the
                         locked ID (its CALL slot went to another caller) unlocks it at once */
} VjoHookCand;

/* The selection's state for one game session (zero it at the worker's start). */
typedef struct {
    uint32_t locked;     /* the locked stream's ID, 0 none */
    uint32_t locked_seq; /* the locked stream's seq when a job last showed it */
    uint32_t streak_id;  /* the stream that agreed with the last job(s), 0 none */
    uint32_t streak;     /* its agreements in a row */
    uint32_t misses;     /* jobs in a row the locked stream disagreed with OCR */
    uint32_t unchecked;  /* jobs in a row the locked stream answered without OCR */
    uint32_t locked_key; /* the locked stream's key when it locked */
} VjoHookSelect;

/* Similarity, 0..1000, of a stream's text h to OCR's text o, after normalising both (full-width forms
 * folded, whitespace and punctuation dropped): the best match of h within o (o may hold a speaker's
 * name or a second line that h lacks), scaled by how much of o h covers. Fewer than VJO_HOOK_MIN_CHARS
 * characters in h (or in o): 0. */
unsigned vjo_hook_similarity(const char *o, size_t on, const char *h, size_t hn);

/* Before recognition: the stream this job may answer from WITHOUT OCR, or 0 (run OCR). A pinned pick
 * returns that stream when it has text (the user's choice: whatever its length); VJO_HOOK_PICK_OCR
 * returns 0. AUTO returns the locked stream only when all of these hold: its seq differs from
 * locked_seq (new text since the last job shown), its text has at least VJO_HOOK_MIN_CHARS compared
 * characters, no miss is pending (misses 0), fewer than VJO_HOOK_UNCHECKED_MAX jobs in a row answered so
 * (unchecked), and its key is the one it had at lock; then unchecked counts the job. *flags gets
 * VJO_HOOK_R_* (vjo_game_ocr.h) for the result. A job that answers so must still run the region's first
 * capture pass. */
uint32_t vjo_hook_select_before(VjoHookSelect *st, uint32_t pick, const VjoHookCand *c, unsigned n,
                                uint32_t *flags);

/* After recognition, with OCR's text: the stream whose text replaces it (0: keep OCR's), its
 * similarity in *score (the best stream's, also when none agreed; the locked stream's when it agrees,
 * which AUTO keeps even where another scores higher) and VJO_HOOK_R_* in *flags. Updates the streak,
 * the lock, the misses and the unchecked count (AUTO only; a pinned or OCR-only pick never changes them;
 * nor does an OCR text of fewer than VJO_HOOK_MIN_CHARS characters, which is no evidence). A locked
 * stream present with another key than at lock unlocks first (VJO_HOOK_R_UNLOCK), whatever OCR read.
 * The locked stream agreeing clears misses and unchecked; present and disagreeing (a text too short to
 * compare included), it counts a miss, and VJO_HOOK_UNLOCK_MISSES in a row unlock it. Equal scores go to
 * the streak's stream, then a site, then the newest seq. */
uint32_t vjo_hook_select_after(VjoHookSelect *st, uint32_t pick, const char *ocr, size_t on,
                               const VjoHookCand *c, unsigned n, unsigned *score, uint32_t *flags);

#endif
