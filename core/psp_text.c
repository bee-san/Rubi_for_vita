/* Native text for PSP games under Adrenaline, its pure-C half (core/psp_text.h). No libc call: it builds in the
 * game worker (-nostdlib) and on the host alike. */
#include "psp_text.h"

#include "hook_luna.h" /* VJO_RF_* */
#include "hook_text.h" /* VJO_HOOK_ENC_* */
#include "port.h"      /* VJO_DATA_TABLE */

/* The profiles, from the farm's PPSSPP runs (psandvita, hooked-b2/psp/<title>/hook: rubi-hook.json's address,
 * signature and load_base, and every text event's register value in pages.jsonl.zst). Both games are 5pb./KID
 * ADV titles that keep the current page in one static buffer: each farm event's register held the same address,
 * and the page's last event was the page the reviewer approved (MO6 1,963 of 1,965 pages, sorekara 1,848 of
 * 1,851). The data segment keeps them (VJO_DATA_TABLE, core/port.h): the worker's code pages are full. */
static const VjoPspProfile profiles[] VJO_DATA_TABLE(psp_text_profiles) = {
    /* Memories Off 6 ～T-wave～ (PSN NPJH50382, UMD ULJM05467; the farm's EBOOT dump is ULJM05467_EBOOT.BIN,
     * absolute ET_EXEC): 0x08898920, where the script VM's text command reaches with v0 = 0x089DA830, the page
     * it has just copied (paired-psp-npjh50382-20261023, 1,967 events, 1,966 approved pairs). Its words: lhu v1,
     * 14(s1); lhu a0, 12(s1); lui v0, 0x089E (the buffer's high half); sll v1, v1, 16. */
    {{"NPJH50382", "ULJM05467"}, 0x08898920u, {0x9623000Eu, 0x9624000Cu, 0x3C02089Eu, 0x00031C00u}, 0x089DA830u,
     VJO_HOOK_ENC_CP932, VJO_RF_5PB},
    /* Memories Off ～それから～ (PSN NPJH50132, UMD ULJM05350, DISC_VERSION 1.01, ET_EXEC): 0x08942070, the common
     * text writer (the callee of Luna's ULJM05350 site 0x089435F8), entered with a0 = 0x08BA8100, the finished
     * page (paired-psp-npjh50132-20261023, 1,855 events, 1,857 approved pairs). Its words: addiu sp, sp, -336;
     * sw ra, 44(sp); sw fp, 40(sp); sw s7, 36(sp). */
    {{"NPJH50132", "ULJM05350"}, 0x08942070u, {0x27BDFEB0u, 0xAFBF002Cu, 0xAFBE0028u, 0xAFB70024u}, 0x08BA8100u,
     VJO_HOOK_ENC_CP932, VJO_RF_KID},
};
#define PROFILES (sizeof profiles / sizeof profiles[0])

static int is_id(const char *s)
{
    for (unsigned i = 0; i < 9; i++) {
        char c = s[i];
        if (i < 4 ? c < 'A' || c > 'Z' : c < '0' || c > '9') return 0; /* a NUL fails both */
    }
    return 1;
}

int vjo_psp_title_id(const uint8_t raw[VJO_PSP_TITLEID_LEN], char id[10])
{
    id[0] = 0;
    if (!raw || !is_id((const char *)raw) || raw[9]) return 0;
    for (unsigned i = 0; i < 9; i++) id[i] = (char)raw[i];
    id[9] = 0;
    return 1;
}

static int same_id(const char *a, const char *b)
{
    for (unsigned i = 0; i < 9; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

const VjoPspProfile *vjo_psp_text_profile(const char *id)
{
    if (!id || !is_id(id)) return NULL;
    for (unsigned i = 0; i < PROFILES; i++)
        if (same_id(id, profiles[i].id[0]) || same_id(id, profiles[i].id[1])) return &profiles[i];
    return NULL;
}

unsigned vjo_psp_text_poll(const VjoPspTextOps *o, const VjoPspProfile **profile, uint8_t *out, unsigned cap,
                           unsigned *n)
{
    uint8_t raw[VJO_PSP_TITLEID_LEN], code[4u * VJO_PSP_CODE_WORDS], again[VJO_PSP_TEXT_MAX];
    const VjoPspProfile *p;
    char id[10];
    unsigned a, b;
    if (profile) *profile = NULL;
    if (n) *n = 0;
    if (!o || !o->read || !out || cap < 2u) return VJO_PSP_TEXT_OFF;
    if (cap > VJO_PSP_TEXT_MAX) cap = VJO_PSP_TEXT_MAX;
    /* 1. the running game, as Adrenaline's PSP side wrote it (initAdrenalineInfo: sceKernelGetGameInfo's ID) */
    if (o->read(o->ud, VJO_PSP_TITLEID_ADDR, raw, sizeof raw, 0) != sizeof raw) return VJO_PSP_TEXT_NO_VIEW;
    if (!vjo_psp_title_id(raw, id)) return VJO_PSP_TEXT_NO_TITLE;
    p = vjo_psp_text_profile(id);
    if (!p) return VJO_PSP_TEXT_UNLISTED;
    /* 2. the code at the profile's site: the farm's words exactly, or this is another build of the game */
    if (o->read(o->ud, p->site, code, sizeof code, 0) != sizeof code) return VJO_PSP_TEXT_NO_VIEW;
    for (unsigned i = 0; i < VJO_PSP_CODE_WORDS; i++) {
        uint32_t w = (uint32_t)code[4u * i] | (uint32_t)code[4u * i + 1u] << 8 | (uint32_t)code[4u * i + 2u] << 16 |
                     (uint32_t)code[4u * i + 3u] << 24;
        if (w != p->code[i]) return VJO_PSP_TEXT_CODE;
    }
    if (profile) *profile = p;
    /* 3. the page: read twice, each whole (its NUL inside cap bytes) and the same, or not taken this time */
    a = o->read(o->ud, p->buffer, out, cap, 1);
    if (!a || a > cap || out[a - 1u]) return VJO_PSP_TEXT_UNENDED;
    b = o->read(o->ud, p->buffer, again, cap, 1);
    if (b != a) return VJO_PSP_TEXT_UNSTABLE;
    for (unsigned i = 0; i < a; i++)
        if (again[i] != out[i]) return VJO_PSP_TEXT_UNSTABLE;
    if (a - 1u < 2u) return VJO_PSP_TEXT_EMPTY;
    if (n) *n = a - 1u;
    return VJO_PSP_TEXT_OK;
}
