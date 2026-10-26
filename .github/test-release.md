**A test build for one console run: Rubi `main` at `627a673` (API 42): 0.9.7 plus #299, #300, #301, #303, #305, #307, #308 and #316.**

#316 adds native text hooks from the hook farm's data bucket:
- **New:** ALIA's CARNIVAL! サクラメント (`PCSG00628`) and 真 流行り神 (`PCSG00380`).
- **Text fixes:** BLACK WOLVES SAGA (`PCSG00935`) keeps its line breaks, 忠臣蔵46+1 V (`PCSG00540`) keeps its blanks, and the PROTOTYPE games (`PCSG00007`, `PCSG00206`, `PCSG00620`) show `・` between katakana words.
- **Version check:** an entry is refused in another game version than the farm recorded.

- **This is a pre-release, not "Latest":** v0.9.7 stays the release.
- **Everything in it is merged.** The test build's own commit adds only this note and the step that published it.
- **Nothing in this build has run on a console yet.** The hooks were proven only in the Vita3K emulator, which says nothing about taiHEN or memory on a real Vita. The steps and what to report are in the test issue.

## What is new

- **#316: verified hook entries from the farm's data bucket** (Vita3K only). Rubi's text equals the farm's on 12,465 of the bucket's 12,503 single-hit pages for the 14 games with entries; the other 38 differ only by a default player name.
  - 真 流行り神 reads the game's page record of character cells, which is new in Rubi.
  - Its site is in the per-frame draw, so the hook runs every frame.
- **#307: OCR reads faster** with vita-vn-ocr's head gate and two-level head (API 42; `ocr_head_gate` in `config.ini`).
- **#303/#305: the adaptive reading time** on Triangle (as in test.12, with #305's fix).

## Install

1. **Back up.** Export all cards on the Rubi app's Anki page.
2. **Close the Rubi app if it is open**, even suspended behind VitaShell (pull the top-right corner of its LiveArea page).
3. **Install the VPK.** Copy **`Rubi-0.9.7-test.14-debug.vpk`** (the **-debug** one) to the Vita and install it in VitaShell: Yes, then Yes to the extended-permissions notice.
4. **Update.** Open **Rubi** and choose **Update** on its Install page. It says it goes back from v0.9.7 to this app's v0.9.7-test.14: a test build sorts before its release, although its code is newer. Then restart the Vita yourself.

You need **Settings > HENkaku Settings > Enable Unsafe Homebrew** on.

**If a game crashes or hangs with its hook:** turn the game OFF on the Rubi app's Games page, hold **L** while you start it, or set **OCR & lookup → Text source → OCR only**.

**If the Vita does not start:**
1. Hold **L** while you turn it on (h-encore/h-encore2: open its menu, holding R as h-encore starts, and choose Exit while holding L).
2. In VitaShell, put `#` before the two `VitaJPOverlay` lines of `ur0:tai/config.txt` and `ux0:tai/config.txt`, or open **Rubi** and choose **Uninstall** (it keeps your settings and cards).

Run it **once**; don't reboot to retry.

**Back to 0.9.7:** install `Rubi-0.9.7.vpk` from the v0.9.7 release and choose Update.
