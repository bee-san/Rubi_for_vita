/* Where a native text hook may be placed in a game's code (docs/native-hooks.md): the pure-C checks
 * the game worker (game_ocr/hooks.c) runs on a site's bytes before taiHookFunctionOffset, and the
 * CALL source's prefilter. No platform calls, no allocation: host tests feed them byte fixtures.
 *
 * taiHEN's offset hook (libsubstitute, taiHEN 309b380 substitute/lib/arm/jump-patch.h) overwrites
 * jump_patch_size bytes at the site, 8, or 12 for a Thumb site at an address = 2 mod 4, and moves
 * every instruction that starts in them into a trampoline. A moved instruction that reads the PC, a
 * branch, an IT block or a branch target inside those bytes would run wrongly there. So a site is
 * accepted only when
 *   - it is a verified entry (Rubi's own list, vjo_place_verified: title, module NID and the bytes
 *     at the site, at least the whole patch, exactly as recorded), or
 *   - its first instruction is a function's entry that saves LR (Thumb PUSH {..., lr} or
 *     PUSH.W {..., lr}, ARM STMFD sp!, {..., lr}), and every instruction that starts in the patch
 *     is on a short allow-list of plain register and stack instructions (vjo_place_check).
 * Everything else is refused, with its reason (VJO_PLACE_*), so a later build can add a verified
 * signature for it. */
#ifndef VJO_HOOK_PLACE_H
#define VJO_HOOK_PLACE_H

#include <stddef.h>
#include <stdint.h>

/* taiHEN's FUNC_SAVE_SIZE (taihen_internal.h): the bytes it saves at a site to restore on release.
 * They must all lie in the segment's code too. */
#define VJO_PLACE_SAVE_BYTES 16u
/* libsubstitute's MAX_JUMP_PATCH_SIZE */
#define VJO_PLACE_PATCH_MAX 12u

/* Results and refusal reasons */
enum {
    VJO_PLACE_OK = 0,
    VJO_PLACE_RANGE = 1,     /* the site, or its saved bytes, is outside segment 0's code */
    VJO_PLACE_ALIGN = 2,     /* misaligned for its instruction set (ARM: 4, Thumb: 2) */
    VJO_PLACE_ENTRY = 3,     /* the first instruction is not a PUSH/STMFD that saves LR */
    VJO_PLACE_WINDOW = 4,    /* an instruction in the patch is not on the allow-list */
    VJO_PLACE_SIGNATURE = 5, /* a verified entry's bytes or module NID differ */
    VJO_PLACE_UNPORTED = 6,  /* the table entry needs code that is not ported (Luna's ported == 0) */
    VJO_PLACE_VERSION = 7,   /* the entry is for another version of the game */
    VJO_PLACE_FULL = 8,      /* no stream slot left */
    VJO_PLACE_TAI = 9,       /* taiHookFunctionOffset refused it */
    VJO_PLACE_RELOCATED = 10, /* a verified entry that reads a static at a fixed distance from its code (register 15)
                               * in a segment 0 that is not at VJO_PLACE_FIXED_SEG0, where it was proven */
    VJO_PLACE_COUNT
};

/* Where a fixed-address executable's segment 0 is linked, and so loaded: such an eboot "cannot be relocated"
 * (h-encore's write-up, 85b345e: games built with an SDK 2.60 and lower). An entry whose register is 15 reads a
 * static at the distance the farm saw from its site, true only where the eboot is loaded as it was linked. */
#define VJO_PLACE_FIXED_SEG0 0x81000000u

/* A short name for a reason (the debug log). */
const char *vjo_place_reason(int reason);

/* libsubstitute's jump_patch_size for a site at address addr (the Thumb bit cleared): the bytes the
 * hook overwrites. */
static inline unsigned vjo_place_patch_bytes(uint32_t addr, int thumb)
{
    return thumb && (addr & 2u) ? 12u : 8u;
}

/* Thumb (1) or ARM (0) for a table site: an address with bit 0 or bit 1 set is Thumb (when its
 * bytes are a Thumb entry); else the bytes decide (a Thumb PUSH/PUSH.W saving LR, or an ARM STMFD
 * sp! saving LR; the two forms cannot be confused). -1 when neither entry form is there. p: avail
 * bytes at the site (the address with bit 0 cleared). */
int vjo_place_isa(const uint8_t *p, unsigned avail, uint32_t addr);

/* The check for an unverified site at segment-0 offset off of a segment whose first code_size bytes
 * are code (code points at them; seg_base is their address in the game): VJO_PLACE_OK or the reason
 * it is refused. */
