/* API 30, scan screen end to end in the game's process: the actual game worker (game_ocr/main.c run_spot,
 * core/vocr_ocr.c vjo_vocr_spot, vita-vn-ocr's runtime/vocr_textspot2.c at the pin) and the actual kernel
 * transport (kernel/game_ocr.c through kernel_model_broker_boundary.c's retail return model; kernel/capture.c's
 * rows as the display hook copies them from the game's frame; the model broker's file calls on real files, the
 * test's own memory card). The frame is procedural here; with --spot FRAME.rgba W H it is the given picture and
 * the result is printed as JSON (host/tests/test_game_spot.py compares it with vita-vn-ocr's twin). The spotter's
 * weights come from cmake/VocrSpot.cmake (VJO_VOCR_SPOT_TEST_DIR); the tests that need them skip without.
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
#include "../../game_ocr/main.c"
#include "vocr_textspot2.h"

#ifndef VJO_VOCR_SPOT_TEST_DIR
#define VJO_VOCR_SPOT_TEST_DIR ""
#endif

VjoKernelState g;
unsigned syscall_depth;
static SceUID caller;
static unsigned locks;
static uint64_t clock_us;

/* ---- the game's frame (what the display hook sees) ---- */
static uint32_t frame32[960 * 544]; /* A8B8G8R8 */
static uint32_t fb_w = 960, fb_h = 544;
static int hook_copy_fail;          /* the hook cannot write the game's block (the kernel strip rung) */
static int resize_after;            /* the frame changes size after this many frames (0: never) */
static int cancel_after;            /* the Shell cancels after this many frames (0: never) */
static unsigned frames, in_hook;
static uint32_t cur_seq;

/* ---- the game's memory: the scan's two blocks ---- */
static unsigned char strip_mem[256u * 1024u] __attribute__((aligned(64)));      /* "VjoGameVocrCapture" */
static unsigned char work_mem[1024u * 1024u] __attribute__((aligned(64)));      /* "VjoGameVocr" */
static int strip_live, work_live, refuse_user, refuse_all;
static unsigned strip_bytes, work_bytes, allocs, frees, live_bytes, peak_bytes, cdram_allocs;
static int64_t game_free = 64 * 1024 * 1024;

/* ---- the memory card: the model broker's files ---- */
static int model_fd = -1;
static unsigned model_opens, model_reads;

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
{ (void)seq; (void)off; (void)bytes; (void)src; TEST_CHECK(0); return -1; }
int vjoOcrShotSave(uint32_t seq, uint32_t bytes) { (void)seq; (void)bytes; TEST_CHECK(0); return -1; }

/* ---- the kernel's platform ---- */
void klog(const char *format, ...) { (void)format; }
SceUID ksceKernelGetProcessId(void) { return caller; }
int ksceKernelGetThreadId(void) { return 0x7001; }
uint64_t ksceKernelGetSystemTimeWide(void) { return clock_us; }
int ksceKernelSetEventFlag(SceUID uid, unsigned bits) { (void)uid; (void)bits; return 0; }
int ksceKernelClearEventFlag(SceUID uid, unsigned bits) { (void)uid; (void)bits; return 0; }
int ksceKernelDelayThread(SceUInt us) { clock_us += us; return 0; }
SceUID ksceKernelFindMemBlockByAddr(const void *addr, SceSize size) { (void)addr; (void)size; return 40; }
/* The strip block is memory of the game process 7. */
SceUID ksceKernelFindProcMemBlockByAddr(SceUID pid, const void *addr, SceSize size)
{
    const unsigned char *p = addr;
    TEST_CHECK(size == 0);
    return pid == 7 && strip_live && p >= strip_mem && p < strip_mem + strip_bytes ? 41 : -1;
}
static void kernel_worker(void)
{
    SceUID saved = caller;
    unsigned depth = syscall_depth;
    caller = 0;
    syscall_depth = 0;
    game_ocr_model_tick();
    caller = saved;
    syscall_depth = depth;
}
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
        game_ocr_rows_hook(7, (uintptr_t)frame32, fb_w, VJO_FB_A8B8G8R8, fb_w, fb_h);
        in_hook = 0;
        if (out) *out = MEV_ROWS;
        return 0;
    }
    if (bits == MEV_DONE) {
        kernel_worker();
        if (out) *out = MEV_DONE;
        return 0;
    }
    clock_us += *timeout;
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

