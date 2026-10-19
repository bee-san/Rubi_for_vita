/* The Rubi app's request for Rubi's duplicate index (anki_index_request.h). */
#include "anki_index_request.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <string.h>

#include "../core/json.h"
#include "preferences.h"

#define JSON_MAX (64u * 1024u) /* the interface's limit for a JSON file */
#define TMP VJO_ANKI_REQUEST_PATH ".tmp"
#define OLD VJO_ANKI_REQUEST_PATH ".old"

/* ---------------- config.ini ---------------- */

static int is_blank(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (lower((unsigned char)*a) != lower((unsigned char)*b))
            return 0;
    return *a == *b;
}

/* Blanks and BOMs before a key, as core/config.c's vjo_config_line_start. */
static size_t line_start(const char *line, size_t n)
{
    size_t s = 0;
    for (;;) {
        if (s < n && is_blank(line[s]))
            s++;
        else if (n - s >= 3 && !memcmp(line + s, "\xEF\xBB\xBF", 3))
            s += 3;
        else
            return s;
    }
}

/* A field's name for the request: 1..VJO_ANKI_FIELD_MAX bytes, no control character. */
static int name_ok(const char *s, size_t n)
{
    if (!n || n > VJO_ANKI_FIELD_MAX)
        return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] < 0x20 || s[i] == 0x7F)
            return 0;
    return 1;
}

void vjo_anki_request_config(VjoAnkiRequestConfig *c, const char *text, size_t len)
{
    size_t i = 0;
    memset(c, 0, sizeof(*c));
    c->on = 1;
    memcpy(c->field, "Expression", sizeof("Expression"));
    memcpy(c->reading, "ExpressionReading", sizeof("ExpressionReading"));
    while (i < len) {
        const char *line = text + i, *eq;
        size_t n = 0, s, e, v;
        char key[40];
        while (i < len && text[i] != '\n')
            i++, n++;
        i++;
        s = line_start(line, n);
        if (s == n || line[s] == ';' || line[s] == '#' || !(eq = memchr(line + s, '=', n - s)))
            continue;
        for (e = (size_t)(eq - line); e > s && is_blank(line[e - 1]); e--) {}
        if (e - s >= sizeof(key))
            continue; /* longer than any of these names */
        memcpy(key, line + s, e - s);
        key[e - s] = '\0';
        /* the value: up to an inline comment (';' or '#' after a blank), trimmed */
        for (v = (size_t)(eq - line) + 1, e = v; e < n; e++)
            if ((line[e] == ';' || line[e] == '#') && is_blank(line[e - 1]))
                break;
        while (v < e && is_blank(line[v]))
            v++;
        while (e > v && is_blank(line[e - 1]))
            e--;
        if (ieq(key, "anki_duplicate_check")) {
            char val[4] = "";
            if (e - v < sizeof(val)) {
                memcpy(val, line + v, e - v);
                val[e - v] = '\0';
            }
            if (ieq(val, "on") || ieq(val, "off"))
                c->on = ieq(val, "on");
            else
                c->warnings++;
        } else if (ieq(key, "anki_duplicate_field") || ieq(key, "anki_duplicate_reading_field")) {
            int reading = ieq(key, "anki_duplicate_reading_field");
            char *dst = reading ? c->reading : c->field;
            if (reading && ((e - v == 4 && !strncmp(line + v, "none", 4)) || e == v)) {
                c->reading[0] = '\0'; /* no reading field: every note gives its form's key */
            } else if (name_ok(line + v, e - v)) {
                memcpy(dst, line + v, e - v);
                dst[e - v] = '\0';
            } else {
                c->warnings++;
            }
        }
    }
}

/* ---------------- files ---------------- */

/* A whole file of at most max bytes, NUL-terminated, in the arena: 0 (*out, *len), 1 when there is
 * none, -1 (an error: *rc the SDK's). */
static int read_file(VjoArena *a, const char *path, size_t max, char **out, size_t *len, int *rc)
{
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    SceOff size;
    char *buf;
    size_t used = 0;
    int r = -1;
    *rc = fd;
    if (fd < 0)
        return fd == (SceUID)0x80010002 ? 1 : -1; /* SCE_ERROR_ERRNO_ENOENT */
    size = sceIoLseek(fd, 0, SCE_SEEK_END);
    if (size >= 0 && (uint64_t)size <= max && sceIoLseek(fd, 0, SCE_SEEK_SET) == 0 &&
        (buf = vjo_arena_alloc(a, (size_t)size + 1)) != NULL) {
        while (used < (size_t)size) {
            int n = sceIoRead(fd, buf + used, (SceSize)((size_t)size - used));
            if (n <= 0)
                break;
            used += (size_t)n;
        }
        if (used == (size_t)size) {
            buf[used] = '\0';
            *out = buf;
            *len = used;
            r = 0;
        }
    }
    if (r)
        *rc = size < 0 ? (int)size : (int)0x80010005; /* EIO: too large, short, or no memory */
    sceIoClose(fd);
    return r;
}

static int exists(const char *path)
{
    SceIoStat st;
    int rc = sceIoGetstat(path, &st);
    return rc >= 0 ? 1 : rc == (int)0x80010002 ? 0 : rc;
}

