/* End-to-end word lookup: real VOCR output (host/fixtures/word_lookup) -> the
 * overlay's sentence -> longest match in the bundled plaintext.vjdict
 * (Bee's Ultimate Dictionary) -> the highlighted span -> the popup
 * entry, with the Shell's own word navigation (Right/Left) and touch hit
 * test. The production code runs unchanged; only file reads are counted.
 *
 * VJO_PLAINTEXT_DIR names the directory holding plaintext.vjdict (CMake
 * prepares it from the pinned archive, tools/prepare_plaintext.py). Without
 * it the test is skipped (exit 77) unless VJO_REQUIRE_PLAINTEXT is set. */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include "acutest.h"
extern "C" {
#include "app_stamp.h"
#include "client.h"
#include "local_dict.h"
#include "platform_posix.h"
#include "styled.h"
#include "text_hit.h"
#include "textjoin.h"
#include "utf.h"
}
#include "../../shell/word_navigation.h"
#include "layout_double.h"

#ifndef VJO_WORD_FIXTURES
#define VJO_WORD_FIXTURES "host/fixtures/word_lookup"
#endif

namespace {

struct Line { std::string id, model, text; };
struct Expect { std::string id, model, at, span, spelling, reading, category; };

std::vector<Line> lines;
std::vector<Expect> expects;
const char *dict_dir;
uint8_t arena_mem[128 * 1024];

/* Counted reads: the console's cost is file reads, not host time. */
int (*inner_open)(void *, const char *, VjoFile *);
VjoFile inner_files[VJO_LOCAL_MAX_DICTS];
int inner_used[VJO_LOCAL_MAX_DICTS];
unsigned long reads, read_bytes, opens, closes;

int counted_read(void *ctx, uint64_t off, void *dst, size_t n)
{
    VjoFile *f = (VjoFile *)ctx;
    reads++; read_bytes += n;
    return f->read(f->ctx, off, dst, n);
}
void counted_close(void *ctx)
{
    VjoFile *f = (VjoFile *)ctx;
    f->close(f->ctx);
    inner_used[f - inner_files] = 0;
    closes++;
}
int counted_open(void *ud, const char *path, VjoFile *out)
{
    for (int i = 0; i < VJO_LOCAL_MAX_DICTS; i++) {
        if (inner_used[i]) continue;
        if (inner_open(ud, path, &inner_files[i])) return -1;
        inner_used[i] = 1; opens++;
        *out = (VjoFile){&inner_files[i], inner_files[i].size, counted_read, counted_close, inner_files[i].stamp};
        return 0;
    }
    return -1;
}

std::vector<std::string> split_tabs(const std::string &s)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t tab = s.find('\t', start);
        out.push_back(s.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos) return out;
        start = tab + 1;
    }
}

std::vector<std::vector<std::string>> read_tsv(const char *name)
{
    std::vector<std::vector<std::string>> rows;
    std::string path = std::string(VJO_WORD_FIXTURES) + "/" + name;
    FILE *f = fopen(path.c_str(), "r");
    char buf[16384];
    TEST_ASSERT_(f != NULL, "fixture %s", path.c_str());
    while (fgets(buf, sizeof(buf), f)) {
        std::string s(buf);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.empty() || s[0] == '#') continue;
        rows.push_back(split_tabs(s));
    }
    fclose(f);
    return rows;
}

void load(void)
{
    if (!lines.empty()) return;
    for (auto &r : read_tsv("vocr_lines.tsv")) {
        TEST_ASSERT(r.size() == 6);
        std::string text;
        for (size_t i = 0; i < r[5].size(); i++) {
            if (r[5][i] == '\\' && i + 1 < r[5].size() && r[5][i + 1] == 'n') { text += '\n'; i++; }
            else text += r[5][i];
        }
        lines.push_back({r[0], r[1], text});
    }
    for (auto &r : read_tsv("expect.tsv")) {
        TEST_ASSERT(r.size() == 7);
        expects.push_back({r[0], r[1], r[2], r[3], r[4], r[5], r[6]});
    }
}

void setup(VjoConfig *cfg, VjoPlatform *p, PosixPlatform *pp)
{
    vjo_config_defaults(cfg); /* the release defaults: word highlight, Japanese characters only OFF, one result */
    cfg->dictionary = VJO_DICT_LOCAL;
    snprintf(cfg->local_dictionary_dir, sizeof(cfg->local_dictionary_dir), "%s", dict_dir);
    snprintf(cfg->local_dictionaries, sizeof(cfg->local_dictionaries), "plaintext.vjdict");
    *pp = PosixPlatform();
    posix_platform_init(pp, p);
    inner_open = p->file_open;
    p->file_open = counted_open;
}

const Line *find_line(const std::string &id, const std::string &model)
{
    for (auto &l : lines) if (l.id == id && l.model == model) return &l;
    return NULL;
}

