/* LunaTranslator's PS Vita hook table (its Vita3K emulator hooks), ported for native
 * hooks in the game's own process (docs/native-hooks.md, docs/hook-titles.md).
 *
 * Provenance: HIllya51/LunaTranslator at 51884b6c88cba8c42e98ce3e43c4f408017082a1
 * (v12.1.2.12, GPL-3.0), src/NativeImpl/LunaHook/LunaHook/emulators/vita3k_1.cpp
 * (the emfunctionhooks_1 table, its hookfuncs and filters) and vita3k.cpp (the
 * address normalisation). tools/gen_luna_vita.py generates core/hook_luna_table.inc
 * from that file; core/hook_luna.c holds the C ports of the hookfuncs and filters.
 * third_party/LunaTranslator/README.md records the pin and the licence.
 *
 * Luna's addresses are Vita3K guest addresses after vita3k.cpp's addr_offset: the
 * main module's (eboot.bin's) first segment starts at VJO_LUNA_SEG0 in that space,
 * so a site's offset in segment 0 is addr - VJO_LUNA_SEG0. Some entries are not in
 * that space: Luna had no addr_offset before f237a9f4 (2026-05-20), and an entry added
 * before it can be the guest address itself. A Vita3K before 11dc3b8b (2026-05-27)
 * loaded a fixed-address executable at its link address (0x81000000 for every marked
 * game), where a console must load it too, and a relocatable one elsewhere (0x00105000,
 * 0x80004000); from 0.2.1 4033 Luna normalises every main module (-0xFFC000). So such
 * an entry is the site's address on a console only for a fixed-address executable:
 * TIME TRAVELERS' 0x8119ADDA (added 2025-09-24) is +0x19ADDA of its fixed-address
 * 0x31258C-byte segment 0 at 0x81000000 (public Vita3K logs), and less VJO_LUNA_SEG0
 * it is past the segment's end. In a relocatable executable such an address is another
 * module's (PCSG00708, a Unity eboot: its Mono AOT module). The generator marks the
 * sites the logs show at a fixed-address executable's guest address (VJO_LUNA_F_GUEST),
 * and vjo_luna_offset reads only those both ways. Vita3K hooks a site when its JIT
 * compiles a block that starts there: a function's entry, or a block inside one.
 * Nothing here touches the game: the worker (game_ocr/hooks.c) resolves the offset
 * against the real segment, checks the bytes there and decides. */
#ifndef VJO_HOOK_LUNA_H
#define VJO_HOOK_LUNA_H

#include <stddef.h>
#include <stdint.h>

#include "hook_text.h"

#define VJO_LUNA_SEG0 0x80004000u

/* VjoLunaSite.flags: Luna's emfuncinfo type bits that change what a hit reads, and Rubi's GUEST. */
#define VJO_LUNA_F_USING_CHAR 0x01u    /* the register holds one character, not a pointer */
#define VJO_LUNA_F_DATA_INDIRECT 0x02u /* the register (+ index) points at the text's pointer */
#define VJO_LUNA_F_FULL_STRING 0x04u   /* Luna's FULL_STRING */
/* Rubi's: Luna recorded the site at Vita3K's guest address in a fixed-address executable, as a public Vita3K log
 * shows it (tools/gen_luna_vita.py GUEST_SITES, with each one's evidence): vjo_luna_offset's second reading */
#define VJO_LUNA_F_GUEST 0x08u

/* VjoLunaSite.func / .filter value for "none" (Luna's 0) */
#define VJO_LUNA_NONE 0u

typedef struct {
    uint32_t addr;     /* Luna's address (VJO_LUNA_SEG0 space), as in its table */
    int32_t padding;   /* Luna's padding: added to the register's value */
    uint16_t title;    /* index into the title list (vjo_luna_title) */
    uint8_t reg;       /* Luna's offset: the guest register (0..15 = r0..r12, sp, lr, pc) read at the site */
    uint8_t encoding;  /* VJO_HOOK_ENC_* (Luna's default is CP932, CODEC_UTF8 / CODEC_UTF16 otherwise) */
    uint8_t flags;     /* VJO_LUNA_F_* */
    uint8_t func;      /* Luna's custom hookfunc, ported (VJO_LUNA_NONE: the default read) */
    uint8_t filter;    /* Luna's filterfun, ported (VJO_LUNA_NONE: none) */
    uint8_t ported;    /* 1: everything this site needs is ported; 0: refused natively (why: docs/hook-titles.md) */
    uint16_t version;  /* the game version Luna's comment names for this entry ("1.01" -> 0x0101), 0: none named */
} VjoLunaSite;

