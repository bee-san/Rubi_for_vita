#define main acutest_main
#include "acutest.h"
#undef main
#include "test_tmpdir.h"
#include "games.h"
#include "sfo.h"
#include "../../vita/games_storage.h"
#include <psp2/io/fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned char memory[40 * 1024];
static VjoArena arena;
static int write_fail, rename_fail, renames, stat_fail;
static size_t written;
static int result(int n) { return n < 0 ? (int)(0x80010000u | errno) : n; }
int sceIoOpen(const char *p, int flags, unsigned mode) { return result(open(p, flags, mode)); }
int sceIoClose(int fd) { return result(close(fd)); }
int sceIoRead(int fd, void *p, size_t len) { return result(read(fd, p, len > 13 ? 13 : len)); }
int sceIoWrite(int fd, const void *p, size_t len)
{
    if (write_fail && written >= 21) return -1;
    int n = (int)write(fd, p, len > 7 ? 7 : len);
    if (n > 0) written += n;
    return result(n);
}
SceOff sceIoLseek(int fd, SceOff off, int whence) { return lseek(fd, off, whence); }
int sceIoMkdir(const char *p, unsigned mode) { return result(mkdir(p, mode)); }
int sceIoGetstat(const char *p, SceIoStat *out)
{
    if (stat_fail) return (int)0x8001000du;
    struct stat s;
    int rc = stat(p, &s);
    if (!rc) out->st_mode = s.st_mode;
    return result(rc);
}
int sceIoRename(const char *from, const char *to)
{ if (++renames == rename_fail) return -1; return result(rename(from, to)); }
int sceIoRemove(const char *p) { return result(unlink(p)); }
int sceIoSync(const char *p, unsigned flags) { (void)p; (void)flags; return 0; }

static void reset_files(void)
{
    static int initialized;
    if (!initialized) {
        char dir[PATH_MAX];
        TEST_ASSERT(vjo_test_tmpdir_new(dir, sizeof(dir)) != NULL);
        TEST_ASSERT(chdir(dir) == 0);
        mkdir("ux0:data", 0777);
        mkdir("ux0:data/VitaJPOverlay", 0777);
        initialized = 1;
    }
    unlink(VJO_GAMES_PATH); unlink(VJO_GAMES_PATH ".bak"); unlink(VJO_GAMES_PATH ".tmp");
    write_fail = rename_fail = renames = stat_fail = 0;
    written = 0;
    vjo_arena_init(&arena, memory, sizeof(memory));
}