/* The memory card: the kernel worker opens, sizes, reads and closes the models' files (real files). */
static void in_kernel_worker(void) { TEST_ASSERT(caller == 0 && !locks && !syscall_depth); }
SceUID ksceIoOpen(const char *path, int flags, unsigned mode)
{
    (void)mode;
    in_kernel_worker();
    TEST_CHECK(flags == SCE_O_RDONLY && !strncmp(path, VJO_VOCR_MODEL_ROOT "/", sizeof(VJO_VOCR_MODEL_ROOT)));
    TEST_ASSERT(model_fd < 0);
    model_opens++;
    model_fd = open(path, O_RDONLY);
    return model_fd >= 0 ? 32 : (int)0x80010002u; /* ENOENT */
}
SceOff ksceIoLseek(SceUID fd, SceOff off, int whence)
{ in_kernel_worker(); TEST_ASSERT(fd == 32 && model_fd >= 0); return lseek(model_fd, (off_t)off, whence); }
int ksceIoPread(SceUID fd, void *out, SceSize n, SceOff off)
{ in_kernel_worker(); TEST_ASSERT(fd == 32 && model_fd >= 0); model_reads++; return (int)pread(model_fd, out, n, (off_t)off); }
int ksceIoPwrite(SceUID fd, const void *data, SceSize n, SceOff off)
{ (void)fd; (void)data; (void)n; (void)off; TEST_CHECK(0); return -1; }
int ksceIoChstatByFd(SceUID fd, SceIoStat *st, int bits) { (void)fd; (void)st; (void)bits; TEST_CHECK(0); return -1; }
int ksceIoClose(SceUID fd)
{
    in_kernel_worker();
    TEST_ASSERT(fd == 32 && model_fd >= 0);
    TEST_CHECK(close(model_fd) == 0);
    model_fd = -1;
    return 0;
}
int ksceIoRename(const char *from, const char *to) { (void)from; (void)to; TEST_CHECK(0); return -1; }
int ksceIoRemove(const char *path) { (void)path; TEST_CHECK(0); return -1; }
int ksceIoSync(const char *device, unsigned int unk) { (void)device; (void)unk; TEST_CHECK(0); return -1; }
int ksceIoSyncByFd(SceUID fd, int flag) { (void)fd; (void)flag; TEST_CHECK(0); return -1; }

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
    int strip = !strcmp(name, "VjoGameVocrCapture");
    (void)opt;
    TEST_CHECK(strip || !strcmp(name, "VjoGameVocr"));
    if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW) TEST_CHECK(!(n % (256u * 1024u)));
    if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW) TEST_CHECK(!(n % (1024u * 1024u)));
    if (refuse_all || (refuse_user && type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW)) return (int)0x80020001u;
    TEST_CHECK(!(n & 4095u) && n <= (strip ? sizeof(strip_mem) : sizeof(work_mem)) && !(strip ? strip_live : work_live));
    if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW) cdram_allocs++;
    if (strip) { strip_live = 1; strip_bytes = n; }
    else { work_live = 1; work_bytes = n; }
    allocs++;
    live_bytes += n;
    if (live_bytes > peak_bytes) peak_bytes = live_bytes;
    return strip ? 11 : 9;
}
int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    TEST_CHECK((uid == 11 && strip_live) || (uid == 9 && work_live));
    *base = uid == 11 ? strip_mem : work_mem;
    return 0;
}
int sceKernelFreeMemBlock(SceUID uid)
{
    TEST_CHECK((uid == 11 && strip_live) || (uid == 9 && work_live));
    if (uid == 11) { strip_live = 0; live_bytes -= strip_bytes; }
    else { work_live = 0; live_bytes -= work_bytes; }
    frees++;
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
int scePowerSetArmClockFrequency(int mhz) { (void)mhz; TEST_CHECK(0); return 0; } /* a scan never boosts */
int sceKernelGetProcessTitleId(SceUID pid, char *out, SceSize size) { (void)pid; snprintf(out, size, "PCSG00415"); return 0; }
int sceAppMgrAppParamGetString(int pid, int param, char *out, SceSize size) { (void)pid; (void)param; (void)out; (void)size; return -1; }
int sceDisplayGetFrameBuf(SceDisplayFrameBuf *fb, int sync) { (void)fb; (void)sync; return -1; }
int sceKernelGetMemBlockInfoByAddr(void *base, SceKernelMemBlockInfo *info) { (void)base; (void)info; return -1; }
int vjoGetVersion(void) { return VJO_API_VERSION; }
SceUID sceIoOpen(const char *path, int flags, unsigned mode) { (void)path; (void)flags; (void)mode; return (int)0x80010013u; }

/* ---- the harness ---- */
#define SPOT_PATH VJO_VOCR_MODEL_ROOT "/" VJO_VOCR_SPOT_NAME
static unsigned char blob[VJO_VOCR_SPOT_BYTES];
static int have_blob;

static int read_blob(void)
{
    char path[600];
    FILE *f;
    if (!VJO_VOCR_SPOT_TEST_DIR[0]) return 0;
    snprintf(path, sizeof(path), "%s/%s", VJO_VOCR_SPOT_TEST_DIR, VJO_VOCR_SPOT_NAME);
    if (!(f = fopen(path, "rb"))) return 0;
    have_blob = fread(blob, 1, sizeof(blob), f) == sizeof(blob) && fgetc(f) == EOF;
    fclose(f);
    return have_blob;
}

/* The card's spotter file: the pinned one, one with a byte changed, one byte short, or none. */
enum { CARD_PINNED, CARD_CHANGED, CARD_SHORT, CARD_NONE };
static void card(int what)
{
    FILE *f;
    unlink(SPOT_PATH);
    if (what == CARD_NONE) return;
    TEST_ASSERT((f = fopen(SPOT_PATH, "wb")) != NULL);
    unsigned char copy[VJO_VOCR_SPOT_BYTES];
    memcpy(copy, blob, sizeof(copy));
    if (what == CARD_CHANGED) copy[sizeof(copy) / 2] ^= 1;
    TEST_ASSERT(fwrite(copy, 1, what == CARD_SHORT ? sizeof(copy) - 1 : sizeof(copy), f) > 0);
    fclose(f);
}

/* A VN-like 960 x 544 frame: a gradient, a dark box at the bottom with light text-like strokes. */
static void procedural_frame(void)
{
    for (uint32_t y = 0; y < 544; y++)
        for (uint32_t x = 0; x < 960; x++) {
            uint32_t v = 120 + x * 60 / 960 + y * 40 / 544, r = v, gg = v, b = v + 20;
            if (y > 380 && y < 520 && x > 30 && x < 930) r = gg = b = 20;
            if (((y > 400 && y < 430) || (y > 450 && y < 480)) && x > 60 && x < 700 && (x / 4) % 7 < 3) r = gg = b = 235;
            frame32[y * 960 + x] = 0xFF000000u | b << 16 | gg << 8 | r;
        }
}

static void reset(void)
{
    memset(&g, 0, sizeof(g));
    game_ocr_init();
    worker = workspace_uid = weights_uid = capture_uid = line_uid = lm_uid = cascade_uid = shot_uid = -1;
    model_owned = 0; model_seq = 0; quitting = active_seq = 0; worker_started = claim_outstanding = blocked = 0;
    io_error = 0; hook_owns_capture = 0; hook_owns_workspace = 0;
    locks = syscall_depth = 0; clock_us = 1000000;
    frames = in_hook = 0; hook_copy_fail = resize_after = cancel_after = 0;
    fb_w = 960; fb_h = 544;
    strip_live = work_live = refuse_user = refuse_all = 0;
    allocs = frees = live_bytes = peak_bytes = cdram_allocs = 0;
    game_free = 64 * 1024 * 1024;
    model_fd = -1; model_opens = model_reads = 0;
    kstrip_live = 0; kstrip_allocs = 0;
    g.lock = g.ievf = 1; g.evf = 2; g.mevf = 5; g.shell_pid = 3; g.game_pid = 7; g.game_active = VJO_GAME; g.ocr_epoch = 1;
    g.fb_w = 960; g.fb_h = 544; g.done_seq = g.capture_seq = 11;
    g.region = (VjoRect){2000, 40000, 60000, 20000}; /* the user's textbox: a scan reads the whole frame */
    mkdir("ux0:data", 0777);
    mkdir("ux0:data/VitaJPOverlay", 0777);
    mkdir(VJO_VOCR_MODEL_ROOT, 0777);
    card(have_blob ? CARD_PINNED : CARD_NONE);
}

static VjoGameOcrRequest scan_request(void)
{
    VjoGameOcrRequest r;
    memset(&r, 0, sizeof(r));
    r.size = sizeof(r);
    r.flags = VJO_GAME_OCR_F_FULL;
    r.speed = VJO_GAME_OCR_S_SPOT;
    snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR_SPOT_NAME);
    snprintf(r.model_dir, sizeof(r.model_dir), "%s", VJO_VOCR_MODEL_ROOT);
    return r;
}

