# Vita JP Overlay — local Japanese OCR

This [bee-san fork](https://github.com/bee-san/vita-jp-overlay) reads visual-novel
text and shows Japanese dictionary entries over your game. **v0.7.2 defaults to
on-console vita-vn-ocr, automatic OCR, and bundled plain-English JMdict.**
Recognition and dictionary lookup work without Wi-Fi or a running computer.
The release includes a standalone per-game settings app. Anki and the offline
card queue are disabled; no cards or screenshots are queued.

Local OCR is cross-built and tested on real VN screenshots on a host. This
release still needs an in-game run on physical hardware; host timings are not
Vita speed measurements. See [OCR measurements and limits](docs/vita-vn-ocr.md).

## Install

Requires a PS Vita on firmware 3.60–3.74 with HENkaku/Ensō or h-encore.
Download `VitaJPOverlay-0.7.2.zip` from this fork's
[release](https://github.com/bee-san/vita-jp-overlay/releases/tag/v0.7.2) and
extract it. It includes all three plugins, their RCO, the settings VPK, FL10/H15
int8 models, and the complete converted JMdict snapshot. Python 3.9+, bash,
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
| D-pad | Navigate words and definitions |
| Square | Draw a dialogue region with the touchscreen; Cross saves, Circle cancels |
| Hold Square | Restore the full-screen region |
| Circle | Close the overlay |

Choose a tight dialogue region first: smaller regions need less RAM and reduce
recognition errors. Regions are saved per game. Automatic mode recognizes
settled dialogue in the background. Set `ocr_mode = on_press` to recognize only
when opening the overlay. Cross and Triangle do not save or send Anki cards.

## Per-game settings

**JP Overlay Settings** lists installed game names, IDs and icons in a dark,
teal-accented interface. Choices save automatically to
`ux0:data/VitaJPOverlay/games.ini` and take effect on game start/resume.

| Control | Action |
| --- | --- |
| D-pad / L or R | Select a game / move a page |
| Cross or touch a game | Toggle OCR and overlay input for that game |
| Triangle or touch the default row | Choose all games by default or selected games only |
| Square | Filter all, enabled or disabled games |
| Start | Refresh the installed-title list |
| Circle | Exit |

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
```

FL10 is the smaller default for lower game memory use. The included
`H15_w8.vocr` trades a larger workspace for better host accuracy. A memory error
asks you to select a tighter region. The complete settings template is
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
Up to eight dictionary files can be enabled in priority order.

Optional network backends remain available: `ocr_backend = lens` uploads the
selected image to Google Lens; `dictionary = hachidori` uses Hachidori Relay on
your trusted LAN (`hachidori_host = COMPUTER_IP`, port 19633). Jiten and JPDB
require their respective API keys. Local defaults make no OCR or dictionary
network requests and have no cloud fallback.

## Memory and troubleshooting

The `*ALL` plugin runs vocr in the enabled game's process, allocating its
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

The `*main` plugin handles UI and disk-backed dictionary lookup with two
128 KiB result arenas and no TLS pool for local jobs. Its Paf fallback preserves
a 1.5 MiB UI reserve. Failed joins/frees keep ownership for a safe stop retry.
The kernel startup instruction and all three plugins' syscall imports are audited.

These are bounded allocations and host checks, not a measured physical Vita
peak or a guarantee that every full-screen region fits in every game.

Set `log_file = on` to record bounded diagnostic logs. Fetch the bootstrap logs
with `bash tools/install_ftp.sh --status VITA_IP`; recognition timings and memory
attempts are in `ux0:data/VitaJPOverlay/log.txt`. If boot fails, hold L while
powering on to skip plugins, start VitaShell FTP, and run
`bash tools/install_ftp.sh --uninstall VITA_IP`. Uninstalling preserves data.

## Build and validation

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
OOM recovery, failed joins, 50 local OCR/lookup lifetimes, 100 game-worker
allocation/free cycles, bounded model reads and stale-job isolation. Real
accuracy is measured separately against vita-vn-ocr's hand-checked shared
`eval/` screenshots. Experimental Meiki and text-extraction branches remain
separate from this release.

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
