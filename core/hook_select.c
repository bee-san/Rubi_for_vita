/* Which text a job shows: OCR's or a native hook stream's (core/hook_select.h, docs/native-hooks.md).
 * Plain C, no allocation, no platform calls. The comparison works on at most CMP_MAX characters of each
 * text, in a form where the same words read the same (folded widths, no spaces or punctuation), and
 * keeps its scratch on the stack: two code point arrays and one row of the edit distance, 2,562 B. */
#include "hook_select.h"

#include "../include/vjo_game_ocr.h" /* VJO_HOOK_PICK_*, VJO_HOOK_R_*, VJO_HOOK_STREAMS */

/* Characters of each text compared: a dialogue box holds far fewer (CLANNAD's three lines about 75), and
 * a stream's 512 raw bytes (game_ocr/hooks.h VJO_HOOK_RAW_BYTES) at most 256 CP932 characters. */
#define CMP_MAX 256u

/* The half-width katakana U+FF66-U+FF9D (CP932's single bytes 0xA6-0xDD) as their full-width forms,
 * U+30A0 + this. */
static const unsigned char half_kata[56] = {
    0x52, 0x01, 0x03, 0x05, 0x07, 0x09, 0x43, 0x45, 0x47, 0x23, 0x5C, 0x02, 0x04, 0x06, 0x08, 0x0A,
    0x0B, 0x0D, 0x0F, 0x11, 0x13, 0x15, 0x17, 0x19, 0x1B, 0x1D, 0x1F, 0x21, 0x24, 0x26, 0x28, 0x2A,
    0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x32, 0x35, 0x38, 0x3B, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x44, 0x46,
    0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4F, 0x53};

/* One code point of UTF-8 at p[*i] (n bytes in all), *i moved past it; 0 for a byte that does not
 * start a valid, shortest sequence (it is skipped alone). */
static uint32_t utf8_next(const unsigned char *p, size_t n, size_t *i)
{
    unsigned a = p[*i], len, k;
    uint32_t c, min;
    if (a < 0x80) {
        (*i)++;
        return a;
    }
    if (a >= 0xC2 && a <= 0xDF) { len = 2; c = a & 0x1Fu; min = 0x80; }
    else if (a >= 0xE0 && a <= 0xEF) { len = 3; c = a & 0x0Fu; min = 0x800; }
    else if (a >= 0xF0 && a <= 0xF4) { len = 4; c = a & 0x07u; min = 0x10000; }
    else { (*i)++; return 0; }
    if (n - *i < len) { (*i)++; return 0; }
    for (k = 1; k < len; k++) {
        unsigned b = p[*i + k];
        if ((b & 0xC0) != 0x80) { (*i)++; return 0; }
        c = c << 6 | (b & 0x3Fu);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) { (*i)++; return 0; }
    *i += len;
    return c;
}

/* The compared form of code point c, 0 when it is dropped: full-width ASCII as ASCII, capitals as small
 * letters, half-width katakana as full-width (*half set), 一 as ー (OCR reads one for the other);
 * whitespace, controls, punctuation and symbols (ASCII's, Latin-1's, U+2000-U+2BFF, the CJK marks but
 * 々〆〇 and the repeat marks, ・, the spacing (semi-)voiced marks, the vertical, small and half-width
 * forms, private use, emoji) dropped. */
static uint32_t fold(uint32_t c, int *half)
{
    *half = 0;
    if (c >= 0xFF01 && c <= 0xFF5E) c -= 0xFEE0;
    if (c < 0x80) {
        if (c >= 'A' && c <= 'Z') return c + 32u;
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ? c : 0u;
    }
    if (c >= 0xFF66 && c <= 0xFF9D) {
        *half = 1;
        return 0x30A0u + half_kata[c - 0xFF66];
    }
    if (c == 0x4E00) return 0x30FC;
    if (c >= 0x3000 && c <= 0x303F)
        return (c >= 0x3005 && c <= 0x3007) || (c >= 0x3031 && c <= 0x3035) || c == 0x303B ? c : 0u;
    if (c < 0xC0 || c == 0xD7 || c == 0xF7 || (c >= 0x2000 && c <= 0x2BFF) || (c >= 0x3099 && c <= 0x309C) ||
        c == 0x30A0 || c == 0x30FB || (c >= 0xE000 && c <= 0xF8FF) || (c >= 0xFE10 && c <= 0xFE6F) ||
        c == 0xFEFF || (c >= 0xFF5F && c <= 0xFF65) || (c >= 0xFFE0 && c <= 0xFFFF) ||
        (c >= 0x1F000 && c <= 0x1FAFF))
        return 0;
    return c;
}