double now_us(void)
{
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

size_t char_end(const char *s, size_t len, size_t at)
{
    size_t next = vjo_utf8_cursor_step(s, len, at, 1);
    return next > at ? next : len;
}

/* The header range the overlay recolors for an entry: UTF-16 units of the
 * sentence before and inside the match (vjo_entry_header_range). */
void check_header_range(const VjoOverlayData *d, size_t start, size_t end)
{
    uint32_t hs = 0, hl = 0, units = 0, span = 0;
    size_t i = 0, len = strlen(d->sentence);
    TEST_ASSERT(vjo_entry_header_range(&d->list, 0, &hs, &hl));
    while (i < start) { uint32_t cp = vjo_utf8_next(d->sentence, len, &i); if (cp != '\r') units += vjo_utf16_units(cp); }
    while (i < end) { uint32_t cp = vjo_utf8_next(d->sentence, len, &i); if (cp != '\r') span += vjo_utf16_units(cp); }
    TEST_CHECK_(hs == units && hl == span, "header range %u+%u, expected %u+%u", hs, hl, units, span);
}

/* The bundled dictionary's name, page 0 bytes 192..255 (bee-san/plaintext-dicts). */
const char kTitle[] = "Bee's Ultimate Dictionary";

/* A converter source label: a first line "[name]" before the definition. */
bool label_line(const char *text)
{
    const char *nl = strchr(text, '\n');
    return text[0] == '[' && nl && nl[-1] == ']';
}

/* The spelling's distinct kanji in order (kanji_lines.py gives 々〆ヵヶ no line). */
std::vector<std::string> distinct_kanji(const char *s)
{
    std::vector<std::string> out;
    size_t len = strlen(s), i = 0;
    while (i < len) {
        size_t at = i;
        uint32_t cp = vjo_utf8_next(s, len, &i);
        std::string c(s + at, i - at);
        if (vjo_is_kanji(cp) && cp != 0x3005 && cp != 0x3006 && cp != 0x30F5 && cp != 0x30F6 &&
            std::find(out.begin(), out.end(), c) == out.end())
            out.push_back(c);
    }
    return out;
}

/* An entry text's kanji lines (vjo_kanji_block), as (kanji, meanings). */
std::vector<std::pair<std::string, std::string>> kanji_lines(const char *text)
{
    std::vector<std::pair<std::string, std::string>> out;
    size_t len = strlen(text), at = vjo_kanji_block(text);
    while (at < len) {
        const char *nl = strchr(text + at, '\n');
        std::string line(text + at, nl ? (size_t)(nl - (text + at)) : len - at);
        size_t eq = line.find('=');
        out.push_back({line.substr(0, eq), line.substr(eq + 1)});
        at += line.size() + 1;
    }
    return out;
}

std::vector<std::string> block_kanji(const VjoVocab *v)
{
    std::vector<std::string> out;
    for (auto &l : kanji_lines(v->meanings[0])) out.push_back(l.first);
    return out;
}

/* UTF-8 as the styled text's UTF-16 units. */
std::u16string u16(const std::string &s)
{
    std::u16string out;
    size_t i = 0;
    while (i < s.size()) {
        uint32_t cp = vjo_utf8_next(s.c_str(), s.size(), &i);
        if (cp >= 0x10000) { cp -= 0x10000; out += (char16_t)(0xD800 | (cp >> 10)); out += (char16_t)(0xDC00 | (cp & 0x3FF)); }
        else if (cp != '\r') out += (char16_t)cp;
    }
    return out;
}

std::u16string styled_text(const VjoStyled &st) { return std::u16string((const char16_t *)st.text, st.len); }

/* The units of an entry's head line and text, as shown whole: a body shorter
 * than this + 1 (the newline between them) was clipped. */
uint32_t units_of_entry(const VjoVocab *v)
{
    std::string head = v->spelling;
    char badge[VJO_RANK_BADGE_MAX];
    if (strcmp(v->spelling, v->reading)) head += std::string(" (") + v->reading + ")";
    head += vjo_rank_badge(badge, v->rank, "Jiten");
    /* vjo_styled_entry adds a blank line between a definition and its kanji lines */
    const char *t = v->meanings[0];
    size_t k = vjo_kanji_block(t);
    return (uint32_t)(u16(head).size() + u16(t).size() + (k && k < strlen(t)));
}

/* The body as overlay.cpp builds it with every line before the entry at its
 * longest (the dictionary page, the Rubi app's notice, a status, the Anki
 * queue and status, the queued line) and four config warnings of the longest
 * kind after it, kept by the reserve: what an entry can count on. */
void worst_case_body(VjoStyled *st, VjoArena *a, const VjoEntryList *l, int ja, int en)
{
    std::string status(255, 's'), queue(79, 'q'), anki(127, 'a'), warning(95, 'w');
    const char *notice = strlen(VJO_APP_NOTICE_NEWER) > strlen(VJO_APP_NOTICE) ? VJO_APP_NOTICE_NEWER : VJO_APP_NOTICE;
    TEST_ASSERT(vjo_styled_init(st, a, VJO_BODY_UNITS, VJO_BODY_SPANS) == 0);
    vjo_styled_dict_page(st, 7, 8, en);
    vjo_styled_puts(st, "\n", VJO_RGB_ERROR, en);
    vjo_styled_puts(st, notice, VJO_RGB_ERROR, en);
    vjo_styled_puts(st, "\n", VJO_RGB_DIM, en);
    vjo_styled_puts(st, status.c_str(), VJO_RGB_DIM, en);
    vjo_styled_puts(st, "\n", VJO_RGB_DIM, en);
    vjo_styled_puts(st, queue.c_str(), VJO_RGB_DIM, en);
    vjo_styled_puts(st, "\n", VJO_RGB_DIM, en);
    vjo_styled_puts(st, anki.c_str(), VJO_RGB_DIM, en);
    vjo_styled_puts(st, "\n", VJO_RGB_DIM, en);
    st->cap -= 4 * (1 + 12 + 96); /* overlay.cpp: each warning's room, at its longest */
    vjo_styled_entry(st, l, 0, ja, en, 1, 1);
    st->cap += 4 * (1 + 12 + 96);
    for (int i = 0; i < 4; i++) {
        vjo_styled_puts(st, "\n", VJO_RGB_ERROR, en);
        vjo_styled_puts(st, "config.ini: ", VJO_RGB_ERROR, en);
        vjo_styled_puts(st, warning.c_str(), VJO_RGB_ERROR, en);
    }
}

} // namespace

/* Every real line, word by word with Right as the Shell navigates before
 * the header is laid out (the whole text one row), with cfg's Japanese
 * characters only: one entry or none per lookup, the highlight starting at
 * the cursor and ending on a character boundary, the popup naming the entry,
 * Right advancing until it wraps from the last word, nothing left allocated
 * between lookups and every file closed. */
static void walk_word_by_word(VjoConfig &cfg, VjoPlatform &p)
{
    const int japanese_only = cfg.japanese_only_selection;
    unsigned long lookups = 0, matched = 0, single = 0, none = 0, letters = 0, covered = 0, max_reads = 0;
    double total_us = 0, max_us = 0;
    for (auto &line : lines) {
        VjoArena a; VjoOverlayData d;
        vjo_arena_init(&a, arena_mem, sizeof(arena_mem));
        TEST_ASSERT(vjo_overlay_ocr_text(&a, &cfg, line.text.c_str(), &d) == VJO_OK);
        const char *s = d.sentence;
        size_t len = strlen(s), mark = vjo_arena_mark(&a);
        /* The Shell's first selection (worker.c): the first Japanese letter, or with Japanese characters only
         * OFF the text's start. */
        size_t cursor = japanese_only ? vjo_utf8_japanese_cursor_step(s, len, 0, 0) : 0;
        VjoWordNavigation nav = VjoWordNavigation();
        for (size_t i = 0; i < len;) { size_t j = i; if (vjo_is_japanese_letter(vjo_utf8_next(s, len, &j))) letters++; i = j; }
        for (int guard = 0; cursor < len; guard++) {
            TEST_ASSERT_(guard < 400, "%s: navigation does not end", line.id.c_str());
            reads = read_bytes = 0;
            double t0 = now_us();
            int rc = vjo_overlay_lookup_at(&a, &p, &cfg, cursor, &d);
            double us = now_us() - t0;
            total_us += us; if (us > max_us) max_us = us;
            if (reads > max_reads) max_reads = reads;
            lookups++;
            TEST_ASSERT_(rc == VJO_OK, "%s at %zu: rc %d", line.id.c_str(), cursor, rc);
            TEST_CHECK(opens == closes);
            TEST_CHECK(d.list.n_entries <= 1);
            size_t end = 0;
            if (d.list.n_entries == 1) {
                const VjoEntry *e = &d.list.entries[0];
                matched++;
                TEST_CHECK(e->hl_start == (int)cursor && e->hl_end > (int)cursor && e->hl_end <= (int)len);
                end = (size_t)e->hl_end;
                TEST_CHECK(end == len || vjo_utf8_cursor_step(s, len, end, 0) == end);
                TEST_CHECK(e->vocab && e->text && !strncmp(e->text, e->vocab->spelling, strlen(e->vocab->spelling)));
                check_header_range(&d, cursor, end);
                if (end == char_end(s, len, cursor)) single++;
                for (size_t i = cursor; i < end;) { size_t j = i; if (vjo_is_japanese_letter(vjo_utf8_next(s, len, &j))) covered++; i = j; }
            } else {
                none++;
            }
            int row = VJO_ROW_STAY;
            size_t next = vjo_word_cursor_step(nav, s, len, cursor, end, 1, cfg.japanese_only_selection, NULL, &row);
            /* without a layout a step in reading order needs none; the text's end does */
            TEST_CHECK(next > cursor ? row == VJO_ROW_MOVED : row == VJO_ROW_UNAVAILABLE || row == VJO_ROW_STAY);
            if (next <= cursor) break; /* the last word: Right stays there (one line: wraps to the first) */
            TEST_CHECK_(!end || next >= end, "%s: Right from %zu to %zu (match end %zu)",
                        line.id.c_str(), cursor, next, end);
            vjo_arena_release(&a, mark); /* the Shell releases the shown entry before the next cursor */
            cursor = next;
        }
    }
    printf("\n  Japanese characters only %s: %zu real VOCR lines, %lu lookups: %lu entries (%lu one character), "
           "%lu without; %lu of %lu Japanese letters inside a matched word (%.1f%%)\n"
           "  host lookup %.1f us mean, %.1f us max; at most %lu file reads per lookup\n",
           japanese_only ? "ON" : "OFF", lines.size(), lookups, matched, single, none, covered, letters,
           100.0 * covered / (letters ? letters : 1), total_us / (lookups ? lookups : 1), max_us, max_reads);
}

