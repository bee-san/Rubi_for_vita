<p align="center"><b>Beta:</b> tested on one PS Vita (PCH-1001, firmware 3.65) in CLANNAD.
  <a href="#supported">What is tested</a></p>

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
  <a href="#native-text-hooks">Text hooks</a> ·
  <a href="#supported">Supported</a> ·
  <a href="#faq">FAQ</a> ·
  <a href="docs/guide.md">Guide</a> ·
  <a href="CHANGELOG.md">Changelog</a> ·
  <a href="http://discord.skerritt.blog">Discord</a> ⬅️
  <br><sub>▶ <a href="https://cdn.jsdelivr.net/gh/bee-san/Rubi_for_vita@ea662f3ac20f3a8429eab193f40359abd0b23406/docs/promo/media/promo-1920x1080.mp4">Watch the 42-second tour</a> (MP4, 42 s)</sub>
</p>

> Rubi is the new name of Vita JP Overlay ([what changed](docs/rename-rubi.md)): its VPK installs
> over the old app and keeps your settings, games and Anki cards.

## What it does

<p align="center">
  <img src="https://cdn.jsdelivr.net/gh/bee-san/Rubi_for_vita@ea662f3ac20f3a8429eab193f40359abd0b23406/docs/promo/media/stills/02-7.8.jpg" width="32%" alt="The overlay over the promo's mock visual novel: the line 「また明日、一緒に見に行こう!」 with 一緒に highlighted, and its entry: いっしょに, frequency rank 125, together (with), and the meanings of its two kanji.">
  <img src="https://cdn.jsdelivr.net/gh/bee-san/Rubi_for_vita@ea662f3ac20f3a8429eab193f40359abd0b23406/docs/promo/media/stills/04-15.9.jpg" width="32%" alt="The Anki card Rubi made of 一緒に, a Lapis note: the word and its reading, the sentence, the definition from Bee's Ultimate Dictionary and the game's screenshot.">
  <img src="https://cdn.jsdelivr.net/gh/bee-san/Rubi_for_vita@ea662f3ac20f3a8429eab193f40359abd0b23406/docs/promo/media/stills/07-26.2.jpg" width="32%" alt="The Rubi app's Games page: Ember Garden turned OFF, with the app's line “Saved. It applies when the game next starts or resumes.”">
</p>

- **Look up any word in the dialogue.** L + R opens the overlay over the game: the dialogue at the
  top, a word's dictionary entry under it, with its reading and how common the word is. Go word by
  word with the d-pad, character by character with the left stick, or tap a word. An inflected word
  is looked up in its dictionary form (した → する).
