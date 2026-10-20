/* API 23, the Anki screenshot end to end in the game's process: the actual game
 * worker (game_ocr/main.c run_shot, core/jpegsw.c) and the actual kernel
 * transport (kernel/game_ocr.c through kernel_model_broker_boundary.c's retail
 * return model, kernel/capture.c's rows as the display hook copies them from the
 * game's frame) write a real JPEG into a real anki_queue/ directory, the files
 * created, written, renamed and synced only by the persistent kernel worker (the
 * game's own namespace has no ux0:). The frame is procedural here; with
 * --shot FRAME.rgba W H FORMAT DIR it is the given picture in the given pixel
 * format, and host/tests/test_game_shot.py decodes the JPEG with Pillow.
 * Host execution is not device proof (docs/offline-testing.md). */
#define main acutest_main
#include "acutest.h"
#undef main
#include "test_tmpdir.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <psp2/host_stubs.h>
#include <psp2kern/host_stubs.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <taihen.h>
#include "../../kernel/vjo_kernel.h"
#include "../../kernel/game_ocr.h"
#include "../../include/vjo_vocr_sizes.h"
#include "../../core/anki_queue.h"
#include "../../game_ocr/main.c"

VjoKernelState g;
unsigned syscall_depth;
static SceUID caller;
static unsigned locks;
static uint64_t clock_us;

/* ---- the game's frame (what the display hook sees) ---- */
static uint32_t frame32[960 * 544];   /* A8B8G8R8 or A2B10G10R10 words */
static uint16_t frame16[960 * 544];   /* A1B5G5R5 */
static uint32_t fb_w = 960, fb_h = 544, fb_fmt = VJO_FB_A8B8G8R8;
static int hook_copy_fail;            /* the hook cannot write the game's block (the kernel strip rung) */
static int resize_after;              /* the frame changes size after this many frames (0: never) */
static int cancel_after;              /* the Shell cancels after this many frames (0: never) */
static unsigned frames, in_hook;
static uint32_t cur_seq; /* the shot in flight (the Shell's cancel names it) */

/* ---- the game's memory ---- */
static unsigned char shot_block[VJO_BUDGET_GAME_SHOT + 4u * 1024u * 1024u] __attribute__((aligned(64)));
static int block_live, refuse_user, refuse_all;
static unsigned block_bytes, block_allocs, block_frees, live_bytes, peak_bytes, cdram_allocs;
static int64_t game_free = 64 * 1024 * 1024;

/* ---- the memory card (the kernel worker's files) ---- */
static int file_fd = -1, open_error, write_error, kernel_files;
/* a slow memory card: after the JPEG's last piece the kernel worker gets to the save only this much
 * later (its syscalls answer BUSY meanwhile), as when its close waits for the card */
static uint64_t late_after_last_write_us, late_after_each_write_us, late_until;
static unsigned file_opens, file_writes, renames, syncs, removes;

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
/* Physical retail hardware returned the kernel's -3 as 0xBFFFFFFD: the same model as the broker test's. */
static int retail_return(int rc) { return rc < 0 ? (int32_t)((uint32_t)rc & ~0x40000000u) : rc; }
int vjoBootInfo(VjoBootInfo *out) { out->mode = VJO_BOOT_NORMAL; return 0; }
int vjoOcrRegister(void) { return retail_return(test_kernel_register()); }
int vjoOcrSubmit(const VjoGameOcrRequest *r) { return retail_return(test_kernel_submit(r)); }
int vjoOcrTake(VjoGameOcrRequest *out) { return retail_return(test_kernel_take(out)); }
int vjoOcrCaptureRows(const VjoGameOcrRows *r, void *dst, VjoGameOcrRowsInfo *info)
{ return retail_return(test_kernel_capture_rows(r, dst, info)); }
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

/* ---- the kernel's platform ---- */
void klog(const char *format, ...) { (void)format; }
SceUID ksceKernelGetProcessId(void) { return caller; }
int ksceKernelGetThreadId(void) { return 0x7001; } /* the game worker's one thread (#138's registration check) */
uint64_t ksceKernelGetSystemTimeWide(void) { return clock_us; }
int ksceKernelSetEventFlag(SceUID uid, unsigned bits) { (void)uid; (void)bits; return 0; }
int ksceKernelClearEventFlag(SceUID uid, unsigned bits) { (void)uid; (void)bits; return 0; }
int ksceKernelDelayThread(SceUInt us) { clock_us += us; return 0; }
SceUID ksceKernelFindMemBlockByAddr(const void *addr, SceSize size) { (void)addr; (void)size; return 40; }
/* The shot's block is memory of the game process 7. */
SceUID ksceKernelFindProcMemBlockByAddr(SceUID pid, const void *addr, SceSize size)
{
    const unsigned char *p = addr;
    TEST_CHECK(size == 0);
    return pid == 7 && block_live && p >= shot_block && p < shot_block + block_bytes ? 41 : -1;
}
static void kernel_worker(void)
{
    if (clock_us < late_until) return; /* (the card is busy: the worker has not got to it yet) */
    SceUID saved = caller;
    unsigned depth = syscall_depth;
    caller = 0;
    syscall_depth = 0;
    game_ocr_model_tick();
    caller = saved;
    syscall_depth = depth;
}
/* A syscall's wait, without the lock: the game's next frame (the display hook
 * runs in its thread), or the kernel worker finishing a file operation. */
int ksceKernelWaitEventFlag(SceUID uid, unsigned bits, unsigned wait, unsigned *out, SceUInt *timeout)
{
    (void)uid; (void)wait;
    TEST_ASSERT(!locks && syscall_depth == 1 && timeout);
    if (bits == MEV_ROWS) {
        frames++;
        if (cancel_after && frames == (unsigned)cancel_after) {
            SceUID saved = caller;
            caller = 3;
            TEST_CHECK(vjoOcrCancel(cur_seq) == 0);
            caller = saved;
        }
        if (resize_after && frames > (unsigned)resize_after) fb_w = 720;
        in_hook = 1;
        game_ocr_rows_hook(7, fb_fmt == VJO_FB_A1B5G5R5 ? (uintptr_t)frame16 : (uintptr_t)frame32, 960, fb_fmt,
                           fb_w, fb_h);
        in_hook = 0;
        if (out) *out = MEV_ROWS;
        return 0;
    }
    if (bits == MEV_DONE) {
        if (clock_us < late_until) { clock_us += *timeout; return SCE_KERNEL_ERROR_WAIT_TIMEOUT; }
        kernel_worker();
        if (out) *out = MEV_DONE;
        return 0;
    }
    clock_us += *timeout; /* a Take or a Read with nothing to wait for */
    return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
}
int ksceKernelLockMutex(SceUID uid, int n, void *timeout) { (void)uid; (void)n; (void)timeout; TEST_CHECK(!locks); locks++; return 0; }
int ksceKernelUnlockMutex(SceUID uid, int n) { (void)uid; (void)n; TEST_CHECK(locks == 1); locks--; return 0; }
int taiGetModuleInfoForKernel(SceUID pid, const char *name, tai_module_info_t *info)
{ (void)pid; (void)name; info->modid = 99; return 0; }
int ksceKernelMemcpyUserToKernel(void *dst, const void *src, SceSize n)
{ if (!dst || !src) return -1; memcpy(dst, src, n); return 0; }
int ksceKernelMemcpyKernelToUser(void *dst, const void *src, SceSize n)
{
    if (in_hook && hook_copy_fail) return -1;
    if (!dst || !src) return -1;
    memcpy(dst, src, n);
    return 0;
}
/* The kernel strip (the ladder's last rung) and nothing else: no capture buffer. */
static unsigned char kstrip[VJO_GAME_OCR_KSTRIP_MAX];
static int kstrip_live;
static unsigned kstrip_allocs;
SceUID ksceKernelAllocMemBlock(const char *name, unsigned type, SceSize bytes, void *opt)
{
    (void)type; (void)opt;
    TEST_CHECK(!strcmp(name, "VjoOcrStrip") && bytes <= sizeof(kstrip) && !kstrip_live);
    kstrip_live = 1;
    kstrip_allocs++;
    return 28;
}
int ksceKernelGetMemBlockBase(SceUID uid, void **base) { TEST_CHECK(uid == 28); *base = kstrip; return 0; }
int ksceKernelFreeMemBlock(SceUID uid) { TEST_CHECK(uid == 28 && kstrip_live); kstrip_live = 0; return 0; }
int ksceIoGetstatByFd(SceUID fd, SceIoStat *st) { (void)fd; memset(st, 0, sizeof(*st)); return 0; }

