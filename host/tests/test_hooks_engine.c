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
/* The segment 0 a test maps at 0x81000000 (fixed_seg0) is freed after every test, a failed one's too (acutest runs
 * TEST_FINI after an aborted test): with --no-exec the tests share one process (audit-rubi-audit-fix-hooks A-N3). */
static void unmap_fixed_seg0(void);
#define TEST_FINI unmap_fixed_seg0()
#include "acutest.h"

#include <stdint.h>
#include <stdio.h>
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
static unsigned test_luna_sites(const char *title_id, const VjoLunaSite **sites);
#define VJO_IMPORT_H 1
#define VJO_IMPORT_READY(fn) test_ready((const void *)(uintptr_t)(fn))
#ifdef VJO_EXTERNAL_HOOK_PROFILES
static const char *profile_test_title;
static unsigned profile_test_reads;
static int profile_test_missing;
int vjoOcrHookProfile(void *dst, uint32_t cap);
#define vjo_hooks_start profile_engine_start
#endif
#define vjo_place_verified test_place_verified
#define vjo_luna_sites test_luna_sites
#include "../../game_ocr/hooks.c"
#undef vjo_place_verified
#undef vjo_luna_sites
unsigned vjo_place_verified(const char *title_id, const VjoPlaceVerified **out);
unsigned vjo_luna_sites(const char *title_id, const VjoLunaSite **sites);

#ifdef VJO_EXTERNAL_HOOK_PROFILES
#undef vjo_hooks_start
static int profile_file_read(void *ud, void *dst, unsigned bytes, uint32_t off)
{
    FILE *f = ud;
    return fseek(f, off, SEEK_SET) ? -1 : (int)fread(dst, 1, bytes, f);
}
int vjoOcrHookProfile(void *dst, uint32_t cap)
{
    FILE *f;
    long size;
    int n;
    profile_test_reads++;
    if (profile_test_missing) return 0;
    f = fopen(VJO_PROFILE_ARCHIVE_PATH, "rb");
    TEST_ASSERT(f != NULL);
    TEST_ASSERT(!fseek(f, 0, SEEK_END)); size = ftell(f);
    TEST_ASSERT(size > 0 && (uint64_t)size <= UINT32_MAX);
    n = vjo_hook_profile_read(profile_file_read, f, (uint32_t)size, profile_test_title, dst, cap);
    fclose(f);
    return n;
}
static void vjo_hooks_start(const char *title, unsigned mode, int psp)
{
    profile_test_title = title;
    profile_engine_start(title, mode, psp);
}
static void profile_for_farm(const char *title)
{
    profile_test_title = title;
    int n = vjoOcrHookProfile(profile.raw, sizeof(profile.raw));
    TEST_ASSERT(n > 0 && vjo_hook_profile_parse(&profile, title, (unsigned)n));
}
#endif

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
static unsigned test_luna_sites(const char *title_id, const VjoLunaSite **sites)
{
    if (!is_test_title(title_id)) return vjo_luna_sites(title_id, sites);
    if (sites) *sites = t_nluna ? t_luna : NULL;
    return t_nluna;
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

static uint8_t exe_bytes[4096];
static size_t exe_size, exe_pos;
static unsigned exe_opens;
static int exe_error, exe_close_error, exe_missing;

SceUID sceIoOpen(const char *path, int flags, unsigned mode)
{
    (void)mode;
    if (!strcmp(path, mod.path)) {
        exe_opens++;
        exe_pos = 0;
        TEST_CHECK(flags == SCE_O_RDONLY);
        return exe_missing ? -1 : 12;
    }
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
    if (fd == 12) {
        if (exe_error && exe_pos >= 1024) return -1;
        k = exe_size - exe_pos;
        if (k > n) k = n;
        memcpy(buf, exe_bytes + exe_pos, k);
        exe_pos += k;
        return (int)k;
    }
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
    closes++;
    return fd == 12 && exe_close_error ? -1 : 0;
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
#ifdef VJO_EXTERNAL_HOOK_PROFILES
    profile_test_reads = 0; profile_test_missing = 0;
#endif
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
    sc3_pin = NULL;
    sc3_table = NULL;
    sc3_count = 0;
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
    exe_size = exe_pos = exe_opens = 0;
    exe_error = exe_close_error = exe_missing = 0;
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
    /* no table entry: APP_VER decides nothing; the debug build reads it for hooks.log only */
    TEST_CHECK(n_offs == 0 && sfo_opens == (VJO_DEBUG ? 1u : 0u));
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

/* A function entry the site check accepts at seg + off (16 bytes: the patch and taiHEN's saved bytes):
 * Thumb PUSH {r4, lr} then MOV r7, sp; ARM PUSH {r4, lr} then SUB sp, sp, #16 (the assembler's bytes).
 * window: the instruction after the entry instead (a refused one). */
static void put_site_at(uint8_t *seg, uint32_t off, int thumb, const Form *window)
{
    const Form *e = form(thumb, 1, "push {r4, lr}"), *w = form(thumb, 0, thumb ? "mov r7, sp" : "sub sp, sp, #16");
    unsigned pos = 0;
    memcpy(seg + off, e->b, e->n);
    pos = e->n;
    if (window) {
        memcpy(seg + off + pos, window->b, window->n);
        pos += window->n;
    }
    while (pos < VJO_PLACE_SAVE_BYTES) {
        memcpy(seg + off + pos, w->b, w->n);
        pos += w->n;
    }
}
static void put_site(uint32_t off, int thumb, const Form *window) { put_site_at(code, off, thumb, window); }

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
    /* the out-of-range sites below are out of range in both of vjo_luna_offset's readings: the code buffer is not
     * at VJO_LUNA_GUEST_SEG0, so only VJO_LUNA_SEG0's reading applies */
    TEST_ASSERT(a32(code) != VJO_LUNA_GUEST_SEG0);
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
    /* l[7], ported and this version's, is at v[0]'s address: the entry takes its text processing */
    TEST_CHECK(site_info[0].ver == &v[0] && site_info[0].luna == &l[7] && slots[0].site == &l[7]);
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
        {UNLISTED, VJO_TEXT_SOURCE_AUTO | VJO_TEXT_SOURCE_NO_SITES, 0, VJO_TEXT_SOURCE_AUTO | VJO_TEXT_SOURCE_NO_SITES, 8, 0},
        {UNLISTED, VJO_TEXT_SOURCE_AUTO, 0, VJO_TEXT_SOURCE_AUTO, 8, 0},
        {UNLISTED, VJO_TEXT_SOURCE_HOOKS, 0, VJO_TEXT_SOURCE_HOOKS, 8, 0},
        {TITLE, VJO_TEXT_SOURCE_HOOKS, 1, VJO_TEXT_SOURCE_HOOKS, 0, 0},
        {UNLISTED, VJO_TEXT_SOURCE_AUTO, 1, VJO_TEXT_SOURCE_AUTO, 0, 0},
        {"", VJO_TEXT_SOURCE_AUTO, 0, VJO_TEXT_SOURCE_AUTO, 0, 0},
        {NULL, VJO_TEXT_SOURCE_AUTO, 0, VJO_TEXT_SOURCE_AUTO, 0, 0},
        {"", VJO_TEXT_SOURCE_HOOKS, 0, VJO_TEXT_SOURCE_HOOKS, 0, 0},
        {NULL, VJO_TEXT_SOURCE_HOOKS, 0, VJO_TEXT_SOURCE_HOOKS, 0, 0},
        {TITLE, VJO_TEXT_SOURCE_HOOKS | 0x200u, 0, VJO_TEXT_SOURCE_OCR, 0, 0},
        {TITLE, VJO_TEXT_SOURCE_HOOKS | 0x80000000u, 0, VJO_TEXT_SOURCE_OCR, 0, 0},
        {TITLE, VJO_TEXT_SOURCE_COUNT, 0, VJO_TEXT_SOURCE_OCR, 0, 0},
        {TITLE, VJO_TEXT_SOURCE_OCR | VJO_TEXT_SOURCE_NO_SITES, 0, VJO_TEXT_SOURCE_OCR, 0, 0},
        {TITLE, VJO_TEXT_SOURCE_OCR, 0, VJO_TEXT_SOURCE_OCR, 0, 0},
        {UNLISTED, VJO_TEXT_SOURCE_OCR, 0, VJO_TEXT_SOURCE_OCR, 0, 0},
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
        /* APP_VER: for Luna's sites (the normal build), for every native game whose sites are tried (the debug
         * build's hooks.log), never with hook_sites = off */
        TEST_CHECK_(sfo_opens == (VJO_DEBUG ? (cases[c].imports && !(cases[c].mode & VJO_TEXT_SOURCE_NO_SITES) ? 1u : 0u)
                                            : (cases[c].sites ? 1u : 0u)),
                    "case %u: param.sfo read %u", c, sfo_opens);
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

/* Automatic attempts CALL imports even without a table entry. The table-site checks still apply. */
static void test_auto_all_native(void)
{
    VjoPlaceVerified v;
    VjoLunaSite l;
    reset();
    put_site(0x100, 1, NULL);
    v = verified_entry(0x100, mod.nid, 6, 0);
    t_ver = &v;
    t_nver = 1;
    vjo_hooks_start(TITLE, VJO_TEXT_SOURCE_AUTO, 0);
    TEST_CHECK(n_imp == 8 && n_offs == 1 && sfo_opens == (VJO_DEBUG ? 1u : 0u)); /* the entry names no version */
    reset();
    put_site(0x200, 1, NULL);
    l = luna_site(0x200, 1, 0, 1, 0);
    t_luna = &l;
    t_nluna = 1;
    vjo_hooks_start(TITLE, VJO_TEXT_SOURCE_AUTO, 0);
    TEST_CHECK(n_imp == 8 && n_offs == 1 && sfo_opens == 1);
    /* A title whose Luna sites are all unported still gets generic discovery; those sites are refused. */
    reset();
    put_site(0x200, 1, NULL);
    l = luna_site(0x200, 1, 0, 0, 0);
    t_luna = &l;
    t_nluna = 1;
    vjo_hooks_start(TITLE, VJO_TEXT_SOURCE_AUTO, 0);
    TEST_CHECK(n_imp == 8 && n_offs == 0 && n_refused == 1 && live);
    reset();
    put_site(0x200, 1, NULL);
    l = luna_site(0x200, 1, 0, 0, 0);
    t_luna = &l;
    t_nluna = 1;
    vjo_hooks_start(TITLE, VJO_TEXT_SOURCE_HOOKS, 0);
    TEST_CHECK(n_imp == 8 && n_offs == 0 && n_refused == 1);
    /* The real tables: listed and unlisted native Vita games get CALL discovery. Unported sites stay out. */
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
            TEST_CHECK_(n_imp == 8 && n_offs == 0 && live, "%s: generic hooks in auto", none[i]);
            TEST_CHECK_(sfo_opens == (i < 7 || VJO_DEBUG ? 1u : 0u),
                        "%s: APP_VER when there are Luna sites (and for hooks.log in the debug build)", none[i]);
            TEST_CHECK_(i < 7 ? n_refused > 0 : n_refused == 0, "%s: refuse unported sites", none[i]);
        }
    }
    /* Exercise the actual installed string-copy hook in an unlisted game, through decoding and release. */
    {
        char copied[64], text[128];
        VjoHookSnap snap;
        unsigned id;
        reset();
        vjo_hooks_start(UNLISTED, VJO_TEXT_SOURCE_AUTO, 0);
        TEST_ASSERT(n_imp == CALL_IMPORTS);
        TEST_CHECK(((char *(*)(char *, const char *))imp[CALL_STRCPY].func)(copied, L_TODAY) == copied);
        id = call_stream_with(L_TODAY, 22);
        TEST_ASSERT(id && vjo_hooks_snap(id, &snap));
        TEST_CHECK(snap.kind == VJO_HOOK_KIND_CALL && snap.site == NULL);
        TEST_CHECK(vjo_hooks_text(&snap, text, sizeof(text)) == strlen("今日はいい天気ですね。"));
        TEST_CHECK(!strcmp(text, "今日はいい天気ですね。"));
        TEST_CHECK(vjo_hooks_stop() == 0 && n_released == CALL_IMPORTS && !live);
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
        vjo_hooks_start(UNLISTED, VJO_TEXT_SOURCE_AUTO, 0);
        TEST_CHECK_(n_imp == 0 && !live, "%s: nothing", bad_paths[k]);
    }
    for (unsigned k = 0; k < 2; k++) {
        reset();
        snprintf(mod.path, sizeof(mod.path), "%s", good_paths[k]);
        vjo_hooks_start(UNLISTED, VJO_TEXT_SOURCE_AUTO, 0);
        TEST_CHECK_(n_imp == 8 && live, "%s: hooked", good_paths[k]);
    }
    reset();
    memset(mod.path, 'a', sizeof(mod.path)); /* no NUL in the path's 256 bytes, though it ends in /eboot.bin */
    memcpy(mod.path + sizeof(mod.path) - 10, "/eboot.bin", 10);
    vjo_hooks_start(UNLISTED, VJO_TEXT_SOURCE_AUTO, 0);
    TEST_CHECK(n_imp == 0);
    reset();
    mod.seg0 = NULL;
    vjo_hooks_start(UNLISTED, VJO_TEXT_SOURCE_AUTO, 0);
    TEST_CHECK(n_imp == 0);
    reset();
    mod.seg0_size = 0;
    vjo_hooks_start(UNLISTED, VJO_TEXT_SOURCE_AUTO, 0);
    TEST_CHECK(n_imp == 0);
    reset();
    mod.tai_rc = (int)0x90010002u;
    vjo_hooks_start(UNLISTED, VJO_TEXT_SOURCE_AUTO, 0);
    TEST_CHECK(n_imp == 0);
    reset();
    mod.mi_rc = (int)0x80020001u;
    vjo_hooks_start(UNLISTED, VJO_TEXT_SOURCE_AUTO, 0);
    TEST_CHECK(n_imp == 0);
    reset();
    MISS(taiGetModuleInfo);
    vjo_hooks_start(UNLISTED, VJO_TEXT_SOURCE_AUTO, 0);
    TEST_CHECK(n_imp == 0 && !live);
    reset();
    MISS(sceKernelGetModuleInfo);
    vjo_hooks_start(UNLISTED, VJO_TEXT_SOURCE_AUTO, 0);
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
    vjo_hooks_start(UNLISTED, VJO_TEXT_SOURCE_AUTO, 0);
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
    /* pinned (key 0, a diagnostic pick): under the slot's lock as well (#366's audit-b, F3): busy, not now, the lock
     * left as it was; free, kept, whichever caller holds it */
    slots[11].lock = 1;
    vjo_hooks_keep(12, 0);
    TEST_CHECK(slots[11].kept == 0 && slots[11].lock == 1);
    slots[11].lock = 0;
    vjo_hooks_keep(12, 0);
    TEST_CHECK(slots[11].kept == 1 && slots[11].lock == 0);
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
    MISS(taiHookFunctionImportForUser);
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
                 {".10", 0},       {"101.01", 0},    {"00.01", 0x0001}, {"0.01", 0x0001}};
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

