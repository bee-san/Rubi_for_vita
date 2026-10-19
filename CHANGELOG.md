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

On `main` since 0.8.0; the next release brings it.

### Added

- An optional small Adrenaline Shell bridge, `compat/adrenaline_vsh.suprx`, in the release zip
  ([how to use it](compat/adrenaline_vsh/README.md)), for an Adrenaline or PSP bubble start that goes
  straight back to LiveArea: on the owner's console, Adrenaline's own bridge did not fit in
  SceShell's memory (`9e36b5e`).
- An opt-in text-presence gate (`ocr_text_gate`, off by default; API 26): a region it finds no text
  in returns no line, so nothing is read off a picture (#214).
- The Rubi app's Games page shows five games a page and searches by title or title ID (#219).

### Changed

- Every verified install deletes the app's own copy of the dictionary when the app's folder can be
  written, and says so on the done screen and in `log.txt` (#200).
- A card's screenshot is written over a file set aside for it beforehand: on the owner's console,
  after a restart, the first write to a new file took about 21 s, so a card took that long or was
  saved without its picture (#208; #207, #210).
- A queued card whose screenshot is missing or damaged is exported without it, and the export says
  so; before, it stayed in the queue (#218).
- The plugins' API is 28: API 26's interface again, after vertical text was merged as API 27
  (#213, #216, #217) and taken out again (#222).

### On the console

- 2026-10-17, with the small bridge: Boku no Natsuyasumi 4 started from its LiveArea bubble, and the
  kernel loaded Rubi's OCR worker into the bubble's PSP emulator process (`PSPEMU004`). Reading text
  in a PSP game is still untested.
- A build made to measure the screenshot fix (#207): the first screenshot after a restart took
  719 ms to capture and encode and 74 ms to write, and saving a card with its screenshot took
  523–905 ms. The finished fix has not run on a console yet.

## [0.8.0] - 2026-10-17

The first release as Rubi, published by the owner from `9ea6542` (API 25; vita-vn-ocr runtime
`d214a05`). Since test.4 it adds the fixes of the owner's test.4.1–4.3 runs, the rename and
prepared lookups; everything else new since 0.7.12 came in the test builds below.
[Release notes](https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.8.0).

### Added

- Prepared lookups: with Read text on Automatically and Whole word highlighting, idle time prepares
  the lookups of up to eight upcoming words, and dictionary handles are kept for the current dialogue
  (#203; measured as host operation counts, not timed on a console).

### Changed

- Renamed to **Rubi** (Rubi for Vita; #189, [what changed](docs/rename-rubi.md)). The app was
  **Overlay** in the 0.8.0 test builds and **JP Overlay Settings** before (0.7.12 and older). Its
  title ID (`VJOSET001`) stays, so the new VPK installs over either and keeps your settings, games
  and Anki cards. The data folders, the plugin files and the Anki deck keep their old names.
- Two downloads: `Rubi-0.8.0.vpk`, the normal build (the main download), and
  `Rubi-0.8.0-debug.vpk`, the test builds' build with the full log and the self-test, each also as a
  zip for the computer installer.

### Fixed

- A dialogue box's short last line is read: the textbox recovery finder for every model (`6674125`)
  and a short-line probe after the finder (`49310ed`).
- A card's screenshot is saved: the kernel's save syncs nothing (`b622c18`).
- When the app cannot read its own folder yet, right after VitaShell installs it, the Install page
  says `Restart the Vita first`, and it logs every reason it shows (`b6755cf`).
- The kernel changes the active game only under its lock (#190).

### On the console

- Its release notes say its lookup changes (#203) were not installed or timed on a console. Its
  fixes ran in test.4.1–4.3 (below).

## 0.8.0-test.5 - cancelled

Planned after test.4 to check its fixes and run what test.4 had not: Small, Read text on a press,
the boost off, the Games page's Default OFF, a PSP game, and the dictionary's grammar and kanji lines
as steps of their own. It was cancelled when 0.8.0 shipped, and none was published
([#191](https://github.com/bee-san/Rubi_for_vita/issues/191),
[#194](https://github.com/bee-san/Rubi_for_vita/issues/194)).

## 0.8.0-test.4.3 - 2026-10-17

Not published: `main` at `b622c18`, installed on the owner's Vita
([#194](https://github.com/bee-san/Rubi_for_vita/issues/194)).

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

The last test build before the release, and the first made to be installed as one VPK
(`VitaJPOverlay-0.8.0-test.4-debug.vpk`): `main` at `3ce0f47`, API 24. It still has the old names:
the app is **Overlay**. Install steps:
[#191](https://github.com/bee-san/Rubi_for_vita/issues/191); the owner's report:
[#194](https://github.com/bee-san/Rubi_for_vita/issues/194).
[Release notes](https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.8.0-test.4), with the full
list.

### Added

- The one-VPK install: the VPK carries the whole install, and the app's Install page installs,
  updates, checks and repairs, and uninstalls (#158 and the PRs after it).
- VOCR5 as the three model sizes, Small, Normal (the default) and Large, with the 1 MB character
  language model (#167).
- Bee's Ultimate Dictionary (#153), with Jiten's frequency rank on every entry (#127) and faster
  lookups (#140, #143).
- Anki cards keep a screenshot of the game, made in the game's own process (#150, #164, #174).
- **Read text** (Automatically, or Only when I press L + R) and **Speed boost while reading
  (444 MHz)** in the app (#146).
- Two builds: the normal build (short log, no self-test) and the debug build (#141, #147, #155).
- Automatic reading keeps each new dialogue text in `history.csv` (#187).
- Adrenaline Bubble Manager bubbles are listed as PSP games, under their game's title (#124, #170).

### Changed

- The kernel plugin loads the OCR worker into every game that is ON, Adrenaline included: no
  taiHEN line per game, no `--title` (#148).
- The app's Games list lists every title `games.ini` names, and Default OFF turns every game OFF
  (#149).
- The normal build makes no log append on the way to a result (#154).
- The left stick moves the cursor fast and looks a word up once it rests (#156); Left and Right cross
  lines and stop at the text's ends (#173).
- A word split over two lines is looked up whole, and the overlay breaks its own lines between
  phrases (#178).

### Removed

- Google Lens and the whole network stack: Rubi is offline only (#130, #132, #145, #157).

### Fixed

- The app opens with no game running (#181).
- No `Recognizing…` left on screen after a cancelled lookup (#95, #179); config warnings never quote
  a value (#185); the subtitle strip after a new region or a game switch (#182, #186).

### On the console

The owner's PCH-1001 with CLANNAD, 2026-10-17:

- VitaShell's first install stopped with error `0x80101114` while test.3's app was still open,
  suspended behind it; after a restart the VPK installed.
- The app, opened straight after, refused to install (`Cannot install the overlay (state 4)`), and
  an Uninstall did not change that; the lead, unconfirmed, is that it could not read its own folder
  until a restart. The install was finished by hand over FTP (since test.4.1 the page says `Restart
  the Vita first` in that case).
- After the restart the app opened from LiveArea with no game running, and Check and repair wrote
  nothing.
- The kernel plugin loaded the OCR worker into CLANNAD with no taiHEN line for it.
- VOCR5 Normal took 2.61 ms per CTC frame (10 jobs) and Large 4.98–5.10 ms (8 jobs), both at
  444 MHz. Large never fell back to a smaller size: CLANNAD had 87.0–91.2 MB free. Every job that
  recognized a line raised the clock 333 → 444 → 333 MHz.
- The dictionary's popups, the left stick and the export of 20 cards worked.
- Both cards' screenshots timed out after 15 s, while the game froze and its audio cut in and out;
  the saves took 17.6 and 23.7 s (fixed in test.4.3).
- A box's short last line was dropped by the line finder, as in test.3 (fixed in test.4.1 and 4.2).
- Not run: Small, Read text on a press, the boost off, Default OFF, a PSP game, and the dictionary
  steps.

## [0.8.0-test.3] - 2026-10-16

One zip, installed with `install-vita.sh --title` and the app's VPK: `main` at `9cfa920`, API 22.
Install steps and results: [#138](https://github.com/bee-san/Rubi_for_vita/issues/138).
[Release notes](https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.8.0-test.3).

### Added

- Model sizes in the app: Small (VOCR3-FL10), Normal (VOCR3-H15, the default) and Large (VOCR4-W20),
  with a smaller one taken by itself when a game is short of memory (#108, #105).
- Faster OCR: up to 3 lines at once, a cache of 8 lines, and 444 MHz while a job reads (#93).
- A fast first scan: the self-test runs in a background job of its own, and each model file is read
  once per game process (#116).
- Anki: the app exports the queued cards as one `.apkg` of Lapis notes (#125, #133).
- The app and the plugins check that they are one build (#122).

### Changed

- The app is a system application: it opens beside a suspended game (#109, #112, #129).
- Enter and Cancel follow the console's own setting (#106).
- A key filter in the dictionary saves about 6 file reads per lookup (#123; host).

### Removed

- AnkiConnect (#125), the online dictionaries jpdb.io and jiten.moe, and the Hachidori relay (#117,
  #126).

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
- PS with the overlay open worked (#104). The app opened beside CLANNAD and closed cleanly, but not
  from LiveArea with no game running (the owner's report; why is still unexplained).
- AIR (`PCSG00940`) got no OCR: this build loaded the worker only into a game with its own taiHEN
  line, and AIR had none; its jobs ended `rc=-117`. The tester saw the same in Adrenaline (#148
  changed this in test.4).
- CLANNAD was missing from the app's Games list, and Default OFF did not turn it off (#149 changed
  this in test.4).
- Log appends took up to 4.4 s of an 8.6 s job (since test.4 the normal build makes none on the way
  to a result, #154).

## [0.8.0-test.2] - 2026-10-15

`main` at `4ac02ac`, API 20. Install steps and results:
[#101](https://github.com/bee-san/Rubi_for_vita/issues/101).
[Release notes](https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.8.0-test.2).

### Added

- test.1's six requests: Up and Down by displayed rows, Left and Right wrapping at a row's ends (#87);
  the OCR box drawn with the stick (#86); no dialogue-changed warning (#92); VOCR3 and Google Lens as
  the two engines (#85); Anki set up in the app (#90).
- Typing text is read once it is whole (#84), and each mark is shown and looked up in one form (#88).
- Beam search with a 1 MB character language model, and a cascade that reads a doubtful line again
  with a larger model (#89).
- The line finder's boxes are refined in PSP games (#94).

### Removed

- VOCR2 (#85).

### On the console

The owner's PCH-1001 with CLANNAD, 2026-10-15:

- The self-test passed every stage, and all 16 installed files matched the package.
- Up and Down by rows, Left and Right wrapping and the stick-drawn box worked; no dialogue-changed
  warning was seen.
- The run used test.1's app, so the engine choice and the Anki page were not tested fairly, and the
  old app wrote `ocr_backend = vocr2` into `config.ini` (since test.3 the app and the plugins check
  that they are one build, #122).
- The first scans took 9–15 s, because the self-test ran inside them (#116, in test.3, whose first
  scan took under 1 s); later scans were faster.

## [0.8.0-test.1] - 2026-10-14

`main` at `b548dc9`, API 19. Install steps and results:
[#81](https://github.com/bee-san/Rubi_for_vita/issues/81).
[Release notes](https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.8.0-test.1).

### Added

- VOCR3 as the default model (VOCR3-H15, falling back to VOCR3-FL10), logging on by default, and
  offline Anki cards saved to the memory card (#77).
- `--collect`, with the memory headroom of each stage (#78).
- Ranked dictionary entries, with one `log.txt` line per lookup (#79).

### Changed

- The memory fixes since 0.7.12: SceShell's pages given back (#68), the OCR worker loaded per game
  (#72), the screen read in the game in grey strips (#75), and the memory routes logged (#76).

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
trial: on the console, two CLANNAD lines took about 8 s each.

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

This fork's first tag (no release): offline OCR, a bundled JMdict and per-game settings (#20).

## 0.1–0.7: upstream

[Dartv/vita-jp-overlay](https://github.com/Dartv/vita-jp-overlay)'s versions: Vita JP Overlay, with
Google Lens OCR over Wi-Fi and definitions from jiten.moe or jpdb.io (0.1), the subtitle strip and
per-game regions (0.3), PSP games in Adrenaline (0.4) and faster automatic reading (0.6). This fork
started from 0.6; upstream's 0.7, the furigana strip with its Anki fields, was merged in after 0.7.12
(#48).

[Unreleased]: https://github.com/bee-san/Rubi_for_vita/compare/v0.8.0...HEAD
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