/* Whether site s, one of the n sites of its title (all, from vjo_luna_sites), may be
 * tried in a game whose param.sfo APP_VER is app_ver ("01.01" -> 0x0101; 0 when it
 * could not be read): a site that names a version only in that version; a site that
 * names none only when no site of its title names app_ver (those unnamed entries are
 * another version's). With app_ver 0 only the sites that name no version may be
 * tried. 1 yes, 0 no. */
int vjo_luna_version_ok(const VjoLunaSite *all, unsigned n, const VjoLunaSite *s, uint16_t app_ver);

/* The segment-0 offset of Luna's site s (its address, bit 0 ignored) in a main module whose segment 0 is code_size
 * bytes at seg_base in the game: addr - VJO_LUNA_SEG0 when that is inside the segment, Luna's space (every site
 * that main's mapping placed is still placed there); else, for a site marked VJO_LUNA_F_GUEST and segment 0 at
 * VJO_LUNA_GUEST_SEG0, where a fixed-address executable always is, addr - VJO_LUNA_GUEST_SEG0 when that is inside
 * it; else VJO_LUNA_NO_OFFSET. No other site gets the second reading: in a relocatable executable Vita3K's guest
 * address was never the eboot's own (above), so it would be unrelated code there. The two readings are 0xFFC000
 * apart: in a segment 0 of up to that size at most one of them is inside it. The bytes there decide as for any
 * Luna site (vjo_place_check). */
#define VJO_LUNA_GUEST_SEG0 0x81000000u
#define VJO_LUNA_NO_OFFSET 0xFFFFFFFFu
uint32_t vjo_luna_offset(const VjoLunaSite *s, uint32_t seg_base, uint32_t code_size);

/* The site of all[0..n) at segment-0 offset off (vjo_luna_offset of its address in segment 0's code_size bytes
 * at seg_base) that is ported and that app_ver may try (vjo_luna_version_ok), or NULL. A Rubi verified entry at
 * the same address takes its text's hookfunc and filter from it (game_ocr/hooks.c). */
const VjoLunaSite *vjo_luna_site_at(const VjoLunaSite *all, unsigned n, uint32_t off, uint16_t app_ver,
                                    uint32_t seg_base, uint32_t code_size);

/* Reads up to max bytes at the game's address addr into dst, never past the end of
 * the memory block that holds addr. unit 1 or 2: stop after the first NUL unit of
 * that size (copied, and counted); unit 0: read exactly max or nothing. Returns the
 * bytes copied, 0 when the address is not readable. The worker's reader checks the
 * block (sceKernelFindMemBlockByAddr); the host tests' reads a fake memory image. */
typedef unsigned (*VjoHookRead)(void *ud, uint32_t addr, void *dst, unsigned max, unsigned unit);

/* The table's sites of title_id (9 characters, e.g. "PCSG00543"), contiguous, in
 * the table's order; 0 and *sites = NULL for a title it does not list. */
unsigned vjo_luna_sites(const char *title_id, const VjoLunaSite **sites);

/* 1 when the table lists title_id, else 0. */
int vjo_luna_title_known(const char *title_id);

/* 1 when the table has at least one site of title_id that is ported (its ported flag, which is
 * vjo_luna_ported's answer: one the worker may try natively), else 0: "has a hook entry" for the Games page's tag (core/hook_titles.c) and the text
 * source auto (game_ocr/hooks.c), audit-hooks-text N2. A title whose every site is unported gets neither. */
int vjo_luna_title_ported(const char *title_id);

/* The title ID of a site's title index, or NULL. */
const char *vjo_luna_title(unsigned index);

/* Hook time: one hit of site s, in the game thread that reached it, inside the
 * game's function. regs[0..15]: r0..r12, sp, lr and the site's address, as they
 * were at the site. Copies the hit's raw text (Luna's default read: the string at
 * regs[reg] + padding, through the pointer for DATA_INDIRECT; one character for
 * USING_CHAR; or the site's ported hookfunc) into raw (cap bytes) and its encoding
 * into *encoding. Every read of game memory goes through rd; no allocation, no
 * other call, bounded work. Returns the bytes copied, 0 for no text. */
unsigned vjo_luna_capture(const VjoLunaSite *s, const uint32_t regs[16], VjoHookRead rd, void *ud,
                          uint8_t *raw, unsigned cap, unsigned *encoding);

/* Job time (the worker's thread): n raw bytes of a hit of s in encoding -> UTF-8
 * with the site's filter applied, NUL-terminated in out (cap bytes). Returns the
 * bytes before the NUL (0: nothing left after the filter). */
