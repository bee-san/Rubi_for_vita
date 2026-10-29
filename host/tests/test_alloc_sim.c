/* The allocation simulator (#74 preflight): the real game worker
 * (game_ocr/main.c) and the real kernel (kernel/game_ocr.c, capture.c,
 * buffers.c) run whole OCR jobs on a 960x544 frame while every pool answers as
 * the owner's PCH-1001 on 3.65 did, or worse:
 *
 *   ScePhyMemPartKD  2,088,960 B refused five times with 0x80024302 [#74];
 *                    458,752 B granted in an earlier session [#74, 917x125x4].
 *                    Its free size is unknown (both exact queries failed), so a
 *                    request is granted only up to kd_largest, and the runs
 *                    repeat with the pool refusing everything.
 *   game USER        91,226,112 B free in CLANNAD [docs/vita-memory.md];
 *                    CDRAM and phycont are modelled pools, refusable one by one.
 *   PAF (SceShell)   2,346,312 B free at the game start [#74].
 *
 * API 18's kernel capture buffer (VjoCapture, which API 24 removed with its
 * allocator) is asked of the kernel pool's model at the sizes it took. Each case
 * prints one line; preflight.sh collects them. Not device evidence: the pools
 * are models of the measured numbers. */
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

#define FB_W 960u
#define FB_H 544u
#define KD_GRANTED_74 458752u        /* granted in an earlier session (917 x 125 x 4, page-rounded) */
#define KD_REFUSED_74 2088960u       /* refused five times, 0x80024302 */
#define USER_FREE_CLANNAD 91226112   /* docs/vita-memory.md */
#define PAF_FREE_74 2346312u         /* at the game start */
#define NO_PAGE ((int)0x80024302u)

VjoKernelState g;
unsigned syscall_depth;
static SceUID caller;
static unsigned locks;
static uint64_t clock_us;
static FILE *file;
static uint32_t frame[FB_W * FB_H];
static int in_hook, hook_copy_fails;
/* API 55 (docs/game-pause.md): a spinner in the text box, a new shade at each frame the game shows, and the display
 * calls a job's rows waited for */
static int spinner;
static unsigned display_calls, spinner_frames;

/* ---- the pools ---- */
static uint32_t kd_largest;      /* the largest KERNEL_RW block the kernel pool grants */
static uint32_t kd_taken, kd_peak, kd_refusals;
static int64_t user_free, cdram_free, phycont_free;
static int64_t user_used, cdram_used, phycont_used, user_peak;
static unsigned refuse;          /* bits: 1 USER capture, 2 CDRAM capture, 4 PHYCONT capture */
/* #528: the console reports the game's free USER memory in whole MiB (#324: all 78 samples of あおかな were 1-4 MiB;
 * every "free USER=" in the owner's logs is a whole MiB): report_mib rounds it down so. The sweep below (mem-game)
 * also tracks the least true free memory the worker left the game beside a block of its that stayed
 * (user_headroom_min), and a block the worker takes, samples once and gives back at once (game_ocr/main.c
 * resident_alloc's probe: the deepest such dip under the reserve, and how many) */
static int report_mib;
static int64_t user_headroom_min;
static SceUID pending_uid;
static int pending_resident; /* the pending block is a resident one's (weights, LM, cascade, spotter): resident_alloc's */
static int64_t pending_headroom, probe_dip_max;
static unsigned probes_refused, pending_samples;
static void pending_commit(void)
{
    if (pending_uid && pending_headroom < user_headroom_min) user_headroom_min = pending_headroom;
    pending_uid = 0;
}
typedef struct { SceUID uid; void *base; size_t bytes; int type; int kernel; } Block;
static Block blocks[16];
static SceUID next_uid = 100;

static Block *find_uid(SceUID uid)
{
    for (unsigned i = 0; i < 16; i++) if (blocks[i].uid == uid && blocks[i].base) return &blocks[i];
    return NULL;
}
static SceUID add_block(size_t bytes, int type, int kernel)
{
    for (unsigned i = 0; i < 16; i++)
        if (!blocks[i].base) {
            if (posix_memalign(&blocks[i].base, 4096, bytes)) return NO_PAGE;
            memset(blocks[i].base, 0xA5, bytes);
            blocks[i].bytes = bytes; blocks[i].type = type; blocks[i].kernel = kernel;
            blocks[i].uid = next_uid++;
            return blocks[i].uid;
        }
    return NO_PAGE;
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

/* the game */
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *out)
{
    int64_t user = user_free - user_used;
    if (pending_uid) pending_samples++;
    if (report_mib && user > 0) user -= user % (1 << 20); /* whole MiB, as the console reports it (#324) */
    out->size_user = (int)user;
    out->size_cdram = (int)(cdram_free - cdram_used);
    out->size_phycont = (int)(phycont_free - phycont_used);
    return 0;
}
SceUID sceKernelAllocMemBlock(const char *name, int type, unsigned n, void *opt)
{
    int capture = !strcmp(name, "VjoGameVocrCapture");
    (void)opt;
    if (capture && ((type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW && (refuse & 1)) ||
                    (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW && (refuse & 2)) ||
                    (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW && (refuse & 4))))
        return NO_PAGE;
    if (*used_of(type) + n > free_of(type)) return NO_PAGE;
    SceUID uid = add_block(n, type, 0);
    if (uid < 0) return uid;
    *used_of(type) += n;
    if (user_used > user_peak) user_peak = user_used;
    /* #528: the least free memory the game kept beside a block of ours that stayed; a block given back right after
     * one sample (the worker's probe, game_ocr/main.c resident_alloc) counts as a dip instead */
    pending_commit();
    pending_uid = uid;
    pending_resident = !strcmp(name, "VjoGameVocrWeights") || !strcmp(name, "VjoGameVocrLM") ||
                       !strcmp(name, "VjoGameVocrCascade") || !strcmp(name, "VjoGameVocrSpot");
    pending_headroom = user_free - user_used;
    pending_samples = 0;
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
    if (pending_uid && uid == pending_uid && pending_resident && pending_samples <= 2) {
        /* taken, sampled (its ALLOC step's sample and the worker's own), given back: a probe */
        if (VJO_GAME_RESERVE - pending_headroom > probe_dip_max) probe_dip_max = VJO_GAME_RESERVE - pending_headroom;
        probes_refused++;
        pending_uid = 0;
    } else {
        pending_commit();
    }
    *used_of(b->type) -= (int64_t)b->bytes;
    free(b->base);
    memset(b, 0, sizeof(*b));
    return 0;
}
/* the kernel pool */
SceUID ksceKernelAllocMemBlock(const char *name, unsigned type, SceSize bytes, void *opt)
{
    (void)name; (void)opt;
    TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW);
    if (bytes > kd_largest) { kd_refusals++; return NO_PAGE; }
    SceUID uid = add_block(bytes, (int)type, 1);
    if (uid < 0) return uid;
    kd_taken += bytes;
    if (kd_taken > kd_peak) kd_peak = kd_taken;
    return uid;
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
    kd_taken -= (uint32_t)b->bytes;
    free(b->base);
    memset(b, 0, sizeof(*b));
    return 0;
}
/* The worker's blocks are memory of the game process 7. */
SceUID ksceKernelFindProcMemBlockByAddr(SceUID pid, const void *addr, SceSize size)
{
    const unsigned char *p = addr;
    (void)size;
    if (pid != 7) return -1;
    for (unsigned i = 0; i < 16; i++)
        if (blocks[i].base && !blocks[i].kernel && p >= (unsigned char *)blocks[i].base &&
            p < (unsigned char *)blocks[i].base + blocks[i].bytes)
            return blocks[i].uid;
    return -1;
}
SceUID ksceKernelFindMemBlockByAddr(const void *addr, SceSize size) { (void)addr; (void)size; return 40; }

