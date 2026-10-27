# Pause the game to read (`ocr_pause`, API 52, #348)

**What it does.** With the Rubi app's **Pause the game to read** ON (`ocr_pause = on` in `config.ini`; OFF by
default), each reading copies the text box from one frame of the game, at once, inside that frame's display call,
which the game waits in for as long. The OCR then reads that still copy. A spinner, a blinking mark or an animated
background in the text box cannot change the rows while they are read, so the job no longer gives up with the
screen changed under it and shows nothing.

**What it costs.** For a CLANNAD-sized box the game's display call waits an estimated 2.4-6.4 ms at 333 MHz, once
per reading. For a whole 960 × 544 screen it waits 10.8-29.0 ms. For a frame in A2B10G10R10 or A1B5G5R5 it waits
about 1.8 times as long. These are estimates: the console's figure is unmeasured. The wait can never last longer than
100 ms, and the game is not paused while the text is recognized.

Not run on a console yet. Everything here is from the source, the host tests and the host bench.

## Why (#348)

A user asked for a pause "like kotoba3ds", because their reads showed nothing although the text lines log had the
text. The owner traced it to the spinning wait icon inside the region. Rubi reads a region strip by strip, a frame per
strip, and checks each row's checksum every time it reads it again. A row that comes back to one of its last 16
states counts as an icon ([stable text](vita-vn-ocr.md#stable-text-and-blank-lines-api-20)). A spinner with more
shades than that, or one that turns at a varying speed, is a change at every read. The job reads again, three times,
then fails with `VJO_E_SCREEN_CHANGED`. Nothing is shown, and no message says why.

`host/pause_bench.c` runs the real job, with vita-vn-ocr's real line finder, on simulated screens. Without weights a
job stops where it would recognize (rc -115), so a -115 is a read that worked. Each strip is a frame at 60 fps.

| 852 × 135 box, Automatically's job | rc | display calls | rows copied | ms to the recognizer |
|---|---:|---:|---:|---:|
| still, strip by strip | -115 | 10 | 583 | 200 |
| still, paused | -115 | 1 | 135 | 16 |
| spinner of 20 shades, one a frame, strip by strip | **-120** | 129 | 8,142 | 2,150 (then nothing) |
| the same, paused | -115 | 1 | 135 | 16 |
| spinner, 1-4 frames a shade, strip by strip | **-120** | 130 | 8,206 | 2,166 |
| the same, paused | -115 | 1 | 135 | 16 |
| blinking next mark, strip by strip | -115 | 27 | 1,614 | 483 |
| the same, paused | -115 | 1 | 135 | 16 |
| spinner, one shade a frame, asked while not settled, strip by strip | **-120** | 129 | 8,142 | 2,150 (then nothing) |
| the same, paused | -115 | 121 | 7,815 | 2,016 |

The last two rows are a job asked while the screen was still changing (`F_SETTLE_FIRST`). It waits on the live
screen before its first pass, as before. A spinner that never comes back to a shade it showed never lets that wait
end, so it spends the wait's whole budget, `ocr_settle_max_ms` (2 s; a press, `ocr_settle_press_ms`, 1 s), and then
copies its frame. Paused, the text is shown that much later. Strip by strip it is never shown.

`host/tests/test_alloc_sim.c` gives the same result with the real game worker, the real kernel transport, the
display hook's copy and the release models (the gate's models lane), on a 917 × 126 box with a spinner. Strip by
strip, rc -120, 72 display calls and nothing shown. Paused, rc 0: one display call of 126 rows, 3 lines read.

## How it works

1. **The request.** The Shell sets `VJO_GAME_OCR_S_PAUSE` (bit 30) in each of the user's jobs when `ocr_pause = on`
   (`shell/worker.c`). The self-test's stages never set it.
2. **The frame block.** The game worker counts a block of the region's size (width × height grey bytes, at most
   522,240 B) in the job's memory check. It takes the block only when it fits beside everything the check chose,
   with that check's reserve. It never steps down a model size, the LM or the cascade for it. In a game short of
   memory it takes it before the helper threads. The block uses the ladder (`USER_RW`, CDRAM, phycont) and is freed
   with the job.