/* A half-width voiced (U+FF9E) or semi-voiced (U+FF9F) mark on the full-width kana k it follows. */
static uint32_t voiced(uint32_t k, uint32_t mark)
{
    if (mark == 0xFF9E) {
        if (k == 0x30A6) return 0x30F4; /* ウ -> ヴ */
        if ((k >= 0x30AB && k <= 0x30C1 && !((k - 0x30AB) & 1u)) || k == 0x30C4 || k == 0x30C6 || k == 0x30C8)
            return k + 1u;
    }
    if (k >= 0x30CF && k <= 0x30DB && !((k - 0x30CF) % 3u)) return k + (mark == 0xFF9E ? 1u : 2u);
    return k;
}

/* The compared form of n bytes of UTF-8 s (up to a NUL): its first CMP_MAX characters in out, returned
 * as their count; *total gets every character's. */
static unsigned cmp_form(const char *s, size_t n, uint32_t *out, unsigned *total)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    unsigned all = 0;
    int half = 0, h;
    while (p && i < n && p[i]) {
        uint32_t c = utf8_next(p, n, &i);
        if (c == 0xFF9E || c == 0xFF9F) {
            if (half && all <= CMP_MAX) out[all - 1u] = voiced(out[all - 1u], c);
            half = 0;
            continue;
        }
        c = fold(c, &h);
        half = h && c;
        if (!c) continue;
        if (all < CMP_MAX) out[all] = c;
        all++;
    }
    *total = all;
    return all < CMP_MAX ? all : CMP_MAX;
}

/* The edit distance (unit costs) of all of h to the part of o it matches best: o's characters before
 * and after that part are free. One row of the table, O(m n). */
static unsigned within(const uint32_t *o, unsigned m, const uint32_t *h, unsigned n)
{
    uint16_t row[CMP_MAX + 1u];
    unsigned j, best;
    for (j = 0; j <= m; j++) row[j] = 0;
    for (unsigned i = 1; i <= n; i++) {
        uint32_t hc = h[i - 1u];
        unsigned diag = row[0], left = i;
        row[0] = (uint16_t)i;
        for (j = 1; j <= m; j++) {
            unsigned up = row[j], v = diag + (hc != o[j - 1u]), gap = (up < left ? up : left) + 1u;
            if (gap < v) v = gap;
            row[j] = (uint16_t)v;
            left = v;
            diag = up;
        }
    }
    best = row[0];
    for (j = 1; j <= m; j++)
        if (row[j] < best) best = row[j];
    return best;
}

/* h's similarity to o (its compared form: m characters kept, mf in all), 0..1000: the characters of h
 * that match o (h's length less its distance within o) over the longer text's length, so OCR's text
 * around the match (a name, another line) lowers it as much as it is long. Characters past CMP_MAX are
 * never compared and count as unmatched: a cut text scores no higher than it would whole. 0 when either
 * text has fewer than VJO_HOOK_MIN_CHARS characters, or when it cannot reach floor (its bound: no more of
 * h can match than o holds). */
static unsigned score_of(const uint32_t *o, unsigned m, unsigned mf, const char *h, size_t hn, unsigned floor)
{
    uint32_t hc[CMP_MAX];
    unsigned nf, n = cmp_form(h, hn, hc, &nf), top, d;
    if (nf < VJO_HOOK_MIN_CHARS || mf < VJO_HOOK_MIN_CHARS) return 0;
    top = nf > mf ? nf : mf;
    if ((n < m ? n : m) * 1000u / top < floor) return 0;
    d = within(o, m, hc, n);
    return d >= n ? 0u : (n - d) * 1000u / top;
}

/* #413's audit F2: OCR's text o (m characters kept, mf in all) against the beginning of h, h's first m characters, as
 * score_of scores h (n <= m <= mf): a line still typing out (a press reads at once since #413) shows its beginning.
 * For texts score_of compares (both of VJO_HOOK_MIN_CHARS or more); with h no longer than o it is score_of's. */
