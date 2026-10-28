# The learned text detector: not in Rubi, for now

**Decision (2026-10-18): Rubi does not take vita-vn-ocr's learned text detector, neither as its line
finder nor as an opt-in setting.** The finders stay as they are: the textbox recovery finder and the
short-line probe in every game ([#194](https://github.com/bee-san/Rubi_for_vita/issues/194)), and the
finder refinement in PSP games only (`ocr_refine_lines = psp`, [vita-vn-ocr.md](../vita-vn-ocr.md#finder-refinement-api-20)).
Nothing in the build changes. This page records why, with the numbers, and what would change it.

The detector is [vita-vn-ocr #97](https://github.com/bee-san/vita-vn-ocr/pull/97) (issue #96, merged as
`c88c520` on 2026-10-18): a small int8 network whose output decodes into ink-tight line boxes, no lines
when a region holds no text, and a vertical flag per box. A C99 runtime runs it (`runtime/vocr_textdet.c`:
no malloc, integer only, the row-callback form Rubi's capture uses) behind a new finder flag,
`VOCR_LINES_TEXTDET`. vita-vn-ocr's own verdict is negative: in three rounds no candidate met its
pre-registered promotion rule. It suggests at most an opt-in finder, chosen per title where GRAD visibly
fails, or a text-presence gate in front of GRAD + refine, either A/B-logged on a console first. Its report
candidate is `s8_c24`: `textdet_s8_c24_w8.vtd` of the pre-release
[v0.3.1-textdet.1](https://github.com/bee-san/vita-vn-ocr/releases/tag/v0.3.1-textdet.1), sha256
`8a13dbddbc7d6305ccfaf8164ad1c4d9030437fde76eeedca68f24ac5cc47179`, not promoted.

Every figure below is vita-vn-ocr's host measurement or estimate (its `docs/textdet.md` and `eval/textdet_*`
JSONs at `c88c520`) unless it says otherwise. Nothing of the detector has run on a console.

## Why it was built

From #96: the recognizers read hand-cut lines far better than the lines the finder cuts.
- VOCR5 reads vita-test + psp-test dialogue at 1.60 / 1.13 / 0.79% CER (Small / Normal / Large) on
  hand-cut lines, and at 8.31 / 7.98 / 7.08% on finder-cut whole dialogue.
- On 443 devnative pairs, VOCR3-W20 reads the GRAD crop at 3.10% and the hand-cut ink crop at 0.51%.
- GRAD returns lines for text-free regions, so a hidden text box or a CG screen is read as junk. Neither
  finder handles vertical text.

## What it would change in Rubi

Rubi does not run plain GRAD. On Vita text it runs the recovery finder, which keeps every GRAD box and adds
rows GRAD missed; on PSP text it adds the refinement. #194 measured recovery against GRAD, with the refinement as
shipped: +0.21 to +0.23 points on vita-vn-ocr's development pool (+0.01 to +0.04 with the refinement off), +0.13
to +0.33 on vita-test and −1.22 to −1.26 on psp-test (the intervals include 0), and +0.07 on CLANNAD's referenced
lines (interval from 0.00). So the table uses GRAD for Rubi's Vita path and GRAD + refine for its PSP path.
Combining the two harnesses' differences (not measured directly) moves the Vita rows up to about 0.3 points in the
detector's favour, leaves CLANNAD worse with it (Normal about +0.12) and narrows psp-test's lead to about 4.3-5.0
points. The detector's numbers are det+refine, its best form: its lines through the same refinement.

Whole-dialogue CER, against the finder Rubi runs on that kind of text, read by VOCR5-S, -N and -L (Small, Normal
and Large; Normal is Rubi's default). Bold: the 95% interval excludes 0.

| set | Rubi's finder there | Normal: Rubi's finder → det+refine | difference, Normal (95% interval) | Small / Large |
|---|---|---|---|---|
| development, Vita frames without v27449 (65 VNs)\* | GRAD | 1.99 → 0.97% | **−1.02** (−2.10 to −0.13) | **−1.16** / −0.77 |
| development, native Vita subset (9 VNs) | GRAD | 1.07 → 0.19% | **−0.88** (−1.72 to −0.03) | −0.54 / −0.78 |
| vita-test (16 games), read once | GRAD | 6.31 → 6.40% | +0.09 (−4.29 to +4.80) | −0.01 / +0.37 |
| CLANNAD long run (2,052 dialogue lines), read once, as scored | GRAD | 0.96 → 1.15% | **+0.19** (+0.04 to +0.35) | +0.16 / **+0.29** |
| CHAOS;CHILD, the owner's console captures (145 regions), report only | GRAD | 0.368 → 0.483% | +0.115 (−0.085 to +0.348) | +0.202 / +0.137 |
| development, PSP frames without v27449 (54 VNs), 480×272 | GRAD + refine | 4.44 → 5.24% | +0.80 (−1.78 to +3.13) | +0.57 / +0.62 |
| psp-test (14 games), 480×272 frames, read once | GRAD + refine | 11.53 → 5.58% | **−5.95** (−12.50 to −0.69) | **−6.24** / **−5.59** |
| PSP text as Rubi captures it (2× bilinear) | GRAD + refine | not measured | | |

\* Derived for this page from vita-vn-ocr's stored development units (below). The other rows are vita-vn-ocr's
published figures: `docs/textdet.md`, `eval/textdet_round3_dev_20261018.json`, `eval/textdet_protected_20261018.json`
(body text, all three region variants) and `eval/textdet_own_dlg_20261018.json` (three decimals as published).

On the 66 text-free development regions, GRAD (and GRAD + refine) returns no line for 5 (8%), det+refine for
61 (92%).

What the table says:
- **On Vita text the evidence conflicts.** The development frames favour the detector and vita-test is level.
  CLANNAD is the one game Rubi's OCR has run in on a console, and CHAOS;CHILD's frames are the owner's own
  captures. On both the detector reads slightly worse, where GRAD's crops already read at 0.2-1.4%. CLANNAD's
  references are per GRAD box, and its scorer mapped the detector's lines to them more strictly than registered;
  vita-vn-ocr calls the effect most likely against the detector, and its size and sign unmeasurable. The figures
  stand as scored.
- **On PSP text the one clear win, psp-test's, is at the PSP's own 480×272.** Rubi never sees that: a PSP game
  reaches its display hook at 2× through Adrenaline's bilinear filter, where the text is about Vita-sized. At that
  scale the refinement's box-fit gain measured at 480×272 was gone: mean IoU with the hand-marked lines 0.559 →
  0.591 at 480×272, 0.608 → 0.604 at 2× ([refine-20261015.json](../benchmarks/refine-20261015.json)). The detector
  has not been scored at 2×. The development PSP frames, without v27449, read slightly worse with it (the
  intervals include 0). And PSP OCR recognition is still unverified on a console ([README](../../README.md#supported)).
- **Most of the development gain over GRAD + refine is one VN, v27449**, a translucent box over a busy scene:
  GRAD and GRAD + refine read 40-47% of its characters wrong, det+refine 6-9%. That is the "GRAD visibly
  fails" case the detector is good at. No title Rubi has run is known to be one.
- **Empty regions are its clearest strength.** In Rubi a text-free region costs recognizer calls and, when a box
  reads as something, junk in the subtitle strip and, in Automatically, in `history.csv`. How often that happens on
  a console is not measured; on CLANNAD only 25 of the recovery finder's 92 extra rows without dialogue read as
  anything (#194). 54 of the 66 empty regions lie on screenshots that the detector's synthetic training canvases also
  drew backgrounds from; on the other 12 it returns no line for 11, the same rate.
- **The vertical flag gives Rubi nothing measured.** The detector drops vertical boxes; Rubi reads horizontal text
  only, and no scored set holds vertical text.

## What it would cost

| | | |
|---|---|---|
| Weights | 5,700 B int8 (3,272 B weights, 1,980 B per-channel constants, 448 B of headers) | exact |
| Arena (det+refine) | 92,639 B at 960×544, the size Rubi captures Vita and PSP games at. The finder runs in the job's scratch before the recognizer, and every size's recognizer arena is larger (124,832 / 182,944 / 261,024 B), so the job's workspace does not grow; the blob itself needs a block or module pages (below) | exact; arithmetic |
| Code in the OCR worker | 8,360 B of text and 64 B of rodata (7,400 B of text at `-Os`), measured for this page: `vocr_textdet.c` at `c88c520`, VitaSDK gcc 15.2.0 with the runtime's Vita flags (`cmake/VocrOCR.cmake`), linked with `--gc-sections` from the loader, the arena size and the row-callback finder. On the debug worker's 78,076 B of text (the ledger's last measured figure, `49310ed`: 252 B into its last page) that is about two more pages, 8 KiB, in every game that is ON, permanently: about 6% of the worker's 128 KiB | measured; the pages an estimate |
| Time, one A9 core | per 900×150 box 34-53 ms at 333 MHz, 25-40 ms at 444 (two methods); per 960×544 screen 129-191 ms at 333 MHz. GRAD measured 23-24 ms at 883×112 on the console (v0.8.0-test.1), where the detector would take about 39 ms, 1.6×. The finder runs at the game's clock: the boost starts with the first line recognized | estimates |
| Download | the weights and their attribution, a few KB in a 61 MB VPK | negligible |
| Work and upkeep | two more pinned runtime files (1,314 lines); the blob through every model pin, including the kernel's file broker (`kernel/game_ocr.c`), the package and the one-VPK payload, or 5.7 KB more of the worker's own pages; a `config.ini` key carried in a request bit, which the kernel's request check must accept: both are kernel plugin changes (AGENTS.md rule 7); a step-log field; host tests; the memory ledger | |

Memory is not the obstacle. The obstacle is that on the text Rubi reads today it gains nothing measured, while
every game that is ON would carry its code.

## Recommendation

**No, not now.**
- On the Vita text closest to what Rubi reads (vita-test, CLANNAD, the owner's CHAOS;CHILD captures), the
  detector is level with Rubi's finder or a little worse; only the development frames favour it. So it would not
  be the default, and an opt-in that no known title needs would still cost every game that is ON about 8 KiB
  (est.) and take a kernel change.
- Its clear wins are psp-test and v27449, at a scale Rubi does not capture or on a title Rubi has not met, and
  empty regions, whose cost in Rubi has not been measured.
- The owner's steering for this work was "we need it to run on a ps vita, so smaller == better". For no measured
  gain on Rubi's own path, the smallest change is none.

## What would change it

1. **A title where the finder visibly fails on a console.** The debug build's log already shows it:
   `game vocr lines:` gives each line's crop size (two merged lines are about twice a line's height), and
   `game vocr text N:` gives the text. If such a title turns up (lines merged, lost on a translucent box, junk read
   off an empty box), check the detector offline on frames of that title. If it reads them clearly better, add the
   opt-in below and A/B it there.
2. **PSP text at Rubi's scale.** Score the detector's boxes on psp_dev as the overlay captures PSP games: 2×
   bilinear, the `--how x2-bilinear` rendering of `tools/refine_eval.py`, box fit against the hand-marked lines on
   the host, the check #94 made for the refinement. A PSP opt-in is worth a test build only if its lead over
   GRAD + refine survives at 2×, and only once a PSP game's lines are recognized on a console at all.
3. **Junk on empty screens.** If a test build's logs or `history.csv` show Automatically reading junk off CG
   screens or hidden text boxes often, try the gate: GRAD + refine as now, and no lines when the detector finds
   none. On development data that costs 0.02-0.03 points, and 61 of the 66 empty regions return nothing. It adds
   the detector's time to every job (est. 34-53 ms per 900×150 box at 333 MHz).
4. **A better detector or recognizer from vita-vn-ocr.** One that meets its promotion rule (for example after
   the fine-tune on detector crops, which its protocol deferred and then withdrew), or one that is not worse on
   CLANNAD.

**The opt-in, if it comes.** Pin vita-vn-ocr at `c88c520` or later: the ten pinned runtime files hash the same
there. Add `runtime/vocr_textdet.c` (`e5be547e33556bc170e37f3b00a2942ba0a221209b1b1cd9147dd38d69e0cda0`) and
`vocr_textdet.h` (`0082d49f49e8b05628611b1302b8ebd6cc9c88112a8b40d3bc1f2f4407b85bd7`) to `cmake/VocrOCR.cmake`.
Ship the blob with the models, pinned by its sha256 and length. Add a `config.ini` key, off by default (e.g.
`ocr_finder = recovery | textdet`), sent in a request bit. Put the finder mode in `gstep job find`'s kind, and
give the job's lines and time in the debug log. Add host tests that the job's lines equal vita-vn-ocr's runtime
on fixtures, and raise the worker's pages in the ledger with the measured build. The A/B is #94's: the same
screens with the key on and off (a backlog, a reloaded save), compared in `game vocr lines:` and the text shown.

## Reproducing the derived row

The row marked \* comes from vita-vn-ocr's stored development units: per-unit error and character counts under
`$HERD/shared/artifacts/textdet`, committed to neither repository. Run the script from a vita-vn-ocr checkout at
`c88c520` with its `.venv-ci`. It also reproduces `docs/textdet.md`'s platform-split figures against GRAD + refine
and its native-Vita figures.

<details>
<summary>The script</summary>

```python
import os
import sys
from pathlib import Path

sys.path[:0] = ["eval", "synth"]
import textdet as TD

T = Path(os.environ["HERD"]) / "shared/artifacts/textdet"
base = TD.load_baseline(T / "baseline" / "wd")
det = TD.load_det_units(T / "eval_r3" / "units", "s8_c24_seed4")
native = TD.native_vita_shots()
keeps = {"vita w/o v27449": lambda u: u["platform"] == "vita" and u["vn"] != "v27449",
         "psp w/o v27449": lambda u: u["platform"] == "psp" and u["vn"] != "v27449",
         "native_vita": lambda u: u["shot"] in native}
for m in TD.MODELS:
    d, g, r = TD.wd_units(det[m], "det"), TD.wd_units(base[m], "grad"), TD.wd_units(base[m], "refine")
    for name, k in keeps.items():
        x, y = TD.vn_diff([(d, g)], k), TD.vn_diff([(d, r)], k)  # 10,000 VN-cluster resamples, seed 0
        print(m, name, {a: TD.cer_of(u, k)["cer"] for a, u in (("grad", g), ("refine", r), ("det", d))},
              "-grad", round(x["delta"], 2), x["ci95"], "-refine", round(y["delta"], 2), y["ci95"])
```

</details>
