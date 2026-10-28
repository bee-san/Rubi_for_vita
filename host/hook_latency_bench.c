/* The trusted-hook latency bench (docs/native-hooks.md, "No wait for a trusted line"): how long after a trusted hook
 * writes a line Rubi has it, with its first word looked up, for the screen that shows it (an L + R then opens on it
 * at once), or (overlay open) shows it in place with that lookup.
 *
 * What runs: the actual game worker (game_ocr/main.c: its loop, the hook watch, the job path and the trusted
 * selection, core/hook_select.c), the actual kernel transport and change detection (kernel/game_ocr.c through
 * host/hook_latency_kernel.c, kernel/scene.c and kernel/capture.c's signature of a frame this bench draws: a dialogue
 * box that types each line, a "next" icon blinking once it is typed), and the actual Shell client
 * (shell/game_ocr_client.c's exchange). The Shell's control and job (VjoNet) threads are a model of shell/worker.c's
 * auto_prefetch, background_throttled, background_open, cancel_outdated_job, on_job_done, on_refresh_done,
 * claim_cache_for, shows_screen and vocr_note_capture, with each build's rules (API 49's hook_scene below
 * #if VJO_API_VERSION >= 49); host/tests/test_shell_worker.c checks the real ones. kernel/main.c's change_detection,
 * vjoGetState, vjoWaitEvent and kernel/display.c's frame step are copied here (they cannot link on the host).
 *
 * The clock: every thread is a cooperative one (ucontext) on one virtual clock, which moves only where the code
 * waits (an event flag's wait with its timeout, a delay, the game's next frame at 60 fps); code takes no virtual
 * time. The first word's lookup is a constant, 70 ms by default: the console's fit (docs/performance.md, "Lookups:
 * 70-81 ms each, nearly all file I/O"). Host execution: no console timing; the waits are the code's, the compute is
 * not modelled (the worker's trusted answer is microseconds on the host).
 *
 *   vjo-hook-latency-bench SCENARIO [--lookup-ms 70] [--cps 30] [--lines N] < lines.txt > lines.jsonl
 *     SCENARIO  reading   the learned source (a string copy, trusted), overlay closed, a reader's pace
 *               quick     the same, the player clicking on (a click completes the typing at 250 ms, the next at 500)
 *               subtitles the same as reading with the subtitle strip on (Select + R)
 *               open      the same as reading with the overlay open (auto-advance), the reader moving to a word
 *                         700 ms after each line is ready
 *               site      a Rubi verified site from the session's first line (the first line is reported apart)
 *               grow      the learned source giving each line as it types: four growing parts 60 ms apart
 *     lines.txt one hooked line a line (UTF-8), e.g. a title's pages from vita-vn-ocr's page pool
 *               (tools/hook_latency_bench.py)
 *
 * One JSON line per dialogue line: its characters, and in ms after its write (a growing line's first part): the
 * worker's report to the kernel (vjoOcrHookLine), the Shell's job start, its result (the text), the text shown (the
 * open overlay's, before its lookup; else when ready), and ready (text and lookup), also after its last part's write;
 * or skipped (a newer line came first); repeat: the page's text is the one the stream already holds (core/hook_stream.c
 * stores an identical copy as a hit, no new sequence: no hook event), left out of the figures. Then one summary line: the figures of every line but the first (the session's start: the worker's
 * first rounds and the empty box's job; first_line_ready_ms), the worker loop's rounds (vjoOcrTake calls) per second
 * of the session and the host time per round. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <psp2/host_stubs.h>
#include <psp2kern/host_stubs.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <taihen.h>
#include "../kernel/vjo_kernel.h"
#include "../kernel/game_ocr.h"
#include "../kernel/scene.h"
#include "../include/vjo_vocr_sizes.h"
#include "../shell/game_ocr_client.h"
#define VJO_TEST_HOOK_CACHE 1
#define VJO_TEST_HOOK_LINE 1
#include "../game_ocr/main.c"

VjoKernelState g;
unsigned syscall_depth;

/* ---------------- cooperative threads on one virtual clock ---------------- */
enum { LB_WORKER, LB_KERNEL, LB_GAME, LB_CTL, LB_NET, LB_THREADS };
#define LB_STACK (1024u * 1024u)
typedef struct {
    ucontext_t ctx;
    char *stack;
    int alive, pid, tid;
    uint64_t wake; /* runnable at; UINT64_MAX: waiting without a timeout */
    int waiting, woken;
    SceUID wuid;
    unsigned wpat, wmode, wgot;
    uint64_t host_ns;
    void (*fn)(void);
} LbThread;
static LbThread th[LB_THREADS];
static int cur = -1;
static ucontext_t lb_sched;
static uint64_t vnow; /* µs */
static unsigned flag_bits[32];

static uint64_t host_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static void lb_block(void) { swapcontext(&th[cur].ctx, &lb_sched); }
static void lb_sleep(uint64_t us)
{
    th[cur].wake = vnow + us;
    lb_block();
}
static void lb_entry(void)
{
    th[cur].fn();
    th[cur].alive = 0;
}
static void lb_spawn(int i, void (*fn)(void), int pid, int tid)
{
    LbThread *t = &th[i];
    t->stack = malloc(LB_STACK);
    getcontext(&t->ctx);
    t->ctx.uc_stack.ss_sp = t->stack;
    t->ctx.uc_stack.ss_size = LB_STACK;
    t->ctx.uc_link = &lb_sched;
    makecontext(&t->ctx, lb_entry, 0);
    t->fn = fn;
    t->alive = 1;
    t->pid = pid;
    t->tid = tid;
    t->wake = 0;
}
static void lb_run(uint64_t until)
{
    for (;;) {
        int best = -1;
        for (int i = 0; i < LB_THREADS; i++)
            if (th[i].alive && (best < 0 || th[i].wake < th[best].wake)) best = i;
        if (best < 0 || th[best].wake == UINT64_MAX) {
            fprintf(stderr, "hook-latency-bench: every thread waits for ever at %llu us\n", (unsigned long long)vnow);
            exit(2);
        }
        if (th[best].wake > until) return;
        if (th[best].wake > vnow) vnow = th[best].wake;
        cur = best;
        uint64_t t0 = host_ns();
        swapcontext(&lb_sched, &th[best].ctx);
        th[best].host_ns += host_ns() - t0;
        cur = -1;
    }
}

