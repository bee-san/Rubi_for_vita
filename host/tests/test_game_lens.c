/* The game worker's Google Lens job (API 21, docs/shell-slim.md): the real
 * game_ocr/main.c and game_ocr/net_game.c with the Vita's memory, rows,
 * SceSysmodule and libnet failure-injected, and a fake Lens server answering
 * over plain HTTP (the host's only way past TLS, core/net.h plain_http) with a
 * recorded reply (host/fixtures/vn_textbox/lens.pb). */
#include "acutest.h"
#include <psp2/host_stubs.h>
#include "../../game_ocr/main.c"
#include "../../game_ocr/net_game.c"
#include "replay.h"

extern int vjo_game_net_plain_http;

#define FW 200u
#define FH 72u
static unsigned char frame[FW * FH]; /* grey: the rows the display hook copies */
static unsigned char strip_mem[64 * 1024] __attribute__((aligned(64)));
static unsigned char lens_mem[1024 * 1024] __attribute__((aligned(64)));
static unsigned char weights_mem[1160 * 1024] __attribute__((aligned(64)));
static int strip_live, lens_live, weights_live2, refuse_strip_user, refuse_lens_user, lens_free_failure;
static unsigned strip_bytes_seen, lens_bytes_seen, lens_types, rows_calls, sig_strips, last_strips;
static int32_t free_now = 90 * 1024 * 1024;
static uint64_t clock_value;
static int cancel_at_rows;

SceUID sceKernelGetProcessId(void) { return 7; }
uint64_t sceKernelGetProcessTimeWide(void) { return clock_value += 100; }
int sceKernelDelayThread(unsigned us) { clock_value += us; return 0; }
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *out)
{
    out->size_user = free_now - (strip_live ? (int)strip_bytes_seen : 0) - (lens_live ? (int)lens_bytes_seen : 0);
    out->size_cdram = 1 << 20; out->size_phycont = 1 << 20;
    return 0;
}
SceUID sceKernelAllocMemBlock(const char *name, int type, unsigned size, void *opts)
{
    TEST_CHECK(!(size & 4095));
    if (!strcmp(name, "VjoGameVocrCapture")) {
        if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW && refuse_strip_user) return (int)0x80024302u;
        TEST_CHECK(!strip_live && size <= sizeof(strip_mem));
        strip_live = 1; strip_bytes_seen = size; return 11;
    }
    if (!strcmp(name, "VjoGameLens")) {
        lens_types |= 1u << (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW ? 0 : 1);
        TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW); /* libnet and TLS work in it: USER_RW only */
        if (refuse_lens_user) return (int)0x80024302u;
        TEST_CHECK(!lens_live && size <= sizeof(lens_mem));
        lens_live = 1; lens_bytes_seen = size; return 14;
    }
    TEST_CHECK_(0, "unexpected block %s", name);
    return -1;
}
int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    if (uid == 11) { *base = strip_mem; return 0; }
    if (uid == 14) { *base = lens_mem; return 0; }
    if (uid == 10) { *base = weights_mem; return 0; }
    return -1;
}
int sceKernelFreeMemBlock(SceUID uid)
{
    if (uid == 11) { TEST_CHECK(strip_live); strip_live = 0; return 0; }
    if (uid == 14) { TEST_CHECK(lens_live); if (lens_free_failure) return -1; lens_live = 0; return 0; }
    if (uid == 10) { weights_live2 = 0; return 0; }
    return -1;
}
SceUID sceKernelCreateThread(const char *n, int (*entry)(SceSize, void *), int p, unsigned stack, unsigned a, int cpu, void *opt)
{ TEST_CHECK(stack == VJO_STACK_GAME); return 20; }
int sceKernelStartThread(SceUID uid, SceSize size, void *args) { return 0; }
int sceKernelWaitThreadEnd(SceUID uid, int *status, void *timeout) { return 0; }
int sceKernelDeleteThread(SceUID uid) { return 0; }
int sceKernelGetProcessTitleId(SceUID pid, char *out, SceSize size) { snprintf(out, size, "PCSG00415"); return 0; }
int sceAppMgrAppParamGetString(int pid, int param, char *out, SceSize cap) { return -1; }
int vjoGetVersion(void) { return VJO_API_VERSION; }
int vjoBootInfo(VjoBootInfo *out) { out->mode = 0; return 0; }
int vjoOcrRegister(void) { return VJO_ERR_NO_GAME; }
int vjoOcrTake(VjoGameOcrRequest *out) { return 1; }
int vjoOcrCancelled(uint32_t seq) { return cancel_at_rows && rows_calls >= (unsigned)cancel_at_rows; }
int vjoOcrComplete(const VjoGameOcrResult *out) { return 0; }
int vjoOcrModelOpen(uint32_t seq, uint32_t which, VjoGameOcrModelInfo *info) { TEST_CHECK_(0, "Lens opens no model"); return -1; }
int vjoOcrModelRead(uint32_t seq, uint32_t offset, uint32_t n, void *out) { return -1; }
int vjoOcrModelClose(uint32_t seq) { return 0; }
SceUID sceIoOpen(const char *path, int flags, unsigned mode) { return -1; }
int sceDisplayGetFrameBuf(SceDisplayFrameBuf *fb, int sync) { return -1; }
int sceKernelGetMemBlockInfoByAddr(void *base, SceKernelMemBlockInfo *info) { return -1; }
/* The display hook's rows: the frame's grey rows into the worker's strip. */
int vjoOcrCaptureRows(const VjoGameOcrRows *req, void *dst, VjoGameOcrRowsInfo *info)
{
    TEST_CHECK(req->size == sizeof(*req) && req->seq == 12 && req->n && req->n <= VJO_GAME_OCR_ROWS_MAX);
    TEST_CHECK(req->y + req->n <= FH && req->dst_stride >= FW && (unsigned char *)dst >= strip_mem);
    TEST_CHECK(req->mode == VJO_GAME_OCR_SRC_HOOK);
    rows_calls++;
    if (req->flags & VJO_ROWS_SIG) sig_strips++;
    if (req->flags & VJO_ROWS_LAST) { TEST_CHECK(req->flags & VJO_ROWS_SIG); last_strips++; }
    info->fb_width = FW; info->fb_height = FH; info->fb_pitch = FW; info->fb_format = 0; info->wait_us = 16000;
    info->armed = 1;
    for (unsigned r = 0; r < req->n; r++) memcpy((unsigned char *)dst + r * req->dst_stride, frame + (req->y + r) * FW, FW);
    return (int)req->n;
}

