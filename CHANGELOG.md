# Changelog

Rubi's releases and test builds, newest first, in the style of
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). A test build is made for a console run on
the owner's PS Vita, a PCH-1001 on firmware 3.65, with CLANNAD (`PCSG00415`), and published as a
pre-release (test.4.1–4.3, respins with fixes, were installed but not published); Rubi's OCR ran in
CLANNAD in every one. What a run showed is under **On the console** (in the 0.7 entries, after "On
the console:"), from the owner's reports and logs; other figures say where they come from. The test
builds are the debug build: no console run of the normal build, the releases' main download, has
been reported.

## [Unreleased]

### Fixed

- **The reading streak's seed and line** (#501's audit; [details](docs/ocr-history.md#streak)). When the overlay's
  own record and `history.csv`'s days both end on one day, or their runs of days overlap (`history.csv` moved away
  before the Rubi app's first start), the seed keeps the whole run, from the earlier start to the later end; it kept
  the file's shorter run before. A record just short of a 64 KiB window no longer makes the seed miss the record
  after it (the overlay writes none that long). The Triangle view shows no streak line until the overlay has read the
  record since it started, never a "current 0 days · longest 0" the file does not hold; and a line recorded while
  the overlay is open redraws it only when the streak changed (a redraw resets the entry's scroll). A Cards change of
  the 0.9.8 test builds that a stop interrupted left both of its cards queued:
  [anki.md](docs/anki.md#the-queue-on-the-memory-card) says which to delete. The normal Shell's code grows by 32 B
  (16 B in the debug build), in the same pages [measured: the module rows]. Host tests; not run on a console yet.

## [0.10.0] - 2026-10-28

VOCR6 for all three model sizes, and a text spotter that finds the lines (vita-vn-ocr #125; v0.4.1-vocr6.2 and
textspot8): measured on a computer through Rubi's path, on held-out pages of the owner's hooked games dialogue error
goes from 1.177 / 1.008 / 0.990 % to 1.049 / 0.936 / 0.917 % (Small / Normal / Large; other pages of the trained
games, which the shipped models then trained on too), other games are level (none of it resolved; Small in point
worse), and the spotter takes other games' screenshots from Rubi's finder's 2.38 % to 1.03 %. VOCR6 was read through
the old finder, not the spotter, and has seen CLANNAD's glyph raster. OCR keeps a fixed 1 MiB of a game's free memory
for every size (#280, which the owner's console run of a 0.9.4 test build confirmed). Also: the Rubi app's Look up
tab and a game's own page with its line finder, richer Anki cards with duplicates and known words, the reading streak
in the overlay's △ view (the test builds' Stats tab and Cards page are gone), a suggested text box on first use,
faster dictionary lookups, verified native hooks for 26 more games (Vita3K-proven; one also on a console), Vertical
text ON fixed on a horizontal box, and a press that reads at once (#413). Nothing else new here has run on a console
yet. Like 0.9.8, this release is the normal build only: `Rubi-0.10.0.vpk` (the main download) and `Rubi-0.10.0.zip`.

### Added

- **A game's own line finder** (#333; [guide](docs/guide.md#a-games-own-page)). The finder (`ocr_finder`, the
  spotter by default since API 54) and Rubi's finder's fixes (`ocr_line_fix`, API 50) were one choice for every game,
  and the best one depends on the game. Now each game can have its own. Hold Enter, or a touch, on a game in the Rubi
  app's Games list for half a second: its page has the game's ON/OFF and its **Line finder**: Default (`config.ini`'s),
  Spotter, or Rubi's finder with Off, On, Junk or All. A press or tap let go sooner still turns the game ON or OFF; the
  list shows `Line finder: On` on a game with its own.
  - **Measured** (host; the per-page records of the runs behind vita-vn-ocr #129 and #149, counted; VOCR5-N, the 1 MB
    LM, the cascade): the spotter, the default, is best overall (1.31% on the 46 hooked games, against All's 1.50% and
    On's 3.29%; 0.99% on #149's 92 development games) and on Narcissu too (63 errors in 4,959 characters, All 137, On
    677), the game #333 was opened for. All read better than the spotter on 17 of the 46 and 23 of the 92 games, on 12
    of those 23 by one or two errors: an upper bound, each game scored on the pages it was compared on. A player could
    give such a game its own. The default is unchanged.
  - **Where:** `ux0:data/VitaJPOverlay/ocr_line_fix.ini`, one `TITLEID = spotter|off|on|junk|all` line a game in
    `games.ini`'s form, read by `games.ini`'s reader. `games.ini` is unchanged: earlier builds read it strictly, and a
    line they do not know makes it unreadable to them (its backup's choices, or every game OFF).
  - **When:** the Shell reads it with the game's ON/OFF as the game starts or resumes, and the value replaces
    `config.ini`'s two keys in the user's jobs (with Spotter, `ocr_line_fix` stays `config.ini`'s, for its fallback);
    `same_pipeline` compares them, and the self-test reads as before. A PSP game without its own follows the shared
    PSP row (Adrenaline / PSP games without a setting of their own), as its ON/OFF does.
  - **No built-in table:** a game without its own uses `config.ini`'s.
  - **log.txt:** the `game … started` line ends with what the game's jobs use (`ocr_finder grad, ocr_line_fix on`),
    and no longer names the dictionary (Local dictionaries in every build since the online ones went);
    `tools/analyse_selftest.py` reads both forms. The debug build no longer logs the title's app parameters 9 and 10
    (answered: 55 game starts in test.1–test.4, parameter 9 always refused).
  - **Memory:** the normal Shell takes 40 B of code and 144 B of data, the debug Shell 48 B and 64 B; no module takes
    a page more, and nothing is allocated ([memory ledger](docs/memory-ledger.md)).
  - **Its audit's follow-up:** the Shell reads `ocr_line_fix.ini` alone, so after a save cut short left only its
    backup the Rubi app puts the file back as it starts, and a hand-edited file it cannot read is kept, beside its
    backup too (before, the app showed the backup's choices and its next save deleted the edit); a touch that slides
    off a game's card does nothing; with the app's banner up, a game's page says that an older overlay ignores its line
    finder.
  - Not run on a console yet.

- **A Look up tab in the Rubi app** (#313; [guide](docs/guide.md#look-up)): look a word up and mine it later, beside
  the suspended game. **Search**: Triangle opens the Vita keyboard (every language's keyboard, 128 characters) for a
  word or a sentence, looked up with the overlay's own lookup and dictionaries (`local_dictionaries`, their order and
  ranking, the other dictionaries' entries below the word); Left/Right move by the matched word, the stick a
  character, a tap to a character. **Earlier lines**: a game's last 200 lines in `history.csv`, newest first, the
  most recent game's first (Square: the next game), read from the file's end a part of it each frame; Enter opens a
  line with the same word cursor. **Enter saves a card**, as the overlay's Enter does but **without a picture**, with
  Card mining ON or OFF: its sentence the line (or the typed text when it is longer than the word), its game, title ID
  and platform the line's, MiscInfo the line's date; **Export cards** exports it with the others
  ([anki.md](docs/anki.md#cards-from-the-look-up-page)).
  - **The same lookup as the overlay's:** on the 50 real VOCR lines of `host/fixtures/word_lookup`, walked word by word,
    the app and the overlay's own pipeline (over the Shell's memory layout) reach the same cursors and show the same
    entry at each: 2,965 lookups in three settings, a line's rows without Japanese left out as the overlay leaves them
    out [measured on the host: `host/tests/test_lookup_parity.c`].
  - **Cards:** the app's writer keeps the overlay's rules (a temporary file, flushed, renamed, the card synced; the ID
    the record's SHA-256); a stop at each of its writes leaves the card whole or not there
    (`host/tests/test_anki_restart.py`), and Anki imports its cards (`host/tests/test_apkg.py`).
  - **No setting:** nothing that users see today changes; the page is new. The tab bar lays itself out from its
    labels (five tabs).
  - **Memory:** the app's own, static: about 430 KiB (a 64 KiB lookup block as the Shell's, a 128 KiB card arena, a
    64 KiB history window; [vita-memory.md](docs/vita-memory.md#54-the-rubi-app-a-system-application)). A font's
    glyph atlas holds about 500 characters (an estimate) and drops new ones once full, so the app now measures text
    with a second font, never drawn with, and loads either font again, with an empty atlas, before it holds 350
    characters: four atlases of 256 KiB at most, and each font replaced leaves 4 KiB of the heap (libvita2d's
    `int_htab_free`; the log counts the switches). The Shell, the game worker and the kernel are
    byte-identical. Not run on a console yet.
- **Duplicates at export** (#314, roadmap 4 stage A; [anki.md](docs/anki.md#duplicates)). Mining a word again from
  another line made another card, and nothing said so. The export now finds a card **whose word an earlier card
  has**, queued or exported before: the word is the record's dictionary form and reading (every inflection is one
  word; katakana count as hiragana, `・` is left out). The Rubi app's new **Duplicates** row (Anki page) says what
  it does: **Tag** (the default) gives the note the tag `duplicate`, so no card is lost and Anki's browser
  finds them (`tag:duplicate`); **Skip** leaves the card in the queue, checked again at each export; **Keep**
  exports every card as before. The page says **Export cards (12 queued, 3 already mined)**. The choice is in the
  Rubi app's own settings file, `ux0:data/VitaJPOverlay/app.ini` (#381; the overlay never reads it), and each
  card's word is cached in `anki_words.bin` (20 B a card) so that a count reads only new records. The overlay,
  the game worker and the kernel are unchanged (the same module rows). Anki 26.09.3 imports the tag
  (`test_apkg.py`). Not run on a console yet.
- **Known words** (#314, stage C; [anki.md](docs/anki.md#known-words)). `tools/known_words.py` writes
  `known_words.txt` from an Anki collection (or a `.colpkg`/`.apkg`, or Anki's Notes in Plain Text export): each
  note's Expression and ExpressionReading by default. On the Rubi app's Anki page, **Known words: Import** reads it
  (one word a line, an optional TAB and reading; up to 65,536 words) into `anki_known.bin`, and the export tags the
  cards of those words `known`: by word and reading, or for a line without a reading every card of that spelling.
  Checked with Anki 26.09.3's own collection and plain-text export (`test_apkg.py`). The overlay is unchanged: the
  popup's mark for a word you have (#314's stage B) does not fit the Shell's pages, so it is not built. Not run on a
  console yet. After its audit: `tools/known_words.py` opens a collection under any path read-only (a `#` or `?` in
  it no longer cuts the path), reads HTML ruby and furigana readings, refuses `--deck`/`--note-type` on a
  plain-text export without that column, keeps a plain-text export without HTML as text and leaves out words
  starting with `#`; the import ignores long extra columns; the page says when the known words' index is damaged;
  an unchecked export is shown as an error; Skip counts a card left in both directories once.
- **Richer Anki cards, made at export** (#311; [docs/anki.md](docs/anki.md#card-type-game-tag-and-sentence-readings)).
  Three rows on the Rubi app's Anki page, which the next export follows, also for cards queued before; they are the
  app's own and live in `ux0:data/VitaJPOverlay/app.ini`, which the overlay never reads:
  - **Card type**: Word (the default, the cards as before), Sentence, Both or Click, Lapis's own card types: the
    export sets the type's switch (`IsSentenceCard` and the like) to `x`.
  - **Game tag** (ON): each note has its game's tag beside `vita-jp-overlay`, `Game::CLANNAD`, in GameSentenceMiner's
    form (the name without its spaces, U+3000 and `::`). A record without a game (versions 1 and 2) has none.
  - **Sentence readings** (ON): `SentenceFurigana` holds the sentence with the readings of its words, from the
    overlay's dictionaries, parsed as Yomitan parses a `{sentence-furigana}` and placed on the inflected words as
    Yomitan places them (`行[い]って`, `来[こ]ない`); the mined word keeps its own reading and its `<b>`. A word whose
    reading the dictionaries cannot be sure of gets none (今日, 方 alone, a single kanji in a compound, a name VNDB
    romanizes otherwise, a counter after a number). On 3,000 real visual-novel lines the rules were not chosen on,
    1.9 readings per 1,000 kanji tokens were wrong (2.1 at the measurement, recounted after its audit) and 174
    missing, against about 46 wrong and 34 missing for
    Yomitan's `{sentence-furigana}` over the same dictionaries
    ([reports/sentence-readings.md](reports/sentence-readings.md); host measurements, the threshold for ON fixed
    before the measurement). An export then takes about 0.5 s more a card (an estimate: 17 lookups and 98 reads a
    sentence on the host); its progress counts every card.
  The Shell, the game worker and the kernel are unchanged: the code is the app's (`core/sentence_reading.c`,
  through the Look up page's `settings/app_platform.c`), with 200 KiB of static memory for the lookups. Not run on a
  console yet. After its audit: the game tag drops the whitespace, then `::` (`A: :B` is `Game::AB`, as GSM's);
  Cancel stops one card's readings before their next lookup (a long sentence takes hundreds); a highlight inside a
  character takes the whole character, so `<b>` is always closed; 通りに, which the longest match reads どおりに
  after a verb too, gets no reading; three verdicts of the measurement corrected (2.12 wrong per 1,000 at the
  protocol's commit, not 1.84; the default stays ON), `tools/sentence_readings/compare.py` checks a part read
  unit's readings where they lie and draws Yomitan's sample itself; tests for a record's own readings, the
  dictionaries closed after an export, the progress, and unreadable dictionaries tried once.
- **The reading streak, in the overlay's Triangle view** ([details](docs/ocr-history.md#streak)). The Stats tab is
  gone (below); its streak is now the last line of △'s view, under the two blocks, for all games:
  `Streak: current 3 days · longest 12` ("1 day" for one). A day counts when `history.csv` gets a line with at least
  one character that day (the `characters` column's rule: a Japanese letter, an ASCII letter or a digit), on the
  Vita's local calendar day, so only **Read text: Automatically** counts (Only when I press L + R keeps no history).
  Current is the days in a row that end today, or yesterday while today has no line yet, else 0; longest is the most
  days in a row. A clock or time zone moved back by hours or a day changes nothing; moved back further, the streak
  starts again at 1, and the longest stays. The record is a 16-byte file, `ux0:data/VitaJPOverlay/reading-streak.bin`:
  the overlay adds each line's day to it as it records the line, writing the file only when it changes (a day's first
  line), and reads it as △ opens. The overlay never scans `history.csv` for it; in SceShell it costs 480 B of the
  normal Shell's code (512 B in the debug build) and 8 B of its `.bss`, within the code pages #428 and #440 freed, no
  allocation and no budget raised [measured: the build's module rows]. The first time the Rubi app opens after the
  update, a thread of its own reads
  `history.csv` once and replays its days through the same rules, so a streak read before the update carries over (an
  app closed before it ends seeds again at its next start). Host tests: a day's rollover, gaps, a time-zone change,
  first use and the seed, a clock set back. Not run on a console yet.

- **A text box suggested on first use** (#358; API 52; [first-box.md](docs/first-box.md)). A game with no box of its
  own is read on its whole screen, picture and buttons included, which first-time users took for recognition that did
  not work. Now the first time the overlay is open over such a game's text, it opens the region mode with a box drawn
  around the dialogue and asks **Use this box?**: Enter saves it as the game's region, as a box drawn with □ is, and
  the overlay closes; the stick moves its corner, Cancel draws your own. Asked once each time the game starts or
  resumes, and never once the game has a box; a box drawn with □ asks the same question (its hint began "Box").
  The game worker finds the dialogue among the lines its job on the whole frame recognized (the block of lines with
  the most kana and kanji, from the last screen read) and suggests the screen's width from just above it to the
  bottom; a menu of stacked items can be offered a band too.
  Measured on a computer on the hooked pages of 20 games the rule was not chosen on: the box holds 96.6 % of the
  dialogue lines of the same game's later screens and reads 0.89 other lines a screen against the whole screen's 3.45
  (0.21 with Japanese, against 0.83). It is ON by default; the Rubi app's **Suggest a text box** OFF (OCR & lookup)
  writes `region = full` in `region.ini`, the line for every game without its own. Not run on a console yet.
  The worker takes 1,040 B more of its pages and the normal Shell 24 B of code and 88 B of data; no module takes a
  page more, and nothing is allocated ([memory ledger](docs/memory-ledger.md)).
- **The text lines are found by a small network: vita-vn-ocr's text spotter** (`ocr_finder = spotter`, the new
  default; `grad` is Rubi's finder as before, which also stays the spotter's fallback). Trained on the owner's hooked
  PS Vita and PSP pages and on MeikiOCR's line boxes of other games' screenshots (vita-vn-ocr #149, textspot8), it finds
  the lines inside your region, and the recognizer reads them as before. On other games' screenshots (host, measured
  through Rubi's path) it takes the dialogue error from Rubi's finder's 2.38% to 1.03% (0.99% with VOCR6), and on a
  fresh set of 41 games from 4.29% to 1.86%; on the hooked test game and on CLANNAD's dialogue it is level with Rubi's
  finder (0.97% against 0.98% and 0.95%), and with CLANNAD's speaker names it reads 1.17% against 2.83%. It no longer
  merges the lines of a translucent box over a busy picture,
  and an animated icon next to the text costs it less; it still misreads side-by-side choice buttons. Its weights
  (52,176 B) are a file beside the models, read once into the game's memory and kept; when they are missing or there
  is no memory for them, and for a region it sees as vertical text under `ocr_vertical = auto`, the job reads with
  Rubi's finder. Memory (exact bytes, [memory ledger](docs/memory-ledger.md#the-spotter-finder-api-54)): its block,
  57,344 B, and its part of the job's scratch, only in jobs that use it; its code, 16 KiB more of the OCR
  worker's pages in every game that is ON (208,896 B, 217,088 B in the debug build). Its time is
  estimated at 82 ms for a 900×150 text box and 105 ms for 900×200 at 333 MHz (vita-vn-ocr's cycle model; nothing of
  it has run on a console) ([the spotter finder](docs/vita-vn-ocr.md#the-spotter-finder-api-54); API 54).
- The open overlay says why it is still recognizing (#358). When the text changed under every read of two jobs in a
  row (an animated icon, a moving picture or text still typing in the region), its next try says "Recognizing…
  Something in the region keeps moving (an animated icon? text still typing?). If a region around just the text
  leaves it out, press □ to draw one." instead of "Recognizing…" alone, until a read shows the text. Before, it said
  "Recognizing…" for as long as the user waited.
  - **When:** with an animated icon, on the host's spinner bench (184 hooked regions, 23 scenarios, est.), 2.85 s after
    L + R (median; 4.8 s at most). In the owner's CLANNAD logs no wait for text met a torn read, so it would have shown
    in none of them.
  - **The fix it names** is a condition: a region drawn with □ leaves out an icon beside the text, not one inside a
    line, a picture behind the text or a line still typing (the audit's follow-up; #364 said "Press □" outright).
    Pause the game to read joins it with #355.
  - **The setting:** a region saved with □ starts the count over. `config.ini` `ocr_wait_hint = off` keeps
    "Recognizing…" (on by default; [why 2 jobs](docs/text-stability.md#why-the-overlay-waits-358)).
  - Not run on a console yet.
- A native text hook for 極限脱出ＡＤＶ 善人シボウデス (Zero Escape: Virtue's Last Reward, `PCSG00020`, APP_VER
  01.00), where users reported OCR never worked and took forever. Its LunaTranslator site could never install: the
  eboot is linked at a fixed 0x81000000, so Luna's address is that address, and Rubi's table read it as an offset
  past the end of the game's code. That site is also inside Lua's pattern matcher, which sees every string, not only
  dialogue. The new site is the entry of the engine's `GUI.Message::setText`, one call per click segment (UTF-8,
  `<K>`, `<P>`, `<F:B>` and other tags removed); a box that grows by clicks shows its newest segment. Found and proven in
  the Vita3K emulator with the psandvita farm (53 pages an AI reviewer approved against the frames the game drew),
  not run on a console ([details](docs/native-hooks.md#table-sites)).
- The same hook for the 善人シボウデス half of ZERO ESCAPE ９時間９人９の扉 善人シボウデス ダブルパック (`PCSG00883`, APP_VER
  01.00), which a user reported broken: the Double Pack starts its 善人シボウデス as a second executable,
  `skp2_psp2.self`, the same engine in another build, so it needs its own entry (the engine's message function at
  another address; the same text clean-up). Its 999 half, the pack's `eboot.bin`, is another engine and still uses
  OCR. Proven in Vita3K with the farm (32 approved pages), not run on a console.
- A native text hook for 蒼の彼方のフォーリズム (`PCSG00700`, APP_VER 01.00), with Rubi's own clean-up rules for
  its text (half-width kana, `<a>(｢)`, `<ruby>(n,reading)`). Found in Vita3K with the psandvita farm's capture
  build: the site gets each page once, 60 pages in a row. **On the console** (a PCH-2000 on 3.65, Rubi 0.9.7 with
  this entry, before trusted hooks, API 43): the site was hooked (`1 table sites`, 0 refused); of its 19 lines in
  two sessions, the 15 compared with OCR agreed (score 1000), the ruby line scored 771 before its rule (1000
  after), 3 scored 0 ([details](docs/native-hooks.md#table-sites)). On `main` the entry is trusted from its first
  line (API 43), so its lines need no OCR: that mode has not run on a console for this site, and the 3 lines that
  scored 0 were not examined.
- Hook requests ([#358](https://github.com/bee-san/Rubi_for_vita/issues/358)): a way to ask for a native text hook
  for a game that has none, or whose hook the worker refuses. The
  [hook request](https://github.com/bee-san/Rubi_for_vita/issues/new?template=hook-request.yml) issue form asks for the
  debug build's `ux0:data/VitaJPOverlay/hooks.log` (it holds no game text) and says how to get it: install the debug
  build, Text source on Automatic, play the game, copy the file over USB or FTP, or run
  `tools/install_ftp.sh --collect`. [docs/hook-requests.md](docs/hook-requests.md) has the steps, what each line of the
  log says and how the owner's hook farm turns a request into a verified entry. `tools/hook_request.py` (in the
  release zip) reads a `hooks.log`, a `--collect` folder or zip, or a request's text, and says what the console saw:
  the game's main module, module NID and `APP_VER`, each site's verdict with the bytes at a refused one, the string
  copies, and what is needed next; in a checkout it compares the log with the tree's tables, and with `--bucket` with
  the farm's hook for the title (did the farm prove this console's build?).
- `tools/install_ftp.sh --collect` fetches `hooks.log` too (it did not), writes `hook-request.md` into the collect's
  folder and zip and prints its one line.
- Native hooks for MAGES/5pb.'s SC3 engine: STEINS;GATE 比翼恋理のだーりん (`PCSG00146`), ROBOTICS;NOTES ELITE
  (`PCSG00352`) and CHAOS;CHILD (`PCSG00555`), 1.00, get verified entries at their page function. Their script
  text is glyph indices into each game's own font, so the hook reads a page through its SC3 end token (its 0x00
  bytes are line breaks and glyphs) and the worker decodes it with the game's charset, a file the release installs in
  `ux0:data/VitaJPOverlay/hooks/` and the kernel opens for that game by its title ID; the speaker's name on its own
  line, ruby's base without its reading. In those games the worker holds an 8 or 16 KiB block for the charset; the
  modules keep their pages ([native-hooks.md](docs/native-hooks.md#sc3-text)). **Proven in the Vita3K emulator
  only, not run on a console** (the psandvita farm's 1,498, 606 and 395 approved pages); without the file the games
  read by OCR as before.
- Native text hooks for 20 more Japanese visual novels, verified entries from the psandvita farm's hook farm, each with
  its text clean-up: 三国恋戦記 (`PCSG00183`) and 三国恋戦記 ～思いでがえし～ (`PCSG00785`), 果つることなき未来ヨリ
  (`PCSG00988`), ISLAND (`PCSG01000`), DIABOLIK LOVERS LIMITED V EDITION (`PCSG00272`), VANDEAD CARNIVAL (`PCSG00472`),
  MORE,BLOOD LIMITED V EDITION (`PCSG00476`), DARK FATE (`PCSG00530`) and LUNATIC PARADE (`PCSG00826`), RE:VICE[D]
  (`PCSG00382`), 戦場の円舞曲 (`PCSG00428`), 忍び、恋うつつ ― 雪月花恋絵巻 ― (`PCSG00602`), 薄桜鬼 真改　風ノ章
  (`PCSG00629`), BROTHERS CONFLICT　Precious Baby (`PCSG00755`), 大図書館の羊飼い -Library Party- (`PCSG00505`),
  あまつみそらに！ 雲のはたてに (`PCSG00638`), もし、この世界に神様がいるとするならば。 (`PCSG00706`), マブラヴ
  オルタネイティヴ (`PCSG00777`), 学園CLUB ～ヒミツのナイトクラブ～ (`PCSG01065`) and スキとスキとでサンカク恋愛
  (`PCSG01250`), all 1.00. Eight of them are not in LunaTranslator's table at all; for 大図書館の羊飼い and 学園CLUB the
  entry lets Luna's own site past Rubi's placement check, and 学園CLUB's `^` now breaks the line as on the screen
  (Luna's filter joined the lines). New clean-ups: the speaker on a line of its own in 果つることなき未来ヨリ and ISLAND,
  DIABOLIK LOVERS' one-cell `!?` and `!!` (written ゞ and ゝ in the script), Aozora ruby (`｜神奈《かんな》`: the base),
  スキとスキとでサンカク恋愛's `/ruby:base&reading/` and layout codes, and 三国恋戦記's speaker without an end code.
  The entries are data in the game worker, which keeps its pages in both builds: some of Rubi's filters are read-only
  data again and `core/hook_luna.c` is built for size, as `core/hook_re.c` is
  ([memory ledger](docs/memory-ledger.md#the-hook-farms-20-verified-entries-no-api-change)).
  In 忍び、恋うつつ, 薄桜鬼 真改 and BROTHERS CONFLICT the game sets a page one line at a time, so the popup shows the
  page's last line. **Proven in the Vita3K emulator only, not run on a console**: 671 pages an AI reviewer approved
  against the frames the games drew (8 to 100 a title); through Rubi's own path the hook's text is the farm's on 603 of
  the 608 single-hit pages, the other 5 differ in how a one-cell `!?` or `!!` is written
  ([details](docs/native-hooks.md#farm-checks)).

### Removed

- **The Rubi app's Stats tab and the Anki page's Cards page**, which only test builds had (the Stats tab
  v0.9.8-test.4 to test.6: #312, #384, #403; the Cards page test.5 and test.6: #315, #392, #417). The owner
  (2026-10-28): "Okay remove goals. Remove the entire stats section. Keep streak, but put that into the Overlay. Just
  longest vs current streak kinda thing. Remove anki card management." The streak is now in the overlay's Triangle
  view (above). With them go the **Daily goal** (an `app.ini` that still has `stats_daily_goal` keeps working: the key
  is ignored, and the app's next save of `app.ini` drops it), `stats_cache.bin` (no longer written; the app removes a
  leftover one once) and `anki_queue/deleted/` (no longer read or written: what a test build left there is harmless
  and can be deleted by hand). Card mining, the export, Duplicates and Known words are as they were. The tabs are
  Games, OCR & lookup, Anki, Look up and Install.

### Fixed

- Packaging: the release zip has `tools/known_words.py`, which the README and docs/anki.md tell you to run for the
  Rubi app's **Known words** import (#314); the zip's tools list never named it (audit-rubi-cleanup-8 F4).
- **OCR keeps a fixed 1 MiB of the game's free memory, for every model size** (#280, the owner's choice of
  2026-10-23), where it kept 4, 8 or 16 MiB for Small, Normal and Large (by the largest size a job holds, its
  cascade's included: vita-vn-ocr's untuned proposal). あおかな (`PCSG00700`) reported 3,072 KiB free, and Small was
  refused there: its 804 KiB model and 192 KiB workspace came beside the 4 MiB. A first job at the full screen now
  needs 2,068,480 B free with Small, 2,494,464 B with Normal and 3,862,528 B with Large, the 1 MiB included (5,214,208,
  9,834,496 and 19,591,168 B before; `tools/model_tiers.json`, estimates from the source); a tighter region needs less.
  Everything else stays: the full page-rounded check before any block, the spotter's block keeping the same reserve
  when it loads and the blocks taken after the check (the weights, the LM, the cascade's model, a tall line over one
  column's bound) too, the LM, the cascade and the helpers only when they also fit, the next smaller size when one
  does not, #324's retry when its weights are refused after the check, and a clean refusal. A job of vertical columns
  (one model, by name) keeps 1 MiB as well. An Anki screenshot's own check keeps 1 MiB too (4 MiB before); a native
  hook's capture-only pass and the SC3 charset kept 1 MiB already. The `Game RAM (KiB)` message and `log.txt`'s `tier …
  did not fit` and `game vocr memory:` lines show the 1 MiB (`1024 reserve`, `reserve 1048576`).
  - **#280's audit:** with `hook_trust = off`, a game at アマツツミ's measured 2 MiB (`PCSG01198`) passes Small's check
    beside the 1 MiB, and where the console reports its free memory in whole MiB (#324) Small's weights are then
    refused once the job's blocks are taken, so the job failed for memory, where under the 4 MiB reserve its check had
    failed and a Rubi verified site's text answered unconfirmed. That answer now also comes when the last model's
    weights are refused after the check (`game_ocr/main.c`; a host test at 2 MiB in whole MiB, which fails without it).
    `hook_trust = on`, the default, answers from a trusted site before any check, as before. Other games' jobs are
    unchanged.
  - **On the console:** #277's debug test build (0.9.4 with #280) carried it, and the owner reports that it works
    (2026-10-28; no figures of that run were reported). #324's measurements in あおかな on a PCH-2000 (0.9.7's debug
    build with #280) ran with it too. Today's `main` with it (VOCR6, the spotter, trusted hooks, #324's retry) has not
    run on a console: 1 MiB stays an estimate.
  - **Tests** (host): a game with 3 MiB free runs Normal or Small, and Large falls back by itself; every size and each
    request by name keeps 1 MiB, at the boundary and with the cascade; the weights refused after the check go to the
    next smaller size at 1 MiB; a verified hook still answers when even Small does not fit; and VOCR6's three sizes
    need their table's bytes.
  - **Memory:** no budget changes, as a reserve is memory left to the game, never taken.

- The hook farm's entries, #394's audit (no change to the game worker or any module): the engine test's check of
  PCSG00505's 8-byte entry, that the farm's next 8 bytes are a `movw`/`movt` pair of one register, also refuses two `bl`s
  and a pair of SP or PC (it took `bl; bl` for a pair); the filters' test vectors say which are the farm's approved hits;
  the filter docs say what the farm's pages do not prove (`AOZORA_RUBY`'s bare form, `PCSG01250`'s `#n`).
- Lookups: what the Yomitan check left (#321; [the report](reports/yomitan-check.md#321-what-remained)). Measured on
  its 64,579 taps of 5,000 hooked lines: 753 shown entries change, SudachiPy calls 661 better and none worse; on
  330,517 taps of 27,970 lines of the same games, 3,962 change, 3,494 better and 3 worse. Host only: not run on a
  console.
  - **A particle before a katakana word** is the particle: のマンション showed 飲む (`-ん`), にエントリー 煮える,
    とセンパイ 渡船, はデート 派手 (457 taps). Not before ァ, ッ or ー (はァ, へッ).
  - **A particle before a form of する** is the particle: 気がした showed 賀する, もせず 模する, 目にした the grammar
    entry にす, はしない 馳す, 気のせい のす (198 taps). An entry spelled as the whole span stays (がする, もしかして).
  - **Ranks of another word** are no longer shown: だと showed ダト's #73,969, にす ニス's, イル いる's #37 (223 taps; a
    spelling-only Jiten row that matched only after kana normalisation, as Yomitan shows none).
  - **わたし shows 私 first**, the character named わたし below it: a character's name spelled as the text still comes
    first (なぎさ, いのり, ミユキ in their games), but not before one of the 1,023 most frequent words. `config.ini`
    `names_first = on` keeps Rubi 0.9.8's order, `off` puts every word first ([the measurement](docs/local-dictionaries.md#names-and-words)).
  - 行っとく shows 行く, not 行う (`-おく` through `-て`, as Yomitan: 言っとく says `-te → -toku`, as 言っておく);
    してんの and 言ってんだ say `-te → progressive or perfect`, not `-n`, the negative; かわされた shows 交わす, not 買う
    (Yomichan's one-step causative passive counts two steps); 手をつかんで shows 手, not 手をつく (`-ん`'s かん left
    out).
  - Kept as they are, and why: [what remains](reports/yomitan-check.md#what-remains) (pitch accents, reading ties,
    the prohibitive な, names' ranks).
  - #321's audit: with Dictionaries shown 1, a particle before a form of する is the particle too, as with 2 (a
    one-step match of the first dictionary kept the whole span before; not seen on the 27,970 lines). With 1, the
    first entry can still differ from 2 for `names_first = off` (8 of 759,994 character positions; [names and
    words](docs/local-dictionaries.md#names-and-words)). Host only.

- **Native hooks: an active string-copy stream no longer loses its slot to a new caller in a race** (#366). When all
  12 CALL streams are held by other callers, a new caller takes the one written least recently. Two races gave it the
  newest one instead: a caller that wrote while the new one searched got a stamp after the clock reading the search
  began with, so its stream looked about 2^32 old; and a stream written, or taken by another new caller, after the
  search had chosen it was taken anyway. The stream that lost its slot (any not kept for agreeing with OCR, so the
  dialogue's too before it agreed) started its hits over and took a slot back at its next copy; its text was never
  wrong. Not run on a console.
  - **The fix:** the search compares the stamps in the clock's serial order, as the job does, without reading the
    clock, and checks its choice under the stream's lock: a stream written since is no longer the oldest, and the new
    caller's copy is dropped, as for a busy stream. The stamps and lengths, which the search and the worker's hook
    watch read without the lock, are written atomically, the stamp last with release order, which the search reads
    with acquire order (so a search that sees a stream's new stamp also sees which caller took it, on the A9 too).
    Their plain writes against those atomic reads were a data race in C11 terms: ThreadSanitizer found 13-14 in 5 runs
    of the race test on `main` (two measurements) and none now (clang 18, the fleet box). A stream that has had text
    no longer reads as empty to the watch while a new caller takes it.
  - **One stream per caller** (#366's audits): a diagnostic pin (no current Shell sends one) takes the stream's lock,
    as an agreeing stream's keep already did, so a keep never lands while a new caller is taking the stream, where it
    could let that caller's other thread take a second one. Should a caller still hold two, the job shows the one it
    writes, whenever it can read that one: it showed the other one's old line, with trust on every job and without OCR
    (the host's job tests).
  - **The wait:** a new caller still waits while the least recently written stream is busy and every other caller
    keeps the CPUs busy. On the host's hammer test (12 callers copying in tight loops; three measurements), on three
    or four CPUs its longest wait was 0.15-0.49 s against 0.04-0.16 s before #366, whose quick takes were then mostly
    the newest stream's; on one CPU about 0.45 s for both; with callers that sleep between copies, at most 17 ms for
    both, but for one run of 40 ms against 4 ms. Passing a busy stream over ended the wait, but gave callers with two
    threads two streams (223 to 5,980 times in 20 runs of 500 ms, two measurements), so it was not taken
    ([details](docs/native-hooks.md)).
  - **Tests:** the issue's state; a suite that runs each interleaving at the search's own points on every run
    (`host/tests/test_hook_stream_seams.c`, also on the ARM lane), 7 of whose 15 tests fail on `main` with its four
    seams added; a check of the watch's reads in the race test, which `main` failed in most runs on two or more CPUs
    (107-200 of 200, two measurements) and in none on one; the job with two streams of one caller
    (`host/tests/test_game_hooks.c`). The fix passed 3,900 runs of the whole race suite (one to four CPUs, unpinned, 8
    copies at once, beside busy loops, gcc and clang). The release and acquire orders compile to the same x86 code, so
    no host test can show them.
  - **Learned hooks reset once:** `core/hook_stream.c` is an input of the hook definition SHA-256, so consoles learn
    each game's hooks again; this fix's three PRs are among the thirteen merged since 0.9.8 that changed it (by #410),
    so the next release does that anyway.
- **A game short of memory reads with the next smaller model** (#324). The console reports a game's free memory in
  whole MiB (あおかな, `PCSG00700`, on a PCH-2000: each of 78 samples was 1, 2, 3 or 4 MiB), so a job that passed its
  memory check with less than 1 MiB to spare saw a whole MiB less once it had taken its own blocks: its model's
  weights were refused and the job failed for memory, with no smaller model tried. Now it gives its blocks back and
  checks the next smaller model, each at most once. On that console, with a debug test build of 0.9.7 and 1 MiB
  reserves (#280, `main`'s reserve since, above), 7 jobs that passed Normal's check read with Small, where 15 had
  failed before; `main` itself has not run on a console. #324's audit: that retry is the same job. It waits no longer than the
  job's stable-text wait (`ocr_settle_max_ms`): it waited again from the start (by the code, up to the whole limit
  again for each smaller model); on the host, with text typing out for 1.9 s at 60 Hz, the job took 2.27 s against
  Small alone's 2.05 s, and now 2.12 s. Its captures and finder time count every try, and the debug build's step log
  keeps the try that read (a full log of 20 steps had dropped its last steps, such as the text's normalisation; the
  refused try's blocks are no longer listed, while a block it gave back for good keeps its free step; after auto's
  vertical-text tries the job needed 21 steps, and the normalisation's was still the one left out, until #385's audit
  dropped a handed-over try's checks too). No module takes a
  page more ([memory ledger](docs/memory-ledger.md)).
- **SC3 text, #368's audit.** In STEINS;GATE 比翼恋理のだーりん, ROBOTICS;NOTES ELITE and CHAOS;CHILD:
  - A charset read that timed out (the memory card's file worker stalled for 2 s) is tried again at a later job, as
    an open is, three tries in all. Before, one such stall at the first job turned the game's hook off for the
    session, and OCR read it.
  - A charset close that fails is now the job's error. Before, the job failed with no cause given (I/O 0), so the
    Shell took the model file for missing or damaged and used a smaller model until the game next started or resumed.
    Now that happens for that one job only, and the remembered hook (the hook cache) is read at the next job, where
    before it was not read for the rest of the session.
  - A charset block the worker could not give back is freed before it takes another (nearly unreachable).
  - The app's Install page names the data files it copies and where they go: "the data files (… MB: the OCR models,
    the names dictionary and the text hooks' charsets) to ux0:data/VitaJPOverlay", and for a repair their folder, for
    example "Copy 1 data file to ux0:data/VitaJPOverlay/hooks". It used to say "the OCR models … to
    ux0:data/VitaJPOverlay/ocr" for all of them.

  Host-tested, not run on a console. No module takes a page more ([memory ledger](docs/memory-ledger.md)).
- **Vertical text: ON no longer stays on "Recognizing…" on a horizontal text box** (API 53,
  [#379](https://github.com/bee-san/Rubi_for_vita/issues/379); a user's PS Vita Slim, Rubi 0.9.8, 遙かなる時空の中で
  Ultimate, `PCSG01157`). ON read every region as columns: on a horizontal box the column finder found up to 16 one-
  or two-character columns, and the job read the box's rows once for each of them, a strip a frame, between their
  recognitions. An animated icon in the box that rested for less than all that tore every try, which the Shell drops
  quietly and tries again, so the text never came; a job of rows reads its lines once. And each try published the
  waiting overlay again, which redrew it at every try: the flicker at its bottom. Now:
  - **ON prefers columns.** It reads the region's rows first, as Automatic does, and the columns unless the rows are
    plainly lines of horizontal text (1 to 4 wide rows, one under another): then it is OFF's job exactly. Where the vertical model cannot be read or has no room, ON reads the rows too, as
    Automatic does (it failed before), and a size whose model fails is left out with ON as with the others. On the
    host the rule read none of 3,715 regions of vertical text as rows (Yunohana SpRING!'s 3,353: tight boxes, boxes with
    the name tag, boxes drawn once and whole screens; vita-vn-ocr #103's 362 development regions; through the job, the
    same text as before on every one), and read 94.6 % of 50,945 hooked horizontal dialogue boxes of 44
    titles as rows (15.6 % of whole screens; [on's rule](docs/vita-vn-ocr.md#ons-rule-api-53-379)). The rule was
    chosen on these regions: nothing held out checks it. A box with a speaker's name or an icon beside its lines is
    still read as columns: draw it around the lines.
  - **A job of columns reads its columns' rows once**, the rectangle around them (Automatic's columns too), before it
    recognizes them: the same text as column by column, from fewer strips (4 columns over 3 strips: 18 → 11 in the
    host test).
  - **A retry no longer redraws the waiting overlay**: the view is published again only when what it shows changes
    (the wait hint, the text).

  On the host's bench (`vjo-vertical-on-bench`, new: the user's kind of box with an icon that turns for 15 frames and
  rests for 15 or 25, every strip read the game's next frame, the Shell's retries; 12 presses each): 0.9.8's ON showed
  the text in 0 of 12 at 30 frames a second; ON now in 12 of 12 each, at OFF's times. (The bench charged a line's
  recognition nothing; #385's audit corrected it, below: at 60 frames a second 0.9.8's ON then shows the text in 12 of
  12, after 4.0 and 8.8 s.) On a box of vertical text ON reads the columns in 12 of 12, before and after. The Rubi app's
  ON line says "Columns, right to left, unless the text is in rows." Not run on a console.
- **Vertical text ON, #385's audit.**
  - **An empty text box is OFF's job.** Between two lines, or over a CG, a box wider than tall holds no line; ON sent it
    to the columns, which gave back the size's model and the cascade's and read the vertical model's weights, and the
    next line read the size's model again: on the host, 6 weight reads and 3 cascade reads in 6 jobs against OFF's 1 and
    1; with the defaults about 0.16 s more on the empty box and 0.23 s (0.47 s with the cascade) at the next line
    (estimates, from the console's read of a 1.16 MB model). Now nothing is read there, as with OFF.
  - **A box at least as tall as wide is read as columns at once**, as in 0.9.8, with the columns' rows read once: ON's
    rule had read none of the 2,659 such boxes of vertical text measured as rows. On the host's bench a box of four
    columns showed its text in 1.6-2.0 s at 30 frames a second and 1.0-2.1 s at 60, against 2.2-3.3 s and 1.8-4.7 s
    (the rows' try first) and 0.9.8's 2.0-5.3 s and 1.3-6.3 s (estimates: each line's CTC frames at a time a frame
    measured for rows in test.4's logs and estimated for columns, the stable-text wait a press had before #413).
  - **The columns' rectangle keeps the job's reserve** (AGENTS.md rule 3). Up to 516,096 B, it was taken outside the
    job's memory check: it could cut 504 KiB into the 4 MiB the job leaves the game, or, with
    `ocr_skip_blank_lines` on, take the room of the weights read after it, so that the job read the columns' text as
    rows. Now a block larger than a column is taken only from what the game has free beyond the reserve and the
    weights (and LM) the job has still to read; otherwise the columns are read one by one, as in 0.9.8
    ([memory ledger](docs/memory-ledger.md)). The same holds for a band of rows that large (a picture, or lines the
    finder could not part, on many whole screens): in a game short of memory a job of rows, OFF's and Automatic's
    too, leaves it out, and its lines are missing from the text.
  - **The step log keeps the last try's steps**: a try of rows whose rows went to the columns leaves the log with its
    memory checks too, so that ON's and Automatic's columns keep their normalisation and parallel steps within the 20.
  - **The bench charges recognition**: it charged none (its clock stood still while a line was read), and now charges
    each line's CTC frames, at a time a frame measured for rows (test.4) and estimated for columns, including the first
    line of each of the job's own retries (#411's audit; one table cell moved: 2.0 s, not 1.4 s). Its table, the
    docs' account of #379 and the
    limits are restated ([on's rule](docs/vita-vn-ocr.md#ons-rule-api-53-379)): a name above the lines and short lines
    (4 characters or fewer) make a box columns, the box Rubi suggests on first use is read as rows in 72 % of the
    measured pages, and an animated icon can still hold a whole screen on "Recognizing…" where its columns are too
    many to read at once.
  - **A press without the stable-text wait** (#413) does not learn an animated icon's rows, so each change of the icon
    between two reads tears the read: on the bench's box with an icon that turns for 15 frames and rests for 15, OFF
    and ON then showed the text after 5.0 s at 30 frames a second (2.0 s with the wait) and not within 20 s at 60
    (1.7 s with it); with slower icons the press was quicker than with the wait. A bench estimate, not a console's;
    `--press wait` gives the press with the wait ([details](docs/vita-vn-ocr.md#ons-rule-api-53-379)).
  - Tests: the empty box (ON's reads equal OFF's), the columns' weights refused after their check (the rows read), the
    longest step log of ON's and Automatic's tries, the rectangle a byte short of room and the LM's room beside it, a
    square box, and a game whose vertical model's file failed; each fails without its fix. No module takes a page more
    ([memory ledger](docs/memory-ledger.md)). Not run on a console.

- **Trusted hooks, #338's audit.**
  - With **Trusted hooks replace OCR** off (`hook_trust = off`), the hook cache is 0.9.7's again: 0.9.8 saved a
    learned string copy at 32 that was not trusted at 16, with the setting off too, so a restart checked it with OCR
    once more before 32, and its first disagreement wrote the cache once more. The cache now keeps the trust in a bit
    of its own, set only with the setting on, and the interval as it is; a source trusted with it on is trusted at the
    next start, as in 0.9.8. The one write 0.9.7 never made: the first launch with the setting off after one with it
    on saves a source that was saved trusted once more, without the bit.
  - `history.csv`: when a line the hook gives in parts grows, its record is replaced only once the file is open (an
    open that failed had lost the earlier part's record), while no count of the reading total reads the file (a count
    that read across it ended "unavailable"), and only when the file still ends with that record (a history.csv
    replaced meanwhile was extended with zeros). A write that fails after the truncation still loses the line's
    record, as the docs now say.
  - The debug build writes a hooked job's source line with its result lines, one append fewer before the line is
    ready (estimated 9-47 ms an append; the normal build has none there).
  - The docs: `docs/ocr-history.md` says a record can be replaced and that lines clicked through are now read and
    counted; "the poll and the lookup are all that is left" holds for the normal build; console steps 8 and 14; the
    poll in press mode, a stream that changes often, the bench's omissions, two lines that merge, "none wrong" by
    construction. The benches' drivers leave out the protected-set titles by default.
  Shell: 32 B of the debug build's code; in the normal build 16 B of code and 32 B of data (a log format moved there),
  the same pages. Not run on a console ([trusted hooks](docs/native-hooks.md#trusted-hooks-api-43)).
- Building and testing, the audit of #397 and #401 (audit-rubi-audit-fix-hooks): the engine test checks every farm
  record's mode, four of them decided by disassembly (`tools/hook_farm.py` `MODES_BY_DISASSEMBLY`), and every one of
  its 16 bytes, so a flipped Thumb flag of PCSG00469, PCSG00553 or PCSG00807 or a shorter signature fails it; the 0x81000000
  segment two tests map is freed after each, a failed one's too; `history.csv`'s rewrite is tested where
  `SCE_O_APPEND` places only the open, which needs its second read of the file's end. One code change, with no text
  changed today: a Luna filter's CP932-only step runs in the two entries that keep CP932's 0xA0 too. The docs say what
  a failed rewrite can leave in the reading total and when a count can still end "unavailable".
- **Native hooks, #316's audit.** ALIA's CARNIVAL! サクラメント (`PCSG00628`): its script's `!X` (the character X)
  is now read before any other code, as the game reads it, so a line with a Latin word in lowercase, a bracket or
  a `|` shows it (`!S!k!y` gave `S！`, now `Sky`), and `u9829.` is ♥ (other `u<n>.` code points are still dropped:
  the farm's survey of the script names only that one). None of the farm's 1,208 pages of the game has either, so
  their text is unchanged. CP932's byte 0xA0 is the blank cell of the two games that draw one (忠臣蔵46+1 V,
  ALIA's CARNIVAL!) and, as in 0.9.7, nothing in any other game's text: 0.9.8 decoded it everywhere and could
  change a line with that byte through Luna's filters or a string copy (none of the farm's other games has one).
  真 流行り神 (`PCSG00380`), whose hook reads a fixed address beside the game's code, is hooked only when the
  game's code is at 0x81000000, where the farm proved it, and refused as `relocated` otherwise (OCR then reads
  the game). As every change to the hook code does, this renews the hook definitions' revision, so a string-copy
  source Rubi had learned for a game is confirmed with OCR once more. Not run on a console
  ([CP932's 0xA0](docs/native-hooks.md#rubis-filters)).
- Building and testing (#316's audit F3, N5): the engine test runs each verified entry in the game as the farm
  recorded its hook (the farm's module NID, code bytes, register and padding, in `host/tests/fixtures/hook_farm.inc`
  from `tools/hook_farm.py fixtures`), not in one made from Rubi's own entry, so a wrong NID, byte, register,
  padding or mode fails it (the audit's 4 surviving mutations, and 3 more, all fail now); `tools/hook_farm.py
  fixtures` writes the committed file byte for byte again (it had added the SC3 games' excerpts, which
  `tools/sc3_charset.py` writes), and its own tests catch a checker without `vjo_hook_clean`, a comparison that
  keeps the white space at a text's ends, and a page's text given to another hit than its last.
- **A trusted hook's line no longer fails for memory** (API 51). When the screen had changed since the hook's line
  arrived (a menu over the line, or a larger change some time after the line drew), a trusted hook's answer still
  read a strip of the screen first, and that strip needed 1 MiB free in the game. A game short of memory then
  failed the job although the hook had the line: 蒼の彼方のフォーリズム (`PCSG00700`) sampled 1 to 4 MiB free on a
  console (#324), so a press some time into a line could fail there. The kernel now marks the answer with the
  screen's current scene without reading pixels, so a trusted hook (a Luna site, a Rubi verified site, a string copy
  that earned trust) needs no game memory at all. It still reads the strip only while the region has no screen yet.
  Not run on a console yet.
- Building and testing (no change to the app or the plugins): the hook streams' race tests
  (`host/tests/test_hook_stream.c`) pass or fail by what the code does, no longer by how the threads were scheduled
  (#344). `replacement_race` failed now and then in CI (`call_replaced > 0`) with nothing wrong: offers never wait,
  so while one thread held the oldest slot's lock off the CPU, every other caller's hits were dropped and no slot
  changed hands (12 to 29 runs in 1,000 on one to four CPUs, measured on the fleet box). And the job thread often
  read before the game threads began: in `concurrent` it read nothing in 378 of 500 runs on one CPU, so a torn read
  could not have been seen. The threads now start together at a barrier and race until the checks have something to
  check (every caller has had a slot, the job has read text). A slow or starved run takes longer: a race can end
  without that only after 10 s and 20,000 more hits of every thread. A failed check prints the race's counters. Each
  caller offers one text of its own, so a hit written into another caller's stream is counted even when the job does
  not read it in time, and `concurrent`'s writers share one clock across three slots, as the worker's slots do. Races
  put in on purpose are caught more often, on two and four CPUs, each run beside one of the old test: the key check
  under the slot's lock removed, in 154 and 748 runs of 1,000 (the old test: 0 and 17); a clock increment that is not
  atomic, in 363 and 486 of 500 (the old test: none).
- **CLANNAD's hooked lines show 岡崎 and 朋也 instead of `*A` and `*A*B`.** CLANNAD's hook reads each line before the
  game puts the protagonist's names in: its script writes them as the name variables `＊Ａ` (the family name) and
  `＊Ｂ` (the given name), which Rubi showed as `*A`, so `「ありゃ、岡崎、どこいくんだよっ」` came out as `「ありゃ、*A、どこいくんだよっ」`.
  The hook's own text filter now writes the names; CLANNAD has no name entry, so they are always 岡崎朋也. Seen in 16
  of 1,347 hooked lines of a console's history (Rubi 0.9.8).
- Native hooks (#349): TIME TRAVELERS' (`PCSG00062`) Luna hook was never installed. Its two sites, `0x8119ADDA` and
  `0x811A6052`, are Vita3K's guest addresses (Luna added them before its `addr_offset`), and Rubi read every Luna
  address as `address − 0x80004000`: past the end of the game's 0x31258C-byte segment 0 at 0x81000000 (public
  Vita3K logs), so both were refused as `range` before a byte was read, on every console. Such an address is the
  game's own only in a fixed-address executable, so the generator now marks the 7 sites whose executable a public
  Vita3K log shows as fixed-address (TIME TRAVELERS ×2, PCSG00139, PCSG00172, PCSG00216, PCSG00272 ×2:
  [the list and its evidence](docs/hook-titles.md#sites-at-vita3ks-guest-address)), and for those, when that reading
  is outside segment 0 and segment 0 is at 0x81000000, the worker reads `address − 0x81000000` (`+0x19ADDA`,
  `+0x1A6052`); the placement check then decides on the game's bytes as for any Luna site. Every other site is read
  as before: the 8 that relocatable executables have at or above 0x81000000 stay refused (their addresses are
  another module's). Whether the game's bytes there pass the check is not known (no dump of the game here): the
  debug build's `hooks.log` shows each site's offset and verdict, with its bytes when refused. Not run on a console.
  STEINS;GATE: no Vita release has a hook entry, in Luna's table or Rubi's
  ([hook-titles.md](docs/hook-titles.md#steinsgate-no-hook-entry-in-any-release)).
- **Docs: L and R no longer page the dictionaries, and △ shows this game.** The README's settings table and the
  guide still described the L or R tap that paged between the dictionaries' entries, which 0.9.6 replaced with
  entries stacked below the word (the right stick scrolls to them; #289), and the guide said △'s totals were
  across games, where △ shows today and the total of the game being played (the roadmap #310's "found along the
  way"). Docs only.
- Docs: [the roadmap](docs/roadmap.md), what is left to build, ranked, and which of its top five (#311-#315) are
  built; it was issue #310. Docs only.

### Changed

- **A removed feature's setting is ignored without a warning** (#418, cleanup package P2). A `config.ini` line of a
  setting that an earlier build removed (`ocr_backend`: Google Lens, the VOCR2 and Meiki trials, ncnn; `log_host`, the
  UDP log's computer; `dictionary = jpdb`, `jiten` or `hachidori`; an older `vocr_model` whose name gives no size)
  gave one warning line in the open overlay and `log.txt`, naming what was removed; now it is ignored silently, as
  the removed API keys, AnkiConnect's settings and `frequency_filter`, `font_size`, `hw_jpeg`, `combo_delay_ms` and
  `hachidori_host` already were. Only a file from before 0.8.0 holds such a line, and the Rubi app's next save and
  the installer remove or rewrite it, as before. Every other setting reads as before, with the same warnings; a
  `vocr_model` that names a size still sets it, and a `dictionary` line with any other value than `local` or a removed
  dictionary's still warns, as does a `dictionary` or `vocr_model` line without `=`. The Shell's code is smaller by it
  (measured: in the PR).
- **All three model sizes are VOCR6** (vita-vn-ocr #125 and #157, pre-release v0.4.1-vocr6.2): VOCR6-S replaces
  VOCR5-S as Small, and VOCR6-N and VOCR6-L replace v0.4.0-vocr6.1's as Normal (the default) and Large
  (`VOCR6.2_S_w8.vocr`, 819,792 B; `VOCR6.2_N_w8g2.vocr`, 1,189,408 B; `VOCR6.2_L_w8_2l.vocr`, 2,475,680 B). Each is
  VOCR5's shape fine-tuned again on the owner's hooked PS Vita and PSP games: Normal and Large three times as long,
  Small at half the learning rate. Measured on a computer (dialogue CER through the GRAD finder's path of Rubi 892cfb9,
  as vita-vn-ocr's final reads model it, with the 1 MB LM and the cascade; VOCR5 → VOCR6, 95 % intervals):
  - the hooked games' held-out pages: Small 1.177 → 1.049 % (−0.128 points, −0.189 to −0.074), Normal 1.008 → 0.936 %
    (−0.072, −0.111 to −0.038), Large 0.990 → 0.917 % (−0.073, −0.118 to −0.036), three held-out seeds each, read with
    the 1 MB LM's screening build. Small's are its shipped recipe's; Normal's and Large's are v0.4.0-vocr6.1's
    15,000-step recipe's, as the shipped 45,000-step blobs (one seed) have no held-out read. The shipped blobs then
    trained on those pages too;
  - other games: VOCR5's development score 1.674 → 1.549 (Small), 1.482 → 1.389 (Normal), 1.439 → 1.396 (Large),
    none resolved. Small in the set as it ships reads it +0.041 (−0.016 to +0.099) against VOCR5-S: worse in point,
    and its recipe was the second tried in its stage (the third at Small);
  - read once, after every choice: CLANNAD's lines 0.73 → 0.47 %, 0.66 → 0.33 % and 0.60 → 0.31 % (each resolved);
    vita-test + psp-test's fixed lines 0.68 → 0.58 %, 0.49 → 0.36 % and 0.47 → 0.40 %, whole dialogue 3.04 → 3.02 %,
    2.83 → 2.70 % and 2.72 → 2.68 % (none resolved; PSP alone not better);
  - against v0.4.0-vocr6.1, the set before this one: CLANNAD's lines better (Small −0.21 and Normal −0.10, resolved;
    Large −0.05, not), CLANNAD's pages through the GRAD finder worse at Normal and Large (+0.33 and +0.27 points,
    resolved; still better than VOCR5 or level) and better at Small (−0.41); vita-test + psp-test's fixed lines Small
    better (−0.23, resolved), Normal and Large level;
  - seen data: every VOCR6 model has seen CLANNAD's glyph raster (two hooked games, Grisaia Spinout and Little
    Busters!), so CLANNAD's gain is not an unseen-game result; Little Busters! and planetarian (Key's box with
    CLANNAD) and Collar x Malice (vita-test's Collar x Malice -Unlimited-) were trained on, and the 1 MB and 4 MB LMs
    hold their text; 38 of the 49 hooked games were in VOCR5's pools already; the LMs' Jiten list holds CLANNAD's
    script.

  Rubi's default finder is now the text spotter, which this set has not been read through. Under `ocr_head_gate = on`
  each blob runs what it carries: Small nothing (its blob is plain, so the setting changes nothing for Small), Normal
  its head gate (ε = 1e-4) and two-level head, Large its two-level head. Estimated box times, one core at 333 / 444 MHz:
  Small 0.63 / 0.50 s (about 0.11 s more than VOCR5-S with its gate), Normal 0.77 / 0.61 s, Large 1.88 / 1.47 s (the
  app shows 0.6, 0.8 and 1.9 s). A Normal job with the 2 MB LM keeps VOCR6-N's own settings for it (vita-vn-ocr #151),
  measured on v0.4.0-vocr6.1's VOCR6-N and not again on this one; every other size and LM tier keeps its tier's,
  Small's included. Normal's weights block is a page larger again (1,191,936 B), Small's and Large's keep theirs, and
  no budget changes: the module rows move by 16 B or less in both Vita builds, in the same pages (measured).
  `ocr/VOCR6-ATTRIBUTION.md` is the new release's (non-commercial use only; trained on copyrighted game data, which is
  not redistributed). An update leaves the sizes' old files in `ux0:data/VitaJPOverlay/ocr/`: 0.9.8's
  `VOCR5_S_w8g.vocr`, `VOCR5_N_w8g2.vocr` and `VOCR5_L_w8g2.vocr` (4,488,464 B), and the test builds
  v0.9.8-test.4's and test.5's `VOCR6_N_w8_2l.vocr` and `VOCR6_L_w8_2l.vocr` (3,663,296 B). Nothing reads them, so they may be deleted. Nothing of
  VOCR6 has run on a console ([details](docs/vita-vn-ocr.md#models)).
- Opening the overlay confirms that the text is ready: its OCR read skips the stability wait. On an unsettled
  screen, a running background read yields to a new read without the wait. Background reads and quiet refreshes
  keep their stability checks; completed results still reopen from the cache.
  - **#413's audit: a press's retry after a read torn by an animated icon waits for it again.** The wait is also what
    tells an icon that cycles from text (`core/vocr_ocr.c`), so with an icon in the region quicker than a read every
    try of a press tore, for ever (#353's case; on the host bench's box with the quickest icon, 0 of 12 presses at 60
    frames a second). Now, once a read's screen changed under every try, the open overlay's next tries wait as a
    background read does (150 ms of still rows, within `ocr_settle_max_ms`), and the press after a read reads at once
    again: 12 of 12 there, a median 6.0 s after the press (7.1 s before #413, when every try waited); with slower icons
    0.9-1.7 s, #413's 0.9-1.3 s (estimates of the bench, its 12 presses spread over the icon's cycle since #441's audit,
    [details](docs/vita-vn-ocr.md#a-press-and-the-stable-text-wait-413)). The one difference from #413: a press that
    finds that count held over from a torn job (Automatically's, a refresh's, the subtitle strip's, or a press closed
    while it still tore) waits from its first read. With **Only when I press** every read is a press, so it never waits but
    for those retries. `ocr_settle_press_ms`, a press's limit before #413, changes nothing, and the guide, the
    `config.ini` template and the docs no longer say otherwise. The bench's `--press wait|nowait|retry[:MS]` replaces
    `--settle-ms`. Tests: the press's retry waits, a held-over count, and a reopen keeps the press's own read (#413's
    audit F3); each fails without its condition. Not run on a console.
  - **#441's audit: a press after a torn read keeps the background read still running.** #413 cancelled a background
    read still running on a screen not settled yet, so that the press read at once; with the count of torn reads held
    over (#441) the press's own read waits too, so the cancel threw away a read further along for one that started its
    wait again. 8 of test.4's 75 openings that waited for a recognition had the count held over, 4 of them on such a
    screen [measured: the console's logs, the Shell's count replayed on the host]. Now that read is kept and its text
    shown. The bench's 12 presses are spread over the icon's cycle (`--cluster` keeps the old spacing, all 12 within
    87 ms of one moment of it, for the older tables), and its press figures are measured again: a sampling of one
    moment had given "5.9 s against 1.7 s before #413", where the cycle gives 6.0 s against 7.1 s. The docs name every
    source of the count, and the auto-vs-press page no longer says a press's job waits. Tests: the held-over count keeps
    the read (it fails without the condition), and a press on a quiet screen cancels as on any other unsettled one.
    The debug Shell takes 16 B more code, the normal one none: 4 B of code room left in each. Not run on a console.
  - **#413's audit: a line typing out no longer counts against a native text hook.** A press's read of a line still
    typing out compared its first part with the hook's whole line: a disagreement, so one such read at a check sent a
    learned hook back to a check every eighth line, its trust further off, and two in a row unlocked it (it had to be
    learned again); a trusted hook was never compared, as its jobs run no OCR. Now, in a read without the wait, OCR's
    text that agrees only with the beginning of the locked hook's line is no evidence either way: the press shows OCR's
    text and the hook keeps its state; any other text still disagrees, and a background read is compared as before
    ([details](docs/native-hooks.md#a-line-typing-out-413s-audit-f2)). On the
    hooked page pool's 29 Vita sessions (host: the selection over OCR's real reads, half of the presses on a line typing
    out) a correct hook is unlocked 47 times instead of 105, and gives 5,149 trusted lines instead of 3,510, as it
    reaches trust sooner; it is taken for a line typing out once until it agrees again, so a hook a page ahead of a page
    that collects its lines is still unlocked, never trusted (in the actual worker with every job a press, 146 wrong
    lines against 103 in two sessions, PCSG00648's and one more). With the default `ocr_settle_ms`, background reads are
    compared exactly as before; with `ocr_settle_ms = 0` no read waits, so every read gets the rule. As every change to
    the hook code does, it renews the hook definitions' revision, so a string-copy source Rubi had learned or trusted
    for a game is learned again after the update. The game worker takes 208 B more code (192 B in the debug build), the
    same pages. A press's partial line in
    Automatically and the settled line after it are two `history.csv` records, so the reading totals count the start
    twice: documented ([details](docs/ocr-history.md)); its fix does not fit the Shell's pages
    ([measured](docs/memory-ledger.md#413s-audit-a-line-typing-out-and-a-presss-partial-line-no-api-change)). Not run on
    a console.
  - **#491's audit:** tests pin the worker's half of the rule (a press's request has no settle time: a case fails with
    it wired to 0, to 1 or to another field) and two guards of the once state (a duplicate read takes no exemption; an
    unlock forgets it). `prefix_score` is no longer inlined: the comparison's deepest stack chain is 2,736 B, where #491
    made it 3,760 B (2,772 B before; measured, `-fstack-usage`), for 32 B of the worker's code, the same pages. Repeated
    presses on one line still typing out can still unlock its hook (documented, the owner's call); the console check is
    [step 19](docs/native-hooks.md#to-test-on-a-console). Not run on a console.
- **Normal and Large are VOCR6** (vita-vn-ocr #125, pre-release v0.4.0-vocr6.1; replaced before a release by the
  follow-up set, all three sizes VOCR6, above): VOCR5-N's and VOCR5-L's shapes,
  fine-tuned again on the owner's hooked PS Vita and PSP games, replace VOCR5-N and VOCR5-L as the Normal (the default)
  and Large model sizes (`VOCR6_N_w8_2l.vocr`, 1,187,616 B; `VOCR6_L_w8_2l.vocr`, 2,475,680 B). Small stays VOCR5-S:
  VOCR6's Small read the hooked games better but other games a little worse (VOCR5's development score, +0.070
  points, resolved). Measured on a computer through
  Rubi's shipped path (dialogue CER, 95 % intervals):
  - the hooked games' held-out pages: Normal 1.008 → 0.936 % (−0.072 points, −0.111 to −0.038), Large 0.990 → 0.917 %
    (−0.073, −0.118 to −0.036), three seeds each;
  - other games level: VOCR5's development score 1.482 → 1.451 (Normal) and 1.439 → 1.442 (Large), neither resolved;
  - CLANNAD's lines 0.66 → 0.44 % and 0.60 → 0.36 %, read once; but every VOCR6 model has seen CLANNAD's glyph raster
    in two hooked games (Grisaia Spinout, Little Busters!), so that is not an unseen-game result;
  - vita-test + psp-test, read once: fixed lines 0.49 → 0.49 % and 0.47 → 0.43 %, whole dialogue 2.83 → 2.63 % and
    2.72 → 2.52 % (PSP alone not better); Collar x Malice, a vita-test game's release, and Key's games were trained on;
  - Small changes only through its cascade, which now reads with VOCR6-N: other games level, CLANNAD's pages worse
    (5.84 → 6.16 %, +0.32 points, resolved).

  Their blobs carry the two-level head and no head gate (vita-vn-ocr rejected the gate on VOCR6's logits): with
  `ocr_head_gate = on` they run the two-level head alone, and `ocr_head_gate = gate` now reads them with the full head.
  Estimated box times, one core at 333 / 444 MHz: Small 0.52 / 0.41 s, Normal 0.79 / 0.62 s, Large 1.87 / 1.46 s (the
  app shows 0.5, 0.8 and 1.9 s; about 30 and 80 ms more for Normal and Large than VOCR5's with both paths, which had a
  gate). A Normal job with the 2 MB LM (`ocr_lm = 2000k`) decodes with VOCR6-N's own settings for it (vita-vn-ocr #151:
  LM weight 0.4, bonus 1.5 nats, both line flags); every other size and LM tier keeps its settings, the default 1 MB
  LM's included. Normal's weights block is a page smaller (1,187,840 B), Large's the same (2,478,080 B), and no budget
  changes: the game worker's code is 128 B larger in both builds, in the same pages, and the Shell and the kernel are
  unchanged (measured on both Vita builds). VOCR6's attribution ships as `ocr/VOCR6-ATTRIBUTION.md`: non-commercial use only, trained
  on copyrighted game data, which is not redistributed. An update leaves 0.9.8's `VOCR5_N_w8g2.vocr` and
  `VOCR5_L_w8g2.vocr` (3,666,880 B) in `ux0:data/VitaJPOverlay/ocr/`: nothing reads them any more, so they may be
  deleted. Nothing of VOCR6 has run on a console ([details](docs/vita-vn-ocr.md#models)).
- **The debug build shows a looked-up word without waiting for its log lines** (#360, change 2). Its lookup wrote
  its `lookup …` line and its `job N done` line to the memory card before it published the word: in the owner's
  test.4 log a cursor lookup's done line came 53.5 ms after its start against the lookup's own 32 ms (medians, 338
  lookups), at 17 ms an append, so about 34 ms of every word step were the two lines (est.). The network thread now
  holds a job's lines (a lookup's, and an OCR job's text, first-word lookup and done line; its recognition's own lines
  go out as before) in a 2 KiB batch on its stack and writes them right after the result is out, one append per file,
  the same lines in the same order; the done line ends `…, N held`. The lookups' results are unchanged (byte-identical
  at all 64,579 taps of the dictionary corpus). The overlay's close line also gives a press-to-screen figure, `lookups
  shown N: mean M ms (A ms after the result), max X ms`, from each lookup's press to the frame that drew its word,
  counted without a log line on the overlay's thread; and a header tap no longer logs its cursor from that thread.
  The normal build is unchanged (its plugins byte for byte). Not run on a console yet
  ([details](docs/release-variants.md#the-debug-builds-job-lines-after-the-result-360)).
- The debug build's `hooks.log` names the game's version (`APP_VER`) for every game whose table sites are tried; it
  read `param.sfo` only for a game with a Luna site or an entry that names its version, and logged `APP_VER 0000`
  for the others, the games a hook request is mostly for. The hooks installed are the same in both builds: `APP_VER`
  decides nothing for those games, and the normal build reads it as before (host-tested; not run on a console).
- **A cold dictionary lookup makes about 2 file reads fewer** (#360, change 3), with the same entries shown. With
  both bundled dictionaries, a cursor lookup that the lookahead has not located made 8.12 reads and now makes 5.98
  (host, all 64,579 taps of rubi-dict-check's corpus, each tap's result byte-identical; est. 37.2 → 28.6 ms median on
  the console at the test.4 fit); the lookahead's own work for a word drops from 7.13 reads to 4.98. The open overlay's
  dictionary session now keeps every dictionary's key-filter directory (226 + 10 blocks in the same 1,024 B that
  held one), so the two files no longer re-read their page 0 at each lookup (1.88 reads); a one-character word (は,
  が, を) is checked against the filter block a longer span has just loaded, where the names dictionary answers
  "absent" without reading an index page (0.27 reads); and the Shell reads a file with one `sceIoPread` instead of
  a seek and a read (its saving unmeasured). Not run on a console.
  decides nothing for those games, and the normal build reads it as before (host-tested; not run on a console). As
  every change to the hook code does, it renews the hook definitions' revision, so a string-copy source Rubi had
  learned for a game is confirmed with OCR once more after the update.
- Hook requests, #367's audit: the form, [docs/hook-requests.md](docs/hook-requests.md) and the README say what the
  farm does and does not (Japanese releases of visual novels, a bounded number of attempts, no PSP path yet: not every
  request gets an entry), which debug zips write `hook-request.md` (test builds after v0.9.8-test.2), and that a
  `--collect` zip (logs with text, Anki cards, settings, taiHEN configurations, crash dumps) is sent privately, never
  attached to a public request. `tools/hook_request.py` says so too (a release that is not Japanese, a PSP game, an
  entry refused as `tai` or `full`: a bug report), reads a reply that quotes the log, warns when `log.txt` shows the
  log is from an earlier session, and exits 2 with a message, never a traceback, on a damaged zip, an unwritable
  `--out` or a malformed farm `rubi-hook.json`.
- **Dictionary lookups of the commonest words read less** (#360, change 4): 私, 中, 日, 頭, こう and the other words of
  the lookup's two short tables (common readings, usual spellings) are ranked without reading each of their entries.
  Their tap read every entry's first 160 bytes, 16 reads for 私, because the tables compare the entries' spellings and
  readings; the bundled dictionary now marks the one entry a table names (posting hints version 2, two spare bits of
  each posting, 99 postings; [format](docs/local-dictionaries.md#binary-format-version-3-with-frequency-ranks)). The
  entries shown are the same at every one of the 64,579 taps of 5,000 real lines (host); there, after change 3, a
  cursor lookup makes 5.67 reads instead of 5.98, and the 2.7% of taps that read 4 to 17 entries' heads (17.0 reads:
  私 22) make 6.2 (私 5.1; est. 64 -> 32 ms on the console at test.4's 2.98 ms a read). The two tables move to
  `tools/lookup_tables.txt`, which the converter reads too (the Shell's bytes for them are unchanged; the reader's
  code takes 0 B more text in the normal Shell and 16 B in the debug one). Your own dictionaries get the marks with
  `convert_dictionary.py --frequency … --table-hints`; a file with them is refused by Rubi 0.9.8 and older, so it is
  not the default. The bundled dictionary's SHA-256 changes (`docs/plaintext-dictionaries.json`). Not run on a
  console yet.
- Building and testing (no change to the app or the plugins): the fast gate also runs the host suites with the
  pinned release models (its `models` lane), which CI has none of: #335's host tests failed only with them (the
  tests' fake memory and, without VOCR5-V, an expectation of the rows' try: both fixed in #341), unseen until a
  release check ran them. It runs when `VJO_VOCR_MODEL_DIR` or
  its cache holds every pinned model the tests read, each checked by size and SHA-256
  (`tools/prepare_ocr.py --backend vocr-tests`, `--check` to only check); `--models on` fetches them first.
- Packaging (no change to the app or the plugins): a release zip packaged again from the same artifacts and models is
  the same bytes. 0.9.8's was not: `package-manifest.json` took the time of the run and the names dictionary's staged
  files the filesystem's order, so the published zip's SHA-256 matched neither the Release run's nor another machine's
  (same files; the VPKs were the same bytes). Both are fixed (`tools/package_release.py`).
- Building and testing, #343's audit: the set also holds v0.3.0's three VOCR5 recognizers in `v0.3.0/`, so the lane
  runs the off-path check (each size's blob read with `ocr_head_gate = off` gives v0.3.0's logits bit for bit), which
  it skipped; the lane checks the models again in its container (a symlink to a file outside the directory fails
  there by name, not as test failures) and gives its build the gate's checked weights. The game worker's tests cover
  the tall-line block (`VjoGameVocrLine`) where only the models reached it, and without them (CI): auto's columns
  try gives it back before the rows' try, a job of columns at its end, and a refused one leaves the columns out.
- Building and testing (no change to the app or the plugins): the fast gate's `/tmp`, in memory, is capped at 2 GiB
  a lane (#417), so a runaway suite fails instead of filling the machine's memory (the largest lane's peak was
  143,616 KiB [measured, 2026-10-27]).

## [0.9.8] - 2026-10-27

The first pieces of VOCR6, vita-vn-ocr's work on the owner's hooked PS Vita and PSP captures (vita-vn-ocr #125):
faster OCR (the head gate and two-level head), a line finder that keeps a line whole across a full-width space, and
new character language models for the 1 MB and 4 MB tiers. Through Rubi's page path on development data, measured on
a computer, the finder fix and the new 1 MB LM together take whole-dialogue error from 3.875 % to 1.636 % on held-out
pages of the hooked games and from 3.036 % to 2.441 % on 93 other games (VOCR5-N without the head gate; the recognizer
itself is still VOCR5's). Most of the hooked games' gain is one title, PCSG00854, whose box decoration the finder
reads as a line that the old LM's reading kept on screen and the new one's does not: without it, 2.330 % to 1.615 %
(−0.72 points, 95 % interval −1.05 to −0.44, 45 titles). The hooked games' figures are optimistic, as the LM's
decoder settings were chosen on those pages. On the other games the gain is the finder's: the LM on top of it is level
(2.340 % → 2.441 %, not resolved). Also: vertical text, trusted hooks that answer without OCR, Yomitan's lookup rules
and the adaptive reading time. Nothing new here has run on a console yet. Like 0.9.7, this release is the normal
build only: `Rubi-0.9.8.vpk` (the main download) and `Rubi-0.9.8.zip`.

### Added

- **Trusted hooks without waiting** (API 49; with **Trusted hooks replace OCR** on, the default): a trusted hook's
  new line is ready to look up as soon as the game writes it, as with a hook on a computer. The worker polls the
  hook every 16 ms (was 100 ms) and reports each trusted line at once (no 200 ms gap; the same text again is no new
  line), from a site's first line on; the kernel wakes the Shell for it even while the last line still types, and the
  job reads no pixels (no capture pass); the Shell reads it past the subtitle strip's and a failure's pacing, and with
  the overlay open replaces the text at once (a line that grows keeps the cursor); `history.csv` keeps one record
  for a line given as it types; a trusted string copy stays trusted when the game starts again (no confirmation
  OCR). On the host, over 8,895 hooked lines of 30 Vita games, from the game's copy to the line and its first word
  ready (the lookup counted as the console's 70 ms): 187 → 78 ms; clicking through, a line every 0.5 s: 8,832 of
  the lines never read → every line in 80 ms; a verified site's first line 1.8 s → 78 ms; the open overlay's text
  117 → 8 ms. In the normal build all that is left is the 16 ms poll and the lookup; the debug build (every test
  build) also writes its result lines before the line is ready, one append per file (estimated 9-94 ms more)
  ([details](docs/native-hooks.md#no-wait-for-a-trusted-line-api-49)). Not run on a console.
- **Vertical text** (`ocr_vertical` in `config.ini`, the Rubi app's **Vertical text** card on the OCR & lookup
  page): Japanese set in columns, read right to left, as in a vertical dialogue box or a manga in NOBORU, is read
  with vita-vn-ocr's vertical model VOCR5-V. **Automatic**, the default, reads the columns where the text box's
  rows look like vertical text, and rows everywhere else exactly as before; **ON** reads columns in every game,
  **OFF** rows only. On the host, through Rubi's own job, it read 863 pages of Yunohana SpRING! (PS Vita, a VN
  the model never trained on) at 4.5 % CER in a box drawn around the columns and 14.0 % in one box drawn once for
  the game (rows read them at 83 %), and on 101,651 regions and screens of 44 horizontal games Automatic read no
  columns and every sampled job's text was main's. The game worker takes one more page in each build (4 KiB, in
  the game's memory); SceShell's and the kernel's pages are as before. Nothing of it has run on a console yet
  ([vertical text](docs/vita-vn-ocr.md#vertical-text-vita-vn-ocr-99); supersedes draft #223).
- **Trusted hooks replace OCR** (the Rubi app's OCR & lookup page, `hook_trust` in `config.ini`, on by default;
  API 43): when a hook Rubi trusts has the line, no OCR runs at all: no model, LM or cascade is read, no OCR
  memory is kept back (its one capture pass keeps 1 MiB instead of 4 MiB), and after eight such lines in a row the
  weights an earlier OCR job left in the game go back to it. Trusted: LunaTranslator's sites (as since 0.9.4) and
  Rubi's verified sites from their first line, and any other hook (a string copy) once it has agreed with OCR on four different lines, the last
  closely. OCR comes back by itself when no trusted hook has text. On the host, over 8,640 hooked lines of 29 Vita
  games, with main's OCR (#307's head gate, #306's LM): a verified site's OCR jobs 0.19 a line → 0 and the worker's
  memory kept between jobs 4.4 MiB → 0; a string copy's 0.19 → 0.08 (trusted in 27 of 29 sessions; its answers are the page's own line by the bench's
  construction, so "no wrong answer" tests nothing); a source one
  line behind the screen was never trusted (it could agree with OCR in 10 sessions and lock in 2: the other ways a
  source can be wrong are not modelled); with 3 MiB free, a Luna site's lines, all refused before, are shown. The price: a screen the hook did not copy (a
  choice menu, the backlog) shows the hook's last line. `hook_trust = off` checks hooks with OCR as 0.9.7 did
  ([details](docs/native-hooks.md#trusted-hooks-api-43)). Not run on a console.
- **Skip OCR on screens without text** on the app's OCR & lookup page (`ocr_text_gate`, API 26's text-presence
  gate, now a switch in the app): a text box that holds no text (the game hid it, a CG, a picture) is not read, so
  no junk text is shown or recognized. It stays **off by default**: on the host, over about 96,000 verified lines
  of 46 games' hooked pages, it also stops some real lines, mostly `……` and one- or two-character ones (「え？」);
  of all their lines, those OCR would have read and the gate stops are 0.004-0.17 % (by platform and region), while
  it leaves 81 of 90 text-free regions without a line (2 without it)
  ([details](docs/vita-vn-ocr.md#text-presence-gate-api-26)). Not run on a console.
- A host test that a mined card carries the game's screenshot end to end: the game worker's JPEG of a frame,
  the overlay's Enter that saves the card with it, the Rubi app's export, and the Lapis note's `Picture` showing
  that JPEG (`host/tests/test_card_screenshot.py`). The code path was already there (since 0.8.0's API 23).
- Native text hooks for two more Vita visual novels from the hook farm's data bucket, with Rubi's own
  clean-up rules for their text: ALIA's CARNIVAL! サクラメント (`PCSG00628`) and 真 流行り神 (`PCSG00380`, a page
  record of character cells, a new way to read a hook's text). Proven in the Vita3K emulator only: Rubi's
  text equals the farm's on all 1,208 farm pages of the first and on 1,060 of 1,092 of the second (the other
  32 differ by the player's default name 北條紗希 only, which the farm's text left out)
  ([farm checks](docs/native-hooks.md#farm-checks)). Unlike the other entries', their games' code was not there to
  check for a jump from elsewhere into the hook's patched bytes (the farm's bucket has no dump of it): if one is
  taken, the game crashes.
- `tools/hook_farm.py` checks the verified entries whose games the farm's bucket has pages for (14 of the 30)
  against the farm's recorded hook bytes and texts; the host tests run 50 short excerpts of them through the
  worker's own hook path.

### Changed

- **OCR: faster reads with vita-vn-ocr's head gate and two-level head** (API 42; `config.ini`
  `ocr_head_gate = on`, the default). The recognizers skip their output layer on the CTC frames a blob's head
  gate calls blank, about three in five (vita-vn-ocr #120, all three sizes), and Normal's and Large's compute
  their rare characters only on the frames that may need one (#122). The sizes now run `VOCR5_S_w8g.vocr`,
  `VOCR5_N_w8g2.vocr` and `VOCR5_L_w8g2.vocr` (vita-vn-ocr pre-release v0.3.6-head2level.1: v0.3.0's
  recognizers, with a 1,792 B gate each and, for Normal and Large, their output rows in frequency order and a
  1,984 B trailer; every weight is v0.3.0's), and the runtime pin went to vita-vn-ocr `47cf3a3` (`f8a1856` since the
  line finder's fix below). On vita-vn-ocr's development data no score was resolved worse; the estimated time per
  3-line box at 333 MHz drops from 0.63 / 0.92 / 2.08 s to 0.51 / 0.72 / 1.82 s (Small / Normal / Large; estimates,
  nothing of it has run on a console). A line whose counter re-decode finds a counter, or (two-level head) whose
  reading holds a full-size カ or ケ before 月, 所, 年, 国 or 条, is read again with the full head (a ヶ the escape
  missed). `ocr_head_gate = gate` or `two_level` keeps one of the two, `off` neither: the full head on every frame,
  as before ([details](docs/vita-vn-ocr.md#head-gate-and-two-level-head-api-42)). Memory: the game worker takes
  two pages (8,192 B) more in each game that is ON, in both builds; Normal's and Large's weights blocks are a page
  larger, the largest job's workspace two; the Shell's pages are unchanged
  ([measured](docs/memory-ledger.md#head-gate-and-two-level-head-api-42-vita-vn-ocr-120-and-122)).
  An update leaves 0.9.7's `VOCR5_S_w8.vocr`, `VOCR5_N_w8.vocr` and `VOCR5_L_w8.vocr` (4,479,120 B) in
  `ux0:data/VitaJPOverlay/ocr/`: nothing reads them any more, so they may be deleted. The plugins of this
  release must all be installed together (API 50, below).
- **New character LMs for the 1 MB (the default) and 4 MB tiers**, from bee-san/vita-vn-ocr pre-release
  v0.3.7-lm6.2 (#127): built with the text of the owner's hooked PS Vita and PSP captures besides the open
  corpora, and decoded with new settings (LM weight 0.6, a bonus of 2.5 nats a character, each line started
  from the start-of-line context and no end-of-line term; `core/vocr_ocr.c` now keeps the settings per tier).
  On vita-vn-ocr's development data (VOCR5-N with its cascade, Rubi's line path and display rules,
  cross-validated) the hooked titles' page-path dialogue CER goes from 4.12% to 3.39% (1 MB) and 4.03% to
  3.57% (4 MB), and their clean lines from 0.49% to 0.40%; the unseen-game development score is level (not
  resolved). Most of the page-path gain is one title whose box decoration the finder reads as a junk line.
  On vita-vn-ocr's synthetic counter lines (#109: 九ヶ月, 三ヶ所 …) the 1 MB LM keeps the small ke slightly less
  often than ctclm.2's (3,015 against 3,104 of 3,969 crops), mostly where the region's edge cuts into the line
  ([details](docs/vita-vn-ocr.md#beam--lm-decoding-api-20)); the loss comes from the new decoder settings, which
  vita-vn-ocr retunes for VOCR6. These figures are VOCR5 v0.3.0's without the head gate. The default combination,
  the gated blobs with `ocr_head_gate = on` and these settings, was measured after this release (vita-vn-ocr #151, on
  a computer): VOCR5-N as this release runs it keeps the counter in 2,940 of the 3,969 crops against 3,015 with the
  gate off (75 lost, none gained: crops cut 2-10 px into the line's first character, where the gate skips the edge
  frames the counter re-decode needs, so nothing is read again), and on real development and held-out lines its
  objective (half the held-out pages' CER, half the development score) moves by +0.004 to +0.012 points (Large's
  resolved worse, and Normal's held-out pages alone, +0.007 points; [details](docs/vita-vn-ocr.md#beam--lm-decoding-api-20)). The 0.5 MB and 2 MB tiers keep ctclm.2's LMs and settings. Both new blobs are smaller than the ones they
  replace (982,320 and 3,603,552 B), so the game's LM block shrinks by 180,224 B at most. An update leaves
  ctclm.2's `vocr_lm_o3_1000k.vlm` and `vocr_lm_o3_4000k.vlm` (about 4.8 MB) in `ux0:data/VitaJPOverlay/ocr/`:
  nothing reads them any more, so they may be deleted
  ([vita-vn-ocr docs/vocr6-lm.md](https://github.com/bee-san/vita-vn-ocr/blob/main/docs/vocr6-lm.md);
  no console run yet).
- **Reading time in the Triangle view's statistics is adaptive** (`reading_time = adaptive` in
  `config.ini`, the default): each line counts the time until the next one, at most 2.5 times what your
  own pace in that game would take for it (2 s to 5 minutes), as GameSentenceMiner counts it, without its
  sessions. 0.9.7 counted a pause only if it was 15 s or less, so a line read with lookups, or any line
  longer than that, counted nothing: on simulated reading it showed about half the real time, and a
  beginner's characters/hour more than twice too high; the adaptive rule is within a few percent there
  ([reading time](docs/reading-time.md); estimates, no console history yet). `reading_time = 15s` keeps
  0.9.7's rule, from the Vita's next start. Each game's statistics are counted once again from
  `history.csv` on the first Triangle press (its cache was counted with the old rule); the file itself is
  unchanged. `tools/reading_stats.py` gives the same numbers (`--time-rule break` or `--break-minutes N`:
  its old rule) ([#303]).
- Building and testing (no change to the app or the plugins): `tools/fast_gate.sh` runs CI's checks locally
  in a few minutes and is the merge gate ([AGENTS.md](AGENTS.md)); CI caches its toolchains, runs ctest in
  parallel and skips what a pull request does not touch (`tools/ci_scope.py`), and builds the Vita targets in
  the VitaSDK image pinned by digest.
- **OCR cuts lines better** (API 50): vita-vn-ocr #129's column fix (`ocr_line_fix = on`, the default): a line's parts
  across a full-width space stay one line. On the owner's hooked PS Vita and PSP pages it takes the whole-dialogue
  error from 3.88% to 3.29%, and on other games' development screenshots from 3.04% to 2.34% (host, measured;
  nothing of it has run on a console). The other settings:
  - `junk`: box frames and ornaments are not read as lines either, for regions drawn with a margin;
  - `all`: `junk`, and touching rows are also split, as Narcissu's are;
  - `off`: the old finder.

  With `ocr_vertical = auto`, a box at least as tall as wide is still checked for vertical text on the old finder's
  rows, which its rule was chosen on, and read with the new finder where it holds rows.

  The runtime pin is now vita-vn-ocr `f8a1856` (#135's merge; it was `47cf3a3`). Memory: the game worker takes two
  pages more, 8 KiB, in each game that is ON, in both builds (192,512 B of module pages; 200,704 B in the debug
  build); the Shell's and the kernel's pages are unchanged. Install all of this build's plugins together (the
  VPK's Install does): the game worker and the Shell must both be API 50.

### Fixed

- **Vertical text, the audit of #335** (host only; nothing of it has run on a console):
  - with **Automatic**, a job whose rows looked like vertical text waited for the text to hold still once
    more in each of its tries: a press could wait up to twice `ocr_settle_press_ms` (three times, the first
    time the vertical model is missing) where it waits at most once. The tries now share the job's wait, as
    a job's other tries do ([text stability](docs/text-stability.md));
  - such a job could name a smaller model size than the one that read its text (a try's step down to a
    smaller size outlived the try), on Anki cards, in `history.csv` and the log; and the debug log's 20
    steps filled up with the earlier try's blocks, so the reading try's own steps were dropped;
  - the kernel now refuses the vertical field's undefined value 3, as the game worker does;
  - the host tests run with the release models too (a column taller than a strip uses the tall-line block,
    which the tests' fake memory did not know); `docs/vita-vn-ocr.md` kept stale copies of two paragraphs
    (the files the broker accepts, without VOCR5-V, and an older attribution), the first of which
    `tools/docs_model_names.py` now catches; it also names the horizontal screens **Automatic** takes for vertical
    text (5 or more short rows lined up, as in a menu, a list of names or a status panel, found on constructed
    screens).
- **Dictionary lookups, checked against Yomitan on 5,000 hooked visual-novel lines** (64,579 taps; the shown
  entry differed from Yomitan's first on 69.2 taps per 1,000, now 65.8;
  [the report](reports/yomitan-check.md), what remains in [#321]):
  - spoken forms the lookup did not find, with Yomitan's rules for them: しておく, 変わらん, 構わん, しちゃ駄目,
    戻らなきゃ, 認めざる, 返さねば, しやがって, 聞きやしねえ, いたしまして, 知らねー, できねー;
  - kana text of a verb spelled in kanji shows the more frequent word of the same number of inflection steps:
    わかった is 分かる, not 分かつ;
  - Yomichan's bare imperative negative is gone, as in Yomitan: 失礼な is 失礼, not a verb 失礼 read しつらい,
    and するな highlights する.

  The Shell's module is 856 B smaller (the rule table is packed), with the same pages; nothing of it has run on
  a console ([#320]).
- The adaptive reading time's pace ([#303], in 0.9.7-test.12) stays at 2 ms a character or more. Below that,
  its 1/32 steps rounded to 0, so a game with many records of 489 or more characters read at under 2 ms a
  character (a hook returning a page at once) could freeze it, and every later line of that game counted
  at most 2 s, also when counted again. Every other history counts exactly as before ([#305]).
- The Rubi app's Games page shows `Hook` for every game with one of Rubi's own verified hook entries. The tag
  kept its own copy of their titles, CLANNAD alone, so 10 of 0.9.7's new games (リトルバスターズ！ Converted
  Edition, グリザイアの果実, 剣が君 for V …) had no tag and 17, アマツツミ among them, showed `Luna hook`; it now
  reads the game worker's own table ([#299]).
- Hook text, from the farm's recorded pages (Vita3K only): BLACK WOLVES SAGA -Weiβ und Schwarz- (`PCSG00935`)
  keeps its line breaks (Luna's filter at its site dropped them: 270 of its 382 farm pages differed);
  忠臣蔵46+1 V (`PCSG00540`) keeps the blank between two sentences (CP932 byte 0xA0, which the decoder dropped:
  170 of 1,037 pages); グリザイアの果実スピンアウト！？ (`PCSG00620`, and the PROTOTYPE engine's two others) shows
  `・` between katakana words, not `…` (12 pages). For the entries 0.9.7 had, the farm's single-hit pages whose
  text is the farm's, as shown: 9,745 of 10,203 before, 10,197 now; the other 6 differ by a default name only.
- A verified entry is not tried in another game version (`APP_VER`) than it was proven in, where the farm
  recorded it: 25 of the 30 entries (01.00); another version is refused as `version` in the debug build's
  `hooks.log`. A param.sfo whose `APP_VER` cannot be read leaves the entry to its module NID and bytes, as
  before.

## [0.9.7] - 2026-10-26

Native text hooks for 26 more Vita visual novels, with Rubi's own clean-up rules for their text,
proven in the Vita3K emulator only; and today's and this game's reading stats on Triangle. Unlike
0.9.4–0.9.6, this release is the normal build only: `Rubi-0.9.7.vpk` (the main download) and
`Rubi-0.9.7.zip`, no -debug build.

### Added

- Rubi's own verified hook entries for 26 more Vita titles, from bee-san/psandvita's Vita3K screenshot
  farm: in Vita3K each hook gave the text of every farm page of its game, each page checked against the
  frame the game drew. 10 are games LunaTranslator's table does not list; 16 are in it, and for 15 of
  those the entry unlocks the Luna site the farm proved, which Rubi's placement check refused. An entry
  hooks only the game version it was made from: the game's module NID and 16 bytes at the site must match
  ([details](docs/native-hooks.md)).
  - PROTOTYPE engine ([#293]): リトルバスターズ！ Converted Edition (`PCSG00007`), グリザイアの果実
    -LE FRUIT DE LA GRISAIA- (`PCSG00206`), グリザイアの果実スピンアウト！？ アイドル魔法少女ちるちる☆みちる (`PCSG00620`).
  - IF:Lib engine ([#294]): MARGINAL#4 IDOL OF SUPERNOVA (`PCSG00448`), CLOCK ZERO ～終焉の一秒～ ExTime
    (`PCSG00469`), ピリオドキューブ～鳥籠のアマデウス～ (`PCSG00853`).
  - Luna's sites that the placement check refused ([#295]): 黒蝶のサイケデリカ (`PCSG00468`), アンジェリーク
    ルトゥール (`PCSG00696`), スカーレッドライダーゼクス Rev. (`PCSG00745`), 悠久のティアブレイド -Lost Chronicle-
    (`PCSG00808`) and -Fragments of Memory- (`PCSG01075`), マジきゅんっ！　ルネッサンス (`PCSG00852`), Starry☆Sky
    ～Summer Stories～ (`PCSG00916`), BLACK WOLVES SAGA -Weiβ und Schwarz- (`PCSG00935`), 紅色天井艶妖綺譚 二藍
    (`PCSG01221`).
  - Other engines ([#296]): キャプテン・アース Mind Labyrinth (`PCSG00450`), POSSESSION MAGENTA (`PCSG00509`),
    忠臣蔵46+1 V (`PCSG00540`), 剣が君 for V (`PCSG00553`), 月に寄りそう乙女の作法 (`PCSG00648`), 百花百狼
    ～戦国忍法帖～ (`PCSG00713`), 鏡界の白雪 (`PCSG00787`), Dance with Devils (`PCSG00807`), EVE burst error R
    (`PCSG00817`), 7'scarlet (`PCSG00876`), 参千世界遊戯 ～Re Multi Universe Myself～ (`PCSG01194`).
- A verified entry can have its own text filter and read its text through a pointer ([#292]). Rubi's
  filters turn ruby into its base, engine codes into line breaks or nothing and half-width kana into
  hiragana; a player-name code the hook sees before the game expands it becomes the game's default name,
  so a line with a name the player typed differs from the screen by that name only
  ([details](docs/native-hooks.md#rubis-filters)).

### Changed

- Triangle now shows the selected line's source (`hooking` / `ocr`) and two reading-stat blocks:
  **TODAY · THIS GAME** and **THIS GAME · TOTAL**, each with characters, estimated active reading
  time and characters/hour. Daily/game counters update in RAM with recorded dialogue and use
  per-game caches between restarts; technical line diagnostics no longer fill the menu ([#291]).

### On the console

- Not run on a console yet: the new entries are proven in Vita3K only, which says nothing about taiHEN's
  placement or memory on a real Vita, and Triangle's stats are host-tested only. If a game misbehaves with
  its hook, set **Text source** to **OCR only**, turn the game OFF on the Games page, or hold L as it starts.
- The game worker keeps Rubi's verified table in its data segment: the same pages as 0.9.6 in both builds
  ([details](docs/memory-ledger.md#tables-in-the-data-segment)).

## [0.9.6] - 2026-10-25

Hooked lines are read as soon as the game copies them, and a word's character name is shown below it
(scroll down with the right stick) instead of on an L/R page. Like 0.9.5, both the normal build and the
debug build are included; the normal VPK is the main download. API 41: install all of its plugins together
(the VPK's Install does).

### Changed

- The other dictionaries' entries for a word are shown below it instead of on L/R pages: with
  **Dictionaries shown** 2 (the default), 渚 shows the word, then Nagisa from the names dictionary
  below it; the right stick scrolls down to it, as it scrolls a long definition. L and R taps no longer
  do anything in the overlay. The extra entries are read with the word into the same result memory (no
  new allocation; one that does not fit is left out), and a long definition is clipped first so the
  ones below keep their room ([details](docs/local-dictionaries.md#dictionaries-shown), [#289]).
- A hooked line is read as soon as the game copies it (API 41). Once a hook answers without OCR (a Luna
  site, or a source Rubi has learned, such as CLANNAD's), the game worker sees the hook stream take a new
  line within 100 ms (est.) and tells the kernel, which wakes the Shell's background read at once instead
  of 300 ms after the line has finished typing, and holds that line's scene while it types out. So with
  **Read text** on **Automatically**, L + R as the line finishes usually opens on it, its first word
  already looked up, instead of waiting for a read. Lines in quick succession (a skip) wake nothing until
  the screen settles, as before, so a skip adds no line to the history. The Shell's code is unchanged,
  as are which text a job shows (and when OCR checks a hook) and **Only when I press**. The debug build
  logs each report in `kernel.txt` with its byte count, which shows whether a game's lines arrive whole
  ([details](docs/native-hooks.md#a-new-line-starts-the-read), [#286]).
- Holding **Left / Right** on the d-pad in the overlay now keeps moving the cursor, as Up / Down and
  the left stick already did: one step at once, then after 0.35 s ten words a second. A held arrow
  stops at the text's ends rather than going round a one-row text ([#285]).

### On the console

- Not run on a console yet: both changes are host-tested only (the held scene, the kernel's syscall and
  the worker's watch; the stacked entries' lookup and layout). This release needs its own restart and game test.

## [0.9.5] - 2026-10-25

Shows a word's character name as its second page, with the cleaned names dictionary, and adds the
reading totals on Triangle. Like 0.9.4, both the normal build and the debug build are included; the
normal VPK is the main download.

### Added

- Triangle in the overlay shows **total characters read, characters/hour and total time played**
  across recorded games. Time estimates active reading from adjacent same-game lines at most 15 seconds
  apart; longer pauses and game changes are excluded. Speed uses only timed lines' characters, and is
  `-` until an interval is available. Existing history is counted in background windows once, then kept
  in a 48-byte cache across restarts; old character-only caches rebuild automatically. No new thread or
  heap allocation is added. [Details](docs/ocr-history.md#total-characters-read).

### Changed

- A word that is also a character's name shows the name too: **Dictionaries shown** is 2 by
  default, so 渚 shows Bee's Ultimate Dictionary's entry (a beach) with `L/R Dictionary 2 of 2`, and
  R shows the name (Nagisa). Before, the word always won and the name never showed. A fresh install
  gets 2. An update that enables the names (the Rubi app's Install page, and now `tools/install_ftp.sh`
  too, which kept the old list and never added them) turns a count of 1 into 2; a count already
  chosen with the names enabled stays. With 2, an Anki card of such a word also carries the name in
  its `Glossary` ([details](docs/local-dictionaries.md#dictionaries-shown)).
- Another dictionary's page is always the same word: only an entry for page 1's whole match counts,
  so a name that is just the start of a longer word (空 in 空港) is no page, and the lookup searches no
  shorter span once page 1 is found. Page 1 itself is unchanged.
- The bundled names dictionary no longer has the one- and two-kana parts of names
  (bee-san/psp_and_vita_vndb_name_dict#1): the particles of foreign names (the デ and ラ of
  ファナ=デ・ラ・…) and honorifics (さん in 村上 さん) matched ordinary words such as で, だ and する.
  Whole names and kanji parts (渚) stay ([#283]).

### On the console

- The owner's Vita runs this release's Shell plugin and names dictionary (CI build of `ca73bd9`, copied
  by FTP over 0.9.4, whose other plugins are byte-identical); it has not been restarted with them yet,
  so no console run of the names page is reported. This release still needs its own restart and game test.

## [0.9.4] - 2026-10-23

Publishes the native-hook changes already merged into main after 0.9.3. Both the normal build
and the debug build are included; the normal VPK is the main download.

### Fixed

- Automatic text selection tries generic string-copy hooks in enabled native Vita games even
  when the game has no title-specific profile ([#275]). Existing source validation and OCR fallback remain.
- Nonempty text from an installed Luna table site answers before OCR ([#274]), avoiding OCR
  validation for that source. Generic CALL sources retain their learning and validation policy.
- アマツツミ has a verified entry-site recipe, and a verified entry can supply text when OCR
  cannot allocate its workspace ([#273]). Hook placement and readable-memory checks remain required.

### On the console

- The owner's initial CLANNAD run used matching debug plugins from main `56a831c`; hooked text
  matched the screen and hook-only readings were recorded. Later mixed-build readings do not
  establish coverage of these changes. This normal release still needs its own restart and game test.
- PSP OCR and the separate probe-v2 experiment do not establish PSP native hooking. The unmerged
  Adrenaline follow-up is outside this release.

## [0.9.3] - 2026-10-23

A second bundled dictionary, of the characters of PSP and Vita visual novels; the figures of a
failed OCR memory check in the overlay's error; hooked text without the game's control codes; and an
install on a console that refuses the Rubi app its own payload's data folders. Like 0.9.0–0.9.2, the
release is the normal build only.

### Added

- A second bundled dictionary, **VNDB Names — PSP & PS Vita** (`vndb-names.vjdict`, [#256];
  [details](docs/guide.md#settings-and-dictionaries)): 30,923 searchable name entries for 12,151
  characters from 1,202 visual novels (VNDB snapshot of 2026-10-22). A full name, a part of one or
  an alias shows the original and romanized name, the game, its platforms and the VNDB link; no
  images or biographies (an alias may reveal a name). A fresh install enables both dictionaries
  (`local_dictionaries = plaintext.vjdict,vndb-names.vjdict`). An update through the Rubi app's
  Install page adds the names after your own dictionaries, keeping their order, unless the list
  would then hold more than eight (it is left as it is, and the plan says so); `install-vita.sh`
  sets both. An update by hand from the ZIP that keeps your `config.ini` keeps its list: add
  `vndb-names.vjdict` to `local_dictionaries` to use the names. Bee's Ultimate Dictionary keeps
  first place for matches of equal length, and **Dictionaries shown** stays 1: set it to 2 to page
  between a word's entry and a name's with L or R. The VNDB attribution and licences
  (ODbL 1.0, DbCL 1.0) ship in the ZIP and the VPK ([attribution](docs/vndb-attribution.txt)).
  The file, 10,452,992 B, is copied to `ux0:data/VitaJPOverlay/dictionaries/`, and the app keeps
  its own copy, as it keeps the models.
- When OCR fails for lack of memory, the overlay's error shows the failed check's figures, in KiB
  ([#259]; [details](docs/guide.md#memory-and-troubleshooting)): the game's free memory, Rubi's
  reclaimable resident memory, and the total needed with its workspace, models and the reserve left
  to the game: `Game RAM (KiB): free F + reclaim R; need N (W workspace + M models + V reserve)`.
  These are the game worker's measurements at that check; the reserve is counted in the need but
  never allocated. A failed memory query shows its native code instead
  (`Game RAM unavailable (0x…)`), and an error before the check has no figures. The figures stay
  with their error when it is shown again, whatever a later job reports. The advice after them is
  shorter, to fit the existing status line (such as
  `Select a tighter dialogue region with Square.`). No allocation or thread is added.

### Fixed

- Hooked text drops backslash control codes such as `\s156` before stream selection and display
  ([#268]; [details](docs/native-hooks.md#which-text-a-job-shows)): `\s156「ばーーーっっ!」` is shown as
  `「ばーーーっっ!」`, so the overlay, mined sentences and new reading-history records omit them, and
  reading statistics no longer count their letters and digits. Numeric `\s` and `\w` commands go
  with their argument, a standalone `\c`, `\k`, `\p` or `\f` goes, and a standalone `\n`, `\r` or
  `\t` becomes whitespace; unknown escapes, doubled backslashes and words such as `\new` are kept,
  and nonprinting ASCII controls other than tab, CR and LF are dropped. It applies to the
  string-copy (CALL) and table-site streams alike, after a site's own filter. OCR and existing
  history files are unchanged.
- The Install page installs on a console that refuses the Rubi app its own folder's payload data
  ([#257], by a contributor). On one PCH-2000 (3.65, Ensō, StorageMgr with ux0 on an SD2Vita,
  rePatch, reF00D, NoNpDrm, Adrenaline), `ux0:app/VJOSET001/payload/ux0/data/…` and `…/ur0/data/…`
  returned `0x80010016` to the app, before and after `app0:`'s unmount, while `app0:payload/…` read
  every file: 0.9.2's Install page said the package's `config.ini` template was missing or damaged,
  which reinstalling the VPK cannot fix. The scan and the install now read such a file through
  `app0:`. When any payload file reads only that way, the install keeps `app0:` mounted, copies
  every file from it, and copies the dictionary instead of moving it, so on such a console the app
  keeps its 157 MB copy too. A console whose folder reads installs as 0.9.2 did. The refusal's cause
  is unknown. Still open ([#260]): the payload's manifest is read through the folder only, and the
  fallback is chosen by `sceIoGetstat`, not by a read that succeeded.

### Changed

- The README's and the guide's installation instructions are six steps from the ZIP ([#258]): copy
  the three plugins to `ur0:tai`, add the kernel and Shell lines to taiHEN's `config.txt` by hand,
  copy the `ur0` and `ux0` data folders (keeping your `config.ini` when updating), install the app's
  `Rubi.vpk`, and reboot. The standalone VPK with its Install page is the guide's alternative.
- The downloads grow by the names dictionary: the VPK and the ZIP by about 2.2 MB each, and the
  VPK's install needs about 270 MB free on ux0 while the VPK is on the card, about 23 MB more than
  0.9.2's [estimate: the 245 MB that docs/one-vpk-install.md derives, plus the names file unpacked
  from the VPK and copied out of the app, 10.5 MB each, and the VPK's own growth]. The ZIP's `ux0`
  data folder is 179.5 MB, 10.5 MB more than 0.9.2's.
- No module's pages change. The Shell's stay at 163,840 B (176,128 B debug), the kernel plugin's at
  118,784 B (122,880 B debug), and each game that is ON carries 172,032 B of the OCR worker's
  (180,224 B debug). #256's default dictionary list had taken the normal Shell's code from 90,100 B
  to 90,116 B, 4 B past its last page, so its Vita build failed the memory ledger's check; #259's
  messages, moved into the data segment, bring it back to 90,004 B [measured: the build logs' module
  rows, Release run 39928577888 at `2f85de0`, Release run 39755005256 at `15e028e` and Release run
  39845048444 at `708e84b`; `docs/memory-ledger.md`].

### On the console

- The contributor's PCH-2000 above ran #257's fix, built as the normal build at 0.9.2's tree,
  before the names dictionary ([#257], the contributor's report): the scan passed, the install kept
  `app0:` mounted, copied the dictionary and finished with `install: done (v0.9.2 normal)`, all 17
  installed files matched the manifest, `ur0:tai/config.txt` gained the two lines after its backup,
  and after a restart both plugins loaded with no errors (`boot.txt` `stage=3 fault=0 failures=0`,
  `status.txt` `Rubi shell started … worker started`). The overlay was not tried in a game, and the
  fix as merged, with the names file among the payload's files, has not run there.
- Users have reported OCR refusing for lack of memory in some games after a fresh install ([#266],
  アマツツミ and あおかな; not reproduced, with no title IDs or memory figures yet). The figures of #259
  are there to show why; they have not been seen on a console yet.
- None of 0.9.3's other changes has run on a console: the names dictionary, the memory figures and
  the control-code clean-up, nor this release's build; the owner's console has run no normal build.
  The native hooks' console record is still 0.9.0's below, CLANNAD's string-copy reads in the
  combined debug test build; the test.7 checklist ([#237]) was closed without reported results, and
  no site from LunaTranslator's table has been reported working on a console.

## [0.9.2] - 2026-10-22

Where each line of `history.csv` came from, with reading statistics by text source; a held ▲ or ▼
that repeats; a hidden view, on △, of where the shown line came from; and two fixes to OCR's text, a
counter's `ヶ` and its numeral, and no box border read as a line. Like 0.9.0 and 0.9.1, the release is
the normal build only.

### Added

- `history.csv` says where each line came from ([#251]): a new last column, `line_source`, is `hook`
  when a native text hook supplied the line and `ocr` when Rubi's OCR read it, the same choice as the
  `engine` column's `hook` or `vocr`. It is the 43rd column, after `characters`: the first five
  (GameSentenceMiner's ExStatic lines) and every other column keep their places, and a file an older
  Rubi began keeps its header: its new rows have more cells than it names. GameSentenceMiner's import
  reads it as before; pandas' `read_csv` stops at the first new row of a file 0.9.0 or 0.9.1 began,
  which it read before, until one `sed` line adds the new name to the file's first line
  ([details](docs/ocr-history.md#the-format-and-older-files)). `tools/reading_stats.py` adds **By
  text source** (native text hook, OCR) to its totals, and reads the source of an older Rubi's rows
  from their `engine` ([docs/ocr-history.md](docs/ocr-history.md#line-source)). Hook lines were
  recorded before too (with **Read text: Automatically**); what is not recorded, from either source,
  is unchanged, and so is a known fault of earlier releases: after a read whose exchange with the game
  worker failed, the next read of the same text is recorded again, and counted twice ([#252], open).
  No popup change. The Shell's code is unchanged in size and its data 8 B smaller, in the same pages
  (`docs/memory-ledger.md`).
- A hidden view of where the shown line came from ([#253];
  [details](docs/native-hooks.md#hidden-line-diagnostics)): with the overlay open, △ (Triangle)
  replaces the dictionary entry with the line's source, **OCR** or **Hook**, and for a hook its stream,
  whether a table site or the CALL source supplied it, its agreement, lock and pin flags, its similarity
  to OCR's reading, the streams available, the hooks installed and refused, the model and the capture;
  △ again brings the entry back. The text and its cursor stay, ▲ ▼ still go through earlier lines, and
  the right stick scrolls the view. An earlier line shows the engine its `history.csv` row saved
  (**Unknown** when the row has no readable engine) and says the rest was not saved. The source is the
  one that supplied the text: a cursor move, or a refresh that keeps the text or is cancelled, does not
  change it. The view is in both builds, named in no hint, off at every opening, and changes no text
  source (test.7's △ source picker stays out). The Shell's pages are unchanged in both builds: the
  header's 2,048 B of ranges moved from its permanent data into the overlay's existing 20,480 B text
  block, which now needs 20,064 B of it, and cold strings into the data space freed
  ([measured](docs/memory-ledger.md#hidden-triangle-line-diagnostics)).

### Fixed

- A counter read as `ッ` is `ヶ` again (`ルッ月です」` → `ルヶ月です」`; [#249]): a text rule in the
  shown and looked-up text's normalisation (`core/textnorm.c`). Before the LM, the job puts back a
  numeral or `「` that the frames hold close below what the recognizer read before a counter, so `ル`
  becomes `九` (vita-vn-ocr #109's counter re-decode, `runtime/vocr_post.c`; pin bumped to `817d203`).
  On vita-vn-ocr's development data neither changes a line; see
  [docs/vita-vn-ocr.md](docs/vita-vn-ocr.md#counters-a-numeral-or-a--the-line-lost-vita-vn-ocr-109).

### Changed

- A held ▲ or ▼ in the overlay repeats ([#250]): one step at once, then, after 350 ms, one every
  150 ms, through the text's rows and on through this game's earlier lines, back and forward
  ([line history](docs/line-history.md)). The pace is steady: at most one step a frame, and no burst
  of steps after a slow frame. Releasing the button stops it, and holding both repeats neither; a
  button already held when the overlay opens moves nothing until it is pressed again, and the region
  selector (□) clears the repeat. Before, a press moved one step however long it was held. Three of
  the Shell's error strings moved to its data segment, so that its code fits the same pages; no
  allocation or thread is added.
- Each game that is ON carries 4,096 B more of the OCR worker's module pages, for the counter re-decode
  ([#249]): 172,032 B in the normal build, 180,224 B in the debug build. The kernel plugin's pages stay
  at 118,784 B (122,880 B debug) and the Shell's at 163,840 B (176,128 B debug) [measured: the build
  logs' module rows, Release run 39735440794 at `4d73121`; `docs/memory-ledger.md`].
- A line that is only a text box's border or scene art read as text, such as the `ーニーーーーニーニー…`
  a box's decorative bottom edge reads as, is no longer shown, saved or looked up ([#247]): three or more
  characters, each one of `ー ニ 一 二 ― - ~ ・`, at least two different ones, `ー` or `ニ` among them, and
  nothing else on the line. It follows `non_japanese_filter` in `config.ini`: `lines`, the default, drops
  such a line, `none` keeps it. On vita-vn-ocr's development data (postproc-research's R08d) it removed
  the one such line there, 35 characters in 1 of 1,161 whole-dialogue units, on every model size, and
  changed no other line: whole-dialogue CER 5.06 → 4.92 % with Normal (3.15 → 3.02 % in the Adrenaline 2×
  view a PSP game has). On correct text it dropped none of 1.6 million corpus lines, and 1 of 8.8 million
  pieces of them wrapped as a text box wraps
  ([details](docs/vita-vn-ocr.md#text-normalisation-shown-and-looked-up)). Not run on a console yet.
- The plugins use API 36 ([#247]): an OCR job's request carries that choice (`VJO_GAME_OCR_S_DROP_BORDERS`, bit 16
  of its `speed` word), and the debug build's `gstep job norm` step counts the lines dropped as its `kind`
  (`tools/analyse_selftest.py` says so). The game worker does the work; no module's pages change
  ([measured](docs/memory-ledger.md#border-lines-dropped-api-36)).

### On the console

- None of 0.9.2's changes has run on a console: the `line_source` column and the statistics by text
  source, the △ view, the held ▲ ▼ repeat, the counter rule and its re-decode, the border-line drop,
  and the normal build itself. The native hooks' console record is still 0.9.0's below, CLANNAD's
  string-copy reads in the combined debug test build; the test.7 checklist ([#237]) has no reported
  results, and no site from LunaTranslator's table has been reported working on a console.

## [0.9.1] - 2026-10-21

Native text hooks that Rubi remembers for each game build and checks less often as they prove
reliable, and the conjugation of a looked-up word in its dictionary entry. Like 0.9.0, the release
is the normal build only.

### Added

- A dictionary entry names the conjugation of the word looked up: under the headword, in blue,
  `Conjugation: polite past` for 食べました (found as 食べる), `Conjugation: negative → past` for
  食べなかった; a word in its dictionary form gets no such line. The reasons are those of the
  Yomichan rules that found the entry, and stay with each word through dictionary paging and
  prepared lookups; a chain of more than four shows `…` before the three nearest the written word.
  Definitions and Anki cards are unchanged ([#243]; [details](docs/conjugation-display.md)). Its
  previews are host renders: the Vita's font and wrapping of the line are unchecked.
- Remembered hooks (API 34; [#242], [#241]): a hook that has proved reliable is kept for each game
  build in `ux0:data/VitaJPOverlay/hooks.cache`, at most 32 games in 3,088 B (by its format): the
  source's kind, its offset in the game's module, its encoding and its check interval, never dialogue.
  An entry is keyed by the title ID, the SHA-256 of the game's whole executable, the text source and a
  revision of Rubi's hook definitions, so a game update, another text source or changed hooks learn
  afresh, as does a missing, damaged or unwritable file. At the game's next start, one agreement with
  OCR resumes the saved interval. The game's worker hashes the executable and reads the entry; the
  kernel's existing file thread writes the file. Nothing new runs in SceShell.

### Changed

- How **Automatic** trusts a native text hook ([#242]). A hook's line still replaces OCR's only when
  the two agree (70 % or more, four characters at least), but trust now takes agreement on two
  different dialogue lines (one line read twice does not count). Then the hook answers seven new
  readings without OCR and OCR checks the eighth; each check it passes widens the interval to 16, then
  32 readings (0.9.0's OCR read at least every third page). Reopening the overlay on unchanged dialogue
  is not a new reading. One disagreement shows OCR's text and checks the next new line, where an
  agreement resumes at 8; two disagreements in a row drop the hook until it earns trust again. An empty
  or too-short OCR reading counts neither way, and a screen without the hook's text (a menu, a blank
  screen) keeps what it learned. The cost: a hook that turns wrong can show up to 31 readings before a
  check catches it.
- Each game that is ON carries a bigger OCR worker: 167,936 B of module pages in the normal build and
  176,128 B in the debug build, 8,192 B more than 0.9.0's. The kernel plugin's pages grow 4,096 B, to
  118,784 B (122,880 B debug); the Shell's stay at 163,840 B (176,128 B debug), the conjugation line
  included [measured: the build logs' module rows, Release run 39581967210 at `d428c55`].
- The README says which games have native text hooks and how **Automatic** picks their text ([#239]):
  CLANNAD, 175 title IDs from LunaTranslator's table (294 ported sites, none verified on a console),
  the string-copy hooks of **Every game** elsewhere, and no PSP hooks. The guide, the design and the
  hook list say the same about what the console has shown.

### On the console

- None of 0.9.1's changes has run on a console: the remembered hooks and their check intervals (how
  often OCR runs while reading, reopening and restarting, the boot and the CPU cost are unmeasured),
  the conjugation line, and the normal build itself. The native hooks' console record is still
  0.9.0's below, CLANNAD's string-copy reads in the combined debug test build; the test.7 checklist
  ([#237]) has no reported results, and no site from LunaTranslator's table has been reported working
  on a console.

## [0.9.0] - 2026-10-20

Native text extraction, dialogue history, reading statistics and Anki screenshot trimming.
The normal build omits the debug traces and on-device self-test; the overlay keeps all user features.

### Added

- An optional small Adrenaline Shell bridge, `compat/adrenaline_vsh.suprx`, in the release zip
  ([how to use it](compat/adrenaline_vsh/README.md)), for an Adrenaline or PSP bubble start that goes
  straight back to LiveArea: on the owner's console, Adrenaline's own bridge did not fit in
  SceShell's memory (`9e36b5e`).
- An opt-in text-presence gate (`ocr_text_gate`, off by default; API 26): a region it scores as
  text-free returns no line. On the host it did so for 59 of 66 text-free regions, against 5 without
  it ([#214]).
- The Rubi app's Games page shows five games a page and searches by title or title ID ([#219]).
- Native text hooks (API 31; [the guide](docs/guide.md#native-text-hooks),
  [the design](docs/native-hooks.md), [#230]): in a game that is ON, the game's worker can read the line
  the game is about to draw instead of OCR's reading. CLANNAD's string-copy source was verified on
  the owner's console in the combined test build; table sites remain unverified on hardware.
  - What is hooked: LunaTranslator's Vita table, 294 ported sites for 175 title IDs, each installed only
    where the game's code at that address is a plain function entry that taiHEN can patch, and, where the
    table names a game version, only in that version ([the games](docs/hook-titles.md)); Rubi's own entry
    for CLANNAD (`PCSG00415`, one site, proven in Vita3K only); and the CALL source, the game's string copies (SceLibc's
    and SceLibKernel's memcpy, strcpy and their kin) grouped by the caller. **Automatic** hooks only the
    listed games; **Every game** adds the CALL source in every game that is ON. `hook_sites = off` in
    `config.ini` installs no table site, only the CALL source, so a console run can tell the two apart.
  - Which text is shown: OCR still reads every page at first; a hook's text replaces OCR's only when
    the two agree (70 % or more, 4 characters at least). After two agreements in a row the hook is
    locked: it then answers new lines of 4 characters or more without recognition, at most 2 pages in a
    row, and OCR reads the next page, where the hook's text must agree. A disagreement makes OCR read
    every page until the hook agrees again; two in a row unlock it, and so does a change of the caller
    behind a CALL stream. So a hook that runs ahead of the screen, or turns to a name or a menu's text,
    shows at most 2 wrong pages.
  - The controls: `text_source = auto | hooks | ocr` (the Rubi app's **Text source**, Automatic by
    default, applied when a game next starts), which the Shell hands the kernel and a game's worker
    reads as the game starts. The popup selects its text automatically and keeps source diagnostics
    in logs; the Games page tags a game with a hook entry (`Hook`: CLANNAD, Rubi's own; `Luna hook`: a title with
    a site Rubi has ported from LunaTranslator's table; the table's 7 titles without one, PCSB00985,
    PCSG00291, PCSG00451, PCSG00595, PCSG00663, PCSG01081 and PCSG01152, get no tag and OCR only in
    Automatic). `log.txt` says what each game's worker hooked (counts only in the normal build),
    `history.csv`'s engine is `hook` for a hook's line.
  - A game turned OFF gets no hooks, even when a `*TITLEID` line kept by hand in taiHEN's config still
    loads the worker into it: the kernel gives the text source only to a game whose worker it loaded as
    ON. Holding L as a game starts, `text_source = ocr` and a safe boot also install none.
  - The cost: every game that is ON carries the bigger worker whatever the text source, even with
    `text_source = ocr`: its module pages grow from 98,304 to 159,744 B (167,936 B in the debug build)
    [measured: the build logs' module rows, `feat/native-hooks` 9fa24c5]. A hook's own CPU time is
    unmeasured.
- Anki screenshots without black bars: with **Trim black bars** (`anki_screenshot_trim_bars`, on by
  default; API 32), a card's picture leaves out the letterbox or pillarbox around the game's picture, as
  Adrenaline shows a PSP game smaller than the screen (at 1×: 484 × 276 of 960 × 544). The game worker
  finds the bars as it reads the rows and encodes the picture's part of the same rows, with no copy of the
  frame; a frame without bars gives the same JPEG as before, byte for byte. On the host, with local copies
  of the owner's frames: none of 1,727 Vita frames trimmed, 282 of 300 PSP frames at 1× and 1.5×, no pixel
  that is not black left out ([details](docs/anki.md#black-bars)). Not run on a console yet. The Rubi
  app's Anki page has the switch; its five rows scroll.
- `history.csv` has a `characters` column: each line's kanji, kana and ASCII letters and digits,
  without punctuation, brackets, symbols, spaces or line breaks (reading trackers such as
  GameSentenceMiner and ッツ Ebook Reader leave punctuation out too). `tools/reading_stats.py` gives
  the characters read and per hour, in all, per game and per day, from new and 0.8.0 files
  ([#233]; [docs/ocr-history.md](docs/ocr-history.md)).
- Earlier lines: in the overlay, Up on the text's top row shows this game's line before it, focused
  between its dimmed neighbours, for lookups and cards (a card from an earlier line has that line as its
  sentence and no screenshot); Down comes forward again. Only with **Read text: Automatically**, from
  the lines `history.csv` keeps: its last 24 KiB, at most 32 lines, in a 16 KiB Paf block while you
  look back ([line history](docs/line-history.md)).

### Changed

- An install or an update no longer writes `text_source = ocr` into `config.ini` (API 31: the setting
  is the user's again); a file that has it keeps it, so choose Automatic once to use native text hooks.
- **Japanese characters only** (`japanese_only_selection`) is OFF by default, as the owner asked:
  the cursor can select any character, Latin letters, digits and punctuation too, and starts at the
  text's first character. A `config.ini` keeps its own line: from 0.7.6 the template wrote
  `japanese_only_selection = on`, so an install updated from an earlier build stays ON until it is
  turned OFF on the Rubi app's OCR & lookup page ([#231]).
- Every verified install deletes the app's own copy of the dictionary when the app's folder can be
  written, and says so on the done screen and in `log.txt` ([#200]).
- A card's screenshot is written over a file set aside for it beforehand: on the owner's console,
  after a restart, the first write to a new file took about 21 s, so a card took that long or was
  saved without its picture ([#208]; [#207], [#210]).
- A queued card whose screenshot is missing or damaged is exported without it, and the export says
  so; before, it stayed in the queue ([#218]).
- The plugins use API 33, combining native hook selection and screenshot trimming in one matched
  protocol. API 31's hook fields and API 32's standalone trimming build cannot be mixed.
- The combined build retains the native-hook worker's 167,936 B debug / 159,744 B normal module
  pages. Compact highlight ranges and fixed tables in existing data-page space keep the Shell at
  176,128 B debug / 163,840 B normal, with the existing PAF reserve unchanged
  ([binary measurements](docs/memory-ledger.md#combined-enhancement-test-build-local-api-33)).
- The popup shows dialogue without hook numbers or source-status controls. Source diagnostics stay
  in the logs, and ordinary text reads select their source automatically.
- The README shows the promo film ([#206]) and says what the console runs showed ([#204]); its build
  history is now this changelog ([#224]).
- The left stick goes a character at a time over the recognized text, in Whole word mode too, and
  looks up the word that starts where it stops (on 昨日すき焼きを食べました, from 食べました to べ,
  then ました); the d-pad still goes word by word. The stick's left no longer retraces the d-pad's
  Rights, so it no longer undoes a d-pad Right that went round a one-row text ([#232]).

### On the console

- 2026-10-20, combined API 33 debug build on the owner's PCH-1001: plugin readbacks matched the
  package and the Vita completed a normal boot. The owner confirmed CLANNAD's popup text, Up/Down
  history, mining an older line, and returning to current dialogue after closing history. The latest
  captured session contained five successful native reads with OCR recognition skipped. All 19 new
  history records' stored character counts matched a desktop recount. This is not a hardware test
  of every Luna table site or of black-bar trimming, and the normal build remains hardware-unmeasured.
- 2026-10-17, with the small bridge: Boku no Natsuyasumi 4 started from its LiveArea bubble, and the
  kernel loaded Rubi's OCR worker into the bubble's PSP emulator process (`PSPEMU004`). Reading text
  in a PSP game is still untested.
- A build made to measure the screenshot fix ([#207]): the first screenshot after a restart took
  719 ms to capture and encode and 74 ms of file work, and saving a card with its screenshot took
  523–905 ms. The finished fix has not run on a console yet.

## [0.8.0] - 2026-10-17

The first release as Rubi, published by the owner from `9ea6542` (API 25; vita-vn-ocr runtime
`d214a05`). Since test.4 it adds the fixes of the owner's test.4.1–4.3 runs, the rename and
prepared lookups; everything else new since 0.7.12 came in the test builds below.
[Release notes](https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.8.0).

### Added

- Prepared lookups: with Read text on Automatically and Whole word highlighting, idle time prepares
  the lookups of up to eight upcoming words, and dictionary handles are kept for the current dialogue
  ([#203]; measured as host operation counts, not timed on a console).

### Changed

- Renamed to **Rubi** (Rubi for Vita; [#189], [what changed](docs/rename-rubi.md)). The app was
  **Overlay** in the published 0.8.0 test builds and **JP Overlay Settings** before (0.7.12 and
  older). Its title ID (`VJOSET001`) stays, so the new VPK installs over either and keeps your
  settings, games and Anki cards. The data folders, the plugin files and the Anki deck keep their old
  names.
- The main download is the normal build, `Rubi-0.8.0.vpk`; `Rubi-0.8.0-debug.vpk` is the test
  builds' build, with the full log and the self-test. Each is also a zip, for the computer installer.
- The README became a short front page, and the old README is now the guide, `docs/guide.md`
  ([#176]). The 42-second promo film is in `docs/promo` ([#193]).

### Fixed

- A dialogue box's short last line is read: the textbox recovery finder for every model (`6674125`)
  and a short-line probe after the finder (`49310ed`).
- A card's screenshot is saved: the kernel's save syncs nothing (`b622c18`).
- When the app cannot read its own folder yet, right after VitaShell installs it, the Install page
  says `Restart the Vita first`, and it logs every reason it shows (`b6755cf`).
- The kernel changes the active game only under its lock ([#190]).

### On the console

- Its release notes say its lookup changes ([#203]) were not installed or timed on a console. Its
  fixes ran in test.4.1–4.3 (below), except the Install page's `Restart the Vita first`, whose case no
  console run has hit yet ([#194](https://github.com/bee-san/Rubi_for_vita/issues/194)).

## 0.8.0-test.5 - cancelled

Planned after test.4 to check its fixes and run what test.4 had not: Small, Read text on a press,
the boost off, the Games page's Default OFF, a PSP game, and the dictionary's grammar and kanji lines
as steps of their own. It was cancelled when 0.8.0 shipped, and no test.5 was published
([#191](https://github.com/bee-san/Rubi_for_vita/issues/191),
[#194](https://github.com/bee-san/Rubi_for_vita/issues/194)).

## 0.8.0-test.4.3 - 2026-10-17

Not published: `main` at `b622c18`, installed on the owner's Vita
([#194](https://github.com/bee-san/Rubi_for_vita/issues/194)). Its app is already named **Rubi**
([#189]).

### Fixed

- The screenshot's save closes and renames its file and syncs nothing (`b622c18`): test.4.2's sync of
  the file (`7f40e7f`) had turned the console off.

### On the console

- Installed by the Install page's **Update**, the third Update on a console: test.4.1, 4.2 and 4.3,
  each over the build before it.
- A card with its screenshot saved in 851 ms (test.4: 23,748 ms). Its `.apkg` note's Picture is the
  game's 960 × 544 frame at the press, and the export removed the screenshot file that test.4.2's
  crash had left.

## 0.8.0-test.4.2 - 2026-10-17

Not published: test.4.1 with a short-line probe after the line finder (`49310ed` on `main`), for
which the debug build's OCR worker grows one page, to 98,304 B (`1f113ad`)
([#194](https://github.com/bee-san/Rubi_for_vita/issues/194)).

### On the console

- Installed by the Install page's **Update**. The short last line `た。` that test.4.1 missed is read.
- Saving a card with Screenshots on powered the Vita off. Its kernel log ends right after the
  screenshot's write, before the save's first call, `7f40e7f`'s sync of the file (fixed in
  test.4.3).

## 0.8.0-test.4.1 - 2026-10-17

Not published: test.4 with three fixes from its run
([#194](https://github.com/bee-san/Rubi_for_vita/issues/194); on `main` as the commits named): the
textbox recovery finder for every model (`6674125`); `Restart the Vita first` on the Install page,
and its reasons in `log.txt` (`b6755cf`); and the screenshot's save syncing only its own file, its
stages traced in `kernel.txt`, and a give-up after 8 s (`7f40e7f`).

### On the console

- Installed by VitaShell (its own installer, started over its FTP connection) and then the Install
  page's **Update** (`Update the overlay from v0.8.0-test.4 to v0.8.0-test.4.1`): the Update's first
  console run. It wrote the three plugins, and Check and repair then reported it installed.
- One short last line (`ていた。`) is read; another (`た。`) is still missed.
- PS-button screenshots were saved; no Anki card was saved in this run.

## [0.8.0-test.4] - 2026-10-17

The last published test build before the release, and the first made to be installed as one VPK
(`VitaJPOverlay-0.8.0-test.4-debug.vpk`): `main` at `3ce0f47`, API 24. It still has the old names:
the app is **Overlay**. Install steps:
[#191](https://github.com/bee-san/Rubi_for_vita/issues/191); the owner's report:
[#194](https://github.com/bee-san/Rubi_for_vita/issues/194).
[Release notes](https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.8.0-test.4), with the full
list.

### Added

- The one-VPK install: the VPK carries the whole install, and the app's Install page installs,
  updates, checks and repairs, and uninstalls ([#158] and the PRs after it).
- VOCR5 as the three model sizes, Small, Normal (the default) and Large, with the 1 MB character
  language model ([#167]).
- Bee's Ultimate Dictionary ([#153]), with Jiten's frequency ranks (53.52% of its entries are ranked;
  the rest show `unranked`) ([#127]), and fewer file reads per lookup ([#140]; host).
- Anki cards keep a screenshot of the game, made in the game's own process ([#150], [#164], [#174]).
- **Read text** (Automatically, or Only when I press L + R) and **Speed boost while reading
  (444 MHz)** in the app ([#146]).
- Two builds: the normal build (short log, no self-test) and the debug build ([#141], [#147], [#155]).
- Automatic reading keeps each new dialogue text in `history.csv` ([#187]).
- Adrenaline Bubble Manager bubbles are listed as PSP games, under their game's title ([#124], [#170]).

### Changed

- The kernel plugin loads the OCR worker into every game that is ON, Adrenaline included: no
  taiHEN line per game, no `--title` ([#148]).
- The app's Games list lists every title `games.ini` names, and Default OFF turns every game OFF
  ([#149]).
- The normal build makes no log append on the way to a result ([#154]).
- The left stick moves the cursor fast and looks a word up once it rests ([#156]); Left and Right cross
  lines and stop at the text's ends ([#173]).
- A word split over two lines is looked up whole, and the overlay breaks its own lines between
  phrases ([#178]).

### Removed

- Google Lens and the whole network stack: Rubi is offline only ([#130], [#132], [#145], [#157]).

### Fixed

- Two possible causes of test.3's app not opening with no game running are handled, and the app logs
  how its start went ([#181]).
- No `Recognizing…` left on screen after a cancelled lookup ([#95], [#179]); config warnings never quote
  a value ([#185]); the subtitle strip after a new region or a game switch ([#182], [#186]).

### On the console

The owner's PCH-1001 with CLANNAD, 2026-10-17:

- VitaShell's first install stopped with error `0x80101114` while test.3's app was still open,
  suspended behind it; after a restart the VPK installed.
- The app, opened straight after, refused to install (`Cannot install the overlay (state 4)`), and
  an Uninstall did not change that; the lead, unconfirmed, is that it could not read its own folder
  until a restart. The install was finished by hand over FTP (since test.4.1 the page says `Restart
  the Vita first` in that case).
- After the restart the app opened from LiveArea with no game running, though neither case [#181]
  handles came up, so why test.3's app did not open is still unexplained
  ([#191](https://github.com/bee-san/Rubi_for_vita/issues/191)); Check and repair wrote nothing.
- The kernel plugin loaded the OCR worker into CLANNAD with no taiHEN line for it.
- VOCR5 Normal took 2.61 ms per CTC frame and Large 4.98–5.10 ms, both at 444 MHz (over the
  analyser's 10 and 8 jobs; Normal ran 15 jobs, Large 11). Large never fell back to a smaller size:
  CLANNAD had 87.0–91.2 MB free. Every job that recognized a line raised the clock
  333 → 444 → 333 MHz.
- The dictionary's popups, the left stick and the export of 20 cards worked.
- Both cards' screenshots timed out after 15 s, while the game froze and its audio cut in and out;
  the saves took 17.6 and 23.7 s (fixed in test.4.3).
- A box's short last line was dropped by the line finder, as in test.3 (fixed in test.4.1 and 4.2).
- Two PS-button screenshots were not saved, and the game seemed frozen while they were taken; the
  logs show no capture of Rubi's at that time ([#194]).
- Not run: Small, Read text on a press, the boost off, Default OFF, a PSP game, and the dictionary
  steps.

## [0.8.0-test.3] - 2026-10-16

One zip, installed with `install-vita.sh --title` and the app's VPK: `main` at `9cfa920`, API 22.
Install steps and results: [#138](https://github.com/bee-san/Rubi_for_vita/issues/138).
[Release notes](https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.8.0-test.3).

### Added

- Model sizes in the app: Small (VOCR3-FL10), Normal (VOCR3-H15, the default) and Large (VOCR4-W20),
  with a smaller one taken by itself when a game is short of memory ([#108], [#105]).
- Faster OCR: up to 3 lines at once, a cache of 8 lines, and 444 MHz while a job reads ([#93]).
- A fast first scan: the self-test runs in a background job of its own, and each model file is read
  once per game process ([#116]).
- Anki: the app exports the queued cards as one `.apkg` of Lapis notes ([#125], [#133]).
- The app and the plugins check that they are one build ([#122]).

### Changed

- The app is a system application: it opens beside a suspended game ([#109], [#112], [#129]).
- Enter and Cancel follow the console's own setting ([#106]).
- A key filter in the dictionary saves about 6 file reads per lookup ([#123]; host).

### Removed

- AnkiConnect ([#125]); the online dictionaries jpdb.io and jiten.moe, and with them the subtitle
  strip's furigana, which only they gave ([#117]); and the Hachidori relay ([#126]).

### On the console

The owner's PCH-1001 with CLANNAD, 2026-10-16:

- The self-test passed every stage. The first scan showed its text in under 1 s; later scans took
  about 2 s, then under 1 s. Recognition took 45–406 ms a job and the line finder 104–494 ms; 22 jobs
  raised the clock 333 → 444 → 333 MHz.
- While Rubi read, the game's frames came late: in the 28 jobs that logged the game's frame pacing,
  291 of 1,609 frames came more than 25 ms after the one before, against 52 of 26,941 in the time
  before each job ([details](docs/auto-vs-press.md)).
- All three sizes ran: Small felt about as fast as Normal, sometimes instant (most likely lines from
  the line cache), and Large was clearly slower; Large never fell back to a smaller size.
- A cursor step's lookup took 35–112 ms (median 56 ms, 31 lookups).
- 7 cards were saved, none failed, and exported as one `.apkg` of 7 Lapis notes (checked on a
  computer, not imported into Anki).
- PS with the overlay open worked ([#104]). The app opened beside CLANNAD and closed cleanly, but not
  from LiveArea with no game running (the owner's report; why is still unexplained).
- AIR (`PCSG00940`) got no OCR: this build loaded the worker only into a game with its own taiHEN
  line, and AIR had none; its jobs ended `rc=-117`. The tester reported the same in Adrenaline ([#148]
  changed this in test.4).
- CLANNAD was missing from the app's Games list, and Default OFF did not turn it off ([#149] changed
  this in test.4).
- Log appends took up to 4.4 s of an 8.6 s job (since test.4 the normal build makes none on the way
  to a result, [#154]).

## [0.8.0-test.2] - 2026-10-15

`main` at `4ac02ac`, API 20. Install steps and results:
[#101](https://github.com/bee-san/Rubi_for_vita/issues/101).
[Release notes](https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.8.0-test.2).

### Added

- test.1's six requests: Up and Down by displayed rows (so local dictionaries now page with an L or R
  tap instead) and Left and Right wrapping at a row's ends ([#87]); the OCR box drawn with the stick
  ([#86]); no dialogue-changed warning ([#92]); VOCR3 and Google Lens as the two engines ([#85]);
  Anki set up in the app ([#90]).
- Typing text is read once it is whole ([#84]), and each mark is shown and looked up in one form
  ([#88]).
- Beam search with a 1 MB character language model, and a cascade that reads a doubtful line again
  with a larger model ([#89]).
- The line finder's boxes are refined in PSP games ([#94]).
- A smoother Games list in the app: icons cached, L and R jump a page, Left and Right jump to the
  previous or next letter, and Start rescans ([#96], [#99]).

### Removed

- VOCR2 ([#85]).

### On the console

The owner's PCH-1001 with CLANNAD, 2026-10-15:

- The self-test passed every stage, and all 16 installed files matched the package.
- Up and Down by rows, Left and Right wrapping and the stick-drawn box worked; no dialogue-changed
  warning was seen.
- The run used test.1's app, so the engine choice and the Anki page were not tested fairly, and the
  old app wrote `ocr_backend = vocr2` into `config.ini` (since test.3 the app and the plugins check
  that they are one build, [#122]).
- The first scans took 9–15 s, because the self-test ran inside them ([#116], in test.3, whose first
  scan took under 1 s); later scans were faster.

## [0.8.0-test.1] - 2026-10-14

`main` at `b548dc9`, API 19. Install steps and results:
[#81](https://github.com/bee-san/Rubi_for_vita/issues/81).
[Release notes](https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.8.0-test.1).

### Added

- VOCR3 as the default model (VOCR3-H15, falling back to VOCR3-FL10) and logging on by default
  ([#77]); Anki cards saved offline to the memory card, then sent to AnkiConnect on the local network
  ([#49], [#77]).
- The frozen Daijirin4 → Sankoku8 → JMdict dictionary ([#73]), several local dictionaries paged with
  Up and Down ([#51]), and ranked entries with one `log.txt` line per lookup ([#79]).
- The boot-safety guard: a boot that does not complete makes the next one a safe boot ([#63]).
- The on-device self-test ([#66]), and `--collect` with the memory headroom of each stage ([#78]).
- The memory ledger: every allocation named and budgeted, and checked in CI ([#64]).

### Changed

- The memory fixes since 0.7.12: SceShell's pages given back ([#68]), the OCR worker loaded per game
  ([#72]), the screen read in the game in grey strips ([#75]), and the memory routes logged ([#76]).
- The settings app is named **Overlay**, with a new icon ([#71]).
- Upstream's 0.7 merged in: Anki note fields and the furigana strip ([#48]).

### Removed

- The Meiki engine ([#50]).

### On the console

The owner's PCH-1001 with CLANNAD, 2026-10-14:

- API 19 booted and the self-test passed (its first stages in the saved region found no line; its
  full-screen stage found five lines).
- 21 OCR jobs: 16 succeeded, 4 found the screen changed and were retried quietly, 1 was cancelled;
  11 read text. VOCR3-H15 took 4.94–5.22 ms per CTC frame; later estimates are calibrated to it.
- 4 Anki cards were saved (text only), none failed.
- Later L + R stopped opening the overlay: a remote screenshot tool's failed capture held 2.2 MB of
  SceShell's memory. A restart without the tool fixed it, with no change to Rubi.
- The owner's six requests went into test.2.

## [0.7.12] - 2026-10-12

VOCR2, an experimental second local model with a textbox finder, can be chosen in **JP Overlay
Settings** (it failed vita-vn-ocr's whole-textbox promotion check); VOCR stays the default. The zip
also has the optional Meiki engine. The pre-release 0.7.12-vocr2.1 was VOCR2's trial build.

## [0.7.11] - 2026-10-11

JP Overlay Settings chooses the engine directly: VOCR, Meiki or Google Lens. The plugins are 0.7.10's.

## [0.7.10] - 2026-10-11

Whole word navigation: Right skips the matched word, Left retraces up to 16 selections, and
automatic OCR looks the first word up before the popup opens.

## [0.7.9] - 2026-10-11

Whole-word or character highlighting, and a tap on the recognized sentence looks that word up. On
the console: the settings app's taps and buttons saved both highlight modes; taps in a game were not
checked.

## [0.7.8] - 2026-10-11

Fixed the settings app's crash while loading game icons (C2-12828-1). On the console: the repaired
app opened and its controls worked.

## [0.7.7] - 2026-10-11

A more readable settings app: larger text, three game cards a screen, ON/OFF badges and page tabs.

## [0.7.6] - 2026-10-11

VOCR (the default) and Meiki installed together and switched in the settings app; Japanese-only
character selection, ON by default. The pre-releases 0.7.6-meiki.1 and .2 were Meiki's single-line
trial. On the console (0.7.6-meiki.2): two CLANNAD lines took about 8 s each with Meiki.

## [0.7.5] - 2026-10-11

Character-by-character selection, and the Shell's lookup in one 64 KiB arena instead of two of
128 KiB. On the console: OCR in CLANNAD's process, a local lookup and the cursor all worked.

## [0.7.4] - 2026-10-11

Fixed L + R also advancing the game's dialogue: the filter now hides the buttons as CLANNAD reads
them. On the console: two L + R presses in CLANNAD were hidden from the game at the controller hook
(the dialogue itself was not checked).

## [0.7.3] - 2026-10-11

Fixed model loading in the game (a status code arrived as `0xBFFFFFFD`; API 13). On the console:
API 13 booted on firmware 3.65, and CLANNAD ran 15 OCR jobs, 7 of them with text.

## [0.7.2] - 2026-10-11

Fixes from the first console test: the kernel reads the models for the game, which could not open
`ux0:` itself (`0x80010013`), and the settings app's LiveArea images are indexed PNGs, after
VitaShell refused the old ones (`0x8010113D`); also installer repairs.

## [0.7.1] - 2026-10-11

Local OCR (VOCR's FL10 and H15 models) runs inside each enabled game (the worker under `*ALL`),
with a bundled JMdict and the standalone **JP Overlay Settings** app. Not yet run on a console when
it was published.

## 0.7.0 - 2026-10-11

This fork's first tag (no release): offline OCR, a bundled JMdict and per-game settings ([#20]).

## 0.1–0.7: upstream

[Dartv/vita-jp-overlay](https://github.com/Dartv/vita-jp-overlay)'s versions: Vita JP Overlay, with
Google Lens OCR over Wi-Fi and definitions from jiten.moe or jpdb.io (0.1), the subtitle strip and
per-game regions (0.3), PSP games in Adrenaline (0.4) and faster automatic reading (0.6). This fork
started from 0.6; upstream's 0.7, the furigana strip with its Anki fields, came in with 0.8.0-test.1
([#48]).

[Unreleased]: https://github.com/bee-san/Rubi_for_vita/compare/v0.10.0...HEAD
[0.10.0]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.10.0
[0.9.8]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.9.8
[0.9.7]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.9.7
[0.9.6]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.9.6
[0.9.5]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.9.5
[0.9.4]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.9.4
[0.9.3]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.9.3
[0.9.2]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.9.2
[0.9.1]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.9.1
[0.9.0]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.9.0
[0.8.0]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.8.0
[0.8.0-test.4]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.8.0-test.4
[0.8.0-test.3]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.8.0-test.3
[0.8.0-test.2]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.8.0-test.2
[0.8.0-test.1]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.8.0-test.1
[0.7.12]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.7.12
[0.7.11]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.7.11
[0.7.10]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.7.10
[0.7.9]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.7.9
[0.7.8]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.7.8
[0.7.7]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.7.7
[0.7.6]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.7.6
[0.7.5]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.7.5
[0.7.4]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.7.4
[0.7.3]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.7.3
[0.7.2]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.7.2
[0.7.1]: https://github.com/bee-san/Rubi_for_vita/releases/tag/v0.7.1
[#20]: https://github.com/bee-san/Rubi_for_vita/pull/20
[#48]: https://github.com/bee-san/Rubi_for_vita/pull/48
[#49]: https://github.com/bee-san/Rubi_for_vita/pull/49
[#50]: https://github.com/bee-san/Rubi_for_vita/pull/50
[#51]: https://github.com/bee-san/Rubi_for_vita/pull/51
[#63]: https://github.com/bee-san/Rubi_for_vita/pull/63
[#64]: https://github.com/bee-san/Rubi_for_vita/pull/64
[#66]: https://github.com/bee-san/Rubi_for_vita/pull/66
[#68]: https://github.com/bee-san/Rubi_for_vita/pull/68
[#71]: https://github.com/bee-san/Rubi_for_vita/pull/71
[#72]: https://github.com/bee-san/Rubi_for_vita/pull/72
[#73]: https://github.com/bee-san/Rubi_for_vita/pull/73
[#75]: https://github.com/bee-san/Rubi_for_vita/pull/75
[#76]: https://github.com/bee-san/Rubi_for_vita/pull/76
[#77]: https://github.com/bee-san/Rubi_for_vita/pull/77
[#78]: https://github.com/bee-san/Rubi_for_vita/pull/78
[#79]: https://github.com/bee-san/Rubi_for_vita/pull/79
[#84]: https://github.com/bee-san/Rubi_for_vita/pull/84
[#85]: https://github.com/bee-san/Rubi_for_vita/pull/85
[#86]: https://github.com/bee-san/Rubi_for_vita/pull/86
[#87]: https://github.com/bee-san/Rubi_for_vita/pull/87
[#88]: https://github.com/bee-san/Rubi_for_vita/pull/88
[#89]: https://github.com/bee-san/Rubi_for_vita/pull/89
[#90]: https://github.com/bee-san/Rubi_for_vita/pull/90
[#92]: https://github.com/bee-san/Rubi_for_vita/pull/92
[#93]: https://github.com/bee-san/Rubi_for_vita/pull/93
[#94]: https://github.com/bee-san/Rubi_for_vita/pull/94
[#95]: https://github.com/bee-san/Rubi_for_vita/issues/95
[#96]: https://github.com/bee-san/Rubi_for_vita/pull/96
[#99]: https://github.com/bee-san/Rubi_for_vita/pull/99
[#104]: https://github.com/bee-san/Rubi_for_vita/issues/104
[#105]: https://github.com/bee-san/Rubi_for_vita/pull/105
[#106]: https://github.com/bee-san/Rubi_for_vita/pull/106
[#108]: https://github.com/bee-san/Rubi_for_vita/pull/108
[#109]: https://github.com/bee-san/Rubi_for_vita/pull/109
[#112]: https://github.com/bee-san/Rubi_for_vita/pull/112
[#116]: https://github.com/bee-san/Rubi_for_vita/pull/116
[#117]: https://github.com/bee-san/Rubi_for_vita/pull/117
[#122]: https://github.com/bee-san/Rubi_for_vita/pull/122
[#123]: https://github.com/bee-san/Rubi_for_vita/pull/123
[#124]: https://github.com/bee-san/Rubi_for_vita/pull/124
[#125]: https://github.com/bee-san/Rubi_for_vita/pull/125
[#126]: https://github.com/bee-san/Rubi_for_vita/pull/126
[#127]: https://github.com/bee-san/Rubi_for_vita/pull/127
[#129]: https://github.com/bee-san/Rubi_for_vita/pull/129
[#130]: https://github.com/bee-san/Rubi_for_vita/pull/130
[#132]: https://github.com/bee-san/Rubi_for_vita/pull/132
[#133]: https://github.com/bee-san/Rubi_for_vita/pull/133
[#140]: https://github.com/bee-san/Rubi_for_vita/pull/140
[#141]: https://github.com/bee-san/Rubi_for_vita/pull/141
[#145]: https://github.com/bee-san/Rubi_for_vita/pull/145
[#146]: https://github.com/bee-san/Rubi_for_vita/pull/146
[#147]: https://github.com/bee-san/Rubi_for_vita/pull/147
[#148]: https://github.com/bee-san/Rubi_for_vita/pull/148
[#149]: https://github.com/bee-san/Rubi_for_vita/pull/149
[#150]: https://github.com/bee-san/Rubi_for_vita/pull/150
[#153]: https://github.com/bee-san/Rubi_for_vita/pull/153
[#154]: https://github.com/bee-san/Rubi_for_vita/pull/154
[#155]: https://github.com/bee-san/Rubi_for_vita/pull/155
[#156]: https://github.com/bee-san/Rubi_for_vita/pull/156
[#157]: https://github.com/bee-san/Rubi_for_vita/pull/157
[#158]: https://github.com/bee-san/Rubi_for_vita/pull/158
[#164]: https://github.com/bee-san/Rubi_for_vita/pull/164
[#167]: https://github.com/bee-san/Rubi_for_vita/pull/167
[#170]: https://github.com/bee-san/Rubi_for_vita/pull/170
[#173]: https://github.com/bee-san/Rubi_for_vita/pull/173
[#174]: https://github.com/bee-san/Rubi_for_vita/pull/174
[#176]: https://github.com/bee-san/Rubi_for_vita/pull/176
[#178]: https://github.com/bee-san/Rubi_for_vita/pull/178
[#179]: https://github.com/bee-san/Rubi_for_vita/pull/179
[#181]: https://github.com/bee-san/Rubi_for_vita/pull/181
[#182]: https://github.com/bee-san/Rubi_for_vita/issues/182
[#185]: https://github.com/bee-san/Rubi_for_vita/pull/185
[#186]: https://github.com/bee-san/Rubi_for_vita/pull/186
[#187]: https://github.com/bee-san/Rubi_for_vita/pull/187
[#189]: https://github.com/bee-san/Rubi_for_vita/pull/189
[#190]: https://github.com/bee-san/Rubi_for_vita/pull/190
[#193]: https://github.com/bee-san/Rubi_for_vita/pull/193
[#194]: https://github.com/bee-san/Rubi_for_vita/issues/194
[#200]: https://github.com/bee-san/Rubi_for_vita/pull/200
[#203]: https://github.com/bee-san/Rubi_for_vita/pull/203
[#204]: https://github.com/bee-san/Rubi_for_vita/pull/204
[#206]: https://github.com/bee-san/Rubi_for_vita/pull/206
[#207]: https://github.com/bee-san/Rubi_for_vita/issues/207
[#208]: https://github.com/bee-san/Rubi_for_vita/pull/208
[#210]: https://github.com/bee-san/Rubi_for_vita/issues/210
[#213]: https://github.com/bee-san/Rubi_for_vita/pull/213
[#214]: https://github.com/bee-san/Rubi_for_vita/pull/214
[#216]: https://github.com/bee-san/Rubi_for_vita/pull/216
[#217]: https://github.com/bee-san/Rubi_for_vita/pull/217
[#218]: https://github.com/bee-san/Rubi_for_vita/pull/218
[#219]: https://github.com/bee-san/Rubi_for_vita/pull/219
[#222]: https://github.com/bee-san/Rubi_for_vita/pull/222
[#224]: https://github.com/bee-san/Rubi_for_vita/pull/224
[#230]: https://github.com/bee-san/Rubi_for_vita/issues/230
[#231]: https://github.com/bee-san/Rubi_for_vita/pull/231
[#232]: https://github.com/bee-san/Rubi_for_vita/pull/232
[#233]: https://github.com/bee-san/Rubi_for_vita/pull/233
[#237]: https://github.com/bee-san/Rubi_for_vita/issues/237
[#239]: https://github.com/bee-san/Rubi_for_vita/pull/239
[#241]: https://github.com/bee-san/Rubi_for_vita/issues/241
[#242]: https://github.com/bee-san/Rubi_for_vita/pull/242
[#243]: https://github.com/bee-san/Rubi_for_vita/pull/243
[#247]: https://github.com/bee-san/Rubi_for_vita/pull/247
[#249]: https://github.com/bee-san/Rubi_for_vita/pull/249
[#250]: https://github.com/bee-san/Rubi_for_vita/pull/250
[#251]: https://github.com/bee-san/Rubi_for_vita/pull/251
[#252]: https://github.com/bee-san/Rubi_for_vita/issues/252
[#253]: https://github.com/bee-san/Rubi_for_vita/pull/253
[#256]: https://github.com/bee-san/Rubi_for_vita/pull/256
[#257]: https://github.com/bee-san/Rubi_for_vita/pull/257
[#258]: https://github.com/bee-san/Rubi_for_vita/pull/258
[#259]: https://github.com/bee-san/Rubi_for_vita/pull/259
[#260]: https://github.com/bee-san/Rubi_for_vita/issues/260
[#266]: https://github.com/bee-san/Rubi_for_vita/issues/266
[#268]: https://github.com/bee-san/Rubi_for_vita/pull/268

[#273]: https://github.com/bee-san/Rubi_for_vita/pull/273
[#274]: https://github.com/bee-san/Rubi_for_vita/pull/274
[#275]: https://github.com/bee-san/Rubi_for_vita/pull/275
[#283]: https://github.com/bee-san/Rubi_for_vita/pull/283
[#285]: https://github.com/bee-san/Rubi_for_vita/pull/285
[#286]: https://github.com/bee-san/Rubi_for_vita/pull/286
[#289]: https://github.com/bee-san/Rubi_for_vita/pull/289
[#291]: https://github.com/bee-san/Rubi_for_vita/pull/291
[#292]: https://github.com/bee-san/Rubi_for_vita/pull/292
[#293]: https://github.com/bee-san/Rubi_for_vita/pull/293
[#294]: https://github.com/bee-san/Rubi_for_vita/pull/294
[#295]: https://github.com/bee-san/Rubi_for_vita/pull/295
[#296]: https://github.com/bee-san/Rubi_for_vita/pull/296
[#299]: https://github.com/bee-san/Rubi_for_vita/pull/299
[#303]: https://github.com/bee-san/Rubi_for_vita/pull/303
[#305]: https://github.com/bee-san/Rubi_for_vita/pull/305
[#320]: https://github.com/bee-san/Rubi_for_vita/pull/320
[#321]: https://github.com/bee-san/Rubi_for_vita/issues/321
