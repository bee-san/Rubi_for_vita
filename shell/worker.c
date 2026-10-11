/* Control thread (kernel events, overlay state, auto/on-press policy) and
 * network thread (capture -> JPEG -> Lens, or capture -> local vita-vn-ocr;
 * then the dictionary). */
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

#include "../core/regions.h"
#include "../core/textfilter.h"
#include "../core/utf.h"
#ifdef VJO_WITH_VOCR
#include "../core/local_ocr.h"
#include "game_ocr_client.h"
#include "../include/vjo_vocr_sizes.h"
#endif
#include "shell.h"

/* Network memory: two result arenas (the overlay shows one while the worker
 * fills the other) and kept connections. Local lookup retires its prior view
 * before OCR, so it needs only one arena. OCR-only local jobs use bounded
 * static text. Control-thread config/region/messages use separate scratch. */
#define RESULT_ARENA_SIZE (256 * 1024)
#define LOCAL_RESULT_ARENA_SIZE (64 * 1024)
#define SCRATCH_SIZE      (24 * 1024)
typedef char config_fits_scratch[(SCRATCH_SIZE >= VJO_CONFIG_TEXT_MAX + 1) ? 1 : -1];
#define POOL_SLOTS        2 /* Lens and the dictionary */
#define POOL_SIZE         ((vjo_net_pool_size(POOL_SLOTS) + 15) & ~(size_t)15)
#define MEM_SIZE          (2 * RESULT_ARENA_SIZE + POOL_SIZE)
/* Well within the servers' (jiten closes an idle connection after 3
 * minutes, Lens after 4), and short enough that one a network dropped
 * silently (a hotspot's NAT) is rarely met: that costs a read timeout. */
#define NET_IDLE_US       (60 * 1000 * 1000)
/* Connections are readied when the region starts changing (for this long),
 * this often at most: a region that never settles readies them once. */
#define WARM_WINDOW_MS    3000u
#define WARM_GAP_US       2000000LL
#define WARM_CONNECT_US   5000000

#define POLL_TIMEOUT_US   50000
#define BACKOFF_MIN_US    2000000LL
#define BACKOFF_MAX_US    60000000LL
#define JPEG_QUALITY      80
#define SUBTITLE_GAP_US   1000000LL /* between subtitle jobs */
#define UNSETTLED_US      2000000LL /* subtitles: a region that keeps changing is recognized this often */

VjoView g_view;

static SceUID view_lock = -1;
static SceUID cmd_lock = -1;
static SceUID ctl_thread = -1, net_thread = -1;
static int threads_started; /* bits: network=1, control=2 */
static SceUID net_evf = -1;
static volatile int running = 1;

static SceUID mem_uid = -1;
static void *mem_heap; /* bounded allocation from ScePaf's existing heap */
static int mem_local;
static VjoArena results[2];
static VjoArena scratch; /* static; control thread only */
static uint8_t scratch_mem[SCRATCH_SIZE];
static int active = -1;               /* results[] index shown/cached */
static uint32_t cache_scene; /* VjoState.scene of the cached result, 0 = unknown */
static uint32_t cache_seq;   /* VjoState.region_seq of its capture */
static int cache_ok;
static int cache_dictionary; /* the cached lookup's VJO_DICT_* */
static VjoOverlayData cache_data[2];
/* Local text outlives the single dictionary arena. Only an OCR job rewrites it,
 * after start_job has detached its prior view. Cursor jobs only read it. */
static char local_text[VJO_STRIP_MAX];
static VjoEntryList local_header;
static volatile int local_text_valid;
static uint32_t local_text_generation, local_text_game_generation;
static uint32_t local_text_scene, local_text_capture_seq;
static uint32_t local_cursor_byte, local_cursor_seq;
static int cursor_waiting;
static int job_cursor_only;
static uint32_t job_cursor_byte, job_cursor_seq, job_text_generation;

static VjoConfig cfg;      /* control thread */
static VjoConfig job_cfg;  /* snapshot used by the network thread */
static VjoPlatform plat;
static VjoNetPool pool; /* plat's; network thread only, except mem_free while it idles */
static char title_id[12];   /* set while a game is active ("" otherwise) */
/* Scene IDs survive kernel resets. A foreground boundary must also reject
 * results captured for the preceding game, even before a new scene arrives. */
static uint32_t game_generation;
static uint32_t job_generation;

/* Control-thread state. The overlay and the network job are independent: an
 * auto job runs while the overlay is closed, the overlay can open while a
 * job runs, and a result can be shown while the next job runs.
 *   overlay: OV_CLOSED, OV_OPEN, or OV_OPEN_OLD_JOB (opened while a job was
 *            already running: its result may be for an earlier screen, which
 *            is checked when it ends)
 *   job:     job_running (set here, finished via job_done from the network
 *            thread)
 *   subtitles: on/off (the strip shows each job's sentence while the
 *            overlay is closed)
 * plus inputs to the background policy: stable_pending (a REGION_STABLE
 * or REGION_QUIET not acted on yet) and the error backoffs, one per stage (a
 * failing dictionary does not stop the subtitles, which need only OCR).
 * Each event has one handler below (on_*). */
enum { OV_CLOSED, OV_OPEN, OV_OPEN_OLD_JOB };
static int ov = OV_CLOSED;
static int subtitles; /* written by set_subtitles only */
static int input_block_error;
static int stable_pending;
/* The overlay waits for the running job ("Recognizing…"), or shows its
 * sentence before the lookup: job_text_list, in the job's arena, which
 * must not be shown once another job starts in it. */
static int view_waiting, view_job_text;
static VjoEntryList job_text_list;
typedef struct {
    int64_t until, us;
} Backoff;
static Backoff ocr_backoff, dict_backoff;
static int64_t job_started_us;

/* network job */
#define NET_EV_JOB  1u
#define NET_EV_QUIT 2u
#define NET_EV_WARM 4u
static volatile int job_running;
static volatile int job_done;
static volatile int job_idx;
static volatile int job_rc;
static volatile int job_lookup;     /* the job looks the words up (else OCR only) */
/* The cached result's text when the job started (NULL: no usable lookup).
 * Its arena is not touched while the job runs (the job fills the other). */
static const char *job_cached_text;
/* The job's text is job_cached_text: no lookup, the cached result stays
 * (and is now for the job's screen). */
static volatile int job_same_text;
/* The job's capture is done (job_capture_scene, job_capture_seq: its
 * VjoState.capture_scene and region_seq). */
static volatile int job_captured;
static volatile uint32_t job_capture_scene, job_capture_seq;
/* The job is for an earlier screen: its requests were aborted, its result
 * is dropped. */
static volatile int job_cancelled;
static VjoNetCancel net_cancel = {-1, -1, 0}; /* the network thread's requests */
/* The network thread opens the connections for the next job (warm_dictionary:
 * VJO_DICT_*, or -1 for Lens only). */
static volatile int warm_running;
static volatile int warm_dictionary;
static int64_t warm_asked_us;
/* cache_data[job_idx].sentence is ready (the lookup may still run): the
 * net thread only appends to the arena, so the control thread may read it */
static volatile int job_text_ready;

static volatile int pending_cmd;
static VjoRect pending_rect;
static uint32_t pending_cursor_byte, pending_cursor_generation;
static void start_cursor_job(void);

static uint32_t advance_generation(uint32_t *value)
{
    if (++*value == 0) *value = 1;
    return *value;
}

static void retire_local_text(int new_generation)
{
    sceKernelLockMutex(cmd_lock, 1, NULL);
    vjo_view_lock();
    local_text_valid = 0;
    cursor_waiting = 0;
    if (new_generation) advance_generation(&local_text_generation);
    g_view.list = NULL;
    g_view.local_cursor_mode = 0;
    g_view.version++;
    vjo_view_unlock();
    sceKernelUnlockMutex(cmd_lock, 1);
}

