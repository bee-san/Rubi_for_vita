# The memory ledger

Every byte our three plugins take from a Vita memory pool is accounted for in one place, checked
in CI, and printed at build time and at run time. The pools themselves, their sizes and who else
uses them are in [docs/vita-memory.md](vita-memory.md); why this exists is
[issue #59's root cause](issue-59-root-cause.md).

## The parts

| Part | Where | What it does |
|---|---|---|
| budget table | [`include/vjo_ledger_budget.h`](../include/vjo_ledger_budget.h) | one `#define` per item (its worst case in bytes) and per pool (its limit), with the source of each number; plain integer arithmetic so the C code and the checker read the same values |
| counters | [`core/ledger.c`](../core/ledger.c) | current and peak bytes per item and per pool, atomics only, no libc; one `VjoLedger` per process |
| wrappers | `shell/memory.cpp`, `shell/ledger.c`, `kernel/buffers.c`, `kernel/main.c`, `game_ocr/main.c` | the only places that call an allocator or create a thread; each names its item and counts the bytes in and out |
| CI check | [`tools/check_memory_ledger.py`](../tools/check_memory_ledger.py) | fails the build on an allocation outside the wrappers, a worst case over its limit, or a packaged module with more pages than declared; prints the table |
| run time | `vjo_ledger_shell_log` (Shell), `kledger_log` (kernel) | the pools and items, current and peak, in `status.txt` and `kernel.txt` |

## The table

Printed by `tools/check_memory_ledger.py` (CI runs it in the host job against the sources and in
the Vita job against the packaged modules; `ctest -R memory_ledger` runs it locally):

```
  pool            permanent   worst case        limit     headroom
  shell pages       532,480      602,112    1,048,576      446,464
  shell paf           8,192      973,296      985,088       11,792
  kernel            131,072    2,220,032    2,220,032            0
  game               81,920    2,330,624    2,392,064       61,440
  packaged modules (loaded pages vs declared budget)
      VitaJPOverlay_Kernel      114,688 B of    114,688 B declared
      VitaJPOverlay_Shell       397,312 B of    397,312 B declared
      VitaJPOverlay_OCR          45,056 B of     45,056 B declared
```

*Permanent* is held from boot (or from the game's start) until exit; *worst case* adds the largest
transient set that can be live at once.

### Pool "shell pages": SceShell's partition (`ScePhyMemPartShell`, 50 MiB)

| item | bytes | what | source |
|---|---|---|---|
| Shell module pages | 397,312 | `VitaJPOverlay_Shell.suprx`: 217,088 text + 180,224 data/bss, permanent (405,504 B before the cold files were built with `-Os`) | packaged SELF segments [measured] |
| OCR worker pages in SceShell | 45,056 | `VitaJPOverlay_OCR.suprx`, which `*ALL` also loads into SceShell, permanent | packaged SELF segments [measured] |
| VjoNet stack+TLS | 69,632 | 65,536 + 4,096, permanent | `shell/worker.c`; TLS per thread [SDK: Kernel-Reference, `sceKernelCreateThread`] |
| VjoControl stack+TLS | 20,480 | 16,384 + 4,096, permanent | same |
| VjoAnki stack+TLS | 69,632 | only while a card is saved or sent | `shell/anki.c` |
| VjoShellMem USER_RW | 0 | the host tests' result memory; the Vita build uses PAF (`VJO_PAF_ALLOC`) | `shell/worker.c` |
| **limit** | **1,048,576** | half of the "genuinely under 2 MiB" a Shell plugin may have at most [expert]; SceShell had 28,672 and 98,304 B free beside a game [measured], so the limit is a ceiling on our footprint, not a promise that it fits: module pages may only shrink, and the checker fails when a packaged module has more pages than declared | AGENTS.md rule 1; [vita-memory.md 5.1](vita-memory.md) |

### Pool "shell paf": SceShell's PAF global heap (5,242,880 B, mapped once)

| item | bytes | what | source |
|---|---|---|---|
| Paf plugin+RCO | 8,192 | the RCO (5,632 B file) and the Paf `Plugin` object | **[estimate]**; the self-test measures it |
| result arenas | 65,536 (local dictionary) or 571,888 (network: 2 × 262,144 + 47,600 connection pool) | held while a game runs | `shell/worker.c` `LOCAL_RESULT_ARENA_SIZE`, `MEM_SIZE`; asserted equal in `test_shell_worker.c` |
| Anki job arena | 393,216 (with a picture field; 131,072 without) | per job | `shell/anki.c` `ANKI_PICTURE_MEM`; asserted equal in `test_shell_anki.c` |
| **limit** | **985,088** | the measured free heap, 2,557,952 B (2,498 KiB beside CLANNAD, 2026-10-10), minus the 1.5 MiB reserve the runtime guard keeps for firmware; the reserve is the headroom | `docs/meiki-ocr.md` on `experiment/meiki-ocr` [measured]; `shell/memory.cpp` |

Allocations here take no new pages from the partition; a failure is a `NULL`, never `0x80024302`
(vita-memory.md 3.2). Later sessions saw 355-441 KiB and 190 KiB free [measured], which would
refuse everything above; the code survives a refusal (host tests), so this limit is the best
measured case, not a guarantee. The worst case (a network dictionary plus an Anki card with a
picture) is 11,792 B under it: the fix PR moves work out of this pool.

### Pool "kernel": `ScePhyMemPartKD` (36 MiB, shared by every driver)

| item | bytes | what | source |
|---|---|---|---|
| kernel module pages | 114,688 | `VitaJPOverlay_Kernel.skprx`: 32,768 text + 81,920 data/bss (the 32 KiB model staging buffer and the 4 KiB log ring are in it) | packaged SELF segments [measured] |
| VjoWorker stack | 16,384 | permanent | `kernel/main.c` |
| VjoCapture | 2,088,960 at most (960 × 544 × 4) | region-sized, `KERNEL_RW`, per capture, freed when the reader has the last row; 458,752 B at 917 × 125 [measured] | `kernel/buffers.c` |
| **limit** | **2,220,032** | today's worst case: a **ratchet**, not a cited safe size. The pool's free space on a retail console is unmeasured; a 2,088,960 B capture failed there once (#14) and has succeeded since. The self-test reads the partition kernel-side and replaces this number | [vita-memory.md 5.3](vita-memory.md) |

### Pool "game": `ScePhyMemPartGame` (256 MiB minus the game's own use)

| item | bytes | what | source |
|---|---|---|---|
| OCR worker pages | 45,056 | the module in the game | packaged SELF segments [measured] |
| VjoGameVocr stack+TLS | 36,864 | 32,768 + 4,096 | `game_ocr/main.c` |
| VjoGameVocrWeights | 1,560,576 at most | F20, the largest accepted model, page-rounded; resident between jobs | `core/vocr_ocr.c`; asserted in `test_ledger.c` |
| VjoGameVocr workspace | 770,048 at most | F20 at a 960 × 544 region, page-rounded, per job | `vjo_vocr_job_bytes`; asserted in `test_ledger.c` |
| **limits** | weights 1,601,536; workspace 790,528; job 2,392,064 | the recognizer's ceiling tier, which the preflight (free `USER` minus the 4 MiB reserve) has to cover before anything is allocated | vita-vn-ocr [`docs/vita-memory-budget.md`](https://github.com/bee-san/vita-vn-ocr/blob/main/docs/vita-memory-budget.md) |

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
ledger boot pages: current=532480 peak=532480 limit=1048576
ledger boot paf: current=8192 peak=8192 limit=985088 free=2557952
ledger   Shell pages: current=397312 peak=397312
ledger   OCR worker pages: current=45056 peak=45056
ledger   VjoNet stack: current=69632 peak=69632
ledger   VjoControl stack: current=20480 peak=20480
ledger   Paf plugin (est.): current=8192 peak=8192
```

`free=` is the PAF heap's free size as `GetFreeSize()` reports it (0 before Paf is ready). Stacks
include their 4 KiB of TLS. The kernel writes `ledger kernel: current=… peak=… limit=…` and its
items to `kernel.txt` at start and at every capture. The game worker keeps its ledger in the
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