/* ---- the rest of the SDK boundary, as test_game_model_broker.c has it ---- */
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
/* API 23: a shot's file (no shot runs here: OCR jobs only) */
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
int ksceKernelWaitEventFlag(SceUID uid, unsigned bits, unsigned wait, unsigned *out, SceUInt *timeout)
{
    (void)uid; (void)wait; (void)out;
    if (bits == MEV_ROWS) { /* the game shows its next frame: the display hook runs in its thread */
        if (spinner) { /* 24 x 24 px right of the last line, inside the text box: never a shade it showed before */
            uint32_t v = 40u + (++spinner_frames * 37u) % 200u;
            for (unsigned y = 490; y < 514; y++)
                for (unsigned x = 906; x < 930; x++) frame[y * FB_W + x] = 0xFF000000u | v << 16 | v << 8 | v;
        }
        display_calls++;
        in_hook = 1;
        game_ocr_rows_hook(7, (uintptr_t)frame, FB_W, 0x00000000u, FB_W, FB_H);
        in_hook = 0;
        clock_us += 16667;
        return 0;
    }
    { SceUID saved = caller; unsigned depth = syscall_depth;
      caller = 0; syscall_depth = 0; game_ocr_model_tick(); caller = saved; syscall_depth = depth; }
    clock_us += *timeout;
    return 0;
}
int ksceKernelLockMutex(SceUID uid, int n, void *timeout) { (void)uid; (void)n; (void)timeout; locks++; return 0; }
int ksceKernelUnlockMutex(SceUID uid, int n) { (void)uid; (void)n; locks--; return 0; }
int taiGetModuleInfoForKernel(SceUID pid, const char *name, tai_module_info_t *info)
{ (void)pid; (void)name; info->modid = 99; return 0; }
int ksceKernelMemcpyUserToKernel(void *dst, const void *src, SceSize n) { if (!dst || !src) return -1; memcpy(dst, src, n); return 0; }
int ksceKernelMemcpyKernelToUser(void *dst, const void *src, SceSize n)
{
    if (!dst || !src || (in_hook && hook_copy_fails)) return -1; /* hook_copy_fails: the hook cannot write the game */
    memcpy(dst, src, n);
    return 0;
}
int ksceIoGetstatByFd(SceUID fd, SceIoStat *st) { (void)fd; memset(st, 0, sizeof(*st)); st->st_mtime = (SceDateTime){2026, 10, 13, 0, 0, 0, 0}; return 0; }
SceUID ksceIoOpen(const char *path, int flags, unsigned mode)
{
    char local[1024];
    (void)flags; (void)mode;
    if (!VJO_VOCR_MODEL_DIR[0] || file) return (int)0x80010002u;
    snprintf(local, sizeof(local), "%s/%s", VJO_VOCR_MODEL_DIR, path + sizeof(VJO_VOCR_MODEL_ROOT));
    file = fopen(local, "rb");
    return file ? 31 : (int)0x80010002u;
}
SceOff ksceIoLseek(SceUID fd, SceOff off, int whence) { (void)fd; fseek(file, (long)off, whence); return (SceOff)ftell(file); }
int ksceIoPread(SceUID fd, void *out, SceSize n, SceOff off)
{ (void)fd; fseek(file, (long)off, SEEK_SET); return (int)fread(out, 1, n, file); }
int ksceIoClose(SceUID fd) { (void)fd; int rc = fclose(file); file = NULL; return rc; }
/* A shot's file calls: no shot job runs in the simulator, so none is ever made. */
int ksceIoPwrite(SceUID fd, const void *p, SceSize n, SceOff off) { (void)fd; (void)p; (void)n; (void)off; TEST_CHECK(0); return -1; }
int ksceIoRemove(const char *path) { (void)path; TEST_CHECK(0); return -1; }
int ksceIoRename(const char *a, const char *b) { (void)a; (void)b; TEST_CHECK(0); return -1; }
int ksceIoSync(const char *dev, unsigned int unk) { (void)dev; (void)unk; TEST_CHECK(0); return -1; }
int ksceIoSyncByFd(SceUID fd, int flag) { (void)fd; (void)flag; TEST_CHECK(0); return -1; }
SceUID sceKernelGetProcessId(void) { return 7; }
uint64_t sceKernelGetProcessTimeWide(void) { return clock_us++; }
int sceKernelDelayThread(unsigned us)
{ SceUID saved = caller; clock_us += us; caller = 0; game_ocr_model_tick(); caller = saved; return 0; }
/* API 22: a helper thread (game_ocr/main.c pool_run) runs its line at once, in StartThread, as test_game_vocr.c's
 * does (the owner scenario's jobs take helpers); any other thread is never run here. */
static unsigned helper_threads;
SceUID sceKernelCreateThread(const char *name, int (*entry)(SceSize, void *), int p, unsigned stack, unsigned a, int cpu, void *opt)
{ return entry == helper_entry ? 41 + (int)(helper_threads++ % 64u) : 20; } /* 41-104: every one > 40 */
int sceKernelStartThread(SceUID uid, SceSize n, void *args) { return uid > 40 ? helper_entry(n, args) : 0; }

/* ocr-speed (API 22): the thread run time and the ARM clock; see test_game_vocr.c for the tested ones. */
int sceKernelGetThreadInfo(SceUID thid, SceKernelThreadInfo *info) { (void)thid; (void)info; return -1; }
SceUID sceKernelGetThreadId(void) { return 21; }
int scePowerGetArmClockFrequency(void) { return 333; }
int scePowerSetArmClockFrequency(int mhz) { (void)mhz; return 0; }
int sceKernelWaitThreadEnd(SceUID uid, int *status, void *timeout) { return 0; }
int sceKernelDeleteThread(SceUID uid) { return 0; }
int sceKernelGetProcessTitleId(SceUID pid, char *out, SceSize size) { snprintf(out, size, "PCSG00415"); return 0; }
int sceAppMgrAppParamGetString(int pid, int param, char *out, SceSize size) { return -1; }
int vjoGetVersion(void) { return VJO_API_VERSION; }
SceUID sceIoOpen(const char *path, int flags, unsigned mode) { return (int)0x80010013u; } /* the game cannot see ux0 */
int sceDisplayGetFrameBuf(SceDisplayFrameBuf *fb, int sync) { (void)fb; (void)sync; return -1; }
int sceKernelGetMemBlockInfoByAddr(void *base, SceKernelMemBlockInfo *info) { (void)base; (void)info; return -1; }