void vjo_view_lock(void)
{
    sceKernelLockMutex(view_lock, 1, NULL);
}

void vjo_view_unlock(void)
{
    sceKernelUnlockMutex(view_lock, 1);
}

void vjo_post_command(int cmd, const VjoRect *rect)
{
    sceKernelLockMutex(cmd_lock, 1, NULL);
    pending_cmd = cmd;
    if (rect)
        pending_rect = *rect;
    sceKernelUnlockMutex(cmd_lock, 1);
}

void vjo_post_cursor(uint32_t byte_offset, uint32_t text_generation)
{
    /* This order is also used by cursor completion: a newer request cannot
     * race past the stale-result check and republish the old definition. */
    sceKernelLockMutex(cmd_lock, 1, NULL);
    if (pending_cmd != VJO_CMD_NONE && pending_cmd != VJO_CMD_CURSOR) {
        sceKernelUnlockMutex(cmd_lock, 1);
        return;
    }
    vjo_view_lock();
    if (!g_view.open || !g_view.local_cursor_mode || !local_text_valid ||
        !text_generation || text_generation != local_text_generation) {
        vjo_view_unlock();
        sceKernelUnlockMutex(cmd_lock, 1);
        return;
    }
    size_t len = sceClibStrnlen(local_text, sizeof(local_text));
    if ((len ? byte_offset >= len : byte_offset != 0) ||
        vjo_utf8_cursor_step(local_text, len, byte_offset, 0) != byte_offset ||
        (cfg.japanese_only_selection &&
         (byte_offset >= len || vjo_utf8_japanese_cursor_step(local_text, len, byte_offset, 0) != byte_offset))) {
        vjo_view_unlock();
        sceKernelUnlockMutex(cmd_lock, 1);
        return;
    }
    local_cursor_byte = byte_offset;
    advance_generation(&local_cursor_seq);
    pending_cursor_byte = byte_offset;
    pending_cursor_generation = text_generation;
    pending_cmd = VJO_CMD_CURSOR;
    local_header = (VjoEntryList){local_text, NULL, 0};
    g_view.list = &local_header; /* detach entries before their arena is reset */
    g_view.cursor_byte = byte_offset;
    g_view.list_seq++;
    sceClibSnprintf(g_view.status, sizeof(g_view.status), "Looking up words…");
    g_view.status_is_error = 0;
    g_view.version++;
    vjo_view_unlock();
    sceKernelUnlockMutex(cmd_lock, 1);
}

static int64_t now_us(void)
{
    return (int64_t)sceKernelGetProcessTimeWide();
}

/* Does a result for scene `a` show the current scene `b`? */
static int same_scene(uint32_t a, uint32_t b)
{
    return a != 0 && a == b;
}

/* Does a result for `scene`, from region capture `seq`, show the screen in
 * st? Its scene is the current one, or its capture still shows the screen
 * (checked now): an icon that kept changing when it was taken got masked
 * since, or is all that changes. */
static int shows_screen(uint32_t scene, uint32_t seq, const VjoState *st)
{
    return same_scene(scene, st->scene) ||
           (seq && seq == st->region_seq && same_scene(st->region_scene, st->scene));
}

/* ---------------- memory ---------------- */

static void mem_free(void);

static int has_result_memory(void)
{
    return mem_heap != NULL || mem_uid >= 0;
}

static int local_pipeline(const VjoConfig *c)
{
    return (c->ocr_backend == VJO_OCR_VOCR || c->ocr_backend == VJO_OCR_MEIKI) &&
           c->dictionary == VJO_DICT_LOCAL;
}

static int mem_alloc_for(const VjoConfig *c)
{
    void *base = NULL;
    uint8_t *p;
    int local = local_pipeline(c);
    size_t arena_size = local ? LOCAL_RESULT_ARENA_SIZE : RESULT_ARENA_SIZE;
    size_t bytes = local ? arena_size : MEM_SIZE;
    if (has_result_memory() && mem_local != local)
        mem_free();
    if (has_result_memory())
        return 0;
#ifdef VJO_PAF_ALLOC
    /* Games can exhaust USER while SceShell still has reserved heap pages.
     * Result arenas and kept TLS sessions have one bounded owner. */
    mem_heap = vjo_shell_heap_alloc(bytes);
    if (!mem_heap) {
        vjo_log("result workspace Paf allocation failed (%u KiB)", (unsigned)(bytes >> 10));
        return -1;
    }
    base = mem_heap;
#else
    mem_uid = sceKernelAllocMemBlock("VjoShellMem", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                     (bytes + 0xFFF) & ~0xFFF, NULL);
    if (mem_uid < 0)
        return mem_uid;
    if (sceKernelGetMemBlockBase(mem_uid, &base) < 0 || !base) {
        sceKernelFreeMemBlock(mem_uid);
        mem_uid = -1;
        return -1;
    }
#endif
    p = (uint8_t *)base;
    vjo_arena_init(&results[0], p, arena_size);
    vjo_arena_init(&results[1], local ? NULL : p + arena_size, local ? 0 : arena_size);
    vjo_net_pool_init(&pool, local ? NULL : p + 2 * arena_size,
                      local ? 0 : POOL_SIZE, NET_IDLE_US);
    mem_local = local;
    vjo_log("%s workspace: %u bytes, %u %u-byte results, %u-byte connection pool",
            local ? "local dictionary" : "network", (unsigned)bytes,
            (unsigned)(local ? 1 : 2), (unsigned)arena_size, (unsigned)(local ? 0 : POOL_SIZE));
    active = -1;
    cache_ok = 0;
    return 0;
}

static int mem_alloc(void) { return mem_alloc_for(&cfg); }

/* Called by the idle control thread or by the network thread owning its job. */
static void mem_free(void)
{
    if (has_result_memory()) {
        vjo_view_lock();
        g_view.list = NULL;
        vjo_view_unlock();
        vjo_net_pool_close(&plat);
        vjo_net_pool_init(&pool, NULL, 0, NET_IDLE_US);
        if (mem_heap)
            vjo_shell_heap_free(mem_heap);
        else
            sceKernelFreeMemBlock(mem_uid);
    }
    mem_heap = NULL;
    mem_uid = -1;
    vjo_arena_init(&results[0], NULL, 0);
    vjo_arena_init(&results[1], NULL, 0);
    active = -1;
    cache_ok = 0;
}

/* ---------------- view ---------------- */

static void view_publish(int open, const VjoEntryList *list, const char *status, int is_error)
{
    vjo_view_lock();
    __atomic_store_n(&g_view.open, open, __ATOMIC_RELEASE); /* also read unlocked (overlay frame) */
    g_view.list = list;
    g_view.list_seq++;
    sceClibSnprintf(g_view.status, sizeof(g_view.status), "%s", status ? status : "");
    g_view.status_is_error = is_error;
    g_view.local_cursor_mode = local_pipeline(&cfg) && local_text_valid && list &&
                               list->header == local_text;
    g_view.cursor_byte = local_cursor_byte;
    g_view.japanese_only_selection = cfg.japanese_only_selection;
    g_view.highlight_mode = cfg.highlight_mode;
    g_view.text_generation = local_text_generation;
    g_view.font_size_ja = cfg.font_size_ja;
    g_view.font_size_en = cfg.font_size_en;
    g_view.n_warnings = cfg.n_warnings;
    sceClibMemcpy(g_view.warnings, cfg.warnings, sizeof(g_view.warnings));
    g_view.version++;
    vjo_view_unlock();
}

static void view_show_cache(void)
{
    view_waiting = view_job_text = 0;
    VjoOverlayData *d = &cache_data[active];
    size_t mark = vjo_arena_mark(&scratch);
    const char *err = d->err.rc ? vjo_err_text(&scratch, d->failed_stage, &d->err) : NULL;
    view_publish(1, &d->list, err, err != NULL);
    vjo_arena_release(&scratch, mark);
}

