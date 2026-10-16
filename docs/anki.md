# Anki cards: an offline queue, exported as Lapis notes

Enter and Cancel below are the Vita's own buttons: × and ○ on most consoles,
○ and × on Japanese ones or after a HENkaku swap ([README](../README.md#use-in-a-game)).

With card mining on, Enter in the open overlay saves the shown dictionary entry
as a card in a queue on the memory card. Nothing is sent anywhere: saving
needs no network, and the queue survives restarts. When you want the cards in
Anki, the **Overlay** app's Anki page writes them into one file,
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

1. In the **Overlay** app's **Anki** page (Select, or tap the tab), turn
   **Card mining** ON (Enter, or Left/Right). This is `anki_mining` in
   `ux0:data/VitaJPOverlay/config.ini`; the overlay reads it when it next opens.
2. In a game, open the overlay, select a word and press Enter. The status line
   shows `Saving card…`, then `Saved for Anki`, and the entry shows
   `Queued for Anki` (on every occurrence of that word in the text). Enter
   again on it says `Already queued`. In a local character lookup, Enter saves
   the match at the cursor. A line above the entry counts the queue:
   `Anki queue: 3 · export in the Overlay app`.
3. Back in the Overlay app (it opens beside the suspended game), **Export
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
   The notes go into the deck **Vita JP Overlay**; move them to your mining
   deck in Anki's browser (Change Deck) if you like.

**Export all cards** writes every card again, the ones exported before as
well: for a new device, or a package that was lost. Anki adds only the notes
it does not have ([below](#exporting-again-guids)), so importing it into a
collection that has the earlier ones adds nothing twice.

| Status (overlay) | Meaning |
| --- | --- |
| `Saved for Anki` | The card is on the memory card |
| `Already queued` | The same card is queued already |
| `Already exported (in an .apkg)` | This exact card was exported before (the √ on the word) |
| `Card not saved: cannot write …anki_queue (space?)` | The memory card refused the write; the card was not saved |
| `Card not saved: over 32 KiB of text` | The card's text exceeds a record's limit |
| `Anki: not enough memory, card not saved` | The Shell's memory reserve refused the job; nothing changed |

| Result (Overlay app) | Meaning |
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
`overlay app:`).

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
| `Sentence` | The recognized text, the word in `<b>` |
| `SentenceFurigana` | Empty, as Lapis's README asks (Lapis then shows `{{furigana:Sentence}}`); only a card queued with the removed online dictionaries, which gave readings per word, fills it |
| `Picture` | `<img src="vitajp_….jpg">`: the card's screenshot, when it has one (none in this build, see below) |
| `Glossary` | Every dictionary's entry for the word, the shown one first, when there are two or more (Dictionaries shown above 1); Lapis hides the ones `MainDefinition` has |
| `PitchPosition`, `PitchCategories` | Pitch accents, when the dictionary has them (Yomitan's `[2]` markup and `heiban,kifuku`) |
| `Frequency` | `<ul><li>SOURCE: 1234</li></ul>`, when the entry has a frequency rank (`<li>Jiten: 1536</li>` with the bundled dictionary) |
| `FreqSort` | The rank; `9999999` without one, as Yomitan's `{frequency-harmonic-rank}`, so sorting by it puts such cards last |
| `MiscInfo` | The game's name and the local date and time of the Enter press: `CLANNAD (2026-10-14 17:12)` |

The other fields (`ExpressionAudio`, `SelectionText`, `DefinitionPicture`,
`SentenceAudio`, `Hint` and the `Is…Card` switches) stay empty. Every note has
the tag `vita-jp-overlay`. The bundled dictionary (Daijirin4 → Sankoku8 →
JMdict in one file) gives every entry its Jiten global frequency rank
([frequency ranks](local-dictionaries.md#frequency-ranks)): a ranked entry
fills `Frequency` and `FreqSort`; an unranked one leaves `Frequency` empty, and
its `FreqSort` is 9999999. It has no pitch accents, so those fields stay empty,
and each entry's own dictionary (one of the three) names its glossary item.
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
next. A record already archived is kept only when it is that card's (its
checksum is its name); a damaged one there is replaced by the queued copy.
`host/tests/test_anki_restart.py` stops a real export in a fresh process at each
of these steps and checks that no card is lost; `host/tests/test_apkg.py`
imports the packages with Anki 26.09.3 itself in CI.

## The queue on the memory card

Cards are files in `ux0:data/VitaJPOverlay/anki_queue/`:

- `<id>.json`: the card, readable UTF-8 JSON (below). The ID is the first 128
  bits of the record's SHA-256, so the same card saved twice in the same
  minute has the same name (the record holds the minute of the Enter press).
- `exported/<id>.json`: a card an export has put in a package (and
  `exported/<id>.jpg`, its screenshot). **Export all cards** reads these too.
- `<id>.jpg`: the screenshot of a card an earlier development build took.
- `<id>.duplicate.json`: a card an earlier build's AnkiConnect send found in
  Anki already; it is kept, not exported.

A card is written to a temp file, flushed and renamed, and the memory card's
directory is synced before Enter reports it; an interrupted save leaves no
card or the complete one, never a partial one. Files with other names,
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
| `meanings`, `dictionary` | The shown entry's definitions as the dictionary gave them, and that dictionary's name (from its definition's first line, `[JMdict Plain]`, or `null` without one; older records may name a dictionary that has been removed since) |
| `more` | The other dictionaries' entries for the word (Dictionaries shown above 1): each `{"dictionary", "spelling", "reading", "meanings"}`, at most 7, in the order of `local_dictionaries`, kept while the record stays within 32 KiB |
| `rank`, `source`, `pitch`, `pos` | Frequency rank (1 = most frequent, 0 = none) and its source (the frequency list's name, `Jiten` for the bundled dictionary; `null` when there is none), pitch-accent downsteps, parts of speech |
| `sentence`, `hl_start`, `hl_end` | The whole recognized text (every line of the textbox) and the word's UTF-8 byte offsets in it |
| `sentence_furigana`, `sf_hl_start`, `sf_hl_end` | The text with readings and the word's offsets in it, or `null` |
| `picture` | Whether `<id>.jpg` goes with it |
| `misc` | MiscInfo: `CLANNAD (2026-10-14 17:12)` |
| `game`, `title_id`, `platform` | The game's name, its title ID, and `vita` or `psp` (`psp` for Adrenaline's shared title IDs, NPXS10028 and PSPEMUCFW, whose name is Adrenaline's; a PSP game started from its own bubble runs under the bubble's ID and is recorded as `vita`) |
| `ocr_engine`, `ocr_model` | The engine, and the model of the last recognition (`VOCR3_H15_w8.vocr`, or its fallback when that ran) |
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

Not in the record, and why:

- **A screenshot of the textbox.** OCR reads the screen in the game's own
  process, a strip of grey rows at a time ([in-game capture](ingame-capture.md));
  a screenshot from those strips needs the game worker to encode it and a path
  to the memory card from the game (the game cannot see `ux0:`), which this
  change does not add. The old full-frame screenshot used the kernel's
  2,088,960 B capture buffer #74's console refused, so this build takes none.
  A card of an earlier development build that has a JPEG exports it in the
  note's `Picture`.
- **The previous and next dialogue.** The Shell keeps only the text on screen
  (one 4 KiB copy); the next line is not known when Enter is pressed. The record
  has the whole textbox, every line of it.

## Settings that were removed

`anki_host` (AnkiConnect's address) and `anki_audio_url` (a word audio source)
went with AnkiConnect: the overlay and the app ignore them without a warning,
the installer removes them from `config.ini` and says so
(`config.ini: anki_host removed (AnkiConnect was removed: …)`), the Overlay
app's next save drops their lines (and, once it completes, keeps none in `config.ini.bak`: an
audio source's URL can carry a key), and
`anki_host.txt` (where the app's network search kept AnkiConnect's address) is
no longer read or written; delete it if you like. Earlier builds had a field
mapping too: `anki_deck`, `anki_note_type`, `anki_tags` and the 13
`anki_field_*` keys, ignored and removed the same way. Only `anki_mining`
remains.

## Memory

In SceShell (the overlay, `shell/anki.c`):

- One job at a time: counting the queue (the overlay opens) or saving a card
  (Enter). A job runs on a short-lived thread with a 64 KiB stack, which the
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
  fewer. The arena's peak was 102,680 B of the 128 KiB (7,000 B entries of kana).
- The note is copied out of the shown lookup under the view lock before
  anything is written; the result memory is never read during file I/O. If
  memory is refused or a write fails, only the Anki status line changes: the
  recognized text, its lookup and the overlay stay usable, the queued cards
  stay, and Enter can be pressed again.
- SceShell sends nothing and opens no socket for Anki.

The Overlay app is its own process (a system application with a 50 MiB
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
possible follow-up). Cards have no audio and, in this build, no screenshot.
