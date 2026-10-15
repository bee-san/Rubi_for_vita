/* End-to-end word lookup: real VOCR output (host/fixtures/word_lookup) -> the
 * overlay's sentence -> longest match in the bundled plaintext.vjdict
 * (Daijirin4 -> Sankoku8 -> JMdict) -> the highlighted span -> the popup
 * entry, with the Shell's own word navigation (Right/Left) and touch hit
 * test. The production code runs unchanged; only file reads are counted.
 *
 * VJO_PLAINTEXT_DIR names the directory holding plaintext.vjdict (CMake
 * prepares it from the pinned archive, tools/prepare_plaintext.py). Without
 * it the test is skipped (exit 77) unless VJO_REQUIRE_PLAINTEXT is set. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include "acutest.h"
extern "C" {
#include "client.h"
#include "local_dict.h"
#include "net_posix.h"
#include "styled.h"
#include "text_hit.h"
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
    vjo_config_defaults(cfg); /* the release defaults: word highlight, Japanese-only, one result */
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

} // namespace

/* Every real line, word by word with Right as the Shell navigates before
 * the header is laid out (the whole text one row): one entry or none per
 * lookup, the highlight starting at the cursor and ending on a character
 * boundary, the popup naming the entry, Right advancing until it wraps from
 * the last word, nothing left allocated between lookups and every file
 * closed. */
static void real_lines_walk_word_by_word(void)
{
    VjoConfig cfg; VjoPlatform p; PosixPlatform pp;
    unsigned long lookups = 0, matched = 0, single = 0, none = 0, letters = 0, covered = 0, max_reads = 0;
    double total_us = 0, max_us = 0;
    load(); setup(&cfg, &p, &pp);
    TEST_CHECK(cfg.highlight_mode == VJO_HIGHLIGHT_WORD && cfg.japanese_only_selection && cfg.ocr_mode == VJO_OCR_AUTO);
    for (auto &line : lines) {
        VjoArena a; VjoOverlayData d;
        vjo_arena_init(&a, arena_mem, sizeof(arena_mem));
        TEST_ASSERT(vjo_overlay_ocr_text(&a, &cfg, line.text.c_str(), &d) == VJO_OK);
        const char *s = d.sentence;
        size_t len = strlen(s), mark = vjo_arena_mark(&a);
        /* The Shell's first selection: the first Japanese letter (worker.c). */
        size_t cursor = vjo_utf8_japanese_cursor_step(s, len, 0, 0);
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
            TEST_CHECK(row == VJO_ROW_UNAVAILABLE || next == cursor);
            if (next <= cursor) break; /* the last word: Right wraps to the first */
            TEST_CHECK_(!end || next >= end, "%s: Right from %zu to %zu (match end %zu)",
                        line.id.c_str(), cursor, next, end);
            vjo_arena_release(&a, mark); /* the Shell releases the shown entry before the next cursor */
            cursor = next;
        }
    }
    printf("\n  %zu real VOCR lines, %lu lookups: %lu entries (%lu one character), %lu without; "
           "%lu of %lu Japanese letters inside a matched word (%.1f%%)\n"
           "  host lookup %.1f us mean, %.1f us max; at most %lu file reads per lookup\n",
           lines.size(), lookups, matched, single, none, covered, letters, 100.0 * covered / (letters ? letters : 1),
           total_us / (lookups ? lookups : 1), max_us, max_reads);
}

/* The arrow keys over displayed rows of every real line (#81), laid out as
 * the header is on the console (font_size_ja 18: 27 px cells in the 920 px
 * box), with each region's lines also joined into one long line (soft wraps
 * at 34 cells) and in a 270 px box (soft wraps every 10 cells). Whole word
 * Right, with the real lookups' match ends, stays on its row and comes back
 * to the row's first word; Left retraces those words, the wrap first. Up and
 * Down from every letter land where the rule restated over the cells
 * (layout_double.h) says. */