/* Copies UTF-8 text, cut at a character boundary to fit. */
static void copy_utf8(char *dst, size_t cap, const char *src)
{
    size_t n = sceClibStrnlen(src, cap - 1);
    if (n == cap - 1)
        while (n && ((uint8_t)src[n] & 0xC0) == 0x80)
            n--;
    sceClibMemcpy(dst, src, n);
    dst[n] = '\0';
}

/* The strip: text (NULL keeps it) of kind VJO_STRIP_*, and the busy mark. */
static void strip_publish(const char *text, int kind, int busy)
{
    vjo_view_lock();
    __atomic_store_n(&g_view.strip_on, subtitles, __ATOMIC_RELEASE); /* also read unlocked */
    if (text) {
        copy_utf8(g_view.strip_text, sizeof(g_view.strip_text), text);
        g_view.strip_kind = kind;
    }
    g_view.strip_busy = busy;
    g_view.font_size_ja = cfg.font_size_ja;
    g_view.font_size_en = cfg.font_size_en;
    g_view.strip_version++;
    vjo_view_unlock();
}

/* ---------------- network thread ---------------- */

typedef struct {
    const uint8_t *data;
} MemJpeg;

static int mem_jpeg_read(void *ud, uint32_t off, void *dst, uint32_t len)
{
    sceClibMemcpy(dst, ((MemJpeg *)ud)->data + off, len);
    return 0;
}

/* ---- local OCR: vita-vn-ocr (ocr_backend = vocr) ---- */

#ifdef VJO_WITH_VOCR
static int vocr_cancelled(void *ud)
{
    (void)ud;
    __sync_synchronize();
    return !running || job_cancelled;
}
static int ocr_submit(void *ud, const VjoGameOcrRequest *r) { (void)ud; return vjoOcrSubmit(r); }
static int ocr_read(void *ud, uint32_t seq, VjoGameOcrResult *r) { (void)ud; return vjoOcrRead(seq, r); }
static int ocr_cancel(void *ud, uint32_t seq) { (void)ud; return vjoOcrCancel(seq); }
static int64_t ocr_clock(void *ud) { (void)ud; return now_us(); }
static void ocr_delay(void *ud, uint32_t us) { (void)ud; sceKernelDelayThread(us); }

static int vocr_consume(void *ud, const VjoState *st)
{
    VjoGameOcrResult *result = ud;
    VjoGameOcrRequest request = {.size=sizeof(request), .done_seq=st->done_seq};
    VjoGameOcrClient client = {NULL, ocr_submit, ocr_read, ocr_cancel, ocr_clock, ocr_delay};
    sceClibSnprintf(request.model_dir, sizeof(request.model_dir), "%s", job_cfg.ocr_model_dir);
    const char *model = job_cfg.ocr_backend == VJO_OCR_MEIKI ? VJO_MEIKI_MODEL_NAME : job_cfg.vocr_model;
    size_t model_len = sceClibStrnlen(model, sizeof(job_cfg.vocr_model));
    if (model_len >= sizeof(request.model)) return VJO_E_OCR_MODEL;
    sceClibMemcpy(request.model, model, model_len+1);
    job_capture_scene = st->capture_scene;
    job_capture_seq = st->region_seq;
    __sync_synchronize();
    job_captured = 1;
    int rc = vjo_game_ocr_exchange(&client, &request, result, vocr_cancelled, NULL, 60000000);
    vjo_log("game vocr rc=%d %s: %ux%u, lines=%u USER workspace=%u, find=%u ms load=%u ms recognize=%u ms io=0x%08X cleanup=%d",
            rc, model, st->width, st->height, result->lines, result->workspace_bytes,
            result->find_us/1000, result->load_us/1000, result->recognize_us/1000,
            (unsigned)result->io_error, result->cleanup_status);
    return rc;
}

static int run_vocr(VjoArena *a, VjoOverlayData *out)
{
    VjoState st;
    static VjoGameOcrResult result;
    int local = local_pipeline(&job_cfg);
    st.capture_scene = 0;
    sceClibMemset(&result, 0, sizeof(result));
    out->err.rc = vjo_capture_raw(0, vocr_consume, &result, &st);
    if (out->err.rc != VJO_OK) {
        if (out->err.rc == VJO_E_OCR_REGION && job_cfg.ocr_backend == VJO_OCR_MEIKI)
            out->err.detail = "Select one horizontal dialogue line, at most 96 pixels high, for the Meiki trial.";
        if (out->err.rc == VJO_E_OCR_MODEL) {
            if (job_cfg.ocr_backend == VJO_OCR_MEIKI)
                out->err.detail = "Meiki weights are missing or invalid. Install the pinned meiki-stream-int8.mnn under ux0:data/VitaJPOverlay/ocr.";
            /* Vita errno ENODEV and EACCES are distinct from absent/corrupt weights. */
            if ((uint32_t)result.io_error == 0x80010013u)
                out->err.detail = "The local OCR model storage is unavailable to this game. See log.txt for details.";
            else if ((uint32_t)result.io_error == 0x8001000Du)
                out->err.detail = "Access to the local OCR model was denied. See log.txt for details.";
        }
        return out->err.rc;
    }
    if (local) {
        VjoArena text_arena;
        vjo_arena_init(&text_arena, local_text, sizeof(local_text));
        out->ocr_text = result.text;
        out->filtered = vjo_filter_lines(&text_arena, result.text, job_cfg.non_japanese_filter);
        /* Filtering only removes bytes, so one complete worker text fits. */
        out->sentence = out->filtered ? vjo_java_trim((char *)out->filtered) : NULL;
        if (!out->sentence) {
            out->failed_stage = VJO_STAGE_OCR;
            return out->err.rc = VJO_E_OOM;
        }
        /* Cursor byte0 is the trimmed sentence, not discarded whitespace. */
        size_t len = sceClibStrnlen(out->sentence, sizeof(local_text) - 1);
        sceClibMemmove(local_text, out->sentence, len + 1);
        out->filtered = out->sentence = local_text;
        out->list.header = out->sentence ? out->sentence : "";
        sceKernelLockMutex(cmd_lock, 1, NULL);
        job_text_generation = advance_generation(&local_text_generation);
        job_cursor_seq = advance_generation(&local_cursor_seq);
        local_cursor_byte = job_cursor_byte = job_cfg.japanese_only_selection
            ? (uint32_t)vjo_utf8_japanese_cursor_step(local_text, len, 0, 0) : 0;
        local_text_game_generation = job_generation;
        local_text_scene = job_capture_scene;
        local_text_capture_seq = job_capture_seq;
        __sync_synchronize();
        local_text_valid = 1;
        sceKernelUnlockMutex(cmd_lock, 1);
        if (!job_lookup || !len || job_cursor_byte >= len || mem_alloc_for(&job_cfg) == 0)
            return VJO_OK;
        out->failed_stage = VJO_STAGE_DICT;
        out->err.dict = VJO_DICT_LOCAL;
        out->err.detail = "Text was recognized, but Shell UI memory is too low for dictionary results. Restart the Vita and try again.";
        return out->err.rc = VJO_E_OOM;
    }
    return vjo_overlay_ocr_text(a, &job_cfg, result.text, out);
}
#else
static int run_vocr(VjoArena *a, VjoOverlayData *out)
{
    (void)a;
    out->err.detail = "This build has no vita-vn-ocr backend. Install a build made with VJO_WITH_VOCR, "
                      "or set ocr_backend = lens.";
    return out->err.rc = VJO_E_OCR_UNAVAILABLE;
}
#endif

