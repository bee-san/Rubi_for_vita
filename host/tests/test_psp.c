/* Adrenaline's PSP games on synthetic data only: AdrBubbleBooter bubble files
 * (core/pspbubble.c), the PARAM.SFO of a bubble's target in every format the
 * Rubi app reads (core/psptarget.c: EBOOT.PBP, ISO 9660, CSO v1/v2, ZSO,
 * built here), and the title IDs a row may carry (core/taicfg.c; the config.txt
 * editor itself is host/tests/test_taicfg.c's). No game data: titles, IDs and images are made up. With
 * zlib (VJO_TEST_ZLIB) the CSO images' deflate blocks are made and read too. */
#include "acutest.h"
#include "games.h"
#include "pspbubble.h"
#include "psptarget.h"
#include "sfo.h"
#include "taicfg.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef VJO_TEST_ZLIB
#include <zlib.h>
#endif

static void put16(unsigned char *p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i)); }

/* ---- AdrBubbleBooter's files ---- */

static size_t boot_bin(unsigned char *b, size_t size, uint32_t magic, size_t at, const char *path)
{
    memset(b, 0, size);
    put32(b, magic);
    put32(b + 4, 1);   /* driver */
    put32(b + 0x0C, 1); /* customized, as ABM writes it */
    if (path) memcpy(b + at, path, strlen(path));
    return size;
}

static void test_boot_bin(void)
{
    unsigned char b[0x200];
    char out[VJO_ABB_PATH_MAX];
    /* AdrBubbleBooter v1.3's 0x140 bytes: the path at 0x40. */
    size_t n = boot_bin(b, 0x140, VJO_ABB_MAGIC, 0x40, "ux0:pspemu/ISO/Synthetic Game.iso");
    TEST_CHECK(vjo_abb_boot_bin(b, n, out, sizeof(out)) == 0 && !strcmp(out, "ux0:pspemu/ISO/Synthetic Game.iso"));
    /* Any other size is the older layout, the path at 0x20 (converted at launch). */
    n = boot_bin(b, 0x120, VJO_ABB_MAGIC, 0x20, "ur0:pspemu/ISO/OLD.CSO");
    TEST_CHECK(vjo_abb_boot_bin(b, n, out, sizeof(out)) == 0 && !strcmp(out, "ur0:pspemu/ISO/OLD.CSO"));
    n = boot_bin(b, 0x141, VJO_ABB_MAGIC, 0x20, "uma0:pspemu/PSP/GAME/NPJH00000/EBOOT.PBP");
    TEST_CHECK(vjo_abb_boot_bin(b, n, out, sizeof(out)) == 0 && !strcmp(out, "uma0:pspemu/PSP/GAME/NPJH00000/EBOOT.PBP"));
    n = boot_bin(b, 0x30, VJO_ABB_MAGIC, 0x20, "ux0:a"); /* a short file: the field ends with it */
    TEST_CHECK(vjo_abb_boot_bin(b, n, out, sizeof(out)) == 0 && !strcmp(out, "ux0:a"));
    /* Not a bubble: no magic. */
    n = boot_bin(b, 0x140, 0x00424242, 0x40, "ux0:pspemu/ISO/X.ISO");
    TEST_CHECK(vjo_abb_boot_bin(b, n, out, sizeof(out)) == -1 && !out[0]);
    TEST_CHECK(vjo_abb_boot_bin(b, 3, out, sizeof(out)) == -1);
    TEST_CHECK(vjo_abb_boot_bin(NULL, 0x140, out, sizeof(out)) == -1);
    /* A bubble whose path is unusable: empty (ABM's template), no device, a
     * control character, no NUL in its 256 bytes, too long for out. */
    n = boot_bin(b, 0x140, VJO_ABB_MAGIC, 0x40, NULL);
    memcpy(b + 0x2C, "BBD1", 4);
    TEST_CHECK(vjo_abb_boot_bin(b, n, out, sizeof(out)) == -2);
    n = boot_bin(b, 0x140, VJO_ABB_MAGIC, 0x40, "pspemu/ISO/X.ISO");
    TEST_CHECK(vjo_abb_boot_bin(b, n, out, sizeof(out)) == -2);
    n = boot_bin(b, 0x140, VJO_ABB_MAGIC, 0x40, "ux0:pspemu/ISO/X\n.ISO");
    TEST_CHECK(vjo_abb_boot_bin(b, n, out, sizeof(out)) == -2);
    n = boot_bin(b, 0x140, VJO_ABB_MAGIC, 0x40, NULL);
    memset(b + 0x40, 'A', 0x100);
    memcpy(b + 0x40, "ux0:", 4);
    TEST_CHECK(vjo_abb_boot_bin(b, n, out, sizeof(out)) == -2);
    n = boot_bin(b, 0x140, VJO_ABB_MAGIC, 0x40, "ux0:pspemu/ISO/LONGER.ISO");
    TEST_CHECK(vjo_abb_boot_bin(b, n, out, 10) == -2);
    n = boot_bin(b, 0x40, VJO_ABB_MAGIC, 0x40, NULL); /* a path field outside a short file */
    TEST_CHECK(vjo_abb_boot_bin(b, 0x20, out, sizeof(out)) == -2);
}

