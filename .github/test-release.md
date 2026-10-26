**A test build for one console run: Rubi `main` at `4bc2604` (0.9.7 and #299, #300, #303), the reading statistics' adaptive reading time.**

- **This is a pre-release, not "Latest":** v0.9.7 stays the release.
- **Everything in it is merged.** The test build's own commit adds only this note and the step that published it.
- **Nothing in this build has run on a console yet.** The steps and what to report are in the test issue (linked from #303).

## What is new

- **#303: reading time on Triangle is adaptive.** `reading_time = adaptive` in `config.ini` is the new default. Each line counts the time until the next line, at most 2.5 times what your own pace in that game takes for it (2 s to 5 minutes). It works like GameSentenceMiner's cap, without sessions.
  - 0.9.7 counted a pause only if it was 15 s or less. A line read with lookups, or any line longer than that, counted nothing. On simulated reading it showed about half the real time, and characters/hour too high.
  - The first Triangle press in each game shows `counting…` once: the game's statistics are counted again from `history.csv`. The file itself is unchanged.
  - `reading_time = 15s` in `config.ini` brings back 0.9.7's rule from the Vita's next start.
- **#299: the Games page tags every game with one of Rubi's verified hook entries** as `Hook`.

## Install

1. **Back up.** Export all cards on the Rubi app's Anki page.
2. **Close the Rubi app if it is open**, even suspended behind VitaShell (pull the top-right corner of its LiveArea page).
3. **Install the VPK.** Copy **`Rubi-0.9.7-test.12-debug.vpk`** (the **-debug** one) to the Vita and install it in VitaShell: Yes, then Yes to the extended-permissions notice.
4. **Update.** Open **Rubi** and choose **Update** on its Install page. It says it goes back from v0.9.7 to this app's v0.9.7-test.12: a test build sorts before its release, although its code is newer. Then restart the Vita yourself.

You need **Settings > HENkaku Settings > Enable Unsafe Homebrew** on.

**If the Vita does not start:** hold **L** while you turn it on (h-encore/h-encore2: open its menu, holding R as h-encore starts, and choose Exit while holding L). Then, in VitaShell, put `#` before the two `VitaJPOverlay` lines of `ur0:tai/config.txt` and `ux0:tai/config.txt`, or open **Rubi** and choose **Uninstall** (it keeps your settings and cards). Run it **once**; don't reboot to retry.

**Back to 0.9.7:** install `Rubi-0.9.7.vpk` from the v0.9.7 release and choose Update.