/* The walk with the release defaults (Whole word, Read text Automatically), once with Japanese characters
 * only ON (the default until 2026-10-20, whose figures docs/touch-lookup.md gives) and once OFF, the
 * default. */
static void real_lines_walk_word_by_word(void)
{
    VjoConfig cfg; VjoPlatform p; PosixPlatform pp;
    load(); setup(&cfg, &p, &pp);
    TEST_CHECK(cfg.highlight_mode == VJO_HIGHLIGHT_WORD && !cfg.japanese_only_selection && cfg.ocr_mode == VJO_OCR_AUTO);
    cfg.japanese_only_selection = 1;
    walk_word_by_word(cfg, p);
    cfg.japanese_only_selection = 0;
    walk_word_by_word(cfg, p);
}

/* The arrow keys over displayed rows of every real line (#81; the owner's rule
 * of 2026-10-16), laid out as the header is on the console (font_size_ja 18:
 * 27 px cells in the 920 px box), with each region's lines also joined into one
 * long line (soft wraps at 34 cells) and in a 270 px box (soft wraps every 10
 * cells). Whole word Right, with the real lookups' match ends, walks the whole
 * text in reading order across its rows, each step to the first letter at or
 * after the match's end (the rule restated over the cells, layout_double.h),
 * and stops at the text's last word, going round to its first word only on a
 * text on one row; the stick's step (no wrap) is the same but there. Left
 * retraces the last 16 Rights, then steps one character back. Up and Down from
 * every letter land where the rule restated over the cells says. */
static void real_lines_navigate_displayed_rows(void)
{
    VjoConfig cfg; VjoPlatform p; PosixPlatform pp;
    static Layout lay;
    unsigned long texts = 0, rows_seen = 0, one_row = 0, crossings = 0, words = 0, verticals = 0, lookups_n = 0,
                  long_walks = 0;
    load(); setup(&cfg, &p, &pp);
    for (auto &line : lines) {
        std::string joined;
        for (char c : line.text) if (c != '\n') joined += c;
        const std::string variants[3] = {line.text, joined, line.text};
        const float widths[3] = {920.0f, 920.0f, 270.0f};
        for (int v = 0; v < 3; v++) {
            VjoArena a; VjoOverlayData d;
            vjo_arena_init(&a, arena_mem, sizeof(arena_mem));
            TEST_ASSERT(vjo_overlay_ocr_text(&a, &cfg, variants[v].c_str(), &d) == VJO_OK);
            const char *s = d.sentence;
            size_t len = strlen(s), mark = vjo_arena_mark(&a);
            layout_text(&lay, s, widths[v], 27.0f, 13.5f);
            VjoTextRows rows = layout_rows(&lay);
            TEST_CASE_("%s variant %d", line.id.c_str(), v);
            std::vector<unsigned> sel;
            for (unsigned k = 0; k < lay.n; k++)
                if (layout_selectable(&lay, k, 1)) sel.push_back(k);
            if (sel.empty()) continue;
            texts++;
            for (uint32_t row = 0, k = 0; row < lay.lines; row++) {
                while (k < sel.size() && lay.line[sel[k]] < row) k++;
                rows_seen += k < sel.size() && lay.line[sel[k]] == row;
            }
            const int one = lay.line[sel.front()] == lay.line[sel.back()];
            one_row += one;
            VjoWordNavigation nav = VjoWordNavigation();
            std::vector<size_t> visited;
            size_t cursor = lay.begin[sel.front()];
            int rc = VJO_ROW_STAY;
            for (int guard = 0;; guard++) {
                TEST_ASSERT_(guard < 1000, "Right does not reach the text's end");
                vjo_arena_release(&a, mark);
                TEST_ASSERT(vjo_overlay_lookup_at(&a, &p, &cfg, cursor, &d) == VJO_OK);
                lookups_n++;
                size_t end = d.list.n_entries ? (size_t)d.list.entries[0].hl_end : 0;
                size_t from = end > cursor && end <= len && (end == len || vjo_utf8_cursor_step(s, len, end, 0) == end)
                                  ? end : cursor + 1;
                int wrapped;
                size_t want = ref_read_step(&lay, cursor, from, 1, 1, 1, &wrapped);
                VjoWordNavigation as_stick = nav;
                int stick_rc;
                size_t stick_next = vjo_word_cursor_step(as_stick, s, len, cursor, end, 1, 1, &rows, &stick_rc, 0);
                visited.push_back(cursor);
                size_t next = vjo_word_cursor_step(nav, s, len, cursor, end, 1, 1, &rows, &rc);
                TEST_CHECK_(next == want && rc == (want == cursor ? VJO_ROW_STAY : wrapped ? VJO_ROW_WRAPPED : VJO_ROW_MOVED),
                            "Right from %zu (match end %zu): %zu rc %d, expected %zu", cursor, end, next, rc, want);
                TEST_CHECK(stick_next == (rc == VJO_ROW_WRAPPED ? cursor : next) && stick_rc >= 0);
                if (rc == VJO_ROW_STAY) {
                    TEST_CHECK(!one || cursor == lay.begin[sel.front()]); /* one row stays only with nowhere to go round */
                    break;
                }
                TEST_ASSERT(rc > 0);
                if (rc == VJO_ROW_WRAPPED) {
                    TEST_CHECK(one && next == lay.begin[sel.front()]);
                    cursor = next;
                    break;
                }
                TEST_CHECK(next > cursor && (!end || next >= end));
                crossings += lay.line[layout_at(&lay, next)] != lay.line[layout_at(&lay, cursor)];
                cursor = next;
                words++;
            }
            /* Left undoes the last 16 Rights, newest first (a wrap among them),
             * then steps one character back, or stays at the text's first. */
            size_t pushed = rc == VJO_ROW_WRAPPED ? visited.size() : visited.size() - 1;
            size_t kept = pushed < 16 ? 0 : pushed - 16;
            for (size_t k = pushed; k-- > kept;) {
                cursor = vjo_word_cursor_step(nav, s, len, cursor, 0, -1, 1, &rows, &rc);
                TEST_CHECK_(cursor == visited[k] && rc == VJO_ROW_MOVED, "Left to %zu, expected %zu", cursor, visited[k]);
            }
            int wrapped;
            size_t want = ref_read_step(&lay, cursor, cursor + 1, -1, 1, 1, &wrapped);
            size_t back = vjo_word_cursor_step(nav, s, len, cursor, 0, -1, 1, &rows, &rc);
            TEST_CHECK(back == want && rc == (want == cursor ? VJO_ROW_STAY : wrapped ? VJO_ROW_WRAPPED : VJO_ROW_MOVED));
            long_walks += kept > 0;
            TEST_CHECK(nav.count == 0);
            /* Up/Down from every letter, with no column and with the one kept. */
            for (unsigned k = 0; k < lay.n; k++) {
                if (!layout_selectable(&lay, k, 1)) continue;
                for (int dy = -1; dy <= 1; dy += 2) {
                    VjoRowGoal goal = {0, 0};
                    size_t out = lay.begin[k], want2 = ref_vertical(&lay, lay.begin[k], dy, 1, -1.0f);
                    vjo_text_row_vertical(s, len, &rows, lay.begin[k], dy, 1, &goal, &out);
                    TEST_CHECK_(out == want2, "byte %zu dy %d: %zu, expected %zu", lay.begin[k], dy, out, want2);
                    if (out != lay.begin[k]) {
                        float column = goal.x;
                        size_t again = out, want3 = ref_vertical(&lay, out, dy, 1, column);
                        vjo_text_row_vertical(s, len, &rows, out, dy, 1, &goal, &again);
                        TEST_CHECK_(again == want3, "byte %zu kept column: %zu, expected %zu", out, again, want3);
                    }
                    verticals++;
                }
            }
            TEST_CHECK(lay.bad_queries == 0 && opens == closes);
        }
    }
    TEST_CHECK(one_row > 0 && one_row < texts && crossings > 0); /* both kinds of text, and rows crossed */
    printf("\n  %zu real lines x 3 layouts: %lu texts on %lu rows (%lu on one row, which wrap), Right through "
           "%lu words crossing %lu row ends (%lu walks over 16 words), %lu lookups; %lu Up/Down checks\n",
           lines.size(), texts, rows_seen, one_row, words, crossings, long_walks, lookups_n, verticals);
}

