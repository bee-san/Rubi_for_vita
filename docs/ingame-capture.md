# In-game capture (API 19)

Local OCR reads the game's screen in the game's own process. The display hook copies only the
region, as 8-bit grey, a strip of rows at a time, straight into a small block of the game
worker's own memory; the recognizer reads its input from there. No copy of the region or of the
frame exists anywhere, and the kernel pool gives nothing to OCR.

## Why (#74)

On the owner's PCH-1001 on 3.65 at `2d1c08c` (API 18) [measured, #74]:

- every OCR job asked the kernel for its capture buffer, 960×544×4 = 2,088,960 B of `KERNEL_RW`
  for the self-test's full-screen region, and got `0x80024302` (`NO_FREE_PHYSICAL_PAGE`) five
  times; the job ended `rc=-103`;
- 458,752 B (917×125×4) had been granted in an earlier session;
- both exact memory queries returned `0x80020001`; SceShell's user view read `-2097152`
  after boot (under the system margin); the PAF heap had 2,346,312 B free at the game start.

`ScePhyMemPartKD` is 36 MiB for every driver and kernel plugin [wiki: Memory_budget]; no working
plugin keeps a full-frame 4 B/px kernel copy while a game runs (the largest on-demand blocks:
reVita 524,288 B, udcd-uvc 786,432 B). The game's own partition had 91,226,112 B free in CLANNAD
[measured, docs/vita-memory.md].

## How a job captures

1. The Shell submits the job; the kernel crops the region (`capture_crop_of`, the same crop as
   before) and the `*TITLEID` worker claims it.
2. The worker checks the game's free memory, then allocates its capture block: one strip of grey
   rows (at most 64 rows and 56 KiB) and a checksum and a bit per region row,
   `vjo_vocr_capture_bytes`: 58,884 B at 960×544, at most 59,588 B (896×544, 64-row strips),
   page-rounded to 61,440 B; under 64 KiB at any region.
3. For each strip the worker calls `vjoOcrCaptureRows` with the strip (`seq`, first row `y`, `n` rows, the rung `mode`, `flags`, `dst_stride`) and `dst`.
   The kernel checks that `[dst, dst + bytes)` is one memory block of the worker's process
   (`ksceKernelFindProcMemBlockByAddr(pid, addr, 0)`, first and last byte), arms the request and
   waits (at most 1.5 s, without the lock, in 50 ms slices that see a cancellation).
4. The next frame the game shows, the display hook (`kernel/display.c`, in the game's own
   thread) converts those rows to grey, `(77 R + 150 G + 29 B) >> 8` after expanding the
   format to 8 bits per channel (`core/fbgrey.h`, shared with the JPEG capture), and writes them
   into `dst` with `ksceKernelMemcpyKernelToUser`: the kernel writes the current process's user
   memory from this hook, as PSVshell and reVita do [1][3]. The call returns the rows.
5. The first pass over the region (`VJO_ROWS_SIG`, from row 0, in order) builds the region's scene
   signature; its last strip (`VJO_ROWS_LAST`) publishes the capture to the Shell as a region
   capture did (`done_seq`, `region_seq`, `capture_scene`). Full-frame and probe passes are not
   published: they are not the user's region.
