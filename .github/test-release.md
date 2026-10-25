**A test build for one console run: Rubi `main` (0.9.5, with #286's quick hook reads) plus the pull request #287, the running game's character names.**

- **This is a pre-release, not "Latest":** v0.9.5 stays the release.
- **#287 is not merged.** This build is its head, `feat/per-game-names` at `d6c6e07` (`main` at `d29595f` with #287's commits), and nothing else unmerged. The test build's own commit adds only this note and the step that published it.
- The test steps and what to report are in the test issue (linked from #287).
- **Nothing in this build has run on a console yet.**

## What is new

- **#287: the running game's names come first.** In a game Rubi knows by its Vita title ID (or, from an Adrenaline bubble, its PSP disc ID), that game's VNDB character names are searched before the other dictionaries. In CLANNAD (`PCSG00415`), 渚 is Furukawa Nagisa (the popup's source says `[CLANNAD]`), and the word "beach" is the next L/R page. Games without an ID behave exactly as in 0.9.5. A new file, `ux0:data/VitaJPOverlay/dictionaries/vndb-games.vjgames` (9,705,406 B), is installed beside `vndb-names.vjdict`.
- **#286 (merged after 0.9.5): a hooked line is read as soon as the game copies it.** With **Read text** on **Automatically**, L + R as a hooked line finishes usually opens on it, its first word already looked up.

## Install

1. **Back up.** Export all cards on the Rubi app's Anki page.
2. **Close the Rubi app if it is open**, even suspended behind VitaShell (pull the top-right corner of its LiveArea page).
3. **Install the VPK.** Copy **`Rubi-0.9.5-test.11-debug.vpk`** (the **-debug** one) to the Vita and install it in VitaShell: Yes, then Yes to the extended-permissions notice.
4. **Update.** Open **Rubi**, choose **Update** on its Install page (it says `Go back from v0.9.5 to this app's v0.9.5-test.11`: a test build sorts before its release, although its code is newer), and restart the Vita yourself.

You need **Settings > HENkaku Settings > Enable Unsafe Homebrew** on, and about 20 MB more free on ux0 than 0.9.5 needed (an estimate: the per-game pack, unpacked by VitaShell and then copied by the Install).

**If the Vita does not start:** hold **L** while you turn it on (h-encore/h-encore2: open its menu, holding R as h-encore starts, and choose Exit while holding L). Then, in VitaShell, put `#` before the two `VitaJPOverlay` lines of `ur0:tai/config.txt` and `ux0:tai/config.txt`, or open **Rubi** and choose **Uninstall** (it keeps your settings and cards).

**Back to 0.9.5:** install `Rubi-0.9.5.vpk` from [v0.9.5](https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.9.5) and choose **Update**. The extra file stays and is not read.

## Files

`Rubi-0.9.5-test.11-debug.vpk` (install this one), `Rubi-0.9.5-test.11-debug.zip` (the same build for a computer: `--collect`), and the normal build's `Rubi-0.9.5-test.11.vpk` and `.zip` (not for this run). `SHA256SUMS` has all four. Packaged by this tag's own CI run with `tools/package_variants.py --set anki_mining=on`, and each zip passed `tools/e2e_package.py`.
