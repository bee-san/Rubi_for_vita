/* The regular-expression subset of LunaTranslator's Vita filters (core/hook_re.h).
 *
 * A pattern compiles into at most RE_PROG instructions of a Pike VM (Thompson's
 * construction, relative jumps): threads run in priority order, so the match is
 * the one ECMAScript's backtracking would find (leftmost, then greedy or lazy
 * preference), with no backtracking: one search step costs at most the
 * program's length. Captures are kept only for the match and the one group the
 * replacement (or the search) asks for. All state is on the stack. */
#include "hook_re.h"

#include <stdint.h>

#define RE_PROG 32
#define RE_CLASSES 4
#define RE_RANGES 12
#define RE_NOPOS 0xFFFFu
#define RE_ANCHORED 1u /* only a match that starts where the search starts */
#define RE_NOTNULL 2u  /* no empty match */
#define RE_DEFAULT_BUDGET 1000000ul

enum { I_CHAR, I_ANY, I_CLASS, I_SPLIT, I_JMP, I_SAVE, I_BOL, I_EOL, I_MATCH };
enum { SH_DIGIT = 1, SH_NDIGIT = 2, SH_WORD = 4, SH_NWORD = 8, SH_SPACE = 16, SH_NSPACE = 32 };

typedef struct {
    uint8_t op, k; /* k: I_CLASS its class, I_SAVE its slot (2, 3: the wanted group) */
    int8_t x, y;   /* I_JMP, I_SPLIT: targets relative to this instruction (x preferred) */
    uint32_t c;    /* I_CHAR: the unit */
} Ins;

typedef struct {
    uint8_t neg, n, sh;
    uint16_t lo[RE_RANGES], hi[RE_RANGES];
} Cls;

typedef struct {
    Ins prog[RE_PROG];
    Cls cls[RE_CLASSES];
    const unsigned char *p; /* the pattern, as parsed so far */
    unsigned np, nc, bytes, want, groups, err;
} Re;

typedef struct {
    uint8_t pc;
    uint16_t cap[4]; /* the match's start (cap[1] unused), the wanted group's start and end */
} Th;

typedef struct {
    Th t[RE_PROG];
    unsigned n;
} List;

/* ---- units ---- */

/* One unit of s[i..n): a byte, or a UTF-8 code point (a malformed byte: one unit
 * that only '.' and negated classes match). Returns its length. */
static unsigned unit_at(const unsigned char *s, size_t n, size_t i, unsigned bytes, uint32_t *u)
{
    unsigned b = s[i], len, k;
    uint32_t v;
    if (bytes || b < 0x80) {
        *u = b;
        return 1;
    }
    if (b >= 0xC2 && b <= 0xDF) len = 2, v = b & 0x1F;
    else if (b >= 0xE0 && b <= 0xEF) len = 3, v = b & 0x0F;
    else if (b >= 0xF0 && b <= 0xF4) len = 4, v = b & 0x07;
    else len = 0, v = 0;
    for (k = 1; len && k < len; k++) {
        if (i + k >= n || (s[i + k] & 0xC0) != 0x80) len = 0;
        else v = v << 6 | (s[i + k] & 0x3F);
    }
    if (!len) {
        *u = 0x110000u | b;
        return 1;
    }
    *u = v;
    return len;
}

