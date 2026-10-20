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
| debug, with the [PSP probe](#psp-probe-debug-build) | 105,312 B | 27,504 B | 135,168 B |
| normal, before | 80,344 B | 15,752 B | 98,304 B |
| normal, with Luna's table linked | 96,524 B | 27,496 B | 126,976 B |

[measured: the build log's module rows on hooks/engine 8aa79eb (local CI), both builds, with
origin/hooks/luna-port merged; the debug "before" is the same tree with the engine unreferenced, the normal "before" the budget
header's API 26 figure; the PSP probe's row: hooks/psp-probe against hooks/engine dca6d5f, whose normal build it
leaves byte for byte as it was.] The data is the slots (20 x 548 B = 10,960 B) and the capture buffer (512 B);
the text is the engine and what it links of Luna's table (its sites, version gate and capture). The
SceShell and kernel modules do not change here. The job path's decoder (`core/hook_text.c` and its CP932
table), Luna's filters (`core/hook_re.c`) and the selection add their own pages when the job path calls
`vjo_hooks_text` (hooks-select).

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
L + R; an Anki screenshot or a self-test job is not one). Adrenaline's XMB runs in the same process as the
game started from it, and a reading there would use the probe up with no game's ID, so start the game
from its own bubble, or set **Read text** to **Only when I press L + R** and press it first in the game. To
probe again, close the emulator (or the bubble) and start it again.

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
431 sites (an entry that names several IDs is a site of each), 5,265 bytes. Which register, encoding and
filter a site has is in Luna's source, found by its address.

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
`host/tests/test_hook_stream.c`, `tools/hook_place_forms.txt`, `tools/gen_hook_place_fixtures.sh`.
The PSP probe: `game_ocr/psp_probe.{h,c}`, `core/psp_probe.{h,c}`, `core/hook_luna_psp.inc` (generated by
`tools/gen_luna_psp.py`), `host/tests/test_psp_probe.c`, `tools/tests/test_gen_luna_psp.py`.
History: PRs #7, #10, #12, #13, #16 and the code removed in c35ebf9.
