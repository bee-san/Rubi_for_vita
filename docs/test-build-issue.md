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
- [ ] Open **Overlay**: it opens on its **Install** page. A plan longer than its box has a bar at its
  right: R shows the next box of it, L the one before, Up/Down a line; it must end with the restart and the
  two recovery lines, then, when a folder has a backup from before the overlay, a "(… is the configuration
  from before the overlay.)" line for each. The hints row at the bottom must end in `○: exit` (`×: exit` on a
  Japanese console), not `...`. Photograph each box of the plan (it lists the files, the backups' names and
  every `config.txt` change), then choose **Install**. Note the time it takes and what the done screen
  says.
- [ ] Restart the Vita yourself (the app never does). It must start normally; then a game, L + R.
- [ ] Open **Overlay** again: its title must say `The overlay is installed (v…)`, and **Check and repair**
  must say that nothing needed writing.
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
