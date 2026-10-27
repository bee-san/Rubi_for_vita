# The memory ledger

Every byte our three plugins take from a Vita memory pool is accounted for in one place, checked
in CI, and printed at build time and at run time. The pools themselves, their sizes and who else
uses them are in [docs/vita-memory.md](vita-memory.md); why this exists is
[issue #59's root cause](issue-59-root-cause.md).

## SC3 text: MAGES' script strings (no API change)

STEINS;GATE 比翼恋理のだーりん, ROBOTICS;NOTES ELITE and CHAOS;CHILD get verified entries whose text is decoded with
each game's charset ([native-hooks.md](native-hooks.md#sc3-text)). **One new block, in those three games only; the
same module pages in every build.**

- **`VjoGameSc3`** (pool "game", `VJO_BUDGET_GAME_SC3` 16,384 B): the game's charset file, read through the kernel's
  file broker at the first job of a game with an SC3 site hooked and held until the worker stops, `USER_RW`,
  page-rounded: 8,192 B for STEINS;GATE (5,364 B) and ROBOTICS;NOTES ELITE (6,432 B), 16,384 B for CHAOS;CHILD
  (16,032 B); taken only when the game has `VJO_GAME_RESERVE_HOOK_ONLY` (1 MiB) free beside it. It is held beside
  every job, so the job's worst case grows by it: 10,403,840 → **10,420,224 B** of the 10,604,544 B limit
  (184,320 B of headroom). In every other game nothing is taken.
- **The modules**, measured with VitaSDK GCC 15.2.0 in `tools/fast_gate.sh`'s image (the build log's module rows,
  text + data/bss), main `892cfb9` against this change:

  | Module | Debug: main → this change | Normal: main → this change | Loaded pages |
  |---|---:|---:|---:|
  | Game worker | 160,888 + 35,328 → 162,088 + 35,624 B | 151,988 + 35,320 → 153,204 + 35,608 B | 200,704 / 192,512 B, unchanged |
  | Kernel | 43,096 + 77,764 → 43,292 + 77,764 B | 37,992 + 76,076 → 38,188 + 76,076 B | 122,880 / 118,784 B, unchanged |
  | Shell | 101,988 + 73,200 → 101,996 + 73,200 B (the self-test's name of the block) | unchanged | 176,128 / 163,840 B, unchanged |

  The worker's text is `core/hook_sc3.c` at `-Os` (the span and the decoder), the SC3 read and decode in
  `game_ocr/hooks.c` and the load in `game_ocr/main.c`; its data, the three verified entries (120 B) and the three
  charsets' pins (144 B) in the data segment ("Tables in the data segment" below) and their state. It keeps 1,752 B
  (debug) and 2,444 B (normal) of its last text page, 1,240 / 1,256 B of data. The kernel's 196 B are the open's
  path from the caller's title (`loader_title`) and its size rule; no buffer: the charset crosses the existing
  32 KiB staging buffer in one read.
- **Not added:** the charsets compiled into the worker (27,732 B of tables: about seven more pages in every game that
  is ON [estimate]), or read in the Shell (no room) or by the Rubi app (not running beside a game).

## Why the overlay waits (#358)

The open overlay's wait hint ([text-stability.md](text-stability.md#why-the-overlay-waits-358)) is Shell code and
data only: the choice of status as a try starts (`shell/worker.c` `start_job_kind`), the count's reset when a region
is saved (`on_command`), `ocr_wait_hint`'s parse (it
shares `ocr_boost_clock`'s branch: `core/config.c`) and its field in each `VjoConfig` copy. The hint (153 B) and the
key (14 B) are in the data segment (`VJO_DATA_TABLE`), and so is "Recognizing…" now (15 B), which gives the Shell's
code that much room back. No block, thread, stack or allocation; no budget raised. Measured with VitaSDK GCC 15.2.0
in the pinned image (`tools/fast_gate.sh`'s; `tools/check_memory_ledger.py`'s module rows, text + data/bss), main
`2f68854` against this change (the same Shell rows on `892cfb9` and `48d6a3b`):

| Module | Debug: main → this change | Normal: main → this change | Loaded pages |
|---|---:|---:|---:|
| Shell | 101,988 + 73,200 → 102,092 + 73,408 B | 89,884 + 73,232 → 89,980 + 73,416 B | 176,128 / 163,840 B, unchanged |
| Kernel, game worker | unchanged | unchanged | unchanged |

The normal Shell keeps 132 B of code room (228 B before) and 312 B of data room (496 B); the debug Shell 308 B and
320 B.

## Line finder fixes (API 50, vita-vn-ocr #129) and the pin at f8a1856

**The game worker, in the game's partition only: two text pages more in each game that is ON, in both builds**
(raised in `include/vjo_ledger_budget.h`).

- **The line finder's fixes.** `vocr_lines_fix.c` replaces `vocr_lines_rows.c` in the worker, with the policy's
  flags and the request's bits. On main `0b1a45b` that measured +1,264 B of debug text (148,424 → 149,688 B) and
  +1,280 B of normal text (139,668 → 140,948 B), within their pages (the build log's module rows, both builds,
  this change before the pin bump, `601a133`).
- **The pin.** vita-vn-ocr `47cf3a3` → `f8a1856`, #135's merge. It also brings #130's `vocr.c`: a light
  global-attention op that no pinned blob has, and that reads every blob bit for bit as before. `vocr.o` goes from
  23,255 to 25,963 B (VitaSDK `-O2` NEON, the build's flags). By symbol: `vocr_run` +984 B, `gate_tile` 806 B new,
  `vocr_load` +442 B, `plan` +268 B, `exp_neg` 228 B and `mat_block` 178 B new.
- **Together** they take the worker's text two pages past API 49's in both builds: each game that is ON then holds
  192,512 → 200,704 B (debug) and 184,320 → 192,512 B (normal, the main download).
- **Measured** with VitaSDK GCC 15.2.0 in the pinned image (`tools/fast_gate.sh`'s; `tools/check_memory_ledger.py`'s
  module rows, text + data/bss), main `3388f7a` against this change at `b74669b` (merged as `0994afc`):

  | Module | Debug: main → this change | Normal: main → this change | Loaded pages |
  |---|---:|---:|---:|
  | Game worker | 155,576 + 35,328 → 160,648 + 35,328 B | 146,676 + 35,320 → 151,748 + 35,320 B | debug 192,512 → **200,704 B**; normal 184,320 → **192,512 B** (`--exact`) |
  | Shell | 101,884 + 73,184 → 101,988 + 73,200 B | 89,764 + 73,192 → 89,884 + 73,232 B | 176,128 / 163,840 B, unchanged |
  | Kernel | 43,080 + 77,764 B, unchanged | 37,976 + 76,076 B, unchanged | 122,880 / 118,784 B, unchanged |

  The worker keeps 3,192 B (debug) and 3,900 B (normal) of its last text page; the normal Shell keeps 228 B of code
  room. The Shell's +104 / +120 B of text are the key's parsing, its request bits and `same_pipeline`'s comparison.
- **Stacks.** The helper threads' deepest chain in the runtime is now `vocr_run` 600 + `run_head` 1,320 + `mm_i16`
  616 = 2,536 B of their 16 KiB, 8 B more than at `47cf3a3`; the two-level path's is `vocr_run` 600 + `run_head2`
  1,240 + `mm_i16` 616 = 2,456 B (`-fstack-usage`, the build's flags). On the host build the largest full-screen job
  is 1,218,240 B, 192 B more than at `47cf3a3`, in the same pages (`host/tests/test_ledger.c`): on the host each
  of its two models and four contexts is 32 B larger.
- **The runtime's structs.** f8a1856's `vocr.h` (#130's attention op) grows `vocr_model` + `vocr_ctx` from 3,872
  to 3,920 B, as the console's compiler lays them out (1,872 + 2,048 B, 16-byte aligned; arm-vita-eabi-gcc 15.2.0,
  `sizeof`). That is `VJO_VOCR_VITA_STRUCTS`, which the Vita build checks. Each tier's workspace (its pinned arena
  plus the structs) keeps its pages: VOCR5-S 128,800 of 131,072 B, VOCR5-N 188,368 of 188,416 B, VOCR5-L 266,448
  of 270,336 B. The finder's arena at 960×544, 83,323 B, is smaller than each (`host/tests/test_vocr.c` checks
  every tier's job bytes with it).
- **Unchanged:** data/bss; the Shell's and the kernel's pages. There is no new block, thread or allocation: the
  fixes use the finder's own arena (`vocr_lines_arena_bytes` is unchanged).

## Trusted hooks without waiting (API 49)

A trusted line is read at once ([native-hooks.md](native-hooks.md#no-wait-for-a-trusted-line-api-49)): the kernel's
`hook_scene` (a word of `.bss`, a word at the end of `VjoState`, which the Shell copies on its stack only), the take's
shorter wait and the rows-less publication (code only); the worker's watch of trusted sources and its last reported
text's hash and length, its trust and site words (four words of `.bss`), the hook pool reused for the watch's text
(no new buffer); the Shell's pacing bypass, the cursor kept when a line grew, and history's newest record (its place,
time and the last append's: 32 B of `.bss`). A trusted answer now takes no capture strip at all (its capture is
published without rows); it takes API 43's strip only when the region shows another scene (API 51: only when the
region has no screen yet). No new block, thread or
stack, no budget raised. API 43's follow-up (`same_pipeline` compares `hook_trust`, +16 B of normal Shell text) is
part of it. The rows [measured: the build log's module rows, VitaSDK 2026.08 (`tools/fast_gate.sh`'s image), main
`b6c715c` and this change, both builds]:

| Module | main `b6c715c` | this change | pages |
|---|---:|---:|---:|
| worker, normal | 146,116 + 35,304 B | 146,676 + 35,320 B | 184,320 B both |
| worker, debug | 155,032 + 35,312 B | 155,576 + 35,328 B | 192,512 B both |
| Shell, normal | 89,300 + 73,160 B | 89,764 + 73,192 B | 163,840 B both (exact) |
| Shell, debug | 101,548 + 73,152 B | 101,884 + 73,184 B | 176,128 B both |
| kernel, normal | 37,624 + 76,068 B | 37,976 + 76,076 B | 118,784 B both |
| kernel, debug | 42,656 + 77,756 B | 43,080 + 77,764 B | 122,880 B both |

The debug worker keeps 72 B of its last text page (616 B before; the watch was written compactly to fit), the
normal worker 780 B (1,340 B); the normal Shell 348 B of code room (812 B) and 536 B of data room, the debug Shell
516 B; the normal kernel 2,984 B.

## Vertical text (API 47)

`ocr_vertical` ([vita-vn-ocr.md](vita-vn-ocr.md#vertical-text-vita-vn-ocr-99)) adds code to the game worker and a
little to the kernel and the Shell, and no block, thread or stack: a job of columns takes its workspace from the job's
own block (its 57,344 B rotation buffer, `VJO_VOCR_ROTATE_BYTES`, within `VJO_BUDGET_GAME_WORKSPACE`:
`test_ledger.c`) and fetches each column taller than a strip, as most are, through the tall-line block every job
already had (`VjoGameVocrLine`, one at a time, at most 57,344 B for a column, within `VJO_BUDGET_GAME_LINE`), and a job
of rows, `off`'s or `auto`'s, takes what it took before. The worker's code is in every game
that is ON, whatever the setting: its column finder (vita-vn-ocr's `runtime/vocr_vertical.c`, `-Os`), the dark flag
of a column (`vocr_line_dark`), the column path, auto's rule and the worker's retry take each build's text one page
past main's. In the Shell, the setting's key, the vertical model's name and one cold message of the OCR's (the denied
model, 69 B) are in the data segment (`VJO_DATA_TABLE`, as main's other keys), so its normal build keeps its exact
pages. The module rows [measured: the build log's module rows, `tools/fast_gate.sh`'s VitaSDK image, main `796168e`
(CI's Release run 40407403783) and this change]:

| module | main `796168e` | this change | pages |
|---|---:|---:|---:|
| Game worker, debug | 149,368 + 35,312 B | 155,032 + 35,312 B | 188,416 → 192,512 B |
| Game worker, normal | 140,596 + 35,304 B | 146,116 + 35,304 B | 180,224 → 184,320 B |
| Shell, debug | 101,460 + 73,008 B | 101,548 + 73,152 B | 176,128 B both |
| Shell, normal | 89,252 + 73,048 B | 89,300 + 73,160 B | 163,840 B both (exact) |
| Kernel, debug | 42,544 + 77,756 B | 42,656 + 77,756 B | 122,880 B both |
| Kernel, normal | 37,512 + 76,068 B | 37,624 + 76,068 B | 118,784 B both |

`VJO_BUDGET_WORKER_MODULE_PAGES` is raised by 4,096 B in each build, in the game's partition only. The normal
worker's text has 1,340 B of its last page left, the debug worker's 616 B; the normal Shell's 812 B. None of it is
measured on a console.

The audit's fixes (auto's tries share the job's stable-text wait and report the last try's model, the step log keeps
the last try's steps, the kernel refuses the field's 3) add 240 B of worker text in both builds on today's main
(160,648 → 160,888 B debug and 151,748 → 151,988 B normal: 2,952 and 3,660 B of the last page left; on main
`b6c715c`, before API 49 and 50, +256 and +240 B), the same pages, and 16 B of normal-kernel text on `b6c715c`
(37,624 → 37,640 B; the debug kernel's 42,656 B unchanged), the same pages; the Shell's code is unchanged
[measured: the build log's module rows, both builds, the fixes on main `241e09f` (whose plugins main `16fa362`
keeps; main's rows: `include/vjo_ledger_budget.h`, API 50) and on `b6c715c` (the fast gate's builds)]. No block,
thread or allocation.

## Trusted hooks and the OCR-skip switches (API 43)

Trusted hooks answer without OCR ([native-hooks.md](native-hooks.md#trusted-hooks-api-43)): the selection's
`vjo_hook_select_trusted` and its close-agreement mark (one word in each of the worker's two `VjoHookSelect`
copies), the job path's branch, its run counter and the warm-up's condition. A trusted answer takes the one capture
strip every hooked answer takes, beside `VJO_GAME_RESERVE_HOOK_ONLY`, and after 8 in a row gives back the resident
blocks: no new block, thread or stack, no budget raised. The Shell parses `hook_trust` in `hook_sites`' branch and
sends it in each job's speed word (a field in each `VjoConfig` copy). The Rubi app gains two cards. The rows
[measured: the build log's module rows, VitaSDK 2026.08 (`tools/fast_gate.sh`'s image), main `23921ad` and this
change, both builds]:

| Module | main `23921ad` | this change | pages |
|---|---:|---:|---:|
| worker, normal | 140,004 + 35,280 B | 140,436 + 35,304 B | 180,224 B both |
| worker, debug | 148,792 + 35,288 B | 149,208 + 35,312 B | 188,416 B both |
| Shell, normal | 89,228 + 73,024 B | 89,252 + 73,048 B | 163,840 B both (exact) |
| Shell, debug | 101,332 + 73,008 B | 101,460 + 73,008 B | 176,128 B both |

The normal Shell keeps 860 B of code room (884 B before), the debug worker 2,344 B (2,760 B). The kernel's rows are
main's.

## Head gate and two-level head (API 42, vita-vn-ocr #120 and #122)

Measured with VitaSDK GCC 15.2.0 in `vitasdk/vitasdk:2026.08` (CI's digest, `tools/ci_container_build.sh`'s),
Release, on the packaged SELF files of both variants (`tools/check_memory_ledger.py`, the build log's module rows),
main `6f2ac6c`, the merge's parent (CI's Release run 40358507420's artifacts), against the merged change (the fast
gate's builds of `c519aa5`, its tree); the worker and kernel rows of main `4bc2604`, measured before the rebase,
are the same (`213fd60`, #305 and #308 change only the Shell's reading-time code). Text + data/bss:

| Module | Debug: main → this change | Normal: main → this change | Loaded pages |
|---|---:|---:|---:|
| Game worker | 142,776 + 34,880 → 148,424 + 34,960 B | 134,004 + 34,872 → 139,668 + 34,952 B | debug 180,224 → **188,416 B**; normal 172,032 → **180,224 B** |
| Shell | 102,204 + 73,464 → 102,212 + 73,544 B | 90,028 + 73,504 → 90,084 + 73,560 B | 176,128 / 163,840 B, unchanged |
| Kernel | 42,536 + 77,756 → 42,540 + 77,756 B | 37,504 + 76,068 → 37,508 + 76,068 B | 122,880 / 118,784 B, unchanged |

- **The game worker, in the game's partition only: two text pages more in each game that is ON, both builds**
  (raised in `include/vjo_ledger_budget.h`). The runtime's code grows from main's pin to this one (vita-vn-ocr
  `817d203` → `47cf3a3`: the head gate, the two-level head, #123's stride check): `vocr.o` 18,218 → 23,255 B
  (+5,037 B; VitaSDK `-O2` NEON, the build's flags). The linked worker by symbol (`arm-vita-eabi-nm -S`, the normal
  build): `run_head2` 1,722 B and `h2_update` 432 B new, `vocr_run` +1,228 B, `vocr_load` +794 B, the plan's buffers,
  `topk_conf` 340 B and `ctc_push` 102 B now out of line, `find_trailer` 98 B, `vocr_has_gate` and
  `vocr_has_two_level` 22 B each; the job's code (`core/vocr_ocr.c`, `game_ocr/main.c`): `finish_line` +388 B (the
  re-read), `vjo_game_ocr_run_request` +122 B, `kana_counter` 84 B, `run_job` +72 B, `line_conf` 60 B (out of line),
  `run_opts` 52 B, `counter_redecode` +20 B, `load_model` +8 B. Data/bss +80 B: the job's line slots (`lines`: the
  runtime's new context fields in each slot, the job's context pointer and its two-level flag). The debug worker's
  text ends 968 B past 147,456 B (36 pages), the normal one's 404 B past 139,264 B.
- **The Shell's pages stay as they are.** The key's parsing, its request bits and `same_pipeline`'s comparison are its
  only code (`apply_config`, `same_pipeline`, `vocr_run_one`: +40, +12 and +4 B of text in the normal build,
  `arm-vita-eabi-nm -S` against main `4bc2604`; the module's text +56 B against `6f2ac6c`). The key's name and its two
  new values (29 B) live in the data segment (`VJO_DATA_TABLE`, core/port.h; [below](#tables-in-the-data-segment)):
  with them in the code segment the normal Shell's text was 90,124 B, 12 B past its last page [measured], and two keys
  (`ocr_head_gate` and `ocr_two_level`, each `on | off`) measured 90,180 B of normal Shell text, 68 B past its last
  page: one page (4,096 B) more of SceShell's. The normal Shell's text keeps 28 B of its last page (90,084 of 90,112
  B), its data/bss 168 B (73,560 of 73,728 B: the three tables, 49 B, and the 4 B the field adds to each of the
  Shell's three copies of the configuration); the debug Shell's text is 8 B larger than main's (188 B of its last page
  left), its data/bss 80 B larger.
- **The models.** Normal's blob is 3,776 B larger than v0.3.0's (1,189,408 B), one weights page more (1,191,936 B);
  Large's 3,776 B (2,477,472 B) take its block one page more too (2,478,080 B, the weights and cascade budgets);
  Small's 1,792 B (821,584 B) stay in its 823,296 B block.
- **Per job.** Each arena with both paths on: Small +48 B (124,880 B), Normal and Large +1,504 B (184,448 /
  262,528 B; the gate's frame indices, the two-level head's level-1 maxima, escape units and fired frames). The
  largest full-screen job (VOCR5-N with VOCR5-L's cascade, both helpers) is 1,218,048 B on the host build, two pages
  more than main's (`VJO_BUDGET_GAME_WORKSPACE` 1,212,416 → 1,220,608 B, `host/tests/test_ledger.c`'s figure,
  page-rounded); a first Normal or Large job takes 4,096 B more (1,445,888 / 2,813,952 B).
- **The pool.** The game's worst case grows by the weights' and cascade's page and the workspace's two (16,384 B, to
  10,584,064 B), its permanent part by the worker's two pages (8,192 B); `VJO_LIMIT_GAME_JOB` grows by both,
  10,579,968 → 10,604,544 B (the convention of API 34 and #109: the limit counts the worker's pages too), 11.8 % of
  the 90,177,536 B of free USER #81 measured (the worst case 11.7 %).
- **Stacks.** The helper threads' deepest chain in the runtime is `vocr_run` 592 + `run_head` 1,320 + `mm_i16` 616 =
  2,528 B of their 16 KiB (2,392 B at main's pin: 448 + 1,328 + 616); the two-level path's is `vocr_run` 592 +
  `run_head2` 1,232 + `mm_i16` 616 = 2,440 B (`-fstack-usage`, the build's flags, `817d203` and `47cf3a3`).
- **The kernel** keeps its pages: its 4 B more of read-only data are the model broker's longer file names
  (`kernel/game_ocr.c`), and three instructions carry 42 instead of 41.
- **No new block, thread or allocation site.** These are binary and host measurements; nothing of it has run on a
  console.
- **The audit's fixes** (#328: a re-read that does not fit keeps the gated reading, greedy-only jobs read the kana
  line again, the cascade's stats) add 144 B of worker text in the debug build and 128 B in the normal one (149,208 →
  149,352 B and 140,436 → 140,564 B on main `93647d3`), the same pages; data/bss is unchanged, and the Shell and the
  kernel are byte for byte main's [measured: the build log's module rows and sha256, both builds, CI's artifacts of
  main `93647d3` and the fast gate's builds of #328].

## VOCR6's character LMs (vita-vn-ocr #127; no API change)

- **The LM budget** is the largest pinned LM, page-rounded: `vocr_lm6_o3_4000k.vlm`'s 3,603,552 B take
  3,604,480 B (`VJO_BUDGET_GAME_LM`; ctclm.2's 3,781,072 B took 3,784,704 B): 180,224 B less. The default tier's
  resident block is 983,040 B (`vocr_lm6_o3_1000k.vlm`, 982,320 B; ctclm.2's 987,600 B took 991,232 B).
- **The pool.** The game's worst case goes 10,584,064 → 10,403,840 B (11.5 % of the 90,177,536 B of free USER #81
  measured; a real peak, VOCR5-N with VOCR5-L's cascade, the 4 MB LM and both helpers, 9,117,696 B);
  `VJO_LIMIT_GAME_JOB` is kept at 10,604,544 B.
- **The decoder's workspace** at the new settings (LM weight 0.6, bonus 2.5 nats, the start-of-line context, no
  end-of-line term) stays within `VJO_VOCR_LM_WS_BYTES` (49,152 B): `host/tests/test_vocr.c` plans every pinned
  model with every LM tier's own settings against it (with the models dir). `vocr_lm_workspace_bytes` is 41,632 B for
  VOCR5-N/L and 39,712 B for VOCR5-S [host measurement, #306's author, v0.3.0's blobs; the gated blobs keep their
  class counts, which the workspace grows with].
- **Code:** the job's LM table carries each tier's weight and flags (`core/vocr_ocr.c` `lms[]`, 8 B more per tier):
  the game worker's text +32 B (140,564 → 140,596 B, normal build); the kernel's text +4 B (37,508 → 37,512 B: the
  model broker's two LM names, one character longer each, `kernel/game_ocr.c` through `include/vjo_vocr_sizes.h`);
  every module in the same pages; the Shell byte for byte main's [measured: both normal Vita builds of `cbaa4e3` and
  `796168e`, audit-vocr6-lm-rubi]. No new block, thread or allocation site. Nothing of it has run on a console.

## The reading stats menu

The Triangle menu retains only line provenance and today's/current-game totals. It reuses the
existing text widgets, UI arena and windowed history scanner. `VjoReadingScope` is 64 bytes and
the per-game cache 112 bytes (host C `sizeof`, not console measurements); committed and scanning
snapshots use fixed state. There are no new allocations or threads, and no budget is raised.
The game worker and kernel are unchanged. Build/module-page checks remain authoritative;
physical-console latency, CPU3 and boot behaviour remain unmeasured.

### The adaptive reading time

`reading_time = adaptive` ([ocr-history.md](ocr-history.md#reading-time)) is integer arithmetic in
`vjo_reading_time_add`, which the writer and the count already share (`vjo_reading_scope_add` with it
inlined: 270 → 386 B in the normal build, `arm-vita-eabi-nm`), and a `reading_time` setting (its parse, a field in each
`VjoConfig` copy, the Shell's one read of it as it starts). `VjoReadingTime` stays 32 bytes and the
cache 112: `last_chars` is 16 bits now (a line has at most 4,095 characters) and the 16-bit pace takes
the other half [C `sizeof`]. No allocation, thread or stack budget is added. To keep the normal Shell's
code pages, three of `vjo_err_text`'s cold messages (the dictionary's format, the OCR model, a failed
recognition: 292 B) move to the data segment, as its other messages did. The Shell's rows
[measured: the build log's module rows, VitaSDK 2026.08, main `d609a4f` and this change]:

| Shell | main `d609a4f` | this change | pages |
|---|---:|---:|---:|
| normal | 90,092 + 73,176 B | 90,012 + 73,504 B | 163,840 B both (exact) |
| debug | 102,276 + 73,152 B | 102,204 + 73,464 B | 176,128 B both |

The normal Shell has 100 B of code room now (20 B before) and 224 B of data room (552 B). The kernel
and the game worker are byte-identical to main's.

The pace's floor (audit of #303: `VJO_READING_PACE_MIN`, one comparison) makes `vjo_reading_scope_add`
386 → 394 B in the normal build (388 → 396 B in the debug one, `arm-vita-eabi-nm`). The Shell's rows
[measured: the build log's module rows, `tools/ci_container_build.sh` as of #303 (VitaSDK 2026.08,
`sha256:fd82d88f…`), main `4bc2604`, whose plugins `213fd60` keeps, and this change]:

| Shell | main `4bc2604` | the floor | pages |
|---|---:|---:|---:|
| normal | 90,012 + 73,504 B | 90,028 + 73,504 B | 163,840 B both (exact) |
| debug | 102,204 + 73,464 B | 102,204 + 73,464 B | 176,128 B both |

The normal Shell has 84 B of code room now. The kernel, the game worker, the RCO and the Rubi app's
`eboot.bin` are byte-identical to main's in both builds.

### Previous all-game reading total

Triangle also shows characters/hour and active time with a 15-second cutoff. Its state adds two
32-byte `VjoReadingTime` snapshots (the committed prefix and scan in progress), measured by C
`sizeof`, within the existing data-page allowance. The cache is now 48 bytes. The forward scan
retains the preceding line across windows; the writer updates the same state on a successful append.
No allocation, stack budget or thread is added. The compiled module ledger remains authoritative;
console timing and CPU3 are unmeasured.

### Original character-only measurements

The figures below describe PR #272's initial character-only version, `b6f4385`, before time and
speed were added. They are historical measurements, not timings or sizes of the extended version.


The overlay's "Total characters read" (△; [ocr-history](ocr-history.md#total-characters-read)), measured
with VitaSDK GCC 15.2.0 in `vitasdk/vitasdk:2026.08` (CI's digest), Release, main `e132517` (0.9.3) against
this change, the build log's module rows (`tools/check_memory_ledger.py`, `--exact` in the normal build):

| Shell | main text + data/bss | this change | Loaded pages, unchanged |
|---|---:|---:|---:|
| Debug | 100,860 + 72,912 B | 101,172 + 73,048 B | 176,128 B |
| Normal | 90,004 + 72,960 B | 90,108 + 73,072 B | 163,840 B (exact) |

The total's code is 1,280 B in the normal Shell (`arm-vita-eabi-nm -S`, the sum of every function's change
but the division's): the count of a window (`vjo_history_chars` 148 B, with line history's record parser
shared, `record_line` 232 B and `record_starts` 42 B, which `vjo_history_scan` used to inline: 528 → 280 B,
and the characters' count, `vjo_reading_characters` 74 B and `vjo_line_characters` 4 B, out of line now that
the writer and the count share it), the count's step (`vjo_history_total_step` 652 B), the number with its
commas (`vjo_history_number` 80 B), the view's and the game exit's (`vjo_history_total_text` 40, `_want` 44,
`_keep` 56, `total_save` 54, `_stop` 24 B), the writer's (`vjo_history_append` 1,068 → 1,100 B, `flush` +6 B)
and the callers' (`render` +16, `frame` +12, `ctl_main` +8, `vjo_worker_stop` +4 B). The debug build's is
1,384 B (its step logs the count: 756 B). The normal Shell's code segment had 108 B of room on main. It is
paid for by the Shell's integer division: libgcc's `__udivsi3`, `__divsi3`, `__aeabi_uidivmod` and
`__aeabi_idivmod` (604 + 660 + 24 + 24 B, unrolled for speed) give way to `shell/divide.c`'s bit-at-a-time
`__aeabi_uidiv` and `__aeabi_idiv` (54 + 36 B, `-Os`), for the three divisions the Shell makes (the region
selector's corners when a region is saved, its stick cursor's speed twice a frame while the stick moves it,
and the debug build's job log, which also takes a remainder: its `__aeabi_uidivmod` is divide.c's 16 B):
1,222 B less in the normal build and 1,206 B in the debug one. On those operands a division takes 145
Cortex-A9 instructions against libgcc's 93 [measured: the Vita compiler's objects of both under `qemu-arm
-cpu cortex-a9` with vita-vn-ocr's instruction-count plugin, 128 divisions of the two kinds]: the stick's two
a frame are about 100 instructions more, est. 0.3 µs at 333 MHz. `host/tests/test_shell_divide.c` checks the
source's every quotient and remainder of 1,048,576 operand pairs, 2,000,000 random ones and the edges against
what C's division means, on the host and on the Cortex-A9 under qemu (the ARM lane, built there by GCC 14;
audit-total-chars ran the Vita compiler's own `divide.c.obj` through it under qemu too: 4/4); the game
worker's OCR divides in its loops and keeps libgcc's. The normal Shell's code segment keeps 4 B of room, its
data segment 656 B.

Data/bss: the total's state (`total`, 56 B in the normal build, 72 B in the debug one with its count's log
figures), the view's two strings (`counting…`, `unavailable`, 24 B) and the number's format (5 B), all in the
data segment; the Triangle view's format string grows by 26 B. No allocation, thread, PAF block or budget is
added. The count reads into the control thread's existing 24 KiB scratch (line history's window,
`VJO_HISTORY_WINDOW` + 1 B) and releases it in the same wakeup; `history_total.bin`'s 16 bytes go through the
existing `vjo_file_write` (a temporary file and a rename). Stacks (`-fstack-usage`, the Shell's flags): the
writer's frame 456 → 464 B on VjoNet, whose deepest chain is a job's (5,624 B, below); on VjoControl the
count adds `vjo_history_total_step` 80 B (96 B in the debug build), `vjo_history_chars` 64, `record_line` 16
and the count of a line 32 B under `ctl_main` (or `vjo_file_read` and `vjo_file_write`, which the config and
region files use already), and line history's `vjo_history_scan` frame is 64 → 72 B, all well inside the
thread's 16 KiB, whose deepest chain was 3,744 B (below). The kernel, the game worker, the RCO and the Rubi
app (`eboot.bin` and every file of the VPK) are byte for byte main's in both builds, and the Shell's import
tables and stub sections are main's [measured: sha256 of the CI-path builds of both commits,
`tools/ci_container_build.sh`; the import tables by audit-total-chars]. These are binary and host
measurements; nothing of it has run on a console.

## history.csv's line source

`history.csv`'s `line_source` column ([ocr-history](ocr-history.md#line-source)), measured with VitaSDK
GCC 15.2.0 in `vitasdk/vitasdk:2026.08` (CI's digest), Release, on the packaged Shell SELF of both variants
(`tools/check_memory_ledger.py`, `--exact` in the normal build), main `ebd9e22` (after #247's border lines)
against this change:

| Shell | main text + data/bss | this change | Loaded pages, unchanged |
|---|---:|---:|---:|
| Debug | 100,988 + 73,680 B | 100,988 + 73,672 B | 176,128 B |
| Normal | 90,100 + 73,728 B | 90,100 + 73,720 B | 163,840 B (exact) |

No code: the writer's two engine cells (`engine`, `hook_engine`, 9 B each) become one table of both
sources' engine and `line_source` cells (`source_cells`, 35 B), and the header names the column (397 → 409 B),
both in the data segment ("Tables in the data segment" below); the writer keeps its pointer into the table in
its own state on its frame, so `vjo_history_append` stays 1,068 B with its 456 B frame (`-fstack-usage`) and
the code segment keeps its 12 B of room in the normal build. The normal Shell's data segment had no room on
main (73,728 B, its last page full), and the table and the header's name took 29 B more (one page past the
exact budget, as built: 73,752 B). The append's open and rollback now use the code segment's copy of the
file's path, which line history's reader (`vjo_history_load`) already has: the data segment's copy (35 B,
`history_path`) goes and the code is the same instructions, so data/bss is 8 B smaller than main's in both
builds (8 B of room in the normal build's last page). The kernel, the game worker, the Rubi app and the RCO
are byte for byte main's [measured: sha256 of both variants' packaged files]. No allocation, thread, stack,
PAF block or budget changes; a record is 6 or 7 B longer on the memory card. These are binary and host
measurements; nothing of it has run on a console.

## Hidden Triangle line diagnostics

Measured with VitaSDK GCC 15.2.0, Release, on the built Shell SELF files, with
`check_memory_ledger.py --exact` in each variant:

| Shell | Text | Data/bss | Loaded pages (existing budget) |
|---|---:|---:|---:|
| Debug | 100,948 B | 72,752 B | 176,128 B |
| Normal | 90,100 B | 72,800 B | 163,840 B |

The source snapshot uses 44 B in the view on ARM (source layout), with its current-line backup and
34 history engine bytes inside the existing bounded history allocation. The 2,048 B header-range
arrays move out of permanent storage into the open page's existing UI block: text/spans plus ranges
need 20,064 of its 20,480 B (source bounds, `test_budget_table_is_consistent`, also checked against
`MAX_RANGES` in the overlay). Cold UI/hook format strings use the freed data space to keep both
segments within their existing pages. No allocation site, thread, block size or budget increases.
These are binary and source-bound measurements; console boot, available heap and PAF layout have
not been measured for this change.

## Border lines dropped (API 36)

Measured with VitaSDK GCC 15.2.0, Release, against main `144bc8e` (after #249's counters and #250's held
Up/Down), on the packaged SELF files (`check_memory_ledger.py` in each variant's build); text + data/bss.
Against main `e33f8f6`, before both, the worker and the kernel grew the same, and the Shell 16 B in each
build:

| Module | Debug: before → after | Normal: before → after | Loaded pages, unchanged |
|---|---:|---:|---:|
| Game worker | 140,032 → 140,272 + 32,888 B | 131,164 → 131,420 + 32,880 B | 180,224 / 172,032 B |
| Shell | 100,956 → 100,988 + 73,680 B | 90,084 → 90,100 + 73,728 B | 176,128 / 163,840 B |
| Kernel | 41,740 → 41,756 + 77,732 B | 36,904 + 76,044 B, unchanged | 122,880 / 118,784 B |

The worker's text normalisation gains its third pass (`core/textnorm.c` at -Os: 1,380 → 1,564 B of
code and 44 → 60 B of read-only data, the 8-entry set; its two functions' stack frames 88 + 40 → 96 + 48 B,
measured `-fstack-usage`), in place on the
result buffer. The Shell sets one request bit from the config it already holds; the normal Shell's
code segment keeps 12 B of its last page. The kernel only accepts the bit (its known-bit mask). No
allocation site, thread, static buffer or budget is added. These are binary measurements; console
boot, memory and timing have not been measured for this change.

## Counters (vita-vn-ocr #109)

Measured with VitaSDK GCC 15.2.0 in `vitasdk/vitasdk:2026.08`, Release, on the packaged SELF files of both
variants (`tools/check_memory_ledger.py`, the build log's module rows), main `e33f8f6` (whose worker is `ae9b02d`'s)
against this change:

| Module | Debug pages | Normal pages | Increase |
|---|---:|---:|---:|
| Game worker | 176,128 → 180,224 B | 167,936 → 172,032 B | 4,096 B |

The worker's text segment grows 137,088 → 140,032 B (debug) and 128,220 → 131,164 B (normal); data/bss
stays 32,888 / 32,880 B, and the kernel and the Shell SELF files are byte for byte main's (sha256, both variants). By symbol, from the
linked ELFs: vita-vn-ocr's counter re-decode (`runtime/vocr_post.c`, built `-Os`: `vocr_post_redecode`
1,770 B, `vocr_post_greedy` 216 B, their helpers 446 B), its calls in the job (`core/vocr_ocr.c`
`counter_redecode` and `finish_line`, +272 B), the counter rule in the text normalisation
(`core/textnorm.c`, +194 B) and the worker's policy and cache bit (`game_ocr/main.c`, +40 B, +44 B in the
normal build): 2,938 B. The normal build's code ended 1,112 B below 128 KiB, under the 1,204 B of SCE data
`vita-elf-create` appends there, so the worker's link now keeps `__sce_headroom`, as the Rubi app's does
(no memory: only the segments are loaded). The game ceiling (`VJO_LIMIT_GAME_JOB`) grows by the same
4,096 B. No new block, thread, allocation or stack: the re-decode works on the LM extras' top-K in place
(about 0.9 KiB of the worker's stack while it runs, `-fstack-usage`). These are binary measurements;
nothing of it has run on a console.

## Dictionary conjugation display

Measured with VitaSDK GCC 15.2.0, Release, against main `ae9b02d`, on the Shell SELF's loaded
segments (including conversion metadata), with `check_memory_ledger.py --exact` in each variant:

| Shell | Before text + data/bss | After text + data/bss | Loaded pages, unchanged |
|---|---:|---:|---:|
| Debug | 100,892 + 72,352 B | 100,940 + 73,560 B | 176,128 B |
| Normal | 90,044 + 72,368 B | 90,092 + 73,576 B | 163,840 B |

The immutable reason IDs, labels and offsets use the data segment. Three existing cold dictionary
diagnostics and the conjugation prefix/separator also move there to keep the normal code segment
within its existing pages. The 24-byte page locators and session cache retain their sizes: the
entry length, already bounded to 8192, uses 16 bits, leaving room for four six-bit reason IDs.
The deinflection candidates and per-occurrence tokens/entries grow inside the existing bounded
arenas; their compile-time bounds and maximum-entry host tests pass. No allocation site, thread,
static runtime buffer or budget is added. These are binary and host measurements; console boot,
heap availability and the PAF layout have not been measured for this change.

## Yomitan's deinflection rules, a packed table

Yomitan's rules for spoken forms (`third_party/yomitan/`) take the table from 569 rules to 724. They
fit because the table is packed (`tools/gen_deinflect.py`): every rule string is three-byte UTF-8,
so the strings are one blob without NULs, sharing bytes where one string contains another or
overlaps its end (5,322 -> 4,549 B), and a rule is six bytes, not eight, with its reason in its own
bits (the 569-byte reason table is gone). Measured with VitaSDK GCC 15.2.0, Release, on the Shell SELF's
loaded segments, main `627a673` and this change, `check_memory_ledger.py --exact` in each variant:

| Shell | Before text + data/bss | After text + data/bss | Loaded pages, unchanged |
|---|---:|---:|---:|
| Debug | 102,212 + 73,544 B | 101,332 + 73,008 B | 176,128 B |
| Normal | 90,084 + 73,560 B | 89,228 + 73,024 B | 163,840 B |

`arm-vita-eabi-nm -S` on the normal Shell: `deinflect_rules` 4,552 -> 4,344 B, `deinflect_strings`
5,322 -> 4,549 B and the new `deinflect_rules_in` 5 B (all rodata), `deinflect_reasons` 569 B -> none,
`conjugation_labels` 333 -> 355 B and `conjugation_offsets` 74 -> 86 B (data). The Rubi app links
`core/local_dict.c` without the rules and is unchanged (its `eboot.bin` byte-identical in both variants). The
lookup's candidates per span stay bounded by the same 128-record scratch: `core/local_dict.c`'s own
`deinflect()` on every span the 64,579 taps of [the Yomitan check](../reports/yomitan-check.md) scan (590,006,
the key stopping where `boundary()` stops) makes 1,175,231 candidates where the old table made 1,174,549, at
most 15 a span either way, none at the cap, and the same 24,432 B arena peak (host, ASan/UBSan, audit of
#320; #320's figures, 590,351 spans and 1,175,170 -> 1,175,863, came from a Python model that did not stop at
U+0004). The VjoNet thread's frames grow by 56 B (`lookup_pages` 1,536 -> 1,584 B, `deinflect` 264 -> 272 B,
`-fstack-usage` with the Shell's flags), inside its 16 KiB stack. No allocation, thread or budget is added.
These are binary and host measurements; nothing of it has run on a console.

## Remembered hooks (API 34, issue #241)

Measured with VitaSDK GCC 15.2.0, Release, on the packaged SELF files in both variants:

| Module | Debug pages | Normal pages | Increase |
|---|---:|---:|---:|
| Kernel | 122,880 B | 118,784 B | 4,096 B |
| Game worker | 176,128 B | 167,936 B | 8,192 B |

The kernel segments are 41,740 + 77,732 B (debug) and 36,904 + 76,044 B (normal). The cache broker
adds one code page, reusing the existing model staging buffer, ownership state and file thread; its
BSS is unchanged. The worker segments are 137,088 + 32,888 B (debug) and 128,220 + 32,880 B (normal).
Its adaptive selector, executable hashing and cache handling add one text page; the stable source
identity and rollback state cross the next data-page boundary. These increases are deliberate in
`include/vjo_ledger_budget.h`; the game ceiling grows by the same 8,192 B. No new allocation site,
thread, Shell storage or PAF allocation is added.

The temporary cache blob is exactly 3,088 B by format, held on the existing worker stack only during
cache operations. The exact executable is hashed with a 1,024-byte streaming buffer on that stack.
The broker's file-worker frame remains 408 B, and its syscall frame is 56 B (measured compiler
`-fstack-usage`, both variants). Local reports are `build/vita-minimal/measure-ledger.json` and
`build/vita-minimal-normal/measure-ledger.json`. These are binary/compiler measurements; console
boot, available memory, OCR invocation counts and CPU cost have not been measured for this change.

Host validation covers the selector's distinct-line validation, 8→16→32 cadence, unchanged dialogue,
restart confirmation and recovery; exact executable hashing; bounded/corrupt cache data; and broker
and worker I/O, cancellation and cleanup failures. Console validation remains the procedure in
[Native hooks](native-hooks.md#release).

## Combined enhancement test build (local, API 33)

The local combination of PRs #209, #233, #234, #235 and #236, with popup source controls removed, fits the
same permanent module pages as test.7. These are **binary measurements**, not console heap measurements:
VitaSDK GCC 15.2.0, both Release variants, `tools/check_memory_ledger.py` on the produced SELF files.

| Module | Debug pages | Normal pages |
|---|---:|---:|
| Kernel | 118,784 B | 114,688 B |
| Shell | 176,128 B | 163,840 B |
| Game worker | 167,936 B | 159,744 B |

The overlay's 512 highlight ranges use 16-bit starts and lengths, retaining every range and checking
32-bit inputs before conversion. That reduces their static storage from 4,096 to 2,048 B. Existing immutable
UTF/config/ledger tables use free space in the Shell's data pages; the Luna title-ID table uses free
space in the game worker's data pages. No feature, model, thread or allocation is removed or added by
these storage changes. The existing 1.5 MiB PAF reserve remains. Line history retains PR #236's 16 KiB
transient PAF block during a history visit.

Measured Shell text + data segments: debug 100,892 + 72,352 B, normal 90,044 + 72,368 B.
Worker segments: debug 134,120 + 32,576 B, normal 125,172 + 32,568 B. Reports are saved in the
local build directory as `combined-debug-ledger.json` and `combined-normal-ledger.json`.
The standalone figures below describe their named revisions and are retained as history.

## The parts

| Part | Where | What it does |
|---|---|---|
| budget table | [`include/vjo_ledger_budget.h`](../include/vjo_ledger_budget.h) | one `#define` per item (its worst case in bytes) and per pool (its limit), with the source of each number; plain integer arithmetic so the C code and the checker read the same values |
| counters | [`core/ledger.c`](../core/ledger.c) | current and peak bytes per item and per pool, atomics only, no libc; one `VjoLedger` per process |
| wrappers | `shell/memory.cpp`, `shell/ledger.c`, `kernel/buffers.c`, `kernel/loader.c`, `kernel/main.c`, `kernel/io_worker.c`, `game_ocr/main.c` | the only places that call an allocator or create a thread; each names its item and counts the bytes in and out |
| CI check | [`tools/check_memory_ledger.py`](../tools/check_memory_ledger.py) | fails the build on an allocation outside the wrappers, a worst case over its limit, or a packaged module with more pages than declared; prints the table |
| run time | `vjo_ledger_shell_log` (Shell), `kledger_log` (kernel) | the pools and items, current and peak, in `status.txt` and `kernel.txt` |

## The table

The pool figures below are the combined source's declared budgets, including line history's
16 KiB visit. They are not console measurements or proof that the combined modules fit.
`tools/check_memory_ledger.py` prints them; CI runs it against the sources and the packaged
modules (`ctest -R memory_ledger` runs it locally):

```
  pool                              permanent   worst case        limit     headroom
  shell pages                         217,088      237,568    1,048,576      811,008
  shell paf                             8,192      241,664      985,088      743,424
  kernel                              155,648      245,760      245,760            0
  game                                237,568   10,403,840   10,604,544      200,704
```

(The debug build's budgets, as `tools/check_memory_ledger.py` prints them with API 50 and VOCR6's LMs; the normal
build's game permanent is 229,376 B.)

Historical packaged-module output retained from PR #234 before this integration (current combined
measurements are above):

```
  packaged modules (loaded pages vs declared budget)
      VitaJPOverlay_Kernel      118,784 B of    118,784 B declared (40,484->40,960 + 77,700->77,824)
      VitaJPOverlay_Shell       176,128 B of    176,128 B declared (98,748->102,400 + 73,304->73,728)
      VitaJPOverlay_OCR         167,936 B of    167,936 B declared (133,392->135,168 + 30,568->32,768)
```

The combined source includes native hooks from PR #234 (`891a8d9`), black-bar trimming from
PR #235 (`de5f7e0`, based on main `75041a0`), character statistics from PR #233 (`8601141`) and
line history from PR #236 (`6ed8f30`). Their standalone measurements below describe their own
source trees. Line history adds the eleventh ledger item and a 16,384 B transient PAF visit block;
its five header spans fit in the unchanged 20,480 B UI arena (18,016 B needed, host test). The measured
combined worker retains PR #234's 167,936 B debug / 159,744 B normal pages. Shell page budgets and the
PAF reserve are unchanged. Trimming still uses the existing 450,560 B screenshot block and adds no
transient allocation. Both combined builds pass the module checks, including the normal build's
exact page check.

Every Shell configuration is the shipped one since Google Lens and the Hachidori relay were
removed: the overlay is offline, a job's lookup takes one 64 KiB arena, and there is no opt-in
network row (it was 707,840 B with Google Lens alone, 731,632 B with the relay's second connection
slot). An Anki card's screenshot (API 23) is made in the game: the Shell's 128 KiB Anki arena
holds only its job's request and result, and the game's `VjoGameShot` block (450,560 B) is a job of
the one job slot, never beside an OCR job's transient blocks, so neither pool's worst case moves.
The shipped default (local JMdict) has 743,424 B of headroom (759,808 B before line history). The kernel row has no full-frame
capture since API 24 (2,088,960 B of its 2,248,704 B worst case until then): no Shell code took it
after Google Lens's removal, and OCR and the screenshot read the screen in the game. Its worst case is
now the permanent part, a kernel strip and a starting game's `games.ini` read.

*Permanent* is held from boot (or from the game's start) until exit; *worst case* adds the largest
transient set that can be live at once.

### Pool "shell pages": SceShell's partition (`ScePhyMemPartShell`, 50 MiB)

| item | bytes | what | source |
|---|---|---|---|
| Shell module pages | 176,128 | `VitaJPOverlay_Shell.suprx`: 102,400 text + 73,728 data/bss, permanent; 262,144 B packaged (188,416 + 73,728, of 266,240 B declared) from the template's removal (#101, below) until Google Lens's, 266,240 B (192,512 + 73,728) before it, 282,624 B (208,896 + 73,728) until AnkiConnect's removal, 286,720 B (212,992 + 73,728) until the Hachidori relay's and 294,912 B (217,088 + 77,824; 299,008 B declared) until the online dictionaries' (both below) (405,504 B at `e759187`: the cold files are built with `-Os`, and the overlay's 96 KiB text arena left `.bss` for the PAF heap). #90's fixed Kiku note took 3,488 B off data/bss (the Anki field mapping's 1,152 B from each of the three `VjoConfig` copies; the LAN search moved to the Rubi app); with #86's region selector and #84 (its `--gc-sections` link, and 28 B of new `VjoConfig` fields) the module is 214,488 B of text and 77,876 B of data/bss; #87's row navigation makes it 214,480 B of text and 77,908 B of data/bss, the same pages, so 2,608 B of text and 4,012 B of data/bss are free in them; #88's normalised text and its log make it 214,816 B of text (main `94d608e`); #92's quiet refresh makes it 214,672 B of text and 77,956 B of data/bss (2,416 and 3,964 B free); #85's two engines and #89's A/B log (the LM tier, the cascade's speed line) make it 215,216 B of text and 77,980 B of data/bss [`bcd3cf0`]; the finder refinement's step and key make it 215,256 B and 77,984 B [`fec85af`], still the same pages (1,832 and 3,936 B free); API 21's model sizes (#108) take `VjoConfig`'s two 64-byte model names from its three copies, so data/bss is 77,624 B in 77,824 B of pages and the module **294,912 B** of the 299,008 B declared, text 216,448 B with #102 [the build log's module row, main `0023e8a` and this change]; API 22 (#93) makes it 216,856 B of text and 77,648 B of data/bss, the same pages [`ba5f69a`]; without jpdb.io and jiten.moe (main `ba5f69a` and this change) it is 210,184 B of text and 71,204 B of data/bss, one text page and one data page fewer (2,808 and 2,524 B free), and the declared budget is lowered to the packaged pages, **286,720 B**; without the Hachidori relay client (main `a5d79f2` and this change) it is 206,192 B of text and 71,020 B of data/bss, one text page fewer again (2,704 and 2,708 B free), **282,624 B**; without AnkiConnect (the Shell's sender, its requests, the screenshot branch, the Kiku note and the audio URL; the Rubi app exports the cards; main `5e68ecd`, 206,664 B and 71,076 B since #116 and #111, and this change) it is 188,472 B of text and 70,104 B of data/bss, four text pages fewer (4,040 and 3,624 B free), **266,240 B**; #123's key filter makes it 189,104 B of text, the same pages (3,408 and 3,624 B free) [the build log's module row, main `c641bdf`]; #122's app check, without the config template, makes it 186,416 B of text and 70,112 B of data/bss, one text page fewer (2,000 and 3,616 B free), 262,144 B [main `ac905b1`]; #127's frequency ranks make it 187,552 B of text and 70,128 B of data/bss, the same pages (864 and 3,600 B free) [main `9f92bc6`]; #124's identification of Adrenaline's PSP games (`shell/game.c`: AppMgr's app name and one description read per launch; `core/games.c`'s shared PSP row) makes it 188,000 B of text and 70,132 B of data/bss, the same 262,144 B (416 and 3,596 B free) [the build log's module rows, main `9f92bc6` and #124]; without Google Lens (with it the Shell's capture and JPEG encoder, the worker's network pipeline, and the TLS, roots and HTTP client the link no longer keeps; main `6020c0b` and this change) it is 100,792 B of text and 70,060 B of data/bss, 21 text pages fewer (1,608 and 3,668 B free), and the declared budget is lowered to the packaged pages, **176,128 B**; #140's posting hints make it 101,024 B of text, the same pages (1,376 and 3,668 B free) [main `32bfc6a`], and #132's VjoNet stack paint and check 101,488 B of text and 70,076 B of data/bss (912 and 3,652 B free); without the network stack's last users (the sockets, resolver and cancellation of `shell/platform_vita.c`, the UDP log and `log_host`; main `3aa08a6` with #132, and this change) it is 99,268 B of text and 69,772 B of data/bss, the same pages (3,132 and 3,956 B free); #138's plain sentence for a game without the worker makes it 99,244 B of text (89,196 B in the normal build), the same pages [the build log's module rows, main `c08db18` and #138]; API 23's Anki screenshot (`shell/anki.c`'s request to the game worker, the queue's second ID, `shell/worker.c`'s shared job slot; the screenshot itself is the game's) makes it 99,244 → 100,484 B of text and 69,772 → 69,828 B of data/bss, the same pages (1,916 and 3,900 B free; the normal build 89,204 → 90,012 and 69,700 → 69,756 B, the same 163,840 B, 100 B of text room left) [the build log's module rows, main `c6e0a4d` and this change, both builds; the same on `dcb7b31`, whose normal Shell was 89,196 B]; #138's left stick (`core/stick_cursor.h` inlined into the overlay's frame, `vjo_text_read_step`, `stick_lookup_ms` in each `VjoConfig` copy and the view, the close's check: 792 B of normal-build text, which would have taken the normal Shell a text page past its exact budget) and `core/local_dict.c` built `-Os` (on main `74f8de8` 2,552 B of the normal Shell's text off and 2,600 B of the debug one's [measured: both builds with and without the line]; a lookup runs 0.4-0.6 % more Cortex-A9 instructions, below) make it 98,684 B of text and 69,860 B of data/bss on main `74f8de8` (100,364 and 69,828 B), the same pages (3,716 and 3,868 B free; the normal build 88,132 and 69,796 B, from 89,892 and 69,756 B, the same 163,840 B, 1,980 B of text room) [the build log's module rows, main `10c8011` and `74f8de8` and #138, both builds]; Bee's Ultimate Dictionary (its title, the popup's kanji lines, the dictionary's name on Anki cards) makes it 99,276 B of text, the same pages (3,124 and 3,868 B free; the normal build 88,724 B, 1,388 B of text room) [main `c354cfb` and this change, both builds]; the cursor across rows and the stick's four directions on the arrows' path (`vjo_text_read_step` and the no-layout double gone) make it 98,972 B of text on main `a7abad7` (99,196 B with API 24), the same data and pages (3,428 B of text room; the normal build 88,460 B, from 88,636 B, 1,652 B of room) [the build log's module rows, main `a7abad7` and this change, both builds]; the normal build's held log lines of a job (#154, `shell/worker.c` `net_main`: [no log append on the result path](release-variants.md#no-log-append-on-the-result-path)) make the normal Shell 88,556 B of text and 69,868 B of data/bss on main `eece6c6` (88,460 and 69,796 B), the same 163,840 B (1,556 B of text room), the debug Shell unchanged [the build log's module rows, main `eece6c6` and #154, both builds]; `hook_sites` (audit-hooks F3: its parse, the text source word's flag, a field in each `VjoConfig` copy) makes the debug Shell 99,836 → 99,924 B of text and 73,368 → 73,376 B of data/bss, the same pages (2,476 and 352 B free; the normal build 89,292 → 89,380 and 73,376 → 73,392 B, the same 163,840 B, 732 B of text room) [the build log's module rows, `hooks/fix-kernel` `88a0170` and `3c39dbb`, both builds] | packaged SELF segments [measured] |
| OCR worker pages in SceShell | 0 | the kernel loads `VitaJPOverlay_OCR.suprx` only into games ON in `games.ini` (#138, `kernel/loader.c`: never SceShell, a system app, VitaShell or the Rubi app); the installers write no line for it and remove the `*TITLEID` and `*ALL` lines older installs left (`core/taicfg.c`, `tools/patch_tai_config.py`), so taiHEN does not load it into SceShell either. Were it loaded there (an older install's `*ALL` line, a hand edit), its `module_start` returns `NO_RESIDENT`, which unloads it at once (it held 45,056 B there before PR #68) | `kernel/loader.c`; `core/taicfg.c`; `tools/patch_tai_config.py`; `game_ocr/main.c`; [SDK: Kernel-Overview, `module_start`] |
| VjoNet stack+TLS | 20,480 | 16,384 + 4,096, permanent (65,536 + 4,096 until #132: nothing on the thread makes a TLS connection since Google Lens's removal) | `shell/worker.c`; TLS per thread [SDK: Kernel-Reference, `sceKernelCreateThread`] |
| VjoControl stack+TLS | 20,480 | 16,384 + 4,096, permanent | same |
| VjoAnki stack+TLS | 20,480 | 16,384 + 4,096 (65,536 + 4,096 until BearSSL's TLS, which only AnkiConnect and an audio source used, went: its deepest chain is 1,728 B, below, 2,128 B with API 23's screenshot request), only while an Anki job runs: with `anki_mining = on`, a count of the queue on every overlay open, or a save (Enter); a refusal is logged with its rc, these bytes and SceShell's free USER (`shell/ledger.c`) | `shell/anki.c` |
| VjoShellMem USER_RW | 0 | the host tests' result memory; the Vita build uses PAF (`VJO_PAF_ALLOC`) | `shell/worker.c` |
| **limit** | **1,048,576** | half of the "genuinely under 2 MiB" a Shell plugin may have at most [expert]; SceShell had 28,672 and 98,304 B free beside a game [measured], so the limit is a ceiling on our footprint, not a promise that it fits: module pages may only shrink, and the checker fails when a packaged module has more pages than declared | AGENTS.md rule 1; [vita-memory.md 5.1](vita-memory.md) |

### Pool "shell paf": SceShell's PAF global heap (5,242,880 B, mapped once)

| item | bytes | what | source |
|---|---|---|---|
| Paf plugin+RCO | 8,192 | the RCO (6,368 B file, measured; 5,632 B before the region selector's cursor) and the Paf `Plugin` object | **[estimate]**; the self-test measures it |
| result arenas | 65,536 (the local dictionary's lookup; the network paths' 2 × 262,144 B arenas and connection pool went with Google Lens and the Hachidori relay) | taken after a job's OCR, freed before the next job's | `shell/worker.c` `LOCAL_RESULT_ARENA_SIZE`; asserted equal in `test_shell_worker.c` |
| Anki job arena | 131,072 | per job: saving a card to the queue. Its screenshot (API 23) is the game worker's: here only the job's request and result, 5,404 B (a one-word card's save peaks at 6,860 B with it); the Shell's own screenshot branch went with AnkiConnect (its 393,216 B arena and the kernel's 2,088,960 B full-frame capture), so the kernel capture is never taken for Anki; a save with 8 dictionaries' entries peaks at 102,624 B, with a screenshot or without (102,616 B on main `dcb7b31`, before `anki_screenshot`'s `VjoConfig` field) [measured, host] | `shell/anki.c` `ANKI_TEXT_MEM`; checked against the budget at compile time and in `test_shell_anki.c` |
| overlay text arena | 20,480 | the header and body texts and their spans (18,016 B needed, `test_ledger.c`: 17,952 B before line history's four header spans), only while the overlay page is open; a refusal keeps the page closed | `shell/overlay.cpp` |
| line history | 16,384 | one visit's block ([line history](line-history.md)): from the overlay's first Up past its text's top row until the current text is back (or the overlay closes, the game exits): the visit's table, the current text and up to 32 of this game's earlier lines from `history.csv`'s last 24,512 B (read into VjoControl's static scratch, not here); a refusal means no visit, Up does nothing | `shell/worker.c` `history_open`; `VJO_BUDGET_PAF_HISTORY`, its fit checked at compile time |
| **limit** | **985,088** | the measured free heap, 2,557,952 B (2,498 KiB beside CLANNAD, 2026-10-10), minus the 1.5 MiB reserve the runtime guard keeps for firmware; the reserve is the headroom | `docs/meiki-ocr.md` on `experiment/meiki-ocr` [measured]; `shell/memory.cpp` |

Allocations here take no new pages from the partition; a failure is a `NULL`, never `0x80024302`
(vita-memory.md 3.2). Later sessions saw 355-441 KiB and 190 KiB free [measured], which would
refuse everything above; the code survives a refusal (host tests), so this limit is the best
measured case, not a guarantee. The worst case of the shipped default (one 64 KiB arena, the
overlay open on an earlier line, an Anki card being saved) is 241,664 B (225,280 B before line
history's 16 KiB visit), the only configuration since the network
paths' removal (the opt-in network row was 707,840 B with Google Lens alone; with a 384 KiB
picture card and the relay's second connection slot it was 993,776 B, 8,688 B over the limit,
which the checker reported as a warning).

The word lookup's ranking (`core/local_dict.c`) takes no allocation and no item of its own: its
common-readings table (70 pairs, 924 B of `const` strings) is in the Shell module's text pages, which
stay at 299,008 B because `shell/worker.c` is now built with `-Os` (the build log's module row:
216,904 B of text, 81,232 B of data and `.bss` [measured]). Its state is on the VjoNet thread's
64 KiB stack: the cursor lookup's frame grows from 576 to 1,328 B (two 8-entry match arrays and the
span ends) and its deepest call chain from 1,008 to 2,464 B [measured: `arm-vita-eabi-gcc -O2
-fstack-usage` of `core/local_dict.c`, main and this change]. The lookup's 48 KiB scratch stays
inside the 64 KiB result arena (compile-time check `WorkspaceFits`). The lookup log line uses a
256-byte stack line, as every `vjo_log` call does.

Drawing the region with the stick (#81, `core/region_select.c`) takes no allocation either. Its
code (1,108 B of `region_select.c`, and `shell/overlay.cpp` at 8,822 B where the old file built
for size is 8,237 B) is paid by building `shell/overlay.cpp` at `-Os` (11,608 B at `-O2`); it
runs only while the overlay is open. The Shell's text goes from 217,048 to 215,312 B and its
data and `.bss` from 81,232 to 81,336 B (the selector's state, 104 B), so the module stays at
299,008 B [measured: the build log's module row, main `4a7ae4d` and this change]. The RCO grows
from 5,632 to 6,368 B for the cursor's five planes and one style; the 8,192 B Paf plugin
estimate is kept, which leaves 1,824 B (was 2,560 B) for the Paf `Plugin` object, a size still
unmeasured. On the Paf main thread's stack the region hint's frame is 248 B with its 192-byte
line, under `render`'s 312 B, so the overlay's deepest frame is unchanged [measured:
`arm-vita-eabi-g++ -Os -fstack-usage`].

Writing a job's log lines together (`shell/log_vita.c` `vjo_log_batch_begin`/`end`: the result's
lines, the recognized text's, the ledger table's) takes no allocation and no item: each batch's
buffer is on the stack of the thread that logs it, for the burst only. On VjoNet's stack (64 KiB
then, sized for BearSSL's TLS, which `vocr_run_one` never runs; 16 KiB since #132) `vocr_run_one`'s frame grows from 832
to 4,992 B (its 4 KiB buffer) and `log_text_lines`' is 1,840 B (1.5 KiB); the deepest chain of the
Shell's own code on that thread goes from 1,544 to 5,672 B. On VjoControl's 16 KiB stack
`vjo_ledger_shell_log`'s frame grows from 32 to 1,128 B (a 1 KiB buffer), its chain from 424 to
1,592 B; the thread's deepest chain of Shell code stays the config reload's, 3,128 B before and
3,200 B after (the flush adds 72 B under `vjo_log`) [measured: `arm-vita-eabi-gcc -fstack-usage
-fcallgraph-info=su` with each file's own flags (`-Os` for these), main `e90a43d` and `72292e1`;
chains of the Shell's own files, the SDK's calls counted as 0; #100 first quoted the same
measurement at `-O2`]. The logger's code grows the Shell's text from
215,256 to 215,952 B and its data from 77,984 to 77,988 B (the open batch's pointer): the module
stays at 299,008 B [measured: the build log's module row].

The overlay's arrow keys over the header's displayed rows (#81, `core/text_rows.c`, 1,462 B of
text at `-Os`) take no allocation either: a key press walks the overlay's existing 4 KiB text copy
(`s_cursor_text`) on the paf thread, its deepest call chain 384 B of stack plus paf's own
`GetCharInfo` [measured: `arm-vita-eabi-gcc -Os -fstack-usage`]. Their state is a static 88-byte
`VjoRowNav` (Right's 16 positions, Up/Down's column, three counters) in place of the 68-byte
`VjoWordNavigation`, the L/R tap's state, the header's point size and the slowest key's time:
`.bss` grows by 32 B, within the module's data pages. The text pages stay at 299,008 B because
`shell/strip.cpp` (4,107 to 2,974 B) and `core/arena.c` (912 to 518 B; `vjo_arena_alloc` is the
same instructions) are built with `-Os` too. They pay for `text_rows.c` and for the key handling's
156 B in `shell/overlay.cpp` (8,822 to 8,978 B at `-Os`): the build log's module row goes from
214,488 to 214,480 B of text and from 77,876 to 77,908 B of data and `.bss` [measured: main
`dca0ba4` and this change].

The open overlay's quiet refresh (#81 request 4, `docs/touch-lookup.md`) takes no allocation and no
item of its own either. A refresh is a game OCR job that keeps the overlay's text and its entry: the
one 64 KiB result arena (`VJO_SL_PAF_RESULTS`) stays held during it instead of being freed before
the job, its text stays in the worker's static result until the control thread compares it in its
24 KiB scratch, and a new text is copied into the existing 4 KiB `local_text`. The item's worst case
is unchanged (the arena was already held with the overlay open). Its code (and the SCREEN_CHANGED
pacing, the self-test's text gate, the job submission's BUSY retry) adds 1,116 B of Shell text
(object text, Vita build, `-Os`: `shell/worker.c` +1,022, `shell/selftest.c` +129,
`shell/game_ocr_client.c` +66, `core/client.c` −101), paid for by building six more cold files with
`-Os` (`vita/CMakeLists.txt`: −1,204 B). The module stays at 299,008 B: the build log's module row
goes from 214,816 to 214,672 B of text and from 77,908 to 77,956 B of data and `.bss` (+48 B of
state) [measured: `.refs/ci.sh … vita` on main 94d608e and on this change].

Removing jpdb.io and jiten.moe, the online dictionaries with an API key, takes the packaged module
from 294,912 to 286,720 B, two pages (main `ba5f69a`, which declared 299,008 B), and lowers
`VJO_BUDGET_SHELL_MODULE_PAGES` from 299,008 to 286,720 B, so the pool's permanent pages go from
389,120 to 376,832 B and its worst case from 458,752 to 446,464 B. The packaged text segment loses
6,672 B (216,856 to 210,184 B: `.text` 149,336 to 143,792 B, `.rodata` 62,936 to 61,896 B, the rest the
three import stubs only the strip's readings used, 48 B, and section alignment): the two
clients (`core/jiten.c`, `core/jpdb.c`, 3,203 B of object text before the link drops unused code),
their request, auth and error paths in `core/client.c` and `core/config.c`, and the subtitle strip's
furigana (`shell/strip.cpp`, `vjo_strip_rubies`, `vjo_entries_rubies`; only those two services gave
readings). `.bss` loses 6,444 B (77,496 to 71,052 B): `shell/worker.c` 3,340 B (`VjoView`'s 64 strip
readings and the API keys, 512 B, of the `cfg` and `job_cfg` copies of `VjoConfig`),
`shell/strip.cpp` 2,584 B (its copy of the readings and their 64 text widgets' pointers),
`shell/anki.c` 512 B (`next_cfg`'s keys) [measured: the build log's module rows and
`arm-vita-eabi-size` of the module and its objects, main `ba5f69a` and this change; the kernel and
OCR worker SELFs are byte-identical to main's]. The PAF items are unchanged: the network path's two 256 KiB arenas and two-slot
connection pool stay for Google Lens (its response is up to 256 KiB) and the Hachidori relay (the next
section removes it and its slot), and a local lookup still takes one 64 KiB arena. Every TLS root stays: none was used only by the two
services (`api.jiten.moe` and `jpdb.io` chain to ISRG Root X1, which HTTPS audio sources such as
`upload.wikimedia.org` use too; Google Lens chains to GTS Root R4 [measured: `openssl s_client`,
2026-10-15]).

Removing the Hachidori relay client, which looked words up in the Hachidori app on a computer on
the local network over plain HTTP (`dictionary = hachidori`, `hachidori_host`), gives SceShell back
one more page: the module goes from 286,720 to 282,624 B (`VJO_BUDGET_SHELL_MODULE_PAGES` lowered
with it), so the pool's permanent pages go from 376,832 to 372,736 B and its worst case from
446,464 to 442,368 B. The packaged text segment loses 3,992 B (210,184 to 206,192 B: `.text`
143,792 to 140,448 B, `.rodata` 61,896 to 61,248 B): the client (`core/hachidori.c`, 2,478 B of
object text and 196 B of `.rodata` before the link drops unused code), its branch, error text and
host check in `core/client.c` and `core/config.c` (`vjo_hachidori_endpoint`, the `hachidori_host`
setting, the template's lines about it), the JSON helper only it used (`vjo_json_append_str`,
`core/json.c` -110 B) and the worker's relay paths (`shell/worker.c` -316 B of object text).
`.bss` loses 184 B (71,052 to 70,868 B): the 64 B `hachidori_host` of the `cfg`, `job_cfg` and
`next_cfg` copies of `VjoConfig` and the cached lookup's dictionary ID [measured: the build log's
module rows and `arm-vita-eabi-size` of the module and its objects, main `a5d79f2` and this
change; the kernel and game worker modules are byte-identical]. Its PAF item shrinks too: the
network path, now Google Lens alone, keeps one connection slot instead of two, so its result memory
goes from 571,888 to 548,096 B (`VJO_BUDGET_PAF_RESULTS_NET`; the host's `sizeof(VjoNetSlot)`,
23,800 B, rounded to 16 B; the Vita compiler's is 23,312 B) and the opt-in row's worst case from
731,632 to 707,840 B. The local path (one 64 KiB arena) is unchanged. No TLS root was the relay's:
it spoke plain HTTP.

The first scan's fixes (docs/first-ocr.md: the self-test's own background job, its records written
a record at a time, the press's settle cap, the done line's exchange and log times) take no
allocation and no item. A self-test record's group is a 2 KiB buffer on the stack of the thread
that writes it, and no thread's deepest chain changes: on VjoNet `vjo_selftest_stage_end`'s frame
is 2,432 B, called by `run_selftest_stages` after `vocr_run_one` (5,000 B) has returned; on
VjoControl the game record's chain is 3,324 B (`ctl_main` 136, `activate_game` 160,
`vjo_selftest_game` 2,120, `sample` 408, `report` 276, the group's flush and the logger's append
below it 224) of its 16 KiB, under the same thread's
config load (136 + 160 + `apply_config` 1,720 + `vjo_config_load` 128 + `vjo_config_parse`
1,760 = 3,904 B); `vjo_selftest_init`'s 3,148 B chain runs on the boot's thread after the same
config load [measured: `arm-vita-eabi-gcc -Os -fstack-usage -fcallgraph-info=su` with the build's
flags, the SDK's calls counted as 0]. The Shell's text goes from 206,192 to
206,240 B and its data and `.bss` from 71,020 to 71,072 B (the last text job's capture, the idle
time, the log's append counters), the same 282,624 B of pages: object text `shell/worker.c` +834,
`shell/selftest.c` +280, `shell/log_vita.c` +88, `core/config.c` +175 (the new key and its range),
paid for by building `core/jpegsw.c` (Lens's encoder, −888) and `core/utf.c` (−396; the Rubi
app's copy −392, not a plugin) with `-Os`. The worker's text goes from 70,124 to 70,812 B
(`game_ocr/main.c` +252, `core/vocr_ocr.c` +440: the warm-up and its steps, the NO_GAME grace, the
TEAR step's retry time) and its data and `.bss` from 14,200 to 14,208 B, the same 90,112 B of pages
[measured: the build log's module rows, main `f85b5d9` and this change, vita-vn-ocr `d214a05`].

The Rubi app check (#101, `core/app_stamp.h`) takes no allocation and no item: at each enabled
game's start, after `vjoSetGameActive`, the control thread reads two one-line stamp files into two
64-byte stack buffers (`shell/worker.c` `check_app`, `shell/files.c` `vjo_file_line`) and stats one file,
and the notice is a pointer to a constant string in the view model. `check_app` is inlined into
`activate_game`, whose frame grows from 160 to 240 B; its deepest new path, through `vjo_log`, is about
680 B, under the existing `apply_config` path (`apply_config` alone 1,400 B) [measured:
`arm-vita-eabi-gcc -Os -fstack-usage` with the Vita build's flags, main `e53d9fb` and this change; #123
and #133 changed neither `shell/worker.c` nor a header it includes but comments]. The
config warnings are compared with the previous load's in the two `VjoConfig` copies `apply_config`
already has. SceShell no longer writes a default `config.ini` when there is none (the installer, the
package and the Rubi app do), so the commented template (3,556 B with its NUL,
`.rodata.vjo_config_default_text.str1.1` of `core/config.c` under the Vita flags; `docs/config.example.ini`
is the same 3,555 B), its 8 B accessor and 60 B of `vjo_config_load` (180 to 120 B) leave the Shell, which
pays for the check: the build log's module row goes from 189,104 to 186,416 B of text and from 70,104 to
70,112 B of data and `.bss`, and the module from 266,240 to 262,144 B, one page fewer [measured: the Vita
builds of main `0b95c9d` (the same rows as on `c641bdf`) and of this change; the template string is in main's Shell ELF and not in this
change's]. `VJO_BUDGET_SHELL_MODULE_PAGES` stays 266,240 B, the ceiling, which concurrent changes to the
Shell share. The check's file I/O is not measured on a console: two open/read/close and one getstat on
`ux0:` at each enabled game start, and one log append when the answer changes, about 30-90 ms on
VjoControl after the game is active (an estimate from [docs/performance.md](performance.md)'s console
figures: 15.3 ms per open/seek/read/close, 9.4-47 ms per append); an L+R in the same control tick waits
that long (est.).

The bundled dictionary's frequency ranks (#127: the format 3 reader, the rank badge, its
`frequency_badge` switch and the ranked files' usual spellings) add no allocation, thread or table
held in memory: the rank is read with each payload the lookup already reads, and the tables are
constant strings in the module. The build log's module row goes from 186,416 to 187,552 B of text and
from 70,112 to 70,128 B of data and `.bss` (`VjoView.frequency_badge`), the same 262,144 B of pages
(864 and 3,600 B free) [measured: the Vita builds of main `ac905b1` and of this change].

Bee's Ultimate Dictionary (the bundled dictionary's title, its kanji lines in the popup, the
entries' dictionary name on Anki cards and in the log) adds no allocation, thread or resident table:
the title is read with the header (page 0 through the page cache when a lookup opens the file,
256 B instead of 128 B when a dictionary page is read, still one read), each of the 8 `Dictionary`
slots of the lookup's scratch keeps its 64 B (`sizeof(Workspace)` 45,872 to 46,384 B of the 48 KiB
budget, compile-time checked), and the shown entry's copy of it (64 B at most) is in its result.
The 64 KiB result arena's peak goes from 54,416 to 54,928 B, and from 62,608 to 63,120 B with two
text copies [measured: `largest_entry_and_input_fit_64k`, main `1ed95ca` and this change; the same
peaks in `tools/dict_lookup_bench.py` on main `dcb7b31`]. The
popup's text arena (`VJO_BUDGET_PAF_UI`) is unchanged: the body keeps its 4,096 units and 96
spans, of which the fullest body with an entry of the bundle takes at most 66 [host test
`every_bundled_entry_fits_the_popup`]. The change's own code is 606 B of the debug build's text and
598 B of the normal one's (`vjo_styled_entry` and the kanji block 404 B, the title's page-0 read 72 B,
the Anki name 76-100 B; per function, `arm-vita-eabi-nm -S`). The module rows go from 98,684 to
99,276 B of text in the debug build, the same 176,128 B of pages (3,124 and 3,868 B free), and from
88,132 to 88,724 B in the normal build, the same 163,840 B (1,388 B of text room); kernel and game
worker unchanged in both [measured: the Vita builds of main `c354cfb` and of this change]. (On main
`aac865c`, before #156 built `core/local_dict.c` for size, the normal build had 100 B of text room and
this change alone took it a page over its budget.) Stack frames (`arm-vita-eabi-gcc -fstack-usage
-fcallgraph-info=su`, each file's flags in the Vita build, main `c354cfb` and this change):
`vjo_local_read_page` 416 to 608 B (its `Dictionary` holds the title), `vjo_styled_entry` 40 to 80 B,
`vjo_anki_note_from_entry` 40 to 136 B, `parse_header` 40 to 48 B, `log_lookup` 280 to 288 B (debug
build), `do_save` 680 to 584 B (its 96-byte name buffer is gone: the note takes the entry's own
dictionary name). The threads' deepest static chains: VjoNet 2,664 B in the normal build (a lookup) and
5,504 B in the debug build (an OCR job), VjoControl 3,752 B, all unchanged, and VjoAnki 1,856 to 1,760 B
in the normal build and 1,872 to 1,776 B in the debug build: every chain through `do_save`, the
screenshot's below among them, is 96 B shallower.

Removing Google Lens (the overlay is offline; the last build with it is the tag
`archive/online-last`, branch `archive/online`) gives SceShell back 21 more pages: the module goes
from 262,144 B (266,240 B declared, the ceiling above) to 176,128 B, and
`VJO_BUDGET_SHELL_MODULE_PAGES` is lowered from 266,240 B to it, so the pool's permanent pages go
from 356,352 to 266,240 B and its worst case from 425,984 to 335,872 B. The packaged text segment
loses 87,208 B (188,000 to 100,792 B: `.text` 128,904 to 62,176 B, `.rodata` 54,544 to 34,120 B)
and data and `.bss` lose 72 B (70,132 to 70,060 B) [measured: the build log's module rows and
`arm-vita-eabi-size`, main `6020c0b` and this change; the kernel and the RCO are byte-identical, the
game worker has the same pages]. Where the 87,208 B went, by object: each object's input sections
that the link keeps in the code segment (`.text`, `.rodata`, `.ARM.exidx`, the import stubs), from
linker maps of the two builds (the same link commands with `-Wl,-Map`; the ELFs are byte-identical
to the builds'). An object's row has its own sections only: the string literals of every object
are merged into two pools (`.rodata.str1.1`, `.rodata.str1.4`) that the map lists under the first
object to have one, `shell/boot.c` and `buttons_vita.c`, so the literals that went with an object
are in those two rows. Kept input sections, the maps' fill and the gaps between output sections
(`readelf -S`) make the ELF's code segment [measured]:

| objects | main | this change | Δ |
|---|---:|---:|---:|
| BearSSL: its TLS engine, X.509, ciphers, curves, MD5, SHA-1 and SHA-384/512, HMAC, the DRBG (all of `third_party/bearssl`) | 66,553 | 1,496 | −65,057 |
| `core/tls_roots.c`: the 15 trust anchors | 6,324 | 0 | −6,324 |
| `core/http.c`, `core/net.c` | 3,996 | 0 | −3,996 |
| `core/jpegsw.c`, `shell/capture.c` | 3,350 | 0 | −3,350 |
| `core/lens.c`, `core/pb.c` | 2,138 | 0 | −2,138 |
| `core/tls.c` | 472 | 0 | −472 |
| the network pipeline: `shell/worker.c`, `core/local_dict.c`, `core/entries.c`, `core/client.c`, `core/textfilter.c` | 38,879 | 35,071 | −3,808 |
| the merged `.rodata.str1.1` pool, which the map lists under `shell/boot.c` (with boot.c's own 360 B of code, unchanged): the literals of Lens, HTTP, the connections, the JPEG encoder, the capture, the engine choice and the worker's network jobs | 17,223 | 15,662 | −1,561 |
| the merged `.rodata.str1.4` pool, listed under `buttons_vita.c` (with its own 120 B, unchanged): BearSSL's TLS PRF labels (`client finished`, `master secret`, …) and others | 6,741 | 6,615 | −126 |
| the rest: `anki.c` −60, `config.c` −44, the two capture syscall stubs −32, linker stubs −16, `selftest.c` −4 | | | −156 |
| kept input sections, total | 185,204 | 98,216 | −86,988 |
| alignment fill between them (the maps' `*fill*`) | 252 | 56 | −196 |
| gaps between output sections (`readelf -S`: `.text`'s end to the 16-aligned import stubs) | 8 | 0 | −8 |
| the ELF's code segment (`readelf -lW`) | 185,464 | 98,272 | −87,192 |
| the SCE module data the packaging adds (the packaged code segment less the ELF's; two capture syscall imports fewer) | 2,536 | 2,520 | −16 |
| the packaged code segment (the build log's module row) | 188,000 | 100,792 | −87,208 |

What is left of BearSSL is SHA-224/256 (`sha2small.c` and its two byte-order helpers, 1,496 B; its
`br_*` symbols 1,082 B), for the Anki queue's IDs. Its `br_*` symbols alone go from 35,526 to
1,082 B (`arm-vita-eabi-nm -S --defined-only`, 200 symbols to 10); the rest of its 65,057 B are
its static tables and helpers without the prefix. What went:

- the Lens client and its protobuf codec, the worker's capture → JPEG → Lens job and its
  connection warm-up, and the engine choice (`ocr_backend`) with its messages;
- the Shell's capture and JPEG encoder (1,392 B of `vjo_jpeg_encode` alone) and the capture lock a
  local OCR job held: AnkiConnect's removal had already taken the Anki screenshot, its other user;
- the worker's network pipeline: the two 256 KiB result arenas, the connection pool, the
  whole-sentence lookup only its jobs made (`vjo_overlay_lookup`, `vjo_local_lookup`,
  `vjo_entries_build`), the cached-sentence comparison;
- with Lens the Shell's last TLS connection, so the link (`--gc-sections`) no longer keeps
  BearSSL's TLS, the trust anchors or the HTTP client. Their sources went with PR-B (below).

The PAF items lose the network row: `VJO_BUDGET_PAF_RESULTS_NET` (548,096 B with one slot) and
`VJO_WORST_SHELL_PAF_NET` (707,840 B) are gone, and a job's lookup takes the one 64 KiB arena. The
kernel's capture syscalls (`vjoRequestCapture`, `vjoReadRaw`) and its full-frame block
(`VjoCapture`, 2,088,960 B at most) stayed, unused, until their own removal, which changed the
kernel API (API 24, [pool "kernel"](#pool-kernel-scephymempartkd-36-mib-shared-by-every-driver)).

Then the network stack itself went (PR-B): the sources the link had stopped keeping
(`core/net.c`, `core/http.c`, `core/tls.c`, `core/tls_roots.c` and `third_party/roots`, BearSSL
but its SHA-1 and SHA-224/256), and the network's last users, which it still kept: the sockets,
DNS resolver and cancellation of `shell/platform_vita.c` (with its random numbers and RTC time,
only TLS's), the UDP log (`log_host`, a removed setting now, and `tools/udp_log_listener.py`) and
the worker's `VjoNetCancel`. No module imports a network library any more, which the Vita build
checks (`tools/check_offline_imports.py`; the Rubi app's three newlib references to SceNet are
defined in `settings/no_net.c`, docs/shell-slim.md). The Shell imports no SceNet or SceNetCtl: 17 function stubs
fewer (72 → 55 `sce[A-Z]*` names by `arm-vita-eabi-nm`), three import libraries fewer (SceNet,
SceNetCtl, SceLibRng's `sceKernelGetRandomNumber`) [measured: `nm` and `readelf -S` of the two
builds]. The module keeps its pages: the text segment goes from 101,488 to 99,268 B (`.text` 62,640
to 61,288 B, `.rodata` 34,328 to 34,048 B) and data and `.bss` from 70,076 to 69,772 B (by symbol: three `VjoConfig` copies' 64 B
`log_host`, 192 B; the UDP socket's state, 20 B; `VjoNetCancel`, 12 B; the worker's and the Anki
job's `VjoPlatform`, 32 B each; `cache_data`'s two `VjoErr`, 8 B each) [measured: the build
log's module rows and `arm-vita-eabi-size`, main `3aa08a6` with #132, and this change; the kernel and
the RCO are byte-identical, the game worker has the same pages]. In the normal build
(docs/release-variants.md) the same removal takes 2,244 B of text (91,464 to 89,220 B: `.text` 56,528
to 55,168 B, `.rodata` 30,440 to 30,152 B) and 304 B of data and `.bss` (70,004 to 69,700 B), and one
text page with them: its module goes from 167,936 to 163,840 B, and its budget, which the check reads
exactly, with it [measured: the normal builds of the same two trees; by object the same Δs as
below but `shell/worker.c`'s, −108 B (11,224 to 11,116 B), with the normal build's own sizes where
they differ: `shell/boot.c`'s pool 12,247 to 11,996 B, `shell/log_vita.c` 1,320 to 1,200 B,
`core/local_dict.c` 19,889 to 19,881 B and `shell/anki.c` 2,844 to 2,840 B; fill 51 to 46 B, no gaps,
SCE data 2,520 to 2,196 B]. By object, as above (linker maps of the two builds, the ELFs byte-identical):

| objects | main | this change | Δ |
|---|---:|---:|---:|
| `shell/platform_vita.c`: the sockets, resolver, cancellation, random numbers and RTC time | 1,324 | 216 | −1,108 |
| the 17 import stubs: 15 SceNet, `sceNetCtlInetGetState`, `sceKernelGetRandomNumber` | 272 | 0 | −272 |
| the merged `.rodata.str1.1` pool (listed under `shell/boot.c`): the sockets', resolver's and UDP log's names, the RTC and TLS messages | 15,864 | 15,613 | −251 |
| `shell/log_vita.c`: the UDP log | 1,408 | 1,288 | −120 |
| `shell/worker.c`: the network thread's cancellations | 14,300 | 14,196 | −104 |
| `core/client.c`: the network errors' texts | 840 | 800 | −40 |
| the rest: `core/local_dict.c` −8, `shell/anki.c` −4, `core/config.c` −4 | | | −16 |
| kept input sections, total | 98,910 | 96,999 | −1,911 |
| alignment fill between them (the maps' `*fill*`) | 50 | 57 | +7 |
| gaps between output sections (`readelf -S`: `.text`'s end to the 16-aligned import stubs) | 0 | 8 | +8 |
| the ELF's code segment (`readelf -lW`) | 98,960 | 97,064 | −1,896 |
| the SCE module data the packaging adds: three import libraries and 17 functions fewer | 2,528 | 2,204 | −324 |
| the packaged code segment (the build log's module row) | 101,488 | 99,268 | −2,220 |

VjoAnki's stack goes from 64 to 16 KiB with it: 64 KiB was BearSSL's TLS, for AnkiConnect and an
`https` audio source, both gone with #125, and the thread's deepest chain of the Shell's own code is
1,728 B, a save's queue ID (`job_main` 24, `do_save` 672, `vjo_queue_save` 416, `vjo_queue_id` 160,
SHA-256 456), with its indirect calls' file callbacks [measured: `arm-vita-eabi-gcc -fstack-usage
-fcallgraph-info=su`, main `3aa08a6` with #132, and this change; the SDK's own code is not counted]. 16 KiB is
VjoControl's stack, whose chain is 3,744 B (3,784 B through its one indirect call, the config's
`local_dictionaries` validator) and which makes the same kinds of SDK calls (sceIo, the
log, Paf's heap). The shell pages' worst case, which an Anki job sets, goes from 286,720 to 237,568 B
in the debug build and from 278,528 to 225,280 B in the normal one, whose permanent pages go from
208,896 to 204,800 B with its module [measured: the ledgers the two builds print]. API 23's Anki
screenshot request (the JPEG is the game worker's) keeps its 5,404 B in the job's Paf arena. With it the
thread's deepest chain is 2,128 B in the debug build (`take_shot` 352 B, then its screenshot line's
`vjo_log` and file write), and 1,920 B in the normal build and for a card without a screenshot
(`vjo_queue_save` 600 B, with the card's two IDs and its JPEG's path) [measured: the same tools on both
builds, main `dcb7b31` and this change].

VjoNet's stack goes from 64 to 16 KiB (#132): 64 KiB was for BearSSL's TLS, and since Google Lens's
removal nothing on the thread makes a TLS connection or reaches BearSSL, the HTTP client or a TCP
socket (the UDP log's send was the one network call left, until the network stack went). Its
deepest chain of the Shell's own code is 5,624 B: `net_main` 144, a job's `vocr_run_one` 5,000 (its
log batch), then `vjo_log` down to the file write 480 [measured: `arm-vita-eabi-gcc -fstack-usage
-fcallgraph-info=su`, main `32bfc6a` and #132; the SDK's own code and the indirect-call sites are
not counted], 5,616 B without the UDP send (`vjo_log_raw` 8 B smaller). `VJO_BUDGET_SHELL_NET_STACK` goes from 69,632 to 20,480 B with its TLS, so SceShell's
permanent pages go from 266,240 to 217,088 B and the worst case from 335,872 to 286,720 B. The
module keeps its 176,128 B: 101,488 B of text (464 B more, the paint and the check) and 70,076 B
of data and `.bss` [measured: the build log's module row]; it imports one function more,
`sceKernelGetThreadInfo` (`sceKernelGetThreadId` was already imported, by `shell/log_vita.c`).
Because nothing measures the thread on a console yet, VjoNet paints its unused stack when it starts
(`shell/worker.c` `net_stack_paint`: from 64 B above the stack's low end up to 512 B below its own
frame, so only when its frame is above pStack + 576 and below the top, and when the kernel reports
the 16,384 B it was created with; else one `VjoNet stack: not painted (…)` line) and at each game
exit logs, when the figure grew, `VjoNet stack: at least N of 16384 B used` (`net_stack_check`; the
first wording said "at most", which `--collect` still reads). N is a lower bound once anything has
used the stack below the paint (any job): a live byte that happens to hold the pattern makes the scan
read low, and use of the unpainted 64 B cannot show, so the largest figure is 16,320 B. Until then N
is the paint's end, which counts the 512 B gap below the painting frame as used: up to 512 B above
the truth [estimate: the frames below the painting one are SDK calls, unmeasured; on the host the
idle thread wrote nothing below the paint's end + 473-512 B, audit-overlay-shell-slim-4 N1]. `--collect`'s analysis keeps the largest figure, WARNs when under a
quarter is left, and WARNs for each boot that did not paint. The paint takes no allocation and no
item. Its lowest 64 B stay as the kernel left them. The PSP's thread manager, whose API the Vita
keeps, fills a new stack with 0xFF and writes the thread's UID in its lowest word, and has a status
for a thread it killed for a stack overflow [pspsdk `PSP_THREAD_ATTR_NO_FILLSTACK` and
`PSP_THREAD_KILLED`; PPSSPP `sceKernelThread.cpp` `FillStack`, whose free-size scan starts at 0x10,
12 B past that word]. The Vita detects
stack overflows too: a core dump records one as its cause and each thread's "Maximum stack size
used" [SDK: Core_Dump-Overview], and `sceKernelCheckThreadStack` gives a thread its remaining stack
[SDK: Kernel-Reference]. How it detects them is unknown; nothing found puts a marker in a stack's
low bytes, and 64 B is 16 times the PSP's UID word. Should a first boot of this build still end in a stack overflow or SceShell
crash naming VjoNet, the paint itself is the first suspect, not the 16 KiB; and a core dump's
VjoNet figures read the painted area as used. The boot guard makes the boot after it safe.

### Pool "kernel": `ScePhyMemPartKD` (36 MiB, shared by every driver)

| item | bytes | what | source |
|---|---|---|---|
| kernel module pages | 118,784 | `VitaJPOverlay_Kernel.skprx` (the debug build; the normal one's below): 40,960 text + 77,824 data/bss (the 32 KiB model staging buffer and the 4 KiB log ring are in it); API 22's frame-pacing step adds 272 B of text (36,608 B) and 48 B of data/bss, the same pages [main 8f1ec8f]; the worker loader (#138: the kernel loads the game worker into every game ON in `games.ini`) adds one text page: `kernel/loader.c` 2,354 B of text and 48 B of bss, `core/games.c` 1,251 B (the Shell's own policy code), the registration's one-thread check 150 B and 8 B, and five more `SceSysclibForDriver` stubs, 36,640 → 40,648 B of text; data/bss 77,768 → 77,824 B fills its pages (one more byte takes a page); API 23's screenshot (its colour rows and its JPEG's file broker, which uses the same staging buffer) within the same pages, paid for by the rest of the kernel's cold code built for size (the syscalls and the worker's loop, the boot guard, the kernel strip, the log) and the request's model directory 256 → 128 B: 40,648 → 40,040 B of text and 77,824 → 77,672 B of data/bss, the data pages no longer full (the normal build 36,444 → 35,900 and 76,136 → 75,984 B, the same 114,688 B) [the build log's module rows, main `dcb7b31` and this change, both builds]; audit-hooks F1 and F3 (`kernel/text_source.c`: the loader's table of the 4 processes it started the worker in as ON, 20 B of `.bss`, and the text source's word check) make it 40,660 → 40,804 B of text and 77,704 → 77,724 B of data/bss, the same pages (156 and 100 B free; the normal build 35,808 → 35,952 and 76,016 → 76,036 B, the same 114,688 B) [the build log's module rows, `feat/native-hooks` `c8cc539` and `hooks/fix-kernel` `3c39dbb`, both builds] | packaged SELF segments [measured: the build log's module rows, main 3aa08a6 and #138; the same on main c08db18] |
| VjoWorker stack | 16,384 | permanent | `kernel/main.c` |
| VjoFile stack | 16,384 | permanent in a normal boot; absent in recovery. The file worker owns broker I/O, reserve preparation, boot-record writes and log flushing so storage cannot block the controller poll. This is the requested stack size from source, not a console headroom measurement. | `kernel/io_worker.c`, `VJO_STACK_KERNEL_IO` |
| VjoOcrStrip | 65,536 at most | the capture ladder's last rung: one strip for one `vjoOcrCaptureRows` call, only when the display hook could not write the game's memory, freed before the call returns (a copy the call abandoned keeps it until the copy ends, when the kernel worker frees it); counted in the worst case since API 24 (before, it was never held with `VjoCapture`, the larger) | `kernel/buffers.c` |
| VjoPolicy | 24,576 | #138: a starting game's `games.ini` read by the kernel worker, `KERNEL_RW`: the parsed table (8,200 B) and the file's text (at most 12,288 B), page-rounded; taken for a candidate game's start only and freed before that start goes on, not refused while a strip is held, so the worst case counts both. Refused: the game starts without the worker, and `kernel.txt` says so | `kernel/loader.c` `policy_alloc`; the fit is checked at compile time |
| **limit** | **241,664** | today's worst case (the debug build's; the normal build's is 237,568 B), including the file worker's 16 KiB stack: a **ratchet**, not a cited safe size. It was 2,248,704 B with the full-frame capture until API 24 (2,220,032 B before #138's page and block). The pool's free space on a retail console is unmeasured; a 2,088,960 B capture failed there (#14, #74) and succeeded in other sessions. The self-test reads the partition kernel-side and replaces this number | [vita-memory.md 5.3](vita-memory.md) |

**API 24** removed the kernel's full-frame capture: `vjoRequestCapture` and `vjoReadRaw`, the
`VjoCapture` block (2,088,960 B at most, 458,752 B at 917 × 125), `kernel/capture_request.c`, the
display hook's copy, the worker's completion and timeout, and `VjoState`'s `alloc_status`,
`capture_result` and `raw_stride`. Its last users were Google Lens's JPEG (#130) and the Shell's Anki
screenshot (#125); OCR reads its region in the game since API 19, and an Anki card's screenshot since
API 23. The kernel pool's worst case goes from 2,248,704 to 225,280 B (debug) and from 2,244,608 to
221,184 B (normal), and the limit with it. The kernel's text goes from 40,040 to 37,964 B and its
data/bss from 77,672 to 77,584 B (normal: 35,900 → 33,840 and 75,984 → 75,896 B), the same pages, and
the Shell's from 100,484 to 100,396 B of text (normal: 90,012 → 89,924 B), the same pages [measured: the
build log's module rows and ledger, both builds of main `836259e` (#150) and API 24]. `tools/check_vita_syscall_abi.py`
fails a module that still exports or imports either syscall.

### Pool "game": `ScePhyMemPartGame` (256 MiB minus the game's own use)

| item | bytes | what | source |
|---|---|---|---|
| OCR worker pages | 192,512 (normal), 200,704 (debug, with its PSP probe) since API 50's line finder fixes and the pin at vita-vn-ocr f8a1856 ([above](#line-finder-fixes-api-50-vita-vn-ocr-129-and-the-pin-at-f8a1856)); 184,320 / 192,512 since API 47's vertical text ([above](#vertical-text-api-47)); 180,224 / 188,416 since API 42's head gate and two-level head ([above](#head-gate-and-two-level-head-api-42-vita-vn-ocr-120-and-122)); 172,032 / 180,224 since vita-vn-ocr #109's counter re-decode ([above](#counters-vita-vn-ocr-109)); 167,936 / 176,128 since API 34's remembered hooks; 159,744 / 167,936 since API 31 | the module, in each game ON in `games.ini` since #138 (the kernel loads it; before, each game it was registered for): 126,976 text + 32,768 data/bss in the normal build, 135,168 + 32,768 in the debug build with its PSP probe, as declared for PR #234's native hooks (their engine, what it links of Luna's table, and the 20 stream slots' static 10,960 B, [native-hooks.md](native-hooks.md#memory)); 98,304 B (81,920 text + 16,384 data/bss) in both builds from API 26; raised from 45,056 B for the in-game capture (strips, ladder, probe, step log), then from 53,248 B by two text pages for API 20's job policy (the stable-text wait and the blank-line check) and the text normalisation (`core/textnorm.c`, run in the game so that SceShell's pages stay as they are), then by two more for the beam + LM decoder (vita-vn-ocr's `runtime/vocr_lm.c` and `vocr.c`'s top-K extras) and the confidence cascade (`core/vocr_ocr.c` built for size: 59,692 B of text [`bcd3cf0`]), then by one more for the finder refinement (vita-vn-ocr #83's `runtime/vocr_lines_refine.c`: 65,084 B of text [`fec85af`], 452 B under the page), then by one text and two data/bss pages for API 22's parallel lines, line cache and clock (text 62,012 → 67,452 B on vita-vn-ocr a540027, after #108's `-Os` for `game_ocr/main.c`; data/bss 5,840 → 14,200 B with the 6,160 B line cache and the 2,080 B line queue and slots [main `8f1ec8f` and this build]), and by one more text page for vita-vn-ocr #85's faster W8I kernels, the runtime pin (text 67,452 → 70,124 B; the build's `vocr.c` object 15,288 → 17,952 B of code), in the game's partition, never SceShell's; #138's one-copy check (`another_copy`, `outside_segments`) adds 284 B of text in both builds (71,088 → 71,372 B debug, 69,724 → 70,008 B normal), the same pages [the build log's module rows, main `c08db18` and #138]; API 23's Anki screenshot takes one text page more, 94,208 B in both builds: its JPEG encoder (`core/jpegsw.c`, 4,038 B; `core/arena.c`, 518 B) and the shot job (text 71,372 → 77,004 B, data/bss 14,208 → 14,160 B with the request's model directory halved; the normal build's text 70,008 → 75,688 B [the build log's module rows, main `dcb7b31` and this change, both builds]); #194's short-line probe (`core/vocr_ocr.c` `probe_short_lines`) one text page more, 98,304 B: the debug build's text 77,196 → 78,076 B [the build log's module rows, main `49310ed` and its parent]; API 26's text-presence gate (vita-vn-ocr #100's `runtime/vocr_textgate.c` at the runtime's `-O2` and `core/vocr_ocr.c`'s use of it, its 1,540 B of weights a file in `ocr/` read into a `.bss` block: 3,488 B of text and 1,560 B of data/bss in both builds, on or off) one text page more in the normal build only, 98,304 B in both: the debug build's 78,156 + 14,160 → 81,644 + 15,720 B in the same pages, the normal build's 76,856 + 14,160 → 80,344 + 15,720 B [the build log's module rows, main `1de7b67` and API 26, both builds]); then API 31's job path ([native-hooks.md](native-hooks.md), which text a job shows: it decodes the streams' texts, linking the decoder and its CP932 table, `core/hook_text.c`, 12,958 B, and Luna's filters, `core/hook_re.c`, 3,407 B, and adds the selection, `core/hook_select.c`, 1,708 B, and a 3,064 B static snapshot of the streams [object sizes]) one data page and seven text pages more in each build: the debug build's 99,016 + 27,496 → 127,144 + 30,560 B (131,072 → 163,840 B), the normal build's 96,524 + 27,496 → 124,668 + 30,560 B (126,976 → 159,744 B) [the build log's module rows, hooks/select `4f9dca9` with hooks/engine `dca6d5f` merged] | packaged SELF segments [measured: standalone native-hooks builds described here; combined build pending] |
| VjoGameVocr stack+TLS | 36,864 | 32,768 + 4,096 | `game_ocr/main.c` |
| VjoGameVocrWeights | 2,478,080 at most | the largest accepted model, the Large size's VOCR5-L (2,477,472 B since API 42: v0.3.0's 2,473,696 B with its head gate and two-level head, [above](#head-gate-and-two-level-head-api-42-vita-vn-ocr-120-and-122); `VJO_VOCR_MAX_BYTES`), page-rounded; `weights_alloc` refuses a larger block; resident between jobs. Raised from 1,560,576 B (F20's shape, Large before VOCR5), then from 2,473,984 B (v0.3.0's VOCR5-L); Normal's block is 1,191,936 B since API 42 (1,187,840 B before) | `include/vjo_vocr_sizes.h`, `core/vocr_ocr.c`; asserted in `test_ledger.c`, `test_game_vocr.c` |
| VjoGameVocr workspace | 1,220,608 at most (1,212,416 before API 42) | per job, at any region, page-rounded: since API 19 it holds no grey copy of the region (770,048 B with one at 960 × 544); since API 20 with the beam + LM decoder's extras (the top-K of every frame, the LM context and the beam workspace, 56,928 B), 4 KiB for the greedy text, and with the cascade its extras (the next size up's arena, structs and decoder: VOCR5-L's 316,160 B beside VOCR5-N; a request by name's VOCR4-W20, 296,192 B), and with the finder's boxes refined its arena (72,400 B at 960 × 544, 16,768 B at 883 × 112): 637,280 B for VOCR5-N with VOCR5-L's cascade; since API 22 ended by the line slots of the job's helpers (`ocr_threads`, at most two: the worker's own slot, 68,160 B, and per helper its recognizer arena, context and slot, 253,104 B for VOCR5-N), taken only when the memory check passes with them: 1,218,240 B for VOCR5-N with two and VOCR5-L's cascade, the largest since API 50's runtime pin, vita-vn-ocr f8a1856, whose structs are larger (1,218,048 B since API 42's head gate and two-level head, 1,211,648 B before) (1,191,680 B with H15 and VOCR4-W20 before VOCR5) | `vjo_vocr_job_bytes` + `vjo_vocr_refine_bytes` + `vjo_vocr_lm_job_bytes` + `vjo_vocr_cascade_job_bytes` + `vjo_vocr_helpers_bytes`; asserted in `test_ledger.c` |
| VjoGameVocrHelper stacks+TLS | 40,960 at most | API 22: 16,384 + 4,096 per helper thread, at most two, each created for one line and deleted when its line is placed; none between jobs | `game_ocr/main.c` `pool_run` |
| VjoGameVocrCascade | 2,478,080 at most | the confidence cascade's larger model, the next model size up of the job's since API 21 (VOCR5-L's 2,477,472 B for Normal since API 42, 2,473,696 B before; VOCR5-N for Small; Large has none; a request by name's VOCR4-W20), page-rounded; `cascade_alloc` refuses a block larger than the Large size's; `USER_RW` only, resident between jobs like the weights, read at the first doubtful line or by a job that found no line (the warm-up), taken only when the memory check passes with it. Raised from 1,560,576 B (VOCR4-W20), then from 2,473,984 B (API 42's VOCR5-L). With the weights row it counts 1,286,144 B more than a job can hold (a job's weights and its cascade's are at most Normal's and Large's: 1,191,936 + 2,478,080 B), kept so that each item's worst case is its own | `include/vjo_vocr_sizes.h`; `test_ledger.c`, `test_game_vocr.c` |
| VjoGameVocrLM | 3,604,480 at most | the largest pinned LM (`vocr_lm6_o3_4000k.vlm`, 3,603,552 B; ctclm.2's 4 MB LM was 3,781,072 B), page-rounded; `USER_RW` only, resident between jobs like the weights, taken only when the memory check passes with it (a smaller tier, or none, otherwise) | `include/vjo_vocr_sizes.h`; `tools/tests/test_vocr_defaults.py` |
| VjoGameVocrCapture | 65,536 at most | one strip of grey rows and, per region row, its checksum, its recent states (16, or as many as fit) and two bits: at most 65,536 B at any region (`VJO_VOCR_CAPTURE_LIMIT`; 65,480 B at 960 × 544, 61,388 B at 917 × 125), per job; `USER_RW`, else CDRAM, else phycont | `vjo_vocr_capture_bytes` |
| VjoGameSc3 | 16,384 at most | an SC3 hook's charset ([native-hooks.md](native-hooks.md#sc3-text)): the game's glyph table, its file page-rounded (8,192 B for PCSG00146 and PCSG00352, 16,384 B for PCSG00555), read at the first job of a game with an SC3 site and held until the worker stops, `USER_RW` only, beside the 1 MiB hook-only reserve; in no other game | `game_ocr/main.c` `hooks_sc3_load`; `host/tests/test_game_hooks.c`, `test_ledger.c` |
| VjoGameShot | 450,560 at most | API 23, an Anki card's screenshot: the JPEG encoder's state (4,096 B at most), one strip of 64 colour rows at 960 px (245,760 B) and the JPEG (196,608 B at most), page-rounded, only while the screenshot runs; `USER_RW`, else CDRAM, else phycont, after its own check that the game keeps 4 MiB beside it. PR #235's trimming scans the strip and encodes its picture sub-rectangle in this same block, with no additional buffer. A screenshot is a job of the one job slot, so this block is never held with an OCR job's workspace, capture block, tall line or helpers (1,843,200 B of the job's worst case since API 42's workspace, 1,835,008 B before): not in the sum | `game_ocr/main.c` `SHOT_BLOCK_BYTES`, checked against the budget at compile time; `host/tests/test_game_shot.c` |
| VjoGameVocrLine | 516,096 at most | a line taller than a strip (a finder band), kept whole at its own width × height, only when one occurs (41 of 372 linefind dev regions; 78,884 B at most there), and since API 47 for most columns of a job of columns: a column's crop, at most 57,344 B (`VJO_VOCR_ROTATE_BYTES`: a larger column is left out before it is fetched), one block at a time, given back with the job and before each of `auto`'s retries; `USER_RW`, else CDRAM, else phycont (in their 256 KiB and 1 MiB units); sized as what fitted the API 19 job limit (2,392,064 B) with F20, the workspace and the capture block, unchanged since | `core/vocr_ocr.h` `VJO_VOCR_LINE_MAX_BYTES`; `host/tests/test_game_vocr.c` |
| **limits** | weights 2,502,656; workspace 1,220,608; job 10,604,544 | the recognizer's ceiling: weights at most the owner's 2.5 MB for the Large size (2026-10-14 18:09Z; 2,500,000 B page-rounded; 1,601,536 B, Normal's 1.6 MB, before VOCR5's Large), which the preflight (free `USER` minus the job's reserve: 4, 8 or 16 MiB by the largest model size it holds since API 21, `VJO_GAME_RESERVE_*`; memory left to the game, never taken) has to cover before anything is allocated, and since API 20 the 4 MB LM and the cascade's model (the job limit raised from 2,392,064 B by their blocks and workspace; a job takes either only when it fits, and #81's CLANNAD log shows 90,177,536 B free at the least); since API 22 the helpers' slots and stacks (614,400 B more at the worst; a job takes one fewer helper at a time, down to none, when the game is short, at the same reserve, 8,720,384 B); and for VOCR5's Large its weights and cascade budgets (913,408 B more each) and the larger workspace: 10,567,680 B, 11.7 % of that free `USER`; API 34's and #109's worker pages (12,288 B) and API 42's head gate and two-level head (the Large blob's page in both model budgets, the workspace's two pages and the worker's two: 24,576 B) make the limit 10,604,544 B, 11.8 %; VOCR6's 4 MB LM (vita-vn-ocr #127) lowers the LM budget by 180,224 B (a job's worst case is 10,403,840 B, 11.5 %; its real peak, VOCR5-N with VOCR5-L's cascade, the 4 MB LM and both helpers, 9,117,696 B, is 1,286,144 B less); an SC3 hook's charset (16,384 B at most, in three games) makes the worst case 10,420,224 B, under the same limit | the owner's ceilings (prompts' rules, 2026-10-14 18:09Z); vita-vn-ocr [`docs/vita-memory-budget.md`](https://github.com/bee-san/vita-vn-ocr/blob/main/docs/vita-memory-budget.md); #81 [measured] |

## The two builds

The release ships two builds of one tree (docs/release-variants.md): the debug build (`VJO_DEBUG=ON`,
the test builds') and the normal build (`OFF`), which compiles the debug-only logging, the
self-test and the traces out. Their module pages differ, so the Shell's and the kernel's module
budgets have an `#if VJO_DEBUG` branch each in `include/vjo_ledger_budget.h`, which
`tools/check_memory_ledger.py --variant debug|normal` reads (the C code sees the same branch; any other
conditional naming `VJO_DEBUG`, an `#elif` or a macro defined twice in one build's lines is refused).
Every other budget is the same. The normal build's check is exact (`--exact`): a module with fewer
pages than its budget fails too, so the normal budgets are always the measured pages.
[measured: both builds of main `3805a13` plus the variants change, the build log's module rows;
since API 23's Anki screenshot both builds of main `c6e0a4d` and that change; the OCR row since API 26's
text-presence gate, both builds of main `1de7b67` and that change]

| module | debug | normal | |
|---|---:|---:|---|
| `VitaJPOverlay_Shell.suprx` | 176,128 B (100,484 + 69,828) | 163,840 B (90,012 + 69,756) | 3 SceShell pages fewer |
| `VitaJPOverlay_Kernel.skprx` | 118,784 B (40,040 + 77,672) | 114,688 B (35,900 + 75,984) | 1 page of the kernel pool fewer |
| `VitaJPOverlay_OCR.suprx` | 167,936 B (133,464 + 30,568) | 159,744 B (124,740 + 30,560) | 2 pages fewer (the debug build's PSP probe) (one budget for both from #124 to API 31; the normal build's raised for API 26's gate; both raised for API 31's native hooks, per build again) |

At main `3805a13` the normal Shell's text is 90,768 B in 94,208 B of pages (3,440 B of room); its
data/bss has 3,740 B. On main `3aa08a6` it is 91,016 B of text (#140), and #132's VjoNet stack paint and check make it
91,464 B of text and 70,004 B of data/bss, the same pages (2,744 and 3,724 B of room); the normal
build's shell pages are then 208,896 B permanent and 278,528 B at worst (258,048 and 327,680 B
before: VjoNet's stack is the same in both builds) [measured: the build log's module rows and
ledger, both builds of main `3aa08a6` and of #132]. With the network stack gone (#145, main
`c08db18`) the normal Shell is 163,840 B (89,220 + 69,700) against the debug Shell's 176,128 B
(99,268 + 69,772), three SceShell pages fewer [measured: CI's module rows of both builds, Release
run 38899320744], and the normal build's shell pages are 204,800 B permanent and 225,280 B at worst
[measured: its ledger].
#138's worker loader adds one kernel text page to each build: 118,784 B (40,648 + 77,824) debug and 114,688 B
(36,444 + 76,136) normal against main `c08db18`'s 114,688 and 110,592 B; the worker (71,372 and 70,008 B of
text) and the Shell (99,244 and 89,196 B) keep their pages [measured: both builds, main `c08db18` and #138].
The VjoNet line's "at least" (#157) makes the normal Shell's text 89,204 B (`.rodata` + 8 B), the same
pages; the debug Shell keeps 99,244 B [measured: both builds, main `dcb7b31` and #157].
API 23's Anki screenshot, on main `c6e0a4d`: the Shell 99,244 → 100,484 and 89,204 → 90,012 B of
text, 69,772 → 69,828 and 69,700 → 69,756 B of data/bss, the same pages (#157's 8 B fit in this build's
alignment: 90,012 B on `dcb7b31` too). The normal Shell's text then
has 100 B of room in its 90,112 B of text pages (908 B before): the next change that adds Shell code
makes room first. Its normal build logs a failed screenshot's reason only, which keeps it there. The
kernel 40,648 → 40,040 and 36,444 → 35,900 B of text, 77,824 → 77,672 and 76,136 → 75,984 B of
data/bss: the debug build's data pages, full with #138, have 152 B again. The worker 71,372 → 77,004
and 70,008 → 75,688 B of text, one page more in both builds: 94,208 B [measured: the build log's
module rows, both builds of main `c6e0a4d` and this change; the same on `dcb7b31`].
#138's left stick, on main `10c8011`: 792 B of the normal Shell's text (`frame()` +536 B, `open_page` +52,
`vjo_text_read_step` 58, `render` +36, `post_cursor_now` 28, `vjo_config_parse` +24), where that build had
100 B of room, so it would be 90,804 B, a page past the exact 163,840 B. `core/local_dict.c`, the
dictionary lookup, is now built `-Os` like the other core files (its object 27,407 → 24,958 B of text and
rodata): the normal Shell 88,236 + 69,796 B and the debug Shell 98,804 + 69,860 B, the same pages
(1,876 and 3,596 B of text room; 88,132 and 98,684 B on main `74f8de8`, whose Shell is 120 B smaller). On the Cortex-A9 lane (`tools/bench_stages.py a9`, the Vita compiler's
objects under qemu-arm, bit-identical output) a lookup runs 0.44 % (`lookup_first`, 134,948 → 135,545
instructions) and 0.60 % (`lookup_every`, 123,845 → 124,590) more [measured: the instructions counted under
qemu-arm, both normal builds], which vita-vn-ocr's Cortex-A9 cost model puts at most 2.1 µs at 333 MHz and
1.6 µs at 444 MHz [estimate], against a console lookup's 70-81 ms, nearly all file reads
([performance.md](performance.md)) [measured: both builds' module rows, main `10c8011` and #138]. The kernel and
the worker are byte-identical to main's in both builds.
API 31's plumbing for native text hooks ([guide](guide.md#native-text-hooks): the kernel's text source, the
Shell's `text_source`, hook pick, Triangle and label, the Rubi app's Text source and Games tag), merged with
`feat/native-hooks` `960ac20` (the engine, Luna's table, the selection), against `960ac20`: the kernel 40,484 →
40,660 B (debug) and 35,632 → 35,808 B (normal) of text and 77,700 → 77,704 and 76,012 → 76,016 B of data/bss (the
text source's one word), the same pages; the Shell 98,748 → 99,836 and 88,348 → 89,292 B of text (the config key and
its push, the session's hook pick, Triangle's cycle and the label, `core/hook_pick.c`, the logs) and 73,304 → 73,368
and 73,312 → 73,376 B of data/bss, the same pages (2,564 and 820 B of text room); the worker 127,144 → 127,216 and
124,668 → 124,740 B of text (its `vjoGetTextSource` import, which turns on the engine's `text_source()`, and the
kernel API check), the same 163,840 and 159,744 B (3,856 and 2,236 B of text room); no budget changes. The Rubi app's
loaded segments 4,835,200 → 4,838,528 B (debug) and 4,834,640 → 4,837,904 B (normal, `tools/check_settings_self.py`),
1,820 B of it Luna's title list (`vjo_luna_ids`, 182 IDs; `--gc-sections` drops the rest of `core/hook_luna.c` and
the filters) [measured: the build log's module rows and `check_settings_self.py`, both builds of `960ac20` and the
merge, VitaSDK gcc 15.2.0]. The interface (`a8fb6cf`) had already grown the request by 4 B and the result by 28 B:
the kernel's three static copies by 60 B of `.bss`, by `sizeof`.
API 26's text-presence gate ([vita-vn-ocr.md](vita-vn-ocr.md#text-presence-gate-api-26)), on main `1de7b67`: the
worker 78,156 → 81,644 B (debug) and 76,856 → 80,344 B (normal) of text and 14,160 → 15,720 B of data/bss in both.
The text is vita-vn-ocr's `runtime/vocr_textgate.c`, at the runtime's `-O2` (at `-Os` it is 564 B smaller and the
normal build would still take the page), and the job's use of it: reading the weights' file through the kernel's
broker and checking its SHA-256. The data/bss is the weights' 1,540 B block (`VjoVocrGateResident`, `.bss`): the
weights are a file in `ocr/`, not built in. The debug worker keeps its 98,304 B and the normal one takes a text page,
94,208 → 98,304 B, where it had 968 B of text room. The Shell 98,620 → 98,732 and 88,260 → 88,348 B of text
(`ocr_text_gate`: the config key, its request bit, the step's name in the debug log) and 73,264 → 73,272 and
73,264 → 73,280 B of data/bss, the same pages; the kernel 40,460 → 40,484 and 35,596 → 35,632 B of text (the
broker's gate file), the same pages [measured: the build log's module rows, both builds of main `1de7b67` and API 26,
VitaSDK gcc 15.2.0; main `b7fd816` changes docs only]. The gate's arena (35,529 B at 960×544) takes scratch the
finder leaves free: no job's workspace grows. The game's permanent figure is the ledger's own (135,168 B in the
debug build's table until API 32).
Standalone PR #235 (`de5f7e0`, its code unchanged from the measured `fb6ed5d`), API 32, the Anki screenshot's
black bars ([anki.md](anki.md#black-bars)), on main `75041a0` without native hooks (the worker's rows the same
on `f482c21` and `44d4e8d`): the worker's text 81,644 → 82,876 B (debug) and 80,344 → 81,608 B (normal), data/bss
15,720 B in both: `core/shottrim.c` (the scan, at `-Os`: 612 B) and `game_ocr/main.c`'s passes (`shot_pass`: the
first rows read for the guess, the picture's sub-rectangle encoded from the strip, the rows below scanned, a frame
read again when its rows come to another picture). The debug worker takes one more text page, 98,304 → 102,400 B;
the normal one keeps its pages, with 312 B of text room. In that standalone build the game's permanent figure is
139,264 B debug and 135,168 B normal, and no transient pool's worst case moves: the screenshot's block is still
450,560 B (the trimmed picture is a sub-rectangle of the strip, nothing copied). The Shell 98,812 → 99,068 and
88,428 → 88,500 B of text (the request's bit, the debug
log's bars line, `anki_screenshot_trim_bars` in the config parser) and 73,272 → 73,288 and 73,280 → 73,288 B of
data/bss (`VjoConfig`'s field in its copies), the same pages; the kernel the same bytes of text and data
[measured: the build log's module rows, both builds of main `75041a0` and PR #235 `fb6ed5d`, CI's container].
These are separate from PR #234's larger native-hook modules; the combined page counts must be measured, not
obtained by replacing either standalone budget or adding rounded page counts.

API 24 (the kernel's full-frame capture removed): the normal kernel 35,900 → 33,840 B of text and
75,984 → 75,896 B of data/bss in its 114,688 B, the normal Shell 90,012 → 89,924 B of text in its
163,840 B (188 B of room in its text pages), and the normal ledger's kernel worst case 221,184 B
[measured: the build log's module rows and ledger, both builds of main `836259e` (#150) and API 24].
The cursor across rows and the stick's four directions (the owner, 2026-10-16) take the overlay's two
cursor paths down to one: the stick's steps go through the arrows' `vjo_text_row_key`, and
`vjo_text_read_step` and the no-layout row double (`flat_position`) are gone. The normal Shell's
`core/text_rows.c` object goes from 1,520 to 1,334 B of text and rodata (`vjo_text_row_step` 304 → 294,
`vjo_word_step` 252 → 206, `vjo_text_row_key` 120 → 146, `vjo_text_row_vertical` 420 → 520 with `cell` 164
inlined into it, `vjo_text_read_step` 58, `flat_position` 18 and its 16 B row table gone; `walk`, `query` and
`vjo_text_selectable` unchanged), `frame()` 2,812 → 2,816 B (3,244 → 3,196 B in the debug build): the normal
Shell 88,636 → 88,460 B of text and the debug one 99,196 → 98,972 B, data unchanged (69,796 and 69,860 B),
the same pages (1,652 and 3,428 B of text room) [measured: both builds' module rows, `arm-vita-eabi-size -A`
and `nm -S`, main `a7abad7` and this change]. No
allocation, no new static; the kernel and the worker are byte-identical to main's in both builds.

Words across a line break and the overlay's own line breaks (owner 2026-10-16, item 4;
[touch-lookup.md](touch-lookup.md#words-split-over-two-lines)), on main `50e8d4e` (#179): the Shell
gets 1,830 B more code (the join rule,
`core/textjoin.c`; the line-break rule, `core/linebreak.c`; the re-wrap, `shell/wrap_fix.h` and its
callers in `overlay.cpp` and `strip.cpp`; `vjo_styled_insert`; the lookup's scan,
`vjo_local_lookup_pages` +52 B) and 261 B of new tables, where the normal Shell's code segment had
1,276 B of room. The read-only tables go to the data segment ("Tables in the data segment" below):
the line-break rule's 217 B, the join rule's 44 B, and from main's `.rodata` the lookup's
`preferred_readings` (924 B) and `usual_spellings` (310 B), so the code segment grows by 608 B in the build log's
text column (596 B of it in named functions and tables, `arm-vita-eabi-nm -S`). So:

| | main `50e8d4e` | this change | room left |
|---|---:|---:|---:|
| normal Shell, text | 88,836 B | 89,444 B (+608) | 668 B of 90,112 |
| normal Shell, data/bss | 69,884 B | 71,596 B (+1,712) | 2,132 B of 73,728 |
| debug Shell, text | 99,252 B | 99,908 B (+656) | 2,492 B of 102,400 |
| debug Shell, data/bss | 69,876 B | 71,588 B (+1,712) | 2,140 B of 73,728 |

The pages stay 163,840 B (exact) and 176,128 B; the data/bss growth is the 1,495 B of tables and
217 B of new state (the header's and the popup's shown texts and their re-wrap, 16 break positions,
the strip's 16 more units). The kernel and the worker are byte-identical to main's in both builds.
No allocation, thread, lock or PAF block is added: the overlay's text arena keeps its 20,480 B and
its 17,952 B worst case (the header's shown text stays in it across the renders that keep it, as a
render that rebuilds it holds it) [measured: the build log's module rows, both builds of main
`50e8d4e` and this change; per-function sizes with `arm-vita-eabi-nm -S`].

The left stick a character at a time (the owner, 2026-10-20; [touch-lookup.md](touch-lookup.md#the-left-stick-138)),
on main `f482c21` (#231; `44d4e8d`'s Shell is the same): the stick's left and right take a branch of their own in `vjo_text_row_key`
(`core/text_rows.c`, 146 → 214 B), the rest of the code unchanged. The Shell's text 98,732 → 98,812 B (debug)
and 88,348 → 88,428 B (normal), +80 B each, data/bss unchanged (73,272 and 73,280 B), the same pages:
176,128 and 163,840 B (exact), with 3,588 and 1,684 B of text room. No allocation, thread, static or PAF
block; the kernel and the worker are byte-identical to main's in both builds [measured: the build log's
module rows, both builds of main `f482c21` and `44d4e8d` and this change, VitaSDK 2026.08's gcc; `arm-vita-eabi-nm -S`].

### Tables in the data segment

The normal Shell's code segment had 1,276 B of room and its data segment 3,844 B (main `50e8d4e`). A read-only table
marked `VJO_DATA_TABLE` (`core/port.h`) is placed in the data segment (`.data.vjo.<name>`) by the
Vita build; it stays `const` and is never written, and the host build keeps it read-only (`.rodata`, or
`.data.rel.ro` for a table of pointers in a position-independent build such as CI's Ubuntu GCC makes by
default: read-only once relocated, a write faults). It costs
nothing more at run time (one copy, loaded with the module; the Shell is loaded into SceShell only)
and moves no page: it uses the data segment's last page, which the module holds anyway. It is the
cheaper of the two ways to keep the Shell's pages when its code grows (the other is `-Os`, which
costs instructions a lookup), and both segments are checked exactly by `tools/check_memory_ledger.py`.
Only tables and cold strings are so marked. In the Shell: `core/local_dict.c`,
`core/linebreak.c` and `core/textjoin.c`'s tables, `shell/history.c`'s header, offsets and strings
(which pay for PR #233's `characters` column: [ocr-history](ocr-history.md)) and the reading total's two
(`counting…`, `unavailable`), `core/history_lines.c`'s number format, `shell/worker.c`'s
out-of-memory and model-storage messages, and line history's strings in `shell/overlay.cpp` and
`shell/anki.c`. In the game worker: Luna's title-ID table (`core/hook_luna_table.inc`, API 33) and
Rubi's filters' ops arrays and their index (`core/hook_luna.c`, 776 B, PR #292, whose code took the
debug build's worker text 8 B past its pages without them; the numbers are in
`include/vjo_ledger_budget.h`), and Rubi's verified entries (`core/hook_place.c`'s `verified`, 28 rows,
1,120 B, PR #296: PRs #293-#296's 26 farm entries took the debug build's worker text 536 B past its pages
without it; the numbers are in `include/vjo_ledger_budget.h` too; 30 rows, 1,200 B, since the farm bucket's
two). Their strings stay in the code segment.

Standalone PR #236 (`6ed8f30`, on main `75041a0`), line history ([line-history.md](line-history.md),
owner 2026-10-20): Up on the overlay's top row shows
this game's earlier lines from `history.csv`. Its code in the normal Shell: the reader,
`vjo_history_scan` 384 B at `-Os` and `vjo_history_load` 244 B; the control thread's visit,
`history_show` 344 B and its other steps inlined into `ctl_main` +264 B (+312 B in the debug build); the
overlay's header, `vjo_styled_history` 54 B and `faded_row` 132 B, the position line in `render` +120 B,
the arrows' line step in `frame` +88 B (+56 B debug), the dimmed rows' recolour in `header_highlight`
+44 B, an earlier line's card in `do_save` +32 B; the rest under 25 B each. The normal Shell's code segment had 1,684 B of room on main `75041a0`
(1,764 B before #232's left stick): it pays with `core/textfilter.c` built `-Os` (`vjo_filter_lines`
190 → 168 B), its two strings in the data segment (`VJO_DATA_TABLE`, 80 B) and two of `shell/worker.c`'s
cold error messages moved there too (`OOM_SHELL_RESULTS`, 108 B, beside the `OOM_*` messages, and
`MODEL_STORAGE`, 82 B), so the code segment grows by 1,616 B (1,680 B debug) and leaves 68 B of room, and
the data segment 144 B. Data/bss grows by 304 B (the strings, `VjoView`'s three line-history fields, the
visit's pointer and step, the overlay's two words, the Shell ledger's eleventh item):

| | main `75041a0` | standalone PR #236 | room left |
|---|---:|---:|---:|
| normal Shell, text | 88,428 B | 90,044 B (+1,616) | 68 B of 90,112 |
| normal Shell, data/bss | 73,280 B | 73,584 B (+304) | 144 B of 73,728 |
| debug Shell, text | 98,812 B | 100,492 B (+1,680) | 1,908 B of 102,400 |
| debug Shell, data/bss | 73,272 B | 73,576 B (+304) | 152 B of 73,728 |

The pages stay 163,840 B (exact) and 176,128 B. `VJO_LEDGER_MAX_ITEMS` goes from 10 to 11 for the
Shell's new item, so every module's `VjoLedger` has 8 B more `.bss`: the kernel 77,636 → 77,644 and
75,948 → 75,956 B, the worker 15,720 → 15,728 B (both builds), the same pages [measured: the build log's
module rows, both builds of main `75041a0` and this change; per function, `arm-vita-eabi-nm -S`]. The
next change that adds more than 68 B of code (or 144 B of data) to that normal Shell must make room
first. A separate merge with PR #233's `characters` column at `6e8a6cb` also fit: normal
90,068 + 73,688 B, debug 100,500 + 73,680 B, the same pages [measured: both builds of that merge,
reported in PR #236]. Those standalone and two-PR results do not measure this combination with
native hooks, black-bar trimming and the popup cleanup; the combined ELF checks remain required.

The worker's budget has separate `#if VJO_DEBUG` branches, as the Shell's and kernel's do. A change
that adds a page only in one build must retain the other build's own measured page count; a shared
increase would fail the normal build's exact check. The combined worker measurements are pending,
so the retained native-hook budgets must be checked against both combined modules before packaging.

## The CI check

`tools/check_memory_ledger.py` runs in both CI jobs and as `ctest -R memory_ledger`:

1. **Allocation sites.** It strips comments and string literals from every file under `shell/`,
   `kernel/`, `game_ocr/`, `core/`, `vita/` and `include/`, and looks for `sceKernelAllocMemBlock`,
   `ksceKernelAllocMemBlock`, `sce_paf_memalign`/`malloc`, `sceKernelCreateThread`,
   `ksceKernelCreateThread`, libc `malloc`/`calloc`/`realloc`, C++ `new` and the other allocators
   listed in the script. Each (file, call) pair must be in `ALLOWED`, with at most the listed
   count; those are the wrappers. Anything else fails: "route it through a ledger wrapper". A
   wrapper that disappears fails too.
2. **Worst cases.** It evaluates the budget header and fails when a pool's worst case exceeds its
   limit, or a game item exceeds its tier limit.
3. **Packaged modules.** Given `--kernel`, `--shell` and `--worker`, it sums each module's loaded
   segments rounded to 4 KiB pages and fails when a module has more pages than its declared
   budget. Growing a module is therefore a deliberate act: raise the number in the header, cite
   where the bytes come from in the PR, and expect the question why they cannot live elsewhere.

`tools/tests/test_memory_ledger.py` proves each failure mode on a copy of the tree (an added raw
allocation in C and C++, a call in a comment or string that must not count, a removed wrapper, a
worst case over its limit, an item over its tier, a module one byte into the next page, a file
that is not a SELF).

## At run time

The Shell writes its ledger to `ux0:data/VitaJPOverlay/status.txt` at boot, at each game start and
at each game exit (`tools/install_ftp.sh --status VITA_IP` fetches it):

```
ledger boot pages: current=217088 peak=217088 limit=1048576
ledger boot paf: current=8192 peak=8192 limit=985088 free=2557952
ledger   Shell pages: current=176128 peak=176128
ledger   VjoNet stack: current=20480 peak=20480
ledger   VjoControl stack: current=20480 peak=20480
ledger   Paf plugin (est.): current=8192 peak=8192
```

`free=` is the PAF heap's free size as `GetFreeSize()` reports it (0 before Paf is ready). Stacks
include their 4 KiB of TLS. The kernel writes `ledger kernel: current=… peak=… limit=…` and its
items to `kernel.txt` once a boot, at its start (`kernel/main.c`; until API 24 also at every
full-frame capture, from the removed `VjoCapture` allocator). The game worker keeps its ledger in the
game's process, which has no file access; its numbers reach the Shell log through the job result
(`game vocr memory: free USER=… need …`, `workspace_bytes`, `resident_bytes`).

Module pages and the Paf plugin are counted from the declared budget (the CI check ties the
declaration to the binary); stacks, blocks and arenas are counted as they are made and freed.

## What is still an estimate

- The Paf plugin and RCO (8,192 B) and whatever the overlay's widgets take from the PAF heap
  while a page is open: unmeasured. The self-test (plan item 5) records `free=` before and after
  opening the overlay.
- The kernel pool's free space, hence its real limit.
- The PAF limit is one console's best sample; the self-test records the free heap at boot and at
  game launch on every run.
