# Promo video (v2)

A 44-second promo for Vita JP Overlay, written as a [HyperFrames](https://hyperframes.heygen.com)
composition: HTML, CSS and one GSAP timeline, rendered frame by frame in headless Chrome. It follows the
owner's script in his order, as someone playing a visual novel would: the text types, L + R opens the
overlay, Right walks the words, × mines a card, and the camera follows each press.

| File | What it is |
| --- | --- |
| `media/promo-1920x1080.mp4` | 1920 × 1080, 30 fps, H.264 and AAC, 44 s, -16 LUFS |
| `media/promo-1080x1920.mp4` | 1080 × 1920, the vertical cut (its own camera, not a crop) |
| `media/promo.gif`, `media/poster.jpg`, `media/stills/` | a loop of the lookup and the mining, the poster, one still per scene |

## Re-render

You need Node.js 22 or newer, FFmpeg and FFprobe on `PATH`, and Python 3 with Pillow and fontTools
(`pip install -r tools/requirements.txt`: pinned).

```sh
cd docs/promo
npm ci                              # hyperframes 0.8.162 and gsap 3.14.2, both pinned
npx hyperframes browser ensure      # the chrome-headless-shell HyperFrames renders with
npm run prep                        # the shared art, captures and sound -> assets/ (tools/prep_assets.py)
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
| `src/script.mjs` | the words and the timing: scene starts, the cue sheet of every press and tap, captions, the sound cues |
| `src/promo.template.html`, `src/promo.css`, `src/promo.js` | the markup, the styles, and the one paused GSAP timeline: the camera rig, the hands, the presses, every screen change |
| `src/overlay-states.json` | the overlay at each beat, as main lays it out, with main's own lookups in the released dictionary |
| `tools/build.mjs` | builds `index.html` (`--layout landscape` or `portrait`, `--scale`); `--check` fails when it is stale |
| `tools/prep_assets.py` | copies the shared art, the Vita3K captures and the sound into `assets/`, with their sha256 in `assets/SOURCES.md` |
| `tools/fonts.py` | fetches the fonts at a pinned google/fonts commit, checks their SHA-256 and subsets them |
| `tools/media.sh`, `tools/motion.py` | the media from the renders; a frame-to-frame motion check |

## Scenes

| # | Time | Caption | On the console |
| --- | --- | --- | --- |
| 1 | 0–4.5 s | Learn Japanese with your PS Vita *for free* | The mock visual novel types its line; the camera pulls back to the Vita; × shows the next line. |
| 2 | 4.5–10 s | Look up words from the games you play, *entirely offline* | L + R (two fingers on the shoulders) opens the overlay on また明日; Right goes to 一緒に. |
| 3 | 10–14.5 s | Create flashcards of *words you don't know* | × saves 一緒に with its screenshot; Right, × saves 見に行く; the cards fly to the queue (0 → 1 → 2). |
| 4 | 14.5–19 s | With *screenshots* | The card in Anki (Lapis), the game's screen in its Picture. |
| 5 | 19–23.5 s | It only reads text *when it changes* / Or only when you press L + R | ○, × shows a new scene and line; the Overlay app's Read text goes to "Only when I press L + R". |
| 6 | 23.5–31 s | *Easy install* | GitHub Releases, the .vpk to the Vita, VitaShell installs it, Overlay's Install page, a restart, done. |
| 7 | 31–37 s | Now it's installed: go to your game and press *L + R* to look up words. | The game starts from its bubble; L + R opens the overlay on キラキラ; Right, Right. |
| 8 | 37–41.5 s | Turn it off for specific games, *so it doesn't get in the way* | ○, PS (the game stays suspended), the home screen, the Overlay app: Neon Drift turned OFF. |
| | 41.5–44 s | End card | Free, fully offline, open source; the project's address. |

## Mock-ups of features still landing

Three shots show features that are not on `main` yet, as their branches build them. Each stays a mock-up
until its feature merges (and, for the install, until the owner's console check), and the real captures
replace it then:

| Scenes | Feature | What the shot is |
| --- | --- | --- |
| 3, 4 | the card's screenshot (overlay-anki-screenshot) | the game's frame as that branch's encoder makes it (960 × 544, quality 70, without the overlay); the card made by its exporter and rendered by Anki 26.09.3 (Lapis 1.7.0) |
| 5 | Read text (PR #146, feat/ocr-mode-setting) | Vita3K captures of that branch's Overlay app |
| 6 | the one-VPK install (overlay-one-vpk-install) | the release's asset names and the ~53 MB size (an estimate) from its design; VitaShell's own strings; the Install page drawn by the app's own code in Vita3K from the design (`docs/one-vpk-install.md`) |

## What the video claims, and why it is true

- **Free**: GPL-3.0; the release bundles the dictionary and the models.
- **Entirely offline**: VOCR, the only engine since #130, runs in the game's process, and the bundled
  `plaintext.vjdict` is read from the memory card. The lookups shown are main's own, in the released
  dictionary (sha256 e1bbd379…): また明日 #3726, 一緒に #125, 見に行く #2199 (見に行こう deinflected),
  キラキラ #3383, 子供の頃 #3356, みたい #95, all JMdict entries with their Jiten ranks.
- **Flashcards**: with card mining on, × queues the word on the memory card ("Saving card…", "Saved for
  Anki", "Queued for Anki", "Anki queue: N · export in the Overlay app"). Mining is off by default, so the
  overlay after a fresh install (scene 7) has no queue line and no "× queue".
- **With screenshots**: the anki-screenshot feature (above): the card keeps the game's whole frame when ×
  is pressed, without the overlay.
- **It only reads text when it changes**: with Read text on Automatically (the default), each new screen is
  read once after it settles; "Only when I press L + R" reads nothing until the press. No battery claim:
  battery use is not measured.
- **Easy install**: the one-VPK flow (above); the app never restarts the Vita itself.
- **L + R**: the default `toggle_button` is `l+r`.
- **Turn it off per game**: the Overlay app's Games page; the status line is the app's own.

## Licences

| Part | Source | Licence |
| --- | --- | --- |
| The composition's code (`src/`, `tools/`, `index.html`) | this project | GPL-3.0-only |
| The console drawing (`assets/device/vita-white.svg`) | drawn from scratch from the console's published dimensions; no photo traced, no logos | CC0-1.0 |
| The mock game 夕凪ステップ (`assets/vn/`), the game icons (`assets/icons/`) | promo-assets: original art made with FLUX.1 [schnell] (Apache-2.0), released CC0 (`assets/SOURCES.md`) | CC0-1.0 |
| The Overlay app's pages (`assets/shots/`), icon and LiveArea art (`assets/app/`) | this project's app, captured in Vita3K; copies of `settings/assets/` | GPL-3.0-only |
| The Anki card (`assets/anki/`) | Lapis 1.7.0's note type as Anki renders it, with JMdict content | GPL-3.0 (Lapis), CC BY-SA 4.0 (JMdict) |
| The music and the sound effects (`assets/audio/`) | promo-assets: synthesised, no samples | CC0-1.0 |
| Noto Sans JP, Zen Maru Gothic (`assets/fonts/`, subsets) | [google/fonts@9430147](https://github.com/google/fonts/tree/94301472b4d809299b4775bc6502592b83dd6934/ofl) | SIL OFL 1.1 |
| The dictionary entries and their ranks | JMdict (EDRDG), the Jiten frequency list | CC BY-SA 4.0 |
| VitaShell's strings, colours and layout | [VitaShell](https://github.com/TheOfficialFloW/VitaShell), recreated in HTML | GPL-3.0 |
| HyperFrames, GSAP (`npm ci`, not committed) | heygen-com/hyperframes 0.8.162; gsap 3.14.2 | Apache-2.0; GreenSock's standard licence |

The video is not affiliated with or endorsed by Sony Interactive Entertainment. "PS Vita" names the console
the software runs on.