3. **The copy.** At the first pass, the worker asks for the whole region at once (`vjoOcrCaptureRows` with
   `VJO_ROWS_FRAME | VJO_ROWS_SIG | VJO_ROWS_LAST`, row 0, every row).
   - The next frame the game submits, the display hook copies every row of the region from it into the block, inside
     that display call (`kernel/game_ocr.c` `game_ocr_rows_hook`). It does this 16 rows at a time and looks at the
     clock between them.
   - Its stop is `VJO_GAME_OCR_PAUSE_MAX_US`, 100 ms. Past it the copy ends with `VJO_ERR_TIMEOUT`, and the display
     call returns.
   - The copy is the first pass. It is published as any first pass is (the capture's scene signature,
     `done_seq`), so the Shell ties the text to the screen it came from.
4. **The read.** Every later read of the job comes from the block, not from a frame (`game_ocr/main.c`
   `strip_source`). The core sees the region frozen (`VjoGreySource.frozen`, `core/vocr_ocr.c`):
   - it forgets the rows a wait read before the copy, so the frozen first pass is the reference;
   - it skips what is left of the stable-text wait (`settle_verify`), since nothing can change;
   - it skips the second read of a region that fits in one strip;
   - it reads a line taller than a strip in place, from the block, the same bytes a whole-region copy gave API 18's
     recognizer. No tall-line block is needed.

   A job asked on a screen still changing (`F_SETTLE_FIRST`) still waits on the live screen first, as before, then
   copies its frame. With `ocr_vertical = auto`, a region whose rows look like vertical text is read again as
   columns, a second try with its own blocks: it copies its frame again, so that job makes the game wait twice (the
   step's us is the sum).
5. **Read as before.** The job reads strip by strip, exactly as with the setting OFF, when:
   - the kernel refuses the copy or cannot finish it;
   - the block does not fit;
   - the game's frames come from its pad calls (the PSP emulator under Adrenaline, `VJO_GAME_STATIC_FB`: refused with
     `VJO_ERR_UNSUPPORTED`).

   In each case the block goes back to the game before the job reads on, so a job never holds the frame block and a
   tall line's block together.

The step log has one `pause` step per job that asked for it (`VJO_GSTEP_PAUSE`):
- kind 1 when the job read one frame;
- arg the rows copied in that one call;
- rc the refusal (UNSUPPORTED, TIMEOUT, COPY; `VJO_E_OOM` for no room);
- sum `VJO_GPAUSE_*`: frozen, no room, refused or unused;
- us the game's wait.

