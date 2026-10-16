/* The Shell's foreground classification (shell/game.c, the code SceShell runs)
 * on the host: AppMgr and the process's title ID are scripted per case, the
 * memory card is a temporary directory with synthetic bubbles (boot.bin,
 * boot.inf, the Overlay app's description) and games.ini. Each case also
 * counts the Shell's file calls: a launch reads at most one boot.bin and one
 * description and lists no directory. */
#include "acutest.h"
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../shell/shell.h"
#include "../../core/pspbubble.h"
#include "../../vita/games_storage.h"

/* ---- the scripted process ---- */
static char proc_id[32], param_id[32], app_name[32], title9[128], title10[128];
static int proc_rc, param_rc, app_rc;
int sceKernelGetProcessTitleId(SceUID pid, char *out, SceSize len)
{
    TEST_CHECK(pid == 0x40010003 && len >= 10);
    if (proc_rc < 0) return proc_rc;
    snprintf(out, len, "%s", proc_id);
    return 0;
}
int sceAppMgrAppParamGetString(int pid, int param, char *out, SceSize cap)
{
    TEST_CHECK(pid == 0x40010003);
    const char *v = param == 12 ? param_id : param == 9 ? title9 : param == 10 ? title10 : "";
    if ((param == 12 && param_rc < 0) || !v[0]) return param == 12 && param_rc < 0 ? param_rc : (int)0x80801000u;
    snprintf(out, cap, "%s", v);
    return 0;
}
int sceAppMgrGetNameById(SceUID pid, char *name)
{
    TEST_CHECK(pid == 0x40010003);
    if (app_rc < 0) return app_rc;
    snprintf(name, 32, "%s", app_name);
    return 0;
}

/* ---- log.txt ---- */
static char log_lines[16][300];
static int log_count;
void vjo_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(log_lines[log_count % 16], sizeof(log_lines[0]), fmt, ap);
    va_end(ap);
    log_count++;
}
static const char *logged(const char *needle)
{
    for (int i = 0; i < log_count && i < 16; i++)
        if (strstr(log_lines[i], needle)) return log_lines[i];
    return NULL;
}

/* ---- the memory card ---- */
static int opens, reads, stats, dirs;
static size_t largest_read; /* the most one sceIoRead asked for */
static int native(int n) { return n < 0 ? (int)(0x80010000u | (unsigned)errno) : n; }
int sceIoOpen(const char *p, int flags, unsigned mode) { opens++; return native(open(p, flags, mode)); }
int sceIoClose(int fd) { return native(close(fd)); }
int sceIoRead(int fd, void *p, size_t len)
{
    reads++;
    if (len > largest_read) largest_read = len;
    return native((int)read(fd, p, len));
}
int sceIoWrite(int fd, const void *p, size_t len) { return native((int)write(fd, p, len)); }
SceOff sceIoLseek(int fd, SceOff off, int whence) { return lseek(fd, off, whence); }
int sceIoMkdir(const char *p, unsigned mode) { return native(mkdir(p, mode)); }
int sceIoGetstat(const char *p, SceIoStat *out)
{
    struct stat st;
    stats++;
    int rc = stat(p, &st);
    if (!rc) out->st_mode = st.st_mode;
    return native(rc);
}
int sceIoRename(const char *a, const char *b) { return native(rename(a, b)); }
int sceIoRemove(const char *p) { return native(unlink(p)); }
int sceIoSync(const char *p, unsigned flags) { return 0; }
int sceIoDopen(const char *p) { dirs++; return -1; }
int sceIoDread(int d, SceIoDirent *e) { return -1; }
int sceIoDclose(int d) { return 0; }

