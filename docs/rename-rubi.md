# Renaming the overlay to Rubi

The owner renamed the project on 2026-10-16 (13:04Z): it is **Rubi**, long form **Rubi for
Vita**, and the repository is `bee-san/Rubi_for_vita` (renamed 2026-10-17, 07:3xZ; GitHub redirects the old
name). This page is the plan, approved by the conductor (2026-10-16 14:27Z), and the record of what
the rename pull request did on main `ad4e837` (GO RENAME came on `caad0e5`; #188, #186 and #184, which name
nothing, merged while it was open), after every pull request on the conductor's list had merged.

The rule: every name a user sees changes; every internal identifier that installs, paths,
taiHEN lines or saved data depend on keeps its old value, so an update is an ordinary update:
no reinstall, no second bubble, no moved data, no second Anki deck.

## The new names

| What | Before | After |
| --- | --- | --- |
| The product | Vita JP Overlay, JP Overlay | **Rubi** (short), **Rubi for Vita** (long form, titles) |
| The settings app on LiveArea (param.sfo `TITLE`, `STITLE`) | Overlay (0.8.0 test builds), JP Overlay Settings (0.7.12 and older) | **Rubi** |
| The app in prose | the Overlay app | the Rubi app |
| The in-game popup | the overlay | the overlay (a common noun, unchanged) |
| Repository | `bee-san/vita-jp-overlay` | `bee-san/Rubi_for_vita` (fork of `Dartv/vita-jp-overlay`, unchanged) |
| Release zips | `VitaJPOverlay-<version>.zip`, `-debug.zip` | `Rubi-<version>.zip`, `Rubi-<version>-debug.zip` |
| One-VPK release assets (overlay-one-vpk-install) | `VitaJPOverlay-<version>.vpk`, `-debug.vpk` | `Rubi-<version>.vpk`, `Rubi-<version>-debug.vpk` |
| The app's VPK in a zip, and on the Vita | `JPOverlaySettings.vpk`, `ux0:data/JPOverlaySettings.vpk` | `Rubi.vpk`, `ux0:data/Rubi.vpk` |
| CI artifacts | `VitaJPOverlay` (debug), `VitaJPOverlay-normal` | `Rubi`, `Rubi-normal` |
| Log lines | `Vita JP Overlay kernel N started`, `Vita JP Overlay shell started`, `TITLEID: JP Overlay disabled`, `overlay app: ...` | `Rubi kernel N started`, `Rubi shell started`, `TITLEID: Rubi disabled`, `Rubi app: ...` |

## What keeps its name, and why

