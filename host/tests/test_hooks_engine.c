/* The game worker's hook engine (game_ocr/hooks.c) itself, built for the host against stubbed taiHEN,
 * modulemgr, sysmem and io (host/tests/hooks_stubs, then game_stubs) (audit-hooks F4): the CALL hooks'
 * bounds (4..512 bytes, the NUL rules, the pointer-array refusal), read_game's block bounds and address
 * wrap, the install order (Rubi's verified entries first, then Luna's sites), the refusal order (ported,
 * version, range, entry), the 8 site slots and duplicate addresses, NO_SITES, the CALL slots' replacement and
 * keep rules and their stats, and vjo_hooks_stop (newest first; STOP_FAIL while a thread is inside).
 *
 * hooks.c keeps game addresses as uint32_t, as on the Vita: the fake code segment and game memory are mapped
 * below 4 GiB (MAP_32BIT on x86_64). The fake taiHEN records each hook and gives each a record whose
 * continuation is a real host function (CALL) or a marker (sites), so a test calls a hook as the game would. */
#include "acutest.h"

#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#include <psp2/host_stubs.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/modulemgr.h>
#include <taihen.h>

#include "../../core/hook_luna.h"
#include "../../core/hook_place.h"
#include "../../include/vjo_api.h"

/* Each import's readiness (a test marks one missing), and the tables a test gives a synthetic title. */
static int test_ready(const void *fn);
static unsigned test_place_verified(const char *title_id, const VjoPlaceVerified **out);
static int test_place_title_verified(const char *title_id);
static unsigned test_luna_sites(const char *title_id, const VjoLunaSite **sites);
static int test_luna_title_ported(const char *title_id);
#define VJO_IMPORT_H 1
#define VJO_IMPORT_READY(fn) test_ready((const void *)(uintptr_t)(fn))
#define vjo_place_verified test_place_verified
#define vjo_place_title_verified test_place_title_verified
#define vjo_luna_sites test_luna_sites
#define vjo_luna_title_ported test_luna_title_ported
#include "../../game_ocr/hooks.c"
#undef vjo_place_verified
#undef vjo_place_title_verified
#undef vjo_luna_sites
#undef vjo_luna_title_ported
unsigned vjo_place_verified(const char *title_id, const VjoPlaceVerified **out);
int vjo_place_title_verified(const char *title_id);
unsigned vjo_luna_sites(const char *title_id, const VjoLunaSite **sites);
int vjo_luna_title_ported(const char *title_id);

/* CP932 (Python's cp932 codec) */
#define L_TODAY "\x8D\xA1\x93\xFA\x82\xCD\x82\xA2\x82\xA2\x93\x56\x8B\x43\x82\xC5\x82\xB7\x82\xCB\x81\x42" /* 今日はいい天気ですね。 22 B */
#define L_TOMORROW "\x82\xDC\x82\xBD\x96\xBE\x93\xFA\x81\x41\x88\xEA\x8F\x8F\x82\xC9\x8C\xA9\x82\xC9\x8D\x73\x82\xB1\x82\xA4\x81\x49" /* また明日、一緒に見に行こう！ 28 B */
#define L_AI "\x82\xA0\x82\xA2" /* あい 4 B */
#define L_HELLO "\x82\xB1\x82\xF1\x82\xC9\x82\xBF\x82\xCD\x81\x41\x95\xFC\x96\xE7\x82\xAD\x82\xF1" /* こんにちは、朋也くん 20 B */

#define TITLE "TEST00001"     /* the synthetic title: test tables */
#define UNLISTED "TEST00002"  /* a title with no hook entry */
#define CODE_SIZE 0x4000u
#define DATA_SIZE 0x4000u
#define PAGE 0x1000u

/* ---- memory below 4 GiB ---- */

static uint8_t *code, *data; /* code: CODE_SIZE bytes, then a PROT_NONE page; data: the game's memory */

