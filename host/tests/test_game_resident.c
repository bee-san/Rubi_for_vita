/* The actual game worker (game_ocr/main.c) and core adapter (core/vocr_ocr.c)
 * with synthetic pinned weights (test_resident_models.h), so the resident
 * weights' lifecycle runs where the release files are absent (CI): read and
 * SHA-256 checked once, kept while the file's size and modification time stay
 * the same, released on change, failure, model change, refused preflight,
 * inactive game and stop. The line finder and recognizer are spies: this
 * makes no inference, accuracy or console-timing claim. */
#include "acutest.h"
#include <psp2/host_stubs.h>
#include "vocr.h"
#include "vocr_lines.h"
#include "test_resident_weights.h"
#include "../../game_ocr/main.c"
#include <bearssl_hash.h>

#define W 200u
#define H 60u
static unsigned char job_memory[1024*1024] __attribute__((aligned(64)));
static unsigned char weights_memory[VJO_VOCR_F20_BYTES + 4096] __attribute__((aligned(64)));
static unsigned char pixels[W*H*4];
static int64_t available;
static unsigned live_bytes, peak_bytes, job_block, weights_block;
static int job_live, weights_live, job_alloc_failure, weights_alloc_failure, weights_free_failure;
static unsigned job_allocs, job_frees, weights_allocs, weights_frees;
static int registered_rc, quit_after_delay, cancel_now, cancel_at_open, cancel_after_reads;
static const char *served;        /* the model the kernel would open for this job */
static int open_failure, size_delta, stat_failure, corrupt;
static uint64_t mtime;
static unsigned opens, closes, reads, bytes_read, loads, delays;
static int file_open_now;
static uint64_t clock_value;

/* ---- the Vita SDK and kernel transport, as the worker sees them ---- */
SceUID sceKernelGetProcessId(void) { return 7; }
uint64_t sceKernelGetProcessTimeWide(void) { return clock_value++; }
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *out)
{ out->size_user = (int)(available - live_bytes); return 0; }
SceUID sceKernelAllocMemBlock(const char *name, int type, unsigned size, void *opts)
{
    TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW && !(size & 4095));
    if (!strcmp(name, "VjoGameVocrWeights")) {
        TEST_CHECK(!weights_live && size <= sizeof(weights_memory)); /* never two weight blocks */
        weights_allocs++;
        if (weights_alloc_failure) return -1;
        weights_live = 1; weights_block = size;
    } else {
        TEST_CHECK(!strcmp(name, "VjoGameVocr") && !job_live && size <= sizeof(job_memory));
        job_allocs++;
        if (job_alloc_failure) return -1;
        job_live = 1; job_block = size;
    }
    live_bytes += size;
    if (live_bytes > peak_bytes) peak_bytes = live_bytes;
    return !strcmp(name, "VjoGameVocrWeights") ? 10 : 9;
}
int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    TEST_CHECK(uid == 10 ? weights_live : uid == 9 && job_live);
    *base = uid == 10 ? (void *)weights_memory : (void *)job_memory; return 0;
}
int sceKernelFreeMemBlock(SceUID uid)
{
    if (uid == 10) {
        TEST_CHECK(weights_live);
        if (weights_free_failure) return -1;
        weights_live = 0; weights_frees++; live_bytes -= weights_block; return 0;
    }
    TEST_CHECK(uid == 9 && job_live);
    job_live = 0; job_frees++; live_bytes -= job_block; return 0;
}
SceUID sceKernelCreateThread(const char *n, int (*entry)(SceSize, void *), int p, unsigned stack, unsigned a, int cpu, void *opt) { return 20; }
int sceKernelStartThread(SceUID uid, SceSize size, void *args) { return 0; }
int sceKernelWaitThreadEnd(SceUID uid, int *status, void *timeout) { return 0; }
int sceKernelDeleteThread(SceUID uid) { return 0; }
int sceKernelDelayThread(unsigned us)
{ delays++; clock_value += us; if (quit_after_delay) quitting = 1; return 0; }
int sceKernelGetProcessTitleId(SceUID pid, char *out, SceSize size) { snprintf(out, size, "PCSG00415"); return 0; }
int sceAppMgrAppParamGetString(int pid, int param, char *out, SceSize cap) { return -1; }
SceUID sceIoOpen(const char *path, int flags, unsigned mode) { return (int)0x80010013u; }
int vjoGetVersion(void) { return VJO_API_VERSION; }
int vjoOcrRegister(void) { return registered_rc; }
int vjoOcrTake(VjoGameOcrRequest *out) { return 1; }
int vjoOcrCancelled(uint32_t seq)
{ return cancel_now || (cancel_after_reads && reads >= (unsigned)cancel_after_reads); }
int vjoOcrComplete(const VjoGameOcrResult *out) { TEST_CHECK(!job_live); return 0; }
int vjoOcrReadRaw(uint32_t seq, uint32_t row, uint32_t n, void *dst)
{ TEST_CHECK(row < H && n <= H-row); memcpy(dst, pixels+row*W*4, n*W*4); return (int)n; }