static unsigned prefix_score(const uint32_t *o, unsigned m, unsigned mf, const char *h, size_t hn)
{
    uint32_t hc[CMP_MAX];
    unsigned nf, n = cmp_form(h, hn, hc, &nf), d;
    if (n > m) n = m;
    d = within(o, m, hc, n);
    return d >= n ? 0u : (n - d) * 1000u / mf;
}

unsigned vjo_hook_similarity(const char *o, size_t on, const char *h, size_t hn)
{
    uint32_t oc[CMP_MAX];
    unsigned mf, m;
    if (!o || !h) return 0;
    m = cmp_form(o, on, oc, &mf);
    return score_of(oc, m, mf, h, hn, 0);
}

static int has_text(const VjoHookCand *k)
{
    return k->id && k->text && k->n;
}

/* Has UTF-8 s (n bytes, up to a NUL) at least VJO_HOOK_MIN_CHARS characters in the compared form? The
 * count cmp_form makes (a voiced mark is never a character of its own), without its array. */
static int enough_chars(const char *s, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    unsigned all = 0;
    int half;
    while (p && i < n && p[i] && all < VJO_HOOK_MIN_CHARS) {
        uint32_t c = utf8_next(p, n, &i);
        if (c != 0xFF9E && c != 0xFF9F && fold(c, &half)) all++;
    }
    return all >= VJO_HOOK_MIN_CHARS;
}

/* Fingerprint the same folded code points as cmp_form, including those beyond CMP_MAX. Only hashes
 * live between jobs; a collision conservatively delays learning/counting rather than adding evidence. */
static uint64_t hash_cp(uint64_t h, uint32_t c)
{
    for (unsigned i = 0; i < 4; i++, c >>= 8) h = (h ^ (c & 255u)) * UINT64_C(1099511628211);
    return h;
}

static uint64_t text_hash(const char *s, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    uint64_t h = UINT64_C(14695981039346656037);
    uint32_t pending = 0, count = 0;
    size_t i = 0;
    int half = 0, next_half;
    while (p && i < n && p[i]) {
        uint32_t c = utf8_next(p, n, &i);
        if (c == 0xFF9E || c == 0xFF9F) {
            if (half) pending = voiced(pending, c);
            half = 0;
            continue;
        }
        c = fold(c, &next_half);
        half = next_half && c;
        if (!c) continue;
        if (pending) h = hash_cp(h, pending);
        pending = c;
        count++;
    }
    if (pending) h = hash_cp(h, pending);
    h = hash_cp(h, count);
    return h ? h : 1u;
}

static void unlock(VjoHookSelect *st)
{
    *st = (VjoHookSelect){0};
}

int vjo_hook_select_restore(VjoHookSelect *st, const VjoHookLearned *saved)
{
    if (!st) return 0;
    unlock(st);
    if (!saved || saved->site > 1u || saved->trusted > 1u ||
        (saved->interval != 8u && saved->interval != 16u && saved->interval != 32u)) return 0;
    st->locked_site = saved->site;
    st->locked_key = saved->key;
    st->locked_encoding = saved->encoding;
    st->interval = saved->interval;
    st->confirm = 1;
    /* API 49: saved trusted (vjo_hook_select_learned with trust on): trusted again at once, without the confirmation
     * OCR read a restored source otherwise gets (vjo_hook_select_trusted ignores confirm; with hook_trust = off the
     * confirmation runs as before) */
    st->strong = saved->trusted && saved->interval >= VJO_HOOK_TRUST_INTERVAL;
    return 1;
}

int vjo_hook_select_learned(const VjoHookSelect *st, int trust, VjoHookLearned *out)
{
    if (out) *out = (VjoHookLearned){0};
    if (!st || !st->interval) return 0;
    if (out)
        *out = (VjoHookLearned){st->locked_site, st->locked_key, st->locked_encoding, st->interval,
                                trust && st->strong && st->interval >= VJO_HOOK_TRUST_INTERVAL};
    return 1;
}

static int same_source(const VjoHookCand *k, uint32_t site, uint32_t key, uint32_t encoding)
{
    return !!k->site == !!site && k->key == key && k->encoding == encoding;
}