/* The Shell submits r (a scan unless changed), the worker claims it, runs it and completes it, and the Shell
 * reads the result (out). Returns the job's rc, or the Submit's refusal (then nothing ran). */
static int scan_with(VjoGameOcrRequest r, VjoGameOcrResult *out)
{
    static VjoGameOcrRequest took;
    static VjoGameOcrResult res;
    int seq;
    memset(out, 0, sizeof(*out));
    caller = 7; TEST_ASSERT(!vjoOcrRegister());
    caller = 3;
    if ((seq = vjoOcrSubmit(&r)) <= 0) return seq;
    cur_seq = (uint32_t)seq;
    caller = 7; memset(&took, 0, sizeof(took)); TEST_ASSERT(!vjoOcrTake(&took));
    vjo_game_ocr_run_request(&took, &res);
    TEST_CHECK(!vjoOcrComplete(&res));
    caller = 3;
    int rr = vjoOcrRead((uint32_t)seq, out);
    if (res.rc == VJO_E_CANCELLED) TEST_CHECK(rr == VJO_ERR_CANCELLED);
    else TEST_CHECK(!rr && out->seq == (uint32_t)seq);
    /* whatever happened: nothing held, in the game or the kernel */
    TEST_CHECK(!strip_live && !work_live && workspace_uid < 0 && capture_uid < 0 && !model_owned &&
               !game_ocr_capture_busy_locked() && model_fd < 0 && !kstrip_live && !locks && !blocked);
    TEST_CHECK(vjo_ledger_current(&g_game_ledger, GL_WORKSPACE) == 0 &&
               vjo_ledger_current(&g_game_ledger, GL_CAPTURE) == 0);
    return res.rc;
}

