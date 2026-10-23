# Native text hooks

A native text hook reads the line a game is about to draw, inside the game, so Rubi can show the game's
exact text instead of what OCR read, as the PC text hookers Textractor and LunaTranslator do. This page is
the technical design of the hook engine in Rubi's game worker: where it runs, what it hooks, the checks a
hook site must pass, what a hit does, and how it is undone. It changes nothing the game computes: a hook
copies bytes and lets the game's function run as before.

Status: built and host-tested. The current CALL source (string-copy hooks) was verified in CLANNAD
(`PCSG00415`) on the owner's Vita with the combined debug test build on 2026-10-20. Table-address hooks,
including CLANNAD's register site, remain unverified on a console; the register site was proven only in
Vita3K. The normal release build has no reported console run. The remaining console checks are in
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
| `auto` (default) | the CALL source in every enabled native Vita game, plus its available table sites that pass the checks below |
| `hooks` | the same attempts as `auto`; retained for existing configurations |

The Games page's hook-entry tag still means Rubi has a verified entry or a ported Luna table site
(`vjo_luna_title_ported`, core/hook_luna.h; audit-hooks-text N2). Seven titles in Luna's table have none,
every site of theirs needing code that is not ported (PCSB00985, PCSG00291, PCSG00451, PCSG00595, PCSG00663,
PCSG01081, PCSG01152): they have no tag, but both `auto` and `hooks` try the CALL source and refuse their
table sites (`unported`). Games without any table entry also get CALL discovery. An unavailable import
or a stream that never supplies matching dialogue falls back to OCR.