/* A hook request (docs/hook-requests.md) needs the player's version, and most requests are for games with no entry:
 * the debug build's hooks.log names APP_VER for every game whose table sites are tried, entries or none, and the
 * hooks are the same in both builds (APP_VER decides nothing without a Luna site or an entry that names it). */
static void test_app_ver_for_requests(void)
{
    static const struct {
        const char *title, *sfo, *line;
        unsigned entry; /* 1: a verified entry that names no version */
    } cases[] = {
        {UNLISTED, "01.02", "main module TestGame NID 12345678 seg0 %08X+4000 APP_VER 0102; 0 verified, 0 Luna sites\n", 0},
        {UNLISTED, NULL, "main module TestGame NID 12345678 seg0 %08X+4000 APP_VER 0000; 0 verified, 0 Luna sites\n", 0},
        {TITLE, "01.00", "main module TestGame NID 12345678 seg0 %08X+4000 APP_VER 0100; 1 verified, 0 Luna sites\n", 1},
    };
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        VjoPlaceVerified v;
        uint32_t mode, inst, refused;
        char want[160];
        reset();
        put_site(0x100, 1, NULL);
        if (cases[c].entry) {
            v = verified_entry(0x100, mod.nid, 6, 0);
            t_ver = &v;
            t_nver = 1;
        }
        if (cases[c].sfo) set_sfo(cases[c].sfo);
        vjo_hooks_start(cases[c].title, VJO_TEXT_SOURCE_AUTO, 0);
        vjo_hooks_counts(&mode, &inst, &refused);
        TEST_CHECK_(mode == VJO_TEXT_SOURCE_AUTO && inst == (cases[c].entry << 16 | CALL_IMPORTS) && refused == 0,
                    "case %u: installed %08X, refused %u", c, (unsigned)inst, (unsigned)refused);
        TEST_CHECK_(n_offs == cases[c].entry && n_imp == CALL_IMPORTS, "case %u: the same hooks in both builds", c);
#if VJO_DEBUG
        snprintf(want, sizeof(want), cases[c].line, (unsigned)a32(code));
        TEST_CHECK_(sfo_opens == 1 && strstr(logbuf, want) != NULL, "case %u: %s", c, logbuf);
#else
        (void)want;
        TEST_CHECK_(sfo_opens == 0 && log_opens == 0, "case %u: the normal build reads no APP_VER for it", c);
#endif
    }
}

