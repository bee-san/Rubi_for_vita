/* The config.txt editor (core/taicfg.c) on the fixtures of tools/tests/fixtures/taicfg/ (made by
 * tools/tests/make_taicfg_fixtures.py; their expected results checked by hand and written by
 * vjo-taicfg expect), and on a seeded corpus of generated configurations. Every result is also
 * checked inside the editor with taiHEN's own parser (third_party/taihen-parser); these tests check
 * the bytes, the reports, idempotence and what an uninstall leaves. tools/tests/test_tai_config.py
 * runs the PC installer's copy on the same files and compares the two. */
#include "acutest.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <taihen/parser.h>
#include "taicfg.h"

#ifndef VJO_SOURCE_DIR
#define VJO_SOURCE_DIR "."
#endif
#define FIXTURES VJO_SOURCE_DIR "/tools/tests/fixtures/taicfg"

static char in[VJO_TAI_TEXT_MAX + 2], out[VJO_TAI_TEXT_MAX + 1], again[VJO_TAI_TEXT_MAX + 1], want[VJO_TAI_TEXT_MAX + 2];

static long slurp(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (long)n;
}

static int edit(const char *text, size_t n, int op, int mode, const char *const *titles, int nt, char *dst,
                VjoTaiReport *r)
{
    return vjo_tai_edit(text, n, op, mode, titles, nt, dst, VJO_TAI_TEXT_MAX + 1, r);
}

/* No line of ours that taiHEN loads, for KERNEL, main and title. */
static int nothing_of_ours(const char *text, const char *title)
{
    const char *titles[] = {"KERNEL", "main", title};
    for (int i = 0; i < 3; i++) {
        VjoTaiLoads l;
        vjo_tai_loads(text, titles[i], &l);
        if (l.kernel || l.shell || l.worker || l.text) return 0;
    }
    return 1;
}

/* One fixture's edit against its expected file; then the same edit of the result changes nothing. */
static void expect_edit(const char *name, long n, int op, int mode, const char *const *titles, int nt,
                        const char *suffix)
{
    char path[512];
    VjoTaiReport r;
    int rc = edit(in, (size_t)n, op, mode, titles, nt, out, &r);
    snprintf(path, sizeof(path), FIXTURES "/%s.%s", name, suffix);
    long w = slurp(path, want, sizeof(want));
    TEST_CHECK_(w >= 0, "%s: %s exists", name, suffix);
    TEST_CHECK_(rc == w && !memcmp(out, want, (size_t)(rc > 0 ? rc : 0)), "%s %s: the result is %s's (%d vs %ld B)",
                name, suffix, path, rc, w);
    if (rc < 0) return;
    TEST_CHECK_(r.changed == (rc != n || memcmp(out, in, (size_t)n)), "%s %s: changed", name, suffix);
    int again_rc = edit(out, (size_t)rc, op, mode, titles, nt, again, &r);
    TEST_CHECK_(again_rc == rc && !memcmp(again, out, (size_t)rc) && !r.changed, "%s %s: idempotent", name, suffix);
    /* a changed result never has the length taiHEN reads past (an unchanged file is not written) */
    TEST_CHECK_(!r.changed || rc % 4096 != 0, "%s %s: no length taiHEN reads past", name, suffix);
}

