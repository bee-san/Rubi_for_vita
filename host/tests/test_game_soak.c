/* The game process soaked (#528, mem-leak): the actual game worker (game_ocr/main.c) and the actual kernel
 * transport (kernel/game_ocr.c, capture.c, buffers.c) run OCR job after job in one game process, as on the
 * console, never reset between jobs, with every allocator they use counted by pool and name
 * (host/tests/soak_track.h): the game's USER, CDRAM and phycont blocks, the kernel pool's blocks, the worker's
 * threads and the kernel's file descriptors. Each of the 18 scenarios is a cycle run many times (VJO_SOAK_CYCLES,
 * else a few for ctest): the owner's settings (VOCR6.2 Large, the 1000k LM, the spotter, vertical auto, two
 * helpers; #528's config.ini), Normal with its cascade and the 4000k LM and the gate, Small with nothing, a setting
 * changed at every job, the pause, a blank text box, the same page again (the line cache), the Shell cancelling
 * during the capture and during the model reads, the game short of memory (a smaller size) and out of it (refused),
 * the weights refused after the check (#324), the capture's ladder (CDRAM, phycont), the kernel strip, the game
 * left for over 5 s of its running time (the resident blocks go back), the worker stopped and started, a hooked
 * game (a verified site's eight trusted lines after an OCR read) and the game suspended (PS) and resumed during its
 * job. After
 * the cycle's first run (which may read the resident models), every item must hold exactly what it held then: zero
 * net bytes per cycle in every pool. The game worker's ledger (g_game_ledger) must agree with the blocks counted
 * here, and the kernel's (g_kernel_ledger) with its strips.
 *
 * With the release models (VJO_VOCR_MODEL_DIR, the fast gate's models lane) the jobs recognize the frame's
 * three lines; without them every job ends at its model (VJO_E_OCR_MODEL), after the same memory check, blocks
 * and capture. No device evidence: the pools are models (CLANNAD's measured free USER, #74's kernel pool). */
#include "acutest.h"
#include <stdlib.h>
#include <psp2/host_stubs.h>
#include <psp2kern/host_stubs.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <taihen.h>
#include "../../kernel/vjo_kernel.h"
#include "../../kernel/game_ocr.h"
#include "../../include/vjo_vocr_sizes.h"
#include "../../game_ocr/main.c"
#include "soak_track.h"

#define FB_W 960u
#define FB_H 544u
#define USER_FREE_CLANNAD 91226112 /* docs/vita-memory.md */
#define KD_GRANTED_74 458752u      /* #74's earlier session: the largest kernel block granted */
#define NO_PAGE ((int)0x80024302u)
#define GAME_PID 7
#define SHELL_PID 3

VjoKernelState g;
unsigned syscall_depth;
static SceUID caller;
static uint32_t job_seq; /* the job running: the one the Shell cancels */
static uint64_t clock_us;
static uint32_t frame[FB_W * FB_H];
static int in_hook;

/* ---- the pools, and every block in them ---- */
static int64_t user_free, cdram_free, phycont_free, user_used, cdram_used, phycont_used;
static uint32_t kd_largest;
static unsigned refuse_weights;   /* the next weights blocks refused (#324: after a passed check) */
static unsigned refuse_capture;   /* bits: the capture block refused in 1 USER, 2 CDRAM, 4 phycont (its ladder) */
static int hook_copy_fails;       /* the display hook cannot write the game's memory: the kernel strip's rung */
static unsigned capture_takes;    /* VjoGameVocrCapture blocks taken (any pool) */
typedef struct {
    SceUID uid;
    void *base;
    size_t bytes;
    int type, kernel;
    char name[32];
} Block;
#define BLOCKS 32
static Block blocks[BLOCKS];
static SceUID next_uid = 100;

static const char *pool_of(int type, int kernel)
{
    if (kernel) return "kernel KD";
    return type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW ? "game CDRAM"
           : type == SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW ? "game PHYCONT" : "game USER";
}
static int64_t *used_of(int type)
{
    return type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW ? &cdram_used
           : type == SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW ? &phycont_used : &user_used;
}
static int64_t free_of(int type)
{
    return type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW ? cdram_free
           : type == SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW ? phycont_free : user_free;
}
static Block *find_uid(SceUID uid)
{
    for (unsigned i = 0; i < BLOCKS; i++)
        if (blocks[i].base && blocks[i].uid == uid) return &blocks[i];
    return NULL;
}
static SceUID add_block(const char *name, size_t bytes, int type, int kernel)
{
    for (unsigned i = 0; i < BLOCKS; i++)
        if (!blocks[i].base) {
            if (posix_memalign(&blocks[i].base, 4096, bytes)) return NO_PAGE;
            memset(blocks[i].base, 0xA5, bytes); /* nothing relies on zeroed memory */
            blocks[i].bytes = bytes;
            blocks[i].type = type;
            blocks[i].kernel = kernel;
            blocks[i].uid = next_uid++;
            snprintf(blocks[i].name, sizeof(blocks[i].name), "%s", name);
            soak_take(pool_of(type, kernel), name, (long long)bytes);
            return blocks[i].uid;
        }
    TEST_CHECK_(0, "more than %d blocks held at once", BLOCKS);
    return NO_PAGE;
}
static int drop_block(Block *b)
{
    TEST_CHECK_(!soak_give(pool_of(b->type, b->kernel), b->name, (long long)b->bytes), "%s freed twice", b->name);
    free(b->base);
    memset(b, 0, sizeof(*b));
    return 0;
}

/* ---- the game's native hooks (game_ocr/hooks.h's job-time half; host/tests/hooks_none.c's answers but here) ----
 * Nothing is installed but in the hooked game's scenario: there one Rubi verified table site (stream HOOK_SITE) holds
 * the game's current line, as vjo_hooks_start installs it in such a game (host/tests/test_game_hooks.c has the
 * engine's every case); while a game thread writes it (hook_busy) its snapshot fails and the job reads the screen.
 * The engine itself allocates nothing (game_ocr/hooks.c, core/hook_*.c: static state; its taiHEN hooks are taken at
 * the worker's start and released at its stop), so the soak counts what the worker takes around it. */
