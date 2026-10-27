/* The game worker's native text hooks (game_ocr/hooks.h, docs/native-hooks.md).
 *
 * Only in a game's process: the worker's module_start calls vjo_hooks_start after its safe-boot,
 * candidate and another-copy checks, and module_stop calls vjo_hooks_stop. Two sources:
 *   - the CALL source: taiHookFunctionImport on the main module's (eboot's) string copies
 *     (SceLibc memcpy, strcpy, strncpy, memmove; SceLibKernel's sceClibMemcpy, sceClibStrncpy,
 *     sceClibStrlcpy, and sceClibStrnlen, a string the game measures), as the removed text/main.c had them (c35ebf9^, the source a console run
 *     showed working in CLANNAD: vita-vn-ocr note 23). Each hook calls the game's function
 *     first (TAI_CONTINUE), then offers the copied bytes to its caller's stream;
 *   - table sites (Rubi's verified entries and Luna's table): taiHookFunctionOffset into segment 0
 *     with the register observer (game_ocr/hooks_observer.S), only where core/hook_place.c accepts
 *     the site.
 * Hook time (a game thread) only copies bounded raw bytes into a static slot under its try-lock:
 * no allocation, no lock wait, and no syscall in the CALL path. Decoding happens at job time.
 * In the PSP emulator's process nothing is hooked; PSP text (core/psp_text.h) is the worker's own read of a
 * PSP game's page buffer, at job time, into a site's slot (vjo_hooks_psp_poll). */
#include <stdarg.h>
#include <bearssl_hash.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <taihen.h>

#include "hooks.h"
#include "../core/hook_place.h"
#include "../core/hook_stream.h"
#include "../core/psp_text.h"
#include "../core/sfo.h"
#include "../include/vjo_api.h"
#include "../include/vjo_import.h"

_Static_assert(VJO_STREAM_RAW == VJO_HOOK_RAW_BYTES, "stream slots hold a stream's raw text");
_Static_assert(VJO_PSP_TEXT_MAX == VJO_HOOK_RAW_BYTES, "a PSP page fits a stream's raw text");

/* The CALL source's imports: library NID, function NID (the old text/main.c's; each checked against
 * vita-headers e66ebe9 db/360/SceLibc.yml and SceLibKernel.yml). */
enum { CALL_MEMCPY, CALL_STRCPY, CALL_STRNCPY, CALL_MEMMOVE, CALL_CLIB_MEMCPY, CALL_CLIB_STRNCPY,
       CALL_CLIB_STRLCPY, CALL_CLIB_STRNLEN, CALL_IMPORTS };
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
static int mem_info_ok;             /* sceKernelGetMemBlockInfoByAddr resolved */

/* The table sites: slot i (stream i + 1) is observer i's. */
typedef struct {
    const VjoLunaSite *luna;     /* a Luna site, or NULL */
    const VjoPlaceVerified *ver; /* Rubi's verified entry, or NULL */
    uint32_t addr;               /* the site in the game (its Thumb bit clear): regs[15] */
} SiteInfo;
static SiteInfo site_info[VJO_HOOK_SITE_STREAMS];
static tai_hook_ref_t site_ref[VJO_HOOK_SITE_STREAMS];
static SceUID site_uid[VJO_HOOK_SITE_STREAMS];
/* A site hit's capture buffer: one hit at a time captures (a hit on another thread meanwhile is
 * dropped and counted), so the game's stack carries no text buffer. */
static uint8_t scratch[VJO_HOOK_RAW_BYTES];
static uint32_t scratch_lock;

/* PSP text (core/psp_text.h): in the PSP emulator's process nothing is hooked; with Every game the worker reads
 * the running PSP game's page buffer itself, at job time (vjo_hooks_psp_poll), into slot 0 (stream 1), a table
 * site's slot that no hook writes there. psp_profile: the profile whose game and code the last poll found (its
 * text in slot 0), else NULL. Only the worker's thread touches these. */
static int psp_text_on;
static const VjoPspProfile *psp_profile;
static uint32_t psp_status; /* the last poll's VJO_PSP_TEXT_* (the debug build's probe logs it) */

