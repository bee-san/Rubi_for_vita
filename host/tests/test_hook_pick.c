/* API 31: core/hook_pick.c, the Shell's △ cycle (automatic -> each stream with text -> OCR only ->
 * automatic) and the overlay's source label, as linked into the Shell. */
#include "acutest.h"
#include <string.h>

#include "../../core/hook_pick.h"
#include "../../include/vjo_game_ocr.h"

#define BIT(id) VJO_HOOK_STREAM_BIT(id)

static void test_cycle(void)
{
    /* a site (1) and two CALL callers (9, 12): the IDs need not be contiguous */
    const uint32_t streams = BIT(1) | BIT(9) | BIT(12);
    uint32_t p = VJO_HOOK_PICK_AUTO, seen[8];
    unsigned n = 0;
    do {
        p = vjo_hook_pick_next(p, streams);
        seen[n++] = p;
    } while (p != VJO_HOOK_PICK_AUTO && n < 8);
    TEST_CHECK(n == 5 && seen[0] == 1 && seen[1] == 9 && seen[2] == 12 && seen[3] == VJO_HOOK_PICK_OCR &&
               seen[4] == VJO_HOOK_PICK_AUTO);
    /* every stream ID, the last one included */
    TEST_CHECK(vjo_hook_pick_next(VJO_HOOK_PICK_AUTO, BIT(VJO_HOOK_STREAMS)) == VJO_HOOK_STREAMS);
    TEST_CHECK(vjo_hook_pick_next(VJO_HOOK_STREAMS, BIT(VJO_HOOK_STREAMS)) == VJO_HOOK_PICK_OCR);
    TEST_CHECK(vjo_hook_pick_next(VJO_HOOK_PICK_AUTO, 0xFFFFFFFFu) == 1);
    TEST_CHECK(vjo_hook_pick_next(VJO_HOOK_STREAMS - 1u, 0xFFFFFFFFu) == VJO_HOOK_STREAMS); /* bits past 20 ignored */
    TEST_CHECK(vjo_hook_pick_next(VJO_HOOK_STREAMS, 0xFFFFFFFFu) == VJO_HOOK_PICK_OCR);
}

static void test_cycle_without_text_and_after_changes(void)
{
    /* hooks installed, no stream with text yet: automatic <-> OCR only */
    TEST_CHECK(vjo_hook_pick_next(VJO_HOOK_PICK_AUTO, 0) == VJO_HOOK_PICK_OCR);
    TEST_CHECK(vjo_hook_pick_next(VJO_HOOK_PICK_OCR, 0) == VJO_HOOK_PICK_AUTO);
    /* the pinned stream lost its text (the mask is the last result's): the next one after it */
    TEST_CHECK(vjo_hook_pick_next(5, BIT(2) | BIT(7)) == 7);
    TEST_CHECK(vjo_hook_pick_next(9, BIT(2) | BIT(7)) == VJO_HOOK_PICK_OCR);
    /* an unknown pick goes back to automatic */
    TEST_CHECK(vjo_hook_pick_next(VJO_HOOK_STREAMS + 1u, BIT(1)) == VJO_HOOK_PICK_AUTO);
    TEST_CHECK(vjo_hook_pick_next(0x80u, BIT(1)) == VJO_HOOK_PICK_AUTO);
    /* every pick it returns is one the kernel accepts */
    for (uint32_t p = 0; p < 0x200u; p++) {
        uint32_t next = vjo_hook_pick_next(p, 0xA5A5A5A5u);
        TEST_CHECK_(VJO_HOOK_PICK_OK(next), "%u -> %u", p, next);
    }
}

static const char *label(uint32_t pick, int installed, int result, uint32_t from, uint32_t flags, uint32_t streams)
{
    static char out[VJO_HOOK_LABEL_BYTES];
    size_t n = vjo_hook_label(out, sizeof(out), pick, installed, result, from, flags, streams);
    TEST_CHECK(n == strlen(out));
    return out;
}

