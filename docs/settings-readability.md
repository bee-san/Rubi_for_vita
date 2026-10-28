# Settings readability on the Vita

The standalone Settings app previously used five 56-pixel game rows and small
PVF drawing scales, with dense hints and a narrow page-switch touch area.
This change affects Settings only; the in-game overlay and OCR workers are
unchanged.

The 960 × 544 Games layout shows five 40-pixel cards with 4-pixel gaps, followed by
the search field and Prev/Next controls. Titles have one measured-width line with
ellipsis for long names; IDs have a separate line and the ON/OFF badge stays at
the right. The page tabs have 48-pixel touch heights (the OCR card's
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

Since #333 a game's card toggles as Enter or the finger is let go, and opens the game's own page when held for half
a second ([the guide](guide.md#a-games-own-page)). On the card's second row, beside the ID and its PSP or Hook tag,
`Line finder: On` (x 360, in the accent colour) marks a game with a line finder of its own, and the focused card says
`Hold ×: settings` (x 600, dim; ○ on a console whose Enter is ○). Both are drawn at fixed places, so a frame
measures no more text. The page reuses the OCR page's cards and 34-pixel badges: the game's ON/OFF (68 pixels), and its
line finder (112 pixels), whose six choices sit in a row below its title (138 pixels wide, 8 apart), its help below.

The OCR & lookup page has a fourth card, **Dictionaries shown**, with eight
number badges (44 × 34 pixels, 4 pixels apart). To fit it, the three smaller
cards are 68 pixels tall, their badges 34 pixels, and the page's status is one
line. The OCR engine card and the Games page are unchanged. Touching a number
chooses it; the card's title and help only focus it.
[A Vita3K render](settings-dictionaries-vita3k.png) shows the page with three
enabled dictionaries and 2 selected. It used the official 3.74 firmware fonts
and a build that opens on this page, because injected keys did not reach the
emulated app; it is not a physical-device check.

### Read text and Speed boost (ocr_mode, ocr_boost_clock)

Two cards come after the Model size card, before the lookup cards: **Read text**
(`ocr_mode`) and **Speed boost while reading (444 MHz)** (`ocr_boost_clock`)
([docs/auto-vs-press.md](auto-vs-press.md)). Read text is 102 pixels tall: its
title on the first row, then two 433 × 54 buttons 12 pixels apart (x 42-474 and
487-919), **Automatically (when it changes)** with "The popup is ready when you
press" below it, and **Only when I press L + R** with "Nothing runs until you
press" (22 and 20 pixel text). The press button names the overlay's
`toggle_button` (Select, Start, Select + L or R; "Only when I double-tap the
back", with "tap", for the rear touchpad). The Speed boost card is a 68-pixel
ON/OFF card like Japanese characters only. Left/Right choose Automatically/the
press and OFF/ON, Enter switches either, a touch on a Read text button chooses
it (its title row, the gap and the margins only focus the card) and a touch
anywhere on the Speed boost card switches it. Both save together
(`settings/preferences.c` `vjo_preferences_save_reading`: each key replaced
where its first line is, later duplicates dropped, a missing one appended,
every other line kept but a removed feature's), and nothing is saved while
`config.ini` cannot be read.

The six cards (94 + 102 + 4 × 68 pixels and 4-pixel gaps: 488) are taller than
their room, y 112 to 422 above the status line (310 pixels). The page scrolls a
whole card at a time (`settings/main.c` `pref_show`): the selected card is
always all in view, by the fewest cards scrolled, and the cards above the view
are not drawn. The card after the last whole one shows as far as the room goes,
cut at 422 (its rectangle and text clipped there), so there is visibly more; a
6-pixel bar at x 942 shows the view's place in the column. A touch on a card that
is cut only selects it, which scrolls it into view; once whole, a touch acts as
on any card. The left stick still stops at the first and last card, and Up/Down
still wrap round. `host/tests/test_settings_app.c` drives it: both cards'
labels, lines and colours, every Read text button's edges and the gaps by touch,
Left/Right/Enter on both cards, the saved `ocr_mode` and `ocr_boost_clock` with
every other line kept, each `toggle_button`'s name, an unreadable `config.ini`
(nothing chosen or saved), the scroll position after every Up/Down and the bar,
the cut card's touch, and no frame drawing a card into the status line.
`host/tests/test_settings_games.c`'s stick test follows the focused card by its
title. Text widths are stub metrics, not firmware fonts: the longest Read text labels (31 characters at
22 pixels, 379 of the button's 433 pixels in the stub's metrics) have not been seen on a console yet.

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

- **An icon cache by title ID** (`settings/icons.c`): five slots for the five
  games on the current page. A frame loads at most one icon. Icons outside the
  page are released after `vita2d_wait_rendering_done`; no following or previous
  page is prefetched. A
  card shows **JP** until its icon is loaded. Toggles, Triangle and filters
  keep matching icons; Start frees it (after the GPU wait) with the new list.
- **Search across the indexed titles and IDs.** L+R together or the search field
  opens the native Vita keyboard. Accepted text filters the list and starts on
  the first result page; cancel leaves the current query and page alone. Empty
  text or Clear restores the list. The enabled/disabled filter composes with the
  search. Search does not rescan the card or load off-page icons. The index of
  title/ID metadata remains bounded to 512 entries; only icon textures are paged.
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
- **L/R page.** The view moves five entries to a non-overlapping page and the selection keeps its row
  when present (clamped on a partial last page); at
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
  selected in its five-game page.

The current layout shows the page number and Prev/Next touch buttons, with five
games per page. Its native keyboard and compact rows have not been checked on a console.

### Memory

The Rubi app is a system application
([vita-memory.md §5.4](vita-memory.md#54-the-rubi-app-a-system-application)), and
libvita2d then allocates every texture as its own `USER_RW` block in the app's
50 MiB budget, rounded up to 4 KiB (pinned libvita2d a8f15ab: `vita2d.c` sets the
texture type to `SCE_KERNEL_MEMBLOCK_TYPE_USER_RW` in system-app mode;
[`utils.c`, `get_aligned_size`](https://github.com/xerpi/libvita2d/blob/a8f15ab09d5233f0a4e4ad0e8f6ade0da888cbed/libvita2d/source/utils.c)).
An icon is drawn from 4 bytes per pixel, so a 128 × 128 icon's texture is 64 KiB
and the largest accepted, 256 × 256, is 256 KiB: the cache holds at most
5 × 256 KiB = 1,280 KiB (1.25 MiB), down from nine cached icons before pagination. If
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
1,024 B, 1,024 B of failure and layout flags and 104 B for the five cache slots
[source: the Vita ABI and `VjoIconCache`]. The earlier nine-slot cache change measured
the Settings ELF's `.bss` going from 1,308,464 to 1,596,352 B and its text from
471,276 to 474,740 B (main `8cf4f3a`). The app runs in its own process; no
plugin and no SceShell memory changes.

### Measurements (host)

The following tables describe the earlier nine-icon implementation on main `8cf4f3a`,
before the five-game pagination change; they are historical measurements.

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
- `settings_games`: the real app over 1 to 520 synthetic games, five-entry page
  boundaries, partial final pages, title/ID text and ON/OFF choices, touch and
  controller paging, letter jumps and repeat timing. At most five icon textures
  are resident, one decoded per frame, and no off-page icon is opened. Texture
  frees still wait for the GPU and follow libvita2d's ownership rules. Search
  covers an off-page ID, Japanese text, no results, cancel, clear and combined
  enabled/disabled filtering, without rescanning the card. The SDK keyboard is
  stubbed; the app's matching, UTF conversion and dialog lifecycle are real code.
- `settings_search`: Unicode and surrogate-pair round trips, matching by title
  or ID, an empty search, cancellation and SDK errors, and balanced IME module
  loading. This is host coverage, not a console measurement.
- `tools/vita3k_settings_smoke.sh`: the app's own code, unchanged, in Vita3K with
  a scripted controller over 120 synthetic games, as a crash check only
  ([offline testing](offline-testing.md#5-vita3k-precisely)).

## Games missing from the list, and Default OFF (#138)

In test.3's run (#138) CLANNAD (`PCSG00415`) was not on the Games page, while
AIR was, and the tester found that turning the Default OFF left CLANNAD on.
`games.ini` held `default = enabled` and `PCSG00415 = enabled`. A title's own
rule comes before the default (`core/games.c`, unchanged), so no Default could
turn CLANNAD OFF while that rule stood, and with CLANNAD missing from the list
the rule could not be seen or changed.

That explains the finding, but this boot did not show it: it saved no Default
change. The collect's listing (`listings/ux0_data_VitaJPOverlay.txt`, the
Vita's clock) has `games.ini` at 191 B from Oct 13 07:14 and `games.ini.bak`
at 192 B from Oct 12 20:06. A save renames the previous `games.ini` to `.bak`
(`vita/games_storage.c:76-81`), and the rename keeps its time: `config.ini.bak`
(07:08) and `config.ini` (07:10) are this boot's two Model-size saves the same
way (`settings/preferences.c:80-82`). So this boot saved `games.ini` once, at
07:14, and the file got 1 B shorter: `PCSG00940 = disabled` became `enabled`,
AIR turned ON between its OFF start (`log.txt` line 12121) and its ON one
(12145). The tester's Default OFF was in another boot, or was not saved
[inference].

Why CLANNAD was missing is not known. The test.3 app (main `9cfa920`) logged
nothing about its scans. It started once in this boot (`log.txt` 11345) and was
in front six times (`VJOSET001` at 11344, 11677, 11877, 12002, 12110 and
12132), with CLANNAD suspended behind it, and AIR at the end. A Start press in
any of those visits rescans and leaves no line. The `PCSG00415 = enabled` rule
shows that an earlier app listed and toggled CLANNAD; when is not recorded. If
that app predated #109, it was an ordinary application, which ran with the game
closed [inference].

**CLANNAD is on the memory card, in `ux0:app`** (found by the PR's audit). The
zip has ten crash dumps: eight of SceShell (`shell.self`) and two of the Rubi
app itself (`eboot.bin`, naming `ux0:app/VJOSET001/eboot.bin`). They are from
earlier sessions: this boot is the first since the install (`boot.txt`
`seq=1`, no failed boot), with one kernel start and one Shell registration in
`kernel.txt`. Two of the SceShell dumps hold CLANNAD's path; in the
decompressed dumps:

- `psp2core-1791732858-…` (Oct 11 15:34 by the Vita's clock) has
  `ux0:/app/PCSG00415/eboot.bin` at 0x7400b0 and `ux0:app/PCSG00415/eboot.bin`
  at 0x7415e8;
- `psp2core-1791868846-…` (Oct 13 05:20 by the Vita's clock) has
  `ux0:app/PCSG00415` beside its LiveArea's address, at 0x71e898.

None of the ten names `gro0:` or `grw0:`. The dumps are not of this boot, so
this is strong evidence, not proof, that CLANNAD was in `ux0:app` in this one
too. (The Vita's clock is about 3 days behind, and the dumps' names follow it.)
The causes that fit, none confirmed:

- **`ux0:app`'s listing missed CLANNAD's folder, or stopped.** The app is a
  system application, listing `ux0:app` while CLANNAD runs suspended from it.
  9cfa920's loop ended at the first failed `sceIoDread` without a word, and it
  skipped an entry that did not read as a folder silently. Whether a running
  game's folder lists differently is not known.
- **Listed under its ID.** When the app reads no title from `param.sfo`, it
  lists the game as `PCSG00415`, under P, not CLANNAD under C. Nothing in the
  collect shows whether this happened.
- **The list's filter.** Square shows all, enabled or disabled games, and the
  card beside the Default row names the choice (`settings/main.c:733-744`). Set
  to Disabled, it hides every game that is ON: CLANNAD here, but not AIR,
  which was OFF until 07:14. The filter starts as All games at each start of
  the app, and no log records it.
- **A game card, or safe mode: unlikely.** CLANNAD's folder is on the memory
  card (above). Without HENkaku's *Enable Unsafe Homebrew*, an unsafe app such
  as this one runs sandboxed: VitaShell then offers `ux0:` alone (VitaShell
  81af709 `file.c:958`), and it detects this case with
  `sceIoDevctl("ux0:", 0x3001, NULL, 0, NULL, 0)` returning `0x80010030`
  (`init.c:353-355`). The collect's FTP server served `ur0:`
  (`ur0:tai/config.txt`), which VitaShell's does only outside safe mode
  (`main.c:197-201`), if it was VitaShell's, as the installer's steps say; and
  the setting is HENkaku's, for every unsafe SELF, the app's included
  (`vita/CMakeLists.txt`: `UNSAFE`) [inference: henkaku 784e483
  `plugin/kernel.c:60-73`].

The candidate rule could not have dropped CLANNAD: it accepts `PCSG00415`. A
full list is very unlikely [inference]: `log.txt` names 20 distinct title IDs,
and the dumps name six `ux0:app` folders by path (CLANNAD's, the Rubi app's,
VitaShell's, Adrenaline's and two homebrews'), against 512 rows; a full list
says `Showing the first 512 installed titles.` (`settings/main.c:480`), which
was not reported. The SceShell dumps also hold a catalogue of 1,138 homebrew
title IDs (`"titleid"` entries, a VitaDB-style list): a list of downloads, not
of what is installed. The collect has no listing of `ux0:app`.

What the app does now:

- **Each scan is logged** (`log.txt`, `log_file = on`): one line per root with
  its entries, how many were listed, already listed or skipped, those listed
  under their ID because no title was read, those left out when the list is
  full, a listing that stopped (its error), then each skipped name with the
  reason (`not a folder`, `not a title ID`, `a system app`, `VitaShell`, `this
  app`, `SceShell's name`), then the IDs listed without a title. A root that
  cannot be opened gets its error and whether the device is there at all:

  ```text
  Rubi app: games: ux0:app: 23 entries: 21 listed, 0 already listed, 2 skipped; skipped: VITASHELL (VitaShell), VJOSET001 (this app)
  Rubi app: games: gro0:app: not opened: 0x80010013 (ENODEV); gro0: is there
  Rubi app: games: 23 rows (the list holds 512): 21 from the folders, the shared PSP row, 1 only in games.ini; safe mode no (ux0: devctl 0x80010016); only in games.ini: PCSG00415 = enabled
  ```

  These lines are the format from the host tests; no console has logged them
  yet. A line holds at most 300 bytes: every count comes first, and names past
  that are counted (`and N more`), the IDs before the skipped names go, never a
  name or a label cut (a folder's name is cut at 24 bytes, at a whole
  character, with `...`). A scan's lines go out in one append after it, as the
  Shell's do for a job (#100), since an append cost the console 12-46 ms in
  #81's timestamps (`settings/applog.c`). Near `log.txt`'s 1 MiB cap each line
  that still fits is written, as before. The cost: an app that crashes during a
  scan loses that scan's lines, which would have named the root it was reading
  (its earlier lines, the start line included, are already written).
- **A title with a choice of its own is listed even when no folder has it**
  (`VJO_ROW_MISSING`): a folder the scan did not list or could not read (as
  CLANNAD's), a game card it cannot read, a deleted game. Its row shows its
  update's title and icon when `ux0:patch/ID` has an update, which VitaShell
  reads as a plain file (`refresh.c:92-95`), or else its ID, followed by
  `(folder not found)`. It can be turned ON or OFF like any row, and the Shell
  reads the same rule; a PSP game's ID (`PSPEMUCFW`, a disc ID) has the PSP tag
  and follows the shared PSP row as in the Shell. A rule the Shell never
  applies (`NPXS…`, this app) gets no row.
- **Default ON -> OFF turns every game OFF** (Triangle, or the Default row touched;
  `vjo_games_disable_all` in `core/games_app.c`). Every `= enabled` rule is
  removed, including those of games not on the list and the shared PSP row's,
  so the Shell sees every game OFF. The `= disabled` rules stay. The status
  names the games it turned OFF too (`Saved: every game is OFF from its next start
  or resume. Turned OFF too: …`). Default OFF -> ON turns on every game except
  those turned OFF one by one, and names them (`Saved: every game but 2 is ON from
  its next start or resume. Still OFF: …`). A game turned ON after that is ON on its own, as before. Each
  change is logged with the rules it removed or kept (`Rubi app: games: Default
  OFF, 4 own ON choices removed: PCSG00415, …`).

`core/games.c`, which the plugins link, is unchanged: the two new helpers are in
`core/games_app.c`, linked into the app and the host tests only, so the
kernel's link (no `--gc-sections`) does not take them in. `games.ini`'s format
and the order of its rules are unchanged.

Tests (`settings_games`, `games`): a game only on the card, listed with its
title and icon; a title on both cards listed once; a refused `gro0:app` with
the titles only in `games.ini` listed, toggled, saved and kept after a restart;
the skip reasons, the line's limit (the skipped names kept before the IDs) and
a name cut at a whole character; a listing that stops, after which no bubble
description is removed; a full list and safe mode in the log; one append per
scan; the batch keeping another thread's line apart; the 1 MiB cap, line by
line as before; a PSP game's ID in no folder following the shared PSP row, with
its tag; disable-all over
explicit entries, through the Shell's own rule (`vjo_games_enabled_as` on the
saved file, PSP games and the timeout fallback included), with OFF -> ON
keeping the games turned OFF; a failed save changing nothing; and the reasons
matching `vjo_game_is_candidate` on every ID shape it tests. None of this has
run on a console. The cause of #138's missing row is for the next console run's
`log.txt` to show.

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
time for a 3-line textbox and the game memory a job of it takes (`0.5 s, 1.0 MB`,
`0.8 s, 1.4 MB`, `1.9 s, 2.8 MB`, from `core/vocr_models.c`). The chosen size is
filled in the accent colour. The card's last line names the chosen size and its
model (`Normal: default, balanced. VOCR6-N found.`), or that file missing.
Up/Down select the size row and the three cards (four rows); on the size row
Left/Right choose the next smaller or larger size and Enter goes round them; a
touch on a size chooses it, the row's title, the gaps and the status line only
focus it. The focus bar at the card's left edge is beside the selected row only.
The status line names the chosen size's model from its pinned metadata (`settings/engine.c`:
`Large: most accurate. VOCR6-L found.`, or `VOCR6-L missing. Reinstall the package.`), so the
generation shown follows the size: VOCR6 for all three of this package's sizes (`Small: fastest. VOCR6-S found.`; a
size that keeps an earlier generation's model shows that one's, as Small's VOCR5-S did in the test builds v0.9.8-test.4
and test.5). When a game's free memory
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
unreadable config.ini, and a later model table whose generations the app names unchanged
(a missing file reported as `VOCR6-N missing. Reinstall the package.`, though a
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
[PR #36](https://github.com/bee-san/Rubi_for_vita/pull/36). Full readback of
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