/* The memory card: the kernel worker's file calls on real files (the test's directory). */
static void in_kernel_worker(void) { TEST_ASSERT(caller == 0 && !locks && !syscall_depth); }
SceUID ksceIoOpen(const char *path, int flags, unsigned mode)
{
    in_kernel_worker();
    TEST_CHECK(!strncmp(path, VJO_GAME_OCR_SHOT_DIR "/", sizeof(VJO_GAME_OCR_SHOT_DIR)) &&
               flags == (SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC));
    file_opens++;
    if (open_error) return open_error;
    TEST_ASSERT(file_fd < 0);
    file_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    TEST_ASSERT(file_fd >= 0);
    kernel_files++;
    return 32;
}
SceOff ksceIoLseek(SceUID fd, SceOff off, int whence) { (void)fd; (void)off; (void)whence; TEST_CHECK(0); return -1; }
int ksceIoPread(SceUID fd, void *out, SceSize n, SceOff off) { (void)fd; (void)out; (void)n; (void)off; TEST_CHECK(0); return -1; }
int ksceIoPwrite(SceUID fd, const void *data, SceSize n, SceOff off)
{
    in_kernel_worker();
    TEST_ASSERT(fd == 32 && file_fd >= 0);
    file_writes++;
    if (write_error) return write_error;
    if (late_after_each_write_us) late_until = clock_us + late_after_each_write_us;
    if (late_after_last_write_us && n < VJO_GAME_OCR_SHOT_CHUNK) late_until = clock_us + late_after_last_write_us;
    return (int)pwrite(file_fd, data, n, (off_t)off);
}
int ksceIoChstatByFd(SceUID fd, SceIoStat *st, int bits)
{
    in_kernel_worker();
    TEST_ASSERT(fd == 32 && file_fd >= 0 && st);
    TEST_CHECK(bits == SCE_CST_SIZE);
    return ftruncate(file_fd, (off_t)st->st_size) ? -1 : 0;
}
int ksceIoClose(SceUID fd)
{
    in_kernel_worker();
    TEST_ASSERT(fd == 32 && file_fd >= 0);
    TEST_CHECK(fsync(file_fd) == 0 && close(file_fd) == 0);
    file_fd = -1;
    return 0;
}
int ksceIoRename(const char *from, const char *to) { in_kernel_worker(); renames++; return rename(from, to) ? -1 : 0; }
int ksceIoRemove(const char *path) { in_kernel_worker(); removes++; return unlink(path) ? (int)0x80010002u : 0; }
int ksceIoSync(const char *device, unsigned int unk) { (void)device; (void)unk; TEST_CHECK(0); return -1; }
int ksceIoSyncByFd(SceUID fd, int flag) { (void)fd; (void)flag; TEST_CHECK(0); syncs++; return -1; } /* #194: a shot's save syncs nothing */

/* ---- the game's platform ---- */
SceUID sceKernelGetProcessId(void) { return 7; }
uint64_t sceKernelGetProcessTimeWide(void) { return clock_us++; }
int sceKernelDelayThread(unsigned us) { clock_us += us; kernel_worker(); return 0; }
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *out)
{
    out->size_user = (int)(game_free - live_bytes);
    out->size_cdram = 8 * 1024 * 1024;
    out->size_phycont = 8 * 1024 * 1024;
    return 0;
}
SceUID sceKernelAllocMemBlock(const char *name, int type, unsigned n, void *opt)
{
    (void)opt;
    TEST_CHECK(!strcmp(name, "VjoGameShot") && !block_live && n <= sizeof(shot_block));
    if (refuse_all || (refuse_user && type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW)) return (int)0x80020001u;
    if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW) { TEST_CHECK(!(n % (256u * 1024u))); cdram_allocs++; }
    block_live = 1;
    block_bytes = n;
    block_allocs++;
    live_bytes += n;
    if (live_bytes > peak_bytes) peak_bytes = live_bytes;
    return 12;
}
int sceKernelGetMemBlockBase(SceUID uid, void **base) { TEST_CHECK(uid == 12 && block_live); *base = shot_block; return 0; }
int sceKernelFreeMemBlock(SceUID uid)
{
    TEST_CHECK(uid == 12 && block_live);
    block_live = 0;
    block_frees++;
    live_bytes -= block_bytes;
    return 0;
}
SceUID sceKernelCreateThread(const char *name, int (*entry)(SceSize, void *), int p, unsigned stack, unsigned a, int cpu, void *opt)
{ (void)name; (void)entry; (void)p; (void)stack; (void)a; (void)cpu; (void)opt; return 20; }
int sceKernelStartThread(SceUID uid, SceSize n, void *args) { (void)uid; (void)n; (void)args; return 0; }
int sceKernelWaitThreadEnd(SceUID uid, int *status, void *timeout) { (void)uid; (void)status; (void)timeout; return 0; }
int sceKernelDeleteThread(SceUID uid) { (void)uid; return 0; }
int sceKernelGetThreadInfo(SceUID thid, SceKernelThreadInfo *info) { (void)thid; (void)info; return -1; }
SceUID sceKernelGetThreadId(void) { return 21; }
int scePowerGetArmClockFrequency(void) { return 333; }
int scePowerSetArmClockFrequency(int mhz) { (void)mhz; TEST_CHECK(0); return 0; } /* a shot never boosts */
int sceKernelGetProcessTitleId(SceUID pid, char *out, SceSize size) { (void)pid; snprintf(out, size, "PCSG00415"); return 0; }
int sceAppMgrAppParamGetString(int pid, int param, char *out, SceSize size) { (void)pid; (void)param; (void)out; (void)size; return -1; }
int sceDisplayGetFrameBuf(SceDisplayFrameBuf *fb, int sync) { (void)fb; (void)sync; return -1; }
int sceKernelGetMemBlockInfoByAddr(void *base, SceKernelMemBlockInfo *info) { (void)base; (void)info; return -1; }
int vjoGetVersion(void) { return VJO_API_VERSION; }
SceUID sceIoOpen(const char *path, int flags, unsigned mode) { (void)path; (void)flags; (void)mode; return (int)0x80010013u; }

/* ---- the harness ---- */
static const char CARD[] = "0123456789abcdef0123456789abcdef";
static char jpg_path[256], tmp_path[256];
static uint32_t shot_speed; /* API 32: the shot's speed word, VJO_GAME_OCR_S_TRIM or 0 */

static void procedural_frame(void)
{
    /* a light gradient with dark text-like strokes and a coloured band, as a VN's screen */
    for (uint32_t y = 0; y < 544; y++)
        for (uint32_t x = 0; x < 960; x++) {
            uint32_t r = 180 + x * 60 / 960, gg = 160 + y * 80 / 544, b = 200 - x * 80 / 960;
            if (y > 380 && y < 520 && (x / 3) % 9 < 2 && (y / 6) % 5 < 4) r = gg = b = 25;
            if (y < 40) { r = 200; gg = 40; b = 60; }
            frame32[y * 960 + x] = 0xFF000000u | b << 16 | gg << 8 | r;
        }
}

