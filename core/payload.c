#include "payload.h"
#include "taicfg.h"
#include <bearssl_hash.h>
#include <stdio.h>
#include <string.h>

#define PAYLOAD_DEST(dest, kind) dest,
#define PAYLOAD_KIND(dest, kind) kind,
static const char *const dests[VJO_PAYLOAD_COUNT] = {VJO_PAYLOAD_FILE_LIST(PAYLOAD_DEST)};
static const int kinds[VJO_PAYLOAD_COUNT] = {VJO_PAYLOAD_FILE_LIST(PAYLOAD_KIND)};
static const char *const set_keys[] = VJO_PAYLOAD_SET_KEYS;

const char *vjo_payload_dest(int index) { return index >= 0 && index < VJO_PAYLOAD_COUNT ? dests[index] : NULL; }
int vjo_payload_kind(int index) { return index >= 0 && index < VJO_PAYLOAD_COUNT ? kinds[index] : -1; }

int vjo_payload_source(int index, char *out, size_t cap)
{
    const char *d = vjo_payload_dest(index);
    if (!d || strlen(d) < 5 || d[3] != ':') return -1;
    /* "ux0:data/..." -> VJO_PAYLOAD_DIR "/ux0/data/..." */
    int n = snprintf(out, cap, VJO_PAYLOAD_DIR "/%.3s/%s", d, d + 4);
    return n < 0 || (size_t)n >= cap ? -1 : 0;
}

void vjo_sha256_hex(const uint8_t sha[32], char out[65])
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i] = digits[sha[i] >> 4];
        out[2 * i + 1] = digits[sha[i] & 15];
    }
    out[64] = '\0';
}

static int hex_digit(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

/* 64 lowercase hex digits (exactly n bytes of s) into sha: 0, or -1. */
static int parse_sha(const char *s, size_t n, uint8_t sha[32])
{
    if (n != 64) return -1;
    for (int i = 0; i < 32; i++) {
        int hi = hex_digit(s[2 * i]), lo = hex_digit(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        sha[i] = (uint8_t)(hi << 4 | lo);
    }
    return 0;
}

/* A decimal of at most 13 digits (under 10 TB), no sign, no leading zero but "0": 0, or -1. */
static int parse_u64(const char *s, size_t n, uint64_t *v)
{
    if (!n || n > 13 || (n > 1 && s[0] == '0')) return -1;
    *v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        *v = *v * 10 + (uint64_t)(s[i] - '0');
    }
    return 0;
}

/* The word of line[0, n) starting at *pos (up to a space or the end): its start and length. */
static size_t word(const char *line, size_t n, size_t *pos, const char **start)
{
    size_t b = *pos, e = b;
    while (e < n && line[e] != ' ') e++;
    *start = line + b;
    *pos = e < n ? e + 1 : e;
    return e - b;
}

static int is_word(const char *s, size_t n, const char *w) { return strlen(w) == n && !memcmp(s, w, n); }

/* A version: 1-39 bytes of [0-9A-Za-z.+~-], starting with a digit. */
static int version_ok(const char *s, size_t n)
{
    if (!n || n >= 40 || s[0] < '0' || s[0] > '9') return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' || c == '+' ||
              c == '~' || c == '-'))
            return 0;
    }
    return 1;
}

static int refuse(char *why, size_t cap, int line, const char *what)
{
    if (why && cap) {
        if (line) snprintf(why, cap, "the package is damaged (payload/manifest.txt line %d: %s)", line, what);
        else snprintf(why, cap, "the package is damaged (payload/manifest.txt: %s)", what);
    }
    return -1;
}