#define HOOK_SITE 2u
static int hooks_on, hook_busy;
static uint32_t hook_seq;
static const char *hook_text;
static unsigned hook_snaps;
void vjo_hooks_counts(uint32_t *mode, uint32_t *installed, uint32_t *refused)
{
    *mode = hooks_on ? VJO_TEXT_SOURCE_AUTO : 0u;
    *installed = hooks_on ? 1u << 16 | 1u : 0u;
    *refused = 0;
}
int vjo_hooks_verified(uint32_t id) { return hooks_on && id == HOOK_SITE; }
int vjo_hooks_snap(uint32_t id, VjoHookSnap *out)
{
    if (!hooks_on || id != HOOK_SITE || hook_busy || !hook_text) return 0;
    hook_snaps++;
    memset(out, 0, sizeof(*out));
    out->id = id;
    out->kind = VJO_HOOK_KIND_SITE;
    out->seq = hook_seq;
    out->hits = 1;
    out->encoding = VJO_HOOK_ENC_UTF8;
    out->n = (uint32_t)strlen(hook_text) < VJO_HOOK_RAW_BYTES ? (uint32_t)strlen(hook_text) : VJO_HOOK_RAW_BYTES;
    memcpy(out->raw, &hook_text, sizeof(hook_text)); /* the fake's raw bytes: the text's address */
    return 1;
}
uint32_t vjo_hooks_seq(uint32_t id, uint32_t *key, uint32_t *kind, uint32_t *bytes)
{
    if (!hooks_on || id != HOOK_SITE || !hook_text) return 0;
    *key = 0;
    *kind = VJO_HOOK_KIND_SITE;
    *bytes = (uint32_t)strlen(hook_text);
    return hook_seq;
}
void vjo_hooks_keep(uint32_t id, uint32_t key) { (void)id; (void)key; }
void vjo_hooks_call_stats(uint32_t *dropped, uint32_t *replaced) { *dropped = *replaced = 0; }
size_t vjo_hooks_text(const VjoHookSnap *s, char *out, size_t cap)
{
    const char *t;
    size_t n;
    memcpy(&t, s->raw, sizeof(t));
    n = strlen(t) < cap ? strlen(t) : cap - 1u;
    memcpy(out, t, n);
    out[n] = '\0';
    return n;
}

/* the game */
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *out)
{
    out->size_user = (int)(user_free - user_used);
    out->size_cdram = (int)(cdram_free - cdram_used);
    out->size_phycont = (int)(phycont_free - phycont_used);
    return 0;
}
SceUID sceKernelAllocMemBlock(const char *name, int type, unsigned n, void *opt)
{
    (void)opt;
    TEST_CHECK_(caller == GAME_PID, "%s allocated outside the game", name);
    if (!strcmp(name, "VjoGameVocrWeights") && refuse_weights) {
        refuse_weights--;
        return NO_PAGE;
    }
    if (!strcmp(name, "VjoGameVocrCapture") &&
        (refuse_capture & (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW ? 2u
                           : type == SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW ? 4u : 1u)))
        return NO_PAGE;
    if (*used_of(type) + n > free_of(type)) return NO_PAGE;
    SceUID uid = add_block(name, n, type, 0);
    if (uid >= 0) *used_of(type) += n;
    if (uid >= 0 && !strcmp(name, "VjoGameVocrCapture")) capture_takes++;
    return uid;
}
int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    Block *b = find_uid(uid);
    if (!b || b->kernel) return -1;
    *base = b->base;
    return 0;
}
int sceKernelFreeMemBlock(SceUID uid)
{
    Block *b = find_uid(uid);
    if (!b || b->kernel) return -1;
    *used_of(b->type) -= (int64_t)b->bytes;
    return drop_block(b);
}
/* the kernel pool */
SceUID ksceKernelAllocMemBlock(const char *name, unsigned type, SceSize bytes, void *opt)
{
    (void)opt;
    TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW);
    if (bytes > kd_largest) return NO_PAGE;
    return add_block(name, bytes, (int)type, 1);
}
int ksceKernelGetMemBlockBase(SceUID uid, void **base)
{
    Block *b = find_uid(uid);
    if (!b || !b->kernel) return -1;
    *base = b->base;
    return 0;
}
int ksceKernelFreeMemBlock(SceUID uid)
{
    Block *b = find_uid(uid);
    if (!b || !b->kernel) return -1;
    return drop_block(b);
}
/* The worker's blocks are the game process's memory. */
SceUID ksceKernelFindProcMemBlockByAddr(SceUID pid, const void *addr, SceSize size)
{
    const unsigned char *p = addr;
    (void)size;
    if (pid != GAME_PID) return -1;
    for (unsigned i = 0; i < BLOCKS; i++)
        if (blocks[i].base && !blocks[i].kernel && p >= (unsigned char *)blocks[i].base &&
            p < (unsigned char *)blocks[i].base + blocks[i].bytes)
            return blocks[i].uid;
    return -1;
}
SceUID ksceKernelFindMemBlockByAddr(const void *addr, SceSize size) { (void)addr; (void)size; return 40; }

