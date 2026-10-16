#include "taicfg.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <taihen/parser.h> /* third_party/taihen-parser (6d817fd), as taiHEN 309b380 builds it */

/* ---------------- lines, cut as taiHEN's lexer cuts them ---------------- */

/* The six bytes the lexer's isspace takes (taihen-parser lexer.c:24-35, NO_CTYPE as taiHEN builds it). */
static int space(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; }

enum { L_COMMENT, L_SECTION, L_PATH };
typedef struct {
    size_t start, raw, next; /* the line's first byte, its length up to its first CR or LF, the next line */
    size_t b, e;             /* its content without the surrounding spaces (a comment's: from its first byte) */
    size_t nb, ne;           /* a section's name */
    int kind, halt;          /* L_*, and a section's '!' */
} Line;

/* The line at pos (< len): a line ends at CR or LF; CRLF is one line end here (taiHEN reads its LF
 * as an empty line, a comment). */
static void cut(const char *t, size_t len, size_t pos, Line *l)
{
    size_t i = pos, b, e, n;
    while (i < len && t[i] != '\r' && t[i] != '\n') i++;
    l->start = pos;
    l->raw = i - pos;
    if (i < len) i += t[i] == '\r' && i + 1 < len && t[i + 1] == '\n' ? 2 : 1;
    l->next = i;
    l->halt = 0;
    l->nb = l->ne = 0;
    b = pos;
    e = pos + l->raw;
    while (b < e && space((unsigned char)t[b])) b++;
    l->b = b;
    l->e = e;
    if (b == e || t[b] == '#') { l->kind = L_COMMENT; return; }
    while (e > b && space((unsigned char)t[e - 1])) e--;
    l->e = e;
    if (t[b] != '*') { l->kind = L_PATH; return; }
    l->kind = L_SECTION;
    for (n = b + 1; n < e && space((unsigned char)t[n]); n++) {}
    if (n < e && t[n] == '!') {
        l->halt = 1;
        for (n++; n < e && space((unsigned char)t[n]); n++) {}
    }
    l->nb = n;
    l->ne = e;
}

static int name_is(const char *t, const Line *l, const char *name)
{
    size_t n = strlen(name);
    return l->kind == L_SECTION && l->ne - l->nb == n && !memcmp(t + l->nb, name, n);
}

/* ---------------- the overlay's lines ---------------- */

enum { O_NONE = 0, O_KERNEL, O_SHELL, O_WORKER, O_TEXT };

/* Which of ours a plugin path is: its file name (after the last '/' or ':'), in any case. */
static int ours(const char *s, size_t n)
{
    static const char *const names[] = {NULL, "vitajpoverlay_kernel.skprx", "vitajpoverlay_shell.suprx",
                                        "vitajpoverlay_ocr.suprx", "vitajpoverlay_text.suprx"};
    size_t i = n;
    while (i && s[i - 1] != '/' && s[i - 1] != ':') i--;
    for (int k = O_KERNEL; k <= O_TEXT; k++) {
        size_t m = strlen(names[k]), j = 0;
        if (n - i != m) continue;
        while (j < m && lower((unsigned char)s[i + j]) == names[k][j]) j++;
        if (j == m) return k;
    }
    return O_NONE;
}

static int line_ours(const char *t, const Line *l) { return l->kind == L_PATH ? ours(t + l->b, l->e - l->b) : O_NONE; }

static int canonical(const char *t, const Line *l, const char *path)
{
    size_t n = strlen(path);
    return l->e - l->b == n && !memcmp(t + l->b, path, n);
}

static int is_note(const char *t, const Line *l)
{
    if (l->kind != L_COMMENT) return 0;
    size_t e = l->e;
    while (e > l->b && space((unsigned char)t[e - 1])) e--;
    return (e - l->b == strlen(VJO_TAI_NOTE) && !memcmp(t + l->b, VJO_TAI_NOTE, e - l->b)) ||
           (e - l->b == strlen(VJO_TAI_APP_NOTE) && !memcmp(t + l->b, VJO_TAI_APP_NOTE, e - l->b));
}

static int blank(const Line *l) { return l->kind == L_COMMENT && l->b == l->e; }

int vjo_tai_title_ok(const char *id)
{
    if (!id) return 0;
    for (int i = 0; i < 9; i++)
        if (!((id[i] >= 'A' && id[i] <= 'Z') || (id[i] >= '0' && id[i] <= '9'))) return 0;
    return !id[9];
}

