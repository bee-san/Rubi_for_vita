# The overlay on the game's text (API 37)

An opt-in mode in which the overlay's work happens on the game's own text, as GSM does on a PC: the word at the
cursor is marked where the game shows it, a tap on the game's text looks up the word there, and the dictionary
popup sits beside the word instead of covering the screen. It is off by default: **Popup on the game's text**
on the Rubi app's OCR & lookup page, `overlay_on_text = on` in `config.ini`. Nothing of it has run on a console
yet; the things to check there are at the end.

## What it does

When the overlay opens on text that Rubi read by OCR, and the text comes with its characters' places (the char
map, below), the overlay:

- **marks the word at the cursor** on the game's text, one translucent rectangle for each line it spans (two at
  most; Whole word mode marks the matched word, Character mode the character);
- **shows its reading above it** (furigana) when the dictionary entry's spelling is not its reading, on a dark
  band over the line above the word, when there is room;
- **puts the popup beside the word**: a band 196 px tall, the screen's width, below the word when it fits there,
  else above it, else at the screen's edge farther from the word. The band holds the dictionary body and the hint
  line. The panel's header, which repeats the game's text, is not shown: the game's own text is the header;
- **takes a tap on the game's text**: the character there becomes the cursor, as a tap on the header's text does
  in the normal popup. A tap on the popup's band or the reading's band, or elsewhere, does nothing (the band scrolls
  with the right stick, as before); in the full popup (no places) taps work on its header as before;
- **moves Up and Down across the game's lines**: to the character of the line above or below whose cell's
  centre is nearest. Left, Right and the left stick move in reading order, as before. Up on the top line does
  what it does now (line history, below).

It falls back to the normal full-screen popup, with nothing drawn on the game's text, whenever it has no place
for the text:

- the text has no char map: a native hook's text when no OCR ran in the job, or a line of it that does not
  align with what OCR read (fewer than half of its characters equal), or a line beyond the map's 16 lines and
  512 characters;
- the screen no longer shows what the map was read from: a new scene or a new line. The marks go at once, and
  Auto mode's quiet refresh brings the new screen's text with its own map;
- an earlier line (line history), the region selector or the line view (△).

Turning the setting on or off retires the shown text, so the next open reads the screen again, with or without
its map.

**With the setting on, every job reads its lines without the line cache**: a cache entry keeps the text, not the
places, so a line that would have come from the cache is recognized again. A quiet refresh of an unchanged screen,
for example while the "next" icon animates, then costs a full recognition, about 0.3-0.5 s a line on the game's
cores (estimate) instead of cache hits. On the owner's test.4 log at least 72 of 482 lines came from the cache, about
15% [measured: the log's `par` steps, audit-overlay-boxes]. Keeping the places in the cache entries would remove
this; it is a larger change and is not done here.

## How the places are made

**In the game worker** (the game's process, never SceShell):

1. With `VJO_GAME_OCR_S_CHARS` in the request's speed bits (the Shell sets it for the user's own job when
   `overlay_on_text` is on), every recognized line's greedy characters get a cell along the line from vita-vn-ocr's
   `runtime/vocr_charbox.c` (#114): the CTC frame each character fires at, mapped through the line's resize, with
   the model's calibration of where its CTC fires. The worker asks for the cells of a find **without INK** (no
   pixel is read again): they tile the line, and on vita-vn-ocr's development lines they pick the right character
   for 99.58-99.89% of taps at a glyph's middle, against 98.84-99.47% for INK's cells, which inside a …… can hold
   one dot (vita-vn-ocr `docs/char-boxes.md`, "Hit-test"; report only, measured on the host). The centres of
   VOCR5-N fall inside the character's reference box for 98.3-98.9% of development characters with these
   cells' calibration (fixed lines), and on synth4's exact glyph boxes 99.6-99.7% of them fall in the exact
   cell (vita-vn-ocr `eval/charboxes_synth_20261023.json`, the `cal` arm). With the LM on, the top-K's top-1 is the frames' classes; without it, the run's own best classes.
2. After the hook selection, the worker writes the **char map** of the text it returns (`core/charmap_build.c`):
   it reproduces the sentence the Shell will show (the non-Japanese line filter, then the trim:
   `core/textfilter.c`, `vjo_java_trim`), matches each kept line to the next of at most four recognized lines
   whose characters align with at least half of its own (`vocr_charbox_align`), and gives each character the cell
   of the recognized character it aligns to. A character the recognizer did not read itself (the beam + LM's
   edit, the text normalisation's, or a hook text's) gets a cell of zero width at its neighbour: it is in the
   word's mark but holds no tap.
3. The map goes after the text and its recognizer's copy in `VjoGameOcrResult.text`, whose last bytes are zero
   otherwise: no struct grows, and the result's last byte stays zero for the kernel's check.

**In the Shell** (UI only): `run_vocr` and the quiet refresh carry the block after the sentence's NUL in
`local_text` when it was made for exactly that sentence (length and FNV-1a), with no lookup and no allocation
(`vjo_cmap_carry`); the open overlay copies it with the text into its UI block and draws from that copy.

**Coordinates.** The worker writes cells in the game frame's pixels (the region's origin added); the overlay maps
them to the 960 × 544 screen: a PSP game's 480 × 272 frame is shown 2×, Adrenaline's 960 × 544 frame is the
screen's own, and any other frame size is scaled to the screen.