static void test_fixtures(void)
{
    DIR *d = opendir(FIXTURES);
    struct dirent *e;
    int files = 0, refused = 0;
    TEST_ASSERT(d != NULL);
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        char name[256], path[512], titles_text[1024], *titles[16];
        int nt = 0;
        if (len < 4 || strcmp(e->d_name + len - 3, ".in")) continue;
        snprintf(name, sizeof(name), "%.*s", (int)(len - 3), e->d_name);
        snprintf(path, sizeof(path), FIXTURES "/%s", e->d_name);
        long n = slurp(path, in, sizeof(in));
        TEST_ASSERT(n >= 0);
        snprintf(path, sizeof(path), FIXTURES "/%s.titles", name);
        if (slurp(path, titles_text, sizeof(titles_text)) > 0)
            for (char *s = strtok(titles_text, "\n"); s && nt < 16; s = strtok(NULL, "\n")) titles[nt++] = s;
        snprintf(path, sizeof(path), FIXTURES "/%s.refused", name);
        FILE *f = fopen(path, "rb");
        if (f) {
            int rc = 0, line = -1;
            TEST_CHECK(fscanf(f, "rc=%d line=%d", &rc, &line) == 2);
            fclose(f);
            VjoTaiReport r;
            for (int op = 0; op < 2; op++)
                for (int mode = 0; mode < 2; mode++) {
                    int got = edit(in, (size_t)n, op, mode, NULL, 0, out, &r);
                    /* a check refusal is the install's (uninstalling such a file has nothing to add) */
                    if (rc == VJO_TAI_E_CHECK && op == VJO_TAI_UNINSTALL) continue;
                    TEST_CHECK_(got == rc && r.line == line && r.why[0], "%s: refused (%d, line %d) in op %d mode %d",
                                name, got, r.line, op, mode);
                }
            refused++;
            files++;
            continue;
        }
        expect_edit(name, n, VJO_TAI_INSTALL, VJO_TAI_WORKER_KERNEL, NULL, 0, "kernel.out");
        expect_edit(name, n, VJO_TAI_INSTALL, VJO_TAI_WORKER_LINES, (const char *const *)titles, nt, "lines.out");
        expect_edit(name, n, VJO_TAI_UNINSTALL, VJO_TAI_WORKER_KERNEL, NULL, 0, "uninstall.out");
        /* uninstalling an install leaves nothing of ours that taiHEN loads, in either mode */
        for (int mode = 0; mode < 2; mode++) {
            VjoTaiReport r;
            int rc = edit(in, (size_t)n, VJO_TAI_INSTALL, mode, (const char *const *)titles, nt, out, &r);
            TEST_ASSERT(rc >= 0);
            int u = edit(out, (size_t)rc, VJO_TAI_UNINSTALL, mode, NULL, 0, again, &r);
            TEST_CHECK_(u >= 0 && nothing_of_ours(again, nt ? titles[0] : "PCSG00415"), "%s mode %d: uninstalled", name, mode);
        }
        files++;
    }
    closedir(d);
    TEST_CHECK_(files >= 38 && refused >= 5, "%d fixtures, %d refused", files, refused);
}

/* HENkaku's stock ux0 file (henkaku 784e483 bootstrap.c:53-69), installed: one kernel line for
 * KERNEL, one Shell line for main, the worker for nobody; and uninstalled: back byte for byte. */
static const char stock[] =
    "# For users plugins, you must refresh taiHEN from HENkaku Settings for\n# changes to take place.\n"
    "# For kernel plugins, you must reboot for changes to take place.\n*KERNEL\n"
    "# henkaku.skprx is hard-coded to load and is not listed here\n*main\n# main is a special titleid for SceShell\n"
    "ur0:tai/henkaku.suprx\n*NPXS10015\n# this is for modifying the version string\nur0:tai/henkaku.suprx\n"
    "*NPXS10016\n# this is for modifying the version string in settings widget\nur0:tai/henkaku.suprx\n";