static int scan(VjoGameOcrResult *out) { return scan_with(scan_request(), out); }

static const VjoGameOcrStep *step_of(const VjoGameOcrResult *r, unsigned id)
{
    for (uint32_t i = 0; i < r->n_steps && i < VJO_GAME_OCR_STEPS; i++)
        if (r->step[i].id == id) return &r->step[i];
    return NULL;
}

static const VjoSpotScan *scan_of(const VjoGameOcrResult *r) { return (const VjoSpotScan *)(const void *)r->text; }

/* The runtime on the same frame in memory, grey as the hook makes it: what the scan's strips must give. */
static void in_memory(VocrTextspotSet *set, uint32_t w, uint32_t h)
{
    static unsigned char grey[960 * 544], arena[VJO_VOCR_SPOT_ARENA];
    vocr_textspot2 m;
    for (uint32_t i = 0; i < w * h; i++) {
        uint32_t p = frame32[(i / w) * fb_w + i % w];
        grey[i] = (unsigned char)((77u * (p & 0xFFu) + 150u * (p >> 8 & 0xFFu) + 29u * (p >> 16 & 0xFFu)) >> 8);
    }
    VocrLinesImage im = {grey, w, h, w, VOCR_LINES_GREY};
    TEST_ASSERT(vocr_textspot2_load(&m, blob, sizeof(blob)) == VOCR_LINES_OK);
    TEST_ASSERT(vocr_textspot2_find(&m, &im, VOCR_TEXTSPOT_NO_DARK, arena, sizeof(arena), set) == VOCR_LINES_OK);
}

#define NEED_BLOB() do { if (!have_blob) { TEST_SKIP("no spotter weights (cmake/VocrSpot.cmake)"); return; } } while (0)

/* The whole frame, its grey rows once top to bottom, 64 a frame through the hook (9 frames), no capture
 * published; the set equals the runtime's on the same frame in memory; the map is the set's. */
static void a_scan_reads_the_whole_frame_once(void)
{
    VjoGameOcrResult out;
    static VocrTextspotSet want;
    NEED_BLOB();
    reset();
    procedural_frame();
    TEST_CHECK(scan(&out) == VJO_OK && !out.rc);
    TEST_CHECK(out.width == 960 && out.height == 544 && !out.region_x && !out.region_y);
    TEST_CHECK(out.source == VJO_GAME_OCR_SRC_HOOK && out.captures == 9 && frames == 9 && out.strip_rows == 64);
    TEST_CHECK(g.done_seq == 11 && !kstrip_allocs); /* no capture published, no kernel block */
    TEST_CHECK(out.weights == VJO_GAME_OCR_WEIGHTS_READ && model_opens == 1 && out.lines == 0 && !out.recognized);
    TEST_CHECK(out.workspace_bytes == vjo_vocr_spot_bytes() && out.reserve_bytes == VJO_GAME_RESERVE);
    TEST_CHECK(out.need_workspace == ((vjo_vocr_spot_bytes() + 4095u) & ~4095u) + 64u * 960u &&
               peak_bytes == out.need_workspace && allocs == 2 && frees == 2);
    TEST_CHECK(vjo_ledger_peak(&g_game_ledger, GL_WORKSPACE) == ((vjo_vocr_spot_bytes() + 4095u) & ~4095u));
    const VjoGameOcrStep *s = step_of(&out, VJO_GSTEP_SPOT);
    TEST_ASSERT(s != NULL);
    in_memory(&want, 960, 544);
    const VjoSpotScan *got = scan_of(&out);
    TEST_CHECK(!memcmp(&got->set, &want, sizeof(want)));
    TEST_MSG("the scan's set differs from the runtime's on the frame in memory");
    TEST_CHECK(s->kind == 1 && !s->rc && s->arg == want.count && s->bytes == (want.block_count << 16 | (uint32_t)(want.pick + 1) << 8 | want.truncated));
    vocr_textspot2 m;
    TEST_ASSERT(vocr_textspot2_load(&m, blob, sizeof(blob)) == VOCR_LINES_OK);
    TEST_CHECK(s->sum == vocr_textspot2_frame_arena_bytes(&m, 960, 544) && s->sum <= VJO_VOCR_SPOT_ARENA);
    VjoSpotMap map;
    vjo_spot_map(&got->set, 960, 544, &map);
    TEST_CHECK(!memcmp(&got->map, &map, sizeof(map)));
    /* the rest of the text is zero: it keeps its NUL */
    for (size_t i = sizeof(VjoSpotScan); i < sizeof(out.text); i++)
        if (out.text[i]) { TEST_CHECK(0); break; }
    printf("measure: 960x544 procedural scan: %u boxes, %u blocks, pick %d, arena %u B, workspace %u B, "
           "strip %u B, %u frames\n", want.count, want.block_count, want.pick, s->sum, out.workspace_bytes,
           64u * 960u, frames);
}

