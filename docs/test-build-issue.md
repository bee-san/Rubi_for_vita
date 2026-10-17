# Test build install issue: the template

The skeleton of the GitHub issue that hands a test build (a pre-release, tag `v0.8.0-test.N`) to the
console tester. It is filled per build from [console-test.md](console-test.md), which has the steps in full;
`{…}` marks what each build fills in. Since the one-VPK install the VPK is the install: it carries the
Overlay app and its plugins together, so the app and the plugins cannot be of two builds as in
[#101](https://github.com/bee-san/vita-jp-overlay/issues/101) (test.1's app beside test.2's plugins), and
the app's Install page names the build it installs.

---

**Download:** [`{VPK}`]({VPK_URL}) from the pre-release [{TAG}]({RELEASE_URL}) ({VPK_BYTES} bytes), and for
the advanced path and `--collect` [`{ZIP}`]({ZIP_URL}) ({ZIP_BYTES} bytes)
**SHA-256:** `{VPK_SHA256}` (VPK), `{SHA256}` (zip)

It is `main` at `{MAIN_SHORT}` (API {API}), built by [Release run {RUN}]({RUN_URL}). {WHAT_IS_NEW}

## 1. Check (and extract the zip for its tools)
```sh
sha256sum {VPK} {ZIP}    # macOS: shasum -a 256 ...; must print the SHA-256s above
unzip -q {ZIP} -d vjo && cd vjo
python3 tools/package_release.py --verify . && python3 tools/package_vpk.py --verify ../{VPK}
```

## 2. Install: the one-VPK install (docs/one-vpk-install.md)
Its first console run: rule 7 of AGENTS.md applies (the Install page's plan shows the exact files and
`config.txt` edits and the recovery before anything is written; it is this run's only change).
- [ ] **Settings > HENkaku Settings > Enable Unsafe Homebrew** is on (the Install page says so if not).
- [ ] Copy `{VPK}` (the debug VPK, SHA-256 `{VPK_SHA256}`) to the Vita with VitaShell's FTP or USB, and
  install it in VitaShell: Yes to "Do you want to install this package?" and to the extended-permissions
  notice. Note the time it takes.
- [ ] Open **Overlay** from its LiveArea: it opens on its **Install** page (VitaShell stays suspended behind
  it, as a game would: an ordinary application). A plan longer than its box has a bar at its
  right: R shows the next box of it, L the one before, Up/Down a line; it must end with the restart and the
  two recovery lines, then, when a folder has a backup from before the overlay, a "(… is the configuration
  from before the overlay.)" line for each. The hints row at the bottom must end in `○: exit` (`×: exit` on a
  Japanese console), not `...`. Photograph each box of the plan (it lists the files, the backups' names and
  every `config.txt` change), then choose **Install**. Note the time it takes and what the done screen
  says.
- [ ] Restart the Vita yourself (the app never does). It must start normally.
- [ ] Then, **before opening any game or VitaShell** (no application running), open **Overlay** from its
  LiveArea. It must open, on its **Games** page (the overlay is installed now: the Install page opens first
  only when it is not); its **Install** page (Select three times, or its tab) must say `The overlay is
  installed (v…)`, and **Check and repair** must say that nothing needed writing. `log.txt`'s `overlay app:`
  lines of that open say how it went; send them all (the collect has them), and say whether the app closed
  by itself or stayed on its start-up picture:
  - `start:`, `install page:`, then `graphics: system application; GPU library 0x0A: 0x00000000; display:
    buffer 2 at read N after M ms; …`, then (this debug build) `frames:`: it opened. `… then its own
    parameter buffer 0x03 …` or a large N or M is what the fix handles; `0x0A: 0x00000000` with read 1 after
    0 ms means it opened without it, and the earlier failure is then not explained;
  - no `start:` line for the open: it stopped before the app's first line (the launch), so the fix is not
    involved; `start:` but no `install page:` line: it stopped reading the card; `start:` and `install
    page:` but no `graphics:` line: in the version check or the graphics start;
  - `graphics: waiting for the display …` and `graphics: no display from the system in 10 s …; closing`
    (it showed only its start-up picture, about 10 s), or `graphics: the GPU library refused the app …;
    closing`: it closed itself, and the line says why. `graphics: waiting …` and then nothing: it was closed
    before its 10 s;
  - `graphics: system application; …` but no `frames:` line: most likely it stopped at its first frame.
- [ ] Close **Overlay** (on its LiveArea page, pull the page's top-right corner): an app that is only resumed
  writes no new lines (test.3's was in front six times and wrote one `start:` line), so each open below must
  be a new start. Start a game, L + R; then press PS and open **Overlay** beside the game: it must open the
  same way, with its own `start:` and `graphics:` lines (read 1 is expected there, not required).

  If the open with no game running did not open, close the app the same way if its page is still on the
  LiveArea, then open it beside VitaShell or the game, and say so (docs/vita-memory.md, "Starting with no
  game running", has what each line means).
- [ ] Arm the self-test: `bash tools/install_ftp.sh --self-test VITA_IP` from the zip's folder (the
  advanced path; or skip it). Reboot the Vita.

If the Vita does not start: hold L while turning it on (h-encore/h-encore2: open its menu, holding R as
h-encore starts, and choose Exit while holding L), then in VitaShell put `#` before the two `VitaJPOverlay`
lines of `ur0:tai/config.txt` and `ux0:tai/config.txt`, or open Overlay and choose **Uninstall**. Run it
**once**; don't reboot to retry.

The zip (`{ZIP}`) is the advanced path: `bash install-vita.sh VITA_IP:1337` on a PC, then install its
`JPOverlaySettings.vpk` from `ux0:data/` in VitaShell; `bash install-vita.sh --check-app VITA_IP` must
print "Overlay app: up to date".

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
