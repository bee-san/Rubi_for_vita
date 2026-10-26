**A test build for one console run: Rubi `main` at `3388f7a` (API 49), which is 0.9.7 plus everything merged since. Its new part is #338: a trusted hook's line is ready to look up as soon as the game writes it.**

- **This is a pre-release, not "Latest":** v0.9.7 stays the release.
- **Everything in it is merged.** The test build's own commit adds only this note and the step that published it.
- **It replaces 0.9.7-test.18** (#325's build, not reported installed yet) and has 0.9.7-test.19's vertical text (#335) too: test this one instead; the steps are in #329.
- **Nothing in this build has run on a console yet.** The numbers below are from a computer. The steps and what to report are in the test issue.

## What is new

- **#338: Trusted hooks without waiting** (with **Trusted hooks replace OCR** ON, the default). Like a hook on a computer: no OCR, no screen capture, no waiting for the screen to settle or the line to finish typing.
  - Rubi checks the hook every 16 ms (was 100 ms) and takes each trusted line at once, also while the last one still types, and from a site's first line on. The same text again is not a new line.
  - With the overlay open, each new line replaces the text as it starts; a line that grows keeps your cursor.
  - The reading history keeps one record for a line the game gives in parts.
  - A trusted hook stays trusted when the game starts again (no OCR check first).
  - On a computer, over 8,895 hooked lines of 30 Vita games, from the game's copy of a line to the line with its first word looked up (the lookup counted as the console's 70 ms): **187 → 78 ms**; clicking through, a line every 0.5 s: 8,832 of the lines were never read before, now **every one in 80 ms**; a verified site's first line **1.8 s → 78 ms**; the open overlay's new text **117 → 8 ms**.
- **#338: #325's audit fixes.** Changing **Trusted hooks replace OCR** now applies at the next open even on the same screen; a rare failure to give the OCR memory back no longer loses lines; the trusted-hook numbers redone with the current OCR, on a computer: a string copy now runs OCR on 0.08 of the lines, against 0.19 with every hook checked (it was trusted in 27 of 29 games' sessions, and never wrong there).
- New since test.18: **#335** vertical text (columns read right to left, Automatic by default; test.19's); **#306** VOCR6's character LMs; **#328** #307's audit fixes; **#331** the Yomitan check's audit.
- Since 0.9.7, in earlier test builds too: **#325** trusted hooks replace OCR, skip OCR on screens without text; **#320** dictionary lookups checked against Yomitan; **#316** hooks from the farm's data bucket; **#307** faster OCR (head gate, two-level head).

## Install

1. **Back up.** Export all cards on the Rubi app's Anki page.
2. **Close the Rubi app if it is open**, even suspended behind VitaShell (pull the top-right corner of its LiveArea page).
3. **Install the VPK.** Copy **`Rubi-0.9.7-test.20-debug.vpk`** (the **-debug** one) to the Vita and install it in VitaShell: Yes, then Yes to the extended-permissions notice.
4. **Update.** Open **Rubi** and choose **Update** on its Install page. It says it goes back from v0.9.7 to this app's v0.9.7-test.20: a test build sorts before its release, although its code is newer. Then restart the Vita yourself.

You need **Settings > HENkaku Settings > Enable Unsafe Homebrew** on.

**If a game crashes or hangs:** turn the game OFF on the Rubi app's Games page, hold **L** while you start it, or set **OCR & lookup → Text source → OCR only**.

**If the Vita does not start:**
1. Hold **L** while you turn it on (h-encore/h-encore2: open its menu, holding R as h-encore starts, and choose Exit while holding L).
2. In VitaShell, put `#` before the two `VitaJPOverlay` lines of `ur0:tai/config.txt` and `ux0:tai/config.txt`, or open **Rubi** and choose **Uninstall** (it keeps your settings and cards).

Run it **once**; don't reboot to retry.

**Back to 0.9.7:** install `Rubi-0.9.7.vpk` from the v0.9.7 release and choose Update.