static void reset(void)
{
    memset(&g, 0, sizeof(g));
    game_ocr_init();
    worker = workspace_uid = weights_uid = capture_uid = line_uid = lm_uid = cascade_uid = shot_uid = -1;
    model_owned = 0; model_seq = 0; quitting = active_seq = 0; worker_started = claim_outstanding = blocked = 0;
    io_error = 0; hook_owns_capture = 0; shot_bytes_held = 0;
    locks = syscall_depth = 0; clock_us = 1000000;
    frames = in_hook = 0; hook_copy_fail = resize_after = cancel_after = 0;
    fb_w = 960; fb_h = 544; fb_fmt = VJO_FB_A8B8G8R8;
    block_live = refuse_user = refuse_all = 0; block_allocs = block_frees = live_bytes = peak_bytes = cdram_allocs = 0;
    game_free = 64 * 1024 * 1024;
    file_fd = -1; open_error = write_error = kernel_files = 0; file_opens = file_writes = renames = syncs = removes = 0;
    late_after_last_write_us = late_after_each_write_us = late_until = 0;
    kstrip_live = 0; kstrip_allocs = 0;
    shot_speed = 0;
    g.lock = g.ievf = 1; g.evf = 2; g.mevf = 5; g.shell_pid = 3; g.game_pid = 7; g.game_active = VJO_GAME; g.ocr_epoch = 1;
    g.fb_w = 960; g.fb_h = 544; g.done_seq = g.capture_seq = 11;
    g.region = (VjoRect){2000, 40000, 60000, 20000}; /* the user's textbox: a shot ignores it */
    mkdir("ux0:data", 0777);
    mkdir("ux0:data/VitaJPOverlay", 0777);
    mkdir(VJO_GAME_OCR_SHOT_DIR, 0777);
    snprintf(jpg_path, sizeof(jpg_path), VJO_GAME_OCR_SHOT_DIR "/%s.jpg", CARD);
    snprintf(tmp_path, sizeof(tmp_path), VJO_GAME_OCR_SHOT_DIR "/%s.jpg.tmp", CARD);
    unlink(jpg_path);
    unlink(tmp_path);
}

/* The Shell submits a shot of CARD, the worker claims and runs it, completes it, and
 * the Shell reads the result (out). Returns the job's rc. */
static int shot(VjoGameOcrResult *out)
{
    static VjoGameOcrRequest r, took;
    static VjoGameOcrResult res;
    int seq;
    memset(&r, 0, sizeof(r));
    r.size = sizeof(r);
    r.flags = VJO_GAME_OCR_F_SHOT;
    r.speed = shot_speed;
    memcpy(r.shot, CARD, sizeof(CARD));
    caller = 7; TEST_ASSERT(!vjoOcrRegister());
    caller = 3; TEST_ASSERT((seq = vjoOcrSubmit(&r)) > 0);
    cur_seq = (uint32_t)seq;
    caller = 7; memset(&took, 0, sizeof(took)); TEST_ASSERT(!vjoOcrTake(&took));
    vjo_game_ocr_run_request(&took, &res);
    TEST_CHECK(!vjoOcrComplete(&res));
    caller = 3;
    memset(out, 0, sizeof(*out));
    int rr = vjoOcrRead((uint32_t)seq, out);
    if (res.rc == VJO_E_CANCELLED) TEST_CHECK(rr == VJO_ERR_CANCELLED);
    else TEST_CHECK(!rr && out->seq == (uint32_t)seq);
    /* whatever happened: nothing held, in the game or the kernel */
    TEST_CHECK(!block_live && shot_uid < 0 && !model_owned && !game_ocr_capture_busy_locked() && file_fd < 0 &&
               !kstrip_live && !locks);
    TEST_CHECK(vjo_ledger_current(&g_game_ledger, GL_SHOT) == 0);
    return res.rc;
}

static long file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) ? -1 : (long)st.st_size;
}

static const VjoGameOcrStep *step_of(const VjoGameOcrResult *r, unsigned id, unsigned nth)
{
    for (uint32_t i = 0; i < r->n_steps && i < VJO_GAME_OCR_STEPS; i++)
        if (r->step[i].id == id && !nth--) return &r->step[i];
    return NULL;
}

/* The whole frame (not the region), 64 rows a frame through the hook, encoded in one
 * block of the game's memory and written beside the card through the kernel worker. */
static void the_shot_is_the_whole_frame_written_beside_the_card(void)
{
    VjoGameOcrResult out;
    reset();
    procedural_frame();
    TEST_CHECK(shot(&out) == VJO_OK && !out.rc);
    TEST_CHECK(out.width == 960 && out.height == 544 && !out.region_x && !out.region_y);
    TEST_CHECK(out.source == VJO_GAME_OCR_SRC_HOOK && out.captures == 9 && frames == 9 && out.strip_rows == 64);
    TEST_CHECK(out.shot_quality == 70 && out.shot_tries == 1 && out.shot_bytes > 1000 &&
               out.shot_bytes <= VJO_GAME_OCR_SHOT_MAX);
    TEST_CHECK(file_size(jpg_path) == (long)out.shot_bytes && file_size(tmp_path) < 0);
    TEST_CHECK(kernel_files == 1 && renames == 1 && syncs == 0 && file_writes == (out.shot_bytes + 32767u) / 32768u);
    /* its block: page-rounded, USER_RW, the ledger's budget at most; the game's peak is that block */
    TEST_CHECK(out.shot_block == SHOT_BLOCK_BYTES(960) && out.shot_block <= VJO_BUDGET_GAME_SHOT &&
               out.shot_memory == VJO_GAME_OCR_MEM_USER && peak_bytes == out.shot_block && block_frees == 1);
    TEST_CHECK(vjo_ledger_peak(&g_game_ledger, GL_SHOT) == out.shot_block);
    TEST_CHECK(!kstrip_allocs && g.done_seq == 11); /* no kernel block; no capture published */
    const VjoGameOcrStep *s = step_of(&out, VJO_GSTEP_SHOT, 0), *w = step_of(&out, VJO_GSTEP_SAVE, 0);
    TEST_ASSERT(s && w);
    TEST_CHECK(s->kind == 70 && s->arg == 1 && !s->rc && s->bytes == out.shot_bytes && s->sum == 9);
    TEST_CHECK(!w->rc && w->bytes == out.shot_bytes && w->kind == file_writes);
    printf("measure: 960x544 procedural shot: %u B JPEG q%u, block %u B, %u strips, %u steps\n", out.shot_bytes,
           out.shot_quality, out.shot_block, out.captures, out.n_steps);
}

/* The game's memory: too little beside the reserve, nothing taken; USER_RW refused,
 * CDRAM (its 256 KiB units); nothing at all, no shot. */
static void the_game_memory_decides(void)
{
    VjoGameOcrResult out;
    reset(); procedural_frame();
    game_free = VJO_GAME_RESERVE + SHOT_BLOCK_BYTES(960) - 1;
    TEST_CHECK(shot(&out) == VJO_E_OOM && !block_allocs && !frames && file_size(jpg_path) < 0);
    TEST_CHECK(out.reserve_bytes == VJO_GAME_RESERVE && out.need_workspace == SHOT_BLOCK_BYTES(960));
    reset(); procedural_frame();
    refuse_user = 1;
    TEST_CHECK(shot(&out) == VJO_OK && out.shot_memory == VJO_GAME_OCR_MEM_CDRAM && cdram_allocs == 1);
    TEST_CHECK(out.shot_block == 512u * 1024u && file_size(jpg_path) == (long)out.shot_bytes);
    reset(); procedural_frame();
    refuse_all = 1;
    TEST_CHECK(shot(&out) == VJO_E_OOM && !frames && file_size(jpg_path) < 0 && !kernel_files);
}

