**A test build for one console run: Rubi `main` at `241e09f`, the first VOCR6 pieces of Rubi's OCR (vita-vn-ocr #125): faster reads (#307, #328), a new language model (#306) and the line finder fix (#330).**

- **This is a pre-release, not "Latest":** v0.9.7 stays the release.
- **Everything in it is merged.** The test build's own commit adds only this note and the step that published it.
- **Nothing in this build has run on a console yet.** The steps and what to report are in the test issue (linked from #330 and vita-vn-ocr #125).

## What is new

- **Faster OCR** (#307, #328; `ocr_head_gate = on`): the recognizers skip their output layer on the frames their head gate calls blank, and Normal and Large compute their rare characters only on the frames that may need one. Estimated time per 3-line box at 333 MHz: 0.63 / 0.92 / 2.08 s → 0.51 / 0.72 / 1.82 s (Small / Normal / Large; estimates). `ocr_head_gate = off` reads exactly as 0.9.7.
- **A new language model** (#306; `ocr_lm = 1000k`, the default, and `4000k`): built with the text of the owner's hooked captures besides the open corpora (vita-vn-ocr #127, release v0.3.7-lm6.2). On development data it reads the hooked titles' held-out pages better and unseen games about the same; counters such as 九ヶ月 are a little weaker. `ocr_lm = 2000k` or `500k` keeps 0.9.7's LMs.
- **The line finder fix** (#330; `ocr_line_fix = on`): a line with a full-width space (after ！ or ？) stays one line. `ocr_line_fix = off` is 0.9.7's finder.
- Together, through Rubi's page path on development data (finder → VOCR5-N → LM; measured on a computer): 0.9.7 → this build, whole-dialogue error: held-out pages of the hooked games 3.875 % → 1.636 %, 93 other games 3.036 % → 2.441 % (the finder fix alone: 3.292 % / 2.340 %; VOCR5-N with VOCR5 v0.3.0's blobs and no head gate, measured on a computer).
- Also merged since 0.9.7: #335 vertical text (`ocr_vertical`), #325 / #338 trusted hooks answer without OCR, #320 / #322 / #331 Yomitan's lookup rules, #316 two more hooked games (ALIA's CARNIVAL!, 真 流行り神), #303 / #305 / #308 the adaptive reading time, #299 the Games page's Hook tag, #337 (docs and tools)

## Install

1. **Back up.** Export all cards on the Rubi app's Anki page.
2. **Close the Rubi app if it is open**, even suspended behind VitaShell (pull the top-right corner of its LiveArea page).
3. **Install the VPK.** Copy **`Rubi-0.9.7-test.21-debug.vpk`** (the **-debug** one) to the Vita and install it in VitaShell: Yes, then Yes to the extended-permissions notice.
4. **Update.** Open **Rubi** and choose **Update** on its Install page, then restart the Vita yourself.

You need **Settings > HENkaku Settings > Enable Unsafe Homebrew** on.

**If the Vita does not start:** hold **L** while you turn it on (h-encore/h-encore2: open its menu, holding R as h-encore starts, and choose Exit while holding L). Then, in VitaShell, put `#` before the two `VitaJPOverlay` lines of `ur0:tai/config.txt` and `ux0:tai/config.txt`, or open **Rubi** and choose **Uninstall** (it keeps your settings and cards). Run it **once**; don't reboot to retry.

**Back to 0.9.7:** install `Rubi-0.9.7.vpk` from the v0.9.7 release and choose Update.
