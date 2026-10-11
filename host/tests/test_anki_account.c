#include "acutest.h"
#include "../../settings/anki_account.h"
#include <psp2/io/fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static unsigned char memory[32 * 1024];
static VjoArena arena;
static int write_fail, close_fail, sync_fail, rename_fail, remove_fail;
static int syncs, renames, handles, write_fd = -1;
static size_t written;
static int native(int n) { return n < 0 ? (int)(0x80010000u | errno) : n; }
int sceIoOpen(const char *p, int flags, unsigned mode)
{ int fd = open(p, flags, mode); if (fd >= 0) { handles++; if (flags & SCE_O_WRONLY) write_fd = fd; } return native(fd); }
int sceIoClose(int fd)
{ int rc = close(fd); if (!rc) handles--; int failed = close_fail && fd == write_fd;
  if (fd == write_fd) write_fd = -1; return failed ? -1 : native(rc); }
int sceIoRead(int fd, void *p, size_t n) { return native(read(fd, p, n > 13 ? 13 : n)); }
int sceIoWrite(int fd, const void *p, size_t n)
{ if (write_fail && written >= 21) return -1; int rc = write(fd, p, n > 7 ? 7 : n);
  if (rc > 0) written += rc; return native(rc); }
SceOff sceIoLseek(int fd, SceOff off, int whence) { return lseek(fd, off, whence); }
int sceIoMkdir(const char *p, unsigned mode) { return native(mkdir(p, mode)); }
int sceIoGetstat(const char *p, SceIoStat *out)
{ struct stat st; int rc = stat(p, &st); if (!rc) out->st_mode = st.st_mode; return native(rc); }
int sceIoRename(const char *a, const char *b) { return ++renames == rename_fail ? -1 : native(rename(a, b)); }
int sceIoRemove(const char *p) { return remove_fail ? -1 : native(unlink(p)); }
int sceIoSync(const char *p, unsigned flags) { (void)p; (void)flags; return ++syncs == sync_fail ? -1 : 0; }
static void put(const char *path, const char *text)
{ int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600); TEST_ASSERT(fd >= 0);
  TEST_ASSERT(write(fd, text, strlen(text)) == (ssize_t)strlen(text)); TEST_ASSERT(close(fd) == 0); }
static void reset(void)
{
    static int initialized;
    if (!initialized) { char dir[] = "/tmp/vjo-account-XXXXXX";
        TEST_ASSERT(mkdtemp(dir)); TEST_ASSERT(chdir(dir) == 0);
        mkdir("ux0:data", 0777); mkdir("ux0:data/VitaJPOverlay", 0777); initialized = 1; }
    unlink(VJO_ANKI_ACCOUNT_PATH); unlink(VJO_ANKI_ACCOUNT_PATH ".bak"); unlink(VJO_ANKI_ACCOUNT_PATH ".tmp");
    write_fail = close_fail = sync_fail = rename_fail = remove_fail = syncs = renames = handles = 0;
    write_fd = -1; written = 0; memset(memory, 0xaa, sizeof(memory));
    vjo_arena_init(&arena, memory, sizeof(memory));
}
static void released(void)
{ TEST_CHECK(arena.used == 0 && handles == 0);
  for (size_t i = 0; i < arena.peak; i++) TEST_ASSERT(memory[i] == 0); }