static void test_stock(void)
{
    VjoTaiReport r;
    VjoTaiLoads l;
    int rc = edit(stock, sizeof(stock) - 1, VJO_TAI_INSTALL, VJO_TAI_WORKER_KERNEL, NULL, 0, out, &r);
    TEST_ASSERT(rc > 0);
    TEST_CHECK(r.changed && r.added == 2 && !r.sections_added && !r.commented && !r.removed && !r.padded);
    TEST_CHECK(strstr(out, "*KERNEL\n" VJO_TAI_KERNEL "\n# henkaku.skprx") != NULL);
    TEST_CHECK(strstr(out, "*main\n" VJO_TAI_SHELL "\n# main is") != NULL);
    vjo_tai_loads(out, "KERNEL", &l);
    TEST_CHECK(l.kernel == 1 && !l.shell && !l.worker);
    vjo_tai_loads(out, "main", &l);
    TEST_CHECK(l.shell == 1 && !l.kernel && !l.worker);
    vjo_tai_loads(out, "PCSG00415", &l);
    TEST_CHECK(!l.kernel && !l.shell && !l.worker);
    TEST_CHECK(vjo_tai_has_kernel_line(out, (size_t)rc) && !vjo_tai_has_kernel_line(stock, sizeof(stock) - 1));
    int u = edit(out, (size_t)rc, VJO_TAI_UNINSTALL, VJO_TAI_WORKER_KERNEL, NULL, 0, again, &r);
    TEST_CHECK(u == (int)sizeof(stock) - 1 && !memcmp(again, stock, sizeof(stock) - 1) && r.removed == 2);
    /* the stock file needs nothing for an uninstall: unchanged */
    u = edit(stock, sizeof(stock) - 1, VJO_TAI_UNINSTALL, VJO_TAI_WORKER_KERNEL, NULL, 0, again, &r);
    TEST_CHECK(u == (int)sizeof(stock) - 1 && !r.changed);
}

/* An older install's worker lines go in kernel mode, each named in the report. */
static void test_report(void)
{
    static const char test3[] = "*KERNEL\n" VJO_TAI_KERNEL "\n*main\n" VJO_TAI_SHELL "\n*ALL\nur0:tai/Every.suprx\n"
                                VJO_TAI_WORKER "\n\n*PCSG00415\n" VJO_TAI_WORKER "\n\n*PSPEMUCFW\n" VJO_TAI_WORKER "\n";
    VjoTaiReport r;
    int rc = edit(test3, sizeof(test3) - 1, VJO_TAI_INSTALL, VJO_TAI_WORKER_KERNEL, NULL, 0, out, &r);
    TEST_ASSERT(rc > 0);
    TEST_CHECK(r.removed == 3 && r.headers_removed == 2 && !r.added);
    TEST_CHECK_(!strcmp(r.removed_sections, "*ALL, *PCSG00415, *PSPEMUCFW"), "%s", r.removed_sections);
    TEST_CHECK(!strcmp(out, "*KERNEL\n" VJO_TAI_KERNEL "\n*main\n" VJO_TAI_SHELL "\n*ALL\nur0:tai/Every.suprx\n"));
    /* lines mode keeps the games registered before and comments out the *ALL line */
    rc = edit(test3, sizeof(test3) - 1, VJO_TAI_INSTALL, VJO_TAI_WORKER_LINES, NULL, 0, out, &r);
    TEST_ASSERT(rc > 0);
    TEST_CHECK(r.commented == 1 && !r.removed && strstr(out, "*ALL\nur0:tai/Every.suprx\n" VJO_TAI_DISABLED VJO_TAI_WORKER "\n"));
    TEST_CHECK(strstr(out, "\n*PCSG00415\n" VJO_TAI_WORKER "\n") && strstr(out, "\n*PSPEMUCFW\n" VJO_TAI_WORKER "\n"));
    /* a byte-order mark before the first header */
    static const char bom[] = "\xEF\xBB\xBF*KERNEL\nur0:tai/a.skprx\n*main\nur0:tai/henkaku.suprx\n";
    rc = edit(bom, sizeof(bom) - 1, VJO_TAI_INSTALL, VJO_TAI_WORKER_KERNEL, NULL, 0, out, &r);
    TEST_CHECK(rc > 0 && r.bom_hides_section && r.sections_added == 1);
}