static void test_boot_inf(void)
{
    char out[VJO_ABB_PATH_MAX], big[1400];
    TEST_CHECK(vjo_abb_boot_inf("PATH=ms0:/ISO/GAME.ISO\r\n", 24, out, sizeof(out)) == 0 &&
               !strcmp(out, "ux0:pspemu/ISO/GAME.ISO"));
    /* bootconv.suprx's rules: the last PATH line wins, any case, comments and
     * lines without '=' are skipped, nothing is trimmed, any prefix "PATH". */
    const char *t = "# old\nPATH=ms0:/ISO/A.ISO\npath=ms0:/ISO/B.CSO\nPATH\nDRIVER=INFERNO\n";
    TEST_CHECK(vjo_abb_boot_inf(t, strlen(t), out, sizeof(out)) == 0 && !strcmp(out, "ux0:pspemu/ISO/B.CSO"));
    t = "PATH=ms0:/ISO/A.ISO\nPATX=ms0:/ISO/WRONG.ISO\nPAT=ms0:/ISO/WRONG2.ISO\n";
    TEST_CHECK(vjo_abb_boot_inf(t, strlen(t), out, sizeof(out)) == 0 && !strcmp(out, "ux0:pspemu/ISO/A.ISO"));
    t = "PATHS=ms0:/PSP/GAME/X/EBOOT.PBP\n#PATH=ms0:/ISO/C.ISO\n PATH=ms0:/ISO/D.ISO";
    TEST_CHECK(vjo_abb_boot_inf(t, strlen(t), out, sizeof(out)) == 0 && !strcmp(out, "ux0:pspemu/PSP/GAME/X/EBOOT.PBP"));
    /* The part after the value's last ':' (a device-qualified path). */
    t = "PATH=ef0:ms0:/ISO/E.ISO";
    TEST_CHECK(vjo_abb_boot_inf(t, strlen(t), out, sizeof(out)) == 0 && !strcmp(out, "ux0:pspemu/ISO/E.ISO"));
    TEST_CHECK(vjo_abb_boot_inf("PATH=/ISO/NODEVICE.ISO", 22, out, sizeof(out)) == -1 && !out[0]);
    TEST_CHECK(vjo_abb_boot_inf("PATH=ms0:", 9, out, sizeof(out)) == -1);
    TEST_CHECK(vjo_abb_boot_inf("DRIVER=MARCH33\n", 15, out, sizeof(out)) == -1);
    TEST_CHECK(vjo_abb_boot_inf("", 0, out, sizeof(out)) == -1);
    TEST_CHECK(vjo_abb_boot_inf("PATH=ms0:/ISO/F.ISO", 19, out, 12) == -1);
    /* A C string of at most 1 KiB, as bootconv reads it. */
    memcpy(big, "PATH=ms0:/ISO/G.ISO\n\0PATH=ms0:/ISO/H.ISO\n", 42);
    TEST_CHECK(vjo_abb_boot_inf(big, 42, out, sizeof(out)) == 0 && !strcmp(out, "ux0:pspemu/ISO/G.ISO"));
    memset(big, '#', sizeof(big));
    big[0] = '#';
    big[1019] = '\n';
    memcpy(big + 1020, "PATH=ms0:/ISO/LATE.ISO\n", 23);
    TEST_CHECK(vjo_abb_boot_inf(big, sizeof(big), out, sizeof(out)) == -1);
    /* The converted path must fit AdrBubbleBooter's 256 bytes. */
    memcpy(big, "PATH=ms0:/", 10);
    memset(big + 10, 'x', 240);
    TEST_CHECK(vjo_abb_boot_inf(big, 250, out, sizeof(out)) == 0 && strlen(out) == 10 + 241);
    memset(big + 10, 'x', 300);
    TEST_CHECK(vjo_abb_boot_inf(big, 310, out, sizeof(out)) == -1);
}

