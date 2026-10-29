# The memory ledger's history

Each change's measurements, newest first, as [the memory ledger](memory-ledger.md) recorded them (moved here
verbatim, #418): the modules' rows, the pools' figures and their sources; at the end, the budget header's
comments until #418 ("Budget history"). The ledger itself, its table and its checks, is
[memory-ledger.md](memory-ledger.md).

## App colour schemes in the overlay

The overlay and subtitle strip read `app.ini`'s colour scheme once per game process, using VjoControl's
existing 24 KiB scratch and restoring its mark. The one selection integer is appended to `VjoView`;
there are no new threads, buffers, PAF widgets, allocation sites or pool budgets. Paper recolours the
existing surfaces; text roles are remapped only for display. The game-facing region selector stays dark.

Repeated PAF widget lookup, position, size and colour construction now share compact helpers instead of
being inlined at every call. The initial footer is rendered by the first frame's normal render, removing
a duplicate format string. Two existing region hint strings and the small palette tables use the data
segment's existing last page. No dictionary or OCR build flags change.

Measured from packaged Shell SELF segments with the gate's pinned VitaSDK GCC 15.2.0, Release builds
(`tools/check_memory_ledger.py`), **not console measurements**:

| Shell | Text | Data/bss | Loaded pages | Room in last code / data pages |
|---|---:|---:|---:|---:|
| Normal | 85,956 B | 69,556 B | 155,648 B, unchanged | 60 / 76 B |
| Debug | 98,156 B | 69,632 B | 167,936 B, unchanged | 148 / 0 B |

Host tests cover parsing and storage errors, backup fallback, arena restoration, the per-process latch,
shared app/text roles and the exact surface recolouring loop with missing widgets. Console appearance
and timing remain unmeasured.

## Compact suffix table (PR #518 merge-gate repair)

The generated deinflection suffix pool stores its 1,494 BMP characters as `uint16_t` codepoints instead of
three-byte UTF-8: 4,483 -> 2,988 B, including the old string's trailing NUL. Rule offsets, lengths, order, labels
and flags are unchanged. A rule expands into the candidate buffer the lookup already uses; no allocation,
static workspace, thread, dictionary format or memory limit changes. The generated-table test decodes every
rule against the vendored inputs, and the ARM lane now also runs the synthetic cursor-lookup suite.

Measured from packaged Shell segments with the gate's pinned VitaSDK GCC 15.2.0, Release/LTO, against `main`
`2365f956`'s Shell sources (the settings-only PR head has the same Shell inputs):

| Build | Code segment before -> after | Data/bss | Loaded pages before -> after | Existing budget |
| --- | ---: | ---: | ---: | ---: |
| Debug | 99,420 -> 97,956 B | 69,184 B | 172,032 -> 167,936 B | 167,936 B |
| Normal | 87,204 -> 85,756 B | 69,108 B | 159,744 -> 155,648 B | 155,648 B |

Both builds fit their unchanged declared page budgets again. These are binary measurements, not console
measurements; the encoding change's console timing is unmeasured.

## Rear double-tap full-screen OCR (API 57)

The open overlay's rear double tap submits the existing `VJO_GAME_OCR_F_FULL` job through the game's
spotter, recognizer and configured LM decoder. The saved region is untouched. No block, thread or
allocation site is added: full-screen workspace and model checks use the existing game-worker ledger,
reserve and fallback. The Shell adds three control integers (the pending/pinned mode, its job snapshot,
and a one-shot UI flag); the kernel adds one rear-gesture mode integer. No pool or module budget is raised.

Measured from the packaged modules with VitaSDK GCC 15.2.0 in local Release builds
(`tools/check_memory_ledger.py`, normal with `--exact`), not measured on a console:

| Module | Debug: text + data/bss | Normal: text + data/bss | Loaded pages, debug / normal |
|---|---:|---:|---:|
| Shell | 97,996 + 69,184 B | 85,724 + 69,116 B | 167,936 / 155,648 B, unchanged |
| Kernel | 43,832 + 72,412 B | 38,640 + 70,724 B | 118,784 / 114,688 B, unchanged |
| Game worker | 180,672 + 32,424 B | 171,740 + 32,408 B | 217,088 / 204,800 B, unchanged |

The normal Shell has 292 B of code room and 516 B of data room in its last pages; the debug Shell
308 B and 448 B. Both builds pass the module and allocation ledger checks. Console CPU and latency
remain unmeasured.

## psandvita #1's six verified entries (no API change)

