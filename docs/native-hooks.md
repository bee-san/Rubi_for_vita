# Native text hooks

A native text hook reads the line a game is about to draw, inside the game, so Rubi can show the game's
exact text instead of what OCR read, as the PC text hookers Textractor and LunaTranslator do. This page is
the technical design of the hook engine in Rubi's game worker: where it runs, what it hooks, the checks a
hook site must pass, what a hit does, and how it is undone. It changes nothing the game computes: a hook
copies bytes and lets the game's function run as before.

Status: built and host-tested; **not run on a console** in this form. What has run on a console is the
older CALL source of PR #16 in CLANNAD (vita-vn-ocr note 23, below). Every console step is in
[To test on a console](#to-test-on-a-console).

## Where it runs

Only in a game's process, in the game worker `VitaJPOverlay_OCR.suprx` (`game_ocr/hooks.c`), which the
kernel loads into each game that is ON as the game starts ([worker-loading.md](worker-loading.md)):
never in SceShell, a system app, VitaShell or the Rubi app, never under `*ALL`, and nothing while L is
held at the game's start (AGENTS.md rules 1 and 2).

The worker's `module_start` calls `vjo_hooks_start(title, text source, psp)` after its existing checks:
not in a safe boot ([boot-safety.md](boot-safety.md): `safe_boot()` returns before anything is
installed), only in a candidate game, and only in the first copy of the worker in the process. It runs
before the game's main thread does. `module_stop` calls `vjo_hooks_stop()` first, then stops the
worker's thread.

What is installed depends on the text source (API 31, `VJO_TEXT_SOURCE_*`, read from the kernel with
`vjoGetTextSource`; the setting, the Shell and the kernel's syscall are described in the
[guide](guide.md)):

| Text source | Installed in this game |
| --- | --- |
| `ocr` | nothing (today's behaviour). Also the value before the Shell sets one, with an older kernel, and in a build whose kernel does not export `vjoGetTextSource` |
| `auto` (default) | only in a title with an entry (Rubi's verified list or Luna's table): its sites that pass the checks below, and the CALL source |
| `hooks` | as `auto`, plus the CALL source in every game that is ON (discovery) |

Nothing is installed in the PSP emulator's process in this version. The worker reports what it did in
every job's result (`hook_mode`; `hook_installed` = sites << 16 | CALL imports; `hook_refused`).

## The CALL source

`taiHookFunctionImport` on the main module's (the game's `eboot.bin`, checked by its path) imports of
these functions; a game that does not import one is simply not hooked there. The NIDs are the removed
`text/main.c`'s (c35ebf9^) and match vita-headers e66ebe9 `db/360/SceLibc.yml` and `SceLibKernel.yml`.

| Library (NID) | Functions (NID) |
| --- | --- |
| SceLibc (0xBE43BB07) | memcpy 0x7205BFDB, strcpy 0x85B924B7, strncpy 0x9F87712D, memmove 0xAF5C218D |
| SceLibKernel clib (0xCAE9ACE6) | sceClibMemcpy 0x14E9DBD7, sceClibStrncpy 0xC458D60A, sceClibStrlcpy 0x2CDFCD1C, sceClibStrnlen 0xAC595E68 |

Each hook calls the game's function first (`TAI_CONTINUE`), then looks at the bytes it has just written
(or measured, for strnlen), in the game's thread:

- a mem copy of 4 to 512 bytes, or a string whose NUL is within its first 512 bytes; anything longer is
  skipped;
- `vjo_place_text_prefilter` (`core/hook_place.c`), at most 16 bytes read: the bytes before the first NUL
  must be well-formed text in one encoding with at least two Japanese characters (CP932 kana or kanji
  lead bytes, or UTF-8 three-byte characters with leads E3-E9). A struct of pointers (0x81xxxxxx) or
  small integers fails it: its low bytes are control bytes or break the encoding;
- the text then goes to its caller's stream: the stream key is the caller's return address (LR), and
  a stream's ID is fixed for the game session once a caller has claimed it.

No syscall, no allocation and no waiting in this path: the slot's try-lock is taken or the hit is
dropped (and counted). Our own worker's string calls are not hooked: only the main module's imports are.

From the console run (note 23): in CLANNAD the text came from one memcpy caller at absolute LR
0x8105910F, picked by hand; segment 0's base was not queried in that run, so its offset 0x5910F is
unverified.

## Table sites

A table site is an address in the main module's segment 0 where the game holds a line in a register.
Two lists name them:

- **Rubi's verified entries** (`core/hook_place.c`): title, module NID, segment-0 offset, Thumb or ARM,
  register, encoding and 8 to 16 bytes recorded at the site. One so far: CLANNAD PCSG00415, NID
  0xC5187D60, +0x2CA70, Thumb, r6, CP932, `AB F8 2C 23 14 F5 53 70` (STRH.W r2, [r11, #0x32C];
  ADDS.W r0, r4, #0x34C). From PR #16's `profiles/PCSG00415.vjhook`; proven in Vita3K only (note 22).
  It is hooked only when the module NID and every recorded byte match.
- **Luna's table** (`core/hook_luna.h`, `core/hook_luna_table.inc`: LunaTranslator's Vita3K table,
  ported by hooks-luna-port; [hook-titles.md](hook-titles.md)). Luna's addresses are Vita3K's: the
  offset into segment 0 is `addr - 0x80004000` (`VJO_LUNA_SEG0`), which matches the CLANNAD Vita3K run
  but is unproven on a console. An entry is tried only when it is ported (`ported`) and its game
  version matches the game's `APP_VER` (`vjo_luna_version_ok`; `APP_VER` is read once from
  `app0:sce_sys/param.sfo`).

The site's offset must lie in segment 0 with the bytes taiHEN saves (16), and a Luna site must pass the
placement check (`vjo_place_check`), because taiHEN's offset hook (libsubstitute) overwrites 8 bytes at
the site (12 for a Thumb site at an address = 2 mod 4) and moves the instructions that start there into
a trampoline. The check accepts a site only when:

1. its first instruction is a function's entry that saves LR: Thumb `PUSH {..., lr}` (0xB5xx) or
   `PUSH.W {..., lr}` (0xE92D 0x4xxx), or ARM `STMFD sp!, {..., lr}` (0xE92D4xxx); Thumb when the
   address has bit 0 or 1 set, else what the bytes are;
2. every instruction that starts in the overwritten bytes is on a short allow-list of plain register
   and stack instructions: PUSH/PUSH.W, ADD/SUB of SP (or R7 from SP) with an immediate, MOV/MOVS between
   registers other than PC, MOVW/MOVT, STR/STRD/LDR with an SP or register base and an immediate, VPUSH.

Everything else is refused: any PC-relative form (LDR literal, ADR), a branch, IT, CBZ/CBNZ, TBB/TBH, PC
as an operand, or a form that is not on the list. Attempt 1 of this work found that libsubstitute
relocates a Thumb 16-bit LDR (literal) with its immediate unscaled, which is why the rule is an
allow-list rather than a list of what relocation handles. Host tests check it on 141 instruction forms
assembled by `arm-vita-eabi-as` (`tools/hook_place_forms.txt`, `tools/gen_hook_place_fixtures.sh`,
`host/tests/test_hook_place.c`). A refused site is counted with its reason (`VJO_PLACE_*`), and the
debug build logs its first 16 bytes, so a later build can add it as a verified entry.

At most 8 sites are hooked (streams 1-8), one per address, verified entries first.

### The observer

A site's hook is `vjo_hooks_observer_N` (`game_ocr/hooks_observer.S`, the removed `text/observer.S`
with its names changed), ARM code for ARM and Thumb sites. It saves r0-r12, the site's SP and LR, APSR
(with GE) and FPSCR in a 72-byte frame and d0-d31 below it (328 bytes of the game thread's stack), calls
`vjo_hooks_observe(slot, frame)` on an 8-byte-aligned stack, and restores every register, the flags and
the stack before it jumps to what C returned: taiHEN's next hook or the trampoline, as `TAI_CONTINUE`
would. `vjo_hooks_observe` passes the registers to the capture (Rubi's entry: the NUL-terminated string
at the register + padding; Luna's: `vjo_luna_capture`) with `frame[15]` set to the site's address.

The capture reads game memory only through `read_game`: `sceKernelGetMemBlockInfoByAddr` gives the
block that holds the address; it must be readable, and no read goes past its end (at most 512 bytes).
One hit captures at a time into a static 512-byte buffer (another thread's hit meanwhile is dropped and
counted), then into the site's stream.

## Streams

20 static slots in the worker's .bss (`core/hook_stream.c`): 1-8 for sites, 9-20 for CALL callers, 512
raw bytes each, with a try-lock, key, sequence stamp, hit count, FNV-1a hash and encoding. An identical
copy only counts a hit; a new text takes the next value of a shared clock. Decoding to UTF-8
(`vjo_luna_text` for a Luna site, `vjo_hook_decode` otherwise) happens at job time in the worker's
thread (`vjo_hooks_snap`, `vjo_hooks_text`), never in a hook. Which stream's text a job shows, and when,
is `core/hook_select.c`'s (hooks-select).

## Which text a job shows

`core/hook_select.c`, called by the job path in `game_ocr/main.c`, in the worker's thread. With nothing
hooked in the game (`vjo_hooks_counts`: no site and no CALL import) a job is OCR's exactly as before: no
snapshot, no comparison. Otherwise a job begins with a snapshot of the streams: each stream that holds
text is copied out and decoded into a static 2,048-byte pool, each text at most 768 bytes (512 raw bytes
of double-byte CP932 whole); the stream the job's pick names (pinned or locked) first, then the newest
texts, so a pool that fills cuts the oldest. Then, by the request's `hook_pick`:

- **Pinned** (a stream ID, the Shell's △): that stream's text, without recognition; OCR when it has none.
- **OCR only** (`VJO_HOOK_PICK_OCR`): OCR's text. The self-test's jobs (every line recognized, or the
  fallback forced) are OCR only whatever their pick.
- **Automatic** (`VJO_HOOK_PICK_AUTO`): OCR runs, and a stream whose text agrees with OCR's (similarity
  at least 700 of 1000) replaces it. Two agreements in a row by one stream lock it; a locked stream with
  new text since the last job shown (its seq) then answers a job without recognition, and one with
  nothing new lets OCR read (the screen changed without it: a choice menu, a CG). Two disagreements in a
  row unlock it. A job whose OCR text has fewer than 4 characters (an empty box) is no evidence: nothing
  changes, and neither does a job that fails.

A job a stream answers reads only the region's first capture pass (a strip at a time into one strip's
block, under the job's reserve, as the probe reads), so the kernel publishes its capture (`capture_seq`,
`done_seq`, the scene) as for an OCR job and the Shell's scene logic is unchanged; it opens no model and
finds and recognizes nothing. The shown text is normalised as OCR's is (`core/textnorm.h`), with, after
its NUL for the debug log, OCR's text (a stream replaced it) or the stream's own (no OCR ran).

The similarity: both texts in a compared form (full-width ASCII as ASCII, capitals as small letters,
half-width katakana as full-width with their voiced marks, 一 as ー; spaces, punctuation and symbols
dropped), at most 256 characters each; then d, the edit distance of all of the stream's text h to the part
of OCR's text o it matches best (o's characters before and after it free), and 1000 × (|h| − d) /
max(|h|, |o|). OCR's text around the match (a speaker's name, another line) lowers it as much as it is
long: a stream with one line of a two-line page, a backlog line or a line prepared ahead of the screen does
not agree, and a name in the box agrees only beside a line about 2.3 times its length. Characters past
the 256 compared count as unmatched, so a cut text never scores higher than it would whole. Fewer than 4
characters in either text: 0. Equal scores: the stream of the current streak (two callers copying one text
must not take turns), then a table site, then the newer text.