static void put_file(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    TEST_ASSERT(f != NULL);
    TEST_ASSERT(fwrite(data, 1, n, f) == n);
    TEST_ASSERT(fclose(f) == 0);
}
static void mkdirs(const char *id)
{
    char p[96];
    mkdir("ux0:app", 0777);
    snprintf(p, sizeof(p), "ux0:app/%s", id); mkdir(p, 0777);
    snprintf(p, sizeof(p), "ux0:app/%s/data", id); mkdir(p, 0777);
}
/* An Adrenaline Bubble Manager bubble: data/boot.bin (0x140 bytes, the magic, the path at 0x40). */
static void bubble(const char *id, const char *target)
{
    unsigned char b[0x140] = {0};
    char p[96];
    mkdirs(id);
    b[0] = 'A'; b[1] = 'B'; b[2] = 'B';
    b[0x0C] = 1;
    memcpy(b + 0x40, target, strlen(target));
    snprintf(p, sizeof(p), "ux0:app/%s/data/boot.bin", id);
    put_file(p, b, sizeof(b));
}
static void described(const char *id, const char *disc, const char *title, const char *target)
{
    char p[96], text[VJO_PSP_INFO_MAX];
    mkdir("ux0:data", 0777); mkdir("ux0:data/VitaJPOverlay", 0777); mkdir(VJO_PSP_INFO_DIR, 0777);
    int n = vjo_psp_info_format(text, sizeof(text), disc, title, target);
    TEST_ASSERT(n > 0);
    snprintf(p, sizeof(p), VJO_PSP_INFO_DIR "/%s.txt", id);
    put_file(p, text, (size_t)n);
}
static void card(void)
{
    static char dir[64];
    if (!dir[0]) {
        snprintf(dir, sizeof(dir), "/tmp/vjo-shell-game-XXXXXX");
        TEST_ASSERT(mkdtemp(dir) != NULL);
        TEST_ASSERT(chdir(dir) == 0);
        bubble("ULJS00378", "ux0:pspemu/ISO/Synthetic One.iso");
        described("ULJS00378", "ULJS00378", "合成ゲーム One", "ux0:pspemu/ISO/Synthetic One.iso");
        bubble("PSPEMU005", "ux0:pspemu/ISO/TWO.CSO");
        described("PSPEMU005", "ULJS00385", "Synthetic Two", "ux0:pspemu/ISO/TWO.CSO");
        bubble("PSPEMU006", "ux0:pspemu/PSP/GAME/NPJH00000/EBOOT.PBP"); /* never described */
        bubble("ABCD12345", "ux0:pspemu/ISO/THREE.ISO");
        mkdirs("OLDBUBBLE");
        put_file("ux0:app/OLDBUBBLE/data/boot.inf", "PATH=ms0:/ISO/OLD.ISO\n", 22);
        /* A Vita homebrew with a data/boot.bin of its own format: not a bubble. */
        mkdirs("HBREW0001");
        put_file("ux0:app/HBREW0001/data/boot.bin", "NOTABUBBLE-0123456789", 21);
    }
}
static void process(const char *proc, const char *param, const char *app)
{
    card();
    snprintf(proc_id, sizeof(proc_id), "%s", proc ? proc : "");
    snprintf(param_id, sizeof(param_id), "%s", param ? param : "");
    snprintf(app_name, sizeof(app_name), "%s", app ? app : "");
    proc_rc = proc ? 0 : (int)0x80010016u;
    param_rc = param ? 0 : (int)0x80800001u;
    app_rc = app ? 0 : (int)0x80800002u;
    title9[0] = title10[0] = '\0';
    opens = reads = stats = dirs = log_count = 0;
}

static VjoGameId identify(void)
{
    VjoGameId g;
    memset(&g, 0x55, sizeof(g));
    TEST_ASSERT(vjo_game_identify(0x40010003, &g) == 0);
    TEST_CHECK(dirs == 0 && stats == 0); /* nothing listed, nothing probed */
    TEST_CHECK(opens == (g.mode != VJO_GAME_NONE) && reads <= 1); /* a game's description, read once */
    return g;
}

static void test_bubble_behind_the_emulator_id(void)
{
    /* The console's case: the Shell's title ID is NPXS10028, AppMgr's name the bubble's. */
    process("NPXS10028", "NPXS10028", "ULJS00378");
    VjoGameId g = identify();
    TEST_CHECK(!strcmp(g.key, "ULJS00378") && g.psp && g.mode == VJO_GAME_STATIC_FB);
    TEST_CHECK(!strcmp(g.disc, "ULJS00378") && !strcmp(g.name, "合成ゲーム One"));
    TEST_CHECK(logged("psp: NPXS10028 ULJS00378 -> ULJS00378; disc ULJS00378; boots ux0:pspemu/ISO/Synthetic One.iso; "
                      "\"合成ゲーム One\"") != NULL);
    TEST_CHECK(log_count == 1 && reads == 1);
    TEST_MSG("%s", log_lines[0]);
    /* Not described yet: still the bubble's own ID, no disc ID or name, and
     * the log says how to get them. */
    process("NPXS10028", NULL, "PSPEMU006");
    g = identify();
    TEST_CHECK(!strcmp(g.key, "PSPEMU006") && g.psp && g.mode == VJO_GAME_STATIC_FB && !g.disc[0] && !g.name[0]);
    TEST_CHECK(logged("-> PSPEMU006; disc ; boots ?; \"\"") != NULL);
    TEST_MSG("%s", log_lines[0]);
    process("NPXS10028", "NPXS10028", "OLDBUBBLE"); /* an old boot.inf bubble */
    g = identify();
    TEST_CHECK(!strcmp(g.key, "OLDBUBBLE") && g.psp);
    /* A PSP game of Sony's own behind the emulator: named by AppMgr. */
    process("NPXS10028", "NPXS10028", "NPJH50443");
    g = identify();
    TEST_CHECK(!strcmp(g.key, "NPJH50443") && g.psp && g.mode == VJO_GAME_STATIC_FB);
    /* Only the emulator's ID: the shared PSP row's key, "PSP" as Anki's name. */
    process("NPXS10028", "NPXS10028", "NPXS10028");
    g = identify();
    TEST_CHECK(!strcmp(g.key, "NPXS10028") && g.psp && g.mode == VJO_GAME_STATIC_FB);
    char name[32];
    vjo_title_name(0x40010003, g.key, name, sizeof(name));
    TEST_CHECK(!strcmp(name, "PSP"));
    process("NPXS10028", NULL, NULL);
    g = identify();
    TEST_CHECK(!strcmp(g.key, "NPXS10028") && g.psp);
}