6. The finder (`vocr_find_lines_rows`, vita-vn-ocr's row-callback form of the frozen finder) reads
   rows through the strip; each line is recognized from the strip, read again from the line's top
   row when the strip does not hold it. A line taller than a strip (a band the finder could not
   split, up to the whole region) goes through the strip too and is kept whole, its own columns
   only, in a block of its size (at most 516,096 B, only when one occurs), so the recognizer gets
   exactly the bytes a copy of the whole region gave it.
7. Every row keeps the checksum of its first read; a later read that differs means the screen
   changed under the job (`VJO_E_SCREEN_CHANGED`, -120): the job reads the region again, up to
   three times, and logs the rows that changed. A region inside one strip is read a second time
   to check it.

Pixel formats: `A8B8G8R8` (0x00000000), `A2B10G10R10` (0x60800000, the top 8 bits of each 10)
and `A1B5G5R5` (0x50000000, each 5-bit channel `<< 3`), as the JPEG capture already converted
them. Any other format is refused (`VJO_ERR_FORMAT`) and its value logged. SceDisplay documents
no RGB565 value, so none is handled; the self-test's `fb` step names the format a game uses.

## The fallback ladder

One job tries each rung in turn and logs every attempt (`gstep alloc` with the memory type, the
block, the native rc and the game's free memory after it):

| rung | where the rows go | when it is used |
|---|---|---|
| 1 | the worker's `USER_RW` block (4 KiB units, cached) | first |
| 2 | `USER_CDRAM_RW` (256 KiB units, uncached; slower CPU reads [SDK: Memory Management Tutorial]) | `USER_RW` refused the block |
| 3 | `USER_MAIN_PHYCONT_NC_RW` (1 MiB units, uncached) | CDRAM refused it too |
| 4 | a kernel strip (`VjoOcrStrip`, ≤ 64 KiB of `KERNEL_RW` for one call, copied out by the syscall) | the hook could not write the game's memory (`VJO_ERR_COPY`/`VJO_ERR_ARG`); never while the JPEG capture holds its buffer |

The workspace and a tall line's block take the same rungs 1-3; the verified weights stay in
`USER_RW`. Before any of them the existing model fallback applies: the configured model → its
fallback (VOCR3-H15 → VOCR3-FL10 by default) when the game's free memory cannot hold the
configured one, then a refusal that names the game's memory. A rows call that
returns BUSY (a copy still running after 2 s) leaves the block allocated for the rest of the
game's process rather than free memory the hook might write.

## Memory per job

| | API 18 | API 19 |
|---|---|---|
| kernel (`ScePhyMemPartKD`) | w×h×4, 2,088,960 B at 960×544 | 0 B (rung 4 only: ≤ 64 KiB for one call) |
| game: capture | the grey plane, w×h in the workspace (522,240 B at 960×544) | ≤ 61,440 B at any region |
| game: workspace with the grey plane (H15) | 712,704 B at 960×544, 303,104 B at 917×125 | 188,416 B at any region (no plane) |
| game: a line taller than a strip | — | its own width × height, only then (41 of 372 linefind dev regions; the largest 533×148 = 78,884 B) |
| game: weights (resident) | H15 1,159,168 B, FL10 659,456 B | unchanged |

So a textbox takes at most 61,440 B of capture memory when all its lines fit a strip: 331 of
the 372 linefind dev regions. The other 41 contain a band the finder reports taller than a strip
(42 such lines), which the recognizer needs whole for byte-identical input, as API 18's grey plane
held it; it takes a block of its own size for that line. With it, 349 regions stay within 64 KiB
of capture memory in all and 23 go over; the most is 131,072 B (an 822×183 region: its 53,364 B
capture block and its 822×89 band of 73,158 B, page-rounded to 57,344 + 73,728 B), and the largest
band is 533×148 = 78,884 B. The design bound is the
ledger's `VJO_BUDGET_GAME_LINE`, 516,096 B of game memory (nearly a whole 960×544 grey frame, should
the finder ever report a band that large). A strict 64 KiB bound for those regions would need a
row-callback form of `vocr_run` in vita-vn-ocr (not done: `vocr.c` is unchanged). Page-rounded figures from `vjo_vocr_job_bytes`/`vjo_vocr_capture_bytes` on the host build; the
ledger counts them as `VJO_BUDGET_GAME_CAPTURE`, `VJO_BUDGET_GAME_LINE`,
`VJO_BUDGET_GAME_WORKSPACE` and `VJO_BUDGET_KERNEL_OCR_STRIP` ([memory-ledger.md](memory-ledger.md)).
The Lens and Anki JPEG captures still use the kernel's region-sized buffer; #74's refusal would
reach a full-screen JPEG too (not changed here).

## Memory queries on 3.60-3.74

- **The game and SceShell:** `sceKernelGetFreeMemorySize` reports the caller's budget minus a
  system margin, as signed ints [SDK: Kernel-Reference]. A negative figure is under the margin:
  the worker counts it as 0 B free, the self-test writes `valid=0`. The worker samples main, CDRAM
  and phycont before and after each block.
- **The kernel, three routes,** each looked up at run time with `module_get_export_func` and each
  reporting its own result (`kernel/meminfo.c`):
  1. *block*: `ksceKernelFindMemBlockByAddr(addr, 0)` (size 0, as h-encore and vmass pass it; a
     size above 0 asks for a block that just fits) → `ksceKernelMemBlockGetInfoEx` →
     `PhyMemPartInfoCore`, with the step that failed;
  2. *cas*: the caller's address-space block at +328/+332/+316 (main/CDRAM/phycont), as PSVshell
     and PSVshellPlus read it on 3.60 and 3.65 [1][2];
  3. *as*: `sceKernelGetProcessAddressSpace` of the kernel process and
     `SceSysmemForKernel_C3EF4055`'s partition list, as faps-coredump reads it on 3.65 [10]; a
     partition of 36 MiB is ScePhyMemPartKD.
  The `PhyMemPartInfoCore` NID changed at 3.63 (0x63A519E5/0x3650963F on 3.60-3.61,
  0x02451F0F/0xB9B69700 on 3.63-3.74) [wiki: SceSysmem]. Kernel objects live below 0x40000000
  (TTBR0; 0x40000000 and up is user memory [VitaSDK `psp2kern/kernel/sysmem/data_transfers.h`;
  wiki: Virtual_Memory]). A partition object or control block is read when it is in PSVshellPlus's
  range, 0x1000-0x1000000 (`info.c:57`, `:91`), or higher in the kernel half only inside a kernel
  memory block; anything else is refused before it is read, with its own step (`kmem ... step=`:
  1 find, 2 block or address-space info, 3 outside the kernel range, 4 in no memory block, 5
  InfoCore). The control block's own pointer is checked too, which PSVshellPlus does not do
  (`info.c:82-91` adds 328 and reads): a `cas` route stopped at step 3 or 4 on a console means
  this guard refused the block, not the firmware. Whether `ksceKernelFindMemBlockByAddr` finds a
  block for a kernel heap object above 0x1000000 is unverified; step 4 says when it does not.
  `host/tests/test_kernel_meminfo.c` runs the routes with the objects at kernel-range,
  upper-kernel and user-range addresses (the user-half ones unreadable, at 0x5... and 0x8..., so a
  read faults the test), and the guard at each of its edges; the guard as #75 first had it (only
  0x40000000 and up read) fails all six cases. Route 3 asks for the kernel process first and the
  caller's if that is refused (faps-coredump asks for a user process). Which route reads on 3.65 is
  [unmeasured]. The routes are diagnostics: `tools/analyse_selftest.py` reports a route that read
  nothing as WARN, and no diagnostic changes the OCR verdict ([self-test.md](self-test.md)).