/* Event flags with the SDK's WAITOR and WAITCLEAR_PAT (the only modes the code uses) */
static int flag_take(SceUID uid, unsigned pat, unsigned mode, unsigned *out)
{
    unsigned b = flag_bits[uid & 31];
    if (!(b & pat)) return 0;
    if (out) *out = b;
    if (mode & SCE_EVENT_WAITCLEAR_PAT) flag_bits[uid & 31] &= ~pat;
    return 1;
}
static int lb_wait_flag(SceUID uid, unsigned pat, unsigned mode, unsigned *out, SceUInt *timeout)
{
    LbThread *t = &th[cur];
    if (flag_take(uid, pat, mode, out)) return 0;
    if (timeout && !*timeout) return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
    t->waiting = 1; t->woken = 0; t->wuid = uid; t->wpat = pat; t->wmode = mode;
    t->wake = timeout ? vnow + *timeout : UINT64_MAX;
    lb_block();
    t->waiting = 0;
    if (t->woken) {
        if (out) *out = t->wgot;
        return 0;
    }
    if (timeout) *timeout = 0;
    return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
}
static void lb_set_flag(SceUID uid, unsigned bits)
{
    flag_bits[uid & 31] |= bits;
    for (int i = 0; i < LB_THREADS; i++) {
        LbThread *t = &th[i];
        if (!t->alive || !t->waiting || t->woken || t->wuid != uid || !(flag_bits[uid & 31] & t->wpat)) continue;
        t->woken = 1;
        t->wgot = flag_bits[uid & 31];
        if (t->wmode & SCE_EVENT_WAITCLEAR_PAT) flag_bits[uid & 31] &= ~t->wpat;
        t->wake = vnow;
    }
}

/* ---------------- the kernel's platform ---------------- */
static int retail_return(int rc) { return rc < 0 ? (int32_t)((uint32_t)rc & ~0x40000000u) : rc; }
void klog(const char *format, ...) { (void)format; }
SceUID ksceKernelGetProcessId(void) { return cur >= 0 ? th[cur].pid : 0; }
int ksceKernelGetThreadId(void) { return cur >= 0 ? th[cur].tid : 0; }
uint64_t ksceKernelGetSystemTimeWide(void) { return vnow; }
int ksceKernelSetEventFlag(SceUID uid, unsigned bits) { lb_set_flag(uid, bits); return 0; }
int ksceKernelClearEventFlag(SceUID uid, unsigned bits) { flag_bits[uid & 31] &= bits; return 0; }
int ksceKernelWaitEventFlag(SceUID uid, unsigned bits, unsigned wait, unsigned *out, SceUInt *timeout)
{
    return lb_wait_flag(uid, bits, wait, out, timeout);
}
int ksceKernelDelayThread(SceUInt us) { lb_sleep(us); return 0; }
int ksceKernelLockMutex(SceUID uid, int n, void *timeout) { (void)uid; (void)n; (void)timeout; return 0; }
int ksceKernelUnlockMutex(SceUID uid, int n) { (void)uid; (void)n; return 0; }
SceUID ksceKernelFindMemBlockByAddr(const void *addr, SceSize size) { (void)addr; (void)size; return 40; }
int taiGetModuleInfoForKernel(SceUID pid, const char *name, tai_module_info_t *info)
{ (void)pid; (void)name; info->modid = 99; return 0; }
int ksceKernelMemcpyUserToKernel(void *dst, const void *src, SceSize n) { if (!dst || !src) return -1; memcpy(dst, src, n); return 0; }
int ksceKernelMemcpyKernelToUser(void *dst, const void *src, SceSize n) { if (!dst || !src) return -1; memcpy(dst, src, n); return 0; }
static unsigned char kstrip[VJO_GAME_OCR_KSTRIP_MAX];
SceUID ksceKernelAllocMemBlock(const char *name, unsigned type, SceSize bytes, void *opt)
{ (void)name; (void)type; (void)opt; return bytes <= sizeof(kstrip) ? 28 : -1; }
int ksceKernelGetMemBlockBase(SceUID uid, void **base) { (void)uid; *base = kstrip; return 0; }
int ksceKernelFreeMemBlock(SceUID uid) { (void)uid; return 0; }
/* No file: a model or cache operation fails (no job of these scenarios needs one) */
int ksceIoGetstatByFd(SceUID fd, SceIoStat *st) { (void)fd; memset(st, 0, sizeof(*st)); return -1; }
SceUID ksceIoOpen(const char *path, int flags, unsigned mode) { (void)path; (void)flags; (void)mode; return (int)0x80010002u; }
SceOff ksceIoLseek(SceUID fd, SceOff off, int whence) { (void)fd; (void)off; (void)whence; return -1; }
int ksceIoPread(SceUID fd, void *out, SceSize n, SceOff off) { (void)fd; (void)out; (void)n; (void)off; return -1; }
int ksceIoPwrite(SceUID fd, const void *data, SceSize n, SceOff off) { (void)fd; (void)data; (void)n; (void)off; return -1; }
int ksceIoChstatByFd(SceUID fd, SceIoStat *st, int bits) { (void)fd; (void)st; (void)bits; return -1; }
int ksceIoClose(SceUID fd) { (void)fd; return 0; }
int ksceIoRename(const char *from, const char *to) { (void)from; (void)to; return -1; }
int ksceIoRemove(const char *path) { (void)path; return -1; }
int ksceIoSync(const char *device, unsigned int unk) { (void)device; (void)unk; return -1; }
int ksceIoSyncByFd(SceUID fd, int flag) { (void)fd; (void)flag; return -1; }