/* ---- the kernel transport, behind the retail return model (test_alloc_sim.c's) ---- */
int test_kernel_register(void);
int test_kernel_submit(const VjoGameOcrRequest *request);
int test_kernel_take(VjoGameOcrRequest *out);
int test_kernel_capture_rows(const VjoGameOcrRows *request, void *dst, VjoGameOcrRowsInfo *info);
int test_kernel_cancelled(uint32_t seq);
int test_kernel_complete(const VjoGameOcrResult *result);
int test_kernel_read(uint32_t seq, VjoGameOcrResult *out);
int test_kernel_cancel(uint32_t seq);
int test_kernel_model_open(uint32_t seq, uint32_t which, VjoGameOcrModelInfo *info);
int test_kernel_model_read(uint32_t seq, uint32_t off, uint32_t bytes, void *dst);
int test_kernel_model_close(uint32_t seq);
int test_kernel_shot_write(uint32_t seq, uint32_t off, uint32_t bytes, const void *src);
int test_kernel_shot_save(uint32_t seq, uint32_t bytes);
static int retail_return(int rc) { return rc < 0 ? (int32_t)((uint32_t)rc & ~0x40000000u) : rc; }
int vjoBootInfo(VjoBootInfo *out) { out->mode = VJO_BOOT_NORMAL; return 0; }
int vjoOcrRegister(void) { return retail_return(test_kernel_register()); }
int vjoOcrSubmit(const VjoGameOcrRequest *r) { return retail_return(test_kernel_submit(r)); }
int vjoOcrTake(VjoGameOcrRequest *out) { return retail_return(test_kernel_take(out)); }
int vjoOcrCaptureRows(const VjoGameOcrRows *request, void *dst, VjoGameOcrRowsInfo *info)
{ return retail_return(test_kernel_capture_rows(request, dst, info)); }
int vjoOcrCancelled(uint32_t seq) { return retail_return(test_kernel_cancelled(seq)); }
int vjoOcrComplete(const VjoGameOcrResult *r) { return retail_return(test_kernel_complete(r)); }
int vjoOcrRead(uint32_t seq, VjoGameOcrResult *out) { return retail_return(test_kernel_read(seq, out)); }
int vjoOcrCancel(uint32_t seq) { return retail_return(test_kernel_cancel(seq)); }
int vjoOcrModelOpen(uint32_t seq, uint32_t which, VjoGameOcrModelInfo *info)
{ return retail_return(test_kernel_model_open(seq, which, info)); }
int vjoOcrModelRead(uint32_t seq, uint32_t off, uint32_t bytes, void *dst)
{ return retail_return(test_kernel_model_read(seq, off, bytes, dst)); }
int vjoOcrModelClose(uint32_t seq) { return retail_return(test_kernel_model_close(seq)); }
int vjoOcrShotWrite(uint32_t seq, uint32_t off, uint32_t bytes, const void *src)
{ return retail_return(test_kernel_shot_write(seq, off, bytes, src)); }
int vjoOcrShotSave(uint32_t seq, uint32_t bytes) { return retail_return(test_kernel_shot_save(seq, bytes)); }
int vjoMemInfo(VjoMemInfo *out) { (void)out; return VJO_ERR_PERM; }
void klog(const char *format, ...) { (void)format; }
SceUID ksceKernelGetProcessId(void) { return caller; }
int ksceKernelGetThreadId(void) { return 0x7001; } /* the game worker's one thread */
uint64_t ksceKernelGetSystemTimeWide(void) { return clock_us; }
int ksceKernelSetEventFlag(SceUID uid, unsigned bits) { (void)uid; (void)bits; return 0; }
int ksceKernelClearEventFlag(SceUID uid, unsigned bits) { (void)uid; (void)bits; return 0; }
int ksceKernelDelayThread(SceUInt us) { clock_us += us; return 0; }
/* the Shell cancels the job at this display call of it (0: never); the game is suspended and resumed at this one */
static unsigned cancel_at_frame, suspend_at_frame, frames_this_job;
static void suspend_and_resume(void);
int ksceKernelWaitEventFlag(SceUID uid, unsigned bits, unsigned wait, unsigned *out, SceUInt *timeout)
{
    (void)uid; (void)wait; (void)out;
    if (bits == MEV_ROWS) { /* the game shows its next frame: the display hook runs in its thread */
        ++frames_this_job;
        if (cancel_at_frame && frames_this_job == cancel_at_frame) {
            SceUID saved = caller;
            caller = SHELL_PID;
            TEST_CHECK(vjoOcrCancel(job_seq) == 0);
            caller = saved;
        }
        if (suspend_at_frame && frames_this_job == suspend_at_frame) suspend_and_resume();
        in_hook = 1;
        game_ocr_rows_hook(GAME_PID, (uintptr_t)frame, FB_W, 0x00000000u, FB_W, FB_H);
        in_hook = 0;
        clock_us += 16667;
        return 0;
    }
    {
        SceUID saved = caller;
        unsigned depth = syscall_depth;
        caller = 0;
        syscall_depth = 0;
        game_ocr_model_tick();
        caller = saved;
        syscall_depth = depth;
    }
    clock_us += *timeout;
    return 0;
}
int ksceKernelLockMutex(SceUID uid, int n, void *timeout) { (void)uid; (void)n; (void)timeout; return 0; }
int ksceKernelUnlockMutex(SceUID uid, int n) { (void)uid; (void)n; return 0; }
int taiGetModuleInfoForKernel(SceUID pid, const char *name, tai_module_info_t *info)
{ (void)pid; (void)name; info->modid = 99; return 0; }
int ksceKernelMemcpyUserToKernel(void *dst, const void *src, SceSize n) { if (!dst || !src) return -1; memcpy(dst, src, n); return 0; }
int ksceKernelMemcpyKernelToUser(void *dst, const void *src, SceSize n)
{
    if (!dst || !src || (in_hook && hook_copy_fails)) return -1;
    memcpy(dst, src, n);
    return 0;
}