/* A section lines mode registers the worker in: a title ID of a game, as tools/patch_tai_config.py
 * is_game (core/games.c vjo_game_is_candidate in the nine-character form). */
static int is_game(const char *s, size_t n)
{
    if (n != 9) return 0;
    for (size_t i = 0; i < 9; i++)
        if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9'))) return 0;
    if (!memcmp(s, "VITASHELL", 9) || !memcmp(s, "VJOSET001", 9)) return 0;
    return !memcmp(s, "NPXS10028", 9) || memcmp(s, "NPXS", 4);
}

/* ---------------- taiHEN's reading of one title, as the edit walks the lines ---------------- */

typedef struct {
    const char *title;
    size_t tn;
    int kernel, record, halt, stopped;
} Sim;

static void sim_init(Sim *s, const char *title)
{
    memset(s, 0, sizeof(*s));
    s->title = title;
    s->tn = strlen(title);
    s->kernel = !strcmp(title, "KERNEL");
}

/* parser.c:237-264: at a header, a matched halting section stops the reading; then the halt and
 * the match are this header's. */
static void sim_section(Sim *s, const char *t, const Line *l)
{
    size_t n = l->ne - l->nb;
    if (s->stopped) return;
    if (s->record && s->halt) { s->stopped = 1; return; }
    s->halt = l->halt;
    s->record = (!s->kernel && n == 3 && !memcmp(t + l->nb, "ALL", 3)) ||
                (n == s->tn && !memcmp(t + l->nb, s->title, n));
}

static int sim_loads(const Sim *s) { return s->record && !s->stopped; }

/* ---------------- the edit ---------------- */

#define GAMES_MAX 512
typedef struct {
    char id[GAMES_MAX][10];
    unsigned char has_header[GAMES_MAX]; /* pass 1: the text has a section of the game */
    unsigned char has_line[GAMES_MAX];   /* pass 1: a canonical worker line in one of its sections */
    unsigned char kept[GAMES_MAX];       /* pass 2: its first such line is kept (or one was added) */
    unsigned char first_done[GAMES_MAX]; /* pass 2: its first section's header is written */
    int n;
} Games;

static int games_find(const Games *g, const char *s, size_t n)
{
    for (int i = 0; i < g->n; i++)
        if (n == 9 && !memcmp(g->id[i], s, 9)) return i;
    return -1;
}

static int games_add(Games *g, const char *s, size_t n)
{
    int i = games_find(g, s, n);
    if (i >= 0) return i;
    if (g->n >= GAMES_MAX) return -1;
    memcpy(g->id[g->n], s, 9);
    g->id[g->n][9] = '\0';
    g->has_header[g->n] = g->has_line[g->n] = g->kept[g->n] = g->first_done[g->n] = 0;
    return g->n++;
}

typedef struct {
    char *out;
    size_t cap, used;
    int full;
} Out;

static void put(Out *o, const char *s, size_t n)
{
    if (o->full || n > o->cap - o->used || o->used + n > VJO_TAI_TEXT_MAX) { o->full = 1; return; }
    memcpy(o->out + o->used, s, n);
    o->used += n;
}

static void puts_(Out *o, const char *s) { put(o, s, strlen(s)); }

/* The note of what a refusal is about. */
static int refuse(VjoTaiReport *r, int rc, int line, const char *why)
{
    r->line = line;
    snprintf(r->why, sizeof(r->why), "%s", why);
    return rc;
}

static void note_section(VjoTaiReport *r, const char *t, const Line *section)
{
    size_t used = strlen(r->removed_sections), n;
    char name[40];
    if (!section) {
        snprintf(name, sizeof(name), "(no section)");
    } else {
        n = section->ne - section->nb;
        snprintf(name, sizeof(name), "*%.*s", (int)(n > 30 ? 30 : n), t + section->nb);
    }
    if (strstr(r->removed_sections, name)) return;
    if (used + strlen(name) + 2 >= sizeof(r->removed_sections) - 4) {
        if (!strstr(r->removed_sections, "...")) snprintf(r->removed_sections + used, sizeof(r->removed_sections) - used, ", ...");
        return;
    }
    snprintf(r->removed_sections + used, sizeof(r->removed_sections) - used, "%s%s", used ? ", " : "", name);
}