/* ---------------- the game worker's platform ---------------- */
#define LB_BLOCKS 16
static struct { void *base; unsigned bytes; int live; } blk[LB_BLOCKS];
static unsigned live_bytes;
SceUID ksceKernelFindProcMemBlockByAddr(SceUID pid, const void *addr, SceSize size)
{
    (void)size;
    for (int i = 0; pid == 7 && i < LB_BLOCKS; i++)
        if (blk[i].live && (const char *)addr >= (char *)blk[i].base && (const char *)addr < (char *)blk[i].base + blk[i].bytes)
            return 100 + i;
    return -1;
}
SceUID sceKernelGetProcessId(void) { return 7; }
uint64_t sceKernelGetProcessTimeWide(void) { return vnow; }
int sceKernelDelayThread(unsigned us) { lb_sleep(us); return 0; }
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *out)
{
    out->size_user = (int)(64u * 1024u * 1024u - live_bytes);
    out->size_cdram = 8 * 1024 * 1024;
    out->size_phycont = 8 * 1024 * 1024;
    return 0;
}
SceUID sceKernelAllocMemBlock(const char *name, int type, unsigned n, void *opt)
{
    (void)name; (void)type; (void)opt;
    for (int i = 0; i < LB_BLOCKS; i++)
        if (!blk[i].live) {
            blk[i].base = aligned_alloc(4096, (n + 4095u) & ~4095u);
            blk[i].bytes = n;
            blk[i].live = 1;
            live_bytes += n;
            return 100 + i;
        }
    return (int)0x80020001u;
}
int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    if (uid < 100 || uid >= 100 + LB_BLOCKS || !blk[uid - 100].live) return -1;
    *base = blk[uid - 100].base;
    return 0;
}
int sceKernelFreeMemBlock(SceUID uid)
{
    if (uid < 100 || uid >= 100 + LB_BLOCKS || !blk[uid - 100].live) return -1;
    free(blk[uid - 100].base);
    blk[uid - 100].live = 0;
    live_bytes -= blk[uid - 100].bytes;
    return 0;
}
SceUID sceKernelCreateThread(const char *name, int (*entry)(SceSize, void *), int p, unsigned stack, unsigned a, int cpu, void *opt)
{ (void)name; (void)entry; (void)p; (void)stack; (void)a; (void)cpu; (void)opt; return -1; } /* no helpers: OCR runs none here */
int sceKernelStartThread(SceUID uid, SceSize n, void *args) { (void)uid; (void)n; (void)args; return -1; }
int sceKernelWaitThreadEnd(SceUID uid, int *status, void *timeout) { (void)uid; (void)status; (void)timeout; return 0; }
int sceKernelDeleteThread(SceUID uid) { (void)uid; return 0; }
int sceKernelGetThreadInfo(SceUID thid, SceKernelThreadInfo *info) { (void)thid; (void)info; return -1; }
SceUID sceKernelGetThreadId(void) { return 21; }
int scePowerGetArmClockFrequency(void) { return 333; }
int scePowerSetArmClockFrequency(int mhz) { (void)mhz; return 0; }
int sceKernelGetProcessTitleId(SceUID pid, char *out, SceSize size) { (void)pid; snprintf(out, size, "PCSG00415"); return 0; }
int sceAppMgrAppParamGetString(int pid, int param, char *out, SceSize size) { (void)pid; (void)param; (void)out; (void)size; return -1; }
int sceDisplayGetFrameBuf(SceDisplayFrameBuf *fb, int sync) { (void)fb; (void)sync; return -1; }
int sceKernelGetMemBlockInfoByAddr(void *base, SceKernelMemBlockInfo *info) { (void)base; (void)info; return -1; }
int vjoGetVersion(void) { return VJO_API_VERSION; }
int vjoBootInfo(VjoBootInfo *out) { out->mode = VJO_BOOT_NORMAL; return 0; }
SceUID sceIoOpen(const char *path, int flags, unsigned mode) { (void)path; (void)flags; (void)mode; return (int)0x80010013u; }

/* ---------------- the hook streams (game_ocr/hooks.h), as the game writes them ---------------- */
typedef struct { uint32_t kind, seq, key; const char *text; int line, part; } LbStream;
static LbStream fs[VJO_HOOK_STREAMS + 1u];
static uint32_t hook_clock, verified_mask;
static int in_job, job_snap_line = -1, job_snap_part = -1;
void vjo_hooks_counts(uint32_t *mode, uint32_t *installed, uint32_t *refused)
{
    *mode = VJO_TEXT_SOURCE_AUTO;
    *installed = 1u << 16 | 4u;
    *refused = 0;
}
int vjo_hooks_verified(uint32_t id) { return id >= 1u && id <= 32u && (verified_mask >> (id - 1u) & 1u); }
void vjo_hooks_keep(uint32_t id, uint32_t key) { (void)id; (void)key; }
int vjo_hooks_psp_poll(void) { return 0; } /* #318's PSP text poll: no PSP emulator here (psp_title 0) */
void vjo_hooks_call_stats(uint32_t *dropped, uint32_t *replaced) { *dropped = *replaced = 0; }
int vjo_hooks_build_id(uint8_t out[32]) { (void)out; return 0; }
int vjo_hooks_snap(uint32_t id, VjoHookSnap *out)
{
    if (!id || id > VJO_HOOK_STREAMS || !fs[id].text || !fs[id].text[0]) return 0;
    memset(out, 0, offsetof(VjoHookSnap, raw));
    out->id = id;
    out->kind = fs[id].kind;
    out->seq = fs[id].seq;
    out->key = fs[id].key;
    out->hits = 1;
    out->encoding = VJO_HOOK_ENC_UTF8;
    out->n = (uint32_t)strlen(fs[id].text) < VJO_HOOK_RAW_BYTES ? (uint32_t)strlen(fs[id].text) : VJO_HOOK_RAW_BYTES;
    memcpy(out->raw, &fs[id].text, sizeof(fs[id].text)); /* the bench's raw bytes: the text's address */
    if (in_job && job_snap_line < 0) { job_snap_line = fs[id].line; job_snap_part = fs[id].part; }
    return 1;
}
uint32_t vjo_hooks_seq(uint32_t id, uint32_t *key, uint32_t *kind, uint32_t *bytes)
{
    if (!id || id > VJO_HOOK_STREAMS || !fs[id].text) return 0;
    *key = fs[id].key;
    *kind = fs[id].kind;
    *bytes = (uint32_t)strlen(fs[id].text);
    return fs[id].seq;
}
size_t vjo_hooks_text(const VjoHookSnap *s, char *out, size_t cap)
{
    const char *t;
    size_t n;
    memcpy(&t, s->raw, sizeof(t));
    n = strlen(t);
    if (n >= cap) {
        n = cap - 1u;
        while (n && ((unsigned char)t[n] & 0xC0) == 0x80) n--;
    }
    memcpy(out, t, n);
    out[n] = '\0';
    return n;
}

