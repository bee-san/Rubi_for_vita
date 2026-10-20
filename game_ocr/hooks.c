/* The game worker's native text hooks (game_ocr/hooks.h, docs/native-hooks.md).
 *
 * Only in a game's process: the worker's module_start calls vjo_hooks_start after its safe-boot,
 * candidate and another-copy checks, and module_stop calls vjo_hooks_stop. Two sources:
 *   - the CALL source: taiHookFunctionImport on the main module's (eboot's) string copies
 *     (SceLibc memcpy, strcpy, strncpy, memmove; SceLibKernel's sceClibMemcpy, sceClibStrncpy,
 *     sceClibStrlcpy), as the removed text/main.c had them (c35ebf9^, the source a console run
 *     showed working in CLANNAD: vita-vn-ocr note 23). Each hook calls the game's function
 *     first (TAI_CONTINUE), then offers the copied bytes to its caller's stream;
 *   - table sites (Rubi's verified entries and Luna's table): taiHookFunctionOffset into segment 0
 *     with the register observer (game_ocr/hooks_observer.S), only where core/hook_place.c accepts
 *     the site.
 * Hook time (a game thread) only copies bounded raw bytes into a static slot under its try-lock:
 * no allocation, no lock wait, and no syscall in the CALL path. Decoding happens at job time. */
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <taihen.h>

#include "hooks.h"
#include "../core/hook_place.h"
#include "../core/hook_stream.h"
#include "../include/vjo_api.h"
#include "../include/vjo_import.h"

_Static_assert(VJO_STREAM_RAW == VJO_HOOK_RAW_BYTES, "stream slots hold a stream's raw text");

/* The CALL source's imports: library NID, function NID (the old text/main.c's, and VitaSDK's db). */
enum { CALL_MEMCPY, CALL_STRCPY, CALL_STRNCPY, CALL_MEMMOVE, CALL_CLIB_MEMCPY, CALL_CLIB_STRNCPY,
       CALL_CLIB_STRLCPY, CALL_IMPORTS };
#define NID_SCE_LIBC 0xBE43BB07u
#define NID_SCE_CLIB 0xCAE9ACE6u

static VjoStreamSlot slots[VJO_HOOK_STREAMS]; /* 0..7 sites, 8..19 CALL callers */
static VjoStreamClock stream_clock;
static tai_hook_ref_t call_ref[CALL_IMPORTS];
static SceUID call_uid[CALL_IMPORTS];
static volatile uint32_t live;      /* hits are taken only while 1 */
static volatile uint32_t in_flight; /* game threads inside a hook of ours */
static uint32_t applied_mode, n_sites, n_calls, n_refused;
static uint32_t seg_base;           /* the main module's segment 0 */

static inline void enter(void) { __atomic_add_fetch(&in_flight, 1u, __ATOMIC_ACQUIRE); }
static inline void leave(void) { __atomic_sub_fetch(&in_flight, 1u, __ATOMIC_RELEASE); }

/* ---- the CALL source (hook time) ---- */

/* The bytes before the first NUL among the first lim (no libc in the hook path). */
static unsigned bounded_len(const uint8_t *p, unsigned lim)
{
    unsigned n = 0;
    while (n < lim && p[n]) n++;
    return n;
}

/* written: the bytes the game's copy has just written at dst (memory that is valid now).
 * mem: a memcpy/memmove (its length is exact: at most a stream's raw bytes), else a string copy
 * (whose text must end, NUL, inside a stream's raw bytes). */
static void call_hit(const uint8_t *dst, unsigned written, int mem, uint32_t caller)
{
    unsigned lim, n;
    VjoStreamSlot *s;
    if (!live || !dst || written < 4) return;
    if (mem && written > VJO_HOOK_RAW_BYTES) return;
    lim = written < VJO_HOOK_RAW_BYTES ? written : VJO_HOOK_RAW_BYTES;
    if (!vjo_place_text_prefilter(dst, lim)) return; /* reads at most 16 bytes */
    n = bounded_len(dst, lim);
    if (n < 4 || (!mem && n == VJO_HOOK_RAW_BYTES)) return;
    s = vjo_stream_caller(&slots[VJO_HOOK_SITE_STREAMS], VJO_HOOK_CALL_STREAMS, caller);
    if (!s) {
        __atomic_add_fetch(&stream_clock.dropped, 1u, __ATOMIC_RELAXED);
        return;
    }
    vjo_stream_offer(s, &stream_clock, dst, n, VJO_HOOK_ENC_AUTO);
}

#define CALLER() ((uint32_t)(uintptr_t)__builtin_return_address(0))

