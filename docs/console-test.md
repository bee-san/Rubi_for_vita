# Console test of a test build on a PS Vita: one zip, one run, about 20 minutes

A test build is a GitHub **pre-release** of this fork (tag `v0.8.0-test.N`, never "Latest") with the
one-VPK install, `Rubi-TAG-debug.vpk` (the **Rubi** app with the three plugins, the OCR models
of the three model sizes, VOCR5-S, VOCR5-N and VOCR5-L, and the frozen dictionary inside it;
[one-vpk-install.md](one-vpk-install.md)), and the zip of the same build with the installer and the
analyser (`--collect`). Its install issue names the tag and gives both SHA-256s. Since #138 the kernel
plugin loads the OCR worker into every game that is ON in the Rubi app, so no game is named
anywhere. "est." marks an estimate; nothing of a test build has run on a console before the owner's
run. The install issue follows [test-build-issue.md](test-build-issue.md).

## What you need
- A PCH-1000/1001 on 3.60–3.74 with HENkaku/Ensō or h-encore, and VitaShell.
- CLANNAD (PCSG00415), ON in the Rubi app → Games.
- A PC on the same Wi-Fi with bash, Python 3.9+ and curl.

## 1. Download and check
Download `Rubi-TAG.zip` from the pre-release, check it and extract it:
```sh
sha256sum Rubi-TAG.zip        # macOS: shasum -a 256; must equal the issue's SHA-256
unzip -q Rubi-TAG.zip -d vjo && cd vjo
python3 tools/package_release.py --verify .
```
The last line must say `Verified N package files`. `checksum failed` means a bad download: delete it
and download again.

## 2. Install
The one-VPK install: copy `Rubi-TAG-debug.vpk` to the Vita, install it in VitaShell, open
**Rubi**, choose **Install** on its Install page and restart the Vita (test-build-issue.md section 2 has
the checklist; the README's [Install](../README.md#install) the steps). The steps below are the advanced path, from a PC.

On the Vita, close any game, open VitaShell and press **Select** to start FTP (Select starts FTP once
**SELECT button** is set to **FTP** in VitaShell's settings, Start; a Vita's default is USB). Then, in `vjo`:
```sh
bash install-vita.sh VITA_IP:1337
```
- [ ] **Step 1, every time: install the Rubi app again.** In VitaShell (press OK on its FTP dialog to
  keep FTP running) open `ux0:data/`, install `Rubi.vpk`, then on the PC check it:
  ```sh
  bash install-vita.sh --check-app VITA_IP     # must print "Rubi app: up to date"
  ```
  The installer ends with a box saying the same. In #101 the app was not reinstalled, so test.1's app ran
  beside test.2's plugins: it still showed VOCR2, had no Anki page and wrote `ocr_backend = vocr2`.
  The plugins notice an app of another build at the next game start, and the first overlay opened then
  says `Rubi app and plugins are different builds: open the Rubi app.` once (`core/app_stamp.h`).
- [ ] Arm the self-test: `bash tools/install_ftp.sh --self-test VITA_IP`.
- The installer first backs up both taiHEN configurations on the PC, in `build/device/backup/`. On the
  Vita it backs up each configuration it changes, next to its `config.txt`, as `config.txt.vjo-bak` and as
  a dated `config.txt.vjo-<date-time>.bak` that is never reused or deleted; one it leaves unchanged is not
  backed up there. It also keeps your `config.ini`'s values (the copy it replaced is in the same backup
  folder).
- It adds the kernel plugin under `*KERNEL` and the Shell plugin under `*main`, removes the OCR
  worker's lines older installs left (the kernel loads the worker now), and prints each change.
- It prints `package settings: ...` for the test build's own settings (`anki_mining=on`).
- Keep the screen on until it prints `Installed. Reboot the Vita, then leave it on for a minute. …`
  and the Rubi app's box after it; `Nothing was changed on the Vita` names the configuration it
  could not edit safely and why.
- It uploads the Rubi app's VPK to `ux0:data/Rubi.vpk` and its record to
  `ux0:data/VitaJPOverlay/vjo_app.txt` with the plugins; installing the VPK is the step above.