## Prior art

Every source was read at the commit listed:

| plugin | what it does with the frame | memory |
|---|---|---|
| [1] Electry/PSVshell @9a47b0e `src/main.c:64-95`, `src/gui.c:738-741`, `src/perf.c:115-146` | kernel display hook in the game's context; writes its OSD rows into the game framebuffer with `ksceKernelMemcpyKernelToUser`; reads memory through the address-space block | `KERNEL_RW` 425,984 B once |
| [2] GrapheneCt/PSVshellPlus @b70f1c7 `PSVshellPlus_Kernel/info.c:75-145` | the same hook; the address-space block with pointer range checks | |
| [3] MERLev/reVita @f35adc5 `reVita/src/main.c:401-437`, `gui/gui.c:490-506`, `gui/renderer.c:205-208` | the same hook and copy | `KERNEL_RW` 524,288 B while its menu is open, refusal shown |
| [4] Electry/VitaGrafix @65b6ee3 `src/main.c:50-66`, `src/osd.c:58-66` | user code in the game, `sceDisplaySetFrameBuf` import hook; reads and blends through `base` in place | none |
| [5] Rinnegatamante/VITA2PC @3c146e3 `psvita/main.c:146-161`, `psvita/encoder.c:44-70` | `sceDisplayGetFrameBuf` from a game thread; reads `base` in place | 1 MiB encoder buffer: CDRAM, else PHYCONT_NC, else software |
| [7] xerpi/vita-udcd-uvc @973fb65 `src/main.c:579-650`, `:681-695`, `:805-844` | the physical frame converted by IFTU; the CPU reads no pixels | 786,432 B physically contiguous |
| [10] Princess-of-Sleeping/faps-coredump @4c35730 `src/process_address_space.c:198-268`, `src/main.c:198-227` | the target's address space and its partitions (3.65 NIDs) | |

## Evidence and limits

Off the console (`tools/preflight.sh` runs all of it and prints GO/NO-GO):

- `host/tests/test_capture_parity.c`: API 18's pipeline (8228c3c's kernel conversion and core,
  verbatim) against API 19's (today's kernel `capture_grey_rows` and core), both run to
  `vocr_run`: byte-identical recognizer input and identical preprocessed tensors on all 372
  regions of vita-vn-ocr's linefind dev manifests (206 Vita, 166 PSP) in each of the three formats,
  and on procedural frames; also on ARMv7 under qemu-arm (`tools/arm_host_tests.sh` runs the
  procedural frames, `tools/preflight.sh` the eval crops through the same ARM build).
- `host/tests/test_alloc_sim.c`: the real worker and kernel under #74's measured pools: API 18's
  full-screen buffer refused with `0x80024302`; API 19 at the full screen and a textbox,
  VOCR3-H15 and VOCR3-FL10 (and v0.2.0's H15), with 0 B of the kernel pool, also when it refuses
  everything; each ladder rung.
- `host/tests/test_kernel_game_ocr.c`, `test_game_model_broker.c`, `test_game_vocr.c`: the rows
  syscall, the hook path, every refusal, cancellation, process exit, the ladder and the probe.
- `tools/vita3k_smoke.sh`: the real worker's five stage jobs and both other formats as a Vita
  homebrew app in Vita3K, crash check only.

Known limitation (from the audit, not reachable in a normal run): the kernel worker tidies a
rows call whose process exited once 10 s have passed, so a thread of an exited process still
running that call's kernel code after that time could act on the next game's request. A call
waits at most 3.5 s (`ROWS_WAIT_US` + `ROWS_COPY_WAIT_US`), and a dying thread's kernel wait is
expected to end at once [inference], so this needs a thread held for seconds inside the syscall
after its process exited; a generation number per request would close it.

Only the console answers: whether the hook can write each memory type of every game, the time a
strip takes in the hook and its CPU3 cost (unmeasured, AGENTS.md rule 8), which memory route
reads on 3.65, and the free figures themselves. The self-test's `probe` stage and `gstep` lines
report each of these.