static const VjoHookCand *locked_cand(const VjoHookSelect *st, const VjoHookCand *c, unsigned n)
{
    for (unsigned i = 0; st->interval && i < n; i++)
        if (same_source(&c[i], st->locked_site, st->locked_key, st->locked_encoding) && has_text(&c[i]))
            return &c[i];
    return NULL;
}

uint32_t vjo_hook_select_before(VjoHookSelect *st, uint32_t pick, const VjoHookCand *c, unsigned n,
                                uint32_t *flags)
{
    const VjoHookCand *k = NULL;
    uint64_t h;
    if (flags) *flags = 0;
    if (!st || (!c && n) || pick == VJO_HOOK_PICK_OCR || pick > VJO_HOOK_STREAMS) return 0;
    if (pick != VJO_HOOK_PICK_AUTO) {
        for (unsigned i = 0; i < n && !k; i++)
            if (c[i].id == pick && has_text(&c[i])) k = &c[i];
        if (!k) return 0;
        if (flags) *flags = VJO_HOOK_R_PINNED | VJO_HOOK_R_NO_OCR | (k->site ? VJO_HOOK_R_SITE : 0u);
        return pick;
    }
    /* These game-specific recipes already passed installation's version/placement checks. Their
     * text is authoritative without OCR evidence; leave generic learning and its cadence alone.
     * Sequence subtraction follows the hook engine's wrapping stream clock. */
    for (unsigned i = 0; i < n; i++) {
        if ((c[i].site & (VJO_HOOK_CAND_SITE | VJO_HOOK_CAND_LUNA)) !=
            (VJO_HOOK_CAND_SITE | VJO_HOOK_CAND_LUNA) || !has_text(&c[i])) continue;
        if (!k || (int32_t)(c[i].seq - k->seq) > 0 ||
            (c[i].seq == k->seq && c[i].id < k->id)) k = &c[i];
    }
    if (k) {
        if (flags) *flags = VJO_HOOK_R_SITE | VJO_HOOK_R_NO_OCR;
        return k->id;
    }
    k = locked_cand(st, c, n);
    if (!k || !enough_chars(k->text, k->n)) {
        if (st->interval) st->confirm = 1;
        return 0;
    }
    st->locked = k->id; /* a restored source, or the same source in a different runtime slot */
    h = text_hash(k->text, k->n);
    if (st->confirm || st->misses || h == st->shown_hash || st->unchecked + 1u >= st->interval) return 0;
    st->locked_seq = k->seq;
    st->shown_hash = h;
    st->unchecked++;
    if (flags) *flags = VJO_HOOK_R_LOCKED | VJO_HOOK_R_NO_OCR | (k->site ? VJO_HOOK_R_SITE : 0u);
    return k->id;
}

uint32_t vjo_hook_select_trusted(VjoHookSelect *st, uint32_t pick, const VjoHookCand *c, unsigned n,
                                 uint32_t *flags)
{
    const VjoHookCand *k = NULL;
    if (flags) *flags = 0;
    if (!st || (!c && n) || pick != VJO_HOOK_PICK_AUTO) return 0;
    /* A site whose recipe passed installation's checks (Luna's version and placement; Rubi's module NID and
     * recorded bytes): the newest text among them, as Luna's sites answer in vjo_hook_select_before. */
    for (unsigned i = 0; i < n; i++) {
        if (!(c[i].site & VJO_HOOK_CAND_SITE) || !((c[i].site & VJO_HOOK_CAND_LUNA) || c[i].verified) ||
            !has_text(&c[i])) continue;
        if (!k || (int32_t)(c[i].seq - k->seq) > 0 || (c[i].seq == k->seq && c[i].id < k->id)) k = &c[i];
    }
    if (k) {
        if (flags) *flags = VJO_HOOK_R_TRUSTED | VJO_HOOK_R_SITE | VJO_HOOK_R_NO_OCR;
        return k->id;
    }
    /* The learned source once it has agreed with OCR often enough, and closely at the trusted interval (strong); a
     * pending disagreement first needs its comparison (vjo_hook_select_before and _after, which reset the interval
     * to 8 on a match). */
    if (st->interval < VJO_HOOK_TRUST_INTERVAL || !st->strong || st->misses || !(k = locked_cand(st, c, n))) return 0;
    st->locked = k->id; /* a restored source, or the same source in a different runtime slot */
    if (flags) *flags = VJO_HOOK_R_TRUSTED | VJO_HOOK_R_LOCKED | VJO_HOOK_R_NO_OCR | (k->site ? VJO_HOOK_R_SITE : 0u);
    return k->id;
}

