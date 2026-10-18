# Text stability: does Rubi read a line before it is whole?

**No, not on the console runs so far.** The owner asked whether Rubi needs a "text stabilisation
model": something that waits until a typewriter line has finished, ignoring an animated box or a
blinking "next" cursor. Rubi already has two checks: the kernel's scene tracker and the game
worker's stable-text wait. On the owner's CLANNAD runs they held:
- 1 of 299 pairs of consecutive reads was early, and that one was a settings menu, not dialogue;
- no read was cut short by the wait's time limit;
- re-reads that motion other than text could have caused cost 0.9 % of the game time spent reading
  (test.3 and test.4).

A model or a new rule would add size and code to every game that is ON, for nothing measured. The
pre-registered rule is in #211. Every figure here is measured from the owner's console logs or
marked est. The logs are private, so only counts are quoted.

## When a read starts, and what counts as stable

**The kernel's scene tracker** (`kernel/scene.c`) decides when Automatically reads:
- **Sampling.** The display hook builds a signature of the text region every 8th game frame
  (`VJO_CHECK_EVERY_FRAMES`): a hash of each cell of a grid of up to 128 cells, about as wide as
  tall. On CLANNAD's 852 × 135 region the grid is 25 × 5, sampling every 6th row and every 4th
  pixel. The kernel's worker compares signatures about every 64 ms (`CHECK_EVERY` polls of 16 ms).
- **Settled.** The region has *settled* after 300 ms (`SCENE_STABLE_US`) without a change outside
  the masked cells.
- **Masked cells.** A cell that keeps cycling through a few states for 2 s without player input is
  masked as an icon, at most 8 cells (`SCENE_ANIM_MAX`). Within 10 s of a mask, cycling cells are
  masked sooner.
- **Quiet.** The region is *quiet* when, for 400 ms (`SCENE_QUIET_US`), every change stayed in one
  spot of 3 × 3 cells that went back to a recent state.
- **The job.** Either event starts Automatically's background job (`shell/worker.c`
  `auto_prefetch`: `auto: screen settled` or `auto: screen quiet`), unless the last result's
  capture still shows the screen. A press that needs a job starts it at once, whatever the tracker
  says.

**The game worker's stable-text wait** (`core/vocr_ocr.c` `settle_wait`, `settle_verify`) runs in
every job, of both modes, and decides when a job may recognize:
- **The rule.** Every grey row of the region has an FNV-1a checksum. Nothing is recognized until
  every row has read the same over passes spanning `ocr_settle_ms` = 150 ms.
- **Two paths.** A job on a screen the kernel had called settled reads its first pass and finds its
  lines first. It then re-reads until a pass starting 150 ms after the first read has matched
  (`settle_verify`). A job on a screen the kernel had not called settled
  (`VJO_GAME_OCR_F_SETTLE_FIRST`) waits before its first pass (`settle_wait`), and so does a retry.