static void test_policy(void)
{
    VjoGames g, copy;
    char out[VJO_GAMES_TEXT_MAX + 1];
    vjo_games_defaults(&g);
    TEST_CHECK(vjo_games_enabled(&g, "PCSA00029"));
    TEST_CHECK(vjo_games_enabled(&g, "GAME"));
    TEST_ASSERT(vjo_games_set(&g, "PCSA00029", 0) == 0);
    TEST_CHECK(!vjo_games_enabled(&g, "GAME"));
    TEST_CHECK(!vjo_games_enabled(&g, "PCSA00029"));
    TEST_CHECK(vjo_games_enabled(&g, "PCSG00202"));
    g.default_enabled = 0;
    TEST_CHECK(!vjo_games_enabled(&g, "PCSG00202"));
    TEST_ASSERT(vjo_games_set(&g, "PCSG00202", 1) == 0);
    TEST_CHECK(vjo_games_enabled(&g, "PCSG00202"));
    TEST_CHECK(!vjo_games_enabled(&g, VJO_SETTINGS_TITLE_ID));
    TEST_CHECK(!vjo_games_enabled(&g, "VITASHELL"));
    TEST_CHECK(!vjo_games_enabled(&g, "NPXS10015"));
    TEST_ASSERT(vjo_games_set(&g, "NPXS10028", 1) == 0);
    TEST_CHECK(vjo_games_enabled(&g, "NPXS10028"));
    int n = vjo_games_format(&g, out, sizeof(out));
    TEST_ASSERT(n > 0);
    TEST_ASSERT(vjo_games_parse(&copy, out, n) == 0);
    TEST_CHECK(!memcmp(&g, &copy, sizeof(g)));
    TEST_CHECK(vjo_games_format(&g, out, 4) < 0);
    const char *ini = " # policy\r\n default = disabled\r\nPCSG00202=enabled\nPCSG00202=disabled\n";
    TEST_ASSERT(vjo_games_parse(&g, ini, strlen(ini)) == 0);
    TEST_CHECK(g.count == 1 && !vjo_games_enabled(&g, "PCSG00202"));
    const char *invalid[] = {"default=maybe", "../bad=disabled", "PCSA00029 disabled", "=enabled", "123456789012=enabled"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        TEST_CHECK(vjo_games_parse(&g, invalid[i], strlen(invalid[i])) < 0);
    TEST_CHECK(vjo_games_parse(&g, "a\0b", 3) < 0);
    vjo_games_defaults(&g);
    for (int i = 0; i < VJO_GAMES_MAX; i++) {
        char id[VJO_TITLE_ID_SIZE];
        snprintf(id, sizeof(id), "GAME%07d", i);
        TEST_ASSERT(vjo_games_set(&g, id, 0) == 0);
    }
    TEST_CHECK(vjo_games_set(&g, "EXTRA", 1) < 0);
    n = vjo_games_format(&g, out, sizeof(out));
    TEST_ASSERT(n > 0 && n <= VJO_GAMES_TEXT_MAX);
    TEST_ASSERT(vjo_games_parse(&copy, out, n) == 0);
    TEST_CHECK(copy.count == VJO_GAMES_MAX);
}

static void test_save_restart(void)
{
    VjoGames g, recovered;
    reset_files();
    TEST_ASSERT(vjo_games_file_load(&g, &arena) == 0);
    TEST_CHECK(g.default_enabled);
    TEST_ASSERT(vjo_games_set(&g, "PCSA00029", 0) == 0);
    TEST_ASSERT(vjo_games_file_save(&g, &arena) == 0);
    memset(&recovered, 0x55, sizeof(recovered));
    TEST_ASSERT(vjo_games_file_load(&recovered, &arena) == 0);
    TEST_CHECK(!vjo_games_enabled(&recovered, "PCSA00029"));
    TEST_CHECK(vjo_games_enabled(&recovered, "PCSG00202"));
    TEST_CHECK(arena.used == 0);
    g.default_enabled = 0;
    TEST_ASSERT(vjo_games_set(&g, "PCSG00202", 1) == 0);
    TEST_ASSERT(vjo_games_file_save(&g, &arena) == 0);
    TEST_ASSERT(vjo_games_file_load(&recovered, &arena) == 0);
    TEST_CHECK(vjo_games_enabled(&recovered, "PCSG00202"));
    TEST_CHECK(!vjo_games_enabled(&recovered, "PCSG00999"));
}

static void test_failed_save_and_backup(void)
{
    VjoGames g, recovered;
    reset_files();
    vjo_games_defaults(&g);
    vjo_games_set(&g, "PCSA00029", 0);
    TEST_ASSERT(vjo_games_file_save(&g, &arena) == 0);
    vjo_games_set(&g, "PCSA00029", 1);
    write_fail = 1; written = 0;
    TEST_CHECK(vjo_games_file_save(&g, &arena) < 0);
    TEST_ASSERT(vjo_games_file_load(&recovered, &arena) == 0);
    TEST_CHECK(!vjo_games_enabled(&recovered, "PCSA00029"));
    write_fail = 0; renames = 0; rename_fail = 2;
    TEST_CHECK(vjo_games_file_save(&g, &arena) < 0);
    /* Publication failed after old primary was moved: recover from .bak. */
    TEST_ASSERT(vjo_games_file_load(&recovered, &arena) == 1);
    TEST_CHECK(!vjo_games_enabled(&recovered, "PCSA00029"));
    rename_fail = 0;
    TEST_ASSERT(vjo_games_file_save(&recovered, &arena) == 0);
    TEST_ASSERT(vjo_games_file_load(&g, &arena) == 0);
    TEST_CHECK(!vjo_games_enabled(&g, "PCSA00029"));
    TEST_CHECK(arena.used == 0);
}

static void test_corruption_disables_input_reservation(void)
{
    VjoGames g;
    reset_files();
    int fd = open(VJO_GAMES_PATH, O_CREAT | O_WRONLY, 0666);
    TEST_ASSERT(fd >= 0);
    TEST_ASSERT(write(fd, "default=garbage", 15) == 15);
    close(fd);
    TEST_CHECK(vjo_games_file_load(&g, &arena) < 0);
    TEST_CHECK(!vjo_games_enabled(&g, "PCSA00029"));
    reset_files();
    stat_fail = 1;
    TEST_CHECK(vjo_games_file_load(&g, &arena) < 0);
    TEST_CHECK(!g.default_enabled); /* permission failure is not a missing file */
}

static void put32(unsigned char *p, uint32_t n)
{ for (int i = 0; i < 4; i++) p[i] = n >> (i * 8); }
static void test_sfo_bounds(void)
{
    unsigned char sfo[80] = {0};
    char out[80];
    put32(sfo, 0x46535000); put32(sfo + 8, 36); put32(sfo + 12, 48); put32(sfo + 16, 1);
    sfo[22] = 4; sfo[23] = 2;
    put32(sfo + 24, 10); put32(sfo + 28, 16);
    memcpy(sfo + 36, "TITLE", 6); memcpy(sfo + 48, "日本語", 10);
    TEST_ASSERT(vjo_sfo_string(sfo, sizeof(sfo), "TITLE", out, sizeof(out)) == 0);
    TEST_CHECK(!strcmp(out, "日本語"));
    TEST_CHECK(vjo_sfo_string(sfo, sizeof(sfo), "TITLE", out, 3) < 0);
    for (size_t i = 0; i < 64; i++) TEST_CHECK(vjo_sfo_string(sfo, i, "TITLE", out, sizeof(out)) < 0);
    put32(sfo + 32, UINT32_MAX);
    TEST_CHECK(vjo_sfo_string(sfo, sizeof(sfo), "TITLE", out, sizeof(out)) < 0);
    put32(sfo + 16, UINT32_MAX);
    TEST_CHECK(vjo_sfo_string(sfo, sizeof(sfo), "TITLE", out, sizeof(out)) < 0);
}

/* Adrenaline Bubble Manager before 01fd277 wrote a bubble's title over its
 * template without the length (left at 1): the string ends at its NUL inside
 * the entry's capacity, never beyond it. */
static void test_sfo_stale_length(void)
{
    unsigned char sfo[96] = {0};
    char out[80];
    put32(sfo, 0x46535000); put32(sfo + 8, 36); put32(sfo + 12, 48); put32(sfo + 16, 1);
    sfo[22] = 4; sfo[23] = 2;
    put32(sfo + 24, 1); put32(sfo + 28, 32); /* length 1, capacity 32 */
    memcpy(sfo + 36, "TITLE", 6); memcpy(sfo + 48, "うたわれるもの", 21);
    TEST_ASSERT(vjo_sfo_string(sfo, sizeof(sfo), "TITLE", out, sizeof(out)) == 0);
    TEST_CHECK(!strcmp(out, "うたわれるもの"));
    put32(sfo + 24, 0); /* no length at all */
    TEST_CHECK(vjo_sfo_string(sfo, sizeof(sfo), "TITLE", out, sizeof(out)) == 0 && !strcmp(out, "うたわれるもの"));
    memset(sfo + 48, 'A', 32); /* no NUL in the capacity: refused, though one follows */
    TEST_CHECK(vjo_sfo_string(sfo, sizeof(sfo), "TITLE", out, sizeof(out)) < 0);
    put32(sfo + 24, 33); /* a length beyond the capacity */
    TEST_CHECK(vjo_sfo_string(sfo, sizeof(sfo), "TITLE", out, sizeof(out)) < 0);
    put32(sfo + 24, 1); put32(sfo + 28, 0); /* no capacity */
    TEST_CHECK(vjo_sfo_string(sfo, sizeof(sfo), "TITLE", out, sizeof(out)) < 0);
}

/* PSP games by ID alone: Adrenaline's NPXS10028, its PSPEMU* bubbles (its own
 * PSPEMUCFW, Adrenaline Bubble Manager's) and bubbles named after their PSP
 * disc or PSN ID; never a Vita game, system app or homebrew ID. */
static void test_psp_titles(void)
{
    static const struct { const char *id; int psp, disc; } ids[] = {
        {"NPXS10028", 1, 0}, {"PSPEMUCFW", 1, 0}, {"PSPEMU001", 1, 0}, {"PSPEMU", 1, 0}, {"PCSG00415", 0, 0},
        {"PSPEM", 0, 0}, {"XPSPEMU01", 0, 0}, {"pspemu001", 0, 0}, {"NPXS10028X", 0, 0}, {"NPXS10027", 0, 0},
        {"NPXS10015", 0, 0}, {"", 0, 0},
        /* UMD and PSN IDs */
        {"ULJS00378", 1, 1}, {"ULJM05800", 1, 1}, {"UCJS10001", 1, 1}, {"ULUS10041", 1, 1}, {"UCES00001", 1, 1},
        {"NPJH50443", 1, 1}, {"NPUG80114", 1, 1}, {"NPEZ00001", 1, 1}, {"NPHH00001", 1, 1},
        /* Vita games, homebrew and near misses */
        {"VCJS10001", 0, 0}, {"VLJM35001", 0, 0}, {"PCSB00792", 0, 0}, {"NPXS10003", 0, 0}, {"NPNA00094", 0, 0},
        {"NPJX00001", 0, 0}, {"NPOA00002", 0, 0}, {"NPUZ12345", 1, 1}, {"NPXG00001", 0, 0}, {"NPXH00001", 0, 0}, {"SVMP00001", 0, 0}, {"SLPS01234", 0, 0}, {"UXJS00378", 0, 0}, {"ULJS0037", 0, 0},
        {"ULJS003780", 0, 0}, {"ULJS0037A", 0, 0}, {"uljs00378", 0, 0}, {"ULjS00378", 0, 0}, {"VITASHELL", 0, 0}};
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        TEST_CHECK_(vjo_game_is_psp(ids[i].id) == ids[i].psp, "%s", ids[i].id);
        TEST_CHECK_(vjo_psp_disc_id(ids[i].id) == ids[i].disc, "%s", ids[i].id);
    }
}