static int is_digit(uint32_t u) { return u >= '0' && u <= '9'; }
static int is_word(uint32_t u)
{
    return is_digit(u) || (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || u == '_';
}
/* std::regex<char>'s \s in the "C" locale (isspace), as Luna's patterns see it */
static int is_space(uint32_t u) { return u == ' ' || (u >= 9 && u <= 13); }

static int cls_has(const Cls *k, uint32_t u)
{
    int in = ((k->sh & SH_DIGIT) && is_digit(u)) || ((k->sh & SH_NDIGIT) && !is_digit(u)) ||
             ((k->sh & SH_WORD) && is_word(u)) || ((k->sh & SH_NWORD) && !is_word(u)) ||
             ((k->sh & SH_SPACE) && is_space(u)) || ((k->sh & SH_NSPACE) && !is_space(u));
    for (unsigned i = 0; !in && i < k->n; i++)
        if (u >= k->lo[i] && u <= k->hi[i]) in = 1;
    return in != k->neg;
}

static int ins_takes(const Re *r, const Ins *in, uint32_t u)
{
    switch (in->op) {
    case I_CHAR:
        return u == in->c;
    case I_ANY: /* ECMAScript: anything but a line terminator */
        return u != '\n' && u != '\r' && (r->bytes || (u != 0x2028 && u != 0x2029));
    case I_CLASS:
        return cls_has(&r->cls[in->k], u);
    default:
        return 0;
    }
}

/* ---- the compiler ---- */

static unsigned emit(Re *r, unsigned op, uint32_t c)
{
    if (r->np >= RE_PROG) {
        r->err = 1;
        return 0;
    }
    r->prog[r->np].op = (uint8_t)op;
    r->prog[r->np].k = 0;
    r->prog[r->np].x = 1;
    r->prog[r->np].y = 1;
    r->prog[r->np].c = c;
    return r->np++;
}

/* An instruction before the one at `at` (the start of what was just parsed): the
 * jumps are relative and never cross `at`, so nothing needs fixing. */
static void insert(Re *r, unsigned at, unsigned op)
{
    if (r->np >= RE_PROG) {
        r->err = 1;
        return;
    }
    for (unsigned i = r->np; i > at; i--) r->prog[i] = r->prog[i - 1];
    r->np++;
    r->prog[at].op = (uint8_t)op;
    r->prog[at].k = 0;
    r->prog[at].x = 1;
    r->prog[at].y = 1;
    r->prog[at].c = 0;
}

/* The next unit of the pattern (a byte, or a code point), 0 at its end. */
static uint32_t pat_unit(Re *r)
{
    uint32_t u;
    size_t len;
    if (!*r->p) return 0;
    len = unit_at(r->p, (size_t)-1, 0, r->bytes, &u);
    if (u >= 0x110000u) r->err = 1;
    r->p += len;
    return u;
}

/* An escape after '\' (the cursor on the escaped character): a shorthand class's
 * bit in *sh (and 0 returned), or the character. */
static uint32_t escape(Re *r, unsigned *sh)
{
    unsigned c = *r->p;
    *sh = 0;
    switch (c) {
    case 'd': *sh = SH_DIGIT; break;
    case 'D': *sh = SH_NDIGIT; break;
    case 'w': *sh = SH_WORD; break;
    case 'W': *sh = SH_NWORD; break;
    case 's': *sh = SH_SPACE; break;
    case 'S': *sh = SH_NSPACE; break;
    case 'n': r->p++; return '\n';
    case 't': r->p++; return '\t';
    case 'r': r->p++; return '\r';
    case 'f': r->p++; return '\f';
    case 'v': r->p++; return '\v';
    default:
        /* an identity escape of punctuation; \c without its letter (Luna's FPCSG00401),
         * \b, \x, \u, back-references and other letters are outside the subset */
        if (c > ' ' && c < 0x7F && !is_word(c)) {
            r->p++;
            return c;
        }
        r->err = 1;
        return 0;
    }
    r->p++;
    return 0;
}

static unsigned new_class(Re *r)
{
    if (r->nc >= RE_CLASSES) {
        r->err = 1;
        return 0;
    }
    r->cls[r->nc].neg = 0;
    r->cls[r->nc].n = 0;
    r->cls[r->nc].sh = 0;
    return r->nc++;
}

static void class_add(Re *r, Cls *k, uint32_t lo, uint32_t hi)
{
    if (lo > hi || hi > 0xFFFF) {
        r->err = 1;
        return;
    }
    for (unsigned i = 0; i < k->n; i++)
        if (k->lo[i] == lo && k->hi[i] == hi) return;
    if (k->n >= RE_RANGES) {
        r->err = 1;
        return;
    }
    k->lo[k->n] = (uint16_t)lo;
    k->hi[k->n] = (uint16_t)hi;
    k->n++;
}

/* [...] after its '[' */
static void parse_class(Re *r)
{
    unsigned ci = new_class(r);
    Cls *k = &r->cls[ci];
    if (r->err) return;
    if (*r->p == '^') {
        k->neg = 1;
        r->p++;
    }
    for (;;) {
        uint32_t lo;
        unsigned sh = 0;
        if (!*r->p || r->err) {
            r->err = 1;
            return;
        }
        if (*r->p == ']') {
            r->p++;
            break;
        }
        if (*r->p == '\\') {
            r->p++;
            lo = escape(r, &sh);
        } else {
            lo = pat_unit(r);
        }
        if (sh) {
            k->sh |= (uint8_t)sh;
            continue;
        }
        if (r->p[0] == '-' && r->p[1] && r->p[1] != ']') { /* a range */
            uint32_t hi;
            r->p++;
            if (*r->p == '\\') {
                r->p++;
                hi = escape(r, &sh);
                if (sh) r->err = 1;
            } else {
                hi = pat_unit(r);
            }
            class_add(r, k, lo, hi);
        } else {
            class_add(r, k, lo, lo);
        }
    }
    emit(r, I_CLASS, 0);
    if (!r->err) r->prog[r->np - 1].k = (uint8_t)ci;
}

static void parse_alt(Re *r);

/* One atom; 0 when it cannot be repeated (an anchor) */
static int parse_atom(Re *r)
{
    unsigned c = *r->p, sh;
    uint32_t u;
    switch (c) {
    case '(': {
        unsigned g;
        r->p++;
        if (*r->p == '?') { /* (?: (?= (?! are outside the subset */
            r->err = 1;
            return 1;
        }
        g = ++r->groups;
        if (g == r->want) {
            emit(r, I_SAVE, 0);
            r->prog[r->np - 1].k = 2;
        }
        parse_alt(r);
        if (*r->p != ')') {
            r->err = 1;
            return 1;
        }
        r->p++;
        if (g == r->want) {
            emit(r, I_SAVE, 0);
            r->prog[r->np - 1].k = 3;
        }
        return 1;
    }
    case '[':
        r->p++;
        parse_class(r);
        return 1;
    case '.':
        r->p++;
        emit(r, I_ANY, 0);
        return 1;
    case '^':
        r->p++;
        emit(r, I_BOL, 0);
        return 0;
    case '$':
        r->p++;
        emit(r, I_EOL, 0);
        return 0;
    case '\\':
        r->p++;
        u = escape(r, &sh);
        if (sh) { /* \d \w \s ...: one class each, shared by its repeats */
            unsigned ci = 0;
            while (ci < r->nc && (r->cls[ci].n || r->cls[ci].neg || r->cls[ci].sh != sh)) ci++;
            if (ci == r->nc) {
                ci = new_class(r);
                if (!r->err) r->cls[ci].sh = (uint8_t)sh;
            }
            emit(r, I_CLASS, 0);
            if (!r->err) r->prog[r->np - 1].k = (uint8_t)ci;
        } else {
            emit(r, I_CHAR, u);
        }
        return 1;
    case '*':
    case '+':
    case '?':
    case '{':
    case '}':
    case ')':
        r->err = 1; /* nothing to repeat, {n,m}, or a stray ')' */
        return 1;
    default:
        emit(r, I_CHAR, pat_unit(r));
        return 1;
    }
}

static void parse_rep(Re *r)
{
    unsigned start = r->np, q, lazy = 0, len;
    int can = parse_atom(r);
    q = *r->p;
    if (q != '*' && q != '+' && q != '?') return;
    r->p++;
    if (*r->p == '?') {
        lazy = 1;
        r->p++;
    }
    if (!can || *r->p == '*' || *r->p == '+' || *r->p == '?' || *r->p == '{') {
        r->err = 1;
        return;
    }
    len = r->np - start;
    if (q == '*') { /* L: split body, out; body; jmp L */
        unsigned j;
        insert(r, start, I_SPLIT);
        j = emit(r, I_JMP, 0);
        if (r->err) return;
        r->prog[j].x = (int8_t)((int)start - (int)j);
        r->prog[start].x = (int8_t)(lazy ? len + 2 : 1);
        r->prog[start].y = (int8_t)(lazy ? 1 : len + 2);
    } else if (q == '+') { /* L: body; split L, out */
        unsigned s = emit(r, I_SPLIT, 0);
        if (r->err) return;
        r->prog[s].x = (int8_t)(lazy ? 1 : (int)start - (int)s);
        r->prog[s].y = (int8_t)(lazy ? (int)start - (int)s : 1);
    } else { /* split body, out; body */
        insert(r, start, I_SPLIT);
        if (r->err) return;
        r->prog[start].x = (int8_t)(lazy ? len + 1 : 1);
        r->prog[start].y = (int8_t)(lazy ? 1 : len + 1);
    }
}

static void parse_alt(Re *r)
{
    unsigned start = r->np;
    while (*r->p && *r->p != '|' && *r->p != ')' && !r->err) parse_rep(r);
    if (*r->p == '|' && !r->err) {
        unsigned j;
        r->p++;
        insert(r, start, I_SPLIT); /* split first, second; first; jmp out; second */
        j = emit(r, I_JMP, 0);
        if (r->err) return;
        r->prog[start].y = (int8_t)(r->np - start);
        parse_alt(r);
        if (!r->err) r->prog[j].x = (int8_t)(r->np - j);
    }
}

static int compile(Re *r, const char *pat, unsigned flags, unsigned want)
{
    r->p = (const unsigned char *)pat;
    r->np = r->nc = r->groups = r->err = 0;
    r->bytes = flags & VJO_RE_BYTES;
    r->want = want;
    if (!pat) return -1;
    parse_alt(r);
    if (*r->p) r->err = 1; /* a ')' without its '(' */
    emit(r, I_MATCH, 0);
    if (want > r->groups) r->err = 1;
    return r->err ? -1 : 0;
}

/* ---- the VM ---- */

/* Adds thread t at position p (gen p + 1 marks what list l already holds),
 * following jumps, splits (the preferred branch first), saves and anchors. */
static int add(const Re *r, List *l, Th *stack, uint16_t *mark, Th t, size_t p, size_t n,
               unsigned long *budget)
{
    unsigned sp = 0;
    uint16_t gen = (uint16_t)(p + 1);
    stack[sp++] = t;
    while (sp) {
        Th c = stack[--sp];
        for (;;) {
            const Ins *in = &r->prog[c.pc];
            if (!*budget) return -1;
            --*budget;
            if (mark[c.pc] == gen) break;
            mark[c.pc] = gen;
            if (in->op == I_JMP) {
                c.pc = (uint8_t)(c.pc + in->x);
            } else if (in->op == I_SPLIT) {
                if (sp < RE_PROG) {
                    stack[sp] = c;
                    stack[sp].pc = (uint8_t)(c.pc + in->y);
                    sp++;
                }
                c.pc = (uint8_t)(c.pc + in->x);
            } else if (in->op == I_SAVE) {
                c.cap[in->k] = (uint16_t)p;
                c.pc++;
            } else if (in->op == I_BOL) {
                if (p) break;
                c.pc++;
            } else if (in->op == I_EOL) {
                if (p != n) break;
                c.pc++;
            } else {
                if (l->n < RE_PROG) l->t[l->n++] = c;
                break;
            }
        }
    }
    return 0;
}

/* The highest-priority match starting at or after `start` (at it with
 * RE_ANCHORED): 1 and best[0..3] (match start, end, group start, end), 0 none,
 * VJO_RE_EBUDGET. */
static int run(const Re *r, const unsigned char *s, size_t n, size_t start, unsigned opts, uint16_t best[4],
               unsigned long *budget)
{
    List a, b, *cl = &a, *nl = &b;
    Th stack[RE_PROG];
    uint16_t mark[RE_PROG];
    size_t p = start;
    int matched = 0;
    for (unsigned i = 0; i < r->np; i++) mark[i] = 0;
    a.n = b.n = 0;
    for (;;) {
        uint32_t u = 0;
        unsigned ul = 1;
        if (p < n) ul = unit_at(s, n, p, r->bytes, &u);
        if (!matched && (p == start || !(opts & RE_ANCHORED))) {
            Th t;
            t.pc = 0;
            t.cap[0] = (uint16_t)p;
            t.cap[1] = t.cap[2] = t.cap[3] = RE_NOPOS;
            if (add(r, cl, stack, mark, t, p, n, budget)) return VJO_RE_EBUDGET;
        }
        if (!cl->n) {
            if (matched || (opts & RE_ANCHORED) || p >= n) break;
            p += ul;
            continue;
        }
        nl->n = 0;
        for (unsigned i = 0; i < cl->n; i++) {
            const Th *t = &cl->t[i];
            const Ins *in = &r->prog[t->pc];
            if (!*budget) return VJO_RE_EBUDGET;
            --*budget;
            if (in->op == I_MATCH) {
                if ((opts & RE_NOTNULL) && t->cap[0] == p) continue;
                matched = 1;
                best[0] = t->cap[0];
                best[1] = (uint16_t)p;
                best[2] = t->cap[2];
                best[3] = t->cap[3];
                break; /* the threads after this one have lower priority */
            }
            if (p < n && ins_takes(r, in, u)) {
                Th nt = *t;
                nt.pc++;
                if (add(r, nl, stack, mark, nt, p + ul, n, budget)) return VJO_RE_EBUDGET;
            }
        }
        {
            List *x = cl;
            cl = nl;
            nl = x;
        }
        if (p >= n) break; /* nothing takes a unit at the end */
        p += ul;
    }
    return matched;
}

/* ---- the output ---- */

/* Appends len bytes, or the whole UTF-8 characters of them that fit: 0 when cut. */
static int put(char *out, size_t cap, size_t *w, const char *src, size_t len)
{
    size_t room = cap - 1 - *w, k;
    if (len <= room) {
        for (k = 0; k < len; k++) out[(*w)++] = src[k];
        return 1;
    }
    k = room; /* back off a character cut by room */
    while (k && ((unsigned char)src[k] & 0xC0) == 0x80) k--;
    for (size_t i = 0; i < k; i++) out[(*w)++] = src[i];
    return 0;
}

/* The group a replacement asks for ($1-$9), 0 for none, -1 for two. */
static int rep_group(const char *rep)
{
    int g = 0;
    for (; rep && *rep; rep++) {
        if (rep[0] != '$') continue;
        if (rep[1] == '$' || rep[1] == '&') {
            rep++;
        } else if (rep[1] >= '1' && rep[1] <= '9') {
            if (g && g != rep[1] - '0') return -1;
            g = rep[1] - '0';
            rep++;
        }
    }
    return g;
}

int vjo_re_sub(const char *pat, unsigned flags, const char *in, size_t n, const char *rep, char *out,
               size_t cap, unsigned long *budget)
{
    Re r;
    unsigned long own = RE_DEFAULT_BUDGET;
    const unsigned char *s = (const unsigned char *)in;
    size_t pos = 0, w = 0;
    int empty = 0, g = rep_group(rep), ok = 1;
    if (!out || !cap) return VJO_RE_EPATTERN;
    out[0] = 0;
    if (!budget) budget = &own;
    if (!in) in = "", n = 0;
    s = (const unsigned char *)in;
    if (g < 0 || n >= RE_NOPOS || compile(&r, pat, flags, (unsigned)g)) return VJO_RE_EPATTERN;
    for (;;) {
        uint16_t m[4];
        int found;
        if (empty) {
            uint32_t u;
            if (pos >= n) break;
            found = run(&r, s, n, pos, RE_ANCHORED | RE_NOTNULL, m, budget);
            if (!found) found = run(&r, s, n, pos + unit_at(s, n, pos, r.bytes, &u), 0, m, budget);
        } else {
            found = run(&r, s, n, pos, 0, m, budget);
        }
        if (found < 0) {
            out[0] = 0;
            return found;
        }
        if (!found) break;
        ok = put(out, cap, &w, in + pos, m[0] - pos);
        for (const char *q = rep; ok && q && *q;) {
            if (q[0] == '$' && (q[1] == '$' || q[1] == '&' || (q[1] >= '1' && q[1] <= '9'))) {
                if (q[1] == '$') ok = put(out, cap, &w, "$", 1);
                else if (q[1] == '&') ok = put(out, cap, &w, in + m[0], (size_t)(m[1] - m[0]));
                else if (m[2] != RE_NOPOS && m[3] != RE_NOPOS) ok = put(out, cap, &w, in + m[2], (size_t)(m[3] - m[2]));
                q += 2;
            } else { /* the literal run up to the next $, whole: put cuts it at a character */
                const char *e = q + 1;
                while (*e && *e != '$') e++;
                ok = put(out, cap, &w, q, (size_t)(e - q));
                q = e;
            }
        }
        if (!ok) break;
        empty = m[1] == m[0];
        pos = m[1];
    }
    if (ok) put(out, cap, &w, in + pos, n - pos);
    out[w] = 0;
    return (int)w;
}

int vjo_re_search(const char *pat, unsigned flags, const char *in, size_t n, unsigned g, long m[4],
                  unsigned long *budget)
{
    Re r;
    uint16_t b[4];
    unsigned long own = RE_DEFAULT_BUDGET;
    int found;
    m[0] = m[1] = m[2] = m[3] = -1;
    if (!budget) budget = &own;
    if (!in) in = "", n = 0;
    if (g > 9 || n >= RE_NOPOS || compile(&r, pat, flags, g)) return VJO_RE_EPATTERN;
    found = run(&r, (const unsigned char *)in, n, 0, 0, b, budget);
    if (found <= 0) return found;
    m[0] = b[0];
    m[1] = b[1];
    if (!g) {
        m[2] = b[0];
        m[3] = b[1];
    } else if (b[2] != RE_NOPOS && b[3] != RE_NOPOS) {
        m[2] = b[2];
        m[3] = b[3];
    }
    return 1;
}
