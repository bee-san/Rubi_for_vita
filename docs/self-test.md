# The on-device self-test

Every console test so far ended with the same two questions: did it boot, and what ran out of
memory. The self-test answers both from one file, in the owner's two commands, and tells the
fleet exactly which step failed and with how much free memory in which pool.

Since API 19 (#74) one run also exercises the whole in-game capture: the capture probe, the
fallback model, the configured model, the full screen and a warm second recognition, each stage
with its own record whether it passed or not ([ingame-capture.md](ingame-capture.md)).

## Running it

1. Arm it (VitaShell FTP running): `bash tools/install_ftp.sh --self-test VITA_IP`. This uploads
   one empty file, `ux0:data/VitaJPOverlay/selftest.txt`.
2. Reboot. Wait for the LiveArea and leave the Vita alone for a minute (the boot guard's `ok`).
3. Start the game to test. Reach a screen with dialogue text and wait about 30 seconds without
   pressing anything: on the first settled screen one OCR job runs every stage below in turn.
   OCR runs automatically while the test is armed, whatever `ocr_mode` says; the region is
   **yours** (draw it with Square first if you have not; with none, the full screen). The game
   needs the OCR worker registered for it (installed with `--title TITLEID`, README 'Install');
   otherwise the stages report `rc=-117` (`ocr unavailable`).
4. Back in VitaShell: `bash tools/install_ftp.sh --status VITA_IP` fetches
   `ux0:data/VitaJPOverlay/selftest-report.txt` (and `status.txt`, `kernel.txt`, `boot.txt`).
5. `python3 tools/analyse_selftest.py build/device/status/selftest-report.txt` prints one
   PASS/FAIL row per stage, the free memory every route read at each step, the game worker's
   memory around each allocation, the findings and a verdict. Attach the report to the issue.

| stage | job | what it shows |
|---|---|---|
| `fallback` | your region, the fallback model forced | FL10 read from the card and recognizing |
| `textbox` | your region, the configured model | H15 read from the card (its load time), the strip capture, the finder and the recognizer |
| `fullscreen` | the whole frame | the full screen through the strips, H15 kept resident |
| `probe` | each capture path into each memory type, one strip | whether the display hook can write the game's `USER_RW`, `CDRAM` and `PHYCONT` memory, the kernel strip, and a user-mode `sceDisplayGetFrameBuf` read; each with its rc and the rows' checksum |
| `textbox2` | your region again | the warm time; its text is the job's result |

The marker is removed after the last stage, so one arming is one run, failed stages included
(#74: the API 18 test removed it only after a successful recognition, stayed armed after the
capture failed, and kept forcing the full-screen region). If the screen changes during the
stages, the job is cancelled and they run again on the next settled screen. A run that stopped
part way (a crash, the power) leaves the marker: the next boot runs the test again, the report
shows the stage that began and never wrote its record, and `status.txt` keeps the previous boot's
copy as `status.prev.txt`.

If the game is disabled in the **Overlay** app (**JP Overlay Settings** in older builds), the
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
kernel boot ledger=131072/131072 limit=2220032 capture=0
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
| `kernel` | `vjoMemInfo` | the kernel ledger and the capture block held (Lens/Anki only; OCR holds none) |
| `stage` | the Shell | `begin` with the model and flags; `end` with the rc, time, region, frame (size/pitch/format), model, fallback, lines |
| `gstep` | the game worker's step log | each step of the job: `mem`, `check`, `alloc`/`free` (memory type, block, native rc, bytes), `pass` (a capture pass), `sig`, `find`, `load`, `recognize`, `fallback`, `fb`, `probe`, each with the game's free main/CDRAM/phycont after it |
| `line` | the game worker | each recognized line: size, CTC frames, time |
| `result` / `memory` | the game worker's result | capture source and memory, block size, strip rows, strips read, rows that changed between reads, timings; the job's memory check |

The kernel routes are looked up at run time by library and function NID through taiHEN's
`module_get_export_func`, never imported, and only when the Shell asks; nothing of this runs at
boot or in a hook. #74's 3.65 console returned `0x80020001` from the block route at every step;
the analyser reports such a route as failed, never its zeros as figures.

## What the analyser says

`tools/analyse_selftest.py` (tests: `tools/tests/test_analyse_selftest.py`, with #74's own report
as `tools/tests/fixtures/selftest-74-report.txt`, and v2 reports written by the real
`shell/selftest.c` on the host, `host/tests/selftest_writer.c`):

```
  stage            result  detail
  boot             PASS    normal boot (seq 2, 0 failed boots before)
  ok               PASS    stable minute at 60999 ms
  game             PASS    PCSG00415 enabled, region user 1536,48188,62600,15059
  fallback         PASS    917x125+22+400 FL10_w8.vocr, 3 line(s), capture hook into user 61,440 B ...
  textbox          FAIL    out of memory (rc -103); failed steps: alloc user/workspace 0x80024302 ...
  fullscreen       PASS    960x544+0+0 H15_w8.vocr, 5 line(s), ...
  probe            PASS    worked: hook->user, kstrip->user, user->user, hook->cdram, ...
  textbox2         PASS    ...
  capture<64KiB    PASS    largest capture block 61,440 B (limit 65,536 B)
  kernel-pool      PASS    every capture was written by the display hook into the game's memory
  recognitions     PASS    11 line(s) recognized in all
  tearing          PASS    0 row(s) changed between two reads (the job reads again)
  kernel-memory    PASS    the user API and a KD figure at every step
  game-memory      PASS    free USER 91,226,112 B, CDRAM 117,440,512 B in the game before its first block
  done             PASS    at 155499 ms, the marker removed
```

then the memory routes per step, the game worker's memory per allocation, the findings and the
verdict: `PASS`, `FAIL: <rows> failed`, or `INCOMPLETE: stage '<name>' began and never wrote its
record` (the Vita stopped during that stage). An API 18 (`selftest v1`) report gets the same
table for its steps (`boot`, `ok`, `game`, `ocr`, `done`); for #74 it says `ocr FAIL: never
reached: the capture failed 5 time(s) (no memory)` and names the 2,088,960 B kernel buffer.

Exit status 0 when every row passed, 1 otherwise, 2 for a file that is not a report.

## Cost and limits

- Shell module pages: 299,008 B, unchanged by API 19 (the stages share the job's one result
  buffer; `vjo_capture_raw` was removed).
- The report is capped at 48 KiB and written line by line; it is also copied into `status.txt`.
- It does not launch the game: the owner does (the expert's rule against automating risky
  actions). It runs one job of five stages, once per arming.
- The game's exact partition figure is not sampled by the kernel; the game's free `USER`, CDRAM
  and phycont come from the worker's own `sceKernelGetFreeMemorySize` before and after each
  allocation (`gstep ... main= cdram= phycont=`).
- Unmeasured until the first run: whether the display hook can write each memory type on the
  owner's 3.65 (the probe answers it), the capture's time per strip (`capture_ms`), and which
  kernel memory route reads there.