/* ---- the frame: a VN screen, a dark textbox with three lines of glyph-like strokes ---- */
static uint32_t seed = 74;
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
static void make_frame(void)
{
    seed = 74;
    for (unsigned y = 0; y < FB_H; y++)
        for (unsigned x = 0; x < FB_W; x++)
            frame[y * FB_W + x] = 0xFF000000u | 150u << 16 | (100 + x * 60 / FB_W) << 8 | (70 + y * 100 / FB_H + rnd(9));
    box(20, 380, 940, 536, 0xFF221812u);
    glyph_line(44, 404, 900, 22, 0xFFF0F0F0u);
    glyph_line(44, 440, 760, 22, 0xFF96ECFAu);
    glyph_line(44, 476, 620, 22, 0xFFECE4E4u);
}

/* ---- one case ---- */
typedef struct {
    int rc;
    uint32_t kd_peak, capture_bytes, capture_memory, source;
    int64_t user_peak;
    unsigned refused_steps, lines;
} Outcome;

static void reset_worker(void)
{
    game_room = 0; /* #528: the worker's figure of the game's free memory, the last case's */
    if (weights_blk.uid >= 0 || resident.data) drop_one(&resident);
    worker = workspace_uid = weights_blk.uid = capture_uid = line_uid = -1;
    resident.model = NULL; resident.data = NULL; resident.capacity = 0;
    model_owned = 0; model_seq = 0; quitting = active_seq = 0;
    worker_started = claim_outstanding = blocked = io_error = stat_error = hook_owns_capture = 0;
    frame_uid = -1; frame_base = NULL; frame_bytes_held = 0; memset(&pz, 0, sizeof(pz));
}

static VjoGameOcrResult last_out; /* the last case's result */
static uint32_t case_speed;       /* API 55: the next case's speed word (VJO_GAME_OCR_S_PAUSE) */
static Outcome run_case(const char *model, uint32_t flags, uint16_t rx, uint16_t ry, uint16_t rw, uint16_t rh)
{
    Outcome o;
    memset(&o, 0, sizeof(o));
    reset_worker();
    kd_taken = kd_peak = 0;
    user_used = cdram_used = phycont_used = user_peak = 0;
    memset(&g, 0, sizeof(g));
    game_ocr_init();
    clock_us = 1000000;
    g.lock = g.ievf = 1; g.evf = 2; g.mevf = 5; g.shell_pid = 3; g.game_pid = 7; g.game_active = VJO_GAME;
    g.ocr_epoch = 1; g.fb_w = FB_W; g.fb_h = FB_H; g.done_seq = g.capture_seq = 11;
    g.region = (VjoRect){rx, ry, rw, rh};
    caller = 7; TEST_ASSERT(!vjoOcrRegister()); caller = 3;
    VjoGameOcrRequest request = {.size = sizeof(request), .flags = flags, .speed = case_speed};
    strcpy(request.model_dir, VJO_VOCR_MODEL_ROOT);
    strcpy(request.model, model);
    int seq = vjoOcrSubmit(&request);
    TEST_ASSERT(seq > 0);
    caller = 7;
    TEST_ASSERT(!vjoOcrTake(&request));
    o.rc = vjo_game_ocr_run_request(&request, &last_out);
    TEST_CHECK(!vjoOcrComplete(&last_out));
    o.kd_peak = kd_peak;
    o.user_peak = user_peak;
    o.capture_bytes = last_out.capture_bytes;
    o.capture_memory = last_out.capture_memory;
    o.source = last_out.source;
    o.lines = last_out.lines;
    for (unsigned i = 0; i < last_out.n_steps && i < VJO_GAME_OCR_STEPS; i++)
        if (last_out.step[i].id == VJO_GSTEP_ALLOC && last_out.step[i].rc < 0) o.refused_steps++;
    /* every block went back: nothing held after the job (weights stay resident) */
    TEST_CHECK(kd_taken == 0 && cdram_used == 0 && phycont_used == 0);
    TEST_CHECK(user_used == (int64_t)(resident.data ? weights_blk.held : 0));
    return o;
}

static const char *const mem_name[] = {"-", "USER", "CDRAM", "PHYCONT", "kernel"};
static const char *const src_name[] = {"-", "hook", "kernel strip", "user"};

static void say(const char *what, const Outcome *o)
{
    fprintf(stderr, "alloc-sim: %-58s rc=%-4d KD %7u B, capture %s %s %6u B, game USER peak %9lld B, %u line(s)%s\n",
            what, o->rc, (unsigned)o->kd_peak, o->source < 4 ? src_name[o->source] : "?",
            o->capture_memory < 5 ? mem_name[o->capture_memory] : "?", (unsigned)o->capture_bytes,
            (long long)o->user_peak, o->lines, o->refused_steps ? " (refused rungs logged)" : "");
}

static int have_models(void)
{
    char path[1024];
    FILE *f;
    static const char *const names[] = {VJO_VOCR_TIER1_NAME, VJO_VOCR_TIER2_NAME, VJO_VOCR_TIER3_NAME,
                                        "VOCR3_H15_w8.vocr", "H15_w8.vocr"};
    if (!VJO_VOCR_MODEL_DIR[0]) return 0;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", VJO_VOCR_MODEL_DIR, names[i]);
        if (!(f = fopen(path, "rb"))) return 0;
        fclose(f);
    }
    return 1;
}

/* API 18: the kernel's region-sized capture buffer (VjoCapture, KERNEL_RW), asked of the pool
 * model at the sizes it took, page-rounded as kernel/buffers.c rounded them until API 24. */
static void api18_kernel_capture_is_refused_at_the_full_screen(void)
{
    kd_largest = KD_GRANTED_74; kd_refusals = 0;
    SceUID full = ksceKernelAllocMemBlock("VjoCapture", SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW, FB_W * FB_H * 4, NULL);
    TEST_CHECK(full == NO_PAGE && kd_refusals == 1);
    fprintf(stderr, "alloc-sim: %-58s rc=0x%08X (as #74: 0x80024302, five times)\n",
            "API 18 full screen: kernel VjoCapture 2,088,960 B", (unsigned)full);
    SceUID box = ksceKernelAllocMemBlock("VjoCapture", SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW,
                                         (917u * 125u * 4u + 4095u) & ~4095u, NULL);
    TEST_CHECK(box > 0 && kd_taken == KD_GRANTED_74);
    fprintf(stderr, "alloc-sim: %-58s rc=%d (granted, as in #74's earlier session)\n",
            "API 18 textbox 917x125: kernel VjoCapture 458,752 B", box > 0 ? 0 : (int)box);
    TEST_CHECK(box > 0 && ksceKernelFreeMemBlock(box) == 0);
    TEST_CHECK(kd_taken == 0);
}

/* API 19 under #74's pools: no kernel pool for OCR, at any region, any model. The
 * shipped sizes (Normal the default, Small, and Large, the largest pin), read through
 * the real kernel broker and checked against their pins by the real worker, and two
 * earlier models the host tools still open by name (VOCR3-H15, v0.2.0's H15). */
