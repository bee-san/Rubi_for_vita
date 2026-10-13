/* Boot-safety guard core: see boot_guard.h. No libc: the kernel plugin links
 * with -nostdlib and the host tests want the exact same bytes. */
#include "boot_guard.h"

static void zero(VjoBootRecord *r)
{
    unsigned char *p = (unsigned char *)r;
    for (unsigned i = 0; i < sizeof(*r); i++)
        p[i] = 0;
}

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

int vjo_boot_complete(const VjoBootRecord *r)
{
    return r->fault == VJO_BOOT_FAULT_NONE && (r->stage >= VJO_BOOT_STAGE_OK || r->inert);
}

void vjo_boot_decide(const VjoBootRecord *prev, int api, VjoBootRecord *next)
{
    zero(next);
    next->api = api;
    next->stage = VJO_BOOT_STAGE_KERNEL;
    if (!prev) {
        next->seq = 1;
        next->mode = VJO_BOOT_NORMAL;
        return;
    }
    next->seq = prev->seq >= VJO_BOOT_MAX_SEQ ? 1 : prev->seq + 1;
    next->prev_mode = prev->mode;
    next->prev_stage = prev->stage;
    next->prev_inert = prev->inert;
    next->prev_fault = prev->fault;
    if (prev->mode == VJO_BOOT_NORMAL) {
        if (vjo_boot_complete(prev)) {
            next->failures = 0;
            next->mode = VJO_BOOT_NORMAL;
        } else {
            next->failures = clamp(prev->failures + 1, 1, VJO_BOOT_MAX_FAILURES);
            next->mode = VJO_BOOT_SAFE;
        }
    } else {
        /* Our plugins were inert: the boot says nothing about them. One
         * more normal attempt follows a complete safe boot, unless normal
         * boots already failed VJO_BOOT_STICKY_FAILURES times in a row. */
        next->failures = clamp(prev->failures, 0, VJO_BOOT_MAX_FAILURES);
        next->mode = vjo_boot_complete(prev) && next->failures < VJO_BOOT_STICKY_FAILURES
                         ? VJO_BOOT_NORMAL
                         : VJO_BOOT_SAFE;
    }
}

int vjo_boot_mark(VjoBootRecord *r, int stage)
{
    if (stage <= r->stage || stage > VJO_BOOT_STAGE_OK)
        return 0;
    r->stage = stage;
    return 1;
}

/* ---- text ---- */

static int put_str(char *out, int cap, int n, const char *s)
{
    while (*s) {
        if (n >= cap)
            return -1;
        out[n++] = *s++;
    }
    return n;
}

