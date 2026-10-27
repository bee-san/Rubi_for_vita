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
  capture still shows the screen.
- **A hooked line** (API 41). A new line in a hook stream that answers without OCR starts the job
  at once, as a quiet screen, and its scene holds while the line types out
  ([native-hooks.md](native-hooks.md#a-new-line-starts-the-read)). A job that reads it with OCR
  still waits for the stable text below. A press with no result for its screen starts a job at once,
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
- **The limit.** A job waits at most `ocr_settle_max_ms` = 2000 ms over all its tries (`ocr_vertical = auto`'s
  column try included, API 47), or
  `ocr_settle_press_ms` = 1000 ms for a press (since test.3). Then it reads anyway.
- **An icon with more frames than that, or a moving picture** (#353): the job watches its found lines'
  own pixels instead of the whole region
  ([below](#animated-icons-and-moving-pictures-353)).

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

## Why the overlay waits (#358)

**The owner's item** (#358): "Say why the user is waiting. After a few seconds of 'Recognizing…' on torn reads,
name the cause and the fix: shrink the box, or turn on Pause the game to read." A job whose screen changed under
every read ends `VJO_E_SCREEN_CHANGED` and is dropped quietly (#81 request 4), and the Shell tries again at
`note_screen_changed`'s pace (at once, then 250 ms, doubling to 4 s). Until now the open overlay said
"Recognizing…" all along, however long that went on: with an animated icon in the region, until the user gave up
(#353).

**What it says now.** As the open overlay starts a try at its text (`shell/worker.c` `start_job_kind`), once the
last 2 jobs in a row ended that way (`WAIT_HINT_AFTER`; the press's own tries, or Automatically's just before the
press), it says:

> Recognizing… Something in the region keeps moving (an animated icon?), so the text never holds still. Press □ to
> draw a region around just the text.

- **Not an error.** The tries go on at the same pace, and the first that reads its screen shows its text and word in
  place of the hint. It is the overlay's status line (dim, as "Recognizing…"), never the subtitle strip's.
- **When.** From the third try in a row: with a press, two torn tries take about 3 s; a press that comes after two
  torn background jobs says it at once, since Rubi already saw the text change under two reads.
- **The fixes it names.** A region drawn around the text alone leaves an icon beside it out. And with
  **Pause the game to read** OFF (`ocr_pause`, API 52, [game-pause.md](game-pause.md)), in a Vita game, it adds
  "Or turn on Pause the game to read in the Rubi app." (`WAIT_HINT_PAUSE`): the pause reads the region from one
  frame, which an animated icon cannot change. Not with the pause ON, and not in a PSP game, which the kernel never
  pauses (`game_static_fb`, the emulator's process).
- **Starting over.** A region saved or cleared (□) starts the count over, as a new game does: the old region's tears
  say nothing of the new one's, and its next press says "Recognizing…" until two of its own jobs tear.
- **Off.** `ocr_wait_hint = off` in `config.ini` keeps "Recognizing…" all along, as before (on by default).

**Why 2.** `tools/wait_hint_replay.py` replays the rule for 1, 2 and 3 torn jobs in a row:
- **On the owner's console** (CLANNAD, every debug build of API 20 or later: test.2-test.4, the test.4 collect's
  `log.old.txt` and `log.txt`): none of the 75 openings that waited for a recognition met a torn job, so the hint would
  have shown in none of them, whatever the count. The 376 background jobs had 16 torn ones, in 15 runs: 14 single, 1
  double, each followed by a read. A wait through them would have met the hint after 15 of the runs with 1 (each
  until the next job's result, a median 1.96 s later), after 1 with 2 (the double, 987 ms before its read), and after
  none with 3.
- **On the host** (rubi-spinner's #353 bench, `host/spinner_bench.c`: 184 hooked dialogue regions of 46 titles,
  15 scenarios, L + R 3 s after the page, 20 s; `main` 892cfb9's timelines; times are the bench's estimates): all
  1,668 runs that never show text (an icon with more frames than a row's ring holds, a picture moving in the box)
  were torn, and every one gets the hint, after L + R:

  | in a row | the hint, after L + R (median / 90th percentile / slowest) | of the 104 runs that show text after a wait: the hint first (< 1 s before the text) |
  |---|---|---|
  | 1 | 1.19 / 1.47 / 2.39 s | 22 (0) |
  | **2** | **2.75 / 3.15 / 4.80 s** | 22 (4) |
  | 3 | 4.50 / 5.15 / 9.35 s | 5 (1) |

  Of the 22, 21 have an icon that turns and rests and 1 the 24-frame icon: the hint is true there (an icon is in the
  region), and shows a median 2.2 s before the text with 2. The still page, the typing line and the 8-frame icon never
  show it.
- **So 2:** "a few seconds" (2.75 s) with an icon, and on the console once in 15 runs of torn reads, never in a wait
  the owner made. 1 would follow 14 single tears that the next job read; 3 waits 1.75 s longer for the same runs.

**With #353's line watch** (its bench on #353's final code, after its audit's fixes, the same 2,760 runs;
`tools/wait_hint_replay.py --bench`): 7 runs never show text, and the hint shows in each (with 2, 6.6 / 7.2 / 7.9 s
after L + R); of the 1,711 runs that show text after a wait, the hint would come first in 45 with 1 torn job in a row
and in 6 with 2 (a median 2.2 s and at most 9.3 s before the text).

**Its cost.** No allocation, thread or static buffer: one `VjoConfig` field, 96 B of the normal Shell's code (104 B
of the debug one's), and the hint's 153 B and the key's 14 B in the data segment, where "Recognizing…" went too
([memory-ledger.md](memory-ledger.md#why-the-overlay-waits-358)).

**Not run on a console.** The rule and its text are host-tested (`host/tests/test_shell_worker.c`
`open_overlay_says_why_it_waits`); whether the hint reads well over a game, and when it shows there, waits for the
owner's test build.

## Animated icons and moving pictures (#353)

**The report.** A user's overlay showed nothing while the log had read the dialogue, in STEINS;GATE and
another game: their OCR region held the game's spinning "wait" icon (#353; the owner's analysis is there).

**What `main` did, on the host.** `host/spinner_bench.c` plays a dialogue region at 60 frames a second with an
icon drawn over it, through the kernel's change detection (`kernel/scene.c` itself), the game worker's job
(`core/vocr_ocr.c` itself, config.ini's defaults, the release models) and a model of the Shell's decisions
(`shell/worker.c`'s, function by function). The player presses L + R 3 s after the page appears and waits 20 s.
The regions are 184 hooked pages of 46 titles (vita-vn-ocr's page pool, 4 pages a title), each page's dialogue
region widened by three times its last line's height to the right and 1.5 times below, so that an icon has room.
Times are estimates (a strip read is one frame, the finder 0.82 µs a pixel, VOCR5-N at 444 MHz); no console has run
it. `tools/spinner_eval.py` runs it and `docs/benchmarks/spinner-20261027.json` has the numbers.

- **An icon of 8 frames** (12 frames a second), after the text or in the region's corner: shown at once, as on a
  still page. The kernel masks its cells, and the worker's rows take its 8 states for an icon's.
- **An icon of more frames** (a 24-frame sprite at 30 frames a second, a smooth spinner of one turn a second, one
  whose speed goes from 0.25 to 1.75 turns a second), **or a picture moving in the box**: shown in 0-5 of the 184
  regions. Each job waited its whole limit, read the region three times and ended `VJO_E_SCREEN_CHANGED`, and the
  Shell tried again, 7-8 times in the 20 s, with "Recognizing…" all along.
- **An icon that turns, then rests** (0.8 s, then 0.6 s still): shown in 174 and 168 of 184, a few after 12-13 s.
  In 7 and 13 regions a job recognized the lines and then dropped them (the log's `game vocr lines:`, nothing
  shown): the report's symptom.

**The list of checksums is bounded already; time is what fails.** A row keeps at most 16 earlier states
(`VJO_VOCR_STATES_MAX`, 6 at 960 × 544) in the job's capture block, whose size is the region's alone (at most
64 KiB, `VJO_BUDGET_GAME_CAPTURE`; 61,388 B at 917 × 125), and the kernel 16 states a cell of its 128 in its
static tracker. The bench counts every allocation of a job: none after the first job's weights and LM, with or
without an icon. An icon with more frames than a row's ring holds is never a "return to a recent state", so the
wait never holds, and every change of its rows after the wait is a tear.

**The line watch** (`core/vocr_ocr.c`, the stable-text wait's; with `ocr_settle_ms = 0` nothing changes).
Once the region keeps changing, the job judges the text by the found lines' own pixels:
- **When.** The rows have changed in the last pass after half the job's wait (1 s in the background, 0.5 s for a
  press): until then the ring learns an icon of up to 16 frames, as before. Also when rows changed while the job
  read its lines after its wait had held (an icon that rests between its turns).
- **What it watches.** Each row's checksum over the columns of the lines the job's finder found (any finder: the
  set of boxes the job reads, so a spotter's boxes would be watched the same way), with its own recent states in
  the ring's memory. A change elsewhere (an icon beside the text) only marks the region as moved.
- **The wait.** The job finds the lines (nothing is published) and waits until their pixels have held for
  `ocr_settle_ms`. If the region moved meanwhile, the finder runs again: other lines (text typing on past its box,
  a new line) are watched instead; the same lines are kept. Then the job's own first pass, the capture the Shell is
  told of, and its finder: lines alike the kept ones are read at once; others are watched again from a new first
  pass (8 times a job at most).
- **Reading anyway.** When the waiting budget runs out while the lines' own pixels still change (an icon the finder
  takes into the last line, a picture moving behind the text), the job reads the lines it found anyway: the changes
  of those lines are no tears from then on, a change of another line still is. Once they are read, the finder runs
  again, no sooner than twice `ocr_settle_ms` after it found them (the span the wait and its check take together
  without the watch), and once more `ocr_settle_ms` later if they were not alike then (an icon beside a line can
  change how the finder cuts it while it turns): lines that are not what they were (text that typed on, a new line)
  end the job as a screen change, and the next one reads them.
- **Alike.** The same count and polarity, each box within a quarter of its height on every edge (2 px at least):
  less than a character, so a line that typed on is another line. A line whose pixels changed may move by half its
  height on its sides and start (an icon drawn in its box), but its end keeps the quarter: a line typing on changes
  its own pixels too (audit-rubi-spinner F1: with 2.5 heights there, a slow typewriter was read half typed). An icon
  after the last character that GRAD+refine take into the box (its end moving by more than the line's height in 26
  of 146 such lines of hooked pages, with the icon's frames) then costs a look or a try more. At the
  limit, an icon-sized box that only one find has is passed over (with an icon in the corner, the count of lines the
  finder found changed with the icon's frames in 21-25 of 138 hooked regions; `vjo-spinner-bench --find`).
- **Its memory.** No block, thread or allocation: the line checksums take the ring's memory (two of its 16-bit words
  a row hold the checksum), the finder's runs the finder's own arena (the recognizer is done with it, or not set up
  yet). On the worker's 32 KiB stack the job's frame grows from 1,512 to 2,616 B (`run_job`, with the line sets it
  watches), and a finder's run takes `look_alike`'s 360 B on its way (`arm-vita-eabi-gcc -Os -fstack-usage`, the
  build's flags); its code is in the module's pages ([memory-ledger.md](memory-ledger.md#the-line-watch-353)).

**What it gives** (the 184 regions, 23 scenarios, eight of them audit-rubi-spinner's typing ones; median / 90th
percentile / slowest time from L + R to the text, est.; "same": the still page's text exactly; "shorter": a read
shorter than the still page's, as a line read half typed is):

| icon or motion | `main` shown | `main` time | this change shown | its time | same | shorter (`main` → this change) |
|---|---:|---|---:|---|---:|---:|
| none (still page) | 181 | 0.0 / 0.0 / 0.0 s | 181 | 0.0 / 0.0 / 0.0 s | 184 | 0 → 0 |
| the last line typing out | 181 | 0.0 / 0.0 / 0.4 s | 181 | 0.0 / 0.0 / 0.4 s | 184 | 0 → 0 |
| 8 frames, after the text / in the corner | 181 / 181 | 0.0 / 0.0 / 0.4 s, 0.0 / 0.0 / 0.5 s | 181 / 181 | 0.0 / 0.0 / 0.4 s, 0.0 / 0.0 / 0.5 s | 180 / 180 | 1 / 1 → 1 / 1 |
| turns and rests, after the text | 168 | 0.0 / 1.3 / 12.4 s | 181 | 0.0 / 2.2 / 4.9 s | 178 | 0 → 0 |
| turns and rests, in the corner | 174 | 0.0 / 1.1 / 13.0 s | 181 | 0.0 / 1.6 / 7.8 s | 178 | 2 → 2 |
| 24 frames, after the text | 5 | 0.9 / 1.3 / 4.7 s | 181 | 0.7 / 2.1 / 12.2 s | 181 | 0 → 0 |
| 24 frames, in the corner | 1 | 0.9 / 0.9 / 0.9 s | 180 | 0.3 / 1.6 / 8.5 s | 177 | 0 → 1 |
| smooth, after the text | 0 | — | 180 | 1.6 / 2.5 / 5.4 s | 177 | 0 → 0 |
| smooth, in the corner | 0 | — | 180 | 1.3 / 2.3 / 9.6 s | 178 | 0 → 1 |
| variable speed, after the text | 0 | — | 180 | 1.5 / 2.3 / 6.6 s | 179 | 0 → 1 |
| variable speed, in the corner | 0 | — | 181 | 1.2 / 2.2 / 8.1 s | 179 | 0 → 2 |
| variable speed in the corner, the line typing | 0 | — | 181 | 1.3 / 2.2 / 8.1 s | 179 | 0 → 2 |
| a picture moving outside the text | 0 | — | 180 | 1.8 / 2.4 / 10.6 s | 178 | 0 → 3 |
| a picture moving behind the text | 0 | — | 179 | 1.7 / 2.4 / 7.9 s | 178 | 0 → 4 |
| slow typewriter, a character every 10 / 15 frames | 181 / 181 | 0.5 / 1.7 / 3.0 s, 1.5 / 3.6 / 6.1 s | 181 / 181 | 0.5 / 2.0 / 4.0 s, 1.5 / 3.8 / 6.1 s | 183 / 182 | 1 / 1 → 1 / 2 |
| typing with 0.5 s pauses | 181 | 0.0 / 0.8 / 2.7 s | 181 | 0.0 / 1.1 / 3.0 s | 171 | 13 → 13 |
| slow typewriter (10 frames), variable icon in the corner | 0 | — | 180 | 1.8 / 2.9 / 7.1 s | 176 | 0 → 2 |
| typing with pauses, variable icon in the corner | 0 | — | 180 | 1.7 / 2.6 / 7.1 s | 177 | 0 → 3 |
| slow typewriter, a smooth icon following the typed end | 0 | — | 180 | 1.9 / 3.0 / 18.7 s | 178 | 0 → 3 |
| typing with pauses, a smooth / 8-frame icon following it | 0 / 181 | —, 0.9 / 1.9 / 3.2 s | 180 / 181 | 1.9 / 2.6 / 11.3 s, 0.9 / 2.1 / 6.6 s | 178 / 180 | 0 / 0 → 2 / 2 |

"Shown" counts a read with text: the still page's read has text in 181 of the 184 regions.
- **No region that `main` showed is lost**, in any scenario, and where nothing moves, or only an 8-frame icon, the
  times and texts are `main`'s region by region. With the icon that rests, 13 and 7 more regions are shown, and
  against `main`, region by region, the time is 0.27 s and 0.13 s shorter on the mean; 17 and 10 regions take more
  than 0.3 s longer (by 2.3 s and 1.1 s at most: `main`'s job happened to read in a rest), 18 and 8 more than 0.3 s
  less. The slow and pausing typewriters that `main` shows take 0.02-0.16 s longer on the mean (a line read anyway
  is looked at again before it is given).
- **Shorter reads.** 28 runs give a read shorter than the still page's where `main` did not: 24 in regions `main`
  never shows at all (an icon of many frames or a moving picture), and 4 of the 1,777 where `main` showed the whole
  line, later:
  the slowest typewriter in 1 region, typing that pauses under an 8-frame icon following it in 2, the 10-frame
  typewriter in 1 (where `main` read another region short). In those 4 the next job (Automatically's) read the whole
  line 0.4-2.3 s later.
- **The jobs.** Over the 20 s an icon of many frames or a moving picture took `main` 1,295-1,376 jobs over the 184
  regions, 1,295-1,371 of them ending with the screen changed; this change 185-204, 1-20 of them.
- **The report's symptom** (lines recognized, then dropped, and nothing shown): 13 and 7 regions on `main` with the
  icon that rests (2 each with the 24-frame one); here 11 runs of the 4,232, 1 or 2 in 10 scenarios, each one of
  the 1-5 regions a scenario never shows.
- **The synthetic box** (917 × 125, three lines of glyph-like strokes; ctest `spinner_bench`, the models lane):
  `main` shows nothing in 15 of its 23 scenarios; this change shows the text in all 23, within 3.9 s (est.; the
  slowest, a slow typewriter followed by a smooth icon), the still read's text wherever nothing moves inside a
  line's box.

**The hint.** A job whose screen changed under every read is still dropped quietly (#81 request 4), but the third
in a row logs, in both builds, `ocr: the screen changed under every read of 3 jobs in a row, the last with N
line(s) recognized and not shown: an animated icon or a moving picture in the region?` (`shell/worker.c`
`note_screen_changed`; the string is in the data segment). What the open overlay says while it waits is #358's
([above](#why-the-overlay-waits-358)). The debug build's
`gstep job settle` says what the watch did (kind: 1 its lines watched, + 2 read anyway, + 4 × the finder's runs;
`tools/analyse_selftest.py --collect` sums them).

**Limits.**
- **Not run on a console.** The times are the host model's (frames, the finder's and the recognizer's estimated
  speeds); each finder's run is about 94 ms for CLANNAD's 852 × 135 region at the measured 0.82 µs a pixel (est.).
- **An icon inside a line's box is read with the line.** As on `main`, its pixels are the line's (a mark
  read after the last character, or nothing). A text-line spotter that never boxes an icon (vita-vn-ocr's VOCR6
  work, #125) would leave it outside the lines, and the watch then waits it out like an icon beside the text.
- **A picture moving behind the glyphs** changes their pixels: the lines are read anyway at the limit.
- **A page turned while the lines are read anyway** with the same layout (as many lines, as long) is not seen
  by the last check: in audit-rubi-spinner's runs of a page turned while a picture moves behind the text, 11 of 987
  read lines of both pages. The Shell's own check of the screen (its scene) then refreshes the open overlay.
- **A slow typewriter** whose pause between two characters is longer than the wait's window and the reading together
  (`ocr_settle_ms`, 150 ms by default, and the read) is read as far as it has typed, on `main` as here; the next job,
  once it types on, reads more. Faster than that, the line never holds still: a line read anyway that typed on ends
  the job as a screen change (the policy tests' `a_slow_typewriter_is_never_read_half_typed`, a character every 8, 10
  or 15 frames, with and without an icon).
- **In-sample.** The watch's rules (half the budget, a quarter and half a line's height) were chosen on these 184
  regions. On 92 other pages of the same 46 titles (halfway between the measured ones, as audit-rubi-spinner held
  them out), the final code shows 89-91 of the 92 with an icon of many frames or a moving picture (`main` 0-3), a
  median 0.4-1.9 s, a 90th percentile 1.6-2.6 s and at most 9.8 s after L + R; no region `main` showed is lost, and
  1 of the 889 runs `main` showed whole is read short first. The bench charges the finder's runs to the job at its
  end, not while the frames play on, and plays 60 frames a second only.
- **The kernel's tracker** is unchanged: with such an icon the region may never settle, and Automatically reads it
  on its quiet events (a spot of 3 × 3 cells that keeps changing) instead.