static int run_job(VjoArena *a, VjoOverlayData *out)
{
    VjoState st;
    VjoJpegSource src;
    VjoBuf jb;
    MemJpeg mem;

    sceClibMemset(out, 0, sizeof(*out));
    out->list.header = "";
    /* Warm connections may still own the old pool when a configuration change
     * requests a job. Only this thread can retire it once that work finishes. */
    if (local_pipeline(&job_cfg)) {
        mem_free();
    } else if (mem_alloc_for(&job_cfg) < 0) {
        out->failed_stage = VJO_STAGE_OCR;
        return out->err.rc = VJO_E_OOM;
    }
    vjo_arena_reset(a);
    if (job_cfg.ocr_backend == VJO_OCR_VOCR || job_cfg.ocr_backend == VJO_OCR_MEIKI) {
        /* On the console only: no image or connection leaves it. */
        if (run_vocr(a, out) != VJO_OK) {
            if (!out->failed_stage)
                out->failed_stage = VJO_STAGE_OCR;
            if (out->err.rc == VJO_E_OCR_UNAVAILABLE)
                out->err.detail = job_cfg.ocr_backend == VJO_OCR_MEIKI
                    ? "Meiki is unavailable. Install the trial worker and its ABI3 engine at app0:/VitaJPOverlay/meiki/meiki-engine.suprx, then restart the game."
                    : "The game OCR worker is unavailable. Enable VitaJPOverlay_OCR under *ALL and restart this game.";
            if (out->err.rc == VJO_E_OOM && out->failed_stage == VJO_STAGE_OCR)
                out->err.detail = job_cfg.ocr_backend == VJO_OCR_MEIKI
                    ? "Meiki needs about 27 MiB of free game memory. Try vocr for this game."
                    : "Select a tighter dialogue region with Square. FL10_w8.vocr uses the least game memory.";
            return out->err.rc;
        }
        goto recognized;
    }
    vjo_buf_init(&jb, a);
    st.capture_scene = 0;
    if ((out->err.rc = vjo_capture_jpeg(a, 0, JPEG_QUALITY, &jb, &st)) != VJO_OK) {
        out->failed_stage = VJO_STAGE_OCR;
        return out->err.rc;
    }
    job_capture_scene = st.capture_scene;
    job_capture_seq = st.region_seq;
    __sync_synchronize(); /* the scene is visible before job_captured */
    job_captured = 1;
    sceClibMemset(&src, 0, sizeof(src));
    src.width = st.width;
    src.height = st.height;
    mem.data = jb.data;
    src.ud = &mem;
    src.read = mem_jpeg_read;
    src.size = (uint32_t)jb.len;
    if (vjo_overlay_ocr(a, &plat, &job_cfg, &src, out))
        return out->err.rc;
recognized:
    /* The screen changed only in pixels (a mark, a cursor, an effect):
     * the cached lookup is this text's. */
    job_same_text = job_cached_text && out->filtered && !sceClibStrcmp(out->filtered, job_cached_text);
    __sync_synchronize(); /* the sentence is visible before job_text_ready */
    job_text_ready = 1;
    if (!job_lookup || job_same_text) return VJO_OK;
    if (local_pipeline(&job_cfg) &&
        job_cursor_byte >= sceClibStrnlen(local_text, sizeof(local_text))) return VJO_OK;
    return local_pipeline(&job_cfg) ? vjo_overlay_lookup_at(a, &plat, &job_cfg, job_cursor_byte, out)
                                   : vjo_overlay_lookup(a, &plat, &job_cfg, out);
}

/* No capture, model, inference or sentence tokenization for cursor moves. */
static int run_cursor_job(VjoArena *a, VjoOverlayData *out)
{
    sceClibMemset(out, 0, sizeof(*out));
    out->ocr_text = out->filtered = out->sentence = local_text;
    out->list.header = local_text;
    if (job_cursor_byte >= sceClibStrnlen(local_text, sizeof(local_text))) return VJO_OK;
    if (mem_alloc_for(&job_cfg) < 0) {
        out->failed_stage = VJO_STAGE_DICT;
        out->err.dict = VJO_DICT_LOCAL;
        out->err.detail = "Text was recognized, but Shell UI memory is too low for dictionary results. Restart the Vita and try again.";
        return out->err.rc = VJO_E_OOM;
    }
    vjo_view_lock(); /* the published list is the static header, never old entries */
    vjo_arena_reset(a);
    vjo_view_unlock();
    return vjo_overlay_lookup_at(a, &plat, &job_cfg, job_cursor_byte, out);
}

static int net_main(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    while (running) {
        unsigned int bits = 0;
        sceKernelWaitEventFlag(net_evf, NET_EV_JOB | NET_EV_WARM | NET_EV_QUIT,
                               SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &bits, NULL);
        if (bits & NET_EV_QUIT)
            break;
        if ((bits & NET_EV_WARM) && !(bits & NET_EV_JOB)) /* a job opens them itself */
            vjo_overlay_warm(&plat, warm_dictionary, WARM_CONNECT_US);
        if (bits & NET_EV_WARM) {
            __sync_synchronize();
            warm_running = 0;
        }
        if (bits & NET_EV_JOB) {
            int idx = job_idx;
            int64_t t0 = now_us();
            job_rc = job_cursor_only ? run_cursor_job(&results[idx], &cache_data[idx])
                                     : run_job(&results[idx], &cache_data[idx]);
            vjo_log("job %d done rc=%d in %d ms, arena peak %u", idx, job_rc,
                    (int)((now_us() - t0) / 1000), (unsigned)results[idx].peak);
            __sync_synchronize(); /* results are visible before job_done */
            job_done = 1;
        }
    }
    return 0;
}

/* The network thread is at work (its memory must stay). */
static int net_busy(void)
{
    __sync_synchronize();
    return job_running || warm_running;
}

/* lookup = 0: OCR only (the subtitles' sentence). */
static void start_job(const char *why, int lookup)
{
    if (job_running)
        return;
    if (local_pipeline(&cfg)) {
        /* Retire the previous lookup before OCR, then recreate result arenas
         * after all capture/model/inference allocations have been freed. */
        retire_local_text(0); /* atomic request exclusion and view detachment */
        if (ov != OV_CLOSED)
            view_publish(1, NULL, "Recognizing…", 0);
        if (!warm_running)
            mem_free();
    }
    vjo_log("job start (%s)%s", why, lookup ? "" : ", OCR only");
    job_lookup = lookup;
    job_cursor_only = 0;
    job_text_generation = 0;
    job_idx = local_pipeline(&cfg) ? 0 : active == 0 ? 1 : 0;
    job_cfg = cfg; /* the control thread may reload cfg while the job runs */
    job_generation = game_generation;
    job_cached_text = !local_pipeline(&cfg) && cache_ok && cache_dictionary == cfg.dictionary
                      ? cache_data[active].filtered : NULL;
    job_same_text = 0;
    job_captured = 0;
    job_capture_scene = 0;
    job_capture_seq = 0;
    job_cancelled = 0;
    vjo_net_cancel_clear(&net_cancel);
    if (view_job_text) { /* its arena may be the new job's */
        view_job_text = 0;
        view_publish(1, NULL, "Recognizing…", 0);
    }
    job_done = 0;
    job_text_ready = 0;
    job_running = 1;
    job_started_us = now_us();
    __sync_synchronize();
    sceKernelSetEventFlag(net_evf, NET_EV_JOB);
    if (subtitles)
        strip_publish(NULL, 0, 1);
}

