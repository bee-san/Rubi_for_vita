# Per-game names

In a game Rubi knows, that game's characters come first: in CLANNAD, 渚 is Furukawa Nagisa
(古河 渚), with the word (a beach) as the next L/R page. In any other game 渚 is the word first, as
before. The names are VNDB's, from the same snapshot as the bundled
[PSP/Vita names dictionary](guide.md#settings-and-dictionaries).

## Which games

A game is known by the ID the console reports, never by its title:

- a Vita game by its title ID (`PCSG00415`);
- a PSP game started from an Adrenaline Bubble Manager bubble by its disc ID (`ULJM05338`), which
  the Rubi app reads from the bubble's game when it lists it (`settings/psp.c`). Adrenaline itself
  reports no game, so a PSP game started from Adrenaline's own menu has no per-game names.

The IDs come from [bee-san/psp_and_vita_vndb_name_dict](https://github.com/bee-san/psp_and_vita_vndb_name_dict)
(`VNDB-Names-Games.zip`): VNDB's catalog numbers and PlayStation Store links, and
[Vita3K's compatibility list](https://github.com/Vita3K/compatibility) for Vita games whose VNDB
releases carry none (a Vita card's catalog number is its box code, `VLJM-35xxx`). In the pinned
archive, 526 of the 751 PSP games (85% of their characters) and 459 of the 618 Vita games (79%)
have an ID. A game without one keeps the combined names dictionary only, as before.

## In the overlay

At a game's start the Shell looks the ID up in `ux0:data/VitaJPOverlay/dictionaries/vndb-games.vjgames`
(a few 20-byte reads; `log.txt` says `names for PCSG00415: 1 (11649 B)`: 1 for this game's names, 0 for none, -1 when the
pack cannot be read). The
game's names are then searched first, before `local_dictionaries`, as one more dictionary: the
popup labels them with the game's title, Anki cards name them `VNDB Names — this game`, and
**Dictionaries shown** counts them. They take no setting; with eight dictionaries configured there
is no room for them and none are searched.

Within an entry, a character whose own name has the spelling comes before one known by it as an
alias, then main before side characters: 岡崎 is Okazaki Tomoya first. Keys that are among the
word dictionary's 3,000 most frequent words (86, such as 一, 光 and 先生) are left out of every
game's names (`third_party/vndb-names/games-common-words.txt`): in Hakuouki, 一 is the word "one"
and 斎藤一 is Saitou Hajime.

## The pack

`tools/prepare_vndb.py` builds it from the vendored archive, pinned in `docs/vndb-names.json`
(`per_game`): each game set (a bundle's ID names all its games) becomes a small format 1
dictionary, and the pack is a 64-byte header (`VJGAMES1`, then version 1, the header size, the ID count and the
record size 20, each a u32), the title IDs sorted bytewise (12 bytes NUL-padded, a dictionary's u32
offset and u32 length) and the dictionaries. A game's dictionary is read as a
window of the file (`core/local_dict.c` `vjo_game_names_find`, `open_game`). It is 9,705,406 B for
851 game sets and 1,335 IDs; the Rubi app's Install copies it like the other names file.

## Memory

No allocation: the game's names use one of the lookup's existing eight dictionary slots and its
page cache. The Shell keeps the window (8 bytes) between the game's start and its end.
