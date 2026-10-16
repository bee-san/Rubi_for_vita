# Two builds: normal and debug

The owner (2026-10-15 19:08Z): "after the final build is done, release one debug build (the one
used to test) and one normal build without all the debug features in it."

One tree builds both, chosen at compile time with `-DVJO_DEBUG=ON|OFF` (`include/vjo_build.h`):

| zip | build | `VJO_DEBUG` | for | CI artifact |
|---|---|---|---|---|
| `VitaJPOverlay-<version>.zip` | normal | `OFF` | everyone: the main download | `VitaJPOverlay-normal` |
| `VitaJPOverlay-<version>-debug.zip` | debug | `ON` (the default) | console tests, and anyone asked for a full log | `VitaJPOverlay` |

The debug build is the test builds' own: its three plugins and RCO are byte-identical to main's
(`3805a13`, measured: the same SHA-256s; also on `6020c0b`, `9f92bc6`, `ac905b1` and `e53d9fb`). Its Overlay app differs in one string: the version beside
its title, `Overlay v0.8.0 debug` (the normal app shows `Overlay v0.8.0`). Test builds stay debug:
`VJO_DEBUG` defaults to `ON` in every CMake project, a file compiled without it is the debug build,
and CI's `VitaJPOverlay` artifact is the debug build as before.

Both zips have the same models, dictionary, config template, installer and tools
(`tools/package_variants.py` refuses two bases that differ anywhere else). Both builds read and
write `config.ini` with the same code: no `VJO_DEBUG` in the parser, the Overlay app's writer, the
installer's migration or the template (`tools/tests/test_release_variants.py`), so a `config.ini`
from either works in the other, `log_file = on` included: the normal build then writes its short log.

## What the normal build drops, reduces or keeps

Drop: compiled out (code and strings). Reduced: a shorter form. Keep: both builds. When in doubt,
keep: anything a user-facing feature or a recovery needs stays.

### SceShell plugin (`VitaJPOverlay_Shell.suprx`)

