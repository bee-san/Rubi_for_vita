# Anki card mining with an offline queue

With `anki_mining = on`, × in the open overlay saves the shown dictionary
entry as a card in a queue on the memory card. Saving needs no network: it
works with Wi-Fi off, Anki closed or `anki_host` unset, and the queue
survives restarts. △ sends the queued cards to
[AnkiConnect](https://ankiweb.net/shared/info/2055492159) running in Anki on
a computer on your local network. Anki then syncs them to AnkiWeb as usual;
the Vita never talks to AnkiWeb and stores no login.

Mining is off by default: with `anki_mining = off`, × and △ do nothing, and
the overlay starts no Anki thread, allocates no Anki memory and creates no
queue directory.

## Setup

1. In `ux0:data/VitaJPOverlay/config.ini`, set `anki_mining = on`. × now
   saves cards; nothing else is needed to collect them.
2. To send them: in Anki on your computer, install AnkiConnect (add-on code
   `2055492159`). Tools → Add-ons → AnkiConnect → Config: set
   `"webBindAddress": "0.0.0.0"`, then restart Anki. AnkiConnect then listens
   on port 8765 of your computer's LAN address. Allow that port in the
   computer's firewall for your private network only.
3. Set `anki_host` to your computer's IP address, for example
   `anki_host = 192.168.1.23`, or `IP:port` if you changed AnkiConnect's
   port. `anki_host = auto` searches your local /24 network when you first
   send, and remembers the computer in `ux0:data/VitaJPOverlay/anki_host.txt`.
4. Set `anki_deck` (created if missing) and, for a note type other than
   [Lapis](https://github.com/donkuri/lapis), `anki_note_type` and the
   `anki_field_*` keys to its field names. An empty `anki_field_*` value leaves
   that item out. Duplicates are checked in the deck on the note type's first
   field, so map the word to that field.
5. Optional, word audio: set `anki_audio_url` to a Yomitan custom audio source
   URL with `{term}` and `{reading}`. When a card is sent, the Vita asks that
   source for the word's best recording and Anki downloads the file.

Settings are reloaded each time the overlay opens. Deck, note type, fields,
tags and audio are applied when △ sends, so you can fix them in `config.ini`
and send again; queued cards are not affected. The Overlay settings app
does not edit these settings: they are free-form text (address, deck, field
names), and the app has no text entry yet.

## Use

Open the overlay, select a word and press ×. The status line shows
`Saving card…`, then `Saved for Anki`, and the entry shows `Queued for Anki`
(on every occurrence of that word in the sentence). Pressing × again on it
says `Already queued`. In a local character lookup, × saves the match at the
cursor. A line above the entry shows the queue: `Anki queue: 3 · △ send`.

Press △ to send the queue, from any overlay screen (no word needs to be
shown). The queue line shows `sending…` and the status `Sending to Anki… 1
sent, 2 left`, then `Sent 3 to Anki`, and the queued words get the green √.
Cards are sent in a fixed order, one at a time.

| Status | Meaning |
| --- | --- |
| `Saved for Anki` | The card is on the memory card |
| `Saved for Anki without a screenshot (…)` | The capture failed, or its JPEG was over 192 KiB; the card has no picture |
| `Already queued` | The same card (word, sentence, definitions) is queued already |
| `Already in Anki (this card was sent before)` | This exact card was sent and Anki had it ([duplicates](#duplicates)) |
| `Card not saved: cannot write …anki_queue (space?)` | The memory card refused the write; the card was not saved |
| `Card not saved: over 32 KiB of text` | The card's text exceeds a record's limit |
| `Sent 3 to Anki, 1 already there` | Done; duplicates were archived |
| `Sent 2 to Anki, 1 kept (unreadable, see log.txt)` | A damaged or edited record, or one whose screenshot is missing, stays queued |
| `1 sent; 2 kept: Anki offline` | Sending stopped; the rest stays queued. Also any AnkiConnect error, an HTTP error or a missing note type |
| `0 sent; 1 kept: audio: …` | The audio source failed; the card waits rather than going without audio. Empty `anki_audio_url` to send without |
| `Set anki_host in config.ini to send; cards stay queued` | No computer configured |
| `Anki: not enough memory…` | The Shell's memory reserve refused the job; nothing changed |

With `log_file = on`, `log.txt` records every storage call that fails, with
its error code, and why each card was kept.

## The queue on the memory card

Cards are files in `ux0:data/VitaJPOverlay/anki_queue/`:

- `<id>.json`: the card, readable UTF-8 JSON with the word, its reading, the
  sentence and the word's place in it, the definitions, the frequency rank,
  the sentence with readings, pitch accents, parts of speech, and the game's
  name and date (MiscInfo). The ID is the first 128 bits of the record's
  SHA-256, so the same card saved twice has the same name.
- `<id>.jpg`: its screenshot, when `anki_field_picture` was set at ×.
- `<id>.duplicate.json`: a card Anki already had, kept with its screenshot
  for review.

A card is written to a temp file, flushed and renamed, after its screenshot,
and the memory card's directory is synced before × reports it; an interrupted
save leaves no card or the complete one, never a partial one. A card leaves
the queue only after AnkiConnect returns its note ID. Each note carries a
stable `vita_queue_<id>` tag, and each send first asks Anki (`findNotes`)
for that tag: a card whose reply was lost, or that Anki got just before a
stop, is removed without being added twice. Files with other names,
including leftover `.tmp` files, are ignored and never sent. Records written
by the first offline queue (#5) are still sent.

Editing a record changes its checksum, so the card is kept and reported as
unreadable rather than sent. Delete a card's `.json` (and `.jpg`) to drop it.

### Duplicates

AnkiConnect refuses a note whose first field is already in the deck. That
card is renamed `<id>.duplicate.json` with its screenshot, and sending goes
on with the next card. Saving the same card again later reports it as
already in Anki.

## Note contents

| Key | Lapis default | Content |
| --- | --- | --- |
| `anki_field_word` | `Expression` | The dictionary form |
| `anki_field_reading` | `ExpressionReading` | Its reading in kana |
| `anki_field_furigana` | `ExpressionFurigana` | `言葉[ことば]` |
| `anki_field_definition` | `MainDefinition` | The definitions, as a numbered list |
| `anki_field_sentence` | `Sentence` | The recognized sentence, the word in bold |
| `anki_field_picture` | `Picture` | A JPEG of the whole game frame at the × press |
| `anki_field_frequency` | `FreqSort` | The frequency rank, when the dictionary has one |
| `anki_field_audio` | `ExpressionAudio` | The word's recording from `anki_audio_url`, looked up when sent |
| `anki_field_sentence_furigana` | `SentenceFurigana` | The sentence with readings, when the lookup has them |
| `anki_field_pitch_position` | `PitchPosition` | Pitch accent downstep, when the dictionary has it |
| `anki_field_pitch_categories` | `PitchCategories` | `heiban`, `atamadaka`, ... |
| `anki_field_frequency_list` | `Frequency` | The rank with its source's name |
| `anki_field_misc_info` | `MiscInfo` | The game's name and the local date and time of the × press |

The field formats are upstream's
([Dartv/vita-jp-overlay](https://github.com/Dartv/vita-jp-overlay) #1, #4
and #6). The bundled local JMdict has no frequency ranks, pitch accents or
per-token readings: with it, the frequency and pitch fields stay empty and
`SentenceFurigana` is the sentence with the word in bold but no readings. The
word's own furigana comes from its reading. jpdb and jiten lookups fill all
of them. A card keeps every value whatever the field mapping, so a field
added before sending is filled too; only the screenshot depends on
`anki_field_picture` being set at ×.

## Memory and network

- One job at a time: counting the queue (the overlay opens), saving a card
  (×) or sending the queue (△). A job runs on a short-lived thread with a
  64 KiB stack, which the control thread joins once it ends. × while the
  queue is being sent says so and saves nothing; △ pressed during a save
  sends right after it.
- Saving or sending takes one bounded allocation from the Shell's Paf heap,
  through the same guard as the result workspace, which keeps the 1.5 MiB UI
  reserve: 128 KiB without a picture field, 384 KiB with
  `anki_field_picture` set. It is freed before the thread ends. Counting
  only reads the directory, on the thread's stack, with no heap allocation.
  Nothing Anki-related stays
  allocated between jobs, and the queue is never read into memory whole:
  each step scans the directory once or reads one card.
- The screenshot is one single-pass full-frame kernel capture, the same as a
  full-screen OCR capture: the kernel allocates the raw frame (2 MiB of kernel
  memory at 960×544, not game memory) and frees it when the encoder reads the
  last row. The JPEG is at most 192 KiB (`VJO_ANKI_PICTURE_MAX`); a larger
  frame is saved without its screenshot. A card's text is at most 32 KiB.
- When sending, the screenshot is read back and streamed to AnkiConnect as
  base64 in 4 KiB pieces, never encoded whole. Before anything is sent, the
  request is built once as a dry run: a card whose text does not fit beside
  its screenshot goes without the screenshot.
- The note is copied out of the shown lookup under the view lock before
  anything is written; the result memory is never read during file or
  network I/O.
- AnkiConnect replies are limited to 64 KiB, audio source replies to 32 KiB.
  Connecting to Anki gives up after 3 seconds; an audio source has a 5-second
  read timeout.
- If memory is refused, a write fails or a request fails, only the Anki
  status line changes. The recognized text, its lookup and the overlay stay
  usable, the queued cards stay, and × or △ can be pressed again.
- Stopping the Shell plugin cancels a job in progress and waits for its
  thread before freeing anything; a card being sent stays queued.

Measured on the host with the actual `shell/anki.c`, `shell/anki_queue.c` and
`shell/capture.c` (`ctest -R shell_anki -V`, "measure:" lines) on a synthetic
960×544 dialogue frame: saving a card with its 62,686-byte JPEG peaked at
134,752 of 393,216 bytes; sending it at 77,480 bytes; a text card with audio
at 8,632 of 131,072 bytes; 100 text cards in one send at 10,688 bytes; a
noise frame whose JPEG exceeds the cap at 383,624 bytes, inside the arena.
These are host measurements; a physical Vita has not run this code yet.

## Security

AnkiConnect's API is plain HTTP without authentication. Anyone on the same
network can use it while it listens on `0.0.0.0`. Use it only on a trusted
home network, keep the firewall rule limited to your private network, and do
not forward port 8765 to the Internet. The overlay does not send
AnkiConnect's optional `apiKey`; leave it unset.

Notes go only to `anki_host`, and only when you press △. The optional
`anki_audio_url` is the one other address contacted, also only when sending:
the word and its reading are sent to whatever source you set there (a
computer on your network, or an Internet service). It is empty, and so never
contacted, by default. The queue's files hold the words, sentences and
screenshots you saved; anyone with access to the memory card can read them.

## Not included

No AnkiWeb or self-hosted sync server and no login (issue
[#29](https://github.com/bee-san/vita-jp-overlay/issues/29) and draft
[#45](https://github.com/bee-san/vita-jp-overlay/pull/45) cover those).
Cards are not sent automatically: only △ contacts Anki.
