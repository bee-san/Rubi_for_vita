# Promo video

A 44-second promo for Vita JP Overlay, written as a [HyperFrames](https://hyperframes.heygen.com)
composition (HTML, CSS and a GSAP timeline, rendered frame by frame in headless Chrome).

| File | |
| --- | --- |
| {{MEDIA_LANDSCAPE}} | 1920 × 1080, 30 fps, H.264 + AAC |
| {{MEDIA_PORTRAIT}} | 1080 × 1920, the vertical cut |
| [`media/poster.jpg`](media/poster.jpg), [`media/promo.gif`](media/promo.gif) | the poster frame and the README loop |
| [`media/stills/`](media/stills/) | one still per scene |

Everything on the console's screen is recreated from this repository's code (the overlay from
`shell/rco/vitajpoverlay.xml`, `shell/overlay.cpp` and `core/styled.h`; the Overlay app from
`settings/main.c`, `settings/engine.c` and `settings/anki_page.c`; the app's own LiveArea art from
`settings/assets/`). The game is an original mock visual novel made for this video. No game,
console or Sony artwork is used.

## Re-render

Needs Node.js 22 or newer, FFmpeg and FFprobe on `PATH`, and Python 3 with fontTools for the fonts.

```sh
cd docs/promo
npm ci                              # hyperframes 0.8.162 and gsap 3.14.2, both pinned
npx hyperframes browser ensure      # the pinned chrome-headless-shell HyperFrames renders with
npm run build                       # src/ -> index.html (the landscape composition)
npx hyperframes lint . && npx hyperframes check .
npm run render                      # renders/promo-1920x1080.mp4
npm run render:vertical             # renders/promo-1080x1920.mp4 (builds the portrait index.html, renders, restores)
```

`npx hyperframes preview` opens the composition in HyperFrames Studio. A render takes about
1.5 minutes per cut with four workers (`--workers 4`).

The sources:

| Path | What it is |
| --- | --- |
| `src/promo.template.html` | the markup of every scene and screen |
| `src/promo.css`, `src/promo.js` | the styles and the one paused GSAP timeline |
| `src/script.mjs` | the words and the timing: captions, dialogue, terminal output, sound cues |
| `tools/build.mjs` | builds `index.html` (`--layout landscape` or `portrait`) and draws the Overlay app pages as SVG with `settings/main.c`'s geometry, colours and strings; `--check` fails when `index.html` is stale |
| `tools/fonts.py` | fetches the fonts at a pinned google/fonts commit, checks their SHA-256 and subsets them to the characters the sources use (`tools/requirements-fonts.txt`); run it after changing on-screen text |
| `tools/make_audio.py` | synthesizes the music and sound effects from code (numpy 2.3.3, then FFmpeg `loudnorm`) |
| `tools/make_stars.py` | bakes the night sky's stars and skyline into `assets/vn/vn-bg.svg` and `vn-livearea.svg` (fixed seeds) |

## Scenes

{{SCENES}}

## What the video claims

Each caption was checked against `main` at {{MAIN_SHA}}. Where the script's line was not true of
today's code, the video says the true thing instead:

{{CLAIMS}}

## Licences

| Part | Source | Licence |
| --- | --- | --- |
| Composition code (`src/`, `tools/`, `index.html`) | this project | GPL-3.0-only |
| Device drawing (`assets/device/vita-white.svg`) | drawn from scratch for this video from the console's published dimensions (182 × 83.5 mm, 960 × 544 screen); no photo traced; no logos or trademarks | CC0-1.0 |
| Mock game art and the fictional game icons (`assets/vn/`) | drawn from scratch for this video | GPL-3.0-only |
| The Overlay app's icon, LiveArea background and gate (`assets/app/`) | copies of `settings/assets/` ([provenance](../../settings/assets/ICON-SOURCE.md)) | GPL-3.0-only |
| Music and sound effects (`assets/audio/`) | synthesized by `tools/make_audio.py` | CC0-1.0 |
| Noto Sans JP, Zen Maru Gothic, JetBrains Mono (`assets/fonts/`, subsets) | [google/fonts@9430147](https://github.com/google/fonts/tree/94301472b4d809299b4775bc6502592b83dd6934/ofl) | SIL OFL 1.1 (`assets/fonts/OFL-*.txt`) |
| Dictionary entries shown (キラキラ, 一緒に) | JMdict, © the Electronic Dictionary Research and Development Group, as converted in the bundled `plaintext.vjdict` | CC BY-SA 4.0 ([licence](../JMdict-LICENSE.html)) |
| The Anki card's design | the [Lapis](https://github.com/donkuri/lapis) note type, recreated in HTML | GPL-3.0 |
| HyperFrames (render tool, `npm ci`, not committed) | [heygen-com/hyperframes](https://github.com/heygen-com/hyperframes) 0.8.162 | Apache-2.0 |
| GSAP (animation runtime, `npm ci`, not committed) | [gsap](https://gsap.com) 3.14.2 | GSAP Standard "no charge" licence |

The video is not affiliated with or endorsed by Sony Interactive Entertainment. "PS Vita" names the
console the software runs on.
