**One combined test build for one console run: Rubi `main` at `892cfb9` (0.9.8) plus every unmerged PR that waits for the owner's console test.** It replaces the separate test builds and test issues: the steps and what to send back are in the one test issue, "Test build: everything waiting for a console test".

- **This is a pre-release, not "Latest":** v0.9.8 stays the release.
- **The PRs in it are not merged.** The scratch branch `test/combined` merges them for this test only (their conflicts are resolved there; API 51). Each new test build is `v0.9.8-test.N` from the same branch and is listed in the same issue.
- **Nothing new in it has run on a console yet.**

## What is in it

| PR | What | Default |
| --- | --- | --- |
| #318 (draft) | **PSP text in Adrenaline**: Memories Off 6 ～T-wave～ and Memories Off ～それから～ read from PSP memory, read-only, shown once OCR agrees; the PSP probe's fix (#248), its `psp_probe.log` format 4 in the -debug build | only with **Text source → Every game** |
| #326 (draft) | **Overlay on the game's text** (#309): the word marked where the game draws it, taps on the game's text, the popup beside the text | OFF (OCR & lookup → **Overlay on the game's text**) |
| 0.9.8 | everything of 0.9.8 (faster OCR, the new language model, the line finder fix, vertical text, trusted hooks, Yomitan's lookup rules, the adaptive reading time, the farm's hooks) | as in 0.9.8 |

With **Overlay on the game's text** ON, a trusted hook does not skip OCR in this build: every read recognizes, to place the text.

## Install

1. **Back up.** Export all cards on the Rubi app's Anki page.
2. **Close the Rubi app if it is open**, even suspended behind VitaShell (pull the top-right corner of its LiveArea page).
3. **Install the VPK.** Copy the **-debug** VPK of this release to the Vita and install it in VitaShell: Yes, then Yes to the extended-permissions notice.
4. **Update.** Open **Rubi** and choose **Update** on its Install page. It says it goes back from v0.9.8 to this test build: a test build sorts before its release, although its code is newer. Then restart the Vita yourself.

You need **Settings > HENkaku Settings > Enable Unsafe Homebrew** on.

**If a game crashes or hangs:** turn the game OFF on the Rubi app's Games page, or hold **L** while you start it.

**If the Vita does not start:** hold **L** while you turn it on (h-encore/h-encore2: open its menu, holding R as h-encore starts, and choose Exit while holding L). Then, in VitaShell, put `#` before the two `VitaJPOverlay` lines of `ur0:tai/config.txt` and `ux0:tai/config.txt`, or open **Rubi** and choose **Uninstall** (it keeps your settings and cards). Run it **once**; don't reboot to retry.

**Back to 0.9.8:** install `Rubi-0.9.8.vpk` from the v0.9.8 release and choose Update.

## The build

- **Source:** `test/combined` = `main` `892cfb9` + #318 (`adrenaline/psp-text` at `d0634fe`) + #326 (`feat/text-overlay-309` at `956833f`), with the merges' conflicts resolved; the tag is on the branch's commit that adds only this note and the step that published it.
- **Both builds:** `-debug` (the one to install: the long log, the PSP probe) and the normal build, each zip checked by `tools/e2e_package.py`, card mining on as in every test build.
- **Memory:** the game worker takes 12 KiB more (-debug) and 4 KiB more (normal) in each game that is ON, in the game's own memory; SceShell's and the kernel's pages are 0.9.8's.
