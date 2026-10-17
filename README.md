<!-- Hero video (to come): the promo's GIF, linked to its MP4, replaces the icon below, as in
     Ciphey's README (<a href="MP4"><img src="GIF" alt="what the video shows"></a>), with
     "▶ Watch the N-second tour (MP4)" under the links line. The promo shows its own mock visual
     novel: never game art. -->
<p align="center">
  <img src="settings/assets/icon0.png" width="128" height="128" alt="Rubi's icon: a girl in round glasses reading a yellow book, beside a speech bubble with the kana あ">
</p>

<h1 align="center">Rubi</h1>

<p align="center">
  <b>Learn Japanese with your PS Vita: look up words from the games you play.</b><br>
  In a visual novel, press L + R: Rubi reads the dialogue off the screen, and you pick a word to see
  what it means and save it as an Anki card with a screenshot. Fully offline, on the Vita itself.
</p>

<p align="center">
  <a href="https://github.com/bee-san/vita-jp-overlay/releases"><img alt="Latest release" src="https://img.shields.io/github/v/release/bee-san/vita-jp-overlay?label=release"></a>
  <a href="https://github.com/bee-san/vita-jp-overlay/actions/workflows/release.yml"><img alt="CI status" src="https://img.shields.io/github/actions/workflow/status/bee-san/vita-jp-overlay/release.yml?branch=main&label=CI"></a>
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
</p>

> Rubi is the new name of Vita JP Overlay. Until the rename lands, the app on the Vita is called
> **Overlay** and the files keep their old names (`VitaJPOverlay-<version>.vpk`).

## What it does

<!-- Stills (to come, with the promo): the popup over the promo's mock visual novel, the Anki card
     and the app's Games page. Never game art. -->

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

> [!IMPORTANT]
> Not released yet. This one-VPK install is on `main` for the next release (0.8.0), and these steps
> are not yet verified on a console: the next test build (test.4) checks them first. Until then, the
> [releases page](https://github.com/bee-san/vita-jp-overlay/releases) has the older 0.7.12 and the
> 0.8.0 test builds. They install from a computer
> ([guide](docs/guide.md#advanced-install-from-a-computer); for 0.7.12, use the installer from `main`)
> and predate much of this page: their release notes say what each one has.

You need a PS Vita on firmware 3.60–3.74 with HENkaku/Ensō or h-encore, VitaShell, **Settings >
HENkaku Settings > Enable Unsafe Homebrew** on, and about 245 MB free on the memory card before you
copy the VPK to it (estimated from the package's size; 61 MB of it is the VPK itself, which you can
delete once it is installed).

1. Download `VitaJPOverlay-<version>.vpk` from the
   [latest release](https://github.com/bee-san/vita-jp-overlay/releases/latest).
2. Copy it to the Vita with VitaShell's USB or FTP connection, press Enter on it in VitaShell, and
   answer Yes to installing it and Yes to its notice about extended permissions.
3. Open **Overlay** from LiveArea, choose **Install** on its Install page, and restart the Vita.

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
| Vita games | Any game that is ON. On a console, Rubi's OCR has run in CLANNAD (the test builds); AIR had no OCR in test.3, for the reason in the PSP row; other games are untested. A game that uses L + R itself: turn it OFF. |
| PSP games (Adrenaline) | Built in, not yet confirmed on a console. Adrenaline and each Adrenaline Bubble Manager bubble are listed in the app and get OCR like a Vita game, but the test.3 build's tester got no OCR in Adrenaline. The likely cause, the OCR worker never being loaded into it, is fixed on `main`; test.4 checks it. |
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
Rubi's OCR worker from its start: 128 KiB of its memory (92 KiB of module pages, 36 KiB of stack and
TLS: the build's figures). If a game or homebrew app won't start with it, turn it OFF in the app, or
hold L while you start it. The guide has every byte, in
[Memory and troubleshooting](docs/guide.md#memory-and-troubleshooting).

### Can it break my Vita? How do I undo it?

The app's Install page (the one-VPK install, which test.4 checks first) changes nothing until you
choose Install, and it keeps a dated backup of each taiHEN `config.txt` it changes. If a start of
the Vita doesn't finish, the next one is a safe boot: Rubi installs no hooks and unloads itself, so
the Vita starts as if Rubi weren't there ([safe boot](docs/boot-safety.md)).

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
reports, logs both.

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
[issues](https://github.com/bee-san/vita-jp-overlay/issues), and questions on
[Discord](http://discord.skerritt.blog). With a bug, the logs help most: from a computer, with
VitaShell's FTP on, `bash tools/install_ftp.sh --collect VITA_IP` gathers them into one zip
([what it collects](docs/self-test.md#collecting-everything---collect)). A good first contribution:
try Rubi on a Vita Slim, a PS TV or a game nobody has tested yet, and send that zip in an issue. To
build and test Rubi, see the guide's [Build and validation](docs/guide.md#build-and-validation);
[AGENTS.md](AGENTS.md) has the rules every change follows.

<a href="https://github.com/bee-san/vita-jp-overlay/graphs/contributors"><img src="https://contrib.rocks/image?repo=bee-san/vita-jp-overlay" alt="Avatars of the people who have contributed to Rubi"></a>

## Credits

This was made heavily with the help of [Shoui](https://github.com/shoui520). Shoui is one of the
smartest people I know, an expert in Windows and PS Vita lowlevels. She advised the design of the
program and helped debug issues, while I simply vibe coded what she suggested. This project would
not be possible without her. She also made the app a system app, so it opens beside a suspended
game ([#109](https://github.com/bee-san/vita-jp-overlay/pull/109)).

- Based on [Dartv/vita-jp-overlay](https://github.com/Dartv/vita-jp-overlay).
- **OCR:** VOCR, from [bee-san/vita-vn-ocr](https://github.com/bee-san/vita-vn-ocr) (GPL-3.0). Its
  VOCR5 recognizers, the three model sizes, were trained in part on NVIDIA's
  OCR-Synthetic-Multilingual-v1 (CC BY 4.0) and on fonts and text that are free for non-commercial
  use only, so their weights are for non-commercial use only: their attribution and terms ship
  beside them as `ocr/MODEL-ATTRIBUTION.md` ([details](docs/vita-vn-ocr.md#models)). Its character
  language models are CC BY-SA 4.0.
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

Rubi's code is GPL-3.0 ([LICENSE](LICENSE)). The OCR's recognizer weights are for non-commercial
use only (`ocr/MODEL-ATTRIBUTION.md`, [details](docs/vita-vn-ocr.md#models)). The dictionary data,
the language models and the third-party code keep their own licences
([attribution](docs/plaintext-attribution.txt), [third_party/](third_party/README.md)).
