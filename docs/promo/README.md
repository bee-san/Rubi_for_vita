# Promo video

A 45-second promo for Rubi (Rubi for Vita), written as a [HyperFrames](https://hyperframes.heygen.com)
composition: HTML, CSS and one GSAP timeline, rendered frame by frame in headless Chrome. It follows the
owner's script in his order, as someone playing a visual novel would: the text types, L + R opens the
overlay, Right walks the words, × mines a card, and the camera follows each press. Everything the console
shows is the software's own: the overlay as its code lays it out, with its own lookups; the card the app
exports, as Anki renders it; the app's pages as its code draws them in Vita3K (see below).

| File | What it is |
| --- | --- |
| `media/promo-1920x1080.mp4` | 1920 × 1080, 30 fps, H.264 and AAC, 45 s, -16 LUFS |
| `media/promo-1080x1920.mp4` | 1080 × 1920, the vertical cut (its own camera, not a crop) |
| `media/promo.gif`, `media/poster.jpg`, `media/stills/` | a loop of the lookup and the mining, the poster, one still per beat |

## Re-render

You need Node.js 22 or newer, FFmpeg and FFprobe on `PATH`, and Python 3 with Pillow and fontTools
(`pip install -r tools/requirements.txt`: pinned).

```sh
cd docs/promo
npm ci                              # hyperframes 0.8.162 and gsap 3.14.2, both pinned
npx hyperframes browser ensure      # the chrome-headless-shell HyperFrames renders with
bash capture/run.sh CHECKOUT CAP    # the real captures of a built checkout (below)
python3 tools/prep_assets.py CAP CHECKOUT PROMO_ASSETS   # the captures, the app's art, the mock game and the sound -> assets/, src/
npm run build && npm run fonts && npm run build
npm run check                       # index.html fresh, the font subsets fresh, hyperframes check
npm run render                      # renders/promo-1920x1080.mp4
npm run render:vertical             # renders/promo-1080x1920.mp4 (builds the portrait index.html, renders, restores)
npm run media                       # media/: the films at -16 LUFS, the GIF, the poster, the stills
python3 tools/motion.py media/promo-1920x1080.mp4   # still holds, frame to frame
```

`node tools/build.mjs --scale 0.5` builds a half-size draft (the whole film drawn into a 960 × 540 frame);
render it with `-f 15` for a quick look. The npm scripts set `HYPERFRAMES_NO_TELEMETRY=1`.

| Path | What it is |
| --- | --- |
| `src/script.mjs` | the words and the timing: scene starts, the cue sheet of every press and tap, captions, the sound cues, the names |
| `src/promo.template.html`, `src/promo.css`, `src/promo.js` | the markup, the styles, and the one paused GSAP timeline: the camera rig, the hands, the presses, every screen change |
| `src/overlay-states.json`, `src/release.json` | the overlay at each beat and the release's files, from `capture/run.sh` |
| `capture/` | the tools that make every real picture from one checkout (below) |
| `tools/build.mjs` | builds `index.html` (`--layout landscape` or `portrait`, `--scale`); `--check` fails when it is stale |
| `tools/prep_assets.py` | copies the captures, the app's art, the mock game and the sound into `assets/`, with their sha256 in `assets/SOURCES.md` |
| `tools/make_bed.py` | the music: promo-assets' 44 s bed with one second more in its breakdown (45 s) |
| `tools/fonts.py` | fetches the fonts at a pinned google/fonts commit, checks their SHA-256 and subsets them |
| `tools/media.sh`, `tools/motion.py` | the media from the renders; a frame-to-frame motion check |

## The captures (`capture/run.sh`)

`capture/run.sh CHECKOUT OUT` makes every real picture from one vita-jp-overlay checkout, built as the
repository's README says (the host tree in `build/host`, the Vita release in `build/vita-normal` and
`build/vita-debug`); the film shows the commit in `OUT/commit.txt`. Its environment is listed at its top
(VitaSDK, the pinned vita-vn-ocr runtime and models, the released dictionary, Vita3K and the official
firmware, docker for Anki). `PROMO_ASSETS` is the folder the fleet's promo-assets agent made for the film: the
original mock game (its art, made with FLUX.1 [schnell] and released CC0), the CC0 sound, and the tools that made
them (its `MANIFEST.md` has every file's licence, source and sha256).

- **The overlay** (`overlay_lookups.c`, `overlay_states.py`): the checkout's own lookups in the released
  `plaintext.vjdict` (its sha256 is checked against `docs/plaintext-dictionaries.json`), each entry drawn by
  `vjo_styled_entry`, laid out in the order `shell/overlay.cpp` `render()` writes the body, with its strings
  read from `shell/overlay.cpp` and `shell/anki.c` (a string that is gone stops the run). Right moves as the
  overlay's arrows do (`vjo_text_row_key`, along the header's displayed rows, over the host tests' renderer
  double), and a tap looks up at the tapped letter: scene 2's two Rights move on a one-row header (the run
  stops if they do not), and on scene 7's two-row header the film taps 子 (on 68fc2d9 Right from キラキラ
  stays on its row; the tap is true either way). `ocr_check.py`: the
  checkout's recognizer reads each line the film types exactly as the film types it (`OUT/ocr-check.md`).