/* ---------------- the session's lines ---------------- */
#define LB_PARTS 4
typedef struct {
    char *text;
    char *part[LB_PARTS]; /* grow: the growing parts, the last the whole line */
    unsigned chars, parts;
    uint64_t arr, typed_at, next, last; /* written (its first part); typing complete; the next line's; its last part */
    uint64_t rep, start, result, ready, first_text;
    int skipped, repeat; /* repeat: the text the stream already holds (no new sequence: no hook event) */
} LbLine;
static LbLine *ln;
static unsigned nl;
static int latest = -1;           /* the newest line written */
static unsigned scenario, lookup_us_cost = 70000, cps = 30;
enum { SC_READING, SC_QUICK, SC_SUBTITLES, SC_OPEN, SC_SITE, SC_GROW };
static uint32_t src_id;           /* the dialogue's stream */
static unsigned takes, reports, shell_jobs;
static uint64_t session_end;
static int trace; /* LB_TRACE=1: every event on stderr */
#define TRACE(...) do { if (trace) { fprintf(stderr, "%10.3f ", vnow / 1000.0); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static unsigned utf8_chars(const char *s)
{
    unsigned n = 0;
    for (; *s; s++) n += ((unsigned char)*s & 0xC0) != 0x80;
    return n;
}
static char *utf8_prefix(const char *s, unsigned chars)
{
    unsigned n = 0;
    const char *p = s;
    for (; *p; p++) {
        if (((unsigned char)*p & 0xC0) != 0x80) {
            if (n == chars) break;
            n++;
        }
    }
    return strndup(s, (size_t)(p - s));
}

/* ---------------- the worker's syscalls: the real kernel's, timed ---------------- */
int lat_kernel_register(void);
int lat_kernel_submit(const VjoGameOcrRequest *request);
int lat_kernel_take(VjoGameOcrRequest *out);
int lat_kernel_capture_rows(const VjoGameOcrRows *request, void *dst, VjoGameOcrRowsInfo *info);
int lat_kernel_cancelled(uint32_t seq);
int lat_kernel_complete(const VjoGameOcrResult *result);
int lat_kernel_read(uint32_t seq, VjoGameOcrResult *out);
int lat_kernel_cancel(uint32_t seq);
int lat_kernel_model_open(uint32_t seq, uint32_t which, VjoGameOcrModelInfo *info);
int lat_kernel_model_read(uint32_t seq, uint32_t off, uint32_t bytes, void *dst);
int lat_kernel_model_close(uint32_t seq);
int lat_kernel_shot_write(uint32_t seq, uint32_t off, uint32_t bytes, const void *src);
int lat_kernel_shot_save(uint32_t seq, uint32_t bytes);
int lat_kernel_hook_cache(uint32_t seq, uint32_t operation, void *data);
int lat_kernel_hook_line(uint32_t seq, uint32_t bytes);
static int completed_line = -1, completed_part = -1;
int vjoOcrRegister(void) { return retail_return(lat_kernel_register()); }
int vjoOcrSubmit(const VjoGameOcrRequest *r) { return retail_return(lat_kernel_submit(r)); }
int vjoOcrTake(VjoGameOcrRequest *out)
{
    int rc;
    takes++;
    rc = retail_return(lat_kernel_take(out));
    if (!rc) { in_job = 1; job_snap_line = job_snap_part = -1; }
    TRACE("take rc=%d", rc);
    return rc;
}
int vjoOcrCaptureRows(const VjoGameOcrRows *r, void *dst, VjoGameOcrRowsInfo *info)
{ return retail_return(lat_kernel_capture_rows(r, dst, info)); }
int vjoOcrCancelled(uint32_t seq) { return retail_return(lat_kernel_cancelled(seq)); }
int vjoOcrComplete(const VjoGameOcrResult *r)
{
    int rc = retail_return(lat_kernel_complete(r));
    if (!rc && in_job) { completed_line = job_snap_line; completed_part = job_snap_part; in_job = 0; }
    return rc;
}
int vjoOcrRead(uint32_t seq, VjoGameOcrResult *out) { return retail_return(lat_kernel_read(seq, out)); }
int vjoOcrCancel(uint32_t seq) { return retail_return(lat_kernel_cancel(seq)); }
int vjoOcrModelOpen(uint32_t seq, uint32_t which, VjoGameOcrModelInfo *info)
{ return retail_return(lat_kernel_model_open(seq, which, info)); }
int vjoOcrModelRead(uint32_t seq, uint32_t off, uint32_t bytes, void *dst)
{ return retail_return(lat_kernel_model_read(seq, off, bytes, dst)); }
int vjoOcrModelClose(uint32_t seq) { return retail_return(lat_kernel_model_close(seq)); }
int vjoOcrShotWrite(uint32_t seq, uint32_t off, uint32_t bytes, const void *src)
{ return retail_return(lat_kernel_shot_write(seq, off, bytes, src)); }
int vjoOcrShotSave(uint32_t seq, uint32_t bytes) { return retail_return(lat_kernel_shot_save(seq, bytes)); }
int vjoOcrHookCache(uint32_t seq, uint32_t operation, void *data)
{ return retail_return(lat_kernel_hook_cache(seq, operation, data)); }
int vjoOcrHookLine(uint32_t seq, uint32_t bytes)
{
    int rc = retail_return(lat_kernel_hook_line(seq, bytes));
    TRACE("hook line seq=%u bytes=%08x rc=%d", seq, bytes, rc);
    if (rc >= 0) {
        reports++;
        if (latest >= 0 && !ln[latest].rep) ln[latest].rep = vnow;
    }
    return rc;
}

