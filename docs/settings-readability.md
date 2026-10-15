# Settings readability on the Vita

The standalone Settings app previously used five 56-pixel game rows and small
PVF drawing scales, with dense hints and a narrow page-switch touch area.
This change affects Settings only; the in-game overlay and OCR workers are
unchanged.

The 960 × 544 layout now shows three 80-pixel game cards. Game titles have two
measured-width lines; their IDs occupy a separate right column below a clear
ON/OFF badge. The page tabs and the two direct engine buttons (the local
engine, named after its model's generation, and Google Lens) have 48-pixel
touch heights. The focused card has a bright border. Save failures have a distinct
error color, and status messages have two lines above the control hints.
Only three icon textures are held at once, retaining the existing unload-on-scroll
ownership.

Games-page controls keep their existing actions. Touch can select either page,
toggle a game or setting, or choose
the local engine or Google Lens directly. Left chooses the local engine, Right
Google Lens; Cross switches to the other one. Touching the OCR heading, help text
or gaps only focuses that row, so it cannot accidentally enable online OCR.
The selected engine's description identifies local/manual behavior or Lens's
Wi-Fi requirement and image upload. Model availability is shown only for the
selected local engine. Game policy and preference loading/saving are unchanged.
The app still re-reads after a save failure to display the actual saved choices;
a failure is never presented as a successful save.

The OCR & lookup page has a fourth card, **Dictionaries shown**, with eight
number badges (44 × 34 pixels, 4 pixels apart). To fit it, the three smaller
cards are 68 pixels tall, their badges 34 pixels, and the page's status is one
line. The OCR engine card and the Games page are unchanged. Touching a number
chooses it; the card's title and help only focus it.
[A Vita3K render](settings-dictionaries-vita3k.png) shows the page with three
enabled dictionaries and 2 selected. It used the official 3.74 firmware fonts
and a build that opens on this page, because injected keys did not reach the
emulated app; it is not a physical-device check.

## Font measurement

The public `vita2d_pvf_text_width` API measures wrapping, centering and ellipsis.
UTF-8 decoding determines every line boundary. The drawing scales target 24-pixel
body, 26-pixel game-title and 32-pixel header **em advances**, calibrated from
the largest positive scale-1 advance of `あ`, `漢` and `Ｗ`. An 18-pixel em fallback
is used if none can be measured. These are not measurements of firmware glyph
bitmap heights or bearings.

PVF's nominal `text_height` is not an adequate bitmap-height measurement. The
supported libvita2d interface exposes scale and font selection, but no public
PVF raster-size setter or underlying font handle. The app scales the existing
system-font raster; it can appear less crisp when enlarged. Titles, preference
content and status regions are clipped to their own boundaries. Actual firmware
font bounds and physical readability still require native visual review.

## Validation

A private host harness compiled the complete, unchanged Settings input/draw
loop with the real `core/games.c`, `core/utf.c` and `core/arena.c`. Bounded stubs
supplied controller/touch events, storage outcomes and PVF/GPU calls. Synthetic
seven-game data included Latin, Japanese and supplementary characters; no
installed-game names, screenshots or fonts were committed.

Twelve scenarios passed with Clang AddressSanitizer and UndefinedBehaviorSanitizer:
page navigation and list-end clamps; third-card touch; row/tab gaps; touch and
controller page switching; direct engine choices; Japanese-selection toggles;
game/preference save failures; unreadable preferences; empty filters; and UTF-8
wrapping. The command shape was:

```sh
clang -std=c11 -DVJO_HOST=1 -O1 -g -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Wall -Wextra -I /path/to/qa/stubs \
  /path/to/qa/harness.c core/games.c core/utf.c core/arena.c \
  -o /path/to/qa/harness
# Run scenarios 1 through 12, each in a separate process.
```

Private 960 × 544 previews replayed the actual draw calls using a local Noto CJK
font and approximate host advances. Games, OCR and failed-save views had no
viewport overflow in those previews. They validate the proposed layout under
those substitute metrics; they are not Vita screenshots, firmware glyph
measurements, or physical readability proof. Existing persistence tests and
the native Settings VPK build remain separate checks.

## Native installation and controls checkpoint

The v0.7.11 OCR selector was checked with 22 scenarios through the actual
Settings input/draw loop under AddressSanitizer and UndefinedBehaviorSanitizer.
These include all three direct engine choices, forward/backward cycling and
wraparound, selection/highlight changes retaining Lens, existing custom-engine
settings, failed/unreadable saves, and row gaps/help text making no changes.
Lens remains selectable without local model files, and its help identifies the
Wi-Fi requirement and image upload without reporting missing local weights.
Approximate host previews of all three choices fit the card and viewport;
firmware rendering and live Google Lens recognition remain unverified.
After v0.7.12 the Meiki choice was removed, and after v0.8.0-test.1 (#81) the
VOCR2 trial. The row now has two 432-pixel buttons, 12 pixels apart, with the
same left and right edges as before: the local engine, labelled with the
generation of the model it runs (VOCR3; `core/vocr_models.c`), and Google Lens.
A saved `ocr_backend = vocr2`, `meiki` or `ncnn` loads as the local engine with a
config warning, and the next save writes `vocr`. `host/tests/test_settings_app.c`
runs the actual input/draw loop with a scripted controller, touch screen and
vita2d, and real files: both buttons, their edges and the gaps (focus only), Left,
Right and Cross, the removed values, missing and wrong-sized models, an
unreadable config.ini, and a later model table whose VOCR5 the app names unchanged
(and VOCR3 while a missing VOCR5 file leaves a VOCR3 fallback to run).
Its text widths are stub metrics, not firmware fonts. These scenarios, previews
and the native checks below predate that change.
The native Settings build, PNG ABI/assets checks and existing Lens transport
tests are separate checks. This change does not deploy anything to a Vita.

Native promotion of the v0.7.7 VPK completed with result 0. Full native readbacks
of the installed 277,084-byte executable and 788-byte `param.sfo` match the
published release members. The SFO's `APP_VER` remains `01.00`; exact executable
bytes and release provenance identify this build. A fresh boot and foreground
launch were confirmed; a snapshot reported `system_ui_overlaid = true`, so
controller tests waited. The user subsequently opened the app and encountered
`C2-12828-1`. Its completed native dump identified an incompatible libpng jump
buffer during icon loading, rather than a PVF measurement failure. The
[PNG runtime repair](settings-png-runtime.md) keeps the readable layout and icons.

The v0.7.8 repair passed exact-head host/Vita CI and merged through
[PR #36](https://github.com/bee-san/vita-jp-overlay/pull/36). Full readback of
the installed 277,212-byte executable matched the repaired candidate. Promotion
status had become uncertain after a disconnect; installed registry/readback and
subsequent native execution reconciled the outcome. No terminal promoter
response is inferred from that uncertain status.

The same Settings process remained in the foreground over 100.106 seconds,
with the final snapshot reporting `system_ui_overlaid = false`. The sampled
native event window contained no crashes and reported zero lost events.
An additional snapshot extended observed survival to 336.637 seconds with the
same foreground Settings process and no reported system UI overlay. No
screenshot was taken.
After an initial Select/readback discrepancy was caught, two completed
front-touch actions normalized OCR-tab/engine-row focus. Five subsequent
controller actions, each followed by a complete config readback, checked Meiki,
VOCR, the already-off Japanese-only state and restoration to on. The final
config exactly matched the intended VOCR/Japanese-only-on configuration, with
unrelated settings preserved. A later native check measured Japanese-only
on-to-off-to-on: the OCR-tab touch retained on, the Japanese-only-card touch
saved off, and Right saved on. Complete config readbacks followed all three
actions; the final 2,951-byte config was again byte-identical to the intended
file. These establish native control/persistence behavior; they do not provide
screenshots or firmware glyph measurements.

A subsequent agent-runtime replacement and fresh reboot also confirmed Settings
relaunched in the foreground with no reported system UI overlay and the intended
config byte-identical. That new boot is separate from the earlier measured
survival interval. Native keep-awake worker initialization and its first DEFAULT
tick returned 0. A subsequent 374.066-second command-free interval ended with a
healthy native response on the same boot and the same foreground Settings
process. The ordinary endpoint snapshot turns the screen on, so it does not
prove physical display visibility during the interval.

Firmware font bounds, sharpness and physical readability remain pending. The
broader installation and private-dictionary recovery state is tracked in
[the deployment checklist](todo-20261011.md).