/* The arena the scan allocates is the runtime's own for any frame up to 960 x 544 with the pinned weights: vita-vn-ocr
 * #108's VTSP version 2 blob (the d2s head), which halves a frame wider than 480 px, so a PSP frame is read whole. */
static void the_pinned_arena_is_the_runtime_s(void)
{
    vocr_textspot2 m;
    NEED_BLOB();
    TEST_ASSERT(vocr_textspot2_load(&m, blob, sizeof(blob)) == VOCR_LINES_OK);
    TEST_CHECK(m.version == VOCR_TEXTSPOT2_VERSION_D2S && m.half_above == 480 && m.stride == 4);
    TEST_CHECK(vocr_textspot2_arena_bytes(&m, 960, 544) == VJO_VOCR_SPOT_ARENA);
    TEST_CHECK(vocr_textspot2_frame_arena_bytes(&m, 480, 272) <= VJO_VOCR_SPOT_ARENA &&
               vocr_textspot2_frame_arena_bytes(&m, 960, 544) <= VJO_VOCR_SPOT_ARENA);
    TEST_CHECK(((vjo_vocr_spot_bytes() + 4095u) & ~4095u) <= VJO_BUDGET_GAME_WORKSPACE &&
               vjo_vocr_spot_strip_bytes(960) <= VJO_BUDGET_GAME_CAPTURE);
    printf("measure: arena %u B up to 960x544 (%zu B at 480x272), workspace %zu B, strip %zu B\n",
           VJO_VOCR_SPOT_ARENA, vocr_textspot2_frame_arena_bytes(&m, 480, 272), vjo_vocr_spot_bytes(),
           vjo_vocr_spot_strip_bytes(960));
}

/* No weights on the card, a changed byte, a short file: no frame read, nothing held, the error kept. */
static void a_missing_or_wrong_file_reads_no_frame(void)
{
    VjoGameOcrResult out;
    reset(); card(CARD_NONE); procedural_frame();
    TEST_CHECK(scan(&out) == VJO_E_OCR_MODEL && !frames && out.weights == VJO_GAME_OCR_WEIGHTS_NONE && out.io_error);
    TEST_CHECK(!out.text[0] && step_of(&out, VJO_GSTEP_SPOT) && step_of(&out, VJO_GSTEP_SPOT)->rc == VJO_E_OCR_MODEL);
    if (!have_blob) return;
    reset(); card(CARD_CHANGED); procedural_frame();
    TEST_CHECK(scan(&out) == VJO_E_OCR_MODEL && !frames && model_reads >= 1 && !out.text[0]);
    reset(); card(CARD_SHORT); procedural_frame();
    TEST_CHECK(scan(&out) == VJO_E_OCR_MODEL && !frames && !model_reads && out.io_error == VJO_ERR_FORMAT);
    reset(); procedural_frame();
    TEST_CHECK(scan(&out) == VJO_OK); /* and the next scan works */
}

/* The game's memory: too little beside the reserve, nothing taken; USER_RW refused, CDRAM; none at all, none. */
static void the_game_memory_decides(void)
{
    VjoGameOcrResult out;
    const uint32_t need = ((vjo_vocr_spot_bytes() + 4095u) & ~4095u) + 64u * 960u;
    reset(); procedural_frame();
    game_free = VJO_GAME_RESERVE + need - 1;
    TEST_CHECK(scan(&out) == VJO_E_OOM && !allocs && !frames && !model_opens);
    TEST_CHECK(out.need_workspace == need && out.reserve_bytes == VJO_GAME_RESERVE);
    reset(); procedural_frame();
    refuse_all = 1;
    TEST_CHECK(scan(&out) == VJO_E_OOM && !frames && !model_opens);
    if (!have_blob) return;
    reset(); procedural_frame();
    refuse_user = 1;
    TEST_CHECK(scan(&out) == VJO_OK && cdram_allocs == 2 && out.capture_memory == VJO_GAME_OCR_MEM_CDRAM);
}