/* ---------------- kernel/main.c and kernel/display.c, as the kernel runs them ---------------- */
/* kernel/main.c change_detection: the newest signature into the scene tracker, its events to the Shell */
static void lb_change_detection(void)
{
    static SceneSig sig;
    uint32_t seq = g.hook_sig_seq;
    SceneEvent ev = SCENE_NONE;
    if (seq != g.seen_sig_seq) {
        sig = g.hook_sig[seq & 1];
        g.seen_sig_seq = seq;
        int64_t now = (int64_t)vnow;
        int64_t input_us = now - (uint32_t)((uint32_t)now - g.input_us32);
        int64_t edge_us = now - (uint32_t)((uint32_t)now - g.input_edge_us32);
        ev = scene_update(&g.tracker, &sig, now, input_us, edge_us);
    }
    if (ev) TRACE("scene event %d scene %u", (int)ev, g.tracker.id);
    if (ev == SCENE_QUIET) lb_set_flag(g.evf, VJO_EV_REGION_QUIET);
    else if (ev == SCENE_SETTLED) lb_set_flag(g.evf, VJO_EV_REGION_STABLE);
}
/* kernel/main.c vjoGetState */
static void lb_state(VjoState *s)
{
    memset(s, 0, sizeof(*s));
    s->size = sizeof(*s);
    s->game_pid = g.game_pid;
    s->fb_width = g.fb_w; s->fb_height = g.fb_h; s->fb_pitch = g.fb_pitch; s->fb_pixelformat = g.fb_fmt;
    s->scene = g.tracker.id;
    s->stable = g.tracker.stable;
    s->unsettled_ms = (uint32_t)(scene_unsettled_us(&g.tracker, (int64_t)vnow) / 1000);
    s->quiet = (g.tracker.quiet || scene_hook_drawing(&g.tracker)) && !g.tracker.stable;
    if (g.region_seq) {
        s->region_seq = g.region_seq;
        s->region_scene = scene_match_since(&g.tracker, &g.capture_sig, g.region_us);
    }
#if VJO_API_VERSION >= 49
    if (g.hook_scene == g.tracker.id && g.tracker.hook_us) s->hook_scene = g.hook_scene;
#endif
    s->width = g.crop_w; s->height = g.crop_h;
    s->capture_scene = g.capture_scene;
    s->done_seq = g.done_seq;
}
static void kernel_main(void)
{
    for (unsigned tick = 1;; tick++) { /* kernel/main.c's worker: POLL_US 16 ms, change detection every 4th */
        lb_sleep(16000);
        game_ocr_rows_tick();
        game_ocr_model_tick();
        if (tick % 4 == 0) lb_change_detection();
    }
}

/* ---------------- the game: its frames, the dialogue box, the hook's writes ---------------- */
#define FB_W 960u
#define FB_H 544u
#define BOX_X 32u
#define BOX_Y 380u
#define BOX_W 896u
#define BOX_H 152u
#define CHAR_PX 20u
#define ROW_CHARS 38u
static uint32_t fb[FB_W * FB_H];
static unsigned drawn; /* characters of the current line drawn */
static int icon_on;
static void fill(unsigned x, unsigned y, unsigned w, unsigned h, uint32_t c)
{
    for (unsigned j = y; j < y + h && j < FB_H; j++)
        for (unsigned i = x; i < x + w && i < FB_W; i++) fb[j * FB_W + i] = c;
}
static void box_clear(void)
{
    fill(BOX_X, BOX_Y, BOX_W, BOX_H, 0xFF302820u);
    drawn = 0;
    icon_on = 0;
}
static void draw_chars(unsigned upto, unsigned line)
{
    for (; drawn < upto && drawn < 3u * ROW_CHARS; drawn++) {
        unsigned r = drawn / ROW_CHARS, c = drawn % ROW_CHARS;
        uint32_t shade = 0xFF000000u | (0xD0u + (line * 7u + drawn * 13u) % 0x2Fu) * 0x010101u;
        fill(BOX_X + 12u + c * (CHAR_PX + 3u), BOX_Y + 14u + r * (CHAR_PX + 22u), CHAR_PX, CHAR_PX, shade);
    }
}
static void hook_write(uint32_t id, const char *text, int line, int part)
{
    if (fs[id].text && !strcmp(fs[id].text, text)) { /* core/hook_stream.c store: an identical copy is a hit only */
        if (!part) ln[line].repeat = 1;
        fs[id].line = line;
        return;
    }
    fs[id].text = text;
    fs[id].seq = ++hook_clock;
    fs[id].line = line;
    fs[id].part = part;
}
static void game_main(void)
{
    unsigned frame = 0, cur_line = 0, next_part = 0;
    int64_t complete_at = -1;
    for (unsigned y = 0; y < FB_H; y++) /* a still background */
        for (unsigned x = 0; x < FB_W; x++)
            fb[y * FB_W + x] = 0xFF000000u | ((60u + y / 8u) << 16) | ((90u + x / 12u) << 8) | (120u + (x ^ y) % 32u);
    box_clear();
    for (;; frame++) {
        uint64_t t = (uint64_t)frame * 1000000ull / 60ull;
        if (t > vnow) lb_sleep(t - vnow);
        /* the game's logic at this frame: the next line (a click, then the hook's copy), a growing line's parts */
        while (cur_line < nl && ln[cur_line].arr <= t) {
            LbLine *l = &ln[cur_line];
            l->arr = t; /* the frame it is written on: every time below is measured from it */
            g.input_us32 = g.input_edge_us32 = (uint32_t)t;
            box_clear();
            latest = (int)cur_line;
            if (cur_line && !ln[cur_line - 1].ready && !ln[cur_line - 1].skipped) ln[cur_line - 1].skipped = 1;
            hook_write(src_id, l->parts > 1 ? l->part[0] : l->text, (int)cur_line, 0);
            l->last = t;
            TRACE("line %u written", cur_line);
            next_part = l->parts > 1 ? 1 : 0;
            complete_at = scenario == SC_QUICK ? (int64_t)(l->arr + 250000) : -1;
            cur_line++;
        }
        if (latest >= 0) {
            LbLine *l = &ln[latest];
            unsigned typed = (unsigned)((t - l->arr) * cps / 1000000ull);
            while (next_part && next_part < l->parts && t >= l->arr + next_part * 60000ull) {
                hook_write(src_id, l->part[next_part], latest, (int)next_part);
                l->last = t;
                next_part++;
            }
            if (complete_at >= 0 && (int64_t)t >= complete_at) { /* the click that completes the typing */
                typed = l->chars;
                if (!l->typed_at) { g.input_us32 = g.input_edge_us32 = (uint32_t)t; }
            }
            if (typed > l->chars) typed = l->chars;
            draw_chars(typed, (unsigned)latest);
            if (typed == l->chars && !l->typed_at) l->typed_at = t;
            if (l->typed_at && (t - l->typed_at) / 500000ull % 2u != (unsigned)icon_on) { /* the "next" icon */
                icon_on ^= 1;
                fill(BOX_X + BOX_W - 28u, BOX_Y + BOX_H - 24u, 14u, 14u, icon_on ? 0xFFF0F0F0u : 0xFF302820u);
            }
        }
        /* kernel/display.c on_frame: the signature every VJO_CHECK_EVERY_FRAMES, an armed job's rows */
        g.fb_pitch = FB_W; g.fb_fmt = VJO_FB_A8B8G8R8; g.fb_w = FB_W; g.fb_h = FB_H;
        if (g.game_active && (frame + 1u) % VJO_CHECK_EVERY_FRAMES == 0) {
            uint32_t seq = g.hook_sig_seq;
            if (region_sig_hook((uintptr_t)fb, FB_W, VJO_FB_A8B8G8R8, FB_W, FB_H, &g.hook_sig[(seq + 1) & 1]) == 0)
                g.hook_sig_seq = seq + 1;
        }
        game_ocr_rows_hook(7, (uintptr_t)fb, FB_W, VJO_FB_A8B8G8R8, FB_W, FB_H);
    }
}