static void test_exact_build_identity(void)
{
    static const uint8_t abc_sha[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    uint8_t a[32], b[32], zero[32] = {0};
    reset();
    vjo_hooks_start(UNLISTED, VJO_TEXT_SOURCE_HOOKS, 0);
    TEST_CHECK(!exe_opens); /* never hash at module_start */
    memcpy(exe_bytes, "abc", 3); exe_size = 3;
    TEST_CHECK(vjo_hooks_build_id(a) && !memcmp(a, abc_sha, sizeof(a)));
    memset(exe_bytes, 0x51, sizeof(exe_bytes)); exe_size = sizeof(exe_bytes);
    TEST_CHECK(vjo_hooks_build_id(a) && exe_pos == exe_size);
    exe_bytes[sizeof(exe_bytes) - 1] ^= 1; /* same title/path/NID, changed executable tail */
    TEST_CHECK(vjo_hooks_build_id(b) && memcmp(a, b, sizeof(a)));
    exe_error = 1;
    TEST_CHECK(!vjo_hooks_build_id(a) && !memcmp(a, zero, sizeof(a)));
    exe_error = 0; exe_size = 0;
    TEST_CHECK(!vjo_hooks_build_id(a) && !memcmp(a, zero, sizeof(a)));
    exe_size = 3; exe_close_error = 1;
    TEST_CHECK(!vjo_hooks_build_id(a) && !memcmp(a, zero, sizeof(a)));
    exe_close_error = 0; exe_missing = 1;
    TEST_CHECK(!vjo_hooks_build_id(a) && !memcmp(a, zero, sizeof(a)));
    exe_missing = 0; MISS(sceIoRead);
    unsigned opened = exe_opens;
    TEST_CHECK(!vjo_hooks_build_id(a) && exe_opens == opened);
}

/* A verified entry at a Luna site's address (PCSG01198's, with that title's real filter): one hook, the
 * entry's register read, Luna's filter on the text; the Luna site is skipped (its address is hooked), not
 * refused. A Luna site at another address, unported, or another version's gives the entry nothing. */
static void test_verified_takes_luna_text(void)
{
    /* $[漢字$/かんじ$] in CP932: the filter keeps the base */
    static const uint8_t ruby[] = {'$', '[', 0x8A, 0xBF, 0x8E, 0x9A, '$', '/', 0x82, 0xA9, 0x82, 0xF1,
                                   0x82, 0xB6, '$', ']', 0};
    VjoPlaceVerified v;
    VjoLunaSite l[1];
    const VjoLunaSite *real;
    uint32_t frame[16], inst, refused;
    VjoHookSnap s;
    char text[128];
    TEST_ASSERT(vjo_luna_sites("PCSG01198", &real) == 1 && real[0].filter != VJO_LUNA_NONE);
    reset();
    put_site(0x100, 1, NULL);
    v = verified_entry(0x100, mod.nid, 2, 0);
    l[0] = luna_site(0x100, 1, 2, 1, 0);
    l[0].filter = real[0].filter;
    t_ver = &v;
    t_nver = 1;
    t_luna = l;
    t_nluna = 1;
    add_block(a32(data), DATA_SIZE, SCE_KERNEL_MEMORY_ACCESS_R | SCE_KERNEL_MEMORY_ACCESS_W);
    vjo_hooks_start(TITLE, VJO_TEXT_SOURCE_AUTO, 0);
    TEST_ASSERT(n_sites == 1 && n_offs == 1);
    TEST_CHECK(site_info[0].ver == &v && site_info[0].luna == &l[0] && slots[0].site == &l[0]);
    vjo_hooks_counts(NULL, &inst, &refused);
    TEST_CHECK(inst >> 16 == 1 && refused == 0);

    memcpy(data + 0x40, ruby, sizeof ruby);
    memset(frame, 0, sizeof(frame));
    frame[2] = a32(data) + 0x40;
    TEST_CHECK(vjo_hooks_observe(0, frame) == (uintptr_t)CONT(0));
    TEST_ASSERT(vjo_hooks_snap(1, &s));
    TEST_CHECK(s.site == &l[0] && s.n == sizeof ruby - 1 && !memcmp(s.raw, ruby, sizeof ruby - 1));
    TEST_CHECK(vjo_hooks_text(&s, text, sizeof(text)) > 0 && !strcmp(text, "漢字"));
    TEST_MSG("got [%s]", text);
    s.site = NULL; /* without Luna's entry: the markup stays */
    TEST_CHECK(vjo_hooks_text(&s, text, sizeof(text)) > 0 && strstr(text, "$[漢字$/かんじ$]") != NULL);

    /* no Luna site at the entry's address, or one that may not be tried: the entry's text is unprocessed */
    static const struct { uint32_t off; uint8_t ported; uint16_t version; } others[] = {
        {0x104, 1, 0}, {0x100, 0, 0}, {0x100, 1, 0x0102}};
    for (unsigned k = 0; k < sizeof others / sizeof others[0]; k++) {
        reset();
        put_site(0x100, 1, NULL);
        v = verified_entry(0x100, mod.nid, 2, 0);
        l[0] = luna_site(others[k].off, 1, 2, others[k].ported, others[k].version);
        l[0].filter = real[0].filter;
        t_ver = &v;
        t_nver = 1;
        t_luna = l;
        t_nluna = 1;
        set_sfo("01.00");
        vjo_hooks_start(TITLE, VJO_TEXT_SOURCE_AUTO, 0);
        TEST_CHECK_(n_sites >= 1 && site_info[0].ver == &v && site_info[0].luna == NULL && slots[0].site == NULL,
                    "case %u", k);
    }
}

/* A verified entry with VJO_PLACE_READ_DEREF reads the text through the pointer at its register + padding (one exact
 * 4-byte read; an unreadable pointer or pointee gives no text); its own filter (a Rubi VJO_RF_*) processes
 * the text, with or without a Luna site at its address, and replaces that site's filter. */
static void test_verified_deref_and_filter(void)
{
    /* <漢,かん>字 in CP932: the ruby filter keeps the base */
    static const uint8_t ruby[] = {'<', 0x8A, 0xBF, ',', 0x82, 0xA9, 0x82, 0xF1, '>', 0x8E, 0x9A, 0};
    /* #Name[2]#n in CP932 is ASCII: PCSG00448's filter names it, Luna's drops it */
    static const char name[] = "#Name[2]#n";
    VjoPlaceVerified v;
    VjoLunaSite l[1];
    const VjoLunaSite *real;
    uint32_t frame[16], p;
    VjoHookSnap s;
    char text[128];
    reset();
    put_site(0x100, 1, NULL);
    v = verified_entry(0x100, mod.nid, 4, 8);
    v.read = VJO_PLACE_READ_DEREF;
    v.filter = VJO_RF_ANGLE_RUBY;
    t_ver = &v;
    t_nver = 1;
    add_block(a32(data), DATA_SIZE, SCE_KERNEL_MEMORY_ACCESS_R | SCE_KERNEL_MEMORY_ACCESS_W);
    vjo_hooks_start(TITLE, VJO_TEXT_SOURCE_AUTO, 0);
    TEST_ASSERT(n_sites == 1 && site_info[0].ver == &v && slots[0].site == NULL);

    p = a32(data) + 0x80;
    memcpy(data + 0x48, &p, 4); /* r4 + 8: the pointer (little-endian, as on the Vita) */
    memcpy(data + 0x80, ruby, sizeof ruby);
    memset(frame, 0, sizeof(frame));
    frame[4] = a32(data) + 0x40;
    TEST_CHECK(vjo_hooks_observe(0, frame) == (uintptr_t)CONT(0));
    TEST_ASSERT(vjo_hooks_snap(1, &s));
    TEST_CHECK(s.n == sizeof ruby - 1 && !memcmp(s.raw, ruby, sizeof ruby - 1) && s.hits == 1);
    TEST_CHECK(vjo_hooks_text(&s, text, sizeof(text)) > 0 && !strcmp(text, "漢字"));
    TEST_MSG("got [%s]", text);
    /* without the entry (another stream's key, a CALL snapshot): the markup stays */
    s.key = 0x104;
    TEST_CHECK(vjo_hooks_text(&s, text, sizeof(text)) > 0 && strstr(text, "<漢,かん>字") != NULL);
    s.key = 0x100;
    s.kind = VJO_HOOK_KIND_CALL;
    TEST_CHECK(vjo_hooks_text(&s, text, sizeof(text)) > 0 && strstr(text, "<漢,かん>字") != NULL);
    /* an unreadable pointer, or the pointer itself out of the block: no hit */
    p = 0x10;
    memcpy(data + 0x48, &p, 4);
    vjo_hooks_observe(0, frame);
    TEST_CHECK(vjo_hooks_snap(1, &s) && s.hits == 1);
    frame[4] = a32(data) + DATA_SIZE - 10; /* + 8: two of the pointer's four bytes in the block */
    vjo_hooks_observe(0, frame);
    TEST_CHECK(vjo_hooks_snap(1, &s) && s.hits == 1);
    /* VJO_PLACE_READ_STRING: the register + padding is the text */
    v.read = VJO_PLACE_READ_STRING;
    memcpy(data + 0x48, ruby, sizeof ruby);
    frame[4] = a32(data) + 0x40;
    vjo_hooks_observe(0, frame);
    TEST_CHECK(vjo_hooks_snap(1, &s) && s.hits == 2 && s.n == sizeof ruby - 1);

    /* at a Luna site's address: the entry's own filter replaces that site's (PCSG00448's real one) */
    {
        const VjoLunaSite *all;
        unsigned nr = vjo_luna_sites("PCSG00448", &all), k = 0;
        while (k < nr && all[k].addr != 0x800718F8u) k++;
        TEST_ASSERT_(k < nr, "PCSG00448's site 0x800718F8");
        real = &all[k];
    }
    reset();
    put_site(0x100, 1, NULL);
    v = verified_entry(0x100, mod.nid, 2, 0);
    v.filter = VJO_RF_PCSG00448;
    l[0] = luna_site(0x100, 1, 2, 1, 0);
    l[0].filter = real->filter;
    t_ver = &v;
    t_nver = 1;
    t_luna = l;
    t_nluna = 1;
    add_block(a32(data), DATA_SIZE, SCE_KERNEL_MEMORY_ACCESS_R | SCE_KERNEL_MEMORY_ACCESS_W);
    vjo_hooks_start(TITLE, VJO_TEXT_SOURCE_AUTO, 0);
    TEST_ASSERT(n_sites == 1 && site_info[0].luna == &l[0] && slots[0].site == &l[0]);
    memcpy(data + 0x40, name, sizeof name);
    memcpy(data + 0x40 + sizeof name - 1, L_AI, sizeof L_AI); /* #Name[2]#nあい */
    memset(frame, 0, sizeof(frame));
    frame[2] = a32(data) + 0x40;
    vjo_hooks_observe(0, frame);
    TEST_ASSERT(vjo_hooks_snap(1, &s));
    TEST_CHECK(vjo_hooks_text(&s, text, sizeof(text)) > 0 && !strcmp(text, "ユエ\nあい"));
    TEST_MSG("got [%s]", text);
    v.filter = VJO_LUNA_NONE; /* the site's own filter: the name and #n go */
    TEST_CHECK(vjo_hooks_text(&s, text, sizeof(text)) > 0 && !strcmp(text, "あい"));
    TEST_MSG("got [%s]", text);
    v.filter = VJO_RF_END; /* an unknown filter: no text */
    TEST_CHECK(vjo_hooks_text(&s, text, sizeof(text)) == 0 && !text[0]);
}

/* A cell record's layout as the farm read it (PCSG00380's reader-config.json; PCSG00914's): its count at +0x14,
 * its cells from +0x20. Written here as numbers, not VJO_PLACE_CELLS_*, so a change of those fails. */
#define REC_COUNT 0x14u
#define REC_CELLS 0x20u
/* foaf4's page-build record as the farm read it (PCSG00769's reader-config.json): its count at +0x20, its cells from
 * +0x2E (VJO_PLACE_READ_CELLS_B), as numbers too */
#define BUILD_COUNT 0x20u
#define BUILD_CELLS 0x2Eu

/* A verified entry that names its APP_VER (VjoPlaceVerified.app_ver) is refused in another version, as
 * VJO_PLACE_VERSION before its bytes are read; a param.sfo without a readable APP_VER leaves it to its NID and
 * bytes (a Luna site that names its version is refused then: it has neither); one that names none is tried in
 * any. APP_VER is read for such an entry with no Luna site too, and not at all for a title whose entries name
 * none and that has no Luna site (as before). */
static void test_verified_version(void)
{
    static const struct { uint16_t app_ver; const char *sfo; int hooked; } cases[] = {
        {0x0100, "01.00", 1}, {0x0100, "01.01", 0}, {0x0100, NULL, 1}, {0x0100, "1.0", 1},
        {0x0102, "01.02", 1}, {0x0102, "01.00", 0}, {0, "01.01", 1}, {0, NULL, 1}};
    for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        VjoPlaceVerified v;
        uint32_t inst, refused;
        reset();
        put_site(0x100, 1, NULL);
        v = verified_entry(0x100, mod.nid, 6, 0);
        v.app_ver = cases[k].app_ver;
        t_ver = &v;
        t_nver = 1;
        if (cases[k].sfo) set_sfo(cases[k].sfo);
        vjo_hooks_start(TITLE, VJO_TEXT_SOURCE_AUTO, 0);
        vjo_hooks_counts(NULL, &inst, &refused);
        TEST_CHECK_(sfo_opens == (cases[k].app_ver || VJO_DEBUG ? 1u : 0u),
                    "case %u: APP_VER read once, for the entry's (and hooks.log's in the debug build)", k);
        TEST_CHECK_((inst >> 16) == (unsigned)cases[k].hooked && refused == (unsigned)!cases[k].hooked &&
                        n_offs == (unsigned)cases[k].hooked,
                    "case %u: app_ver %04X, APP_VER %s: hooked %u, refused %u", k, (unsigned)cases[k].app_ver,
                    cases[k].sfo ? cases[k].sfo : "(none)", (unsigned)(inst >> 16), (unsigned)refused);
#if VJO_DEBUG
        TEST_CHECK_(cases[k].hooked || strstr(logbuf, " refused: version") != NULL, "case %u: the log", k);
#endif
    }
}

