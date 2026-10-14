/* kernel/meminfo.c, the self-test's three kernel memory routes, with the
 * firmware's functions faked behind module_get_export_func by their 3.65 NIDs
 * and its objects (partition objects, the address space control block) mapped
 * at fixed addresses: in PSVshellPlus's kernel range (below 0x1000000), higher
 * in the kernel half (below 0x40000000, read only inside a kernel memory
 * block), and in the user half (0x40000000 and up, never read). #74's audit
 * found API 19's first guard inverted (only user-half pointers were read): on
 * a console every route would have failed before reading anything. */
#define _GNU_SOURCE
#include "acutest.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <psp2kern/host_stubs.h>
#include <taihen.h>
#include "../../kernel/vjo_kernel.h"
#include "../../kernel/meminfo.c"

VjoKernelState g;
unsigned syscall_depth;
VjoLedger g_kernel_ledger;
static int locks;
int ksceKernelLockMutex(SceUID u, int c, void *t) { (void)u; (void)c; (void)t; locks++; return 0; }
int ksceKernelUnlockMutex(SceUID u, int c) { (void)u; (void)c; locks--; return 0; }

/* ---- the firmware's objects at fixed addresses ---- */
typedef struct { uint32_t magic, total, free; } FakePart; /* what our InfoCore fake reads */
static uintptr_t region;      /* where the objects are mapped */
static int block_known;       /* ksceKernelFindMemBlockByAddr finds the objects' block */
static int block_anywhere;    /* ... or a block at every address (the guard's edges) */
static unsigned core_calls, core_bad, find_calls;
static uintptr_t find_last;

static void *map_at(uintptr_t want)
{
    void *p = mmap((void *)want, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                   -1, 0);
    return p == MAP_FAILED || (uintptr_t)p != want ? NULL : p;
}
/* A free 64 KiB in [lo, hi), or 0. */
static uintptr_t map_in(uintptr_t lo, uintptr_t hi)
{
    for (uintptr_t a = lo; a + 0x10000 <= hi; a += 0x100000)
        if (map_at(a)) return a;
    return 0;
}
static void unmap(uintptr_t a) { if (a) munmap((void *)a, 0x10000); }

#define PART_MAIN 0x100u   /* offsets of the objects in the mapped region */
#define PART_CDRAM 0x200u
#define PART_PHY 0x300u
#define PART_KD 0x400u
#define CAS 0x1000u
#define MB_UID 0x77
static void place(uintptr_t at)
{
    region = at;
    FakePart *m = (FakePart *)(at + PART_MAIN), *c = (FakePart *)(at + PART_CDRAM), *p = (FakePart *)(at + PART_PHY),
             *k = (FakePart *)(at + PART_KD);
    *m = (FakePart){0x50415254, 52428800, 98304};
    *c = (FakePart){0x50415254, 4194304, 1048576};
    *p = (FakePart){0x50415254, 0, 0};
    *k = (FakePart){0x50415254, 37748736, 9437184};
    uint32_t *cas = (uint32_t *)(at + CAS);
    memset(cas, 0, 512);
    cas[CAS_MAIN / 4] = (uint32_t)(at + PART_MAIN);
    cas[CAS_CDRAM / 4] = (uint32_t)(at + PART_CDRAM);
    cas[CAS_PHYCONT / 4] = 0; /* none, as in SceShell */
}