/* Inflections, kana-only words, katakana loanwords, names, common readings
 * and words broken by OCR errors, each at a real recognized line. */
static void expected_entries_at_real_lines(void)
{
    VjoConfig cfg; VjoPlatform p; PosixPlatform pp;
    load(); setup(&cfg, &p, &pp);
    TEST_CHECK(expects.size() >= 40);
    for (auto &x : expects) {
        const Line *line = find_line(x.id, x.model);
        VjoArena a; VjoOverlayData d;
        TEST_CASE_("%s %s %s (%s)", x.id.c_str(), x.model.c_str(), x.at.c_str(), x.category.c_str());
        TEST_ASSERT(line != NULL);
        vjo_arena_init(&a, arena_mem, sizeof(arena_mem));
        TEST_ASSERT(vjo_overlay_ocr_text(&a, &cfg, line->text.c_str(), &d) == VJO_OK);
        const char *at = strstr(d.sentence, x.at.c_str());
        TEST_ASSERT_(at != NULL, "\"%s\" not in \"%s\"", x.at.c_str(), d.sentence);
        size_t cursor = (size_t)(at - d.sentence), len = strlen(d.sentence);
        TEST_ASSERT(vjo_overlay_lookup_at(&a, &p, &cfg, cursor, &d) == VJO_OK);
        if (x.span.empty()) {
            /* No entry starts here: the overlay keeps the character highlighted
             * and Right steps one character. */
            TEST_CHECK_(d.list.n_entries == 0, "unexpected entry %s",
                        d.list.n_entries ? d.list.entries[0].vocab->spelling : "");
            VjoWordNavigation nav = VjoWordNavigation();
            TEST_CHECK(vjo_word_cursor_step(nav, d.sentence, len, cursor, 0, 1, 1) ==
                       vjo_utf8_japanese_cursor_step(d.sentence, len, char_end(d.sentence, len, cursor), 0));
            continue;
        }
        TEST_ASSERT_(d.list.n_entries == 1, "no entry at \"%s\"", x.at.c_str());
        const VjoEntry *e = &d.list.entries[0];
        std::string span(d.sentence + e->hl_start, (size_t)(e->hl_end - e->hl_start));
        TEST_CHECK_(span == x.span, "span \"%s\", expected \"%s\"", span.c_str(), x.span.c_str());
        TEST_CHECK_(x.spelling == e->vocab->spelling, "entry %s, expected %s", e->vocab->spelling, x.spelling.c_str());
        const char *reading = strcmp(e->vocab->reading, e->vocab->spelling) ? e->vocab->reading : "-";
        TEST_CHECK_(x.reading == reading, "reading %s, expected %s", reading, x.reading.c_str());
        /* The popup text: "spelling (reading) rank\ndefinition\n\n漢=meaning..." with
         * no source label: the dictionary's name is its file's title. */
        TEST_CHECK_(!label_line(e->vocab->meanings[0]), "a label line: %s", e->vocab->meanings[0]);
        TEST_CHECK(e->vocab->dictionary && !strcmp(e->vocab->dictionary, kTitle));
        TEST_CHECK_(block_kanji(e->vocab) == distinct_kanji(e->vocab->spelling), "kanji lines of %s",
                    e->vocab->spelling);
        check_header_range(&d, (size_t)e->hl_start, (size_t)e->hl_end);
    }
}

/* Touching a character of the header selects the word starting there: the
 * hit test returns its byte (including after a newline), and the lookup at
 * that byte is the entry Right navigation shows. Left retraces. */
namespace {
struct Grid { const char *s; size_t len; float cell; };
int grid_position(void *ctx, uint32_t index16, float *x, uint32_t *line)
{
    const Grid *g = (const Grid *)ctx;
    uint32_t unit = 0, col = 0, row = 0;
    for (size_t i = 0; i < g->len;) {
        uint32_t cp = vjo_utf8_next(g->s, g->len, &i);
        if (cp == '\r') continue;
        if (unit == index16) { *x = col * g->cell; *line = row; return 0; }
        unit += (uint32_t)vjo_utf16_units(cp);
        if (cp == '\n') { row++; col = 0; } else col++;
    }
    return -1;
}
}

