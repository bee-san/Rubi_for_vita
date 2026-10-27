**One combined test build for one console run: Rubi `main` at `db1550c` plus every open draft PR that waits for the owner's console test.** It replaces the separate test builds and test issues: the steps and what to send back are in the one test issue, #351 ("Test build: everything waiting for a console test"), which lists every build.

- **This is a pre-release, not "Latest":** v0.9.8 stays the release.
- **The drafts in it are not merged.** The scratch branch `test/combined` merges them for this test only (their conflicts are resolved there; API 55). Each new test build is `v0.9.8-test.N` from the same branch.
- **Nothing new in it has run on a console yet.**

## What is in it

| PR | What | Default |
| --- | --- | --- |
| #318 (draft) | **PSP text in Adrenaline**: Memories Off 6 ～T-wave～ and Memories Off ～それから～ read from PSP memory, read-only, shown once OCR agrees; the PSP probe's fix (#248), its `psp_probe.log` format 4 in the -debug build | only with **Text source → Every game** |
| #326 (draft) | **Overlay on the game's text** (#309): the word marked where the game draws it, taps on the game's text, the popup beside the text | OFF (OCR & lookup → **Overlay on the game's text**) |
| #355 (draft) | **Pause the game to read** (#348): each reading copies the text box from one frame at once, so a spinning or blinking icon in it can't stop the read | OFF (OCR & lookup → **Pause the game to read**, the last card) |
| #359 (draft) | **Text past an animated icon** (#353): when the region keeps changing, the reading watches the found lines' own pixels and reads them anyway at the limit (in this build such a job finds its lines with Rubi's finder, the watch's own, not the spotter) | always on |
| `main` | everything of v0.9.8-test.4's `main` (`686f800`: VOCR6 for Normal and Large, the Stats page, the suggested text box, the wait hint, SC3 text, TIME TRAVELERS' hook, …), and since: **the text spotter finds the lines by default** (`ocr_finder = spotter`, #396, #412), **Vertical text ON reads a horizontal text box as rows** (#385, #411; #379), **a game's own line finder** on its page in the Rubi app (#395, #406; #333), lookups: what the Yomitan check left (#386, #398, #402; #321), the Rubi app's **Look up** tab (#391, #405; #313) and **Cards** page (#392; #315), **duplicates and known words** at export (#390, #393, #404; #314), **richer Anki cards**: card type, game tag, sentence readings (#407; #311), the Stats page's audit (#403), hook streams' audits (#400, #409; #366), the hooks' audits (#397, #401, #410: ALIA's CARNIVAL!'s escapes, CP932's 0xA0, 真 流行り神 only at 0x81000000, the trust bit with Trusted hooks OFF), #382/#387's audit (#399), the roadmap (#408) | as on `main` |

With **Overlay on the game's text** ON, a trusted hook does not skip OCR in this build: every read recognizes, to place the text. Draft #318's stop after three changed-screen tries is not in this build: `main`'s #364 replaces it (the tries go on, with the hint). Once a reading watches its lines (#359), it finds them with Rubi's finder (GRAD), which the watch compares them with; every other reading finds them with `main`'s spotter (the debug log's `spot` step of a watched job: `rc=0xFFFFFF88`).

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

- **Source:** `test/combined` = `main` `db1550c` + #318 (`adrenaline/psp-text` at `d0634fe`) + #326 (`feat/text-overlay-309` at `956833f`) + #355 (`feat/pause-348` at `9e44739`) + #359 (at `472f8ae`), with the merges' conflicts resolved; the tag is on the branch's head, whose last commit adds only this note and the build's version.
- **Both builds:** `-debug` (the one to install: the long log, the PSP probe) and the normal build, each zip checked by `tools/e2e_package.py`, card mining on as in every test build.
- **Memory:** the game worker's pages are measured on the builds (the game's own memory): 237,568 B (-debug) and 225,280 B (normal), 20 KiB and 16 KiB more than `main`'s; SceShell's (176,128 / 163,840 B) and the kernel's pages are `main`'s. With **Pause the game to read** ON, a reading also holds one block of the text box's size (at most 512 KiB) while it runs, only when it fits.