static void api19_runs_with_no_kernel_memory(void)
{
    static const struct { const char *what, *model; uint32_t flags; uint16_t r[4]; } cases[] = {
        {"API 19 full screen 960x544, Normal " VJO_VOCR_TIER2_NAME, VJO_VOCR_TIER2_NAME, VJO_GAME_OCR_F_FULL, {0, 0, 0, 0}},
        {"API 19 textbox 917x125, Normal " VJO_VOCR_TIER2_NAME, VJO_VOCR_TIER2_NAME, 0, {1536, 48188, 62600, 15059}},
        {"API 19 full screen 960x544, Small " VJO_VOCR_TIER1_NAME, VJO_VOCR_TIER1_NAME, VJO_GAME_OCR_F_FULL, {0, 0, 0, 0}},
        {"API 19 textbox 917x125, Small " VJO_VOCR_TIER1_NAME, VJO_VOCR_TIER1_NAME, 0, {1536, 48188, 62600, 15059}},
        {"API 19 full screen 960x544, Large " VJO_VOCR_TIER3_NAME, VJO_VOCR_TIER3_NAME, VJO_GAME_OCR_F_FULL, {0, 0, 0, 0}},
        {"API 19 textbox 917x125, Large " VJO_VOCR_TIER3_NAME, VJO_VOCR_TIER3_NAME, 0, {1536, 48188, 62600, 15059}},
        {"API 19 textbox 917x125, VOCR3-H15 (by name)", "VOCR3_H15_w8.vocr", 0, {1536, 48188, 62600, 15059}},
        {"API 19 textbox 917x125, v0.2.0 H15 (by name)", "H15_w8.vocr", 0, {1536, 48188, 62600, 15059}},
    };
    if (!have_models()) { TEST_MSG("release weights absent (VJO_VOCR_MODEL_DIR): simulator skipped"); return; }
    make_frame();
    for (unsigned pass = 0; pass < 2; pass++) {
        kd_largest = pass ? 0 : KD_GRANTED_74; /* pass 1: the kernel pool refuses everything */
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            char what[96];
            user_free = USER_FREE_CLANNAD; cdram_free = 128 << 20; phycont_free = 16 << 20; refuse = 0;
            hook_copy_fails = 0;
            Outcome o = run_case(cases[i].model, cases[i].flags, cases[i].r[0], cases[i].r[1], cases[i].r[2], cases[i].r[3]);
            snprintf(what, sizeof(what), "%s%s", cases[i].what, pass ? ", KD refuses all" : "");
            say(what, &o);
            TEST_CHECK_(o.rc == 0 && o.kd_peak == 0 && o.source == VJO_GAME_OCR_SRC_HOOK &&
                        o.capture_memory == VJO_GAME_OCR_MEM_USER && o.capture_bytes <= 64u * 1024u && o.lines > 0,
                        "%s: rc %d KD %u capture %u", what, o.rc, (unsigned)o.kd_peak, (unsigned)o.capture_bytes);
        }
    }
}

/* The ladder, one run per rung: USER -> CDRAM -> PHYCONT -> the kernel strip. */
static void api19_ladder_under_refusals(void)
{
    if (!have_models()) { TEST_MSG("release weights absent: ladder skipped"); return; }
    make_frame();
    kd_largest = KD_GRANTED_74;
    user_free = USER_FREE_CLANNAD; cdram_free = 128 << 20; phycont_free = 16 << 20;
    const uint16_t box[4] = {1536, 48188, 62600, 15059};

    refuse = 1; hook_copy_fails = 0;
    Outcome o = run_case(VJO_VOCR_TIER2_NAME, 0, box[0], box[1], box[2], box[3]);
    say("USER refuses the capture block -> CDRAM", &o);
    TEST_CHECK(o.rc == 0 && o.capture_memory == VJO_GAME_OCR_MEM_CDRAM && o.kd_peak == 0 && o.refused_steps == 1);

    refuse = 3;
    o = run_case(VJO_VOCR_TIER2_NAME, 0, box[0], box[1], box[2], box[3]);
    say("USER and CDRAM refuse it -> PHYCONT", &o);
    TEST_CHECK(o.rc == 0 && o.capture_memory == VJO_GAME_OCR_MEM_PHYCONT && o.kd_peak == 0 && o.refused_steps == 2);

    refuse = 0; hook_copy_fails = 1;
    o = run_case(VJO_VOCR_TIER2_NAME, 0, box[0], box[1], box[2], box[3]);
    say("the hook cannot write the game's memory -> kernel strip", &o);
    TEST_CHECK(o.rc == 0 && o.source == VJO_GAME_OCR_SRC_KSTRIP && o.kd_peak > 0 && o.kd_peak <= 64u * 1024u);

    kd_largest = 0;
    o = run_case(VJO_VOCR_TIER2_NAME, 0, box[0], box[1], box[2], box[3]);
    say("hook cannot write and KD refuses the strip", &o);
    TEST_CHECK(o.rc != 0 && o.kd_peak == 0);

    kd_largest = KD_GRANTED_74; hook_copy_fails = 0; refuse = 7;
    o = run_case(VJO_VOCR_TIER2_NAME, 0, box[0], box[1], box[2], box[3]);
    say("every rung refuses the capture block", &o);
    TEST_CHECK(o.rc == VJO_E_OOM && o.refused_steps == 3 && o.kd_peak == 0);
    refuse = 0;
}

/* The Shell's side at #74's game start: its result arena and the overlay's UI
 * arena above the PAF reserve (ledger constants; the Shell takes nothing else). */
static void shell_paf_fits_at_the_74_game_start(void)
{
    uint32_t reserve = 1536u * 1024u, need = VJO_BUDGET_PAF_RESULTS_LOCAL + VJO_BUDGET_PAF_UI;
    fprintf(stderr, "alloc-sim: %-58s %u B needed, %u B above the %u B reserve\n",
            "Shell PAF at the game start (2,346,312 B free)", (unsigned)need, (unsigned)(PAF_FREE_74 - reserve),
            (unsigned)reserve);
    TEST_CHECK(need <= PAF_FREE_74 - reserve);
}


/* API 55, the pause for the frame (docs/game-pause.md): a CLANNAD-sized text box (917 x 126) with a spinner beside its
 * last line that shows a new shade at every frame, read with the stable-text wait (150 ms, at most 1 s). Strip by
 * strip, the region is read over many display calls and its re-reads tear (on main the job then shows nothing,
 * VJO_E_SCREEN_CHANGED: #348's report; the rc is printed, not asserted, as another stabilisation may read it anyway);
 * paused, one display call copies the box from one frame, the job reads it, waits for nothing and goes on to the
 * model (its text with the release models; without them it stops there, VJO_E_OCR_MODEL), and the frame block goes
 * back with the job. Each case prints the display calls the job waited for and the rows of the longest one. */
