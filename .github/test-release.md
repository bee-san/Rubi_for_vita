**One combined test build for one console run: Rubi `main` at `3f864a3` (0.10.0 and the code cleanup since) plus every open draft PR that waits for the owner's console test, first among them the 512 KiB OCR game memory reserve (#515).** The steps and what to send back are in the one test issue, #351 ("Test build: everything waiting for a console test"), which lists every build.

- **This is a pre-release, not "Latest":** v0.10.0 stays the release.
- **The drafts in it are not merged.** The scratch branch `test/combined` merges them for this test only (their conflicts are resolved there; API 55). Each new test build is `v0.10.0-test.N` from the same branch.
- **Nothing new in it has run on a console yet.**

## What is in it

| PR | What | Default |
| --- | --- | --- |
| #515 (draft) | **OCR keeps 512 KiB of the game's free memory** instead of #280's 1 MiB (0.10.0's), for every model size: a game short of memory reads with a larger model, or reads where it was refused. A first job at the full screen needs about 1,508 KiB free with Small, 1,924 KiB with Normal and 3,260 KiB with Large (2,020, 2,436 and 3,772 KiB in 0.10.0; estimates). A native hook's capture-only pass keeps its 1 MiB. | always on in this build |
| #318 (draft) | **PSP text in Adrenaline**: Memories Off 6 ～T-wave～ and Memories Off ～それから～ read from PSP memory, read-only, shown once OCR agrees; the PSP probe's fix (#248), its `psp_probe.log` format 4 in the -debug build | only with **Text source → Every game** |
| #326 (draft) | **Overlay on the game's text** (#309): the word marked where the game draws it, taps on the game's text, the popup beside the text | OFF (OCR & lookup → **Overlay on the game's text**) |
| #359 (draft) | **Text past an animated icon** (#353): when the region keeps changing, the reading watches the found lines' own pixels and reads them anyway at the limit (such a job finds its lines with Rubi's finder, the watch's own, not the spotter) | always on |
| `main` | everything of 0.10.0 (#280's 1 MiB reserve, the stats rework: the streak in the overlay's △ view, no Stats tab or Cards page), and since: **#355, Pause the game to read** (merged, off by default), **VOCR6-V for vertical text** (#505, #509), #500 and #504 (#280's audits), and **the code cleanup** (#418: about 75 PRs; SceShell 8 KiB of pages smaller in each build, the kernel 4 KiB, the worker 4 KiB in each game, the Rubi app's .bss 78,352 B smaller; no setting changes) | as on `main` |

With **Overlay on the game's text** ON, a trusted hook does not skip OCR in this build: every read recognizes, to place the text. Once a reading watches its lines (#359), it finds them with Rubi's finder (GRAD), which the watch compares them with; every other reading finds them with `main`'s spotter (the debug log's `spot` step of a watched job: `rc=0xFFFFFF88`).

Not in it: draft #363 (the licence: nothing to test on a console).

## Install

1. **Back up.** Export all cards on the Rubi app's Anki page.
2. **Close the Rubi app if it is open**, even suspended behind VitaShell (pull the top-right corner of its LiveArea page).
3. **Install the VPK.** Copy the **-debug** VPK of this release to the Vita and install it in VitaShell: Yes, then Yes to the extended-permissions notice.
4. **Update.** Open **Rubi** and choose **Update** on its Install page. It says it goes back from v0.10.0 (or updates from an earlier test build) to this test build: a test build sorts before its release, although its code is newer. Then restart the Vita yourself.

You need **Settings > HENkaku Settings > Enable Unsafe Homebrew** on.

**If a game crashes or hangs:** turn the game OFF on the Rubi app's Games page, or hold **L** while you start it.

**If the Vita does not start:** hold **L** while you turn it on (h-encore/h-encore2: open its menu, holding R as h-encore starts, and choose Exit while holding L). Then, in VitaShell, put `#` before the two `VitaJPOverlay` lines of `ur0:tai/config.txt` and `ux0:tai/config.txt`, or open **Rubi** and choose **Uninstall** (it keeps your settings and cards). Run it **once**; don't reboot to retry.

**Back to 0.10.0:** install `Rubi-0.10.0.vpk` from the v0.10.0 release and choose Update.

## The build

- **Source:** `test/combined` = `main` `3f864a3` + the 512 KiB reserve (#515, `test/ocr-reserve-512k` at `cbb42ad`) + #318 (`adrenaline/psp-text` at `d0634fe`) + #326 (`feat/text-overlay-309` at `956833f`) + #359 (`feat/spinner-353` at `472f8ae`), merged afresh on `main` (v0.9.8-test.6's branch is kept as a parent; its resolutions were the reference), with the merges' conflicts resolved; the tag is on the branch's head, whose last commit adds only this note, the build's version and the publishing step.
- **Both builds:** `-debug` (the one to install: the long log, the PSP probe) and the normal build, each zip checked by `tools/e2e_package.py`, card mining on as in every test build.
- **Memory:** the game worker's pages are measured on the builds (the game's own memory): 229,376 B (-debug) and 217,088 B (normal), 12 KiB more than `main`'s in each (217,088 and 204,800 B: the drafts' code); SceShell's (167,936 / 155,648 B) and the kernel's pages are `main`'s. The 512 KiB reserve takes no memory: it is what a job leaves free.