/* AFEJ00001, the native どうぶつの森e＋ r4 port (#529): its own row through the worker's path, in both suites (the
 * built-in table and the shipped archive; audit-rubi-merge-529 N8). The port's 16 bytes at +0x20F0, with its module NID
 * and APP_VER 00.01, are hooked; another APP_VER or NID is refused, and an unreadable APP_VER leaves the decision to the
 * NID and the bytes. r0's UTF-8 page is read up to its NUL, at most VJO_HOOK_RAW_BYTES and never past its memory block
 * (a guard page follows it), nothing is written to it, and its text gets Rubi's clean-up only (no filter, no Luna site
 * at the address). */
static void test_eplus_row(void)
{
    static const uint8_t sig[16] = {0x10, 0xB5, 0x04, 0x46, 0x24, 0x46, 0x24, 0x46,
                                    0x24, 0x46, 0x24, 0x46, 0x24, 0x46, 0x24, 0x46};
    static const struct {
        const char *sfo;
        uint32_t nid;
        int hooked;
        const char *log; /* the debug build's hooks.log line for the row */
    } cases[] = {{"00.01", 0x7FEE3308u, 1, "verified +20F0 thumb r0 hooked, stream 1"},
                 {NULL, 0x7FEE3308u, 1, "verified +20F0 thumb r0 hooked, stream 1"},
                 {"00.02", 0x7FEE3308u, 0, "verified +20F0 thumb r0 refused: version"},
                 {"00.01", 0x52DEA492u, 0, "verified +20F0 thumb r0 refused: signature"}};
    static const char page[] = "こんにちは！\x01\n今日もいい天気だね。";
    static const char want[] = "こんにちは！\n今日もいい天気だね。";
    char text[1024], cut[3u * 170u + 1u];
    uint32_t frame[16], inst, refused, seq;
    VjoHookSnap s;
    for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        reset();
        memcpy(code + 0x20F0, sig, sizeof sig);
        mod.nid = cases[k].nid;
        if (cases[k].sfo) set_sfo(cases[k].sfo);
        vjo_hooks_start("AFEJ00001", VJO_TEXT_SOURCE_AUTO, 0);
        vjo_hooks_counts(NULL, &inst, &refused);
        /* AFEJ00001 has three rows (r4, r5, r6: one module NID each); the two other builds' rows are refused */
        TEST_CHECK_((inst >> 16) == (unsigned)cases[k].hooked && refused == 3u - (unsigned)cases[k].hooked &&
                        n_offs == (unsigned)cases[k].hooked,
                    "case %u (APP_VER %s, NID %08X): hooked %u, refused %u", k, cases[k].sfo ? cases[k].sfo : "(none)",
                    (unsigned)cases[k].nid, (unsigned)(inst >> 16), (unsigned)refused);
#if VJO_DEBUG
        TEST_CHECK_(strstr(logbuf, cases[k].log) != NULL, "case %u: hooks.log says \"%s\"", k, cases[k].log);
#endif
    }

    reset();
    memcpy(code + 0x20F0, sig, sizeof sig);
    mod.nid = 0x7FEE3308u;
    set_sfo("00.01");
    add_block(a32(data), DATA_SIZE, SCE_KERNEL_MEMORY_ACCESS_R | SCE_KERNEL_MEMORY_ACCESS_W);
    vjo_hooks_start("AFEJ00001", VJO_TEXT_SOURCE_AUTO, 0);
    TEST_ASSERT(n_sites == 1 && site_info[0].ver && !site_info[0].luna && vjo_hooks_verified(1));
    TEST_CHECK(n_offs == 1 && offs[0].segidx == 0 && offs[0].offset == 0x20F0u && offs[0].thumb == 1);
    TEST_CHECK(site_info[0].ver->filter == 0 && site_info[0].ver->read == VJO_PLACE_READ_STRING);

    /* a page: its NUL ends the read; the clean-up drops a C0 control and keeps the line break */
    memcpy(data + 0x100, page, sizeof page);
    memset(frame, 0, sizeof frame);
    frame[0] = a32(data) + 0x100;
    TEST_CHECK(vjo_hooks_observe(0, frame) == (uintptr_t)CONT(0));
    TEST_ASSERT(vjo_hooks_snap(1, &s));
    TEST_CHECK(s.kind == VJO_HOOK_KIND_SITE && s.key == 0x20F0u && s.site == NULL);
    TEST_CHECK(s.encoding == VJO_HOOK_ENC_UTF8 && s.n == sizeof page - 1u && !memcmp(s.raw, page, s.n));
    TEST_CHECK(vjo_hooks_text(&s, text, sizeof text) == sizeof want - 1u && !strcmp(text, want));
    TEST_MSG("got [%s]", text);

    /* a page of 600 bytes before its NUL: the first VJO_HOOK_RAW_BYTES of them; the character the cut splits is
     * dropped */
    for (unsigned i = 0; i < 200; i++) memcpy(data + 0x400 + 3u * i, "あ", 3);
    for (unsigned i = 0; i < 170; i++) memcpy(cut + 3u * i, "あ", 3);
    cut[3u * 170u] = 0;
    frame[0] = a32(data) + 0x400;
    TEST_CHECK(vjo_hooks_observe(0, frame) == (uintptr_t)CONT(0));
    TEST_ASSERT(vjo_hooks_snap(1, &s));
    TEST_CHECK(s.n == VJO_HOOK_RAW_BYTES && !memcmp(s.raw, data + 0x400, VJO_HOOK_RAW_BYTES));
    TEST_CHECK(vjo_hooks_text(&s, text, sizeof text) == 3u * 170u && !strcmp(text, cut));

    /* a page its memory block ends inside, with no NUL: read to the block's end only (the guard page after it faults) */
    memcpy(data + DATA_SIZE - 9u, "あいう", 9);
    frame[0] = a32(data) + DATA_SIZE - 9u;
    TEST_CHECK(vjo_hooks_observe(0, frame) == (uintptr_t)CONT(0));
    TEST_ASSERT(vjo_hooks_snap(1, &s));
    TEST_CHECK(s.n == 9u && vjo_hooks_text(&s, text, sizeof text) == 9u && !strcmp(text, "あいう"));

    /* r0 in no memory block: no text, the stream keeps its last */
    seq = s.seq;
    frame[0] = 0x10u;
    TEST_CHECK(vjo_hooks_observe(0, frame) == (uintptr_t)CONT(0));
    TEST_CHECK(vjo_hooks_snap(1, &s) && s.seq == seq && s.n == 9u);
    /* the hits only read the game's memory */
    TEST_CHECK(!memcmp(data + 0x100, page, sizeof page) && !memcmp(code + 0x20F0, sig, sizeof sig));
}

/* ---- Rubi's verified entries on the farm's own bytes (tools/hook_farm.py fixtures) ---- */

typedef struct {
    const char *title;
    uint32_t off;     /* the entry's segment-0 offset */
    uint16_t app_ver; /* the farm's APP_VER */
    const char *raw;  /* the bytes the farm's hook read at the entry's register */
    unsigned n;
    const char *text; /* the text the farm verified against the frame */
} FarmRow;
/* The farm's record of a hook (its rubi-hook.json and reader), in the terms of a VjoPlaceVerified */
typedef struct {
    const char *title;
    uint32_t nid, off;
    int thumb;        /* 1 Thumb, 0 ARM: the record's flag, or the mode its bytes decode to where the record says to
                       * check it by disassembly (tools/hook_farm.py MODES_BY_DISASSEMBLY) */
    unsigned reg;
    uint32_t padding; /* 0x81000000 or more: the game's address of the text, the register 0 at the site */
    unsigned encoding, read;
    unsigned count_off, data_off; /* a cell record's count and cells (VJO_PLACE_READ_CELLS) */
    const char *sig;  /* the bytes at the site */
    unsigned sig_len;
} FarmSite;
#include "fixtures/hook_farm.inc"
static const FarmRow farm_rows[] = {HOOK_FARM_ROWS};
static const FarmSite farm_sites[] = {HOOK_FARM_SITES};
_Static_assert(sizeof farm_rows / sizeof farm_rows[0] == HOOK_FARM_FIXTURES, "every excerpt");
_Static_assert(sizeof farm_sites / sizeof farm_sites[0] == HOOK_FARM_SITE_COUNT, "every hook record");
#define FARM_SEG0 0x81000000u /* segment 0 where the farm's Vita3K had it for every one of these games */

/* The farm's record of title's hook, and Rubi's entry at its site, which must say what the record says: the NID, the
 * offset, the mode (where the record knows it), the bytes, the encoding, the read and a cell record's layout, the
 * register and padding (an address of the farm's with its register 0 at the site is r15 + padding from a segment 0 at
 * FARM_SEG0: PCSG00380's static). #316's audit F3: the excerpts installed Rubi's own NID, bytes, register and
 * padding, so a wrong one passed. */