/* The Shell cancels mid-frame, the frame changes size mid-frame: no set, nothing held, the next scan works. */
static void a_failure_mid_frame_keeps_the_game_running(void)
{
    VjoGameOcrResult out;
    NEED_BLOB();
    reset(); procedural_frame();
    cancel_after = 3;
    TEST_CHECK(scan(&out) == VJO_E_CANCELLED && frames == 3);
    reset(); procedural_frame();
    resize_after = 2;
    TEST_CHECK(scan(&out) == VJO_E_SOURCE && !out.text[0]);
    const VjoGameOcrStep *p = step_of(&out, VJO_GSTEP_PASS);
    TEST_CHECK(p && p->rc == VJO_ERR_FORMAT); /* the native error, in the PASS step */
    resize_after = 0; fb_w = 960;
    TEST_CHECK(scan(&out) == VJO_OK);
}

/* The hook cannot write the game's block: the kernel strip, the same set. */
static void the_kernel_strip_rung_gives_the_same_set(void)
{
    VjoGameOcrResult out, hook;
    NEED_BLOB();
    reset(); procedural_frame();
    TEST_CHECK(scan(&hook) == VJO_OK);
    reset(); procedural_frame();
    hook_copy_fail = 1;
    TEST_CHECK(scan(&out) == VJO_OK && out.source == VJO_GAME_OCR_SRC_KSTRIP && kstrip_allocs == 9);
    TEST_CHECK(!memcmp(&scan_of(&out)->set, &scan_of(&hook)->set, sizeof(VjoSpotResult)));
}

/* Only a scan: the kernel refuses another shape (S_SPOT with tiers, another speed bit, another flag, no F_FULL,
 * a fallback) and an unknown speed bit; the spotter's file opens only for a scan, and a scan opens no model. */
static void the_kernel_and_the_worker_take_only_a_scan(void)
{
    VjoGameOcrResult out;
    VjoGameOcrRequest r;
    static VjoGameOcrRequest took;
    static VjoGameOcrResult res;
    reset();
    r = scan_request(); r.tiers = 2;
    TEST_CHECK(scan_with(r, &out) == VJO_ERR_ARG);
    r = scan_request(); r.speed |= VJO_GAME_OCR_S_GATE;
    TEST_CHECK(scan_with(r, &out) == VJO_ERR_ARG);
    r = scan_request(); r.speed |= VJO_GAME_OCR_S_BOOST;
    TEST_CHECK(scan_with(r, &out) == VJO_ERR_ARG);
    r = scan_request(); r.flags |= VJO_GAME_OCR_F_BLANK;
    TEST_CHECK(scan_with(r, &out) == VJO_ERR_ARG);
    r = scan_request(); r.flags = 0;
    TEST_CHECK(scan_with(r, &out) == VJO_ERR_ARG);
    r = scan_request(); snprintf(r.fallback, sizeof(r.fallback), "%s", VJO_VOCR5_S_NAME);
    TEST_CHECK(scan_with(r, &out) == VJO_ERR_ARG);
    r = scan_request(); r.speed = 0x40u; /* past VJO_GAME_OCR_S_ALL */
    TEST_CHECK(scan_with(r, &out) == VJO_ERR_ARG && VJO_GAME_OCR_S_ALL == 0x3Fu);
    TEST_CHECK(!frames && !model_opens);
    /* an OCR job by tiers may not open the spotter's file; a scan opens none of the models */
    r = scan_request(); r.speed = 0; r.flags = 0; r.tiers = 2; snprintf(r.model, sizeof(r.model), "%s", VJO_VOCR5_N_NAME);
    caller = 7; TEST_ASSERT(!vjoOcrRegister());
    caller = 3; TEST_ASSERT(vjoOcrSubmit(&r) > 0);
    caller = 7; TEST_ASSERT(!vjoOcrTake(&took));
    TEST_CHECK(test_kernel_model_open(took.seq, VJO_GAME_OCR_OPEN_SPOT, NULL) == VJO_ERR_ARG);
    res.size = sizeof(res); res.seq = took.seq; res.rc = VJO_E_CANCELLED;
    TEST_CHECK(!vjoOcrComplete(&res));
    caller = 3; vjoOcrCancel(took.seq);
    r = scan_request();
    caller = 3; TEST_ASSERT(vjoOcrSubmit(&r) > 0);
    caller = 7; TEST_ASSERT(!vjoOcrTake(&took));
    TEST_CHECK(test_kernel_model_open(took.seq, VJO_GAME_OCR_OPEN_MODEL, NULL) == VJO_ERR_ARG);
    TEST_CHECK(test_kernel_model_open(took.seq, VJO_GAME_OCR_OPEN_TIER + 1, NULL) == VJO_ERR_ARG);
    TEST_CHECK(test_kernel_model_open(took.seq, VJO_GAME_OCR_OPEN_GATE, NULL) == VJO_ERR_ARG);
    TEST_CHECK(test_kernel_model_open(took.seq, VJO_GAME_OCR_OPEN_LM, NULL) == VJO_ERR_ARG);
    res.seq = took.seq;
    TEST_CHECK(!vjoOcrComplete(&res));
    caller = 3; vjoOcrCancel(took.seq);
    /* a malformed scan handed to the worker itself (no kernel's check): refused before anything */
    r = scan_request(); r.seq = 5; r.capture_seq = 4; r.fb_width = r.width = 960; r.fb_height = r.height = 544;
    r.tiers = 1;
    caller = 7;
    vjo_game_ocr_run_request(&r, &out);
    TEST_CHECK(out.rc != VJO_OK && !allocs && !frames && !model_opens && !out.n_steps);
}