| feature | where | normal |
|---|---|---|
| the on-device self-test: the marker, the five stages, the report, its memory samples | `shell/selftest.c`, `worker.c` `selftest_job_due`, `run_selftest_stages` | **drop**. An armed marker gives one line, `self-test: armed, but this is the normal build: install the -debug build to run it`, and stays for a debug build |
| the game worker's step log, one `gstep` line per step | `worker.c` `log_vocr_steps` | **drop** |
| each job's lines: result, capture, memory check, speed, per-line crops, frames and confidence | `worker.c` `vocr_run_one`, `log_vocr_measurements` | **reduced**: one line, `ocr rc=… MODEL: N lines, T ms`, for the first job of each activation of the game (a start or a resume: `game_generation`) that read its screen, for its first job that found a line (the first is often a title screen's, with none), and for every failed job, which also logs its refused memory check's numbers |
| a model size that did not fit (`tier large did not fit in …; using normal`) | `worker.c` `log_tier_checks` | **reduced**: once per activation and again when what did not fit (or the size the job went on to) changes; the debug build logs it for every job (the README's "only `log.txt` says so" holds in both) |
| a model file unusable (`game vocr: … is unusable here`) | `worker.c` `run_vocr` | **keep** |
| the 444 MHz boost: before, during, after | `worker.c` (the step log's clock step) | **reduced**: `clock: 333 -> 444 MHz while reading, 333 after` (or `left as it is`, `refused`), for each activation's first job that recognized a line and when it changes |
| the recognized text, raw and as shown | `worker.c` `log_text_lines` | **drop** (the normal log has no recognized or looked-up text: the Anki save line names no word either) |
| the dictionary lookup line (text at the cursor, entry, reads, time) | `worker.c` `log_lookup` | **reduced**: `lookup … failed: rc=…` only |
| the job's done line (time, arena peak, game time, log time and appends) | `worker.c` `net_job` | **drop** |
| job start / cancel / open / refresh / dropped-screen / subtitle traces; the workspace line; `%s: system app, not a game` | `worker.c` | **drop** |
| input block on/off, the overlay's close with its `t=` | `worker.c` | **drop**; a refused block (the overlay cannot open) is logged |
| the overlay's close counters (frames, moves, stick, slowest frame and key) | `overlay.cpp` | **drop** (the counters too) |
| the front touch region's on/off and hand-back trace | `overlay.cpp` | **drop**; a refusal is logged |
| the region selector's corner and box lines; a header tap's cursor (`header touch cursor=`) | `overlay.cpp` | **drop**; the saved region (`region set for …`) is kept |
| every Paf allocation | `memory.cpp` | **reduced**: refusals only (the reserve guard's line is kept) |
| the ledger's per-item table at boot, game start and exit | `ledger.c` | **reduced**: the two pool lines stay |
| Anki: the queue count, the arena peak, the record's bytes | `anki.c`, `anki_queue.c` | **drop**; each save is kept without its word (`anki: save rc=0, 12 ms`; the debug build's names it), and every error |
| an Anki card's screenshot (API 23): its line (`anki: screenshot rc=…`: size, block, strips, times) and, for a failed one, the game worker's steps | `anki.c` `take_shot` | **reduced**: a failed screenshot's reason only, after the save's line (`anki: screenshot rc=-103`; rc -1: no file after it; the overlay points to log.txt), no figures and no step lines, for the Shell's pages; the screenshot itself is the same in both builds |
| the title's parameter line | `game.c` | **drop** |
| a PSP game's launch: its IDs, disc ID and the file it boots (`psp: …`, #124) | `game.c` | **keep** (the README's way to register a bubble: one line per launch) |
| the log's append timing (`vjo_log_io`) | `log_vita.c` | **drop** |
| the bootstrap and boot guard lines, `status.txt` | `boot.c`, `guard.c`, `log_vita.c` | **keep** (recovery: `docs/boot-safety.md`) |
| config warnings, the Overlay app check (#122), game start and exit, errors | `worker.c`, `files.c` | **keep** |
| `log.txt` (`log_file`, 1 MB + one rotation) | `log_vita.c` | **keep**, the short log |
| the UDP log (`log_host`) | `log_vita.c` | gone from both builds with the network stack (#145); `log_host` is a removed setting, a set one gives one config warning in both |
| the network code's lines (sockets, resolver) | `platform_vita.c`, `core/` | gone from both builds: Google Lens and its capture with #130, the sockets, resolver and TLS with #145 |

### Kernel plugin (`VitaJPOverlay_Kernel.skprx`)

| feature | where | normal |
|---|---|---|
| the self-test's memory sample (`vjoMemInfo`, three routes) | `kernel/meminfo.c` | **drop**: not built; the export stays (one NID table) and returns `VJO_ERR_UNSUPPORTED` |
| the frame pacing of each OCR job (the `pace` step) | `game_ocr.c`, `display.c` | **drop** (no work in the display hook) |
| `region quiet` / `region settled` for every screen | `main.c` | **drop** |
| each OCR job's line and its first-pass line | `game_ocr.c` | **drop**; a failed rows call is kept |
| the controller hooks' traces (blocked, trigger, foreign, empty), the input block's line | `input.c`, `main.c` | **drop** (no trace read of the game's pad either) |
| the ledger's per-item lines | `ledger.c` | **reduced**: the pool line stays |
| foreground changes and the game's activation (`t=`) | `lifecycle.c` | **keep** (a few a session; #104-style problems need them) |
| boot guard, hooks, start, shell registration, errors | `boot.c`, `main.c` | **keep** |
| `kernel.txt` (256 KB a boot, the previous boot's kept) | `main.c` | **keep**: shorter, the same file |

### Game worker (`VitaJPOverlay_OCR.suprx`)

| feature | where | normal |
|---|---|---|
| the self-test's capture probe (every capture path into every memory type) | `game_ocr/main.c` `run_probe` | **drop**: a probe request is refused as one it cannot run (`VJO_E_SOURCE`) |
| the step log in the job's result | `game_ocr/main.c` `step_add` | **keep**: the result's fixed array (no strings); its tier checks and clock step feed the normal log |
| the forced-fallback flag and the no-cache flag (only the self-test sets them) | `game_ocr/main.c` | **keep** (part of API 22's request; no strings) |

### Overlay app (`JPOverlaySettings.vpk`)

| feature | where | normal |
|---|---|---|
| the frame interval line (the first 60 frames) | `settings/main.c` | **drop** |
| the start line (budget, heap) | `settings/main.c` | **keep**, now with the version: `start: v0.8.0 debug; …` or `start: v0.8.0; …` |
| the version beside the title | `settings/main.c` | `Overlay v<version>` in the normal build, `Overlay v<version> debug` in the debug one |
| the Anki export's lines (its storage errors, each export) | `settings/anki_export.c`, `anki_page.c` | **keep** |
| the Adrenaline bubbles found and the taiHEN sections the app adds (#124) | `settings/` | **keep** (a few lines per start; a config.txt the app changed must be traceable) |
| each scan of the Games list (one line per root, one for the rows) and each Default change (#138) | `settings/catalog.c`, `settings/main.c` | **keep** (four lines a scan in one append, at start and on Start; they are what tells a missing game's cause) |

### Tools, package and the rest

| item | normal |
|---|---|
| `install_ftp.sh --collect` (every log and record in one zip, with its analysis) | **keep**: the way to send a short log when something breaks. The analysis names the build (`package 0.8.0, the normal build (a short log; no self-test)`, and the log's boots of it) and reads the normal build's own lines: its OCR jobs (each rc, model, lines and time), its clock lines, a refused PAF request, the self-test note; a failed job is a problem line; its kernel.txt logs no job, which the analysis says instead of counting 0. `analyse_timeline.py` says the short log has nothing to time |
| `install_ftp.sh --self-test` | keep; from the normal package it says the self-test needs the `-debug` zip, and arms the marker for that build |
| `analyse_selftest.py`, `analyse_timeline.py` | keep (they read either build's files; the timeline is the debug build's lines, and so are `--collect`'s presses by `ocr_mode` and its "read before finished" re-reads, [auto-vs-press.md](auto-vs-press.md): for the normal build's boots the analysis says so) |
| debug config keys | none: every key is the same in both builds (`log_host`, the UDP log's, is a removed setting in both since #145) |
| asserts | none at run time in the plugins or the app; the compile-time checks (`static_assert`, the negative-array typedefs) cost nothing in either |
| test and mutation hooks (`VJO_HOST`, `VJO_GAME_CASCADE_CONF`, `VJO_GAME_OCR_NO_MODULE_ENTRY`, `VJO_SETTINGS_SMOKE`, `VJO_LOCAL_STATS`, `VJO_VOCR_MODEL_INFO_TEST`) | in neither release build: host tests and the Vita3K smokes only |
| symbols | in neither zip: the SELFs carry no symbol table (exports by NID); the ELFs stay in the build directory |
| the boot-safety guard, the memory ledger's checks, the worker's loading (the kernel's loader since #138; `*TITLEID` lines before) | keep: unchanged in both |

## Measured

Both builds of this tree on main `3805a13` (#130 removed Google Lens from both), VitaSDK as
`tools/setup_toolchain.sh` installs it, `tools/check_memory_ledger.py`'s reader (loaded segments rounded
to 4 KiB) [measured: `build/rv`, the `release_variants` target]:

| module | debug: text + data/bss | pages | normal: text + data/bss | pages | freed |
|---|---:|---:|---:|---:|---:|
| `VitaJPOverlay_Shell.suprx` (SceShell) | 100,792 + 70,060 | 176,128 B | 90,768 + 69,988 | **167,936 B** | **8,192 B (2 pages)** |
| `VitaJPOverlay_Kernel.skprx` (the kernel pool) | 36,640 + 77,768 | 114,688 B | 32,332 + 76,080 | 110,592 B | 4,096 B (1 page) |
| the same after #138's worker loader | 40,648 + 77,824 | 118,784 B | 36,444 + 76,136 | 114,688 B | 4,096 B (1 page) |
| `VitaJPOverlay_OCR.suprx` (each registered game) | 71,072 + 14,208 | 90,112 B | 69,708 + 14,208 | 90,112 B | 0 (1,364 B of text) |
| `eboot.bin` (the Overlay app, 50 MiB system-app budget) | 485,988 + 3,213,728 | 3,702,784 B | 485,516 + 3,213,696 | 3,702,784 B | 0 |

With API 23's Anki screenshot (both builds of main `c6e0a4d` and that change, the build log's module
rows): the Shell 100,484 + 69,828 and 90,012 + 69,756 (the same pages, 176,128 and 163,840 B; the normal
one's text has 100 B of room left), the kernel 40,040 + 77,672 and 35,900 + 75,984 (the same pages), the
game worker 77,004 + 14,160 and 75,688 + 14,160 in 94,208 B, one text page more in both builds for the
JPEG encoder ([memory-ledger.md](memory-ledger.md#the-two-builds)).

The normal Shell's 8,192 B are SceShell's for as long as the console is on (its partition had
28-98 KiB free beside a game [measured, vita-memory.md 5.1]). The saving moves with the code: three
pages on main `ac905b1`, two since #127 and #124, two again after #130, and three after the network
stack's removal (#145: 176,128 against 163,840 B [measured: both builds of main `3aa08a6` with #132
and #145]). Its budget is checked exactly
(`--exact`: a normal budget above its pages fails the build too), so whoever changes either build's
pages re-measures it; the check names the value to set. The game worker's saving (1,364 B of text) no
longer frees a page since #124, so both builds share its budget. The Overlay app's text differs by
472 B and its pages not at all with the local SDK; with CI's container SDK, which builds the
release, its pages are the same in main, debug and normal [measured: audit-overlay-release-variants
on `b79ecf9`]: it is not SceShell memory either way.

The log [measured: host suites, `host/tests/test_shell_worker.c`]: the debug build writes 23 lines per
OCR job (`vocr_job_log_is_appended_in_batches`), its text lines and a lookup line besides. The normal
build writes, per activation of a game (each start or resume), the first job that read its screen,
its first that found a line, the clock line, and a model size that did not fit, each once and again
when it changes; and every failed job with its refused memory check
(`normal_build_logs_the_first_jobs_and_failures`, `normal_build_logs_tier_misses_once_and_on_change`).

## Building

```sh
cmake -S vita -B build/vita -DCMAKE_BUILD_TYPE=Release -DVJO_DEBUG=OFF ...   # the normal build; ON (the default): debug
cmake --build build/vita --target release         # VitaJPOverlay-<version>.zip, or -debug.zip
```

Every Vita build checks it is the build it says (`tools/check_variant.py`: the normal modules carry
none of the debug build's own strings, the debug modules all of them, the app's version; with
`--sources`, which the build passes, the normal modules also carry none of the literals of 12 bytes or
more that their sources compile only into the debug build and kept code does not use too
(`#if VJO_DEBUG` branches, `vjo_dlog()`/`klog_d()` calls, `shell/selftest.c`, `kernel/meminfo.c`:
71 of the Shell's, 11 of the kernel's, 1 of the worker's and 1 of the app's at main `dcb7b31`), and
each debug module at least one of them, the worker's only check). That catches a module built with the
wrong `VJO_DEBUG`. It cannot catch a source change that moves a debug line into the normal build (the
sources and the module then agree): that is the inventory's to review. Nor does it see a literal
joined from macros (one of the Shell's 71), or a leaked string that follows non-string data with no
separating byte (about 1 of 847 literals at main `7fc40df`, measured by
audit-overlay-release-variants-c). The build also checks its pages against its own budget
(`include/vjo_ledger_budget.h`'s `#if VJO_DEBUG` branches, `tools/check_memory_ledger.py --variant`).
The host suites, the ARM lane, the Vita3K smokes and
`tools/preflight.sh` take the same switch (`-DVJO_DEBUG=OFF`, or `VJO_DEBUG=0` in the environment);
CI runs the host and vita jobs once for each build. Its `package` job then makes the release's two
zips of the vita jobs' artifacts, as below, and runs `tools/e2e_package.py` on each: the package's
own `install-vita.sh` and `tools/install_ftp.sh` against a simulated Vita (its partitions as
directories behind an FTP mock), over a v0.8.0-test.2 install, a fresh one and an older config
(`--check-app`, `--self-test`, `--collect` and its analysis, `--uninstall`).

## The release's two zips

From source, one target builds both and packages them (the other build in `variant-<other>/` of the
same build directory):

```sh
cmake -S vita -B build/vita -DCMAKE_BUILD_TYPE=Release -DVJO_VERSION=0.8.0 -DVJO_RELEASE_MODELS=/path/to/models \
  -DVJO_RELEASE_SOURCE_REVISION=$(git rev-parse HEAD) ...
cmake --build build/vita --target release_variants
# build/vita/release-variants/VitaJPOverlay-0.8.0.zip, VitaJPOverlay-0.8.0-debug.zip, SHA256SUMS
```

From CI (the final release's way: the push to `main` it is cut from), with that run's two artifacts:

```sh
gh run download RUN -R bee-san/vita-jp-overlay -n VitaJPOverlay -D ci/debug
gh run download RUN -R bee-san/vita-jp-overlay -n VitaJPOverlay-normal -D ci/normal
python3 tools/package_variants.py --normal ci/normal/VitaJPOverlay-0.8.0.zip \
  --debug ci/debug/VitaJPOverlay-0.8.0-debug.zip --models /path/to/models --version 0.8.0 \
  --source-revision SHA --sources . -o out/
```

The models are the pinned files of `tools/prepare_ocr.py` (its `--backend vocr-tiers` and
`--backend vocr-lm` download and check them), or the vita-vn-ocr releases' assets checked against
those pins, as the test builds' packaging fetches them. The final release passes no `--set`, so its
config template is the repository's (`anki_mining = off`), unless the owner decides otherwise; the test
builds add `--set anki_mining=on`.

The Overlay app's version is the tree's `VJO_VERSION` (0.8.0 now): a test package
(`VitaJPOverlay-0.8.0-test.N.zip`, `package_release.py --version 0.8.0-test.N`) shows `Overlay v0.8.0
debug`, the build its binaries are.

`tools/package_variants.py` refuses bases that are not the builds they are given as (with `--sources`,
a checkout of their commit, each module is checked as the Vita build checks it, the worker included),
an Overlay app of another version than `--version` (set `VJO_VERSION` in `vita/CMakeLists.txt` to the release's
version before that run), and bases that differ in anything but the build's own files (the
plugins, the app's VPK and its stamp). It packages both with `tools/package_release.py` (which
records `variant` and `app_version` in each manifest), verifies both, checks they differ only
there, and writes `SHA256SUMS`. Then run the package check on each zip, as CI does on every pull
request that changes more than Markdown:

```sh
python3 tools/e2e_package.py out/VitaJPOverlay-0.8.0.zip
python3 tools/e2e_package.py out/VitaJPOverlay-0.8.0-debug.zip
```

Publish both zips and `SHA256SUMS` in one release, the normal zip first: it is the main download.
