# Anki card mining over your local network

With `anki_host` set, × in the open overlay sends the shown dictionary entry
to [AnkiConnect](https://ankiweb.net/shared/info/2055492159) running in Anki on
a computer on your local network. The note goes straight into your collection
there; Anki syncs it to AnkiWeb as usual. The Vita never talks to AnkiWeb,
keeps no offline queue and stores no login.

Mining is off by default: with `anki_host` empty, × does nothing and the
overlay starts no Anki thread and allocates no Anki memory.

## Setup

1. In Anki on your computer, install AnkiConnect (add-on code `2055492159`).
2. Tools → Add-ons → AnkiConnect → Config: set `"webBindAddress": "0.0.0.0"`,
   then restart Anki. AnkiConnect then listens on port 8765 of your
   computer's LAN address. Allow that port in the computer's firewall for
   your private network only.
3. In `ux0:data/VitaJPOverlay/config.ini`, set `anki_host` to your computer's
   IP address, for example `anki_host = 192.168.1.23`, or `IP:port` if you
   changed AnkiConnect's port. `anki_host = auto` searches your local /24
   network once and remembers the computer in
   `ux0:data/VitaJPOverlay/anki_host.txt`.
4. Set `anki_deck` (created if missing) and, for a note type other than
   [Lapis](https://github.com/donkuri/lapis), `anki_note_type` and the
   `anki_field_*` keys to its field names. An empty `anki_field_*` value leaves
   that item out. Duplicates are checked in the deck on the note type's first
   field, so map the word to that field.
5. Optional, word audio: set `anki_audio_url` to a Yomitan custom audio source
   URL with `{term}` and `{reading}`. The Vita asks that source for the word's
   best recording and Anki downloads the file.

Settings are reloaded each time the overlay opens. The JP Overlay Settings app
does not edit them: they are free-form text (address, deck, field names), and
the app has no text entry yet.

## Use

Open the overlay, select a word, press ×. The status line shows
`Adding to Anki…`, then `Added to Anki` and a green √ after the entry (on every
occurrence of that word in the sentence), or why nothing was added:
`Anki offline`, `Already in Anki`, `Note type "…" not found in Anki`, an HTTP
error, or `not enough memory`. A missing screenshot or recording is named in
the status; the note is still added. While a card is being sent, another × is
ignored. In a local character lookup, × sends the match at the cursor.
Triangle is unused: there is no queue to send.

Lookups are not checked against your deck in advance, which would cost a
request per lookup: a word already in the deck is marked √ when × reports
`Already in Anki`.

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
| `anki_field_audio` | `ExpressionAudio` | The word's recording from `anki_audio_url` |
| `anki_field_sentence_furigana` | `SentenceFurigana` | The sentence with readings, when the lookup has them |
| `anki_field_pitch_position` | `PitchPosition` | Pitch accent downstep, when the dictionary has it |
| `anki_field_pitch_categories` | `PitchCategories` | `heiban`, `atamadaka`, ... |
| `anki_field_frequency_list` | `Frequency` | The rank with its source's name |
| `anki_field_misc_info` | `MiscInfo` | The game's name and the local date and time |

The field formats are upstream's
([Dartv/vita-jp-overlay](https://github.com/Dartv/vita-jp-overlay) #1, #4
and #6). The bundled local JMdict has no frequency ranks, pitch accents or
per-token readings: with it, the frequency and pitch fields stay empty and
`SentenceFurigana` is the sentence with the word in bold but no readings. The
word's own furigana comes from its reading. jpdb and jiten lookups fill all
of them.

## Memory and network

- One card at a time. × creates a short-lived thread with a 64 KiB stack. It
  takes one bounded allocation from the Shell's Paf heap, through the same
  guard as the result workspace, which keeps the 1.5 MiB UI reserve:
  128 KiB for a text-only note, 384 KiB when `anki_field_picture` is set. The
  allocation is freed before the thread ends; the control thread joins the
  thread on its next wakeup. Nothing Anki-related stays allocated between
  cards.
- The screenshot is one single-pass full-frame kernel capture, the same as a
  full-screen OCR capture: the kernel allocates the raw frame (2 MiB of kernel
  memory at 960×544, not game memory) and frees it when the encoder reads the
  last row. The JPEG is at most 192 KiB (`VJO_ANKI_PICTURE_MAX`); a larger
  frame is sent without its screenshot. It is streamed to AnkiConnect as
  base64 in 4 KiB pieces, never encoded whole. Before anything is sent, the
  request is built once as a dry run: a note whose text does not fit beside
  its screenshot is sent without the screenshot.
- The note is copied out of the shown lookup under the view lock before any
  request; the result memory is never read during the network calls.
- AnkiConnect replies are limited to 64 KiB, audio source replies to 32 KiB.
  Connecting to Anki gives up after 3 seconds; an audio source has a 5-second
  read timeout.
- If memory is refused (`Anki: not enough memory, card not sent`), or a
  request fails, only the Anki status line changes. The recognized text, its
  lookup and the overlay stay usable, and × can be pressed again.
- Stopping the Shell plugin cancels a card in progress and waits for its
  thread before freeing anything.

Measured on the host with the actual `shell/anki.c` and `shell/capture.c`
(`ctest -R shell_anki -V`, "measure:" lines) on a synthetic 960×544 dialogue
frame: a picture card's arena peaked at 134,744 of 393,216 bytes with a
62,688-byte JPEG; a text card with audio at 7,736 of 131,072 bytes; a noise
frame whose JPEG exceeds the cap at 383,616 bytes, inside the arena. These are
host measurements; a physical Vita has not run this code yet.

## Security

AnkiConnect's API is plain HTTP without authentication. Anyone on the same
network can use it while it listens on `0.0.0.0`. Use it only on a trusted
home network, keep the firewall rule limited to your private network, and do
not forward port 8765 to the Internet. The overlay does not send
AnkiConnect's optional `apiKey`; leave it unset.

Notes go only to `anki_host`. The optional `anki_audio_url` is the one other
address contacted: the word and its reading are sent to whatever source you
set there (a computer on your network, or an Internet service). It is empty,
and so never contacted, by default.

## Not included

No AnkiWeb or self-hosted sync server, no login, and no offline queue
(issue [#29](https://github.com/bee-san/vita-jp-overlay/issues/29) and draft
[#45](https://github.com/bee-san/vita-jp-overlay/pull/45) cover those). The
older offline queue code (`shell/anki_queue.c`) is not built.
