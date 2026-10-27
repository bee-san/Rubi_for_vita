/* The actual Shell worker (shell/worker.c) with only Vita scheduling and memory stubbed. */
#include "acutest.h"
#include <stdarg.h>
#include <stdio.h>
#include "client.h"
#include "platform_posix.h"
#include "textfilter.h"

#ifdef VJO_VOCR_TIERS_TEST
/* Another package's tier map (host/tests/test_shared_tiers.h): this file's own
 * tests for it only, its TEST_LIST at the end; the others are left unused. */
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define VJO_TEST_MEMORY_STUBS 1
#define VJO_TEST_THREAD_STUBS 1
#define VJO_TEST_MUTEX_STUBS 1
#define VJO_TEST_THREAD_INFO_STUBS 1
#include <psp2/host_stubs.h>
#include <pthread.h>

static VjoState state;
static VjoConfig loaded;
static uint64_t test_now;
static unsigned posted, user_allocs, user_frees, heap_allocs, heap_frees;
static int fail_alloc, fail_base, input_blocked, fail_input_block, game_enabled = 1;
static uint8_t storage[2 * 1024 * 1024];
/* Separate simulated game process workspace. Shell allocator must never own it. */
static uint8_t vocr_storage[2 * 1024 * 1024] __attribute__((aligned(64)));
static unsigned vocr_allocs, vocr_frees, vocr_paf_allocs;
static int fail_vocr_user, fail_vocr_paf;
static int phases_checked, vocr_live, fail_join;
static unsigned char raw_region[200 * 60 * 4];
static VjoState raw_state;
static int has_result_memory(void);
static int held_cmd, held_view, retirement_checks;
/* Set by the refresh tests: whenever the view lock is released, an open local
 * view that shows local_text shows it with its own generation (a replaced
 * text never appears with the old one's entry or cursor). */
static int view_invariant, view_invariant_checks;
static uint32_t invariant_generation;
static unsigned invariant_seq;
uint64_t sceKernelGetProcessTimeWide(void) { return test_now; }
/* The calling thread's stack as the kernel reports it (VjoNet's high-water mark):
 * by default a 16 KiB buffer the host's frames are not in; the thread test reports
 * its own thread's stack (thread_info_stack), or misreads it on purpose. */
static uint8_t fake_stack[16384];
static int thread_info_rc, thread_info_size = (int)sizeof(fake_stack);
static uintptr_t thread_info_below_sp; /* nonzero: pStack is this far below the SP at the call */
static void *thread_info_stack;
/* The stack pointer where it is read. gcc inlines the stub below into net_stack_paint, so a local's
 * address or __builtin_frame_address(0) here can lie above the caller's own frame
 * (audit-overlay-shell-slim-2b B1); the register cannot. */
static inline __attribute__((always_inline)) uintptr_t current_sp(void)
{
    uintptr_t sp;
#if defined(__x86_64__)
    __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
#elif defined(__i386__)
    __asm__ volatile("mov %%esp, %0" : "=r"(sp));
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ volatile("mov %0, sp" : "=r"(sp));
#else
    sp = (uintptr_t)__builtin_frame_address(0); /* weaker: may lie above the caller's frame */
#endif
    return sp;
}
SceUID sceKernelGetThreadId(void) { return 30; }
int sceKernelGetThreadInfo(SceUID thid, SceKernelThreadInfo *info)
{
    if (thid != 30 || info->size != sizeof(*info) || thread_info_rc < 0) return thread_info_rc < 0 ? thread_info_rc : -1;
    /* thread_info_below_sp: pStack read near "the thread's stack pointer" (VitaSDK's name for the
     * field), that far below the SP at this call (the caller's frame lies a little above it) */
    info->stack = thread_info_below_sp ? (void *)(current_sp() - thread_info_below_sp)
                  : thread_info_stack ? thread_info_stack : fake_stack;
    info->stackSize = thread_info_size;
    return 0;
}
/* The log's clock (kernel.txt's): system time, here 3 s ahead of SceShell's process time. */
uint64_t sceKernelGetSystemTimeWide(void) { return test_now + 3000000; }
int sceKernelWaitThreadEnd(SceUID uid, int *status, void *timeout) { return fail_join ? -1 : 0; }
/* on_event_flag: a test's hook, called after the bits are posted (the network thread waking at once);
 * reset() installs post_hook (a check for a whole test's posts, the session's included). */
static void (*on_event_flag)(unsigned bits);
static void (*post_hook)(unsigned bits);
int sceKernelSetEventFlag(SceUID id, unsigned bits) { posted |= bits; if (on_event_flag) on_event_flag(bits); return 0; }
int vjoGetState(VjoState *out) { *out = state; return 0; }
static VjoRect saved_region; /* w != 0: a saved region for every game */
static int regions_set, last_region_full;
int vjoSetRegion(const VjoRect *r) { regions_set++; last_region_full = r == NULL; return 0; }
int vjoSetTriggers(int a, int b) { return 0; }
/* API 31: the text source the Shell pushed from its config, and how often */
static uint32_t text_source_pushed = 0xFFFFFFFFu;
static unsigned text_source_pushes;
int vjoSetTextSource(uint32_t mode) { text_source_pushed = mode; text_source_pushes++; return 0; }
/* The order of a game start's steps, one letter each: A vjoSetGameActive (a game), C a config load, S a
 * stamp read (the Rubi app check). */
static char start_order[64];
static void start_step(char c) { size_t n = strlen(start_order); if (n + 1 < sizeof(start_order)) start_order[n] = c, start_order[n + 1] = 0; }
static int active_mode = -1; /* the last vjoSetGameActive's */
int vjoSetGameActive(int pid, int mode) { active_mode = mode; if (mode != VJO_GAME_NONE) start_step('A'); return 0; }
static int fail_input_unblock; /* the kernel refuses to drop the block (vjoSetInputBlock(0)) */
int vjoSetInputBlock(int on)
{
    if (on && fail_input_block) return -123;
    if (!on && fail_input_unblock) return (int)0x80010016;
    input_blocked = on;
    return 0;
}
int vjoGetVersion(void) { return VJO_API_VERSION; }
int vjoRegisterShell(void) { return 0; }
int vjoWaitEvent(uint32_t mask, uint32_t *out, uint32_t timeout) { *out = 0; return -1; }
static unsigned shell_notifications;
/* A test's look at the moment the network thread tells the control thread (job_done, job_text_ready). */
static void (*on_notify)(void);
int vjoNotifyShell(void) { shell_notifications++; if (on_notify) on_notify(); return 0; }
SceUID sceKernelAllocMemBlock(const char *name, int type, unsigned size, void *opt)
{
    TEST_CHECK(strcmp(name, "VjoVocr") && strcmp(name, "VjoGameVocr"));
    if (phases_checked) TEST_CHECK(!vocr_live && size == 64 * 1024);
    user_allocs++; TEST_CHECK(size <= sizeof(storage)); return fail_alloc ? -1 : 7;
}
int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    if (uid == 9) { *base = vocr_storage; return 0; }
    *base = fail_base ? NULL : storage; return fail_base ? -1 : 0;
}
int sceKernelFreeMemBlock(SceUID uid) { if (uid == 9) { vocr_frees++; vocr_live = 0; } else user_frees++; return 0; }

/* net_main itself in a test: its waits come from the test's script (test_wait_event_flag), not the
 * SDK stub's (which never wakes). */
static int test_wait_event_flag(SceUID uid, unsigned int bits, unsigned int mode, unsigned int *out,
                                unsigned int *timeout);
#define sceKernelWaitEventFlag(uid, bits, mode, out, timeout) test_wait_event_flag(uid, bits, mode, out, timeout)
#include "../../shell/worker.c"
#undef sceKernelWaitEventFlag
#include "../../shell/ledger.c" /* the ledger worker.c counts in */
#include "../../shell/strip_view.c" /* the strip view model worker.c publishes through */
#include "../../core/stick_cursor.h" /* the overlay's left stick (#138): its close against this worker */
#include "../../core/text_rows.h"
#include "../../shell/lookup_shown.h" /* the overlay's press-to-screen figure (#360) */

/* run_job and run_cursor_job as a test runs them, outside net_job: the debug build's lines they hold
 * (net_log_hold, #360) go out as net_job's end writes them after the result, so no batch is left open
 * across the next job's exchange (exchange_batch_ok). */
static void net_log_end_t(void)
{
#if VJO_DEBUG
    vjo_log_batch_end(&net_held->b);
    net_held->b.buf = NULL;
#endif
}
static int run_job_t(VjoArena *a, VjoOverlayData *out) { int rc = run_job(a, out); net_log_end_t(); return rc; }
static int run_cursor_job_t(VjoArena *a, VjoOverlayData *out)
{
    int rc = run_cursor_job(a, out);
    net_log_end_t();
    return rc;
}

/* audit-overlay-shell-slim-1 A5: the overlay's thread posts a cursor move at the
 * n-th cmd_lock acquisition from now (vjo_post_cursor runs on Paf's thread). */
static int race_on_lock;
static void post_local_lookup(uint32_t generation);
static void cursor_race(void) { local_cursor_byte = 3; post_local_lookup(local_text_generation); }

int sceKernelLockMutex(SceUID uid, int count, void *timeout)
{
    if (uid == 81) { TEST_CHECK(!held_view); held_view++; }
    if (uid == 82) { TEST_CHECK(!held_cmd && !held_view); held_cmd++;
        if (race_on_lock && --race_on_lock == 0) cursor_race(); }
    return 0;
}
int sceKernelUnlockMutex(SceUID uid, int count)
{
    if (uid == 81 && view_invariant && g_view.open && g_view.local_cursor_mode && g_view.list &&
        g_view.list->header == local_text && local_text_valid) {
        view_invariant_checks++;
        TEST_CHECK_(g_view.text_generation == local_text_generation && g_view.cursor_byte == local_cursor_byte &&
                    (g_view.list == &local_header || g_view.list == &cache_data[0].list),
                    "view generation %u, text %u", g_view.text_generation, local_text_generation);
        /* an entry shown is the current lookup's (never the replaced text's) */
        TEST_CHECK(g_view.list != &cache_data[0].list || cache_ok || !g_view.list->n_entries);
        /* a new text is published with a new list_seq: an Anki save asked for the old list fails */
        TEST_CHECK_(g_view.text_generation == invariant_generation || g_view.list_seq != invariant_seq,
                    "text %u shown under the old list_seq %u", g_view.text_generation, g_view.list_seq);
        invariant_generation = g_view.text_generation;
        invariant_seq = g_view.list_seq;
    }
    if (uid == 81) { TEST_CHECK(held_view == 1); held_view--; }
    if (uid == 82) {
        TEST_CHECK(held_cmd == 1 && !held_view);
        if (!local_text_valid && !g_view.list && !g_view.local_cursor_mode) retirement_checks++;
        held_cmd--;
    }
    return 0;
}

void *vjo_shell_heap_alloc(size_t size)
{ if (phases_checked) TEST_CHECK(!vocr_live && size == 64 * 1024);
  heap_allocs++; TEST_CHECK(size <= sizeof(storage)); return fail_alloc ? NULL : storage; }
/* Line history's visit block (VJO_SL_PAF_HISTORY): one at a time, filled with a pattern when taken
 * (nothing relies on zeroed memory) and when given back (a pointer the view kept into it shows). */
static uint8_t history_storage[VJO_BUDGET_PAF_HISTORY] __attribute__((aligned(16)));
static unsigned history_allocs, history_frees;
static int fail_history_alloc;
void *vjo_shell_heap_alloc_for(int item, size_t size)
{
    if (item == VJO_SL_PAF_RESULTS) {
        void *p = vjo_shell_heap_alloc(size);
        if (p) vjo_ledger_add(&g_shell_ledger, item, (uint32_t)size);
        return p;
    }
    if (item == VJO_SL_PAF_HISTORY) {
        TEST_CHECK(size == VJO_BUDGET_PAF_HISTORY && history_allocs == history_frees && !held_view && !held_cmd);
        if (fail_history_alloc) return NULL;
        history_allocs++;
        memset(history_storage, 0xA5, sizeof(history_storage));
        vjo_ledger_add(&g_shell_ledger, item, (uint32_t)size);
        return history_storage;
    }
    vocr_paf_allocs++;
    TEST_CHECK(0); /* game vocr must never borrow the Shell Paf heap */
    return NULL;
}
void vjo_shell_heap_free(int item, void *p, size_t bytes)
{
    if (p == history_storage) {
        TEST_CHECK(item == VJO_SL_PAF_HISTORY && bytes == VJO_BUDGET_PAF_HISTORY && history_frees < history_allocs);
        TEST_CHECK(!g_view.history_before && !g_view.history_after && !g_view.history); /* the view let it go */
        history_frees++;
        memset(history_storage, 0x5A, sizeof(history_storage));
        vjo_ledger_sub(&g_shell_ledger, item, (uint32_t)bytes);
        return;
    }
    TEST_CHECK(p == storage || p == vocr_storage);
    if (p == vocr_storage) { vocr_frees++; vocr_live = 0; } else heap_frees++;
    vjo_ledger_sub(&g_shell_ledger, item, (uint32_t)bytes);
}
uint32_t vjo_shell_heap_free_size(void) { return 2557952; }
void vjo_config_load(VjoConfig *out, VjoArena *a) { start_step('C'); *out = loaded; }
static void configure_stub(const VjoConfig *c);
void vjo_log_configure(const VjoConfig *c) { configure_stub(c); }
/* The last log lines, for the game OCR measurement tests. */
static char log_lines[32][256];
static unsigned log_count;
/* Every config warning line (the last 16) and the Rubi app check's lines, counted since reset. */
static char warning_logs[16][128], last_app_log[256];
static unsigned config_warning_logs, app_logs;
static unsigned warnings_logged(const char *text)
{
    unsigned n = 0;
    for (unsigned i = 0; i < config_warning_logs && i < 16; i++)
        n += !strcmp(warning_logs[i], text);
    return n;
}
/* The logger's batches (shell/log_vita.c vjo_log_batch_begin/end), by its own
 * rules: a line outside a batch is one append of log.txt; a batch's lines go
 * out together when it ends, or when the next one would pass its
 * VJO_LOG_BATCH_LINES lines or its buffer (host/tests/test_shell_log.c shows
 * the real logger writes exactly these appends, byte for byte as line by line). */
/* test_thread: the thread logging (the harness runs the network thread's work as NET_THREAD:
 * as_network_thread, finish_current_job, run_vocr_job). A batch's lines go out together when it ends,
 * when the next one would pass its VJO_LOG_BATCH_LINES lines or its buffer, or when another thread logs a
 * line (that line is then written at once, after them), as shell/log_vita.c writes them. */
enum { CTL_THREAD = 1, NET_THREAD = 2 };
static int test_thread = CTL_THREAD, batch_owner;
static VjoLogBatch *open_batch;
static int batch_lines_held, batch_bytes_held, batch_cap;
static unsigned log_appends, batched_lines;
/* Each append: its lines, the thread that made it, whether a job was running (job_running: the control
 * thread had not taken its result yet), the network thread's phase then (net_phase), whether its job had
 * published its result (job_done: an append after it is not in the result's path), and the overlay's
 * view version (g_view.version: an append before a result is shown has a smaller one). */
#define APPEND_LOG 4096
typedef struct { unsigned lines; int thread, job_running, in_result_path, recognizing, job_done; unsigned view; } AppendSeen;
static AppendSeen appends_seen[APPEND_LOG];
static unsigned appends_recorded, result_path_appends, recognizing_appends, lines_appended, most_lines_appended;
static unsigned lines_logged, net_lines_logged, most_lines_held;
/* log.txt as the appends write it (#360): every line written since reset, in the order written; line k
 * since reset is the log_lines ring's lines_logged - k lines back from its newest (a test may set log_count
 * back to 0: the ring's index and lines_logged move together). The ring holds 32 lines, more than a batch
 * holds (VJO_LOG_BATCH_LINES), so a written line's text is always there. */
static char file_text[32768];
static size_t file_len;
static void file_note(unsigned lines)
{
    for (unsigned k = lines_appended; k < lines_appended + lines; k++) {
        const char *line = log_lines[(log_count - lines_logged + k) % 32];
        size_t n = strlen(line);
        if (file_len + n + 1 < sizeof(file_text)) {
            memcpy(file_text + file_len, line, n);
            file_text[file_len + n] = '\n';
            file_len += n + 1;
            file_text[file_len] = 0;
        }
    }
}
/* The network thread's place in a job: NET_IDLE (no job, or one not submitted yet), NET_RECOGNIZING (the
 * game has the job: its submission was taken, its result not read yet) or NET_RESULT (the result is
 * known: read from the game, or the submission refused; a lookup's whole run), until job_done. The
 * result's path is NET_RESULT: the normal build makes no append of the network thread there. */
enum { NET_IDLE, NET_RECOGNIZING, NET_RESULT };
static int net_phase;
static void note_append(unsigned lines)
{
    AppendSeen a = {lines, test_thread, job_running,
                    test_thread == NET_THREAD && net_phase == NET_RESULT && job_running && !job_done,
                    test_thread == NET_THREAD && net_phase == NET_RECOGNIZING, job_done, g_view.version};
    if (appends_recorded < APPEND_LOG)
        appends_seen[appends_recorded] = a;
    appends_recorded++;
    result_path_appends += (unsigned)a.in_result_path;
    recognizing_appends += (unsigned)a.recognizing;
    file_note(lines);
    lines_appended += lines;
    if (lines > most_lines_appended) most_lines_appended = lines;
    log_appends++;
}
/* net_log_locks: the network thread's calls that take log_mutex without a line to log (a begin or an
 * end; the real logger takes it for every call): a steady-state quiet job makes none. */
static unsigned net_log_locks;
/* A flush of the open batch's lines (shell/log_vita.c batch_flush_locked) reads its buffer: it must be set. */
static void note_batch_append(void)
{
    TEST_CHECK_(open_batch->buf != NULL, "a flush of %d held lines with no buffer", batch_lines_held);
    note_append((unsigned)batch_lines_held);
}
int vjo_log_batch_begin(VjoLogBatch *b, char *buf, int cap)
{
    net_log_locks += test_thread == NET_THREAD;
    if (open_batch || !b || !buf || cap <= 0) return 0; /* as shell/log_vita.c: no buffer, no batch */
    open_batch = b; batch_cap = cap; batch_lines_held = batch_bytes_held = 0; batch_owner = test_thread;
    b->buf = buf; b->n = b->used = 0; /* as shell/log_vita.c sets them: buf only when it opens */
    return 1;
}
void vjo_log_batch_end(VjoLogBatch *b)
{
    net_log_locks += test_thread == NET_THREAD;
    if (!b || open_batch != b) return;
    if (batch_lines_held) note_batch_append();
    batch_lines_held = batch_bytes_held = 0;
    b->n = b->used = 0;
    open_batch = NULL;
}
void vjo_log(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(log_lines[log_count % 32], sizeof(log_lines[0]), fmt, ap);
    va_end(ap);
    if (!strncmp(log_lines[log_count % 32], "config warning: ", 16))
        snprintf(warning_logs[config_warning_logs++ % 16], sizeof(warning_logs[0]), "%s", log_lines[log_count % 32] + 16);
    if (!strncmp(log_lines[log_count % 32], "Rubi app ", 9)) {
        app_logs++;
        snprintf(last_app_log, sizeof(last_app_log), "%s", log_lines[log_count % 32]);
    }
    log_count++;
    lines_logged++;
    net_lines_logged += test_thread == NET_THREAD;
    n = (n > 254 ? 254 : n) + 1; /* the line and its '\n', as vjo_log writes it */
    if (!open_batch) { note_append(1); return; }
    if (batch_owner != test_thread) { /* another thread's line: the batch's lines first, then this one */
        if (batch_lines_held) note_batch_append();
        batch_lines_held = batch_bytes_held = 0;
        open_batch->n = open_batch->used = 0;
        note_append(1);
        return;
    }
    batched_lines++;
    if (batch_lines_held < VJO_LOG_BATCH_LINES && batch_cap - batch_bytes_held >= n) {
        batch_lines_held++; batch_bytes_held += n;
        open_batch->n = batch_lines_held;
        open_batch->used = batch_bytes_held;
        if ((unsigned)batch_lines_held > most_lines_held) most_lines_held = (unsigned)batch_lines_held;
        return;
    }
    if (batch_lines_held) note_batch_append(); /* the batch so far goes out first */
    batch_lines_held = 1; batch_bytes_held = n;
    open_batch->n = 1;
    open_batch->used = n;
}
/* shell/log_vita.c's vjo_log_configure: the open batch's lines go out first, on the caller's thread; in
 * the normal build only when log_file changes (with it kept, a held batch stays held). */
static int configure_log_file;     /* the last configure's log_file */
static unsigned configure_writes;   /* configures that wrote held lines */
static void configure_stub(const VjoConfig *c)
{
#if !VJO_DEBUG
    if (c->log_file != configure_log_file)
#endif
        if (open_batch && batch_lines_held) {
            note_batch_append();
            configure_writes++;
            batch_lines_held = batch_bytes_held = 0;
            open_batch->n = 0;
        }
    configure_log_file = c->log_file;
}
/* net_main's batch outside net_main (the harness's jobs): at a job's wake net_log_wake opens it unless it
 * is open (its stack buffer's stand-in here); after a wait, net_log_idle writes what is held once no job
 * runs. Only a timed wait (a line held) ends without a job, so the harness's idle step calls it only
 * then: an empty batch stays open, as net_main's untimed wait keeps it (audit-t3-log-path-d's N6). */
#if !VJO_DEBUG
static char net_log_test_buf[NET_LOG_BYTES]; /* net_main's held_buf */
static int net_wait_timed; /* the wait net_main goes back to: timed iff its batch held a line then */
static void net_job_begin(void) { net_log_wake(net_log_test_buf); }
static void net_job_end(void) { net_wait_timed = net_log.n != 0; }
static void net_idle_write(void)
{
    if (net_wait_timed) {
        net_log_idle();
        net_wait_timed = net_log.n != 0;
    }
}
#endif
/* net_main's waits, scripted: each wait runs the test's next control-thread step (wait_script) and returns
 * the bits it posted, cleared at the wake; with nothing posted, a timed wait times out (as the SDK's:
 * SCE_KERNEL_ERROR_WAIT_TIMEOUT, *out the flag's bits, 0). The timeout asked is recorded. */
static int (*wait_script)(int step);
static int wait_step;
static unsigned last_wait_timeout; /* the last wait's timeout (0: none, an untimed wait) */
static int test_wait_event_flag(SceUID uid, unsigned int bits, unsigned int mode, unsigned int *out,
                                unsigned int *timeout)
{
    unsigned give;
    (void)uid; (void)mode;
    if (!wait_script) return -1; /* as the SDK stub: no thread waits here */
    last_wait_timeout = timeout ? *timeout : 0;
    net_phase = NET_IDLE; /* waiting: between jobs */
    test_thread = CTL_THREAD;
    if (!wait_script(wait_step++)) { running = 0; posted |= NET_EV_QUIT; }
    test_thread = NET_THREAD;
    give = posted & bits;
    posted &= ~give;
    *out = give;
    if (give) return 0;
    if (timeout) { *timeout = 0; return (int)0x80028005; } /* SCE_KERNEL_ERROR_WAIT_TIMEOUT */
    TEST_CHECK_(0, "an untimed wait with nothing posted would never return");
    running = 0;
    *out = NET_EV_QUIT;
    return 0;
}
static const char *logged(const char *needle)
{
    for (unsigned i = log_count > 32 ? log_count - 32 : 0; i < log_count; i++)
        if (strstr(log_lines[i % 32], needle)) return log_lines[i % 32];
    return NULL;
}
/* A debug-build line (docs/release-variants.md): the debug build logs it, the
 * normal build never does (vjo_dlog). */
#if VJO_DEBUG
#define DLOGGED(needle) (logged(needle) != NULL)
#else
#define DLOGGED(needle) (logged(needle) == NULL)
#endif
/* #358: region.ini's line for every game, "region = full" (the Rubi app's Suggest a text box OFF) */
static int saved_region_all_full;
int vjo_region_load(const char *id, VjoRect *out, VjoArena *a)
{
    if (!saved_region.w) {
        memset(out, 0, sizeof(*out));
        return saved_region_all_full ? VJO_REGION_ALL : 0;
    }
    *out = saved_region;
    return VJO_REGION_GAME;
}
/* #358: the saves (the last one's rect; NULL: the full screen) */
static int region_saves, region_save_failure;
static VjoRect region_saved_rect;
int vjo_region_save(const char *id, const VjoRect *r, VjoArena *a)
{
    region_saves++;
    region_saved_rect = r ? *r : (VjoRect){0, 0, 0, 0};
    return region_save_failure ? -1 : 0;
}
/* The Rubi app's stamps and its eboot.bin (shell/files.c), scripted (core/app_stamp.h). */
static char app_installed[64], app_record[64];
static int app_installed_rc, app_record_rc, app_eboot;
int vjo_file_line(const char *path, char *buf, int cap)
{
    int installed = !strcmp(path, VJO_APP_STAMP_INSTALLED);
    TEST_CHECK_(installed || !strcmp(path, VJO_APP_STAMP_PACKAGE), "read %s", path);
    start_step('S');
    int rc = installed ? app_installed_rc : app_record_rc;
    snprintf(buf, (size_t)cap, "%s", rc >= 0 ? (installed ? app_installed : app_record) : "");
    return rc;
}
int vjo_file_exists(const char *path)
{
    TEST_CHECK_(!strcmp(path, VJO_APP_EBOOT_INSTALLED), "stat %s", path);
    return app_eboot;
}
static int anki_configured, anki_reaps, anki_stop_rc, anki_counts;
/* The self-test's interface (shell/selftest.c writes the report): the stages
 * one job runs, recorded here. */
static uint32_t selftest_flags_seen[8], selftest_speed_seen[8]; /* every job's request (vjoOcrSubmit) */
#if VJO_DEBUG /* the normal build has no self-test: shell.h's stubs, worker.c's own vjo_selftest_init */
static int selftest_armed, selftest_games, selftest_steps, selftest_done;
static unsigned selftest_begins, selftest_ends, selftest_stage_count;
static int selftest_rcs[8];
static VjoRect selftest_region;
/* shell/selftest.c's order: the last stage's result is the job's */
static const char *const selftest_names[] = {"fallback", "textbox", "fullscreen", "probe", "textbox2"};
static const uint32_t selftest_stage_flags_tab[] = {VJO_GAME_OCR_F_FORCE_FALLBACK, 0, VJO_GAME_OCR_F_FULL,
                                                    VJO_GAME_OCR_F_PROBE, 0};
int vjo_selftest_armed(void) { return selftest_armed && !selftest_done; }
void vjo_selftest_init(const VjoConfig *cfg) {}
void vjo_selftest_step(const char *step) { selftest_steps++; }
void vjo_selftest_game(const char *title, int enabled, int mode, const VjoRect *region)
{ selftest_games++; if (region) selftest_region = *region; }
unsigned vjo_selftest_stages(void) { return vjo_selftest_armed() ? selftest_stage_count : 0; }
const char *vjo_selftest_stage_name(unsigned i) { return i < 5 ? selftest_names[i] : "?"; }
uint32_t vjo_selftest_stage_flags(unsigned i) { return i < 5 ? selftest_stage_flags_tab[i] : 0; }
static uint32_t selftest_begin_speed[8];
void vjo_selftest_stage_begin(unsigned i, const char *model, const char *fallback, uint32_t speed)
{ if (i < 8) selftest_begin_speed[i] = speed; selftest_begins++; }
/* shell/selftest.c's rule: no stage uses the line cache, textbox2 (stage 4) runs without helpers (its other bits
 * kept, as there: the clock and, API 43, the trust bit) */
uint32_t vjo_selftest_stage_speed(unsigned i, uint32_t configured)
{ return (i == 4 ? configured & ~VJO_GAME_OCR_S_HELPERS_MASK : configured) | VJO_GAME_OCR_S_NOCACHE; }
/* shell/selftest.c's rule (its report lines are host/tests/selftest_writer.c's). */
static int selftest_waits;
int vjo_selftest_stage_wait(unsigned i, int rc, const VjoGameOcrResult *r)
{
    if (!vjo_selftest_armed() || i || !((rc == VJO_OK && !r->lines) || rc == VJO_E_SCREEN_CHANGED) ||
        selftest_waits >= VJO_SELFTEST_WAITS)
        return 0;
    selftest_waits++;
    return 1;
}
void vjo_selftest_stage_end(unsigned i, int rc, const VjoGameOcrResult *r, uint32_t ms, const char *model)
{ if (selftest_ends < 8) selftest_rcs[selftest_ends] = rc; selftest_ends++; }
void vjo_selftest_done(void) { selftest_done = 1; }
int vjo_gstep_format(char *buf, size_t cap, const char *who, const VjoGameOcrStep *st)
{ return snprintf(buf, cap, "gstep %s %u rc=0x%08X", who, (unsigned)st->id, (unsigned)st->rc); }
#else
static int selftest_waits;
/* The normal build's vjo_selftest_init reads whether the marker is there (sceIoGetstat). */
static int selftest_marker, selftest_marker_reads;
int sceIoGetstat(const char *path, SceIoStat *st)
{
    selftest_marker_reads++;
    TEST_CHECK(!strcmp(path, "ux0:data/VitaJPOverlay/selftest.txt"));
    if (st) memset(st, 0, sizeof(*st));
    return selftest_marker ? 0 : (int)0x80010002;
}
#endif
static int guard_marks[8], guard_mark_rc;
int vjo_guard_mark(int stage) { if (stage >= 0 && stage < 8) guard_marks[stage]++; return guard_mark_rc; }
int vjo_anki_start(void) { return 0; }
int vjo_anki_stop(void) { return anki_stop_rc; }
int vjo_anki_configure(const VjoConfig *c) { anki_configured++; return c->anki_mining; }
void vjo_anki_reap(void) { anki_reaps++; }
void vjo_anki_post_count(void) { anki_counts++; }
static char anki_game[128], anki_tid[16], anki_model[64];
static int anki_psp;
const char *vjo_anki_get_game(char name[128], char tid[12])
{
    snprintf(name, 128, "%s", anki_game);
    snprintf(tid, 12, "%s", anki_tid);
    return anki_psp ? "psp" : "vita";
}
void vjo_anki_set_game(const char *tid, const char *name, int psp)
{
    snprintf(anki_game, sizeof(anki_game), "%s", name);
    snprintf(anki_tid, sizeof(anki_tid), "%s", tid);
    anki_psp = psp;
}
/* Runs once, as the control thread, where run_vocr calls this stub: after the game's answer (the exchange's
 * last cancel check) and before the text is published (#186's audit N1: a region change in that window). */
static void (*between_answer_and_publish)(void);
void vjo_anki_set_ocr_model(const char *model)
{
    snprintf(anki_model, sizeof(anki_model), "%s", model);
    if (between_answer_and_publish) {
        void (*hook)(void) = between_answer_and_publish;
        int thread = test_thread;
        between_answer_and_publish = NULL;
        test_thread = CTL_THREAD;
        hook();
        test_thread = thread;
    }
}
void vjo_title_name(SceUID pid, const char *tid, char *name, int size) { snprintf(name, size, "Title of %s", tid); }
void vjo_platform_vita(VjoPlatform *p) {}
/* shell/game.c (its own suite, shell_game): not readable unless a test says. */
static int identify_rc = -1, enabled_psp = -1;
static VjoGameId identified;
int vjo_game_identify(SceUID pid, VjoGameId *out)
{ /* as shell/game.c: all zero when the title is not readable */
    if (!identify_rc) *out = identified; else memset(out, 0, sizeof(*out));
    return identify_rc;
}
int vjo_game_enabled(const char *id, int psp, VjoArena *a) { enabled_psp = psp; return game_enabled; }
static void activate(SceUID pid, const char *tid, int mode)
{
    VjoGameId id;
    memset(&id, 0, sizeof(id));
    snprintf(id.key, sizeof(id.key), "%s", tid);
    id.mode = mode;
    activate_game(pid, &id);
}

int sceKernelDelayThread(unsigned us) { test_now += us; return 0; }
#ifdef VJO_WITH_VOCR
#include "../../core/vocr_ocr.h"
#include "../../core/textnorm.h"
static unsigned history_calls;
static int history_rtc_fail, history_write_fail;
static VjoHistoryRecord last_history;
static char history_text[VJO_GAME_OCR_TEXT_BYTES];
static uint32_t history_text_from; /* the record's result's text_from, as the writer sees it */
static void (*history_on_append)(void);
int sceRtcGetCurrentClockLocalTime(SceDateTime *date) { (void)date; return -1; }
int sceRtcGetTick(const SceDateTime *date, SceRtcTick *tick) { (void)date; (void)tick; return -1; }
int sceRtcConvertLocalTimeToUtc(const SceRtcTick *local, SceRtcTick *utc) { (void)local; (void)utc; return -1; }
int sceRtcGetCurrentTick(SceRtcTick *tick)
{
    tick->tick = 62135596800000000ull + 1792220400250000ull + test_now;
    return history_rtc_fail ? -1 : 0;
}
int vjo_history_append(const VjoHistoryRecord *record)
{
    /* This is the first storage call: result publication must precede it,
     * with no control/view lock held. Saving failure is independent of OCR. */
    TEST_CHECK(job_done && !held_view && !held_cmd);
    history_calls++;
    last_history = *record;
    snprintf(history_text, sizeof(history_text), "%s", record->result->text);
    history_text_from = record->result->text_from;
    if (history_on_append) history_on_append();
    return history_write_fail ? -1 : 1;
}
/* The reading total (shell/history.c, its own tests in test_shell_history.c): the control thread's calls.
 * total_windows: the windows a count still has (each wakeup's step takes one, and wakes the thread again). */
static unsigned total_steps, total_keeps, total_windows;
int vjo_history_total_step(VjoArena *scratch)
{
    TEST_CHECK(scratch != NULL && !held_view && !held_cmd);
    total_steps++;
    return total_windows ? (int)(--total_windows > 0) : 0;
}
void vjo_history_total_keep(void) { TEST_CHECK(!held_view && !held_cmd); total_keeps++; }
static unsigned total_stops;
void vjo_history_total_stop(void) { TEST_CHECK(!held_view && !held_cmd && !threads_started); total_stops++; }
/* Line history: history.csv as the tests write it (history_csv), read by the real scanner
 * (core/history_lines.c) in one window, as shell/history.c reads its windows, through the scratch it
 * is given. history_loads counts the reads; history_load_rc < 0 is the file's error. */
static char history_csv[16384];
static unsigned history_loads;
static int history_load_rc;
static char history_uuid[24];
int vjo_history_load(VjoHistory *h, VjoArena *lines, const char *uuid, int filter, VjoArena *scratch)
{
    size_t mark = vjo_arena_mark(scratch), n = strlen(history_csv);
    char *buf = vjo_arena_alloc(scratch, VJO_HISTORY_WINDOW + 1);
    TEST_CHECK(!held_view && !held_cmd); /* no lock held (a job may run: it only reads local_text) */
    history_loads++;
    snprintf(history_uuid, sizeof(history_uuid), "%s", uuid);
    if (history_load_rc < 0 || !buf) {
        vjo_arena_release(scratch, mark);
        return history_load_rc < 0 ? history_load_rc : -1;
    }
    TEST_ASSERT(n <= VJO_HISTORY_WINDOW);
    memcpy(buf, history_csv, n);
    vjo_history_scan(h, lines, buf, n, 1, uuid, filter);
    vjo_arena_release(scratch, mark);
    return h->n;
}
/* A record of history.csv as shell/history.c writes it (its first four fields quoted, '"' doubled). */
static void history_csv_add(const char *uuid, const char *line)
{
    size_t n = strlen(history_csv);
    const char *f[4] = {uuid, "vjo-1-2", "CLANNAD", line};
    for (int k = 0; k < 4; k++) {
        history_csv[n++] = '"';
        for (const char *s = f[k]; *s; s++) {
            if (*s == '"') history_csv[n++] = '"';
            history_csv[n++] = *s;
        }
        history_csv[n++] = '"';
        history_csv[n++] = ',';
    }
    n += (size_t)snprintf(history_csv + n, sizeof(history_csv) - n,
        "1792220400.250123,\"vita\",\"PCSG00415\",\"vocr\",\"model\"\n");
    TEST_ASSERT(n < sizeof(history_csv));
}
static VjoGameOcrRequest fake_game_request;
static VjoGameOcrResult fake_game_result;
static int32_t fake_model_io_error;
static const char *fake_recognized_text;
static unsigned fake_lines;
static int32_t fake_free_user;
static uint32_t fake_need_weights, fake_reserve, fake_release;
static const char *fake_missing_model;
static int fake_memory_fallback, fake_missing_with_fallback, fake_cancel_on_model_error;
static int32_t fake_missing_io;
static unsigned submits;
static int fake_screen_changes; /* the next reads end with VJO_E_SCREEN_CHANGED */
static uint16_t fake_line_flags; /* VjoGameOcrLine.flags of the fake lines (0: 0x3FF, read greedily) */
static int fake_rows_vertical; /* API 47, ocr_vertical = auto: the worker found the rows to be vertical text */
static uint32_t fake_cascade_frames, fake_cascade_us; /* a CASCADE step with W20's frames and time (0: none) */
static int fake_steps; /* the steps of a resident job on main's defaults (game_ocr/main.c), as the worker reports */
/* A clock step (game_ocr/main.c job_busy): the ARM clock before, while and after, and the boost's rc. */
static int fake_clock_on, fake_clock_rc;
static uint32_t fake_clock_mhz[3];
/* API 21: tiers whose memory check the fake worker refuses (bit t - 1), in a
 * request by tiers: it logs a CHECK step per tier tried, largest first, as the
 * worker does, and runs the first one it does not refuse (all refused: OOM). */
static unsigned fake_tier_checks;
static unsigned fake_top(unsigned tiers)
{
    unsigned top = 0;
    for (unsigned t = 1; t <= VJO_VOCR_TIERS; t++) if (tiers & (1u << (t - 1u))) top = t;
    return top;
}
static void fake_check(unsigned tier, unsigned top, int refused)
{
    VjoGameOcrStep *s = &fake_game_result.step[fake_game_result.n_steps++];
    memset(s, 0, sizeof(*s));
    s->id = VJO_GSTEP_CHECK;
    s->kind = (uint8_t)(tier << 4);
    s->arg = tier != top;
    s->rc = refused;
    s->bytes = 1000000u * tier; /* a made-up need: workspace and weights */
    s->sum = 4096u * tier;      /* own resident blocks it would release first */
}
/* #138: what the kernel answers when no worker is registered for the game (none
 * was loaded at its start); 0: the fake worker takes the job. */
static int fake_submit_rc;
/* No log batch is held across the game's exchange (#100), except the normal build's network thread's
 * own: it stays open across its jobs' exchanges and reads, and its lines go out after their results are
 * taken (net_main). */
static int exchange_batch_ok(void)
{
#if VJO_DEBUG
    return !open_batch;
#else
    return !open_batch || (open_batch == &net_log && batch_owner == NET_THREAD);
#endif
}
int vjoOcrSubmit(const VjoGameOcrRequest *r)
{
    TEST_CHECK(exchange_batch_ok()); /* no log batch is held across the game's exchange */
    if (fake_submit_rc) { net_phase = NET_RESULT; return fake_submit_rc; } /* its refusal is its result */
    net_phase = NET_RECOGNIZING;
    fake_game_request = *r;
    TEST_CHECK(!vocr_live && !(r->flags & ~VJO_GAME_OCR_F_ALL));
    if (submits < 8) selftest_flags_seen[submits] = r->flags, selftest_speed_seen[submits] = r->speed;
    submits++;
    fake_game_request.seq = 12;
    fake_game_request.width = raw_state.width; fake_game_request.height = raw_state.height;
    /* The game reads its first pass: the kernel publishes the capture (API 19). */
    state.done_seq++;
    memset(&fake_game_result, 0, sizeof(fake_game_result));
    fake_game_result.size = sizeof(fake_game_result); fake_game_result.seq = 12;
    return 12;
}
/* fake_republish: the job's first two reads are still pending; the kernel
 * meanwhile voids the job's first capture (region_seq 0: the screen changed,
 * the worker waits and reads again) and then publishes the retry's (scene 3,
 * capture 5), as API 20's worker does. */
static int fake_republish, republish_reads, captured_while_void;
/* shell/log_vita.c's counter of file appends and their time: each read 3 ms and 2
 * appends on from the last, as if a batch went out to both files meanwhile. */
static unsigned log_io_calls;
uint32_t vjo_log_io(uint32_t *appends) { log_io_calls++; *appends = 2u * log_io_calls; return 3000u * log_io_calls; }
static uint32_t fake_exchange_us; /* the game worker's time for a job, as the Shell's read waits for it */
/* fake_cancel_on_read: the next job is cancelled while the game reads (a game exit, a new screen), its
 * result arriving after (the exchange then returns VJO_E_CANCELLED). Once. */
static int fake_cancel_on_read;
static unsigned fake_model_misses; /* the exchanges that found the model missing (a job then reads again) */
static int fake_ocr_read(uint32_t seq, VjoGameOcrResult *out);
/* API 31: the next results' text_from, hook_flags, hook_score, hook_streams, hook_mode, hook_installed and
 * hook_refused (zero: a game where the worker hooked nothing) */
static uint32_t fake_hook[7];
/* API 52 (#358): the next successful results' suggested band, its rows (0: none) */
static uint32_t fake_suggest_rows;
int vjoOcrRead(uint32_t seq, VjoGameOcrResult *out)
{
    int rc = fake_ocr_read(seq, out);
    if (rc == 0) net_phase = NET_RESULT; /* the result is the Shell's from here to job_done */
    return rc;
}
static int fake_ocr_read(uint32_t seq, VjoGameOcrResult *out)
{
    TEST_CHECK(seq == 12);
    test_now += fake_exchange_us;
    TEST_CHECK(exchange_batch_ok());
    if (fake_republish && republish_reads < 2) {
        if (republish_reads++ == 0) state.region_seq = 0;
        else { captured_while_void = job_captured; state.capture_scene = state.scene = state.region_scene = 3; state.region_seq = 5; }
        return 1;
    }
    vocr_allocs++;
    if (phases_checked) TEST_CHECK(!has_result_memory());
    if (fake_screen_changes > 0) {
        /* The real worker's result after VJO_VOCR_TRIES reads of a changing region. */
        fake_screen_changes--;
        fake_game_result.rc = VJO_E_SCREEN_CHANGED;
        fake_game_result.text[0] = 0;
        fake_game_result.tears = 3;
        vocr_frees++; /* the worker freed the job's workspace before answering */
        *out = fake_game_result;
        return 0;
    }
    /* The model the job runs (API 21): the request's largest tier less the tiers the
     * worker stepped down (its memory check), else the request's model by name. */
    unsigned top = fake_top(fake_game_request.tiers), ran = top;
    if (fake_tier_checks && top) {
        for (unsigned t = top; t >= 1u; t--) {
            int refused;
            if (!(fake_game_request.tiers & (1u << (t - 1u)))) continue;
            refused = (int)((fake_tier_checks >> (t - 1u)) & 1u);
            fake_check(t, top, refused);
            ran = t;
            if (!refused) break;
        }
        fake_game_result.fallback = top - ran;
        if ((fake_tier_checks >> (ran - 1u)) & 1u) { /* every tier refused: the worker's OOM */
            fake_game_result.rc = VJO_E_OOM;
            fake_game_result.free_user = fake_free_user; fake_game_result.reserve_bytes = 4194304;
            fake_game_result.need_workspace = 245760; fake_game_result.need_weights = 659456;
            *out = fake_game_result;
            return 0;
        }
    } else if (fake_memory_fallback && top > 1u) ran = top - 1u;
    const char *ran_model = top ? vjo_vocr_tier_model((int)ran) : fake_game_request.model;
    /* API 47: a job of columns runs the vertical model, whatever its tiers (game_ocr/main.c): ocr_vertical = on,
     * or auto where the rows looked like vertical text and the vertical model can be read (else its rows) */
    unsigned vmode = (fake_game_request.speed >> VJO_GAME_OCR_S_VERTICAL_SHIFT) & VJO_GAME_OCR_S_VERTICAL_MASK;
    int fake_columns = vmode == VJO_GAME_OCR_VERTICAL_ON ||
                       (vmode == VJO_GAME_OCR_VERTICAL_AUTO && fake_rows_vertical &&
                        !(fake_missing_model && !strcmp(fake_missing_model, VJO_VOCR_VERTICAL_NAME)));
    if (fake_columns) ran_model = VJO_VOCR_VERTICAL_NAME;
    if (fake_missing_model && !strcmp(ran_model, fake_missing_model)) {
        fake_model_misses++;
        fake_game_result.rc = VJO_E_OCR_MODEL; /* as the worker reports an absent file */
        fake_game_result.io_error = fake_missing_io;
        fake_game_result.fallback = top ? top - ran : (uint32_t)fake_missing_with_fallback; /* the tier that failed */
        if (fake_cancel_on_model_error) job_cancelled = 1;
        *out = fake_game_result;
        return 0;
    }
    if (fake_memory_fallback) {
        /* The real worker's report when H15 did not fit but FL10 did (FL10 resident). */
        fake_game_result.fallback = 1;
        fake_game_result.model_need_workspace = 303104; fake_game_result.model_need_weights = 1159168;
        fake_game_result.model_release_bytes = 659456;
    }
    if (fail_vocr_user) {
        /* The real worker reports its refused check (game_ocr/main.c). */
        fake_game_result.rc = VJO_E_OOM;
        fake_game_result.free_user = fake_free_user;
        fake_game_result.need_workspace = 245760; fake_game_result.need_weights = fake_need_weights;
        fake_game_result.reserve_bytes = fake_reserve;
        fake_game_result.release_bytes = fake_release;
        fake_game_result.workspace_bytes = 244896;
    }
    else {
        size_t bytes = vjo_vocr_workspace_bytes(ran_model, raw_state.width, raw_state.height);
        static unsigned char capture[64 * 1024];
        VjoPlatform game_platform = {0}; VjoVocrStats stats;
        VjoRgbaImage rgba = {raw_region, raw_state.width, raw_state.height, raw_state.width * 4};
        VjoGreySource im;
        vjo_grey_source_rgba(&rgba, &im);
        vocr_live = 1;
        fake_game_result.rc = vjo_vocr_ocr(&game_platform, fake_game_request.model_dir,
            ran_model, &im, vocr_storage, bytes, capture,
            vjo_vocr_capture_bytes(raw_state.width, raw_state.height),
            fake_game_result.text, sizeof(fake_game_result.text), &stats);
        vocr_live = 0; vocr_frees++;
    }
    if (fake_game_result.rc == VJO_E_OCR_MODEL) fake_game_result.io_error = fake_model_io_error;
    /* A supplied successful worker response tests Shell publication, not OCR accuracy. */
    if (fake_game_result.rc == VJO_OK && fake_recognized_text) {
        snprintf(fake_game_result.text, sizeof(fake_game_result.text), "%s", fake_recognized_text);
        /* as the worker returns it (API 20): normalised, its own text kept after the NUL */
        vjo_text_normalize_keep(fake_game_result.text, sizeof(fake_game_result.text), NULL, 0, NULL);
        /* A supplied measurement, as the real worker copies VjoVocrStats. */
        fake_game_result.free_user = fake_memory_fallback ? 5300000 : 91226112;
        fake_game_result.reserve_bytes = 4194304;
        fake_game_result.need_workspace = 245760; fake_game_result.need_weights = 0;
        fake_game_result.recognize_us = 92345; fake_game_result.frames = 205;
        fake_game_result.lines = fake_lines;
        fake_game_result.recognized = fake_lines;
        for (unsigned i = 0; i < fake_lines; i++)
            fake_game_result.line[i] = (VjoGameOcrLine){(uint16_t)(917 - i), 30, (uint16_t)(114 - i),
                                                        (uint16_t)((fake_line_flags ? fake_line_flags : 0x3FFu) |
                                                                   (fake_columns ? VJO_GAME_OCR_LINE_COLUMN : 0u)),
                                                        52100u + i};
        if (fake_steps) {
            static const uint8_t ids[] = {VJO_GSTEP_MEM, VJO_GSTEP_CHECK, VJO_GSTEP_ALLOC, VJO_GSTEP_ALLOC,
                                          VJO_GSTEP_PASS, VJO_GSTEP_SIG, VJO_GSTEP_FIND, VJO_GSTEP_SETTLE,
                                          VJO_GSTEP_LOAD, VJO_GSTEP_RECOGNIZE, VJO_GSTEP_LM, VJO_GSTEP_NORM,
                                          VJO_GSTEP_FREE, VJO_GSTEP_FREE};
            for (unsigned k = 0; k < sizeof(ids); k++) {
                VjoGameOcrStep *s = &fake_game_result.step[fake_game_result.n_steps++];
                memset(s, 0, sizeof(*s));
                s->id = ids[k];
                s->kind = ids[k] == VJO_GSTEP_ALLOC || ids[k] == VJO_GSTEP_FREE ? VJO_GAME_OCR_MEM_USER : 0;
                s->arg = ids[k] == VJO_GSTEP_ALLOC || ids[k] == VJO_GSTEP_FREE ? VJO_GITEM_CAPTURE + (k & 1) : 1;
                s->bytes = 57344; s->free_main = 90177536; s->free_cdram = 12582912; s->free_phycont = 27262976;
                s->us = 1000u * k + 17u;
            }
        }
        if (fake_clock_on) {
            VjoGameOcrStep *s = &fake_game_result.step[fake_game_result.n_steps++];
            memset(s, 0, sizeof(*s));
            s->id = VJO_GSTEP_CLOCK;
            s->arg = (uint16_t)fake_clock_mhz[0]; s->rc = fake_clock_rc;
            s->bytes = fake_clock_mhz[1]; s->sum = fake_clock_mhz[2];
        }
        if (fake_cascade_frames) {
            VjoGameOcrStep *s = &fake_game_result.step[fake_game_result.n_steps++];
            memset(s, 0, sizeof(*s));
            s->id = VJO_GSTEP_CASCADE;
            s->arg = 1;
            s->bytes = VJO_GLM_READ | 1u << 24 | fake_cascade_frames;
            s->us = fake_cascade_us + 1u;
        }
    }
    if (fake_cancel_on_read) { job_cancelled = 1; fake_cancel_on_read = 0; }
    /* API 31: the hook fields, as the worker fills them in every result */
    fake_game_result.text_from = fake_hook[0]; fake_game_result.hook_flags = fake_hook[1];
    fake_game_result.hook_score = fake_hook[2]; fake_game_result.hook_streams = fake_hook[3];
    fake_game_result.hook_mode = fake_hook[4]; fake_game_result.hook_installed = fake_hook[5];
    fake_game_result.hook_refused = fake_hook[6];
    fake_game_result.suggest_rows = fake_game_result.rc == VJO_OK ? fake_suggest_rows : 0; /* (a failure: none) */
    *out = fake_game_result;
    return 0;
}
int vjoOcrCancel(uint32_t seq) { return 0; }
#endif

static void reset(void)
{
    /* API 31: no game session's hook state, no hook fields in the results */
    hook_pid = 0; memset(hook_logged, 0, sizeof(hook_logged));
#ifdef VJO_WITH_VOCR
    memset(fake_hook, 0, sizeof(fake_hook));
    fake_suggest_rows = 0; /* #358: no box suggested */
#endif
    g_view.suggest = 0; /* #358: no game's box */
    saved_region_all_full = region_save_failure = 0;
    region_saves = 0;
    /* the logger stub: no batch open, no append recorded; the harness runs as the control thread */
    open_batch = NULL;
    batch_lines_held = batch_bytes_held = 0;
    configure_writes = net_log_locks = 0;
#if !VJO_DEBUG
    memset(&net_log, 0, sizeof(net_log)); /* closed, as at the worker's start */
    net_wait_timed = 0;
#else
    static NetHeld test_held; /* net_main's, on its stack */
    test_held.b.buf = NULL; /* closed, as net_main starts it */
    net_held = &test_held;
#endif
    file_len = 0;
    file_text[0] = 0;
    on_notify = NULL;
    appends_recorded = result_path_appends = recognizing_appends = lines_appended = most_lines_appended = 0;
    lines_logged = net_lines_logged = most_lines_held = 0;
    net_phase = NET_IDLE;
    test_thread = CTL_THREAD;
    on_event_flag = post_hook;
    fail_input_unblock = 0;
    net_stack_lo = net_stack_painted = net_stack_top = NULL;
    net_stack_logged = 0;
    thread_info_rc = 0;
    thread_info_size = (int)sizeof(fake_stack);
    thread_info_stack = NULL;
    thread_info_below_sp = 0;
    race_on_lock = 0;
    if (has_result_memory()) mem_free();
    if (history) { /* a test that ended on an earlier line: its block goes as the worker frees it */
        vjo_view_lock();
        HistoryVisit *v = history_detach();
        vjo_view_unlock();
        history_free(v);
    }
    history_step = 0;
    history_allocs = history_frees = 0;
    fail_history_alloc = 0;
#ifdef VJO_WITH_VOCR
    history_csv[0] = history_uuid[0] = 0;
    history_loads = 0;
    history_load_rc = 0;
    total_steps = total_keeps = total_windows = 0;
#endif
    memset(&g_view, 0, sizeof(g_view));
    view_lock = 81;
    cmd_lock = 82;
    held_cmd = held_view = retirement_checks = 0;
    local_text[0] = 0;
    local_text_valid = cursor_waiting = job_cursor_only = 0;
    job_prepare = 0;
    prepared_seq = prepare_cursor = 0;
    prepare_left = 0;
    shell_notifications = 0;
    local_text_generation = local_text_game_generation = 0;
    local_cursor_seq = local_cursor_byte = 0;
    job_cursor_byte = job_cursor_seq = job_text_generation = 0;
    local_page = job_page = pages_limit = 0;
    pages_generation = 0;
    pending_cmd = VJO_CMD_NONE;
    pending_cursor_byte = pending_cursor_generation = 0;
    memset(&state, 0, sizeof(state));
    memset(&plat, 0, sizeof(plat));
    dict_close_pending = 0;
    vjo_config_defaults(&loaded);
    loaded.dictionary = VJO_DICT_LOCAL;
    cfg = loaded;
    configure_log_file = cfg.log_file; /* the worker's start configured the logger, before any batch */
    memcpy(title_id, "PCSG00415", 10);
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    state.scene = state.capture_scene = state.region_scene = 2;
    state.region_seq = 4;
    state.stable = 1;
    test_now = 10000000;
    posted = user_allocs = user_frees = heap_allocs = heap_frees = 0;
    fail_alloc = fail_base = input_blocked = fail_input_block = input_block_error = 0;
    game_enabled = 1;
    running = 1; threads_started = 0;
    vocr_allocs = vocr_frees = vocr_paf_allocs = 0;
    fail_vocr_user = fail_vocr_paf = 0;
#ifdef VJO_WITH_VOCR
    history_calls = 0;
    history_rtc_fail = history_write_fail = 0;
    history_on_append = NULL;
    memset(&last_history, 0, sizeof(last_history));
    history_text[0] = '\0';
    history_text_from = 0;
    anki_game[0] = anki_tid[0] = '\0';
    anki_psp = 0;
    fake_model_io_error = 0;
    fake_recognized_text = NULL;
    fake_lines = 2;
    fake_line_flags = 0;
    fake_rows_vertical = 0;
    fake_steps = 0;
    fake_clock_on = fake_clock_rc = 0;
    fake_cascade_frames = fake_cascade_us = 0;
    fake_free_user = 5000000;
    fake_need_weights = 659456;
    fake_reserve = 4194304;
    fake_release = 0;
    fake_missing_model = NULL;
    fake_memory_fallback = fake_missing_with_fallback = fake_cancel_on_model_error = 0;
    fake_missing_io = (int32_t)0x80010002u;
    vocr_unusable = 0; vocr_unusable_generation = 0;
    fake_tier_checks = 0;
    memset(job_title, 0, sizeof(job_title));
    log_count = 0;
    fake_screen_changes = 0;
    fake_cancel_on_read = 0;
#endif
    selftest_waits = 0;
    job_refresh = 0;
#if VJO_DEBUG
    job_selftest = 0;
#endif
#ifdef VJO_WITH_VOCR
    fake_exchange_us = 0;
#endif
    job_prepares = 1; /* run_vocr_job: an Auto job, the overlay closed */
    text_generation = 0;
    idle_since_us = 0;
    job_started_us = 0;
    refresh_pending = open_retry = 0;
    refresh_after_us = lookup_us = 0;
    screen_changed_streak = 0;
    screen_retry_until = 0;
    phases_checked = vocr_live = fail_join = 0;
    anki_configured = anki_reaps = anki_stop_rc = anki_counts = anki_on = 0;
    memset(&raw_state, 0, sizeof(raw_state));
    raw_state.done_seq = 11;
    raw_state.width = 200;
    raw_state.height = 60;
    raw_state.capture_scene = 2;
    raw_state.region_seq = 4;
    memset(raw_region, 0, sizeof(raw_region));
    ov = OV_CLOSED;
    active = -1;
    game_generation = job_generation = 0;
#if !VJO_DEBUG
    short_generation = 0xFFFFFFFFu; /* the normal build's lines per activation: none yet (tests start at 0) */
    short_logged = 0; short_tiers = 0;
#endif
    cache_ok = subtitles = stable_pending = job_running = job_cancelled = 0;
    strip_waiting = 0;
    between_answer_and_publish = NULL;
    view_waiting = view_job_text = 0;
    memset(&ocr_backoff, 0, sizeof(ocr_backoff));
    memset(&dict_backoff, 0, sizeof(dict_backoff));
    memset(&refresh_backoff, 0, sizeof(refresh_backoff));
    memset(cache_data, 0, sizeof(cache_data));
    app_state = -1;
    app_installed_rc = app_record_rc = (int)0x80010002u; /* neither stamp: a package from before them */
    app_eboot = 0;
    app_installed[0] = app_record[0] = 0;
    app_logs = config_warning_logs = 0;
    last_app_log[0] = 0;
}

static void test_local_open(void)
{
    reset();
    open_overlay();
    TEST_CHECK(job_running && job_lookup && input_blocked);
    TEST_CHECK(posted & NET_EV_JOB);
    TEST_CHECK(!g_view.status_is_error && want_lookup());
    /* Opening takes no memory: the job takes its one lookup arena after OCR
     * (in the network thread). */
    TEST_CHECK(!has_result_memory() && !user_allocs && !heap_allocs);
    TEST_ASSERT(mem_alloc() == 0); /* the job's lookup arena */
    TEST_CHECK(results[0].size + vjo_local_session_size() == LOCAL_RESULT_ARENA_SIZE && !results[1].size);
    job_running = 0;
    mem_free();
    TEST_CHECK(!has_result_memory());
#ifdef VJO_PAF_ALLOC
    TEST_CHECK(heap_allocs == 1 && heap_frees == 1 && user_allocs == 0);
#else
    TEST_CHECK(user_allocs == 1 && user_frees == 1);
#endif
}

static void test_config_cache(void)
{
    reset();
    cache_ok = 1;
    apply_config();
    TEST_CHECK(cache_ok);
    strcpy(loaded.local_dictionaries, "daijirin.vjdict,main.vjdict");
    apply_config();
    TEST_CHECK(!cache_ok);
    cache_ok = 1;
    strcpy(loaded.local_dictionary_dir, "ux0:data/other");
    apply_config();
    TEST_CHECK(!cache_ok);
    /* the removed Hachidori relay's choice loads as local: the same pipeline, the cache kept */
    {
        const char *old = "dictionary = hachidori\nhachidori_host = 192.168.1.2\n"
                          "local_dictionary_dir = ux0:data/other\n"
                          "local_dictionaries = daijirin.vjdict,main.vjdict\n";
        VjoConfig c;
        vjo_config_defaults(&c);
        vjo_config_parse(&c, old, strlen(old));
        TEST_CHECK(c.dictionary == VJO_DICT_LOCAL && c.n_warnings == 1);
        cache_ok = 1;
        loaded = c;
        apply_config();
        TEST_CHECK(cache_ok && want_lookup());
    }
}

static void test_cached_scene(void)
{
    reset();
    TEST_ASSERT(mem_alloc() == 0);
    active = 0;
    cache_ok = 1;
    cache_scene = state.scene;
    cache_seq = state.region_seq;
    cache_data[0].list.header = "日本語";
    open_overlay();
    TEST_CHECK(!job_running && g_view.list == &cache_data[0].list);
    TEST_CHECK(input_blocked && !g_view.status_is_error);
    close_overlay();
    TEST_CHECK(!g_view.list && !input_blocked);
    mem_free();
}

static void test_stale_cancel(void)
{
    reset();
    job_running = job_captured = 1;
    job_capture_scene = 1;
    job_capture_seq = 1;
    TEST_CHECK(cancel_outdated_job(&state));
    TEST_CHECK(job_cancelled);
    TEST_CHECK(!cancel_outdated_job(&state)); /* once: the job is already cancelled */
    job_running = 0;
}

static void test_background_local(void)
{
    reset();
    TEST_ASSERT(mem_alloc() == 0);
    stable_pending = 1;
    auto_prefetch();
    TEST_CHECK(job_running && job_lookup && (posted & NET_EV_JOB));
    job_running = 0;
    mem_free();
}

/* The removed Hachidori relay: a config.ini that chose it, with any hachidori_host (none, not a
 * host, a valid one), loads as local, and the overlay opens as with the local dictionaries: no
 * relay host message (the relay without a valid host once showed one, and no job started;
 * #117's audit N6, M27), a job starts. */
static void test_removed_relay_opens_as_local(void)
{
    static const char *const hosts[] = {"", "http://192.168.1.2", "192.168.1.2:19633"};
    for (unsigned i = 0; i < sizeof(hosts) / sizeof(hosts[0]); i++) {
        char ini[128];
        reset();
        snprintf(ini, sizeof(ini), "dictionary = hachidori\nhachidori_host = %s\n", hosts[i]);
        vjo_config_defaults(&loaded);
        vjo_config_parse(&loaded, ini, strlen(ini));
        TEST_CHECK(loaded.dictionary == VJO_DICT_LOCAL && loaded.n_warnings == 1);
        cfg = loaded;
        open_overlay();
        TEST_CHECK_(g_view.open && !g_view.status_is_error && !strstr(g_view.status, "achidori"),
                    "host '%s': %s", hosts[i], g_view.status);
        TEST_CHECK(job_running && job_lookup && (posted & NET_EV_JOB) && want_lookup());
        job_running = 0;
        close_overlay();
        mem_free();
    }
}

/* The lookup's result arena (taken after OCR): a refusal leaves nothing held
 * and the next allocation succeeds; a block whose base cannot be read is freed
 * again (the host build's memory-block path). A job's refused arena keeps its
 * recognized text: test_shell_result_oom_preserves_recognition. */
static void test_alloc_failure(void)
{
    reset();
    fail_alloc = 1;
    TEST_CHECK(mem_alloc() < 0 && !has_result_memory());
    fail_alloc = 0;
    TEST_CHECK(mem_alloc() == 0 && has_result_memory() && results[0].size + vjo_local_session_size() == LOCAL_RESULT_ARENA_SIZE);
    mem_free();
#ifndef VJO_PAF_ALLOC
    fail_base = 1;
    TEST_CHECK(mem_alloc() < 0 && !has_result_memory());
    TEST_CHECK(user_frees == 2);
#endif
}

static void test_ocr_config_migration(void)
{
    VjoConfig c;
    const char old[] = "text_source = hooks\nocr_backend = meiki\n";
    vjo_config_defaults(&c);
    TEST_CHECK(c.text_source == VJO_SOURCE_AUTO); /* API 31: no line, automatic */
    vjo_config_parse(&c, old, sizeof(old)-1);
    /* API 31: hooks is a text source again, without a warning; the removed engine falls back to VOCR with one. */
    TEST_CHECK(c.text_source == VJO_SOURCE_HOOKS && c.dictionary == VJO_DICT_LOCAL);
    TEST_CHECK(c.n_warnings == 1);
    /* the three values in any case, the kernel's numbers; anything else is automatic, with a warning */
    static const struct { const char *text; int source, warnings; } cases[] = {
        {"text_source = auto\n", VJO_SOURCE_AUTO, 0}, {"text_source = OCR\n", VJO_SOURCE_OCR, 0},
        {"text_source = Hooks\n", VJO_SOURCE_HOOKS, 0}, {"text_source = ocr\ntext_source = native\n", VJO_SOURCE_AUTO, 1},
        {"text_source =\n", VJO_SOURCE_AUTO, 1}, {"text_source = hooks ; every game\n", VJO_SOURCE_HOOKS, 0}};
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        vjo_config_defaults(&c);
        vjo_config_parse(&c, cases[i].text, strlen(cases[i].text));
        TEST_CHECK_(c.text_source == cases[i].source && c.n_warnings == cases[i].warnings, "%s", cases[i].text);
    }
    TEST_CHECK(VJO_SOURCE_OCR == (int)VJO_TEXT_SOURCE_OCR && VJO_SOURCE_AUTO == (int)VJO_TEXT_SOURCE_AUTO &&
               VJO_SOURCE_HOOKS == (int)VJO_TEXT_SOURCE_HOOKS);
}

/* audit-hooks F3: config.ini's hook_sites reaches the kernel in the text source's word, at each config read
 * (the same call and moments as text_source): off adds VJO_TEXT_SOURCE_NO_SITES to the mode, on (the
 * default) sends the mode alone. */
static void test_hook_sites_word(void)
{
    static const struct { int source, sites; uint32_t word; } cases[] = {
        {VJO_SOURCE_AUTO, 1, VJO_TEXT_SOURCE_AUTO},
        {VJO_SOURCE_AUTO, 0, VJO_TEXT_SOURCE_AUTO | VJO_TEXT_SOURCE_NO_SITES},
        {VJO_SOURCE_HOOKS, 0, VJO_TEXT_SOURCE_HOOKS | VJO_TEXT_SOURCE_NO_SITES},
        {VJO_SOURCE_HOOKS, 1, VJO_TEXT_SOURCE_HOOKS},
        {VJO_SOURCE_OCR, 0, VJO_TEXT_SOURCE_OCR | VJO_TEXT_SOURCE_NO_SITES},
        {VJO_SOURCE_OCR, 1, VJO_TEXT_SOURCE_OCR},
    };
    reset();
    TEST_CHECK(loaded.hook_sites == 1); /* the default: on */
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        unsigned pushes = text_source_pushes;
        loaded.text_source = cases[i].source;
        loaded.hook_sites = cases[i].sites;
        apply_config();
        TEST_CHECK_(text_source_pushes == pushes + 1 && text_source_pushed == cases[i].word, "case %u: 0x%X", i,
                    (unsigned)text_source_pushed);
    }
    /* a game's exit reads the config and pushes the word again (its worker reads it at the next start) */
    loaded.text_source = VJO_SOURCE_AUTO;
    loaded.hook_sites = 0;
    unsigned pushes = text_source_pushes;
    state.game_pid = 0;
    control_tick(VJO_EV_GAME_EXIT);
    TEST_CHECK(text_source_pushes == pushes + 1 &&
               text_source_pushed == (VJO_TEXT_SOURCE_AUTO | VJO_TEXT_SOURCE_NO_SITES));
    mem_free();
}

static void test_changed_dictionary_drops_running_result(void)
{
    reset();
    TEST_ASSERT(mem_alloc() == 0);
    active = 0;
    cache_ok = 1;
    cache_data[0].filtered = "日本語";
    start_job("test", 1);
    ov = OV_OPEN_OLD_JOB;
    strcpy(loaded.local_dictionaries, "new.vjdict");
    apply_config();
    TEST_CHECK(job_cancelled && !cache_ok);
    on_job_done();
    TEST_CHECK(job_running && !cache_ok);
    TEST_CHECK(strcmp(job_cfg.local_dictionaries, "new.vjdict") == 0);
    TEST_CHECK(g_view.list != &cache_data[0].list);
    job_running = 0;
    mem_free();
}

static void test_game_exit_drains_previous_job(void)
{
    reset();
    TEST_ASSERT(mem_alloc() == 0);
    active = 0;
    cache_ok = 1;
    cache_data[0].filtered = "日本語";
    cache_scene = state.scene;
    cache_seq = state.region_seq;
    start_job("old game", 1);
    TEST_ASSERT(mem_alloc() == 0); /* the job's lookup arena, taken after its OCR */
    ov = OV_OPEN;
    input_blocked = 1;

    on_game_exit();
    TEST_CHECK(job_cancelled && !cache_ok);
    TEST_CHECK(!game_active() && ov == OV_CLOSED && !input_blocked);
    TEST_CHECK(has_result_memory()); /* the running job still owns its arena */
    TEST_CHECK(job_generation != game_generation);
    on_job_done();
    TEST_CHECK(!job_running && !cache_ok && !stable_pending);
    TEST_CHECK(!g_view.list);
    mem_free();
    TEST_CHECK(!has_result_memory());
}

static void test_game_start_rejects_previous_generation(void)
{
    reset();
    TEST_ASSERT(mem_alloc() == 0);
    active = 0;
    cache_ok = 1;
    cache_data[0].filtered = "日本語";
    cache_scene = state.scene;
    cache_seq = state.region_seq;
    start_job("old game", 1);
    cache_data[job_idx].sentence = "old game text";
    job_capture_scene = state.scene;
    job_capture_seq = state.region_seq;
    ov = OV_OPEN;
    input_blocked = 1;
    state.game_pid = 101;

    on_game_start(); /* a foreground switch need not include GAME_EXIT */
    TEST_CHECK(job_cancelled && !cache_ok);
    TEST_CHECK(ov == OV_CLOSED && !input_blocked && pending_pid == 101);
    activate(state.game_pid, "PCSG00202", VJO_GAME);
    open_overlay(); /* the kernel has not sampled a new scene ID yet */
    TEST_CHECK(ov == OV_OPEN_OLD_JOB && !g_view.list && !cache_ok);

    /* The generation check is independent of the socket cancellation flag. */
    job_cancelled = 0;
    job_text_ready = 1;
    on_job_text();
    TEST_CHECK(!g_view.list && !view_job_text);
    on_job_done();
    TEST_CHECK(job_running && job_generation == game_generation);
    TEST_CHECK(!cache_ok);
    TEST_CHECK(!g_view.list);
    job_running = 0;
    mem_free();
}

/* ---- #104: PS with the overlay open ---- */

#define PS_GAME_PID 0x62C03
/* The overlay open over the game in front, from a cached result (no job). */
static void open_over_the_game(void)
{
    if (!has_result_memory()) TEST_ASSERT(mem_alloc() == 0);
    state.game_pid = PS_GAME_PID;
    active = 0;
    cache_ok = 1;
    cache_scene = state.scene;
    cache_seq = state.region_seq;
    cache_data[0].list.header = "日本語";
    control_tick(VJO_EV_TRIGGER); /* L + R */
    TEST_ASSERT(ov == OV_OPEN && input_blocked && g_view.open && !job_running);
}

/* open -> PS -> released: the kernel's GAME_EXIT with no process in front
 * (SceShell's LiveArea is) closes the overlay in the same wakeup, releases the
 * game's input, and tells the page to leave SceShell's touch region alone; a
 * stale L + R does not open it over the LiveArea. resume -> closed: the game
 * comes back with the overlay closed and its input free, and only the next
 * L + R opens it, which Cancel closes in game, giving the region back. */
static void test_ps_with_the_overlay_open_releases_input_and_leaves_touch_to_shell(void)
{
    reset();
    open_over_the_game();
    state.game_pid = 0;                              /* PS: the game suspended, SceShell in front */
    control_tick(VJO_EV_GAME_EXIT | VJO_EV_TRIGGER); /* L + R a moment before PS: one wakeup */
    TEST_CHECK(ov == OV_CLOSED && !input_blocked && !g_view.open && !game_active());
    TEST_CHECK(g_view.touch_handback == VJO_TOUCH_LEAVE);
    TEST_CHECK(DLOGGED("overlay close (game left) t=13000 ms: input block off rc=0"));
    control_tick(VJO_EV_TRIGGER); /* over the LiveArea */
    TEST_CHECK(ov == OV_CLOSED && !g_view.open && !input_blocked);

    test_now += 2000000;
    state.game_pid = PS_GAME_PID; /* resume */
    control_tick(VJO_EV_GAME_START);
    TEST_CHECK(ov == OV_CLOSED && !g_view.open && !input_blocked && pending_pid == PS_GAME_PID);
    pending_pid = 0;
    activate(PS_GAME_PID, "PCSG00415", VJO_GAME); /* classify_pending's verdict */
    control_tick(0);
    TEST_CHECK(ov == OV_CLOSED && !g_view.open && !input_blocked && game_active());

    open_over_the_game();
    TEST_CHECK(DLOGGED("input block on rc=0 (00000000) t=15000 ms"));
    vjo_post_command(VJO_CMD_CLOSED, NULL); /* Cancel */
    control_tick(0);
    TEST_CHECK(ov == OV_CLOSED && !input_blocked && !g_view.open);
    TEST_CHECK(g_view.touch_handback == VJO_TOUCH_RELEASE);
    TEST_CHECK(DLOGGED("overlay close (in game) t=15000 ms: input block off rc=0"));
    mem_free();
}

/* Any other way out of the game's foreground keeps the in-game hand-back
 * (deactivate): a process started over the game (the game stays in front),
 * or the kernel going back to a game behind the one that left. */
static void test_foreground_switch_gives_the_touch_region_back(void)
{
    reset();
    open_over_the_game();
    state.game_pid = 0x1137D9; /* a system app starts over the game */
    control_tick(VJO_EV_GAME_START);
    TEST_CHECK(ov == OV_CLOSED && !input_blocked && !g_view.open);
    TEST_CHECK(g_view.touch_handback == VJO_TOUCH_RELEASE);
    TEST_CHECK(DLOGGED("overlay close (foreground changed) t=13000 ms"));

    reset();
    open_over_the_game();
    state.game_pid = 0x7000; /* the game left, the kernel is back at the game behind it */
    control_tick(VJO_EV_GAME_EXIT);
    TEST_CHECK(ov == OV_CLOSED && !input_blocked && g_view.touch_handback == VJO_TOUCH_RELEASE);
    mem_free();
}

/* Cancel and PS together: the in-game close a moment before the game left is
 * turned into "leave it" while the page may still be open; a later close sets
 * its own hand-back again. */
static void test_close_just_before_ps_leaves_touch_to_shell(void)
{
    reset();
    open_over_the_game();
    vjo_post_command(VJO_CMD_CLOSED, NULL);
    control_tick(0);
    TEST_CHECK(ov == OV_CLOSED && g_view.touch_handback == VJO_TOUCH_RELEASE);
    state.game_pid = 0;
    control_tick(VJO_EV_GAME_EXIT);
    TEST_CHECK(ov == OV_CLOSED && !input_blocked && g_view.touch_handback == VJO_TOUCH_LEAVE);
    activate(PS_GAME_PID, "PCSG00415", VJO_GAME);
    open_over_the_game();
    control_tick(VJO_EV_TRIGGER); /* L + R closes it in game */
    TEST_CHECK(ov == OV_CLOSED && g_view.touch_handback == VJO_TOUCH_RELEASE);

    /* Only the game's exit turns a close into "leave": Cancel, then a process
     * started over the game, keeps it (audit-overlay-ps-touch A1, N2). */
    reset();
    open_over_the_game();
    vjo_post_command(VJO_CMD_CLOSED, NULL);
    control_tick(0);
    state.game_pid = 0x1137D9;
    control_tick(VJO_EV_GAME_START);
    TEST_CHECK_(g_view.touch_handback == VJO_TOUCH_RELEASE, "handback %d", g_view.touch_handback);
    TEST_CHECK(!input_blocked && !g_view.open);
    mem_free();
}

/* ---- OCR: the game worker's vita-vn-ocr ---- */

static int run_vocr_job(void)
{
    int rc;
    raw_state.done_seq = 11;
    TEST_ASSERT(mem_alloc() == 0);
    job_cfg = cfg;
    job_cancelled = 0;
    job_captured = 0;
    vjo_arena_reset(&results[0]);
    test_thread = NET_THREAD; /* as net_main runs it */
    net_phase = NET_IDLE;
#if !VJO_DEBUG
    net_job_begin();
#endif
    rc = run_job_t(&results[0], &cache_data[0]);
    net_phase = NET_IDLE;
#if !VJO_DEBUG
    net_job_end();
    net_idle_write(); /* its result is the test's now: what is held goes out (net_main's timed wait) */
#endif
    test_thread = CTL_THREAD;
    return rc;
}

#ifdef VJO_WITH_VOCR
/* A job asks by tiers (API 21): the configured size and every smaller one, the
 * largest's model naming it; the tier it reports is the one the worker ran, whose
 * model name (the self-test's stage record and the Anki cards read it after the
 * job) is a constant, never the request's copy, a local of the call. */
/* Normal reads always let the game select the source. Source metadata stays in diagnostic logs,
 * including across resume and a new game session; self-test jobs still force OCR. */
static void finish_current_job(void);
static void test_native_hooks_automatic_text_source_and_diagnostics(void)
{
    VjoGameOcrResult result;
    unsigned ran = 0;
    reset();
    fake_recognized_text = "猫";
    text_source_pushes = 0;
    activate(PS_GAME_PID, "PCSG00415", VJO_GAME);
    TEST_CHECK(text_source_pushed == VJO_TEXT_SOURCE_AUTO && text_source_pushes == 1);
    open_over_the_game();
    job_cfg = cfg; job_generation = game_generation;
    snprintf(job_title, sizeof(job_title), "%s", title_id);
    (void)vocr_run_one(&result, 0, vocr_tiers(), &ran, -1);
    TEST_CHECK(fake_game_request.hook_pick == VJO_HOOK_PICK_AUTO);
    TEST_CHECK(DLOGGED("hooks in PCSG00415: text source ocr, 0 table sites and 0 string copies hooked, 0 sites refused"));

    const uint32_t streams = VJO_HOOK_STREAM_BIT(1) | VJO_HOOK_STREAM_BIT(9) | VJO_HOOK_STREAM_BIT(12);
    const uint32_t hooked[7] = {9, VJO_HOOK_R_AGREED, 820, streams, VJO_TEXT_SOURCE_AUTO, 1u << 16 | 8u, 2};
    memcpy(fake_hook, hooked, sizeof(fake_hook));
    (void)vocr_run_one(&result, 0, vocr_tiers(), &ran, -1);
    TEST_CHECK(fake_game_request.hook_pick == VJO_HOOK_PICK_AUTO && result.text_from == 9);
    TEST_CHECK(logged("hooks in PCSG00415: text source auto, 1 table sites and 8 string copies hooked, 2 sites refused"));
    TEST_CHECK(DLOGGED("game vocr source: hook 9, flags 0x01, score 820, streams 0x00901, pick 0"));
    unsigned lines = log_count;
    (void)vocr_run_one(&result, 0, vocr_tiers(), &ran, -1);
    unsigned again = 0;
    TEST_ASSERT(log_count - lines <= 32u);
    for (unsigned i = lines; i < log_count; i++) again += strstr(log_lines[i % 32], "hooks in ") != NULL;
    TEST_CHECK(again == 0);

    /* A locked source can answer without recognition; the next request remains automatic. */
    fake_hook[1] = VJO_HOOK_R_LOCKED | VJO_HOOK_R_NO_OCR;
    (void)vocr_run_one(&result, 0, vocr_tiers(), &ran, -1);
    TEST_CHECK(fake_game_request.hook_pick == VJO_HOOK_PICK_AUTO && result.text_from == 9);
    TEST_CHECK(DLOGGED("game vocr source: hook 9, flags 0x0A, score 820, streams 0x00901, pick 0"));
    (void)vocr_run_one(&result, 0, vocr_tiers(), &ran, 0);
    TEST_CHECK(fake_game_request.hook_pick == VJO_HOOK_PICK_OCR);
    (void)vocr_run_one(&result, VJO_GAME_OCR_F_FULL, vocr_tiers(), &ran, -1);
    TEST_CHECK(fake_game_request.hook_pick == VJO_HOOK_PICK_OCR);

    activate(PS_GAME_PID, "PCSG00415", VJO_GAME);
    unsigned pushes = text_source_pushes;
    state.game_pid = 0;
    control_tick(VJO_EV_GAME_EXIT);
    TEST_CHECK(text_source_pushes == pushes + 1 && text_source_pushed == VJO_TEXT_SOURCE_AUTO);
    activate(PS_GAME_PID + 4, "PCSG00415", VJO_GAME);
    /* hook_sites = off (audit-hooks F3): the worker applied the word with NO_SITES, so no table site; the
     * line says so in both builds, for each mode */
    job_cfg = cfg; job_generation = game_generation;
    snprintf(job_title, sizeof(job_title), "%s", title_id);
    const uint32_t calls_only[7] = {9, VJO_HOOK_R_AGREED, 820, VJO_HOOK_STREAM_BIT(9),
                                    VJO_TEXT_SOURCE_AUTO | VJO_TEXT_SOURCE_NO_SITES, 0u << 16 | 8u, 0};
    memcpy(fake_hook, calls_only, sizeof(fake_hook));
    (void)vocr_run_one(&result, 0, vocr_tiers(), &ran, -1);
    TEST_CHECK(logged("hooks in PCSG00415: text source auto, sites off, 0 table sites and 8 string copies hooked, 0 sites refused"));
    fake_hook[4] = VJO_TEXT_SOURCE_HOOKS | VJO_TEXT_SOURCE_NO_SITES;
    (void)vocr_run_one(&result, 0, vocr_tiers(), &ran, -1);
    TEST_CHECK(logged("hooks in PCSG00415: text source hooks, sites off, 0 table sites and 8 string copies hooked, 0 sites refused"));
    memset(fake_hook, 0, sizeof(fake_hook));
    mem_free();
}

static void test_vocr_ran_names_outlive_the_job(void)
{
    VjoGameOcrResult result;
    unsigned ran = 0;
    reset();
    fake_recognized_text = "猫";
    TEST_ASSERT(mem_alloc() == 0);
    job_cfg = cfg;
    fake_memory_fallback = 1; /* the game ran the next smaller tier */
    (void)vocr_run_one(&result, 0, vocr_tiers(), &ran, -1);
    TEST_CHECK(fake_game_request.tiers == 3u && !strcmp(fake_game_request.model, VJO_VOCR_TIER2_NAME) &&
               !fake_game_request.fallback[0]);
    TEST_CHECK(result.fallback && ran == VJO_VOCR_TIER_SMALL && !strcmp(vjo_vocr_tier_model((int)ran), VJO_VOCR_TIER1_NAME));
    fake_memory_fallback = 0;
    (void)vocr_run_one(&result, 0, vocr_tiers(), &ran, -1);
    TEST_CHECK(!result.fallback && ran == VJO_VOCR_TIER_NORMAL);
    /* each size asks for itself and every smaller one */
    static const unsigned want[] = {0, 1u, 3u, 7u};
    static const char *const model[] = {"", VJO_VOCR_TIER1_NAME, VJO_VOCR_TIER2_NAME, VJO_VOCR_TIER3_NAME};
    for (int t = 1; t <= 3; t++) {
        job_cfg.vocr_tier = t;
        (void)vocr_run_one(&result, 0, vocr_tiers(), &ran, -1);
        TEST_CHECK_(fake_game_request.tiers == want[t] && !strcmp(fake_game_request.model, model[t]) && ran == (unsigned)t,
                    "tier %d: tiers 0x%X model %s ran %u", t, fake_game_request.tiers, fake_game_request.model, ran);
    }
    mem_free();
}

/* The job's capture is its latest one: voided while the worker waits for a
 * changed screen to hold still, then the retry's. A settled screen cancels no
 * job whose retry already shows it. */
static void test_vocr_capture_follows_the_retry(void)
{
    reset();
    fake_republish = 1; republish_reads = captured_while_void = 0;
    TEST_CHECK(run_vocr_job() == VJO_OK);
    TEST_CHECK(republish_reads == 2 && !captured_while_void);
    TEST_CHECK(job_captured && job_capture_scene == 3 && job_capture_seq == 5);
    TEST_CHECK(shows_screen(job_capture_scene, job_capture_seq, &state));
    fake_republish = 0;
    mem_free();
}
#endif

#ifdef VJO_WITH_VOCR
/* The recognizer's text is logged as it read it, a line per line; the sentence
 * shown, saved and looked up is normalised (one form of each mark, ー or 一 by
 * context), and what changed is logged. */
static void test_vocr_text_is_logged_raw_and_shown_normalised(void)
{
    reset();
    fake_recognized_text = "コ一ヒ一！？\n統ー的．．．";
    TEST_CHECK(run_vocr_job() == VJO_OK);
    TEST_CHECK(!strcmp(cache_data[0].sentence, "コーヒー!?\n統一的…"));
    TEST_MSG("shown: %s", cache_data[0].sentence);
#if VJO_DEBUG /* the text lines: the debug build's */
    TEST_CHECK(logged("game vocr text 0: コ一ヒ一！？") != NULL && logged("game vocr text 1: 統ー的．．．") != NULL);
    TEST_CHECK(logged("game vocr text 0 shown: コーヒー!?") != NULL && logged("game vocr text 1 shown: 統一的…") != NULL);
#else
    TEST_CHECK(!logged("game vocr text") && !logged("コ")); /* the normal build logs no recognized text */
#endif
    fake_recognized_text = "猫";
    log_count = 0;
    TEST_CHECK(run_vocr_job() == VJO_OK && !strcmp(cache_data[0].sentence, "猫"));
#if VJO_DEBUG
    TEST_CHECK(logged("game vocr text 0: 猫") != NULL && !logged("shown:")); /* unchanged: one line */
#endif
    mem_free();
}
#endif

/* Every job is the game worker's: on the console, no image or connection leaves it. */
static void test_vocr_empty_region(void)
{
    reset();
    int rc = run_vocr_job();
#ifdef VJO_WITH_VOCR
    /* An empty region: no line, no model read, the workspace is released. */
    TEST_CHECK(rc == VJO_OK && cache_data[0].failed_stage == VJO_STAGE_NONE);
    TEST_CHECK(cache_data[0].ocr_text && !cache_data[0].ocr_text[0]);
    TEST_CHECK(job_captured && job_capture_scene == 2 && job_capture_seq == 4);
    TEST_CHECK(vocr_allocs == 1 && vocr_frees == 1 && vocr_paf_allocs == 0);
#else
    TEST_CHECK(rc == VJO_E_OCR_UNAVAILABLE && cache_data[0].failed_stage == VJO_STAGE_OCR);
    TEST_CHECK(cache_data[0].err.detail && strstr(cache_data[0].err.detail, "OCR is not running in this game"));
    TEST_CHECK(vocr_allocs == 0);
#endif
    mem_free();
}

/* #138: a game with no worker (it was turned ON while it ran, L was held as it
 * started, or its load failed) shows one plain sentence, not an install step:
 * the kernel loads the worker at the game's next start. */
static void test_no_worker_says_restart_the_game(void)
{
#ifdef VJO_WITH_VOCR
    reset();
    fake_submit_rc = VJO_ERR_NO_GAME;
    int rc = run_vocr_job();
    fake_submit_rc = 0;
    TEST_CHECK(rc == VJO_E_OCR_UNAVAILABLE && cache_data[0].failed_stage == VJO_STAGE_OCR);
    TEST_CHECK(!job_captured && !vocr_allocs);
    const char *text = vjo_err_text(&scratch, cache_data[0].failed_stage, &cache_data[0].err);
    TEST_CHECK_(text && !strcmp(text, "OCR is not running in this game. Close the game and start it again."), "%s",
                text ? text : "(none)");
    TEST_CHECK(text && !strstr(text, "--title") && !strstr(text, "register"));
    mem_free();
#endif
}

/* A config.ini that still chooses a removed engine (Google Lens, the VOCR2
 * trial, ncnn) runs the local engine: the configured size with the smaller
 * ones, in the game, with no screenshot and no connection. */
static void test_removed_engine_choice_runs_the_local_engine(void)
{
    static const char *const ini[] = {"ocr_backend = lens\n", "ocr_backend = vocr2\n", "ocr_backend = ncnn\n"};
    static const char *const warning[] = {
        "config warning: ocr_backend: Google Lens was removed (the overlay is offline); using vocr",
        "config warning: ocr_backend: VOCR2 was removed; using vocr",
        "config warning: ocr_backend: ncnn runs only in host tools; using vocr"};
    for (unsigned i = 0; i < 3; i++) {
        reset();
        vjo_config_defaults(&loaded);
        vjo_config_parse(&loaded, ini[i], strlen(ini[i]));
        apply_config();
        TEST_CHECK(cfg.n_warnings == 1);
        TEST_CHECK(cfg.dictionary == VJO_DICT_LOCAL && background_wanted());
        TEST_CHECK_(logged(warning[i]) != NULL, "%s", warning[i]);
        int rc = run_vocr_job();
#ifdef VJO_WITH_VOCR
        TEST_CHECK(rc == VJO_OK && job_captured && vocr_allocs == 1 && vocr_frees == 1);
        TEST_CHECK(!strcmp(fake_game_request.model, VJO_VOCR_TIER2_NAME));
        TEST_CHECK(fake_game_request.tiers == 3u && !fake_game_request.fallback[0]); /* Normal, then Small */
        TEST_CHECK(cache_data[0].failed_stage == VJO_STAGE_NONE);
#else
        TEST_CHECK(!job_captured && !vocr_allocs && cache_data[0].failed_stage == VJO_STAGE_OCR);
        TEST_CHECK(rc == VJO_E_OCR_UNAVAILABLE);
#endif
        mem_free();
    }
}

/* The cached result belongs to its model files: switching the size or the
 * models' directory drops it; a setting that is no pipeline's keeps it. */
static void test_engine_cache_tracks_backend_models_and_storage(void)
{
    reset();
    apply_config();
    cache_ok = 1;
    loaded.font_size_ja = 18; /* the overlay's look: no pipeline's */
    apply_config();
    TEST_CHECK(cache_ok);
    loaded.vocr_tier = VJO_VOCR_TIER_SMALL;
    apply_config();
    TEST_CHECK(!cache_ok);
    cache_ok = 1;
    loaded.vocr_tier = VJO_VOCR_TIER_LARGE; /* another model size: another pipeline */
    apply_config();
    TEST_CHECK(!cache_ok);
    cache_ok = 1;
    loaded.vocr_tier = VJO_VOCR_TIER_NORMAL;
    apply_config();
    TEST_CHECK(!cache_ok);
    cache_ok = 1;
    strcpy(loaded.ocr_model_dir, "ux0:data/other-ocr");
    apply_config();
    TEST_CHECK(!cache_ok);
    /* API 43: hook_trust decides which text a job shows (a trusted hook's or OCR's): switching it drops the result,
     * so the overlay's next open reads the screen with the new rule */
    cache_ok = 1;
    loaded.hook_trust = !loaded.hook_trust;
    apply_config();
    TEST_CHECK(!cache_ok);
}

static void test_vocr_config(void)
{
    VjoConfig c;
    const char ini[] = "ocr_backend = VOCR\nvocr_model = FL10_w8.vocr\n";
    const char bad[] = "vocr_model = ../H15_w8.vocr\nvocr_model = H15_w8.bin\nvocr_model =\n";
    vjo_config_defaults(&c);
    /* Normal (VOCR6-N) by default; a job runs Small (VOCR5-S) when the game's memory has no room for it. */
    TEST_CHECK(c.vocr_tier == VJO_VOCR_TIER_NORMAL && c.log_file == 1);
    TEST_CHECK(!strcmp(vjo_vocr_tier_model(c.vocr_tier), VJO_VOCR_TIER2_NAME) &&
               !strcmp(vjo_vocr_tier_model(VJO_VOCR_TIER_SMALL), VJO_VOCR_TIER1_NAME) &&
               !strcmp(vjo_vocr_tier_model(VJO_VOCR_TIER_LARGE), VJO_VOCR_TIER3_NAME));
    vjo_config_parse(&c, ini, sizeof(ini) - 1); /* v0.2.0's FL10 chose the smallest size */
    TEST_CHECK(c.vocr_tier == VJO_VOCR_TIER_SMALL && c.n_warnings == 0);
    vjo_config_defaults(&c);
    vjo_config_parse(&c, bad, sizeof(bad) - 1);
    TEST_CHECK(c.vocr_tier == VJO_VOCR_TIER_NORMAL && c.n_warnings == 3); /* each kept the default */
    const char fallbacks[] = "vocr_fallback_model = none\nvocr_fallback_model = ../x.vocr\n";
    vjo_config_defaults(&c);
    vjo_config_parse(&c, fallbacks, sizeof(fallbacks) - 1); /* no longer read: the tiers fall back */
    TEST_CHECK(c.vocr_tier == VJO_VOCR_TIER_NORMAL && !c.n_warnings);
    /* A saved VOCR2 choice keeps the configured model's size: the local engine runs it. */
    const char old_ini[] = "ocr_backend = VoCr2\nvocr_model = H15_w8.vocr\n";
    vjo_config_defaults(&c);
    vjo_config_parse(&c, old_ini, sizeof(old_ini) - 1);
    TEST_CHECK(c.n_warnings == 1 && c.vocr_tier == VJO_VOCR_TIER_NORMAL);
}

static void test_vocr_model_change_invalidates_cache(void)
{
    reset();
    apply_config();
    cache_ok = 1;
    loaded.vocr_tier = VJO_VOCR_TIER_SMALL;
    apply_config();
    TEST_CHECK(!cache_ok);
    cache_ok = 1;
    loaded.ocr_refine_lines = loaded.ocr_refine_lines == 1 ? 2 : 1; /* and so is the refinement's mode */
    apply_config();
    TEST_CHECK(!cache_ok);
    cache_ok = 1;
    loaded.ocr_vertical = !loaded.ocr_vertical; /* and vertical text (another model, columns) */
    apply_config();
    TEST_CHECK(!cache_ok);
    cache_ok = 1;
    loaded.ocr_line_fix = loaded.ocr_line_fix == 2 ? 1 : 2; /* API 50: and the line finder's fixes */
    apply_config();
    TEST_CHECK(!cache_ok);
    cache_ok = 1;
    apply_config(); /* the same again: nothing changed */
    TEST_CHECK(cache_ok);
}

/* API 47: config.ini ocr_vertical goes into the speed word of every user job as it is (off 0, on 1, auto 2), which
 * keeps the job's tiers, model name and the rest of its flags and speed. The text's Anki cards and the job's log
 * line name the vertical model where its lines were columns (VJO_GAME_OCR_LINE_COLUMN): with on, and with auto where
 * the rows looked like vertical text; else the size's model, as before. A vertical model that fails to open (on)
 * leaves no size out of the game's ladder and is not retried with a smaller size: its file is not theirs. Off (the
 * default) sends no vertical bit. */
static void test_vocr_vertical_request(void)
{
    const uint32_t field = VJO_GAME_OCR_S_VERTICAL;
    reset();
    fake_recognized_text = "猫";
    TEST_CHECK(cfg.ocr_vertical == VJO_VERTICAL_DEFAULT && run_vocr_job() == VJO_OK);
    const uint32_t off_flags = fake_game_request.flags, off_speed = fake_game_request.speed & ~field;
    TEST_CHECK(!(fake_game_request.speed & field) || VJO_VERTICAL_DEFAULT != VJO_VERTICAL_OFF);
    mem_free();
    for (int mode = VJO_VERTICAL_OFF; mode <= VJO_VERTICAL_AUTO; mode++)
        for (int rows_vertical = 0; rows_vertical <= 1; rows_vertical++) {
            reset();
            cfg.ocr_vertical = mode;
            fake_rows_vertical = rows_vertical;
            fake_recognized_text = "猫";
            int columns = mode == VJO_VERTICAL_ON || (mode == VJO_VERTICAL_AUTO && rows_vertical);
            const char *model = columns ? VJO_VOCR_VERTICAL_NAME : VJO_VOCR_TIER2_NAME;
            TEST_CHECK(run_vocr_job() == VJO_OK && cache_data[0].sentence && !strcmp(cache_data[0].sentence, "猫"));
            TEST_CHECK_(fake_game_request.flags == off_flags && (fake_game_request.speed & ~field) == off_speed &&
                            (fake_game_request.speed & field) == (uint32_t)mode << VJO_GAME_OCR_S_VERTICAL_SHIFT &&
                            fake_game_request.tiers == 3u && !strcmp(fake_game_request.model, VJO_VOCR_TIER2_NAME) &&
                            !strcmp(anki_model, model),
                        "mode %d rows_vertical %d: flags 0x%08X (0x%08X), speed 0x%08X, tiers 0x%X, model %s, cards %s",
                        mode, rows_vertical, (unsigned)fake_game_request.flags, (unsigned)off_flags,
                        (unsigned)fake_game_request.speed, fake_game_request.tiers, fake_game_request.model, anki_model);
#if VJO_DEBUG
            TEST_CHECK(columns ? logged("game vocr rc=0 " VJO_VOCR_VERTICAL_NAME ": ") != NULL
                               : logged("game vocr rc=0 " VJO_VOCR_TIER2_NAME ": ") != NULL);
#endif
            mem_free();
        }
    /* on, its file missing: the job fails once, no size is left out, none is tried instead */
    reset();
    cfg.ocr_vertical = VJO_VERTICAL_ON;
    fake_missing_model = VJO_VOCR_VERTICAL_NAME;
    fake_model_misses = 0;
    TEST_CHECK(run_vocr_job() == VJO_E_OCR_MODEL && vocr_allocs == 1 && fake_model_misses == 1 && !vocr_unusable);
    TEST_CHECK(logged("is unusable here") == NULL);
    fake_missing_model = NULL;
    mem_free();
    /* auto, its file missing: the worker reads the rows (the size's model names the cards) */
    reset();
    cfg.ocr_vertical = VJO_VERTICAL_AUTO;
    fake_rows_vertical = 1;
    fake_missing_model = VJO_VOCR_VERTICAL_NAME;
    fake_recognized_text = "猫";
    TEST_CHECK(run_vocr_job() == VJO_OK && !strcmp(anki_model, VJO_VOCR_TIER2_NAME) && !vocr_unusable);
    fake_missing_model = NULL;
    mem_free();
    /* off again: the sizes as before */
    reset();
    cfg.ocr_vertical = VJO_VERTICAL_OFF;
    fake_recognized_text = "猫";
    TEST_CHECK(run_vocr_job() == VJO_OK && fake_game_request.flags == off_flags && !(fake_game_request.speed & field) &&
               !strcmp(fake_game_request.model, VJO_VOCR_TIER2_NAME));
    mem_free();
}

#ifdef VJO_WITH_VOCR
static void strokes(unsigned y0, unsigned y1)
{
    for (unsigned y = y0; y < y1; y++)
        for (unsigned x = 10; x < 190; x += 6)
            memset(raw_region + ((size_t)y * 200 + x) * 4, 235, 6); /* 1.5 px of R, G, B */
}

static void test_vocr_missing_model_releases_workspace(void)
{
    reset();
    cfg.vocr_tier = VJO_VOCR_TIER_SMALL; /* the smallest size: one job, one error */
    strokes(15, 39); /* one light line on black */
    int rc = run_vocr_job();
    TEST_CHECK(rc == VJO_E_OCR_MODEL && cache_data[0].failed_stage == VJO_STAGE_OCR);
    TEST_CHECK(!cache_data[0].sentence && vocr_allocs == 1 && vocr_frees == 1 && fake_game_request.tiers == 1u);
    mem_free();
    reset(); /* the default size, both files missing: two jobs, the smaller one's error */
    strokes(15, 39);
    TEST_CHECK(run_vocr_job() == VJO_E_OCR_MODEL && cache_data[0].failed_stage == VJO_STAGE_OCR);
    TEST_CHECK(!cache_data[0].sentence && vocr_allocs == 2 && vocr_frees == 2);
    TEST_CHECK(!strcmp(fake_game_request.model, VJO_VOCR_TIER1_NAME) && fake_game_request.tiers == 1u &&
               !fake_game_request.fallback[0]);
    TEST_CHECK(logged(VJO_VOCR_TIER1_NAME " is unusable here (io=0x00000000), using none for this game") != NULL);
    mem_free();
}

/* The chosen size's model is missing or corrupt on the card: the same job is
 * repeated with the next smaller tier, and this game's later jobs leave it out
 * until the game changes. Only the log says so. The memory ladder itself is the
 * game worker's (test_game_vocr.c); this is the Shell's half. */
static void test_vocr_unusable_model_uses_the_fallback(void)
{
    reset();
    fake_missing_model = VJO_VOCR_TIER2_NAME;
    fake_recognized_text = "猫";
    TEST_CHECK(run_vocr_job() == VJO_OK && cache_data[0].sentence && !strcmp(cache_data[0].sentence, "猫"));
    TEST_CHECK(vocr_allocs == 2 && !strcmp(fake_game_request.model, VJO_VOCR_TIER1_NAME) && fake_game_request.tiers == 1u);
    TEST_CHECK(logged(VJO_VOCR_TIER2_NAME " is unusable here (io=0x80010002), using " VJO_VOCR_TIER1_NAME " for this game") != NULL);
    TEST_CHECK(!cache_data[0].err.detail && cache_data[0].failed_stage == VJO_STAGE_NONE); /* nothing shown of it */
    TEST_CHECK(!strcmp(anki_model, VJO_VOCR_TIER1_NAME)); /* the cards name the model that read the text */
    mem_free();
    unsigned before = vocr_allocs;
    TEST_ASSERT(mem_alloc() == 0);
    job_cfg = cfg; vjo_arena_reset(&results[0]);
    TEST_CHECK(run_job_t(&results[0], &cache_data[0]) == VJO_OK); /* one job, Normal left out */
    TEST_CHECK(vocr_allocs == before + 1 && !strcmp(fake_game_request.model, VJO_VOCR_TIER1_NAME));
    TEST_CHECK(fake_game_request.tiers == 1u && !fake_game_request.fallback[0]);
    mem_free();
    game_generation++; job_generation = game_generation; /* another game: Normal is tried again */
    fake_missing_model = NULL;
    TEST_ASSERT(mem_alloc() == 0);
    job_cfg = cfg; vjo_arena_reset(&results[0]);
    TEST_CHECK(run_job_t(&results[0], &cache_data[0]) == VJO_OK);
    TEST_CHECK(!strcmp(fake_game_request.model, VJO_VOCR_TIER2_NAME) && fake_game_request.tiers == 3u);
    mem_free();
    /* Large chosen and Normal's file bad: Large stays in this game's ladder, Normal is left out (when
     * Large has another file: shell_worker_shared_tiers). */
    if (!strcmp(VJO_VOCR_TIER2_NAME, VJO_VOCR_TIER3_NAME)) return;
    reset();
    cfg.vocr_tier = VJO_VOCR_TIER_LARGE;
    fake_tier_checks = 1u << (VJO_VOCR_TIER_LARGE - 1u); /* Large does not fit this game */
    fake_missing_model = VJO_VOCR_TIER2_NAME; fake_recognized_text = "猫";
    TEST_CHECK(run_vocr_job() == VJO_OK && vocr_allocs == 2);
    TEST_CHECK(vocr_unusable == 1u << (VJO_VOCR_TIER_NORMAL - 1u) && fake_game_request.tiers == 1u);
    mem_free();
    TEST_ASSERT(mem_alloc() == 0);
    job_cfg = cfg; vjo_arena_reset(&results[0]);
    TEST_CHECK(run_job_t(&results[0], &cache_data[0]) == VJO_OK);
    TEST_CHECK(fake_game_request.tiers == 5u && !strcmp(fake_game_request.model, VJO_VOCR_TIER3_NAME));
    /* the worker went from Large to Small: two tiers down, Small read the text */
    TEST_CHECK(logged("tier large did not fit in this game (need 19777216, free 91238400); using small") != NULL);
    mem_free();
}

/* A transient failure (an Open timeout, unavailable storage) retries this job
 * with the next smaller tier but does not stop later jobs from trying Normal. */
static void test_vocr_transient_model_failure_is_not_sticky(void)
{
    const int32_t transient[] = {VJO_ERR_TIMEOUT, (int32_t)0x80010013u, VJO_ERR_CANCELLED};
    for (unsigned i = 0; i < 3; i++) {
        reset();
        fake_missing_model = VJO_VOCR_TIER2_NAME; fake_missing_io = transient[i];
        fake_recognized_text = "猫";
        TEST_CHECK(run_vocr_job() == VJO_OK && vocr_allocs == 2 && !strcmp(fake_game_request.model, VJO_VOCR_TIER1_NAME));
        TEST_CHECK(logged("using " VJO_VOCR_TIER1_NAME " for this job") != NULL && !vocr_unusable);
        mem_free();
        fake_missing_model = NULL;
        TEST_ASSERT(mem_alloc() == 0);
        job_cfg = cfg; vjo_arena_reset(&results[0]);
        TEST_CHECK(run_job_t(&results[0], &cache_data[0]) == VJO_OK && vocr_allocs == 3);
        TEST_CHECK(!strcmp(fake_game_request.model, VJO_VOCR_TIER2_NAME) && fake_game_request.tiers == 3u);
        mem_free();
    }
    /* A hash/format rejection (io 0) is a bad file: sticky, like a missing one (for Large too when
     * Normal kept Large's model: one file) */
    reset();
    fake_missing_model = VJO_VOCR_TIER2_NAME; fake_missing_io = 0; fake_recognized_text = "猫";
    TEST_CHECK(run_vocr_job() == VJO_OK && vocr_unusable == (strcmp(VJO_VOCR_TIER2_NAME, VJO_VOCR_TIER3_NAME) ? 2u : 6u));
    mem_free();
}

/* No retry when the tier the worker ran is already the smallest (its own file
 * failed: then it is left out, and later jobs still try Normal), when the job
 * was cancelled, or for the smallest size. */
static void test_vocr_model_error_retry_limits(void)
{
    reset();
    fake_memory_fallback = 1; fake_missing_model = VJO_VOCR_TIER1_NAME; fake_missing_with_fallback = 1;
    TEST_CHECK(run_vocr_job() == VJO_E_OCR_MODEL && vocr_allocs == 1 && vocr_unusable == 1u);
    mem_free();
    TEST_ASSERT(mem_alloc() == 0);
    fake_memory_fallback = 0; fake_missing_model = NULL;
    job_cfg = cfg; vjo_arena_reset(&results[0]);
    TEST_CHECK(run_job_t(&results[0], &cache_data[0]) == VJO_OK && fake_game_request.tiers == 2u); /* Normal alone */
    mem_free();
    reset();
    fake_missing_model = VJO_VOCR_TIER2_NAME; fake_cancel_on_model_error = 1;
    run_vocr_job();
    TEST_CHECK(vocr_allocs == 1 && !vocr_unusable);
    mem_free();
    reset();
    cfg.vocr_tier = VJO_VOCR_TIER_SMALL;
    fake_missing_model = VJO_VOCR_TIER1_NAME;
    TEST_CHECK(run_vocr_job() == VJO_E_OCR_MODEL && vocr_allocs == 1 && fake_game_request.tiers == 1u);
    mem_free();
}

/* Neither tier fitted: the job line says the smaller one was refused too. */
static void test_vocr_refused_fallback_is_labelled(void)
{
    reset();
    fail_vocr_user = 1; fake_memory_fallback = 1; fake_free_user = 4300000;
    TEST_CHECK(run_vocr_job() == VJO_E_OOM);
#if VJO_DEBUG
    TEST_CHECK(logged("game vocr rc=-103 " VJO_VOCR_TIER1_NAME " (fallback, refused too):") != NULL);
#else
    /* the normal build's job line, and its refused memory check */
    TEST_CHECK(logged("ocr rc=-103 " VJO_VOCR_TIER1_NAME ": 0 lines, ") != NULL);
    TEST_CHECK(logged("game vocr memory: free USER=4300000 + own weights 0, needs workspace ") != NULL &&
               logged("check refused") != NULL);
#endif
    TEST_CHECK(cache_data[0].err.detail && !strcmp(cache_data[0].err.detail, OOM_GAME_REGION));
    mem_free();
    /* every tier refused by the ladder: one line per tier, the last says nothing smaller fits */
    reset();
    cfg.vocr_tier = VJO_VOCR_TIER_LARGE;
    fake_tier_checks = VJO_VOCR_TIERS_ALL; fake_free_user = 4300000;
    memcpy(job_title, "PCSG00415", 10);
    TEST_CHECK(run_vocr_job() == VJO_E_OOM);
    TEST_CHECK(logged("tier large did not fit in PCSG00415 (need 19777216, free 4312288); trying normal") != NULL);
    TEST_CHECK(logged("tier normal did not fit in PCSG00415 (need 10388608, free 4308192); trying small") != NULL);
    TEST_CHECK(logged("tier small did not fit in PCSG00415 (need 5194304, free 4304096); no smaller tier: "
                      "not enough memory") != NULL);
    mem_free();
}

/* API 21, the owner's 04:54Z rule: a size that does not fit the game is left
 * silently. The worker ran a smaller tier: the log names each tier that did not
 * fit, in that game, with what it needed against what was free, and the tier
 * used; the user sees the text as usual and no note of it anywhere. */
static void test_vocr_memory_fallback_is_logged(void)
{
    reset();
    cfg.vocr_tier = VJO_VOCR_TIER_LARGE;
    fake_recognized_text = "猫";
    fake_tier_checks = 1u << (VJO_VOCR_TIER_LARGE - 1u) | 1u << (VJO_VOCR_TIER_NORMAL - 1u);
    memcpy(job_title, "PCSG00415", 10);
    TEST_CHECK(run_vocr_job() == VJO_OK && !strcmp(cache_data[0].sentence, "猫") && vocr_allocs == 1);
    TEST_CHECK(fake_game_request.tiers == 7u && !strcmp(fake_game_request.model, VJO_VOCR_TIER3_NAME));
#if VJO_DEBUG
    const char *line = logged("game vocr rc=0 " VJO_VOCR_TIER1_NAME " (fallback):");
#else
    const char *line = logged("ocr rc=0 " VJO_VOCR_TIER1_NAME ": "); /* the normal build's job line: the size that ran */
#endif
    TEST_CHECK(line != NULL);
    /* need: the check's bytes and the tier's reserve; free: free USER and the own blocks released first */
    line = logged("tier large");
    TEST_CHECK(line && !strcmp(line, "tier large did not fit in PCSG00415 (need 19777216, free 91238400); trying normal"));
    TEST_MSG("%s", line ? line : "(no large line)");
    line = logged("tier normal");
    TEST_CHECK(line && !strcmp(line, "tier normal did not fit in PCSG00415 (need 10388608, free 91234304); using small"));
    TEST_MSG("%s", line ? line : "(no normal line)");
    TEST_CHECK(!logged("tier small")); /* it fitted */
    /* nothing of it in what the user sees: no error, no warning, the Anki cards name Small's model */
    TEST_CHECK(!cache_data[0].err.detail && cache_data[0].failed_stage == VJO_STAGE_NONE && !g_view.n_warnings);
    TEST_CHECK(!strcmp(anki_model, VJO_VOCR_TIER1_NAME));
    mem_free();
    /* the largest tier fitted: no tier line at all */
    reset();
    cfg.vocr_tier = VJO_VOCR_TIER_LARGE;
    fake_recognized_text = "猫"; fake_tier_checks = 0;
    TEST_CHECK(run_vocr_job() == VJO_OK && !logged("did not fit") && !strcmp(anki_model, VJO_VOCR_TIER3_NAME));
    mem_free();
    /* a job's game is the title it started in, kept while the job runs */
    reset();
    memcpy(title_id, "PCSB00123", 10);
    TEST_ASSERT(mem_alloc() == 0);
    start_job("test", 0);
    TEST_CHECK(!strcmp(job_title, "PCSB00123"));
    memcpy(title_id, "PCSG00415", 10);
    TEST_CHECK(!strcmp(job_title, "PCSB00123"));
    job_running = 0;
    mem_free();
}

static void test_vocr_allocation_fallback_and_failure(void)
{
    reset();
    fail_vocr_user = 1;
    int rc = run_vocr_job();
    TEST_CHECK(rc == VJO_E_OOM && cache_data[0].failed_stage == VJO_STAGE_OCR);
    TEST_CHECK(vocr_paf_allocs == 0 && vocr_frees == 0);
    mem_free();
}

#if VJO_DEBUG /* the debug build's log lines (the normal build's: test_normal_build_*) */
static void test_vocr_logs_memory_check_and_frames(void)
{
    reset();
    fail_vocr_user = 1; /* refused: the numbers are logged, not workspace=0 */
    TEST_CHECK(run_vocr_job() == VJO_E_OOM);
    const char *line = logged("game vocr rc=");
    TEST_CHECK(line && strstr(line, "USER workspace=244896"));
    line = logged("game vocr memory:");
    /* 5,000,000 - 4,194,304 - 245,760 - 659,456 = -99,520: the job was 99,520 B short */
    TEST_CHECK(line && !strcmp(line, "game vocr memory: free USER=5000000, needs workspace 245760 + "
                                      "weights 659456 + reserve 4194304: check refused, headroom -99520"));
    TEST_MSG("%s", line ? line : "(no memory line)");
    TEST_CHECK(!logged("game vocr speed:") && !logged("game vocr lines"));
    mem_free();

    reset();
    fail_vocr_user = 1; fake_free_user = (int32_t)0x80020001u; /* the query itself failed */
    TEST_CHECK(run_vocr_job() == VJO_E_OOM);
    line = logged("game vocr memory:");
    TEST_CHECK(line && !strcmp(line, "game vocr memory: free USER unknown (error 0x80020001), needs "
                                      "workspace 245760 + weights 659456 + reserve 4194304: check refused"));
    TEST_MSG("%s", line ? line : "(no memory line)");
    mem_free();

    reset();
    fake_recognized_text = "猫";
    fake_lines = 14; /* three log lines of at most six */
    TEST_CHECK(run_vocr_job() == VJO_OK); /* blank region: the fake result is published */
    line = logged("game vocr rc=0");
    TEST_CHECK(line && strstr(line, "recognize=92 ms frames=205 "));
    line = logged("game vocr memory:");
    TEST_CHECK(line && strstr(line, "free USER=91226112") && strstr(line, "check passed, headroom "));
    line = logged("game vocr speed:");
    TEST_CHECK(line && !strcmp(line, "game vocr speed: 205 frames in 92345 us, 450 us per frame"));
    line = logged("game vocr lines:");
    TEST_CHECK(line && !strncmp(line, "game vocr lines: 917x30 114f 52.1ms 916x30 113f 52.1ms", 54));
    TEST_MSG("%s", line ? line : "(no lines)");
    unsigned chunks = 0, entries = 0;
    for (unsigned i = 0; i < log_count && i < 32; i++)
        if (!strncmp(log_lines[i], "game vocr lines", 15)) {
            chunks++;
            for (const char *c = log_lines[i]; (c = strstr(c, "ms")); c += 2) entries++;
            TEST_CHECK(strlen(log_lines[i]) < 254); /* vjo_log's line limit */
        }
    TEST_CHECK(chunks == 3 && entries == 14);
    mem_free();
    /* API 20: lines the LM decoded carry their confidence, a '*' where it changed the text;
     * still six to a log line and under vjo_log's limit at the longest */
    reset();
    fake_recognized_text = "猫";
    fake_lines = 6;
    fake_line_flags = 0x8000u | 0x4000u | 612u;
    TEST_CHECK(run_vocr_job() == VJO_OK);
    line = logged("game vocr lines:");
    TEST_CHECK(line && !strncmp(line, "game vocr lines: 917x30 114f 52.1ms c612* 916x30 113f 52.1ms c612*", 66));
    TEST_MSG("%s", line ? line : "(no lines)");
    TEST_CHECK(line && strlen(line) < 254 && !logged("game vocr lines (cont)"));
    reset();
    fake_recognized_text = "猫";
    fake_lines = 1;
    fake_line_flags = 0x4000u | 999u; /* decoded, unchanged */
    TEST_CHECK(run_vocr_job() == VJO_OK);
    line = logged("game vocr lines:");
    TEST_CHECK(line && !strcmp(line, "game vocr lines: 917x30 114f 52.1ms c999"));
    TEST_MSG("%s", line ? line : "(no lines)");
    /* read again by the cascade's larger model (its reading and confidence shown), changed */
    reset();
    fake_recognized_text = "猫";
    fake_lines = 1;
    fake_line_flags = 0x8000u | 0x4000u | 0x0400u | 845u;
    TEST_CHECK(run_vocr_job() == VJO_OK);
    line = logged("game vocr lines:");
    TEST_CHECK(line && !strcmp(line, "game vocr lines: 917x30 114f 52.1ms c845*^"));
    TEST_MSG("%s", line ? line : "(no lines)");
    TEST_CHECK(!logged("game vocr cascade speed:"));
    /* the cascade's model, from its step: its frames and time apart from the job's model's */
    reset();
    fake_recognized_text = "猫";
    fake_lines = 1;
    fake_line_flags = 0x8000u | 0x4000u | 0x0400u | 845u;
    fake_cascade_frames = 160;
    fake_cascade_us = 128000;
    TEST_CHECK(run_vocr_job() == VJO_OK);
    line = logged("game vocr speed:");
    TEST_CHECK(line && !strcmp(line, "game vocr speed: 205 frames in 92345 us, 450 us per frame"));
    line = logged("game vocr cascade speed:");
    TEST_CHECK(line && !strcmp(line, "game vocr cascade speed: 160 frames in 128000 us, 800 us per frame"));
    TEST_MSG("%s", line ? line : "(no cascade speed line)");
    fake_cascade_frames = fake_cascade_us = 0;
    fake_line_flags = 0;
    mem_free();
}
#endif

#if VJO_DEBUG /* the debug build's log lines (the normal build's: test_normal_build_*) */
/* The job policy in the request (API 20): the stable-text wait from its keys,
 * the blank-line check only with ocr_skip_blank_lines = on (off by default). */
/* A job's log on main's defaults (15 steps with the cascade, two lines, the
 * lookup): the result's lines (vocr_run_one) and the text's (log_text_lines)
 * each go out as one append, the rest one each, and no batch is held across
 * the game's exchange (vjoOcrSubmit/Read check it). On the console an append
 * of log.txt costs 12-46 ms (#81's self-test timestamps). */
static void test_vocr_job_log_is_appended_in_batches(void)
{
    reset();
    fake_recognized_text = "猫が\n好き"; fake_lines = 2;
    fake_steps = 1; fake_cascade_frames = 40; fake_cascade_us = 202000;
    unsigned lines0 = log_count, appends0 = log_appends;
    batched_lines = 0;
    TEST_CHECK(run_vocr_job() == VJO_OK);
    unsigned lines = log_count - lines0, appends = log_appends - appends0;
    /* rc, capture, memory, speed, cascade speed, lines, 15 steps: 21 in one append; 2 text lines in one */
    TEST_CHECK(batched_lines == 23);
    TEST_CHECK(lines - appends == 23 - 2);
    TEST_CHECK(!open_batch);
    TEST_MSG("%u lines, %u appends, %u batched", lines, appends, batched_lines);
    printf("\n  a job's %u log lines: %u appends of log.txt (line by line: %u)\n", lines, appends, lines);
    /* every line is still logged, the steps and the text included */
    TEST_CHECK(logged("game vocr rc=0") != NULL);
    TEST_CHECK(logged("gstep job 18 ") != NULL); /* the cascade's step (this file's vjo_gstep_format) */
    TEST_CHECK(logged("game vocr text 1: ") != NULL);
    fake_steps = 0; fake_cascade_frames = fake_cascade_us = 0;
    mem_free();
}
#endif

static void test_vocr_request_carries_the_job_policy(void)
{
    reset();
    fake_recognized_text = "猫";
    TEST_CHECK(run_vocr_job() == VJO_OK);
    TEST_CHECK(!(fake_game_request.flags & VJO_GAME_OCR_F_BLANK));
    TEST_CHECK((fake_game_request.flags >> VJO_GAME_OCR_SETTLE_SHIFT & 0xFFu) == 15u &&
               (fake_game_request.flags >> VJO_GAME_OCR_SETTLE_MAX_SHIFT & 0xFFu) == 40u);
    TEST_CHECK(!(fake_game_request.flags & VJO_GAME_OCR_F_SETTLE_FIRST)); /* the kernel saw the region settle */
    TEST_MSG("default flags 0x%08X", (unsigned)fake_game_request.flags);
    mem_free();
    /* a screen the kernel has not seen settle (VjoState.stable 0): the job waits first */
    reset();
    fake_recognized_text = "猫";
    state.stable = 0;
    TEST_CHECK(run_vocr_job() == VJO_OK);
    TEST_CHECK(fake_game_request.flags & VJO_GAME_OCR_F_SETTLE_FIRST);
    TEST_MSG("unsettled screen: flags 0x%08X", (unsigned)fake_game_request.flags);
    mem_free();
    /* ... unless the wait is off: then nothing of it is sent */
    reset();
    cfg.ocr_settle_ms = 0;
    fake_recognized_text = "猫";
    state.stable = 0;
    TEST_CHECK(run_vocr_job() == VJO_OK);
    TEST_CHECK(!(fake_game_request.flags & VJO_GAME_OCR_F_SETTLE_FIRST) &&
               !(fake_game_request.flags >> VJO_GAME_OCR_SETTLE_SHIFT));
    mem_free();
    reset();
    cfg.ocr_skip_blank_lines = 1;
    cfg.ocr_settle_ms = 0;
    fake_recognized_text = "猫";
    TEST_CHECK(run_vocr_job() == VJO_OK);
    TEST_CHECK((fake_game_request.flags & VJO_GAME_OCR_F_BLANK) &&
               !(fake_game_request.flags >> VJO_GAME_OCR_SETTLE_SHIFT & 0xFFu) &&
               !(fake_game_request.flags & VJO_GAME_OCR_F_SETTLE_FIRST));
    TEST_MSG("blank on, wait off: flags 0x%08X", (unsigned)fake_game_request.flags);
    mem_free();
    /* API 26: the text-presence gate rides in the speed word, only with ocr_text_gate = on (off by default) */
    reset();
    fake_recognized_text = "猫";
    TEST_CHECK(run_vocr_job() == VJO_OK && !(fake_game_request.speed & VJO_GAME_OCR_S_GATE));
    mem_free();
    reset();
    cfg.ocr_text_gate = 1;
    fake_recognized_text = "猫";
    TEST_CHECK(run_vocr_job() == VJO_OK && (fake_game_request.speed & VJO_GAME_OCR_S_GATE) &&
               !(fake_game_request.speed & ~VJO_GAME_OCR_S_ALL));
    TEST_MSG("gate on: speed 0x%08X", (unsigned)fake_game_request.speed);
    mem_free();
    /* API 42: the head gate and the two-level head ride in the speed word as ocr_head_gate's bits: both with on, the
     * default, either alone with gate or two_level, neither with off */
    for (int bits = 3; bits >= 0; bits--) {
        const uint32_t want = (bits & VJO_HEAD_GATE ? VJO_GAME_OCR_S_HEAD_GATE : 0u) |
                              (bits & VJO_HEAD_TWO_LEVEL ? VJO_GAME_OCR_S_TWO_LEVEL : 0u);
        reset();
        TEST_CHECK(bits != 3 || cfg.ocr_head_gate == 3); /* the default */
        cfg.ocr_head_gate = bits;
        fake_recognized_text = "猫";
        TEST_CHECK(run_vocr_job() == VJO_OK &&
                   (fake_game_request.speed & (VJO_GAME_OCR_S_HEAD_GATE | VJO_GAME_OCR_S_TWO_LEVEL)) == want &&
                   !(fake_game_request.speed & ~VJO_GAME_OCR_S_ALL));
        TEST_MSG("ocr_head_gate bits %d: speed 0x%08X", bits, (unsigned)fake_game_request.speed);
        mem_free();
    }
    /* API 36: the border rule (core/textnorm.h) rides in the speed word with non_japanese_filter = lines, the
     * default, and not with none */
    reset();
    fake_recognized_text = "猫";
    TEST_CHECK(run_vocr_job() == VJO_OK && (fake_game_request.speed & VJO_GAME_OCR_S_DROP_BORDERS) &&
               !(fake_game_request.speed & ~VJO_GAME_OCR_S_ALL));
    mem_free();
    reset();
    cfg.non_japanese_filter = VJO_FILTER_NONE;
    fake_recognized_text = "猫";
    TEST_CHECK(run_vocr_job() == VJO_OK && !(fake_game_request.speed & VJO_GAME_OCR_S_DROP_BORDERS));
    TEST_MSG("non_japanese_filter = none: speed 0x%08X", (unsigned)fake_game_request.speed);
    mem_free();
    /* API 43: hook_trust (on by default) rides in the speed word as VJO_GAME_OCR_S_TRUST; hook_trust = off drops it
     * (Rubi 0.9.7's selection), and the job's pick stays automatic either way */
    reset();
    fake_recognized_text = "猫";
    TEST_CHECK(cfg.hook_trust == 1);
    TEST_CHECK(run_vocr_job() == VJO_OK && (fake_game_request.speed & VJO_GAME_OCR_S_TRUST) &&
               !(fake_game_request.speed & ~VJO_GAME_OCR_S_ALL) && fake_game_request.hook_pick == VJO_HOOK_PICK_AUTO);
    mem_free();
    reset();
    cfg.hook_trust = 0;
    fake_recognized_text = "猫";
    TEST_CHECK(run_vocr_job() == VJO_OK && !(fake_game_request.speed & VJO_GAME_OCR_S_TRUST) &&
               fake_game_request.hook_pick == VJO_HOOK_PICK_AUTO);
    TEST_MSG("hook_trust = off: speed 0x%08X", (unsigned)fake_game_request.speed);
    mem_free();
    /* API 50: the line finder's fixes ride in the speed word: the default (on) bit 21 alone, junk bits 21 and 22,
     * all bits 21-23, off none */
    const uint32_t lf_bits = VJO_GAME_OCR_S_LINE_FIX | VJO_GAME_OCR_S_LINE_JUNK | VJO_GAME_OCR_S_LINE_VALLEY;
    static const struct { int cfg; uint32_t want; } fixes[] = {
        {1, VJO_GAME_OCR_S_LINE_FIX}, {2, VJO_GAME_OCR_S_LINE_FIX | VJO_GAME_OCR_S_LINE_JUNK},
        {3, VJO_GAME_OCR_S_LINE_FIX | VJO_GAME_OCR_S_LINE_JUNK | VJO_GAME_OCR_S_LINE_VALLEY}, {0, 0u}};
    for (unsigned k = 0; k < sizeof(fixes) / sizeof(fixes[0]); k++) {
        reset();
        if (k == 0) TEST_CHECK(cfg.ocr_line_fix == 1);       /* the default */
        cfg.ocr_line_fix = fixes[k].cfg;
        fake_recognized_text = "猫";
        TEST_CHECK(run_vocr_job() == VJO_OK &&
                   (fake_game_request.speed & lf_bits) == fixes[k].want &&
                   !(fake_game_request.speed & ~VJO_GAME_OCR_S_ALL));
        TEST_MSG("ocr_line_fix %d: speed 0x%08X", fixes[k].cfg, (unsigned)fake_game_request.speed);
        mem_free();
    }
    /* the refinement's mode: psp by default, on, off */
    static const unsigned modes[] = {VJO_GAME_OCR_REFINE_PSP, VJO_GAME_OCR_REFINE_ON, 0};
    for (unsigned i = 0; i < 3; i++) {
        reset();
        if (i) cfg.ocr_refine_lines = (int)modes[i];
        fake_recognized_text = "猫";
        TEST_CHECK(run_vocr_job() == VJO_OK);
        TEST_CHECK((fake_game_request.flags >> VJO_GAME_OCR_REFINE_SHIFT & VJO_GAME_OCR_REFINE_MASK) == modes[i]);
        TEST_MSG("mode %u: flags 0x%08X", modes[i], (unsigned)fake_game_request.flags);
        mem_free();
    }
    /* the cascade's ceiling in 250 ms steps: 2 s by default, off with 0, 1-124 ms one step, 3,750 the most */
    static const struct { int ms; unsigned units; } cascade[] = {{2000, 8}, {0, 0}, {100, 1}, {375, 2}, {3750, 15}};
    for (unsigned i = 0; i < sizeof(cascade) / sizeof(cascade[0]); i++) {
        reset();
        cfg.ocr_cascade_ms = cascade[i].ms;
        fake_recognized_text = "猫";
        TEST_CHECK(run_vocr_job() == VJO_OK);
        TEST_CHECK((fake_game_request.flags >> VJO_GAME_OCR_CASCADE_SHIFT & VJO_GAME_OCR_CASCADE_MASK) == cascade[i].units);
        TEST_MSG("ocr_cascade_ms %d: flags 0x%08X", cascade[i].ms, (unsigned)fake_game_request.flags);
        mem_free();
    }
    /* a wait of 1-4 ms is the shortest one (10 ms), not none; 1,000 ms and 10 s fit their bits */
    static const struct { int ms, max_ms; unsigned settle, max; } units[] = {
        {3, 100, 1, 2}, {14, 2000, 1, 40}, {15, 2000, 2, 40}, {1000, 10000, 100, 200}};
    for (unsigned i = 0; i < sizeof(units) / sizeof(units[0]); i++) {
        reset();
        cfg.ocr_settle_ms = units[i].ms;
        cfg.ocr_settle_max_ms = units[i].max_ms;
        fake_recognized_text = "猫";
        TEST_CHECK(run_vocr_job() == VJO_OK);
        TEST_CHECK((fake_game_request.flags >> VJO_GAME_OCR_SETTLE_SHIFT & 0xFFu) == units[i].settle &&
                   (fake_game_request.flags >> VJO_GAME_OCR_SETTLE_MAX_SHIFT & 0xFFu) == units[i].max);
        TEST_MSG("%d ms / %d ms: flags 0x%08X", units[i].ms, units[i].max_ms, (unsigned)fake_game_request.flags);
        mem_free();
    }
    /* ocr-first: a job the open overlay waits for (not Auto's, a refresh's or the self-test's)
     * waits at most ocr_settle_press_ms (1 s by default), ocr_settle_max_ms if that is less;
     * 0 is the job's own limit. */
    static const struct { int press, max_ms, prepares, refresh; unsigned max; } press[] = {
        {1000, 2000, 0, 0, 20}, {1000, 2000, 1, 0, 40}, {1000, 2000, 0, 1, 40}, {0, 2000, 0, 0, 40},
        {1000, 500, 0, 0, 10}, {300, 2000, 0, 0, 6}};
    for (unsigned i = 0; i < sizeof(press) / sizeof(press[0]); i++) {
        reset();
        cfg.ocr_settle_press_ms = press[i].press;
        cfg.ocr_settle_max_ms = press[i].max_ms;
        job_prepares = press[i].prepares;
        job_refresh = press[i].refresh;
        fake_recognized_text = "猫";
        TEST_CHECK(run_vocr_job() == VJO_OK);
        TEST_CHECK((fake_game_request.flags >> VJO_GAME_OCR_SETTLE_MAX_SHIFT & 0xFFu) == press[i].max);
        TEST_MSG("press %d ms, max %d ms, prepares %d, refresh %d: flags 0x%08X", press[i].press, press[i].max_ms,
                 press[i].prepares, press[i].refresh, (unsigned)fake_game_request.flags);
        job_refresh = 0;
        mem_free();
    }
}

/* The whole status line the user sees for an OCR-stage error. */
static const char *ocr_error_text(void)
{
    static char memory[512]; VjoArena arena;
    vjo_arena_init(&arena, memory, sizeof(memory));
    return vjo_err_text(&arena, cache_data[0].failed_stage, &cache_data[0].err);
}
static void check_oom_message(const char *advice, const char *must, const char *must_not)
{
    const char *text = ocr_error_text();
    TEST_CHECK(cache_data[0].err.rc == VJO_E_OOM && cache_data[0].failed_stage == VJO_STAGE_OCR);
    TEST_CHECK(cache_data[0].err.detail == advice || (advice && cache_data[0].err.detail &&
                                                      !strcmp(cache_data[0].err.detail, advice)));
    TEST_CHECK(!strncmp(text, "Text recognition failed: not enough memory", 42));
    if (advice) TEST_CHECK(strstr(text, advice) != NULL);
    TEST_CHECK(strlen(text) < sizeof(g_view.status)); /* shown whole in the status line */
    if (must) TEST_CHECK(strstr(text, must) != NULL);
    if (must_not) TEST_CHECK(strstr(text, must_not) == NULL);
    /* Memory statistics are requested now; model names and fallback hints stay in the log. */
    static const char *const hints[] = {"smallest", "smaller", "fallback", "fall back", "falls back", "instead",
                                        "did not fit in", "Small", "Normal", "Large", "MiB", "tier", "size", "VOCR"};
    for (unsigned i = 0; i < sizeof(hints) / sizeof(hints[0]); i++)
        TEST_CHECK_(strstr(text, hints[i]) == NULL, "'%s' in: %s", hints[i], text);
    TEST_MSG("%s", text);
}

static void test_vocr_oom_advice_names_the_stage(void)
{
    /* The game worker's memory check, from its reported numbers (game_ocr/main.c):
     * job = reserve 4194304 + workspace 245760 = 4440064. */
    /* job + FL10's 659456-byte block = 5099520; job + H15's 1159168 = 5599232. A job asks by
     * tiers (API 21): the weights its last check refused are its smallest tier's. No advice
     * names a model, a model file, or that a smaller one was tried. */
    const struct { int32_t free_user; uint32_t weights; const char *advice, *must, *must_not; } cases[] = {
        {4000000, 659456, OOM_GAME_NONE, "any OCR right now", "MiB"},            /* below the reserve */
        {4300000, 659456, OOM_GAME_REGION, "tighter dialogue region", "FL10"},   /* the region's job */
        {4300000, 0, OOM_GAME_REGION, "tighter dialogue region", "FL10"},        /* resident weights */
        {5200000, 1159168, OOM_GAME_SMALLEST, "for the OCR model's weights", "vocr"},
        {5000000, 1159168, OOM_GAME_SMALLEST, "Try again later", "VOCR3"},
        {5000000, 659456, OOM_GAME_SMALLEST, "for the OCR model's weights", "Select FL10"}, /* FL10's weights */
        {6000000, 659456, OOM_GAME_ALLOCATION, "after the memory check passed", "FL10"},
        {5000000, 0, OOM_GAME_ALLOCATION, "Try again", "FL10"},
        {(int32_t)0x80020001u, 659456, OOM_GAME_UNKNOWN, "could not be checked", "FL10"},
    };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        reset();
        fail_vocr_user = 1; fake_free_user = cases[i].free_user; fake_need_weights = cases[i].weights;
        TEST_CHECK(run_vocr_job() == VJO_E_OOM);
        TEST_CASE_("memory check %u", i);
        check_oom_message(cases[i].advice, cases[i].must, cases[i].must_not);
        const char *text = ocr_error_text();
        if (cases[i].free_user < 0) {
            TEST_CHECK(strstr(text, "Game RAM unavailable (0x80020001)") != NULL);
            TEST_CHECK(strstr(text, "free ") == NULL);
        } else {
            char want[160];
            snprintf(want, sizeof(want), "Game RAM (KiB): free %u + reclaim 0; need %u (240 workspace + %u models + 4096 reserve)",
                (unsigned)cases[i].free_user / 1024u, (unsigned)((4440064u + cases[i].weights + 1023u) / 1024u),
                (unsigned)((cases[i].weights + 1023u) / 1024u));
            TEST_CHECK_(strstr(text, want) != NULL, "%s", text);
        }
        mem_free();
    }
    /* A game OOM without the check's numbers gets no specific advice. */
    reset();
    fail_vocr_user = 1; fake_reserve = 0;
    TEST_CHECK(run_vocr_job() == VJO_E_OOM);
    check_oom_message(NULL, "not enough memory", "FL10");
    TEST_CHECK(!strcmp(ocr_error_text(), "Text recognition failed: not enough memory"));
    mem_free();
}

static void test_vocr_oom_statistics_are_cached(void)
{
    reset();
    fail_vocr_user = 1; fake_release = 659456;
    TEST_ASSERT(run_vocr_job() == VJO_E_OOM);
    TEST_CHECK(strstr(ocr_error_text(), "free 4882 + reclaim 644; need 4980") != NULL);
    TEST_CHECK(cache_data[0].err.detail == OOM_GAME_ALLOCATION);
    /* A later transport result cannot overwrite this error's snapshot. */
    memset(&vocr_result, 0xFF, sizeof(vocr_result));
    active = 0;
    view_show_cache();
    TEST_CHECK(strstr(g_view.status, "free 4882 + reclaim 644; need 4980") != NULL);
    TEST_CHECK(strstr(g_view.status, "Try again.") != NULL);
    mem_free();

    /* Bounded output and 64-bit totals even for impossible, oversized reports. */
    reset();
    fail_vocr_user = 1; fake_free_user = INT32_MAX;
    fake_need_weights = fake_reserve = UINT32_MAX;
    TEST_ASSERT(run_vocr_job() == VJO_E_OOM);
    TEST_CHECK(strstr(ocr_error_text(), "need 8388848") != NULL);
    TEST_CHECK(strlen(ocr_error_text()) < sizeof(g_view.status));
    mem_free();
}

static void test_vocr_storage_errors_are_specific(void)
{
    const struct { uint32_t io; const char *message; int specific; } cases[] = {
        {0x80010013u, "storage is unavailable to this game", 1}, /* ENODEV */
        {0x8001000Du, "Access to the local OCR model was denied", 1}, /* EACCES */
        {0x80010002u, "Local OCR model is missing or invalid", 0}, /* ENOENT */
        {0, "Local OCR model is missing or invalid", 0}, /* size/hash/format rejection */
    };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        reset();
        fake_model_io_error = (int32_t)cases[i].io;
        strokes(15, 39);
        TEST_CHECK(run_vocr_job() == VJO_E_OCR_MODEL);
        TEST_CHECK(!!cache_data[0].err.detail == cases[i].specific);
        char memory[256]; VjoArena arena;
        vjo_arena_init(&arena, memory, sizeof(memory));
        const char *message = vjo_err_text(&arena, VJO_STAGE_OCR, &cache_data[0].err);
        TEST_CHECK(strstr(message, cases[i].message) != NULL);
        if (cases[i].specific) TEST_CHECK(strstr(message, "prepare_ocr.py") == NULL);
        /* The default fallback repeats the job once; the fallback's error is shown. */
        TEST_CHECK(!cache_data[0].sentence && vocr_allocs == 2 && vocr_frees == 2);
        mem_free();
    }
    reset();
    cfg.vocr_tier = VJO_VOCR_TIER_SMALL; /* the smallest size: no smaller tier to repeat the job with */
    fake_model_io_error = (int32_t)0x80010013u;
    strokes(15, 39);
    TEST_CHECK(run_vocr_job() == VJO_E_OCR_MODEL && cache_data[0].err.detail && vocr_allocs == 1);
    /* Every size left out in this game: refused before submitting, and the previous job's I/O
     * error is not reused. */
    vocr_unusable = VJO_VOCR_TIERS_ALL; vocr_unusable_generation = job_generation;
    unsigned before = submits;
    TEST_CHECK(run_vocr_job() == VJO_E_OCR_MODEL && !cache_data[0].err.detail && submits == before);
    mem_free();
}

#endif

/* The subtitle strip shows the recognized text alone: its readings came only
 * from the removed online dictionaries (jpdb.io, jiten.moe). Subtitle jobs
 * stay OCR-only while the overlay is closed (no lookup, no result arena for a
 * local dictionary), and a strip's sentence is the job's text as recognized. */
static void test_subtitles_are_ocr_only(void)
{
    reset();
    cfg.ocr_mode = loaded.ocr_mode = VJO_OCR_ON_PRESS;
    subtitles = 1;
    TEST_CHECK(!want_lookup()); /* local dictionary: OCR only while the overlay is closed */
    /* a removed dictionary in config.ini (the online ones, the Hachidori relay) loads as
     * local: still OCR only */
    static const char *const olds[] = {
        "dictionary = jiten\njiten_api_key = test-key\nocr_mode = on_press\n",
        "dictionary = hachidori\nhachidori_host = 192.168.1.2\nocr_mode = on_press\n",
    };
    for (size_t i = 0; i < sizeof(olds) / sizeof(olds[0]); i++) {
        VjoConfig c;
        vjo_config_defaults(&c);
        vjo_config_parse(&c, olds[i], strlen(olds[i]));
        TEST_CHECK(c.dictionary == VJO_DICT_LOCAL && c.n_warnings == 1);
        cfg = loaded = c;
        TEST_CHECK(!want_lookup());
    }
    vjo_strip_configure(1, &cfg);
    vjo_strip_show("雨だ", VJO_STRIP_SENTENCE, 0);
    TEST_CHECK(g_view.strip_on && g_view.strip_kind == VJO_STRIP_SENTENCE && !strcmp(g_view.strip_text, "雨だ"));
    unsigned seq = g_view.strip_text_seq;
    vjo_strip_show("雨だ", VJO_STRIP_SENTENCE, 1); /* the same text: only busy changes */
    TEST_CHECK(g_view.strip_text_seq == seq && g_view.strip_busy);
    vjo_strip_show("次", VJO_STRIP_SENTENCE, 0);
    TEST_CHECK(g_view.strip_text_seq == seq + 1 && !strcmp(g_view.strip_text, "次"));
    subtitles = 0;
    TEST_CHECK(!want_lookup());
    vjo_arena_reset(&scratch);
}

static void test_input_block_denied(void)
{
    reset();
    fail_input_block = 1;
    open_overlay();
    TEST_CHECK(ov == OV_CLOSED && !job_running && !input_blocked && !g_view.open);
    TEST_CHECK(g_view.strip_on && g_view.strip_kind == VJO_STRIP_ERROR);
    fail_input_block = 0;
    open_overlay();
    TEST_CHECK(input_blocked && job_running && !input_block_error);
    job_running = 0;
    mem_free();
}

static void test_disabled_game_retires_work(void)
{
    reset();
    open_overlay();
    TEST_ASSERT(job_running && input_blocked);
    game_enabled = 0;
    on_game_start();
    activate(123, "PCSA00029", VJO_GAME);
    TEST_CHECK(!game_active() && !input_blocked && job_cancelled && !g_view.open);
    job_running = 0;
    mem_free();
}

/* A PSP game's launch as shell/game.c identifies it (its own suite checks
 * how): its key (a bubble's own ID) names the game, the kernel gets its mode
 * (the emulator's static framebuffer), the PSP fallback policy is asked, and
 * Anki records the game's disc ID, its name and the psp platform. */
static void test_psp_launch_uses_its_identity(void)
{
    static const struct { const char *key, *disc, *name, *tid; } games[] = {
        {"ULJS00378", "ULJS00378", "Synthetic PSP Game", "ULJS00378"},
        {"PSPEMU005", "ULJS00385", "Other Synthetic Game", "ULJS00385"},
        {"PSPEMU006", "", "", "PSPEMU006"}};
    for (unsigned i = 0; i < sizeof(games) / sizeof(games[0]); i++) {
        reset();
        memset(&identified, 0, sizeof(identified));
        snprintf(identified.key, sizeof(identified.key), "%s", games[i].key);
        snprintf(identified.disc, sizeof(identified.disc), "%s", games[i].disc);
        snprintf(identified.name, sizeof(identified.name), "%s", games[i].name);
        identified.mode = VJO_GAME_STATIC_FB;
        identified.psp = 1;
        identify_rc = 0;
        active_mode = enabled_psp = -1;
        state.game_pid = 77;
        on_game_start();
        classify_pending();
        TEST_CHECK_(pending_pid == 0 && active_mode == VJO_GAME_STATIC_FB && enabled_psp == 1, "%s", games[i].key);
        TEST_CHECK_(!strcmp(title_id, games[i].key) && !strcmp(anki_tid, games[i].tid) && anki_psp == 1, "%s", games[i].key);
        /* No description: the name comes from the title's app parameters, as for a Vita game. */
        TEST_CHECK_(!strcmp(anki_game, games[i].name[0] ? games[i].name : "Title of PSPEMU006"), "%s", anki_game);
        on_game_exit();
        TEST_CHECK(!game_active() && !anki_tid[0] && !anki_psp);
    }
    /* Turned OFF: input passes through and no game is active. */
    reset();
    game_enabled = 0;
    identify_rc = 0;
    state.game_pid = 78;
    on_game_start();
    classify_pending();
    TEST_CHECK(!game_active() && active_mode == VJO_GAME_NONE && enabled_psp == 1);
    game_enabled = 1;
    /* A Vita game keeps its own mode and no PSP fallback; a title not readable
     * for 3 s is a game named GAME, as before. */
    reset();
    memset(&identified, 0, sizeof(identified));
    memcpy(identified.key, "PCSG00415", 10);
    identified.mode = VJO_GAME;
    state.game_pid = 79;
    on_game_start();
    classify_pending();
    TEST_CHECK(active_mode == VJO_GAME && enabled_psp == 0 && !strcmp(title_id, "PCSG00415") && !anki_psp);
    identify_rc = -1;
    reset();
    state.game_pid = 80;
    on_game_start();
    classify_pending();
    TEST_CHECK(pending_pid == 80);
    test_now += 3100000;
    classify_pending();
    TEST_CHECK(pending_pid == 0 && active_mode == VJO_GAME && !strcmp(title_id, "GAME") && enabled_psp == 0);
    mem_free();
}

static void test_failed_join_keeps_allocations(void)
{
    reset();
    TEST_ASSERT(mem_alloc() == 0);
    uint8_t *base = results[0].base;
    size_t bytes = results[0].size;
    threads_started = 1;
    fail_join = 1;
#ifdef VJO_WITH_VOCR
    total_stops = 0;
#endif
    TEST_CHECK(vjo_worker_stop() < 0);
    TEST_CHECK(has_result_memory() && results[0].base == base && results[0].size == bytes && threads_started == 1);
#ifdef VJO_WITH_VOCR
    TEST_CHECK(!total_stops); /* the control thread may still count: its file stays open */
#endif
    fail_join = 0;
    TEST_CHECK(vjo_worker_stop() == 0);
    TEST_CHECK(!has_result_memory() && threads_started == 0);
#ifdef VJO_WITH_VOCR
    TEST_CHECK(total_stops == 1); /* the reading total's count closes its history.csv */
#endif
}

/* Anki is off by default; anki_mining reaches the overlay with each config
 * reload, nothing about it changes the OCR pipeline, and only with it on
 * does opening the overlay count the offline queue. */
static void test_anki_mining_enables_cross_in_view(void)
{
    reset();
    anki_configured = 0;
    apply_config();
    TEST_CHECK(anki_configured == 1 && !anki_on);
    view_publish(1, NULL, NULL, 0);
    TEST_CHECK(!g_view.anki_enabled);
    apply_config(); /* a reload alone: still off */
    TEST_CHECK(!anki_on);
    open_overlay();
    TEST_CHECK(anki_counts == 0);
    close_overlay();
    loaded.anki_mining = 1;
    cache_ok = 1;
    apply_config();
    TEST_CHECK(anki_on && cache_ok); /* same pipeline: cache kept */
    view_publish(1, NULL, NULL, 0);
    TEST_CHECK(g_view.anki_enabled);
    open_overlay();
    TEST_CHECK(anki_counts == 1);
    close_overlay();
    loaded.anki_mining = 0;
    apply_config();
    view_publish(1, NULL, NULL, 0);
    TEST_CHECK(!g_view.anki_enabled && !anki_on);
}

/* A card that cannot be joined keeps the worker's memory and locks too. */
static void test_anki_stop_failure_keeps_worker(void)
{
    reset();
    TEST_ASSERT(mem_alloc() == 0);
    anki_stop_rc = -1;
    TEST_CHECK(vjo_worker_stop() < 0);
    TEST_CHECK(has_result_memory() && view_lock == 81 && cmd_lock == 82);
    anki_stop_rc = 0;
    TEST_CHECK(vjo_worker_stop() == 0);
    TEST_CHECK(!has_result_memory());
}

#ifdef VJO_WITH_VOCR

/* Tiny generated dictionary exercises actual point-lookup file callbacks.
 * All text below is a hand-written fixture, not game or dictionary data. */
static unsigned char cursor_dict[1024];
static size_t cursor_dict_size;
static unsigned cursor_opens, cursor_reads, cursor_closes;
static int cursor_handle_live, read_action;
static void cursor_u16(unsigned char *p, uint16_t x) { p[0] = x; p[1] = x >> 8; }
static void cursor_u32(unsigned char *p, uint32_t x) { cursor_u16(p, x); cursor_u16(p + 2, x >> 16); }
static void cursor_u64(unsigned char *p, uint64_t x) { cursor_u32(p, x); cursor_u32(p + 4, x >> 32); }
static void cursor_build_dict(void)
{
    const char *keys[] = {"犬", "猫", "鳥"};
    const char *meanings[] = {"dog", "cat", "bird"};
    size_t off = 64 + 3 * 24;
    memset(cursor_dict, 0, sizeof(cursor_dict));
    memcpy(cursor_dict, "VJDICT1\0", 8);
    cursor_u32(cursor_dict + 8, 1); cursor_u32(cursor_dict + 12, 64);
    cursor_u32(cursor_dict + 16, 3); cursor_u32(cursor_dict + 20, 24);
    cursor_u64(cursor_dict + 24, 64); cursor_u64(cursor_dict + 32, off);
    for (int i = 0; i < 3; i++) {
        unsigned char *record = cursor_dict + 64 + 24 * i;
        cursor_u64(record, off); cursor_u16(record + 16, 3);
        memcpy(cursor_dict + off, keys[i], 3); off += 3;
        size_t payload = off;
        const char *parts[] = {keys[i], keys[i], meanings[i]};
        for (int j = 0; j < 3; j++) {
            size_t n = strlen(parts[j]) + 1;
            memcpy(cursor_dict + off, parts[j], n); off += n;
        }
        cursor_u64(record + 8, payload); cursor_u32(record + 20, off - payload);
    }
    cursor_dict_size = off; cursor_u64(cursor_dict + 40, off);
}
static void next_scene(uint32_t scene);
static int read_fail; /* the dictionary's reads fail (a lookup that fails: rc != 0) */
static int cursor_file_read(void *ud, uint64_t off, void *dst, size_t n)
{
    (void)ud;
    cursor_reads++;
    TEST_CHECK(cursor_handle_live == 1);
    if (read_action) {
        int action = read_action, thread = test_thread; read_action = 0;
        test_thread = CTL_THREAD; /* the control thread, while the network thread reads */
        if (action == 1) { vjo_post_cursor(6, local_text_generation); on_command(); }
        if (action == 2) close_overlay();
        if (action == 3) { close_overlay(); open_overlay(); }
        if (action == 4) invalidate_game_work();
        if (action == 5) { state.scene = state.region_scene = 8; state.region_seq++; }
        if (action == 6) { /* line history: Up while an OCR job looks its text up, then a settled new screen */
            control_tick(0); /* the job's text, published (on_job_text) */
            vjo_post_command(VJO_CMD_EARLIER, NULL);
            next_scene(21);
            control_tick(VJO_EV_REGION_STABLE); /* the step (it waits for the job), and the job cancelled */
        }
        test_thread = thread;
    }
    if (read_fail) return -1;
    if (off > cursor_dict_size || n > cursor_dict_size - off) return -1;
    memcpy(dst, cursor_dict + off, n); return 0;
}
static void cursor_file_close(void *ud)
{ (void)ud; TEST_CHECK(cursor_handle_live == 1); cursor_handle_live = 0; cursor_closes++; }
static int cursor_file_open(void *ud, const char *path, VjoFile *out)
{
    (void)ud;
    TEST_CHECK(strstr(path, "main.vjdict") != NULL && !cursor_handle_live);
    cursor_handle_live = 1; cursor_opens++;
    *out = (VjoFile){NULL, cursor_dict_size, cursor_file_read, cursor_file_close, 0}; return 0;
}
static void cursor_setup(void)
{
    reset(); cursor_build_dict();
    cursor_opens = cursor_reads = cursor_closes = 0;
    cursor_handle_live = read_action = read_fail = 0;
    cfg.non_japanese_filter = loaded.non_japanese_filter = VJO_FILTER_NONE;
    cfg.ocr_mode = loaded.ocr_mode = VJO_OCR_AUTO;
    strcpy(cfg.local_dictionaries, "main.vjdict");
    strcpy(loaded.local_dictionaries, "main.vjdict");
    plat.file_open = cursor_file_open;
    fake_recognized_text = "猫犬鳥";
}
static void finish_current_job(void)
{
    TEST_ASSERT(job_running);
    test_thread = NET_THREAD;
    net_phase = job_cursor_only ? NET_RESULT : NET_IDLE; /* a lookup's whole job is its result's path */
#if !VJO_DEBUG
    net_job_begin();
#endif
    net_job(); /* as the network thread: job_idx's slot and arena, the done line, job_done */
    net_phase = NET_IDLE;
#if !VJO_DEBUG
    net_job_end();
#endif
    test_thread = CTL_THREAD;
    TEST_ASSERT(job_done);
    on_job_done();
#if !VJO_DEBUG
    /* the network thread's timed wait ends with no job, the result taken: what is held goes out */
    test_thread = NET_THREAD;
    net_idle_write();
    test_thread = CTL_THREAD;
#endif
}
/* One wakeup of the network thread (net_main's loop) for what was posted since the last: a job, or (the
 * normal build, idle: nothing posted) the end of its timed wait, which writes what is held once no job
 * runs. Both build's waits clear the bits they return (SCE_EVENT_WAITCLEAR_PAT). */
static __attribute__((unused)) void as_network_thread(void)
{
    unsigned bits = posted & (NET_EV_JOB | NET_EV_QUIT);
    posted &= ~(NET_EV_JOB | NET_EV_QUIT);
    test_thread = NET_THREAD;
    if (bits & NET_EV_JOB) {
        net_phase = job_cursor_only ? NET_RESULT : NET_IDLE;
#if !VJO_DEBUG
        net_job_begin();
#endif
        net_job();
        net_phase = NET_IDLE;
#if !VJO_DEBUG
        net_job_end();
#endif
    }
#if !VJO_DEBUG
    else {
        net_idle_write(); /* nothing posted: a timed wait (a line held) timed out; an untimed one sleeps on */
    }
#endif
    test_thread = CTL_THREAD;
}
static void cursor_saved_text(void)
{
    cursor_setup();
    cfg.highlight_mode = loaded.highlight_mode = VJO_HIGHLIGHT_CHARACTER;
    stable_pending = 1; auto_prefetch();
    TEST_ASSERT(job_running && !job_lookup && !job_cursor_only);
    finish_current_job();
    TEST_CHECK(local_text_valid && !strcmp(local_text, "猫犬鳥"));
    TEST_CHECK(!has_result_memory() && !cursor_opens && !user_allocs && !heap_allocs);
    TEST_CHECK(vocr_allocs == 1 && vocr_frees == 1);
}
static void check_current_definition(const char *meaning, uint32_t cursor)
{
    TEST_ASSERT(g_view.list != NULL);
    TEST_CHECK(g_view.local_cursor_mode && g_view.cursor_byte == cursor);
    TEST_CHECK(!strcmp(g_view.list->header, "猫犬鳥"));
    TEST_ASSERT(g_view.list->n_entries == 1);
    TEST_CHECK(strstr(g_view.list->entries[0].text, meaning) != NULL);
    TEST_CHECK(g_view.list->entries[0].hl_start == (int)cursor);
    TEST_CHECK(cursor_opens == cursor_closes + cursor_handle_live && cursor_handle_live <= 1);
    TEST_CHECK(!held_cmd && !held_view);
}
static void test_japanese_only_selection_and_config_reload(void)
{
    cursor_setup();
    TEST_CHECK(!loaded.japanese_only_selection); /* the default, OFF since 2026-10-20: ON from config.ini here */
    cfg.japanese_only_selection = loaded.japanese_only_selection = 1;
    fake_recognized_text = "ABC「猫」 123 한글。犬!";
    open_overlay(); finish_current_job();
    uint32_t first = (uint32_t)(strstr(local_text, "猫") - local_text);
    uint32_t second = (uint32_t)(strstr(local_text, "犬") - local_text);
    TEST_CHECK(g_view.japanese_only_selection && g_view.cursor_byte == first);
    TEST_ASSERT(g_view.list && g_view.list->n_entries == 1);
    TEST_CHECK(strstr(g_view.list->entries[0].text, "cat") != NULL);
    unsigned inferences = vocr_allocs;
    uint32_t generation = local_text_generation;
    vjo_post_cursor(0, generation);
    TEST_CHECK(pending_cmd == VJO_CMD_NONE && g_view.cursor_byte == first);
    vjo_post_cursor(second, generation); on_command(); finish_current_job();
    TEST_CHECK(g_view.cursor_byte == second && vocr_allocs == inferences);
    TEST_ASSERT(g_view.list && g_view.list->n_entries == 1);
    TEST_CHECK(strstr(g_view.list->entries[0].text, "dog") != NULL);
    /* Presentation changes reuse the immutable recognition and lookup.
     * They must not recapture or allocate a second inference workspace. */
    TEST_CHECK(g_view.highlight_mode == VJO_HIGHLIGHT_WORD);
    close_overlay(); loaded.highlight_mode = VJO_HIGHLIGHT_CHARACTER;
    unsigned reads_before = cursor_reads;
    open_overlay();
    TEST_CHECK(g_view.highlight_mode == VJO_HIGHLIGHT_CHARACTER);
    TEST_CHECK(local_text_valid && !job_running && cursor_reads == reads_before);
    TEST_CHECK(local_text_valid && !job_running && vocr_allocs == inferences);
    TEST_CHECK(g_view.cursor_byte == second);
    close_overlay(); loaded.highlight_mode = VJO_HIGHLIGHT_WORD;
    open_overlay();
    TEST_CHECK(g_view.highlight_mode == VJO_HIGHLIGHT_WORD);
    TEST_CHECK(local_text_valid && !job_running && cursor_reads == reads_before);
    TEST_CHECK(local_text_valid && !job_running && vocr_allocs == inferences);
    /* frequency_badge too: the popup's badge follows config.ini, with the
     * same text and lookup (default on, then off, then on again) */
    TEST_CHECK(g_view.frequency_badge == 1);
    close_overlay(); loaded.frequency_badge = 0;
    open_overlay();
    TEST_CHECK(g_view.frequency_badge == 0);
    TEST_CHECK(local_text_valid && !job_running && cursor_reads == reads_before && vocr_allocs == inferences);
    close_overlay(); loaded.frequency_badge = 1;
    open_overlay();
    TEST_CHECK(g_view.frequency_badge == 1 && local_text_valid && !job_running);
    close_overlay(); loaded.japanese_only_selection = 0;
    open_overlay();
    TEST_CHECK(!local_text_valid && job_running && !job_cursor_only);
    finish_current_job();
    TEST_CHECK(!g_view.japanese_only_selection && g_view.cursor_byte == 0);
    generation = local_text_generation;
    vjo_post_cursor(1, generation); on_command(); finish_current_job();
    TEST_CHECK(g_view.cursor_byte == 1);
    close_overlay(); loaded.vocr_tier = VJO_VOCR_TIER_SMALL;
    open_overlay();
    TEST_CHECK(cfg.vocr_tier == VJO_VOCR_TIER_SMALL && job_cfg.vocr_tier == VJO_VOCR_TIER_SMALL);
    TEST_CHECK(!local_text_valid && job_running && !job_cursor_only);
    close_overlay(); finish_current_job(); mem_free();
}

static void test_no_japanese_selection_skips_dictionary_memory_and_io(void)
{
    cursor_setup();
    cfg.japanese_only_selection = loaded.japanese_only_selection = 1; /* ON: OFF, the default, selects any character */
    fake_recognized_text = "\xF0\x83\x81\x82" "ABC 123 한글・「」😀";
    open_overlay(); finish_current_job();
    TEST_CHECK(g_view.local_cursor_mode && g_view.japanese_only_selection);
    TEST_ASSERT(g_view.list != NULL);
    TEST_CHECK(g_view.list->n_entries == 0 && !strcmp(g_view.list->header, fake_recognized_text));
    TEST_CHECK(g_view.cursor_byte == strlen(local_text));
    TEST_CHECK(!cursor_opens && !has_result_memory() && !user_allocs && !heap_allocs);
    vjo_post_cursor(0, local_text_generation);
    TEST_CHECK(pending_cmd == VJO_CMD_NONE && !job_running);
    close_overlay(); open_overlay();
    TEST_CHECK(!job_running && g_view.list && !g_view.list->n_entries);
    TEST_CHECK(!cursor_opens && !has_result_memory() && vocr_allocs == 1);
    close_overlay(); mem_free();
}

/* The overlay's left stick (#138, core/stick_cursor.h) against this worker. The
 * overlay's own lines (shell/overlay.cpp: input_overlay's deferred post, close_page's
 * vjo_stick_cursor_kept, render's new_text and cursor_matches) are paf code no host
 * target compiles; they are restated here around the real helpers and worker. */
typedef struct {
    VjoStickCursor st;
    VjoRowNav nav;               /* s_nav: the arrows' path, which the stick's steps take too */
    uint32_t cursor, generation; /* s_cursor_byte, s_text_generation */
    int local;                   /* s_local_cursor_mode */
    uint64_t now;
} StickOverlay;

/* render: the worker's cursor taken for new text; whether the overlay shows its word */
static int stick_render(StickOverlay *o)
{
    if (!o->local || o->generation != g_view.text_generation) {
        o->local = 1;
        o->generation = g_view.text_generation;
        o->cursor = g_view.cursor_byte;
        vjo_stick_cursor_new_text(&o->st);
    }
    int matches = g_view.cursor_byte == o->cursor;
    vjo_stick_cursor_shown(&o->st, matches);
    return matches;
}

/* render's s_match_end: in Whole word mode, the end of the word the worker shows when it starts at
 * the overlay's cursor (0 for none, a pending lookup or Character mode) */
static size_t stick_shown_end(const StickOverlay *o)
{
    const VjoEntryList *l = g_view.list;
    size_t len = strlen(local_text);
    if (g_view.highlight_mode != VJO_HIGHLIGHT_WORD || g_view.cursor_byte != o->cursor || !l || !l->n_entries)
        return 0;
    const VjoEntry *e = &l->entries[0];
    return e->hl_start == (int)o->cursor && e->hl_end > (int)o->cursor && (size_t)e->hl_end <= len ? (size_t)e->hl_end : 0;
}

/* a frame with the stick at (lx, 128): its step, a Left/Right's, as input_overlay hands it to the
 * arrows' code (the shown word's end and Japanese-only from the view; the stick's left/right ask no
 * layout and go a character a step) */
static void stick_frame(StickOverlay *o, uint8_t lx)
{
    int direction = 0, vertical = 0;
    o->now += 16667u;
    int step = vjo_stick_cursor_step(vjo_stick_cursor(&o->st, lx, 128, o->now), &direction, &vertical);
    if (step) {
        size_t to = o->cursor;
        vjo_text_row_key(&o->nav, local_text, strlen(local_text), NULL, o->cursor, stick_shown_end(o), direction,
                         vertical, g_view.japanese_only_selection, 1, &to);
        vjo_stick_cursor_moved(&o->st, to != o->cursor, o->now);
        if (to != o->cursor) {
            o->cursor = (uint32_t)to;
            stick_render(o);
        }
    }
    if (vjo_stick_cursor_lookup(&o->st, o->now))
        vjo_post_cursor(o->cursor, o->generation);
}

/* close_page, then L + R on the same screen: what the reopened overlay shows */
static int stick_close_and_reopen(StickOverlay *o)
{
    if (ov != OV_CLOSED) { vjo_post_command(VJO_CMD_CLOSED, NULL); on_command(); }
    TEST_CHECK(ov == OV_CLOSED);
    if (!vjo_stick_cursor_kept(&o->st, o->cursor, g_view.cursor_byte))
        o->local = 0;
    open_overlay();
    return stick_render(o);
}

static StickOverlay stick_overlay_on_cat(void)
{
    cursor_saved_text(); open_overlay(); finish_current_job();
    check_current_definition("cat", 0);
    StickOverlay o;
    memset(&o, 0, sizeof(o));
    o.st.rest_us = (uint32_t)g_view.stick_lookup_ms * 1000u; /* render's copy */
    o.now = 5000000u;
    TEST_CHECK(stick_render(&o) && o.cursor == 0);
    stick_frame(&o, 128); /* the first frame at rest: new text latched the stick */
    return o;
}

/* #156's audit B1: a flick from 猫 to 犬, Cancel 183 ms later; the control thread
 * wakes (every 50 ms) after the rest has ended, so the deferred lookup is posted
 * while VJO_CMD_CLOSED is pending and refused, with nothing due any more. The
 * close does not keep the overlay's cursor (it is not the worker's): the
 * reopened overlay shows the worker's word, 猫, not "Looking up words…" with no
 * lookup coming. */
static void test_stick_lookup_refused_by_a_pending_close(void)
{
    StickOverlay o = stick_overlay_on_cat();
    uint32_t generation = local_text_generation;
    stick_frame(&o, 255);
    TEST_CHECK(o.cursor == 3 && o.st.due && !pending_cmd);
    for (int f = 0; f < 10; f++)
        stick_frame(&o, 128);
    TEST_CHECK(o.st.due && pending_cmd == VJO_CMD_NONE);
    vjo_post_command(VJO_CMD_CLOSED, NULL); /* Cancel */
    stick_frame(&o, 128);
    stick_frame(&o, 128); /* the rest ends: posted and refused */
    TEST_CHECK(!o.st.due && o.st.lookups == 1);
    TEST_CHECK(pending_cmd == VJO_CMD_CLOSED && local_cursor_byte == 0 && g_view.cursor_byte == 0);
    on_command(); /* the control thread wakes: closed */
    TEST_CHECK(!vjo_stick_cursor_kept(&o.st, o.cursor, g_view.cursor_byte));
    int shows = stick_close_and_reopen(&o);
    TEST_CHECK_(shows && o.cursor == 0, "reopened: the overlay's cursor %u, the worker's %u", o.cursor, g_view.cursor_byte);
    TEST_CHECK(!job_running && !cursor_waiting && local_text_generation == generation);
    check_current_definition("cat", 0);
    /* and the stick goes on from there: 犬 looked up once it rests */
    stick_frame(&o, 128); /* the latch: let go */
    stick_frame(&o, 255);
    TEST_CHECK(o.cursor == 3 && o.st.due);
    for (int f = 0; f < 13; f++)
        stick_frame(&o, 128);
    TEST_CHECK(pending_cmd == VJO_CMD_CURSOR && g_view.cursor_byte == 3);
    on_command(); finish_current_job();
    check_current_definition("dog", 3);
    close_overlay(); mem_free();
}

/* The other closes: Cancel before the rest ends (the word still due), and the
 * deferred lookup accepted with Cancel in the same frame (its command
 * overwritten by VJO_CMD_CLOSED): the reopened overlay shows or looks up the
 * cursor's word either way. */
static void test_stick_close_before_and_after_the_rest(void)
{
    StickOverlay o = stick_overlay_on_cat();
    stick_frame(&o, 255);
    stick_frame(&o, 128);
    TEST_CHECK(o.cursor == 3 && o.st.due);
    TEST_CHECK(stick_close_and_reopen(&o) && o.cursor == 0); /* due at the close: the worker's 猫 */
    check_current_definition("cat", 0);
    /* accepted, then Cancel in the same frame */
    stick_frame(&o, 128);
    stick_frame(&o, 255);
    for (int f = 0; f < 12 && o.st.due; f++)
        stick_frame(&o, 128);
    TEST_CHECK(!o.st.due && pending_cmd == VJO_CMD_CURSOR && g_view.cursor_byte == 3);
    TEST_CHECK(vjo_stick_cursor_kept(&o.st, o.cursor, g_view.cursor_byte));
    vjo_post_command(VJO_CMD_CLOSED, NULL);
    on_command();
    TEST_CHECK(stick_close_and_reopen(&o) && o.cursor == 3); /* kept: the worker's too, its lookup on its way */
    TEST_CHECK(job_running && job_cursor_only && job_cursor_byte == 3);
    finish_current_job();
    TEST_CHECK(stick_render(&o));
    check_current_definition("dog", 3);
    close_overlay(); mem_free();
}

/* config.ini's stick_lookup_ms reaches the overlay through the view (#156's audit W1/W2). */
static void test_stick_lookup_ms_reaches_the_view(void)
{
    cursor_setup();
    TEST_CHECK(loaded.stick_lookup_ms == 200);
    open_overlay(); finish_current_job();
    TEST_CHECK(g_view.stick_lookup_ms == 200);
    close_overlay();
    loaded.stick_lookup_ms = 0;
    open_overlay();
    TEST_CHECK(g_view.stick_lookup_ms == 0);
    close_overlay();
    loaded.stick_lookup_ms = 1000;
    open_overlay();
    TEST_CHECK(g_view.stick_lookup_ms == 1000 && g_view.frequency_badge == 1 && g_view.font_size_en == 14);
    close_overlay(); mem_free();
}

static void test_character_open_reuses_background_text(void)
{
    cursor_saved_text();
    uint32_t generation = local_text_generation;
    open_overlay();
    TEST_ASSERT(job_running && job_cursor_only);
    TEST_CHECK(!cursor_opens && vocr_allocs == 1);
    finish_current_job();
    check_current_definition("cat", 0);
    TEST_CHECK(local_text_generation == generation && vocr_allocs == 1);
    TEST_CHECK(results[0].size + vjo_local_session_size() == 64 * 1024 && !results[1].size);

    /* Opening between recognition and completion starts only point lookup.
     * The real mutex stubs reject recursively reacquiring cmd_lock. */
    cursor_setup(); start_job("background still finishing", 0);
    TEST_CHECK(run_job_t(&results[0], &cache_data[0]) == VJO_OK);
    open_overlay(); TEST_CHECK(cursor_waiting && job_running && !job_cursor_only);
    on_job_done(); TEST_ASSERT(job_running && job_cursor_only);
    finish_current_job(); check_current_definition("cat", 0);
    TEST_CHECK(vocr_allocs == 1);
}

static void test_word_background_lookup_and_reopen_cache(void)
{
    cursor_setup();
    TEST_CHECK(want_lookup());
    cfg.ocr_mode = loaded.ocr_mode = VJO_OCR_ON_PRESS;
    TEST_CHECK(!want_lookup());
    cfg.ocr_mode = loaded.ocr_mode = VJO_OCR_AUTO;
    stable_pending = 1; auto_prefetch();
    TEST_ASSERT(job_running && job_lookup && !job_cursor_only && ov == OV_CLOSED);
    finish_current_job();
    TEST_CHECK(cache_ok && active == 0 && !g_view.open && !g_view.list);
    TEST_CHECK(results[0].size + vjo_local_session_size() == 64 * 1024 && !results[1].size);
    unsigned reads_before = cursor_reads, inferences = vocr_allocs;
    uint32_t generation = local_text_generation;
    open_overlay();
    TEST_CHECK(!job_running && cursor_reads == reads_before && vocr_allocs == inferences);
    check_current_definition("cat", 0);
    close_overlay(); open_overlay();
    TEST_CHECK(!job_running && cursor_reads == reads_before && local_text_generation == generation);
    check_current_definition("cat", 0);
    /* A cursor posted before its job starts must exclude the old cache. */
    vjo_post_cursor(3, generation);
    close_overlay(); open_overlay();
    TEST_ASSERT(job_running && job_cursor_only && !g_view.list->n_entries);
    finish_current_job(); check_current_definition("dog", 3);
    close_overlay();
    state.scene = state.region_scene = 8; state.region_seq++;
    open_overlay();
    TEST_ASSERT(job_running && !job_cursor_only && !g_view.list);
    finish_current_job(); check_current_definition("cat", 0);
    TEST_CHECK(local_text_generation != generation && vocr_allocs == inferences + 1);
    close_overlay(); mem_free();

    cursor_setup(); stable_pending = 1; auto_prefetch();
    TEST_ASSERT(job_running && job_lookup);
    TEST_CHECK(run_job_t(&results[0], &cache_data[0]) == VJO_OK);
    reads_before = cursor_reads;
    open_overlay(); /* lookup finished, completion has not been processed */
    TEST_CHECK(job_running && !cursor_waiting && !job_cursor_only);
    job_done = 1; on_job_done();
    TEST_CHECK(!job_running && cursor_reads == reads_before && vocr_allocs == 1);
    check_current_definition("cat", 0);
    close_overlay(); mem_free();
}

/* The Shell's word lookup end to end with the bundled plaintext.vjdict
 * (VJO_PLAINTEXT_DIR; skipped without it) and a real VOCR line
 * (host/fixtures/word_lookup, vita_tight_v13090_154662 H15): Auto OCR
 * prepares the first word while the overlay is closed, opening shows it with
 * no further file I/O, Right (the word step) and a tap each look one word up,
 * and every lookup writes its log line. */
static unsigned real_opens;
static int (*real_open)(void *, const char *, VjoFile *);
static VjoFile real_file;
/* As platform_vita.c's local_read: the lookup log's read count. */
static int counting_read(void *ctx, uint64_t off, void *dst, size_t n)
{ vjo_dict_reads++; vjo_dict_read_bytes += (uint32_t)n; return real_file.read(real_file.ctx, off, dst, n); }
static void counting_close(void *ctx) { real_file.close(real_file.ctx); }
static int counting_open(void *ud, const char *path, VjoFile *out)
{
    real_opens++;
    if (real_open(ud, path, &real_file)) return -1;
    *out = (VjoFile){NULL, real_file.size, counting_read, counting_close, real_file.stamp};
    return 0;
}
static const char *lookup_line(const char *why)
{
    for (unsigned i = log_count > 32 ? log_count - 32 : 0; i < log_count; i++)
        if (!strncmp(log_lines[i % 32], "lookup ", 7) && strstr(log_lines[i % 32], why)) return log_lines[i % 32];
    return NULL;
}
static void test_word_lookup_with_the_bundled_dictionary(void)
{
    const char *dir = getenv("VJO_PLAINTEXT_DIR");
    PosixPlatform pp = {0};
    VjoPlatform posix;
    if (!dir) { printf("  skipped: VJO_PLAINTEXT_DIR unset\n"); return; }
    cursor_setup();
    posix_platform_init(&pp, &posix);
    real_open = posix.file_open; real_opens = 0;
    plat.file_open = counting_open;
    snprintf(loaded.local_dictionary_dir, sizeof(loaded.local_dictionary_dir), "%s", dir);
    strcpy(loaded.local_dictionaries, "plaintext.vjdict");
    VjoConfig defaults;
    vjo_config_defaults(&defaults);
    loaded.non_japanese_filter = defaults.non_japanese_filter; /* the release default */
    cfg = loaded;
    fake_recognized_text = "伊佐那社\nふふ、春休み入ってから\nほとんど会ってなかったからね。\nふたりばデート？";
    TEST_CHECK(cfg.highlight_mode == VJO_HIGHLIGHT_WORD && want_lookup());
    stable_pending = 1; auto_prefetch(); /* Auto OCR on a settled screen, overlay closed */
    TEST_ASSERT(job_running && job_lookup && ov == OV_CLOSED);
    finish_current_job();
    TEST_CHECK(cache_ok && real_opens == 1);
#if VJO_DEBUG /* the lookup lines: the debug build's (the normal build logs a failed lookup only) */
    const char *first = lookup_line("first word at 0: ");
    TEST_ASSERT_(first != NULL, "no first-word lookup log line");
    TEST_CHECK(!strncmp(first, "lookup first word at 0: 2 chars, dictionary 1/1, page 1/1, ", 59));
    /* The entry's dictionary is the file's title (page 0): Bee's Ultimate
     * Dictionary's definitions carry no source name of their own. */
    TEST_CHECK(strstr(first, " ms; \"伊佐那社\" -> 伊佐 (いさ) [Bee's Ultimate Dictionary]") != NULL &&
               strstr(first, ", rc=0, ") != NULL);
    unsigned logged_reads = 0;
    TEST_CHECK(sscanf(first + 59, "%u reads", &logged_reads) == 1 && logged_reads > 0);
#else
    TEST_CHECK(!lookup_line("first word at 0: "));
#endif
    /* Opening shows the prepared word at once: no job, no file opened. */
    open_overlay();
    TEST_CHECK(!job_running && real_opens == 1);
    TEST_ASSERT(g_view.list && g_view.list->n_entries == 1);
    TEST_CHECK(!strcmp(g_view.list->entries[0].vocab->spelling, "伊佐"));
    TEST_CHECK(g_view.list->entries[0].hl_start == 0 && g_view.list->entries[0].hl_end == (int)strlen("伊佐"));
    /* Right: the word step past 伊佐 (shell/word_navigation.h) looks up 那. */
    uint32_t generation = local_text_generation;
    vjo_post_cursor((uint32_t)strlen("伊佐"), generation); on_command(); finish_current_job();
    TEST_ASSERT(g_view.list && g_view.list->n_entries == 1);
    TEST_CHECK(g_view.cursor_byte == strlen("伊佐") && real_opens == 1); /* Auto retains its text's session */
#if VJO_DEBUG
    TEST_CHECK(lookup_line("cursor at 6: 1 chars, dictionary 1/1"));
    /* The bundle lists its one 那 article (Daijirin4's) under な and だ; Jiten
     * ranks 那 only read だ (#38948), so the frequency tie-break shows だ.
     * 伊佐那社 is いさな: neither reading is this line's, and both entries carry
     * the same article. */
    TEST_CHECK(lookup_line("\"那社\" -> 那 (だ) [Bee's Ultimate Dictionary] #38948"));
#endif
    TEST_CHECK(!strcmp(g_view.list->entries[0].vocab->spelling, "那"));
    /* A tap on 会 looks up the inflected word: 会ってなかった -> 会う. */
    uint32_t at = (uint32_t)(strstr(local_text, "会って") - local_text);
    vjo_post_cursor(at, generation); on_command(); finish_current_job();
    TEST_ASSERT(g_view.list && g_view.list->n_entries == 1);
    const VjoEntry *e = &g_view.list->entries[0];
    TEST_CHECK(!strcmp(e->vocab->spelling, "会う") && e->hl_start == (int)at &&
               e->hl_end == (int)(at + strlen("会ってなかった")));
    /* the bundled dictionary's frequency: 会う's Jiten rank, its list named */
    TEST_CHECK(e->vocab->rank == 134 && g_view.list->source && !strcmp(g_view.list->source, "Jiten"));
#if VJO_DEBUG
    const char *tap = lookup_line("会ってなかった");
    TEST_ASSERT(tap != NULL);
    TEST_CHECK(strstr(tap, ": 7 chars, dictionary 1/1, page 1/1, ") &&
               strstr(tap, "-> 会う (あう) [Bee's Ultimate Dictionary] #134"));
    TEST_CHECK(e->vocab->dictionary && !strcmp(e->vocab->dictionary, "Bee's Ultimate Dictionary") &&
               !strstr(e->vocab->meanings[0], "Plain]"));
    /* a name Jiten does not rank is logged so */
    TEST_CHECK(strstr(first, "-> 伊佐 (いさ) [Bee's Ultimate Dictionary] unranked") != NULL);
    printf("\n  %s\n  %s\n  %s\n", first, lookup_line("cursor at 6: "), tap);
#else
    TEST_CHECK(!lookup_line("会ってなかった")); /* the normal build logs no lookup's text */
#endif
    close_overlay(); mem_free();
}

/* The left stick a character at a time with this worker and the bundled dictionary (the owner,
 * 2026-10-20 08:37Z: on 昨日すき焼きを食べました, from 食べました to ました), Japanese-only on and
 * off: Auto OCR prepares 昨日; the d-pad's Rights look each word up at once, to 食べました (食べる);
 * the stick's right from it is べ, looked up once the cursor rests, from that character (べ), and
 * again ま (ました: the longest match starting there), each shown with its header range, the
 * Whole word highlight, starting at the cursor. Selection never reruns OCR: one recognition, the
 * text's generation kept, no new result memory and the dictionary opened once. */
static void test_stick_steps_characters_with_the_bundled_dictionary(void)
{
    const char *dir = getenv("VJO_PLAINTEXT_DIR");
    if (!dir) { printf("  skipped: VJO_PLAINTEXT_DIR unset\n"); return; }
    for (int jp = 0; jp <= 1; jp++) {
        PosixPlatform pp = {0};
        VjoPlatform posix;
        TEST_CASE_("Japanese-only %d", jp);
        cursor_setup();
        posix_platform_init(&pp, &posix);
        real_open = posix.file_open; real_opens = 0;
        plat.file_open = counting_open;
        snprintf(loaded.local_dictionary_dir, sizeof(loaded.local_dictionary_dir), "%s", dir);
        strcpy(loaded.local_dictionaries, "plaintext.vjdict");
        loaded.japanese_only_selection = jp; /* both values, whatever the default */
        loaded.highlight_mode = VJO_HIGHLIGHT_WORD;
        cfg = loaded;
        fake_recognized_text = "昨日すき焼きを食べました"; /* 食 21, べ 24, ま 27 */
        stable_pending = 1; auto_prefetch();
        TEST_ASSERT(job_running && job_lookup && ov == OV_CLOSED);
        finish_current_job();
        open_overlay();
        TEST_ASSERT(!job_running && g_view.list && g_view.list->n_entries == 1 && g_view.japanese_only_selection == jp);
        unsigned inferences = vocr_allocs, users = user_allocs, heaps = heap_allocs;
        uint32_t generation = local_text_generation;
        StickOverlay o;
        memset(&o, 0, sizeof(o));
        o.st.rest_us = (uint32_t)g_view.stick_lookup_ms * 1000u;
        o.now = 5000000u;
        TEST_CHECK(stick_render(&o) && o.cursor == 0);
        stick_frame(&o, 128); /* new text latched the stick: let go */
        static const char *const heads[] = {"昨日", "すき焼き", "を", "食べる", "べ", "ました"};
        static const uint32_t at[] = {0, 6, 18, 21, 24, 27}, ends[] = {6, 18, 21, 36, 27, 36};
        for (int i = 0; i < 6; i++) {
            if (i && i < 4) { /* the d-pad's Right: the arrows' code, the word skipped, looked up at once */
                size_t to = o.cursor;
                vjo_text_row_key(&o.nav, local_text, strlen(local_text), NULL, o.cursor, stick_shown_end(&o), 1, 0,
                                 g_view.japanese_only_selection, 0, &to);
                o.cursor = (uint32_t)to;
                o.st.due = 0; /* post_cursor_now */
                vjo_post_cursor(o.cursor, o.generation);
            } else if (i) { /* the stick pushed right once, then let go: looked up once it rests */
                stick_frame(&o, 255);
                TEST_CHECK(o.cursor == at[i] && o.st.due && pending_cmd == VJO_CMD_NONE);
                for (int f = 0; f < 30 && pending_cmd == VJO_CMD_NONE; f++)
                    stick_frame(&o, 128);
                TEST_CHECK(!o.st.due && o.st.lookups == i - 3);
            }
            if (i) {
                TEST_ASSERT(pending_cmd == VJO_CMD_CURSOR && g_view.cursor_byte == at[i]);
                on_command(); finish_current_job();
            }
            TEST_CHECK(stick_render(&o) && o.cursor == at[i]);
            TEST_ASSERT_(g_view.list && g_view.list->n_entries == 1, "no entry at %u", at[i]);
            const VjoEntry *e = &g_view.list->entries[0];
            uint32_t hs = 0, hl = 0;
            TEST_CHECK_(!strcmp(e->vocab->spelling, heads[i]) && e->hl_start == (int)at[i] && e->hl_end == (int)ends[i],
                        "at %u: %s [%d, %d), expected %s [%u, %u)", at[i], e->vocab->spelling, e->hl_start, e->hl_end,
                        heads[i], at[i], ends[i]);
            TEST_CHECK(vjo_entry_header_range(g_view.list, 0, &hs, &hl) && hs == at[i] / 3 && hl == (ends[i] - at[i]) / 3);
        }
        TEST_CHECK(o.nav.words.count == 5 && o.st.moves == 2 && o.st.lookups == 2);
        TEST_CHECK_(vocr_allocs == inferences && local_text_generation == generation && user_allocs == users &&
                    heap_allocs == heaps && real_opens == 1,
                    "OCR %u -> %u, generation %u -> %u, allocations %u/%u -> %u/%u, %u opens", inferences, vocr_allocs,
                    generation, local_text_generation, users, heaps, user_allocs, heap_allocs, real_opens);
        close_overlay(); mem_free();
    }
}
#if VJO_DEBUG /* the debug build's log lines (the normal build's: test_normal_build_*) */
/* vjo_log keeps 254 bytes of a line. The lookup line's numbers come first and
 * its texts are bounded in whole characters, so even with every field at its
 * largest (4-byte characters, eight dictionaries, a 4-digit cursor, the
 * largest rank, a dictionary name of 31 bytes or more) nothing is cut: the
 * line ends with the frequency badge. The name is the file's title (63 bytes
 * at most, 31 logged) or, without one, a definition's "[name]" line. */
static void test_lookup_log_line_fits_with_worst_case_fields(void)
{
    static char text[VJO_LOCAL_MAX_TEXT];
    static char name[40 * 4 + 1];
    static const char title[] = "A_dictionary_title_of_the_longest_kind:_a_page-0_field_holds_63";
    static const char label[] = "[A legacy label of 31 bytes, max]\ndefinition";
    const char *meaning = "definition";
    VjoVocab v; VjoEntry e; VjoLocalPages pages; VjoOverlayData out;
    reset();
    TEST_ASSERT(sizeof(title) == 64); /* the longest a page-0 field holds */
    memset(text, 'a', 3900); text[3900] = 0;
    for (int i = 0; i < 40; i++) strcat(text, "\xF0\xA0\xAE\xB7");
    name[0] = 0;
    for (int i = 0; i < 40; i++) strcat(name, "\xF0\xA0\xAE\xB7");
    memset(&v, 0, sizeof(v)); memset(&e, 0, sizeof(e)); memset(&pages, 0, sizeof(pages)); memset(&out, 0, sizeof(out));
    v.spelling = name; v.reading = name + 4; v.meanings = &meaning; v.n_meanings = 1;
    v.rank = 16777215; /* the format's largest */
    v.dictionary = title;
    e.vocab = &v; e.hl_start = 3900; e.hl_end = 3900 + 32 * 4;
    pages.count = 8; pages.page[7].dict = 7;
    out.sentence = text; out.list.entries = &e; out.list.n_entries = 1; out.pages = &pages; out.page = 7;
    out.list.source = "Jiten";
    strcpy(job_cfg.local_dictionaries, "a1.vjdict,a2.vjdict,a3.vjdict,a4.vjdict,a5.vjdict,a6.vjdict,a7.vjdict,a8.vjdict");
    vjo_dict_reads = 99999; vjo_dict_read_bytes = 9999999;
    log_lookup("first word", &out, 3900, -115, (int64_t)test_now - 99999000);
    const char *line = lookup_line("first word at 3900: 32 chars, dictionary 8/8, page 8/8, 99999 reads 9999999 B, rc=-115, 99999 ms; ");
    TEST_ASSERT(line != NULL);
    TEST_CHECK_(strlen(line) <= 254 && strlen(line) > 10 && !strcmp(line + strlen(line) - 10, " #16777215"),
                "%zu bytes: %s", strlen(line), line);
    TEST_CHECK(strstr(line, " [A_dictionary_title_of_the_longe] #16777215") != NULL);
    printf("\n  worst case: %zu of 254 bytes\n", strlen(line));
    /* a file without a title: its definition's label line, as long */
    v.dictionary = NULL;
    meaning = label;
    log_count = 0; /* only the next line */
    log_lookup("first word", &out, 3900, -115, (int64_t)test_now - 99999000);
    line = lookup_line("first word at 3900: 32 chars, dictionary 8/8, page 8/8, 99999 reads 9999999 B, rc=-115, 99999 ms; ");
    TEST_ASSERT(line != NULL);
    TEST_CHECK_(strlen(line) <= 254 && strstr(line, " [A legacy label of 31 bytes, max] #16777215") != NULL,
                "%zu bytes: %s", strlen(line), line);
}
#endif
static void test_character_coalesces_and_drops_inflight_definition(void)
{
    cursor_saved_text(); open_overlay(); finish_current_job();
    uint32_t generation = local_text_generation;
    unsigned reads_before = cursor_reads;
    vjo_post_cursor(3, generation); vjo_post_cursor(6, generation); vjo_post_cursor(0, generation);
    TEST_CHECK(cursor_reads == reads_before && pending_cmd == VJO_CMD_CURSOR);
    TEST_CHECK(g_view.cursor_byte == 0 && !g_view.list->n_entries);
    on_command(); TEST_ASSERT(job_running && job_cursor_byte == 0);
    finish_current_job(); check_current_definition("cat", 0);

    vjo_post_cursor(3, generation); on_command();
    read_action = 1; /* a later move arrives at an actual file-read boundary */
    finish_current_job();
    TEST_ASSERT(job_running && job_cursor_only && job_cursor_byte == 6);
    TEST_CHECK(g_view.cursor_byte == 6 && !g_view.list->n_entries);
    finish_current_job(); check_current_definition("bird", 6);
    TEST_CHECK(local_text_generation == generation && vocr_allocs == 1);
}
static void test_character_close_reopen_and_retirement(void)
{
    cursor_saved_text(); open_overlay(); finish_current_job();
    uint32_t generation = local_text_generation;
    vjo_post_cursor(3, generation); on_command(); read_action = 3;
    finish_current_job();
    TEST_ASSERT(job_running && job_cursor_only && !job_cancelled && ov == OV_OPEN);
    TEST_CHECK(!g_view.list->n_entries);
    finish_current_job(); check_current_definition("dog", 3);
    TEST_CHECK(vocr_allocs == 1 && local_text_generation == generation);

    vjo_post_cursor(6, generation); on_command(); read_action = 2;
    finish_current_job();
    TEST_CHECK(ov == OV_CLOSED && !job_running && !g_view.list);
    unsigned n = cursor_opens;
    vjo_post_cursor(0, generation); on_command();
    TEST_CHECK(!job_running && cursor_opens == n && !g_view.list);
    open_overlay(); finish_current_job();
    vjo_post_cursor(3, generation); on_command(); read_action = 4;
    finish_current_job();
    TEST_CHECK(!local_text_valid && !g_view.list && !job_running);
    TEST_CHECK(game_generation != job_generation && local_text_generation != generation);
    TEST_CHECK(vocr_allocs == 1 && retirement_checks > 0);
}
/* #81 request 4: the region's scene moving on (an icon's mask ending, an
 * effect; in the test build's console log one open overlay saw ten new scene
 * numbers while the game's input was blocked) never retires the shown text or shows a
 * message, before a lookup or during it, and a settled screen meanwhile
 * does not cancel the lookup: it is looked up in the shown text and shown.
 * Reopening on the changed screen recognizes it again, as before. A region
 * change still retires the text: the user drew another region. */
static void test_character_scene_change_keeps_the_shown_text(void)
{
    for (int during_read = 0; during_read < 3; during_read++) {
        cursor_saved_text(); open_overlay(); finish_current_job();
        uint32_t generation = local_text_generation;
        int retired = retirement_checks;
        vjo_post_cursor(3, generation);
        if (!during_read) { state.scene = state.region_scene = 8; state.region_seq++; }
        on_command();
        TEST_ASSERT(job_running && job_cursor_only);
        if (during_read == 1) read_action = 5;
        if (during_read == 2) {
            state.scene = state.region_scene = 8; state.region_seq++;
            control_tick(VJO_EV_REGION_STABLE); /* a settled screen: the lookup is not cancelled */
            TEST_CHECK(job_running && job_cursor_only && !job_cancelled);
        }
        finish_current_job();
        check_current_definition("dog", 3);
        TEST_CHECK(local_text_valid && local_text_generation == generation && vocr_allocs == 1);
        TEST_CHECK(!g_view.status_is_error && !strstr(g_view.status, "changed") && retirement_checks == retired);
        vjo_post_cursor(6, generation); on_command(); finish_current_job();
        check_current_definition("bird", 6); /* still usable */
        close_overlay(); open_overlay();
        TEST_CHECK(job_running && !job_cursor_only && !g_view.list);
        job_running = 0;
    }
    static const int region_cmds[] = {VJO_CMD_SET_REGION, VJO_CMD_CLEAR_REGION};
    for (unsigned k = 0; k < 2; k++) {
        int cmd = region_cmds[k];
        cursor_saved_text(); open_overlay(); finish_current_job();
        uint32_t generation = local_text_generation;
        VjoRect r = {2, 4, 100, 50};
        vjo_post_command(cmd, &r); on_command();
        /* Kernel may retain scene ID after a region reset. */
        TEST_CHECK(state.scene == 2 && !local_text_valid && ov == OV_CLOSED);
        TEST_CHECK(local_text_generation != generation && !g_view.list);
        open_overlay(); TEST_CHECK(job_running && !job_cursor_only && !g_view.list);
        job_running = 0;
    }
}
/* #358, the text box suggested on first use: a job on the whole screen of a game with no region line at all puts the
 * band its result carries in the view once the job is done (on_job_done), the newest screen's: one without dialogue
 * takes an earlier screen's out. A game with a region line (its own, or region.ini's "full" for every game: the Rubi
 * app's Suggest a text box OFF) gets none, nor one whose box the overlay took in this activation
 * (core/region_select.h VJO_RSEL_TAKEN), until it next starts or resumes (push_region). The overlay's enter saves the
 * box as a drawn one is (VJO_CMD_SET_REGION): the overlay closes, and the game has its region. */
static void test_the_suggested_box_reaches_the_view_once_an_activation(void)
{
    const VjoRect r = vjo_rsel_rect(0, 544 - 169, 960, 169);
    unsigned scene = 3;
    cursor_setup();
    saved_region.w = 0;
    push_region();
    TEST_CHECK(g_view.suggest == 0);
    fake_suggest_rows = 169;
    cfg.highlight_mode = loaded.highlight_mode = VJO_HIGHLIGHT_CHARACTER;
#define NEXT_SCREEN() /* Auto's job on a newly settled screen, the overlay closed */ \
    do { \
        state.scene = state.capture_scene = state.region_scene = scene++; \
        stable_pending = 1; auto_prefetch(); \
        TEST_ASSERT(job_running); \
        finish_current_job(); \
    } while (0)
    NEXT_SCREEN();
    TEST_CHECK_(g_view.suggest == 169, "%u", g_view.suggest);
    /* a later screen without dialogue: its job's result has no box, and takes the earlier screen's out */
    fake_suggest_rows = 0;
    NEXT_SCREEN();
    TEST_CHECK_(g_view.suggest == 0, "%u", g_view.suggest);
    /* the next screen's box; the overlay takes it (shell/overlay.cpp): none after it in this activation */
    fake_suggest_rows = 170;
    NEXT_SCREEN();
    TEST_CHECK(g_view.suggest == 170 && vjo_rsel_take(&g_view.suggest) == 170);
    fake_suggest_rows = 169;
    NEXT_SCREEN();
    TEST_CHECK(g_view.suggest == VJO_RSEL_TAKEN && !vjo_rsel_take(&g_view.suggest));
    /* the game resumes (activate_game's push_region): its next box is offered again */
    push_region();
    TEST_CHECK(g_view.suggest == 0);
    NEXT_SCREEN();
    TEST_CHECK(g_view.suggest == 169);
    /* a game with a region of its own: none, and its jobs' boxes are not taken */
    saved_region = (VjoRect){100, 200, 30000, 8000};
    push_region();
    TEST_CHECK(g_view.suggest == VJO_RSEL_TAKEN);
    NEXT_SCREEN();
    TEST_CHECK(g_view.suggest == VJO_RSEL_TAKEN);
    /* region.ini's line for every game ("region = full"): none either */
    saved_region.w = 0;
    saved_region_all_full = 1;
    push_region();
    TEST_CHECK(g_view.suggest == VJO_RSEL_TAKEN && logged("region: all games', full screen"));
    NEXT_SCREEN();
    TEST_CHECK(g_view.suggest == VJO_RSEL_TAKEN);
    saved_region_all_full = 0;
    /* the box taken with the overlay open and saved by its enter, as a drawn one is: the overlay closes, the game's
     * region is the box, none after it */
    push_region();
    NEXT_SCREEN();
    open_overlay();
    if (job_running) finish_current_job();
    TEST_CHECK(ov == OV_OPEN && vjo_rsel_take(&g_view.suggest) == 169);
    region_saves = 0;
    saved_region = r; /* what region.ini holds once saved (the stub's save keeps no file) */
    vjo_post_command(VJO_CMD_SET_REGION, &r); on_command();
    TEST_CHECK(region_saves == 1 && !memcmp(&region_saved_rect, &r, sizeof(r)) && logged("region set for PCSG00415"));
    TEST_CHECK(ov == OV_CLOSED && !__atomic_load_n(&g_view.open, __ATOMIC_ACQUIRE));
    TEST_CHECK(g_view.suggest == VJO_RSEL_TAKEN); /* push_region: the box is this game's region now */
    NEXT_SCREEN();
    TEST_CHECK(g_view.suggest == VJO_RSEL_TAKEN);
    /* a region.ini that cannot be written: the game keeps its whole screen (no line), and is not asked again in this
     * activation */
    saved_region.w = 0;
    push_region();
    NEXT_SCREEN();
    TEST_CHECK(vjo_rsel_take(&g_view.suggest) == 169);
    region_save_failure = 1;
    vjo_post_command(VJO_CMD_SET_REGION, &r); on_command();
    TEST_CHECK(region_saves == 2 && logged("region.ini: write failed") && g_view.suggest == VJO_RSEL_TAKEN);
    NEXT_SCREEN();
    TEST_CHECK(g_view.suggest == VJO_RSEL_TAKEN);
    region_save_failure = 0;
#undef NEXT_SCREEN
    fake_suggest_rows = 0;
    saved_region.w = 0;
}

static void test_character_command_priority_and_generation_boundaries(void)
{
    cursor_saved_text(); open_overlay(); finish_current_job();
    uint32_t generation = local_text_generation;
    unsigned n = cursor_opens;
    vjo_post_cursor(1, generation); vjo_post_cursor(9, generation);
    vjo_post_cursor(0, generation + 1);
    TEST_CHECK(pending_cmd == VJO_CMD_NONE && !job_running && cursor_opens == n);
    const int commands[] = {VJO_CMD_CLOSED, VJO_CMD_SET_REGION, VJO_CMD_CLEAR_REGION};
    for (size_t i = 0; i < sizeof(commands)/sizeof(commands[0]); i++) {
        vjo_post_command(commands[i], NULL);
        vjo_post_cursor(3, generation);
        TEST_CHECK(pending_cmd == commands[i] && g_view.cursor_byte == 0);
        pending_cmd = VJO_CMD_NONE;
    }
    local_cursor_seq = UINT32_MAX;
    vjo_post_cursor(3, generation); TEST_CHECK(local_cursor_seq == 1);
    on_command(); finish_current_job(); check_current_definition("dog", 3);
    local_text_generation = UINT32_MAX;
    retire_local_text(1);
    TEST_CHECK(local_text_generation == 1 && !local_text_valid && !g_view.list);
    vjo_post_cursor(0, generation); TEST_CHECK(pending_cmd == VJO_CMD_NONE);
    TEST_CHECK(retirement_checks > 0 && !held_cmd && !held_view);
}

/* Three generated dictionaries for paging through the actual worker. The
 * definitions name their dictionary; third.vjdict has no 犬. */
#define PAGED_DICTS 3
static unsigned char paged_dict[PAGED_DICTS][1024];
static size_t paged_size[PAGED_DICTS];
static int paged_index[PAGED_DICTS] = {0, 1, 2};
static unsigned paged_opens[PAGED_DICTS], paged_reads, paged_live;
static int paged_action; /* page + 1 to request during the next read */
static const char *const paged_names[PAGED_DICTS] = {"main.vjdict", "extra.vjdict", "third.vjdict"};
static void paged_build(int index)
{
    const char *keys[] = {"犬", "猫", "鳥"};
    const char *animals[] = {"dog", "cat", "bird"};
    unsigned char *d = paged_dict[index];
    int first = index == 2 ? 1 : 0, n = 3 - first;
    size_t off = 64 + (size_t)n * 24;
    memset(d, 0, sizeof(paged_dict[index]));
    memcpy(d, "VJDICT1\0", 8);
    cursor_u32(d + 8, 1); cursor_u32(d + 12, 64); cursor_u32(d + 16, (uint32_t)n); cursor_u32(d + 20, 24);
    cursor_u64(d + 24, 64); cursor_u64(d + 32, off);
    for (int i = 0; i < n; i++) {
        unsigned char *record = d + 64 + 24 * i;
        char meaning[32];
        snprintf(meaning, sizeof(meaning), "%.5s %s", paged_names[index], animals[first + i]);
        cursor_u64(record, off); cursor_u16(record + 16, 3);
        memcpy(d + off, keys[first + i], 3); off += 3;
        size_t payload = off;
        const char *parts[] = {keys[first + i], keys[first + i], meaning};
        for (int j = 0; j < 3; j++) {
            size_t len = strlen(parts[j]) + 1;
            memcpy(d + off, parts[j], len); off += len;
        }
        cursor_u64(record + 8, payload); cursor_u32(record + 20, (uint32_t)(off - payload));
    }
    paged_size[index] = off; cursor_u64(d + 40, off);
}
static int paged_read(void *ud, uint64_t off, void *dst, size_t n)
{
    int i = *(int *)ud;
    paged_reads++;
    TEST_CHECK(paged_live & (1u << i));
    if (paged_action) {
        int page = paged_action - 1; paged_action = 0;
        TEST_CHECK(vjo_post_page(page, local_text_generation)); on_command();
    }
    if (off > paged_size[i] || n > paged_size[i] - off) return -1;
    memcpy(dst, paged_dict[i] + off, n); return 0;
}
static void paged_close(void *ud)
{ int i = *(int *)ud; TEST_CHECK(paged_live & (1u << i)); paged_live &= ~(1u << i); }
static int paged_open(void *ud, const char *path, VjoFile *out)
{
    (void)ud;
    for (int i = 0; i < PAGED_DICTS; i++) {
        if (!strstr(path, paged_names[i])) continue;
        TEST_CHECK(!(paged_live & (1u << i)));
        paged_live |= 1u << i; paged_opens[i]++;
        *out = (VjoFile){&paged_index[i], paged_size[i], paged_read, paged_close, 0};
        return 0;
    }
    return -1;
}
static void paged_setup(int results)
{
    cursor_setup();
    for (int i = 0; i < PAGED_DICTS; i++) paged_build(i);
    memset(paged_opens, 0, sizeof(paged_opens));
    paged_reads = paged_live = 0; paged_action = 0;
    strcpy(cfg.local_dictionaries, "main.vjdict,extra.vjdict,third.vjdict");
    strcpy(loaded.local_dictionaries, cfg.local_dictionaries);
    cfg.local_dictionary_results = loaded.local_dictionary_results = results;
    plat.file_open = paged_open;
}
static void check_page(const char *meaning, uint32_t cursor, int page, int pages)
{
    TEST_ASSERT(g_view.list != NULL && g_view.list->n_entries == 1);
    TEST_CHECK(strstr(g_view.list->entries[0].text, meaning) != NULL);
    TEST_MSG("shown: %s; expected %s", g_view.list->entries[0].text, meaning);
    TEST_CHECK(g_view.local_cursor_mode && g_view.cursor_byte == cursor);
    TEST_CHECK(g_view.dict_page == page && g_view.dict_pages == pages);
    TEST_MSG("page %d/%d, expected %d/%d", g_view.dict_page, g_view.dict_pages, page, pages);
    TEST_CHECK(!held_cmd && !held_view && !job_running);
    TEST_CHECK(ov != OV_CLOSED || !paged_live);
}
static unsigned paged_total_opens(void) { return paged_opens[0] + paged_opens[1] + paged_opens[2]; }

static void test_dictionary_pages_read_one_entry_each(void)
{
    paged_setup(3);
    open_overlay(); finish_current_job();
    check_page("main. cat", 0, 0, 3);
    TEST_CHECK(paged_opens[0] == 1 && paged_opens[1] == 1 && paged_opens[2] == 1);
    TEST_CHECK(results[0].size + vjo_local_session_size() == 64 * 1024 && !results[1].size && vocr_allocs == 1);
    unsigned arenas = user_allocs + heap_allocs;
    size_t first_peak = results[0].peak;
    uint32_t generation = local_text_generation;
    /* Down: reuse extra.vjdict's handle at its located payload. */
    TEST_CHECK(vjo_post_page(1, generation));
    TEST_CHECK(pending_cmd == VJO_CMD_CURSOR && g_view.list == &local_header);
    TEST_CHECK(g_view.dict_page == 1 && g_view.dict_pages == 3 && !strcmp(g_view.status, "Looking up words…"));
    on_command(); TEST_ASSERT(job_running && job_cursor_only && job_page == 1);
    unsigned reads_before = paged_reads;
    finish_current_job();
    check_page("extra cat", 0, 1, 3);
    TEST_CHECK(paged_opens[0] == 1 && paged_opens[1] == 1 && paged_opens[2] == 1);
    TEST_CHECK(paged_reads - reads_before == 1); /* the session already validated its header */
    /* Ignored: the current page, out of range, a stale text. */
    TEST_CHECK(!vjo_post_page(1, generation) && !vjo_post_page(3, generation));
    TEST_CHECK(!vjo_post_page(-1, generation) && !vjo_post_page(2, generation + 1));
    TEST_CHECK(pending_cmd == VJO_CMD_NONE && !job_running);
    /* Rapid presses coalesce into the newest page. */
    TEST_CHECK(vjo_post_page(2, generation) && vjo_post_page(0, generation));
    on_command(); TEST_ASSERT(job_page == 0); finish_current_job();
    check_page("main. cat", 0, 0, 3);
    TEST_CHECK(vjo_post_page(2, generation)); on_command(); finish_current_job();
    check_page("third cat", 0, 2, 3);
    /* A press during a page read supersedes that page. */
    TEST_CHECK(vjo_post_page(1, generation)); on_command(); finish_current_job();
    check_page("extra cat", 0, 1, 3);
    TEST_CHECK(vjo_post_page(2, generation)); on_command();
    paged_action = 0 + 1; /* while reading page 2, ask for page 0 */
    finish_current_job();
    TEST_ASSERT(job_running && job_cursor_only && job_page == 0);
    finish_current_job(); check_page("main. cat", 0, 0, 3);
    TEST_CHECK(vjo_post_page(1, generation)); on_command(); finish_current_job();
    check_page("extra cat", 0, 1, 3);
    /* Paging never reallocates, recaptures, or needs lookup scratch. */
    TEST_CHECK(user_allocs + heap_allocs == arenas && vocr_allocs == 1);
    TEST_CHECK(results[0].peak <= first_peak && results[0].size + vjo_local_session_size() == 64 * 1024 && !results[1].size);
    TEST_CHECK(local_text_generation == generation);

    /* A new character starts at its best match; pages come with its result. */
    vjo_post_cursor(3, generation);
    TEST_CHECK(g_view.dict_page == 0 && g_view.dict_pages == 0 && !vjo_post_page(1, generation));
    on_command(); finish_current_job();
    check_page("main. dog", 3, 0, 2); /* third.vjdict has no 犬 */
    TEST_CHECK(vjo_post_page(1, generation)); on_command(); finish_current_job();
    check_page("extra dog", 3, 1, 2);
    /* Reopening unchanged dialogue shows the same page without reads. */
    close_overlay();
    TEST_CHECK(!paged_live); /* the shown entry can stay cached; its files cannot */
    reads_before = paged_reads; unsigned opens_before = paged_total_opens();
    open_overlay();
    TEST_CHECK(!job_running && paged_reads == reads_before && paged_total_opens() == opens_before);
    check_page("extra dog", 3, 1, 2);
    /* Closing during a page read drops it; reopening reads that page again. */
    TEST_CHECK(vjo_post_page(0, generation)); on_command();
    TEST_ASSERT(job_running); close_overlay(); TEST_CHECK(job_cancelled);
    job_rc = run_cursor_job_t(&results[0], &cache_data[0]); job_done = 1; on_job_done();
    TEST_CHECK(!job_running && !g_view.list && !paged_live);
    open_overlay(); TEST_ASSERT(job_running && job_cursor_only && job_page == 0);
    finish_current_job(); check_page("main. dog", 3, 0, 2);
    TEST_CHECK(vocr_allocs == 1 && user_allocs + heap_allocs == arenas);
    close_overlay(); mem_free();
}

static void test_dictionary_count_change_relooks_up_without_ocr(void)
{
    paged_setup(3);
    open_overlay(); finish_current_job();
    check_page("main. cat", 0, 0, 3);
    uint32_t generation = local_text_generation;
    TEST_CHECK(vjo_post_page(2, generation)); on_command(); finish_current_job();
    check_page("third cat", 0, 2, 3);
    /* Settings → Dictionaries shown 1: the same text, looked up again. */
    close_overlay(); loaded.local_dictionary_results = 1;
    open_overlay();
    TEST_CHECK(local_text_valid && local_text_generation == generation);
    TEST_ASSERT(job_running && job_cursor_only && job_page == 0);
    unsigned opens = paged_total_opens();
    finish_current_job();
    check_page("main. cat", 0, 0, 1);
    TEST_CHECK(!cache_data[0].pages && paged_total_opens() - opens == 3);
    TEST_CHECK(!vjo_post_page(1, generation) && pending_cmd == VJO_CMD_NONE);
    /* Back to 2: two pages again, still without OCR or recapture. */
    close_overlay(); loaded.local_dictionary_results = 2;
    open_overlay(); finish_current_job();
    check_page("main. cat", 0, 0, 2);
    TEST_CHECK(vocr_allocs == 1 && local_text_generation == generation);
    /* An unchanged count keeps the cached page. */
    TEST_CHECK(vjo_post_page(1, generation)); on_command(); finish_current_job();
    close_overlay(); open_overlay();
    TEST_CHECK(!job_running); check_page("extra cat", 0, 1, 2);
    /* A prepared lookup made with the old count is replaced on opening. */
    close_overlay(); retire_local_text(1); cache_ok = 0; /* as a region change */
    stable_pending = 1; auto_prefetch();
    TEST_ASSERT(job_running && job_lookup && !job_cursor_only);
    TEST_CHECK(run_job_t(&results[0], &cache_data[0]) == VJO_OK);
    loaded.local_dictionary_results = 3;
    open_overlay(); /* reloads config while the prepared lookup finishes */
    job_done = 1; on_job_done();
    TEST_ASSERT(job_running && job_cursor_only);
    finish_current_job(); check_page("main. cat", 0, 0, 3);
    /* One finishing after a reload (e.g. subtitles on) with another count is
     * not cached for the next opening. */
    close_overlay(); retire_local_text(1); cache_ok = 0;
    stable_pending = 1; auto_prefetch();
    TEST_ASSERT(job_running && job_lookup && job_cfg.local_dictionary_results == 3);
    TEST_CHECK(run_job_t(&results[0], &cache_data[0]) == VJO_OK);
    loaded.local_dictionary_results = 2; apply_config();
    job_done = 1; on_job_done();
    TEST_CHECK(!job_running && !cache_ok);
    open_overlay(); TEST_ASSERT(job_running && job_cursor_only);
    finish_current_job(); check_page("main. cat", 0, 0, 2);
    TEST_CHECK(vocr_allocs == 3);
    close_overlay(); mem_free();
}

static void test_dictionary_page_after_replaced_file_locates_again(void)
{
    paged_setup(3);
    open_overlay(); finish_current_job();
    check_page("main. cat", 0, 0, 3);
    uint32_t generation = local_text_generation;
    /* End the session before replacing a dictionary (installation requires
     * closed games). Cached page locators must still reject its new size. */
    vjo_local_session_close(plat.local_session);
    paged_size[1] += 1; cursor_u64(paged_dict[1] + 40, paged_size[1]);
    TEST_CHECK(vjo_post_page(1, generation)); on_command(); finish_current_job();
    TEST_ASSERT(g_view.list && !g_view.list->n_entries && !strcmp(g_view.list->header, "猫犬鳥"));
    TEST_CHECK(g_view.status_is_error && strstr(g_view.status, "changed") != NULL);
    TEST_CHECK(g_view.dict_page == 1 && g_view.dict_pages == 3 && !paged_live);
    /* The next page locates every dictionary again instead of trusting old offsets. */
    unsigned opens = paged_total_opens();
    TEST_CHECK(vjo_post_page(2, generation)); on_command(); finish_current_job();
    TEST_CHECK(paged_total_opens() - opens == 3); /* three to locate; the page borrows its handle */
    check_page("third cat", 0, 2, 3);
    TEST_CHECK(vjo_post_page(1, generation)); on_command(); finish_current_job();
    check_page("extra cat", 0, 1, 3);
    TEST_CHECK(vocr_allocs == 1 && local_text_generation == generation);
    close_overlay(); mem_free();
}

static void test_dictionary_pages_oom_keeps_text_and_disables_paging(void)
{
    paged_setup(3);
    fail_alloc = 1;
    open_overlay(); finish_current_job();
    TEST_ASSERT(g_view.list && !g_view.list->n_entries && !strcmp(g_view.list->header, "猫犬鳥"));
    TEST_CHECK(g_view.status_is_error && strstr(g_view.status, "Shell UI memory"));
    TEST_CHECK(!g_view.dict_pages && !paged_total_opens() && !has_result_memory());
    TEST_CHECK(!vjo_post_page(1, local_text_generation));
    fail_alloc = 0;
    vjo_post_cursor(3, local_text_generation); on_command(); finish_current_job();
    check_page("main. dog", 3, 0, 2);
    close_overlay(); mem_free();
}

static void test_shell_result_oom_preserves_recognition(void)
{
    reset();
    cfg.non_japanese_filter = VJO_FILTER_LINES;
    fake_recognized_text = "HUD only\r\n  日本語です  \r\nEnglish only";
    subtitles = 1;
    ov = OV_OPEN;
    fail_alloc = 1;
    backoff_note(&ocr_backoff, 1); /* previous OCR failure must be cleared */
    start_job("Shell result refusal after recognition", 1);
    int idx = job_idx;
    TEST_CHECK(run_job_t(&results[idx], &cache_data[idx]) == VJO_E_OOM);
    VjoOverlayData *d = &cache_data[idx];
    TEST_CHECK(d->failed_stage == VJO_STAGE_DICT && d->err.dict == VJO_DICT_LOCAL);
    TEST_CHECK(d->ocr_text && !strcmp(d->ocr_text, fake_recognized_text));
    TEST_CHECK(d->sentence && !strcmp(d->sentence, "日本語です"));
    TEST_CHECK(d->list.header && !strcmp(d->list.header, "日本語です"));
    TEST_CHECK(!d->list.n_entries && !d->list.entries);
    TEST_CHECK(!has_result_memory() && !vocr_live && vocr_allocs == vocr_frees);
    on_job_done();
    TEST_CHECK(!ocr_backoff.until && !ocr_backoff.us);
    TEST_CHECK(dict_backoff.until > now_us() && dict_backoff.us == BACKOFF_MIN_US);
    TEST_CHECK(g_view.status_is_error && strstr(g_view.status, "Shell UI memory"));
    TEST_CHECK(!strstr(g_view.status, "Text recognition failed") && !strstr(g_view.status, "region"));
    TEST_CHECK(g_view.list == &d->list && !strcmp(g_view.list->header, "日本語です"));
    TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !strcmp(g_view.strip_text, "日本語です"));
    TEST_CHECK(!g_view.strip_busy && !job_running && !cache_ok);

    /* A new job removes every published fallback pointer before reuse. */
    close_overlay(); /* background OCR-only jobs run with the overlay closed */
    fail_alloc = 0;
    fake_recognized_text = "次の文";
    start_job("Shell memory recovered", 0);
    TEST_CHECK(!g_view.list && !has_result_memory());
    TEST_CHECK(run_job_t(&results[job_idx], &cache_data[job_idx]) == VJO_OK);
    on_job_done();
    TEST_CHECK(!strcmp(g_view.strip_text, "次の文") && !ocr_backoff.until);
    TEST_CHECK(!has_result_memory() && vocr_allocs == vocr_frees);
    mem_free();
    TEST_CHECK(!has_result_memory());
}

static void test_shell_result_oom_keeps_max_worker_text(void)
{
    char longest[VJO_GAME_OCR_TEXT_BYTES];
    for (size_t i = 0; i < sizeof(longest) - 1; i += 3)
        memcpy(longest + i, "語", 3);
    longest[sizeof(longest) - 1] = 0;
    for (int mode = VJO_FILTER_NONE; mode <= VJO_FILTER_LINES; mode++) {
        reset();
        cfg.non_japanese_filter = mode;
        fake_recognized_text = longest;
        fail_alloc = 1;
        start_job("maximum recognized text under Shell pressure", 1);
        TEST_CHECK(run_job_t(&results[job_idx], &cache_data[job_idx]) == VJO_E_OOM);
        VjoOverlayData *d = &cache_data[job_idx];
        TEST_CHECK(d->failed_stage == VJO_STAGE_DICT);
        TEST_CHECK(d->sentence && !strcmp(d->sentence, longest));
        TEST_CHECK(d->list.header && !strcmp(d->list.header, longest));
        TEST_CHECK(!has_result_memory() && !vocr_live && vocr_allocs == vocr_frees);
        job_running = 0;
    }
}

static void test_local_phases_and_recovery(void)
{
    reset();
    phases_checked = 1;
    for (int i = 0; i < 50; i++) {
        start_job("local phase check", 0);
        TEST_CHECK(!has_result_memory() && g_view.list == NULL);
        TEST_CHECK(run_job_t(&results[job_idx], &cache_data[job_idx]) == VJO_OK);
        TEST_CHECK(!has_result_memory() && !results[0].size && !results[1].size);
        TEST_CHECK(!user_allocs && !heap_allocs);
        TEST_CHECK(vocr_allocs == vocr_frees && !vocr_live);
        job_running = 0;
    }
    fail_vocr_user = fail_vocr_paf = 1;
    start_job("local allocation refusal", 0);
    TEST_CHECK(run_job_t(&results[job_idx], &cache_data[job_idx]) == VJO_E_OOM);
    TEST_CHECK(!has_result_memory() && !vocr_live);
    job_running = 0;
    fail_vocr_user = fail_vocr_paf = 0;
    start_job("local recovered", 0);
    TEST_CHECK(run_job_t(&results[job_idx], &cache_data[job_idx]) == VJO_OK);
    job_running = 0;
    mem_free();
    TEST_CHECK(!has_result_memory() && user_allocs == user_frees && heap_allocs == heap_frees);
    phases_checked = 0;
}

static void test_local_lookup_single_arena(void)
{
    cursor_setup(); /* This allocation gate needs an actual selectable sentence. */
    phases_checked = 1;
    ov = OV_OPEN;
    start_job("local lookup", 1);
    TEST_CHECK(job_idx == 0 && !g_view.list);
    TEST_CHECK(run_job_t(&results[job_idx], &cache_data[job_idx]) == VJO_OK);
    TEST_CHECK(has_result_memory() && results[0].size + vjo_local_session_size() == 64 * 1024);
    TEST_CHECK(!results[1].base && !results[1].size);
    on_job_done();
    TEST_CHECK(cache_ok && active == 0 && g_view.list == &cache_data[0].list);
    start_job("local OCR only replaces cached lookup", 0);
    TEST_CHECK(job_idx == 0 && !has_result_memory() && !g_view.list && !cache_ok);
    TEST_CHECK(run_job_t(&results[job_idx], &cache_data[job_idx]) == VJO_OK);
    TEST_CHECK(!has_result_memory());
    job_running = 0;
    phases_checked = 0;
    mem_free();
}

#endif

/* ---- the Shell worker's one result arena (audit-overlay-shell-slim-1's A1, A1b, A4, A5) ---- */
static void next_scene(uint32_t scene);
static void check_shown(const char *text, const char *gloss, uint32_t cursor);
#ifdef VJO_WITH_VOCR
static unsigned audit_live(void) { return heap_allocs + user_allocs - heap_frees - user_frees; }

/* A1: a game exit while the first open's lookup still runs. The job owns the
 * arena it took after OCR: the exit never frees it under the job; once the job
 * ends, with no game active, the same control tick frees it. */
static void test_audit_exit_during_lookup_frees_the_arena_after_the_job(void)
{
    cursor_setup();
    open_overlay();
    TEST_ASSERT(job_running && job_lookup && !job_cursor_only && !job_refresh && job_idx == 0);
    TEST_CHECK(!has_result_memory()); /* opening takes no memory */
    /* the network thread: OCR in the game, then the lookup's arena */
    job_rc = run_job_t(&results[job_idx], &cache_data[job_idx]);
    TEST_CHECK(job_rc == VJO_OK && has_result_memory() && results[0].size + vjo_local_session_size() == LOCAL_RESULT_ARENA_SIZE);
    on_game_exit(); /* before the control thread saw job_done */
    TEST_CHECK(has_result_memory() && audit_live() == 1); /* still the job's */
    job_done = 1;
    control_tick(0); /* the job's end, then the idle check */
    TEST_CHECK(!job_running && !has_result_memory() && audit_live() == 0);
    TEST_CHECK(!held_cmd && !held_view);
}

/* A1b: the same with a cursor job (a word lookup in the shown text). */
static void test_audit_exit_during_cursor_job_frees_the_arena_after_the_job(void)
{
    cursor_setup();
    open_overlay(); finish_current_job();
    TEST_ASSERT(has_result_memory() && cache_ok && active == 0);
    vjo_post_cursor(3, local_text_generation);
    on_command();
    TEST_ASSERT(job_running && job_cursor_only);
    job_rc = run_cursor_job_t(&results[0], &cache_data[0]);
    on_game_exit();
    TEST_CHECK(has_result_memory() && audit_live() == 1);
    job_done = 1;
    control_tick(0);
    TEST_CHECK(!job_running && !has_result_memory() && audit_live() == 0);
    TEST_CHECK(!held_cmd && !held_view);
}

/* A4: a refresh (job_idx 1) has no arena of its own: results[1] stays {NULL, 0},
 * the job allocates nothing, and the shown entry's arena is left as it was. */
static void test_audit_refresh_has_no_arena(void)
{
    cursor_setup();
    open_overlay(); finish_current_job();
    TEST_ASSERT(has_result_memory() && active == 0 && cache_ok);
    uint8_t *base = results[0].base;
    size_t used = results[0].used;
    unsigned allocs = heap_allocs + user_allocs;
    next_scene(8);
    control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh && job_idx == 1 && !job_lookup);
    TEST_CHECK(!results[1].base && !results[1].size);
    job_rc = run_job_t(&results[1], &cache_data[1]);
    TEST_CHECK(job_rc == VJO_OK && !results[1].base && !results[1].size && !results[1].used && !results[1].peak);
    TEST_CHECK(results[0].base == base && results[0].used == used && heap_allocs + user_allocs == allocs);
    TEST_CHECK(cache_data[1].ocr_text != NULL && !cache_data[1].list.n_entries && !cache_data[1].pages);
    job_done = 1;
    on_job_done();
    TEST_CHECK(DLOGGED("refresh rc=0: same text") && has_result_memory() && results[0].base == base);
    TEST_CHECK(!held_cmd && !held_view);
    close_overlay(); mem_free();
}
#endif

#ifdef VJO_WITH_VOCR
/* A5: on_job_done's publish path when the overlay's thread posts a cursor move
 * just before on_job_done takes cmd_lock (vjo_post_cursor runs on Paf's thread,
 * under cmd_lock): the newer lookup runs, with cmd_lock released first. The
 * lock stub fires cursor_race at that acquisition. */
static void test_audit_cursor_posted_while_publishing(void)
{
    cursor_setup();
    open_overlay();
    TEST_ASSERT(job_running && job_lookup && !job_cursor_only);
    job_rc = run_job_t(&results[0], &cache_data[0]);
    job_done = 1;
    uint32_t seq = local_cursor_seq;
    race_on_lock = 1; /* the first cmd_lock taken from here: on_job_done's publish */
    on_job_done();
    TEST_CHECK(race_on_lock == 0 && local_cursor_seq != seq);
    TEST_CHECK(!held_cmd && !held_view);
    TEST_ASSERT(job_running && job_cursor_only && job_cursor_byte == 3);
    finish_current_job();
    check_shown("猫犬鳥", "dog", 3);
    TEST_CHECK(!held_cmd && !held_view);
    close_overlay(); mem_free();
}
#endif

static unsigned times_logged_prefix(const char *prefix)
{
    unsigned n = 0;
    for (unsigned i = log_count > 32 ? log_count - 32 : 0; i < log_count; i++)
        if (!strncmp(log_lines[i % 32], prefix, strlen(prefix))) n++;
    return n;
}

/* VjoNet's stack (16 KiB since Google Lens's removal) is measured on the console:
 * a game exit logs the most the thread has used, once per growth. The arithmetic,
 * on a buffer the test paints itself; the paint itself on a real thread: below. */
static void test_net_stack_high_water_is_logged_once_per_growth(void)
{
    reset();
    net_stack_check();
    TEST_CHECK(!logged("VjoNet stack: at least")); /* nothing painted, nothing claimed */
    /* painted below a frame at 12,000 B: nothing used below it yet */
    memset(fake_stack, 0, sizeof(fake_stack));
    stack_paint(fake_stack, 12000, fake_stack + sizeof(fake_stack));
    int painted = 1;
    for (unsigned i = 0; i < 12000; i++) painted &= fake_stack[i] == STACK_PAINT;
    TEST_CHECK(painted && fake_stack[12000] == 0 && net_stack_painted == fake_stack + 12000);
    TEST_CHECK(stack_high_water() == sizeof(fake_stack) - 12000);
    /* a job's frames reached 6,000 B from the bottom: 10,384 B used; logged once */
    memset(fake_stack + 6000, 0x11, 64);
    TEST_CHECK(stack_high_water() == sizeof(fake_stack) - 6000);
    log_count = 0;
    on_game_exit();
    TEST_CHECK(logged("VjoNet stack: at least 10384 of 16384 B used") != NULL);
    on_game_exit();
    TEST_CHECK(times_logged_prefix("VjoNet stack: at least") == 1); /* no growth, no line */
    fake_stack[100] = 0; /* deeper, to 100 B from the bottom: logged again */
    net_stack_check();
    TEST_CHECK(logged("VjoNet stack: at least 16284 of 16384 B used") && times_logged_prefix("VjoNet stack: at least") == 2);
    fake_stack[0] = 0; /* the whole stack */
    TEST_CHECK(stack_high_water() == sizeof(fake_stack));
    /* the budget the ledger counts is the stack this measures */
    TEST_CHECK(VJO_STACK_SHELL_NET == 16384u && VJO_BUDGET_SHELL_NET_STACK == 16384u + VJO_LEDGER_TLS);
}

/* The paint on a real thread whose 16 KiB stack is a known buffer between two
 * guard areas, as the kernel reports it (pStack its bottom, stackSize its size):
 * the stack's lowest 64 B stay as they were (the PSP's thread manager keeps the
 * thread's UID there, audit-overlay-shell-slim-2c B2), [pStack + 64, painted) is
 * the pattern, the 64 B just above it and both guards are
 * untouched, the painting frame is above the STACK_PAINT_GAP bytes, the thread
 * runs on, and a deeper use written later is read back. The thread first clears
 * its stack below its own frame: what ran there before the start routine (glibc
 * 2.39 and later name the thread's stack with snprintf in start_thread) is left
 * behind, and where the 64 B window falls in it depends on the compiler
 * (audit-overlay-shell-slim-2c B1). Only the gap's bottom is checked: its top
 * holds what the paint's own calls write there, return addresses and saved
 * registers, and under PIE such an address can hold the pattern byte
 * (audit-overlay-shell-slim-2b B2). Misreports paint nothing and log their own
 * line, once (audit-overlay-shell-slim-2 B1, -2b B1). */
#define PAINT_STACK 16384u
#define PAINT_GUARD 8192u /* 4 KiB low is not enough: glibc puts the TCB at the top of a setstack stack */
#define PAINT_GAP 512u /* STACK_PAINT_GAP as documented: not taken from the code it checks */
#define PAINT_FLOOR 64u /* STACK_PAINT_FLOOR as documented: the stack's lowest bytes are never painted */
static uint8_t paint_region[PAINT_GUARD + PAINT_STACK + PAINT_GUARD] __attribute__((aligned(4096)));
static uint8_t paint_snap[PAINT_STACK];
static uintptr_t paint_local_after, paint_deepest;
static uintptr_t paint_frame_seen; /* the painting frame, from mode 5's refusal line */
static int paint_depth;
static __attribute__((noinline)) void paint_deep(int n)
{
    volatile uint8_t buf[512];
    for (unsigned i = 0; i < sizeof(buf); i++) buf[i] = (uint8_t)(i % 100 + 1); /* 1..100: never the pattern */
    if ((uintptr_t)buf < paint_deepest) paint_deepest = (uintptr_t)buf;
    if (n > 0) paint_deep(n - 1);
    buf[0] ^= 1;
}
static void *paint_thread(void *arg)
{
    volatile int local_after = 1;
    uint8_t *lo = paint_region + PAINT_GUARD;
    (void)arg;
    /* the background below this frame; the 256 B just under it stay for memset's own */
    memset(lo, 0, (size_t)((uint8_t *)(current_sp() - 256) - lo));
    net_stack_paint();
    memcpy(paint_snap, paint_region + PAINT_GUARD, PAINT_STACK); /* right after the paint */
    paint_local_after = (uintptr_t)&local_after;
    if (paint_depth) paint_deep(paint_depth);
    return NULL;
}
/* mode 0: as documented; 1: pStack = the top; 2: size 0; 3: pStack in the guard below, with a
 * size that keeps the frame inside (only the size check refuses it); 4: no info; 5: pStack 416 B
 * below the stack pointer at the call, the exact size: the frame lies above the unpainted floor and
 * more than half the gap above it but within STACK_PAINT_GAP, so only the full gap term of the lower
 * bound refuses it; 6: pStack 8 KiB low, the exact size (only the upper bound refuses it: the frame
 * is above the reported top); 7: pStack 16,448 B below the SP, the exact size: the frame lies less
 * than 1 KiB above the reported top, so only the upper bound refuses it (audit-overlay-shell-slim-2c
 * N14); 8 and 9 take the painting frame F from mode 5's line (the same stack and call path in every
 * mode) and pin both bounds to the byte, F on each bound's edge: 8, pStack = F - 576, so F is
 * pStack + the floor + the gap, which the lower bound's <= refuses (a bound one byte short paints
 * 0 B and publishes lo); 9, pStack = F - 16,384, so F is the reported top, which the upper bound's
 * >= refuses (one byte long paints from below the real stack, into the guard)
 * (audit-overlay-shell-slim-2d N1, -4 N2) */
#define PAINT_MODES 9
static const char *const paint_refusal[PAINT_MODES + 1] = {
    NULL, "VjoNet stack: not painted (frame 0x", "VjoNet stack: not painted (the kernel reports 0 B, not 16384)",
    "VjoNet stack: not painted (the kernel reports 24576 B, not 16384)", "VjoNet stack: not painted (no thread info)",
    "VjoNet stack: not painted (frame 0x", "VjoNet stack: not painted (frame 0x", "VjoNet stack: not painted (frame 0x",
    "VjoNet stack: not painted (frame 0x", "VjoNet stack: not painted (frame 0x"};
static int paint_on_a_thread(int mode, int depth)
{
    uint8_t *lo = paint_region + PAINT_GUARD;
    pthread_attr_t at;
    pthread_t t;
    reset();
    memset(paint_region, 0x5A, sizeof(paint_region)); /* the guards */
    memset(lo, 0, PAINT_STACK);
    paint_depth = depth;
    paint_deepest = UINTPTR_MAX;
    thread_info_stack = mode == 1 ? lo + PAINT_STACK : mode == 3 ? lo - PAINT_GUARD : mode == 6 ? lo - 8192
                      : mode == 8 ? (uint8_t *)(paint_frame_seen - (PAINT_FLOOR + PAINT_GAP))
                      : mode == 9 ? (uint8_t *)(paint_frame_seen - PAINT_STACK) : lo;
    thread_info_size = mode == 2 ? 0 : mode == 3 ? (int)(PAINT_STACK + PAINT_GUARD) : (int)PAINT_STACK;
    thread_info_rc = mode == 4 ? -1 : 0;
    thread_info_below_sp = mode == 5 ? 416 : mode == 7 ? 16448 : 0;
    log_count = 0;
    TEST_ASSERT(pthread_attr_init(&at) == 0 && pthread_attr_setstack(&at, lo, PAINT_STACK) == 0);
    TEST_ASSERT(pthread_create(&t, &at, paint_thread, NULL) == 0 && pthread_join(t, NULL) == 0);
    pthread_attr_destroy(&at);
    int guards = 1;
    for (unsigned i = 0; i < PAINT_GUARD; i++)
        guards &= paint_region[i] == 0x5A && paint_region[PAINT_GUARD + PAINT_STACK + i] == 0x5A;
    TEST_CHECK_(guards, "mode %d: a guard area was written", mode);
    if (mode) {
        /* the stack's lower half, which the thread's own frames never reach, still as it was */
        int touched = 0;
        for (unsigned i = 0; i < PAINT_STACK / 2; i++) touched += paint_snap[i] != 0;
        TEST_CHECK_(!__atomic_load_n(&net_stack_lo, __ATOMIC_ACQUIRE) && !touched,
                    "mode %d (a misreport): %d bytes of the lower half written, lo %p", mode, touched, (void *)net_stack_lo);
        /* its own refusal, and only that line */
        const char *line = log_count ? log_lines[(log_count - 1) % 32] : "";
        TEST_CHECK_(log_count == 1 && !strncmp(line, paint_refusal[mode], strlen(paint_refusal[mode])),
                    "mode %d: %u line(s), the last '%s'", mode, log_count, line);
        if (mode >= 5) { /* the frame the line names is where this mode puts it */
            unsigned frame = 0, low = 0;
            int size = 0;
            TEST_ASSERT(sscanf(line, "VjoNet stack: not painted (frame 0x%X outside 0x%X + %d)", &frame, &low, &size) == 3);
            uint32_t above = (uint32_t)(frame - low);
            if (mode == 5) /* the frame's full address: the 32-bit line's offset from lo */
                paint_frame_seen = (uintptr_t)lo + (uint32_t)(frame - (uint32_t)(uintptr_t)lo);
            if (mode >= 8)
                TEST_CHECK_((uintptr_t)lo + (uint32_t)(frame - (uint32_t)(uintptr_t)lo) == paint_frame_seen,
                            "mode %d: the frame moved", mode);
            TEST_CHECK_(mode == 8   ? above == PAINT_FLOOR + PAINT_GAP
                        : mode == 9 ? above == (uint32_t)size
                        : mode == 5 ? above > PAINT_FLOOR + PAINT_GAP / 2 && above <= PAINT_FLOOR + PAINT_GAP
                        : mode == 7 ? above >= (uint32_t)size && above < (uint32_t)size + 1024
                                    : above >= (uint32_t)size,
                        "mode %d: the frame is %u B above the reported pStack (size %d)", mode, (unsigned)above, size);
        }
        TEST_CHECK(!stack_high_water());
        return guards;
    }
    TEST_ASSERT_(net_stack_lo == lo + PAINT_FLOOR, "not painted: %s", log_count ? log_lines[(log_count - 1) % 32] : "(no line)");
    size_t painted = (size_t)(net_stack_painted - lo), wrong = 0, gap = 0, floor_written = 0;
    for (size_t i = 0; i < PAINT_FLOOR; i++) floor_written += paint_snap[i] != 0; /* as the test left it */
    for (size_t i = PAINT_FLOOR; i < painted; i++) wrong += paint_snap[i] != STACK_PAINT;
    TEST_CHECK_(!floor_written, "%zu of the stack's lowest %u B painted", floor_written, PAINT_FLOOR);

    for (size_t i = painted; i < painted + 64 && i < PAINT_STACK; i++) gap += paint_snap[i] != 0; /* as the test left it */
    uintptr_t gap_end = (uintptr_t)net_stack_painted + PAINT_GAP; /* the painting frame is above it */
    TEST_CHECK_(painted > PAINT_STACK / 2 && !wrong && !gap,
                "painted %zu B, %zu not the pattern, %zu of the 64 B above it written", painted, wrong, gap);
    TEST_CHECK_(gap_end <= paint_local_after && gap_end < (uintptr_t)(lo + PAINT_STACK),
                "the gap's end 0x%lx is not below the caller's frame 0x%lx", (unsigned long)gap_end,
                (unsigned long)paint_local_after);
    TEST_CHECK(net_stack_top == lo + PAINT_STACK);
    uint32_t hw = stack_high_water();
    if (depth) {
        size_t truth = (size_t)(lo + PAINT_STACK - (uint8_t *)paint_deepest);
        TEST_CHECK_(hw >= truth && hw <= truth + 256, "depth %d: high-water %u B, the deepest write %zu B below the top",
                    depth, (unsigned)hw, truth);
    } else {
        TEST_CHECK(hw == (uint32_t)(net_stack_top - net_stack_painted)); /* nothing used below the paint */
        /* the line a game exit logs from this paint: the stack's size, not the painted span (2d N4: MK4) */
        net_stack_logged = 0;
        net_stack_check();
        char want[96];
        snprintf(want, sizeof(want), "VjoNet stack: at least %u of 16384 B used", (unsigned)hw);
        TEST_CHECK_(logged(want) != NULL, "%s", want);
    }
    return guards;
}
static void test_net_stack_paint_on_a_real_thread_stays_in_its_stack(void)
{
#if defined(__SANITIZE_ADDRESS__)
#define VJO_PAINT_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define VJO_PAINT_ASAN 1
#endif
#endif
#ifdef VJO_PAINT_ASAN
    /* Under AddressSanitizer the test cannot hold: with clang, ASan's memset outgrows the 256 B the
     * test leaves below its own frame when it clears the thread's stack (a SEGV in the paint thread);
     * with gcc, the frame's address-taken local is not on the thread's stack at all (ASan's fake
     * stack, most likely), so the paint refuses ("not painted (frame ... outside ...)"). And the paint
     * writes below the stack pointer on purpose. Neither is a defect of the code under test
     * (t3-worker-any-game, 2026-10-16; audit-overlay-shell-slim-4 N5). */
    TEST_SKIP("the stack paint writes below the stack pointer on purpose: not under AddressSanitizer");
    return;
#endif
    TEST_CHECK(paint_on_a_thread(0, 0));
    TEST_CHECK(paint_on_a_thread(0, 12)); /* 13 frames of 512 B and more: about 7 KiB deeper */
    for (int mode = 1; mode <= PAINT_MODES; mode++)
        TEST_CHECK_(paint_on_a_thread(mode, 0), "mode %d", mode);
    reset();
}

/* The boot guard's "ok": the control thread alive for a minute, marked once;
 * a BUSY kernel (its own uptime shorter) is retried a bounded number of
 * times, five seconds apart. */
static void test_boot_guard_ok_after_a_stable_minute(void)
{
    memset(guard_marks, 0, sizeof(guard_marks));
    guard_mark_rc = 0;
    test_now = 1000000;
    ctl_started_us = now_us();
    boot_ok_attempts = 0;
    for (int s = 0; s < 60; s++) {
        boot_guard_ok();
        test_now += 1000000;
    }
    TEST_CHECK(guard_marks[VJO_BOOT_STAGE_OK] == 0); /* 59 s: not yet */
    boot_guard_ok();                                 /* 60 s */
    TEST_CHECK(guard_marks[VJO_BOOT_STAGE_OK] == 1);
    test_now += 3600LL * 1000000;
    boot_guard_ok();
    boot_guard_ok();
    TEST_CHECK(guard_marks[VJO_BOOT_STAGE_OK] == 1); /* once per boot */

    /* refused: retried every 5 s, at most BOOT_OK_ATTEMPTS times */
    memset(guard_marks, 0, sizeof(guard_marks));
    guard_mark_rc = -1;
    ctl_started_us = now_us();
    boot_ok_attempts = 0;
    test_now += 60LL * 1000000;
    for (int s = 0; s < 200; s++) {
        boot_guard_ok();
        test_now += 1000000;
    }
    TEST_CHECK_(guard_marks[VJO_BOOT_STAGE_OK] == BOOT_OK_ATTEMPTS, "%d attempts", guard_marks[VJO_BOOT_STAGE_OK]);
    guard_mark_rc = 0;
    boot_guard_ok();
    TEST_CHECK(guard_marks[VJO_BOOT_STAGE_OK] == BOOT_OK_ATTEMPTS); /* gave up */

    /* refused twice, then accepted */
    memset(guard_marks, 0, sizeof(guard_marks));
    guard_mark_rc = -1;
    ctl_started_us = now_us();
    boot_ok_attempts = 0;
    test_now += 61LL * 1000000;
    boot_guard_ok();
    test_now += 5LL * 1000000;
    boot_guard_ok();
    guard_mark_rc = 0;
    test_now += 5LL * 1000000;
    boot_guard_ok();
    boot_guard_ok();
    TEST_CHECK(guard_marks[VJO_BOOT_STAGE_OK] == 3);
    TEST_CHECK(boot_ok_attempts == -1);
}

/* The ledger's static table must describe this code: the result memory is the
 * local arena, LOCAL_RESULT_ARENA_SIZE (the only one: the overlay is offline),
 * and every allocation is counted in and out of the pool it comes from. */
static void test_ledger_counts_result_memory_against_the_budget(void)
{
    TEST_CHECK(LOCAL_RESULT_ARENA_SIZE == VJO_BUDGET_PAF_RESULTS_LOCAL);
    TEST_CHECK(VJO_WORST_SHELL_PAF <= VJO_LIMIT_SHELL_PAF);
    TEST_CHECK(VJO_WORST_SHELL_PAGES <= VJO_LIMIT_SHELL_PAGES);
    vjo_ledger_shell_init();
    TEST_CHECK(vjo_ledger_pool_current(&g_shell_ledger, VJO_LEDGER_POOL_SHELL_PAGES) ==
               VJO_BUDGET_SHELL_MODULE_PAGES + VJO_BUDGET_WORKER_IN_SHELL_PAGES);
    TEST_CHECK(VJO_BUDGET_WORKER_IN_SHELL_PAGES == 0); /* the worker is registered per game, not loaded in SceShell */
    TEST_CHECK(vjo_ledger_pool_current(&g_shell_ledger, VJO_LEDGER_POOL_SHELL_PAF) == VJO_BUDGET_PAF_PLUGIN);
    mem_free();
    fail_alloc = 0;
    cfg = loaded;
#ifdef VJO_PAF_ALLOC
    TEST_CHECK(mem_alloc() == 0);
    TEST_CHECK(vjo_ledger_current(&g_shell_ledger, VJO_SL_PAF_RESULTS) == VJO_BUDGET_PAF_RESULTS_LOCAL);
    TEST_CHECK(vjo_ledger_pool_current(&g_shell_ledger, VJO_LEDGER_POOL_SHELL_PAF) == VJO_BUDGET_PAF_PLUGIN + VJO_BUDGET_PAF_RESULTS_LOCAL);
    TEST_CHECK(vjo_ledger_pool_current(&g_shell_ledger, VJO_LEDGER_POOL_SHELL_PAGES) ==
               VJO_BUDGET_SHELL_MODULE_PAGES + VJO_BUDGET_WORKER_IN_SHELL_PAGES);
    mem_free();
    TEST_CHECK(vjo_ledger_current(&g_shell_ledger, VJO_SL_PAF_RESULTS) == 0);
    TEST_CHECK(vjo_ledger_peak(&g_shell_ledger, VJO_SL_PAF_RESULTS) == VJO_BUDGET_PAF_RESULTS_LOCAL);
    TEST_CHECK(vjo_ledger_pool_current(&g_shell_ledger, VJO_LEDGER_POOL_SHELL_PAF) == 8192 &&
               vjo_ledger_pool_peak(&g_shell_ledger, VJO_LEDGER_POOL_SHELL_PAF) == 73728); /* 8,192 + 65,536 */
#else
    TEST_CHECK(mem_alloc() == 0);
    TEST_CHECK(vjo_ledger_current(&g_shell_ledger, VJO_SL_USERRW_RESULTS) == VJO_BUDGET_PAF_RESULTS_LOCAL);
    TEST_CHECK(vjo_ledger_pool_current(&g_shell_ledger, VJO_LEDGER_POOL_SHELL_PAF) == VJO_BUDGET_PAF_PLUGIN);
    mem_free();
    TEST_CHECK(vjo_ledger_current(&g_shell_ledger, VJO_SL_USERRW_RESULTS) == 0);
#endif
    loaded.dictionary = VJO_DICT_LOCAL;
    cfg = loaded;
    vjo_ledger_shell_log("test");
    TEST_CHECK(!open_batch); /* the ledger table's batch is closed: no pointer into this stack frame */
}

#if VJO_DEBUG /* a self-test test: the debug build's */
/* The self-test, while armed, runs OCR automatically whatever config.ini says,
 * and keeps the user's region (#74: the API 18 test forced the full screen and,
 * never finishing after a failure, kept forcing it). Its full-screen stage asks
 * for the whole frame itself. */
static void test_selftest_keeps_the_user_region_and_auto_ocr(void)
{
    saved_region = (VjoRect){100, 200, 30000, 8000};
    strcpy(title_id, "PCSG00415");
    loaded.ocr_mode = VJO_OCR_ON_PRESS;
    selftest_armed = 1; selftest_done = 0;
    apply_config();
    TEST_CHECK(cfg.ocr_mode == VJO_OCR_AUTO);
    regions_set = 0;
    push_region();
    TEST_CHECK(regions_set == 1 && !last_region_full && game_region.w == 30000);
    selftest_armed = 0;
    apply_config();
    TEST_CHECK(cfg.ocr_mode == VJO_OCR_ON_PRESS);
    push_region();
    TEST_CHECK(regions_set == 2 && !last_region_full);
    saved_region.w = 0;
    push_region();
    TEST_CHECK(last_region_full && !game_region.w);
    loaded.ocr_mode = VJO_OCR_AUTO;
    apply_config();
}
#endif

#ifdef VJO_WITH_VOCR
#if VJO_DEBUG /* a self-test test: the debug build's */
/* The self-test's job (job_selftest) runs every stage in turn, each with its
 * record, a failed stage included, and then ends the test. It shows nothing:
 * neither its text nor a lookup reaches the overlay's text or result. */
static void test_selftest_stages_run_in_one_job_and_finish(void)
{
    reset();
    selftest_armed = 1; selftest_done = 0; selftest_stage_count = 5;
    selftest_begins = selftest_ends = submits = 0;
    fake_recognized_text = "猫";
    job_selftest = 1;
    TEST_CHECK(run_vocr_job() == VJO_OK);
    TEST_CHECK(selftest_begins == 5 && selftest_ends == 5 && submits == 5 && selftest_done);
    /* The stage's own flags, and the job policy every stage carries (API 20):
     * the default 150 ms stable-text wait (15 x 10 ms), its 2 s limit (40 x 50
     * ms); blank lines are kept by default. */
    const uint32_t stage_bits = VJO_GAME_OCR_F_FULL | VJO_GAME_OCR_F_FORCE_FALLBACK | VJO_GAME_OCR_F_PROBE;
    const uint32_t policy = 15u << VJO_GAME_OCR_SETTLE_SHIFT | 40u << VJO_GAME_OCR_SETTLE_MAX_SHIFT |
                            2u << VJO_GAME_OCR_LM_SHIFT |     /* the 1 MB LM at most */
                            8u << VJO_GAME_OCR_CASCADE_SHIFT | /* the cascade within 2 s */
                            VJO_GAME_OCR_REFINE_PSP << VJO_GAME_OCR_REFINE_SHIFT; /* refined in PSP games */
    TEST_CHECK((selftest_flags_seen[0] & stage_bits) == VJO_GAME_OCR_F_FORCE_FALLBACK &&
               (selftest_flags_seen[1] & stage_bits) == 0 &&
               (selftest_flags_seen[2] & stage_bits) == VJO_GAME_OCR_F_FULL &&
               (selftest_flags_seen[3] & stage_bits) == VJO_GAME_OCR_F_PROBE &&
               (selftest_flags_seen[4] & stage_bits) == 0);
    for (unsigned i = 0; i < 5; i++) {
        /* API 22: the default ocr_threads = 3 (two helpers), the default ocr_boost_clock = on (444 MHz while
         * recognizing), no line cache in any stage; textbox2 (the last) serially, to compare with textbox,
         * with the clock kept; the begin record says so. API 36: no stage drops a border line (as the gate,
         * the user's jobs only). API 43: hook_trust (on by default) rides in every job's speed word, the stages'
         * too: their pick is OCR, which a trusted hook never answers (game_ocr/main.c) */
        const uint32_t boost = VJO_GAME_OCR_S_BOOST | VJO_GAME_OCR_S_NOCACHE | VJO_GAME_OCR_S_TRUST;
        TEST_CHECK(selftest_speed_seen[i] == (i == 4 ? boost : 2u | boost));
        TEST_MSG("stage %u speed 0x%08X", i, (unsigned)selftest_speed_seen[i]);
        TEST_CHECK(selftest_begin_speed[i] == selftest_speed_seen[i]);
        TEST_CHECK((selftest_flags_seen[i] & ~(stage_bits | VJO_GAME_OCR_F_SETTLE_FIRST)) == policy);
        TEST_MSG("stage %u flags 0x%08X", i, (unsigned)selftest_flags_seen[i]);
    }
    TEST_CHECK(!cache_data[0].sentence && !cache_data[0].ocr_text && !local_text_valid && !job_text_ready);
    TEST_CHECK(!text_generation); /* nor is its text the screen's for a later self-test */
    TEST_CHECK(!vjo_selftest_armed() && !vjo_selftest_stages()); /* done: the next job is a normal one */
    mem_free();
    /* A failing stage still gets its record and the test still finishes. */
    reset();
    selftest_armed = 1; selftest_done = 0; selftest_begins = selftest_ends = submits = 0;
    fail_vocr_user = 1; fake_free_user = 1000; fake_need_weights = 659456; fake_reserve = 4194304;
    job_selftest = 1;
    TEST_CHECK(run_vocr_job() == VJO_E_OOM);
    TEST_CHECK(selftest_ends == 5 && selftest_rcs[0] == VJO_E_OOM && selftest_rcs[1] == VJO_E_OOM && selftest_done);
    fail_vocr_user = 0; fake_recognized_text = NULL; selftest_armed = 0; selftest_stage_count = 0;
    mem_free();
}
#endif

#if VJO_DEBUG /* a self-test test: the debug build's */
/* #81's self-test began 2 s after the game started, on its opening notice: the
 * three region stages found no line (the full screen five, above the region).
 * The stages now wait for text in the region: a first stage with no line, or
 * whose screen changed under every read, ends the job with its own result and
 * the stages run on a later settled screen; after VJO_SELFTEST_WAITS such
 * screens they run anyway (their records then say there was no text). */
static void test_selftest_waits_for_text_in_the_region(void)
{
    /* The stage's own bits (a job's policy may ride in the other flags). */
    const uint32_t stage = VJO_GAME_OCR_F_FULL | VJO_GAME_OCR_F_FORCE_FALLBACK | VJO_GAME_OCR_F_PROBE;
    reset();
    selftest_armed = 1; selftest_done = 0; selftest_stage_count = 5;
    selftest_begins = selftest_ends = submits = 0;
    job_selftest = 1;
    TEST_CHECK(run_vocr_job() == VJO_OK); /* an empty region: no line */
    TEST_CHECK(selftest_begins == 1 && !selftest_ends && submits == 1 && selftest_waits == 1 && !selftest_done);
    TEST_CHECK(vjo_selftest_stages() == 5 && !cache_data[0].sentence);
    TEST_CHECK((selftest_flags_seen[0] & stage) == VJO_GAME_OCR_F_FORCE_FALLBACK);
    mem_free();
    fake_screen_changes = 1; /* typing text: the screen changed under every read */
    TEST_CHECK(run_vocr_job() == VJO_E_SCREEN_CHANGED && selftest_waits == 2 && !selftest_ends && !selftest_done);
    mem_free();
    fake_recognized_text = "猫"; /* dialogue in the region: every stage, then done */
    TEST_CHECK(run_vocr_job() == VJO_OK && !cache_data[0].sentence);
    TEST_CHECK(selftest_begins == 2 + 5 && selftest_ends == 5 && submits == 2 + 5 && selftest_done);
    TEST_CHECK((selftest_flags_seen[2] & stage) == VJO_GAME_OCR_F_FORCE_FALLBACK &&
               (selftest_flags_seen[4] & stage) == VJO_GAME_OCR_F_FULL);
    mem_free();
    /* Bounded: after VJO_SELFTEST_WAITS empty screens the stages run on the next anyway. */
    reset();
    selftest_armed = 1; selftest_done = 0; selftest_stage_count = 5;
    selftest_begins = selftest_ends = submits = 0;
    job_selftest = 1;
    for (int i = 0; i < VJO_SELFTEST_WAITS; i++) {
        TEST_CHECK(run_vocr_job() == VJO_OK && !selftest_done);
        mem_free();
    }
    TEST_CHECK(selftest_waits == VJO_SELFTEST_WAITS && !selftest_ends);
    TEST_CHECK(run_vocr_job() == VJO_OK && selftest_ends == 5 && selftest_done && selftest_rcs[0] == VJO_OK);
    selftest_armed = 0; selftest_stage_count = 0; job_selftest = 0;
    mem_free();
}
#endif

/* Nothing about a changed screen reaches the user (#81 request 4), but the open overlay's wait hint after
 * WAIT_HINT_AFTER torn jobs in a row (#358: test_open_overlay_says_why_it_waits), which is allowed by its text
 * (not by the words it happens to avoid) and is never an error. */
static void check_quiet(void)
{
    TEST_CHECK(!g_view.status_is_error && (!strcmp(g_view.status, WAIT_HINT) ||
                                           (!strstr(g_view.status, "changed") && !strstr(g_view.status, "reopen"))));
    TEST_CHECK(g_view.strip_kind != VJO_STRIP_ERROR && !strstr(g_view.strip_text, "changed"));
}

/* #81: 4 of 21 console jobs read a screen that changed under every read
 * (VJO_E_SCREEN_CHANGED) and were dropped. They still are, quietly and logged;
 * the next try starts at once, from the second in a row 250 ms later,
 * doubling to 4 s, and a job that reads its screen ends the run. Auto (the
 * overlay closed, subtitles on) and an open overlay waiting for its text
 * ("Recognizing…" all along) follow the same pace; closing the overlay
 * during a wait leaves the retry to Auto. The error text no longer exists. */
static void test_screen_changed_is_retried_quietly_at_a_bounded_pace(void)
{
    char memory[64]; VjoArena arena;
    VjoErr err = {.rc = VJO_E_SCREEN_CHANGED};
    vjo_arena_init(&arena, memory, sizeof(memory));
    TEST_CHECK(!strcmp(vjo_err_text(&arena, VJO_STAGE_OCR, &err), ""));

    cursor_setup();
    fake_screen_changes = 8;
    const int64_t waits[] = {0, 250, 500, 1000, 2000, 4000, 4000, 4000}; /* ms, before tries 2..9 */
    stable_pending = 1; control_tick(0);
    for (int i = 0; i < 8; i++) {
        char line[128];
        TEST_ASSERT(job_running && !job_refresh);
        finish_current_job();
        snprintf(line, sizeof(line), "job dropped: the screen changed under it (%d in a row), "
                 "next try in %u ms", i + 1, (unsigned)waits[i]);
        TEST_CHECK_(DLOGGED(line), "%s", line);
        TEST_CHECK(!job_running && stable_pending);
        check_quiet();
        if (waits[i]) {
            test_now += waits[i] * 1000 - 1000;
            control_tick(0);
            TEST_CHECK(!job_running); /* not yet */
            test_now += 1000;
        }
        control_tick(0);
    }
    TEST_ASSERT(job_running);
    finish_current_job(); /* this one reads its screen */
    TEST_CHECK(local_text_valid && !strcmp(local_text, "猫犬鳥") && !screen_changed_streak && !screen_retry_until);
    TEST_CHECK(vocr_allocs == 9 && vocr_frees == 9);
    check_quiet();
    mem_free();

    /* The subtitles: nothing reaches the strip; the retry keeps its 1 s pace. */
    cursor_setup();
    subtitles = 1;
    fake_screen_changes = 1;
    stable_pending = 1; control_tick(0);
    TEST_ASSERT(job_running && g_view.strip_busy);
    finish_current_job();
    TEST_CHECK(!g_view.strip_busy && !g_view.strip_text[0]);
    check_quiet();
    control_tick(0);
    TEST_CHECK(!job_running); /* SUBTITLE_GAP_US after the last job */
    test_now += SUBTITLE_GAP_US; control_tick(0);
    TEST_ASSERT(job_running);
    finish_current_job();
    TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !strcmp(g_view.strip_text, "猫犬鳥"));
    subtitles = 0;
    mem_free();

    /* Another game (or the same one restarted) starts the pace over. */
    cursor_setup();
    fake_screen_changes = 2;
    stable_pending = 1; control_tick(0); finish_current_job();
    control_tick(0); finish_current_job();
    TEST_CHECK(screen_changed_streak == 2 && screen_retry_until > now_us());
    invalidate_game_work();
    TEST_CHECK(!screen_changed_streak && !screen_retry_until);
    stable_pending = 1; control_tick(0);
    TEST_ASSERT(job_running); /* at once */
    finish_current_job();
    mem_free();
    /* A job of the previous game that ends after the change counts for no game. */
    cursor_setup();
    fake_screen_changes = 1;
    stable_pending = 1; control_tick(0);
    TEST_ASSERT(job_running);
    invalidate_game_work();
    finish_current_job();
    TEST_CHECK(!screen_changed_streak && !screen_retry_until);
    mem_free();

    cursor_setup();
    fake_screen_changes = 3;
    open_overlay();
    TEST_ASSERT(job_running);
    finish_current_job(); /* the first: again at once */
    TEST_ASSERT(job_running && !strcmp(g_view.status, "Recognizing…"));
    finish_current_job(); /* the second: 250 ms */
    TEST_CHECK(!job_running && open_retry && !strcmp(g_view.status, "Recognizing…") && !g_view.list);
    check_quiet();
    test_now += 249000; control_tick(0);
    TEST_CHECK(!job_running);
    test_now += 1000; control_tick(0);
    TEST_ASSERT(job_running && !open_retry);
    finish_current_job(); /* the third: 500 ms */
    test_now += 500000; control_tick(0);
    TEST_ASSERT(job_running);
    finish_current_job();
    check_current_definition("cat", 0);
    check_quiet();
    close_overlay(); mem_free();

    cursor_setup();
    fake_screen_changes = 2;
    open_overlay(); finish_current_job(); finish_current_job();
    TEST_CHECK(open_retry && !job_running);
    close_overlay();
    TEST_CHECK(!open_retry && stable_pending);
    control_tick(0);
    TEST_CHECK(!job_running);
    test_now += 250000; control_tick(0);
    TEST_ASSERT(job_running && job_prepares); /* Auto's job for this screen */
    finish_current_job();
    TEST_CHECK(local_text_valid && !g_view.open && cache_ok);
    check_quiet();
    mem_free();
}

/* The region shows scene id (as the kernel numbers it): a job then captures that scene. */
static void next_scene(uint32_t id)
{
    state.scene = state.region_scene = state.capture_scene = id;
    state.region_seq++;
}

/* The open overlay with this text, its cursor and its entry. */
static void check_shown(const char *text, const char *meaning, uint32_t cursor)
{
    TEST_ASSERT(g_view.list != NULL && g_view.list->n_entries == 1);
    TEST_CHECK(g_view.open && g_view.local_cursor_mode && g_view.cursor_byte == cursor);
    TEST_CHECK(!strcmp(g_view.list->header, text) && !strcmp(local_text, text));
    TEST_CHECK(strstr(g_view.list->entries[0].text, meaning) != NULL && g_view.list->entries[0].hl_start == (int)cursor);
    TEST_CHECK(!held_cmd && !held_view);
    check_quiet();
}

/* As finish_current_job, with the job cancelled for a newer screen (cancel_outdated_job, a region or a game change)
 * after the game answered and before the control thread takes the result; with sentence, its sentence was shown
 * first (on_job_text). A job cancelled before its read never reads (VJO_E_CANCELLED): this one's result is the
 * game's, VJO_E_SCREEN_CHANGED with fake_screen_changes. */
static void finish_current_job_cancelled(int sentence)
{
    TEST_ASSERT(job_running);
    test_thread = NET_THREAD;
    net_phase = NET_IDLE;
#if !VJO_DEBUG
    net_job_begin();
#endif
    net_job();
#if !VJO_DEBUG
    net_job_end();
#endif
    test_thread = CTL_THREAD;
    TEST_ASSERT(job_done);
    if (sentence) {
        TEST_ASSERT(job_text_ready);
        on_job_text();
        TEST_ASSERT(view_job_text && g_view.list == &job_text_list);
    }
    job_cancelled = 1;
    on_job_done();
#if !VJO_DEBUG
    test_thread = NET_THREAD;
    net_idle_write();
    test_thread = CTL_THREAD;
#endif
}

/* The open overlay waits for its text and says only "Recognizing…" (as it always did). */
static void check_waiting(const char *status)
{
    TEST_CHECK_(!strcmp(g_view.status, status), "status '%s', not '%s'", g_view.status, status);
    TEST_CHECK(g_view.open && !g_view.list && !g_view.status_is_error);
}

/* #358 (the owner: "Say why the user is waiting. After a few seconds of 'Recognizing…' on torn reads, name the cause
 * and the fix"): once WAIT_HINT_AFTER (2) jobs in a row found the screen changed under every read, the open overlay's
 * next tries say why it still is recognizing and what helps (WAIT_HINT, from the data segment), never as an error,
 * until a read shows its text. One torn read never does. Automatically's torn jobs before the press count: a press in
 * the pace's gap starts a try that says why at once, one while Automatically's next try runs says "Recognizing…"
 * until that try ends, and the overlay's own next try says why. A try cancelled after it showed its sentence keeps
 * it; a torn try cancelled after the game answered counts for no screen. ocr_wait_hint = off says "Recognizing…" all
 * along; a region drawn as it asks, or a new game, starts over; a refresh's torn reads leave the shown text alone;
 * the subtitle strip never says why, with the overlay open or closed. */
static void test_open_overlay_says_why_it_waits(void)
{
    /* still trying; the cause (an icon, or text still typing); the fix as a condition (audit F1: □ cannot leave out an
     * icon inside a line, a picture behind the text or a line still typing) */
    TEST_CHECK(strstr(WAIT_HINT, "Recognizing…") == WAIT_HINT && strstr(WAIT_HINT, "keeps moving") &&
               strstr(WAIT_HINT, "typing") && strstr(WAIT_HINT, "If a region around just the text leaves it out") &&
               strstr(WAIT_HINT, "□") && strlen(WAIT_HINT) < sizeof(g_view.status));

    /* Three torn reads after the press, then one that reads its screen. */
    cursor_setup();
    TEST_CHECK(loaded.ocr_wait_hint == 1); /* on by default */
    fake_screen_changes = 3;
    open_overlay();
    TEST_ASSERT(job_running);
    check_waiting("Recognizing…");
    finish_current_job(); /* the first torn: again at once, still "Recognizing…" */
    TEST_ASSERT(job_running);
    check_waiting("Recognizing…");
    finish_current_job(); /* the second: the next try 250 ms later */
    TEST_CHECK(!job_running && open_retry);
    check_waiting("Recognizing…");
    test_now += 250000; control_tick(0);
    TEST_ASSERT(job_running); /* the third try, after two in a row: why */
    check_waiting(WAIT_HINT);
    TEST_CHECK(DLOGGED("job start (the screen changed during the last one)"));
    finish_current_job(); /* the third torn: 500 ms; the hint stays meanwhile */
    TEST_CHECK(!job_running && open_retry);
    check_waiting(WAIT_HINT);
    test_now += 500000; control_tick(0);
    TEST_ASSERT(job_running);
    check_waiting(WAIT_HINT);
    finish_current_job(); /* it reads its screen: the text and its word replace the hint */
    check_current_definition("cat", 0);
    TEST_CHECK(!g_view.status[0] && !screen_changed_streak);
    check_quiet();
    close_overlay(); mem_free();

    /* One torn read, then a read: never the hint. */
    cursor_setup();
    fake_screen_changes = 1;
    open_overlay(); finish_current_job();
    TEST_ASSERT(job_running);
    check_waiting("Recognizing…");
    finish_current_job();
    check_current_definition("cat", 0);
    close_overlay(); mem_free();

    /* Automatically's two torn jobs while the overlay is closed show nothing; a press in the pace's gap (no job runs)
     * starts a try of its own, which says why at once. */
    cursor_setup();
    fake_screen_changes = 3;
    stable_pending = 1; control_tick(0);
    TEST_ASSERT(job_running && job_prepares);
    finish_current_job();
    control_tick(0);
    TEST_ASSERT(job_running); /* again at once */
    finish_current_job();
    TEST_CHECK(screen_changed_streak == 2 && !g_view.open && !g_view.status[0]);
    check_quiet();
    open_overlay();
    TEST_ASSERT(job_running);
    check_waiting(WAIT_HINT);
    finish_current_job(); /* torn: 500 ms */
    check_waiting(WAIT_HINT);
    test_now += 500000; control_tick(0);
    TEST_ASSERT(job_running);
    finish_current_job();
    check_current_definition("cat", 0);
    close_overlay(); mem_free();

    /* A try that showed its sentence ("Looking up words…") and was then cancelled for a newer screen: the next try
     * still says why (start_job_kind gives both its publishes the same status). */
    cursor_setup();
    fake_screen_changes = 2;
    open_overlay(); finish_current_job(); finish_current_job();
    test_now += 250000; control_tick(0);
    TEST_ASSERT(job_running);
    check_waiting(WAIT_HINT);
    finish_current_job_cancelled(1); /* its sentence shown, then cancelled for a newer screen */
    TEST_ASSERT(job_running && !view_job_text);
    check_waiting(WAIT_HINT);
    finish_current_job();
    check_current_definition("cat", 0);
    close_overlay(); mem_free();

    /* A press while Automatically's next try runs (the common case: that try starts once the pace allows) says
     * "Recognizing…" until the try ends; it tore too, so the overlay's own next try, after the pace, says why
     * (audit F4: not at once). */
    cursor_setup();
    fake_screen_changes = 3;
    stable_pending = 1; control_tick(0); finish_current_job();
    control_tick(0); finish_current_job();
    TEST_CHECK(screen_changed_streak == 2 && !job_running);
    test_now += 250000; control_tick(0);
    TEST_ASSERT(job_running && job_prepares); /* Automatically's third try */
    open_overlay();
    TEST_CHECK(ov == OV_OPEN_OLD_JOB);
    check_waiting("Recognizing…");
    finish_current_job(); /* torn: the overlay's try waits out the 500 ms pace */
    TEST_CHECK(!job_running && open_retry && screen_changed_streak == 3);
    check_waiting("Recognizing…");
    test_now += 500000; control_tick(0);
    TEST_ASSERT(job_running);
    check_waiting(WAIT_HINT);
    finish_current_job();
    check_current_definition("cat", 0);
    close_overlay(); mem_free();

    /* A torn try cancelled for a newer screen after the game answered counts for no screen (on_job_done): the count
     * stays at 1, the next try starts at once and says "Recognizing…" (audit F5). */
    cursor_setup();
    fake_screen_changes = 2;
    open_overlay(); finish_current_job();
    TEST_ASSERT(job_running && screen_changed_streak == 1);
    finish_current_job_cancelled(0); /* torn too, and cancelled */
    TEST_CHECK(screen_changed_streak == 1);
    TEST_ASSERT(job_running);
    check_waiting("Recognizing…");
    finish_current_job();
    check_current_definition("cat", 0);
    close_overlay(); mem_free();

    /* The subtitle strip never says why: with the overlay closed and the count at 2, Select + R's strip says
     * "Recognizing…" while its job runs (audit F5). */
    cursor_setup();
    fake_screen_changes = 2;
    stable_pending = 1; control_tick(0); finish_current_job();
    control_tick(0); finish_current_job();
    TEST_CHECK(screen_changed_streak == 2 && ov == OV_CLOSED && !job_running);
    control_tick(VJO_EV_SUBTITLE); /* Select + R */
    TEST_CHECK_(subtitles && g_view.strip_kind == VJO_STRIP_STATUS && !strcmp(g_view.strip_text, "Recognizing…"),
                "strip '%s' (kind %d)", g_view.strip_text, g_view.strip_kind);
    TEST_ASSERT(job_running);
    finish_current_job();
    TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !strcmp(g_view.strip_text, "猫犬鳥"));
    subtitles = 0;
    mem_free();

    /* ocr_wait_hint = off: "Recognizing…" all along, as before #358. */
    cursor_setup();
    cfg.ocr_wait_hint = loaded.ocr_wait_hint = 0; /* the overlay's opening loads the settings */
    fake_screen_changes = 3;
    open_overlay(); finish_current_job(); finish_current_job();
    test_now += 250000; control_tick(0);
    TEST_ASSERT(job_running);
    check_waiting("Recognizing…");
    finish_current_job();
    test_now += 500000; control_tick(0);
    TEST_ASSERT(job_running);
    check_waiting("Recognizing…");
    finish_current_job();
    check_current_definition("cat", 0);
    close_overlay(); mem_free();

    /* The region drawn as the hint asks (□, a box, Enter) starts over: the old region's try, cancelled, counts for
     * no region, and the next press says "Recognizing…" again, with no retry pace held over. */
    cursor_setup();
    fake_screen_changes = 3;
    open_overlay(); finish_current_job(); finish_current_job();
    test_now += 250000; control_tick(0);
    TEST_ASSERT(job_running);
    check_waiting(WAIT_HINT);
    VjoRect drawn = {10, 10, 200, 60};
    vjo_post_command(VJO_CMD_SET_REGION, &drawn); control_tick(0); /* saved: the overlay closes, the try cancelled */
    TEST_CHECK(ov == OV_CLOSED && job_running && job_cancelled && !screen_changed_streak && !screen_retry_until);
    finish_current_job(); /* cancelled before its read: VJO_E_CANCELLED, no read (the third torn result is left) */
    TEST_CHECK(!screen_changed_streak && !screen_retry_until && fake_screen_changes == 1);
    /* the new region: that one torn read, then the text */
    open_overlay();
    TEST_ASSERT(job_running);
    check_waiting("Recognizing…");
    finish_current_job(); /* one in a row: again at once, still "Recognizing…" */
    TEST_ASSERT(job_running);
    check_waiting("Recognizing…");
    finish_current_job();
    check_current_definition("cat", 0);
    close_overlay(); mem_free();

    /* Another game (or the same one restarted) starts over: its press says "Recognizing…". */
    cursor_setup();
    fake_screen_changes = 2;
    stable_pending = 1; control_tick(0); finish_current_job();
    control_tick(0); finish_current_job();
    TEST_CHECK(screen_changed_streak == 2);
    invalidate_game_work();
    open_overlay();
    TEST_ASSERT(job_running);
    check_waiting("Recognizing…");
    finish_current_job();
    check_current_definition("cat", 0);
    close_overlay(); mem_free();

    /* The open overlay shows a text: its refreshes' torn reads count, but leave that text, its entry and its status
     * alone (a refresh publishes no status), and the subtitle strip shows the text, never the hint. */
    cursor_setup();
    open_overlay(); finish_current_job();
    check_shown("猫犬鳥", "cat", 0);
    const VjoEntryList *shown = g_view.list;
    unsigned seq = g_view.list_seq;
    subtitles = 1;
    fake_screen_changes = 3;
    for (uint32_t i = 0; i < 3; i++) {
        next_scene(20 + i);
        test_now += 10 * REFRESH_GAP_US;
        control_tick(VJO_EV_REGION_STABLE);
        TEST_ASSERT(job_running && job_refresh);
        finish_current_job();
        TEST_CHECK(g_view.list == shown && g_view.list_seq == seq && !g_view.status[0]);
        TEST_CHECK(!strstr(g_view.strip_text, "keeps moving"));
    }
    TEST_CHECK(screen_changed_streak == 3);
    check_shown("猫犬鳥", "cat", 0);
    subtitles = 0;
    close_overlay(); mem_free();
}

/* ---- line history (docs/line-history.md) ---- */

/* The overlay on 猫犬鳥 (its first word looked up), this game's earlier lines in history.csv with
 * another game's between them, and the current text appended already (as Auto appends it). */
static void history_setup(void)
{
    cursor_setup();
    vjo_ledger_shell_init(); /* the visit's block is counted (VJO_SL_PAF_HISTORY) */
    history_csv_add("vita-PCSG00415", "鳥");
    history_csv_add("vita-PCSG00002", "他のゲーム");
    history_csv_add("vita-PCSG00415", "犬鳥");
    history_csv_add("vita-PCSG00415", "猫犬鳥");
    view_invariant = 1; view_invariant_checks = 0; invariant_generation = 0; invariant_seq = 0;
    open_overlay(); finish_current_job();
    check_shown("猫犬鳥", "cat", 0);
}

/* The overlay's d-pad key past the text's rows (overlay.cpp input_overlay), and the control thread's tick. */
static void history_key(int cmd)
{
    vjo_post_command(cmd, NULL);
    control_tick(0);
}

static unsigned history_held(void) { return vjo_ledger_current(&g_shell_ledger, VJO_SL_PAF_HISTORY); }

/* The owner's example (2026-10-20): on 昨日すき焼きを食べました, Up shows ４７７８万 between 昭和５２ and
 * the current line, dimmed; here 猫犬鳥 is the current line. Up walks back, each earlier line shown
 * between its neighbours and looked up as the current text is; Down walks forward and, past the newest,
 * returns to the current text with its cursor, as read from its screen. */
static void test_history_up_walks_back_and_down_returns(void)
{
    history_setup();
    VjoLineDebug debug = g_view.line_debug;
    TEST_CHECK(debug.origin == VJO_LINE_OCR && debug.model != NULL);
    uint32_t scene = local_text_scene, seq = local_text_capture_seq;
    vjo_post_cursor(3, local_text_generation); on_command(); finish_current_job();
    check_shown("猫犬鳥", "dog", 3);
    unsigned list_seq = g_view.list_seq;
    history_key(VJO_CMD_EARLIER);
    TEST_CHECK(g_view.line_debug.origin == VJO_LINE_OCR && !g_view.line_debug.model);
    TEST_CHECK(history_loads == 1 && !strcmp(history_uuid, "vita-PCSG00415\",")); /* this game's lines only */
    TEST_ASSERT(job_running && job_cursor_only && job_cursor_byte == 0);
    TEST_CHECK(g_view.history == 1 && !strcmp(g_view.history_before, "鳥") && !strcmp(g_view.history_after, "猫犬鳥"));
    TEST_CHECK(g_view.list == &local_header && !g_view.list->n_entries && !strcmp(local_text, "犬鳥"));
    TEST_CHECK(g_view.list_seq != list_seq && g_view.text_generation == local_text_generation && !g_view.dict_pages);
    TEST_CHECK(local_text_scene == 0 && local_text_capture_seq == 0); /* no screen shows it now */
    TEST_CHECK(history_held() == VJO_BUDGET_PAF_HISTORY && history_allocs == 1);
    finish_current_job();
    check_shown("犬鳥", "dog", 0);
    TEST_CHECK(g_view.history == 1);
    /* the arrows and lookups on the earlier line, as on the current text */
    vjo_post_cursor(3, local_text_generation); on_command(); finish_current_job();
    check_shown("犬鳥", "bird", 3);
    history_key(VJO_CMD_EARLIER);
    finish_current_job();
    check_shown("鳥", "bird", 0);
    TEST_CHECK(g_view.history == 2 && !g_view.history_before && !strcmp(g_view.history_after, "犬鳥"));
    /* the oldest: Up does nothing, as at the text's top row before line history */
    history_key(VJO_CMD_EARLIER);
    TEST_CHECK(!job_running && g_view.history == 2 && history_loads == 1 && !g_view.status[0]);
    check_shown("鳥", "bird", 0);
    /* Down: the line after it, then the current text again with its cursor and its screen */
    history_key(VJO_CMD_LATER);
    finish_current_job();
    check_shown("犬鳥", "dog", 0);
    TEST_CHECK(g_view.history == 1 && !strcmp(g_view.history_before, "鳥"));
    history_key(VJO_CMD_LATER);
    TEST_ASSERT(job_running && job_cursor_only && job_cursor_byte == 3);
    TEST_CHECK(g_view.history == 0 && !g_view.history_before && !g_view.history_after && !history);
    TEST_CHECK(history_allocs == 1 && history_frees == 1 && !history_held());
    TEST_CHECK(local_text_scene == scene && local_text_capture_seq == seq);
    TEST_CHECK(!memcmp(&debug, &g_view.line_debug, sizeof(debug)));
    finish_current_job();
    check_shown("猫犬鳥", "dog", 3);
    /* Down on the current text: nothing */
    history_key(VJO_CMD_LATER);
    TEST_CHECK(!job_running && history_loads == 1 && !history);
    /* reopened on this screen, the current text's saved word is shown: no recognition, no lookup */
    close_overlay(); control_tick(0);
    unsigned reads = cursor_reads, inferences = vocr_allocs;
    open_overlay();
    TEST_CHECK(!job_running && cursor_reads == reads && vocr_allocs == inferences && DLOGGED("open: saved local word"));
    check_shown("猫犬鳥", "dog", 3);
    /* a second visit reads the file again, from the start */
    history_key(VJO_CMD_EARLIER);
    finish_current_job();
    check_shown("犬鳥", "dog", 0);
    TEST_CHECK(history_loads == 2 && history_allocs == 2 && history_frees == 1);
    TEST_CHECK(view_invariant_checks > 0);
    view_invariant = 0;
}

/* Only Read text: Automatically keeps lines (history.csv): in on_press mode Up does what it did. No
 * earlier line, or no memory for a visit: nothing, and nothing kept. */
static void test_history_needs_auto_lines_and_memory(void)
{
    history_setup();
    cfg.ocr_mode = VJO_OCR_ON_PRESS;
    history_key(VJO_CMD_EARLIER);
    TEST_CHECK(!job_running && !history && !history_loads && !history_allocs && !g_view.history && !g_view.status[0]);
    check_shown("猫犬鳥", "cat", 0);
    cfg.ocr_mode = VJO_OCR_AUTO;
    /* no Paf memory: no visit, no file read */
    fail_history_alloc = 1;
    history_key(VJO_CMD_EARLIER);
    TEST_CHECK(!job_running && !history && !history_loads && !history_held());
    /* ... and a lookup running then is not dropped: its word is shown (no "Looking up words…" left) */
    vjo_post_cursor(3, local_text_generation); on_command();
    TEST_ASSERT(job_running && job_cursor_only);
    history_key(VJO_CMD_EARLIER);
    TEST_CHECK(!job_cancelled && !history_step && !history);
    finish_current_job();
    check_shown("猫犬鳥", "dog", 3);
    fail_history_alloc = 0;
    /* no history.csv yet, or none of this game's lines in it */
    history_load_rc = (int)0x80010002;
    history_key(VJO_CMD_EARLIER);
    TEST_CHECK(!job_running && !history && history_loads == 1 && history_allocs == 1 && history_frees == 1);
    history_load_rc = 0;
    history_csv[0] = 0;
    history_csv_add("vita-PCSG00002", "他のゲーム");
    history_csv_add("vita-PCSG00415", "猫犬鳥"); /* the current text only */
    history_key(VJO_CMD_EARLIER);
    TEST_CHECK(!job_running && !history && history_loads == 2 && history_allocs == 2 && history_frees == 2);
    TEST_CHECK(!history_held() && !g_view.history && !g_view.status[0]);
    check_shown("猫犬鳥", "dog", 3);
    /* a PSP game's lines are under its platform and its own disc ID, as net_job writes them */
    vjo_anki_set_game("ULJM05000", "PSP", 1);
    history_key(VJO_CMD_EARLIER);
    TEST_CHECK(!strcmp(history_uuid, "psp-ULJM05000\","));
}

/* A job reads local_text: a step asked for meanwhile waits for its end, and the lookup, page or
 * refresh of the line it leaves is dropped; no refresh runs while an earlier line is shown, and the
 * one asked for runs once the current text is back. */
static void test_history_waits_for_the_running_job(void)
{
    history_setup();
    vjo_post_cursor(3, local_text_generation); on_command();
    TEST_ASSERT(job_running && job_cursor_only);
    history_key(VJO_CMD_EARLIER); /* the visit opens at once (the job only reads local_text), the step waits */
    TEST_CHECK(job_cancelled && history_step == 1 && history && history_loads == 1 && !history->h.at);
    TEST_CHECK(!strcmp(local_text, "猫犬鳥") && !g_view.history);
    history_key(VJO_CMD_EARLIER); /* another while one waits: dropped, not stacked */
    TEST_CHECK(history_step == 1 && history_loads == 1);
    vjo_post_cursor(6, local_text_generation); /* a move of the line it leaves, while it waits */
    control_tick(0);
    TEST_CHECK(history_step == 1 && history_loads == 1);
    finish_current_job(); /* the cancelled lookup: nothing shown, nothing started after it */
    TEST_CHECK(!job_running);
    control_tick(0);
    TEST_ASSERT(job_running && job_cursor_only && history);
    finish_current_job();
    check_shown("犬鳥", "dog", 0);
    /* a settled screen while an earlier line is shown: no refresh until the current text is back */
    next_scene(8);
    test_now += 10 * REFRESH_GAP_US;
    control_tick(VJO_EV_REGION_STABLE);
    TEST_CHECK(!job_running && refresh_pending);
    test_now += 10 * REFRESH_GAP_US;
    control_tick(0);
    TEST_CHECK(!job_running && refresh_pending);
    history_key(VJO_CMD_LATER);
    finish_current_job();
    check_shown("猫犬鳥", "bird", 6); /* the cursor it had last: the move made while the step waited */
    test_now += 10 * REFRESH_GAP_US;
    control_tick(0);
    TEST_ASSERT(job_running && job_refresh);
    /* Up during that refresh: it yields (asked again), and the step follows its end */
    history_key(VJO_CMD_EARLIER);
    TEST_CHECK(job_cancelled && history_step == 1 && history_loads == 2);
    finish_current_job();
    TEST_CHECK(!job_running && refresh_pending);
    control_tick(0);
    TEST_ASSERT(job_running && job_cursor_only && history);
    finish_current_job();
    check_shown("犬鳥", "dog", 0);
    /* back to the current text; then Up during a lookup, a close and a reopen while it runs: the step
     * goes with the close, and the visit, which never showed a line, once no job runs; a settled
     * screen's refresh is not held back by it */
    history_key(VJO_CMD_LATER);
    finish_current_job();
    TEST_CHECK(!history && history_frees == 2);
    vjo_post_cursor(0, local_text_generation); on_command();
    history_key(VJO_CMD_EARLIER);
    TEST_ASSERT(job_running && job_cursor_only && history && history_step == 1 && !history->h.at);
    close_overlay(); open_overlay();
    TEST_CHECK(!history_step && history && !g_view.history);
    while (job_running) /* the cancelled lookup, then the reopen's own */
        finish_current_job(), control_tick(0);
    TEST_CHECK(!history && history_frees == 3 && !history_held() && !g_view.history);
    check_shown("猫犬鳥", "cat", 0);
    next_scene(30);
    test_now += 10 * REFRESH_GAP_US;
    control_tick(VJO_EV_REGION_STABLE);
    TEST_CHECK(job_running && job_refresh);
    finish_current_job();
}

/* Up with nothing to go to (on_press, the oldest line kept, no earlier line) changes nothing, as on
 * the text's top row before line history: the overlay posts the step all the same (it cannot tell),
 * and a move (a Right, a tap, an L/R page) posted before the control thread takes it wins the slot,
 * so the overlay and the worker never disagree on the cursor ("Looking up words…" left behind:
 * audit-line-history F2). A step never takes a close's or a region's slot (M4). */
static void test_history_a_step_never_holds_a_move_or_a_close(void)
{
    history_setup();
    for (int c = 0; c < 2; c++) {
        cfg.ocr_mode = c ? VJO_OCR_AUTO : VJO_OCR_ON_PRESS;
        if (c) { /* Auto, on the oldest line kept: nothing earlier */
            history_key(VJO_CMD_EARLIER); finish_current_job();
            history_key(VJO_CMD_EARLIER); finish_current_job();
            check_shown("鳥", "bird", 0);
        }
        vjo_post_command(VJO_CMD_EARLIER, NULL); /* the overlay's Up, the control thread busy */
        vjo_post_cursor(c ? 0 : 3, local_text_generation); /* the next frame's move */
        TEST_CHECK_(pending_cmd == VJO_CMD_CURSOR, "case %d: the move was refused (slot %d)", c, pending_cmd);
        control_tick(0);
        TEST_ASSERT(job_running && job_cursor_only);
        finish_current_job();
        if (c)
            check_shown("鳥", "bird", 0);
        else
            check_shown("猫犬鳥", "dog", 3);
        TEST_CHECK(g_view.history == 2 * c && history_loads == (unsigned)c);
    }
    /* a page posted over a pending step wins too (a second dictionary's entry) */
    vjo_post_command(VJO_CMD_EARLIER, NULL);
    g_view.dict_pages = 2;
    TEST_CHECK(vjo_post_page(1, local_text_generation) && pending_cmd == VJO_CMD_CURSOR);
    control_tick(0);
    finish_current_job();
    /* a step never takes a close: Cancel, then Up in the next frame */
    vjo_post_command(VJO_CMD_CLOSED, NULL);
    vjo_post_command(VJO_CMD_EARLIER, NULL);
    TEST_CHECK(pending_cmd == VJO_CMD_CLOSED);
    control_tick(0);
    while (job_running)
        finish_current_job(), control_tick(0);
    TEST_CHECK_(ov == OV_CLOSED && !g_view.open, "the close was lost: ov %d, open %d", ov, g_view.open);
    TEST_CHECK(!history && !g_view.history && !history_held());
    /* nor a region's */
    open_overlay();
    while (job_running)
        finish_current_job(), control_tick(0);
    VjoRect r = {10, 10, 300, 80};
    vjo_post_command(VJO_CMD_SET_REGION, &r);
    vjo_post_command(VJO_CMD_EARLIER, NULL);
    TEST_CHECK(pending_cmd == VJO_CMD_SET_REGION);
    control_tick(0);
    TEST_CHECK(ov == OV_CLOSED && !history);
}

/* The overlay closed on an earlier line while its lookup runs, then Select + R before that lookup ends,
 * or in the tick of its end: the subtitle strip shows the screen's line or "Recognizing…" and a job,
 * never the earlier line the overlay's text still is (audit-line-history F3). */
static void test_history_the_strip_never_shows_an_earlier_line(void)
{
    for (int same_tick = 0; same_tick < 2; same_tick++) {
        history_setup();
        history_key(VJO_CMD_EARLIER); /* 犬鳥 shown, its first word's lookup runs */
        TEST_ASSERT(job_running && job_cursor_only && !strcmp(local_text, "犬鳥"));
        close_overlay();
        if (same_tick) { /* the lookup's end and Select + R in one wakeup */
            test_thread = NET_THREAD;
            net_job();
            test_thread = CTL_THREAD;
        }
        control_tick(VJO_EV_SUBTITLE);
        TEST_CHECK(subtitles);
        while (job_running) {
            TEST_CHECK_(strcmp(g_view.strip_text, "犬鳥") != 0, "case %d: the strip shows the earlier line", same_tick);
            finish_current_job();
            control_tick(0);
        }
        TEST_CHECK_(strcmp(g_view.strip_text, "犬鳥") != 0 && g_view.strip_kind == VJO_STRIP_SENTENCE,
                    "case %d: the strip shows '%s' (kind %d)", same_tick, g_view.strip_text, g_view.strip_kind);
        TEST_CHECK(!history && !history_held());
        set_subtitles(0);
    }
}

/* An OCR job is not cancelled by a step (it is no lookup or refresh): Up while it looks its own text
 * up opens the visit and waits for it. A settled new screen then cancels that job, and its end retires
 * the text, the visit with it: the waiting step must go too, or every later lookup would wait for a
 * step that never comes ("Looking up words…" until a close) and Up would stay dead
 * (audit-line-history-b B-7). */
static void test_history_a_step_waiting_for_a_cancelled_ocr_job(void)
{
    history_setup();
    view_invariant = 0; /* (an OCR job's text is published as job_text_list, which that check does not model) */
    close_overlay(); control_tick(0);
    next_scene(20);
    fake_recognized_text = "犬猫";
    open_overlay(); /* another screen: recognized, then looked up */
    TEST_ASSERT(job_running && !job_cursor_only && !job_refresh && job_lookup);
    read_action = 6;
    finish_current_job();
    TEST_CHECK(history_loads == 1 && history_frees == history_allocs && !history && !history_held());
    TEST_CHECK_(!history_step, "a step was left behind with no visit: %d", history_step);
    while (job_running) /* the new screen's own recognition */
        finish_current_job(), control_tick(0);
    TEST_ASSERT(local_text_valid && !strcmp(local_text, "犬猫") && g_view.local_cursor_mode);
    vjo_post_cursor(3, local_text_generation); on_command();
    TEST_CHECK_(job_running && job_cursor_only, "the move was not looked up (job %d, step %d)", job_running,
                history_step);
    if (job_running)
        finish_current_job();
    TEST_CHECK(g_view.cursor_byte == 3 && strcmp(g_view.status, "Looking up words…") != 0);
    history_key(VJO_CMD_EARLIER); /* and Up works again */
    TEST_CHECK(history_loads == 2 && history != NULL);
    while (job_running)
        finish_current_job(), control_tick(0);
}

/* Closing on an earlier line: the current text comes back once no job reads local_text, so a reopen
 * on its screen shows it; reopened while that job still ran, the screen is recognized. A game exit or
 * a new region lets the visit go with the text. */
static void test_history_close_exit_and_region(void)
{
    history_setup();
    /* Up while a lookup runs (the visit open, its step waiting), then a close before the lookup ends:
     * the step goes with the close, the visit once the lookup is done; the current text is unchanged */
    vjo_post_cursor(3, local_text_generation); on_command();
    history_key(VJO_CMD_EARLIER);
    TEST_ASSERT(job_running && history && history_step == 1 && !history->h.at);
    close_overlay();
    TEST_CHECK(!history_step && history);
    control_tick(0);
    TEST_CHECK(history && job_running); /* the lookup still reads local_text */
    finish_current_job();
    control_tick(0);
    TEST_CHECK(!history && history_frees == 1 && !strcmp(local_text, "猫犬鳥") && local_cursor_byte == 3 && !g_view.history);
    open_overlay();
    TEST_ASSERT(job_running && job_cursor_only && job_cursor_byte == 3);
    finish_current_job();
    check_shown("猫犬鳥", "dog", 3);
    vjo_post_cursor(0, local_text_generation); on_command(); finish_current_job();
    check_shown("猫犬鳥", "cat", 0);
    /* ... and reopened on the same screen while that lookup still runs: the text is shown and looked up
     * again; the visit, never shown, goes once no job runs (no close would end it now), and a settled
     * screen's refresh is not held back by it */
    vjo_post_cursor(3, local_text_generation); on_command();
    history_key(VJO_CMD_EARLIER);
    TEST_ASSERT(job_running && history && history_step == 1 && !history->h.at);
    close_overlay(); open_overlay();
    TEST_CHECK(ov == OV_OPEN && !history_step && history && g_view.list == &local_header);
    while (job_running) /* the cancelled lookup, then the reopen's own */
        finish_current_job(), control_tick(0);
    TEST_CHECK(!history && history_frees == 2 && !history_held() && !g_view.history);
    check_shown("猫犬鳥", "dog", 3);
    next_scene(30);
    test_now += 10 * REFRESH_GAP_US;
    control_tick(VJO_EV_REGION_STABLE);
    TEST_CHECK(job_running && job_refresh);
    finish_current_job();
    vjo_post_cursor(0, local_text_generation); on_command(); finish_current_job();
    history_frees = history_allocs = 0;
    history_key(VJO_CMD_EARLIER);
    TEST_ASSERT(job_running && job_cursor_only);
    close_overlay();
    TEST_CHECK(job_cancelled && !history_step);
    control_tick(0);
    TEST_CHECK(history && !strcmp(local_text, "犬鳥")); /* the lookup still reads it */
    finish_current_job();
    control_tick(0);
    TEST_CHECK(!history && history_frees == 1 && !strcmp(local_text, "猫犬鳥") && local_text_valid && !g_view.history);
    open_overlay();
    TEST_ASSERT(job_running && job_cursor_only && job_cursor_byte == 0);
    finish_current_job();
    check_shown("猫犬鳥", "cat", 0);
    /* reopened before the earlier line's lookup ended: its screen is recognized afresh */
    history_key(VJO_CMD_EARLIER);
    TEST_ASSERT(job_running && job_cursor_only && history);
    close_overlay(); open_overlay();
    TEST_CHECK(ov == OV_OPEN_OLD_JOB && !g_view.list && !strcmp(g_view.status, "Recognizing…"));
    finish_current_job(); /* the lookup, dropped: a recognition of the screen follows, and the visit goes */
    TEST_ASSERT(job_running && !job_cursor_only && !history && history_frees == 2);
    finish_current_job();
    check_shown("猫犬鳥", "cat", 0);
    /* a game exit on an earlier line: the visit goes with the text, nothing comes back */
    history_key(VJO_CMD_EARLIER);
    finish_current_job();
    check_shown("犬鳥", "dog", 0);
    control_tick(VJO_EV_GAME_EXIT);
    TEST_CHECK(!history && history_frees == 3 && !local_text_valid && !g_view.history && !history_held());
    /* a region drawn on an earlier line: the same */
    activate(0x40010003, "PCSG00415", VJO_GAME);
    history_csv[0] = 0;
    history_csv_add("vita-PCSG00415", "犬鳥");
    open_overlay(); finish_current_job();
    history_key(VJO_CMD_EARLIER);
    finish_current_job();
    check_shown("犬鳥", "dog", 0);
    VjoRect r = {10, 10, 300, 80};
    vjo_post_command(VJO_CMD_SET_REGION, &r);
    control_tick(0);
    TEST_CHECK(!history && history_frees == 4 && ov == OV_CLOSED && !history_held());
}

/* #81 request 4, the open overlay: its text stays usable whatever the screen
 * does. A settled screen it was not read from is recognized in the
 * background (a refresh) while the entry, the arena and an Anki save keep
 * working; the same text changes nothing, a new text replaces the shown one
 * quietly with its first word looked up, no text or a changing screen keeps
 * it. Refreshes wait for a settled screen and are paced (REFRESH_GAP_US
 * apart), a lookup goes first (a refresh in progress
 * yields and is asked again once lookups pause), and closing the overlay
 * drops one in progress; reopening meanwhile waits for a recognition. */
static void test_open_overlay_refreshes_its_text_quietly(void)
{
    cursor_setup();
    fake_hook[0] = 9;
    fake_hook[1] = VJO_HOOK_R_LOCKED | VJO_HOOK_R_NO_OCR;
    fake_hook[3] = VJO_HOOK_STREAM_BIT(9);
    fake_hook[4] = VJO_TEXT_SOURCE_AUTO;
    fake_hook[5] = 8;
    view_invariant = 1; view_invariant_checks = 0; invariant_generation = 0; invariant_seq = 0;
    open_overlay(); finish_current_job();
    check_shown("猫犬鳥", "cat", 0);
    VjoLineDebug debug = g_view.line_debug;
    TEST_CHECK(debug.origin == VJO_LINE_HOOK && debug.stream == 9 && debug.flags == fake_hook[1]);
    TEST_CHECK(debug.streams == fake_hook[3] && debug.mode == VJO_TEXT_SOURCE_AUTO && debug.installed == 8);
    memset(fake_hook, 0, sizeof(fake_hook)); /* later OCR results must not relabel the accepted hook line */
    uint32_t generation = local_text_generation;
    unsigned seq = g_view.list_seq, inferences = vocr_allocs;
    const VjoEntryList *shown = g_view.list;
    control_tick(VJO_EV_REGION_STABLE); /* the screen the text was read from */
    TEST_CHECK(!job_running && !refresh_pending && vocr_allocs == inferences);

    next_scene(8); /* an icon's mask ended */
    control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh && !job_lookup && job_idx == 1);
    /* What anki.c checks before saving the shown entry still holds. */
    TEST_CHECK(g_view.open && g_view.list == shown && g_view.list_seq == seq && g_view.list->n_entries == 1);
    TEST_CHECK(local_text_valid && has_result_memory() && !g_view.status[0]);
    finish_current_job();
    TEST_CHECK(DLOGGED("refresh rc=0: same text") && local_text_scene == 8 && cache_scene == 8);
    TEST_CHECK(!memcmp(&debug, &g_view.line_debug, sizeof(debug)));
    check_shown("猫犬鳥", "cat", 0);
    TEST_CHECK(g_view.list_seq == seq && local_text_generation == generation && vocr_allocs == inferences + 1);
    control_tick(VJO_EV_REGION_STABLE);
    TEST_CHECK(!job_running && refresh_pending); /* paced: REFRESH_GAP_US after the last refresh */
    test_now += REFRESH_GAP_US; control_tick(0);
    TEST_CHECK(!job_running && !refresh_pending); /* the shown text is this screen's now */
    state.stable = 0; next_scene(14); /* nor one that still changes */
    control_tick(VJO_EV_REGION_STABLE);
    TEST_CHECK(!job_running && !refresh_pending && vocr_allocs == inferences + 1);
    state.stable = 1;

    /* The game moved on (auto-advance): replaced quietly. */
    fake_recognized_text = "犬猫";
    next_scene(9);
    control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    finish_current_job();
    TEST_CHECK(DLOGGED("refresh rc=0: new text"));
    TEST_CHECK(g_view.line_debug.origin == VJO_LINE_OCR && !g_view.line_debug.stream);
    TEST_CHECK(g_view.line_debug.scene == 9 && g_view.line_debug.model != NULL);
    TEST_ASSERT(job_running && job_cursor_only && job_cursor_byte == 0);
    TEST_CHECK(!strcmp(local_text, "犬猫") && local_text_generation != generation && local_text_scene == 9);
    TEST_CHECK(g_view.list == &local_header && !g_view.list->n_entries && g_view.text_generation == local_text_generation);
    TEST_CHECK(g_view.list_seq != seq && !strcmp(g_view.status, "Looking up words…") && !g_view.status_is_error);
    vjo_post_cursor(3, generation); /* made for the old text: refused */
    TEST_CHECK(pending_cmd == VJO_CMD_NONE);
    finish_current_job();
    check_shown("犬猫", "dog", 0);
    generation = local_text_generation;

    /* A lookup while a refresh reads: the refresh yields. */
    next_scene(10);
    test_now += REFRESH_GAP_US;
    control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    vjo_post_cursor(3, generation); control_tick(0);
    TEST_CHECK(job_cancelled && cursor_waiting);
    finish_current_job(); /* the refresh, cancelled: the lookup follows */
    TEST_CHECK(DLOGGED("refresh rc=-118: dropped"));
    TEST_ASSERT(job_running && job_cursor_only && job_cursor_byte == 3);
    finish_current_job();
    check_shown("犬猫", "cat", 3);
    TEST_CHECK(refresh_pending);
    control_tick(0);
    TEST_CHECK(!job_running); /* lookups have not paused for REFRESH_IDLE_US yet */
    test_now += REFRESH_IDLE_US; control_tick(0);
    TEST_CHECK(!job_running); /* nor REFRESH_GAP_US passed since the last refresh */
    test_now += REFRESH_GAP_US - REFRESH_IDLE_US; control_tick(0);
    TEST_ASSERT(job_running && job_refresh);
    finish_current_job(); /* the same text: the cursor and its entry stay */
    check_shown("犬猫", "cat", 3);

    /* A lookup long after the last refresh still goes first: a screen that
     * settles right after it waits REFRESH_IDLE_US; a command posted by the
     * overlay meanwhile (a move) also holds the refresh back. */
    test_now += 3 * REFRESH_GAP_US;
    vjo_post_cursor(0, local_text_generation); on_command(); finish_current_job();
    check_shown("犬猫", "dog", 0);
    next_scene(18); control_tick(VJO_EV_REGION_STABLE);
    TEST_CHECK(!job_running && refresh_pending);
    test_now += REFRESH_IDLE_US - 1; control_tick(0);
    TEST_CHECK(!job_running);
    test_now += 1;
    vjo_post_cursor(3, local_text_generation); /* the overlay's move, not handled yet */
    background_open();
    TEST_CHECK(!job_running && pending_cmd == VJO_CMD_CURSOR);
    on_command(); finish_current_job();
    check_shown("犬猫", "cat", 3);
    test_now += REFRESH_IDLE_US; control_tick(0);
    TEST_ASSERT(job_running && job_refresh);
    finish_current_job();
    check_shown("犬猫", "cat", 3);

    /* A refresh after each settled screen, REFRESH_GAP_US apart at most. */
    next_scene(15);
    test_now += REFRESH_GAP_US - 1; control_tick(VJO_EV_REGION_STABLE);
    TEST_CHECK(!job_running && refresh_pending);
    test_now += 1; control_tick(0);
    TEST_ASSERT(job_running && job_refresh);
    finish_current_job();
    check_shown("犬猫", "cat", 3);

    /* No text in the region (a box fading between lines), then a changing
     * screen: the shown text stays usable. */
    fake_recognized_text = "";
    next_scene(11);
    test_now += REFRESH_GAP_US; control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    finish_current_job();
    TEST_CHECK(DLOGGED("refresh rc=0: kept"));
    check_shown("犬猫", "cat", 3);
    fake_screen_changes = 1;
    next_scene(12);
    test_now += REFRESH_GAP_US; control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    finish_current_job();
    TEST_CHECK(DLOGGED("refresh dropped: the screen changed under it (1 in a row), next try in 0 ms"));
    check_shown("犬猫", "cat", 3);
    TEST_CHECK(refresh_pending && !job_running);
    fake_recognized_text = "鳥";
    control_tick(0); /* the next try, at once */
    TEST_ASSERT(job_running && job_refresh);
    finish_current_job(); finish_current_job();
    check_shown("鳥", "bird", 0);
    TEST_CHECK(!screen_changed_streak);

    /* Closing during a refresh drops it; Auto has the settled screen. */
    next_scene(13);
    test_now += REFRESH_GAP_US; control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    close_overlay();
    TEST_CHECK(job_cancelled && !refresh_pending);
    finish_current_job();
    TEST_CHECK(!job_running && !g_view.open && stable_pending && local_text_valid);
    check_quiet();
    /* Reopened while a refresh still runs, on a screen the text was not read
     * from: the opening waits for its own recognition. */
    fake_recognized_text = "犬";
    open_overlay(); finish_current_job();
    check_shown("犬", "dog", 0);
    next_scene(17);
    test_now += REFRESH_GAP_US; control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    close_overlay(); open_overlay();
    TEST_CHECK(ov == OV_OPEN_OLD_JOB && !strcmp(g_view.status, "Recognizing…"));
    finish_current_job();
    TEST_ASSERT(job_running && !job_refresh && !job_cursor_only && ov == OV_OPEN);
    fake_recognized_text = "猫";
    finish_current_job();
    check_shown("猫", "cat", 0);
    close_overlay(); mem_free();
    TEST_CHECK(view_invariant_checks > 10);
    view_invariant = 0;
}

/* ocr_mode = on_press recognizes only when the overlay opens: the open
 * overlay keeps its text and runs no refresh, however the screen settles. */
/* API 49: a trusted hook's line (VjoState.hook_scene, the kernel's while the region shows it) is read at once.
 * Overlay closed: past the subtitles' SUBTITLE_GAP_US and a failure's backoff, which still hold an untrusted
 * screen back. Open: its refresh starts right after the last refresh and lookup (REFRESH_GAP_US, REFRESH_IDLE_US
 * hold an untrusted screen back), and a line that grew (the text shown, then more) keeps the cursor where it was. */
static void test_trusted_hook_line_is_read_at_once(void)
{
    cursor_setup();
    subtitles = 1;
    stable_pending = 1; control_tick(0);
    TEST_ASSERT(job_running);
    finish_current_job();
    next_scene(30); control_tick(VJO_EV_REGION_QUIET);
    TEST_CHECK(!job_running && stable_pending); /* an untrusted screen: SUBTITLE_GAP_US after the last job */
    state.hook_scene = 30; control_tick(0);
    TEST_ASSERT(job_running); /* the trusted line: at once */
    finish_current_job();
    subtitles = 0;
    ocr_backoff.until = test_now + 10000000; /* a failure's backoff */
    state.hook_scene = 0; next_scene(31); control_tick(VJO_EV_REGION_QUIET); /* (the kernel's: only the scene's own) */
    TEST_CHECK(!job_running && stable_pending);
    state.hook_scene = 31; control_tick(0);
    TEST_ASSERT(job_running);
    finish_current_job();
    ocr_backoff.until = 0;
    /* the open overlay */
    open_overlay();
    if (job_running) finish_current_job();
    check_shown("猫犬鳥", "cat", 0);
    fake_recognized_text = "犬猫";
    next_scene(32); state.hook_scene = 0; control_tick(VJO_EV_REGION_QUIET);
    TEST_ASSERT(job_running && job_refresh); /* the first refresh: nothing to pace it yet */
    finish_current_job();
    TEST_ASSERT(job_running && job_cursor_only);
    finish_current_job();
    check_shown("犬猫", "dog", 0);
    uint32_t generation = local_text_generation;
    vjo_post_cursor(3, generation); on_command(); finish_current_job(); /* the reader moves to 猫 */
    check_shown("犬猫", "cat", 3);
    state.hook_scene = 0; next_scene(33); control_tick(VJO_EV_REGION_QUIET);
    TEST_CHECK(!job_running && refresh_pending); /* untrusted: REFRESH_GAP_US and REFRESH_IDLE_US */
    state.hook_scene = 33; fake_recognized_text = "犬猫鳥"; control_tick(0);
    TEST_ASSERT(job_running && job_refresh); /* trusted: at once */
    finish_current_job();
    TEST_CHECK(DLOGGED("refresh rc=0: new text") && !strcmp(local_text, "犬猫鳥"));
    TEST_ASSERT(job_running && job_cursor_only && job_cursor_byte == 3); /* the line grew: the cursor stays */
    finish_current_job();
    check_shown("犬猫鳥", "cat", 3);
    /* another line, trusted: replaced at once, the cursor at its start */
    next_scene(34); state.hook_scene = 34; fake_recognized_text = "鳥猫"; control_tick(VJO_EV_REGION_QUIET);
    TEST_ASSERT(job_running && job_refresh);
    finish_current_job();
    TEST_ASSERT(job_running && job_cursor_only && job_cursor_byte == 0);
    finish_current_job();
    state.hook_scene = 0;
    close_overlay();
    mem_free();
}

static void test_on_press_overlay_never_refreshes(void)
{
    cursor_setup();
    cfg.ocr_mode = loaded.ocr_mode = VJO_OCR_ON_PRESS;
    open_overlay(); finish_current_job();
    check_shown("猫犬鳥", "cat", 0);
    unsigned inferences = vocr_allocs;
    for (int i = 0; i < 3; i++) {
        next_scene(20 + i);
        test_now += 4 * REFRESH_GAP_US; control_tick(VJO_EV_REGION_STABLE);
        TEST_CHECK(!job_running && vocr_allocs == inferences);
    }
    check_shown("猫犬鳥", "cat", 0);
    close_overlay(); mem_free();
}

/* A replaced text's dictionary pages go with it: the overlay never offers
 * the old text's pages for the new text's word. */
static void test_refresh_replacement_resets_dictionary_pages(void)
{
    paged_setup(3);
    open_overlay(); finish_current_job();
    check_page("main. cat", 0, 0, 3);
    TEST_CHECK(vjo_post_page(1, local_text_generation)); on_command(); finish_current_job();
    check_page("extra cat", 0, 1, 3);
    fake_recognized_text = "犬";
    next_scene(8);
    test_now += REFRESH_IDLE_US; /* the page's lookup goes first */
    control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    finish_current_job();
    TEST_ASSERT(job_running && job_cursor_only);
    TEST_CHECK(!g_view.dict_page && !g_view.dict_pages && !local_page && !vjo_post_page(1, local_text_generation));
    finish_current_job();
    check_page("main. dog", 0, 0, 2); /* third.vjdict has no 犬 */
    close_overlay(); mem_free();
}

/* The job on the network thread, then one control-thread wakeup handles its
 * sentence and its end in ctl_main's order (finish_current_job skips the
 * sentence step). */
static void finish_by_tick(void)
{
    TEST_ASSERT(job_running);
    if (job_prepare) { net_job(); control_tick(0); return; }
    int idx = job_idx;
    job_rc = job_cursor_only ? run_cursor_job_t(&results[idx], &cache_data[idx]) : run_job_t(&results[idx], &cache_data[idx]);
    job_done = 1;
    control_tick(0);
}

/* Subtitles on, overlay open (#92's audit, B1): a refresh that reads a new text
 * shows it on the subtitle strip too, as every other OCR job does; its lookup
 * tags the cache with the refresh's capture, so after Circle no job reads that
 * screen again and the strip must already be right. A refresh that reads the
 * same text, or none, leaves the strip alone. */
static void test_refresh_updates_the_subtitle_strip(void)
{
    cursor_setup();
    subtitles = 1; /* Select+R before opening the overlay */
    open_overlay(); finish_current_job();
    check_shown("猫犬鳥", "cat", 0);
    TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !strcmp(g_view.strip_text, "猫犬鳥"));
    unsigned strip_seq = g_view.strip_text_seq;
    next_scene(8); /* the same text */
    control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh && !g_view.strip_busy);
    finish_current_job();
    TEST_CHECK(g_view.strip_text_seq == strip_seq && !strcmp(g_view.strip_text, "猫犬鳥"));
    fake_recognized_text = ""; /* no text: the shown one stays, and so does the strip */
    next_scene(9);
    test_now += REFRESH_GAP_US; control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    finish_current_job();
    TEST_CHECK(g_view.strip_text_seq == strip_seq && !strcmp(g_view.strip_text, "猫犬鳥"));
    fake_recognized_text = "犬猫"; /* the game moves on while the overlay is open */
    next_scene(10);
    test_now += REFRESH_GAP_US; control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    finish_current_job();
    TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !strcmp(g_view.strip_text, "犬猫") && !g_view.strip_busy);
    TEST_ASSERT(job_running && job_cursor_only);
    finish_current_job(); /* its first word: tags the cache with the refresh's capture */
    check_shown("犬猫", "dog", 0);
    unsigned inferences = vocr_allocs;
    close_overlay(); /* Circle */
    for (int i = 0; i < 3; i++) {
        test_now += SUBTITLE_GAP_US; control_tick(0);
        if (job_running) finish_current_job();
    }
    TEST_CHECK_(!strcmp(g_view.strip_text, "犬猫"), "strip shows [%s] while the region shows 犬猫", g_view.strip_text);
    TEST_CHECK(vocr_allocs == inferences); /* the screen's text was already read */
    subtitles = 0;
    mem_free();
}

/* The cards name the model that read their text (#90's record): a refresh
 * that keeps the shown text leaves the model alone, even when another model
 * read it (the memory fallback); one that replaces the text names its own. */
static void test_refresh_names_its_model_on_the_cards_only_when_it_replaces(void)
{
    cursor_setup();
    open_overlay(); finish_current_job();
    check_shown("猫犬鳥", "cat", 0);
    TEST_CHECK(!strcmp(anki_model, VJO_VOCR_TIER2_NAME));
    fake_memory_fallback = 1; /* the next jobs: H15 did not fit, FL10 ran */
    next_scene(8); /* the same text */
    control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    finish_current_job();
    TEST_CHECK(!strcmp(anki_model, VJO_VOCR_TIER2_NAME));
    fake_recognized_text = "犬猫";
    next_scene(9);
    test_now += REFRESH_GAP_US; control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    finish_current_job(); finish_current_job();
    check_shown("犬猫", "dog", 0);
    TEST_CHECK(!strcmp(anki_model, VJO_VOCR_TIER1_NAME));
    close_overlay(); mem_free();
}

/* A refresh logs its own lines only: no "subtitle text ready" (it has no
 * sentence until it is compared). A failing one (here the game's memory)
 * shows nothing, keeps the shown text, and backs off the refreshes: 2 s,
 * 4 s, ... between tries; Auto's backoff stays clear (#95 item 1); a refresh
 * that works ends the backoff. */
static void test_refresh_logs_quietly_and_a_failing_one_backs_off(void)
{
    cursor_setup();
    open_overlay(); finish_current_job();
    check_shown("猫犬鳥", "cat", 0);
    next_scene(8);
    control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    log_count = 0;
    finish_by_tick();
    TEST_CHECK(!job_running && DLOGGED("refresh rc=0: same text") && !logged("subtitle text ready"));
    fail_vocr_user = 1; fake_free_user = 1000; /* the game has no memory for OCR now */
    next_scene(9);
    test_now += REFRESH_GAP_US; control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    finish_by_tick();
    TEST_CHECK(DLOGGED("refresh rc=-103: kept") && refresh_backoff.us == BACKOFF_MIN_US);
    TEST_CHECK(!ocr_backoff.us && !ocr_backoff.until);
    check_shown("猫犬鳥", "cat", 0);
    TEST_CHECK(!g_view.status_is_error && !g_view.status[0]);
    next_scene(10);
    test_now += BACKOFF_MIN_US; control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh); /* 2 s later: the next try */
    finish_by_tick();
    TEST_CHECK(refresh_backoff.us == 2 * BACKOFF_MIN_US && !ocr_backoff.until);
    next_scene(11);
    test_now += BACKOFF_MIN_US; control_tick(VJO_EV_REGION_STABLE);
    TEST_CHECK(!job_running && refresh_pending); /* backing off: 4 s now */
    fail_vocr_user = 0;
    test_now += BACKOFF_MIN_US; control_tick(0);
    TEST_ASSERT(job_running && job_refresh);
    finish_by_tick();
    TEST_CHECK(!refresh_backoff.us && !refresh_backoff.until && !ocr_backoff.until);
    check_shown("猫犬鳥", "cat", 0);
    close_overlay(); mem_free();
}

/* Wakeups of the control thread, one a second, with each job run to its end as
 * the network thread would (finish_by_tick). */
static void run_for_seconds(int seconds)
{
    for (int i = 0; i < seconds; i++) {
        test_now += 1000000;
        if (job_running) finish_by_tick();
        else control_tick(0);
    }
}

/* #159 item 1 (#146's audit): reopening the overlay while a lookup of its saved
 * text still runs (the close cancelled it; a lookup takes 20-110 ms), on a
 * screen that text was not read from, waits for a recognition of its own, as an
 * opening during any job does (restart_old_job): once the lookup ends, a job
 * reads the screen. Back on the text's screen by then, the word at the cursor is
 * looked up instead. Before, the lookup's end started nothing and the overlay
 * said "Recognizing…" until it was closed (the audit's repro: 8 s of ticks). */
static void test_reopen_during_a_cancelled_lookup_gets_its_own_recognition(void)
{
    for (int back = 0; back < 2; back++) {
        cursor_saved_text(); /* Auto read 猫犬鳥 on scene 2 (Character mode: no word looked up yet) */
        open_overlay(); /* the saved text: its word's lookup */
        TEST_ASSERT(job_running && job_cursor_only && job_cursor_byte == 0);
        close_overlay(); /* Cancel: the lookup is cancelled, and still runs */
        TEST_CHECK(job_cancelled && ov == OV_CLOSED);
        next_scene(8); /* the game moved on */
        fake_recognized_text = "犬";
        open_overlay(); /* L + R */
        TEST_CHECK(ov == OV_OPEN_OLD_JOB && view_waiting && !strcmp(g_view.status, "Recognizing…"));
        if (back) state.scene = state.region_scene = state.capture_scene = 2; /* the text's screen again */
        log_count = 0;
        finish_by_tick(); /* the lookup ends */
        run_for_seconds(8);
        TEST_CHECK_(ov == OV_OPEN && !job_running && !view_waiting && strcmp(g_view.status, "Recognizing…"),
                    "after 8 s: ov %d (OV_OPEN_OLD_JOB %d), job_running %d, view_waiting %d, status '%s'", ov,
                    OV_OPEN_OLD_JOB, job_running, view_waiting, g_view.status);
        if (back) {
            check_shown("猫犬鳥", "cat", 0); /* the saved text's word: a lookup, no OCR */
            TEST_CHECK(vocr_allocs == 1);
        } else {
            check_shown("犬", "dog", 0); /* the new screen's text and its first word */
            TEST_CHECK(vocr_allocs == 2 && DLOGGED("job start (result was for an earlier screen)"));
        }
        close_overlay(); mem_free();
    }
}

/* Opened while Auto's job reads the screen (OV_OPEN_OLD_JOB: the opening waits
 * for it), its text shown as soon as it is read (on_job_text), and a move made
 * before the job ends: the job's end looks the moved-to word up instead
 * (on_job_done), and that lookup's end settles what the opening waited for, as
 * #159 item 1's does (on_cursor_done, restart_old_job). The text still shows the
 * screen: its word is shown and the overlay is an ordinary open one again,
 * whose refreshes follow the screen (before, it stayed OV_OPEN_OLD_JOB, and no
 * refresh ran for the rest of that opening). A screen that changed meanwhile
 * gets a recognition of its own. */
static void test_a_move_before_the_end_of_the_job_an_opening_waits_for(void)
{
    for (int changed = 0; changed < 2; changed++) {
        cursor_setup();
        stable_pending = 1; auto_prefetch(); /* Auto, Whole word: the first word too */
        TEST_ASSERT(job_running && job_lookup && !job_cursor_only);
        open_overlay(); /* before the job has read the text */
        TEST_CHECK(ov == OV_OPEN_OLD_JOB && view_waiting && !strcmp(g_view.status, "Recognizing…"));
        TEST_CHECK(run_job_t(&results[0], &cache_data[0]) == VJO_OK); /* the network thread: text, then the word */
        on_job_text(); /* the control thread: the text at once */
        TEST_ASSERT(g_view.list && !strcmp(g_view.list->header, "猫犬鳥") && g_view.local_cursor_mode);
        vjo_post_cursor(3, local_text_generation); on_command(); /* Right, while the job ends */
        TEST_CHECK(cursor_waiting);
        job_done = 1; on_job_done(); /* its word is superseded: the move's lookup */
        TEST_ASSERT(job_running && job_cursor_only && job_cursor_byte == 3);
        if (changed) { /* the screen moved on during the lookup */
            next_scene(8);
            fake_recognized_text = "犬";
        }
        finish_by_tick();
        if (changed) {
            TEST_ASSERT_(job_running && !job_cursor_only && !strcmp(g_view.status, "Recognizing…"),
                         "the changed screen: job_running %d, a lookup %d, status '%s'", job_running, job_cursor_only,
                         g_view.status);
            TEST_CHECK(DLOGGED("job start (result was for an earlier screen)"));
            finish_by_tick();
            check_shown("犬", "dog", 0);
        } else {
            check_shown("猫犬鳥", "dog", 3);
        }
        TEST_CHECK_(ov == OV_OPEN, "ov %d (OV_OPEN %d)", ov, OV_OPEN);
        /* A screen that settles now gets the open overlay's quiet refresh. */
        next_scene(20);
        test_now += REFRESH_IDLE_US; control_tick(VJO_EV_REGION_STABLE);
        TEST_ASSERT_(job_running && job_refresh, "no refresh: job_running %d, refresh_pending %d", job_running,
                     refresh_pending);
        finish_by_tick();
        if (changed) check_shown("犬", "dog", 0);
        else check_shown("猫犬鳥", "dog", 3);
        close_overlay(); mem_free();
    }
}

/* #95 item 3a (#92's audit): Select + R right after Cancel, while the closed
 * overlay's lookup or refresh still runs. The strip says "Recognizing…" and
 * waits for the job in progress (subtitles_show), which brings it nothing: a
 * lookup has no sentence for the strip, and a refresh is dropped with the
 * overlay. Once it ends, the strip gets the screen's sentence, the cached one or
 * one a job of its own reads. Before, it said "Recognizing…" with no job until
 * the next settled screen. */
static void test_subtitles_on_during_a_closed_overlays_lookup_or_refresh(void)
{
    for (int moved = 0; moved < 2; moved++) {
        cursor_saved_text();
        open_overlay();
        TEST_ASSERT(job_running && job_cursor_only);
        if (moved) { /* the game moved on while the overlay was open; nothing has settled yet */
            next_scene(8);
            fake_recognized_text = "犬";
        }
        unsigned inferences = vocr_allocs;
        close_overlay(); /* Cancel */
        control_tick(VJO_EV_SUBTITLE); /* Select + R */
        TEST_CHECK(subtitles && g_view.strip_busy && !strcmp(g_view.strip_text, "Recognizing…"));
        TEST_ASSERT(job_running && job_cursor_only); /* the lookup is still the job in progress */
        finish_by_tick();
        run_for_seconds(8);
        const char *want = moved ? "犬" : "猫犬鳥";
        TEST_CHECK_(g_view.strip_kind == VJO_STRIP_SENTENCE && !g_view.strip_busy && !strcmp(g_view.strip_text, want),
                    "after 8 s: strip '%s' (kind %d, busy %d), job_running %d; the region shows %s", g_view.strip_text,
                    g_view.strip_kind, g_view.strip_busy, job_running, want);
        TEST_CHECK_(vocr_allocs == inferences + (unsigned)moved, "moved %d: %u jobs read the screen, expected %d", moved,
                    vocr_allocs - inferences, moved); /* a job only for a screen not read yet */
        subtitles = 0;
        mem_free();
    }
    /* A refresh dropped by Cancel while the screen changes again (Auto has no
     * settled screen to read): the strip's own job reads it. */
    cursor_setup();
    open_overlay(); finish_current_job();
    check_shown("猫犬鳥", "cat", 0);
    fake_recognized_text = "犬猫";
    next_scene(9);
    control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    close_overlay();
    state.stable = 0; /* the next line types out */
    control_tick(VJO_EV_SUBTITLE);
    TEST_CHECK(subtitles && g_view.strip_busy && !strcmp(g_view.strip_text, "Recognizing…"));
    TEST_ASSERT(job_running && job_refresh);
    finish_by_tick(); /* the refresh, dropped */
    run_for_seconds(8);
    TEST_CHECK_(g_view.strip_kind == VJO_STRIP_SENTENCE && !g_view.strip_busy && !strcmp(g_view.strip_text, "犬猫"),
                "after 8 s: strip '%s' (kind %d, busy %d), job_running %d; the region shows 犬猫", g_view.strip_text,
                g_view.strip_kind, g_view.strip_busy, job_running);
    subtitles = 0;
    mem_free();
    /* Reopened during that refresh, on yet another screen: the opening's own
     * recognition (on_refresh_done) brings the strip its sentence too, so the
     * strip waits for nothing more. */
    cursor_setup();
    open_overlay(); finish_current_job();
    fake_recognized_text = "犬猫";
    next_scene(9);
    control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    close_overlay();
    control_tick(VJO_EV_SUBTITLE); /* Select + R */
    TEST_CHECK(strip_waiting && g_view.strip_busy && !strcmp(g_view.strip_text, "Recognizing…"));
    next_scene(10);
    control_tick(VJO_EV_TRIGGER); /* L + R */
    TEST_ASSERT(ov == OV_OPEN_OLD_JOB && job_running && job_refresh);
    finish_by_tick(); /* the refresh ends: the opening's job starts */
    TEST_ASSERT(job_running && !job_refresh && !job_cursor_only);
    TEST_CHECK(!strip_waiting && g_view.strip_busy);
    finish_by_tick();
    check_shown("犬猫", "dog", 0);
    TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !g_view.strip_busy && !strcmp(g_view.strip_text, "犬猫"));
    close_overlay();
    subtitles = 0;
    mem_free();
    /* Subtitles already on when the overlay opened: the strip waits for
     * nothing, so a lookup the close dropped leaves its sentence and starts no
     * job of the strip's; Auto reads the new screen once it settles. */
    cursor_setup();
    subtitles = 1;
    open_overlay(); finish_current_job();
    check_shown("猫犬鳥", "cat", 0);
    TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !strcmp(g_view.strip_text, "猫犬鳥"));
    vjo_post_cursor(3, local_text_generation); on_command();
    TEST_ASSERT(job_running && job_cursor_only);
    next_scene(8);
    state.stable = 0; /* the next line types out */
    fake_recognized_text = "犬";
    unsigned inferences = vocr_allocs;
    close_overlay(); /* Cancel */
    finish_by_tick();
    TEST_CHECK_(!job_running && vocr_allocs == inferences && !g_view.strip_busy && !strcmp(g_view.strip_text, "猫犬鳥"),
                "after the dropped lookup: job_running %d, jobs %u, strip '%s' (busy %d)", job_running,
                vocr_allocs - inferences, g_view.strip_text, g_view.strip_busy);
    test_now += SUBTITLE_GAP_US;
    state.stable = 1; control_tick(VJO_EV_REGION_STABLE); /* it settles */
    TEST_ASSERT(job_running && !job_cursor_only);
    finish_by_tick();
    TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !g_view.strip_busy && !strcmp(g_view.strip_text, "犬"));
    subtitles = 0;
    mem_free();
}

/* #95 item 3b (#92's audit): a move made during a lookup that a close then
 * cancelled is never looked up; the next open decides (open_overlay). Reopened
 * on a new screen, a job the game refuses (its memory) shows its error: the move
 * was the previous text's, which that job retired, so it supersedes no lookup of
 * the job's. Before, the job's end took it for a move made during the job,
 * whose lookup then found no text: no error, "Recognizing…" with no job. */
static void test_a_refused_job_after_a_cancelled_lookup_and_a_move_shows_its_error(void)
{
    cursor_saved_text();
    open_overlay();
    TEST_ASSERT(job_running && job_cursor_only && job_cursor_byte == 0);
    vjo_post_cursor(3, local_text_generation); on_command(); /* Right: it waits for the lookup */
    TEST_CHECK(cursor_waiting && pending_cmd == VJO_CMD_NONE);
    close_overlay(); /* Cancel: the lookup is cancelled, the move's never starts */
    finish_by_tick();
    TEST_CHECK(!job_running && !cursor_waiting && local_cursor_seq != job_cursor_seq && local_cursor_byte == 3);
    next_scene(8);
    fail_vocr_user = 1; fake_free_user = 1000; /* the game has no memory for OCR now */
    open_overlay();
    TEST_ASSERT(job_running && !job_cursor_only && view_waiting);
    finish_by_tick();
    run_for_seconds(8);
    TEST_CHECK_(!job_running && !view_waiting && g_view.status_is_error && strstr(g_view.status, "not enough memory"),
                "after 8 s: job_running %d, view_waiting %d, status '%s' (error %d)", job_running, view_waiting,
                g_view.status, g_view.status_is_error);
    TEST_CHECK(g_view.open && !g_view.local_cursor_mode && !local_text_valid);
    /* The memory comes back: the next open reads the screen and its first word. */
    fail_vocr_user = 0;
    close_overlay(); open_overlay();
    TEST_ASSERT(job_running && !job_cursor_only);
    finish_by_tick();
    check_shown("猫犬鳥", "cat", 0);
    close_overlay(); mem_free();

    /* The same, reopened while Auto's job for the new screen runs (Whole word:
     * it would look the word up), so its end goes through restart_old_job: the
     * job is for the screen shown, and the game refused it. */
    cursor_setup();
    stable_pending = 1; auto_prefetch(); finish_current_job();
    open_overlay();
    check_shown("猫犬鳥", "cat", 0); /* Auto's word, at once */
    uint32_t generation = local_text_generation;
    vjo_post_cursor(3, generation); on_command(); /* Right: a lookup */
    TEST_ASSERT(job_running && job_cursor_only);
    vjo_post_cursor(6, generation); on_command(); /* and again: it waits */
    TEST_CHECK(cursor_waiting);
    close_overlay(); finish_by_tick();
    TEST_CHECK(!job_running && local_cursor_seq != job_cursor_seq);
    fail_vocr_user = 1; fake_free_user = 1000;
    next_scene(8);
    control_tick(VJO_EV_REGION_STABLE); /* Auto's job for it */
    TEST_ASSERT(job_running && job_lookup && !job_cursor_only);
    open_overlay();
    TEST_CHECK(ov == OV_OPEN_OLD_JOB && view_waiting);
    finish_by_tick();
    run_for_seconds(8);
    TEST_CHECK_(!job_running && !view_waiting && g_view.status_is_error && strstr(g_view.status, "not enough memory"),
                "reopened during Auto's job, after 8 s: ov %d, job_running %d, view_waiting %d, status '%s' (error %d)",
                ov, job_running, view_waiting, g_view.status, g_view.status_is_error);
    TEST_CHECK(ov == OV_OPEN);
    close_overlay(); mem_free();
}

/* #95 item 1 (#92's audit): refreshes that fail (here the game's memory, until
 * the backoff's 60 s cap) still back off, but no longer delay Auto after Cancel:
 * the settled screen is read at once, as before #92, when no job ran while the
 * overlay was open. Before, Auto waited for the refreshes' backoff, up to 59 s,
 * and the subtitle strip kept the old sentence that long. */
static void test_failing_refreshes_do_not_delay_auto_after_close(void)
{
    cursor_setup();
    open_overlay(); finish_current_job();
    check_shown("猫犬鳥", "cat", 0);
    fail_vocr_user = 1; fake_free_user = 1000;
    for (int i = 0; i < 6; i++) { /* backing off 2, 4, 8, 16, 32, then 60 s */
        next_scene(20 + i);
        test_now += BACKOFF_MAX_US; control_tick(VJO_EV_REGION_STABLE);
        TEST_ASSERT(job_running && job_refresh);
        finish_by_tick();
        check_shown("猫犬鳥", "cat", 0);
    }
    next_scene(30);
    test_now += REFRESH_GAP_US; control_tick(VJO_EV_REGION_STABLE);
    TEST_CHECK(!job_running && refresh_pending); /* the refreshes back off: 60 s since the last */
    fail_vocr_user = 0; /* the game's memory came back */
    fake_recognized_text = "犬猫";
    close_overlay(); /* Cancel, 2 s after the last refresh failed */
    control_tick(0);
    TEST_ASSERT_(job_running && !job_refresh && !job_cursor_only, "Auto after Cancel: job_running %d", job_running);
    finish_by_tick();
    TEST_CHECK(local_text_valid && !strcmp(local_text, "犬猫") && cache_ok && !g_view.open);
    open_overlay(); /* the screen's text and word at once */
    TEST_CHECK(!job_running);
    check_shown("犬猫", "dog", 0);
    /* OCR works again (Auto's job read its screen): the next refresh need not
     * wait for the failed ones' 60 s, as with one backoff for every job. */
    TEST_CHECK(!refresh_backoff.us && !refresh_backoff.until && !ocr_backoff.until);
    fake_recognized_text = "鳥";
    next_scene(31);
    test_now += REFRESH_GAP_US; control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    finish_by_tick(); finish_by_tick();
    check_shown("鳥", "bird", 0);
    close_overlay(); mem_free();
    /* A game's start (or resume) ends it too, as the other backoffs. */
    backoff_note(&refresh_backoff, 1);
    TEST_CHECK(refresh_backoff.until > now_us());
    activate(state.game_pid, "PCSG00415", VJO_GAME);
    TEST_CHECK(!refresh_backoff.us && !refresh_backoff.until);
}

/* #95 item 2b (#92's audit): a refresh takes its capture for the text it shows,
 * the scene and the region capture's seq, whether it replaces the text or reads
 * the same one. When the region's scene number moves on with no new capture (an
 * icon masked since), the text still shows the screen: no refresh reads it
 * again, and reopening shows the saved word. next_scene moves both, so a refresh
 * that kept the old text's seq was caught by no test. */
static void test_a_refresh_takes_its_captures_seq(void)
{
    cursor_setup();
    open_overlay(); finish_current_job();
    check_shown("猫犬鳥", "cat", 0);
    fake_recognized_text = "犬猫";
    for (int same = 0; same < 2; same++) { /* a new text, then the same text again */
        next_scene(9 + 2 * same);
        test_now += 2 * REFRESH_GAP_US; control_tick(VJO_EV_REGION_STABLE);
        TEST_ASSERT(job_running && job_refresh);
        uint32_t seq = state.region_seq;
        finish_by_tick();
        if (!same) { /* the new text's first word */
            TEST_ASSERT(job_running && job_cursor_only);
            finish_by_tick();
        }
        check_shown("犬猫", "dog", 0);
        TEST_CHECK(local_text_scene == (uint32_t)(9 + 2 * same) && local_text_capture_seq == seq);
        TEST_CHECK(cache_ok && active == 0 && cache_seq == seq);
        state.scene = state.region_scene = 10 + 2 * same; /* the scene number moves on, the capture does not */
        test_now += 2 * REFRESH_GAP_US; control_tick(VJO_EV_REGION_STABLE);
        TEST_CHECK_(!job_running && !refresh_pending, "a refresh of the screen the text was read from (job %d)",
                    job_running);
        if (job_running) finish_by_tick();
        if (!same) {
            close_overlay();
            log_count = 0;
            open_overlay();
            TEST_CHECK(!job_running && DLOGGED("open: saved local word"));
            check_shown("犬猫", "dog", 0);
        }
    }
    close_overlay(); mem_free();
}

/* Found with item 2b's test: reopening on the screen a refresh read the same
 * text from shows the saved word at once, as reopening unchanged dialogue does
 * (docs/guide.md). The refresh keeps the text, its lookup and the cache
 * (on_refresh_done), and leaves the lookup's text generation, which open_overlay
 * compares, as it is. Before, a refresh cleared it, so the reopen said "Looking
 * up words…" and read the dictionary again for the word it had. */
static void test_reopen_after_a_same_text_refresh_shows_the_saved_word(void)
{
    cursor_setup();
    open_overlay(); finish_current_job();
    check_shown("猫犬鳥", "cat", 0);
    next_scene(8);
    control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    finish_by_tick();
    TEST_CHECK(DLOGGED("refresh rc=0: same text"));
    check_shown("猫犬鳥", "cat", 0);
    unsigned reads = cursor_reads, opens = cursor_opens;
    close_overlay();
    log_count = 0;
    open_overlay();
    TEST_CHECK_(!job_running && cursor_reads == reads && cursor_opens == opens,
                "reopen: job_running %d (a lookup: %d), dictionary reads %u -> %u", job_running, job_cursor_only, reads,
                cursor_reads);
    TEST_CHECK(DLOGGED("open: saved local word"));
    if (job_running) finish_by_tick();
    check_shown("猫犬鳥", "cat", 0);
    close_overlay(); mem_free();
}

/* #179's audit, O6: Select + R while Auto's OCR job reads the screen. The strip
 * waits for that job, whose end brings its sentence (on_job_done); nothing is
 * left waiting after it (strip_waiting is a lookup's or a refresh's only), so a
 * lookup the close drops later starts no job of the strip's and the sentence
 * stays. */
static void test_subtitles_on_during_an_ocr_job_leave_no_wait_behind(void)
{
    cursor_setup();
    cfg.highlight_mode = loaded.highlight_mode = VJO_HIGHLIGHT_CHARACTER; /* Auto's job reads the text only */
    stable_pending = 1; auto_prefetch();
    TEST_ASSERT(job_running && !job_cursor_only && !job_refresh);
    control_tick(VJO_EV_SUBTITLE); /* Select + R while Auto reads the screen */
    TEST_CHECK(subtitles && g_view.strip_busy && !strcmp(g_view.strip_text, "Recognizing…"));
    TEST_CHECK_(!strip_waiting, "strip_waiting %d for an OCR job", strip_waiting);
    finish_by_tick(); /* its sentence on the strip */
    TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !g_view.strip_busy && !strcmp(g_view.strip_text, "猫犬鳥"));
    TEST_CHECK(!strip_waiting);
    open_overlay(); /* the saved text: its word's lookup */
    TEST_ASSERT(job_running && job_cursor_only);
    next_scene(8);
    state.stable = 0; /* the next line types out */
    fake_recognized_text = "犬";
    unsigned inferences = vocr_allocs;
    close_overlay(); /* Cancel */
    finish_by_tick(); /* the dropped lookup */
    TEST_CHECK_(!job_running && vocr_allocs == inferences && !g_view.strip_busy && !strcmp(g_view.strip_text, "猫犬鳥"),
                "after the dropped lookup: job_running %d, jobs %u, strip '%s' (busy %d)", job_running,
                vocr_allocs - inferences, g_view.strip_text, g_view.strip_busy);
    subtitles = 0;
    if (job_running) finish_by_tick();
    mem_free();
}

/* Storage follows publication; the control thread may consume the result and
 * switch games before the writer returns. Only the snapshot is persisted. */
static void history_consume_result(void)
{
    int was_thread = test_thread;
    test_thread = CTL_THREAD;
    on_job_done();
    TEST_CHECK(!job_running && local_text_valid);
    vjo_anki_set_game("OTHER", "Another game", 0);
    strcpy(job_title, "OTHER");
    job_cfg.ocr_mode = VJO_OCR_ON_PRESS;
    test_thread = was_thread;
}

static void test_history_snapshots_auto_ocr_after_publication(void)
{
    cursor_setup();
    vjo_anki_set_game("ULJM00001", "Japanese PSP title", 1);
    history_on_append = history_consume_result;
    fake_exchange_us = 123456;
    start_job("history OCR only", 0);
    uint64_t started_at = (uint64_t)job_started_us;
    test_thread = NET_THREAD;
    net_job();
    test_thread = CTL_THREAD;
    TEST_CHECK(history_calls == 1 && !job_running);
    TEST_CHECK(!strcmp(last_history.name, "Japanese PSP title"));
    TEST_CHECK(!strcmp(last_history.title_id, "ULJM00001") && !strcmp(last_history.platform, "psp"));
    TEST_CHECK(!strcmp(history_text, "猫犬鳥") && last_history.result == &vocr_result);
    TEST_CHECK(!strcmp(last_history.model, VJO_VOCR_TIER2_NAME));
    TEST_CHECK(last_history.time_us == 1792220400250000ull + started_at);
    TEST_CHECK(last_history.monotonic_us == started_at && last_history.elapsed_us >= 123456);
    TEST_CHECK(last_history.scene == 2 && last_history.region_seq == 4);
    mem_free();
}

static void test_history_ignores_storage_failure_and_dictionary_oom(void)
{
    cursor_setup();
    fail_alloc = 1;
    history_write_fail = 1;
    start_job("history with no dictionary memory", 1);
    finish_current_job();
    TEST_CHECK(history_calls == 1 && !strcmp(history_text, "猫犬鳥"));
    TEST_CHECK(local_text_valid && !strcmp(local_text, "猫犬鳥"));
    TEST_CHECK(!has_result_memory());
    mem_free();
}

/* A native text hook's line is recorded as OCR's is (docs/ocr-history.md#line-source): the record is the
 * Automatically read's own result, whose text_from (here a string copy's stream, 9, locked, answering without
 * recognition) the writer turns into `engine` and `line_source` hook (test_shell_history.c checks the writer);
 * OCR's next line keeps 0. The request is the automatic pick both times. */
static void test_history_records_a_hooks_line_with_its_source(void)
{
    const uint32_t locked[7] = {9, VJO_HOOK_R_LOCKED | VJO_HOOK_R_NO_OCR, 1000, VJO_HOOK_STREAM_BIT(9),
                                VJO_TEXT_SOURCE_AUTO, 0u << 16 | 8u, 0};
    cursor_setup();
    memcpy(fake_hook, locked, sizeof(fake_hook));
    start_job("history: a hook's line", 0);
    finish_current_job();
    TEST_CHECK(fake_game_request.hook_pick == VJO_HOOK_PICK_AUTO);
    TEST_CHECK(history_calls == 1 && last_history.result == &vocr_result && !strcmp(history_text, "猫犬鳥"));
    TEST_CHECK_(history_text_from == 9, "the hook's record: text_from %u", (unsigned)history_text_from);
    memset(fake_hook, 0, sizeof(fake_hook));
    next_scene(8);
    fake_recognized_text = "犬猫";
    start_job("history: OCR's line", 0);
    finish_current_job();
    TEST_CHECK(fake_game_request.hook_pick == VJO_HOOK_PICK_AUTO);
    TEST_CHECK(history_calls == 2 && !strcmp(history_text, "犬猫"));
    TEST_CHECK_(history_text_from == 0, "OCR's record: text_from %u", (unsigned)history_text_from);
    mem_free();
}

static void test_history_records_refreshes_but_not_cursor_jobs(void)
{
    cursor_setup();
    open_overlay(); finish_current_job();
    TEST_ASSERT(history_calls == 1);
    vjo_post_cursor(3, local_text_generation); on_command();
    TEST_ASSERT(job_running && job_cursor_only);
    finish_current_job();
    TEST_CHECK(history_calls == 1);
    next_scene(8);
    fake_recognized_text = "犬猫";
    test_now += REFRESH_GAP_US + REFRESH_IDLE_US;
    control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    finish_current_job();
    TEST_CHECK(history_calls == 2 && !strcmp(history_text, "犬猫"));
    if (job_running) finish_current_job();
    TEST_CHECK(history_calls == 2);
    close_overlay(); mem_free();
}

static void test_history_excludes_disabled_failed_cancelled_and_invalid_clock(void)
{
    for (int kind = 0; kind < 5; kind++) {
        cursor_setup();
        if (kind == 0) cfg.ocr_mode = VJO_OCR_ON_PRESS;
        if (kind == 1) fail_vocr_user = 1;
        if (kind == 2) fake_cancel_on_read = 1;
        if (kind == 3) history_rtc_fail = 1;
        start_job("history exclusion", 0);
        if (kind == 4) game_generation++;
        finish_current_job();
        TEST_CHECK_(history_calls == 0, "excluded case %d wrote %u records", kind, history_calls);
        if (job_running) job_running = 0;
        mem_free();
    }
}

/* #179's audit, O2: a job of the previous game whose answer was already in
 * publishes that game's text after the switch retired it (run_vocr takes cmd_lock
 * after retire_local_text did). That job's end retires the text again (#186's
 * audit N1: a cancelled job's result goes, the text it published too), so Select
 * + R in the new game, before the kernel has sampled a new scene, never shows it
 * on the strip; a job of the strip's reads the new game's screen. subtitles_show's
 * check of the text's game stays as a second guard. */
static void test_the_previous_games_text_is_never_the_strips(void)
{
    cursor_setup();
    stable_pending = 1; auto_prefetch(); /* game A's job */
    TEST_ASSERT(job_running && !job_cursor_only);
    identify_rc = 0;
    memset(&identified, 0, sizeof(identified));
    snprintf(identified.key, sizeof(identified.key), "PCSG00202");
    identified.mode = VJO_GAME;
    state.game_pid = 101;
    control_tick(VJO_EV_GAME_START); /* game B in front, classified in the same wakeup */
    identify_rc = -1;
    TEST_ASSERT(game_active() && !strcmp(title_id, "PCSG00202") && job_cancelled && !local_text_valid);
    job_cancelled = 0; /* the answer was in: the text is published after the retirement */
    TEST_CHECK(run_job_t(&results[job_idx], &cache_data[job_idx]) == VJO_OK);
    job_cancelled = 1;
    state.stable = 0; /* nothing has settled in game B */
    job_done = 1; control_tick(0); /* the stale job's end shows nothing and retires its text */
    TEST_ASSERT(!job_running);
    TEST_CHECK_(!local_text_valid, "the previous game's text is valid after its job's end (its game %u, now %u)",
                local_text_game_generation, game_generation);
    control_tick(VJO_EV_SUBTITLE); /* Select + R in game B: still scene 2 */
    TEST_CHECK_(g_view.strip_kind == VJO_STRIP_STATUS && !strcmp(g_view.strip_text, "Recognizing…"),
                "game B's strip shows '%s'", g_view.strip_text);
    TEST_ASSERT(job_running && !job_cursor_only); /* game B's own */
    fake_recognized_text = "鳥";
    finish_by_tick();
    TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !g_view.strip_busy && !strcmp(g_view.strip_text, "鳥"));
    subtitles = 0;
    mem_free();
}

/* #179's audit, O5: the refreshes' backoff ends when a job reads its screen (or
 * a game starts), not when the overlay closes. Closed and reopened on the screen
 * its text was read from (the saved word: no job), the next settled screen still
 * waits for the failed refreshes' backoff (docs/touch-lookup.md: "until a job
 * reads its screen again"). */
static void test_the_refreshes_backoff_outlasts_a_close_with_no_job(void)
{
    cursor_setup();
    open_overlay(); finish_current_job();
    check_shown("猫犬鳥", "cat", 0);
    fail_vocr_user = 1; fake_free_user = 1000; /* the game has no memory for OCR now */
    next_scene(8);
    control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    finish_by_tick(); /* refused: the next refresh in 2 s */
    next_scene(9);
    test_now += BACKOFF_MIN_US; control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    finish_by_tick(); /* refused again: 4 s */
    TEST_CHECK(refresh_backoff.us == 2 * BACKOFF_MIN_US && !ocr_backoff.until);
    fail_vocr_user = 0; /* the memory comes back; nothing has read a screen yet */
    close_overlay(); /* Cancel */
    state.scene = state.region_scene = state.capture_scene = 2; /* the text's screen again */
    control_tick(0);
    TEST_CHECK(!job_running); /* Auto: the cached word is this screen's */
    open_overlay(); /* L + R: the saved word at once */
    TEST_CHECK(!job_running);
    check_shown("猫犬鳥", "cat", 0);
    next_scene(10);
    test_now += REFRESH_GAP_US; control_tick(VJO_EV_REGION_STABLE);
    TEST_CHECK_(!job_running && refresh_pending, "a refresh 2 s into the 4 s backoff (job %d)", job_running);
    test_now += 2 * BACKOFF_MIN_US - REFRESH_GAP_US; control_tick(0);
    TEST_ASSERT(job_running && job_refresh); /* the backoff is over */
    finish_by_tick(); /* it reads its screen: the backoff ends */
    TEST_CHECK(!refresh_backoff.us && !refresh_backoff.until);
    check_shown("猫犬鳥", "cat", 0);
    close_overlay(); mem_free();
}

/* #179's audit, O4: an opening during Auto's job (OV_OPEN_OLD_JOB) on a screen
 * that changed after the job's capture gets a recognition of its own when the
 * job ends (restart_old_job). Its reason is "result was for an earlier screen"
 * for a job that looked the word up, "the running job was OCR only" for one that
 * did not (tools/analyse_selftest.py PRESS_REASONS counts both). */
static void test_an_opening_during_autos_job_on_a_changed_screen_gets_its_own(void)
{
    for (int ocr_only = 0; ocr_only < 2; ocr_only++) {
        cursor_setup();
        if (ocr_only) cfg.highlight_mode = loaded.highlight_mode = VJO_HIGHLIGHT_CHARACTER;
        stable_pending = 1; auto_prefetch();
        TEST_ASSERT(job_running && !job_cursor_only && job_lookup == !ocr_only);
        open_overlay(); /* L + R before the job ends */
        TEST_CHECK(ov == OV_OPEN_OLD_JOB && view_waiting && !strcmp(g_view.status, "Recognizing…"));
        TEST_CHECK(run_job_t(&results[0], &cache_data[0]) == VJO_OK); /* it read scene 2 */
        next_scene(8); /* then the game moved on */
        fake_recognized_text = "犬";
        log_count = 0;
        job_done = 1; on_job_done();
        TEST_ASSERT(job_running && !job_cursor_only && ov == OV_OPEN);
        TEST_CHECK_(DLOGGED(ocr_only ? "job start (the running job was OCR only)"
                                     : "job start (result was for an earlier screen)"), "ocr_only %d", ocr_only);
        finish_by_tick();
        check_shown("犬", "dog", 0);
        close_overlay(); mem_free();
    }
}

/* #182 (a), #179's audit N4: a region drawn (Square, a box, Enter) while the
 * open overlay's lookup or refresh runs, the subtitle strip on. The region change
 * retires the text and the cached result, and the close drops that job, which
 * brings the strip nothing; the strip's job for the new region cannot start
 * beside it. The strip says "Recognizing…" and waits for the job's end
 * (strip_after_wait), which starts the strip's job: the new region's sentence.
 * Before, the strip kept the old region's sentence, not busy, until the new
 * region's screen settled (here no event comes), whether or not the kernel's
 * scene number had moved on. */
static void test_a_region_drawn_during_a_lookup_or_refresh_gives_the_strip_its_sentence(void)
{
    VjoRect r = {10, 10, 200, 60};
    for (int moved = 0; moved < 2; moved++) { /* the kernel's scene number not moved on yet, then moved on */
        cursor_saved_text(); /* Auto read 猫犬鳥 (Character mode: OCR only) */
        control_tick(VJO_EV_SUBTITLE); /* Select + R: its sentence */
        TEST_CHECK(subtitles && g_view.strip_kind == VJO_STRIP_SENTENCE && !strcmp(g_view.strip_text, "猫犬鳥"));
        open_overlay(); /* the saved text: its word's lookup */
        TEST_ASSERT(job_running && job_cursor_only);
        vjo_post_command(VJO_CMD_SET_REGION, &r); /* Square, a box, Enter */
        fake_recognized_text = "鳥"; /* the new region's text */
        if (moved) next_scene(30);
        unsigned inferences = vocr_allocs;
        control_tick(0); /* saved: the text and the cache retired, the overlay closed */
        TEST_ASSERT(ov == OV_CLOSED && !local_text_valid && job_running && job_cursor_only);
        TEST_CHECK_(g_view.strip_kind == VJO_STRIP_STATUS && g_view.strip_busy && !strcmp(g_view.strip_text, "Recognizing…"),
                    "moved %d, at the region change: strip '%s' (kind %d, busy %d)", moved, g_view.strip_text,
                    g_view.strip_kind, g_view.strip_busy);
        finish_by_tick(); /* the dropped lookup ends */
        TEST_CHECK_(job_running && !job_cursor_only,
                    "moved %d, after the dropped lookup: no job for the new region (job %d, strip '%s', busy %d)",
                    moved, job_running, g_view.strip_text, g_view.strip_busy);
        if (job_running) finish_by_tick(); /* the new region's sentence */
        TEST_CHECK_(g_view.strip_kind == VJO_STRIP_SENTENCE && !g_view.strip_busy && !strcmp(g_view.strip_text, "鳥"),
                    "moved %d: strip '%s' (kind %d, busy %d)", moved, g_view.strip_text, g_view.strip_kind,
                    g_view.strip_busy);
        TEST_CHECK(vocr_allocs == inferences + 1 && local_text_valid && !strcmp(local_text, "鳥") && !strip_waiting);
        subtitles = 0;
        mem_free();
    }
    /* The same with a refresh in progress. */
    cursor_setup();
    control_tick(VJO_EV_SUBTITLE); /* Select + R: nothing yet, the strip's job */
    TEST_ASSERT(job_running);
    finish_by_tick();
    TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !strcmp(g_view.strip_text, "猫犬鳥"));
    open_overlay(); /* the saved word */
    check_shown("猫犬鳥", "cat", 0);
    next_scene(9); /* a screen settles that the text was not read from: a refresh */
    control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh);
    vjo_post_command(VJO_CMD_SET_REGION, &r);
    fake_recognized_text = "鳥";
    control_tick(0);
    TEST_ASSERT(ov == OV_CLOSED && job_running && job_refresh && job_cancelled);
    TEST_CHECK_(g_view.strip_busy && !strcmp(g_view.strip_text, "Recognizing…"),
                "a refresh, at the region change: strip '%s' (busy %d)", g_view.strip_text, g_view.strip_busy);
    finish_by_tick(); /* the dropped refresh ends */
    TEST_CHECK_(job_running && !job_refresh && !job_cursor_only,
                "after the dropped refresh: no job for the new region (job %d, strip '%s')", job_running,
                g_view.strip_text);
    if (job_running) finish_by_tick();
    TEST_CHECK_(g_view.strip_kind == VJO_STRIP_SENTENCE && !g_view.strip_busy && !strcmp(g_view.strip_text, "鳥"),
                "a refresh: strip '%s' (kind %d, busy %d)", g_view.strip_text, g_view.strip_kind, g_view.strip_busy);
    subtitles = 0;
    mem_free();
}

/* #182 (a), an OCR job in progress: a region drawn while a job reads the old
 * region (the overlay opened during Auto's job, which it waits for; then Square, a
 * box, Enter, the job's result already in). The region change cancels that job:
 * its text and its lookup are the old region's, which the change retired. With
 * the strip on, the strip says "Recognizing…" and the job's end starts the
 * strip's job for the new region; with it off, Auto reads the new region. Before,
 * the job published them again after the change: its lookup cached as if the new
 * region's (a reopen showed it at once), and the strip showed the old region's
 * sentence until a screen settled. */
static void test_a_region_drawn_during_an_ocr_job_drops_its_old_regions_result(void)
{
    VjoRect r = {10, 10, 200, 60};
    for (int strip = 0; strip < 2; strip++) {
        cursor_setup(); /* Whole word: the job looks the first word up too */
        if (strip)
            control_tick(VJO_EV_SUBTITLE); /* Select + R: nothing yet, the strip's job reads the screen */
        else
            stable_pending = 1, auto_prefetch(); /* Auto reads the screen */
        TEST_ASSERT(job_running && !job_cursor_only && job_lookup);
        open_overlay(); /* L + R during it: the opening waits for it */
        TEST_CHECK(ov == OV_OPEN_OLD_JOB && view_waiting);
        TEST_CHECK(run_job_t(&results[0], &cache_data[0]) == VJO_OK); /* it has read the old region: 猫犬鳥 */
        job_done = 1;
        vjo_post_command(VJO_CMD_SET_REGION, &r); /* Enter, before the control thread takes the result */
        fake_recognized_text = "鳥"; /* the new region's text */
        control_tick(0); /* the region change, then the job's end (control_tick's order) */
        TEST_CHECK_(!cache_ok && !(local_text_valid && !strcmp(local_text, "猫犬鳥")),
                    "strip %d: the old region's result kept: cache_ok %d, text valid %d '%s'", strip, cache_ok,
                    local_text_valid, local_text);
        if (strip) {
            TEST_CHECK_(g_view.strip_kind == VJO_STRIP_STATUS && g_view.strip_busy &&
                        !strcmp(g_view.strip_text, "Recognizing…"),
                        "after the region change the strip shows '%s' (kind %d, busy %d)", g_view.strip_text,
                        g_view.strip_kind, g_view.strip_busy);
            TEST_CHECK_(job_running && !job_cursor_only, "no job of the strip's for the new region (job %d)",
                        job_running);
            if (job_running) finish_by_tick();
            TEST_CHECK_(g_view.strip_kind == VJO_STRIP_SENTENCE && !g_view.strip_busy && !strcmp(g_view.strip_text, "鳥"),
                        "strip '%s' (kind %d, busy %d)", g_view.strip_text, g_view.strip_kind, g_view.strip_busy);
            TEST_CHECK(!strip_waiting);
        } else {
            open_overlay(); /* L + R at once: the new region's recognition, never the old region's word */
            TEST_CHECK_(!strcmp(g_view.status, "Recognizing…") && !(g_view.list && g_view.list->n_entries),
                        "the reopen shows '%s' with %d entries", g_view.status,
                        g_view.list ? g_view.list->n_entries : -1);
            TEST_CHECK(job_running && !job_cursor_only);
            if (job_running) finish_by_tick();
            check_shown("鳥", "bird", 0);
            close_overlay();
        }
        subtitles = 0;
        mem_free();
    }
}

/* #182 (b), #179's audit N4: the strip's cached sentence is the shown text
 * (run_vocr reads it into local_text, cache_data[active].sentence), so it is the
 * strip's only while that text is valid. Select + R right after a direct switch to
 * another game (a GAME_START with no GAME_EXIT, classified in the same wakeup),
 * before the kernel samples a new scene, says "Recognizing…" and reads game B's
 * screen; so does Select + R right after a region was drawn with the strip off.
 * Before, the strip showed game A's sentence (the old region's) until a screen
 * settled. */
static void test_a_retired_texts_cached_sentence_is_never_the_strips(void)
{
    cursor_setup();
    stable_pending = 1; auto_prefetch(); finish_current_job(); /* game A: 猫犬鳥, its word cached */
    TEST_ASSERT(cache_ok && active == 0 && local_text_valid);
    identify_rc = 0;
    memset(&identified, 0, sizeof(identified));
    snprintf(identified.key, sizeof(identified.key), "PCSG00202");
    identified.mode = VJO_GAME;
    state.game_pid = 101;
    control_tick(VJO_EV_GAME_START); /* game B in front, classified in the same wakeup */
    identify_rc = -1;
    TEST_ASSERT(game_active() && !strcmp(title_id, "PCSG00202") && !local_text_valid);
    fake_recognized_text = "鳥";
    control_tick(VJO_EV_SUBTITLE); /* Select + R in game B: the kernel's scene number is still 2 */
    TEST_CHECK_(g_view.strip_kind == VJO_STRIP_STATUS && !strcmp(g_view.strip_text, "Recognizing…"),
                "game B's strip shows '%s'", g_view.strip_text);
    TEST_CHECK(job_running && !job_cursor_only); /* game B's own */
    if (job_running) finish_by_tick();
    TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !g_view.strip_busy && !strcmp(g_view.strip_text, "鳥"));
    subtitles = 0;
    mem_free();

    /* A region drawn with the strip off, then Select + R at once. */
    cursor_setup();
    stable_pending = 1; auto_prefetch(); finish_current_job();
    open_overlay(); /* the saved word, at once */
    check_shown("猫犬鳥", "cat", 0);
    VjoRect r = {10, 10, 200, 60};
    vjo_post_command(VJO_CMD_SET_REGION, &r); control_tick(0);
    TEST_ASSERT(ov == OV_CLOSED && !job_running && !local_text_valid);
    fake_recognized_text = "鳥";
    control_tick(VJO_EV_SUBTITLE); /* Select + R: the scene number has not moved on yet */
    TEST_CHECK_(g_view.strip_kind == VJO_STRIP_STATUS && !strcmp(g_view.strip_text, "Recognizing…"),
                "after the region change the strip shows '%s'", g_view.strip_text);
    TEST_CHECK(job_running && !job_cursor_only);
    if (job_running) finish_by_tick();
    TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !strcmp(g_view.strip_text, "鳥"));
    subtitles = 0;
    mem_free();
}

/* Square, a box, Enter, handled by the control thread (the hook between_answer_and_publish runs it). */
static void region_drawn_now(void)
{
    static const VjoRect r = {10, 10, 200, 60};
    vjo_post_command(VJO_CMD_SET_REGION, &r);
    on_command();
}

/* #186's audit N1: a region drawn after the game has answered a job but before
 * run_vocr publishes its text (the exchange's last cancel check is behind it). The
 * job publishes the old region's text after the change retired it; the cancelled
 * job's end retires it again: a cancelled job's result goes, the text it published
 * too. So neither the strip (on) nor an L + R at once (strip off) shows the old
 * region's text, and the new region is read. Before, the old text stayed valid
 * until the next job; while the kernel's scene number had not moved on, the strip
 * showed it and a reopen looked its word up. */
static void test_a_region_drawn_between_a_jobs_answer_and_its_publish_never_brings_its_text_back(void)
{
    for (int strip = 0; strip < 2; strip++) {
        for (int moved = 0; moved < 2; moved++) {
            cursor_setup();
            cfg.highlight_mode = loaded.highlight_mode = VJO_HIGHLIGHT_CHARACTER; /* the jobs read the text only */
            if (strip)
                control_tick(VJO_EV_SUBTITLE); /* Select + R: the strip's job reads the screen */
            else
                stable_pending = 1, auto_prefetch(); /* Auto's job reads the screen */
            TEST_ASSERT(job_running && !job_cursor_only);
            open_overlay(); /* L + R during it: the opening waits for it */
            TEST_ASSERT(ov == OV_OPEN_OLD_JOB);
            between_answer_and_publish = region_drawn_now; /* Enter, once the game has answered: 猫犬鳥 */
            int idx = job_idx;
            test_thread = NET_THREAD;
            job_rc = run_job_t(&results[idx], &cache_data[idx]);
            test_thread = CTL_THREAD;
            TEST_ASSERT(!between_answer_and_publish && job_cancelled && ov == OV_CLOSED);
            fake_recognized_text = "鳥"; /* the new region's text, for the jobs after it */
            if (moved) next_scene(30); /* the kernel sampled the new region before the job's end was taken */
            state.stable = 0;           /* not settled yet: no Auto job for it */
            job_done = 1; control_tick(0); /* the cancelled job's end */
            TEST_CHECK_(!(local_text_valid && !strcmp(local_text, "猫犬鳥")),
                        "strip %d moved %d: the old region's text is valid after the cancelled job's end", strip, moved);
            if (strip) {
                TEST_CHECK_(strcmp(g_view.strip_text, "猫犬鳥"),
                            "strip on, moved %d: the strip shows the old region's sentence (busy %d)", moved,
                            g_view.strip_busy);
                TEST_CHECK_(job_running && !job_cursor_only, "strip on, moved %d: no job for the new region", moved);
            } else {
                open_overlay(); /* L + R at once */
                TEST_CHECK_(!(g_view.list && g_view.list->header && !strcmp(g_view.list->header, "猫犬鳥")) &&
                            !strcmp(g_view.status, "Recognizing…"),
                            "strip off, moved %d: the reopen shows '%s' (status '%s', a lookup %d)", moved,
                            g_view.list && g_view.list->header ? g_view.list->header : "", g_view.status,
                            job_cursor_only);
                TEST_CHECK(job_running && !job_cursor_only);
            }
            if (job_running) finish_by_tick();
            if (strip)
                TEST_CHECK_(g_view.strip_kind == VJO_STRIP_SENTENCE && !g_view.strip_busy &&
                            !strcmp(g_view.strip_text, "鳥"), "strip on, moved %d: strip '%s' (busy %d)", moved,
                            g_view.strip_text, g_view.strip_busy);
            else
                check_shown("鳥", "bird", 0);
            close_overlay();
            subtitles = 0;
            mem_free();
        }
    }
}

/* #186's audit N2: the strip waits for any job in progress that brings it
 * nothing (subtitles_show): a lookup, a refresh, or one that was cancelled, as a
 * region change cancels the job it finds. Select + R off and on during the region
 * change's wait keeps it; Select + R on during a job the change cancelled with the
 * strip off waits for it too, and so does Select + R on during a job of the
 * previous game (a direct switch). Each end starts the strip's job. Before, the
 * strip said "Recognizing…" with no job until the new screen settled (here it does
 * not), and once the old region's text came back (N1) it showed that. */
static void test_select_r_during_a_cancelled_job_waits_for_its_end(void)
{
    static const VjoRect r = {10, 10, 200, 60};
    for (int how = 0; how < 3; how++) { /* 0: off and on during the wait; 1: on, strip off at the change; 2: a switch */
        cursor_setup();
        cfg.highlight_mode = loaded.highlight_mode = VJO_HIGHLIGHT_CHARACTER;
        if (!how)
            control_tick(VJO_EV_SUBTITLE); /* Select + R: the strip's job reads the screen */
        else
            stable_pending = 1, auto_prefetch(); /* Auto's job reads the screen */
        TEST_ASSERT(job_running && !job_cursor_only);
        if (how < 2) {
            open_overlay(); /* L + R during it */
            TEST_ASSERT(ov == OV_OPEN_OLD_JOB);
            next_scene(30); state.stable = 0; /* the region about to be drawn: a new scene, not settled */
            vjo_post_command(VJO_CMD_SET_REGION, &r); control_tick(0); /* the job cancelled */
            TEST_ASSERT(ov == OV_CLOSED && job_running && job_cancelled && strip_waiting == !how);
        } else {
            identify_rc = 0;
            memset(&identified, 0, sizeof(identified));
            snprintf(identified.key, sizeof(identified.key), "PCSG00202");
            identified.mode = VJO_GAME;
            state.game_pid = 101;
            state.stable = 0; /* game B's screen not settled yet */
            control_tick(VJO_EV_GAME_START); /* game B in front, classified at once: game A's job cancelled */
            identify_rc = -1;
            TEST_ASSERT(game_active() && job_running && job_cancelled);
        }
        if (!how)
            control_tick(VJO_EV_SUBTITLE); /* Select + R: off */
        control_tick(VJO_EV_SUBTITLE); /* Select + R: on, during the cancelled job */
        TEST_CHECK_(subtitles && g_view.strip_busy && !strcmp(g_view.strip_text, "Recognizing…") && strip_waiting,
                    "how %d: strip '%s' (busy %d), strip_waiting %d", how, g_view.strip_text, g_view.strip_busy,
                    strip_waiting);
        finish_by_tick(); /* the cancelled job ends: it read the old region (game A's screen): 猫犬鳥 */
        fake_recognized_text = "鳥";
        TEST_CHECK_(job_running && !job_cursor_only,
                    "how %d: after the cancelled job no job for the strip: strip '%s' (kind %d, busy %d)", how,
                    g_view.strip_text, g_view.strip_kind, g_view.strip_busy);
        TEST_CHECK(!strip_waiting);
        if (job_running) finish_by_tick();
        TEST_CHECK_(g_view.strip_kind == VJO_STRIP_SENTENCE && !g_view.strip_busy && !strcmp(g_view.strip_text, "鳥"),
                    "how %d: strip '%s' (kind %d, busy %d)", how, g_view.strip_text, g_view.strip_kind,
                    g_view.strip_busy);
        subtitles = 0;
        mem_free();
    }
}

/* #186's audit N3, X1: a region drawn with the strip on and no job running starts
 * the strip's job for the new region ("region changed"); the change cancels only a
 * job it finds, never that one. */
static void test_a_region_drawn_with_the_strip_on_and_no_job_starts_the_strips_own(void)
{
    static const VjoRect r = {10, 10, 200, 60};
    cursor_saved_text();
    control_tick(VJO_EV_SUBTITLE); /* Select + R: its sentence */
    TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !strcmp(g_view.strip_text, "猫犬鳥"));
    open_overlay(); finish_by_tick(); /* the saved word, shown: no job runs */
    TEST_ASSERT(!job_running);
    fake_recognized_text = "鳥";
    log_count = 0;
    vjo_post_command(VJO_CMD_SET_REGION, &r); control_tick(0);
    TEST_ASSERT_(job_running && !job_cursor_only, "no job for the new region");
    TEST_CHECK_(!job_cancelled && !strip_waiting && DLOGGED("job start (region changed)"),
                "the strip's own job: cancelled %d, strip_waiting %d", job_cancelled, strip_waiting);
    finish_by_tick();
    TEST_CHECK_(g_view.strip_kind == VJO_STRIP_SENTENCE && !g_view.strip_busy && !strcmp(g_view.strip_text, "鳥"),
                "strip '%s' (kind %d, busy %d)", g_view.strip_text, g_view.strip_kind, g_view.strip_busy);
    TEST_CHECK(local_text_valid && !strcmp(local_text, "鳥"));
    subtitles = 0;
    mem_free();
}

/* #186's audit N3, X8: an L + R during the strip's wait for a job the region
 * change cancelled. That job's end starts the opening's recognition, whose result
 * the strip takes, and leaves nothing waiting: a lookup the close drops later
 * starts no job of the strip's. */
static void test_a_reopen_during_the_strips_wait_leaves_nothing_waiting(void)
{
    static const VjoRect r = {10, 10, 200, 60};
    cursor_setup();
    cfg.highlight_mode = loaded.highlight_mode = VJO_HIGHLIGHT_CHARACTER;
    control_tick(VJO_EV_SUBTITLE); /* Select + R: the strip's job reads the screen */
    TEST_ASSERT(job_running && !job_cursor_only);
    open_overlay(); /* L + R: waits for it */
    TEST_ASSERT(ov == OV_OPEN_OLD_JOB);
    next_scene(30); state.stable = 0;
    vjo_post_command(VJO_CMD_SET_REGION, &r); control_tick(0); /* the job cancelled: the strip waits */
    TEST_ASSERT(ov == OV_CLOSED && job_running && job_cancelled && strip_waiting);
    control_tick(VJO_EV_TRIGGER); /* L + R during the wait */
    TEST_ASSERT(ov == OV_OPEN_OLD_JOB && !strcmp(g_view.status, "Recognizing…"));
    finish_by_tick(); /* the cancelled job ends (it read the old region): the opening's own recognition */
    fake_recognized_text = "犬猫";
    TEST_ASSERT(job_running && !job_cursor_only && ov == OV_OPEN);
    TEST_CHECK_(!strip_waiting, "strip_waiting %d after the job it waited for", strip_waiting);
    finish_by_tick();
    check_shown("犬猫", "dog", 0);
    TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !g_view.strip_busy && !strcmp(g_view.strip_text, "犬猫"));
    vjo_post_cursor(3, local_text_generation); on_command(); /* Right: a lookup */
    TEST_ASSERT(job_running && job_cursor_only);
    next_scene(40); /* the next line types out */
    unsigned inferences = vocr_allocs;
    close_overlay(); /* Cancel: the lookup is dropped */
    finish_by_tick();
    TEST_CHECK_(!job_running && vocr_allocs == inferences && !g_view.strip_busy && !strcmp(g_view.strip_text, "犬猫"),
                "after the dropped lookup: job_running %d, jobs %u, strip '%s' (busy %d)", job_running,
                vocr_allocs - inferences, g_view.strip_text, g_view.strip_busy);
    subtitles = 0;
    if (job_running) finish_by_tick();
    mem_free();
}

#if VJO_DEBUG /* a self-test test: the debug build's */
/* #182 (a), the self-test's job: never what the strip waits for. Its end shows
 * the strip the screen's sentence itself (on_selftest_done), or an opening during
 * it gets a recognition whose result the strip takes. A region drawn while it
 * still runs (the overlay opened, which stops it, then Square, a box, Enter before
 * its exchange has ended) leaves nothing waiting, so a lookup a close drops later
 * starts no job of the strip's. */
static void test_a_region_drawn_during_the_selftest_job_leaves_the_strip_no_wait(void)
{
    cursor_setup();
    game_generation = 1;
    selftest_armed = 1; selftest_done = 0; selftest_stage_count = 5; selftest_begins = 0;
    apply_config();
    stable_pending = 1; control_tick(0);
    TEST_ASSERT(job_running && !job_selftest);
    finish_current_job(); /* Auto read 猫犬鳥 and its word */
    control_tick(VJO_EV_SUBTITLE); /* Select + R: its sentence */
    TEST_CHECK(subtitles && !strcmp(g_view.strip_text, "猫犬鳥"));
    test_now += SELFTEST_IDLE_US; control_tick(0);
    TEST_ASSERT(job_running && job_selftest); /* the self-test's job */
    open_overlay(); /* L + R: it yields (its exchange ends a little later) */
    TEST_CHECK(job_cancelled && ov == OV_OPEN);
    VjoRect r = {10, 10, 200, 60};
    vjo_post_command(VJO_CMD_SET_REGION, &r); control_tick(0); /* Square, a box, Enter */
    TEST_CHECK_(!strip_waiting && ov == OV_CLOSED && job_running && job_selftest, "strip_waiting %d", strip_waiting);
    open_overlay(); /* L + R again: it waits for that job */
    TEST_CHECK(ov == OV_OPEN_OLD_JOB);
    fake_recognized_text = "犬猫"; /* the new region's text */
    finish_by_tick(); /* the self-test's job ends: the opening's own recognition */
    TEST_ASSERT(job_running && !job_selftest && !job_cursor_only);
    finish_by_tick(); /* its result, the strip's too */
    check_shown("犬猫", "dog", 0);
    TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !strcmp(g_view.strip_text, "犬猫") && !strip_waiting);
    vjo_post_cursor(3, local_text_generation); on_command(); /* Right: a lookup */
    TEST_ASSERT(job_running && job_cursor_only);
    next_scene(9);
    state.stable = 0; /* the next line types out */
    unsigned inferences = vocr_allocs;
    close_overlay(); /* Cancel: the lookup is dropped */
    finish_by_tick();
    TEST_CHECK_(!job_running && vocr_allocs == inferences && !g_view.strip_busy && !strcmp(g_view.strip_text, "犬猫"),
                "after the dropped lookup: job_running %d, jobs %u, strip '%s' (busy %d)", job_running,
                vocr_allocs - inferences, g_view.strip_text, g_view.strip_busy);
    subtitles = 0;
    selftest_armed = selftest_done = 0; selftest_stage_count = 0;
    if (job_running) finish_by_tick();
    mem_free();
}
#endif

#if VJO_DEBUG /* a self-test test: the debug build's */
/* Select + R during the self-test's job after the overlay stopped it: the strip
 * says "Recognizing…" but waits for nothing, as that job's end shows the strip
 * its sentence itself (on_selftest_done), never through strip_after_wait. So
 * nothing is left waiting after it, and a lookup a close drops later starts no
 * job of the strip's (subtitles_show leaves the self-test's job out of the jobs
 * it waits for, a cancelled one included). */
static void test_select_r_during_a_stopped_selftest_job_leaves_no_wait(void)
{
    cursor_setup();
    game_generation = 1;
    selftest_armed = 1; selftest_done = 0; selftest_stage_count = 5; selftest_begins = 0;
    apply_config();
    stable_pending = 1; control_tick(0);
    TEST_ASSERT(job_running && !job_selftest);
    finish_current_job(); /* Auto read 猫犬鳥 and its word */
    test_now += SELFTEST_IDLE_US; control_tick(0);
    TEST_ASSERT(job_running && job_selftest); /* the self-test's job */
    open_overlay(); /* L + R: it yields; the saved word shows */
    TEST_CHECK(job_cancelled);
    close_overlay(); /* Cancel, before that job's exchange has ended */
    control_tick(VJO_EV_SUBTITLE); /* Select + R during it */
    TEST_CHECK_(subtitles && !strip_waiting, "strip_waiting %d for the self-test's job", strip_waiting);
    finish_by_tick(); /* the self-test's job ends: the strip shows the cached sentence */
    TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !strcmp(g_view.strip_text, "猫犬鳥") && !strip_waiting);
    selftest_armed = selftest_done = 0; selftest_stage_count = 0;
    open_overlay(); /* the saved word, at once */
    TEST_ASSERT(!job_running);
    vjo_post_cursor(3, local_text_generation); on_command(); /* Right: a lookup */
    TEST_ASSERT(job_running && job_cursor_only);
    next_scene(9);
    state.stable = 0; /* the next line types out */
    unsigned inferences = vocr_allocs;
    close_overlay(); /* Cancel: the lookup is dropped */
    finish_by_tick();
    TEST_CHECK_(!job_running && vocr_allocs == inferences && !strcmp(g_view.strip_text, "猫犬鳥"),
                "after the dropped lookup: job_running %d, jobs %u, strip '%s' (busy %d)", job_running,
                vocr_allocs - inferences, g_view.strip_text, g_view.strip_busy);
    subtitles = 0;
    if (job_running) finish_by_tick();
    mem_free();
}
#endif

#if VJO_DEBUG /* a self-test test: the debug build's */
/* Subtitles turned on while the self-test's job runs show "Recognizing…" and
 * wait for a job's result, which that job never brings: once it ends, the strip
 * shows the screen's cached sentence, or a job reads a screen the cache is not
 * for (audit-overlay-first-ocr S1). */
static void test_subtitles_on_during_the_selftest_job(void)
{
    cursor_setup();
    game_generation = 1;
    selftest_armed = 1; selftest_done = 0; selftest_stage_count = 5; selftest_begins = 0;
    apply_config();
    stable_pending = 1; control_tick(0);
    TEST_ASSERT(job_running && !job_selftest);
    finish_current_job();
    test_now += SELFTEST_IDLE_US; control_tick(0);
    TEST_ASSERT(job_running && job_selftest);
    set_subtitles(1);
    TEST_CHECK(g_view.strip_kind == VJO_STRIP_STATUS && g_view.strip_busy && job_selftest);
    finish_current_job();
    TEST_CHECK(!job_running && g_view.strip_kind == VJO_STRIP_SENTENCE && !strcmp(g_view.strip_text, "猫犬鳥"));
    TEST_CHECK(!g_view.strip_busy);
    set_subtitles(0);
    /* ... on a screen the cache is not for (the next line, settled after the stages began): a job reads it */
    selftest_done = 0; selftest_begins = 0;
    test_now += 2 * SELFTEST_IDLE_US; control_tick(0);
    TEST_ASSERT(job_running && job_selftest);
    set_subtitles(1);
    next_scene(41); /* no event yet: the stages run on */
    fake_recognized_text = "鳥";
    finish_current_job();
    TEST_ASSERT(job_running && !job_selftest && logged("job start (subtitles on)") != NULL);
    finish_current_job();
    TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !strcmp(g_view.strip_text, "鳥"));
    set_subtitles(0);
    selftest_armed = selftest_done = 0; selftest_stage_count = 0;
    mem_free();
}
#endif

#if VJO_DEBUG /* the debug build's log lines (the normal build's: test_normal_build_*) */
/* A job's done line (net_job): the time in exchanges with the game worker, each
 * job's own, the log's appends meanwhile, and the lines held until its result (#360:
 * the text's one line and this one), in the format tools/analyse_timeline.py reads
 * (docs/first-ocr.md). */
static void test_the_done_line_has_the_exchange_and_log_times(void)
{
    reset();
    fake_recognized_text = "猫";
    fake_exchange_us = 7000;
    for (int k = 0; k < 2; k++) {
        int idx = -1, rc = -1, ms = -1, page = -1, pages = -1, end = -1, held = -1;
        unsigned peak = 0, game = 0, log_ms = 0, appends = 0;
        const char *line;
        raw_state.done_seq = 11;
        job_cfg = cfg;
        job_idx = 0; job_cursor_only = 0; job_cancelled = job_captured = 0; job_done = 0;
        net_job();
        TEST_CHECK(job_done && job_rc == VJO_OK && !open_batch && !net_held->b.buf); /* written: none held after */
        line = logged("job 0 done rc=0 in ");
        TEST_ASSERT(line != NULL);
        TEST_CHECK(sscanf(line, "job %d done rc=%d in %d ms, arena peak %u, dictionary page %d/%d, game %u ms, "
                          "log %u ms in %u appends, %d held%n", &idx, &rc, &ms, &peak, &page, &pages, &game, &log_ms,
                          &appends, &held, &end) == 10);
        TEST_CHECK(end > 0 && (line[end] == '\0' || line[end] == '\n')); /* to its last word */
        TEST_CHECK(idx == 0 && !rc && ms >= 7 && game == 7 && log_ms == 3 && appends == 2); /* each job's own */
        TEST_CHECK(held == 1); /* "game vocr text 0: 猫", written with this line after job_done */
        TEST_CHECK(strstr(file_text, "game vocr text 0: 猫\njob 0 done rc=0 in ") != NULL); /* in their order */
        TEST_MSG("%s", line);
        log_count = 0;
        mem_free();
    }
}

/* #360: at the network thread's notification of a job's end (job_done), what log.txt holds of its lines,
 * and the control thread taking the result there and then, as on the console it can while the network
 * thread goes on to write the held lines. */
static size_t done_file_len;
static unsigned done_lines_logged, done_lines_written, done_appends;
static int done_shown_entries; /* the entries the view showed once the control thread took the result */
static void take_the_result_at_job_done(void)
{
    if (test_thread != NET_THREAD || !job_done)
        return; /* (a job's text, job_text_ready, or another thread's notification) */
    done_file_len = file_len;
    done_lines_logged = lines_logged;
    done_lines_written = lines_appended;
    done_appends = appends_recorded;
    test_thread = CTL_THREAD;
    on_job_done();
    done_shown_entries = g_view.list == &cache_data[0].list ? g_view.list->n_entries : -1;
    test_thread = NET_THREAD;
}
/* Whether log.txt's bytes [from, to) hold needle (file_text: the lines as the appends wrote them). */
static int file_has_between(size_t from, size_t to, const char *needle)
{
    const char *p = strstr(file_text + from, needle);
    return p && (size_t)(p - file_text) + strlen(needle) <= to;
}
/* The network thread's job, with the control thread taking its result at job_done. */
static void net_job_taken_at_job_done(void)
{
    TEST_ASSERT(job_running && (posted & NET_EV_JOB));
    posted &= ~NET_EV_JOB;
    done_file_len = 0;
    done_shown_entries = -2;
    on_notify = take_the_result_at_job_done;
    test_thread = NET_THREAD;
    net_phase = job_cursor_only ? NET_RESULT : NET_IDLE;
    net_job();
    net_phase = NET_IDLE;
    test_thread = CTL_THREAD;
    on_notify = NULL;
    TEST_CHECK(!job_running && !net_held->b.buf && !open_batch); /* taken, and nothing held after the job */
}

/* #360: the debug build's lookup holds its lookup line and its done line until it has published its
 * result: when the control thread can take it (job_done), neither is in log.txt, and the word is shown;
 * then the network thread writes both, in their order, in one append, out of the result's path. */
static void test_debug_build_writes_a_lookups_lines_after_its_result(void)
{
    const char *lookup, *done;
    cursor_saved_text(); open_overlay(); finish_current_job();
    TEST_ASSERT(local_text_valid && !job_running);
    vjo_post_cursor(3, local_text_generation); on_command();
    TEST_ASSERT(job_running && job_cursor_only && job_cursor_byte == 3);
    unsigned appends0 = appends_recorded, logged0 = lines_logged;
    size_t file0 = file_len;
    net_job_taken_at_job_done();
    /* at job_done: both lines logged, neither written; the control thread shows the word then */
    TEST_CHECK(done_lines_logged == logged0 + 2 && done_lines_written == logged0 && done_appends == appends0);
    TEST_CHECK(done_file_len == file0 && !file_has_between(file0, done_file_len, "lookup cursor at 3:"));
    TEST_CHECK(done_shown_entries == 1); /* 犬 ("dog") on screen before its lines are written */
    /* then: one append of both, the lookup's line first, the done line saying it held one */
    TEST_ASSERT(appends_recorded == appends0 + 1);
    TEST_CHECK(appends_seen[appends0].lines == 2 && appends_seen[appends0].thread == NET_THREAD &&
               !appends_seen[appends0].in_result_path && !appends_seen[appends0].job_running);
    lookup = strstr(file_text + done_file_len, "lookup cursor at 3: 1 chars");
    done = strstr(file_text + done_file_len, "job 0 done rc=0 in ");
    TEST_ASSERT(lookup && done);
    TEST_CHECK(lookup < done && strchr(lookup, '\n') + 1 == done); /* in their order, back to back */
    TEST_CHECK(strstr(done, " appends, 1 held\n") != NULL);
    TEST_CHECK(lines_appended == lines_logged); /* nothing left held */
    TEST_MSG("%s", file_text + done_file_len);
    printf("\n  written after the result:\n%s", file_text + done_file_len);
    close_overlay();
    mem_free();
}

/* #360: an OCR job's lines after its recognition (its text's, its result arena's, its first word's lookup)
 * and its done line are held until its result is published, then written in one append; its recognition's
 * own lines (vocr_run_one's batch) go out as before, ahead of them. */
static void test_debug_build_writes_an_ocr_jobs_lines_after_its_result(void)
{
    const char *text, *lookup, *done;
    cursor_setup();
    fake_recognized_text = "猫犬鳥";
    cfg.ocr_mode = loaded.ocr_mode = VJO_OCR_AUTO;
    cfg.highlight_mode = loaded.highlight_mode = VJO_HIGHLIGHT_WORD; /* Auto + Whole word: the first word too */
    history_rtc_fail = 1; /* no history.csv line: the control thread takes the result before it (and resets job_done) */
    stable_pending = 1; auto_prefetch();
    TEST_ASSERT(job_running && job_lookup && !job_cursor_only);
    unsigned appends0 = appends_recorded;
    size_t file0 = file_len;
    net_job_taken_at_job_done();
    /* the recognition's batch was written before job_done; the text's lines were not */
    TEST_CHECK(done_appends > appends0 && file_has_between(file0, done_file_len, "game vocr rc=0 "));
    TEST_CHECK(!file_has_between(file0, done_file_len, "game vocr text 0: "));
    /* then one append, of every line held: the text's, the lookup's and the done line, in their order */
    TEST_ASSERT(appends_recorded == done_appends + 1);
    TEST_CHECK(!appends_seen[done_appends].in_result_path && appends_seen[done_appends].thread == NET_THREAD);
    TEST_CHECK(appends_seen[done_appends].lines == done_lines_logged - done_lines_written &&
               appends_seen[done_appends].lines >= 3);
    text = strstr(file_text + done_file_len, "game vocr text 0: 猫犬鳥");
    lookup = strstr(file_text + done_file_len, "lookup first word at 0: ");
    done = strstr(file_text + done_file_len, "job 0 done rc=0 in ");
    TEST_ASSERT(text && lookup && done);
    TEST_CHECK(text == file_text + done_file_len && text < lookup && lookup < done); /* in their order */
    TEST_CHECK(lines_appended == lines_logged);
    TEST_MSG("%s", file_text + done_file_len);
    printf("\n  written after the result:\n%s", file_text + done_file_len);
    mem_free();
}

/* #360: an OCR job whose held lines fill the 2 KiB batch: the batch writes what it holds before the next line
 * (an append before the result, as the normal build's batch does when full) and goes on holding; the done line
 * says how many it still held, and every line is written once, in its order. */
static void test_debug_build_a_full_batch_goes_on_holding(void)
{
    static char text[16 * 200];
    const char *done;
    size_t n = 0;
    cursor_setup();
    for (int line = 0; line < 12; line++) { /* 12 lines of 60 characters: 18 + 180 B a log line */
        for (int c = 0; c < 60; c++) {
            memcpy(text + n, "猫", 3);
            n += 3;
        }
        text[n++] = '\n';
    }
    text[n - 1] = 0;
    fake_recognized_text = text;
    fake_lines = 12;
    cfg.ocr_mode = loaded.ocr_mode = VJO_OCR_AUTO;
    cfg.highlight_mode = loaded.highlight_mode = VJO_HIGHLIGHT_WORD;
    history_rtc_fail = 1;
    stable_pending = 1; auto_prefetch();
    TEST_ASSERT(job_running && job_lookup && !job_cursor_only);
    size_t file0 = file_len;
    net_job_taken_at_job_done();
    /* the batch filled at its 10th text line (10 x 199 B; an 11th does not fit 2,048 B): written before the result */
    TEST_CHECK(file_has_between(file0, done_file_len, "game vocr text 9: "));
    TEST_CHECK(!file_has_between(file0, done_file_len, "game vocr text 10: "));
    done = strstr(file_text + done_file_len, "job 0 done rc=0 in ");
    TEST_ASSERT(done != NULL);
    /* still held when the done line was logged: the lines written after the result before it (text lines 10 and
     * 11, raw and shown here, the workspace line and the lookup's) */
    {
        unsigned before = 0, held = 0;
        for (const char *p = file_text + done_file_len; p < done; p++)
            before += *p == '\n';
        TEST_CHECK(sscanf(strstr(done, " appends, ") + 10, "%u held", &held) == 1);
        TEST_CHECK(held == before && before >= 4);
        TEST_MSG("%u held, %u written after the result before the done line", held, before);
    }
    TEST_CHECK(strstr(file_text + done_file_len, "game vocr text 10: ") == file_text + done_file_len);
    TEST_CHECK(lines_appended == lines_logged && !open_batch);
    TEST_MSG("%s", done);
    mem_free();
}

/* #360: the press-to-screen figure (shell/lookup_shown.h). A post records its cursor_seq and the time; the
 * control thread publishes the lookup's result with its cursor_seq and its job's job_done time; the overlay's
 * render (here: the test, under the view lock as render holds it) takes the figure once, only for the result
 * of the latest request, drawn for the cursor it asked for; a close forgets a request not shown yet. */
static void test_debug_build_lookup_shown_figure(void)
{
    VjoShown s;
    char line[128];
    memset(&s, 0, sizeof(s));
    cursor_saved_text(); open_overlay(); finish_current_job();
    uint32_t generation = local_text_generation;
    TEST_CHECK(!vjo_shown_take(&s, &g_view, 1)); /* the opening's own lookup: nothing asked for */
    /* 1. a move: 30 ms of lookup, then 12 ms to the frame */
    test_now = 50000000;
    vjo_post_cursor(3, generation);
    TEST_CHECK(g_view.ask_seq == local_cursor_seq && g_view.ask_us == 50000000u && g_view.shown_seq != g_view.ask_seq);
    TEST_CHECK(!vjo_shown_take(&s, &g_view, 1)); /* "Looking up words…": not its result */
    on_command(); TEST_ASSERT(job_running && job_cursor_only);
    test_now += 30000;
    finish_current_job();
    TEST_CHECK(g_view.shown_seq == g_view.ask_seq && g_view.done_us == (uint32_t)test_now);
    test_now += 12000;
    TEST_CHECK(vjo_shown_take(&s, &g_view, 1) && s.n == 1 && s.sum_ms == 42 && s.max_ms == 42 && s.after_ms == 12);
    TEST_CHECK(!g_view.ask_seq && !vjo_shown_take(&s, &g_view, 1) && s.n == 1); /* once */
    /* 2. two moves before the first's result: the first is never shown, the second from its own post */
    vjo_post_cursor(6, generation); on_command(); TEST_ASSERT(job_running);
    test_now += 5000;
    vjo_post_cursor(0, generation); /* while the first runs */
    uint32_t second = (uint32_t)test_now;
    test_now += 20000;
    finish_current_job(); /* the first's result is not published: the second's job starts */
    TEST_ASSERT(job_running && job_cursor_byte == 0);
    TEST_CHECK(g_view.shown_seq != g_view.ask_seq && !vjo_shown_take(&s, &g_view, 1));
    test_now += 25000;
    finish_current_job();
    test_now += 8000;
    TEST_CHECK(vjo_shown_take(&s, &g_view, 1) && s.n == 2);
    TEST_CHECK(s.sum_ms == 42 + ((uint32_t)test_now - second) / 1000u && s.max_ms == 53 && s.after_ms == 12 + 8);
    /* 3. drawn for another cursor (the stick moved on before its result): forgotten, never counted later */
    vjo_post_cursor(6, generation); on_command(); finish_current_job();
    TEST_CHECK(g_view.ask_seq && g_view.shown_seq == g_view.ask_seq);
    TEST_CHECK(!vjo_shown_take(&s, &g_view, 0) && !g_view.ask_seq); /* the stick's cursor is elsewhere */
    TEST_CHECK(!vjo_shown_take(&s, &g_view, 1) && s.n == 2); /* back on the word: no late figure */
    /* 4. a close forgets a request whose result was published but not drawn: a reopen's saved word is no figure */
    vjo_post_cursor(3, generation); on_command(); finish_current_job();
    TEST_CHECK(g_view.ask_seq && g_view.shown_seq == g_view.ask_seq);
    close_overlay();
    TEST_CHECK(!g_view.ask_seq);
    open_overlay();
    if (job_running) finish_current_job();
    TEST_CHECK(!vjo_shown_take(&s, &g_view, 1) && s.n == 2);
    /* 5. the process time's low 32 bits wrap between the post and the frame */
    vjo_post_cursor(6, generation);
    g_view.ask_us = 0xFFFFF000u; /* 4,096 us before the wrap */
    on_command(); finish_current_job();
    g_view.done_us = 0x00000800u; /* published 2,048 us after it */
    {
        uint64_t keep = test_now;
        test_now = 0x100004000ull; /* 16,384 us after it: 20 ms after the post, 14 after the result */
        TEST_CHECK(vjo_shown_take(&s, &g_view, 1) && s.n == 3);
        test_now = keep;
    }
    TEST_CHECK(s.sum_ms == 42 + 53 + 20 && s.max_ms == 53 && s.after_ms == 20 + 14);
    /* the close line's figure: the means of the three, the largest */
    snprintf(line, sizeof(line), VJO_SHOWN_FORMAT, VJO_SHOWN_ARGS(&s));
    TEST_CHECK(!strcmp(line, ", lookups shown 3: mean 38 ms (11 ms after the result), max 53 ms"));
    TEST_MSG("%s", line);
    memset(&s, 0, sizeof(s)); /* none shown: means of 0, no division */
    snprintf(line, sizeof(line), VJO_SHOWN_FORMAT, VJO_SHOWN_ARGS(&s));
    TEST_CHECK(!strcmp(line, ", lookups shown 0: mean 0 ms (0 ms after the result), max 0 ms"));
    s.n = s.sum_ms = s.max_ms = s.after_ms = UINT32_MAX; /* 58 B of text and its numbers: at most 4 x 10 */
    TEST_CHECK(snprintf(line, sizeof(line), VJO_SHOWN_FORMAT, VJO_SHOWN_ARGS(&s)) == 58 + 10 + 1 + 1 + 10);
    close_overlay();
    mem_free();
}
#endif

#if VJO_DEBUG /* a self-test test: the debug build's */
/* The self-test's stages publish captures of their own, so the kernel's
 * region_seq moves on: a shown text that matched the screen only through its
 * capture (an icon masked after it, a new scene id without an event) is tagged
 * with the scene first, so a press after the stages still looks it up instead
 * of recognizing the screen again (audit-overlay-first-ocr). */
static void test_selftest_keeps_the_shown_texts_screen(void)
{
  for (int word = 0; word < 2; word++) {
    cursor_setup();
    if (!word) /* Character mode: no cached lookup; Whole word: a cached lookup, retagged too */
        cfg.highlight_mode = loaded.highlight_mode = VJO_HIGHLIGHT_CHARACTER;
    game_generation = 1;
    selftest_armed = 1; selftest_done = 0; selftest_stage_count = 5;
    apply_config();
    stable_pending = 1; control_tick(0);
    TEST_ASSERT(job_running && !job_selftest);
    finish_current_job();
    TEST_ASSERT(local_text_valid);
    state.scene = state.region_scene = 9; /* the icon masked: a new id, the same capture */
    TEST_CHECK(shows_screen(local_text_scene, local_text_capture_seq, &state) && local_text_scene != 9);
    test_now += SELFTEST_IDLE_US; control_tick(0);
    TEST_ASSERT(job_running && job_selftest);
    finish_current_job();
    TEST_CHECK(selftest_done);
    state.region_seq += 5; /* its stages' captures */
    unsigned n = submits;
    open_overlay();
    if (word) {
        TEST_CHECK(!job_running && submits == n && logged("open: saved local word") != NULL); /* at once */
    } else {
        TEST_ASSERT(job_running && job_cursor_only && submits == n); /* a lookup of the shown text, no OCR */
        finish_current_job();
    }
    check_shown("猫犬鳥", "cat", 0);
    close_overlay();
    selftest_armed = selftest_done = 0; selftest_stage_count = 0;
    mem_free();
  }
}
#endif

#if VJO_DEBUG /* a self-test test: the debug build's */
/* The overlay opened during the self-test's job keeps its text when the job ends,
 * whatever the screen did meanwhile (#81 request 4): with subtitles on, the strip
 * shows that text and starts no job of its own (one would retire the text and show
 * "Recognizing…"); a new screen is the quiet refresh's, whose text the strip then
 * shows (audit-overlay-first-ocr-b RB1, its PROBE7). */
static void test_selftest_end_keeps_the_open_overlays_text(void)
{
    for (int same = 0; same < 2; same++) {
        cursor_setup(); /* Whole word: the saved word shows at once */
        game_generation = 1;
        selftest_armed = 1; selftest_done = 0; selftest_stage_count = 5; selftest_begins = 0;
        apply_config();
        stable_pending = 1; control_tick(0);
        TEST_ASSERT(job_running && !job_selftest);
        finish_current_job();
        TEST_ASSERT(local_text_valid && cache_ok);
        set_subtitles(1);
        TEST_CHECK(!job_running && g_view.strip_kind == VJO_STRIP_SENTENCE);
        test_now += SELFTEST_IDLE_US; control_tick(0);
        TEST_ASSERT(job_running && job_selftest);
        open_overlay(); /* it yields; the saved word shows at once */
        TEST_CHECK(job_cancelled && logged("open: saved local word") != NULL);
        check_shown("猫犬鳥", "cat", 0);
        next_scene(41);
        control_tick(VJO_EV_REGION_STABLE); /* a new screen settles before the stage ends */
        fake_recognized_text = same ? "猫犬鳥" : "犬猫";
        finish_current_job(); /* the self-test's job ends */
        TEST_CHECK(!job_running && !logged("job start (subtitles on)"));
        TEST_CHECK(local_text_valid && !strcmp(local_text, "猫犬鳥") && strcmp(g_view.status, "Recognizing…"));
        check_shown("猫犬鳥", "cat", 0);
        TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !g_view.strip_busy &&
                   !strcmp(g_view.strip_text, "猫犬鳥"));
        test_now += REFRESH_GAP_US + REFRESH_IDLE_US; control_tick(0);
        TEST_ASSERT(job_running && job_refresh && !job_lookup); /* the quiet refresh */
        finish_current_job();
        TEST_CHECK(local_text_valid && !strcmp(local_text, same ? "猫犬鳥" : "犬猫"));
        TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !strcmp(g_view.strip_text, same ? "猫犬鳥" : "犬猫"));
        TEST_MSG("same %d: strip [%s], overlay [%s]", same, g_view.strip_text, local_text);
        while (job_running) finish_current_job(); /* the new text's first word */
        close_overlay();
        set_subtitles(0);
        selftest_armed = selftest_done = 0; selftest_stage_count = 0;
        mem_free();
    }
}
#endif

#if VJO_DEBUG /* a self-test test: the debug build's */
/* Subtitles turned on during the self-test's job wait in "Recognizing…"; an overlay
 * opened next on a text without its lookup (Character mode) waits for the job too.
 * When the job ends, the lookup runs and the strip shows the overlay's text at once,
 * not after the stages run again (audit-overlay-first-ocr-b, its PROBE10). */
static void test_selftest_end_shows_the_open_overlays_text_in_the_strip(void)
{
    cursor_setup();
    cfg.highlight_mode = loaded.highlight_mode = VJO_HIGHLIGHT_CHARACTER;
    game_generation = 1;
    selftest_armed = 1; selftest_done = 0; selftest_stage_count = 5; selftest_begins = 0;
    apply_config();
    stable_pending = 1; control_tick(0);
    TEST_ASSERT(job_running && !job_selftest);
    finish_current_job();
    test_now += SELFTEST_IDLE_US; control_tick(0);
    TEST_ASSERT(job_running && job_selftest);
    set_subtitles(1);
    TEST_CHECK(g_view.strip_kind == VJO_STRIP_STATUS && g_view.strip_busy);
    unsigned n = submits;
    open_overlay(); /* it yields; the lookup waits for the job */
    TEST_CHECK(job_cancelled && cursor_waiting);
    finish_current_job(); /* the self-test's job ends: the lookup runs */
    TEST_ASSERT(job_running && job_cursor_only);
    TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !g_view.strip_busy && !strcmp(g_view.strip_text, "猫犬鳥"));
    n = submits;
    finish_current_job();
    TEST_CHECK(submits == n && !job_running); /* a lookup, no OCR */
    check_shown("猫犬鳥", "cat", 0);
    close_overlay();
    set_subtitles(0);
    selftest_armed = selftest_done = 0; selftest_stage_count = 0;
    mem_free();
}
#endif

#if VJO_DEBUG /* a self-test test: the debug build's */
/* The self-test's job keeps the capture of the text it runs on: the stages'
 * own captures (published by the kernel as any job's) never stand in for it,
 * so a new screen still stops the job, whichever capture a stage published or
 * voided last. */
static void test_selftest_job_keeps_the_texts_capture(void)
{
    reset();
    job_capture_scene = 7; job_capture_seq = 9; job_captured = 1;
    job_selftest = 1;
    vocr_capture_fixed = 0;
    state.done_seq = vocr_done_before + 1; /* a stage's first pass is in */
    state.capture_scene = 12; state.region_seq = 13;
    vocr_note_capture();
    TEST_CHECK(job_capture_scene == 7 && job_capture_seq == 9 && job_captured);
    state.region_seq = 0; /* a stage's try voided its capture */
    vocr_note_capture();
    TEST_CHECK(job_captured && job_capture_scene == 7);
    job_selftest = 0; /* any other job takes the published capture */
    state.region_seq = 13;
    vocr_note_capture();
    TEST_CHECK(job_capture_scene == 12 && job_capture_seq == 13 && job_captured);
}
#endif

#if VJO_DEBUG /* a self-test test: the debug build's */
/* The open overlay's refresh runs while the self-test is armed: the stages run
 * in the self-test's own job only (job_selftest), never in a job the overlay
 * waits for or in a refresh, and never while the overlay is open. */
static void test_refresh_while_the_selftest_is_armed(void)
{
    cursor_setup();
    game_generation = 1;
    selftest_armed = 1; selftest_done = 0; selftest_stage_count = 5; selftest_begins = 0;
    open_overlay(); finish_current_job();
    check_shown("猫犬鳥", "cat", 0);
    TEST_CHECK(!selftest_begins); /* the overlay's own job ran no stage */
    next_scene(8);
    test_now += REFRESH_GAP_US; control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && job_refresh && !job_selftest);
    finish_current_job();
    TEST_CHECK(!selftest_begins && logged("refresh rc=0: same text") != NULL);
    check_shown("猫犬鳥", "cat", 0);
    for (int i = 0; i < 3; i++) {
        test_now += 2 * SELFTEST_IDLE_US; control_tick(0);
        TEST_CHECK(!job_selftest && !selftest_begins); /* never with the overlay open */
        if (job_running) finish_current_job();
    }
    selftest_armed = selftest_done = 0; selftest_stage_count = 0;
    close_overlay(); mem_free();
}
#endif

#if VJO_DEBUG /* a self-test test: the debug build's */
/* test.2's first scans ran beside the self-test's stages and took 9-15 s
 * (docs/first-ocr.md). The user's jobs never run them now: the first settled
 * screen gets a plain job, and once a job read text the stages run in a
 * background job of their own on that screen, with the overlay closed and the
 * Shell idle for SELFTEST_IDLE_US; it changes neither the text nor its lookup.
 * Opening the overlay stops it at once (the saved word shows without waiting
 * for it), and so does a new screen, whose own job then comes first; the stages
 * run again later, from the first. */
static void test_selftest_runs_after_the_first_text_in_its_own_job(void)
{
    cursor_setup();
    game_generation = 1;
    selftest_armed = 1; selftest_done = 0; selftest_stage_count = 5;
    selftest_begins = selftest_ends = submits = 0;
    apply_config();
    stable_pending = 1; control_tick(0); /* the first settled screen: a plain Auto job */
    TEST_ASSERT(job_running && !job_selftest && job_lookup);
    finish_current_job();
    TEST_CHECK(submits == 1 && !selftest_begins && local_text_valid && !strcmp(local_text, "猫犬鳥") && cache_ok);
    TEST_CHECK(text_generation == game_generation);
    uint32_t generation = local_text_generation, cursor_seq = local_cursor_seq;
    control_tick(0);
    TEST_CHECK(!job_running); /* not before the Shell has been idle SELFTEST_IDLE_US */
    test_now += SELFTEST_IDLE_US - 1; control_tick(0);
    TEST_CHECK(!job_running);
    test_now += 1; control_tick(0);
    TEST_ASSERT(job_running && job_selftest && !job_lookup && job_idx == 1);
    TEST_CHECK(logged("job start (self-test)") != NULL);
    int64_t started = job_started_us;
    test_now += 2 * SELFTEST_IDLE_US; control_tick(0);
    TEST_CHECK(job_started_us == started); /* never a second job while one runs */
    finish_current_job();
    TEST_CHECK(selftest_begins == 5 && selftest_ends == 5 && submits == 1 + 5 && selftest_done && !job_running);
    for (unsigned i = 0; i < 1 + 5; i++) /* Auto's job and every stage: the 2 s limit, not a press's 1 s */
        TEST_CHECK((selftest_flags_seen[i] >> VJO_GAME_OCR_SETTLE_MAX_SHIFT & 0xFFu) == 40u);
    TEST_CHECK(local_text_valid && !strcmp(local_text, "猫犬鳥") && local_text_generation == generation);
    TEST_CHECK(local_cursor_seq == cursor_seq && cache_ok && active == 0);
    unsigned n = submits;
    open_overlay(); /* the prepared word, at once: no job */
    TEST_CHECK(logged("open: saved local word") != NULL && !job_running && submits == n);
    check_shown("猫犬鳥", "cat", 0);
    close_overlay();
    test_now += 2 * SELFTEST_IDLE_US; control_tick(0);
    TEST_CHECK(!job_running); /* done: once a boot */

    /* Armed again: the overlay opened while the stages run stops them, and the saved
     * word shows at once; they wait while it is open and run again after it closes. */
    selftest_done = 0; selftest_begins = selftest_ends = submits = 0;
    next_scene(30); control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && !job_selftest);
    finish_current_job();
    state.stable = 0; next_scene(33); /* the next line typing out: not settled, not read */
    test_now += 2 * SELFTEST_IDLE_US; control_tick(0);
    TEST_CHECK(!job_running); /* never on a screen the last job did not read */
    state.stable = 1; next_scene(30); /* the box shows the text read again */
    test_now += SELFTEST_IDLE_US; control_tick(0);
    TEST_ASSERT(job_running && job_selftest);
    open_overlay();
    TEST_CHECK(job_cancelled && logged("self-test: stopped for the overlay") != NULL);
    TEST_CHECK(logged("open: saved local word") != NULL);
    check_shown("猫犬鳥", "cat", 0);
    finish_current_job();
    TEST_CHECK(!job_running && !selftest_done && !selftest_begins && submits == 1);
    test_now += 2 * SELFTEST_IDLE_US; control_tick(0);
    TEST_CHECK(!job_running); /* not while the overlay is open */
    close_overlay();
    control_tick(0);
    TEST_CHECK(!job_running); /* idle first */
    test_now += SELFTEST_IDLE_US; control_tick(0);
    TEST_ASSERT(job_running && job_selftest); /* again, on the same screen */

    /* A new screen stops it; that screen's own job runs first, then the stages. */
    next_scene(31); control_tick(VJO_EV_REGION_STABLE);
    TEST_CHECK(job_cancelled);
    finish_current_job();
    TEST_CHECK(!job_running && !selftest_done);
    control_tick(0);
    TEST_ASSERT(job_running && !job_selftest); /* scene 31's plain job */
    finish_current_job();
    TEST_CHECK(!selftest_begins);
    test_now += SELFTEST_IDLE_US; control_tick(0);
    TEST_ASSERT(job_running && job_selftest);
    finish_current_job();
    TEST_CHECK(selftest_begins == 5 && selftest_ends == 5 && selftest_done);

    /* The overlay opened while the stages run on a screen the shown text is not
     * from (the next line, not settled yet): it waits for its own recognition,
     * which starts once the self-test's job has stopped. */
    selftest_done = 0; selftest_begins = 0;
    test_now += 2 * SELFTEST_IDLE_US; control_tick(0);
    TEST_ASSERT(job_running && job_selftest);
    state.stable = 0; next_scene(34);
    fake_recognized_text = "鳥";
    open_overlay();
    TEST_CHECK(job_cancelled && ov == OV_OPEN_OLD_JOB && !strcmp(g_view.status, "Recognizing…"));
    finish_current_job();
    TEST_ASSERT(job_running && !job_selftest && job_lookup && ov == OV_OPEN);
    state.stable = 1;
    finish_current_job();
    check_shown("鳥", "bird", 0);
    close_overlay();
    /* ... and on the text's own screen with its lookup gone: the lookup waits for the
     * stopped job, then runs. */
    test_now += 2 * SELFTEST_IDLE_US; control_tick(0);
    TEST_ASSERT(job_running && job_selftest);
    cache_ok = 0;
    open_overlay();
    TEST_CHECK(job_cancelled && cursor_waiting && ov == OV_OPEN);
    finish_current_job();
    TEST_ASSERT(job_running && job_cursor_only);
    finish_current_job();
    check_shown("鳥", "bird", 0);
    close_overlay();
    TEST_CHECK(!selftest_done && !selftest_begins);

    /* No text in the region: the self-test waits for a job that reads some. */
    selftest_done = 0; selftest_begins = 0;
    fake_recognized_text = "";
    next_scene(32); control_tick(VJO_EV_REGION_STABLE);
    TEST_ASSERT(job_running && !job_selftest);
    finish_current_job();
    TEST_CHECK(!text_generation);
    test_now += 2 * SELFTEST_IDLE_US; control_tick(0);
    TEST_CHECK(!job_running && !selftest_begins);
    selftest_armed = selftest_done = 0; selftest_stage_count = 0;
    mem_free();
}
#endif
#endif


#if !VJO_DEBUG
/* ---- the normal build's short log (docs/release-variants.md) ---- */

/* A job's line for the first job of each activation of the game (a start or a resume) that read
 * its screen, for its first job that found a line, and for every failed one (a cancelled job, or one
 * whose screen changed under it, is retried quietly: no line): its result, the size
 * that ran, the lines and the time; never the recognized text, the capture, a passed memory check,
 * the step log or the timings of every job. */
static void test_normal_build_logs_the_first_jobs_and_failures(void)
{
    reset();
    fake_recognized_text = "猫が鳴いた";
    fake_steps = 1;
    game_generation = job_generation = 51;
    fake_lines = 0; /* a title screen: the first job finds no line */
    TEST_CHECK(run_vocr_job() == VJO_OK);
    const char *line = logged("ocr rc=0 ");
    static const char first0[] = "ocr rc=0 " VJO_VOCR_TIER2_NAME ": 0 lines, ", first2[] = "ocr rc=0 " VJO_VOCR_TIER2_NAME ": 2 lines, ";
    TEST_CHECK(line && !strncmp(line, first0, sizeof(first0) - 1));
    TEST_MSG("%s", line ? line : "(none)");
    TEST_CHECK(log_count == 1);
    mem_free();
    log_count = 0;
    fake_lines = 2; /* the first job that finds a line: its line too */
    TEST_CHECK(run_vocr_job() == VJO_OK && !strcmp(cache_data[0].sentence, "猫が鳴いた"));
    line = logged("ocr rc=0 ");
    TEST_CHECK(line && !strncmp(line, first2, sizeof(first2) - 1));
    TEST_CHECK(log_count == 1); /* that line only */
    for (unsigned i = 0; i < log_count && i < 32; i++)
        TEST_CHECK_(!strstr(log_lines[i], "猫") && !strstr(log_lines[i], "game vocr") && !strstr(log_lines[i], "gstep"),
                    "%s", log_lines[i]);
    mem_free();
    /* the same activation's next jobs, with or without lines: nothing */
    log_count = 0;
    TEST_CHECK(run_vocr_job() == VJO_OK && log_count == 0);
    mem_free();
    fake_lines = 0;
    TEST_CHECK(run_vocr_job() == VJO_OK && log_count == 0);
    fake_lines = 2;
    mem_free();
    /* a screen that changed under every read is retried quietly: nothing */
    fake_screen_changes = 1;
    TEST_CHECK(run_vocr_job() == VJO_E_SCREEN_CHANGED && log_count == 0);
    mem_free();
    /* a failed one: its line, every time, and its refused memory check */
    fail_vocr_user = 1;
    TEST_CHECK(run_vocr_job() == VJO_E_OOM && logged("ocr rc=-103 " VJO_VOCR_TIER2_NAME ": ") != NULL &&
               logged("check refused") != NULL);
    log_count = 0;
    TEST_CHECK(run_vocr_job() == VJO_E_OOM && logged("ocr rc=-103 ") != NULL);
    fail_vocr_user = 0;
    mem_free();
    /* the next activation (a resume moves the generation on too): its first job again */
    log_count = 0;
    job_generation = 52;
    TEST_CHECK(run_vocr_job() == VJO_OK && logged("ocr rc=0 " VJO_VOCR_TIER2_NAME ": 2 lines") != NULL && log_count == 1);
    mem_free();
}

/* A model size that did not fit: its lines once per activation, and again when what did not fit
 * (or the size the job went on to) changes; the debug build logs them for every job. */
static void test_normal_build_logs_tier_misses_once_and_on_change(void)
{
    reset();
    cfg.vocr_tier = VJO_VOCR_TIER_LARGE;
    fake_recognized_text = "猫";
    memcpy(job_title, "PCSG00415", 10);
    game_generation = job_generation = 61;
    fake_tier_checks = 1u << (VJO_VOCR_TIER_LARGE - 1u); /* Large does not fit */
    TEST_CHECK(run_vocr_job() == VJO_OK && logged("tier large did not fit in PCSG00415") != NULL);
    mem_free();
    log_count = 0;
    TEST_CHECK(run_vocr_job() == VJO_OK && !logged("did not fit")); /* the same: nothing */
    mem_free();
    fake_tier_checks = 1u << (VJO_VOCR_TIER_LARGE - 1u) | 1u << (VJO_VOCR_TIER_NORMAL - 1u); /* now Normal neither */
    TEST_CHECK(run_vocr_job() == VJO_OK && logged("tier large did not fit") && logged("tier normal did not fit") &&
               logged("; using small") != NULL);
    mem_free();
    log_count = 0;
    TEST_CHECK(run_vocr_job() == VJO_OK && !logged("did not fit"));
    mem_free();
    job_generation = 62; /* the next activation: once again */
    TEST_CHECK(run_vocr_job() == VJO_OK && logged("tier normal did not fit") != NULL);
    mem_free();
    fake_tier_checks = 0;
}

/* The 444 MHz boost's one line: the first job of a game and whenever it changes, never per job. */
static void test_normal_build_logs_the_clock_once_and_on_change(void)
{
    reset();
    fake_recognized_text = "猫";
    fake_clock_on = 1; fake_clock_rc = 0;
    fake_clock_mhz[0] = 333; fake_clock_mhz[1] = 444; fake_clock_mhz[2] = 333;
    game_generation = job_generation = 41;
    TEST_CHECK(run_vocr_job() == VJO_OK && logged("clock: 333 -> 444 MHz while reading, 333 after") != NULL);
    mem_free();
    log_count = 0;
    TEST_CHECK(run_vocr_job() == VJO_OK && !logged("clock:")); /* the same clock: nothing */
    mem_free();
    fake_clock_rc = 1; fake_clock_mhz[0] = fake_clock_mhz[1] = fake_clock_mhz[2] = 500; /* PSVshell's 500 */
    TEST_CHECK(run_vocr_job() == VJO_OK && logged("clock: 500 MHz, left as it is while reading (444 or more)") != NULL);
    mem_free();
    log_count = 0;
    fake_clock_rc = (int32_t)0x80010016; fake_clock_mhz[0] = 333; fake_clock_mhz[1] = 333; fake_clock_mhz[2] = 333;
    TEST_CHECK(run_vocr_job() == VJO_OK && logged("clock: 444 MHz refused 0x80010016, 333 MHz while reading") != NULL);
    mem_free();
    log_count = 0;
    job_generation = 42; /* the next game: its first job says it again */
    TEST_CHECK(run_vocr_job() == VJO_OK && logged("clock: 444 MHz refused") != NULL);
    mem_free();
    log_count = 0;
    fake_clock_on = 0; /* ocr_boost_clock = off: no step, no line */
    job_generation = 43;
    TEST_CHECK(run_vocr_job() == VJO_OK && !logged("clock:"));
    mem_free();
}

/* A lookup's line only when it failed, without its text. */
static void test_normal_build_logs_a_failed_lookup_only(void)
{
    VjoOverlayData out;
    memset(&out, 0, sizeof(out));
    out.sentence = "猫が鳴いた";
    log_count = 0;
    log_lookup("cursor", &out, 3, VJO_OK, 0);
    TEST_CHECK(log_count == 0);
    log_lookup("cursor", &out, 3, VJO_E_OOM, 0);
    TEST_CHECK(log_count == 1 && logged("lookup cursor at 3 failed: rc=") != NULL && !logged("猫"));
}

/* No self-test: never armed, no stage, no job of its own; an armed marker is said so once. */
static void test_normal_build_has_no_selftest(void)
{
    reset();
    selftest_marker = 1; selftest_marker_reads = 0;
    vjo_selftest_init(&cfg);
    TEST_CHECK(selftest_marker_reads == 1 &&
               logged("self-test: armed, but this is the normal build: install the -debug build to run it") != NULL);
    TEST_CHECK(!vjo_selftest_armed() && !vjo_selftest_stages());
    log_count = 0; selftest_marker = 0;
    vjo_selftest_init(&cfg);
    TEST_CHECK(log_count == 0); /* not armed: nothing */
}

/* audit-overlay-release-variants-b note A: a job cancelled during its exchange (a new screen, a game
 * exit: vocr_cancelled) logs no job line, as one whose screen changed under it, and leaves the
 * activation's first job line to the next job that reads its screen. What a result that arrived says
 * is logged as from any job's (audit-t3-log-path N2): a size that did not fit and the clock once per
 * activation, a refused memory check for each job; the boost or the memory miss happened whether or not
 * its text was wanted. */
static void test_normal_build_cancelled_job_is_quiet(void)
{
    const unsigned large = 1u << (VJO_VOCR_TIER_LARGE - 1u);
    for (int kind = 0; kind < 3; kind++) { /* 0: nothing in the result; 1: Large refused and the clock; 2: OOM */
        reset();
        fake_recognized_text = "猫";
        cfg.vocr_tier = kind == 1 ? VJO_VOCR_TIER_LARGE : VJO_VOCR_TIER_NORMAL;
        memcpy(job_title, "PCSG00415", 10);
        fake_clock_on = kind == 1; fake_clock_rc = 0;
        fake_clock_mhz[0] = 333; fake_clock_mhz[1] = 444; fake_clock_mhz[2] = 333;
        fake_tier_checks = kind == 1 ? large : 0;
        fail_vocr_user = kind == 2;
        game_generation = job_generation = 81u + (uint32_t)kind;
        fake_cancel_on_read = 1; /* the job is cancelled while the game reads; its result arrives */
        TEST_CHECK(run_vocr_job() == VJO_E_CANCELLED);
        TEST_CHECK(!logged("ocr rc=")); /* no job line */
        TEST_CHECK(log_count == (kind == 1 ? 2u : kind == 2 ? 1u : 0u));
        TEST_CHECK(kind != 1 || (logged("tier large did not fit in PCSG00415") && logged("clock: 333 -> 444 MHz")));
        TEST_CHECK(kind != 2 || logged("check refused") != NULL);
        TEST_MSG("kind %d: %u line(s), the first '%s'", kind, log_count, log_count ? log_lines[0] : "");
        mem_free();
        if (kind == 2) { /* another cancelled job with a refused check: its line again (each job's, as any job's) */
            log_count = 0;
            fake_cancel_on_read = 1;
            TEST_CHECK(run_vocr_job() == VJO_E_CANCELLED && log_count == 1 && logged("check refused") != NULL &&
                       !logged("ocr rc="));
            mem_free();
        }
        /* the next job that reads its screen: the activation's first job line; the rest only once */
        log_count = 0;
        fail_vocr_user = 0;
        TEST_CHECK(run_vocr_job() == VJO_OK && logged("ocr rc=0 ") != NULL);
        TEST_CHECK(log_count == 1 && !logged("clock:") && !logged("did not fit"));
        TEST_MSG("kind %d, the next job: %u line(s)", kind, log_count);
        mem_free();
        fake_clock_on = 0; fake_tier_checks = 0;
    }
}

/* audit-overlay-release-variants-b note A: a refused input block is logged, on open (the overlay cannot
 * open) and on close; one that works is not. */
static void test_normal_build_logs_refused_input_blocks(void)
{
    reset();
    fail_input_block = 1;
    open_overlay();
    TEST_CHECK(ov == OV_CLOSED && log_count == 1 &&
               logged("input block refused 0xFFFFFF85: the overlay cannot open") != NULL);
    fail_input_block = 0;
    log_count = 0;
    open_overlay();
    TEST_CHECK(input_blocked && job_running && log_count == 0);
    fail_input_unblock = 1;
    close_overlay();
    TEST_CHECK(ov == OV_CLOSED && log_count == 1 && logged("input block off refused 0x80010016") != NULL);
    fail_input_unblock = 0;
    log_count = 0;
    open_overlay();
    close_overlay();
    TEST_CHECK(log_count == 0);
    job_running = 0;
    mem_free();
}


/* ---- the normal build: no log append on a job's result path (t3-log-path) ----
 * The owner (2026-10-16, test.3): the normal build is the logless one. The network thread keeps a log
 * batch open (net_main), which holds every line it logs, and writes it, one append per file, once its
 * wait, timed while lines are held, ends after LOG_IDLE_US (0.5 s) with no job, the control thread having
 * taken the job's result (net_main). The logger stub records each append with the thread that made it and its phase (net_phase):
 * in a job's result path (NET_RESULT: the result known, until job_done; a lookup's whole run) or while the
 * game recognizes (NET_RECOGNIZING). */

/* The network thread's appends from record mark on: their count and lines (and how many were made while
 * the game recognized a job); none may be in a result path. */
static __attribute__((unused)) unsigned net_appends_since(unsigned mark, unsigned *lines, unsigned *recognizing)
{
    unsigned n = 0;
    if (lines) *lines = 0;
    if (recognizing) *recognizing = 0;
    for (unsigned i = mark; i < appends_recorded && i < APPEND_LOG; i++) {
        if (appends_seen[i].thread != NET_THREAD) continue;
        TEST_CHECK_(!appends_seen[i].in_result_path, "append %u (%u lines) in a job's result path", i,
                    appends_seen[i].lines);
        n++;
        if (lines) *lines += appends_seen[i].lines;
        if (recognizing) *recognizing += (unsigned)appends_seen[i].recognizing;
    }
    return n;
}
static int held_lines(void) { return open_batch == &net_log ? batch_lines_held : 0; }

static void test_normal_build_writes_a_jobs_lines_after_its_result(void)
{
    unsigned mark, lines, recognizing, count;
    cursor_setup(); /* Whole word, Auto, the test dictionary: a job looks its first word up */
    fake_clock_on = 1; fake_clock_mhz[0] = 333; fake_clock_mhz[1] = 444; fake_clock_mhz[2] = 333;
    game_generation = 100;

    /* 1. An activation's first job (Auto, the overlay closed): its line and the clock's are held through
     *    the exchange's end, the first word's lookup and the control thread's taking of the result; they go
     *    out when the network thread's wait times out with no job. */
    mark = appends_recorded;
    next_scene(30); stable_pending = 1; control_tick(0);
    TEST_ASSERT(job_running && (posted & NET_EV_JOB));
    as_network_thread(); /* the job: the exchange, its lines, the lookup; job_done */
    TEST_CHECK(job_done && held_lines() == 2 && appends_recorded == mark); /* held: nothing written yet */
    TEST_CHECK(logged("ocr rc=0 " VJO_VOCR_TIER2_NAME ": 2 lines, ") != NULL && logged("clock: 333 -> 444 MHz") != NULL);
    TEST_CHECK(net_log.n == 2); /* what net_main's next wait reads: a timed one */
    as_network_thread(); /* a timeout before the control thread took the result: nothing (its job runs) */
    TEST_CHECK(held_lines() == 2 && appends_recorded == mark);
    control_tick(0); /* the result is the control thread's */
    TEST_CHECK(!job_running && cache_ok && appends_recorded == mark);
    as_network_thread(); /* the wait times out, no job runs: the held lines go out */
    TEST_CHECK(net_appends_since(mark, &lines, &recognizing) == 1 && lines == 2 && !recognizing);
    TEST_CHECK(appends_recorded == mark + 1 && !appends_seen[mark].job_running && !open_batch &&
               net_log.n == 0); /* written and closed (the next job opens it): the next wait is untimed */

    /* 2. Its next job: no line, no append. */
    mark = appends_recorded; count = log_count;
    next_scene(31); stable_pending = 1; control_tick(0);
    TEST_ASSERT(job_running);
    as_network_thread(); control_tick(0); as_network_thread();
    TEST_CHECK(!job_running && log_count == count && appends_recorded == mark && net_log.n == 0);

    /* 3. A job the open overlay waits for ("Recognizing…") that fails: its line and its refused memory
     *    check are held until the error is shown. */
    fail_vocr_user = 1;
    mark = appends_recorded;
    next_scene(32); open_overlay();
    TEST_ASSERT(job_running && ov == OV_OPEN && !strcmp(g_view.status, "Recognizing…"));
    as_network_thread();
    TEST_CHECK(job_done && appends_recorded == mark && logged("ocr rc=-103 ") != NULL && logged("check refused") != NULL);
    control_tick(0);
    TEST_CHECK(g_view.status_is_error && appends_recorded == mark);
    as_network_thread();
    TEST_CHECK(net_appends_since(mark, &lines, NULL) == 1 && lines == 2 && appends_recorded == mark + 1);
    fail_vocr_user = 0;
    close_overlay();
    test_now += BACKOFF_MAX_US; /* past the failure's backoff */

    /* 4. A failed lookup: its line waits until its result (the error) is shown. */
    next_scene(33); stable_pending = 1; control_tick(0);
    as_network_thread(); control_tick(0); as_network_thread();
    open_overlay(); /* the saved word, at once */
    TEST_ASSERT(!job_running && local_text_valid);
    mark = appends_recorded;
    read_fail = 1;
    vjo_post_cursor(3, local_text_generation); control_tick(0);
    TEST_ASSERT(job_running && job_cursor_only);
    as_network_thread();
    TEST_CHECK(job_done && appends_recorded == mark && logged("lookup cursor at 3 failed: rc=") != NULL);
    control_tick(0); /* on_cursor_done: the error is shown */
    TEST_CHECK(!job_running && g_view.status_is_error && appends_recorded == mark);
    as_network_thread();
    TEST_CHECK(net_appends_since(mark, &lines, NULL) == 1 && lines == 1);
    read_fail = 0;

    /* 5. Lookups the user starts while a job's lines are held (the next activation's first job, its
     *    result shown, the cursor moved before the wait timed out): they keep them; out after the last. */
    close_overlay();
    game_generation = 101; /* a resume */
    mark = appends_recorded;
    next_scene(34); open_overlay();
    TEST_ASSERT(job_running && !job_cursor_only);
    as_network_thread();
    control_tick(0); /* the result is shown */
    TEST_CHECK(!job_running && g_view.open && g_view.list && g_view.list->n_entries == 1);
    for (int k = 0; k < 3; k++) {
        vjo_post_cursor(k & 1 ? 3 : 6, local_text_generation); control_tick(0);
        TEST_ASSERT(job_running && job_cursor_only);
        as_network_thread(); /* the lookup (the wait ended by its job: no write) */
        control_tick(0);
        TEST_CHECK(!job_running && appends_recorded == mark && held_lines() == 2);
    }
    check_shown(local_text, "bird", 6);
    as_network_thread(); /* the timeout */
    TEST_CHECK(net_appends_since(mark, &lines, &recognizing) == 1 && lines == 2 && !recognizing);
    close_overlay();

    /* 6. An OCR job asked for before the wait times out (a new screen settles at once): it keeps the held
     *    lines across its exchange (none goes out while the game reads, none after its result), and they
     *    go out with its own after its result is taken. */
    game_generation = 102;
    mark = appends_recorded;
    next_scene(35); stable_pending = 1; control_tick(0);
    as_network_thread();
    next_scene(36);
    control_tick(VJO_EV_REGION_STABLE); /* takes the result, then starts the new screen's job */
    TEST_ASSERT(job_running && !job_cursor_only);
    TEST_CHECK(held_lines() == 2 && appends_recorded == mark);
    as_network_thread(); /* the new job */
    TEST_CHECK(job_done && held_lines() == 2 && appends_recorded == mark);
    control_tick(0); as_network_thread();
    TEST_CHECK(net_appends_since(mark, &lines, &recognizing) == 1 && lines == 2 && !recognizing);

    /* 7. A model that cannot be used, and the smaller one read instead: all four lines (its first try's
     *    job line, the unusable model, the retry's job line and the clock's) held across the retry's
     *    exchange, then one append after the result. */
    game_generation = 103;
    fake_missing_model = VJO_VOCR_TIER2_NAME;
    mark = appends_recorded; log_count = 0;
    next_scene(37); stable_pending = 1; control_tick(0);
    as_network_thread();
    TEST_CHECK(job_done && log_count == 4 && held_lines() == 4 && appends_recorded == mark);
    TEST_CHECK(logged("ocr rc=-115 " VJO_VOCR_TIER2_NAME) != NULL && logged("is unusable here") != NULL &&
               logged("ocr rc=0 " VJO_VOCR_TIER1_NAME ": 2 lines") != NULL);
    control_tick(0); as_network_thread();
    TEST_CHECK(net_appends_since(mark, &lines, &recognizing) == 1 && lines == 4 && !recognizing);
    fake_missing_model = NULL;

    /* 7b. The overlay's open re-reads config.ini (vjo_log_configure): with log_file kept the held lines
     *     stay held (the stub as shell/log_vita.c's: shell_log's configure_keeps_a_held_batch), as in 8;
     *     normal_build_open_in_the_pause_writes_nothing has the open in the pause itself. */

    /* 8. A game without its worker (VJO_E_OCR_UNAVAILABLE): the submission is refused; the earlier job's
     *    held lines and its own stay held, across the overlay's open, until its error is shown. */
    game_generation = 104;
    mark = appends_recorded;
    next_scene(38); stable_pending = 1; control_tick(0);
    as_network_thread();
    control_tick(0); /* the activation's first job: 2 lines held */
    TEST_CHECK(held_lines() == 2);
    fake_submit_rc = VJO_ERR_NO_GAME; /* #138: no worker was loaded at the game's start */
    next_scene(39); open_overlay();
    as_network_thread();
    TEST_CHECK(job_done && logged("ocr rc=-117 ") != NULL && appends_recorded == mark && held_lines() == 3);
    control_tick(0);
    TEST_CHECK(g_view.status_is_error); /* its error is shown */
    as_network_thread();
    TEST_CHECK(net_appends_since(mark, &lines, NULL) == 1 && lines == 3);
    fake_submit_rc = 0;
    close_overlay();
    TEST_CHECK(result_path_appends == 0 && lines_appended == lines_logged); /* every line written, none held */
    TEST_MSG("%u lines logged, %u written, %u appends in a result path", lines_logged, lines_appended,
             result_path_appends);
    fake_clock_on = 0;
    mem_free();
}

/* net_main itself, from its start to its stop: its wait is untimed while nothing is held, timed
 * (LOG_IDLE_US) once a job logged lines; the timeout writes them once the result is taken; a job that
 * comes before the timeout keeps them (no write at a job's wake); the stop writes what is still held
 * before the batch's stack goes. */
static unsigned net_main_mark;
static int net_main_stop_early;
static int script_first_job(int step)
{
    switch (step) {
    case 0: /* the thread's first wait: nothing held, untimed; the control thread starts a job */
        TEST_CHECK(last_wait_timeout == 0 && !open_batch); /* the first job opens the batch */
        net_main_mark = appends_recorded; /* after the thread's start: its stack paint may log a line */
        next_scene(60u + (unsigned)net_main_stop_early); stable_pending = 1; control_tick(0);
        TEST_CHECK(job_running && (posted & NET_EV_JOB));
        return 1;
    case 1: /* the job ran: 2 lines held, so this wait is timed; it times out before the result is taken */
        TEST_CHECK(last_wait_timeout == 500000u /* 0.5 s */ && held_lines() == 2 && appends_recorded == net_main_mark);
        return 1;
    case 2: /* nothing written by that timeout (its job still ran); the result is taken, and a second
             * (quiet) job starts before the next timeout */
        TEST_CHECK(last_wait_timeout == 500000u && appends_recorded == net_main_mark);
        control_tick(0);
        TEST_CHECK(!job_running);
        next_scene(70u + (unsigned)net_main_stop_early); stable_pending = 1; control_tick(0);
        TEST_CHECK(job_running && (posted & NET_EV_JOB));
        return 1;
    case 3: /* the second job ran at that wake: the first job's lines still held (no write at a job's wake) */
        TEST_CHECK(last_wait_timeout == 500000u && held_lines() == 2 && appends_recorded == net_main_mark);
        if (net_main_stop_early) /* the worker stops before the control thread took this result: the wait */
            return 0;            /* ends with a job running, so only net_main's end writes what is held */
        control_tick(0); /* its result is taken */
        TEST_CHECK(!job_running);
        return 1;
    default: /* the timeout wrote them; nothing held now: untimed */
        TEST_CHECK(last_wait_timeout == 0 && appends_recorded == net_main_mark + 1 && net_log.n == 0);
        return 0;
    }
}
static void test_normal_build_net_main_writes_held_lines(void)
{
    for (net_main_stop_early = 0; net_main_stop_early < 2; net_main_stop_early++) {
        unsigned lines;
        cursor_setup();
        vjo_log_batch_end(&net_log); /* the harness's batch: net_main makes its own */
        fake_clock_on = 1; fake_clock_mhz[0] = 333; fake_clock_mhz[1] = 444; fake_clock_mhz[2] = 333;
        game_generation = 130u + (uint32_t)net_main_stop_early;
        wait_script = script_first_job; wait_step = 0;
        test_thread = NET_THREAD;
        net_main(0, NULL); /* the network thread, from its start to its stop */
        test_thread = CTL_THREAD;
        wait_script = NULL;
        TEST_CHECK(wait_step == (net_main_stop_early ? 4 : 5) && !open_batch); /* its batch went with it */
        TEST_CHECK(!net_log.buf); /* and no pointer into its stack frame is left */
        TEST_CHECK(net_appends_since(net_main_mark, &lines, NULL) == 1 && lines == 2);
        if (net_main_stop_early) { job_running = 0; job_done = 0; } /* the result never taken: drop it */
        fake_clock_on = 0;
        running = 1;
        reset();
    }
}

/* The real net_main over more jobs (audit-t3-log-path-d's audit_net_main_more_jobs, extended): after a
 * write the next job opens the batch again at its wake, a single held line (a failed lookup) times the
 * wait too, and once the batch is open a quiet job's wake takes no log lock (net_log_wake). */
static unsigned more_jobs_mark, more_jobs_locks;
static int script_more_jobs(int step)
{
    switch (step) {
    case 0: /* the first wait: untimed; job A, an activation's first: its line and the clock's */
        TEST_CHECK(last_wait_timeout == 0 && !open_batch && !net_log.buf);
        more_jobs_mark = appends_recorded;
        next_scene(90); stable_pending = 1; control_tick(0);
        TEST_CHECK(job_running && (posted & NET_EV_JOB));
        return 1;
    case 1: /* A ran: 2 held, a timed wait; A's result is taken during it, then it times out */
        TEST_CHECK(last_wait_timeout == 500000u && held_lines() == 2 && appends_recorded == more_jobs_mark);
        control_tick(0);
        TEST_CHECK(!job_running && local_text_valid);
        return 1;
    case 2: /* the timeout wrote A's lines and closed the batch: untimed. A failed lookup, job B */
        TEST_CHECK(last_wait_timeout == 0 && appends_recorded == more_jobs_mark + 1 && !open_batch && !net_log.buf &&
                   net_log.n == 0);
        open_overlay(); /* the saved word, at once */
        TEST_CHECK(!job_running && g_view.open);
        read_fail = 1;
        more_jobs_locks = net_log_locks;
        vjo_post_cursor(3, local_text_generation); control_tick(0);
        TEST_CHECK(job_running && job_cursor_only && (posted & NET_EV_JOB));
        return 1;
    case 3: /* B ran in a batch opened again at its wake (one lock): its one line held, the wait timed */
        TEST_CHECK(last_wait_timeout == 500000u && held_lines() == 1 && appends_recorded == more_jobs_mark + 1);
        TEST_CHECK(net_log_locks == more_jobs_locks + 1 && logged("lookup cursor at 3 failed: rc=") != NULL);
        control_tick(0); /* its error is shown */
        TEST_CHECK(!job_running && g_view.status_is_error);
        read_fail = 0;
        return 1;
    case 4: /* the timeout wrote B's line: untimed. A lookup that finds its word, job C: quiet */
        TEST_CHECK(last_wait_timeout == 0 && appends_recorded == more_jobs_mark + 2 && !open_batch && net_log.n == 0);
        vjo_post_cursor(6, local_text_generation); control_tick(0);
        TEST_CHECK(job_running && job_cursor_only);
        return 1;
    case 5: /* C opened the batch and held nothing: untimed, the batch open. Its result; job D, quiet */
        TEST_CHECK(last_wait_timeout == 0 && open_batch == &net_log && net_log.buf && net_log.n == 0 &&
                   appends_recorded == more_jobs_mark + 2);
        control_tick(0);
        TEST_CHECK(!job_running);
        more_jobs_locks = net_log_locks;
        vjo_post_cursor(3, local_text_generation); control_tick(0);
        TEST_CHECK(job_running && job_cursor_only);
        return 1;
    default: /* D's wake found the batch open: no lock, nothing written */
        TEST_CHECK(last_wait_timeout == 0 && net_log_locks == more_jobs_locks && appends_recorded == more_jobs_mark + 2);
        control_tick(0);
        TEST_CHECK(!job_running);
        return 0;
    }
}
static void test_normal_build_net_main_more_jobs(void)
{
    unsigned lines = 0;
    cursor_setup();
    fake_clock_on = 1; fake_clock_mhz[0] = 333; fake_clock_mhz[1] = 444; fake_clock_mhz[2] = 333;
    game_generation = 140;
    wait_script = script_more_jobs; wait_step = 0;
    test_thread = NET_THREAD;
    net_main(0, NULL);
    test_thread = CTL_THREAD;
    wait_script = NULL;
    TEST_CHECK(wait_step == 7 && !open_batch && !net_log.buf);
    TEST_CHECK(net_appends_since(more_jobs_mark, &lines, NULL) == 2 && lines == 3);
    TEST_MSG("%d waits, %u lines in the network thread's appends", wait_step, lines);
    fake_clock_on = 0;
    running = 1;
    close_overlay();
    mem_free();
    reset();
}

/* L + R within the pause after a job that held lines (an activation's first): the overlay's open re-reads
 * config.ini (apply_config, vjo_log_configure) before it shows the saved result. With log_file kept the
 * configure keeps the held lines (shell/log_vita.c), so nothing is written before the overlay shows the
 * result, and the network thread's timeout writes them after. config.ini's log_file changed meanwhile
 * writes them at the open, on the control thread (audit-t3-log-path-d's B2a and N1; its
 * audit_open_writes_the_held_lines_first, with the configure as main's). */
static void test_normal_build_open_in_the_pause_writes_nothing(void)
{
    unsigned mark, lines;
    cursor_setup();
    fake_clock_on = 1; fake_clock_mhz[0] = 333; fake_clock_mhz[1] = 444; fake_clock_mhz[2] = 333;
    for (int change = 0; change < 2; change++) {
        game_generation = 150u + (uint32_t)change;
        next_scene(95u + (unsigned)change); stable_pending = 1; control_tick(0); /* Auto: the activation's first job */
        TEST_ASSERT(job_running);
        as_network_thread(); /* its line and the clock's, held */
        control_tick(0);     /* its result is taken; the 0.5 s are not over */
        TEST_CHECK(!job_running && cache_ok && held_lines() == 2);
        mark = appends_recorded;
        configure_writes = 0;
        if (change) loaded.log_file = 0; /* turned off in the Rubi app meanwhile */
        open_overlay();      /* L + R now */
        TEST_CHECK(ov == OV_OPEN && g_view.open && g_view.list != NULL && !job_running); /* the saved result */
        if (!change) {
            unsigned shown = g_view.version;
            TEST_CHECK(appends_recorded == mark && held_lines() == 2 && configure_writes == 0);
            as_network_thread(); /* the timeout: written on the network thread, the overlay showing */
            TEST_CHECK(net_appends_since(mark, &lines, NULL) == 1 && lines == 2 && appends_recorded == mark + 1);
            TEST_CHECK(appends_seen[mark].view >= shown); /* after the result was shown */
        } else {
            TEST_CHECK(appends_recorded == mark + 1 && appends_seen[mark].thread == CTL_THREAD &&
                       appends_seen[mark].lines == 2 && configure_writes == 1 && held_lines() == 0);
            as_network_thread(); /* the timeout: nothing left to write */
            TEST_CHECK(appends_recorded == mark + 1);
            loaded.log_file = 1;
        }
        close_overlay();
    }
    fake_clock_on = 0;
    mem_free();
}

/* The open's exceptions (the PR's list; audit-t3-log-path-e's audit_open_exceptions): (a) an open that logs a
 * line of its own, a config warning that changed: that line writes the held lines first, on the control
 * thread, before the overlay shows the result; (b) L + R while an activation's first job still runs: nothing
 * until its result is shown, then the network thread's timeout writes its lines. */
static void test_normal_build_open_exceptions(void)
{
    unsigned mark, shown;
    cursor_setup();
    fake_clock_on = 1; fake_clock_mhz[0] = 333; fake_clock_mhz[1] = 444; fake_clock_mhz[2] = 333;
    game_generation = 180; /* (a) */
    next_scene(110); stable_pending = 1; control_tick(0);
    TEST_ASSERT(job_running);
    as_network_thread(); control_tick(0);
    TEST_CHECK(!job_running && cache_ok && held_lines() == 2);
    snprintf(loaded.warnings[0], sizeof(loaded.warnings[0]), "a_key = 1: unknown");
    loaded.n_warnings = 1;
    mark = appends_recorded;
    open_overlay();
    shown = g_view.version;
    TEST_CHECK(g_view.open && g_view.list != NULL);
    TEST_CHECK(appends_recorded == mark + 2 && appends_seen[mark].thread == CTL_THREAD && appends_seen[mark].lines == 2 &&
               appends_seen[mark + 1].lines == 1 && appends_seen[mark].view < shown); /* the held 2, then its own */
    as_network_thread(); /* nothing held: the wait is untimed */
    TEST_CHECK(appends_recorded == mark + 2);
    loaded.n_warnings = 0;
    close_overlay();
    game_generation = 181; /* (b) */
    next_scene(111); stable_pending = 1; control_tick(0);
    TEST_ASSERT(job_running);
    mark = appends_recorded;
    open_overlay(); /* while the job runs: the overlay waits for its result */
    TEST_CHECK(g_view.open && job_running && appends_recorded == mark);
    as_network_thread(); /* the job: its 2 lines held */
    TEST_CHECK(held_lines() == 2 && appends_recorded == mark);
    control_tick(0); /* its result is taken and shown */
    shown = g_view.version;
    TEST_CHECK(!job_running && appends_recorded == mark && held_lines() == 2);
    as_network_thread(); /* the timeout */
    TEST_CHECK(appends_recorded == mark + 1 && appends_seen[mark].thread == NET_THREAD && appends_seen[mark].view >= shown);
    fake_clock_on = 0;
    close_overlay();
    mem_free();
}

/* A job's wake while another thread's batch is open (the ledger table's, microseconds at a game start or exit):
 * net_log_wake's begin is refused and leaves buf NULL; the job's lines go out one by one (the documented
 * exception). The next job's wake opens the batch and holds its lines: a refused begin is retried and leaves no
 * stale buf (audit-t3-log-path-e's N1; its audit_refused_begin_then_the_next_job_holds). net_main's wake is
 * taken step by step here, as as_network_thread's own begin would be a second wake. */
static void test_normal_build_refused_begin_then_the_next_job_holds(void)
{
    VjoLogBatch other;
    static char other_buf[512];
    unsigned mark, one_by_one = 0;
    cursor_setup();
    fake_clock_on = 1; fake_clock_mhz[0] = 333; fake_clock_mhz[1] = 444; fake_clock_mhz[2] = 333;
    game_generation = 170;
    next_scene(99); stable_pending = 1; control_tick(0); /* an activation's first job: it logs */
    TEST_ASSERT(job_running && (posted & NET_EV_JOB));
    TEST_ASSERT(vjo_log_batch_begin(&other, other_buf, sizeof(other_buf)) == 1); /* the table's (control thread) */
    mark = appends_recorded;
    posted &= ~(NET_EV_JOB | NET_EV_QUIT); /* net_main's wake: the bits cleared */
    test_thread = NET_THREAD;
    net_log_wake(net_log_test_buf); /* refused: the table's batch is open */
    TEST_CHECK(!net_log.buf && open_batch == &other);
    test_thread = CTL_THREAD;
    vjo_log_batch_end(&other); /* the table's end */
    test_thread = NET_THREAD;
    net_phase = NET_IDLE;
    net_job(); /* with no batch */
    net_job_end();
    test_thread = CTL_THREAD;
    for (unsigned i = mark; i < appends_recorded && i < APPEND_LOG; i++)
        one_by_one += appends_seen[i].thread == NET_THREAD;
    TEST_CHECK(one_by_one == 2 && !net_log.buf && !open_batch); /* its 2 lines, one by one */
    control_tick(0);
    TEST_CHECK(!job_running);
    as_network_thread(); /* nothing posted, nothing held: the wait is untimed */
    TEST_CHECK(!net_log.buf && !open_batch);
    game_generation = 171;
    next_scene(100); stable_pending = 1; control_tick(0); /* the next activation's first job */
    TEST_ASSERT(job_running);
    mark = appends_recorded;
    as_network_thread();
    TEST_CHECK(net_log.buf != NULL && open_batch == &net_log && held_lines() == 2 && appends_recorded == mark);
    control_tick(0);
    TEST_CHECK(!job_running && appends_recorded == mark && held_lines() == 2);
    as_network_thread(); /* the timeout */
    TEST_CHECK(appends_recorded == mark + 1 && appends_seen[mark].thread == NET_THREAD && appends_seen[mark].lines == 2);
    TEST_CHECK(!open_batch && !net_log.buf);
    fake_clock_on = 0;
    mem_free();
}

/* The ledger's table (a game start or exit, the control thread) while the network thread's batch is open
 * with nothing held (after a quiet job its wait is untimed, so the batch stays open): the table's own batch
 * is refused and its 2 lines go out one by one, one append per file more than main's normal build makes
 * (audit-t3-log-path-d's N4; its audit_ledger_table_while_the_batch_is_open). */
static void test_normal_build_ledger_table_beside_an_open_batch(void)
{
    unsigned mark, with_batch, without;
    cursor_setup();
    game_generation = 160;
    next_scene(97); stable_pending = 1; control_tick(0);
    as_network_thread(); control_tick(0); /* the activation's first job: its line held */
    as_network_thread();                  /* the timeout: written, the batch closed */
    TEST_CHECK(!open_batch && !net_log.buf);
    next_scene(98); stable_pending = 1; control_tick(0);
    as_network_thread(); control_tick(0); /* a quiet job: the batch opened at its wake; nothing held */
    TEST_CHECK(open_batch == &net_log && net_log.n == 0 && !job_running);
    mark = appends_recorded;
    vjo_ledger_shell_log("exit");
    with_batch = appends_recorded - mark;
    test_thread = NET_THREAD;
    net_log_idle(); /* closed, as main's normal build has no network thread batch */
    test_thread = CTL_THREAD;
    mark = appends_recorded;
    vjo_ledger_shell_log("exit");
    without = appends_recorded - mark;
    TEST_CHECK(with_batch == 2 && without == 1);
    TEST_MSG("the ledger's table: %u appends beside the open batch, %u without", with_batch, without);
    mem_free();
}
#endif

#ifdef VJO_WITH_VOCR
/* A long session through the real worker (t3-log-path): 3 games x 3 activations x 90 steps of screens
 * settling (some without a line), the overlay opening and closing, cursor moves, the subtitle strip,
 * failures (memory, an unusable model, a lookup's reads), screens that change under a job, jobs
 * cancelled during their exchange, sizes that do not fit (Large configured), the clock line, and the
 * network thread's timed wait. The normal build makes no append in a job's result path, writes its
 * jobs' lines after their results, holds at most a few lines, and writes every line; both builds print
 * their numbers (the debug build's appends in a result path are its accepted cost:
 * docs/release-variants.md). */
static uint32_t session_rng;
static unsigned session_next(unsigned mod)
{
    session_rng ^= session_rng << 13; session_rng ^= session_rng >> 17; session_rng ^= session_rng << 5;
    return session_rng % mod;
}
static unsigned session_jobs, session_lookups, session_jobs_with_net_lines;
/* Both threads until neither has anything to do: the network thread takes what was posted, the control
 * thread a finished job. */
static void session_settle(void)
{
    for (int round = 0; round < 64; round++) {
        if (posted & NET_EV_JOB) {
            int job = (posted & NET_EV_JOB) != 0, lookup = job && job_cursor_only;
            unsigned lines0 = net_lines_logged;
            as_network_thread();
            if (job) {
                session_jobs++;
                session_lookups += (unsigned)lookup;
                session_jobs_with_net_lines += net_lines_logged != lines0;
            }
            continue;
        }
        if (job_running && job_done) {
            control_tick(0);
            continue;
        }
        TEST_CHECK(!job_running || job_done || (posted & NET_EV_JOB)); /* a running job was posted */
        return;
    }
    TEST_CHECK(0); /* still busy after 64 rounds */
}

static void test_session_log_appends(void)
{
    static const char *const texts[] = {"猫犬鳥", "犬猫", "鳥", "猫"};
    static const char *const games[] = {"PCSG00415", "PCSG00202", "PCSA00029"};
    const unsigned large = 1u << (VJO_VOCR_TIER_LARGE - 1u), normal = 1u << (VJO_VOCR_TIER_NORMAL - 1u);
    unsigned net_appends = 0, ctl_appends = 0, scene = 100, opens = 0, moves = 0;
    cursor_setup();
    loaded.vocr_tier = cfg.vocr_tier = VJO_VOCR_TIER_LARGE;
    fake_clock_on = 1; fake_clock_mhz[0] = 333; fake_clock_mhz[1] = 444; fake_clock_mhz[2] = 333;
    session_rng = 20261016u;
    session_jobs = session_lookups = session_jobs_with_net_lines = 0;
    fake_model_misses = 0;
    state.game_pid = 0; /* activate() names the game: no classification */
    for (unsigned gi = 0; gi < 3; gi++) {
        for (unsigned act = 0; act < 3; act++) {
            on_game_start(); /* a start or a resume: a new activation */
            activate(0x40 + gi, games[gi], VJO_GAME);
            session_settle();
            for (unsigned step = 0; step < 90; step++) {
                unsigned r = session_next(100), idle = session_next(2); /* drawn in both builds: the same session */
                /* this step's conditions */
                fail_vocr_user = session_next(100) < 5;
                fake_screen_changes = session_next(100) < 5;
                fake_cancel_on_read = session_next(100) < 4;
                read_fail = session_next(100) < 5;
                fake_missing_model = session_next(100) < 2 ? VJO_VOCR_TIER3_NAME : NULL;
                {
                    unsigned t = session_next(100);
                    fake_tier_checks = t < 70 ? 0u : t < 90 ? large : large | normal;
                }
                if (session_next(100) < 3) { /* PSVshell's clock, or the boost again */
                    int psv = fake_clock_mhz[0] == 333;
                    fake_clock_rc = psv; fake_clock_mhz[0] = fake_clock_mhz[1] = fake_clock_mhz[2] = psv ? 500 : 333;
                    if (!psv) fake_clock_mhz[1] = 444;
                }
                if (r < 30) { /* a screen settles */
                    unsigned k = session_next(100);
                    fake_lines = k < 15 ? 0u : 1u + session_next(3);
                    fake_recognized_text = k < 15 ? "" : texts[session_next(4)];
                    next_scene(++scene);
                    control_tick(VJO_EV_REGION_STABLE);
                } else if (r < 45) { /* L + R */
                    opens += ov == OV_CLOSED;
                    control_tick(VJO_EV_TRIGGER);
                } else if (r < 65) { /* a cursor move in the shown text */
                    size_t len = local_text_valid ? strlen(local_text) : 0;
                    if (g_view.open && g_view.local_cursor_mode && len) {
                        vjo_post_cursor((uint32_t)(3 * session_next((unsigned)(len / 3))), local_text_generation);
                        moves++;
                    }
                    control_tick(0);
                } else if (r < 70) { /* Select + R */
                    control_tick(VJO_EV_SUBTITLE);
                } else {
                    control_tick(0);
                }
                session_settle();
                test_now += 100000u + session_next(2900000u);
                if (idle) as_network_thread(); /* the network thread's timed wait ends (or nothing held) */
                control_tick(0); /* the time's own work: Auto's next job, a refresh */
                session_settle();
            }
        }
    }
    fail_vocr_user = fake_screen_changes = fake_cancel_on_read = read_fail = 0;
    fake_missing_model = NULL;
    fake_tier_checks = 0;
    close_overlay();
    session_settle();
    as_network_thread(); /* idle: the normal build's held lines go out */
    for (unsigned i = 0; i < appends_recorded && i < APPEND_LOG; i++) {
        net_appends += appends_seen[i].thread == NET_THREAD;
        ctl_appends += appends_seen[i].thread == CTL_THREAD;
    }
    TEST_CHECK(appends_recorded <= APPEND_LOG);
    TEST_CHECK(session_jobs > 300 && session_lookups > 50 && opens > 40 && moves > 40);
    TEST_CHECK(lines_appended == lines_logged); /* every line written, none held at the end */
#if !VJO_DEBUG
    TEST_CHECK(result_path_appends == 0);
    /* at most one append for each job's lines (fewer: the jobs between two idle waits share one) */
    TEST_CHECK(net_appends <= session_jobs_with_net_lines);
    TEST_MSG("%u network appends, %u jobs with lines, %u model misses", net_appends, session_jobs_with_net_lines,
             fake_model_misses);
    TEST_CHECK(most_lines_held < VJO_LOG_BATCH_LINES); /* never full: no append forced by the batch */
    TEST_CHECK(recognizing_appends == 0); /* none while the game reads either: they wait for the idle wait */
    /* log_file never changes here: no configure (an open, a game start, the strip) writes held lines; and the
     * network thread's log locks without a line come from its writes (a begin after each, an end at each), not
     * from every job's wake (audit-t3-log-path-e's N2: the harness's idle step as net_main's wait) */
    TEST_CHECK(configure_writes == 0);
    TEST_CHECK(net_log_locks < session_jobs / 2);
    TEST_MSG("%u configure writes, %u network thread log locks for %u jobs", configure_writes, net_log_locks,
             session_jobs);
#else
    TEST_CHECK(result_path_appends > 0); /* the debug build's lines, as they were */
#endif
    TEST_MSG("%u jobs", session_jobs);
    printf("\n  %s build, a session of %u jobs (%u lookups, %u with the network thread's lines), %u opens, %u moves:"
           " %u lines in %u appends (network thread %u, %u of them while the game recognized; control thread %u),"
           " %u in a job's result path; at most %u lines in one append, %u held at once; %u model misses\n",
           VJO_VARIANT, session_jobs, session_lookups, session_jobs_with_net_lines, opens, moves, lines_logged,
           appends_recorded, net_appends, recognizing_appends, ctl_appends, result_path_appends, most_lines_appended,
           most_lines_held, fake_model_misses);
    fake_clock_on = fake_clock_rc = 0;
    close_overlay();
    set_subtitles(0);
    mem_free();
}

/* Every NET_EV_JOB post finds job_running already set (start_job_kind, start_cursor_job and the self-test's
 * set it, then a barrier, then the post), over the whole session: the normal build's net_main then writes
 * nothing at a job's wake, whenever the network thread runs (audit-t3-log-path-d's
 * audit_every_job_post_finds_job_running). */
static unsigned posts_seen, posts_idle;
static void check_job_post(unsigned bits)
{
    if (!(bits & NET_EV_JOB)) return;
    posts_seen++;
    posts_idle += !job_running;
}
static void test_every_job_post_finds_job_running(void)
{
    posts_seen = posts_idle = 0;
    post_hook = check_job_post; /* each reset() of the session installs it */
    test_session_log_appends();
    post_hook = NULL;
    on_event_flag = NULL;
    TEST_CHECK(posts_seen > 300 && posts_idle == 0);
    TEST_MSG("%u job posts, %u with no job running", posts_seen, posts_idle);
}
#endif

/* test.2's log.txt (#101): each config load (an overlay open, a game start,
 * subtitles on) logged every warning again, 76 lines from 8 distinct ones.
 * A warning is logged when the loaded set changes; the overlay still lists
 * the warnings each time it opens. */
static void test_config_warnings_are_logged_once_per_change(void)
{
    static const char stale[] = "ocr_backend = vocr2\nmeiki_layout = auto\nocr_settle_ms = 150\n";
    reset();
    vjo_config_defaults(&loaded);
    vjo_config_parse(&loaded, stale, strlen(stale));
    TEST_ASSERT(loaded.n_warnings == 2);
    for (int i = 0; i < 3; i++) {
        apply_config();
        open_overlay(); /* re-reads config.ini */
        TEST_CHECK(g_view.n_warnings == 2 && !strcmp(g_view.warnings[1], "unknown setting 'meiki_layout'"));
        close_overlay();
        activate(state.game_pid, "PCSG00415", VJO_GAME);
        set_subtitles(1);
        set_subtitles(0);
    }
    TEST_CHECK_(config_warning_logs == 2, "%u config warning lines", config_warning_logs);
    TEST_CHECK(warnings_logged("ocr_backend: VOCR2 was removed; using vocr") == 1);
    TEST_CHECK(warnings_logged("unknown setting 'meiki_layout'") == 1);
    /* the file changes: its new set is logged once */
    vjo_config_defaults(&loaded);
    vjo_config_parse(&loaded, "ocr_backend = vocr2\n", 20);
    apply_config();
    apply_config();
    TEST_CHECK(config_warning_logs == 3 && warnings_logged("ocr_backend: VOCR2 was removed; using vocr") == 2);
    /* another warning in its place, the same count: a new set, logged once */
    vjo_config_defaults(&loaded);
    vjo_config_parse(&loaded, "ocr_backend = meiki\n", 20);
    TEST_ASSERT(loaded.n_warnings == 1);
    apply_config();
    apply_config();
    TEST_CHECK(config_warning_logs == 4 && warnings_logged("ocr_backend: Meiki was removed; using vocr") == 1);
    /* repaired (the app's next save drops the engine line): nothing more is logged or shown */
    vjo_config_defaults(&loaded);
    apply_config();
    open_overlay();
    TEST_CHECK(config_warning_logs == 4 && !g_view.n_warnings);
    close_overlay();
    mem_free();
}

static void app_files(int installed_rc, const char *installed, int eboot, int record_rc, const char *record)
{
    app_installed_rc = installed_rc;
    snprintf(app_installed, sizeof(app_installed), "%s", installed);
    app_eboot = eboot;
    app_record_rc = record_rc;
    snprintf(app_record, sizeof(app_record), "%s", record);
}
#define STAMP_21 "vjoapp1 api=21 build=0123456789abcdef"
#define STAMP_20 "vjoapp1 api=20 build=fedcba9876543210"
#define NOENT ((int)0x80010002u)

/* overlay.cpp's render: an open overlay's body takes the notice through vjo_view_notice, which marks
 * it drawn (the view lock is render's own). */
static const char *overlay_draws(void)
{
    return g_view.open ? vjo_view_notice() : NULL;
}
static int notice_is(const char *shown, const char *text) { return shown && !strcmp(shown, text); }
static void next_game(void)
{
    on_game_exit();
    activate(state.game_pid, "PCSG00415", VJO_GAME);
}

/* The installed Rubi app is checked against the plugins' record at each
 * enabled game's start: logged when the answer changes, and a mismatch is
 * said once, in the body of the next overlay that draws its page (core/app_stamp.h). */
static void test_overlay_app_mismatch_is_logged_and_said_once(void)
{
    reset();
    /* #101: the app from before the stamps (its eboot, no vjo_app.txt) beside newer plugins */
    app_files(NOENT, "", 1, 37, STAMP_21);
    activate(state.game_pid, "PCSG00415", VJO_GAME);
    TEST_CHECK(app_logs == 1);
    TEST_CHECK_(strstr(last_app_log, "Rubi app MISMATCH: installed '' (0x80010002, app 1), plugins' '"
                       STAMP_21 "' (0x00000025)") != NULL, "%s", last_app_log);
    TEST_CHECK(!g_view.open && !g_view.strip_text[0]); /* nothing is drawn over the game */
    open_overlay();
    TEST_CHECK(notice_is(overlay_draws(), VJO_APP_NOTICE));
    view_publish(1, NULL, "Recognizing…", 0); /* the same overlay keeps it while it is open */
    TEST_CHECK(notice_is(overlay_draws(), VJO_APP_NOTICE));
    close_overlay();
    open_overlay();
    TEST_CHECK(g_view.open && !overlay_draws()); /* once */
    close_overlay();
    /* the next game start finds the same app: nothing new */
    next_game();
    open_overlay();
    TEST_CHECK(app_logs == 1 && !overlay_draws());
    close_overlay();
    /* the VPK installed meanwhile (VitaShell beside the running plugins): ok, logged once, no notice */
    app_files(37, STAMP_21, 1, 37, STAMP_21);
    next_game();
    TEST_CHECK(app_logs == 2 && strstr(last_app_log, "Rubi app ok: installed '" STAMP_21 "'"));
    open_overlay();
    TEST_CHECK(!overlay_draws());
    close_overlay();
    /* an older app installed again: a mismatch again, said again */
    app_files(37, STAMP_20, 1, 37, STAMP_21);
    next_game();
    TEST_CHECK(app_logs == 3 && strstr(last_app_log, "Rubi app MISMATCH: installed '" STAMP_20 "'"));
    open_overlay();
    TEST_CHECK(notice_is(overlay_draws(), VJO_APP_NOTICE));
    close_overlay();
    /* an overlay that cannot draw its page (no Paf memory for it, as 21 times in test.2's log): the
     * notice waits for the next overlay that does */
    app_files(37, STAMP_21, 1, 37, STAMP_21);
    next_game();
    app_files(NOENT, "", 1, 37, STAMP_21);
    next_game();
    TEST_CHECK(app_logs == 5);
    open_overlay(); /* not drawn */
    close_overlay();
    open_overlay();
    TEST_CHECK(notice_is(overlay_draws(), VJO_APP_NOTICE));
    close_overlay();
    open_overlay();
    TEST_CHECK(!overlay_draws());
    close_overlay();
    /* a mismatch that is gone before an overlay drew it leaves nothing to say */
    app_files(37, STAMP_21, 1, 37, STAMP_21);
    next_game();
    app_files(NOENT, "", 1, 37, STAMP_21);
    next_game();
    open_overlay(); /* not drawn */
    close_overlay();
    app_files(37, STAMP_21, 1, 37, STAMP_21);
    next_game();
    TEST_CHECK(app_logs == 8 && !g_view.notice);
    open_overlay();
    TEST_CHECK(!overlay_draws());
    close_overlay();
    /* an app newer than the plugins (the newer package's installer not run): the other notice */
    app_files(37, "vjoapp1 api=22 build=0123456789abcdef", 1, 37, STAMP_21);
    next_game();
    open_overlay();
    TEST_CHECK(notice_is(overlay_draws(), VJO_APP_NOTICE_NEWER));
    close_overlay();
    /* a disabled game starts no check */
    unsigned before = app_logs;
    app_files(NOENT, "", 1, 37, STAMP_21);
    game_enabled = 0;
    next_game();
    TEST_CHECK(app_logs == before);
    mem_free();
}

/* The check runs after the game is active and after the config load: its file reads never delay
 * the game's input or capture, and its log lines follow the start's own. */
static void test_overlay_app_check_runs_after_the_game_is_active(void)
{
    reset();
    app_files(37, STAMP_21, 1, 37, STAMP_21);
    start_order[0] = 0;
    activate(state.game_pid, "PCSG00415", VJO_GAME);
    TEST_CHECK_(!strcmp(start_order, "CASS"), "order %s", start_order);
    TEST_CHECK(logged("game PCSG00415 started") && app_logs == 1);
    /* a disabled game: no config load before it is refused, and no stamp read */
    start_order[0] = 0;
    game_enabled = 0;
    on_game_exit();
    activate(state.game_pid, "PCSG00415", VJO_GAME);
    TEST_CHECK_(!strchr(start_order, 'S') && !strchr(start_order, 'A'), "order %s", start_order);
    mem_free();
}

/* A PSP game is checked as any game (#139): launched as shell/game.c
 * identifies it (a bubble's own ID, Adrenaline's PSPEMUCFW, the shared
 * NPXS10028 row), the stamps are read after the game is active, and a mismatch
 * is said in its first overlay. */
static void test_overlay_app_check_runs_for_psp_games(void)
{
    static const char *const keys[] = {"ULJS00378", "PSPEMU005", "PSPEMUCFW", "NPXS10028"};
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        reset();
        app_files(NOENT, "", 1, 37, STAMP_21); /* #101: the app from before the stamps */
        memset(&identified, 0, sizeof(identified));
        snprintf(identified.key, sizeof(identified.key), "%s", keys[i]);
        identified.mode = VJO_GAME_STATIC_FB;
        identified.psp = 1;
        identify_rc = 0;
        start_order[0] = 0;
        state.game_pid = 77;
        on_game_start();
        classify_pending();
        TEST_CHECK_(pending_pid == 0 && active_mode == VJO_GAME_STATIC_FB && !strcmp(title_id, keys[i]), "%s",
                    keys[i]);
        TEST_CHECK_(!strcmp(start_order, "CASS"), "%s: order %s", keys[i], start_order);
        TEST_CHECK_(app_logs == 1 && strstr(last_app_log, "Rubi app MISMATCH: installed ''"), "%s: %s", keys[i],
                    last_app_log);
        open_overlay();
        TEST_CHECK_(notice_is(overlay_draws(), VJO_APP_NOTICE), "%s", keys[i]);
        close_overlay();
        on_game_exit();
        identify_rc = -1;
        mem_free();
    }
}

/* Nothing to compare, or nothing installed, is no mismatch: an older package
 * brings no record, docs/guide.md lets the app be deleted, and a file the Shell
 * cannot read says nothing about the app. */
static void test_overlay_app_unknown_is_never_said(void)
{
    static const struct { int irc; const char *installed; int eboot, prc; const char *record; int state; } cases[] = {
        {NOENT, "", 1, NOENT, "", VJO_APP_UNKNOWN},             /* test.2's package: no record */
        {37, STAMP_20, 1, NOENT, "", VJO_APP_UNKNOWN},
        {37, STAMP_21, 1, 0, "", VJO_APP_UNKNOWN},             /* an empty record */
        {NOENT, "", 0, 37, STAMP_21, VJO_APP_UNKNOWN},          /* no app installed */
        {(int)0x80010013u, "", 1, 37, STAMP_21, VJO_APP_UNKNOWN}, /* the stamp not readable here */
        {37, STAMP_21, 1, (int)0x80010013u, "", VJO_APP_UNKNOWN},
        {37, STAMP_21, 1, 37, STAMP_21, VJO_APP_OK},
        {37, STAMP_21, 0, 37, STAMP_21, VJO_APP_OK},           /* the stamp decides, not the stat */
        {0, "", 1, 37, STAMP_21, VJO_APP_MISMATCH},            /* an empty stamp */
        {37, STAMP_20, 1, 37, STAMP_21, VJO_APP_MISMATCH},
        {NOENT, "", 1, 37, STAMP_21, VJO_APP_MISMATCH},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        reset();
        app_files(cases[i].irc, cases[i].installed, cases[i].eboot, cases[i].prc, cases[i].record);
        TEST_CHECK_(vjo_app_state(cases[i].irc, cases[i].installed, cases[i].eboot, cases[i].prc, cases[i].record) ==
                    cases[i].state, "case %u", i);
        activate(state.game_pid, "PCSG00415", VJO_GAME);
        TEST_CHECK_(app_logs == 1 && strstr(last_app_log, cases[i].state == VJO_APP_OK ? "Rubi app ok: " :
                                            cases[i].state == VJO_APP_MISMATCH ? "Rubi app MISMATCH: " :
                                            "Rubi app unknown: "), "case %u: %s", i, last_app_log);
        open_overlay();
        TEST_CHECK_(!overlay_draws() == (cases[i].state != VJO_APP_MISMATCH), "case %u", i);
        close_overlay();
        mem_free();
    }
    TEST_CHECK(vjo_app_stamp_api(STAMP_21) == 21 && vjo_app_stamp_api("vjoapp1 api=7") == 7);
    TEST_CHECK(vjo_app_stamp_api("vjoapp2 api=21 build=0") == -1 && vjo_app_stamp_api("vjoapp1 api= build=") == -1);
    TEST_CHECK(vjo_app_stamp_api("vjoapp1 api=21x build=0") == -1 && vjo_app_stamp_api("") == -1);
}

#ifdef VJO_WITH_VOCR
static void test_auto_prepares_words_without_replacing_the_shown_entry(void)
{
    cursor_setup();
    stable_pending = 1; control_tick(0);
    TEST_ASSERT(job_running && !job_prepare);
    finish_current_job();
    TEST_ASSERT(cache_ok && local_text_valid && cursor_handle_live);
    const VjoEntryList *shown = &cache_data[0].list;
    const char *definition = shown->entries[0].text;
    char saved[256]; snprintf(saved, sizeof(saved), "%s", definition);
    unsigned inferences = vocr_allocs, allocations = heap_allocs + user_allocs, notifications = shell_notifications;
    int jobs = 0;
    for (int i = 0; i < 12; i++) {
        test_now += 100001;
        control_tick(0);
        if (job_running) {
            TEST_ASSERT(job_prepare);
            finish_current_job();
            jobs++;
        }
        TEST_CHECK(!strcmp(saved, definition) && cache_ok && !g_view.open);
    }
    TEST_CHECK(jobs > 1 && jobs <= 8 && vocr_allocs == inferences);
    TEST_CHECK(heap_allocs + user_allocs == allocations && shell_notifications > notifications);
    open_overlay();
    TEST_CHECK(!job_running && g_view.list == shown && g_view.cursor_byte == 0);
    int before = cursor_reads;
    notifications = shell_notifications;
    vjo_post_cursor(3, local_text_generation);
    TEST_CHECK(shell_notifications == notifications + 1);
    control_tick(VJO_EV_SHELL);
    TEST_ASSERT(job_running && job_cursor_only && !job_prepare);
    finish_current_job();
    TEST_CHECK(cursor_reads == before + 1 && g_view.list->n_entries == 1);
    TEST_CHECK(strstr(g_view.list->entries[0].text, "dog") && g_view.cursor_byte == 3);
    TEST_CHECK(vocr_allocs == inferences && heap_allocs + user_allocs == allocations);
    close_overlay();
    TEST_CHECK(!cursor_handle_live && cursor_opens == cursor_closes);
    test_now += 200000; control_tick(0);
    TEST_CHECK(!job_running); /* closing does not immediately reopen the files */
    mem_free();
}

static void test_opening_during_preparation_keeps_the_current_screen(void)
{
    for (int moved = 0; moved < 2; moved++) {
        cursor_setup(); stable_pending = 1; control_tick(0); finish_current_job();
        test_now += 100001; control_tick(0);
        TEST_ASSERT(job_running && job_prepare);
        if (moved) { next_scene(8); fake_recognized_text = "犬"; }
        open_overlay();
        if (!moved) TEST_CHECK(g_view.list && g_view.list->n_entries == 1 && strstr(g_view.list->entries[0].text, "cat"));
        else TEST_CHECK(ov == OV_OPEN_OLD_JOB && view_waiting);
        finish_current_job();
        if (moved) {
            TEST_ASSERT(job_running && !job_prepare && !job_cursor_only);
            finish_current_job();
        }
        TEST_CHECK(ov == OV_OPEN && !view_waiting && g_view.list && g_view.list->n_entries == 1);
        TEST_CHECK(strstr(g_view.list->entries[0].text, moved ? "dog" : "cat"));
        close_overlay(); mem_free();
    }
}

static void test_input_wins_the_worker_slot_after_one_prepared_word(void)
{
    cursor_setup(); open_overlay(); finish_current_job();
    test_now += 100001; control_tick(0);
    TEST_ASSERT(job_running && job_prepare);
    finish_current_job(); /* the already selected first word */
    test_now += 100001; control_tick(0);
    TEST_ASSERT(job_running && job_prepare && job_cursor_byte == 3);
    const char *shown = g_view.list->entries[0].text;
    TEST_CHECK(strstr(shown, "cat"));
    vjo_post_cursor(6, local_text_generation); control_tick(VJO_EV_SHELL);
    TEST_CHECK(job_prepare && cursor_waiting);
    finish_current_job();
    TEST_ASSERT(job_running && job_cursor_only && !job_prepare && job_cursor_byte == 6);
    finish_current_job();
    TEST_CHECK(g_view.cursor_byte == 6 && strstr(g_view.list->entries[0].text, "bird"));
    close_overlay(); mem_free();
    /* Manual recognition and Character mode leave the worker idle. */
    for (int mode = 0; mode < 2; mode++) {
        cursor_setup();
        if (!mode) cfg.ocr_mode = loaded.ocr_mode = VJO_OCR_ON_PRESS;
        else cfg.highlight_mode = loaded.highlight_mode = VJO_HIGHLIGHT_CHARACTER;
        open_overlay(); finish_current_job();
        test_now += 100001; control_tick(0);
        TEST_CHECK(!job_running);
        close_overlay(); mem_free();
    }
}
#endif

#ifdef VJO_WITH_VOCR
/* The reading total (docs/ocr-history.md#total-characters-read; shell/history.c, whose own tests are
 * test_shell_history.c's): each wakeup takes one window of a count asked for and wakes the thread again
 * for the next, after everything else the wakeup does; a game's exit keeps the total. */
static void test_reading_total_on_the_control_thread(void)
{
    reset();
    control_tick(0);
    TEST_CHECK(total_steps == 1 && !shell_notifications); /* nothing asked for: no wakeup */
    total_windows = 3;
    control_tick(0);
    TEST_CHECK(total_steps == 2 && shell_notifications == 1);
    control_tick(VJO_EV_SHELL);
    TEST_CHECK(total_steps == 3 && shell_notifications == 2);
    control_tick(VJO_EV_SHELL); /* the last window */
    TEST_CHECK(total_steps == 4 && shell_notifications == 2 && !total_windows);
    state.game_pid = PS_GAME_PID;
    control_tick(VJO_EV_GAME_START);
    TEST_CHECK(!total_keeps); /* a game's start reads nothing for it */
    state.game_pid = 0;
    control_tick(VJO_EV_GAME_EXIT);
    TEST_CHECK(total_keeps == 1);
    mem_free();
}
#endif

#ifdef VJO_VOCR_TIERS_TEST
/* Two sizes, one model (host/tests/test_shared_tiers.h: Normal and Large
 * VOCR4-W20): a bad file leaves out both of its sizes at once, and the job goes on
 * with the next size that has another model, never the same file again. */
static void test_vocr_a_shared_bad_model_leaves_out_both_its_sizes(void)
{
    TEST_ASSERT(!strcmp(VJO_VOCR_TIER2_NAME, VJO_VOCR_TIER3_NAME) && strcmp(VJO_VOCR_TIER1_NAME, VJO_VOCR_TIER2_NAME));
    TEST_CHECK(vocr_same_file(1) == 1u && vocr_same_file(2) == 6u && vocr_same_file(3) == 6u);
    reset();
    cfg.vocr_tier = VJO_VOCR_TIER_LARGE;
    fake_missing_model = VJO_VOCR_TIER3_NAME; fake_recognized_text = "猫";
    TEST_CHECK(run_vocr_job() == VJO_OK && cache_data[0].sentence && !strcmp(cache_data[0].sentence, "猫"));
    TEST_CHECK(vocr_allocs == 2); /* the failed job, then Small's: never Normal's, the same file */
    TEST_CHECK(!strcmp(fake_game_request.model, VJO_VOCR_TIER1_NAME) && fake_game_request.tiers == 1u);
    TEST_CHECK(logged(VJO_VOCR_TIER3_NAME " is unusable here (io=0x80010002), using " VJO_VOCR_TIER1_NAME " for this game") != NULL);
    TEST_CHECK(vocr_unusable == 6u && !cache_data[0].err.detail);
    mem_free();
    /* this game's next job: Small only */
    TEST_ASSERT(mem_alloc() == 0);
    job_cfg = cfg; vjo_arena_reset(&results[0]);
    TEST_CHECK(run_job_t(&results[0], &cache_data[0]) == VJO_OK && fake_game_request.tiers == 1u &&
               !strcmp(fake_game_request.model, VJO_VOCR_TIER1_NAME));
    mem_free();
    /* Normal chosen, the same file bad: Small at once, and Large left out too */
    reset();
    cfg.vocr_tier = VJO_VOCR_TIER_NORMAL;
    fake_missing_model = VJO_VOCR_TIER2_NAME; fake_recognized_text = "猫";
    TEST_CHECK(run_vocr_job() == VJO_OK && vocr_allocs == 2 && fake_game_request.tiers == 1u && vocr_unusable == 6u);
    mem_free();
}


TEST_LIST = {
    {"vocr_a_shared_bad_model_leaves_out_both_its_sizes", test_vocr_a_shared_bad_model_leaves_out_both_its_sizes},
    {NULL, NULL}
};
#else
TEST_LIST = {
    {"config_warnings_are_logged_once_per_change", test_config_warnings_are_logged_once_per_change},
    {"overlay_app_mismatch_is_logged_and_said_once", test_overlay_app_mismatch_is_logged_and_said_once},
    {"overlay_app_unknown_is_never_said", test_overlay_app_unknown_is_never_said},
    {"overlay_app_check_runs_after_the_game_is_active", test_overlay_app_check_runs_after_the_game_is_active},
    {"overlay_app_check_runs_for_psp_games", test_overlay_app_check_runs_for_psp_games},
#if !VJO_DEBUG
    {"normal_build_logs_the_first_jobs_and_failures", test_normal_build_logs_the_first_jobs_and_failures},
    {"normal_build_logs_tier_misses_once_and_on_change", test_normal_build_logs_tier_misses_once_and_on_change},
    {"normal_build_logs_the_clock_once_and_on_change", test_normal_build_logs_the_clock_once_and_on_change},
    {"normal_build_logs_a_failed_lookup_only", test_normal_build_logs_a_failed_lookup_only},
    {"normal_build_has_no_selftest", test_normal_build_has_no_selftest},
    {"normal_build_cancelled_job_is_quiet", test_normal_build_cancelled_job_is_quiet},
    {"normal_build_logs_refused_input_blocks", test_normal_build_logs_refused_input_blocks},
    {"normal_build_writes_a_jobs_lines_after_its_result", test_normal_build_writes_a_jobs_lines_after_its_result},
    {"normal_build_net_main_writes_held_lines", test_normal_build_net_main_writes_held_lines},
    {"normal_build_net_main_more_jobs", test_normal_build_net_main_more_jobs},
    {"normal_build_open_in_the_pause_writes_nothing", test_normal_build_open_in_the_pause_writes_nothing},
    {"normal_build_ledger_table_beside_an_open_batch", test_normal_build_ledger_table_beside_an_open_batch},
    {"normal_build_open_exceptions", test_normal_build_open_exceptions},
    {"normal_build_refused_begin_then_the_next_job_holds", test_normal_build_refused_begin_then_the_next_job_holds},
#endif
#ifdef VJO_WITH_VOCR
    {"auto_prepares_words_without_replacing_the_shown_entry", test_auto_prepares_words_without_replacing_the_shown_entry},
    {"opening_during_preparation_keeps_the_current_screen", test_opening_during_preparation_keeps_the_current_screen},
    {"input_wins_the_worker_slot_after_one_prepared_word", test_input_wins_the_worker_slot_after_one_prepared_word},
    {"session_log_appends", test_session_log_appends},
    {"every_job_post_finds_job_running", test_every_job_post_finds_job_running},
#endif
#if VJO_DEBUG
    {"selftest_keeps_the_user_region_and_auto_ocr", test_selftest_keeps_the_user_region_and_auto_ocr},
#endif
#ifdef VJO_WITH_VOCR
#if VJO_DEBUG
    {"selftest_stages_run_in_one_job_each_recorded_then_finish", test_selftest_stages_run_in_one_job_and_finish},
#endif
#if VJO_DEBUG
    {"selftest_waits_for_text_in_the_region_a_bounded_number_of_screens", test_selftest_waits_for_text_in_the_region},
#endif
    {"screen_changed_is_retried_quietly_at_a_bounded_pace", test_screen_changed_is_retried_quietly_at_a_bounded_pace},
    {"open_overlay_says_why_it_waits", test_open_overlay_says_why_it_waits},
    {"open_overlay_keeps_its_text_and_refreshes_it_quietly", test_open_overlay_refreshes_its_text_quietly},
    {"api49_trusted_hook_line_is_read_at_once", test_trusted_hook_line_is_read_at_once},
    {"history_up_walks_back_and_down_returns", test_history_up_walks_back_and_down_returns},
    {"history_needs_auto_lines_and_memory", test_history_needs_auto_lines_and_memory},
    {"history_waits_for_the_running_job", test_history_waits_for_the_running_job},
    {"history_close_exit_and_region", test_history_close_exit_and_region},
    {"history_a_step_never_holds_a_move_or_a_close", test_history_a_step_never_holds_a_move_or_a_close},
    {"history_the_strip_never_shows_an_earlier_line", test_history_the_strip_never_shows_an_earlier_line},
    {"history_a_step_waiting_for_a_cancelled_ocr_job", test_history_a_step_waiting_for_a_cancelled_ocr_job},
    {"refresh_replacement_resets_dictionary_pages", test_refresh_replacement_resets_dictionary_pages},
    {"on_press_overlay_never_refreshes", test_on_press_overlay_never_refreshes},
    {"refresh_updates_the_subtitle_strip", test_refresh_updates_the_subtitle_strip},
    {"refresh_names_its_model_on_the_cards_only_when_it_replaces", test_refresh_names_its_model_on_the_cards_only_when_it_replaces},
    {"refresh_logs_quietly_and_a_failing_one_backs_off", test_refresh_logs_quietly_and_a_failing_one_backs_off},
    {"reopen_during_a_cancelled_lookup_gets_its_own_recognition (#159 1)",
     test_reopen_during_a_cancelled_lookup_gets_its_own_recognition},
    {"a_move_before_the_end_of_the_job_an_opening_waits_for", test_a_move_before_the_end_of_the_job_an_opening_waits_for},
    {"subtitles_on_during_a_closed_overlays_lookup_or_refresh (#95 3a)",
     test_subtitles_on_during_a_closed_overlays_lookup_or_refresh},
    {"a_refused_job_after_a_cancelled_lookup_and_a_move_shows_its_error (#95 3b)",
     test_a_refused_job_after_a_cancelled_lookup_and_a_move_shows_its_error},
    {"failing_refreshes_do_not_delay_auto_after_close (#95 1)", test_failing_refreshes_do_not_delay_auto_after_close},
    {"a_refresh_takes_its_captures_seq (#95 2b)", test_a_refresh_takes_its_captures_seq},
    {"reopen_after_a_same_text_refresh_shows_the_saved_word", test_reopen_after_a_same_text_refresh_shows_the_saved_word},
    {"subtitles_on_during_an_ocr_job_leave_no_wait_behind", test_subtitles_on_during_an_ocr_job_leave_no_wait_behind},
    {"the_previous_games_text_is_never_the_strips", test_the_previous_games_text_is_never_the_strips},
    {"the_refreshes_backoff_outlasts_a_close_with_no_job", test_the_refreshes_backoff_outlasts_a_close_with_no_job},
    {"an_opening_during_autos_job_on_a_changed_screen_gets_its_own",
     test_an_opening_during_autos_job_on_a_changed_screen_gets_its_own},
    {"a_region_drawn_during_a_lookup_or_refresh_gives_the_strip_its_sentence (#182 a)",
     test_a_region_drawn_during_a_lookup_or_refresh_gives_the_strip_its_sentence},
    {"a_region_drawn_during_an_ocr_job_drops_its_old_regions_result (#182 a)",
     test_a_region_drawn_during_an_ocr_job_drops_its_old_regions_result},
    {"a_retired_texts_cached_sentence_is_never_the_strips (#182 b)", test_a_retired_texts_cached_sentence_is_never_the_strips},
    {"a_region_drawn_between_a_jobs_answer_and_its_publish_never_brings_its_text_back",
     test_a_region_drawn_between_a_jobs_answer_and_its_publish_never_brings_its_text_back},
    {"select_r_during_a_cancelled_job_waits_for_its_end", test_select_r_during_a_cancelled_job_waits_for_its_end},
    {"a_region_drawn_with_the_strip_on_and_no_job_starts_the_strips_own",
     test_a_region_drawn_with_the_strip_on_and_no_job_starts_the_strips_own},
    {"a_reopen_during_the_strips_wait_leaves_nothing_waiting", test_a_reopen_during_the_strips_wait_leaves_nothing_waiting},
#if VJO_DEBUG
    {"a_region_drawn_during_the_selftest_job_leaves_the_strip_no_wait",
     test_a_region_drawn_during_the_selftest_job_leaves_the_strip_no_wait},
#endif
#if VJO_DEBUG
    {"select_r_during_a_stopped_selftest_job_leaves_no_wait", test_select_r_during_a_stopped_selftest_job_leaves_no_wait},
#endif
#if VJO_DEBUG
    {"refresh_while_the_selftest_is_armed", test_refresh_while_the_selftest_is_armed},
#endif
#if VJO_DEBUG
    {"selftest_job_keeps_the_texts_capture", test_selftest_job_keeps_the_texts_capture},
#endif
#if VJO_DEBUG
    {"subtitles_on_during_the_selftest_job", test_subtitles_on_during_the_selftest_job},
#endif
#if VJO_DEBUG
    {"selftest_keeps_the_shown_texts_screen", test_selftest_keeps_the_shown_texts_screen},
#endif
#if VJO_DEBUG
    {"selftest_end_keeps_the_open_overlays_text", test_selftest_end_keeps_the_open_overlays_text},
#endif
#if VJO_DEBUG
    {"selftest_end_shows_the_open_overlays_text_in_the_strip", test_selftest_end_shows_the_open_overlays_text_in_the_strip},
#endif
#if VJO_DEBUG
    {"the_done_line_has_the_exchange_and_log_times", test_the_done_line_has_the_exchange_and_log_times},
    {"debug_build_writes_a_lookups_lines_after_its_result", test_debug_build_writes_a_lookups_lines_after_its_result},
    {"debug_build_writes_an_ocr_jobs_lines_after_its_result",
     test_debug_build_writes_an_ocr_jobs_lines_after_its_result},
    {"debug_build_a_full_batch_goes_on_holding", test_debug_build_a_full_batch_goes_on_holding},
    {"debug_build_lookup_shown_figure", test_debug_build_lookup_shown_figure},
#endif
#if VJO_DEBUG
    {"selftest_runs_after_the_first_text_in_its_own_job", test_selftest_runs_after_the_first_text_in_its_own_job},
#endif
#endif
    {"ledger_counts_result_memory_against_the_budget", test_ledger_counts_result_memory_against_the_budget},
    {"boot_guard_ok_after_a_stable_minute", test_boot_guard_ok_after_a_stable_minute},
    {"vjonet_stack_high_water_is_logged_once_per_growth", test_net_stack_high_water_is_logged_once_per_growth},
    {"vjonet_stack_paint_on_a_real_thread_stays_in_its_stack_and_misreports_paint_nothing",
     test_net_stack_paint_on_a_real_thread_stays_in_its_stack},
#ifdef VJO_WITH_VOCR
    {"game_exit_during_a_lookup_frees_its_arena_after_the_job", test_audit_exit_during_lookup_frees_the_arena_after_the_job},
    {"game_exit_during_a_cursor_job_frees_its_arena_after_the_job", test_audit_exit_during_cursor_job_frees_the_arena_after_the_job},
    {"a_refresh_has_no_arena_and_leaves_the_shown_one", test_audit_refresh_has_no_arena},
    {"a_cursor_posted_while_publishing_runs_after_cmd_lock_is_released", test_audit_cursor_posted_while_publishing},
#endif
    {"subtitles_are_ocr_only", test_subtitles_are_ocr_only},
    {"removed_relay_opens_as_local", test_removed_relay_opens_as_local},
    {"input_block_refusal_keeps_overlay_closed", test_input_block_denied},
    {"disabled_game_retires_previous_job", test_disabled_game_retires_work},
    {"psp_launch_uses_its_identity", test_psp_launch_uses_its_identity},
    {"failed_join_keeps_allocations_for_retry", test_failed_join_keeps_allocations},
    {"anki_mining_enables_cross_and_keeps_the_pipeline", test_anki_mining_enables_cross_in_view},
    {"anki_stop_failure_keeps_worker_memory_for_retry", test_anki_stop_failure_keeps_worker},
#ifdef VJO_WITH_VOCR
    {"japanese_only_selects_first_letter_rejects_other_scripts_and_reloads_choices", test_japanese_only_selection_and_config_reload},
    {"no_Japanese_text_allocates_no_results_and_performs_no_dictionary_IO", test_no_japanese_selection_skips_dictionary_memory_and_io},
    {"character_open_reuses_background_OCR_without_dictionary_or_capture_work", test_character_open_reuses_background_text},
    {"word_Auto_prepares_first_match_and_reopen_reuses_only_current_cache", test_word_background_lookup_and_reopen_cache},
    {"history_snapshots_auto_ocr_after_publication", test_history_snapshots_auto_ocr_after_publication},
    {"history_ignores_storage_failure_and_dictionary_oom", test_history_ignores_storage_failure_and_dictionary_oom},
    {"history_records_a_hooks_line_with_its_source", test_history_records_a_hooks_line_with_its_source},
    {"history_records_refreshes_but_not_cursor_jobs", test_history_records_refreshes_but_not_cursor_jobs},
    {"history_excludes_disabled_failed_cancelled_and_invalid_clock", test_history_excludes_disabled_failed_cancelled_and_invalid_clock},
    {"word_lookup_with_the_bundled_dictionary_first_word_right_and_tap", test_word_lookup_with_the_bundled_dictionary},
    {"stick_steps_characters_with_the_bundled_dictionary", test_stick_steps_characters_with_the_bundled_dictionary},
#if VJO_DEBUG
    {"lookup_log_line_fits_with_worst_case_fields", test_lookup_log_line_fits_with_worst_case_fields},
#endif
    {"character_moves_coalesce_and_reject_old_inflight_definition", test_character_coalesces_and_drops_inflight_definition},
    {"character_close_reopen_restarts_lookup_and_retirement_rejects_old_results", test_character_close_reopen_and_retirement},
    {"character_scene_change_keeps_the_shown_text_and_region_change_retires_it", test_character_scene_change_keeps_the_shown_text},
    {"the_suggested_box_reaches_the_view_once_an_activation", test_the_suggested_box_reaches_the_view_once_an_activation},
    {"character_commands_preserve_close_region_priority_and_nonzero_generations", test_character_command_priority_and_generation_boundaries},
    {"dictionary_pages_read_one_entry_each_in_the_single_arena", test_dictionary_pages_read_one_entry_each},
    {"dictionary_count_change_looks_up_again_without_OCR", test_dictionary_count_change_relooks_up_without_ocr},
    {"dictionary_page_after_a_replaced_file_locates_again", test_dictionary_page_after_replaced_file_locates_again},
    {"dictionary_pages_OOM_keeps_text_and_disables_paging", test_dictionary_pages_oom_keeps_text_and_disables_paging},
    {"Shell_result_OOM_preserves_OCR_sentence_and_dictionary_backoff", test_shell_result_oom_preserves_recognition},
    {"Shell_result_OOM_preserves_maximum_filtered_worker_text", test_shell_result_oom_keeps_max_worker_text},
    {"local_lookup_uses_one_arena", test_local_lookup_single_arena},
    {"local_ocr_and_dictionary_phases_repeat_and_recover", test_local_phases_and_recovery},
#endif
    {"vocr_empty_region_reads_no_line_and_frees_the_workspace", test_vocr_empty_region},
    {"a game without a worker says to start it again (#138)", test_no_worker_says_restart_the_game},
#ifdef VJO_WITH_VOCR
    {"vocr_capture_follows_the_retry", test_vocr_capture_follows_the_retry},
    {"vocr_ran_names_outlive_the_job", test_vocr_ran_names_outlive_the_job},
    {"native hooks: automatic source and diagnostic logs (API 31)", test_native_hooks_automatic_text_source_and_diagnostics},
    {"vocr_text_is_logged_raw_and_shown_normalised", test_vocr_text_is_logged_raw_and_shown_normalised},
#endif
    {"removed_engine_choice_VOCR2_or_ncnn_runs_the_local_model_with_its_fallback", test_removed_engine_choice_runs_the_local_engine},
    {"engine_cache_tracks_backend_local_models_and_storage", test_engine_cache_tracks_backend_models_and_storage},
    {"vocr_config_parses_and_validates", test_vocr_config},
    {"vocr_model_change_invalidates_cache", test_vocr_model_change_invalidates_cache},
#ifdef VJO_WITH_VOCR
    {"vocr_missing_model_releases_workspace", test_vocr_missing_model_releases_workspace},
    {"vocr_storage_errors_have_specific_messages", test_vocr_storage_errors_are_specific},
    {"vocr_vertical_request_sets_the_bit_names_the_model_and_leaves_no_size_out", test_vocr_vertical_request},
#if VJO_DEBUG
    {"vocr_logs_memory_check_frames_and_lines", test_vocr_logs_memory_check_and_frames},
#endif
#if VJO_DEBUG
    {"vocr_job_log_is_appended_in_batches", test_vocr_job_log_is_appended_in_batches},
#endif
    {"vocr_request_carries_the_job_policy", test_vocr_request_carries_the_job_policy},
    {"vocr_oom_advice_names_the_stage", test_vocr_oom_advice_names_the_stage},
    {"vocr_oom_statistics_are_cached", test_vocr_oom_statistics_are_cached},
    {"vocr_unusable_model_uses_the_fallback", test_vocr_unusable_model_uses_the_fallback},
    {"vocr_memory_fallback_is_logged", test_vocr_memory_fallback_is_logged},
    {"vocr_transient_model_failure_is_not_sticky", test_vocr_transient_model_failure_is_not_sticky},
    {"vocr_model_error_retry_limits", test_vocr_model_error_retry_limits},
    {"vocr_refused_fallback_is_labelled", test_vocr_refused_fallback_is_labelled},
    {"game_vocr_OOM_never_falls_back_to_Shell", test_vocr_allocation_fallback_and_failure},
#endif
    {"stick_lookup_refused_by_a_pending_close", test_stick_lookup_refused_by_a_pending_close},
    {"stick_close_before_and_after_the_rest", test_stick_close_before_and_after_the_rest},
    {"stick_lookup_ms_reaches_the_view", test_stick_lookup_ms_reaches_the_view},
    {"local_dictionary_opens_without_key", test_local_open},
    {"ordered_dictionary_config_invalidates_cache", test_config_cache},
    {"scene_cache_is_immediate", test_cached_scene},
    {"outdated_scene_cancels_once", test_stale_cancel},
    {"local_dictionary_background_prefetch", test_background_local},
    {"allocation_failure_recovers_and_releases", test_alloc_failure},
    {"text_source and a removed engine (API 31)", test_ocr_config_migration},
    {"hook_sites = off: the text source's word with NO_SITES (audit-hooks F3)", test_hook_sites_word},
    {"changed_dictionary_drops_inflight_cached_result", test_changed_dictionary_drops_running_result},
    {"game_exit_drains_previous_job_without_reviving_cache", test_game_exit_drains_previous_job},
    {"game_start_rejects_previous_generation_and_restarts", test_game_start_rejects_previous_generation},
    {"ps_with_the_overlay_open_releases_input_and_leaves_touch_to_shell",
     test_ps_with_the_overlay_open_releases_input_and_leaves_touch_to_shell},
    {"foreground_switch_gives_the_touch_region_back", test_foreground_switch_gives_the_touch_region_back},
    {"close_just_before_ps_leaves_touch_to_shell", test_close_just_before_ps_leaves_touch_to_shell},
#ifdef VJO_WITH_VOCR
    {"reading_total_a_window_a_wakeup_kept_at_a_games_exit",
     test_reading_total_on_the_control_thread},
#endif
    {NULL, NULL}
};
#endif /* VJO_VOCR_TIERS_TEST */