static void test_psp_info(void)
{
    char text[VJO_PSP_INFO_MAX + 1], longt[200], path[400], *title, *where, *disc;
    int n = vjo_psp_info_format(text, sizeof(text), "ULJS00378", "合成テストゲーム", "ux0:pspemu/ISO/x.iso");
    TEST_ASSERT(n > 0 && (size_t)n == strlen(text));
    TEST_CHECK(!strcmp(text, "ULJS00378\n合成テストゲーム\nux0:pspemu/ISO/x.iso\n"));
    disc = vjo_psp_info_lines(text, &title, &where);
    TEST_CHECK(disc == text && !strcmp(disc, "ULJS00378") && !strcmp(title, "合成テストゲーム") &&
               !strcmp(where, "ux0:pspemu/ISO/x.iso"));
    /* No disc ID; missing lines; nothing at all; more lines than three; a CR
     * (the app writes none) stays in its line. */
    char a[] = "\nA Title\nux0:p\n", b[] = "NPJH50443", c[] = "", d[] = "\n\n\n\n", e[] = "X\nY", f[] = "1\n2\n3\n4\n";
    char g[] = "D\r\nT\r\n";
    disc = vjo_psp_info_lines(a, &title, &where);
    TEST_CHECK(!disc[0] && !strcmp(title, "A Title") && !strcmp(where, "ux0:p"));
    disc = vjo_psp_info_lines(b, &title, &where);
    TEST_CHECK(!strcmp(disc, "NPJH50443") && !title[0] && !where[0]);
    disc = vjo_psp_info_lines(c, &title, &where);
    TEST_CHECK(!disc[0] && !title[0] && !where[0]);
    disc = vjo_psp_info_lines(d, &title, &where);
    TEST_CHECK(!disc[0] && !title[0] && !where[0]);
    disc = vjo_psp_info_lines(e, &title, &where);
    TEST_CHECK(!strcmp(disc, "X") && !strcmp(title, "Y") && !where[0]);
    disc = vjo_psp_info_lines(f, &title, &where);
    TEST_CHECK(!strcmp(disc, "1") && !strcmp(title, "2") && !strcmp(where, "3"));
    disc = vjo_psp_info_lines(g, &title, &where);
    TEST_CHECK(!strcmp(disc, "D\r") && !strcmp(title, "T\r") && !where[0] && !vjo_game_id_valid(disc));
    /* Formatting refuses what could not be read back the same way. */
    TEST_CHECK(vjo_psp_info_format(text, sizeof(text), "ulj s0378", "T", "p") < 0);
    TEST_CHECK(vjo_psp_info_format(text, sizeof(text), "ULJS00378", "two\nlines", "p") < 0);
    TEST_CHECK(vjo_psp_info_format(text, sizeof(text), "ULJS00378", "T", "p\r") < 0);
    TEST_CHECK(vjo_psp_info_format(text, 8, "ULJS00378", "T", "p") < 0);
    memset(longt, 'T', sizeof(longt) - 1);
    longt[sizeof(longt) - 1] = '\0';
    memset(path, 'p', sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    TEST_CHECK(vjo_psp_info_format(text, sizeof(text), "ULJS00378", longt, path) < 0); /* over VJO_PSP_INFO_MAX */
    TEST_CHECK(vjo_psp_info_format(text, sizeof(text), "", "Title", "ux0:p") == (int)strlen("\nTitle\nux0:p\n"));
}

/* ---- PARAM.SFO of a target ---- */

/* An SFO with string keys (sorted, as Sony's are). */
static size_t make_sfo(unsigned char *sfo, size_t cap, const char *const *keys, const char *const *values, int n)
{
    size_t key_bytes = 0, value_bytes = 0;
    for (int i = 0; i < n; i++) {
        key_bytes += strlen(keys[i]) + 1;
        value_bytes += (strlen(values[i]) + 4) & ~3u;
    }
    size_t key_at = 20 + 16 * (size_t)n, value_at = (key_at + key_bytes + 3) & ~3u, size = value_at + value_bytes;
    TEST_ASSERT(size <= cap);
    memset(sfo, 0, size);
    memcpy(sfo, "\0PSF", 4);
    put32(sfo + 4, 0x101);
    put32(sfo + 8, (uint32_t)key_at);
    put32(sfo + 12, (uint32_t)value_at);
    put32(sfo + 16, (uint32_t)n);
    size_t k = 0, v = 0;
    for (int i = 0; i < n; i++) {
        unsigned char *e = sfo + 20 + 16 * i;
        size_t len = strlen(values[i]) + 1, room = (len + 3) & ~3u;
        put16(e, (uint32_t)k);
        put16(e + 2, 0x0204);
        put32(e + 4, (uint32_t)len);
        put32(e + 8, (uint32_t)room);
        put32(e + 12, (uint32_t)v);
        memcpy(sfo + key_at + k, keys[i], strlen(keys[i]) + 1);
        memcpy(sfo + value_at + v, values[i], len);
        k += strlen(keys[i]) + 1;
        v += room;
    }
    return size;
}

static unsigned char game_sfo[512];
static size_t game_sfo_len;
static void make_game_sfo(void)
{
    static const char *const keys[] = {"CATEGORY", "DISC_ID", "TITLE"};
    static const char *const values[] = {"UG", "ULJS00999", "合成ゲーム Synthetic"};
    game_sfo_len = make_sfo(game_sfo, sizeof(game_sfo), keys, values, 3);
}

/* A file in memory, read through VjoPspFile; reads counted. */
typedef struct { const unsigned char *data; size_t size; int reads; size_t bytes; } Mem;
static int mem_read(void *ctx, uint64_t off, void *buf, size_t n)
{
    Mem *m = ctx;
    TEST_ASSERT(off <= m->size && n <= m->size - off); /* the reader never asks beyond the file */
    memcpy(buf, m->data + off, n);
    m->reads++;
    m->bytes += n;
    return 0;
}
#ifdef VJO_TEST_ZLIB
static long zinflate(void *ctx, const void *in, size_t n, void *out, size_t cap)
{
    z_stream z;
    (void)ctx;
    memset(&z, 0, sizeof(z));
    if (inflateInit2(&z, -15) != Z_OK) return -1;
    z.next_in = (Bytef *)(uintptr_t)in;
    z.avail_in = (uInt)n;
    z.next_out = out;
    z.avail_out = (uInt)cap;
    int rc = inflate(&z, Z_FINISH);
    long done = (long)(cap - z.avail_out);
    inflateEnd(&z);
    return rc == Z_STREAM_END || (!z.avail_out && (rc == Z_OK || rc == Z_BUF_ERROR)) ? done : -1;
}
#endif
static VjoPspWork work;
static unsigned char sfo_out[VJO_PSP_SFO_MAX];

static int read_sfo(const unsigned char *data, size_t size, Mem *m, int with_inflate, size_t *len)
{
    *m = (Mem){data, size, 0, 0};
    VjoPspFile f = {m, size, mem_read, NULL};
#ifdef VJO_TEST_ZLIB
    if (with_inflate) f.inflate = zinflate;
#else
    (void)with_inflate;
#endif
    return vjo_psp_param_sfo(&f, &work, sfo_out, sizeof(sfo_out), len);
}

static int sfo_is_the_game(size_t len)
{
    char title[64], disc[16];
    return len == game_sfo_len && !memcmp(sfo_out, game_sfo, len) &&
           !vjo_sfo_string(sfo_out, len, "TITLE", title, sizeof(title)) && !strcmp(title, "合成ゲーム Synthetic") &&
           !vjo_sfo_string(sfo_out, len, "DISC_ID", disc, sizeof(disc)) && !strcmp(disc, "ULJS00999");
}

static void test_pbp(void)
{
    static unsigned char pbp[4096];
    Mem m;
    size_t len;
    make_game_sfo();
    memset(pbp, 0, sizeof(pbp));
    memcpy(pbp, "\0PBP", 4);
    put32(pbp + 4, 0x10000);
    for (int i = 0; i < 8; i++) put32(pbp + 8 + 4 * i, (uint32_t)(0x28 + (i ? game_sfo_len : 0)));
    memcpy(pbp + 0x28, game_sfo, game_sfo_len);
    TEST_CHECK(read_sfo(pbp, 0x28 + game_sfo_len + 100, &m, 0, &len) == VJO_PSP_PBP && sfo_is_the_game(len));
    TEST_CHECK(m.reads == 2 && m.bytes == 0x28 + game_sfo_len);
    /* Damaged headers: the SFO's end before its start, beyond the file, inside the header. */
    put32(pbp + 12, 0x20);
    TEST_CHECK(read_sfo(pbp, sizeof(pbp), &m, 0, &len) == VJO_PSP_E_NO_SFO);
    put32(pbp + 12, 0x10000);
    TEST_CHECK(read_sfo(pbp, sizeof(pbp), &m, 0, &len) == VJO_PSP_E_NO_SFO);
    put32(pbp + 8, 0x10);
    put32(pbp + 12, 0x100);
    TEST_CHECK(read_sfo(pbp, sizeof(pbp), &m, 0, &len) == VJO_PSP_E_NO_SFO);
    TEST_CHECK(read_sfo(pbp, 0x20, &m, 0, &len) == VJO_PSP_E_READ);
    TEST_CHECK(read_sfo(pbp, 3, &m, 0, &len) == VJO_PSP_E_READ);
}

/* A synthetic UMD image: sector 16 the volume descriptor, 17 its terminator,
 * 18 the root, 19 /PSP_GAME, 20 PARAM.SFO, 21 ICON0.PNG's stand-in. */
#define ISO_SECTORS 24
static unsigned char iso[ISO_SECTORS * VJO_PSP_SECTOR];
static size_t dir_record(unsigned char *at, uint32_t lba, uint32_t size, int dir, const char *name, size_t name_len)
{
    size_t len = 33 + name_len + !(name_len & 1);
    memset(at, 0, len);
    at[0] = (unsigned char)len;
    put32(at + 2, lba);
    for (int i = 0; i < 4; i++) at[6 + i] = (unsigned char)(lba >> (24 - 8 * i));
    put32(at + 10, size);
    for (int i = 0; i < 4; i++) at[14 + i] = (unsigned char)(size >> (24 - 8 * i));
    at[25] = dir ? 2 : 0;
    put16(at + 28, 1);
    at[32] = (unsigned char)name_len;
    memcpy(at + 33, name, name_len);
    return len;
}
static void make_iso(const char *game_dir, const char *sfo_name)
{
    unsigned char *s;
    size_t at;
    make_game_sfo();
    memset(iso, 0, sizeof(iso));
    s = iso + 16 * VJO_PSP_SECTOR;
    s[0] = 1; memcpy(s + 1, "CD001", 5); s[6] = 1;
    memcpy(s + 40, "SYNTHETIC_VOLUME", 16);
    put32(s + 80, ISO_SECTORS);
    put16(s + 120, 1); put16(s + 124, 1); put16(s + 128, VJO_PSP_SECTOR);
    dir_record(s + 156, 18, VJO_PSP_SECTOR, 1, "\0", 1);
    s = iso + 17 * VJO_PSP_SECTOR;
    s[0] = 255; memcpy(s + 1, "CD001", 5); s[6] = 1;
    s = iso + 18 * VJO_PSP_SECTOR;
    at = dir_record(s, 18, VJO_PSP_SECTOR, 1, "\0", 1);
    at += dir_record(s + at, 18, VJO_PSP_SECTOR, 1, "\1", 1);
    at += dir_record(s + at, 22, 16, 0, "UMD_DATA.BIN;1", 14);
    at += dir_record(s + at, 19, VJO_PSP_SECTOR, 1, game_dir, strlen(game_dir));
    s = iso + 19 * VJO_PSP_SECTOR;
    at = dir_record(s, 19, VJO_PSP_SECTOR, 1, "\0", 1);
    at += dir_record(s + at, 18, VJO_PSP_SECTOR, 1, "\1", 1);
    at += dir_record(s + at, 21, 64, 0, "ICON0.PNG;1", 11);
    at += dir_record(s + at, 20, (uint32_t)game_sfo_len, 0, sfo_name, strlen(sfo_name));
    memcpy(iso + 20 * VJO_PSP_SECTOR, game_sfo, game_sfo_len);
    memset(iso + 21 * VJO_PSP_SECTOR, 0x89, 64);
    memcpy(iso + 22 * VJO_PSP_SECTOR, "ULJS-00999|0001|", 16);
}

static void test_iso(void)
{
    Mem m;
    size_t len;
    make_iso("PSP_GAME", "PARAM.SFO;1");
    TEST_CHECK(read_sfo(iso, sizeof(iso), &m, 0, &len) == VJO_PSP_ISO && sfo_is_the_game(len));
    TEST_CHECK(m.reads == 5 && m.bytes == 0x28 + 3 * VJO_PSP_SECTOR + game_sfo_len); /* header, PVD, 2 dirs, SFO */
    /* Names without a version, in small letters, and with the period ISO 9660
     * puts after a name without an extension. */
    make_iso("psp_game", "param.sfo");
    TEST_CHECK(read_sfo(iso, sizeof(iso), &m, 0, &len) == VJO_PSP_ISO && sfo_is_the_game(len));
    make_iso("PSP_GAME.", "PARAM.SFO.;1");
    TEST_CHECK(read_sfo(iso, sizeof(iso), &m, 0, &len) == VJO_PSP_ISO && sfo_is_the_game(len));
    make_iso("PSP_GAME.;1", "PARAM.SFO");
    TEST_CHECK(read_sfo(iso, sizeof(iso), &m, 0, &len) == VJO_PSP_ISO && sfo_is_the_game(len));
    /* A file named like the directory, and a directory named like the file,
     * come first: each is skipped by its kind. */
    make_iso("PSP_GAME", "PARAM.SFO;1");
    unsigned char *root = iso + 18 * VJO_PSP_SECTOR, *game = iso + 19 * VJO_PSP_SECTOR;
    memset(root, 0, VJO_PSP_SECTOR);
    size_t at = dir_record(root, 18, VJO_PSP_SECTOR, 1, "\0", 1);
    at += dir_record(root + at, 18, VJO_PSP_SECTOR, 1, "\1", 1);
    at += dir_record(root + at, 22, 16, 0, "PSP_GAME;1", 10);
    at += dir_record(root + at, 19, VJO_PSP_SECTOR, 1, "PSP_GAME", 8);
    memset(game, 0, VJO_PSP_SECTOR);
    at = dir_record(game, 19, VJO_PSP_SECTOR, 1, "\0", 1);
    at += dir_record(game + at, 18, VJO_PSP_SECTOR, 1, "\1", 1);
    at += dir_record(game + at, 21, VJO_PSP_SECTOR, 1, "PARAM.SFO", 9);
    at += dir_record(game + at, 20, (uint32_t)game_sfo_len, 0, "PARAM.SFO;1", 11);
    TEST_CHECK(read_sfo(iso, sizeof(iso), &m, 0, &len) == VJO_PSP_ISO && sfo_is_the_game(len));
    /* No game directory, no SFO, an SFO too large, a file named like the directory. */
    make_iso("PSP_GAMES", "PARAM.SFO;1");
    TEST_CHECK(read_sfo(iso, sizeof(iso), &m, 0, &len) == VJO_PSP_E_NO_SFO);
    make_iso("PSP_GAME", "PARAM.SF;1");
    TEST_CHECK(read_sfo(iso, sizeof(iso), &m, 0, &len) == VJO_PSP_E_NO_SFO);
    make_iso("PSP_GAME", "PARAM.SFO;1");
    unsigned char *rec = iso + 19 * VJO_PSP_SECTOR;
    rec += rec[0]; rec += rec[0]; rec += rec[0]; /* PARAM.SFO's record */
    put32(rec + 10, VJO_PSP_SFO_MAX + 1);
    TEST_CHECK(read_sfo(iso, sizeof(iso), &m, 0, &len) == VJO_PSP_E_NO_SFO);
    put32(rec + 10, 0x10000000); /* beyond the image */
    TEST_CHECK(read_sfo(iso, sizeof(iso), &m, 0, &len) == VJO_PSP_E_NO_SFO);
    put32(rec + 10, (uint32_t)game_sfo_len);
    put32(rec + 2, 1000); /* its sector beyond the image */
    TEST_CHECK(read_sfo(iso, sizeof(iso), &m, 0, &len) == VJO_PSP_E_READ);
    /* A damaged record and a damaged volume descriptor. */
    make_iso("PSP_GAME", "PARAM.SFO;1");
    iso[18 * VJO_PSP_SECTOR] = 20;
    TEST_CHECK(read_sfo(iso, sizeof(iso), &m, 0, &len) == VJO_PSP_E_IMAGE);
    make_iso("PSP_GAME", "PARAM.SFO;1");
    iso[18 * VJO_PSP_SECTOR + 32] = 200; /* a name longer than its record */
    TEST_CHECK(read_sfo(iso, sizeof(iso), &m, 0, &len) == VJO_PSP_E_IMAGE);
    make_iso("PSP_GAME", "PARAM.SFO;1");
    put16(iso + 16 * VJO_PSP_SECTOR + 128, 512);
    TEST_CHECK(read_sfo(iso, sizeof(iso), &m, 0, &len) == VJO_PSP_E_FORMAT);
    make_iso("PSP_GAME", "PARAM.SFO;1");
    TEST_CHECK(read_sfo(iso, 16 * VJO_PSP_SECTOR, &m, 0, &len) == VJO_PSP_E_FORMAT);
    /* A huge directory is looked at for VJO_PSP_DIR_MAX bytes only. */
    put32(iso + 16 * VJO_PSP_SECTOR + 156 + 10, 0xFFFFF800u);
    TEST_CHECK(read_sfo(iso, sizeof(iso), &m, 0, &len) == VJO_PSP_ISO && sfo_is_the_game(len));
    /* Formats it does not read. */
    static unsigned char dax[64] = "DAX";
    TEST_CHECK(read_sfo(dax, sizeof(dax), &m, 0, &len) == VJO_PSP_E_FORMAT);
    static unsigned char noise[40000];
    for (size_t i = 0; i < sizeof(noise); i++) noise[i] = (unsigned char)(i * 2654435761u >> 13);
    TEST_CHECK(read_sfo(noise, sizeof(noise), &m, 0, &len) == VJO_PSP_E_FORMAT);
}

/* A root directory larger than the 64 KiB the reader looks at: an entry in
 * its 32nd sector is found, one past it is not, and nothing past it is read. */
static void test_iso_directory_cap(void)
{
    static unsigned char big[64 * VJO_PSP_SECTOR];
    Mem m;
    size_t len;
    make_game_sfo();
    for (int beyond = 0; beyond < 2; beyond++) {
        memset(big, 0, sizeof(big));
        unsigned char *s = big + 16 * VJO_PSP_SECTOR;
        s[0] = 1; memcpy(s + 1, "CD001", 5); s[6] = 1;
        put16(s + 128, VJO_PSP_SECTOR);
        dir_record(s + 156, 18, 40 * VJO_PSP_SECTOR, 1, "\0", 1); /* the root: sectors 18-57 */
        uint32_t at = 18 + (beyond ? 32 : 31);                    /* PSP_GAME's record */
        dir_record(big + (size_t)at * VJO_PSP_SECTOR, 60, VJO_PSP_SECTOR, 1, "PSP_GAME", 8);
        dir_record(big + 60 * VJO_PSP_SECTOR, 61, (uint32_t)game_sfo_len, 0, "PARAM.SFO;1", 11);
        memcpy(big + 61 * VJO_PSP_SECTOR, game_sfo, game_sfo_len);
        int rc = read_sfo(big, sizeof(big), &m, 0, &len);
        if (beyond)
            TEST_CHECK_(rc == VJO_PSP_E_NO_SFO && m.bytes == 0x28 + VJO_PSP_SECTOR + VJO_PSP_DIR_MAX, "rc %d, %zu B read", rc,
                        m.bytes);
        else
            TEST_CHECK_(rc == VJO_PSP_ISO && sfo_is_the_game(len), "rc %d", rc);
    }
}

/* ---- LZ4 blocks: the decoder, and a small encoder for the images ---- */

static size_t lz4_put_len(unsigned char *o, size_t n)
{
    size_t k = 0;
    for (n -= 15; n >= 255; n -= 255) o[k++] = 255;
    o[k++] = (unsigned char)n;
    return k;
}
/* A valid LZ4 block of in[0, n): greedy, matches found by search (small inputs). */
static size_t lz4_encode(const unsigned char *in, size_t n, unsigned char *out)
{
    size_t ip = 0, op = 0, anchor = 0;
    while (ip + 12 <= n) {
        size_t best = 0, from = 0;
        for (size_t c = ip > 4096 ? ip - 4096 : 0; c < ip; c++) {
            size_t l = 0;
            while (ip + l + 5 < n && in[c + l] == in[ip + l]) l++;
            if (l > best) best = l, from = c;
        }
        if (best < 4) { ip++; continue; }
        size_t lit = ip - anchor, ml = best - 4;
        unsigned char *token = out + op++;
        *token = (unsigned char)((lit < 15 ? lit : 15) << 4 | (ml < 15 ? ml : 15));
        if (lit >= 15) op += lz4_put_len(out + op, lit);
        memcpy(out + op, in + anchor, lit); op += lit;
        put16(out + op, (uint32_t)(ip - from)); op += 2;
        if (ml >= 15) op += lz4_put_len(out + op, ml);
        ip += best;
        anchor = ip;
    }
    size_t lit = n - anchor;
    out[op++] = (unsigned char)((lit < 15 ? lit : 15) << 4);
    if (lit >= 15) op += lz4_put_len(out + op, lit);
    memcpy(out + op, in + anchor, lit);
    return op + lit;
}

static void test_lz4(void)
{
    unsigned char in[3000], packed[4000], out[3000];
    for (size_t i = 0; i < sizeof(in); i++) in[i] = (unsigned char)(i % 7 == 0 ? (int)i : i < 1000 ? 'A' : "PSPgame"[i % 7]);
    size_t n = lz4_encode(in, sizeof(in), packed);
    TEST_ASSERT(n < sizeof(in));
    TEST_CHECK(vjo_lz4_block(packed, n, out, sizeof(out)) == (long)sizeof(in) && !memcmp(in, out, sizeof(in)));
    /* Padding after a full block is ignored (a ZSO block's alignment). */
    memset(packed + n, 0xEE, 16);
    TEST_CHECK(vjo_lz4_block(packed, n + 16, out, sizeof(out)) == (long)sizeof(in) && !memcmp(in, out, sizeof(in)));
    /* An overlapping match (a run), long literal and match lengths. */
    static const unsigned char run[] = {0x1F, 'z', 1, 0, 255, 255, 10, 0x00};
    TEST_CHECK(vjo_lz4_block(run, 7, out, 1 + 4 + 15 + 255 + 255 + 10) == 1 + 4 + 15 + 255 + 255 + 10);
    for (int i = 0; i < 1 + 4 + 15 + 255 + 255 + 10; i++) TEST_CHECK(out[i] == 'z');
    /* Damaged: an offset of 0, before the start, a match past the output,
     * literals past the input, a cut length, a cut offset. */
    static const unsigned char zero[] = {0x14, 'a', 0, 0};
    static const unsigned char before[] = {0x14, 'a', 2, 0};
    static const unsigned char past[] = {0x1F, 'a', 1, 0, 200};
    static const unsigned char lits[] = {0x50, 'a', 'b'};
    static const unsigned char cut[] = {0xF0, 255};
    static const unsigned char cut_off[] = {0x14, 'a', 1};
    TEST_CHECK(vjo_lz4_block(zero, sizeof(zero), out, 100) == -1);
    TEST_CHECK(vjo_lz4_block(before, sizeof(before), out, 100) == -1);
    TEST_CHECK(vjo_lz4_block(past, sizeof(past), out, 100) == -1);
    TEST_CHECK(vjo_lz4_block(lits, sizeof(lits), out, 100) == -1);
    TEST_CHECK(vjo_lz4_block(cut, sizeof(cut), out, 100) == -1);
    TEST_CHECK(vjo_lz4_block(cut_off, sizeof(cut_off), out, 100) == -1);
    /* Literals beyond the output, and an input that ends early (short). */
    TEST_CHECK(vjo_lz4_block(packed, n, out, 100) == -1);
    TEST_CHECK(vjo_lz4_block(packed, n / 2, out, sizeof(out)) < (long)sizeof(in));
}

/* ---- block-compressed images ---- */

enum { BLOCK_STORED, BLOCK_DEFLATE, BLOCK_LZ4 };
static unsigned char packed_image[2 * sizeof(iso) + 4096];
/* The image iso[0, size) as a CSO (version 1 or 2) or ZSO with block_size and
 * index alignment shift; how(b) chooses each block's coding (one that does
 * not pay is stored). */
static size_t make_packed(const char *magic, int version, uint32_t block, int shift, int (*how)(uint32_t b))
{
    uint32_t blocks = (uint32_t)((sizeof(iso) + block - 1) / block);
    size_t at = 0x18 + 4 * (size_t)(blocks + 1), unit = (size_t)1 << shift;
    unsigned char *h = packed_image;
    memset(packed_image, 0, sizeof(packed_image));
    memcpy(h, magic, 4);
    put32(h + 4, 0x18);
    put32(h + 8, (uint32_t)sizeof(iso));
    put32(h + 16, block);
    h[20] = (unsigned char)version;
    h[21] = (unsigned char)shift;
    for (uint32_t b = 0; b <= blocks; b++) {
        at = (at + unit - 1) & ~(unit - 1);
        uint32_t entry = (uint32_t)(at >> shift);
        if (b == blocks) { put32(h + 0x18 + 4 * b, entry); break; }
        const unsigned char *src = iso + (size_t)b * block;
        size_t n = sizeof(iso) - (size_t)b * block < block ? sizeof(iso) - (size_t)b * block : block, out = 0;
        int code = how(b);
        unsigned char *dst = packed_image + at;
        if (code == BLOCK_LZ4) out = lz4_encode(src, n, dst);
#ifdef VJO_TEST_ZLIB
        if (code == BLOCK_DEFLATE) {
            z_stream z;
            memset(&z, 0, sizeof(z));
            TEST_ASSERT(deflateInit2(&z, 9, Z_DEFLATED, -15, 9, Z_DEFAULT_STRATEGY) == Z_OK);
            z.next_in = (Bytef *)(uintptr_t)src; z.avail_in = (uInt)n;
            z.next_out = dst; z.avail_out = (uInt)(2 * block);
            TEST_ASSERT(deflate(&z, Z_FINISH) == Z_STREAM_END);
            out = 2 * block - z.avail_out;
            deflateEnd(&z);
        }
#endif
        int stored = code == BLOCK_STORED || !out || out + unit >= block;
        if (stored) {
            memcpy(dst, src, n);
            out = n;
        }
        /* v1 and ZSO: the high bit means stored; v2: stored by size, the high bit LZ4. */
        if (strcmp(magic, "CISO") || version < 2) entry |= stored ? 0x80000000u : 0;
        else entry |= !stored && code == BLOCK_LZ4 ? 0x80000000u : 0;
        if (version >= 2 && stored && !strcmp(magic, "CISO")) {
            /* v2 tells a stored block by its size: at least block_size. */
            out = block;
            if (n < block) memset(dst + n, 0, block - n);
        }
        put32(h + 0x18 + 4 * b, entry);
        at += out;
    }
    TEST_ASSERT(at <= sizeof(packed_image));
    return at;
}
static int all_stored(uint32_t b) { (void)b; return BLOCK_STORED; }
static int all_lz4(uint32_t b) { (void)b; return BLOCK_LZ4; }
static int mixed_lz4(uint32_t b) { return b % 3 == 0 ? BLOCK_STORED : BLOCK_LZ4; }
#ifdef VJO_TEST_ZLIB
static int all_deflate(uint32_t b) { (void)b; return BLOCK_DEFLATE; }
static int deflate_lz4(uint32_t b) { return b % 2 ? BLOCK_DEFLATE : BLOCK_LZ4; }
#endif

static void test_compressed_images(void)
{
    Mem m;
    size_t len, n;
    make_iso("PSP_GAME", "PARAM.SFO;1");
    struct { const char *magic; int version; uint32_t block; int shift; int (*how)(uint32_t); int format; } cases[] = {
        {"CISO", 1, 2048, 0, all_stored, VJO_PSP_CSO}, {"CISO", 0, 2048, 2, all_stored, VJO_PSP_CSO},
        {"ZISO", 1, 2048, 0, all_lz4, VJO_PSP_ZSO}, {"ZISO", 1, 2048, 3, mixed_lz4, VJO_PSP_ZSO},
        {"ZISO", 1, 8192, 2, mixed_lz4, VJO_PSP_ZSO}, {"CISO", 2, 2048, 0, mixed_lz4, VJO_PSP_CSO},
        {"CISO", 2, 4096, 4, all_lz4, VJO_PSP_CSO},
#ifdef VJO_TEST_ZLIB
        {"CISO", 1, 2048, 0, all_deflate, VJO_PSP_CSO}, {"CISO", 1, 2048, 2, all_deflate, VJO_PSP_CSO},
        {"CISO", 2, 2048, 1, deflate_lz4, VJO_PSP_CSO}, {"CISO", 1, 16384, 0, all_deflate, VJO_PSP_CSO},
#endif
    };
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        n = make_packed(cases[c].magic, cases[c].version, cases[c].block, cases[c].shift, cases[c].how);
        TEST_CHECK_(read_sfo(packed_image, n, &m, 1, &len) == cases[c].format && sfo_is_the_game(len), "case %u", c);
        /* Bounded: the header, eight index entries and one block per sector it needs. */
        TEST_CHECK_(m.bytes <= 0x28 + 4 * (8 + 2 * cases[c].block), "case %u read %zu", c, m.bytes);
    }
#ifdef VJO_TEST_ZLIB
    /* No deflate codec: a deflate block is refused, an LZ4 or stored one read. */
    n = make_packed("CISO", 1, 2048, 0, all_deflate);
    TEST_CHECK(read_sfo(packed_image, n, &m, 0, &len) == VJO_PSP_E_CODEC);
#endif
    /* Damaged images: a block that does not decode, index entries going
     * back or past the file's end, a short index, bad headers. */
    n = make_packed("ZISO", 1, 2048, 0, all_lz4);
    unsigned char *index = packed_image + 0x18;
    uint32_t e16 = index[64] | index[65] << 8 | index[66] << 16 | (uint32_t)index[67] << 24;
    packed_image[(e16 & 0x7FFFFFFFu) + 1] ^= 0x55;
    TEST_CHECK(read_sfo(packed_image, n, &m, 0, &len) < 0);
    n = make_packed("ZISO", 1, 2048, 0, all_lz4);
    put32(index + 68, 0x20);
    TEST_CHECK(read_sfo(packed_image, n, &m, 0, &len) == VJO_PSP_E_IMAGE);
    n = make_packed("ZISO", 1, 2048, 0, all_lz4);
    put32(index + 68, 0x7FFFFFFFu);
    TEST_CHECK(read_sfo(packed_image, n, &m, 0, &len) == VJO_PSP_E_IMAGE);
    n = make_packed("CISO", 1, 2048, 0, all_stored);
    TEST_CHECK(read_sfo(packed_image, 0x18 + 40, &m, 0, &len) == VJO_PSP_E_FORMAT);
    put32(packed_image + 16, 1000); /* not a multiple of a sector */
    TEST_CHECK(read_sfo(packed_image, n, &m, 0, &len) == VJO_PSP_E_FORMAT);
    put32(packed_image + 16, 65536); /* larger than the reader's block */
    TEST_CHECK(read_sfo(packed_image, n, &m, 0, &len) == VJO_PSP_E_FORMAT);
    put32(packed_image + 16, 2048);
    packed_image[20] = 3; /* a CSO version after 2 */
    TEST_CHECK(read_sfo(packed_image, n, &m, 0, &len) == VJO_PSP_E_FORMAT);
    packed_image[20] = 1;
    put32(packed_image + 8, 0); put32(packed_image + 12, 0); /* no image */
    TEST_CHECK(read_sfo(packed_image, n, &m, 0, &len) == VJO_PSP_E_FORMAT);
    put32(packed_image + 12, 0x40000000u); /* an index larger than the file */
    TEST_CHECK(read_sfo(packed_image, n, &m, 0, &len) == VJO_PSP_E_FORMAT);
    put32(packed_image + 8, 0xFFFFFFFFu); put32(packed_image + 12, 0xFFFFFFFFu); /* a total whose block count would wrap */
    TEST_CHECK(read_sfo(packed_image, n, &m, 0, &len) == VJO_PSP_E_FORMAT);
}

