<p align="center">
  <a href="https://cdn.jsdelivr.net/gh/bee-san/Rubi_for_vita@ea662f3ac20f3a8429eab193f40359abd0b23406/docs/promo/media/promo-1920x1080.mp4"><img src="https://cdn.jsdelivr.net/gh/bee-san/Rubi_for_vita@ea662f3ac20f3a8429eab193f40359abd0b23406/docs/promo/media/promo.gif" width="540" alt="Rubi on a PS Vita: L + R opens the overlay over a line of a visual novel, Right moves to the next word and shows its dictionary entry, and × saves the word as an Anki card."></a>
</p>

<h1 align="center">Rubi</h1>

<p align="center">
  <b>Learn Japanese with your PS Vita: look up words from the games you play.</b><br>
  In a visual novel, press L + R: Rubi reads the dialogue off the screen, and you pick a word to see
  what it means and save it as an Anki card with a screenshot. Fully offline, on the Vita itself.
</p>

<p align="center">
  <a href="https://github.com/bee-san/Rubi_for_vita/releases"><img alt="Latest release" src="https://img.shields.io/github/v/release/bee-san/Rubi_for_vita?label=release"></a>
  <a href="https://github.com/bee-san/Rubi_for_vita/actions/workflows/release.yml"><img alt="CI status" src="https://img.shields.io/github/actions/workflow/status/bee-san/Rubi_for_vita/release.yml?branch=main&label=CI"></a>
  <a href="LICENSE"><img alt="License: GPL-3.0" src="https://img.shields.io/badge/license-GPL--3.0-blue"></a>
  <a href="http://discord.skerritt.blog"><img alt="Discord" src="https://img.shields.io/discord/754001738184392704?label=discord"></a>
</p>

<p align="center">
  ➡️ <a href="#install">Install</a> ·
  <a href="#use-in-a-game">Use it</a> ·
  <a href="#what-it-does">Features</a> ·
  <a href="#faq">FAQ</a> ·
  <a href="docs/guide.md">Guide</a> ·
  <a href="http://discord.skerritt.blog">Discord</a> ⬅️
  <br><sub>▶ <a href="https://cdn.jsdelivr.net/gh/bee-san/Rubi_for_vita@ea662f3ac20f3a8429eab193f40359abd0b23406/docs/promo/media/promo-1920x1080.mp4">Watch the 42-second tour</a> (MP4, 42 s)</sub>
</p>

> Rubi is the new name of Vita JP Overlay ([what changed](docs/rename-rubi.md)). The app was called
> **Overlay** in the 0.8.0 test builds and **JP Overlay Settings** before; the new VPK installs over
> either and keeps your settings, games and Anki cards.

## What it does

<p align="center">
  <img src="https://cdn.jsdelivr.net/gh/bee-san/Rubi_for_vita@ea662f3ac20f3a8429eab193f40359abd0b23406/docs/promo/media/stills/02-7.8.jpg" width="32%" alt="The overlay over the promo's mock visual novel: the line 「また明日、一緒に見に行こう!」 with 一緒に highlighted, and its entry: いっしょに, frequency rank 125, together (with), and the meanings of its two kanji.">
  <img src="https://cdn.jsdelivr.net/gh/bee-san/Rubi_for_vita@ea662f3ac20f3a8429eab193f40359abd0b23406/docs/promo/media/stills/04-15.9.jpg" width="32%" alt="The Anki card Rubi made of 一緒に, a Lapis note: the word and its reading, the sentence, the definition from Bee's Ultimate Dictionary and the game's screenshot.">
  <img src="https://cdn.jsdelivr.net/gh/bee-san/Rubi_for_vita@ea662f3ac20f3a8429eab193f40359abd0b23406/docs/promo/media/stills/07-26.2.jpg" width="32%" alt="The Rubi app's Games page: Ember Garden turned OFF, with the app's line “Saved. It applies when the game next starts or resumes.”">
</p>

- **Look up any word in the dialogue.** L + R opens the overlay over the game: the dialogue at the
  top, a word's dictionary entry under it, with its reading and how common the word is. Go word by
  word with the d-pad or the left stick, or tap a word. An inflected word is looked up in its
  dictionary form (した → する).
