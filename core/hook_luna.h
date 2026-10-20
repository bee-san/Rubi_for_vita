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
 * so a site's offset in segment 0 is addr - VJO_LUNA_SEG0. Vita3K hooks a site when
 * its JIT compiles a block that starts there: a function's entry, or a block inside
 * one. Nothing here touches the game: the worker (game_ocr/hooks.c) resolves the
 * offset against the real segment, checks the bytes there and decides. */
#ifndef VJO_HOOK_LUNA_H
#define VJO_HOOK_LUNA_H

#include <stddef.h>
#include <stdint.h>

#include "hook_text.h"

#define VJO_LUNA_SEG0 0x80004000u

/* VjoLunaSite.flags: Luna's emfuncinfo type bits that change what a hit reads. */
#define VJO_LUNA_F_USING_CHAR 0x01u    /* the register holds one character, not a pointer */
#define VJO_LUNA_F_DATA_INDIRECT 0x02u /* the register (+ index) points at the text's pointer */
#define VJO_LUNA_F_FULL_STRING 0x04u   /* Luna's FULL_STRING */

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
} VjoLunaSite;

/* Reads up to max bytes at the game's address addr into dst, never past the end of
 * the memory block that holds addr. unit 1 or 2: stop after the first NUL unit of
 * that size (copied, and counted); unit 0: read exactly max or nothing. Returns the
 * bytes copied, 0 when the address is not readable. The worker's reader checks the
 * block (sceKernelFindMemBlockByAddr); the host tests' reads a fake memory image. */
typedef unsigned (*VjoHookRead)(void *ud, uint32_t addr, void *dst, unsigned max, unsigned unit);

/* The table's sites of title_id (9 characters, e.g. "PCSG00543"), contiguous, in
 * the table's order; 0 and *sites = NULL for a title it does not list. */
unsigned vjo_luna_sites(const char *title_id, const VjoLunaSite **sites);

/* 1 when the table lists title_id (the Games page's "hook available"), else 0. */
int vjo_luna_title_known(const char *title_id);

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

#endif
