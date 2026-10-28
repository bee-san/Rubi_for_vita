/* Native text for PSP games under Adrenaline, read from the Vita side (docs/native-hooks.md, "PSP text
 * (Adrenaline)"): the pure-C half, host-tested (host/tests/test_psp_text.c). game_ocr/hooks.c gives it one read
 * op in the PSP emulator's process and calls vjo_psp_text_poll at job time, in the worker's thread only.
 *
 * Nothing is hooked or patched in the PSP game and no PSP plugin is added. A PSP game's MIPS code runs on the
 * Vita's PSP processor, where a Vita plugin cannot hook it (reports/hooks-psp.md section 1); what the worker can
 * do is read PSP RAM, which Adrenaline maps into the emulator's process at Vita 0x68000000 + (PSP address &
 * 0x0FFFFFFF) (TheOfficialFloW/Adrenaline 7d382b7 adrenaline_compat.h CONVERT_ADDRESS; Adrenaline's own menu reads
 * SceAdrenaline and the native frame buffer there directly, user/menu.c:478-479 and user/states.c:103-111). So a
 * profile names a game that keeps its dialogue page in one buffer at a fixed PSP address, as the farm's PPSSPP
 * runs found it (hooked-b2/psp/<title>/hook): the worker reads that buffer, read-only, only while the running
 * game's ID and the code at the farm's hook site are the profile's, and the text then has to agree with OCR
 * before it is shown, as any regular hook's (core/hook_select.h).
 *
 * Each profile's game is an ET_EXEC program (absolute addresses: hooked-b2's load_base), so its code sits at
 * the same address in PPSSPP and on a PSP, and the farm's site bytes (from the decrypted EBOOT) are what must be
 * there. A relocatable program (Sharin no Kuni, Hayarigami) would need its load address found first: not here.
 * Seen in PPSSPP only; nothing of this has run on a console. */
#ifndef VJO_PSP_TEXT_H
#define VJO_PSP_TEXT_H

#include <stdint.h>

#define VJO_PSP_TEXT_MAX 512u                  /* a page's bytes read at most, its NUL included (= VJO_HOOK_RAW_BYTES) */
#define VJO_PSP_ADRENALINE_ADDR 0xABCDE000u    /* SceAdrenaline, the struct Adrenaline's PSP side fills (ADRENALINE_ADDRESS) */
#define VJO_PSP_TITLEID_ADDR (VJO_PSP_ADRENALINE_ADDR + 0x98u) /* its titleid[12] (adrenaline_compat.h, both lineages) */
#define VJO_PSP_TITLEID_LEN 12u
#define VJO_PSP_CODE_WORDS 4u                  /* the farm's site bytes: 16 */

/* Adrenaline's view: a PSP address's Vita address in the emulator's process (CONVERT_ADDRESS). */
static inline uint32_t vjo_psp_view(uint32_t psp_addr)
{
    return 0x68000000u + (psp_addr & 0x0FFFFFFFu);
}

typedef struct {
    char id[2][10];      /* the game's IDs, its PSN release's and its UMD's: SceAdrenaline.titleid holds one */
    uint32_t site;       /* PSP address of the farm's hook site */
    uint32_t code[VJO_PSP_CODE_WORDS]; /* the instruction words there, as loaded (the farm's site bytes) */
    uint32_t buffer;     /* PSP address of the page buffer the site's register held in every farm event */
    uint8_t encoding;    /* VJO_HOOK_ENC_* (core/hook_text.h) of the buffer's NUL-terminated text */
    uint8_t filter;      /* VJO_RF_* (core/hook_luna.h): the engine's codes the game does not draw */
} VjoPspProfile;

/* The one read the poll makes (game_ocr/hooks.c: Adrenaline's view, through read_game's mapping check): up to max
 * bytes at PSP address psp_addr into dst, never past the end of the readable mapping that holds them. unit 1: it
 * stops after the first NUL (copied, counted); 0: exactly max bytes or none. Returns the bytes copied. */
typedef struct {
    void *ud;
    unsigned (*read)(void *ud, uint32_t psp_addr, void *dst, unsigned max, unsigned unit);
} VjoPspTextOps;

/* vjo_psp_text_poll's answers, in the order it checks */
enum {
    VJO_PSP_TEXT_OFF,       /* no ops or no room: nothing read */
    VJO_PSP_TEXT_NO_VIEW,   /* SceAdrenaline's title ID (or the site) is not in a readable mapping */
    VJO_PSP_TEXT_NO_TITLE,  /* the title ID is not one (empty in Adrenaline's XMB) */
    VJO_PSP_TEXT_UNLISTED,  /* no profile has that ID */
    VJO_PSP_TEXT_CODE,      /* the code at the profile's site is not the farm's: another revision, not read */
    VJO_PSP_TEXT_UNENDED,   /* the buffer has no NUL in its first VJO_PSP_TEXT_MAX bytes (or ran out of mapping) */
    VJO_PSP_TEXT_UNSTABLE,  /* two reads of the buffer differ (the game was writing it) */
    VJO_PSP_TEXT_EMPTY,     /* fewer than 2 bytes of text */
    VJO_PSP_TEXT_OK,        /* *n bytes of text in out */
    VJO_PSP_TEXT_STATES
};

/* SceAdrenaline.titleid's 12 raw bytes -> the game's ID: 1 when they start with 4 capital letters and 5 digits
 * followed by a NUL ("ULJM05467"), id then those 9 characters and a NUL; else 0 and id "". */
int vjo_psp_title_id(const uint8_t raw[VJO_PSP_TITLEID_LEN], char id[10]);

/* The profile of a 9-character game ID (either of its IDs), or NULL. */
const VjoPspProfile *vjo_psp_text_profile(const char *id);

/* One poll: SceAdrenaline's title ID, its profile, the code at the profile's site, then its buffer read twice
 * (both reads whole, NUL-terminated and the same). *profile: the profile whose code matched (from
 * VJO_PSP_TEXT_UNENDED on), else NULL; on VJO_PSP_TEXT_OK, *n bytes of text (without the NUL) in out (cap bytes,
 * at most VJO_PSP_TEXT_MAX of them used). Reads only through o->read; writes nothing anywhere else. */
unsigned vjo_psp_text_poll(const VjoPspTextOps *o, const VjoPspProfile **profile, uint8_t *out, unsigned cap,
                           unsigned *n);

#endif