static void real_lines_navigate_displayed_rows(void)
{
    VjoConfig cfg; VjoPlatform p; PosixPlatform pp;
    static Layout lay;
    unsigned long rows_seen = 0, wrapped_rows = 0, words = 0, verticals = 0, lookups_n = 0, long_rows = 0;
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
            for (uint32_t row = 0; row < lay.lines; row++) {
                int first = -1;
                for (unsigned k = 0; k < lay.n && first < 0; k++)
                    if (lay.line[k] == row && layout_selectable(&lay, k, 1)) first = (int)k;
                if (first < 0) continue;
                rows_seen++;
                VjoWordNavigation nav = VjoWordNavigation();
                std::vector<size_t> visited;
                size_t cursor = lay.begin[first];
                int rc = VJO_ROW_STAY;
                for (int guard = 0;; guard++) {
                    TEST_ASSERT_(guard < 100, "row %u does not come back", row);
                    vjo_arena_release(&a, mark);
                    TEST_ASSERT(vjo_overlay_lookup_at(&a, &p, &cfg, cursor, &d) == VJO_OK);
                    lookups_n++;
                    size_t end = d.list.n_entries ? (size_t)d.list.entries[0].hl_end : 0;
                    visited.push_back(cursor);
                    size_t next = vjo_word_cursor_step(nav, s, len, cursor, end, 1, 1, &rows, &rc);
                    TEST_ASSERT(rc >= 0);
                    if (rc == VJO_ROW_STAY) break;
                    TEST_CHECK_(lay.line[layout_at(&lay, next)] == row, "Right left row %u", row);
                    if (rc == VJO_ROW_WRAPPED) {
                        TEST_CHECK(next == lay.begin[first]);
                        wrapped_rows++;
                        cursor = next;
                        break;
                    }
                    TEST_CHECK(next > cursor && (!end || next >= end));
                    cursor = next;
                    words++;
                }
                /* Left undoes the last 16 Rights, newest first: the wrap, then
                 * each word; past those it steps one character on the row. */
                if (rc == VJO_ROW_WRAPPED) {
                    size_t kept = visited.size() < 16 ? 0 : visited.size() - 16;
                    for (size_t k = visited.size(); k-- > kept;) {
                        cursor = vjo_word_cursor_step(nav, s, len, cursor, 0, -1, 1, &rows, &rc);
                        TEST_CHECK_(cursor == visited[k] && rc == VJO_ROW_MOVED, "Left to %zu, expected %zu",
                                    cursor, visited[k]);
                    }
                    int wrapped;
                    size_t want = ref_step(&lay, cursor, cursor + 1, -1, 1, &wrapped);
                    cursor = vjo_word_cursor_step(nav, s, len, cursor, 0, -1, 1, &rows, &rc);
                    TEST_CHECK(cursor == want && rc == (wrapped ? VJO_ROW_WRAPPED : VJO_ROW_MOVED));
                    long_rows += kept > 0;
                }
                TEST_CHECK(nav.count == 0);
            }
            /* Up/Down from every letter, with no column and with the one kept. */
            for (unsigned k = 0; k < lay.n; k++) {
                if (!layout_selectable(&lay, k, 1)) continue;
                for (int dy = -1; dy <= 1; dy += 2) {
                    VjoRowGoal goal = {0, 0};
                    size_t out = lay.begin[k], want = ref_vertical(&lay, lay.begin[k], dy, 1, -1.0f);
                    vjo_text_row_vertical(s, len, &rows, lay.begin[k], dy, 1, &goal, &out);
                    TEST_CHECK_(out == want, "byte %zu dy %d: %zu, expected %zu", lay.begin[k], dy, out, want);
                    if (out != lay.begin[k]) {
                        float column = goal.x;
                        size_t again = out, want2 = ref_vertical(&lay, out, dy, 1, column);
                        vjo_text_row_vertical(s, len, &rows, out, dy, 1, &goal, &again);
                        TEST_CHECK_(again == want2, "byte %zu kept column: %zu, expected %zu", out, again, want2);
                    }
                    verticals++;
                }
            }
            TEST_CHECK(lay.bad_queries == 0 && opens == closes);
        }
    }
    printf("\n  %zu real lines x 3 layouts: %lu rows (%lu wrap back to their first word, %lu of them over 16 words; "
           "%lu words, %lu lookups), %lu Up/Down checks\n", lines.size(), rows_seen, wrapped_rows, long_rows, words,
           lookups_n, verticals);
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
        /* The popup text: "spelling (reading) rank\n[source]\ndefinition". */
        TEST_CHECK(strstr(e->text, "\n[") != NULL);
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
    static const char *const forms[][3] = {
        {"やめて", "やめて", "止める"}, {"やめろ", "やめろ", "止める"}, {"やめなさい", "やめなさい", "止める"},
        {"おじさんが", "おじさん", "小父さん"}, {"せいで", "せい", "所為"}, {"なくなった", "なくなった", "無くなる"},
        {"なくして", "なくして", "無くして"}, {"によらず", "によらず", "に依らず"}};
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

TEST_LIST = {
    {"preferred_readings_exist_in_the_bundle", preferred_readings_exist_in_the_bundle},
    {"usual_spellings_win_in_the_bundle", usual_spellings_win_in_the_bundle},
    {"bundled_dictionary_deinflects", bundled_dictionary_deinflects},
    {"real_lines_walk_word_by_word", real_lines_walk_word_by_word},
    {"expected_entries_at_real_lines", expected_entries_at_real_lines},
    {"touch_and_left_right_follow_the_words", touch_and_left_right_follow_the_words},
    {"real_lines_navigate_displayed_rows", real_lines_navigate_displayed_rows},
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
