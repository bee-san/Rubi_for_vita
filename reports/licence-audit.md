# Rubi's licence: the audit for #332 and #358

**Not legal advice.** An AI agent wrote this from the licence texts, the FSF's and the European Commission's
published guidance and this repository's history (main `892cfb9`, 2026-10-27). A lawyer should confirm it before
anyone relies on it.

**Finding.** Rubi as a whole can be distributed only under the GNU GPL, version 3. The EUPL 1.2 cannot apply to it.
Rubi is a modified version of Dartv/vita-jp-overlay (GPL-3.0) and contains code from LunaTranslator and Lapis
(GPL-3.0) and from Yomichan, Yomitan and Adrenaline (GPL-3.0-or-later), and the GPL gives no permission to license a
work based on that code in any other way. It cannot be "version 3 or any later version" either, because the upstream
fork, LunaTranslator and Lapis grant version 3 alone.

## The change: `bd130a8`

- `bd130a8` "Update LICENSE" was committed on 2026-10-26 at 07:00:47Z. The author is Autumn (Bee)
  <github@skerritt.blog> (the owner's account) and the committer is GitHub, so the edit was made in GitHub's web
  editor. It has the editor's default message, no body and no pull request. It replaced the 674-line GPL v3 text with
  the EUPL v1.2's English text (153 lines). SPDX's copy of the EUPL differs from it only in spacing, a soft hyphen and
  one full stop. GitHub now reports the repository's licence as `EUPL-1.2`.
- Before that, `LICENSE` was the file from the fork's first commit (`1cfddba`, Dmytro Artamonov, "Vita JP Overlay
  0.1", 2026-10-04). It matches gnu.org's `gpl-3.0.txt` byte for byte (SHA-256 `3972dc97…b36986`), and upstream's
  `LICENSE` is the same file.
- It shipped in releases. The published `Rubi-0.9.7.zip`, `Rubi-0.9.7.vpk`, `Rubi-0.9.8.zip` and `Rubi-0.9.8.vpk`
  (the VPKs as `licenses/LICENSE`) carry the EUPL file (SHA-256 `fe1a35a4…4b7d93`, read out of the release assets).
  So do the test-build tags from v0.9.7-test.12 on and v0.9.8-test.1, while v0.9.6 and earlier have the GPL. The
  0.9.8 zip also holds the GPL text, as LunaTranslator's licence (`third_party/LunaTranslator/LICENSE`,
  `third_party/licenses/LunaTranslator-GPL-3.0.txt`).
- Nothing else changed with it. The README's badge and License section, `third_party/` and the docs said GPL-3.0
  before and after, and no other file in the tree mentions the EUPL.

## What carries which licence

