/* core/vocr_ocr.c's model and LM tables for test_vocr_parallel.c only: two models
 * and one LM whose sizes, SHA-256 and arenas the test pins at run time
 * (test_pin_model, test_pin_lm), for tiny valid weights blobs and a tiny LM it
 * builds itself, so the parallel, cached, LM and cascade paths run real
 * vocr_run and vocr_lm_decode calls where the release files are absent (CI). */
static VjoVocrModel models[] = {
    {"TINY_w8.vocr", 0, {0}, 0, VOCR_LINES_GRAD},
    {"TINY2_w8.vocr", 0, {0}, 0, VOCR_LINES_GRAD},
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
