# Console test of a main build on a PS Vita PCH-1000, about 15 minutes

Builds after v0.7.12 are not released, so they are tested from main: the plugins come from the build's `Release` CI run, the
models and JMdict from v0.7.12. The READY comment on the console-test issue (#70) names the build (`REV`) and its run (`RUN`).
From that build on, the installer registers the OCR worker only for the games named with `--title` (here CLANNAD, `PCSG00415`),
never under `*ALL`. This guide was first written for main `e759187` (PRs #54-#57: resident weights, memory-check logs, stage
names in memory errors, H15 by default with an FL10 fallback). The READY build has not run on a console yet; "est." marks an estimate.

## 1. Build the package (a PC on the Vita's network with git, bash, Python 3.9+, curl, unzip and a logged-in `gh`)
```sh
REV=...; RUN=...   # the build hash and its Release run id, from the READY comment
git clone https://github.com/bee-san/vita-jp-overlay && cd vita-jp-overlay && git checkout $REV
gh run download $RUN -R bee-san/vita-jp-overlay -n VitaJPOverlay -D build/ci                       # the build's plugins
gh release download v0.7.12 -R bee-san/vita-jp-overlay -p VitaJPOverlay-0.7.12.zip -D build/rel   # models, JMdict
R=build/rel/x; D=$R/ux0/data/VitaJPOverlay; unzip -qo build/rel/*.zip -d $R && cp $R/docs/jmdict-conversion.json $D/dictionaries/jmdict.json
python3 tools/package_release.py build/ci/*.zip -o build/test.zip --models $D/ocr --dictionary $D/dictionaries/jmdict.vjdict \
  --license-file $R/docs/JMdict-LICENSE.html --source-revision "$(git rev-parse HEAD)"
unzip -qo build/test.zip -d build/pkg && cd build/pkg && python3 tools/package_release.py --verify .   # Verified 53 package files
```
- If a command fails, fix what it names (often `gh auth login`) and rerun it; `checksum failed` means a bad download: delete, rerun.
- The installer copies the weights (bee-san/vita-vn-ocr v0.2.0) to `ux0:data/VitaJPOverlay/ocr/`; the plugin refuses other bytes:
  `H15_w8.vocr` (default) 1,155,968 B, sha256 `316ebc377ec34520440991c5085199d8e74031f01a867938be90a60c1e3f03cd`;
  `FL10_w8.vocr` (fallback) 657,664 B, sha256 `500e0b842e5f0cb326f55e7d991a52ab66dd24977ccc89bd188228aedc825e84`.

## 2. Install and boot (firmware 3.60-3.74 with HENkaku/Ensō or h-encore, and VitaShell)
1. Close the game, open VitaShell and press Select (FTP), then in `build/pkg` run
   `bash install-vita.sh --title PCSG00415 VITA_IP:1337` (it registers the OCR worker for CLANNAD only, `*PCSG00415`, never
   `*ALL`; an existing `*ALL` worker line is commented out). Keep the
   screen on (it uploads the 77 MB JMdict, reading every file back) until `Installed. Reboot the Vita.` `Cannot reach` = FTP off;
   `Nothing was changed on the Vita` = `--title` is missing; else rerun.
2. It sets H15 + FL10 fallback, `ocr_mode = auto`, `log_file = on`, `local_dictionaries = jmdict.vjdict` (drops Daijirin); old config in `backup:`.
3. Reboot normally, start FTP, run `bash tools/install_ftp.sh --status VITA_IP`: `status.txt` needs `shell API 18 entered` and
   `worker started`; if not (e.g. `kernel API version N, expected 18` or `kernel plugin not loaded`), install again and reboot.
4. Boot hangs or crashes: hold L while powering on (skips plugins), start FTP, `bash tools/install_ftp.sh --uninstall VITA_IP`,
   reboot, run `--status` and keep its four files (the new kernel imports `ksceIoGetstatByFd`: suspect that first). Back to v0.7.12
   at any time: `cd ../rel/x && bash install-vita.sh VITA_IP:1337`, then reboot (v0.7.12's installer predates `--title` and
   registers its worker under `*ALL` again).

## 3. Test in CLANNAD (PCSG00415, where FL10 OCR worked before)
1. Start CLANNAD (it must be ON in JP Overlay Settings → Games). Keep its saved region; if none: L+R, Square, drag round the text, Cross, Circle.
2. Advance about 10 text pages, pausing ~3 s on each so automatic OCR runs. On 3 of them open the overlay (L+R) and photograph the
   screen with its recognized text (keep photos private: the game is copyrighted), then Circle. Note slowdown, stutter or crashes.
3. Optional, 2 min: L+R, hold Square 1 s (full-screen region), Circle, read 2 pages (H15 should still fit), restore the tight region
   (L+R, Square, drag, Cross, Circle); press PS, wait 5 s, resume, read 1 page (its job usually logs `weights=read` again).

## 4. Copy back (close the game, start FTP; in `build/pkg`); give the fleet these files and the photos in one folder
```sh
curl -O ftp://VITA_IP:1337/ux0:/data/VitaJPOverlay/log.txt      # job log; also log.old.txt if it exists
bash tools/install_ftp.sh --status VITA_IP   # from that Vita folder: status.txt, kernel.txt (+ .prev) -> build/device/status/
bash tools/install_ftp.sh --dumps VITA_IP    # after a crash only: ux0:data/*.psp2dmp -> build/device/dumps/
```
- Per text job (in `log.txt` and `status.txt`): `game vocr rc=0 H15_w8.vocr: WxH, lines=N USER workspace=B weights=read|resident
  resident=1159168, find=… ms load=… ms recognize=… ms frames=F io=0x00000000 stat=0x00000000 cleanup=0`: load/recognition ms, CTC frames;
- `game vocr memory: free USER=U, needs workspace J + weights W + reserve 4194304: check passed`: the memory check;
- `game vocr speed: F frames in T us, P us per frame` and `game vocr lines: WxH Nf X.Yms …`, one entry per text line.
- **Success:** every job `rc=0 … cleanup=0` (`rc=-118` after `job cancelled` = fast page turn, fine) and names `H15_w8.vocr` (no
  `(fallback)`, no `unusable`); the first text job `weights=read` (load est. 0.23-0.26 s), later ones `weights=resident` with `load=`
  a few ms (est.; FL10 took 213-359 ms per job before); `speed` est. 2,700-5,000 us per frame at 333 MHz; text roughly matches; `lines=0` ok.

## 5. If a job fails
- "The game OCR worker is unavailable" on screen: the game has no worker section; install again with `--title PCSG00415` and reboot.
- `stat=0x8…` and `weights=read` on every job: the file-time query is unsupported; note the code (loads still skip the old sleeps).
- `(fallback)` in CLANNAD: keep the `game vocr fallback:` line (short of memory). `H15_w8.vocr is unusable here … for this game`: H15
  on the card is missing or damaged, install again; `… for this job`: that job used FL10 (can recur), note `io=` (`0x80010013`: storage unavailable).
- "not enough memory" on screen: photograph it; it names the stage (kernel capture, the game's memory for the region or the weights,
  the Shell); `game vocr memory:` has its numbers (none for the kernel capture). `PCSG00415: JP Overlay disabled` in the log: CLANNAD is OFF.
- H15 too slow or worse than FL10: `curl -o vita-config.ini ftp://VITA_IP:1337/ux0:/data/VitaJPOverlay/config.ini`, edit that copy to
  `vocr_model = FL10_w8.vocr`, `curl -T vita-config.ini ftp://VITA_IP:1337/ux0:/data/VitaJPOverlay/config.ini`; applies at the next L+R.
