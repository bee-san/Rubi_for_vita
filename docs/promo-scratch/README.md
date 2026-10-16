# Promo video, made from scratch (promo-scratch)

A 44-second promo for Vita JP Overlay in two cuts, 1920×1080 and 1080×1920. It shows one continuous play
session on a white PCH-1000: a visual novel types out, L + R opens the overlay, the cursor walks the
words, × mines cards, the card turns over, the install runs, and a game is turned off in the Overlay app.

Nothing here uses HyperFrames or a video editor. The console is a three.js model built in code
(`src/device.js`), its screen is a canvas redrawn every frame (`src/screen.js`), and every frame is
rendered on its own, from its time alone, in headless Chrome with software WebGL. FFmpeg joins the
frames and the soundtrack.

| | |
| --- | --- |
| Engine | [three.js](https://threejs.org) 0.186.1 (MIT), in chrome-headless-shell 152.0.7977.30 (SwiftShader WebGL 2), driven by puppeteer-core 25.14.0 (Apache-2.0) |
| Encode | FFmpeg n8.1.3: H.264 (yuv420p, CRF 18) + AAC 192 kb/s, 30 fps |
| Sound | promo-assets' music bed and SFX (CC0, self-made), mixed by `tools/audio.py` (numpy 2.3.3), two-pass loudnorm to −16 LUFS, true peak ≤ −1.5 dBTP |
| Inputs | `tools/assets.sh` copies them from promo-assets' shared folder and checks each against `assets.lock` (SHA-256) |

## Re-render

```sh
cd docs/promo-scratch
npm ci
npx @puppeteer/browsers install chrome-headless-shell@152.0.7977.30 --path .cache/browsers
bash tools/assets.sh                         # PROMO_ASSETS=<the promo-assets folder>; checks assets.lock
uv venv .venv && uv pip install -p .venv/bin/python -r tools/requirements-audio.txt
FFMPEG=/path/to/ffmpeg-8.1 WORKERS=4 bash tools/build.sh all     # -> out/media/, out/stills/
```

`tools/render.mjs` serves this folder to the page by request interception: nothing listens on a port
and the page makes no network request. A frame is a pure function of its time (no clock, no
`requestAnimationFrame`, seeded noise), so any frame range can be re-rendered on its own
(`node tools/render.mjs --cut 9x16 --from 600 --to 700`, or `--times 12.5,30`).

## The film

The captions are the owner's script lines (PR #134, 2026-10-16 06:59Z) as the conductor set them on
09:07Z, "the script as written". They live in `src/script.js`, one place to swap a line.

| Time | Caption | What happens |
| --- | --- | --- |
| 0.0–5.0 | Learn Japanese with your PS Vita for free | The game types out a line; × advances it; the camera pulls back from the screen to the console |
| 5.0–10.5 | Look up words from the games you play, entirely offline | L + R pressed together (top-edge view); the overlay comes up on また明日; Right moves to 一緒に |
| 10.5–14.3 | Create flashcards of words you don't know | × saves 一緒に ("Saving card…", "Saved for Anki", queue 1); a card flies out; Right, × saves 見に行く (queue 2) |
| 14.3–16.9 | With screenshots | The 一緒に card comes forward and turns over: its back has the game screen as it was at the × |
| 16.9–21.1 | It only reads the text when it changes | ○ closes the overlay, × advances to a new scene; once the text has stopped typing, one sweep reads it; the app's Read text setting |
| 21.1–26.5 | Easy install | The release page, the .vpk copied to the Vita, VitaShell's install, the Overlay app's Install page, the restart |
| 26.5–34.7 | Now it's installed, go to your game / and press L + R to look up words. | Home, the game's LiveArea, Start; the hilltop scene types; L + R; キラキラ → 子供の頃 → みたい |
| 34.7–41.3 | Turn it off for specific games, so it doesn't get in the way | PS, the Overlay app from the home screen, the Games page: Neon Drift OFF ("Saved. Resume or restart your game to apply."); PS, Continue, the game goes on |
| 41.3–44.0 | (end card) | Vita JP Overlay · Free · Offline · Open source · github.com/bee-san/vita-jp-overlay |

The 1080×1920 cut has the same events, reframed: the caption above the console, and below it the
card deck, the panels, the install steps and a large call-out of each looked-up word.

## What is real and what is a mock-up

Real, from `main`:
- The overlay (promo-assets' `overlay/` layers: main's layout, colours and strings, and the lookups
  `vjo_overlay_lookup_at` returns from the release's dictionary, sha256 e1bbd379…). Every word shown is a
  JMdict entry: また明日 #3726, 一緒に #125, 見に行く #2199, キラキラ #3383, 子供の頃 #3356, みたい #95.
- With card mining on, the strings and the queue count after each × ("Saving card…", "Saved for Anki",
  "Queued for Anki", "Anki queue: N · export in the Overlay app"); the s3 beats are the fresh-install
  look (mining is off by default: no queue line, no "× queue").
- The Games page: real Vita3K captures of main's Overlay app (promo-assets' `app/main-1ed95ca/`).
- Line 5's caption: `ocr_mode = auto` is the default (`core/config.c`), and a settled new screen is read
  once in the background; an unchanged screen is not read again. No battery claim is made.

Mock-ups of features landing for test.4 (named in the preview comment; swap in real captures when they merge):

| Time | Shot | Branch |
| --- | --- | --- |
| 14.3–16.9 | The Lapis card with its game screenshot (made by that branch's encoder and exporter, rendered by Anki 26.09.3) | overlay-anki-screenshot (`feat/anki-screenshot`) |
| 19.0–21.0 | The app's "Read text" setting (a Vita3K capture of the PR branch) | #146 (`feat/ocr-mode-setting` 21488a7) |
| 21.1–26.5 | The one-VPK install: the release's assets, VitaShell's install prompts, the Overlay app's Install page (drawn here from the branch's strings until a capture lands) | `feat/one-vpk-install` |

Simplified renditions (not our UI, drawn here): the home screen, the LiveArea pages, the browser and
VitaShell (VitaShell 2.02's strings; a stand-in until promo-assets' captures land). The game is an
original mock visual novel, 夕凪ステップ (promo-assets: CC0). The sweep over the text box, the press
rings, the touch ripples and the word call-outs are the video's own graphics.

## Licences

- This folder's code: GPL-3.0, as the repository.
- promo-assets' inputs, per their MANIFEST.md: the VN art, icons, music and SFX are CC0-1.0; the overlay
  layers and app captures are GPL-3.0-or-later (the app) with CC BY-SA 4.0 dictionary content; the
  Lapis card renders are GPL-3.0-or-later (Lapis 1.7.0) + CC BY-SA 4.0 (content).
- Noto Sans JP: SIL OFL 1.1 (`assets/shared/fonts/OFL-NotoSansJP.txt`).
- Dictionary content: JMdict (EDRDG, CC BY-SA 4.0); frequency ranks: the Jiten frequency list (CC BY-SA 4.0).
- No Sony logos, product photos or game art.