static void *libc_memcpy_hook(void *dst, const void *src, size_t n)
{
    enter();
    void *r = TAI_CONTINUE(void *, call_ref[CALL_MEMCPY], dst, src, n);
    call_hit(dst, n > 0xFFFFu ? 0xFFFFu : (unsigned)n, 1, CALLER());
    leave();
    return r;
}

static char *libc_strcpy_hook(char *dst, const char *src)
{
    enter();
    char *r = TAI_CONTINUE(char *, call_ref[CALL_STRCPY], dst, src);
    /* the length is not known: the text must end inside a stream's bytes (bounded_len) */
    call_hit((const uint8_t *)dst, VJO_HOOK_RAW_BYTES, 0, CALLER());
    leave();
    return r;
}

static char *libc_strncpy_hook(char *dst, const char *src, size_t n)
{
    enter();
    char *r = TAI_CONTINUE(char *, call_ref[CALL_STRNCPY], dst, src, n);
    call_hit((const uint8_t *)dst, n > 0xFFFFu ? 0xFFFFu : (unsigned)n, 0, CALLER());
    leave();
    return r;
}

static void *libc_memmove_hook(void *dst, const void *src, size_t n)
{
    enter();
    void *r = TAI_CONTINUE(void *, call_ref[CALL_MEMMOVE], dst, src, n);
    call_hit(dst, n > 0xFFFFu ? 0xFFFFu : (unsigned)n, 1, CALLER());
    leave();
    return r;
}

static void *clib_memcpy_hook(void *dst, const void *src, SceSize n)
{
    enter();
    void *r = TAI_CONTINUE(void *, call_ref[CALL_CLIB_MEMCPY], dst, src, n);
    call_hit(dst, n > 0xFFFFu ? 0xFFFFu : (unsigned)n, 1, CALLER());
    leave();
    return r;
}

static char *clib_strncpy_hook(char *dst, const char *src, SceSize n)
{
    enter();
    char *r = TAI_CONTINUE(char *, call_ref[CALL_CLIB_STRNCPY], dst, src, n);
    call_hit((const uint8_t *)dst, n > 0xFFFFu ? 0xFFFFu : (unsigned)n, 0, CALLER());
    leave();
    return r;
}

static SceSize clib_strlcpy_hook(char *dst, const char *src, SceSize cap)
{
    enter();
    SceSize r = TAI_CONTINUE(SceSize, call_ref[CALL_CLIB_STRLCPY], dst, src, cap);
    /* strlcpy writes min(strlen(src), cap - 1) bytes and a NUL */
    if (cap) call_hit((const uint8_t *)dst, cap > 0xFFFFu ? 0xFFFFu : (unsigned)cap, 0, CALLER());
    leave();
    return r;
}

static const struct {
    uint32_t lib, nid;
    const void *func;
} call_imports[CALL_IMPORTS] = {
    [CALL_MEMCPY] = {NID_SCE_LIBC, 0x7205BFDBu, (const void *)libc_memcpy_hook},
    [CALL_STRCPY] = {NID_SCE_LIBC, 0x85B924B7u, (const void *)libc_strcpy_hook},
    [CALL_STRNCPY] = {NID_SCE_LIBC, 0x9F87712Du, (const void *)libc_strncpy_hook},
    [CALL_MEMMOVE] = {NID_SCE_LIBC, 0xAF5C218Du, (const void *)libc_memmove_hook},
    [CALL_CLIB_MEMCPY] = {NID_SCE_CLIB, 0x14E9DBD7u, (const void *)clib_memcpy_hook},
    [CALL_CLIB_STRNCPY] = {NID_SCE_CLIB, 0xC458D60Au, (const void *)clib_strncpy_hook},
    [CALL_CLIB_STRLCPY] = {NID_SCE_CLIB, 0x2CDFCD1Cu, (const void *)clib_strlcpy_hook},
};

/* ---- start and stop (module_start / module_stop) ---- */

/* The main module (the game's eboot.bin): taiHEN's TAI_MAIN_MODULE, checked by its path. */
static int main_module(tai_module_info_t *tai, SceKernelModuleInfo *mi)
{
    unsigned len;
    if (!VJO_IMPORT_READY(taiGetModuleInfo) || !VJO_IMPORT_READY(sceKernelGetModuleInfo)) return -1;
    sceClibMemset(tai, 0, sizeof(*tai));
    tai->size = sizeof(*tai);
    if (taiGetModuleInfo(TAI_MAIN_MODULE, tai) < 0) return -1;
    sceClibMemset(mi, 0, sizeof(*mi));
    mi->size = sizeof(*mi);
    if (sceKernelGetModuleInfo(tai->modid, mi) < 0) return -1;
    len = (unsigned)sceClibStrnlen(mi->path, sizeof(mi->path));
    if (len < 10 || len == sizeof(mi->path) || sceClibStrcmp(mi->path + len - 9, "eboot.bin") ||
        (mi->path[len - 10] != '/' && mi->path[len - 10] != ':'))
        return -1;
    if (!mi->segments[0].vaddr || !mi->segments[0].memsz) return -1;
    return 0;
}

