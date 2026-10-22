# One VPK is the whole install

The owner (PR #134, 2026-10-16 06:59Z): "i would hope the easiest install would be to go to GitHub
releases, download the latest vpk, and install it and it's done. Let's queue an agent to make this
our release process".

This is the design, written and reviewed before the code. The design audit
(audit-overlay-one-vpk-install-design, `reports/audit-overlay-one-vpk-install-design.md` in the
fleet's reports) asked for changes R1-R7; they are folded in, and the last section lists them.

## The user's steps

1. Download `Rubi-<version>.vpk` from the latest GitHub release.
2. Copy it to the Vita with VitaShell's USB or FTP connection (Select starts the one chosen under
   **SELECT button** in VitaShell's settings, Start; a Vita's default is USB), then install it in VitaShell
   (Enter on the file; answer Yes to VitaShell's "This package requests extended permissions").
3. Open **Rubi** from LiveArea. It opens on its **Install** page, which lists exactly what it will
   do; choose **Install**, then restart the Vita.

Nothing is downloaded on the Vita: the VPK holds the plugins, the models and the dictionary. A game
is then played as today (L + R). An update is the same three steps with the new VPK: the app sees
that its overlay is not the installed one, and its Install page offers **Update**.

### Requirements

- A PS Vita on 3.60-3.74 with HENkaku/Ensō or h-encore, and VitaShell, as today.
- **Settings > HENkaku Settings > Enable Unsafe Homebrew** on, which the PC installer did not need.
  HENkaku's default is off (henkaku 784e483 `plugin/kernel.c:156`), and only then does an unsafe SELF
  such as the app's `eboot.bin` (authid `0x2F00000000000001`) get extended permissions
  (`plugin/kernel.c:60-73`); without them the app runs sandboxed and cannot write `ur0:tai`. The app
  detects it as VitaShell does (`sceIoDevctl("ux0:", 0x3001, NULL, 0, NULL, 0)` returns `0x80010030`,
  VitaShell 81af709 `init.c:353-355`) and says how to turn it on.
- About 245 MB free on ux0, the VPK counted: VitaShell's step needs the 61.1 MB VPK plus its 169.9 MB
  unpacked, and the app's Install 12.2 MB more while the VPK is still on the card (the models it
  copies and its 0.5 MiB margin, `settings/install.c`'s room check, which otherwise stops before
  writing and says the `.vpk` can go) [estimate from the measured sizes: #158's debug packages, #153's
  with Bee's Ultimate Dictionary, whose file is 13.9 MB larger, and #167's with VOCR5's three models and
  their `MODEL-ATTRIBUTION.md`, 1.1 MB more than the three before them: 243.2 MB in all].
  VitaShell's own check lets an install start with up to 40 MiB less (below), so the README asks for
  the room itself. After the install, and once the VPK is deleted,
  the overlay takes about 181.6 MB: 169.2 MB of data, the 0.5 MB app and the 11.9 MB of plugins and
  models the app keeps for Repair [estimate].

`install-vita.sh` (the release zip) stays as the advanced and developer path.

## What a release ships

| asset | build | for |
|---|---|---|
| `Rubi-<version>.vpk` | normal | everyone: the main download |
| `Rubi-<version>-debug.vpk` | debug (the test builds') | console tests, a full log |
| `Rubi-<version>.zip`, `Rubi-<version>-debug.zip` | normal, debug | `install-vita.sh`, a manual install, the tools |
| `SHA256SUMS` | | all four, the normal VPK first |

The two builds are #141's (`docs/release-variants.md`): the same models, dictionary and config
template; the debug build has the full log and the self-test. Both VPKs are the same app (title ID
`VJOSET001`), so either installs over the other, and each carries its own build's plugins. A release
is about 4 × 61 MB of assets.

The zips keep today's layout and their own `Rubi.vpk`, which carries no overlay files
(the "no payload" state below), so a zip is not 61 MB larger.

## Inside the VPK

The Rubi app as today (`eboot.bin`, `sce_sys/`, the #122 stamp `vjo_app.txt`), plus `payload/`,
which mirrors the zip's device folders [sizes measured: #141's packages, e5e2a26]:

| in the VPK | installed to | bytes |
|---|---|---:|
| `payload/ur0/tai/VitaJPOverlay_Kernel.skprx` | `ur0:tai/` | 31,316 (debug 34,556) |
| `payload/ur0/tai/VitaJPOverlay_Shell.suprx` | `ur0:tai/` | 75,904 (debug 83,908) |
| `payload/ur0/tai/VitaJPOverlay_OCR.suprx` | `ur0:tai/` | 63,552 (debug 64,764) |
| `payload/ur0/data/VitaJPOverlay/vitajpoverlay.rco` | `ur0:data/VitaJPOverlay/` | 6,000 |
| `payload/ux0/data/VitaJPOverlay/ocr/` (3 recognizers, 4 LMs, `LM-ATTRIBUTION.md`, `MODEL-ATTRIBUTION.md`, the text-presence gate's `textgate_p8_c8_w8.vtd` and `GATE-ATTRIBUTION.md`, scan screen's text spotter weights `textspot3_t64n8c16_kd_w8.vtsp` and `TEXTSPOT-ATTRIBUTION.md`) | `ux0:data/VitaJPOverlay/ocr/` | 11,784,086 with API 30's text spotter's weights and attribution (+53,960: vita-vn-ocr #108's 51,008 + 2,952; 11,739,203 with #104's, 5,732 + 3,345; 11,730,126 with API 26's gate, +5,246; VOCR5's 11,724,880; 10,614,250 before) |
| `payload/ux0/data/VitaJPOverlay/dictionaries/plaintext.vjdict` | moved to `ux0:data/VitaJPOverlay/dictionaries/` | 157,298,688 (Bee's Ultimate Dictionary, #153; 143,425,536 before) |
| `payload/ux0/data/VitaJPOverlay/config.ini` | written only when there is none | 3,552 |
| `payload/manifest.txt` | read by the app | small |
| `licenses/` (`LICENSE`, the GPL; `docs/plaintext-attribution.txt`; `docs/JMdict-LICENSE.html`; `third_party/README.md` and `third_party/licenses/`), at their zip paths | stays in the app, `ux0:app/VJOSET001/licenses/` | 79,336 (this PR's 0.8.0 packages) |

The dictionary is 93% of the 169,223,170 B the payload installs (the debug package with VOCR5's models, on main
`eece6c6`; 168,112,360 B there before them); the app itself is 0.5 MB.

`payload/manifest.txt` is written by `tools/package_vpk.py`, one item per line:

```
vjopayload 1
version 0.8.0
variant normal
api 24
worker kernel
file ur0:tai/VitaJPOverlay_Kernel.skprx 31316 <sha256> copy
...
file ux0:data/VitaJPOverlay/dictionaries/plaintext.vjdict 157298688 <sha256> move
template ux0:data/VitaJPOverlay/config.ini 3552 <sha256>
set anki_mining on
```

The app accepts a manifest only when it is exactly its own build's (R4); anything else is
"cannot: the package is damaged":

- its file set equals a list compiled into the app (`include/vjo_payload.h`): the three plugins, the
  RCO, the files of `ocr/` (the models and LMs, by the kernel broker's names,
  `tools/docs_model_names.py`; the text-presence gate's weights; their attributions), the
  dictionary, each with its kind (`copy`, or `move` for the dictionary only), and `template` for
  `config.ini` only; a test checks the list against `package_release.py`'s;
- no destination twice; sizes parse as 64-bit decimals without overflow; hashes are 64 hex digits;
- `api`, `app_version`, `variant` and `worker` equal the app's compiled values (`VJO_API_VERSION`,
  `VJO_VERSION`, `VJO_DEBUG`, `VJO_KERNEL_LOADS_WORKER`), and `build` is the app's own eboot stamp;
  `version` is the package's (a test build's `0.8.0-test.4` beside the app's `0.8.0`), shown in titles and
  recorded in `installed.txt`;
- `set` keys come from a compiled list (`anki_mining`, `log_file`), at most 4, each value one of the
  key's own (`on`, `off`);
- at most 64 lines of at most 255 bytes, ASCII only.

This is robustness, not a security boundary: whoever can edit the VPK can edit the eboot, which has
extended permissions anyway.

## What VitaShell does with it

From VitaShell 81af709 (`package_installer.c` `install_thread`):

- It scans the archive's SELFs for unsafe ones and returns at the first (`archive.c:416-500`): an
  import of SceVshBridge's `_vshIoMount`/`vshIoUmount` (NIDs `0x3C522C35`, `0x35BC26AC`,
  `elf.c:126-134`) gives the "can potentially brick your device" warning; an import of ScePromoterUtil
  or SceShellSvc, or an authid other than `0x2F00000000000002`, gives "This package requests extended
  permissions" (`archive.c:487-498`, `resources/english_us.txt:142-143`). The app's `eboot.bin` is the
  VPK's second entry, after `sce_sys/param.sfo`, with authid `0x2F00000000000001`, so the scan stops
  there with the extended-permissions warning, as today's VPK does, and never reads the payload.
  The packaging asserts that order, and scans every SELF of the VPK itself for the two mount NIDs and
  the two libraries, which is stricter than VitaShell (our plugins import neither mount call:
  `shell/boot.c` uses `_vshKernelSearchModuleByName` only).
- `checkMemoryCardFreeSpace` (`utils.c:91-125`) refuses only when the unpacked size is at least ux0's
  free space plus 40 MiB, so it lets an install start with up to 40 MiB less room than it needs; the
  extraction then fails on a full card and VitaShell deletes `ux0:data/pkg`
  (`package_installer.c:538-539`). Hence the README's 245 MB.
- It extracts everything into `ux0:data/pkg`, writes `sce_sys/package/head.bin`, and calls
  `scePromoterUtilityPromotePkgWithRif`, which has SceShell move the files into `ux0:app/VJOSET001`
  and add the bubble (henkaku wiki, ScePromoterUtil and Packages). The `.vpk` can be deleted after.
- Nothing in it limits an app's size or its number of files (v0.8.0's release zip has 72 entries; a
  later release's differs). The promoter's own limits are documented nowhere we could read.
- The README says to close the Rubi app before installing a VPK over it. In test.4's console run
  VitaShell's install stopped with `0x80101114` while test.3's app was suspended behind it, and the
  same VPK installed after a restart (#194); what the promoter does to a running system application is
  otherwise unmeasured.

## The dictionary is moved, not copied

Copying the dictionary out of the app folder would need another 150.0 MiB (its 157,298,688 B, above)
and that much writing.
Instead the app moves it (`sceIoRename` within ux0: the same device, no data written), and says so
on its Install page.

The app folder cannot be written while it is the app's `app0:`: VitaShell unmounts its own
(`sceAppMgrUmount("app0:")`, "Allow writing to ux0:app/VITASHELL", `init.c:349-351`). So the install,
after its OK and before its first move or delete in the app folder, unmounts `app0:` the same way, and
from then on reads the payload and the app's stamp through `ux0:app/VJOSET001/` only
(`settings/app_check.c:77-80` already falls back to that path for the stamp). Nothing else of the app
reads `app0:` while it runs. Whether a system application may unmount its `app0:` is unmeasured. If
the unmount, or the rename after it, fails, the app copies the dictionary instead when ux0 has room;
once the copy is checked and in place, the app folder's copy is deleted when the folder can be written
(the probe moved a file out of it), and otherwise the app says the folder keeps its copy too (300.0 MiB
in all). Without room it stops before anything is written.

- After the move the app folder no longer has the dictionary. A **Repair** of a missing or damaged
  dictionary therefore needs the VPK installed again first; the app says so when it is the case.
- An update brings the dictionary again (VitaShell installs the whole VPK), and so does the same VPK
  installed again. When the installed dictionary already has the manifest's SHA-256 (the dictionary is
  frozen, so usually it does), the app deletes the app folder's copy and frees 150.0 MiB; only the
  installed one is hashed. Otherwise it replaces it: the old file to `plaintext.vjdict.vjo-old`, the new
  one moved in and hashed, the old one deleted (renamed back when the hash fails).
- Whichever way the installed dictionary got its SHA-256 (moved, copied, an earlier run's
  `.vjo-new`, or already there), the app folder's copy goes in the same run, and the done screen and
  `log.txt` say what became of it: `install: this app's copy of the dictionary deleted (N B freed)`, or
  `kept (N B)` with the reason. A **Check and repair** that writes no file of the overlay but deletes
  that copy says both (#194: test.4's owner had the overlay's files from FTP beside the VPK's payload,
  157 MB twice, and its done screen said only "nothing needed writing"). `log.txt` also says whether the
  dictionary was moved or copied out of the app.
- Plugins, the RCO and the models (11.9 MB) are copied: the app keeps them for a Repair without the
  VPK.
- An 0.7.12 install's `dictionaries/jmdict.vjdict` is no longer used (`local_dictionaries` names
  `plaintext.vjdict` in its place); the app deletes nothing of the user's, so it stays until deleted in
  VitaShell (the guide's [Settings and dictionaries](guide.md#settings-and-dictionaries)).

The Vita's `sceIoRename` does not replace an existing file: it returns `SCE_ERROR_ERRNO_EEXIST`
(VitaShell `file.c:518-551` deletes the destination and renames again). Every replacement here goes
through `.vjo-old`, so a file is missing between two renames for one call at most.

## The app: an Install page

A fourth tab: **Games | OCR & lookup | Anki | Install**. At start the app reads its
`payload/manifest.txt` and works out a state, evaluated in this order (nothing is written):

| state | when | start page |
|---|---|---|
| interrupted | a `config.txt.vjo-new` or `config.txt.vjo-old` left by a stopped swap (step 5), beside a missing `config.txt`, or beside one that came back (h-encore and h-encore2 write their stock text into both configurations when both are missing, h-encore-2 c59a248 `bootstrap/bootstrap.c:372-379,574-581`) or never left (a stop while `.vjo-new` was written) | Install, offering to put a whole file back and to keep the current one (step 5) |
| cannot | Unsafe Homebrew is off; or the package is damaged (R4); or a configuration the editor refuses (below) | Install, with the reason and what to do |
| no payload | the zip's `Rubi.vpk`, or a source build: the app carries no overlay | Games; Install offers Uninstall (when a configuration has the overlay's lines) and says `install-vita.sh` installs this build; no Check: without a manifest there is nothing to check the files against |
| not installed | no configuration loads the overlay's kernel and Shell plugins | Install |
| update | `installed.txt` (below) names another build, or there is none | Install |
| repair | this build (`installed.txt`), but a plugin file is not the payload's (size and SHA-256), the plugins' record (`ux0:data/VitaJPOverlay/vjo_app.txt`, #122) is missing or another app build's, a data file is missing or has the wrong size, or a configuration does not load both plugins (config.ini is not part of it: over this build it is the user's, below) | Install |
| installed | every plugin file is the payload's, every data file has its size, every configuration loads both plugins, and both records are this build's | Games |

At start only the four small files (the three plugins and the RCO, about 180 KB) are hashed; the
models and the dictionary are checked by size. Install and **Check and repair** hash everything.

**installed.txt (R7).** The #122 stamp (`vjoapp1 api=N build=<eboot hash>`) has no version or build
variant, so while the API stays 22 it cannot tell an older VPK from a newer one, or debug from normal.
Install writes `ux0:data/VitaJPOverlay/installed.txt`, one line,
`vjoinst1 version=0.8.0 variant=normal api=24 build=<eboot hash> manifest=<sha256 of manifest.txt>`,
by temp file and rename, and reads it back; `install-vita.sh` writes the same line from the zip's
`package-manifest.json`. The state table then says "update to 0.8.1", "this app is older (0.8.0) than
the installed overlay (0.8.1): Install goes back to 0.8.0", or "the same version, the debug build".
`vjo_app.txt` stays the one line the Shell reads (`shell/worker.c:1892-1908`).

The page shows what the action will do before anything happens, with the exact lines:

```
Update the overlay from v0.8.0 to v0.8.1
1. Copy the 3 plugins and their RCO to ur0:tai and ur0:data/VitaJPOverlay, and the OCR models
   (11.7 MB) to ux0:data/VitaJPOverlay/ocr; a file that already matches is not written again.
   The dictionary is already installed: once it is checked, this app's copy is deleted (157 MB freed).
2. Back up ux0:tai/config.txt and ur0:tai/config.txt as config.txt.vjo-20261016-071503.bak in each
   folder (kept).
3. ux0:tai/config.txt (taiHEN reads this one at boot): remove the OCR worker's lines from *ALL,
   *PCSG00415.
4. ur0:tai/config.txt: remove the OCR worker's lines from *ALL, *PCSG00415.
5. config.ini: local_dictionaries = plaintext.vjdict (every other setting stays; the old file is kept
   as config.ini.bak).
6. Read everything back to check it.
Then restart the Vita yourself: this app never restarts it.
If the Vita does not start: hold L while you turn it on (h-encore or h-encore2: open its menu, holding
R as h-encore starts, and choose Exit while holding L), then in VitaShell put # before the two
VitaJPOverlay lines of ur0:tai/config.txt and ux0:tai/config.txt, or choose Uninstall here.
If a game does not start: turn it OFF on the Games page, or hold L while you start it.
[Update]  [Uninstall]
×: Update   Left/Right: action   Up/Down: scroll   ○: exit
```

**Reading it.** The plan box holds about ten lines. A plan or a result longer than its box scrolls a
line at a time with Up/Down (or the left stick) and a box at a time with L/R (as L/R page the Games
list), a bar at its right shows where the view is (by entries: only the entries in view are laid out),
and only then do the hints say `Up/Down: scroll`. A plan has at most 40 lines: one longer
than that lists fewer of `config.ini`'s changed lines, then fewer of each `config.txt`'s exact lines
(never fewer than two of either), and counts the rest (`and 6 more lines (log.txt has every one).`); its
closing lines, the restart, the recovery and the backups from before the overlay, are always shown
(code audit note 2). The exact lines are a minimal diff (below, "The exact lines").

Enter starts it; a progress bar shows the step and the bytes; at the end the page says what was done
and "Restart the Vita now", with the recovery text again. **The app never restarts the Vita** (no
power call), and it never repeats anything after a failure: it stops and says what is left as it was.

The page's actions, each with its own plan and confirmation: **Install**, **Update** or **Repair** by
the state (one procedure: a file that already has the payload's SHA-256 is not written again), or
**Check and repair** when everything is installed (it hashes every file and resets a stuck boot
guard, below); in the interrupted state **Put it back** and **Keep the current one** (below); and
**Uninstall**.

## What Install (and Update, Repair) does, in order

0. **Checks, before the OK.** The manifest is the app's own (R4); every payload file is there with its
   size (the dictionary in the app, or already installed with its hash); ur0 and ux0 have room for the
   copies and their temporary files (every file it will write, plus 512 KiB);
   every configuration that exists reads, and the editor accepts it, makes its new text and checks it
   (below); the new `config.ini` text is made and parsed with `vjo_config_parse`, and a setting it would
   turn off or warn about is left as it was, with a note in the plan (R6). Every new text is known
   before the OK, so the plan shows them, and a text equal to its file's bytes is not written at all.
1. **After the OK.** The PS button and the power-off menu are locked (`sceShellUtilLock(
   SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN | SCE_SHELL_UTIL_LOCK_TYPE_POWEROFF_MENU)`; VitaShell locks the PS
   button for its installs, `utils.c:146-160`) and auto-suspend is held off (`sceKernelPowerTick(
   SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND)` every 10 s, `utils.c:128-138`) until the install ends.
   Both are best effort: a refusal is logged and the install goes on (in a system application they are
   unmeasured).
   `app0:` is unmounted. Each target directory is probed: a `.vjo-probe` file is created, synced,
   renamed and deleted in `ur0:tai`, `ur0:data/VitaJPOverlay`, `ux0:data/VitaJPOverlay`, the
   dictionary's folder, `ux0:tai` when its `config.txt` exists, and the app folder. A failed probe stops
   the install before anything is written.
2. **Backups.** Each configuration the install changes is copied to
   `config.txt.vjo-YYYYMMDD-HHMMSS.bak` in its own folder (local time), written, synced and read back.
   The name is never reused (`-2`, `-3` when it exists) and the app never deletes one. A configuration
   whose new text equals its old one is neither backed up nor written, so an ordinary update has no
   swap at all. Since our lines name the same paths in every version and step 3 replaces the files at
   those paths, a backup taken before an update loads the new plugins too (taiHEN loads whatever is at
   the path, `plugin.c:165-172`): it is "the configuration from before this install", which does not
   undo an update. The plan names the newest backup that has no kernel line of ours, if there is one,
   as the configuration from before the overlay.
3. **Files.** Each copied file goes to `<destination>.vjo-new`, is synced, read back and hashed
   against the manifest; then the old file (if any) is renamed to `.vjo-old`, the new one into place,
   and `.vjo-old` removed. The dictionary is moved and hashed (above). At most 256 KiB is read,
   written or hashed per call on the install's thread, so the page keeps drawing; the boot guard's
   record `ur0:data/VitaJPOverlay/boot.txt` is emptied with the plugins, as `install_ftp.sh`
   `reset_boot_record` does: a new build starts with a clean record (`docs/boot-safety.md`). The running
   kernel writes its in-memory record at its next change, the Shell's ok 60 s after the start
   (`kernel/boot.c` `BOOT_OK_MIN_US`), so the reset waits for 70 s of uptime and the done text says when it
   was left for a later Check and repair.
4. **config.ini** (below), by the app's own `publish` (a temp file, sync, the old file as
   `config.ini.bak`, rename; `settings/preferences.c:61-90`). Only one `config.ini.bak` is kept, as for
   the app's saves; the plan says so.
5. **config.txt.** ux0's first, then ur0's: for each configuration whose new text differs from its
   old one, the new text is written to `config.txt.vjo-new`, synced and read back; then `config.txt` is
   renamed to `config.txt.vjo-old` and `config.txt.vjo-new` to `config.txt` in two consecutive calls,
   synced, read back and compared byte for byte, and `config.txt.vjo-old` removed. If the last
   read-back differs, the file read is renamed to `config.txt.vjo-bad`, `config.txt.vjo-old` is renamed
   back, and the step reports what is on the card.

   The Vita's rename cannot replace a file, so between those two calls the file is missing. While both
   configurations exist that moment stays bootable (taiHEN reads the other one), which is why ux0's is
   swapped first. With only ur0's, or on an SD2Vita setup (whose ux0 file taiHEN cannot see at boot), a
   power cut exactly then leaves taiHEN no configuration, and HENkaku does not start (below); holding L
   at power-on recovers it. The next start of the app finds the stopped swap's files and offers, with the
   user's OK, to put one back: `.vjo-new` when it is the editor's result of `.vjo-old` (an install's or an
   uninstall's; complete: written, synced and read back before the first rename), else `.vjo-old`.
   A `.vjo-new` beside an intact `config.txt` and no `.vjo-old` is a stop before the swap began (during
   `.vjo-new`'s own write: empty or a prefix after a power cut): `config.txt` is as it was, so
   **Keep the current one** is offered first, and **Put it back** only when `.vjo-new` is the editor's
   result of the current `config.txt` byte for byte, a result that changes it (a copy of `config.txt` is
   no run's whole file: an install that appends its sections, stopped at `config.txt`'s length, leaves
   one; the follow-ups' audit); a file that is neither is never put in place
   (code audit R2). The reason names the run that was writing it, an install's or an uninstall's (the one
   whose edit of the current `config.txt` starts with the file's bytes; "an install's or uninstall's" when
   both could have, as for an empty file), and what to choose again after Keep. A missing `config.txt`
   with no whole file to put back offers nothing: the reason names the newest dated backup to copy in
   VitaShell. With
   h-encore the configuration may have come back as the stock text meanwhile (both files missing make it
   write both): the page says what `config.txt` is now (HENkaku's default, the overlay's, or another),
   and **Put it back** renames the current one to a dated backup first, **Keep the current one** keeps
   it and renames the leftovers to dated backups (`config.txt.vjo-<date>-old.bak`, `-new.bak`). Neither
   removes a file; an Install stops rather than write over a leftover. Writing the file in place instead has no such moment,
   but a cut during the write leaves a prefix of it, which can end at a multiple of 4 KiB (a full card,
   a cluster), the one length taiHEN reads past its buffer (`plugin.c:96`). The rename's window is two
   metadata calls on a thread that does nothing else; it is the smaller risk, and both are recoverable
   with L.
6. **Stale copies.** A `VitaJPOverlay_Kernel.skprx`, `VitaJPOverlay_Shell.suprx` or
   `VitaJPOverlay_OCR.suprx` in `ux0:tai/` is renamed to `<name>.vjo-stale-<date>` (a name of its own,
   outside the leftover rule below; our installers never put one there;
   the kernel loads the worker from `ux0:tai/` only when `ur0:tai/`'s is missing, `VJO_GAME_OCR_PATH_UX0`
   in t3-worker-any-game's PR, so a stale copy there could be loaded in its place). The plan lists each.
7. **The records.** `ux0:data/VitaJPOverlay/vjo_app.txt` gets the app's own stamp (read from
   `ux0:app/VJOSET001/vjo_app.txt`): the Shell's check (#122, `shell/worker.c check_app`) then finds the
   installed app and the plugins' record equal. Then `installed.txt` (above).

Files go first so that a configuration never names a missing file; the configurations come after
the files so that a failure before step 5 leaves the old overlay loading as it was. Each file is the
old or the new one at any moment, and each configuration too except between the swap's two renames. A
power cut or a crash leaves the next start in "update", "repair" or "interrupted", whose action
finishes the job. A run that stops says what does: the same action again (`Install again finishes the
job.`, or `Uninstall again` after an uninstall), or, when a stopped swap's file is left beside a
configuration and after Put it back or Keep, the Install page's next plan (`the Install page shows what is
left to do`), which offers to put it back or keep the current one first. The leftover rule: a configuration's `config.txt.vjo-new` and `.vjo-old` are never
removed (they make the state "interrupted", above). A payload file's `.vjo-new` or `.vjo-old` with the
payload's SHA-256 is used first (a stopped swap's new file, or the old one put aside when the payload's
is the same, as with the frozen dictionary); one without it is an interrupted copy of the overlay's own
file and is replaced; beside a destination that is right, both are the overlay's own and go.

| a stop at | on the card | next boot |
|---|---|---|
| steps 0-2 | nothing changed, at most a partial dated backup | as before |
| step 3, between a file's two renames | that plugin missing, `.vjo-old` beside it | taiHEN skips the line it cannot load (`plugin.c:171-172`); a Shell without its kernel goes inert (`docs/boot-safety.md`) |
| step 3, between two files | the new kernel with the old Shell, or the reverse | an API mismatch makes the Shell inert and the worker unavailable (`shell/worker.c:2226-2227`, `game_ocr/main.c:1492`); one API from two builds runs together [untested] |
| step 4 | `config.ini` old or new | the Shell runs on its defaults if the stop falls between the renames |
| step 5, ux0's window | `ux0:tai/config.txt` missing | taiHEN reads ur0's file, as it was (`plugin.c:73-83`) |
| step 5, ur0's window, no ux0 file | no configuration taiHEN can open | no HENkaku (`taihen.c:349-353`); hold L recovers (`taihen.c:346-357`) |
| steps 6-7 | stale copies, the records | boots |

A game suspended behind the app (the app is a system application) keeps the plugins it loaded until
the restart: the files on the card are replaced, not the loaded modules, as with `install-vita.sh`
over FTP. The dictionary and the models are opened per lookup and per job (`core/local_dict.c`
closes its files at the end of each lookup), and neither runs while the game is suspended.

## The config.txt editor

`core/taicfg.c` gets the editor; `tools/patch_tai_config.py` gets the same rules, so the app and
`install-vita.sh` write the same bytes. Both are tested on one set of fixtures with expected outputs
(`tools/tests/fixtures/taicfg/`), and against each other.

**How taiHEN reads the file** (taiHEN 309b380 `plugin.c`, `taihen.c`, `hen.c`; its parser
DaveeFTW/taihen-parser 6d817fd `lexer.c`, `parser.c`):

- `ux0:tai/config.txt` is opened first and `ur0:tai/config.txt` only when that fails
  (`plugin.c:73-83`); the whole file is read into a block of its size rounded up to 4 KiB and parsed
  as a C string (`plugin.c:85-96`).
- A line ends at CR or LF only (`lexer.c:104-117`); leading and trailing bytes of the six C spaces
  (space, `\t`, `\n`, `\v`, `\f`, `\r`) are ignored (`lexer.c:24-35,78-102`); an empty line or one
  starting with `#` is a comment; `*NAME` opens a section and `*!NAME` one that stops the reading for
  its title at the next section header; `ALL` matches every title but `KERNEL`; names compare exactly
  (`*kernel`, `*Main` and `*all` are other titles); any other line is a plugin path; parsing stops at
  the first raw line, spaces included, of 256 bytes or more (`lexer.c:127-135`).
- `taihen_config_validate` returns non-zero for a valid file and zero for an invalid one, but
  `plugin.c:129` treats non-zero as the failure, and `check_utf8` rejects any valid character
  (`parser.c:70-83`), so in effect taiHEN keeps every file that has a section or a path line and
  refuses one with nothing but comments and blank lines: it then loads no plugin, but starts
  (`plugin_load_config` returns 1, which `taihen.c:350` does not treat as a failure). A 0-byte file
  asks `ksceKernelAllocMemBlock` for 0 bytes (`plugin.c:95-96`); whether that succeeds is not
  documented, and taiHEN opens an empty ux0 file without falling back to ur0's, so the editor refuses
  an empty file ("cannot": restore a backup or HENkaku's default) rather than treat it as missing.
- When neither file can be opened, `plugin_load_config` returns the open's error and taiHEN's
  `module_start` fails (`taihen.c:349-353`): no HENkaku, so no VitaShell, until the Vita is started
  with L held, which skips the configuration altogether (`taihen.c:346-357`). TheOfficialFloW's taiHEN
  9f9a904, which h-encore and h-encore2 ship, starts anyway without a configuration (`taihen.c:348-353`),
  with no plugin loaded.
- Holding L when an app starts skips that app's taiHEN plugins (`hen.c:368-373`); t3-worker-any-game's
  kernel loader repeats that test for the worker it loads.

**Which file.** taiHEN reads `ux0:tai/config.txt` when it exists at the time it starts, else
`ur0:tai/config.txt`. HENkaku's own installer writes both when they are missing (henkaku 784e483
`bootstrap/bootstrap.c:441-448`). With an SD2Vita, ux0 is the SD card only once a storage plugin
listed in ur0's `*KERNEL` has run (StorageMgr, gamesd, YAMT), so taiHEN reads ur0's, unless a Sony
card or a PCH-2000's or PS TV's internal 1 GB is mounted as ux0 at that time. The app cannot ask
taiHEN which file it read, so, as `install-vita.sh` and the app's PSP registration (#124) do, it edits
every configuration that exists; the plan says which one taiHEN reads at boot, naming a storage
plugin when ur0's lists one. When neither exists (or both are empty) it refuses ("taiHEN's config.txt
was not found: is HENkaku installed?"); it never creates one.

**Where the plugins go.** Always `ur0:tai/`, whichever configuration is read: the lines name
`ur0:tai/` paths, ur0 is mounted before any plugin loads (so an SD2Vita setup loads them too), and
every earlier install put them there, so an update replaces the same files.

**Our lines.** A path line is the overlay's when its file name (after the last `/` or `:`) is
`VitaJPOverlay_Kernel.skprx`, `VitaJPOverlay_Shell.suprx`, `VitaJPOverlay_OCR.suprx` or the removed
`VitaJPOverlay_Text.suprx`, in any letter case and on any device (`ur0:tai/`, `ur0:/tai/`, a manual
install's `ux0:tai/`): the memory card's file system ignores case, so taiHEN loads each spelling, and
two copies of the kernel plugin would install its hooks twice. The canonical lines are
`ur0:tai/VitaJPOverlay_Kernel.skprx` and `ur0:tai/VitaJPOverlay_Shell.suprx`. Comment lines are never
ours, whatever they mention, except the editor's note lines.

**Install:**

- one canonical kernel line in a `*KERNEL` section that taiHEN reads for `KERNEL` (the first such line
  is kept; else one goes right after the first `*KERNEL` header, as today; else a section is added at
  the end), and one canonical Shell line in a `*main` section likewise;
- every other kernel or Shell line of ours (a second copy, another spelling or device, one in another
  section such as `*ALL`) is commented out with today's prefix `# Mainline OCR: `, so the user sees
  what it was; the Shell line is kept only in a section named exactly `main`, as the kernel line only
  in `KERNEL` (`*ALL` also matches `main`, but a Shell line there loads it into every app);
- worker lines: by the worker mode (below). In `kernel` mode every worker line and every removed Text
  plugin line is removed: a comment and an absent line are the same to taiHEN (`lexer.c:146-151`), so
  removing rather than commenting changes nothing it loads. The section header an older installer
  added for a worker goes with it when it is not `*!` (the halt is set by `*!` alone and acted on at
  the next header, `parser.c:237-264`), no path or comment line is left under it, and it is a title's
  section that held our worker or carries our note; the blank line or note before it goes too. An empty
  `*ALL`, `*KERNEL` or `*main` that is not marked stays. A worker line in a section with other plugins
  (an `*ALL` one) goes alone;
- a section the editor adds (only `*KERNEL` or `*main`, when a file has none) is one note line, its
  header and the plugin, with no blank line: `# VitaJPOverlay (added by its installer)`, `*KERNEL`,
  `ur0:tai/VitaJPOverlay_Kernel.skprx`. The note is what lets Uninstall remove the section; #124's note
  (`# VitaJPOverlay: the OCR worker in this Adrenaline bubble's PSP game (Overlay app)`) is treated the
  same way;
- each line keeps its own line end; added lines use the file's (CRLF when it has one, else LF, or CR
  when it uses only CR); a file without a final line end gets one before an added line;
- a result whose length is a multiple of 4,096 bytes gets one more line end, after a removal too
  (taiHEN's buffer then has no room for the C string's end; `core/taicfg.c` already pads an appended
  section). An input of such a length is accepted: its dated backup is its exact bytes, and the
  recovery text asks for a backup to be copied back only when `config.txt` is missing and there is no
  whole file to put back (a 4,096-byte backup copied back is then one taiHEN reads past; a configuration
  the editor writes always has its pad);
- a UTF-8 byte-order mark stays where it is. taiHEN reads it as part of the first line, so a first
  line `\xEF\xBB\xBF*KERNEL` is a path to taiHEN, before any section, and to the editor too; when a mark
  hides a section header the plan says so (the plugins under it do not load today).

Installing twice gives the same bytes as installing once.

**Uninstall** removes every line of ours that is not a comment (in any spelling), every note line and
the section header after it when nothing else is left in that section. Lines Install commented out
stay commented. So uninstalling after an install of a file that had none of our lines gives back the
original file, except a line end added at the end of a file that had none and the 4 KiB pad. It
cannot bring back the worker sections a `kernel`-mode install removed, which is right: nothing they
name loads without the kernel plugin.

**Refused** (nothing is written; the message says what to do): a file over 64 KiB, a NUL byte, a raw
line of 256 bytes or more, an empty file, no section or path line at all (taiHEN loads nothing from
it: restore a backup or HENkaku's default first), and any result the check rejects. Any other byte is
accepted (a Latin-1 or Shift-JIS comment is nothing to taiHEN or to the editor).

**The check is taiHEN's own parser (R3).** DaveeFTW/taihen-parser 6d817fd (MIT; `lexer.c`,
`parser.c` and their headers) is vendored into `third_party/taihen-parser/`, pinned by SHA-256, and
built into the app and the host tests. For the old and the new text the editor runs
`taihen_config_parse` for `KERNEL`, `main`, every section name in either file and one title named in
none (what `*ALL` gives any other game). The new text must load the kernel plugin exactly once for
`KERNEL` and never for another title, the Shell exactly once for `main` and never for another title,
the worker for no title in `kernel` mode (once for each registered game, and never for `main`, in
`lines` mode), and every other plugin for exactly the titles and in the order it loaded before (none of
ours after Uninstall). A `*!` section that would stop taiHEN before an added line (a `*!ALL` before the
end of a file that has no `*main`, say) fails it: refused, with the reason (the halt's own line is not
named).

**The Python patcher works on bytes** with taiHEN's rules: it cuts lines at `\r` and `\n` only, trims
only `b" \t\n\v\f\r"`, measures the 256-byte limit on the raw line before trimming, and refuses NUL.
Today's `patch_tai_config.py` decodes UTF-8 and uses `str.splitlines()` and `str.strip()`, which also
cut at VT, FF, `\x1c`-`\x1e`, NEL and U+2028/2029 and strip U+00A0, U+3000 and others: the audit
measured `b"# note\x0c*ALL\n..."`, one comment line to taiHEN, written back as a live `*ALL` header. A
differential test runs the Python and the vendored C parser on every fixture and on a generated corpus
of lines built from CR, LF, VT, FF, NEL, U+2028, U+3000, U+00A0, `*`, `!`, `#` and our paths.

## Which games get the worker

None through config.txt. The owner (issue #138, Agent A, 2026-10-16): "Remove the need for a taiHEN
line for each title before the OCR worker can run ... Load the worker from the kernel plugin when a
process starts and its title is enabled in `games.ini` ... Do not use `*ALL`. The installer should
stop writing worker lines for each title and remove the ones older installs left." That is
t3-worker-any-game's PR (kernel, Shell, worker; it merges before this one): the kernel loads
`ur0:tai/VitaJPOverlay_OCR.suprx` (`ux0:tai/`'s only when ur0's is missing) into every process whose
title is ON, never SceShell or a system app but Adrenaline's, skips it while L is held at the app's
start, and for one release tolerates a worker line left in a configuration (the worker refuses a second
copy of itself in a process). AGENTS.md rule 2, `docs/boot-safety.md`, the README's Install and Memory
sections (now [the guide's](guide.md#memory-and-troubleshooting)) and `docs/vita-memory.md` §4.3 change to that rule in t3-worker-any-game's PR.

So this installer, and `install-vita.sh` with it, in **`kernel` mode**:

- installs the worker file in `ur0:tai/` as before, although no line names it;
- writes no worker line, and removes the ones older installs left (above);
- needs no game list: `install-vita.sh --title` is accepted and ignored with a note, and the patcher's
  refusal of an install with no game (`NO_TITLES`, and `install_ftp.sh`'s exit-3 handling) goes;
- the Rubi app's Games page no longer edits config.txt: turning a PSP bubble or Adrenaline ON
  registers nothing (#124's `vjo_psp_register`, its dim "not registered" PSP tag and its status line
  go). Turning any game ON or OFF takes effect at its next start.

In lines mode the done text says that no game has OCR yet and names `install-vita.sh --title`; the
release recipe ships a kernel-mode main VPK only, unless the owner decides otherwise.

The mode is gated twice (R2), so that the loader can be reverted or fixed without an editor change and
no user loses working lines:

- at build time by `VJO_KERNEL_LOADS_WORKER` (`include/vjo_api.h`, t3-worker-any-game's PR): this PR's
  code fails to build without it (`#error`), which enforces the merge order;
- at run time by the manifests' `worker kernel` or `worker lines` (the VPK's `payload/manifest.txt`
  and the zip's `package-manifest.json`). They come from the built base zip, never from the checkout
  that packages it: the Rubi app's eboot carries a `vjoworker=kernel` (or `lines`) mark compiled
  from the header (`settings/install_page.c`), which the Vita build checks against the header
  (`tools/package_vpk.py --check-build`), `package_release.py` reads from the base zip's app into both
  manifests (an app without it is `lines`), and `install-vita.sh` passes to the patcher. The app accepts a
  payload only when its `worker` and `variant` are its own compiled values (`VJO_KERNEL_LOADS_WORKER`,
  `VJO_DEBUG`), as for `api` and `version` (R4): a mismatched package could otherwise remove working
  worker lines.

In **`lines` mode** (the define 0) the editor and the patcher keep today's per-title rules: every game
registered before keeps its worker line, the worker goes right after the first header of each named
game's section (`install-vita.sh --title`), never under `*ALL`, and misplaced worker lines are
commented out. The in-app Install then keeps the lines it finds and adds no game of its own (a first
install in that mode needs `install-vita.sh --title`, as before #124). The app's Games page edits no
configuration in either mode: #124's registration is removed with this PR, so a reverted loader would
also need it back for PSP rows. The fixtures run both modes.

## Versions (#122)

- After Install the plugins' record is the app's stamp, and `installed.txt` names the build.
- Installing a new VPK gives a new app beside the old plugins: at start the app finds the records are
  another build's, opens on Install with **Update the overlay to 0.8.1**, and lists the plan. An older
  VPK, or the other variant, is named as such (R7).
- The Shell's in-game notice becomes one neutral line, since the Shell cannot tell which side is newer
  when both have one API: `Rubi app and plugins are different builds: open the Rubi app.` The
  app says which way it is and what to do. It replaces both of today's notices (`VJO_APP_NOTICE`,
  `VJO_APP_NOTICE_NEWER` in `core/app_stamp.h`, 82 and 89 characters) with one of 67, so SceShell's
  pages do not grow (AGENTS.md rule 1; the ledger's page check shows it). Nothing else of the Shell,
  the kernel or the worker changes here, and the API stays 22.
- An app without a payload (the zip's) keeps today's banner texts, which name `install-vita.sh`.

## config.ini on install and update

On a first install, and on an install of another build than `installed.txt` names (an update, a
downgrade, the other variant), an existing `config.ini` is patched as below. Over the build
`installed.txt` names (**Check and repair**, the same VPK again) it is the user's: a later choice
(card mining OFF on the Anki page, `log_file = off`, another dictionary list) is no damage, so it is
left as it is and does not make the state "repair" (code audit R3). A missing one is written again: from
`config.ini.bak` when only the backup is there (a save that stopped between `publish`'s two renames: the
app reads the backup, the plugins run on their defaults until the app's next save, code audit note 6),
as the app's own save would write it (its settings as they were, a removed feature's lines and the API
keys gone; the plan says `config.ini is missing beside its backup config.ini.bak (a save that stopped):
it is written again from the backup, your settings as they were.`), else from the template.

When there is none (neither `config.ini` nor its `config.ini.bak`: the app reads the backup when the
file is missing, `settings/preferences.c:34-44`, which a stop between `publish`'s two renames leaves,
`:80-82`), the payload's template is written (the PC installer's first install writes the same
template). Otherwise the text the app's own load reads (the file, else its backup) is kept and patched
as the app's own saves patch it
(`vjo_config_patch_keys`: a removed feature's line is dropped or replaced, an API key goes), plus:

- an older build's `vocr_model` becomes its `vocr_tier` unless the file already names a valid one, and
  `vocr_model` and `vocr_fallback_model` go, as `tools/migrate_config.py` does (`:115-123`);
- `local_dictionary_dir`, `ocr_model_dir` and `text_source` are set to the paths the overlay installs
  to (`install-vita.sh` sets them too);
- `plaintext.vjdict` is put in `local_dictionaries` when it is not listed: in place of 0.7.12's
  `jmdict.vjdict`, else first. A list that would then hold more than 8 names (`core/config.c` turns an
  invalid list off, `:308,531-540`) is left as it was, with a note in the plan;
- the package's `set` lines (a test build's `anki_mining on`; the debug VPK's `log_file on`, so that a
  debug install over 0.7.12's `log_file = off` records its log);
- the removed settings the plugins ignore without a warning go (`frequency_filter`, `font_size`,
  `hw_jpeg`, `combo_delay_ms`, `hachidori_host`), as `tools/migrate_config.py` drops them.

The two installers differ in what the PC path still forces: `install-vita.sh` sets `log_file = on` and
`local_dictionaries = plaintext.vjdict` on every install (its `--set`s, from before this design), where
the app sets `log_file` only for the debug build and puts `plaintext.vjdict` into the user's list.

Every other setting is the user's and is kept: `vocr_tier`, `ocr_mode`, `ocr_boost_clock` (the OCR &
lookup page's choices, overlay-ocr-mode-setting), `log_file`, `anki_mining` (unless the package sets
it), the user's comments and order. The plan lists each changed line of `config.ini`, and names each line
that only goes (a removed setting, an API key, a second copy of a setting) by its setting, never its value:
`ocr_backend: a removed setting, goes` (12 lines at most, fewer in a long plan, the rest counted; a text that
differs but sets every setting as before says so, never an empty list), and each `config.txt` it changes
with the exact lines it adds and removes (8 per file on the page, fewer in a long plan, every one in
`log.txt`), as AGENTS.md rule 7 asks. The result is parsed with `vjo_config_parse` before the OK (step 0).

**The exact lines** (`vjo_install_diff`, `settings/install.c`) are a minimal line diff of the
configuration and the editor's result, the same lines on the page and in `log.txt`: CR, LF and CRLF each
end one line. The common first and last lines are skipped; Myers' O(ND) pass gives the fewest changed
lines D, and a table over the diagonals that D allows picks, of the shortest diffs, the one that keeps
the most lines that do not name the overlay, then shows a removal before an addition. So a line the
editor comments out is its old text removed and its commented text added, and a user's line is never
shown as removed and added back where another shortest diff keeps it (code audit note 1: when a line of
ours had a copy further on, the earlier greedy walk showed the user's lines in between as added, then
removed). A changed stretch of more than 1,024 lines, or 256 changes or more, gets the greedy walk, still
a correct edit; the editor's own results never come near (it changes a few lines; the 64 KiB input
limit is the editor's). Its tables are 45,060 B of the app's static memory (the host build's `dw`;
the system app's, outside the plugins' ledger, within `check_settings_self`'s budget).

## Uninstall

Uninstall shows its plan (the lines it removes from each configuration and the dated backup it
makes), then removes the lines, reads each configuration back and says "Restart the Vita: the overlay
no longer loads". It keeps every backup, the plugin files, the models, the dictionary and the user's
data (`config.ini`, `games.ini`, regions, the Anki queue and exports), as `install_ftp.sh --uninstall`
does, and says where they are for whoever wants the room back. The app itself is deleted from
LiveArea as any app. The "no payload" app offers Uninstall too.

## Recovery (R1)

The app's pages and the README say the same, in this order:

1. A boot that does not complete makes the next one a safe boot (no hooks, no overlay; `status.txt`
   says why), as today (`docs/boot-safety.md`): most bad builds never need more. **Check and repair**
   resets a boot guard stuck in safe mode once the files are right.
2. If the Vita does not start, or HENkaku does not (no VitaShell): hold L while turning it on with
   Ensō; with h-encore or h-encore2, open its menu (h-encore shows it only while R is held as it
   starts, h-encore 6d43b4c `payload.c:672-713`; h-encore2 always shows it, h-encore-2 c59a248
   `payload/payload.c:690-711`) and choose Exit while holding L, as h-encore2's README says ("holding
   the L trigger while exiting the h-encore bootstrap menu", `README.md:64`): taiHEN then reads no
   configuration (`taihen.c:346-357`). Then either open VitaShell and
   put `#` before the two `VitaJPOverlay` lines of `ur0:tai/config.txt` (and `ux0:tai/config.txt`), or
   open the Rubi app and choose Uninstall: homebrew still runs after an L start, since taiHEN installs
   its patches before the L test (`taihen.c:341-348`), and the app keeps its extended permissions: Ensō
   loads `taihen.skprx` and `henkaku.skprx` from `boot_config.txt` (enso `installer/src/kernel.c:521`;
   h-encore loads both itself, `payload.c:555-581`), and henkaku's `module_start` installs the permission
   hook with no button test (henkaku `plugin/kernel.c:179-189`) [from the sources; a console run shows
   it]. Restart normally. This works after any install.
   A `config.txt.vjo-<date>.bak` is the configuration from before that install; it does not undo an
   update, whose plugin files are replaced. If a `config.txt` is missing, copying the newest backup to
   `config.txt` makes taiHEN start again.
3. If one game does not start or misbehaves: turn it OFF on the Games page, or hold L while starting it
   (t3-worker-any-game's loader honours it as taiHEN does).
4. Never more than one attempt: the app does nothing by itself after a restart.

## The release process

- `tools/package_vpk.py RELEASE.zip -o OUT.vpk` writes the VPK of a `package_release.py` zip: the zip's
  `Rubi.vpk`, entry for entry, then `payload/` (the files the zip installs, the same bytes,
  deflated) and `payload/manifest.txt`. It takes the app's facts from the app itself (its start line,
  its `vjoworker=` mark, its stamp), checks that the payload's hashes are the zip manifest's, the entry
  order and the SELF scan (above); `--verify` checks a VPK again, `--check-build` is the Vita build's
  module check (`vjo_one_vpk`). `package_release.py` writes the same payload manifest into the zip as
  `payload-manifest.txt` and its `worker` into `package-manifest.json`, so that `install-vita.sh` records
  the same `installed.txt` line.
- `tools/package_variants.py` writes both VPKs, both zips and `SHA256SUMS`, normal first; the debug
  VPK and zip `set log_file on`.
- CI keeps building the base zips (the models are added at packaging, as for the zips today), and a
  pytest builds a VPK from a fixture base zip.
- `docs/release-variants.md`'s recipe gets the exact `gh release create -R bee-san/Rubi_for_vita ...`
  line with the five assets in order (the normal VPK first) and the release notes' text: the three
  steps, the requirements, the recovery.
- A test build ships both VPKs too; its issue's step 1 becomes "install the VPK in VitaShell, open
  Rubi, Install, restart", and its checklist adds the console checks below.
- README's Install section becomes the three steps and the requirements; `install-vita.sh` moves under
  "Advanced: install from a computer".
- `install-vita.sh` keeps parity: the same patcher and rules, the same dated on-Vita backup name
  (beside today's `config.txt.vjo-bak`), `installed.txt`, and stale `ux0:tai` copies renamed. It
  uploads `config.txt` by FTP STOR in place, as today. VitaShell's FTP server (libftpvita) can rename
  (RNFR/RNTO with `sceIoRename`, `ftpvita.c:691-723`; a failed RNTO answers 550 and then 226, so a rename
  is checked by listing, as `install_ftp.sh` does for a stale copy), so the PC path could swap like the
  app; it keeps its in-place upload for now.

## Tests

- The editor, in C (ctest) and Python (pytest) against the same expected files, through install,
  install again and uninstall, in both worker modes; the Python against the vendored parser on every
  fixture and a generated corpus. Fixtures:
  1. HENkaku's stock `ur0` and `ux0` files (`bootstrap.c:46-69`) and its "updated" layout with the
     stock text appended again (`bootstrap.c:553-559`);
  2. Adrenaline's kernel line (`ux0:app/PSPEMUCFW/sce_module/adrenaline_kernel.skprx`, Adrenaline's
     readme) and an older install's `*PSPEMUCFW` worker section;
  3. repeated sections; CRLF; CR only; mixed line ends (a lone CR or LF in a CRLF file), `\r\r\n`; no
     final line end;
  4. a BOM before a comment and before `*KERNEL`;
  5. bytes that are line ends or spaces to Python but not to taiHEN: VT and FF inside a line, NEL,
     U+2028/2029, `\x1c`-`\x1e`, U+00A0 and U+3000 at a line's start or end;
  6. raw lines of 255 and 256 bytes, spaces included, before and after the insertion point;
  7. `*kernel`, `*Main`, `*all`; `* KERNEL`, `*  !  ALL`, tabs; our paths with trailing spaces;
  8. `*!KERNEL`, `*!main`, a `*!ALL` before and after `*main`;
  9. our plugins in the wrong section: the worker under `*main`, `*KERNEL` and `*NPXS10015`; the kernel
     plugin under `*main` and `*ALL`; the Shell under `*KERNEL`;
  10. old lines: the Text plugin, an `*ALL` worker, per-title worker sections from `install-vita.sh`
      and #124, a commented-out line of ours, `# Mainline OCR:` lines of an earlier run, a comment that
      merely names our files, a duplicate, a `ux0:tai/` and a case-variant kernel line;
  11. a worker section with a user comment left under it; a hand-made `*TITLEID` holding only our worker
      (the guide's manual install); a `*!TITLEID` left empty, which stays;
  12. an input of exactly 4,096 B, a removal that leaves exactly 4,096 B, the CRLF pad;
  13. Japanese UTF-8 comments; Latin-1 and Shift-JIS comments;
  14. refusals: NUL, a 256-byte line, empty, comments only, over 64 KiB, a `*!ALL` that hides an added
      `*main`.
- The install engine: the app's real code on a temporary memory card (real files, as the Games page
  tests), with `sceIoRename` refusing an existing destination as the Vita does and the probe, lock and
  unmount calls stubbed and recorded: a fresh install (ur0 only, both), update, repair, check, the
  other variant, an older VPK, uninstall, the no-payload app, the dictionary present, absent or
  different, Unsafe Homebrew off, a damaged manifest (each R4 rule), a full dictionary list; failures
  injected at every open, write, sync, rename, read-back and probe (the card is then as the plan says:
  old or new); a stop after every step followed by a fresh start, the "interrupted" state at each of
  the swap's two windows; the plan's text.
- Packaging: the VPK's entries, order, manifest, hashes, eboot and stamp; the SELF scan; both variants;
  `--verify`.
- Preflight: a row for these suites.

Vita3K is not evidence for any of it (it has no taiHEN). The test build's console run is.

## After the code audit (audit-overlay-one-vpk-install-b's notes)

The code audit (MERGE_OK on #158) left six suggestions, all taken here:

1. The exact lines are a minimal diff, above.
2. A plan longer than its 40 lines keeps its restart and recovery lines; L/R page the box and a bar shows
   where the view is (above, "Reading it").
3. The test gaps: a failed put-back puts the current `config.txt` back (N5), CRLF is one line end (N17),
   `--verify`'s check of the version against the file name (N22), no VPK of a lines-mode build (N23), the
   reason a sentence a line on the page (N24).
4. A stopped uninstall's `.vjo-new` is named an uninstall's, and its next step is Uninstall; this build
   with a damaged plugin or another record is **Repair**, not "another build of it is installed".
5. `docs/console-test.md`'s notice and backup lines and `tools/install_ftp.sh`'s header are current; N1
   above says what is done with an empty file.
6. Over this build a `config.ini` missing beside its `config.ini.bak` is written again from the backup.

Host tests only (`host/tests/test_settings_install.c`); the page's paging is a console check of
test.4.

## Unmeasured until a console run

- VitaShell's install time for the 61 MB VPK, and the app's Install time: about 170 MB read and
  hashed, 256 KiB per call on a thread on CPU3 (the app is a system application), whose CPU3 cost is
  unmeasured (AGENTS.md rule 8).
- That a system application may unmount its `app0:`, that `sceIoRename` then moves the dictionary out
  of `ux0:app/VJOSET001`, and that the Unsafe Homebrew probe means for a system application what it
  means for VitaShell.
- What the promoter does with a previous version's files on an update, and to a running app (one
  observation: test.4's install stopped with `0x80101114` while test.3's app was suspended, and went
  through after a restart, #194; the code's meaning is undocumented in the sources we read).
- That the app's writes to `ur0:tai` work while a game is suspended behind it.
- `sceShellUtilLock` and `sceKernelPowerTick` in a system application; the app's permissions after an
  L start.
- `ksceKernelAllocMemBlock` with a size of 0 (an empty `config.txt`).

The owner's first console run of this is AGENTS.md rule 7's "show the exact files and taiHEN
configuration edits, back up both configurations, state the recovery steps, get the owner's explicit
OK": the plan screen shows them, and it is that run's only change.

## The design audit and what changed

The design audit (audit-overlay-one-vpk-install-design, DESIGN_CHANGES on 3aef3bd) found, and this
version folds in:

- E1-E3 (sent during the review): the app folder is writable only with `app0:` unmounted; Unsafe
  Homebrew is a prerequisite; no `config.txt` exists between the swap's two renames (the
  "interrupted" state).
- R1: the recovery text recovered a first install only (an update's backup loads the new files);
  h-encore; one game.
- R2: the worker-line policy gated by `VJO_KERNEL_LOADS_WORKER` and the manifests' `worker` key, with
  both modes in the fixtures.
- R3: the check is taiHEN's own parser, vendored; the Python patcher on bytes, with taiHEN's line ends
  and spaces; a differential test.
- R4: the manifest's file set is the build's own, compiled in.
- R5: VitaShell's scan stops at the eboot; its free-space check is 40 MiB lenient; the packaging's own
  SELF scan and entry order.
- R6: every new text made and checked before the OK, `config.ini` included; probes after the OK; no
  write of an unchanged file; the PS button, power-off menu and auto-suspend held.
- R7: `installed.txt` names the version and the variant.

The delta review (audit-overlay-one-vpk-install-design-b, on daa5107 and 4291ccc) added C1-C4: the
h-encore and h-encore2 recovery (open the menu, Exit while holding L); the worker mode from the built
base zip, with the app requiring its compiled worker mode and variant; `config.ini` "none" meaning
neither `config.ini` nor its `.bak`; and the leftover rule (a configuration's leftovers are never
removed, the "interrupted" state also beside a `config.txt` h-encore wrote back; stale `ux0:tai`
copies as `.vjo-stale-<date>`). Its notes N1-N17 are folded in where they change the design (the
compiled-values bullet, the taiHEN fork, the permissions after an L start, the boot record's 60 s,
lines mode's done text, libftpvita's rename).
- Notes taken: an empty file named for what it is (N1: it is refused as "cannot", restore a backup or
  HENkaku's default, rather than treated as missing, above), the 4 KiB input (N2), `config.ini` details (N3),
  stale copies renamed (N4), the storage plugin in "Which file" (N5), sizes (N6), wording (N7),
  `install-vita.sh` parity (N8), "interrupted" first (N9), the no-payload app's Uninstall (N10; no
  Check, as it has no manifest), any byte but NUL accepted (N11), the boot guard reset (N12), closing the app before a VPK
  install (N13), the CPU3 note (N14), the console run as rule 7's checkpoint (N15).