typedef struct {
    const char *t;
    size_t len;
    int op, mode;
    const char *nl;          /* the line end of added lines */
    Games *games;
    /* pass 1 */
    long kernel_keep, shell_keep;     /* the kept canonical line's start, -1 */
    long kernel_header, main_header;  /* the first *KERNEL / *main header's start, -1 */
    int keep_marked_headers;          /* uninstall's second attempt: an emptied, marked KERNEL or main stays */
} Edit;

/* What this edit does with a line of ours (or not ours: 0 keep). */
enum { KEEP = 0, COMMENT, REMOVE };
static int fate(const Edit *x, const char *t, const Line *l, const Line *section, int *kept_game)
{
    int o = line_ours(t, l);
    *kept_game = -1;
    if (!o) return KEEP;
    if (x->op == VJO_TAI_UNINSTALL) return REMOVE;
    if (o == O_KERNEL) return (long)l->start == x->kernel_keep ? KEEP : COMMENT;
    if (o == O_SHELL) return (long)l->start == x->shell_keep ? KEEP : COMMENT;
    if (x->mode == VJO_TAI_WORKER_KERNEL) return REMOVE; /* the worker and the removed Text plugin */
    if (o == O_TEXT) return COMMENT;
    /* lines mode: the first canonical worker line in a section of one of the games */
    if (section && canonical(t, l, VJO_TAI_WORKER) && is_game(t + section->nb, section->ne - section->nb)) {
        int g = games_find(x->games, t + section->nb, section->ne - section->nb);
        if (g >= 0 && !x->games->kept[g]) { *kept_game = g; return KEEP; }
    }
    return COMMENT;
}

/* After this edit, is the section whose header is h left with nothing but blank lines and notes
 * (which belong to the next header)? Also says whether it held a worker line of ours. */
static int emptied(const Edit *x, const Line *h, int *held_worker)
{
    Line l;
    int kept;
    *held_worker = 0;
    for (size_t pos = h->next; pos < x->len; pos = l.next) {
        cut(x->t, x->len, pos, &l);
        if (l.kind == L_SECTION) break;
        if (l.kind == L_COMMENT) {
            if (!blank(&l) && !is_note(x->t, &l)) return 0;
            continue;
        }
        int o = line_ours(x->t, &l);
        if (o == O_WORKER || o == O_TEXT) *held_worker = 1;
        if (fate(x, x->t, &l, h, &kept) != REMOVE) return 0;
    }
    return 1;
}

/* A header this edit removes: emptied by it and not *!; then, for an install (kernel mode), a title's
 * section that held our worker or Text plugin, whatever comes before it (a note left by an earlier
 * edit must not decide it, or a second install would remove more than the first); for an uninstall,
 * that too, or any header marked by a note right before it (a *KERNEL, *main or *ALL only so: an
 * unmarked one stays, and the second attempt keeps every one). */
static int removable(const Edit *x, const Line *h, int marked)
{
    int held;
    if (h->kind != L_SECTION || h->halt) return 0;
    if (x->op == VJO_TAI_INSTALL && x->mode == VJO_TAI_WORKER_LINES) return 0;
    if (!emptied(x, h, &held)) return 0;
    int reserved = name_is(x->t, h, "KERNEL") || name_is(x->t, h, "main") || name_is(x->t, h, "ALL");
    if (x->op == VJO_TAI_INSTALL) return held && !reserved;
    if (reserved) return marked && !x->keep_marked_headers;
    return held || marked;
}

/* A note the edit drops wherever it is: every note in an uninstall; #124's, which names the worker, in
 * a kernel-mode install. (A note right before a header the edit removes goes with it, emit.) */
static int note_goes(const Edit *x, const char *t, const Line *l);

/* Pass 1: which kernel and Shell lines are kept, where the first *KERNEL and *main are, and lines
 * mode's games. */