static void touch_and_left_right_follow_the_words(void)
{
    VjoConfig cfg; VjoPlatform p; PosixPlatform pp;
    load(); setup(&cfg, &p, &pp);
    const Line *line = find_line("vita_tight_v13090_154662", "H15"); /* 伊佐那社 / ふふ、春休み入ってから / ... */
    TEST_ASSERT(line != NULL);
    VjoArena a; VjoOverlayData d;
    vjo_arena_init(&a, arena_mem, sizeof(arena_mem));
    TEST_ASSERT(vjo_overlay_ocr_text(&a, &cfg, line->text.c_str(), &d) == VJO_OK);
    const char *s = d.sentence;
    size_t len = strlen(s), mark = vjo_arena_mark(&a);
    uint32_t lines_n = 1;
    for (size_t i = 0; i < len; i++) if (s[i] == '\n') lines_n++;
    Grid grid = {s, len, 24.0f};
    VjoTextHitLayout layout = {100.0f, 50.0f, 24.0f * 40, 30.0f * lines_n, 30.0f * lines_n, 0.0f, 24.0f,
                               lines_n, grid_position, &grid};
    /* Right from the first selection to 春休み, recording the cursors. */
    VjoWordNavigation nav = VjoWordNavigation();
    size_t cursor = vjo_utf8_japanese_cursor_step(s, len, 0, 0), target = (size_t)(strstr(s, "春休み") - s);
    std::vector<size_t> visited;
    while (cursor < target) {
        TEST_ASSERT(vjo_overlay_lookup_at(&a, &p, &cfg, cursor, &d) == VJO_OK);
        size_t end = d.list.n_entries ? (size_t)d.list.entries[0].hl_end : 0;
        visited.push_back(cursor);
        size_t next = vjo_word_cursor_step(nav, s, len, cursor, end, 1, 1);
        TEST_ASSERT(next > cursor);
        vjo_arena_release(&a, mark);
        cursor = next;
    }
    TEST_CHECK(cursor == target);
    TEST_ASSERT(vjo_overlay_lookup_at(&a, &p, &cfg, cursor, &d) == VJO_OK);
    TEST_ASSERT(d.list.n_entries == 1 && !strcmp(d.list.entries[0].vocab->spelling, "春休み"));
    /* Left retraces exactly the visited starts, newest first. */
    for (size_t k = visited.size(); k-- > 0;) {
        cursor = vjo_word_cursor_step(nav, s, len, cursor, 0, -1, 1);
        TEST_CHECK(cursor == visited[k]);
    }
    /* A tap on each character of line 2 selects that character's word. */
    size_t line2 = (size_t)(strchr(s, '\n') - s) + 1;
    uint32_t col = 0;
    for (size_t i = line2; i < len && s[i] != '\n'; i = char_end(s, len, i), col++) {
        size_t hit = len;
        float x = layout.left + col * grid.cell + grid.cell / 2, y = layout.top + 30.0f * 1 + 15.0f;
        int ok = vjo_text_hit(s, len, &layout, x, y, 1, &hit);
        uint32_t cp; size_t j = i; cp = vjo_utf8_next(s, len, &j);
        if (!vjo_is_japanese_letter(cp)) { TEST_CHECK(!ok); continue; } /* 、 has no hit (Japanese-only) */
        TEST_CHECK_(ok && hit == i, "tap column %u: byte %zu, expected %zu", col, hit, i);
        vjo_arena_release(&a, mark);
        TEST_ASSERT(vjo_overlay_lookup_at(&a, &p, &cfg, hit, &d) == VJO_OK);
        TEST_CHECK(d.list.n_entries <= 1 && (!d.list.n_entries || d.list.entries[0].hl_start == (int)i));
    }
    /* Character mode steps one letter at a time over the same text. */
    size_t c = vjo_utf8_japanese_cursor_step(s, len, 0, 0);
    TEST_CHECK(vjo_utf8_japanese_cursor_step(s, len, c, 1) == char_end(s, len, c));
}

/* The deinflection the overlay relies on, with the bundled dictionary:
 * Yomichan's 569 suffix rules (core/deinflect.inc) and the converter's
 * part-of-speech flags. 静かな is the na-adjective and the next word (な), as
 * in Yomitan. Synthetic phrases, not game text. */
static void bundled_dictionary_deinflects(void)
{
    static const char *const cases[][3] = {
        {"食べた", "食べた", "食べる"}, {"行かなかった", "行かなかった", "行く"},
        {"静かな", "静か", "静か"}, {"美しかった", "美しかった", "美しい"},
        {"読んでいる", "読んでいる", "読む"}, {"来ました", "来ました", "来る"},
        {"高くなかった", "高くなかった", "高い"}, {"食べませんでした", "食べませんでした", "食べる"},
        {"しなかった", "しなかった", "する"}, {"見せてくれ", "見せて", "見せる"}};
    VjoConfig cfg; VjoPlatform p; PosixPlatform pp;
    setup(&cfg, &p, &pp);
    for (auto &c : cases) {
        VjoArena a; VjoOverlayData d;
        TEST_CASE(c[0]);
        vjo_arena_init(&a, arena_mem, sizeof(arena_mem));
        TEST_ASSERT(vjo_overlay_ocr_text(&a, &cfg, c[0], &d) == VJO_OK);
        TEST_ASSERT(vjo_overlay_lookup_at(&a, &p, &cfg, 0, &d) == VJO_OK && d.list.n_entries == 1);
        std::string span(d.sentence, (size_t)d.list.entries[0].hl_end);
        TEST_CHECK_(span == c[1] && !strcmp(d.list.entries[0].vocab->spelling, c[2]), "%s: %s, entry %s",
                    c[0], span.c_str(), d.list.entries[0].vocab->spelling);
    }
}

/* The built-in table of common readings (local_dict.c preferred_readings):
 * with the bundled dictionary, the text of each key alone shows the entry
 * with the listed reading (or, for a kana key, spelling), over the whole key.
 * A pair whose entry is missing, or that something ranks above, fails. */
static void preferred_readings_exist_in_the_bundle(void)
{
    VjoConfig cfg; VjoPlatform p; PosixPlatform pp;
    int pairs = 0;
    setup(&cfg, &p, &pp);
    for (const char *key = vjo_local_preferred_table(); *key; key += strlen(key) + 1) {
        const char *value = key + strlen(key) + 1;
        VjoArena a; VjoOverlayData d;
        TEST_CASE_("%s -> %s", key, value);
        vjo_arena_init(&a, arena_mem, sizeof(arena_mem));
        TEST_ASSERT(vjo_overlay_ocr_text(&a, &cfg, key, &d) == VJO_OK);
        TEST_ASSERT(vjo_overlay_lookup_at(&a, &p, &cfg, 0, &d) == VJO_OK && d.list.n_entries == 1);
        const VjoVocab *v = d.list.entries[0].vocab;
        TEST_CHECK_((size_t)d.list.entries[0].hl_end == strlen(key), "span %d of %zu",
                    d.list.entries[0].hl_end, strlen(key));
        TEST_CHECK_(!strcmp(v->spelling, value) || !strcmp(v->reading, value), "shows %s (%s)", v->spelling,
                    v->reading);
        key = value;
        pairs++;
    }
    printf("  %d pairs\n", pairs);
    TEST_CHECK(pairs == 70); /* a truncated table would test fewer */
}

/* The ranked files' usual spellings (local_dict.c usual_spellings): with the
 * bundled dictionary (format 3), each key alone shows the listed spelling (or
 * reading) over the whole key, as do its inflections; and the text's own entry
 * still comes before an inflection's usual spelling (audit-dict-freq-b). */
