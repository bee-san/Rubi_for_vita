# SC3 charsets

The glyph tables of Rubi's MAGES/5pb. SC3 hooks ([docs/native-hooks.md](../../docs/native-hooks.md#sc3-text)):
for each game, which character each glyph index of its font draws. An SC3 string holds glyph indices, not
characters, so the game worker decodes a hooked page with its game's table. The files hold no script text.

Each `<title>.sc3` is written by `tools/sc3_charset.py --farm` from the bee-san/psandvita farm's
`hooks/vita/<title>/sc3-charset.json` (schema `vn-sc3-charset/1`) and the module NID of its `rubi-hook.json`; the
format is in `core/hook_sc3.h`, and `core/hook_sc3_pins.inc` pins each file's size and SHA-256 (the worker accepts
no other file; ctest `hook_sc3_pins` checks the pins against these files). The release installs them to
`ux0:data/VitaJPOverlay/hooks/`.

| File | Game | Glyphs (named) | Bytes | SHA-256 |
| --- | --- | ---: | ---: | --- |
| `PCSG00146.sc3` | STEINS;GATE 比翼恋理のだーりん | 2,666 (2,528) | 5,364 | `278637d67f43caaee9b9008daf6a102e2c32fccc4cb83dade0c60c04a069399e` |
| `PCSG00352.sc3` | ROBOTICS;NOTES ELITE | 3,200 (2,688) | 6,432 | `a8ebe56944198a4df3a7f1c94daecf1559ebff93067e740e11d0061a5d1e98cc` |
| `PCSG00555.sc3` | CHAOS;CHILD | 8,000 (7,345) | 16,032 | `c95d566c64190cf97e58abc2592654dee32b1fbfcd547b9ab81b2ca8c486ee7f` |

Farm inputs (SHA-256 of `sc3-charset.json`, the farm's commit `56f4f88`): PCSG00146 `fa7b2387…61b3`, PCSG00352
`d581ec7f…e5cd4`, PCSG00555 `ee776425…4c41`. A glyph the table cannot name is 0 in the file: the decoder drops it,
it never guesses one.

## Where each table comes from

- **PCSG00146.** Of indices 0x000-0x33F (symbols, Latin, kana), 715 are the game's own UTF-16 tables in its eboot
  (segment 1 at 0x8112F6CA and 0x8112F776), which the game uses for its system strings, two (a placeholder slot used
  as a full-width space, and `&` in emoticons) the PC texthook's below, and 115 unknown. Indices 0x340-0xA69
  are the kanji in the order the story scripts first use them; that order was aligned, by dynamic programming over
  the first-appearance events of the game's own scripts, with the charset of the PC texthook for *STEINS;GATE My
  Darling's Embrace* in [0xDC00/scripts](https://github.com/0xDC00/scripts) (`41af798`, MIT; the script credits
  "@SofiaScarlet" for its table). Five glyphs were inferred from their contexts and nine placed in the PC list's
  preseed order; 23 stay unknown. Four entries were corrected on 2026-10-25 from the game's decoded font atlas
  (77 `_`, 269 `²`, 270 `♥`, 271 `©`).
- **PCSG00352.** The first 3,200 characters of [CommitteeOfZero/sc3tools](https://github.com/CommitteeOfZero/sc3tools)'
  `resources/rn` table (MIT, Committee of Zero), as 0xDC00/scripts' PC texthook for *ROBOTICS;NOTES ELITE* carries it
  (MIT); its private-use placeholders and filler slots are unknown (512). That table is the English patch's font: 14
  slots were restored to the Japanese font's glyphs (「」『』 and others; event 33 of the farm's run draws 「」), and
  three corrected from the font atlas on 2026-10-25 (269 `²`, 270 `♥`, 271 `©`).
- **PCSG00555.** 0xDC00/scripts' PC texthook for *CHAOS;CHILD* (MIT), which is based on sc3tools' `sg0` resources
  (MIT); its private-use placeholders and filler slots are unknown (655). Every index was then checked against the
  Vita game's own font atlas (`USRDIR/system.mpk`, `font.gxt`: 64 × 125 cells of 24 px), cell by cell for indices
  0-703 and by image correlation for every named cell, and 16 entries were corrected.

The two MIT notices are in [../licenses/0xDC00-scripts-MIT.txt](../licenses/0xDC00-scripts-MIT.txt) and
[../licenses/CommitteeOfZero-sc3tools-MIT.txt](../licenses/CommitteeOfZero-sc3tools-MIT.txt). The order of the
glyphs is each game's own font's.

## Checks (Vita3K only, not a console)

The farm's approved pages (an AI reviewer compared each with the frame the game drew in Vita3K), the bytes its hook
read decoded with these files by the farm's decoder:

| Game | Single-hit pages | Decoded to the page's text | The others |
| --- | ---: | ---: | --- |
| PCSG00146 | 1,496 | 1,493 | 3 pages read `⑯` before glyph 269 was corrected; the screen draws `²` |
| PCSG00352 | 605 | 586 | 19 pages' text was made before the 14 slots' restoration: `“”` and `––` where the screen draws `「」` and `――` |
| PCSG00555 | 390 | 390 | |

Statically, the farm decoded the games' script strings with the same tables: 45,697 of 45,698 (PCSG00352) and
46,496 of 46,496 (PCSG00555) decode with no unknown glyph. `host/tests/test_hook_sc3.c` and
`host/tests/test_hooks_engine.c` run a few short pages of each game (`host/tests/fixtures/hook_sc3_farm.inc`,
`tools/sc3_charset.py --fixtures`) through Rubi's own reader and decoder.
