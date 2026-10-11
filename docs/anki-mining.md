# Local Anki mining

Mining is opt-in. In JP Overlay Settings, open **Anki** and enable **Mine
selected words**. Reopen the overlay to load the choice, wait for a definition,
then press Cross to save that selected entry. Disabled games retain their
existing input policy.

This build stores text on the Vita. **AnkiWeb synchronization is not implemented
yet.** A saved login has not been checked against AnkiWeb, and "saved locally"
does not mean that a card was created or uploaded. The Settings page displays
the number of saved records and keeps this distinction visible.

The native engine investigation and remaining sync work are tracked in
[issue #29](https://github.com/bee-san/vita-jp-overlay/issues/29) and
[the feasibility report](anki-native-feasibility.md). The intended sync client
runs as a standalone Vita app; no PC companion is required by that design.

## Files and settings

- `ux0:data/VitaJPOverlay/anki_queue.jsonl`: saved text records. Back up this
  file to export the queue; this build does not delete or mark records sent.
- `ux0:data/VitaJPOverlay/anki_account.json`: username/password entered with
  the Vita keyboard. These are stored as ordinary readable JSON on-device,
  separately from configuration and the queue. Password input is masked.
  Square on the Anki page removes the main file, backup and temporary copy.
- `ux0:data/VitaJPOverlay/config.ini`: `anki_mining = on | off` (off by
  default), plus destination metadata. Mining changes preserve other settings.

Destination metadata is captured when each word is saved:

```ini
anki_deck = Default
anki_note_type = VitaJPOverlay
anki_tags = vita-jp-overlay
anki_field_word = Expression
anki_field_reading = ExpressionReading
anki_field_furigana = ExpressionFurigana
anki_field_definition = MainDefinition
anki_field_sentence = Sentence
anki_field_source = Source
```

The app does not create that note type yet. These names describe the intended
fields for the future sync importer. The source is the game title ID; resolving
the human-readable title and optional media remains future work. Frequency
ranks, audio and screenshots are not queued.

AnkiConnect and its discovery/upload worker have been removed. Legacy
`anki_host`, `anki_audio_url`, `anki_field_audio`, `anki_field_picture` and
`anki_field_frequency` keys are ignored. Historical queue directories are
left alone; their per-record format is not automatically imported into the
new JSONL file.

## Durability and memory

A save uses the existing lookup worker and borrows its result arena. It adds
no persistent Shell memory allocation, extra worker thread or OCR capture.
The selected result remains alive until the accepted save finishes, including
when the game closes. A stale or busy selection is rejected before writing.
If temporary encoding space is insufficient, the definition remains visible
and the user can retry.

Reads use small fixed buffers rather than loading the queue into RAM. Each
text payload is limited to 32 KiB and the whole file to 16 MiB. A SHA-256 based
record ID detects exact repeated saves and damaged complete records. A torn
final append is removed before the next save; completed records are retained.
Writes handle short I/O and report success only after file/mount flushes. A
retry after an unconfirmed flush does not append a duplicate record.

Each line wraps a version 2 note object:

```json
{"id":"32 lowercase hex characters","note":{"version":2,"spelling":"猫","reading":"ねこ","sentence":"猫がいる。","hl_start":0,"hl_end":3,"meanings":["cat"],"source_id":"PCSG00415","deck":"Default","note_type":"VitaJPOverlay","tags":"vita-jp-overlay","fields":{"word":"Expression","reading":"ExpressionReading","furigana":"ExpressionFurigana","definition":"MainDefinition","sentence":"Sentence","source":"Source"}}}
```

The example ID is a placeholder. Real IDs are the first 16 bytes of SHA-256
over the exact UTF-8 bytes of the `note` object, represented as lowercase hex.
Highlight offsets are UTF-8 byte offsets. Destination and source metadata are
part of identity; changing them can create a distinct record for the same word.
No credentials appear in the queue.

## Frequency removal

Frequency lookup/display was removed in a separate commit,
`277aa9c` (`Remove frequency ranks from dictionary lookup and display`). Its
patch can be reviewed with `git show 277aa9c` when restoring the feature later.
Subsequent AnkiConnect removal also removes the obsolete frequency note field;
restoration should keep that newer configuration and queue format intact.