static uint8_t *low_map(size_t n, size_t guard)
{
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_32BIT
    flags |= MAP_32BIT;
#endif
    uint8_t *p = mmap(NULL, n + guard, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (p == MAP_FAILED || (uint64_t)(uintptr_t)p + n + guard > 0xFFFFFFFFull) return NULL;
    if (guard && mprotect(p + n, guard, PROT_NONE)) return NULL;
    return p;
}

static uint32_t a32(const void *p) { return (uint32_t)(uintptr_t)p; }

/* ---- fake imports ---- */

static const void *missing[8];
static unsigned n_missing;
static int test_ready(const void *fn)
{
    for (unsigned i = 0; i < n_missing; i++)
        if (missing[i] == fn) return 0;
    return 1;
}
#define MISS(fn) (missing[n_missing++] = (const void *)(uintptr_t)(fn))

/* ---- the synthetic title's tables ---- */

static const VjoPlaceVerified *t_ver;
static unsigned t_nver;
static const VjoLunaSite *t_luna;
static unsigned t_nluna;
static int is_test_title(const char *t) { return t && !strcmp(t, TITLE); }
static unsigned test_place_verified(const char *title_id, const VjoPlaceVerified **out)
{
    if (!is_test_title(title_id)) return vjo_place_verified(title_id, out);
    *out = t_nver ? t_ver : NULL;
    return t_nver;
}
static int test_place_title_verified(const char *title_id)
{
    return is_test_title(title_id) ? t_nver > 0 : vjo_place_title_verified(title_id);
}
static unsigned test_luna_sites(const char *title_id, const VjoLunaSite **sites)
{
    if (!is_test_title(title_id)) return vjo_luna_sites(title_id, sites);
    if (sites) *sites = t_nluna ? t_luna : NULL;
    return t_nluna;
}
static int test_luna_title_ported(const char *title_id)
{
    if (!is_test_title(title_id)) return vjo_luna_title_ported(title_id);
    for (unsigned i = 0; i < t_nluna; i++)
        if (vjo_luna_ported(&t_luna[i])) return 1;
    return 0;
}

/* ---- taiHEN and modulemgr ---- */

static struct {
    int tai_rc, mi_rc;
    SceUID modid;
    uint32_t nid;
    char name[27];
    char path[256];
    void *seg0;
    SceSize seg0_size;
} mod;

int taiGetModuleInfo(const char *module, tai_module_info_t *info)
{
    TEST_CHECK(module == TAI_MAIN_MODULE);
    TEST_CHECK(info->size == sizeof(*info));
    if (mod.tai_rc < 0) return mod.tai_rc;
    info->modid = mod.modid;
    info->module_nid = mod.nid;
    snprintf(info->name, sizeof(info->name), "%s", mod.name);
    return 0;
}

int sceKernelGetModuleInfo(SceUID modid, SceKernelModuleInfo *info)
{
    TEST_CHECK(modid == mod.modid);
    TEST_CHECK(info->size == sizeof(*info));
    if (mod.mi_rc < 0) return mod.mi_rc;
    memcpy(info->path, mod.path, sizeof(info->path));
    info->segments[0].vaddr = mod.seg0;
    info->segments[0].memsz = mod.seg0_size;
    return 0;
}

/* the game's own string functions, under the hooks */
static unsigned reals, real_runs[CALL_IMPORTS]; /* all, and each function's */
#define RAN(k) (reals++, real_runs[k]++)
static void *real_memcpy(void *d, const void *s, size_t n) { RAN(CALL_MEMCPY); return memcpy(d, s, n); }
static char *real_strcpy(char *d, const char *s) { RAN(CALL_STRCPY); return strcpy(d, s); }
static char *real_strncpy(char *d, const char *s, size_t n) { RAN(CALL_STRNCPY); return strncpy(d, s, n); }
static void *real_memmove(void *d, const void *s, size_t n) { RAN(CALL_MEMMOVE); return memmove(d, s, n); }
static void *real_clib_memcpy(void *d, const void *s, SceSize n) { RAN(CALL_CLIB_MEMCPY); return memcpy(d, s, n); }
static char *real_clib_strncpy(char *d, const char *s, SceSize n) { RAN(CALL_CLIB_STRNCPY); return strncpy(d, s, n); }
static SceSize real_clib_strlcpy(char *d, const char *s, SceSize cap)
{
    size_t n = strlen(s), k;
    RAN(CALL_CLIB_STRLCPY);
    if (cap) {
        k = n < cap - 1u ? n : cap - 1u;
        memcpy(d, s, k);
        d[k] = 0;
    }
    return (SceSize)n;
}
static SceSize real_clib_strnlen(const char *s, SceSize max) { RAN(CALL_CLIB_STRNLEN); return (SceSize)strnlen(s, max); }
static unsigned chained_runs;
static void *chained_memcpy(void *d, const void *s, size_t n) { chained_runs++; return memcpy(d, s, n); }

static const struct {
    uint32_t lib, nid;
    void *real;
} game_imports[CALL_IMPORTS] = {
    {NID_SCE_LIBC, 0x7205BFDBu, (void *)real_memcpy},       {NID_SCE_LIBC, 0x85B924B7u, (void *)real_strcpy},
    {NID_SCE_LIBC, 0x9F87712Du, (void *)real_strncpy},      {NID_SCE_LIBC, 0xAF5C218Du, (void *)real_memmove},
    {NID_SCE_CLIB, 0x14E9DBD7u, (void *)real_clib_memcpy},  {NID_SCE_CLIB, 0xC458D60Au, (void *)real_clib_strncpy},
    {NID_SCE_CLIB, 0x2CDFCD1Cu, (void *)real_clib_strlcpy}, {NID_SCE_CLIB, 0xAC595E68u, (void *)real_clib_strnlen},
};

typedef struct {
    struct _tai_hook_user user;
    char module[28]; /* the name hooks.c passed (its stack's, valid during the call only) */
    uint32_t lib, nid;
    const void *func;
    SceUID uid;
} ImportRec;
static ImportRec imp[CALL_IMPORTS];
static unsigned n_imp;
static uint32_t not_imported[CALL_IMPORTS]; /* function NIDs the game does not import */
static unsigned n_not_imported;
#define TAI_NOT_FOUND ((SceUID)0x90010003u)

SceUID taiHookFunctionImportForUser(tai_hook_ref_t *p_hook, tai_hook_args_t *args)
{
    ImportRec *r;
    void *real = NULL;
    TEST_CHECK(args->size == sizeof(*args));
    for (unsigned k = 0; k < n_not_imported; k++)
        if (args->func_nid == not_imported[k]) return TAI_NOT_FOUND;
    for (unsigned k = 0; k < CALL_IMPORTS; k++)
        if (game_imports[k].lib == args->library_nid && game_imports[k].nid == args->func_nid) real = game_imports[k].real;
    TEST_ASSERT_(real != NULL, "an import the game's table has: %08X/%08X", (unsigned)args->library_nid,
                 (unsigned)args->func_nid);
    TEST_ASSERT(n_imp < CALL_IMPORTS);
    r = &imp[n_imp++];
    snprintf(r->module, sizeof(r->module), "%s", args->module);
    r->lib = args->library_nid;
    r->nid = args->func_nid;
    r->func = args->hook_func;
    r->user.next = 0;
    r->user.func = (void *)(uintptr_t)args->hook_func;
    r->user.old = real;
    r->uid = 0x2000 + (SceUID)n_imp;
    *p_hook = (tai_hook_ref_t)&r->user;
    return r->uid;
}

typedef struct {
    struct _tai_hook_user user;
    SceUID modid;
    int segidx;
    uint32_t offset;
    int thumb;
    const void *source;
    SceUID uid;
} OffsetRec;
static OffsetRec offs[16];
static unsigned n_offs, offset_calls;
static uint32_t fail_offset; /* taiHEN refuses this offset (0: none) */
#define CONT(k) ((void *)(uintptr_t)(0xC0DE0000u + (k))) /* the moved instructions' trampoline */
#define TAI_PATCH_FAIL ((SceUID)0x90010005u)

SceUID taiHookFunctionOffsetForUser(tai_hook_ref_t *p_hook, tai_offset_args_t *args)
{
    OffsetRec *r;
    TEST_CHECK(args->size == sizeof(*args));
    offset_calls++;
    if (fail_offset && args->offset == fail_offset) return TAI_PATCH_FAIL;
    TEST_ASSERT(n_offs < 16);
    r = &offs[n_offs];
    r->modid = args->modid;
    r->segidx = args->segidx;
    r->offset = args->offset;
    r->thumb = args->thumb;
    r->source = args->source;
    r->user.next = 0;
    r->user.func = (void *)(uintptr_t)args->source;
    r->user.old = CONT(n_offs);
    r->uid = 0x3000 + (SceUID)n_offs;
    n_offs++;
    *p_hook = (tai_hook_ref_t)&r->user;
    return r->uid;
}

static SceUID released[32];
static unsigned n_released;
int taiHookRelease(SceUID tai_uid, tai_hook_ref_t hook)
{
    int found = 0;
    for (unsigned k = 0; k < n_imp; k++)
        if (imp[k].uid == tai_uid) found = hook == (tai_hook_ref_t)&imp[k].user;
    for (unsigned k = 0; k < n_offs; k++)
        if (offs[k].uid == tai_uid) found = hook == (tai_hook_ref_t)&offs[k].user;
    TEST_CHECK_(found, "release of hook %08X with its own record", (unsigned)tai_uid);
    if (n_released < 32) released[n_released++] = tai_uid;
    return 0;
}

/* ---- sysmem: the game's memory blocks ---- */

typedef struct {
    uint32_t base, size;
    unsigned access;
} Block;
static Block blocks[4];
static unsigned n_blocks, mb_calls;
static int liar = -1; /* a block answered for every address (a kernel bug read_game must survive) */

int sceKernelGetMemBlockInfoByAddr(void *addr, SceKernelMemBlockInfo *mb)
{
    const uint32_t a = a32(addr);
    const Block *b = NULL;
    mb_calls++;
    TEST_CHECK(mb->size == sizeof(*mb));
    if (liar >= 0) b = &blocks[liar];
    for (unsigned k = 0; !b && k < n_blocks; k++)
        if (a >= blocks[k].base && a - blocks[k].base < blocks[k].size) b = &blocks[k];
    if (!b) return (int)0x80020D0Au;
    mb->mappedBase = (void *)(uintptr_t)b->base;
    mb->mappedSize = b->size;
    mb->access = b->access;
    return 0;
}

/* ---- io: param.sfo and the debug build's hooks.log ---- */

static uint8_t sfo[64];
static int sfo_len; /* < 0: no param.sfo */
static unsigned sfo_opens, log_opens, closes;
static int log_flags;
static char logbuf[16384];
static size_t loglen;

SceUID sceIoOpen(const char *path, int flags, unsigned mode)
{
    (void)mode;
    if (!strcmp(path, "app0:sce_sys/param.sfo")) {
        sfo_opens++;
        TEST_CHECK(flags == SCE_O_RDONLY);
        return sfo_len < 0 ? (SceUID)0x80010002u : 10;
    }
    if (!strcmp(path, "ux0:data/VitaJPOverlay/hooks.log")) {
        log_opens++;
        log_flags = flags;
        loglen = 0;
        logbuf[0] = 0;
        return 11;
    }
    TEST_CHECK_(0, "no other file: %s", path);
    return -1;
}

int sceIoRead(SceUID fd, void *buf, SceSize n)
{
    size_t k = sfo_len < 0 ? 0u : (size_t)sfo_len;
    TEST_CHECK(fd == 10);
    if (k > n) k = n;
    memcpy(buf, sfo, k);
    return (int)k;
}

int sceIoWrite(SceUID fd, const void *d, SceSize n)
{
    TEST_CHECK(fd == 11);
    if (loglen + n < sizeof(logbuf)) {
        memcpy(logbuf + loglen, d, n);
        loglen += n;
        logbuf[loglen] = 0;
    }
    return (int)n;
}

int sceIoClose(SceUID fd)
{
    (void)fd;
    closes++;
    return 0;
}

/* param.sfo with one key, APP_VER (its 0x0204 string format) */
static void set_sfo(const char *app_ver)
{
    const unsigned keys = 20 + 16, values = keys + 8, len = (unsigned)strlen(app_ver) + 1u;
    memset(sfo, 0, sizeof(sfo));
    memcpy(sfo, "\0PSF", 4);
    sfo[4] = 1;
    sfo[8] = (uint8_t)keys;
    sfo[12] = (uint8_t)values;
    sfo[16] = 1;
    sfo[22] = 0x04; /* the entry: key 0, format 0x0204, length, capacity 8, data 0 */
    sfo[23] = 0x02;
    sfo[24] = (uint8_t)len;
    sfo[28] = 8;
    memcpy(sfo + keys, "APP_VER", 8);
    memcpy(sfo + values, app_ver, len);
    sfo_len = (int)(values + 8);
}

/* ---- threadmgr ---- */

static unsigned delays, delay_us, released_at_delay;
static int leave_on_delay; /* the thread inside a hook leaves while vjo_hooks_stop waits */
int sceKernelDelayThread(unsigned us)
{
    delays++;
    delay_us = us;
    released_at_delay = n_released;
    if (leave_on_delay) __atomic_store_n(&in_flight, 0u, __ATOMIC_RELEASE);
    return 0;
}

/* ---- the observers (hooks_observer.S on the Vita): distinct bodies, distinct addresses ---- */

static volatile unsigned observer_runs[VJO_HOOK_SITE_STREAMS];
void vjo_hooks_observer_0(void) { observer_runs[0]++; }
void vjo_hooks_observer_1(void) { observer_runs[1] += 2; }
void vjo_hooks_observer_2(void) { observer_runs[2] += 3; }
void vjo_hooks_observer_3(void) { observer_runs[3] += 4; }
void vjo_hooks_observer_4(void) { observer_runs[4] += 5; }
void vjo_hooks_observer_5(void) { observer_runs[5] += 6; }
void vjo_hooks_observer_6(void) { observer_runs[6] += 7; }
void vjo_hooks_observer_7(void) { observer_runs[7] += 8; }

/* ---- the engine's state and the fakes', reset ---- */

static void reset(void)
{
    if (!code) code = low_map(CODE_SIZE, PAGE);
    if (!data) data = low_map(DATA_SIZE, PAGE);
    TEST_ASSERT_(code && data, "memory below 4 GiB");
    memset(code, 0, CODE_SIZE);
    memset(data, 0, DATA_SIZE);
    /* hooks.c */
    memset(slots, 0, sizeof(slots));
    memset(&stream_clock, 0, sizeof(stream_clock));
    memset(call_ref, 0, sizeof(call_ref));
    memset(call_uid, 0, sizeof(call_uid));
    live = 0;
    in_flight = 0;
    applied_mode = n_sites = n_calls = n_refused = 0;
    seg_base = 0;
    mem_info_ok = 0;
    memset(site_info, 0, sizeof(site_info));
    memset(site_ref, 0, sizeof(site_ref));
    memset(site_uid, 0, sizeof(site_uid));
    memset(scratch, 0, sizeof(scratch));
    scratch_lock = 0;
#if VJO_DEBUG
    dlog = -1;
#endif
    set_kinds();
    /* the fakes */
    n_missing = 0;
    t_ver = NULL;
    t_nver = 0;
    t_luna = NULL;
    t_nluna = 0;
    memset(&mod, 0, sizeof(mod));
    mod.modid = 0x40010001;
    mod.nid = 0x12345678u;
    snprintf(mod.name, sizeof(mod.name), "TestGame");
    snprintf(mod.path, sizeof(mod.path), "app0:eboot.bin");
    mod.seg0 = code;
    mod.seg0_size = CODE_SIZE;
    reals = chained_runs = 0;
    memset(real_runs, 0, sizeof(real_runs));
    memset(imp, 0, sizeof(imp));
    n_imp = n_not_imported = 0;
    memset(offs, 0, sizeof(offs));
    n_offs = offset_calls = 0;
    fail_offset = 0;
    n_released = 0;
    memset(blocks, 0, sizeof(blocks));
    n_blocks = mb_calls = 0;
    liar = -1;
    sfo_len = -1;
    sfo_opens = log_opens = closes = 0;
    log_flags = 0;
    loglen = 0;
    logbuf[0] = 0;
    delays = delay_us = released_at_delay = 0;
    leave_on_delay = 0;
}

static void add_block(uint32_t base, uint32_t size, unsigned access)
{
    blocks[n_blocks].base = base;
    blocks[n_blocks].size = size;
    blocks[n_blocks].access = access;
    n_blocks++;
}

/* The CALL streams that hold a caller now. */
static unsigned call_slots_used(void)
{
    unsigned n = 0;
    for (unsigned i = VJO_HOOK_SITE_STREAMS; i < VJO_HOOK_STREAMS; i++) n += slots[i].key != 0;
    return n;
}

/* The CALL stream whose latest text is these n bytes: its id, or 0. */
static uint32_t call_stream_with(const void *p, unsigned n)
{
    VjoHookSnap s;
    for (uint32_t id = VJO_HOOK_SITE_STREAMS + 1u; id <= VJO_HOOK_STREAMS; id++)
        if (vjo_hooks_snap(id, &s) && s.kind == VJO_HOOK_KIND_CALL && s.n == n && !memcmp(s.raw, p, n)) return id;
    return 0;
}

/* A pointer struct of 20 bytes (five user pointers, 0x81000000-0x9FFFFFFF) that passes the CALL prefilter;
 * with cut, one whose bytes before a NUL at 8 still pass it (a struct with a short text-like start). */
static int pointer_struct(uint8_t b[20], int cut)
{
    uint32_t x = 7u;
    for (unsigned tries = 0; tries < 2000000u; tries++) {
        for (unsigned w = 0; w < 5; w++) {
            uint32_t v;
            x = x * 1103515245u + 12345u;
            v = 0x81000000u + (x >> 3) % 0x1F000000u;
            b[4 * w] = (uint8_t)v;
            b[4 * w + 1] = (uint8_t)(v >> 8);
            b[4 * w + 2] = (uint8_t)(v >> 16);
            b[4 * w + 3] = (uint8_t)(v >> 24);
        }
        if (cut) b[8] = 0;
        if (vjo_place_text_prefilter(b, 20) && vjo_place_pointer_array(b, 20)) return 1;
    }
    return 0;
}

/* ---- the CALL source ---- */

static void test_call_hit_bounds(void)
{
    uint8_t b[600], ptr[20];
    const uint32_t K = 0x81000100u;
    reset();
    seg_base = a32(code);

    /* nothing before vjo_hooks_start (live 0) */
    memcpy(b, L_TODAY, 22);
    call_hit(b, 22, 1, K);
    TEST_CHECK(call_slots_used() == 0);
    live = 1;

    /* a mem copy: exactly its bytes */
    call_hit(b, 22, 1, K);
    TEST_CHECK(call_stream_with(L_TODAY, 22) == VJO_HOOK_SITE_STREAMS + 1u);
    /* 4 bytes at least, of the copy and of its text */
    memcpy(b, L_AI, 4);
    call_hit(b, 3, 1, K + 4);
    call_hit(b, 3, 0, K + 4);
    call_hit(NULL, 22, 1, K + 4);
    TEST_CHECK(call_slots_used() == 1);
    call_hit(b, 4, 1, K + 4);
    TEST_CHECK(call_stream_with(L_AI, 4) != 0);
    /* a mem copy's text ends at its first NUL; one of more than 512 bytes is not text */
    memset(b, 0, sizeof(b));
    memcpy(b, L_TOMORROW, 28);
    memset(b + 29, 'x', 3);
    call_hit(b, 32, 1, K + 8);
    TEST_CHECK(call_stream_with(L_TOMORROW, 28) != 0);
    for (unsigned i = 0; i + 22 <= sizeof(b); i += 22) memcpy(b + i, L_TODAY, 22);
    call_hit(b, 513, 1, K + 12);
    TEST_CHECK(call_slots_used() == 3);
    call_hit(b, 512, 1, K + 12);
    TEST_CHECK(call_stream_with(b, 512) != 0);
    /* a string copy's text must end (NUL) inside 512 bytes; a longer copy's first 512 are looked at */
    call_hit(b, 600, 0, K + 16);
    call_hit(b, 512, 0, K + 16);
    TEST_CHECK(call_slots_used() == 4);
    b[506] = 0;
    call_hit(b, 600, 0, K + 16);
    TEST_CHECK(call_stream_with(b, 506) != 0);
    /* not text: ASCII only, a NUL in the first 4 bytes */
    call_hit((const uint8_t *)"Hello, world", 12, 1, K + 20);
    call_hit((const uint8_t *)"\x82\xA0\0\0\x82\xA2\x82\xA4", 8, 1, K + 20);
    TEST_CHECK(call_slots_used() == 5);

    /* a pointer array's shape (audit-hooks F2): refused, a mem copy's whole copy too */
    TEST_ASSERT(pointer_struct(ptr, 0));
    call_hit(ptr, 20, 1, K + 24);
    call_hit(ptr, 20, 0, K + 24);
    TEST_CHECK(call_slots_used() == 5);
    TEST_ASSERT(pointer_struct(ptr, 1));
    call_hit(ptr, 20, 1, K + 24); /* its 8 bytes of text are a pointer struct's start */
    TEST_CHECK(call_slots_used() == 5);
    call_hit(ptr, 20, 0, K + 24); /* a string copy: only the 8 bytes it keeps */
    TEST_CHECK(call_stream_with(ptr, 8) != 0);
}

/* Each CALL hook: the game's function first (its result returned), then its bytes as the hook's rule says. */
static void test_call_hooks(void)
{
    char d[64], s[64];
    uint32_t mode = 9, inst = 9, refused = 9;
    struct _tai_hook_user chained;
    reset();
    vjo_hooks_start(UNLISTED, VJO_TEXT_SOURCE_HOOKS, 0);
    TEST_ASSERT(n_imp == CALL_IMPORTS);
    for (unsigned k = 0; k < CALL_IMPORTS; k++) {
        TEST_CHECK(!strcmp(imp[k].module, "TestGame"));
        TEST_CHECK(imp[k].lib == call_imports[k].lib && imp[k].nid == call_imports[k].nid);
        TEST_CHECK(imp[k].func == call_imports[k].func);
    }
    vjo_hooks_counts(&mode, &inst, &refused);
    TEST_CHECK(mode == VJO_TEXT_SOURCE_HOOKS && inst == CALL_IMPORTS && refused == 0);
    TEST_CHECK(n_offs == 0 && sfo_opens == 0);
    TEST_CHECK(slots[VJO_HOOK_SITE_STREAMS].kind == VJO_HOOK_KIND_CALL && slots[0].kind == VJO_HOOK_KIND_SITE);

    memset(d, 0x55, sizeof(d));
    TEST_CHECK(((void *(*)(void *, const void *, size_t))imp[CALL_MEMCPY].func)(d, L_TODAY, 22) == d);
    TEST_CHECK(reals == 1 && !memcmp(d, L_TODAY, 22));
    TEST_CHECK(call_stream_with(L_TODAY, 22) != 0);
    TEST_CHECK(((char *(*)(char *, const char *))imp[CALL_STRCPY].func)(d, L_TOMORROW) == d);
    TEST_CHECK(call_stream_with(L_TOMORROW, 28) != 0);
    TEST_CHECK(((char *(*)(char *, const char *, size_t))imp[CALL_STRNCPY].func)(d, L_HELLO, 40) == d);
    TEST_CHECK(call_stream_with(L_HELLO, 20) != 0); /* its NUL padding is not text */
    memcpy(s, L_AI L_AI, 9);
    TEST_CHECK(((void *(*)(void *, const void *, size_t))imp[CALL_MEMMOVE].func)(d, s, 8) == d);
    TEST_CHECK(call_stream_with(L_AI L_AI, 8) != 0);
    TEST_CHECK(((void *(*)(void *, const void *, SceSize))imp[CALL_CLIB_MEMCPY].func)(d, L_AI, 4) == d);
    TEST_CHECK(call_stream_with(L_AI, 4) != 0);
    memset(d, 0x55, sizeof(d));
    TEST_CHECK(((char *(*)(char *, const char *, SceSize))imp[CALL_CLIB_STRNCPY].func)(d, L_TOMORROW, 28) == d);
    TEST_CHECK(call_stream_with(L_TOMORROW, 28) != 0); /* no NUL: exactly the 28 bytes written */
    TEST_CHECK(((SceSize(*)(char *, const char *, SceSize))imp[CALL_CLIB_STRLCPY].func)(d, L_HELLO, 11) == 20);
    TEST_CHECK(call_stream_with(L_HELLO, 10) != 0); /* min(strlen, cap - 1) bytes and a NUL */
    memcpy(s, L_TODAY, 23);
    TEST_CHECK(((SceSize(*)(const char *, SceSize))imp[CALL_CLIB_STRNLEN].func)(s, 512) == 22);
    TEST_CHECK(((SceSize(*)(const char *, SceSize))imp[CALL_CLIB_STRNLEN].func)(s, 8) == 8);
    TEST_CHECK(call_stream_with(L_TODAY, 8) != 0); /* the 8 bytes the game measured */
    TEST_CHECK(reals == 9 && in_flight == 0);
    for (unsigned k = 0; k < CALL_IMPORTS; k++)
        TEST_CHECK_(real_runs[k] == (k == CALL_CLIB_STRNLEN ? 2u : 1u), "import %u: its own function", k);
    TEST_CHECK(call_slots_used() == 9);

    /* another hook after ours (taiHEN's chain): TAI_CONTINUE runs it, not the game's function */
    chained.next = 0;
    chained.old = NULL;
    chained.func = (void *)(uintptr_t)chained_memcpy;
    imp[CALL_MEMCPY].user.next = (uintptr_t)&chained;
    ((void *(*)(void *, const void *, size_t))imp[CALL_MEMCPY].func)(d, L_HELLO, 20);
    TEST_CHECK(chained_runs == 1 && reals == 9 && !memcmp(d, L_HELLO, 20));
    imp[CALL_MEMCPY].user.next = 0;

    /* more than 512 bytes: a mem copy is no text even with a NUL inside 512; a string copy's text is */
    {
        static char big_s[640], big_d[640];
        const unsigned used = call_slots_used();
        for (unsigned i = 0; i + 22 <= 600; i += 22) memcpy(big_s + i, L_TODAY, 22);
        big_s[506] = 0;
        ((void *(*)(void *, const void *, size_t))imp[CALL_MEMCPY].func)(big_d, big_s, 600);
        TEST_CHECK(call_stream_with(big_d, 506) == 0 && call_slots_used() == used);
        ((char *(*)(char *, const char *, size_t))imp[CALL_STRNCPY].func)(big_d, big_s, 600);
        TEST_CHECK(call_stream_with(big_d, 506) != 0);
    }
}

/* ---- read_game ---- */

static void test_read_game(void)
{
    uint8_t out[64];
    uint32_t base;
    reset();
    base = a32(data);
    add_block(base, PAGE, SCE_KERNEL_MEMORY_ACCESS_R | SCE_KERNEL_MEMORY_ACCESS_W);
    memcpy(data + 0x10, "abc\0def", 8);
    TEST_CHECK(read_game(NULL, base + 0x10, out, 16, 1) == 0 && mb_calls == 0); /* sysmem not resolved */
    mem_info_ok = 1;
    TEST_CHECK(read_game(NULL, 0, out, 16, 1) == 0);
    TEST_CHECK(read_game(NULL, base + 0x10, out, 0, 1) == 0);
    TEST_CHECK(read_game(NULL, base + 0x10, NULL, 16, 1) == 0);
    TEST_CHECK(mb_calls == 0);
    /* unit 1: through the first NUL, counted; unit 0: exactly max */
    TEST_CHECK(read_game(NULL, base + 0x10, out, 16, 1) == 4 && !memcmp(out, "abc", 4));
    TEST_CHECK(read_game(NULL, base + 0x10, out, 16, 0) == 16 && !memcmp(out, "abc\0def", 8));
    TEST_CHECK(read_game(NULL, base + 0x10, out, 2, 1) == 2);
    /* unit 2: through the first NUL pair at an even offset; an odd max rounded down */
    memcpy(data + 0x40, "a\0\0b\0\0c\0", 8);
    TEST_CHECK(read_game(NULL, base + 0x40, out, 16, 2) == 6);
    memcpy(data + 0x60, "a\0b\0c\0d\0e\0", 10);
    TEST_CHECK(read_game(NULL, base + 0x60, out, 7, 2) == 6);
    /* the block's end: unit 1 and 2 stop there, unit 0 reads nothing */
    memset(data + PAGE - 8, 'x', 8);
    memset(out, 0xEE, sizeof(out));
    TEST_CHECK(read_game(NULL, base + PAGE - 5, out, 16, 1) == 5 && out[5] == 0xEE);
    TEST_CHECK(read_game(NULL, base + PAGE - 5, out, 16, 2) == 4);
    TEST_CHECK(read_game(NULL, base + PAGE - 5, out, 16, 0) == 0);
    TEST_CHECK(read_game(NULL, base + PAGE - 5, out, 5, 0) == 5);
    /* no block, or one that is not readable */
    TEST_CHECK(read_game(NULL, base + PAGE, out, 16, 1) == 0);
    add_block(base + 2 * PAGE, PAGE, SCE_KERNEL_MEMORY_ACCESS_W);
    TEST_CHECK(read_game(NULL, base + 2 * PAGE, out, 16, 1) == 0);
    /* a kernel answer that does not hold the address: one that starts after it, or ends at it */
    blocks[0].base = base + 0x100;
    blocks[0].size = PAGE;
    liar = 0;
    TEST_CHECK(read_game(NULL, base + 0x10, out, 16, 1) == 0);
    blocks[0].base = base;
    blocks[0].size = 0x10;
    TEST_CHECK(read_game(NULL, base + 0x10, out, 16, 1) == 0);
    blocks[0].size = 0x8; /* one that ends before it */
    TEST_CHECK(read_game(NULL, base + 0x10, out, 16, 1) == 0);
    /* a block whose end wraps past 4 GiB holds nothing at the top (never read: no memory there) */
    blocks[0].base = 0xFFFF0000u;
    blocks[0].size = 0x20000u;
    blocks[0].access = SCE_KERNEL_MEMORY_ACCESS_R;
    TEST_CHECK(read_game(NULL, 0xFFFF8000u, out, 16, 1) == 0);
    blocks[0].base = 0xFFFFF000u;
    blocks[0].size = 0x1000u;
    TEST_CHECK(read_game(NULL, 0xFFFFF800u, out, 16, 0) == 0);
}

/* ---- table sites ---- */

typedef struct {
    int thumb, ok, entry;
    uint8_t b[12];
    unsigned n;
    const char *src;
} Form;
static const Form forms[] = {
#include "fixtures/hook_place_forms.inc"
};

static const Form *form(int thumb, int entry, const char *src)
{
    for (unsigned i = 0; i < sizeof(forms) / sizeof(forms[0]); i++)
        if (forms[i].thumb == thumb && forms[i].entry == entry && !strcmp(forms[i].src, src)) return &forms[i];
    TEST_ASSERT_(0, "fixture form %s", src);
    return NULL;
}

/* A function entry the site check accepts at code + off (16 bytes: the patch and taiHEN's saved bytes):
 * Thumb PUSH {r4, lr} then MOV r7, sp; ARM PUSH {r4, lr} then SUB sp, sp, #16 (the assembler's bytes).
 * window: the instruction after the entry instead (a refused one). */
static void put_site(uint32_t off, int thumb, const Form *window)
{
    const Form *e = form(thumb, 1, "push {r4, lr}"), *w = form(thumb, 0, thumb ? "mov r7, sp" : "sub sp, sp, #16");
    unsigned pos = 0;
    memcpy(code + off, e->b, e->n);
    pos = e->n;
    if (window) {
        memcpy(code + off + pos, window->b, window->n);
        pos += window->n;
    }
    while (pos < VJO_PLACE_SAVE_BYTES) {
        memcpy(code + off + pos, w->b, w->n);
        pos += w->n;
    }
}

static VjoLunaSite luna_site(uint32_t off, int thumb, uint8_t reg, uint8_t ported, uint16_t version)
{
    VjoLunaSite s;
    memset(&s, 0, sizeof(s));
    s.addr = VJO_LUNA_SEG0 + off + (thumb ? 1u : 0u);
    s.reg = reg;
    s.encoding = VJO_HOOK_ENC_CP932;
    s.ported = ported;
    s.version = version;
    return s;
}

static VjoPlaceVerified verified_entry(uint32_t off, uint32_t nid, uint8_t reg, int32_t padding)
{
    VjoPlaceVerified v;
    memset(&v, 0, sizeof(v));
    v.title = TITLE;
    v.module_nid = nid;
    v.off = off;
    v.thumb = 1;
    v.reg = reg;
    v.encoding = VJO_HOOK_ENC_CP932;
    v.sig_len = 8;
    v.padding = padding;
    memcpy(v.sig, code + off, 8);
    return v;
}

#if VJO_DEBUG
/* The outcome of each site in hooks.log, in order: hooked, skipped, or the refusal's reason (a site's line,
 * then the lines its outcome may follow: taiHEN's code before a refusal). */
static void log_outcomes(char *out, size_t cap)
{
    const char *p = logbuf;
    size_t o = 0;
    int open = 0;
    out[0] = 0;
    while (p && *p) {
        const char *eol = strchr(p, '\n'), *r;
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[200], what[16] = "";
        snprintf(line, sizeof(line), "%.*s", (int)(len < 199 ? len : 199), p);
        if (!strncmp(line, "verified ", 9) || !strncmp(line, "luna ", 5)) {
            if (open) o += (size_t)snprintf(out + o, o < cap ? cap - o : 0, "%s?", o ? "," : "");
            open = 1;
        }
        if (strstr(line, " hooked, stream")) snprintf(what, sizeof(what), "hooked");
        else if (strstr(line, " skipped:")) snprintf(what, sizeof(what), "skipped");
        else if ((r = strstr(line, " refused: ")) != NULL) sscanf(r + 10, "%15s", what);
        if (open && what[0]) {
            o += (size_t)snprintf(out + o, o < cap ? cap - o : 0, "%s%s", o ? "," : "", what);
            open = 0;
        }
        p = eol ? eol + 1 : NULL;
    }
}

#endif

/* Rubi's verified entries first, then Luna's sites in the table's order; a Luna site is refused as
 * unported, then for another version, then out of the segment (before any byte of it is read), then
 * not an entry, then by the window; an address already hooked is skipped (one stream per address). */
static void test_install_order(void)
{
    VjoPlaceVerified v[2];
    VjoLunaSite l[13];
    VjoStreamSlot zero;
    uint32_t mode, inst, refused;
    char outcomes[256];
    reset();
    put_site(0x100, 1, NULL);
    put_site(0x200, 1, NULL);
    put_site(0x300, 0, NULL);
    put_site(0x380, 1, NULL);
    put_site(0x400, 1, form(1, 0, "ldr r0, [pc, #4]"));
    put_site(0x480, 1, NULL);
    put_site(0x500, 1, NULL);
    v[0] = verified_entry(0x100, mod.nid, 6, 0);
    v[1] = verified_entry(0x200, mod.nid + 1u, 6, 0); /* another module's NID: its signature differs */
    l[0] = luna_site(0x200, 1, 0, 1, 0x0101);
    l[1] = luna_site(CODE_SIZE + 0x10, 1, 0, 0, 0x0102);    /* unported (and another version's, out of range) */
    l[2] = luna_site(CODE_SIZE + 0x10, 1, 0, 1, 0x0102);    /* another version (and out of range) */
    l[3] = luna_site(CODE_SIZE + 0x10, 1, 0, 1, 0x0101);    /* in the guard page past the segment */
    l[4] = luna_site(0, 1, 0, 1, 0x0101);
    l[4].addr = VJO_LUNA_SEG0 - 0x10u;                      /* below segment 0 */
    l[5] = luna_site(0x280, 1, 0, 1, 0x0101);               /* zeros: no entry */
    l[6] = luna_site(0x300, 0, 1, 1, 0x0101);               /* ARM */
    l[7] = luna_site(0x100, 1, 0, 1, 0x0101);               /* the verified entry's address */
    l[8] = luna_site(0x380, 1, 0, 1, 0);                    /* unnamed, beside named ones that match */
    l[9] = luna_site(0x400, 1, 0, 1, 0x0101);               /* a PC-relative load in the window */
    l[10] = luna_site(0x480, 1, 0, 0, 0x0101);              /* unported, at an entry the check accepts */
    l[11] = luna_site(0x500, 1, 0, 1, 0x0110);              /* another version's, at an accepted entry */
    l[12] = luna_site(CODE_SIZE, 1, 0, 1, 0x0101);          /* exactly at the segment's end */
    t_ver = v;
    t_nver = 2;
    t_luna = l;
    t_nluna = 13;
    set_sfo("01.01");
    vjo_hooks_start(TITLE, VJO_TEXT_SOURCE_AUTO, 0);

    TEST_ASSERT(n_offs == 3);
    TEST_CHECK(offs[0].offset == 0x100 && offs[0].thumb == 1 && offs[0].source == (const void *)observers[0]);
    TEST_CHECK(offs[1].offset == 0x200 && offs[1].thumb == 1 && offs[1].source == (const void *)observers[1]);
    TEST_CHECK(offs[2].offset == 0x300 && offs[2].thumb == 0 && offs[2].source == (const void *)observers[2]);
    for (unsigned i = 0; i < 3; i++) TEST_CHECK(offs[i].modid == mod.modid && offs[i].segidx == 0);
    TEST_CHECK(offset_calls == 3);
    TEST_CHECK(site_info[0].ver == &v[0] && site_info[0].luna == NULL && slots[0].site == NULL);
    TEST_CHECK(site_info[1].luna == &l[0] && slots[1].site == &l[0] && site_info[2].luna == &l[6]);
    TEST_CHECK(slots[0].key == 0x101 && slots[1].key == 0x201 && slots[2].key == 0x301 && slots[3].key == 0);
    TEST_CHECK(site_info[1].addr == a32(code) + 0x200);
    vjo_hooks_counts(&mode, &inst, &refused);
    TEST_CHECK(mode == VJO_TEXT_SOURCE_AUTO);
    TEST_CHECK(inst == (3u << 16 | CALL_IMPORTS));
    TEST_CHECK_(refused == 11, "refused %u", (unsigned)refused);
    /* APP_VER read once, into the CALL slots' storage, zeroed after */
    TEST_CHECK(sfo_opens == 1);
    memset(&zero, 0, sizeof(zero));
    zero.kind = VJO_HOOK_KIND_CALL;
    for (unsigned i = VJO_HOOK_SITE_STREAMS; i < VJO_HOOK_STREAMS; i++)
        TEST_CHECK_(!memcmp(&slots[i], &zero, sizeof(zero)), "CALL slot %u clean", i);
#if VJO_DEBUG
    log_outcomes(outcomes, sizeof(outcomes));
    TEST_CHECK_(!strcmp(outcomes, "hooked,signature,hooked,unported,version,range,range,entry,hooked,skipped,version,"
                                  "window,unported,version,range"),
                "outcomes %s", outcomes);
    TEST_CHECK(strstr(logbuf, "APP_VER 0101; 2 verified, 13 Luna sites") != NULL);
    TEST_CHECK(strstr(logbuf, "sites hooked 3, refused 11; CALL imports hooked 8 of 8") != NULL);
    TEST_CHECK(log_opens == 1 && closes == 2 && log_flags == (SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC));
#else
    (void)outcomes;
    TEST_CHECK(log_opens == 0 && closes == 1);
#endif
}

/* 8 site slots; a duplicate address is one stream; a site taiHEN refuses leaves its slot to the next. */
static void test_site_cap(void)
{
    VjoLunaSite l[11];
    uint32_t inst, refused;
    reset();
    for (unsigned k = 0; k < 11; k++) {
        put_site(0x100 + 0x40 * k, 1, NULL);
        l[k] = luna_site(0x100 + 0x40 * k, 1, 0, 1, 0);
    }
    l[3] = l[1];                 /* the same address again */
    fail_offset = 0x100 + 0x40 * 2; /* taiHEN refuses this one */
    t_luna = l;
    t_nluna = 11;
    vjo_hooks_start(TITLE, VJO_TEXT_SOURCE_AUTO, 0);
    TEST_CHECK(sfo_opens == 1); /* no param.sfo: APP_VER 0, unnamed sites only */
    TEST_ASSERT(n_offs == 8);
    TEST_CHECK(offset_calls == 9);
    TEST_CHECK(offs[2].offset == 0x100 + 0x40 * 4 && site_info[2].luna == &l[4] && slots[2].key == offs[2].offset + 1);
    for (unsigned i = 0; i < 8; i++) {
        TEST_CHECK(offs[i].source == (const void *)observers[i]);
        TEST_CHECK(slots[i].key == offs[i].offset + 1 && site_uid[i] == offs[i].uid);
    }
    TEST_CHECK(offs[7].offset == 0x100 + 0x40 * 9);
    vjo_hooks_counts(NULL, &inst, &refused);
    TEST_CHECK(inst >> 16 == 8);
    TEST_CHECK_(refused == 2, "refused %u (tai, full)", (unsigned)refused);
#if VJO_DEBUG
    {
        char outcomes[256];
        log_outcomes(outcomes, sizeof(outcomes));
        TEST_CHECK_(!strcmp(outcomes, "hooked,hooked,tai,skipped,hooked,hooked,hooked,hooked,hooked,hooked,full"),
                    "outcomes %s", outcomes);
    }
#endif
    /* the one site taiHEN refuses leaves its slot as it was */
    reset();
    put_site(0x100, 1, NULL);
    l[0] = luna_site(0x100, 1, 0, 1, 0);
    t_luna = l;
    t_nluna = 1;
    fail_offset = 0x100;
    vjo_hooks_start(TITLE, VJO_TEXT_SOURCE_AUTO, 0);
    vjo_hooks_counts(NULL, &inst, &refused);
    TEST_CHECK(n_offs == 0 && offset_calls == 1 && n_sites == 0 && inst >> 16 == 0 && refused == 1);
    TEST_CHECK(slots[0].key == 0 && slots[0].site == NULL && site_uid[0] < 0);
}

/* The text source word: its mode, NO_SITES (no table site, the CALL source as the mode says), and the words
 * that are OCR (an unknown mode or flag). */
static void test_word(void)
{
    static const struct {
        const char *title;
        unsigned word;
        int psp;
        uint32_t mode, imports, sites;
    } cases[] = {
        {TITLE, VJO_TEXT_SOURCE_HOOKS, 0, VJO_TEXT_SOURCE_HOOKS, 8, 2},
        {TITLE, VJO_TEXT_SOURCE_AUTO, 0, VJO_TEXT_SOURCE_AUTO, 8, 2},
        {TITLE, VJO_TEXT_SOURCE_HOOKS | VJO_TEXT_SOURCE_NO_SITES, 0, VJO_TEXT_SOURCE_HOOKS | VJO_TEXT_SOURCE_NO_SITES, 8, 0},
        {TITLE, VJO_TEXT_SOURCE_AUTO | VJO_TEXT_SOURCE_NO_SITES, 0, VJO_TEXT_SOURCE_AUTO | VJO_TEXT_SOURCE_NO_SITES, 8, 0},
        {UNLISTED, VJO_TEXT_SOURCE_AUTO | VJO_TEXT_SOURCE_NO_SITES, 0, VJO_TEXT_SOURCE_AUTO | VJO_TEXT_SOURCE_NO_SITES, 0, 0},
        {UNLISTED, VJO_TEXT_SOURCE_AUTO, 0, VJO_TEXT_SOURCE_AUTO, 0, 0},
        {UNLISTED, VJO_TEXT_SOURCE_HOOKS, 0, VJO_TEXT_SOURCE_HOOKS, 8, 0},
        {TITLE, VJO_TEXT_SOURCE_HOOKS, 1, VJO_TEXT_SOURCE_HOOKS, 0, 0},
        {"", VJO_TEXT_SOURCE_HOOKS, 0, VJO_TEXT_SOURCE_HOOKS, 0, 0},
        {NULL, VJO_TEXT_SOURCE_HOOKS, 0, VJO_TEXT_SOURCE_HOOKS, 0, 0},
        {TITLE, VJO_TEXT_SOURCE_HOOKS | 0x200u, 0, VJO_TEXT_SOURCE_OCR, 0, 0},
        {TITLE, VJO_TEXT_SOURCE_HOOKS | 0x80000000u, 0, VJO_TEXT_SOURCE_OCR, 0, 0},
        {TITLE, VJO_TEXT_SOURCE_COUNT, 0, VJO_TEXT_SOURCE_OCR, 0, 0},
        {TITLE, VJO_TEXT_SOURCE_OCR | VJO_TEXT_SOURCE_NO_SITES, 0, VJO_TEXT_SOURCE_OCR, 0, 0},
        {TITLE, VJO_TEXT_SOURCE_OCR, 0, VJO_TEXT_SOURCE_OCR, 0, 0},
    };
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        VjoPlaceVerified v;
        VjoLunaSite l;
        uint32_t mode = 9, inst = 9;
        reset();
        put_site(0x100, 1, NULL);
        put_site(0x200, 1, NULL);
        v = verified_entry(0x100, mod.nid, 6, 0);
        l = luna_site(0x200, 1, 0, 1, 0);
        t_ver = &v;
        t_nver = 1;
        t_luna = &l;
        t_nluna = 1;
        vjo_hooks_start(cases[c].title, cases[c].word, cases[c].psp);
        vjo_hooks_counts(&mode, &inst, NULL);
        TEST_CHECK_(mode == cases[c].mode, "case %u: mode %u", c, (unsigned)mode);
        TEST_CHECK_(n_imp == cases[c].imports && n_offs == cases[c].sites, "case %u: %u imports, %u sites", c, n_imp,
                    n_offs);
        TEST_CHECK_(inst == (cases[c].sites << 16 | cases[c].imports), "case %u: installed %08X", c, (unsigned)inst);
        TEST_CHECK_(sfo_opens == (cases[c].sites ? 1u : 0u), "case %u: param.sfo read only for Luna's sites", c);
        TEST_CHECK_(live == (cases[c].imports ? 1u : 0u), "case %u: live", c);
#if VJO_DEBUG
        TEST_CHECK_(log_opens == (cases[c].mode != VJO_TEXT_SOURCE_OCR), "case %u: hooks.log", c);
        if (cases[c].mode & VJO_TEXT_SOURCE_NO_SITES) {
            TEST_CHECK_(strstr(logbuf, ", sites off\n") != NULL, "case %u: the first line says sites off", c);
            if (cases[c].imports)
                TEST_CHECK_(strstr(logbuf, "sites off (hook_sites = off): no table site\n") &&
                                strstr(logbuf, "sites off; CALL imports hooked 8 of 8\n"),
                            "case %u: %s", c, logbuf);
        } else {
            TEST_CHECK_(strstr(logbuf, "sites off") == NULL, "case %u", c);
        }
#endif
    }
}