static void start_cursor_job(void)
{
    VjoState st = {.size = sizeof(st)};
    if (job_running) { cursor_waiting = 1; return; }
    if (ov == OV_CLOSED || !title_id[0] || !local_pipeline(&cfg) || !local_text_valid ||
        local_text_game_generation != game_generation) {
        cursor_waiting = 0;
        return;
    }
    if (vjoGetState(&st) < 0 || !shows_screen(local_text_scene, local_text_capture_seq, &st)) {
        retire_local_text(1);
        view_publish(1, NULL, "The dialogue changed. Close and reopen the overlay.", 1);
        return;
    }
    sceKernelLockMutex(cmd_lock, 1, NULL);
    job_cursor_only = 1;
    job_lookup = 1;
    job_idx = 0;
    job_cfg = cfg;
    job_generation = game_generation;
    job_text_generation = local_text_generation;
    job_cursor_byte = local_cursor_byte;
    job_cursor_seq = local_cursor_seq;
    cursor_waiting = 0;
    job_cached_text = NULL;
    job_same_text = job_cancelled = job_done = job_text_ready = 0;
    job_captured = 1;
    job_capture_scene = local_text_scene;
    job_capture_seq = local_text_capture_seq;
    vjo_net_cancel_clear(&net_cancel);
    local_header = (VjoEntryList){local_text, NULL, 0};
    view_publish(1, &local_header, "Looking up words…", 0);
    job_running = 1;
    job_started_us = now_us();
    __sync_synchronize();
    sceKernelSetEventFlag(net_evf, NET_EV_JOB);
    sceKernelUnlockMutex(cmd_lock, 1);
}

/* ---------------- control thread ---------------- */

static int same_pipeline(const VjoConfig *a, const VjoConfig *b)
{
    if (a->dictionary != b->dictionary || a->ocr_backend != b->ocr_backend ||
        a->non_japanese_filter != b->non_japanese_filter ||
        a->japanese_only_selection != b->japanese_only_selection)
        return 0;
    if ((a->ocr_backend == VJO_OCR_VOCR || a->ocr_backend == VJO_OCR_MEIKI) &&
        (sceClibStrcmp(a->ocr_model_dir, b->ocr_model_dir) ||
         (a->ocr_backend == VJO_OCR_VOCR && sceClibStrcmp(a->vocr_model, b->vocr_model))))
        return 0;
    if (a->dictionary == VJO_DICT_LOCAL)
        return !sceClibStrcmp(a->local_dictionary_dir, b->local_dictionary_dir) &&
               !sceClibStrcmp(a->local_dictionaries, b->local_dictionaries);
    if (a->dictionary == VJO_DICT_HACHIDORI)
        return !sceClibStrcmp(a->hachidori_host, b->hachidori_host);
    return !sceClibStrcmp(vjo_config_api_key(a), vjo_config_api_key(b));
}

static void apply_config(void)
{
    VjoConfig previous = cfg;
    vjo_config_load(&cfg, &scratch);
    if (!same_pipeline(&previous, &cfg)) {
        cache_ok = 0;
        retire_local_text(1);
        if (job_running && !same_pipeline(&job_cfg, &cfg)) {
            job_cancelled = 1;
            vjo_net_cancel(&net_cancel);
        }
    }
    vjo_log_configure(&cfg);
    if (vjoSetTriggers(cfg.toggle_button, cfg.subtitle_button) < 0)
        vjo_log("kernel refused the buttons: keeping the previous ones");
    for (int i = 0; i < cfg.n_warnings; i++)
        vjo_log("config warning: %s", cfg.warnings[i]);
}

static void push_region(void)
{
    VjoRect r;
    int found = vjo_region_load(title_id, &r, &scratch);
    vjoSetRegion(found && r.w ? &r : NULL);
    vjo_log("region: %s%s", found == VJO_REGION_GAME ? "this game's" : found ? "all games'" : "none",
            found && r.w ? "" : ", full screen");
}

static int game_active(void)
{
    return title_id[0] != '\0';
}

/* The running job captured another screen than the one settled now: it is
 * abandoned (Lens and the dictionary take seconds), so the job for this
 * screen can start at once. Returns 1 if it was. */
static int cancel_outdated_job(const VjoState *st)
{
    __sync_synchronize(); /* pairs with the network thread's barrier */
    if (!job_running || job_cancelled || !job_captured || !(st->stable || st->quiet) ||
        shows_screen(job_capture_scene, job_capture_seq, st))
        return 0;
    job_cancelled = 1;
    vjo_net_cancel(&net_cancel);
    vjo_log("job cancelled: the screen changed since its capture");
    return 1;
}

/* Is the cached result for the screen in st? If so it is retagged with
 * st's scene, which stays valid through later captures. */
static int claim_cache_for(const VjoState *st)
{
    if (!cache_ok || active < 0 || !shows_screen(cache_scene, cache_seq, st))
        return 0;
    if (cache_scene != st->scene) {
        vjo_view_lock();
        cache_scene = st->scene;
        vjo_view_unlock();
    }
    return 1;
}

static void clear_input_block_error(void)
{
    if (input_block_error) {
        input_block_error = 0;
        strip_publish("", VJO_STRIP_STATUS, 0);
    }
}

static void open_overlay(void)
{
    VjoState st;
    int block_rc = vjoSetInputBlock(1);
    vjo_log("input block on rc=%d (%08X)", block_rc, block_rc);
    if (block_rc < 0) {
        input_block_error = 1;
        ov = OV_CLOSED;
        view_publish(0, NULL, NULL, 0);
        strip_publish("Overlay unavailable: cannot block game controls.", VJO_STRIP_ERROR, 0);
        vjo_view_lock();
        __atomic_store_n(&g_view.strip_on, 1, __ATOMIC_RELEASE);
        vjo_view_unlock();
        return;
    }
    clear_input_block_error();
    ov = OV_OPEN;
    apply_config(); /* settings are re-read on every open */
    if (!net_busy() && !local_pipeline(&cfg) && mem_alloc() < 0) {
        view_publish(1, NULL, "Not enough memory for the overlay", 1);
        return;
    }
    if (!vjo_config_dict_ready(&cfg)) {
        VjoErr e = {cfg.dictionary == VJO_DICT_HACHIDORI ? VJO_E_NO_HOST : VJO_E_NO_KEY,
                    0, 0, NULL, cfg.dictionary};
        size_t mark = vjo_arena_mark(&scratch);
        view_publish(1, NULL, vjo_err_text(&scratch, VJO_STAGE_DICT, &e), 1);
        vjo_arena_release(&scratch, mark);
        return;
    }
    st.size = sizeof(st);
    vjoGetState(&st);
    if (local_pipeline(&cfg) && local_text_valid &&
        local_text_game_generation == game_generation &&
        shows_screen(local_text_scene, local_text_capture_seq, &st)) {
        local_header = (VjoEntryList){local_text, NULL, 0};
        view_publish(1, &local_header, "Looking up words…", 0);
        start_cursor_job();
        return;
    }
    if (st.alloc_status == VJO_ALLOC_FAIL) {
        view_publish(1, NULL, "Not enough memory to capture the screen", 1);
        return;
    }
    if (cfg.ocr_mode == VJO_OCR_AUTO && claim_cache_for(&st)) {
        vjo_log("open: background result");
        view_show_cache();
        return;
    }
    /* Why the background result can't be used (auto mode tuning). */
    if (cfg.ocr_mode == VJO_OCR_AUTO)
        vjo_log("open: %s (stable %u, quiet %u)",
                job_running ? "background job still running"
                : !cache_ok || active < 0 ? "no background result"
                                          : "screen changed since the background result",
                st.stable, st.quiet);
    view_publish(1, NULL, "Recognizing…", 0);
    view_waiting = 1;
    if (job_running) {
        ov = OV_OPEN_OLD_JOB; /* a cancelled one is followed by a new one */
        cancel_outdated_job(&st);
    } else {
        start_job("overlay opened", 1);
    }
}

static void close_overlay(void)
{
    if (ov == OV_CLOSED)
        return;
    ov = OV_CLOSED;
    cursor_waiting = 0;
    if (job_running && job_cursor_only) job_cancelled = 1;
    view_waiting = view_job_text = 0;
    vjoSetInputBlock(0);
    vjo_view_lock();
    __atomic_store_n(&g_view.open, 0, __ATOMIC_RELEASE);
    /* The next job may reuse the shown arena; an open republishes it. */
    g_view.list = NULL;
    g_view.local_cursor_mode = 0;
    g_view.version++;
    vjo_view_unlock();
}