/* A PSP game's 480 x 272 frame: read without halving; the map's blocks on the screen at twice the size. */
static void a_psp_frame_is_mapped_to_the_screen(void)
{
    VjoGameOcrResult out;
    static VocrTextspotSet want;
    NEED_BLOB();
    reset();
    procedural_frame();
    for (uint32_t y = 0; y < 272; y++) /* the frame's top-left quarter, 960 apart, as a 480 x 272 frame */
        for (uint32_t x = 0; x < 480; x++) frame32[y * 480 + x] = frame32[(2 * y) * 960 + 2 * x];
    fb_w = 480; fb_h = 272; g.fb_w = 480; g.fb_h = 272;
    TEST_CHECK(scan(&out) == VJO_OK && out.width == 480 && out.height == 272 && out.captures == 5);
    in_memory(&want, 480, 272);
    TEST_CHECK(!memcmp(&scan_of(&out)->set, &want, sizeof(want)));
    const VjoSpotMap *m = &scan_of(&out)->map;
    for (int i = 0; i < m->count; i++) {
        const VjoSpotBlock *b = &scan_of(&out)->set.block[i];
        TEST_CHECK(m->rect[i][0] == (b->x * 2 > VJO_SPOT_MARGIN ? b->x * 2 - VJO_SPOT_MARGIN : 0));
        TEST_CHECK(m->rect[i][0] + m->rect[i][2] <= 960 && m->rect[i][1] + m->rect[i][3] <= 544);
    }
}

/* Off is as before: a scan leaves the slot as it found it, and the next OCR request is refused nothing
 * (the kernel's state, the game's memory and the worker's blocks are those before the scan). */
static void scans_leave_the_slot_as_they_found_it(void)
{
    VjoGameOcrResult out;
    reset(); procedural_frame();
    for (int i = 0; i < 5; i++)
        scan(&out);
    TEST_CHECK(allocs == frees && !live_bytes && !game_ocr_capture_busy_locked() && g.done_seq == 11);
}

/* The core's own refusals (core/vocr_ocr.c vjo_vocr_spot), before any file or row: a workspace or strip one byte
 * short, a misaligned workspace or result, a region past 960 x 544; each leaves the set empty (pick -1). */
static int no_open(void *ud, const char *path, VjoFile *out) { (void)ud; (void)path; (void)out; TEST_CHECK(0); return -1; }
static int no_strip(void *ud, unsigned y, unsigned n, unsigned char *dst, size_t stride, unsigned flags)
{ (void)ud; (void)y; (void)n; (void)dst; (void)stride; (void)flags; TEST_CHECK(0); return -1; }
static void the_core_refuses_short_or_misaligned_blocks(void)
{
    static unsigned char ws[320 * 1024] __attribute__((aligned(16))), strip[64 * 960];
    static uint32_t text[sizeof(VjoSpotResult) / 4 + 1];
    VjoPlatform p = {.file_open = no_open};
    VjoGreySource im = {NULL, 960, 544, no_strip, NULL, NULL};
    VjoVocrStats st;
    VjoSpotResult *out = (VjoSpotResult *)(void *)text;
    const size_t need = vjo_vocr_spot_bytes();
    TEST_ASSERT(need <= sizeof(ws) && vjo_vocr_spot_strip_bytes(960) == sizeof(strip) && !vjo_vocr_spot_strip_bytes(961));
    TEST_CHECK(vjo_vocr_spot(&p, "d", &im, ws, need - 1, strip, sizeof(strip), out, &st) == VJO_E_OOM && out->pick == -1);
    TEST_CHECK(vjo_vocr_spot(&p, "d", &im, ws, need, strip, sizeof(strip) - 1, out, &st) == VJO_E_OOM);
    TEST_CHECK(vjo_vocr_spot(&p, "d", &im, ws + 8, need, strip, sizeof(strip), out, &st) == VJO_E_OOM);
    TEST_CHECK(vjo_vocr_spot(&p, "d", &im, ws, need, strip, sizeof(strip),
                             (VjoSpotResult *)(void *)((unsigned char *)text + 2), &st) == VJO_E_OOM);
    im.width = 961;
    TEST_CHECK(vjo_vocr_spot(&p, "d", &im, ws, need, strip, sizeof(strip), out, &st) == VJO_E_OCR_REGION);
    TEST_CHECK(!out->count && !out->block_count && out->pick == -1 && !out->truncated);
}