Every result says where its text came from: `text_from` (0 OCR, else the stream's ID), `hook_flags`
(`VJO_HOOK_R_*`), `hook_score` (the shown stream's similarity, else the best one's; 0 when nothing was
compared), `hook_streams` (bit ID − 1 of each stream that held text as the job began) and `hook_mode`,
`hook_installed`, `hook_refused` (`vjo_hooks_counts`), in failed results too (with `text_from` and
`hook_flags` 0: a failed result has no text).

Cost: `core/hook_select.c` is 1,708 B of code at `-Os`; its comparison's 2,562 B of arrays are on the
worker's stack only while it compares (its frames 2,712 B, `-fstack-usage`); the job path's static
snapshot is 3,064 B of `.bss`. The comparison's inner loop is 19 instructions a cell at `-Os`: a
dialogue box of 60 characters against a few streams is about 10^4 cells, under a millisecond at 444 MHz,
and the worst case, 20 streams of 256 characters against 256, 1.3 million cells, about 30-60 ms
[estimate: the instructions of the `-Os` disassembly at 0.5-1 cycle each, not run on a console].

## Release

`vjo_hooks_stop` stops taking hits, releases every hook newest first (CALL imports, then sites, in
reverse order of installation), waits 2 ms if a game thread is still inside a hook of ours, and returns
nonzero while one is: `module_stop` then fails and the module stays loaded, so no code a game thread
may still run in is unloaded. A thread between a hook's entry and its in-flight count, a few
instructions, is covered only by that wait.

## Memory

All static: no allocation, in a hook or anywhere in the engine. `param.sfo` is read once at the start
into the CALL slots' storage before any hook exists, then zeroed. The worker's module pages, in every
game that is ON, whatever its text source (`include/vjo_ledger_budget.h`):

| Build | Text segment | Data segment | Module pages |
| --- | --- | --- | --- |
| debug, before | 81,644 B | 15,752 B | 98,304 B |
| debug, the engine alone | 88,904 B | 27,496 B | 118,784 B |
| debug, with Luna's table linked | 99,016 B | 27,496 B | 131,072 B |
| normal, before | 80,344 B | 15,752 B | 98,304 B |
| normal, with Luna's table linked | 96,524 B | 27,496 B | 126,976 B |
| debug, with the job path (the decoder, Luna's filters, the selection) | 127,144 B | 30,560 B | 163,840 B |
| normal, with the job path (the decoder, Luna's filters, the selection) | 124,668 B | 30,560 B | 159,744 B |

[measured: the build log's module rows on hooks/engine 8aa79eb (local CI), both builds, with
origin/hooks/luna-port merged; the debug "before" is the same tree with the engine unreferenced, the normal "before" the budget
header's API 26 figure.] The data is the slots (20 x 548 B = 10,960 B) and the capture buffer (512 B);
the text is the engine and what it links of Luna's table (its sites, version gate and capture). The
SceShell and kernel modules do not change here. The job path decodes the streams' texts
(`vjo_hooks_text`), so it links the decoder (`core/hook_text.c` and its CP932 table) and Luna's filters
(`core/hook_re.c`, at `-Os`), and adds the selection (`core/hook_select.c`, 1,708 B at `-Os`) and its 3,064 B
static snapshot: the last two rows [measured: the build log's module rows on hooks/select `4f9dca9` with
hooks/engine `dca6d5f` merged, both builds].

Stack: a site hit uses the observer's 328 bytes and the C frames below it on the game's thread, an
estimate of under 1 KiB in all (not measured); a CALL hit, a few dozen bytes. CPU time per hit, and any
CPU3 load, are unmeasured.

## Logs

The normal build writes no hook text anywhere. The debug build writes
`ux0:data/VitaJPOverlay/hooks.log` at each game start (rewritten): the title, the text source, the main
module's name, NID and segment 0, `APP_VER`, each site with its verdict, reason and first 16 bytes, and
each CALL import hooked or not. No text is in it. Job results carry the counts; the Shell's debug log
shows them (hooks-api).

## Recovery

If a game misbehaves with hooks:

1. Hold L while the game starts: the kernel loads no worker, so no hooks.
2. Turn the game OFF on the Rubi app's Games page.
3. Set the text source to OCR only (`text_source = ocr` in `ux0:data/VitaJPOverlay/config.ini`, or the
   app's setting): no hooks in any game, from the next game start.
4. A boot that did not finish makes the next one a safe boot: no hooks anywhere
   ([boot-safety.md](boot-safety.md)).

## To test on a console

Hardware is paused; these are for the test issue (#230):

1. With `text_source = ocr`, a game starts and runs as before, and `hooks.log` is not written.
2. CLANNAD with `auto`: `hooks.log` shows NID C5187D60, the verified site +0x2CA70 hooked (or refused
   with its bytes), the CALL imports it has, and the game runs through several scenes.
3. CLANNAD: the CALL stream that holds the dialogue has key 0x5910F (LR minus segment 0's base).
4. A game outside the tables with `hooks`: only CALL imports in `hooks.log`; no slowdown that shows.
5. A Luna-listed game with `auto`: every site's verdict in `hooks.log`; a hooked site's text.
6. Quitting a game with hooks: the console returns to LiveArea, and the next game start works.
7. CLANNAD with `auto`, Automatically: the first pages show the CALL stream's text with OCR's agreement
   (the debug log's `hook_flags` AGREED and a `hook_score`), and from the third page on the stream answers
   without recognition (NO_OCR: no model load or recognize step, and the overlay's text sooner); the
   text matches the screen on every page, and the scene logic (a page's text cached, a new page read) works
   as with OCR.
8. A choice menu or a CG without text (the stream unchanged): OCR's text, or none, and the lock stays.
9. With Only when I press L + R, and with the subtitle strip: the same pages, the same texts.
10. A game where no stream follows the dialogue (or one that runs ahead of the screen): OCR's text on
    every page, nothing locked.
11. △ (hooks-api) pins a stream: its text, without OCR; OCR only: OCR's text, whatever agrees.

## Files

`game_ocr/hooks.h` (the interface), `game_ocr/hooks.c`, `game_ocr/hooks_observer.S`,
`core/hook_place.{h,c}`, `core/hook_stream.{h,c}`, `host/tests/test_hook_place.c`,
`host/tests/test_hook_stream.c`, `tools/hook_place_forms.txt`, `tools/gen_hook_place_fixtures.sh`; the
selection's `core/hook_select.{h,c}`, `host/tests/test_hook_select.c`, `host/tests/test_game_hooks.c` (the
job path) and `host/tests/hooks_none.c` (nothing installed, for the worker's other host tests).
History: PRs #7, #10, #12, #13, #16 and the code removed in c35ebf9.