static void test_bubble_by_its_own_id(void)
{
    /* Under the bubble's own title ID (its booter, an ordinary app: the console
     * showed PSPEMU005 starting before NPXS10028): a PSP game for the policy,
     * Anki and the log, but no static framebuffer sampling, as before. */
    process("PSPEMU005", "NPXS10028", "PSPEMU005");
    VjoGameId g = identify();
    TEST_CHECK(!strcmp(g.key, "PSPEMU005") && g.psp && g.mode == VJO_GAME);
    TEST_CHECK(!strcmp(g.disc, "ULJS00385") && !strcmp(g.name, "Synthetic Two"));
    TEST_CHECK(logged(" PSPEMU005 PSPEMU005 -> PSPEMU005; disc ULJS00385; boots ux0:pspemu/ISO/TWO.CSO; \"Synthetic Two\"") != NULL);
    /* Any ID the Overlay app described (it found the bubble by its files). */
    described("ABCD12345", "", "Synthetic Three", "ux0:pspemu/ISO/THREE.ISO");
    process("ABCD12345", "ABCD12345", "ABCD12345");
    g = identify();
    TEST_CHECK(!strcmp(g.key, "ABCD12345") && g.psp && g.mode == VJO_GAME && !g.disc[0] &&
               !strcmp(g.name, "Synthetic Three"));
    unlink(VJO_PSP_INFO_DIR "/ABCD12345.txt");
    /* Adrenaline's emulator: its PSPEMUCFW. */
    process("NPXS10028", "NPXS10028", "PSPEMUCFW");
    g = identify();
    TEST_CHECK(!strcmp(g.key, "PSPEMUCFW") && g.psp && g.mode == VJO_GAME_STATIC_FB);
    /* Undescribed, under their own title ID (a booter, an ordinary app): ID
     * shapes are not used here, so they are Vita games, as on main. */
    static const char *const own[] = {"PSPEMUCFW", "PSPEMU006", "ULJS00403", "ABCD12345"};
    for (unsigned i = 0; i < sizeof(own) / sizeof(own[0]); i++) {
        process(own[i], own[i], own[i]);
        g = identify();
        TEST_CHECK_(!strcmp(g.key, own[i]) && !g.psp && g.mode == VJO_GAME && !g.disc[0] && log_count == 0, "%s", own[i]);
    }
}

