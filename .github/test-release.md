**A test build for one console run: Rubi `main` at `23921ad` (API 42) plus the unmerged draft PR #326, the overlay on the game's text (#309).**

- **This is a pre-release, not "Latest":** v0.9.7 stays the release.
- **#326 stays an unmerged draft until you have tested it.** The test build's own commit adds only this note and the step that published it.
- **The overlay on the game's text is OFF unless you turn it on** (Rubi app → OCR & lookup → **Overlay on the game's text**). Off, this build behaves as `main`.
- **Nothing in this build has run on a console yet.** The steps and what to send back are in the test issue (linked from #326).

## What is new (#326, #309)

- **The overlay on the game's text** (API 46, testing): as GameSentenceMiner's overlay does on a PC, the word at the cursor is marked where the game draws it, the game's lines are underlined, a tap on a character looks its word up, Up and Down go to the line above or below, and the dictionary popup is a band beside the text instead of the full screen. Enter still saves the word as an Anki card.
- **Where the places come from:** the OCR's own CTC frames (vita-vn-ocr #114). On the hooked corpus's held-out pages, a tap at a glyph's middle selected the right character for 97.3% of 43,294 characters, with OCR's text and with a hook's (measured on a computer).
- **The cost when it is on:** every read recognizes (also in a game whose hook answers without OCR) and skips the line cache. Off, nothing of it runs. The game worker's code is one page (4,096 B) larger in each game that is ON; SceShell's pages are unchanged.

## Install

1. **Back up.** Export all cards on the Rubi app's Anki page.
2. **Close the Rubi app if it is open**, even suspended behind VitaShell (pull the top-right corner of its LiveArea page).
3. **Install the VPK.** Copy **`Rubi-0.9.7-test.17-debug.vpk`** (the **-debug** one) to the Vita and install it in VitaShell: Yes, then Yes to the extended-permissions notice.
4. **Update.** Open **Rubi** and choose **Update** on its Install page. It says it goes back from v0.9.7 to this app's v0.9.7-test.17: a test build sorts before its release, although its code is newer. Then restart the Vita yourself.

You need **Settings > HENkaku Settings > Enable Unsafe Homebrew** on.

**If the overlay misbehaves** (marks in the wrong place, a stutter, a crash): turn **Overlay on the game's text** OFF in the Rubi app, or put `overlay_on_text = off` in `ux0:data/VitaJPOverlay/config.ini`.

**If a game crashes or hangs:** turn the game OFF on the Rubi app's Games page, or hold **L** while you start it.

**If the Vita does not start:**
1. Hold **L** while you turn it on (h-encore/h-encore2: open its menu, holding R as h-encore starts, and choose Exit while holding L).
2. In VitaShell, put `#` before the two `VitaJPOverlay` lines of `ur0:tai/config.txt` and `ux0:tai/config.txt`, or open **Rubi** and choose **Uninstall** (it keeps your settings and cards).

Run it **once**; don't reboot to retry.

**Back to 0.9.7:** install `Rubi-0.9.7.vpk` from the v0.9.7 release and choose Update.

## The build

- **Source:** draft PR **#326** (`feat/text-overlay-309` at `ca3652c`) on `main` at `23921ad`. The tag `v0.9.7-test.17` is on the test branch's commit, which adds only the release step and this note.
- **Both builds:** `Rubi-0.9.7-test.17-debug.*` (the one to install for this run: the long log) and `Rubi-0.9.7-test.17.*` (the normal build, the main download's kind), each zip checked by `tools/e2e_package.py`, card mining on as in every test build.