static int analyse(Edit *x, VjoTaiReport *r)
{
    Sim k, m;
    Line l, section;
    int have_section = 0, lineno = 0, content = 0;
    memset(&section, 0, sizeof(section));
    sim_init(&k, "KERNEL");
    sim_init(&m, "main");
    x->kernel_keep = x->shell_keep = x->kernel_header = x->main_header = -1;
    for (size_t pos = 0; pos < x->len; pos = l.next) {
        cut(x->t, x->len, pos, &l);
        lineno++;
        if (l.raw >= VJO_TAI_LINE_MAX)
            return refuse(r, VJO_TAI_E_LONG_LINE, lineno, "A line is 256 bytes or longer: taiHEN stops reading the file "
                          "there, so nothing after it loads. Shorten or remove it in VitaShell first.");
        if (l.kind == L_COMMENT) continue;
        content = 1;
        if (l.kind == L_SECTION) {
            sim_section(&k, x->t, &l);
            sim_section(&m, x->t, &l);
            if (x->kernel_header < 0 && name_is(x->t, &l, "KERNEL")) x->kernel_header = (long)l.start;
            if (x->main_header < 0 && name_is(x->t, &l, "main")) x->main_header = (long)l.start;
            if (is_game(x->t + l.nb, l.ne - l.nb)) {
                int g = games_find(x->games, x->t + l.nb, l.ne - l.nb);
                if (g >= 0) x->games->has_header[g] = 1;
            }
            section = l;
            have_section = 1;
            continue;
        }
        int o = line_ours(x->t, &l);
        if (o == O_KERNEL && x->kernel_keep < 0 && canonical(x->t, &l, VJO_TAI_KERNEL) && sim_loads(&k))
            x->kernel_keep = (long)l.start;
        /* the Shell line is kept only in a section named main, as the kernel line is: *ALL also matches
         * main, but a Shell line there is another section's, so it is commented out like any misplaced one */
        if (o == O_SHELL && x->shell_keep < 0 && canonical(x->t, &l, VJO_TAI_SHELL) && sim_loads(&m) && have_section &&
            name_is(x->t, &section, "main"))
            x->shell_keep = (long)l.start;
        /* lines mode: a game registered before (a canonical worker line in its section) is kept */
        if (o == O_WORKER && x->mode == VJO_TAI_WORKER_LINES && x->op == VJO_TAI_INSTALL && have_section &&
            canonical(x->t, &l, VJO_TAI_WORKER) && is_game(x->t + section.nb, section.ne - section.nb)) {
            int g = games_add(x->games, x->t + section.nb, section.ne - section.nb);
            if (g < 0) return refuse(r, VJO_TAI_E_SPACE, lineno, "More than 512 games have the OCR worker.");
            x->games->has_header[g] = x->games->has_line[g] = 1;
        }
    }
    if (!x->len) return refuse(r, VJO_TAI_E_EMPTY, 0, "It is empty: taiHEN may not start with it. Restore a backup "
                               "(config.txt.vjo-*.bak) or HENkaku's default first.");
    if (!content) return refuse(r, VJO_TAI_E_NO_SECTION, 0, "It has no section (*KERNEL, *main) and no plugin: taiHEN "
                                "loads nothing from it. Restore a backup (config.txt.vjo-*.bak) or HENkaku's default first.");
    return 0;
}

/* The line end of added lines: the file's (CRLF when it has one, else LF, else CR when it has only CR). */
static const char *line_end(const char *t, size_t len)
{
    int cr = 0;
    for (size_t i = 0; i < len; i++) {
        if (t[i] == '\r' && i + 1 < len && t[i + 1] == '\n') return "\r\n";
        if (t[i] == '\r') cr = 1;
        if (t[i] == '\n') return "\n";
    }
    return cr ? "\r" : "\n";
}

/* Before a line that follows the text's last one, which had no line end: the file's line end. A
 * file that ends without one keeps it that way when nothing follows (an unchanged file stays). */
static void line_start(Out *o, const char *nl)
{
    if (o->used && o->out[o->used - 1] != '\n' && o->out[o->used - 1] != '\r') puts_(o, nl);
}

/* A line of the text as it is, with its own line end (none for a last line without one). */
static void put_line(Out *o, const char *t, const Line *l, const char *nl)
{
    line_start(o, nl);
    put(o, t + l->start, l->next - l->start);
}

/* A line the editor writes, with the file's line end. */
static void put_new(Out *o, const char *s, const char *nl)
{
    line_start(o, nl);
    puts_(o, s);
    puts_(o, nl);
}

static int is_app_note(const char *t, const Line *l)
{
    size_t e = l->e, n = strlen(VJO_TAI_APP_NOTE);
    while (e > l->b && space((unsigned char)t[e - 1])) e--;
    return l->kind == L_COMMENT && e - l->b == n && !memcmp(t + l->b, VJO_TAI_APP_NOTE, n);
}