/* ---- the kernel's files: the release models, read only, each descriptor counted ---- */
#define FILES 4
static FILE *files[FILES];
static int fd_index(SceUID fd) { return fd >= 31 && fd < 31 + FILES && files[fd - 31] ? fd - 31 : -1; }
int ksceIoGetstatByFd(SceUID fd, SceIoStat *st)
{
    if (fd_index(fd) < 0) return -1;
    memset(st, 0, sizeof(*st));
    st->st_mtime = (SceDateTime){2026, 10, 13, 0, 0, 0, 0};
    return 0;
}
SceUID ksceIoOpen(const char *path, int flags, unsigned mode)
{
    char local[1024];
    (void)mode;
    /* a model: VJO_VOCR_MODEL_ROOT/<name>, read only; anything else (the screenshot's spare) is not there */
    if (!VJO_VOCR_MODEL_DIR[0] || (flags & SCE_O_WRONLY) || strncmp(path, VJO_VOCR_MODEL_ROOT "/", sizeof(VJO_VOCR_MODEL_ROOT)))
        return (int)0x80010002u;
    snprintf(local, sizeof(local), "%s/%s", VJO_VOCR_MODEL_DIR, path + sizeof(VJO_VOCR_MODEL_ROOT));
    for (int i = 0; i < FILES; i++)
        if (!files[i]) {
            if (!(files[i] = fopen(local, "rb"))) return (int)0x80010002u;
            soak_take("kernel fds", "model file", 1);
            return 31 + i;
        }
    TEST_CHECK_(0, "more than %d kernel files open", FILES);
    return (int)0x80010024u;
}
SceOff ksceIoLseek(SceUID fd, SceOff off, int whence)
{
    int i = fd_index(fd);
    if (i < 0) return -1;
    fseek(files[i], (long)off, whence);
    return (SceOff)ftell(files[i]);
}
/* the Shell cancels the job at this model read of it (0: never); the game is suspended and resumed at this one */
static unsigned cancel_at_read, suspend_at_read, reads_this_job;
int ksceIoPread(SceUID fd, void *out, SceSize n, SceOff off)
{
    int i = fd_index(fd);
    if (i < 0) return -1;
    ++reads_this_job;
    if (cancel_at_read && reads_this_job == cancel_at_read) {
        SceUID saved = caller;
        caller = SHELL_PID;
        TEST_CHECK(vjoOcrCancel(job_seq) == 0);
        caller = saved;
    }
    if (suspend_at_read && reads_this_job == suspend_at_read) suspend_and_resume();
    fseek(files[i], (long)off, SEEK_SET);
    return (int)fread(out, 1, n, files[i]);
}
int ksceIoClose(SceUID fd)
{
    int i = fd_index(fd);
    if (i < 0) return -1;
    fclose(files[i]);
    files[i] = NULL;
    TEST_CHECK(!soak_give("kernel fds", "model file", 1));
    return 0;
}
/* No screenshot runs here: its file calls never come. */
int ksceIoPwrite(SceUID fd, const void *p, SceSize n, SceOff off) { (void)fd; (void)p; (void)n; (void)off; TEST_CHECK(0); return -1; }
int ksceIoRemove(const char *path) { (void)path; TEST_CHECK(0); return -1; }
int ksceIoRename(const char *a, const char *b) { (void)a; (void)b; TEST_CHECK(0); return -1; }
int ksceIoSync(const char *dev, unsigned int unk) { (void)dev; (void)unk; TEST_CHECK(0); return -1; }
int ksceIoSyncByFd(SceUID fd, int flag) { (void)fd; (void)flag; TEST_CHECK(0); return -1; }

/* ---- the game's SDK, its threads counted ---- */
SceUID sceKernelGetProcessId(void) { return GAME_PID; }
uint64_t sceKernelGetProcessTimeWide(void) { return clock_us++; }
/* worker_main's loop ends after this many of its sleeps (0: never; the job calls never sleep here) */
static unsigned loop_sleeps;
int sceKernelDelayThread(unsigned us)
{
    SceUID saved = caller;
    clock_us += us;
    caller = 0;
    game_ocr_model_tick();
    caller = saved;
    if (loop_sleeps && !--loop_sleeps) __atomic_store_n(&quitting, 1, __ATOMIC_RELEASE);
    return 0;
}
typedef struct { SceUID uid; long long bytes; char name[32]; } Thread;
static Thread threads[8];
static SceUID next_thread = 200;
SceUID sceKernelCreateThread(const char *name, int (*entry)(SceSize, void *), int p, unsigned stack, unsigned a, int cpu,
                             void *opt)
{
    (void)entry; (void)p; (void)a; (void)cpu; (void)opt;
    for (unsigned i = 0; i < 8; i++)
        if (!threads[i].uid) {
            threads[i].uid = next_thread++;
            threads[i].bytes = (long long)stack + 4096; /* the stack and its TLS page */
            snprintf(threads[i].name, sizeof(threads[i].name), "%s", name);
            soak_take("game threads", name, threads[i].bytes);
            return threads[i].uid;
        }
    TEST_CHECK_(0, "more than 8 threads");
    return -1;
}
static Thread *find_thread(SceUID uid)
{
    for (unsigned i = 0; i < 8; i++)
        if (threads[i].uid == uid) return &threads[i];
    return NULL;
}
/* A helper runs its line at once (a real thread would have ended before its join); the worker's own thread is
 * this test, which calls its job and loop functions itself. */
int sceKernelStartThread(SceUID uid, SceSize n, void *args)
{
    Thread *t = find_thread(uid);
    if (!t) return -1;
    if (!strcmp(t->name, "VjoGameVocrHelper")) return helper_entry(n, args);
    return 0;
}
int sceKernelWaitThreadEnd(SceUID uid, int *status, void *timeout) { (void)status; (void)timeout; return find_thread(uid) ? 0 : -1; }
int sceKernelDeleteThread(SceUID uid)
{
    Thread *t = find_thread(uid);
    if (!t) return -1;
    TEST_CHECK(!soak_give("game threads", t->name, t->bytes));
    memset(t, 0, sizeof(*t));
    return 0;
}
int sceKernelGetThreadInfo(SceUID thid, SceKernelThreadInfo *info) { (void)thid; (void)info; return -1; }
SceUID sceKernelGetThreadId(void) { return 21; }
int scePowerGetArmClockFrequency(void) { return 333; }
int scePowerSetArmClockFrequency(int mhz) { (void)mhz; return 0; }
int sceKernelGetProcessTitleId(SceUID pid, char *out, SceSize size) { (void)pid; snprintf(out, size, "PCSG00415"); return 0; }
int sceAppMgrAppParamGetString(int pid, int param, char *out, SceSize size) { (void)pid; (void)param; (void)out; (void)size; return -1; }
int vjoGetVersion(void) { return VJO_API_VERSION; }
SceUID sceIoOpen(const char *path, int flags, unsigned mode) { (void)path; (void)flags; (void)mode; return (int)0x80010013u; } /* no ux0 */
int sceDisplayGetFrameBuf(SceDisplayFrameBuf *fb, int sync) { (void)fb; (void)sync; return -1; }
int sceKernelGetMemBlockInfoByAddr(void *base, SceKernelMemBlockInfo *info) { (void)base; (void)info; return -1; }

/* ---- the frame: a VN screen, a dark text box with three lines of glyph-like strokes (test_alloc_sim.c's) ---- */
static uint32_t seed;
static unsigned rnd(unsigned n) { seed = seed * 1103515245u + 12345u; return (seed >> 8) % n; }
static void box(unsigned x0, unsigned y0, unsigned x1, unsigned y1, uint32_t abgr)
{
    for (unsigned y = y0; y < y1; y++)
        for (unsigned x = x0; x < x1; x++) frame[y * FB_W + x] = abgr;
}
static void glyph_line(unsigned x0, unsigned y0, unsigned x1, unsigned gh, uint32_t abgr)
{
    for (unsigned x = x0; x + gh <= x1; x += gh + gh / 6) {
        unsigned strokes = 3 + rnd(4), t = gh / 10 ? gh / 10 : 1;
        for (unsigned k = 0; k < strokes; k++) {
            if (rnd(2)) { unsigned yy = y0 + rnd(gh - t); box(x, yy, x + gh, yy + t, abgr); }
            else { unsigned xx = x + rnd(gh - t); box(xx, y0, xx + t, y0 + gh, abgr); }
        }
    }
}
/* page: another page of text each time (a new seed), so that the line cache does not answer every line; 0: the
 * text box with no text */