static void test_round_trip_and_limits(void)
{
    reset(); VjoAnkiAccount value = {{0}, {0}}, out;
    TEST_ASSERT(vjo_anki_account_load(&out, &arena) == 0);
    TEST_CHECK(!out.username[0] && !out.password[0]);
    strcpy(value.username, "日本語@example.com"); strcpy(value.password, "a\"b\\u0000\n🌳");
    TEST_ASSERT(vjo_anki_account_save(&value, &arena) == 0);
    TEST_ASSERT(vjo_anki_account_load(&out, &arena) == 0);
    TEST_CHECK(!strcmp(value.username, out.username) && !strcmp(value.password, out.password)); released();
    memset(value.password, 'x', 255); value.password[255] = 0;
    TEST_ASSERT(vjo_anki_account_save(&value, &arena) == 0);
    TEST_ASSERT(vjo_anki_account_load(&out, &arena) == 0);
    TEST_CHECK(strlen(out.password) == 255); released();
    memset(value.password, 'x', sizeof(value.password));
    TEST_CHECK(vjo_anki_account_save(&value, &arena) < 0);
    TEST_ASSERT(vjo_anki_account_load(&out, &arena) == 0); TEST_CHECK(strlen(out.password) == 255);
}
static void test_failed_saves_keep_confirmable_account(void)
{
    for (int fault = 0; fault < 6; fault++) {
        reset(); VjoAnkiAccount first = {"old@example.com", "old-secret"}, next = {"new@example.com", "new-secret"}, out;
        TEST_ASSERT(vjo_anki_account_save(&first, &arena) == 0); written = syncs = renames = 0;
        if (!fault) write_fail = 1;
        if (fault == 1) close_fail = 1;
        if (fault == 2) sync_fail = 1;
        if (fault == 3) rename_fail = 1;
        if (fault == 4) rename_fail = 2;
        if (fault == 5) sync_fail = 2;
        TEST_CHECK(vjo_anki_account_save(&next, &arena) < 0);
        TEST_ASSERT(vjo_anki_account_load(&out, &arena) >= 0);
        TEST_CHECK(!strcmp(out.username, fault == 5 ? next.username : first.username));
        TEST_CHECK(!strcmp(out.password, fault == 5 ? next.password : first.password)); released();
    }
    reset(); put(VJO_ANKI_ACCOUNT_PATH ".bak", "{\"version\":1,\"username\":\"a\",\"password\":\"b\"}");
    VjoAnkiAccount out; TEST_CHECK(vjo_anki_account_load(&out, &arena) == 1);
    TEST_CHECK(!strcmp(out.password, "b")); released();
}
static void test_bad_accounts_are_not_accepted(void)
{
    const char *bad[] = {"", "{}", "{\"version\":1.5,\"username\":\"a\",\"password\":\"b\"}",
        "{\"version\":1,\"username\":\"a\",\"password\":\"b\\u0000c\"}",
        "{\"version\":1,\"username\":\"a\",\"password\":null}",
        "{\"version\":1,\"username\":\"a\",\"password\":\"b\"} {}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        reset(); put(VJO_ANKI_ACCOUNT_PATH, bad[i]); VjoAnkiAccount out;
        TEST_CHECK(vjo_anki_account_load(&out, &arena) < 0);
        TEST_CHECK(!out.username[0] && !out.password[0]); released();
    }
}
static void test_forget_removes_recovery_copies_only(void)
{
    reset(); VjoAnkiAccount one = {"old", "secret"}, two = {"new", "secret2"}, out;
    TEST_ASSERT(vjo_anki_account_save(&one, &arena) == 0);
    TEST_ASSERT(vjo_anki_account_save(&two, &arena) == 0);
    put(VJO_ANKI_ACCOUNT_PATH ".tmp", "partial-secret");
    put("ux0:data/VitaJPOverlay/config.ini", "untouched");
    put("ux0:data/VitaJPOverlay/anki_queue.jsonl", "untouched");
    remove_fail = 1; TEST_CHECK(vjo_anki_account_forget() < 0); remove_fail = 0;
    TEST_ASSERT(vjo_anki_account_forget() == 0);
    TEST_CHECK(access(VJO_ANKI_ACCOUNT_PATH, F_OK) < 0 && access(VJO_ANKI_ACCOUNT_PATH ".bak", F_OK) < 0);
    TEST_CHECK(access(VJO_ANKI_ACCOUNT_PATH ".tmp", F_OK) < 0);
    TEST_ASSERT(vjo_anki_account_load(&out, &arena) == 0); TEST_CHECK(!out.username[0] && !out.password[0]);
    TEST_CHECK(access("ux0:data/VitaJPOverlay/config.ini", F_OK) == 0);
    TEST_CHECK(access("ux0:data/VitaJPOverlay/anki_queue.jsonl", F_OK) == 0); released();
}
TEST_LIST = {
    {"account_UTF8_escapes_and_255_byte_fields_round_trip", test_round_trip_and_limits},
    {"failed_account_writes_and_publication_preserve_confirmable_login", test_failed_saves_keep_confirmable_account},
    {"corrupt_NUL_and_wrong_version_accounts_are_rejected", test_bad_accounts_are_not_accepted},
    {"forget_removes_primary_backup_and_temporary_login_without_touching_queue", test_forget_removes_recovery_copies_only},
    {NULL, NULL}
};
