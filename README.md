# Vita JP Overlay — local Japanese OCR

This [bee-san fork](https://github.com/bee-san/vita-jp-overlay) reads visual-novel
text and shows Japanese dictionary entries over your game. **The v0.7.12 release defaults to
on-console vita-vn-ocr, automatic OCR, and bundled plain-English JMdict.**
Recognition and dictionary lookup work without Wi-Fi or a running computer.
The release includes a standalone per-game settings app. Anki and the offline
card queue are disabled; no cards or screenshots are queued.

Open **JP Overlay Settings**, press **Select**, and choose **VOCR**,
**[VOCR2](docs/vita-vn-ocr.md#experimental-vocr2-trial)**,
**[Meiki](docs/meiki-trial.md)**, or **Google Lens** in the OCR engine row.
Tap an engine directly, or use **Left/Right** or **Cross** to cycle the choices.
VOCR is the default and supports automatic or manual local OCR. Meiki runs
manually on one horizontal line and uses more game memory. Google Lens requires
Wi-Fi and sends the selected image to Google, using the original
[upstream implementation](https://github.com/Dartv/vita-jp-overlay).
The release stores the local models; choosing an engine keeps them installed
and applies when you next open the overlay.

The optional VOCR2 trial pairs a separately trained FL10-sized model with a
textbox finder. Shared screenshot totals improved on PSP and Vita, but some
games regressed by one or two characters and its strict promotion guard failed.
It is experimental, has no physical-device validation, and must be selected
explicitly in Settings; VOCR remains the default. The
[`0.7.12` release](https://github.com/bee-san/vita-jp-overlay/releases/tag/v0.7.12)
contains the three plugins, Settings, FL10/H15/VOCR2, JMdict and the optional
Meiki engine. Earlier releases remain available.

VOCR and Meiki have completed actual CLANNAD OCR jobs on the physical Vita.
Two observed Meiki jobs took about eight seconds each. Hardware accuracy/CER
and broader game compatibility remain unmeasured; host crop results are separate.
See [OCR measurements and limits](docs/vita-vn-ocr.md) and
[project lessons](docs/project-lessons.md).

The v0.7.10 release prepares the first local match during automatic OCR and
reuses completed results when reopening unchanged dialogue. In Whole word
mode, Right skips the matched source word and Left retraces recent selections.
The header also explicitly refreshes its highlight, with bounded layout retries.
Host tests cover these paths; the physical Vita result remains unverified.
This release does not include
the experimental local-dictionary memory reservation from
[issue #42](https://github.com/bee-san/vita-jp-overlay/issues/42).

All six published v0.7.9 assets match the audited bundle. That version's installed
Shell/Settings bytes and native Settings registration/launch are verified, and
four actual Settings touch/controller actions saved both highlight modes while
preserving VOCR and Japanese-only ON. In-game header taps and highlight geometry
still require hardware checks. See the
[installation checkpoint](docs/todo-20261011.md#v079-touchhighlight-installation-checkpoint)
for the measured results and remaining hardware checks.

## Install

Requires a PS Vita on firmware 3.60–3.74 with HENkaku/Ensō or h-encore.
Download `VitaJPOverlay-0.7.12.zip` from this fork's
[release](https://github.com/bee-san/vita-jp-overlay/releases/tag/v0.7.12) and
extract it. It includes all three plugins, their RCO, the settings VPK, FL10/H15
int8 models including the opt-in VOCR2 model, the pinned Meiki recognizer/engine, and the complete converted
JMdict snapshot. Meiki also needs its engine mounted for each enabled game;
see [the setup guide](docs/meiki-trial.md). Python 3.9+, bash,
and curl are needed for the computer-side installer.

Close games, open VitaShell, press Select to start FTP, and run from the
extracted folder:

```sh
bash install-vita.sh VITA_IP:1337
```

The installer checks the package manifest, backs up both taiHEN configurations
and your existing overlay settings, uploads and checksum-reads back each file,
and registers the three plugins without duplicating entries. It selects the new
local defaults while preserving other settings and per-game choices. Older
experimental Text registrations and duplicate worker entries are commented out. The backup is
in `build/device/backup/` in the extracted folder.

In VitaShell, install `ux0:data/JPOverlaySettings.vpk`, then reboot the Vita.
Open **JP Overlay Settings** from LiveArea to enable or disable individual games.
The settings app itself is always excluded from the overlay.

For manual installation, copy `ur0/` and `ux0/` to their matching Vita partitions
and install `JPOverlaySettings.vpk`. Back up your old overlay `config.ini` before
copying the included one. Add `ur0:tai/VitaJPOverlay_Kernel.skprx` under `*KERNEL`
`ur0:tai/VitaJPOverlay_Shell.suprx` under `*main`, and
`ur0:tai/VitaJPOverlay_OCR.suprx` under `*ALL` in the active taiHEN config.
Disable old `VitaJPOverlay_Text.suprx` entries and duplicate worker registrations,
then reboot. Keep unrelated plugins and configuration intact.

## Use in a game

| Control | Action |
| --- | --- |
| L + R | Open or close the dictionary overlay |
| Select + R | Toggle the subtitle strip |
| Left / Right | Skip matched words in Whole word mode; step one character in Character mode |
| Touch a recognized character | Look up the word starting there with a local dictionary |
| Up / Down | Navigate words with a network dictionary |
| Square | Draw a dialogue region with the touchscreen; Cross saves, Circle cancels |
| Hold Square | Restore the full-screen region |
| Circle | Close the overlay |

Choose a tight dialogue region first: smaller regions need less RAM and reduce
recognition errors. Regions are saved per game. Automatic mode recognizes
settled dialogue in the background and prepares its first local match in Whole
word mode. A completed lookup appears immediately when opening unchanged
dialogue. Character mode defers dictionary lookup until opening the overlay.
Selection changes never recapture the screen or rerun OCR. Set `ocr_mode = on_press` to recognize only
when opening the overlay. Cross and Triangle do not save or send Anki cards.

## Per-game settings

**JP Overlay Settings** shows three large game cards at a time, with wrapped
titles, visible IDs and clear ON/OFF badges. Tap the **Games** or **OCR & lookup**
tab, or press Select, to change pages. Choices save automatically to
`ux0:data/VitaJPOverlay/games.ini` and take effect on game start/resume.

| Control | Action |
| --- | --- |
| D-pad / L or R | Select a game / move a page |
| Cross or touch a game | Toggle OCR and overlay input for that game |
| Triangle or touch the default row | Choose all games by default or selected games only |
| Square | Filter all, enabled or disabled games |
| Start | Refresh the installed-title list |
| Select or touch a page tab | Switch between Games and OCR/lookup settings |
| Circle | Exit |

On the **OCR & lookup** page, select a card with Up/Down. Cross
or touch a card changes its choice: OCR engine, Japanese-only selection, or
whole-word/character highlighting.
Touch an engine button to choose it directly;
Left/Right cycle the four engine choices on the model card, OFF/ON on the selection card,
or Whole word/Character on the Highlight card.
Choices save automatically to `config.ini` and apply when the overlay next opens.
Model files remain stored; only the selected engine runs. Start reloads
the saved preferences. Other config values, including dictionary priority,
paths, capture mode and API keys, are preserved.

Japanese-only selection is **ON by default**. The initial cursor and Left/Right
skip Latin text, digits, punctuation and Hangul while selecting kana and Han
characters. It does not tokenize words or hide the recognized sentence. With
it OFF, every Unicode character is selectable. No Japanese characters means
no character lookup or dictionary allocation.

**Whole word** highlighting is the default. It highlights the actual dictionary
match, including the original inflected text, when lookup finishes. **Character**
highlights only the selected character. A pending or unsuccessful word lookup
keeps the character highlighted. In Whole word mode, Right advances past the
matched source span and Left retraces up to 16 recent forward selections. With
no match or previous selection, movement falls back to one character.
Character mode always moves one character; Japanese-only applies to both modes.
Tap a character in the recognized sentence at the top of the open overlay to
look up the word starting there; Japanese-only selection also applies to taps.
[Touch selection details and validation](docs/touch-lookup.md).

Disable games such as Uncharted when you need their normal L+R controls.
Disabled games have no overlay capture or input filtering. An unreadable or
corrupt policy fails closed and reports the problem rather than enabling every
game. Adrenaline uses one shared policy for its PSP titles.

## Settings and dictionaries

`ux0:data/VitaJPOverlay/config.ini` is reloaded when the overlay opens:

```ini
dictionary = local
local_dictionary_dir = ux0:data/VitaJPOverlay/dictionaries
local_dictionaries = jmdict.vjdict
text_source = ocr
ocr_backend = vocr
ocr_model_dir = ux0:data/VitaJPOverlay/ocr
vocr_model = FL10_w8.vocr
ocr_mode = auto
japanese_only_selection = on
```

FL10 is the smaller default for lower game memory use. The included
`H15_w8.vocr` trades a larger workspace for better host accuracy. A game OCR
memory error asks you to select a tighter region. A Shell UI memory error
keeps recognized text and identifies the unavailable dictionary results.
The complete settings template is
[docs/config.example.ini](docs/config.example.ini).

JMdict is disk-backed: its approximately 74 MiB file is never loaded into RAM.
The bundle uses plain English definitions, Japanese spellings/readings, and
inflection rules. Source provenance is in [docs/jmdict-source.json](docs/jmdict-source.json).
Copyright belongs to James William Breen and the EDRDG; it is distributed under
[CC BY-SA 4.0](https://www.edrdg.org/edrdg/licence.html). The release includes
attribution, its conversion manifest and the license. To rebuild this pinned
snapshot, run `python3 tools/prepare_jmdict.py -o jmdict.vjdict`; the original
gzip snapshot is also attached to the release.

You can add your own Yomitan dictionaries, including a legally obtained
Daijirin export, with [the converter and installation guide](docs/local-dictionaries.md).
Up to eight dictionary files can be enabled in priority order. The downloaded
Daijirin v3 has been converted and measured separately; the commercial file is
installed privately, while the public release includes the converter and JMdict.
The dictionary stays on disk; it is never loaded whole into RAM.

Google Lens can also be selected with `ocr_backend = lens`; it uploads the
selected image to Google and works with the configured dictionary, including
local JMdict. Selecting an OCR engine preserves the dictionary, OCR mode and
other settings. `dictionary = hachidori` uses Hachidori Relay on
your trusted LAN (`hachidori_host = COMPUTER_IP`, port 19633). Jiten and JPDB
require their respective API keys. With either key set, the subtitle strip
(Select + R) shows furigana over its kanji once the words are looked up, so
each settled subtitle's recognized text is also sent to that service. Local
dictionaries and Hachidori show the strip without readings and add no lookups
for it. Local defaults make no OCR or dictionary network requests and have no
cloud fallback.

## Memory and troubleshooting

The `*ALL` plugin runs the selected local OCR engine in the enabled game's process, allocating its
region-sized workspace from that game's `USER_RW`. It checks the title at
startup and starts no worker thread in SceShell, VitaShell, the settings app or
other system titles. Candidate games keep only an idle worker until the Shell
confirms their per-game policy; disabled games load no weights and allocate no
OCR workspace. Adrenaline's PSP titles share its Vita process.

The kernel transports bounded jobs, copied pixels and text. Game workers cannot
submit jobs or control input. A job is tied to its game, capture sequence and
foreground generation; suspend, game changes, cancellation and Shell restarts
invalidate old work. The single-pass raw capture is freed when the game worker
reads its final row. The worker frees its model/workspace before returning text.
It leaves a 4 MiB free USER memory reserve before allocating; allocation can
still fail because the game or system uses memory concurrently.

Model files are opened by the existing kernel worker because some games cannot
access the `ux0:` storage namespace (`0x80010013`, ENODEV). The broker accepts
only the three original VOCR model names, the separate pinned VOCR2 trial
model, and the pinned Meiki recognizer in
`ux0:data/VitaJPOverlay/ocr`, checks
their lengths, and copies at most 8 KiB per request into game-owned memory.
The game then checks SHA-256 and runs inference. The kernel holds one 8 KiB
staging buffer, no complete model, and no saved game pointers. Cancellation or
process exit drains any file operation before the next job can reuse its slot.
The shared API 13 error codes keep bit 30 clear. On the physical Vita, the
previous BUSY value `-3` arrived as `0xBFFFFFFD`, so the game failed to poll a
pending model operation and retained its job ownership. The new codes match
that return encoding and also remain consistent in copied status fields.
Native SDK file errors are unchanged. Install all three API 13 plugins together
and reboot; older plugin versions cannot use this transport.

The `*main` plugin handles UI and disk-backed dictionary lookup with one
64 KiB result arena and no TLS pool for local lookup. Local OCR-only/subtitle
jobs use a static 4 KiB filtered-text buffer and no dynamic dictionary arena.
The UI keeps its own 4 KiB text copy for Unicode character navigation. A lookup
reads only the selected position and returns zero or one entry.
The previous local result is retired before either is reused; network jobs
retain two result arenas. Paf allocations preserve a 1.5 MiB UI reserve. If a
local result allocation is refused after OCR succeeds, the recognized text
stays visible and only dictionary lookup backs off. Failed joins/frees keep
ownership for a safe stop retry.
The kernel startup instruction and all three plugins' syscall imports are audited.

These are bounded allocations and host checks, not a measured physical Vita
peak or a guarantee that every full-screen region fits in every game.

Set `log_file = on` to record bounded diagnostic logs. Fetch the bootstrap logs
with `bash tools/install_ftp.sh --status VITA_IP`; recognition timings and memory
attempts are in `ux0:data/VitaJPOverlay/log.txt`. If boot fails, hold L while
powering on to skip plugins, start VitaShell FTP, and run
`bash tools/install_ftp.sh --uninstall VITA_IP`. Uninstalling preserves data.

## Build and validation

The Vita build checks the artwork inside `JPOverlaySettings.vpk` before it can
ship. Bubble and LiveArea PNGs must use opaque, non-interlaced, 8-bit indexed
color; RGBA PNGs can fail installation with `0x8010113D`. To recreate the
committed artwork, install `tools/requirements-assets.txt` and run
`python3 tools/make_settings_assets.py`. Palette encoding preserves every pixel.

The default build enables vocr. Its runtime is external to this repository and
checksum-pinned; see [the build guide](docs/vita-vn-ocr.md#build). The OCR source
repository currently requires access. Set up VitaSDK/Paf with
`bash tools/setup_toolchain.sh`; standalone-app dependencies are hash-pinned.
Python and Pillow are needed for host image tests.

```sh
cmake -S . -B build/host -G Ninja -DVJO_VOCR_SOURCE_DIR=/path/to/vita-vn-ocr
cmake --build build/host
ctest --test-dir build/host --output-on-failure
cmake -S vita -B build/vita -DVJO_VOCR_SOURCE_DIR=/path/to/vita-vn-ocr
cmake --build build/vita --target release
```

The cross-build produces a base ZIP. `tools/package_release.py` combines it with
the checksum-pinned models, JMdict conversion and license to create the complete
installable release and its per-file manifest. CI builds the kernel, Shell UI
and game vocr worker using a read-only private-source key; publication happens
after validation.

The host suite covers configuration, disk-backed dictionary lookup,
deinflection, kernel/input ownership, per-game policies, and the actual Shell
worker with platform stubs. It checks cancelled jobs, denied input blocking,
OOM recovery, failed joins, 50 local OCR-only lifetimes, cursor coalescing,
close/reopen during lookup, scene/region invalidation, Unicode navigation and
100 game-worker
allocation/free cycles, bounded model reads and stale-job isolation. Real
model tests also run the actual game worker through the actual kernel broker
with FL10/H15, unavailable game storage, short reads, corrupt weights and
failed cleanup. The integration tests model the observed syscall return
encoding and require a subsequent real capture request after each completed job.
Timeout and ownership tests run without release weights. Real
accuracy is measured separately against vita-vn-ocr's hand-checked shared
`eval/` screenshots. Experimental Meiki and text-extraction branches remain
separate from this release.

Controller regressions exercise all 12 hooked read/peek variants, including
negative logic and 64-sample batches. The [pinned VitaSDK headers](https://github.com/vitasdk/vita-headers/blob/e66ebe90b73d1fa4cce005a5b2072cec27322544/include/psp2/ctrl.h)
map Vita shoulders to L1/R1 in `*2`/`Ext2` calls. A physical v0.7.3 trace
confirmed a game `ReadPositive2` sample of `0xC00` alongside raw L+R `0x300`;
the older filter allowed that opening chord to reach the game. The repaired
filter uses shared canonical chord/replay state, preserves genuine L2/R2
in the mapped calls, and clears hidden buttons' pressure values. Full overlay
blocking clears all 12 pressure fields while preserving timestamps and the
SDK's four reserved bytes. Only a replayed short tap receives a fabricated
pressure of 255. Host checks cover mixed APIs, held suppression and contended
reads; physical validation of this input repair remains required.

## Credits and license

Based on [Dartv/vita-jp-overlay](https://github.com/Dartv/vita-jp-overlay).
Thanks to [meikidroid](https://github.com/rtr46/meikidroid),
[jmdict-vita](https://github.com/shoui520/jmdict-vita),
[PSVshellPlus](https://github.com/GrapheneCt/PSVshellPlus),
[PSVshell](https://github.com/Electry/PSVshell),
[reVita](https://github.com/MERLev/reVita),
[ScePaf-RE](https://github.com/GrapheneCt/ScePaf-RE), VitaSDK, BearSSL,
jsmn, acutest, and Yomichan for its Japanese deinflection rules.
Pinned sources and attribution are in `third_party/`.

Code: GPL-3.0 ([LICENSE](LICENSE)). JMdict: CC BY-SA 4.0.