static void make_frame(unsigned page)
{
    seed = 74;
    for (unsigned y = 0; y < FB_H; y++)
        for (unsigned x = 0; x < FB_W; x++)
            frame[y * FB_W + x] = 0xFF000000u | 150u << 16 | (100 + x * 60 / FB_W) << 8 | (70 + y * 100 / FB_H + rnd(9));
    box(20, 380, 940, 536, 0xFF221812u);
    if (!page) return;
    seed = 74u + page * 7919u;
    glyph_line(44, 404, 900, 22, 0xFFF0F0F0u);
    glyph_line(44, 440, 760, 22, 0xFF96ECFAu);
    glyph_line(44, 476, 620, 22, 0xFFECE4E4u);
}

/* ---- the game process: the kernel's state for it once, then job after job ---- */
static void game_starts(void)
{
    memset(&g, 0, sizeof(g));
    game_ocr_init();
    clock_us = 1000000;
    g.lock = g.ievf = 1;
    g.evf = 2;
    g.mevf = 5;
    g.shell_pid = SHELL_PID;
    g.game_pid = GAME_PID;
    g.game_active = VJO_GAME;
    g.ocr_epoch = 1;
    g.fb_w = FB_W;
    g.fb_h = FB_H;
    g.done_seq = g.capture_seq = 11;
    g.region = (VjoRect){1536, 48188, 62600, 15059}; /* a CLANNAD-sized text box */
    user_free = USER_FREE_CLANNAD;
    cdram_free = 128 << 20;
    phycont_free = 16 << 20;
    kd_largest = KD_GRANTED_74;
    caller = GAME_PID;
    if (!g_game_ledger.defs) ledger_init();
    kledger_init();
}

/* The request the Shell makes (shell/worker.c vocr_run_one) for a size (1-3), its LM tier (0: off), its cascade
 * (ms, 0: off) and its speed word. */
static VjoGameOcrRequest shell_request(unsigned tier, unsigned lm, unsigned cascade_ms, uint32_t speed)
{
    VjoGameOcrRequest r = {.size = sizeof(r), .tiers = (1u << tier) - 1u, .speed = speed,
                           .hook_pick = VJO_HOOK_PICK_AUTO};
    strcpy(r.model_dir, VJO_VOCR_MODEL_ROOT);
    snprintf(r.model, sizeof(r.model), "%s", tier == 3 ? VJO_VOCR_TIER3_NAME : tier == 2 ? VJO_VOCR_TIER2_NAME
                                                                                        : VJO_VOCR_TIER1_NAME);
    r.flags = lm << VJO_GAME_OCR_LM_SHIFT | 2u << VJO_GAME_OCR_REFINE_SHIFT | /* ocr_refine_lines = psp */
              15u << VJO_GAME_OCR_SETTLE_SHIFT | 40u << VJO_GAME_OCR_SETTLE_MAX_SHIFT; /* 150 ms, at most 2 s */
    if (cascade_ms) r.flags |= ((cascade_ms + 125u) / 250u) << VJO_GAME_OCR_CASCADE_SHIFT;
    return r;
}
/* config.ini's defaults beside the size: helpers 2, the 444 MHz boost, trusted hooks, the head gate and the
 * two-level head, the dropped borders, the line finder's fixes */
#define SPEED_BASE (2u | VJO_GAME_OCR_S_BOOST | VJO_GAME_OCR_S_TRUST | VJO_GAME_OCR_S_HEAD_GATE | \
                    VJO_GAME_OCR_S_TWO_LEVEL | VJO_GAME_OCR_S_DROP_BORDERS | VJO_GAME_OCR_S_LINE_FIX)
#define SPEED_SPOT (VJO_GAME_OCR_S_SPOT | VJO_GAME_OCR_S_SPOT_REFINE)
#define SPEED_VAUTO (VJO_GAME_OCR_VERTICAL_AUTO << VJO_GAME_OCR_S_VERTICAL_SHIFT)

/* One job, as the console runs it: the Shell submits, the worker takes, runs and completes it, the Shell reads the
 * result. The worker's state carries over from job to job. */
static VjoGameOcrResult job_out;
static unsigned shell_cancelled; /* jobs whose result the Shell read as cancelled */
static int job(VjoGameOcrRequest request)
{
    VjoGameOcrRequest taken;
    int seq, rc, done = 0;
    caller = GAME_PID;
    TEST_ASSERT(vjoOcrRegister() == 0);
    caller = SHELL_PID;
    seq = vjoOcrSubmit(&request);
    TEST_ASSERT_(seq > 0, "submit: %d", seq);
    job_seq = (uint32_t)seq;
    caller = GAME_PID;
    memset(&taken, 0, sizeof(taken));
    taken.size = sizeof(taken);
    TEST_ASSERT(!vjoOcrTake(&taken));
    frames_this_job = reads_this_job = 0;
    claim_outstanding = 1;
    rc = vjo_game_ocr_run_request(&taken, &job_out);
    /* as worker_main: a completion the kernel refuses (a model file still closing) is tried again after 50 ms */
    for (unsigned k = 0; k < 100 && !done; k++) {
        done = !vjoOcrComplete(&job_out) && !job_out.cleanup_status;
        if (!done) sceKernelDelayThread(50000);
    }
    TEST_CHECK_(done, "the job's completion was refused for 5 s (rc %d)", rc);
    claim_outstanding = 0;
    caller = SHELL_PID;
    {
        VjoGameOcrResult shell;
        int r = vjoOcrRead(job_seq, &shell);
        TEST_CHECK_(r == 0 || r == VJO_ERR_CANCELLED, "read: %d", r);
        shell_cancelled += r == VJO_ERR_CANCELLED;
    }
    caller = GAME_PID;
    /* between jobs: no job block, no helper, no file, no kernel strip */
    TEST_CHECK_(workspace_uid < 0 && capture_uid < 0 && line_uid < 0 && frame_uid < 0 && shot_uid < 0,
                "a job block held after the job (rc %d)", rc);
    return rc;
}

