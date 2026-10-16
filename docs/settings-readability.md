# Settings readability on the Vita

The standalone Settings app previously used five 56-pixel game rows and small
PVF drawing scales, with dense hints and a narrow page-switch touch area.
This change affects Settings only; the in-game overlay and OCR workers are
unchanged.

The 960 × 544 layout now shows three 80-pixel game cards. Game titles have two
measured-width lines; their IDs occupy a separate right column below a clear
ON/OFF badge. The page tabs have 48-pixel touch heights (the OCR card's
engine buttons went with Google Lens: [the OCR card](#native-installation-and-controls-checkpoint)). The focused card has a bright border. Save failures have a distinct
error color, and status messages have two lines above the control hints.
Icons are cached by title ID and loaded one per frame
([The Games list](#the-games-list)).

Games-page controls keep their existing actions. Touch can select either page,
toggle a game or setting, or choose a model size directly. Touching a row's
title, the status line or the gaps only focuses that row. The OCR card's last
line names the model size and its model file (since API 21 the separate help
line made way for the Model size row). Game policy and preference loading/saving are unchanged.
The app still re-reads after a save failure to display the actual saved choices;
a failure is never presented as a successful save.

The OCR & lookup page has a fourth card, **Dictionaries shown**, with eight
number badges (44 × 34 pixels, 4 pixels apart). To fit it, the three smaller
cards are 68 pixels tall, their badges 34 pixels, and the page's status is one
line. The OCR engine card and the Games page are unchanged. Touching a number
chooses it; the card's title and help only focus it.
[A Vita3K render](settings-dictionaries-vita3k.png) shows the page with three
enabled dictionaries and 2 selected. It used the official 3.74 firmware fonts
and a build that opens on this page, because injected keys did not reach the
emulated app; it is not a physical-device check.

## The Games list

The owner found the Games page laggy with many games installed. Three things
in `settings/main.c` made it so (read in the source, counted in the harness
below; none timed on a console):

- **Icons were decoded on the UI thread whenever the view moved.** `draw()`
  freed the three visible icons and decoded three PNG files from the memory card
  with `vita2d_load_PNG_file` before drawing, each time the list's top row
  changed: every scroll step at the list's edge and every L/R. `rebuild()` did
  the same after every toggle, Triangle and filter change. libpng reads a file
  in pieces, one `sceIoRead` per chunk header, chunk and CRC (14 per icon in the
  harness), and each load allocated and GPU-mapped a new texture block.
- **Every wrapped text was laid out again every frame.** `fit_line` measures
  each prefix of a row with `vita2d_pvf_text_width`, so a row of n characters
  costs about n²/2 glyph measurements; three titles, the status and the hints
  came to 3,323 measured glyphs in an idle frame of the harness's list (6,049
  with 512 games' longer titles). libvita2d measures a glyph with an atlas
  lookup and a `scePvfGetKerningInfo` call into the firmware's font library.
- **Textures were freed while the GPU could still read them,** with no
  `vita2d_wait_rendering_done` between the last draw and the free
  ([libvita2d #28](https://github.com/xerpi/libvita2d/issues/28)), and with two
  fields libvita2d never sets for a plain texture: its loader `malloc`s the
  struct without clearing it, and `vita2d_free_texture` destroys `gxm_rtgt` and
  frees `depth_UID` when they are non-zero (pinned `libvita2d.a`,
  `vita2d_free_texture`: offsets 24 and 96). Neither fault has been seen on a
  console. A damaged icon was decoded again on every move; libvita2d's loader
  does not free the texture when libpng fails inside the image, so each try
  kept one CDRAM block.

The catalog (`settings/catalog.c`) was already read only at start and on
Start; that is unchanged, and a host test counts the scans.

What the page does now:

- **An icon cache by title ID** (`settings/icons.c`): 9 slots, three times the
  three cards. The window is the visible rows, a page below and a page above;
  a frame loads at most one icon, the first of the window without one: the
  visible rows, then the page the list last moved towards, then the other,
  nearest row first. When the cache is full, the least recently used icon
  outside the window is freed first, after `vita2d_wait_rendering_done`. A
  card shows **JP** until its icon is loaded. Toggles, Triangle and filters
  keep the cache; Start frees it (after the GPU wait) with the new list.
- **Icons are read whole and checked first.** `vjo_icon_read` reads the file in
  one bounded read into a 256 KiB buffer; `vjo_icon_png_check` requires the
  PNG signature, IHDR first, at most 256 × 256 pixels, every chunk inside the
  file with a name libpng accepts, a matching CRC on every critical chunk, the
  image data as one run of IDATs (after the PLTE of a palette image), and IEND.
  Only then does `vita2d_load_PNG_buffer` (which has no length) decode it. An
  ancillary chunk's CRC is not checked, since libpng only warns on it and
  loads the image. The CRC and IDAT rules keep damaged files away from libpng's
  image decoding, where a failure would cost a texture; a file whose CRCs hold
  but whose deflate data is broken still fails there, once per scan. A missing,
  larger, damaged or non-PNG icon shows **JP** and is not tried again until
  Start. After a load, the texture's `gxm_rtgt` and `depth_UID` are cleared.
- **Titles are laid out once per scan**, by the same rule
  (`settings/text_layout.c`, `fit_line` moved there unchanged), and drawn
  from the stored rows. A frame with no icon to load lays out one title of the
  window. Other wrapped text, the status, the hints and the page header, goes
  through a 16-entry layout memo keyed by the exact text, width, size and row
  count. The `n/m` counter, which changes on every move, is drawn apart from
  the hints, right-aligned, so the hints keep their layout. The drawn strings
  are those of the old `wrapped_text`: a differential test compares them on
  168,000 generated layouts and on every card of every frame of the Games tests.
- **L/R page.** The view moves three rows and the selection keeps its row; at
  the last or first page L/R selects the last or first game. Held, L/R repeat
  like Up/Down (350 ms, then every 90 ms).
- **Left/Right jump between letters.** A title's letter is its first ASCII
  letter in upper case, `#` for other ASCII (digits, symbols, a space), the
  first kana of its gojuon row for kana (か for が and ご, あ for ゔ, か for ゕ
  and ゖ; katakana apart from hiragana: ア for ヴ, カ for ヵ and ヶ), and the
  first character itself otherwise (U+FFFD for a byte that is not UTF-8;
  `settings/letter.c`). The catalog sorts by that letter first (`#`, A–Z, then
  by code point: hiragana rows, katakana rows, other characters), then
  alphabetically with capital and small letters together, so each letter is
  one run of the list: by bytes alone, ゔ and ヴ would sort after ん and ン and
  `{|}~` after `z`, splitting their letters. Right goes to the first game of the
  next letter (the last game from the last letter), Left to the first game of
  the current letter, or of the previous one when already there; the game is
  shown at the top.

A pagination screen was not added: the per-frame work above no longer grows
with the number of games, and L/R move a page. Whether the page now feels
smooth on the console has not been checked.

### Memory

The Overlay app is a system application
([vita-memory.md §5.4](vita-memory.md#54-the-overlay-app-a-system-application)), and
libvita2d then allocates every texture as its own `USER_RW` block in the app's
50 MiB budget, rounded up to 4 KiB (pinned libvita2d a8f15ab: `vita2d.c` sets the
texture type to `SCE_KERNEL_MEMBLOCK_TYPE_USER_RW` in system-app mode;
[`utils.c`, `get_aligned_size`](https://github.com/xerpi/libvita2d/blob/a8f15ab09d5233f0a4e4ad0e8f6ade0da888cbed/libvita2d/source/utils.c)).
An icon is drawn from 4 bytes per pixel, so a 128 × 128 icon's texture is 64 KiB
and the largest accepted, 256 × 256, is 256 KiB: the cache holds at most
9 × 256 KiB = 2,304 KiB (2.25 MiB), against 768 KiB for the old three icons. If
`USER_RW` refuses, libvita2d's `gpu_alloc` tries `USER_CDRAM_RW` (rounded to
256 KiB) and then `USER_MAIN_PHYCONT_RW` (rounded to 1 MiB); a texture that gets
neither is NULL and the card shows the placeholder. Before the app became a
system application, its textures were `USER_CDRAM_RW` blocks of 256 KiB each in
the game CDRAM budget (`ScePhyMemPartGameCdram`), with `USER_RW` and phycont as
the fallbacks. These
figures are from the source; the app's free memory on the console, and whether
either fallback is ever taken, have not been measured. In the
app's own memory (`.bss`, Vita build): the file buffer 262,160 B, the title
rows 18,432 B (512 games × 36 B), the layout memo 5,068 B, the PNG CRC table
1,024 B, 1,024 B of failure and layout flags and 184 B for the cache's slots:
the Settings ELF's `.bss` goes from 1,308,464 to 1,596,352 B and its text from
471,276 to 474,740 B (main `8cf4f3a`). The app runs in its own process; no
plugin and no SceShell memory changes.

### Measurements (host)

`vjo-settings-bench` runs the real `settings/main.c` and `settings/catalog.c`
over synthetic installed games (generated titles and icons, 128 × 128 8-bit
palette PNGs compressed with zlib) and times every frame on the host; its
decoder inflates and expands each icon as libpng does. Text widths are stub
metrics. The "before" column is main `7ed16d8`'s `main.c` in the same harness
(`-DVJO_SETTINGS_BENCH_MAIN=…`). Medians of 5 runs, 100 games:

| | before | after |
|---|---:|---:|
| idle frame, glyphs measured | 3,323 | 61 |
| scroll (Down held 30+ rows and back): frames that move the view, mean / max ms | 0.517 / 0.581 | 0.166 / 0.220 |
| scroll: PNG decodes per view move | 2.9 | 0.91 |
| scroll: icon file reads in 440 frames | 2,352 | 106 |
| scroll: glyphs measured per frame, mean | 4,681 | 95 |
| scroll: most PNG decodes in one frame | 3 | 1 |
| icons alive, most | 3 | 9 |
| textures freed while the GPU could read them | 168 | 0 |
| toggle and filter changes: PNG decodes | 26 | 2 |

The milliseconds are this host's, not the Vita's: the console's PVF calls, its
memory card reads and its texture allocation are not modelled, and none of it
has been timed on the console. The counts are what the change removes there.
With 512 games the after column is the same; before, an idle frame measured
6,049 glyphs. The four tables (100 and 512 games, before and after) are in
[benchmarks/settings-gamelist-20261015.md](benchmarks/settings-gamelist-20261015.md). Run it with
`cmake --build build/host --target vjo-settings-bench && build/host/vjo-settings-bench 100 5`.

### Tests

- `settings_layout` (also on the ARM lane): the layout against the old
  `wrapped_text` (`host/tests/settings_wrap_reference.h`, verbatim from main) on
  168,000 generated texts, widths and sizes under three measure rules; the
  memo; the PNG check on every truncation of an icon and every single-bit change
  (against an oracle from the PNG spec: only a change inside an ancillary
  chunk, or one leaving it another valid ancillary type, still loads), lengths
  that would wrap 32 bits, sizes, chunk names, critical and ancillary CRCs,
  split and interrupted IDATs, PLTE order and missing IEND or image data, each
  check run on a copy that ends at an unmapped page, so an over-read faults
  without a sanitizer; the bounded file read; the cache's LRU order and window;
  every title's letter for the ASCII and kana ranges against a gojuon chart
  written in the test; the catalog order (each letter one run, a total order,
  the case tie-break) over shuffled titles with every edge initial.
- `settings_games`: the app over 1 to 520 synthetic games. Every card of every
  frame shows its own game's ID, the old layout's rows and its own icon or
  **JP**; at most one decode per frame; visible rows first, then the page in
  the direction of travel; no icon reloaded while it stayed in the window;
  scrolling back reloads nothing; after a move up (Up, L, Left) the page above
  is loaded before the page below; L/R pages and their ends; Left/Right stop
  at each letter, both ways, over titles with every edge initial; a held key
  repeats after 350 ms, then every 90 ms, and a key pressed while another is
  held acts at once; the list keys do nothing on the other pages; the hints
  keep 140 px for the right-aligned counter, cut with "..." when Latin glyphs
  are wider; the window's titles are laid out one a frame ahead, so a view move
  lays out none; toggles and filters reload nothing; broken icons are tried
  once; a game installed while the app runs appears only after Start, which
  rescans exactly once; 512 games stay within 9 icons and 2.25 MiB. Each
  texture follows libvita2d's rules: freed only outside drawing, after a GPU
  wait since its last draw, with its two stray fields cleared. Built against
  main `7ed16d8`'s `main.c`, 14 of its 16 cases fail; the other two (the
  key repeat timing, the list keys' page gate) guard behaviour main already had.
- `tools/vita3k_settings_smoke.sh`: the app's own code, unchanged, in Vita3K with
  a scripted controller over 120 synthetic games, as a crash check only
  ([offline testing](offline-testing.md#5-vita3k-precisely)).

## Font measurement

The public `vita2d_pvf_text_width` API measures wrapping, centering and ellipsis.
UTF-8 decoding determines every line boundary. The drawing scales target 24-pixel
body, 26-pixel game-title and 32-pixel header **em advances**, calibrated from
the largest positive scale-1 advance of `あ`, `漢` and `Ｗ`. An 18-pixel em fallback
is used if none can be measured. These are not measurements of firmware glyph
bitmap heights or bearings.

PVF's nominal `text_height` is not an adequate bitmap-height measurement. The
supported libvita2d interface exposes scale and font selection, but no public
PVF raster-size setter or underlying font handle. The app scales the existing
system-font raster; it can appear less crisp when enlarged. Titles, preference
content and status regions are clipped to their own boundaries. Actual firmware
font bounds and physical readability still require native visual review.

## Validation

A private host harness compiled the complete, unchanged Settings input/draw
loop with the real `core/games.c`, `core/utf.c` and `core/arena.c`. Bounded stubs
supplied controller/touch events, storage outcomes and PVF/GPU calls. Synthetic
seven-game data included Latin, Japanese and supplementary characters; no
installed-game names, screenshots or fonts were committed.

Twelve scenarios passed with Clang AddressSanitizer and UndefinedBehaviorSanitizer:
page navigation and list-end clamps; third-card touch; row/tab gaps; touch and
controller page switching; direct engine choices; Japanese-selection toggles;
game/preference save failures; unreadable preferences; empty filters; and UTF-8
wrapping. The command shape was:

```sh
clang -std=c11 -DVJO_HOST=1 -O1 -g -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Wall -Wextra -I /path/to/qa/stubs \
  /path/to/qa/harness.c core/games.c core/utf.c core/arena.c \
  -o /path/to/qa/harness
# Run scenarios 1 through 12, each in a separate process.
```

Private 960 × 544 previews replayed the actual draw calls using a local Noto CJK
font and approximate host advances. Games, OCR and failed-save views had no
viewport overflow in those previews. They validate the proposed layout under
those substitute metrics; they are not Vita screenshots, firmware glyph
measurements, or physical readability proof. Existing persistence tests and
the native Settings VPK build remain separate checks.

## Native installation and controls checkpoint

The v0.7.11 OCR selector was checked with 22 scenarios through the actual
Settings input/draw loop under AddressSanitizer and UndefinedBehaviorSanitizer.
These include all three direct engine choices, forward/backward cycling and
wraparound, selection/highlight changes retaining Lens, existing custom-engine
settings, failed/unreadable saves, and row gaps/help text making no changes.
Lens remains selectable without local model files, and its help identifies the
Wi-Fi requirement and image upload without reporting missing local weights.
Approximate host previews of all three choices fit the card and viewport;
firmware rendering remained unverified.
After v0.7.12 the Meiki choice was removed, after v0.8.0-test.1 (#81) the
VOCR2 trial, and with the overlay going offline (2026-10-15) Google Lens and with
it the engine row: the card has no engine choice, only the local engine's Model size.

**Model size (API 21).** The OCR card is one row, the **Model size**, since
Google Lens and its engine row were removed: the card is 94 pixels tall at
y = 112 (146 with the engine row), and the cards below move up with it
(`settings/main.c` `OCR_Y`, `OCR_H`, `SELECTION_Y`). The row has its title, with
"estimated" below it, and three 220-pixel buttons, 54 pixels tall and 13 apart
(from x = 232), Small, Normal and Large, each with a second line, its estimated
time for a 3-line textbox and the game memory a job of it takes (`0.7 s, 0.9 MB`,
`1.3 s, 1.4 MB`, `1.7 s, 1.9 MB`, from `core/vocr_models.c`). The chosen size is
filled in the accent colour. The card's last line names the chosen size and its
model (`Normal: default, balanced. VOCR3-H15 found.`), or that file missing.
Up/Down select the size row and the three cards (four rows); on the size row
Left/Right choose the next smaller or larger size and Enter goes round them; a
touch on a size chooses it, the row's title, the gaps and the status line only
focus it. The focus bar at the card's left edge is beside the selected row only.
The status line names the chosen size's model from its pinned metadata (`settings/engine.c`:
`Large: most accurate. VOCR4-W20 found.`, or `VOCR4-W20 missing. Reinstall the package.`), so the
generation shown follows the size: VOCR3 for Small and Normal, VOCR4 for Large. When a game's free memory
has no room for the chosen size a job runs a smaller one; nothing in the app says
so (owner 2026-10-15 04:54Z), only `log.txt`. `host/tests/test_settings_app.c`
drives it: every size's edges and gaps by touch, Down/Left/Right/Enter/Up, the
saved `vocr_tier` (an older `vocr_model` line dropped on the first save), no
frame naming an engine or Google Lens, each size's file missing (the chosen size's
model reported missing, no other model named), and no frame naming a fallback. A saved `ocr_backend` line (`vocr`, or a removed engine: `lens`, `vocr2`,
`meiki`, `ncnn`) loads as the local engine, a removed one with a config warning,
and the next save drops the line. `host/tests/test_settings_app.c`
runs the actual input/draw loop with a scripted controller, touch screen and
vita2d, and real files: the size buttons, their edges and the gaps (focus only),
Left, Right and Enter, the removed values, missing and wrong-sized models, an
unreadable config.ini, and a later model table whose VOCR5 the app names unchanged
(a missing VOCR5 file reported as `VOCR5-N missing. Reinstall the package.`, though a
smaller size's file would run).
Its text widths are stub metrics, not firmware fonts. These scenarios, previews
and the native checks below predate that change.
The native Settings build and the PNG ABI/assets checks are separate checks. This change does not deploy anything to a Vita.

Native promotion of the v0.7.7 VPK completed with result 0. Full native readbacks
of the installed 277,084-byte executable and 788-byte `param.sfo` match the
published release members. The SFO's `APP_VER` remains `01.00`; exact executable
bytes and release provenance identify this build. A fresh boot and foreground
launch were confirmed; a snapshot reported `system_ui_overlaid = true`, so
controller tests waited. The user subsequently opened the app and encountered
`C2-12828-1`. Its completed native dump identified an incompatible libpng jump
buffer during icon loading, rather than a PVF measurement failure. The
[PNG runtime repair](settings-png-runtime.md) keeps the readable layout and icons.

The v0.7.8 repair passed exact-head host/Vita CI and merged through
[PR #36](https://github.com/bee-san/vita-jp-overlay/pull/36). Full readback of
the installed 277,212-byte executable matched the repaired candidate. Promotion
status had become uncertain after a disconnect; installed registry/readback and
subsequent native execution reconciled the outcome. No terminal promoter
response is inferred from that uncertain status.

The same Settings process remained in the foreground over 100.106 seconds,
with the final snapshot reporting `system_ui_overlaid = false`. The sampled
native event window contained no crashes and reported zero lost events.
An additional snapshot extended observed survival to 336.637 seconds with the
same foreground Settings process and no reported system UI overlay. No
screenshot was taken.
After an initial Select/readback discrepancy was caught, two completed
front-touch actions normalized OCR-tab/engine-row focus. Five subsequent
controller actions, each followed by a complete config readback, checked Meiki,
VOCR, the already-off Japanese-only state and restoration to on. The final
config exactly matched the intended VOCR/Japanese-only-on configuration, with
unrelated settings preserved. A later native check measured Japanese-only
on-to-off-to-on: the OCR-tab touch retained on, the Japanese-only-card touch
saved off, and Right saved on. Complete config readbacks followed all three
actions; the final 2,951-byte config was again byte-identical to the intended
file. These establish native control/persistence behavior; they do not provide
screenshots or firmware glyph measurements.

A subsequent agent-runtime replacement and fresh reboot also confirmed Settings
relaunched in the foreground with no reported system UI overlay and the intended
config byte-identical. That new boot is separate from the earlier measured
survival interval. Native keep-awake worker initialization and its first DEFAULT
tick returned 0. A subsequent 374.066-second command-free interval ended with a
healthy native response on the same boot and the same foreground Settings
process. The ordinary endpoint snapshot turns the screen on, so it does not
prove physical display visibility during the interval.

Firmware font bounds, sharpness and physical readability remain pending. The
broader installation and private-dictionary recovery state is tracked in
[the deployment checklist](todo-20261011.md).