/* The hook cannot write the game's block: the kernel strip, 16 rows a call, and the
 * same JPEG. */
static void the_kernel_strip_rung_gives_the_same_jpeg(void)
{
    VjoGameOcrResult out, hook;
    unsigned char first[VJO_GAME_OCR_SHOT_MAX];
    FILE *f;
    reset(); procedural_frame();
    TEST_CHECK(shot(&hook) == VJO_OK);
    TEST_ASSERT((f = fopen(jpg_path, "rb")) != NULL);
    TEST_ASSERT(fread(first, 1, sizeof(first), f) == hook.shot_bytes);
    fclose(f);
    reset(); procedural_frame();
    hook_copy_fail = 1;
    TEST_CHECK(shot(&out) == VJO_OK && out.source == VJO_GAME_OCR_SRC_KSTRIP && out.strip_rows == 16);
    TEST_CHECK(kstrip_allocs == 34 && out.shot_bytes == hook.shot_bytes); /* 544 / 16 */
    TEST_ASSERT((f = fopen(jpg_path, "rb")) != NULL);
    unsigned char again[VJO_GAME_OCR_SHOT_MAX];
    TEST_CHECK(fread(again, 1, sizeof(again), f) == out.shot_bytes && !memcmp(first, again, out.shot_bytes));
    fclose(f);
}

/* A frame that makes too large a JPEG is encoded again at a lower quality; one that is
 * too large even then gives no screenshot and no file. */
static void too_large_is_encoded_lower_then_given_up(void)
{
    VjoGameOcrResult out;
    uint32_t v = 12345;
    reset();
    for (uint32_t i = 0; i < 960 * 544; i++) { /* noise: the worst case */
        v = v * 1103515245u + 12345u;
        frame32[i] = 0xFF000000u | (v >> 8);
    }
    int rc = shot(&out);
    const VjoGameOcrStep *s0 = step_of(&out, VJO_GSTEP_SHOT, 0);
    TEST_ASSERT(s0 != NULL);
    TEST_CHECK(s0->kind == 70 && s0->rc == VJO_E_TOO_LARGE);
    if (rc == VJO_OK) {
        TEST_CHECK(out.shot_tries >= 2 && out.shot_quality < 70 && file_size(jpg_path) == (long)out.shot_bytes);
    } else {
        TEST_CHECK(rc == VJO_E_TOO_LARGE && out.shot_tries == 3 && file_size(jpg_path) < 0 && !kernel_files);
    }
    printf("measure: 960x544 noise: rc %d after %u tries (q%u), %u B\n", rc, out.shot_tries, out.shot_quality,
           out.shot_bytes);
}

/* The memory card refuses the file, the Shell cancels, the frame changes size: no
 * screenshot, no file left, nothing held. */
static void failures_leave_no_file(void)
{
    VjoGameOcrResult out;
    reset(); procedural_frame();
    open_error = (int)0x80010013u;
    TEST_CHECK(shot(&out) == VJO_E_LOCAL_IO && out.io_error == open_error && file_size(jpg_path) < 0);
    reset(); procedural_frame();
    write_error = (int)0x8001001Cu; /* ENOSPC */
    TEST_CHECK(shot(&out) == VJO_E_LOCAL_IO && file_size(jpg_path) < 0 && file_size(tmp_path) < 0 && removes == 1);
    reset(); procedural_frame();
    cancel_after = 3;
    TEST_CHECK(shot(&out) == VJO_E_CANCELLED && frames == 3 && !kernel_files);
    reset(); procedural_frame();
    resize_after = 2;
    TEST_CHECK(shot(&out) == VJO_E_SOURCE && !kernel_files && file_size(jpg_path) < 0);
}

/* A slow memory card: the kernel worker saves the JPEG 10 s after its last piece (its close): the
 * worker waits for it, and the card has its screenshot; one 40 s late is given up, its file removed,
 * and the worker is not left blocked: the next screenshot works. */
static void a_slow_card_is_waited_for_and_never_blocks_the_worker(void)
{
    VjoGameOcrResult out;
    reset(); procedural_frame();
    late_after_each_write_us = 3000000;
    TEST_CHECK(shot(&out) == VJO_OK && file_size(jpg_path) == (long)out.shot_bytes && !blocked);
    TEST_CHECK(file_writes > 1 && out.shot_write_us >= 6000000u);
    reset(); procedural_frame();
    late_after_last_write_us = 10000000;
    TEST_CHECK(shot(&out) == VJO_OK && file_size(jpg_path) == (long)out.shot_bytes && !blocked);
    TEST_CHECK(out.shot_write_us >= 10000000u);
    reset(); procedural_frame();
    late_after_last_write_us = 40000000;
    TEST_CHECK(shot(&out) == VJO_E_LOCAL_IO && out.io_error == VJO_ERR_TIMEOUT);
    TEST_CHECK(!blocked && !out.cleanup_status && file_size(jpg_path) < 0 && file_size(tmp_path) < 0);
    late_after_last_write_us = late_until = 0;
    TEST_CHECK(shot(&out) == VJO_OK && file_size(jpg_path) == (long)out.shot_bytes);
}

/* Each individual card delay fits thirty seconds, but the chunks and final
 * close share that time: two sixteen-second waits must not publish a JPEG. */
static void all_file_calls_share_one_deadline(void)
{
    VjoGameOcrResult out;
    reset(); procedural_frame();
    late_after_each_write_us = 16000000;
    TEST_CHECK(shot(&out) == VJO_E_LOCAL_IO && out.io_error == VJO_ERR_TIMEOUT);
    TEST_CHECK(file_writes > 1 && out.shot_write_us >= VJO_GAME_OCR_SHOT_FILE_US &&
               out.shot_write_us < VJO_GAME_OCR_SHOT_FILE_US + 1000000u);
    TEST_CHECK(!out.shot_bytes && !out.cleanup_status && !blocked && !renames);
    TEST_CHECK(file_size(jpg_path) < 0 && file_size(tmp_path) < 0 && removes == 1);
    late_after_each_write_us = late_until = 0;
    TEST_CHECK(shot(&out) == VJO_OK && file_size(jpg_path) == (long)out.shot_bytes);
}

/* The OCR job after a shot is as before it: the slot, the kernel and the game hold nothing of it. */
static void a_shot_leaves_the_slot_as_it_found_it(void)
{
    VjoGameOcrResult out;
    reset(); procedural_frame();
    for (int i = 0; i < 5; i++) {
        unlink(jpg_path);
        TEST_CHECK(shot(&out) == VJO_OK);
    }
    TEST_CHECK(block_allocs == 5 && block_frees == 5 && !game_ocr_capture_busy_locked());
}

/* ---- API 32: the frame's black bars (VJO_GAME_OCR_S_TRIM, core/shottrim.h) ---- */

/* What the JPEG must be: the frame's sub-rectangle (x, y, w, h) through core/jpegsw.c at quality,
 * as one picture (vjo_jpeg_encode), in want[]; its length. */