/* ---- SceSysmodule and libnet ---- */
static int net_loaded, load_calls, unload_calls, libnet_up, init_calls, term_calls, term_error, game_libnet;
static int connects, sockets_open, refuse_connect;
static char request_bytes[256 * 1024];
static size_t request_len;
static char reply[300 * 1024];
static size_t reply_len, reply_pos;
int sceSysmoduleIsLoaded(SceSysmoduleModuleId id) { return net_loaded ? SCE_SYSMODULE_LOADED : SCE_SYSMODULE_ERROR_UNLOADED; }
static int load_error;
int sceSysmoduleLoadModule(SceSysmoduleModuleId id) { load_calls++; if (load_error) return load_error; net_loaded = 1; return 0; }
int sceSysmoduleUnloadModule(SceSysmoduleModuleId id) { unload_calls++; return 0; }
int sceNetInit(SceNetInitParam *p)
{
    init_calls++;
    if (libnet_up) return (int)SCE_NET_ERROR_EBUSY;
    TEST_CHECK((unsigned char *)p->memory >= lens_mem && (unsigned char *)p->memory + p->size <= lens_mem + lens_bytes_seen);
    TEST_CHECK(p->size == (int)VJO_GAME_NET_POOL);
    libnet_up = 1;
    return 0;
}
int sceNetTerm(void) { term_calls++; if (term_error) return term_error; libnet_up = 0; return 0; }
int sceNetSocket(const char *name, int domain, int type, int protocol) { TEST_CHECK(libnet_up); sockets_open++; return 50; }
int sceNetSocketClose(int s) { sockets_open--; return 0; }
int sceNetSetsockopt(int s, int level, int optname, const void *optval, unsigned int optlen) { return 0; }
int sceNetConnect(int s, const SceNetSockaddr *name, unsigned int namelen)
{
    if (refuse_connect) return (int)0x8041013Du; /* ECONNREFUSED */
    /* non-blocking: in progress, then connected */
    return ++connects & 1 ? (int)SCE_NET_ERROR_EINPROGRESS : (int)SCE_NET_ERROR_EISCONN;
}
int sceNetSend(int s, const void *msg, unsigned int len, int flags)
{
    TEST_CHECK(request_len + len <= sizeof(request_bytes));
    memcpy(request_bytes + request_len, msg, len);
    request_len += len;
    return (int)len;
}
int sceNetRecv(int s, void *buf, unsigned int len, int flags)
{
    size_t n = reply_len - reply_pos;
    if (n > len) n = len;
    if (n > 1000) n = 1000;
    memcpy(buf, reply + reply_pos, n);
    reply_pos += n;
    return (int)n;
}
int sceNetInetPton(int af, const char *src, void *dst) { return 0; }
unsigned short int sceNetHtons(unsigned short int v) { return v; }
static int resolves;
int sceNetResolverCreate(const char *name, SceNetResolverParam *param, int flags) { return 3; }
int sceNetResolverStartNtoa(int rid, const char *host, SceNetInAddr *addr, int timeout, int retry, int flags)
{ resolves++; TEST_CHECK(!strcmp(host, VJO_LENS_HOST)); addr->s_addr = 0x0A000001u; return 0; }
int sceNetResolverDestroy(int rid) { return 0; }
int sceKernelGetRandomNumber(void *out, SceSize size) { memset(out, 0x33, size); return 0; }
int sceRtcGetCurrentTick(SceRtcTick *t) { t->tick = (62135596800ull + 1760000000ull) * 1000000ull; return 0; }

