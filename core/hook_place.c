/* Where a native text hook may be placed (core/hook_place.h, docs/native-hooks.md). Pure C. */
#include "hook_place.h"

#include <string.h>

#include "hook_luna.h" /* VJO_RF_*: a verified entry's filter */
#include "hook_text.h"

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
 * only (vita-vn-ocr notes 22), not yet on a console.
 * アマツツミ (PCSG01198, AMATSUTSUMI_PSVITA, APP_VER 01.00): Luna's site 0x800785C8 (r2, CP932, its
 * filter: Luna's entry at the same address post-processes the text), which the placement check refuses
 * as no entry: it is inside a function (0x81070E3A's loop, 0x810745C8 in the game). Its 8 patched bytes
 * are ADD.W lr, r9, r7, LSL #2; ADD.W lr, lr, #0x1620 (no PC use), and no branch in segment 0 targets
 * them (the loop's BLT goes to +0x745D0, past them): checked on the decrypted eboot, 2026-10-23. Proven on a
 * console (PCH-2000, 3.65, 2026-10-23): hooked ("1 table sites"), its stream held text, it answered the jobs
 * (core/hook_select.h vjo_hook_select_no_memory: "game vocr source: hook 1, flags 0x98", rc 0), the overlay
 * showed the game's line (checked by eye against the screen), and the game did not crash. */
static const VjoPlaceVerified verified[] = {
    {"PCSG00415", 0xC5187D60u, 0x2CA70u, 1, 6, VJO_HOOK_ENC_CP932, 8, 0,
     {0xAB, 0xF8, 0x2C, 0x23, 0x14, 0xF5, 0x53, 0x70}, 0, 0},
    {"PCSG01198", 0x8AA413F2u, 0x745C8u, 1, 2, VJO_HOOK_ENC_CP932, 16, 0,
     {0x09, 0xEB, 0x87, 0x0E, 0x0E, 0xF5, 0xB1, 0x5E, 0x01, 0x37, 0x4E, 0xF8, 0x04, 0x0B, 0x09, 0x2F}, 0, 0},
    /* PROTOTYPE engine (psandvita farm, Vita3K only): VJO_RF_PROTOTYPE; PCSG00007 reads through the text struct's buffer pointer */
    /* PCSG00007 リトルバスターズ！ Converted Edition (APP_VER 01.00): Vita3K 0x8101d15c, 562 farm pages (paired-vita-pcsg00007-20261023); patch: ldr.w r7, [r8, #0x40]; movw r0, #0x6b04 */
    {"PCSG00007", 0x8A335ECDu, 0x1D15Cu, 1, 4, VJO_HOOK_ENC_CP932, 16, 0,
     {0xD8, 0xF8, 0x40, 0x70, 0x46, 0xF6, 0x04, 0x30, 0xD8, 0xF8, 0x08, 0x60, 0x46, 0xF6, 0xF8, 0x22}, VJO_RF_PROTOTYPE, 1},
    /* PCSG00206 グリザイアの果実 -LE FRUIT DE LA GRISAIA- (APP_VER 01.00): Vita3K 0x81025fde, 3292 farm pages (paired-vita-pcsg00206-20261023); patch: ldr.w r0, [r8, #0x48]; movw r1, #0x8be8; ldr.w r7, [r8, #0x8] */
    {"PCSG00206", 0x4668F40Fu, 0x25FDEu, 1, 4, VJO_HOOK_ENC_CP932, 16, 10282,
     {0xD8, 0xF8, 0x48, 0x00, 0x48, 0xF6, 0xE8, 0x31, 0xD8, 0xF8, 0x08, 0x70, 0x48, 0xF6, 0xD8, 0x33}, VJO_RF_PROTOTYPE, 0},
    /* PCSG00620 グリザイアの果実スピンアウト！？ アイドル魔法少女ちるちる☆みちる (APP_VER 01.00): Vita3K 0x81031e90, 995 farm pages (paired-vita-pcsg00620-20261023); patch: ldr.w lr, [r5, #0x50]; ldr r2, [r5, #0x8]; cmp lr, r2 */
    {"PCSG00620", 0x90E7EC14u, 0x31E90u, 1, 4, VJO_HOOK_ENC_CP932, 16, 1066,
     {0xD5, 0xF8, 0x50, 0xE0, 0xAA, 0x68, 0x96, 0x45, 0x05, 0xDB, 0xBA, 0xF1, 0x00, 0x0F, 0x02, 0xD1}, VJO_RF_PROTOTYPE, 0},
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