static unsigned char want[VJO_GAME_OCR_SHOT_MAX + 64 * 1024];
typedef struct { uint32_t x, y; } Origin;
static int sub_rows(void *ud, uint32_t row, uint32_t n, uint8_t *dst)
{
    const Origin *o = ud;
    for (uint32_t r = 0; r < n; r++)
        memcpy(dst + (size_t)r * 960u * 4u, &frame32[(o->y + row + r) * 960u + o->x], 960u * 4u - o->x * 4u);
    return (int)n;
}
static size_t reference(uint32_t x, uint32_t y, uint32_t w, uint32_t h, int quality)
{
    static unsigned char mem[4096 + 16 + 960 * 4 * 16 + sizeof(want) + 64];
    VjoArena a;
    VjoBuf b = {0};
    Origin o = {x, y};
    vjo_arena_init(&a, mem, sizeof(mem));
    TEST_ASSERT(vjo_jpeg_encode(&a, w, h, 960u * 4u, sub_rows, &o, quality, &b) == 0 && b.len <= sizeof(want));
    memcpy(want, b.data, b.len);
    return b.len;
}
/* The card's JPEG is want[] (len bytes). */
static int jpeg_is_want(size_t len)
{
    static unsigned char got[sizeof(want)];
    FILE *f = fopen(jpg_path, "rb");
    size_t n;
    if (!f) return 0;
    n = fread(got, 1, sizeof(got), f);
    fclose(f);
    return n == len && !memcmp(got, want, len);
}
static void black_frame(void)
{
    for (uint32_t i = 0; i < 960 * 544; i++) frame32[i] = 0xFF000000u;
}
/* A game's picture, never black (each channel 40 or more), at x0, y0 */
static void picture_at(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h)
{
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t r = 180 + x * 60 / w, gg = 160 + y * 80 / h, b = 200 - x * 80 / w;
            if (y > h * 7 / 10 && y < h * 19 / 20 && (x / 3) % 9 < 2 && (y / 6) % 5 < 4) r = gg = b = 40;
            frame32[(y0 + y) * 960 + x0 + x] = 0xFF000000u | b << 16 | gg << 8 | r;
        }
}
/* The shot's TRIM step: kind, how, and the picture it kept. */
static void check_trim_step(const VjoGameOcrResult *out, unsigned kind, unsigned how, uint32_t x, uint32_t y, uint32_t w,
                            uint32_t h)
{
    const VjoGameOcrStep *t = step_of(out, VJO_GSTEP_TRIM, 0);
    TEST_ASSERT(t != NULL);
    TEST_CHECK_(t->kind == kind && t->arg == how && !t->rc && t->bytes == (x << 16 | y) && t->sum == (w << 16 | h) && t->us,
                "trim step: kind %u how %u, %ux%u at %u,%u; want kind %u how %u, %ux%u at %u,%u", t->kind, t->arg,
                t->sum >> 16, t->sum & 0xFFFF, t->bytes >> 16, t->bytes & 0xFFFF, kind, how, w, h, x, y);
    TEST_CHECK(!step_of(out, VJO_GSTEP_TRIM, 1));
}

/* With anki_screenshot_trim_bars off (no VJO_GAME_OCR_S_TRIM) a shot is API 23's, the whole frame as one
 * picture, byte for byte, and no trim step; on, a frame whose first rows reach its top and side edges is
 * read and encoded the same way, the same bytes, in the same 9 strips (decided by its first rows). */
static void api32_untrimmed_frames_are_byte_identical(void)
{
    VjoGameOcrResult out;
    size_t len;
    reset(); procedural_frame();
    len = reference(0, 0, 960, 544, 70);
    TEST_CHECK(shot(&out) == VJO_OK && out.shot_bytes == len && jpeg_is_want(len));
    TEST_CHECK(out.captures == 9 && frames == 9 && !step_of(&out, VJO_GSTEP_TRIM, 0));
    reset(); procedural_frame();
    shot_speed = VJO_GAME_OCR_S_TRIM;
    TEST_CHECK(shot(&out) == VJO_OK && out.shot_bytes == len && jpeg_is_want(len) && out.shot_tries == 1);
    TEST_CHECK(out.captures == 9 && frames == 9 && out.width == 960 && out.height == 544);
    check_trim_step(&out, VJO_TRIM_NONE, 0, 0, 0, 960, 544);
    TEST_CHECK(out.shot_block == SHOT_BLOCK_BYTES(960) && peak_bytes == out.shot_block); /* no more memory */
}

/* Adrenaline's layouts of a PSP game's 480 x 272 picture: at 1x and 1.5x its bars trimmed, the JPEG the
 * picture and 2 px of each bar (the sub-rectangle of the same rows, byte for byte), in 9 strips as before
 * (the first rows guess it, every row checks it); at 2x nothing to trim, decided by the first rows. */
static void api32_adrenaline_layouts_are_trimmed(void)
{
    static const struct { uint32_t w, h, strips; } sizes[] = {{480, 272, 9}, {600, 340, 9}, {720, 408, 9}, {840, 476, 9},
                                                              {960, 544, 9}};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        VjoGameOcrResult out;
        uint32_t x0 = (960 - sizes[i].w) / 2, y0 = (544 - sizes[i].h) / 2;
        uint32_t cx = x0 ? (x0 - 2) & ~1u : 0, cy = y0 ? (y0 - 2) & ~1u : 0;
        size_t len;
        reset(); black_frame(); picture_at(x0, y0, sizes[i].w, sizes[i].h);
        shot_speed = VJO_GAME_OCR_S_TRIM;
        len = reference(cx, cy, 960 - 2 * cx, 544 - 2 * cy, 70);
        TEST_CHECK_(shot(&out) == VJO_OK && out.shot_bytes == len && jpeg_is_want(len), "%ux%u: %u B, want %u B",
                    sizes[i].w, sizes[i].h, out.shot_bytes, (unsigned)len);
        TEST_CHECK_(out.captures == sizes[i].strips && frames == sizes[i].strips, "%ux%u: %u strips", sizes[i].w,
                    sizes[i].h, out.captures);
        if (x0) check_trim_step(&out, VJO_TRIM_TRIMMED, 1, cx, cy, 960 - 2 * cx, 544 - 2 * cy);
        else check_trim_step(&out, VJO_TRIM_NONE, 0, 0, 0, 960, 544);
        printf("measure: a PSP picture %ux%u in the 960x544 frame: JPEG %ux%u, %u B, %u strips\n", sizes[i].w, sizes[i].h,
               960 - 2 * cx, 544 - 2 * cy, out.shot_bytes, out.captures);
    }
}

/* A 4:3 pillarbox (a PS1 game's): guessed from the first strip's one-colour bars, checked on every row,
 * 9 strips. Its first rows' bars in a frame whose lower rows reach the edges: every row comes to the whole
 * frame, which is read again (9 strips more, the second pass at the same quality) and is API 23's JPEG,
 * byte for byte. Side bars of more than one black under no top bar are guessed the whole frame: right, at
 * once, in 9 strips. */
static void api32_a_pillarbox_and_a_wrong_guess(void)
{
    VjoGameOcrResult out;
    size_t len;
    reset(); black_frame(); picture_at(117, 0, 726, 544);
    shot_speed = VJO_GAME_OCR_S_TRIM;
    len = reference(114, 0, 732, 544, 70);
    TEST_CHECK(shot(&out) == VJO_OK && jpeg_is_want(len) && out.captures == 9 && out.shot_tries == 1);
    check_trim_step(&out, VJO_TRIM_TRIMMED, 1, 114, 0, 732, 544);
    /* the guess wrong: rows 300-339 across the frame */
    reset(); black_frame(); picture_at(117, 0, 726, 544); picture_at(0, 300, 960, 40);
    shot_speed = VJO_GAME_OCR_S_TRIM;
    len = reference(0, 0, 960, 544, 70);
    TEST_CHECK(shot(&out) == VJO_OK && jpeg_is_want(len) && out.captures == 18 && frames == 18 && out.shot_tries == 2);
    const VjoGameOcrStep *s0 = step_of(&out, VJO_GSTEP_SHOT, 0), *s1 = step_of(&out, VJO_GSTEP_SHOT, 1);
    TEST_ASSERT(s0 && s1);
    TEST_CHECK(s0->kind == 70 && s0->arg == 1 && !s0->rc && s0->sum == 9 && s1->kind == 70 && s1->arg == 2 && !s1->rc &&
               s1->sum == 9 && s1->bytes == len);
    check_trim_step(&out, VJO_TRIM_NONE, 2, 0, 0, 960, 544);
    /* the same frame, its bars two blacks (a dark picture's edges): the whole frame guessed, 9 strips */
    reset(); black_frame(); picture_at(117, 0, 726, 544); picture_at(0, 300, 960, 40);
    for (uint32_t y = 0; y < 544; y += 2) frame32[y * 960 + 5] = 0xFF010101u;
    shot_speed = VJO_GAME_OCR_S_TRIM;
    TEST_CHECK(shot(&out) == VJO_OK && jpeg_is_want(reference(0, 0, 960, 544, 70)) && out.captures == 9);
    check_trim_step(&out, VJO_TRIM_NONE, 1, 0, 0, 960, 544);
}