static void usual_spellings_win_in_the_bundle(void)
{
    VjoConfig cfg; VjoPlatform p; PosixPlatform pp;
    int pairs = 0;
    /* Bee's Ultimate Dictionary has grammar entries spelled as some whole kana
     * forms: they are the text as written, so they win at that span (せいで,
     * によらず); なくして shows 無くして, whose grammar summary comes first. */
    static const char *const forms[][3] = {
        {"やめて", "やめて", "止める"}, {"やめろ", "やめろ", "止める"}, {"やめなさい", "やめなさい", "止める"},
        {"おじさんが", "おじさん", "小父さん"}, {"せいで", "せいで", "せいで"}, {"なくなった", "なくなった", "無くなる"},
        {"なくして", "なくして", "無くして"}, {"によらず", "によらず", "によらず"}};
    setup(&cfg, &p, &pp);
    for (const char *key = vjo_local_usual_table(); *key; key += strlen(key) + 1) {
        const char *value = key + strlen(key) + 1;
        VjoArena a; VjoOverlayData d;
        TEST_CASE_("%s -> %s", key, value);
        vjo_arena_init(&a, arena_mem, sizeof(arena_mem));
        TEST_ASSERT(vjo_overlay_ocr_text(&a, &cfg, key, &d) == VJO_OK);
        TEST_ASSERT(vjo_overlay_lookup_at(&a, &p, &cfg, 0, &d) == VJO_OK && d.list.n_entries == 1);
        const VjoVocab *v = d.list.entries[0].vocab;
        TEST_CHECK_((size_t)d.list.entries[0].hl_end == strlen(key), "span %d of %zu",
                    d.list.entries[0].hl_end, strlen(key));
        TEST_CHECK_(!strcmp(v->spelling, value) || !strcmp(v->reading, value), "shows %s (%s)", v->spelling,
                    v->reading);
        TEST_CHECK_(v->rank != VJO_NO_RANK, "a ranked entry"); /* the bundle has ranks */
        key = value;
        pairs++;
    }
    printf("  %d pairs\n", pairs);
    TEST_CHECK(pairs == 15);
    for (const auto &f : forms) {
        VjoArena a; VjoOverlayData d;
        TEST_CASE(f[0]);
        vjo_arena_init(&a, arena_mem, sizeof(arena_mem));
        TEST_ASSERT(vjo_overlay_ocr_text(&a, &cfg, f[0], &d) == VJO_OK);
        TEST_ASSERT(vjo_overlay_lookup_at(&a, &p, &cfg, 0, &d) == VJO_OK && d.list.n_entries == 1);
        TEST_CHECK_((size_t)d.list.entries[0].hl_end == strlen(f[1]) &&
                        !strcmp(d.list.entries[0].vocab->spelling, f[2]),
                    "span %d, shows %s", d.list.entries[0].hl_end, d.list.entries[0].vocab->spelling);
    }
}

/* Bee's Ultimate Dictionary's own content at a cursor, as the popup gets it:
 * a word with one kanji line (木), one with three in spelling order (日本語),
 * and grammar points whose summary replaced the definition and that have no
 * kanji (the particle は, ～てしまう after a verb's て-form, its でしまう alias
 * key, the contraction ちゃう), every one without a source label and named by
 * the file's title. The popup body shows each kanji line's kanji at the
 * headword's size. */
static void ultimate_dictionary_fixture_entries(void)
{
    struct Case {
        const char *text, *at, *span, *spelling, *reading, *contains;
        std::vector<std::pair<std::string, std::string>> lines;
    };
    const std::vector<Case> cases = {
        {"木を切る", "木", "木", "木", "き", "a tree", {{"木", "tree, wood"}}},
        {"日本語を話す", "日本語", "日本語", "日本語", "にほんご", "Japanese",
         {{"日", "day, counter for days, Japan, sun"}, {"本", "real, true, origin, book"}, {"語", "word, language"}}},
        {"猫は可愛い", "は", "は", "は", "-", "Topic particle", {}},
        {"傘を忘れてしまった", "てしまった", "てしまった", "てしまう", "-", "end up", {}},
        {"全部飲んでしまった", "でしまった", "でしまった", "てしまう", "-", "end up", {}},
        {"もう寝ちゃう", "ちゃう", "ちゃう", "ちゃう", "-", "Contracted てしまう", {}},
    };
    VjoConfig cfg; VjoPlatform p; PosixPlatform pp;
    setup(&cfg, &p, &pp);
    for (auto &c : cases) {
        VjoArena a; VjoOverlayData d;
        TEST_CASE(c.text);
        vjo_arena_init(&a, arena_mem, sizeof(arena_mem));
        TEST_ASSERT(vjo_overlay_ocr_text(&a, &cfg, c.text, &d) == VJO_OK);
        const char *at = strstr(d.sentence, c.at);
        TEST_ASSERT(at != NULL);
        TEST_ASSERT(vjo_overlay_lookup_at(&a, &p, &cfg, (size_t)(at - d.sentence), &d) == VJO_OK);
        TEST_ASSERT_(d.list.n_entries == 1, "no entry at %s", c.at);
        const VjoEntry *e = &d.list.entries[0];
        const VjoVocab *v = e->vocab;
        std::string span(d.sentence + e->hl_start, (size_t)(e->hl_end - e->hl_start));
        const char *reading = strcmp(v->reading, v->spelling) ? v->reading : "-";
        TEST_CHECK_(span == c.span, "span %s, expected %s", span.c_str(), c.span);
        TEST_CHECK_(!strcmp(v->spelling, c.spelling) && !strcmp(reading, c.reading), "entry %s (%s)", v->spelling,
                    reading);
        TEST_CHECK_(strstr(v->meanings[0], c.contains) != NULL, "%s: %s", c.contains, v->meanings[0]);
        TEST_CHECK(v->n_meanings == 1 && !label_line(v->meanings[0]) && !strstr(v->meanings[0], "Plain]"));
        TEST_CHECK(v->dictionary && !strcmp(v->dictionary, kTitle));
        TEST_CHECK(d.list.source && !strcmp(d.list.source, "Jiten"));
        TEST_CHECK_(kanji_lines(v->meanings[0]) == c.lines, "kanji lines of %s: %s", v->spelling,
                    v->meanings[0]);
        /* The popup: a blank line after the definition, then each kanji line
         * as written, its kanji at ja px and the rest at en px, in text order,
         * two spans a line. */
        VjoStyled st;
        TEST_ASSERT(vjo_styled_init(&st, &a, VJO_BODY_UNITS, VJO_BODY_SPANS) == 0);
        vjo_styled_entry(&st, &d.list, 0, 27, 21, 0, 1);
        std::u16string shown = styled_text(st), want = c.lines.empty() ? u"" : u"\n";
        for (auto &l : c.lines) want += u16("\n" + l.first + "=" + l.second);
        TEST_CHECK(!st.truncated && shown.size() >= want.size() &&
                   shown.compare(shown.size() - want.size(), want.size(), want) == 0);
        int kanji_spans = 0;
        for (int i = 0; i < st.n_spans; i++)
            if (st.spans[i].px == 27 && st.spans[i].rgb == VJO_RGB_TEXT) {
                std::u16string k(shown, st.spans[i].start, st.spans[i].len);
                TEST_CHECK(kanji_spans < (int)c.lines.size() && k == u16(c.lines[kanji_spans].first));
                kanji_spans++;
            }
        TEST_CHECK(kanji_spans == (int)c.lines.size());
        TEST_CHECK(opens == closes);
    }
}