/* Random damage to every format: an error or the right SFO, never a read
 * outside the file (mem_read asserts it; ASan watches the rest). */
static void test_damaged_images_fuzz(void)
{
    static unsigned char copy[sizeof(packed_image)];
    uint32_t seed = 12345;
    Mem m;
    size_t len;
    for (int round = 0; round < 3; round++) {
        make_iso("PSP_GAME", "PARAM.SFO;1");
        size_t n = round == 0 ? sizeof(iso) : make_packed(round == 1 ? "ZISO" : "CISO", round == 1 ? 1 : 2, 2048, 1,
                                                          mixed_lz4);
        const unsigned char *src = round == 0 ? iso : packed_image;
        for (int i = 0; i < 1500; i++) {
            memcpy(copy, src, n);
            for (int k = 0; k < 1 + i % 8; k++) {
                seed = seed * 1103515245u + 12345u;
                size_t at = round == 0 ? 16 * VJO_PSP_SECTOR + (seed >> 8) % (5 * VJO_PSP_SECTOR) : (seed >> 8) % n;
                copy[at] ^= (unsigned char)(1u << (seed >> 29));
            }
            int rc = read_sfo(copy, n, &m, 1, &len);
            TEST_CHECK(rc < 0 || (len <= sizeof(sfo_out) && len > 0));
        }
    }
}

