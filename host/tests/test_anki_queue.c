/* Real queue implementation on POSIX, with short writes and injected failures. */
#include "acutest.h"
#include "../../shell/anki_queue.h"
#include <psp2/io/fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char memory[64 * 1024];
static VjoArena arena;
static VjoConfig cfg;
static int fail_after, sync_fail, writes;
static const char *meanings[] = {"cat \"neko\"", "<pet>&\nline"};
static VjoAnkiNote note = {"猫", "ねこ", meanings, 2, "猫が\n好き𠮷", 0, 3};
static int native(int n) { return n < 0 ? (int)(0x80010000u | errno) : n; }
int sceIoOpen(const char *p, int f, unsigned mode) { return native(open(p, f, mode)); }
int sceIoClose(int f) { return native(close(f)); }
int sceIoRead(int f, void *p, size_t n) { return native(read(f, p, n > 17 ? 17 : n)); }
int sceIoWrite(int f, const void *p, size_t n)
{ if (fail_after && ++writes >= fail_after) return -1; return native(write(f, p, n > 7 ? 7 : n)); }
SceOff sceIoLseek(int f, SceOff p, int w) { return lseek(f, p, w); }
int sceIoMkdir(const char *p, unsigned m) { return native(mkdir(p, m)); }
int sceIoChstatByFd(int f, const SceIoStat *s, unsigned bits)
{ TEST_CHECK(bits == SCE_CST_SIZE); return native(ftruncate(f, s->st_size)); }
int sceIoSyncByFd(int f, int flag) { (void)flag; return sync_fail ? -1 : fsync(f); }
int sceIoSync(const char *p, unsigned flags) { (void)p; (void)flags; return sync_fail ? -1 : 0; }
static void reset(void)
{
    static int initialized;
    if (!initialized) {
        char dir[] = "/tmp/vjo-file-queue-XXXXXX";
        TEST_ASSERT(mkdtemp(dir)); TEST_ASSERT(chdir(dir) == 0); initialized = 1;
    }
    unlink(VJO_QUEUE_PATH); fail_after = sync_fail = writes = 0;
    vjo_arena_init(&arena, memory, sizeof(memory)); vjo_config_defaults(&cfg);
}
static char *read_queue(void)
{
    FILE *f = fopen(VJO_QUEUE_PATH, "rb"); TEST_ASSERT(f);
    TEST_ASSERT(fseek(f, 0, SEEK_END) == 0); long n = ftell(f); rewind(f);
    char *p = malloc((size_t)n + 1); TEST_ASSERT(p);
    TEST_ASSERT(fread(p, 1, (size_t)n, f) == (size_t)n); p[n] = 0; fclose(f); return p;
}
static void test_roundtrip(void)
{
    reset(); TEST_CHECK(vjo_queue_count() == 0);
    TEST_ASSERT(vjo_queue_save(&arena, &note, &cfg, "PCSG00415") == 0);
    TEST_CHECK(vjo_queue_count() == 1 && arena.used == 0);
    char *p = read_queue();
    TEST_CHECK(strstr(p, "\"version\":2") && strstr(p, "PCSG00415") && strstr(p, "cat \\\"neko\\\""));
    TEST_CHECK(!strstr(p, "rank") && !strstr(p, "password") && !strstr(p, "picture"));
    TEST_CHECK(vjo_queue_save(&arena, &note, &cfg, "PCSG00415") == 1 && vjo_queue_count() == 1);
    /* Same word from a different source/sentence is a distinct mined record. */
    TEST_CHECK(vjo_queue_save(&arena, &note, &cfg, "PCSG00001") == 0 && vjo_queue_count() == 2);
    free(p);
}
static void test_torn_append(void)
{
    reset(); TEST_ASSERT(vjo_queue_save(&arena, &note, &cfg, "PCSG00415") == 0);
    char *before = read_queue();
    /* Interrupt every write boundary of a second record, then restart. */
    for (int boundary = 1; boundary < 90; boundary++) {
        FILE *f = fopen(VJO_QUEUE_PATH, "wb"); TEST_ASSERT(f);
        TEST_ASSERT(fwrite(before, 1, strlen(before), f) == strlen(before)); fclose(f);
        fail_after = boundary; writes = 0;
        int rc = vjo_queue_save(&arena, &note, &cfg, "PCSG00001");
        fail_after = 0; vjo_arena_init(&arena, memory, sizeof(memory));
        TEST_CHECK(rc < 0 || rc == 0);
        TEST_CHECK(vjo_queue_save(&arena, &note, &cfg, "PCSG00001") >= 0);
        TEST_CHECK(vjo_queue_count() == 2 && arena.used == 0);
        char *after = read_queue(); TEST_CHECK(!memcmp(before, after, strlen(before))); free(after);
    }
    free(before);
}
static void test_sync_and_corruption(void)
{
    reset(); sync_fail = 1;
    TEST_CHECK(vjo_queue_save(&arena, &note, &cfg, "PCSG00415") < 0);
    sync_fail = 0; TEST_CHECK(vjo_queue_save(&arena, &note, &cfg, "PCSG00415") == 1);
    char *p = read_queue(); size_t n = strlen(p); p[7] = p[7] == 'a' ? 'b' : 'a';
    FILE *f = fopen(VJO_QUEUE_PATH, "wb"); TEST_ASSERT(f); fwrite(p, 1, n, f); fclose(f);
    TEST_CHECK(vjo_queue_count() == -1);
    TEST_CHECK(vjo_queue_save(&arena, &note, &cfg, "PCSG00001") == -1);
    char *after = read_queue(); TEST_CHECK(!strcmp(p, after)); free(p); free(after);
}
static void test_memory_and_limit(void)
{
    reset(); vjo_arena_init(&arena, memory, 32);
    TEST_CHECK(vjo_queue_save(&arena, &note, &cfg, "PCSG00415") == -3 && arena.used == 0);
    TEST_CHECK(vjo_queue_count() == 0);
    const char *ini = "anki_mining=on\nanki_host=auto\nanki_audio_url=secret\n";
    vjo_config_parse(&cfg, ini, strlen(ini));
    TEST_CHECK(cfg.anki_mining && !cfg.n_warnings);
}
TEST_LIST = {{"roundtrip", test_roundtrip}, {"interrupted_append_restart", test_torn_append},
             {"uncertain_sync_and_corruption", test_sync_and_corruption}, {"bounded_memory", test_memory_and_limit}, {NULL, NULL}};