static int note_goes(const Edit *x, const char *t, const Line *l)
{
    return is_note(t, l) && (x->op == VJO_TAI_UNINSTALL || (x->mode == VJO_TAI_WORKER_KERNEL && is_app_note(t, l)));
}

/* Pass 2: the result. */
static void emit(const Edit *x, Out *o, VjoTaiReport *r)
{
    const char *t = x->t;
    Line l, section, held;
    int have_section = 0, holding = 0, in_removed = 0, kept;
    memset(&section, 0, sizeof(section));
    memset(&held, 0, sizeof(held));
    for (size_t pos = 0; pos < x->len; pos = l.next) {
        cut(t, x->len, pos, &l);
        if (l.kind == L_SECTION) {
            int marked = holding && is_note(t, &held);
            if (removable(x, &l, marked)) {
                /* the header goes, with the blank line or the note right before it */
                if (holding && (blank(&held) || is_note(t, &held))) { if (is_note(t, &held)) r->removed++; holding = 0; }
                r->headers_removed++;
                in_removed = 1;
                section = l;
                have_section = 1;
                continue;
            }
            in_removed = 0;
            if (holding) {
                if (note_goes(x, t, &held)) r->removed++;
                else put_line(o, t, &held, x->nl);
                holding = 0;
            }
            put_line(o, t, &l, x->nl);
            section = l;
            have_section = 1;
            if (x->op != VJO_TAI_INSTALL) continue;
            /* the canonical lines after the first *KERNEL and *main, when none is kept */
            if ((long)l.start == x->kernel_header && x->kernel_keep < 0) { put_new(o, VJO_TAI_KERNEL, x->nl); r->added++; }
            if ((long)l.start == x->main_header && x->shell_keep < 0) { put_new(o, VJO_TAI_SHELL, x->nl); r->added++; }
            if (x->mode == VJO_TAI_WORKER_LINES && is_game(t + l.nb, l.ne - l.nb)) {
                int g = games_find(x->games, t + l.nb, l.ne - l.nb);
                /* after the first header of a game that has no worker line in any of its sections */
                if (g >= 0 && !x->games->first_done[g]) {
                    x->games->first_done[g] = 1;
                    if (!x->games->has_line[g]) {
                        put_new(o, VJO_TAI_WORKER, x->nl);
                        x->games->kept[g] = 1;
                        r->added++;
                    }
                }
            }
            continue;
        }
        if (holding) {
            if (note_goes(x, t, &held)) r->removed++;
            else put_line(o, t, &held, x->nl);
            holding = 0;
        }
        if (l.kind == L_COMMENT) {
            /* blank lines and notes wait: they go with a header that goes */
            if (blank(&l) || is_note(t, &l)) { held = l; holding = 1; continue; }
            put_line(o, t, &l, x->nl);
            continue;
        }
        int f = fate(x, t, &l, have_section ? &section : NULL, &kept);
        if (f == KEEP) {
            if (kept >= 0) x->games->kept[kept] = 1;
            if (!in_removed) put_line(o, t, &l, x->nl);
            continue;
        }
        if (line_ours(t, &l) == O_WORKER || line_ours(t, &l) == O_TEXT) note_section(r, t, have_section ? &section : NULL);
        if (f == COMMENT && strlen(VJO_TAI_DISABLED) + l.raw < VJO_TAI_LINE_MAX) {
            line_start(o, x->nl);
            puts_(o, VJO_TAI_DISABLED);
            put(o, t + l.start, l.next - l.start);
            r->commented++;
            continue;
        }
        r->removed++; /* removed, or a commented line would be too long for taiHEN */
    }
    if (holding) {
        if (note_goes(x, t, &held)) r->removed++;
        else put_line(o, t, &held, x->nl);
    }
    if (x->op != VJO_TAI_INSTALL) return;
    /* sections the file does not have, at its end */
    int need_kernel = x->kernel_header < 0, need_main = x->main_header < 0;
    if (need_kernel) {
        put_new(o, VJO_TAI_NOTE, x->nl); put_new(o, "*KERNEL", x->nl); put_new(o, VJO_TAI_KERNEL, x->nl);
        r->added++; r->sections_added++;
    }
    if (need_main) {
        put_new(o, VJO_TAI_NOTE, x->nl); put_new(o, "*main", x->nl); put_new(o, VJO_TAI_SHELL, x->nl);
        r->added++; r->sections_added++;
    }
    if (x->mode == VJO_TAI_WORKER_LINES)
        for (int g = 0; g < x->games->n; g++) {
            if (x->games->has_header[g]) continue;
            char header[12];
            snprintf(header, sizeof(header), "*%s", x->games->id[g]);
            put_new(o, VJO_TAI_NOTE, x->nl); put_new(o, header, x->nl); put_new(o, VJO_TAI_WORKER, x->nl);
            x->games->kept[g] = 1;
            r->added++; r->sections_added++;
        }
}

