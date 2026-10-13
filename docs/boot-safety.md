# Boot safety: a bad build must never need recovery again

Issue #59 ended in recovery mode: a build that crashed SceShell during boot crashed it again
at every boot, because the plugins were loaded the same way each time. The boot-safety guard
makes the second boot different. It is the one kernel-side change that every other boot-path
change depends on (AGENTS.md rule 7).

## How it works

The kernel plugin keeps one line in `ur0:data/VitaJPOverlay/boot.txt`:

```
vjoboot1 seq=12 mode=0 stage=4 inert=0 fault=0 failures=0 api=17 prev=0/4/0/0
```

| field | meaning |
|---|---|
| `seq` | boot number |
| `mode` | this boot: `0` normal, `1` safe |
| `stage` | the furthest point this boot reached: `1` kernel started, `2` the Shell plugin's `module_start` ran, `3` the Shell loaded its Paf plugin and started its threads, `4` ok: the Shell's control thread was still alive a minute later |
| `inert` | the Shell plugin unloaded itself on purpose (safe mode, missing RCO, kernel API mismatch) |
| `fault` | `1`: the kernel saw the registered SceShell process exit while our plugin was loaded in it |
| `failures` | normal boots in a row that did not complete |
| `prev` | the previous boot's mode/stage/inert/fault, for the status note |

A boot is **complete** when it reached stage `4` or was inert, with no fault.

1. `boot_init` runs first thing in the kernel's `module_start`, before any hook, thread,
   event flag or memory block exists. It reads the previous record, decides this boot's mode
   and writes this boot's record with `stage=1`. ur0 is the internal eMMC; taiHEN read its
   own `ur0:tai/config.txt` from it moments earlier, while ux0 can still be remapped
   (SD2Vita).
2. In a **normal** boot the kernel installs its hooks and the Shell plugin marks stages `2`
   and `3` through `vjoBootMark` as it reaches them; its control thread marks `4` after 60 s.
   The kernel believes an `ok` only after 55 s of its own uptime. Each mark is written back by
   the kernel worker thread from its own context, one write per change.
3. In a **safe** boot the kernel installs no hooks (display, input, process events), so it
   touches no game and no SceShell thread; its worker thread only writes the record and the
   kernel log. The Shell plugin's `module_start` asks `vjoBootInfo`, writes the note below to
   `ux0:data/VitaJPOverlay/status.txt`, marks itself inert and returns
   `SCE_KERNEL_START_NO_RESIDENT`: its pages are freed at once and SceShell keeps everything
   we would have held. The game worker (`*ALL`) does the same in every process. If no Shell
   plugin reports in at all, the kernel completes the safe boot by itself after 60 s of
   uptime.

The decision, in `core/boot_guard.c` (`vjo_boot_decide`):

| previous boot | next boot |
|---|---|
| no record, or an unreadable or empty one | normal, `failures=0` (a fresh install, or the installer's reset) |
| normal and complete | normal, `failures=0` |
| normal and incomplete (stopped before `ok`, or SceShell exited) | **safe**, `failures+1` |
| safe and complete, `failures < 2` | normal: one more attempt |
| safe and complete, `failures >= 2` | safe, until the record is reset |
| safe and incomplete (power cut within its minute) | safe, `failures` unchanged |

So a bad build costs at most two crashing boots, then every boot is a safe one. With #59's
sequence: the first boot completes, the game launch kills SceShell (fault), the next boot is
safe. Without the fault event: the first boot completes, the second crashes during boot and
stops at stage `2` or `3`, the third is safe.

### The note

A safe boot writes to `status.txt` (fetch it with `tools/install_ftp.sh --status VITA_IP`):

```
BOOT GUARD: SAFE MODE, boot 3. No hooks, no overlay, no OCR this boot.
boot guard: boot 2 (normal) reached stage shell and never reached ok, 1 failed normal boot in a row.
boot guard: reboot to try the overlay once more; a second failure keeps safe mode.
boot guard: send status.txt and kernel.txt (install_ftp.sh --status). No boot at all: hold L at power-on, then install_ftp.sh --uninstall.
```

### Resetting

- `tools/install_ftp.sh` (and the release's `install-vita.sh`) uploads an empty `boot.txt`
  with every install and uninstall: a new build gets a fresh normal attempt, and safe mode
  never outlives the build that caused it.
- By hand: delete `ur0:data/VitaJPOverlay/boot.txt` in VitaShell, or run the installer again.

### What is unchanged

- taiHEN's **hold L while powering on** still skips every plugin. It is the last resort when
  even the kernel plugin's `module_start` cannot run; the guard does not replace it.
- `tools/install_ftp.sh --uninstall VITA_IP` after a hold-L boot still restores the backed-up
  taiHEN configuration.

### Failing closed at boot

- Kernel: if any of the three hook sets fails to install, all of them are released and the
  kernel runs without hooks (previously the installed part stayed). A failed event flag,
  mutex or worker thread returns `SCE_KERNEL_START_FAILED`, which unloads the module.
- Shell: a failed `vjo_worker_start` leaves the plugin loaded without its threads; that boot
  never reaches `ok`, so the next one is safe. A missing RCO or kernel plugin, or a failed
  bootstrap hook, marks the Shell inert and unloads it: a configuration problem, not a
  failure.
- Every `boot.txt` read or write failure is logged and the boot continues in normal mode: the
  guard narrows what a bad build can do, it is not allowed to become a new way to lose the
  overlay. A record written by a newer build is read for the fields this build knows.

## Tested on the host

`host/tests/test_boot_guard.c` (`ctest -R boot_guard`) runs the real `kernel/boot.c` with the
ur0 file and the clock replaced, boot after simulated boot: the first boot's record, healthy
boots, a crash before `ok` followed by the safe boot and the single retry, two failures in a
row sticking to safe mode until the record is emptied or deleted, a safe boot completing by
uptime with no Shell plugin, the SceShell-exit fault after `ok`, early or backwards marks
refused, and every I/O failure failing open. The record's parser is tested against CRLF,
truncation, unknown keys, oversized numbers and garbage. `test_shell_worker.c` covers the
Shell's stable-minute mark and its bounded retries; `test_game_vocr.c` covers the game
worker unloading itself in a safe boot.

## What still needs the console

Marked as unverified until the self-test (plan item 5) reports them:

1. that `ur0:data/VitaJPOverlay/boot.txt` can be written from the kernel plugin's
   `module_start`. taiHEN reads `ur0:tai/config.txt` moments earlier, and PSVshell's kernel
   plugin creates `ur0:data/PSVshell/profiles/` and reads profiles in its own `module_start`
   ([audit note](plugin-audit/notes/PSVshell.md)), so writing is expected to work; it is not
   yet observed on this console;
2. that `taiGetModuleInfoForKernel` finds `VitaJPOverlay_Shell` while its own `module_start`
   runs (taiHEN lists modules in every state; if not, the Shell's marks at stages 2 and 3 are
   refused and the `ok` at 60 s, after registration, still counts);
3. that SceShell's exit reaches the process-event hook before the console goes down (without
   it the guard still trips one boot later).

Memory cost, measured from the packaged modules: the Shell module's pages in SceShell's
partition are unchanged (`405,504 B`, the same 99 pages as `e759187`; the guard's code took
the slack of the last text page); the kernel module grew by one page (`28,672 → 32,768 B` of
text) in `ScePhyMemPartKD`; the game worker's pages are unchanged (`45,056 B`).
