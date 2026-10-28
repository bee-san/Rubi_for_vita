# Settings and colour schemes

Rubi opens on **Settings**, with Games, Reading & dictionaries, Anki, Appearance, and Install & repair.
The top bar has only **Look up** and **Settings**. Cancel/Back goes up a level; Select switches destinations.
Technical reading options are under **Advanced reading**.

**Settings → Appearance** offers exactly three schemes. Tap a preview card, use Left/Right, or press Enter to cycle.
Every app page changes immediately; `colour_scheme` in `app.ini` keeps the choice after closing the app.
Mint is the default for existing installations and unknown values. The in-game overlay keeps its existing colours.

These are **host-rendered mock screenshots**, not physical Vita captures. They come from the actual
`settings/main.c` input/draw loop and real preference saves in `host/tests/test_settings_app.c`.
The harness supplies the rectangles, text positions, colours and clipping. Its substitute font metrics and the
renderer’s Noto Sans CJK JP font approximate firmware PVF; hardware layout and performance remain unverified.
Each PNG labels that distinction below the 960 × 544 app viewport.

The category menu:

| Mint | Sakura | Paper |
| --- | --- | --- |
| ![Mint Settings menu](mint-home.png) | ![Sakura Settings menu](sakura-home.png) | ![Paper Settings menu](paper-home.png) |

Reading and appearance:

| Mint · navy and mint | Sakura · plum and pink | Paper · warm light and green |
| --- | --- | --- |
| ![Mint settings](mint.png) | ![Sakura settings](sakura.png) | ![Paper settings](paper.png) |
| ![Mint picker](mint-picker.png) | ![Sakura picker](sakura-picker.png) | ![Paper picker](paper-picker.png) |

Advanced reading keeps hook and OCR tuning out of the usual controls:

![Advanced reading](reading-advanced.png)

## Recreate the previews

Build the host UI harness, then use Pillow and a Japanese OpenType font (the committed previews use
NotoSansCJKjp-Regular.otf, distributed under the SIL Open Font License). The font is not bundled with the app.

```sh
cmake -S . -B build/host -G Ninja -DVJO_WITH_VOCR=OFF
cmake --build build/host --target vjo-settings-app-tests
python3 tools/render_theme_previews.py --font /path/to/NotoSansCJKjp-Regular.otf
```

The renderer runs `settings_navigation` and `app_colour_schemes`, which also checks controller/touch choices, persistence, a failed save,
an unreadable file, backup recovery and independence from an unreadable `config.ini`. The app's existing
preference and lookup suites cover value round trips, file preservation and themed dictionary rows.