size_t vjo_luna_text(const VjoLunaSite *s, const uint8_t *raw, unsigned n, unsigned encoding, char *out,
                     size_t cap);

/* Rubi's own filters, for its verified entries (VjoPlaceVerified.filter, core/hook_place.h). They are
 * numbered from VJO_RF_BASE, so one byte names either a Luna filter (VJO_LF_*, below it) or one of
 * these, and they are written with the same ops as Luna's (core/hook_luna.c). Each one cites the
 * emulator evidence it comes from (docs/native-hooks.md#rubis-filters). */
#define VJO_RF_BASE 0x80u
enum {
    VJO_RF_PROTOTYPE = VJO_RF_BASE, /* PROTOTYPE engine: half-width kana, $t name, $d, $[base$/reading$] */
    VJO_RF_IFENGINE,                /* IF:Lib GAME-ENGINE 3.00: #Ruby[base,reading], #Tag[..], #n */
    VJO_RF_ANGLE_RUBY,              /* <base,reading> ruby: the base */
    VJO_RF_BR_NEWLINE,              /* <br> is a line break */
    VJO_RF_BACKSLASH_NEWLINE,       /* a lone \ is a line break */
    VJO_RF_PCSG00448,               /* MARGINAL#4 IDOL OF SUPERNOVA: default names, then IFENGINE */
    VJO_RF_PCSG00450,               /* キャプテン・アース Mind Labyrinth: 《SP ..》 voice cues */
    VJO_RF_PCSG00469,               /* CLOCK ZERO ExTime: the default name, then IFENGINE */
    VJO_RF_PCSG00509,               /* POSSESSION MAGENTA: ＊Ｂ name code, ^, <base,reading> */
    VJO_RF_PCSG00540,               /* 忠臣蔵46+1 V: MSGSET name, voice, wait codes, b ruby, half-width kana */
    VJO_RF_PCSG00817,               /* EVE burst error R: [n], [xNN] layout codes */
    VJO_RF_PCSG00853,               /* ピリオドキューブ: default names, then IFENGINE */
    VJO_RF_PCSG00876,               /* 7'scarlet: @@ line break, $<digit> style codes */
    VJO_RF_PCSG01194,               /* 参千世界遊戯: shin @-markup, half-width kana */
    VJO_RF_PCSG00628,               /* ALIA's CARNIVAL! サクラメント: MSGSET's SNR markup, half-width kana */
    VJO_RF_PCSG00380,               /* 真 流行り神: dash pieces, the default player name */
    VJO_RF_END
};

/* 1 when filter is VJO_LUNA_NONE or names a filter this build has (a ported Luna filter or one of
 * Rubi's), else 0. */
int vjo_luna_filter_known(unsigned filter);

/* vjo_luna_text for a Rubi verified entry with its own filter: the raw bytes decoded, the hookfunc
 * clean-up of s (a ported Luna site at the entry's address, or NULL: none) and Luna's commonfilter,
 * then filter (a VJO_LF_* or VJO_RF_*) in place of s's own, as on a site whose encoding is
 * site_encoding. An unknown filter, or an s that is not ported, gives 0 (no text). */
size_t vjo_luna_text_filter(const VjoLunaSite *s, unsigned filter, unsigned site_encoding, const uint8_t *raw,
                            unsigned n, unsigned encoding, char *out, size_t cap);

/* Added by hooks-luna-port (core/hook_luna.c). */

/* vjo_luna_capture reads through rd at most this many times per hit (a string
 * per call; the strings of a hookfunc that joins several, a pointer, a length). */
#define VJO_LUNA_READS 32u

/* vjo_luna_text works on at most this many bytes of decoded text (a longer text is
 * cut at its last whole character first): its scratch, on the caller's stack. */
#define VJO_LUNA_TEXT_MAX 1536u

/* 1 when s is ported (s->ported) and this build has every part it needs: the
 * sites vjo_luna_capture and vjo_luna_text serve; 0 otherwise. */
int vjo_luna_ported(const VjoLunaSite *s);

/* vjo_luna_capture: cap must be at least 4 (0 otherwise); its return counts the
 * text's bytes up to the first NUL unit (not the NUL); a site that is not ported
 * gives 0. */

/* What the flags change (texthook.cc, LunaHost's textthread.cpp at the pin):
 * FULL_STRING changes no read; LunaHost ends each such hit's text with a newline
 * and flushes the hits that come within its flush delay (100 ms by default) as
 * one sentence, a site without it the same without the newline. Rubi keeps a
 * site's latest hit. */

#endif