/* The strict title ID (core/games.h vjo_title_id_strict) the kernel's SC3 charset open and the hook cache share:
 * 9 capital letters or digits and a NUL, nothing else (no lowercase, no path, no byte above 0x7F). */
static void test_title_id_strict(void)
{
    static const struct { const char *id; int ok; } ids[] = {
        {"PCSG00146", 1}, {"NPXS10028", 1}, {"ULJS00378", 1}, {"VJOSET001", 1}, {"123456789", 1}, {"ZZZZZZZZZ", 1},
        {"pcsg00146", 0}, {"PCSg00146", 0}, {"PCSG0014", 0}, {"PCSG001466", 0}, {"PCSG0014/", 0}, {"../../tai", 0},
        {"PCSG_0146", 0}, {"PCSG-0146", 0}, {"PCSG 0146", 0}, {"PCSG\xC0" "0146", 0}, {"@CSG00146", 0}, {"[CSG00146", 0},
        {"PCSG0014:", 0}, {"", 0}};
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
        TEST_CHECK_(vjo_title_id_strict(ids[i].id) == ids[i].ok, "%s", ids[i].id);
    char padded[12] = "PCSG00146"; /* a NUL inside the 9 characters fails at it */
    padded[4] = 0;
    TEST_CHECK(!vjo_title_id_strict(padded));
}