The kernel answers `vjoGetTextSource` with the Shell's value only in a process its loader started the
worker in as ON (it records the process as it decides, before the worker's `module_start` runs); every
other process reads `ocr`, so a worker that a leftover `*TITLEID` line loads into a game that is OFF
installs nothing (audit-hooks F1, [worker-loading.md](worker-loading.md)).

Nothing is installed in the PSP emulator's process in this version. The worker reports what it did in
every job's result (`hook_mode`; `hook_installed` = sites << 16 | CALL imports; `hook_refused`).

`hook_sites = off` in `config.ini` (the kernel's text source word then carries `VJO_TEXT_SOURCE_NO_SITES`,
include/vjo_api.h) installs no table site, neither Rubi's verified entries nor Luna's: only the CALL source,
in every enabled native Vita game with `auto` or `hooks`, so a
console run can tell the CALL source from the sites (audit-hooks F3). `hook_mode` is then the mode with that
flag, `log.txt`'s line says so (`hooks in PCSG00415: text source auto, sites off, 0 table sites and 8 string
copies hooked, 0 sites refused`), and the debug build's `hooks.log` says `sites off`. The worker takes the
word only when its low byte is a mode it knows and no other bit is set; anything else is `ocr`.

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
- `vjo_place_pointer_array` (audit-hooks F2): a copy that has a pointer array's shape is refused too: 20
  bytes or more, a multiple of 4, and every byte at index 3 mod 4 in 0x81..0x9F (the high byte of a user
  pointer, which is also a CP932 lead, so 0.6-0.75% of such structs pass the prefilter: the audit's
  measurement). Measured with the C function on every Japanese line of this repository (1,080 CP932 and
  1,335 UTF-8 lines of 8 bytes or more) and on 1,982 / 2,168 real visual-novel lines (vita-vn-ocr's
  realdata training labels), each copied exactly, with its NUL, and as every substring between character
  boundaries (64,136 to 144,750 per set): **0 refused**. It still refuses 6,982 of the 7,147 random
  user-pointer structs (2 to 128 words) that pass the prefilter; the 165 it lets through are 2 to 4 words.
  With a minimum of 8 bytes it refused real text: ソラユメ, 逢魔が刻, イヴァン, ジャンル, ダンベル, チンピラ,
  佐伯世羅, 森嶋永遠 (8 bytes each in CP932) and 18分前 in UTF-8, so the minimum is 20 (5 words)
  [measured: `$HERD/shared/artifacts/hooks-fix-engine/prefilter_false_refusals*.txt`];
- the text then goes to its caller's stream: the stream key is the caller's return address (LR). A new
  caller claims a free CALL slot (9-20). When all 12 hold other callers, it takes the one written least
  recently (a slot that never had text first) that is not kept, under that slot's try-lock: the slot's
  key, hits and text become the new caller's, so a job's snapshot sees another key for that ID; a busy
  slot, or every slot kept, drops the hit, never waiting. A stream that agreed with OCR, and a stream pinned by a
  diagnostic request, is kept (`vjo_hooks_keep`, from the job path): never given to another caller. An agreeing
  stream is kept only while its slot still holds the caller the job's snapshot saw (checked under the
  slot's lock), so a slot another caller took during the job is not kept for the new one. The job's
  result says `VJO_HOOK_R_CALLS_FULL` (0x40 in `hook_flags`, the debug log's `flags`) once a caller found
  every slot taken (`vjo_hooks_call_stats`: hits dropped for lack of a slot, callers that replaced one).

No syscall, no allocation and no waiting in this path: the slot's try-lock is taken or the hit is
dropped (and counted). Our own worker's string calls are not hooked: only the main module's imports are.

From the console run (note 23): in CLANNAD the text came from one caller of the removed code's
string-copy hooks ("CALL kind 2": any of its hooked memcpy, strcpy, strncpy and memmove imports and
SceLibKernel's copies; which one the note does not say) at absolute LR 0x8105910F, picked by hand; segment
0's base was not queried in that run, so its offset 0x5910F is unverified. Its texts were 24 → 6 → 18 →
72 → 18 bytes, so a 2-character text can come through a dialogue caller. Only the earlier emulator run's
caller 0x8102CA7F is named as a memcpy caller (note 23).

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
raw bytes each, with a try-lock, key, sequence stamp, hit count, FNV-1a hash, encoding and a kept mark
(a CALL slot no other caller may take). An identical
copy only counts a hit; a new text takes the next value of a shared clock. Decoding to UTF-8
(`vjo_luna_text` for a Luna site, `vjo_hook_decode` otherwise) happens at job time in the worker's
thread (`vjo_hooks_snap`, `vjo_hooks_text`), never in a hook. Which stream's text a job shows, and when,
is `core/hook_select.c`'s (hooks-select).

## Which text a job shows

At job time, `vjo_hooks_text` decodes the snapshot, applies a table site's game-specific filter when
present, then cleans common backslash controls in place (`core/hook_text.c`, `vjo_hook_clean`).
Numeric `\s` and `\w` commands, including their decimal arguments (`\s156`), are removed;
standalone `\c`, `\k`, `\p` and `\f` are removed, and literal `\n`, `\r` and `\t` become
whitespace. Unknown escapes and doubled backslashes are kept. Nonprinting ASCII controls other
than real tab/CR/LF are dropped. This runs for CALL and table-site streams, after decoding CP932,
UTF-8 or UTF-16, before comparison or display: the overlay, mined sentence, history line and its
`characters` count all use the cleaned text. OCR is unchanged. Existing history is not rewritten.

`core/hook_select.c`, called by the job path in `game_ocr/main.c`, in the worker's thread. With nothing
hooked in the game (`vjo_hooks_counts`: no site and no CALL import) a job is OCR's exactly as before: no
snapshot, no comparison. Otherwise a job begins with a snapshot of the streams: each stream that holds
text is copied out and decoded into a static 2,048-byte pool, each text at most 768 bytes (512 raw bytes
of double-byte CP932 whole); the stream the job's pick names (pinned or locked) first, then the newest
texts, so a pool that fills cuts the oldest. Then, by the request's `hook_pick`:

- **Pinned** (a stream ID in a diagnostic request): that stream's text, without recognition; OCR when it has none.
- **OCR only** (`VJO_HOOK_PICK_OCR`): OCR's text. The self-test's jobs (every line recognized, or the
  fallback forced) are OCR only whatever their pick.
- **Automatic** (`VJO_HOOK_PICK_AUTO`): OCR runs initially. An agreeing hook replaces its text at
  the existing normalized similarity threshold of 700/1000 and a minimum of four characters. Two
  different normalized dialogue lines from the same source establish confidence. Reopening the same
  dialogue, a duplicate job, or another background copy cannot establish confidence or advance checks.
  The next eight fresh readings include seven hook-only readings and one OCR comparison. A successful
  scheduled comparison grows the interval to 16, then 32, capped at 32. Failed jobs roll back their state.
- **Recovery:** one comparable disagreement retains the source but shows the existing OCR fallback and
  checks the next fresh line. A match resumes at eight; two consecutive comparable disagreements discard
  learning. Empty/short OCR is inconclusive and neither clears nor adds a pending disagreement. A missing
  or short hook temporarily uses OCR and requires confirmation when it returns; menus and blank screens
  do not erase a reliable identity. Changed source kind, offset or encoding must earn fresh confidence.

Learning is automatic, with no picker or extra setup. The cache holds at most 32 titles in a fixed
3,088-byte blob (source-derived file size, not a console memory measurement), plus bounded temporary and
backup files for recoverable replacement. Each entry stores the source kind, module-relative site or
caller offset, encoding, and learned interval. Its key includes the title ID, SHA-256 of the complete
executable, the applied text-source word, and a CMake-generated SHA-256 revision of hook definitions,
placement rules, filters and decoders. It never saves dialogue, stream slots or runtime addresses.
On the next launch, one fresh OCR agreement confirms the saved source and resumes its interval with a
fresh counter; this startup check does not promote the interval. A single startup disagreement follows
the same recovery rule. Missing, corrupt, inaccessible or unwritable storage means fresh learning.
Hashing and cache work run in the game worker; the existing kernel file thread handles its fixed paths
through the optional `vjoOcrHookCache` broker. There is no new Shell allocation or UI.

A longer interval delays detection: a learned source that starts returning incorrect text can show up
to 31 hook-only readings before the next scheduled comparison. A hook that is wrong between checks can
still pass a check; reliability is conditional, not guaranteed. Choose **OCR only** in the Rubi app and
restart the game to disable hooks ([Recovery](#recovery)).

The Shell's normal jobs always request Automatic. The popup has no hook labels or source picker;
source IDs, flags and scores stay in the debug log. The job protocol retains explicit stream and
OCR-only requests for diagnostics and the self-test.

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
not agree (so it never locks; once a stream is locked, its new text is checked as above), and a name in the
box agrees only beside a line about 2.3 times its length. The other side of the same rule: a stream that
agrees matches at least 70 % of OCR's characters, so the text shown may lack up to 30 % of the box, and
is then shown alone, after a lock without OCR too. A speaker's name beside the line is the intended case;
a box that grows by clicks is another: `「えっ」` and then a 25-character segment, whose stream holds only
the segment, scores 916, so the popup shows the segment without `えっ` (audit-hooks-text N3; 7 of 10
characters score 700, 6 of 10 600) [measured: `vjo_hook_similarity` on the host]. Characters past
the 256 compared count as unmatched, so a cut text never scores higher than it would whole. Fewer than 4
characters in either text: 0. Equal scores: the stream of the current streak (two callers copying one text
must not take turns), then a table site, then the newer text.

Every result says where its text came from: `text_from` (0 OCR, else the stream's ID), `hook_flags`
(`VJO_HOOK_R_*`), `hook_score` (the shown stream's similarity, else the best one's; 0 when nothing was
compared), `hook_streams` (bit ID − 1 of each stream that held text as the job began) and `hook_mode`,
`hook_installed`, `hook_refused` (`vjo_hooks_counts`), in failed results too (with `text_from` and
`hook_flags` 0: a failed result has no text).

Historical baseline (before adaptive validation and persistence): `core/hook_select.c` is 1,992 B of code at `-Os` (1,708 B before the lock's checks of F3); its
comparison's 2,562 B of arrays are on the worker's stack only while it compares (its frames at most
2,772 B: `vjo_hook_select_after` 1,112 + `score_of` 1,600 + `cmp_form` 40 + `utf8_next` 20; `-fstack-usage`,
vitasdk 2026.08, the worker's flags); the job path's static snapshot is 3,160 B of `.bss` (3,064 B before
F3's key and counts: 20 candidates of 24 B and two states of 28 B, `arm-vita-eabi-nm`), and the worker's
pages are unchanged by it [measured: hooks/fix-text c0ea49d's Vita jobs, debug 133,784 + 30,664 B and normal
125,076 + 30,656 B in 167,936 and 159,744 B]. The comparison's inner loop is 19 instructions a cell at `-Os`: a
dialogue box of 60 characters against a few streams is about 10^4 cells, under a millisecond at 444 MHz,
and the worst case, 20 streams of 256 characters against 256, 1.3 million cells, about 30-60 ms
[estimate: the instructions of the `-Os` disassembly at 0.5-1 cycle each, not run on a console].

## Release

`vjo_hooks_stop` stops taking hits, releases every hook newest first (CALL imports, then sites, in
reverse order of installation), then, whenever a hook was installed, always waits 2 ms, and returns
nonzero while a game thread is still counted inside a hook of ours: `module_stop` then fails and the
module stays loaded, so no code a game thread may still run in is unloaded. The wait also covers the
few instructions the in-flight count does not (the observer's assembly around its C call, a CALL
hook's prologue and epilogue): a thread there when the hooks were released has left after it.

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
| debug, with the job path and the [PSP probe](#psp-probe-debug-build), hooks/psp-probe | 133,392 B | 30,568 B | 167,936 B |
| debug, `feat/native-hooks` 9fa24c5 (the probe and hooks-api's text source import) | 133,464 B | 30,568 B | 167,936 B |
| normal, `feat/native-hooks` 9fa24c5 | 124,740 B | 30,560 B | 159,744 B |

[measured: the build log's module rows on hooks/engine 8aa79eb (local CI), both builds, with
origin/hooks/luna-port merged; the debug "before" is the same tree with the engine unreferenced, the normal "before" the budget
header's API 26 figure.] The data is the slots (20 x 548 B = 10,960 B) and the capture buffer (512 B);
the text is the engine and what it links of Luna's table (its sites, version gate and capture). The
SceShell and kernel modules do not change here. The job path decodes the streams' texts
(`vjo_hooks_text`), so it links the decoder (`core/hook_text.c` and its CP932 table) and Luna's filters
(`core/hook_re.c`, at `-Os`), and adds the selection (`core/hook_select.c`, 1,708 B at `-Os`) and its 3,064 B
static snapshot: the two job path rows [measured: the build log's module rows on hooks/select `4f9dca9` with
hooks/engine `dca6d5f` merged, both builds]. The PSP probe's row: hooks/psp-probe with feat/native-hooks `960ac20`
merged, against `960ac20`, whose normal build it leaves byte for byte as it was [measured, the build log's module rows].
The two `9fa24c5` rows: the PR's head as audited, where hooks-api's `vjoGetTextSource` import adds 72 B of
text to the debug build in the same pages [measured: the module rows of local CI's Vita jobs on 9fa24c5,
audit-hooks section 3].

Stack, on the game's thread: a site hit about 564 B at most (the observer's 328 B frame and up to 4 B to
align it, then `vjo_hooks_observe` 56, `vjo_luna_capture` 72, `mirror` 56 and `read_game` 48; the
memory-block syscall runs on the kernel's stack); a CALL hit about 80 B (the hook 24, `call_hit` 32, then
the prefilter or the stream's offer 24) [measured: the C frames by `-fstack-usage` of 9fa24c5's worker,
the same in both builds, the observer's from its code; audit-hooks section 2]. CPU time per hit, and any
CPU3 load, are unmeasured.

## Hidden line diagnostics

With the overlay open, press **Triangle** to replace the dictionary body with diagnostics for the
displayed text; press it again to return. The text and cursor remain visible, Up/Down still visit
history, and the right stick scrolls the details. The toggle is absent from the normal hints and
starts off each time the overlay opens, in both normal and debug builds.

`Source` says **OCR** or **Hook** for the job that supplied that text. Hook details include its stream
ID and table-site/CALL origin, agreement/lock/pin/no-OCR/unlock flags, similarity, available streams,
the worker's applied mode, installed/refused hooks, model and capture identifiers. Similarity zero
can mean no comparison, rather than a failed match. A hook-only answer says the OCR model did not run.
Cursor lookups and refreshes that keep the same text preserve its original provenance. A replacement
gets its own provenance; cancelled/dropped refreshes do not replace it.

Earlier lines show the engine saved in `history.csv`. Their detailed hook/capture information was
not saved and is explicitly unavailable; old records without an engine say **Unknown**. Returning
to the current text restores its full diagnostics.

## Logs

The normal build logs no hook text, as it logs no OCR text; with **Read text** on **Automatically**, both
builds keep the shown line in `ux0:data/VitaJPOverlay/history.csv`, a hook's as OCR's (engine `hook`
instead of `vocr`, and `line_source` `hook` instead of `ocr`: `shell/history.c`;
[ocr-history.md](ocr-history.md#line-source)). The debug build writes
`ux0:data/VitaJPOverlay/hooks.log` at each game start (rewritten): the title, the text source, the main
module's name, NID and segment 0, `APP_VER`, each site with its verdict, reason and first 16 bytes, and
each CALL import hooked or not. No text is in it. Job results carry the counts; the Shell's debug log
shows them (hooks-api).

## Recovery

If a game misbehaves with hooks:

1. Hold L while the game starts: the kernel loads no worker, so no hooks.
2. Turn the game OFF on the Rubi app's Games page. This also holds for a worker that a `*TITLEID` line kept
   in taiHEN's config by hand loads into the game: the kernel gives the text source only to a process
   whose worker its own loader started as ON, and answers every other process OCR, so that worker
   installs no hook (audit-hooks F1; `kernel/text_source.c`).
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
4. A game outside the tables with `auto` or `hooks`: only CALL imports in `hooks.log`; no slowdown that shows.
5. A Luna-listed game with `auto`: every site's verdict in `hooks.log`; a hooked site's text.
6. Quitting a game with hooks: the console returns to LiveArea, and the next game start works.
7. CLANNAD with `auto`, Automatically: record OCR invocation counts separately from host results.
   On a fresh cache, confirm two distinct lines, then seven hook-only readings and a checked eighth;
   successful scheduled checks step to 16 and 32. Close/reopen the unchanged seventh line and confirm
   the next new line is still the eighth, rather than resetting or promoting the interval.
8. Restart the same exact game build: one startup confirmation, then the saved interval with a fresh
   counter. A menu, blank screen, missing hook, or inconclusive OCR retains the learned identity and
   requires confirmation when comparable dialogue returns.
9. With Only when I press L + R, and with the subtitle strip: the same texts and confidence rules.
10. Check one comparable disagreement, then agreement: OCR fallback first, then resume at eight. Two
    consecutive disagreements on fresh dialogue must unlock. Inconclusive and unchanged repeated readings
    must not add or clear pending disagreements. The wider interval may show up to 31 wrong hook-only
    readings before detecting a source that starts returning incorrect text. Change the executable,
    text-source setting or definitions and confirm that old learning is not reused.
11. The popup shows text without hook numbers or source-status labels. Triangle does not change the
    text source. Normal jobs keep `hook_pick = VJO_HOOK_PICK_AUTO`; diagnostic jobs can still request
    a stream directly through the job protocol.

## PSP probe (debug build)

Status: built and host-tested; **not run on a console**. Nothing is hooked in a PSP game in this version.
The probe only reads and logs, so that one console run can answer whether PSP hooks (a later round) can
work: can the worker read PSP RAM, does the running game's ID come through, and do LunaTranslator's PSP
addresses land on code on a console. It follows section 5 of the PSP feasibility report (hooks-psp, on
[#230](https://github.com/bee-san/Rubi_for_vita/issues/230)); every address below comes from that
report's sources, none has been seen on a console.

**When it runs.** Only in the debug build (`game_ocr/psp_probe.c` is in no other build), only in the PSP
emulator's process (the worker's `psp_title`: ScePspemu is loaded, or the title is the PSP emulator's or
Adrenaline's), only when the text source is `auto` or `hooks`, and once per process: at the first OCR job
(the first reading of the screen: in the background with **Read text: Automatically**, else at the first
L + R; the self-test's OCR stages are OCR jobs too, while an Anki screenshot and the self-test's capture
probe are not). Adrenaline's XMB runs in the same process as the
game started from it, and a reading there would use the probe up with no game's ID, so start the game
from its own bubble, or set **Read text** to **Only when I press L + R** and press it first in the game. To
probe again, close the emulator (or the bubble) and start it again. The text source reaches the worker
through the kernel's `vjoGetTextSource` (`kernel/exports.yml` exports it, and the worker imports it:
`VJO_HAVE_TEXT_SOURCE_IMPORT` in `vita/CMakeLists.txt`), so the debug build's probe runs as described
here; it does nothing with `text_source = ocr`, or with a kernel of another API version, which the worker
reads as OCR.

**What it does.** In the worker's thread, at its lowest priority:

1. `taiGetModuleInfo("ScePspemu")`: its return code and the module's NID, which tells the ScePspemu
   build (3.60 `0x2714F07D`, 3.65-3.70 `0x3F75D4D3`, 3.71 `0xEA8C1AE2`, 3.72-3.74 `0x5459B715`, from the
   report). Then, at each of three Vita addresses of Adrenaline's view of PSP RAM (`CONVERT_ADDRESS(a) =
   0x68000000 + (a & 0x0FFFFFFF)`): `0x70000000` (PSP RAM's start), `0x70800000` (the user partition)
   and `0x73CDE000` (`SceAdrenaline`, the struct Adrenaline's PSP side shares),
   `sceKernelFindMemBlockByAddr` and `sceKernelGetMemBlockInfoByAddr`. It stops unless all three blocks
   are found, hold their address and are readable.
2. The 12 bytes of `SceAdrenaline.titleid` at `0x73CDE098`, read only when they lie inside a block found
   in step 1.
3. When Luna's PSP table lists that ID: at each of its sites (at most 16; 6 is the table's most), the four
   32-bit words from target − 8 to target + 4, each read only when it lies inside a found block. The
   target is Luna's PPSSPP address in the view, assuming the game's main module is where PPSSPP puts it,
   at `0x08804000`.

It never writes to the game's or the PSP's memory, makes no cache operation, calls nothing in ScePspemu,
allocates nothing, starts no thread and does nothing at `module_start`. The report's fourth step, timing
a 1 MiB read of the view, is left out: it matters only for a RAM scan (the report's design c), not for
the hooks the next round builds (design a), and it would read 256 times the bytes of steps 2 and 3
together, of a view whose layout (SceCompat's CDRAM "with hole") no console run has shown yet.

Luna's PSP table is in the debug worker as `core/hook_luna_psp.inc`, generated by `tools/gen_luna_psp.py`
from the vendored `third_party/LunaTranslator/ppsspp_1.cpp` (51884b6, the SHA-256 pin of
`gen_luna_vita.py`): for each of its 415 entries only the address and the title IDs. That is 354 IDs and
431 sites (an entry that names several IDs is a site of each), 3,515 bytes: each ID packed in a word as
its prefix's index (5 prefixes, `ULJM` ...) and its number. Which register, encoding and filter a site
has is in Luna's source, found by its address.

**Where.** Appended to `ux0:data/VitaJPOverlay/hooks.log`, which the worker rewrites at each game start
when the text source is not `ocr` ([Logs](#logs)); `tools/install_ftp.sh --collect` does not fetch it
yet, so copy it with VitaShell (FTP or USB). Each line is written before the probe's next read, so if a
read faulted the log would end at the step that caused it.

**How to read it.** Every line starts `psp probe 1:` (the 1 is the format's version); every number is raw,
in hex unless it counts something:

| Line | What it says |
| --- | --- |
| `start: text source S` | the probe ran; S is the text source (1 `auto`, 2 `hooks`) |
| `ScePspemu: rc R nid N` | `taiGetModuleInfo`'s answer and the module NID (above) |
| `block A: uid U info I base B size Z memtype M access X type T ok` | the block at Vita address A: U the block's UID (negative: none found), I `sceKernelGetMemBlockInfoByAddr`'s answer, then its base and size, memory type (`0xD0` cached, `0x80` uncached), access (`0x4` readable) and block type; `ok` or `bad` (step 1's check). One UID on all three lines: one block holds all of PSP RAM |
| `titleid A: <24 hex digits> "<text>"` | the 12 bytes at A and their printable part, up to the first NUL (other bytes as `.`); `outside the blocks`: not read |
| `luna ID: T sites, R read` or `luna ID: not listed` | Luna's entries for the ID (empty when the bytes are not an ID) |
| `site P at V: W0 W1 W2 W3` | Luna's site P (a PSP address), V its Vita address, and the words at V − 8, V − 4, V and V + 4 (MIPS instruction words, as loaded); `--------`: outside the blocks, not read |
| `stop: <why>` or `done` | the probe ended early, or finished |

Telling what the words are is left to a host script of a later round. For a first look: Luna's sites are
block starts in PPSSPP, so W2 is often a function's first instruction, `addiu sp, sp, -N` (`27BDxxxx`),
or the instruction after a call, which is then two words back in W0 (`jal`, `0Cxxxxxx`). Words like these
at most sites say the `0x08804000` base holds on a console; data, zeros or the same words at every site
say it does not, or that the view is not where the report expects.

**To test on a console** (hardware is paused; for #230), with the debug build and `text_source = auto`:

1. A PSP game Luna lists, under Adrenaline: ましろ色シンフォニー *mutsu-no-hana `ULJM05889` (one site,
   `0x08868AB8`), started from its bubble (When it runs, above). Wait for text, press L + R once. The game
   and the popup work as before; then copy `hooks.log`. Expected: `start`, the ScePspemu line, three `block` lines ending `ok`, `titleid ...
   "ULJM05889"`, `luna ULJM05889: 1 sites, 1 read`, one `site 0x08868AB8 at 0x70868AB8:` line, `done`.
   咎狗の血 True Blood Portable `ULJM05795` (one site, `0x08836918`) too, if at hand.
2. A PSP game Luna does not list, e.g. MEMORIES OFF `ULJM05334` (Luna names it only in a comment): the
   same up to `luna ULJM05334: not listed`, then `done`.
3. L + R again, several times: no second probe in the same emulator process.
4. With `text_source = ocr`: no `psp probe` line (no `hooks.log` at all).
5. If a game stops at the first L + R: send `hooks.log` (its last line is the step) and the crash dump
   (`--collect` fetches the dumps).

The answers decide the next round: a `bad` block or a fault means the worker cannot read PSP RAM this way
(the report's fallbacks: `ScePspemuConvertAddress`, or a small kernel syscall); a missing or wrong title ID
means the disc ID must come another way; function entries and return points at the sites mean Luna's
addresses can be patched as they are.

## Files

`game_ocr/hooks.h` (the interface), `game_ocr/hooks.c`, `game_ocr/hooks_observer.S`,
`core/hook_place.{h,c}`, `core/hook_stream.{h,c}`, `host/tests/test_hook_place.c`,
`host/tests/test_hook_stream.c`, `tools/hook_place_forms.txt`, `tools/gen_hook_place_fixtures.sh`; the
selection's `core/hook_select.{h,c}`, `host/tests/test_hook_select.c`, `host/tests/test_game_hooks.c` (the
job path) and `host/tests/hooks_none.c` (nothing installed, for the worker's other host tests).
The PSP probe: `game_ocr/psp_probe.{h,c}`, `core/psp_probe.{h,c}`, `core/hook_luna_psp.inc` (generated by
`tools/gen_luna_psp.py`), `host/tests/test_psp_probe.c`, `tools/tests/test_gen_luna_psp.py`.
History: PRs #7, #10, #12, #13, #16 and the code removed in c35ebf9.
