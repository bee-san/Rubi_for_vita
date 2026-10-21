# Line history: Up walks back through this game's earlier lines

The owner, 2026-10-20 09:01Z: "Mine from history. In the overlay, you can click Up to access prior
lines. The overlay will then display 3 lines." On 昨日すき焼きを食べました, Up shows

```
昭和５２                      (dimmed)
４７７８万                    (the line in focus: the cursor and its lookup are on it)
昨日すき焼きを食べました      (dimmed)
```

"This should have per-game lines, so not every line is shown." 09:22Z: "nah, only work on
automatic": line history has the lines of **Read text: Automatically** only, the ones
`history.csv` already records ([ocr-history.md](ocr-history.md)); nothing new records a line for it.

<img src="img/line-history-host.png" width="480" alt="Three overlay states: the current line 昨日すき焼きを食べました with 昨日's entry; after Up, 4778万 in white between 昭和52 and 昨日すき焼きを食べました, both dimmed, with 万's entry and the line Earlier line -1; after Enter, the status Saved for Anki (an earlier line: no screenshot).">

*A host rendering of the code's own spans (the header from `core/styled.c`'s `vjo_styled_history`,
the body in `shell/overlay.cpp` `render()`'s order and strings, the bundled dictionary's real
lookups), drawn in IPA P Gothic at the overlay's geometry: not a console capture. The game worker's
normalisation stores full-width digits as ASCII (４７７８ → 4778), as `history.csv` keeps them. The
rendering's lookups use **Japanese characters only** ON (the promo capture's setting), under which an
earlier line opens with the cursor on 万; with today's default, OFF, it opens on 4.*

## What it does

| Key, in the overlay | Action |
| --- | --- |
| Up, on the text's top row (no row above to go to) | Show this game's line before the one shown. The first Up opens a *visit*: the current text waits, and the earlier line takes its place |
| Up, on an earlier line's top row | The line before that one, up to 32 lines back |
| Down, on an earlier line's last row | The line after it; after the newest earlier line, the current text again, with its cursor where it was |