/* At equal scores, is a the better pick than b? The stream of the current streak (two copies of one text
 * must not take turns and keep each other from locking), then a table site, then the newer text. */
static int wins_tie(const VjoHookCand *a, const VjoHookCand *b, uint32_t streak_id)
{
    if ((a->id == streak_id) != (b->id == streak_id)) return a->id == streak_id;
    if (!a->site != !b->site) return a->site != 0;
    return (int32_t)(a->seq - b->seq) > 0;
}

uint32_t vjo_hook_select_no_memory(uint32_t pick, const VjoHookCand *c, unsigned n, uint32_t *flags)
{
    const VjoHookCand *best = NULL;
    if (flags) *flags = 0;
    if (pick != VJO_HOOK_PICK_AUTO) return 0;
    for (unsigned i = 0; c && i < n; i++) {
        const VjoHookCand *k = &c[i];
        if (!has_text(k) || !k->site || !k->verified || !enough_chars(k->text, k->n)) continue;
        if (!best || (int32_t)(k->seq - best->seq) > 0) best = k;
    }
    if (!best) return 0;
    if (flags) *flags = VJO_HOOK_R_UNCONFIRMED | VJO_HOOK_R_NO_OCR | VJO_HOOK_R_SITE;
    return best->id;
}

uint32_t vjo_hook_select_after(VjoHookSelect *st, uint32_t pick, const char *ocr, size_t on,
                               const VjoHookCand *c, unsigned n, unsigned *score, uint32_t *flags, int unsettled)
{
    uint32_t oc[CMP_MAX], f = 0;
    unsigned mf, m, best = 0, agree = 0, lscore = 0, pscore = 0;
    const VjoHookCand *top = NULL, *chosen = NULL, *locked = NULL, *pinned = NULL;
    uint64_t hh = 0, oh;
    if (score) *score = 0;
    if (flags) *flags = 0;
    if (!st || (!c && n)) return 0;
    if (pick == VJO_HOOK_PICK_AUTO && st->interval) {
        locked = locked_cand(st, c, n);
        if (locked) st->locked = locked->id;
        else {
            /* If the old slot now names another source, that source needs its own learning. A busy
             * or absent slot is merely temporary and keeps its identity/interval. */
            for (unsigned i = 0; st->locked && i < n; i++) {
                if (c[i].id == st->locked && !same_source(&c[i], st->locked_site, st->locked_key,
                                                         st->locked_encoding)) {
                    unlock(st);
                    f |= VJO_HOOK_R_UNLOCK;
                    break;
                }
            }
            if (st->interval) st->confirm = 1;
        }
    }
    m = cmp_form(ocr, ocr ? on : 0, oc, &mf);
    for (unsigned i = 0; i < n; i++) {
        const VjoHookCand *k = &c[i];
        int is_locked = k == locked, is_pinned = k->id == pick;
        unsigned s;
        if (!has_text(k)) continue;
        s = score_of(oc, m, mf, k->text, k->n, is_locked || is_pinned ? 0u : best);
        if (is_locked) lscore = s;
        if (is_pinned) { pinned = k; pscore = s; }
        if (s > best || (s && s == best && wins_tie(k, top, st->streak_id))) {
            best = s;
            top = k;
        }
    }
    if (pick != VJO_HOOK_PICK_AUTO) {
        if (pinned && pick <= VJO_HOOK_STREAMS) {
            if (score) *score = pscore;
            if (flags) *flags = VJO_HOOK_R_PINNED | (pscore >= VJO_HOOK_AGREE ? VJO_HOOK_R_AGREED : 0u) |
                                (pinned->site ? VJO_HOOK_R_SITE : 0u);
            return pinned->id;
        }
        if (score) *score = best;
        return 0;
    }
    if (locked && enough_chars(locked->text, locked->n)) hh = text_hash(locked->text, locked->n);
    else if (st->interval) st->confirm = 1;
    oh = mf >= VJO_HOOK_MIN_CHARS ? text_hash(ocr, on) : 0;
    if (!oh) { /* no evidence: retain the last comparable pair, including a pending disagreement */
        /* An unscheduled inconclusive read asks for a confirmation next time. An overdue scheduled
         * check stays overdue so its eventual valid match can still increase the interval. */
        if (st->interval && st->unchecked + 1u < st->interval) st->confirm = 1;
        if (flags) *flags = f;
        return 0;
    }
    if (locked && lscore >= VJO_HOOK_AGREE) { chosen = locked; agree = lscore; }
    else if (top && best >= VJO_HOOK_AGREE) { chosen = top; agree = best; }
    if (st->interval && hh) {
        int fresh = hh != st->compare_hook_hash ||
                    (st->compare_ocr_hash && oh != st->compare_ocr_hash);
        /* #413's audit F2: read without the wait (a press since #413), OCR's text agrees with the beginning of the
         * locked stream's line: the line typing out, not a disagreement. No evidence either way: nothing changes, not
         * the pair compared last, a pending check or disagreement, or trust. A read with the wait is compared as
         * before: a stream that holds more than the still screen (a page that collects its lines) still disagrees. */
        if (unsettled && fresh && lscore < VJO_HOOK_AGREE &&
            prefix_score(oc, m, mf, locked->text, locked->n) >= VJO_HOOK_AGREE)
            fresh = 0;
        if (fresh) {
            st->compare_hook_hash = hh;
            st->compare_ocr_hash = oh;
            if (lscore >= VJO_HOOK_AGREE) {
                int scheduled = hh != st->shown_hash && !st->confirm && !st->misses &&
                                st->unchecked + 1u >= st->interval;
                if (st->misses) st->interval = VJO_HOOK_INTERVAL_MIN;
                else if (scheduled && st->interval < VJO_HOOK_INTERVAL_MAX) st->interval *= 2u;
                if (scheduled || st->confirm || st->misses) st->unchecked = 0;
                st->confirm = st->misses = 0;
                st->shown_hash = hh;
                st->locked_seq = locked->seq;
                /* API 43: trust needs an agreement this close at the trusted interval (vjo_hook_select_trusted) */
                if (lscore >= VJO_HOOK_TRUST_SCORE && st->interval >= VJO_HOOK_TRUST_INTERVAL) st->strong = 1;
            } else {
                st->confirm = 1;
                st->strong = 0;
                if (++st->misses >= VJO_HOOK_UNLOCK_MISSES) {
                    unlock(st);
                    f |= VJO_HOOK_R_UNLOCK;
                    locked = NULL;
                }
            }
        }
    }
    if (!chosen) {
        if (!st->interval && mf >= VJO_HOOK_MIN_CHARS) st->streak_id = st->streak = 0;
        if (score) *score = best;
        if (flags) *flags = f;
        return 0;
    }
    if (!st->interval) {
        uint64_t h = text_hash(chosen->text, chosen->n);
        if (st->streak && same_source(chosen, st->streak_site, st->streak_key, st->streak_encoding)) {
            if (h != st->streak_hash) { st->streak++; st->streak_hash = h; }
        } else {
            st->streak = 1;
            st->streak_hash = h;
            st->streak_key = chosen->key;
            st->streak_site = !!chosen->site;
            st->streak_encoding = chosen->encoding;
        }
        st->streak_id = chosen->id;
        if (st->streak >= VJO_HOOK_LOCK_AGREEMENTS) {
            st->locked = chosen->id;
            st->locked_key = chosen->key;
            st->locked_site = !!chosen->site;
            st->locked_encoding = chosen->encoding;
            st->interval = VJO_HOOK_INTERVAL_MIN;
            st->shown_hash = st->compare_hook_hash = h;
            st->compare_ocr_hash = oh;
            st->locked_seq = chosen->seq;
        }
    }
    if (st->interval && same_source(chosen, st->locked_site, st->locked_key, st->locked_encoding))
        f |= VJO_HOOK_R_LOCKED;
    if (score) *score = agree;
    if (flags) *flags = f | VJO_HOOK_R_AGREED | (chosen->site ? VJO_HOOK_R_SITE : 0u);
    return chosen->id;
}