int vjo_payload_parse(const char *text, size_t len, const VjoPayloadBuild *own, VjoPayload *out, char *why,
                      size_t cap)
{
    enum { K_VERSION = 1, K_APP_VERSION = 2, K_VARIANT = 4, K_API = 8, K_WORKER = 16, K_BUILD = 32, K_ALL = 63 };
    unsigned seen_keys = 0;
    unsigned char seen_file[VJO_PAYLOAD_COUNT];
    char variant_text[8] = "", app_version[40] = "";
    int lines = 0;
    if (!text || !out || !own) return refuse(why, cap, 0, "missing");
    memset(out, 0, sizeof(*out));
    memset(seen_file, 0, sizeof(seen_file));
    if (len > VJO_PAYLOAD_MANIFEST_MAX) return refuse(why, cap, 0, "too large");
    if (!len || text[len - 1] != '\n') return refuse(why, cap, 0, "it does not end with a line end");
    for (size_t pos = 0; pos < len;) {
        const char *nl = memchr(text + pos, '\n', len - pos);
        size_t n = (size_t)(nl - (text + pos));
        const char *line = text + pos;
        pos += n + 1;
        if (++lines > VJO_PAYLOAD_LINES_MAX) return refuse(why, cap, lines, "too many lines");
        if (n > VJO_PAYLOAD_LINE_MAX) return refuse(why, cap, lines, "a line is too long");
        for (size_t i = 0; i < n; i++)
            if ((unsigned char)line[i] < 0x20 || (unsigned char)line[i] > 0x7e)
                return refuse(why, cap, lines, "not printable ASCII");
        if (lines == 1) {
            if (!is_word(line, n, "vjopayload 1")) return refuse(why, cap, 1, "not a payload manifest");
            continue;
        }
        size_t at = 0;
        const char *k, *a = NULL, *b = NULL, *c = NULL, *d = NULL;
        size_t kn = word(line, n, &at, &k);
        if (is_word(k, kn, "file") || is_word(k, kn, "template")) {
            int template_line = is_word(k, kn, "template");
            size_t an = word(line, n, &at, &a), bn = word(line, n, &at, &b), cn = word(line, n, &at, &c);
            size_t dn = template_line ? 0 : word(line, n, &at, &d);
            if (at != n || !an || !bn || !cn || (!template_line && !dn)) return refuse(why, cap, lines, "a file line's fields");
            int index = -1;
            for (int i = 0; i < VJO_PAYLOAD_COUNT; i++)
                if (is_word(a, an, dests[i])) index = i;
            if (index < 0) return refuse(why, cap, lines, "a file this app does not install");
            if (seen_file[index]) return refuse(why, cap, lines, "a file named twice");
            int kind = template_line ? VJO_PAYLOAD_TEMPLATE : is_word(d, dn, "copy") ? VJO_PAYLOAD_COPY
                     : is_word(d, dn, "move") ? VJO_PAYLOAD_MOVE : -1;
            if (kind != kinds[index]) return refuse(why, cap, lines, "a file's kind");
            VjoPayloadFile *f = &out->file[index];
            if (parse_u64(b, bn, &f->bytes) < 0) return refuse(why, cap, lines, "a size");
            if (parse_sha(c, cn, f->sha256) < 0) return refuse(why, cap, lines, "a SHA-256");
            f->dest = dests[index];
            f->kind = kind;
            seen_file[index] = 1;
            continue;
        }
        if (is_word(k, kn, "set")) {
            size_t an = word(line, n, &at, &a), bn = word(line, n, &at, &b);
            int known = 0;
            if (at != n || !an || !bn) return refuse(why, cap, lines, "a set line's fields");
            for (unsigned i = 0; i < sizeof(set_keys) / sizeof(set_keys[0]); i++) known |= is_word(a, an, set_keys[i]);
            if (!known) return refuse(why, cap, lines, "a setting this app does not set");
            if (!is_word(b, bn, "on") && !is_word(b, bn, "off")) return refuse(why, cap, lines, "a setting's value");
            for (int i = 0; i < out->sets; i++)
                if (is_word(a, an, out->set[i].key)) return refuse(why, cap, lines, "a setting named twice");
            if (out->sets >= VJO_PAYLOAD_SETS_MAX) return refuse(why, cap, lines, "too many settings");
            memcpy(out->set[out->sets].key, a, an);
            memcpy(out->set[out->sets].value, b, bn);
            out->sets++;
            continue;
        }
        size_t vn = word(line, n, &at, &a);
        unsigned key = is_word(k, kn, "version") ? K_VERSION : is_word(k, kn, "app_version") ? K_APP_VERSION
                     : is_word(k, kn, "variant") ? K_VARIANT : is_word(k, kn, "api") ? K_API
                     : is_word(k, kn, "worker") ? K_WORKER : is_word(k, kn, "build") ? K_BUILD : 0;
        if (!key) return refuse(why, cap, lines, "an unknown line");
        if (at != n || !vn) return refuse(why, cap, lines, "a line's fields");
        if (seen_keys & key) return refuse(why, cap, lines, "a line named twice");
        seen_keys |= key;
        if (key == K_VERSION || key == K_APP_VERSION) {
            if (!version_ok(a, vn)) return refuse(why, cap, lines, "a version");
            memcpy(key == K_VERSION ? out->version : app_version, a, vn);
        } else if (key == K_VARIANT) {
            if (!is_word(a, vn, "normal") && !is_word(a, vn, "debug")) return refuse(why, cap, lines, "the build");
            memcpy(variant_text, a, vn);
        } else if (key == K_API) {
            uint64_t api;
            if (parse_u64(a, vn, &api) < 0 || api > 9999) return refuse(why, cap, lines, "the API");
            out->api = (int)api;
        } else if (key == K_WORKER) {
            if (is_word(a, vn, "kernel")) out->worker = VJO_TAI_WORKER_KERNEL;
            else if (is_word(a, vn, "lines")) out->worker = VJO_TAI_WORKER_LINES;
            else return refuse(why, cap, lines, "the worker mode");
        } else {
            if (vn != 16) return refuse(why, cap, lines, "the app's build");
            for (size_t i = 0; i < 16; i++)
                if (hex_digit(a[i]) < 0) return refuse(why, cap, lines, "the app's build");
            memcpy(out->build, a, 16);
        }
    }
    if (seen_keys != K_ALL) return refuse(why, cap, 0, "a version, app_version, variant, api, worker or build line is missing");
    for (int i = 0; i < VJO_PAYLOAD_COUNT; i++)
        if (!seen_file[i]) return refuse(why, cap, 0, "a file is missing");
    memcpy(out->app_version, app_version, sizeof(app_version));
    memcpy(out->variant, variant_text, sizeof(variant_text));
    /* exactly the build this app is (R4; the delta review's C2): another build's payload would install
     * plugins this app does not describe, or remove worker lines its kernel needs */
    if (why && cap) why[0] = '\0';
    if (out->api != own->api || strcmp(app_version, own->app_version) || strcmp(variant_text, own->variant) ||
        out->worker != own->worker) {
        if (why && cap)
            snprintf(why, cap, "the package's overlay (v%s %s, API %d, worker %s) is not this app's build (v%s %s, API %d, "
                     "worker %s): install the VPK again", app_version, variant_text, out->api,
                     out->worker == VJO_TAI_WORKER_KERNEL ? "kernel" : "lines", own->app_version, own->variant, own->api,
                     own->worker == VJO_TAI_WORKER_KERNEL ? "kernel" : "lines");
        return -1;
    }
    br_sha256_context h;
    br_sha256_init(&h);
    br_sha256_update(&h, text, len);
    br_sha256_out(&h, out->manifest_sha256);
    return 0;
}

