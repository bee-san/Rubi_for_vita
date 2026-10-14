# Console test of a test build on a PS Vita: one zip, one run, about 20 minutes

A test build is a GitHub **pre-release** of this fork (tag `v0.8.0-test.N`, never "Latest") with a
single zip that has everything: the three plugins, the **Overlay** settings app, the OCR models
(VOCR3-H15 and VOCR3-FL10, plus v0.2.0's H15 and FL10), the frozen dictionary, the installer and
the analyser. It needs no git, no `gh` and no packaging. Its install issue names the tag and gives
the zip's SHA-256. The installer registers the OCR worker only for the games named with `--title`
(here CLANNAD, `PCSG00415`), never under `*ALL`. "est." marks an estimate; nothing of a test build
has run on a console before the owner's run.

## What you need
- A PCH-1000/1001 on 3.60–3.74 with HENkaku/Ensō or h-encore, and VitaShell.
- CLANNAD (PCSG00415), ON in the Overlay app → Games.
- A PC on the same Wi-Fi with bash, Python 3.9+ and curl.

## 1. Download and check
Download `VitaJPOverlay-TAG.zip` from the pre-release, check it and extract it:
```sh
sha256sum VitaJPOverlay-TAG.zip        # macOS: shasum -a 256; must equal the issue's SHA-256
unzip -q VitaJPOverlay-TAG.zip -d vjo && cd vjo
python3 tools/package_release.py --verify .
```
The last line must say `Verified N package files`. `checksum failed` means a bad download: delete it
and download again.

## 2. Install
On the Vita, close any game, open VitaShell and press **Select** to start FTP. Then, in `vjo`:
```sh
bash install-vita.sh --title PCSG00415 VITA_IP:1337
bash tools/install_ftp.sh --self-test VITA_IP
```
- The installer first backs up both taiHEN configurations: on the PC in `build/device/backup/`, and
  on the Vita as `config.txt.vjo-bak` next to each `config.txt`. It also keeps your `config.ini`'s
  values (the copy it replaced is in the same backup folder).
- It registers the OCR worker for CLANNAD only (`*PCSG00415`), the kernel plugin under `*KERNEL`
  and the Shell plugin under `*main`, and prints each change.
- It prints `package settings: ...` for the test build's own settings (`anki_mining=on`).
- Keep the screen on until it prints `Installed. Reboot the Vita, then leave it on for a minute. …`;
  `Nothing was changed on the Vita` means you left out `--title`.
- In VitaShell, install `ux0:data/JPOverlaySettings.vpk` (the **Overlay** app).

Manual install without the install script (VitaShell's file manager, over USB or FTP): first copy
`ur0:tai/config.txt` to `ur0:tai/config.txt.bak` and your `ux0:data/VitaJPOverlay/config.ini` aside
(the zip's replaces it), copy the zip's `ur0/` and `ux0/` folders onto the Vita's `ur0:` and `ux0:`,
copy the zip's `JPOverlaySettings.vpk` (at its top level) to `ux0:data/` and install it with VitaShell,
delete `ur0:data/VitaJPOverlay/boot.txt` if it exists (the installer resets it, so a safe mode left by
an earlier build ends), then add `ur0:tai/VitaJPOverlay_Kernel.skprx` under `*KERNEL`,
`ur0:tai/VitaJPOverlay_Shell.suprx` under `*main` and, in a new section,
```ini
*PCSG00415
ur0:tai/VitaJPOverlay_OCR.suprx
```
never under `*ALL` (and the same in `ux0:tai/config.txt` if that file exists). The self-test is armed
by an empty `ux0:data/VitaJPOverlay/selftest.txt` (what `--self-test` uploads); copy one there.

## 3. Run it (once)
1. Reboot. Wait for the LiveArea, then leave the Vita alone for 1 minute.
2. Start CLANNAD and go to a dialogue screen. Keep your saved textbox region; with none, draw it
   first: L+R, Square, drag around the text, Cross, Circle. Wait 1 minute without pressing anything:
   the self-test runs its five stages (VOCR3-FL10, VOCR3-H15, full screen, capture probe, VOCR3-H15
   again) on the first settled screen and finishes by itself.
3. Read about 10 pages, pausing about 3 s on each. On 3 of them open the overlay (L+R), take a phone
   photo of the screen, then press Circle. Optional: on one of them press × to save the word as an
   Anki card (it is saved offline on the card). Note any slowdown, stutter or crash.
4. Close CLANNAD, go back to the LiveArea and wait 30 s.

## 4. Send back one zip
Start FTP in VitaShell again, then in `vjo`:
```sh
bash tools/install_ftp.sh --collect VITA_IP
```
It reads everything the overlay wrote (logs, the self-test report, status/kernel/boot records,
`config.ini` with its API keys redacted, both taiHEN configurations, the Anki queue, crash dumps, the
installed files' hashes) into one folder and zip under `build/device/collect/`, writes nothing on the
Vita, and prints the analysis. Send the zip it names (`Send this one file: ...`) and one line per step
saying what you saw ("overlay showed", "no OCR", "crash at step 3"...). Send it privately, not on a
public issue: API keys are removed, but it holds your queued Anki cards' sentences (text of a
copyrighted game), your PC's LAN address (`anki_host`), your taiHEN plugin list, your games' title
IDs and any crash dump (a memory image of the crashed process). Keep the photos private too.

The analysis's self-test verdict is decided by the OCR rows alone; diagnostics marked WARN (the probe,
the memory routes) are measurements to send back, not failures.

## If something goes wrong
- **No overlay or no OCR:** run step 4 anyway; the analysis shows the boot lines (`shell API 19
  entered`, `worker started`) and where taiHEN loads each plugin. `BOOT GUARD: SAFE MODE` means the
  previous boot did not finish: reboot once (one normal retry is allowed), then do step 4.
- **Crash:** step 4 collects the crash dumps too.
- **Won't boot:** hold **L** while powering on (taiHEN skips every plugin), start VitaShell FTP, run
  `bash tools/install_ftp.sh --uninstall VITA_IP` in `vjo`, reboot. If that fails, comment out the
  `VitaJPOverlay` lines in `ur0:tai/config.txt` (and `ux0:tai/config.txt` if it exists) with
  VitaShell, or copy `config.txt.vjo-bak` over `config.txt`.
- **Back to v0.7.12:** download `VitaJPOverlay-0.7.12.zip` from the v0.7.12 release, extract it and run
  `bash install-vita.sh VITA_IP:1337` there, then reboot (that older installer registers the worker
  under `*ALL`, as 0.7.12 always did).
- Run it **once**. Don't keep rebooting to retry; send what you have.

## Models
VOCR3-H15 (`VOCR3_H15_w8.vocr`) is the default and VOCR3-FL10 (`VOCR3_FL10_w8.vocr`) its automatic
fallback when the game has too little free memory; both have the sizes and memory of v0.2.0's H15 and
FL10, which are installed too. To switch, edit `ux0:data/VitaJPOverlay/config.ini` with VitaShell:
`vocr_model = H15_w8.vocr` and `vocr_fallback_model = FL10_w8.vocr` for v0.2.0,
`vocr_model = VOCR3_FL10_w8.vocr` for the smaller model only. It applies when the overlay next opens.

## Anki (optional)
`anki_mining = on` in the test build: × in the overlay saves the shown word and its sentence as a card
in `ux0:data/VitaJPOverlay/anki_queue/`, with Wi-Fi off or Anki closed. △ sends the queue to
AnkiConnect on your PC: in Anki's AnkiConnect settings set `"webBindAddress": "0.0.0.0"`, and in
`config.ini` set `anki_host = PC_IP` (or `auto` to search the local network once). Cards have no
screenshot in this build: its full-frame capture is the kernel buffer #74's console refused.

## What it tells us
- **The capture in the game:** whether the display hook can write the game's own memory, per
  memory type (the `probe` stage), the strip capture's time (`capture_ms`), and that the kernel
  pool gave OCR nothing (`kernel-pool`).
- **Memory:** the game's free USER/CDRAM/phycont before and after each block, each stage's headroom,
  and the model tier (Small/Normal/Large, vita-vn-ocr #74's proposal) the game's free memory allows;
  SceShell's and the kernel pool's free memory by each of three query routes, from boot into the
  game and back.
- **Speed:** VOCR3-H15's and VOCR3-FL10's load and recognition times on a console (est. until
  measured), the warm second recognition, and the time per line and per CTC frame of every job.