/* sceKernelGetPhyMemPartInfoCoreForKernel */
static int fake_info_core(void *part, PartInfoCore *out)
{
    const FakePart *p = part;
    core_calls++;
    if ((uintptr_t)part < region || (uintptr_t)part >= region + 0x10000 || p->magic != 0x50415254) {
        core_bad++; /* a pointer the guard should have refused */
        return (int)0x80020001u;
    }
    out->total = p->total;
    out->free = p->free;
    return 0;
}
static int cas_override; /* fake_cas returns cas_at instead of the region's control block */
static uintptr_t cas_at;
static void *fake_cas(void) { return (void *)(cas_override ? cas_at : region + CAS); }
static int as_kernel_refused;
static SceUID fake_as_of_pid(SceUID pid)
{
    if (pid == KERNEL_PID && as_kernel_refused) return (int)0x80020005u;
    return pid == KERNEL_PID ? 0x2001 : 0x2002;
}
static int fake_as_get(SceUID asid, void *info)
{
    AsInfo *a = info;
    TEST_CHECK(a->size == sizeof(AsInfo) && (asid == 0x2001 || asid == 0x2002));
    a->phy_mem_part[0] = (uint32_t)(region + PART_KD);
    a->phy_mem_part[3] = (uint32_t)(region + PART_MAIN);
    a->phy_mem_part[5] = 0x40001000u; /* a user-half value: never read */
    return 0;
}
static int missing_core;
int module_get_export_func(SceUID pid, const char *mod, uint32_t lib, uint32_t fn, uintptr_t *out)
{
    TEST_CHECK(pid == KERNEL_PID);
    *out = 0;
    if (!strcmp(mod, "SceSysmem") && lib == 0x02451F0F && fn == 0xB9B69700 && !missing_core) *out = (uintptr_t)fake_info_core;
    else if (!strcmp(mod, "SceSysmem") && lib == 0x3691DA45 && fn == 0x63EBB05B) *out = (uintptr_t)fake_cas;
    else if (!strcmp(mod, "SceProcessmgr") && lib == 0xEB1F8EF7 && fn == 0x9BC44974) *out = (uintptr_t)fake_as_of_pid;
    else if (!strcmp(mod, "SceSysmem") && lib == 0x02451F0F && fn == 0xC3EF4055) *out = (uintptr_t)fake_as_get;
    return *out ? 0 : (int)0x90010002u; /* taiHEN: not found (3.60 NIDs on this "3.65") */
}
/* The module's own block (route 1 kernel part) and the caller's (route 1 caller part). */
SceUID ksceKernelFindMemBlockByAddr(const void *addr, SceSize size)
{
    TEST_CHECK(size == 0);
    find_calls++;
    find_last = (uintptr_t)addr;
    if (addr == (const void *)&g) return MB_UID; /* the module's own block: the kernel's partition */
    if (block_anywhere) return MB_UID + 1;
    if ((uintptr_t)addr >= region && (uintptr_t)addr < region + 0x10000) return block_known ? MB_UID + 1 : -1;
    return (int)0x80024308u;
}
SceUID ksceKernelFindProcMemBlockByAddr(SceUID pid, const void *addr, SceSize size)
{
    TEST_CHECK(size == 0 && pid == 3);
    return addr ? MB_UID + 2 : -1;
}
int ksceKernelMemBlockGetInfoEx(SceUID uid, SceKernelMemBlockInfoEx *info)
{
    TEST_CHECK(info->size == sizeof(*info));
    info->core_info.phy_mem_part = (uint32_t)(region + (uid == MB_UID ? PART_KD : PART_MAIN));
    return uid == MB_UID || uid == MB_UID + 2 ? 0 : (int)0x80020001u;
}

static VjoMemInfo run(void)
{
    static uint32_t out_word;
    VjoMemInfo m;
    part_info = NULL; /* look it up again */
    core_calls = core_bad = 0;
    meminfo_fill(3, &out_word, &m);
    TEST_CHECK(!locks);
    return m;
}

/* Objects where a console has them: every route reads its figures. */
static void kernel_range_objects_are_read(void)
{
    uintptr_t at = map_in(0x00100000u, 0x00F00000u);
    TEST_ASSERT_(at != 0, "no free 64 KiB below 0x1000000 in this process");
    place(at);
    block_known = 0; /* below 0x1000000 the memory-block check is not needed (PSVshellPlus) */
    VjoMemInfo m = run();
    TEST_CHECK(m.kernel_rc == 0 && m.kernel_step == 0 && m.kernel_total == 37748736 && m.kernel_free == 9437184);
    TEST_CHECK(m.caller_rc == 0 && m.caller_step == 0 && m.caller_total == 52428800 && m.caller_free == 98304);
    TEST_CHECK(m.cas_rc == 0 && m.cas_step == 0 && m.cas_main_total == 52428800 && m.cas_main_free == 98304);
    TEST_CHECK(m.cas_cdram_total == 4194304 && m.cas_cdram_free == 1048576 && m.cas_phycont_total == 0);
    TEST_CHECK(m.as_rc == 0 && m.as_parts == 2 && !m.as_caller && m.as_skipped == 1 &&
               m.as_step == VJO_MEMSTEP_RANGE); /* the user-half slot was refused, not read */
    TEST_CHECK(m.as_total[0] == 37748736 && m.as_free[0] == 9437184 && m.as_total[1] == 52428800);
    TEST_CHECK(core_bad == 0);
    TEST_MSG("objects at 0x%08lX: %u InfoCore calls", (unsigned long)at, core_calls);
    unmap(at);
}