static const VjoVocrModel *served_model(void) { return vjo_vocr_model(served); }
static uint32_t served_seed(void) { return !strcmp(served, "FL10_w8.vocr") ? TEST_FL10_SEED : TEST_H15_SEED; }
int vjoOcrModelOpen(uint32_t seq, VjoGameOcrModelInfo *info)
{
    TEST_CHECK(seq == 12 && !file_open_now);
    opens++;
    /* The Shell cancels while the kernel opens the file (it answers CANCELLED). */
    if (cancel_at_open) { cancel_now = 1; return VJO_ERR_CANCELLED; }
    if (open_failure) return open_failure;
    file_open_now = 1;
    uint32_t size = served_model()->bytes + (uint32_t)size_delta;
    *info = (VjoGameOcrModelInfo){sizeof(*info), size, stat_failure, 0, stat_failure ? 0 : mtime};
    return (int)size;
}
int vjoOcrModelRead(uint32_t seq, uint32_t off, uint32_t n, void *dst)
{
    TEST_CHECK(seq == 12 && file_open_now && n && n <= VJO_GAME_OCR_MODEL_CHUNK);
    reads++;
    for (uint32_t i = 0; i < n; i++)
        ((unsigned char *)dst)[i] = test_weight_byte(off + i, served_seed()) ^ (corrupt && off + i == 777 ? 1 : 0);
    bytes_read += n;
    return (int)n;
}
int vjoOcrModelClose(uint32_t seq) { TEST_CHECK(seq == 12); if (file_open_now) closes++; file_open_now = 0; return 0; }

/* ---- the runtime: spies, each checking it got the verified resident bytes ---- */
int vocr_find_lines(const VocrLinesImage *im, int mode, void *arena, size_t bytes, VocrLineSet *out)
{
    memset(out, 0, sizeof(*out));
    out->count = 1; out->lines[0] = (VocrLine){10, 15, 180, 24, 0};
    return VOCR_LINES_OK;
}
int vocr_load(vocr_model *m, const void *blob, size_t bytes)
{
    const VjoVocrModel *pin = served_model();
    loads++;
    TEST_CHECK(blob == (const void *)weights_memory && bytes == pin->bytes && weights_live);
    for (uint32_t i = 0; i < bytes; i += 997)
        if (((const unsigned char *)blob)[i] != test_weight_byte(i, served_seed())) { TEST_CHECK(!"resident bytes changed"); break; }
    memset(m, 0, sizeof(*m)); m->blob = blob; m->bytes = bytes;
    return VOCR_OK;
}
size_t vocr_arena_bytes(const vocr_model *m, const vocr_opts *o) { return served_model()->arena_bytes; }
int vocr_init(vocr_ctx *c, const vocr_model *m, const vocr_opts *o, void *arena, size_t bytes) { return VOCR_OK; }
int vocr_run(vocr_ctx *c, const vocr_image *img, char *text, size_t cap, vocr_extra *extra)
{ snprintf(text, cap, "猫"); extra->frames = 22; extra->width = 176; return VOCR_OK; }