Manual install without the install script (VitaShell's file manager, over USB or FTP): first copy
`ur0:tai/config.txt` to `ur0:tai/config.txt.bak` and your `ux0:data/VitaJPOverlay/config.ini` aside
(the zip's replaces it), copy the zip's `ur0/` and `ux0/` folders onto the Vita's `ur0:` and `ux0:`,
copy the zip's `Rubi.vpk` (at its top level) to `ux0:data/` and install it with VitaShell,
delete `ur0:data/VitaJPOverlay/boot.txt` if it exists (the installer resets it, so a safe mode left by
an earlier build ends), then add `ur0:tai/VitaJPOverlay_Kernel.skprx` under `*KERNEL` and
`ur0:tai/VitaJPOverlay_Shell.suprx` under `*main` (and the same in `ux0:tai/config.txt` if that
file exists). The worker needs no line: the kernel plugin loads `ur0:tai/VitaJPOverlay_OCR.suprx`
into each game that is ON; never list it under `*ALL`. The self-test is armed
by an empty `ux0:data/VitaJPOverlay/selftest.txt` (what `--self-test` uploads); copy one there.

## 3. Run it (once)
1. Reboot. Wait for the LiveArea, then leave the Vita alone for 1 minute.
2. Start CLANNAD and go to a dialogue screen. Keep your saved textbox region; with none, draw it
   first: L+R, Square, drag around the text, Enter, Cancel. Wait 1 minute without pressing anything:
   the self-test runs its five stages (VOCR5-S, VOCR5-N, full screen, capture probe, VOCR5-N
   again) once, on this first launch, in the background, after the first scan that reads text in
   your region, and finishes by itself (screens without text, such as the game's opening notice,
   are not tested). It shows nothing and your scans never wait for it: opening the overlay or
   moving to the next line stops it, and it runs again later. A scan made while it runs waits for
   it to stop: about 0.2 s, at most about half a second (est.); v0.8.0-test.2 ran the
   stages inside your first scans, which took 9-15 s ([first-ocr.md](first-ocr.md)).
3. Read about 10 pages, pausing about 3 s on each. On 3 of them open the overlay (L+R), take a phone
   photo of the screen, then press Cancel. Optional: on one of them press Enter to save the word as an
   Anki card (it is saved offline on the card). Note any slowdown, stutter or crash.
4. Close CLANNAD, go back to the LiveArea and wait 30 s.

## 4. Send back one zip
Start FTP in VitaShell again, then in `vjo`:
```sh
bash tools/install_ftp.sh --collect VITA_IP
```
It reads everything the overlay wrote (logs, the self-test report, status/kernel/boot records,
`config.ini` with its API keys and audio source URL redacted, both taiHEN configurations and their backups, the
Rubi app's install record `installed.txt`, the Anki queue, crash dumps, the installed files' hashes) into one
folder and zip under `build/device/collect/`, writes nothing on the
Vita, and prints the analysis. Leave the Rubi app open in the background if it is: test.3's collect
could not enter its folder, most likely because it was open ([#138](https://github.com/bee-san/Rubi_for_vita/issues/138)).
The collect now also reads its two files by their full paths, and the analysis's `Rubi app:` line
says which way read them; whether VitaShell opens them that way is what this run shows (not yet seen on a
console). Send the zip it names (`Send this one file: ...`) and one line per step
saying what you saw ("overlay showed", "no OCR", "crash at step 3"...). Send it privately, not on a
public issue: API keys are removed, but it holds your queued Anki cards' sentences (text of a
copyrighted game), your taiHEN plugin list, your games' title
IDs and any crash dump (a memory image of the crashed process). Keep the photos private too.

The analysis dates each crash dump against this boot: `NEW (this boot)` for this run's, `before this
boot` for older ones, `not placed` when the records cannot tell ([self-test.md](self-test.md#crash-dumps)).
When there are older ones, the collect asks at the end whether to archive them off the Vita. Answer `y`
to delete them from `ux0:data` (the zip keeps them, and the next collect then shows only the ones kept),
or press Enter to leave them.

The analysis's self-test verdict is decided by the OCR rows alone; diagnostics marked WARN (the probe,
the memory routes) are measurements to send back, not failures.

## If something goes wrong
- **No overlay or no OCR:** run step 4 anyway; the analysis shows the boot lines (`shell API 19
  entered`, `worker started`) and where taiHEN loads each plugin. `BOOT GUARD: SAFE MODE` means the
  previous boot did not finish: reboot once (one normal retry is allowed), then do step 4.
- **Crash:** step 4 collects the crash dumps too, and its analysis marks this run's `NEW (this boot)`.
- **Won't boot:** hold **L** while powering on (taiHEN skips every plugin), start VitaShell FTP, run
  `bash tools/install_ftp.sh --uninstall VITA_IP` in `vjo`, reboot. If that fails, comment out the
  `VitaJPOverlay` lines in `ur0:tai/config.txt` (and `ux0:tai/config.txt` if it exists) with
  VitaShell, or copy `config.txt.vjo-bak` over `config.txt`.
- **Back to v0.7.12:** download `VitaJPOverlay-0.7.12.zip` from the v0.7.12 release, extract it and run
  `bash install-vita.sh VITA_IP:1337` there, then reboot (that older installer registers the worker
  under `*ALL`, as 0.7.12 always did).
- Run it **once**. Don't keep rebooting to retry; send what you have.

## Models
The Rubi app's OCR page has the **Model size**: Small (VOCR5-S), Normal (VOCR5-N, the
default) and Large (VOCR5-L). When the game has too little free memory for the chosen size a job
runs the next smaller one; the screen does not say so, `log.txt` does (`tier large did not fit in
PCSG00415 (need N, free F); using normal`). It applies when the overlay next opens.

## Anki (optional)
`anki_mining = on` in the test build: Enter in the overlay saves the shown word and its sentence as a card
in `ux0:data/VitaJPOverlay/anki_queue/`; nothing uses the network. In the Rubi app's Anki page,
**Export cards** writes them into `ux0:data/VitaJPOverlay/anki/<date>_<time>-<N>cards.apkg`: note the
time the page shows it took and the file's size, copy the file to your computer (VitaShell's FTP or USB
mode, or the SD2Vita card) and open it in Anki. Each card keeps a JPEG of the game's screen
(`anki_screenshot = on`, the default; the game worker makes it, [anki.md](anki.md#the-screenshot)):
check that the note's Picture shows the game, and note the `anki: screenshot rc=0 ...` line in
`log.txt` (its strips, encoding and write times and the JPEG's size: the first console figures for
them; only the debug build, the test builds' own, writes it: the normal build logs a failed
screenshot's reason only). One edge case to watch: the kernel's one worker thread saves the JPEG
(a close, a rename) and also reads `games.ini` for each game that starts
(#138, at most 500 ms). A game started while a slow save still runs can start without OCR; its
`worker for` line in `kernel.txt` then says `no answer from the kernel worker in 500 ms`, and a
restart of the game loads the worker. See [anki.md](anki.md).

## What it tells us
- **The capture in the game:** whether the display hook can write the game's own memory, per
  memory type (the `probe` stage), the strip capture's time (`capture_ms`), and that the kernel
  pool gave OCR nothing (`kernel-pool`).
- **Memory:** the game's free USER/CDRAM/phycont before and after each block, each stage's headroom,
  and the model tier (Small/Normal/Large, vita-vn-ocr #74's proposal) the game's free memory allows;
  SceShell's and the kernel pool's free memory by each of three query routes, from boot into the
  game and back.
- **Speed:** VOCR5-N's and VOCR5-S's load and recognition times on a console (est. until
  measured; no console has run a VOCR5 model before), the warm second recognition, and the time per line and per CTC frame of every job;
  each job's timeline (`python3 tools/analyse_timeline.py --collect DIR --jobs`: the game's job
  stage by stage, its exchange with the Shell, the log's appends, the first scan of each game
  start).