/* ---------------- the check: taiHEN's own parser ---------------- */

typedef struct {
    uint64_t hash; /* FNV-1a of every other plugin's path, in order, each with its NUL */
    unsigned others;
    VjoTaiLoads ours;
} Seen;

static void seen_module(const char *module, void *param)
{
    Seen *s = param;
    size_t n = strlen(module);
    switch (ours(module, n)) {
    case O_KERNEL: s->ours.kernel++; return;
    case O_SHELL: s->ours.shell++; return;
    case O_WORKER: s->ours.worker++; return;
    case O_TEXT: s->ours.text++; return;
    default: break;
    }
    for (size_t i = 0; i <= n; i++) {
        s->hash ^= (unsigned char)module[i];
        s->hash *= 1099511628211ull;
    }
    s->others++;
}

static void parse_title(const char *text, const char *title, Seen *s)
{
    memset(s, 0, sizeof(*s));
    s->hash = 14695981039346656037ull;
    taihen_config_parse(text, title, seen_module, s);
}

void vjo_tai_loads(const char *text, const char *title, VjoTaiLoads *out)
{
    Seen s;
    parse_title(text ? text : "", title, &s);
    *out = s.ours;
}

/* Whether name (n bytes) is a section of text (NUL-terminated, len bytes) before the line at upto. */
static int has_section(const char *t, size_t len, const char *name, size_t n, size_t upto)
{
    Line l;
    for (size_t pos = 0; pos < len && pos < upto; pos = l.next) {
        cut(t, len, pos, &l);
        if (l.kind == L_SECTION && l.ne - l.nb == n && !memcmp(t + l.nb, name, n)) return 1;
    }
    return 0;
}

/* One title of the check: the other plugins as before, ours as the edit must leave them. */
static int check_title(const Edit *x, const char *old, const char *neu, const char *title, VjoTaiReport *r)
{
    Seen a, b;
    char why[200];
    parse_title(old, title, &a);
    parse_title(neu, title, &b);
    if (a.hash != b.hash || a.others != b.others) {
        snprintf(why, sizeof(why), "The edit would change what taiHEN loads for *%.40s besides the overlay: nothing "
                 "was changed (please report this).", title);
        return refuse(r, VJO_TAI_E_CHECK, 0, why);
    }
    int is_kernel = !strcmp(title, "KERNEL"), is_main = !strcmp(title, "main");
    int game = x->op == VJO_TAI_INSTALL && x->mode == VJO_TAI_WORKER_LINES && !is_main && !is_kernel &&
               games_find(x->games, title, strlen(title)) >= 0;
    int want_kernel = x->op == VJO_TAI_INSTALL && is_kernel, want_shell = x->op == VJO_TAI_INSTALL && is_main;
    if (b.ours.kernel != want_kernel) {
        snprintf(why, sizeof(why), want_kernel ? "taiHEN would not load the kernel plugin from it exactly once (a *! "
                 "section stops it before *KERNEL?): nothing was changed." : "taiHEN would load the kernel plugin "
                 "for *%.40s: nothing was changed.", title);
        return refuse(r, VJO_TAI_E_CHECK, 0, why);
    }
    if (b.ours.shell != want_shell) {
        snprintf(why, sizeof(why), want_shell ? "taiHEN would not reach the Shell plugin's line (a *! section, *!ALL "
                 "say, stops it before *main): nothing was changed." : "taiHEN would load the Shell plugin for "
                 "*%.40s: nothing was changed.", title);
        return refuse(r, VJO_TAI_E_CHECK, 0, why);
    }
    if (b.ours.worker != game || b.ours.text) {
        snprintf(why, sizeof(why), game ? "taiHEN would not load the OCR worker once for *%.40s (a *! section stops it?): "
                 "nothing was changed." : "taiHEN would load an old OCR plugin line for *%.40s: nothing was changed.",
                 title);
        return refuse(r, VJO_TAI_E_CHECK, 0, why);
    }
    return 0;
}