- **The card** (`anki_card.c`, `anki_cards.py`, `render_cards.js`): the game's frame when × is pressed,
  encoded by the game worker's JPEG encoder at its quality (host `vjo-jpeg`, `core/jpegsw.c` at 70); the
  Shell's note and queue record with that screenshot (`vjo_anki_note_from_entry`, `card_facts`'s values,
  `vjo_queue_encode`); the app's export (`vjo_export_run`) into a real `.apkg`; imported by Anki 26.09.3
  (`tools/requirements-anki.txt`) and rendered by Anki itself, then drawn in Chrome.
- **The release** (`run.sh`): `tools/package_variants.py` of the two builds, as a release makes it: the
  file names and sizes on the Releases page and in VitaShell's list.
- **The app** (`app_capture_patch.py`, `capture_package.py`, `capture_app.sh`, `split_holds.py`): the
  app's Vita3K smoke build with the promo's pad scripts (and two Vita3K workarounds under `VJO_SMOKE_PAD`),
  its eboot put in the release's app VPK, the release packaged around it by the checkout's own
  `package_release.py` and `package_vpk.py`, and that one VPK installed in Vita3K on a card with HENkaku's
  stock taiHEN configurations: the app opens on its Install page, Install runs on Vita3K's card (it copies
  the plugins and the models, moves the dictionary, backs up and edits both `config.txt`, writes
  `installed.txt`: `OUT/app/run/card-after-install.txt`), then the app started again opens on Games. The
  shots are its own drawing; `tools/prep_assets.py` names the holds it uses. Vita3K says nothing about the
  console ([AGENTS.md](../../AGENTS.md) rule 5).

## Scenes

| # | Time | Caption | On the console |
| --- | --- | --- | --- |
| 1 | 0–4.5 s | Learn Japanese with your PS&nbsp;Vita *for free* | The mock visual novel types its line; the camera pulls back to the Vita; × shows the next line. |
| 2 | 4.5–9.5 s | Look up words from the games you play, *entirely offline* | L + R (two fingers on the shoulders) opens the overlay on また明日; Right goes to 一緒に. |
| 3 | 9.5–14 s | Create flashcards of *words you don't know* | × saves 一緒に with its screenshot; Right, × saves 見に行く; the cards fly to the queue (0 → 1 → 2). |
| 4 | 14–17.6 s | With *screenshots* | The card in Anki (Lapis): its front turns to its back, the game's screen in its Picture. |
| 5 | 17.6–22.4 s | It only reads text *when it changes* / or only when you press L + R | ○, × shows a new scene and line; the app's Read text goes to "Only when I press L + R". |
| 6 | 22.4–32 s | *Easy install* | GitHub Releases, the .vpk to the Vita; VitaShell: Down to it, × and its two questions; PS, a flick to the home screen; the app's Install page and its run; the user holds the power button and selects Power Off; done. |
| 7 | 32–37.75 s | Now it's installed: go to your game and press *L&nbsp;+&nbsp;R* to look up words. | (A cut: the Vita on again.) The game starts from its bubble; L + R opens the overlay on キラキラ; a tap on 子 looks up 子供の頃. |
| 8 | 37.75–43 s | Turn it off for specific games, *so it doesn't get in the way* | ○, PS (the game stays suspended), the home screen, the app: Down, Down, × turns Ember Garden OFF. |
| | 43–45 s | End card | Rubi for Vita: free, fully offline, open source; the project's address. |

The owner's note on the first cut (#134, 2026-10-16 14:15Z: "the pressing is a little too fast, slow it
down a tad"): every press now sinks, holds and comes back over 0.3 s (0.2 s before), each finger comes in
and goes out more slowly, and no two presses are closer than 0.5 s (v2 had twelve gaps under that). The
film is a second longer, 45 s, and the install, the game and turning a game off have more time. His frames 1
and 2 (the export page; "It only runs in the games you turn on") are not in the film.

## What the video claims, and why it is true

Checked against the checkout the captures are of (`assets/SOURCES.md` names it).

