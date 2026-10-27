# Dictionary lookahead over the whole line (#360, change 1)

This follows [Dictionary lookup preparation](dictionary-lookahead.md) (#203), whose session, locators and
idle-time preparation it keeps and extends. Issue #360 measured a cursor lookup at 8.12 file reads (est.
37 ms on the console; 5.98 reads, est. 29 ms, since its change 3, #370) and a word the lookahead had
located at 1.04 (est. 16 ms), and found the lookahead covering only Right, at most eight words, in Auto
with Whole word, after 100 ms idle. Here it covers the line.

## What changed

- **Every mode.** With the overlay open the lookahead runs in Whole word and Character mode, with **Read
  text: Automatically** and **Only when I press L + R** alike (a pressed text is looked up as an Automatic
  one). #203 gave no reason for its restriction (its commit and PR say none); the one that still holds is
  the closed overlay's: with the overlay closed only Auto with Whole word keeps a result block
  (`want_lookup`) and Only when I press reads nothing before the press
  ([auto-vs-press.md](../docs/auto-vs-press.md)). So with the overlay closed the pass covers #203's eight
  positions from Auto's first word (that word, located, and seven jobs); opening goes on from there.
- **The whole line, both directions.** From the cursor, in order: the positions Right reaches (Whole word:
  past each located word; Character mode: each next character), then each character before the cursor
  (a Left with nothing to retrace goes one back), then every other character after it (a tap, the stick,
  Up and Down), nearest first, to the line's ends (`vjo_text_row_step`, Japanese letters only with
  `japanese_only_selection`). A position already located takes no job (`vjo_local_located`: the session
  answers without a read); a new cursor starts the order again from itself and keeps what is located. A
  pass has 256 units (`VJO_LOCAL_LOCATED`; 15 closed), a unit a job and a unit a step past a located
  position: at most 128 jobs, so with Dictionaries shown 1 or 2 (the default) it never overwrites its own
  locators (with more, a long line's first positions may be located again when they come up), and the walk
  over located positions after a cursor move is at most 256 steps of the control thread (host: 9 µs for a
  29-character line, the corpus mean, 314 µs for its longest, 170, and 1.6 ms for a 4 KiB text, the most
  the overlay holds [measured, x86-64]; the A9 is slower, unmeasured). A refresh that replaces the text
  starts a new pass.
- **Sooner.** The first prepare starts 20 ms after the last job (`PREPARE_IDLE_US`, was 100 ms), and with
  the overlay open the control thread's wait times out after 20 ms (`ctl_wait_us`; 50 ms closed, as before),
  so the next prepare does not wait for the 50 ms poll. 20 ms is longer than a 60 fps frame (16.7 ms), in
  which the overlay reads the pad and posts a press (it wakes the control thread through the kernel event),
  so the press that follows a result finds the worker idle; held arrows repeat 100 ms apart and the stick
  looks its word up after 200 ms at rest. A user's command (`pending_cmd`, a cursor move waiting), the
  subtitle strip, a screenshot, the self-test and a refresh go first, as before.
- **Compact locators.** The session's 16 locators of 204 bytes (`VjoLocalPages`, 3,264 B) are 256 records of
  12 bytes (`Located`, 3,072 B): the payload's 32-bit offset, its length, the dictionary, the conjugation,
  the cursor byte and the span; the file size and pos16 come from the session and the cursor. A position
  takes one record a located page (a miss one), so the ring holds 256 positions with one page, 128 with
  Dictionaries shown 2 and 32 with 8; the oldest go first, and a position whose first record went is searched
  again. Every position fits: a span is under 128 UTF-16 units (32 characters of at most 2 units, and the few
  line breaks a scan can cross where lines join: `SpanFits`), and a dictionary file must be under 4 GiB: one
  of 4 GiB or more is refused as it opens, as an invalid dictionary (`parse_header`, which allowed 2^63 B), and
  the converter and the FTP installer refuse one with "the dictionary would be 4 GiB or larger; Rubi reads
  dictionary files under 4 GiB" (`tools/convert_dictionary.py` `MAX_FILE_SIZE`; the bundled dictionary is
  157 MB). The session is 6,528 B on the Vita (6,784 before) and 6,600 B on x86-64 (6,856), under its 7 KiB
  compile-time cap; `Candidate` is unchanged.
- **Hit rate in the debug log.** The overlay close line (`overlay close (…) … input block off rc=0`) ends with
  `lookups N located, M cold`: the open overlay's word lookups that found their word located (or nothing to
  look up at the cursor) and those that searched. The normal build counts nothing.

Results cannot change: a located word is read through the same `vjo_local_read_page` as before, from a
locator that holds what `lookup_pages` found at that cursor (offset, length, dictionary, span and
conjugation exactly), and only for the same text generation, dictionaries and Dictionaries shown.

## Measured on the host

The rubi-dict-check corpus (64,579 taps of 5,000 real VN lines, 46 games), both bundled dictionaries
(`plaintext.vjdict`, `vndb-names.vjdict`), Dictionaries shown 2, gcc 11.5 `-O2`, warm OS cache; console
milliseconds are `tools/dict_stage_report.py`'s estimate (13.1 ms + 2.98 ms a read, test.4's fit).

