/* API 53: the pinned text spotter's own weights (include/vjo_vocr_sizes.h: vita-vn-ocr's pre-release
 * v0.3.10-textspot8.1), read from VJO_VOCR_MODEL_DIR as the models lane gives it (tools/prepare_ocr.py --backend
 * vocr-tests fetches them; without the directory every check here is skipped, as the other real-model tests are):
 * - the file is the pin: vjo_vocr_spot_load, the worker's own load, takes it (its size, SHA-256 and the runtime's
 *   own checks) in a build with this build's runtime flags (VOCR_TEXTSPOT6_NO_MS: the trunk form);
 * - its block is within the ledger's VJO_BUDGET_GAME_SPOT, its scratch for any region of a 960 x 544 frame within
 *   vjo_vocr_spot_bytes_max();
 * - it finds both lines of the short-line fixture's text box (host/fixtures/short_line: GRAD finds only the first),
 *   read as a job reads a region of the Vita's frame (the blob halves it: VOCR_TEXTSPOT6_SCALE2), as vita-vn-ocr's
 *   twin gives them for these weights (synth/textspot6_twin.spot(..., scale=2) at vita-vn-ocr 4674e54): (0, 11, 774,
 *   43) and (3, 59, 66, 37), both dialogue, and no box on the page arrow at the bottom right. */
#include <stdio.h>
#include <string.h>

#include "acutest.h"
#include "../../core/vocr_ocr.h"
#include "../../include/vjo_ledger_budget.h"
#include "../../include/vjo_vocr_sizes.h"
#include "vocr_textspot6.h"

#ifndef VJO_VOCR_MODEL_DIR
#define VJO_VOCR_MODEL_DIR ""
#endif
#ifndef VJO_SOURCE_DIR
#error "VJO_SOURCE_DIR: the source tree (its host/fixtures)"
#endif

/* ---- the model directory's file, read as the worker reads one (VjoFile) ---- */
static unsigned char file_data[VJO_VOCR_SPOT_MAX_BYTES + 1];
static size_t file_bytes;
static int file_read(void *ctx, uint64_t off, void *dst, size_t n)
{
    (void)ctx;
    if (off > file_bytes || n > file_bytes - off) return -1;
    memcpy(dst, file_data + off, n);
    return 0;
}
static void file_close(void *ctx) { (void)ctx; }
static int file_open(void *ud, const char *path, VjoFile *out)
{
    FILE *f = fopen(path, "rb");
    (void)ud;
    if (!f) return -1;
    file_bytes = fread(file_data, 1, sizeof(file_data), f);
    fclose(f);
    *out = (VjoFile){NULL, (uint64_t)file_bytes, file_read, file_close, 1};
    return 0;
}
static uint64_t now(void *ud)
{
    static uint64_t t;
    (void)ud;
    return t += 7;
}
static VjoPlatform platform = {NULL, file_open, now, NULL, 0};

/* The resident's block: a static of the ledger's budget for it (the worker's game_memblock_alloc gives one block) */
static unsigned char block_mem[VJO_BUDGET_GAME_SPOT] __attribute__((aligned(64)));
static unsigned char *block;
static void *blk_alloc(void *ud, size_t bytes)
{
    (void)ud;
    TEST_CHECK(!block);
    if (bytes > sizeof(block_mem)) return NULL;
    block = block_mem;
    return block;
}
static int blk_release(void *ud)
{
    (void)ud;
    block = NULL;
    return 0;
}

/* The pinned weights, loaded by the worker's own path; 0 when the models lane's directory does not hold them. */
static int load(VjoVocrSpotResident *res)
{
    char path[600];
    FILE *f;
    if (!VJO_VOCR_MODEL_DIR[0]) {
        TEST_MSG("no VJO_VOCR_MODEL_DIR: the release spotter's checks are skipped");
        return 0;
    }
    snprintf(path, sizeof(path), "%s/%s", VJO_VOCR_MODEL_DIR, VJO_VOCR_SPOT_NAME);
    f = fopen(path, "rb");
    TEST_ASSERT_(f != NULL, "%s: the models lane fetches it (tools/prepare_ocr.py --backend vocr-tests)", path);
    fclose(f);
    memset(res, 0, sizeof(*res));
    res->alloc = blk_alloc;
    res->release = blk_release;
    unsigned read = 0;
    int rc = vjo_vocr_spot_load(&platform, VJO_VOCR_MODEL_DIR, res, NULL, &read);
    TEST_CHECK_(rc == VJO_OK && res->verified && read == 1, "load rc %d verified %d read %u", rc, res->verified, read);
    return rc == VJO_OK;
}