/* ---------------- the Shell (a model of shell/worker.c's control and job (VjoNet) threads) ---------------- */
#define POLL_TIMEOUT_US 50000ull
#define SUBTITLE_GAP_US 1000000ull
#define REFRESH_GAP_US 2000000ull
#define REFRESH_IDLE_US 1500000ull
#define NET_EVF 9
enum { JOB_NONE, JOB_BG, JOB_REFRESH, JOB_CURSOR };
static int ov_open, subtitles, job_running, job_kind, job_lookup, job_done, job_cancelled, job_rc, job_line, job_part;
static int stable_pending, refresh_pending, cache_ok, cache_line = -1, local_valid, local_line = -1;
static uint32_t cache_scene, cache_seq, local_scene, local_seq;
static volatile int job_captured;
static uint32_t job_capture_scene, job_capture_seq, vocr_done_before;
static uint64_t job_started_us, lookup_at, refresh_after_us, user_lookup_at;

static int same_scene(uint32_t a, uint32_t b) { return a != 0 && a == b; }
static int shows_screen(uint32_t scene, uint32_t seq, const VjoState *st)
{
    return same_scene(scene, st->scene) || (seq && seq == st->region_seq && same_scene(st->region_scene, st->scene));
}
static void vocr_note_capture(void)
{
    VjoState st;
    lb_state(&st);
    if (st.done_seq == vocr_done_before) return;
    if (!st.region_seq) { job_captured = 0; return; }
    job_capture_scene = st.capture_scene;
    job_capture_seq = st.region_seq;
    job_captured = 1;
}
static int vocr_poll_cancelled(void *ud) { (void)ud; vocr_note_capture(); return job_cancelled; }
static int c_submit(void *ud, const VjoGameOcrRequest *r) { (void)ud; return vjoOcrSubmit(r); }
static int c_read(void *ud, uint32_t seq, VjoGameOcrResult *r) { (void)ud; return vjoOcrRead(seq, r); }
static int c_cancel(void *ud, uint32_t seq) { (void)ud; return vjoOcrCancel(seq); }
static int64_t c_now(void *ud) { (void)ud; return (int64_t)vnow; }
static void c_delay(void *ud, uint32_t us) { (void)ud; lb_sleep(us); }
static const VjoGameOcrClient lb_client = {NULL, c_submit, c_read, c_cancel, c_now, c_delay};