/* --spot FRAME.rgba W H: one scan of the given picture; prints its set, map and figures as JSON. */
static int spot_step(int argc, char **argv)
{
    VjoGameOcrResult out;
    FILE *f;
    uint32_t w, h;
    if (argc < 5) return 2;
    w = (uint32_t)atoi(argv[3]);
    h = (uint32_t)atoi(argv[4]);
    if (!w || w > 960 || !h || h > 544) return 2;
    reset();
    if (!(f = fopen(argv[2], "rb"))) return 2;
    for (uint32_t i = 0; i < w * h; i++) {
        unsigned char p[4];
        if (fread(p, 1, 4, f) != 4) return 2;
        frame32[i] = (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
    }
    fclose(f);
    fb_w = g.fb_w = w;
    fb_h = g.fb_h = h;
    int rc = scan(&out);
    const VjoSpotScan *s = scan_of(&out);
    const VjoGameOcrStep *st = step_of(&out, VJO_GSTEP_SPOT);
    printf("{\"rc\":%d,\"weights\":%d,\"strips\":%u,\"arena\":%u,\"workspace\":%u,\"need\":%u,\"pick\":%d,"
           "\"truncated\":%u,\"boxes\":[", rc, have_blob, out.captures, st ? st->sum : 0u, out.workspace_bytes,
           out.need_workspace, (int)s->set.pick, s->set.truncated);
    for (uint32_t i = 0; i < s->set.count && i < VJO_SPOT_MAX; i++) {
        const VjoSpotBox *b = &s->set.box[i];
        printf("%s[%u,%u,%u,%u,%u,%u,%u,%u]", i ? "," : "", b->x, b->y, b->w, b->h, b->vertical, b->conf, b->dark,
               b->block);
    }
    printf("],\"blocks\":[");
    for (uint32_t i = 0; i < s->set.block_count && i < VJO_SPOT_MAX; i++) {
        const VjoSpotBlock *b = &s->set.block[i];
        printf("%s[%u,%u,%u,%u,%u,%u,%u]", i ? "," : "", b->x, b->y, b->w, b->h, b->vertical, b->first, b->count);
    }
    printf("],\"map\":{\"count\":%u,\"start\":%d,\"vertical\":%u,\"rect\":[", s->map.count, s->map.start,
           s->map.vertical);
    for (int i = 0; i < s->map.count; i++)
        printf("%s[%d,%d,%d,%d]", i ? "," : "", s->map.rect[i][0], s->map.rect[i][1], s->map.rect[i][2],
               s->map.rect[i][3]);
    printf("],\"next\":[");
    for (int i = 0; i < s->map.count; i++)
        printf("%s[%u,%u,%u,%u]", i ? "," : "", s->map.next[i][0], s->map.next[i][1], s->map.next[i][2],
               s->map.next[i][3]);
    printf("]}}\n");
    return rc ? 1 : 0;
}

TEST_LIST = {
    {"API 30: a scan reads the whole frame once, 64 rows a frame, and gives the runtime's set and its map",
     a_scan_reads_the_whole_frame_once},
    {"API 30: the pinned arena is the runtime's for every frame up to 960x544", the_pinned_arena_is_the_runtime_s},
    {"API 30: a missing, changed or short weights file reads no frame", a_missing_or_wrong_file_reads_no_frame},
    {"API 30: the game's memory decides: the reserve, USER_RW, then CDRAM, else none", the_game_memory_decides},
    {"API 30: a cancellation or a resized frame mid-frame fails safe; the next scan works",
     a_failure_mid_frame_keeps_the_game_running},
    {"API 30: the kernel strip rung gives the same set", the_kernel_strip_rung_gives_the_same_set},
    {"API 30: the kernel and the worker take a scan only in its shape; the spotter's file opens only for one",
     the_kernel_and_the_worker_take_only_a_scan},
    {"API 30: a PSP game's 480x272 frame is read as it is and mapped to the screen", a_psp_frame_is_mapped_to_the_screen},
    {"API 30: scans leave the slot, the kernel and the game as they found them", scans_leave_the_slot_as_they_found_it},
    {"API 30: the core refuses a short or misaligned block, or a region past the screen, before any read",
     the_core_refuses_short_or_misaligned_blocks},
    {NULL, NULL}
};

/* The tests' own memory card (test_tmpdir.h). */
int main(int argc, char **argv)
{
    const char *dir;
    read_blob();
    dir = vjo_test_tmpdir("game-spot");
    if (!dir || chdir(dir))
        return 2;
    if (argc >= 2 && !strcmp(argv[1], "--spot"))
        return spot_step(argc, argv);
    return acutest_main(argc, argv);
}