- **Icons.** A row that goes back to one of its 16 recent states (a dialogue box's ring) is taken
  for an icon, not a change, for at most 64 rows a job (`VJO_VOCR_CYCLE_ROWS`). Text that types out
  only moves on to new states.
- **Tears.** A row that changes after the wait is a tear: the job reads again, up to 3 tries
  (`VJO_VOCR_TRIES`), then fails with `VJO_E_SCREEN_CHANGED` and is retried later.
- **The limit.** A job waits at most `ocr_settle_max_ms` = 2000 ms in all, or
  `ocr_settle_press_ms` = 1000 ms for a press (since test.3), then reads anyway.

The gap this leaves is known ([auto-vs-press.md](auto-vs-press.md#what-settled-means)): a
typewriter pause longer than the window passes as settled.

## What the console runs show

**The data.** Every game window of a debug build of API 20 or later in the owner's logs:
- **The builds.** test.2 (`4ac02ac`, API 20), test.3 and the later play on its plugins (`9cfa920`,
  API 22), and test.4 (`3ce0f47`, API 24).
- **The source.** test.4's collect (`vjo-collect-20261017-112501Z`) has them all in `log.old.txt`
  and `log.txt`: each earlier collect's log is a byte prefix of the next, so each job is counted
  once.
- **The size.** 42 game windows, 411 jobs and 336 jobs with text: 299 pairs of consecutive text
  jobs in one window.
- **The game.** Every text job is CLANNAD's (`PCSG00415`), in **Automatically**. Three other titles'
  21 jobs read no text.
- **The code.** The wait's functions (`stream_check`, `stream_read`, `settle_wait`,
  `settle_verify`) and the scene tracker are the same in all three builds and on `main`, compared
  function by function.
- **Not in the data.** No collect has `history.csv` (`tools/collect_vita.py` does not fetch it), and
  **Only when I press** was never run.

| what was counted | test.2 | test.3 | test.4 | all |
|---|---:|---:|---:|---:|
| text jobs / consecutive pairs | 61 / 45 | 256 / 238 | 19 / 16 | 336 / 299 |
| **read early**: the next read's text begins with this one's and goes on | 0 | 1 | 0 | **1** (0.33 %) |
| the next read's start within 10 % edits of this one, 2 or more characters longer (also at 20 and 30 %) | 0 | 0 | 0 | 0 |
| a read 2-3 jobs later that extends one, other text between | 0 | 0 | 0 | 0 |
| jobs that reached the wait's limit | 2 | 7 | 3 | 12 |
| of those, a text the next read extended | 0 | 0 | 0 | **0** |
| the same text read again by the next text job | 6 | 28 | 2 | 36 (12.0 %) |
| of those, with the capture's checksum changed | 2 | 4 | 1 | **7** (2.3 %) |

**The committed analyser agrees.** `tools/analyse_selftest.py --collect` on the same collect gives
the first row for test.3 and test.4 (`read before finished: 1 of 254 pairs`, and `30 read the same
text again`). It leaves out API 20 windows; test.2's 45 pairs add none. The other rows come from the
job lines of the same logs: each job's `gstep job settle` and `gstep job tear` steps and its
`game vocr capture` checksum.

**The one early read was not dialogue.** It was CLANNAD's text-settings menu. The first read had the
menu's help line alone. The next had the same line and the menu's "reset to defaults" button, after
867 row changes in between. The first job's wait held after 4 passes (241 ms) without a row change,
so the screen it read had been still for longer than both windows. Whether the game drew the button
late or the player's cursor brought it back, the log cannot say. Of the 238 pairs where neither read
is a menu, title or system screen (told apart by their button words), none was early: the 95 %
upper bound is 1.54 % (Clopper-Pearson).

**The wait's limit.** 12 jobs ran out of waiting (10 background, 2 presses), and none of them
showed a partial line:
- **9 read no text.** All 3 tries were torn, with 19-192 rows changing. A later job read text: the
  next one in 8 of the 9.
- **3 read text.** Two were whole sentences. The third was a box whose full-width first line was
  read whole and whose short last line the finder dropped (#194, below), read the same by the 3 jobs
  before it.
- **Their next reads** were other lines, not extensions.

**The same text read again** (36 pairs):
- **Same pixels.** In 29 the capture's checksum did not change: the job read the same pixels again.
  15 of these 29 followed an overlay open. An input edge ends every mask the kernel holds, which
  counts as a change (`kernel/scene.h`).
- **Presses.** 7 were presses that found `open: screen changed since the background result` (6) or
  `no background result` (1). The kernel's tracker had seen a change since the last result, and the
  job then read the same text (the same pixels in 6).
- **Cost.** 27 recognized nothing, because every line came from the line cache. In test.3 and
  test.4 (test.2's log has no game time per job), the 30 such re-reads took 23.0 s of their 275 text
  jobs' 302.7 s of game time: 7.6 %. Automatically's own re-reads of the text just read were 22 jobs
  and 16.6 s of that (5.5 %).
- **Motion that was not text.** Only the 7 whose checksum changed are re-reads that it could have
  caused: in test.3 and test.4, 5 jobs and 2.8 s (0.9 %).

## What holds the reads back

**The box is still changing when Automatically asks, and it is not the typing (inferred).**
- **How often, and how long.** Of the 327 background jobs whose wait held, 203 saw rows change
  first: median 1.14 s, and 174 of them with rows going back to a recent state (an icon). The 124 on
  a still screen took a median of 0.10 s.
- **Not the line's length.** The wait barely grows with the text: medians of 1.09, 1.11 and 1.16 s
  for texts of 0-9, 10-19 and 20 or more characters (Spearman 0.10, 190 jobs).
- **Why it is not the typing.** These jobs start only once the kernel saw 300 ms without a change
  outside its masked cells (settled), or 400 ms of changes in one spot of 3 × 3 cells (quiet). So what
  the worker waits out is a masked icon or that spot, or something else in CLANNAD's box that
  animates for about a second after a new line, as
  [first-ocr.md](first-ocr.md#each-cost-and-its-cause) found in test.2.
- **Not a fault.** The worker's rows cannot see the kernel's mask, so the wait absorbs it: that
  costs time, not correctness.

**Tears look like that icon, not text (inferred).** 137 jobs were torn and read again. Their torn
tries changed a median of 19 rows, 69 % of them 24 rows or fewer, against a median text-line height
of 38 px (lines at least 300 px wide). A pass of the wait took a median of 57 ms in test.3's build:
three strips, a game frame each.

**Mid-sentence reads are the finder, not early reads.** 13 dialogue reads (9 screens) end
mid-sentence. Each has one line box the full width of the region (843-850 px), with 26 or 27
characters. 12 of their waits held, and one reached a press's limit:
- **The two screens read more than once** (6 reads) read the same text each time.
- **No later read extended any of the 9.**
- **#194.** The owner's test.4 report names 5 of these screens: a box's short last line, beside a
  full-width one, dropped by the line finder. Its offline repro drops the line on a still
  screenshot.
- **Fixed.** `main` fixed it after these builds with the textbox recovery finder (`6674125`,
  `49310ed`).

## The replay

No frame sequence of a real typewriter exists: the owner's captures are stills taken seconds
apart. So the replay is the host's simulated screen (est.: the console's frame pacing differs).
Text types one character every 3 frames at 60 fps, the job is asked with 12 of 25 characters
shown, and the typing resumes after a pause:

| region | the job's path | read whole up to a pause of | read with 12 of 25 from |
|---|---|---:|---:|
| 917 × 125 (`vjo-settle-bench 150`) | the kernel had called it settled | 8 frames (133 ms) | 24 frames (400 ms) |
| 852 × 135 (CLANNAD's), the bench's `run()` with the pause in 2-frame steps | the kernel had called it settled | 200 ms | 233 ms |
| 852 × 135, the same | not settled (`settle_first`) | 366 ms | 400 ms |

**What a pause has to beat.**
- **Automatically.** Its job starts only after the kernel has seen 300 ms without a change, more with
  a signature every 8th frame. So a line is read early only when the game stops typing mid-line for
  about 0.55 s or more (est.: those 300 ms, then the 233 ms of the table's second row).
- **A press** has only the job's own window from the moment of the press. With **Only when I
  press**, a press during such a pause shows the part read, and nothing reads it again until the
  overlay is reopened ([auto-vs-press.md](auto-vs-press.md#what-settled-means)).

No dialogue read in these runs was early. So CLANNAD's typewriter never paused that long while a
job read it (inferred).

## Why no fix

**The rule.** #211's pre-registered rule called the problem real at any of:
- 1 % early reads with at least 3 events;
- 1 % of text jobs extended after the limit;
- 10 % checksum-changed re-reads costing 10 % of the game time.

The runs gave 1 event (0.33 %), 0, and 2.3 % costing 0.9 %.

**The cheap rules would change no read.** None of the cheap rules #211 lists would change a measured
read:
- **Hashing only the rows inside the box.** The worker already hashes only the region's rows.
- **Leaving the cursor out.** The kernel masks it, and the worker's ring takes it for an icon.
- **Accepting a read only when the next one does not extend it.** That would delay every read by a
  second capture.

**A model would not help either.** The wait's cost is the box's own animation, which a learned model
would have to wait out too.

**A speed-up, not a fix (est.).** The worker's wait could skip the cell the kernel masks. That could
take the 203 background waits that saw changes (median 1.14 s) towards the 0.10 s of a still screen.
It would have shortened the presses that found a background job still running: 45 of the 140 opens
in these runs. It needs the mask passed through the kernel's API, and a console run, so it is out of
scope here.

## What would change this

- **Another game.** Every measured text is CLANNAD's. A game whose typewriter stops mid-line for
  half a second or more (a dramatic beat, a voice cue), or whose box animates for longer than
  `ocr_settle_max_ms`, could read lines early or at the limit.
- **Only when I press.** It has no console run, and it gets only the job's own 150 ms window.
- **To check:** a session of each mode in such a game with the debug build, then
  `tools/install_ftp.sh --collect`. `tools/analyse_selftest.py --collect` prints `read before
  finished`, the first row above, and the job lines give the rest.
- **If early reads reach the rule's 1 % with 3 or more events,** the cheapest fixes come first:
  - raise `ocr_settle_ms` for that title;
  - let **Only when I press** refresh the open overlay's text once the screen settles again, as
    Automatically already does (`background_open`).
