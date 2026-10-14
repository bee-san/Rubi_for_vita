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

/* Every real line, word by word with Right as the Shell navigates: one entry
 * or none per lookup, the highlight starting at the cursor and ending on a
 * character boundary, the popup naming the entry, Right always advancing,
 * nothing left allocated between lookups and every file closed. */
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
            size_t next = vjo_word_cursor_step(nav, s, len, cursor, end, 1, cfg.japanese_only_selection);
            if (next == cursor) break;
            TEST_CHECK_(next > cursor && (!end || next >= end), "%s: Right from %zu to %zu (match end %zu)",
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

TEST_LIST = {
    {"bundled_dictionary_deinflects", bundled_dictionary_deinflects},
    {"real_lines_walk_word_by_word", real_lines_walk_word_by_word},
    {"expected_entries_at_real_lines", expected_entries_at_real_lines},
    {"touch_and_left_right_follow_the_words", touch_and_left_right_follow_the_words},
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