/* An all-black frame, and a text box under a black band over 3/10 of the frame: kept whole, decided by
 * the first 3 strips, whose rows above the strip in hand are that one row's colour: 9 strips, API 23's
 * bytes. A noisy black band (more than one colour): read again from row 0, 3 + 9 strips, the same bytes. */
static void api32_black_frames_are_kept_whole(void)
{
    VjoGameOcrResult out;
    size_t len;
    reset(); black_frame();
    shot_speed = VJO_GAME_OCR_S_TRIM;
    len = reference(0, 0, 960, 544, 70);
    TEST_CHECK(shot(&out) == VJO_OK && jpeg_is_want(len) && out.captures == 9 && frames == 9);
    check_trim_step(&out, VJO_TRIM_KEPT, 0, 0, 0, 960, 544);
    reset(); black_frame(); picture_at(20, 380, 920, 150);
    shot_speed = VJO_GAME_OCR_S_TRIM;
    len = reference(0, 0, 960, 544, 70);
    TEST_CHECK(shot(&out) == VJO_OK && jpeg_is_want(len) && out.captures == 9);
    check_trim_step(&out, VJO_TRIM_KEPT, 0, 0, 0, 960, 544);
    reset(); black_frame(); picture_at(20, 380, 920, 150);
    for (uint32_t i = 0; i < 300 * 960; i += 7) frame32[i] = 0xFF0A0A0Au; /* black, but not all one colour */
    shot_speed = VJO_GAME_OCR_S_TRIM;
    len = reference(0, 0, 960, 544, 70);
    TEST_CHECK(shot(&out) == VJO_OK && jpeg_is_want(len) && out.captures == 12);
    check_trim_step(&out, VJO_TRIM_KEPT, 0, 0, 0, 960, 544);
    /* trim off: the same JPEG */
    reset(); black_frame(); picture_at(20, 380, 920, 150);
    for (uint32_t i = 0; i < 300 * 960; i += 7) frame32[i] = 0xFF0A0A0Au;
    TEST_CHECK(shot(&out) == VJO_OK && jpeg_is_want(len) && out.captures == 9 && !step_of(&out, VJO_GSTEP_TRIM, 0));
}

/* Too large at the first quality: the first pass reads on to the last row for the bars, 9 strips; a
 * trimmed picture whose guess holds is then encoded lower; one whose guess fails is read again at the
 * same quality first. A later try stops reading once it is too large, as API 23's did. */
static void noise_between_bars(void)
{
    uint32_t v = 99;
    black_frame();
    for (uint32_t y = 0; y < 544; y++) /* noise between 8-px bars: too large at 70 */
        for (uint32_t x = 8; x < 952; x++) {
            v = v * 1103515245u + 12345u;
            frame32[y * 960 + x] = 0xFF000000u | (v >> 8) | 0x00404040u;
        }
}
static void api32_too_large_after_a_trim(void)
{
    VjoGameOcrResult out;
    int rc;
    reset(); noise_between_bars();
    shot_speed = VJO_GAME_OCR_S_TRIM;
    rc = shot(&out);
    const VjoGameOcrStep *s0 = step_of(&out, VJO_GSTEP_SHOT, 0), *s1 = step_of(&out, VJO_GSTEP_SHOT, 1);
    TEST_ASSERT(s0 && s1);
    TEST_CHECK(s0->kind == 70 && s0->rc == VJO_E_TOO_LARGE && s0->sum == 9 && s1->kind == 50 && s1->arg == 2);
    check_trim_step(&out, VJO_TRIM_TRIMMED, 1, 6, 0, 948, 544);
    if (rc == VJO_OK) TEST_CHECK(jpeg_is_want(reference(6, 0, 948, 544, (int)out.shot_quality)));
    printf("measure: 948x544 noise between bars: rc %d after %u tries (q%u), %u B, %u strips\n", rc, out.shot_tries,
           out.shot_quality, out.shot_bytes, out.captures);
    /* the guess fails (a full row at the bottom): the whole frame at 70 again, then lower */
    reset(); noise_between_bars();
    for (uint32_t x = 0; x < 960; x++) frame32[540 * 960 + x] = 0xFF808080u;
    shot_speed = VJO_GAME_OCR_S_TRIM;
    rc = shot(&out);
    s0 = step_of(&out, VJO_GSTEP_SHOT, 0), s1 = step_of(&out, VJO_GSTEP_SHOT, 1);
    TEST_ASSERT(s0 && s1);
    TEST_CHECK(s0->kind == 70 && s0->rc == VJO_E_TOO_LARGE && s0->sum == 9 && s1->kind == 70 && s1->arg == 2);
    check_trim_step(&out, VJO_TRIM_NONE, 2, 0, 0, 960, 544);
    TEST_CHECK(out.shot_tries >= 2 && out.shot_tries <= 4);
    for (unsigned t = 2; t < out.shot_tries; t++) { /* 70 (again), 50, 35 */
        const VjoGameOcrStep *st = step_of(&out, VJO_GSTEP_SHOT, t);
        TEST_CHECK_(st && st->kind == (t == 2 ? 50 : 35) && st->arg == t + 1, "try %u", t + 1);
    }
    printf("measure: 948x544 noise, guess failed: rc %d after %u tries (q%u), %u B, %u strips\n", rc, out.shot_tries,
           out.shot_quality, out.shot_bytes, out.captures);
    /* API 23's noise frame (too large at 70 and 50) with 8-px black bars in its first strip only: the guess is
     * wrong and too large, the whole frame is read again at 70, then 50, then 35, a fourth try */
    {
        uint32_t v = 12345;
        reset();
        for (uint32_t i = 0; i < 960 * 544; i++) {
            v = v * 1103515245u + 12345u;
            frame32[i] = 0xFF000000u | (v >> 8);
        }
        for (uint32_t y = 0; y < 64; y++)
            for (uint32_t x = 0; x < 8; x++) frame32[y * 960 + x] = frame32[y * 960 + 959 - x] = 0xFF000000u;
        shot_speed = VJO_GAME_OCR_S_TRIM;
        rc = shot(&out);
        TEST_CHECK_(rc == VJO_OK && out.shot_tries == 4 && out.shot_quality == 35 && jpeg_is_want(reference(0, 0, 960, 544, 35)),
                    "rc %d, %u tries, q%u", rc, out.shot_tries, out.shot_quality);
        static const unsigned q[4] = {70, 70, 50, 35};
        for (unsigned t = 0; t < 4; t++) {
            const VjoGameOcrStep *st = step_of(&out, VJO_GSTEP_SHOT, t);
            TEST_CHECK_(st && st->kind == q[t] && st->arg == t + 1 && st->rc == (t == 3 ? 0 : VJO_E_TOO_LARGE), "try %u", t + 1);
        }
        check_trim_step(&out, VJO_TRIM_NONE, 2, 0, 0, 960, 544);
    }
}

/* The kernel strip (the hook cannot write the block): the same trimmed JPEG, 16 rows a call; a cancel while
 * the first strips are read (all black, before the guess): no file, nothing held, no trim step; and a shot
 * asking for any other speed bit is refused by the worker as by the kernel. */