/* A PSP game with no rule of its own follows the shared PSP row (NPXS10028),
 * then the default; its own rule wins; a Vita game never looks there. */
static void test_psp_shared_row(void)
{
    VjoGames g;
    vjo_games_defaults(&g);
    TEST_CHECK(vjo_games_enabled_as(&g, "ULJS00378", 1) && vjo_games_enabled_as(&g, "PCSG00415", 0));
    TEST_ASSERT(vjo_games_set(&g, "NPXS10028", 0) == 0);
    TEST_CHECK(!vjo_games_enabled_as(&g, "ULJS00378", 1) && !vjo_games_enabled_as(&g, "PSPEMUCFW", 1));
    TEST_CHECK(!vjo_games_enabled_as(&g, "NPXS10028", 1) && !vjo_games_enabled_as(&g, "NPXS10028", 0));
    TEST_CHECK(vjo_games_enabled_as(&g, "PCSG00415", 0) && vjo_games_enabled_as(&g, "ABCD12345", 0));
    TEST_ASSERT(vjo_games_set(&g, "ULJS00378", 1) == 0);
    TEST_CHECK(vjo_games_enabled_as(&g, "ULJS00378", 1) && !vjo_games_enabled_as(&g, "ULJS00385", 1));
    /* Without the shared row's rule: the default, both ways. */
    vjo_games_defaults(&g);
    g.default_enabled = 0;
    TEST_ASSERT(vjo_games_set(&g, "PSPEMU005", 1) == 0);
    TEST_CHECK(vjo_games_enabled_as(&g, "PSPEMU005", 1) && !vjo_games_enabled_as(&g, "PSPEMU006", 1));
    TEST_ASSERT(vjo_games_set(&g, "NPXS10028", 1) == 0);
    TEST_CHECK(vjo_games_enabled_as(&g, "PSPEMU006", 1) && !vjo_games_enabled_as(&g, "PCSG00415", 0));
    /* Not a game: never, whatever the flag. */
    TEST_CHECK(!vjo_games_enabled_as(&g, "NPXS10015", 1) && !vjo_games_enabled_as(&g, "main", 1) &&
               !vjo_games_enabled_as(&g, "VJOSET001", 1));
}

/* The Rubi app's catalog logs why it skips a folder (core/games_app.c): a
 * reason exactly when vjo_game_is_candidate refuses the ID, never otherwise,
 * over IDs of every shape the candidate rule looks at. */
