# Testing without the console: what runs where, and what cannot

The owner's words: "we have no way to properly test this without a live device". This page
says exactly what is testable off the console, what CI runs, what Vita3K can and cannot tell us,
and what only the console can answer. The two things the console kept failing on, boot and
memory, are the two things no emulator models: a plugin's pages inside SceShell's 50 MiB
partition, and taiHEN loading our modules into firmware processes
([vita-memory.md](vita-memory.md), AGENTS.md rule 5).

## 1. The matrix

| Our code | host x86_64 (CI `host` job) | ARMv7 under qemu-arm (`tools/arm_host_tests.sh`) | Vita3K | the console |
|---|---|---|---|---|
| kernel: scene detection, triggers, foreground state machine and the process event hook's input release, the capture ladder's kernel strip, the game-OCR broker, the boot guard, the ledger | real code, SDK calls stubbed (`host/tests/vita_stubs`) | real code, same stubs, 32-bit, LDREX/STREX atomics | not loadable: the kernel plugin needs `SceDisplay`/`SceCtrl`/`SceTouch`/`SceProcessmgr` ForDriver exports to hook and `SceSysmem`/`SceIofilemgr` ForDriver calls; Vita3K has no firmware kernel to export them (its taiHEN HLE implements "all functions taiHEN itself relies on", nothing of a kernel plugin's imports) | the only place its hooks, its partition and its pool exist |
| kernel: the hooks themselves (display, pad, touch, process events) | no | no | no | only here |
| Shell plugin: control policy, memory lifetime, Anki miner and queue, strip, cursor, dictionary paging | real `shell/worker.c`, `anki.c`, `anki_queue.c`, `ledger.c`, `log_vita.c`, `platform_vita.c`, `strip_view.c`, `game.c`, `files.c`, `game_ocr_client.c` with stubs (the suites `shell_worker`, `shell_worker_paf`, `shell_anki`, `shell_log`, `platform_vita`, `shell_game`, `preferences`, `game_ocr_client`) | not built (needs BearSSL and the core library; vita-vn-ocr's ARM lane covers the recognizer) | not loadable in its real context: Vita3K has no SceShell ("Shell support is pretty spotty" is about its own replacement UI); ScePaf, SceShell's partition and the LiveArea daily job do not exist there | only here |
| Shell plugin: the Paf UI (`overlay.cpp`, `strip.cpp`) | header-level tests (`test_header_highlight.cpp`, `test_word_navigation.cpp`, `test_touch_hold.c`: what the page's close does with the touch region, #104); the arrows' row walk (`core/text_rows.c`) against a layout double (`test_text_rows.c`); the left stick's timing, cap and deferred lookup (`core/stick_cursor.h`, `test_stick_cursor.c`, #138); the cursor over several lines with the d-pad and the stick (`test_cursor_nav.c`) | `test_text_rows.c`, `test_stick_cursor.c`, `test_cursor_nav.c` | no | only here |
| game worker: memory check and the model-size ladder (API 21), the capture ladder and probe, resident weights, model I/O through the broker, cancellation, the safe-boot exit | real `game_ocr/main.c` with stubs, real FL10/H15 when the models are present; with the real kernel (rows syscall, display-hook copy) in `test_game_model_broker.c` and under #74's measured pools in `test_alloc_sim.c` | the capture parity test (API 18's capture against API 19's, `test_capture_parity.c`) | as a plugin it could load into a game through Vita3K's taiHEN HLE, but its `vjoOcr*` syscalls come from our kernel plugin, which cannot load. `tools/vita3k_smoke.sh` runs its job code as a homebrew app instead, with an in-process kernel stand-in: a crash check, nothing more | only here |
| packaged modules: syscall ABI, firmware-independent kernel imports, ARM-only instructions, page budgets | `check_vita_syscall_abi.py`, `check_kernel_instructions.py`, `check_memory_ledger.py` in the `vita` job | | | |
| the release zips: their installer, `install_ftp.sh` (`--check-app`, `--self-test`, `--collect` and its analysis, `--uninstall`) | `tools/e2e_package.py` in the `package` job, on both builds' zips: the package's own scripts against the Vita's partitions as directories behind an FTP mock (`tools/tests/ftp_mock.py`) | | no | VitaShell's FTP server and the real partitions |
| SceShell's memory under our footprint | the replay model (section 4), from console samples | | not modelled: no budgets, no SceShell | the self-test ([self-test.md](self-test.md)) |

## 2. What CI runs (and why it is real code)

The host suites compile the plugin sources themselves (`#include "../../shell/worker.c"`,
`kernel/boot.c`, `game_ocr/main.c`), not copies, and replace only the SDK boundary: memory
blocks, threads, mutexes, files, the clock, and the kernel syscalls. Allocation failures,
failed joins, cancelled jobs and crashes between steps are injected there. 63 ctest suites with
the release models (`VJO_VOCR_MODEL_DIR`), 62 in the normal build, which has no self-test memory
routes to test (`kernel_meminfo`); the `host` job sets no model directory, so it runs 62 and 61,
without `vocr_model`
plus the Python tool suites run in the `host` job (with the frozen dictionary checked out, the
word lookup on real recognized lines and the step that unpacks the dictionary for it, which a pull request from a fork, without secrets, leaves out) (the Hachidori relay's contract
test went with the relay client, Google Lens's JPEG encoder and its first `test_jpegsw.py` with Lens, and the HTTP
transport test, `vjo-http-transport-tests`, with the network stack, whose `platform_vita` suite now
tests what is left of the Shell's platform: its files and its clock; API 23's Anki screenshot brought a smaller encoder back into the game worker,
with `jpeg_encoder`, every JPEG of it decoded by Pillow, and `game_shot`, a screenshot through the actual
worker and kernel transport into real files). The Overlay app's `.apkg` packages are imported there by Anki
itself, and a card's screenshot is checked as Anki shows it (the `<img>` of its rendered answer, the file in
its media):
the `anki` package (Anki 26.09.3's backend), hash-pinned in `tools/requirements-anki.txt`, in a
venv of its own (`VJO_ANKI_PYTHON`; `VJO_REQUIRE_ANKI=1` makes a missing one fail, not skip).
AnkiDroid and AnkiMobile have not imported one.

Both builds run (docs/release-variants.md): the `host` and `vita` jobs are a matrix of the debug
build (`VJO_DEBUG=ON`, the test builds') and the normal one (`OFF`), the ARM lane included
(`VJO_DEBUG=0` for `tools/arm_host_tests.sh`). The debug build's zip is the `VitaJPOverlay`
artifact as before; the normal build's is `VitaJPOverlay-normal`. The `package` job makes the
release's two zips of them as the final release does (`tools/package_variants.py`, with the models
and LMs `tools/prepare_ocr.py` downloads from vita-vn-ocr's public releases and checks against their
pins) and runs `tools/e2e_package.py` on each. `tools/preflight.sh` checks
either (`VJO_DEBUG=0` for the normal one), and the Vita3K smokes build each in its own directory.

### CI's Vita build in its container

The `vita` job builds in the `vitasdk/vitasdk:2026.08` container. The plugins link only libgcc
and the SDK's import stubs (`-nostdlib`), and their segments come out the same from another
VitaSDK. The Overlay app's `eboot.bin` links the SDK's libraries, which differ, so it does not:
for the same commit, v0.8.0-test.2's audit measured `6ee23974…` (segment 0 of 477,804 B) from the
fleet's VitaSDK, also GCC 15.2.0, against CI's `b60205c6…` (473,004 B). `tools/ci_container_build.sh`
builds a commit the way the job does, so its eboot can be compared with a release's:

```sh
bash tools/ci_container_build.sh --rev 4ac02ac --compare VitaJPOverlay-0.8.0-test.2.zip
bash tools/ci_container_build.sh --vocr-source /path/to/vita-vn-ocr --compare VitaJPOverlay-0.8.0-test.2.zip
```

`--variant normal` builds the normal build (`VJO_DEBUG=OFF`), to compare with the release's main
zip or CI's `VitaJPOverlay-normal` artifact; the default is the debug build.

- **The image, by digest.** `vitasdk/vitasdk@sha256:fd82d88ff456d118eef16533ebf9a3a6ae0e292684fb8a3a35f15f0707b4692d`,
  the tag's multi-platform index as CI pulled it for v0.8.0-test.2 (Release run 38689391878, job
  `vita`, 2026-10-15 05:55Z: its "Initialize containers" step prints `Digest: sha256:fd82d88f…`). Its
  `linux/amd64` image, the one CI's runner runs and the script asks for, is `sha256:5c0c8b36…`
  (GCC 15.2.0). The workflow names the tag: if the tag moves, that step's digest changes, and
  `VJO_CI_IMAGE` runs the script with the new one.
- **The job's steps.** The commit's tree (`git archive`, what `actions/checkout` gives; uncommitted
  changes are not built) is unpacked at CI's path, `/__w/vita-jp-overlay/vita-jp-overlay`. For a
  pull request CI builds its merge with the base, not its head: to compare with a pull request's
  artifact, `git fetch origin pull/N/merge` and pass `--rev FETCH_HEAD` (the head alone gives the
  same files only while the pull request is on its base's tip). Then the
  job's own commands run: its `apt-get install` (of whatever Ubuntu serves that day, as for CI),
  `tools/setup_toolchain.sh` (the checksum-pinned app packages, vita-headers, ScePaf,
  psp2cxml-tool) and `cmake -S vita -B build/vita -DCMAKE_BUILD_TYPE=Release`. With
  `--vocr-source` (a vita-vn-ocr clone holding the commit `cmake/VocrOCR.cmake` pins) its runtime
  goes to `.refs/vita-vn-ocr` and the build is the job's `cmake --build build/vita`; the `release`
  target, which needs the private dictionary, is not run. Without it only the Overlay app's VPK is
  built, with `VJO_WITH_VOCR=OFF`, which the app does not use.
- **Nothing of the host goes in.** The source goes in on standard input and the built files come out
  on standard output; nothing is mounted. The container needs the network, as CI's does: apt and
  GitHub (clones, release downloads, codeload.github.com). It runs with an init process and a name,
  and is removed whenever the script ends, so a `timeout` or Ctrl-C leaves no build running.
- **The comparison.** `--compare` takes an `eboot.bin`, a VPK, or a zip holding
  `JPOverlaySettings.vpk` (a release, or CI's `VitaJPOverlay` artifact). The VPK's files are compared,
  not its zip bytes: `vita-pack-vpk` records each file's time, so two builds never give the same
  VPK file. A package's plugins and RCO are compared as well when they were built. The script prints
  the built files' sha256 and exits 0 when they are identical, 1 on any difference, 2 before building
  for what it cannot use (an option, a revision, a reference without an `eboot.bin`, no docker or
  podman) and 3 when the build itself fails, its log in `--out`.

Measured on the fleet box with docker 25.0 (podman is supported by the script but was not run),
the image already pulled: building main `4ac02ac` (v0.8.0-test.2's commit), the Overlay app alone
took 66 s. Its `eboot.bin` is `b60205c61ed20c62c276ebb357aaac7c03134202fb17e681016011d13e9cc725`
(371,393 B), byte-identical to the one in `VitaJPOverlay-0.8.0-test.2.zip`'s VPK, and so are the
VPK's other five files. The whole build (with vita-vn-ocr `a540027`, 79 s) also gives the release's
kernel, Shell and OCR plugins and RCO byte for byte (`c8fc5018…`, `a1a0eee9…`, `79492384…`,
`2140d87b…`). Nothing here runs on a console; it shows the bytes are CI's, not that they work.

## 3. ARM under qemu-arm: the same C on the Vita's CPU

`tools/arm_host_tests.sh` builds the pure-C kernel and core suites with a Linux ARM toolchain
(`-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -static`, the flags vita-vn-ocr's runtime lane
uses) and runs them with `qemu-arm -cpu cortex-a9`:

| suite | cases | what the ARM build adds over x86_64 |
|---|---|---|
| boot_guard | 11 | 32-bit `uint32_t`/`int` arithmetic of the record, byte-identical record text |
| ledger | 2 | `__sync` atomics compile to LDREX/STREX; 32-bit counters |
| fbgrey | 4 | `core/fbgrey.h`'s word-at-a-time grey (two multiplies a pixel, the A1B5G5R5 expansion folded into the weights) against the per-channel code it replaced, for every R, G, B, alpha pattern and A1B5G5R5 value: its carries and wraps in 32-bit arithmetic |
| kernel_capture | 6 | the job's crop of the region (a box, a small frame, the whole frame, the 32 px minimum); the OCR grey rows in every pixel format; a shot's colour rows (API 23) in every format, into the worker's memory and a kernel strip; the first pass's region signature in every format, strip height and 400 random regions (the A1B5G5R5 one-pass rows never skip a sampled row); every box the region selector can save is cropped as drawn. API 24 removed the full-frame capture's tests with it |
| kernel_input | 14 | the trigger state machine |
| kernel_lifecycle | 5 | the process event hook (`kernel/lifecycle.c`, #104): a foreground change releases the game's input block in the hook's own call, an `__atomic_exchange_n` (LDREX/STREX) |
| kernel_game_ocr | 50 (49 normal) | the broker's 32-bit offsets and `SceOff` (64-bit on a 32-bit CPU); the rows syscall and its state machine; the job hand-off's bounded waits (take and read wake on submit and complete); a request by model sizes opens only its sizes' models by size (API 21); the frame-pacing step's 32-bit sums (API 22) |
| kernel_loader | 12 | the worker loader's question to the kernel worker (`kernel/loader.c`, #138): its 32-bit seqlock and tagged answer, and the Shell's `games.ini` rules on the Vita's word size |
| kernel_meminfo | 6 | the three kernel memory routes with the firmware's objects at 32-bit kernel-range, upper-kernel and user-range addresses (the user-half ones unreadable, at 0x5... and 0x8...), and the guard at each of its edges |
| touch_hold | 6 | the overlay page's hold on SceShell's front touch region (`shell/touch_hold.h`, #104) against a model of the region |
| region_select | 15 | the region selector's stick cursor (`core/region_select.c`): its 1/256-pixel position, speed and timer arithmetic in 32 bits |
| text_rows | 12 | the overlay arrows' row walk (`core/text_rows.c`, #81; across rows since the owner's 2026-10-16 rule): float cell edges and 32-bit offsets on the Vita's FPU |
| stick_cursor | 26 | the overlay's left stick (`core/stick_cursor.h`, #138): its 32-bit microsecond differences across the clock's wrap and its credit by the clock at 20-60 Hz, the cap, the deferred lookup, and the step along the text (the arrows' Left/Right without the wrap) with 32-bit offsets |
| cursor_nav | 13 | the cursor over several lines (the owner, 2026-10-16): the d-pad's Left/Right across rows to the text's ends, and the stick's four directions against the d-pad's from every character of nine layouts, with 32-bit offsets and float cells |
| settings_layout | 12 | the Overlay app's text layouts, icon PNG check and title letters (`settings/text_layout.c`, `settings/icons.c`, `settings/letter.c`): 32-bit `size_t` in the chunk walk, chunk lengths that would wrap it, each check against an unmapped page |
| shell_log | 13 (12 normal) | the Shell's logger (`shell/log_vita.c`) on an in-memory file system: a batch of lines gives the files the same bytes as line by line through `status.txt`'s 64 KiB cap and `log.txt`'s 1 MiB rotation, with 32-bit sizes; every append counted with its time (a job's done line) |
| apkg | 23 | the Overlay app's `.apkg` export (`core/sqlw.c`, `core/zipw.c`, `core/apkg.c`, `settings/anki_export.c`): 32-bit `size_t` in the SQLite B-tree, its overflow pages and the ZIP offsets, every storage failure, the sweep of screenshots no card names (#161) with its 64-bit file-time arithmetic on a 32-bit CPU; the four packages it writes (`--write`) are byte-identical to the host build's |
| psp | 10 | Adrenaline bubbles (`core/pspbubble.c`), PSP images (`core/psptarget.c`: EBOOT.PBP, ISO 9660, CSO and ZSO with LZ4; deflate only where zlib is, so not here) and the title IDs a bubble's row may carry (`core/taicfg.c` `vjo_tai_title_ok`) on synthetic files: 32-bit offsets into images, ARM's unsigned `char`, the damaged-image fuzz against reads outside the file |
| taicfg | 6 | the one-VPK install's `config.txt` editor (`core/taicfg.c`, #158) on its fixtures and a seeded corpus, each result checked with taiHEN's own parser, on the Vita's word size |
| capture_parity | 2 | with `VJO_VOCR_SOURCE_DIR`: API 18's and API 19's capture give the recognizer identical bytes on the Vita's CPU (procedural frames; `tools/capture_parity.py` adds the eval crops) |
| word_lookup | 1,321 lookups | with `VJO_PLAINTEXT_DIR`: `core/local_dict.c` with the bundled dictionary at every Japanese letter of the real VOCR lines in `host/fixtures/word_lookup` gives output identical to the host build's (entries, spans, definitions; `host/tests/word_lookup_parity.c`) |

Measured on the fleet box (Arm GNU Toolchain 14.2.Rel1, qemu-arm 10.0.13): all 248 cases of the
debug build pass (240 of the normal build, which has no `kernel_meminfo` and fewer log cases;
API 24 took `kernel_capture` from 11 cases to 6, #161 added 4 to `apkg`) [measured: the preflights' ARM logs, both builds],
and the eval-crop parity (372 regions × 3 formats) has no difference there either.
In CI it is the `arm` step of the host job (`gcc-14-arm-linux-gnueabihf`, `qemu-user-static`; the
pinned runtime's NEON kernels need GCC 14 or later, and Ubuntu 24.04's default cross gcc is 13). It
is not a Vita: no firmware, no taiHEN, no SceShell, no budgets; a pass here says the C is
32-bit clean, nothing more.

## 4. The Shell budget replay model

`tools/shell_budget_model.py` replays the boot and game-launch allocation sequence of the
current build against the console's measured SceShell memory:

- every item's size comes from `include/vjo_ledger_budget.h`, which CI ties to the packaged
  modules ([memory-ledger.md](memory-ledger.md));
- every sample comes from `docs/benchmarks/shell-budget-samples.json`, with its date, source,
  and the overlay build that was loaded when it was taken, measured from that release's SELF
  segments. A sample taken with build B loaded means SceShell had `free + pages(B)` without
  us; the headroom the current build leaves at each step is that minus the pages it holds.

Current build (`--anki` adds a card save), against the 11 October session (v0.7.4, CLANNAD in
front, 98,304 B free, the build holding 462,848 B):

```
step                                        held      headroom
Shell module loaded into SceShell        176,128       385,024
OCR worker pages in SceShell             176,128       385,024   (0: registered per game, not loaded there)
VjoNet thread                            196,608       364,544
VjoControl thread (boot complete)        217,088       344,064
game start: result memory (local)        217,088       344,064
Anki card saved: job thread              237,568       323,584
```

(The model holds the declared module pages: the online dictionaries' removal lowered them from
299,008 to 286,720 B, 12,288 B less at every step above, the Hachidori relay client's to
282,624 B, 4,096 B less again, AnkiConnect's to 266,240 B, 16,384 B less again, and Google
Lens's to 176,128 B, 90,112 B less again; the packaged module went from 294,912 B, main
`ba5f69a`'s, to 286,720 B (main `a5d79f2`), 282,624 B (main `9824b55`; #116 and #111 kept those
pages), 266,240 B (main `e53d9fb`), 262,144 B (main `ac905b1`, #122, to `6020c0b`, #124; still
266,240 B declared) and then to 176,128 B [measured: the build log's module row]. VjoNet's stack, 65,536 B until
#132, is 16,384 B: 49,152 B less from the VjoNet thread's step on. VjoAnki's
stack, 65,536 B until the network went, is 16,384 B: 49,152 B less at the Anki step.)

What this says, and what it does not:

- Before the fix (main at `ae94879`) the build held 532,480 B at the end of boot and would have
  left 28,672 B in that session: seven pages, less than the 65,544 B change that broke the boot
  in #42; an Anki save would have found no pages for its thread. The fix (worker `NO_RESIDENT`,
  the overlay's text arena out of `.bss`) held 389,120 B, 73,728 B below the v0.7.4 build that
  ran beside a game, and left 172,032 B at the end of boot in the same session; without
  AnkiConnect the build held 356,352 B and left 204,800 B (the model, with the module's
  266,240 B), without Google Lens it held 266,240 B and left 294,912 B (the module's
  176,128 B), and with VjoNet's 16 KiB stack (#132) it holds 217,088 B and leaves 344,064 B.
- `e759187`, the build that failed in #59, would show 20,480 B of headroom in the same
  scenario, positive. The model cannot prove that a build fits: SceShell's own use differs
  between sessions (a second sample, ownership unverified, had 28,672 B free), the daily
  LiveArea job's request size is unknown, and a private testing agent was resident in SceShell
  in both sampled sessions. It ranks builds and changes, and it says which step would fail
  first. The console decides.
- The PAF heap scenarios (2,538,568 B and 2,557,952 B free after a reboot; 355,256 B with a
  leaked 2 MiB pool) show every allocation of ours refused at the low-water mark and all of
  them fitting after a reboot, which is what the console showed.

`tools/tests/test_shell_budget_model.py` checks the arithmetic on a synthetic budget, that the
real tree's boot completes in the v0.7.4 scenario (the regression this guards), and that the
pre-fix footprint (`--shell-pages 397312 --worker-pages 45056`) ranks worse than the tree's.

## 5. Vita3K, precisely

Vita3K gained a taiHEN HLE in its PR #3885 (wiki page "Using taiHEN plugins", revised May
2026): `config.txt` is read from `ux0:tai`/`ur0:tai`, `*KERNEL`, `*TITLEID`, `*ALL` and `*main`
are parsed, and "all functions taiHEN itself relies on are implemented. Plugins that use
functions Vita3K does not implement likely will not work." The page's own note on `*main`:
"Shell support is pretty spotty". For us:

- the kernel plugin imports `SceDisplayForDriver`, `SceCtrlForDriver`, `SceTouchForDriver`,
  `SceProcessmgrForDriver` (hook targets), `SceSysmemForDriver`, `SceIofilemgrForDriver`,
  `SceThreadmgrForDriver` and `taihenModuleUtils`; none is a function "taiHEN itself relies
  on", and there is no firmware kernel behind them: it does not load;
- the Shell plugin needs SceShell and ScePaf, which do not exist in Vita3K; its memory
  question (pages in a 50 MiB partition shared with firmware) is the one Vita3K does not
  model at all;
- the game worker could be injected into a game, but without the kernel plugin its weak
  `vjoOcr*` imports stay unresolved and `module_start` starts nothing (by design: that is also
  what happens on a console whose kernel plugin failed).

So Vita3K can run none of the three plugins as they are, and it would say nothing about boot
or memory if it could: no `ScePhyMemPartShell`, no `ScePhyMemPartKD`, no `0x80024302`
[expert; AGENTS.md rule 5]. It is not wired into CI. `tools/vita3k_smoke.sh` (#74 preflight)
builds `tools/vita3k_smoke/` as a homebrew app: the real game worker's five self-test jobs and
the two other pixel formats in a Vita user process, on a 32 KiB thread stack, with the release
weights and an in-process stand-in for the kernel's syscalls. It passes when every job returns
and the app and the emulator exit 0: it catches crashes in that code, and is not evidence of
anything on a console. vita-vn-ocr uses qemu-arm, not Vita3K,
for the recognizer's ARM parity and cycle estimates, for the same reason.

The Overlay app ships as a system application (SELF attribute 0x0E, 50 MiB, since #109). The
Vita build checks the eboot it packages for that (`tools/check_settings_self.py`: the SELF's
control info and process parameters, the ELF's heap and thread calls; `tools/tests/test_settings_self.py`
on synthetic files), which is a check of the file, not of what a console does with it. The
smoke target below is built as an ordinary app (`vita-make-fself -c`: no attribute, no budget).
libvita2d picks its system-application path when `sceAppMgrGetBudgetInfo` succeeds, and
Vita3K's is a stub that returns -1, "not system mode" (Vita3K b75d5e6,
`SceAppMgrUser.cpp:234-238`; the logs of the smoke app's run and of the release VPK's show it
called), so under Vita3K libvita2d takes its ordinary path for any SELF. A pass is not evidence
for the system-application path on a console: the shared framebuffer, the VSH GXM
initialisation, the 50 MiB budget, or the exit through `vita2d_fini` that would crash it (#112:
found in libvita2d's source and the pinned `.a`, not run on a console). What else Vita3K does
with the release SELF's attribute has not been checked.
`tools/vita3k_settings_smoke.sh` builds the app's sources unchanged with a scripted controller
and counted textures (`tools/vita3k_settings_smoke.c`, linked by `-Wl,--wrap`; the
`VJO_SETTINGS_SMOKE` target of `vita/CMakeLists.txt`, never in the release), installs 120
synthetic games (`tools/make_settings_smoke_games.py`: generated titles and icons, some
missing, damaged, oversized or not PNG) and runs the script: the Games list scrolled down and
back, paged with L/R pressed and held, letters with Left/Right, toggles, the filters, a Start
rescan, the OCR & lookup page scrolled to its last card and back (its cut card clipped by
libvita2d's stencil), the other pages, Cancel. Toggles and the exit use the enter and cancel buttons the app
reads from the console's setting (#106; Vita3K's registry has ○ enter, and the script's old
× exited the app at its first toggle). It fails on a crash, a hang, an icon texture freed after a
draw with no GPU wait in between, or one left alive. Given `RELEASE_VPK`, the release
`JPOverlaySettings.vpk` must also keep running for 30 s over the same games. The app draws
text with the firmware's PVF library, which this Vita3K does not emulate (`scePvfNewLib` is
unimplemented): it needs Sony's 3.74 system software and font package
(`VITA3K_FIRMWARE`, `VITA3K_FONTS`), which Vita3K then loads as the real modules. A pass
says the app's own code survives that script under Vita3K's GPU emulation; its timings are
the host's dynarec and are not used. (Vita3K does not fault on a texture freed while the GPU
reads it, so that is counted, not observed: main `7ed16d8`'s app freed 365 of its 366 icon
textures so, with 66 failed reloads of broken icons, and exited 0.) If `vita-elf-create` refuses
the smoke app's ELF ("segment 1 overlaps": its code segment ends too close to the data
segment), the script rebuilds it with 4 KiB of padding (`VJO_SETTINGS_SMOKE_PAD`).

The harness's `sceKernelExitProcess` does not return. On a console it should not either: the
caller is one of the process's threads, which the kernel ends "without being allowed to be
executed to completion" (`SDK_doc__System__Kernel-Overview_e.txt`, "Resource Collection When
Terminating a Process"; an inference from that passage, not run on a console, since the call's
own reference says only that it exits the process).
Vita3K's returns (b75d5e6, `SceLibKernel.cpp:1293-1297`), and since #112 the app exits with GXM
still up, as the release does. Its `main` then returned into newlib's exit, whose
`_free_vita_heap` frees the heap block that holds libvita2d's GXM context (Vita3K builds the
context in the app's host memory, `SceGxm.cpp:1985-1988`), and Vita3K's shutdown read that
context to destroy it (`app_init.cpp:476`): SIGSEGV, "Unhandled access to 0x481800008", exit
133 after the app's exit 0, which failed the "Vita3K app" row until #108. The core dump's
stack is `destroy_gxm_context` ← `gxm::destroy_all_contexts` ← `app::shutdown_app_runtime`; a
diagnostic run found the context at 0x81800008 in the app's 8 MiB heap block at 0x81800000,
gone when newlib's exit called the wrapper a second time, and Vita3K maps guest memory from
0x400000000 (`mem.cpp:76`), so the fault is that context. #108's harness called `vita2d_fini`
before the exit, which removed the context first, with a teardown the release never runs; the
wrapper now runs the release's exit unchanged. A crash in Vita3K's shutdown still fails the
run, and the failure says what ended Vita3K: the signal (its exit status less 128) and dynarmic's
"Unhandled SIGSEGV at rip 0x…", which goes straight to stderr. Vita3K's own "[signal_handler]:
Unhandled access to 0x…" is quoted when its logger got it out; in some crashed runs the SIGTRAP
cut it off. The only critical line of a passing run is "[translate_path]: Unknown device with
path gro0:app used" (the app lists `gro0:app`, `settings/catalog.c`), which the failure used to
quote as the crash. When `gro0:app` does not open, the app looks at `gro0:` itself (#138); in
Vita3K that is an error line, not a critical one: "[stat_file]: Cannot find device for path:
gro0:" (the preflight's run of `6be519f`).

Vita3K does not fault on an access to unmapped guest memory either. It logs "Invalid write …",
"Invalid read …" or "Invalid exclusive write …" with the guest's PC and goes on, a read giving 0
and a write dropped (b75d5e6 `vita3k/cpu/src/dynarmic_cpu.cpp:161-173, 198-213, 234-248`). So a
NULL write in the app's toggle, which a console ends the app on, passed the script (#121's
audit). It also logs an instruction it cannot run and goes on (`:272-317`): an undefined one,
which a console faults on, or one it does not implement, its own gap. Any of these lines now
fails the run, the scripted app's and the release VPK's. The failure quotes the first such line
and the number of such lines: for an invalid access with the PC, SP and LR Vita3K logs after it
(the PC is where its JIT block began, so it names the function rather than the line), for an
instruction with its address (Vita3K logs two lines for each). No passing run's log has one. A
write to the app's own code or read-only data is neither faulted nor logged: Vita3K maps the
module's segments writable (`load_self.cpp:651`, `mem.cpp:183`), while on a console the code
segment is read-only, so such a write would end the app there (not run on a console). A jump to
address 0 shows as invalid reads at PC 0 until the timeout ends the run.

## 6. What only the console answers

1. Whether the boot completes with our pages present (the self-test's `boot`/`ok` steps and
   the boot guard's record).
2. SceShell's free partition at boot and at game launch with the current build, and the
   kernel pool's free size (the self-test's `mem`/`kernel` lines). These replace the model's
   samples and the ledger's ratchet.
3. The game's free `USER` per title (the self-test's `game` line; the fleet's model-limit
   work asks for every title tried).
4. The hooks: input filtering, capture, process events; and the PAF UI.
5. CPU3 load, by PSVshell readings with matched clocks, plugin set and workload (unmeasured
   until then; AGENTS.md rule 8).