static void backoff_note(Backoff *b, int failed)
{
    if (!failed) {
        b->us = 0;
        b->until = 0;
        return;
    }
    b->us = b->us ? b->us * 2 : BACKOFF_MIN_US;
    if (b->us > BACKOFF_MAX_US)
        b->us = BACKOFF_MAX_US;
    b->until = now_us() + b->us;
}

/* The strip for a finished job: its sentence, else its error. */
static void strip_show_result(const VjoOverlayData *d)
{
    size_t mark = vjo_arena_mark(&scratch);
    if (d->sentence)
        strip_publish(d->sentence, VJO_STRIP_SENTENCE, 0);
    else
        strip_publish(vjo_err_text(&scratch, d->failed_stage, &d->err), VJO_STRIP_ERROR, 0);
    vjo_arena_release(&scratch, mark);
}

/* The overlay was opened while the job ran: is the result for the screen
 * shown now, with its lookup? Else a new job. Returns 1 if one started. */
static int restart_old_job(int has_lookup)
{
    VjoState st;
    if (ov != OV_OPEN_OLD_JOB)
        return 0;
    ov = OV_OPEN;
    st.size = sizeof(st);
    vjoGetState(&st);
    if (shows_screen(job_capture_scene, job_capture_seq, &st) && has_lookup)
        return 0;
    if (local_pipeline(&cfg) && local_text_valid &&
        local_text_game_generation == game_generation &&
        shows_screen(local_text_scene, local_text_capture_seq, &st)) {
        start_cursor_job();
        return 1;
    }
    if (cfg.ocr_backend == VJO_OCR_MEIKI) {
        view_publish(1, NULL, "The dialogue changed. Close and reopen the overlay.", 1);
        return 1; /* a manual trial never reloads 5.4 MB automatically */
    }
    start_job(has_lookup ? "result was for an earlier screen" : "the running job was OCR only", 1);
    return 1;
}

/* The network thread finished the job. */
static void on_cursor_done(void)
{
    VjoState st = {.size = sizeof(st)};
    int same_screen = vjoGetState(&st) >= 0 && shows_screen(job_capture_scene, job_capture_seq, &st);
    job_running = job_done = job_text_ready = 0;
    sceKernelLockMutex(cmd_lock, 1, NULL);
    int current = job_generation == game_generation &&
                local_text_valid && job_text_generation == local_text_generation &&
                same_pipeline(&job_cfg, &cfg) && same_screen && ov != OV_CLOSED;
    int valid = current && !job_cancelled;
    int newer = valid && job_cursor_seq != local_cursor_seq;
    if (valid && !newer) {
        active = 0;
        cache_ok = cache_data[0].err.rc == VJO_OK;
        cache_dictionary = job_cfg.dictionary;
        cache_scene = job_capture_scene;
        cache_seq = job_capture_seq;
        backoff_note(&dict_backoff, cache_data[0].err.rc != VJO_OK);
        view_show_cache();
    }
    job_cancelled = 0;
    sceKernelUnlockMutex(cmd_lock, 1);
    if (!same_screen && local_text_valid) {
        retire_local_text(1);
        if (ov != OV_CLOSED)
            view_publish(1, NULL, "The dialogue changed. Close and reopen the overlay.", 1);
    }
    /* A close cancels the old result; a reopen may already be waiting for
     * another lookup of this still-current saved text. */
    if (newer || (current && cursor_waiting)) start_cursor_job();
    else cursor_waiting = 0;
}

static void on_job_done(void)
{
    if (job_cursor_only) { on_cursor_done(); return; }
    int idx = job_idx, ocr_failed;
    VjoOverlayData *d = &cache_data[idx];
    __sync_synchronize(); /* pairs with the network thread's barrier */
    job_running = 0;
    job_done = 0;
    job_text_ready = 0;
    if (job_cancelled || job_generation != game_generation || !same_pipeline(&job_cfg, &cfg)) {
        cache_ok = 0;
        job_cancelled = 0;
        if (ov != OV_CLOSED && game_active()) { /* the overlay waits for this screen's */
            ov = OV_OPEN;
            if (cfg.ocr_backend == VJO_OCR_MEIKI)
                view_publish(1, NULL, "The dialogue changed. Close and reopen the overlay.", 1);
            else
                start_job("the screen changed during the last one", 1);
        } else {
            stable_pending = game_active(); /* only the current game's next screen */
            if (subtitles)
                strip_publish(NULL, 0, 0);
        }
        return;
    }
    ocr_failed = d->failed_stage == VJO_STAGE_OCR;
    backoff_note(&ocr_backoff, ocr_failed);
    if (subtitles)
        strip_show_result(d);
    if (job_same_text) {
        /* The cached result's text: that result (its arena stays active)
         * is this screen's. A region change meanwhile cleared cache_ok, but
         * the lookup depends on the text only. */
        vjo_log("same text as the cached result: kept for this screen");
        vjo_view_lock();
        cache_ok = 1;
        cache_scene = job_capture_scene;
        cache_seq = job_capture_seq;
        vjo_view_unlock();
        if (ov == OV_OPEN && !job_lookup)
            return; /* OCR only: the overlay already shows a result */
        if (!restart_old_job(1) && ov != OV_CLOSED)
            view_show_cache();
        return;
    }
    if (!ocr_failed && job_lookup)
        backoff_note(&dict_backoff, d->err.rc != VJO_OK);
    /* Character moves received while initial lookup was working supersede
     * that definition; the saved OCR sentence is already immutable. */
    if (local_pipeline(&job_cfg) && local_text_valid && ov != OV_CLOSED &&
        (cursor_waiting || (job_lookup && job_cursor_seq != local_cursor_seq))) {
        start_cursor_job();
        return;
    }
    if (!job_lookup && ov == OV_OPEN) {
        /* OCR only, while the overlay shows a result or an error: that stays
         * (and its arena stays active); the strip took the sentence */
        return;
    }
    /* Publish the new arena; the old one becomes the next job's target. */
    int local = local_pipeline(&job_cfg);
    if (local) sceKernelLockMutex(cmd_lock, 1, NULL);
    if (local && ov != OV_CLOSED && job_lookup && job_cursor_seq != local_cursor_seq) {
        sceKernelUnlockMutex(cmd_lock, 1);
        start_cursor_job();
        return;
    }
    vjo_view_lock();
    active = idx;
    cache_ok = d->err.rc == VJO_OK && job_lookup; /* the overlay needs the lookup */
    cache_dictionary = job_cfg.dictionary;
    cache_scene = job_capture_scene;
    cache_seq = job_capture_seq;
    vjo_view_unlock();
    int needs_restart = ov == OV_OPEN_OLD_JOB;
    if (!needs_restart && ov != OV_CLOSED)
        view_show_cache();
    if (local) sceKernelUnlockMutex(cmd_lock, 1);
    /* Restart helpers may take cmd_lock; never call them while holding it. */
    if (needs_restart && !restart_old_job(job_lookup) && ov != OV_CLOSED) {
        if (local) sceKernelLockMutex(cmd_lock, 1, NULL);
        int newer = local && job_lookup && job_cursor_seq != local_cursor_seq;
        if (!newer) view_show_cache();
        if (local) sceKernelUnlockMutex(cmd_lock, 1);
        if (newer) start_cursor_job();
    }
}

/* Does a job started now look the words up? Yes when the overlay can show
 * them: it is open, or opens from the background result (auto). */
static int want_lookup(void)
{
    return vjo_config_dict_ready(&cfg) &&
           (local_pipeline(&cfg) ? ov != OV_CLOSED : ov != OV_CLOSED || cfg.ocr_mode == VJO_OCR_AUTO) &&
           now_us() >= dict_backoff.until;
}

