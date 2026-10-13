# The on-device self-test

Every console test so far ended with the same two questions: did it boot, and what ran out of
memory. The self-test answers both from one file, in the owner's two commands, and tells the
fleet exactly which step failed and with how much free memory in which pool.

## Running it

1. Arm it (VitaShell FTP running): `bash tools/install_ftp.sh --self-test VITA_IP`. This uploads
   one empty file, `ux0:data/VitaJPOverlay/selftest.txt`.
2. Reboot. Wait for the LiveArea and leave the Vita alone for a minute (the boot guard's `ok`).
3. Start the game to test. Reach a screen with dialogue text and wait about 30 seconds: the
   self-test recognizes the first settled screen at the **full-screen region** (the largest a job
   can need) with the configured model, whatever `ocr_mode` and the saved region say.
4. Back in VitaShell: `bash tools/install_ftp.sh --status VITA_IP` fetches
   `ux0:data/VitaJPOverlay/selftest-report.txt` (and `status.txt`, `kernel.txt`, `boot.txt`).
5. `python3 tools/analyse_selftest.py build/device/status/selftest-report.txt` prints the steps
   reached, every pool's free memory at each step, the findings and a verdict. Attach the report
   to the issue.

The marker is removed when the OCR step is written, so one arming is one run. A run that did not
get that far leaves the marker: the next boot runs the test again, and the report shows how far
the previous one got (the report is truncated at each boot, `status.txt` keeps the previous
boot's copy as `status.prev.txt`).

If the game is disabled in the **Overlay** app (**JP Overlay Settings** in older builds), the
report says so and no OCR runs. If the boot is a safe one (the previous boot did not complete),
the report has the boot sample only and says so; reboot once more.

## What it writes

`shell/selftest.c` writes one line per event; every step has a memory sample:

```
selftest v1 api=18
boot seq=7 mode=0 prev=0/4/0/0 failures=0 write=0x00000000
config backend=2 model=H15_w8.vocr fallback=FL10_w8.vocr dictionary=3 anki=0
step boot t=12
mem boot shell_main=1200000 shell_cdram=4194304 shell_phycont=0 rc=0x00000000 paf_free=2557952
ledger boot pages=532480/532480 paf=8192/8192
kernel boot kd_total=37748736 kd_free=9000000 kd_rc=0x00000000 shell_total=52428800 shell_free=1265536 shell_rc=0x00000000 ledger=131072/131072 limit=2220032 capture=0
step ok t=60012
...
step game t=95000 title=PCSG00415 enabled=1 mode=1 region=full
...
step ocr t=99000 rc=0 model=H15_w8.vocr fallback=0 960x544 lines=3 recognized=3 frames=330
game free_user=91226112 reserve=4194304 need_workspace=712704 need_weights=1159168 release=0 workspace=709936 resident=1159168 weights=1 io=0x00000000 stat=0x00000000 cleanup=0 capture_rc=0x00000000 find_ms=120 load_ms=900 recognize_ms=640
...
step done t=99100
```

| line | source | meaning |
|---|---|---|
| `mem` | `sceKernelGetFreeMemorySize` in SceShell | SceShell's budget as the user API reports it (main, CDRAM, phycont, each minus a system margin [SDK: Kernel-Reference]); `paf_free` is the PAF heap's `GetFreeSize()` |
| `ledger` | the Shell ledger ([memory-ledger.md](memory-ledger.md)) | current/peak bytes we hold in SceShell's partition and in its PAF heap |
| `kernel` | `vjoMemInfo` (API 18, `kernel/meminfo.c`) | exact free/total of the partition the kernel module lives in (`kd_*`: ScePhyMemPartKD when the total is 36 MiB; the analyser checks) and of SceShell's partition (`shell_*`, exact, next to the user view), the kernel ledger and the capture block held |
| `game` | the game worker's result | the game's free `USER` and what the job needed, the blocks it held, the model I/O status, timings |
| `fail` | the Shell | a step that could not complete, with the kernel's code (`capture rc=0xBFFFFFFE`: no memory for the capture block in ScePhyMemPartKD) |

Steps: `boot` (the Shell's worker started), `ok` (the boot guard's stable minute), `game` (the
first game started, its title, whether it is enabled), `ocr` (the first job's result), `done`.
Exit is recorded as `step exit` when it happens before `done`.

The kernel figures come from `sceKernelGetPhyMemPartInfoCoreForKernel`, whose NID changed at
firmware 3.63; it is looked up at run time by library and function NID (0x63A519E5/0x3650963F on
3.60-3.61, 0x02451F0F/0xB9B69700 on 3.63-3.74 [wiki: SceSysmem; PSVshell `src/main.c`]) through
taiHEN's `module_get_export_func`, never imported, and only when the Shell asks. The partition
object comes from `ksceKernelMemBlockGetInfoEx` of a block we own (our own module pages name the
kernel partition; the Shell's out pointer names SceShell's). Nothing of this runs at boot or in a
hook.

## What the analyser says

`tools/analyse_selftest.py` (tested in `tools/tests/test_analyse_selftest.py`):

- the step reached, and which step was never written: `INCOMPLETE: the run reached 'game' and
  never wrote an OCR result`;
- SceShell's free partition under 512 KiB at any step, with the #59 reference; a PAF heap under
  the 1.5 MiB reserve (every Shell result allocation refused);
- the game's free `USER` against the job's need, and whether an out-of-memory was the **game's**
  (a smaller model or region fits) or the kernel's capture block;
- a kernel partition whose total is not 36 MiB (the KD figure is then unconfirmed);
- the fallback model, model I/O errors (ENODEV, ENOENT), a game worker that could not free its
  memory, a disabled title, a safe boot;
- the memory the run added between boot, game start and the OCR result, per pool.

Exit status 0 is a pass (boot, game start and an OCR job at the full screen completed), 1
otherwise, 2 for a file that is not a report.

## Cost and limits

- Shell module pages: 397,312 B, down from 405,504 B (the self-test's 2,000 B of code and the
  ledger's were paid for by building the cold files with `-Os`: configuration, request building,
  parsers, queue files, bootstrap; lookup, OCR text handling, JPEG and TLS stay at `-O2`).
- The report is capped at 32 KiB and written line by line; it is also copied into `status.txt`.
- It does not launch the game: the owner does (the expert's rule against automating risky
  actions). It does not run more than the first job.
- The game's exact partition figure is not sampled (the game process makes no syscall for it);
  its free `USER` comes from the worker's own documented check.
- Unverified until the first run: that `module_get_export_func` finds the partition function on
  the owner's 3.65 (otherwise `kd_rc`/`shell_rc` say why, and the user view is still there).