static void test_vita_games_and_system_apps(void)
{
    process("PCSG00415", "PCSG00415", "PCSG00415");
    VjoGameId g = identify();
    TEST_CHECK(!strcmp(g.key, "PCSG00415") && !g.psp && g.mode == VJO_GAME && !g.disc[0] && log_count == 0);
    /* A homebrew's own data/boot.bin is no bubble's. */
    process("HBREW0001", "HBREW0001", "HBREW0001");
    g = identify();
    TEST_CHECK(!strcmp(g.key, "HBREW0001") && !g.psp && g.mode == VJO_GAME);
    /* System apps, SceShell and VitaShell are not games. */
    process("NPXS10015", "NPXS10015", "NPXS10015");
    g = identify();
    TEST_CHECK(g.mode == VJO_GAME_NONE && !g.psp && !strcmp(g.key, "NPXS10015"));
    process("main", NULL, NULL);
    g = identify();
    TEST_CHECK(g.mode == VJO_GAME_NONE && !g.psp);
    process("VITASHELL", "VITASHELL", "VITASHELL");
    g = identify();
    TEST_CHECK(g.mode == VJO_GAME_NONE && !g.psp);
    /* A title ID too long for any game's is no game, whatever its first 11 characters. */
    process("ABCDEFGHIJKLMNOP", NULL, NULL);
    g = identify();
    TEST_CHECK(g.mode == VJO_GAME_NONE && !g.psp && strlen(g.key) == 11);
    /* The title from AppMgr's parameter when the process's is not readable; an
     * app name that is not the emulator's changes nothing. */
    process(NULL, "PCSG00415", "PCSG00415");
    g = identify();
    TEST_CHECK(!strcmp(g.key, "PCSG00415") && g.mode == VJO_GAME);
    process("PCSG00415", NULL, "PCSG00416");
    g = identify();
    TEST_CHECK(!strcmp(g.key, "PCSG00415") && !g.psp && log_count == 0);
    /* Nothing readable yet: the error, so the Shell tries again. */
    process(NULL, NULL, "PCSG00415");
    VjoGameId none, zero;
    memset(&none, 0x55, sizeof(none));
    memset(&zero, 0, sizeof(zero));
    TEST_CHECK(vjo_game_identify(0x40010003, &none) == (int)0x80800001u && opens == 0);
    TEST_CHECK(!memcmp(&none, &zero, sizeof(none))); /* the Shell's GAME fallback relies on it */
}

/* The per-game policy through games.ini: a bubble's own row wins, else the
 * shared PSP row, else the default; a Vita game never looks at the PSP row. */
static void test_policy(void)
{
    static unsigned char mem[64 * 1024];
    VjoArena a;
    VjoGames g;
    card();
    vjo_arena_init(&a, mem, sizeof(mem));
    vjo_games_defaults(&g);
    TEST_ASSERT(vjo_games_set(&g, "NPXS10028", 0) == 0);
    TEST_ASSERT(vjo_games_set(&g, "PSPEMU005", 1) == 0);
    TEST_ASSERT(vjo_games_file_save(&g, &a) == 0);
    TEST_CHECK(!vjo_game_enabled("ULJS00378", 1, &a) && vjo_game_enabled("PSPEMU005", 1, &a));
    TEST_CHECK(!vjo_game_enabled("NPXS10028", 1, &a) && vjo_game_enabled("PCSG00415", 0, &a));
    TEST_ASSERT(vjo_games_set(&g, "ULJS00378", 1) == 0 && vjo_games_set(&g, "NPXS10028", 1) == 0);
    TEST_ASSERT(vjo_games_set(&g, "PSPEMU005", 0) == 0);
    TEST_ASSERT(vjo_games_file_save(&g, &a) == 0);
    TEST_CHECK(vjo_game_enabled("ULJS00378", 1, &a) && !vjo_game_enabled("PSPEMU005", 1, &a) &&
               vjo_game_enabled("PSPEMU006", 1, &a));
}

/* ---- descriptions and AppMgr names nobody should write (after audit-overlay-adrbubble's cases) ---- */
static void put_desc(const char *id, const void *data, size_t n)
{
    char p[96];
    mkdir("ux0:data", 0777); mkdir("ux0:data/VitaJPOverlay", 0777); mkdir(VJO_PSP_INFO_DIR, 0777);
    snprintf(p, sizeof(p), VJO_PSP_INFO_DIR "/%s.txt", id);
    put_file(p, data, n);
}

/* Longer than VJO_PSP_INFO_MAX: 512 bytes are read, one fewer than the
 * buffer, which keeps its NUL; a title is cut to the name's 127 bytes. */