/* ---- the title IDs a bubble's row may carry (core/taicfg.c vjo_tai_title_ok, used by settings/psp.c) ---- */

static void test_title_ok(void)
{
    static const char *const bad[] = {"ALL", "KERNEL", "main", "MAIN", "ALL!", "", "ULJS0037", "ULJS003780",
                                      "uljs00378", "ULJS_0378", "ULJS-0378", "ULJS 0378", "\xEF\xBC\xB5LJS0037"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) TEST_CHECK_(!vjo_tai_title_ok(bad[i]), "%s", bad[i]);
    TEST_CHECK(!vjo_tai_title_ok(NULL));
    static const char *const good[] = {"ULJS00378", "PSPEMU005", "PSPEMUCFW", "PCSG00415", "ABCD12345", "000000000"};
    for (unsigned i = 0; i < sizeof(good) / sizeof(good[0]); i++) TEST_CHECK_(vjo_tai_title_ok(good[i]), "%s", good[i]);
}

TEST_LIST = {
    {"boot.bin: AdrBubbleBooter's layouts, magic and path", test_boot_bin},
    {"boot.inf: bootconv's PATH line", test_boot_inf},
    {"the app's description of a bubble's game", test_psp_info},
    {"EBOOT.PBP: its PARAM.SFO", test_pbp},
    {"ISO 9660: /PSP_GAME/PARAM.SFO, damaged images", test_iso},
    {"ISO 9660: a directory is read for 64 KiB at most", test_iso_directory_cap},
    {"LZ4 blocks", test_lz4},
    {"CSO v1/v2 and ZSO images", test_compressed_images},
    {"damaged images never read outside the file", test_damaged_images_fuzz},
    {"title IDs a bubble's row may carry", test_title_ok},
    {NULL, NULL}
};
