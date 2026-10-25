/* The game worker's native text hooks (docs/native-hooks.md): what game_ocr/hooks.c offers the rest of
 * the worker. Hooks exist only in a game's process, installed as the worker starts in that game (before
 * the game's main thread runs) and released when it stops; never in a safe boot, never in SceShell.
 *
 * Hook time (a game thread, inside the game's function) only copies bounded raw bytes into a stream's
 * static slot. Everything else (decoding, filters, comparing with OCR) happens at job time, in the
 * worker's thread, through vjo_hooks_snap and vjo_hooks_text. */
#ifndef VJO_GAME_OCR_HOOKS_H
#define VJO_GAME_OCR_HOOKS_H

#include <stddef.h>
#include <stdint.h>

#include "../include/vjo_game_ocr.h"
#include "../core/hook_luna.h"
#include "../core/hook_text.h"

#define VJO_HOOK_RAW_BYTES 512u /* a stream's raw text, at most (a hit copies no more) */

/* VjoHookSnap.kind */
#define VJO_HOOK_KIND_SITE 1u /* a table site (Luna's or Rubi's own) */
#define VJO_HOOK_KIND_CALL 2u /* a CALL caller: the game's string copies from one return address */

typedef struct {
    uint32_t id;       /* 1..VJO_HOOK_STREAMS (vjo_game_ocr.h) */
    uint32_t kind;     /* VJO_HOOK_KIND_* */
    uint32_t seq;      /* changes at each new text (an identical copy does not change it) */
    uint32_t hits;     /* hits so far, identical copies included */
    uint32_t key;      /* a site's segment-0 offset; a caller's return address minus segment 0's base */
    uint32_t encoding; /* VJO_HOOK_ENC_* of raw */
    uint32_t n;        /* bytes in raw */
    const VjoLunaSite *site; /* the site (its filter), NULL for a CALL caller or Rubi's own entry */
    uint8_t raw[VJO_HOOK_RAW_BYTES];
} VjoHookSnap;

/* Called once from module_start, after the safe-boot, candidate and another-copy checks: installs what
 * the text source word `mode` allows in this game (vjoGetTextSource's: a VJO_TEXT_SOURCE_* in its low
 * byte; with VJO_TEXT_SOURCE_NO_SITES no table site, only the CALL source as the mode says; an unknown
 * mode or flag is OCR) (title_id, 9 characters; psp: the PSP emulator's process, where nothing is
 * installed in this version). Never fails the module's start: a hook that cannot be installed is counted
 * as refused and logged. */
void vjo_hooks_start(const char *title_id, unsigned mode, int psp);

/* Called from module_stop: releases every hook, newest first. 0 when none can still be running in a
 * game thread (the module may unload); nonzero otherwise (module_stop then fails and the module stays). */
int vjo_hooks_stop(void);

/* What vjo_hooks_start did, for every job's result: the text source word applied (its mode, with
 * VJO_TEXT_SOURCE_NO_SITES when it applied; OCR carries no flag), sites hooked << 16 | CALL imports
 * hooked, sites refused. */
void vjo_hooks_counts(uint32_t *mode, uint32_t *installed, uint32_t *refused);

/* 1 when stream id is a table site hooked from a Rubi verified entry (core/hook_place.c: its module NID and
 * bytes checked at hook time), else 0. */
int vjo_hooks_verified(uint32_t id);

/* Worker thread only, never module_start or a game hook: SHA-256 of the complete main executable at
 * the path reported by modulemgr. Hashing relocated/patched runtime pages or a module NID would not
 * identify an exact build. An inaccessible, empty, oversized or unreadable executable disables cached
 * learning (0); fresh automatic selection still works. No allocation or retained executable bytes. */
int vjo_hooks_build_id(uint8_t out[32]);

/* Job time: stream id agreed with OCR, or the Shell pinned it: its CALL slot is never given to another
 * caller (audit-hooks F2). key: the stream's key as the job's snapshot saw it (VjoHookSnap.key): kept only
 * while the slot still holds that caller (a slot another caller took during the job is not kept for it),
 * checked under the slot's try-lock (busy: not kept this time); 0: whichever caller holds it (a pinned
 * pick). No effect on a table site, or on an id that is not a stream. */
void vjo_hooks_keep(uint32_t id, uint32_t key);

/* Job time: since the game started, with every CALL slot held by other callers: dropped, the CALL hits that
 * took no slot (the least recently written one not kept was busy, or every slot was kept; also a hit whose
 * slot went to another caller just before it wrote); replaced, the callers that took the least recently
 * written slot that was not kept (audit-hooks F2). */
void vjo_hooks_call_stats(uint32_t *dropped, uint32_t *replaced);

/* Job time: stream id's latest text, copied out under its slot's try-lock. 1 when copied; 0 when the
 * stream is empty, unknown, or busy (a game thread is writing it: try again at the next job). */
int vjo_hooks_snap(uint32_t id, VjoHookSnap *out);

/* The hook watch (API 41, game_ocr/main.c hooks_watch), the worker's thread between jobs: stream id's
 * sequence (0: no text yet, or no such stream), with its key and kind as vjo_hooks_snap gives them and
 * its raw length, read without the slot's lock, so a game thread writing it is never turned away for
 * this. A hint that the stream took a new line, which the job's snapshot then reads properly; a read
 * across a write may mix two texts' fields. */
uint32_t vjo_hooks_seq(uint32_t id, uint32_t *key, uint32_t *kind, uint32_t *bytes);

/* Job time: a snapshot's raw text as UTF-8, NUL-terminated, in out (cap bytes): vjo_luna_text for a
 * Luna site (its filter), vjo_hook_decode otherwise, then vjo_hook_clean for both.
 * Returns the bytes before the NUL. */
size_t vjo_hooks_text(const VjoHookSnap *s, char *out, size_t cap);

#endif
