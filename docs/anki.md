# Anki card mining with an offline queue

With card mining on, × in the open overlay saves the shown dictionary entry
as a card in a queue on the memory card. Saving needs no network: it works
with Wi-Fi off, Anki closed or no computer set, and the queue survives
restarts. The cards go to [AnkiConnect](https://ankiweb.net/shared/info/2055492159)
in Anki on a computer on your local network, as notes of one note type,
[Kiku](https://github.com/youyoumu/kiku), in one deck, **Kiku**: △ in the
overlay sends the queue, and so does **Send queued cards now** in the Overlay
app. Anki then syncs them to AnkiWeb as usual; the Vita never talks to
AnkiWeb and stores no login.

There is nothing else to set up: no deck, note type or field mapping to
choose. Mining is off by default: with it off, × and △ do nothing, and the
overlay starts no Anki thread, allocates no Anki memory and creates no
queue directory.

## Setup

On the computer:

1. Install AnkiConnect (add-on code `2055492159`). Tools → Add-ons →
   AnkiConnect → Config: set `"webBindAddress": "0.0.0.0"`, then restart Anki.
   AnkiConnect then listens on port 8765 of your computer's LAN address.
   Allow that port in the computer's firewall for your private network only.
2. Import Kiku's note type: download `Kiku_v2.1.0.apkg` from
   [Kiku's v2.1.0 release](https://github.com/youyoumu/kiku/releases/tag/v2.1.0)
   and open it in Anki (File → Import). Anki then has a **Kiku** note type
   (and a Kiku deck with two example cards).

On the Vita, open the **Overlay** app and its **Anki** page (Select, or tap
the tab):

1. **Card mining**: ON (Cross, or Left/Right).
2. **AnkiConnect address**: Cross opens the keyboard. Type your computer's IP
   address, such as `192.168.1.23` (`IP:port` if you changed AnkiConnect's
   port; `http://` and a trailing `/` are accepted and dropped), or `auto` to
   let the app search your local network. A new address is tested at once
   (for `auto`, that is the search: a few seconds).
3. **Test connection**: the result says whether AnkiConnect answered, whether
   Anki has the Kiku note type with the fields a card fills, and whether the
   Kiku deck exists (else the first card creates it). With `auto`, the test
   searches the network when the saved computer does not answer, and saves
   what it finds (`ux0:data/VitaJPOverlay/anki_host.txt`) for △.

These are `anki_mining` and `anki_host` in `ux0:data/VitaJPOverlay/config.ini`;
the app saves them as it saves its other pages' choices, keeping every other
line. The overlay reads them when it next opens.

## Use

Open the overlay, select a word and press ×. The status line shows
`Saving card…`, then `Saved for Anki`, and the entry shows `Queued for Anki`
(on every occurrence of that word in the sentence). Pressing × again on it
says `Already queued`. In a local character lookup, × saves the match at the
cursor. A line above the entry shows the queue: `Anki queue: 3 · △ send`.

To send the queue: press △ in the overlay, from any overlay screen (no word
needs to be shown), or **Send queued cards now** on the Overlay app's Anki
page, which shows how many cards are queued and its progress (Circle
stops it; the rest stay queued). Cards are sent in a fixed order, one at a
time. In the overlay the queue line shows `sending…` and the status
`Sending to Anki… 1 sent, 2 left`, then `Sent 3 to Anki`, and the queued
words get the green √.

| Status (overlay) | Meaning |
| --- | --- |
| `Saved for Anki` | The card is on the memory card |
| `Already queued` | The same card is queued already |
| `Already in Anki (this card was sent before)` | This exact card was sent and Anki had it ([duplicates](#duplicates)) |
| `Card not saved: cannot write …anki_queue (space?)` | The memory card refused the write; the card was not saved |
| `Card not saved: over 32 KiB of text` | The card's text exceeds a record's limit |
| `Sent 3 to Anki, 1 already there` | Done; duplicates were archived |
| `Sent 2 to Anki, 1 kept (unreadable, see log.txt)` | A damaged or edited record, or one whose screenshot is missing, stays queued |
| `1 sent; 2 kept: Anki offline` | Sending stopped; the rest stays queued. Also any AnkiConnect error and an HTTP error |
| `0 sent; 1 kept: Kiku note type not found in Anki: import Kiku (docs/anki.md)` | Anki has no Kiku note type ([setup](#setup)) |
| `0 sent; 1 kept: Anki not found: test the connection in the Overlay app` | `anki_host` is `auto`, and no search has found AnkiConnect yet or the computer it found does not answer now (Anki closed, or the computer has another address); the app's test searches again |
| `0 sent; 1 kept: audio: …` | The audio source failed; the card waits rather than going without audio |
| `Set Anki's address in the Overlay app; cards stay queued` | No computer set |
| `Anki: not enough memory…` | The Shell's memory reserve refused the job; nothing changed |

The app's page says the same in its own words, and with `log_file = on` both
write to `log.txt` (the app's lines start `overlay app:`): every storage call
that fails, with its error code, and why each card was kept.

## The Kiku note

The note type is `Kiku`, the deck `Kiku` (created with the first card if it
is missing), the tags `vita-jp-overlay` and the card's own
`vita_queue_<id>`. The field names are those of Kiku **v2.1.0**: tag `v2.1.0`,
commit [`b7fb283`](https://github.com/youyoumu/kiku/tree/b7fb283c29a35e6a74553e32427b2529bd9e617e)
(`packages/note/src/lib/types.ts`), the same 24 fields in its release's
`Kiku_v2.1.0.apkg` (SHA-256 `e6a3f1d37ee530e07c02ea7f6d64d98f5702e9691c2ccc512ea288b372cef045`).
Kiku is designed to be compatible with Lapis, and v2.1.0's
[installation guide](https://github.com/youyoumu/kiku/blob/b7fb283c29a35e6a74553e32427b2529bd9e617e/apps/docs/mds/installation.md)
gives the Yomitan value each field expects; a card fills them the same way:

| Kiku field | Filled with |
| --- | --- |
| `Expression` | The dictionary form |
| `ExpressionFurigana` | `言葉[ことば]`: the dictionary's own ruby where it has one (jiten), else the word's reading |
| `ExpressionReading` | The reading in kana |
| `ExpressionAudio` | The word's recording from `anki_audio_url`, looked up when sent (optional, empty by default) |
| `MainDefinition` | The entry shown, as Yomitan writes one dictionary's glossary: `<div class="yomitan-glossary"><ol><li data-dictionary="JMdict Plain">…</li></ol></div>`, so Kiku names its page after the dictionary |
| `Sentence` | The recognized text, the word in `<b>` |
| `SentenceFurigana` | The same with readings, when the lookup has them (else without; Kiku bolds the word from `Sentence`) |
| `Picture` | The card's screenshot, when it has one (none in this build, see below) |
| `Glossary` | Every dictionary's entry for the word, the shown one first, when there are two or more (Dictionaries shown above 1); Kiku leaves out the one `MainDefinition` has |
| `PitchPosition`, `PitchCategories` | Pitch accents, when the dictionary has them |
| `Frequency`, `FreqSort` | The frequency rank (with its source), when the dictionary has one |
| `MiscInfo` | The game's name and the local date and time of the × press |

The other Kiku fields (`SelectionText`, `DefinitionPicture`,
`SentenceTranslation`, `SentenceAudio`, `RelatedExpression`, `Hint` and the
`Is…Card` switches) stay empty. AnkiConnect ignores a field a note type
lacks; the connection test lists any of the filled ones a Kiku note type does
not have. The bundled dictionary (Daijirin4 → Sankoku8 → JMdict in one file)
has no frequency ranks, pitch accents or per-token readings: with it those
fields stay empty, and each entry's own dictionary (one of the three) names
its glossary item. jpdb and jiten lookups fill all of them.

## The queue on the memory card

Cards are files in `ux0:data/VitaJPOverlay/anki_queue/`:

- `<id>.json`: the card, readable UTF-8 JSON (below). The ID is the first 128
  bits of the record's SHA-256, so the same card saved twice in the same
  minute has the same name (the record holds the minute of the × press).
- `<id>.jpg`: its screenshot, for a card of an earlier build that took one.
- `<id>.duplicate.json`: a card Anki already had, kept for review.

A card is written to a temp file, flushed and renamed (after its screenshot,
if any), and the memory card's directory is synced before × reports it; an
interrupted save leaves no card or the complete one, never a partial one. A
card leaves the queue only after AnkiConnect returns its note ID. Each note
carries a stable `vita_queue_<id>` tag, and each send first asks Anki
(`findNotes`) for that tag: a card whose reply was lost, or that Anki got just
before a stop, is removed without being added twice. Files with other names,
including leftover `.tmp` files, are ignored and never sent. Editing a record
changes its checksum, so the card is kept and reported as unreadable rather
than sent. Delete a card's `.json` (and `.jpg`) to drop it.

### The record (version 3)

× records everything it knows about the card when it is pressed, so that a
card can be made into a note later, by any version that reads the record:

| Key | Content |
| --- | --- |
| `version` | 3 |
| `spelling`, `reading`, `furigana` | The word's dictionary form, its reading, and the dictionary's ruby of it (`来[く]る`; `null` when the dictionary has none) |
| `meanings`, `dictionary` | The shown entry's definitions as the dictionary gave them, and that dictionary's name (a local one's from its definition's first line, `[JMdict Plain]`; a service's name for jpdb, jiten or Hachidori) |
| `more` | The other dictionaries' entries for the word (Dictionaries shown above 1): each `{"dictionary", "spelling", "reading", "meanings"}`, at most 7 |
| `rank`, `source`, `pitch`, `pos` | Frequency rank (99999 = none) and its source, pitch-accent downsteps, parts of speech |
| `sentence`, `hl_start`, `hl_end` | The whole recognized text (every line of the textbox) and the word's UTF-8 byte offsets in it |
| `sentence_furigana`, `sf_hl_start`, `sf_hl_end` | The text with readings and the word's offsets in it, or `null` |
| `picture` | Whether `<id>.jpg` goes with it |
| `misc` | MiscInfo: `CLANNAD (2026-10-14 17:12)` |
| `game`, `title_id`, `platform` | The game's name, its title ID, and `vita` or `psp` (Adrenaline's shared title IDs) |
| `ocr_engine`, `ocr_model` | `vocr` or `lens`, and the model of the last recognition (`VOCR3_H15_w8.vocr`, or its fallback when that ran) |
| `ocr_confidence` | `null`: the recognizer reports no confidence yet; the key is there for when it does |
| `time` | When × was pressed, to the minute, as MiscInfo: seconds since 1970 (UTC) |

A record is at most 32 KiB; a card whose other dictionaries' entries would
make it larger is saved without them (log.txt says so). Records of version 2
(#49, v0.8.0-test.1: the same keys up to `misc`) and version 1 (#5) still load
and send as Kiku notes; a local definition's first line names its dictionary
there too. A version this build does not know is kept, not sent.

Not in the record, and why:

- **A screenshot of the textbox.** OCR reads the screen in the game's own
  process, a strip of grey rows at a time ([in-game capture](ingame-capture.md));
  a screenshot from those strips needs the game worker to encode it and a path
  to the memory card from the game (the game cannot see `ux0:`), which this
  change does not add. The old full-frame screenshot used the kernel's
  2,088,960 B capture buffer #74's console refused, so this build takes none
  (`shell/anki.c` is built with `VJO_ANKI_SCREENSHOT 0`). A card of an earlier
  development build that has a JPEG sends it from the Overlay app; △ sends it
  without the picture, as v0.8.0-test.1 does.
- **The previous and next dialogue.** The Shell keeps only the text on screen
  (one 4 KiB copy); the next line is not known when × is pressed. The record
  has the whole textbox, every line of it.

### Duplicates

AnkiConnect refuses a note whose first field (`Expression`) is already in the
Kiku deck. That card is renamed `<id>.duplicate.json` with its screenshot, and
sending goes on with the next card. Saving the same card again later reports
it as already in Anki.

## Settings that were removed

Earlier builds had a field mapping: `anki_deck`, `anki_note_type`,
`anki_tags` and the 13 `anki_field_*` keys. They are ignored without a
warning now, and the installer removes them from `config.ini`. `anki_mining`,
`anki_host` and the optional `anki_audio_url` remain; the last is set in
`config.ini` only (a Yomitan audio source URL with `{term}` and `{reading}`;
when a card is sent, the source's best recording is attached and Anki
downloads it).

## Memory and network

In SceShell (the overlay, `shell/anki.c`):

- One job at a time: counting the queue (the overlay opens), saving a card
  (×) or sending the queue (△). A job runs on a short-lived thread with a
  64 KiB stack, which the control thread joins once it ends. × while the
  queue is being sent says so and saves nothing; △ pressed during a save
  sends right after it.
- Saving or sending takes one bounded allocation from the Shell's Paf heap,
  through the same guard as the result workspace, which keeps the 1.5 MiB UI
  reserve: 128 KiB (`ANKI_TEXT_MEM`, the ledger's `VJO_BUDGET_PAF_ANKI`; the
  build checks they agree). It is freed before the thread ends. Counting
  only reads the directory, on the thread's stack, with no heap allocation.
  Nothing Anki-related stays allocated between jobs, and the queue is never
  read into memory whole: each step scans the directory once or reads one card.
- With Dictionaries shown above 1, × reads each other dictionary's entry for
  the word from its file into that arena, one at a time (at most 8,448 B each,
  `VJO_LOCAL_SELECTED_BUDGET`), after copying the 208-byte page locators under
  the view lock.
- △ sends to `anki_host`, or for `auto` to the address in `anki_host.txt`,
  read again on every send. SceShell does not search the network: the
  search (up to 254 hosts in batches of 32 sockets) is the Overlay app's.
- The note is copied out of the shown lookup under the view lock before
  anything is written; the result memory is never read during file or
  network I/O. If memory is refused, a write fails or a request fails, only
  the Anki status line changes: the recognized text, its lookup and the
  overlay stay usable, the queued cards stay, and × or △ can be pressed again.
- AnkiConnect replies are limited to 64 KiB, audio source replies to 32 KiB.
  Connecting to Anki gives up after 3 seconds; an audio source has a 5-second
  read timeout.
- Stopping the Shell plugin cancels a job in progress and waits for its
  thread before freeing anything; a card being sent stays queued.

The Overlay app is its own process, not one of the plugins, so it is not in
the plugins' [memory ledger](memory-ledger.md): its Anki page holds a static
512 KiB arena for a send (a card with a 192 KiB screenshot, its request and
the reply), SceNet's 256 KiB pool (loaded only when the page first tests or
sends) and a 128 KiB stack for the one worker thread that does the network
work while the page keeps drawing.

Measured on the host with the actual `shell/anki.c`, `shell/anki_queue.c`
(`ctest -R shell_anki -V`, "measure:" lines): a save with two other
dictionaries' entries read from their files peaked at 12,136 of 131,072 bytes;
a text card with audio sent at 10,400; 100 text cards in one send at 10,408. The
host build of the screenshot path (`vjo-shell-anki-tests`): saving a card
with its 62,686-byte JPEG peaked at 133,712 of 393,216 bytes, sending it at
77,200. The app's send of the four version-2 records of the v0.8.0-test.1
console's queue peaked at 39,960 bytes on the host (`test_anki_app`). These
are host measurements; a physical Vita has not run this code yet.

## Security

AnkiConnect's API is plain HTTP without authentication. Anyone on the same
network can use it while it listens on `0.0.0.0`. Use it only on a trusted
home network, keep the firewall rule limited to your private network, and do
not forward port 8765 to the Internet. The overlay and the app do not send
AnkiConnect's optional `apiKey`; leave it unset.

Notes go only to `anki_host` (or the computer the app's search found), and
only when you press △ or Send in the app. The search connects to port 8765 of
each address of your /24 network and asks the ones that answer for
AnkiConnect's version. The optional `anki_audio_url` is the one other address
contacted, also only when sending: the word and its reading are sent to
whatever source you set there. It is empty, and so never contacted, by
default. The queue's files hold the words, sentences and game names you
saved; anyone with access to the memory card can read them.

## Not included

No AnkiWeb or self-hosted sync server and no login (issue
[#29](https://github.com/bee-san/vita-jp-overlay/issues/29) and draft
[#45](https://github.com/bee-san/vita-jp-overlay/pull/45) cover those).
Cards are not sent automatically: only △ and the app's Send contact Anki.