/* Every entry of the bundled dictionary, as the popup body shows it when
 * everything else in the body is at its longest: nothing is truncated, the
 * room for the config warnings after it is kept, every kanji line is shown
 * whole and, when a definition has more than one line, so is its first (an
 * appended grammar summary, which the bundle puts first). Over-long ones are
 * clipped at the end of their definition and marked; the rest show their text
 * as written. Reads the file's index itself (format 3,
 * docs/local-dictionaries.md), every payload once. */
static void every_bundled_entry_fits_the_popup(void)
{
    std::string path = std::string(dict_dir) + "/plaintext.vjdict";
    FILE *f = fopen(path.c_str(), "rb");
    TEST_ASSERT(f != NULL);
    std::vector<uint8_t> file;
    {
        uint8_t buf[1 << 16];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) file.insert(file.end(), buf, buf + n);
        fclose(f);
    }
    auto u32 = [&](size_t o) { return (uint32_t)file[o] | (uint32_t)file[o + 1] << 8 | (uint32_t)file[o + 2] << 16 |
                                      (uint32_t)file[o + 3] << 24; };
    auto u64 = [&](size_t o) { return (uint64_t)u32(o) | (uint64_t)u32(o + 4) << 32; };
    TEST_ASSERT(file.size() > 8192 && !memcmp(file.data(), "VJDICT3\0", 8));
    TEST_CHECK(!memcmp(file.data() + 192, kTitle, sizeof(kTitle)));
    uint64_t pages = u64(56);
    std::vector<std::pair<uint64_t, uint32_t>> payloads;
    for (uint64_t page = 1; page <= pages; page++) {
        size_t o = (size_t)(page * 4096);
        TEST_ASSERT(!memcmp(file.data() + o, "VJHP", 4));
        for (uint32_t k = 0, count = u32(o + 4); k < count; k++) {
            size_t slot = o + 16 + k * 24, group = (size_t)u64(slot + 8);
            uint32_t n = u32(slot + 16), key_len = (uint32_t)(file[slot + 20] | file[slot + 21] << 8);
            for (uint32_t j = 0; j < n; j++) {
                size_t post = group + key_len + j * 16;
                payloads.push_back({u64(post), u32(post + 8)});
            }
        }
    }
    std::sort(payloads.begin(), payloads.end());
    payloads.erase(std::unique(payloads.begin(), payloads.end()), payloads.end());
    static uint8_t mem[64 * 1024];
    unsigned long entries = 0, with_lines = 0, lines_shown = 0, clipped = 0, firsts_kept = 0, failures = 0;
    unsigned long typical_clipped = 0, misordered = 0;
    int most_spans = 0;
    for (auto &pl : payloads) {
        const char *payload = (const char *)file.data() + pl.first + 4; /* after the frequency word */
        const char *spelling = payload, *reading = spelling + strlen(spelling) + 1, *text = reading + strlen(reading) + 1;
        VjoVocab v = VjoVocab();
        VjoEntry e = VjoEntry();
        VjoEntryList l = VjoEntryList();
        VjoArena a;
        VjoStyled st;
        v.spelling = spelling; v.reading = *reading ? reading : spelling; v.rank = (int)(u32((size_t)pl.first) & 0xffffff);
        v.meanings = &text; v.n_meanings = 1; v.dictionary = kTitle;
        e.vocab = &v; e.pos16 = e.hl_start = e.hl_end = -1;
        l.header = ""; l.entries = &e; l.n_entries = 1; l.source = "Jiten";
        /* with nothing else in the body: is it shown as written? */
        vjo_arena_init(&a, mem, sizeof(mem));
        TEST_ASSERT(vjo_styled_init(&st, &a, VJO_BODY_UNITS, VJO_BODY_SPANS) == 0);
        vjo_styled_entry(&st, &l, 0, 27, 21, 0, 1);
        typical_clipped += st.len < 1 + units_of_entry(&v);
        vjo_arena_init(&a, mem, sizeof(mem));
        worst_case_body(&st, &a, &l, 27, 21);
        entries++;
        auto lines = kanji_lines(text);
        /* the block is exactly the spelling's kanji lines (the bundle's own shape) */
        {
            std::vector<std::string> kanji = distinct_kanji(spelling), got;
            for (auto &k : lines) got.push_back(k.first);
            size_t at = 0;
            for (auto &k : got) {
                while (at < kanji.size() && kanji[at] != k) at++;
                if (at == kanji.size()) break;
                at++;
            }
            if (at == kanji.size() && !got.empty() && got.back() != kanji.back()) at = 0;
            bool ordered = true;
            for (size_t i = 0, j = 0; i < got.size(); i++) {
                while (j < kanji.size() && kanji[j] != got[i]) j++;
                ordered = ordered && j < kanji.size();
                j++;
            }
            if (!ordered && misordered++ < 4)
                TEST_CHECK_(ordered, "%s: kanji lines not the spelling's, in order", spelling);
        }
        /* the definition, without the newline its kanji lines follow; then a blank line */
        size_t block = vjo_kanji_block(text), dlen = block;
        if (block < strlen(text))
            while (dlen && text[dlen - 1] == '\n') dlen--;
        std::string definition(text, dlen);
        std::u16string shown = styled_text(st), all = dlen && !lines.empty() ? u"\n" : u"", first;
        for (auto &k : lines) all += u16("\n" + k.first + "=" + k.second);
        if (definition.find('\n') != std::string::npos) first = u16("\n" + definition.substr(0, definition.find('\n') + 1));
        std::u16string whole = (dlen ? u16("\n" + definition) : std::u16string()) + all;
        /* the entry ends before the four warnings (4 x 108 units) */
        size_t end = shown.size() >= 4 * (1 + 12 + 95) ? shown.size() - 4 * (1 + 12 + 95) : 0;
        auto ends_with = [&](const std::u16string &x) {
            return end >= x.size() && shown.compare(end - x.size(), x.size(), x) == 0;
        };
        bool ok = !st.truncated && st.n_spans <= VJO_BODY_SPANS && ends_with(all), cut = !ends_with(whole);
        /* a clip never splits a character: no replacement character the text has not */
        ok = ok && (shown.find(u'\uFFFD') == std::u16string::npos || strstr(text, "\xEF\xBF\xBD"));
        if (cut) {
            /* clipped: marked, the kanji lines and a first line before a later one kept */
            ok = ok && shown.find(u'\u2026') != std::u16string::npos &&
                 (first.empty() || shown.find(first) != std::u16string::npos);
            clipped++;
            firsts_kept += !first.empty();
        }
        with_lines += !lines.empty();
        lines_shown += lines.size();
        if (st.n_spans > most_spans) most_spans = st.n_spans;
        if (!ok && failures++ < 8)
            TEST_CHECK_(ok, "%s (%s): %u units, %d spans, truncated %d", spelling, reading, st.len, st.n_spans,
                        st.truncated);
    }
    TEST_CHECK_(failures == 0, "%lu entries do not fit", failures);
    TEST_CHECK_(misordered == 0, "%lu entries' kanji lines are not their spelling's", misordered);
    TEST_CHECK(entries > 480000 && with_lines > 360000);
    printf("\n  %lu entries (%lu with kanji lines, %lu lines); clipped in their definition: %lu alone in the body, "
           "%lu with the longest status and warnings (%lu of them keeping a first line of several whole); at most %d "
           "spans of %d\n", entries, with_lines, lines_shown, typical_clipped, clipped, firsts_kept, most_spans,
           VJO_BODY_SPANS);
}

