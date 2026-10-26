**A test build for one console run: Rubi `main` at `e2100e0` (API 42), which is 0.9.7 plus everything merged since. Its new part is #320: the dictionary lookups checked against Yomitan.**

On 5,000 lines of hooked visual-novel text, the popup's entry differed from Yomitan's first entry on 69.2 of every 1,000 taps before this change and on 65.8 after it ([the report](https://github.com/bee-san/Rubi_for_vita/blob/main/reports/yomitan-check.md)).

- **Spoken forms are found.** しておく, 変わらん, 構わん, しちゃ駄目, 戻らなきゃ, 認めざる, 返さねば, しやがって, 聞きやしねえ, いたしまして, 知らねー and できねー now show their dictionary form, with a conjugation line (`Conjugation: -te → -toku`).
- **Kana verbs show the usual word.** わかった shows 分かる, not 分かつ.
- **A word with な after it is not a verb any more.** 失礼な shows 失礼 (しつれい), not a verb 失礼 read しつらい. するな highlights する.

- **This is a pre-release, not "Latest":** v0.9.7 stays the release.
- **Everything in it is merged.** The test build's own commit adds only this note and the step that published it.
- **Nothing in this build has run on a console yet.** The lookups were checked on a computer, with the same code and dictionary. Memory: the Shell's pages are unchanged and it is 856 B smaller. The steps and what to report are in the test issue.

## What is new

- **#320: lookups checked against Yomitan.** It adds Yomitan's rules for spoken forms, a ranked choice between kana verbs, and removes the old bare `な` rule.
- Since 0.9.7, and in test.14 too: **#316** native text hooks from the farm's data bucket (ALIA's CARNIVAL!, 真 流行り神, three text fixes); **#307** faster OCR (head gate, two-level head; `ocr_head_gate` in `config.ini`); **#303/#305/#308** the adaptive reading time on Triangle.

## Install

1. **Back up.** Export all cards on the Rubi app's Anki page.
2. **Close the Rubi app if it is open**, even suspended behind VitaShell (pull the top-right corner of its LiveArea page).
3. **Install the VPK.** Copy **`Rubi-0.9.7-test.16-debug.vpk`** (the **-debug** one) to the Vita and install it in VitaShell: Yes, then Yes to the extended-permissions notice.
4. **Update.** Open **Rubi** and choose **Update** on its Install page. It says it goes back from v0.9.7 to this app's v0.9.7-test.16: a test build sorts before its release, although its code is newer. Then restart the Vita yourself.

You need **Settings > HENkaku Settings > Enable Unsafe Homebrew** on.

**If a game crashes or hangs:** turn the game OFF on the Rubi app's Games page, hold **L** while you start it, or set **OCR & lookup → Text source → OCR only**.

**If the Vita does not start:**
1. Hold **L** while you turn it on (h-encore/h-encore2: open its menu, holding R as h-encore starts, and choose Exit while holding L).
2. In VitaShell, put `#` before the two `VitaJPOverlay` lines of `ur0:tai/config.txt` and `ux0:tai/config.txt`, or open **Rubi** and choose **Uninstall** (it keeps your settings and cards).

Run it **once**; don't reboot to retry.

**Back to 0.9.7:** install `Rubi-0.9.7.vpk` from the v0.9.7 release and choose Update.