/* auto: a title with a hook entry gets hooks: Rubi's verified entry alone, or Luna's sites alone. */
static void test_auto_listed(void)
{
    VjoPlaceVerified v;
    VjoLunaSite l;
    reset();
    put_site(0x100, 1, NULL);
    v = verified_entry(0x100, mod.nid, 6, 0);
    t_ver = &v;
    t_nver = 1;
    vjo_hooks_start(TITLE, VJO_TEXT_SOURCE_AUTO, 0);
    TEST_CHECK(n_imp == 8 && n_offs == 1 && sfo_opens == 0);
    reset();
    put_site(0x200, 1, NULL);
    l = luna_site(0x200, 1, 0, 1, 0);
    t_luna = &l;
    t_nluna = 1;
    vjo_hooks_start(TITLE, VJO_TEXT_SOURCE_AUTO, 0);
    TEST_CHECK(n_imp == 8 && n_offs == 1 && sfo_opens == 1);
    /* a title whose every Luna site is unported has no hook entry (audit-hooks-text N2): nothing in auto, its
     * sites refused (unported) in hooks */
    reset();
    put_site(0x200, 1, NULL);
    l = luna_site(0x200, 1, 0, 0, 0);
    t_luna = &l;
    t_nluna = 1;
    vjo_hooks_start(TITLE, VJO_TEXT_SOURCE_AUTO, 0);
    TEST_CHECK(n_imp == 0 && n_offs == 0 && !live);
    reset();
    put_site(0x200, 1, NULL);
    l = luna_site(0x200, 1, 0, 0, 0);
    t_luna = &l;
    t_nluna = 1;
    vjo_hooks_start(TITLE, VJO_TEXT_SOURCE_HOOKS, 0);
    TEST_CHECK(n_imp == 8 && n_offs == 0 && n_refused == 1);
    /* the real tables: CLANNAD (Rubi's entry), Luna titles with a ported site; the 7 without one, and an
     * unlisted title, get nothing */
    {
        static const char *const listed[] = {"PCSG00415", "PCSG00917", "PCSG00543"};
        static const char *const none[] = {"PCSB00985", "PCSG00291", "PCSG00451", "PCSG00595",
                                           "PCSG00663", "PCSG01081", "PCSG01152", UNLISTED};
        for (unsigned i = 0; i < 3; i++) {
            reset();
            vjo_hooks_start(listed[i], VJO_TEXT_SOURCE_AUTO, 0);
            TEST_CHECK_(n_imp == 8 && live, "%s: hooked in auto", listed[i]);
        }
        for (unsigned i = 0; i < 8; i++) {
            reset();
            vjo_hooks_start(none[i], VJO_TEXT_SOURCE_AUTO, 0);
            TEST_CHECK_(n_imp == 0 && n_offs == 0 && !live && sfo_opens == 0, "%s: nothing in auto", none[i]);
        }
    }
}