static const VjoGameOcrStep *step_of(const VjoGameOcrResult *r, unsigned id)
{
    for (unsigned i = 0; i < r->n_steps && i < VJO_GAME_OCR_STEPS; i++)
        if (r->step[i].id == id) return &r->step[i];
    return NULL;
}
static void api55_pause_reads_one_frame_beside_a_spinner(void)
{
    const uint16_t box[4] = {1536, 48188, 62600, 15059};
    const uint32_t settle = 15u << VJO_GAME_OCR_SETTLE_SHIFT | 20u << VJO_GAME_OCR_SETTLE_MAX_SHIFT;
    const int models = have_models();
    make_frame();
    kd_largest = KD_GRANTED_74;
    user_free = USER_FREE_CLANNAD; cdram_free = 128 << 20; phycont_free = 16 << 20; refuse = 0; hook_copy_fails = 0;
    spinner = 1; spinner_frames = 0;
    display_calls = 0; case_speed = 0;
    Outcome o = run_case(VJO_VOCR_TIER2_NAME, settle, box[0], box[1], box[2], box[3]);
    fprintf(stderr, "alloc-sim: %-58s rc=%-4d %u display calls (64 rows at most in one), %u rows torn\n",
            "API 55 textbox 917x126 + spinner, strip by strip", o.rc, display_calls, (unsigned)last_out.tears);
    TEST_CHECK_(last_out.tears > 0 && display_calls > 3 && !step_of(&last_out, VJO_GSTEP_PAUSE),
                "strip by strip: rc %d, %u tears, %u display calls", o.rc, (unsigned)last_out.tears, display_calls);
    display_calls = 0; case_speed = VJO_GAME_OCR_S_PAUSE;
    o = run_case(VJO_VOCR_TIER2_NAME, settle, box[0], box[1], box[2], box[3]);
    const VjoGameOcrStep *ps = step_of(&last_out, VJO_GSTEP_PAUSE);
    fprintf(stderr, "alloc-sim: %-58s rc=%-4d %u display call(s), %u rows in the longest, %u lines%s\n",
            "API 55 textbox 917x126 + spinner, paused", o.rc, display_calls, ps ? ps->arg : 0u, o.lines,
            models ? "" : " (no release models: stops at the model)");
    TEST_CHECK_(ps && ps->kind == 1 && ps->arg == last_out.height && (ps->sum & 0xFFu) == VJO_GPAUSE_FROZEN &&
                ps->sum >> 8 == VJO_GAME_OCR_MEM_USER && !ps->rc &&
                ps->bytes == PAGE_ROUND(last_out.width * last_out.height), "the pause step");
    TEST_CHECK_(display_calls == 1 && last_out.captures == 1 && !last_out.tears && o.kd_peak == 0,
                "paused: %u display calls, %u captures, %u tears", display_calls, (unsigned)last_out.captures,
                (unsigned)last_out.tears);
    if (models) TEST_CHECK_(o.rc == 0 && o.lines >= 3 && last_out.text[0], "paused: rc %d, %u lines", o.rc, o.lines);
    else TEST_CHECK_(o.rc == VJO_E_OCR_MODEL, "paused without models: rc %d", o.rc);
    spinner = 0; case_speed = 0;
}

/* ---- #528 (mem-game): the game's memory, swept with the release models ----
 * Jobs by tiers as the owner's 0.10.1 Shell asks for them (issue #528's config.ini: vocr_tier = large, so Large,
 * Normal and Small; ocr_lm = 1000k, or 4000k; ocr_cascade_ms = 2000; ocr_threads = 3: two helpers; the boost;
 * hook_trust; ocr_finder = spotter; non_japanese_filter = lines; ocr_vertical = auto; ocr_line_fix = on;
 * ocr_head_gate = on; ocr_refine_lines = psp), on CLANNAD's text box (833 x 127, history.csv) and the full screen
 * (Deltarune's and e+'s jobs, history.csv), at a game's free USER memory from the logs and around every tier's need.
 * Free memory is reported exactly, or in whole MiB as the console reports it (#324). Each level runs two jobs, the
 * second beside what the first left resident. The worker's own step log says what ran: the check that passed last
 * (its tier, LM and cascade), the LM's and the spotter's own steps, the helpers. Not device evidence: the pool is a
 * model of the logs' numbers. */
typedef struct {
    int rc;
    unsigned tier, lm, lm_rc_bad, cascade, spot, spot_oom, helpers, checks, passed, loads, lm_loads, probes;
    int64_t peak, headroom, held, dip;
} GameJob;

/* held: the worker's blocks after the job, the resident ones (USER) */
static int64_t worker_held(void)
{
    return (int64_t)weights_blk.held * (weights_blk.uid >= 0) + (int64_t)lm_blk.held * (lm_blk.uid >= 0) +
           (int64_t)cascade_blk.held * (cascade_blk.uid >= 0) + (int64_t)spot_blk.held * (spot_blk.uid >= 0) +
           (int64_t)gate_blk.held * (gate_blk.uid >= 0);
}

static void game_sim_reset(void)
{
    TEST_CHECK(!drop_resident());
    reset_worker();
    TEST_CHECK(user_used == 0); /* nothing of the worker's is left in the game */
    user_used = cdram_used = phycont_used = 0;
}

static void dump_steps(const char *who)
{
    fprintf(stderr, "  %s: rc %d fallback %u free_user %d reserve %u need ws %u weights %u release %u\n", who,
            (int)last_out.rc, (unsigned)last_out.fallback, (int)last_out.free_user, (unsigned)last_out.reserve_bytes,
            (unsigned)last_out.need_workspace, (unsigned)last_out.need_weights, (unsigned)last_out.release_bytes);
    for (unsigned i = 0; i < last_out.n_steps && i < VJO_GAME_OCR_STEPS; i++)
        fprintf(stderr, "    step id %2u kind %3u arg %5u rc %11d bytes %10u sum 0x%08X main %9d\n",
                last_out.step[i].id, last_out.step[i].kind, last_out.step[i].arg, last_out.step[i].rc,
                last_out.step[i].bytes, last_out.step[i].sum, last_out.step[i].free_main);
}

static GameJob game_job(unsigned lm_tier, const uint16_t rect[4], uint32_t flags)
{
    GameJob o;
    memset(&o, 0, sizeof(o));
    kd_taken = kd_peak = 0;
    user_peak = user_used;
    user_headroom_min = user_free - user_used;
    pending_uid = 0; pending_samples = 0; probe_dip_max = 0; probes_refused = 0;
    memset(&g, 0, sizeof(g));
    game_ocr_init();
    clock_us = 1000000;
    g.lock = g.ievf = 1; g.evf = 2; g.mevf = 5; g.shell_pid = 3; g.game_pid = 7; g.game_active = VJO_GAME;
    g.ocr_epoch = 1; g.fb_w = FB_W; g.fb_h = FB_H; g.done_seq = g.capture_seq = 11;
    g.region = (VjoRect){rect[0], rect[1], rect[2], rect[3]};
    caller = 7; TEST_ASSERT(!vjoOcrRegister()); caller = 3;
    VjoGameOcrRequest request = {.size = sizeof(request), .tiers = VJO_VOCR_TIERS_ALL};
    request.flags = flags | (lm_tier & VJO_GAME_OCR_LM_MASK) << VJO_GAME_OCR_LM_SHIFT |
                    (2000u + 125u) / 250u << VJO_GAME_OCR_CASCADE_SHIFT | 2u << VJO_GAME_OCR_REFINE_SHIFT;
    request.speed = 2u | VJO_GAME_OCR_S_BOOST | VJO_GAME_OCR_S_TRUST | VJO_GAME_OCR_S_SPOT |
                    VJO_GAME_OCR_S_DROP_BORDERS | VJO_GAME_OCR_S_HEAD_GATE | VJO_GAME_OCR_S_TWO_LEVEL |
                    (uint32_t)VJO_GAME_OCR_VERTICAL_AUTO << VJO_GAME_OCR_S_VERTICAL_SHIFT | VJO_GAME_OCR_S_LINE_FIX;
    strcpy(request.model_dir, VJO_VOCR_MODEL_ROOT);
    strcpy(request.model, VJO_VOCR_TIER3_NAME);
    int seq = vjoOcrSubmit(&request);
    TEST_ASSERT(seq > 0);
    caller = 7;
    TEST_ASSERT(!vjoOcrTake(&request));
    o.rc = vjo_game_ocr_run_request(&request, &last_out);
    TEST_CHECK(!vjoOcrComplete(&last_out));
    pending_commit();
    o.peak = user_peak;
    o.headroom = user_headroom_min;
    o.probes = probes_refused;
    o.dip = probe_dip_max;
    o.held = worker_held();
    for (unsigned i = 0; i < last_out.n_steps && i < VJO_GAME_OCR_STEPS; i++) {
        const VjoGameOcrStep *s = &last_out.step[i];
        if (s->id == VJO_GSTEP_CHECK && VJO_GCHECK_TIER(s->kind)) {
            o.checks++;
            if (!s->rc) { /* the last check that passed is the one that ran (#324's retry checks again) */
                o.passed++;
                o.tier = VJO_GCHECK_TIER(s->kind);
                o.lm = s->kind & 0x7u;
                o.cascade = !!(s->kind & VJO_GCHECK_CASCADE);
            }
        } else if (s->id == VJO_GSTEP_LM) {
            o.lm_rc_bad = s->rc != 0;
            o.lm_loads += !!(s->bytes & VJO_GLM_READ);
        } else if (s->id == VJO_GSTEP_SPOT) {
            o.spot = s->kind;
            o.spot_oom = s->rc == VJO_E_OOM;
        } else if (s->id == VJO_GSTEP_PAR) {
            o.helpers = s->arg;
        } else if (s->id == VJO_GSTEP_LOAD) {
            o.loads += s->kind == VJO_GAME_OCR_WEIGHTS_READ;
        }
    }
    if (o.rc) o.tier = o.lm = o.cascade = 0;
    TEST_CHECK(kd_taken == 0 && cdram_used == 0 && phycont_used == 0);
    TEST_CHECK_(user_used == o.held, "after the job the game holds %lld B of ours, the resident blocks %lld B",
                (long long)user_used, (long long)o.held);
    return o;
}