- **Vertical text too.** Columns of Japanese, read right to left, as in a vertical dialogue box or a manga in
  NOBORU: in a text box drawn around them Rubi finds them by itself (**Vertical text**: Automatic) and reads them
  with its vertical model ([how well](docs/vita-vn-ocr.md#vertical-text-vita-vn-ocr-99); not run on a console yet).
- **Fully offline.** Rubi reads dialogue directly with [native text hooks](#native-text-hooks) where
  available, or uses its own OCR on the Vita, then looks it up in a dictionary on the memory card:
  no Wi-Fi, no computer, no account, and no network code
  ([privacy](#does-it-send-anything-over-the-internet)).
- **A big dictionary built in.** Bee's Ultimate Dictionary: 486,529 entries with English
  definitions from Daijirin, Sankoku and JMdict, the meanings of a word's kanji, a short
  English note on each grammar point (は, に, ので …) and Jiten's frequency ranks. The bundled
  [PSP/Vita names dictionary](docs/vndb-attribution.txt) adds 12,151 VNDB characters from 1,202
  visual novels, with their games and romanized names. You can add your own Yomitan dictionaries too.
- **Anki cards with screenshots.** With card mining on, press Enter on a word to save it with its
  sentence and a picture of the game's screen. Before you export, the app's Cards page shows each
  card: delete one saved by mistake, fix an OCR mistake in its sentence, or drop its picture. The app
  exports the cards as one `.apkg` file of [Lapis](https://github.com/donkuri/lapis) notes for Anki.
- **Look up and mine later.** The Rubi app's **Look up** tab, beside the suspended game: type a word or a sentence
  with the Vita's keyboard, or open one of a game's last 200 lines, and look its words up with the overlay's
  dictionaries; Enter saves a word as a card, without a picture ([details](docs/guide.md#look-up)).
- **Fast or accurate, now or when you ask.** Choose a Small, Normal or Large model (a game short of
  memory gets the next smaller one by itself), and whether Rubi reads each new screen in the
  background, so the popup is ready when you press, or only when you press L + R.
- **On or off for each game.** Every game is ON to start with, PSP games in Adrenaline too
  (Adrenaline and each bubble are listed, a bubble under its game's title); turn off the ones that
  need L + R themselves. The app opens beside your game, which waits suspended behind it (a game
  that uses extended memory may have to close first).

<a id="install"></a>

## Installation instructions

You need a PS Vita on firmware 3.60–3.74 with HENkaku/Ensō or h-encore, VitaShell, and
**Settings > HENkaku Settings > Enable Unsafe Homebrew** turned on.

Download the [Rubi .zip](https://github.com/bee-san/Rubi_for_vita/releases/download/v0.9.8/Rubi-0.9.8.zip)
and extract it.

### 1. Transfer modules

Using VitaShell FTP, transfer the files from `Rubi-0.9.8/ur0/tai/` to `ur0:tai/` on the Vita:

- `VitaJPOverlay_Kernel.skprx`
- `VitaJPOverlay_OCR.suprx`
- `VitaJPOverlay_Shell.suprx`

### 2. Edit the taiHEN config

Back up `ur0:tai/config.txt`, then add `ur0:tai/VitaJPOverlay_Kernel.skprx` to its `*KERNEL`
section and `ur0:tai/VitaJPOverlay_Shell.suprx` to its `*main` section. The two sections can
appear in either order; each plugin must be beneath its matching section. Keep your other entries.

Example (`ur0:tai/config.txt`):

```text
*KERNEL
ur0:tai/VitaJPOverlay_Kernel.skprx

*main
ur0:tai/VitaJPOverlay_Shell.suprx
```

If `ux0:tai/config.txt` exists, taiHEN reads it first: back it up and add the entries there instead.
`VitaJPOverlay_OCR.suprx` needs no config entry; the kernel plugin loads it into enabled games.

### 3. Transfer data files for the plugin

Transfer `Rubi-0.9.8/ur0/data/VitaJPOverlay` to `ur0:data/` on the Vita, so the folder is present
as `ur0:data/VitaJPOverlay`.

### 4. Transfer data files for OCR and the dictionary

**This is a separate folder, not a duplicate of step 3.** Transfer
`Rubi-0.9.8/ux0/data/VitaJPOverlay` to `ux0:data/` on the Vita, so the folder is present
as `ux0:data/VitaJPOverlay`.

If updating an existing installation, back up your `ux0:data/VitaJPOverlay/config.ini` first
and keep your existing settings file when copying the data.

### 5. Transfer and install the VPK / companion app

Close Rubi first if it is already open, including when suspended behind VitaShell: pull the
top-right corner of its LiveArea page. Transfer `Rubi-0.9.8/Rubi.vpk` to the Vita and install
(promote) it with VitaShell. Accept the installation and extended-permissions prompts.

### 6. Reboot the Vita

In VitaShell, press **START** and choose **Reboot**.

Start a game and press **L + R** to open Rubi. Every game is ON by default.

The [guide](docs/guide.md#install) also covers the standalone VPK and computer-side installers.

## Use in a game

Enter and Cancel are your Vita's own buttons: × and ○ on most consoles, ○ and × on Japanese ones
(HENkaku can swap them).

1. **Open it.** In a game, press **L + R** when there is text on the screen. The overlay shows the
   dialogue at the top and a word's entry below it. **L + R** or **Cancel** closes it.
2. **Set the text box, once per game.** The first time a game's text is shown, Rubi draws a box
   around its dialogue and asks **Use this box?** Press **Enter** to save it as the game's box
   ([how it is found](docs/first-box.md)), or **Cancel** to draw your own. Or later: press **□**: a
   cursor appears. Move it with the left stick (or the d-pad, a pixel at a time), press **Enter** at
   one corner of the dialogue box, then again at the opposite corner. A tight box needs less memory
   and makes fewer mistakes. Hold **□** for a second to go back to the whole screen.
3. **Look a word up.** **◀ ▶** go word by word and **▲ ▼** a row up or down. The left stick goes a
   character at a time, faster while held, and looks up the word that starts where it stops. Or tap
   a word. The right stick scrolls a long entry. **▲** on the top row goes back to this game's earlier
   lines, one at a time (with **Read text: Automatically**), and **▼** comes forward again
   ([earlier lines](docs/line-history.md)).
4. **Make a card.** Turn **Card mining** ON on the app's **Anki** page once. Then press **Enter** on
   a word: it is saved with its sentence and a screenshot. **Export cards** on the Anki page writes
   them into `ux0:data/VitaJPOverlay/anki/<date>_<time>-<N>cards.apkg`: copy that file to a computer
   (VitaShell's USB or FTP connection) and open it in Anki.
5. **Look up later.** Suspend the game and open the Rubi app: on its **Look up** tab, type a word (Triangle), or
   pick an earlier line of a game (L/R), and look its words up; Enter saves the word as a card, without a picture
   ([details](docs/guide.md#look-up)).

Every control, setting and file is in the [guide](docs/guide.md#use-in-a-game).

## Settings

In the Rubi app, Select or a tap on a tab changes the page. A choice is saved at once and applies
when the overlay next opens; a game's ON or OFF applies when the game next starts or resumes.

| Page | Setting | Default |
| --- | --- | --- |
| Games | Each game ON or OFF, and **Default** ON or OFF ([details](docs/guide.md#per-game-settings)) | ON |
| | **Line finder** for one game (hold Enter on it): what cuts its text box into lines, the spotter or Rubi's finder with its fixes (`config.ini`'s `ocr_finder` and `ocr_line_fix` for that game alone), for a game the spotter reads worse ([details](docs/guide.md#a-games-own-page)) | Default (`config.ini`'s, the spotter) |
| OCR & lookup | **Model size**: Small (fastest), Normal or Large (most accurate) | Normal |
| | **Read text**: Automatically (when it changes), or Only when I press L + R | Automatically |
| | **Speed boost while reading (444 MHz)** | ON |
| | **Japanese characters only**: the cursor skips Latin letters, digits and punctuation | OFF |
| | **Highlight**: Whole word or Character | Whole word |
| | **Dictionaries shown**: how many dictionaries' entries for the same word are shown, the best match first and the others below it (the right stick scrolls to them), 1–8 (2: a word and the same word as a character's name; which comes first: `names_first`, [details](docs/local-dictionaries.md#names-and-words)) | 2 |
| | **Text source**: Automatic (tries hooks in every enabled native Vita game, with OCR fallback), Every game (the same, plus PSP text in two PSP games, testing), or OCR only; applies when the game next starts ([hook support](#native-text-hooks)) | Automatic |
| | **Trusted hooks replace OCR**: a hook Rubi trusts gives the line with no OCR at all, ready within a poll (16 ms) and the first word's lookup of the game writing it (measured on a computer; not run on a console yet; [details](docs/native-hooks.md#trusted-hooks-api-43)) | ON |
| | **Skip OCR on screens without text**: a text box that holds no text (hidden, a picture) is not read; it also skips a few very short lines, which is why it is off ([details](docs/vita-vn-ocr.md#text-presence-gate-api-26)) | OFF |
| | **Vertical text**: Automatic (columns where the text box holds vertical text), ON (columns, but rows where the box plainly holds lines of text) or OFF (rows only) ([vertical text](docs/vita-vn-ocr.md#vertical-text-vita-vn-ocr-99)) | Automatic |
| | **Suggest a text box**: a game without a box of its own is offered the one Rubi finds around its dialogue, once each time it starts or resumes ([details](docs/first-box.md)) | ON |
| | **Overlay on the game's text** (testing): the word is marked where the game shows it, a tap on the game's text looks it up, and the popup sits beside the text ([details](docs/overlay-on-text.md)) | OFF |
| | **Pause the game to read**: each reading copies the text box from one frame at once, the game waiting a moment (an estimated 2-6 ms for a drawn text box, 11-29 ms for the whole screen: draw a box with □ first), so a spinning icon in it can't stop the read ([details](docs/game-pause.md)) | OFF |
| Anki | **Card mining**: Enter saves the word as a card | OFF |
| | **Screenshots**: each card keeps a picture of the game's screen | ON |
| | **Trim black bars**: the picture leaves out the black borders around the game's picture (a PSP game shown smaller than the screen) | ON |
| | **Card type**: what the card's front shows: the word, its sentence, both, or the word with the sentence a tap away ([card types](docs/anki.md#card-type)) | Word |
| | **Game tag**: each card is tagged with its game, `Game::CLANNAD` | ON |
| | **Sentence readings**: readings over the sentence's kanji on the card's back, where the dictionaries are sure of them ([how often they are wrong](docs/anki.md#sentence-readings)) | ON |
| | **Duplicates**: a card of a word you mined before gets Anki's `duplicate` tag (Tag), stays in the queue (Skip) or is exported as before (Keep); the page counts them, "N queued, M already mined" ([details](docs/anki.md#duplicates)) | Tag |
| | **Cards: N queued**: each card before it is exported, newest first, with its sentence and picture; delete it, fix its sentence or drop its picture ([details](docs/anki.md#the-cards-page)) | |
| | **Known words**: Import reads `known_words.txt`, which `tools/known_words.py` writes from your Anki collection; their cards get the tag `known` ([details](docs/anki.md#known-words)) | none |
| | **Export cards** and **Export all cards**: write them into one `.apkg` file | |
| Look up | No setting: type a word (Triangle), or open an earlier line of a game (L/R); Enter saves its word as a card, without a picture, with Card mining ON or OFF ([details](docs/guide.md#look-up)) | |
| Stats | Your reading from `history.csv`: today, the last 14 days, the year's heatmap, the streak, all games and each game (characters, reading time, characters/hour, cards mined), counted as △ counts ([details](docs/guide.md#stats)) | |
| | **Daily goal**: Off, 2,000 to 50,000 characters or 15 to 120 minutes a day; a day at the goal counts for the streak (with none, any day with reading). Kept in the app's own `app.ini` | Off |
| Install | **Install**, **Update** or **Check and repair**, and **Uninstall** | |

Everything else, such as the button that opens the overlay (`toggle_button`), the OCR's timings and
logging, is in `ux0:data/VitaJPOverlay/config.ini`
([guide](docs/guide.md#settings-and-dictionaries), [template](docs/config.example.ini)).

## Native text hooks

A text hook reads dialogue directly from the game instead of recognizing letters in a screenshot.
With **OCR & lookup → Text source → Automatic** (the default), Rubi identifies the game's title ID,
tries its available hooks and selects the text automatically. **You do not choose a hook or enter
codes.** Keep the game ON on the Games page; after changing Text source, close and restart the game.
Older installs may have kept **OCR only**, so choose Automatic once if needed.

| Game or group | Native text support | Console testing |
| --- | --- | --- |
| **CLANNAD**, Japanese Vita release (`PCSG00415`) | Automatic; string-copy hooks and Rubi's own game profile | String-copy extraction confirmed in the combined debug test build, including readings without OCR. The separate game profile's hook site remains unverified on a real Vita. |
| **175 Vita title IDs from LunaTranslator's table** | Automatic attempts using 294 ported hook sites, plus string-copy hooks | Experimental: these game-specific hook sites have not been verified on a real Vita. Examples include **TIME TRAVELERS** (`PCSG00062`), **Code:Realize ～創世の姫君～** (`PCSG00402`) and **DRAMAtical Murder** (`PCSG00420`). |
| **54 more Vita title IDs with Rubi's own verified entries** (26 in 0.9.7, **ALIA's CARNIVAL! サクラメント** (`PCSG00628`) and **真 流行り神** (`PCSG00380`) in 0.9.8, **極限脱出ＡＤＶ 善人シボウデス** (`PCSG00020`, Zero Escape: Virtue's Last Reward), **蒼の彼方のフォーリズム** (`PCSG00700`), the **ZERO ESCAPE ダブルパック**'s 善人シボウデス (`PCSG00883`) and the SC3 games **STEINS;GATE 比翼恋理のだーりん** (`PCSG00146`), **ROBOTICS;NOTES ELITE** (`PCSG00352`) and **CHAOS;CHILD** (`PCSG00555`), and 20 from the farm's hook farm such as **マブラヴ オルタネイティヴ** (`PCSG00777`), **大図書館の羊飼い -Library Party-** (`PCSG00505`), **DIABOLIK LOVERS** (`PCSG00272` and four more) and **ISLAND** (`PCSG01000`), on `main`), such as **リトルバスターズ！ Converted Edition** (`PCSG00007`), **グリザイアの果実** (`PCSG00206`) and **CLOCK ZERO ～終焉の一秒～ ExTime** (`PCSG00469`) | Automatic: a site checked against the game's exact code bytes, with Rubi's own text clean-up. 29 are games of Luna's table: for 17 of them the entry unlocks Luna's own site, which Rubi's placement check refused | Experimental: proven in the Vita3K emulator only (a screenshot farm), not on a real Vita; 蒼の彼方のフォーリズム also on a PS Vita (15 of its hook's 19 lines agreed with OCR; 3 scored 0, not examined). For ALIA's CARNIVAL! サクラメント and 真 流行り神 a jump from elsewhere in the game's code into the hook's patched bytes is unchecked (the farm had no copy of their code): one would crash the game. |
| Other native Vita games | Automatic tries general string-copy hooks | Experimental discovery; useful dialogue is not guaranteed. OCR is used until a matching source is found. **Every game** makes the same attempts. |
| PSP games in Adrenaline: **Memories Off 6 ～T-wave～** (`NPJH50382`/`ULJM05467`) and **Memories Off ～それから～** (`NPJH50132`/`ULJM05350`) | With **Every game** only: Rubi reads the game's dialogue page from PSP memory, read-only, when the game's ID and code are the expected ones; OCR must agree before the text is shown | Experimental: proven in the PPSSPP emulator only (the farm), not on a real Vita ([PSP text](docs/native-hooks.md#psp-text-adrenaline)). |
| Other PSP games in Adrenaline | OCR only | PSP text hooks in the game itself (a PSP plugin) are not implemented, in any mode. |

That is **201 Vita title IDs with game-specific hook entries**, including CLANNAD; Automatic also
tries generic string-copy hooks in other enabled native Vita games.
See the **[full Vita game list](docs/hook-titles.md#vita-titles)** for title
IDs, versions and individual hook status; entries marked **refused** are not installed. The PSP table
on that page is reference material for future work.

Automatic selection does not guarantee every listed game or version works. Game-specific hook sites
must pass Rubi's version and code checks, and a hook must produce usable dialogue. Rubi checks hook
text against OCR initially and periodically afterwards, and falls back to OCR when a hook cannot
supply it. Both text sources work
offline. The [guide](docs/guide.md#native-text-hooks) explains the settings; the
[technical design](docs/native-hooks.md) covers string-copy hooks and game-specific hook sites.

**Your game has no hook, or its hook is refused?** Ask for one with a
[hook request](https://github.com/bee-san/Rubi_for_vita/issues/new?template=hook-request.yml): play the game once
with the debug build and send its `hooks.log` (no game text in it). The owner's hook farm can turn a request into a
verified entry: it works on the Japanese releases of visual novels, and not every request gets one
([how it works](docs/hook-requests.md)).

## Supported

✅ verified on a console · ⚠️ works with caveats, or partly verified · ❓ untested, or an estimate only

| | What | On a console |
| :-: | --- | --- |
| ✅ | PS Vita (PCH-1000 series) | the owner's PCH-1001, in every test build |
| ❓ | PS Vita Slim (PCH-2000), PS TV | untested |
| ✅ | Firmware 3.65 | the owner's, with taiHEN |
| ❓ | Other firmware, 3.60–3.74 | untested |
| ✅ | CLANNAD | OCR, lookups and Anki cards (OCR in every test build) |
| ❓ | Other Vita games | Rubi's OCR has read no other game's text on a console yet |
| ⚠️ | PSP games (Adrenaline) | one game started, with Rubi's OCR worker, using the [small bridge](compat/adrenaline_vsh/README.md) (on `main`, not in 0.8.0); reading its text is untested |
| ✅ | Updating a one-VPK install, in the app | three times, the old app closed first |
| ⚠️ | Updating an install made from a computer (0.7.12 or older) | refused in its one console run, and finished by hand; the cause is unconfirmed |
| ❓ | A first install, on a Vita without Rubi | not run on a console yet |
| ✅ | Model sizes Normal and Large | Large fitted in CLANNAD's free memory |
| ❓ | Model size Small | 0.8.0's Small model has not run on a console |
| ❓ | VOCR6, the Small, Normal and Large models on `main` | not run on a console yet ([details](docs/vita-vn-ocr.md#models)) |
| ✅ | Read text: Automatically | the mode of every 0.8.0 test run |
| ❓ | Read text: Only when I press L + R | not run on a console by a 0.8.0 build |
| ⚠️ | Speed boost while reading | the clock goes to 444 MHz and back; the time it saves is unmeasured |
| ❓ | Tapping a word in the game | not checked on a console yet |
| ❓ | Overlay on the game's text (off by default) | not run on a console yet ([details](docs/overlay-on-text.md)) |
| ❓ | Earlier lines (▲ on the text's top row) | not run on a console yet ([line history](docs/line-history.md)) |
| ❓ | The app's Look up tab: typed search, a game's earlier lines, cards from them | not run on a console yet; measured on the host ([details](docs/guide.md#look-up)) |
| ❓ | Vertical text (columns, right to left) | not run on a console yet; measured on the host ([vertical text](docs/vita-vn-ocr.md#vertical-text-vita-vn-ocr-99)) |
| ❓ | The text box suggested on first use (Use this box?) | not run on a console yet; measured on the host ([details](docs/first-box.md)) |
| ❓ | Pause the game to read | not run on a console yet; the game's wait per reading is an estimate ([details](docs/game-pause.md)) |
| ❓ | Characters read, characters/hour and active time (△ in the overlay) | not run on a console yet ([details](docs/ocr-history.md#total-characters-read)) |
| ❓ | The Rubi app's Stats tab: days, heatmap, streak, daily goal, each game | not run on a console yet; measured on the host ([details](docs/guide.md#stats)) |
| ❓ | Default OFF on the Games page | 0.8.0's has not run on a console |
| ❓ | A game's own line finder (its page in the Rubi app) | not run on a console yet ([details](docs/guide.md#a-games-own-page)) |
| ⚠️ | [Native text hooks](#native-text-hooks) | CLANNAD's string-copy extraction verified in the combined debug test build; game-specific hook sites and other titles remain unverified on a console |
| ⚠️ | Anki screenshots | saved in 851 ms; after a restart, the first card can take about 21 s or lose its picture ([#207](https://github.com/bee-san/Rubi_for_vita/issues/207); `main`'s fix untested) |
| ❓ | Trim black bars on Anki screenshots | not run on a console yet ([details](docs/anki.md#black-bars)) |
| ❓ | The Cards page (delete a card, fix its sentence, drop its picture) | not run on a console yet; tested on the host, with a stop at each step ([details](docs/anki.md#the-cards-page)) |
| ❓ | The normal build (the main download) | no console run reported; the test runs used the debug build |

What each build changed, and what its console run showed: the [changelog](CHANGELOG.md).

## Limits

- Vertical text is read only in a text box drawn around it: on a whole screen Automatic reads rows, and ON reads columns
  on most screens, a horizontal game's too (84 % of the hooked ones measured), which gives no sensible text. Not run on
  a console yet ([vertical text](docs/vita-vn-ocr.md#vertical-text-vita-vn-ocr-99)).
- One console has tested Rubi ([Supported](#supported)). OCR accuracy, battery use and CPU3 load are
  not measured on a console yet.
- The game can stutter while Rubi reads ([FAQ](#will-it-slow-my-game-down)).
- A big text box or model may not fit in a game's free memory: Rubi then uses a smaller model, or
  says it can't read ([FAQ](#does-it-work-with-my-game-is-there-enough-memory)).
- A game that uses L + R itself: turn Rubi OFF for it, or choose another button (`toggle_button`).
- Cards reach Anki only as a file that you copy: there is no sync.
- Rubi runs on a PS Vita, not on a PSP.

## FAQ

### Does it drain the battery?

Battery use is not measured yet. With **Read text** on **Automatically** (the default), Rubi reads
each new screen of text in the background, at the lowest priority, with the CPU at 444 MHz while it
recognizes (**Speed boost**). For less work: **Only when I press L + R** reads nothing until you
press (unless the subtitle strip, Select + R, is on), **Speed boost** OFF keeps the game's own
clock, and a game turned OFF gets no OCR at all ([details](docs/auto-vs-press.md)).

### Will it slow my game down?

Reading a screen takes CPU time from the game, so the game can stutter while Rubi reads. On the
owner's console, with a 0.8.0 test build, 291 of 1,609 frames during readings came late (more than
25 ms after the one before), against 52 of 26,941 frames in the time before each reading
([how it was measured](docs/auto-vs-press.md)). With **Only when I press L + R** that happens only
when you press.

### Does it work with my game? Is there enough memory?

Rubi's OCR runs inside the game, in the game's own memory (for a PSP game, the emulator's), never in
the home screen (SceShell) or another system app. Before each reading it checks how much memory the
game has free, leaves the game a reserve, and uses a smaller model when the chosen one doesn't fit;
if even the smallest doesn't, it says so (a tighter text box helps). A game that is ON carries
Rubi's OCR worker from its start: 256 KiB of its memory (220 KiB of module pages, 36 KiB of stack and
TLS: the normal build's figures, this combined test build's; the debug build's pages are 12 KiB more). If a game or homebrew app won't start with it, turn it OFF in the app, or
hold L while you start it. The guide has every byte, in
[Memory and troubleshooting](docs/guide.md#memory-and-troubleshooting).

### Can it break my Vita? How do I undo it?

The app's Install page changes nothing until you choose Install, and it keeps a dated backup of
each taiHEN `config.txt` it changes. If a start of the Vita doesn't finish, the next one is a safe
boot: Rubi installs no hooks and unloads itself, so the Vita starts as if Rubi weren't there
([safe boot](docs/boot-safety.md)). On a console, updating a one-VPK install has worked; updating a
computer install was refused once, and a first install has not run yet ([Supported](#supported)).

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
read, with its game, time, number of characters and whether a native text hook or the OCR read it,
in `ux0:data/VitaJPOverlay/history.csv`, for reading statistics (the CSV that GameSentenceMiner
imports; the Rubi app's **Stats** tab shows them on the Vita, and on a computer
`python3 tools/reading_stats.py history.csv` gives the characters read and per hour, by text source,
per game and per day; [details](docs/ocr-history.md)). It stays on the
Vita too, grows as you read and is never deleted by itself; **Only when I press L + R** saves none.

### How good is the OCR?

Rubi reads text with VOCR, small recognizers that [vita-vn-ocr](https://github.com/bee-san/vita-vn-ocr)
trains for visual-novel dialogue boxes, and decodes each line with a language model. On `main`, all three sizes are
VOCR6, retrained on the owner's hooked PS Vita and PSP games (better on those games; on others level, Small a little
worse in point; measured on a computer). Its accuracy on
a console is not measured yet; the host measurements are in [docs/vita-vn-ocr.md](docs/vita-vn-ocr.md).

### Can I use my own dictionaries?

Yes. Convert Yomitan dictionaries (their ZIP files) on a computer with `tools/convert_dictionary.py`
and copy them to the memory card; up to eight are used, in your order
([how](docs/local-dictionaries.md)).

## Documentation

- [The guide](docs/guide.md): every control and setting, the files and logs, the installer for a
  computer, memory, recovery, building and testing.
- [The changelog](CHANGELOG.md): what each release and test build changed, and what its console run
  showed.
- [Anki cards](docs/anki.md) · [Your own dictionaries](docs/local-dictionaries.md) ·
  [Read text: automatically or on a press](docs/auto-vs-press.md) · [The OCR](docs/vita-vn-ocr.md) ·
  [The one-VPK install](docs/one-vpk-install.md) · [Safe boot](docs/boot-safety.md) ·
  [Vita memory](docs/vita-memory.md) · [Hook requests](docs/hook-requests.md)
- [The roadmap](docs/roadmap.md): what is left to build, ranked, and what of it is built.

## Contributing

Bug reports, ideas and pull requests are welcome in
[issues](https://github.com/bee-san/Rubi_for_vita/issues), and questions on
[Discord](http://discord.skerritt.blog). With a bug, the logs help most: from a computer, with
VitaShell's FTP on, `bash tools/install_ftp.sh --collect VITA_IP` gathers them into one zip
([what it collects](docs/self-test.md#collecting-everything---collect)). A good first contribution:
try Rubi on a Vita Slim, a PS TV or a game nobody has tested yet, and send that zip in an issue; for a game
without a text hook, a [hook request](docs/hook-requests.md) with its `hooks.log`. To
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
  recognizers, VOCR6-S, VOCR6-N and VOCR6-L for Small, Normal and Large (v0.4.1-vocr6.2: VOCR5's checkpoints
  fine-tuned again on the owner's hooked PS Vita and PSP games, Normal with its head gate and two-level head, Large with
  its two-level head), were trained in part on NVIDIA's OCR-Synthetic-Multilingual-v1 (CC BY 4.0), on fonts and text
  that are free for non-commercial use only and on copyrighted game captures used for training only, so their weights
  are for non-commercial use only: their attribution and terms ship beside them as `ocr/VOCR6-ATTRIBUTION.md` and
  `ocr/MODEL-ATTRIBUTION.md` (VOCR5's, the checkpoints they start from; [details](docs/vita-vn-ocr.md#models)). Its character
  language models are CC BY-SA 4.0; the 1 MB and 4 MB ones also carry n-gram counts of the owner's hooked console
  captures (vita-vn-ocr #127). The optional text-presence gate's weights (`ocr_text_gate`, off by
  default) were trained on synthetic text in fonts and text pools free for non-commercial use only and on
  VNDB screenshots, so they are for non-commercial use only too: their attribution ships beside them as
  `ocr/GATE-ATTRIBUTION.md` ([details](docs/vita-vn-ocr.md#text-presence-gate-api-26)).
- **Dictionary:** Bee's Ultimate Dictionary. English definitions from Daijirin4 and Sankoku8
  (© Sanseido) and [JMdict](https://www.edrdg.org/) (EDRDG, CC BY-SA 4.0); frequency ranks from
  [Jiten](https://jiten.moe) (CC BY-SA 4.0); kanji lines from Bee's Ultimate Kanji Dictionary
  (Jiten and KANJIDIC2 data, CC BY-SA 4.0); grammar notes written from Bee's Ultimate Grammar
  Dictionary, whose sources include Yokubi and NINJAL's 日本語文型データベース (CC BY 4.0). Every
  source keeps its own licence: the [attribution](docs/plaintext-attribution.txt) has them all.
- **Anki:** the [Lapis](https://github.com/donkuri/lapis) note type by donkuri (GPL-3.0).
- **Native text hooks:** [LunaTranslator](https://github.com/HIllya51/LunaTranslator) by HIllya51 and
  contributors (GPL-3.0): its PS Vita hook table, and its text clean-up for each game ported to C, from
  release v12.1.2.12 (commit 51884b6), whose source files Rubi keeps in
  [third_party/LunaTranslator/](third_party/LunaTranslator/README.md)
  ([the games](docs/hook-titles.md)).
- **Lookups:** [Yomichan](https://github.com/FooSoft/yomichan)'s deinflection rules (GPL-3.0), with
  [Yomitan](https://github.com/yomidevs/yomitan)'s for spoken forms (しておく, 変わらん, 戻らなきゃ;
  GPL-3.0-or-later), pinned in [third_party/](third_party/README.md). The dictionary's key filter is modelled on the Bloom
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
(`ocr/MODEL-ATTRIBUTION.md`, `ocr/VOCR6-ATTRIBUTION.md`, `ocr/GATE-ATTRIBUTION.md`, [details](docs/vita-vn-ocr.md#models)). The dictionary data,
the language models and the third-party code keep their own licences
([attribution](docs/plaintext-attribution.txt), [third_party/](third_party/README.md)).