static const FarmSite *farm_entry(const char *title, const VjoPlaceVerified **entry)
{
#ifdef VJO_EXTERNAL_HOOK_PROFILES
    profile_for_farm(title);
#endif
    const FarmSite *fs = NULL;
    const VjoPlaceVerified *all, *v = NULL;
    unsigned nv;
    for (unsigned k = 0; k < HOOK_FARM_SITE_COUNT; k++)
        if (!strcmp(farm_sites[k].title, title)) fs = &farm_sites[k];
    TEST_ASSERT_(fs != NULL, "%s: the farm's record of its hook", title);
    nv = vjo_place_verified(title, &all);
    for (unsigned i = 0; i < nv; i++)
        if (all[i].off == fs->off) v = &all[i];
    TEST_ASSERT_(v != NULL, "%s: Rubi's entry at the farm's +%X", title, (unsigned)fs->off);
    TEST_CHECK_(v->module_nid == fs->nid, "%s: NID %08X, the farm's %08X", title, (unsigned)v->module_nid,
                (unsigned)fs->nid);
    /* the mode of every record (audit-rubi-audit-fix-hooks A-F1: those the farm says to check by disassembly were
     * skipped, and a flipped flag passed for PCSG00469, PCSG00553 and PCSG00807) */
    TEST_CHECK_((fs->thumb == 0 || fs->thumb == 1) && v->thumb == (unsigned)fs->thumb, "%s: Thumb %u, the farm's %d",
                title, (unsigned)v->thumb, fs->thumb);
    /* every byte the farm recorded (A-N1: 12 of the 16 passed); an entry that pins fewer, for a relocated address in
     * them, says why here: PCSG00505's 8 (#394), the farm's next 8 being a movw/movt of an address the loader writes
     * for where it loads the relocatable eboot (another address wherever segment 0 is not at 0x81000000), which the
     * check below confirms in the farm's bytes: MOVW and MOVT T3 (11110i10_0100/_1100 imm4, then 0 imm3 Rd imm8: bit 15
     * of the second halfword clear, which a BL's is not) of one register, neither SP nor PC (audit-rubi-merge-394 F1:
     * a check of the first halfwords and the Rd nibbles alone took bl; bl for a pair) */
    unsigned want = fs->sig_len;
    if (!strcmp(title, "PCSG00505") && fs->sig_len == 16u) {
        const uint8_t *b = (const uint8_t *)fs->sig + 8;
        unsigned h1 = b[0] | b[1] << 8, h2 = b[2] | b[3] << 8, h3 = b[4] | b[5] << 8, h4 = b[6] | b[7] << 8;
        unsigned rd = (h2 >> 8) & 0xFu;
        TEST_CHECK_((h1 & 0xFBF0u) == 0xF240u && (h3 & 0xFBF0u) == 0xF2C0u && !(h2 & 0x8000u) && !(h4 & 0x8000u) &&
                        rd == ((h4 >> 8) & 0xFu) && rd != 13u && rd != 15u,
                    "%s: the farm's bytes after the entry's are a movw/movt pair of one register", title);
        want = 8u;
    }
    TEST_CHECK_(v->sig_len == want && !memcmp(v->sig, fs->sig, v->sig_len),
                "%s: the %u bytes at the site are the farm's (%u of its %u)", title, (unsigned)v->sig_len, want,
                fs->sig_len);
    /* (CP932 with its 0xA0 kept is CP932: VJO_HOOK_ENC_CP932_A0, the MSGSET titles' blank cell) */
    TEST_CHECK_((v->encoding & ~VJO_HOOK_DECODE_A0) == fs->encoding && v->read == fs->read,
                "%s: encoding %u, read %u; the farm's %u, %u", title, (unsigned)v->encoding, (unsigned)v->read,
                fs->encoding, fs->read);
    if (fs->padding >= FARM_SEG0)
        TEST_CHECK_(v->reg == 15 && FARM_SEG0 + v->off + (uint32_t)v->padding == fs->padding,
                    "%s: r%u + 0x%X from a segment 0 at 0x81000000, the farm's address 0x%08X", title, (unsigned)v->reg,
                    (unsigned)v->padding, (unsigned)fs->padding);
    else
        TEST_CHECK_(v->reg == fs->reg && (uint32_t)v->padding == fs->padding, "%s: r%u + %d, the farm's r%u + %u",
                    title, (unsigned)v->reg, (int)v->padding, fs->reg, (unsigned)fs->padding);
    {
        uint32_t count_at = 0, cells_at = 0;
        TEST_CHECK_(!vjo_place_cells_layout(v->read, &count_at, &cells_at) ||
                        (fs->count_off == count_at && fs->data_off == cells_at),
                    "%s: the record's count at +0x%X and cells at +0x%X", title, fs->count_off, fs->data_off);
    }
    *entry = v;
    return fs;
}

/* A segment 0 at FARM_SEG0 (0x81000000), size bytes, for the test that runs (MAP_FIXED_NOREPLACE: an address already
 * taken fails the test, nothing is overwritten; under x86-64 ASan that address is shadow memory). TEST_FINI frees it
 * after the test, whatever its outcome. */
static uint8_t *fixed_map;
static size_t fixed_map_size;
static uint8_t *fixed_seg0(uint32_t size)
{
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    void *p;
#ifdef MAP_FIXED_NOREPLACE
    flags |= MAP_FIXED_NOREPLACE;
#endif
    p = mmap((void *)(uintptr_t)FARM_SEG0, size, PROT_READ | PROT_WRITE, flags, -1, 0);
    TEST_ASSERT_(p == (void *)(uintptr_t)FARM_SEG0, "a segment 0 at 0x81000000 (mmap gave %p)", p);
    fixed_map = p;
    fixed_map_size = size;
    return p;
}
static void unmap_fixed_seg0(void)
{
    if (fixed_map) munmap(fixed_map, fixed_map_size);
    fixed_map = NULL;
}

/* Each excerpt through the worker's own path in the game as the farm recorded it: a main module with the farm's NID
 * and the farm's bytes at the farm's offset, in a segment 0 at 0x81000000, and the farm's APP_VER in param.sfo; the
 * hit's bytes where the farm's hook read them, its register + padding (through the pointer there, or as a cell
 * record's count and cells), then a NUL unit; Rubi's real entry, the observer, the stream's snapshot and
 * vjo_hooks_text. The hook must go where the farm's did, and the text must be the farm's, exactly. */
static void test_farm_excerpts(void)
{
    uint8_t *seg;
    uint32_t seg_size = 0;
    unsigned titles = 0;
    const char *last = "";
    for (unsigned k = 0; k < HOOK_FARM_SITE_COUNT; k++) { /* the farthest site, or a static the farm read */
        const FarmSite *fs = &farm_sites[k];
        uint32_t end = fs->padding >= FARM_SEG0 ? fs->padding - FARM_SEG0 + fs->data_off + 2u * VJO_HOOK_RAW_BYTES + 4u
                                                : fs->off + 64u;
        if (end > seg_size) seg_size = end;
    }
    seg_size = (seg_size + PAGE - 1u) & ~(PAGE - 1u);
    seg = fixed_seg0(seg_size); /* only the pages its bytes touch exist */
    for (unsigned k = 0; k < HOOK_FARM_FIXTURES; k++) {
        const FarmRow *row = &farm_rows[k];
        const VjoPlaceVerified *v;
        const FarmSite *fs = farm_entry(row->title, &v);
        uint32_t frame[16], at;
        uint8_t *p;
        VjoHookSnap s;
        char text[1024], ver[8];
        size_t len;
        TEST_CHECK_(row->off == fs->off, "%s: the hit at the farm's site", row->title);
        TEST_CHECK_(v->app_ver == row->app_ver, "%s: the entry's APP_VER %04X is the farm's %04X", row->title,
                    (unsigned)v->app_ver, (unsigned)row->app_ver);
        titles += strcmp(last, row->title) != 0;
        last = row->title;
        reset();
        memcpy(seg + fs->off, fs->sig, fs->sig_len);
        mod.seg0 = seg;
        mod.seg0_size = fs->off + 64u;
        mod.nid = fs->nid;
        if (row->app_ver) {
            snprintf(ver, sizeof ver, "%02X.%02X", (unsigned)(row->app_ver >> 8), (unsigned)(row->app_ver & 0xFFu));
            set_sfo(ver);
        }
        add_block(a32(data), DATA_SIZE, SCE_KERNEL_MEMORY_ACCESS_R | SCE_KERNEL_MEMORY_ACCESS_W);
        add_block(a32(seg), seg_size, SCE_KERNEL_MEMORY_ACCESS_R | SCE_KERNEL_MEMORY_ACCESS_W);
        vjo_hooks_start(row->title, VJO_TEXT_SOURCE_AUTO, 0);
        TEST_ASSERT_(n_sites >= 1 && site_info[0].ver == v, "%s: its entry is hooked first", row->title);
        TEST_CHECK_(n_offs >= 1 && offs[0].offset == fs->off && offs[0].thumb == fs->thumb,
                    "%s: hooked at the farm's +%X", row->title, (unsigned)fs->off);
        TEST_ASSERT(row->n + 2u <= DATA_SIZE - 0x200u);
        memset(frame, 0, sizeof frame);
        /* where the farm's hook read: its register + padding (an address of the game's, its register 0 there) */
        at = fs->padding >= FARM_SEG0 ? fs->padding : a32(data) + 0x100u;
        if (fs->padding < FARM_SEG0) frame[fs->reg] = at - fs->padding;
        p = (uint8_t *)(uintptr_t)at;
        if (fs->read == VJO_PLACE_READ_CELLS || fs->read == VJO_PLACE_READ_CELLS_B) { /* the farm read the record's
                                                                                          * count of cells, then them */
            uint32_t count = row->n / 2u;
            memcpy(p + fs->count_off, &count, 4);
            memcpy(p + fs->data_off, row->raw, row->n);
        } else if (fs->read == VJO_PLACE_READ_DEREF) { /* the text's pointer there */
            uint32_t text_at = a32(data) + 0x200u;
            memcpy(p, &text_at, 4);
            memcpy(data + 0x200, row->raw, row->n);
            data[0x200 + row->n] = data[0x201 + row->n] = 0;
        } else {
            memcpy(p, row->raw, row->n);
            p[row->n] = p[row->n + 1u] = 0;
        }
        TEST_CHECK(vjo_hooks_observe(0, frame) == (uintptr_t)CONT(0));
        TEST_ASSERT_(vjo_hooks_snap(1, &s), "%s: the hit is in its stream", row->title);
        len = vjo_hooks_text(&s, text, sizeof text);
        TEST_CHECK_(len == strlen(row->text) && !strcmp(text, row->text), "%s excerpt %u", row->title, k);
        TEST_MSG("got  [%s]", text);
        TEST_MSG("want [%s]", row->text);
        memset(seg + fs->off, 0, fs->sig_len);
        if (fs->padding >= FARM_SEG0) memset(p, 0, fs->data_off + row->n);
    }
    TEST_CHECK_(titles >= 13, "excerpts of %u titles", titles);
}

/* TIME TRAVELERS (PCSG00062): Luna's two sites, the real table's (0x8119ADDA r7 and 0x811A6052 r8, CP932, its filter
 * PCSG00062), in a main module laid out as the game's eboot.bin is in public Vita3K logs (compatibility #400): a
 * fixed-address executable, segment 0 at its link address 0x81000000, 0x31258C bytes (APP_VER 01.00; 01.01's is
 * 0x306C90). Less VJO_LUNA_SEG0 the sites are 0x1196DDA and 0x11A2052, past the segment's end: main refused both as
 * range without reading a byte of them. Luna's entry (2025-09-24) predates its addr_offset (2026-05-20): the
 * addresses are Vita3K's guest addresses, +0x19ADDA and +0x1A6052 of that segment, where the placement check now
 * reads the bytes (the generator marks both VJO_LUNA_F_GUEST: the logs show a fixed-address executable). Zeros there
 * (as for a block inside a function, which a console run may show) refuse both as no entry, with the bytes in the
 * debug log; a function's entry at both hooks both, and a hit's text goes through Luna's filter. With segment 0
 * anywhere but 0x81000000 the guest address is not read, nor for a relocatable executable's unmarked site. The game's
 * own bytes there are not known here (no dump of it): its console run decides. The test maps 0x81000000 itself
 * (fixed_seg0, freed after it; MAP_FIXED_NOREPLACE: an address already taken fails the test cleanly, nothing is
 * overwritten); under x86-64 ASan that address is shadow memory (0x7fff8000-0x8fff6fff), so the test cannot run in such
 * a build. */
