/* core/vocr_ocr.c's model and LM tables for test_vocr_vertical.c only: a model of rows (today's
 * finder, #194's) and three of columns (vita-vn-ocr #99's column finder: P0 and P1 with the dark flags
 * left to the job, VOCR_COLUMNS_NO_DARK, as the product's VOCR5-V; and P1 with the finder's own), and one
 * LM, whose sizes, SHA-256 and arenas the test pins at run time (test_pin_model, test_pin_lm) for tiny
 * valid weights blobs and a tiny LM it builds itself, so vertical jobs run real vocr_run and
 * vocr_lm_decode calls where the release files are absent (CI). VJO_TEST_OFF_ONLY: the model of rows
 * alone, as a tree without vertical text builds it (the off part's golden figures were recorded so). */
static VjoVocrModel models[] = {
    {"TINY_w8.vocr", 0, {0}, 0, VOCR_LINES_TEXTBOX_RECOVERY},
#ifndef VJO_TEST_OFF_ONLY
    {"TINYV_w8.vocr", 0, {0}, 0, VJO_VOCR_FINDER_COLUMNS | VOCR_COLUMNS_NO_DARK},
    {"TINYV1_w8.vocr", 0, {0}, 0, VJO_VOCR_FINDER_COLUMNS | VOCR_COLUMNS_VALLEYS | VOCR_COLUMNS_NO_DARK},
    {"TINYVD_w8.vocr", 0, {0}, 0, VJO_VOCR_FINDER_COLUMNS | VOCR_COLUMNS_VALLEYS},
#endif
};
void test_pin_model(unsigned which, uint32_t bytes, const unsigned char sha256[32], uint32_t arena_bytes);
void test_pin_model(unsigned which, uint32_t bytes, const unsigned char sha256[32], uint32_t arena_bytes)
{
    models[which].bytes = bytes;
    memcpy(models[which].sha256, sha256, 32);
    models[which].arena_bytes = arena_bytes;
}
/* every tier is the one tiny LM (its tier number aside) */
#define VJO_VOCR_TEST_LMS 1
static VjoVocrLm lms[VJO_VOCR_LM_TIERS] = {
    {"TINY_lm.vlm", 0, {0}, 3000, 1},
    {"TINY_lm.vlm", 0, {0}, 3000, 2},
    {"TINY_lm.vlm", 0, {0}, 3000, 3},
    {"TINY_lm.vlm", 0, {0}, 3000, 4},
};
void test_pin_lm(uint32_t bytes, const unsigned char sha256[32]);
void test_pin_lm(uint32_t bytes, const unsigned char sha256[32])
{
    for (unsigned i = 0; i < VJO_VOCR_LM_TIERS; i++) {
        lms[i].bytes = bytes;
        memcpy(lms[i].sha256, sha256, 32);
    }
}
