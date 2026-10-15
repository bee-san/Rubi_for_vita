# Test build install issue: the template

The skeleton of the GitHub issue that hands a test build (a pre-release, tag `v0.8.0-test.N`) to the
console tester. It is filled per build from [console-test.md](console-test.md), which has the steps in full;
`{…}` marks what each build fills in. Keep the first item of section 2 as it is: in
[#101](https://github.com/bee-san/vita-jp-overlay/issues/101) the Overlay app was not reinstalled, so
test.1's app ran beside test.2's plugins, showed VOCR2, had no Anki page and wrote `ocr_backend = vocr2`,
and two of the six requests could not be judged.

---

**Download:** [`{ZIP}`]({ZIP_URL}) from the pre-release [{TAG}]({RELEASE_URL}) ({ZIP_BYTES} bytes)
**SHA-256:** `{SHA256}`

It is `main` at `{MAIN_SHORT}` (API {API}), built by [Release run {RUN}]({RUN_URL}). {WHAT_IS_NEW}

## 1. Check and extract
```sh
sha256sum {ZIP}          # macOS: shasum -a 256 {ZIP}; must print the SHA-256 above
unzip -q {ZIP} -d vjo && cd vjo
python3 tools/package_release.py --verify .
```

## 2. Install
On the Vita, close any game, open VitaShell and press **Select** to start FTP. On the PC, in `vjo`:
```sh
bash install-vita.sh --title PCSG00415 VITA_IP:1337
```
- [ ] **Step 1, every time: install the Overlay app again.** The installer has just put this build's
  `JPOverlaySettings.vpk` in `ux0:data/` and ends with a box that says whether the Vita's app is this
  build's. In VitaShell (press OK on its FTP dialog to keep FTP running) open `ux0:data/` and install
  `JPOverlaySettings.vpk`. Then, on the PC:
  ```sh
  bash install-vita.sh --check-app VITA_IP     # must print "Overlay app: up to date"
  ```
  If an overlay in a game says `Overlay app is out of date: …`, this step was missed: do it, then restart
  the game.
- [ ] Arm the self-test: `bash tools/install_ftp.sh --self-test VITA_IP`.
- [ ] Reboot the Vita.

{INSTALL_NOTES}

## 3. The run
{RUN_STEPS}

## 4. Send back one zip
Start VitaShell's FTP again, then in `vjo`: `bash tools/install_ftp.sh --collect VITA_IP`. Its analysis has
a line saying whether the Overlay app was this build's (`Overlay app: the plugins' (…)`), after the
package and installed-files lines. Send the zip
privately ([console-test.md](console-test.md#4-send-back-one-zip) says what it holds).

## If something goes wrong
See [console-test.md](console-test.md#if-something-goes-wrong). Run it **once**; don't reboot to retry.
