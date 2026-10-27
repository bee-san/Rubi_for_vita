/* The Rubi app's VjoPlatform (settings/app_platform.c: sceIo, as shell/platform_vita.c without its counters) on the
 * host's files, through sceIo stubs that cut every read short: a file's size, reads at any offset (again after a
 * short read), none past the end, a failed read, the close; it keeps no state. And with the bundled dictionary
 * (VJO_PLAINTEXT_DIR), the overlay's lookups through it are the host platform's, entry for entry. */
#define main acutest_main
#include "acutest.h"
#undef main
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <psp2/io/fcntl.h>
#include "../../settings/app_platform.h"
#include "client.h"
#include "config.h"
#include "local_dict.h"
#include "platform_posix.h"
#include "test_tmpdir.h"
#include "utf.h"

static int opens, closes, preads, short_reads, fail_pread_at;
static size_t pread_cap = 7; /* every read gets at most this: the read loop must go on */
int sceIoOpen(const char *p, int flags, unsigned mode)
{
    int fd = open(p, flags, mode);
    if (fd < 0)
        return (int)(0x80010000u | (unsigned)errno);
    opens++;
    return fd;
}
int sceIoClose(int fd)
{
    closes++;
    return close(fd);
}
SceOff sceIoLseek(int fd, SceOff off, int whence) { return lseek(fd, off, whence); }
int sceIoPread(int fd, void *p, size_t n, SceOff off)
{
    if (++preads == fail_pread_at)
        return (int)0x80010005u; /* EIO */
    if (n > pread_cap) {
        n = pread_cap;
        short_reads++;
    }
    return (int)pread(fd, p, n, off);
}
uint64_t sceKernelGetProcessTimeWide(void) { return 123456789ull; }

static void reads_sizes_and_short_reads(void)
{
    VjoPlatform p;
    VjoFile f;
    uint8_t data[1000], got[1000];
    char path[PATH_MAX];
    FILE *out;
    for (int i = 0; i < 1000; i++)
        data[i] = (uint8_t)(i * 7 + 3);
    TEST_ASSERT(snprintf(path, sizeof(path), "%s/data.bin", vjo_test_tmpdir_path) < (int)sizeof(path));
    TEST_ASSERT((out = fopen(path, "wb")) && fwrite(data, 1, sizeof(data), out) == sizeof(data) && !fclose(out));
    memset(&p, 0x5A, sizeof(p));
    vjo_app_platform(&p);
    /* stateless: no session, no text generation, no user data; the process clock */
    TEST_CHECK(!p.ud && !p.local_session && !p.local_text_generation && p.file_open && p.now_us);
    TEST_CHECK(p.now_us(p.ud) == 123456789ull);
    memset(&f, 0x5A, sizeof(f));
    TEST_ASSERT(p.file_open(p.ud, path, &f) == 0);
    TEST_CHECK(f.size == 1000 && f.stamp == 0 && f.read && f.close && opens == 1);
    /* at the start, in the middle, to the last byte: each in 7-byte reads */
    preads = short_reads = 0;
    TEST_CHECK(f.read(f.ctx, 0, got, 100) == 0 && !memcmp(got, data, 100) && preads == 15 && short_reads == 14);
    TEST_CHECK(f.read(f.ctx, 513, got, 487) == 0 && !memcmp(got, data + 513, 487));
    TEST_CHECK(f.read(f.ctx, 999, got, 1) == 0 && got[0] == data[999]);
    TEST_CHECK(f.read(f.ctx, 0, got, 0) == 0);
    /* past the end, or a failed read: -1 */
    TEST_CHECK(f.read(f.ctx, 990, got, 20) == -1);
    TEST_CHECK(f.read(f.ctx, 1000, got, 1) == -1);
    TEST_CHECK(f.read(f.ctx, UINT64_MAX - 3, got, 8) == -1);
    preads = 0;
    fail_pread_at = 3;
    TEST_CHECK(f.read(f.ctx, 0, got, 100) == -1);
    fail_pread_at = 0;
    f.close(f.ctx);
    TEST_CHECK(closes == 1);
    /* a file that is not there: nothing opened */
    TEST_ASSERT(snprintf(path, sizeof(path), "%s/none.bin", vjo_test_tmpdir_path) < (int)sizeof(path));
    TEST_CHECK(p.file_open(p.ud, path, &f) == -1 && opens == 1 && closes == 1);
}

/* ---- the bundled dictionary: the same entries through either platform ---- */

static int same_vocab(const VjoVocab *a, const VjoVocab *b)
{
    if (strcmp(a->spelling, b->spelling) || strcmp(a->reading, b->reading) || a->rank != b->rank ||
        a->n_meanings != b->n_meanings || a->n_pos != b->n_pos ||
        strcmp(a->dictionary ? a->dictionary : "", b->dictionary ? b->dictionary : ""))
        return 0;
    for (int i = 0; i < a->n_meanings; i++)
        if (strcmp(a->meanings[i], b->meanings[i]))
            return 0;
    return 1;
}