static void test_reject_reasons(void)
{
    static const struct { const char *id, *why; } named[] = {
        {"PCSG00415", NULL}, {"NPXS10028", NULL}, {"PSPEMUCFW", NULL}, {"VITASHELX", NULL}, {"NPX", NULL},
        {"VITASHELL", "VitaShell"}, {"VITASHELL1", "VitaShell"}, {"NPXS10001", "a system app"},
        {"NPXS", "a system app"}, {"VJOSET001", "this app"}, {"main", "SceShell's name"},
        {"mainmenu", "SceShell's name"}, {"", "not a title ID"}, {"Not An ID", "not a title ID"},
        {"PCSG004151234", "not a title ID"}, {"ab/cd", "not a title ID"}, {"\xE3\x81\x82", "not a title ID"}};
    for (unsigned i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        const char *why = vjo_game_reject_reason(named[i].id);
        TEST_CHECK_(named[i].why ? why && !strcmp(why, named[i].why) : !why, "%s: %s", named[i].id, why ? why : "(none)");
    }
    TEST_CHECK(!strcmp(vjo_game_reject_reason(NULL), "not a title ID"));
    /* every prefix the rule tests, with each tail */
    static const char *const heads[] = {"NPXS", "NPX", "main", "mai", "VITASHELL", "VITASHEL", "VJOSET001", "VJOSET00",
                                        "PCSG", "a", "_", "-", "Z9", ""};
    static const char *const tails[] = {"", "1", "10028", "0001", "X", "_x", "-", " ", "12345678901", "."};
    int checked = 0;
    for (unsigned h = 0; h < sizeof(heads) / sizeof(heads[0]); h++)
        for (unsigned t = 0; t < sizeof(tails) / sizeof(tails[0]); t++) {
            char id[40];
            snprintf(id, sizeof(id), "%s%s", heads[h], tails[t]);
            TEST_CHECK_(!vjo_game_reject_reason(id) == !!vjo_game_is_candidate(id), "%s", id);
            checked++;
        }
    TEST_MSG("%d IDs", checked);
}

/* "Disable all" (core/games_app.c): the default OFF, every "= enabled" rule
 * gone in order, the "= disabled" ones kept; the Shell (vjo_games_enabled_as)
 * then sees every game OFF, PSP games and the timeout fallback included, from
 * the formatted file as from the policy. */
static void test_disable_all(void)
{
    VjoGames g, back;
    char text[VJO_GAMES_TEXT_MAX + 1];
    vjo_games_defaults(&g);
    static const struct { const char *id; int on; } rules[] = {
        {"PCSG00940", 1}, {"PCSB00792", 1}, {"PCSF00001", 0}, {"PSPEMUCFW", 1}, {"PCSG00415", 1},
        {"PCSG00402", 0}, {"NPXS10028", 1}, {"ULJS00378", 1}};
    for (unsigned i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) TEST_ASSERT(vjo_games_set(&g, rules[i].id, rules[i].on) == 0);
    TEST_CHECK(vjo_games_enabled(&g, "PCSG00415")); /* #138: an explicit enabled rule beat the default */
    TEST_CHECK(vjo_games_disable_all(&g) == 6);
    TEST_CHECK(g.default_enabled == 0 && g.count == 2 && !strcmp(g.rules[0].title_id, "PCSF00001") &&
               !strcmp(g.rules[1].title_id, "PCSG00402") && !g.rules[0].enabled && !g.rules[1].enabled);
    for (int i = g.count; i < VJO_GAMES_MAX; i++)
        TEST_CHECK_(!g.rules[i].title_id[0] && !g.rules[i].enabled, "rule %d left behind", i);
    int n = vjo_games_format(&g, text, sizeof(text));
    TEST_CHECK(n > 0 && !strcmp(text, "# Rubi per-game settings\ndefault = disabled\nPCSF00001 = disabled\n"
                                      "PCSG00402 = disabled\n"));
    TEST_ASSERT(vjo_games_parse(&back, text, (size_t)n) == 0);
    static const char *const ids[] = {"PCSG00940", "PCSB00792", "PCSF00001", "PSPEMUCFW", "PCSG00415", "PCSG00402",
                                      "NPXS10028", "ULJS00378", "PSPEMU005", "PCSA00029"};
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
        for (int psp = 0; psp < 2; psp++)
            TEST_CHECK_(!vjo_games_enabled_as(&g, ids[i], psp) && !vjo_games_enabled_as(&back, ids[i], psp), "%s psp %d", ids[i], psp);
    TEST_CHECK(!vjo_games_enabled(&back, "GAME"));
    /* again: nothing more to remove */
    TEST_CHECK(vjo_games_disable_all(&g) == 0 && g.count == 2);
    /* the default back ON: every game but the two turned OFF */
    g.default_enabled = 1;
    TEST_CHECK(vjo_games_enabled(&g, "PCSG00415") && vjo_games_enabled_as(&g, "ULJS00378", 1) &&
               !vjo_games_enabled(&g, "PCSF00001") && !vjo_games_enabled(&g, "PCSG00402"));
    /* a full table of enabled rules empties; an empty one only turns the default OFF */
    vjo_games_defaults(&g);
    for (int i = 0; i < VJO_GAMES_MAX; i++) {
        char id[VJO_TITLE_ID_SIZE];
        snprintf(id, sizeof(id), "PCSX%05d", i);
        TEST_ASSERT(vjo_games_set(&g, id, 1) == 0);
    }
    TEST_CHECK(vjo_games_disable_all(&g) == VJO_GAMES_MAX && g.count == 0 && !g.default_enabled);
    vjo_games_defaults(&g);
    TEST_CHECK(vjo_games_disable_all(&g) == 0 && g.count == 0 && !g.default_enabled);
    /* a count out of range: refused, nothing changed */
    vjo_games_defaults(&g);
    g.count = VJO_GAMES_MAX + 1;
    TEST_CHECK(vjo_games_disable_all(&g) == -1 && g.default_enabled == 1 && g.count == VJO_GAMES_MAX + 1);
    g.count = -1;
    TEST_CHECK(vjo_games_disable_all(&g) == -1 && g.default_enabled == 1);
}

