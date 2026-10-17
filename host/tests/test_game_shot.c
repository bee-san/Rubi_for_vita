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
 * later (its syscalls answer BUSY meanwhile), as when its two syncs wait for the card */
static uint64_t late_after_last_write_us, late_until;
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
    if (late_after_last_write_us && n < VJO_GAME_OCR_SHOT_CHUNK) late_until = clock_us + late_after_last_write_us;
    return (int)pwrite(file_fd, data, n, (off_t)off);
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
int ksceIoSync(const char *device, unsigned int unk) { in_kernel_worker(); TEST_CHECK(!strcmp(device, "ux0:") && !unk); syncs++; return 0; }

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
    late_after_last_write_us = late_until = 0;
    kstrip_live = 0; kstrip_allocs = 0;
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
    TEST_CHECK(kernel_files == 1 && renames == 1 && syncs == 2 && file_writes == (out.shot_bytes + 32767u) / 32768u);
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

/* A slow memory card: the kernel worker saves the JPEG 10 s after its last piece (its two syncs): the
 * worker waits for it, and the card has its screenshot; one 40 s late is given up, its file removed,
 * and the worker is not left blocked: the next screenshot works. */
static void a_slow_card_is_waited_for_and_never_blocks_the_worker(void)
{
    VjoGameOcrResult out;
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

/* --shot FRAME.rgba W H FORMAT DIR: one shot of the given picture, in the given pixel
 * format (abgr, a2b10, a1b5), its JPEG left in DIR/ux0:data/VitaJPOverlay/anki_queue/;
 * prints the result as JSON (host/tests/test_game_shot.py). */
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
    int rc = shot(&out);
    printf("{\"rc\":%d,\"bytes\":%u,\"quality\":%u,\"tries\":%u,\"block\":%u,\"strips\":%u,\"path\":\"%s/%s\"}\n", rc,
           out.shot_bytes, out.shot_quality, out.shot_tries, out.shot_block, out.captures, argv[6], jpg_path);
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
