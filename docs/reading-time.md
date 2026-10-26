# Reading time: the AFK rule and how it was chosen

The overlay's reading statistics (Triangle, [ocr-history.md](ocr-history.md#total-characters-read))
credit each recorded line with the time to the next recorded line of its game. What else that gap
holds, Rubi cannot see: a lookup, a slow line, or an AFK, a suspend, standby, the next day. The rule
that decides how much of a gap counts is `reading_time` in `config.ini`
([ocr-history.md](ocr-history.md#reading-time)). Rubi 0.9.7 counted a gap only if it was 15 s or less
(`15s`); since this change the default is `adaptive`. This page says how the rules were compared, and
how GameSentenceMiner, whose numbers Rubi's users compare theirs with, came to its own rule.

## How the rules were measured

No `history.csv` from a console was available (no collect fetches it), so the rules were measured on
simulated timelines: `tools/reading_time_sim.py`. It is a test harness, not a feature.

- **Readers**: 3,000, 6,000, 10,000 and 18,000 characters an hour, word lookups included (Poisson,
  0.06 to 0.002 a character, 4-20 s each), a line's time log-normal around the reader's pace (spread
  0.4), 0.6 s to press on. A text box has the characters of one of the 50 real VOCR regions of
  `host/fixtures/word_lookup` (6-44, mean 26.6), or 0-4 (6 %).
- **What happens besides reading**, per hour of reading: AFK breaks (4 of 20-120 s, 0.5 of 2-60 min;
  the Vita's own standby after 5 minutes), suspends (0.5-1 of 1-15 min), standby (0.25-0.5 of 5 min
  to 8 h), menus (3 of 5-40 s, their text recorded half the time), the backlog (1 of 1-4 old lines,
  each recorded 60 % of the time), skip mode (bursts of 20-200 lines at 0.1 s), the game's auto mode
  (10-50 % of lines, at about the reader's pace), a typewriter pause recorded as a part line (0.5 %),
  a refresh that reads a line differently (1 %).
- **What Rubi records** (shell/history.c, Read text: Automatically): one record per settled screen whose
  text is not the record before's (a blank read lets the same text be written again), timed at the
  read's start: 300 ms after the line finished typing (30 a second) for OCR, at once for a hook
  (API 41); a line the player pressed past before the read is not recorded; skipped lines are not
  recorded (they never settle, and a hook's quick lines wake nobody).
- **The truth**: the time on each read line, lookups and re-reading in the backlog included; no AFK,
  suspend, standby, menu or skipped line. Its characters: each read line once.

Eight scenarios (clean reading, AFK breaks, suspend and standby, menus and backlog, skip mode, auto mode,
and an everyday mix of all of them, by OCR and by hook), four readers, 12 runs of 4 hours each. Every
parameter is an assumption, so `--sensitivity` reruns the comparison with each changed. To reproduce:

```sh
python3 tools/reading_time_sim.py --seeds 12 --sensitivity
python3 tools/reading_time_sim.py --fixtures host/fixtures/reading_time   # the tests' timelines
```

`host/fixtures/reading_time/*.tsv` are ten 1-hour timelines from it, with what each rule credits;
`host/tests/test_shell_history.c` appends them through the real writer and checks the Shell's counters
and a recount, and `tools/tests/test_reading_stats.py` checks `tools/reading_stats.py` and the error
against the truth.

## Results

Reading-time error (estimated / true − 1) over all 32 scenario-reader pairs, mean of the absolute
error, and the worst pair [estimates: simulated timelines]:

| rule | reading time: mean abs | worst | characters/hour: mean abs | worst |
|---|---:|---:|---:|---:|
| Rubi 0.9.7 overlay: a gap over 15 s is a break | 52.7 % | −93 % | 63.1 % | +149 % |
| Rubi 0.9.7 `reading_stats.py`: over 5 minutes | 4.2 % | +13 % | 3.5 % | −11 % |
| GameSentenceMiner 2025-09: every gap capped at 120 s | 5.1 % | +15 % | 4.2 % | −12 % |
| GameSentenceMiner 2025-12: capped at 60 s | 3.3 % | +10 % | 3.0 % | −8 % |
| GameSentenceMiner 2026-03: max(15 s, 3 s a character) + IQR | 4.1 % | +12 % | 3.4 % | −10 % |
| GameSentenceMiner 2026-06: 2.5 × the median pace | 4.2 % | −9 % | 4.6 % | +10 % |
| GameSentenceMiner 2026-07: 2.5 × half the median | 2.8 % | +11 % | 2.3 % | −9 % |
| GameSentenceMiner 2026-10 (now): 2.5 × the session median | 4.4 % | −9 % | 4.8 % | +10 % |
| JL: the line's characters at 10 a minute | 6.0 % | +16 % | 5.0 % | −14 % |
| a gap over 120 s is a break | 2.5 % | +10 % | 2.2 % | −8 % |
| **Rubi `adaptive`** | **2.2 %** | **+9 %** | **1.9 %** | **−7 %** |

The 15 s rule loses most of the reading time of anyone whose lines take more than 15 s, a beginner's
nearly all of it, and the lines it keeps are the fast ones, so characters/hour comes out high. Any rule
that keeps the longer lines does far better; among those, the ones whose cap follows the reader do best
on average, and the per-line caps are also the most stable when the assumptions change:

| rule | as assumed | spread 0.25 | spread 0.6 | no lookups | lookups ×1.5 | interruptions ×3 | ×3, no lookups |
|---|---:|---:|---:|---:|---:|---:|---:|
| Rubi 0.9.7 overlay (15 s) | 52.7 | 51.5 | 55.2 | 47.2 | 58.3 | 53.2 | 47.8 |
| GameSentenceMiner now | 4.4 | 4.4 | 5.3 | 1.5 | 9.0 | 4.6 | 4.3 |
| a gap over 120 s is a break | 2.5 | 2.4 | 2.5 | 2.7 | 2.5 | 6.7 | 7.1 |
| Rubi `adaptive` | 2.2 | 2.2 | 2.2 | 2.4 | 2.8 | 6.3 | 6.8 |

(mean absolute reading-time error, %.) Where each still errs: GameSentenceMiner's 2.5 × median cuts
lines with many lookups (−9 % for a beginner as assumed, −48 % with half as many lookups again); a
fixed 2-minute break counts fast readers' short AFKs whole (+20-28 % with three times the breaks);
Rubi's cap, which follows the slower quarter of the reader's lines, gives a beginner's AFKs more room
(+23-31 % with three times the breaks).

Smaller fixes, measured on the same grid with the adaptive rule:

- **Leaving out the gap around a suspend or standby** (the Shell sees the game leave the front, but
  `history.csv` does not, so a recount could not): 2.2 → 2.1 %. Not done: the cap already bounds such
  a gap to the line's expected time.
- **SceShell's process time instead of the clock** (it may stand still in standby; unverified on a
  console): 2.2 → 2.1 %. Not done.
- **Counting a line again in the backlog or after a menu once only** (a record whose text is one of the
  8 before it): characters 0.5 → 0.2 % (everyday OCR +3 → +1 %), time unchanged. Not done: it needs the
  last texts in the Shell, for a small gain.
- **Sessions** (GameSentenceMiner's 30-minute break, its median per session): 2.2 → 2.1 %. Not done, by
  the owner's decision: Rubi has no sessions.

## GameSentenceMiner's AFK rules, in order

[GameSentenceMiner](https://github.com/bpwhelan/GameSentenceMiner) went through most of these rules on
the way to its own. Its stats were added by bee-san in 2025; the commits are its repository's.

| when | commit | rule | why it changed |
|---|---|---|---|
| 2025-09-06 | `b36aedf7` "Add stats" | every gap at most `afk_timer_seconds`, 120 s (30-600 s), even overnight; sessions (gaps over 1 h) only counted | |
| 2025-09-13 | issue #185 | the timer was hard-coded to 120 s in places of `stats.html` | |
| 2025-12-17 | issue #321 "Sentence Length Aware AFK" | | AFK on 「ああ...」 with a 60 s timer gives 120 characters an hour; suggests [JL](https://github.com/rampaa/JL)'s per-length timer and the reader's own speed for that game, with clamping so a skip cannot set it |
| 2025-12-31 | `cb405a01` | the timer's default 60 s; a `minimum_chars_per_hour` (5,000) setting declared, never used | |
| 2026-03-13 | `6dd5b958` (plan `.opencode/plans/reading_speed.md`), `6906151d` (`test_afk_detection.py`: fixed, 1.2 s a character, an EMA × 3), `5c11e6b9` "remove afk timer" (and its setting); shipped in #394, #409 | each line at most max(15 s, 3 s a character), 300 s ceiling; then a line slower than Q1 − 1.5 IQR is re-timed at the median speed | the flat timer cut long lines, lookups and re-reading, so it understated time and overstated speed, and users had to tune it by hand |
| 2026-06-19 | `f4c5a955` "Adaptive reading-time v2" (opt-in) | each line at most 2.5 × its expected time at the median pace of the lines (5+ characters), 2 s floor, 300 s ceiling, 3 s a character before a pace; time and characters credited together when the next line comes | short lines and pauses no longer over-count |
| 2026-07-05 | `e59edb51` | 2.5 × at 80 % of the median | so fast, easy lines do not make later caps too tight |
| 2026-07-10 | `ad677c4b` "Use a more conservative adaptive reading pace" | 2.5 × at 50 % of the median (5 × its expected time) | |
| 2026-09-30 | `f13f205f` | the adaptive rule for everyone; the 2026-03 rule and its opt-in setting removed | |
| 2026-10-01 | `9eba9390` "use consistent session pace" | back to 2.5 × the median, now per 30-minute session (a longer gap: no credit); a line teaches the pace only with 5+ characters and 1-300 s (`reading_speed_sample`); an empty line gets nothing | sub-second bursts measure text delivery, and a gap over the ceiling would teach a break as reading; filtering the samples, not loosening the cap, fixed the too-fast median |
| 2026-10-05 | `ca5eb642` | a live session expires after 30 minutes idle | |
| 2026-10-10 | `6d58dd08` | a gap across an explicit pause (text intake paused, from `fb5ccb00`) is neither credited nor a sample | |
| 2026-10-21 | `093dc8cd` | the live day's raw time: a gap over 30 minutes falls back to the adaptive cap | |

Rubi's `adaptive` takes the cap, its floor and ceiling, the 3 s fallback and the sample filter from the
current rule (`util/stats/stats_util.py` at `5190b613`), and, as issue #321 suggested, the pace per
game. It leaves out the sessions, and in place of the median of a session it keeps a running 75th
percentile, which needs no list of lines (the Shell keeps 16 bits) and measured best on the simulated
timelines (the median: 4.4 %; 0.8 or 0.5 of the median, GameSentenceMiner's July values: 2.1 and
2.8 %).

Other trackers: [exSTATic](https://github.com/KamWithK/exSTATic) stops its clock 60 s after the last line
(`afk_max_time`, `src/vn/vn.svelte`); JL stops it after the line's characters at
`MinCharactersPerMinuteBeforeStoppingTimeTracking` (10 a minute), at least 1.5 s
(`JL.Core/Statistics/StatsUtils.cs` `SetIdleTimeTimerInterval`, `3f7752ed`); ッツ Ebook Reader pauses its
tracker on reader events (Moderate) or any loss of focus (Strict), and after an idle time without page
interaction (off by default), rolling the idle time back (`book-reading-tracker.svelte`,
`settings-content.svelte`, `e7ec1d26`): the 15 s rule was that idle rollback at 15 s.

## Limits

- Simulated, not measured on a console: the error figures depend on the assumed readers and breaks
  (hence the sensitivity table). A console history with the owner's own reading would test them.
- An AFK in the game's auto mode looks like reading: the lines keep coming at a reading pace. No rule
  based on the records' times can tell.
- The pace starts at 1.2 s a character for a game with no history (3,000 an hour); a much faster
  reader's first few dozen lines get a looser cap while it learns.