/* One lookup block as shell/worker.c lays it out: the dictionary session at its start, the arena after it. */
typedef struct {
    uint8_t mem[64 * 1024] __attribute__((aligned(16)));
    VjoArena arena;
    VjoPlatform p;
} Block;
static Block app, host;
static PosixPlatform posix;

static void block_init(Block *b, int app_side)
{
    size_t session = vjo_local_session_size();
    if (app_side)
        vjo_app_platform(&b->p);
    else
        posix_platform_init(&posix, &b->p);
    memset(b->mem, 0, session);
    b->p.local_session = (VjoLocalSession *)b->mem;
    vjo_arena_init(&b->arena, b->mem + session, sizeof(b->mem) - session);
}

static void lookups_through_it_are_the_host_platform_s(void)
{
    static const char *const sentences[] = {
        "昨日すき焼きを食べました", "「また明日、一緒に見に行こう！」", "猫が好きだ。\n犬も好きです",
        "勉強しておかなきゃ", "朋也くん、おはよう", "ＡＢＣとabc123",
    };
    const char *dir = getenv("VJO_PLAINTEXT_DIR");
    VjoConfig cfg;
    int lookups = 0, entries = 0, stacked = 0;
    if (!dir) {
        TEST_SKIP("VJO_PLAINTEXT_DIR is not set: no bundled dictionary");
        return;
    }
    pread_cap = 1u << 20; /* whole reads: the lookups' bytes are what is compared */
    vjo_config_defaults(&cfg);
    snprintf(cfg.local_dictionary_dir, sizeof(cfg.local_dictionary_dir), "%s", dir);
    snprintf(cfg.local_dictionaries, sizeof(cfg.local_dictionaries), "plaintext.vjdict,plaintext.vjdict");
    for (int results = 1; results <= 2; results++) {
        cfg.local_dictionary_results = results;
        block_init(&app, 1);
        block_init(&host, 0);
        for (unsigned s = 0; s < sizeof(sentences) / sizeof(sentences[0]); s++) {
            size_t len = strlen(sentences[s]);
            for (size_t cursor = 0, next = 0; cursor < len; cursor = next) {
                vjo_utf8_next(sentences[s], len, &next); /* the next character's start */
                VjoOverlayData a, h;
                vjo_arena_reset(&app.arena);
                vjo_arena_reset(&host.arena);
                TEST_ASSERT(vjo_overlay_ocr_text(&app.arena, &cfg, sentences[s], &a) == VJO_OK);
                TEST_ASSERT(vjo_overlay_ocr_text(&host.arena, &cfg, sentences[s], &h) == VJO_OK);
                if (cursor >= strlen(a.sentence))
                    break;
                int ra = vjo_overlay_lookup_at(&app.arena, &app.p, &cfg, cursor, &a);
                int rh = vjo_overlay_lookup_at(&host.arena, &host.p, &cfg, cursor, &h);
                lookups++;
                TEST_CHECK_(ra == rh && ra == VJO_OK && a.list.n_entries == h.list.n_entries &&
                                a.list.n_more == h.list.n_more,
                            "'%s' at %zu: rc %d/%d, %d/%d entries, %d/%d stacked", sentences[s], cursor, ra, rh,
                            a.list.n_entries, h.list.n_entries, a.list.n_more, h.list.n_more);
                if (ra || rh || a.list.n_entries != h.list.n_entries || !a.list.n_entries)
                    continue;
                entries++;
                const VjoEntry *ea = &a.list.entries[0], *eh = &h.list.entries[0];
                TEST_CHECK_(same_vocab(ea->vocab, eh->vocab) && ea->hl_start == eh->hl_start && ea->hl_end == eh->hl_end &&
                                ea->conjugation == eh->conjugation && !strcmp(ea->text, eh->text),
                            "'%s' at %zu: %s / %s", sentences[s], cursor, ea->vocab->spelling, eh->vocab->spelling);
                for (int k = 0; k < a.list.n_more && k < h.list.n_more; k++, stacked++)
                    TEST_CHECK(same_vocab(a.list.more[k], h.list.more[k]));
            }
        }
        vjo_local_session_close(app.p.local_session);
        vjo_local_session_close(host.p.local_session);
    }
    TEST_CHECK_(opens == closes, "%d opens, %d closes", opens, closes);
    TEST_CHECK_(lookups > 60 && entries > 40 && stacked > 20, "%d lookups, %d entries, %d stacked", lookups, entries,
                stacked);
    TEST_MSG("%d lookups, %d entries, %d stacked", lookups, entries, stacked);
}

TEST_LIST = {
    {"reads, sizes and short reads; no state", reads_sizes_and_short_reads},
    {"the overlay's lookups through it are the host platform's", lookups_through_it_are_the_host_platform_s},
    {NULL, NULL},
};

int main(int argc, char **argv)
{
    if (!vjo_test_tmpdir("app-platform")) /* the suite's files, removed when it ends */
        return 2;
    return acutest_main(argc, argv);
}