static int check(const Edit *x, const char *old, size_t old_len, const char *neu, size_t new_len, VjoTaiReport *r)
{
    char title[VJO_TAI_LINE_MAX];
    Line l;
    int rc;
    /* taiHEN must keep the file (plugin.c:129: its validate returns 0 for a file with a section or a
     * path), and every line must be one taiHEN reads whole. */
    if (!new_len || taihen_config_validate(neu) != 0)
        return refuse(r, VJO_TAI_E_CHECK, 0, "taiHEN would not keep the result: nothing was changed.");
    for (size_t pos = 0; pos < new_len; pos = l.next) {
        cut(neu, new_len, pos, &l);
        if (l.raw >= VJO_TAI_LINE_MAX) return refuse(r, VJO_TAI_E_CHECK, 0, "The result would have a line taiHEN stops at.");
    }
    if ((rc = check_title(x, old, neu, "KERNEL", r)) || (rc = check_title(x, old, neu, "main", r))) return rc;
    /* every section name of either text, once */
    for (int pass = 0; pass < 2; pass++) {
        const char *t = pass ? neu : old;
        size_t len = pass ? new_len : old_len;
        for (size_t pos = 0; pos < len; pos = l.next) {
            cut(t, len, pos, &l);
            size_t n = l.ne - l.nb;
            if (l.kind != L_SECTION || has_section(t, len, t + l.nb, n, l.start) ||
                (pass && has_section(old, old_len, t + l.nb, n, old_len)))
                continue;
            if (n >= sizeof(title)) continue; /* not reached: lines are shorter */
            memcpy(title, t + l.nb, n);
            title[n] = '\0';
            if (!strcmp(title, "KERNEL") || !strcmp(title, "main")) continue;
            if ((rc = check_title(x, old, neu, title, r))) return rc;
        }
    }
    /* a title named in neither: what *ALL gives any other game */
    for (unsigned i = 0; i < 1000; i++) {
        snprintf(title, sizeof(title), "VJOCHK%03u", i);
        if (has_section(old, old_len, title, 9, old_len) || has_section(neu, new_len, title, 9, new_len)) continue;
        return check_title(x, old, neu, title, r);
    }
    return 0;
}