The debug build's kernel.txt has `the frame's N rows in one display call, T us`.

## Every way a copy ends

Nothing is held outside the game's own display call: the "pause" is the copy, in the game's thread, and it ends when
that function returns. `host/tests/test_kernel_game_ocr.c` runs each path through the real kernel code:

| how it ends | the game's display call | the job |
|---|---|---|
| every row copied | returns after the copy (`info.copy_us`) | reads the block; its first pass is published |
| the cap, 100 ms | returns at the next 16-row look at the clock: at most the cap and one chunk | reads strip by strip; nothing published |
| the hook cannot write the block | returns at once (`VJO_ERR_COPY`) | strip by strip |
| a frame of another size | returns at once (`VJO_ERR_FORMAT`) | strip by strip |
| no frame within 1.5 s | not called | strip by strip (`VJO_ERR_TIMEOUT`) |
| the job cancelled before a frame came | not called | ends cancelled, the block freed |
| the game exits right after the frame | returned already; the call tidies the state | ends |
| a PSP game (`VJO_GAME_STATIC_FB`), or the watchdog's switch | not called | strip by strip (`VJO_ERR_UNSUPPORTED`) |
| a copy that outlives its call (2 s) | still copying: nothing outside the game's thread may stop it | ends; the block is never freed, the worker takes no more jobs, as for a capture block |

**The watchdog.** The kernel worker's tick runs every 16 ms (`kernel/game_ocr.c` `game_ocr_rows_tick`). If a frame
copy has run for more than twice its cap (`PAUSE_WATCHDOG_US`, 200 ms), it turns pausing off until the next boot,
logged in kernel.txt (`ocr pause: ...`). Every later frame copy is then refused and jobs read strip by strip. The
copy's own cap should never let this happen: only a row read that itself stalls would. A copy that is only slow, an
ordinary strip's copy and an idle state never trip it (tested).

## What was studied

**kotoba3ds** ([icebreaker2718/kotoba-3ds](https://github.com/icebreaker2718/kotoba-3ds) at `e8199603`, a Luma3DS
plugin built on CTRPluginFramework):
- `plugin/Sources/main.cpp:1216-1217`: a hotkey calls `Process::Pause()` before it reads the screens.
- `:1200-1213` `ResumeGame`: `Process::Play()`, again up to 8 times while `Process::IsPaused()`.
- `:383-398`: the lid is watched, because "the paused game can't answer the system's sleep query (that takes its main
  thread, which the pause holds)".
- `:1187-1197`: it waits at most 3 s for the buttons to be let go before the game goes on.
- The pause lasts as long as the lookup is open, with no time limit.

**CTRPluginFramework** ([thepixellizeross/ctrpluginframework](https://gitlab.com/thepixellizeross/ctrpluginframework)
at `96fd3d94`):
- `ProcessImpl.cpp:142-185`: `Pause` and `Play` count nested pauses.
- `OSDImpl__Sync.cpp:111-139` `PauseFrame`: the game's own frame-present hook (`OSDImpl.cpp:257`) blocks the game's
  thread there.
- `:145-175` `WaitFramePaused`: the plugin's thread waits for that, in 50 ms steps.
- `ProcessImpl.cpp:256-270` `LockGameThreads`: Luma's `svcControlProcess(PROCESSOP_SCHEDULE_THREADS)` stops every
  other thread of the game but the plugin's (told apart by a TLS magic).
- `OSDImpl__Sync.cpp:177-215` `ResumeFrame`: it unlocks them and releases the frame.

The 3DS has a kernel service made for this. The Vita has none: what it has are debugger calls.

**What a Vita plugin could do** (VitaSDK headers in `vitasdk/vitasdk:2026.08`, the gate's image):

| mechanism | used by (source read) | what can go wrong | taken? |
|---|---|---|---|
| suspend the whole process: `ksceKernelSuspendProcess(pid, 0x1C)`, `ksceKernelResumeProcess` (NIDs 0x6AECE4CD / 0x080CDC59 on 3.60, 0x93B8E785 / 0x6894499C on 3.63+) | kvdb `2ab4bb2` `src/debugger.cpp:179`, divasplit `8c05df1` `prof/src/main.c:7`, psvita-gdb `56db1fd` `vita-gdb/src/kernel/utils.rs:274`, Project-Pebble `511d4ed` `kernel/src/main.c:246` | every thread of the game, audio included, stops, and our own worker with them, since it runs in the game: the worker could not resume the game it suspended, so a kernel thread would have to. A missed resume leaves a frozen game. Different NIDs before and after 3.63. | no |
| debug-suspend the game's threads: `ksceKernelGetThreadIdList`, `ksceKernelDebugSuspendThread` / `DebugResumeThread` | Project-Pebble `gui.c:1233`, `:1247` (the main thread while its GUI is open); VitaDebugger `c01a386` `kernel/src/main.c:796-900` (every thread but the caller's, a 250-5000 ms lease, a watchdog thread that resumes at the deadline, `:342`, and a recovery with `ksceKernelChangeThreadSuspendStatus`, `:260-340`) | vita-trace `c0995c5` `kernel/sampler_debug_fallback.c:53-60`: a thread suspended in the middle of a syscall "hangs the process with no crash dump", confirmed on hardware. Suspending the game's audio thread starves its buffer, a held lock deadlocks the threads left running, and a game timing itself on the system clock skips ahead. | no |
| hold the game's display call for the whole read (kotoba3ds' scope): wait in the display hook until the recognition ends, at most 1.5 s | (none on the Vita; reVita `f35adc5` `reVita/src/main.c:400-437` locks a mutex inside the same hook but releases it before `TAI_CONTINUE`; PSVshell `9a47b0e` `src/main.c:64-95` and PSVshellPlus `b70f1c7` `PSVshellPlus_Kernel/main.c:42-53` hold nothing) | the game's display thread, often GXM's display-queue callback, waits up to 1.5 s: the queue fills and the game's render thread stops on it, every line, while **Read text: Automatically** reads in the background. The OCR gains nothing from it: it reads a copy. A cross-thread wait needs the worker, at the lowest priority, to be scheduled to end it, and a watchdog to end it when it is not. | no |
| **copy the region in one display call** (this) | the display hook's own copy (`kernel/display.c`, `kernel/capture.c`), which reads at most 64 rows a frame today | the display call waits as long as the copy, an estimated 2.4-6.4 ms for a dialogue box at 333 MHz (est.), once per reading; no thread but that one waits, and it waits for nothing but its own work | **yes** |
| the PSP emulator's own pause, `ScePspemuPausePops` | Adrenaline `7d382b7` `user/menu.c:179`, `:193` (a function inside ScePspemu at a fixed offset, `user/main.c:122-127`) | only for PSP games, an address per firmware build of ScePspemu | no (PSP games read as before) |

No kernel or system thread is ever paused. No game thread is suspended, and nothing waits for another thread.

## Risks that remain

- **A longer frame, once per reading.** The display call waits the copy's time (estimated above). At 60 fps a
  dialogue box's copy fits in one frame time. A whole screen's may take one or two frames. A game that times its
  frames sees one slow frame, as when a strip is copied today, only longer. Today's job copies each row about four
  times over nine or so frames (the bench's still box: 583 rows in 10 calls). A paused one copies each row once in
  one call, so the game loses less time in all.
- **Audio, watchdogs, the clock.** No thread but the game's display thread waits, and only for the copy. An audio
  thread keeps running, and no system watchdog is near 100 ms (inferred: there is no console measurement).
- **Text read before it is whole.** The stable-text wait after the first pass is skipped. A job the kernel calls
  settled reads its frame at once, about 0.18 s sooner than strip by strip (still box: 16 ms against 200 ms to the
  recognizer). A typewriter that pauses longer than the kernel's settled window (300 ms) is then read as typed so far,
  where strip by strip it needed about 0.5 s (est., [text stability](text-stability.md#the-replay)). A job on a screen
  still changing waits as before.
- **A spinner the kernel cannot mask.** The kernel's tracker masks a cell that cycles through its recent states
  ([text stability](text-stability.md#when-a-read-starts-and-what-counts-as-stable)). A spinner that never returns
  to a state keeps the screen unsettled. Each job then waits out its budget on the live screen (above) before it
  copies its frame: an estimated 2 s with **Read text: Automatically**, 1 s for a press. Skipping that wait for a
  paused job would read a line still typing out, so this PR keeps it.
- **The screen is not frozen while it is recognized.** Only the copy is still. The game goes on during recognition,
  as it does with the setting OFF.
- **A game that draws into the buffer it shows.** A single-buffered game could change its frame while its own
  display call copies it. That is a race of a few ms against the strips' 150 ms or more today (inferred).
- **PSP games.** They read as before (above).
- **The frame block.** It takes 4-512 KiB of the game's memory per job (below), only when it fits.

## Memory

`VjoGameVocrFrame` is width × height bytes, page-rounded: 118,784 B for an 852 × 135 box, 524,288 B at
960 × 544. `VJO_BUDGET_GAME_FRAME`, 524,288 B, is in `include/vjo_ledger_budget.h`. A job holds this block or a tall
line's (`VJO_BUDGET_GAME_LINE`, 516,096 B), never both, so the game's worst case counts the larger
(`VJO_BUDGET_GAME_LINE_OR_FRAME`). That is 8,192 B more: 10,412,032 B against the 10,604,544 B limit
([memory ledger](memory-ledger.md)). The kernel takes nothing more: no block, no thread, no stack. SceShell's copies of the parsed settings
grow by 4 B each, and its request by a bit.

## The game's wait per reading (estimates)

`vjo-pause-bench` prints these from the rows of each job's longest display call. It multiplies them by the display
hook's cost per pixel, taken from [performance.md](performance.md)'s `capture_abgr_sig` row (A8B8G8R8 with the
capture's signature) and `capture_a2b10` row. Those rows are the Vita compiler's instructions counted under qemu-arm,
with the A9 cost model's range. Not measured on a console:

| region | longest display call, strip by strip | paused | wait at 444 MHz | at 333 MHz | A2B10G10R10, 333 MHz |
|---|---:|---:|---:|---:|---:|
| 852 × 135 (CLANNAD's box) | 64 rows | 135 rows | 1.8-4.8 ms | 2.4-6.4 ms | 4.4-10.4 ms |
| 917 × 126 | 62 rows | 126 rows | 1.8-4.8 ms | 2.4-6.4 ms | 4.4-10.5 ms |
| 960 × 544 (the whole screen) | 59 rows | 544 rows | 8.1-21.7 ms | 10.8-29.0 ms | 20.0-47.3 ms |

A console run measures it: the debug build's `gstep pause` step (its us) and kernel.txt's line, per job.

## Tests

- **Kernel transport** (`host/tests/test_kernel_game_ocr.c`, the real `kernel/game_ocr.c`):
  - one display call copies and publishes the region;
  - every other shape is refused, and so are a job without the bit, a probe and a static-framebuffer game;
  - the cap stops the copy within one chunk, and the job then reads strip by strip;
  - every other end (copy error, another size, no frame, cancelled, process exit) returns the display call;
  - the watchdog's threshold, its off switch, and a stalled copy given up and tidied.
- **Core** (`host/tests/test_vocr_policy.c`, real finder):
  - a frozen region that never holds still is read once without waiting, where strip by strip gives -120;
  - a frozen first pass forgets the wait before it;
  - a refused freeze reads exactly as before (the same strips, flags and frames);
  - a tall line is read in place.
- **Game worker** (`host/tests/test_game_resident.c`, synthetic weights):
  - one frame call beside a spinner, the block freed and the ledger back to 0;
  - a refusal (UNSUPPORTED, TIMEOUT, COPY) frees the block before any strip;
  - no room, or a refused allocation, keeps the model;
  - a cancelled copy, and one that outlived its call.
- **End to end** (`host/tests/test_alloc_sim.c`): above; with the release models in the gate's models lane.
- **Shell, config, app**: the bit only with `ocr_pause = on` and never in the self-test's stages (`test_shell_worker.c`);
  the key's parse and template (`test_core.c`); the Rubi app's card by keys and touch (`test_settings_app.c`).

## What a console run must check

- With the setting ON, in a game with an animated icon in the text box: a line is shown, and the job's
  `gstep pause` step says kind 1. How long after the line it is shown (about 2 s, or 1 s for a press, when the
  icon keeps the screen unsettled: the job's `gstep job settle` line has `rc=0x00000002`, the wait's limit).
- The game's wait per reading, from the step's us and kernel.txt's line. Does the game stutter?
- With it ON in CLANNAD: text as before, and lines not read before they are whole.
- With it OFF: nothing changes (no `pause` step).