static void the_pinned_weights_load_within_their_budgets(void)
{
    VjoVocrSpotResident res;
    if (!load(&res)) return;
    TEST_CHECK(file_bytes == VJO_VOCR_SPOT_BYTES && VJO_VOCR_SPOT_PUBLISHED == 1);
    TEST_CHECK(!strcmp(VJO_VOCR_SPOT_FORM, "trunk"));
    /* the block (the weights and their parsed form), page-rounded, within the ledger's budget for it */
    size_t b = vjo_vocr_spot_block_bytes();
    TEST_CHECK_(b > 0 && (b + 4095u) / 4096u * 4096u <= VJO_BUDGET_GAME_SPOT, "block %zu B", b);
    /* its scratch for a whole 960 x 544 region and for the fixture's 852 x 135, within the most the ledger counts */
    size_t whole = vjo_vocr_spot_bytes(&res, 960, 544, 0, 0), box = vjo_vocr_spot_bytes(&res, 852, 135, 0, 0);
    TEST_CHECK_(whole > 0 && whole <= vjo_vocr_spot_bytes_max() && box > 0 && box <= whole, "scratch %zu / %zu B",
                whole, box);
    TEST_MSG("block %zu B, scratch %zu B (960 x 544) and %zu B (852 x 135)", b, whole, box);
    TEST_CHECK(vjo_vocr_spot_drop(&res) == 0 && !block);
}

static unsigned char grey[852 * 135];
static unsigned char arena[VJO_VOCR_SPOT_ARENA_MAX];
static void it_finds_both_lines_of_the_short_line_text_box(void)
{
    VjoVocrSpotResident res;
    char head[16];
    if (!load(&res)) return;
    FILE *f = fopen(VJO_SOURCE_DIR "/host/fixtures/short_line/textbox.pgm", "rb");
    TEST_ASSERT(f != NULL);
    TEST_ASSERT(fread(head, 1, 15, f) == 15 && !memcmp(head, "P5\n852 135\n255\n", 15));
    TEST_ASSERT(fread(grey, 1, sizeof(grey), f) == sizeof(grey));
    fclose(f);
    vocr_textspot6 m;
    TEST_ASSERT(vocr_textspot6_load(&m, res.data, VJO_VOCR_SPOT_BYTES) == VOCR_LINES_OK);
    TEST_CHECK(m.version == 4 && m.form == VOCR_TEXTSPOT6_FORM_TRUNK && m.half_above == 480);
    VocrLinesImage im = {grey, 852, 135, 852, VOCR_LINES_GREY};
    static VocrTextspot6Set set;
    int rc = vocr_textspot6_find(&m, &im, VOCR_TEXTSPOT6_SCALE2, arena, sizeof(arena), &set);
    TEST_ASSERT_(rc == VOCR_LINES_OK, "rc %d", rc);
    static const unsigned short want[2][4] = {{0, 11, 774, 43}, {3, 59, 66, 37}};
    TEST_CHECK_(set.count == 2, "%u boxes", set.count);
    for (unsigned i = 0; i < set.count && i < 2; i++) {
        const VocrTextspot6Box *x = &set.boxes[i];
        TEST_CHECK_(x->x == want[i][0] && x->y == want[i][1] && x->w == want[i][2] && x->h == want[i][3] &&
                        !x->vertical && x->tag == VOCR_TEXTSPOT6_DIALOGUE,
                    "box %u: %u %u %u %u vertical %u tag %u", i, x->x, x->y, x->w, x->h, x->vertical, x->tag);
    }
    TEST_CHECK(vjo_vocr_spot_drop(&res) == 0);
}

TEST_LIST = {
    {"the pinned weights load within their budgets", the_pinned_weights_load_within_their_budgets},
    {"it finds both lines of the short-line text box", it_finds_both_lines_of_the_short_line_text_box},
    {NULL, NULL},
};