/* The ledgers agree with the blocks counted here: the game worker's (its module and stack from the budget, its
 * blocks as they are made) and the kernel's (its strip and policy read). */
static void ledgers_agree(const char *when)
{
    long long user = 0, kd = 0;
    for (unsigned i = 0; i < BLOCKS; i++)
        if (blocks[i].base) {
            if (blocks[i].kernel) kd += (long long)blocks[i].bytes;
            else user += (long long)blocks[i].bytes;
        }
    long long ledger = (long long)g_game_ledger.pool_current[VJO_LEDGER_POOL_GAME] - VJO_BUDGET_GAME_MODULE_PAGES -
                       (worker >= 0 ? VJO_BUDGET_GAME_STACK : 0) -
                       (long long)g_game_ledger.current[GL_HELPERS];
    TEST_CHECK_(ledger == user, "%s: the game ledger holds %lld B of blocks, the pools %lld B", when, ledger, user);
    TEST_CHECK_(g_game_ledger.current[GL_HELPERS] == 0, "%s: helper stacks %u B in the ledger", when,
                (unsigned)g_game_ledger.current[GL_HELPERS]);
    TEST_CHECK_((long long)vjo_ledger_current(&g_kernel_ledger, VJO_KL_OCR_STRIP) == kd && kd == 0,
                "%s: the kernel ledger's strip %u B, the kernel pool %lld B", when,
                (unsigned)vjo_ledger_current(&g_kernel_ledger, VJO_KL_OCR_STRIP), kd);
}

static int have_models(void)
{
    char path[1024];
    static const char *const names[] = {VJO_VOCR_TIER1_NAME, VJO_VOCR_TIER2_NAME, VJO_VOCR_TIER3_NAME,
                                        VJO_VOCR_LM2_NAME, VJO_VOCR_SPOT_NAME};
    FILE *f;
    if (!VJO_VOCR_MODEL_DIR[0]) return 0;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", VJO_VOCR_MODEL_DIR, names[i]);
        if (!(f = fopen(path, "rb"))) return 0;
        fclose(f);
    }
    return 1;
}

/* ---- the scenarios ---- */
typedef struct {
    const char *name;
    void (*cycle)(unsigned i); /* one cycle: its jobs and events */
} Scenario;
static unsigned page_no = 1, models_ok;
static unsigned recognized, cycles_run;

static void note_job(int rc)
{
    if (rc == VJO_OK && job_out.lines) recognized++;
}
static void owner_job(unsigned i)
{
    make_frame(page_no++);
    note_job(job(shell_request(3, 2, 2000, SPEED_BASE | SPEED_SPOT | SPEED_VAUTO)));
    (void)i;
}
static void normal_cascade_job(unsigned i)
{
    make_frame(page_no++);
    note_job(job(shell_request(2, 4, 2000, SPEED_BASE | VJO_GAME_OCR_S_GATE)));
    (void)i;
}
static void small_plain_job(unsigned i)
{
    make_frame(page_no++);
    note_job(job(shell_request(1, 0, 0, 0)));
    (void)i;
}
/* a setting changed at every job (one cycle: four jobs, each another setting): the resident blocks of the one before
 * are given back, others read */
static void changing_job(unsigned i)
{
    owner_job(i);
    normal_cascade_job(i);
    small_plain_job(i);
    make_frame(page_no++);
    note_job(job(shell_request(3, 4, 0, SPEED_BASE | VJO_GAME_OCR_S_GATE | SPEED_VAUTO)));
}
static void paused_job(unsigned i)
{
    make_frame(page_no++);
    note_job(job(shell_request(2, 2, 2000, SPEED_BASE | SPEED_SPOT | VJO_GAME_OCR_S_PAUSE)));
    (void)i;
}
static void blank_job(unsigned i)
{
    make_frame(0);
    note_job(job(shell_request(3, 2, 2000, SPEED_BASE | SPEED_SPOT | SPEED_VAUTO)));
    (void)i;
}
static void same_page_job(unsigned i)
{
    make_frame(1); /* the same page again: its lines from the line cache */
    note_job(job(shell_request(3, 2, 2000, SPEED_BASE | SPEED_SPOT | SPEED_VAUTO)));
    (void)i;
}
/* the Shell cancels each job at its 1st, 2nd, ... 6th display call */
static void cancelled_job(unsigned i)
{
    make_frame(page_no++);
    cancel_at_frame = 1u + i % 6u;
    (void)job(shell_request(3, 2, 2000, SPEED_BASE | SPEED_SPOT | SPEED_VAUTO));
    cancel_at_frame = 0;
}
/* the game short of memory: a smaller size fits once the resident blocks are counted as given back (it reads
 * with it), then a job of the owner's again */
static void short_of_memory_job(unsigned i)
{
    make_frame(page_no++);
    user_free = (int64_t)user_used + 1536 * 1024;
    note_job(job(shell_request(3, 2, 2000, SPEED_BASE | SPEED_SPOT | SPEED_VAUTO)));
    user_free = USER_FREE_CLANNAD;
    owner_job(i);
}
/* nothing fits beside the reserve (no resident block to give back): the job is refused, then one fits again */
static void no_memory_job(unsigned i)
{
    make_frame(page_no++);
    TEST_CHECK(!drop_resident());
    user_free = (int64_t)user_used + 512 * 1024;
    TEST_CHECK(job(shell_request(3, 2, 2000, SPEED_BASE | SPEED_SPOT | SPEED_VAUTO)) == VJO_E_OOM);
    user_free = USER_FREE_CLANNAD;
    owner_job(i);
}
/* the capture's ladder: USER refuses its block (CDRAM takes it), then USER and CDRAM (phycont takes it) */
static void ladder_job(unsigned i)
{
    make_frame(page_no++);
    refuse_capture = i & 1u ? 3u : 1u;
    note_job(job(shell_request(3, 2, 2000, SPEED_BASE | SPEED_SPOT | SPEED_VAUTO)));
    refuse_capture = 0;
}
/* the display hook cannot write the game's memory: each strip goes through the kernel pool's strip */
static void kernel_strip_job(unsigned i)
{
    make_frame(page_no++);
    hook_copy_fails = 1;
    note_job(job(shell_request(3, 2, 2000, SPEED_BASE | SPEED_SPOT | SPEED_VAUTO)));
    hook_copy_fails = 0;
    (void)i;
}
/* the Shell cancels while the kernel reads the models (their 1st to 4th chunk) */
static void cancelled_read_job(unsigned i)
{
    make_frame(page_no++);
    TEST_CHECK(!drop_resident());
    cancel_at_read = 1u + i % 4u;
    (void)job(shell_request(3, 2, 2000, SPEED_BASE | SPEED_SPOT | SPEED_VAUTO));
    cancel_at_read = 0;
    owner_job(i);
}
/* #324: the check passed and the weights' block is refused after it: the job gives its blocks back and tries the
 * next smaller model */
