# Console test of main `e759187` (PRs #54-#57) on a PS Vita PCH-1000, about 15 minutes

PRs #54-#57 keep verified weights resident, log memory checks and CTC frames, name the stage in memory errors and make H15 the
default with an FL10 fallback. **No release has them yet** (v0.7.12 is older), so test main `e759187`: its plugins come from CI
run 38326458989, the models and JMdict from v0.7.12. Nothing here has run on a console yet; "est." marks an estimate.

## 1. Build the package (a PC on the Vita's network with bash, Python 3.9+, curl, unzip and a logged-in `gh`)
```sh
git clone https://github.com/bee-san/vita-jp-overlay && cd vita-jp-overlay && git checkout e759187
gh run download 38326458989 -R bee-san/vita-jp-overlay -n VitaJPOverlay -D build/ci                 # e759187's plugins
gh release download v0.7.12 -R bee-san/vita-jp-overlay -p VitaJPOverlay-0.7.12.zip -D build/rel   # models, JMdict
R=build/rel/x; D=$R/ux0/data/VitaJPOverlay; unzip -qo build/rel/*.zip -d $R && cp $R/docs/jmdict-conversion.json $D/dictionaries/jmdict.json
python3 tools/package_release.py build/ci/*.zip -o build/test.zip --models $D/ocr --dictionary $D/dictionaries/jmdict.vjdict \
  --license-file $R/docs/JMdict-LICENSE.html --source-revision e759187637e9155f61b4404d673fab6199fe9863
unzip -qo build/test.zip -d build/pkg && cd build/pkg && python3 tools/package_release.py --verify .   # Verified 52 package files
```
- If a command fails, fix what it names (often `gh auth login`) and rerun it; `checksum failed` means a bad download: delete, rerun.
- The installer copies the weights (bee-san/vita-vn-ocr v0.2.0) to `ux0:data/VitaJPOverlay/ocr/`; the plugin refuses other bytes:
  `H15_w8.vocr` (default) 1,155,968 B, sha256 `316ebc377ec34520440991c5085199d8e74031f01a867938be90a60c1e3f03cd`;
  `FL10_w8.vocr` (fallback) 657,664 B, sha256 `500e0b842e5f0cb326f55e7d991a52ab66dd24977ccc89bd188228aedc825e84`.

## 2. Install and boot (firmware 3.60-3.74 with HENkaku/Ensō or h-encore, and VitaShell)
1. Close the game, open VitaShell and press Select (FTP), then in `build/pkg` run `bash install-vita.sh VITA_IP:1337`. Keep the
   screen on (it uploads the 77 MB JMdict, reading every file back) until `Installed. Reboot the Vita.` `Cannot reach` = FTP off; else rerun.
2. It sets `vocr_model = H15_w8.vocr`, `vocr_fallback_model = FL10_w8.vocr`, `ocr_mode = auto`, `log_file = on` and
   `local_dictionaries = jmdict.vjdict` (drops a Daijirin entry); your old `config.ini` is in the `backup:` folder it prints.
3. Reboot normally, start FTP, run `bash tools/install_ftp.sh --status VITA_IP`: `status.txt` needs `shell API 16 entered`, `worker started`.
4. Boot hangs or crashes: hold L while powering on (skips plugins), start FTP, `bash tools/install_ftp.sh --uninstall VITA_IP`,
   reboot, run `--status` and keep its four files. The new kernel imports `ksceIoGetstatByFd`: suspect that first.
   `kernel API version N, expected 16`: an old plugin still runs; install again. Back to v0.7.12: `cd ../rel/x && bash install-vita.sh VITA_IP:1337`.

## 3. Test in CLANNAD (PCSG00415, where FL10 OCR worked before)
1. Start CLANNAD (it must be ON in JP Overlay Settings → Games). Keep its saved region; if none: L+R, Square, drag round the text, Cross.
2. Advance about 10 text pages, pausing ~3 s on each so automatic OCR runs. On 3 of them open the overlay (L+R) and
   photograph the screen with its recognized text (keep photos private: the game is copyrighted).
3. Optional, 2 min: L+R, hold Square 1 s (full-screen region), read 2 pages (H15 should still fit), draw the tight region
   again; press PS, wait 5 s, resume, read 1 page (its job usually logs `weights=read` again). Note slowdown, stutter or crashes.

## 4. Copy back (close the game, start FTP; in `build/pkg`); give the fleet these files and the photos in one folder
```sh
curl -O ftp://VITA_IP:1337/ux0:/data/VitaJPOverlay/log.txt      # job log; also log.old.txt if it exists
bash tools/install_ftp.sh --status VITA_IP   # same folder: status.txt, kernel.txt (+ .prev) -> build/device/status/
bash tools/install_ftp.sh --dumps VITA_IP    # after a crash only: ux0:data/*.psp2dmp -> build/device/dumps/
```
- Per text job (in `log.txt` and `status.txt`): `game vocr rc=0 H15_w8.vocr: WxH, lines=N USER workspace=B weights=read|resident
  resident=1159168, find=… ms load=… ms recognize=… ms frames=F io=0x00000000 stat=0x00000000 cleanup=0`: load/recognition ms, CTC frames;
- `game vocr memory: free USER=U, needs workspace J + weights W + reserve 4194304: check passed`: the memory check;
- `game vocr speed: F frames in T us, P us per frame` and `game vocr lines: WxH Nf X.Yms …`, one entry per text line.
- **Success:** every job `rc=0 … cleanup=0`; the first text job `weights=read`, later ones `weights=resident` with `load=` a few
  ms (est.; FL10 took 213-359 ms on every job before; H15's first load est. 0.23-0.26 s); `speed` est. 2,700-5,000 us per frame
  at the default 333 MHz; no `(fallback)`; the text roughly matches the screen. `lines=0` jobs (no text found) are normal.

## 5. If a job fails
- `stat=0x8…` and `weights=read` on every job: the file-time query is unsupported; note the code (loads still skip the old sleeps).
- `(fallback)` in CLANNAD: keep the `game vocr fallback:` line (the title is short of memory). `H15_w8.vocr is unusable here`:
  H15 on the card is missing or damaged and FL10 took over; install again. `io=0x80010013`: the game cannot reach the model.
- "not enough memory" on screen: photograph it; it names the stage (kernel capture, the game's memory for the region or the
  weights, the Shell), and the `game vocr memory:` line has the numbers. `PCSG00415: JP Overlay disabled`: turn CLANNAD ON.
- H15 too slow or worse than FL10: `curl -O ftp://VITA_IP:1337/ux0:/data/VitaJPOverlay/config.ini`, set `vocr_model = FL10_w8.vocr`,
  then `curl -T config.ini ftp://VITA_IP:1337/ux0:/data/VitaJPOverlay/config.ini`; it applies at the next L+R.