/* #333: ocr_line_fix.ini, a game's own line finder: games.ini's format and parser with five names (Rubi's finder with
 * config.ini's four ocr_line_fix values, then the spotter; lowercase, a row's value its index), no default line; the
 * lookup is a row's own, else a PSP game's shared row's (NPXS10028), else -1; the Rubi app's set removes a row for
 * Default; its format reads back as the same rows. games.ini keeps its own two values. */
#define G VJO_LINE_FINDER_GRAD
static void test_line_fix_table(void)
{
    VjoGames g, back;
    char out[VJO_GAMES_TEXT_MAX + 1];
    TEST_CHECK(!strcmp(vjo_line_finder_names[VJO_LINE_FINDER_SPOTTER], "spotter") &&
               !strcmp(vjo_line_finder_names[VJO_LINE_FINDER_GRAD(0)], "off") &&
               !strcmp(vjo_line_finder_names[VJO_LINE_FINDER_GRAD(1)], "on") &&
               !strcmp(vjo_line_finder_names[VJO_LINE_FINDER_GRAD(2)], "junk") &&
               !strcmp(vjo_line_finder_names[VJO_LINE_FINDER_GRAD(3)], "all") && !vjo_line_finder_names[VJO_LINE_FINDERS] &&
               VJO_LINE_FINDER_SPOTTER == 0 && VJO_LINE_FINDER_GRAD(0) == 1);
    const char *ini = "# a comment\r\n ULJM05674 = all\nPCSG00415=off\n; another\n\nNPXS10028 = junk \nPCSG00940 = on\n"
                      "PCSG00415 = on\nPCSG00628 = spotter\n"; /* the last row wins, as in games.ini */
    TEST_ASSERT(vjo_line_fix_parse(&g, ini, strlen(ini)) == 0);
    TEST_CHECK(g.count == 5 && vjo_games_row_as(&g, "PCSG00628", 0) == VJO_LINE_FINDER_SPOTTER);
    static const struct { const char *id; int psp, want; } rows[] = {
        {"ULJM05674", 1, 4}, {"ULJM05674", 0, 4}, {"PCSG00415", 0, 2}, {"PCSG00940", 0, 2}, {"PCSG00001", 0, -1},
        {"PSPEMU005", 1, 3}, {"PSPEMU005", 0, -1}, {"NPXS10028", 0, 3}, {"GAME", 0, -1}, {"VJOSET001", 0, -1}};
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
        TEST_CHECK_(vjo_games_row_as(&g, rows[i].id, rows[i].psp) == rows[i].want, "%s psp %d: %d", rows[i].id,
                    rows[i].psp, vjo_games_row_as(&g, rows[i].id, rows[i].psp));
    /* every value, and each kind of bad line: the whole file is refused, as games.ini's is */
    static const char *const bad[] = {"default = on", "default = spotter", "PCSG00415 = enabled", "PCSG00415 = disabled",
                                      "PCSG00415 = ON", "PCSG00415 = Spotter", "PCSG00415 = grad", "PCSG00415 = valley",
                                      "PCSG00415 =", "PCSG00415 all", "= all", "PCSG004151234 = all", "../x = all",
                                      "PCSG00415 = all extra"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        TEST_CHECK_(vjo_line_fix_parse(&g, bad[i], strlen(bad[i])) < 0, "'%s'", bad[i]);
    TEST_CHECK(vjo_line_fix_parse(&g, "a\0b", 3) < 0);
    /* and games.ini does not take them: its values stay enabled and disabled */
    TEST_CHECK(vjo_games_parse(&g, "PCSG00415 = all", 15) < 0 && vjo_games_parse(&g, "PCSG00415 = enabled", 19) == 0);
    TEST_CHECK(vjo_line_fix_parse(&g, "", 0) == 0 && g.count == 0); /* no rows (the reader refuses an empty file) */
    /* the app's set: a value, then Default removes the row, the others keep their order */
    vjo_games_defaults(&g);
    TEST_CHECK(vjo_line_fix_set(&g, "ULJM05674", G(3)) == 0 && vjo_line_fix_set(&g, "PCSG00415", G(0)) == 0 &&
               vjo_line_fix_set(&g, "PCSG00940", G(2)) == 0 && g.count == 3);
    TEST_CHECK(vjo_line_fix_set(&g, "PCSG00415", G(1)) == 0 && g.count == 3 && vjo_games_row_as(&g, "PCSG00415", 0) == G(1));
    TEST_CHECK(vjo_line_fix_set(&g, "PCSG00415", -1) == 0 && g.count == 2 && vjo_games_row_as(&g, "PCSG00415", 0) == -1);
    TEST_CHECK(!strcmp(g.rules[0].title_id, "ULJM05674") && !strcmp(g.rules[1].title_id, "PCSG00940") &&
               !g.rules[2].title_id[0] && !g.rules[2].enabled);
    TEST_CHECK(vjo_line_fix_set(&g, "PCSG00415", -1) == 0 && g.count == 2); /* no row: nothing to remove */
    TEST_CHECK(vjo_line_fix_set(&g, "PCSG00415", VJO_LINE_FINDERS) < 0 && vjo_line_fix_set(&g, "PCSG00415", -2) < 0 &&
               vjo_line_fix_set(&g, "../x", 1) < 0 && g.count == 2);
    TEST_CHECK(vjo_line_fix_set(&g, "PCSG00628", VJO_LINE_FINDER_SPOTTER) == 0 && g.count == 3);
    int n = vjo_line_fix_format(&g, out, sizeof(out));
    TEST_ASSERT(n > 0);
    TEST_CHECK(strstr(out, "\nULJM05674 = all\nPCSG00940 = junk\nPCSG00628 = spotter\n") != NULL && out[0] == '#');
    TEST_CHECK(!strstr(out, "default") && !strstr(out, "enabled"));
    TEST_ASSERT(vjo_line_fix_parse(&back, out, (size_t)n) == 0);
    TEST_CHECK(back.count == 3 && vjo_games_row_as(&back, "ULJM05674", 0) == G(3) &&
               vjo_games_row_as(&back, "PCSG00940", 0) == G(2) && vjo_games_row_as(&back, "PCSG00628", 0) == VJO_LINE_FINDER_SPOTTER);
    TEST_CHECK(vjo_games_parse(&back, out, (size_t)n) < 0); /* never a games.ini */
    TEST_CHECK(vjo_line_fix_format(&g, out, 8) < 0);
    g.rules[0].enabled = VJO_LINE_FINDERS;
    TEST_CHECK(vjo_line_fix_format(&g, out, sizeof(out)) < 0);
    /* a full table: 512 rows fit the file's limit and read back; a 513th is refused */
    vjo_games_defaults(&g);
    for (int i = 0; i < VJO_GAMES_MAX; i++) {
        char id[VJO_TITLE_ID_SIZE];
        snprintf(id, sizeof(id), "GAME%07d", i);
        TEST_ASSERT(vjo_line_fix_set(&g, id, i % VJO_LINE_FINDERS) == 0);
    }
    TEST_CHECK(vjo_line_fix_set(&g, "EXTRA", 1) < 0 && g.count == VJO_GAMES_MAX);
    n = vjo_line_fix_format(&g, out, sizeof(out));
    TEST_ASSERT(n > 0 && n <= VJO_GAMES_TEXT_MAX);
    TEST_ASSERT(vjo_line_fix_parse(&back, out, (size_t)n) == 0);
    TEST_CHECK(back.count == VJO_GAMES_MAX && vjo_games_row_as(&back, "GAME0000511", 0) == 511 % VJO_LINE_FINDERS);
}

/* #333: the app saves ocr_line_fix.ini as games.ini is saved (a temporary file, sync, the old one as the backup) and
 * reads it back, from its backup when a save stopped between the renames; the Shell's read takes the file alone; an
 * unreadable file is -1 and is not written over by the load; games.ini is never touched. */
static void test_line_fix_storage(void)
{
    VjoGames g, back;
    reset_files();
    unlink(VJO_LINE_FIX_PATH); unlink(VJO_LINE_FIX_BACKUP); unlink(VJO_LINE_FIX_PATH ".tmp");
    TEST_CHECK(vjo_line_fix_file_load(&g, &arena) == 0 && g.count == 0); /* neither file: no rows */
    TEST_CHECK(vjo_line_fix_file_read(&g, &arena) < 0);                  /* the Shell: nothing to read */
    TEST_ASSERT(vjo_line_fix_set(&g, "ULJM05674", G(3)) == 0);
    TEST_ASSERT(vjo_line_fix_file_save(&g, &arena) == 0);
    TEST_CHECK(access(VJO_GAMES_PATH, F_OK) != 0 && access(VJO_GAMES_BACKUP, F_OK) != 0); /* games.ini untouched */
    memset(&back, 0x55, sizeof(back));
    TEST_CHECK(vjo_line_fix_file_read(&back, &arena) == 0 && vjo_games_row_as(&back, "ULJM05674", 1) == G(3));
    TEST_CHECK(vjo_line_fix_file_load(&back, &arena) == 0 && back.count == 1 && arena.used == 0);
    /* the second save keeps the first as the backup */
    TEST_ASSERT(vjo_line_fix_set(&g, "PCSG00415", G(0)) == 0 && vjo_line_fix_file_save(&g, &arena) == 0);
    TEST_ASSERT(vjo_line_fix_file_load(&back, &arena) == 0 && back.count == 2);
    FILE *f = fopen(VJO_LINE_FIX_BACKUP, "rb");
    TEST_ASSERT(f != NULL);
    char text[256];
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    TEST_CHECK(strstr(text, "ULJM05674 = all\n") && !strstr(text, "PCSG00415"));
    /* a save cut short after the old file became the backup: the app reads the backup; the Shell reads nothing */
    renames = 0; rename_fail = 2;
    TEST_ASSERT(vjo_line_fix_set(&g, "PCSG00940", VJO_LINE_FINDER_SPOTTER) == 0);
    TEST_CHECK(vjo_line_fix_file_save(&g, &arena) < 0);
    rename_fail = 0;
    TEST_CHECK(vjo_line_fix_file_load(&back, &arena) == 1 && back.count == 2 && vjo_games_row_as(&back, "PCSG00940", 0) < 0);
    TEST_CHECK(vjo_line_fix_file_read(&back, &arena) < 0);
    TEST_ASSERT(vjo_line_fix_file_save(&g, &arena) == 0); /* the next save puts it back */
    TEST_CHECK(vjo_line_fix_file_read(&back, &arena) == 0 && back.count == 3);
    /* a file that cannot be read (a hand-written line of another kind): -1, kept as it is */
    unlink(VJO_LINE_FIX_BACKUP);
    f = fopen(VJO_LINE_FIX_PATH, "wb");
    TEST_ASSERT(f && fputs("ULJM05674 = valley\n", f) >= 0 && fclose(f) == 0);
    TEST_CHECK(vjo_line_fix_file_load(&back, &arena) < 0 && vjo_line_fix_file_read(&back, &arena) < 0);
    f = fopen(VJO_LINE_FIX_PATH, "rb");
    TEST_ASSERT(f != NULL);
    n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    TEST_CHECK(!strcmp(text, "ULJM05674 = valley\n"));
    /* the same beside a good backup (audit-rubi-333 F1): still -1 and no rows, as the Shell reads it, so the app
     * writes nothing over the hand-edited file and shows none of the backup's rows */
    f = fopen(VJO_LINE_FIX_BACKUP, "wb");
    TEST_ASSERT(f && fputs("ULJM05674 = all\n", f) >= 0 && fclose(f) == 0);
    memset(&back, 0x55, sizeof(back));
    TEST_CHECK(vjo_line_fix_file_load(&back, &arena) < 0 && back.count == 0 && vjo_line_fix_file_read(&g, &arena) < 0);
    /* the backup alone (a save cut short): 1 with its rows, which a save puts back where the Shell reads them */
    unlink(VJO_LINE_FIX_PATH);
    TEST_CHECK(vjo_line_fix_file_load(&back, &arena) == 1 && vjo_games_row_as(&back, "ULJM05674", 0) == G(3));
    TEST_CHECK(vjo_line_fix_file_read(&g, &arena) < 0);
    TEST_ASSERT(vjo_line_fix_file_save(&back, &arena) == 0);
    TEST_CHECK(vjo_line_fix_file_read(&g, &arena) == 0 && vjo_games_row_as(&g, "ULJM05674", 0) == G(3));
    TEST_CHECK(vjo_line_fix_file_load(&back, &arena) == 0);
    TEST_CHECK(arena.used == 0);
    unlink(VJO_LINE_FIX_PATH); unlink(VJO_LINE_FIX_BACKUP);
}

TEST_LIST = {
    {"per-title blacklist and selected-only policies", test_policy},
    {"#333: ocr_line_fix.ini's rows: parse, lookup, set and format", test_line_fix_table},
    {"#333: ocr_line_fix.ini on the card: save, load, backup, never games.ini", test_line_fix_storage},
    {"PSP titles: NPXS10028, PSPEMU* bubbles, disc IDs", test_psp_titles},
    {"the strict title ID: 9 capital letters or digits", test_title_id_strict},
    {"PSP games without a rule follow the shared PSP row", test_psp_shared_row},
    {"the app's reasons for skipping a folder match the candidate rule", test_reject_reasons},
    {"disable all: no game stays ON by a choice of its own", test_disable_all},
    {"SFO strings with a stale length (older ABM bubbles)", test_sfo_stale_length},
    {"saved choices survive fresh loads", test_save_restart},
    {"partial writes and interrupted rename retain disabled games", test_failed_save_and_backup},
    {"corrupt or unreadable settings disable OCR", test_corruption_disables_input_reservation},
    {"Japanese SFO titles and malicious offsets", test_sfo_bounds},
    {NULL, NULL}
};

int main(int argc, char **argv)
{
    if (!vjo_test_tmpdir("games")) return 2; /* each test's card in it, all removed at the end */
    return acutest_main(argc, argv);
}
