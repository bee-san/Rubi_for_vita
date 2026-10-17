# Offline OCR history (internal)

Automatic OCR silently appends to `ux0:data/VitaJPOverlay/history.csv`. There is
no UI, setting, network connection or export job. Copy the file with VitaShell.
Only successful, current-game OCR in `ocr_mode = auto` is eligible: ordinary
background scans, scans on opening, and refreshes of an open overlay. Dictionary
failure does not discard recognized text. Cursor lookups, self-tests, cancelled
or failed scans, and `on_press` mode produce no records. Blank scans reset the
consecutive-text fingerprint without writing a record.

The first five columns are GSM's ExStatic lines CSV format:

| Column | Meaning |
| --- | --- |
| `uuid` | Platform and actual game title/disc ID |
| `given_identifier` | Precise observation timestamp and byte offset of the append |
| `name` | Game title, falling back to its ID |
| `line` | Entire normalized OCR dialogue, preserving its wrapped lines |
| `time` | UTC Unix seconds with six fractional digits, at the job's start |

The timestamp uses the console RTC at completion minus the job's elapsed
process time. It is an observation time, not an import time, and relies on a
correct console clock. An unavailable/invalid RTC skips the record. IDs are
written into the file; repeatedly importing the same file into GSM's
`/api/import-exstatic` endpoint skips records with those IDs. The importer
accepts extra columns; Japanese should be selected as GSM's target language
before import. Text/timestamps support reading totals, kanji history and GSM's
estimated time/speed; Anki-card counts need separate mining metadata.

The remaining columns preserve platform/title ID, engine and actual model,
raw recognized text (as much as the existing transport retained), scene and
region sequence, process time and job duration, job sequence, region and frame
geometry/format, detected and recognized line counts, CTC frames, find/load/
recognition/capture timings, capture strips and changed rows, region checksum,
capture route/memory, workspace/resident bytes and model fallback. `line_metrics`
is a `|`-separated list of `width:height:frames:flags:us` for each reported line.
The low ten flag bits are confidence in thousandths (1023 means unavailable);
0x4000 means LM decoded, 0x8000 means LM changed the line (see
`include/vjo_game_ocr.h`). No screenshot, model or dictionary content is copied.

VjoNet publishes `job_done` before appending. The UI/control thread can consume
the result while the existing worker streams it to disk; that worker cannot
replace the existing transport result until the append returns. Only game/job
facts are copied on its existing stack. The writer has a fixed 192-byte output
buffer and a 16-byte static fingerprint (source sizes, not a console memory
measurement); no allocation, thread, growing queue, second OCR-text buffer or
history read is added. A seek obtains the append offset even for a large file.
Both module-page budgets and thread-stack budgets remain enforced by the ledger.

Consecutive identical normalized text within a game is suppressed, including
rescans caused by a blinking icon or reopening/resuming. A different sentence,
game, or successful blank scan permits the same text again. Fingerprints reset
on Shell restart. This is deliberately lightweight: an immediately repeated
identical sentence with no observed change may be collapsed, and a long pause
while text types out can still produce a partial sentence followed by its full
version. The existing stable-text wait mitigates this; history adds no further
text buffering or heuristics.

Short writes are retried. Write/flush/close errors are best effort and do not
change OCR or its UI; a failed append attempts to restore the previous file
size, and does not advance the fingerprint. Power loss during an append can
leave an incomplete final CSV record; discard that record before importing if
necessary. History is never rotated or erased automatically. Disk use grows
with the saved text; RAM use does not. Console I/O latency and memory headroom
for this change remain unmeasured until a physical-console run.