/* The network thread posted the job's sentence (before its lookup). */
static void on_job_text(void)
{
    const char *sentence;
    __sync_synchronize(); /* pairs with the network thread's barrier */
    sentence = cache_data[job_idx].sentence;
    job_text_ready = 0;
    if (job_cancelled || job_generation != game_generation)
        return; /* for an earlier screen */
    vjo_log("subtitle text ready in %d ms", (int)((now_us() - job_started_us) / 1000));
    if (subtitles && sentence)
        strip_publish(sentence, VJO_STRIP_SENTENCE, 1);
    /* The overlay waits for this job: the sentence now, the words after
     * the lookup (if the job is for the screen shown). */
    if (ov != OV_CLOSED && view_waiting && job_lookup && !job_same_text && sentence) {
        VjoState st;
        st.size = sizeof(st);
        vjoGetState(&st);
        if (shows_screen(job_capture_scene, job_capture_seq, &st)) {
            job_text_list.header = sentence;
            job_text_list.entries = NULL;
            job_text_list.n_entries = 0;
            view_publish(1, &job_text_list, "Looking up words…", 0);
            view_job_text = 1;
        }
    }
}

static void set_subtitles(int on)
{
    if (on) apply_config();
    if (on && cfg.ocr_backend == VJO_OCR_MEIKI) {
        subtitles = 0;
        strip_publish("Meiki is manual only. Select one dialogue line and open the overlay.", VJO_STRIP_ERROR, 0);
        return;
    }
    VjoState st;
    const VjoOverlayData *d = active >= 0 ? &cache_data[active] : NULL;
    subtitles = on;
    vjo_log("subtitles %s", on ? "on" : "off");
    if (!on) {
        strip_publish("", VJO_STRIP_SENTENCE, 0);
        return;
    }
    if (!net_busy() && !local_pipeline(&cfg) && mem_alloc() < 0) {
        strip_publish("Not enough memory for subtitles", VJO_STRIP_ERROR, 0);
        return;
    }
    /* settings were re-read before choosing the backend */
    st.size = sizeof(st);
    vjoGetState(&st);
    if (st.alloc_status == VJO_ALLOC_FAIL) {
        strip_publish("Not enough memory to capture the screen", VJO_STRIP_ERROR, 0);
        return;
    }
    if (!job_running && d && d->sentence && shows_screen(cache_scene, cache_seq, &st)) {
        strip_publish(d->sentence, VJO_STRIP_SENTENCE, 0);
        return;
    }
    strip_publish("Recognizing…", VJO_STRIP_STATUS, 1);
    start_job("subtitles on", want_lookup()); /* if one runs, its result comes */
}

/* subtitle_button: from the full overlay, everything closes. */
static void on_subtitle(void)
{
    if (!game_active())
        return;
    if (ov != OV_CLOSED) {
        close_overlay();
        if (subtitles)
            set_subtitles(0);
    } else {
        set_subtitles(!subtitles);
    }
}

/* A command from the overlay (paf main thread). */
static void on_command(void)
{
    int cmd;
    VjoRect r;
    uint32_t cursor_byte, cursor_generation;
    sceKernelLockMutex(cmd_lock, 1, NULL);
    cmd = pending_cmd;
    r = pending_rect;
    cursor_byte = pending_cursor_byte;
    cursor_generation = pending_cursor_generation;
    pending_cmd = VJO_CMD_NONE;
    sceKernelUnlockMutex(cmd_lock, 1);

    switch (cmd) {
    case VJO_CMD_CURSOR:
        if (ov == OV_CLOSED || !local_pipeline(&cfg) || !local_text_valid ||
            cursor_generation != local_text_generation || cursor_byte != local_cursor_byte)
            break;
        if (!job_running || !job_cursor_only || job_cursor_seq != local_cursor_seq)
            start_cursor_job();
        break;
    case VJO_CMD_CLOSED:
        close_overlay();
        break;
    case VJO_CMD_SET_REGION:
    case VJO_CMD_CLEAR_REGION:
        if (!game_active()) { /* the game exited meanwhile */
            close_overlay();
            break;
        }
        if (cmd == VJO_CMD_SET_REGION)
            vjo_log("region set for %s: %u,%u,%u,%u", title_id, r.x, r.y, r.w, r.h);
        else
            vjo_log("region for %s: full screen", title_id);
        if (vjo_region_save(title_id, cmd == VJO_CMD_SET_REGION ? &r : NULL, &scratch) < 0)
            vjo_log("region.ini: write failed");
        push_region();
        cache_ok = 0;
        retire_local_text(1);
        close_overlay();
        if (subtitles)
            start_job("region changed", want_lookup());
        break;
    }
}

/* ---- is the new foreground process a game? ----
 * Its title ID is often not readable right when the process starts, so the
 * lookup is retried for a few seconds. Unknown after that = a game. */
#define CLASSIFY_TIMEOUT_US 3000000

static SceUID pending_pid;
static int64_t pending_since;

static void activate_game(SceUID pid, const char *tid, int mode)
{
    if (!vjo_game_enabled(tid, &scratch)) {
        title_id[0] = '\0';
        vjoSetGameActive(pid, VJO_GAME_NONE);
        vjo_log("%s: JP Overlay disabled; game input passes through", tid);
        return;
    }
    sceClibSnprintf(title_id, sizeof(title_id), "%s", tid);
    apply_config();
    if (!net_busy() && !local_pipeline(&cfg))
        mem_alloc();
    push_region();
    backoff_note(&ocr_backoff, 0);
    backoff_note(&dict_backoff, 0);
    vjoSetGameActive(pid, mode);
    vjo_log("game %s started: dictionary %s (key %s), trigger %s, subtitles %s, ocr_mode %s", title_id,
            vjo_dict_name(cfg.dictionary), vjo_config_api_key(&cfg)[0] ? "set" : "MISSING",
            vjo_trigger_name(cfg.toggle_button), vjo_trigger_name(cfg.subtitle_button),
            cfg.ocr_mode == VJO_OCR_AUTO ? "auto" : "on_press");
}

static void classify_pending(void)
{
    char tid[32];
    SceUID pid = pending_pid;
    int ret = vjo_title_id(pid, tid, sizeof(tid));
    if (ret == 0) {
        pending_pid = 0;
        int mode = vjo_title_game_mode(tid);
        if (mode == VJO_GAME_NONE) {
            vjo_log("%s: system app, not a game", tid);
            vjoSetGameActive(pid, VJO_GAME_NONE);
        } else {
            activate_game(pid, tid, mode);
        }
    } else if (now_us() - pending_since > CLASSIFY_TIMEOUT_US) {
        pending_pid = 0;
        vjo_log("no title ID for 0x%X (0x%08X): treating it as a game", pid, ret);
        activate_game(pid, "GAME", VJO_GAME);
    }
}

static void invalidate_game_work(void)
{
    if (++game_generation == 0)
        game_generation = 1;
    cache_ok = 0;
    retire_local_text(1);
    stable_pending = 0;
    if (job_running)
        job_cancelled = 1;
    if (net_busy())
        vjo_net_cancel(&net_cancel);
}

static void on_game_exit(void)
{
    invalidate_game_work();
    pending_pid = 0;
    close_overlay();
    if (subtitles)
        set_subtitles(0);
    clear_input_block_error();
    /* Free memory unless the network thread still uses it. */
    if (!net_busy())
        mem_free();
    title_id[0] = '\0';
}

/* A new foreground process: classified by classify_pending. */
static void on_game_start(void)
{
    VjoState st;
    invalidate_game_work();
    close_overlay();
    if (subtitles) /* over a game, without its exit: the strip goes */
        set_subtitles(0);
    clear_input_block_error();
    st.size = sizeof(st);
    vjoGetState(&st);
    title_id[0] = '\0';
    pending_pid = st.game_pid;
    pending_since = now_us();
}