static const char *const tier_id[] = {"-", "Small", "Normal", "Large"};
static const char *const lm_id[] = {"no LM", "500k", "1000k", "2000k", "4000k"};

static void game_say(const char *what, int64_t free_bytes, const GameJob *a, const GameJob *b)
{
    fprintf(stderr,
            "game-mem: %-34s free %9lld B%s  1st rc=%-4d %-6s %-5s%s%s h%u peak %8lld  2nd rc=%-4d %-6s %-5s%s%s "
            "h%u kept %8lld least left %8lld%s%s\n",
            what, (long long)free_bytes, report_mib ? " (MiB)" : "      ", a->rc, tier_id[a->tier & 3u],
            lm_id[a->lm <= 4u ? a->lm : 0u], a->lm_rc_bad ? "!" : "", a->spot_oom ? " grad" : "", a->helpers,
            (long long)a->peak, b->rc, tier_id[b->tier & 3u], lm_id[b->lm <= 4u ? b->lm : 0u],
            b->lm_rc_bad ? "!" : "", b->spot_oom ? " grad" : "", b->helpers, (long long)b->held,
            (long long)(a->headroom < b->headroom ? a->headroom : b->headroom),
            a->passed > 1 || b->passed > 1 ? "  (a passed check was retried smaller)" : "",
            a->probes || b->probes ? "  (a probe went back)" : "");
    if (a->probes || b->probes)
        fprintf(stderr, "game-mem:   probes given back %u + %u, at most %lld B under the reserve for one sample\n",
                a->probes, b->probes, (long long)(a->dip > b->dip ? a->dip : b->dip));
}

/* The free USER memory a job of tier t (no LM, no cascade, no spotter, no helpers) needs at a w x h region, beside
 * the reserve: its page-rounded workspace, capture block and weights, as the worker's check counts them. */
static int64_t tier_need(unsigned t, unsigned w, unsigned h)
{
    const VjoVocrModel *m = vjo_vocr_model(tier_names[t - 1u]);
    size_t job = m ? vjo_vocr_job_bytes(m->name, w, h) : 0;
    TEST_ASSERT(job);
    return (int64_t)PAGE_ROUND(job) + PAGE_ROUND(vjo_vocr_capture_bytes(w, h)) + PAGE_ROUND(m->bytes) +
           VJO_GAME_RESERVE;
}

