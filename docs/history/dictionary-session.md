# Dictionary file sessions

This records the first implementation, before the subsequent directory cache,
word preparation and event wakeups. Current behavior and validation are in
[Dictionary lookup preparation](dictionary-lookahead.md); the measurements below
are the preserved first-stage comparison.

Reuse dictionary handles and parsed headers while the overlay is open. This removes
the repeated open, size seek and close from cursor moves, and the header read from
dictionary-page changes. Dictionary contents, the four-page cache and deinflection
scratch still live only for one lookup. No file format, ranking or highlight policy changes.

## Ownership and memory

`VjoPlatform.local_session` is optional, zero-initialized, caller-owned storage.
`vjo_local_session_size()` gives its size; `vjo_local_session_close()` releases its
handles and can be called again. Only one job uses a session at a time. Directory,
ordered filenames and platform owner/callback are compared before reusing it. A failed
open/header validation closes the partial set; an I/O/format failure discards the live
set. A dictionary page borrows a matching live handle, or uses the original stateless
open/header/payload/close path when there is no matching set.

The Shell places the session ahead of the resettable lookup arena, inside its existing
65,536-byte allocation. It is 2,416 bytes on Vita and 2,488 bytes on this x86-64 host
[measured: Vita GCC 15.2.0's `vjo_local_session_size` and benchmark arena peaks]. A compile-time
check caps it at 3 KiB. There is no additional PAF allocation or budget increase, and at
most eight dictionary handles. Session close precedes freeing the allocation.

Overlay close while a job runs sets a control-thread flag: completion closes the
session before a waiting lookup can start, even after a rapid reopen. Game exit and
worker stop use the existing result-release paths. A full OCR job releases the old
results/session before recognizing; a quiet refresh keeps them. A completed background
job closes the files while retaining its cached word. Installing dictionaries requires
closed games, as before: live files are not polled for external modification or replacement.

The static deinflection records omit their trailing padding, preserving every generated
rule. On Vita, the 569 records are 14 rather than 16 bytes each. This pays for the session
code inside the existing Shell pages. No address of a packed field is taken.

Vita builds, same local SDK, baseline `1f113ad6f29f452cb9ab7a81df6d8d2af5d31b8b`
[measured SELF segments; the numbers include segment padding]:

| Normal Shell | text bytes | data/bss bytes | loaded pages |
|---|---:|---:|---:|
| Baseline | 90,004 | 73,216 | 163,840 |
| Sessions | 89,380 | 73,224 | 163,840 |

The session debug Shell is 99,852 + 73,224 bytes, 176,128 loaded bytes [measured]. Both
Shell variants pass the existing memory ledger; the normal Shell passes its exact-page check.

## Real-dictionary comparison

Run the production benchmark twice, keeping each line as one overlay session:

```sh
build/host/vjo-dict-bench DICT_DIR plaintext.vjdict host/fixtures/word_lookup/vocr_lines.tsv words 11 > stateless.jsonl
VJO_DICT_BENCH_SESSION=1 build/host/vjo-dict-bench DICT_DIR plaintext.vjdict host/fixtures/word_lookup/vocr_lines.tsv words 11 > session.jsonl
```

The dictionary is the current pinned shipped file, SHA-256
`1c563c86a29801267c37fdf1def42650b02c69657f4144f09bd26d61b80d79b5`,
157,298,688 bytes [measured]. The 50 committed VOCR lines produce 616 word selections.
Every result/error, selected definition, highlight and digest agrees between modes.
Both modes also verify their output across timed rounds. The warm host OS cache is not
a Vita memory-card measurement.

| Host measurement | Stateless | Sessions |
|---|---:|---:|
| Opens | 616 | 50 |
| Reads | 3,416 | 3,349 |
| Reads per selection | 5.545 | 5.437 |
| Header-page reads | 616 | 549 |
| Peak lookup bytes, including session | 54,928 | 57,416 |

[Aggregate results for first-word, word and character selections](../benchmarks/dict-session-20261017.json).
First-word selection is cold in both modes (50 opens and 312 reads); this optimization
targets subsequent selections and dictionary pages.

Most header-page reads remain: the first candidate must consult the Bloom directory,
which shares page 0. Parsed-header reuse does not retain that 4 KiB page. The main saving
is 566 open/size-seek/close cycles (91.9% fewer opens); reads fall 2.0%. A warm dictionary-page
change needs one payload read instead of a header plus payload and has no open/close.

No console latency or CPU3 improvement is measured. The older console fit's fixed
overhead includes other work, so removing opens is not proof of saving its entire intercept.
On hardware, compare cold first selection, repeated word moves and dictionary pages with
the same build variant, dictionary, clocks and game. Keep short/long definitions, misses
and inflections; record median/p95, result identity and failures. Host tests and the
build ledger do not establish console usability.

## Validation

The 68 CTest entries outside the combined Python tools group pass, with the shipped
dictionary enabled. The 64 focused dictionary, benchmark, memory-ledger and capture-parity
Python tests also pass. The generated dictionary/cursor tests pass with AddressSanitizer
and UndefinedBehaviorSanitizer, and the Cortex-A9 Linux build runs under qemu-arm.
The normal and debug Shell builds fit their original page budgets; the debug three-plugin
ledger and both builds' packaged syscall ABI checks pass.

Broader checks expose existing baseline problems: eight OCR-model pin/name tests also
fail on unmodified `1f113ad`, and the normal OCR worker's shared 98,304-byte declaration
does not equal its 94,208 loaded bytes. These are separate from the Shell's exact-page
check and have not been changed here. No console installation or timing run was performed.