- **Fully offline.** Rubi reads the text with its own OCR on the Vita and looks it up in a
  dictionary on the memory card: no Wi-Fi, no computer, no account. It has no network code: the
  Vita build fails if any of its modules imports a network library.
- **A big dictionary built in.** Bee's Ultimate Dictionary: 486,529 entries with English
  definitions from Daijirin, Sankoku and JMdict, the meanings of a word's kanji, a short
  English note on each grammar point (は, に, ので …) and Jiten's frequency ranks. You can add your
  own Yomitan dictionaries too.
- **Anki cards with screenshots.** With card mining on, press Enter on a word to save it with its
  sentence and a picture of the game's screen. The app exports the cards as one `.apkg` file of
  [Lapis](https://github.com/donkuri/lapis) notes for Anki.
- **Small, Normal or Large.** Choose how fast or how accurate the OCR is. When a game is short of
  memory, Rubi uses the next smaller model by itself.
- **Reads when the text changes, or only when you ask.** By default Rubi reads each new screen of
  text in the background, so the popup is ready when you press. Or set it to read only when you
  press L + R.
- **On or off for each game.** Every game is ON to start with; turn off the ones that need L + R
  themselves. The app opens beside your game, which waits suspended behind it (a game that uses
  extended memory may have to close first).

## Install

You need a PS Vita on firmware 3.60–3.74 with HENkaku/Ensō or h-encore, VitaShell, **Settings >
HENkaku Settings > Enable Unsafe Homebrew** on, and about 246 MB free on the memory card before you
copy the VPK to it (estimated from the package's size; 62 MB of it is the VPK itself, which you can
delete once it is installed).

1. Download `Rubi-<version>.vpk` from the
   [latest release](https://github.com/bee-san/Rubi_for_vita/releases/latest).
2. Copy it to the Vita with VitaShell's USB or FTP connection, press Enter on it in VitaShell, and
   answer Yes to installing it and Yes to its notice about extended permissions. Close the app first
   if an older version of it is open, even suspended behind VitaShell (Rubi, Overlay or JP Overlay
   Settings): pull the top-right corner of its LiveArea page. In test.4's console run VitaShell stopped
   with error `0x80101114` while the old app was open
   ([#194](https://github.com/bee-san/Rubi_for_vita/issues/194)); if you see it, close the app or
   restart the Vita, then install again.
3. Open **Rubi** from LiveArea, choose **Install** on its Install page, and restart the Vita.

That's all: nothing is downloaded on the Vita, and every game is ON. Start a game and press L + R.
To update, close the app and do the same with the new VPK (the Install page then offers
**Update**); **Uninstall** is on the same page. The [guide](docs/guide.md#install) has what Install
changes, the debug build and the installer for a computer.

## Use in a game

Enter and Cancel are your Vita's own buttons: × and ○ on most consoles, ○ and × on Japanese ones
(HENkaku can swap them).

1. **Open it.** In a game, press **L + R** when there is text on the screen. The overlay shows the
   dialogue at the top and a word's entry below it. **L + R** or **Cancel** closes it.
2. **Draw the text box, once per game.** With the overlay open, press **□**: a cursor appears. Move
   it with the left stick (or the d-pad, a pixel at a time), press **Enter** at one corner of the
   dialogue box, then again at the opposite corner. A tight box needs less memory and makes fewer
   mistakes. Hold **□** for a second to go back to the whole screen.
3. **Look a word up.** **◀ ▶** go word by word, **▲ ▼** go a row up or down, and the left stick
   moves faster. Or tap a word. The right stick scrolls a long entry.
4. **Make a card.** Turn **Card mining** ON on the app's **Anki** page once. Then press **Enter** on
   a word: it is saved with its sentence and a screenshot. **Export cards** on the Anki page writes
   them into `ux0:data/VitaJPOverlay/anki/<date>_<time>-<N>cards.apkg`: copy that file to a computer
   (VitaShell's USB or FTP connection) and open it in Anki.

Every control, setting and file is in the [guide](docs/guide.md#use-in-a-game).

## Supported

| | Status |
| --- | --- |
| PS Vita (PCH-1000 series) | The test builds run on the owner's PCH-1001 on firmware 3.65. |
| PS Vita Slim (PCH-2000), PS TV | Not tested yet. |
| Firmware | 3.60–3.74 with taiHEN: HENkaku (Ensō) or h-encore. Tested on 3.65. |
| Vita games | Any game that is ON. On a console, Rubi's OCR has run in CLANNAD in every test build. In test.4 the kernel plugin loaded the OCR worker into CLANNAD with no taiHEN line for it ([#191](https://github.com/bee-san/Rubi_for_vita/issues/191), [#194](https://github.com/bee-san/Rubi_for_vita/issues/194)); AIR got none in test.3, which loaded the worker only into a game with such a line. Other games are untested. A game that uses L + R itself: turn it OFF. |
| PSP games (Adrenaline) | Boku no Natsuyasumi 4 launched on the owner's console on 2026-10-17 after replacing Adrenaline's oversized Shell bridge with the [small compatibility bridge](compat/adrenaline_vsh/README.md). The kernel loaded Rubi's worker into the bubble's PSP emulator process (`PSPEMU004`). PSP OCR recognition is still unverified. Adrenaline and each Adrenaline Bubble Manager bubble are listed in the app, a bubble under its game's title. |
| A PSP | No: Rubi runs on a PS Vita. |

## FAQ

### Does it drain the battery?

Battery use is not measured yet. With **Read text** on **Automatically** (the default), Rubi reads
each new screen of text in the background, at the lowest priority, with the CPU at 444 MHz while it
recognizes (**Speed boost**, on by default). Choose **Only when I press L + R** and it reads nothing
until you press (unless the subtitle strip, Select + R, is on); turn **Speed boost** off to keep the
game's own clock. A game you turn OFF gets no OCR at all. Both settings are on the app's OCR &
lookup page ([details](docs/auto-vs-press.md)).

### Will it slow my game down?

Reading a screen takes CPU time from the game, so the game can stutter while Rubi reads: in the
test.3 build's console logs, 291 of 1,609 frames during a reading came late (more than 25 ms after
the one before), against 52 of 26,941 frames in the time before each reading. With **Only when I
press L + R** that happens only when you press.

### Does it work with my game? Is there enough memory?

Rubi's OCR runs inside the game, in the game's own memory (for a PSP game, the emulator's), never in
the home screen (SceShell) or another system app. Before each reading it checks how much memory the
game has free, leaves the game a reserve, and uses a smaller model when the chosen one doesn't fit;
if even the smallest doesn't, it says so (a tighter text box helps). A game that is ON carries
Rubi's OCR worker from its start: 136 KiB of its memory (100 KiB of module pages, 36 KiB of stack and
TLS: the build's figures). If a game or homebrew app won't start with it, turn it OFF in the app, or
hold L while you start it. The guide has every byte, in
[Memory and troubleshooting](docs/guide.md#memory-and-troubleshooting).

### Can it break my Vita? How do I undo it?

The app's Install page changes nothing until you choose Install, and it keeps a dated backup of
each taiHEN `config.txt` it changes. On the owner's Vita its **Update** has run three times
(test.4.1, 4.2 and 4.3, each over the build before it; the first wrote the three plugins, and Check
and repair then reported it installed); an install on a Vita without the overlay has not run on a
console yet ([#194](https://github.com/bee-san/Rubi_for_vita/issues/194)). If a start of the Vita
doesn't finish, the next one is a safe boot: Rubi installs no hooks and unloads itself, so the Vita
starts as if Rubi weren't there ([safe boot](docs/boot-safety.md)).

If the Vita does not start: hold L while you turn it on (h-encore or h-encore2: open its menu,
holding R as h-encore starts, and choose Exit while holding L), then in VitaShell put `#` before the
two `VitaJPOverlay` lines of `ur0:tai/config.txt` and `ux0:tai/config.txt`, or choose **Uninstall**
on the app's Install page. If a game does not start: turn it OFF on the Games page, or hold L while
you start it. Uninstalling keeps your settings and cards.

### Does it send anything over the internet?

No. Rubi has no network code: the Vita build fails if any of its modules imports a network library.
Recognition, lookups and cards all stay on the Vita; your cards leave it only when you copy the
`.apkg` yourself. Rubi keeps its logs on the memory card, in `ux0:data/VitaJPOverlay/`
(`log_file = off` in `config.ini` turns off the main one, `log.txt`). The normal build, the
release's main download, logs no recognized or looked-up text; the `-debug` build, made for bug
reports, logs both. With **Read text** on **Automatically**, both builds also keep the dialogue they
read, with its game and time, in `ux0:data/VitaJPOverlay/history.csv`, for reading statistics (the
CSV that GameSentenceMiner imports; [details](docs/ocr-history.md)). It stays on the Vita too, grows
as you read and is never deleted by itself; **Only when I press L + R** saves none.

### How good is the OCR?

Rubi reads text with VOCR, small recognizers that [vita-vn-ocr](https://github.com/bee-san/vita-vn-ocr)
trains for visual-novel dialogue boxes, and decodes each line with a language model. Its accuracy on
a console is not measured yet; the host measurements are in [docs/vita-vn-ocr.md](docs/vita-vn-ocr.md).

### Can I use my own dictionaries?

Yes. Convert Yomitan dictionaries (their ZIP files) on a computer with `tools/convert_dictionary.py`
and copy them to the memory card; up to eight are used, in your order
([how](docs/local-dictionaries.md)).

## Documentation

- [The guide](docs/guide.md): every control and setting, the files and logs, the installer for a
  computer, memory, recovery, building and testing.
- [Anki cards](docs/anki.md) · [Your own dictionaries](docs/local-dictionaries.md) ·
  [Read text: automatically or on a press](docs/auto-vs-press.md) · [The OCR](docs/vita-vn-ocr.md) ·
  [The one-VPK install](docs/one-vpk-install.md) · [Safe boot](docs/boot-safety.md) ·
  [Vita memory](docs/vita-memory.md)

## Contributing

Bug reports, ideas and pull requests are welcome in
[issues](https://github.com/bee-san/Rubi_for_vita/issues), and questions on
[Discord](http://discord.skerritt.blog). With a bug, the logs help most: from a computer, with
VitaShell's FTP on, `bash tools/install_ftp.sh --collect VITA_IP` gathers them into one zip
([what it collects](docs/self-test.md#collecting-everything---collect)). A good first contribution:
try Rubi on a Vita Slim, a PS TV or a game nobody has tested yet, and send that zip in an issue. To
build and test Rubi, see the guide's [Build and validation](docs/guide.md#build-and-validation);
[AGENTS.md](AGENTS.md) has the rules every change follows.

<a href="https://github.com/bee-san/Rubi_for_vita/graphs/contributors"><img src="https://contrib.rocks/image?repo=bee-san/Rubi_for_vita" alt="Avatars of the people who have contributed to Rubi"></a>

## Credits

This was made heavily with the help of [Shoui](https://github.com/shoui520). Shoui is one of the
smartest people I know, an expert in Windows and PS Vita lowlevels. She advised the design of the
program and helped debug issues, while I simply vibe coded what she suggested. This project would
not be possible without her. She also made the app a system app, so it opens beside a suspended
game ([#109](https://github.com/bee-san/Rubi_for_vita/pull/109)).

- Based on [Dartv/vita-jp-overlay](https://github.com/Dartv/vita-jp-overlay).
- **OCR:** VOCR, from [bee-san/vita-vn-ocr](https://github.com/bee-san/vita-vn-ocr) (GPL-3.0). Its
  VOCR5 recognizers, the three model sizes, were trained in part on NVIDIA's
  OCR-Synthetic-Multilingual-v1 (CC BY 4.0) and on fonts and text that are free for non-commercial
  use only, so their weights are for non-commercial use only: their attribution and terms ship
  beside them as `ocr/MODEL-ATTRIBUTION.md` ([details](docs/vita-vn-ocr.md#models)). Its character
  language models are CC BY-SA 4.0. The optional text-presence gate's weights (`ocr_text_gate`, off by
  default) were trained on synthetic text in fonts and text pools free for non-commercial use only and on
  VNDB screenshots, so they are for non-commercial use only too: their attribution ships beside them as
  `ocr/GATE-ATTRIBUTION.md` ([details](docs/vita-vn-ocr.md#text-presence-gate-api-26)). The vertical model for
  text in columns (`ocr_vertical`, off by default), VOCR5-V, is VOCR5-S fine-tuned on synthetic columns, so it
  carries VOCR5-S's training data and terms and is for non-commercial use only too: its attribution ships beside it
  as `ocr/VERTICAL-ATTRIBUTION.md` ([details](docs/vita-vn-ocr.md#vertical-text-vita-vn-ocr-99)).
- **Dictionary:** Bee's Ultimate Dictionary. English definitions from Daijirin4 and Sankoku8
  (© Sanseido) and [JMdict](https://www.edrdg.org/) (EDRDG, CC BY-SA 4.0); frequency ranks from
  [Jiten](https://jiten.moe) (CC BY-SA 4.0); kanji lines from Bee's Ultimate Kanji Dictionary
  (Jiten and KANJIDIC2 data, CC BY-SA 4.0); grammar notes written from Bee's Ultimate Grammar
  Dictionary, whose sources include Yokubi and NINJAL's 日本語文型データベース (CC BY 4.0). Every
  source keeps its own licence: the [attribution](docs/plaintext-attribution.txt) has them all.
- **Anki:** the [Lapis](https://github.com/donkuri/lapis) note type by donkuri (GPL-3.0).
- **Lookups:** Yomichan's deinflection rules. The dictionary's key filter is modelled on the Bloom
  filter of [hoshidicts](https://github.com/Manhhao/hoshidicts), Manhhao's dictionary library, which
  bee-san's [Hachidori](https://github.com/bee-san/hachidori) app is built on; Rubi has no code of
  either.
- **Thanks to** [meikidroid](https://github.com/rtr46/meikidroid),
  [jmdict-vita](https://github.com/shoui520/jmdict-vita),
  [PSVshellPlus](https://github.com/GrapheneCt/PSVshellPlus),
  [PSVshell](https://github.com/Electry/PSVshell), [reVita](https://github.com/MERLev/reVita),
  [ScePaf-RE](https://github.com/GrapheneCt/ScePaf-RE), VitaSDK, BearSSL, jsmn, acutest,
  [taihen-parser](https://github.com/DaveeFTW/taihen-parser) and libpng, and to
  [Adrenaline Bubble Manager](https://github.com/ONElua/AdrenalineBubbleManager) and
  [AdrBubbleBooter](https://github.com/shoui520/AdrBubbleBooter-oss), whose sources show how PSP
  games start. Pinned sources and their licences are in [third_party/](third_party/README.md).
- The mascot was drawn by a pinned, Apache-2.0 text-to-image model
  ([how](settings/assets/ICON-SOURCE.md)).

## AI use

- The code is vibe coded: AI coding agents write it, under the rules in [AGENTS.md](AGENTS.md).
- In the dictionary, Daijirin's and Sankoku's English is credited to GPT models, and the grammar
  notes were written by Claude agents ([attribution](docs/plaintext-attribution.txt)).
- Part of the OCR's training labels, lines from real screenshots, were reviewed by Claude agents,
  with Codex as a second vote ([vita-vn-ocr](https://github.com/bee-san/vita-vn-ocr/blob/main/docs/realplus.md)).
- The mascot is drawn by a text-to-image model (above).

## License

Rubi's code is GPL-3.0 ([LICENSE](LICENSE)). The OCR's recognizer weights and the text-presence gate's
are data files Rubi reads, not part of its code, and are for non-commercial use only
(`ocr/MODEL-ATTRIBUTION.md`, `ocr/GATE-ATTRIBUTION.md`, `ocr/VERTICAL-ATTRIBUTION.md`,
[details](docs/vita-vn-ocr.md#models)). The dictionary data,
the language models and the third-party code keep their own licences
([attribution](docs/plaintext-attribution.txt), [third_party/](third_party/README.md)).