static void test_refusals(void)
{
    static char big[VJO_TAI_TEXT_MAX + 2];
    VjoTaiReport r;
    /* not a title ID, or one the worker never serves (a system app, VitaShell, the Overlay app) */
    const char *bad_titles[] = {"ALL", "main", "NPXS10015", "VITASHELL", "VJOSET001", "PCSG0041", "PCSG 0415"};
    memset(big, '#', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    TEST_CHECK(edit(big, VJO_TAI_TEXT_MAX + 1, VJO_TAI_INSTALL, VJO_TAI_WORKER_KERNEL, NULL, 0, out, &r) ==
               VJO_TAI_E_TOO_LARGE && strstr(r.why, "64 KiB"));
    TEST_CHECK(edit(stock, sizeof(stock) - 1, 7, VJO_TAI_WORKER_KERNEL, NULL, 0, out, &r) == VJO_TAI_E_ARGS);
    TEST_CHECK(edit(stock, sizeof(stock) - 1, VJO_TAI_INSTALL, 7, NULL, 0, out, &r) == VJO_TAI_E_ARGS);
    TEST_CHECK(vjo_tai_edit(stock, sizeof(stock) - 1, VJO_TAI_INSTALL, 0, NULL, 0, out, 40, &r) == VJO_TAI_E_SPACE);
    for (unsigned i = 0; i < sizeof(bad_titles) / sizeof(bad_titles[0]); i++) {
        const char *t[] = {bad_titles[i]};
        TEST_CHECK_(edit(stock, sizeof(stock) - 1, VJO_TAI_INSTALL, VJO_TAI_WORKER_LINES, t, 1, out, &r) ==
                    VJO_TAI_E_TITLE, "%s", bad_titles[i]);
        TEST_CHECK_(vjo_tai_title_ok(bad_titles[i]) == (strlen(bad_titles[i]) == 9 && !strchr(bad_titles[i], ' ')),
                    "%s", bad_titles[i]);
    }
    /* kernel mode ignores titles; lines mode takes them in any case, with a '*' */
    const char *good[] = {"pcsg00415", "*PSPEMUCFW"};
    TEST_CHECK(edit(stock, sizeof(stock) - 1, VJO_TAI_INSTALL, VJO_TAI_WORKER_KERNEL, good, 2, out, &r) > 0 &&
               !strstr(out, VJO_TAI_WORKER));
    TEST_CHECK(edit(stock, sizeof(stock) - 1, VJO_TAI_INSTALL, VJO_TAI_WORKER_LINES, good, 2, out, &r) > 0 &&
               strstr(out, "*PCSG00415\n" VJO_TAI_WORKER "\n") && strstr(out, "*PSPEMUCFW\n" VJO_TAI_WORKER "\n"));
}

static void test_storage_plugin(void)
{
    static const char ur0[] = "*KERNEL\nur0:tai/StorageMgr.skprx\n*main\nur0:tai/henkaku.suprx\n";
    static const char yamt[] = "*KERNEL\nur0:tai/yamt.skprx\n";
    static const char not_kernel[] = "*KERNEL\nur0:tai/a.skprx\n*main\nur0:tai/gamesd.skprx\n";
    char name[40];
    TEST_CHECK(vjo_tai_storage_plugin(ur0, sizeof(ur0) - 1, name, sizeof(name)) && !strcmp(name, "StorageMgr.skprx"));
    TEST_CHECK(vjo_tai_storage_plugin(yamt, sizeof(yamt) - 1, name, sizeof(name)) && !strcmp(name, "yamt.skprx"));
    TEST_CHECK(!vjo_tai_storage_plugin(not_kernel, sizeof(not_kernel) - 1, name, sizeof(name)));
    TEST_CHECK(!vjo_tai_storage_plugin(stock, sizeof(stock) - 1, name, sizeof(name)));
}

/* ---- a seeded corpus: lines built from the bytes and names taiHEN and Python's str disagree on ---- */
static unsigned long seed = 88172645463325252ul;
static unsigned next_rand(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return (unsigned)(seed >> 11);
}

static const char *const pieces[] = {
    "*KERNEL", "*main", "*ALL", "*!ALL", "*PCSG00415", "*!PCSG00415", "*PSPEMUCFW", "*NPXS10015", "*kernel", "* main",
    "*  !  KERNEL", "*", "*!", "# a comment", "#", "", "  ", "\t", VJO_TAI_KERNEL, VJO_TAI_SHELL, VJO_TAI_WORKER,
    VJO_TAI_TEXT_PLUGIN, "ux0:tai/VitaJPOverlay_Kernel.skprx", "UR0:TAI/VITAJPOVERLAY_SHELL.SUPRX",
    "ur0:/tai/VitaJPOverlay_OCR.suprx", "ur0:tai/henkaku.suprx", "ur0:tai/other.skprx", "ux0:tai/x.suprx",
    VJO_TAI_NOTE, VJO_TAI_APP_NOTE, VJO_TAI_DISABLED VJO_TAI_WORKER, "\xEF\xBB\xBF*KERNEL", "\xEF\xBB\xBF# bom",
    "\xE3\x80\x80" VJO_TAI_WORKER, VJO_TAI_WORKER "\xC2\xA0", "# x\x0c*ALL", "# y\xC2\x85*main", "ur0:tai/a.skprx\x0b",
    "\xE6\x97\xA5\xE6\x9C\xAC", "caf\xE9"};
static const char *const ends[] = {"\n", "\r\n", "\r", "\n", "\n", "\r\n"};

static size_t generate(char *buf, size_t cap)
{
    size_t used = 0;
    unsigned lines = 1 + next_rand() % 24;
    for (unsigned i = 0; i < lines; i++) {
        const char *p = pieces[next_rand() % (sizeof(pieces) / sizeof(pieces[0]))];
        const char *e = ends[next_rand() % (sizeof(ends) / sizeof(ends[0]))];
        const char *lead = next_rand() % 8 == 0 ? "  " : "", *trail = next_rand() % 8 == 0 ? " \t" : "";
        int n = snprintf(buf + used, cap - used, "%s%s%s%s", lead, p, trail, i + 1 == lines && next_rand() % 4 == 0 ? "" : e);
        if (n < 0 || (size_t)n >= cap - used) break;
        used += (size_t)n;
    }
    buf[used] = '\0';
    return used;
}

static void test_corpus(void)
{
    static char text[4096];
    const char *titles[] = {"PCSG00415", "ULJS00378"};
    int ok = 0, refused = 0;
    for (int i = 0; i < 3000; i++) {
        size_t n = generate(text, sizeof(text));
        for (int op = 0; op < 2; op++)
            for (int mode = 0; mode < 2; mode++) {
                VjoTaiReport r;
                int rc = edit(text, n, op, mode, titles, mode ? 2 : 0, out, &r);
                if (rc < 0) {
                    TEST_CHECK_(rc == VJO_TAI_E_CHECK || rc == VJO_TAI_E_NO_SECTION || rc == VJO_TAI_E_EMPTY,
                                "case %d op %d mode %d: rc %d (%s)", i, op, mode, rc, r.why);
                    refused++;
                    continue;
                }
                ok++;
                int again_rc = edit(out, (size_t)rc, op, mode, titles, mode ? 2 : 0, again, &r);
                TEST_CHECK_(again_rc == rc && !memcmp(again, out, (size_t)rc), "case %d op %d mode %d: idempotent", i, op, mode);
                TEST_CHECK_(taihen_config_validate(out) == 0, "case %d: taiHEN keeps the result", i);
                if (op == VJO_TAI_UNINSTALL) TEST_CHECK_(nothing_of_ours(out, "PCSG00415"), "case %d: uninstalled", i);
            }
    }
    TEST_CHECK_(ok > 5000 && refused > 100, "%d edits, %d refused", ok, refused);
    TEST_MSG("%d edits, %d refused", ok, refused);
}

TEST_LIST = {
    {"fixtures", test_fixtures},
    {"henkaku_stock", test_stock},
    {"report", test_report},
    {"refusals", test_refusals},
    {"storage_plugin", test_storage_plugin},
    {"corpus", test_corpus},
    {NULL, NULL}};