/* ---- helpers ---- */
static VjoGameOcrRequest request(const char *name)
{
    VjoGameOcrRequest r = {.size=sizeof(r), .seq=12, .done_seq=11, .width=W, .height=H, .stride=W*4};
    snprintf(r.model, sizeof(r.model), "%s", name);
    snprintf(r.model_dir, sizeof(r.model_dir), "%s", VJO_VOCR_MODEL_ROOT);
    served = name;
    return r;
}
static void reset(void)
{
    if (weights_live) { weights_free_failure = 0; TEST_CHECK(!drop_weights()); }
    worker = workspace_uid = weights_uid = -1;
    resident.model = NULL; resident.data = NULL; resident.capacity = 0; resident.size = resident.stamp = 0;
    model_owned = 0; model_seq = 0;
    quitting = active_seq = 0; worker_started = claim_outstanding = blocked = io_error = stat_error = 0;
    available = 64*1024*1024; live_bytes = peak_bytes = 0;
    job_live = weights_live = job_alloc_failure = weights_alloc_failure = weights_free_failure = 0;
    job_allocs = job_frees = weights_allocs = weights_frees = 0;
    registered_rc = quit_after_delay = cancel_now = cancel_at_open = cancel_after_reads = 0;
    open_failure = size_delta = stat_failure = corrupt = 0; mtime = 0x12345678;
    opens = closes = reads = bytes_read = loads = delays = 0; file_open_now = 0;
    clock_value = 0;
    memset(pixels, 0, sizeof(pixels));
}
static int run(const char *name, VjoGameOcrResult *out)
{
    VjoGameOcrRequest r = request(name);
    int rc = vjo_game_ocr_run_request(&r, out);
    TEST_CHECK(!job_live && !file_open_now && !model_owned && !out->cleanup_status);
    /* Between jobs a weights block is held only for verified weights. */
    TEST_CHECK(weights_live == (resident.model != NULL));
    TEST_CHECK(out->resident_bytes == (weights_live ? weights_block : 0));
    return rc;
}