int vjo_installed_format(char *out, size_t cap, const VjoPayload *p)
{
    char hex[65];
    if (!out || !p) return -1;
    vjo_sha256_hex(p->manifest_sha256, hex);
    int n = snprintf(out, cap, "vjoinst1 version=%s variant=%s api=%d build=%s manifest=%s\n", p->version, p->variant,
                     p->api, p->build, hex);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

int vjo_installed_parse(const char *text, size_t len, VjoInstalled *out)
{
    char line[256];
    unsigned seen = 0;
    if (!text || !out || !len) return -1;
    memset(out, 0, sizeof(*out));
    size_t n = 0;
    while (n < len && text[n] != '\n' && text[n] != '\r') n++;
    if (n >= sizeof(line) || n < 9 || memcmp(text, "vjoinst1 ", 9)) return -1;
    memcpy(line, text, n);
    line[n] = '\0';
    for (char *tok = strtok(line + 9, " "); tok; tok = strtok(NULL, " ")) {
        char *eq = strchr(tok, '=');
        if (!eq) return -1;
        *eq = '\0';
        const char *v = eq + 1;
        size_t vn = strlen(v);
        if (!strcmp(tok, "version") && version_ok(v, vn)) { memcpy(out->version, v, vn + 1); seen |= 1; }
        else if (!strcmp(tok, "variant") && (!strcmp(v, "normal") || !strcmp(v, "debug"))) { memcpy(out->variant, v, vn + 1); seen |= 2; }
        else if (!strcmp(tok, "api")) {
            uint64_t api;
            if (parse_u64(v, vn, &api) < 0 || api > 9999) return -1;
            out->api = (int)api;
            seen |= 4;
        } else if (!strcmp(tok, "build") && vn == 16) { memcpy(out->build, v, 17); seen |= 8; }
        else if (!strcmp(tok, "manifest") && parse_sha(v, vn, out->manifest_sha256) == 0) seen |= 16;
        else return -1;
    }
    return seen == 31 ? 0 : -1;
}

/* One identifier of a version's part: its digits as a number when all are digits. */
static int compare_ids(const char *a, size_t an, const char *b, size_t bn)
{
    int ad = an > 0, bd = bn > 0;
    for (size_t i = 0; i < an; i++) ad &= a[i] >= '0' && a[i] <= '9';
    for (size_t i = 0; i < bn; i++) bd &= b[i] >= '0' && b[i] <= '9';
    if (ad && bd) {
        while (an > 1 && *a == '0') a++, an--;
        while (bn > 1 && *b == '0') b++, bn--;
        if (an != bn) return an < bn ? -1 : 1;
        int c = memcmp(a, b, an);
        return c < 0 ? -1 : c > 0;
    }
    if (ad != bd) return ad ? -1 : 1; /* a number is older than a word */
    int c = memcmp(a, b, an < bn ? an : bn);
    if (c) return c < 0 ? -1 : 1;
    return an == bn ? 0 : an < bn ? -1 : 1;
}

/* Compares the dot-separated identifiers of a[0, an) and b[0, bn); missing ones are zero when
 * pad, else fewer is older. */
static int compare_dotted(const char *a, size_t an, const char *b, size_t bn, int pad)
{
    size_t i = 0, j = 0;
    while (i < an || j < bn) {
        size_t ie = i, je = j;
        while (ie < an && a[ie] != '.') ie++;
        while (je < bn && b[je] != '.') je++;
        if (i >= an || j >= bn) {
            if (!pad) return i >= an ? -1 : 1;
            int c = compare_ids(i < an ? a + i : "0", i < an ? ie - i : 1, j < bn ? b + j : "0", j < bn ? je - j : 1);
            if (c) return c;
        } else {
            int c = compare_ids(a + i, ie - i, b + j, je - j);
            if (c) return c;
        }
        i = ie < an ? ie + 1 : an;
        j = je < bn ? je + 1 : bn;
    }
    return 0;
}

int vjo_version_compare(const char *a, const char *b)
{
    size_t an = strcspn(a, "+"), bn = strcspn(b, "+"); /* build metadata is ignored */
    const char *ap = memchr(a, '-', an), *bp = memchr(b, '-', bn);
    size_t am = ap ? (size_t)(ap - a) : an, bm = bp ? (size_t)(bp - b) : bn;
    int c = compare_dotted(a, am, b, bm, 1);
    if (c) return c;
    if (!ap || !bp) return !ap && !bp ? 0 : ap ? -1 : 1; /* a pre-release is older than its release */
    return compare_dotted(ap + 1, an - am - 1, bp + 1, bn - bm - 1, 0);
}