int vjo_place_check(const uint8_t *code, uint32_t code_size, uint32_t seg_base, uint32_t off, int thumb);

/* Rubi's own verified sites (emulator- or console-checked, docs/native-hooks.md). */
typedef struct {
    const char *title;     /* 9 characters */
    uint32_t module_nid;   /* the main module's NID (taiGetModuleInfo) */
    uint32_t off;          /* segment 0 offset */
    uint8_t thumb;
    uint8_t reg;           /* the register holding the text's address (r0-r12, sp, lr; 15: the site's own
                            * address, for a static at a fixed distance from the code: padding is that distance,
                            * and the entry is tried only with segment 0 at VJO_PLACE_FIXED_SEG0) */
    uint8_t encoding;      /* VJO_HOOK_ENC_* (VJO_HOOK_ENC_CP932_A0: CP932 with its 0xA0 byte, for a filter that maps it) */
    uint8_t sig_len;       /* 8..16, at least the patch's bytes */
    int32_t padding;       /* added to the register's value */
    uint8_t sig[16];       /* the bytes at the site */
    /* The text's processing (core/hook_luna.h): 0, a Luna site's at the same address if there is one,
     * else none; or a VJO_LF_* or VJO_RF_* filter, which replaces that site's filter (its hookfunc's
     * clean-up still runs first). */
    uint8_t filter;
    /* How the text is read at the register's value + padding (VJO_PLACE_READ_*): the string there, through a
     * 32-bit pointer there (one more read, as Luna's DATA_INDIRECT), a page record of 16-bit cells (one of two
     * layouts), or an SC3 string through its end token. */
    uint8_t read;
    /* The game version the entry was proven in, its param.sfo APP_VER as game_ocr/hooks.c reads it ("01.00" ->
     * 0x0100): another version refuses the entry; an APP_VER that cannot be read leaves it to the module NID and
     * the bytes, which pin the build anyway (a game's update changes its eboot and so its NID); 0: the version was
     * not recorded. In the row's tail padding: the row is 40 bytes on the Vita either way. */
    uint16_t app_ver;
} VjoPlaceVerified;

/* VjoPlaceVerified.read */
#define VJO_PLACE_READ_STRING 0u /* the NUL-terminated string at the address, in the entry's encoding */
#define VJO_PLACE_READ_DEREF 1u  /* the address holds the string's 32-bit pointer (an unreadable one: no text) */
/* A page record of 16-bit cells (the engines of PCSG00380 and PCSG00914): a 32-bit count of cells at the
 * address + VJO_PLACE_CELLS_COUNT, the cells from + VJO_PLACE_CELLS_DATA, each little-endian: its low byte the
 * character's first CP932 byte, its high byte the second one, or 0 for a single-byte character
 * (vjo_place_cells). Exactly count cells are read (none past the record: a longer page left there is not
 * read), at most VJO_HOOK_RAW_BYTES / 2. */
#define VJO_PLACE_READ_CELLS 2u
#define VJO_PLACE_CELLS_COUNT 0x14u
#define VJO_PLACE_CELLS_DATA 0x20u
/* A MAGES SC3 string (core/hook_sc3.h; the entry's encoding is VJO_HOOK_ENC_SC3): read through its 0xFF end token,
 * past the 0x00 bytes it holds (line breaks, glyphs' low bytes), at most VJO_HOOK_RAW_BYTES and none past the memory
 * block; a string whose end is not among them gives no text (vjo_sc3_span). */
#define VJO_PLACE_READ_SC3 3u
/* The same cells in Nippon Ichi's foaf4 page-build record (PCSG00769's, which the engine copies to the page it shows):
 * its 32-bit count at + VJO_PLACE_CELLS_B_COUNT, the cells from + VJO_PLACE_CELLS_B_DATA, read as
 * VJO_PLACE_READ_CELLS reads its record's. */
#define VJO_PLACE_READ_CELLS_B 4u
#define VJO_PLACE_CELLS_B_COUNT 0x20u
#define VJO_PLACE_CELLS_B_DATA 0x2Eu

/* A read of cells (VJO_PLACE_READ_CELLS or VJO_PLACE_READ_CELLS_B): 1 and its record's count and cells offsets in
 * *count and *data; else 0. */
