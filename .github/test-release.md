**A test build for one console run: Rubi `main` at `b6c715c` (API 47), which is 0.9.7 plus everything merged since. Its new part is #335: vertical Japanese text, read in columns, right to left.**

- **This is a pre-release, not "Latest":** v0.9.7 stays the release.
- **Everything in it is merged.** The test build's own commit adds only this note and the step that published it.
- **Nothing in this build has run on a console yet.** The numbers below are from a computer. The steps and what to report are in the test issue.

## What is new

- **#335: Vertical text** (a new last card on the app's OCR & lookup page, `ocr_vertical` in `config.ini`, **Automatic** by default).
  - Japanese set in columns (a vertical dialogue box, a manga in NOBORU) is read column by column, right to left, with vita-vn-ocr's vertical model (VOCR5-V, its own 0.8 MB file, read only for vertical text).
  - **Automatic:** where the text box you drew holds vertical text, Rubi reads its columns; everywhere else it reads rows exactly as before. **ON:** columns in every game. **OFF:** rows only, as 0.9.7.
  - **Draw the text box (□) around the vertical text.** On a whole screen Rubi keeps reading rows.
  - On a computer, through Rubi's own job: Yunohana SpRING! (PS Vita), 863 hooked pages that the vertical model never trained on:
    - a box drawn around the columns: 4.5 % of characters wrong (rows: 83 %);
    - one box drawn once for the game's text box: 14 % (rows: 84 %).
  - Horizontal games: on 101,651 boxes and screens of 44 hooked games, Automatic never switched to columns, and on 3,080 of them every job's text was main's. The game worker takes one more 4 KiB page in each game that is ON; SceShell's and the kernel's pages are unchanged.
- **#306: VOCR6's character LMs** for the 1 MB (the default) and 4 MB tiers, built with the hooked games' text.
- Since 0.9.7, in earlier test builds too: **#325** trusted hooks replace OCR; **#320** dictionary lookups checked against Yomitan; **#316** hooks from the farm's data bucket; **#307/#328** faster OCR (head gate, two-level head) and its audit fixes; **#303/#305/#308** the adaptive reading time on Triangle.

## Install

1. **Back up.** Export all cards on the Rubi app's Anki page.
2. **Close the Rubi app if it is open**, even suspended behind VitaShell (pull the top-right corner of its LiveArea page).
3. **Install the VPK.** Copy **`Rubi-0.9.7-test.19-debug.vpk`** (the **-debug** one) to the Vita and install it in VitaShell: Yes, then Yes to the extended-permissions notice.
4. **Update.** Open **Rubi** and choose **Update** on its Install page. It says it goes back from v0.9.7 to this app's v0.9.7-test.19: a test build sorts before its release, although its code is newer. Then restart the Vita yourself.

You need **Settings > HENkaku Settings > Enable Unsafe Homebrew** on.

**If a game crashes or hangs:** turn the game OFF on the Rubi app's Games page, hold **L** while you start it, or set **OCR & lookup → Vertical text → OFF**.

**If the Vita does not start:**
1. Hold **L** while you turn it on (h-encore/h-encore2: open its menu, holding R as h-encore starts, and choose Exit while holding L).
2. In VitaShell, put `#` before the two `VitaJPOverlay` lines of `ur0:tai/config.txt` and `ux0:tai/config.txt`, or open **Rubi** and choose **Uninstall** (it keeps your settings and cards).

Run it **once**; don't reboot to retry.

**Back to 0.9.7:** install `Rubi-0.9.7.vpk` from the v0.9.7 release and choose Update.
