# The Rubi guide

Everything about Rubi in detail: installing it (also from a computer), every control and setting,
the dictionaries, Anki, memory, logs and recovery, and building and testing it. The
[README](../README.md) is the short version.

Until the short README replaced it, this page was the README. Its text moved here unchanged, with
its links adjusted to this folder.

Contents:

- [Rubi — local Japanese OCR](#rubi--local-japanese-ocr)
- [Installation instructions](#install)
  - [Alternative: install with the standalone VPK](#alternative-install-with-the-standalone-vpk)
  - [Advanced: install from a computer](#advanced-install-from-a-computer)
- [Use in a game](#use-in-a-game)
  - [Drawing the region](#drawing-the-region)
- [Per-game settings](#per-game-settings)
- [Settings and dictionaries](#settings-and-dictionaries)
  - [Removed features](#removed-features)
  - [Anki](#anki)
- [Memory and troubleshooting](#memory-and-troubleshooting)
- [Build and validation](#build-and-validation)
- [Credits and license](#credits-and-license)

## Rubi — local Japanese OCR

**Rubi** (Rubi for Vita) is the new name of Vita JP Overlay ([what changed](rename-rubi.md)).
This [bee-san fork](https://github.com/bee-san/Rubi_for_vita) reads visual-novel
text and shows Japanese dictionary entries over your game. **Rubi reads the text on the Vita
with its own OCR, automatically, and looks words up in the bundled Bee's Ultimate Dictionary.**
Recognition and dictionary lookup work without Wi-Fi or a running computer.
The release includes **Rubi**, a standalone per-game settings app. Optional
[Anki card mining](anki.md) is fully offline: Enter saves the shown word as
a card in a queue on the memory card, and the Rubi app's Anki page, where it
is also switched on, exports the queue as [Lapis](https://github.com/donkuri/lapis)
notes into an `.apkg` file that you copy to Anki yourself (FTP, VitaShell's USB
mode, or an SD2Vita's card). Nothing is sent from the Vita; there is no
AnkiConnect or AnkiWeb sync.

Text is recognized on the Vita, offline, by the local engine, named after the
generation of the model it runs (**VOCR6** for all three sizes in this package), automatically or
when the overlay opens. In the **Rubi** app (press **Select** for the
OCR & lookup page), **Model size** chooses its recognizer: **Small**
(fastest), **Normal** (the default, balanced) or **Large** (most accurate), each
shown with its estimated time per textbox and the game memory it takes
([model sizes](vita-vn-ocr.md#model-sizes-small-normal-large-api-21)). When
a game has too little free memory for the chosen size, a job runs the next
smaller one by itself; only `log.txt` says so. The release stores the three
sizes' models; a choice applies when you next open the overlay.
Google Lens, the VOCR2 trial, Meiki, the online dictionaries and the Hachidori relay
were removed ([what becomes of their settings](#removed-features)).

The engine's name comes from the pinned metadata of the model it runs
(`core/vocr_models.c`, also written per model into the package's
`package-manifest.json`), not from the app: VOCR6 for this package's three sizes (vita-vn-ocr's v0.4.1-vocr6.2
pre-release, retrained on the owner's hooked games; Normal with its head gate and two-level head, Large with its
two-level head, Small plain); in the test builds v0.9.8-test.4 and test.5 VOCR6 for Normal and Large and VOCR5 for
Small, VOCR5 for all
three sizes in 0.8.0 to 0.9.8, and before it VOCR3 for Small and Normal
and VOCR4 for Large). A release that pins a new generation's models as the sizes
shows that generation's name, with no change to the app; a size that keeps an
earlier generation's model shows that one's. A `.vocr` file itself carries no
generation. v0.2.0's models are generation VOCR1.

VOCR has completed actual CLANNAD OCR jobs on the physical Vita.
Hardware accuracy/CER and broader game compatibility remain unmeasured; host crop results are separate.
See [OCR measurements and limits](vita-vn-ocr.md) and
[project lessons](history/project-lessons.md).

<a id="install"></a>

## Installation instructions

You need a PS Vita on firmware 3.60–3.74 with HENkaku/Ensō or h-encore, VitaShell, and
**Settings > HENkaku Settings > Enable Unsafe Homebrew** turned on.

Download the [Rubi .zip](https://github.com/bee-san/Rubi_for_vita/releases/download/v0.10.0/Rubi-0.10.0.zip)
and extract it.

### 1. Transfer modules

Using VitaShell FTP, transfer the files from `Rubi-0.10.0/ur0/tai/` to `ur0:tai/` on the Vita:

- `VitaJPOverlay_Kernel.skprx`
- `VitaJPOverlay_OCR.suprx`
- `VitaJPOverlay_Shell.suprx`

### 2. Edit the taiHEN config

Back up `ur0:tai/config.txt`, then add `ur0:tai/VitaJPOverlay_Kernel.skprx` to its `*KERNEL`
section and `ur0:tai/VitaJPOverlay_Shell.suprx` to its `*main` section. The two sections can
appear in either order; each plugin must be beneath its matching section. Keep your other entries.

Example (`ur0:tai/config.txt`):

```text
*KERNEL
ur0:tai/VitaJPOverlay_Kernel.skprx

*main
ur0:tai/VitaJPOverlay_Shell.suprx
```

If `ux0:tai/config.txt` exists, taiHEN reads it first: back it up and add the entries there instead.
`VitaJPOverlay_OCR.suprx` needs no config entry; the kernel plugin loads it into enabled games.

### 3. Transfer data files for the plugin

Transfer `Rubi-0.10.0/ur0/data/VitaJPOverlay` to `ur0:data/` on the Vita, so the folder is present
as `ur0:data/VitaJPOverlay`.

### 4. Transfer data files for OCR and the dictionary

**This is a separate folder, not a duplicate of step 3.** Transfer
`Rubi-0.10.0/ux0/data/VitaJPOverlay` to `ux0:data/` on the Vita, so the folder is present
as `ux0:data/VitaJPOverlay`.

If updating an existing installation, back up your `ux0:data/VitaJPOverlay/config.ini` first
and keep your existing settings file when copying the data.

### 5. Transfer and install the VPK / companion app

Close Rubi first if it is already open, including when suspended behind VitaShell: pull the
top-right corner of its LiveArea page. Transfer `Rubi-0.10.0/Rubi.vpk` to the Vita and install
(promote) it with VitaShell. Accept the installation and extended-permissions prompts.

### 6. Reboot the Vita

In VitaShell, press **START** and choose **Reboot**.

Start a game and press **L + R** to open Rubi. Every game is ON by default.

### Alternative: install with the standalone VPK

The separate `Rubi-<version>.vpk` download includes the files that the ZIP method copies by hand.
With that download, use the app's Install page as follows:

1. Download `Rubi-<version>.vpk` from the
   [latest release](https://github.com/bee-san/Rubi_for_vita/releases/latest).
2. Copy it to the Vita with VitaShell's USB or FTP connection (Select in VitaShell starts the one
   chosen under **SELECT button** in its settings, Start; a Vita's default is USB) and install it in
   VitaShell: press Enter on the file, then Yes to "Do you want to install this package?" and to its
   notice that the package requests extended permissions.
3. Open **Rubi** from LiveArea. It opens on its **Install** page, which lists exactly what it will
   do; choose **Install**, then restart the Vita.

Nothing is downloaded on the Vita: the VPK holds the plugins, the models and the dictionary
([how it works](one-vpk-install.md)). Then start a game and press L + R. Every game is ON by
default and needs no step of its own: the kernel plugin loads the OCR worker into each game that is
ON as it starts ([worker loading](worker-loading.md)), never into SceShell, a system app,
VitaShell or the Rubi app, and never while L is held at the game's start. Close the Rubi app
before installing a VPK over it (on its LiveArea page, pull the top-right corner), also when it is only
suspended behind VitaShell: in 0.8.0-test.4's console run VitaShell stopped with error `0x80101114` while the
old app was open, and installed after a restart ([#194](https://github.com/bee-san/Rubi_for_vita/issues/194)).
If the Install page then says `Restart the Vita first`, the app cannot read its own folder yet, as
right after VitaShell installs it: restart the Vita and open Rubi again (nothing has been written).
That path is host-tested only: in the owner's 0.8.0-test.4.1 to 4.3 runs, with the app closed before
VitaShell's install, the app read its folder every time, and its **Update** ran three times, each
over the build before it ([#194](https://github.com/bee-san/Rubi_for_vita/issues/194)). An install on a Vita
without the overlay has not run on a console yet.

**What Install does.** It copies the three plugins to `ur0:tai` and the models to
`ux0:data/VitaJPOverlay/ocr`, moves the dictionary (157 MB) out of the app rather than copying it,
backs up every taiHEN configuration it changes as `config.txt.vjo-<date-time>.bak` (kept), adds the
kernel plugin under `*KERNEL` and the Shell plugin under `*main` in `ux0:tai/config.txt` and
`ur0:tai/config.txt` (whichever exist; taiHEN reads ux0's first), removes the OCR worker's
per-game and `*ALL` lines older installs left, keeps your `config.ini` settings (it writes the
template only when there is none), and reads everything back. Every other plugin of yours loads as
before: the edit is checked with taiHEN's own parser. It never restarts the Vita and never retries by
itself; a stop part way leaves every file the old or the new one, and the next start of the app
shows what is on the card and offers to finish or to keep what is there (never a half-written
configuration). Over the same build, Check and repair leaves your `config.ini` as it is.

**Updating** is the same three steps with the new VPK: the app sees that the installed overlay is
another build's and its Install page offers **Update**. **Uninstall** is on the same page: it removes
the overlay's lines (with a dated backup first) and keeps the files and your settings and cards.

Releases up to 0.9.6 also have `Rubi-<version>-debug.vpk`, the test builds' own build with every log
line, the self-test and the timings; 0.9.7 and later releases are the normal build only, and every test build (a
pre-release) has the debug one. Install it only when asked for a full log, or for a
[hook request](hook-requests.md); either installs over the other and the settings carry over
([release-variants](release-variants.md)).

Open **Rubi** from LiveArea to enable or disable individual games. Older builds call it **Overlay**
(the 0.8.0 test builds) or **JP Overlay Settings** (0.7.12 and older); the title ID (`VJOSET001`) is
unchanged, so the new VPK installs over it, and the app keeps its settings, games and Anki cards. An
earlier PC install left the app's VPK under its old name, `ux0:data/JPOverlaySettings.vpk`, which the
older app's red banner names: `install-vita.sh` puts the new app there too, so either file installs the
Rubi app. Once the Rubi app is installed, that file is no longer used and can be deleted. The settings app itself is always excluded from the overlay. Each
build's app carries a version stamp (`vjo_app.txt`) and the plugins a record of the app they came
with (`ux0:data/VitaJPOverlay/vjo_app.txt`); when the two differ, the Shell plugin says so once, in
red under the recognized text of the first overlay opened in a game (`Rubi app and plugins are
different builds: open the Rubi app.`), and the app's Install page says which way it is.

The Rubi app is a system application, like the Music or Video app: it
opens beside a running game, which stays suspended behind it, so there is no
need to close the game first. It should also open from its LiveArea with no game
running. In v0.8.0-test.3, the first build whose app is a system application, it
did not (the owner's report, 2026-10-16); in 0.8.0-test.4's console run it did
([#194](https://github.com/bee-san/Rubi_for_vita/issues/194)).
That start's log line (`GPU library 0x0A: 0x00000000; display: buffer 2 at read 1 after 0 ms`)
shows that neither cause below applied there, so 0.8.0-test.3's failure is still unexplained.
A possible cause, found in its graphics library's source and not
seen on a console: its start waited for SceShell to hand it the screen without
letting SceShell's threads at or below its priority run, and used the system's
shared GPU parameter buffer with no fallback. It now pauses for SceShell, falls
back to a buffer of its own, closes itself after 10 s if the system never hands it
the screen, and says how its start went in `log.txt` (`Rubi app: graphics: ...`;
[no game running](vita-memory.md#starting-with-no-game-running), with what each
line means). Games turned ON or OFF there take effect when the
game next starts or resumes, except that a game turned ON while it runs gets
OCR at its next start (the worker is loaded as a game starts; until then the
overlay says `OCR is not running in this game. Close the game and start it
again.`); preferences apply when the overlay next opens. A
game that uses extended memory (`ATTRIBUTE2` in its `param.sfo`) may still have
to close first, as for other system applications.

PSP games run in Sony's PSP emulator. That process has the title ID
of the app that started it (`PSPEMU004` observed on a console on 2026-10-17),
as Adrenaline's and AdrBubbleBooter's own plugins key on it: `PSPEMUCFW` for Adrenaline
itself, and the bubble's own ID for a game started from its own LiveArea bubble
([Adrenaline Bubble Manager](https://github.com/ONElua/AdrenalineBubbleManager)
bubbles, booted by [AdrBubbleBooter](https://github.com/shoui520/AdrBubbleBooter-oss)), such as
`PSPEMU005` or `ULJS00378`. The kernel plugin loads the worker into these processes
like any game: by the ID's own row in the Rubi app, else the shared PSP row,
else the default. `NPXS10028`, the emulator's own ID, is treated the same should
the kernel see it. With `log_file = on` (the default), `log.txt` has a `psp:`
line for each PSP game as it starts, naming its IDs, its disc ID and the file it
boots.

### Advanced: install from a computer

Each release also has `Rubi-<version>.zip` (and `-debug.zip`) for the computer-side
installer, the developers' path. It needs Python 3.9+, bash and curl. Close games, open VitaShell,
set **SELECT button** to **FTP** in its settings (Start; once: a Vita's default is USB, #136), press
Select to start FTP, and run from the extracted folder:

```sh
bash install-vita.sh VITA_IP:1337
```

It checks the package manifest, backs up both taiHEN configurations in `build/device/backup/` of the
extracted folder, and on the Vita each one it changes (as `config.txt.vjo-bak` and a dated
`config.txt.vjo-<date-time>.bak`), and your overlay settings, uploads and checksum-reads back each file, and edits the
configurations with the same rules as the Rubi app (`tools/patch_tai_config.py`, the app's
`core/taicfg.c` in Python; both are tested on one set of fixtures). `--title` is no longer needed and
is ignored with a note. It ends with a box that says whether the Vita's Rubi app is this
package's: install `ux0:data/Rubi.vpk` in VitaShell afterwards (this zip's app carries no
overlay files of its own, so its Install page names this installer);
`bash install-vita.sh --check-app VITA_IP` asks again, read-only. The 0.7.12 archive's own
`install-vita.sh` predates all of this and registers the worker under `*ALL`; use the installer from
`main` or a later release.

For a manual installation, follow [Installation instructions](#install) above. If replacing an old
install, disable old `VitaJPOverlay_Text.suprx` entries and old OCR worker lines. Never list
`VitaJPOverlay_OCR.suprx` under `*ALL`; the kernel plugin loads it into enabled games. Keep unrelated
plugins and configuration intact.

## Use in a game

**Enter** and **Cancel** are the Vita's own: × enters and ○ cancels on most
consoles, the other way round on Japanese ones, and HENkaku can swap them on
any console. The overlay, its hints and the Rubi app follow that setting
(`/CONFIG/SYSTEM/button_assign`): the overlay reads it each time it opens, the
app when it starts.

| Control | Action |
| --- | --- |
| L + R | Open or close the dictionary overlay |
| Select + R | Toggle the subtitle strip |
| Left / Right | Move through the text in reading order, across its rows: skip matched words in Whole word mode, one character in Character mode (the left stick: one character in both); Right from a row's last word goes to the next row's first, Left from a row's first to the row above's last, and the text's first and last stop the cursor. A text on one row goes round its ends (Right from the last to the first, Left from the first to the last). Held, it repeats (after 0.35 s, 10 steps a second), stopping at the text's ends |
| Touch a recognized character | Look up the word starting there with a local dictionary |
| Up / Down | Move to the displayed row above/below, soft-wrapped rows included, keeping the horizontal position (held: repeats). **Up on the text's top row shows this game's earlier line** (with **Read text: Automatically**), dimmed between its neighbours: more Up goes further back, up to 32 lines; Down on an earlier line's last row goes to the line after it, and past the newest back to the current text ([line history](line-history.md)) |
| Left stick | Move the cursor in the d-pad's four directions, faster while held: left/right a character at a time in reading order across the rows, in Whole word mode too (the shown word is not skipped, and its left never retraces a Right), up/down a row as Up/Down, a diagonal as the direction pushed farther; it stops at the text's first and last character, a one-row text's too. With **Japanese characters only** ON it lands only on kana and kanji, OFF on every visible character, punctuation and Latin included. Once the cursor rests for `stick_lookup_ms` (200 ms), not at each step, the word starting at its character is looked up and, in Whole word mode, highlighted ([left stick](touch-lookup.md#the-left-stick-138)) |
| Right stick | Scroll a long definition |
| Triangle | The line's source (hook or OCR), then **characters read, reading time and characters/hour** of this game: today, and its total. Time estimates active reading: each line counts the time to the next, at most 2.5 times what your pace would take for it (`reading_time = adaptive` in `config.ini`, the default, from the Vita's next start; `15s` is 0.9.7's 15-second cutoff, no setting in the app: [reading time](ocr-history.md#reading-time)); speed uses timed lines only. Its last line is your reading streak, all games together: `Streak: current 3 days · longest 12`, the days in a row with a line recorded, ending today (or yesterday while today has none yet), and the most ever ([streak](ocr-history.md#streak)). Triangle again brings the entry back. The first press may show `counting…` while history is counted in background windows; subsequent presses read cached totals. [Details](ocr-history.md#total-characters-read). |
| Square | Draw the dialogue region with the left stick or the touchscreen ([Drawing the region](#drawing-the-region)) |
| Hold Square | Restore the full-screen region |
| Enter | Save the shown word as an Anki card in the offline queue, with card mining on ([Anki mining](anki.md)) |
| Cancel | Close the overlay |

**Earlier lines.** With **Read text** on **Automatically**, the overlay can go back through this game's
earlier dialogue, the lines `history.csv` keeps ([ocr-history.md](ocr-history.md)): Up on the text's
top row shows the line before it, in white between the last row of the line before that and the
first row of the line after it, both dimmed, and the body's first line says how far back it is
(`Earlier line -2`). Everything works on it as on the current text: the cursor, taps, the left stick,
lookups, the other dictionaries' entries below the word and Enter, whose card has that line as its sentence and **no
screenshot** (Rubi keeps no frame of an earlier line; the status line says so). Down past the newest
earlier line comes back to the current text, its cursor where it was; closing the overlay does too.
Up and Down between lines are the d-pad's: the left stick stops at the text's first and last rows.
With **Only when I press L + R**, which records no line, Up on the top row does nothing, as before.
The last 24 KiB of `history.csv` are read when Up first goes past the top row (about 27 three-row
textboxes of every game, at most 32 of this game's lines kept): [line history](line-history.md).

Pressing PS with the overlay open closes it: the game's controls are released
in the suspend's own event, the overlay no longer turns off the LiveArea's
touch region on its way out (#104), and the game resumes with the overlay
closed. Host tests cover this; the console run is still to confirm it
([PS and resume](touch-lookup.md#ps-suspend-and-resume-104)).

Choose a tight dialogue region first: smaller regions need less RAM and reduce
recognition errors. Regions are saved per game. Automatic mode recognizes
settled dialogue in the background and prepares its first local match in Whole
word mode. A completed lookup appears immediately when opening unchanged
dialogue. Character mode defers dictionary lookup until opening the overlay.
Selection changes never recapture the screen or rerun OCR. While the overlay is
open, its text stays usable for lookups and Anki whatever the screen does, with no
"dialogue changed" message: in Auto mode a newly settled screen is recognized in the
background and replaces the text quietly only when it reads a different one
([touch lookup](touch-lookup.md)). A job that read a screen changing under it
shows nothing and is retried quietly. The Rubi app's **Read text** chooses this, **Automatically
(when it changes)**, the default, or **Only when I press L + R** (`ocr_mode = on_press`), which recognizes
only when the overlay opens: nothing runs before, and the press then waits for the reading
([what each costs, and 0.8.0-test.3's times](auto-vs-press.md)). With card mining on, Enter saves the shown word,
its sentence and every dictionary's entry for it to the card queue on the
memory card, with a JPEG of the game's screen (about 60 KB, made in the game's
own process: [screenshots](anki.md#the-screenshot)); the Rubi app exports
the queue as an `.apkg`, the screenshot in each note's Picture.

### Native text hooks

A native text hook reads the line a game is about to draw, so the popup gets the game's own text
instead of OCR's reading of the screen, as Textractor and LunaTranslator do for PC games. Rubi's
hooks run in the game's own process, installed as the game starts; how, and why a site is
installed or refused, is in [docs/native-hooks.md](native-hooks.md). CLANNAD's string-copy source was
verified on the owner's Vita with the combined debug test build on 2026-10-20. Table-address hooks,
including CLANNAD's register site, remain unverified on a console, and the normal release build has no
reported console run. The behaviour below is checked on the host.

- **Text source** (the Rubi app's OCR & lookup page, `text_source` in `config.ini`) applies when a
  game next starts: **Automatic** (`auto`, the default) tries the game's own string-copy hooks in every
  native Vita game that is ON, plus available table sites from Rubi or LunaTranslator.
  Seven titles that the table lists have no ported site (PCSB00985, PCSG00291, PCSG00451, PCSG00595,
  PCSG00663, PCSG01081, PCSG01152): Automatic still tries their string-copy hooks, as it does in games
  with no table entry, and falls back to OCR if they provide no usable dialogue.
  **Every game** (`hooks`) is retained for existing configurations and now makes the same attempts as
  Automatic; **OCR only** (`ocr`) installs none. PSP games still use OCR only.
  An install or an update keeps the file's value; installs before this one wrote `text_source =
  ocr`, so choose Automatic once to use hooks.
- **Trusted hooks replace OCR** (the Rubi app's OCR & lookup page, `hook_trust` in `config.ini`, on by default,
  applies when the overlay next opens): a hook Rubi trusts gives the line with no OCR at all, no model read and
  no OCR memory kept back for it, so a game short of memory still gets its lines. Trusted: LunaTranslator's and
  Rubi's own verified sites from their first line, and a string copy (or any other hook) once it has agreed
  with OCR four times, on four different lines. OCR comes back by itself when no trusted hook has text (or a
  string copy's caller is replaced). The price: a screen the hook did not copy (a choice menu, the backlog)
  shows the hook's last line. A string copy that earned trust is trusted again at the game's next start (the hook
  cache keeps that apart from its learned interval). `hook_trust = off` checks every hook with OCR as 0.9.7 did, below,
  its cache's interval and writes included, but for one write at the first launch with it off after one with it on
  (a source saved trusted is saved again without the trust) ([details](native-hooks.md#trusted-hooks-api-43)). A trusted line is also read at once (API 49): within 16 ms of
  the game's copy, without waiting for the screen to settle or the line to finish typing, so L + R opens on it with
  its first word looked up, and the open overlay shows each new line as it starts
  ([details](native-hooks.md#no-wait-for-a-trusted-line-api-49)).
- **What is shown** with `hook_trust = off`. Nonempty Luna hook text is preferred immediately, including short
  or unchanged lines, without initial or periodic OCR checks. With multiple Luna streams, the newest wins.
  When Luna supplies no text, regular hooks use the following validation and cache.
  A regular hook first agrees with OCR on two different dialogue lines (70 % normalized
  similarity, four characters at least). It then answers seven new readings without OCR and checks the
  eighth. Successful scheduled checks grow the interval to 16, then 32. Reopening unchanged dialogue
  does not advance it. Rubi remembers the source for the exact game build and confirms it once after
  restart. One disagreement uses OCR and checks the next new line; agreement resumes at eight, while
  two comparable disagreements discard confidence. Empty OCR or temporarily missing dialogue does not
  erase it. Longer gaps can delay detection of incorrect hook text by up to 31 hook-only readings.
  Missing or unwritable cache storage means fresh learning. Choose **OCR only** and restart the game
  to disable hooks.
- **When it is read.** Once a hook answers without OCR, its new line is read as the game copies it
  (with **Read text** on **Automatically**): the overlay opened as the line finishes typing usually shows
  it at once, its first word looked up. Lines in quick succession (a skip) are read once the screen
  settles, as before ([details](native-hooks.md#a-new-line-starts-the-read)).
- **The popup** shows the dialogue and dictionary entries. Source selection is automatic; there is
  no hook number, source-status label or Triangle source picker. Technical source details remain in
  the debug log.
- **The Games page** tags a game with a hook entry: `Hook` for one of Rubi's own verified entries (CLANNAD,
  アマツツミ and the 26 Vita3K-farm games of 0.9.7, then the three MAGES SC3 games, STEINS;GATE 比翼恋理のだーりん,
  ROBOTICS;NOTES ELITE and CHAOS;CHILD: the worker's own table, so each new entry is tagged), `Luna hook`
  for a ported site in LunaTranslator's table (its entries are not checked by Rubi; the worker installs one only where
  the game's code there looks as the entry expects). The seven titles above, whose every site in the
  table is unported, have no tag.
- **Logs.** `log.txt` has one line per game session with what the worker installed (`hooks in
  PCSG00415: text source auto, 1 table sites and 8 string copies hooked, 0 sites refused`; the normal
  build only where something was hooked or refused); the debug build also rewrites
  `ux0:data/VitaJPOverlay/hooks.log` at each game start (each site's verdict and why, each string copy
  hooked or not, the game's version, no text: [native-hooks.md](native-hooks.md#logs)), and adds to `log.txt` each job's source, flags,
  agreement score, the streams with text and the pick (`game vocr source: hook 9, flags 0x03, score
  1000, streams 0x00901, pick 0`) and logs the shown text, a hook's too. The normal build logs no
  hook text, as it logs no OCR text. `history.csv` gets the text shown, with `hook` instead of `vocr`
  as its engine for a hook's line and `hook` or `ocr` in its last column, `line_source`
  ([ocr-history.md](ocr-history.md#line-source)); `kernel.txt` has `text source N` when the setting changes.
- **If a game misbehaves with hooks**, hold L while starting it (no Rubi in it at all), turn it OFF
  on the Games page, or set Text source to OCR only (`text_source = ocr`): each applies at the
  game's next start. A safe boot ([boot safety](boot-safety.md)) installs no hook anywhere.
- **A game without a hook** (or whose hook the worker refuses): ask for one with a
  [hook request](https://github.com/bee-san/Rubi_for_vita/issues/new?template=hook-request.yml). Play the game once with
  the debug build and Text source on Automatic, then send its `hooks.log`; `tools/install_ftp.sh --collect` fetches it,
  and from a debug zip made after v0.9.8-test.2 writes `hook-request.md` beside it ([hook requests](hook-requests.md)).
- **Table sites off, for a test**: `hook_sites = off` in `config.ini` (no setting in the app; `on` by
  default) installs no table site in any game, only the string copies, from a game's next start, so a
  console run can tell the two apart; `kernel.txt`'s `text source N` is then 256 more than the mode
  (257 for `auto`).

### Drawing the region

Square dims the game and shows a cursor in the middle of the screen. Move it
with the left stick, which speeds up while you hold it, or one pixel per press
with the d-pad (held, the d-pad repeats). Press Enter at one corner of the text
box, then move to the opposite corner: the box follows the cursor, and the line
at the top shows its size in screen pixels. Enter saves it as this game's
region. A box smaller than 32 × 32 pixels, the capture's minimum, is not saved.
Cancel takes the first corner back; with no first corner it leaves without
saving. A touch drag still draws the box: the first corner is where the finger
lands, and after it lifts the stick or the d-pad can still move the opposite
corner before Enter. Hold Square for one second for the full screen. The cursor
and its acceleration are covered by host tests; how they feel on the console is
not tested yet.

A game's animated "wait" or "next" icon, or a picture moving in the box, can stay
inside the region: Rubi then judges the text by its lines' own pixels and reads
it once they hold still, or reads it anyway at the wait's limit (`ocr_settle_press_ms`
for a press, `ocr_settle_max_ms` in the background;
[text stability](text-stability.md#animated-icons-and-moving-pictures-353)). If
the text still does not show after a few seconds, a region drawn around the text
alone, leaving the icon out, is read at once.

#### The box Rubi suggests

A game with no box of its own (no line for it in `region.ini`, not even a full screen saved with Square held) is
read on its whole screen, which reads the picture, the buttons and the menus as well as the dialogue. So the first
time the overlay is open over that game's text, Rubi offers a box: the region mode opens by itself with the box drawn
around the screen's dialogue and asks **Use this box?** Enter saves it as this game's region, as a box drawn with
Square is: the overlay closes, and from the next L + R the game reads the box only. The stick and the d-pad move its
bottom right corner first (Enter then saves the box as it is); Cancel takes its corner back to draw your own, and
Cancel again leaves the region mode, the game still on its whole screen. The question comes once each time the game
starts or resumes (not again after Cancel, nor after a box drawn with Square), and never once the game has a box
(any box, or the full screen with Square held). A box drawn with Square asks **Use this box?** too.

The box is the screen's width, from just above the dialogue's first line to the screen's bottom, so the longer and
the extra lines of the screens after it fit too. The game worker finds the dialogue among the lines it read on the
whole screen: the lines with Japanese in them at a dialogue line's height, grouped into blocks of lines that follow
each other closely, the block with the most Japanese, from the last screen read. On the hooked pages of 42 games,
read on a computer, the box holds 96.6 % of the dialogue lines of the same game's later screens (the half of the
games the rule was not chosen on) and reads a quarter of the whole screen's other lines. Where no block looks like
dialogue (a logo, a picture), nothing is offered; a menu or a choice list of stacked items can be, a band from its
first item down, so press Cancel there and wait for a screen of dialogue ([first-box.md](first-box.md): the rule, its
measurement and its limits). The Rubi app's **Suggest a text box** OFF (OCR & lookup page) writes `region = full` in
`region.ini`, the line for every game without its own, so that such a game reads its whole screen and is offered
nothing; ON takes the line out. An install older than a box for each game may hold one box for every game
(`region = x,y,w,h`): the app shows **Suggest a text box** OFF for it, and ON deletes that line. Not run on a console
yet.

## Per-game settings

The **Rubi** app shows five games per page, with compact titles, visible
IDs and ON/OFF badges. The last page may have fewer games. Games are listed by letter (#,
A–Z, kana by row, then other characters), alphabetically within it, capital
and small letters together. Tap the **Games**, **OCR & lookup**, **Anki**, **Look up**
or **Install** tab, or press Select, to change pages. Choices save automatically to
`ux0:data/VitaJPOverlay/games.ini` and take effect on game start/resume.
Icons load one per frame for the current page only, with at most five textures
kept. Changing pages releases the previous page's icons; a card
shows **JP** until its icon is loaded, or for good when the icon is missing or
unreadable ([details and memory](settings-readability.md#the-games-list)).

| Control | Action |
| --- | --- |
| Up / Down | Select a game (held: repeats) |
| Left stick up / down | As Up / Down, as in VitaShell: pushed past half way it moves one row, then, held, after a sixth of a second 60 rows a second, at whatever rate the app draws; how far it is pushed beyond that does not change the speed. Also on the OCR & lookup and Anki pages, where it stops at the first and last card while Up / Down wrap round |
| L / R | Page up / page down; the selection keeps its row (held: repeats) |
| L + R together, or touch the search field | Open the Vita keyboard and search all indexed games by title or title ID. Latin matching ignores case; Japanese text is matched as entered. The Enabled/Disabled filter still applies. Cancel keeps the current search; accept an empty query or touch Clear to show all games again |
| Touch Prev / Next | Change the five-game page; the page number is shown between them |
| Left / Right | Left: first game of this letter, or of the previous one when already there; Right: first game of the next letter. Letters: # (digits, symbols), A–Z, kana by row (か for が, ア for ヴ), others by first character |
| Enter or touch a game | Toggle OCR and overlay input for that game, as the button or the finger is let go |
| Hold Enter, or a touch, on a game for half a second | Open [the game's own page](#a-games-own-page): its ON/OFF and its **Line finder** (the spotter, or Rubi's finder with its fixes). The selected game's card says "Hold ×: settings" (○ on a console whose Enter is ○) |
| Triangle or touch the default row | Default ON or OFF. OFF turns every game OFF, including those turned ON one by one; ON turns every game ON except those turned OFF one by one. The status line names them ([details](settings-readability.md#games-missing-from-the-list-and-default-off-138)) |
| Square | Filter all, enabled or disabled games |
| Start | Refresh the installed-title list (the only time it is read, with the app's start) |
| Select or touch a page tab | Switch between the Games, OCR & lookup, [Anki](anki.md#setup-and-use), [Look up](#look-up) and Install pages |
| Cancel | Exit |

On the **OCR & lookup** page, select a row with Up/Down. Enter
or touch a row changes its choice: the Model size, **Read text**, the **Speed boost while
reading (444 MHz)**, Japanese-only selection, whole-word/character highlighting, how many
dictionaries are shown, the **Text source** ([native text hooks](#native-text-hooks)), (API 43) the two
switches that skip OCR where it has nothing to read: **Trusted hooks replace OCR** (`hook_trust`, ON by default)
and **Skip OCR on screens without text** (`ocr_text_gate`, OFF by default), applied when the overlay next opens,
**Vertical text** (`ocr_vertical`: [vertical text](vita-vn-ocr.md#vertical-text-vita-vn-ocr-99)),
**Suggest a text box** (ON by default: [the box Rubi suggests](#the-box-rubi-suggests); kept in
`region.ini`, so it applies when a game next starts or resumes), and **Pause the game to read** (`ocr_pause`,
OFF by default, the last card: [pause](game-pause.md)).
Touch a size, a Read text choice, a number, a Text source or a Vertical text choice to choose it directly;
Left/Right choose the next smaller or larger size on the Model size row (Enter goes round them),
Automatically/Only when I press on Read text, OFF/ON on the Speed boost, selection, two skip, Suggest and pause cards,
Whole word/Character on the Highlight card, 1–8 on Dictionaries shown, Automatic/Every
game/OCR only on Text source (`text_source`: its save says it applies when a game next starts,
as a game's worker reads it then), or OFF/Automatic/ON on Vertical text (`ocr_vertical`, from the
overlay's next open: Automatic, the default, reads columns where the text box holds vertical text
and rows elsewhere; ON reads columns, but rows where the box plainly holds lines of text). The page is taller
than the screen: it scrolls a card at a time to keep the selected one whole, the next card shows
at the bottom, a bar at the right shows where the view is, and a touch on a card cut at the
bottom only scrolls it into view. Read text (`ocr_mode`, [Use in a game](#use-in-a-game)) reads
settled text in the background (**Automatically (when it changes)**, the default: the popup is
ready when you press) or only once the overlay opens (**Only when I press L + R**, named after
`toggle_button`: nothing runs until you press, and the press waits for the reading); the Speed
boost (`ocr_boost_clock`, ON by default) runs the ARM clock at 444 MHz only while text is read
([docs/auto-vs-press.md](auto-vs-press.md)). Each size button shows its estimated time for a 3-line
textbox and the game memory a job of it takes (`0.8 s, 1.4 MB` for Normal), from
the models' pinned metadata. The
card's last line names the size and its model file (`Normal: default, balanced.
VOCR6-N found.`), or reports that file missing; it never says that a game ran a
smaller size. Choices save automatically to `config.ini` and apply when the overlay next opens.
Model files remain stored. Start reloads
the saved preferences. Other config values, including dictionary priority,
paths and capture mode, are preserved; only the lines of removed features are
rewritten (the [removed online dictionaries](#settings-and-dictionaries)'
API keys go, `dictionary = jpdb`, `jiten` or `hachidori` becomes `local`, and the
removed engine choice's `ocr_backend` line goes).

On the **Anki** page the rows are **Card mining**, **Screenshots** and **Trim black bars** (ON/OFF:
Enter, Left/Right or a touch; `anki_mining`, `anki_screenshot`, `anki_screenshot_trim_bars`), then the
exported cards' **Card type** (Word, Sentence, Both or Click: Enter takes the next, Left/Right the one beside it,
a touch the one touched), **Game tag** and **Sentence readings** (ON/OFF), **Duplicates** (Tag, Skip or Keep: as
Card type: [duplicates](anki.md#duplicates)), all four the Rubi app's own, in `app.ini`, which the overlay never
reads (`anki_card_type`, `anki_game_tag`, `anki_sentence_readings`, `anki_duplicates`;
[card type, game tag and sentence readings](anki.md#card-type-game-tag-and-sentence-readings)), **Known words**
(Import: `known_words.txt`, which `tools/known_words.py` writes from Anki; [known words](anki.md#known-words)), then
**Export cards** and **Export all cards**. Ten rows are more than the room above the page's status
box: they scroll a row at a time as the OCR page's cards do, a bar at the right shows where the view is,
and a touch on the row cut at the bottom only scrolls it into view ([Anki](anki.md#setup-and-use)).

Japanese-only selection (**Japanese characters only**) is **OFF by default** (the
owner's choice, 2026-10-20; ON before): every visible character is selectable, the
initial cursor is the text's first character, and the arrows skip spaces and line
breaks. With it ON, the initial cursor and the arrows skip Latin text, digits,
punctuation and Hangul while selecting kana and Han characters; it does not
tokenize words or hide the recognized sentence, and a text without Japanese
characters means no character lookup or dictionary allocation. An update keeps
`config.ini`'s own `japanese_only_selection` line, which the template wrote as
`on` from 0.7.6: an install updated from an earlier build stays ON until it is
turned OFF here.

**Whole word** highlighting is the default. It highlights the actual dictionary
match, including the original inflected text, when lookup finishes. **Character**
highlights only the selected character. A pending or unsuccessful word lookup
keeps the character highlighted. In Whole word mode, Right advances past the
matched source span and Left retraces up to 16 recent forward selections. With
no match or previous selection, movement falls back to one character.
Character mode always moves one character; Japanese-only applies to both modes.
The left stick moves one character in both modes (below).
The arrows follow the header as it is drawn: Left/Right go through the text in
reading order, across its displayed rows (Right from a row's last word or
character goes to the next row's first; Left from a row's first goes to the
row above's last character, or back to the word a Right came from), and stop at
the text's first and last selectable character. A text on one row is the one
that goes round its ends, as #81 asked: Right from its last goes to its first,
Left from its first to its last, and Left retraces such a wrap like any other
Right. Up/Down go to the nearest row above or below
with a selectable character, a soft-wrapped part of a long line included, to
the character nearest the same horizontal position, which repeated Up/Down
keep across a shorter row. The rows and positions are the header's own
laid-out lines and character starts, as for taps
([row navigation](touch-lookup.md#rows-and-arrow-keys)).
The left stick crosses a long line faster (#138), a character at a time where
the d-pad goes a word at a time (the owner, 2026-10-20): on 昨日すき焼きを食べました,
from 食べました the stick's right goes to べ, then to ま, whose lookup is ました,
where the d-pad's Right goes past the whole word. Each arrow press looks its
word up at once: 35-112 ms a step in the 0.8.0-test.3 console run (median 56 ms, its
31 cursor lookups in `log.txt`), and in Whole word mode Right skips a word only
once that word's lookup is shown. The stick moves in the arrows' four
directions: held, it steps through the text in reading order across rows, a
character a step whatever word is shown (or a row up or down, as Up/Down), one
step at once, the next after a sixth of a second as in the Rubi app's lists,
then each sooner, up to 20 steps a second (the cap, chosen so the cursor does
not overshoot: an estimate, untested on a console); it stops at the text's
first and last character, a one-row text's too, instead of going round. Its
left steps a character back and never retraces a Right: the d-pad's Left
retraces the d-pad's Rights and the stick's alike, and from inside a word the
d-pad skipped it goes back to that word's start. Japanese-only applies to the
stick as to the arrows and taps: ON, it lands only on kana and kanji; OFF, on
every visible character, punctuation and Latin included, never a space or a
line break. Nothing is looked up while it moves: once the cursor has rested for
`stick_lookup_ms` (`config.ini`, 200 ms by default, 0-1000), or has been held
against the text's end that long, the word starting at its character is looked
up (the longest match there, as for a tap) and, in Whole word mode, highlighted
as that match; back on the word shown before that, nothing is looked up again.
The d-pad and taps still look up at once.
The right stick scrolls a long definition (before, either stick did). Host
tests cover the timing, the cap and the deferred lookup
([left stick](touch-lookup.md#the-left-stick-138)); how it feels on a
console is not tested yet.
Tap a character in the recognized sentence at the top of the open overlay to
look up the word starting there; Japanese-only selection also applies to taps.
Kana text shows the kana word (から, する) and inflected text its dictionary
form (した → する); a short built-in list gives very frequent kanji their common
reading (私 → わたし). Each lookup writes one `log.txt` line with the text at the
cursor, the entry, its length, the dictionary, file reads and time
([word lookup on recognized text](touch-lookup.md#word-lookup-on-recognized-text)).
The recognized lines are one running text: where the game wrapped a word over
two lines (食べ / るかな), the word is looked up whole from its first part and
highlighted on both lines, Right skips it as one word, and an Anki card's
Sentence has the lines joined there; a speaker's name line, a new sentence or a
line the game broke short keeps its own line. In the real line sets about one
line in six that has a next line in its box ends inside a word
([words split over two lines](touch-lookup.md#words-split-over-two-lines)).
Where the overlay wraps text itself (the recognized text, the popup, the
subtitle strip, the Rubi app's rows) it breaks a line between phrases, where
kana gives way to kanji or katakana, at spaces and at punctuation, never inside a
run of kanji, katakana or kana, and keeps Japanese line-breaking rules: no line
starts with 、。」ー っ, none ends with 「. It is no dictionary walk, so a compound
can still break at its seam (女の|子)
([the overlay's own line breaks](touch-lookup.md#the-overlays-own-line-breaks)).
How these look on a console is not tested yet.
[Touch selection details and validation](touch-lookup.md).

Disable games such as Uncharted when you need their normal L+R controls.
Disabled games have no overlay capture or input filtering. A game with its own
ON or OFF in `games.ini` is listed even when the app does not find its folder
(as with CLANNAD in #138, a game card it cannot read, or a deleted game). It is
listed under its update's title or its ID, with `(folder not found)`, so its
choice can still be changed. Each Start (and the app's start) writes
what each folder held to `log.txt` ([why](settings-readability.md#games-missing-from-the-list-and-default-off-138)). An unreadable or
corrupt policy fails closed and reports the problem rather than enabling every
game. A game turned ON gets OCR at its next start: the kernel plugin loads the
OCR worker into it as it starts ([Install](#install)).

PSP games have a `PSP` tag; a Vita game with a native text hook entry has `Hook` (Rubi's own) or
`Luna hook` (a ported site in LunaTranslator's table: [native text hooks](#native-text-hooks)), a title
whose every site is unported none. Each Adrenaline bubble is listed under its game's
title, which the app reads from the `PARAM.SFO` of the ISO, CSO, ZSO or EBOOT.PBP
it boots. Its icon is the bubble's, which Adrenaline Bubble Manager made from
the game's. A bubble without its own choice follows the shared row
**Adrenaline / PSP games without a setting of their own** (`NPXS10028`), and so do
games started from inside Adrenaline (`PSPEMUCFW`, if it has no choice of its
own); with none there, the default applies. The kernel plugin loads the OCR
worker by the same rows, so turning a bubble or Adrenaline ON, like any game,
edits no taiHEN configuration and takes effect at its next start (an earlier
build's app appended a `*<its ID>` section for it; the Install page removes it).
Each bubble keeps its own
OCR region, and so does Adrenaline (`PSPEMUCFW`). A region drawn for PSP games before this release,
under `NPXS10028`, is used only when the Shell cannot tell which app started the
emulator, so draw it again once. The app writes what
it found for each bubble to `ux0:data/VitaJPOverlay/psp/<ID>.txt`, the only file
the Shell reads for it, and logs it to `log.txt`. Reading the bubbles makes the
app's start and Start slower by a few small file reads per bubble; how much with
many bubbles is not measured on a console yet. A folder in `ux0:app` that is not
named like a title ID (nine capital letters and digits) is never taken for a
bubble, whatever it holds.

### A game's own page

Hold Enter, or a touch, on a game in the list for half a second: its page opens, with its title and ID and two
settings (#333). A shorter press or tap still turns the game ON or OFF, as it is let go; a finger that slides off the
game's card first does neither.

- **Rubi in this game**: its ON or OFF, as on the list.
- **Line finder**: what cuts this game's text box into lines, `config.ini`'s `ocr_finder` and `ocr_line_fix` for this
  game alone. **Default** (the start) uses `config.ini`'s: the text spotter unless you changed it. **Spotter** is
  vita-vn-ocr's text spotter ([the spotter finder](vita-vn-ocr.md#the-spotter-finder-api-54)). **Off**, **On**,
  **Junk** and **All** are Rubi's own finder (`ocr_finder = grad`) with that `ocr_line_fix`
  ([line finder fixes](vita-vn-ocr.md#line-finder-fixes-api-50)). The card says what the chosen one does, or what
  Default gives this game.

Up/Down select a setting; Left/Right or Enter choose (Enter goes round the line finder's choices); touch a choice to
take it; Cancel goes back to the list, and Select or a tab to another page. A choice is saved at once and applies
when the game next starts or resumes, as its ON/OFF does; the list then shows `Line finder: On` (for example) on the
game's card. With the app's red banner up (the installed overlay is not the app's build), the page says that an
overlay from before it ignores its line finder.

Which to choose: keep **Default** unless a game reads badly. The spotter is the measured best overall, and on Narcissu
too, whose rows touch. On the owner's hooked pages (host, measured: VOCR5-N, the 1 MB LM and the cascade, regions drawn
with a margin; counted from the per-page records of the runs behind vita-vn-ocr #129 and #149, the spotter as #149
chose it, before its weights were trained again with these pages), Narcissu (`ULJM05674`) read with 63 errors in 4,959
characters with the spotter, 137 with All, and 677 with On. Over the 46 hooked games the spotter made 2,135 errors in
162,471 characters (1.31%), All 2,445 (1.50%), Junk 3,083 (1.90%), On 5,349 (3.29%) and Off 6,296 (3.88%); on the 92
other games of #149's development set 251 errors in 25,455 characters (0.99%), against All's 1.52% and On's 2.38%.
Rubi's finder with All made fewer errors than the spotter on 17 of the 46 hooked games and 23 of the 92 (On on 16 and
22), by up to 35 errors a game, but on 12 of those 23 by only one or two. These per-game counts are an upper bound on
what a choice gains: each game was scored on the pages it was compared on, and a small difference may not hold on
other pages. So a player could give a game that reads worse with the spotter Rubi's finder: **All** with a box drawn
a few pixels outside the text, or **On** in a box drawn tight (Junk and All can drop a real line there). Rubi has no
list of games that get another value: a game gets one only when you choose it.

PSP games follow the same rows as their ON/OFF. A bubble's own choice applies to it; without one it takes the line
finder of the shared row **Adrenaline / PSP games without a setting of their own** (`NPXS10028`), and without that,
`config.ini`'s. A PSP game started from inside Adrenaline is Adrenaline's (`PSPEMUCFW`): Rubi does not know which
game it is, so give the game a bubble for a choice of its own.

The choices are kept in `ux0:data/VitaJPOverlay/ocr_line_fix.ini`, one line a game in `games.ini`'s form
(`PCSG00001 = on`; the values are `spotter`, `off`, `on`, `junk` and `all`, in lower case), with a backup
(`ocr_line_fix.ini.bak`) as `games.ini` has. The Shell reads `ocr_line_fix.ini` alone: after a save that was cut
short (the Vita turned off during it) left only the backup, games use `config.ini`'s line finder until the Rubi app
next starts, which puts the file back from the backup. The choices are not in `games.ini` itself: earlier builds
read that file strictly, and a line they do not know would make it unreadable to them (its backup's choices, or
every game OFF). A file the app cannot read is left as it is, beside its backup if it has one, and a game's page says
so; the Shell then uses `config.ini`'s line finder for every game. One line it cannot read makes the whole file so: a
value in capitals, a byte-order mark, a comment after a value, a line that is not one of the five values. The game's
start line in `log.txt` names what its jobs use: `game PCSG00001 started: trigger l+r, subtitles select+r, ocr_mode
auto, ocr_finder grad, ocr_line_fix on` (with the spotter, `ocr_line_fix` is its fallback's). Not run on a console
yet.

## Settings and dictionaries

`ux0:data/VitaJPOverlay/config.ini` is reloaded when the overlay opens. A line
the plugins do not know or cannot use is listed in red in the open overlay and
logged in `log.txt` once, and again only when the set of such lines changes
(0.8.0-test.2's log had 76 lines from 8 of them, #101). A warning names the setting, or
a line without `=` by its number, and never quotes a value, which can be a key or
an address. A removed feature's setting (any `ocr_backend` line: VOCR is the
only engine; `log_host`; the online dictionaries' and AnkiConnect's settings) is
ignored without a warning, and the app's next save and the installer delete or
rewrite its line. The installer, the package's own copy and the Rubi
app write the file; the Shell plugin never does, and with no file it runs on
these defaults:

```ini
dictionary = local
local_dictionary_dir = ux0:data/VitaJPOverlay/dictionaries
local_dictionaries = plaintext.vjdict,vndb-names.vjdict
local_dictionary_results = 2
text_source = auto
ocr_model_dir = ux0:data/VitaJPOverlay/ocr
vocr_tier = normal
ocr_mode = auto
japanese_only_selection = off
log_file = on
```

`vocr_tier` is the local engine's model size, the Rubi app's Model size:
`small` runs VOCR6-S (819,792 B of int8 weights: FL10's topology with 4,987 classes, a plain blob), `normal` (the
default) VOCR6-N (1,189,408 B: H15's topology with 5,631 classes, with a head gate and a two-level head) and `large`
VOCR6-L (2,475,680 B: a late-wide shape, 5,631 classes, with a two-level head), vita-vn-ocr's VOCR6 recognizers (its
v0.4.1-vocr6.2 pre-release: VOCR5-S's, VOCR5-N's and VOCR5-L's shapes fine-tuned again on the owner's hooked PS Vita
and PSP games; selection and scores in that repository's `docs/vocr6.md`, "The follow-up", and `docs/vocr5.md`,
`docs/headgate.md` and `docs/head2level.md`). Each is inside the owner's weights ceiling for its size (Small < 1 MB,
Normal <= 1.6 MB, Large <= 2.5 MB). Their estimated times for a 3-line textbox are 0.6 s (Small), 0.8 s (Normal) and
1.9 s (Large) on one core at 333 MHz with the pinned runtime (no credit for its weight prefetch, with Normal's head
gate and two-level head and Large's two-level head, `ocr_head_gate = on`: 0.6, 0.9 and 2.1 s with `off`; Small has
neither), calibrated to VOCR3-H15's measured 4.94 ms per CTC frame on the console (v0.8.0-test.1); at 444 MHz, the
clock a job raises by default, 0.5, 0.6 and 1.5 s, and less again with lines read in parallel ([OCR
speed](ocr-speed.md)). Small is about 0.1 s a box slower than 0.9.8's VOCR5-S, which had a head gate. On a console,
0.8.0-test.4's logs measured VOCR5-N at 2.61 ms per CTC frame (10 jobs) and VOCR5-L at 4.98-5.10 ms (8 jobs), at 444 MHz,
each a job's recognition time over its frames (`tools/analyse_selftest.py --collect`,
[#191](https://github.com/bee-san/Rubi_for_vita/issues/191)); VOCR6 has VOCR5's shapes, but no console has run it, and
the box times above stay estimates.
A job runs the chosen size when the
game's free memory has room for it beside the fixed 1 MiB reserve,
else the next smaller size, silently: `log.txt` has a line `tier large did not
fit in PCSG00415 (need N, free F); using normal`, and so does the `--collect`
summary, but nothing on screen. A size whose model file is missing or corrupt
is left out for that game the same way. An older `config.ini`'s `vocr_model`
becomes its size (the FL10 family Small, H15 Normal, F20 and W20 Large), and
`vocr_fallback_model` is no longer read; the app's next save and the installer
rewrite them. `install-vita.sh` keeps the chosen size on an update, and the app's Read text
(`ocr_mode`) and Speed boost (`ocr_boost_clock`) too. A memory
error names where the memory ran out: the game's memory for this region
(a tighter region helps) or for the smallest size's weights, or the Shell's
memory for results (restart the Vita). Local OCR
reads the screen in the game since API 19, into the game worker's own memory; the
kernel takes memory for it only on the capture ladder's last rung, a strip of at most
64 KiB for one call ([in-game capture](ingame-capture.md)). An Anki card's screenshot
is read in the game too, in colour strips, and encoded there. API 24 removed the kernel's
full-frame capture buffer, which nothing used any more. A Shell UI memory error
after recognition keeps the recognized text and identifies the unavailable
dictionary results.

In the background, text that types out is read once it is whole: a background read recognizes nothing
until every row of the region has held still for `ocr_settle_ms` (150 ms by default, `0` turns it off),
waiting at most `ocr_settle_max_ms` (2000) per recognition, and a blinking "next" icon does not hold it up;
on the console it has read no dialogue line early ([text stability](text-stability.md)). A press reads at
once: opening the overlay says the text is ready (#413), so a line still typing is read as far as it has
typed (with **Automatically** the open overlay's refresh replaces it once the screen settles). The wait is
also what tells an animated icon from text, so once a press's read changed under every try, its next tries
wait as a background read does, and the press after a read reads at once again. A press right after a torn
read (a background read, a refresh, the subtitle strip's) waits from its first read, and keeps a background
read still running then rather than start its own ([text stability](text-stability.md)).
`ocr_settle_press_ms`, a press's limit before #413, changes nothing.
When the text changed under
every read of two jobs in a row (an animated icon in the region, or text still typing), the open overlay says so as
it tries again, and what helps where it can: a region drawn around just the text, if that leaves the moving thing
out (`ocr_wait_hint = off` keeps
"Recognizing…"; [why the overlay waits](text-stability.md#why-the-overlay-waits-358)). `ocr_skip_blank_lines = on`
skips found lines with no text-like ink; it is off by default, because once it
keeps soft text (a PSP game shown 2x, an upscaled render) it finds next to
nothing to skip in the test screenshots. Both are logged per job; see
[stable text and blank lines](vita-vn-ocr.md#stable-text-and-blank-lines-api-20)
for what each costs and what the host measurements show.
The recognized text is shown, saved and looked up in one form of each mark
(full-width `！` as `!`, `〜` as `~`, `...` as `…`, `ー` or `一` by context), the
form vita-vn-ocr's scorer uses, and without a line that is only a box's border
read as text (`ーニーーーーニーニー…`; `non_japanese_filter = none` keeps it); the
recognizer's own text is in `log.txt`
([text normalisation](vita-vn-ocr.md#text-normalisation-shown-and-looked-up)).
Each line is decoded with a beam search and a character language model
(`ocr_lm = 1000k` by default; `4000k`, `2000k`, `500k` or `off`). The LM is
vita-vn-ocr's, CC BY-SA 4.0. The game keeps the largest LM that fits its memory
resident, and reads greedily when none does. A line read with low confidence
is read again by the next model size up (VOCR6-L for Normal, VOCR6-N for
Small; Large has none), when its model fits too, whose reading is shown, while the job
stays within `ocr_cascade_ms` (2000; `0` turns it off). On the host's
development dialogue lines the two brought VOCR3-H15, the Normal size before
VOCR5, from 1.40 % to 0.73 % CER (with VOCR4-W20 as its cascade).
The greedy reading, each line's confidence and what the cascade read again are
logged beside the shown text
([beam + LM decoding](vita-vn-ocr.md#beam--lm-decoding-api-20),
[confidence cascade](vita-vn-ocr.md#confidence-cascade-api-20)).
In PSP games the line finder's boxes are also refined, cut back to their text
and split where two text rows were merged (`ocr_refine_lines = psp`; `on` for
every game, `off`). vita-vn-ocr #83 measured a dialogue gain with it on native
480×272 PSP frames; on native Vita text it was level or worse, and worse on
CLANNAD for five of six models. The overlay sees a PSP game as
Adrenaline shows it, 2× through a bilinear filter, where its accuracy is
untested and the host's box fit is level, so `psp` is a trial that a console run's logs
decide; none has reported it yet (0.8.0-test.3's PSP game got no OCR, and 0.8.0-test.4 started none).
Refined lines are read wider: on the
host, 12 % more recognizer frames in PSP regions shown 2×, an estimated
22-23 ms more per line found with VOCR3-H15 on the console
([finder refinement](vita-vn-ocr.md#finder-refinement-api-20)).
`ocr_vertical = auto` (the default; the Rubi app's **Vertical text**) reads Japanese set in columns, right to
left, with vita-vn-ocr's vertical model, where the line finder's rows in the region look like vertical text, and
rows everywhere else, as `off` does; `on` reads columns, and rows only where they are plainly lines of horizontal
text or where the box holds no line at all (since API 53, #379: before, columns everywhere; a box at least as tall as
wide is always read as columns, since #385's audit at once). Draw the text box around the columns. On
the host it read a vertical PS Vita VN's dialogue at 4.5-23 % CER, depending on how the box is drawn, and changed
no horizontal job's text ([vertical text](vita-vn-ocr.md#vertical-text-vita-vn-ocr-99); not run on a console).
`ocr_pause = on` (the Rubi app's **Pause the game to read**; off by default) copies each reading's text box from one
frame at once, inside that frame's display call, which the game waits in for as long: an estimated 2.4-6.4 ms for a
dialogue box at 333 MHz, and 10.8-29.0 ms for the whole screen, the region before a text box is drawn (draw one with
□ first), at most 100 ms. The OCR reads that still copy, so a spinning icon in the box cannot stop the read
([pause](game-pause.md); not run on a console). In a PSP game (Adrenaline) it does nothing; while it is off in a Vita
game, the open overlay's hint for text that keeps changing names it.
`ocr_text_gate = on` (the Rubi app's **Skip OCR on screens without text**; off by default) puts vita-vn-ocr
#100's text-presence gate in front of the finder: a region it says holds no text (the game hid its text box, a CG
screen) returns no line, so nothing is recognized and no junk is read off the picture. It stays off because it also
stops a few real lines, mostly very short ones (measured on the host on 46 games' hooked pages: lines OCR would
have read, 0.004-0.17 % of all, by platform and region); it costs an estimated 0.1 s per job at the game's clock
([text-presence gate](vita-vn-ocr.md#text-presence-gate-api-26)). Those figures are with the frozen finder's
lines: the gate scores the lines the finder returns, and with `ocr_line_fix = on` (API 50) it scores the fixed
finder's, which has not been measured.
`ocr_head_gate = on`, the default, lets the recognizer skip its output layer on the frames vita-vn-ocr #117's
head gate calls blank, and compute its rare characters only on the frames that may need one, the two-level head,
where the size's model has them: Normal's has both (its gate skips about half the frames), Large's the two-level head
alone, Small's neither (vita-vn-ocr's rules kept no gate for it, so the setting changes nothing for Small). That is an
estimated 0.14 / 0.19 s less per 3-line box at 333 MHz (Normal / Large), with no development score resolved worse;
nothing of it has run on a console yet. `gate` and `two_level` keep one of the two; `off` reads every frame with the
full head, as before API 42 ([head gate and two-level head](vita-vn-ocr.md#head-gate-and-two-level-head-api-42)).
`ocr_finder = spotter`, the default, finds the lines with vita-vn-ocr's text spotter (#149), a small network
trained on hooked game pages and on MeikiOCR's line boxes, instead of Rubi's finder (`grad`). On other games'
development screenshots (host, measured) it takes the whole-dialogue error of Rubi's finder from 2.38% to 1.03%
(0.99% with v0.4.0-vocr6.1's VOCR6; the three sizes' VOCR6 of v0.4.1-vocr6.2 has not been read through it), and on a
fresh set of 41 games from 4.29% to 1.86%. `ocr_spotter_refine = on` refines its
boxes too. Its weights are a file beside the models, kept in the game's memory between jobs; without them a job
reads with Rubi's finder, as does a region the spotter sees as vertical text under `ocr_vertical = auto`
([the spotter finder](vita-vn-ocr.md#the-spotter-finder-api-54)).

`ocr_line_fix` tunes Rubi's finder (`grad`, also the spotter's fallback): `ocr_line_fix = on`, the default, runs it
with vita-vn-ocr #129's column fix: a line's parts across a full-width space stay one line. On the owner's hooked PS Vita and PSP pages (host, measured), this takes the
whole-dialogue error from 3.88% to 3.29%, and on other games' development screenshots from 3.04% to 2.34%. It gains as
much on regions drawn tight.

- `junk` also stops box frames and ornaments from being read as lines. It is for regions drawn a few pixels outside
  the text, because in a tight region it can drop a real line.
- `all` is `junk` plus a split of two touching rows (Narcissu), so it is for regions drawn with a margin too.
- `off` is the frozen finder
  ([line finder fixes](vita-vn-ocr.md#line-finder-fixes-api-50)).
- A game can have its own line finder, which replaces `ocr_finder` and `ocr_line_fix` in its jobs: the **Line
  finder** on [its page in the Rubi app](#a-games-own-page) (#333), for a game that reads better with Rubi's finder
  than with the spotter.

Recognition runs up to three lines at once (`ocr_threads = 3`, the default; `1` reads one line
after another), each on its own thread at the lowest priority on the game's cores. Each line's LM
decode, the cascade's re-read and its place in the text still follow line order on the job's own
thread, so the text is the serial job's (except where the cascade's time ceiling decides: a job that
recognizes its lines sooner may read more doubtful ones again). A line whose pixels are unchanged
since an earlier job gets that job's text without being recognized again. `ocr_boost_clock = on`
(the default, the Rubi app's **Speed boost while reading**; `off` keeps the game's clock) raises the ARM clock to 444 MHz while a job recognizes and
puts it back after; a clock already at 444 MHz or more, such as PSVshell's 500, is left as it is, and
the bus and GPU clocks are never changed. The
log shows each job's helpers, its lines' share of a core, the clock, and the game's frame pacing
during the job against the time before it ([OCR speed](ocr-speed.md); estimates until a
console run).
The complete settings template is
[docs/config.example.ini](config.example.ini).

The release bundles the frozen disk-backed `plaintext.vjdict`, **Bee's
Ultimate Dictionary**: word definitions from **Daijirin4 → Sankoku8 → JMdict** in
that priority order, an English summary of each grammar point (は, に, ので,
～てしまう …), a line for each kanji with its most common meanings, and the
**Jiten global frequency rank** of every entry. It is built once in the private
[plaintext-dicts repository](https://github.com/bee-san/plaintext-dicts)
(the release `ultimate-v1`, commit `e9b6e76`) and not updated.
The public release includes the processed dictionary and its attribution.
The snapshots it is built from are pinned by exact commit and SHA-256; builds
perform no dictionary updates and need no conversion or current upstream
snapshot.

It also bundles `vndb-names.vjdict`, **VNDB Names — PSP & PS Vita**, with 30,709
searchable name entries for 12,151 characters across 1,202 qualifying visual
novels (snapshot cutoff 22 October 2026). Full names, name parts and aliases show
the original/romanized name, source game, platform and VNDB link; a name part of
one or two kana (the デ of ファナ=デ・ラ・…, the さん of 村上 さん) is no entry, as it
would match ordinary words. No images or
biographies are included; aliases may reveal names. Both dictionaries are
enabled on a fresh install. An update adds missing bundled files to the enabled
list while keeping custom dictionaries in order; if the list would exceed eight
files, it stays as it was and the install plan asks you to choose.

The main dictionary comes first for equal-length word matches; longer full names
can still match the names dictionary. **Dictionaries shown** is 2 by default, so a
word that is also a name (渚: a beach, and Nagisa) shows the word, then the name
below it; scroll down with the right stick. Another dictionary's entry is always the
same word: a name that only matches the start of a longer word (空 in 空港) is not
shown. An update
that enables the names (the Install page, `install_ftp.sh`) turns a count of 1
into 2. The names source ZIP is committed unchanged, converted offline
with `python3 tools/prepare_vndb.py`, and verified against input/output checksums
in [vndb-names.json](vndb-names.json). [Attribution and licences](vndb-attribution.txt)
ship in both release ZIPs and standalone VPKs (ODbL 1.0 / DbCL 1.0).

An entry shows its definition (or a grammar point's summary), a blank line, then
one line per kanji of its spelling, the kanji at the headword's size:

```
日本語 (にほんご) #4372
The language used by the Japanese people since ancient times …

日=day, counter for days, Japan, sun
本=real, true, origin, book
語=word, language
```

No entry names a source: there are no `[Daijirin4 Plain]`-style labels. The
dictionary names itself in its file, and that name, `Bee's Ultimate Dictionary`,
is what Anki cards and `log.txt` record and what the Rubi app's
**Dictionaries shown** card shows. A function word's summary replaces its
definition (は is "Topic particle, pronounced "wa" …"); a content word with a
grammatical use (上, こと, 無くなる) keeps its definition, with the summary on the
line before it. An entry longer than the popup holds is cut at the end of its
definition and marked `…`, and keeps its kanji lines and that summary line;
the right stick scrolls a long one ([popup](local-dictionaries.md#the-popup)).

Deduplication keeps a spelling and normalized reading pair from the first
source that contains it, preserving that source's separate senses, distinct
readings and inflection rules. The result has 486,529 entries (301,720
Daijirin4, 12,557 Sankoku8, 170,920 JMdict and 1,332 grammar points no word entry
had), 369,838 of them with kanji lines, and occupies about 150 MiB on disk,
904 KiB of it a key filter that answers most inflection guesses that are not
words without an index read ([key filter](local-dictionaries.md#key-filter)).
It stays on disk and is never loaded whole into RAM. Eleven definitions are
cut at the existing 7,000-byte limit; 16 empty or overlong source rows cannot
be indexed. Exact provenance and counts are in
[docs/plaintext-dictionaries.json](plaintext-dictionaries.json), with
[attribution](plaintext-attribution.txt) and the
[JMdict license](JMdict-LICENSE.html).

Every entry carries its rank in the [Jiten](https://jiten.moe) global
frequency list (CC BY-SA 4.0), or is explicitly unranked: 53.52% of the
entries are ranked, and a tap on 99.24% of the word occurrences in Jiten's
2,162 visual-novel scripts shows a ranked entry. The popup shows the rank after the
reading (`猫 (ねこ) #1536`; `frequency_badge = off` hides it), Anki cards record
it, and when the text ranks two entries alike the more frequent one is shown
([frequency ranks](local-dictionaries.md#frequency-ranks)). The rank
travels inside each entry on disk: no table of ranks is held in memory.

You can add your own Yomitan dictionaries with
[the converter and installation guide](local-dictionaries.md).
Up to eight dictionary files can be enabled in priority order. The Rubi
app's **Dictionaries shown** (`local_dictionary_results`, 1–8, default 2) pages
through files for the selected word. Bee's Ultimate Dictionary combines its
word, grammar and kanji sources in one file; the names are a second file. Only
the page on screen is held in memory. A character's name spelled as the text
(なぎさ) comes before the word spelled otherwise (渚), but not before one of the
1,023 most frequent words (わたし shows 私 first): `config.ini` `names_first`,
`auto` by default, `on` for names first always, `off` for the word first
([names and words](local-dictionaries.md#names-and-words)).

The subtitle strip (Select + R) shows the recognized text, without readings,
and adds no lookups for it. Recognition and dictionary lookup make no network
request and have no cloud fallback: the local dictionaries are the only ones.

### Removed features

Rubi is offline and has one OCR engine. What earlier versions had, and what becomes of a setting they saved:

- **Google Lens**, the [upstream's](https://github.com/Dartv/vita-jp-overlay) online engine: recognition never uses
  the network. A saved `ocr_backend` line is ignored, without a warning, and the Rubi app's next save and the
  installer delete it. The last build with Google Lens is the tag `archive/online-last` (branch `archive/online`).
- **The VOCR2 trial** (a separately trained FL10-sized model with a textbox finder, opt-in in 0.7.12 to v0.8.0-test.1;
  its promotion guard failed): a saved `ocr_backend = vocr2` loads as the local engine, and the app's next save and
  the installer delete the line; so does `ncnn`, an engine only the host tools had, since removed.
  `ux0:data/VitaJPOverlay/ocr/VOCR2_FL10_w8.vocr` can be deleted.
- **Meiki**, a manual single-line recognizer, removed from the source after 0.7.12: its MNN engine had to be copied
  into each game's own `app0:` folder through a per-title rePatch mount, and two observed jobs took about eight
  seconds each. A saved `ocr_backend = meiki` loads as VOCR, and the installer deletes the line. Files copied for it,
  `ux0:data/VitaJPOverlay/ocr/meiki-stream-int8.mnn` and any `ux0:rePatch/TITLEID/VitaJPOverlay/meiki/` folders, can
  be deleted. The [`0.7.12` release](https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.7.12) contains the three
  plugins, Settings, FL10/H15/VOCR2, JMdict and the optional Meiki engine; earlier releases remain available.
- **The online dictionaries** jpdb.io and jiten.moe, which needed an API key (the local Jitendex and every other
  Yomitan dictionary still work, converted as above): a saved `dictionary = jpdb` or `jiten` loads as `local`; the
  installer (`tools/migrate_config.py`) and the Rubi app's next save write `local` and delete the `jpdb_api_key`,
  `jiten_api_key` and older `api_key` lines (the app's save leaves none in `config.ini.bak` either; the installer
  names each removed key, never its value). The subtitle strip's furigana, which only those two services gave, went
  with them.
- **The Hachidori relay**, which looked words up in the Hachidori app on a computer on your local network (`dictionary
  = hachidori`, `hachidori_host`); the Hachidori app itself is unaffected. A saved `dictionary = hachidori` loads as
  `local`, and the Rubi app's next save and the installer write `local`; `hachidori_host` is ignored, and the
  installer drops it.

### Anki

Card mining is off by default (the test builds turn it on). With it on, Enter
saves cards as readable JSON files in `ux0:data/VitaJPOverlay/anki_queue/`, each
with a JPEG of the game's screen beside it (`anki_screenshot = on`, the default:
960 × 544 at quality 70, 55.6 KB for the median of 294 Vita screens, 46.6 KB of
304 PSP ones). The game worker reads the frame and encodes it in the game's
memory (a 450,560 B block while it runs), and the kernel writes it to the memory
card; SceShell holds none of it ([the screenshot](anki.md#the-screenshot)). With
`anki_screenshot_trim_bars = on`, the default, the JPEG leaves out the frame's black
letterbox or pillarbox bars (a PSP game shown smaller than the screen: at 1× a 484 × 276
picture), found as the rows are read, with no copy of the frame
([black bars](anki.md#black-bars)).
The cards survive restarts and need no network. Each record (version 4) keeps everything known at the
press: the word, its reading and furigana, the shown entry and its
dictionary's name, the other dictionaries' entries, the whole recognized text
with the word's offsets (as recognized; the exported Sentence joins the lines
the game wrapped), the game, its title ID and platform, the time, and the OCR
engine and model.

The Rubi app's **Anki** page has card mining ON/OFF, **Screenshots**
ON/OFF, **Trim black bars** ON/OFF, the cards' **Card type**, **Game tag** and **Sentence readings**
([card type, game tag and sentence readings](anki.md#card-type-game-tag-and-sentence-readings)), **Duplicates**
(Tag, Skip or Keep), **Known words** (Import), **Export cards (N queued, M already mined)** and **Export all cards**
(ten rows, which scroll a row at a time). An export writes the cards as
notes of [Lapis](https://github.com/donkuri/lapis) 1.7.0 (GPL-3.0, its
templates bundled unchanged), in the deck **Vita JP Overlay** (the name from before the rename,
kept so that Anki adds the cards to the same deck as before, not to a second one), into
`ux0:data/VitaJPOverlay/anki/<date>_<time>-<N>cards.apkg`; the page shows the
file's name. Copy it to a computer, by VitaShell's FTP or USB connection
(Select in VitaShell starts the one set in its settings) or, with an SD2Vita as
`ux0:`, on its card in a card reader, and open it in Anki. Cards leave the
queue only after the file is written, read back and checked, named and synced,
so a full memory card or a stop loses nothing. A note's GUID
comes from its card, so a card exported twice is one note in Anki, and a
profile that has Lapis keeps its own note type and styling. Nothing in the
overlay or the app uses the network for Anki. `anki_host` and
`anki_audio_url` (AnkiConnect, removed) are ignored, and the installer and the
app's next save remove them. See [docs/anki.md](anki.md) for the fields, the queue, the package
and memory.

### Look up

The Rubi app's **Look up** page (#313) looks words up beside the suspended game, with the overlay's own dictionaries
and lookup, and saves them as Anki cards: a word you type, or a word of an earlier line of a game. L or R, or a tap on
**Search** or **Earlier lines**, goes from one to the other; each keeps its text and cursor.

- **Search.** Triangle (or, before anything is typed, Enter or a tap on the text box) opens the Vita keyboard for a word or a sentence
  of up to 128 characters; every language's keyboard is offered, Japanese among them, whatever the system's
  keyboards are. The text is looked up as the overlay looks up its text: the word at the cursor, longest match first,
  in its dictionary form (した → する), in `local_dictionaries`' order with the same ranking, the other dictionaries'
  entries for the word below it (`local_dictionary_results`). The entry shows its headword, reading, frequency
  badge (`frequency_badge`), the conjugation line, the definitions, and the kanji and grammar lines.
- **Earlier lines.** The most recent game's last 200 lines in `history.csv`, newest first, each with its local
  time; Square, or a tap on the game's name, goes to the next game. The lines are those the overlay's line history
  shows: Read text: Automatically's ([OCR history](ocr-history.md)), filtered as the overlay shows a text
  (`non_japanese_filter`), none the same as the one after it. Enter, or a tap, opens a line with the same word
  cursor as Search; Cancel goes back to the list. A big file is read from its end back, a part of it each frame
  (at most its last 8 MiB): the list grows while the status line says it is reading. Start reads the file again.
- **A card.** Enter on an entry saves it to the Anki queue, as the overlay's Enter does, **without a picture** (the
  screen of that moment is gone), whether Card mining is ON or OFF: it is the page's own explicit press. The
  sentence is the line, or the typed text when it is longer than the word (a word typed alone has none); the game,
  title ID and platform are the line's (a typed search has none); MiscInfo is the game and the line's local date (a
  typed search: the date it is saved). **Export cards** on the Anki page exports them with the others
  ([the cards](anki.md#cards-from-the-look-up-page)).

| Control | Action |
| --- | --- |
| Triangle | Search: type a word or a sentence |
| Left / Right | Move the cursor by the matched word (a character with **Highlight: Character**); held, it repeats and stops at the text's ends |
| Left stick | Left/right: a character a step, the word looked up once the cursor rests (`stick_lookup_ms`); up/down: scroll the entry |
| Touch the text | Look up the word that starts at the character touched |
| Up / Down, right stick | Scroll the entry (in the list: choose a line; Left/Right a page of lines) |
| Enter | Save the entry as a card (Search with no text yet: the keyboard; the list: open the line) |
| Square | Earlier lines: the next game |
| Cancel | An open line: back to the list; else exit the app |

The page uses no network, and its own memory is static: about 430 KiB of the app's 50 MiB
([vita-memory.md](vita-memory.md#54-the-rubi-app-a-system-application)); leaving the page closes the dictionaries'
files. The many characters it draws and measures make the app load its font again now and then, with an empty glyph
atlas (libvita2d's holds about 500, an estimate), each time leaving 4 KiB of its heap behind. Earlier lines lists at
most 32 games. The Shell, the game worker and the kernel are unchanged. Not run on a console yet.

## Memory and troubleshooting

If Adrenaline or a PSP bubble immediately returns to LiveArea, its Shell
bridge may have failed to fit in SceShell's memory. The release ZIP includes
an optional [small Adrenaline bridge](../compat/adrenaline_vsh/README.md), with
backup, replacement and recovery instructions. It restored a tested bubble's
startup on a console on 2026-10-17; it does not require a taiHEN config edit.

The game OCR worker runs the local OCR engine in the enabled game's process, allocating its
region-sized workspace from that game's `USER_RW`. The kernel plugin loads it into each game
that is ON in `games.ini` as the game starts, at the point where taiHEN loads a game's own
plugins, never into SceShell, a system app, VitaShell or the Rubi app, and a game keeps one
copy ([worker loading](worker-loading.md)). It still checks the title at startup and
unloads itself at once (`SCE_KERNEL_START_NO_RESIDENT`) in SceShell, VitaShell, the settings
app and other system titles, should a configuration line load it there.
Candidate games keep only an idle worker, at the lowest user priority so every
game thread preempts a recognition, until the Shell confirms their per-game
policy; disabled games load no weights and allocate no OCR workspace. Each game that is ON
holds the worker's 212,992 B of module pages (221,184 B in the debug build; API 31's native hooks and
their job path added 61,440 B and 65,536 B, the debug build's PSP probe 4,096 B more, [native-hooks.md](native-hooks.md#memory),
API 34's remembered hooks 8,192 B, vita-vn-ocr #109's counter re-decode 4,096 B, API 42's head gate and
two-level head 8,192 B, API 47's vertical text 4,096 B, API 50's line finder fixes with vita-vn-ocr f8a1856's
runtime 8,192 B, API 54's text spotter 16,384 B and API 55's pause for the frame 4,096 B, [memory-ledger.md](memory-ledger.md)) and its 36,864 B thread stack and TLS from its
start, homebrew included (ON by the default): an app that fails to start with them turns OFF
in the Rubi app, or starts with L held.
Games started from inside Adrenaline share its process (`PSPEMUCFW`); a game
started from its own bubble runs in a PSP emulator process of its own, under the
bubble's ID (observed for `PSPEMU004` on 2026-10-17). The worker is also loaded
into the bubble's own small starter app (an ordinary app under that ID) when that runs,
where it stays idle until the app exits.

The kernel transports bounded jobs and text. Game workers cannot
submit jobs or control input. A job is tied to its game, capture sequence and
foreground generation; suspend, game changes, cancellation and Shell restarts
invalidate old work. The screen is read in the game: the display hook writes
the region's rows, as 8-bit grey, a strip at a time (under 64 KiB at any
region) into a block of the game worker's own memory, where the recognizer
reads them; no copy of the region or of the frame exists, and the kernel pool
gives OCR nothing. If the game's `USER_RW` refuses that block the worker tries
CDRAM, then phycont, then a kernel strip of at most 64 KiB for one call, and
logs each attempt ([docs/ingame-capture.md](ingame-capture.md)). The
worker frees each job's workspace before returning text. The verified weights stay in their own `USER_RW` block between jobs
(819,792 B for VOCR6-S, 1,189,408 B for VOCR6-N and 2,475,680 B for VOCR6-L,
held as 823,296, 1,191,936 and 2,478,080 B page-rounded blocks; the worker takes
no larger one) and are read and
SHA-256 checked again only when the model file's size or modification time
changes, or the model changes. A job that finds no line (a title screen, a
box between two lines) reads them, and the LM and the cascade's model, if they
are not resident yet, so the first scan with text does not wait for them
([first-ocr.md](first-ocr.md)). They are released when a job's memory check
fails, when the file is missing or wrong, when the game has not been the active
one for 5 s of its running time (another app in front of it, title disabled; the
time stops while the game is suspended, so a suspend and resume keeps them) and
on exit. A job
then needs only its region-sized workspace. The worker leaves free USER memory
for the game beside what the job holds: a fixed 1 MiB for every model size,
including cascades and helper threads, and for an Anki screenshot (owner
2026-10-23, #280; #277's console test build carried it, and the owner reports
that it works, 2026-10-28; an estimate still, as no figures of that run were
reported). This replaces the untuned 4/8/16 MiB tier margins, which
blocked Small in あおかな despite 3 MiB free and only 996 KiB needed for OCR.
The worker checks the full page-rounded memory need plus this reserve before
allocating any block; allocation can still fail because the game or system uses memory
concurrently.

If OCR fails for lack of memory, the overlay shows the failed check's game RAM
figures in KiB: free memory, Rubi's reclaimable resident memory, and the total
needed, broken down into workspace, models and the reserve left to the game.
These are the game worker's measurements at that check, not the Vita's total RAM
or a live reading after cleanup. Requirements include the reserve; Rubi does not
allocate it. If the memory query failed, the error shows its native code instead
of a free-memory figure. An error before the check has no memory figures.

Model files are opened by the existing kernel worker because some games cannot
access the `ux0:` storage namespace (`0x80010013`, ENODEV). The broker accepts
only these names in `ux0:data/VitaJPOverlay/ocr`:<!-- names from kernel/game_ocr.c, written by tools/docs_model_names.py -->
the recognizers `VOCR6.2_N_w8g2.vocr`, `VOCR6.2_S_w8.vocr` and
`VOCR6.2_L_w8_2l.vocr` (VOCR6, release v0.4.1-vocr6.2); `VOCR3_H15_w8.vocr` and
`VOCR3_FL10_w8.vocr` (VOCR3, release v0.2.5-vocr3.1); `FL10_w8.vocr`,
`H15_w8.vocr` and `F20_w8.vocr` (VOCR1, release v0.2.0); `VOCR4_W20_w8.vocr`
(VOCR4, release v0.2.5-vocr4.1); `VOCR6_V_w8.vocr` (VOCR6, release
v0.4.2-vertical6.1); and the character LMs `vocr_lm_o3_500k.vlm`,
`vocr_lm6_o3_1000k.vlm`, `vocr_lm_o3_2000k.vlm` and `vocr_lm6_o3_4000k.vlm`
(release v0.3.7-lm6.2)<!-- end of the names -->.
Since API 21 a job asks by model size: the broker opens its sizes' models and,
when the request sets a cascade ceiling, the next size up, each by its size,
never by a name or a path. A request by name (the host tools) opens only its own
model and fallback and, with a cascade ceiling, the cascade's model. Either opens
the LMs up to the tier its request allows. The broker checks their lengths,
reports their modification time, and copies at most 32 KiB per request into
game-owned memory. Each model call waits (at most 50 ms, without the kernel
lock) for the kernel worker instead of polling with a 1 ms sleep.
The game then checks SHA-256 and runs inference. A job is handed over the same
way: the game worker's take waits (at most 100 ms) for the Shell's submission
and the Shell's read (at most 50 ms) for the game's result, so neither waits
for the other's next poll (a 100 ms and a 50 ms one before: 75 ms a job on
average, est.). The kernel holds one 32 KiB
staging buffer, no complete model, and no saved game pointers. Cancellation or
process exit drains any file operation before the next job can reuse its slot.
The shared API 13+ error codes keep bit 30 clear. On the physical Vita, the
previous BUSY value `-3` arrived as `0xBFFFFFFD`, so the game failed to poll a
pending model operation and retained its job ownership. The new codes match
that return encoding and also remain consistent in copied status fields.
Native SDK file errors are unchanged. Install all three plugins of one build
together (API 24 since the kernel's full-frame capture was removed) and reboot; older plugin versions
cannot use this transport.

The `*main` plugin handles UI and disk-backed dictionary lookup with one
64 KiB result arena, taken after recognition, and no connection pool. Its module
holds 176,128 B of SceShell's partition (262,144 B, of 266,240 B declared, before Google
Lens's removal; 266,240 B before the default `config.ini` template left it, #101; 282,624 B
before AnkiConnect was removed, 286,720 B before the Hachidori relay client's removal,
294,912 B before the online dictionaries', of
299,008 B declared then; 405,504 B in 0.7.12's successor builds): the
overlay's text arena is a 20 KiB Paf allocation taken when the overlay opens and
returned when it closes, never a static buffer; if it is refused, the overlay
does not open that time and the game keeps its input. Local OCR-only/subtitle
jobs use a static 4 KiB filtered-text buffer and no dynamic dictionary arena.
The UI keeps its own 4 KiB text copy for Unicode character navigation. A lookup
reads only the selected position and shows zero or one entry. With Dictionaries
shown above 1 it also keeps a 208-byte table locating the other dictionaries'
entries in the same arena, and reads each one's payload after the best match's
in that arena; one that does not fit is left out, with those after it
([Dictionaries shown](local-dictionaries.md#dictionaries-shown)).
The previous local result is retired before it is reused. Paf allocations preserve a 1.5 MiB UI reserve. If a
local result allocation is refused after OCR succeeds, the recognized text
stays visible and only dictionary lookup backs off. Failed joins/frees keep
ownership for a safe stop retry.
With `anki_mining = on`, saving a card (Enter) takes one
bounded Paf allocation through the same reserve guard (128 KiB; a card's
screenshot is the game worker's, so it adds only its job's 5,404 B request and
result, and no kernel capture) and a 16 KiB thread stack; both
are released when the job is done. Counting the queue when the
overlay opens takes only the thread's stack. Anki failures, storage errors
and refused memory never touch the recognized text. Nothing in SceShell or
the Rubi app uses the network for Anki.
The kernel startup instruction and all three plugins' syscall imports are audited.

These are bounded allocations and host checks, not a measured physical Vita
peak or a guarantee that every full-screen region fits in every game.
[docs/vita-memory.md](vita-memory.md) is the reference for the Vita's memory
pools, which process each plugin runs in, and the budgets each context can use.
[docs/memory-ledger.md](memory-ledger.md) is the ledger of every byte the
three plugins take from those pools: a budget table checked in CI (an allocation
outside the wrappers, a worst case over a pool's limit, or a module that grew by a
page fails the build), printed in the build log and, with current and peak per
pool, in `status.txt` at boot and at each game start and exit and in `kernel.txt`
at boot.

`log_file = on`, the default, records bounded diagnostic logs:
`ux0:data/VitaJPOverlay/log.txt` holds at most 1 MB and one rotated
`log.old.txt`, each line written and closed at once so a crash keeps every line
before it, except that a burst of lines formatted at once (a job's result, about
21 lines, its recognized text, the memory ledger's table) is written in one
append, as a file append costs the console 12-46 ms (#81's self-test timestamps). The normal build
holds a job's few lines until its result is taken (shown, or kept for the overlay's next open) and writes
them after half a second with no job, one append per file (a crash before that loses them: at most 24 lines;
[no append on the result path](release-variants.md#no-log-append-on-the-result-path)); the debug build holds a
lookup's lines, and an OCR job's after its recognition, until it has published its result, and writes them right
after it, one append per file ([#360](release-variants.md#the-debug-builds-job-lines-after-the-result-360)); `kernel.txt`
holds up to 256 KB per boot (the previous boot's is
`kernel.prev.txt`). `bash tools/install_ftp.sh --collect VITA_IP` fetches all of
it read-only into one timestamped folder and zip, with its audio source URL and
any API key an older build left in `config.ini` redacted, both taiHEN configurations, the Anki queue, crash dumps and the
installed files' hashes, and prints its analysis
([self-test](self-test.md#collecting-everything---collect)), which dates each crash dump against the boot and,
in a terminal, offers to archive the older ones off the Vita ([crash dumps](self-test.md#crash-dumps)); `--status VITA_IP`
fetches just the bootstrap logs. Recognition timings and memory attempts are in
`log.txt`. For a decisive console test,
arm the **self-test** with `bash tools/install_ftp.sh --self-test VITA_IP`, reboot,
start a game and reach a text screen: once a scan has read text in your saved
region, a background job of the self-test's own (your scans never wait for it;
the overlay or a new screen stops it until a later scan with text) runs
the smallest model size, the configured size, the full screen, the capture probe and
the configured size again, in your saved region, each stage with its own record,
and the report has every pool's free memory at each step by every query route;
`--status` fetches it and `python3 tools/analyse_selftest.py` prints a PASS/FAIL
row per stage (NOTEXT for a region stage that found no line: no proof of recognition)
and the diagnostics (probe, memory routes) as PASS/WARN, which never change
the OCR verdict. See [docs/self-test.md](self-test.md). Each game OCR job logs the
game's free USER memory against what the job needed (also when it refused the
job) and, for each recognized line, its size, CTC frames and recognition time.

A boot that does not complete (the Shell plugin not alive a minute after start,
or SceShell exiting with the plugin loaded) makes the next boot a **safe boot**:
the kernel plugin installs no hooks and the Shell and game plugins unload
themselves, so the Vita starts as if the overlay were not installed, and
`status.txt` says why. Reboot once to try again; after two failed boots in a
row safe mode stays until you install a new build (the installer resets the
record) or delete `ur0:data/VitaJPOverlay/boot.txt`. See
[docs/boot-safety.md](boot-safety.md). If the Vita does not boot at all,
hold L while turning it on (with h-encore or h-encore2: open its menu, holding R as
h-encore starts, and choose Exit while holding L), so that taiHEN loads no plugin; then
in VitaShell put `#` before the two `VitaJPOverlay` lines of `ur0:tai/config.txt` (and
`ux0:tai/config.txt`), or open the Rubi app and choose **Uninstall** on its Install
page, or from a computer run `bash tools/install_ftp.sh --uninstall VITA_IP`. Restart
normally. Uninstalling preserves data. A `config.txt.vjo-<date-time>.bak` is the
configuration from before that install, not before the overlay: an update replaces the
plugin files its lines name. If one game does not start, turn it OFF in the Rubi app,
or hold L while starting it ([recovery](one-vpk-install.md#recovery-r1)); if it starts but misbehaves
since its native text hooks, set Text source to OCR only ([native text hooks](#native-text-hooks)).

The Rubi app is not a plugin and never starts by itself, so it cannot stop
the Vita from starting, and its Install page writes nothing until you choose an
action. If it misbehaves, close it from its LiveArea page. To go back to an older
build, install that release's VPK and choose Install on its Install page (it names the
version it goes back to); deleting the app leaves the overlay as it is, and the
plugins keep reading `games.ini` and `config.ini` without it. With no app installed the overlay says
nothing about it; an older one than the plugins' is said once a boot, at the first
overlay opened.

## Build and validation

The Vita build stamps the Rubi app: `tools/app_stamp.py` writes `vjo_app.txt`
(`vjoapp1 api=API build=` the first 16 hex digits of its eboot.bin's SHA-256)
into the VPK and, the same bytes, into the release's `ux0/data/VitaJPOverlay/`
as the record of the app these plugins come with ([core/app_stamp.h](../core/app_stamp.h)).
`package_release.py` refuses a base zip whose two copies differ and writes the
stamp and the eboot's SHA-256 into the manifest (`app`). `--check-app` compares the
Vita's app with the package's stamp file and its VPK's `eboot.bin`, `--collect`'s
analysis with the record and the manifest's `app`.

The Vita build checks the artwork inside `Rubi.vpk` before it can
ship. Bubble and LiveArea PNGs must use opaque, non-interlaced, 8-bit indexed
color with no text chunks (`tEXt`, `iTXt`, `zTXt`), the bubble icon may be at most
32 KiB and the start-up image at most 600 KiB; RGBA PNGs can fail installation
with `0x8010113D`. It also checks that the app's `eboot.bin` is the system
application it is built as (`tools/check_settings_self.py`: the SELF's attribute
and 50 MiB budget, the main thread's CPU3 and priority, the heap, every thread
created on CPU3). The Rubi app's mascot was drawn by a pinned, Apache-2.0
text-to-image model; [settings/assets/ICON-SOURCE.md](../settings/assets/ICON-SOURCE.md)
records the model, checksums, prompt and seed. To recreate the committed artwork
from that image, install `tools/requirements-assets.txt` and run
`python3 tools/make_settings_assets.py`; it writes each image with at most 256
colours and checks that the palette PNG decodes to exactly that art.

The default build enables vocr. Its runtime is external to this repository and
checksum-pinned; see [the build guide](vita-vn-ocr.md#build). Set up VitaSDK/Paf with
`bash tools/setup_toolchain.sh`; standalone-app dependencies are hash-pinned.
Python and Pillow are needed for host image tests.

```sh
cmake -S . -B build/host -G Ninja -DVJO_VOCR_SOURCE_DIR=/path/to/vita-vn-ocr
cmake --build build/host
ctest --test-dir build/host --output-on-failure
cmake -S vita -B build/vita -DCMAKE_BUILD_TYPE=Release -DVJO_VOCR_SOURCE_DIR=/path/to/vita-vn-ocr \
  -DVJO_PLAINTEXT_SOURCE=/path/to/plaintext-dicts
cmake --build build/vita --target release
```

`bash tools/fast_gate.sh` runs all of CI's checks at once, in containers it builds once (docker or
podman; `tools/gate/`): both builds' host suites (`ctest -j`), the ARM lane, both Vita builds with their checks
and, when the package's files changed, the package e2e. Where it finds the pinned release models
(`VJO_VOCR_MODEL_DIR`, else its cache; `python3 tools/prepare_ocr.py --backend vocr-tests --output DIR` fetches
them), it also runs the host suites with them, which CI cannot (the `models` lane). It is the merge gate
([AGENTS.md](https://github.com/bee-san/Rubi_for_vita/blob/main/AGENTS.md#merge-gate-binding-owner-2026-10-26)); its comments list its options.
`ctest -j` is safe: each test has a `TMPDIR` of its own, and the tools' Python suites are one test
per module (`tools_<module>`).

The Vita build is Release, as in CI; `vita/` uses Release when no build type (or
an empty one) is given. A `Debug` build compiles libpng without optimisation, which fails the
Rubi app's [PNG ABI check](settings-png-runtime.md), so the app does not
build.

`-DVJO_DEBUG=OFF` builds the normal build, the release's main download, without
the debug build's log lines, self-test, timings and traces (12 KiB of SceShell
pages fewer on main `c08db18`); `ON`, the default, is the debug build, which the test builds are. The
`release_variants` target builds the other one too and, with
`-DVJO_RELEASE_MODELS=/path/to/models`, packages both as
`Rubi-<version>.zip` and `-debug.zip` with their `SHA256SUMS`; CI builds
and tests both on every pull request ([release-variants](release-variants.md)).

Check out the exact `plaintext-dicts` commit recorded in
`docs/plaintext-dictionaries.json` and point `VJO_PLAINTEXT_SOURCE` at that
checkout. CI uses a read-only deploy key and the same exact commit; a pull
request from a fork gets no secrets, so its CI leaves out the dictionary, the
tests on it and the release zip, and runs every other build and test. To prepare
the dictionary separately, run `python3 tools/prepare_plaintext.py --source
/path/to/plaintext-dicts -o plaintext.vjdict`; without `--source`, authenticated
GitHub CLI access can fetch the pinned archive. Both the archive and unpacked
file must match their committed checksums.

CI builds the Vita targets in the `vitasdk/vitasdk:2026.08` container, pinned by digest, and the
Rubi app's `eboot.bin` depends on the SDK's libraries: another VitaSDK gives
it other bytes. `bash tools/ci_container_build.sh --compare RELEASE.zip`
rebuilds a commit in that container, pinned by the digest CI pulled, under
docker or podman, and compares the result with a release
([how](offline-testing.md#cis-vita-build-in-its-container)).

The cross-build produces a base ZIP containing the frozen dictionary and
attribution. `tools/package_release.py BASE.zip -o RELEASE.zip --models
/path/to/models` adds the checksum-pinned OCR models and per-file manifest
(`tools/package_variants.py` does it for both builds' bases at once).
Packaging reads the dictionary already in the base ZIP, requires its exact
checksum, and never substitutes a newer JMdict-only snapshot. Publication
still happens explicitly after validation. `python3 tools/e2e_package.py RELEASE.zip` runs a
package's own installer and `tools/install_ftp.sh` against a simulated Vita (an FTP mock of its
partitions): an update over an older build, a fresh install, `--check-app`, `--self-test`,
`--collect` and `--uninstall`. First it checks the package against the checkout it runs from (the
package's own commit): the manifest's build and app version against all four modules, the worker
included, and the models against the pins, with nothing else in `ocr/`
([release-variants](release-variants.md#the-releases-packages)). CI runs it on both builds' zips of every pull request that changes
more than Markdown (a fork's pull request, without the private dictionary, has no zips).

The host suite covers configuration, disk-backed dictionary lookup,
deinflection, kernel/input ownership, per-game policies, and the actual Shell
worker with platform stubs. It checks cancelled jobs, denied input blocking,
OOM recovery, failed joins, 50 local OCR-only lifetimes, cursor coalescing,
close/reopen during lookup, scene/region invalidation, Unicode navigation and
100 game-worker
allocation/free cycles, bounded model reads and stale-job isolation. The Shell
Anki miner is tested with its offline queue on real files: saves, restarts,
Japanese text and escaping, failed and short writes, temp and damaged files,
every record version, other dictionaries' entries, duplicates, refused memory,
cancellation and 50 card lifetimes. The Rubi app's `.apkg` export runs on
the same queue storage, with failures injected at every write, flush, rename,
sync and read-back, and its packages are opened with Python's `zipfile` and
`sqlite3` and imported by Anki itself (the pinned `anki` package); crash/restart
tests stop each save and export step in a fresh process. Real
model tests also run the actual game worker through the actual kernel broker
with FL10/H15, unavailable game storage, short reads, corrupt weights and
failed cleanup. The integration tests model the observed syscall return
encoding and require a subsequent real capture request after each completed job.
Timeout and ownership tests run without release weights. Real
accuracy is measured separately against vita-vn-ocr's hand-checked shared
`eval/` screenshots.

[docs/offline-testing.md](offline-testing.md) says what of this runs where:
the real plugin sources with the SDK boundary stubbed on the host, the pure-C
kernel and core suites on the Vita's CPU under qemu-arm (`tools/arm_host_tests.sh`),
nothing in Vita3K (it has no SceShell, no kernel plugins and no memory budgets),
and the Shell budget replay model (`tools/shell_budget_model.py`) that ranks a
build's SceShell footprint against the console's measured samples. Boot and
memory on the console remain the self-test's to answer.

[docs/performance.md](performance.md) is the stage benchmark
(`tools/bench_stages.py`): every stage of an OCR job and of the Shell's lookups on
the real sources, timed on the host and counted as the Vita compiler builds them
on a Cortex-A9 under qemu-arm, with a model of one job's latency calibrated with
#81's console logs (estimates where the console measured nothing). Its smoke run
(ctest `stage_bench_smoke`) checks every stage's output against a recorded
checksum, so a speed change that alters one fails; the log stage is a replica of
the Shell's formatting, so for logging it checks the formatted bytes only.

Controller regressions exercise all 12 hooked read/peek variants, including
negative logic and 64-sample batches. The [pinned VitaSDK headers](https://github.com/vitasdk/vita-headers/blob/e66ebe90b73d1fa4cce005a5b2072cec27322544/include/psp2/ctrl.h)
map Vita shoulders to L1/R1 in `*2`/`Ext2` calls. A physical v0.7.3 trace
confirmed a game `ReadPositive2` sample of `0xC00` alongside raw L+R `0x300`;
the older filter allowed that opening chord to reach the game. The repaired
filter uses shared canonical chord/replay state, preserves genuine L2/R2
in the mapped calls, and clears hidden buttons' pressure values. Full overlay
blocking clears all 12 pressure fields while preserving timestamps and the
SDK's four reserved bytes. Only a replayed short tap receives a fabricated
pressure of 255. Host checks cover mixed APIs, held suppression and contended
reads; physical validation of this input repair remains required.

## Credits and license

This was made heavily with the help of [Shoui](https://github.com/shoui520)

Shoui is one of the smartest people I know, an expert in Windows and PS Vita lowlevels. She advised the design of the program and helped debug issues, while I simply vibe coded what she suggested. This project would not be possible wihtout her.

Based on [Dartv/vita-jp-overlay](https://github.com/Dartv/vita-jp-overlay).
Thanks to [meikidroid](https://github.com/rtr46/meikidroid),
[jmdict-vita](https://github.com/shoui520/jmdict-vita),
[PSVshellPlus](https://github.com/GrapheneCt/PSVshellPlus),
[PSVshell](https://github.com/Electry/PSVshell),
[reVita](https://github.com/MERLev/reVita),
[ScePaf-RE](https://github.com/GrapheneCt/ScePaf-RE), VitaSDK, BearSSL,
jsmn, acutest, and Yomichan for its Japanese deinflection rules.
Pinned sources and attribution are in `third_party/`.

Code: GPL-3.0 ([LICENSE](../LICENSE)). JMdict: CC BY-SA 4.0. The bundled dictionary's frequency
ranks: the Jiten global frequency list by Jiten (https://jiten.moe), CC BY-SA 4.0, attached to
its entries; its kanji lines: Bee's Ultimate Kanji Dictionary (Jiten and KANJIDIC2 / EDRDG data),
CC BY-SA 4.0; its grammar summaries: written from Bee's Ultimate Grammar Dictionary, whose
sources include Yokubi and NINJAL's 日本語文型データベース (CC BY 4.0)
([attribution](plaintext-attribution.txt)). The beam + LM decoder's character
LMs (bee-san/vita-vn-ocr v0.3.7-lm6.2; the 1 MB and 4 MB ones also carry n-gram counts of the owner's
hooked console captures): CC BY-SA 4.0, with their sources and changes in
`ocr/LM-ATTRIBUTION.md`, which ships beside them. The model sizes' recognizers, VOCR6-S, VOCR6-N and VOCR6-L
(bee-san/vita-vn-ocr v0.4.1-vocr6.2, VOCR5 checkpoints fine-tuned again), were trained in part on NVIDIA's
OCR-Synthetic-Multilingual-v1 (CC BY 4.0), on non-commercial fonts and text, and on the owner's hooked game captures
(copyrighted game data, used for training only and not redistributed): their attribution and terms, including that
the weights are for non-commercial use only, are in `ocr/VOCR6-ATTRIBUTION.md` (VOCR6's) and
`ocr/MODEL-ATTRIBUTION.md` (VOCR5's, the checkpoints they start from), which ship beside them
([details](vita-vn-ocr.md#models)). The text-presence gate's weights
(`ocr/textgate_p8_c8_w8.vtd`, bee-san/vita-vn-ocr v0.3.2-textgate.1; every package ships them,
whether `ocr_text_gate` is on or off) were trained on non-commercial fonts and text and on VNDB
screenshots: they are for non-commercial use only too, and their attribution is in
`ocr/GATE-ATTRIBUTION.md`, which ships beside them
([details](vita-vn-ocr.md#text-presence-gate-api-26)).