static void test_luna_own_address(void)
{
    enum { TT_SEG0 = 0x81000000u, TT_SIZE = 0x31258Cu, TT_MAP = 0x315000u }; /* TT_MAP: segment 0 a page higher too */
    /* [時間/じかん]を<#FF0000>旅</#>する\n人 (a literal backslash and n) and <b>未来</b>は[変/か]わる, CP932 */
    static const char l7[] = "[\x8E\x9E\x8A\xD4/\x82\xB6\x82\xA9\x82\xF1]\x82\xF0<#FF0000>\x97\xB7</#>\x82\xB7\x82\xE9"
                             "\\n\x90\x6C";
    static const char l8[] = "<b>\x96\xA2\x97\x88</b>\x82\xCD[\x95\xCF/\x82\xA9]\x82\xED\x82\xE9";
    uint8_t *tt;
    const VjoLunaSite *real;
    uint32_t frame[16], inst, refused;
    VjoHookSnap s;
    char text[128];
    _Static_assert(sizeof l7 == 39 && sizeof l8 == 25, "the two lines' CP932 bytes and their NUL");
    TEST_ASSERT(vjo_luna_sites("PCSG00062", &real) == 2);
    TEST_ASSERT(real[0].addr == 0x8119ADDAu && real[0].reg == 7 && real[1].addr == 0x811A6052u && real[1].reg == 8);
    TEST_CHECK(real[0].addr - VJO_LUNA_SEG0 >= TT_SIZE && real[1].addr - VJO_LUNA_SEG0 >= TT_SIZE);
    _Static_assert(TT_SEG0 == FARM_SEG0, "one fixed segment 0");
    tt = fixed_seg0(TT_MAP + PAGE); /* freed after the test (TEST_FINI), as the farm test's */
    TEST_ASSERT(!mprotect(tt + TT_MAP, PAGE, PROT_NONE));

    /* zeros at both sites: no function's entry there, refused, with the bytes the check read */
    reset();
    memset(tt, 0, TT_MAP);
    mod.seg0 = tt;
    mod.seg0_size = TT_SIZE;
    set_sfo("01.00");
    vjo_hooks_start("PCSG00062", VJO_TEXT_SOURCE_AUTO, 0);
    vjo_hooks_counts(NULL, &inst, &refused);
    TEST_CHECK(n_offs == 0 && offset_calls == 0 && inst >> 16 == 0 && refused == 2);
#if VJO_DEBUG
    {
        char outcomes[64];
        log_outcomes(outcomes, sizeof outcomes);
        TEST_CHECK_(!strcmp(outcomes, "entry,entry"), "outcomes %s", outcomes);
        TEST_CHECK(strstr(logbuf, "seg0 81000000+31258C APP_VER 0100; 0 verified, 2 Luna sites\n") != NULL);
        TEST_CHECK(strstr(logbuf, "luna 8119ADDA r7 +19ADDA refused: entry\n bytes 00000000000000000000000000000000\n") !=
                   NULL);
        TEST_CHECK(strstr(logbuf, "luna 811A6052 r8 +1A6052 refused: entry\n bytes ") != NULL);
        TEST_MSG("%s", logbuf);
    }
#endif

    /* a function's entry at both: both hooked, as Thumb sites (both addresses are 2 mod 4: 12 bytes patched) */
    reset();
    memset(tt, 0, TT_MAP);
    put_site_at(tt, 0x19ADDAu, 1, NULL);
    put_site_at(tt, 0x1A6052u, 1, NULL);
    mod.seg0 = tt;
    mod.seg0_size = TT_SIZE;
    set_sfo("01.00");
    add_block(a32(data), DATA_SIZE, SCE_KERNEL_MEMORY_ACCESS_R | SCE_KERNEL_MEMORY_ACCESS_W);
    vjo_hooks_start("PCSG00062", VJO_TEXT_SOURCE_AUTO, 0);
    TEST_ASSERT(n_offs == 2 && n_sites == 2);
    TEST_CHECK(offs[0].offset == 0x19ADDAu && offs[0].thumb == 1 && offs[1].offset == 0x1A6052u && offs[1].thumb == 1);
    TEST_CHECK(site_info[0].luna == &real[0] && site_info[1].luna == &real[1] && !site_info[0].ver);
    TEST_CHECK(site_info[0].addr == TT_SEG0 + 0x19ADDAu && site_info[1].addr == TT_SEG0 + 0x1A6052u);
    TEST_CHECK(slots[0].key == 0x19ADDBu && slots[1].key == 0x1A6053u);
    vjo_hooks_counts(NULL, &inst, &refused);
    TEST_CHECK(inst >> 16 == 2 && refused == 0);
#if VJO_DEBUG
    TEST_CHECK(strstr(logbuf, "luna 8119ADDA r7 +19ADDA hooked, stream 1\n") != NULL);
    TEST_CHECK(strstr(logbuf, "luna 811A6052 r8 +1A6052 hooked, stream 2\n") != NULL);
#endif
    /* a hit at each, the text in r7 and r8; Luna's filter: [base/reading] to the base, <tags> and the literal \n */
    memcpy(data + 0x40, l7, sizeof l7);
    memset(frame, 0, sizeof frame);
    frame[7] = a32(data) + 0x40;
    TEST_CHECK(vjo_hooks_observe(0, frame) == (uintptr_t)CONT(0));
    TEST_CHECK(frame[15] == TT_SEG0 + 0x19ADDAu);
    TEST_ASSERT(vjo_hooks_snap(1, &s));
    TEST_CHECK(s.kind == VJO_HOOK_KIND_SITE && s.key == 0x19ADDAu && s.site == &real[0] && s.n == sizeof l7 - 1);
    TEST_CHECK(vjo_hooks_text(&s, text, sizeof text) > 0 && !strcmp(text, "時間を旅する\n人"));
    TEST_MSG("got [%s]", text);
    memcpy(data + 0x100, l8, sizeof l8);
    frame[8] = a32(data) + 0x100;
    TEST_CHECK(vjo_hooks_observe(1, frame) == (uintptr_t)CONT(1));
    TEST_ASSERT(vjo_hooks_snap(2, &s));
    TEST_CHECK(s.key == 0x1A6052u && s.site == &real[1] && s.n == sizeof l8 - 1);
    TEST_CHECK(vjo_hooks_text(&s, text, sizeof text) > 0 && !strcmp(text, "未来は変わる"));
    TEST_MSG("got [%s]", text);

    /* 01.01's segment 0 (0x306C90 bytes) holds both; one that ends between them: the second is out of range in
     * both readings */
    for (unsigned k = 0; k < 2; k++) {
        static const uint32_t size[2] = {0x306C90u, 0x1A0000u};
        reset();
        memset(tt, 0, TT_MAP);
        put_site_at(tt, 0x19ADDAu, 1, NULL);
        put_site_at(tt, 0x1A6052u, 1, NULL);
        mod.seg0 = tt;
        mod.seg0_size = size[k];
        set_sfo(k ? "01.00" : "01.01");
        vjo_hooks_start("PCSG00062", VJO_TEXT_SOURCE_AUTO, 0);
        vjo_hooks_counts(NULL, &inst, &refused);
        TEST_CHECK_(inst >> 16 == (k ? 1u : 2u) && refused == k, "segment 0 of 0x%X bytes", (unsigned)size[k]);
#if VJO_DEBUG
        if (k) TEST_CHECK(strstr(logbuf, "luna 811A6052 r8 refused: range\n bytes \n") != NULL);
#endif
    }

    /* segment 0 one page higher (not where a fixed-address executable is): no guest reading. The entries are where
     * a rule that read the address less any segment base would look (0x8119ADDA itself, +0x199DDA of this segment),
     * so such a rule would hook them, in both builds */
    reset();
    memset(tt, 0, TT_MAP);
    put_site_at(tt, 0x19ADDAu, 1, NULL);
    put_site_at(tt, 0x1A6052u, 1, NULL);
    mod.seg0 = tt + PAGE;
    mod.seg0_size = TT_SIZE;
    set_sfo("01.00");
    vjo_hooks_start("PCSG00062", VJO_TEXT_SOURCE_AUTO, 0);
    vjo_hooks_counts(NULL, &inst, &refused);
    TEST_CHECK(n_offs == 0 && offset_calls == 0 && inst >> 16 == 0 && refused == 2);
#if VJO_DEBUG
    {
        char outcomes[64];
        log_outcomes(outcomes, sizeof outcomes);
        TEST_CHECK_(!strcmp(outcomes, "range,range"), "outcomes %s", outcomes);
    }
#endif

    /* a relocatable executable's site at or above 0x81000000 (PCSG00787's 0x810286C8, not marked: its address is not
     * the eboot's own) in a segment 0 at 0x81000000 of its logged 0xCE687 bytes, with a function's entry at the
     * guest offset 0x286C8: not read there, refused as range as on main; its other sites and its verified entry
     * find zeros. Nothing is hooked, in both builds */
    {
        const VjoLunaSite *l;
        unsigned nl = vjo_luna_sites("PCSG00787", &l), k = 0;
        while (k < nl && l[k].addr != 0x810286C8u) k++;
        TEST_ASSERT_(k < nl && l[k].ported && !(l[k].flags & VJO_LUNA_F_GUEST), "PCSG00787's site 0x810286C8, unmarked");
        reset();
        memset(tt, 0, TT_MAP);
        put_site_at(tt, 0x286C8u, 1, NULL);
        mod.seg0 = tt;
        mod.seg0_size = 0xCE687u;
        set_sfo("01.00");
        vjo_hooks_start("PCSG00787", VJO_TEXT_SOURCE_AUTO, 0);
        vjo_hooks_counts(NULL, &inst, &refused);
        TEST_CHECK_(n_offs == 0 && offset_calls == 0 && inst >> 16 == 0 && refused == 1u + nl, "%u sites, %u refused",
                    (unsigned)(inst >> 16), (unsigned)refused);
#if VJO_DEBUG
        TEST_CHECK(strstr(logbuf, "luna 810286C8 r0 refused: range\n bytes \n") != NULL);
        TEST_MSG("%s", logbuf);
#endif
    }
}