/* A real text box's lines, '\n' between them (the fixtures escape it). */
static std::string unescape(const std::string &s)
{
    std::string t;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') { t += '\n'; i++; }
        else t += s[i];
    }
    return t;
}

/* Words a text box's wrapping split over two lines (host/fixtures/cross_lines,
 * owner 2026-10-16 item 4: boxes of vita-test and psp-test, and synthetic ones
 * in the layouts of the real pool's): the lookup at the word's first character
 * shows the whole word, its highlight running across the break, and the arrows
 * step over it. Every row is checked: one without an entry does not stop the rest. */
static void split_words_are_looked_up_whole(void)
{
    VjoConfig cfg; VjoPlatform p; PosixPlatform pp;
    int n = 0;
    setup(&cfg, &p, &pp);
    for (auto &r : read_tsv("../cross_lines/split_words.tsv")) {
        VjoArena a; VjoOverlayData d;
        TEST_ASSERT(r.size() == 5);
        std::string text = unescape(r[2]);
        size_t cursor = 0, cp = 0, len = text.size();
        TEST_CASE_("%s %s", r[0].c_str(), r[4].c_str());
        while (cursor < len && cp < (size_t)atoi(r[3].c_str())) { vjo_utf8_next(text.c_str(), len, &cursor); cp++; }
        vjo_arena_init(&a, arena_mem, sizeof(arena_mem));
        TEST_ASSERT(vjo_overlay_ocr_text(&a, &cfg, text.c_str(), &d) == VJO_OK);
        TEST_ASSERT(!strcmp(d.sentence, text.c_str())); /* every line Japanese, nothing trimmed */
        n++;
        TEST_ASSERT(vjo_overlay_lookup_at(&a, &p, &cfg, cursor, &d) == VJO_OK);
        if (!TEST_CHECK_(d.list.n_entries == 1, "no entry at the cursor"))
            continue;
        const VjoEntry *e = &d.list.entries[0];
        std::string span(d.sentence + e->hl_start, (size_t)(e->hl_end - e->hl_start));
        TEST_CHECK_(span.find('\n') != std::string::npos, "the span %s ends before the break", span.c_str());
        TEST_CHECK_(r[4] == e->vocab->spelling, "entry %s, expected %s", e->vocab->spelling, r[4].c_str());
        check_header_range(&d, (size_t)e->hl_start, (size_t)e->hl_end);
        /* The arrows (#173's rule, the overlay's own key function): Right skips the joined word
         * to the first Japanese letter after its tail, on the next line, and Left comes back; the
         * same before the header is laid out and on a 920 px layout of its rows. */
        {
            static Layout lay;
            size_t want = (size_t)e->hl_end, to = 0;
            while (want < len) { /* the first letter the arrows select at or after the word's end */
                size_t at = want;
                uint32_t c = vjo_utf8_next(d.sentence, len, &want);
                if (vjo_text_selectable(c, want - at, 1)) { want = at; break; }
            }
            layout_text(&lay, d.sentence, 920.0f, 27.0f, 13.5f);
            VjoTextRows rows = layout_rows(&lay);
            for (int laid = 0; laid < 2; laid++) {
                VjoRowNav nav = VjoRowNav();
                vjo_text_row_key(&nav, d.sentence, len, laid ? &rows : NULL, cursor, (size_t)e->hl_end, 1, 0, 1, 0, &to);
                if (want >= len) {
                    TEST_CHECK_(to == cursor, "Right at the text's end moved to %zu", to);
                    continue;
                }
                TEST_CHECK_(to == want, "Right from %zu: %zu, expected %zu (layout %d)", cursor, to, want, laid);
                TEST_CHECK(memchr(d.sentence + cursor, '\n', to - cursor) != NULL); /* onto the next line */
                vjo_text_row_key(&nav, d.sentence, len, laid ? &rows : NULL, to, 0, -1, 0, 1, 0, &to);
                TEST_CHECK_(to == cursor, "Left came back to %zu, not %zu (layout %d)", to, cursor, laid);
            }
        }
    }
    TEST_CHECK(n == 77); /* 56 synthetic, 21 of vita-test and psp-test */
}

/* Breaks after a speaker's name or at a sentence's end stay breaks. */
static void kept_breaks_stay_breaks(void)
{
    int n = 0;
    for (auto &r : read_tsv("../cross_lines/kept_breaks.tsv")) {
        TEST_ASSERT(r.size() == 5);
        std::string text = unescape(r[2]);
        size_t at = std::string::npos;
        for (int k = 0; k <= atoi(r[3].c_str()); k++) at = text.find('\n', at + 1);
        TEST_CASE_("%s %s", r[0].c_str(), r[4].c_str());
        TEST_ASSERT(at != std::string::npos);
        TEST_CHECK(!vjo_text_joins(text.c_str(), text.size(), at));
        n++;
    }
    TEST_CHECK(n == 54); /* 32 synthetic, 22 of vita-test and psp-test */
}

TEST_LIST = {
    {"preferred_readings_exist_in_the_bundle", preferred_readings_exist_in_the_bundle},
    {"usual_spellings_win_in_the_bundle", usual_spellings_win_in_the_bundle},
    {"bundled_dictionary_deinflects", bundled_dictionary_deinflects},
    {"real_lines_walk_word_by_word", real_lines_walk_word_by_word},
    {"expected_entries_at_real_lines", expected_entries_at_real_lines},
    {"touch_and_left_right_follow_the_words", touch_and_left_right_follow_the_words},
    {"real_lines_navigate_displayed_rows", real_lines_navigate_displayed_rows},
    {"ultimate_dictionary_fixture_entries", ultimate_dictionary_fixture_entries},
    {"every_bundled_entry_fits_the_popup", every_bundled_entry_fits_the_popup},
    {"split_words_are_looked_up_whole", split_words_are_looked_up_whole},
    {"kept_breaks_stay_breaks", kept_breaks_stay_breaks},
    {NULL, NULL}};

/* acutest's main, behind the dictionary check (skip without the bundle). */
namespace {
struct Skip {
    Skip()
    {
        dict_dir = getenv("VJO_PLAINTEXT_DIR");
        std::string path = dict_dir ? std::string(dict_dir) + "/plaintext.vjdict" : "";
        FILE *f = dict_dir ? fopen(path.c_str(), "rb") : NULL;
        if (f) { fclose(f); return; }
        if (getenv("VJO_REQUIRE_PLAINTEXT")) { fprintf(stderr, "plaintext.vjdict required but missing\n"); exit(1); }
        printf("SKIP: no plaintext.vjdict (set VJO_PLAINTEXT_DIR, or configure with VJO_PLAINTEXT_SOURCE)\n");
        exit(77);
    }
} skip_without_dictionary;
}
