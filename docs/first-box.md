# The text box suggested on first use (#358)

A game with no box of its own is read on its whole screen. That reads the picture, the buttons and the menus as well
as the dialogue, and first-time users took the result for recognition that did not work: no box had been drawn. So
the first time the overlay shows such a game's text, Rubi draws a box around its dialogue and asks **Use this box?**
Enter takes it ([the guide](guide.md#the-box-rubi-suggests) has the controls). Nothing of it has run on a console yet;
everything below was measured on a computer.

## When it is offered

- The game has **no line in `region.ini`** (no box saved with □, no full screen saved by holding □), and no line for
  every game (`region = ...`, which the Rubi app's **Suggest a text box** OFF writes as `region = full`).
- A job of the game worker read the **whole frame** with rows (not columns) and returned a box (`VjoGameOcrResult.
  suggest`, API 52). The Shell keeps the newest such box in its view, for the game's activation (its start or resume).
- The overlay is open, not drawing a box, and has not offered a box in this activation. After the frame's buttons it
  opens the region mode with the box drawn (`vjo_rsel_preset`: its far corner must be on the screen), the hint line
  "Use this box? Enter yes · Cancel draw your own" (the console's own glyphs).

Enter saves the box as one drawn with □ is, and the overlay reads it at once (`VJO_CMD_USE_BOX`: the region command,
then the open's own path, "Recognizing…"). Cancel takes the box's corner back, as for any box: drawing by hand from
there, and Cancel again leaves the whole screen. Holding □ still saves the full screen.

## The rule

In the game worker, on the job's recognized lines (`core/region_suggest.c`, `vjo_region_suggest`):

1. A line counts when its text holds at least **2 kana or kanji** (U+3040-30FF, U+4E00-9FAF, as the Shell's line
   filter counts Japanese) and its height is a dialogue line's: **12-90 px** of a 544-px frame.
2. The lines are grouped top to bottom: a line joins the one above it when the **gap is at most the taller one's
   height**, their **heights are within twice** each other, and they **overlap across or start within 40 px** (of
   960) of each other.
3. The group with the **most kana and kanji** is the dialogue (the upper one on a tie), with at least **6** of them;
   else no box (a title screen, a menu, a picture).
4. The box is the **frame's width**, from **half the group's median line height above its first line** to the
   **frame's bottom**, at least 32 px tall.

Why a band to the bottom and not the lines plus a margin: a box saved once serves every later screen of the game, and
the first screen's dialogue is often one short line. A box drawn tight around it leaves out the longer and the extra
lines of the screens after it (the pool's own tight boxes, measured the same way, hold 57 % of the later screens'
lines). The band costs reading what lies below the dialogue (the box's buttons and name plates), which the line
filter drops when it holds no Japanese.

## How it was chosen and measured

`tools/firstbox_eval.py` runs the game worker's job (`host/page_ocr.c`, config.ini's defaults: VOCR5-N, the 1000k LM,
the cascade, `ocr_line_fix = on`, `ocr_vertical = auto`) on whole pages of vita-vn-ocr's hooked page pool (vocr6-ingest
`pagepool/v1`, verified dialogue line boxes): 60 pages of each of 42 titles (2,421 pages; the three titles that share a
release with a protected set and the one test title are never read), a PSP page as Adrenaline shows it, 2x. A page's
box is scored on **the same title's other pages**, as a saved box serves later screens. The titles are split in two
by a hash of their ID; the rule's parameters were chosen on half 0 (22 titles), and half 1 (20 titles) is the
held-out report.

| Box | Later dialogue lines inside (half 1) | Worst title (half 1) | Other lines read per screen | ...with Japanese | Screen area | Offered |
|---|---:|---:|---:|---:|---:|---:|
| **the suggested band** | **96.6 %** | 88.4 % | **0.89** | **0.21** | 37 % | 92 % of pages |
| the whole screen (no box) | 100 % | 100 % | 3.45 | 0.83 | 100 % | |
| the page's dialogue + 12 px (a tight box) | 57.5 % | 48.0 % | 0.02 | 0.004 | 10 % | |

On all 42 titles: 95.7 % of later lines inside, 1.01 other lines read (whole screen 3.58), 33 titles at 95 % or more.
The page's own dialogue lies wholly inside its box on 97 % of the pages offered one. A line is inside with 95 % of its
box; a recognized line counts as read with half of it inside.

**Where it does worst** (half 0, the titles the rule was chosen on): dialogue that moves about the screen (PCSG00751,
63 %: lines placed by the scene), a full-screen novel layout (PCSG00648, 75 %), and a speaker's name in 【】 on a row
of its own above the text, which the pool counts as a dialogue line (NPJH50297, 73 %; 97.6 % without those rows).
There the user can draw a box with □ as before; the overlay asks once per activation.

Not measured: on a console (the time, how the question feels, real games' first screens), and games outside the
pool's 42. The rule reads only what the job already returns (no pixel, no allocation): a pass over at most 16 lines
after the job's recognition, not timed. It adds 1,104 B of code to the worker and 120 B to the normal Shell (below).

## Memory

No new block, thread or allocation. The box rides in the result (8 B, API 52), the Shell copies it into its view (8 B
and an activation number), and the region selector draws it as a box it already knows how to draw. Pages: none
change (`docs/memory-ledger.md`, "The box suggested on first use").
