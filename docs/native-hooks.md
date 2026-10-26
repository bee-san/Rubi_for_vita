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
  register, encoding and 8 to 16 bytes recorded at the site, and optionally a filter and a pointer read:
  - CLANNAD PCSG00415, NID 0xC5187D60, +0x2CA70, Thumb, r6, CP932, `AB F8 2C 23 14 F5 53 70` (STRH.W r2,
    [r11, #0x32C]; ADDS.W r0, r4, #0x34C). From PR #16's `profiles/PCSG00415.vjhook`; proven in Vita3K
    only (note 22).
  - アマツツミ PCSG01198 (APP_VER 01.00), NID 0x8AA413F2, +0x745C8, Thumb, r2, CP932, 16 bytes from
    `09 EB 87 0E` (ADD.W lr, r9, r7, LSL #2; ADD.W lr, lr, #0x1620). Luna's site 0x800785C8, which the
    placement check refuses as no entry (it is inside a function's loop); no branch in segment 0 targets
    its 8 patched bytes (checked on the decrypted eboot). Proven on a console (PCH-2000, 3.65,
    2026-10-23): hooked, its stream held text, and with OCR refused for memory the overlay showed the
    game's line.

  - PROTOTYPE engine titles from the psandvita Vita3K farm (proven in Vita3K only; no branch, PC use or IT in the patched bytes and no branch in segment 0 into them, checked on each dumped segment 0):
    - PCSG00007 リトルバスターズ！ Converted Edition, NID 0x8A335ECD, +0x1D15C, Thumb, r4 (deref), CP932, filter `RF_PROTOTYPE`; 562 farm pages
    - PCSG00206 グリザイアの果実 -LE FRUIT DE LA GRISAIA-, NID 0x4668F40F, +0x25FDE, Thumb, r4 + 10282, CP932, filter `RF_PROTOTYPE`; 3292 farm pages
    - PCSG00620 グリザイアの果実スピンアウト！？ アイドル魔法少女ちるちる☆みちる, NID 0x90E7EC14, +0x31E90, Thumb, r4 + 1066, CP932, filter `RF_PROTOTYPE`; 995 farm pages

  - IF:Lib GAME-ENGINE 3.00 titles from the psandvita Vita3K farm (proven in Vita3K only; patched bytes checked as above). Each one's Luna site at the same address is refused by the placement check, and Luna's filter there drops the player name that the hook sees before the game expands it:
    - PCSG00448 MARGINAL#4 IDOL OF SUPERNOVA, NID 0x84668EFE, +0x6D8F8, Thumb, r0, CP932, filter `RF_PCSG00448`; 497 farm pages
    - PCSG00469 CLOCK ZERO ～終焉の一秒～ ExTime, NID 0x31270870, +0x27A9C, Thumb, r0, UTF-8, filter `RF_PCSG00469`; 2047 farm pages
    - PCSG00853 ピリオドキューブ～鳥籠のアマデウス～, NID 0xA92335D4, +0x7DF2, Thumb, r5, UTF-8, filter `RF_PCSG00853`; 749 farm pages

  - LunaTranslator sites that Rubi's placement check refuses (not a function entry, or a window instruction off the allow-list), which the psandvita Vita3K farm proved at the same address and register (r12 + 8 in Luna's PCSG00745 entry; the farm's r5 is recorded). Proven in Vita3K only; patched bytes checked as above. Filter 0: Luna's filter at the address:
    - PCSG00468 黒蝶のサイケデリカ, NID 0x4DB3D205, +0x3F538, Thumb, r1, UTF-8, filter Luna's at its address; 412 farm pages
    - PCSG00696 アンジェリーク ルトゥール, NID 0x519F46E7, +0x87D1A, Thumb, r1, CP932, filter Luna's at its address; 127 farm pages
    - PCSG00745 スカーレッドライダーゼクス Rev., NID 0xC30CA4C1, +0x82D4, Thumb, r5, UTF-8, filter Luna's at its address; 410 farm pages
    - PCSG00808 悠久のティアブレイド -Lost Chronicle-, NID 0x8C990BE1, +0x3142A, Thumb, r10, UTF-8, filter Luna's at its address; 488 farm pages
    - PCSG00852 マジきゅんっ！　ルネッサンス, NID 0x8DFD228D, +0x7F75A, Thumb, r1, CP932, filter Luna's at its address; 278 farm pages
    - PCSG00916 Starry☆Sky ～Summer Stories～, NID 0x4813A011, +0x30114, Thumb, r0, CP932, filter Luna's at its address; 949 farm pages
    - PCSG00935 BLACK WOLVES SAGA  -Weiβ und Schwarz-, NID 0x8EA89B16, +0x541A2, Thumb, r0, UTF-8, filter `RF_IFENGINE` (Luna's at its address, PCSG00766, dropped every `#n`: 270 of the bucket's 382 pages lost their line breaks); 387 farm pages
    - PCSG01075 悠久のティアブレイド -Fragments of Memory-, NID 0x8ECBA36A, +0x31F44, Thumb, r10, UTF-8, filter Luna's at its address; 333 farm pages
    - PCSG01221 紅色天井艶妖綺譚 二藍, NID 0x1A9CF884, +0x3B554, Thumb, r4, CP932, filter Luna's at its address; 5 farm pages

  - Other engines from the psandvita Vita3K farm, Rubi's own sites (proven in Vita3K only; patched bytes checked as above):
    - PCSG00450 キャプテン・アース Mind Labyrinth, NID 0x28CB1B14, +0x6AB56, Thumb, r1 + 32 (deref), UTF-16LE, filter `RF_PCSG00450`; 78 farm pages
    - PCSG00509 POSSESSION MAGENTA, NID 0xA11A6741, +0x12C76, Thumb, r0, CP932, filter `RF_PCSG00509`; 1299 farm pages
    - PCSG00540 忠臣蔵46+1 V, NID 0xC595CF25, +0x2B90A, Thumb, r1, CP932, filter `RF_PCSG00540`; 1037 farm pages
    - PCSG00553 剣が君 for V, NID 0xCF234754, +0x11452B8, ARM, r1 + 12, UTF-16LE, filter `RF_BR_NEWLINE`; 1130 farm pages
    - PCSG00648 月に寄りそう乙女の作法, NID 0xE5819F44, +0x26EFA, Thumb, r2, CP932, filter `RF_ANGLE_RUBY`; 1397 farm pages
    - PCSG00713 百花百狼 ～戦国忍法帖～, NID 0x08B1647C, +0x112F46, Thumb, r6, UTF-8, filter none; 1464 farm pages
    - PCSG00787 鏡界の白雪, NID 0xA88A2E95, +0x21F1A, Thumb, r6, UTF-8, filter `RF_BACKSLASH_NEWLINE`; 502 farm pages
    - PCSG00807 Dance with Devils, NID 0x608A4999, +0xD68164, ARM, r1 + 12, UTF-16LE, filter `RF_BR_NEWLINE`; 690 farm pages
    - PCSG00817 EVE burst error R, NID 0x2DBAE009, +0x19132, Thumb, r0, CP932, filter `RF_PCSG00817`; 685 farm pages
    - PCSG00876 7'scarlet, NID 0x16FEB862, +0x12B60, Thumb, r6, UTF-8, filter `RF_PCSG00876`; 1051 farm pages
    - PCSG01194 参千世界遊戯 ～Re Multi Universe Myself～, NID 0xECD30306, +0x56E24, Thumb, r0, CP932, filter `RF_PCSG01194`; 30 farm pages

  - Found with the psandvita farm's Vita3K capture build, then tested on a console:
    - PCSG00700 蒼の彼方のフォーリズム (APP_VER 01.00, eboot SHA-256 d1d827ee…), NID 0x3BB91404 (`vita_app`),
      +0x74D48, Thumb, r1, CP932, filter `RF_PCSG00700`. The site is the entry of the function that sets a page's
      script string on the text window (a `strlen` probe found it: the page's one call returns to 0x81074d6a); it
      gets each page once (60 pages in a row in Vita3K). Patch (8 bytes): `push.w {r4-r11, lr}; sub sp, #0x1c;
      movs.w r10, #0`, no branch into it in the dumped segment 0. On a PCH-2000 (3.65, Rubi 0.9.7 with this
      entry, 2026-10-26): `hooks in PCSG00700: text source auto, 1 table sites and 3 string copies hooked, 0 sites
      refused`; of the site's 19 lines in two sessions, the 15 compared with OCR scored 1000, the ruby line 771
      before `RF_PCSG00700` had its ruby rule (then 1000), 3 scored 0.
  - From the farm's data bucket (the hooks of its finished titles, with every approved page's hook bytes:
    [farm checks](#farm-checks)), Rubi's own sites, proven in Vita3K only. The bucket has no segment-0 dump,
    so only the recorded 16 bytes were checked: no branch, PC use or IT among the patched instructions
    (disassembled with LLVM 18's `llvm-mc`). **Unchecked for these two:** a branch elsewhere in segment 0 into
    the middle of the patched bytes, which the entries above had checked on their dumps (one would crash the
    game when it is taken; the farm's dumps, on the owner's machine, can settle it):
    - PCSG00628 ALIA's CARNIVAL! サクラメント (APP_VER 01.00), NID 0x1EE45F02, +0x3373C, Thumb, r5, CP932, filter
      `RF_PCSG00628`; 1208 farm pages. Patch: `str r4, [r6, #0x2c]; movs r4, #0; str.w r9, [r6, #0xb4]`.
    - PCSG00380 真 流行り神 (APP_VER 01.00), NID 0x9A11E118, +0x4DE9E, Thumb, r15 + 0x98E152, the cells of
      foaf4's page record (`VJO_PLACE_READ_CELLS`), CP932, filter `RF_PCSG00380`; 1095 farm pages. The site is
      the per-frame message-window draw, so it is hit every frame: two reads of game memory (the count, the
      cells) and an unchanged text counts a hit only. Patch (12 bytes): `str r4, [sp, #0xa8]; movt r0,
      #0x819d; ldr r7, [r0]; ldr.w r12, [r0, #0x4]`. The record is a static (`.bss`), which the farm read at
      0x819DBFF0 with segment 0 at 0x81000000 (its r4, 0 at the site, + that address): r15, the site's own
      address, + 0x98E152 is that address wherever the eboot is loaded as it was linked. A game built with an
      SDK up to about 3.00 is a statically linked executable at 0x81000000 that cannot be relocated
      ([h-encore's write-up](https://github.com/TheOfficialFloW/h-encore/blob/85b345e28f38d728a7ac33a35b9d6f32db93e8fc/WRITE-UP.md));
      真 流行り神 came out in 2014. Were it relocated, the loader would also rewrite the `movt r0, #0x819d` in
      the recorded bytes (a static's address in the record's 64 KiB), so the signature refuses the entry unless
      it came out as in Vita3K. Every read stays inside one readable memory block (`read_game`): a wrong record
      gives no text, or one that OCR does not agree with, never a fault.

  An entry is hooked only when the module NID and every recorded byte match, and, when it names one, not in
  another game version than it was proven in: `app_ver` is the farm's param.sfo `APP_VER` (`0x0100` for
  01.00); another `APP_VER` refuses the entry as `version` (`vjo_place_verified_version`; `APP_VER` is read
  for such an entry also when the title has no Luna site). One that cannot be read leaves the entry to its
  NID and bytes, which pin the build anyway (an update changes the eboot, and so its NID); a Luna site that
  names its version is refused then, having neither. 25 of the 30 entries name 01.00; CLANNAD's, POSSESSION
  MAGENTA's, 忠臣蔵46+1 V's, 剣が君 for V's and 月に寄りそう乙女の作法's name none (the farm did not record it), so
  their NID and bytes alone decide. An entry at the address of
  a ported Luna site that this version may try takes that site's hookfunc and filter for its text
  (`vjo_luna_site_at`); the Luna site is then skipped as hooked. Two more fields, both 0 by default:
  `filter` names a filter for the entry's text, a Luna one (`VJO_LF_*`) or one of
  [Rubi's own](#rubis-filters) (`VJO_RF_*`), in place of the Luna site's filter at that address (whose
  hookfunc clean-up still runs first; `vjo_luna_text_filter`); `read` says how the text is read at the
  register + padding: the string there (`VJO_PLACE_READ_STRING`), through a 32-bit pointer there, one exact
  4-byte read, as Luna's `DATA_INDIRECT` (`VJO_PLACE_READ_DEREF`; an unreadable pointer gives no text), or a
  page record of 16-bit cells (`VJO_PLACE_READ_CELLS`: a 32-bit count at +0x14, the cells from +0x20, each
  a character's first CP932 byte and its second one or 0; exactly that many cells are read, at most 256,
  none past the record, and `vjo_place_cells` makes them CP932 bytes). The register may be r15, the site's
  own address: the padding is then a static's distance from the site. The host test `verified: every entry
  of the list` checks every entry's alignment, recorded bytes against the patch, register, encoding, read,
  filter and `APP_VER`; `the farm's excerpts through each verified entry` runs each entry the farm's bucket
  has pages for on its own recorded bytes ([farm checks](#farm-checks)). When OCR is refused for memory
  (`VJO_E_OOM`) with the automatic pick, a verified entry's newest text answers unconfirmed
  (`vjo_hook_select_no_memory`, `VJO_HOOK_R_UNCONFIRMED`), its capture pass beside
  `VJO_GAME_RESERVE_HOOK_ONLY`; Luna's sites and CALL callers never do.
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

### Rubi's filters

Rubi's own filters (`VJO_RF_*`, `core/hook_luna.c`) clean a verified entry's text with the same ops
as Luna's filters (literal replacements, the regex subset of `core/hook_re.c`, `remapkatakana`,
`Trim`). They come from the Vita3K screenshot farm in bee-san/psandvita: each code below was seen in a
hook's bytes there and compared with the native frame the game drew at that moment (its
`hooks/vita/<title>/RUBI-HOOK.md` and `hooks/hook-text-artifacts.jsonl`; event and sample numbers are
that farm's). **Proven in Vita3K only, not on a console.** Host test: `rubi_filters`
(`host/tests/test_hook_luna.c`), one example from the farm's bytes per rule.

| Filter | Titles | What it does | Farm evidence |
| --- | --- | --- | --- |
| `PROTOTYPE` | PCSG00007, PCSG00206, PCSG00620 | `･` to `・` (the middle dot between katakana words, not Luna's `…`); half-width kana to hiragana (Luna's map, as its PCSG01198 filter); `$t<name>$d` to the name on its own line, `$t<name>@` to the name; `$d` to a line break; `$[base$/reading$]` to the base; drops `$L1` `$A1` `$S<nnn>` `$S` `$w` `$C[..]`; `＿` to a blank; trims | PCSG00007 events 1-3; PCSG00206 event 367; PCSG00620 events 80-82, and its 12 `･` (page 375: セブンズ･カオス drawn セブンズ・カオス) |
| `IFENGINE` | PCSG00935 (with names: PCSG00448, PCSG00469, PCSG00853) | `#Ruby[base,reading]` to the base; drops `#Tag` and `#Tag[..]`; `#n` to a line break; trims | PCSG00448 events 1, 54; PCSG00853 sample 1; PCSG00935 event 1, all 382 of its bucket pages |
| `ANGLE_RUBY` | PCSG00648 (and PCSG00509) | `<base,reading>` to the base | PCSG00648 sample 5; PCSG00509 event 50 |
| `BR_NEWLINE` | PCSG00553, PCSG00807 | `<br>` to a line break; trims | PCSG00807 sample 2; PCSG00553 events 33-37 |
| `BACKSLASH_NEWLINE` | PCSG00787 | a lone `\` to a line break; trims | PCSG00787 event 1 |
| `PCSG00448` | MARGINAL#4 IDOL OF SUPERNOVA | `#Name[1]` 片岡, `#Name[2]` ユエ, then `IFENGINE` | event 208, sample 206 |
| `PCSG00450` | キャプテン・アース Mind Labyrinth | drops `《SP QUE,1,NNNN》` voice cues | script.dat (355 uses), event 4 |
| `PCSG00469` | CLOCK ZERO ExTime | `#Name[1]` 撫子, then `IFENGINE` | samples 80, 182, 1345 |
| `PCSG00509` | POSSESSION MAGENTA | `＊Ｂ` 鈴; `^` to a line break; `<base,reading>` to the base | events 50, 1011 (67 pages hold `＊Ｂ`) |
| `PCSG00540` | 忠臣蔵46+1 V | `c<id>.<name>r` to the name on its own line, narration's `r`, `v<voice>.` and `w<ms>.` dropped, `b<reading>.<base>` to the base, half-width kana (･ ﾞ ﾟ drawn … ！ ？); a 0xA0 byte is a blank cell, `　` | events 1-5, 51, 53, 55; 0xA0 on 170 of the bucket's 1037 pages (page 82: 東京都　陵南学園。, 9 glyphs) |
| `PCSG00628` | ALIA's CARNIVAL! サクラメント | ADV::MSGSET's SNR markup as `PCSG00540`'s without its `c<id>.`: `<name>r` to the name on its own line, `r` to a line break, `v<voice>.` dropped, `z<n>.` and `u<n>.` dropped (a size and a code point: in the farm's survey of `main.snr`, in none of its pages), `b<reading>.<base>` to the base (an `r` before the base is a line break), `[` `]` `\|` `k` `y` dropped, `!X` to the ASCII X (a lowercase X is read as a code first: none in the pages); half-width kana as `PCSG00540`; the 0xA0 blank | events 1, 20, 31, 89, 148, 207; all 1208 bucket pages |
| `PCSG00380` | 真 流行り神 | `㊤㊥㊦` (0x8785-0x8787, the pieces of a long dash) to `―` each; `㌫` `㍊` (0x876C, 0x876D, the player's family and given name) to the defaults 北條 and 紗希; trims (a page's last hit ends with a line break); the `\x04` button-icon codes are controls `vjo_hook_clean` drops | event 93016; pages 37 (6 glyphs: 北條紗希も、) and 158 (26) |
| `PCSG00700` | 蒼の彼方のフォーリズム | `<a>(X)` to X (a spoken line opens `<a>(｢)`); `<ruby>(n,reading)` and `<ruby2>(..)` dropped, leaving the base; any other `<tag>` dropped (the eboot's tag table also names `f`, `c`, `parallel`); half-width kana to hiragana | Vita3K: 31 of the first 60 pages; console: `<ruby>(2,しとう)四島市` drawn 四島市 (the other tags in no page seen) |
| `PCSG00817` | EVE burst error R | `[n]` to a line break; drops `[wNNN]` `[hNNN]` `[cRRGGBB]` `[e]` and the other `[x]` codes | events 1-59, 432 |
| `PCSG00853` | ピリオドキューブ | `#Name[1]` 花宮, `#Name[2]` かずは, `#Name[3]` カズハ, then `IFENGINE` | samples 78, 120, 259, 371, 464, 492 |
| `PCSG00876` | 7'scarlet | `@@` to a line break; drops `$<digit>` style switches | scn op 0x10 records |
| `PCSG01194` | 参千世界遊戯 | `<name>@r` to the name on its own line, `@r` to a line break, drops `@p<n>.` and `@v<voice>.`, half-width kana (･ ﾞ ﾟ drawn … ！ ？) | events 1, 11, 21-23 |

**CP932's 0xA0.** Shift_JIS leaves the single byte 0xA0 undefined; Windows' cp932 (and so Luna's filters)
reads it as U+F8F0, a private-use code point, and the decoder (`vjo_hook_decode`) does too: a filter turns it
into what its game draws there (the MSGSET titles: a blank cell, `　`), and `vjo_hook_clean` drops one no
filter mapped, as the decoder used to drop the byte.

**Player names.** Where a hook reads a line before the game expands its player-name code (`#Name[n]`,
`＊Ｂ`, foaf4's `㌫` `㍊`), Rubi cannot know the name the player typed. The filter writes the game's default name instead,
the one the farm kept at name entry and saw drawn. A player who renamed the character gets that line
with the default name: the popup then differs from the screen by that name only, and OCR agreement
(70 % similarity) still holds for most lines. Dropping the code would cut the name out of the sentence
instead. A per-game name setting could replace the default later.

### Farm checks

The farm keeps a data bucket (private: it holds the games' text) with, for each title it finished, its hook
(`hook/rubi-hook.json`: the site, module NID, the 16 bytes there, register, encoding, `APP_VER`) and every page an
AI reviewer approved against the frame the game drew, with the bytes the hook read
(`raw_text_events[].raw_hex`) and the text the farm made of them (`text_clean`). `tools/hook_farm.py` runs
Rubi's entries on them:

- `tools/hook_farm.py --bucket DIR check [TITLE ...]` builds `host/hook_farm_check.c` (the worker's capture
  rules and `vjo_hooks_text`'s path: the entry's read, the Luna site's text processing or the entry's filter,
  `vjo_hook_clean`, on the host) and compares Rubi's text for every recorded hit with the farm's, as shown:
  both normalised as the worker normalises what it shows (`core/textnorm.h`), white space at the two ends of
  the text dropped (Rubi's filters trim the first line's indentation and the blank lines around a page, the
  farm keeps them), the farm's U+F8F0 read as U+3000 and its C0 controls dropped (above). A page whose text
  differs only by a default player name is counted apart: the farm's text dropped the name code, and its
  own glyph counts show the name drawn. The worker shows a site's latest hit, so a page the farm built from
  several hits (a name, then the line; lines added one by one) is compared with its hits joined.
- `fixtures` writes `host/tests/fixtures/hook_farm.inc`: per entry, up to 5 hits of at most 128 bytes that
  Rubi turns into exactly the farm's text, chosen to cover the codes in them. The engine test `the farm's
  excerpts through each verified entry` installs each entry from Rubi's table in a fake main module (its NID,
  its bytes at its offset, its `APP_VER`), puts the hit's bytes where its register points, and runs the
  observer, the stream and `vjo_hooks_text`: 50 excerpts of 14 titles, short lines only, never a script.

Single-hit pages of the bucket of 2026-10-26 (04:36Z) whose text is the farm's, as shown, with main's
filters before this change and Rubi's now [measured: `tools/hook_farm.py check` on the host]:

| Title | Pages | Single-hit | Before | Now | Default name | Hits joined |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| PCSG00007 | 562 | 561 | 561 | 561 | | 1 of 1 |
| PCSG00380 | 1,095 | 1,092 | (no entry) | 1,060 | 32 | 0 of 3 (tutorial pages with a menu) |
| PCSG00468 | 412 | 403 | 403 | 403 | | 9 of 9 |
| PCSG00469 | 2,047 | 1,599 | 1,593 | 1,593 | 6 | 444 of 448 (the farm's blank lines) |
| PCSG00509 | 1,299 | 1,299 | 1,299 | 1,299 | | |
| PCSG00540 | 1,037 | 1,037 | 867 | 1,037 | | |
| PCSG00553 | 1,129 | 1,128 | 1,128 | 1,128 | | 1 of 1 |
| PCSG00620 | 995 | 972 | 960 | 972 | | 23 of 23 |
| PCSG00628 | 1,208 | 1,208 | (no entry) | 1,208 | | |
| PCSG00648 | 1,393 | 895 | 895 | 895 | | 498 of 498 |
| PCSG00713 | 1,463 | 188 | 188 | 188 | | 1,275 of 1,275 |
| PCSG00807 | 690 | 690 | 690 | 690 | | |
| PCSG00876 | 1,049 | 1,049 | 1,049 | 1,049 | | |
| PCSG00935 | 382 | 382 | 112 | 382 | | |

That is 12,465 of the 12,503 single-hit pages, and the other 38 differ by a default name only. Exactly, before
the shown form, 12,131 (the rest: full-width `！？` for `!?`, the ends' white space, the 0xA0 blank). The bucket's
other 19 Vita titles have no entry, for one of these reasons (the hook bucket's inventory: `rubi-bucket-hooks`
report):

- MAGES' SC3 script text (STEINS;GATE 比翼恋理のだーりん `PCSG00146`, ROBOTICS;NOTES ELITE `PCSG00352`,
  CHAOS;CHILD `PCSG00555`): the hook's bytes are glyph indices into a charset of 2,666 to 8,000 characters
  per game, which no read or filter here decodes.
- HuneX's page record (帝国海軍恋慕情 `PCSG00547`): the farm polled it from the game's
  `sceCtrlReadBufferPositive` import stub (`mvn r0, #0; bx lr` in the patched bytes) and decodes its glyph
  words with the game's own charset.
- A branch among the instructions the hook would overwrite (Vita3K hooks a block without patching it):
  `PCSG00367`, `PCSG00576`, `PCSG00611`, `PCSG00653`, `PCSG00666`, `PCSG00751`, `PCSG00753`, `PCSG00805`,
  `PCSG00854`, `PCSG00866`, `PCSG00914`, `PCSG01110`. A site next to it needs more than the 16 bytes the
  bucket recorded; Luna's table has other sites for some of them, which the placement check decides on the
  console.
- No module NID or bytes recorded: `PCSG00667` (LOVE:QUIZ, at Luna's own site 0x8003ACBA), `PCSG00917`,
  `PCSG00918` (Starry☆Sky Autumn and Winter, Luna's sites too).

### The observer

A site's hook is `vjo_hooks_observer_N` (`game_ocr/hooks_observer.S`, the removed `text/observer.S`
with its names changed), ARM code for ARM and Thumb sites. It saves r0-r12, the site's SP and LR, APSR
(with GE) and FPSCR in a 72-byte frame and d0-d31 below it (328 bytes of the game thread's stack), calls
`vjo_hooks_observe(slot, frame)` on an 8-byte-aligned stack, and restores every register, the flags and
the stack before it jumps to what C returned: taiHEN's next hook or the trampoline, as `TAI_CONTINUE`
would. `vjo_hooks_observe` passes the registers to the capture (Rubi's entry: what its `read` says at the
register + padding, the NUL-terminated string, the string its pointer there points at, or a record's cells;
Luna's: `vjo_luna_capture`) with `frame[15]` set to the site's address.

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
of double-byte CP932 whole). Automatic jobs keep Luna sites first, then the learned stream, then the
newest remaining texts, so generic traffic cannot fill the pool before Luna is decoded. Other jobs
keep their pinned stream first, then the newest texts. Then, by the request's `hook_pick`:

- **Pinned** (a stream ID in a diagnostic request): that stream's text, without recognition; OCR when it has none.
- **OCR only** (`VJO_HOOK_PICK_OCR`): OCR's text. The self-test's jobs (every line recognized, or the
  fallback forced) are OCR only whatever their pick.
- **Automatic, trusted** (API 43, `VJO_GAME_OCR_S_TRUST`, `hook_trust = on`, the default): before anything
  below, a trusted source with text answers with no OCR at all: Luna's sites and Rubi's verified sites from
  their first line, a regular source once it has agreed with OCR four times
  ([Trusted hooks](#trusted-hooks-api-43)). Without the bit (`hook_trust = off`) the bullets below are the
  whole rule, Rubi 0.9.7's.
- **Automatic, Luna** (`VJO_HOOK_PICK_AUTO`): nonempty text from an installed Luna site answers
  immediately, without initial OCR, periodic OCR or learned-source cache work. Installation still
  requires the existing port, game-version and site-placement checks. Short and unchanged text also
  skips recognition. With several nonempty Luna sites, the newest sequence wins (a tie goes to the
  lower stream ID). This does not identify a stream's semantic role, such as a speaker name versus
  dialogue; it trusts the existing game-specific extraction and filters. With `hook_trust = off`, Rubi's own
  table sites are not Luna sites and still require the regular validation below, even when they borrow a Luna
  filter (PCSG01198, #273); their verified low-memory fallback remains available. An empty, busy or filtered-empty Luna
  site is unavailable for this job: the remaining hooks and OCR provide the fallback.
- **Automatic, regular hooks**: without a nonempty Luna candidate, OCR runs initially. An agreeing hook replaces its text at
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
Luna does not establish, restore, advance or erase this learning while it supplies text. If it stops
supplying text, regular selection resumes its existing state or attempts the cache on first use.
Luna answers carry `SITE | NO_OCR`, without claiming an OCR agreement or learned lock.

The Luna distinction uses provenance bits in the candidate's existing `site` word. The fixed candidate
array, selection state and text pool keep their layouts and bounds; no allocation site or thread is
added [source inspection]. Packaged module pages and console timing are not measured locally for this
change; the existing build-time ledger still enforces the declared module budgets.

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

## Trusted hooks (API 43)

The owner's rule (2026-10-26): when a trusted hook supplies the text, no OCR at all. A job whose request carries
`VJO_GAME_OCR_S_TRUST` (bit 20 of its speed word, `0x100000`; the Shell sets it from `config.ini` `hook_trust`, on by default,
the Rubi app's **Trusted hooks replace OCR**) first asks `vjo_hook_select_trusted` (`core/hook_select.c`):

| Source | Trusted when | Why |
| --- | --- | --- |
| A ported Luna site | from its first line, as #274 already had it | its game version and placement were checked as the game started |
| A Rubi verified site | from its first line | its module NID and every recorded byte matched the game's code as the game started: the exact instruction the farm proved |
| A regular source (a string copy's caller, or a site without either check) | once its learned interval reaches 32 with a close agreement there: it agreed with OCR on two distinct lines (the lock) and passed both scheduled checks (at 8 and 16), four agreements in all, the last scoring at least 900 of 1,000 (`VJO_HOOK_TRUST_SCORE`; else a later check at 32 that does) | the measured choice below |

A trusted source with text (decoded and filtered, any length, new or the same as last time) answers the job: the
sites first, the newest text among them (a tie: the lower ID), then the learned source while a slot holds it (the
same kind, module-relative key and encoding). Such a job (`VJO_HOOK_R_TRUSTED | VJO_HOOK_R_NO_OCR`, with `SITE`
or `LOCKED`):

- opens no model, LM or cascade file and makes no OCR memory check, and (API 49) reads no pixels: the kernel
  publishes the trusted line's scene as its capture ([below](#no-wait-for-a-trusted-line-api-49)). Only when the
  region shows another scene does it read its one capture pass (API 43's), beside `VJO_GAME_RESERVE_HOOK_ONLY`
  (1 MiB) instead of the OCR reserve (4 MiB). So a game short of memory still gets a Luna or verified site's lines;
  a string copy earns trust only through OCR, so in a game that never has the OCR reserve free it is never trusted
  (the bench's tight rows refuse every one of its jobs, trust on or off);
- gives back the weights, LM and cascade model an earlier OCR job left resident (`drop_resident`) once trusted
  answers have run 8 jobs in a row (`TRUST_DROP_ANSWERS`): a game whose title screen was read by OCR before its
  first hooked line holds none of them while its hook supplies the text. Not at the first answer: a job between
  them that runs OCR (the stream busy, or its text filtered out, at that moment) would read them all again each
  time;
- moves no learning state, and once a trusted source has answered, a later OCR job that finds no line reads no
  weights ahead (no warm-up: [first-ocr.md](first-ocr.md)).

OCR resumes by itself, in the same job, when no trusted source has text: an empty, busy or filtered-empty site;
a learned source whose slot another caller took (that job also unlocks it, as before); a pending disagreement
(`misses`). The selection then goes on as before (`vjo_hook_select_before`, OCR, `vjo_hook_select_after`), and
such a job's disagreement takes the close agreement's mark back. A trusted source that has text is not compared
again: no job of it runs OCR, so in its session only its slot changing hands, or a job without its text whose OCR
then disagrees, ends its trust. Learning never stops: a regular source learns and is checked as in 0.9.7 until it is
trusted. The cache keeps the trust (API 49): it saves the interval 32 only for a trusted source (16 for one that
reached 32 without the close agreement, or with a pending disagreement), and a source restored at 32 (the same
caller of the same executable, the cache key's SHA-256) is trusted at once at the next launch, with no startup
confirmation (API 43 ran one OCR job a launch for it; `vjo_hook_select_before` still asks for it with
`hook_trust = off`). With `hook_trust = off`
(or any pick but the automatic one, the self-test's included) nothing of this runs: Rubi 0.9.7's selection, byte
for byte the same decisions (`host/tests/test_game_hooks.c`, `host/tests/test_hook_select.c`).

What trust gives up: a trusted hook's text is never checked against the screen. A screen the hook did not copy (a
choice menu, a backlog, a CG with the box hidden) shows the hook's last line, as a Luna site's always has, and a
trusted source that starts copying something else shows it until its slot changes hands. Choose
`hook_trust = off` to have every hook checked with OCR again (0.9.7), or **OCR only** to use no hook.

### Why four agreements (measured)

`tools/hook_trust_bench.py` replays game sessions through `host/hook_trust_bench.c`: the actual game worker, core
adapter and vita-vn-ocr runtime with the pinned models (VOCR5 Normal with Small below it, the 1 MB LM, the cascade to
Large within 2 s: the Shell's defaults; main's OCR, vita-vn-ocr `47cf3a3` with #307's head gate and two-level head
and #306's VOCR6 1 MB LM, and this branch's selection), one background job per new hooked line (the Shell's for API 41's new
line). A fake of `game_ocr/hooks.h` holds one stream, which takes each page's hook text in turn; the kernel's rows are
the page's pixels in the one region a player would draw for the game. The sessions: the first 300 pages of each of 29
Vita titles of vita-vn-ocr's hooked page pool (`vocr6-ingest` `pagepool/v1`, hook text verified against the drawn
glyphs; PCSG00653 has no dialogue region, and PCSG00007, PCSG00866 and PCSG00807 are left out: they share releases
with CLANNAD and vita-test, or are the pool's test title), 8,640 lines a configuration, 64 MiB free (3 MiB for the
tight rows). Four kinds of source: a Luna site, a Rubi verified site, a string copy's caller, and a caller one page
behind the screen (the backlog, a line prepared ahead), which must never be trusted [measured, host; no console
claim]:

| Source | `hook_trust = off` (0.9.7) | `hook_trust = on` |
| --- | --- | --- |
| Luna site | 0 OCR jobs | 0 OCR jobs |
| Rubi verified site | 1,620 OCR jobs (0.188 a line), 0.449 lines recognized a line, 17.5 CTC frames a line, the weights read 29 times; the worker's blocks 4,747 KiB at a job's peak and 4,487 KiB kept after it (means) | **0 OCR jobs**, nothing recognized or read, 0 KiB at a job's peak (API 49: no capture strip), 0 kept |
| String copy | the same as the verified site's | **656 OCR jobs (0.076 a line)**, 0.185 lines recognized a line, 8.0 CTC frames a line; 1,313 / 1,202 KiB; trusted in 27 of 29 sessions, from line 43 (median), 6,444 trusted answers, none wrong |
| The caller one page behind | 8,579 OCR jobs | the same: **never trusted**, no wrong trusted answer |
| Luna site, 3 MiB free | **every one of the 8,640 jobs refused**: its pass kept the 4 MiB OCR reserve | 0 refused |
| Rubi verified site, 3 MiB free | 603 refused (lines under 4 characters, the no-memory path's limit) | 0 refused |
| String copy, 3 MiB free | every one of the 8,640 jobs refused | the same: it earns trust only through OCR |

In no session did trust add an OCR job. The two string-copy sessions never trusted (PCSG00805, PCSG00854) are those
whose source kept disagreeing with OCR, so 0.9.7's recovery kept resetting it: they run as in 0.9.7. Which sessions
earn trust depends on the OCR, so a real source can sit at the 900 bar: on #325's OCR (vita-vn-ocr `817d203`, VOCR5
v0.3.0's files) 828 OCR jobs and 25 sessions, PCSG00468, PCSG00553, PCSG00805 and PCSG00854 never trusted
(PCSG00468's check at 32 scored 877); with #307's head gate 741 and 27, PCSG00553 and PCSG00854; with #306's LM as
above.
On the host a job that runs OCR takes 131 ms on average (#325's records); a line under 0.9.7's selection, where one
in five runs OCR, 26 ms; one without OCR well under 1 ms. On the console a CTC frame of VOCR5 Normal took 2.61 ms at
444 MHz (test.4, without the head gate, which now skips the output layer on about three frames in five), so 17.5 frames
a line is about 30-50 ms of recognition, before the finder, the model reads and the stable-text wait, against none for
a trusted line [estimate].

How many agreements, the same sessions:

| Rule for a string copy | Sessions trusted | OCR jobs (a line) | The caller one page behind |
| --- | --- | --- | --- |
| 2 agreements (the lock), without the 900 rule (#325, the OCR before #307) | 29 of 29 | 103 (0.012) | trusted in 2 sessions: 583 trusted answers, 455 wrong |
| 3 (the lock and the check at 8), without it (the same) | 29 of 29 | 276 (0.032) | trusted in 1 (PCSG00648): 180 answers, 127 wrong |
| 4 (the lock and the checks at 8 and 16), without it (the same) | 28 of 29 | 626 (0.072) | never |
| 2, 3, **4** with the 900 rule below | 29, 29, **27** | 184, 278, **656** | never |

The caller one page behind agrees only where a page shows the line before it and more (a box that grows by
clicks): 700-850 by coverage, rarely 900 or more (4 of its 51 agreements in #325's run). In most games the line
before is never on the screen with the new one, so it cannot agree at all: it agreed with OCR in 10 of the 29
sessions and locked in 2 (PCSG00648: 40 agreements, 62 answers while locked; PCSG00468: 2 and 8). So "never trusted"
tests the rule in those two sessions. A source with the screen's line agrees at 1,000 or close (82 % of the real
string copies' agreements at 900 or more). So the agreement that makes a source trusted, at interval 32, must also
score at least `VJO_HOOK_TRUST_SCORE` (900). The 900 was set after the first run's numbers, and the later runs replay
the same sessions, so they are not an independent check of it; four agreements are kept for that reason, although two
or three with the 900 rule were never fooled here either.

What the bench does not model, so the rule is untested against it: a caller that copies dialogue and also other
strings (names, choices, menu or system text, backlog entries); a correct source while the screen shows something it
did not copy (choice menus, the backlog, a hidden box: every bench page has a new hook line, so the cost of trusting a
stale line is unmeasured); a source ahead of the screen, or one that copies before the line is drawn; partial or
joined lines, name-only sources; garbled decoding; several callers competing for the CALL slots; and a source whose
behaviour changes after the first 300 pages.

## A new line starts the read

API 41 (for a trusted source API 49 removes the waits below: [No wait for a trusted
line](#no-wait-for-a-trusted-line-api-49)). A hook has the line as soon as the game copies it, but the Shell used to ask for it only once the
region's pixels settled (300 ms after the line finished typing: [text-stability.md](text-stability.md#when-a-read-starts-and-what-counts-as-stable))
or when the overlay opened, so a press soon after a line appeared waited for that read. Now the new line
itself starts the read:

- **The watch** (`game_ocr/main.c` `hooks_watch`). Each round of the worker's loop between jobs (its
  `vjoOcrTake` waits for a job at most 100 ms) reads the sequence numbers of the streams a job answers
  without recognition: Luna's sites that have held text, Rubi's verified sites once a trusted job (API 43) has
  read them, and the learned source while its slot still holds that source (`hk.sel.locked`, its key and kind). It reads them without the slots' try-locks
  (`vjo_hooks_seq`), so a game thread writing one is never turned away for it. A sequence newer than every
  text the last job's snapshot saw is a new line, reported once with `vjoOcrHookLine`, with its raw length;
  at most one report every 200 ms (`HOOK_LINE_GAP_US`), and a refused one is tried again at the next round.
  Nothing is watched before a source answers without recognition: a Luna site's first line and a learned
  source's first agreeing lines are read as before.
- **The kernel** (`kernel/game_ocr.c` `vjoOcrHookLine`: the registered worker's thread only, never while
  a job is claimed) gives the region a new scene at once, held while the line draws (`kernel/scene.c`
  `scene_hook`), stops the last capture standing for the screen (`region_seq` 0), sets `VjoState.quiet`
  while the line draws, and wakes the Shell with `VJO_EV_REGION_QUIET`. A line reported while the last
  reported one is still drawing (lines in quick succession: a skip, clicks through the typing) gets its
  scene but wakes nobody: it is read once the region settles, as without the watch, so a skip reads no
  line it passes (nor adds it to `history.csv`).
- **The Shell** is unchanged. As for any quiet screen, Automatically's background read (`shell/worker.c`
  `auto_prefetch`, a job `auto: screen quiet`) starts at once; the stream answers it with the first capture
  pass only, and its text and first word are kept for its capture's scene. That capture began after the
  report, so it is the held scene's (`scene_capture`), and L + R on that scene shows it at once
  (`open: background result` or `open: saved local word`).
- **The held scene.** The box clearing, the line typing out and the player skipping the typing keep the
  scene's number until the region settles, so the result still stands for the screen once the line has
  drawn. After that an icon-sized change keeps it too (the blinking "next" arrow, a mask that ended), and
  a larger one (a menu, a line without a hook) is another screen, read as before. A region that has not
  settled `SCENE_HOOK_DRAW_US` (10 s) after the report is held through icon-sized changes only. The next
  report, a region change or the game leaving the front ends it.

What does not change: which text a job shows ([above](#which-text-a-job-shows)). A scheduled check, a
confirmation, or a short or missing line still runs OCR, only sooner; such a job waits for the rows to hold
still before its first pass, as for any screen that has not settled (`VJO_GAME_OCR_F_SETTLE_FIRST`), so it
reads the whole line. **Only when I press** starts no background read, so a press there still starts its
own job.

When the text is ready [estimate, not measured on a console]: the report at most 100 ms after the copy,
then one job: its first capture pass (three game frames for CLANNAD's region; 56-73 ms a pass in test.3,
[auto-vs-press.md](auto-vs-press.md#what-settled-means)) and the Shell's lookup of the first word. That is
before most lines have finished typing, so the overlay opens on the line with its word already looked up.

Limits:
- A line reported more than 300 ms before the box starts to change settles as drawn on the unchanged
  screen; its typing is then another screen, read as before.
- A menu opened while the line is still drawing is held as the line's screen, until a change larger than
  an icon after the region has settled.
- Each new text of a watched stream is a report: a game that sends a line through the same caller in parts
  reports each part. The debug build's `kernel.txt` logs every report (`hook line: seq N, B bytes, scene S`),
  so a console run shows whether a game's lines arrive whole.

No block, thread or buffer: a few words of `.bss` in the kernel and the worker, and the code, within the
modules' declared pages (`tools/check_memory_ledger.py` fails the build otherwise).

## No wait for a trusted line (API 49)

The owner's rule (2026-10-26): a trusted hook is used as a desktop hook is. No OCR, no frame capture, no wait for
the screen to settle, no confirmation read, no typing wait: the line is there to look up as soon as the game writes
it. With `hook_trust = on` (the default) and a trusted source ([above](#trusted-hooks-api-43)), every wait between
the game's copy of a line and its lookup is gone but two: the worker's poll of the stream and the lookup itself.

- **The watch** (`game_ocr/main.c` `hooks_watch`), when the last user job carried `VJO_GAME_OCR_S_TRUST`: a trusted
  source's new line is reported at once, `vjoOcrHookLine` with `VJO_HOOK_LINE_TRUSTED`. A trusted source here: a site
  that answers without recognition (a Luna site, or a Rubi verified one) from its first line, which no job need have
  read, and the learned source once `vjo_hook_select_trusted` would answer from it (a restored one by its identity,
  its caller and kind, before a job has found its slot). No 200 ms gap: instead the same text again (decoded as a job
  would show it, from that stream or another watched one) is no new line, nor is a write its decoder leaves empty
  (a name the filter drops). A stream a game thread is writing, or a job in the way, is read at the next round. While
  the game has a table site, or a trusted learned source, the loop polls every `HOOK_WATCH_US` (16 ms) instead of
  100 ms: the worker asks `vjoOcrTake` for that wait in the request's `seq` (API 49; at least 4 ms, never over 100).
- **The kernel** (`kernel/game_ocr.c`): a trusted line wakes the Shell even while the last line still draws (API 41
  woke nobody then, so a skip read no line it passed), and its scene is `VjoState.hook_scene` while the region shows
  it (the tracker keeps a hooked line's number while it draws and through an icon's changes). A trusted answer's job
  publishes its capture with `vjoOcrCaptureRows`' `VJO_ROWS_HOOK`: that scene, with no rows armed, no frame waited
  for and no signature, so the result stands for that scene alone. Once the region shows another scene (a menu over
  the line, an untrusted line since) the kernel refuses it and the worker reads its first pass as before.
- **The Shell** (`shell/worker.c`): a hook scene is read at once, past the background read's pacing
  (`background_throttled`: the subtitle strip's 1 s between jobs, a failure's backoff, the retry after a screen that
  changed under a job) and, with the overlay open, past the refresh's (`background_open`: 2 s between refreshes, 1.5 s
  after the reader's last lookup): the new line replaces the text at once. A refreshed text that is the shown one and
  more (a line that grew) keeps the cursor where it was.
- **History** (`shell/history.c`): the line is recorded when the job that read it ends, as before, which is now
  right after the line. A line that grows (a hook that gives its text as it types, each part read at once: its text
  the last record's and more, the same game, within 750 ms of the last record) replaces that record, with the same ID
  and time, instead of adding one; the reading total counts it once (`core/history_lines.c`
  `vjo_reading_scope_grow`). The same text twice in a row is one record, as before.
- **The trust is kept** between launches (`core/hook_select.c`, [above](#trusted-hooks-api-43)): a restored trusted
  source needs no confirmation OCR.

`hook_trust = off`, or a source that is not trusted: API 41-43 as they were (the 100 ms take, the 200 ms gap, no
wakeup while a line draws, the capture pass, the Shell's pacing).

### How fast (measured)

`tools/hook_latency_bench.py` replays sessions through `host/hook_latency_bench.c`: the actual game worker (its loop,
the watch, the job path and the selection), the actual kernel transport and change detection (`kernel/game_ocr.c`,
`kernel/scene.c`, and `kernel/capture.c`'s signature of a frame the bench draws: a dialogue box that types each line
at 30 characters a second, a "next" icon that blinks once it is typed) and the actual Shell client
(`shell/game_ocr_client.c`), with the Shell's control and network threads as a model of `shell/worker.c`'s rules for
each build (`host/tests/test_shell_worker.c` tests the real ones). Every thread runs on one virtual clock that moves
only where the code waits (an event flag's wait with its timeout, a delay, the game's 60 fps frames); code takes no
time, and the first word's lookup is the console's fit, 70 ms (performance.md). The source: the learned string copy,
trusted (CLANNAD's kind), or a Rubi verified site. The sessions: the first 300 pages of the 30 Vita titles of the
hooked page pool the trust bench uses, one hook line a page at a reader's pace (the typing, then 1.5 s and 120 ms a
character), 8,895 lines a row (each session's first line, and 15 pages that repeat the page before, which the stream
stores as no new text, are left out) [measured, host; no console claim]. From the game's copy of the line to its
text and first word ready for the screen that shows it, so that L + R opens on it at once (or, overlay open, shown
in place with its lookup):

| Scenario | Before (main `b6c715c`, API 47) | API 49 |
| --- | --- | --- |
| Reading, overlay closed | 187 ms (median 187, 95 % under 220, worst 2,970) | **78 ms** (79, 85, 86) |
| The player clicking on, a line every 0.5 s | **8,832 of 8,895 lines never read** (read only once the player stops) | **every line: 80 ms** (82, 86, 86) |
| The subtitle strip on (Select + R) | 188 ms | 78 ms |
| Overlay open (auto-advance): the text shown in place | 117 ms; with its lookup 187 ms | **8 ms**; with its lookup 78 ms |
| A verified site's first line of the session | 1,786 ms (1,770, 2,720, 3,820) | 78 ms |
| A line given as it types (4 parts 60 ms apart), from its last part | 1,469 ms | 107 ms (107, 112, 113) |

Before, a line waited for the watch's report (58 ms on average: the take's 100 ms wait), then the job's capture pass
(three frames for the region), then the lookup; a line written while the last one drew (clicks) woke nobody, and a
site's first line, or one that grows, waited for the pixels to settle. With API 49 the report comes 8 ms after the
copy on average (16 at most), the job's text with it (no frame waited for), then the lookup's 70 ms: the poll and the
lookup are all that is left.

**The poll.** The same sessions, reading, with the watch's pace built 4, 8, 16 and 33 ms (`vjo-hook-latency-bench-*`):

| `HOOK_WATCH_US` | Ready | The worker's rounds a second |
| --- | --- | --- |
| 4 ms | 72 ms (worst 74) | 250 |
| 8 ms | 74 ms (78) | 125 |
| **16 ms** | **78 ms (86)** | **63** |
| 33 ms | 87 ms (103) | 30 |
| main, API 47 (the 100 ms take) | | 10 |

A round (two syscalls and the watch's reads) took 1.9 µs on the host, at every pace; on the console it is two
syscalls and a thread wakeup each, not measured. 16 ms is one 60 fps frame: a line is on the screen no sooner than
its frame, and a faster poll saves 4-6 ms of the 78 for two to four times the wakeups in the game's process.

Limits:
- A line the game writes before it draws it is in the overlay before (or while) it shows on the screen.
- With the overlay open, a new line replaces the text at once, also while the reader looks up words of the line
  before.
- A text that comes back (the same line after another text, or a backlog that copies it again) is a new line again;
  only the same text twice in a row is not.
- The poll wakes the worker 63 times a second instead of 10 while a game has table sites or a trusted source;
  that cost is not measured on a console.
- History: a line given in parts more than 750 ms apart is two records.

No block, thread or buffer: a few words of `.bss` (the kernel's `hook_scene`, the worker's trust and the last
reported text's hash, the Shell's newest record's place: 32 B of data), and the code, within the modules' declared
pages ([memory-ledger.md](memory-ledger.md)).

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
the same in both builds, the observer's from its code; audit-hooks section 2]. With the page-record read,
`vjo_hooks_observe` is 56 B (48 B on main 0b1a45b), and a verified entry's hit adds `read_game` 48 or the
stream's offer, under that maximum [measured: `-fstack-usage`, tools/fast_gate.sh's vitasdk image, both
builds]. CPU time per hit, and any CPU3 load, are unmeasured; PCSG00380's site is hit every frame.

## Hidden line diagnostics

With the overlay open, press **Triangle** to replace the dictionary body with diagnostics for the
displayed text; press it again to return. The text and cursor remain visible, Up/Down still visit
history, and the right stick scrolls the details. The toggle is absent from the normal hints and
starts off each time the overlay opens, in both normal and debug builds. Its first line is the total
characters read, characters/hour and total active reading time across every game's lines in
`history.csv`, with a 15-second AFK cutoff (`counting…` until the background count completes:
[ocr-history.md](ocr-history.md#total-characters-read)).

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
shows them (hooks-api). API 41: the debug build's `kernel.txt` has a line for each new line the hook watch
reports, its sequence, raw length and held scene (`hook line: seq N, B bytes, scene S`, with `, no wakeup`
for a line reported while the last one was still drawing), no text.

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
11. API 41, a new line starts the read (CLANNAD, Automatically, the debug build): once the source is
    learned, each new line gives one `hook line` in `kernel.txt` (its byte counts show whether lines arrive
    whole), followed by an `auto: screen quiet` job that the stream answers; L + R pressed as the line
    finishes typing opens on it at once (`open: background result` or `open: saved local word`, not
    `background job still running`). A menu opened after the line has drawn, then L + R, shows the menu's
    text as before.
12. The popup shows text without hook numbers or source-status labels. Triangle does not change the
    text source. Normal jobs keep `hook_pick = VJO_HOOK_PICK_AUTO`; diagnostic jobs can still request
    a stream directly through the job protocol.
13. API 43, trusted hooks (the debug build, `hook_trust = on`, the default), CLANNAD with Automatically: a job
    the verified site +0x2CA70 answers says `flags 0x118` (TRUSTED | SITE | NO_OCR) from its first line; the
    string-copy source, when it answers instead, agrees with OCR on four lines (`flags 0x01`/`0x03`, then `0x0A`
    between checks) and then says `flags 0x10A` (TRUSTED | LOCKED | NO_OCR). A trusted job's step log has no LOAD,
    CHECK or RECOGNIZE step and its job line `lines=0`; after 8 of them in a row `resident=0`. A screen the hook did
    not copy (the backlog, a choice menu) shows the hook's last line; with `hook_trust = off` the flags have no
    `0x100` and OCR runs now and then. In a game short of memory (アマツツミ): `flags 0x118` from its first line,
    no refusal.
14. API 49, no wait for a trusted line (the debug build, CLANNAD, Automatically, once step 13's source is trusted):
    each new line gives `hook line: … trusted` in `kernel.txt`, then a job `auto: screen quiet` whose capture is
    `trusted hook's capture, scene N` (no `first pass in`), all before the line has finished typing; L + R at once
    opens on it with its first word looked up. Click through several lines quickly: each one is read (a `hook line …
    trusted` and a job each), and the overlay shows the last. With the overlay open and the game on auto: each line
    replaces the text as it starts. Close the game and start it again: the first line is trusted at once (no OCR job
    before it). `history.csv` has one record a line.

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
job path) and `host/tests/hooks_none.c` (nothing installed, for the worker's other host tests); API 43's bench,
`host/hook_trust_bench.c` and `tools/hook_trust_bench.py`.
The farm checks: `tools/hook_farm.py`, `host/hook_farm_check.c`, `host/tests/fixtures/hook_farm.inc` (generated),
`tools/tests/test_hook_farm.py`.
The PSP probe: `game_ocr/psp_probe.{h,c}`, `core/psp_probe.{h,c}`, `core/hook_luna_psp.inc` (generated by
`tools/gen_luna_psp.py`), `host/tests/test_psp_probe.c`, `tools/tests/test_gen_luna_psp.py`.
History: PRs #7, #10, #12, #13, #16 and the code removed in c35ebf9.