extern void vjo_hooks_observer_0(void), vjo_hooks_observer_1(void), vjo_hooks_observer_2(void),
    vjo_hooks_observer_3(void), vjo_hooks_observer_4(void), vjo_hooks_observer_5(void),
    vjo_hooks_observer_6(void), vjo_hooks_observer_7(void);
static void (*const observers[VJO_HOOK_SITE_STREAMS])(void) = {
    vjo_hooks_observer_0, vjo_hooks_observer_1, vjo_hooks_observer_2, vjo_hooks_observer_3,
    vjo_hooks_observer_4, vjo_hooks_observer_5, vjo_hooks_observer_6, vjo_hooks_observer_7};
_Static_assert(VJO_HOOK_SITE_STREAMS == 8, "one observer per site slot (hooks_observer.S)");

#if VJO_DEBUG
/* The debug build's record of what vjo_hooks_start did in this game (sites, reasons, their first
 * bytes; never text): ux0:data/VitaJPOverlay/hooks.log, rewritten at each game start. */
static SceUID dlog = -1;
static void dlog_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void dlog_line(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    int n;
    if (dlog < 0) return;
    va_start(ap, fmt);
    n = sceClibVsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((unsigned)n >= sizeof(line)) n = (int)sizeof(line) - 1;
    sceIoWrite(dlog, line, (SceSize)n);
}
static void dlog_bytes(const uint8_t *code, uint32_t code_size, uint32_t off)
{
    char hex[16 * 2 + 1];
    unsigned k = off < code_size ? (code_size - off < 16u ? code_size - off : 16u) : 0u;
    for (unsigned i = 0; i < k; i++) {
        static const char digits[] = "0123456789ABCDEF";
        hex[2 * i] = digits[code[off + i] >> 4];
        hex[2 * i + 1] = digits[code[off + i] & 15];
    }
    hex[2 * k] = 0;
    dlog_line(" bytes %s\n", hex);
}
#define DLOG(...) dlog_line(__VA_ARGS__)
#else
#define DLOG(...) ((void)0)
#define dlog_bytes(code, size, off) ((void)0)
#endif

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
    if (!live || !dst || written < 4) return;
    if (mem && written > VJO_HOOK_RAW_BYTES) return;
    lim = written < VJO_HOOK_RAW_BYTES ? written : VJO_HOOK_RAW_BYTES;
    if (!vjo_place_text_prefilter(dst, lim)) return; /* reads at most 16 bytes */
    n = bounded_len(dst, lim);
    if (n < 4 || (!mem && n == VJO_HOOK_RAW_BYTES)) return;
    /* a pointer array's shape (audit-hooks F2): the bytes the stream would keep, and a mem copy's whole copy
     * (both valid: the game has just written them) */
    if (vjo_place_pointer_array(dst, n) || (mem && n < lim && vjo_place_pointer_array(dst, lim))) return;
    /* the caller's slot, an unused one, or (every slot held) the least recently written one not kept */
    vjo_stream_offer_caller(&slots[VJO_HOOK_SITE_STREAMS], VJO_HOOK_CALL_STREAMS, &stream_clock, caller, dst, n,
                            VJO_HOOK_ENC_AUTO);
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

static SceSize clib_strnlen_hook(const char *s, SceSize max)
{
    enter();
    SceSize r = TAI_CONTINUE(SceSize, call_ref[CALL_CLIB_STRNLEN], s, max);
    /* the game measured a string: r bytes of it (and its NUL when r < max) are there now */
    call_hit((const uint8_t *)s, r < max ? (r + 1u > 0xFFFFu ? 0xFFFFu : (unsigned)r + 1u)
                                         : (r > 0xFFFFu ? 0xFFFFu : (unsigned)r), 0, CALLER());
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
    [CALL_CLIB_STRNLEN] = {NID_SCE_CLIB, 0xAC595E68u, (const void *)clib_strnlen_hook},
};

/* ---- table sites (hook time) ---- */

/* VjoHookRead: up to max bytes at the game's address addr, never past the end of the memory block
 * that holds it, and only from a readable one. unit 1 or 2: stops after the first NUL unit (copied,
 * counted); 0: exactly max or nothing. One syscall (the block's bounds). */