static void weights_refused_job(unsigned i)
{
    make_frame(page_no++);
    if (resident.data || weights_blk.uid >= 0) TEST_CHECK(!drop_resident()); /* (the weights to be read again) */
    refuse_weights = 1;
    note_job(job(shell_request(3, 2, 2000, SPEED_BASE | SPEED_SPOT | SPEED_VAUTO)));
    refuse_weights = 0;
    (void)i;
}
/* another app in front of the game for over 5 s of its running time: worker_main's loop gives the resident blocks
 * back (inactive_long), then the game is in front again and reads */
static void left_for_long_job(unsigned i)
{
    owner_job(i);
    g.game_active = 0;
    loop_sleeps = 60; /* 100 ms sleeps: 6 s of the game's time */
    worker_main(0, NULL);
    loop_sleeps = 0;
    __atomic_store_n(&quitting, 0, __ATOMIC_RELEASE);
    TEST_CHECK_(weights_blk.uid < 0 && lm_blk.uid < 0 && spot_blk.uid < 0 && !blocked,
                "the resident blocks went back to a game left for 6 s");
    g.game_active = VJO_GAME;
    (void)inactive_long(0); /* the loop's next round, the game in front: its clock starts again */
}
/* the worker stopped (module_stop, the game's exit) and started again (another game in the same kind of process):
 * everything but the module's pages goes, and the next game's jobs read their models again */
static void stop_start_job(unsigned i)
{
    owner_job(i);
    TEST_CHECK(vjo_game_ocr_worker_stop() == VJO_OK);
    caller = 0;
    game_ocr_process_gone(GAME_PID);
    caller = GAME_PID;
    {
        long long held = 0;
        for (unsigned k = 0; k < BLOCKS; k++)
            if (blocks[k].base) held += (long long)blocks[k].bytes;
        TEST_CHECK_(held == 0 && worker < 0, "the stopped worker holds %lld B", held);
        TEST_CHECK_(g_game_ledger.pool_current[VJO_LEDGER_POOL_GAME] == VJO_BUDGET_GAME_MODULE_PAGES,
                    "the stopped worker's ledger: %u B", (unsigned)g_game_ledger.pool_current[VJO_LEDGER_POOL_GAME]);
    }
    TEST_CHECK(vjo_game_ocr_worker_start() == VJO_OK);
}

/* The game's frames between jobs, as the kernel samples them: a region signature every VJO_CHECK_EVERY_FRAMES frames
 * (kernel/display.c on_frame) and the scene tracker's update with it (kernel/main.c change_detection), n frames of
 * 1/60 s. The tracker then knows the region's screen, which a trusted answer's capture without pixels needs. */
static void frames_shown(unsigned n)
{
    SceneSig sig;
    for (unsigned k = 1; k <= n; k++) {
        clock_us += 16667;
        if (!g.game_active || k % VJO_CHECK_EVERY_FRAMES) continue;
        if (region_sig_hook((uintptr_t)frame, FB_W, 0x00000000u, FB_W, FB_H, &sig)) continue;
        VJO_LOCK();
        (void)scene_update(&g.tracker, &sig, (int64_t)clock_us, 0, 0);
        VJO_UNLOCK();
    }
}
/* The Shell sets the region again (kernel/main.c reset_change_detection): a region without a screen yet. */
static void region_set_again(void)
{
    VJO_LOCK();
    scene_reset(&g.tracker);
    g.hook_scene = 0;
    g.seen_sig_seq = g.hook_sig_seq;
    VJO_UNLOCK();
}
/* A hooked game: a Rubi verified table site holds the game's line (the config's trusted hooks, in SPEED_BASE). One
 * cycle: the region set again, and the site busy at the snapshot (a game thread writing it), so OCR reads the screen
 * with the owner's settings and reads or keeps the resident models; then TRUST_DROP_ANSWERS (8) new lines answered by
 * the site alone, without OCR: the first while the region has no screen yet (its first pass, one strip's block,
 * VjoGameVocrCapture), the others once the game has shown its frames (the kernel publishes the line without pixels,
 * API 49: no block). After the 8th the resident blocks go back to the game. */
static void hooked_job(unsigned i)
{
    static const char *const lines[] = {"「明日の朝、駅前の公園でもう一度会おうよ」",
                                        "「そんなこと急に言われても、困るんだけどな」",
                                        "「だったら今夜のうちに準備しておけばいいじゃない」"};
    unsigned snaps0, takes0;
    hooks_on = 1;
    region_set_again();
    hook_busy = 1;
    hook_text = lines[i % 3u];
    hook_seq++;
    owner_job(i);
    TEST_CHECK_(!job_out.text_from && job_out.hook_installed, "cycle %u: the busy site's job read the screen", i);
    hook_busy = 0;
    for (unsigned k = 1; k <= TRUST_DROP_ANSWERS; k++) {
        make_frame(page_no++);
        if (k > 1) frames_shown(4u * VJO_CHECK_EVERY_FRAMES); /* the new line's screen, settled */
        hook_text = lines[(i + k) % 3u];
        hook_seq++;
        snaps0 = hook_snaps;
        takes0 = capture_takes;
        TEST_CHECK(job(shell_request(3, 2, 2000, SPEED_BASE | SPEED_SPOT | SPEED_VAUTO)) == VJO_OK);
        TEST_CHECK_(job_out.text_from == HOOK_SITE && !strcmp(job_out.text, hook_text) && hook_snaps > snaps0 &&
                        (job_out.hook_flags & VJO_HOOK_R_NO_OCR),
                    "cycle %u, line %u: the site's line without OCR (from %u, flags 0x%x)", i, k,
                    (unsigned)job_out.text_from, (unsigned)job_out.hook_flags);
        TEST_CHECK_(capture_takes - takes0 == (k == 1 ? 1u : 0u), "cycle %u, line %u: %u capture blocks", i, k,
                    capture_takes - takes0);
    }
    TEST_CHECK_(weights_blk.uid < 0 && lm_blk.uid < 0 && spot_blk.uid < 0,
                "cycle %u: the resident blocks went back after %u trusted answers", i, TRUST_DROP_ANSWERS);
    hooks_on = 0;
}
/* The game suspended (the PS button: SceShell's LiveArea in front) and resumed during its job: the kernel's foreground
 * events as kernel/lifecycle.c applies them (each moves the epoch on; the resumed game is active again at the Shell's
 * verdict, vjoSetGameActive). The game's process is frozen meanwhile, so its worker sees both at its next call. */