int vjo_afv_speaks_1(VjoArena *a)
{
    size_t mark = vjo_arena_mark(a), len = 0;
    char *text = NULL;
    VjoJson j;
    int rc, ok = 0, arr;
    if (read_file(a, VJO_AFV_INTERFACE_PATH, JSON_MAX, &text, &len, &rc) == 0 && vjo_json_parse(a, text, len, &j) == 0 &&
        j.n > 0 && vjo_json_is_type(&j, 0, JSMN_OBJECT) && (arr = vjo_json_obj_get(&j, 0, "interfaces")) >= 0 &&
        vjo_json_is_type(&j, arr, JSMN_ARRAY)) {
        for (int k = 0, e; !ok && (e = vjo_json_arr_get(&j, arr, k)) >= 0; k++) /* the number 1, as written */
            ok = vjo_json_is_type(&j, e, JSMN_PRIMITIVE) && j.t[e].end - j.t[e].start == 1 && j.js[j.t[e].start] == '1';
    }
    vjo_arena_release(a, mark);
    return ok;
}

static int failed(char *msg, size_t cap, const char *what, int rc)
{
    snprintf(msg, cap, "Anki for Vita: duplicate index request: %s failed 0x%08X", what, (unsigned)rc);
    return VJO_ANKI_REQUEST_FAILED;
}

int vjo_anki_index_request(VjoArena *a, char *msg, size_t cap)
{
    size_t mark = vjo_arena_mark(a), len = 0, have_len = 0;
    VjoAnkiRequestConfig c;
    char *text = NULL, *have = NULL, *want;
    VjoBuf b;
    SceUID fd;
    int rc, r, moved = 0;
    msg[0] = '\0';
    r = read_file(a, VJO_PREFERENCES_PATH, VJO_PREFERENCES_TEXT_MAX, &text, &len, &rc);
    vjo_anki_request_config(&c, r == 0 ? text : "", r == 0 ? len : 0); /* no config.ini: the defaults */
    vjo_arena_release(a, mark);
    if (!c.on) {
        snprintf(msg, cap, "Anki for Vita: duplicate check off");
        return VJO_ANKI_REQUEST_OFF;
    }
    if (!vjo_afv_speaks_1(a)) {
        snprintf(msg, cap, "Anki for Vita: not installed (or no interface version 1)");
        return VJO_ANKI_REQUEST_NO_APP;
    }
    /* what Rubi asks for */
    vjo_buf_init(&b, a);
    vjo_buf_puts(&b, "{\"v\": 1, \"notetypes\": [], \"key\": ");
    vjo_json_write_string(&b, c.field);
    vjo_buf_puts(&b, ", \"reading\": ");
    vjo_json_write_string(&b, c.reading);
    vjo_buf_puts(&b, "}\n");
    if (!(want = vjo_buf_cstr(&b))) {
        vjo_arena_release(a, mark);
        return failed(msg, cap, "memory", 0);
    }
    /* an earlier write stopped between its renames: undone (only the .old) or finished (both) */
    if ((r = exists(OLD)) < 0 || (r && (rc = exists(VJO_ANKI_REQUEST_PATH)) < 0)) {
        vjo_arena_release(a, mark);
        return failed(msg, cap, "getstat", r < 0 ? r : rc);
    }
    if (r && (rc = rc ? sceIoRemove(OLD) : sceIoRename(OLD, VJO_ANKI_REQUEST_PATH)) < 0) {
        vjo_arena_release(a, mark);
        return failed(msg, cap, "a leftover's rename or removal", rc);
    }
    r = read_file(a, VJO_ANKI_REQUEST_PATH, JSON_MAX, &have, &have_len, &rc);
    if (r < 0) {
        vjo_arena_release(a, mark);
        return failed(msg, cap, "read", rc);
    }
    if (r == 0 && have_len == b.len && !memcmp(have, want, b.len)) {
        vjo_arena_release(a, mark);
        snprintf(msg, cap, "Anki for Vita: duplicate index requested already");
        return VJO_ANKI_REQUEST_SAME;
    }
    sceIoMkdir(VJO_AFV_INDEX_DIR, 0777); /* fails when it is there */
    if ((fd = sceIoOpen(TMP, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666)) < 0) {
        vjo_arena_release(a, mark);
        return failed(msg, cap, "open", fd);
    }
    {
        size_t off = 0;
        rc = 0;
        while (!rc && off < b.len) {
            int n = sceIoWrite(fd, want + off, (SceSize)(b.len - off));
            if (n <= 0)
                rc = n ? n : (int)0x80010005;
            else
                off += (size_t)n;
        }
        if (!rc && (r = sceIoSyncByFd(fd, 0)) < 0)
            rc = r;
        if ((r = sceIoClose(fd)) < 0 && !rc)
            rc = r;
    }
    vjo_arena_release(a, mark);
    if (rc) {
        sceIoRemove(TMP);
        return failed(msg, cap, "write", rc);
    }
    /* the old request aside, the new one in its place */
    if ((r = exists(VJO_ANKI_REQUEST_PATH)) != 0) {
        if (r < 0 || (r = sceIoRename(VJO_ANKI_REQUEST_PATH, OLD)) < 0) {
            sceIoRemove(TMP);
            return failed(msg, cap, "rename", r);
        }
        moved = 1;
    }
    if ((r = sceIoRename(TMP, VJO_ANKI_REQUEST_PATH)) < 0) {
        if (moved)
            sceIoRename(OLD, VJO_ANKI_REQUEST_PATH); /* back (else the next start undoes it) */
        sceIoRemove(TMP);
        sceIoSync("ux0:", 0);
        return failed(msg, cap, "rename", r);
    }
    if ((r = sceIoSync("ux0:", 0)) < 0)
        return failed(msg, cap, "sync", r);
    if (moved && ((r = sceIoRemove(OLD)) < 0 || (r = sceIoSync("ux0:", 0)) < 0))
        return failed(msg, cap, "the old request's removal", r); /* the new one is in place */
    snprintf(msg, cap, "Anki for Vita: duplicate index requested (%s, %s)", c.field, c.reading[0] ? c.reading : "no reading");
    return VJO_ANKI_REQUEST_WRITTEN;
}