static inline int vjo_place_cells_layout(unsigned read, uint32_t *count, uint32_t *data)
{
    if (read != VJO_PLACE_READ_CELLS && read != VJO_PLACE_READ_CELLS_B) return 0;
    *count = read == VJO_PLACE_READ_CELLS ? VJO_PLACE_CELLS_COUNT : VJO_PLACE_CELLS_B_COUNT;
    *data = read == VJO_PLACE_READ_CELLS ? VJO_PLACE_CELLS_DATA : VJO_PLACE_CELLS_B_DATA;
    return 1;
}

/* The CP932 bytes of n cells at p, in place (they never grow): each cell's low byte, then its high byte
 * unless 0; a cell whose low byte is 0 ends the text. Returns the bytes. */
unsigned vjo_place_cells(uint8_t *p, unsigned n);

/* The verified entries of title_id, contiguous; 0 and *out = NULL when none. */
unsigned vjo_place_verified(const char *title_id, const VjoPlaceVerified **out);

/* Rubi's whole list (the host tests check every entry): its entries in *out, their number returned. */
unsigned vjo_place_verified_all(const VjoPlaceVerified **out);

/* 1 when Rubi's own list has an entry for title_id (the Games page's tag), else 0. */
int vjo_place_title_verified(const char *title_id);

/* The check for a verified entry v in a module with NID module_nid: its bytes (sig_len at
 * code + v->off, inside the code) and its NID exactly as recorded, and for an entry that reads a static from
 * its site's own address (register 15) segment 0 at VJO_PLACE_FIXED_SEG0 (VJO_PLACE_RELOCATED: #316's audit F4).
 * VJO_PLACE_OK or the reason. The entry's version is the caller's to check (vjo_place_verified_version). */
int vjo_place_check_verified(const VjoPlaceVerified *v, const uint8_t *code, uint32_t code_size,
                             uint32_t seg_base, uint32_t module_nid);

/* Whether v may be tried in a game whose APP_VER is app_ver (0: unreadable): 0 only when both are known
 * and differ. An unreadable APP_VER does not refuse an entry, unlike a Luna site that names its version
 * (vjo_luna_version_ok): the entry's NID and bytes are checked either way, and a Luna site has neither. */
static inline int vjo_place_verified_version(const VjoPlaceVerified *v, uint16_t app_ver)
{
    return !v->app_ver || !app_ver || v->app_ver == app_ver;
}

/* The CALL source's prefilter, in the hooked string function (a game thread): may the n bytes at p,
 * which the game has just copied, be a line of Japanese text? Reads at most 16 bytes. 1 when the
 * bytes before the first NUL (at least 4; at most 16 looked at) are all well-formed text in one
 * encoding (printable ASCII, tab, CR, LF, and whole CP932 double-byte or whole UTF-8 characters; a
 * character cut by the 16-byte limit counts as whole) with at least two Japanese characters (CP932:
 * leads 0x82, 0x83 (kana), 0x88-0x9F, 0xE0-0xEA (kanji); 0x81's punctuation is text but does not
 * count; half-width katakana is refused. UTF-8: three-byte characters with leads E3-E9), else 0. A
 * binary struct of pointers (0x81xxxxxx little-endian) or small integers fails: its low bytes are
 * control bytes or break the encoding. */
int vjo_place_text_prefilter(const uint8_t *p, unsigned n);

/* The CALL source's second test, for a copy the prefilter passed (audit-hooks F2): 1 when the n bytes at
 * p, all readable, have a pointer array's shape: n % 4 == 0, n >= VJO_PLACE_POINTER_MIN, and every byte at
 * index 3 mod 4 is 0x81..0x9F, the high byte of a little-endian user pointer (0x81000000-0x9FFFFFFF; such
 * a byte is also a CP932 lead, and the next word's low byte often a valid trail, so a struct of them can
 * pass the prefilter). Else 0. Reads n / 4 bytes at most, and stops at the first one out of that range. */
int vjo_place_pointer_array(const uint8_t *p, unsigned n);

/* The shortest copy vjo_place_pointer_array refuses: five words. With the conductor's 8 (two words), real
 * Japanese was refused: 2 of the repo's 1,080 CP932 lines of 8 B or more copied exactly (ソラユメ, 逢魔が刻,
 * 8 B each) and 120 of 64,136 substrings at character boundaries (106 of them 8 B, 12 at 12 B, 2 at 16 B);
 * from 20 B on none, in CP932 or UTF-8, while it still refuses 98% of the pointer structs that pass the
 * prefilter (2,868 of 2,930 uniform user pointers, 2-128 words) [measured, hooks-fix-engine's report and
 * $HERD/shared/artifacts/hooks-fix-engine/shape_variants.txt]. */
#define VJO_PLACE_POINTER_MIN 20u

#endif