static void api32_the_kernel_strip_a_cancel_and_other_bits(void)
{
    VjoGameOcrResult out;
    size_t len;
    reset(); black_frame(); picture_at(240, 136, 480, 272);
    shot_speed = VJO_GAME_OCR_S_TRIM;
    hook_copy_fail = 1;
    len = reference(238, 134, 484, 276, 70);
    TEST_CHECK(shot(&out) == VJO_OK && jpeg_is_want(len) && out.source == VJO_GAME_OCR_SRC_KSTRIP);
    check_trim_step(&out, VJO_TRIM_TRIMMED, 1, 238, 134, 484, 276);
    reset(); black_frame(); picture_at(240, 136, 480, 272);
    shot_speed = VJO_GAME_OCR_S_TRIM;
    cancel_after = 2;
    TEST_CHECK(shot(&out) == VJO_E_CANCELLED && frames == 2 && !kernel_files && file_size(jpg_path) < 0);
    TEST_CHECK(!step_of(&out, VJO_GSTEP_TRIM, 0));
    /* the worker's own check: a claimed shot whose speed has another bit (past the kernel's check) */
    static const uint32_t forged[] = {VJO_GAME_OCR_S_TRIM | VJO_GAME_OCR_S_BOOST, VJO_GAME_OCR_S_GATE, 0x20, 0x80};
    for (unsigned i = 0; i < sizeof(forged) / sizeof(forged[0]); i++) {
        static VjoGameOcrRequest r, took;
        static VjoGameOcrResult res;
        int seq;
        reset(); procedural_frame();
        memset(&r, 0, sizeof(r));
        r.size = sizeof(r);
        r.flags = VJO_GAME_OCR_F_SHOT;
        memcpy(r.shot, CARD, sizeof(CARD));
        caller = 7; TEST_ASSERT(!vjoOcrRegister());
        caller = 3; TEST_ASSERT((seq = vjoOcrSubmit(&r)) > 0);
        caller = 7; memset(&took, 0, sizeof(took)); TEST_ASSERT(!vjoOcrTake(&took));
        took.speed = forged[i];
        TEST_CHECK_(vjo_game_ocr_run_request(&took, &res) == VJO_E_SOURCE && !block_allocs && !frames,
                    "speed 0x%X: rc %d", forged[i], res.rc);
        TEST_CHECK(!vjoOcrComplete(&res));
    }
}

/* A valid trimmed screenshot crosses the enlarged hook request transport first. A forged source pick
 * after Take must still be refused by the real game worker before it captures or writes a picture. */
static void screenshots_reject_text_source_picks(void)
{
    static const uint32_t picks[] = {1u, VJO_HOOK_STREAMS, VJO_HOOK_PICK_OCR,
                                     VJO_HOOK_STREAMS + 1u, 0xFFFFFFFFu};
    for (unsigned trim = 0; trim < 2; trim++) {
        for (unsigned i = 0; i < sizeof(picks) / sizeof(picks[0]); i++) {
            static VjoGameOcrRequest r, took;
            static VjoGameOcrResult res, out;
            int seq;
            reset(); procedural_frame();
            memset(&r, 0, sizeof(r));
            r.size = sizeof(r);
            r.flags = VJO_GAME_OCR_F_SHOT;
            r.speed = trim ? VJO_GAME_OCR_S_TRIM : 0u;
            memcpy(r.shot, CARD, sizeof(CARD));
            caller = 7; TEST_ASSERT(!vjoOcrRegister());
            caller = 3; TEST_ASSERT((seq = vjoOcrSubmit(&r)) > 0);
            caller = 7; memset(&took, 0, sizeof(took)); TEST_ASSERT(!vjoOcrTake(&took));
            TEST_CHECK(took.size == sizeof(took) && took.speed == r.speed &&
                       took.hook_pick == VJO_HOOK_PICK_AUTO);
            took.hook_pick = picks[i];
            TEST_CHECK_(vjo_game_ocr_run_request(&took, &res) == VJO_E_SOURCE && !block_allocs && !frames &&
                        !file_opens && !kernel_files && !res.text[0],
                        "shot speed 0x%X, pick 0x%X: rc %d", took.speed, picks[i], res.rc);
            TEST_CHECK(!vjoOcrComplete(&res));
            caller = 3; memset(&out, 0, sizeof(out));
            TEST_CHECK(!vjoOcrRead((uint32_t)seq, &out) && out.size == sizeof(out) &&
                       out.rc == VJO_E_SOURCE && !out.text[0]);
            TEST_CHECK(!block_live && shot_uid < 0 && file_size(jpg_path) < 0 &&
                       vjo_ledger_current(&g_game_ledger, GL_SHOT) == 0);
        }
    }
}

/* A PSP game at 1x with one grey pixel in its left bar, at rows 186-249: the first strip read after the guess
 * starts at the guess's last band (row 182) and scans from its 11th row (row 192, game_ocr/main.c shot_pass:
 * band + (s->rows - have) * stride), so every one of those rows is scanned once: the frame is kept whole, the
 * pixel in its JPEG (audit-blackbars F1; a scan of the strip's first rows instead lost it). */
static void api32_a_bar_pixel_in_the_realigned_strip(void)
{
    VjoGameOcrResult out;
    size_t len;
    for (uint32_t row = 186; row < 250; row += 3) {
        reset(); black_frame(); picture_at(240, 136, 480, 272);
        frame32[row * 960 + 5] = 0xFF808080u;
        len = reference(0, 0, 960, 544, 70); /* the whole frame, its pixel included */
        shot_speed = VJO_GAME_OCR_S_TRIM;
        TEST_CHECK_(shot(&out) == VJO_OK && jpeg_is_want(len), "the pixel at row %u: the JPEG is not the whole frame", row);
        const VjoGameOcrStep *t = step_of(&out, VJO_GSTEP_TRIM, 0);
        /* how 1: the pixel in the guess's own strip (rows 128-191) kept it whole; 2: below it, read again */
        TEST_CHECK_(t && t->kind == VJO_TRIM_KEPT && t->arg == (row < 192 ? 1 : 2) && t->sum == (960u << 16 | 544u),
                    "the pixel at row %u: kind %u how %u, %ux%u", row, t ? t->kind : 9, t ? t->arg : 9,
                    t ? t->sum >> 16 : 0, t ? t->sum & 0xFFFF : 0);
    }
}

/* A frame whose first rows reach its top edge and one side's, the other side a bar: decided by those rows
 * (how 0, kept whole: one bar alone), in 9 strips; with both side edges reached as well (audit-blackbars N1). */
static void api32_one_side_edge_decides_with_the_first_rows(void)
{
    static const struct { uint32_t x, w; } pictures[] = {{0, 900}, {60, 900}, {0, 960}};
    for (unsigned i = 0; i < sizeof(pictures) / sizeof(pictures[0]); i++) {
        VjoGameOcrResult out;
        reset(); black_frame(); picture_at(pictures[i].x, 0, pictures[i].w, 544);
        shot_speed = VJO_GAME_OCR_S_TRIM;
        TEST_CHECK_(shot(&out) == VJO_OK && jpeg_is_want(reference(0, 0, 960, 544, 70)) && out.captures == 9,
                    "picture %u: %u strips", i, out.captures);
        check_trim_step(&out, pictures[i].w == 960 ? VJO_TRIM_NONE : VJO_TRIM_KEPT, 0, 0, 0, 960, 544);
    }
}

/* A frame that changes size while a trimmed shot reads it (VJO_ERR_FORMAT from the hook): in its first rows,
 * before the guess, no trim step (the pass never ended); in the second pass of a wrong guess, the first pass's
 * step is there. Either way no file and nothing held (shot() checks); a cancel in the second pass too
 * (audit-blackbars N2). */