static unsigned read_game(void *ud, uint32_t addr, void *dst, unsigned max, unsigned unit)
{
    SceKernelMemBlockInfo mb;
    uint32_t base, end;
    const volatile uint8_t *src;
    uint8_t *d = dst;
    unsigned n = 0;
    (void)ud;
    if (!addr || !max || !dst || !mem_info_ok) return 0;
    sceClibMemset(&mb, 0, sizeof(mb));
    mb.size = sizeof(mb);
    if (sceKernelGetMemBlockInfoByAddr((void *)(uintptr_t)addr, &mb) < 0) return 0;
    if (!(mb.access & SCE_KERNEL_MEMORY_ACCESS_R)) return 0;
    base = (uint32_t)(uintptr_t)mb.mappedBase;
    end = base + mb.mappedSize;
    if (addr < base || end <= addr) return 0;
    if (end - addr < max) {
        if (!unit) return 0;
        max = end - addr;
    }
    src = (const volatile uint8_t *)(uintptr_t)addr;
    if (unit == 2) {
        for (max &= ~1u; n < max; n += 2) {
            d[n] = src[n];
            d[n + 1] = src[n + 1];
            if (!d[n] && !d[n + 1]) return n + 2;
        }
        return n;
    }
    for (; n < max; n++) {
        d[n] = src[n];
        if (unit == 1 && !d[n]) return n + 1;
    }
    return n;
}

static void site_hit(unsigned i, const uint32_t *regs)
{
    const SiteInfo *si = &site_info[i];
    unsigned n = 0, enc = VJO_HOOK_ENC_CP932, unit;
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&scratch_lock, &expected, 1u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        __atomic_add_fetch(&stream_clock.dropped, 1u, __ATOMIC_RELAXED);
        return;
    }
    if (si->ver) {
        uint32_t addr = regs[si->ver->reg & 15u] + (uint32_t)si->ver->padding, word;
        uint8_t w[4];
        enc = si->ver->encoding;
        unit = enc == VJO_HOOK_ENC_UTF16LE ? 2u : 1u;
        /* the text's pointer there, or a cell record's count: one exact 4-byte read (none: no text) */
        word = si->ver->read && read_game(NULL, addr + (si->ver->read == VJO_PLACE_READ_CELLS ? VJO_PLACE_CELLS_COUNT
                                                                                            : 0u), w, 4, 0) == 4
                   ? (uint32_t)w[0] | (uint32_t)w[1] << 8 | (uint32_t)w[2] << 16 | (uint32_t)w[3] << 24
                   : 0u;
        if (si->ver->read == VJO_PLACE_READ_CELLS) { /* exactly its cells, as CP932 bytes */
            if (word > sizeof(scratch) / 2u) word = sizeof(scratch) / 2u;
            n = word && read_game(NULL, addr + VJO_PLACE_CELLS_DATA, scratch, word * 2u, 0) == word * 2u
                    ? vjo_place_cells(scratch, word)
                    : 0u;
        } else {
            n = read_game(NULL, si->ver->read ? word : addr, scratch, sizeof(scratch), unit);
            /* the text without its NUL unit, as vjo_luna_capture counts it */
            if (n >= unit && !scratch[n - 1] && (unit == 1 || !scratch[n - 2])) n -= unit;
        }
    } else if (si->luna) {
        n = vjo_luna_capture(si->luna, regs, read_game, NULL, scratch, sizeof(scratch), &enc);
        if (n > sizeof(scratch)) n = 0;
    }
    if (n) vjo_stream_offer(&slots[i], &stream_clock, scratch, n, enc);
    __atomic_store_n(&scratch_lock, 0u, __ATOMIC_RELEASE);
}

/* Called by observer slot (hooks_observer.S) with its frame: r0-r12, SP, LR as they were at the
 * site; frame[15] is free until it returns. Returns where the game continues: taiHEN's next hook,
 * or the moved instructions' trampoline (what TAI_CONTINUE calls), without a C prototype on the
 * game's live registers. */
