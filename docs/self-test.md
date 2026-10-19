# The on-device self-test

Every console test so far ended with the same two questions: did it boot, and what ran out of
memory. The self-test answers both from one file, in the owner's two commands, and tells the
fleet exactly which step failed and with how much free memory in which pool.

Since API 19 (#74) one run also exercises the whole in-game capture: the capture probe, the
smallest model size (the fallback stage, API 21), the configured size, the full screen and a warm
second recognition, each stage with its own record whether it passed or not
([ingame-capture.md](ingame-capture.md)).

The self-test is the debug build's (`Rubi-<version>-debug.zip`, and every test build): the
normal build, the release's main download, has none (docs/release-variants.md). Armed under the
normal build, the Shell says so once in `log.txt` and leaves the marker for a debug build;
`install_ftp.sh --self-test` from the normal package says so too.

## Running it

1. Arm it (VitaShell FTP running): `bash tools/install_ftp.sh --self-test VITA_IP`. This uploads
   one empty file, `ux0:data/VitaJPOverlay/selftest.txt`.
2. Reboot. Wait for the LiveArea and leave the Vita alone for a minute (the boot guard's `ok`).
3. Start the game to test. Reach a screen with dialogue text and wait about 30 seconds without
   pressing anything: once an OCR job has read text in your region, the stages below run in a
   background job of their own on that screen, after 2 s with the overlay closed and nothing
   else running. Your own scans never run them (test.2's first scans did, beside them, and took
   9-15 s: [first-ocr.md](first-ocr.md)): the first scans are as fast as later ones, except one
   made while the stages run, which waits for them to stop (about 0.2 s, at most about 0.5 s, est.),
   and the self-test's job shows nothing. Opening the overlay or a new screen stops it; its stages run
   again, from the first, after the next scan that reads text. Screens without a line in your
   region are not tested: the game's logos and notices (#81's run began 2 s after the game
   started, on CLANNAD's opening notice, whose five lines lay above the region, and all three
   region stages found nothing), menus, black screens. A first stage that still finds no line,
   or whose screen changes under every read, closes its `begin` with a `stage fallback wait`
   record; after 30 such screens the stages run anyway, and the analyser says when they then
   found no text. OCR runs automatically while the test is armed, whatever `ocr_mode` says; the
   region is
   **yours** (draw it with Square first if you have not; with none, the full screen). The game
   needs the OCR worker, which the kernel loads into it at its start when it is ON in the Rubi
   app (since #138; before, a `--title` line); a game without it (turned ON while it ran, started
   with L held) reports `rc=-117` (`ocr unavailable`) in every stage, and `kernel.txt`'s `worker
   for` line says why.
4. Back in VitaShell: `bash tools/install_ftp.sh --collect VITA_IP` fetches the report with every
   log and record into one zip ([below](#collecting-everything---collect)); `--status VITA_IP` fetches
   just `ux0:data/VitaJPOverlay/selftest-report.txt` (and `status.txt`, `kernel.txt`, `boot.txt`).
5. `python3 tools/analyse_selftest.py build/device/status/selftest-report.txt` prints the run
   and its OCR, one PASS/FAIL row per stage, then the diagnostics (PASS/WARN), the free memory
   every route read at each step, the game worker's memory around each allocation, the findings
   and a verdict. Only the OCR rows decide the verdict; a diagnostic WARN never does. Attach the
   report to the issue.

| stage | job | what it shows |
|---|---|---|
| `fallback` | your region, the smallest model size forced | the smallest size's model (VOCR5-S) read from the card and recognizing; with Small configured, Small itself (there is none smaller: the config record's `fallback=-`) |
| `textbox` | your region, the configured model size | the configured size's model (VOCR5-N for the default, Normal) read from the card (its load time), the strip capture, the finder and the recognizer |
| `fullscreen` | the whole frame | the full screen through the strips, the configured model kept resident |
| `probe` | each capture path into each memory type, one strip | whether the display hook can write the game's `USER_RW`, `CDRAM` and `PHYCONT` memory, the kernel strip, and a user-mode `sceDisplayGetFrameBuf` read; each with its rc and the rows' checksum |
| `textbox2` | your region again, serially | the warm time on one thread (API 22: textbox ran with `ocr_threads`' helpers); it leaves the configured size's model resident for your next scan |

No stage uses the line cache (API 22: each stage's request carries `VJO_GAME_OCR_S_NOCACHE`), so every stage
recognizes what it reads; the `stage … begin` record gives each stage's request word as `speed=`, the
`stage … end` record a checksum of the text it read as `text=`.

The marker is removed after the last stage, so one arming is one run, failed stages included
(#74: the API 18 test removed it only after a successful recognition, stayed armed after the
capture failed, and kept forcing the full-screen region). If the screen changes during the
stages, or the overlay opens, the job is cancelled and they run again after the next scan that
reads text. A run that stopped
part way (a crash, the power) leaves the marker: the next boot runs the test again, the report
shows the stage that began and never wrote its record, and `status.txt` keeps the previous boot's
copy as `status.prev.txt`.

If the game is disabled in the **Rubi** app (**Overlay** or **JP Overlay Settings** in older builds), the
report says so and no OCR runs. If the boot is a safe one (the previous boot did not complete),
the report has the boot sample only and says so; reboot once more.

## What it writes

`shell/selftest.c` writes one line per event; every step has a memory sample by every route:

```
selftest v2 api=19
boot seq=2 mode=0 prev=0/0/0/0 failures=0 write=0x00000000
config backend=2 model=H15_w8.vocr fallback=FL10_w8.vocr dictionary=3 anki=0
step boot t=0
mem boot rc=0x00000000 main=7340032 cdram=4194304 phycont=0 valid=1 paf_free=3733640
ledger boot pages=299008/299008 paf=8192/8192
kmem boot route=block part=kernel rc=0x00000000 step=0 total=37748736 free=9437184
kmem boot route=block part=shell rc=0x00000000 step=0 total=52428800 free=98304
kmem boot route=cas part=shell rc=0x00000000 step=0 main=98304/52428800 cdram=0/4194304 phycont=0/0
kmem boot route=as part=kernel rc=0x00000000 step=3 parts=2 skipped=1 p0=9437184/37748736 p1=4194304/16777216
kernel boot ledger=131072/131072 limit=225280
step ok t=60999
...
step game t=142999 title=PCSG00415 enabled=1 mode=1 region=user rect=1536,48188,62600,15059
...
stage textbox begin t=145499 flags=0x0 model=H15_w8.vocr fallback=FL10_w8.vocr
gstep textbox mem kind=- arg=0 rc=0x00000000 bytes=0 sum=0x00000000 main=91226112 cdram=117440512 phycont=0 us=10
gstep textbox alloc kind=user arg=capture rc=0x00000041 bytes=61440 sum=0x00000000 main=91164672 cdram=117440512 phycont=0 us=30
gstep textbox pass kind=hook arg=3 rc=0x00000000 bytes=114625 sum=0x00000000 main=89817088 cdram=117440512 phycont=0 us=34000
...
line textbox i=0 w=877 h=30 frames=110 us=273333
stage textbox end rc=0 t=147999 ms=2500 region=917x125+22+400 fb=960x544/960/0x00000000 model=H15_w8.vocr fallback=0 lines=3 recognized=3 frames=330
result textbox source=hook mem=user capture=61440 strip_rows=62 strips=9 tears=0 capture_ms=34 workspace=188416 resident=1159168 weights=read find_ms=9 load_ms=1050 recognize_ms=820
memory textbox free_user=91226112 reserve=4194304 need_workspace=249856 need_weights=1159168 release=0 io=0x00000000 stat=0x00000000 cleanup=0 sum=0x5EED0003
...
step done t=155499
```

(The figures above are the host writer's synthetic ones, `tools/tests/fixtures/selftest-v2-pass.txt`.)

| line | source | meaning |
|---|---|---|
| `mem` | `sceKernelGetFreeMemorySize` in SceShell | SceShell's budget as the user API reports it: each figure is the free size minus a system margin [SDK: Kernel-Reference], so a negative one (#74: `-2097152` after boot) is under the margin, 0 B free above it, not an error (`valid=0`); `paf_free` is the PAF heap's `GetFreeSize()` |
| `ledger` | the Shell ledger ([memory-ledger.md](memory-ledger.md)) | current/peak bytes we hold in SceShell's partition and in its PAF heap |
| `kmem` | `vjoMemInfo` (`kernel/meminfo.c`), one line per route | `route=block`: FindMemBlockByAddr (size 0) → MemBlockGetInfoEx → PhyMemPartInfoCore for the kernel's own block (`part=kernel`, ScePhyMemPartKD when the total is 36 MiB) and the caller's (`part=shell`); `step` names where a route stopped (0 read; 1 find; 2 block or address-space info; 3 a pointer outside the kernel range, below 0x40000000; 4 in no kernel memory block; 5 InfoCore). `route=cas`: the caller's address-space block at +328/+332/+316 (main/CDRAM/phycont), as PSVshell and PSVshellPlus read it. `route=as`: the kernel process's address-space partitions (SceSysmemForKernel_C3EF4055), as faps-coredump reads them, or the caller's (`part=shell`) when the kernel's is refused; `skipped`: partitions not read, `step` the reason. Every route reports its own rc |
| `kernel` | `vjoMemInfo` | the kernel ledger: current/peak bytes we hold in the kernel's pool and its limit (`ledger=`, `limit=`). API 24 removed the `capture=` field with the kernel's full-frame capture; OCR and the game worker's Anki screenshot (API 23) read the screen in the game |
| `stage` | the Shell | `begin` with the model and flags; `end` with the rc, time, region, frame (size/pitch/format), model, fallback, lines; `wait` (first stage only) when that screen had no line in your region (rc 0) or changed under every read (rc -120): its time, rc and `screens=k/30`; the stages run on a later settled screen |
| `gstep` | the game worker's step log | each step of the job, at most 20: `mem`, `check`, `alloc`/`free` (memory type, block, native rc, bytes; the `free` of every block the job can still free keeps a slot), `pass` (the job's first capture pass, or a failed one), `sig`, `find`, `load`, `recognize`, `fb`, `probe` (API 19 also logged `fallback`; since API 20 the fallback's `check`, `arg=1`, says it runs, with `sum=1` when the self-test forced it), and since API 20 `settle` (the stable-text wait), `blank` (a skipped line, [vita-vn-ocr.md](vita-vn-ocr.md#stable-text-and-blank-lines-api-20)), `norm` (the text normalised, [vita-vn-ocr.md](vita-vn-ocr.md#text-normalisation-shown-and-looked-up)), `lm` (the beam + LM decoder: its tier as `kind`, lines changed and doubtful, [vita-vn-ocr.md](vita-vn-ocr.md#beam--lm-decoding-api-20)), `cascade` (the doubtful lines read again, W20's frames, [vita-vn-ocr.md](vita-vn-ocr.md#confidence-cascade-api-20)) and `refine` (the finder's boxes refined, its mode as `kind`, [vita-vn-ocr.md](vita-vn-ocr.md#finder-refinement-api-20)); `check`'s `kind` is the LM tier it chose, plus 8 with the cascade; and since API 22 `par` (parallel lines and the line cache), `clock` (`ocr_boost_clock`) and `pace` (the game's frame pacing, added by the kernel; [ocr-speed.md](ocr-speed.md#what-the-log-shows-api-22)), and since API 23 an Anki screenshot's `shot` (each encoding: the JPEG quality as `kind`, its bytes, the strips) and `save` (its file: chunks as `kind`, bytes, the kernel's rc), which `log.txt` shows as `gstep shot ...` when a screenshot fails, each with the game's free main/CDRAM/phycont after it (`pace` uses those three fields for the frames before the job) |
| `line` | the game worker | each recognized line: size, CTC frames, time |
| `result` / `memory` | the game worker's result | capture source and memory, block size, strip rows, strips read, rows that changed between reads, timings; the job's memory check |

The kernel routes are looked up at run time by library and function NID through taiHEN's
`module_get_export_func`, never imported, and only when the Shell asks; nothing of this runs at
boot or in a hook. #74's 3.65 console returned `0x80020001` from the block route at every step;
the analyser reports such a route as failed, never its zeros as figures.

## What the analyser says

`tools/analyse_selftest.py` (tests: `tools/tests/test_analyse_selftest.py`, with #74's own report
as `tools/tests/fixtures/selftest-74-report.txt`, and v2 reports written by the real
`shell/selftest.c` on the host, `host/tests/selftest_writer.c`) prints two tables. The first is
the run and its OCR, PASS, FAIL or NOTEXT; these rows alone decide the verdict:

```
the run and its OCR (PASS/FAIL, NOTEXT: no line in your region; these rows decide the verdict)
  stage            result  detail
  boot             PASS    normal boot (seq 2, 0 failed boots before)
  ok               PASS    stable minute at 60999 ms
  game             PASS    PCSG00415 enabled, region user 1536,48188,62600,15059
  fallback         PASS    917x125+22+400 FL10_w8.vocr, 3 line(s), capture hook into user 61,440 B ...
  textbox          FAIL    out of memory (rc -103); failed steps: alloc user/workspace 0x80024302 ...
  fullscreen       PASS    960x544+0+0 H15_w8.vocr, 5 line(s), ...
  textbox2         PASS    ...
  recognitions     PASS    11 line(s) recognized in all, 6 in your region
  done             PASS    at 155499 ms, the marker removed
```

A region stage (`fallback`, `textbox`, `textbox2`) that completed but found no line in your region
is `NOTEXT`, not PASS: it shows the capture and the memory, not recognition. #81's report (the
v0.8.0-test.1 console run) had all three at 0 lines while the full screen found 5, and this tool
then said PASS. `recognitions` is NOTEXT when no region stage recognized a line. A run with no
FAIL but a NOTEXT row ends `NO TEXT: ...` and exits 1; a FAIL wins over it. The verdict names the
stages without a line and either what the full screen found (no region stage read a line: the
text was elsewhere) or what the other region stages read there (the screen changed between the
stages, when the region's checksum did). The `fallback` row and the findings count the settled screens
the stages waited past (`selftest-v2-waited.txt`), and say when the wait reached its bound
(`selftest-v2-notext.txt`, shaped like #81's run); a report that ends while the stages still wait
is INCOMPLETE and says so (`selftest-v2-waiting.txt`).

The second is the diagnostics, PASS or WARN, never FAIL. They say how OCR ran and what the
console measured, so a diagnostic that read nothing or did not work is a WARN and leaves the
verdict alone. #75's first build checked the wrong address range in every kernel memory route;
its report would have said FAIL with OCR working. `selftest-v2-diagnostics.txt` is that case: OCR
works in every stage while every diagnostic a passing run can warn does (probe,
capture<64KiB, kernel-pool, kernel-memory; tearing and game-memory cannot warn when OCR passes).

```
diagnostics (PASS/WARN: a WARN never changes the verdict)
  stage            result  detail
  probe            WARN    the display hook could not write the game's USER memory: worked: kstrip->user, ...
  capture<64KiB    WARN    capture block over 65,536 B in: fullscreen 262,144 B of cdram (USER_RW refused it there, ...
  kernel-pool      WARN    the kernel strip (<= 64 KiB of the kernel pool) was used in: fallback, textbox, ...
  tearing          PASS    0 row(s) changed between two reads (the job reads again)
  kernel-memory    WARN    no KD figure or no user figure at: boot, ok, game, done; routes that read nothing: ...
  game-memory      PASS    free USER 91,226,112 B, CDRAM 117,440,512 B in the game before its first block
```

| diagnostic | WARN means |
|---|---|
| `probe` | the display hook could not write the game's `USER_RW`; the paths that did work are listed |
| `capture<64KiB` | a capture block over 64 KiB: USER_RW refused it and CDRAM (256 KiB units) or phycont (1 MiB) gave one |
| `kernel-pool` | the ladder's last rung, the kernel strip (at most 64 KiB, one call), carried a stage's capture |
| `kernel-memory` | no KD or no user figure at some step; each route that read nothing is named with its rc and step |
| `game-memory` | the game's free memory could not be read |
| `parallel` | textbox (with the helpers) and textbox2 (serially, the same region) read different texts: the parallel path read differently on this console, or the screen changed between the stages; the row also gives both recognition times |

Then come the memory routes per step (a WARN cell gives the route's native result and the step it
stopped at), the game worker's memory per allocation, the findings, a `diagnostics:` line (`PASS`,
or `WARN (rows)`) and the verdict: `PASS` (it names any diagnostics that warned),
`FAIL: <rows> failed`, `NO TEXT: ...` (no FAIL, but no line in your region), or
`INCOMPLETE: stage '<name>' began and never wrote its record` (or `INCOMPLETE: the stages were
still waiting for text in your region`). A stop
is the run's failure in whichever stage it happens, the probe included, and never a WARN. An API
18 (`selftest v1`) report gets the same table for its steps (`boot`, `ok`, `game`, `ocr`, `done`),
with its memory routes as diagnostics; for #74 it says `ocr FAIL: never reached: the capture
failed 5 time(s) (no memory)` and names the 2,088,960 B kernel buffer.

Exit status follows the verdict: 0 for PASS (diagnostic WARNs included), 1 for FAIL, NO TEXT or
INCOMPLETE, 2 for a file that is not a report. `--json` gives `ocr_result` (`PASS`, `FAIL`,
`NOTEXT` or `INCOMPLETE`), `diagnostics_result`,
`warnings` and each row's `kind` (`ocr` or `diagnostic`).

## Memory headroom and the model tier

After the diagnostics the analyser prints each OCR stage's **headroom**: the game's free USER
memory at the job's memory check, plus another model's resident weights the job released first,
minus the model size's reserve (4, 8 or 16 MiB for Small, Normal and Large since API 21; 4 MiB
before), the job's workspace and capture block and the weights it had to load. A
negative headroom is a refused job. The Shell's `game vocr memory:` log line carries the same figure
for every OCR job, not only the self-test's.

Then a **model tier** table: the Small / Normal / Large recognizer tiers of
[`tools/model_tiers.json`](../tools/model_tiers.json), the one table the Rubi app's tier choice
reads as well, so the analyser and the app cannot price different tiers. Each tier is priced at its
ceilings, an estimate, with vita-vn-ocr's formula (`docs/model-tiers.md` there, section 1.5):
page(weights) + page(max(the line finder's 83,323 B arena, the tier's W8I arena) + 3,760 B of
runtime structs) + the 65,536 B capture block (today's `VJO_BUDGET_GAME_CAPTURE`; that section has
API 19's 61,440 B) + the 516,096 B line-block ceiling, plus the tier's own reserve (4 / 8 /
16 MiB), against the free USER the game had before the worker's first block.
The recommendation is the largest tier whose first job fits: it is one sample in one game, and
memory alone. It prices each tier at its ceilings; the game worker checks the pinned model of
each size (`model`, `job_bytes` in the same table), which needs less, and logs each size that did
not fit as a `tier ... did not fit` line (the collect summary counts them). The weights ceilings are the owner's of 2026-10-14 (Small under 1 MB, Normal at most
1.6 MB, Large at most 2.5 MB of int8 weights); the box time is each tier's limit for a typical full
textbox, 3 lines of about 640 px on one Cortex-A9 core at 444 MHz, estimated, and wins where it is
tighter. A table the analyser cannot read leaves the tiers out with a finding; the verdict never
depends on them.

| tier | weights at most | box time | W8I arena ceiling | first job needs | reserve | free USER needed |
|---|---|---|---|---|---|---|
| Large | 2,500,000 B | < 3 s | 1,310,720 B | 4,399,104 B | 16 MiB | 21,176,320 B |
| Normal | 1,600,000 B | < 2 s | 262,144 B | 2,449,408 B | 8 MiB | 10,838,016 B |
| Small | 999,999 B | < 1 s | 131,072 B | 1,720,320 B | 4 MiB | 5,914,624 B |

## Collecting everything: `--collect`

```sh
bash tools/install_ftp.sh --collect VITA_IP
```

With VitaShell's FTP running, this fetches, **read-only** (the Vita receives only the login and
`PWD`, `CWD`, `TYPE`, `PASV`/`EPSV`, `LIST`, `SIZE`, `RETR` and `QUIT`; nothing that writes, renames or
deletes; `tools/collect_vita.py`; the offer to archive old crash dumps after it is a separate step, taken
only on a yes: [below](#archiving-older-dumps-off-the-vita)), into one timestamped folder
`build/device/collect/vjo-collect-YYYYMMDD-HHMMSSZ/` and a zip of it next to it:

- `ux0:data/VitaJPOverlay/`: `log.txt`, `log.old.txt`, `selftest-report.txt`, `selftest.txt` (the
  marker, when the test is still armed), `status.txt`, `status.prev.txt`, `kernel.txt`,
  `kernel.prev.txt`, `config.ini` with every `*api_key` and `anki_audio_url` value (an audio source
  URL can carry a key; the setting was removed with AnkiConnect, but an older `config.ini` can still
  hold it) replaced by its length before anything is written (also a commented-out one, a word that
  starts with a removed online dictionary's key name, `jpdb_api_keyKEY`, and a key line without `=`;
  the text after either name in the logs' config warnings is masked too), `games.ini`, `region.ini`,
  the whole `anki_queue/` with its `exported/` cards, and the listing (names and sizes only) of
  `anki/`, the Rubi app's `.apkg` packages, `vjo_app.txt`, the record of the Rubi app the plugins came
  with, and `installed.txt`, the one-VPK install's record ([one-vpk-install.md](one-vpk-install.md)), which
  the analysis compares with the package's `payload-manifest.txt`;
- `ux0:app/VJOSET001/vjo_app.txt`, the installed Rubi app's version stamp, and the SHA-256 of its
  `eboot.bin` (hashed, never kept; [core/app_stamp.h](../core/app_stamp.h)). Both are read as `--check-app`
  reads them: through the folder (curl's CWD into each folder on the way), then, when that is answered "not
  there", by the file's full path without entering the folder (`SIZE` and `RETR` of
  `ux0:/app/VJOSET001/<file>`). test.3's collect could not enter that folder, most likely because the Rubi
  app was open beside VitaShell, while the Shell read the stamp in it ([#138](https://github.com/bee-san/Rubi_for_vita/issues/138));
  `COLLECT.json`'s `app` says which way read each file. When neither can be read, the app's folder is
  listed, and, when it cannot be entered, `ux0:app`, of which only whether it lists `VJOSET001` is kept;
- `ur0:data/VitaJPOverlay/boot.txt`; `ur0:tai/config.txt` and `ux0:tai/config.txt` with the
  installers' `.vjo-bak` copies, their dated backups (`config.txt.vjo-YYYYMMDD-HHMMSS.bak`) and an
  interrupted swap's `config.txt.vjo-new` / `.vjo-old`; every `ux0:data/*.psp2dmp` crash dump, with its
  LIST time ([crash dumps](#crash-dumps));
- the listing of each directory, and the SHA-256 of the installed plugins, RCO, settings VPK, OCR
  models, the beam + LM decoder's LMs (`*.vlm`), their `LM-ATTRIBUTION.md`, the model sizes'
  `MODEL-ATTRIBUTION.md`, the text-presence gate's weights (`*.vtd`) and `GATE-ATTRIBUTION.md`, and scan
  screen's text spotter weights (`*.vtsp`) and `TEXTSPOT-ATTRIBUTION.md` (downloaded to a
  temporary file and deleted, never kept), compared with the package's `package-manifest.json`; the
  dictionary is compared by size.

Each file keeps its device path in the folder (`ux0/data/VitaJPOverlay/log.txt`, ...); `COLLECT.json`
lists every file with its bytes and SHA-256, every listed file with its LIST time by the Vita's clock
(`date`), what was not on the Vita and every error, and the
package's version, defaults and install settings, with each API key and the audio source URL only as
`set` or empty. Then
`analyse_selftest.py --collect` writes `analysis.txt` and `analysis.json` into the folder and prints
the summary: whether the installed files match the package (a file it does not ship, such as a
removed engine's model, is listed as not in this package), whether the installed Rubi app is the
one the plugins came with (an app from before the stamps, v0.8.0-test.2 and older, is recognised by its
eboot.bin without one; "not installed" only when a listing shows the folder or its `eboot.bin` is not
there, or the Shell's check found neither; when the collect could not read the app, the Shell's own check
at a game start of the log's last boot answers for it, with where in `log.txt`, if it compared the app
with the same record), the configuration (with a note for any `ocr_backend` line, which is no setting
any more since VOCR is the only engine: a removed engine's value, which an older app writes, or another,
and that today's app or installer deletes the line), where taiHEN loads
each plugin (the worker under `*TITLEID` only, a line #138's kernel no longer needs), the boot record, the kernel's start and hooks, every
OCR job (results, models and fallbacks, capture route, find/load/recognize times, per-line and
per-frame times, free USER and headroom, failed worker steps), the ARM clock by game window (each
window's clock before, during and after, a boost-off window's as assumed, and the time per CTC frame
with `ocr_boost_clock` off against raised, over all jobs and like for like, by window, with a one-line
`boost effective:` verdict; [ocr-speed.md](ocr-speed.md#what-the-log-shows-api-22)), the dictionary jobs, the overlay's
slowest frame, Anki saves and each export's result (a failed export, or one that left cards queued for a
problem, is also among the problem lines; a successful one is not), the config warnings counted once
each, the Shell's own checks of the
Rubi app at game starts, the lines that report a problem, the Anki queue, the crash dumps, each dated
against the boot ([below](#crash-dumps)), the
model tier from each free-memory sample, the full self-test analysis when a report is there, and what
it cannot decide, with where in the zip the data to decide it is. The zip is the one file to send.

## Crash dumps

When a process crashes, the system writes a memory image of it to
`ux0:data/psp2core-<time>-0x<pid>-<executable>.psp2dmp`. `<time>` is the crash's Unix time by the
Vita's clock, `0x<pid>` is the process, and `<executable>` is `shell.self` for SceShell or `eboot.bin`
for an app or a game. The card keeps every dump until something deletes it. test.3's card held ten,
from October 10 to 13 (#138): the oldest came from before v0.7.0, and none came from test.1, test.2 or
test.3.

The collect fetches every dump and lists `ux0:data`. Its analysis then gives each dump's crash time,
process, pid and size, and places it as `NEW (this boot)`, `before this boot` or `not placed`. "This
boot" is the last one the overlay recorded, normally the one the collect ran in. A dump from this
boot is also named among the things the analyser cannot decide. The analysis says `before this boot`
only when nothing points the other way, because those dumps can be archived off the Vita
([below](#archiving-older-dumps-off-the-vita)).

- **This boot's start, by the Vita's clock.** VitaShell's LIST gives each file's time to the minute
  (libftpvita `gen_list_format`: the month, the day and `HH:MM`, or the year for another year than
  the Vita's current one). The year of an `HH:MM` time comes from a dump's name, or from this
  computer's clock when no dump names it.
  - This boot began after the previous boot's last `kernel.txt` write. The kernel renames that file
    to `kernel.prev.txt` once per boot, the first time the Shell registers or marks a stage, and a
    rename keeps a file's time. test.1's listing shows that for the Shell's own rename: its
    `status.prev.txt`, renamed as that boot's Shell plugin started, is dated 15:55, while that boot
    began at about 16:03–16:04 (its `kernel.prev.txt` 16:03, its `boot.txt` ok mark 16:05). No
    listing shows it for the kernel's rename, so the analysis opens the window 2 minutes before
    `kernel.prev.txt`'s time.
  - `status.prev.txt` is not used: the Shell plugin rotates `status.txt` every time it starts,
    including in a SceShell restarted after a crash.
  - This boot began before `boot.txt`'s last write. When the record says the boot reached its ok
    mark (`stage=4`, no fault, not inert), that write came at least 55 s into the boot
    (`kernel/boot.c`). Without `kernel.prev.txt`, the start is estimated as at most 3 minutes
    before that mark, and without that mark too, it is not known.
  - The clock was set back between the boots when `boot.txt`'s last write is dated before
    `kernel.prev.txt`'s, or less than 55 s after it for an ok mark. It was set back during this
    boot when this boot's `kernel.txt` or `status.txt` (both rewritten every boot) was last written
    before the boot began. Nothing is placed in either case.
- **The clock, checked at `kernel.txt`'s last write.** The kernel's foreground and game lines carry
  the uptime (`t=… ms`, the system time since power-on), and so does the debug build's `input block=`
  line. When `kernel.txt`'s last line is one of them, its LIST time less that uptime is this boot's
  start by the clock at that write. test.3's `kernel.txt`, last written at 07:15 and ending at
  878,340 ms, puts its start at 07:00:21–07:01:21, inside its window.
  - A final `game exit` may follow a game's exit or suspend line, as the worker's own line for
    that event. A `game exit` alone, though, comes from any app that exits without a line of its own
    (the Rubi app closed, VitaShell), at any later time. And the hook raises the worker's flag
    before it logs its own line (`kernel/lifecycle.c` `fg_store`), so the event's `game exit` can
    come first: test.3's `kernel.txt` has that order at lines 104–105.
  - The kernel logs every foreground line, and every `game pid … active` and `pid … gone` or
    `is not a game` line, while it holds its foreground lock, and it raises that flag within the
    hold. So an event's `game exit` logged before its own line lies after the last such line before
    it. The final `game exit` counts as the line's own only when no other `game exit` lies between
    them; otherwise the clock is not checked.
  - A start before the window means the clock was set back during this boot, or between the boots,
    and nothing is placed.
  - A start more than 2 minutes after the window means the clock went forward, which may hide a
    set-back before it, or the Vita slept, should the uptime stop in standby (the wiki's Suspend
    page has the system time in the context the kernel saves; not measured). The clock is then not
    checked.
  - The clock is not checked either when `kernel.txt`'s last line carries no uptime, when it reached
    its 256 KB cap, when it is not this boot's whole log (below), or when the window is only an
    estimate.
  - The summary says whether the clock was checked, and how.
- **The pids of this boot.** The analysis uses `kernel.txt` only when it is this boot's whole log:
  it starts with the boot guard's first line, and its `boot guard: boot N` is `boot.txt`'s `seq=N`.
  Otherwise, the kernel's 4 KiB ring dropped lines before its first write, or it is another boot's
  log. `kernel.txt` registers each SceShell (`shell registered: pid ...`), a restarted one
  included. Its other lines name the games' pids and others (the foreground transitions, the input
  lines); the analysis takes every pid any line names.
- **How a dump is placed.**
  - `before this boot` needs all three: a date before this boot began, the clock checked, and a pid
    that `kernel.txt` never names. A named pid is not placed: the pid repeated, or the clock went
    back during the boot. With the clock not checked, no dump is placed before this boot.
  - A dump dated after this boot began is from this boot, unless it is a SceShell whose pid this
    boot never registered. Such a dump is not placed. It may be from an earlier boot under a clock
    set back since: test.3's last dump, from October 13 14:09, is dated after test.3's boot by a
    clock about three days behind. It may also be from a later boot that the overlay did not record,
    one started with L held.
  - A dump dated within the start window is not placed. The exception is this boot's own SceShell
    when `boot.txt` records its exit (`fault=1`). A match with the previous boot's SceShell pid
    (`kernel.prev.txt`) is only noted, because pids repeat across boots.
- **What the clock check cannot see.** A set-back after `kernel.txt`'s last write, or one put right
  again before it, leaves no trace in these times. Should the uptime stop in standby, a standby
  during the boot would also offset a set-back of about its own length. The rule for a final
  `game exit` also takes `kernel.txt` to hold every line the kernel logged: the kernel reads up to
  512 B of its log before it opens the file, so a write that fails loses them (`kernel/main.c`
  `kernel_file_flush`), and were a raced `game exit` among them, a later lone one would count as
  its line's own. No collect so far shows a lost line. A crash after such a set-back, of a process
  whose pid `kernel.txt` does not name, is then dated, and placed, before this boot. The kernel
  names no pid for the Rubi app, VitaShell or a system app, nor for a game started after its
  last write. An archived dump stays in the collect's folder and zip
  ([below](#archiving-older-dumps-off-the-vita)).
- **Notes the analysis adds.** A game's pid is named with the game. A dump dated after this boot's
  newest `kernel.txt` write gets a note: it crashed after it, or the clock was set back since. The
  summary prints that write's time beside this computer's at the collect, so a wrong Vita clock is
  plain to see.

The analyser does not decode a dump. vita-parse-core (xyzz/vita-parse-core, Python 3.9 with
pyelftools 0.24, and VitaSDK's `arm-vita-eabi-addr2line` and `objdump`) does, given the ELF of the
crashed module, which `cmake --build build/vita` leaves as `build/vita/VitaJPOverlay_Shell` (and
`_OCR`, `_Kernel`, `_Settings`).

1. Find which build ran from the dump's own module list. Each module's segment sizes are the
   `p_memsz` of the program headers of that build's SELF (`.suprx`, or the VPK's `eboot.bin`).
2. Rebuild that tag with the pinned VitaSDK. Check that the SELF it makes is byte-identical to the
   release's.
3. Run vita-parse-core with that ELF for the module it belongs to. Its `main.py` hands every address
   to the one ELF it is given, and adds `addr2line` only for a module named `*.elf`. For a plugin's
   frames, symbolise the addresses in that module alone.

The decode of test.3's ten dumps, posted on #138:

- two are v0.7.7's Rubi app, crashing in libpng's jump buffer while it loads an icon (fixed in
  v0.7.8, [settings-png-runtime.md](settings-png-runtime.md));
- six are SceShell out of physical pages, `ScePaf` aborting with `0x80024302` in firmware threads,
  #59's pattern;
- two are a remote-control agent's thread inside the pngshot plugin, neither of them ours.

### Archiving older dumps off the Vita

The collect itself writes nothing. After it, the dumps it placed before this boot can be archived:
this collect keeps them, and they are deleted from `ux0:data`. The next collect then shows only the
dumps that were kept. In a terminal, `--collect` asks after printing the analysis. The default,
Enter, leaves them on the Vita, and so does running without a terminal or with `VJO_NO_PROMPT` set;
the collect then prints the command:

```sh
bash tools/install_ftp.sh --archive-dumps VITA_IP
```

It archives from the newest collect under `build/device/collect/`, and only when that collect is of
this Vita (its `COLLECT.json` names the same address). It places the dumps again from that collect's
own records, never from an `analysis.json` that can be edited or that an older analyser wrote. It
deletes a dump only when all of these hold:

- the dump is placed before that collect's boot;
- it has the system's own name (`psp2core-<digits>-0x<hex>-<name>.psp2dmp`, directly in `ux0:data`);
- that collect's copy is the file it fetched (its bytes and SHA-256 in `COLLECT.json`);
- a new listing shows the Vita's file with the size and LIST time of that collect's listing
  (`listings/ux0_data.txt`, also of a collect made before `COLLECT.json` recorded the times).

Besides the login, the passive data connections and the listings, it sends `DELE` for each such
dump and no other command that writes. It then lists `ux0:data` again: that listing, not `DELE`'s
answer, says whether each one is gone, also when the connection dropped before the answer came. It
prints and appends to the collect's `archive.log` every deletion and every dump it left, with the
reason. A dump from this boot, or one not placed, stays on the Vita until a later collect places it.
An archived dump is not lost: the collect's folder and zip keep the copy it checked, so a dump
placed before this boot by mistake ([the clock check's limit](#crash-dumps)) can still be sent.

## Cost and limits

- Shell module pages: 299,008 B, unchanged by API 19 (the stages share the job's one result
  buffer; `vjo_capture_raw` was removed).
- The report is capped at 48 KiB and written a record at a time, one append per file (a stage's
  end, about 25 lines, in one or two; test.2 wrote it line by line, each line appended to the
  report and `log.txt` in turn: 0.4-2.4 s a stage record, 13.5 s over its 17 stage runs,
  [first-ocr.md](first-ocr.md)); it is also copied into `status.txt` and `log.txt`.
- It does not launch the game: the owner does (the expert's rule against automating risky
  actions). It runs one job of five stages, once per arming, after your first scan with text.
- The game's exact partition figure is not sampled by the kernel; the game's free `USER`, CDRAM
  and phycont come from the worker's own `sceKernelGetFreeMemorySize` before and after each
  allocation (`gstep ... main= cdram= phycont=`).
- Unmeasured until the first run: whether the display hook can write each memory type on the
  owner's 3.65 (the probe answers it), the capture's time per strip (`capture_ms`), and which
  kernel memory route reads there.
