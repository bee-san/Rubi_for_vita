**A test build for one console run: Rubi `main` at `93647d3` (API 43), which is 0.9.7 plus everything merged since. Its new part is #325: when a hook Rubi trusts has the line, no OCR runs at all.**

- **This is a pre-release, not "Latest":** v0.9.7 stays the release.
- **Everything in it is merged.** The test build's own commit adds only this note and the step that published it.
- **Nothing in this build has run on a console yet.** The numbers below are from a computer. The steps and what to report are in the test issue.

## What is new

- **#325: Trusted hooks replace OCR** (new card on the app's OCR & lookup page, `hook_trust` in `config.ini`, ON by default).
  - A trusted hook gives the line with no OCR: no model is read, nothing is recognized, and no OCR memory is kept back, so a game short of memory still gets its lines. After eight such lines in a row, the OCR model's memory goes back to the game.
  - **Trusted:** LunaTranslator's sites and Rubi's own verified sites, from their first line. Any other hook (a string copy), once it has agreed with OCR on four different lines, the last one closely.
  - On a computer, over 8,640 hooked lines of 29 Vita games:
    - a verified site went from 0.20 OCR jobs a line to 0, and the memory Rubi keeps in the game between lines from 4.4 MiB to 0;
    - a string copy went from 0.20 OCR jobs a line to 0.10;
    - a hook one line behind the screen was never trusted.
  - The price: a screen the hook did not copy (a choice menu, the backlog) shows the hook's last line. Turn the card OFF to check every hook with OCR again, as 0.9.7 did.
- **#325: Skip OCR on screens without text** (new card, `ocr_text_gate`, OFF by default). A text box with no text in it (hidden, a CG) is not read at all. It stays off because, on a computer, it also skipped a few real lines, mostly `……` and one- or two-character ones (「え？」).
- **#325:** a computer test that a mined card carries the game's screenshot, from the game's frame to the card's picture in Anki.
- Since 0.9.7, in earlier test builds too: **#320** dictionary lookups checked against Yomitan; **#316** hooks from the farm's data bucket; **#307** faster OCR (head gate, two-level head); **#303/#305/#308** the adaptive reading time on Triangle.

## Install

1. **Back up.** Export all cards on the Rubi app's Anki page.
2. **Close the Rubi app if it is open**, even suspended behind VitaShell (pull the top-right corner of its LiveArea page).
3. **Install the VPK.** Copy **`Rubi-0.9.7-test.18-debug.vpk`** (the **-debug** one) to the Vita and install it in VitaShell: Yes, then Yes to the extended-permissions notice.
4. **Update.** Open **Rubi** and choose **Update** on its Install page. It says it goes back from v0.9.7 to this app's v0.9.7-test.18: a test build sorts before its release, although its code is newer. Then restart the Vita yourself.

You need **Settings > HENkaku Settings > Enable Unsafe Homebrew** on.

**If a game crashes or hangs:** turn the game OFF on the Rubi app's Games page, hold **L** while you start it, or set **OCR & lookup → Text source → OCR only**.

**If the Vita does not start:**
1. Hold **L** while you turn it on (h-encore/h-encore2: open its menu, holding R as h-encore starts, and choose Exit while holding L).
2. In VitaShell, put `#` before the two `VitaJPOverlay` lines of `ur0:tai/config.txt` and `ux0:tai/config.txt`, or open **Rubi** and choose **Uninstall** (it keeps your settings and cards).

Run it **once**; don't reboot to retry.

**Back to 0.9.7:** install `Rubi-0.9.7.vpk` from the v0.9.7 release and choose Update.