static void long_and_damaged_descriptions(void)
{
    static unsigned char big[2048];
    VjoGameId g;
    card();
    memset(big, 'A', 600);
    put_desc("PSPEMU006", big, 600);
    process("NPXS10028", NULL, "PSPEMU006");
    largest_read = 0;
    g = identify();
    TEST_CHECK(!strcmp(g.key, "PSPEMU006") && g.psp && !g.disc[0] && !g.name[0] && g.mode == VJO_GAME_STATIC_FB);
    TEST_CHECK_(reads == 1 && largest_read == VJO_PSP_INFO_MAX, "read %zu B", largest_read);
    memcpy(big, "ULJS00378\n", 10);
    memset(big + 10, 'T', 503);
    for (size_t n = 511; n <= 513; n++) {
        put_desc("PSPEMU006", big, n);
        process("NPXS10028", NULL, "PSPEMU006");
        g = identify();
        TEST_CHECK_(!strcmp(g.disc, "ULJS00378") && strlen(g.name) == 127 && g.psp, "%zu B", n);
    }
    /* A first line that is no title ID, and NULs. */
    static const char *const firsts[] = {"ULJS00378XYZ\nT\np\n", "ULJS 00378\nT\np\n", "../../x\nT\np\n"};
    for (unsigned i = 0; i < sizeof(firsts) / sizeof(firsts[0]); i++) {
        put_desc("PSPEMU006", firsts[i], strlen(firsts[i]));
        process("NPXS10028", NULL, "PSPEMU006");
        g = identify();
        TEST_CHECK_(!g.disc[0] && !strcmp(g.name, "T") && g.psp, "%u", i);
    }
    static const unsigned char nul[] = "ULJS00378\nTi\0tle\npath\n";
    put_desc("PSPEMU006", nul, sizeof(nul) - 1);
    process("NPXS10028", NULL, "PSPEMU006");
    g = identify();
    TEST_CHECK(!strcmp(g.disc, "ULJS00378") && !strcmp(g.name, "Ti") && g.psp);
    /* A title cut inside a character keeps whole characters only. */
    char text[VJO_PSP_INFO_MAX];
    char title[200];
    memset(title, 'x', 126);
    memcpy(title + 126, "\xE3\x81\x82\xE3\x81\x82", 7);
    int dn = vjo_psp_info_format(text, sizeof(text), "", title, "ux0:p");
    TEST_ASSERT(dn > 0);
    put_desc("PSPEMU006", text, (size_t)dn);
    process("NPXS10028", NULL, "PSPEMU006");
    g = identify();
    TEST_CHECK_(strlen(g.name) == 126, "%zu", strlen(g.name));
    unlink(VJO_PSP_INFO_DIR "/PSPEMU006.txt");
}

/* A description that opens but cannot be read (a directory by that name):
 * no description, no crash; the emulator's process is PSP anyway. */
static void an_unreadable_description(void)
{
    card();
    mkdir("ux0:data", 0777); mkdir("ux0:data/VitaJPOverlay", 0777); mkdir(VJO_PSP_INFO_DIR, 0777);
    mkdir(VJO_PSP_INFO_DIR "/PSPEMU007.txt", 0777);
    process("NPXS10028", NULL, "PSPEMU007");
    VjoGameId g = identify();
    TEST_CHECK(!strcmp(g.key, "PSPEMU007") && g.psp && !g.disc[0] && !g.name[0] && g.mode == VJO_GAME_STATIC_FB);
    TEST_CHECK(logged("boots ?; \"\"") != NULL);
    rmdir(VJO_PSP_INFO_DIR "/PSPEMU007.txt");
}

/* AppMgr names that are no game's key the emulator's NPXS10028: SceShell's,
 * a system app's, VitaShell's, the Overlay app's, 12+ characters (AppMgr's
 * name has 32 bytes), a path. An 11-character ID is a game's. */
static void names_that_are_no_games(void)
{
    static const struct { const char *app, *key; } cases[] = {
        {"main", "NPXS10028"}, {"NPXS10001", "NPXS10028"}, {"VITASHELL", "NPXS10028"}, {"VJOSET001", "NPXS10028"},
        {"ABCDEFGHIJKL", "NPXS10028"}, {"ABCDEFGHIJKLMNOPQRSTUVWXYZ01234", "NPXS10028"}, {"../../../../x", "NPXS10028"},
        {"ux0:app/ULJS00378", "NPXS10028"}, {"", "NPXS10028"}, {"ABCDEFGHIJK", "ABCDEFGHIJK"}};
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        process("NPXS10028", "NPXS10028", cases[i].app);
        VjoGameId g = identify();
        TEST_CHECK_(!strcmp(g.key, cases[i].key) && g.mode == VJO_GAME_STATIC_FB && g.psp, "%s -> %s", cases[i].app, g.key);
    }
}

TEST_LIST = {
    {"a bubble, Adrenaline or Sony's PSP game behind the emulator's NPXS10028", test_bubble_behind_the_emulator_id},
    {"a PSP game by its description or its ID", test_bubble_by_its_own_id},
    {"Vita games and system apps unchanged", test_vita_games_and_system_apps},
    {"the policy: own row, shared PSP row, default", test_policy},
    {"long and damaged descriptions", long_and_damaged_descriptions},
    {"a description that cannot be read", an_unreadable_description},
    {"AppMgr names that are no game's", names_that_are_no_games},
    {NULL, NULL}
};
