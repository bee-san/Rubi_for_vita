/* The host suites' temporary files: one directory for a suite's run, removed with everything in it
 * when the suite ends (the pattern of #164's game_shot fix, for every suite).
 *
 * main() calls vjo_test_tmpdir("NAME") before acutest_main: it makes $TMPDIR/vjo-NAME-XXXXXX (/tmp
 * when TMPDIR is unset or empty), keeps its absolute path, and registers an atexit handler that
 * removes the whole tree. acutest runs each test in a forked child that ends in exit() too, so the
 * handler acts only in the process that made the directory, the suite's own: a test's exit never
 * removes it under the next test, and whatever a test leaves there (a failed assertion, a crash,
 * its own forked processes' files) goes when the suite ends. vjo_test_tmpdir_new() gives a test a
 * new empty directory inside it, as mkdtemp under /tmp did before. vjo_test_remove_tree() removes a
 * tree in-process (no shell, so no quoting of TMPDIR): it never follows a symbolic link and never
 * touches a path it could not spell whole.
 *
 * ctest gives every test a TMPDIR of its own, which must be empty again when the run ends
 * (tmp_leak_setup and tmp_leak_check, host/tests/tmp_leak.py); tools/arm_host_tests.sh checks its
 * suites the same way. NAME is the source's name, test_NAME.c with '-' for '_', so a leftover
 * directory names its suite. */
#ifndef VJO_TEST_TMPDIR_H
#define VJO_TEST_TMPDIR_H
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static char vjo_test_tmpdir_path[PATH_MAX]; /* the suite's directory, absolute */
static pid_t vjo_test_tmpdir_owner;

/* path and, for a directory, everything under it: 0 when path is gone (or never was). A symbolic
 * link is removed, never followed; an entry whose path would not fit PATH_MAX is left, and so is
 * the directory holding it (-1). */
static inline int vjo_test_remove_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            char child[PATH_MAX];
            int n;
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            n = snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            if (n > 0 && n < (int)sizeof(child)) vjo_test_remove_tree(child);
        }
        if (d) closedir(d);
        return rmdir(path) == 0 ? 0 : -1;
    }
    return unlink(path) == 0 ? 0 : -1;
}

static inline void vjo_test_tmpdir_remove(void)
{
    int out;
    if (!vjo_test_tmpdir_path[0] || getpid() != vjo_test_tmpdir_owner) return; /* a forked test's exit */
    out = chdir("/"); /* the suites work inside it */
    (void)out;
    vjo_test_remove_tree(vjo_test_tmpdir_path);
}

/* Makes the suite's directory, owned by this process (call it from main, before acutest_main forks
 * the tests); its absolute path, or NULL when it cannot be made. */
static inline const char *vjo_test_tmpdir(const char *name)
{
    const char *base = getenv("TMPDIR");
    char made[PATH_MAX];
    int n;
    if (vjo_test_tmpdir_path[0]) return vjo_test_tmpdir_path;
    if (!base || !*base) base = "/tmp";
    n = snprintf(made, sizeof(made), "%s/vjo-%s-XXXXXX", base, name);
    if (n < 0 || n >= (int)sizeof(made) || !mkdtemp(made)) {
        fprintf(stderr, "cannot make a temporary directory vjo-%s-XXXXXX in %s\n", name, base);
        return NULL;
    }
    /* absolute: the handler removes it from "/", and a relative TMPDIR names the start's directory */
    if (!realpath(made, vjo_test_tmpdir_path)) {
        fprintf(stderr, "cannot resolve the temporary directory %s\n", made);
        rmdir(made);
        vjo_test_tmpdir_path[0] = '\0';
        return NULL;
    }
    vjo_test_tmpdir_owner = getpid();
    if (atexit(vjo_test_tmpdir_remove) != 0) {
        rmdir(vjo_test_tmpdir_path);
        vjo_test_tmpdir_path[0] = '\0';
        return NULL;
    }
    return vjo_test_tmpdir_path;
}

/* A new empty directory in the suite's: its path in dir (cap bytes), or NULL (dir then empty) when
 * main made no suite directory or this one cannot be made. It goes when the suite's does. */
static inline char *vjo_test_tmpdir_new(char *dir, size_t cap)
{
    int n;
    if (!cap) return NULL;
    dir[0] = '\0';
    if (!vjo_test_tmpdir_path[0]) return NULL;
    n = snprintf(dir, cap, "%s/XXXXXX", vjo_test_tmpdir_path);
    if (n < 0 || (size_t)n >= cap || !mkdtemp(dir)) {
        dir[0] = '\0';
        return NULL;
    }
    return dir;
}
#endif