static unsigned char fixture_mem[64 * 1024];
static const char *pb;
static size_t pb_len;
static void set_reply(int status, const char *body, size_t len)
{
    int head = snprintf(reply, sizeof(reply), "HTTP/1.1 %d X\r\nContent-Length: %zu\r\n\r\n", status, len);
    memcpy(reply + head, body, len);
    reply_len = (size_t)head + len;
    reply_pos = 0;
}

static const char *lens_text(void)
{
    static unsigned char mem[128 * 1024];
    static char *t;
    VjoArena a;
    VjoLensResult r;
    if (t) return t;
    vjo_arena_init(&a, mem, sizeof(mem));
    TEST_ASSERT(!vjo_lens_parse_response(&a, (const uint8_t *)pb, pb_len, &r));
    t = vjo_lens_text(&a, &r);
    return t;
}

static VjoGameOcrRequest setup(void)
{
    VjoGameOcrRequest r;
    static VjoArena fa;
    if (!pb) {
        vjo_arena_init(&fa, fixture_mem, sizeof(fixture_mem));
        pb = vjo_read_file(&fa, VJO_FIXTURES "/vn_textbox/lens.pb", &pb_len);
        TEST_ASSERT(pb != NULL);
    }
    worker = workspace_uid = weights_uid = capture_uid = line_uid = lm_uid = cascade_uid = lens_uid = -1;
    lens_bytes_held = 0; lens_pool_held = 0;
    hook_owns_capture = blocked = model_owned = 0;
    resident.model = NULL; resident.data = NULL; resident.capacity = 0;
    lm_res.lm = NULL; lm_res.data = NULL; cascade_res.model = NULL; cascade_res.data = NULL;
    quitting = active_seq = 0;
    strip_live = lens_live = refuse_strip_user = refuse_lens_user = lens_free_failure = 0;
    strip_bytes_seen = lens_bytes_seen = lens_types = rows_calls = sig_strips = last_strips = 0;
    free_now = 90 * 1024 * 1024; cancel_at_rows = 0;
    net_loaded = load_calls = unload_calls = libnet_up = init_calls = term_calls = term_error = game_libnet = 0;
    connects = sockets_open = refuse_connect = resolves = load_error = 0;
    request_len = 0;
    set_reply(200, pb, pb_len);
    vjo_game_net_plain_http = 1;
    for (unsigned y = 0; y < FH; y++)
        for (unsigned x = 0; x < FW; x++)
            frame[y * FW + x] = (unsigned char)(((y / 6) % 2 && (x / 5) % 3) ? 30 : 210);
    memset(&r, 0, sizeof(r));
    r.size = sizeof(r); r.seq = 12; r.capture_seq = 5;
    r.width = FW; r.height = FH; r.fb_width = FW; r.fb_height = FH;
    snprintf(r.model, sizeof(r.model), "%s", VJO_GAME_OCR_LENS);
    snprintf(r.model_dir, sizeof(r.model_dir), "ux0:data/VitaJPOverlay/ocr");
    if (!g_game_ledger.defs) ledger_init();
    return r;
}