## The block

`core/charmap.h` has the format: 16-bit little-endian words at any byte offset, a 16-byte header (`'C' 'M'`, the
version, the line count, the character count, the sentence's length and FNV-1a, the frame's size), then 8 bytes a
line (its rows, its first character and their count) and 6 bytes a character (its byte offset in the sentence and
its cell). At most 16 lines and 512 characters: 3,216 bytes. The reader refuses a block that does not tile its
characters in order, points outside the sentence, or runs past the buffer (`host/tests/test_charmap.c` flips every
bit of a block).

## Memory

Measured on the packaged modules of both builds against main `2f85de0` ([memory-ledger](memory-ledger.md),
"Overlay on the game's text"):

- **SceShell: the module's pages are unchanged** (176,128 B debug, 163,840 B normal, checked exactly). The overlay's
  copy of the cursor's text (`s_cursor_text`, 4,096 B) moved from the Shell's data segment into the open page's UI
  block, where the char map now follows it. That block is a PAF allocation made when the page opens and freed when
  it closes, so `VJO_BUDGET_PAF_UI` goes from 20,480 to 24,576 B. The data segment gives back the page the new code
  takes. No thread, stack or permanent allocation is added.
- **The game worker: one text page more in both builds** (180,224 → 184,320 B debug, 172,032 → 176,128 B normal),
  in the game's partition. The code is vita-vn-ocr's find and align and the char map's writer. The job's characters
  (`VjoVocrChars`: 256 characters, 16 lines and one line's box buffer, which the map's builder borrows) fit in its
  data pages. No new block, thread or allocation.

## Cost (estimates)

Instruction counts under qemu-arm (vita-vn-ocr's `insn_mix` plugin) of this repo's `core/charmap.c` and
`core/charmap_build.c` built with Arm GNU 14.2 and the Vita's flags (`-Os -mthumb -mcpu=cortex-a9`), minus a run
that only sets the text up; one instruction a cycle gives the times at 333 MHz. Estimates: nothing was measured
on a console.

| What | Where, when | Instructions | at 333 MHz |
|---|---|---:|---:|
| the cells of one line (`vocr_charbox_find` without INK) | the game worker, per recognized line | 10-27 K (vita-vn-ocr's count) | ≤ 0.1 ms |
| the char map of a 3-line, 90-character text (`vjo_cmap_build`) | the game worker, once a job | 33 K | 0.1 ms |
| carrying and checking it (`vjo_cmap_carry` + `vjo_cmap_find`) | the Shell, once a new text | 9 K | 0.03 ms |
| a tap's hit-test and a word's rectangles (`vjo_cmap_hit` + `vjo_cmap_span`) | the Shell, a tap or a key | 11 K | 0.03 ms |

A recognized line costs about 0.3-0.5 s, so the places' own work adds well under 1% to a job. The larger cost is the
line cache, which jobs with the setting on do not use (above): about 15% of lines on the owner's test.4 log would be
recognized again. The drawing is paf's: the
overlay sets the size and place of four planes (two marks, the reading's band and the popup's panel) on a key
press, a tap or a new text, never every frame; what paf's renderer takes for them and the frame pacing beside
the game are unmeasured.

## Limits

- Horizontal text only: no places for vertical columns.
- MeikiOCR's boxes are the development reference of the cells; a game font far from training's could place them
  worse; narrow punctuation is the least exact (vita-vn-ocr `docs/char-boxes.md`).
- A hook text gets places only for the lines OCR also read in that job and that align with OCR's reading.
- The reading is one line of kana above the word's first line, not per kanji, and a reading with `<` or `&` (a user
  dictionary's) is not shown.
- After a visit to an earlier line longer than the current text, the current text's map is overwritten in the
  Shell's buffer (line history reuses it): its marks come back with the next refresh. The reader refuses the
  overwritten block, so nothing wrong is drawn.
- A block is checked for its own consistency only: a corrupted one with rows or cells outside the frame would draw
  its marks off the text (inside the overlay's own planes).
- Nothing of it has run on a console: the popup's layout, the marks' alignment on the screen and the drawing's
  cost are host-built and unchecked there.

## To test on the console

With `overlay_on_text = on` (the Rubi app: OCR & lookup, **Popup on the game's text**):

- CLANNAD (PCSG00415): the marks sit on the word in the dialogue box, a tap on a character picks it, Up/Down
  move between the box's lines, the popup does not cover the word, the reading sits above it, and the marks go
  when the next line appears;
- one PSP game in Adrenaline: the same, on a 2× picture;
- the game's frame pacing with the overlay open (no stutter while moving the cursor), and how often the quiet
  refresh re-reads an unchanged screen (no line cache with the setting on);
- Up on the game's top line reaches the earlier lines when the first line is long (the hidden header's rows decide);
- logs: `log.txt`, `kernel.txt` and `tools/install_ftp.sh --collect`.
