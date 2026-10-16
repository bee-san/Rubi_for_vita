# Issue #59: why SceShell died in `SceLiveAreaDailyCronJob` with `0x80024302`

Written 2026-10-13 for [issue #59](https://github.com/bee-san/vita-jp-overlay/issues/59).
Every number below was measured on the host from the two builds involved, or is quoted from an
earlier console log; the two kinds are kept apart. The console was a PCH-1001 on firmware 3.65.

## Summary

- The crashing thread is firmware's, not ours: `SceLiveAreaDailyCronJob` in SceShell, inside
  `ScePaf`, with `R0 = 0x80024302` (`SCE_KERNEL_ERROR_NO_FREE_PHYSICAL_PAGE`) and a prefetch
  abort whose IFSR `0x2` is a *debug event*: a `BKPT`, i.e. ScePaf's own assert after
  `sceKernelAllocMemBlock` failed. SceShell's memory partition had no free page left when a
  LiveArea task asked for more.
- SceShell's partition is 50 MiB and, on this console, was already within tens of KiB of empty
  while a game ran (two earlier samples: 28,672 B and 98,304 B free). An eager 786,432 B result
  allocation of ours had already failed in that same partition with the same `0x80024302` in
  October (#11), so the exhaustion itself is a measured fact for this console, not a theory.
- Between the API 13 build that boots (v0.7.12, `aff03cb`) and the API 16 build that does not
  (`e759187`), our permanent footprint inside SceShell's partition grew by **36,864 B**: the Shell
  plugin's mapped pages grew by 49,152 B (+40 KiB of code for the Anki miner, the furigana strip,
  dictionary paging and new worker logic; +10.6 KiB of `.bss`), while the game worker, which
  `*ALL` also loads into SceShell, shrank by 12,288 B. Nothing else our plugins hold in SceShell
  changed: the two thread stacks, the RCO and the PAF plugin are the same.
- **Cause, as far as the host can show it:** our Shell-side plugins hold about 0.52 MiB of
  SceShell's partition permanently (module pages, thread stacks and TLS), in a partition that
  firmware itself fills to within about 0.1 MiB during a game, and that footprint grew by
  36,864 B in exactly the build that stopped booting. The daily LiveArea task then found no free
  page. The same mechanism explains issue #42, where adding a 65,544 B static buffer to the Shell
  plugin hung the boot twice and removing it fixed it.
- **What the host cannot prove:** that the LiveArea task's failed request fell inside those
  36,864 B rather than inside the other 0.48 MiB we were already holding, and how much SceShell
  had free at that instant. The two dumps' backtrace (the ScePaf caller and the requested size)
  and one `sceKernelGetFreeMemorySize` sample from SceShell at boot would settle it. The
  self-test (#5 of the plan) records that sample.
- **Consequence for the fix:** the trigger is a few pages, so trimming is not enough; every byte
  the Shell plugin does not need in SceShell must leave it, and the total must be enforced in
  CI (the ledger). The fix PR and the ledger PR give the numbers.

## 1. The crash signature, decoded

| Dump field | Value | Meaning | Source |
|---|---|---|---|
| thread | `SceLiveAreaDailyCronJob` | a SceShell thread that runs LiveArea's daily maintenance; none of our threads (`VjoControl`, `VjoNet`, `VjoAnki`) | the two dumps, as reported in #59 |
| exception | `PREFETCH_ABORT`, IFSR `0x2` | ARMv7 IFSR status `0b00010` is "debug event": a `BKPT` instruction was executed. This is ScePaf's assert path, not a bad jump | ARM Architecture Reference Manual ARMv7-A, IFSR encodings; dump |
| PC | `ScePaf@1+0x13d8` | inside firmware's ScePaf module | dump |
| R0 | `0x80024302` | `SCE_KERNEL_ERROR_NO_FREE_PHYSICAL_PAGE`: the memory partition the caller allocates from has no free physical page. It is a *partition* error, not a heap-full error | SDK `Error-Overview` (code C1-2569-2); VitaSDK `psp2/kernel/error.h` |

So a firmware PAF heap asked the kernel for pages (a new heap, or growth of one) and the
partition was empty. R0 still holding the kernel's return value at the `BKPT` is what an assert
of the form `if (ret < 0) trap()` leaves behind.

## 2. Whose partition, and how full it was

- SceShell's main memory comes from `ScePhyMemPartShell`, **50 MiB** on retail firmware
  ([Memory budget](https://wiki.henkaku.xyz/index.php?title=Memory_budget&oldid=18995)). Its
  CDRAM partition (16 MiB) is separate and not involved: CDRAM exhaustion returns `0x80024309`.
- Everything a `*main` plugin holds is charged to that partition: its module segments (loaded by
  taiHEN into SceShell's process), its thread stacks and TLS ("the user mode stack and user mode
  TLS are obtained from the application's memory budget at the time of thread creation", SDK
  `Kernel-Overview`; each thread also takes at least 4 KiB of TLS, SDK `Kernel-Reference`,
  `sceKernelCreateThread`), and every `sceKernelAllocMemBlock` it makes. Page granularity is
  4 KiB (`sceKernelAllocMemBlock`, SDK `Kernel-Reference`).
- The PAF global heap is different: SceShell maps it once, 5,242,880 B
  (`docs/meiki-ocr.md` on the `experiment/meiki-ocr` branch, measured on this console), so our
  `sce_paf_memalign` allocations do not take new pages from the partition. They do take room that
  firmware's own PAF users need, which is why the 1.5 MiB reserve guard exists.
- Measured on this console while a game ran, from our own Shell plugin:
  - `sizeMain` free **98,304 B** of 52,428,800 B (vita-vn-ocr `notes/26`, native sample during
    CLANNAD, "Shell figures");
  - **28,672 B** free of 50 MiB in an earlier pushed sample (vita-vn-ocr `notes/08`; address
    space ownership not verified, consistent with the Shell);
  - our eager 2 × 384 KiB `VjoShellMem` `USER_RW` result allocation failed with `0x80024302`
    (vita-vn-ocr `notes/06`, fixed by #11). Same partition, same error code as #59.
- Issue #42 (v0.7.9 era): a build that only added one static 64 KiB buffer (65,544 B of `.bss`)
  to the Shell plugin "caused a reproducible boot/loading hang on the physical Vita", twice;
  restoring the original Shell fixed it both times. That is the same partition flipping from
  "just enough" to "not enough" on a 16-page change.

These samples are from different days and different SceShell states, so the headroom is not a
constant. What they establish is its order of magnitude: tens of KiB, not MiB. Any permanent
allocation of ours in SceShell is therefore a direct subtraction from what firmware has left.

## 3. Every byte our plugins hold in SceShell's partition

Measured from the packaged SELF files with `tools/check_vita_syscall_abi.py`'s parser (segment
`memsz`, rounded up to 4 KiB pages as the loader does) and from the source. A local rebuild of
each commit with VitaSDK gcc 15.2 reproduces CI's segment sizes to within a few bytes, so the
per-object attribution in 3.4 comes from those local builds.

### 3.1 At boot and at steady state (overlay closed, local pipeline)

| Item | pool | API 13, v0.7.12 (`aff03cb`) | API 16 (`e759187`) | change |
|---|---|---|---|---|
| `VitaJPOverlay_Shell.suprx` text segment (RX) | SceShell partition, module pages | 181,744 B → 184,320 B | 221,984 B → 225,280 B | +40,960 B |
| `VitaJPOverlay_Shell.suprx` data+bss segment (RW) | same | 168,024 B → 172,032 B | 178,628 B → 180,224 B | +8,192 B |
| `VitaJPOverlay_OCR.suprx`, loaded into SceShell too (see 3.3) | same | 46,040 + 4,944 B → 57,344 B | 35,872 + 4,480 B → 45,056 B | −12,288 B |
| `VjoNet` thread stack + TLS | same | 65,536 + 4,096 B | same | 0 |
| `VjoControl` thread stack + TLS | same | 16,384 + 4,096 B | same | 0 |
| **permanent pages in SceShell's partition** | | **503,808 B** | **540,672 B** | **+36,864 B (9 pages)** |
| RCO loaded as a PAF plugin | PAF global heap (pre-mapped) | 5,248 B file | 5,632 B file | +384 B |
| PAF `Plugin`, `MainThreadCallList` entry | PAF global heap | small, unmeasured | same | 0 |
| mutexes, event flags | kernel objects | 4 (`VjoView`, `VjoCmd`, `VjoNetEv`, `VjoNetCancel`) + `VjoLog`, `VjoStatus` | + `VjoAnkiBox` | +1 |

Module-manager bookkeeping for a loaded module (relocation tables, import/export binding) is
charged somewhere too; its size is unknown and not counted here.

### 3.2 At game launch and during use

| Event | API 13 | API 16 | pool |
|---|---|---|---|
| game start (`activate_game`): config, region, `vjoSetGameActive` | static scratch only | same, plus two `sceAppMgrAppParamGetString` calls (title name for Anki) | none in SceShell |
| overlay open, local pipeline | result arena 65,536 B when a lookup runs | same | PAF heap, through the 1.5 MiB reserve guard |
| overlay open, network dictionary or Lens (both removed since) | 2 × 262,144 B arenas + connection pool | same | PAF heap |
| Anki `×`/`△` (`anki_mining = on`, off by default) | not built into the Shell | 131,072 B (393,216 B with a picture field) + a 65,536 B thread stack + 4 KiB TLS, freed after the job | PAF heap; stack from the partition |
| strip with furigana (needs a jiten/jpdb key; removed with those dictionaries since) | not built | `vjo_strip_ruby_tpl` text widgets, made on demand | PAF heap |

With the default configuration used in the test (VOCR, local JMdict, Anki off) the only
allocation after boot is the 64 KiB result arena, and it did not run: no OCR job was logged.

### 3.3 The game worker is loaded into SceShell as well

taiHEN's config parser records `*ALL` entries for every section except `*KERNEL`, and
SceShell's section is `*main`
([taihen-parser `parser.c`, lines 242-246](https://github.com/DaveeFTW/taihen-parser/blob/6d817fdb801ab74a53d96f0fefb02b258c820aab/src/parser.c#L242-L246)).
`VitaJPOverlay_OCR.suprx` is registered under `*ALL`, so it is loaded into SceShell (and into
every other application) at start. Its `module_start` finds that `main` is not a candidate
title and starts nothing, but it returns `SCE_KERNEL_START_SUCCESS`
([`game_ocr/main.c`](../game_ocr/main.c), end of file), so the module stays resident: 45,056 B
of SceShell's pages for code that never runs there. Returning `SCE_KERNEL_START_NO_RESIDENT`
there frees them at once; the fix PR does this, and the plan moves the worker to `*TITLEID`
sections so it is not loaded into SceShell at all.

### 3.4 Where the Shell plugin's 40 KiB of new code came from

Per object file, API 13 → API 16 (local builds of both commits with the same toolchain):

| object | text | `.bss` | what |
|---|---|---|---|
| `shell/anki.c` + `core/anki.c` + `core/anki_queue.c` + `shell/anki_queue.c` | +23,358 B | +3,824 B | Anki mining with the offline queue (#49); not linked into the API 13 Shell |
| `shell/worker.c` | +4,090 B | +4,132 B | resident-weight reporting, fallback model, OOM advice (#54-#57); dictionary pages (#51) |
| `shell/strip.cpp` + `core/furigana.c` + `core/pitch.c` + `shell/strip_view.c` | +4,798 B | +2,608 B | furigana strip (upstream merge #48) |
| `core/config.c` | +2,660 B | 0 | new keys (Anki fields, fallback model, dictionary pages) |
| `core/local_dict.c` | +1,261 B | 0 | dictionary paging (#51) |
| everything else | +3,292 B | +16 B | |
| **total** | **+39,459 B** | **+10,580 B** | |

The Shell's `.bss` at API 16 is 178,468 B. The large items: `s_ui_mem` 98,304 B (the overlay's
text-building arena, used only while the overlay page is open), `scratch_mem` 24,576 B,
`s_mem_buf` 8,480 B and `s_strip_u` 8,194 B (the strip), `g_view` 7,800 B, three `VjoConfig`
copies of 3,516 B each, `local_text` and `s_cursor_text` 4,096 B each, the 4,384 B game-OCR
result. All of it is charged to SceShell's partition whether or not the overlay is ever opened.

## 4. Candidates that are not the cause

| candidate | why not |
|---|---|
| H15 as the default model (#57), resident weights (#54) | they live in the *game's* partition (`VjoGameVocrWeights`, `VjoGameVocr` blocks in `game_ocr/main.c`), 1.16 MiB against the 87-91 MiB CLANNAD had free. SceShell's partition is a different pool. No game OCR job even ran before the crash. |
| the kernel's 32 KiB model staging buffer (#54: 8 → 32 KiB, kernel `.bss` 53,008 → 78,128 B) | kernel module pages come from `ScePhyMemPartKD`, not from SceShell's partition. It costs the kernel pool 28,672 B more pages and is now in the ledger, but it cannot produce `0x80024302` in a SceShell thread. |
| a PAF heap leak of ours | the PAF global heap is pre-mapped; filling it would starve PAF users but not the kernel's page allocator. Our only PAF allocations after boot are the 64 KiB result arena (not run) and the RCO. |
| the Anki thread (64 KiB) | created only when a card is saved or sent, and `anki_mining` was off. |
| the installer or configuration | the tai configuration was byte-identical before, during and after; the config.ini only selected H15 and the FL10 fallback. |
| a SceShell PAF *heap* exhaustion (the 5 MiB heap) | would surface as a PAF allocation returning NULL to SceShell code, not as `0x80024302` in R0. |

## 5. The cause, stated plainly

SceShell's 50 MiB partition is nearly full while a game runs (measured: 28-98 KiB free). Our
`*main` plugin and the `*ALL` worker hold 540,672 B of it permanently in the API 16 build, plus
the PAF plugin, and that permanent share grew by 36,864 B between the API 13 build that boots and
the API 16 build that does not. The daily LiveArea task asked ScePaf for memory, ScePaf asked the
kernel for pages, none were left, and ScePaf's assert took SceShell down. On the next boot the
task was still due and the same thing happened, which is why a normal boot "refused" until the
API 13 plugins were restored.

Whether the task's request would have fitted with 36,864 B more free is the one link the host
cannot check. It is the most economical explanation of five facts: the error code and the
thread, the measured headroom, the identical earlier failure of our own 786 KiB allocation, the
#42 experiment in which 65,544 B of extra `.bss` alone broke the boot, and the clean boot after
restoring a plugin set that differs by exactly those pages. It is not a proof.

## 6. What would make it a proof

1. The two dumps' backtrace of `SceLiveAreaDailyCronJob`: the ScePaf function at `+0x13d8` and
   its caller tell which heap grew and by how much (the `vsize` argument is in R2 on entry to
   `sceKernelAllocMemBlock`).
2. One `sceKernelGetFreeMemorySize` sample from SceShell at boot and one at game launch, in both
   builds. The self-test (plan item 5) writes these into its report.
3. The LiveArea task's own allocation size, if the dump holds the heap parameters.

## 7. What follows

- Boot safety first (plan item 4): a failed boot must fall back to a mode in which our plugins
  hold nothing in SceShell, so a bad build never needs recovery again.
- The ledger (item 3): the Shell module's page count, its stacks and every allocation become
  a budget checked in CI, printed in the build log and in `status.txt`.
- The fix (item 7, done): `NO_RESIDENT` for the worker outside a candidate game (−45,056 B in
  SceShell and every system app), the overlay's 96 KiB text arena out of `.bss` into a 20 KiB
  PAF allocation held only while the page is open (−98,304 B of pages), the cold files built
  for size (−8,192 B). Permanent pages in SceShell's partition: 540,672 → 389,120 B, 73,728 B
  below the v0.7.4 build that ran beside a game. `--gc-sections` was tried and grew the Shell's
  text (section alignment under `-Wl,-q`); not applied. Still to come, as the expert's rule
  requires: the dictionary lookup and the Anki queue leave SceShell for the game process through
  the kernel broker; the Shell keeps only the PAF UI. Done since: the worker moved from `*ALL`
  to `*TITLEID` (the installer's `--title`; [vita-memory.md 4.3](vita-memory.md)).
