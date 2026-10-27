/* Where a native text hook may be placed (core/hook_place.h, docs/native-hooks.md). Pure C. */
#include "hook_place.h"

#include <string.h>

#include "hook_luna.h" /* VJO_RF_*: a verified entry's filter */
#include "hook_text.h"
#include "port.h"      /* VJO_DATA_TABLE: the verified entries' table */

const char *vjo_place_reason(int reason)
{
    static const char *const names[VJO_PLACE_COUNT] = {
        "ok", "range", "align", "entry", "window", "signature", "unported", "version", "full", "tai"};
    return reason >= 0 && reason < VJO_PLACE_COUNT ? names[reason] : "?";
}

static inline uint16_t hw_at(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t word_at(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* A Thumb halfword that starts a 32-bit instruction (0b11101, 0b11110, 0b11111). */
static inline int thumb32(uint16_t hw) { return hw >= 0xE800u; }

/* Thumb PUSH {..., lr}, or PUSH.W {..., lr} without SP or PC. */
static int thumb_entry(uint16_t hw1, uint16_t hw2)
{
    if ((hw1 & 0xFF00u) == 0xB500u) return 1;
    return hw1 == 0xE92Du && (hw2 & 0x4000u) && !(hw2 & 0xA000u);
}

/* ARM STMFD sp!, {..., lr} (PUSH), condition AL, without SP or PC. */
static int arm_entry(uint32_t w)
{
    return (w & 0xFFFF4000u) == 0xE92D4000u && !(w & 0xA000u);
}

/* The allow-list (the conductor's SCOPE, 2026-10-20): PUSH / PUSH.W, ADD / SUB of SP or R7 from SP
 * with an immediate, MOV / MOVS between registers other than PC, MOVW / MOVT, STR / STRD / LDR
 * (word) with an SP or register base and an immediate offset, VPUSH. Nothing on it reads or writes
 * the PC or branches. Every accepted and refused form is assembled by the VitaSDK assembler into
 * host/tests/fixtures/hook_place_*.inc (tools/gen_hook_place_fixtures.sh): the host test decides. */

/* Thumb, 16-bit. */
static int thumb16_ok(uint16_t hw)
{
    if ((hw & 0xFE00u) == 0xB400u) return 1;  /* PUSH {list}, with or without LR */
    if ((hw & 0xFF00u) == 0xB000u) return 1;  /* ADD SP, SP, #imm7 / SUB SP, SP, #imm7 */
    if ((hw & 0xFF00u) == 0xAF00u) return 1;  /* ADD R7, SP, #imm8 (0xA000-0xA7FF, ADR, is refused) */
    if ((hw & 0xFFC0u) == 0x0000u) return 1;  /* MOVS Rd, Rm (LSLS #0), low registers */
    if ((hw & 0xF000u) == 0x6000u) return 1;  /* STR / LDR Rt, [Rn, #imm5], low registers */
    if ((hw & 0xF000u) == 0x9000u) return 1;  /* STR / LDR Rt, [SP, #imm8] */
    if ((hw & 0xFF00u) == 0x4600u) {          /* MOV Rd, Rm, any registers but PC */
        unsigned rd = (hw & 7u) | ((hw >> 4) & 8u), rm = (hw >> 3) & 15u;
        return rd != 15u && rm != 15u;
    }
    return 0;
}

/* Thumb, 32-bit. */
static int thumb32_ok(uint16_t hw1, uint16_t hw2)
{
    unsigned rn = hw1 & 15u, rt = hw2 >> 12, rd = (hw2 >> 8) & 15u;
    if (hw1 == 0xE92Du) return !(hw2 & 0xA000u);                 /* PUSH.W {list}, no SP or PC */
    if ((hw1 == 0xED2Du || hw1 == 0xED6Du) && (hw2 & 0x0E00u) == 0x0A00u) return 1; /* VPUSH */
    if (!(hw2 & 0x8000u)) {
        unsigned dp = hw1 & 0xFBE0u;
        /* ADD.W / SUB.W SP or R7, SP, #const */
        if ((dp == 0xF100u || dp == 0xF1A0u) && rn == 13u) return rd == 13u || rd == 7u;
        /* ADDW / SUBW SP or R7, SP, #imm12 (Rn PC, ADR, is refused: Rn must be SP) */
        if ((hw1 & 0xFBFFu) == 0xF20Du || (hw1 & 0xFBFFu) == 0xF2ADu) return rd == 13u || rd == 7u;
        /* MOVW / MOVT Rd, #imm16 */
        if ((hw1 & 0xFBF0u) == 0xF240u || (hw1 & 0xFBF0u) == 0xF2C0u) return rd != 15u && rd != 13u;
        /* MOV.W Rd, Rm, no shift */
        if ((hw1 & 0xFFEFu) == 0xEA4Fu && (hw2 & 0x70F0u) == 0) return rd != 15u && (hw2 & 15u) != 15u;
    }
    /* STR.W / LDR.W Rt, [Rn, #imm12] */
    if ((hw1 & 0xFFE0u) == 0xF8C0u) return rn != 15u && rt != 15u;
    /* STR / LDR Rt, [Rn, #+/-imm8] with P, U, W (STR Rt, [SP, #-4]! is a one-register push); P or W set:
     * P = W = 0 is UNDEFINED (audit-hooks N1) */
    if ((hw1 & 0xFFE0u) == 0xF840u && (hw2 & 0x0800u) && (hw2 & 0x0500u)) return rn != 15u && rt != 15u;
    /* STRD Rt, Rt2, [Rn, #+/-imm8] (P or W set: else STREX, TBB, TBH) */
    if ((hw1 & 0xFE50u) == 0xE840u && (hw1 & 0x0120u)) return rn != 15u && rt != 15u && rd != 15u;
    return 0;
}

/* ARM (condition AL only). */
static int arm_ok(uint32_t w)
{
    unsigned rn = (w >> 16) & 15u, rd = (w >> 12) & 15u;
    if ((w & 0xFFFF0000u) == 0xE92D0000u) return !(w & 0xA000u);          /* STMFD sp!, no SP or PC */
    if ((w & 0xFFEF0000u) == 0xE28D0000u || (w & 0xFFEF0000u) == 0xE24D0000u)
        return rd == 13u || rd == 7u;                                      /* ADD / SUB SP or R7, SP, #imm */
    if ((w & 0xFFEF0FF0u) == 0xE1A00000u) return rd != 15u && (w & 15u) != 15u; /* MOV Rd, Rm */
    if ((w & 0xFFF00000u) == 0xE3000000u || (w & 0xFFF00000u) == 0xE3400000u)
        return rd != 15u && rd != 13u;                                     /* MOVW / MOVT */
    if ((w >> 25) == 0x72u && !(w & 0x00400000u)) return rn != 15u && rd != 15u; /* STR / LDR imm12, word */
    if ((w & 0xFE5000F0u) == 0xE04000F0u) return rn != 15u && !(rd & 1u) && rd != 14u; /* STRD imm */
    if ((w & 0xFFBF0E00u) == 0xED2D0A00u) return 1;                        /* VPUSH */
    return 0;
}

int vjo_place_isa(const uint8_t *p, unsigned avail, uint32_t addr)
{
    if (avail < 4) return -1;
    if (addr & 3u) return thumb_entry(hw_at(p), hw_at(p + 2)) ? 1 : -1; /* the Thumb bit, or 2 mod 4 */
    if (thumb_entry(hw_at(p), hw_at(p + 2))) return 1;
    if (arm_entry(word_at(p))) return 0;
    return -1;
}

int vjo_place_check(const uint8_t *code, uint32_t code_size, uint32_t seg_base, uint32_t off, int thumb)
{
    const uint8_t *p;
    unsigned patch, pos;
    if (off >= code_size || code_size - off < VJO_PLACE_SAVE_BYTES) return VJO_PLACE_RANGE;
    if (off & (thumb ? 1u : 3u) || (seg_base & 3u)) return VJO_PLACE_ALIGN;
    p = code + off;
    patch = vjo_place_patch_bytes(seg_base + off, thumb);
    if (thumb) {
        uint16_t hw = hw_at(p);
        if (!thumb_entry(hw, hw_at(p + 2))) return VJO_PLACE_ENTRY;
        /* Every instruction that starts in the patch; the last may end past it, never past the
         * 16 saved bytes (it starts by byte 10 and is at most 4 long). */
        for (pos = thumb32(hw) ? 4u : 2u; pos < patch; ) {
            hw = hw_at(p + pos);
            if (thumb32(hw)) {
                if (!thumb32_ok(hw, hw_at(p + pos + 2))) return VJO_PLACE_WINDOW;
                pos += 4;
            } else {
                if (!thumb16_ok(hw)) return VJO_PLACE_WINDOW;
                pos += 2;
            }
        }
        return VJO_PLACE_OK;
    }
    if (!arm_entry(word_at(p))) return VJO_PLACE_ENTRY;
    for (pos = 4; pos < patch; pos += 4)
        if (!arm_ok(word_at(p + pos))) return VJO_PLACE_WINDOW;
    return VJO_PLACE_OK;
}

/* Rubi's own verified sites. CLANNAD (PCSG00415): the function that receives each line in r6 as
 * CP932, at segment 0 + 0x2CA70 (Thumb: STRH.W r2, [r11, #0x32C]; ADDS.W r0, r4, #0x34C, no PC
 * use). From the PR #16 profile (profiles/PCSG00415.vjhook, removed in c35ebf9); proven in Vita3K
 * only (vita-vn-ocr notes 22), not yet on a console. Its line holds the script's name variables, ＊Ａ
 * and ＊Ｂ, where the game draws the protagonist's names: RF_PCSG00415 writes them (a console's
 * history.csv, Rubi 0.9.8: 16 of 1,347 hook lines showed *A or *A*B).
 * アマツツミ (PCSG01198, AMATSUTSUMI_PSVITA, APP_VER 01.00): Luna's site 0x800785C8 (r2, CP932, its
 * filter: Luna's entry at the same address post-processes the text), which the placement check refuses
 * as no entry: it is inside a function (0x81070E3A's loop, 0x810745C8 in the game). Its 8 patched bytes
 * are ADD.W lr, r9, r7, LSL #2; ADD.W lr, lr, #0x1620 (no PC use), and no branch in segment 0 targets
 * them (the loop's BLT goes to +0x745D0, past them): checked on the decrypted eboot, 2026-10-23. Proven on a
 * console (PCH-2000, 3.65, 2026-10-23): hooked ("1 table sites"), its stream held text, it answered the jobs
 * (core/hook_select.h vjo_hook_select_no_memory: "game vocr source: hook 1, flags 0x98", rc 0), the overlay
 * showed the game's line (checked by eye against the screen), and the game did not crash.
 * The table is kept in the worker's data segment (VJO_DATA_TABLE, core/port.h), whose last page has room:
 * in its code segment the farm's entries took the debug build's worker past its pages
 * (include/vjo_ledger_budget.h; docs/memory-ledger.md, "Tables in the data segment"). Its title strings stay
 * read-only. */
static const VjoPlaceVerified verified[] VJO_DATA_TABLE(place_verified) = {
    {"PCSG00415", 0xC5187D60u, 0x2CA70u, 1, 6, VJO_HOOK_ENC_CP932, 8, 0,
     {0xAB, 0xF8, 0x2C, 0x23, 0x14, 0xF5, 0x53, 0x70}, VJO_RF_PCSG00415, 0, 0},
    {"PCSG01198", 0x8AA413F2u, 0x745C8u, 1, 2, VJO_HOOK_ENC_CP932, 16, 0,
     {0x09, 0xEB, 0x87, 0x0E, 0x0E, 0xF5, 0xB1, 0x5E, 0x01, 0x37, 0x4E, 0xF8, 0x04, 0x0B, 0x09, 0x2F}, 0, 0, 0x0100u},
    /* PROTOTYPE engine (psandvita farm, Vita3K only): VJO_RF_PROTOTYPE; PCSG00007 reads through the text struct's buffer pointer */
    /* PCSG00007 リトルバスターズ！ Converted Edition (APP_VER 01.00): Vita3K 0x8101d15c, 562 farm pages (paired-vita-pcsg00007-20261023); patch: ldr.w r7, [r8, #0x40]; movw r0, #0x6b04 */
    {"PCSG00007", 0x8A335ECDu, 0x1D15Cu, 1, 4, VJO_HOOK_ENC_CP932, 16, 0,
     {0xD8, 0xF8, 0x40, 0x70, 0x46, 0xF6, 0x04, 0x30, 0xD8, 0xF8, 0x08, 0x60, 0x46, 0xF6, 0xF8, 0x22}, VJO_RF_PROTOTYPE, 1, 0x0100u},
    /* PCSG00206 グリザイアの果実 -LE FRUIT DE LA GRISAIA- (APP_VER 01.00): Vita3K 0x81025fde, 3292 farm pages (paired-vita-pcsg00206-20261023); patch: ldr.w r0, [r8, #0x48]; movw r1, #0x8be8; ldr.w r7, [r8, #0x8] */
    {"PCSG00206", 0x4668F40Fu, 0x25FDEu, 1, 4, VJO_HOOK_ENC_CP932, 16, 10282,
     {0xD8, 0xF8, 0x48, 0x00, 0x48, 0xF6, 0xE8, 0x31, 0xD8, 0xF8, 0x08, 0x70, 0x48, 0xF6, 0xD8, 0x33}, VJO_RF_PROTOTYPE, 0, 0x0100u},
    /* PCSG00620 グリザイアの果実スピンアウト！？ アイドル魔法少女ちるちる☆みちる (APP_VER 01.00): Vita3K 0x81031e90, 995 farm pages (paired-vita-pcsg00620-20261023); patch: ldr.w lr, [r5, #0x50]; ldr r2, [r5, #0x8]; cmp lr, r2 */
    {"PCSG00620", 0x90E7EC14u, 0x31E90u, 1, 4, VJO_HOOK_ENC_CP932, 16, 1066,
     {0xD5, 0xF8, 0x50, 0xE0, 0xAA, 0x68, 0x96, 0x45, 0x05, 0xDB, 0xBA, 0xF1, 0x00, 0x0F, 0x02, 0xD1}, VJO_RF_PROTOTYPE, 0, 0x0100u},
    /* IF:Lib GAME-ENGINE 3.00 (psandvita farm, Vita3K only): each title's VJO_RF_ filter writes its default names, then VJO_RF_IFENGINE; Luna's filter at the same address dropped them */
    /* PCSG00448 MARGINAL#4 IDOL OF SUPERNOVA (APP_VER 01.00): Vita3K 0x8106d8f8, 497 farm pages (paired-vita-pcsg00448-20261023); patch: ldr.w r0, [r8, #0x354]; ldrsb.w r0, [r0, #0x5]; Luna's site here (r0, filter FPCSG00448) */
    {"PCSG00448", 0x84668EFEu, 0x6D8F8u, 1, 0, VJO_HOOK_ENC_CP932, 16, 0,
     {0xD8, 0xF8, 0x54, 0x03, 0x90, 0xF9, 0x05, 0x00, 0xF5, 0xF7, 0x25, 0xFC, 0x28, 0x1C, 0x25, 0xF0}, VJO_RF_PCSG00448, 0, 0x0100u},
    /* PCSG00469 CLOCK ZERO ～終焉の一秒～ ExTime (APP_VER 01.00): Vita3K 0x81027a9c, 2047 farm pages (paired-vita-pcsg00469-20261023); patch: push.w {r4, r5, r6, r7, r8, r11, lr}; sub sp, #0x14; movs r1, #0x0; Luna's site here (r0, filter FPCSG00815) */
    {"PCSG00469", 0x31270870u, 0x27A9Cu, 1, 0, VJO_HOOK_ENC_UTF8, 16, 0,
     {0x2D, 0xE9, 0xF0, 0x49, 0x85, 0xB0, 0x00, 0x21, 0x00, 0x91, 0x04, 0x1C, 0x04, 0x91, 0x83, 0xF1}, VJO_RF_PCSG00469, 0, 0x0100u},
    /* PCSG00853 ピリオドキューブ～鳥籠のアマデウス～ (APP_VER 01.00): Vita3K 0x81007df2, 749 farm pages (paired-vita-pcsg00853-20261023); patch: movw r0, #0x1098; adds r0, r4, r0; ldrsb.w r4, [r5]; movs.w r9, #0x0; Luna's site here (r5, filter PCSG00530) */
    {"PCSG00853", 0xA92335D4u, 0x7DF2u, 1, 5, VJO_HOOK_ENC_UTF8, 16, 0,
     {0x41, 0xF2, 0x98, 0x00, 0x20, 0x18, 0x95, 0xF9, 0x00, 0x40, 0x5F, 0xF0, 0x00, 0x09, 0xE8, 0x90}, VJO_RF_PCSG00853, 0, 0x0100u},
    /* Luna sites the placement check refuses, unlocked by their signature (psandvita farm, Vita3K only): filter 0 takes Luna's filter at the address */
    /* PCSG00468 黒蝶のサイケデリカ (APP_VER 01.00): Vita3K 0x8103f538, 412 farm pages (paired-vita-pcsg00468-20261023); patch: ldr r2, [r5]; subs r2, r4, r2; strd r7, r9, [sp]; Luna's site here (r1, filter FPCSG00468) */
    {"PCSG00468", 0x4DB3D205u, 0x3F538u, 1, 1, VJO_HOOK_ENC_UTF8, 16, 0,
     {0x2A, 0x68, 0xA2, 0x1A, 0xCD, 0xE9, 0x00, 0x79, 0x57, 0x1E, 0xCD, 0xE9, 0x02, 0x85, 0x72, 0x46}, 0, 0, 0x0100u},
    /* PCSG00696 アンジェリーク ルトゥール (APP_VER 01.00): Vita3K 0x81087d1a, 127 farm pages (paired-vita-pcsg00696-20261023); patch: ldr.w r0, [r7, #0x9c]; adds r0, #0x1; str.w r0, [r7, #0x9c]; cmp r0, #0x20; Luna's site here (r1, filter FPCSG00696) */
    {"PCSG00696", 0x519F46E7u, 0x87D1Au, 1, 1, VJO_HOOK_ENC_CP932, 16, 0,
     {0xD7, 0xF8, 0x9C, 0x00, 0x01, 0x30, 0xC7, 0xF8, 0x9C, 0x00, 0x20, 0x28, 0x05, 0xD3, 0x00, 0x20}, 0, 0, 0x0100u},
    /* PCSG00745 スカーレッドライダーゼクス Rev. (APP_VER 01.00): Vita3K 0x810082d4, 410 farm pages (paired-vita-pcsg00745-20261023); patch: vcvt.s32.f32 s2, s19; ldr r0, [r4]; adds r1, r6, #0x0; Luna's site here (r12, filter PCSG01247) */
    {"PCSG00745", 0xC30CA4C1u, 0x82D4u, 1, 5, VJO_HOOK_ENC_UTF8, 16, 0,
     {0xBD, 0xEE, 0xE9, 0x1A, 0x20, 0x68, 0x31, 0x1C, 0xB0, 0xEE, 0x68, 0x0A, 0x40, 0x68, 0x2A, 0x1C}, 0, 0, 0x0100u},
    /* PCSG00808 悠久のティアブレイド -Lost Chronicle- (APP_VER 01.00): Vita3K 0x8103142a, 488 farm pages (paired-vita-pcsg00808-20261023); patch: adds.w r6, r4, #0x40; movw r2, #0x2fa8; adds r0, r6, #0x0; movt r2, #0x8121; Luna's site here (r10, filter FPCSG00808) */
    {"PCSG00808", 0x8C990BE1u, 0x3142Au, 1, 10, VJO_HOOK_ENC_UTF8, 16, 0,
     {0x14, 0xF1, 0x40, 0x06, 0x42, 0xF6, 0xA8, 0x72, 0x30, 0x1C, 0xC8, 0xF2, 0x21, 0x12, 0x53, 0x46}, 0, 0, 0x0100u},
    /* PCSG00852 マジきゅんっ！　ルネッサンス (APP_VER 01.00): Vita3K 0x8107f75a, 278 farm pages (paired-vita-pcsg00852-20261023); patch: movw r3, #0x2980; movt r3, #0x810d; vldr s1, [r3, #12]; Luna's site here (r1, filter FPCSG00852) */
    {"PCSG00852", 0x8DFD228Du, 0x7F75Au, 1, 1, VJO_HOOK_ENC_CP932, 16, 0,
     {0x42, 0xF6, 0x80, 0x13, 0xC8, 0xF2, 0x0D, 0x13, 0xD3, 0xED, 0x03, 0x0A, 0x96, 0x46, 0x93, 0xED}, 0, 0, 0x0100u},
    /* PCSG00916 Starry☆Sky ～Summer Stories～ (APP_VER 01.00): Vita3K 0x81030114, 949 farm pages (paired-vita-pcsg00916-20261023); patch: bfi r3, r2, #8, #8; sxth.w lr, r3; Luna's site here (r0, filter FPCSG00912) */
    {"PCSG00916", 0x4813A011u, 0x30114u, 1, 0, VJO_HOOK_ENC_CP932, 16, 0,
     {0x62, 0xF3, 0x0F, 0x23, 0x0F, 0xFA, 0x83, 0xFE, 0x41, 0xF2, 0xC4, 0x30, 0xAD, 0xF8, 0xA8, 0xE0}, 0, 0, 0x0100u},
    /* PCSG00935 BLACK WOLVES SAGA  -Weiβ und Schwarz- (APP_VER 01.00): Vita3K 0x810541a2, 387 farm pages (paired-vita-pcsg00935-20261023); patch: push {r4, lr}; movw r2, #0xd3ac; adds r4, r2, #0x0; movt r4, #0x816f; Luna's site here (r0, filter PCSG00766) */
    {"PCSG00935", 0x8EA89B16u, 0x541A2u, 1, 0, VJO_HOOK_ENC_UTF8, 16, 0,
     {0x10, 0xB5, 0x4D, 0xF2, 0xAC, 0x32, 0x14, 0x1C, 0xC8, 0xF2, 0x6F, 0x14, 0x01, 0x1C, 0x14, 0xF1}, VJO_RF_IFENGINE, 0, 0x0100u},
    /* PCSG01075 悠久のティアブレイド -Fragments of Memory- (APP_VER 01.00): Vita3K 0x81031f44, 333 farm pages (paired-vita-pcsg01075-20261023); patch: adds.w r6, r4, #0x40; movw r2, #0x446c; Luna's site here (r10, filter FPCSG01075) */
    {"PCSG01075", 0x8ECBA36Au, 0x31F44u, 1, 10, VJO_HOOK_ENC_UTF8, 16, 0,
     {0x14, 0xF1, 0x40, 0x06, 0x44, 0xF2, 0x6C, 0x42, 0x30, 0x1C, 0xC8, 0xF2, 0x22, 0x12, 0x53, 0x46}, 0, 0, 0x0100u},
    /* PCSG01221 紅色天井艶妖綺譚 二藍 (APP_VER 01.00): Vita3K 0x8103b554, 5 farm pages (paired-vita-pcsg01221-20261023); patch: movw r0, #0xb8c8; movt r0, #0x812f; Luna's site here (r4, filter FPCSG00852) */
    {"PCSG01221", 0x1A9CF884u, 0x3B554u, 1, 4, VJO_HOOK_ENC_CP932, 16, 0,
     {0x4B, 0xF6, 0xC8, 0x00, 0xC8, 0xF2, 0x2F, 0x10, 0x02, 0x68, 0x42, 0xF6, 0x3C, 0x60, 0xC8, 0xF2}, 0, 0, 0x0100u},
    /* Other engines (psandvita farm, Vita3K only): Rubi filters per title; PCSG00450 reads through the message's text pointer */
    /* PCSG00380 真 流行り神 (APP_VER 01.00): Vita3K 0x8104de9e, 1095 farm pages (paired-vita-pcsg00380-20261023), the
     * per-frame message-window draw (r4 = 0 there); the page is foaf4's static record at 0x819dbff0, 0x98e152 past
     * the site (r15): its cells (VJO_PLACE_READ_CELLS); patch (12): str r4, [sp, #0xa8]; movt r0, #0x819d; ldr r7,
     * [r0]; ldr.w r12, [r0, #0x4] */
    {"PCSG00380", 0x9A11E118u, 0x4DE9Eu, 1, 15, VJO_HOOK_ENC_CP932, 16, 0x98E152,
     {0x2A, 0x94, 0xC8, 0xF2, 0x9D, 0x10, 0x07, 0x68, 0xD0, 0xF8, 0x04, 0xC0, 0x4C, 0xF2, 0xE0, 0x60}, VJO_RF_PCSG00380,
     VJO_PLACE_READ_CELLS, 0x0100u},
    /* PCSG00450 キャプテン・アース Mind Labyrinth (APP_VER 01.00): Vita3K 0x8106ab56, 78 farm pages (paired-vita-pcsg00450-20261023); patch: push {r4, r5, r6, lr}; sub sp, #0x1f8; movw lr, #0x4250; movt lr, #0x8123 */
    {"PCSG00450", 0x28CB1B14u, 0x6AB56u, 1, 1, VJO_HOOK_ENC_UTF16LE, 16, 32,
     {0x70, 0xB5, 0xFE, 0xB0, 0x44, 0xF2, 0x50, 0x2E, 0xC8, 0xF2, 0x23, 0x1E, 0xDE, 0xF8, 0x00, 0xE0}, VJO_RF_PCSG00450, 1, 0x0100u},
    /* PCSG00509 POSSESSION MAGENTA (APP_VER ?): Vita3K 0x81012c76, 1299 farm pages (paired-vita-pcsg00509-20261023); patch: adds r2, r5, #0x0; adds.w r0, r4, #0x9c0; movs r5, #0x0; movs.w r1, #0x104; Luna's site here (r0, filter FPCSG00852) */
    {"PCSG00509", 0xA11A6741u, 0x12C76u, 1, 0, VJO_HOOK_ENC_CP932, 16, 0,
     {0x2A, 0x1C, 0x14, 0xF5, 0x1C, 0x60, 0x00, 0x25, 0x5F, 0xF4, 0x82, 0x71, 0x00, 0x23, 0xFE, 0xF7}, VJO_RF_PCSG00509, 0, 0},
    /* PCSG00540 忠臣蔵46+1 V (APP_VER ?): Vita3K 0x8102b90a, 1037 farm pages (paired-vita-pcsg00540-20261023); patch: push.w {r4, r5, r6, r7, r8, r9, r10, r11, lr}; sub sp, #0x84; movw r4, #0x5bf8; movt r4, #0x8106 */
    {"PCSG00540", 0xC595CF25u, 0x2B90Au, 1, 1, VJO_HOOK_ENC_CP932, 16, 0,
     {0x2D, 0xE9, 0xF0, 0x4F, 0xA1, 0xB0, 0x45, 0xF6, 0xF8, 0x34, 0xC8, 0xF2, 0x06, 0x14, 0x24, 0x68}, VJO_RF_PCSG00540, 0, 0},
    /* PCSG00553 剣が君 for V (APP_VER ?): Vita3K 0x821452b8, 1130 farm pages (paired-vita-pcsg00553-20261023); patch: mov r12, sp; push {r4, r5, r6, r7, r8, r11, r12, lr} */
    {"PCSG00553", 0xCF234754u, 0x11452B8u, 0, 1, VJO_HOOK_ENC_UTF16LE, 16, 12,
     {0x0D, 0xC0, 0xA0, 0xE1, 0xF0, 0x59, 0x2D, 0xE9, 0x20, 0xD0, 0x4D, 0xE2, 0x0D, 0xB0, 0xA0, 0xE1}, VJO_RF_BR_NEWLINE, 0, 0},
    /* PCSG00628 ALIA's CARNIVAL! サクラメント (APP_VER 01.00): Vita3K 0x8103373c, 1208 farm pages (paired-vita-pcsg00628-20261023); patch: str r4, [r6, #0x2c]; movs r4, #0x0; str.w r9, [r6, #0xb4] */
    {"PCSG00628", 0x1EE45F02u, 0x3373Cu, 1, 5, VJO_HOOK_ENC_CP932, 16, 0,
     {0xF4, 0x62, 0x00, 0x24, 0xC6, 0xF8, 0xB4, 0x90, 0xA1, 0x46, 0x95, 0xF9, 0x00, 0x00, 0x18, 0xB1}, VJO_RF_PCSG00628, 0, 0x0100u},
    /* PCSG00648 月に寄りそう乙女の作法 (APP_VER ?): Vita3K 0x81026efa, 1397 farm pages (paired-vita-pcsg00648-20261023); patch: strh r0, [r4]; movw r1, #0x8638; ldrb r0, [r7]; movt r1, #0x8128; Luna's site here (r2, filter none) */
    {"PCSG00648", 0xE5819F44u, 0x26EFAu, 1, 2, VJO_HOOK_ENC_CP932, 16, 0,
     {0x20, 0x80, 0x48, 0xF2, 0x38, 0x61, 0x38, 0x78, 0xC8, 0xF2, 0x28, 0x11, 0x7A, 0x78, 0xB1, 0xF9}, VJO_RF_ANGLE_RUBY, 0, 0},
    /* PCSG00713 百花百狼 ～戦国忍法帖～ (APP_VER 01.00): Vita3K 0x81112f46, 1464 farm pages (paired-vita-pcsg00713-20261023); patch: movs.w r10, #0x0; str.w r10, [sp, #0x1c]; add r2, sp, #0x1c; mov r0, r11 */
    {"PCSG00713", 0x08B1647Cu, 0x112F46u, 1, 6, VJO_HOOK_ENC_UTF8, 16, 0,
     {0x5F, 0xF0, 0x00, 0x0A, 0xCD, 0xF8, 0x1C, 0xA0, 0x07, 0xAA, 0x58, 0x46, 0x03, 0x21, 0x09, 0xF7}, 0, 0, 0x0100u},
    /* PCSG00787 鏡界の白雪 (APP_VER 01.00): Vita3K 0x81021f1a, 502 farm pages (paired-vita-pcsg00787-20261023); patch: str.w r4, [r5, #0x148]; adds.w r6, r5, #0x148; str.w r4, [r5, #0x14c] */
    {"PCSG00787", 0xA88A2E95u, 0x21F1Au, 1, 6, VJO_HOOK_ENC_UTF8, 16, 0,
     {0xC5, 0xF8, 0x48, 0x41, 0x15, 0xF5, 0xA4, 0x76, 0xC5, 0xF8, 0x4C, 0x41, 0x30, 0x1C, 0xC5, 0xF8}, VJO_RF_BACKSLASH_NEWLINE, 0, 0x0100u},
    /* PCSG00807 Dance with Devils (APP_VER 01.00): Vita3K 0x81d68164, 690 farm pages (paired-vita-pcsg00807-20261023); patch: mov r12, sp; push {r4, r5, r6, r7, r8, r11, r12, lr} */
    {"PCSG00807", 0x608A4999u, 0xD68164u, 0, 1, VJO_HOOK_ENC_UTF16LE, 16, 12,
     {0x0D, 0xC0, 0xA0, 0xE1, 0xF0, 0x59, 0x2D, 0xE9, 0x20, 0xD0, 0x4D, 0xE2, 0x0D, 0xB0, 0xA0, 0xE1}, VJO_RF_BR_NEWLINE, 0, 0x0100u},
    /* PCSG00817 EVE burst error R (APP_VER 01.00): Vita3K 0x81019132, 685 farm pages (paired-vita-pcsg00817-20261023); patch: str r0, [sp, #0x38]; ldr r0, [sp, #0x14]; str r0, [sp, #0x3c]; movw r0, #0xe0; ldr.w r0, [sp, r0] */
    {"PCSG00817", 0x2DBAE009u, 0x19132u, 1, 0, VJO_HOOK_ENC_CP932, 16, 0,
     {0x0E, 0x90, 0x05, 0x98, 0x0F, 0x90, 0x40, 0xF2, 0xE0, 0x00, 0x5D, 0xF8, 0x00, 0x00, 0x10, 0x90}, VJO_RF_PCSG00817, 0, 0x0100u},
    /* PCSG00876 7'scarlet (APP_VER 01.00): Vita3K 0x81012b60, 1051 farm pages (paired-vita-pcsg00876-20261023); patch: movs r1, #0x0; movs r2, #0x0; adds r3, r1, #0x0; strd r2, r3, [sp, #8] */
    {"PCSG00876", 0x16FEB862u, 0x12B60u, 1, 6, VJO_HOOK_ENC_UTF8, 16, 0,
     {0x00, 0x21, 0x00, 0x22, 0x0B, 0x1C, 0xCD, 0xE9, 0x02, 0x23, 0x28, 0x1C, 0xCD, 0xE9, 0x04, 0x23}, VJO_RF_PCSG00876, 0, 0x0100u},
    /* PCSG01194 参千世界遊戯 ～Re Multi Universe Myself～ (APP_VER 01.00): Vita3K 0x81056e24, 30 farm pages (paired-vita-pcsg01194-20261023); patch: ldr r1, [r4, #0x4]; adds r1, r7, r1; str r0, [sp, #0xc]; adds r0, r1, #0x2; Luna's site here (r0, filter none) */
    {"PCSG01194", 0xECD30306u, 0x56E24u, 1, 0, VJO_HOOK_ENC_CP932, 16, 0,
     {0x61, 0x68, 0x79, 0x18, 0x03, 0x90, 0x88, 0x1C, 0xD6, 0xF8, 0x20, 0x13, 0x00, 0xAA, 0x60, 0x60}, VJO_RF_PCSG01194, 0, 0x0100u},
};
#define VERIFIED_N (sizeof(verified) / sizeof(verified[0]))

unsigned vjo_place_verified(const char *title_id, const VjoPlaceVerified **out)
{
    unsigned first = VERIFIED_N, n = 0;
    *out = NULL;
    if (!title_id) return 0;
    for (unsigned i = 0; i < VERIFIED_N; i++) {
        if (strncmp(verified[i].title, title_id, 9) || title_id[9]) continue;
        if (first == VERIFIED_N) first = i;
        n++;
    }
    if (n) *out = &verified[first];
    return n;
}

unsigned vjo_place_verified_all(const VjoPlaceVerified **out)
{
    *out = verified;
    return VERIFIED_N;
}

int vjo_place_title_verified(const char *title_id)
{
    const VjoPlaceVerified *v;
    return vjo_place_verified(title_id, &v) > 0;
}

int vjo_place_check_verified(const VjoPlaceVerified *v, const uint8_t *code, uint32_t code_size,
                             uint32_t seg_base, uint32_t module_nid)
{
    unsigned need = v->sig_len > VJO_PLACE_SAVE_BYTES ? v->sig_len : VJO_PLACE_SAVE_BYTES;
    if (v->off >= code_size || code_size - v->off < need) return VJO_PLACE_RANGE;
    if (v->off & (v->thumb ? 1u : 3u) || (seg_base & 3u)) return VJO_PLACE_ALIGN;
    if (v->sig_len < vjo_place_patch_bytes(seg_base + v->off, v->thumb) || v->sig_len > 16) return VJO_PLACE_SIGNATURE;
    if (module_nid != v->module_nid || memcmp(code + v->off, v->sig, v->sig_len)) return VJO_PLACE_SIGNATURE;
    return VJO_PLACE_OK;
}

unsigned vjo_place_cells(uint8_t *p, unsigned n)
{
    unsigned w = 0;
    for (unsigned i = 0; i < n; i++) {
        uint8_t lo = p[2 * i], hi = p[2 * i + 1]; /* read before w (at most 2 i) writes over them */
        if (!lo) break;
        p[w++] = lo;
        if (hi) p[w++] = hi;
    }
    return w;
}

/* Text bytes that are neither ever in a line: C0 controls but tab, LF, CR; DEL. */
static inline int ascii_ok(uint8_t b) { return (b >= 0x20u && b < 0x7Fu) || b == '\t' || b == '\n' || b == '\r'; }

static int cp932_text(const uint8_t *p, unsigned k, int cut)
{
    unsigned jp = 0;
    for (unsigned i = 0; i < k; ) {
        uint8_t b = p[i];
        if (b < 0x80u) {
            if (!ascii_ok(b)) return 0;
            i++;
            continue;
        }
        /* Double-byte leads only: half-width katakana (A1-DF) and 80, A0, FD-FF are not text here. */
        if (!((b >= 0x81u && b <= 0x9Fu) || (b >= 0xE0u && b <= 0xFCu))) return 0;
        if (i + 1 >= k) {
            if (!cut) return 0;
        } else {
            uint8_t t = p[i + 1];
            if (t < 0x40u || t == 0x7Fu || t > 0xFCu) return 0;
        }
        if (b == 0x82u || b == 0x83u || (b >= 0x88u && b <= 0x9Fu) || (b >= 0xE0u && b <= 0xEAu)) jp++;
        i += 2;
    }
    return jp >= 2;
}

static int utf8_text(const uint8_t *p, unsigned k, int cut)
{
    unsigned jp = 0;
    for (unsigned i = 0; i < k; ) {
        uint8_t b = p[i];
        unsigned len, j;
        if (b < 0x80u) {
            if (!ascii_ok(b)) return 0;
            i++;
            continue;
        }
        if (b >= 0xC2u && b <= 0xDFu) len = 2;
        else if (b >= 0xE0u && b <= 0xEFu) len = 3;
        else if (b >= 0xF0u && b <= 0xF4u) len = 4;
        else return 0;
        for (j = 1; j < len && i + j < k; j++)
            if ((p[i + j] & 0xC0u) != 0x80u) return 0;
        if (j < len && !cut) return 0;
        /* Shortest forms and no surrogates: E0 A0-BF, ED 80-9F, F0 90-BF, F4 80-8F. */
        if (j > 1 && ((b == 0xE0u && p[i + 1] < 0xA0u) || (b == 0xEDu && p[i + 1] > 0x9Fu) ||
                      (b == 0xF0u && p[i + 1] < 0x90u) || (b == 0xF4u && p[i + 1] > 0x8Fu))) return 0;
        if (len == 3 && b >= 0xE3u && b <= 0xE9u) jp++;
        i += len;
    }
    return jp >= 2;
}

int vjo_place_text_prefilter(const uint8_t *p, unsigned n)
{
    unsigned k = n < 16u ? n : 16u, z;
    int cut;
    if (!p || n < 4) return 0;
    for (z = 0; z < k && p[z]; z++) {}
    cut = z == 16u && n > 16u; /* the 16-byte limit, not the text's end, stops the look */
    if (z < 4) return 0;
    return utf8_text(p, z, cut) || cp932_text(p, z, cut);
}

int vjo_place_pointer_array(const uint8_t *p, unsigned n)
{
    if (!p || n < VJO_PLACE_POINTER_MIN || n % 4) return 0;
    for (unsigned i = 3; i < n; i += 4)
        if (p[i] < 0x81u || p[i] > 0x9Fu) return 0;
    return 1;
}
