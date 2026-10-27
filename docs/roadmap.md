# Roadmap

What is left to build in Rubi, ranked. This was issue [#310](https://github.com/bee-san/Rubi_for_vita/issues/310)
(2026-10-26); its top five plans are built ([below](#built-the-top-five)) and the issue is closed, so the ranking
of what remains lives here. The list draws on four sources:

- Rubi's issues and their history;
- [GameSentenceMiner](https://github.com/bpwhelan/GameSentenceMiner/tree/5190b613f5ff2c70bfe505377b297854aacf6b27)
  and its features (source of 2026-10-25, release v2026.10.1);
- [Yomitan](https://github.com/yomidevs/yomitan/tree/d3c54c10264673eca0d66b970ef74cd5c8b048e3) and its features
  (master of 2026-10-22, the 26.10.22.0 pre-release; the latest stable release then was 26.10.12.0);
- the Vita-side tools: Anki for Vita, Hachidori, jmdict-vita and others.

Nothing below has run on a console. Costs are estimates.

## How it is ranked

| Factor | Scale |
|---|---|
| **Value** | 1–5: how much it helps the read → look up → mine → review loop, how many users it reaches, and the evidence that people want it. A request closed on 10-26 counts at most 2.5. |
| **Cost** | S = 1 (a few days, 1–2 PRs), M = 2 (1–2 weeks), L = 3 (more). S–M = 1.5 and M–L = 2.5. The cost is for the whole item; its PRs overlap. |
| **Risk**, by where the code runs | The Rubi app or a PC: 0, since the game and SceShell are untouched. The game worker: 1. The Shell: 2, or 3 when it needs more Shell bytes than are free. |

**Score** = 2 × value − cost − risk. An item that other work in progress covers goes after the ones that can start
now.

## Where the room is

| Where | Room | Notes |
|---|---|---|
| Normal Shell | tens of bytes of code room at most ([memory-ledger.md](memory-ledger.md); #314's stage B measured 36 B on main `030c7ea`) | [AGENTS.md](../AGENTS.md) rule 1 lets its pages only shrink, and `--gc-sections` already drops dead code |
| Game worker | loaded into every game that is ON: every page costs every game | the text spotter (API 54) took four text pages more in each build |
| Rubi app | a 50 MiB budget, in its own process | links the dictionary reader, the Anki code, the apkg writer and the IME |

Two rules apply to app features:

- **App-only settings do not go into `config.ini`.** The overlay shows "unknown setting" for a key it does not know.
  The app keeps its own in `app.ini` (#381).
- **The Shell must stay byte-identical.** Some `core/` files are linked into the Shell as well, so changes to them
  must leave it unchanged.

## Built: the top five

All in the Rubi app (none of them in the Shell, the game worker or the kernel), in Rubi 0.10.0, measured on the host,
not run on a console yet.

| # | What | Issue | Merged in |
|---|---|---|---|
| 1 | Richer Anki cards at export: sentence readings, card type, game tag | #311 | #407 |
| 2 | A Stats page: all games, days, heatmap, streak, daily goal | #312 | #384, #403 |
| 3 | Look up and mine later: typed search, and a game's earlier lines | #313 | #391, #405 |
| 4 | Duplicate-aware mining: tags at export (stage A) and known words (stage C) | #314 | #381, #390, #393, #404 |
| 5 | A Cards page: list, delete, fix the sentence, drop the picture before export | #315 | #392 |

#314's stage B, a mark in the overlay's popup for a word you already mined, is **not built**: a minimal version needs
about 840 B more code in the normal Shell than its pages have ([anki.md](anki.md#not-in-the-popup)). Whether it gets
an item of its own is the owner's call.

## Next

| # | What | Value | Cost | Runs in | Score |
|---|---|:-:|:-:|---|:-:|
| 6 | Pitch accent, end to end | 4 | M–L | converter + app; the Shell for the bundled dictionary (3) | 2.5 to 5.5 |
| 7 | Overlay settings in the app: font sizes, dictionaries, frequency badge, reading time | 2.5 | S | app | 4 |
| 8 | First use: a checklist in the app | 2.5 | S–M | app | 3.5 |
| 9 | JLPT level, as a stacked entry and a card tag | 2.5 | S–M | converter + app | 3.5 |
| 10 | First-install robustness (#260–#262) | 2.5 | S–M | app | 3.5 |

### 6. Pitch accent

No pitch is shown, or put on new cards. `VjoVocab.pitch` is never filled; the converter ignores pitch banks in a term
dictionary and rejects a pitch-only one; `core/pitch.c` runs only at export. Yomitan with a pitch dictionary shows one
on 549 of every 1,000 taps of the dictionary check's corpus ([yomitan-check.md](../reports/yomitan-check.md#what-remains)).

**Plan:** import Yomitan pitch banks and show the downstep in the reading's line; fill `VjoVocab.pitch`, so the
record's `pitch`, and with it the existing `PitchPosition` and `PitchCategories` export, have data. The cost depends
on the dictionary:

- **A user's dictionary:** the converter can write the pitch into the definition text: no Shell code (score 5.5).
- **The frozen bundled dictionary:** a pitch file and a Shell reader (score 2.5).

### 7. Overlay settings in the app

`font_size_ja`/`font_size_en`, `frequency_badge`, `local_dictionaries` and `reading_time` can only be set in
`config.ini`. **Plan:** the app writes these keys, which the Shell already reads, through `vjo_config_patch_keys`, and
lets you enable and order the `.vjdict` files. Buttons are not included: remapping is cancelled.

### 8. First use

#263 and #267 (first-time users "thought recognition was broken", because no box had been drawn) were closed on 10-26.
The suggested text box (#369, "Use this box?") covers the box. **Plan:** a checklist on the Games page that the app can
verify: installed, restarted, the game ON, a box saved. It ends with "press L + R". App only.

### 9. JLPT level

**Plan:** the converter reads Yomitan meta banks and turns
[jlpt-levels-yomitan](https://github.com/bee-san/jlpt-levels-yomitan) into a small `jlpt.vjdict` whose entry reads
`JLPT N3`; the overlay stacks it under the word, as it does the names dictionary, with no Shell code; the export adds a
`JLPT::N3` tag. The bundled dictionary stays frozen.

### 10. First-install robustness

- #260: read payload files through `app0:` on a real failure (partly done by #257).
- #261: tell missing, unreadable and damaged files apart.
- #262: show progress while the dictionary is verified.

A first install has never run on a console (README).

<details><summary>11–23</summary>

| # | What | Value | Cost | Runs in | Score |
|---|---|:-:|:-:|---|:-:|
| 11 | VN progress and time left per game (needs a title ID → length table) | 2.5 | M | app | 3 |
| 12 | PC companion for exported cards: sentence TTS (Kokoro_tts_for_anki), word audio (yomitan-audio-fast), frequency-first new cards (anki_sorter). Not audio capture | 2 | S | PC | 3 |
| 13 | "Already in Anki": the app turns Anki for Vita's duplicate index into #314's markers (Anki for Vita is not released) | 2 | S | app | 3 |
| 14 | The previous line on the card, from history.csv at export | 2 | S | app | 3 |
| 15 | Colour known words in the line, and jump to the next unknown with ◀▶ (after #314; a check per word) | 4 | L | Shell (3) | 2 |
| 16 | Convert Yomitan ZIPs on the Vita | 2.5 | L | app | 2 |
| 17 | Kanji grid: the kanji seen per game, and their lines | 2 | M | app | 2 |
| 18 | Compressed `.apkg` | 1.5 | S | app | 2 |
| 19 | The running game's character names first (#287, closed unmerged) | 2 | S | Shell (2) | 1 |
| 20 | Structured definitions: lists, labels, cross-references | 2.5 | M–L | converter + Shell (3) | −0.5 |
| 21 | Lookup counts and definition blur (Hachidori 0.3.2) | 2 | M | Shell (3) | −1 |
| 22 | A personal dictionary entry from the overlay | 2 | M | Shell (3) | −1 |
| 23 | OCR exclusion boxes. Names do not matter (owner), and one box per game covers dialogue | 1.5 | S–M | worker + Shell (3) | −1.5 |

</details>

## Out of scope

**The owner's:** sessions; audio capture. **Removed or cancelled by the owner:** all network code (AnkiConnect,
Google Lens, the online dictionaries, the Hachidori relay, and sync in Rubi: #220 moved it to Anki for Vita); key
remapping and the ○/× buttons.

**Not recommended:**

- **Machine translation on the Vita:** no offline model fits. GameSentenceMiner's needs a PC or an LLM.
- **Nested popups, and Yomitan's 18 MB stroke-order font:** both cost Shell code or memory.
- **Animated screenshots:** they take the game's memory and CPU.
- **A Quick Menu entry:** it needs a hook in SceShell.
- **The popup on the game's text** (#271, closed; draft #326 is the owner's console test).
- **More OCR engines:** Rubi uses one engine.

## Sources

The ranking of 2026-10-26 (issue #310): Rubi main `0b1a45b` (all 51 issues with their comments, and the closed drafts'
discussions); GameSentenceMiner `5190b613` and gsm-docs `be184133`; Yomitan `d3c54c10`; Anki for Vita, Hachidori
0.3.2 (`4745397`), hoshidicts, jlpt-levels-yomitan, Kokoro_tts_for_anki, yomitan-audio-fast, anki_sorter,
jmdict-vita, meikidroid, PSVshell(Plus) and reVita. Each plan issue cites file and line.