static void kernel_foreground(SceUID pid, int active)
{
    VJO_LOCK();
    __atomic_add_fetch(&g.ocr_epoch, 1, __ATOMIC_RELEASE);
    g.game_pid = pid;
    g.game_active = active;
    game_ocr_foreground_changed_locked();
    VJO_UNLOCK();
}
static unsigned suspensions, suspended_cancelled;
static void suspend_and_resume(void)
{
    SceUID saved = caller;
    caller = 0;
    kernel_foreground(0, 0);                /* PROCEV_SUSPEND of the game in front */
    kernel_foreground(GAME_PID, 0);         /* PROCEV_RESUME: in front, not yet the Shell's verdict */
    kernel_foreground(GAME_PID, VJO_GAME);  /* the verdict */
    caller = saved;
    suspensions++;
}
/* at the job's 1st to 6th display call, or (the models to be read again) its 1st to 4th model read (without the
 * model files no job reads one: a display call then too); the job ends cancelled and gives its blocks back, then a job
 * of the owner's reads */
static void suspended_job(unsigned i)
{
    unsigned before = suspensions, cancelled = shell_cancelled;
    make_frame(page_no++);
    if ((i & 1u) || !models_ok) {
        suspend_at_frame = 1u + i % 6u;
    } else {
        TEST_CHECK(!drop_resident());
        suspend_at_read = 1u + i % 4u;
    }
    (void)job(shell_request(3, 2, 2000, SPEED_BASE | SPEED_SPOT | SPEED_VAUTO));
    TEST_CHECK_(suspensions == before + 1u, "cycle %u: the game was suspended during its job", i);
    suspended_cancelled += shell_cancelled - cancelled;
    suspend_at_frame = suspend_at_read = 0;
    owner_job(i);
}

static const Scenario scenarios[] = {
    {"owner: Large, LM 1000k, spotter, vertical auto", owner_job},
    {"Normal, its cascade, LM 4000k, the gate", normal_cascade_job},
    {"Small, no LM, no cascade, one thread", small_plain_job},
    {"a setting changed at every job", changing_job},
    {"the pause for the frame", paused_job},
    {"a blank text box (no line)", blank_job},
    {"the same page again (line cache)", same_page_job},
    {"the Shell cancels during the capture", cancelled_job},
    {"short of memory: a smaller size, then Large", short_of_memory_job},
    {"no memory: refused, then room again", no_memory_job},
    {"the Shell cancels while the models are read", cancelled_read_job},
    {"the capture's ladder: CDRAM, then phycont", ladder_job},
    {"the kernel strip (the hook cannot write)", kernel_strip_job},
    {"weights refused after the check (#324)", weights_refused_job},
    {"left for 6 s (the resident blocks go back)", left_for_long_job},
    {"the worker stopped and started", stop_start_job},
    {"a hooked game: OCR, then 8 trusted answers", hooked_job},
    {"the game suspended (PS) and resumed during its job", suspended_job},
};

static void the_game_process_holds_no_more_after_each_cycle(void)
{
    const unsigned cycles = soak_cycles(3);
    unsigned grew = 0, jobs0;
    models_ok = have_models();
    if (!models_ok)
        TEST_MSG("release models absent (VJO_VOCR_MODEL_DIR): every job ends at its model, after its blocks");
    game_starts();
    worker = -1;
    TEST_ASSERT(vjo_game_ocr_worker_start() == VJO_OK);
    for (unsigned s = 0; s < sizeof(scenarios) / sizeof(scenarios[0]); s++) {
        SoakSnap a, b;
        recognized = 0;
        jobs0 = cycles_run;
        /* the first run reads what this scenario keeps resident (its models, LM, spotter) */
        scenarios[s].cycle(0);
        ledgers_agree(scenarios[s].name);
        soak_snap(&a);
        for (unsigned i = 1; i <= cycles; i++) {
            scenarios[s].cycle(i);
            cycles_run++;
        }
        ledgers_agree(scenarios[s].name);
        soak_snap(&b);
        grew += soak_growth("game", scenarios[s].name, &a, &b, cycles);
        if (scenarios[s].cycle == suspended_job)
            printf("soak game: %u jobs suspended, %u of them read as cancelled by the Shell\n", suspensions,
                   suspended_cancelled);
        if (models_ok && scenarios[s].cycle != cancelled_job && scenarios[s].cycle != blank_job &&
            scenarios[s].cycle != no_memory_job)
            TEST_CHECK_(recognized > 0, "%s: no job recognized a line", scenarios[s].name);
        (void)jobs0;
    }
    TEST_CHECK_(grew == 0, "%u items grew over the cycles (soak game lines above)", grew);
    /* the game's exit: nothing of the worker's stays but its module (the process's end takes that) */
    TEST_CHECK(vjo_game_ocr_worker_stop() == VJO_OK);
    {
        SoakSnap end;
        soak_snap(&end);
        for (unsigned i = 0; i < end.n; i++)
            TEST_CHECK_(end.live[i] == 0, "%s %s: %lld held at the game's exit", soak_items[i].pool, soak_items[i].name,
                        end.live[i]);
    }
    soak_table("game");
}

TEST_LIST = {
    {"#528 soak: the game process holds no more after each cycle of every scenario",
     the_game_process_holds_no_more_after_each_cycle},
    {NULL, NULL}
};