/* The main module: taiHEN's TAI_MAIN_MODULE whose path ends in eboot.bin, with a segment 0; and the imports
 * this firmware may not resolve. */
static void test_main_module(void)
{
    static const char *const bad_paths[] = {"app0:xeboot.bin", "eboot.bin", "app0:eboot.bin2", "app0:sce_module/x.suprx",
                                            "app0:/eboot.bix", "app0:/aboot.bin"};
    static const char *const good_paths[] = {"app0:eboot.bin", "ux0:app/PCSG00415/eboot.bin"};
    for (unsigned k = 0; k < sizeof(bad_paths) / sizeof(bad_paths[0]); k++) {
        reset();
        snprintf(mod.path, sizeof(mod.path), "%s", bad_paths[k]);
        vjo_hooks_start(UNLISTED, VJO_TEXT_SOURCE_HOOKS, 0);
        TEST_CHECK_(n_imp == 0 && !live, "%s: nothing", bad_paths[k]);
    }
    for (unsigned k = 0; k < 2; k++) {
        reset();
        snprintf(mod.path, sizeof(mod.path), "%s", good_paths[k]);
        vjo_hooks_start(UNLISTED, VJO_TEXT_SOURCE_HOOKS, 0);
        TEST_CHECK_(n_imp == 8 && live, "%s: hooked", good_paths[k]);
    }
    reset();
    memset(mod.path, 'a', sizeof(mod.path)); /* no NUL in the path's 256 bytes, though it ends in /eboot.bin */
    memcpy(mod.path + sizeof(mod.path) - 10, "/eboot.bin", 10);
    vjo_hooks_start(UNLISTED, VJO_TEXT_SOURCE_HOOKS, 0);
    TEST_CHECK(n_imp == 0);
    reset();
    mod.seg0 = NULL;
    vjo_hooks_start(UNLISTED, VJO_TEXT_SOURCE_HOOKS, 0);
    TEST_CHECK(n_imp == 0);
    reset();
    mod.seg0_size = 0;
    vjo_hooks_start(UNLISTED, VJO_TEXT_SOURCE_HOOKS, 0);
    TEST_CHECK(n_imp == 0);
    reset();
    mod.tai_rc = (int)0x90010002u;
    vjo_hooks_start(UNLISTED, VJO_TEXT_SOURCE_HOOKS, 0);
    TEST_CHECK(n_imp == 0);
    reset();
    mod.mi_rc = (int)0x80020001u;
    vjo_hooks_start(UNLISTED, VJO_TEXT_SOURCE_HOOKS, 0);
    TEST_CHECK(n_imp == 0);
    reset();
    MISS(taiGetModuleInfo);
    vjo_hooks_start(UNLISTED, VJO_TEXT_SOURCE_HOOKS, 0);
    TEST_CHECK(n_imp == 0 && !live);
    reset();
    MISS(sceKernelGetModuleInfo);
    vjo_hooks_start(UNLISTED, VJO_TEXT_SOURCE_HOOKS, 0);
    TEST_CHECK(n_imp == 0 && !live);
    /* no import hook API: no CALL source; no offset hook API: every site refused (tai) */
    {
        VjoLunaSite l;
        uint32_t inst, refused;
        reset();
        put_site(0x200, 1, NULL);
        l = luna_site(0x200, 1, 0, 1, 0);
        t_luna = &l;
        t_nluna = 1;
        MISS(taiHookFunctionImportForUser);
        MISS(taiHookFunctionOffsetForUser);
        vjo_hooks_start(TITLE, VJO_TEXT_SOURCE_AUTO, 0);
        vjo_hooks_counts(NULL, &inst, &refused);
        TEST_CHECK(n_imp == 0 && n_offs == 0 && offset_calls == 0 && inst == 0 && refused == 1);
        TEST_CHECK(slots[0].key == 0);
    }
    /* imports the game does not have: skipped, not an error */
    reset();
    not_imported[n_not_imported++] = 0x85B924B7u; /* strcpy */
    not_imported[n_not_imported++] = 0xAC595E68u; /* sceClibStrnlen */
    vjo_hooks_start(UNLISTED, VJO_TEXT_SOURCE_HOOKS, 0);
    TEST_CHECK(n_imp == 6 && n_calls == 6 && call_uid[CALL_STRCPY] < 0 && call_uid[CALL_CLIB_STRNLEN] < 0);
#if VJO_DEBUG
    TEST_CHECK(strstr(logbuf, "import BE43BB07/85B924B7: not hooked\n") != NULL);
    TEST_CHECK(strstr(logbuf, "CALL imports hooked 6 of 8\n") != NULL);
#endif
    TEST_CHECK(vjo_hooks_stop() == 0 && n_released == 6);
}