Six more verified entries ([native-hooks.md](native-hooks.md#table-sites)), four filters
([Rubi's filters](native-hooks.md#rubis-filters)) and a second layout of the cell record's read (`VJO_PLACE_READ_CELLS_B`,
`vjo_place_cells_layout`: `site_hit` reads the count and cells at either layout's offsets). The six rows (240 B) and the
four filters' ops (156 B: `PCSG00769`'s ends in a call of `FOAF4`, `PCSG00380`'s array from its third op, no array of
its own) are in the worker's data segment (`VJO_DATA_TABLE`), which has had room since #418 freed a page; the six title
strings, the filters' index and the new patterns are read-only data. Measured with VitaSDK GCC 15.2.0 in
`tools/fast_gate.sh`'s image (`tools/check_memory_ledger.py`'s module rows, `arm-vita-eabi-size -A` on both builds'
`VitaJPOverlay_OCR`), main `140fb9c3` against this change (the rows are the same on `dc494f44` and `a06e6d44`):

| Game worker | Debug: main → this change | Normal: main → this change |
|---|---:|---:|
| `.text` | 144,640 → 144,648 B | 140,440 → 140,448 B |
| `.rodata` | 33,704 → 33,816 B | 29,144 → 29,256 B |
| `.data` | 5,272 → 5,668 B | 5,264 → 5,660 B |
| module rows, text + data/bss | 180,512 + 31,968 → 180,640 + 32,368 B | 171,596 + 31,952 → 171,708 + 32,352 B |
| loaded pages | 217,088 B, unchanged | 204,800 B, unchanged |

- Room left in the last pages: the normal worker 324 B of code and 416 B of data, the debug worker 3,680 B and 400 B.
  With the four filters' ops in read-only data instead, the normal worker's code page would have 180 B left.
- The kernel's and the Shell's rows are unchanged. No block, thread or allocation; no budget raised. None of it is
  measured on a console.
- Its audit's follow-up (audit-psandvita-1 F1: `RF_PCSG01282` drops five more of the engine's codes, two patterns and
  three literals): `.data` 5,668 → 5,728 B (debug) and 5,660 → 5,720 B (normal), `.rodata` 33,816 → 33,848 B and
  29,256 → 29,288 B, `.text` unchanged; module rows debug 180,640 + 32,368 → 180,672 + 32,424 B, normal 171,708 + 32,352
  → 171,740 + 32,408 B, the same pages (room left: normal 292 B of code, 360 B of data; debug 3,648 B and 344 B) [the
  same measurements, main `3f864a3a` and the follow-up].

## Cleanup P1: dead Shell state (#418; no API change)

State the Shell wrote and nothing read. **No block, thread, stack or allocation; no module takes a page more.**

- **The Triangle view's line source** (`VjoLineDebug`, `shell/shell.h`): the view's snapshot keeps `origin`, the one
  field the overlay shows ("This line came in through: …", `shell/overlay.cpp`); its ten other fields (the hook's
  stream, flags, score, streams, mode, installed and refused counts, the scene, the capture and the model) were written
  at each accepted text and read only by host tests since #291 replaced the detailed view (44 B: "Hidden Triangle line
  diagnostics", the section of this file that added them): 4 B in `g_view` and in the line-history visit's copy
  (`HistoryVisit`, 236 → 196 B, inside its unchanged 16,384 B block), and `line_debug_accept` one store. The visit's
  earlier lines get the 40 B (`history_open` gives them the rest of the block: 16,148 → 16,188 B), so a visit whose
  lines filled them can keep one more earlier line [measured: `sizeof` with `arm-vita-eabi-gcc`, and `vjo_history_scan`
  over one history.csv window at both sizes on the host, audit-rubi-cleanup-1: 4 earlier lines → 5 in its boundary
  case].
- **`VjoErr.dict`** (`core/platform.h`): set to `VJO_DICT_LOCAL`, the only dictionary, in six places and read in none
  (the per-dictionary error texts went with jpdb.io, jiten.moe and the Hachidori relay): 4 B less in each `VjoErr`, two
  of them in the Shell's `.bss` (`cache_data`) and two in the Rubi app's (`settings/lookup.c`'s `data` and `none`).
- **The text source's names** (`hook_note`'s hooks line in `shell/worker.c`): `config.c`'s table, which
  `text_source` is parsed with (now `vjo_text_source_names`, in the data segment as before), instead of a second
  copy of it (12 B of the code segment's rodata).

Measured with VitaSDK GCC 15.2.0 in the gate's pinned image (`tools/check_memory_ledger.py`'s module rows; the app's
ELF with `arm-vita-eabi-size -A`), main `41678d6` against this change:

| Module | Debug: main → this change | Normal: main → this change | Loaded pages |
|---|---:|---:|---:|
| Shell | 98,076 + 69,264 → 97,884 + 69,216 B | 85,732 + 69,200 → 85,636 + 69,152 B | 167,936 / 155,648 B, unchanged |
| Kernel, game worker | the SELF files byte-identical | the SELF files byte-identical | unchanged |
| Rubi app (`.text`, `.bss`) | 481,456 → 481,456 B, 5,004,872 → 5,004,856 B | 481,008 → 481,072 B, 5,004,856 → 5,004,840 B | its own process |

The Shell's code is 192 B (debug) and 96 B (normal) smaller, its data/bss 48 B: room left in the last pages, the normal
Shell's code 380 B (284 before) and data 480 B (432), the debug Shell's 420 B (228) and 416 B (368). In the app (its own
process, not SceShell's; `config.c` built `-Os`, as `vita/CMakeLists.txt` builds it in every module, without LTO):
`parse` +42 B, `entry_at` +6 B, `local_lookup_begin` -4 B, `vjo_local_read_page` -4 B, `lookup_pages` -12 B: +28 B of
its functions [`arm-vita-eabi-nm -S`], +64 B (normal) and 0 B (debug) of its `.text` with their alignment; its `.data`
and `.rodata` the same. These are binary and host measurements; nothing of it has run on a console.

## Cleanup P1: the subtitle strip's text without its arena (#418; no API change)

The subtitle strip (Select + R, `shell/strip.cpp`) turned its sentence into UTF-16 in a static 8,480 B styling arena
(`s_mem_buf`: `vjo_styled_init`, `vjo_styled_puts`) and copied the units from there into its own buffer, `s_strip_u`;
the arena's one span was never read (`strip_apply` sets the strip's one size and color over the whole text). The units
now go straight into `s_strip_u` (`core/styled.c` `vjo_styled_units`: `vjo_styled_puts` itself over that buffer, without
a span): at most `VJO_STRIP_MAX` units, whole characters, and their NUL, which leaves the `VJO_WRAP_BREAKS` units after
it for the breaks, as before. **No block, thread, stack or allocation; two data pages fewer in both builds, and
`VJO_BUDGET_SHELL_MODULE_PAGES` lowered to them** (AGENTS.md rule 1: the Shell's pages only shrink). Measured with
VitaSDK GCC 15.2.0 in the gate's pinned image (`tools/check_memory_ledger.py`'s module rows), main `8b18f99` against
this change:

| Module | Debug: main → this change | Normal: main → this change | Loaded pages (debug / normal) |
|---|---:|---:|---:|
| Shell | 98,108 + 77,760 → 98,076 + 69,264 B | 85,764 + 77,696 → 85,732 + 69,200 B | 176,128 → **167,936** B / 163,840 → **155,648** B |
| Kernel, game worker | the SELF files byte-identical | the SELF files byte-identical | unchanged |

- **The Shell** (SceShell's pages): 8,496 B less `.bss` in both builds (`s_mem_buf` and its 16 B `VjoArena`), 19 → 17
  data pages; 32 B (debug) and 32 B (normal) less code. `vjo_styled_init` is kept out of line (`noinline`, as
  `vjo_games_row_as` is): once the strip no longer called it, LTO put a copy of it in each of the overlay's two calls,
  which was 48 B (debug) and 96 B (normal) more code than this form [measured on main `b57fb7e`: the builds without the
  attribute, 98,124 and 85,828 B of text]. Room left in the last pages: the normal Shell's code 284 B (252 before) and
  data 432 B (128), the debug Shell's 228 B (196) and 368 B (64).
- **The paf thread's stack:** the text is converted where it is kept, never on the stack: `vjo_strip_frame` (with
  `strip_render` in it) 128 B before and 128 B after; the conversion's chain from it to `vjo_styled_add` 192 → 240 B
  (212 → 260 B with `vjo_utf8_next`'s 20 B leaf), the 48 B frame of `vjo_styled_units` [`-fstack-usage`, the normal
  Shell's flags without LTO]. In the packaged Shell, LTO puts `vjo_styled_units` and the strip into the overlay's
  `frame`, whose frame is 288 B (normal) and 296 B (debug) before and after, and `vjo_styled_puts` and `vjo_styled_add`
  are called from it as before: no packaged frame grows [measured: the prologues of `frame` in both builds' Shells,
  `arm-vita-eabi-objdump -d`, main `8b18f99` and this change].
- **The same units:** `wrap_fix`'s `strip_units_are_the_styled_text` compares `vjo_styled_units` with the arena's
  `vjo_styled_puts` (every unit, the count, the NUL, the cut at a whole character, nothing written after the NUL) on
  edge cases (a surrogate pair at the cap, CR, cut and overlong sequences, the longest strip texts) and 4,000 random
  UTF-8 texts at the strip's cap and at small caps; `word_lookup`'s `strip_units_of_every_bundled_text` on every text
  of the bundled dictionary (486,529 entries' spellings, readings and texts) and the 50 real VOCR lines: 1,459,637
  texts, 46 of them longer than the strip's 4,096 units, all identical. Both fail when the cap is one unit more, or
  when one unit after the NUL is written (tried).
- **The model** (`tools/shell_budget_model.py`, the debug build's budgets): SceShell's peak pages from us 217,088 →
  208,896 B; at the worst step of the 2026-10-11 v0.7.4 sample, 344,064 → 352,256 B of headroom (est.: the model ranks
  builds, docs/offline-testing.md).
- These are binary and host measurements; nothing of it has run on a console.

## The worker's data page: the gate's weights and the line cache's greedy texts (#418, no API change)

Cleanup package P4 of [#418](https://github.com/bee-san/Rubi_for_vita/issues/418) gives every game that is ON one page
of the game worker back, in both builds: its data/bss had 440 B (debug) and 456 B (normal) of its last page left, and
two static buffers held bytes most games never needed.

- **The text-presence gate's weights** (`ocr_text_gate`, off by default) were 1,560 B of `.bss`
  (`VjoVocrGateResident`, the file's 1,540 B) in every game, the gate on or off. They are a block of their own now,
  **`VjoGameVocrGate`**: 4,096 B of `USER_RW` (`VJO_BUDGET_GAME_GATE`, the pinned file page-rounded), taken through
  `game_memblock_alloc` at the first job that asks for the gate, kept between such jobs like the LM, given back by a
  job without the gate, with the other resident blocks (a job that does not fit, eight trusted hook answers, a game no
  longer the active one, the worker's stop) and when its file goes. Since #418's audit (F1, F3) the job decides the
  block from its own memory check, by the check's rule and its sample of the game's free memory, right after the check
  and before API 55's frame block and the helpers, and counts it in its need there, as 0.10.0's static page was
  counted (taken out of the free memory): every later decision (the frame block, the helpers, another model's weights
  kept beside the job) is then 0.10.0's. The core takes the block without a second look at the game's free memory, as
  the job's workspace and capture blocks are taken: the console reports that figure in whole MiB (#324), so a second
  look after the job's own blocks could see a whole MiB less than the check did and refuse the gate in a game short
  of memory, where 0.10.0 gated (#471 took that second look). Without the room the job reads as without the gate (its
  step's rc `VJO_E_OOM`), never with a smaller model: the model check itself does not count the block, so a job
  within 4 KiB of that check's limit runs the larger model without the gate where 0.10.0 ran a smaller one with it
  (the one outcome that differs; with the block held, nothing differs: its bytes are out of the free memory as the
  `.bss` page's were). A try of columns and a job whose spotter finds its lines read no gate and count no room for it.
  Its step-log item is its own (`VJO_GITEM_GATE`, `gate` in the debug log: the Shell's item table, the same rows and
  pages); its ledger item is the spotter's (`GL_GATE` = `GL_SPOT`, the finder's weights), as a probe's strip shares
  the capture's and API 55's frame block the tall line's. That is a choice: a 13th item (`VJO_LEDGER_MAX_ITEMS`
  12 → 13) adds 8 B of `.bss` to every module's `VjoLedger`, SceShell's included, for an item the worker never logs
  (its ledger is read by host tests only). On main `b55c5f5` it pushed the debug Shell's code 12 B past its last page
  (98,276 → 98,316 B of text, 98,284 B with 12 items); after #418's Shell cleanups it fits (+16 B of debug Shell code,
  every module's pages unchanged [measured: audit-rubi-cleanup-4 F6, main `b57fb7ee`]). The gate's block is counted
  all the same: the item's current and peak bytes, the pool's, and `VJO_BUDGET_GAME_GATE` in the game job's worst case.
- **The line cache's greedy texts**: each of its eight entries (`VjoVocrCacheLine`) held a second 368 B buffer for
  the line's recognizer (greedy) text, a copy of the shown text on every line that neither the LM nor the cascade
  changed. An entry keeps it after the shown text's NUL now, only where it is another text (`greedy_at`): 2,944 B of
  `.bss` less (the cache 6,160 → 3,216 B). The cache's rule: a shown text of 368 B or more is not kept, as before; a
  line whose greedy text is another is kept only when the two fit one entry's 368 B together (before: each its own),
  and a line left out is counted (`VjoVocrStats.cache_long`, the `recognize` step's `sum` bits 8-15 in the debug log).
  The same hits and misses on every host test's sequence: `host/tests/cache_replay.h` replays every lookup and entry
  of `test_vocr_parallel.c`'s, `test_game_vocr.c`'s and `test_vocr_vertical.c`'s jobs through the old layout and finds
  the same cache after each (and main gives the same counts on the two dedicated replay sequences: measured on
  `b55c5f5` and `e17f42e`).
- Before them came the package's first change, one allocator, one file loader and one poll loop
  ([above](#one-allocator-one-loader-and-one-poll-loop-in-the-worker-418-no-api-change)); the gate's block and the
  cache's layout take 96 B (debug) and 80 B (normal) of text less again (the gate's loader is the shared one).

The game job's worst case grows by the gate's block, 11,022,336 → 11,026,432 B (with an SC3 charset and API 55's frame
block), under its unchanged limit, 11,214,848 B (`test_ledger.c`); its permanent part loses the page (253,952 B debug,
241,664 B normal): with the gate on the block takes the page the `.bss` gave back, with it off (the default) the game
keeps it. Measured with VitaSDK GCC 15.2.0 in the gate's pinned image (`tools/check_memory_ledger.py`'s module rows),
main `a4ab7f49` (#455's) against this change:

| Module | Debug: main → this change | Normal: main → this change | Loaded pages, debug / normal |
|---|---:|---:|---:|
| Game worker | 180,544 + 36,424 → 180,448 + 31,968 B | 171,596 + 36,408 → 171,516 + 31,952 B | 221,184 → **217,088** / 208,896 → **204,800** B |
| Shell | 98,108 + 77,760 B, the same rows (its SELF differs: the step log's `gate` item) | 85,764 + 77,696 B, unchanged (byte-identical SELF) | 176,128 / 163,840 B, unchanged |
| Kernel | unchanged (byte-identical SELF) | unchanged (byte-identical SELF) | 118,784 / 114,688 B, unchanged |

`VJO_BUDGET_WORKER_MODULE_PAGES` is lowered to the packaged sizes, 217,088 B (debug) and 204,800 B (normal); the
worker's data/bss keeps 800 B (debug) and 816 B (normal) of its last page, its code 3,872 B and 516 B. The audit's
follow-up (F1: the gate's room from the job's own check) adds 64 B (debug) and 80 B (normal) of worker code, the same
pages: 3,808 B and 436 B left [measured: the module rows, main `579aa61a` against it]. None of it is measured on a
console.

## One allocator, one loader and one poll loop in the worker (#418, no API change)

Cleanup package P4's first change ([#418](https://github.com/bee-san/Rubi_for_vita/issues/418)): the game worker's
four `*_alloc` / `*_release` / `drop_*` sets (weights, LM, cascade, spotter) and the SC3 charset's release are one set
over a block descriptor (`ResidentBlock`: its constants beside the code, its uid and bytes held in the data segment as
before), `core/vocr_ocr.c`'s resident-file loaders one `resident_file()` over one resident type, and the five BUSY poll
loops of the kernel broker's calls one `broker_poll()`. Every step, ledger item, block name and order is as before; no
block, thread or allocation.

| Game worker | Debug: main → this change | Normal: main → this change |
|---|---:|---:|
| module rows, text + data/bss | 181,936 + 36,424 → 180,544 + 36,424 B | 172,972 + 36,416 → 171,596 + 36,408 B |
| loaded pages | 221,184 B, unchanged | 212,992 → **208,896** B |

The normal worker's code is under its last page again (API 55's pause, #355, had taken it a page past): one page of
the game's memory less in every game that is ON, in the normal build; `VJO_BUDGET_WORKER_MODULE_PAGES` is lowered to
it. Code room left: 436 B (normal) and 3,776 B (debug). Measured with VitaSDK GCC 15.2.0 in the gate's pinned image
(`tools/check_memory_ledger.py`'s module rows), main `93c99c7e` against this change; the Shell's and the kernel's rows
and SELF files are main's. None of it is measured on a console.

## #418's cleanup P3: the kernel's rows in place, one signature grid (no API change)

The display hook converts each row of the game's frame in one scratch row instead of two, the OCR job's first pass
samples the region signature's grid instead of a grid of its own, and the kernel worker's copy of the hook's signature
is on its stack. **No block, thread or allocation; the kernel takes one data page fewer in both builds**
(`VJO_BUDGET_KERNEL_MODULE_PAGES` lowered to the packaged pages, both `#if VJO_DEBUG` branches); the Shell's and the
game worker's SELFs are byte for byte main's.

- **The rows** (`kernel/capture.c`, `core/fbgrey.h`): a 16- or 10-bit row is copied from the game into the one
  3,840 B scratch row (`VJO_MAX_W` words) and converted there (`vjo_fb_to_abgr_inplace`: a 16-bit row from its last
  pixel down, so that each word it writes lies over pixels it has read), and a row's grey bytes are written over the
  words or pixels they come from (`vjo_abgr_to_grey`, `vjo_a1b5g5r5_to_grey`: byte i after pixel i). One copy from
  the game per row, as before. 3,840 B less `.bss` (`scratch_tmp32`).
- **One grid** (`kernel/scene.h` `SceneRows`): the job's first pass, fed over several frames, keeps its own size,
  next row and grid row (20 B) and samples the region signature's `SceneAcc` grid. The grid is a function of the
  region's size only; it is rebuilt for the size that needs it, in the row that needs it, when the two differ (a
  full-frame job beside a text box; a region changed during a job, which `vjoSetRegion` does not stop), and only in
  `on_frame`: the job's begin, in its syscall, writes no grid, which the display hook could be reading. 1,044 B less
  `.bss` (`capture_acc`, 1,064 B, for `capture_rows`, 20 B). **Time:** a region job's first pass has the region's
  size (`vjoOcrSubmit` crops the region), so the grid is never rebuilt for it; a pass of another size (the debug
  self-test's `fullscreen` and `probe` stages, a region changed during a job) rebuilds it in the display hook at most
  twice for each region signature taken during the pass (one every 8th frame): 10,360 instructions for 960 × 544,
  12,066 for 896 × 156 (`qemu-arm -one-insn-per-tb` on the Vita build's `scene.c`; about 25-40 µs at 444-333 MHz,
  **est.**). Main rebuilt each grid only when its own size changed, the job's in its syscall.
- **The signature's copy** (`kernel/main.c` `change_detection`): 524 B on VjoWorker's 16 KiB stack instead of `.bss`
  (`sig.0`); `change_detection` is not inlined into the thread's loop (`noinline` since its audit's F4: GCC 15.2 did
  not inline it anyway, the same SELF), so the frame is live only in its call. The thread's deepest path of the
  plugin's own frames: 600 → 1,000 B (normal) and 608 → 1,008 B (debug), the firmware's calls counted as 0 B as before
  (`-fstack-usage -fcallgraph-info=su`, the gate's vitasdk image).
- **Other stacks** (the same measure; its audit's F4, F5): the display hook's deepest path 284 → 368 B and the static
  framebuffer's pad-call path (`display_static_fb_sample`) 300 → 384 B in both builds, the job's grid now rebuilt there
  (`scene_rows_add` 32 + `grid_for` 80 B) on the game thread's kernel stack, whose own size and the firmware's frames on
  it are unmeasured (+84 B: harmless, **est.**). #430's six model syscalls, their step now out of line under
  `model_syscall`'s 72 B frame (the indirect call added by hand): Open 120 → 168 B, Read 80 → 136, Close 64 → 120,
  HookCache 88 → 136, ShotWrite 80 → 136, ShotSave 80 → 128 B, both builds (a syscall's stack size is not cited here:
  harmless, **est.**).
- **Measured** with VitaSDK GCC 15.2.0 in the gate's pinned image (`tools/check_memory_ledger.py`'s module rows),
  main `85e9da1` (v0.10.0, #355's `ocr_pause` and #430) against this change:

  | Module | Debug: main → this change | Normal: main → this change | Loaded pages |
  |---|---:|---:|---:|
  | Kernel | 43,704 + 77,812 → 43,752 + 72,404 B | 38,496 + 76,124 → 38,560 + 70,716 B | 122,880 → 118,784 / 118,784 → 114,688 B |
  | Shell | 98,108 + 77,760 B, the same SELF | 85,764 + 77,696 B, the same SELF | 176,128 / 163,840 B, unchanged |
  | Game worker | 181,936 + 36,424 B, the same SELF | 172,972 + 36,416 B, the same SELF | 221,184 / 212,992 B, unchanged |

  Room left in the kernel's last pages: 1,304 B of code and 1,324 B of data (12 B before) in the debug build,
  2,400 B and 3,012 B (1,700 B) in the normal one. The kernel pool's worst case and limit (`VJO_WORST_KERNEL`):
  245,760 → 241,664 B (debug) and 241,664 → 237,568 B (normal); its permanent part 155,648 → 151,552 B and
  151,552 → 147,456 B (`tools/check_memory_ledger.py`'s pool table). The text grows 48 B (debug) and 64 B (normal):
  the second signature's begin and row, the grid's rebuild check, `change_detection` out of line. (On `b55c5f5`, before
  #355 and #430: 43,508 + 77,780 → 43,572 + 72,372 B and 38,356 + 76,092 → 38,436 + 70,684 B.)
- **Proof** (host): `capture_parity`, the recognizer's input against API 18's capture, on 162 regions, two of them
  odd-width crops (77 px) over a line of text, one from the frame's first row and one to its last, each asserted to give
  lines (since the audit's F2: as merged, those two lay in text-free corners and compared no line), and its first pass
  with the capture's signature, as the worker asks for it (VJO_ROWS_SIG), so A1B5G5R5 frames take the in-place
  expansion of the rows the signature samples (F3; before, `sig = 0` read every 16-bit row in one pass): 324 lines,
  4,317,784 B of recognizer input, 0 differences (288 lines and 4,252,948 B before); `kernel_capture`, every format's
  grey and colour rows from random pixels at odd widths, the frame's first and last rows and columns and a whole
  960-pixel row, and the region's and a job's signatures over the one grid at any two sizes (widths, heights or both
  differing), each against its own accumulator's, and the rule the grid rests on: a job's begin writes no byte of the
  region signature's `SceneAcc` (F1; `kernel_game_ocr` checks it through `vjoOcrCaptureRows`, at the rows call's wait for
  the frame); `fbgrey`, the conversions in place at every length to 961 pixels, 4,095 pixels and every 16-bit value.
  The stage bench's capture and scene stages (`stage_bench_smoke`) read A1B5G5R5 rows without a signature only, so
  they do not see the 16-bit expansion (F3); `fbgrey`, `kernel_capture`, `capture_parity` and `game_shot` do. Mutations
  (the audit's): a forward 16-bit expansion fails `fbgrey`, `kernel_capture`, `capture_parity` and `game_shot`; a begin
  that writes the grid, `kernel_capture` and `kernel_game_ocr`; a grid keyed on the width only, `core` and
  `kernel_capture`. **Not run on a console.**

## The pause for the frame (API 55)

`ocr_pause` (#355, off by default; [game-pause.md](game-pause.md)). Measured with `tools/check_memory_ledger.py`'s
module rows, both builds, main `f32c5e3` (v0.10.0 and #503's streak fix: the kernel's and worker's rows are
v0.10.0's) and this change, tools/fast_gate.sh's vitasdk image (text + data/bss):

| module | debug | normal | pages |
|---|---|---|---|
| Kernel | 43,496 + 77,780 → 44,152 + 77,812 B | 38,344 + 76,092 → 38,928 + 76,124 B | 122,880 / 118,784 B, unchanged |
| Shell | 98,132 + 77,760 → 98,108 + 77,760 B | 85,828 + 77,672 → 85,764 + 77,696 B | 176,128 / 163,840 B, unchanged |
| Game worker | 180,176 + 36,840 → 181,936 + 36,424 B | 171,260 + 36,832 → 172,972 + 36,416 B | 217,088 → **221,184** / 208,896 → **212,992** B |

- **The game worker takes one page more in each game that is ON** (`VJO_BUDGET_WORKER_MODULE_PAGES` raised by
  4,096 B in both builds, deliberately): the pause's code (`game_ocr/main.c` pause_copy, the frame block's check,
  ladder, release and step; `strip_source`'s frozen reads; `core/vocr_ocr.c`'s frozen region) is 1,280 B (debug) and
  1,232 B (normal), against 48 B and 772 B left in the last code page. Its 64 B of statics needed the data segment's
  last page, which had 24 and 32 B: the pinned model table (`core/vocr_ocr.c` `models`, 480 B, in the data segment
  since #379, `VJO_DATA_TABLE`) is read-only data again, in the new code page. That leaves 2,384 / 3,156 B of code room
  and 440 / 448 B of data room: room at last for the next verified hook rows (40 B each), which audit-rubi-merge-394
  N5 found had none. The page is 0.005 % of the 90,177,536 B free USER the least any job saw in CLANNAD [measured,
  #81's v0.8.0-test.1 log], whether or not the setting is on.
- **The Shell keeps its pages, with more code room than main** (SceShell's pages may only shrink: AGENTS.md rule 1).
  Its part is the key's parse, one speed bit (folded into the user jobs' vertical-text line), `VjoConfig`'s 4 B in
  each of its three copies, `game_static_fb` and the wait hint's clause (`WAIT_HINT_PAUSE`, a 54 B format of the one
  hint, [below](#why-the-overlay-waits-358)'s design, written into a 256 B buffer in `start_job_kind`'s frame: 24 →
  280 B, `arm-vita-eabi-gcc -fstack-usage` at -Os, both builds, on VjoControl's 16 KiB stack). `core/config.c` parses
  its twelve plain off/on settings from one table in place of a branch each, and #358's and API 54's keys
  (`ocr_wait_hint`, `ocr_spotter_refine`) and #413's debug-only `open: …` format are read-only data again: 144 B of the
  normal Shell's code and 32 B of its data, 152 B and 40 B of the debug one's [measured: this change with and without
  the table, both builds, on `efbee95`]; the config golden files are unchanged by it. Room left: 252 B of code and
  128 B of data in the normal Shell (188 and 152 B on main), 196 B and 64 B in the debug one (172 and 64 B).
  - **What the table is for.** On `efbee95` (after #428 and #440 freed Shell code) the pause fits without it too
    (85,876 + 77,728 and 98,244 + 77,800 B); on `eef0fc4` it did not (86,084 + 77,784 and 98,428 + 77,856 B, a page
    more in both builds). The draft's own Shell part, with the hint as a second copy (214 B) and its branch in the
    parse, does not fit `efbee95` either: 85,860 + 77,888 and 98,212 + 77,992 B, a data page more in both builds
    [measured: the same rows].
- **The kernel keeps its pages**: the frame copy's checks, the hook's chunked copy and cap, the watchdog's count.
- **Memory at run time**: the frame block (`VjoGameVocrFrame`, at most 524,288 B, [the ledger](memory-ledger.md#pool-game-scephymempartgame-256-mib-minus-the-games-own-use))
  is counted as the tall line's ledger item (`GL_LINE`), which it replaces: a job holds one of the two, never both, and
  the worst case counts the larger (`VJO_BUDGET_GAME_LINE_OR_FRAME`, +8,192 B: 11,022,336 B with an SC3 charset,
  under the 11,214,848 B limit). Every module's ledger keeps its twelve items (`VJO_LEDGER_MAX_ITEMS`). No thread, no
  kernel buffer.

## The reading streak in the overlay (no API change)

The owner (2026-10-28) removed the Rubi app's Stats tab and its Cards page and moved the streak into the overlay's
Triangle view ([ocr-history.md](ocr-history.md#streak)). The Shell gains the streak's record (`shell/history.c`
`vjo_history_streak`: a 16-byte file read when a line is recorded and when the view asks, written on a day's first
line) and the view's line; nothing is allocated, no thread or PAF block is added, and the Shell's imports are the
ones it had (`sceRtc*`, `sceIo*`). Measured with `tools/check_memory_ledger.py`'s module rows, both builds, main
`52eb948` (#428 and #440 in it; `ec27187f`, #501's base, has the same Shell) against #501:

| Shell | main `52eb948` | #501 |
|---|---:|---:|
| normal | 85,316 + 77,664 B | 85,796 + 77,672 B (+480 code, +8 data) |
| debug | 97,604 + 77,752 B | 98,116 + 77,760 B (+512 code, +8 data) |

Both in the pages they had (163,840 and 176,128 B, no budget raised): 220 and 188 B of code room left, 152 and 64 B
of data room. The kernel's and the game worker's SELFs are byte-identical to main's in both builds. By symbol in the
normal build (`arm-vita-eabi-nm -S`): `vjo_history_streak` 328 B, `render` +64 B (the line), `net_main` +48 B (the
call after an append: the writer is inlined there), `vjo_history_total_step` +12 B (the call on a view's request),
the line's format 44 B of rodata, and LTO's other moves −16 B; `total` 264 → 272 B of `.bss` (the streak shown, in
what was padding, and the writer's update count). Of the 480 B, the update when a line is recorded is 144 B (120 B in
the debug build) and the view's line, with the record's read and the clock, 336 B (384 B) [measured: a build without
the update, `arm-vita-eabi-size`]. The update stays in the Shell: the game worker cannot write `ux0:data` itself (its
files go through the kernel's syscalls) and does not know which lines `history.csv` records (the Shell's writer
decides, `vjo_history_append`). The Rubi app's seed is the app's own memory ([vita-memory.md](vita-memory.md#54-the-rubi-app-a-system-application)).

#501's audit's follow-up (F3, F4: no streak line before the record's first read, and no redraw when the streak shown
did not change) makes the Shell 85,796 → 85,828 B of code (normal, +32) and 98,116 → 98,132 B (debug, +16), data
unchanged, the same pages (188 and 172 B of code room left) [measured: the module rows, #501 `92c19fb7` against the
follow-up]. Its other fixes (F1, F2: the seed's merge and window loop) are the Rubi app's: its text 651,172 →
651,364 B (normal) [measured: `arm-vita-eabi-size`].

## A press and the stable-text wait: #413's audit (#441) and #441's audit (no API change)

Two conditions in the Shell's `shell/worker.c`, no block, thread, buffer or string: #441's (a press's retries wait
once a read tore under every try, `start_job_kind`) and its audit's (a press keeps a running background read while
that count is held over, `open_overlay`). Measured with `tools/check_memory_ledger.py`'s module rows, both builds:

| Shell code | main `5fde142` (before #441) | #441 (`e17f42e`) | #441's audit |
|---|---:|---:|---:|
| normal | 85,996 B | 86,012 B (+16) | 86,012 B (+0) |
| debug | 98,284 B | 98,284 B (+0) | 98,300 B (+16) |

Both in 86,016 and 98,304 B of code pages, data unchanged (77,728 and 77,816 B): **4 B of code room left in each
build**, 96 and 8 B of data room. The next Shell change has to move a string to the data segment (core/port.h
`VJO_DATA_TABLE`; the debug Shell's 8 B take none) or save code elsewhere; no budget is raised. The kernel's and the
game worker's rows are main's.
## #413's audit: a line typing out, and a press's partial line (no API change)

**F2** ([native-hooks.md](native-hooks.md#a-line-typing-out-413s-audit-f2)) is `core/hook_select.c`'s `prefix_score`,
one condition and its argument (the request's settle time, `game_ocr/main.c`) and a word of `VjoHookSelect` (`typing`,
in the worker's job state: never persisted, the cache's record unchanged), in the game worker only (`core/hook_select.c`
built `-Os`): its code 170,796 → 170,972 B in the normal worker and 179,760 → 179,920 B in the debug one (+176 and
+160 B), the same pages (1,060 and 304 B of code room left), data unchanged (the state is in `.bss`'s existing page).
The Shell's and the kernel's rows are main's [measured: `tools/check_memory_ledger.py`'s module rows, both builds, main
`b6507605` against this change].

**Its stack, #491's audit F3.** `prefix_score`'s scratch is the similarity's own (a code point array, then `within`'s
row of the edit distance), and it is called beside `score_of`, never inside it, so #491 said here that the stack's peak
was unchanged. As built it was not: GCC inlined `prefix_score` into `vjo_hook_select_after`, whose frame then held its
1 KiB array beside `score_of`'s call, so the comparison's deepest chain grew by 988 B, its arrays from 2,562 to 3,586 B.
`prefix_score` is now `noinline` (as `core/games.c`'s `vjo_games_row_as` is), and the chain is 36 B under #491's
parent's, since `within` stays out of line too:

| `core/hook_select.c` | `vjo_hook_select_after` | `score_of` | `prefix_score` | `within` | deepest chain |
|---|---:|---:|---:|---:|---:|
| #491's parent `9398b006` | 1,112 B | 1,600 B (`within` inlined) | | | 2,772 B (+ `cmp_form` 40 + `utf8_next` 20) |
| #491 (main `36e570c4`) | 2,136 B (`prefix_score` inlined) | 1,056 B | | 568 B | 3,760 B |
| `noinline` | 1,112 B | 1,056 B | 1,056 B | 568 B | 2,736 B (through either) |

The same in both builds [measured: `-fstack-usage -fcallgraph-info=su`, the worker's flags (`-Os`), arm-vita-eabi-gcc
15.2.0 of the gate's vitasdk image]. Its one caller is `vjo_game_ocr_run_request` (4,528 B), under `worker_main` (72 B),
on `VjoGameVocr`'s 32 KiB stack (`VJO_STACK_GAME`): that path takes 7,336 B (8,360 B on main, 7,372 B at the parent);
the worker's deepest path overall is not measured here. The attribute takes 32 B of code in each build, in the same
pages: 170,972 → 171,004 B in the normal worker (1,028 B of code room left) and 179,920 → 179,952 B in the debug one
(272 B), data unchanged; the Shell's and the kernel's rows are main's [measured: the module rows, both builds, main
`36e570c4` against this change]. As every change to `core/hook_select.c` does, it renews the hook definitions' revision
(`cmake/HookRevision.cmake`), as #491 did: one renewal for the release that ships both.

**F5** (a press's partial line and the settled one are two `history.csv` records:
[ocr-history.md](ocr-history.md)) is documented, not changed. Its fix, a record of a read without the wait that a
later extension replaces within 10 s instead of 750 ms (a field in `VjoHistoryRecord`, set in `net_job` from the job's
snapshot, and the window's choice in `vjo_history_append`), measured as a prototype on main `b6507605`: **+32 B of code
and +8 B of data in each Shell build**. The normal Shell would take 167,936 B of pages against its 163,840 B budget
(86,044 B of code, 4 B of room before it), the debug Shell 180,224 B against 176,128 B (98,332 B of code, 4 B of room
before it since #441's audit; its data then 77,824 B, full). So it needs 28 B of each Shell's code moved to its data
segment, which the normal build has room for (88 B after it) and the debug build has not (none after it): not done
here, and no budget raised.

## The hook farm's 20 verified entries (no API change)

Twenty more verified entries ([native-hooks.md](native-hooks.md#table-sites)) and their filters
([Rubi's filters](native-hooks.md#rubis-filters)) are data: 20 rows of 40 B in the worker's data segment, the new
filters' ops and the filters' index, and the title strings and new patterns in its read-only data. `PROTOTYPE_NAMELINE`
and `PCSG00472` are `PROTOTYPE`'s and `IFENGINE`'s arrays from one and two ops earlier (no `RCALL` and `END` of their
own: 48 B less), and `PCSG01250` shares Luna's own patterns for that title. The 20 title strings are about 200 B, and
Luna's ID table, a `char[10]` array in the data segment, shares none of them.

In the normal worker that is 1,288 B (`.rodata` +304 B, `.data` +976 B), and on `main` `b55c5f5` the last pages of its
code and data segments had 788 + 152 = 940 B of room (772 + 152 B on `665479d`): no placement of the tables keeps its
pages. As #394 wrote them,
every table in the data segment as Rubi's filters were, the worker took one page more in the normal build and two in
the debug build. So:

- the filters' index, `PCSG00628`'s ops and escapes (`r_PCSG00628`, `r628_esc`: #316's audit) and the five arrays this
  batch adds or grows (`r_PROTOTYPE_NAMELINE`, with `PROTOTYPE` in it, `r_PCSG00472`, with `IFENGINE`,
  `r_AOZORA_RUBY`, `r_CARET_NEWLINE`, `r_PCSG01250`), 853 B [`arm-vita-eabi-nm -S`], are read-only data again
  (`core/hook_luna.c`, without `VJO_DATA_TABLE`), and
- `core/hook_luna.c` is built for size in the worker, as `core/hook_re.c` is (`vita/CMakeLists.txt`; the Rubi app's copy,
  a source property, too: its `.text` 779,156 → 779,092 B, normal build, audit-rubi-merge-394 N4): its filter ops and
  Luna sites' reads are control code, run a few times per hit or job; its object's text goes from 5,612 to 4,168 B and
  its rodata from 13,595 to 13,347 B (the normal build's flags). Its speed is not measured on a console.

Both builds then keep their pages. Measured with VitaSDK GCC 15.2.0 in `tools/fast_gate.sh`'s image
(`tools/check_memory_ledger.py`'s module rows, `arm-vita-eabi-size -A` on both builds' `VitaJPOverlay_OCR`), main
`665479d` against this change (the last two rows: on `b55c5f5`):

| Game worker | Debug: main → this change | Normal: main → this change |
|---|---:|---:|
| `.text` | 145,824 → 144,520 B | 141,592 → 140,288 B |
| `.rodata` | 32,216 → 33,064 B | 27,656 → 28,504 B |
| `.data` | 5,560 → 5,680 B | 5,552 → 5,672 B |
| module rows, text + data/bss | 180,208 + 36,720 → 179,760 + 36,840 B | 171,260 + 36,712 → 170,796 + 36,832 B |
| loaded pages | 217,088 B, unchanged | 208,896 B, unchanged |
| as #394 wrote it: rows, pages | 180,496 + 37,696 B, 225,280 B | 171,548 + 37,688 B, 212,992 B |
| the tables moved, `-O2`: rows, pages | 181,360 + 36,840 B, 221,184 B | 172,412 + 36,832 B, 212,992 B |

- Room left in the last pages: the normal worker 1,236 B of code and 32 B of data, the debug worker 464 B and 24 B.
- The kernel's and the Shell's rows are unchanged. No block, thread or allocation; no budget raised. None of it is
  measured on a console.

## #338's audit: the trusted hooks' fixes (no API change)

No block, thread, stack or allocation; no module takes a page more. Measured with VitaSDK GCC 15.2.0 in the gate's
pinned image (`tools/check_memory_ledger.py`'s module rows), main `70deda7` against this change:

| Module | Debug: main → this change | Normal: main → this change | Loaded pages |
|---|---:|---:|---:|
| Game worker | 179,760 + 36,704 → 179,840 + 36,712 B | 170,828 + 36,696 → 170,876 + 36,704 B | 217,088 / 208,896 B, unchanged |
| Shell | 98,244 + 77,792 → 98,276 + 77,792 B | 85,972 + 77,696 → 85,988 + 77,728 B | 176,128 / 163,840 B, unchanged |
| Kernel | unchanged | unchanged | unchanged |

- **The Shell** (SceShell's pages, AGENTS.md rule 1): `shell/history.c`'s grown-line rewrite is main's steps
  reordered (the open first, the truncation inside the count's odd window), with the two steps the order needs: the
  check that the file still ends with the record it replaces (else a file replaced or shortened since would be extended
  with zeros) and a second read of its end after the truncation (the next write's place whether the console's
  `SCE_O_APPEND` places each write or only the open). 48 B of the normal Shell's text, 32 B of which the normal
  build's short job line pays back (its format, `worker_short_format`, in the data segment: +32 B there): +16 B of text,
  28 B left of its last page; and, with the debug build's hook source line moved into the result's batch (#338's audit
  F2), 32 B of the debug Shell's (28 B left). `vjo_history_append`'s frame 504 → 528 B on VjoNet's fixed 20 KiB stack (`-fstack-usage`, the normal
  Shell's flags without LTO). Two other forms were measured and left: the rewrite without the end's check (on main
  `9ad8eb5` +16 B of normal text instead of +48 B; on `363f822`, with the format in the data segment, 16 B of normal
  text less than this form and 16 B of debug text more [audit-rubi-audit-fix-hooks' measurement]; but it extends a
  replaced file with zeros, the audit's P6) and the record's start set from the earlier one with a seek to it (+48 B
  debug and +80 B normal on main `4c5ad04`, a page past both budgets there).
- **The worker**: the cache's trust bit (`VjoHookLearned.trusted`, bit 1 of the 96-byte record's site word) and
  `vjo_hook_select_learned`'s trust argument.

## A game's own line finder (#333)

A game's own line finder ([guide](guide.md#a-games-own-page)) is read by the Shell with the game's ON/OFF, into the
`VjoGames` table that read already takes from the control thread's static 24 KiB scratch (`shell/game.c`
`vjo_game_enabled`: games.ini's rows, then ocr_line_fix.ini's into the same 8,200 B, and the file's text, at most
`VJO_GAMES_TEXT_MAX`, 12,288 B: arena marks, released before the call returns). **No block, thread, stack or
allocation; no module takes a page more; no budget raised.**

- **The Shell:** `games.ini`'s reader with the line finder's five names (`core/games.c` `vjo_games_parse_kind`), the
  row lookup both reads use (`vjo_games_row_as`, not inlined: LTO's two inlined copies are 32 B more of the code in
  both builds on main `73db61e`, a page past each build's budget, and were 32 B (normal) and 48 B (debug) on #395's
  merge [measured: full Vita builds without and with the attribute; on `099b156`, audit-rubi-333's]), the second read
  and the value's place in `cfg` (`apply_config`: `ocr_finder`, and for Rubi's
  finder `ocr_line_fix`), `title_line_fix` (4 B), and the game's start line's two fields. Its tables are in the data
  segment ("Tables in the data segment" below): the five names (`vjo_line_finder_names`, whose last four are
  `config.c`'s `ocr_line_fix` table now: 24 B for its 20), `ocr_finder`'s two (`vjo_finder_names`, config.c's own table
  before, now shared with the start line), `games.ini`'s two (12 B, code before), the new file's path (40 B), and the
  start line's format, 1 B longer for its two fields than with the dictionary's name.
- **Room for it in the Shell's code:** `vjo_title_name` reads the title into its caller's buffer, without its two
  128 B buffers and the debug build's line of both parameters, which a console run answered (in test.1-test.4's logs
  every one of the 55 game starts had parameter 9 refused, 0x80801000, and parameter 10 the title: the owner's
  `--collect` logs, counted; 115 lines, as each collect's log repeats the one before); the normal build's self-test
  notice (83 B) is in the data segment.
- **The kernel:** `games.ini`'s reader is the Shell's (`core/games.c`, built with `VJO_KERNEL`, which leaves the line
  finder's names out): +76 B of code for the names' loop and the row lookup, the same pages. Its two names stay in its
  code pages, which have room, where its data page has less.
- **The Rubi app** (its own process, not SceShell's): the game page and the file's table, 16,816 B more `.bss` (the
  table's two copies, 8,200 B each, as `games.ini`'s two; the list's status while the page shows its own) and
  6,144 B (normal) / 6,214 B (debug) more code.
- **Measured** with VitaSDK GCC 15.2.0 in the gate's pinned image (`tools/check_memory_ledger.py`'s module rows, text
  + data/bss; the app's ELF with `arm-vita-eabi-size`), #395's merge `099b156` against its parent `786dd8b`:

  | Module | Debug: `786dd8b` → `099b156` | Normal: `786dd8b` → `099b156` | Loaded pages |
  |---|---:|---:|---:|
  | Shell | 98,164 + 77,728 → 98,212 + 77,792 B | 85,900 + 77,552 → 85,940 + 77,696 B | 176,128 / 163,840 B, unchanged |
  | Kernel | 43,432 + 77,780 → 43,508 + 77,780 B | 38,280 + 76,092 → 38,356 + 76,092 B | 122,880 / 118,784 B, unchanged |
  | Game worker | 179,760 + 36,704 B, unchanged | 170,812 + 36,696 B, unchanged | unchanged |
  | Rubi app (ELF: text, data, bss) | 961,484, 15,944, 7,138,272 → 967,698, 16,016, 7,155,088 B | 961,080, 15,936, 7,138,240 → 967,224, 16,008, 7,155,056 B | its own process |

  Room left in the last pages then: the normal Shell 76 B of code (116 before) and 128 B of data (272); the debug Shell
  92 B of code (140) and 32 B of data (96); the debug kernel 1,548 B of code and 44 B of data. (The same Shell and
  kernel rows as on main `9ad8eb5`, where the PR measured them; audit-rubi-333 measured these too.)
- **Its audit's follow-up** (the app puts a file left as its backup back and keeps a hand-edited one, a slid touch
  does nothing, the page's note beside an older overlay): app code only. On main `73db61e` the Shell's and the
  kernel's SELFs are byte for byte main's in both builds (`sha256sum`); the app's text 968,372 → 968,768 B (normal)
  and 968,832 → 969,232 B (debug), its data the same, its `.bss` 7,155,072 → 7,155,088 B (normal) and the same in
  the debug build.
- **Time:** at a game's start or resume the Shell opens and reads one more small file (at most 12 KiB; a missing one
  is one failed open), unmeasured on the console. Nothing changes per job or per frame.

## #382's and #387's audit's follow-up (no API change)

A try that hands the job to its next one leaves the log its blocks' steps, as before, but keeps the FREE step of a
block it did not take: an earlier job's resident weights, LM or cascade model its check counted as free and released
(`game_ocr/main.c` `steps_drop_blocks`). The Rubi app's plugins-only plan line names its files. No block, thread,
stack or allocation; no module takes a page more.

Measured with VitaSDK GCC 15.2.0 in the gate's pinned image (`tools/check_memory_ledger.py`'s module rows, text +
data/bss), main `b88772c` against this change:

| Module | Debug: main → this change | Normal: main → this change | Loaded pages |
|---|---:|---:|---:|
| Game worker | 179,712 + 36,704 → 179,760 + 36,704 B | 170,764 + 36,696 → 170,812 + 36,696 B | 217,088 / 208,896 B, unchanged |
| Kernel, Shell | unchanged | unchanged | unchanged |

The worker keeps 464 B (debug) and 1,220 B (normal) of its last text page.

## #316's audit: the hook fixes (no API change)

The game worker only, in every game that is ON: no block, thread, stack or allocation, and no module takes a page more.
`RF_PCSG00628`'s 15 new ops and their strings (`r628_esc`, 109 B) are in the data segment (`VJO_DATA_TABLE`), as
Rubi's other filters are. The code: the decoder's 0xA0 branch (a select on the encoding, `VJO_HOOK_ENC_CP932_A0`,
which the two MSGSET entries name: no code chooses it), `vjo_hook_clean` without its U+F8F0 drop, and the r15 entry's
check. Measured with VitaSDK GCC 15.2.0 in the gate's pinned image (`tools/check_memory_ledger.py`'s module rows), main
`9ad8eb5` against this change:

| Module | Debug: main → this change | Normal: main → this change | Loaded pages |
|---|---:|---:|---:|
| Game worker | 179,712 + 36,416 → 179,712 + 36,704 B | 170,780 + 36,408 → 170,764 + 36,696 B | 217,088 / 208,896 B, unchanged |
| Kernel, Shell | the SELF files byte-identical | the SELF files byte-identical | unchanged |

The worker keeps 512 B (debug) and 1,268 B (normal) of its last text page and 160 B / 168 B of its last data page. On
main `3d095b4`, before API 54's four text pages, the debug worker had 40 B left, and the first form of this change took
96 B, a page past its budget: the decoder's flag as a parameter kept a register and a stack slot through its loop
(48 B); as an encoding that the two entries name, with a select in the branch, it is 14 B (+16 B in all there).

## SC3 text, #368's audit's follow-up (no API change)

The charset's load ([native-hooks.md](native-hooks.md#sc3-text)) tries a timed-out read again, keeps a failed close as
the job's error and frees a block it could not give back before it takes another (`game_ocr/main.c`
`hooks_sc3_load`, `sc3_release`); the Rubi app's Install page names the data files and their folder
(`settings/install.c`). No block, thread, stack or allocation: `VjoGameSc3` is the same block, taken and freed at the
same places, and never two.

Measured with VitaSDK GCC 15.2.0 in the gate's pinned image (`tools/check_memory_ledger.py`'s module rows, text +
data/bss), main `68041a7` against this change:

| Module | Debug: main → this change | Normal: main → this change | Loaded pages |
|---|---:|---:|---:|
| Game worker | 163,656 + 35,880 → 163,688 + 35,880 B | 154,692 + 35,864 → 154,724 + 35,864 B | 200,704 / 192,512 B, unchanged |
| Kernel, Shell | unchanged | unchanged | unchanged |

The worker keeps 152 B (debug) and 924 B (normal) of its last text page.

## VOCR6's follow-up set: all three sizes (vita-vn-ocr #157; no API change)

The three sizes' recognizers become VOCR6-S, VOCR6-N and VOCR6-L of the v0.4.1-vocr6.2 pre-release
(`VOCR6.2_S_w8.vocr`, `VOCR6.2_N_w8g2.vocr`, `VOCR6.2_L_w8_2l.vocr`; [vita-vn-ocr.md](vita-vn-ocr.md#models)): Small's
blob is plain, Normal's has a head gate again (and the two-level head), Large's the two-level head alone. Every figure
below is the real files' (`host/tests/test_vocr.c` plans each pinned blob and checks its arena; `host/tests/test_ledger.c`
and `core/vocr_ocr.c`'s layout code give the jobs) [measured on the host build, the release's public files]:

| | before (main: VOCR5-S, v0.4.0-vocr6.1's VOCR6-N and VOCR6-L) | this set | the ledger |
|---|---:|---:|---|
| Small's blob, weights block | 821,584 B, 823,296 B | 819,792 B, 823,296 B (no gate section; the same 201 pages) | — |
| Normal's blob, weights block | 1,187,616 B, 1,187,840 B | 1,189,408 B, **1,191,936 B** (its 1,792 B gate section: a page more) | — |
| Large's blob, weights and cascade blocks | 2,475,680 B, 2,478,080 B | 2,475,680 B, 2,478,080 B (the same size) | `VJO_BUDGET_GAME_WEIGHTS`, `_CASCADE` 2,478,080 B, unchanged |
| the W8I arenas, `ocr_head_gate = on` (Small / Normal / Large) | 124,880 / 184,400 / 262,480 B | 124,832 / 184,448 / 262,480 B | — |
| the largest full-screen job (Normal, Large's cascade, both helpers, the LM, the refinement, the spotter's scratch; host build) | 1,297,488 B (1,218,048 B with GRAD) | 1,297,584 B (1,218,192 B with GRAD): the gate's 48 B in Normal's helpers' contexts (and its own, with GRAD) | `VJO_BUDGET_GAME_WORKSPACE` 1,298,432 B, unchanged (848 B left) |
| a first Small / Normal / Large job at the full screen (weights, workspace, capture; the app's figure) | 1,019,904 / 1,441,792 / 2,813,952 B | 1,019,904 / **1,445,888** / 2,813,952 B | — |
| a real peak (Normal with Large's cascade, the 4 MB LM, both helpers), GRAD / the spotter | 9,113,600 / 9,248,768 B | 9,117,696 / 9,252,864 B | `VJO_LIMIT_GAME_JOB` 11,214,848 B (the worst case 11,014,144 B), unchanged |

- **No budget changes.** Normal's weights block takes back the page v0.4.0-vocr6.1's VOCR6-N had saved (VOCR5-N's
  1,191,936 B); Small's keeps its pages; every job's workspace keeps its pages. The LM blocks are unchanged.
- **Code.** None: the pins' names, sizes, SHA-256 and arenas. The kernel's broker and the Shell's tier names change
  length (`VOCR6.2_S_w8.vocr`, `VOCR6.2_N_w8g2.vocr` and `VOCR6.2_L_w8_2l.vocr`, 4 B longer in all than the three
  before). The module rows, both builds [measured with `tools/check_memory_ledger.py`: main `b55c5f5` from its Release
  run's artifacts (40597389507) against this change's fast-gate builds, the same pinned image]:

  | Module | Debug: main → this change | Normal: main → this change | Loaded pages |
  |---|---:|---:|---:|
  | Game worker | 180,208 + 36,720 B, unchanged | 171,244 + 36,712 → 171,260 + 36,712 B | 217,088 / 208,896 B, unchanged |
  | Shell | 98,276 + 77,816 → 98,284 + 77,816 B | 85,988 + 77,728 → 85,996 + 77,728 B | 176,128 / 163,840 B, unchanged |
  | Kernel | 43,508 + 77,780 → 43,496 + 77,780 B | 38,356 + 76,092 → 38,344 + 76,092 B | 122,880 / 118,784 B, unchanged |

  The debug worker keeps its 16 B of the last text page, the normal one 772 B; the normal Shell keeps 20 B of its last
  text page (28 B before).
- **The one-VPK payload** keeps its 26 files (24 and, since API 54, the spotter's two): `VOCR6-ATTRIBUTION.md` is
  v0.4.1-vocr6.2's (4,442 B, 4,222 B before) and
  the three recognizers are the new set's (4,484,880 B, 4,484,880 B before: Small 1,792 B smaller, Normal 1,792 B
  larger). No block, thread or allocation site is new. Nothing of it has run on a console.

## VOCR6's Normal and Large (vita-vn-ocr #125; no API change)

The Normal and Large sizes' recognizers become VOCR6-N and VOCR6-L (`VOCR6_N_w8_2l.vocr`, `VOCR6_L_w8_2l.vocr`, the
v0.4.0-vocr6.1 pre-release; [vita-vn-ocr.md](vita-vn-ocr.md#models)); Small keeps VOCR5-S. Every figure below is the
real files' (`host/tests/test_vocr.c` plans each pinned blob and checks its arena; `host/tests/test_ledger.c` and
`core/vocr_ocr.c`'s layout code give the jobs) [measured on the host build, the staged release files, whose SHA-256s
are the pins]:

| | VOCR5 (0.9.8) | VOCR6 (this change) | the ledger |
|---|---:|---:|---|
| Normal's blob, weights block | 1,189,408 B, 1,191,936 B | 1,187,616 B, **1,187,840 B** (no gate section: a page fewer) | — |
| Large's blob, weights and cascade blocks | 2,477,472 B, 2,478,080 B | 2,475,680 B, 2,478,080 B (the same 605 pages) | `VJO_BUDGET_GAME_WEIGHTS`, `_CASCADE` 2,478,080 B, unchanged |
| Normal's and Large's W8I arenas, `ocr_head_gate = on` | 184,448 / 262,528 B | 184,400 / 262,480 B (no gate: 48 B less each) | — |
| the largest full-screen job (Normal, Large's cascade, both helpers, the LM, the refinement; host build) | 1,218,240 B | 1,218,048 B (192 B less: four contexts) | `VJO_BUDGET_GAME_WORKSPACE` 1,220,608 B, unchanged |
| a first Normal / Large job at the full screen (weights, workspace, capture; the app's figure) | 1,445,888 / 2,813,952 B | **1,441,792** / 2,813,952 B | — |
| the game's worst case / a real peak (Normal with Large's cascade, the 4 MB LM, both helpers) | 10,420,224 / 9,117,696 B | 10,420,224 / 9,113,600 B (+16,384 B in an SC3 game) | `VJO_LIMIT_GAME_JOB` 10,604,544 B, unchanged |

- **No budget changes.** Large's blob is 1,792 B smaller than VOCR5-L's two-level blob (it has no gate section) and
  keeps its pages; Normal's drops a page; every job's workspace keeps its pages. The LM blocks are unchanged.
- **Code.** The game worker gains the per-model LM settings (`core/vocr_ocr.c` `lm_retuned` and
  `vjo_vocr_lm_settings`, which `lm_opts` calls for the job's decoder and its cascade's); the kernel's broker and the
  Shell's tier names are one byte longer each (`VOCR6_N_w8_2l.vocr` against `VOCR5_N_w8g2.vocr`). The module
  rows, both builds [measured with VitaSDK GCC 15.2.0, `tools/check_memory_ledger.py`'s rows: main `1c9d048` from its
  Release run's artifacts (40546045095) against this change's fast-gate builds of `7033637`, the same pinned image]:

  | Module | Debug: main → this change | Normal: main → this change | Loaded pages |
  |---|---:|---:|---:|
  | Game worker | 163,512 + 35,880 → 163,640 + 35,880 B | 154,548 + 35,864 → 154,676 + 35,864 B | 200,704 / 192,512 B, unchanged |
  | Shell | 102,372 + 73,664 B, unchanged | 90,044 + 73,520 B, unchanged | 176,128 / 163,840 B, unchanged |
  | Kernel | 43,356 + 77,772 B, unchanged | 38,204 + 76,084 B, unchanged | 122,880 / 118,784 B, unchanged |

  The worker's code grows 128 B in both builds (the settings lookup, the two decoders' extra argument) and keeps
  200 B (debug) and 972 B (normal) of its last text page; the normal Shell keeps its 68 B of code room. The kernel's
  longer names fit its read-only data as it was.

- **The one-VPK payload** gains `VOCR6-ATTRIBUTION.md` (4,222 B): the Rubi app's install copies one more file, with no
  new buffer (`include/vjo_payload.h`). No block, thread or allocation site is new. Nothing of it has run on a
  console.

## #324's retry, its audit's follow-up (no API change)

The retry after a model's refused weights (#324) now runs in API 47's retry block: the tries' totals, the shared
stable-text wait, the step log's drop of the given-back blocks and the kept capture are one copy of code for both
(`game_ocr/main.c`). No block, thread, stack or allocation; no module takes a page more.

Measured with VitaSDK GCC 15.2.0 in the gate's pinned image (`tools/check_memory_ledger.py`'s module rows, text +
data/bss), main `84dbad9` against this change:

| Module | Debug: main → this change | Normal: main → this change | Loaded pages |
|---|---:|---:|---:|
| Game worker | 163,528 + 35,880 → 163,512 + 35,880 B | 154,548 + 35,864 B, unchanged | 200,704 / 192,512 B, unchanged |
| Kernel, Shell | the SELF files byte-identical | the SELF files byte-identical | unchanged |

The worker keeps 328 B (debug) and 1,100 B (normal) of its last text page.


## The box suggested on first use (API 52, #358)

[The box](first-box.md) rides in the result: no block, thread, stack or allocation, and no module takes a page more.

- **The game worker** runs the rule on its job's recognized lines (`core/region_suggest.c`, `-Os` like the job's
  control code; `vjo_region_suggest`'s frame 240 B and `vjo_vocr_suggest`'s 200 B on the worker's 32 KiB stack,
  `-fstack-usage`) and counts each line's kana as it places its text. `VjoVocrLineStat` keeps the line's box and
  kana count (in its pad and 4 B more: 16 lines, 64 B of the job's stats on that stack).
- **The result** is 4 B longer (`suggest_rows`, the band's rows at the screen's bottom, before the text): the
  kernel's two copies of it are 8 B more of `.bss`.
- **The Shell** keeps the band in its view as one word (`VjoView.suggest`: none, the newest screen's rows, or taken
  in this activation; `core/region_select.c` `vjo_rsel_publish` and `vjo_rsel_take`, compare-and-swap, no lock), and
  the overlay opens the region mode with it (`vjo_rsel_preset`) once an activation. Enter saves it with the region
  command a box drawn with □ uses (`VJO_CMD_SET_REGION`): no command, publish or job of its own. The setting is the
  Rubi app's, in `region.ini` (no `config.ini` key: no parse in the Shell, no field in its three `VjoConfig` copies).
- **Fitting the Shell.** #376 left the debug Shell 28 B of code room, and the first version took 152 B. What made it
  fit: Enter saves as a drawn box does (the read at once and its command went); the band crosses as one word, not
  four coordinates, and the view keeps one word, not a box and an activation number; every drawn box's hint asks the
  question ("Use this box? %d x %d" for "Box %d x %d": one format, no second copy); and the game's start line, a cold
  log format, is in the data segment (`VJO_DATA_TABLE`), as earlier changes did with others.
- **Measured** with VitaSDK GCC 15.2.0 in the gate's pinned image (`tools/check_memory_ledger.py`'s module rows, text +
  data/bss), main `9bb6943` against this change:

  | Module | Debug: main → this change | Normal: main → this change | Loaded pages |
  |---|---:|---:|---:|
  | Game worker | 162,472 + 35,872 → 163,528 + 35,880 B | 153,508 + 35,856 → 154,548 + 35,864 B | 200,704 / 192,512 B, unchanged |
  | Shell | 102,388 + 73,568 → 102,372 + 73,664 B | 90,020 + 73,432 → 90,044 + 73,520 B | 176,128 / 163,840 B, unchanged |
  | Kernel | 43,356 + 77,764 → 43,356 + 77,772 B | 38,204 + 76,076 → 38,204 + 76,084 B | 122,880 / 118,784 B, unchanged |

  The normal Shell keeps 68 B of its last text page (92 B before) and 208 B of its last data page (296 B); the debug
  Shell 28 B (12 B) and 64 B (160 B); the debug kernel 52 B of its last data page (60 B); the worker 312 B (debug)
  and 1,100 B (normal) of its last text page. The Shell's frames: `region_hint` 256 B (its 192-byte hint buffer, as
  before), `leave_region_mode` 8 B, the word's helpers and `vjo_rsel_preset` none (`-fstack-usage`, the normal
  build's flags without LTO).

## Duplicate-aware mining (#314): the Rubi app only

Duplicates and known words at export ([anki.md](anki.md#duplicates)) are the Rubi app's code and files: the Shell, the
game worker and the kernel are byte-identical to main's, in both builds (the build log's module rows, main `3d095b4`
and #390 at `aab20f9`: Shell 85,980 + 77,488 B normal, 98,260 + 77,664 B debug). The app's export takes, in its 2 MiB arena
(`VJO_EXPORT_MEM`), 24 B a card for every card's word while it lists the cards (20,005 cards: 1,121,712 B at its
peak with the package's own work, measured on the host) and 8 B a known word (65,536 at most: an import peaks at
541,704 B, and Export all of the 20,005 cards with 65,536 known words loaded at 1,646,000 B, the worst case, measured on
the host); with more cards than that holds, it exports without the duplicate check and says so.

**Stage B, a mark in the popup, as #314 specifies it, does not fit.** A minimal version (the word key linked into the
Shell at `-Os`, a marker file at Enter, one `sceIoGetstat` after each lookup, the KNOWN mark and its dim colour; no log
line, no app-side sync), built with the gate's toolchain on main `030c7ea`:

| Shell | main | minimal stage B | by symbol (normal) |
|---|---:|---:|---|
| normal | 85,980 + 77,488 B (36 B of code room) | 86,820 + 77,488 B: 167,936 B of pages, over the 163,840 B budget | `fold` 180, `net_main` +132, `word_marker` 124, `vjo_word_key` 116, `view_publish` +88, `do_save` +36, `render` +24 |
| debug | 98,260 + 77,664 B (44 B of code room) | 99,084 + 77,664 B: 180,224 B of pages, over the 176,128 B budget | |

The data segments had 336 B (normal) and 160 B (debug) of room, so moving cold strings there cannot pay for 804 / 780 B
of code, and every cold Shell file is already `-Os`. Rule 1 lets the Shell's pages only shrink: that design is not
built. A smaller split (the Rubi app keeping markers named by the entry's own strings, one stat in the Shell) is neither
built nor measured: about 135-175 B of Shell code by audit-rubi-rm4's estimate [estimate], against 28 B of code room in
both builds on main `33fdf30` (normal 85,988 + 77,728 B, debug 98,276 + 77,792 B, the module rows) [measured]. Whether
it gets an item of its own is the owner's call.

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
  text + data/bss), main `199fe31` against this change:

  | Module | Debug: main → this change | Normal: main → this change | Loaded pages |
  |---|---:|---:|---:|
  | Game worker | 161,016 + 35,328 → 162,232 + 35,624 B | 152,084 + 35,320 → 153,268 + 35,608 B | 200,704 / 192,512 B, unchanged |
  | Kernel | 43,160 + 77,764 → 43,356 + 77,764 B | 38,008 + 76,076 → 38,204 + 76,076 B | 122,880 / 118,784 B, unchanged |
  | Shell | 102,092 + 73,408 → 102,100 + 73,408 B (the self-test's name of the block) | unchanged | 176,128 / 163,840 B, unchanged |

  The worker's text is `core/hook_sc3.c` at `-Os` (the span and the decoder), the SC3 read and decode in
  `game_ocr/hooks.c` and the load in `game_ocr/main.c`; its data, the three verified entries (120 B) and the three
  charsets' pins (144 B) in the data segment ("Tables in the data segment" below) and their state. It kept 1,608 B
  (debug) and 2,380 B (normal) of its last text page on its branch, 1,240 / 1,256 B of data. Merged on main (its
  parent `19986c7` → `e2c49ee`) it is +1,200 / +1,168 B of text and +288 / +280 B of data/bss, 161,272 + 35,584 →
  162,472 + 35,872 B (debug) and 152,340 + 35,576 → 153,508 + 35,856 B (normal), which left 1,368 / 2,140 B of the
  last text page and 992 / 1,008 B of data [measured: the same rows, both builds of both revisions]. The kernel's
  196 B are the open's path from the caller's title (`loader_title`) and its size rule; no buffer: the charset
  crosses the existing 32 KiB staging buffer in one read.
- **Not added:** the charsets compiled into the worker (27,732 B of tables: about seven more pages in every game that
  is ON [estimate]), or read in the Shell (no room) or by the Rubi app (not running beside a game).

## Lookups: #321

#321's lookups ([the report](../reports/yomitan-check.md#321-what-remained)) are Shell code and data only, and
the Shell keeps its pages in both builds: the rule table moves to the data segment, which takes a page more while
the code takes a page fewer.

- **The rule table** (`tools/gen_deinflect.py`): 724 → 711 rules (Yomitan's whole `-おく` for the old `-toku`,
  16 → 6; Rubi's てん and でん; `-ん`'s かん out), `deinflect_rules` 4,344 → 4,266 B, now `VJO_DATA_TABLE`;
  `deinflect_strings` 4,549 → 4,483 B, still rodata. A key's bucket is its last byte's, a continuation byte, so
  `deinflect_offsets` holds 65 offsets, not 257: 514 → 130 B of data. The causative passive's two steps are one
  compare in `deinflect`.
- **The lookup** (`lookup_pages`, `-Os`): the particle before a katakana word (once a lookup, on the text's first
  two characters), the particle before a form of する (per candidate: two compares, then one `memcmp`),
  `names_first`'s order at a span, and a later shallower candidate replacing a deeper one: `lookup_pages` 5,128 →
  5,400 B normal, 5,144 → 5,424 B debug; `read_entry`'s hidden rank +16 B; `take_postings`' fewer steps first
  +2 B. `particle_forms` (44 B) is in the data segment.
- **`names_first`**: its parse shares `ocr_vertical`'s branch and values (`apply_config` +32 B normal, +28 B debug;
  the key, 12 B, in the data segment) and a field in each of the three `VjoConfig` copies (4 B each).

Measured with VitaSDK GCC 15.2.0 in the pinned image (`tools/fast_gate.sh`'s; `tools/check_memory_ledger.py`'s
module rows, text + data/bss; `arm-vita-eabi-nm -S` for the symbols), main `68041a7` against this change:

| Shell | main | #321 | Loaded pages |
|---|---:|---:|---:|
| Debug | 102,372 + 73,664 B (102,400 + 73,728) | 98,284 + 77,608 B (98,304 + 77,824) | 176,128 B, unchanged |
| Normal | 90,044 + 73,520 B (90,112 + 73,728) | 85,948 + 77,488 B (86,016 + 77,824) | 163,840 B, unchanged (`--exact`) |

Room left: the debug Shell's code 20 B (28 B on main), its data 216 B (64 B); the normal Shell's code 68 B (68 B),
its data 336 B (208 B). The kernel and the game worker are byte-identical to main's in both builds. The Rubi app,
which links `core/config.c` and `core/local_dict.c` but none of the rules, grows by 234 B (normal) and 155 B (debug)
of `eboot.bin`, in its own process. The VjoNet thread's
frames are unchanged (`lookup_pages` 1,584 B, `deinflect` 272 B, `take_postings` 416 B, `read_entry` 64 B;
`-fstack-usage` with the Shell's flags, both builds). Candidates per span (`deinflect()` on every span the taps
scan, host, ASan/UBSan, the audit of #320's harness): 1,175,231 → 1,175,510 on the 64,579 taps, 5,810,009 →
5,811,312 on 330,517, at most 15 and 17 a span either way, none at the 128 cap, the same arena peaks (24,432 and
27,696 B). No allocation, thread, stack or budget is added; nothing of it has run on a console.

**#321's audit.** The particle's する check moved before the depth test, so the span's particle is the same for any
Dictionaries shown (`lookup_pages` +28 B normal, +24 B debug). Measured as above, main `099b156` against this
change:

| Shell | main | #321's audit | Loaded pages |
|---|---:|---:|---:|
| Debug | 98,212 + 77,792 B | 98,244 + 77,792 B | 176,128 B, unchanged |
| Normal | 85,940 + 77,696 B | 85,972 + 77,696 B | 163,840 B, unchanged (`--exact`) |

Room left: the debug Shell's code 60 B, its data 32 B; the normal Shell's code 44 B, its data 128 B. The kernel and
the game worker are byte-identical to main's in both builds; the Rubi app's `eboot.bin` (which links
`core/local_dict.c` and `core/config.c`) grows by 17 B (debug) and 28 B (normal), in its own process. The VjoNet
frames are unchanged (`-fstack-usage`, as above). Nothing of it has run on a console.

## Why the overlay waits (#358)

The open overlay's wait hint ([text-stability.md](text-stability.md#why-the-overlay-waits-358)) is Shell code and
data only:
- the choice of status as a try starts (`shell/worker.c` `start_job_kind`);
- the count's reset when a region is saved (`on_command`);
- `ocr_wait_hint`'s parse, which shares `ocr_boost_clock`'s branch (`core/config.c`), and its field in each
  `VjoConfig` copy.

The hint (163 B since the audit's follow-up, 153 B in #364) and the key (14 B) are in the data segment
(`VJO_DATA_TABLE`), and so is "Recognizing…" now (15 B), which gives the Shell's code that much room back. No block,
thread, stack or allocation; no budget raised.

Measured with VitaSDK GCC 15.2.0 in the pinned image (`tools/fast_gate.sh`'s; `tools/check_memory_ledger.py`'s
module rows, text + data/bss):

| Module | Debug | Normal | Loaded pages |
|---|---:|---:|---:|
| Shell, main `2f68854` (before #364) | 101,988 + 73,200 B | 89,884 + 73,232 B | 176,128 / 163,840 B |
| Shell, #364 (`199fe31`; the same on `6bfd1f4`) | 102,092 + 73,408 B | 89,980 + 73,416 B | unchanged |
| Shell, the audit's follow-up on `6bfd1f4` (the hint's new text) | 102,092 + 73,416 B | 89,980 + 73,432 B | unchanged |
| Shell, `main` `8da24c8` (#376) → the follow-up | 102,372 + 73,560 → 102,372 + 73,568 B | 90,020 + 73,416 → 90,020 + 73,432 B | unchanged |
| Kernel, game worker | unchanged | unchanged | unchanged |

On `8da24c8` with the follow-up, the normal Shell keeps 92 B of code room and 296 B of data room; the debug Shell
28 B and 160 B.

**With #355 and #359** (the audit's F2): the measured rows, `main` 8da24c8 with this change and each sibling's Shell
change (#355 at 9e44739, #359 at f630b69), text + data/bss. Those heads predate #376 and #368, so the scratch merges
were resolved for the build only.

| Shell | Normal | Debug | Ledger |
|---|---:|---:|---|
| `main` 8da24c8 (#376 took the debug Shell's code from 102,092 to 102,372 B) | 90,020 + 73,416 B | 102,372 + 73,560 B | pass |
| this change | 90,020 + 73,432 B | 102,372 + 73,568 B | pass (92 / 28 B of code room) |
| + #359's Shell part (its log line, `SCREEN_MOVING_LOG` 190 B) | 90,036 + 73,616 B | 102,404 + 73,760 B | **debug FAIL** (code 4 B past its page) |
| + #355, no clause | 90,076 + 73,456 B | 102,436 + 73,576 B | **debug FAIL** |
| + #355 as it is (`WAIT_HINT_PAUSE`, a second copy of the hint, 214 B) | 90,108 + 73,664 B | 102,468 + 73,800 B | **debug FAIL** (code and data) |
| + #355 as it is + #359 | 90,124 + 73,856 B | 102,500 + 73,984 B | **both FAIL**: 172,032 / 184,320 B of pages |
| + #355 with its clause as a format of one copy + #359 | 90,140 + 73,680 B | 102,500 + 73,808 B | **both FAIL** (the code) |

- **The clause cannot be a second copy.** As a format of one copy (`"%s Or turn on Pause the game to read."`,
  written into a 201 B buffer in `start_job_kind`'s frame on VjoControl's 16 KiB stack when the pause would help),
  the normal Shell's data stays in its page: before #376 it measured 90,100 + 73,680 B, and the ledger passed in
  both builds.
- **The debug Shell's code is what fails now.** #376 left it 28 B of room, and either sibling alone needs more. Room
  has to be found first: a table to the data segment, as `RECOGNIZING` did, or a debug-only log line shortened.
- The stack buffer's depth is not painted, as VjoNet's is, so #355 measures the frame (`-fstack-usage`) when it takes
  the clause.

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
## The spotter finder (API 54)

`ocr_finder = spotter`, the default ([vita-vn-ocr.md](vita-vn-ocr.md#the-spotter-finder-api-54); `grad` is the
finder path as before), adds to the game's pool one block and one part of the job's scratch, both only in a job that
uses the spotter, and the spotter's code to the game worker, in every game that is ON:

- **`VjoGameVocrSpot`**, the spotter's weights and their parsed form (`vocr_textspot6`, 3,964 B on the Vita), each
  16-byte aligned, page-rounded: 57,344 B for the pinned 52,176 B (vita-vn-ocr `v0.3.10-textspot8.1`), at most
  516,096 B (`VJO_BUDGET_GAME_SPOT`: the 512,000 B blob vita-vn-ocr's rule admits). `USER_RW` only, through
  `game_memblock_alloc` (`GL_SPOT`, the game ledger's twelfth item, after #368's `GL_SC3`; `VJO_LEDGER_MAX_ITEMS`
  11 → 12, 8 B more of bss in each module's ledger) after its own check that the game keeps
  `VJO_GAME_RESERVE` (1 MiB since #280; 4 MiB before) beside it, before the job's memory check (its parsed form
  sizes its scratch); kept
  between jobs like the LM, given back by a job without the spotter and with the other resident blocks. The memory
  check counts it as a block the job gives back, and the job keeps it only when it and its scratch fit beside what
  the check chose, at that check's reserve (1 MiB for every size since #280; 4, 8 or 16 MiB before), so it never
  takes a model size's, the LM's or the
  cascade's room; otherwise the job gives it back and reads with GRAD.
- **Its part of the scratch**: its arena and its 1,684 B result, at most 262,144 + 1,696 = 263,840 B
  (`VJO_VOCR_SPOT_ARENA_MAX`), in the scratch GRAD's and the recognizer's arenas reuse in turn, so a job grows by
  what it exceeds the larger of them: at most 79,440 B with the Normal tier's VOCR6-N (its job's 189,376 B), 1,360 B
  with the Large tier's VOCR6-L (267,456 B) and 138,960 B with the Small tier's VOCR5-S (129,856 B), the largest
  (since v0.4.1-vocr6.2's set: 79,392 B with VOCR6-N's 189,424 B and 139,008 B with VOCR6-S's 129,808 B,
  [above](#vocr6s-follow-up-set-all-three-sizes-vita-vn-ocr-157-no-api-change))
  growth (with the pinned blob, measured by audit-vocr6-spotter2-rubi: Small +111,936 B for a 900×150 region and
  +121,424 B at 960×544, Normal +52,416 / +61,904 B, Large 0). Taken after the memory check passed, in the room it left,
  and before the helpers' line slots (so in a game near its limit it can cost a helper); without that room the job
  reads with GRAD. A try of columns (auto's, after the spotter and auto's check handed the rows over) takes none of it
  and none of the spotter's refinement: it reads without the spotter and keeps a verified block that fits beside it,
  for the next job of rows (`vjo_vocr_job_bytes_spot` of a model of columns is `vjo_vocr_job_bytes`). With
  `ocr_vertical = on` no job takes the block or the scratch: on's job reads without the spotter. The ledger's largest job (Normal with the Large cascade, the 4 MB LM and both helpers)
  is 1,297,488 B with it (`test_ledger.c`; 1,218,048 B without; 1,297,584 / 1,218,192 B with v0.4.1-vocr6.2's
  VOCR6-N, the same pages): the workspace budget and limit rise 1,220,608 → 1,298,432 B, the game
  job's worst case by that and the block, 10,420,224 → 11,014,144 B (with an SC3 charset; 12.2 % of the 90,177,536 B of
  free `USER` #81 measured), and its limit by those and the worker's four text pages (the convention of API 34, #109
  and API 42), 10,604,544 → 11,214,848 B (12.4 %).
- **Its stack**: `vocr_textspot6_find_rows` peaks at 2,712 B (vita-vn-ocr `eval/textspot6_a9_20261026.json`
  "stack": the Vita compiler's `-fcallgraph-info` at `-O2`), on the worker's 32 KiB thread stack (`VJO_STACK_GAME`);
  the worker's whole peak with it is not measured.
- **The worker's code**: vita-vn-ocr's `runtime/vocr_textspot6.c` at the runtime's `-O2` with
  `VOCR_TEXTSPOT6_NO_MS` (the trunk form of the pin), the runtime's `vocr_line_dark` it calls for each box's dark
  flag, `core/vocr_ocr.c`'s finder path, sizes and auto's vertical hand-over, and `game_ocr/main.c`'s block and job
  flow: four text pages more in each build. Data/bss stay in their pages.

Measured with the gate's Vita image (VitaSDK gcc 15.2.0), Release, on the packaged SELF files
(`tools/check_memory_ledger.py`, the build log's module rows), main `3d095b4` (API 53: after VOCR6, #373, #369's
suggested box, #387, #386 and #385; #390 after it changes no module) and this change, both builds;
text + data/bss:

| Module | Debug: main → this change | Normal: main → this change | Loaded pages, debug / normal |
|---|---:|---:|---:|
| Game worker | 163,800 + 36,360 → 179,712 + 36,416 B | 154,884 + 36,344 → 170,780 + 36,408 B | 200,704 → **217,088** / 192,512 → **208,896** B |
| Shell | 98,260 + 77,664 → 98,164 + 77,728 B | 85,980 + 77,488 → 85,900 + 77,552 B | 176,128 / 163,840 B, unchanged |
| Kernel | 43,356 + 77,772 → 43,432 + 77,780 B | 38,204 + 76,084 → 38,280 + 76,092 B | 122,880 / 118,784 B, unchanged |

The worker's code ends 512 B under its last page in the debug build, 1,252 B in the normal one, on main `3d095b4`.
#396's audit follow-up (no spotter with `ocr_vertical = on`, a try of columns without the spotter's part, the `spot`
step across a job's tries) adds 160 B of worker text in the debug build and 144 B in the normal one against main
`5e18b60` (180,048 → 180,208 B and 171,100 → 171,244 B; data/bss, the Shell and the kernel unchanged), the same
pages; the debug worker's code keeps 16 B of its last page. Built with the
spotter's multi-scale paths too (`-DVJO_VOCR_SPOT_NO_MS=OFF`, for a pin of the `ms` form), the worker's text would be
about 11,900 B more, three more pages (measured on the earlier main `796168e`); `trunk`, the pinned form, builds
without them, and `cmake/VocrOCR.cmake` refuses `ms` with them left out.

The Shell parses the two settings (`ocr_finder` in a branch of its own, `ocr_spotter_refine` in `ocr_text_gate`'s;
their keys and choices in the data segment) and sets the two request bits from the config it already holds, which
`same_pipeline` compares. To keep the Shell's code pages, five settings that parsed `off` / `on` by hand
(`japanese_only_selection`, `frequency_badge`, `ocr_skip_blank_lines`, `log_file` and the three `anki_*` switches)
now read one shared choice table through `set_choice`, with the same values and the same warning, and the three
off / on tables became that one (`core/config.c`): the Shell's code is **80 B smaller** than main's in the normal build
and 96 B in the debug build, its data/bss 64 B larger (the finder's names and table, the three `VjoConfig` copies'
two fields, the ledger's item), the same pages; the normal Shell's code keeps 116 B of its last page (36 B on main),
the debug build's 140 B (44 B), and its data/bss 272 B (336 B) and 96 B (160 B). The kernel only accepts the open kind (`VJO_GAME_OCR_OPEN_SPOT`, 19, at the pinned size, for a
request with the spotter's bit) and the two bits: 76 B of code and 8 B of data/bss in each build, the same pages. No thread, static buffer or
other block is added. These are binary
measurements; console boot, memory and timing have not been measured for this change.

**Vertical text ON prefers columns (API 53, #379).** No block, thread or stack is added. A job of columns now asks
the tall-line block (`VjoGameVocrLine`, the same one, freed with the job) for the rectangle around its columns, where it
asked for one column at a time: at most `VJO_VOCR_COLUMNS_BATCH` = `VJO_VOCR_LINE_MAX_BYTES`, 516,096 B, the block's
bound and `VJO_BUDGET_GAME_LINE` as before, so the game's worst case is unchanged; a larger rectangle, or a refused
block, has each column read alone, as before. On the host's measured regions the rectangle was 29-34 KB on drawn boxes
(median; 153 KB at most) and 428 KB on whole screens (516 KB at most), where a column's was at most 57,344 B
([on's rule](vita-vn-ocr.md#ons-rule-api-53-379)). On's rows try first is a job of rows, the size's own blocks, freed
before its columns try takes the vertical model's, as auto's tries do.

The code had no room: `main` `d25c48c` (after VOCR6, #373, #387 and #386) left the debug worker 152 B of its last code
page and the debug Shell 20 B. So two read-only tables move to the data segments (`VJO_DATA_TABLE`), whose last pages had
room: the game worker's pinned model table (`core/vocr_ocr.c` `models`, 480 B) and one debug log line of the Shell's
(52 B), and on's rule is the plain one (126 B; one that allowed a name or an icon beside the lines took 200 B). The
module rows [measured: `tools/check_memory_ledger.py`, both builds, `main` `d25c48c` and this change,
`tools/fast_gate.sh`'s VitaSDK image]:

| module | `main` `d25c48c` | this change | pages |
|---|---:|---:|---:|
| Game worker, debug | 163,688 + 35,880 B | 163,800 + 36,360 B | 200,704 B both |
| Game worker, normal | 154,724 + 35,864 B | 154,884 + 36,344 B | 192,512 B both |
| Shell, debug | 98,284 + 77,608 B | 98,260 + 77,664 B | 176,128 B both |
| Shell, normal | 85,948 + 77,488 B | 85,980 + 77,488 B | 163,840 B both |

The worker's code segment grows by 112 B (debug) and 160 B (normal), the change's code less the table's 480 B moved out;
the debug worker has 40 B of its last code page left and 504 B of its data page, the normal one 764 and 520 B; the debug
Shell 44 and 160 B, the normal Shell 36 and 336 B. None of it is measured on a console.

**#385's audit: the rectangle keeps the job's reserve (AGENTS.md rule 3).** The rectangle around a job's columns was
outside the job's memory check and reserve: `check_memory` counts the workspace, the capture block, the weights, the LM
and the cascade's model, not the tall-line block, and `line_buffer` asked the memory ladder alone. Up to 516,096 B on
a whole screen (measured on the host by #385 and its audit: 428 KB median on Yunohana SpRING!'s, 345 KB on hooked
horizontal ones read with `on`; 29-43 KB on drawn boxes, 153 KB at most), it could cut up to 504 KiB into a job of
columns' reserve (4 MiB then; 1 MiB since #280); with `ocr_skip_blank_lines`
on, whose job reads the weights only at its first line that is not blank, after the columns are fetched, it could
take the weights' room, so that they were refused and the job read the columns' text as rows (find kind 5). Now
`line_buffer` (`game_ocr/main.c`) takes a block larger than one column's bound (`VJO_VOCR_ROTATE_BYTES`, 57,344 B:
every block a job could ask for before #379) only when the game's free USER memory, less the job's reserve
(`free_user_after_reserve`), holds it and the weights the job has still to read where they are not resident, and for
a job of columns its LM's too (`job_model`, `job_lm`, set while the core runs). Refused, the job reads its columns one
by one, each in a block of its own, as before #379. A band of rows that large is left out and counted
(`tall_skipped`), its text not read, so a job of rows keeps no LM's or cascade's room for it (those blocks keep the
reserve themselves, and their refusal costs only their reading). Such bands are not rare on whole screens (#411's
audit, the row finder with the fixes on the hooked pages): a band taller than a strip and over 57,344 B in 27,167 of
50,706 whole screens (53.6 %, up to 522,240 B), holding dialogue (a reference line at least half inside it) in 6,198 of
them, 7,391 of their 92,389 reference lines; in 118 of 50,945 drawn boxes (0.2 %, up to 93,210 B), dialogue in all 118,
222 lines; in 6 of 358 linefind development regions (73,158 B at most). Only a game short of memory refuses one: the
band needs room of its own beyond the job's reserve, and with the blank-line check the weights' room too. Then those
lines are missing from the text, with `off` and auto as with on. A block of a column's size or less is taken as before. The worst
case is unchanged (`VJO_BUDGET_GAME_LINE`); `host/tests/test_game_vocr.c` `columns_rectangle_keeps_the_reserve` holds a
job a byte short of its rectangle's pages beyond its check (the frames' rectangles are under a column's bound, so that
file checks blocks over a page: `VJO_GAME_LINE_UNCHECKED`; it never takes a block under the worker's bound without the
check), with and without the blank-line check: the rectangle refused, the columns read one by one, the text the same
as with room (before: the rectangle taken, and with the check the columns read as rows); and
`columns_rectangle_keeps_the_lm_s_room` keeps the LM's room beside it (#411's audit).

The same change reads a region at least as tall as wide as columns from `on`'s first try, has a region wider than tall
with no row read as rows (nothing), and drops a try of rows' memory checks from the step log when it hands over to
the columns ([vertical text](vita-vn-ocr.md#ons-rule-api-53-379)): no block, thread or stack. The rows [measured:
`tools/check_memory_ledger.py` on both builds, `main` `e4c1a02` and this change, `tools/fast_gate.sh`'s VitaSDK image]:

| module | `main` `e4c1a02` | this change | pages |
|---|---:|---:|---:|
| Game worker, debug | 179,856 + 36,712 B | 180,048 + 36,720 B | 217,088 B both |
| Game worker, normal | 170,924 + 36,704 B | 171,100 + 36,712 B | 208,896 B both |

The worker's code grows by 192 B (debug) and 176 B (normal), its data/bss by 8 B (the two pointers); the debug worker
keeps 176 B of its last code page and 144 B of its data page, the normal one 932 and 152 B. The Shell's and the kernel's
rows are main's. None of it is measured on a console.

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
  new values (29 B) live in the data segment (`VJO_DATA_TABLE`, core/port.h; [the ledger](memory-ledger.md#tables-in-the-data-segment)):
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

## Budget history (include/vjo_ledger_budget.h)

The comments `include/vjo_ledger_budget.h` kept beside the three module page budgets and 6
other items until #418 (cleanup package P5), moved here verbatim as main 6b028b2b had them: each change's
figures, in the order they were written. The header keeps each budget's current figures and their
citation; changes since record theirs in their own section above. "This change", "above" and
"below" are the original comments' words.

### VitaJPOverlay_Shell.suprx (`VJO_BUDGET_SHELL_MODULE_PAGES`)

VitaJPOverlay_Shell.suprx: 102,400 text + 73,728 data/bss. The fixed Kiku note (#90) took 3,488 B
off data/bss (the Anki field mapping's 1,152 B per VjoConfig copy); with #86's region selector and #84 (its --gc-sections link; 28 B of new VjoConfig fields) the module is 214,488 text + 77,876
data/bss; with #87, #88, #92, #85 and #89 it is 215,216 text + 77,980 data/bss [measured, bcd3cf0],
and with the finder refinement 215,256 + 77,984 [measured, fec85af]: 217,088 + 81,920 B of pages;
API 21's model sizes (#108) made it 216,448 + 77,624, 294,912 B of pages [measured, 8f1ec8f], and
API 22 (#93) 216,856 + 77,648, the same pages [measured, ba5f69a].
Lowered from 299,008 B by the removal of jpdb.io and jiten.moe, the online dictionaries: their
clients and the subtitle strip's furigana (only their lookups gave readings) took 6,672 B off the
text segment (.text -5,544 B, .rodata -1,040 B, the import stubs -48 B, alignment) and 6,444 B off
.bss (shell/worker.c -3,340 B: VjoView's strip readings and two VjoConfig copies' API keys;
shell/strip.cpp -2,584 B: its copy of the readings and their widgets; shell/anki.c -512 B: a third
VjoConfig's keys), so the module is 210,184 text + 71,204 data/bss [measured: the build log's
module rows, main ba5f69a and this change], one text page and one data page fewer than main's
294,912 B.
Lowered from 286,720 B by the removal of the Hachidori relay client (core/hachidori.c, its
dictionary branches, hachidori_host and its validation, the relay's error text, the JSON helper
only it used): 3,992 B off the text segment (.text -3,344 B, .rodata -648 B) and 184 B off .bss
(the 64 B hachidori_host of three VjoConfig copies and shell/worker.c's 4 B cached dictionary
ID, 196 B, less 12 B of alignment), so the module is 206,192 text + 71,020 data/bss [measured:
the build log's module rows, main a5d79f2 and this change], one text page fewer: 208,896 +
73,728 B of pages. Lowered again by the removal of AnkiConnect (the Shell's sender and its
requests, the screenshot branch, the Kiku note, the audio URL; the Rubi app exports the cards):
18,192 B of text and 972 B of data/bss, so the module is 188,472 text + 70,104 data/bss [measured:
the build log's module rows, main 5e68ecd (#116, #111: 206,664 + 71,076, the same 282,624 B of
pages) and this change], four text pages fewer: 192,512 + 73,728 B of pages. (On main c4a6ade,
before #111's 424 B of text, it was 188,056 B of text, five pages fewer.) #123's key filter makes it
189,104 text, the same pages [measured: the build log's module row, main c641bdf]. #122's
removal of the default config.ini template makes it 186,416 + 70,112, one text page fewer:
262,144 B packaged (docs/memory-ledger.md). #127's frequency ranks make it 187,552 + 70,128, the
same pages (docs/memory-ledger.md). #124's identification of Adrenaline's PSP games (shell/game.c:
AppMgr's app name and one description read per launch; core/games.c's shared PSP row) makes it
188,000 + 70,132, the same 262,144 B [measured: the build log's module rows, main 9f92bc6 and #124].
Lowered from 266,240 B by the removal of Google Lens (the overlay is offline): its client and
protobuf codec (core/lens.c, core/pb.c), the worker's capture -> JPEG -> Lens job and its
connection warm-up, the Shell's capture and JPEG encoder (shell/capture.c, core/jpegsw.c), the
worker's network pipeline (two 256 KiB arenas, a connection pool, the whole-sentence lookup only
its jobs made) and the engine choice (ocr_backend). Lens was the last of the Shell's TLS
connections once AnkiConnect was removed, so the link no longer keeps BearSSL's TLS, the roots
or the HTTP client either (only SHA-256, the queue's IDs): 87,208 B off the text segment (.text
-66,728 B, .rodata -20,424 B) and 72 B off data/bss, so the module is 100,792 text + 70,060
data/bss [measured: the build log's module rows and arm-vita-eabi-size, main 6020c0b and this
change], 21 text pages fewer than main's packaged 262,144 B: 102,400 + 73,728 B of pages. #140's
posting hints make it 101,024 text, the same pages, and #132's VjoNet stack paint and check
101,488 + 70,076 [measured: the build log's module rows, main 32bfc6a and #132; the same on
3aa08a6, #141]. The network stack's last users (shell/platform_vita.c's sockets, resolver and
cancellation, the UDP log, log_host) went next: 99,268 text + 69,772 data/bss, the same pages, and no
SceNet or SceNetCtl import [measured: the build log's module rows, main 3aa08a6 with #132 and this
change].
API 23's Anki screenshot (anki.c's request to the game worker, the queue's second ID, the game
worker's job slot in worker.c) makes it 99,244 -> 100,484 text and 69,772 -> 69,828 data/bss, the same
pages [measured: the build log's module rows, main c6e0a4d and this change]; the normal build
89,204 -> 90,012 text and 69,700 -> 69,756 data/bss, the same pages, 100 B of text room left (its
normal-build log is a failed screenshot's reason only). The screenshot itself is never in SceShell. #138's left stick over the overlay's text (core/stick_cursor.h, the step along the text,
stick_lookup_ms, the close's check) and core/local_dict.c built for size (vita/CMakeLists.txt) make
it 100,364 -> 98,684 text and 69,828 -> 69,860 data/bss, the same pages [measured: the build log's
module rows, main 74f8de8 and #138]. The cursor across rows with the stick on the arrows' path
(owner, 2026-10-16) makes it 99,196 -> 98,972 text on main a7abad7, the same data and pages
[measured: the build log's module rows, main a7abad7 and this change]. The left stick a character
at a time (owner, 2026-10-20) makes it 98,732 -> 98,812 text on main f482c21, the same data and
pages [measured: the build log's module rows, main f482c21 and this change]. Line history (owner,
2026-10-20; docs/line-history.md) makes it 98,812 -> 100,492 text and 73,272 -> 73,576 data/bss on
main 75041a0, the same pages [measured: the build log's module rows, main 75041a0 and this change].
Yomitan's deinflection rules in a packed table (docs/memory-ledger.md) make it 102,212 -> 101,332 text
and 73,544 -> 73,008 data/bss on main 627a673, the same pages [measured: the same rows, main 627a673
and this change]. #360's job lines after the result and the close line's press-to-screen figure (the debug
build's only; docs/release-variants.md) make it 102,140 -> 102,372 text and 73,408 -> 73,560 data/bss on
main e2c49ee, the same pages (28 and 168 B left), the normal build's plugins byte for byte main's
[measured: the same rows, both builds, main e2c49ee and this change]. The table hints (#360: rank_posting
ranks the lookup tables' words from their postings' bits; the tables, moved to core/lookup_tables.inc, are the
same bytes) make it 102,372 -> 102,388 text on main 2bb09c4, the same data and pages (12 B left) [measured: the
same rows, main 2bb09c4 and this change]. #321's lookups (docs/memory-ledger.md, "Lookups: #321") put the rule
table (deinflect_rules, 4,266 B) in the data segment and its bucket offsets in 130 B (from 514): 102,372 + 73,664
-> 98,284 + 77,608 on main 68041a7, a text page fewer and a data page more, the same 176,128 B (20 and 216 B left)
[measured: the same rows, main 68041a7 and this change]. A game's own line finder (#333: ocr_line_fix.ini read
with games.ini, the spotter or Rubi's finder with its fixes in cfg; the parameters' log line gone, the tables in the
data segment) makes it 98,164 + 77,728 -> 98,212 + 77,792 on main 9ad8eb5, the same pages (92 and 32 B left)
[measured: the same rows, main 9ad8eb5 and this change]. #321's audit (the particle's する check before the depth
test, lookup_pages +24 B) makes it 98,244 + 77,792 on main 099b156, the same pages (60 and 32 B left)
[measured: the same rows, main 099b156 and this change]. #338's audit fixes (shell/history.c's grown-line
rewrite reordered: the file opened before the truncation, the truncation inside the count's odd window and only over
the file's own last record, the end read again after it; the debug build's hook source line in the result's log
batch) make it 98,244 -> 98,276 text on main 70deda7, the same data and pages (28 B left) [measured: the same rows, main
70deda7 and this change].
Lowered from 176,128 B by cleanup P1 (#418): the subtitle strip's text is converted into its own buffer
(shell/strip.cpp, core/styled.c vjo_styled_units), without its 8,480 B styling arena (s_mem_buf): 98,108 + 77,760 ->
98,076 + 69,264 on main 8b18f99, two data pages fewer, 167,936 B (228 B of code room, 368 B of data room) [measured:
the same rows, main 8b18f99 and this change].

The normal build's (its `#else` branch):

The normal build (docs/release-variants.md): the self-test (shell/selftest.c), the lines of the
game worker's step log, of the recognized text and of every job and lookup, and the timings and
traces compiled out: 90,768 text + 69,988 data/bss against the debug build's 100,792 + 70,060,
94,208 + 73,728 B of pages, two text pages fewer [measured: the build log's module rows, both builds
of this tree on main 3805a13 (#130, #142)]. #140's posting hints make it 91,016 + 69,988, and #132's
VjoNet stack paint and check 91,464 + 70,004, the same pages [measured: the build log's module
rows, main 3aa08a6 and #132]. Lowered from 167,936 B by the network stack's removal (its sockets,
resolver, cancellation and UDP log): 89,220 text + 69,700 data/bss, one text page fewer, 90,112 +
73,728 B of pages [measured: the build log's module rows, the normal builds of main 3aa08a6 with #132 and of this change]. #138's left stick (792 B of text; in both builds, its close-line counts
the debug build's) would take it 90,012 -> 90,804 B of text with API 23's screenshot (main 10c8011),
a page past this budget; with core/local_dict.c built for size (2,552 B off, a lookup 0.4-0.6 % more
Cortex-A9 instructions, vita/CMakeLists.txt) it is 88,132 + 69,796 on main 74f8de8 (89,892 + 69,756),
the same pages, 1,980 B of text room [measured: the build log's module rows, the normal builds of
main 10c8011 and 74f8de8 and #138]. The cursor across rows with the stick on the arrows' path makes
it 88,636 -> 88,460 + 69,796 on main a7abad7, 1,652 B of text room [measured: the build log's module
rows, main a7abad7 and this change]. The held log lines of a job (t3-log-path, shell/worker.c
net_main) make it 88,556 + 69,868 on main eece6c6 (88,460 + 69,796), the same pages, 1,556 B of text
room [measured: the same rows, main eece6c6 and t3-log-path]. The left stick a character at a time
(owner, 2026-10-20) makes it 88,348 -> 88,428 + 73,280 on main f482c21, the same pages, 1,684 B of
text room [measured: the same rows, main f482c21 and this change]. Line history (owner, 2026-10-20;
docs/line-history.md, paid in part by core/textfilter.c at -Os and strings in the data segment)
makes it 88,428 -> 90,044 + 73,280 -> 73,584 on main 75041a0, the same pages, 68 B of text room and
144 B of data room [measured: the same rows, main 75041a0 and this change]. Yomitan's deinflection
rules in a packed table (docs/memory-ledger.md) make it 90,084 -> 89,228 + 73,560 -> 73,024 on main
627a673, the same pages, 884 B of text room and 704 B of data room [measured: the same rows, main
627a673 and this change]. API 47's vertical text (the request's vertical field, the model a text was read with by
its lines' column flag, config.ini ocr_vertical's parse; its key, the model's name and one cold message of the
OCR's in the data segment, VJO_DATA_TABLE) makes it 89,252 -> 89,300 + 73,048 -> 73,160 on main 796168e, the same
pages, 812 B of text room [measured: the same rows, main 796168e and this change; the debug build 101,460 ->
101,548 + 73,008 -> 73,152, the same pages]. The table hints (#360) leave it 90,020 + 73,416 on main
2bb09c4, the same text bytes, 92 B of text room [measured: the same rows, main 2bb09c4 and this change]. #321's lookups (above) make it 90,044 + 73,520 -> 85,948 + 77,488 on main 68041a7, a text page fewer and a data
page more, the same 163,840 B, 68 B of text room and 336 B of data room [measured: the same rows, main 68041a7 and
this change]. A game's own line finder (#333, above; the title read into its caller's buffer, the self-test's
notice in the data segment) makes it 85,900 + 77,552 -> 85,940 + 77,696 on main 9ad8eb5, the same pages, 76 B of
text room and 128 B of data room [measured: the same rows, main 9ad8eb5 and this change]. #321's audit (above,
lookup_pages +28 B) makes it 85,972 + 77,696 on main 099b156, 44 B of text room [measured: the same rows, main
099b156 and this change]. #338's audit's history rewrite (above), with the short job line's format moved to the data segment (32 B), makes it
85,972 + 77,696 -> 85,988 + 77,728 on main 70deda7, the same pages, 28 B of text room [measured: the same rows, main 70deda7 and this change]: main's steps reordered, with the end's check and
its second read, which a file replaced since and an SCE_O_APPEND that places only the open need. Its check is exact
(--exact): this must equal its pages. API 55's ocr_pause (#355; docs/game-pause.md) in the Shell is its key's parse,
one speed bit, VjoConfig's 4 B, the active game's STATIC_FB flag and the wait hint's clause (WAIT_HINT_PAUSE, a 54 B
format of the one hint, in the data segment), with core/config.c's off/on settings in one table and three strings
back in read-only data (#358's and API 54's keys, #413's debug-only open format): 85,828 -> 85,764 + 77,672 ->
77,696 on main f32c5e3, the same pages, 252 B of text room and 128 B of data room [measured:
tools/check_memory_ledger.py's module rows, both builds, main f32c5e3 and this change, tools/fast_gate.sh's vitasdk
image; the debug build 98,132 -> 98,108 + 77,760 -> 77,760, the same pages, 196 B and 64 B of room]. Without the
table the pause also fits on efbee95 (85,876 + 77,728 and 98,244 + 77,800 B, after #428 and #440 freed the Shell's
code); on eef0fc4 it took a page more in both builds (86,084 + 77,784 and 98,428 + 77,856 B). The draft's hint as a
second copy (214 B) takes a data page more in both builds on efbee95 too (85,860 + 77,888 and 98,212 + 77,992 B).
Lowered from 163,840 B by cleanup P1 (#418, above: the strip's text into its own buffer, without the 8,480 B arena):
85,764 + 77,696 -> 85,732 + 69,200 on main 8b18f99, two data pages fewer, 155,648 B (284 B of text room, 432 B of
data room) [measured: the same rows, main 8b18f99 and this change].

### VitaJPOverlay_OCR.suprx (`VJO_BUDGET_WORKER_MODULE_PAGES`)

VitaJPOverlay_OCR.suprx: 65,536 text + 8,192 data/bss [measured, API 20 build]. Raised from 45,056 B
(36,864 + 8,192): the in-game capture, its memory ladder, probe and step log run in the worker,
in the game's own USER_RW (91,226,112 B free measured in CLANNAD, docs/vita-memory.md), never in SceShell.
Raised again from 53,248 B (45,056 + 8,192) by one text page for API 20's job policy: the
stable-text wait and the blank-line check (core/vocr_ocr.c), and by one more for the text
normalisation (core/textnorm.c, -Os), which runs here so that SceShell's pages stay as they are.
Raised again from 61,440 B by two text pages for the beam + LM decoder: vita-vn-ocr's
runtime/vocr_lm.c (the decoder and the LM scorer) and vocr.c's top-K extras, with their
planning and per-line confidence in core/vocr_ocr.c, and the confidence cascade
(59,692 B text measured at bcd3cf0, core/vocr_ocr.c built for size); and by one more for the finder
refinement (vita-vn-ocr #83's runtime/vocr_lines_refine.c, -O2 as the runtime is built; 65,084 B
text measured at fec85af).
The game's partition has the room (90,177,536 B free USER the least #81 saw).

Raised again from 73,728 B for API 22 (parallel lines, the line cache, the clock) and the runtime
pin, vita-vn-ocr #85's faster W8I kernels: one text page and two data/bss pages for API 22 (text
62,012 -> 67,452 B: the scheduler, the helper threads, the clock, the cache; data/bss 5,840 ->
14,200 B: the 6,160 B VjoVocrCache, eight lines each with its shown and greedy text, and the
2,080 B line queue and slots, both on vita-vn-ocr a540027), and one text page for #85 (the build's
vocr.c object 15,288 -> 17,952 B of code; the worker's text 67,452 -> 70,124 B) [measured: main
8f1ec8f against the API 22 build], in the same game partition. #138's one-copy check (game_ocr/main.c
another_copy, outside_segments) adds 284 B of text in both builds, the same pages: 71,088 -> 71,372 B
debug, 69,724 -> 70,008 B normal [measured: the build log's module rows, main c08db18 and #138].

Both builds (docs/release-variants.md): the normal build compiles the self-test's capture probe
out, 1,348 B of text, in the same pages since #124 (the PSP bubbles' ID: +260 B): 69,724 + 14,208
against the debug build's 71,072 + 14,208 [measured: the build log's module rows, both builds on
main 6020c0b; on 9f92bc6 the normal worker was 69,464 B, a text page fewer]. The rest of the
worker's debug-only code is the step log, whose tier checks and clock step the normal Shell's
short log reads.
Raised from 90,112 B by one text page for API 23, the Anki screenshot, in both builds: its JPEG
encoder (core/jpegsw.c, 4,038 B at -O2, and core/arena.c, 518 B), the shot job and its capture
(game_ocr/main.c run_shot) take the debug build's text 71,372 -> 77,004 B and its data/bss
14,208 -> 14,160 B (the request's model directory 256 -> 128 B), the normal build's 70,008 -> 75,688 B
and 14,208 -> 14,160 B [measured: the build log's module rows, main dcb7b31 and this change, both
builds], in the game's partition, never SceShell's: the screenshot is encoded where OCR runs
(AGENTS.md rule 1).
Raised from 94,208 B by one text page for #194, a dialogue box's short last line
(core/vocr_ocr.c probe_short_lines): the debug build's text 77,196 -> 78,076 B, 252 B past
the page, data/bss 14,160 B as before [measured: the build log's module rows, main 49310ed and
its parent], in the game's partition, never SceShell's. The normal build's text still fits the
page it had: 94,208 B [CI's vita (normal) build log, main b622c18], so the budget is per build.
Raised from 94,208 B in the normal build by one text page for API 26, the text-presence gate (vita-vn-ocr #100:
runtime/vocr_textgate.c at the runtime's -O2, the code its time estimate is for, and core/vocr_ocr.c's use of
it; its 1,540 B of int8 weights are a file the kernel broker opens, VJO_GAME_OCR_OPEN_GATE, kept in a static
VjoVocrGateResident): 3,488 B of text and 1,560 B of .bss in both builds, whether the gate is on or off. The
debug build's text 78,156 -> 81,644 B and data/bss 14,160 -> 15,720 B fit the pages it had, 98,304 B; the normal
build's text 76,856 -> 80,344 B (968 B of room before it) takes one more, and its data/bss 14,160 -> 15,720 B
fit: 94,208 -> 98,304 B [measured: the build log's module rows, both builds, main 1de7b67 and this change]. At
-Os the gate's code is 564 B smaller (2,072 against 2,636 B, vita-vn-ocr eval/textgate_sizes_20261018.json),
which would still need the page. In the game's partition, never SceShell's. Both builds now have the same
budget.
Raised for API 31, the native text hooks (docs/native-hooks.md), in the game's partition only (the worker
never loads into SceShell, AGENTS.md rules 1-2), in every game that is ON whatever its text source, and now
per build again: the engine (game_ocr/hooks.c and hooks_observer.S, core/hook_place.c, core/hook_stream.c,
core/sfo.c, at -Os) and what it links of Luna's ported table (core/hook_luna.c and its table: the sites, the
version gate and the capture's hookfuncs) take the debug build's text 81,644 -> 99,016 B and the normal
build's 80,344 -> 96,524 B (1,780 B of room in its last page), and both builds' data/bss 15,752 -> 27,496 B: debug 98,304 -> 131,072 B, normal
98,304 -> 126,976 B [measured: the build log's module rows, both builds, hooks/engine 8aa79eb with origin/hooks/
luna-port merged; the engine alone was 88,904 / 86,396 B of text; the debug "before" is the same
tree with the engine unreferenced (81,644 + 15,752, as main's), the normal "before" this header's API 26
figure]. The data is the 20 stream slots' static storage (VJO_HOOK_STREAMS x 548 B = 10,960 B: a stream's
512 raw bytes and its header, include/vjo_game_ocr.h) and a site hit's 512-byte capture buffer: static, so
a hook in a game thread never allocates. Not in these figures: the job path's decoder (core/hook_text.c,
its CP932 table), Luna's filters (core/hook_re.c) and the selection (core/hook_select.c), linked when the
job path calls vjo_hooks_text (hooks-select); they add their own pages then.
Raised again for API 31's job path (docs/native-hooks.md: which text a job shows, OCR's or a native hook
stream's), which decodes the streams' texts (vjo_hooks_text) and so links the decoder and its CP932 table
(core/hook_text.c, 12,958 B of text and rodata) and Luna's filters (their part of core/hook_luna.c, and
core/hook_re.c, 3,407 B at -Os), and adds the selection (core/hook_select.c, 1,708 B at -Os, and the job
path in game_ocr/main.c) [object sizes, arm-vita-eabi-gcc at the worker's flags]; its static snapshot of
the streams (one stream's copy, 548 B, the texts as the job began, 2,048 B, and the candidates, 400 B) is
3,064 B more .bss, and the comparison's 2,562 B of arrays are on the worker's stack only while it compares.
The debug build's text 99,016 -> 127,144 B, the normal build's 96,524 -> 124,668 B, both builds' data/bss
27,496 -> 30,560 B: debug 131,072 -> 163,840 B, normal 126,976 -> 159,744 B [measured: the build log's
module rows, both builds, hooks/select 4f9dca9 with hooks/engine dca6d5f merged]. The selection alone,
before the engine (a stand-in for game_ocr/hooks.c), took 98,304 -> 106,496 B in both builds: text 81,644
-> 84,588 B debug, 80,344 -> 83,336 B normal, data/bss 15,752 -> 18,816 B [measured, hooks/select
7fd2cb8].
Raised in the debug build only, 163,840 -> 167,936 B, by one text page for the read-only PSP probe
(docs/native-hooks.md "PSP probe (debug build)": game_ocr/psp_probe.c, 881 B of text, and core/psp_probe.c,
5,345 B of text and rodata with Luna's PSP table's 3,515 B, both at -Os, and main.c's call): the debug build's
text 127,144 -> 133,392 B, 2,320 B past its page; its data/bss 30,560 -> 30,568 B (two words) fit the pages it
had [measured: the build log's module rows, debug build, hooks/psp-probe with feat/native-hooks 960ac20 merged
against 960ac20; the objects' sizes: arm-vita-eabi-size]. No block, no thread. The normal build has none of
it: its worker is byte for byte its parent's. In the game's partition only.

Standalone PR #235 (de5f7e0, based on main 75041a0), API 32: the Anki screenshot's black bars
(core/shottrim.c at -Os, 612 B of text, and game_ocr/main.c shot_pass,
the passes that guess, check and read again) take the debug build's text 81,644 -> 82,876 B, 956 B past the 81,920 B
of its text pages, and the normal build's 80,344 -> 81,608 B, which still fits (312 B left); data/bss 15,720 B as
before in both [measured: the build log's module rows, both builds, main 75041a0 (the worker's the same on f482c21
and 44d4e8d) and PR #235 fb6ed5d (de5f7e0 changes docs only), CI's container]. So the debug
build, the test builds' own and the -debug download, takes one more text page in each game that is ON, 98,304 ->
102,400 B, and the normal build, the main download, keeps 98,304 B. Already cut to get there: the scan at -Os
(816 B at -O2) and no row buffer for black rows above the strip. Kept: the one-colour rule, which keeps a dark Vita
picture's edges from being guessed as bars and read twice (24 of 1,727 frames without it, 1 with it [measured, host,
on local copies of the owner's frames, docs/anki.md "Black bars"]), and the one-compare path for runs of the bars'
colour, which pays back most of what the rule's colour check costs: every row of a PSP game's 1x frame scanned in
3.55 million Cortex-A9 instructions, 5.11 million with the check and no such path, 2.75 million with neither (the
rule out) [measured: qemu-arm's insn_mix plugin, the Vita compiler's objects; the last by audit-blackbars]. In the
game's partition, never SceShell's.

Combined enhancement build, API 33: native hooks and screenshot trimming retain the original
native-hook page budgets. Move the immutable 1820-byte Luna title-ID table into existing data-page
room (VJO_DATA_TABLE); no new allocation or per-pixel work. Measured with VitaSDK GCC 15.2.0:
debug segments 134120+32576 B, normal 125172+32568 B; rounded 167936/159744 B
(build/combined-{debug,normal}-ledger.json). Shell budgets and PAF reserve remain unchanged.

API 34, remembered hooks: one extra text page for adaptive selection/cache/build hashing and one
data page for stable source identity and rollback state. No new block or thread; the cache blob
uses the existing worker stack. Measured packaged SELF, VitaSDK GCC 15.2.0 Release:
debug segments 137088+32888 B ->176128 B, normal 128220+32880 B ->167936 B,
local build/vita-minimal and build/vita-minimal-normal measure-ledger.json reports.
These are binary measurements, not console pool or boot measurements.

vita-vn-ocr #109, counters (ルッ月 -> 九ヶ月): one text page more in both builds, in the game's partition
only. The counter re-decode (vita-vn-ocr's runtime/vocr_post.c, built -Os: vocr_post_redecode 1,770 B,
vocr_post_greedy 216 B and their helpers 446 B, as linked), its calls before the LM and in the cascade
(core/vocr_ocr.c counter_redecode and finish_line: +272 B), the counter rule in the text normalisation
(core/textnorm.c fold_forms and its numeral table: +194 B) and the worker's policy and cache bit
(game_ocr/main.c: +40 B, +44 B normal), 2,938 B of text in all [the linked ELFs' symbols, both builds],
take the debug build's text 137,088 -> 140,032 B and the normal build's 128,220 -> 131,164 B;
data/bss as before (32,888 / 32,880 B) [measured: the build log's module rows, both builds, main e33f8f6
(whose worker is ae9b02d's) and this change, VitaSDK GCC 15.2.0 in vitasdk/vitasdk:2026.08; the kernel and
Shell SELF files are byte-identical to main's]. The normal build's code ended 1,112 B
below 128 KiB, less than the SCE data vita-elf-create appends (1,204 B): the worker's link now keeps
__sce_headroom (vita/CMakeLists.txt), as the Rubi app's does. No new block, thread or allocation.

PR #292, Rubi's own filters for its verified entries (core/hook_luna.c's VJO_RF_* and vjo_luna_text_filter,
the entries' filter and deref fields, game_ocr/hooks.c): +1,616 B of text in both builds took the debug
build's text 141,752 -> 143,368 B, 8 B past its 143,360 B of pages (the normal build's 132,996 -> 134,612 B
still fit) [measured: the build log's module rows, both builds, main bd130a8 and the PR's acddaaf, CI's
container]. The filters' ops arrays and their index, 776 B of tables [arm-vita-eabi-nm -S], are kept in the
data segment instead (VJO_DATA_TABLE, core/port.h), whose last page had room (3,880 B debug, 3,888 B
normal): text 142,600 B debug (760 B of room) and 133,828 B normal (1,340 B), data/bss 32,984 -> 33,760 B
debug and 32,976 -> 33,752 B normal, the same pages, 180,224 and 172,032 B [measured: the build log's
module rows, both builds, the PR rebased on main bd130a8 with this change, vitasdk/vitasdk:2026.08's gcc
15.2.0]. The kernel and Shell SELF files are byte-identical to main's. No block, thread or allocation.

PRs #293-#296, 26 verified entries from the psandvita Vita3K farm (core/hook_place.c): each 40 B row and
its title string took the debug build's worker text 142,600 -> 143,896 B in their code segment, 536 B past
its 143,360 B of pages: 184,320 B against this budget (#293 142,744, #294 142,904, #295 143,352 B still
fit; the normal build's 133,828 -> 135,124 B still fit) [measured: the build log's module rows, both builds
of each PR rebased on main cf5e4c5, vitasdk/vitasdk:2026.08's gcc 15.2.0]. Rubi's verified table (28 rows,
1,120 B [arm-vita-eabi-nm -S]) is kept in the data segment instead (VJO_DATA_TABLE, core/port.h), whose last
page had room (3,104 B debug, 3,112 B normal): text 142,776 B debug (584 B of room) and 134,004 B normal
(1,164 B), data/bss 33,760 -> 34,880 B debug and 33,752 -> 34,872 B normal, the same pages, 180,224 and
172,032 B [measured: the build log's module rows, both builds, #296 rebased on main cf5e4c5 with this
change]. Its title strings stay read-only. The kernel, Shell, RCO and Rubi app files are byte-identical to
main's CI build. No block, thread or allocation.

API 42, vita-vn-ocr #120's head gate and #122's two-level head (docs/vita-vn-ocr.md "Head gate and two-level
head"): two text pages more in both builds, in the game's partition only. The runtime's code from main's pin to
this one (vita-vn-ocr 817d203 -> 47cf3a3: vocr.o 18,218 -> 23,255 B, VitaSDK -O2 NEON) and the job's (the re-read,
the paths' options, the recognize step's share: core/vocr_ocr.c, game_ocr/main.c, +627 B in the normal build) take
the debug build's text 142,776 -> 148,424 B, 968 B past 147,456 B, and the normal build's 134,004 -> 139,668 B,
404 B past 139,264 B; data/bss 34,880 -> 34,960 B and 34,872 -> 34,952 B, within their pages [measured: the
build log's module rows, both builds, main 4bc2604 (byte for byte CI's) and this change, vitasdk/vitasdk:2026.08's
gcc 15.2.0]. So each game that is ON holds 180,224 -> 188,416 B (debug) and 172,032 -> 180,224 B (normal, the main
download). Kept: an estimated 0.12 / 0.20 / 0.26 s less per 3-line box at 333 MHz (Small / Normal / Large).

rubi-bucket-hooks, two more verified entries from the farm's data bucket (PCSG00628, PCSG00380), their two
filters, the page-record read (VJO_PLACE_READ_CELLS, vjo_place_cells), the entries' APP_VER gate, CP932 0xA0:
worker text 148,424 -> 148,792 B debug (2,760 B of room left) and 139,668 -> 140,004 B normal (3,356 B),
data/bss 34,960 -> 35,288 B debug and 34,952 -> 35,280 B normal (+328 B: the verified table 1,120 -> 1,200 B,
the new filters' ops 132 + 84 B, PROTOTYPE's and PCSG00540's one more op each, 12 + 12 B, the filters' index
56 -> 64 B [arm-vita-eabi-nm -S]), the same pages, 188,416 and 180,224 B [measured: the build log's module
rows, both builds, main 0b1a45b and this change, tools/fast_gate.sh's vitasdk image]. The kernel and Shell SELF
files are byte-identical to main's; the Rubi app's eboot.bin, which links core/hook_place.c for the Games tag,
is 120 B (debug) and 79 B (normal) larger. The page-record read is two exact reads of game memory into the
existing capture buffer: no block, thread or allocation.

API 43, trusted hooks (core/hook_select.c vjo_hook_select_trusted, game_ocr/main.c's branch, its run counter and
the warm-up's condition): debug text 148,792 -> 149,208 B (2,344 B of room left) and data/bss 35,288 -> 35,312 B,
normal 140,004 -> 140,436 B and 35,280 -> 35,304 B, the same pages [measured: the build log's module rows, both
builds, main 23921ad and this change, tools/fast_gate.sh's vitasdk image]. No block, thread or allocation.

API 47, vertical text (config.ini ocr_vertical off, on or auto; vita-vn-ocr #99 and #103, closed draft #223's column
job): one text page more in both builds, in the game's partition only, in every game that is ON whatever the
setting. vita-vn-ocr's column finder and rotation (runtime/vocr_vertical.c, built -Os), the dark flag the job
computes on a column's pixels (vocr_lines.c's vocr_line_dark, linked now), the job's column path, VOCR5-V's model
entry, auto's rule (core/vocr_ocr.c vjo_vocr_rows_vertical) and the worker's request and retry (game_ocr/main.c):
on main 796168e the debug build's text 149,368 -> 155,032 B (616 B of room), 188,416 -> 192,512 B of pages, the
normal build's 140,596 -> 146,116 B (1,340 B), 180,224 -> 184,320 B; data/bss unchanged (35,312 and 35,304 B)
[measured: the build log's module rows, both builds, main 796168e (CI's Release run 40407403783) and this change,
tools/fast_gate.sh's vitasdk image]. A job of columns allocates only what a job allocates (its workspace holds the
57,344 B rotation buffer: test_ledger.c), none when ocr_vertical is off. The kernel and the Shell keep their
pages. No block, thread or allocation.

API 49, trusted hooks without waiting (game_ocr/main.c hooks_watch's trusted reports and pace, hook_publish):
debug text 155,032 -> 155,576 B (72 B of room left) and data/bss 35,312 -> 35,328 B, normal 146,116 ->
146,676 B and 35,304 -> 35,320 B, the same pages [measured: the build log's module rows, both builds, main
b6c715c and this change, tools/fast_gate.sh's vitasdk image]. No block, thread or allocation: the watch reads a
trusted line's text into the hook pool, which no job uses between jobs.

API 50, vita-vn-ocr #129's line finder fixes and the pin at its merge (f8a1856, which also brings #130's vocr.c):
one text page more in both builds, in the game's partition only. The fixes (runtime/vocr_lines_fix.c in place of
vocr_lines_rows.c, the policy's flags, the request's bits) were +1,264 B of debug text and +1,280 B of normal text
[measured: the build log's module rows, both builds, main 0b1a45b and this change before the pin, 601a133]. #130's
vocr.c, a light attention op that no pinned blob has, is vocr.o 23,255 -> 25,963 B (VitaSDK -O2 NEON, the
build's flags; vocr_run +984 B, gate_tile 806 B, vocr_load +442 B, plan +268 B by symbol). Together they take
two more text pages each on top of API 49's: 192,512 -> 200,704 B (debug) and 184,320 -> 192,512 B (normal, the main
download) in each game that is ON. Measured: debug 155,576 -> 160,648 B of text, normal 146,676 -> 151,748 B,
data/bss 35,328 / 35,320 B as before; 3,192 / 3,900 B of text room left [measured: the build log's module rows,
both builds, main 3388f7a and this change at b74669b (merged as 0994afc), tools/fast_gate.sh's vitasdk image]. No
block, thread or allocation.

API 47's audit fixes (auto's tries share the wait and report the last try's model, the step log's room): debug
text 160,648 -> 160,888 B (2,952 B of room left), normal 151,748 -> 151,988 B (3,660 B), the same pages [measured:
the build log's module rows, both builds, the fixes on main 241e09f, whose plugins main 16fa362 keeps; main's rows
as above]. No block, thread or allocation.

PCSG00020, one more verified entry and its filter (core/hook_place.c, core/hook_luna.c r_PCSG00020): worker text
unchanged, rodata +32 B, data +68 B, both builds, the same pages [measured: arm-vita-eabi-size -A on both builds'
VitaJPOverlay_OCR, main 892cfb9 (#354's build, whose worker differs only in VJO_API_VERSION) and this change,
tools/fast_gate.sh's vitasdk image]. No block, thread or allocation.

PCSG00700, one more verified entry and its filter: worker text unchanged, rodata +64 B (the entry's 40 B, the
filter's patterns), data +104 B (the filter's ops, the filters' index 64 -> 68 B), both builds, the same pages
[measured: arm-vita-eabi-size, both builds, main cbaa4e3 and this change; rebased on main 892cfb9, VitaSDK 2026.08]. No block, thread or
allocation.

PCSG00883, one more verified entry (core/hook_place.c; it reuses RF_PCSG00020): worker text unchanged, rodata +16 B
(debug) and +0 B (normal), data +40 B, the same pages [measured: arm-vita-eabi-size -A on both builds'
VitaJPOverlay_OCR, #356's head 2ea1809 and this change, tools/fast_gate.sh's vitasdk image]. No block, thread or
allocation.

MAGES' SC3 text (docs/native-hooks.md "SC3 text": core/hook_sc3.c at -Os, the SC3 read and decode in
game_ocr/hooks.c, the charset's load in game_ocr/main.c, three verified entries and three charset pins in the data
segment): debug text 161,016 -> 162,232 B (1,608 B of room left) and data/bss 35,328 -> 35,624 B, normal 152,084 ->
153,268 B (2,380 B) and 35,320 -> 35,608 B, the same pages [measured: the build log's module rows, both builds, main
199fe31 and this change, tools/fast_gate.sh's vitasdk image]. Its one block, VjoGameSc3, is in the game pool
below. Merged on main (its parent 19986c7 -> e2c49ee): debug 161,272 -> 162,472 B of text (1,368 B of room left)
and 35,584 -> 35,872 B of data/bss, normal 152,340 -> 153,508 B (2,140 B) and 35,576 -> 35,856 B [measured: the
same rows, both builds of both revisions, #368's audit's follow-up].

#324's audit (the retry after refused weights in API 47's retry block: one copy of the tries' bookkeeping):
debug text 163,528 -> 163,512 B (328 B of room left), normal 154,548 B unchanged (1,100 B), the same pages
[measured: the build log's module rows, both builds, main 84dbad9 and this change, tools/fast_gate.sh's vitasdk
image]. No block, thread or allocation.

VOCR6's Normal and Large (vita-vn-ocr #125) and VOCR6-N's settings for the 2000k LM (core/vocr_ocr.c lm_retuned,
vjo_vocr_lm_settings, the job model passed to both decoders' lm_opts): worker text 163,512 -> 163,640 B debug (200 B
of room left) and 154,548 -> 154,676 B normal (972 B), data/bss unchanged (35,880 / 35,864 B), the same pages; the
kernel's and the Shell's rows unchanged [measured: tools/check_memory_ledger.py, main 1c9d048's Release run 40546045095
artifacts against this change's fast-gate builds, VitaSDK 2026.08]. No block, thread or allocation.

SC3 text, #368's audit (a timed-out charset read tried again, a failed close kept as the job's error, a block not
given back freed before another is taken: game_ocr/main.c hooks_sc3_load, sc3_release, hooks_cache_prepare): worker
text 163,656 -> 163,688 B debug (152 B of room left) and 154,692 -> 154,724 B normal (924 B), data/bss unchanged,
the same pages; the kernel's and the Shell's rows unchanged [measured: the build log's module rows, both builds,
main 68041a7 and this change, tools/fast_gate.sh's vitasdk image]. No new block, thread or allocation.

API 53 (#379, vertical text ON prefers columns; a job of columns reads its columns' rows once): the worker's code
segment 163,688 -> 163,800 B (debug: 40 B of the last page left) and 154,724 -> 154,884 B (normal: 764 B), its data
segment 35,880 -> 36,360 B and 35,864 -> 36,344 B (core/vocr_ocr.c's pinned model table, 480 B, moved there:
VJO_DATA_TABLE), the same pages; the Shell's 98,284 + 77,608 -> 98,260 + 77,664 B (debug: one debug log line, 52
B, moved to its data segment) and 85,948 -> 85,980 B (normal), the same pages [measured: tools/check_memory_ledger.py
on both builds, main d25c48c (the worker's rows are main 313666a's too) and this change, tools/fast_gate.sh's vitasdk
image]. No block, thread or allocation: the
columns' rectangle is the tall-line block (VJO_BUDGET_GAME_LINE, its bound unchanged).

API 54, the text spotter as the finder (ocr_finder = spotter, the default; docs/memory-ledger.md, "The spotter
finder"): vita-vn-ocr's runtime/vocr_textspot6.c at the runtime's -O2, built with VOCR_TEXTSPOT6_NO_MS (only the
trunk form the pin uses; cmake/VocrOCR.cmake VJO_VOCR_SPOT_NO_MS), with the vocr_line_dark it calls, and its use
(core/vocr_ocr.c, game_ocr/main.c's block and job flow) in the game's partition only: four text pages more in each
build. Debug text 163,800 -> 179,712 B and data/bss 36,360 -> 36,416 B, 200,704 -> 217,088 B of pages; normal
154,884 -> 170,780 B and 36,344 -> 36,408 B, 192,512 -> 208,896 B (512 / 1,252 B of text room left) [measured:
the build log's module rows, both builds, main 3d095b4 (after VOCR6, #373, #369, #387, #386 and #385; #390 after
it changes no module) and this change, tools/fast_gate.sh's vitasdk image].
Its weights are a block of their own (VJO_BUDGET_GAME_SPOT), never static; data/bss keep their pages.

#396's audit (no spotter with ocr_vertical = on; a try of columns keeps the spotter's block without its scratch or
refinement; the job's spot step across its tries): worker text 180,048 -> 180,208 B debug (16 B of the last page
left) and 171,100 -> 171,244 B normal (788 B), data/bss unchanged, the same pages; the kernel's and the Shell's rows
unchanged [measured: the build log's module rows, both builds, main 5e18b60 and this change, tools/fast_gate.sh's
vitasdk image]. No block, thread or allocation.

#316's audit fixes (PCSG00628's !X escapes and u9829. as 15 more ops of its filter and their 109 B of strings, in the
data segment; CP932 0xA0 decoded only in VJO_HOOK_ENC_CP932_A0, the two MSGSET entries' encoding, and vjo_hook_clean's
U+F8F0 drop gone; an r15 entry only with segment 0 at 0x81000000): worker text 179,712 B debug unchanged (512 B of
room left) and 170,780 -> 170,764 B normal (1,268 B), data/bss 36,416 -> 36,704 B and 36,408 -> 36,696 B
(r_PCSG00628 132 -> 312 B, r628_esc 109 B [arm-vita-eabi-nm -S]), the same pages [measured: the build log's module
rows, both builds, main 9ad8eb5 and this change, tools/fast_gate.sh's vitasdk image]. On main 3d095b4, with 40 B
of the debug worker's last text page left, its first form took 96 B, a page past this budget: the decoder's flag as
a parameter cost 48 B there, as an encoding the two entries name with a select in its 0xA0 branch 14 B (+16 B in
all). The kernel and Shell SELF files are byte-identical to main's. No block, thread or allocation.

#382's and #387's audit (the step log keeps the FREE step of a block a retried try gave back for good:
game_ocr/main.c steps_drop_blocks): worker text 179,712 -> 179,760 B debug (464 B of room left) and 170,764 ->
170,812 B normal (1,220 B), data/bss unchanged, the same pages; the kernel's and the Shell's rows unchanged
[measured: the build log's module rows, both builds, main b88772c and this change, tools/fast_gate.sh's vitasdk
image]. No block, thread or allocation.

#338's audit fixes (the cache's trust bit, VjoHookLearned.trusted; vjo_hook_select_learned's trust argument, the
interval saved as it is): worker text 179,760 -> 179,840 B debug (384 B of room left) and 170,828 -> 170,876 B
normal (1,156 B), data/bss 36,704 -> 36,712 B and 36,696 -> 36,704 B, the same pages [measured: the build log's
module rows, both builds, main 70deda7 and this change, tools/fast_gate.sh's vitasdk image]. No block, thread or
allocation; the cache's record keeps its 96 B.

The audit of #397 and #401 (core/hook_luna.c run_ops: an op gated by encoding reads VJO_HOOK_ENC_CP932_A0 as CP932):
every module's row unchanged (worker text 179,856 B debug, 368 B of room left, and 170,924 B normal, 1,108 B), the
kernel and Shell SELF files byte-identical to main's [measured: the build log's module rows, both builds, main
2eb2d98 and this change, tools/fast_gate.sh's vitasdk image; on main cc25de3 it was -16 B debug, +16 B normal]. No
block, thread or allocation.

#385's audit fixes (game_ocr/main.c: on reads a tall region's columns at once; a tall-line block over a column's
bound keeps the job's reserve and its weights' room, a job of columns' its LM's too, job_model/job_lm; a hand-over
try's checks leave the step log; core/vocr_ocr.c: no row in a wide region is off's job): worker text 179,856 ->
180,048 B debug (176 B of room left) and 170,924 -> 171,100 B normal (932 B), data/bss 36,712 -> 36,720 B and
36,704 -> 36,712 B, the same
pages; the kernel's and the Shell's rows unchanged [measured: tools/check_memory_ledger.py, both builds, main
e4c1a02 and this change, tools/fast_gate.sh's vitasdk image]. No block, thread or allocation: the rectangle is the
tall-line block (VJO_BUDGET_GAME_LINE, its bound unchanged).

VOCR6's follow-up set (vita-vn-ocr #157, v0.4.1-vocr6.2: VOCR6-S, VOCR6-N and VOCR6-L as the three sizes; the
pins' names, sizes, SHA-256 and arenas, no code path): worker text 180,208 B debug unchanged (16 B of room left) and
171,244 -> 171,260 B normal (772 B), data/bss unchanged (36,720 / 36,712 B); the Shell's 98,276 -> 98,284 B debug and
85,988 -> 85,996 B normal (the sizes' file names, 4 B longer in all; 20 B of the normal build's last text page left);
the kernel's 43,508 -> 43,496 B and 38,356 -> 38,344 B; every module the same pages [measured:
tools/check_memory_ledger.py, main b55c5f5's Release run 40597389507 artifacts against this change's fast-gate builds,
tools/fast_gate.sh's vitasdk image]. No block, thread or allocation.

The psandvita hook farm's 20 verified entries of 2026-10-27 and their filters (#394: core/hook_place.c; core/hook_luna.c
PROTOTYPE_NAMELINE, PCSG00472, AOZORA_RUBY, CARET_NEWLINE, PCSG01250 and one more PROTOTYPE op): data, 1,288 B in
the normal worker (.rodata +304 B, most of it the 20 titles' strings: Luna's ID table is a char[][10], which shares
none; .data +976 B: the 20 rows' 800 B, the new ops, the filters' index 76 -> 96 B), against 788 + 152 = 940 B of
room in the last pages of its code and data segments on main b55c5f5 (772 + 152 B on 665479d). As #394 wrote them
(all in the data segment) the worker took one page more in the normal build and two in the debug build on b55c5f5
(171,548 + 37,688 B, 212,992 B; 180,496 + 37,696 B, 225,280 B). So the filters' index, PCSG00628's ops and escapes (#316's audit) and the five arrays
the batch adds or grows, 853 B [arm-vita-eabi-nm -S], are read-only data again, and core/hook_luna.c is built for
size as core/hook_re.c is (vita/CMakeLists.txt: the filters' ops and Luna sites' reads, control code; its object's
text 5,612 -> 4,168 B and rodata 13,595 -> 13,347 B, normal build): both builds keep their pages. Normal text
171,260 -> 170,796 B (1,236 B of room) and data/bss 36,712 -> 36,832 B (32 B); debug text 180,208 -> 179,760 B
(464 B) and data/bss 36,720 -> 36,840 B (24 B) [measured: tools/check_memory_ledger.py's module rows and
arm-vita-eabi-size -A, both builds, main 665479d and this change, tools/fast_gate.sh's vitasdk image; on b55c5f5
16 B less normal text and debug text 179,744 B]. The kernel's and the Shell's rows unchanged. No block, thread or
allocation. The data segment's last page then has 32 B (normal) and 24 B (debug) left: the next verified row (40 B of
verified[], VJO_DATA_TABLE) takes a page in both builds unless a table moves to read-only data first (code room
1,236 / 464 B) or the budget is raised (audit-rubi-merge-394 N5).

API 55, the pause for the frame (#355; ocr_pause, off by default; docs/game-pause.md, docs/memory-ledger.md "The
pause for the frame": game_ocr/main.c pause_copy, the frame block's memory check, ladder, release and step, and
strip_source's frozen reads; core/vocr_ocr.c's frozen region): 1,280 B (debug) and 1,232 B (normal) of code and
64 B of statics, against 48 B and 772 B of the last code page and 24 B and 32 B of the last data page left on main
efbee95 (v0.10.0). So this budget is raised by one page, 4,096 B, in both builds (217,088 -> 221,184 B debug,
208,896 -> 212,992 B normal): the code takes one page more, and the pinned model table (core/vocr_ocr.c models,
480 B) is read-only data again, in that page, so the data segment keeps its pages with room for the statics and, at
last, for more verified hook rows (40 B each). Debug 180,176 -> 181,936 B of text (2,384 B of room) and 36,840 ->
36,424 B of data/bss (440 B), normal 171,260 -> 172,972 B (3,156 B) and 36,832 -> 36,416 B (448 B) [measured:
tools/check_memory_ledger.py's module rows, both builds, main efbee95 and this change, tools/fast_gate.sh's vitasdk
image]. Its one block, VjoGameVocrFrame, is VJO_BUDGET_GAME_FRAME's (counted as the tall line's ledger item, which
it replaces); no thread. The page is in every game that is ON, whether or not ocr_pause is: one page of the game's
memory, 0.005 % of the 90,177,536 B free USER the least any job saw in CLANNAD [measured, #81's v0.8.0-test.1
log].

#418 (cleanup P4, #455): one resident-block allocator, one file loader and one broker poll loop in the worker:
debug 181,936 -> 180,544 B of text (data/bss 36,424 B unchanged), the same pages; normal 172,972 -> 171,596 B and
36,416 -> 36,408 B, under its last code page again: 212,992 -> 208,896 B of pages, the page #355 took (436 B of code
room left in the normal build, 3,776 B in the debug one) [measured: tools/check_memory_ledger.py's module rows,
both builds, main 93c99c7e and this change, tools/fast_gate.sh's vitasdk image]. No block, thread or allocation.

#418 (cleanup P4, #471): the text-presence gate's weights a block of their own (VJO_BUDGET_GAME_GATE, 1,560 B of
.bss before) and the line cache's greedy texts inside its entries' text (core/vocr_ocr.h VJO_VOCR_CACHE_TEXT:
2,944 B of .bss before): worker data/bss 36,424 -> 31,968 B debug and 36,408 -> 31,952 B normal, 9 -> 8 data pages,
text 180,544 -> 180,448 B and 171,596 -> 171,516 B: 221,184 -> 217,088 B and 208,896 -> 204,800 B of pages, one page
fewer in every game that is ON [measured: tools/check_memory_ledger.py's module rows, both builds, main a4ab7f49 and
this change, tools/fast_gate.sh's vitasdk image].

#418's audit follow-up (F1: the gate's block decided from the job's own memory check, no second look): worker text
180,448 -> 180,512 B debug and 171,516 -> 171,596 B normal, data/bss unchanged, the same pages (436 B of the normal
build's last code page left) [measured: tools/check_memory_ledger.py's module rows, both builds, main 579aa61a and
this change, tools/fast_gate.sh's vitasdk image].

### VitaJPOverlay_Kernel.skprx (`VJO_BUDGET_KERNEL_MODULE_PAGES`)

VitaJPOverlay_Kernel.skprx. API 22's frame-pacing step (kernel/game_ocr.c game_ocr_frame,
VJO_GSTEP_PACE) took 36,336 -> 36,608 B of text and 77,720 -> 77,768 B of data/bss, the same
pages [measured: main 8f1ec8f against the API 22 build]. Raised by one text page in both builds
for the worker loader (#138: the kernel loads the game worker into every game ON in games.ini):
kernel/loader.c 2,354 B of text and 48 B of bss, core/games.c 1,251 B (the Shell's own policy
code, so that the kernel decides by the Shell's rules), the registration's one-thread check
150 B and 8 B, and five more SceSysclibForDriver stubs [measured: the build's objects]. The debug
build: 36,640 -> 40,648 B of text and 77,768 -> 77,824 B of data/bss, the data/bss pages full
(one more byte takes a page) [measured: the build log's module rows, main 3aa08a6 and #138; the same on
main c08db18].
API 23's Anki screenshot (the shot's colour rows and its JPEG's file broker, kernel/game_ocr.c and
kernel/capture.c) is paid for within the same pages by the rest of the kernel's cold code built
for size (vita/CMakeLists.txt: the syscalls and the worker's loop, the boot guard, the kernel
strip, the log) and the request's model directory 256 -> 128 B: 40,648 -> 40,040 B of text and
77,824 -> 77,672 B of data/bss, the data/bss pages no longer full; the normal build 36,444 -> 35,900
and 76,136 -> 75,984 B, the same pages too [measured: the build log's module rows, main dcb7b31 and
this change, both builds].
API 24 removes the full-frame capture (kernel/capture_request.c, the VjoCapture allocator, the
display hook's copy, the worker's completion and timeout, two syscalls): 40,040 -> 37,964 B of
text and 77,672 -> 77,584 B of data/bss, the normal build 35,900 -> 33,840 and 75,984 -> 75,896 B,
the same pages [measured: the build log's module rows, both builds of main 836259e (#150) and API 24].

API 34, remembered hooks: the authenticated cache operation reuses model_stage/model_io and the
existing file thread. It adds one code page, with no buffer/thread/heap growth: packaged SELF
segments 41740+77732 B (debug), 36904+76044 B (normal), rounded 122880/118784 B.
Measured with VitaSDK GCC 15.2.0, Release, local minimal-build kernel-measure-ledger.json reports;
binary sizes only, console boot/free-pool impact unverified.

API 51, a trusted answer's capture on another scene (kernel/game_ocr.c VJO_ROWS_HOOK): text 41,444 -> 41,508 B
(debug) and 36,356 -> 36,380 B (normal), data/bss unchanged (77,732 and 76,044 B), the same pages [measured:
arm-vita-eabi-size on both builds' VitaJPOverlay_Kernel, main 892cfb9 and this change, tools/fast_gate.sh's
vitasdk image]. No block, thread or allocation; the worker's API 43 strip is now taken only on a region without a
screen yet.

An SC3 hook's charset open (VJO_GAME_OCR_OPEN_SC3: the path from the caller's title, loader_title, and the open's
size rule) adds 196 B of text in both builds, no data, no buffer: 43,160 -> 43,356 B (debug) and 38,008 ->
38,204 B (normal), the same pages [measured: the build log's module rows, both builds, main 199fe31 and this
change].

#333: games.ini's reader shared with ocr_line_fix.ini's (core/games.c vjo_games_parse_kind and vjo_games_row_as;
the kernel reads games.ini only): text 43,432 -> 43,508 B (debug) and 38,280 -> 38,356 B (normal), data/bss
unchanged, the same pages [measured: tools/check_memory_ledger.py on both builds, main 9ad8eb5 and this change].

API 55, the pause for the frame (kernel/game_ocr.c: VJO_ROWS_FRAME's checks, the hook's chunked copy and its cap,
the watchdog in the rows tick with #355's review's count of the time the game is in front): debug 43,496 -> 44,152 B
of text and 77,780 -> 77,812 B of data/bss, normal 38,344 -> 38,928 and 76,092 -> 76,124 B, the same pages
[measured: tools/check_memory_ledger.py's module rows, both builds, main efbee95 (v0.10.0) and this change,
tools/fast_gate.sh's vitasdk image]. No block, thread or allocation: the copy runs in the game's own display call, as
a strip's does.

#418's cleanup P3, lowered by one data page in both builds: the display hook's rows converted in place in one
scratch row (kernel/capture.c, core/fbgrey.h vjo_fb_to_abgr_inplace: 3,840 B), the OCR job's first pass sampling
the region signature's grid (kernel/scene.h SceneRows: one SceneAcc, 1,044 B) and change_detection's copy of the
hook's signature on VjoWorker's stack (kernel/main.c: 524 B): data/bss 77,812 -> 72,404 B (debug) and 76,124 ->
70,716 B (normal), text 43,704 -> 43,752 B and 38,496 -> 38,560 B, the same text pages [measured:
tools/check_memory_ledger.py's module rows, both builds, main 85e9da1 and this change].

The normal build's (its `#else` branch):

The normal build: the self-test's memory routes (kernel/meminfo.c), the frame pacing, and the
per-screen, per-job and input traces compiled out: 36,444 + 76,136 against the debug build's
40,648 + 77,824, one text page fewer; 32,332 + 76,080 before #138, which adds one text page
[measured: the build log's module rows, both builds, main 3aa08a6 and #138].

### Other budgets

Before `VJO_STACK_SHELL_NET`:

VjoNet: a job's OCR exchange with the game worker and the local dictionary's lookup. Lowered from
65,536 B ("BearSSL needs it for TLS") once Google Lens, the last of its jobs that made a TLS
connection, was removed: nothing on the thread reaches BearSSL, the HTTP client or a TCP socket
any more (and, since the network stack's removal, no network call at all), and its deepest chain
of the Shell's own code is 5,616 B: net_main 144, a job's vocr_run_one 5,000 (its log batch), then
vjo_log down to the file write 472 [measured: arm-vita-eabi-gcc -fstack-usage -fcallgraph-info=su
of this build, on main 3aa08a6 with #132 and the network's removal; 5,624 B with the UDP send;
the SDK's own code and the indirect-call sites are not counted]. The normal build's is 4,720 B:
net_main 2,144 (the 2 KiB log buffer that holds a job's lines until its result is taken, shell/worker.c
net_main), then a lookup down to the dictionary's page cache [measured the same way, on main eece6c6
with t3-log-path]. The thread paints its
unused stack and logs, at each game exit, the most it has used (shell/worker.c net_stack_check),
so a console run measures it.

Before `VJO_STACK_SHELL_ANKI`:

VjoAnki, while an Anki job runs: with anki_mining on, a count of the queue on every overlay open, a
save (Enter). Lowered from 65,536 B (BearSSL's TLS, for AnkiConnect and an https audio source, both
removed with #125): its deepest chain of the Shell's own code is 1,728 B, a save's queue ID (job_main
24, do_save 672, vjo_queue_save 416, vjo_queue_id 160, SHA-256 456), the file callbacks of its
indirect calls included [measured: arm-vita-eabi-gcc -fstack-usage -fcallgraph-info=su of this
build; the SDK's own code is not counted]. 16 KiB is VjoControl's stack, which runs the same kinds of
SDK calls (sceIo, the log, Paf's heap) on every console.

Before `VJO_BUDGET_PAF_RESULTS_LOCAL`:

One result arena, the local dictionary's lookup (worker.c LOCAL_RESULT_ARENA_SIZE): the only result
memory since the network paths, Google Lens and the Hachidori relay, were removed (they took
2 x 262,144 B arenas and a connection pool: 571,888 B with two slots, 548,096 B with one).

Before `VJO_BUDGET_PAF_ANKI`:

An Anki job (anki.c ANKI_TEXT_MEM): saving a card to the queue. The Shell takes no screenshot
itself: that branch (the kernel's 2,088,960 B full-frame capture, which #74's console refused,
and a 393,216 B arena here) was removed with AnkiConnect, and since API 23 the game worker
makes the card's JPEG in the game's memory (VJO_BUDGET_GAME_SHOT); here only its request and
result, 5,404 B in this arena while the shot runs, after the record's dry runs are released.
A save with 8 dictionaries' entries peaks at 102,624 B, with a screenshot or without (102,616 B
on main dcb7b31) [measured, host, test_shell_anki.c]. anki.c checks its arena against this at compile time.

Before `VJO_WORST_SHELL_PAF`:

The only configuration (the overlay is offline): one 64 KiB arena, the
overlay open on an earlier line, an Anki card being saved. The opt-in network
row went with Google Lens and the Hachidori relay (707,840 B with one connection slot).

Before `VJO_BUDGET_KERNEL_OCR_STRIP`:

API 24 removed the kernel's full-frame capture (vjoRequestCapture, vjoReadRaw; VjoCapture,
KERNEL_RW, at most 960 x 544 x 4 = 2,088,960 B, per request), the worst case's largest item
until then: no Shell code had requested it since Google Lens's JPEG and the Shell's Anki
screenshot were removed (#130, #125), and local OCR (API 19) and the Anki screenshot (API 23)
read the screen in the game. On the owner's 3.65 that 2,088,960 B request failed with
0x80024302 at every attempt (#74).

## Current-game hook profiles (API 57)

The worker's full Luna/verified tables and per-game filter/postprocessor programs move to an indexed
disk archive. Only the current title is read at startup. Its bounded file buffer and decoded records
are static in the worker; the kernel reuses its model staging buffer and file worker. No new block,
thread, Shell allocation or kernel data buffer.

Measured from the packaged SELF segments with the pinned VitaSDK image (2026.08),
`tools/check_memory_ledger.py`, both builds:

| Worker | Before: loaded pages | After: text + data/bss | After: loaded pages | Saved |
|---|---:|---:|---:|---:|
| Debug | 217,088 B | 168,264 + 28,840 B | 204,800 B | 12,288 B |
| Normal | 204,800 B | 159,156 + 28,824 B | 192,512 B | 12,288 B |

`VJO_BUDGET_WORKER_MODULE_PAGES` is lowered to those page counts. Kernel and Shell page budgets
are unchanged; the debug kernel remains 118,784 B (44,712 + 72,404 B), Shell 167,936 B
(97,620 + 69,192 B). These are binary measurements, not console headroom measurements.

## The overlay's text block from the Shell's start (#528)

#528's logs (Rubi 0.10.1, API 57, PCH-1001 on 3.65) hold 34 `overlay not opened: no Paf memory for its text (20 KiB)`
lines (24 at API 57, 10 at API 31): the 20,480 B text block was taken at every open, through the 1.5 MiB reserve
guard, while another plugin's retained 2,187,264 B buffer left the PAF heap 202,296-426,872 B free, and nothing
was shown. The block is now taken once, at the end of `vjo_worker_start` (`shell/overlay.cpp` `vjo_overlay_boot`,
the seam's `shell/overlay_mem.h` `vjo_overlay_mem_boot`), through the same guard while the heap is at its fullest
(3,725,976-3,744,264 B free at all 90 logged Shell starts), and kept (`vjo_overlay_mem_close` gives nothing back);
an open takes it only if that take was refused. The reserve, the guard and every
budget are unchanged: `VJO_BUDGET_PAF_UI` moves from the transient items to `VJO_WORST_SHELL_PAF_PERMANENT`.

| Pool "shell paf" | Before | After |
|---|---:|---:|
| permanent (`VJO_WORST_SHELL_PAF_PERMANENT`) | 8,192 B | 28,672 B |
| worst case (`VJO_WORST_SHELL_PAF`) | 241,664 B | 241,664 B |
| limit / headroom | 985,088 / 743,424 B | 985,088 / 743,424 B |

Shell module, packaged SELF segments with the pinned VitaSDK image, `tools/check_memory_ledger.py`, main `b024c93b`
and this change:

| Shell | Before: text + data/bss | After: text + data/bss | Loaded pages |
|---|---:|---:|---:|
| Debug | 98,164 + 69,632 B | 98,148 + 69,632 B | 167,936 B (unchanged) |
| Normal | 85,948 + 69,556 B | 85,932 + 69,556 B | 155,648 B (unchanged, exact) |

The owner scenario (`tools/owner_scenario.py`, [owner-scenario.md](owner-scenario.md)): no overlay fails to open in
its 273 presses (34 before; the page's own 103,296 B fits at every one); the presses whose overlay now opens ask for
their lookup, so two scenarios' `refused` limits go up (23 → 25, 0 → 9: an open overlay that shows the text and
why there is no entry, `told`, 34 in all), and every scenario's `held` (and three scenarios' `peak`) is 28,672 B:
the kept block.

What SceShell gives up: 20,496 B of its PAF heap while no overlay is open (the block and the heap's 16 B per block),
from the start, beside the 1,977,448 B the heap had at its lowest in #528's 293 sessions without the outside buffer
[measured]; with the overlay open nothing changes. These are binary and log measurements, not a console run of this
build.

The follow-up: with the block held, the 1.5 MiB guard no longer stood before `PageOpen`, and at the logs' lowest free
(131,432 B at a game start) the page would have left SceShell about 8 KB (110,952 B with the block, less the page's
103,296 B). The page now opens only with `VJO_PAF_PAGE_MAX` (103,296 B, the most it took in 81 measured opens) and
`VJO_PAF_PAGE_MARGIN` (64 KiB) free (`shell/paf_guard.h` `vjo_paf_page_fits`, run by `shell/overlay_mem.h`
`vjo_overlay_mem_open`; the refusal is logged in the guard's format). The margin is about three times what SceShell
was seen to take between two of our allocations within one open (at most 17,712 B, 12 pairs) and its median swing over
a game session (21,904 B, 293 sessions) [measured, log]; whether it is enough at the very bottom is not measured
[estimate]. Every press of the owner scenario still opens (the least room, 181,816 B, is above the 168,832 B it asks).
The open's code takes 48 B more in the normal Shell (85,932 -> 85,980 B of text) and none in the debug one (98,148 B),
as the arena resets at the close, at a failed `PageOpen` and at the start went: the block is never given back, so
`s_ui` never points into freed memory, and `open_page` sets it afresh. Pages and data unchanged in both builds.
