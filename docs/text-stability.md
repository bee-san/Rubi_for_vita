# Text stability: does Rubi read a line before it is whole?

**No, not on the console runs so far.** The owner asked whether Rubi needs a "text stabilisation
model": something that waits until a typewriter line has finished, ignoring an animated box or a
blinking "next" cursor. Rubi already has two checks: the kernel's scene tracker and the game
worker's stable-text wait. On the owner's CLANNAD runs they held:
- 2 of 299 pairs of consecutive reads count as early by the rule, and neither is a dialogue line
  read before it had finished: one is a settings menu, the other a 1-character misread;
- no read was cut short by the wait's time limit;
- re-reads of text already read cost 7.6 % of the game time spent reading (test.3 and test.4). For
  those whose captured pixels changed, the rule's measure, it was 0.9 %.

A model or a new rule would add size and code to every game that is ON, for nothing measured. The
decision rule was set in #211 before this measurement. The committed analyser's headline counts,
seen while orienting, are disclosed there. Every figure here is measured from the owner's
console logs or marked est. The logs hold the owner's reading, so only counts are quoted.

## When a read starts, and what counts as stable

**The kernel's scene tracker** (`kernel/scene.c`) decides when Automatically reads:
- **Sampling.** The display hook builds a signature of the text region every 8th game frame
  (`VJO_CHECK_EVERY_FRAMES`). It is a hash of each cell of a grid of up to 128 cells, about as wide
  as tall. On CLANNAD's 852 × 135 region the grid is 25 × 5, sampling every 6th row and every 4th
  pixel. The kernel's worker compares signatures about every 64 ms (`CHECK_EVERY` polls of 16 ms).
- **Settled.** The region has *settled* after 300 ms (`SCENE_STABLE_US`) without a change outside
  the masked cells, once no unmasked cell is starting to cycle.
- **Masks.** A cell that keeps cycling through a few states for 2 s without player input is masked
  as an icon, sooner within 10 s of a mask. At most 8 icon cells are masked (`SCENE_ANIM_MAX`), plus
  up to 8 cells at their edges, and none when more cells than that animate (an animated background).
- **Quiet.** The region is *quiet* when, for 400 ms (`SCENE_QUIET_US`), every change stayed in one
  spot of 3 × 3 cells that went back to a recent state.
- **The job.** Either event starts Automatically's background job (`shell/worker.c`
  `auto_prefetch`: `auto: screen settled` or `auto: screen quiet`), unless the last result's
  capture still shows the screen. A press with no result for its screen starts a job at once,
  whatever the tracker says. If a background job is still running, the press waits for it instead.

**The game worker's stable-text wait** (`core/vocr_ocr.c` `settle_wait`, `settle_verify`) decides
when a job may recognize, in every job of both modes:
- **The rule.** Every grey row of the region has an FNV-1a checksum. Nothing is recognized until
  every row has read the same over passes spanning `ocr_settle_ms` = 150 ms.
- **Two paths.**
  - **A screen the kernel had called settled.** The job reads its first pass and finds its lines
    first. Then it re-reads until a pass starting 150 ms after the first read has matched
    (`settle_verify`).
  - **A screen it had not** (`VJO_GAME_OCR_F_SETTLE_FIRST`). The job waits before its first pass
    (`settle_wait`), and so does a retry.
