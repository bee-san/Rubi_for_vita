# Anki cards: an offline queue, exported as Lapis notes

Enter and Cancel below are the Vita's own buttons: × and ○ on most consoles,
○ and × on Japanese ones or after a HENkaku swap ([README](../README.md#use-in-a-game)).

With card mining on, Enter in the open overlay saves the shown dictionary entry
as a card in a queue on the memory card, with a JPEG of the game's screen
([the screenshot](#the-screenshot)). Nothing is sent anywhere: saving
needs no network, and the queue survives restarts. When you want the cards in
Anki, the **Rubi** app's Anki page writes them into one file,
`ux0:data/VitaJPOverlay/anki/<YYYY-MM-DD_HHMMSS>-<N>cards.apkg`, as notes of
the [Lapis](https://github.com/donkuri/lapis) note type (`N` is the number of
notes in it). Copy that file to a computer and open it in Anki (AnkiDroid and
AnkiMobile read the same format, but have not imported one yet): the cards
are then in your collection, and Anki syncs them as usual. The Vita never
talks to Anki, AnkiWeb or any computer.

Mining is off by default: with it off, Enter does nothing, and the overlay
starts no Anki thread, allocates no Anki memory and creates no queue
directory.

## Setup and use

1. In the **Rubi** app's **Anki** page (Select, or tap the tab), turn
   **Card mining** ON (Enter, or Left/Right). This is `anki_mining` in
   `ux0:data/VitaJPOverlay/config.ini`; the overlay reads it when it next opens.
   **Screenshots**, below it, is ON by default (`anki_screenshot`): each card
   keeps a picture of the game's screen, about 60 KB. Turn it OFF for
   text-only cards.
2. In a game, open the overlay, select a word and press Enter. The status line
   shows `Saving card…`, then `Saved for Anki`, and the entry shows
   `Queued for Anki` (on every occurrence of that word in the text). Enter
   again on it says `Already queued`. In a local character lookup, Enter saves
   the match at the cursor. A line above the entry counts the queue:
   `Anki queue: 3 · export in the Rubi app`.
3. Back in the Rubi app (it opens beside the suspended game), **Export
   cards (N queued)** writes the package. Its progress is shown;
   Cancel stops it, and then nothing has changed. When it is done the page says
   where the file is: `Exported 12 cards (34 KB) to 2026-10-15_143000-12cards.apkg`.
4. Copy the file from `ux0:data/VitaJPOverlay/anki/` to a computer or phone,
   any way you like:
   - VitaShell's FTP server or its USB connection: Select in VitaShell starts
     the one chosen under **SELECT button** in its settings (Start); a Vita's
     default is USB, a PS TV's FTP. For USB, **USB device** chooses what the
     computer sees: the Memory Card, or `sd2vita` when an SD2Vita is your
     `ux0:` (VitaShell `81af709`: `browser.c`, `settings.c`, `vitashell_config.h`).
   - With an SD2Vita as `ux0:`, the microSD card itself, in a card reader.
5. Open it in Anki (File > Import on a computer, or open the file on a phone).
   The notes go into the deck **Vita JP Overlay** (its name from before the Rubi rename: Anki finds a
   deck by its name, so a new name would put new cards in a second deck beside the old one); move them
   to your mining deck in Anki's browser (Change Deck) if you like.

**Export all cards** writes every card again, the ones exported before as
well: for a new device, or a package that was lost. Anki adds only the notes
it does not have ([below](#exporting-again-guids)), so importing it into a
collection that has the earlier ones adds nothing twice.

| Status (overlay) | Meaning |
| --- | --- |
| `Saved for Anki` | The card is on the memory card, with its screenshot when Screenshots is ON |
| `Saved for Anki, without a screenshot (log.txt)` | The card is saved, but its screenshot could not be taken (the game's memory, the game worker busy or not registered for this game, a write that failed, the overlay closed meanwhile); `log.txt` says which, in an `anki: screenshot rc=` line |
| `Already queued` | The same card is queued already |
| `Already exported (in an .apkg)` | This exact card was exported before (the √ on the word) |
| `Card not saved: cannot write …anki_queue (space?)` | The memory card refused the write; the card was not saved |
| `Card not saved: over 32 KiB of text` | The card's text exceeds a record's limit |
| `Anki: not enough memory, card not saved` | The Shell's memory reserve refused the job; nothing changed |

| Result (Rubi app) | Meaning |
| --- | --- |
| `Exported 12 cards (34 KB) to ….apkg.` | Written, read back and checked; those cards left the queue |
| `… 2 unreadable or too large stay queued.` | A damaged or edited record, one whose screenshot is missing, or (rare) one too large to make into a note: they stay, see log.txt |
| `… 1 stay queued (see log.txt).` | The package has the card, but moving it out of the queue failed; the next export has it again, harmlessly |
| `… Stopped while clearing: 40 stay queued, for the next export too.` | Cancel after the package was made: it has every card, and those 40 are in the next one too (Anki adds no note twice) |
| `… 5 newer stay queued: export again.` | A package holds 20,000 cards at most: the oldest went, the newest wait (**Export all cards**: `5 newer are not in it`) |
| `Could not write a complete file (memory card full? see log.txt). The queue is unchanged.` | No package; the queue is as it was |
| `Stopped. No file was made; the queue is unchanged.` | Cancel during the export |
| `No cards queued: save words in the overlay first.` | Nothing to export |

With `log_file = on` the app writes every storage call that fails, with its
error code, and each export's counts to `log.txt` (lines starting
`Rubi app:`).

## The Lapis note

The package carries the note type itself: [Lapis](https://github.com/donkuri/lapis)
**1.7.0** (tag `1.7.0`, commit `38238d0`; its release `Lapis.apkg` has SHA-256
`85984f8c6f1263e2882c364bbbfdee4bce9e1e03f053a0a5b0ccc6cd90de0088`), whose
templates and styling are vendored unchanged in `third_party/lapis/` with
their GPL-3.0 licence, the same licence as this project. It keeps Lapis's own
note type ID (`1667218449922`), name, 22 fields and template (`Mining`), so:

- a profile without Lapis gets it from the package;
- a profile that has Lapis 1.6.0 or 1.7.0 (installed from its `Lapis.apkg`)
  keeps its own note type, with your styling and settings, and the notes use
  it. The package's copy is marked older than any Lapis release, so Anki's
  default import option (update note types *if newer*) never replaces yours.
  Checked with Anki 26.09.3 against Lapis 1.7.0's and 1.6.0's own packages;
- with Lapis older than 1.6.0 (19 to 21 fields), Anki's default import adds
  the package's as a second note type, `Lapis+`, for its notes; with **Merge
  note types** ticked they go into your Lapis instead, without the fields it
  lacks. Update Lapis first (import its newest `Lapis.apkg`) to have them in
  one note type with every field. (Checked with Anki 26.09.3 and Lapis 1.0.0,
  1.4.0 and 1.5.1.)

The fields are filled as Lapis's README asks Yomitan to fill them:

| Lapis field | Filled with |
| --- | --- |
| `Expression` | The dictionary form |
| `ExpressionFurigana` | `言葉[ことば]` (Yomitan's `{furigana-plain}`): the dictionary's own ruby where it has one, else the word's reading |
| `ExpressionReading` | The reading in kana |
| `MainDefinition` | The entry shown, as Yomitan writes one dictionary's glossary: `<div class="yomitan-glossary"><ol><li data-dictionary="JMdict">…</li></ol></div>`, so Lapis names its page after the dictionary |
| `Sentence` | The recognized text, the word in `<b>`, its lines joined where the game wrapped the sentence (a speaker's name line and a new sentence keep their `<br>`; [words split over two lines](touch-lookup.md#words-split-over-two-lines)). The queue keeps the text as recognized, so this happens at export, also for cards queued before |
| `SentenceFurigana` | Empty, as Lapis's README asks (Lapis then shows `{{furigana:Sentence}}`); only a card queued with the removed online dictionaries, which gave readings per word, fills it |
| `Picture` | `<img src="vitajp_….jpg">`: the card's screenshot, when it has one ([below](#the-screenshot)) |
| `Glossary` | Every dictionary's entry for the word, the shown one first, when there are two or more (Dictionaries shown above 1); Lapis hides the ones `MainDefinition` has |
| `PitchPosition`, `PitchCategories` | Pitch accents, when the dictionary has them (Yomitan's `[2]` markup and `heiban,kifuku`) |
| `Frequency` | `<ul><li>SOURCE: 1234</li></ul>`, when the entry has a frequency rank (`<li>Jiten: 1536</li>` with the bundled dictionary) |
| `FreqSort` | The rank; `9999999` without one, as Yomitan's `{frequency-harmonic-rank}`, so sorting by it puts such cards last |
| `MiscInfo` | The game's name and the local date and time of the Enter press: `CLANNAD (2026-10-14 17:12)` |

The other fields (`ExpressionAudio`, `SelectionText`, `DefinitionPicture`,
`SentenceAudio`, `Hint` and the `Is…Card` switches) stay empty. Every note has
the tag `vita-jp-overlay`. The bundled dictionary, Bee's Ultimate Dictionary,
gives every entry its Jiten global frequency rank
([frequency ranks](local-dictionaries.md#frequency-ranks)): a ranked entry
fills `Frequency` and `FreqSort`; an unranked one leaves `Frequency` empty, and
its `FreqSort` is 9999999. It has no pitch accents, so those fields stay empty.
Its entries name no source; the file's own title names their glossary item,
`<li data-dictionary="Bee's Ultimate Dictionary">`, and the definition keeps its
kanji lines (`日=day, counter for days, Japan, sun`), each on its own line.
Another local dictionary has ranks when it is converted with `--frequency`;
none imports pitch accents ([local dictionaries](local-dictionaries.md)).
jpdb.io and jiten.moe, the online dictionaries that also gave pitch accents and
readings per word, have been removed; cards they queued before still export
with all of theirs.

### Exporting again: GUIDs

Anki recognizes a note by its GUID. A note's GUID here comes from its card's
queue ID (the record's checksum, below): base91 of its first 64 bits, the
alphabet Anki's own GUIDs use. So every package that has the same card has the
same note, and Anki's import (rslib `import_export/package/apkg/import/notes.rs`)
leaves a note it has as it is: the package's note is never newer than yours
(its modification time is the Enter press, by the Vita's clock), so your edits
stay, and its card (and its reviews) are not added twice. That holds while the
Vita's clock is not ahead of the computer's: an edit made in Anki before the
Enter time the Vita recorded would lose to the package's note. A note you deleted in Anki is added again
by a package that has it: use **Export cards** (the queue only) for new cards,
**Export all cards** when you want everything.

Saving the same word from two different texts makes two cards, as two Enter
presses do; Anki keeps both (its browser marks notes whose first field is the
same as Duplicate).

### The package

The `.apkg` is a ZIP (stored entries, no compression) of `collection.anki2`,
an SQLite database of Anki's schema 11 (rslib `storage/schema11.sql`), the
`media` map (a JSON object of numbered entries to file names) and the numbered
media files. It has no `meta` entry, which Anki reads as its "legacy 1"
package (`import_export/package/meta.rs`): the oldest of the three package
versions that Anki's shared import code reads, which every Anki since 2.1 can
import, unlike the newer zstd-compressed `collection.anki21b` (2.1.50 and
later). Tested here with Anki 26.09.3's own backend (the `anki` package);
AnkiDroid and AnkiMobile have not imported one yet. The app writes the database itself
(`core/sqlw.c`, a writer of SQLite's file format for tables written once, in
rowid order) and the ZIP (`core/zipw.c`); there is no SQLite library in the
app. A screenshot is named by its own SHA-256 (`vitajp_<32 hex digits>.jpg`),
so the same picture is one file, in one package and across packages (Anki
keeps one copy of a name it has with the same content).

An export is safe at every step: the package is written under a temporary
name (`<date>_<time>.apkg.tmp`), flushed and closed, then opened again and
checked whole: the ZIP's directory and every entry's CRC-32 (computed from the
bytes as they were written, so a card that does not read back as written
fails), the database's header and tables, every note's ID and GUID in order,
the cards and the media map. Only a checked file gets its name, and the memory
card is synced; only then do its cards move out of the queue into
`anki_queue/exported/` (their screenshots first). So a file under an `.apkg`
name is always a checked one. A failure or Cancel before that removes the file
and changes nothing; a Cancel while the queue is cleared stops between two
cards, and a stop there leaves the rest queued, which the next export takes
again (the same notes). An interrupted export's `.tmp` file is removed by the
next, and an export that verified its package removes the queue's screenshots
no card names, unless a Cancel stopped its clearing
([below](#the-queue-on-the-memory-card)). A record already
archived is kept only when it is that card's (its
checksum is its name); a damaged one there is replaced by the queued copy.
`host/tests/test_anki_restart.py` stops a real export in a fresh process at each
of these steps and checks that no card is lost; `host/tests/test_apkg.py`
imports the packages with Anki 26.09.3 itself in CI.
`host/tests/test_settings_anki.c` drives the Anki page itself
(`settings/anki_page.c`): its rows' layout, Up/Down, the left stick, Left/Right,
Enter and touch on each row, each row's save into `config.ini` as the overlay
then reads it, and an export on its worker thread, with Cancel and the app's
exit during one.

## The queue on the memory card

Cards are files in `ux0:data/VitaJPOverlay/anki_queue/`:

- `<id>.json`: the card, readable UTF-8 JSON (below). The ID is the first 128
  bits of the record's SHA-256, so the same card saved twice in the same
  minute has the same name (the record holds the minute of the Enter press).
- `<id>.jpg`: the card's screenshot, a baseline JPEG of the game's screen
  ([below](#the-screenshot)), when its record says `"picture": true`. A JPEG
  whose record is not there (a save that stopped between the two) is no card:
  it is ignored and never exported. The next save of that card in the same
  minute (the same ID) removes it first; a later save is another card, with
  another ID, so a JPEG left by a power loss or a crash between the two writes
  (or a `<id>.jpg.tmp` left by one during the JPEG's write), about 56 KiB,
  stays until the next export that writes a package (#161). Once that package
  is verified, the export removes from `anki_queue/` every `.jpg` that no
  record names (no `<id>.json` there or in `exported/`, and no
  `<id>.duplicate.json`) and every `.jpg.tmp`, each once its file time is 10
  minutes older than the package's (`log.txt`: `anki export: …: no card names
  it, removed`). A younger one, one dated after the package (a clock set back)
  or not dated at all, and one whose records the memory card cannot say are
  absent (a check that fails other than "not found") stay for a later export:
  a save still finishing has its record within seconds of its
  JPEG (its screenshot job ends within 8 s), and the kernel removes a stopped
  screenshot's `.jpg.tmp` itself. A removal that fails is logged and tried
  again by the next export. `exported/` is never swept: an export
  stopped between moving a card's JPEG and its record leaves the JPEG there
  ahead of its record, and the next export takes it from there.
- `exported/<id>.json`: a card an export has put in a package (and
  `exported/<id>.jpg`, its screenshot). **Export all cards** reads these too.
- `<id>.duplicate.json`: a card an earlier build's AnkiConnect send found in
  Anki already; it is kept, not exported.

A card is written to a temp file, flushed and renamed, and the memory card's
directory is synced before Enter reports it; an interrupted save leaves no
card or the complete one, never a partial one. A card's screenshot is on the
memory card (renamed from its own temp file, `<id>.jpg.tmp`; the record's
own sync syncs it too, #194) before its record is written, so a record never names a picture that is not
there. Files with other names,
including leftover `.tmp` files, are ignored. Editing a record changes its
checksum, so the card is kept and reported as unreadable rather than exported.
Delete a card's `.json` to drop it.

### The record (version 4)

Enter records everything it knows about the card when it is pressed, so that a
card can be made into a note later, by any version that reads the record:

| Key | Content |
| --- | --- |
| `version` | 4 |
| `spelling`, `reading`, `furigana` | The word's dictionary form, its reading, and the dictionary's ruby of it (`来[く]る`; `null` when the dictionary has none, as every one now: only the removed online dictionaries gave one) |
| `meanings`, `dictionary` | The shown entry's definitions as the dictionary gave them, and that dictionary's name: its file's title (`Bee's Ultimate Dictionary`, page 0 of the file), else its definition's first line (`[JMdict Plain]`, as the converter writes one), or `null` with neither; older records may name a dictionary that has been removed since |
| `more` | The other dictionaries' entries for the word (Dictionaries shown above 1): each `{"dictionary", "spelling", "reading", "meanings"}`, at most 7, in the order of `local_dictionaries`, kept while the record stays within 32 KiB |
| `rank`, `source`, `pitch`, `pos` | Frequency rank (1 = most frequent, 0 = none) and its source (the frequency list's name, `Jiten` for the bundled dictionary; `null` when there is none), pitch-accent downsteps, parts of speech |
| `sentence`, `hl_start`, `hl_end` | The whole recognized text (every line of the textbox, as recognized) and the word's UTF-8 byte offsets in it; a word across a line break the text joins has the break inside them |
| `sentence_furigana`, `sf_hl_start`, `sf_hl_end` | The text with readings and the word's offsets in it, or `null` |
| `picture` | Whether `<id>.jpg`, its screenshot, goes with it. The ID is the record's checksum, so a card with a screenshot has another ID than the same card without one; saving a card checks both, so the same card is never queued twice whatever the setting |
| `misc` | MiscInfo: `CLANNAD (2026-10-14 17:12)` |
| `game`, `title_id`, `platform` | The game's name, its title ID, and `vita` or `psp`. A PSP game started from an Adrenaline bubble is recorded with the title and disc ID the Rubi app read from the file it boots (`ux0:data/VitaJPOverlay/psp/<bubble ID>.txt`), else with the bubble's own; games started from inside Adrenaline are recorded as `PSPEMUCFW`, under the name AppMgr gives that process (not checked on a console), or as `NPXS10028`, named `PSP`, when the Shell cannot tell which app started the emulator |
| `ocr_engine`, `ocr_model` | The engine, `vocr` (a record of a build before Google Lens's removal may say `lens`), and the model of the last recognition (`VOCR5_N_w8.vocr`, the Normal size's, or the smaller size's when that ran) |
| `ocr_confidence` | `null`: the recognizer reports no confidence yet; the key is there for when it does |
| `time` | When Enter was pressed, to the minute, as MiscInfo: seconds since 1970 (UTC); the note's creation and modification time |

A record is at most 32 KiB. Version 4 is version 3 with a new "no rank": 0,
since Jiten's ranks reach 398,255 and include 99999, which versions 1-3 wrote
for none (jpdb's missing rank). Records of version 3 (v0.8.0-test.2), version 2
(#49, v0.8.0-test.1: the same keys up to `misc`) and version 1 (#5) still load
and export, their rank 99999 read as none (the notes of versions 1 and 2 have
a fixed time, 2023-11-14, the same in every package); a local definition's
first line names its dictionary there too. A version this build does not know
is kept, not exported.

Not in the record, and why: **the previous and next dialogue.** The Shell
keeps only the text on screen (one 4 KiB copy); the next line is not known when
Enter is pressed. The record has the whole textbox, every line of it.

## The screenshot

With **Screenshots** ON (`anki_screenshot = on`, the default), Enter keeps a
JPEG of the game's screen with the card, and the export puts it in the note's
`Picture`: Lapis shows it beside the word, at most 400 px tall, and full size
when it is tapped. Upstream's builds and this fork's until #125 sent one to
AnkiConnect with each note; this is its return, made where heavy work belongs
(AGENTS.md rule 1): in the game's process, never in SceShell.

It is the game's whole frame when Enter is pressed, 960 × 544 (a PSP game as
Adrenaline shows it, at 2×), without the overlay, which is SceShell's layer
and not in the game's frame. With the overlay open the game has no input, so
this is normally the screen the text was read from; a game that changes its
screen by itself (a scene that goes on alone, a video) is pictured as it is
then, and, as the frame is read over 9 of the game's frames, possibly with a
seam between two bands of rows.

### How it is made (API 23)

1. The Shell's Anki thread makes the card's record and its ID first, then asks
   the game worker for a screenshot of that card (`VJO_GAME_OCR_F_SHOT`): a job
   of the worker's one job slot, which `shell/worker.c` shares with the OCR jobs.
   A quiet refresh of the open overlay yields to it; a running OCR job is waited
   for, at most 4 s.
2. The game worker checks the game's free memory (its block beside the 4 MiB
   every job leaves to the game: a screenshot holds no model, and the larger
   reserves, 8 and 16 MiB, follow the largest model a job holds, so it keeps
   the base one even beside Large's resident weights), takes one block of
   450,560 B (`USER_RW`, else
   CDRAM, else phycont) and reads the frame in colour, 64 rows a frame, through
   the OCR capture's own path: the display hook copies the rows as A8B8G8R8
   words straight into the block (`VJO_ROWS_RGBA`), or, when it cannot write
   the block, through the kernel strip, 16 rows a call
   ([in-game capture](ingame-capture.md#an-anki-cards-screenshot-api-23)).
3. It encodes each band of 16 rows as it comes (`core/jpegsw.c`: baseline JPEG,
   YCbCr 4:2:0, the standard tables, quality 70) into the same block. A JPEG
   over 192 KiB (the most an export reads) is encoded again at quality 50, then
   35; none of the 598 frames measured below needed it.
4. It hands the JPEG to the kernel in 32 KiB pieces (`vjoOcrShotWrite`, through
   the staging buffer the model reads use), and the kernel's worker thread
   writes them to `anki_queue/<id>.jpg.tmp`, closes it and renames it to
   `<id>.jpg` (`vjoOcrShotSave`). It syncs nothing: test.4's two whole-card
   syncs held the kernel's worker thread for over 15 s, and test.4.2's sync of
   the file turned the console off (#194). The Shell's record, written next,
   is committed with a sync of the memory card, which takes the JPEG with it. The game has no
   `ux0:` of its own (`0x80010013`, as for the models); the path is the kernel's,
   made from the card's ID it checked (32 lowercase hex digits), and the bytes
   must start and end as a JPEG does.
5. The Shell commits the record, with `"picture": true`, once `<id>.jpg` is
   there. Any failure on the way leaves no file and saves the card without one
   (`Saved for Anki, without a screenshot (log.txt)`). The Shell waits at most
   8 s for the whole screenshot, then cancels it (sooner when the overlay
   closes); that ends the game worker's wait for a slow memory card's save,
   and the worker then waits up to 30 s more for the file's close. A save that
   ends within those 30 s, at most 45 s after the request, leaves the worker
   free for the next job, its file removed by the kernel's worker thread. One
   slower still leaves it refusing jobs until the game restarts, as any file
   the kernel holds for the game does (it never acknowledges a file it has not
   closed), and its `.tmp` until the kernel's close runs.

SceShell holds no pixel and no byte of the JPEG, only the job's request and
result (5,404 B) in the Anki job's arena. Closing the overlay stops a
screenshot in progress.

### Size and quality

Quality 70 at the full 960 × 544, measured with the encoder itself
(`vjo-jpeg`, `host/jpeg_tool.c`) on local copies of the owner's game screens
(598 frames of 80 games; not in the repository), in KB of 1,024 B:

| Frames | Count (games) | Median | Mean | p90 | p95 | Largest |
| --- | --- | --- | --- | --- | --- | --- |
| Vita, 960 × 544 | 294 (47) | 55.6 KB | 55.5 KB | 89.7 KB | 101.8 KB | 135.3 KB |
| PSP, 480 × 272 shown at 2× (bilinear) | 304 (33) | 46.6 KB | 46.0 KB | 69.7 KB | 74.3 KB | 87.4 KB |

Quality 70 keeps a typical card near 50 to 60 KB with the game's text legible
in the picture: `docs/screenshot.jpg` (a game screen with the overlay) comes
out at 48,655 B and 36.6 dB PSNR, against 38,579 B and 33.9 dB at quality 50
(medians 41.8 KB for Vita frames, 36.0 KB for PSP ones) and 103,421 B and
49.5 dB at 95. The full resolution is kept because Lapis opens the picture at
its own size when it is tapped: a smaller one would be blurred there, and
saves little (the JPEG's size goes with its detail, not its pixels alone).

On the memory card a card takes its JPEG and its record (at most 32 KiB;
under 1 KB for a short entry, more with long definitions and, with
Dictionaries shown above 1, the other dictionaries' entries), each rounded up
to the file system's clusters (not measured here). A thousand Vita cards'
screenshots are about 54 MiB.

### Time and memory

Not measured on a console yet; `log.txt`'s `anki: screenshot rc=…` line has
every part of it for a console run (the strips' wait, the encoding, the file,
the whole). Estimates: the colour strips take 9 of the game's frames (150 ms at
60 fps), each strip's copy 0.28 to 2.45 ms in the game's display call at 444 MHz
(0.38 to 3.02 ms at 333 MHz; 131,401 instructions, mostly the copy, whose
firmware routine is unknown code: `capture_shot_abgr`); the encoding is 60.0
million instructions for a 960 × 544 frame on
the Cortex-A9 (`tools/bench_stages.py`'s `jpeg_shot`, the Vita compiler's code
counted under qemu-arm, `docs/performance.md`): 69 to 149 ms at 444 MHz, 93 to
198 ms at 333 MHz (a screenshot keeps the game's clock); it runs at the lowest
priority on the game's cores, so every game thread goes first. The memory
card's writes, close and rename are timed in the debug build's `kernel.txt`
(`ocr shot write`, `ocr shot save`). The screenshot's load on CPU3
is unmeasured too (AGENTS.md rule 8): no PSVshell reading exists.

In the game, the screenshot's block is 450,560 B (the encoder's state, one
strip of 64 colour rows and the JPEG, page-rounded; `VJO_BUDGET_GAME_SHOT`),
only while it runs; a screenshot is a job of the one job slot, so it never runs
beside an OCR job's workspace and adds nothing to the game's worst case
([memory ledger](memory-ledger.md)). The kernel takes nothing new: the 32 KiB
staging buffer it already has, and on the last rung a kernel strip of at most
64 KiB for one call, as for OCR.

### Export

The export (`settings/anki_export.c`) reads `<id>.jpg`
beside its record, names it in the package by its own SHA-256
(`vitajp_<32 hex digits>.jpg`, so the same picture is one file), adds it to the
`media` map and writes `<img src="…">` into `Picture`. Once the package is
written, read back and checked it moves the JPEG into `anki_queue/exported/`
before its record, never earlier, and **Export all cards** reads it there
again. Nothing deletes a card's screenshot: an exported card's stays in `exported/` so
that **Export all** can include it; delete `exported/<id>.jpg` and its record
to free the space. The only pictures an export removes are the queue's that
no card names ([above](#the-queue-on-the-memory-card), #161).

What the host checks: every encoder output decoded by Pillow
(`host/tests/test_jpegsw.py`); a screenshot through the actual game worker and
kernel transport in each pixel format a game submits, decoded too
(`host/tests/test_game_shot.c`, `test_game_shot.py`); the kernel's rules
(`test_kernel_game_ocr.c`); the Shell's saves with one and without
(`test_shell_anki.c`); a save stopped at each of its writes in a fresh process
(`test_anki_restart.py`); the screenshots such a stop leaves without a card,
removed by the next export that verifies a package and no other picture
(`test_apkg.c`, and `test_anki_restart.py` with real stopped saves and
exports); and Anki 26.09.3 importing a package whose card shows
its screenshot (`test_apkg.py`: the `<img>` in the rendered answer, the file in
the collection's media with the same bytes, a JPEG that decodes). The console
has not run it yet.

## Settings that were removed

`anki_host` (AnkiConnect's address) and `anki_audio_url` (a word audio source)
went with AnkiConnect: the overlay and the app ignore them without a warning,
the installer removes them from `config.ini` and says so
(`config.ini: anki_host removed (AnkiConnect was removed: …)`), the Rubi
app's next save drops their lines (and, once it completes, keeps none in `config.ini.bak`: an
audio source's URL can carry a key), and
`anki_host.txt` (where the app's network search kept AnkiConnect's address) is
no longer read or written; delete it if you like. Earlier builds had a field
mapping too: `anki_deck`, `anki_note_type`, `anki_tags` and the 13
`anki_field_*` keys, ignored and removed the same way. `anki_mining` and,
since the screenshot's return, `anki_screenshot` remain.

## Memory

In SceShell (the overlay, `shell/anki.c`):

- One job at a time: counting the queue (the overlay opens) or saving a card
  (Enter). A job runs on a short-lived thread with a 16 KiB stack, which the
  control thread joins once it ends. Enter during a count saves right after it.
- Saving takes one bounded allocation from the Shell's Paf heap, through the
  same guard as the result workspace, which keeps the 1.5 MiB UI reserve:
  128 KiB (`ANKI_TEXT_MEM`, the ledger's `VJO_BUDGET_PAF_ANKI`; the build checks
  they agree). It is freed before the thread ends. Counting only reads the
  directory, on the thread's stack, with no heap allocation. Nothing
  Anki-related stays allocated between jobs, and the queue is never read into
  memory whole.
- With Dictionaries shown above 1, Enter reads each other dictionary's entry for
  the word from its file into that arena, one at a time (at most 8,448 B each,
  `VJO_LOCAL_SELECTED_BUDGET`, then compacted to the entry's own strings),
  after copying the 208-byte page locators under the view lock. An entry is
  kept while the card's record stays within 32 KiB; the first that would not
  fit, and the ones after it, are left out, and log.txt says how many were
  kept. Measured on the host with 8 dictionaries
  (`what_enter_saves_whatever_the_dictionaries`): entries of 2,700 B of kana all
  kept (a 22,673 B record); 7,000 B, 3 of 7; text that JSON escaping grows keeps
  fewer. The arena's peak was 102,624 B of the 128 KiB (7,000 B entries of kana; 102,616 B on main
  `dcb7b31`, before `anki_screenshot` added a field to the settings' copy at the arena's start).
- The note is copied out of the shown lookup under the view lock before
  anything is written; the result memory is never read during file I/O. If
  memory is refused or a write fails, only the Anki status line changes: the
  recognized text, its lookup and the overlay stay usable, the queued cards
  stay, and Enter can be pressed again.
- A card's screenshot adds only its job's request and result to the arena,
  5,404 B: a one-word card's save peaks at 6,860 B with it, and the 8-dictionary
  worst case is the same with it or without (102,624 B) [measured, host]. Its pixels and its JPEG
  are the game's ([above](#the-screenshot)).
- SceShell sends nothing and opens no socket for Anki.

The Rubi app is its own process (a system application with a 50 MiB
budget), not one of the plugins, so it is not in the plugins' [memory
ledger](memory-ledger.md). Its Anki page holds a static 2 MiB arena for an
export (`VJO_EXPORT_MEM`: the list of cards, 32 B each for up to 20,000, the
writers' state, about 40 KiB, and one card's work at a time) and a 64 KiB stack
for the one worker thread that exports while the page keeps drawing. It uses no
network. Measured on the host (`vjo-apkg-tests`, "measure:" lines): an export
of 20,005 cards (the 20,000 a package holds, 10,182,874 B) peaked at 804,713 B of
the 2 MiB; one of 700 cards, some with 9 KB definitions, peaked at 187,111 B and
wrote a 606,426 B package. These are host measurements; a physical Vita has not
run this code yet.

## Not included

No AnkiConnect, AnkiWeb or self-hosted sync and no login: the package is the
only way cards leave the Vita, and you copy it yourself. A package is not
compressed (a deflated one would be smaller over FTP; the app has zlib, a
possible follow-up). Cards have no audio.
