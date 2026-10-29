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
   before) and the game's worker claims it.
2. The worker checks the game's free memory, then allocates its capture block: one strip of grey
   rows (at most 64 rows and 56 KiB) and a checksum and a bit per region row,
   `vjo_vocr_capture_bytes`: within 64 KiB at any region (65,480 B at 960×544, 61,388 B at
   917×125). Since API 20 each row also keeps its recent states, so an animated icon is not a change in a
   job with the stable-text wait ([stable text](vita-vn-ocr.md#stable-text-and-blank-lines-api-20)): 16 earlier states when they
   fit, as many as fit otherwise (6 at 960×544, 5 at 896×544, the widest strip).
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
   to check it. A job whose every read changed shows nothing, and no message (#81 request 4: 4 of
   the test build's 21 console jobs ended so); the Shell logs `job dropped: the screen changed
   under it (N in a row), next try in T ms` and tries again: at once, from the
   second in a row 250 ms later, doubling up to 4 s, until a job reads its screen
   (`shell/worker.c` `note_screen_changed`). An open overlay waiting for its text shows
   "Recognizing…" meanwhile; one already showing text keeps it.

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
| 4 | a kernel strip (`VjoOcrStrip`, ≤ 64 KiB of `KERNEL_RW` for one call, copied out by the syscall) | the hook could not write the game's memory (`VJO_ERR_COPY`/`VJO_ERR_ARG`) |

The workspace and a tall line's block take the same rungs 1-3; the verified weights stay in
`USER_RW`. Before any of them the existing model fallback applies: the configured model → its
fallback (since API 21 the next smaller model size, Normal → Small by default) when the game's free memory cannot hold the
configured one, then a refusal that names the game's memory. A rows call that
returns BUSY after arming (a copy still running after 2 s) ends its job and leaves the block
allocated rather than free memory the hook might write, until the next job: the kernel hands out
no job while that copy runs (`vjoOcrSubmit` answers BUSY until the kernel worker sees the copy
end), so the next job gives the block back before it takes its own (`release_abandoned_capture`).
Until 0.10.1 the block stayed for the rest of the game's process and the worker took no more jobs,
which also kept its resident weights, LM and spotter there until the game exited (#528). Through
the kernel strip (rung 4) the hook writes the kernel's strip, never the worker's block, which goes
back with its job.

## Memory per job

| | API 18 | API 19 |
|---|---|---|
| kernel (`ScePhyMemPartKD`) | w×h×4, 2,088,960 B at 960×544 | 0 B (rung 4 only: ≤ 64 KiB for one call) |
| game: capture | the grey plane, w×h in the workspace (522,240 B at 960×544) | ≤ 65,536 B at any region (API 19 kept one checksum and one bit a row: ≤ 61,440 B) |
| game: workspace with the grey plane (H15) | 712,704 B at 960×544, 303,104 B at 917×125 | 188,416 B at any region (no plane) |
| game: a line taller than a strip | — | its own width × height, only then (41 of 372 linefind dev regions; the largest 533×148 = 78,884 B) |
| game: weights (resident) | H15 1,159,168 B, FL10 659,456 B | unchanged |

So a textbox takes at most 65,536 B of capture memory when all its lines fit a strip: 331 of
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
API 24 removed the kernel's region-sized capture buffer (`VjoCapture`, with `vjoRequestCapture` and
`vjoReadRaw`): Google Lens's JPEG was its last Shell user, and an Anki card's screenshot is read in
the game too, in colour strips (below). #74's refusal would have reached a full-screen JPEG too. The
kernel strip (rung 4) is now the kernel's only block for a job.

## An Anki card's screenshot (API 23)

A card's screenshot ([docs/anki.md](anki.md#the-screenshot)) is a job of the same slot
(`VJO_GAME_OCR_F_SHOT`) and reads the frame through the same path, with three differences:

- **The whole frame, in colour.** The job's region is the frame (whatever the user's region is),
  and its rows calls carry `VJO_ROWS_RGBA`: the hook copies each row as A8B8G8R8 words, the frame's
  format expanded to 8 bits per channel exactly as for grey (`kernel/capture.c` `read_row`), 4 bytes
  a pixel, into the worker's block. Only a shot reads colour, and a shot reads nothing else; it
  publishes no capture and builds no signature (no `VJO_ROWS_SIG`).
- **64 rows a frame, one pass** (API 23; with Trim black bars, below, more). The worker reads the frame once, top to bottom, 64 rows (a strip
  of 245,760 B at 960 px) a display frame, 9 frames for 544 rows, and encodes each band of 16 rows
  as it comes; there are no checksums and no second pass. A screen that changes during those 9
  frames is pictured partly before and partly after the change (a band at the seam); with the
  overlay open the game has no input and normally shows a still screen. With **Trim black bars**
  (API 32, on by default) the first pass reads the first rows before it encodes, and may read some
  of them again: 9 to 12 strips; a frame whose rows come to another picture than its first rows' is
  read again, a second pass of the picture's rows, so a shot reads at most 19 strips (4 tries with a
  JPEG too large) ([black bars](anki.md#black-bars)). When the hook cannot write
  the block, the rows come through the kernel strip, 16 rows a call (61,440 B, inside its 64 KiB).
- **Its own block.** One block of 450,560 B (`VjoGameShot`: the encoder's state, the strip and the
  JPEG, page-rounded) on the same ladder (`USER_RW`, CDRAM, phycont), after a check that the game
  keeps its 1 MiB beside it (`VJO_GAME_RESERVE`, every job's since #280; 4 MiB before); freed when the job ends,
  the resident weights untouched.

Then the JPEG goes to the memory card through the kernel's worker thread, as the models come from
it (`vjoOcrShotWrite`, `vjoOcrShotSave`; the game itself has no `ux0:`). `host/tests/test_game_shot.c`
runs it through the actual worker and kernel transport on a 960 × 544 frame in each of the three
pixel formats, and `test_kernel_game_ocr.c` the kernel's rules: colour only for a shot, the
strides and the kernel strip in colour, the file's order, size and form.

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
  the three model sizes (and, by name, VOCR3-H15 and v0.2.0's H15), with 0 B of the kernel pool, also when it refuses
  everything; each ladder rung.
- `host/tests/test_kernel_game_ocr.c`, `test_game_model_broker.c`, `test_game_vocr.c`: the rows
  syscall, the hook path, every refusal, cancellation, process exit, the ladder and the probe.
- `tools/vita3k_smoke.sh`: the real worker's five stage jobs, one job per model size and both
  other formats as a Vita homebrew app in Vita3K, crash check only.

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