/* A site's hit, through the observer's C entry: the continuation (taiHEN's next hook, else the moved
 * instructions), the site's address in frame[15], its text read through read_game into its stream. */
static void test_site_hits(void)
{
    VjoPlaceVerified v;
    VjoLunaSite l[2];
    uint32_t frame[16];
    struct _tai_hook_user next;
    VjoHookSnap s;
    char text[128];
    uint32_t seq;
    reset();
    put_site(0x100, 1, NULL);
    put_site(0x200, 1, NULL);
    put_site(0x300, 0, NULL);
    v = verified_entry(0x100, mod.nid, 6, 2);
    l[0] = luna_site(0x200, 1, 0, 1, 0);
    l[1] = luna_site(0x300, 0, 1, 1, 0);
    t_ver = &v;
    t_nver = 1;
    t_luna = l;
    t_nluna = 2;
    add_block(a32(data), DATA_SIZE, SCE_KERNEL_MEMORY_ACCESS_R | SCE_KERNEL_MEMORY_ACCESS_W);
    vjo_hooks_start(TITLE, VJO_TEXT_SOURCE_AUTO, 0);
    TEST_ASSERT(n_sites == 3 && mem_info_ok);

    memcpy(data + 0x42, L_TODAY, 23);
    memset(frame, 0, sizeof(frame));
    frame[6] = a32(data) + 0x40; /* the verified entry's register; its padding is 2 */
    TEST_CHECK(vjo_hooks_observe(0, frame) == (uintptr_t)CONT(0));
    TEST_CHECK(frame[15] == a32(code) + 0x100);
    TEST_ASSERT(vjo_hooks_snap(1, &s));
    TEST_CHECK(s.id == 1 && s.kind == VJO_HOOK_KIND_SITE && s.key == 0x100 && s.site == NULL);
    TEST_CHECK(s.encoding == VJO_HOOK_ENC_CP932 && s.n == 22 && !memcmp(s.raw, L_TODAY, 22) && s.hits == 1);
    TEST_CHECK(vjo_hooks_text(&s, text, sizeof(text)) > 0 && !strcmp(text, "今日はいい天気ですね。"));
    seq = s.seq;
    TEST_CHECK(in_flight == 0);
    /* a slot a game thread holds: no snapshot, and its lock stays the thread's */
    slots[0].lock = 1;
    TEST_CHECK(!vjo_hooks_snap(1, &s) && slots[0].lock == 1);
    slots[0].lock = 0;

    memcpy(data + 0x80, L_HELLO, 21);
    frame[0] = a32(data) + 0x80;
    TEST_CHECK(vjo_hooks_observe(1, frame) == (uintptr_t)CONT(1));
    TEST_ASSERT(vjo_hooks_snap(2, &s));
    TEST_CHECK(s.key == 0x200 && s.site == &l[0] && s.n == 20 && !memcmp(s.raw, L_HELLO, 20));
    TEST_CHECK(vjo_hooks_text(&s, text, sizeof(text)) > 0 && !strcmp(text, "こんにちは、朋也くん"));
    /* a Luna site's text goes through its filter (DEL "^" here); a CALL caller's or Rubi's entry's does not */
    {
        const VjoLunaSite *real;
        unsigned nr = vjo_luna_sites("PCSG00420", &real), k = 0;
        while (k < nr && real[k].addr != 0x8004630Au) k++;
        TEST_ASSERT_(k < nr, "PCSG00420's site 0x8004630A (its filter deletes ^)");
        l[0].filter = real[k].filter;
    }
    memcpy(data + 0x80, "\x82\xA0^\x82\xA2^", 7);
    TEST_CHECK(vjo_hooks_observe(1, frame) == (uintptr_t)CONT(1));
    TEST_ASSERT(vjo_hooks_snap(2, &s));
    TEST_CHECK(s.n == 6 && vjo_hooks_text(&s, text, sizeof(text)) == 6 && !strcmp(text, "あい"));
    s.site = NULL;
    TEST_CHECK(vjo_hooks_text(&s, text, sizeof(text)) == 8 && !strcmp(text, "あ^い^"));
    l[0].filter = VJO_LUNA_NONE;
    memcpy(data + 0x80, L_HELLO, 21);
    TEST_CHECK(vjo_hooks_observe(1, frame) == (uintptr_t)CONT(1));
    TEST_CHECK(vjo_hooks_snap(2, &s) && s.n == 20);
    seq = s.seq;
    /* the slot number is masked to the 8 observers: 9 is observer 1; the same text is a hit, not a new seq */
    TEST_CHECK(vjo_hooks_observe(9, frame) == (uintptr_t)CONT(1));
    TEST_CHECK(vjo_hooks_snap(2, &s) && s.hits == 4 && s.seq == seq);

    /* taiHEN's next hook at the same site runs after ours */
    next.next = 0;
    next.func = (void *)(uintptr_t)0xBEEF0000u;
    next.old = NULL;
    offs[2].user.next = (uintptr_t)&next;
    frame[1] = a32(data) + 0x80;
    TEST_CHECK(vjo_hooks_observe(2, frame) == (uintptr_t)0xBEEF0000u);
    TEST_CHECK(vjo_hooks_snap(3, &s) && s.key == 0x300 && s.site == &l[1]);

    /* one capture at a time: a hit while another holds the scratch is dropped, counted */
    memcpy(data + 0x42, L_TOMORROW, 29);
    scratch_lock = 1;
    TEST_CHECK(vjo_hooks_observe(0, frame) == (uintptr_t)CONT(0));
    TEST_CHECK(stream_clock.dropped == 1 && stream_clock.call_dropped == 0);
    {
        uint32_t cd = 9, cr = 9;
        vjo_hooks_call_stats(&cd, &cr);
        TEST_CHECK(cd == 0 && cr == 0);
    }
    TEST_CHECK(vjo_hooks_snap(1, &s) && s.n == 22 && s.hits == 1);
    scratch_lock = 0;
    /* an unreadable address: no text */
    frame[6] = 0x10;
    vjo_hooks_observe(0, frame);
    TEST_CHECK(vjo_hooks_snap(1, &s) && s.n == 22 && s.hits == 1);
    frame[6] = a32(data) + 0x40;
    vjo_hooks_observe(0, frame);
    TEST_CHECK(vjo_hooks_snap(1, &s) && s.n == 28 && s.hits == 2);
    /* after vjo_hooks_stop's live = 0, the continuation still, no hit */
    live = 0;
    memcpy(data + 0x42, L_HELLO, 21);
    TEST_CHECK(vjo_hooks_observe(0, frame) == (uintptr_t)CONT(0));
    TEST_CHECK(vjo_hooks_snap(1, &s) && s.n == 28 && in_flight == 0);
    /* a snapshot of no stream, or of one with no text */
    TEST_CHECK(!vjo_hooks_snap(0, &s) && !vjo_hooks_snap(VJO_HOOK_STREAMS + 1, &s) && !vjo_hooks_snap(4, &s));
    TEST_CHECK(!vjo_hooks_snap(1, NULL));
}