- **Icons.** A row that goes back to one of its 16 recent states (a dialogue box's ring) is taken
  for an icon, not a change, for at most 64 rows a job (`VJO_VOCR_CYCLE_ROWS`). Text that types out
  only moves on to new states.
- **Tears.** A row that changes after the wait is a tear. The job reads again, up to 3 tries
  (`VJO_VOCR_TRIES`), then fails with `VJO_E_SCREEN_CHANGED` and is retried later.
- **The limit.** A job waits at most `ocr_settle_max_ms` = 2000 ms over all its tries, or
  `ocr_settle_press_ms` = 1000 ms for a press (since test.3). Then it reads anyway.
- **The pace (API 29).** A job nobody waits for reads its wait's passes at most every
  `ocr_settle_pace_ms` = 150 ms. It never ends the wait sooner than back to back, so the windows
  below are unchanged (host bench, [stutter.md](stutter.md)).

The gap this leaves is known ([auto-vs-press.md](auto-vs-press.md#what-settled-means)): a
typewriter pause longer than the window passes as settled.

## What the console runs show

**The data.** Every game window of a debug build of API 20 or later in the owner's logs:
- **Builds.** Three sets of plugins: test.2's (`4ac02ac`, API 20) and test.3's (`9cfa920`, API 22),
  each in its test run and in the play after it, and test.4's (`3ce0f47`, API 24). The play after
  the test runs is 18 of test.2's 20 windows and 12 of test.3's 19.
- **Source.** test.4's collect (`vjo-collect-20261017-112501Z`) holds them all in `log.old.txt` and
  `log.txt`. Each earlier collect's log is a byte prefix of the next, so each job is counted once.
- **Size.** 42 game windows, 411 jobs and 336 jobs with text. That gives 299 pairs of consecutive
  text jobs in one window.
- **Game and mode.** Every text job is CLANNAD's (`PCSG00415`), in **Automatically**. Three other
  titles' 21 jobs read no text.
- **Code.** The wait's functions (`stream_check`, `stream_read`, `settle_wait`, `settle_verify`) and
  the scene tracker are the same in all three builds and on `main`, compared function by function.
- **Not in the data.** No collect has `history.csv` (`tools/collect_vita.py` does not fetch it).
  **Only when I press** was never run on a build with the stable-text wait: its 60 windows are of
  APIs 8-13.

| what was counted (the text as shown) | test.2 | test.3 | test.4 | all |
|---|---:|---:|---:|---:|
| text jobs / consecutive pairs | 61 / 45 | 256 / 238 | 19 / 16 | 336 / 299 |
| **read early**: the next read's text begins with this one's and goes on | 0 | 1 | 0 | **1** (0.33 %) |
| near: the next read's start within max(1, 10 %) edits of this one, 2 or more characters longer (the same at 20 and 30 %) | 0 | 1\* | 0 | 1\* |
| early by #211's rule (the two rows above) | 0 | 2 (0.84 %) | 0 | **2** (0.67 %) |
| a read 2-3 jobs later that extends one, other text between | 0 | 1\* | 0 | 1\* |
| jobs that spent the wait's whole budget | 9 | 9 | 3 | 21 |
| of those, a text the next read extended | 0 | 0 | 0 | **0** |
| the same text read again by the next text job | 6 | 28 | 2 | 36 (12.0 %) |
| of those, with the capture's checksum changed | 2 | 4 | 1 | **7** (2.3 %) |

\* The same 1-character misread (a 267 × 95 box read at low confidence) in both rows. The next read
is a 31-character sentence that does not begin with it, and the one after that has 8 characters.
With max(1, 10 %) edits, any text 2 or more characters longer is "near" a read of one character.

**The committed analyser agrees.** `tools/analyse_selftest.py --collect` on the same collect gives
the read-early row for test.3 and test.4: `read before finished: 1 of 254 pairs`, and `30 read the
same text again`. It leaves out API 20 windows, and test.2's 45 pairs add none. The other rows come
from the job lines of the same logs: each job's `gstep job settle` and `gstep job tear` steps, and
its `game vocr capture` checksum.

**The one strict early read was not dialogue.** It was CLANNAD's text-settings menu:
- **The pair.** The first read had the menu's help line alone. The next had the same line and the
  menu's "reset to defaults" button, after 867 row changes in between.
- **The wait.** The first job's wait held after 4 passes (241 ms) without a row change. So the screen
  it read had been still for longer than both windows.
- **The cause.** Whether the game drew the button late or the player's cursor brought it back, the
  log cannot say.

**No dialogue was read early.** Take the 238 pairs where neither read is a menu, the title screen or
a system screen, told apart by the menus' button words and the title logo. None was a line read
before it had finished. The rule counts only the 1-character misread above. For 0 of 238, the
two-sided 95 % interval's upper end is 1.54 % (one-sided: 1.25 %).

**The wait's limit.** 21 jobs spent the wait's whole budget: 19 background jobs and 2 presses.
- **How the step logs it.** The step logs `limit` only when no try's wait held: `stats.settled` is
  the job's, set by any try's wait (`core/vocr_ocr.c` `settle_wait` or `settle_verify`). So 12 say
  `limit` and 9 say `held`. Those 9 held in at least one try, were torn, and spent the rest of the
  budget on their other tries (2 or 3 tries in all).
- **None showed a partial line:**
  - **15 read no text.** All 3 tries were torn, with 19-192 rows changing. A later job read text in
    each; the next job did in 14.
  - **6 read text.** 5 were whole sentences. The sixth was a box whose full-width first line was
    read whole and whose short last line the finder dropped (#194, below), read the same by the 3
    jobs before it.
- **Extended.** The next read extended none of the 6 (one had no next read in its window).

**The same text read again (36 pairs):**
- **Same pixels.** In 29 of them the capture's checksum did not change, so both captures saw the
  same rows. Something had changed between the two jobs, for the kernel's tracker (a new scene) or
  for the worker (a torn capture), yet the rows read the same at both captures.
  - The log cannot say what moved. An icon cell's mask ending is one way the code allows it
    (inferred).
  - 15 of the 29 had the overlay opened between the two jobs: 6 presses that found the screen
    changed since the background result, 5 refreshes of the open overlay, 3 jobs the open overlay
    started again (2 for a result of an earlier screen, 1 after a job cancelled for a newer screen),
    and 1 Automatically job after it closed.
- **Presses.** 7 of the 36 were presses.
  - 6 found `open: screen changed since the background result`: the kernel's tracker had seen a
    change since the last result, and the job read the same pixels again.
  - 1 found `no background result`, and its capture's checksum changed.
- **Cost.** 27 recognized nothing: every line came from the line cache.
  - test.3 and test.4 (test.2's log has no game time per job): the 30 such re-reads took 23.0 s of
    their 275 text jobs' 302.7 s of game time, 7.6 %.
  - Automatically's own re-reads of the text just read were 22 jobs and 16.6 s of that (5.5 %), and
    26 jobs over all three builds.
- **The rule's measure.** #211 counted only the re-reads whose capture's checksum changed: 7 pairs
  (2.3 %). In test.3 and test.4 that is 5 jobs and 2.8 s (0.9 %).
  - Counting every re-read of the same text, they are 36 of 299 pairs (12.0 %), above the rule's
    10 % of pairs.
  - But they took 7.6 % of the game time, under its 10 %, and the rule needs both.

## What holds the reads back

**The box is still changing when Automatically asks, and it is not the typing (inferred):**
- **How often.** Of the 327 background jobs whose wait held, 203 saw rows change first. Their median
  wait was 1.14 s, and 174 of them had rows going back to a recent state (an icon). The 124 on a
  still screen took a median of 0.10 s.
- **Not the line's length.** The wait barely grows with the text: medians of 1.09, 1.11 and 1.16 s
  for texts of 0-9, 10-19 and 20 or more characters (Spearman 0.11, 190 jobs).
- **Not the typing.** These jobs start only once the kernel saw 300 ms without a change outside its
  masked cells (settled), or 400 ms of changes in one spot of 3 × 3 cells (quiet). So what the
  worker waits out is a masked icon or that spot, or something else in CLANNAD's box that animates
  for about a second after a new line. [first-ocr.md](first-ocr.md#each-cost-and-its-cause) found
  the same in test.2.
- **Not a fault.** The worker's rows cannot see the kernel's mask, so the wait absorbs it. That
  costs time, not correctness.

**Tears look like that icon, not text (inferred):**
- 137 jobs were torn and read again.
- Their torn tries changed a median of 19 rows (69 % of them 24 rows or fewer), against a median
  text-line height of 38 px (lines at least 300 px wide).
- A pass of the wait took a median of 57 ms in test.3's build: three strips, a game frame each.

**Mid-sentence reads come from the finder, not from early reads.** 13 dialogue reads (9 screens) end
mid-sentence. Each has one line box the full width of the region (843-850 px), with 26 or 27
characters. 12 of their waits held, and one reached a press's limit. The finder explains them:
- **#194.** The owner's test.4 report names 5 of these screens: a box's short last line, beside a
  full-width one, dropped by the line finder. Its offline repro drops the line on a still
  screenshot.
- **The other 4 (inferred).** No later read extended any of the 9. The two screens read more than
  once (6 reads) read the same text each time.
- **Fixed.** `main` fixed the drop after these builds with the textbox recovery finder (`6674125`,
  `49310ed`).

## The replay

No frame sequence of a real typewriter exists: the owner's captures are stills taken seconds
apart. So the replay is the host's simulated screen (est.: the console's frame pacing differs):
- **The typing.** Text types one character every 3 frames.
- **The question.** The job is asked with 12 of 25 characters shown, and the typing resumes after a
  pause.
- **The sweep.** The bench's `run()` (`host/settle_bench.c`), swept in 1-frame steps:
  `run(name, w, h, fps, 150, first, 0, 0, P)` for P = 1 to 40 frames. CLANNAD's 852 × 135 region
  and the bench's 917 × 125 give the same frames. The bench's own
  `vjo-settle-bench 150` has two points of the first row at 60 fps: 8 frames read whole, 24 frames
  read as 12 of 25.

| the job's path | 60 fps: read whole up to | 60 fps: 12 of 25 from | 30 fps: whole up to | 30 fps: 12 of 25 from |
|---|---:|---:|---:|---:|
| the kernel had called it settled | 12 frames (200 ms) | 13 frames (217 ms) | 10 frames (333 ms) | 11 frames (367 ms) |
| not settled (`settle_first`) | 22 frames (367 ms) | 23 frames (383 ms) | 16 frames (533 ms) | 17 frames (567 ms) |

**What a pause has to beat:**
- **In Automatically.** Its job starts only after the kernel has seen 300 ms without a change, more
  with a signature every 8th frame. So a line is read early only when the game stops typing
  mid-line for about 0.5 s or more at 60 fps (est.: those 300 ms, then the 217 ms of the first row).
- **On a press.** A press has only the job's own window, from the moment of the press.
  - With **Only when I press**, a press during such a pause shows the part read. Nothing reads it
    again until the overlay is reopened ([auto-vs-press.md](auto-vs-press.md#what-settled-means)).

No dialogue read in these runs was early. So CLANNAD's typewriter never paused that long while a
job read it (inferred).

## Why no fix

**The rule.** #211's pre-registered rule called the problem real at any of:
- **Early reads:** 1 % early reads with at least 3 events, or 2 % in one build with at least 3;
- **The limit:** 1 % of text jobs extended after the limit;
- **Re-reads:** 10 % checksum-changed re-reads costing 10 % of the game time.

**The runs gave:**
- **Early reads:** 2 events (0.67 %; test.3: 2 of 238, 0.84 %), neither a dialogue line read
  before it had finished;
- **The limit:** 0;
- **Re-reads:** 2.3 %, costing 0.9 %. All same-text re-reads: 12.0 % of the pairs and 7.6 % of the
  time.

**The cheap rules the workstream considered would change no measured read:**
- **Hashing only the rows inside the box.** The worker already hashes only the region's rows.
- **Leaving the cursor out.** The kernel masks it, and the worker's ring takes it for an icon.
- **An adaptive wait from the measured reveal rate.** With no dialogue read early, there is nothing
  to adapt to.
- **Accepting a read only when the next one does not extend it.** That would delay every read by a
  second capture.

**A model would not help either (inferred).** The wait's cost is the box's own animation, which a
learned model would have to wait out too.

**A speed-up, not a fix (est.).** The worker's wait could skip the cell the kernel masks:
- **The gain.** That could take the 203 background waits that saw changes (median 1.14 s) towards
  the 0.10 s of a still screen.
- **Who would feel it.** It would shorten the presses that find a background job still running: 45
  of the 140 opens in these runs.
- **Why not here.** It needs the mask passed through the kernel's API, and a console run, so it is
  out of scope here.

## What would change this

- **Another game.** Every measured text is CLANNAD's. A game whose typewriter stops mid-line for
  half a second or more (a dramatic beat, a voice cue), or whose box animates for longer than
  `ocr_settle_max_ms`, could read lines early or at the limit.
- **Only when I press.** It has no run on a build with the stable-text wait. It also gets only the
  job's own window.
- **How to check.** Play a session of each mode in such a game with the debug build. Then run
  `tools/install_ftp.sh --collect`.
  - `tools/analyse_selftest.py --collect` prints `read before finished` (the table's read-early
    row).
  - The job lines give the rest.
- **If early reads reach the rule's 1 % with 3 or more events,** the cheapest fixes come first:
  - raise `ocr_settle_ms`, which is one global setting today (per title needs code);
  - let **Only when I press** refresh the open overlay's text once the screen settles again, as
    Automatically already does (`background_open`).
