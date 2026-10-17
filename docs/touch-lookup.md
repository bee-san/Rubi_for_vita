# Touch selection and highlighting

Overlay app → OCR & lookup → Highlight chooses **Whole word** (default) or
**Character**. Local dictionaries show one entry at a time; with
**Dictionaries shown** above 1, an L or R tap pages to other dictionaries'
entries at the same character ([details](local-dictionaries.md#dictionaries-shown)).
Each page highlights its own match, and Right skips the one shown. In Whole
word mode, Right skips the current matched source span and Left retraces up to
16 recent forward selections. Without a match or previous selection, movement
falls back to one Unicode character. Character mode always steps one character.
Both go in reading order across the header's displayed rows and stop at the
text's ends (a text on one row goes round them), and Up/Down change rows
([rows and arrow keys](#rows-and-arrow-keys)). The left stick moves the cursor
in the same four directions, faster, and looks its word up only once it rests
([the left stick](#the-left-stick-138)).
Whole word uses the matched source span, including an inflected ending, rather
than the dictionary's normalized spelling. While a lookup runs or has no result,
it highlights the selected character instead. Changing the preference preserves
the recognized text; a completed selected lookup is reused when opening again.

Automatic OCR prepares the first local match before the popup is opened.
Reopening unchanged dialogue reuses a completed, current result without more
dictionary reads. Changed text, scene, region, dictionary or cursor excludes
that cache. On-press OCR still requires recognition and an initial lookup when
opening; new selections perform one asynchronous lookup each. The existing
single dynamic 64 KiB result workspace is retained; there is no sentence-wide
definition cache or static workspace reservation. Navigation stores only 16
byte offsets, and a tap or a new sentence clears that history.

While the overlay is open, the text it shows stays usable whatever the screen
does (#81 request 4): moves, taps, dictionary pages and Anki saves always use
it, and nothing says the dialogue changed. The region's scene can change while
the dialogue does not: in the test build's console log one open overlay saw ten
new scene numbers with the game's input blocked and one cell masked as
animated (an icon whose mask comes and goes is enough).
In Auto mode (not with `ocr_mode = on_press`, the Overlay app's Read text **Only when I press**, which
recognizes only when the overlay opens: [auto-vs-press.md](auto-vs-press.md)), a screen that settles and is not the one the text was read from
is recognized in the background, a refresh, which leaves the text, its entry and the result
arena alone: the same text changes nothing, no text (a box between lines)
keeps the shown one, and a different text replaces it quietly, with the cursor
on its first character and its first word looked up; the subtitle strip (Select
+ R, if on) shows it too, and Anki cards name the model that read it. Requests
made for the old text are refused by its generation. A lookup goes first: it
cancels a refresh in progress, and a refresh starts only once no lookup has
started for 1.5 s and at least 2 s after the previous one. A refresh whose
screen changed under every read is tried again at the pace of any such job
([in-game capture](ingame-capture.md)); one that fails (the game's memory, say)
shows nothing and backs off the next refresh, 2 s doubling to 60 s, until a job
reads its screen again. That backoff is the refreshes' own (#95): after Cancel,
Auto reads a settled screen at once, as when no job ran while the overlay was
open. No refresh
runs while the self-test is armed. Region and game changes still retire the text.

In v0.7.10, a highlight change explicitly invalidates the header drawing after
updating its color styles. The applied range is committed only after the native
calls succeed; unavailable layout or failed updates get at most six frame
retries. A partial failure clears the header's color styles before retrying, so
a newer cursor or mode cannot leave an old word yellow. The header string and
scroll position are retained.

Tap a character in the recognized sentence at the top of the open overlay to
look up the word starting there. This selects the overlay's recognized header,
not an unrecognized glyph in the game's framebuffer. Japanese-only selection
applies equally to arrows and touch: a tap on Latin text, digits, punctuation or
Hangul has no effect when it is ON. A held finger produces one request; it must
be released before another tap. Touch already held while opening is ignored.

The overlay keeps Shell's front touch region active through normal and crop
modes. Its existing game input block still filters game touch calls. Entering
or leaving the crop editor does not briefly hand front touches back to the
game. Failed region activation is not marked active. How a close gives the
region back depends on what is in front then ([PS and resume](#ps-suspend-and-resume-104)).

Hit testing allocates no memory and runs only on a new touch. It uses the
header's measured PAF character starts, line count, content height and current
scroll. The header has one font size, so the line pitch is measured height
divided by line count, as in existing header scrolling. The trailing cell on a
line is bounded by the configured Japanese point width; it is not a separately
measured glyph bitmap. Blank trailing areas and clipped header rows have no hit.
CR omission, UTF-16 surrogate pairs, UTF-8 source offsets and wrapped lines are
handled without borrowing dictionary arena pointers outside the view lock.
Undocumented `LineInfo.frame` fields and `GetNextDownHit` are not used.

Host checks exercise proportional advances, wrapping, scrolling/clipping, blank
space, Japanese-only taps, CR/newline handling, supplementary Han and unavailable
layout. Existing worker tests verify that changing highlight mode preserves OCR
and that rapid selected lookups cannot publish stale definitions. Config tests
cover both modes and invalid values; transactional preference tests preserve
unrelated config bytes and exercise save/restore failures.

The header recoloring tests exercise the actual update helper against a text
double with cached drawing. They cover character-to-word expansion, character
mode, stale-color cleanup, failed layout/style/invalidation calls, invalid
ranges, and inflected source spans after CR and a UTF-16 surrogate pair.
These tests establish the explicit refresh contract, not native PAF behavior.

These checks validate the portable helper and worker/config integration. Native
PAF font geometry, finger hit accuracy and game touch routing require hardware
validation and are recorded separately from host results.

## PS, suspend and resume (#104)

SceTouch gives a touch to the frontmost process whose active region holds it
(SDK Touch Overview, "Ownership of Touch Data"). SceShell keeps its default
front region inactive while a game is in front, so the game has the panel, and
active while its LiveArea is (this project's model since 0.1, in
`shell/overlay.cpp`'s notes; inferred, not read from SceShell). The open
overlay activates it to read taps (for the whole overlay since v0.7.9, for the
crop editor before). Every close deactivated it. Pressing PS with the overlay
open closes the overlay too, but only after the game's suspend has reached the
Shell: the kernel's worker, the Shell's control thread, then the page's next
frame (within a frame or two, estimated and unmeasured; longer while the
control thread is inside a log append, 12-46 ms each on the console, #81).
test.1's `log.txt` shows
that order (`touch region off` after the game's exit ledger lines). The
deactivation then lands on the LiveArea's region, which SceShell has activated
for itself by that time (the inference from #104's symptom: nothing else of
ours reaches SceShell's input), and the LiveArea has no touch until SceShell
next sets it.

Now:

- **The game's input** is released in the kernel's process event hook itself,
  in the suspend's (or exit's, or any foreground change's) own call, before the
  firmware's handler runs (`kernel/lifecycle.c` `fg_store`). Held buttons stay
  hidden from the game until released, as at a normal close. The block only
  ever applied to the game's own pad and touch calls; SceShell's input was
  never filtered.
- **The overlay** closes in the Shell's next wakeup on the kernel's game-exit
  event (log.txt says why: `in game`, `game left` or `foreground changed`).
  If the game has left and SceShell is in front (PS, or the game exited), the
  page leaves SceShell's touch region alone: SceShell's own switch to the
  LiveArea owns it. Closed in game (Cancel, L + R, Select + R, a saved
  region), or when another process comes in front of the game, it deactivates
  the region as before, the state SceShell had over the game. Two signals say
  the game left, and either is enough (`shell/touch_hold.h`): the control
  thread's hand-back with the close (`shell/worker.c` `close_overlay_for`; the
  game's exit also turns an in-game close the page has not carried out yet
  into "leave it"), and the kernel's foreground as the page closes
  (`shell/overlay.cpp` `close_page` asks `vjoGetState`: no process in front).
  The kernel's foreground changes in the process event hook itself, so Cancel
  then PS before the page's next frame is covered too, with the control thread
  still behind. That assumes the kernel's suspend comes before SceShell's
  switch to the LiveArea: the hook runs in the suspend's own event, but
  SceShell's order is not measured.
- **Resume** starts closed: the overlay and the kernel's block (a confirmed
  game starts with its input free) are off until the next L + R, the subtitle
  strip until the next Select + R; SceShell's own switch back to the game
  gives the game the panel.

Each transition is logged with the system clock in ms (`t=`), the same clock in
both files, so a console run lines them up. A PS and resume with the overlay
open (illustrative numbers):

```
kernel.txt  foreground 0x62C03 -> 0x0 (suspend) t=1791614284 ms: input released
            game exit
log.txt     overlay close (game left) t=1791614286 ms: input block off rc=0
            touch region left to SceShell t=1791614299 ms
            overlay closed: 11269 frames, 6 word moves, ...
kernel.txt  foreground 0x0 -> 0x62C03 (resume) t=1791616400 ms
            game pid 0x62C03 active t=1791616410 ms
log.txt     game PCSG00415 started: dictionary ...
```

`overlay close (in game)` and `touch region off` follow a Cancel; `foreground
changed` a process starting over the game. The kernel's `input block=` lines
and the Shell's `input block on` carry `t=` too.

Host checks: `host/tests/test_kernel_lifecycle.c` runs the real process event
hook and foreground state machine (released before the firmware's handler
runs; SceShell's own and other processes' events keep an open overlay's block;
a block set while nothing was in front goes with the next game's
confirmation; an app never confirmed as a game logs nothing);
`test_shell_worker.c` runs PS, a stale L + R over the LiveArea, resume, Cancel,
a process over the game, and Cancel with PS through the real control thread;
`test_touch_hold.c` runs the page's rule against a model of the region in which
the old rule reproduces #104 and the new one leaves the LiveArea its touch
whichever of SceShell's switch and the page's close comes first, also when
Cancel's close reaches the page before the control thread has handled PS, and
when a process starts over the LiveArea before the page closes. The model is an
assumption about SceShell, the one that explains both the working header taps
over a game and #104. Whether the LiveArea's touch now
works at once, and the game's after a resume (SceShell's own switch back to the
game is what deactivates the region then), is for the console run.

## Rows and arrow keys

The arrows move by the header as it is drawn, not by the text's line breaks
([#81](https://github.com/bee-san/vita-jp-overlay/issues/81), requests 1 and 2;
the owner's rule of 2026-10-16 for Left/Right). A recognized line longer than
the 920 px box wraps onto a second displayed row; each row is its own row for
the arrows.

- **Left/Right** go in reading order through the whole text, across its rows,
  soft-wrapped and hard-broken alike. Right from a row's last word (Whole word)
  or character (Character) goes to the next row's first; Left from a row's
  first goes to the row above's last character. In Whole word mode Right skips
  the matched source span wherever it ends: a word that continues onto the next
  row (or, looked up whole, past a line break) is passed and Right lands on the
  first selectable character after its tail there. Left retraces the last 16
  Rights, back across the rows they crossed, and otherwise steps one character
  back: a word's start is known only from its lookup, which runs after the
  cursor lands, so Left from a row's first word without a Right to retrace goes
  to the row above's last character, whose lookup shows the word starting there.
- **The text's ends stop the cursor**: Right at its last selectable character
  (or last word) and Left at its first do nothing, so Left at the start of a
  text of several rows no longer goes to its far end (test.3's item G). One case
  keeps #81's wrap, deliberately: a text whose selectable characters are all on
  one displayed row (a short line) goes round its ends, Right from the last to
  the first and Left from the first to the last, as before; Left retraces such a
  wrap like any other Right. The left stick stops there too
  ([the left stick](#the-left-stick-138)).
- **Up/Down** go to the nearest row above or below that has a selectable
  character, a soft-wrapped part of a long line included, and there to the
  character whose cell is nearest the same horizontal position (inside it, or
  the nearest edge; the earlier on a tie), which can be inside a word, as a
  tap can. Repeated Up/Down keep that column while the cursor stays where the
  last one left it, so going down through a short row and on to a long one
  returns to it; Left/Right, a tap or new text take it afresh. A row change
  forgets Right's history, so Left retraces only the new row's words.
- **Japanese-only** applies to all four arrows, as to taps: with it ON the
  cursor lands only on kana and Han; OFF, on any visible character. The arrows
  never land on spaces (U+0020, U+3000), line breaks or control characters,
  and pass over a row of only those (an empty line, or with Japanese-only one
  of only Latin text and punctuation), which also never counts as a row of the
  text's own for the wrap.
- Dictionary paging, which used Up/Down, is an **L or R tap**: a shoulder
  pressed and released with no other button, so the L + R chord that closes the
  overlay and Select + L/R never page.

The rows are the header's own layout, as for taps: the laid-out line and start
x of each character from paf (`GetCharInfo`), with a row's last cell bounded by
the Japanese point size (27 px at the default `font_size_ja = 18`, 34 cells
across the 920 px box). Undocumented `LineInfo.frame` fields and paf's
`GetNextUpHit`/`GetNextDownHit` are not used (pressing ▼ with ScrollView
`DoSnap` and `GetNextDownHit` crashed the console). Left/Right walk the header's
text (at most 4,095 bytes) from its start to the character they move to and ask
paf for no character's position (only its line count, `GetLineCount`, as every
key and stick step over the header does); only a press at the text's end, which
could go round, asks for the rows of its first and last selectable characters
(two `GetCharInfo` queries). Up/Down walk it
to the row after the cursor's and ask for the cursor's character, the
selectable characters and, where a cell's end is needed, the character after
one (at most 2N + 2 queries for N characters). Neither allocates or takes a
lock. Before the header is laid out, or if paf reports no position, the line
breaks stand for the rows at the text's ends (a text with a line break between
its first and last selectable characters stops there, one without goes round),
and Up/Down do nothing; the cursor stays on a selectable character either way.
The overlay's close line in `log.txt` counts the row changes, wraps and presses
that needed a layout without one, and times the slowest key's walk (the
`GetCharInfo` queries included; not measured on a console yet), all the arrows'
(the stick's moves are counted apart); `--collect`'s analysis sums them
(`tools/analyse_selftest.py`). The format, with illustrative numbers:

```
overlay closed: 1158 frames, 19 word moves, stick 71, slowest frame 2 ms, 4 row moves, 2 wraps, 0 without layout, slowest key 412 us
```

`host/tests/test_text_rows.c` checks the rules on layout fixtures
(`host/tests/layout_double.h`, a renderer double with 10 full-width cells per
row): soft-wrapped and hard-broken rows, the column kept across a short row,
mixed full-width and narrow advances (Latin cells are half as wide), kinsoku
punctuation hanging past the edge, CR and UTF-16 surrogate pairs, a line break
before the first or after the last selectable character, unavailable layouts
and invalid positions, whole words crossing a row's end or a line break, and
Left's retrace. It then compares every arrow from every character of 120
generated texts (four box widths, Japanese-only ON and OFF, with and without
the wrap) with the rules restated over the laid-out cells (`ref_read_step`,
`ref_vertical`), counts the positions each press asks for, and checks that Right
from the text's first selectable character visits each one once in reading
order and stops at the last (or goes round, on one row), and Left the same way
back. `host/tests/test_cursor_nav.c` drives the overlay's frame over texts of
two and three lines, a line of one word, an empty line, a line without
Japanese, a soft-wrapped line, a word run on after a line break and one row,
with the d-pad and the left stick in each direction. The word lookup test
(`real_lines_navigate_displayed_rows`) walks the 50 real VOCR lines, laid out at
the default 27 px font size as recognized, as one joined line (soft wraps) and
in a narrow box: Whole word Right with the bundled dictionary's real match ends
goes through each text across its rows and stops at its last word (on one row,
round to its first), and Left undoes it. The shoulder-tap rule is in
`host/tests/test_word_navigation.cpp`. These are host checks of the portable
code against a layout double; the paf layout itself, the key feel and the
shoulder taps are for the console run.

## The left stick (#138)

Each arrow press looks its word up at once, and in Whole word mode Right skips
a word only once that word's lookup is shown: in the test.3 console run the 31
cursor lookups took 35-112 ms each (median 56 ms; the `lookup cursor` and
`job 0 done` lines from `log.txt` line 10630 on, in
`vjo-collect-20261016-071743Z.zip`), and the tester found crossing a long line
slow. The left stick now moves the cursor without looking anything up on the
way:

- **Speed, by the clock** (`core/stick_cursor.h`), as the Overlay app's lists
  since #107, #113 and #129 (`core/stick_scroll.h`): pushed more than half way
  (64 of its 128 counts), the stick moves the cursor one step at once; held, the
  next step comes after the lists' 166.7 ms, then after 125 ms, each later one
  12.5 ms sooner than the one before, down to one every 50 ms. That is 13 steps
  in the first second, then 20 a second, **the cap**: an estimate of what a
  reader can stop on, not measured on a console. How far the stick is pushed
  beyond half way does not change the speed, as in the lists. A frame's time
  counts at most 50 ms, so a frame moves the cursor at most once and a stalled
  frame never makes it jump, at 60 Hz, 30 Hz or anything between. The clock is
  SceShell's process time; only its low 32 bits are kept, whose differences are
  right across their wrap every 71.6 minutes.
- **The four directions are the d-pad's** (the owner, 2026-10-16): each step
  of the stick is handed to the arrows' own code as that arrow's press
  (`vjo_text_row_key`, `core/text_rows.c`, with its `stick` flag). Left and
  right go in reading order across the displayed rows and line breaks as
  Left/Right do: Right from a word whose lookup is shown skips that word; every
  other step is one character, because no lookup tells where the next word ends
  while the stick moves; Left retraces the Rights, the d-pad's and the stick's
  alike (one history), and otherwise steps one character back. Up and down go a
  displayed row, keeping the d-pad's column across a short row. Japanese-only
  applies as to the arrows. A diagonal counts as the axis pushed farther (a tie
  as left/right), and the one held keeps counting while it stays past half way,
  so a stick rolled round changes direction once. The one difference: the stick
  stops at the text's first and last character even on a text on one row, which
  the d-pad goes round, as the app's lists stop at their ends while their
  Up/Down wrap, so a held stick never circles a short line. Its Left still
  undoes a d-pad Right that went round there (Right at the last character to
  the first, then the stick left: back to the last), because any Left retraces
  the Rights before it; the stick itself never goes round.
- **The deferred lookup.** While the stick moves the cursor, the body says
  "Looking up words…" and the header highlights the character. The word at the
  cursor is looked up once, when the cursor has rested there for
  `stick_lookup_ms` (`config.ini`, 200 ms by default, 0-1000; 0 looks up as soon
  as the stick stops driving the cursor), counted from its last move with the
  steps' 1 ms slack for the microsecond clock (six frames at exactly 30 Hz read
  199,998 us: 200 ms): with the stick let go, or held against the text's end or
  a row it cannot leave. A held stick is never a rest, the 166.7 ms before its
  first repeat included. Flicks closer together than the rest give one lookup,
  after the last. Back on the word shown before the rest, nothing is looked up
  again: the word stays, its dictionary page included.
- **The d-pad and taps** keep one step (a tap, its character) and a lookup at
  once, and drop a lookup the stick left due. An arrow pressed takes its frame:
  the stick's step due in it is dropped.
- **The right stick** scrolls a long definition. Before #138 either stick did.
- A stick already pushed when the overlay opens, when the region selector
  closes, while there is no text yet (recognizing, an error) or when new text
  replaces the shown one (a quiet refresh) moves nothing until it is let back
  inside half way.
- **The close.** The next open keeps the overlay's cursor only when it is the
  worker's (the word the worker shows, or looks up) and nothing is due; else it
  takes the worker's cursor, as for new text (`vjo_stick_cursor_kept`, under
  the view lock). A word still due at the close is such a case, and so is a
  lookup the worker refused: Cancel posts its close for the control thread,
  which wakes every 50 ms, and `vjo_post_cursor` posts nothing meanwhile, nor
  while the overlay is closing; a rest that ended in those frames, or an arrow
  pressed in them, left the two apart, and the reopened overlay said "Looking
  up words…" with no lookup coming (#156's audit, B1).

The overlay's close line in `log.txt` (the debug build's) counts the stick's
cursor moves and its deferred lookups and names the rest time in force
(`stick %d` is now the largest deflection either stick reached, 0-128). The
format, with illustrative numbers (no console has logged the stick's fields yet):

```
overlay closed: 3733 frames, 2 word moves, stick 127, slowest frame 11 ms, 4 row moves, 1 wraps, 0 without layout, slowest key 128 us, 37 stick moves, 3 deferred lookups after 200 ms
```

`word moves`, `row moves`, `wraps`, `without layout` and `slowest key` stay the
arrows', taps' and dictionary pages' (the stick's steps take the arrows' code
with its `stick` flag, which leaves those counts alone, and are counted among
its own `stick moves`); `--collect`'s analysis adds
the stick's sums (`tools/analyse_selftest.py`,
`left stick (N of them): M cursor moves, K deferred lookups (after 200 ms at
rest)`).

`host/tests/test_stick_cursor.c` (26 cases, on the ARM lane too) drives the stick
by the clock at 20 to 60 Hz and on irregular 1-60 ms frames: the push, the pause,
the acceleration step by step (each step on the first frame due, with the app's
1 ms slack), never more than 21 steps in any second and 20 a second held (the
cap), one step at most for a 2 s frame, a clock going back, the clock's 32-bit
wrap (the same step times from just before it, a rest across it), the axes and
the latch. Then the overlay's frame over a cursor, with the overlay's helpers
(`vjo_stick_cursor_step`, `_shown`, `_new_text`, `_kept`) and a worker's cursor
that can refuse a post: one lookup per rest, 200 ms after the last move (199,998
us at exactly 30 Hz, not 1,001 us sooner), none while held, a stick held into
an end and rolled back moving again without a lookup at any rest, the owner's
150-250 ms range and 0, flicks, the text's end held, the d-pad's lookup at once
and its frame, back on the shown word, new text, and the close after a refused
post; and the stick's step along the text (the arrows' Left/Right without the
wrap) from every character of 200 generated texts against the rule restated.
`host/tests/test_cursor_nav.c` (13 cases, on the ARM lane too) compares the
stick's four directions with the d-pad's from every selectable character of
nine multi-line and one-row layouts, in both highlight modes (1,520 moves: the
same character, the d-pad's looked up at once, the stick's once it rests), and
drives diagonals, the stick held across lines and rows to the text's ends, and
the history the two share. `host/tests/test_shell_worker.c` runs the
close with the real worker: the audit's B1 sequence (a flick, Cancel 183 ms
later, the rest ending while the close waits: the reopened overlay shows the
worker's word, and the stick goes on from there), the close before and after the
rest, and `stick_lookup_ms` reaching the view. 44 mutants of `core/stick_cursor.h`
and the step, and 4 of the close and the view's copy against the worker, each
fail a test. `input_overlay` and `close_page` themselves are paf code no host
target builds: the tests replay their calls around the real helpers.

In test.3 the overlay drew at 59.5-59.9 Hz (the two sessions with arrow moves:
6,882 frames in 115.0 s and 3,733 in 62.8 s between the page's touch-region
on and off lines), so the cap is a step every third frame there. The stick's
feel, and whether 20 steps a second overshoots, are the console run's.

## Word lookup on recognized text

`host/tests/test_word_lookup.cpp` checks the whole path on real recognizer
output: 50 VOCR lines of development screenshots
([fixtures](../host/fixtures/word_lookup/README.md)) go through the overlay's
sentence filter, the first Japanese letter, the lookup at the cursor with the
bundled `plaintext.vjdict`, the header range the overlay recolors, the popup
entry, and the Shell's own Right/Left navigation and touch hit test. Every
lookup gives one entry or none, the highlight starts at the cursor and ends on
a character boundary, and Right always advances. With the bundled dictionary,
1,319 of the 1,321 Japanese letters lie inside a matched word (614 entries in
616 lookups; 617 in 619 before a word could cross a line break the text joins:
five do on these lines, 呼び / ましょう and 頑 / 張ろう among them, and 彼氏様 /
だもんね's 様だ, as the unbroken line gives it;
[words split over two lines](#words-split-over-two-lines)); a character
without an entry stays highlighted and Right steps one character. The 56 cases of `expect.tsv` cover inflections
(会ってなかった → 会う, 殺されてしまう → 殺す, した → する), 静かな (静か; な is
the next word, as in Yomitan), kana words (から, して, ありがとう), katakana loanwords,
names, common readings (私 わたし, 先生 せんせい) and OCR errors (FL10's
美味しいつて still finds 美味しい; H15's 脱組んで, for 腕組んで, falls back to 脱). The Shell worker
test runs one line through Auto OCR (the first word prepared while the
overlay is closed, shown on opening without file I/O), Right and a tap.

Every lookup writes one `log.txt` line, for the console run. The Shell worker
test's lines (the host test's clock does not advance, hence 0 ms):

```
lookup first word at 0: 2 chars, dictionary 1/1, page 1/1, 5 reads 16703 B, rc=0, 0 ms; "伊佐那社" -> 伊佐 (いさ) [Bee's Ultimate Dictionary] unranked
lookup cursor at 6: 1 chars, dictionary 1/1, page 1/1, 5 reads 16496 B, rc=0, 0 ms; "那社" -> 那 (だ) [Bee's Ultimate Dictionary] #38948
lookup cursor at 59: 7 chars, dictionary 1/1, page 1/1, 5 reads 16948 B, rc=0, 0 ms; "会ってなかったか" -> 会う (あう) [Bee's Ultimate Dictionary] #134
```

That is why (`first word` during Auto OCR, `on open`, `cursor` for an arrow
or a tap, `page` for an L or R tap), the cursor byte, the matched span in
characters, the dictionary file (`?` among several with one shown), the page,
the file reads and bytes, the result and the time; then up to 8 characters
at the cursor, the entry, its dictionary's name (the file's title, as the bundled
Bee's Ultimate Dictionary has, else the `[name]` first line a converted
dictionary gives its definitions) and its frequency badge. The numbers come
first and the texts are bounded in whole characters, so a line is at most 246
bytes even with 4-byte characters in every field and a 63-byte title (a host
test) and `vjo_log` never cuts it. On the host the lookup takes
45 µs on average and at most 38 file reads
([measurements](benchmarks/word-lookup-20261014.json)); one A9 core at 444 MHz
is **estimated** at about 3–15 ms on average and under 45 ms at worst, mostly
memory-card reads. The console's log lines measure it.

## Words split over two lines

A game wraps its dialogue where a line is full, often inside a word: どうしてお母 / さんも, 他の人間を求 /
める姿を, だったら、プロ / ポーズみたい (vita-test). The overlay reads the recognized lines as the one running text they are
(owner, 2026-10-16, item 4: "watashi wo tabe / ru kana?" is taberu):

- **Which breaks join** (`core/textjoin.h`). A break joins the lines on either side when the line
  before it is full: as wide as the text's widest line within two characters (a mark pushed to the
  next row or hanging), at least eight characters wide (a half-width character counts half), not
  ending with a sentence's end or a closing bracket (。．！？!?」』）】…), and the next line not
  starting with an opening one (「『（【…). So a speaker's name line (short, or in brackets), a new
  sentence and a line the game broke short keep their break.
- **The lookup** (`core/local_dict.c`) crosses a joining break as if the lines were one: the key
  and the text as written leave the break out, the matched span keeps it. At every character it
  finds what it finds in the unbroken line (host test `joined_lines_match_as_one_line`). The word's
  highlight runs over both parts, the end of one line and the start of the next.
- **The cursor**: in Whole word mode Right skips the joined word, as any matched word, to the first
  character after its end on the next line, and Left retraces it ([rows and arrow keys](#rows-and-arrow-keys)).
- **Anki**: the card's Sentence is the text with its joining breaks removed, the word in it
  (`core/anki.c`, at export in the Overlay app); the queue's record keeps the text as recognized,
  so a card queued before this build exports joined too. A name's line keeps its own `<br>`.
- The header, the strip and the log show the lines as recognized, breaks and all.

How often it happens, measured on the real line sets (`tools/cross_line_eval.py`; text only, the
private sets' text stays local): a pair is two consecutive lines of one text box, and a word
crosses its break when the overlay's longest-match segmentation of the first line, over the two
joined, has a word that starts on the first line and ends on the second (the raw count, before the
join rule). Then how many of those breaks the rule joins, and how many of the breaks after a
speaker's name line it joins.

| set | lines | line pairs | a word crosses the break | breaks joined | crossings joined | breaks after a name joined |
|---|---:|---:|---:|---:|---:|---:|
| vita-test (16 games) | 206 | 122 | 20 (16.4 %) | 46 of 120 | 18 (90 %) | 0 of 53 |
| psp-test (14 games) | 245 | 152 | 12 (7.9 %) | 39 of 152 | 9 (75 %) | 0 of 61 |
| CLANNAD long run | 3,048 | 1,002 | 104 (10.4 %) | 167 of 989 | 99 (99 %) | 0 of 815 |
| realplus-v3 (training pool) | 100,915 | 28,943 | 5,225 (18.1 %) | 11,146 of 28,943 | 4,974 (95 %) | 3 of 2,432 |
| all | 104,414 | 30,219 | 5,361 (17.7 %) | 11,398 of 30,204 | 5,100 (95 %) | 3 of 3,361 |

So about one line in six that has a next line in its box ends inside a word: 5.1 % of all
lines, counting the last lines of boxes and one-line boxes. Not every such crossing is a word the
game split: on review, 56 of 60 sampled realplus-v3 crossings that the overlay shows across a
joined break were split words or grammatical units (the other four: longest matches across a phrase
boundary, as the unbroken line gives them), and 22 of the 32 vita-test and psp-test ones, one
more doubtful (the rest are longest matches across a phrase boundary, きた / おばあ様 → きたお). The rule leaves the
crossings it does not join at a short or hand-broken line (次のコンサートも / いい演奏), a name's line or
a line clearly shorter than the box's widest. Realplus-v3's boxes are line labels grouped by
position (consecutive labels just below each other), so its counts include some lines that are no
dialogue box. [measured: host build of this change, the bundled dictionary, 2026-10-16]

`host/fixtures/cross_lines` holds 56 of the reviewed realplus-v3 boxes, each a word looked up
whole across its break (`split_words_are_looked_up_whole`), and 32 breaks that must stay
(`kept_breaks_stay_breaks`: 20 after a name, 12 at a sentence's end); `host/tests/test_textjoin.c`
the rule, `test_local_cursor.c` the lookup, its pages, its highlight and the card, all on
synthetic texts too.

## The overlay's own line breaks

Where the overlay wraps text itself it never breaks inside a word (the owner: "we shouldn't split a
word down the middle either"). `core/linebreak.h` says where a line may break: after a space, after
punctuation or a closing mark, before an opening mark, and where a Japanese phrase starts after kana
(a kanji or katakana after hiragana, 彼女は|学校へ, but not after the prefixes お ご, not after one
hiragana between kanji that is no particle, 振り返る, and not before a suffix such as 様 達); never
inside a run of kanji, katakana or kana, between kanji and its kana, inside a Latin word or number,
or between Latin and Japanese; and by the Japanese line-breaking rules (kinsoku): no line starts
with 、。」』）！？…ー っ ゃ and the like, none ends with 「『（.

- **The recognized text, the popup and the subtitle strip** are paf texts that paf wraps (word wrap
  and kinsoku on, which still breaks Japanese anywhere). Once a text is laid out, the overlay looks
  for the first line paf started inside a word (`shell/wrap_fix.h`) and puts a line break at the
  last place that line may break instead, sets the text again, and repeats a frame later, up to 16
  breaks a text. Only paf's own layout of the text is read (its lines and character positions), so
  the console's glyph widths decide. The strip cuts a text taller than the screen only once that
  has settled, so the lines it keeps end between words too. A word wider than the line keeps paf's break. The header's
  breaks are mapped back to the recognized text's own characters for the cursor, taps, arrows and
  the highlight. The popup's definitions, mostly English, already break at spaces; its Japanese
  (headwords, examples, kanji lines) is what this changes.
- **The Overlay app**'s rows (`settings/text_layout.c`) end at the last space that fits, as before,
  or later where a line may break: a Japanese game title no longer breaks inside a word, and Latin
  text breaks as before (host test `latin_text_breaks_as_before`).

Host tests: `test_linebreak.c` (the places, kinsoku, the UTF-16 and UTF-8 walks), `test_wrap_fix.cpp`
(the re-wrap on a model of paf's layout: greedy lines, Japanese broken anywhere, Latin at its last
space, a layout missing or one text behind; the header's unit map), `test_settings_layout.c` (the
app's rows). Not verified on a console: how soon paf lays a text out after it is set (the step
waits two frames, then up to six more), that a position past the end of an older layout is refused
(which tells a layout of the text before the last break; one read too early can only end a line
early, never inside a word), and the frame time it takes (a binary search of the text's units per
line, at most once a frame per text) [estimate: well under a millisecond for the header, unmeasured
for a long definition; the debug build's close line logs the slowest frame].

## Installation checkpoint

After the v0.7.9 checkpoint, the user reported that Whole word still showed one
highlighted character while a definition was displayed
([issue #41](https://github.com/bee-san/vita-jp-overlay/issues/41)). They later
clarified that the word appears when lookup finishes and requested word jumps.
The v0.7.10 changes prepare/reuse the first result and navigate matched spans;
they have not been deployed to or visually
verified on the physical Vita.

[PR #38](https://github.com/bee-san/vita-jp-overlay/pull/38) merged after checks
at `ee796f1ea683d84a668bbc52961bf146a9f9986c`: 22 host suites, native build/gates
and a separate ASan/UBSan run of the actual `text_hit` helper's three scenarios.
The audited v0.7.9 release bundle has 60 manifested payloads plus one manifest.
Installed Shell and Settings executable readbacks match that candidate, and
Settings is registered and confirmed in the native foreground. The uncertain
promotion status is preserved separately from that reconciliation.

Four actual native Settings actions passed complete config readbacks: touch
**Character** → Left to **Whole word** → Right to **Character** → touch
**Whole word**. VOCR, Japanese-only ON and all unrelated bytes were retained;
the final configuration selects whole-word highlighting. All six independently
downloaded public release assets match their audited candidates and checksums.

Settings-card taps are separate from recognized-header taps in a game.
Fresh native inventory confirms the installed Shell plugin loaded. In-game
touch/highlight tests remain pending; no native finger-accuracy, glyph-bound,
word-span or game-routing result is claimed from the host checks or Settings saves. See
[the complete install checkpoint](todo-20261011.md#v079-touchhighlight-installation-checkpoint).