- The focused line is shown in white between the **last row of the line before it** and the **first
  row of the line after it** (at first, the current text), both dimmed (`VJO_RGB_RANK`, the colour of
  the frequency badge), with no characters of theirs selectable. The header grows to at most five
  rows for it, and the definition below shrinks by as much (`HEADER_MAX_LINES_HISTORY`). An earlier
  line has no `[HOOK]` or `[OCR]` before it, as the current text has: its source is not kept
  ([source badge](native-hooks.md#the-popups-source-badge)); the current text has its own again when
  Down comes back to it.
- The body's first line says where the text is: `Earlier line -2 · ▲ earlier · ▼ later` (without
  `▲ earlier` on the oldest line kept).
- On the focused line everything works as on the current text: Left/Right, Up/Down between its own
  rows, the left stick, a tap, the dictionaries' pages (L/R) and Enter. Each line's first word is
  looked up when it is shown, as the current text's is when the overlay opens. The arrows' Up/Down
  between lines are the d-pad's only: a held left stick stops at the text's first and last rows, as it
  stops at a one-row text's ends ([touch-lookup.md](touch-lookup.md#the-left-stick-138)).
- Only this game's lines: a record whose first column (`uuid`, `vita-PCSG00415` or `psp-<disc ID>`)
  is the running game's, as `net_job` writes it from the game Anki's cards name. Another game's lines
  between two of this game's are skipped. Where that ID is shared, so are the lines: PSP games started
  from inside Adrenaline's own menu all write `psp-PSPEMUCFW` (the Rubi app describes no disc for them),
  and a game whose title ID cannot be read writes `vita-GAME`, as their Anki cards say already. Consecutive identical lines are shown once (history.csv
  already collapses rescans of one screen; a line that is the same as the line after it, once filtered,
  is skipped too), and a line again after a different one (A, B, A) stays.
- A line is shown as the overlay shows a text: its rows without Japanese left out when
  `non_japanese_filter = lines` (the default), trimmed. A line with nothing left is skipped.
- **Read text: Only when I press L + R**: Up on the top row does what it did before line history,
  nothing (owner 09:22Z: "only work on automatic"); that mode records no line. Up does nothing either
  when there is no earlier line: no `history.csv` yet, none of this game's lines in its last 24 KiB,
  on the oldest line kept (the 32nd at most), or no Paf memory for a visit. The debug build's log has
  a line for each read (`history: N earlier lines of …`), and both builds log a Paf block the reserve
  guard refused; an Up on the oldest line has none. With no earlier line, each such Up reads the
  file again (it keeps nothing to remember that there was none).
- A step goes to its line once no job reads the overlay's text: a lookup or a refresh of the line it
  leaves is dropped (a refresh is asked again once the current text is back), a word's preparation
  ends first. Another Up or Down pressed while a step waits for that is dropped (press again). A move,
  a tap or an L/R page pressed before the control thread has taken an Up or Down drops that step
  instead: the latest press wins, and nothing is left waiting.
- Closing the overlay on an earlier line ends the visit: the current text comes back with its cursor
  and its screen (once no lookup of the earlier line runs), so a reopen on the same screen shows it,
  its word looked up again (the dictionary only: no recognition), and Auto, closed, does not read that
  screen again for a settled event the overlay saw (its lookup left the cache with the earlier line's).
  Reopened before that lookup ended, the overlay recognizes the screen, as after any lookup a close
  dropped. A game exit, a new region or another OCR pipeline ends the visit with the text, as they
  retire any shown text.

## Cards from an earlier line

Enter on an earlier line saves a card as on the current text, with **that line as the sentence** (it
is the overlay's text then), its word, every dictionary's entry for it, the game and the time. It has
**no screenshot**: Rubi keeps no frame of a line, and a screenshot taken now would be of another
screen (whatever the game shows now), so none is asked for. The status line says so: `Saved for Anki
(an earlier line: no screenshot)` when screenshots are on; with `anki_screenshot = off` it says what
any card's does. Its record names no OCR model: the last recognition's, which a card of the current
text names, may not be the one that read that line. No audio either (Rubi records none).

## Where the lines come from (and why)

The task left two sources open: a bounded RAM ring of recent texts, or a bounded read of
`history.csv`'s end when Up asks for earlier lines. The owner's 09:22Z note settled the modes
(Automatically only, which is what `history.csv` records), and the evidence settles the rest:

- **The Shell's code pages were nearly full.** On main `f482c21` the normal Shell had 1,764 B of text
  room in its 90,112 B of text pages (1,684 B since #232, `75041a0`), which `tools/check_memory_ledger.py` checks exactly
  [measured: the build log's module row]. AGENTS.md rule 1: its permanent footprint may only shrink.
- **A RAM ring is permanent SceShell memory.** 32 lines kept for the moment Up is pressed would hold
  the texts in SceShell's `.bss` (or a permanent Paf block) for the whole session, beside a game; the
  data pages had 448 B of room. A ring also loses its lines at every Shell restart, and holds texts
  that are already on the memory card.
- **`history.csv` already has them**, with the game in its first column, written by VjoNet after each
  Automatically read (and a text source that appends its lines through `vjo_history_append`, as text
  hooks should, shows up in line history with no change). Reading them back costs nothing until Up is
  pressed past the top row.

So: when a visit opens, the control thread reads the **last 24,512 B of `history.csv`** (one read,
with the byte before them, into its existing 24 KiB static scratch, released before the call
returns), finds this game's records newest first, and keeps up to **32** of their lines in one Paf
block of **16 KiB**, held only while the visit lasts. A three-row textbox's record (a 290 B line, its
raw text as long, and its 39 other fields) is 871 B [measured: the host test's record, by
the real writer], so the window holds about 28 such records, of every game; a shorter line's record is
smaller. Nothing is read or kept at any other time; the file is never written by this code.

- The read is on VjoControl, never on the Paf thread, so the overlay keeps drawing. It is one open,
  two seeks, one read of at most 24,513 B and one close, once a visit. Console time unmeasured: the
  dictionary's lookups on `ux0:` take 15.3 ms for an open, a seek, a small read and a close, and
  3.82 ms more per 4 KiB read ([performance.md](performance.md)), so about 15-40 ms [estimate]. The
  debug build adds two log appends a visit (the Paf block's line and the visit's), 9.4-47 ms each on
  the console [measured there for other lines, docs/performance.md]. Steps within a visit read nothing.
- An append in progress on VjoNet (it appends after publishing each Automatically result) can only
  make the file's last record incomplete: a record is read only once its line's field is closed and
  followed by the comma and the time's first digit, so a cut record is skipped, and an append rolled
  back meanwhile only shortens the read.
- A record a power loss or a crash cut is not always the last one: the next boot's append follows the
  cut with no newline between. The cut record's fields then run into the new record's, and it would
  read as another field's text (the game's name) or as bytes of both; the time's digit after the line
  field rejects every such merge (the writer always writes one there, a merge has another field's
  `"`), so neither the cut record nor the one appended right after it is shown, and the lines before
  and after both are (audit-line-history F1: host tests at every cut position, and the auditor's fuzz
  of the real writer and reader, 0 bad lines in 39,759 cut-and-append cases).
- The record parser matches `history.csv`'s own quoting (`shell/history.c`: the first four fields
  quoted, `"` doubled): a record starts at a `"` after a newline whose next byte is neither `"` nor
  `,`, which no byte inside a quoted field can be.
- The visit opens when Up is pressed, even while a lookup or a refresh runs (they only read the
  overlay's text, which only the control thread writes). A refresh dropped then may already have
  appended the screen's newer line: records newer than the current text's newest record are not
  earlier lines, so that line is not shown as one (an older record of the same text still is). The
  rule's one cost: when the current text has no record of its own (an invalid clock or a failed append
  wrote none, or Up came within the milliseconds before VjoNet's append of it landed) and an older
  record has the same text (a short line said again, 「うん」), the lines between that record and now
  are not shown; the ones before it are.
- Within a visit no refresh runs (the shown line is not the screen's); a settled screen is
  recognized once the current text is back (the refresh waits, `refresh_pending`).

## Memory and pages

Every figure from the build log's module rows and ledger, both builds, main `75041a0` (#232's left stick
on `f482c21`) and this change,
VitaSDK gcc 15.2.0 in `vitasdk/vitasdk:2026.08` [measured]:

| | main `75041a0` | this change | room left |
|---|---:|---:|---:|
| normal Shell, text | 88,428 B | 90,044 B (+1,616) | 68 B of 90,112 |
| normal Shell, data/bss | 73,280 B | 73,584 B (+304) | 144 B of 73,728 |
| debug Shell, text | 98,812 B | 100,492 B (+1,680) | 1,908 B of 102,400 |
| debug Shell, data/bss | 73,272 B | 73,576 B (+304) | 152 B of 73,728 |
| kernel / OCR worker data/bss | | +8 B each | `VjoLedger`'s eleventh item |

The pages stay 163,840 B (normal, exact) and 176,128 B (debug); the kernel and the worker keep theirs.
The normal Shell's text pays for line history's code with `core/textfilter.c` built `-Os` (its object
288 → 240 B) and four strings in the data segment (`VJO_DATA_TABLE`: line history's two, and two of
`shell/worker.c`'s cold error messages, 108 and 82 B, moved out of `.rodata`); per function in
[memory-ledger.md](memory-ledger.md#tables-in-the-data-segment). Beside #233 (the `characters` column,
its head `6e8a6cb` merged into this one) both builds still fit: the normal Shell 90,068 + 73,688 B
(44 B of text room, 40 B of data room), the debug one 100,500 + 73,680 B [measured: the two builds of
that merge].

- **Paf, `VJO_SL_PAF_HISTORY` (16,384 B)**: one block per visit, from the first Up past the top row to
  the return to the current text (or the close, the game's exit). `VJO_WORST_SHELL_PAF` adds it: the
  worst case (an Anki save from an earlier line, the overlay open, its lookup) goes from 225,280 to
  241,664 B, of the 985,088 B limit. It holds the visit's table (`HistoryVisit`, 156 B on the Vita),
  the current text (at most 4,096 B, compile-time checked) and the lines, each at its own length;
  a line that does not fit ends the list there (the lines kept are the newest, without a gap).
  Typical lines (20-60 characters of 3 B) are 60-180 B, so 32 take 2-6 KiB [estimate]. A refused
  block means no visit: Up does nothing.
- **VjoControl's stack** (16 KiB): `history_open` and `history_ask` are inlined into `ctl_main`, whose
  frame goes from 256 to 312 B in the normal build and from 248 to 304 B in the debug one (+56 B under
  every chain of the thread); line history's deepest chain is 576 B (`ctl_main` 312, `vjo_history_load`
  80, `vjo_history_scan` 64, `vjo_filter_lines` 48, `vjo_buf_append` 16, `buf_reserve` 40,
  `vjo_arena_extend_last` 16; `history_show`'s 96 B, with its `VjoState`, is beside it), under the
  thread's config load
  (`apply_config` alone is 1,352 B) [measured: each file's exact compile command from the Vita build
  (`flags.make` and its own options) with `-fstack-usage`, both builds, main `75041a0` and this change;
  the SDK's own calls not counted].
- **The overlay's text arena** (`VJO_BUDGET_PAF_UI`, 20,480 B): the header's spans go from 1 to 5
  (`VJO_HEADER_SPANS`: the text and each neighbour's row and line break), so its need goes from
  17,952 to 18,016 B (`test_ledger.c`).
- **No new thread, no `sceKernelAllocMemBlock`, nothing in the kernel or the game worker** but the
  ledger's eleventh slot (`VJO_LEDGER_MAX_ITEMS` 10 → 11: 8 B of `.bss` in each module's `VjoLedger`).

## Tests

- `host/tests/test_history_lines.c` (also on the ARM lane, `tools/arm_host_tests.sh`): the parser on
  `history.csv`'s records: this game's lines newest first, other games' and a longer title ID's
  skipped, the current text not shown again, A/B/A, lines newer than the current text's newest record
  skipped (and no pointer to them left in the table), quotes, commas, a textbox's rows, the filter and
  trim, a record cut by the window's start or by an append at every byte, a record cut at every byte
  and run into the next append (F1), a damaged separator, a record with more columns, at most 32 lines,
  a full block ending the list without a gap, the writer's longest text.
- `host/tests/test_shell_history.c`: the real writer and the real reader over real files: a file
  larger than the window (its last 24,512 B read, once), another game's lines in between, short reads,
  no file, a failed read, a file shorter than its size, no scratch, the writer's maximum text, a record
  cut by a power loss, a record at the window's first byte (and a byte either side), and a measure of
  a three-row textbox's record (871 B: the window holds 28).
- `host/tests/test_shell_worker.c` (`history_*`): the real control thread: Up and Down through a
  visit with lookups and moves on each line, the oldest line, the return with the cursor and the
  screen, a reopen showing the saved word, on_press, no memory (and a lookup running then still
  shown), no file, a PSP game's ID; a step while a lookup or a refresh runs, a second Up while it
  waits; a step waiting for an OCR job that a new screen cancels goes with the retired text (B-7); no
  refresh in a visit and the one asked for after it; Up during a lookup then a close and a
  reopen on the same screen or another: the visit, never shown, goes and refreshes resume; a close on
  an earlier line (no re-read of the text's screen after it), a reopen during its lookup, a game exit
  and a new region; a move or a page posted over a pending step (F2) and a step never taking a close's
  or a region's slot; the subtitle strip turned on during an earlier line's lookup after a close (F3);
  the Paf block's ledger item and its release (filled with a pattern when taken and freed, so a
  pointer kept into it shows).
- `host/tests/test_shell_anki.c`: a card from an earlier line has that line as its sentence and no
  screenshot, says so, asks the game for none and names no OCR model; the current line's card still has
  its screenshot and its model.
- `host/tests/test_core.c` (`styled_history_header`): the header's composition and dimming: the
  neighbours' rows and colours, the text's first unit, a textbox's rows, a pathological row cut at 128
  units without splitting a surrogate pair.
- `host/tests/test_cursor_nav.c` (`up_and_down_past_an_earlier_lines_rows`): the arrows over a header
  with the dimmed rows (the layout double): Up on the text's top row and Down on an earlier line's last
  row are the line steps, Up/Down between its own rows and Left/Right stay in the text.

A mutation pass over the new code (28 one-line mutants: the parser's record rules and the time check, the
newer-than-current reset, the dedupe and the limit, the step's wait, cancellations and slot rules, a
retired visit's step, on_press, the return's cursor and screen, the visit's frees, the refresh guard, the
cache's tag, the earlier card's screenshot and model, the header's lead and dimming, the reader's window
and its byte before) is killed 28 of 28 by these suites [measured: a scratch copy of the tree, each mutant
built and every suite above run]. The auditors' own tests (F1-F3, the slot rule, B-7's path) and their
mutant sets pass and are killed on this head too.

Not tested off the console: how the dimmed rows look on the Vita's screen, the read's time on `ux0:`,
and the Paf heap's free size during a visit. Vita3K is not evidence for any of them (AGENTS.md
rule 5).