uintptr_t vjo_hooks_observe(unsigned slot, uint32_t *frame);
uintptr_t vjo_hooks_observe(unsigned slot, uint32_t *frame)
{
    const struct _tai_hook_user *cur, *next;
    uintptr_t cont;
    enter();
    slot &= VJO_HOOK_SITE_STREAMS - 1u;
    /* the continuation first: taiHEN's hook record is valid until the hook is released, and
     * vjo_hooks_stop releases it only after clearing live; a thread counted in in_flight keeps the
     * module (and this code) loaded */
    cur = (const struct _tai_hook_user *)site_ref[slot];
    next = (const struct _tai_hook_user *)cur->next;
    cont = (uintptr_t)(next ? next->func : cur->old);
    if (live && slot < n_sites) {
        frame[15] = site_info[slot].addr;
        site_hit(slot, frame);
    }
    leave();
    return cont;
}

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

int vjo_hooks_build_id(uint8_t out[32])
{
    tai_module_info_t tai;
    SceKernelModuleInfo mi;
    br_sha256_context hash;
    uint8_t chunk[1024];
    /* A damaged/malicious stream must not keep the worker reading forever. Larger executables still
     * use fresh learning; this is a cache limit, not a game or text limit. */
    const uint32_t limit = 256u * 1024u * 1024u;
    uint32_t total = 0;
    SceUID fd;
    int got, ok = 0;
    if (!out) return 0;
    sceClibMemset(out, 0, 32);
    if (!VJO_IMPORT_READY(sceIoOpen) || !VJO_IMPORT_READY(sceIoRead) || !VJO_IMPORT_READY(sceIoClose) ||
        main_module(&tai, &mi) < 0) return 0;
    fd = sceIoOpen(mi.path, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    br_sha256_init(&hash);
    while ((got = sceIoRead(fd, chunk, sizeof(chunk))) > 0) {
        if ((unsigned)got > sizeof(chunk) || (uint32_t)got > limit - total) goto done;
        total += (uint32_t)got;
        br_sha256_update(&hash, chunk, (size_t)got);
    }
    if (!got && total) {
        br_sha256_out(&hash, out);
        ok = 1;
    }
done:
    if (sceIoClose(fd) < 0) ok = 0;
    if (!ok) sceClibMemset(out, 0, 32);
    return ok;
}

static void install_calls(const char *module_name)
{
    if (!VJO_IMPORT_READY(taiHookFunctionImportForUser)) return; /* taiHookFunctionImport is its inline helper */
    for (unsigned i = 0; i < CALL_IMPORTS; i++) {
        /* a game that does not import the function: skipped, not an error */
        call_uid[i] = taiHookFunctionImport(&call_ref[i], module_name, call_imports[i].lib, call_imports[i].nid,
                                            call_imports[i].func);
        if (call_uid[i] >= 0) n_calls++;
        DLOG("import %08X/%08X: %s\n", (unsigned)call_imports[i].lib, (unsigned)call_imports[i].nid,
             call_uid[i] >= 0 ? "hooked" : "not hooked");
    }
}

/* The game's version, APP_VER of its param.sfo ("01.10" -> 0x0110, as Luna's comments are coded;
 * 0 when unreadable). Read once, before any hook exists, into the CALL slots' storage (zeroed again
 * after): no allocation and no buffer of its own. */
static uint16_t app_version(void)
{
    uint8_t *buf = (uint8_t *)&slots[VJO_HOOK_SITE_STREAMS];
    const unsigned cap = sizeof(VjoStreamSlot) * VJO_HOOK_CALL_STREAMS;
    char ver[16];
    unsigned major = 0, minor = 0, i = 0, k;
    int n;
    SceUID fd;
    if (!VJO_IMPORT_READY(sceIoOpen)) return 0;
    fd = sceIoOpen("app0:sce_sys/param.sfo", SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    n = sceIoRead(fd, buf, cap);
    sceIoClose(fd);
    k = n > 0 && (unsigned)n < cap && vjo_sfo_string(buf, (size_t)n, "APP_VER", ver, sizeof(ver)) == 0;
    sceClibMemset(buf, 0, cap);
    if (!k) return 0;
    for (k = 0; k < 2 && ver[i] >= '0' && ver[i] <= '9'; k++) major = major << 4 | (unsigned)(ver[i++] - '0');
    if (!k || ver[i++] != '.') return 0;
    for (k = 0; k < 2 && ver[i] >= '0' && ver[i] <= '9'; k++) minor = minor << 4 | (unsigned)(ver[i++] - '0');
    if (k != 2 || ver[i]) return 0;
    return (uint16_t)(major << 8 | minor);
}

/* Hooks the site at segment-0 offset off (its Thumb bit clear) in the next site slot. */
static int hook_site(SceUID modid, uint32_t off, int thumb, const VjoLunaSite *luna, const VjoPlaceVerified *ver)
{
    unsigned i = n_sites;
    SceUID uid;
    for (unsigned k = 0; k < n_sites; k++)
        if (site_info[k].addr == seg_base + off) return -1; /* already hooked: one stream per address */
    if (i >= VJO_HOOK_SITE_STREAMS) return VJO_PLACE_FULL;
    if (!VJO_IMPORT_READY(taiHookFunctionOffsetForUser)) return VJO_PLACE_TAI;
    site_info[i].luna = luna;
    site_info[i].ver = ver;
    site_info[i].addr = seg_base + off;
    slots[i].key = off + 1u;
    slots[i].site = luna;
    uid = taiHookFunctionOffset(&site_ref[i], modid, 0, off, thumb, (const void *)observers[i]);
    if (uid < 0) {
        slots[i].key = 0;
        slots[i].site = NULL;
        DLOG(" tai %08X\n", (unsigned)uid);
        return VJO_PLACE_TAI;
    }
    site_uid[i] = uid;
    n_sites++;
    return VJO_PLACE_OK;
}

static void site_result(int r, uint32_t off, const uint8_t *code, uint32_t code_size)
{
    if (r == VJO_PLACE_OK) {
        DLOG(" hooked, stream %u\n", (unsigned)n_sites);
        return;
    }
    if (r < 0) {
        DLOG(" skipped: the address is hooked already\n");
        return;
    }
    n_refused++;
    DLOG(" refused: %s\n", vjo_place_reason(r));
    dlog_bytes(code, code_size, off);
    (void)off; (void)code; (void)code_size;
}

static void install_sites(const char *title_id, const tai_module_info_t *tai, const SceKernelModuleInfo *mi)
{
    const uint8_t *code = (const uint8_t *)mi->segments[0].vaddr;
    const uint32_t code_size = mi->segments[0].memsz;
    const VjoPlaceVerified *ver;
    const VjoLunaSite *sites;
    unsigned nv = vjo_place_verified(title_id, &ver), nl = vjo_luna_sites(title_id, &sites), need = nl;
    uint16_t appv;
    for (unsigned i = 0; i < nv; i++) need |= ver[i].app_ver; /* an entry that names its version */
    appv = need ? app_version() : 0;
    DLOG("main module %s NID %08X seg0 %08X+%X APP_VER %04X; %u verified, %u Luna sites\n", tai->name,
         (unsigned)tai->module_nid, (unsigned)seg_base, (unsigned)code_size, (unsigned)appv, nv, nl);
    for (unsigned i = 0; i < nv; i++) {
        const VjoPlaceVerified *v = &ver[i];
        /* a Luna site at the same address: the entry reads the text, Luna's hookfunc and filter post-process it */
        const VjoLunaSite *same = vjo_luna_site_at(sites, nl, v->off, appv);
        int r = vjo_place_verified_version(v, appv) ? vjo_place_check_verified(v, code, code_size, seg_base,
                                                                              tai->module_nid)
                                                     : VJO_PLACE_VERSION;
        DLOG("verified +%X %s r%u%s", (unsigned)v->off, v->thumb ? "thumb" : "arm", (unsigned)v->reg,
             same ? ", Luna's text" : "");
        if (r == VJO_PLACE_OK) r = hook_site(tai->modid, v->off, v->thumb, same, v);
        site_result(r, v->off, code, code_size);
    }
    for (unsigned i = 0; i < nl; i++) {
        const VjoLunaSite *l = &sites[i];
        uint32_t a = l->addr & ~1u, off = a - VJO_LUNA_SEG0;
        int r = VJO_PLACE_OK, thumb = 0;
        DLOG("luna %08X r%u", (unsigned)l->addr, (unsigned)l->reg);
        if (!l->ported) r = VJO_PLACE_UNPORTED;
        else if (!vjo_luna_version_ok(sites, nl, l, appv)) r = VJO_PLACE_VERSION;
        else if (a < VJO_LUNA_SEG0 || off >= code_size) r = VJO_PLACE_RANGE;
        else if ((thumb = vjo_place_isa(code + off, code_size - off, l->addr)) < 0) r = VJO_PLACE_ENTRY;
        else r = vjo_place_check(code, code_size, seg_base, off, thumb);
        if (r == VJO_PLACE_OK) r = hook_site(tai->modid, off, thumb, l, NULL);
        site_result(r, off, code, code_size);
    }
}

static void set_kinds(void)
{
    for (unsigned i = 0; i < VJO_HOOK_STREAMS; i++)
        slots[i].kind = i < VJO_HOOK_SITE_STREAMS ? VJO_HOOK_KIND_SITE : VJO_HOOK_KIND_CALL;
}

static void start(const char *title_id, unsigned word, int psp)
{
    tai_module_info_t tai;
    SceKernelModuleInfo mi;
    if (psp) {
        /* Nothing is hooked in the PSP emulator. Every game (and the table sites, hook_sites = on) also turns on
         * PSP text: the worker's own read of a profiled PSP game's page at job time (vjo_hooks_psp_poll); nothing
         * is read here, before any game runs in the emulator. */
        mem_info_ok = VJO_IMPORT_READY(sceKernelGetMemBlockInfoByAddr);
        psp_text_on = mem_info_ok && (word & VJO_TEXT_SOURCE_MODE_MASK) == VJO_TEXT_SOURCE_HOOKS &&
                      !(word & VJO_TEXT_SOURCE_NO_SITES);
        DLOG("the PSP emulator's process: no hooks; PSP text %s\n", psp_text_on ? "on (Every game)" : "off");
        return;
    }
    if (!title_id || !title_id[0]) {
        DLOG("nothing: no title ID\n");
        return;
    }
    if (main_module(&tai, &mi) < 0) {
        DLOG("nothing: no main module (eboot.bin) with a segment 0\n");
        return;
    }
    seg_base = (uint32_t)(uintptr_t)mi.segments[0].vaddr;
    mem_info_ok = VJO_IMPORT_READY(sceKernelGetMemBlockInfoByAddr);
    live = 1;
    /* hook_sites = off (audit-hooks F3): no table site, verified or Luna's, so a console run can tell the
     * CALL source from the sites */
    if (word & VJO_TEXT_SOURCE_NO_SITES) DLOG("sites off (hook_sites = off): no table site\n");
    else install_sites(title_id, &tai, &mi);
    set_kinds(); /* app_version may have used the CALL slots' storage */
    install_calls(tai.name);
    if (word & VJO_TEXT_SOURCE_NO_SITES)
        DLOG("sites off; CALL imports hooked %u of %u\n", (unsigned)n_calls, (unsigned)CALL_IMPORTS);
    else
        DLOG("sites hooked %u, refused %u; CALL imports hooked %u of %u\n", (unsigned)n_sites, (unsigned)n_refused,
             (unsigned)n_calls, (unsigned)CALL_IMPORTS);
}

void vjo_hooks_start(const char *title_id, unsigned word, int psp)
{
    const unsigned mode = word & VJO_TEXT_SOURCE_MODE_MASK;
    for (unsigned i = 0; i < CALL_IMPORTS; i++) call_uid[i] = -1;
    for (unsigned i = 0; i < VJO_HOOK_SITE_STREAMS; i++) site_uid[i] = -1;
    set_kinds();
    /* the word the kernel gave (an unknown mode, or a flag other than NO_SITES, is OCR), even when nothing
     * applies to this game; OCR carries no flag */
    applied_mode = (mode == VJO_TEXT_SOURCE_AUTO || mode == VJO_TEXT_SOURCE_HOOKS) &&
                           !(word & ~(VJO_TEXT_SOURCE_MODE_MASK | VJO_TEXT_SOURCE_NO_SITES))
                       ? word
                       : VJO_TEXT_SOURCE_OCR;
    if (applied_mode == VJO_TEXT_SOURCE_OCR) return;
#if VJO_DEBUG
    if (VJO_IMPORT_READY(sceIoOpen))
        dlog = sceIoOpen("ux0:data/VitaJPOverlay/hooks.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    DLOG("%.9s: text source %u%s\n", title_id ? title_id : "", (unsigned)mode,
         (applied_mode & VJO_TEXT_SOURCE_NO_SITES) ? ", sites off" : "");
#endif
    start(title_id, applied_mode, psp);
#if VJO_DEBUG
    if (dlog >= 0) sceIoClose(dlog);
    dlog = -1;
#endif
}

int vjo_hooks_stop(void)
{
    const int had = n_calls || n_sites;
    live = 0;
    if (VJO_IMPORT_READY(taiHookRelease)) {
        for (unsigned i = CALL_IMPORTS; i-- > 0;)
            if (call_uid[i] >= 0) {
                taiHookRelease(call_uid[i], call_ref[i]);
                call_uid[i] = -1;
            }
        for (unsigned i = n_sites; i-- > 0;)
            if (site_uid[i] >= 0) {
                taiHookRelease(site_uid[i], site_ref[i]);
                site_uid[i] = -1;
            }
    }
    /* A thread that had entered a hook before its release may still be on its way out: one counted in
     * in_flight, or one in the instructions the count does not cover (the observer's assembly around its C
     * call, a CALL hook's prologue and epilogue: audit-hooks N8). So a short wait after the releases whenever
     * a hook was installed, then the count. */
    if (had || __atomic_load_n(&in_flight, __ATOMIC_ACQUIRE)) sceKernelDelayThread(2000);
    return __atomic_load_n(&in_flight, __ATOMIC_ACQUIRE) ? 1 : 0;
}

void vjo_hooks_counts(uint32_t *mode, uint32_t *installed, uint32_t *refused)
{
    if (mode) *mode = applied_mode;
    if (installed) *installed = n_sites << 16 | n_calls;
    if (refused) *refused = n_refused;
}

int vjo_hooks_verified(uint32_t id)
{
    /* a site's stream ID is its slot + 1 (hook_site) */
    return id >= 1u && id <= n_sites && id <= VJO_HOOK_SITE_STREAMS && site_info[id - 1u].ver != NULL;
}

/* ---- job time ---- */

/* PSP text's read: Adrenaline's view of PSP RAM (core/psp_text.h vjo_psp_view), through read_game: only inside
 * the readable mapping sceKernelGetMemBlockInfoByAddr reports for each read, never past its end. The view is
 * uncached on the Vita side (memory type 0x80, the owner's console, #248), so no ARM cache stands between the
 * read and RAM; the PSP's own data cache can still hold a line the game has just written (docs/native-hooks.md,
 * "PSP text"). */
#ifndef VJO_PSP_VITA_ADDR
#define VJO_PSP_VITA_ADDR(a) vjo_psp_view(a)
#endif
static unsigned psp_read(void *ud, uint32_t psp_addr, void *dst, unsigned max, unsigned unit)
{
    return read_game(ud, VJO_PSP_VITA_ADDR(psp_addr), dst, max, unit);
}

int vjo_hooks_psp_poll(void)
{
    static const VjoPspTextOps ops = {NULL, psp_read};
    uint8_t text[VJO_HOOK_RAW_BYTES];
    const VjoPspProfile *p;
    const uint32_t had = n_sites;
    unsigned n = 0;
    if (!psp_text_on) return 0;
    psp_status = vjo_psp_text_poll(&ops, &p, text, sizeof text, &n);
    if (p != psp_profile) {
        /* another game in the emulator, its XMB, or code that is not the profile's: slot 0's text was the last
         * game's, and must never answer for this one. Only this thread uses the slot here: it is free. */
        if (!vjo_stream_try_lock(&slots[0])) return 0;
        slots[0].seq = slots[0].n = slots[0].hits = 0;
        slots[0].key = p ? p->site + 1u : 0u; /* a site's key: its address + 1 (vjo_hooks_snap) */
        vjo_stream_unlock(&slots[0]);
        psp_profile = p;
        n_sites = p ? 1u : 0u; /* stream 1, without a hook: what vjo_hooks_counts reports as installed */
    }
    if (psp_status == VJO_PSP_TEXT_OK) vjo_stream_offer(&slots[0], &stream_clock, text, n, p->encoding);
    return n_sites != had;
}

uint32_t vjo_hooks_psp_status(void)
{
    return psp_text_on ? psp_status : VJO_PSP_TEXT_OFF;
}

void vjo_hooks_keep(uint32_t id, uint32_t key)
{
    VjoStreamSlot *s;
    if (id <= VJO_HOOK_SITE_STREAMS || id > VJO_HOOK_STREAMS) return; /* a site's slot is never given away */
    s = &slots[id - 1];
    if (!key) { /* pinned: whichever caller holds it (a caller's key is never 0: it returns into segment 0) */
        vjo_stream_keep(s);
        return;
    }
    /* the caller the job saw, under the slot's lock: a caller replacing it (also under the lock) either finds
     * it kept, or changed the key first and it is not kept; busy: not now (the next agreeing job keeps it) */
    if (!vjo_stream_try_lock(s)) return;
    if (__atomic_load_n(&s->key, __ATOMIC_RELAXED) - seg_base == key) vjo_stream_keep(s);
    vjo_stream_unlock(s);
}

void vjo_hooks_call_stats(uint32_t *dropped, uint32_t *replaced)
{
    if (dropped) *dropped = __atomic_load_n(&stream_clock.call_dropped, __ATOMIC_RELAXED);
    if (replaced) *replaced = __atomic_load_n(&stream_clock.call_replaced, __ATOMIC_RELAXED);
}

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

uint32_t vjo_hooks_seq(uint32_t id, uint32_t *key, uint32_t *kind, uint32_t *bytes)
{
    const VjoStreamSlot *s;
    uint32_t k;
    if (!id || id > VJO_HOOK_STREAMS) return 0;
    s = &slots[id - 1];
    k = __atomic_load_n(&s->key, __ATOMIC_ACQUIRE);
    if (key) *key = s->kind == VJO_HOOK_KIND_SITE ? k - 1u : k - seg_base;
    if (kind) *kind = s->kind;
    if (bytes) *bytes = __atomic_load_n(&s->n, __ATOMIC_RELAXED);
    return __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
}

size_t vjo_hooks_text(const VjoHookSnap *s, char *out, size_t cap)
{
    size_t n;
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!s || !s->n || s->n > VJO_HOOK_RAW_BYTES) return 0;
    if (psp_profile && s->id == 1u && s->kind == VJO_HOOK_KIND_SITE && s->key == psp_profile->site) {
        /* PSP text: the profile's engine filter (core/psp_text.c) */
        n = vjo_luna_text_filter(NULL, psp_profile->filter, psp_profile->encoding, s->raw, s->n, s->encoding, out, cap);
    } else if (s->kind == VJO_HOOK_KIND_SITE && s->id >= 1u && s->id <= n_sites && s->id <= VJO_HOOK_SITE_STREAMS &&
        site_info[s->id - 1u].ver && site_info[s->id - 1u].ver->filter &&
        site_info[s->id - 1u].addr - seg_base == s->key) {
        /* a Rubi verified entry's own filter, in place of a Luna site's at its address */
        const VjoPlaceVerified *v = site_info[s->id - 1u].ver;
        n = vjo_luna_text_filter(s->site, v->filter, v->encoding, s->raw, s->n, s->encoding, out, cap);
    } else if (s->site) n = vjo_luna_text(s->site, s->raw, s->n, s->encoding, out, cap);
    else n = vjo_hook_decode(s->encoding, s->raw, s->n, out, cap, NULL);
    /* One clean text for selection, display, mining and history's character count. */
    return vjo_hook_clean(out, n);
}