int vjo_tai_edit(const char *text, size_t len, int op, int mode, const char *const *titles, int ntitles,
                 char *out, size_t cap, VjoTaiReport *r)
{
    static Games games; /* lines mode's games: the app's install thread and the host tests, one at a time */
    VjoTaiReport scratch;
    Edit x;
    Out o;
    int rc;
    if (!r) r = &scratch;
    memset(r, 0, sizeof(*r));
    if (!text || !out || !cap || (op != VJO_TAI_INSTALL && op != VJO_TAI_UNINSTALL) ||
        (mode != VJO_TAI_WORKER_KERNEL && mode != VJO_TAI_WORKER_LINES) || ntitles < 0 || (ntitles && !titles))
        return refuse(r, VJO_TAI_E_ARGS, 0, "The editor was called wrongly.");
    if (len > VJO_TAI_TEXT_MAX)
        return refuse(r, VJO_TAI_E_TOO_LARGE, 0, "It is over 64 KiB, larger than a taiHEN configuration should be: "
                      "restore a backup (config.txt.vjo-*.bak) or HENkaku's default first.");
    const char *nul = memchr(text, '\0', len);
    if (!nul && text[len] != '\0') return refuse(r, VJO_TAI_E_ARGS, 0, "The editor's text must end with a NUL.");
    if (nul) {
        int line = 1;
        for (const char *p = text; p < nul; p++) line += *p == '\n' || (*p == '\r' && !(p + 1 < nul && p[1] == '\n'));
        return refuse(r, VJO_TAI_E_NUL, line, "It has a NUL byte: taiHEN stops reading there (HENkaku rewrites such a "
                      "file). Restore a backup (config.txt.vjo-*.bak) or HENkaku's default first.");
    }
    memset(&games, 0, sizeof(games));
    memset(&x, 0, sizeof(x));
    x.t = text;
    x.len = len;
    x.op = op;
    x.mode = mode;
    x.games = &games;
    x.nl = line_end(text, len);
    if (op == VJO_TAI_INSTALL && mode == VJO_TAI_WORKER_LINES)
        for (int i = 0; i < ntitles; i++) {
            char id[10];
            const char *v = titles[i];
            size_t n;
            while (*v == ' ' || *v == '*') v++;
            for (n = 0; v[n] && n < 9; n++) id[n] = (char)(v[n] >= 'a' && v[n] <= 'z' ? v[n] - 32 : v[n]);
            id[n] = '\0';
            if (v[n] || !vjo_tai_title_ok(id) || !is_game(id, 9))
                return refuse(r, VJO_TAI_E_TITLE, 0, "A title the OCR worker cannot serve: a game's title ID has 9 "
                              "capital letters and digits (PCSG00415), and is no system app, VitaShell or Overlay.");
            if (games_add(&games, id, 9) < 0) return refuse(r, VJO_TAI_E_SPACE, 0, "More than 512 games.");
        }
    if ((rc = analyse(&x, r))) return rc;
    {
        /* a byte-order mark before a section header: a path to taiHEN, so that section is hidden */
        Line first;
        cut(text, len, 0, &first);
        if (first.raw >= 3 && !memcmp(text, "\xEF\xBB\xBF", 3)) {
            size_t b = 3;
            while (b < first.raw && space((unsigned char)text[b])) b++;
            r->bom_hides_section = b < first.raw && text[b] == '*';
        }
    }
    for (int attempt = 0; attempt < 2; attempt++) {
        VjoTaiReport kept = *r;
        o.out = out;
        o.cap = cap - 1; /* room for the NUL */
        o.used = 0;
        o.full = 0;
        for (int g = 0; g < games.n; g++) games.kept[g] = games.first_done[g] = 0;
        x.keep_marked_headers = attempt;
        emit(&x, &o, r);
        if (o.full) return refuse(r, VJO_TAI_E_SPACE, 0, "The result would be over 64 KiB.");
        /* taiHEN's buffer is the length rounded up to 4 KiB, with no room for the C string's end at a
         * multiple of it (plugin.c:96): one more line end, in a result that changes the file anyway (an
         * unchanged file is not written at all) */
        if (o.used && o.used % 4096 == 0 && (o.used != len || memcmp(out, text, len))) {
            put(&o, x.nl, strlen(x.nl));
            r->padded = 1;
            if (o.full) return refuse(r, VJO_TAI_E_SPACE, 0, "The result would be over 64 KiB.");
        }
        out[o.used] = '\0';
        /* uninstall leaves no section: keep the *KERNEL and *main headers it marked */
        if (op == VJO_TAI_UNINSTALL && !attempt && taihen_config_validate(out) != 0) {
            *r = kept;
            continue;
        }
        break;
    }
    r->changed = o.used != len || memcmp(out, text, len) != 0;
    /* the check reads both texts as taiHEN does: C strings */
    if ((rc = check(&x, text, len, out, o.used, r))) return rc;
    return (int)o.used;
}

int vjo_tai_has_kernel_line(const char *text, size_t len)
{
    Line l;
    if (!text) return 0;
    for (size_t pos = 0; pos < len; pos = l.next) {
        cut(text, len, pos, &l);
        if (line_ours(text, &l) == O_KERNEL) return 1;
    }
    return 0;
}

int vjo_tai_storage_plugin(const char *text, size_t len, char *name, size_t cap)
{
    static const char *const plugins[] = {"storagemgr.skprx", "gamesd.skprx", "yamt.skprx"};
    Line l;
    Sim k;
    if (!text) return 0;
    sim_init(&k, "KERNEL");
    for (size_t pos = 0; pos < len; pos = l.next) {
        cut(text, len, pos, &l);
        if (l.kind == L_SECTION) sim_section(&k, text, &l);
        if (l.kind != L_PATH || !sim_loads(&k)) continue;
        size_t i = l.e;
        while (i > l.b && text[i - 1] != '/' && text[i - 1] != ':') i--;
        for (unsigned p = 0; p < sizeof(plugins) / sizeof(plugins[0]); p++) {
            size_t m = strlen(plugins[p]), j = 0;
            if (l.e - i != m) continue;
            while (j < m && lower((unsigned char)text[i + j]) == plugins[p][j]) j++;
            if (j < m) continue;
            if (name && cap) snprintf(name, cap, "%.*s", (int)(l.e - i), text + i);
            return 1;
        }
    }
    return 0;
}