/* Every CALL slot taken: a new caller takes the least recently written one that is not kept, under its
 * try-lock (busy: dropped, never waiting); its key becomes the new caller's (the snapshot's key changes);
 * vjo_hooks_keep keeps an agreed or pinned stream's slot; vjo_hooks_call_stats counts both. */
static void test_replacement_keep(void)
{
    uint8_t t[16][22];
    VjoHookSnap s;
    uint32_t dropped = 9, replaced = 9, seq_max = 0;
    const uint32_t K = 0x1000u;
    reset();
    seg_base = a32(code);
    live = 1;
    for (unsigned i = 0; i < 16; i++) {
        memcpy(t[i], L_HELLO "\x82", 21);
        t[i][21] = (uint8_t)(0xA0 + i); /* another hiragana each */
    }
    for (unsigned i = 0; i < VJO_HOOK_CALL_STREAMS; i++) call_hit(t[i], 22, 1, seg_base + K + 0x10 * i);
    for (unsigned i = 0; i < VJO_HOOK_CALL_STREAMS; i++) {
        TEST_ASSERT(vjo_hooks_snap(VJO_HOOK_SITE_STREAMS + 1 + i, &s));
        TEST_CHECK(s.key == K + 0x10 * i && !memcmp(s.raw, t[i], 22));
        if (s.seq > seq_max) seq_max = s.seq;
    }
    vjo_hooks_call_stats(&dropped, &replaced);
    TEST_CHECK(dropped == 0 && replaced == 0);
    vjo_hooks_call_stats(NULL, NULL);

    /* a 13th caller: the oldest, caller 0's slot (stream 9), with a new key and seq */
    call_hit(t[12], 22, 1, seg_base + K + 0x10 * 12);
    TEST_ASSERT(vjo_hooks_snap(9, &s));
    TEST_CHECK(s.key == K + 0x10 * 12 && !memcmp(s.raw, t[12], 22) && s.hits == 1 && s.seq > seq_max);
    vjo_hooks_call_stats(&dropped, &replaced);
    TEST_CHECK(dropped == 0 && replaced == 1);
    /* caller 0 again: it has no slot now, so it takes the oldest (caller 1's, stream 10) */
    /* kept: caller 1's stream 10 agreed with OCR; the next caller takes caller 2's (stream 11) */
    vjo_hooks_keep(10, 0);
    call_hit(t[13], 22, 1, seg_base + K + 0x10 * 13);
    TEST_CHECK(vjo_hooks_snap(10, &s) && s.key == K + 0x10 && !memcmp(s.raw, t[1], 22));
    TEST_CHECK(vjo_hooks_snap(11, &s) && s.key == K + 0x10 * 13 && !memcmp(s.raw, t[13], 22));
    /* a kept slot's own caller still writes it */
    call_hit(t[14], 22, 1, seg_base + K + 0x10);
    TEST_CHECK(vjo_hooks_snap(10, &s) && s.key == K + 0x10 && !memcmp(s.raw, t[14], 22));
    vjo_hooks_call_stats(&dropped, &replaced);
    TEST_CHECK(dropped == 0 && replaced == 2);

    /* the oldest one not kept (caller 3's, stream 12) busy: dropped, nothing waits, nothing else changes */
    slots[11].lock = 1;
    call_hit(t[15], 22, 1, seg_base + K + 0x10 * 15);
    slots[11].lock = 0;
    vjo_hooks_call_stats(&dropped, &replaced);
    TEST_CHECK(dropped == 1 && replaced == 2);
    TEST_CHECK(vjo_hooks_snap(12, &s) && s.key == K + 0x10 * 3 && call_stream_with(t[15], 22) == 0);

    /* every slot kept: dropped */
    for (uint32_t id = VJO_HOOK_SITE_STREAMS + 1; id <= VJO_HOOK_STREAMS; id++) vjo_hooks_keep(id, 0);
    call_hit(t[15], 22, 1, seg_base + K + 0x10 * 15);
    vjo_hooks_call_stats(&dropped, &replaced);
    TEST_CHECK(dropped == 2 && replaced == 2 && call_stream_with(t[15], 22) == 0);

    /* a site's slot, or no stream: no effect */
    {
        VjoStreamClock before = stream_clock;
        vjo_hooks_keep(0, 0);
        vjo_hooks_keep(1, 0);
        vjo_hooks_keep(1, slots[0].key - 1u);
        vjo_hooks_keep(VJO_HOOK_SITE_STREAMS, 0);
        vjo_hooks_keep(VJO_HOOK_STREAMS + 1, 0);
        vjo_hooks_keep(0xFFFFFFFFu, 0);
        for (unsigned i = 0; i < VJO_HOOK_SITE_STREAMS; i++) TEST_CHECK(slots[i].kept == 0);
        TEST_CHECK(!memcmp(&before, &stream_clock, sizeof(before)));
    }
}