/* ---- tests ---- */
static void synthetic_pins_match_their_bytes(void)
{
    const char *names[] = {"FL10_w8.vocr", "H15_w8.vocr"};
    for (unsigned k = 0; k < 2; k++) {
        const VjoVocrModel *m = vjo_vocr_model(names[k]);
        uint32_t seed = k ? TEST_H15_SEED : TEST_FL10_SEED;
        br_sha256_context h; unsigned char digest[32], byte;
        br_sha256_init(&h);
        for (uint32_t i = 0; i < m->bytes; i++) { byte = test_weight_byte(i, seed); br_sha256_update(&h, &byte, 1); }
        br_sha256_out(&h, digest);
        TEST_CHECK(!memcmp(digest, m->sha256, 32));
    }
}
static void weights_are_read_once_and_kept(void)
{
    reset(); VjoGameOcrResult out;
    const uint32_t n = TEST_FL10_BYTES, pieces = (n + VJO_GAME_OCR_MODEL_CHUNK - 1) / VJO_GAME_OCR_MODEL_CHUNK;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && !strcmp(out.text, "猫"));
    TEST_CHECK(out.weights == VJO_GAME_OCR_WEIGHTS_READ && bytes_read == n && reads == pieces);
    TEST_CHECK(weights_allocs == 1 && weights_live && out.resident_bytes == PAGE_ROUND(n));
    TEST_CHECK(out.workspace_bytes == vjo_vocr_job_bytes("FL10_w8.vocr", W, H));
    for (unsigned i = 0; i < 50; i++) {
        TEST_CHECK(!run("FL10_w8.vocr", &out) && !strcmp(out.text, "猫"));
        TEST_CHECK(out.weights == VJO_GAME_OCR_WEIGHTS_RESIDENT);
    }
    /* Each job checks the file's size and time (Open/Close), none reads it again. */
    TEST_CHECK(bytes_read == n && reads == pieces && opens == 51 && closes == 51 && loads == 51);
    TEST_CHECK(weights_allocs == 1 && !weights_frees && job_allocs == 51 && job_frees == 51);
    TEST_CHECK(!delays); /* answered model calls never make the worker sleep */
}
static void changed_file_is_read_again(void)
{
    reset(); VjoGameOcrResult out;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_READ);
    mtime++; /* replaced, same size */
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_READ);
    TEST_CHECK(bytes_read == 2*TEST_FL10_BYTES && weights_allocs == 1 && !weights_frees); /* same block reused */
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_RESIDENT);
    size_delta = 1; /* a different length is never this model: refused, weights released */
    TEST_CHECK(run("FL10_w8.vocr", &out) == VJO_E_OCR_MODEL && !out.text[0]);
    TEST_CHECK(!weights_live && weights_frees == 1 && !out.resident_bytes && bytes_read == 2*TEST_FL10_BYTES);
    size_delta = 0;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_READ && weights_allocs == 2);
}
static void unknown_modification_time_reads_every_job(void)
{
    reset(); VjoGameOcrResult out; stat_failure = (int)0x80010016u;
    for (unsigned i = 0; i < 3; i++) {
        TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_READ);
        TEST_CHECK(out.stat_error == stat_failure);
    }
    TEST_CHECK(bytes_read == 3*TEST_FL10_BYTES && weights_allocs == 1);
    stat_failure = 0; /* known again: the next job keys on it, then reuses */
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_READ && !out.stat_error);
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_RESIDENT);
}
static void missing_or_corrupt_file_releases_weights(void)
{
    reset(); VjoGameOcrResult out;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && weights_live);
    loads = 0;
    open_failure = (int)0x80010002u; /* deleted while the game runs */
    TEST_CHECK(run("FL10_w8.vocr", &out) == VJO_E_OCR_MODEL && out.io_error == open_failure);
    TEST_CHECK(!weights_live && !out.resident_bytes);
    open_failure = 0; corrupt = 1; mtime++;
    TEST_CHECK(run("FL10_w8.vocr", &out) == VJO_E_OCR_MODEL && !weights_live && !loads);
    corrupt = 0;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && weights_live && loads == 1);
    corrupt = 1; /* same size and time: the verified copy is used, the card is not read */
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_RESIDENT);
}
static void cancellation_keeps_verified_and_drops_partial_weights(void)
{
    reset(); VjoGameOcrResult out;
    TEST_CHECK(!run("FL10_w8.vocr", &out));
    cancel_at_open = 1; /* cancelled while checking the file: proves nothing about it */
    TEST_CHECK(run("FL10_w8.vocr", &out) == VJO_E_CANCELLED && weights_live && resident.model);
    TEST_CHECK(out.resident_bytes == weights_block && !out.text[0]);
    cancel_at_open = cancel_now = 0;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_RESIDENT);
    mtime++; cancel_after_reads = (int)reads + 2; /* cancelled half-way through a new read */
    TEST_CHECK(run("FL10_w8.vocr", &out) == VJO_E_CANCELLED && !weights_live && !resident.model);
    cancel_after_reads = 0;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_READ);
}
static void model_change_releases_the_old_weights_first(void)
{
    reset(); VjoGameOcrResult out;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && weights_block == PAGE_ROUND(TEST_FL10_BYTES));
    peak_bytes = live_bytes;
    TEST_CHECK(!run("H15_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_READ);
    TEST_CHECK(weights_frees == 1 && weights_allocs == 2 && weights_block == PAGE_ROUND(TEST_H15_BYTES));
    /* The alloc stub asserts one weights block at a time; the peak is H15's job. */
    TEST_CHECK(peak_bytes == PAGE_ROUND(TEST_H15_BYTES) + PAGE_ROUND(vjo_vocr_job_bytes("H15_w8.vocr", W, H)));
    TEST_CHECK(!run("FL10_w8.vocr", &out) && weights_frees == 2 && weights_block == PAGE_ROUND(TEST_FL10_BYTES));
}
static void peak_is_the_old_single_block_within_a_page(void)
{
    const char *names[] = {"FL10_w8.vocr", "H15_w8.vocr"};
    for (unsigned k = 0; k < 2; k++) {
        reset(); VjoGameOcrResult out;
        TEST_CHECK(!run(names[k], &out));
        uint32_t old_block = PAGE_ROUND(vjo_vocr_workspace_bytes(names[k], W, H));
        TEST_CHECK(peak_bytes <= old_block + 4096u);
        TEST_MSG("%s: peak %u, one-block %u", names[k], peak_bytes, old_block);
        TEST_CHECK(live_bytes == PAGE_ROUND(vjo_vocr_model(names[k])->bytes)); /* kept between jobs */
    }
}
static void preflight_counts_resident_weights_and_refusal_releases_them(void)
{
    reset(); VjoGameOcrResult out;
    const uint32_t job = PAGE_ROUND(vjo_vocr_job_bytes("FL10_w8.vocr", W, H)), weights = PAGE_ROUND(TEST_FL10_BYTES);
    available = GAME_RESERVE + job + weights - 1; /* the first job also needs the weights */
    TEST_CHECK(run("FL10_w8.vocr", &out) == VJO_E_OOM && !job_allocs && !weights_allocs);
    available = GAME_RESERVE + job + weights;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && weights_live);
    /* Resident weights are not counted twice: the reserve and the job fit exactly. */
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_RESIDENT);
    available--; /* the game took a byte: the job no longer fits beside its reserve */
    TEST_CHECK(run("FL10_w8.vocr", &out) == VJO_E_OOM && !weights_live && weights_frees == 1);
    TEST_CHECK(job_allocs == 2 && !out.resident_bytes); /* refused before allocating */
    weights_alloc_failure = 1; available = 64*1024*1024; /* the weights block itself is refused */
    TEST_CHECK(run("FL10_w8.vocr", &out) == VJO_E_OOM && !weights_live && !out.text[0]);
    weights_alloc_failure = 0;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && out.weights == VJO_GAME_OCR_WEIGHTS_READ);
}
static void inactive_game_and_stop_release_weights(void)
{
    reset(); VjoGameOcrResult out;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && weights_live);
    registered_rc = VJO_ERR_BUSY; quit_after_delay = 1; /* another claim pending: keep them */
    worker_main(0, NULL);
    TEST_CHECK(weights_live && !blocked);
    quitting = 0; registered_rc = VJO_ERR_NO_GAME; /* suspended, backgrounded or disabled */
    worker_main(0, NULL);
    TEST_CHECK(!weights_live && !resident.model && !blocked);
    quitting = 0; registered_rc = 0; quit_after_delay = 0;
    TEST_CHECK(!run("FL10_w8.vocr", &out) && weights_live && out.weights == VJO_GAME_OCR_WEIGHTS_READ);
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !weights_live && !resident.model);
}
static void failed_release_blocks_until_stop_retries(void)
{
    reset(); VjoGameOcrResult out;
    TEST_CHECK(!run("FL10_w8.vocr", &out));
    weights_free_failure = 1; size_delta = 1; /* the refusal must release them, and cannot */
    VjoGameOcrRequest r = request("FL10_w8.vocr");
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_UNAVAILABLE && blocked && weights_live);
    TEST_CHECK(out.cleanup_status && !out.text[0] && !resident.model);
    TEST_CHECK(vjo_game_ocr_worker_stop() == VJO_E_OCR_UNAVAILABLE && weights_live);
    weights_free_failure = 0; size_delta = 0;
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !weights_live && !blocked);
    TEST_CHECK(!vjo_game_ocr_worker_start()); /* a restarted worker loads them again */
    TEST_CHECK(!run("FL10_w8.vocr", &out) && weights_live && out.weights == VJO_GAME_OCR_WEIGHTS_READ);
    TEST_CHECK(!vjo_game_ocr_worker_stop() && !weights_live);
}

TEST_LIST = {
    {"synthetic pins match their generated bytes", synthetic_pins_match_their_bytes},
    {"weights are read and hashed once, then kept for 50 jobs", weights_are_read_once_and_kept},
    {"a changed modification time is read again; a changed size is refused", changed_file_is_read_again},
    {"an unknown modification time reads the weights for every job", unknown_modification_time_reads_every_job},
    {"a missing or corrupt file releases the resident weights", missing_or_corrupt_file_releases_weights},
    {"cancellation keeps verified weights and drops a partial read", cancellation_keeps_verified_and_drops_partial_weights},
    {"a model change releases the old weights before allocating the new", model_change_releases_the_old_weights_first},
    {"the job peak stays within a page of the old single block", peak_is_the_old_single_block_within_a_page},
    {"the preflight counts resident weights once; a refusal releases them", preflight_counts_resident_weights_and_refusal_releases_them},
    {"an inactive game and the worker stop release the weights", inactive_game_and_stop_release_weights},
    {"a failed release blocks the worker until stop retries it", failed_release_blocks_until_stop_retries},
    {NULL, NULL}
};