| | reads/lookup before → after | est. console ms/lookup (mean) | results |
|---|---:|---:|---|
| cursor lookup, nothing located (`cursor`) | 5.98 → 5.98 | 31.4 → 31.4 | IDENTICAL at every tap |
| word located first (`prepared`) | 1.04 → 1.04 | 16.2 → 16.2 | IDENTICAL at every tap |
| the open overlay's taps in order, the lookahead between them, Whole word (`word`) | 2.03 → 1.04 | 19.3 → 16.3 | IDENTICAL |
| the same, Character mode (`character`) | 5.66 → 1.04 | 30.5 → 16.3 | IDENTICAL |

"Before" is main `208cdb8`, which has #360's change 3 (#370: 8.12 → 5.98 reads for a cold lookup); against
the plan's baseline (main `892cfb9`) the cursor lookup is 8.12 → 5.98 and the located one 1.04 → 1.04,
IDENTICAL at every tap. `vjo-dict-taps` output is byte-identical to rubi-dict-check's oracle (`cmp`:
TAPS_IDENTICAL), and `tools/dict_lookup_golden.tsv` is unchanged. The `word` and `character` modes of
`vjo-dict-stage-bench` (new here) open each line on its first word and run the worker's lookahead to its
end between taps (the idle time is assumed); "before" is the same bench built against main, whose
lookahead locates the cursor's word and the next seven Right reaches, in Whole word only. Taps whose word
was located: 80.3 % → 100 % (Whole word), 5.6 % → 100 % (Character mode). The background reads that buy
it: 54 → 129 reads a line (10.0 a tap), about 0.38 s of reads a line on the console [estimate, 2.98 ms a
read], spread over idle time.

**A prepare in flight when the user presses.** The press waits for that one prepare, never more (the next
slot is the user's). One prepare's reads over the corpus (`prepared`'s background reads): mean 4.98, p50 4,
p95 9, p99 21, max 24, so est. 15 ms mean, 27 ms p95, 63 ms p99 and 72 ms at most on the console
[estimate, 2.98 ms a read]. Before this change the same wait could happen after 100 ms of idle time; now
after 20 ms.

Raw per-tap JSONL: `herd-vita-ocr/shared/artifacts/rubi-dict-speed-1/`. Reproduce (`D`, `F` as above):

```sh
build/host/vjo-dict-stage-bench $D $F 2 cursor 5 < corpus.tsv > after-cursor.jsonl
build/host/vjo-dict-stage-bench $D $F 2 prepared 5 < corpus.tsv > after-prepared.jsonl
build/host/vjo-dict-stage-bench $D $F 2 word 3 < corpus.tsv > after-word.jsonl
build/host/vjo-dict-stage-bench $D $F 2 character 3 < corpus.tsv > after-character.jsonl
python3 tools/dict_stage_report.py after-word.jsonl --before before-word.jsonl
```

Host tests: `local_cursor`'s compact locators (a line of 300 positions with 1, 2 and 8 dictionaries, the
oldest evicted and searched again with the same pages, a position whose other pages outlived its first
record never taken for it), `vjo_local_located` (no read; punctuation, the end, a miss, another text,
limit or dictionary list), and files of 4 GiB (refused: a format error, nothing open) and just under it (a
payload offset above 2^32 − 8 KiB located exactly); `tools_local_dictionary`'s converter and installer limit
(each format and the key filter's pages, the previous output kept); `shell_worker`'s
lookahead in Whole word and Character mode, Automatically and on press (a tap first, cold; then the order
above, every position one read or none, nothing left to prepare from any cursor; the close line's counts),
a tap past the eighth word within the pass's budget (128 jobs from a located cursor; the closed pass's seven
and 124 more on opening; a new cursor on a located line makes no job and stops), and a user's
command during a prepare (it waits for that one only; a command not taken yet holds the next prepare back).

## Memory

No new allocation, block, thread, budget or static buffer. The locators are in the session inside the
existing 64 KiB PAF block (6,784 → 6,528 B, measured: `sizeof` with the Vita compiler); the result
arena's budget is unchanged (its compile-time checks hold). The normal Shell had 92 B of code room and
312 B of data room on main `208cdb8`, and the change needs 320 B of code (`lookup_pages` +236 B, the
scheduler in `control_tick` +60, `locate` +112, `ctl_main`'s wait +20, the overlay's `frame` +184 as LTO now
inlines `vjo_word_step`, −196, into its one caller; `net_main` −96 without the old prepare's end): five cold
strings pay for it in the data segment (`VJO_DATA_TABLE`), two of `vjo_err_text`'s messages (98 B) and
`net_stack_paint`'s three log formats, once a boot at most (164 B).

| packaged Shell (both builds) | main `208cdb8` | this change | room left |
|---|---:|---:|---:|
| normal text | 90,020 B | 90,060 B | 52 B of 90,112 |
| normal data/bss | 73,416 B | 73,680 B | 48 B of 73,728 |
| debug text | 102,132 B | 102,340 B | 60 B of 102,400 |
| debug data/bss | 73,408 B | 73,672 B | 56 B of 73,728 |

The same 163,840 B (normal) and 176,128 B (debug) of module pages; `tools/check_memory_ledger.py` passes in
both builds [measured: the Vita builds' module rows]. The lookahead's question (`vjo_local_located`) now runs on
VjoControl: its chain is 3,116 B (`ctl_main` 24, `control_tick` 864 as before, `locate` 312, `lookup_pages`
1,896 for its no-search answer, `vjo_utf8_next` 20) of the thread's 16 KiB [measured: the normal build's
frames]; the prepares themselves run on VjoNet as before, through `locate` instead of the old
`vjo_local_prepare_at` frame.

Nothing here has run on a console: the console numbers are estimates.
