**One combined test build for one console run: Rubi `main` at `6bfd1f4` plus every open draft PR that waits for the owner's console test.** It replaces the separate test builds and test issues: the steps and what to send back are in the one test issue, #351 ("Test build: everything waiting for a console test"), which lists every build.

- **This is a pre-release, not "Latest":** v0.9.8 stays the release.
- **The drafts in it are not merged.** The scratch branch `test/combined` merges them for this test only (their conflicts are resolved there; API 53). Each new test build is `v0.9.8-test.N` from the same branch.
- **Nothing new in it has run on a console yet.**

## What is in it

| PR | What | Default |
| --- | --- | --- |
| #318 (draft) | **PSP text in Adrenaline**: Memories Off 6 ～T-wave～ and Memories Off ～それから～ read from PSP memory, read-only, shown once OCR agrees; the PSP probe's fix (#248), its `psp_probe.log` format 4 in the -debug build | only with **Text source → Every game** |
| #326 (draft) | **Overlay on the game's text** (#309): the word marked where the game draws it, taps on the game's text, the popup beside the text | OFF (OCR & lookup → **Overlay on the game's text**) |
| #355 (draft) | **Pause the game to read** (#348): each reading copies the text box from one frame at once, so a spinning or blinking icon in it can't stop the read | OFF (OCR & lookup → **Pause the game to read**, the last card) |
| #359 (draft) | **Text past an animated icon** (#353): when the region keeps changing, the reading watches the found lines' own pixels and reads them anyway at the limit | always on |
| `main` | everything of 0.9.8, and since: why the overlay waits (#364, #358), TIME TRAVELERS' Luna hook (#352, #349), trusted hooks with no memory (#354), a smaller model when the weights are refused (#324), verified hooks for 善人シボウデス (#356, and the ZERO ESCAPE Double Pack's, #362) and 蒼の彼方のフォーリズム (#350), CLANNAD's name variables (#357), the hook request form (#367) | as on `main` |

With **Overlay on the game's text** ON, a trusted hook does not skip OCR in this build: every read recognizes, to place the text. Draft #318's stop after three changed-screen tries is not in this build: `main`'s #364 replaces it (the tries go on, with the hint).

## Install

1. **Back up.** Export all cards on the Rubi app's Anki page.
2. **Close the Rubi app if it is open**, even suspended behind VitaShell (pull the top-right corner of its LiveArea page).
3. **Install the VPK.** Copy the **-debug** VPK of this release to the Vita and install it in VitaShell: Yes, then Yes to the extended-permissions notice.
4. **Update.** Open **Rubi** and choose **Update** on its Install page. It says it goes back from v0.9.8 (or updates from an earlier test build) to this test build: a test build sorts before its release, although its code is newer. Then restart the Vita yourself.

You need **Settings > HENkaku Settings > Enable Unsafe Homebrew** on.

**If a game crashes or hangs:** turn the game OFF on the Rubi app's Games page, or hold **L** while you start it.

**If the Vita does not start:** hold **L** while you turn it on (h-encore/h-encore2: open its menu, holding R as h-encore starts, and choose Exit while holding L). Then, in VitaShell, put `#` before the two `VitaJPOverlay` lines of `ur0:tai/config.txt` and `ux0:tai/config.txt`, or open **Rubi** and choose **Uninstall** (it keeps your settings and cards). Run it **once**; don't reboot to retry.

**Back to 0.9.8:** install `Rubi-0.9.8.vpk` from the v0.9.8 release and choose Update.

## The build

- **Source:** `test/combined` = `main` `6bfd1f4` + #318 (`adrenaline/psp-text` at `d0634fe`) + #326 (`feat/text-overlay-309` at `956833f`) + #355 (`feat/pause-348` at `9e44739`) + #359 (at `f630b69`), with the merges' conflicts resolved; the tag is on the branch's head, whose last commit adds only this note and the build's version.
- **Both builds:** `-debug` (the one to install: the long log, the PSP probe) and the normal build, each zip checked by `tools/e2e_package.py`, card mining on as in every test build.
- **Memory:** the game worker takes 16 KiB (-debug) and 8 KiB (normal) more than `main`'s in each game that is ON, in the game's own memory (measured on the builds); SceShell's and the kernel's pages are `main`'s. With **Pause the game to read** ON, a reading also holds one block of the text box's size (at most 512 KiB) while it runs, only when it fits.
