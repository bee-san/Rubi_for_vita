# Changelog

Rubi's releases and test builds, newest first, in the style of
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). A test build is made for a console run on
the owner's PS Vita, a PCH-1001 on firmware 3.65, with CLANNAD (`PCSG00415`), and published as a
pre-release (test.4.1–4.3, respins with fixes, were installed but not published); Rubi's OCR ran in
CLANNAD in every one. What a run showed is under **On the console** (in the 0.7 entries, after "On
the console:"), from the owner's reports and logs; other figures say where they come from. The test
builds are the debug build: no console run of the normal build, the releases' main download, has
been reported.

## [Unreleased]

### Added

- The popup says where its text came from: the dialogue starts with a dim `[HOOK]` when a native text
  hook supplied the line and `[OCR]` when Rubi read it off the screen (the owner asked for a hook emoji
  and an eye, then for ASCII over an image). An earlier line (line history) has none (`history.csv`
  keeps its source; the popup does not read it back). `source_badge` in `config.ini` (no setting in the
  app): `on` (the default), `off` hides it, `icons` shows an anchor and an eye from the system's own
  fonts instead. It sits on the text's first row, as wide as 2.9 Japanese characters (`[OCR]` 2.3) at
  the default sizes; Rubi never leaves it alone on a row, and the cursor, taps and the highlight skip
  it. Emoji are not used: none of the Vita's 20 system fonts has a character beyond U+FFFF, so none has
  🪝 or 👁 (nor U+2693 ⚓ or ✓, which the console draws as `_`); their ARIB symbols font
  (`music_arib.pvf`) has an anchor (U+E1BB) and an eye (U+E541), which `icons` uses and which appear
  only if SceShell's PAF loaded that font (else `_` or nothing: unverified); the firmware's `imagefont.bin`
  and `psexchar.pvf` have neither [measured: the fonts of Sony's font package and the 3.74 update, on
  the host; [details](docs/native-hooks.md#the-popups-source-badge)]. Host-tested only.

### Changed

- Each game that is ON carries 4,096 B more of the OCR worker's module pages, for the counter re-decode:
  172,032 B in the normal build, 180,224 B in the debug build (`docs/memory-ledger.md`).
- The Shell keeps its pages (163,840 B normal, 176,128 B debug) with the badge: its libc shims, which
  only branch to SceLibKernel's ARM stubs, are built as ARM code in the Shell, so the linker drops nine
  interworking veneers (144 B), and the enter button's registry read is built for size. The normal
  Shell's code segment is 16 B larger (4 B of room left), the debug one's unchanged; the kernel plugin
  and the game worker are byte-identical [measured: both builds' module rows;
  [memory ledger](docs/memory-ledger.md#the-popups-source-badge)].

### Fixed

- A counter read as `ッ` is `ヶ` again (`ルッ月です」` → `ルヶ月です」`): a text rule in the shown and
  looked-up text's normalisation (`core/textnorm.c`). Before the LM, the job puts back a numeral or
  `「` that the frames hold close below what the recognizer read before a counter, so `ル` becomes `九`
  (vita-vn-ocr #109's counter re-decode, `runtime/vocr_post.c`; pin bumped). On vita-vn-ocr's
  development data neither changes a line; see [docs/vita-vn-ocr.md](docs/vita-vn-ocr.md#counters-a-numeral-or-a--the-line-lost-vita-vn-ocr-109).

## [0.9.1] - 2026-10-21

Native text hooks that Rubi remembers for each game build and checks less often as they prove
reliable, and the conjugation of a looked-up word in its dictionary entry. Like 0.9.0, the release
is the normal build only.

### Added

- A dictionary entry names the conjugation of the word looked up: under the headword, in blue,
  `Conjugation: polite past` for 食べました (found as 食べる), `Conjugation: negative → past` for
  食べなかった; a word in its dictionary form gets no such line. The reasons are those of the
  Yomichan rules that found the entry, and stay with each word through dictionary paging and
  prepared lookups; a chain of more than four shows `…` before the three nearest the written word.
  Definitions and Anki cards are unchanged ([#243]; [details](docs/conjugation-display.md)). Its
  previews are host renders: the Vita's font and wrapping of the line are unchecked.
- Remembered hooks (API 34; [#242], [#241]): a hook that has proved reliable is kept for each game
  build in `ux0:data/VitaJPOverlay/hooks.cache`, at most 32 games in 3,088 B (by its format): the
  source's kind, its offset in the game's module, its encoding and its check interval, never dialogue.
  An entry is keyed by the title ID, the SHA-256 of the game's whole executable, the text source and a
  revision of Rubi's hook definitions, so a game update, another text source or changed hooks learn
  afresh, as does a missing, damaged or unwritable file. At the game's next start, one agreement with
  OCR resumes the saved interval. The game's worker hashes the executable and reads the entry; the
  kernel's existing file thread writes the file. Nothing new runs in SceShell.

### Changed

- How **Automatic** trusts a native text hook ([#242]). A hook's line still replaces OCR's only when
  the two agree (70 % or more, four characters at least), but trust now takes agreement on two
  different dialogue lines (one line read twice does not count). Then the hook answers seven new
  readings without OCR and OCR checks the eighth; each check it passes widens the interval to 16, then
  32 readings (0.9.0's OCR read at least every third page). Reopening the overlay on unchanged dialogue
  is not a new reading. One disagreement shows OCR's text and checks the next new line, where an
  agreement resumes at 8; two disagreements in a row drop the hook until it earns trust again. An empty
  or too-short OCR reading counts neither way, and a screen without the hook's text (a menu, a blank
  screen) keeps what it learned. The cost: a hook that turns wrong can show up to 31 readings before a
  check catches it.
- Each game that is ON carries a bigger OCR worker: 167,936 B of module pages in the normal build and
  176,128 B in the debug build, 8,192 B more than 0.9.0's. The kernel plugin's pages grow 4,096 B, to
  118,784 B (122,880 B debug); the Shell's stay at 163,840 B (176,128 B debug), the conjugation line
  included [measured: the build logs' module rows, Release run 39581967210 at `d428c55`].
- The README says which games have native text hooks and how **Automatic** picks their text ([#239]):
  CLANNAD, 175 title IDs from LunaTranslator's table (294 ported sites, none verified on a console),
  the string-copy hooks of **Every game** elsewhere, and no PSP hooks. The guide, the design and the
  hook list say the same about what the console has shown.

### On the console

- None of 0.9.1's changes has run on a console: the remembered hooks and their check intervals (how
  often OCR runs while reading, reopening and restarting, the boot and the CPU cost are unmeasured),
  the conjugation line, and the normal build itself. The native hooks' console record is still
  0.9.0's below, CLANNAD's string-copy reads in the combined debug test build; the test.7 checklist
  ([#237]) has no reported results, and no site from LunaTranslator's table has been reported working
  on a console.

## [0.9.0] - 2026-10-20

Native text extraction, dialogue history, reading statistics and Anki screenshot trimming.
The normal build omits the debug traces and on-device self-test; the overlay keeps all user features.

### Added

- An optional small Adrenaline Shell bridge, `compat/adrenaline_vsh.suprx`, in the release zip
  ([how to use it](compat/adrenaline_vsh/README.md)), for an Adrenaline or PSP bubble start that goes
  straight back to LiveArea: on the owner's console, Adrenaline's own bridge did not fit in
  SceShell's memory (`9e36b5e`).
- An opt-in text-presence gate (`ocr_text_gate`, off by default; API 26): a region it scores as
  text-free returns no line. On the host it did so for 59 of 66 text-free regions, against 5 without
  it ([#214]).
- The Rubi app's Games page shows five games a page and searches by title or title ID ([#219]).
- Native text hooks (API 31; [the guide](docs/guide.md#native-text-hooks),
  [the design](docs/native-hooks.md), [#230]): in a game that is ON, the game's worker can read the line
  the game is about to draw instead of OCR's reading. CLANNAD's string-copy source was verified on
  the owner's console in the combined test build; table sites remain unverified on hardware.
  - What is hooked: LunaTranslator's Vita table, 294 ported sites for 175 title IDs, each installed only
    where the game's code at that address is a plain function entry that taiHEN can patch, and, where the
    table names a game version, only in that version ([the games](docs/hook-titles.md)); Rubi's own entry
    for CLANNAD (`PCSG00415`, one site, proven in Vita3K only); and the CALL source, the game's string copies (SceLibc's
    and SceLibKernel's memcpy, strcpy and their kin) grouped by the caller. **Automatic** hooks only the
    listed games; **Every game** adds the CALL source in every game that is ON. `hook_sites = off` in
    `config.ini` installs no table site, only the CALL source, so a console run can tell the two apart.
  - Which text is shown: OCR still reads every page at first; a hook's text replaces OCR's only when
    the two agree (70 % or more, 4 characters at least). After two agreements in a row the hook is
    locked: it then answers new lines of 4 characters or more without recognition, at most 2 pages in a
    row, and OCR reads the next page, where the hook's text must agree. A disagreement makes OCR read
    every page until the hook agrees again; two in a row unlock it, and so does a change of the caller
    behind a CALL stream. So a hook that runs ahead of the screen, or turns to a name or a menu's text,
    shows at most 2 wrong pages.
  - The controls: `text_source = auto | hooks | ocr` (the Rubi app's **Text source**, Automatic by
    default, applied when a game next starts), which the Shell hands the kernel and a game's worker
    reads as the game starts. The popup selects its text automatically and keeps source diagnostics
    in logs; the Games page tags a game with a hook entry (`Hook`: CLANNAD, Rubi's own; `Luna hook`: a title with
    a site Rubi has ported from LunaTranslator's table; the table's 7 titles without one, PCSB00985,
    PCSG00291, PCSG00451, PCSG00595, PCSG00663, PCSG01081 and PCSG01152, get no tag and OCR only in
    Automatic). `log.txt` says what each game's worker hooked (counts only in the normal build),
    `history.csv`'s engine is `hook` for a hook's line.
  - A game turned OFF gets no hooks, even when a `*TITLEID` line kept by hand in taiHEN's config still
    loads the worker into it: the kernel gives the text source only to a game whose worker it loaded as
    ON. Holding L as a game starts, `text_source = ocr` and a safe boot also install none.
  - The cost: every game that is ON carries the bigger worker whatever the text source, even with
    `text_source = ocr`: its module pages grow from 98,304 to 159,744 B (167,936 B in the debug build)
    [measured: the build logs' module rows, `feat/native-hooks` 9fa24c5]. A hook's own CPU time is
    unmeasured.
- Anki screenshots without black bars: with **Trim black bars** (`anki_screenshot_trim_bars`, on by
  default; API 32), a card's picture leaves out the letterbox or pillarbox around the game's picture, as
  Adrenaline shows a PSP game smaller than the screen (at 1×: 484 × 276 of 960 × 544). The game worker
  finds the bars as it reads the rows and encodes the picture's part of the same rows, with no copy of the
  frame; a frame without bars gives the same JPEG as before, byte for byte. On the host, with local copies
  of the owner's frames: none of 1,727 Vita frames trimmed, 282 of 300 PSP frames at 1× and 1.5×, no pixel
  that is not black left out ([details](docs/anki.md#black-bars)). Not run on a console yet. The Rubi
  app's Anki page has the switch; its five rows scroll.
- `history.csv` has a `characters` column: each line's kanji, kana and ASCII letters and digits,
  without punctuation, brackets, symbols, spaces or line breaks (reading trackers such as
  GameSentenceMiner and ッツ Ebook Reader leave punctuation out too). `tools/reading_stats.py` gives
  the characters read and per hour, in all, per game and per day, from new and 0.8.0 files
  ([#233]; [docs/ocr-history.md](docs/ocr-history.md)).
- Earlier lines: in the overlay, Up on the text's top row shows this game's line before it, focused
  between its dimmed neighbours, for lookups and cards (a card from an earlier line has that line as its
  sentence and no screenshot); Down comes forward again. Only with **Read text: Automatically**, from
  the lines `history.csv` keeps: its last 24 KiB, at most 32 lines, in a 16 KiB Paf block while you
  look back ([line history](docs/line-history.md)).

### Changed

- An install or an update no longer writes `text_source = ocr` into `config.ini` (API 31: the setting
  is the user's again); a file that has it keeps it, so choose Automatic once to use native text hooks.
- **Japanese characters only** (`japanese_only_selection`) is OFF by default, as the owner asked:
  the cursor can select any character, Latin letters, digits and punctuation too, and starts at the
  text's first character. A `config.ini` keeps its own line: from 0.7.6 the template wrote
  `japanese_only_selection = on`, so an install updated from an earlier build stays ON until it is
  turned OFF on the Rubi app's OCR & lookup page ([#231]).
- Every verified install deletes the app's own copy of the dictionary when the app's folder can be
  written, and says so on the done screen and in `log.txt` ([#200]).
- A card's screenshot is written over a file set aside for it beforehand: on the owner's console,
  after a restart, the first write to a new file took about 21 s, so a card took that long or was
  saved without its picture ([#208]; [#207], [#210]).
- A queued card whose screenshot is missing or damaged is exported without it, and the export says
  so; before, it stayed in the queue ([#218]).
- The plugins use API 33, combining native hook selection and screenshot trimming in one matched
  protocol. API 31's hook fields and API 32's standalone trimming build cannot be mixed.
- The combined build retains the native-hook worker's 167,936 B debug / 159,744 B normal module
  pages. Compact highlight ranges and fixed tables in existing data-page space keep the Shell at
  176,128 B debug / 163,840 B normal, with the existing PAF reserve unchanged
  ([binary measurements](docs/memory-ledger.md#combined-enhancement-test-build-local-api-33)).
- The popup shows dialogue without hook numbers or source-status controls. Source diagnostics stay
  in the logs, and ordinary text reads select their source automatically.
- The README shows the promo film ([#206]) and says what the console runs showed ([#204]); its build
  history is now this changelog ([#224]).
- The left stick goes a character at a time over the recognized text, in Whole word mode too, and
  looks up the word that starts where it stops (on 昨日すき焼きを食べました, from 食べました to べ,
  then ました); the d-pad still goes word by word. The stick's left no longer retraces the d-pad's
  Rights, so it no longer undoes a d-pad Right that went round a one-row text ([#232]).

### On the console

- 2026-10-20, combined API 33 debug build on the owner's PCH-1001: plugin readbacks matched the
  package and the Vita completed a normal boot. The owner confirmed CLANNAD's popup text, Up/Down
  history, mining an older line, and returning to current dialogue after closing history. The latest
  captured session contained five successful native reads with OCR recognition skipped. All 19 new
  history records' stored character counts matched a desktop recount. This is not a hardware test
  of every Luna table site or of black-bar trimming, and the normal build remains hardware-unmeasured.
- 2026-10-17, with the small bridge: Boku no Natsuyasumi 4 started from its LiveArea bubble, and the
  kernel loaded Rubi's OCR worker into the bubble's PSP emulator process (`PSPEMU004`). Reading text
  in a PSP game is still untested.
- A build made to measure the screenshot fix ([#207]): the first screenshot after a restart took
  719 ms to capture and encode and 74 ms of file work, and saving a card with its screenshot took
  523–905 ms. The finished fix has not run on a console yet.

## [0.8.0] - 2026-10-17

The first release as Rubi, published by the owner from `9ea6542` (API 25; vita-vn-ocr runtime
`d214a05`). Since test.4 it adds the fixes of the owner's test.4.1–4.3 runs, the rename and
prepared lookups; everything else new since 0.7.12 came in the test builds below.
[Release notes](https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.8.0).

### Added

- Prepared lookups: with Read text on Automatically and Whole word highlighting, idle time prepares
  the lookups of up to eight upcoming words, and dictionary handles are kept for the current dialogue
  ([#203]; measured as host operation counts, not timed on a console).

### Changed

- Renamed to **Rubi** (Rubi for Vita; [#189], [what changed](docs/rename-rubi.md)). The app was
  **Overlay** in the published 0.8.0 test builds and **JP Overlay Settings** before (0.7.12 and
  older). Its title ID (`VJOSET001`) stays, so the new VPK installs over either and keeps your
  settings, games and Anki cards. The data folders, the plugin files and the Anki deck keep their old
  names.
- The main download is the normal build, `Rubi-0.8.0.vpk`; `Rubi-0.8.0-debug.vpk` is the test
  builds' build, with the full log and the self-test. Each is also a zip, for the computer installer.
- The README became a short front page, and the old README is now the guide, `docs/guide.md`
  ([#176]). The 42-second promo film is in `docs/promo` ([#193]).

### Fixed

- A dialogue box's short last line is read: the textbox recovery finder for every model (`6674125`)
  and a short-line probe after the finder (`49310ed`).
- A card's screenshot is saved: the kernel's save syncs nothing (`b622c18`).
- When the app cannot read its own folder yet, right after VitaShell installs it, the Install page
  says `Restart the Vita first`, and it logs every reason it shows (`b6755cf`).
- The kernel changes the active game only under its lock ([#190]).

### On the console

- Its release notes say its lookup changes ([#203]) were not installed or timed on a console. Its
  fixes ran in test.4.1–4.3 (below), except the Install page's `Restart the Vita first`, whose case no
  console run has hit yet ([#194](https://github.com/bee-san/Rubi_for_vita/issues/194)).

## 0.8.0-test.5 - cancelled

Planned after test.4 to check its fixes and run what test.4 had not: Small, Read text on a press,
the boost off, the Games page's Default OFF, a PSP game, and the dictionary's grammar and kanji lines
as steps of their own. It was cancelled when 0.8.0 shipped, and no test.5 was published
([#191](https://github.com/bee-san/Rubi_for_vita/issues/191),
[#194](https://github.com/bee-san/Rubi_for_vita/issues/194)).

## 0.8.0-test.4.3 - 2026-10-17

Not published: `main` at `b622c18`, installed on the owner's Vita
([#194](https://github.com/bee-san/Rubi_for_vita/issues/194)). Its app is already named **Rubi**
([#189]).

### Fixed

- The screenshot's save closes and renames its file and syncs nothing (`b622c18`): test.4.2's sync of
  the file (`7f40e7f`) had turned the console off.

### On the console

- Installed by the Install page's **Update**, the third Update on a console: test.4.1, 4.2 and 4.3,
  each over the build before it.
- A card with its screenshot saved in 851 ms (test.4: 23,748 ms). Its `.apkg` note's Picture is the
  game's 960 × 544 frame at the press, and the export removed the screenshot file that test.4.2's
  crash had left.

## 0.8.0-test.4.2 - 2026-10-17

Not published: test.4.1 with a short-line probe after the line finder (`49310ed` on `main`), for
which the debug build's OCR worker grows one page, to 98,304 B (`1f113ad`)
([#194](https://github.com/bee-san/Rubi_for_vita/issues/194)).

### On the console

- Installed by the Install page's **Update**. The short last line `た。` that test.4.1 missed is read.
- Saving a card with Screenshots on powered the Vita off. Its kernel log ends right after the
  screenshot's write, before the save's first call, `7f40e7f`'s sync of the file (fixed in
  test.4.3).

## 0.8.0-test.4.1 - 2026-10-17

Not published: test.4 with three fixes from its run
([#194](https://github.com/bee-san/Rubi_for_vita/issues/194); on `main` as the commits named): the
textbox recovery finder for every model (`6674125`); `Restart the Vita first` on the Install page,
and its reasons in `log.txt` (`b6755cf`); and the screenshot's save syncing only its own file, its
stages traced in `kernel.txt`, and a give-up after 8 s (`7f40e7f`).

### On the console

- Installed by VitaShell (its own installer, started over its FTP connection) and then the Install
  page's **Update** (`Update the overlay from v0.8.0-test.4 to v0.8.0-test.4.1`): the Update's first
  console run. It wrote the three plugins, and Check and repair then reported it installed.
- One short last line (`ていた。`) is read; another (`た。`) is still missed.
- PS-button screenshots were saved; no Anki card was saved in this run.

## [0.8.0-test.4] - 2026-10-17

The last published test build before the release, and the first made to be installed as one VPK
(`VitaJPOverlay-0.8.0-test.4-debug.vpk`): `main` at `3ce0f47`, API 24. It still has the old names:
the app is **Overlay**. Install steps:
[#191](https://github.com/bee-san/Rubi_for_vita/issues/191); the owner's report:
[#194](https://github.com/bee-san/Rubi_for_vita/issues/194).
[Release notes](https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.8.0-test.4), with the full
list.

### Added

- The one-VPK install: the VPK carries the whole install, and the app's Install page installs,
  updates, checks and repairs, and uninstalls ([#158] and the PRs after it).
- VOCR5 as the three model sizes, Small, Normal (the default) and Large, with the 1 MB character
  language model ([#167]).
- Bee's Ultimate Dictionary ([#153]), with Jiten's frequency ranks (53.52% of its entries are ranked;
  the rest show `unranked`) ([#127]), and fewer file reads per lookup ([#140]; host).
- Anki cards keep a screenshot of the game, made in the game's own process ([#150], [#164], [#174]).
- **Read text** (Automatically, or Only when I press L + R) and **Speed boost while reading
  (444 MHz)** in the app ([#146]).
- Two builds: the normal build (short log, no self-test) and the debug build ([#141], [#147], [#155]).
- Automatic reading keeps each new dialogue text in `history.csv` ([#187]).
- Adrenaline Bubble Manager bubbles are listed as PSP games, under their game's title ([#124], [#170]).

### Changed

- The kernel plugin loads the OCR worker into every game that is ON, Adrenaline included: no
  taiHEN line per game, no `--title` ([#148]).
- The app's Games list lists every title `games.ini` names, and Default OFF turns every game OFF
  ([#149]).
- The normal build makes no log append on the way to a result ([#154]).
- The left stick moves the cursor fast and looks a word up once it rests ([#156]); Left and Right cross
  lines and stop at the text's ends ([#173]).
- A word split over two lines is looked up whole, and the overlay breaks its own lines between
  phrases ([#178]).

### Removed

- Google Lens and the whole network stack: Rubi is offline only ([#130], [#132], [#145], [#157]).

### Fixed

- Two possible causes of test.3's app not opening with no game running are handled, and the app logs
  how its start went ([#181]).
- No `Recognizing…` left on screen after a cancelled lookup ([#95], [#179]); config warnings never quote
  a value ([#185]); the subtitle strip after a new region or a game switch ([#182], [#186]).

### On the console

The owner's PCH-1001 with CLANNAD, 2026-10-17:

- VitaShell's first install stopped with error `0x80101114` while test.3's app was still open,
  suspended behind it; after a restart the VPK installed.
- The app, opened straight after, refused to install (`Cannot install the overlay (state 4)`), and
  an Uninstall did not change that; the lead, unconfirmed, is that it could not read its own folder
  until a restart. The install was finished by hand over FTP (since test.4.1 the page says `Restart
  the Vita first` in that case).
- After the restart the app opened from LiveArea with no game running, though neither case [#181]
  handles came up, so why test.3's app did not open is still unexplained
  ([#191](https://github.com/bee-san/Rubi_for_vita/issues/191)); Check and repair wrote nothing.
- The kernel plugin loaded the OCR worker into CLANNAD with no taiHEN line for it.
- VOCR5 Normal took 2.61 ms per CTC frame and Large 4.98–5.10 ms, both at 444 MHz (over the
  analyser's 10 and 8 jobs; Normal ran 15 jobs, Large 11). Large never fell back to a smaller size:
  CLANNAD had 87.0–91.2 MB free. Every job that recognized a line raised the clock
  333 → 444 → 333 MHz.
- The dictionary's popups, the left stick and the export of 20 cards worked.
- Both cards' screenshots timed out after 15 s, while the game froze and its audio cut in and out;
  the saves took 17.6 and 23.7 s (fixed in test.4.3).
- A box's short last line was dropped by the line finder, as in test.3 (fixed in test.4.1 and 4.2).
- Two PS-button screenshots were not saved, and the game seemed frozen while they were taken; the
  logs show no capture of Rubi's at that time ([#194]).
- Not run: Small, Read text on a press, the boost off, Default OFF, a PSP game, and the dictionary
  steps.

## [0.8.0-test.3] - 2026-10-16

One zip, installed with `install-vita.sh --title` and the app's VPK: `main` at `9cfa920`, API 22.
Install steps and results: [#138](https://github.com/bee-san/Rubi_for_vita/issues/138).
[Release notes](https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.8.0-test.3).

### Added

- Model sizes in the app: Small (VOCR3-FL10), Normal (VOCR3-H15, the default) and Large (VOCR4-W20),
  with a smaller one taken by itself when a game is short of memory ([#108], [#105]).
- Faster OCR: up to 3 lines at once, a cache of 8 lines, and 444 MHz while a job reads ([#93]).
- A fast first scan: the self-test runs in a background job of its own, and each model file is read
  once per game process ([#116]).
- Anki: the app exports the queued cards as one `.apkg` of Lapis notes ([#125], [#133]).
- The app and the plugins check that they are one build ([#122]).

### Changed

- The app is a system application: it opens beside a suspended game ([#109], [#112], [#129]).
- Enter and Cancel follow the console's own setting ([#106]).
- A key filter in the dictionary saves about 6 file reads per lookup ([#123]; host).

### Removed

- AnkiConnect ([#125]); the online dictionaries jpdb.io and jiten.moe, and with them the subtitle
  strip's furigana, which only they gave ([#117]); and the Hachidori relay ([#126]).

### On the console

The owner's PCH-1001 with CLANNAD, 2026-10-16:

- The self-test passed every stage. The first scan showed its text in under 1 s; later scans took
  about 2 s, then under 1 s. Recognition took 45–406 ms a job and the line finder 104–494 ms; 22 jobs
  raised the clock 333 → 444 → 333 MHz.
- While Rubi read, the game's frames came late: in the 28 jobs that logged the game's frame pacing,
  291 of 1,609 frames came more than 25 ms after the one before, against 52 of 26,941 in the time
  before each job ([details](docs/auto-vs-press.md)).
- All three sizes ran: Small felt about as fast as Normal, sometimes instant (most likely lines from
  the line cache), and Large was clearly slower; Large never fell back to a smaller size.
- A cursor step's lookup took 35–112 ms (median 56 ms, 31 lookups).
- 7 cards were saved, none failed, and exported as one `.apkg` of 7 Lapis notes (checked on a
  computer, not imported into Anki).
- PS with the overlay open worked ([#104]). The app opened beside CLANNAD and closed cleanly, but not
  from LiveArea with no game running (the owner's report; why is still unexplained).
- AIR (`PCSG00940`) got no OCR: this build loaded the worker only into a game with its own taiHEN
  line, and AIR had none; its jobs ended `rc=-117`. The tester reported the same in Adrenaline ([#148]
  changed this in test.4).
- CLANNAD was missing from the app's Games list, and Default OFF did not turn it off ([#149] changed
  this in test.4).
- Log appends took up to 4.4 s of an 8.6 s job (since test.4 the normal build makes none on the way
  to a result, [#154]).

## [0.8.0-test.2] - 2026-10-15

`main` at `4ac02ac`, API 20. Install steps and results:
[#101](https://github.com/bee-san/Rubi_for_vita/issues/101).
[Release notes](https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.8.0-test.2).

### Added

- test.1's six requests: Up and Down by displayed rows (so local dictionaries now page with an L or R
  tap instead) and Left and Right wrapping at a row's ends ([#87]); the OCR box drawn with the stick
  ([#86]); no dialogue-changed warning ([#92]); VOCR3 and Google Lens as the two engines ([#85]);
  Anki set up in the app ([#90]).
- Typing text is read once it is whole ([#84]), and each mark is shown and looked up in one form
  ([#88]).
- Beam search with a 1 MB character language model, and a cascade that reads a doubtful line again
  with a larger model ([#89]).
- The line finder's boxes are refined in PSP games ([#94]).
- A smoother Games list in the app: icons cached, L and R jump a page, Left and Right jump to the
  previous or next letter, and Start rescans ([#96], [#99]).

### Removed

- VOCR2 ([#85]).

### On the console

The owner's PCH-1001 with CLANNAD, 2026-10-15:

- The self-test passed every stage, and all 16 installed files matched the package.
- Up and Down by rows, Left and Right wrapping and the stick-drawn box worked; no dialogue-changed
  warning was seen.
- The run used test.1's app, so the engine choice and the Anki page were not tested fairly, and the
  old app wrote `ocr_backend = vocr2` into `config.ini` (since test.3 the app and the plugins check
  that they are one build, [#122]).
- The first scans took 9–15 s, because the self-test ran inside them ([#116], in test.3, whose first
  scan took under 1 s); later scans were faster.

## [0.8.0-test.1] - 2026-10-14

`main` at `b548dc9`, API 19. Install steps and results:
[#81](https://github.com/bee-san/Rubi_for_vita/issues/81).
[Release notes](https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.8.0-test.1).

### Added

- VOCR3 as the default model (VOCR3-H15, falling back to VOCR3-FL10) and logging on by default
  ([#77]); Anki cards saved offline to the memory card, then sent to AnkiConnect on the local network
  ([#49], [#77]).
- The frozen Daijirin4 → Sankoku8 → JMdict dictionary ([#73]), several local dictionaries paged with
  Up and Down ([#51]), and ranked entries with one `log.txt` line per lookup ([#79]).
- The boot-safety guard: a boot that does not complete makes the next one a safe boot ([#63]).
- The on-device self-test ([#66]), and `--collect` with the memory headroom of each stage ([#78]).
- The memory ledger: every allocation named and budgeted, and checked in CI ([#64]).

### Changed

- The memory fixes since 0.7.12: SceShell's pages given back ([#68]), the OCR worker loaded per game
  ([#72]), the screen read in the game in grey strips ([#75]), and the memory routes logged ([#76]).
- The settings app is named **Overlay**, with a new icon ([#71]).
- Upstream's 0.7 merged in: Anki note fields and the furigana strip ([#48]).

### Removed

- The Meiki engine ([#50]).

### On the console

The owner's PCH-1001 with CLANNAD, 2026-10-14:

- API 19 booted and the self-test passed (its first stages in the saved region found no line; its
  full-screen stage found five lines).
- 21 OCR jobs: 16 succeeded, 4 found the screen changed and were retried quietly, 1 was cancelled;
  11 read text. VOCR3-H15 took 4.94–5.22 ms per CTC frame; later estimates are calibrated to it.
- 4 Anki cards were saved (text only), none failed.
- Later L + R stopped opening the overlay: a remote screenshot tool's failed capture held 2.2 MB of
  SceShell's memory. A restart without the tool fixed it, with no change to Rubi.
- The owner's six requests went into test.2.

## [0.7.12] - 2026-10-12

VOCR2, an experimental second local model with a textbox finder, can be chosen in **JP Overlay
Settings** (it failed vita-vn-ocr's whole-textbox promotion check); VOCR stays the default. The zip
also has the optional Meiki engine. The pre-release 0.7.12-vocr2.1 was VOCR2's trial build.

## [0.7.11] - 2026-10-11

JP Overlay Settings chooses the engine directly: VOCR, Meiki or Google Lens. The plugins are 0.7.10's.

## [0.7.10] - 2026-10-11

Whole word navigation: Right skips the matched word, Left retraces up to 16 selections, and
automatic OCR looks the first word up before the popup opens.

## [0.7.9] - 2026-10-11

Whole-word or character highlighting, and a tap on the recognized sentence looks that word up. On
the console: the settings app's taps and buttons saved both highlight modes; taps in a game were not
checked.

## [0.7.8] - 2026-10-11

Fixed the settings app's crash while loading game icons (C2-12828-1). On the console: the repaired
app opened and its controls worked.

## [0.7.7] - 2026-10-11

A more readable settings app: larger text, three game cards a screen, ON/OFF badges and page tabs.

## [0.7.6] - 2026-10-11

VOCR (the default) and Meiki installed together and switched in the settings app; Japanese-only
character selection, ON by default. The pre-releases 0.7.6-meiki.1 and .2 were Meiki's single-line
trial. On the console (0.7.6-meiki.2): two CLANNAD lines took about 8 s each with Meiki.

## [0.7.5] - 2026-10-11

Character-by-character selection, and the Shell's lookup in one 64 KiB arena instead of two of
128 KiB. On the console: OCR in CLANNAD's process, a local lookup and the cursor all worked.

## [0.7.4] - 2026-10-11

Fixed L + R also advancing the game's dialogue: the filter now hides the buttons as CLANNAD reads
them. On the console: two L + R presses in CLANNAD were hidden from the game at the controller hook
(the dialogue itself was not checked).

## [0.7.3] - 2026-10-11

Fixed model loading in the game (a status code arrived as `0xBFFFFFFD`; API 13). On the console:
API 13 booted on firmware 3.65, and CLANNAD ran 15 OCR jobs, 7 of them with text.

## [0.7.2] - 2026-10-11

Fixes from the first console test: the kernel reads the models for the game, which could not open
`ux0:` itself (`0x80010013`), and the settings app's LiveArea images are indexed PNGs, after
VitaShell refused the old ones (`0x8010113D`); also installer repairs.

## [0.7.1] - 2026-10-11

Local OCR (VOCR's FL10 and H15 models) runs inside each enabled game (the worker under `*ALL`),
with a bundled JMdict and the standalone **JP Overlay Settings** app. Not yet run on a console when
it was published.

## 0.7.0 - 2026-10-11

This fork's first tag (no release): offline OCR, a bundled JMdict and per-game settings ([#20]).

## 0.1–0.7: upstream

[Dartv/vita-jp-overlay](https://github.com/Dartv/vita-jp-overlay)'s versions: Vita JP Overlay, with
Google Lens OCR over Wi-Fi and definitions from jiten.moe or jpdb.io (0.1), the subtitle strip and
per-game regions (0.3), PSP games in Adrenaline (0.4) and faster automatic reading (0.6). This fork
started from 0.6; upstream's 0.7, the furigana strip with its Anki fields, came in with 0.8.0-test.1
([#48]).

[Unreleased]: https://github.com/bee-san/Rubi_for_vita/compare/v0.9.1...HEAD
[0.9.1]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.9.1
[0.9.0]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.9.0
[0.8.0]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.8.0
[0.8.0-test.4]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.8.0-test.4
[0.8.0-test.3]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.8.0-test.3
[0.8.0-test.2]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.8.0-test.2
[0.8.0-test.1]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.8.0-test.1
[0.7.12]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.7.12
[0.7.11]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.7.11
[0.7.10]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.7.10
[0.7.9]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.7.9
[0.7.8]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.7.8
[0.7.7]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.7.7
[0.7.6]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.7.6
[0.7.5]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.7.5
[0.7.4]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.7.4
[0.7.3]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.7.3
[0.7.2]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.7.2
[0.7.1]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.7.1
[#20]: https://github.com/bee-san/Rubi_for_vita/pull/20
[#48]: https://github.com/bee-san/Rubi_for_vita/pull/48
[#49]: https://github.com/bee-san/Rubi_for_vita/pull/49
[#50]: https://github.com/bee-san/Rubi_for_vita/pull/50
[#51]: https://github.com/bee-san/Rubi_for_vita/pull/51
[#63]: https://github.com/bee-san/Rubi_for_vita/pull/63
[#64]: https://github.com/bee-san/Rubi_for_vita/pull/64
[#66]: https://github.com/bee-san/Rubi_for_vita/pull/66
[#68]: https://github.com/bee-san/Rubi_for_vita/pull/68
[#71]: https://github.com/bee-san/Rubi_for_vita/pull/71
[#72]: https://github.com/bee-san/Rubi_for_vita/pull/72
[#73]: https://github.com/bee-san/Rubi_for_vita/pull/73
[#75]: https://github.com/bee-san/Rubi_for_vita/pull/75
[#76]: https://github.com/bee-san/Rubi_for_vita/pull/76
[#77]: https://github.com/bee-san/Rubi_for_vita/pull/77
[#78]: https://github.com/bee-san/Rubi_for_vita/pull/78
[#79]: https://github.com/bee-san/Rubi_for_vita/pull/79
[#84]: https://github.com/bee-san/Rubi_for_vita/pull/84
[#85]: https://github.com/bee-san/Rubi_for_vita/pull/85
[#86]: https://github.com/bee-san/Rubi_for_vita/pull/86
[#87]: https://github.com/bee-san/Rubi_for_vita/pull/87
[#88]: https://github.com/bee-san/Rubi_for_vita/pull/88
[#89]: https://github.com/bee-san/Rubi_for_vita/pull/89
[#90]: https://github.com/bee-san/Rubi_for_vita/pull/90
[#92]: https://github.com/bee-san/Rubi_for_vita/pull/92
[#93]: https://github.com/bee-san/Rubi_for_vita/pull/93
[#94]: https://github.com/bee-san/Rubi_for_vita/pull/94
[#95]: https://github.com/bee-san/Rubi_for_vita/issues/95
[#96]: https://github.com/bee-san/Rubi_for_vita/pull/96
[#99]: https://github.com/bee-san/Rubi_for_vita/pull/99
[#104]: https://github.com/bee-san/Rubi_for_vita/issues/104
[#105]: https://github.com/bee-san/Rubi_for_vita/pull/105
[#106]: https://github.com/bee-san/Rubi_for_vita/pull/106
[#108]: https://github.com/bee-san/Rubi_for_vita/pull/108
[#109]: https://github.com/bee-san/Rubi_for_vita/pull/109
[#112]: https://github.com/bee-san/Rubi_for_vita/pull/112
[#116]: https://github.com/bee-san/Rubi_for_vita/pull/116
[#117]: https://github.com/bee-san/Rubi_for_vita/pull/117
[#122]: https://github.com/bee-san/Rubi_for_vita/pull/122
[#123]: https://github.com/bee-san/Rubi_for_vita/pull/123
[#124]: https://github.com/bee-san/Rubi_for_vita/pull/124
[#125]: https://github.com/bee-san/Rubi_for_vita/pull/125
[#126]: https://github.com/bee-san/Rubi_for_vita/pull/126
[#127]: https://github.com/bee-san/Rubi_for_vita/pull/127
[#129]: https://github.com/bee-san/Rubi_for_vita/pull/129
[#130]: https://github.com/bee-san/Rubi_for_vita/pull/130
[#132]: https://github.com/bee-san/Rubi_for_vita/pull/132
[#133]: https://github.com/bee-san/Rubi_for_vita/pull/133
[#140]: https://github.com/bee-san/Rubi_for_vita/pull/140
[#141]: https://github.com/bee-san/Rubi_for_vita/pull/141
[#145]: https://github.com/bee-san/Rubi_for_vita/pull/145
[#146]: https://github.com/bee-san/Rubi_for_vita/pull/146
[#147]: https://github.com/bee-san/Rubi_for_vita/pull/147
[#148]: https://github.com/bee-san/Rubi_for_vita/pull/148
[#149]: https://github.com/bee-san/Rubi_for_vita/pull/149
[#150]: https://github.com/bee-san/Rubi_for_vita/pull/150
[#153]: https://github.com/bee-san/Rubi_for_vita/pull/153
[#154]: https://github.com/bee-san/Rubi_for_vita/pull/154
[#155]: https://github.com/bee-san/Rubi_for_vita/pull/155
[#156]: https://github.com/bee-san/Rubi_for_vita/pull/156
[#157]: https://github.com/bee-san/Rubi_for_vita/pull/157
[#158]: https://github.com/bee-san/Rubi_for_vita/pull/158
[#164]: https://github.com/bee-san/Rubi_for_vita/pull/164
[#167]: https://github.com/bee-san/Rubi_for_vita/pull/167
[#170]: https://github.com/bee-san/Rubi_for_vita/pull/170
[#173]: https://github.com/bee-san/Rubi_for_vita/pull/173
[#174]: https://github.com/bee-san/Rubi_for_vita/pull/174
[#176]: https://github.com/bee-san/Rubi_for_vita/pull/176
[#178]: https://github.com/bee-san/Rubi_for_vita/pull/178
[#179]: https://github.com/bee-san/Rubi_for_vita/pull/179
[#181]: https://github.com/bee-san/Rubi_for_vita/pull/181
[#182]: https://github.com/bee-san/Rubi_for_vita/issues/182
[#185]: https://github.com/bee-san/Rubi_for_vita/pull/185
[#186]: https://github.com/bee-san/Rubi_for_vita/pull/186
[#187]: https://github.com/bee-san/Rubi_for_vita/pull/187
[#189]: https://github.com/bee-san/Rubi_for_vita/pull/189
[#190]: https://github.com/bee-san/Rubi_for_vita/pull/190
[#193]: https://github.com/bee-san/Rubi_for_vita/pull/193
[#194]: https://github.com/bee-san/Rubi_for_vita/issues/194
[#200]: https://github.com/bee-san/Rubi_for_vita/pull/200
[#203]: https://github.com/bee-san/Rubi_for_vita/pull/203
[#204]: https://github.com/bee-san/Rubi_for_vita/pull/204
[#206]: https://github.com/bee-san/Rubi_for_vita/pull/206
[#207]: https://github.com/bee-san/Rubi_for_vita/issues/207
[#208]: https://github.com/bee-san/Rubi_for_vita/pull/208
[#210]: https://github.com/bee-san/Rubi_for_vita/issues/210
[#213]: https://github.com/bee-san/Rubi_for_vita/pull/213
[#214]: https://github.com/bee-san/Rubi_for_vita/pull/214
[#216]: https://github.com/bee-san/Rubi_for_vita/pull/216
[#217]: https://github.com/bee-san/Rubi_for_vita/pull/217
[#218]: https://github.com/bee-san/Rubi_for_vita/pull/218
[#219]: https://github.com/bee-san/Rubi_for_vita/pull/219
[#222]: https://github.com/bee-san/Rubi_for_vita/pull/222
[#224]: https://github.com/bee-san/Rubi_for_vita/pull/224
[#230]: https://github.com/bee-san/Rubi_for_vita/issues/230
[#231]: https://github.com/bee-san/Rubi_for_vita/pull/231
[#232]: https://github.com/bee-san/Rubi_for_vita/pull/232
[#233]: https://github.com/bee-san/Rubi_for_vita/pull/233
[#237]: https://github.com/bee-san/Rubi_for_vita/issues/237
[#239]: https://github.com/bee-san/Rubi_for_vita/pull/239
[#241]: https://github.com/bee-san/Rubi_for_vita/issues/241
[#242]: https://github.com/bee-san/Rubi_for_vita/pull/242
[#243]: https://github.com/bee-san/Rubi_for_vita/pull/243