/* vjo_hooks_keep with the key the job saw: kept only while the slot holds that caller (a slot another caller
 * took during the job is not kept for it), under the slot's lock (busy: not kept now). */
static void test_keep_key(void)
{
    uint8_t a[22], b[22];
    VjoHookSnap s;
    const uint32_t K = 0x2000u;
    reset();
    seg_base = a32(code);
    live = 1;
    memcpy(a, L_TODAY, 22);
    memcpy(b, L_HELLO "\x82\xA0", 22);
    for (unsigned i = 0; i < VJO_HOOK_CALL_STREAMS; i++) call_hit(a, 22, 1, seg_base + K + 0x10 * i);
    TEST_ASSERT(vjo_hooks_snap(9, &s) && s.key == K);
    /* the job saw K under stream 9; another caller took the slot since: not kept */
    call_hit(b, 22, 1, seg_base + K + 0x10 * 12);
    TEST_ASSERT(vjo_hooks_snap(9, &s) && s.key == K + 0x10 * 12);
    vjo_hooks_keep(9, K);
    TEST_CHECK(slots[8].kept == 0);
    /* the key it holds: kept (the next caller takes another slot) */
    vjo_hooks_keep(9, K + 0x10 * 12);
    TEST_CHECK(slots[8].kept == 1);
    /* busy: not kept now, the lock left as it was */
    slots[9].lock = 1;
    vjo_hooks_keep(10, K + 0x10);
    TEST_CHECK(slots[9].kept == 0 && slots[9].lock == 1);
    slots[9].lock = 0;
    vjo_hooks_keep(10, K + 0x10);
    TEST_CHECK(slots[9].kept == 1 && slots[9].lock == 0);
    /* a key the slot never held */
    vjo_hooks_keep(11, 0x7777u);
    TEST_CHECK(slots[10].kept == 0);
}