/* VJO_PLACE_READ_CELLS: the record at the register + padding; exactly its count of cells, at most 256 (512 bytes), as
 * CP932 bytes; an unreadable count or cells, or a count of 0, give no hit. The record's layout is the farm's (PCSG00380
 * reader-config.json, PCSG00914's): its count at +0x14, its cells from +0x20 (REC_COUNT, REC_CELLS). Register 15, the
 * site's own address (PCSG00380's static), only with segment 0 at 0x81000000, where the farm read it (#316's audit
 * F4): here it is elsewhere, so such an entry is refused with its bytes matching (test_farm_excerpts runs PCSG00380's
 * at 0x81000000); the cells are read through r4. */
static void test_verified_cells(void)
{
    /* 「あ」 then a line break and い as cells; a stale cell after them that the count leaves out */
    static const uint8_t cells[] = {0x81, 0x75, 0x82, 0xA0, 0x81, 0x76, 0x0A, 0x00, 0x82, 0xA2, 0x82, 0xA4};
    VjoPlaceVerified v;
    uint32_t frame[16], count = 5, rec, inst, refused;
    VjoHookSnap s;
    char text[64];
    reset();
    TEST_ASSERT(a32(code) != 0x81000000u);
    put_site(0x100, 1, NULL);
    v = verified_entry(0x100, mod.nid, 15, 0x200);
    v.read = VJO_PLACE_READ_CELLS;
    t_ver = &v;
    t_nver = 1;
    vjo_hooks_start(TITLE, VJO_TEXT_SOURCE_AUTO, 0);
    vjo_hooks_counts(NULL, &inst, &refused);
    TEST_CHECK(n_sites == 0 && n_offs == 0 && refused == 1);
#if VJO_DEBUG
    TEST_CHECK(strstr(logbuf, "r15 refused: relocated\n") != NULL);
    TEST_MSG("%s", logbuf);
#endif
    reset();
    put_site(0x100, 1, NULL);
    v = verified_entry(0x100, mod.nid, 4, 0x20);
    v.read = VJO_PLACE_READ_CELLS;
    rec = a32(data) + 0x200;
    t_ver = &v;
    t_nver = 1;
    add_block(a32(data), DATA_SIZE, SCE_KERNEL_MEMORY_ACCESS_R | SCE_KERNEL_MEMORY_ACCESS_W);
    vjo_hooks_start(TITLE, VJO_TEXT_SOURCE_AUTO, 0);
    TEST_ASSERT(n_sites == 1 && site_info[0].ver == &v);
    memcpy(data + 0x200 + REC_COUNT, &count, 4);
    memcpy(data + 0x200 + REC_CELLS, cells, sizeof cells);
    memset(frame, 0, sizeof frame);
    frame[4] = rec - 0x20u; /* the register + padding: the record */
    TEST_CHECK(vjo_hooks_observe(0, frame) == (uintptr_t)CONT(0));
    TEST_ASSERT(vjo_hooks_snap(1, &s));
    TEST_CHECK(s.n == 9 && !memcmp(s.raw, "\x81\x75\x82\xA0\x81\x76\x0A\x82\xA2", 9));
    TEST_CHECK(vjo_hooks_text(&s, text, sizeof text) > 0 && !strcmp(text, "「あ」\nい"));
    TEST_MSG("got [%s]", text);
    /* a count past the buffer reads 256 cells; one past the block, or 0, gives no hit */
    count = 1000;
    memcpy(data + 0x200 + REC_COUNT, &count, 4);
    for (unsigned i = 0; i < 256; i++) data[0x200 + REC_CELLS + 2 * i] = 'A', data[0x201 + REC_CELLS + 2 * i] = 0;
    vjo_hooks_observe(0, frame);
    TEST_CHECK(vjo_hooks_snap(1, &s) && s.hits == 2 && s.n == 256);
    /* a record 0x40 bytes before the block's end: its count is there, 32 cells (64 bytes) are not */
    frame[4] = a32(data) + DATA_SIZE - 0x40u - 0x20u;
    count = 32;
    memcpy(data + DATA_SIZE - 0x40u + REC_COUNT, &count, 4);
    vjo_hooks_observe(0, frame);
    TEST_CHECK(vjo_hooks_snap(1, &s) && s.hits == 2);
    count = 0;
    frame[4] = rec - 0x20u;
    memcpy(data + 0x200 + REC_COUNT, &count, 4);
    vjo_hooks_observe(0, frame);
    TEST_CHECK(vjo_hooks_snap(1, &s) && s.hits == 2);
}

/* VJO_PLACE_READ_CELLS_B: the same cells in foaf4's page-build record (PCSG00769), its count at +0x20 and its cells from
 * +0x2E (BUILD_COUNT, BUILD_CELLS), not the page record's +0x14 and +0x20: a record whose +0x14 holds another count
 * (read the page record's way it would give one control byte, the low byte of the count at +0x20) gives its own text;
 * the bounds are the page record's. */
static void test_verified_build_cells(void)
{
    /* 「あ」 then a line break and い as cells; a stale cell after them that the count leaves out */
    static const uint8_t cells[] = {0x81, 0x75, 0x82, 0xA0, 0x81, 0x76, 0x0A, 0x00, 0x82, 0xA2, 0x82, 0xA4};
    VjoPlaceVerified v;
    uint32_t frame[16], count = 5, three = 3, rec;
    VjoHookSnap s;
    char text[64];
    reset();
    put_site(0x100, 1, NULL);
    v = verified_entry(0x100, mod.nid, 6, 0);
    v.read = VJO_PLACE_READ_CELLS_B;
    rec = a32(data) + 0x200;
    t_ver = &v;
    t_nver = 1;
    add_block(a32(data), DATA_SIZE, SCE_KERNEL_MEMORY_ACCESS_R | SCE_KERNEL_MEMORY_ACCESS_W);
    vjo_hooks_start(TITLE, VJO_TEXT_SOURCE_AUTO, 0);
    TEST_ASSERT(n_sites == 1 && site_info[0].ver == &v);
    memcpy(data + 0x200 + REC_COUNT, &three, 4);
    memcpy(data + 0x200 + BUILD_COUNT, &count, 4);
    memcpy(data + 0x200 + BUILD_CELLS, cells, sizeof cells);
    memset(frame, 0, sizeof frame);
    frame[6] = rec; /* the register: the record */
    TEST_CHECK(vjo_hooks_observe(0, frame) == (uintptr_t)CONT(0));
    TEST_ASSERT(vjo_hooks_snap(1, &s));
    TEST_CHECK(s.n == 9 && !memcmp(s.raw, "\x81\x75\x82\xA0\x81\x76\x0A\x82\xA2", 9));
    TEST_CHECK(vjo_hooks_text(&s, text, sizeof text) > 0 && !strcmp(text, "「あ」\nい"));
    TEST_MSG("got [%s]", text);
    /* a count past the buffer reads 256 cells (VJO_HOOK_RAW_BYTES / 2), as the page record's (audit-psandvita-1 F4: with
     * the clamp for the page record only, every test passed) */
    count = 1000;
    memcpy(data + 0x200 + BUILD_COUNT, &count, 4);
    for (unsigned i = 0; i < 256; i++) data[0x200 + BUILD_CELLS + 2 * i] = 'A', data[0x201 + BUILD_CELLS + 2 * i] = 0;
    vjo_hooks_observe(0, frame);
    TEST_CHECK(vjo_hooks_snap(1, &s) && s.hits == 2 && s.n == 256);
    /* its cells past the block's end (the count read, 32 cells not), or a count of 0: no hit */
    frame[6] = a32(data) + DATA_SIZE - 0x40u;
    count = 32;
    memcpy(data + DATA_SIZE - 0x40u + BUILD_COUNT, &count, 4);
    vjo_hooks_observe(0, frame);
    TEST_CHECK(vjo_hooks_snap(1, &s) && s.hits == 2);
    count = 0;
    frame[6] = rec;
    memcpy(data + 0x200 + BUILD_COUNT, &count, 4);
    vjo_hooks_observe(0, frame);
    TEST_CHECK(vjo_hooks_snap(1, &s) && s.hits == 2);
}

/* ---- MAGES SC3 sites on the farm's own pages (tools/sc3_charset.py --fixtures) ---- */

typedef struct {
    const char *title;
    const char *raw; /* the bytes the farm's hook read at r0, through the string's end token */
    unsigned n;
    const char *text; /* the farm's verified text, ruby's base only */
} Sc3Row;
#include "fixtures/hook_sc3_farm.inc"
static const Sc3Row sc3_rows[] = {HOOK_SC3_FARM_ROWS};

/* A committed charset file (third_party/sc3-charsets), as the worker's load reads it into its block. */
static size_t sc3_file(const char *title, uint8_t *out, size_t cap)
{
    char path[512];
    FILE *f;
    size_t n;
    snprintf(path, sizeof path, "%s/third_party/sc3-charsets/%s.sc3", VJO_SOURCE_DIR, title);
    f = fopen(path, "rb");
    TEST_ASSERT_(f != NULL, "%s", path);
    n = fread(out, 1, cap, f);
    fclose(f);
    return n;
}

/* Rubi's real SC3 entries in the game as the farm recorded it (its record's NID, the bytes at its offset and its
 * register: farm_entry; APP_VER 01.00), the farm's pages
 * at r0 with other bytes after the end token, as a script buffer has them (zeros, the next string): the stream keeps
 * the string through its end token, so the bytes after it never make another text; until the charset is set its
 * text is empty, and the charset is set only from exactly the pinned file (another title's, one byte changed, one
 * short: refused); then vjo_hooks_text gives the farm's text. A string whose end token is not within the 512 bytes
 * a hit reads gives no hit; one that ends at its block's end is read (the read stops at the block). */