static void on_trigger(void)
{
    if (ov != OV_CLOSED)
        close_overlay();
    else if (game_active())
        open_overlay();
}

/* A background job for a settled screen: the auto prefetch (for an instant
 * overlay) and the subtitles' refresh (whatever ocr_mode says). */
static int background_wanted(void)
{
    return cfg.ocr_backend != VJO_OCR_MEIKI &&
           (subtitles || (cfg.ocr_mode == VJO_OCR_AUTO && (local_pipeline(&cfg) || want_lookup())));
}

static int background_throttled(void)
{
    int64_t t = now_us();
    return t < ocr_backoff.until || (subtitles && t < job_started_us + SUBTITLE_GAP_US);
}

/* A stable event that arrives while busy (or throttled) is kept, so the
 * newest screen is still recognized afterwards. With subtitles, a region
 * that never settles (a video behind the text) is recognized every
 * UNSETTLED_US anyway. */
static void auto_prefetch(void)
{
    VjoState st;
    int unsettled_due;
    if (ov != OV_CLOSED || job_running || !game_active() || !background_wanted() || background_throttled())
        return;
    unsettled_due = subtitles && now_us() - job_started_us >= UNSETTLED_US;
    if (!stable_pending && !unsettled_due)
        return;
    st.size = sizeof(st);
    vjoGetState(&st);
    if (stable_pending) {
        stable_pending = 0;
        if ((st.stable || st.quiet) && !claim_cache_for(&st))
            start_job(st.stable ? "auto: screen settled" : "auto: screen quiet", want_lookup());
    } else if (st.unsettled_ms >= UNSETTLED_US / 1000) {
        start_job("subtitles: screen keeps changing", 0); /* no lookup every 2 s */
    }
}

/* When the region starts changing, the network thread readies the next
 * job's connections (keeping usable ones), so its requests skip the
 * handshakes. */
static void warm_connections(void)
{
    VjoState st;
    int lookup;
    if (!game_active() || !has_result_memory() || net_busy() || now_us() - warm_asked_us < WARM_GAP_US)
        return;
    if (cfg.ocr_backend != VJO_OCR_LENS) /* local OCR opens no Lens connection */
        return;
    lookup = vjo_config_dict_ready(&cfg);
    if (!lookup && !subtitles)
        return;
    st.size = sizeof(st);
    vjoGetState(&st);
    if (!st.unsettled_ms || st.unsettled_ms > WARM_WINDOW_MS)
        return;
    warm_asked_us = now_us();
    warm_dictionary = lookup ? cfg.dictionary : -1;
    vjo_net_cancel_clear(&net_cancel);
    warm_running = 1;
    __sync_synchronize();
    sceKernelSetEventFlag(net_evf, NET_EV_WARM);
}

static int ctl_main(SceSize args, void *argp)
{
    (void)args;
    (void)argp;

    /* The order of the handlers within one wakeup is part of the policy
     * (e.g. a trigger and a game start in the same wakeup). */
    while (running) {
        uint32_t bits = 0;
        vjoWaitEvent(VJO_EV_TRIGGER | VJO_EV_SUBTITLE | VJO_EV_REGION_STABLE | VJO_EV_REGION_QUIET |
                         VJO_EV_GAME_START | VJO_EV_GAME_EXIT,
                     &bits, POLL_TIMEOUT_US);

        if (job_running) sceKernelPowerTick(0);
        if (bits & VJO_EV_GAME_EXIT)
            on_game_exit();
        if (bits & VJO_EV_GAME_START)
            on_game_start();
        if (pending_pid > 0)
            classify_pending();
        if (bits & VJO_EV_TRIGGER)
            on_trigger();
        if (bits & VJO_EV_SUBTITLE)
            on_subtitle();
        if (pending_cmd != VJO_CMD_NONE)
            on_command();
        if (job_running && job_text_ready)
            on_job_text();
        if (job_running && job_done)
            on_job_done();
        if (bits & (VJO_EV_REGION_STABLE | VJO_EV_REGION_QUIET)) {
            stable_pending = 1;
            if (job_running) {
                VjoState st;
                st.size = sizeof(st);
                vjoGetState(&st);
                cancel_outdated_job(&st);
            }
        }
        auto_prefetch();
        warm_connections();
        if (!game_active() && !net_busy() && has_result_memory() && ov == OV_CLOSED)
            mem_free();
    }
    return 0;
}

int vjo_worker_start(void)
{
    int ver = vjoGetVersion(), rc;
    if (ver != VJO_API_VERSION) {
        vjo_log("kernel API version %d, expected %d: not starting", ver, VJO_API_VERSION);
        return -1;
    }
    rc = vjoRegisterShell();
    if (rc < 0) {
        vjo_log("kernel refused the shell registration %d: not starting", rc);
        return -1;
    }
    view_lock = sceKernelCreateMutex("VjoView", 0, 0, NULL);
    cmd_lock = sceKernelCreateMutex("VjoCmd", 0, 0, NULL);
    net_evf = sceKernelCreateEventFlag("VjoNetEv", 0, 0, NULL);
    if (view_lock < 0 || cmd_lock < 0 || vjo_capture_init() < 0 || net_evf < 0 || vjo_net_cancel_init(&net_cancel) < 0) {
        vjo_worker_stop();
        return -1;
    }
    vjo_platform_vita(&plat, &net_cancel);
    plat.pool = &pool; /* no slots until mem_alloc */
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    vjo_arena_init(&results[0], NULL, 0);
    vjo_arena_init(&results[1], NULL, 0);
    apply_config(); /* logging and triggers are set up before the first game */
    vjo_log("Vita JP Overlay shell started (kernel API %d)", ver);

    net_thread = sceKernelCreateThread("VjoNet", net_main, 0x10000100, 0x10000, 0, 0, NULL);
    ctl_thread = sceKernelCreateThread("VjoControl", ctl_main, 0x10000100, 0x4000, 0, 0, NULL);
    if (net_thread < 0 || ctl_thread < 0) {
        vjo_worker_stop();
        return -1;
    }
    if (sceKernelStartThread(net_thread, 0, NULL) < 0) {
        vjo_worker_stop();
        return -1;
    }
    threads_started = 1;
    if (sceKernelStartThread(ctl_thread, 0, NULL) < 0) {
        vjo_worker_stop();
        return -1;
    }
    threads_started |= 2;
    return 0;
}

/* Also undoes a partial vjo_worker_start. */
int vjo_worker_stop(void)
{
    running = 0;
    if (net_cancel.lock >= 0)
        vjo_net_cancel(&net_cancel); /* a request in progress ends now */
    if (net_evf >= 0)
        sceKernelSetEventFlag(net_evf, NET_EV_QUIT);
    if (threads_started) {
        if (((threads_started & 1) && sceKernelWaitThreadEnd(net_thread, NULL, NULL) < 0) ||
            ((threads_started & 2) && sceKernelWaitThreadEnd(ctl_thread, NULL, NULL) < 0)) {
            vjo_log("worker join failed; keeping allocations until stop can be retried");
            return -1;
        }
        threads_started = 0;
    }
    if (net_thread >= 0)
        sceKernelDeleteThread(net_thread);
    if (ctl_thread >= 0)
        sceKernelDeleteThread(ctl_thread);
    net_thread = ctl_thread = -1;
    mem_free();
    if (net_evf >= 0)
        sceKernelDeleteEventFlag(net_evf);
    if (cmd_lock >= 0)
        sceKernelDeleteMutex(cmd_lock);
    vjo_capture_fini();
    if (view_lock >= 0)
        sceKernelDeleteMutex(view_lock);
    if (net_cancel.lock >= 0)
        sceKernelDeleteMutex(net_cancel.lock);
    net_evf = cmd_lock = view_lock = net_cancel.lock = -1;
    return 0;
}
