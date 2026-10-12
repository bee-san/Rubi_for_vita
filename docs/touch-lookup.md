# Touch selection and highlighting

Settings → OCR & lookup → Highlight chooses **Whole word** (default) or
**Character**. Local dictionaries show one entry at a time; with
**Dictionaries shown** above 1, Up/Down pages to other dictionaries' entries
at the same character ([details](local-dictionaries.md#dictionaries-shown)).
Each page highlights its own match, and Right skips the one shown. In Whole
word mode, Right skips the current matched source span and Left retraces up to
16 recent forward selections. Without a match or previous selection, movement
falls back to one Unicode character. Character mode always steps one character.
Whole word uses the matched source span, including an inflected ending, rather
than the dictionary's normalized spelling. While a lookup runs or has no result,
it highlights the selected character instead. Changing the preference preserves
the recognized text; a completed selected lookup is reused when opening again.

Automatic OCR prepares the first local match before the popup is opened.
Reopening unchanged dialogue reuses a completed, current result without more
dictionary reads. Changed text, scene, region, dictionary or cursor excludes
that cache. On-press OCR still requires recognition and an initial lookup when
opening; new selections perform one asynchronous lookup each. The existing
single dynamic 64 KiB result workspace is retained; there is no sentence-wide
definition cache or static workspace reservation. Navigation stores only 16
byte offsets, and a tap or a new sentence clears that history.

In v0.7.10, a highlight change explicitly invalidates the header drawing after
updating its color styles. The applied range is committed only after the native
calls succeed; unavailable layout or failed updates get at most six frame
retries. A partial failure clears the header's color styles before retrying, so
a newer cursor or mode cannot leave an old word yellow. The header string and
scroll position are retained.

Tap a character in the recognized sentence at the top of the open overlay to
look up the word starting there. This selects the overlay's recognized header,
not an unrecognized glyph in the game's framebuffer. Japanese-only selection
applies equally to arrows and touch: a tap on Latin text, digits, punctuation or
Hangul has no effect when it is ON. A held finger produces one request; it must
be released before another tap. Touch already held while opening is ignored.

The overlay keeps Shell's front touch region active through normal and crop
modes and releases it on close. Its existing game input block still filters
game touch calls. Entering or leaving the crop editor does not briefly hand
front touches back to the game. Failed region activation is not marked active.

Hit testing allocates no memory and runs only on a new touch. It uses the
header's measured PAF character starts, line count, content height and current
scroll. The header has one font size, so the line pitch is measured height
divided by line count, as in existing header scrolling. The trailing cell on a
line is bounded by the configured Japanese point width; it is not a separately
measured glyph bitmap. Blank trailing areas and clipped header rows have no hit.
CR omission, UTF-16 surrogate pairs, UTF-8 source offsets and wrapped lines are
handled without borrowing dictionary arena pointers outside the view lock.
Undocumented `LineInfo.frame` fields and `GetNextDownHit` are not used.

Host checks exercise proportional advances, wrapping, scrolling/clipping, blank
space, Japanese-only taps, CR/newline handling, supplementary Han and unavailable
layout. Existing worker tests verify that changing highlight mode preserves OCR
and that rapid selected lookups cannot publish stale definitions. Config tests
cover both modes and invalid values; transactional preference tests preserve
unrelated config bytes and exercise save/restore failures.

The header recoloring tests exercise the actual update helper against a text
double with cached drawing. They cover character-to-word expansion, character
mode, stale-color cleanup, failed layout/style/invalidation calls, invalid
ranges, and inflected source spans after CR and a UTF-16 surrogate pair.
These tests establish the explicit refresh contract, not native PAF behavior.

These checks validate the portable helper and worker/config integration. Native
PAF font geometry, finger hit accuracy and game touch routing require hardware
validation and are recorded separately from host results.

## Installation checkpoint

After the v0.7.9 checkpoint, the user reported that Whole word still showed one
highlighted character while a definition was displayed
([issue #41](https://github.com/bee-san/vita-jp-overlay/issues/41)). They later
clarified that the word appears when lookup finishes and requested word jumps.
The v0.7.10 changes prepare/reuse the first result and navigate matched spans;
they have not been deployed to or visually
verified on the physical Vita.

[PR #38](https://github.com/bee-san/vita-jp-overlay/pull/38) merged after checks
at `ee796f1ea683d84a668bbc52961bf146a9f9986c`: 22 host suites, native build/gates
and a separate ASan/UBSan run of the actual `text_hit` helper's three scenarios.
The audited v0.7.9 release bundle has 60 manifested payloads plus one manifest.
Installed Shell and Settings executable readbacks match that candidate, and
Settings is registered and confirmed in the native foreground. The uncertain
promotion status is preserved separately from that reconciliation.

Four actual native Settings actions passed complete config readbacks: touch
**Character** → Left to **Whole word** → Right to **Character** → touch
**Whole word**. VOCR, Japanese-only ON and all unrelated bytes were retained;
the final configuration selects whole-word highlighting. All six independently
downloaded public release assets match their audited candidates and checksums.

Settings-card taps are separate from recognized-header taps in a game.
Fresh native inventory confirms the installed Shell plugin loaded. In-game
touch/highlight tests remain pending; no native finger-accuracy, glyph-bound,
word-span or game-routing result is claimed from the host checks or Settings saves. See
[the complete install checkpoint](todo-20261011.md#v079-touchhighlight-installation-checkpoint).