static void game_memory_sweep(void)
{
    static const struct { const char *what; uint16_t r[4]; uint32_t flags; } regions[] = {
        {"CLANNAD box", {3619, 49996, 56866, 15299}, 0},
        {"full screen", {0, 0, 0, 0}, VJO_GAME_OCR_F_FULL},
    };
    /* the logs' free USER (#528's collection, the debug builds' "game vocr memory" lines; #324; #280): CLANNAD
     * 87-91 MiB, the PSP emulator 29-44 MiB, Code:Realize 10-17 MiB, あおかな 1-4 MiB, アマツツミ 2 MiB */
    static const int64_t logged[] = {91226112, 29360128, 10485760, 4194304, 3145728, 2097152, 1048576};
    if (!have_models()) { TEST_MSG("release weights absent (VJO_VOCR_MODEL_DIR): sweep skipped"); return; }
    make_frame();
    kd_largest = KD_GRANTED_74; refuse = 0; hook_copy_fails = 0;
    cdram_free = phycont_free = 0; /* USER alone: a full game's other pools give the ladder nothing */
    for (unsigned ri = 0; ri < sizeof(regions) / sizeof(regions[0]); ri++) {
        const uint16_t *rect = regions[ri].r;
        unsigned w, h;
        user_free = logged[0]; report_mib = 0;
        game_sim_reset();
        GameJob probe = game_job(2u, rect, regions[ri].flags); /* the region's size, as the kernel cuts it */
        if (getenv("GAME_SIM_DEBUG")) {
            fprintf(stderr, "probe rc %d tier %u passed %u checks %u io 0x%08X\n", probe.rc, probe.tier, probe.passed,
                    probe.checks, (unsigned)last_out.io_error);
            for (unsigned i = 0; i < last_out.n_steps && i < VJO_GAME_OCR_STEPS; i++)
                fprintf(stderr, "  step id %u kind %u arg %u rc %d bytes %u sum 0x%X main %d\n", last_out.step[i].id,
                        last_out.step[i].kind, last_out.step[i].arg, last_out.step[i].rc, last_out.step[i].bytes,
                        last_out.step[i].sum, last_out.step[i].free_main);
        }
        TEST_ASSERT(!probe.rc && probe.tier == 3u);
        w = last_out.width; h = last_out.height;
        const int64_t small = tier_need(1, w, h), large = tier_need(3, w, h);
        for (unsigned lmi = 0; lmi < 2; lmi++) {
            const unsigned lm_tier = lmi ? 4u : 2u;
            for (unsigned mode = 0; mode < 2; mode++) {
                int64_t levels[64];
                unsigned nl = 0;
                for (unsigned i = 0; i < sizeof(logged) / sizeof(logged[0]); i++) levels[nl++] = logged[i];
                /* around every tier's need: a page under and at it, and every whole MiB from Small's up to Large's
                 * with the largest LM, the spotter's scratch and both helpers */
                for (unsigned t = 1; t <= VJO_VOCR_TIERS; t++) {
                    levels[nl++] = tier_need(t, w, h) - 4096;
                    levels[nl++] = tier_need(t, w, h);
                }
                for (int64_t m = small >> 20; m <= (large + VJO_BUDGET_GAME_LM + VJO_BUDGET_GAME_WORKSPACE) >> 20 &&
                                              nl < 64; m++)
                    levels[nl++] = m << 20;
                for (unsigned i = 0; i < nl; i++) {
                    char what[64];
                    const int64_t f = levels[i], seen = mode && f > 0 ? f - f % (1 << 20) : f;
                    user_free = f;
                    report_mib = 0; /* the reset runs on exact figures */
                    game_sim_reset();
                    report_mib = (int)mode;
                    snprintf(what, sizeof(what), "%s %ux%u, %s", regions[ri].what, w, h, lm_id[lm_tier]);
                    GameJob a = game_job(lm_tier, rect, regions[ri].flags);
                    if (getenv("GAME_SIM_DUMP") && atoll(getenv("GAME_SIM_DUMP")) == f && (int)mode == atoi(getenv("GAME_SIM_DUMP_MODE") ? getenv("GAME_SIM_DUMP_MODE") : "1") && !ri && !lmi) dump_steps("1st");
                    GameJob b = game_job(lm_tier, rect, regions[ri].flags);
                    if (getenv("GAME_SIM_DUMP") && atoll(getenv("GAME_SIM_DUMP")) == f && (int)mode == atoi(getenv("GAME_SIM_DUMP_MODE") ? getenv("GAME_SIM_DUMP_MODE") : "1") && !ri && !lmi) dump_steps("2nd");
                    if (getenv("GAME_SIM_STEADY")) { /* six more jobs: the steady state's failures and weight reads */
                        char seq[64];
                        unsigned fails = 0, reads = a.loads + b.loads, lm_reads = a.lm_loads + b.lm_loads, k = 0;
                        seq[k++] = (char)("-SNL"[a.tier & 3u]); seq[k++] = (char)("-SNL"[b.tier & 3u]);
                        fails = !!a.rc + !!b.rc;
                        for (unsigned j = 0; j < 6; j++) {
                            GameJob c = game_job(lm_tier, rect, regions[ri].flags);
                            seq[k++] = (char)("-SNL"[c.tier & 3u]);
                            fails += !!c.rc; reads += c.loads; lm_reads += c.lm_loads;
                        }
                        seq[k] = 0;
                        fprintf(stderr, "game-steady: %-30s free %9lld%s  tiers %s  failed %u/8  weight reads %u  LM reads %u\n",
                                what, (long long)f, mode ? " (MiB)" : "      ", seq, fails, reads, lm_reads);
                    }
                    game_say(what, f, &a, &b);
                    /* The reserve holds in truth: the game always keeps VJO_GAME_RESERVE beside our blocks. */
                    TEST_CHECK_(a.headroom >= VJO_GAME_RESERVE && b.headroom >= VJO_GAME_RESERVE,
                                "%s at %lld B: %lld / %lld B left to the game", what, (long long)f,
                                (long long)a.headroom, (long long)b.headroom);
                    /* a block the worker takes to see whether it fits, and gives back at once, leaves the game less
                     * than the reserve for one sample only, by less than the console's unit */
                    TEST_CHECK_(a.dip < (1 << 20) && b.dip < (1 << 20) &&
                                    (mode || f % (1 << 20) == 0 || (!a.probes && !b.probes)),
                                "%s at %lld B%s: probes %u / %u, %lld / %lld B under the reserve", what, (long long)f,
                                mode ? " (MiB)" : "", a.probes, b.probes, (long long)a.dip, (long long)b.dip);
                    /* A job never fails for memory while Small fits beside the reserve as its check sees the game,
                     * and one that does fail leaves the game nothing of ours. */
                    if (seen >= small)
                        TEST_CHECK_(!a.rc && (mode || !b.rc), "%s at %lld B%s (Small needs %lld): rc %d / %d", what,
                                    (long long)f, mode ? " (MiB)" : "", (long long)small, a.rc, b.rc);
                    if (f < small)
                        TEST_CHECK_(a.rc == VJO_E_OOM && b.rc == VJO_E_OOM, "%s at %lld B, under Small's %lld: rc %d / %d",
                                    what, (long long)f, (long long)small, a.rc, b.rc);
                    TEST_CHECK_((!a.rc || a.rc == VJO_E_OOM) && (!b.rc || b.rc == VJO_E_OOM) && (!b.rc || !b.held),
                                "%s at %lld B: rc %d / %d, %lld B kept", what, (long long)f, a.rc, b.rc,
                                (long long)b.held);
                    /* The reserve is applied once: the tier the check chose is the tier that reads, whether the
                     * console reports whole MiB or not (#324 refused Normal's weights after a passed check). */
                    TEST_CHECK_(a.passed <= 1 && b.passed <= 1, "%s at %lld B%s: %u / %u passed checks", what,
                                (long long)f, mode ? " (MiB)" : "", a.passed, b.passed);
                }
            }
        }
    }
    report_mib = 0;
    cdram_free = 128 << 20; phycont_free = 16 << 20;
    game_sim_reset();
}

/* The owner scenario (#528, docs/owner-scenario.md), the game's part: the owner's OCR job, as the Shell asks for it
 * with his config.ini (Large, ocr_lm 1000k, ocr_cascade_ms 2000, ocr_threads 3, the spotter and the other defaults;
 * shell/worker.c vocr_run_one packs it: keep the two in step, test_shell_worker.c's
 * vocr_request_carries_the_job_policy holds that side), through the real worker and kernel, at the free USER sizes
 * his games logged [measured: the debug builds' "game vocr memory: free USER=" lines] and at smaller ones (his native
 * ports' are unmeasured: the normal build logs none) [estimate], on a textbox and on the full screen. Each case
 * prints one `owner-scenario game NAME: key=value ...` line: rc, the tier, LM tier and cascade the memory check
 * chose, whether the spotter found the lines, the helpers, and the USER peak of Rubi's blocks; told=1 when the job
 * read text or its refusal carries the memory check's figures, which the Shell shows and logs (game_oom_detail,
 * log_tier_checks). tools/owner_scenario.py holds them against tools/owner_scenario.json. */
#include "../../core/config.h"
#include "owner_request.h" /* the owner's config and the Shell's packing of his job, shared with test_shell_worker.c */
/* One job of the owner scenario's game part at the pool's current state (cold: nothing resident; warm: what the job
 * before it left), and its `owner-scenario game NAME:` line. */