static int has_step(const VjoGameOcrResult *out, unsigned id, const VjoGameOcrStep **found)
{
    for (uint32_t i = 0; i < out->n_steps; i++)
        if (out->step[i].id == id) { if (found) *found = &out->step[i]; return 1; }
    return 0;
}

static VjoGameOcrResult out;

static void test_lens_job_through_the_worker(void)
{
    VjoGameOcrRequest r = setup();
    const VjoGameOcrStep *ls = NULL, *ns = NULL;
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == VJO_OK);
    TEST_CHECK(out.rc == VJO_OK && out.cleanup_status == 0 && !strcmp(out.text, lens_text()));
    /* the request: Lens's path, from the game's network, which the worker started and ended */
    TEST_CHECK(!strncmp(request_bytes, "POST " VJO_LENS_PATH " HTTP/1.1\r\n", strlen("POST " VJO_LENS_PATH " HTTP/1.1\r\n")));
    TEST_CHECK(resolves == 1 && connects == 2 && sockets_open == 0);
    TEST_CHECK(load_calls == 1 && unload_calls == 0 && init_calls == 1 && term_calls == 1 && !libnet_up);
    /* the region: an encoding pass (its strips the job's first pass, the last LAST) and a comparing pass */
    TEST_CHECK(sig_strips == 2 && last_strips == 1 && rows_calls == 4);
    TEST_CHECK(out.source == VJO_GAME_OCR_SRC_HOOK && out.captures == 4 && out.strip_rows == 64 && !out.tears);
    /* the steps: the check, both blocks USER, the pass, the network (the worker's) and the request */
    TEST_CHECK(has_step(&out, VJO_GSTEP_CHECK, NULL) && has_step(&out, VJO_GSTEP_PASS, NULL) && has_step(&out, VJO_GSTEP_SIG, NULL));
    TEST_ASSERT(has_step(&out, VJO_GSTEP_NET, &ns) && has_step(&out, VJO_GSTEP_LENS, &ls));
    TEST_CHECK(ns->kind == VJO_GNET_OURS && ns->arg == VJO_GNET_OURS && ns->rc == 0 && ns->sum == 0);
    /* a 2xx reply sets no error status; the request's time is in us */
    TEST_CHECK(ls->kind == 1 && ls->arg == 0 && ls->rc == 0 && ls->bytes > 0 && ls->sum == 0 && ls->us > 1);
    TEST_CHECK(lens_types == 1 && out.capture_memory == VJO_GAME_OCR_MEM_USER && out.workspace_bytes == lens_bytes_seen);
    TEST_CHECK(out.free_user == 90 * 1024 * 1024 && out.reserve_bytes == VJO_GAME_RESERVE);
    TEST_CHECK(out.need_workspace == strip_bytes_seen + lens_bytes_seen);
    /* every block is back; the ledger agrees */
    TEST_CHECK(!strip_live && !lens_live && lens_uid < 0 && capture_uid < 0);
    TEST_CHECK(vjo_ledger_current(&g_game_ledger, GL_LENS) == 0 && vjo_ledger_peak(&g_game_ledger, GL_LENS) == lens_bytes_seen);
    TEST_CHECK(lens_bytes_seen <= VJO_BUDGET_GAME_LENS && strip_bytes_seen <= VJO_BUDGET_GAME_CAPTURE);
    /* the network lines follow the text's NUL, for the Shell's log: no text in them */
    const char *net = vjo_text_raw(out.text, sizeof(out.text));
    TEST_CHECK(strstr(net, "Lens image=200x72 jpeg=") != NULL);
    TEST_CHECK(strstr(net, VJO_LENS_HOST ":443 " VJO_LENS_PATH " -> rc=0 status=200") != NULL);
    TEST_CHECK(!out.text[sizeof(out.text) - 1]);
}

static void test_lens_uses_the_games_network(void)
{
    VjoGameOcrRequest r = setup();
    const VjoGameOcrStep *ns = NULL;
    net_loaded = libnet_up = 1; /* the game's own network is up */
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == VJO_OK);
    TEST_ASSERT(has_step(&out, VJO_GSTEP_NET, &ns));
    TEST_CHECK(ns->kind == VJO_GNET_GAME && ns->arg == VJO_GNET_GAME && (unsigned)ns->rc == SCE_NET_ERROR_EBUSY);
    TEST_CHECK(load_calls == 0 && term_calls == 0 && libnet_up && unload_calls == 0); /* never ended or unloaded */
}