/* Higher in the kernel half: read only inside a kernel memory block. */
static void upper_kernel_half_needs_a_block(void)
{
    uintptr_t at = map_in(0x20000000u, 0x3FF00000u);
    TEST_ASSERT_(at != 0, "no free 64 KiB in 0x20000000-0x3FF00000 in this process");
    place(at);
    block_known = 1;
    VjoMemInfo m = run();
    TEST_CHECK(m.kernel_rc == 0 && m.caller_rc == 0 && m.cas_rc == 0 && m.as_rc == 0 && core_bad == 0);
    block_known = 0;
    m = run();
    TEST_CHECK(m.kernel_rc == VJO_ERR_ARG && m.kernel_step == VJO_MEMSTEP_NO_BLOCK);
    TEST_CHECK(m.caller_rc == VJO_ERR_ARG && m.caller_step == VJO_MEMSTEP_NO_BLOCK);
    TEST_CHECK(m.cas_rc == VJO_ERR_ARG && m.cas_step == VJO_MEMSTEP_NO_BLOCK); /* the CAS block itself */
    TEST_CHECK(m.as_rc == VJO_ERR_FORMAT && m.as_parts == 0 && m.as_skipped == 3 && core_calls == 0);
    unmap(at);
}

/* The user half is never handed to InfoCore. */
static void user_half_objects_are_refused(void)
{
    uintptr_t at = map_in(0x50000000u, 0x7FF00000u);
    TEST_ASSERT_(at != 0, "no free 64 KiB in 0x50000000-0x7FF00000 in this process");
    place(at);
    block_known = 1;
    VjoMemInfo m = run();
    TEST_CHECK(m.kernel_rc == VJO_ERR_ARG && m.kernel_step == VJO_MEMSTEP_RANGE);
    TEST_CHECK(m.caller_rc == VJO_ERR_ARG && m.caller_step == VJO_MEMSTEP_RANGE);
    TEST_CHECK(m.cas_rc == VJO_ERR_ARG && m.cas_step == VJO_MEMSTEP_RANGE);
    TEST_CHECK(m.as_rc == VJO_ERR_FORMAT && m.as_parts == 0 && core_calls == 0);
    unmap(at);
}

/* A missing function, a refused kernel address space, null and misaligned pointers. */
static void lookups_fallbacks_and_bad_pointers(void)
{
    uintptr_t at = map_in(0x00100000u, 0x00F00000u);
    TEST_ASSERT_(at != 0, "no free 64 KiB below 0x1000000 in this process");
    place(at);
    as_kernel_refused = 1;
    VjoMemInfo m = run();
    TEST_CHECK(m.as_rc == 0 && m.as_caller == 1 && m.as_parts == 2); /* the caller's address space instead */
    as_kernel_refused = 0;
    ((uint32_t *)(at + CAS))[CAS_MAIN / 4] = 0;
    m = run();
    TEST_CHECK(m.cas_rc == VJO_ERR_ARG && m.cas_step == VJO_MEMSTEP_RANGE && core_bad == 0);
    ((uint32_t *)(at + CAS))[CAS_MAIN / 4] = (uint32_t)(at + PART_MAIN + 2); /* misaligned */
    m = run();
    TEST_CHECK(m.cas_rc == VJO_ERR_ARG && m.cas_step == VJO_MEMSTEP_RANGE && core_bad == 0);
    /* The control block itself is checked, not only the partitions it names: a null
     * one, and one in the user half that names a valid kernel partition. */
    cas_override = 1; cas_at = 0;
    m = run();
    TEST_CHECK(m.cas_rc == VJO_ERR_ARG && m.cas_step == VJO_MEMSTEP_RANGE && !m.cas_main_total && core_bad == 0);
    uintptr_t user = map_in(0x50000000u, 0x7FF00000u);
    TEST_ASSERT_(user != 0, "no free 64 KiB in 0x50000000-0x7FF00000 in this process");
    memset((void *)user, 0, 512);
    ((uint32_t *)user)[CAS_MAIN / 4] = (uint32_t)(at + PART_MAIN);
    cas_at = user;
    m = run();
    TEST_CHECK(m.cas_rc == VJO_ERR_ARG && m.cas_step == VJO_MEMSTEP_RANGE && !m.cas_main_total);
    unmap(user);
    cas_override = 0;
    missing_core = 1;
    m = run();
    TEST_CHECK(m.kernel_rc == (int)0x90010002u && m.kernel_step == VJO_MEMSTEP_CORE && m.cas_step == VJO_MEMSTEP_CORE);
    missing_core = 0;
    unmap(at);
}

/* The guard itself, at each of its edges: 0x1000 < p < 0x40000000, 4-aligned;
 * at or above 0x1000000 only with a kernel memory block (one lookup), below it
 * with no lookup. #75's first guard read p >= 0x40000000 only. */
