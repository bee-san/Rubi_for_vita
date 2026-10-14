/* Host tests only (VJO_VOCR_MODEL_INFO_TEST, host/tests/test_settings_app.c): a
 * later package's model metadata, with generation VOCR5 pinned beside VOCR3.
 * The names and sizes are made up; no such files exist. */
static const VjoVocrModelInfo infos[] = {
    {"VOCR5_N_w8.vocr", "VOCR5", "N", 3000000u},
    {"VOCR5_S_w8.vocr", "VOCR5", "S", 900000u},
    {VJO_VOCR3_H15_NAME, "VOCR3", "H15", VJO_VOCR_H15_BYTES},
    {VJO_VOCR3_FL10_NAME, "VOCR3", "FL10", VJO_VOCR_FL10_BYTES},
};
