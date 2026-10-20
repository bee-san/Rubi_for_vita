/* A small regular-expression engine for exactly the patterns of LunaTranslator's
 * PS Vita filters (core/hook_luna.c), kept verbatim in their ECMAScript syntax
 * (Luna's re::sub is C++'s std::regex_replace with ECMAScript): literals, '.',
 * classes ([...], [^...], ranges, \d \w \s \S inside), groups, '|', the greedy and
 * lazy '*', '+', '?', and the anchors '^' and '$' (no multiline). Anything else
 * (backreferences, lookarounds, {n,m}, (?:, \c without a letter...) is refused.
 *
 * Plain C: no allocation, no platform calls, its state on the caller's stack
 * (under 2 KiB), time bounded by the caller's budget (a Pike VM: each step of a
 * search costs at most the program's length, so no pattern backtracks
 * exponentially). The game worker runs it at job time, in its own thread. */
#ifndef VJO_HOOK_RE_H
#define VJO_HOOK_RE_H

#include <stddef.h>

/* flags: the units the pattern matches. 0: UTF-8 code points (Luna's std::wregex
 * over UTF-16, or over text it decoded first); VJO_RE_BYTES: bytes (Luna's
 * std::regex over a byte string: the same bytes when the text is UTF-8). */
#define VJO_RE_BYTES 1u

#define VJO_RE_EPATTERN (-1) /* the pattern (or the replacement) is outside the subset */
#define VJO_RE_EBUDGET (-2)  /* *budget ran out */

/* As std::regex_replace(in, regex(pat, ECMAScript), rep): every match, from the
 * left, replaced by rep ($1-$9 a group, at most one group besides $& per
 * replacement; $& the match; $$ a dollar sign; an unmatched group is empty); an
 * empty match after an empty one first retries as a non-empty match at the same
 * place, then moves on by one unit. The result goes to out (cap >= 1 bytes,
 * NUL-terminated), cut at the last whole UTF-8 character that fits. n <= 65535.
 * Each step of the matcher takes one from *budget. Returns the bytes written, or
 * VJO_RE_EPATTERN / VJO_RE_EBUDGET (out then holds an empty string). */
int vjo_re_sub(const char *pat, unsigned flags, const char *in, size_t n, const char *rep, char *out,
               size_t cap, unsigned long *budget);

/* As std::regex_search: the leftmost match in in[0..n). 1 and m[0..1] the match's
 * [start, end), m[2..3] group g's (both -1 when it did not take part; g 0: the
 * match again); 0 when nothing matches; VJO_RE_EPATTERN / VJO_RE_EBUDGET. */
int vjo_re_search(const char *pat, unsigned flags, const char *in, size_t n, unsigned g, long m[4],
                  unsigned long *budget);

#endif
