**A test build for one console run: Rubi `main` at `627a673` (API 42) plus the unmerged draft PR #318, PSP text in Adrenaline.**

- **This is a pre-release, not "Latest":** v0.9.7 stays the release.
- **#318 stays an unmerged draft until you have tested it.** The test build's own commit adds only this note and the step that published it.
- **Nothing in this build has run on a console yet.** The PSP text was proven only in the PPSSPP emulator (the farm). The steps and what to send back are in the test issue.

## What is new (#318)

- **PSP text in Adrenaline, with Text source → Every game only** (off for everyone else). Rubi reads the dialogue page of two PSP games straight from PSP memory, read-only: Memories Off 6 ～T-wave～ (`NPJH50382` / `ULJM05467`) and Memories Off ～それから～ (`NPJH50132` / `ULJM05350`).
  - It reads only while the game's ID and the code at the farm's hook site are exactly the expected ones.
  - OCR must agree with the text before it is shown.
  - Nothing is hooked or patched in the PSP game, and no PSP plugin or Adrenaline setting is added.
- **The PSP probe (this -debug build) fixed:** in v0.9.1-test.8 it stopped before reading anything, because PSP memory has no block UID on the Vita (`0x80024501`, #248). It now reads, writes `psp_probe.log` format 4, and runs with Automatic or Every game.
- **The overlay stops waiting on a screen that keeps changing:** after three tries it says so.
- Everything of `main` at `627a673` (test.14's contents): the farm bucket's Vita hooks (#316), the faster OCR (#307) and the adaptive reading time (#303, #305).

## Install

1. **Back up.** Export all cards on the Rubi app's Anki page.
2. **Close the Rubi app if it is open**, even suspended behind VitaShell (pull the top-right corner of its LiveArea page).
3. **Install the VPK.** Copy **`Rubi-0.9.7-test.15-debug.vpk`** (the **-debug** one) to the Vita and install it in VitaShell: Yes, then Yes to the extended-permissions notice.
4. **Update.** Open **Rubi** and choose **Update** on its Install page. It says it goes back from v0.9.7 to this app's v0.9.7-test.15: a test build sorts before its release, although its code is newer. Then restart the Vita yourself.

You need **Settings > HENkaku Settings > Enable Unsafe Homebrew** on.

**If a PSP game crashes or hangs:** choose **OCR & lookup → Text source → OCR only** (it turns PSP text and the probe off), turn Adrenaline (and the bubble) OFF on the Rubi app's Games page, or hold **L** while you start Adrenaline or the bubble.

**If the Vita does not start:**
1. Hold **L** while you turn it on (h-encore/h-encore2: open its menu, holding R as h-encore starts, and choose Exit while holding L).
2. In VitaShell, put `#` before the two `VitaJPOverlay` lines of `ur0:tai/config.txt` and `ux0:tai/config.txt`, or open **Rubi** and choose **Uninstall** (it keeps your settings and cards).

Run it **once**; don't reboot to retry.

**Back to 0.9.7:** install `Rubi-0.9.7.vpk` from the v0.9.7 release and choose Update.