static void api32_a_resize_or_a_cancel_with_trim(void)
{
    VjoGameOcrResult out;
    for (unsigned k = 0; k < 3; k++) {
        reset(); black_frame(); picture_at(117, 0, 726, 544); picture_at(0, 300, 960, 40); /* the wrong guess */
        shot_speed = VJO_GAME_OCR_S_TRIM;
        if (k == 0) resize_after = 1;  /* the first strip in, the second resized */
        if (k == 1) resize_after = 12; /* the second pass's 3rd strip */
        if (k == 2) cancel_after = 13;
        int rc = shot(&out);
        TEST_CHECK_(rc == (k == 2 ? VJO_E_CANCELLED : VJO_E_SOURCE) && file_size(jpg_path) < 0 && !kernel_files,
                    "case %u: rc %d", k, rc);
        if (k == 0) TEST_CHECK(!step_of(&out, VJO_GSTEP_TRIM, 0));
        if (k == 1) check_trim_step(&out, VJO_TRIM_NONE, 2, 0, 0, 960, 544); /* (a cancelled job returns no result) */
    }
}

/* The most strips a shot reads without a JPEG too large, 19: a top bar ending at a strip's end (the picture's
 * first row starts the 3rd strip, so its 2 px margin is read again from the strip before: 10 in the first
 * pass), then a wrong guess (a pixel in the bottom bar) read again, 9 more (audit-blackbars N4). */
static void api32_the_most_strips_a_shot_reads(void)
{
    VjoGameOcrResult out;
    reset(); black_frame(); picture_at(0, 128, 960, 288);
    frame32[500 * 960 + 480] = 0xFF101010u;
    shot_speed = VJO_GAME_OCR_S_TRIM;
    TEST_CHECK(shot(&out) == VJO_OK && jpeg_is_want(reference(0, 0, 960, 544, 70)));
    const VjoGameOcrStep *s0 = step_of(&out, VJO_GSTEP_SHOT, 0), *s1 = step_of(&out, VJO_GSTEP_SHOT, 1);
    TEST_ASSERT(s0 && s1);
    TEST_CHECK_(out.captures == 19 && s0->sum == 10 && s1->sum == 9 && out.shot_tries == 2, "%u strips (%u + %u), %u tries",
                out.captures, s0->sum, s1->sum, out.shot_tries);
    check_trim_step(&out, VJO_TRIM_KEPT, 2, 0, 0, 960, 544);
}

/* --shot FRAME.rgba W H FORMAT DIR [trim]: one shot of the given picture, in the given pixel
 * format (abgr, a2b10, a1b5), its JPEG left in DIR/ux0:data/VitaJPOverlay/anki_queue/; with
 * trim (API 32) its black bars trimmed; prints the result as JSON (host/tests/test_game_shot.py). */
static int shot_step(int argc, char **argv)
{
    VjoGameOcrResult out;
    FILE *f;
    uint32_t w, h;
    if (argc < 7) return 2;
    w = (uint32_t)atoi(argv[3]);
    h = (uint32_t)atoi(argv[4]);
    if (w != 960 || h != 544 || chdir(argv[6])) return 2;
    reset();
    if (!(f = fopen(argv[2], "rb"))) return 2;
    for (uint32_t i = 0; i < w * h; i++) {
        unsigned char p[4];
        if (fread(p, 1, 4, f) != 4) return 2;
        if (!strcmp(argv[5], "a1b5")) {
            fb_fmt = VJO_FB_A1B5G5R5;
            frame16[i] = (uint16_t)(0x8000u | (p[2] >> 3) << 10 | (p[1] >> 3) << 5 | p[0] >> 3);
        } else if (!strcmp(argv[5], "a2b10")) {
            fb_fmt = VJO_FB_A2B10G10R10;
            frame32[i] = 0xC0000000u | (uint32_t)p[2] << 22 | (uint32_t)p[1] << 12 | (uint32_t)p[0] << 2;
        } else {
            frame32[i] = (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
        }
    }
    fclose(f);
    shot_speed = argc > 7 && !strcmp(argv[7], "trim") ? VJO_GAME_OCR_S_TRIM : 0u;
    int rc = shot(&out);
    const VjoGameOcrStep *t = step_of(&out, VJO_GSTEP_TRIM, 0);
    printf("{\"rc\":%d,\"bytes\":%u,\"quality\":%u,\"tries\":%u,\"block\":%u,\"strips\":%u,\"path\":\"%s/%s\","
           "\"trim\":%d,\"how\":%d,\"x\":%u,\"y\":%u,\"w\":%u,\"h\":%u}\n", rc,
           out.shot_bytes, out.shot_quality, out.shot_tries, out.shot_block, out.captures, argv[6], jpg_path,
           t ? t->kind : -1, t ? t->arg : -1, t ? t->bytes >> 16 : 0u, t ? t->bytes & 0xFFFFu : 0u,
           t ? t->sum >> 16 : w, t ? t->sum & 0xFFFFu : h);
    return rc ? 1 : 0;
}

TEST_LIST = {
    {"API 23: the shot is the whole frame, encoded in the game and written beside the card by the kernel",
     the_shot_is_the_whole_frame_written_beside_the_card},
    {"API 23: the game's memory decides: the reserve, USER_RW, then CDRAM, else none", the_game_memory_decides},
    {"API 23: the kernel strip rung gives the same JPEG", the_kernel_strip_rung_gives_the_same_jpeg},
    {"API 23: too large is encoded lower, then given up", too_large_is_encoded_lower_then_given_up},
    {"API 23: a refused file, a cancellation, a resized frame leave no file", failures_leave_no_file},
    {"API 23: five shots leave the slot, the kernel and the game as they were", a_shot_leaves_the_slot_as_it_found_it},
    {"API 23: a slow card's save is waited for; one given up leaves no file and no blocked worker",
     a_slow_card_is_waited_for_and_never_blocks_the_worker},
    {"API 23: all JPEG chunks and the close share one file deadline", all_file_calls_share_one_deadline},
    {"API 32: untrimmed frames are byte-identical, with the option or without", api32_untrimmed_frames_are_byte_identical},
    {"API 32: Adrenaline's layouts of a PSP game are trimmed, in 9 strips", api32_adrenaline_layouts_are_trimmed},
    {"API 32: a pillarbox, and a wrong guess read again", api32_a_pillarbox_and_a_wrong_guess},
    {"API 32: black frames are kept whole", api32_black_frames_are_kept_whole},
    {"API 32: too large after a trim", api32_too_large_after_a_trim},
    {"API 32: the kernel strip, a cancel, and other speed bits", api32_the_kernel_strip_a_cancel_and_other_bits},
    {"combined ABI: screenshots reject text source picks with trimming off and on", screenshots_reject_text_source_picks},
    {"API 32: a bar pixel in the strip read after the guess", api32_a_bar_pixel_in_the_realigned_strip},
    {"API 32: the first rows decide with one side's edge", api32_one_side_edge_decides_with_the_first_rows},
    {"API 32: a resize or a cancel while a trimmed shot reads", api32_a_resize_or_a_cancel_with_trim},
    {"API 32: the most strips a shot reads", api32_the_most_strips_a_shot_reads},
    {NULL, NULL}
};


/* The tests' own memory card. acutest_main ends in exit() and runs each test in a child
 * that exits through atexit as well, so the owner process removes it, with every file the
 * tests left in it, from an atexit handler (test_tmpdir.h). */
int main(int argc, char **argv)
{
    const char *dir;
    if (argc >= 2 && !strcmp(argv[1], "--shot"))
        return shot_step(argc, argv);
    dir = vjo_test_tmpdir("game-shot");
    if (!dir || chdir(dir))
        return 2;
    return acutest_main(argc, argv);
}