static void install_calls(const char *module_name)
{
    if (!VJO_IMPORT_READY(taiHookFunctionImport)) return;
    for (unsigned i = 0; i < CALL_IMPORTS; i++) {
        /* a game that does not import the function: skipped, not an error */
        call_uid[i] = taiHookFunctionImport(&call_ref[i], module_name, call_imports[i].lib, call_imports[i].nid,
                                            call_imports[i].func);
        if (call_uid[i] >= 0) n_calls++;
    }
}

/* 1 when title_id has a hook entry: Rubi's verified list or Luna's table (auto's titles). */
static int title_listed(const char *title_id)
{
    return vjo_place_title_verified(title_id) || vjo_luna_title_known(title_id);
}

void vjo_hooks_start(const char *title_id, unsigned mode, int psp)
{
    tai_module_info_t tai;
    SceKernelModuleInfo mi;
    for (unsigned i = 0; i < CALL_IMPORTS; i++) call_uid[i] = -1;
    for (unsigned i = 0; i < VJO_HOOK_STREAMS; i++)
        slots[i].kind = i < VJO_HOOK_SITE_STREAMS ? VJO_HOOK_KIND_SITE : VJO_HOOK_KIND_CALL;
    /* the mode the kernel gave (an unknown value is OCR), even when nothing applies to this game */
    applied_mode = mode == VJO_TEXT_SOURCE_AUTO || mode == VJO_TEXT_SOURCE_HOOKS ? mode : VJO_TEXT_SOURCE_OCR;
    if (applied_mode == VJO_TEXT_SOURCE_OCR) return;
    if (psp || !title_id || !title_id[0]) return; /* nothing in the PSP emulator in this version */
    if (mode == VJO_TEXT_SOURCE_AUTO && !title_listed(title_id)) return;
    if (main_module(&tai, &mi) < 0) return;
    seg_base = (uint32_t)(uintptr_t)mi.segments[0].vaddr;
    live = 1;
    install_calls(tai.name);
}

int vjo_hooks_stop(void)
{
    live = 0;
    if (VJO_IMPORT_READY(taiHookRelease))
        for (unsigned i = CALL_IMPORTS; i-- > 0;)
            if (call_uid[i] >= 0) {
                taiHookRelease(call_uid[i], call_ref[i]);
                call_uid[i] = -1;
            }
    /* A thread that had entered a hook before its release may still be on its way out. */
    if (__atomic_load_n(&in_flight, __ATOMIC_ACQUIRE)) sceKernelDelayThread(2000);
    return __atomic_load_n(&in_flight, __ATOMIC_ACQUIRE) ? 1 : 0;
}

void vjo_hooks_counts(uint32_t *mode, uint32_t *installed, uint32_t *refused)
{
    if (mode) *mode = applied_mode;
    if (installed) *installed = n_sites << 16 | n_calls;
    if (refused) *refused = n_refused;
}

/* ---- job time ---- */

int vjo_hooks_snap(uint32_t id, VjoHookSnap *out)
{
    VjoStreamSlot *s;
    if (!out || !id || id > VJO_HOOK_STREAMS) return 0;
    s = &slots[id - 1];
    if (!vjo_stream_try_lock(s)) return 0;
    if (!s->seq || !s->n || s->n > VJO_HOOK_RAW_BYTES) {
        vjo_stream_unlock(s);
        return 0;
    }
    out->id = id;
    out->kind = s->kind;
    out->seq = s->seq;
    out->hits = s->hits;
    /* a site's slot key is its segment-0 offset + 1; a caller's is its return address */
    out->key = s->kind == VJO_HOOK_KIND_SITE ? s->key - 1u : s->key - seg_base;
    out->encoding = s->encoding;
    out->n = s->n;
    out->site = (const VjoLunaSite *)s->site;
    sceClibMemcpy(out->raw, s->raw, s->n);
    vjo_stream_unlock(s);
    return 1;
}

size_t vjo_hooks_text(const VjoHookSnap *s, char *out, size_t cap)
{
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!s || !s->n || s->n > VJO_HOOK_RAW_BYTES) return 0;
    if (s->site) return vjo_luna_text(s->site, s->raw, s->n, s->encoding, out, cap);
    return vjo_hook_decode(s->encoding, s->raw, s->n, out, cap, NULL);
}
