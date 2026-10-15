/* Host tests only (VJO_VOCR_MODEL_INFO_TEST, host/tests/test_settings_app.c): a
 * later package's model metadata, with generation VOCR5's three blobs pinned as
 * its tiers beside VOCR3, as a pin update would. The names, sizes and estimates
 * are made up; no such files exist. */
static const VjoVocrModelInfo infos[] = {
    {"VOCR5_S_w8.vocr", "VOCR5", "S", 900000u, 900u, 1100000u},
    {"VOCR5_N_w8.vocr", "VOCR5", "N", 1500000u, 1000u, 1800000u},
    {"VOCR5_L_w8.vocr", "VOCR5", "L", 2400000u, 2600u, 2900000u},
    {VJO_VOCR3_H15_NAME, "VOCR3", "H15", VJO_VOCR_H15_BYTES, H15_BOX_MS, H15_JOB_BYTES},
    {VJO_VOCR3_FL10_NAME, "VOCR3", "FL10", VJO_VOCR_FL10_BYTES, FL10_BOX_MS, FL10_JOB_BYTES},
};
static const VjoVocrTierInfo tiers[VJO_VOCR_TIERS] = {
    {"small", "Small", "fastest", "VOCR5_S_w8.vocr", VJO_GAME_RESERVE_SMALL},
    {"normal", "Normal", "default, balanced", "VOCR5_N_w8.vocr", VJO_GAME_RESERVE_NORMAL},
    {"large", "Large", "most accurate", "VOCR5_L_w8.vocr", VJO_GAME_RESERVE_LARGE},
};