static void the_guard_at_its_edges(void)
{
    static const struct { uint32_t p; int want; int looked_up; } cases[] = {
        {0x00000000u, VJO_MEMSTEP_RANGE, 0},    {0x00001000u, VJO_MEMSTEP_RANGE, 0},
        {0x00001002u, VJO_MEMSTEP_RANGE, 0},    {0x00001004u, 0, 0},
        {0x00800000u, 0, 0},                    {0x00FFFFFCu, 0, 0},
        {0x01000000u, VJO_MEMSTEP_NO_BLOCK, 1}, {0x20000000u, VJO_MEMSTEP_NO_BLOCK, 1},
        {0x3FFFFFFCu, VJO_MEMSTEP_NO_BLOCK, 1}, {0x3FFFFFFEu, VJO_MEMSTEP_RANGE, 0},
        {0x40000000u, VJO_MEMSTEP_RANGE, 0},    {0x50000000u, VJO_MEMSTEP_RANGE, 0},
        {0x81000000u, VJO_MEMSTEP_RANGE, 0},    {0xFFFFFFFCu, VJO_MEMSTEP_RANGE, 0},
    };
    region = 0; block_known = 0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        for (int anywhere = 0; anywhere < 2; anywhere++) {
            int want = cases[i].want == VJO_MEMSTEP_NO_BLOCK && anywhere ? 0 : cases[i].want;
            block_anywhere = anywhere;
            find_calls = 0;
            int got = kernel_object_ok(cases[i].p);
            TEST_CHECK_(got == want, "0x%08X (a block %s): %d, want %d", (unsigned)cases[i].p,
                        anywhere ? "everywhere" : "nowhere", got, want);
            TEST_CHECK_(find_calls == (unsigned)cases[i].looked_up && (!find_calls || find_last == cases[i].p),
                        "0x%08X: %u lookups", (unsigned)cases[i].p, find_calls);
        }
    }
    block_anywhere = 0;
}

/* x86-64 AddressSanitizer keeps its shadow memory from 0x7FFF8000 up, so a sanitized
 * build cannot map anything at 0x8...: it tests the 0x5... range only. The plain, ARM
 * and CI builds test both. */
#if defined(__SANITIZE_ADDRESS__)
#define VJO_TEST_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define VJO_TEST_ASAN 1
#endif
#endif
#ifdef VJO_TEST_ASAN
#define USER_HALF_RANGES 1u
#else
#define USER_HALF_RANGES 2u
#endif

/* The user half (0x40000000 and up) is never read, even when every lookup would
 * name a block: the objects at 0x5... and at 0x8... are made unreadable, so any
 * read of one faults this test. */
static void user_half_objects_are_never_touched(void)
{
    static const uintptr_t ranges[][2] = {{0x50000000u, 0x7FF00000u}, {0x80000000u, 0xBFF00000u}};
    if (USER_HALF_RANGES < 2u) /* always visible, not only after a failed check (acutest's TEST_MSG) */
        fprintf(stderr, "note: AddressSanitizer build, the 0x8... range is its shadow memory, not tested\n");
    for (unsigned i = 0; i < USER_HALF_RANGES; i++) {
        uintptr_t at = map_in(ranges[i][0], ranges[i][1]);
        TEST_ASSERT_(at != 0, "no free 64 KiB in 0x%08lX-0x%08lX in this process", (unsigned long)ranges[i][0],
                     (unsigned long)ranges[i][1]);
        place(at);
        TEST_ASSERT(mprotect((void *)at, 0x10000, PROT_NONE) == 0);
        block_known = 1; block_anywhere = 1;
        VjoMemInfo m = run();
        TEST_CHECK(m.kernel_rc == VJO_ERR_ARG && m.kernel_step == VJO_MEMSTEP_RANGE);
        TEST_CHECK(m.caller_rc == VJO_ERR_ARG && m.caller_step == VJO_MEMSTEP_RANGE);
        TEST_CHECK(m.cas_rc == VJO_ERR_ARG && m.cas_step == VJO_MEMSTEP_RANGE && !m.cas_main_total);
        TEST_CHECK(m.as_rc == VJO_ERR_FORMAT && m.as_parts == 0 && m.as_skipped == 3 && m.as_step == VJO_MEMSTEP_RANGE);
        TEST_CHECK(core_calls == 0);
        TEST_MSG("objects at 0x%08lX, unreadable", (unsigned long)at);
        block_anywhere = 0;
        unmap(at);
    }
}

TEST_LIST = {
    {"objects in PSVshellPlus's kernel range are read by every route", kernel_range_objects_are_read},
    {"objects higher in the kernel half are read only inside a memory block", upper_kernel_half_needs_a_block},
    {"user-half objects are never read", user_half_objects_are_refused},
    {"lookups, the address-space fallback and bad pointers", lookups_fallbacks_and_bad_pointers},
    {"the guard at each of its edges", the_guard_at_its_edges},
    {"unreadable user-half objects at 0x5... and 0x8... are never touched", user_half_objects_are_never_touched},
    {NULL, NULL}
};
