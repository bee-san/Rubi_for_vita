# Touch selection and highlighting

Overlay app → OCR & lookup → Highlight chooses **Whole word** (default) or
**Character**. Local dictionaries show one entry at a time; with
**Dictionaries shown** above 1, an L or R tap pages to other dictionaries'
entries at the same character ([details](local-dictionaries.md#dictionaries-shown)).
Each page highlights its own match, and Right skips the one shown. In Whole
word mode, Right skips the current matched source span and Left retraces up to
16 recent forward selections. Without a match or previous selection, movement
falls back to one Unicode character. Character mode always steps one character.
Both stay on the header's displayed row and wrap at its ends, and Up/Down
change rows ([rows and arrow keys](#rows-and-arrow-keys)).
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
shows nothing and backs off as any OCR job, 2 s doubling to 60 s. No refresh
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
([#81](https://github.com/bee-san/vita-jp-overlay/issues/81), requests 1 and 2).
A recognized line longer than the 920 px box wraps onto a second displayed row;
each row is its own row for the arrows.

- **Left/Right** stay on the cursor's row. Right from the row's last word
  (Whole word) or character (Character) goes to the row's first, Left from
  its first to the row's last character. In Whole word mode Right still skips
  the matched source span, a word that continues onto the next row included:
  Right then wraps to the first word of the cursor's row. Left retraces the
  last 16 Rights, a wrap among them, and otherwise steps one character back:
  a word's start is known only from its lookup, which runs after the cursor
  lands, so Left from a row's first word goes to the row's last character and
  that character's lookup shows the word starting there.
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
  and pass over a row of only those (or, with Japanese-only, of only Latin
  text and punctuation).
- Dictionary paging, which used Up/Down, is an **L or R tap**: a shoulder
  pressed and released with no other button, so the L + R chord that closes the
  overlay and Select + L/R never page.

The rows are the header's own layout, as for taps: the laid-out line and start
x of each character from paf (`GetCharInfo`), with a row's last cell bounded by
the Japanese point size (27 px at the default `font_size_ja = 18`, 34 cells
across the 920 px box). Undocumented `LineInfo.frame` fields and paf's
`GetNextUpHit`/`GetNextDownHit` are not used (pressing ▼ with ScrollView
`DoSnap` and `GetNextDownHit` crashed the console). A key press walks the
header's text (at most 4,095 bytes) from its start to the row after the
cursor's. It asks paf for the cursor's character, the selectable characters
and, where a cell's end is needed, the character after one (at most 2N + 2
queries for N characters), allocates nothing and takes no lock. Before the
header is laid out, or if paf reports no position for a character, Left/Right
step through the text as one row and Up/Down do nothing;
the cursor stays on a selectable character either way. The overlay's close line in
`log.txt` counts the row changes, wraps and presses without a layout, and times
the slowest key's walk (the `GetCharInfo` queries included; not measured on a
console yet); `--collect`'s analysis sums them (`tools/analyse_selftest.py`). The
format, with illustrative numbers:

```
overlay closed: 1158 frames, 19 word moves, stick 71, slowest frame 2 ms, 4 row moves, 2 wraps, 0 without layout, slowest key 412 us
```

`host/tests/test_text_rows.c` checks the rules on layout fixtures
(`host/tests/layout_double.h`, a renderer double with 10 full-width cells per
row): soft-wrapped and hard-broken rows, the column kept across a short row,
mixed full-width and narrow advances (Latin cells are half as wide), kinsoku
punctuation hanging past the edge, CR and UTF-16 surrogate pairs, unavailable
layouts and invalid positions, whole words crossing a row's end, and Left's
retrace of a wrap. It then compares every arrow from every character of 120
generated texts (four box widths, Japanese-only ON and OFF) with the rules
restated over the laid-out cells, and checks that Right from any selectable
character visits each one of its row once and returns. The word lookup test
(`real_lines_navigate_displayed_rows`) does the same on the 50 real VOCR lines,
laid out at the default 27 px font size as recognized, as one joined line
(soft wraps) and in a narrow box: Whole word Right with the bundled dictionary's
real match ends stays on its row and wraps to the row's first word, and Left
undoes it. The shoulder-tap rule is in `host/tests/test_word_navigation.cpp`.
These are host checks of the portable code against a layout double; the paf
layout itself, the key feel and the shoulder taps are for the console run.

## Word lookup on recognized text

`host/tests/test_word_lookup.cpp` checks the whole path on real recognizer
output: 50 VOCR lines of development screenshots
([fixtures](../host/fixtures/word_lookup/README.md)) go through the overlay's
sentence filter, the first Japanese letter, the lookup at the cursor with the
bundled `plaintext.vjdict`, the header range the overlay recolors, the popup
entry, and the Shell's own Right/Left navigation and touch hit test. Every
lookup gives one entry or none, the highlight starts at the cursor and ends on
a character boundary, and Right always advances. With the bundled dictionary,
1,317 of the 1,321 Japanese letters lie inside a matched word (625 entries in
629 lookups); a character without an entry stays highlighted and Right steps
one character. The 56 cases of `expect.tsv` cover inflections
(会ってなかった → 会う, 殺されてしまう → 殺す, した → する), 静かな (静か; な is
the next word, as in Yomitan), kana words (から, して, ありがとう), katakana loanwords,
names, common readings (私 わたし, 先生 せんせい) and OCR errors (FL10's
美味しいつて still finds 美味しい; H15's 脱組んで, for 腕組んで, falls back to 脱). The Shell worker
test runs one line through Auto OCR (the first word prepared while the
overlay is closed, shown on opening without file I/O), Right and a tap.

Every lookup writes one `log.txt` line, for the console run. The Shell worker
test's lines (the host test's clock does not advance, hence 0 ms):

```
lookup first word at 0: 2 chars, dictionary 1/1, page 1/1, 7 reads 20837 B, rc=0, 0 ms; "伊佐那社" -> 伊佐 (いさ) [Daijirin4 Plain]
lookup cursor at 6: 1 chars, dictionary 1/1, page 1/1, 6 reads 16564 B, rc=0, 0 ms; "那社" -> 那 (な) [Daijirin4 Plain]
lookup cursor at 59: 7 chars, dictionary 1/1, page 1/1, 23 reads 86633 B, rc=0, 0 ms; "会ってなかったか" -> 会う (あう) [Daijirin4 Plain]
```

That is why (`first word` during Auto OCR, `on open`, `cursor` for an arrow
or a tap, `page` for an L or R tap), the cursor byte, the matched span in
characters, the dictionary file (`?` among several with one shown), the page,
the file reads and bytes, the result and the time; then up to 8 characters
at the cursor, the entry and the source named in its definition. The numbers
come first and the texts are bounded in whole characters, so a line is at
most 228 bytes even with 4-byte characters in every field (a host test) and
`vjo_log` never cuts it. On the host the lookup takes
45 µs on average and at most 38 file reads
([measurements](benchmarks/word-lookup-20261014.json)); one A9 core at 444 MHz
is **estimated** at about 3–15 ms on average and under 45 ms at worst, mostly
memory-card reads. The console's log lines measure it.

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