static void start_job(int kind, int lookup)
{
    job_running = 1; job_kind = kind; job_lookup = lookup; job_done = 0; job_cancelled = 0;
    job_started_us = vnow;
    if (kind == JOB_CURSOR) lookup_at = vnow; /* start_cursor_job: the refreshes' REFRESH_IDLE_US */
    if (kind != JOB_CURSOR) { job_captured = 0; job_capture_scene = job_capture_seq = 0; }
    else { job_captured = 1; job_capture_scene = local_scene; job_capture_seq = local_seq; }
    if (kind != JOB_CURSOR && latest >= 0 && !ln[latest].start) ln[latest].start = vnow;
    if (kind != JOB_CURSOR) shell_jobs++;
    TRACE("shell job kind=%d", kind);
    lb_set_flag(NET_EVF, 1);
}
static void net_main(void)
{
    static VjoGameOcrResult res;
    for (;;) {
        unsigned bits;
        lb_wait_flag(NET_EVF, 1, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &bits, NULL);
        if (job_kind != JOB_CURSOR) { /* vocr_run_one: the request, the exchange, the capture */
            VjoGameOcrRequest rq;
            VjoState st0;
            memset(&rq, 0, sizeof(rq));
            rq.size = sizeof(rq);
            strcpy(rq.model, "FL10_w8.vocr");
            strcpy(rq.model_dir, VJO_VOCR_MODEL_ROOT);
            rq.speed = VJO_GAME_OCR_S_TRUST; /* config_speed()'s hook_trust = on (the default) */
            rq.hook_pick = VJO_HOOK_PICK_AUTO;
            lb_state(&st0);
            vocr_done_before = st0.done_seq;
            job_rc = vjo_game_ocr_exchange(&lb_client, &rq, &res, vocr_poll_cancelled, NULL, 60000000);
            vocr_note_capture();
            job_line = completed_line; job_part = completed_part;
            if (!job_rc && job_line >= 0 && !job_cancelled) {
                LbLine *l = &ln[job_line];
                if (!l->result) l->result = vnow;
            }
        }
        if (!job_rc && !job_cancelled && (job_lookup || job_kind == JOB_CURSOR)) lb_sleep(lookup_us_cost);
        job_done = 1;
        lb_set_flag(g.evf, VJO_EV_SHELL); /* vjoNotifyShell */
    }
}
static int background_throttled(void)
{
    return subtitles && vnow < job_started_us + SUBTITLE_GAP_US;
}
static void ready(int line, int part)
{
    if (line < 0 || line != latest) return;
    LbLine *l = &ln[line];
    if (!l->first_text) l->first_text = vnow;
    if (!l->ready && (part < 0 || (unsigned)part + 1u >= l->parts)) l->ready = vnow;
}
static int local_part_shown = -1;
static void on_job_done(void)
{
    VjoState st;
    int kind = job_kind;
    job_running = job_done = 0;
    if (kind == JOB_CURSOR) { /* the lookup in the text shown */
        if (!job_cancelled && local_valid) ready(local_line, local_part_shown);
        job_cancelled = 0;
        return;
    }
    if (kind == JOB_REFRESH) { /* on_refresh_done */
        refresh_after_us = vnow + REFRESH_GAP_US;
        if (job_cancelled || job_rc) { refresh_pending = job_cancelled; job_cancelled = 0; return; }
        local_scene = job_capture_scene;
        local_seq = job_capture_seq;
        if (job_line == local_line && job_part == local_part_shown) return; /* the same text: this screen's now */
        local_line = job_line; /* a new text, or the line grew: shown at once, a word of it looked up */
        local_part_shown = job_part;
        if (job_line >= 0 && job_line == latest && !ln[job_line].first_text) ln[job_line].first_text = vnow;
        start_job(JOB_CURSOR, 1);
        return;
    }
    if (job_cancelled || job_rc) { /* nothing shown; the next screen's job */
        job_cancelled = 0;
        stable_pending = 1;
        return;
    }
    cache_ok = job_lookup;
    cache_scene = job_capture_scene;
    cache_seq = job_capture_seq;
    cache_line = job_line;
    lb_state(&st);
    if (cache_ok && shows_screen(cache_scene, cache_seq, &st)) ready(job_line, job_part);
    if (scenario == SC_OPEN && !ov_open && job_line == 0 && ln[0].ready) { /* the reader opens the overlay on it */
        ov_open = 1;
        local_valid = 1; local_line = 0; local_part_shown = job_part;
        local_scene = cache_scene; local_seq = cache_seq;
    }
}
static void cancel_outdated_job(const VjoState *st)
{
    if (!job_running || job_kind == JOB_CURSOR || job_cancelled || !job_captured || !(st->stable || st->quiet) ||
        shows_screen(job_capture_scene, job_capture_seq, st))
        return;
    job_cancelled = 1;
}
static void auto_prefetch(void)
{
    VjoState st;
    if (ov_open || job_running || !stable_pending) return;
    lb_state(&st);
#if VJO_API_VERSION >= 49
    if (background_throttled() && !st.hook_scene) return;
#else
    if (background_throttled()) return;
#endif
    stable_pending = 0;
    if ((st.stable || st.quiet) && !(cache_ok && shows_screen(cache_scene, cache_seq, &st))) start_job(JOB_BG, 1);
}
static void background_open(void)
{
    VjoState st;
    if (!ov_open || job_running || !refresh_pending) return;
    lb_state(&st);
#if VJO_API_VERSION >= 49
    if (!st.hook_scene && (vnow < refresh_after_us || vnow - lookup_at < REFRESH_IDLE_US)) return;
#else
    if (vnow < refresh_after_us || vnow - lookup_at < REFRESH_IDLE_US) return;
#endif
    refresh_pending = 0;
    if (local_valid && (st.stable || st.quiet) && !shows_screen(local_scene, local_seq, &st)) start_job(JOB_REFRESH, 0);
}
static void ctl_main(void)
{
    for (;;) {
        unsigned bits = 0;
        SceUInt to = POLL_TIMEOUT_US;
        lb_wait_flag(g.evf, VJO_EV_REGION_STABLE | VJO_EV_REGION_QUIET | VJO_EV_SHELL,
                     SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &bits, &to);
        if (job_running && job_done) on_job_done();
        if (bits & (VJO_EV_REGION_STABLE | VJO_EV_REGION_QUIET)) {
            VjoState st;
            stable_pending = 1;
            if (ov_open) refresh_pending = 1;
            if (job_running) { lb_state(&st); cancel_outdated_job(&st); }
        }
        auto_prefetch();
        background_open();
        /* the open overlay's reader: a word looked up 700 ms after each line is ready */
        if (ov_open && latest >= 0 && ln[latest].ready && !job_running && user_lookup_at < ln[latest].ready &&
            vnow >= ln[latest].ready + 700000ull) {
            user_lookup_at = vnow;
            start_job(JOB_CURSOR, 1);
        }
    }
}