/* vjo_hooks_stop: live off, every hook released newest first (CALL imports, then the sites), each once; then
 * always 2 ms (audit-hooks N8: a thread in the instructions in_flight does not count has left), whenever a hook
 * was installed; nonzero (module_stop's STOP_FAIL) while a thread is still inside a hook after the wait. */
static void test_stop(void)
{
    VjoPlaceVerified v;
    VjoLunaSite l[2];
    char d[32];
    reset();
    put_site(0x100, 1, NULL);
    put_site(0x200, 1, NULL);
    put_site(0x300, 0, NULL);
    v = verified_entry(0x100, mod.nid, 6, 0);
    l[0] = luna_site(0x200, 1, 0, 1, 0);
    l[1] = luna_site(0x300, 0, 1, 1, 0);
    t_ver = &v;
    t_nver = 1;
    t_luna = l;
    t_nluna = 2;
    vjo_hooks_start(TITLE, VJO_TEXT_SOURCE_HOOKS, 0);
    TEST_ASSERT(n_imp == 8 && n_offs == 3 && live);
    TEST_CHECK(vjo_hooks_stop() == 0);
    TEST_CHECK(!live);
    TEST_CHECK(delays == 1 && delay_us == 2000 && released_at_delay == 11); /* after every release */
    TEST_ASSERT(n_released == 11);
    for (unsigned k = 0; k < 8; k++) TEST_CHECK(released[k] == imp[7 - k].uid);
    for (unsigned k = 0; k < 3; k++) TEST_CHECK(released[8 + k] == offs[2 - k].uid);
    for (unsigned k = 0; k < CALL_IMPORTS; k++) TEST_CHECK(call_uid[k] < 0);
    for (unsigned k = 0; k < VJO_HOOK_SITE_STREAMS; k++) TEST_CHECK(site_uid[k] < 0);
    /* again: nothing left to release */
    TEST_CHECK(vjo_hooks_stop() == 0 && n_released == 11);
    /* a hook that still runs (the game called it before its release) takes no hit */
    ((void *(*)(void *, const void *, size_t))imp[CALL_MEMCPY].func)(d, L_TODAY, 22);
    TEST_CHECK(call_slots_used() == 0 && !memcmp(d, L_TODAY, 22));

    /* a thread inside a hook: it waits, then reports it (the module must stay) */
    in_flight = 1;
    delays = 0;
    TEST_CHECK(vjo_hooks_stop() != 0);
    TEST_CHECK(delays >= 1 && delay_us == 2000);
    leave_on_delay = 1;
    TEST_CHECK(vjo_hooks_stop() == 0);

    /* the CALL source alone: the same wait */
    reset();
    vjo_hooks_start(UNLISTED, VJO_TEXT_SOURCE_HOOKS, 0);
    TEST_CHECK(vjo_hooks_stop() == 0 && n_released == 8 && delays == 1 && released_at_delay == 8);
    /* nothing to release when taiHookRelease did not resolve */
    reset();
    vjo_hooks_start(UNLISTED, VJO_TEXT_SOURCE_HOOKS, 0);
    MISS(taiHookRelease);
    TEST_CHECK(vjo_hooks_stop() == 0 && n_released == 0 && !live);
    /* OCR, or nothing installed: nothing released, no wait */
    reset();
    vjo_hooks_start(UNLISTED, VJO_TEXT_SOURCE_OCR, 0);
    TEST_CHECK(vjo_hooks_stop() == 0 && n_released == 0 && delays == 0);
    reset();
    vjo_hooks_start(UNLISTED, VJO_TEXT_SOURCE_AUTO, 0);
    TEST_CHECK(vjo_hooks_stop() == 0 && n_released == 0 && delays == 0);
}

/* APP_VER: two digits, a dot, two digits ("01.10" -> 0x0110, a leading digit may be missing); anything else 0. */
static void test_app_version(void)
{
    static const struct {
        const char *ver;
        uint16_t want;
    } cases[] = {{"01.01", 0x0101}, {"1.10", 0x0110}, {"02.00", 0x0200}, {"01.1", 0}, {"01.01a", 0},
                 {"a1.00", 0},     {"", 0},          {"100.00", 0},     {"01,01", 0}, {"01.0x", 0},
                 {".10", 0},       {"101.01", 0}};
    VjoStreamSlot zero;
    memset(&zero, 0, sizeof(zero));
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        reset();
        set_sfo(cases[c].ver);
        memset(&slots[VJO_HOOK_SITE_STREAMS], 0xAA, sizeof(VjoStreamSlot) * VJO_HOOK_CALL_STREAMS);
        TEST_CHECK_(app_version() == cases[c].want, "APP_VER \"%s\"", cases[c].ver);
        for (unsigned i = VJO_HOOK_SITE_STREAMS; i < VJO_HOOK_STREAMS; i++)
            TEST_CHECK(!memcmp(&slots[i], &zero, sizeof(zero)));
    }
    reset();
    TEST_CHECK(app_version() == 0 && sfo_opens == 1); /* no param.sfo */
    reset();
    set_sfo("01.01");
    MISS(sceIoOpen);
    TEST_CHECK(app_version() == 0 && sfo_opens == 0);
}

TEST_LIST = {
    {"call_hit: 4..512 bytes, the NUL rules, the pointer-array shape", test_call_hit_bounds},
    {"the CALL hooks: the game's function first, each one's length rule", test_call_hooks},
    {"read_game: the block's bounds, units, a kernel answer that does not hold the address, wrap", test_read_game},
    {"install order: verified first; unported, version, range, entry, window; duplicates", test_install_order},
    {"8 site slots, a duplicate address, taiHEN's refusal", test_site_cap},
    {"the text source word: modes, NO_SITES, OCR for an unknown mode or flag", test_word},
    {"auto: Rubi's entry alone, Luna's alone, no ported site, the real tables", test_auto_listed},
    {"the main module (eboot.bin, segment 0) and unresolved imports", test_main_module},
    {"site hits through the observer's C entry", test_site_hits},
    {"CALL slots: replacement, keep, busy, stats", test_replacement_keep},
    {"vjo_hooks_keep: only while the slot holds the caller the job saw", test_keep_key},
    {"vjo_hooks_stop: newest first, once; STOP_FAIL while a thread is inside", test_stop},
    {"APP_VER of param.sfo", test_app_version},
    {NULL, NULL},
};