| Component | Where it is in Rubi | Its licence, from its own notices |
| --- | --- | --- |
| Dartv/vita-jp-overlay, the fork's base (13 commits, all merged) | The plugins, the host tools and the build. Of the 208,240 lines in the 667 source, build and doc files outside `third_party/`, 8,949 are still those commits' lines (`git blame -w`): `kernel/scene.c` 92 %, `kernel/input.c` 77 %, `kernel/main.c` 66 %, and `core/json.c`, `core/furigana.c`, `core/arena.c` and `core/regions.c` 100 %. The rest of the tree is built on them. | GPL-3.0. Its `LICENSE` is the GPL v3 text and its README says "GPL-3.0. See LICENSE." No file says "or any later version". |
| LunaTranslator v12.1.2.12 (`third_party/LunaTranslator/`) | Its Vita hook table becomes `core/hook_luna_table.inc` and its PSP table `core/hook_luna_psp.inc`. Its hookfuncs and filters are ported by hand to C, with the patterns verbatim, in `core/hook_luna.c` (the game worker and the Rubi app). | GPL-3.0: its `LICENSE` is the GPL v3 text, with no "or later". |
| Lapis 1.7.0 (`third_party/lapis/`) | Its templates and styling become `core/lapis_notetype.inc`, which is in every `.apkg` the Rubi app exports. | GPL-3.0: its `LICENSE`, with no "or later". |
| Yomichan `2f13cf5` (`third_party/yomichan/`) | Its deinflection rules become `core/deinflect.inc`, used by the lookups. | GPL-3.0-or-later. Its `LICENSE` and source headers say "either version 3 … or (at your option) any later version". |
| Yomitan 26.10.12.0 (`third_party/yomitan/`) | The rules of `japanese-transforms.js`, ported into `core/deinflect.inc` and `core/conjugation.inc`. | GPL-3.0-or-later (the file's header). |
| Adrenaline, through AdrBubbleBooter-oss `b603ca0` | `compat/adrenaline_vsh/main.c`, the PSP bridge in the zip. | GPL-3.0-or-later (its SPDX line and the original files' headers). |
| The vita-vn-ocr runtime (pinned in `cmake/VocrOCR.cmake`, not vendored) | The OCR in the game worker. | GPL-3.0-or-later: its README says "matching Rubi". It is the owner's own repository. |
| shoui520's contributions (#106, #107, #109, 2026-10-15) and Mattias's (#257, #273, 2026-10-23) | The Rubi app, the installer and the hooks. | GPL-3.0. Both were made while `LICENSE` was the GPL. GitHub's terms, D.6: content added to a repository with a licence notice is licensed under that licence ("inbound=outbound"). |
| BearSSL (its hashes only), jsmn, acutest (tests only), taihen-parser | Vendored. | MIT. |
| libpng 1.6.58 (fetched); ncnn (optional, off) | The Rubi app; `VJO_WITH_NCNN`. | The libpng licence; BSD-3-Clause. |
| The OCR's recognizer and gate weights; the character LMs | Data files the OCR reads. | Non-commercial use only (their releases' `ATTRIBUTION.md`); the LMs are CC BY-SA 4.0. |
| The dictionaries and the VNDB names | Data. | Their own licences (`docs/plaintext-attribution.txt`, `docs/vndb-attribution.txt`). |

The permissive licences (MIT, BSD-3-Clause, libpng's) are compatible with the GPL v3, and the data keeps its own
licences, as before. The weights' non-commercial terms are a separate question that the README already answers
(they are data, not part of Rubi's code), and this audit does not change that.

## Can the EUPL 1.2 apply?

Not to Rubi as distributed:

1. **The GPL forbids it.** Under the GPL v3, a modified version must be licensed "as a whole, under this License …
   This License gives no permission to license the work in any other way" (section 5(c)). "Any attempt otherwise to
   propagate or modify it is void, and will automatically terminate your rights under this License" (section 8), and
   "You may not impose any further restrictions" (section 10). Rubi is a work based on vita-jp-overlay and contains
   LunaTranslator's and Lapis's code, verbatim or ported. A port to another language is still a modification: "the
   translated program must be covered by the same versions of the GNU GPL" (FSF FAQ, #TranslateCode).
2. **The EUPL's compatibility runs one way only: from EUPL to GPL.** Its Article 5 (the compatibility clause) lets a
   licensee distribute a derivative of an EUPL work and of a work under a Compatible Licence under that Compatible
   Licence. The GPL v2 and v3 are in its Appendix. Nothing in the EUPL works in the other direction, and an EUPL
   licensor cannot grant rights in someone else's code: under Article 6 the licensor warrants that the copyright "is
   owned by him/her or licensed to him/her and that he/she has the power and authority to grant the Licence".
3. **The European Commission says the same.** In its EUPL compatibility matrix, incorporating GPLv3 code in a
   work distributed under the EUPL is "NO (exceptions exist)": it is allowed only where the GPL code's licensor has
   published an exception that names the EUPL, while "it is legally possible to distribute the larger work under the
   GPLv3". A note in the Commission's EUPL collection adds that the compatibility clause "does not (never) allow
   changing the licence of a covered component".
4. **So does the FSF.** Its licence list says the EUPL 1.2 "has a copyleft comparable to the GPL's, and incompatible
   with it". The relicensing it describes goes from the EUPL to the GPL.
5. **Nobody but the authors can relicense their code.** If X releases V1 under the GPL and Y contributes to V2, then
   "X must get Y's permission before releasing that code under another license" (FSF FAQ, #Consider). Here that
   means the upstream author, the authors of LunaTranslator, Lapis, Yomichan, Yomitan and Adrenaline, shoui520 and
   Mattias.

To put Rubi under the EUPL, every one of those copyright holders would have to agree, or all of their code would
have to be removed and written again without it. That code is the upstream base the plugins are built on, the hooks
for LunaTranslator's 175 Vita titles, the Lapis note type, the deinflection rules and the Adrenaline bridge.

The owner can still offer his own code under the EUPL as well, for use outside Rubi. A copyright holder "can release
it under several different licenses in parallel" (FSF FAQ, #HeardOtherLicense). Rubi itself stays under the GPL.

## Why version 3, not "or any later version"

"Or any later version" applies only where a program says so (GPL v3, section 14). vita-jp-overlay says "GPL-3.0" and
ships the v3 text. LunaTranslator and Lapis ship the v3 text and say nothing more. Nobody downstream can add "or
later" on their behalf. Rubi's own new code could be "or later", but the whole could then still be conveyed only under
version 3. The FSF's compatibility matrix says this about GPLv2-only code in a "GPLv2 or later" project (its footnote
2: "the work as a whole … can only be conveyed under the terms of GPLv2"), and the same applies one version up.
Yomichan's, Yomitan's, Adrenaline's and vita-vn-ocr's "or later" code is used under version 3, which their licences
allow. If a GPL v4 ever appears, Rubi can move to it only with the consent of every version-3-only copyright holder.

## What this means for 0.9.7 and 0.9.8

The packages of 0.9.7 and 0.9.8 name the EUPL as the licence of a work that has to be under the GPL. Under section 8,
a licence that ends this way is reinstated provisionally as soon as the violation stops. It becomes permanent if no
copyright holder sends a notice within 60 days, or if a first notice is cured within 30 days. Restoring the GPL
promptly is what that asks for. A note on those two release pages would tell their users which licence applies. That
note is for the owner to add; it is not part of this change.

## Sources

- The GNU GPL v3: <https://www.gnu.org/licenses/gpl-3.0.txt> (SHA-256 `3972dc97…b36986`, this tree's `LICENSE`),
  sections 4, 5, 8, 10 and 14.
- The EUPL v1.2: `bd130a8`'s `LICENSE` and <https://interoperable-europe.ec.europa.eu/collection/eupl/eupl-text-eupl-12>,
  Articles 5 and 6 and the Appendix.
- The FSF's licence list, EUPL-1.2: <https://www.gnu.org/licenses/license-list.html#EUPL-1.2>.
- The FSF's GPL FAQ: <https://www.gnu.org/licenses/gpl-faq.html> (#WhatDoesCompatMean, #TranslateCode, #Consider,
  #HeardOtherLicense, #AllCompatibility).
- The European Commission's matrix of EUPL-compatible licences:
  <https://interoperable-europe.ec.europa.eu/collection/eupl/matrix-eupl-compatible-open-source-licences>.
- The European Commission's EUPL collection (P.-E. Schmitz), "A tenacious error about the compatibility clause":
  <https://joinup.ec.europa.eu/collection/eupl/news/tenacious-error-about-c>.
- GitHub's Terms of Service, D.6:
  <https://docs.github.com/en/site-policy/github-terms/github-terms-of-service#6-contributions-under-repository-license>.
