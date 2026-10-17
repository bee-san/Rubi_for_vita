# Lapis note type (vendored)

[Lapis](https://github.com/donkuri/lapis) by Ruri, kuri, itokatsu and
contributors: the Anki note type the Rubi app's `.apkg` export uses
(docs/anki.md). GPL-3.0, the same licence as this project: [LICENSE](LICENSE)
is Lapis's own copy.

- Release `1.7.0`: tag `1.7.0`, commit `38238d0b1c072fe7d7cd121e1e4c838a959f8d6b`
  (2025-11-24); its release asset `Lapis.apkg` (published 2026-01-20) has
  SHA-256 `85984f8c6f1263e2882c364bbbfdee4bce9e1e03f053a0a5b0ccc6cd90de0088`.
- `front.html`, `back.html` and `styling.css` are that tag's `src/` files,
  byte for byte. `tools/gen_lapis.py` checks their SHA-256 and turns them into
  `core/lapis_notetype.inc`, the note type's schema-11 JSON.
- The note type in the release's `Lapis.apkg` has the ID `1667218449922`, the
  name `Lapis`, the 22 fields below and one template, `Mining`. Its templates
  and styling are these files without their final newline (Anki keeps none),
  except that two spaces in `back.html`'s script are U+00A0 in the tag and
  plain spaces in the release (JavaScript whitespace either way). The field
  fonts, sizes and the collapsed `Glossary` are the release's.

Fields: Expression, ExpressionFurigana, ExpressionReading, ExpressionAudio,
SelectionText, MainDefinition, DefinitionPicture, Sentence, SentenceFurigana,
SentenceAudio, Picture, Glossary, Hint, IsWordAndSentenceCard, IsClickCard,
IsSentenceCard, IsAudioCard, PitchPosition, PitchCategories, Frequency,
FreqSort, MiscInfo.

To update: copy the new tag's `src/` files here, update the pins and the field
table in `tools/gen_lapis.py`, run it, and check the import tests
(`host/tests/test_apkg.py`).