static int put_uint(char *out, int cap, int n, uint32_t v)
{
    char digits[12];
    int d = 0;
    do {
        digits[d++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (d) {
        if (n >= cap)
            return -1;
        out[n++] = digits[--d];
    }
    return n;
}

static int put_field(char *out, int cap, int n, const char *key, uint32_t v)
{
    if (n < 0 || (n = put_str(out, cap, n, key)) < 0)
        return -1;
    return put_uint(out, cap, n, v);
}

int vjo_boot_format(const VjoBootRecord *r, char *out, int cap)
{
    int n = put_str(out, cap, 0, "vjoboot1");
    n = put_field(out, cap, n, " seq=", r->seq);
    n = put_field(out, cap, n, " mode=", (uint32_t)clamp(r->mode, 0, 1));
    n = put_field(out, cap, n, " stage=", (uint32_t)clamp(r->stage, 0, VJO_BOOT_STAGE_OK));
    n = put_field(out, cap, n, " inert=", (uint32_t)clamp(r->inert, 0, 1));
    n = put_field(out, cap, n, " fault=", (uint32_t)clamp(r->fault, 0, 9));
    n = put_field(out, cap, n, " failures=", (uint32_t)clamp(r->failures, 0, VJO_BOOT_MAX_FAILURES));
    n = put_field(out, cap, n, " api=", (uint32_t)clamp(r->api, 0, 9999));
    n = put_field(out, cap, n, " prev=", (uint32_t)clamp(r->prev_mode, 0, 1));
    n = put_field(out, cap, n, "/", (uint32_t)clamp(r->prev_stage, 0, VJO_BOOT_STAGE_OK));
    n = put_field(out, cap, n, "/", (uint32_t)clamp(r->prev_inert, 0, 1));
    n = put_field(out, cap, n, "/", (uint32_t)clamp(r->prev_fault, 0, 9));
    if (n < 0 || n + 2 > cap)
        return -1;
    out[n++] = '\n';
    out[n] = 0;
    return n;
}

static int key_is(const char *text, int len, int at, const char *key, int *value_at)
{
    int i = 0;
    while (key[i]) {
        if (at + i >= len || text[at + i] != key[i])
            return 0;
        i++;
    }
    *value_at = at + i;
    return 1;
}

/* Digits at text[at..]; stops at the first non-digit. Returns the count of
 * digits consumed (0: none). Values beyond 9 digits saturate. */
static int read_uint(const char *text, int len, int at, uint32_t *v)
{
    int digits = 0;
    *v = 0;
    while (at + digits < len && text[at + digits] >= '0' && text[at + digits] <= '9') {
        if (digits < 9)
            *v = *v * 10 + (uint32_t)(text[at + digits] - '0');
        else
            *v = 999999999u;
        digits++;
    }
    return digits;
}

int vjo_boot_parse(const char *text, int len, VjoBootRecord *out)
{
    static const char magic[] = "vjoboot1";
    int at = 0, have_seq = 0, have_mode = 0;
    uint32_t v, a, b, c, d;
    zero(out);
    if (!text || len < (int)sizeof(magic) - 1)
        return -1;
    for (; at < (int)sizeof(magic) - 1; at++)
        if (text[at] != magic[at])
            return -1;
    while (at < len) {
        int va, n;
        if (text[at] == '\n' || text[at] == '\r')
            break; /* only the first line is the record */
        if (text[at] == ' ') {
            at++;
            continue;
        }
        if (key_is(text, len, at, "seq=", &va) && (n = read_uint(text, len, va, &v)) > 0) {
            out->seq = v > VJO_BOOT_MAX_SEQ ? VJO_BOOT_MAX_SEQ : v;
            have_seq = v > 0;
            at = va + n;
        } else if (key_is(text, len, at, "mode=", &va) && (n = read_uint(text, len, va, &v)) > 0) {
            out->mode = clamp((int)v, 0, 1);
            have_mode = 1;
            at = va + n;
        } else if (key_is(text, len, at, "stage=", &va) && (n = read_uint(text, len, va, &v)) > 0) {
            out->stage = clamp((int)v, 0, VJO_BOOT_STAGE_OK);
            at = va + n;
        } else if (key_is(text, len, at, "inert=", &va) && (n = read_uint(text, len, va, &v)) > 0) {
            out->inert = clamp((int)v, 0, 1);
            at = va + n;
        } else if (key_is(text, len, at, "fault=", &va) && (n = read_uint(text, len, va, &v)) > 0) {
            out->fault = clamp((int)v, 0, 9);
            at = va + n;
        } else if (key_is(text, len, at, "failures=", &va) && (n = read_uint(text, len, va, &v)) > 0) {
            out->failures = clamp((int)v, 0, VJO_BOOT_MAX_FAILURES);
            at = va + n;
        } else if (key_is(text, len, at, "api=", &va) && (n = read_uint(text, len, va, &v)) > 0) {
            out->api = clamp((int)v, 0, 9999);
            at = va + n;
        } else if (key_is(text, len, at, "prev=", &va) && (n = read_uint(text, len, va, &a)) > 0) {
            int p = va + n;
            out->prev_mode = clamp((int)a, 0, 1);
            if (p < len && text[p] == '/' && (n = read_uint(text, len, p + 1, &b)) > 0) {
                out->prev_stage = clamp((int)b, 0, VJO_BOOT_STAGE_OK);
                p += 1 + n;
                if (p < len && text[p] == '/' && (n = read_uint(text, len, p + 1, &c)) > 0) {
                    out->prev_inert = clamp((int)c, 0, 1);
                    p += 1 + n;
                    if (p < len && text[p] == '/' && (n = read_uint(text, len, p + 1, &d)) > 0) {
                        out->prev_fault = clamp((int)d, 0, 9);
                        p += 1 + n;
                    }
                }
            }
            at = p;
        } else {
            /* an unknown or damaged token: skip it */
            while (at < len && text[at] != ' ' && text[at] != '\n' && text[at] != '\r')
                at++;
        }
    }
    if (!have_seq || !have_mode) {
        zero(out);
        return -1;
    }
    return 0;
}

const char *vjo_boot_mode_name(int mode)
{
    return mode == VJO_BOOT_SAFE ? "safe" : "normal";
}

const char *vjo_boot_stage_name(int stage)
{
    switch (stage) {
    case VJO_BOOT_STAGE_KERNEL: return "kernel";
    case VJO_BOOT_STAGE_SHELL:  return "shell";
    case VJO_BOOT_STAGE_WORKER: return "worker";
    case VJO_BOOT_STAGE_OK:     return "ok";
    default:                    return "none";
    }
}
