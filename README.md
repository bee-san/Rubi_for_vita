

# Vita JP Overlay — local Japanese OCR

This [bee-san fork](https://github.com/bee-san/vita-jp-overlay) reads visual-novel
text and shows Japanese dictionary entries over your game. **The v0.7.12 release defaults to
on-console vita-vn-ocr, automatic OCR, and bundled plain-English JMdict.**
Recognition and dictionary lookup work without Wi-Fi or a running computer.
The release includes **Overlay**, a standalone per-game settings app. Optional
[Anki card mining](docs/anki.md) is fully offline: Enter saves the shown word as
a card in a queue on the memory card, and the Overlay app's Anki page, where it
is also switched on, exports the queue as [Lapis](https://github.com/donkuri/lapis)
notes into an `.apkg` file that you copy to Anki yourself (FTP, VitaShell's USB
mode, or an SD2Vita's card). Nothing is sent from the Vita; there is no
AnkiConnect or AnkiWeb sync.

Text is recognized on the Vita, offline, by the local engine, named after the
generation of the model it runs (**VOCR5** in this package), automatically or
when the overlay opens. In the **Overlay** app (press **Select** for the
OCR & lookup page), **Model size** chooses its recognizer: **Small**
(fastest), **Normal** (the default, balanced) or **Large** (most accurate), each
shown with its estimated time per textbox and the game memory it takes
([model sizes](docs/vita-vn-ocr.md#model-sizes-small-normal-large-api-21)). When
a game has too little free memory for the chosen size, a job runs the next
smaller one by itself; only `log.txt` says so. The release stores the three
sizes' models; a choice applies when you next open the overlay.

Google Lens, the [upstream's](https://github.com/Dartv/vita-jp-overlay) online
engine, has been removed: recognition never uses the network. A saved
`ocr_backend` line is ignored (`lens` with a config warning in `log.txt` and the
overlay), and the Overlay app's next save and the installer delete it. The last
build with Google Lens is the tag `archive/online-last` (branch `archive/online`).

The engine's name comes from the pinned metadata of the model it runs
(`core/vocr_models.c`, also written per model into the package's
`package-manifest.json`), not from the app: VOCR5 for each of this package's
three sizes (vita-vn-ocr's v0.3.0 release; before it VOCR3 for Small and Normal
and VOCR4 for Large). A release that pins a new generation's models as the sizes
shows that generation's name, with no change to the app; a size that keeps an
earlier generation's model shows that one's. A `.vocr` file itself carries no
generation. v0.2.0's models are generation VOCR1.

The experimental VOCR2 trial (a separately trained FL10-sized model with a
textbox finder, opt-in in 0.7.12 to v0.8.0-test.1; its promotion guard failed)
has been removed. A saved `ocr_backend = vocr2` now loads as the local engine
with a config warning, and the app's next save and the installer delete the
line; so does `ncnn`, which only the host tools run. The no-longer-used
`ux0:data/VitaJPOverlay/ocr/VOCR2_FL10_w8.vocr` can be deleted. The
[`0.7.12` release](https://github.com/bee-san/vita-jp-overlay/releases/tag/v0.7.12)
contains the three plugins, Settings, FL10/H15/VOCR2 and JMdict; it also
carried the optional Meiki engine. Earlier releases remain available.

Meiki, a manual single-line recognizer, has been removed from the source after
0.7.12. Its MNN engine had to be copied into each game's own `app0:` folder
through a per-title rePatch mount, and two observed jobs took about eight
seconds each. A saved `ocr_backend = meiki` now loads as VOCR with a config
warning, and the installer deletes the line. Files copied for it,
`ux0:data/VitaJPOverlay/ocr/meiki-stream-int8.mnn` and any
`ux0:rePatch/TITLEID/VitaJPOverlay/meiki/` folders, are no longer used and can be deleted.

VOCR has completed actual CLANNAD OCR jobs on the physical Vita.
Hardware accuracy/CER and broader game compatibility remain unmeasured; host crop results are separate.
See [OCR measurements and limits](docs/vita-vn-ocr.md) and
[project lessons](docs/project-lessons.md).

The v0.7.10 release prepares the first local match during automatic OCR and
reuses completed results when reopening unchanged dialogue. In Whole word
mode, Right skips the matched source word and Left retraces recent selections.
The header also explicitly refreshes its highlight, with bounded layout retries.
Host tests cover these paths; the physical Vita result remains unverified.
This release does not include
the experimental local-dictionary memory reservation from
[issue #42](https://github.com/bee-san/vita-jp-overlay/issues/42).

All six published v0.7.9 assets match the audited bundle. That version's installed
Shell/Settings bytes and native Settings registration/launch are verified, and
four actual Settings touch/controller actions saved both highlight modes while
preserving VOCR and Japanese-only ON. In-game header taps and highlight geometry
still require hardware checks. See the
[installation checkpoint](docs/todo-20261011.md#v079-touchhighlight-installation-checkpoint)
for the measured results and remaining hardware checks.

## Install

You need a PS Vita on firmware 3.60–3.74 with HENkaku/Ensō or h-encore, VitaShell, **Settings >
HENkaku Settings > Enable Unsafe Homebrew** on, and about 245 MB free on the memory card (ux0) before
you copy the VPK to it: 61 MB for the VPK, 170 MB for VitaShell to unpack it and 12 MB for the models
Install copies. Deleting the `.vpk` once VitaShell has installed it gives its 61 MB back.

1. Download `VitaJPOverlay-<version>.vpk` from the
   [latest release](https://github.com/bee-san/vita-jp-overlay/releases/latest).
2. Copy it to the Vita with VitaShell's USB or FTP connection (Select in VitaShell starts the one
   chosen under **SELECT button** in its settings, Start; a Vita's default is USB) and install it in
   VitaShell: press Enter on the file, then Yes to "Do you want to install this package?" and to its
   notice that the package requests extended permissions.
3. Open **Overlay** from LiveArea. It opens on its **Install** page, which lists exactly what it will
   do; choose **Install**, then restart the Vita.

Nothing is downloaded on the Vita: the VPK holds the plugins, the models and the dictionary
([how it works](docs/one-vpk-install.md)). Then start a game and press L + R. Every game is ON by
default and needs no step of its own: the kernel plugin loads the OCR worker into each game that is
ON as it starts ([worker loading](docs/worker-loading.md)), never into SceShell, a system app,
VitaShell or the Overlay app, and never while L is held at the game's start. Close the Overlay app
before installing a VPK over it. These steps are not yet verified on a console: the next test build's
run checks them.

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

The release also has `VitaJPOverlay-<version>-debug.vpk`, the test builds' own build with every log
line, the self-test and the timings. Install it only when asked for a full log; either installs over
the other and the settings carry over ([release-variants](docs/release-variants.md)).

Open **Overlay** from LiveArea to enable or disable individual games. Older builds, including
0.7.12, call it **JP Overlay Settings**; the title ID (`VJOSET001`) and VPK file name are unchanged,
so the new VPK installs over it. The settings app itself is always excluded from the overlay. Each
build's app carries a version stamp (`vjo_app.txt`) and the plugins a record of the app they came
with (`ux0:data/VitaJPOverlay/vjo_app.txt`); when the two differ, the Shell plugin says so once, in
red under the recognized text of the first overlay opened in a game (`Overlay app and plugins are
different builds: open the Overlay app.`), and the app's Install page says which way it is.

The Overlay app is a system application, like the Music or Video app: it
opens beside a running game, which stays suspended behind it, so there is no
need to close the game first. It should also open from its LiveArea with no game
running. In v0.8.0-test.3, the first build whose app is a system application, it
did not (the owner's report, 2026-10-16), and test.4 is the first build to check it
on a console. A possible cause, found in its graphics library's source and not
seen on a console: its start waited for SceShell to hand it the screen without
letting SceShell's threads at or below its priority run, and used the system's
shared GPU parameter buffer with no fallback. It now pauses for SceShell, falls
back to a buffer of its own, closes itself after 10 s if the system never hands it
the screen, and says how its start went in `log.txt` (`overlay app: graphics: ...`;
[no game running](docs/vita-memory.md#starting-with-no-game-running), with what each
line means). Games turned ON or OFF there take effect when the
game next starts or resumes, except that a game turned ON while it runs gets
OCR at its next start (the worker is loaded as a game starts; until then the
overlay says `OCR is not running in this game. Close the game and start it
again.`); preferences apply when the overlay next opens. A
game that uses extended memory (`ATTRIBUTE2` in its `param.sfo`) may still have
to close first, as for other system applications.

PSP games run in Sony's PSP emulator. That process most likely has the title ID
of the app that started it, as Adrenaline's and AdrBubbleBooter's own plugins key
on it (from their sources; not yet seen on a console): `PSPEMUCFW` for Adrenaline
itself, and the bubble's own ID for a game started from its own LiveArea bubble
([Adrenaline Bubble Manager](https://github.com/ONElua/AdrenalineBubbleManager)
bubbles, booted by [AdrBubbleBooter](https://github.com/shoui520/AdrBubbleBooter-oss)), such as
`PSPEMU005` or `ULJS00378`. The kernel plugin loads the worker into these processes
like any game: by the ID's own row in the Overlay app, else the shared PSP row,
else the default. `NPXS10028`, the emulator's own ID, is treated the same should
the kernel see it. With `log_file = on` (the default), `log.txt` has a `psp:`
line for each PSP game as it starts, naming its IDs, its disc ID and the file it
boots.

### Advanced: install from a computer

Each release also has `VitaJPOverlay-<version>.zip` (and `-debug.zip`) for the computer-side
installer, the developers' path. It needs Python 3.9+, bash and curl. Close games, open VitaShell,
set **SELECT button** to **FTP** in its settings (Start; once: a Vita's default is USB, #136), press
Select to start FTP, and run from the extracted folder:

```sh
bash install-vita.sh VITA_IP:1337
```

It checks the package manifest, backs up both taiHEN configurations in `build/device/backup/` of the
extracted folder, and on the Vita each one it changes (as `config.txt.vjo-bak` and a dated
`config.txt.vjo-<date-time>.bak`), and your overlay settings, uploads and checksum-reads back each file, and edits the
configurations with the same rules as the Overlay app (`tools/patch_tai_config.py`, the app's
`core/taicfg.c` in Python; both are tested on one set of fixtures). `--title` is no longer needed and
is ignored with a note. It ends with a box that says whether the Vita's Overlay app is this
package's: install `ux0:data/JPOverlaySettings.vpk` in VitaShell afterwards (this zip's app carries no
overlay files of its own, so its Install page names this installer);
`bash install-vita.sh --check-app VITA_IP` asks again, read-only. The 0.7.12 archive's own
`install-vita.sh` predates all of this and registers the worker under `*ALL`; use the installer from
`main` or a later release.

For a manual installation, copy `ur0/` and `ux0/` of the zip to their matching Vita partitions and
install `JPOverlaySettings.vpk`. Back up your old overlay `config.ini` before copying the included
one. Add `ur0:tai/VitaJPOverlay_Kernel.skprx` under `*KERNEL` and `ur0:tai/VitaJPOverlay_Shell.suprx`
under `*main` in the active taiHEN config (`ux0:tai/config.txt` if it exists, else
`ur0:tai/config.txt`). `ur0:tai/VitaJPOverlay_OCR.suprx` needs no line: the kernel plugin loads it
into each game that is ON (from `ux0:tai/` only when `ur0:tai/` has none, so leave no older copy
there). Never list it under `*ALL`. Disable old `VitaJPOverlay_Text.suprx` entries and old worker
lines, then reboot. Keep unrelated plugins and configuration intact.

## Use in a game

**Enter** and **Cancel** are the Vita's own: × enters and ○ cancels on most
consoles, the other way round on Japanese ones, and HENkaku can swap them on
any console. The overlay, its hints and the Overlay app follow that setting
(`/CONFIG/SYSTEM/button_assign`): the overlay reads it each time it opens, the
app when it starts.

| Control | Action |
| --- | --- |
| L + R | Open or close the dictionary overlay |
| Select + R | Toggle the subtitle strip |
| Left / Right | Move through the text in reading order, across its rows: skip matched words in Whole word mode, one character in Character mode; Right from a row's last word goes to the next row's first, Left from a row's first to the row above's last, and the text's first and last stop the cursor. A text on one row goes round its ends (Right from the last to the first, Left from the first to the last) |
| Touch a recognized character | Look up the word starting there with a local dictionary |
| Up / Down | Move to the displayed row above/below, soft-wrapped rows included, keeping the horizontal position |
| Left stick | Move the cursor in the d-pad's four directions, faster while held: left/right as Left/Right (the shown word skipped, then a character a step; Left retraces), up/down a row as Up/Down, a diagonal as the direction pushed farther; it stops at the text's first and last character, a one-row text's too (its left still undoes a d-pad Right that went round, as any Left retraces a Right). The word is looked up once the cursor rests for `stick_lookup_ms` (200 ms), not at each step ([left stick](docs/touch-lookup.md#the-left-stick-138)) |
| Right stick | Scroll a long definition |
| Square | Draw the dialogue region with the left stick or the touchscreen ([Drawing the region](#drawing-the-region)) |
| Hold Square | Restore the full-screen region |
| L or R (tap alone) | Show the previous/next local dictionary's entry when **Dictionaries shown** is 2–8 |
| Enter | Save the shown word as an Anki card in the offline queue, with card mining on ([Anki mining](docs/anki.md)) |
| Cancel | Close the overlay |

Pressing PS with the overlay open closes it: the game's controls are released
in the suspend's own event, the overlay no longer turns off the LiveArea's
touch region on its way out (#104), and the game resumes with the overlay
closed. Host tests cover this; the console run is still to confirm it
([PS and resume](docs/touch-lookup.md#ps-suspend-and-resume-104)).

Choose a tight dialogue region first: smaller regions need less RAM and reduce
recognition errors. Regions are saved per game. Automatic mode recognizes
settled dialogue in the background and prepares its first local match in Whole
word mode. A completed lookup appears immediately when opening unchanged
dialogue. Character mode defers dictionary lookup until opening the overlay.
Selection changes never recapture the screen or rerun OCR. While the overlay is
open, its text stays usable for lookups and Anki whatever the screen does, with no
"dialogue changed" message: in Auto mode a newly settled screen is recognized in the
background and replaces the text quietly only when it reads a different one
([touch lookup](docs/touch-lookup.md)). A job that read a screen changing under it
shows nothing and is retried quietly. The Overlay app's **Read text** chooses this, **Automatically
(when it changes)**, the default, or **Only when I press L + R** (`ocr_mode = on_press`), which recognizes
only when the overlay opens: nothing runs before, and the press then waits for the reading
([what each costs, and test.3's times](docs/auto-vs-press.md)). With card mining on, Enter saves the shown word,
its sentence and every dictionary's entry for it to the card queue on the
memory card, with a JPEG of the game's screen (about 60 KB, made in the game's
own process: [screenshots](docs/anki.md#the-screenshot)); the Overlay app exports
the queue as an `.apkg`, the screenshot in each note's Picture.

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

## Per-game settings

The **Overlay** app shows three large game cards at a time, with wrapped
titles, visible IDs and clear ON/OFF badges. Games are listed by letter (#,
A–Z, kana by row, then other characters), alphabetically within it, capital
and small letters together. Tap the **Games**, **OCR & lookup**
or **Anki** tab, or press Select, to change pages. Choices save automatically to
`ux0:data/VitaJPOverlay/games.ini` and take effect on game start/resume.
Icons load one per frame, the cards on screen first, and stay loaded for those
cards and a page either side, so scrolling back does not reload them; a card
shows **JP** until its icon is loaded, or for good when the icon is missing or
unreadable ([details and memory](docs/settings-readability.md#the-games-list)).

| Control | Action |
| --- | --- |
| Up / Down | Select a game (held: repeats) |
| Left stick up / down | As Up / Down, as in VitaShell: pushed past half way it moves one row, then, held, after a sixth of a second 60 rows a second, at whatever rate the app draws; how far it is pushed beyond that does not change the speed. Also on the OCR & lookup and Anki pages, where it stops at the first and last card while Up / Down wrap round |
| L / R | Page up / page down; the selection keeps its row (held: repeats) |
| Left / Right | Left: first game of this letter, or of the previous one when already there; Right: first game of the next letter. Letters: # (digits, symbols), A–Z, kana by row (か for が, ア for ヴ), others by first character |
| Enter or touch a game | Toggle OCR and overlay input for that game |
| Triangle or touch the default row | Default ON or OFF. OFF turns every game OFF, including those turned ON one by one; ON turns every game ON except those turned OFF one by one. The status line names them ([details](docs/settings-readability.md#games-missing-from-the-list-and-default-off-138)) |
| Square | Filter all, enabled or disabled games |
| Start | Refresh the installed-title list (the only time it is read, with the app's start) |
| Select or touch a page tab | Switch between Games, OCR/lookup and [Anki](docs/anki.md#setup-and-use) pages |
| Cancel | Exit |

On the **OCR & lookup** page, select a row with Up/Down. Enter
or touch a row changes its choice: the Model size, **Read text**, the **Speed boost while
reading (444 MHz)**, Japanese-only selection, whole-word/character highlighting, or how many
dictionaries are shown. Touch a size, a Read text choice or a number to choose it directly;
Left/Right choose the next smaller or larger size on the Model size row (Enter goes round them),
Automatically/Only when I press on Read text, OFF/ON on the Speed boost and selection cards,
Whole word/Character on the Highlight card, or 1–8 on Dictionaries shown. The page is taller
than the screen: it scrolls a card at a time to keep the selected one whole, the next card shows
at the bottom, a bar at the right shows where the view is, and a touch on a card cut at the
bottom only scrolls it into view. Read text (`ocr_mode`, [Use in a game](#use-in-a-game)) reads
settled text in the background (**Automatically (when it changes)**, the default: the popup is
ready when you press) or only once the overlay opens (**Only when I press L + R**, named after
`toggle_button`: nothing runs until you press, and the press waits for the reading); the Speed
boost (`ocr_boost_clock`, ON by default) runs the ARM clock at 444 MHz only while text is read
([docs/auto-vs-press.md](docs/auto-vs-press.md)). Each size button shows its estimated time for a 3-line
textbox and the game memory a job of it takes (`0.9 s, 1.4 MB` for Normal), from
the models' pinned metadata. The
card's last line names the size and its model file (`Normal: default, balanced.
VOCR5-N found.`), or reports that file missing; it never says that a game ran a
smaller size. Choices save automatically to `config.ini` and apply when the overlay next opens.
Model files remain stored. Start reloads
the saved preferences. Other config values, including dictionary priority,
paths and capture mode, are preserved; only the lines of removed features are
rewritten (the [removed online dictionaries](#settings-and-dictionaries)'
API keys go, `dictionary = jpdb`, `jiten` or `hachidori` becomes `local`, and the
removed engine choice's `ocr_backend` line goes).

Japanese-only selection is **ON by default**. The initial cursor and the
arrows skip Latin text, digits, punctuation and Hangul while selecting kana
and Han characters. It does not tokenize words or hide the recognized
sentence. With it OFF, every visible character is selectable; the arrows skip
spaces and line breaks. No Japanese characters means no character lookup or
dictionary allocation.

**Whole word** highlighting is the default. It highlights the actual dictionary
match, including the original inflected text, when lookup finishes. **Character**
highlights only the selected character. A pending or unsuccessful word lookup
keeps the character highlighted. In Whole word mode, Right advances past the
matched source span and Left retraces up to 16 recent forward selections. With
no match or previous selection, movement falls back to one character.
Character mode always moves one character; Japanese-only applies to both modes.
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
([row navigation](docs/touch-lookup.md#rows-and-arrow-keys)).
The left stick crosses a long line faster (#138). Each arrow press looks its
word up at once: 35-112 ms a step in the test.3 console run (median 56 ms, its
31 cursor lookups in `log.txt`), and in Whole word mode Right skips a word only
once that word's lookup is shown. The stick moves in the arrows' four
directions, by the arrows' own rules: held, it steps through the text in
reading order across rows (or a row up or down), one step at once, the next
after a sixth of a second as in the Overlay app's lists, then each sooner, up
to 20 steps a second (the cap, chosen so the cursor does not overshoot: an
estimate, untested on a console); it stops at the text's first and
last character, a one-row text's too, instead of going round (its left still
undoes a d-pad Right that went round, as any Left retraces a Right). Nothing is looked up while it moves: the
word at the cursor is looked up once the cursor has rested there for
`stick_lookup_ms` (`config.ini`, 200 ms by default, 0-1000), or has been held
against the text's end that long; back on the word shown before that, nothing
is looked up again. The d-pad and taps still look up at once.
The right stick scrolls a long definition (before, either stick did). Host
tests cover the timing, the cap and the deferred lookup
([left stick](docs/touch-lookup.md#the-left-stick-138)); how it feels on a
console is not tested yet.
Tap a character in the recognized sentence at the top of the open overlay to
look up the word starting there; Japanese-only selection also applies to taps.
Kana text shows the kana word (から, する) and inflected text its dictionary
form (した → する); a short built-in list gives very frequent kanji their common
reading (私 → わたし). Each lookup writes one `log.txt` line with the text at the
cursor, the entry, its length, the dictionary, file reads and time
([word lookup on recognized text](docs/touch-lookup.md#word-lookup-on-recognized-text)).
The recognized lines are one running text: where the game wrapped a word over
two lines (食べ / るかな), the word is looked up whole from its first part and
highlighted on both lines, Right skips it as one word, and an Anki card's
Sentence has the lines joined there; a speaker's name line, a new sentence or a
line the game broke short keeps its own line. In the real line sets about one
line in six that has a next line in its box ends inside a word
([words split over two lines](docs/touch-lookup.md#words-split-over-two-lines)).
Where the overlay wraps text itself (the recognized text, the popup, the
subtitle strip, the Overlay app's rows) it breaks a line between phrases, where
kana gives way to kanji or katakana, at spaces and at punctuation, never inside a
run of kanji, katakana or kana, and keeps Japanese line-breaking rules: no line
starts with 、。」ー っ, none ends with 「. It is no dictionary walk, so a compound
can still break at its seam (女の|子)
([the overlay's own line breaks](docs/touch-lookup.md#the-overlays-own-line-breaks)).
How these look on a console is not tested yet.
[Touch selection details and validation](docs/touch-lookup.md).

Disable games such as Uncharted when you need their normal L+R controls.
Disabled games have no overlay capture or input filtering. A game with its own
ON or OFF in `games.ini` is listed even when the app does not find its folder
(as with CLANNAD in #138, a game card it cannot read, or a deleted game). It is
listed under its update's title or its ID, with `(folder not found)`, so its
choice can still be changed. Each Start (and the app's start) writes
what each folder held to `log.txt` ([why](docs/settings-readability.md#games-missing-from-the-list-and-default-off-138)). An unreadable or
corrupt policy fails closed and reports the problem rather than enabling every
game. A game turned ON gets OCR at its next start: the kernel plugin loads the
OCR worker into it as it starts ([Install](#install)).

PSP games have a `PSP` tag. Each Adrenaline bubble is listed under its game's
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

## Settings and dictionaries

`ux0:data/VitaJPOverlay/config.ini` is reloaded when the overlay opens. A line
the plugins do not know or cannot use is listed in red in the open overlay and
logged in `log.txt` once, and again only when the set of such lines changes
(test.2's log had 76 lines from 8 of them, #101). A removed engine
(`ocr_backend = lens`, `vocr2`, `meiki`, `ncnn`) runs the local engine with a
config warning, and the app's next save and the installer delete the line (any
`ocr_backend` line: VOCR is the only engine). The installer, the package's own copy and the Overlay
app write the file; the Shell plugin never does, and with no file it runs on
these defaults:

```ini
dictionary = local
local_dictionary_dir = ux0:data/VitaJPOverlay/dictionaries
local_dictionaries = plaintext.vjdict
local_dictionary_results = 1
text_source = ocr
ocr_model_dir = ux0:data/VitaJPOverlay/ocr
vocr_tier = normal
ocr_mode = auto
japanese_only_selection = on
log_file = on
```

`vocr_tier` is the local engine's model size, the Overlay app's Model size:
`small` runs VOCR5-S (819,792 B of int8 weights: FL10's topology with 4,987
classes), `normal` (the default) VOCR5-N (1,185,632 B: H15's topology with 5,631
classes) and `large` VOCR5-L (2,473,696 B: a late-wide shape, 5,631 classes), the
three recognizers of vita-vn-ocr's VOCR5 round (its v0.3.0 release; selection and
scores in that repository's `docs/vocr5.md`). Each is inside the owner's weights
ceiling for its size (Small < 1 MB, Normal <= 1.6 MB, Large <= 2.5 MB). Their
estimated times for a 3-line textbox are 0.6 s (Small), 0.9 s (Normal) and
2.1 s (Large) on one core at 333 MHz with the pinned runtime (vita-vn-ocr #85's
kernels, no credit for their weight prefetch), calibrated to VOCR3-H15's measured
4.94 ms per CTC frame on the console (v0.8.0-test.1); at 444 MHz, the clock a job
raises by default, 0.5, 0.7 and 1.6 s, and less again with lines read in parallel
([OCR speed](docs/ocr-speed.md)). No console has run a VOCR5 model yet: these are
estimates until a test build's logs measure them.
A job runs the chosen size when the
game's free memory has room for it beside that size's reserve (4, 8 or 16 MiB),
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
64 KiB for one call ([in-game capture](docs/ingame-capture.md)). An Anki card's screenshot
is read in the game too, in colour strips, and encoded there. API 24 removed the kernel's
full-frame capture buffer, which nothing used any more. A Shell UI memory error
after recognition keeps the recognized text and identifies the unavailable
dictionary results.

Text that types out is read once it is whole: a job recognizes nothing until
every row of the region has held still for `ocr_settle_ms` (150 ms by default,
`0` turns it off), waiting at most `ocr_settle_max_ms` (2000) per recognition,
or `ocr_settle_press_ms` (1000; `0`: the same limit) when the open overlay waits
for it ([first-ocr.md](docs/first-ocr.md)), and a blinking "next" icon does not hold it up. `ocr_skip_blank_lines = on`
skips found lines with no text-like ink; it is off by default, because once it
keeps soft text (a PSP game shown 2x, an upscaled render) it finds next to
nothing to skip in the test screenshots. Both are logged per job; see
[stable text and blank lines](docs/vita-vn-ocr.md#stable-text-and-blank-lines-api-20)
for what each costs and what the host measurements show.
The recognized text is shown, saved and looked up in one form of each mark
(full-width `！` as `!`, `〜` as `~`, `...` as `…`, `ー` or `一` by context), the
form vita-vn-ocr's scorer uses; the recognizer's own text is in `log.txt`
([text normalisation](docs/vita-vn-ocr.md#text-normalisation-shown-and-looked-up)).
Each line is decoded with a beam search and a character language model
(`ocr_lm = 1000k` by default; `4000k`, `2000k`, `500k` or `off`). The LM is
vita-vn-ocr's, CC BY-SA 4.0. The game keeps the largest LM that fits its memory
resident, and reads greedily when none does. A line read with low confidence
is read again by the next model size up (VOCR5-L for Normal, VOCR5-N for
Small; Large has none), when its model fits too, whose reading is shown, while the job
stays within `ocr_cascade_ms` (2000; `0` turns it off). On the host's
development dialogue lines the two brought VOCR3-H15, the Normal size before
VOCR5, from 1.40 % to 0.73 % CER (with VOCR4-W20 as its cascade).
The greedy reading, each line's confidence and what the cascade read again are
logged beside the shown text
([beam + LM decoding](docs/vita-vn-ocr.md#beam--lm-decoding-api-20),
[confidence cascade](docs/vita-vn-ocr.md#confidence-cascade-api-20)).
In PSP games the line finder's boxes are also refined, cut back to their text
and split where two text rows were merged (`ocr_refine_lines = psp`; `on` for
every game, `off`). vita-vn-ocr #83 measured a dialogue gain with it on native
480×272 PSP frames; on native Vita text it was level or worse, and worse on
CLANNAD for five of six models. The overlay sees a PSP game as
Adrenaline shows it, 2× through a bilinear filter, where its accuracy is
untested and the host's box fit is level, so `psp` is the next test build's
trial, which that build's logs decide. Refined lines are read wider: on the
host, 12 % more recognizer frames in PSP regions shown 2×, an estimated
22-23 ms more per line found with VOCR3-H15 on the console
([finder refinement](docs/vita-vn-ocr.md#finder-refinement-api-20)).
Recognition runs up to three lines at once (`ocr_threads = 3`, the default; `1` reads one line
after another), each on its own thread at the lowest priority on the game's cores. Each line's LM
decode, the cascade's re-read and its place in the text still follow line order on the job's own
thread, so the text is the serial job's (except where the cascade's time ceiling decides: a job that
recognizes its lines sooner may read more doubtful ones again). A line whose pixels are unchanged
since an earlier job gets that job's text without being recognized again. `ocr_boost_clock = on`
(the default, the Overlay app's **Speed boost while reading**; `off` keeps the game's clock) raises the ARM clock to 444 MHz while a job recognizes and
puts it back after; a clock already at 444 MHz or more, such as PSVshell's 500, is left as it is, and
the bus and GPU clocks are never changed. The
log shows each job's helpers, its lines' share of a core, the clock, and the game's frame pacing
during the job against the time before it ([OCR speed](docs/ocr-speed.md); estimates until a
console run).
The complete settings template is
[docs/config.example.ini](docs/config.example.ini).

The next release bundles one frozen disk-backed `plaintext.vjdict`, **Bee's
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
is what Anki cards and `log.txt` record and what the Overlay app's
**Dictionaries shown** card shows. A function word's summary replaces its
definition (は is "Topic particle, pronounced "wa" …"); a content word with a
grammatical use (上, こと, 無くなる) keeps its definition, with the summary on the
line before it. An entry longer than the popup holds is cut at the end of its
definition and marked `…`, and keeps its kanji lines and that summary line;
the right stick scrolls a long one ([popup](docs/local-dictionaries.md#the-popup)).

Deduplication keeps a spelling and normalized reading pair from the first
source that contains it, preserving that source's separate senses, distinct
readings and inflection rules. The result has 486,529 entries (301,720
Daijirin4, 12,557 Sankoku8, 170,920 JMdict and 1,332 grammar points no word entry
had), 369,838 of them with kanji lines, and occupies about 150 MiB on disk,
904 KiB of it a key filter that answers most inflection guesses that are not
words without an index read ([key filter](docs/local-dictionaries.md#key-filter)).
It stays on disk and is never loaded whole into RAM. Eleven definitions are
cut at the existing 7,000-byte limit; 16 empty or overlong source rows cannot
be indexed. Exact provenance and counts are in
[docs/plaintext-dictionaries.json](docs/plaintext-dictionaries.json), with
[attribution](docs/plaintext-attribution.txt) and the
[JMdict license](docs/JMdict-LICENSE.html).

Every entry carries its rank in the [Jiten](https://jiten.moe) global
frequency list (CC BY-SA 4.0), or is explicitly unranked: 53.52% of the
entries are ranked, and a tap on 99.24% of the word occurrences in Jiten's
2,162 visual-novel scripts shows a ranked entry. The popup shows the rank after the
reading (`猫 (ねこ) #1536`; `frequency_badge = off` hides it), Anki cards record
it, and when the text ranks two entries alike the more frequent one is shown
([frequency ranks](docs/local-dictionaries.md#frequency-ranks)). The rank
travels inside each entry on disk: no table of ranks is held in memory.

You can add your own Yomitan dictionaries with
[the converter and installation guide](docs/local-dictionaries.md).
Up to eight dictionary files can be enabled in priority order. The Overlay
app's **Dictionaries shown** (`local_dictionary_results`, 1–8, default 1) pages
through files for the selected word. The combined bundle is one file, so
it displays the preferred source; add separate dictionaries to page through
other definitions. Only the page on screen is held in memory.

The subtitle strip (Select + R) shows the recognized text, without readings,
and adds no lookups for it. Recognition and dictionary lookup make no network
request and have no cloud fallback: the local dictionaries are the only ones.

The online dictionaries jpdb.io and jiten.moe, which needed an API key, have
been removed (the local Jitendex and every other Yomitan dictionary still
work, converted as above). A saved `dictionary = jpdb` or `jiten` now loads
as `local` with a config warning in `log.txt` and the overlay; the installer
(`tools/migrate_config.py`) and the Overlay app's next save write `local` and
delete the `jpdb_api_key`, `jiten_api_key` and older `api_key` lines (the
app's save leaves none in `config.ini.bak` either; the installer names each
removed key, never its value). The subtitle strip's furigana, which only
those two services gave, went with them.

The Hachidori relay, which looked words up in the Hachidori app on a computer
on your local network (`dictionary = hachidori`, `hachidori_host`), has been
removed from the overlay too; the Hachidori app itself is unaffected. A saved
`dictionary = hachidori` now loads as `local` with one config warning, and the
Overlay app's next save and the installer write `local`; `hachidori_host` is
ignored, and the installer drops it.

### Anki

Card mining is off by default (the test builds turn it on). With it on, Enter
saves cards as readable JSON files in `ux0:data/VitaJPOverlay/anki_queue/`, each
with a JPEG of the game's screen beside it (`anki_screenshot = on`, the default:
960 × 544 at quality 70, 55.6 KB for the median of 294 Vita screens, 46.6 KB of
304 PSP ones). The game worker reads the frame and encodes it in the game's
memory (a 450,560 B block while it runs), and the kernel writes it to the memory
card; SceShell holds none of it ([the screenshot](docs/anki.md#the-screenshot)).
The cards survive restarts and need no network. Each record (version 4) keeps everything known at the
press: the word, its reading and furigana, the shown entry and its
dictionary's name, the other dictionaries' entries, the whole recognized text
with the word's offsets (as recognized; the exported Sentence joins the lines
the game wrapped), the game, its title ID and platform, the time, and the OCR
engine and model.

The Overlay app's **Anki** page has card mining ON/OFF, **Screenshots**
ON/OFF, **Export cards (N queued)** and **Export all cards**. An export writes the cards as
notes of [Lapis](https://github.com/donkuri/lapis) 1.7.0 (GPL-3.0, its
templates bundled unchanged), in the deck **Vita JP Overlay**, into
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
app's next save remove them. See [docs/anki.md](docs/anki.md) for the fields, the queue, the package
and memory.

## Memory and troubleshooting

The game OCR worker runs the local OCR engine in the enabled game's process, allocating its
region-sized workspace from that game's `USER_RW`. The kernel plugin loads it into each game
that is ON in `games.ini` as the game starts, at the point where taiHEN loads a game's own
plugins, never into SceShell, a system app, VitaShell or the Overlay app, and a game keeps one
copy ([worker loading](docs/worker-loading.md)). It still checks the title at startup and
unloads itself at once (`SCE_KERNEL_START_NO_RESIDENT`) in SceShell, VitaShell, the settings
app and other system titles, should a configuration line load it there.
Candidate games keep only an idle worker, at the lowest user priority so every
game thread preempts a recognition, until the Shell confirms their per-game
policy; disabled games load no weights and allocate no OCR workspace. Each game that is ON
holds the worker's 94,208 B of module pages and its 36,864 B thread stack and TLS from its
start, homebrew included (ON by the default): an app that fails to start with them turns OFF
in the Overlay app, or starts with L held.
Games started from inside Adrenaline share its process (`PSPEMUCFW`); a game
started from its own bubble runs in a PSP emulator process of its own, under the
bubble's ID (inferred from AdrBubbleBooter's source). The worker is also loaded
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
logs each attempt ([docs/ingame-capture.md](docs/ingame-capture.md)). The
worker frees each job's workspace before returning text. The verified weights stay in their own `USER_RW` block between jobs
(819,792 B for VOCR5-S, 1,185,632 B for VOCR5-N and 2,473,696 B for VOCR5-L,
held as 823,296, 1,187,840 and 2,473,984 B page-rounded blocks; the worker takes
no larger one) and are read and
SHA-256 checked again only when the model file's size or modification time
changes, or the model changes. A job that finds no line (a title screen, a
box between two lines) reads them, and the LM and the cascade's model, if they
are not resident yet, so the first scan with text does not wait for them
([first-ocr.md](docs/first-ocr.md)). They are released when a job's memory check
fails, when the file is missing or wrong, when the game has not been the active
one for 5 s of its running time (another app in front of it, title disabled; the
time stops while the game is suspended, so a suspend and resume keeps them) and
on exit. A job
then needs only its region-sized workspace. The worker leaves free USER memory
for the game beside what the job holds: 4 MiB with Small, 8 MiB with Normal and
16 MiB with Large (the largest size the job holds, the cascade's included;
vita-vn-ocr docs/model-tiers.md proposes them, an estimate), before allocating
any block; allocation can still fail because the game or system uses memory
concurrently.

Model files are opened by the existing kernel worker because some games cannot
access the `ux0:` storage namespace (`0x80010013`, ENODEV). The broker accepts
only these names in `ux0:data/VitaJPOverlay/ocr`:<!-- names from kernel/game_ocr.c, written by tools/docs_model_names.py -->
the recognizers `VOCR5_N_w8.vocr`, `VOCR5_S_w8.vocr` and `VOCR5_L_w8.vocr`
(VOCR5, release v0.3.0); `VOCR3_H15_w8.vocr` and `VOCR3_FL10_w8.vocr` (VOCR3,
release v0.2.5-vocr3.1); `FL10_w8.vocr`, `H15_w8.vocr` and `F20_w8.vocr` (VOCR1,
release v0.2.0); `VOCR4_W20_w8.vocr` (VOCR4, release v0.2.5-vocr4.1); and the
character LMs `vocr_lm_o3_500k.vlm`, `vocr_lm_o3_1000k.vlm`,
`vocr_lm_o3_2000k.vlm` and `vocr_lm_o3_4000k.vlm` (release v0.2.6-ctclm.2)<!-- end of the names -->.
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
entries in the same arena; an L or R tap releases the shown entry and reads one
other payload, so several definitions are never resident together.
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
the Overlay app uses the network for Anki.
The kernel startup instruction and all three plugins' syscall imports are audited.

These are bounded allocations and host checks, not a measured physical Vita
peak or a guarantee that every full-screen region fits in every game.
[docs/vita-memory.md](docs/vita-memory.md) is the reference for the Vita's memory
pools, which process each plugin runs in, and the budgets each context can use.
[docs/memory-ledger.md](docs/memory-ledger.md) is the ledger of every byte the
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
[no append on the result path](docs/release-variants.md#no-log-append-on-the-result-path)); `kernel.txt`
holds up to 256 KB per boot (the previous boot's is
`kernel.prev.txt`). `bash tools/install_ftp.sh --collect VITA_IP` fetches all of
it read-only into one timestamped folder and zip, with its audio source URL and
any API key an older build left in `config.ini` redacted, both taiHEN configurations, the Anki queue, crash dumps and the
installed files' hashes, and prints its analysis
([self-test](docs/self-test.md#collecting-everything---collect)); `--status VITA_IP`
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
the OCR verdict. See [docs/self-test.md](docs/self-test.md). Each game OCR job logs the
game's free USER memory against what the job needed (also when it refused the
job) and, for each recognized line, its size, CTC frames and recognition time.

A boot that does not complete (the Shell plugin not alive a minute after start,
or SceShell exiting with the plugin loaded) makes the next boot a **safe boot**:
the kernel plugin installs no hooks and the Shell and game plugins unload
themselves, so the Vita starts as if the overlay were not installed, and
`status.txt` says why. Reboot once to try again; after two failed boots in a
row safe mode stays until you install a new build (the installer resets the
record) or delete `ur0:data/VitaJPOverlay/boot.txt`. See
[docs/boot-safety.md](docs/boot-safety.md). If the Vita does not boot at all,
hold L while turning it on (with h-encore or h-encore2: open its menu, holding R as
h-encore starts, and choose Exit while holding L), so that taiHEN loads no plugin; then
in VitaShell put `#` before the two `VitaJPOverlay` lines of `ur0:tai/config.txt` (and
`ux0:tai/config.txt`), or open the Overlay app and choose **Uninstall** on its Install
page, or from a computer run `bash tools/install_ftp.sh --uninstall VITA_IP`. Restart
normally. Uninstalling preserves data. A `config.txt.vjo-<date-time>.bak` is the
configuration from before that install, not before the overlay: an update replaces the
plugin files its lines name. If one game does not start, turn it OFF in the Overlay app,
or hold L while starting it ([recovery](docs/one-vpk-install.md#recovery-r1)).

The Overlay app is not a plugin and never starts by itself, so it cannot stop
the Vita from starting, and its Install page writes nothing until you choose an
action. If it misbehaves, close it from its LiveArea page. To go back to an older
build, install that release's VPK and choose Install on its Install page (it names the
version it goes back to); deleting the app leaves the overlay as it is, and the
plugins keep reading `games.ini` and `config.ini` without it. With no app installed the overlay says
nothing about it; an older one than the plugins' is said once a boot, at the first
overlay opened.

## Build and validation

The Vita build stamps the Overlay app: `tools/app_stamp.py` writes `vjo_app.txt`
(`vjoapp1 api=API build=` the first 16 hex digits of its eboot.bin's SHA-256)
into the VPK and, the same bytes, into the release's `ux0/data/VitaJPOverlay/`
as the record of the app these plugins come with ([core/app_stamp.h](core/app_stamp.h)).
`package_release.py` refuses a base zip whose two copies differ and writes the
stamp and the eboot's SHA-256 into the manifest (`app`). `--check-app` compares the
Vita's app with the package's stamp file and its VPK's `eboot.bin`, `--collect`'s
analysis with the record and the manifest's `app`.

The Vita build checks the artwork inside `JPOverlaySettings.vpk` before it can
ship. Bubble and LiveArea PNGs must use opaque, non-interlaced, 8-bit indexed
color with no text chunks (`tEXt`, `iTXt`, `zTXt`), the bubble icon may be at most
32 KiB and the start-up image at most 600 KiB; RGBA PNGs can fail installation
with `0x8010113D`. It also checks that the app's `eboot.bin` is the system
application it is built as (`tools/check_settings_self.py`: the SELF's attribute
and 50 MiB budget, the main thread's CPU3 and priority, the heap, every thread
created on CPU3). The Overlay mascot was drawn by a pinned, Apache-2.0
text-to-image model; [settings/assets/ICON-SOURCE.md](settings/assets/ICON-SOURCE.md)
records the model, checksums, prompt and seed. To recreate the committed artwork
from that image, install `tools/requirements-assets.txt` and run
`python3 tools/make_settings_assets.py`; it writes each image with at most 256
colours and checks that the palette PNG decodes to exactly that art.

The default build enables vocr. Its runtime is external to this repository and
checksum-pinned; see [the build guide](docs/vita-vn-ocr.md#build). Set up VitaSDK/Paf with
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

The Vita build is Release, as in CI; `vita/` uses Release when no build type (or
an empty one) is given. A `Debug` build compiles libpng without optimisation, which fails the
Overlay app's [PNG ABI check](docs/settings-png-runtime.md), so the app does not
build.

`-DVJO_DEBUG=OFF` builds the normal build, the release's main download, without
the debug build's log lines, self-test, timings and traces (12 KiB of SceShell
pages fewer on main `c08db18`); `ON`, the default, is the debug build, which the test builds are. The
`release_variants` target builds the other one too and, with
`-DVJO_RELEASE_MODELS=/path/to/models`, packages both as
`VitaJPOverlay-<version>.zip` and `-debug.zip` with their `SHA256SUMS`; CI builds
and tests both on every pull request ([release-variants](docs/release-variants.md)).

Check out the exact `plaintext-dicts` commit recorded in
`docs/plaintext-dictionaries.json` and point `VJO_PLAINTEXT_SOURCE` at that
checkout. CI uses a read-only deploy key and the same exact commit; a pull
request from a fork gets no secrets, so its CI leaves out the dictionary, the
tests on it and the release zip, and runs every other build and test. To prepare
the dictionary separately, run `python3 tools/prepare_plaintext.py --source
/path/to/plaintext-dicts -o plaintext.vjdict`; without `--source`, authenticated
GitHub CLI access can fetch the pinned archive. Both the archive and unpacked
file must match their committed checksums.

CI builds the Vita targets in the `vitasdk/vitasdk:2026.08` container, and the
Overlay app's `eboot.bin` depends on the SDK's libraries: another VitaSDK gives
it other bytes. `bash tools/ci_container_build.sh --compare RELEASE.zip`
rebuilds a commit in that container, pinned by the digest CI pulled, under
docker or podman, and compares the result with a release
([how](docs/offline-testing.md#cis-vita-build-in-its-container)).

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
([release-variants](docs/release-variants.md#the-releases-two-zips)). CI runs it on both builds' zips of every pull request that changes
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
cancellation and 50 card lifetimes. The Overlay app's `.apkg` export runs on
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
`eval/` screenshots. The experimental text-extraction branch remains
separate from this release.

[docs/offline-testing.md](docs/offline-testing.md) says what of this runs where:
the real plugin sources with the SDK boundary stubbed on the host, the pure-C
kernel and core suites on the Vita's CPU under qemu-arm (`tools/arm_host_tests.sh`),
nothing in Vita3K (it has no SceShell, no kernel plugins and no memory budgets),
and the Shell budget replay model (`tools/shell_budget_model.py`) that ranks a
build's SceShell footprint against the console's measured samples. Boot and
memory on the console remain the self-test's to answer.

[docs/performance.md](docs/performance.md) is the stage benchmark
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

Code: GPL-3.0 ([LICENSE](LICENSE)). JMdict: CC BY-SA 4.0. The bundled dictionary's frequency
ranks: the Jiten global frequency list by Jiten (https://jiten.moe), CC BY-SA 4.0, attached to
its entries; its kanji lines: Bee's Ultimate Kanji Dictionary (Jiten and KANJIDIC2 / EDRDG data),
CC BY-SA 4.0; its grammar summaries: written from Bee's Ultimate Grammar Dictionary, whose
sources include Yokubi and NINJAL's 日本語文型データベース (CC BY 4.0)
([attribution](docs/plaintext-attribution.txt)). The beam + LM decoder's character
LMs (bee-san/vita-vn-ocr v0.2.6-ctclm.2): CC BY-SA 4.0, with their sources and changes in
`ocr/LM-ATTRIBUTION.md`, which ships beside them. The model sizes' VOCR5 recognizers
(bee-san/vita-vn-ocr v0.3.0) were trained in part on NVIDIA's OCR-Synthetic-Multilingual-v1
(CC BY 4.0) and on non-commercial fonts and text: their attribution and terms, including that
the weights are for non-commercial use only, are in `ocr/MODEL-ATTRIBUTION.md`, which ships
beside them ([details](docs/vita-vn-ocr.md#models)).
