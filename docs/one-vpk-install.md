# One VPK is the whole install

The owner (PR #134, 2026-10-16 06:59Z): "i would hope the easiest install would be to go to GitHub
releases, download the latest vpk, and install it and it's done. Let's queue an agent to make this
our release process".

This is the design. It was reviewed before any code was written; the review's findings and what
changed are at the end.

## The user's three steps

1. Download `VitaJPOverlay-<version>.vpk` from the latest GitHub release.
2. Copy it to the Vita (VitaShell: Select starts FTP, or its USB mode) and install it in VitaShell
   (press Enter on the file).
3. Open **Overlay** from LiveArea. It opens on its **Install** page, which lists what it will do;
   choose **Install**, then restart the Vita.

Nothing is downloaded on the Vita: the VPK holds the plugins, the models and the dictionary. Then a
game is played as today (L + R). An update is the same three steps with the new VPK: the app sees
that its overlay is not the installed one and its Install page offers **Update**.

Requirements stay those of today's install: a PS Vita on 3.60-3.74 with HENkaku/Ensō or h-encore,
and VitaShell, plus one setting the PC installer did not need: **Settings > HENkaku Settings >
Enable Unsafe Homebrew** on. HENkaku's default is off (henkaku 784e483 `plugin/kernel.c:156`), and
only then does an unsafe SELF such as the app's `eboot.bin` get extended permissions
(`plugin/kernel.c:60-73`); without them the app runs sandboxed and cannot write `ur0:tai`. The app
detects it as VitaShell does (`sceIoDevctl("ux0:", 0x3001, NULL, 0, NULL, 0)` returns `0x80010030`,
VitaShell `init.c:353-355`) and says how to turn it on. `install-vita.sh` (the release zip) stays as
the advanced and developer path.

## What a release ships

| asset | build | for |
|---|---|---|
| `VitaJPOverlay-<version>.vpk` | normal | everyone: the main download |
| `VitaJPOverlay-<version>-debug.vpk` | debug (the test builds') | console tests, a full log |
| `VitaJPOverlay-<version>.zip`, `VitaJPOverlay-<version>-debug.zip` | normal, debug | `install-vita.sh`, a manual install, the tools |
| `SHA256SUMS` | | all four, the normal VPK first |

The two builds are #141's (`docs/release-variants.md`): same models, dictionary and config template;
the debug build has the full log and the self-test. Both VPKs are the same app (title ID
`VJOSET001`), so either installs over the other; each carries its own build's plugins.

The zips keep today's layout and their own `JPOverlaySettings.vpk`, which carries no overlay files
(below: "an app without a payload"), so a zip is not 50 MB larger.

## Inside the VPK

The Overlay app as today (`eboot.bin`, `sce_sys/`, the #122 stamp `vjo_app.txt`), plus `payload/`,
which mirrors the zip's device folders:

| in the VPK | installed to | bytes [measured: #141's packages, e5e2a26] |
|---|---|---:|
| `payload/ur0/tai/VitaJPOverlay_Kernel.skprx` | `ur0:tai/` | 31,316 (debug 34,556) |
| `payload/ur0/tai/VitaJPOverlay_Shell.suprx` | `ur0:tai/` | 75,904 (debug 83,908) |
| `payload/ur0/tai/VitaJPOverlay_OCR.suprx` | `ur0:tai/` | 63,552 (debug 64,764) |
| `payload/ur0/data/VitaJPOverlay/vitajpoverlay.rco` | `ur0:data/VitaJPOverlay/` | 6,000 |
| `payload/ux0/data/VitaJPOverlay/ocr/` (3 recognizers, 4 LMs, `LM-ATTRIBUTION.md`) | `ux0:data/VitaJPOverlay/ocr/` | 10,614,250 |
| `payload/ux0/data/VitaJPOverlay/dictionaries/plaintext.vjdict` | moved to `ux0:data/VitaJPOverlay/dictionaries/` | 143,425,536 |
| `payload/ux0/data/VitaJPOverlay/config.ini` | written only when there is none | 3,554 |
| `payload/licenses/` (the dictionary's attribution, the JMdict licence) | stays in the app | small |
| `payload/manifest.txt` | read by the app | small |

`payload/manifest.txt` is written by `tools/package_release.py`, one line per item:

```
vjopayload 1
version 0.8.0
variant normal
api 22
file ur0:tai/VitaJPOverlay_Kernel.skprx 31316 <sha256> copy
...
file ux0:data/VitaJPOverlay/dictionaries/plaintext.vjdict 143425536 <sha256> move
template ux0:data/VitaJPOverlay/config.ini 3554 <sha256>
set anki_mining on
```

`set` lines are the package's own settings (`package_release.py --set`, which a test build uses for
`anki_mining = on`; the final release has none). The app accepts a destination only under
`ur0:tai/VitaJPOverlay_`, `ur0:data/VitaJPOverlay/` or `ux0:data/VitaJPOverlay/`, of the characters
`[A-Za-z0-9_./-]` and with no `..`, so a damaged or edited manifest cannot name `config.txt`,
`henkaku.suprx` or anything else. The manifest is bounded (64 items, 255-byte lines).

Size: the VPK is about the release zip's size, 53.0 MB (the zips of #141's head measured 53,005,464 B
normal), since the zip's added bytes are the same files; installed, the payload is about 155 MB
[estimate: the sum of the table, plus the app's own 1-4 MB].

## What VitaShell does with it

From VitaShell's source (TheOfficialFloW/VitaShell 81af709, `package_installer.c` `install_thread`):

- It checks every SELF in the archive (`archive.c:416-500`, `archiveCheckFilesForUnsafeFself`): an
  import of SceVshBridge's `_vshIoMount`/`vshIoUmount` (NIDs `0x3C522C35`, `0x35BC26AC`, `elf.c:126-134`)
  shows the "can potentially brick your device" warning; any other unsafe SELF (an authid other than
  `0x2F00000000000002`, or ScePromoterUtil/SceShellSvc imports) the "requests extended permissions"
  one. Today's VPK already shows the latter (its eboot is an unsafe homebrew SELF). Our plugins import
  neither mount call (`shell/boot.c` uses `_vshKernelSearchModuleByName` only): the VPK shows the same
  warning as today. The packaging checks this on the built VPK (it reimplements that scan).
- `checkMemoryCardFreeSpace(PACKAGE_DIR, size)` (`utils.c:91-120`) compares the archive's unpacked
  size with ux0's free space plus 40 MiB; then it extracts everything into `ux0:data/pkg`, writes
  `sce_sys/package/head.bin`, and calls `scePromoterUtilityPromotePkgWithRif`, which has SceShell
  move the files into `ux0:app/VJOSET001` and add the bubble (henkaku wiki, ScePromoterUtil and
  Packages). So the unpacked size is needed once, beside the `.vpk` itself: about 155 MB plus the
  53 MB VPK [estimate], and the VPK can be deleted afterwards.
- Nothing in it limits an app's size or its number of files; a zip holds up to 4 GiB and 65,535
  entries, far above ours.
- The scan reads (inflates) every entry once before extracting, so the install reads the archive
  twice. How long that takes on a console is unmeasured.

## The dictionary is moved, not copied

The dictionary is 92% of the payload. Copying it out of the app folder would need another 137 MiB
and a write of 137 MiB. Instead the app moves it (`sceIoRename` within ux0: the same device, no data
written). The app says so on its Install page: "the dictionary (137 MB) is moved out of the app, not
copied".

The app folder cannot be written while it is mounted as the app's `app0:`: VitaShell unmounts its
own (`sceAppMgrUmount("app0:")`, "Allow writing to ux0:app/VITASHELL", VitaShell `init.c:349-351`).
So the install, once its checks pass and before its first move or delete in the app folder, unmounts
`app0:` the same way, and from then on reads the payload and the app's stamp through
`ux0:app/VJOSET001/` only (`settings/app_check.c` already falls back to that path for the stamp).
Nothing else of the app reads `app0:` while it runs. Whether a system application may unmount its
`app0:` is unmeasured; if the unmount or the rename fails, the app copies the dictionary instead when
ux0 has room for it, and says that the app folder then keeps its copy too (274 MiB in all, until the
app is deleted or a later install can move it); without room it stops before anything is written.

- After the move the app folder no longer has it. A **Repair** of a missing or damaged dictionary
  therefore needs the VPK installed again in VitaShell first; the app says that when it is the case.
- An update brings the dictionary again (VitaShell installs the whole VPK). When the installed
  dictionary already has the payload's SHA-256 (the dictionary is frozen, so usually it does), the
  app deletes the copy in the app folder and frees 137 MiB. Otherwise it replaces it: the old file to
  `plaintext.vjdict.vjo-old`, the new one moved in and hashed, then the old one deleted (renamed back
  when the hash fails).
- Plugins, the RCO and the models (about 11 MB) are copied: the app keeps them for a Repair without
  the VPK.

The Vita's `sceIoRename` does not replace an existing file: it returns `SCE_ERROR_ERRNO_EEXIST`
(VitaShell `file.c:518-547` removes the destination and renames again). Every replacement here goes
through `.vjo-old`, so a file is never missing between two renames for longer than one call.

## The app: an Install page

A fourth tab: **Games | OCR & lookup | Anki | Install**. At start the app reads its
`payload/manifest.txt` and works out a state (nothing is written):

| state | when | start page |
|---|---|---|
| not installed | no configuration loads the overlay's kernel and Shell plugins | Install |
| update | the overlay is installed, but a plugin file is not the payload's (size and SHA-256), or the plugins' record (`ux0:data/VitaJPOverlay/vjo_app.txt`, #122) is not this app's stamp | Install |
| repair | this app's overlay, but a data file is missing or has the wrong size | Install |
| installed | every plugin file is the payload's, every data file has its size, both configurations load the plugins, and the record is this app's stamp | Games |
| cannot | Unsafe Homebrew is off (above), or a configuration the editor refuses (below) | Install, with the reason and what to do |
| interrupted | a `config.txt` is missing beside its `config.txt.vjo-new` or `config.txt.vjo-old` (a stop between the swap's two renames, step 4) | Install, offering to finish the swap |
| no payload | the zip's `JPOverlaySettings.vpk` (or a source build): the app carries no overlay | Games; Install says that `install-vita.sh` installs this build |

At start only the four small files (the three plugins and the RCO, about 180 KB) are hashed; the
models and the dictionary are checked by size. Install and Repair hash everything.

The page shows what the action will do before anything happens, with the exact lines:

```
Install the overlay (v0.8.0)
1. Copy 3 plugins to ur0:tai and the OCR models (10.6 MB) to ux0:data/VitaJPOverlay;
   move the dictionary (137 MB) there from this app.
2. Back up ux0:tai/config.txt and ur0:tai/config.txt as config.txt.vjo-20261016-071503.bak (kept).
3. Add to both: *KERNEL  ur0:tai/VitaJPOverlay_Kernel.skprx
                *main    ur0:tai/VitaJPOverlay_Shell.suprx
   and remove the OCR worker's lines an older install left: *PCSG00415, *ALL (3 lines).
4. Read everything back to check it.
Then restart the Vita yourself. If it does not start: hold L while you turn it on, then in
VitaShell put # before the two VitaJPOverlay lines of config.txt in ur0:tai (and ux0:tai).
(config.txt.vjo-20261016-071503.bak is the configuration from before the overlay.)
If one game does not start: turn it OFF on the Games page.
Enter: install   Cancel: not now
```

Enter starts it; a progress bar shows the step and the bytes; at the end the page says what was done
and "Restart the Vita now", with the recovery text again. **The app never restarts the Vita** (no
power call), and it never repeats anything after a failure: it stops and says what is left as it was.

The page's actions, each with its own plan and confirmation: the main one is **Install**, **Update**
or **Repair** by the state (one procedure: files that already have the payload's SHA-256 are not
written again), or **Check and repair** when everything is installed (it hashes every file); and
**Uninstall**.

## What Install (and Update, Repair) does, in order

Nothing is written until every check of step 0 passes.

0. **Checks.** The manifest parses; every payload file is there with its size (the dictionary in the
   app or already installed); ur0 and ux0 have room for the copies and their temporary files (the
   largest file twice, plus 512 KiB, as `settings/psp.c` keeps); each configuration reads, and the
   editor accepts it and simulates the result (below).
1. **Backups.** Each configuration that the install changes is copied to
   `config.txt.vjo-YYYYMMDD-HHMMSS.bak` in its own folder (local time), written, synced and read back.
   The name is never reused (`-2`, `-3` when it exists) and the app never deletes one. A configuration
   whose new text is its old text is neither backed up nor written (step 4), so an ordinary update has
   no swap at all. Our lines name the same paths in every version and step 2 replaces the files at
   those paths, so a backup taken before an update loads the new plugins too (taiHEN loads whatever is
   at the path); only a backup without our kernel line is the configuration from before the overlay.
   The plan names the newest such backup in the folder, if there is one; the recovery text's first
   instruction is to comment out our two lines, which works after any install.
2. **Files.** Each copied file goes to `<destination>.vjo-new`, is synced, read back and hashed
   against the manifest, then the old file (if any) is renamed to `.vjo-old`, the new one into place,
   and `.vjo-old` removed. The dictionary is moved and hashed (above). At most 256 KiB is read,
   written or hashed per frame, so the page keeps drawing.
3. **config.ini.** When there is none, the payload's template is written (the PC installer's first
   install writes the same template). Otherwise it is kept and patched as the app's own saves patch
   it (`vjo_config_patch_keys`: a removed feature's line is dropped or replaced, an API key goes),
   plus: the package's `set` lines; `local_dictionary_dir`, `ocr_model_dir` and `text_source` set to
   the paths the overlay installs to (`install-vita.sh` sets them too); and `plaintext.vjdict` first in
   `local_dictionaries` when it is not listed (0.7.12's `jmdict.vjdict` is replaced by it; other
   dictionaries keep their order). Every other setting is the user's and is kept: `vocr_tier`,
   `ocr_mode`, `ocr_boost_clock` (the OCR & lookup page's choices, overlay-ocr-mode-setting),
   `log_file`, the Anki page's `anki_mining` (unless the package sets it). The plan lists each changed
   line.
4. **config.txt.** For each configuration whose new text differs from its old text: the editor's new
   text is written to `config.txt.vjo-new`,
   synced and read back; `config.txt` is renamed to `config.txt.vjo-old` and `config.txt.vjo-new` to
   `config.txt` in two consecutive calls, synced, read back and compared byte for byte, and
   `config.txt.vjo-old` removed. If the last read-back differs, the backup is written back and the
   step reports what is on the card. ux0's file is replaced before ur0's.

   The Vita's rename cannot replace a file, so between those two calls the file is missing. A power
   cut exactly then, with no other configuration, would leave taiHEN unable to start (above); holding
   L at power-on recovers it, and the recovery text says to copy the backup to `config.txt`. The next
   start of the app finds the configuration missing beside its `.vjo-new` (complete: it was written,
   synced and read back before the first rename) or `.vjo-old`, and offers to finish the swap
   (`.vjo-new` to `config.txt`, else `.vjo-old` back), with the user's OK; it deletes neither while
   `config.txt` is missing. Writing
   the file in place instead has no such moment, but a cut during the write leaves a prefix of it,
   which can end at a multiple of 4 KiB (a full card or a cluster write), the one length taiHEN reads
   past its buffer (`plugin.c:96`). The rename's window is two metadata calls on a thread that does
   nothing else; the write's is the whole write. The rename is the smaller risk; both are
   recoverable with L.
5. **Stale copies.** A `VitaJPOverlay_Kernel.skprx`, `VitaJPOverlay_Shell.suprx` or
   `VitaJPOverlay_OCR.suprx` in `ux0:tai/` is deleted (our installers never put one there; the kernel
   loads the worker from `ur0:tai/`, and from `ux0:tai/` only when ur0's is missing, so a stale copy
   there could be loaded instead of the new one). The plan lists each.
6. **The record.** `ux0:data/VitaJPOverlay/vjo_app.txt` gets the app's own stamp
   (`app0:vjo_app.txt`): the Shell's check (#122, `shell/worker.c check_app`) then finds the installed
   app and the plugins' record equal.
7. **The boot record.** `ur0:data/VitaJPOverlay/boot.txt` is emptied, as `install_ftp.sh`
   `reset_boot_record` does: a new build starts with a clean boot-guard record
   (`docs/boot-safety.md`).

Files go first so that a configuration never names a missing file; the configurations come after
the files so that a failure before step 4 leaves the old overlay loading as it was (the new kernel
plugin with an old configuration's worker lines is the one-release fallback of the kernel-loaded
worker, below); each file and
each file is either the old or the new one at any moment, and each configuration too except
between the swap's two renames (step 4). A power cut or a crash in the middle leaves the next start
in "update", "repair" or "interrupted", whose action finishes the job. A `.vjo-new` or `.vjo-old`
left beside a file that exists is removed by the next install; one beside a missing file is used
first, never removed.

A game suspended behind the app (the app is a system application) keeps the plugins it loaded until
the restart: the files on the card are replaced, not the loaded modules, as with `install-vita.sh`
over FTP. The dictionary and the models are opened per lookup and per job (`core/local_dict.c` closes
its files at the end of each lookup), and neither runs while the game is suspended.

## The config.txt editor

`core/taicfg.c` gets the editor; `tools/patch_tai_config.py` gets the same rules, so the app and
`install-vita.sh` write the same bytes. Both are tested on one set of fixtures with expected
outputs (`tools/tests/fixtures/taicfg/`).

**How taiHEN reads the file** (taiHEN 309b380 `plugin.c`, `taihen.c`, `hen.c`; its parser
DaveeFTW/taihen-parser 6d817fd `lexer.c`, `parser.c`):

- `ux0:tai/config.txt` is opened first and `ur0:tai/config.txt` only when that fails
  (`plugin.c:73-80`); the whole file is read into a block of its size rounded up to 4 KiB and parsed
  as a C string (`plugin.c:96`).
- A line ends at CR or LF; leading and trailing white space is ignored; an empty line or one starting
  with `#` is a comment; `*NAME` opens a section and `*!NAME` one that stops the reading for that
  title at the next section header; `ALL` matches every title but `KERNEL`; any other line is a
  plugin path; parsing stops at the first line of 256 bytes or more (`lexer.c:131-135`).
- `taihen_config_validate` returns non-zero for a valid file and zero for an invalid one, but
  `plugin.c:129` treats non-zero as the failure, and `check_utf8` rejects any valid character, so in
  effect taiHEN loads every file that has a section or a path line and **refuses one with nothing but
  comments and blank lines, or an empty one** (it then loads no plugin at all, but starts:
  `plugin_load_config` returns 1, which `taihen.c:350` does not treat as a failure).
- When neither file can be opened, `plugin_load_config` returns the open's error and taiHEN's
  `module_start` fails (`taihen.c:349-353`): no HENkaku, so no VitaShell, until the Vita is started
  with L held, which skips the configuration altogether (`taihen.c:348`). A missing configuration is
  therefore the one state the editor must never leave, even for a moment, where the other one does
  not exist.
- Holding L when taiHEN starts skips the configuration and every plugin (`taihen.c:346-356`); holding
  it when an app starts skips that app's plugins (`hen.c:368-372`).

**Which file.** taiHEN reads `ux0:tai/config.txt` when it exists at the time it starts, else
`ur0:tai/config.txt`. HENkaku's own installer writes both (henkaku 784e483
`bootstrap/bootstrap.c:441-448`). With Ensō and an SD2Vita, ux0 becomes the SD card only after a
kernel plugin listed in ur0's file has run, so taiHEN reads ur0's. The app cannot ask taiHEN which file
it read, so, as `install-vita.sh` and the app's PSP registration (#124) do, it edits every
configuration that exists, and the plan says which one taiHEN reads first. When neither exists it
refuses ("taiHEN's config.txt was not found: is HENkaku installed?"); it never creates one.

**Where the plugins go.** Always `ur0:tai/`, whichever configuration is read: the lines name
`ur0:tai/` paths, ur0 is mounted before any plugin loads (so an SD2Vita setup loads them too), and
every earlier install put them there, so an update replaces the same files.

**Our lines.** A path line is the overlay's when its file name (after the last `/` or `:`) is
`VitaJPOverlay_Kernel.skprx`, `VitaJPOverlay_Shell.suprx`, `VitaJPOverlay_OCR.suprx` or the removed
`VitaJPOverlay_Text.suprx`, in any letter case and on any device (`ur0:tai/`, `ur0:/tai/`, a manual
install's `ux0:tai/`): the memory card's file system ignores case, so taiHEN loads each spelling, and
two copies of the kernel plugin would install its hooks twice. The canonical lines are
`ur0:tai/VitaJPOverlay_Kernel.skprx` and `ur0:tai/VitaJPOverlay_Shell.suprx`.

**Install:**

- one canonical kernel line in a `*KERNEL` section that taiHEN reads for `KERNEL` (the first one there
  is kept; else it goes right after the first `*KERNEL` header, as today; else a section is added at
  the end), and one canonical Shell line in a `*main` section likewise;
- every other kernel or Shell line of ours (a second copy, another spelling or device, one in another
  section such as `*ALL`) is commented out with today's prefix `# Mainline OCR: `, so the user sees
  what it was;
- **no worker line.** The kernel plugin loads the worker into every game turned ON (#138, Agent A,
  t3-worker-any-game's PR, which merges first). Every worker line and every removed Text plugin line
  is removed, and with it the section header an older installer added for it (a `*TITLEID`, not `*!`,
  with nothing else left in it, and the blank line or note before it). A worker line in a section with
  other plugins (an `*ALL` one) goes alone;
- a section the editor adds (only `*KERNEL` or `*main`, when a file has none) is one note line, its
  header and the plugin, with no blank line: `# VitaJPOverlay (added by its installer)`, `*KERNEL`,
  `ur0:tai/VitaJPOverlay_Kernel.skprx`. The note is what lets Uninstall remove the section; #124's note
  (`# VitaJPOverlay: the OCR worker in this Adrenaline bubble's PSP game (Overlay app)`) is treated the
  same way;
- each line keeps its own line end; added lines use the file's (CRLF when it has one, else LF, or CR
  when it uses only CR). (`install-vita.sh` rewrote every line end to one style; now neither does.)
- a result whose length is a multiple of 4,096 bytes gets one more line end (taiHEN's buffer has no
  room for the C string's end then; `core/taicfg.c` already does this for an appended section);
- a UTF-8 byte-order mark stays where it is. taiHEN reads it as part of the first line, so a first
  line `\xEF\xBB\xBF*KERNEL` is no section to taiHEN, and none to the editor either; when a mark hides
  a section header the plan says so (the plugins under it do not load today).

Installing twice gives the same bytes as installing once.

**Uninstall** removes every line of ours that is not commented out (in any spelling), every note
line and the section header after it when nothing else is left in that section. Lines Install
commented out stay commented. So uninstalling after an install of a file that had none of our lines
gives back the original file, except a line end added at the end of a file that had none and the
4 KiB pad.

**Refused** (nothing is written; the message says what to do): a file over 64 KiB, a NUL byte, text
that is not UTF-8, a line of 256 bytes or more, no section or path line at all (taiHEN loads nothing
from it: restore a backup or HENkaku's default first), and any result the check below rejects.

**The check.** The editor simulates `taihen_config_parse` on the old and the new text for `KERNEL`,
`main`, every section name in the file and one title named in none (what `*ALL` gives any other
game). The new text must load the kernel plugin exactly once for `KERNEL` and never for another title,
the Shell exactly once for `main` and never for another title, the worker for no title, and every
other plugin for exactly the titles and in the order it loaded before (none of ours after
Uninstall). A `*!` section that would stop taiHEN before an added line (a `*!ALL` before the end of a
file that has no `*main`, say) fails it: refused, naming the line.

## Which games get the worker

None through config.txt. The owner (issue #138, Agent A, 2026-10-16): "Remove the need for a taiHEN
line for each title before the OCR worker can run ... Load the worker from the kernel plugin when a
process starts and its title is enabled in `games.ini` ... Do not use `*ALL`. The installer should
stop writing worker lines for each title and remove the ones older installs left." That is
t3-worker-any-game's PR (kernel, Shell, worker; it merges before this one): the kernel loads
`ur0:tai/VitaJPOverlay_OCR.suprx` (or `ux0:tai/`'s when ur0's is missing) into every process whose
title is ON, never SceShell or a system app but Adrenaline's; for one release it also tolerates a
worker line left in a configuration (the worker refuses a second copy of itself in a process).

So this installer, and `install-vita.sh` with it, in **worker kernel** mode:

- installs the worker file in `ur0:tai/` as before, although no line names it;
- writes no worker line, and removes the ones older installs left (above): a comment and an absent
  line are the same to taiHEN (`lexer.c:146-151`), and a header is deleted only when it is not `*!`
  (the halt is set by `*!` alone and acted on at the next header, `parser.c:237-264`), no path or
  comment line is left under it, and it is a title's section that held our worker or carries our note
  (an empty `*ALL`, `*KERNEL` or `*main` that is not marked stays);
- needs no game list: `install-vita.sh --title` is accepted and ignored with a note, and the patcher's
  refusal of an install with no game (`NO_TITLES`) goes;
- the Overlay app's Games page no longer edits config.txt: turning a PSP bubble or Adrenaline ON
  registers nothing (#124's `vjo_psp_register`, its dim "not registered" PSP tag and the status line
  about it go). Turning any game ON or OFF takes effect at its next start, as the Shell's input
  already does.

The mode is gated twice, so that the loader can be reverted or fixed without an editor change and no
user loses working lines:

- at build time by `VJO_KERNEL_LOADS_WORKER` in `include/vjo_api.h`, which t3-worker-any-game's PR
  defines: this PR's code fails to build without it (`#error`), which enforces the merge order;
- at run time by the payload manifest's `worker kernel` or `worker lines` line, which
  `package_release.py` writes from that define (the zip's manifest gets the same key, which
  `install-vita.sh` passes to the patcher).

In **worker lines** mode (the define 0) both installers keep today's per-title rules: every game
registered before keeps its worker line, the worker goes right after the first header of each named
game's section (`install-vita.sh --title`) or of each PSP row turned ON (#124's Games page), never
under `*ALL`, and misplaced worker lines are commented out. The in-app Install then adds no game of
its own (a first install in that mode needs `install-vita.sh --title`, as today). The fixtures run
both modes.

## Versions (#122)

- After Install the record is the app's stamp, so the Shell's check passes.
- Installing a new VPK gives a new app beside the old plugins: at start the app finds the record
  is another stamp, opens on Install with **Update the overlay to v0.8.1**, and lists the plan.
  Installing an older VPK is the same, with "this app's overlay is older than the installed one".
- The Shell's in-game notice becomes one neutral line, since the Shell cannot tell which side is
  newer when both have one API: `Overlay app and plugins are different builds: open the Overlay app.`
  The app says which way it is and what to do. It replaces both of today's notices
  (`VJO_APP_NOTICE`, `VJO_APP_NOTICE_NEWER` in `core/app_stamp.h`) with one shorter string, so
  SceShell's pages do not grow (AGENTS.md rule 1). Nothing else of the Shell, the kernel or the worker
  changes here (the "worker unavailable" message is t3-worker-any-game's), and the API stays 22.
- An app without a payload (the zip's) keeps today's banner texts, which name `install-vita.sh`.

## Uninstall

Uninstall shows its plan (the lines it removes from each configuration and the dated backup it
makes), then removes the lines, reads each configuration back and says "Restart the Vita: the overlay
no longer loads". It keeps every backup, the plugin files, the models, the dictionary and the user's
data (`config.ini`, `games.ini`, regions, the Anki queue and exports), as `install_ftp.sh --uninstall`
does, and says where they are for whoever wants the space back. The app itself is deleted from
LiveArea as any app.

## Recovery

The app's pages and the README say the same:

- A boot that does not complete makes the next one a safe boot (no hooks, no overlay; `status.txt`
  says why), as today (`docs/boot-safety.md`).
- If HENkaku does not start (no VitaShell: a `config.txt` missing), hold L while turning the Vita on:
  taiHEN then starts without reading any configuration (`taihen.c:348`).
- If the Vita does not start: hold L while turning it on (taiHEN then loads no plugin), open VitaShell,
  and in `ur0:tai` (and `ux0:tai`) put `#` before the two `VitaJPOverlay` lines of `config.txt`. Then
  restart normally. (The worker needs no line: without the kernel plugin nothing loads it.) This
  works after any install; copying a `config.txt.vjo-<date>.bak` back recovers only when it is one the
  plan named as from before the overlay, since every version's lines name the same files.
- If one game does not start or misbehaves: turn it OFF on the Overlay app's Games page; the kernel
  then does not load the worker into it. taiHEN's hold-L at a game's start (`hen.c:368-373`) skips
  only taiHEN's own plugins, not a worker the kernel loads (unless t3-worker-any-game's loader honours
  it too, which the README will then say).
- Never more than one attempt: the app does nothing by itself after a restart.

## The release process

- `tools/package_release.py --vpk OUT.vpk` writes the VPK next to the zip: the base zip's
  `JPOverlaySettings.vpk` with `payload/` (the files the zip installs, from the same bytes) and
  `payload/manifest.txt`. It checks that its eboot and stamp are the base's, that the payload's hashes
  are the zip manifest's, and VitaShell's scan (no mount import); and reports the VPK's size.
- `tools/package_variants.py` writes both VPKs and both zips and `SHA256SUMS`, normal first.
- CI keeps building the base zips (the full VPKs need the models, which are added at packaging, as
  for the zips today). `docs/release-variants.md`'s recipe publishes all five assets in one release,
  the normal VPK first.
- A test build ships both VPKs too; its issue's step 1 becomes "install the VPK in VitaShell, open
  Overlay, Install, restart".
- README's Install section becomes the three steps; `install-vita.sh` moves under "Advanced:
  install from a computer".

## Tests

- The editor: the fixtures (HENkaku's stock `ur0` and `ux0` files from `bootstrap.c`; Adrenaline's
  kernel plugin (`ux0:app/PSPEMUCFW/sce_module/adrenaline_kernel.skprx`, Adrenaline's readme) and an
  older install's `*PSPEMUCFW` worker section; repeated sections; CRLF; CR; a BOM before a comment and
  before `*KERNEL`; old lines: the Text plugin, an `*ALL` worker, per-title worker sections from
  `install-vita.sh` and #124, a commented-out line, a duplicate, a `ux0:tai/` and a case-variant
  kernel line; `*!` sections that do and do not block; the 4 KiB pad; refusals: NUL, not UTF-8, a
  256-byte line, comments only, empty, over 64 KiB), each through install, install again and
  uninstall, in C (ctest) and Python (pytest) against the same expected files; the simulation against
  hand-worked cases of `taihen_config_parse`.
- The install engine: the app's real code on a temporary memory card (real files, as the Games page
  tests), with `sceIoRename` refusing an existing destination as the Vita does: a fresh install (ur0
  only, both), update, repair, uninstall, the dictionary present/absent/different, failures injected
  at every open, write, sync, rename and read-back (the card is then as the plan says: old or new,
  never half), a stop after every step followed by a fresh start, and the plan's text.
- Packaging: the VPK's contents, manifest, hashes, eboot and stamp; VitaShell's scan; both variants.
- Preflight: a new row for these suites.

Vita3K is not evidence for any of it (it has no taiHEN). The test build's console run is.

## Unmeasured until a console run

- VitaShell's install time for the 53 MB VPK and the app's Install time (about 155 MB read and
  hashed, at most 256 KiB a frame); the work runs on CPU3 (the app is a system application), whose
  cost is unmeasured (AGENTS.md rule 8).
- That `sceIoRename` moves a file out of `ux0:app/VJOSET001` while the app runs (unsafe homebrew have
  the access; VitaShell moves files this way).
- What the promoter does with the files of a previous version on an update (replaced or kept: both
  are handled).
- That the app's writes to `ur0:tai` work while a game is suspended (the #124 registration does the
  same; not yet run on a console either).