static void test_labels(void)
{
    const uint32_t five = BIT(1) | BIT(2) | BIT(9) | BIT(10) | BIT(15);
    /* no hook in this game: no label */
    TEST_CHECK(!strcmp(label(VJO_HOOK_PICK_AUTO, 0, 1, 0, 0, 0), ""));
    TEST_CHECK(!strcmp(label(VJO_HOOK_PICK_AUTO, 0, 0, 0, 0, 0), ""));
    /* automatic */
    TEST_CHECK(!strcmp(label(VJO_HOOK_PICK_AUTO, 1, 1, 0, 0, five), "OCR"));
    TEST_CHECK(!strcmp(label(VJO_HOOK_PICK_AUTO, 1, 1, 2, VJO_HOOK_R_AGREED | VJO_HOOK_R_SITE, five), "Hook 2/5"));
    TEST_CHECK(!strcmp(label(VJO_HOOK_PICK_AUTO, 1, 1, 9, VJO_HOOK_R_LOCKED | VJO_HOOK_R_NO_OCR, five),
                       "Hook 3/5 locked"));
    TEST_CHECK(!strcmp(label(VJO_HOOK_PICK_AUTO, 1, 0, 0, 0, five), "Automatic"));
    /* pinned */
    TEST_CHECK(!strcmp(label(10, 1, 1, 10, VJO_HOOK_R_PINNED | VJO_HOOK_R_NO_OCR, five), "Hook 4/5 pinned"));
    TEST_CHECK(!strcmp(label(10, 1, 0, 0, 0, five), "Hook 4/5 pinned"));    /* △ just pressed */
    TEST_CHECK(!strcmp(label(3, 1, 1, 0, 0, five), "OCR, Hook 3/6 empty")); /* its stream had no text */
    /* OCR only, whatever the result */
    TEST_CHECK(!strcmp(label(VJO_HOOK_PICK_OCR, 1, 1, 0, 0, five), "OCR only"));
    TEST_CHECK(!strcmp(label(VJO_HOOK_PICK_OCR, 1, 0, 0, 0, five), "OCR only"));
    /* a result naming a stream the mask lacks still counts it; a bad text_from is OCR */
    TEST_CHECK(!strcmp(label(VJO_HOOK_PICK_AUTO, 1, 1, 20, 0, 0), "Hook 1/1"));
    TEST_CHECK(!strcmp(label(VJO_HOOK_PICK_AUTO, 1, 1, 21, 0, five), "OCR"));
    TEST_CHECK(!strcmp(label(VJO_HOOK_STREAMS + 1u, 1, 1, 0, 0, five), ""));
}

static void test_label_bounds(void)
{
    char out[VJO_HOOK_LABEL_BYTES], small[6];
    /* the longest: every stream with text, the last pinned and empty */
    size_t n = vjo_hook_label(out, sizeof(out), VJO_HOOK_STREAMS, 1, 1, 0, 0, 0xFFFFFu);
    TEST_CHECK(!strcmp(out, "OCR, Hook 20/20 empty") && n == strlen(out) && n < sizeof(out));
    n = vjo_hook_label(small, sizeof(small), VJO_HOOK_STREAMS, 1, 1, 0, 0, 0xFFFFFu);
    TEST_CHECK(n == sizeof(small) - 1 && !strcmp(small, "OCR, "));
    TEST_CHECK(vjo_hook_label(out, 0, VJO_HOOK_PICK_OCR, 1, 1, 0, 0, 0) == 0);
}

TEST_LIST = {
    {"the cycle: automatic, each stream with text, OCR only", test_cycle},
    {"the cycle with no text, after the streams changed, and unknown picks", test_cycle_without_text_and_after_changes},
    {"the labels", test_labels},
    {"the labels fit their buffer", test_label_bounds},
    {NULL, NULL},
};