static void test_lens_drops_the_local_engines_memory(void)
{
    VjoGameOcrRequest r = setup();
    /* weights a local job left resident: useless to Lens, given back to the game first */
    weights_uid = 10; weights_bytes_held = 1159168; weights_live2 = 1;
    resident.model = vjo_vocr_model("VOCR3_H15_w8.vocr"); resident.data = weights_mem; resident.capacity = 1159168;
    resident.release = weights_release;
    vjo_ledger_add(&g_game_ledger, GL_WEIGHTS, 1159168);
    TEST_ASSERT(vjo_game_ocr_run_request(&r, &out) == VJO_OK);
    TEST_CHECK(!weights_live2 && weights_uid < 0 && !resident.model && out.resident_bytes == 0);
}

static void test_lens_memory_and_network_failures(void)
{
    VjoGameOcrRequest r;
    const VjoGameOcrStep *ns = NULL, *ls = NULL;
    /* too little free memory: refused before anything is allocated */
    r = setup();
    free_now = 4 * 1024 * 1024 + 100000;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM);
    TEST_CHECK(!strip_live && !lens_live && !rows_calls && !init_calls && out.reserve_bytes == VJO_GAME_RESERVE);
    TEST_CHECK(out.need_workspace > 0 && !out.text[0]);
    /* USER_RW refuses the block: no other memory is tried for it */
    r = setup();
    refuse_lens_user = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OOM && lens_types == 1 && !strip_live && !init_calls);
    /* the network module cannot be loaded: the job fails, its line says why */
    r = setup();
    load_error = (int)0x805A10FFu;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_NET);
    TEST_ASSERT(has_step(&out, VJO_GSTEP_NET, &ns));
    TEST_CHECK(ns->kind == VJO_GNET_FAILED && ns->rc == (int32_t)0x805A10FFu && !rows_calls);
    TEST_CHECK(!out.text[0] && strstr(vjo_text_raw(out.text, sizeof(out.text)), "loading SCE_SYSMODULE_NET failed 0x805A10FF"));
    TEST_CHECK(!strip_live && !lens_live && out.cleanup_status == 0);
    /* the connect is refused: libnet still ends, the failed request's line is kept */
    r = setup();
    refuse_connect = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_NET);
    TEST_CHECK(term_calls == 1 && !libnet_up && sockets_open == 0 && !lens_live);
    TEST_CHECK(!out.text[0] && strstr(vjo_text_raw(out.text, sizeof(out.text)), "net: connect lensfrontend-pa.googleapis.com failed 0x8041013D"));
    /* a server error: its status in the LENS step */
    r = setup();
    set_reply(503, "busy", 4);
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_STATUS);
    TEST_ASSERT(has_step(&out, VJO_GSTEP_LENS, &ls));
    TEST_CHECK((ls->sum >> 16) == 503 && ls->rc == VJO_E_STATUS && !out.text[0]);
    /* cancelled while the region is read: nothing is sent, libnet ends */
    r = setup();
    cancel_at_rows = 1;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_CANCELLED);
    TEST_CHECK(!request_len && term_calls == 1 && !lens_live && !strip_live);
}

static void test_lens_keeps_a_pool_libnet_may_use(void)
{
    VjoGameOcrRequest r = setup();
    term_error = (int)0x80410101u;
    /* sceNetTerm failed: libnet may still use the pool in the block, which stays; the worker stops */
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_UNAVAILABLE);
    TEST_CHECK(lens_live && lens_uid >= 0 && out.cleanup_status != 0 && blocked);
    TEST_CHECK(strstr(vjo_text_raw(out.text, sizeof(out.text)), "sceNetTerm failed 0x80410101: its pool is kept") != NULL);
    /* no later job starts on it */
    unsigned rows_before = rows_calls;
    TEST_CHECK(vjo_game_ocr_run_request(&r, &out) == VJO_E_OCR_UNAVAILABLE && rows_calls == rows_before && lens_live);
}

TEST_LIST = {
    {"lens_job_through_the_worker", test_lens_job_through_the_worker},
    {"lens_uses_the_games_network", test_lens_uses_the_games_network},
    {"lens_drops_the_local_engines_memory", test_lens_drops_the_local_engines_memory},
    {"lens_memory_and_network_failures", test_lens_memory_and_network_failures},
    {"lens_keeps_a_pool_libnet_may_use", test_lens_keeps_a_pool_libnet_may_use},
    {NULL, NULL}};
