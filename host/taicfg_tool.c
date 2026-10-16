/* vjo-taicfg: the Overlay app's config.txt editor (core/taicfg.c) and taiHEN's own parser
 * (third_party/taihen-parser) on the host, for tools/tests/test_tai_config.py's differential tests
 * and for writing the fixtures' expected results (tools/tests/fixtures/taicfg/).
 *
 *   vjo-taicfg edit FILE install|uninstall kernel|lines [TITLE...]
 *       the result on stdout (exit 0), or "REFUSED rc=N line=N: why" (exit 1)
 *   vjo-taicfg parse FILE TITLE      the paths taiHEN loads for TITLE, one per line
 *   vjo-taicfg validate FILE         taihen_config_validate's value (0: taiHEN keeps the file)
 *   vjo-taicfg expect DIR            writes DIR/NAME.{kernel,lines,uninstall}.out or NAME.refused
 *                                    for every DIR/NAME.in (with DIR/NAME.titles for lines mode) */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <taihen/parser.h>
#include "taicfg.h"

static char text[VJO_TAI_TEXT_MAX + 2], out[VJO_TAI_TEXT_MAX + 1];

static long slurp(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    int more = fgetc(f) != EOF;
    fclose(f);
    buf[n] = '\0';
    return more ? -2 : (long)n;
}

static void print_path(const char *module, void *param)
{
    (void)param;
    printf("%s\n", module);
}

/* An edit's result into path (a refusal as its .refused file). */
static int edit_into(const char *dir, const char *name, long n, int op, int mode, const char *const *titles,
                     int ntitles, const char *suffix)
{
    char path[512];
    VjoTaiReport r;
    int rc = vjo_tai_edit(text, (size_t)n, op, mode, titles, ntitles, out, sizeof(out), &r);
    snprintf(path, sizeof(path), "%s/%s.%s", dir, name, rc < 0 ? "refused" : suffix);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (rc < 0) fprintf(f, "rc=%d line=%d\n", rc, r.line);
    else fwrite(out, 1, (size_t)rc, f);
    fclose(f);
    return rc;
}

static int expect(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int files = 0;
    if (!d) return 2;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        char name[256], path[512], titles_text[1024], *titles[16];
        int ntitles = 0;
        if (len < 4 || strcmp(e->d_name + len - 3, ".in") || len - 3 >= sizeof(name)) continue;
        memcpy(name, e->d_name, len - 3);
        name[len - 3] = '\0';
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        long n = slurp(path, text, sizeof(text));
        if (n < 0) { fprintf(stderr, "%s: too large\n", path); continue; }
        snprintf(path, sizeof(path), "%s/%s.titles", dir, name);
        if (slurp(path, titles_text, sizeof(titles_text)) > 0)
            for (char *s = strtok(titles_text, "\n"); s && ntitles < 16; s = strtok(NULL, "\n")) titles[ntitles++] = s;
        if (edit_into(dir, name, n, VJO_TAI_INSTALL, VJO_TAI_WORKER_KERNEL, NULL, 0, "kernel.out") >= 0) {
            edit_into(dir, name, n, VJO_TAI_INSTALL, VJO_TAI_WORKER_LINES, (const char *const *)titles, ntitles,
                      "lines.out");
            edit_into(dir, name, n, VJO_TAI_UNINSTALL, VJO_TAI_WORKER_KERNEL, NULL, 0, "uninstall.out");
        }
        files++;
    }
    closedir(d);
    printf("%d fixtures\n", files);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "expect")) return expect(argv[2]);
    if (argc < 3) {
        fprintf(stderr, "usage: vjo-taicfg edit|parse|validate|expect ...\n");
        return 2;
    }
    long n = slurp(argv[2], text, sizeof(text));
    if (n == -1) { perror(argv[2]); return 2; }
    if (n == -2) n = VJO_TAI_TEXT_MAX + 1; /* over the editor's limit: it refuses */
    if (!strcmp(argv[1], "parse") && argc == 4) {
        taihen_config_parse(text, argv[3], print_path, NULL);
        return 0;
    }
    if (!strcmp(argv[1], "validate")) {
        printf("%d\n", taihen_config_validate(text));
        return 0;
    }
    if (!strcmp(argv[1], "edit") && argc >= 5) {
        VjoTaiReport r;
        int op = !strcmp(argv[3], "uninstall") ? VJO_TAI_UNINSTALL : VJO_TAI_INSTALL;
        int mode = !strcmp(argv[4], "lines") ? VJO_TAI_WORKER_LINES : VJO_TAI_WORKER_KERNEL;
        int rc = vjo_tai_edit(text, (size_t)n, op, mode, (const char *const *)argv + 5, argc - 5, out, sizeof(out), &r);
        if (rc < 0) {
            printf("REFUSED rc=%d line=%d: %s\n", rc, r.line, r.why);
            return 1;
        }
        fwrite(out, 1, (size_t)rc, stdout);
        fprintf(stderr, "changed=%d added=%d sections_added=%d commented=%d removed=%d headers_removed=%d padded=%d "
                "bom_hides_section=%d removed_sections=%s\n", r.changed, r.added, r.sections_added, r.commented,
                r.removed, r.headers_removed, r.padded, r.bom_hides_section, r.removed_sections);
        return 0;
    }
    fprintf(stderr, "usage: vjo-taicfg edit|parse|validate|expect ...\n");
    return 2;
}