- **Free**: GPL-3.0; the release bundles the dictionary and the models.
- **Entirely offline**: VOCR, the only engine, runs in the game's process, and the bundled
  `plaintext.vjdict` is read from the memory card; there is no network code (#130, #145). The lookups shown
  are the checkout's own, in the released dictionary: また明日 #3726, 一緒に #125, 見に行く #2199
  (見に行こう deinflected), キラキラ #3383, 子供の頃 #3356. Their definitions are JMdict's, and their kanji
  lines (一=one, ...) are Bee's Ultimate Kanji Dictionary's, both as the bundle carries them.
- **Flashcards**: with card mining on, × queues the word on the memory card ("Saving card…", "Saved for
  Anki", "Queued for Anki", "Anki queue: N · export in the … app"). Mining is off by default, so the overlay
  after a first install (scene 7) has no queue line and no "× queue".
- **With screenshots**: the card keeps the game's whole frame when × is pressed, without the overlay (#150;
  the app's Screenshots setting is on by default); the card in scene 4 is the app's own export of it.
- **It only reads text when it changes**: Read text on Automatically (the default) reads each new screen
  once it settles; "Only when I press L + R" reads nothing until the press (#146). No battery claim: battery
  use is not measured.
- **Easy install**: the one-VPK install (#158; the README's Install, `docs/one-vpk-install.md`): the
  release's VPK, copied to the Vita and installed in VitaShell (its own two questions), then the app's
  Install page, then a restart by the user (the app never restarts the Vita). Every game is ON by default
  and the kernel loads the OCR worker into each (#148), so no per-game step follows. Not yet verified on a
  console: the owner's test.4 run checks it, and the film is published as final only once it does.
- **L + R**: the default `toggle_button` is `l+r`.
- **Turn it off per game**: the app's Games page; the status line is the app's own.

Drawn by the film from the code, not captured: the overlay, from `src/overlay-states.json` (its strings,
colours, sizes and order are the checkout's; Noto Sans JP stands in for the console's system font, and its
line heights, 38 px for 27 px text and 30 px for 21 px, are estimates, not measured on a console; the overlay
itself opens with no animation: its short rise is the film's). Drawn by the film itself: the console (with its
power button on the top edge) and the hands; the home screen and the LiveArea pages (simplified: the game's,
VitaShell's and the app's, with the app's own icon, name and LiveArea art; PS on an app shows its LiveArea,
and a flick reaches the home screen, as the console's guide says); VitaShell's screens, from VitaShell 2.02's
layout, Default theme and strings, with the release's file and its size as VitaShell writes it; the Releases
page, with the release's files and sizes as GitHub lists and writes them (by name, then the two source
archives); the restart: a finger holds the power button while a ring fills (the guide's two seconds, drawn
shorter), selects [Power Off] on the screen that appears (drawn simplified), and the screen goes dark; the
film then cuts to the Vita on again (the guide: hold the power button for two seconds to turn it on). The
release is the one its checkout builds (version 0.8.0); the published release's sizes may differ by a few
bytes.

## Licences

| Part | Source | Licence |
| --- | --- | --- |
| The composition's code (`src/`, `tools/`, `capture/`, `index.html`) | this project | GPL-3.0-only |
| The console drawing (`assets/device/vita-white.svg`) | drawn from scratch from the console's published dimensions; no photo traced, no logos | CC0-1.0 |
| The mock game 夕凪ステップ (`assets/vn/`) and its icon (`assets/icons/PROMO0001.png`) | promo-assets: original art made with FLUX.1 [schnell] (Apache-2.0), released CC0 (`assets/SOURCES.md`) | CC0-1.0 |
| The other games' icons (`assets/icons/PROMO0002-5.png`) | promo-assets: gradients with a letter in Zen Maru Gothic (OFL), drawn by its `make_promo_games.py` (`capture/make_promo_games.py`) | CC0-1.0 |
| VitaShell's bubble (`assets/icons/vitashell.svg`) | a generic file-manager icon, drawn from scratch for promo v2 | GPL-3.0-only |
| The app's pages (`assets/shots/`), icon and LiveArea art (`assets/app/`) | this project's app, captured in Vita3K; copies of `settings/assets/` | GPL-3.0-only |
| The Anki card (`assets/anki/`) | Lapis 1.7.0's note type as Anki renders it, with JMdict and Bee's Ultimate Kanji Dictionary content | GPL-3.0 (Lapis), CC BY-SA 4.0 (content) |
| The music and the sound effects (`assets/audio/`) | promo-assets: synthesised, no samples; the bed lengthened by `tools/make_bed.py` | CC0-1.0 |
| Noto Sans JP, Zen Maru Gothic (`assets/fonts/`, subsets) | [google/fonts@9430147](https://github.com/google/fonts/tree/94301472b4d809299b4775bc6502592b83dd6934/ofl) | SIL OFL 1.1 |
| The dictionary entries, kanji lines and ranks | JMdict (EDRDG); Bee's Ultimate Kanji Dictionary (Jiten, KANJIDIC2); the Jiten frequency list | CC BY-SA 4.0 |
| VitaShell's strings, colours and layout | [VitaShell](https://github.com/TheOfficialFloW/VitaShell), recreated in HTML | GPL-3.0 |
| HyperFrames, GSAP (`npm ci`, not committed) | heygen-com/hyperframes 0.8.162; gsap 3.14.2 | Apache-2.0; GreenSock's standard licence |

The video is not affiliated with or endorsed by Sony Interactive Entertainment. "PS Vita" names the console
the software runs on.