/* ---------------- the session ---------------- */
static double ms(uint64_t at, uint64_t from) { return at ? (double)(at - from) / 1000.0 : -1.0; }
static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}
static void stats(const char *name, double *v, unsigned n, int comma)
{
    double sum = 0;
    if (!n) { printf("\"%s\": null%s", name, comma ? ", " : ""); return; }
    qsort(v, n, sizeof(*v), cmp_double);
    for (unsigned i = 0; i < n; i++) sum += v[i];
    printf("\"%s\": {\"n\": %u, \"mean\": %.1f, \"p50\": %.1f, \"p95\": %.1f, \"max\": %.1f}%s", name, n,
           sum / n, v[n / 2], v[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1], v[n - 1], comma ? ", " : "");
}
static void worker_thread(void) { worker_main(0, NULL); }
int main(int argc, char **argv)
{
    static const char *const names[] = {"reading", "quick", "subtitles", "open", "site", "grow"};
    char buf[8192];
    unsigned limit = 0, cap = 0;
    int sc = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--lookup-ms") && i + 1 < argc) lookup_us_cost = (unsigned)atoi(argv[++i]) * 1000u;
        else if (!strcmp(argv[i], "--cps") && i + 1 < argc) cps = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lines") && i + 1 < argc) limit = (unsigned)atoi(argv[++i]);
        else for (int k = 0; k < 6; k++) if (!strcmp(argv[i], names[k])) sc = k;
    }
    if (sc < 0 || !cps) {
        fprintf(stderr, "usage: vjo-hook-latency-bench reading|quick|subtitles|open|site|grow [--lookup-ms 70] [--cps 30] "
                        "[--lines N] < lines.txt\n");
        return 2;
    }
    scenario = (unsigned)sc;
    trace = getenv("LB_TRACE") != NULL;
    while (fgets(buf, sizeof(buf), stdin) && (!limit || nl < limit)) {
        size_t n = strcspn(buf, "\r\n");
        buf[n] = 0;
        if (!n) continue;
        if (nl == cap) { cap = cap ? cap * 2 : 256; ln = realloc(ln, cap * sizeof(*ln)); }
        memset(&ln[nl], 0, sizeof(ln[nl]));
        ln[nl].text = strdup(buf);
        ln[nl].chars = utf8_chars(buf);
        ln[nl].parts = 1;
        if (scenario == SC_GROW && ln[nl].chars >= LB_PARTS) {
            ln[nl].parts = LB_PARTS;
            for (unsigned k = 0; k < LB_PARTS; k++)
                ln[nl].part[k] = k + 1 == LB_PARTS ? ln[nl].text : utf8_prefix(buf, ln[nl].chars * (k + 1) / LB_PARTS);
        }
        nl++;
    }
    if (!nl) { fprintf(stderr, "no lines\n"); return 2; }
    /* the schedule: the first line 200 ms in (before the empty box would settle), then a reader's pace (the typing,
     * then 1.5 s and 120 ms a character, at most 15 s), or the player clicking on (quick) */
    uint64_t t = 200000;
    for (unsigned i = 0; i < nl; i++) {
        uint64_t typing = (uint64_t)ln[i].chars * 1000000ull / cps, read = 1500000ull + ln[i].chars * 120000ull;
        if (read > 15000000ull) read = 15000000ull;
        ln[i].arr = t;
        t += scenario == SC_QUICK ? 500000ull : typing + read;
        ln[i].next = t;
    }
    session_end = ln[nl - 1].arr + 5000000ull;
    /* the kernel as test_game_shot.c sets it up; the region: the dialogue box */
    memset(&g, 0, sizeof(g));
    game_ocr_init();
    g.lock = 1; g.ievf = 6; g.evf = 2; g.mevf = 5;
    g.shell_pid = 3; g.game_pid = 7; g.game_active = VJO_GAME; g.ocr_epoch = 1;
    g.fb_w = FB_W; g.fb_h = FB_H; g.fb_pitch = FB_W; g.fb_fmt = VJO_FB_A8B8G8R8;
    g.done_seq = g.capture_seq = 11;
    g.region = (VjoRect){(uint16_t)(BOX_X * 65536u / FB_W), (uint16_t)(BOX_Y * 65536u / FB_H),
                         (uint16_t)(BOX_W * 65536u / FB_W), (uint16_t)(BOX_H * 65536u / FB_H)};
    scene_reset(&g.tracker);
    /* the dialogue's source, trusted: a learned string copy (four agreements and a close one, earlier in the session)
     * or a Rubi verified site; the worker's state as after the title screen's job (its hook counts; API 49: its
     * VJO_GAME_OCR_S_TRUST) */
    if (scenario == SC_SITE) {
        src_id = 2;
        fs[2].kind = VJO_HOOK_KIND_SITE; fs[2].key = 0x2222u;
        verified_mask = 1u << 1;
    } else {
        src_id = 9;
        fs[9].kind = VJO_HOOK_KIND_CALL; fs[9].key = 0x1234u;
        hk.sel.locked = 9; hk.sel.locked_key = 0x1234u; hk.sel.locked_encoding = VJO_HOOK_ENC_UTF8;
        hk.sel.interval = VJO_HOOK_TRUST_INTERVAL; hk.sel.strong = 1;
    }
    vjo_hooks_counts(&hk.mode, &hk.installed, &hk.refused);
#if VJO_API_VERSION >= 49
    hk.trust_on = 1;
#endif
    subtitles = scenario == SC_SUBTITLES;
    lb_spawn(LB_WORKER, worker_thread, 7, 0x7001);
    lb_spawn(LB_KERNEL, kernel_main, 0, 0x1001);
    lb_spawn(LB_GAME, game_main, 7, 0x7002);
    lb_spawn(LB_CTL, ctl_main, 3, 0x3001);
    lb_spawn(LB_NET, net_main, 3, 0x3002);
    lb_run(session_end);
    /* the report */
    double *ready_v = calloc(nl, sizeof(double)), *text_v = calloc(nl, sizeof(double)), *first_v = calloc(nl, sizeof(double));
    unsigned nr = 0, nt = 0, nf = 0, skipped = 0, from = 1u; /* the first line: apart (the session's start) */
    for (unsigned i = 0; i < nl; i++) {
        LbLine *l = &ln[i];
        printf("{\"line\": %u, \"chars\": %u, \"report_ms\": %.1f, \"start_ms\": %.1f, \"text_ms\": %.1f, "
               "\"first_ms\": %.1f, \"ready_ms\": %.1f, \"ready_last_ms\": %.1f, \"skipped\": %d, \"repeat\": %d}\n",
               i, l->chars, ms(l->rep, l->arr), ms(l->start, l->arr), ms(l->result, l->arr), ms(l->first_text, l->arr),
               ms(l->ready, l->arr), l->ready >= l->last ? ms(l->ready, l->last) : -1.0, l->skipped, l->repeat);
        if (i < from || l->repeat) continue; /* a repeated text is no hook event: the screen's, as without a hook */
        if (l->ready) ready_v[nr++] = ms(l->ready, l->arr);
        else skipped++;
        if (l->result) text_v[nt++] = ms(l->result, l->arr);
        if (l->first_text) first_v[nf++] = ms(l->first_text, l->arr);
    }
#ifdef HOOK_WATCH_US
    unsigned watch = HOOK_WATCH_US;
#else
    unsigned watch = 0;
#endif
    printf("{\"summary\": \"%s\", \"api\": %d, \"watch_us\": %u, \"lookup_ms\": %u, \"cps\": %u, \"lines\": %u, "
           "\"counted\": %u, \"not_ready\": %u, ", names[scenario], VJO_API_VERSION, watch, lookup_us_cost / 1000u, cps,
           nl, nl - from, skipped);
    stats("ready_ms", ready_v, nr, 1);
    stats("text_ms", text_v, nt, 1);
    stats("first_text_ms", first_v, nf, 1);
    printf("\"first_line_ready_ms\": %.1f, \"rounds_per_s\": %.2f, \"worker_host_ns_per_round\": %.0f, "
           "\"reports\": %u, \"shell_jobs\": %u, \"session_s\": %.1f}\n", ms(ln[0].ready, ln[0].arr),
           takes / ((double)session_end / 1e6), takes ? (double)th[LB_WORKER].host_ns / takes : 0.0, reports,
           shell_jobs, (double)session_end / 1e6);
    return 0;
}