static void scenario_job(const char *name, const VjoConfig *c, uint32_t flags, const uint16_t r[4], int64_t free_bytes)
{
    VjoGameOcrRequest request;
    unsigned tier = 0, lm = 0, casc = 0, spot = 0, helpers = 0;
    int rc, told;
    kd_taken = kd_peak = 0;
    user_peak = user_used; /* a warm job's resident blocks are Rubi's too */
    memset(&g, 0, sizeof(g));
    game_ocr_init();
    clock_us = 1000000;
    g.lock = g.ievf = 1; g.evf = 2; g.mevf = 5; g.shell_pid = 3; g.game_pid = 7; g.game_active = VJO_GAME;
    g.ocr_epoch = 1; g.fb_w = FB_W; g.fb_h = FB_H; g.done_seq = g.capture_seq = 11;
    g.region = (VjoRect){r[0], r[1], r[2], r[3]};
    caller = 7; TEST_ASSERT(!vjoOcrRegister()); caller = 3;
    owner_request(&request, c, flags, tier_names[c->vocr_tier - 1]); /* the largest tier's model names the job */
    TEST_ASSERT(vjoOcrSubmit(&request) > 0);
    caller = 7;
    TEST_ASSERT(!vjoOcrTake(&request));
    rc = vjo_game_ocr_run_request(&request, &last_out);
    TEST_CHECK(!vjoOcrComplete(&last_out));
    for (unsigned i = 0; i < last_out.n_steps && i < VJO_GAME_OCR_STEPS; i++) {
        const VjoGameOcrStep *s = &last_out.step[i];
        if (s->id == VJO_GSTEP_CHECK && !s->rc) {
            tier = VJO_GCHECK_TIER(s->kind); lm = s->kind & 7u; casc = !!(s->kind & VJO_GCHECK_CASCADE);
        }
        if (s->id == VJO_GSTEP_SPOT) spot = s->kind == 1 && !s->rc;
        if (s->id == VJO_GSTEP_PAR) helpers = s->arg;
    }
    told = rc == 0 || (rc == VJO_E_OOM && last_out.reserve_bytes != 0);
    fprintf(stderr, "owner-scenario game %s: free=%lld rc=%d failed=%d tier=%u lm=%u cascade=%u spot=%u "
                    "helpers=%u peak=%lld lines=%u told=%d\n", name, (long long)free_bytes, rc, rc != 0, rc ? 0u : tier,
            rc ? 0u : lm, rc ? 0u : casc, spot, helpers, (long long)user_peak, last_out.lines, told);
    /* any policy: no block outlives the job but the resident ones, the kernel's pool untouched */
    TEST_CHECK(kd_taken == 0 && cdram_used == 0 && phycont_used == 0);
    TEST_CHECK_(user_peak <= free_bytes, "%s: peak %lld over the free %lld", name, (long long)user_peak,
                (long long)free_bytes);
}

/* #528 (mem-game): the console reports the game's free USER memory in whole MiB, rounded down (#324: all 78 samples of
 * あおかな were 1-4 MiB; every "free USER=" the owner's debug builds logged is a whole MiB), so every case runs with
 * that figure; each level runs the job twice, cold (nothing of Rubi's resident) and warm (`-warm`: beside the weights,
 * LM and spotter the cold job left, as every later job of a session runs); and the owner's config at each model size
 * (`tier-SIZE-88MiB`: vocr_tier = small, normal, large; CLANNAD's measured free USER, textbox, cold), which #528's
 * summary reports game memory per size from. */
static void owner_scenario_game(void)
{
    static const struct { const char *name; int64_t free; } levels[] = {
        {"clannad-88MiB", 88080384},   /* [measured] PCSG00415, API 31-36 */
        {"psp-28MiB", 29360128},       /* [measured] Adrenaline, API 35 */
        {"pcsg00402-12MiB", 12664832}, /* [measured] API 36 */
        {"pcsg00402-10MiB", 10485760}, /* [measured] API 36 */
        {"est-8MiB", 8 << 20}, {"est-6MiB", 6 << 20}, {"est-5MiB", 5 << 20}, {"est-4MiB", 4 << 20},
        {"est-3MiB", 3 << 20}, {"est-2.5MiB", 5 << 19}, {"est-2MiB", 2 << 20}, {"est-1.5MiB", 3 << 19},
    };
    static const struct { const char *name; uint32_t flags; uint16_t r[4]; } regions[] = {
        {"box", 0, {1536, 48188, 62600, 15059}}, {"full", VJO_GAME_OCR_F_FULL, {0, 0, 0, 0}},
    };
    static const struct { const char *name; int tier; } sizes[] = {
        {"tier-small-88MiB", VJO_VOCR_TIER_SMALL}, {"tier-normal-88MiB", VJO_VOCR_TIER_NORMAL},
        {"tier-large-88MiB", VJO_VOCR_TIER_LARGE},
    };
    VjoConfig c;
    char name[64];
    if (!have_models()) {
        TEST_MSG("release weights absent (VJO_VOCR_MODEL_DIR): the owner scenario's game part is left out");
        fprintf(stderr, "owner-scenario game: skipped (no release models)\n");
        return;
    }
    vjo_config_defaults(&c);
    vjo_config_parse(&c, owner_config, sizeof(owner_config) - 1);
    TEST_ASSERT(c.vocr_tier == VJO_VOCR_TIER_LARGE && c.ocr_lm == 2 && c.ocr_helpers == 2 && c.ocr_finder);
    make_frame();
    kd_largest = KD_GRANTED_74;
    report_mib = 1;
    for (unsigned li = 0; li < sizeof(levels) / sizeof(levels[0]); li++)
        for (unsigned ri = 0; ri < sizeof(regions) / sizeof(regions[0]); ri++) {
            TEST_CHECK(drop_resident() == 0); /* a cold job: nothing kept from the case before */
            reset_worker();
            user_free = levels[li].free; cdram_free = 128 << 20; phycont_free = 16 << 20; refuse = 0;
            hook_copy_fails = 0; spinner = 0;
            user_used = cdram_used = phycont_used = user_peak = 0;
            snprintf(name, sizeof(name), "%s-%s", levels[li].name, regions[ri].name);
            scenario_job(name, &c, regions[ri].flags, regions[ri].r, levels[li].free);
            snprintf(name, sizeof(name), "%s-%s-warm", levels[li].name, regions[ri].name);
            scenario_job(name, &c, regions[ri].flags, regions[ri].r, levels[li].free);
        }
    for (unsigned si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
        VjoConfig t = c;
        t.vocr_tier = sizes[si].tier;
        TEST_CHECK(drop_resident() == 0);
        reset_worker();
        user_free = 88080384; cdram_free = 128 << 20; phycont_free = 16 << 20; refuse = 0;
        hook_copy_fails = 0; spinner = 0;
        user_used = cdram_used = phycont_used = user_peak = 0;
        scenario_job(sizes[si].name, &t, regions[0].flags, regions[0].r, 88080384);
    }
    report_mib = 0;
    TEST_CHECK(drop_resident() == 0);
    reset_worker();
}

TEST_LIST = {
    {"owner scenario", owner_scenario_game}, /* #528: tools/owner_scenario.py runs it by this name */
    {"#528 the game's memory swept: every tier, the LM and the spotter, exact and in whole MiB", game_memory_sweep},
    {"API 55 the pause reads one frame beside a spinner that tears every strip-by-strip read", api55_pause_reads_one_frame_beside_a_spinner},
    {"API 18 kernel capture refused at the full screen under #74's pool", api18_kernel_capture_is_refused_at_the_full_screen},
    {"API 19 OCR takes no kernel memory under #74's pools", api19_runs_with_no_kernel_memory},
    {"API 19 ladder under refusals, each rung logged", api19_ladder_under_refusals},
    {"Shell PAF fits at #74's game start", shell_paf_fits_at_the_74_game_start},
    {NULL, NULL}
};