static void test_sc3_sites(void)
{
    static uint8_t *seg;
    static uint8_t file[VJO_SC3_MAX_BYTES + 1u], other[VJO_SC3_MAX_BYTES];
    const uint32_t seg_size = 0x30000u;
    unsigned titles = 0;
    const char *last = "";
    if (!seg) seg = low_map(seg_size, PAGE);
    TEST_ASSERT(seg != NULL);
    for (unsigned k = 0; k < HOOK_SC3_FARM_FIXTURES; k++) {
        const Sc3Row *row = &sc3_rows[k];
        const VjoPlaceVerified *v;
        const FarmSite *fs;
        uint32_t frame[16];
        VjoHookSnap s;
        char text[1024];
        size_t n, n_other;
        uint32_t seq;
#ifdef VJO_EXTERNAL_HOOK_PROFILES
        profile_for_farm(row->title);
#endif
        TEST_ASSERT_(vjo_place_verified(row->title, &v) == 1 && v->read == VJO_PLACE_READ_SC3 &&
                     v->off + 64u <= seg_size, "%s: its SC3 entry", row->title);
        fs = farm_entry(row->title, &v); /* the farm's record of the hook, which the game below is */
        titles += strcmp(last, row->title) != 0;
        last = row->title;
        reset();
        memset(seg, 0, seg_size);
        memcpy(seg + fs->off, fs->sig, fs->sig_len);
        mod.seg0 = seg;
        mod.seg0_size = fs->off + 64u;
        mod.nid = fs->nid;
        set_sfo("01.00");
        add_block(a32(data), DATA_SIZE, SCE_KERNEL_MEMORY_ACCESS_R | SCE_KERNEL_MEMORY_ACCESS_W);
        vjo_hooks_start(row->title, VJO_TEXT_SOURCE_AUTO, 0);
        TEST_ASSERT_(n_sites == 1 && site_info[0].ver == v, "%s: its entry is hooked", row->title);
        TEST_CHECK(vjo_hooks_sc3_want() == vjo_sc3_pin(row->title) && vjo_hooks_sc3_want());
        /* the page, then zeros and the next string */
        memcpy(data + 0x100, row->raw, row->n);
        memcpy(data + 0x100 + row->n + 16u, "\x01\x80\x01\x02\x80\x00\xFF", 7);
        memset(frame, 0, sizeof frame);
        frame[fs->reg] = a32(data) + 0x100 - fs->padding; /* where the farm's hook read the page */
        TEST_CHECK(vjo_hooks_observe(0, frame) == (uintptr_t)CONT(0));
        TEST_ASSERT_(vjo_hooks_snap(1, &s), "%s: the hit is in its stream", row->title);
        TEST_CHECK_(s.n == row->n && s.encoding == VJO_HOOK_ENC_SC3 && !memcmp(s.raw, row->raw, row->n),
                    "%s excerpt %u: the string through its end token (%u B)", row->title, k, (unsigned)s.n);
        TEST_CHECK(vjo_hooks_text(&s, text, sizeof text) == 0 && !text[0]); /* no charset yet */
        /* only the pinned file */
        n = sc3_file(row->title, file, sizeof file);
        n_other = sc3_file(strcmp(row->title, "PCSG00146") ? "PCSG00146" : "PCSG00352", other, sizeof other);
        TEST_CHECK(!vjo_hooks_sc3_set(other, (uint32_t)n_other) && vjo_hooks_sc3_want());
        TEST_CHECK(!vjo_hooks_sc3_set(file, (uint32_t)n - 1u) && !vjo_hooks_sc3_set(NULL, (uint32_t)n));
        file[VJO_SC3_HEADER + 100u] ^= 1u;
        TEST_CHECK(!vjo_hooks_sc3_set(file, (uint32_t)n));
        file[VJO_SC3_HEADER + 100u] ^= 1u;
        TEST_ASSERT(vjo_hooks_sc3_set(file, (uint32_t)n) && !vjo_hooks_sc3_want());
        TEST_CHECK_(vjo_hooks_text(&s, text, sizeof text) == strlen(row->text) && !strcmp(text, row->text),
                    "%s excerpt %u", row->title, k);
        TEST_MSG("got  [%s]", text);
        TEST_MSG("want [%s]", row->text);
        /* other bytes after the end token are the same text: one more hit, no new sequence */
        seq = s.seq;
        memset(data + 0x100 + row->n, 0xA5, 8);
        vjo_hooks_observe(0, frame);
        TEST_CHECK(vjo_hooks_snap(1, &s) && s.seq == seq && s.hits == 2);
        /* no end token within 512 bytes: no hit */
        for (unsigned i = 0; i < VJO_HOOK_RAW_BYTES; i += 2) data[0x400 + i] = 0x80, data[0x401 + i] = 0x01;
        frame[fs->reg] = a32(data) + 0x400 - fs->padding;
        vjo_hooks_observe(0, frame);
        TEST_CHECK(vjo_hooks_snap(1, &s) && s.seq == seq && s.hits == 2);
        /* a string that ends at its block's end: the read stops there and keeps it */
        memcpy(data + DATA_SIZE - row->n, row->raw, row->n);
        frame[fs->reg] = a32(data) + DATA_SIZE - row->n - fs->padding;
        vjo_hooks_observe(0, frame);
        TEST_CHECK(vjo_hooks_snap(1, &s) && s.seq == seq && s.hits == 3 && s.n == row->n);
        TEST_CHECK(!vjo_hooks_sc3_set(NULL, 0) && vjo_hooks_text(&s, text, sizeof text) == 0); /* forgotten */
        memset(seg + fs->off, 0, fs->sig_len);
    }
    TEST_CHECK_(titles == 3, "excerpts of %u titles", titles);
    /* a site without an SC3 entry wants no charset */
    reset();
    TEST_CHECK(!vjo_hooks_sc3_want() && !vjo_hooks_sc3_set(file, VJO_SC3_HEADER));
}

static void test_clean_stream_text(void)
{
    static const struct { unsigned enc; const char *raw; size_t n; } cases[] = {
        {VJO_HOOK_ENC_UTF8, "\\s156猫\\n犬\\k", sizeof("\\s156猫\\n犬\\k") - 1},
        {VJO_HOOK_ENC_AUTO, "\\s156猫\\n犬\\k", sizeof("\\s156猫\\n犬\\k") - 1},
        {VJO_HOOK_ENC_CP932, "\\s156\x94\x4c\\n\x8c\xa2\\k", 13},
        {VJO_HOOK_ENC_UTF16LE, "\\\0s\0" "1\0" "5\0" "6\0\x2b\x73\\\0n\0\xac\x72\\\0k\0", 22},
    };
    for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        VjoHookSnap s = {0};
        char text[64];
        s.encoding = cases[k].enc;
        s.n = cases[k].n;
        memcpy(s.raw, cases[k].raw, s.n);
        TEST_CHECK(vjo_hooks_text(&s, text, sizeof(text)) == strlen("猫\n犬"));
        TEST_CHECK(!strcmp(text, "猫\n犬"));
        /* The same cleanup applies to a table site, after its own filter. */
        VjoLunaSite site = {.ported = 1, .encoding = cases[k].enc};
        s.site = &site;
        TEST_CHECK(vjo_hooks_text(&s, text, sizeof(text)) == strlen("猫\n犬"));
        TEST_CHECK(!strcmp(text, "猫\n犬"));
        TEST_CHECK(vjo_hooks_text(&s, text, 1) == 0 && text[0] == 0);
    }
}

#ifdef VJO_EXTERNAL_HOOK_PROFILES
static void test_profile_fallback(void)
{
    reset(); profile_test_missing = 1;
    vjo_hooks_start("PCSG00415", VJO_TEXT_SOURCE_AUTO, 0);
    TEST_CHECK(profile_test_reads == 1 && !vjo_hook_profile_current());
    TEST_CHECK(n_sites == 0 && n_calls == CALL_IMPORTS && live);
    TEST_CHECK(!vjo_hooks_stop());
    reset();
    vjo_hooks_start(UNLISTED, VJO_TEXT_SOURCE_AUTO, 0);
    TEST_CHECK(profile_test_reads == 1 && n_sites == 0 && n_calls == CALL_IMPORTS);
    TEST_CHECK(!vjo_hooks_stop());
    reset();
    vjo_hooks_start("PCSG00415", VJO_TEXT_SOURCE_OCR, 0);
    TEST_CHECK(profile_test_reads == 0 && !live);
    reset();
    vjo_hooks_start("PCSG00415", VJO_TEXT_SOURCE_AUTO | VJO_TEXT_SOURCE_NO_SITES, 0);
    TEST_CHECK(profile_test_reads == 0 && n_sites == 0 && n_calls == CALL_IMPORTS);
    TEST_CHECK(!vjo_hooks_stop());
    reset();
    vjo_hooks_start("PCSG00415", VJO_TEXT_SOURCE_AUTO, 1);
    TEST_CHECK(profile_test_reads == 0 && !live);
}
#endif
TEST_LIST = {
    {"AFEJ00001 (native e+ r4): its row through the worker, r0's UTF-8 page, bounded", test_eplus_row},
#ifdef VJO_EXTERNAL_HOOK_PROFILES
    {"external profiles: current-title farm excerpts through real capture and filters", test_farm_excerpts},
    {"external profiles: SC3 sites still load their character tables", test_sc3_sites},
    {"external profiles: missing/unlisted CALL fallback, OCR/sites-off/PSP skip disk", test_profile_fallback},
#else
    {"a verified entry at a Luna site's address takes Luna's text processing", test_verified_takes_luna_text},
    {"a verified entry: deref, its own filter in place of a Luna site's", test_verified_deref_and_filter},
    {"a verified entry's APP_VER: only that version, read without Luna sites", test_verified_version},
    {"a verified entry's page record of cells; r15 only with segment 0 at 0x81000000", test_verified_cells},
    {"a verified entry's page-build record of cells: its own count and cells offsets", test_verified_build_cells},
    {"SC3 sites: the string through its end token, the pinned charset, the farm's text", test_sc3_sites},
    {"the farm's excerpts through each verified entry, in the game as the farm recorded its hook", test_farm_excerpts},
    {"TIME TRAVELERS: Luna's sites at their own addresses in a segment 0 at 0x81000000", test_luna_own_address},
    {"clean controls from CALL and table-site streams in every encoding", test_clean_stream_text},
    {"exact build SHA-256: whole executable, worker only, safe failure", test_exact_build_identity},
    {"call_hit: 4..512 bytes, the NUL rules, the pointer-array shape", test_call_hit_bounds},
    {"the CALL hooks: the game's function first, each one's length rule", test_call_hooks},
    {"read_game: the block's bounds, units, a kernel answer that does not hold the address, wrap", test_read_game},
    {"install order: verified first; unported, version, range, entry, window; duplicates", test_install_order},
    {"8 site slots, a duplicate address, taiHEN's refusal", test_site_cap},
    {"the text source word: modes, NO_SITES, OCR for an unknown mode or flag", test_word},
    {"auto: discover CALL text in every native game; retain table-site checks", test_auto_all_native},
    {"the main module (eboot.bin, segment 0) and unresolved imports", test_main_module},
    {"site hits through the observer's C entry", test_site_hits},
    {"CALL slots: replacement, keep, busy, stats", test_replacement_keep},
    {"vjo_hooks_keep: only while the slot holds the caller the job saw", test_keep_key},
    {"vjo_hooks_stop: newest first, once; STOP_FAIL while a thread is inside", test_stop},
    {"APP_VER of param.sfo", test_app_version},
    {"hooks.log names APP_VER for a hook request; the same hooks in both builds", test_app_ver_for_requests},
#endif
    {NULL, NULL},
};
