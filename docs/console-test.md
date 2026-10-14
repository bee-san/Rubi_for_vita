# Console test of a main build on a PS Vita, one run, about 20 minutes

Builds after v0.7.12 are not released, so they are tested from main: the plugins come from the
build's `Release` CI run, the models from v0.7.12 and the frozen dictionary from the build itself.
The console-test comment (#74 for API 19) names the build (`REV`) and its run (`RUN`). The
installer registers the OCR worker only for the games named with `--title` (here CLANNAD,
`PCSG00415`), never under `*ALL`. API 19 reads the screen inside the game: the kernel gives OCR no
capture buffer ([ingame-capture.md](ingame-capture.md)). Nothing of it has run on a console yet;
"est." marks an estimate.

## What you need
- A PCH-1000/1001 on 3.60–3.74 with HENkaku/Ensō or h-encore, and VitaShell.
- CLANNAD (PCSG00415), ON in the Overlay app → Games.
- A PC on the same Wi-Fi with git, bash, Python 3.9+, curl, unzip and a logged-in `gh`.

## 1. Build the package on your PC
```sh
REV=...; RUN=...   # the build hash and its Release run id, from the console-test comment
git clone https://github.com/bee-san/vita-jp-overlay && cd vita-jp-overlay && git checkout $REV
gh run download $RUN -R bee-san/vita-jp-overlay -n VitaJPOverlay -D build/ci
gh release download v0.7.12 -R bee-san/vita-jp-overlay -p VitaJPOverlay-0.7.12.zip -D build/rel
R=build/rel/x; D=$R/ux0/data/VitaJPOverlay; unzip -qo build/rel/*.zip -d $R
python3 tools/package_release.py build/ci/*.zip -o build/test.zip --models $D/ocr --source-revision "$(git rev-parse HEAD)"
unzip -qo build/test.zip -d build/pkg && cd build/pkg && python3 tools/package_release.py --verify .
```
If a command fails, fix what it names (usually `gh auth login`) and run it again. `checksum failed`
means a bad download: delete the file and rerun. The weights the plugin accepts (vita-vn-ocr
v0.2.0): `H15_w8.vocr` 1,155,968 B, sha256 `316ebc37…3f03cd`; `FL10_w8.vocr` 657,664 B, sha256
`500e0b84…825e84`.

## 2. Install
On the Vita, close any game, open VitaShell and press **Select** to start FTP. Then, in `build/pkg`:
```sh
bash install-vita.sh --title PCSG00415 VITA_IP:1337
bash tools/install_ftp.sh --self-test VITA_IP
```
- The installer backs up `ur0:tai/config.txt` first, on your PC and as `config.txt.vjo-bak` on
  the Vita, and registers the OCR worker for CLANNAD only.
- Keep the screen on until it prints `Installed. Reboot the Vita.`
- `Nothing was changed on the Vita` means you left out `--title`.

## 3. Run it
1. Reboot. Wait for the LiveArea, then leave the Vita alone for 1 minute.
2. Start CLANNAD and get to a dialogue screen. The self-test uses **CLANNAD's saved region**
   (your textbox; with none saved, the full screen). Wait 30 s without pressing anything: on the
   first settled screen it runs its five stages (FL10, H15, full screen, capture probe, H15
   again), each with its own record, then finishes by itself.
3. Read about 10 pages, pausing about 3 s on each. On 3 of them, open the overlay (L+R), take a
   phone photo of the screen, then press Circle. Note any slowdown, stutter or crash.
4. Close CLANNAD, go back to the LiveArea and wait 30 s.

## 4. Send back the results
Start FTP again, then in `build/pkg`:
```sh
bash tools/install_ftp.sh --status VITA_IP
python3 tools/analyse_selftest.py build/device/status/selftest-report.txt
curl -O ftp://VITA_IP:1337/ux0:/data/VitaJPOverlay/log.txt
```
Attach `selftest-report.txt`, `status.txt`, `kernel.txt` and `boot.txt` (from
`build/device/status/`), plus `log.txt` and the analyser's output, and one line per step saying
what you saw ("overlay showed", "no OCR", "crash at step 3"...). Keep the photos private (the game
is copyrighted): send them directly, not on the issue.

## If something goes wrong
- **No overlay or no OCR:** look at the top of `status.txt`. It needs `shell API 19 entered` and
  `worker started`. `BOOT GUARD: SAFE MODE` means the previous boot didn't finish: reboot once
  (one normal retry is allowed), then do step 4 anyway.
- **Crash:** also run `bash tools/install_ftp.sh --dumps VITA_IP` and attach `build/device/dumps/`.
- **Won't boot:** hold **L** while powering on (taiHEN skips every plugin), start VitaShell FTP,
  run `bash tools/install_ftp.sh --uninstall VITA_IP`, reboot. If that fails, comment out the
  `VitaJPOverlay` lines in `ur0:tai/config.txt` with VitaShell.
- **Back to v0.7.12:** `cd ../rel/x && bash install-vita.sh VITA_IP:1337`, then reboot.
- Run it **once**. Don't keep rebooting to retry; post what you have.

## What it tells us
- **The capture in the game:** whether the display hook can write the game's own memory, per
  memory type (the `probe` stage), the strip capture's time (`capture_ms`), and that the kernel
  pool gave OCR nothing (`kernel-pool`).
- **Memory:** the game's free USER/CDRAM/phycont before and after each block (`gstep` lines),
  SceShell's and the kernel pool's free memory by each of three query routes (`kmem` lines), from
  boot into the game and back.
- **Speed:** H15's and FL10's load and recognition times on a console (est. until measured), and
  the warm second recognition.