| Kept | Value | Why |
| --- | --- | --- |
| Title ID | `VJOSET001` | Another ID is another app: a second bubble, a reinstall, the old app's files orphaned. With this one the new VPK installs over the old app as an update. |
| Data folders | `ux0:data/VitaJPOverlay/`, `ur0:data/VitaJPOverlay/` | They hold `config.ini`, `games.ini`, the regions, the Anki queue and exports, the models, the 150 MiB dictionary (157,298,688 B in both zips), the logs and the boot record. Moving them means a migration on the console for a name only VitaShell shows. |
| Plugin files, module names | `VitaJPOverlay_Kernel.skprx`, `VitaJPOverlay_Shell.suprx`, `VitaJPOverlay_OCR.suprx` | The taiHEN lines name the files; the kernel loader loads the worker by its path (`include/vjo_api.h`); the Shell finds the kernel module by its name (`shell/boot.c`); the app's Install page finds its own taiHEN lines by them (`settings/install.c`, #158, #166); the installers, `--uninstall`, `--collect` and the analyser find earlier installs by these names. |
| taiHEN lines and the app's note | `# VitaJPOverlay: the OCR worker in this Adrenaline bubble's PSP game (Overlay app)` | The app (`core/taicfg.c`) and `tools/patch_tai_config.py` find and remove their own lines by this exact text. |
| Kernel export library | `VitaJPOverlayForUser` | Its NIDs come from the name; the Shell and the worker import them (`tools/check_vita_syscall_abi.py`). |
| RCO | `vitajpoverlay.rco`, resource `vitajpoverlay_plugin` | Its string table is empty: it shows no text. |
| Stamps, identifiers | `vjo_app.txt` (`vjoapp1 ...`), `vjo_*`, `VJO_*`, `Vjo*`, CMake projects and targets, the Vita3K smoke VPK (`VJOSMK002`) | Internal; the stamps are compared across builds. |
| Anki deck and tag | deck **Vita JP Overlay** (ID `1760534400000`), tag `vita-jp-overlay` | Anki matches an imported deck by its name: a new name would add a second deck beside the old one in a collection that has imported cards (the owner's rule: no duplicate decks). The tag stays for the same reason: one tag for all the owner's cards. The docs say the deck keeps its name from before the rename. |
| History | published releases and their assets (`VitaJPOverlay-0.7.12.zip`, the 0.8.0 test builds), dated records (`docs/history/todo-20261011.md`, `docs/history/plugin-audit*`, `docs/history/issue-59-root-cause.md`, `docs/benchmarks/*.json`), test fixtures that are old logs or files | Facts about those builds. Their GitHub links are updated (the commits are the same). |
| The `JP` placeholder | a game card without its icon | Not the product's name. |

Nothing internal is renamed, so nothing needs a migration or a console test of one.

## What changes, by surface

1. **LiveArea.** `vita/CMakeLists.txt` `NAME "Overlay"` becomes `NAME "Rubi"`, which sets
   param.sfo `TITLE` and `STITLE` (checked: `vita-mksfoex` writes the name into both). The
   gate `startup.png`, the background `bg.png` and the splash `pic0.png` show a drawn
   "Overlay" wordmark: `tools/make_settings_assets.py` draws "Rubi" in the same round strokes
   (cap height and stroke ratios of "Overlay"; prototype renders in
   `shared/artifacts/rubi-rename/art/v1/`, all three opaque 8-bit palette PNGs, 15.6, 55.2
   and 68.7 KB). `icon0.png`, the bubble, has no text and stays.
   `settings/assets/ICON-SOURCE.md` says so.
2. **The app.** Its header (`settings/main.c`) says Rubi, with the version beside it
   (`Rubi v0.8.0`, `Rubi v0.8.0 debug`); the three of `settings/app_check.c`'s six red-banner
   messages that name the app say "Rubi app" and `ux0:data/Rubi.vpk`; its `log.txt` lines start
   with `Rubi app: `.
3. **The plugins.** The Shell's out-of-date notices (`core/app_stamp.h`), its Anki queue line
   (`shell/overlay.cpp`) and start line (`shell/worker.c`), the kernel's start line
   (`kernel/main.c`). Every changed string is shorter than the old one, so no module can grow
   (the memory ledger checks pages in the build).
4. **Files written on the Vita.** The `config.ini` template's header and comment lines
   (`core/config.c`, and `docs/config.example.ini`, which `test_core` compares with it) and
   the `games.ini` header (`core/games.c`). An existing `config.ini` keeps its header: the app
   patches lines.
5. **Installers and tools.** `install-vita.sh`, `tools/install_ftp.sh` (uploads
   `ux0:data/Rubi.vpk`), `tools/app_stamp.py`'s boxes, `tools/migrate_config.py` and
   `tools/install_dictionary.py` messages. The readers of earlier installs accept both names:
   `tools/analyse_selftest.py`, `tools/analyse_timeline.py` (the old start lines, prefixes and
   the disabled line) and `tools/collect_vita.py` (hashes `ux0:data/Rubi.vpk` and an earlier
   install's `ux0:data/JPOverlaySettings.vpk`). Every install before the rename uploaded the app
   to `ux0:data/JPOverlaySettings.vpk`, and that build's app, still the installed one until the new VPK
   is installed, tells the user in its red banner to install that file. So `tools/install_ftp.sh`
   puts this build's app there as well, where the file exists (never creating it): the old banner then
   installs the new app, not the old one beside new plugins (#101; audit-rubi-rename B1). Once the Rubi
   app is installed the file is no longer used and can be deleted (the guide says so). The one-VPK
   install needs nothing: VitaShell installs the new app itself.
6. **Packages and CI.** `vita/CMakeLists.txt` (VPK target, zip names, build messages),
   `tools/package_release.py`, `tools/package_variants.py`, the one-VPK packager,
   `tools/e2e_package.py`, `tools/preflight.sh`, `.github/workflows/release.yml` (artifact
   names, the package job's globs and version parsing), `tools/ci_container_build.sh` (the VPK
   name, CI's checkout path `/__w/Rubi_for_vita/Rubi_for_vita`; `--compare` accepts a release
   zip with either VPK name).
7. **README and the reference docs.** The product name, "Overlay app" becomes "Rubi app",
   **Overlay** becomes **Rubi**, with one history note (the app was Overlay in the 0.8.0 test
   builds and JP Overlay Settings in 0.7.12 and older; the title ID is the same, so the new VPK
   installs over either). The one heading with the name (`docs/vita-memory.md` 5.4) and its one
   anchor link (`docs/settings-readability.md`) change together.
8. **Links.** Every `github.com/bee-san/vita-jp-overlay` URL (31 in 12 files at `ed9fbe7`,
   pinned commit links included) and every `gh -R bee-san/vita-jp-overlay` (7) name
   `bee-san/Rubi_for_vita`; `AGENTS.md`'s fork rules too. Links to `Dartv/vita-jp-overlay`
   stay: that is the upstream's name.
9. **Comments.** "the Overlay app" in code comments becomes "the Rubi app" (no effect on any
   built file), so that a search for the old names finds only history and the kept identifiers.

## Inventory, before and after

`shared/artifacts/rubi-rename/inventory.py` counts each form in the tracked text files, leaving out this page,
on main `ad4e837` and on the pull request's head. What is left of a renamed form is history (dated records,
old logs as test fixtures), the readers' patterns and tests for an earlier build's lines and files, and the
notes that say what an old name was; `name_sweep.py` lists each such line with its reason.

| Form | `ad4e837` | Head | Becomes |
| --- | ---: | ---: | --- |
| `Vita JP Overlay` | 59 in 31 files | 45 in 19 | Rubi; left: the Anki deck, old-build logs |
| `JP Overlay Settings` | 5 in 5 | 6 in 6 | history notes; a value the param.sfo check refuses (its test) |
| `JP Overlay` (other) | 15 in 7 | 6 in 3 | Rubi; left: old-build logs, the readers' patterns |
| `Overlay app` | 548 in 147 | 17 in 8 | Rubi app; left: the taiHEN note, old-build logs |
| `**Overlay**` in docs | 15 in 6 | 2 in 2 | **Rubi**; left: history notes |
| `"Overlay"` literals | 7 in 5 | 2 in 2 | `"Rubi"`; left: the wordmark's history note, a value the param.sfo check refuses (its test) |
| GitHub URLs to this repository | 30 in 12 | 0 | `bee-san/Rubi_for_vita` |
| `bee-san/vita-jp-overlay` outside a URL | 9 in 4 | 2 in 2 | `bee-san/Rubi_for_vita`; left: AGENTS.md's note |
| `VitaJPOverlay-<version>` file names | 79 in 16 | 6 in 4 | `Rubi-<version>`; left: published releases', the readers |
| `JPOverlaySettings.vpk` | 103 in 33 | 30 in 10 | `Rubi.vpk`; left: an earlier install's copy, refreshed and read |
| CI artifact names | 10 in 4 | 3 in 3 | `Rubi`, `Rubi-normal`; left: notes |
| kept: data paths, module files, title IDs, RCO, export library, deck constant | 447, 618, 115, 37, 3, 4 | 448, 618, 121, 37, 3, 4 | unchanged (the one more data path is a test's fixture line; the six more title IDs are the param.sfo check and its tests) |

## The repository rename

1. Done first (`gh repo rename Rubi_for_vita`, 2026-10-17 07:3xZ), so that the pull request's new
   links resolve as soon as it merges. Its description is the README's pitch: "Learn Japanese with
   your PS Vita: look up words from the games you play. Fully offline."
2. GitHub redirects the old name's web pages, API, git fetch and push, and release downloads.
   Checked after the rename: the old web URL lands on the new one (200), the API answers
   `bee-san/Rubi_for_vita`, `gh pr list -R bee-san/vita-jp-overlay` lists the open pull requests,
   a v0.7.1 release asset downloads (206 for a range) and `git ls-remote` over the old SSH URL
   gives main. So the fleet's `gh -R bee-san/vita-jp-overlay` and old clones keep working.
   A new repository named `bee-san/vita-jp-overlay` would end the redirects: never create one.
3. Secrets (`VOCR_SOURCE_KEY`, `PLAINTEXT_DICTS_SOURCE_KEY`), branch protection, issues, pull
   requests and releases stay with the repository. The deploy keys belong to vita-vn-ocr and
   plaintext-dicts and are not affected.
4. CI's checkout path becomes `/__w/Rubi_for_vita/Rubi_for_vita`. It does not enter the built
   files: `tools/ci_container_build.sh` built main `ed9fbe7` (vita-vn-ocr `d214a05`) in CI's
   container at the old and at the new path, for the debug and the normal build, and the
   `eboot.bin`, the three plugins, the RCO and all eight files in the VPK are byte-identical
   (only the VPK file itself differs, as it does between any two builds: `vita-pack-vpk`
   records file times). So the script takes the new path, and its `--compare` with releases
   built before the rename still holds.
5. This clone's `origin` becomes `git@github.com:bee-san/Rubi_for_vita.git`; other clones work
   through the redirect.

## Sibling repositories

| Repository (commit) | What names the overlay | Change |
| --- | --- | --- |
| bee-san/vita-vn-ocr (`b3b136d`; `378e354` when changed) | 129 GitHub URLs in 22 files (128 in Markdown, 1 in `ports/meiki/dependencies.json`) | #95, merged as `8a9ff96`: the URLs, the README's five mentions of the project and `notes/10`'s remote. Protocols, result JSON, code comments and the other notes' prose are records and stay. CI (pytest, ruff) green, and run locally first (1826 passed). |
| bee-san/plaintext-dicts (`f3b228b`; `e9b6e76` when changed, private) | no URL; the README names the overlay three times | #4, merged as `bda2d89`: the README. `ux0:data/VitaJPOverlay/dictionaries/` in `tools/convert_dictionary.py` is a kept path. The overlay's pin (`e9b6e76`, ultimate-v1) does not move: the dictionary does not change. Merged only after ultimate-v1 was released (19:48Z), so that it did not collide with its #3 (conductor, 14:27Z). No CI there; Markdown only. |
| bee-san/hachidori (`dfc5e0c`), bee-san/hoshidicts (`20019e4`) | nothing (0 matches for any form; GitHub code search over bee-san finds only the two above) | none |

## Promo, README rewrite and the fleet

- **Promo.** The captions already say Rubi (promo v2's brief). The Vita3K captures of the app
  show its title as Overlay, and the VitaShell capture's stand-in file is
  `VitaJPOverlay-0.8.0.vpk`: the final cut re-captures them from the renamed main
  (promo-assets' `RECAPTURE.md`). The promo agents have finished; the conductor relays.
- **overlay-readme.** Its pull request (a short new README; today's README moved verbatim to
  `docs/guide.md`) merges after the rename (conductor, 22:45Z): it rebases, moves the renamed
  README and uses the new names. When the repository is renamed its description (empty today)
  becomes the README's pitch: "Learn Japanese with your PS Vita: look up words from the games
  you play. Fully offline."
- **Fleet prompts** that name `bee-san/vita-jp-overlay` keep working through the redirect.
  Prompts that download the CI artifact `VitaJPOverlay` must use `Rubi` for runs after the
  merge (older runs keep their old artifact names).

## Verification

Off the console, on the pull request's tree (the pull request lists the commands and their output):

- Host build and every ctest suite: debug 70 of 70, normal 69 of 69 (`apkg_packages`' Anki import
  runs in CI: the pinned `anki` wheel needs glibc 2.35, the fleet box has 2.34).
- Vita build, both builds, Release: every check of the build (kernel instructions, syscall ABI, memory
  ledger: pass, the app's PNG ABI, SELF and artwork, and since audit-rubi-rename's note N3 the VPK's
  param.sfo: `TITLE` = `STITLE` = `Rubi`, `TITLE_ID` = `VJOSET001`, `tools/check_settings_assets.py`). The base zips `Rubi-0.8.0.zip` and `Rubi-0.8.0-debug.zip` hold `Rubi.vpk` and
  every kept path (`ur0/tai/VitaJPOverlay_*.s?prx`, `ur0/data/VitaJPOverlay/vitajpoverlay.rco`, ...).
- `tools/package_variants.py` with the pinned VOCR5 models and LMs: `Rubi-0.8.0.vpk`, `-debug.vpk`,
  `.zip`, `-debug.zip` and `SHA256SUMS`; `tools/package_vpk.py --verify` on both VPKs; `tools/e2e_package.py`
  on both zips: 151 of 151 checks each, with the update over test.2's state refreshing its
  `ux0:data/JPOverlaySettings.vpk` and a fresh install making none (audit-rubi-rename B1).
- `tools/preflight.sh` with every optional input: GO for both builds, 0 checks skipped.
- The art: `tools/make_settings_assets.py` with the pinned Pillow writes `startup.png`, `bg.png` and
  `pic0.png` byte for byte as the reviewed prototype; `icon0.png` is unchanged; `tools/check_settings_assets.py`
  passes.
- The readers: a test feeds the same log with the old and the new names to `tools/analyse_selftest.py`
  and gets the same summary, and checks that today's sources write the new names
  (`tools/tests/test_analyse_selftest.py` `RenamedLines`, `test_analyse_timeline.py`: dropping either name
  from a pattern of `tools/analyse_selftest.py`, `tools/analyse_timeline.py` or `tools/collect_vita.py` fails a
  test; `tools/package_vpk.py --verify`'s old file name and `tools/ci_container_build.sh --compare`'s old VPK
  name were checked by hand, without a test: no `VitaJPOverlay-<v>.vpk` was ever published, and the latter
  needs docker); `--collect` hashes an earlier install's
  `ux0:data/JPOverlaySettings.vpk` and lists it as not in the package (`tools/tests/test_collect.py`).
- Every distinct `github.com/bee-san/Rubi_for_vita` link in the tree answers 200 (24 URLs).
- CI's checkout path: measured before the change (above), the built files do not depend on it.
- The Anki deck's name and ID are pinned by a test (`host/tests/test_apkg.py`).

Console: not needed to merge. Nothing is added to the boot path (no page, thread or allocation), and
every changed string in a plugin is shorter than the one it replaces. The next test build shows the new
title and art; the owner's check there: the bubble says Rubi, the app opens with its saved settings,
games still get OCR, and an Anki export lands in the existing deck.

## Order after GO RENAME (as done)

1. Take the inventory again on the main of that moment.
2. Rename the repository and this clone's remote.
3. On `rename/rubi`: the changes above, scripted where they are mechanical and reviewed line by
   line; regenerate the art with `tools/make_settings_assets.py`.
4. The local checks above, then one push and one pull request (`gh pr create -R
   bee-san/Rubi_for_vita --base main`), CI on both builds.
5. The audit (`audit-rubi-rename`), fixes, merge on MERGE_OK.
6. The sibling repositories' pull requests.
7. Tell the conductor, who relays to the promo cut and the fleet, and overlay-readme.
