# The memory ledger

Every byte our three plugins take from a Vita memory pool is accounted for in one place, checked
in CI, and printed at build time and at run time. The pools themselves, their sizes and who else
uses them are in [docs/vita-memory.md](vita-memory.md); why this exists is
[issue #59's root cause](issue-59-root-cause.md).

## The parts

| Part | Where | What it does |
|---|---|---|
| budget table | [`include/vjo_ledger_budget.h`](../include/vjo_ledger_budget.h) | one `#define` per item (its worst case in bytes) and per pool (its limit), with the source of each number; plain integer arithmetic so the C code and the checker read the same values |
| counters | [`core/ledger.c`](../core/ledger.c) | current and peak bytes per item and per pool, atomics only, no libc; one `VjoLedger` per process |
| wrappers | `shell/memory.cpp`, `shell/ledger.c`, `kernel/buffers.c`, `kernel/main.c`, `game_ocr/main.c` | the only places that call an allocator or create a thread; each names its item and counts the bytes in and out |
| CI check | [`tools/check_memory_ledger.py`](../tools/check_memory_ledger.py) | fails the build on an allocation outside the wrappers, a worst case over its limit, or a packaged module with more pages than declared; prints the table |
| run time | `vjo_ledger_shell_log` (Shell), `kledger_log` (kernel) | the pools and items, current and peak, in `status.txt` and `kernel.txt` |

## The table

Printed by `tools/check_memory_ledger.py` (CI runs it in the host job against the sources and in
the Vita job against the packaged modules; `ctest -R memory_ledger` runs it locally):

```
  pool                              permanent   worst case        limit     headroom
  shell pages                         217,088      286,720    1,048,576      761,856
  shell paf                             8,192      225,280      985,088      759,808
  kernel                              131,072    2,220,032    2,220,032            0
  game                                126,976    8,720,384    8,720,384            0
  packaged modules (loaded pages vs declared budget)
      VitaJPOverlay_Kernel      114,688 B of    114,688 B declared (36,640->36,864 + 77,768->77,824)
      VitaJPOverlay_Shell       176,128 B of    176,128 B declared (101,256->102,400 + 70,076->73,728)
      VitaJPOverlay_OCR          90,112 B of     90,112 B declared (71,072->73,728 + 14,208->16,384)
```

Every Shell configuration is the shipped one since Google Lens and the Hachidori relay were
removed: the overlay is offline, a job's lookup takes one 64 KiB arena, and there is no opt-in
network row (it was 707,840 B with Google Lens alone, 731,632 B with the relay's second connection
slot). Anki cards are text-only (128 KiB arena; `shell/anki.c` has no screenshot branch since
AnkiConnect's removal). The shipped default (local JMdict) has 759,808 B of headroom. The kernel
row keeps the full-frame capture's 2,088,960 B (`kernel/buffers.c`): since Google Lens's removal
no Shell code takes it (Anki takes no screenshot, OCR reads the screen in the game); it goes with
the capture's own removal.

*Permanent* is held from boot (or from the game's start) until exit; *worst case* adds the largest
transient set that can be live at once.

### Pool "shell pages": SceShell's partition (`ScePhyMemPartShell`, 50 MiB)

| item | bytes | what | source |
|---|---|---|---|
| Shell module pages | 176,128 | `VitaJPOverlay_Shell.suprx`: 102,400 text + 73,728 data/bss, permanent; 262,144 B packaged (188,416 + 73,728, of 266,240 B declared) from the template's removal (#101, below) until Google Lens's, 266,240 B (192,512 + 73,728) before it, 282,624 B (208,896 + 73,728) until AnkiConnect's removal, 286,720 B (212,992 + 73,728) until the Hachidori relay's and 294,912 B (217,088 + 77,824; 299,008 B declared) until the online dictionaries' (both below) (405,504 B at `e759187`: the cold files are built with `-Os`, and the overlay's 96 KiB text arena left `.bss` for the PAF heap). #90's fixed Kiku note took 3,488 B off data/bss (the Anki field mapping's 1,152 B from each of the three `VjoConfig` copies; the LAN search moved to the Overlay app); with #86's region selector and #84 (its `--gc-sections` link, and 28 B of new `VjoConfig` fields) the module is 214,488 B of text and 77,876 B of data/bss; #87's row navigation makes it 214,480 B of text and 77,908 B of data/bss, the same pages, so 2,608 B of text and 4,012 B of data/bss are free in them; #88's normalised text and its log make it 214,816 B of text (main `94d608e`); #92's quiet refresh makes it 214,672 B of text and 77,956 B of data/bss (2,416 and 3,964 B free); #85's two engines and #89's A/B log (the LM tier, the cascade's speed line) make it 215,216 B of text and 77,980 B of data/bss [`bcd3cf0`]; the finder refinement's step and key make it 215,256 B and 77,984 B [`fec85af`], still the same pages (1,832 and 3,936 B free); API 21's model sizes (#108) take `VjoConfig`'s two 64-byte model names from its three copies, so data/bss is 77,624 B in 77,824 B of pages and the module **294,912 B** of the 299,008 B declared, text 216,448 B with #102 [the build log's module row, main `0023e8a` and this change]; API 22 (#93) makes it 216,856 B of text and 77,648 B of data/bss, the same pages [`ba5f69a`]; without jpdb.io and jiten.moe (main `ba5f69a` and this change) it is 210,184 B of text and 71,204 B of data/bss, one text page and one data page fewer (2,808 and 2,524 B free), and the declared budget is lowered to the packaged pages, **286,720 B**; without the Hachidori relay client (main `a5d79f2` and this change) it is 206,192 B of text and 71,020 B of data/bss, one text page fewer again (2,704 and 2,708 B free), **282,624 B**; without AnkiConnect (the Shell's sender, its requests, the screenshot branch, the Kiku note and the audio URL; the Overlay app exports the cards; main `5e68ecd`, 206,664 B and 71,076 B since #116 and #111, and this change) it is 188,472 B of text and 70,104 B of data/bss, four text pages fewer (4,040 and 3,624 B free), **266,240 B**; #123's key filter makes it 189,104 B of text, the same pages (3,408 and 3,624 B free) [the build log's module row, main `c641bdf`]; #122's app check, without the config template, makes it 186,416 B of text and 70,112 B of data/bss, one text page fewer (2,000 and 3,616 B free), 262,144 B [main `ac905b1`]; #127's frequency ranks make it 187,552 B of text and 70,128 B of data/bss, the same pages (864 and 3,600 B free) [main `9f92bc6`]; #124's identification of Adrenaline's PSP games (`shell/game.c`: AppMgr's app name and one description read per launch; `core/games.c`'s shared PSP row) makes it 188,000 B of text and 70,132 B of data/bss, the same 262,144 B (416 and 3,596 B free) [the build log's module rows, main `9f92bc6` and #124]; without Google Lens (with it the Shell's capture and JPEG encoder, the worker's network pipeline, and the TLS, roots and HTTP client the link no longer keeps; main `6020c0b` and this change) it is 100,792 B of text and 70,060 B of data/bss, 21 text pages fewer (1,608 and 3,668 B free), and the declared budget is lowered to the packaged pages, **176,128 B**; #132's VjoNet stack paint and check make it 101,256 B of text and 70,076 B of data/bss, the same pages (1,144 and 3,652 B free) | packaged SELF segments [measured] |
| OCR worker pages in SceShell | 0 | `VitaJPOverlay_OCR.suprx` is registered per game (one `*TITLEID` section each, the installer's `--title`), never under `*ALL`, so taiHEN does not load it into SceShell. Were it loaded there (an older install's `*ALL` line, a hand edit), its `module_start` returns `NO_RESIDENT`, which unloads it at once (it held 45,056 B there before PR #68) | `tools/patch_tai_config.py`; `game_ocr/main.c`; [SDK: Kernel-Overview, `module_start`] |
| VjoNet stack+TLS | 20,480 | 16,384 + 4,096, permanent (65,536 + 4,096 until #132: nothing on the thread makes a TLS connection since Google Lens's removal) | `shell/worker.c`; TLS per thread [SDK: Kernel-Reference, `sceKernelCreateThread`] |
| VjoControl stack+TLS | 20,480 | 16,384 + 4,096, permanent | same |
| VjoAnki stack+TLS | 69,632 | only while an Anki job runs: with `anki_mining = on`, a count of the queue on every overlay open, or a save (Enter); a refusal is logged with its rc, these bytes and SceShell's free USER (`shell/ledger.c`) | `shell/anki.c` |
| VjoShellMem USER_RW | 0 | the host tests' result memory; the Vita build uses PAF (`VJO_PAF_ALLOC`) | `shell/worker.c` |
| **limit** | **1,048,576** | half of the "genuinely under 2 MiB" a Shell plugin may have at most [expert]; SceShell had 28,672 and 98,304 B free beside a game [measured], so the limit is a ceiling on our footprint, not a promise that it fits: module pages may only shrink, and the checker fails when a packaged module has more pages than declared | AGENTS.md rule 1; [vita-memory.md 5.1](vita-memory.md) |

### Pool "shell paf": SceShell's PAF global heap (5,242,880 B, mapped once)

| item | bytes | what | source |
|---|---|---|---|
| Paf plugin+RCO | 8,192 | the RCO (6,368 B file, measured; 5,632 B before the region selector's cursor) and the Paf `Plugin` object | **[estimate]**; the self-test measures it |
| result arenas | 65,536 (the local dictionary's lookup; the network paths' 2 × 262,144 B arenas and connection pool went with Google Lens and the Hachidori relay) | taken after a job's OCR, freed before the next job's | `shell/worker.c` `LOCAL_RESULT_ARENA_SIZE`; asserted equal in `test_shell_worker.c` |
| Anki job arena | 131,072 | per job: saving a text-only card to the queue; the Shell has no screenshot branch (removed with AnkiConnect: its 393,216 B arena and the kernel's 2,088,960 B full-frame capture), so the kernel capture is never taken for Anki; a save with 8 dictionaries' entries peaks at 102,680 B [measured, host, `e53d9fb`] | `shell/anki.c` `ANKI_TEXT_MEM`; checked against the budget at compile time and in `test_shell_anki.c` |
| overlay text arena | 20,480 | the header and body texts and their spans (17,952 B needed, `test_ledger.c`), only while the overlay page is open; a refusal keeps the page closed | `shell/overlay.cpp` |
| **limit** | **985,088** | the measured free heap, 2,557,952 B (2,498 KiB beside CLANNAD, 2026-10-10), minus the 1.5 MiB reserve the runtime guard keeps for firmware; the reserve is the headroom | `docs/meiki-ocr.md` on `experiment/meiki-ocr` [measured]; `shell/memory.cpp` |

Allocations here take no new pages from the partition; a failure is a `NULL`, never `0x80024302`
(vita-memory.md 3.2). Later sessions saw 355-441 KiB and 190 KiB free [measured], which would
refuse everything above; the code survives a refusal (host tests), so this limit is the best
measured case, not a guarantee. The worst case of the shipped default (one 64 KiB arena, the
overlay open, an Anki card being saved) is 225,280 B, the only configuration since the network
paths' removal (the opt-in network row was 707,840 B with Google Lens alone; with a 384 KiB
picture card and the relay's second connection slot it was 993,776 B, 8,688 B over the limit,
which the checker reported as a warning).

The word lookup's ranking (`core/local_dict.c`) takes no allocation and no item of its own: its
common-readings table (70 pairs, 924 B of `const` strings) is in the Shell module's text pages, which
stay at 299,008 B because `shell/worker.c` is now built with `-Os` (the build log's module row:
216,904 B of text, 81,232 B of data and `.bss` [measured]). Its state is on the VjoNet thread's
64 KiB stack: the cursor lookup's frame grows from 576 to 1,328 B (two 8-entry match arrays and the
span ends) and its deepest call chain from 1,008 to 2,464 B [measured: `arm-vita-eabi-gcc -O2
-fstack-usage` of `core/local_dict.c`, main and this change]. The lookup's 48 KiB scratch stays
inside the 64 KiB result arena (compile-time check `WorkspaceFits`). The lookup log line uses a
256-byte stack line, as every `vjo_log` call does.

Drawing the region with the stick (#81, `core/region_select.c`) takes no allocation either. Its
code (1,108 B of `region_select.c`, and `shell/overlay.cpp` at 8,822 B where the old file built
for size is 8,237 B) is paid by building `shell/overlay.cpp` at `-Os` (11,608 B at `-O2`); it
runs only while the overlay is open. The Shell's text goes from 217,048 to 215,312 B and its
data and `.bss` from 81,232 to 81,336 B (the selector's state, 104 B), so the module stays at
299,008 B [measured: the build log's module row, main `4a7ae4d` and this change]. The RCO grows
from 5,632 to 6,368 B for the cursor's five planes and one style; the 8,192 B Paf plugin
estimate is kept, which leaves 1,824 B (was 2,560 B) for the Paf `Plugin` object, a size still
unmeasured. On the Paf main thread's stack the region hint's frame is 248 B with its 192-byte
line, under `render`'s 312 B, so the overlay's deepest frame is unchanged [measured:
`arm-vita-eabi-g++ -Os -fstack-usage`].

Writing a job's log lines together (`shell/log_vita.c` `vjo_log_batch_begin`/`end`: the result's
lines, the recognized text's, the ledger table's) takes no allocation and no item: each batch's
buffer is on the stack of the thread that logs it, for the burst only. On VjoNet's stack (64 KiB
then, sized for BearSSL's TLS, which `vocr_run_one` never runs; 16 KiB since #132) `vocr_run_one`'s frame grows from 832
to 4,992 B (its 4 KiB buffer) and `log_text_lines`' is 1,840 B (1.5 KiB); the deepest chain of the
Shell's own code on that thread goes from 1,544 to 5,672 B. On VjoControl's 16 KiB stack
`vjo_ledger_shell_log`'s frame grows from 32 to 1,128 B (a 1 KiB buffer), its chain from 424 to
1,592 B; the thread's deepest chain of Shell code stays the config reload's, 3,128 B before and
3,200 B after (the flush adds 72 B under `vjo_log`) [measured: `arm-vita-eabi-gcc -fstack-usage
-fcallgraph-info=su` with each file's own flags (`-Os` for these), main `e90a43d` and `72292e1`;
chains of the Shell's own files, the SDK's calls counted as 0; #100 first quoted the same
measurement at `-O2`]. The logger's code grows the Shell's text from
215,256 to 215,952 B and its data from 77,984 to 77,988 B (the open batch's pointer): the module
stays at 299,008 B [measured: the build log's module row].

The overlay's arrow keys over the header's displayed rows (#81, `core/text_rows.c`, 1,462 B of
text at `-Os`) take no allocation either: a key press walks the overlay's existing 4 KiB text copy
(`s_cursor_text`) on the paf thread, its deepest call chain 384 B of stack plus paf's own
`GetCharInfo` [measured: `arm-vita-eabi-gcc -Os -fstack-usage`]. Their state is a static 88-byte
`VjoRowNav` (Right's 16 positions, Up/Down's column, three counters) in place of the 68-byte
`VjoWordNavigation`, the L/R tap's state, the header's point size and the slowest key's time:
`.bss` grows by 32 B, within the module's data pages. The text pages stay at 299,008 B because
`shell/strip.cpp` (4,107 to 2,974 B) and `core/arena.c` (912 to 518 B; `vjo_arena_alloc` is the
same instructions) are built with `-Os` too. They pay for `text_rows.c` and for the key handling's
156 B in `shell/overlay.cpp` (8,822 to 8,978 B at `-Os`): the build log's module row goes from
214,488 to 214,480 B of text and from 77,876 to 77,908 B of data and `.bss` [measured: main
`dca0ba4` and this change].

The open overlay's quiet refresh (#81 request 4, `docs/touch-lookup.md`) takes no allocation and no
item of its own either. A refresh is a game OCR job that keeps the overlay's text and its entry: the
one 64 KiB result arena (`VJO_SL_PAF_RESULTS`) stays held during it instead of being freed before
the job, its text stays in the worker's static result until the control thread compares it in its
24 KiB scratch, and a new text is copied into the existing 4 KiB `local_text`. The item's worst case
is unchanged (the arena was already held with the overlay open). Its code (and the SCREEN_CHANGED
pacing, the self-test's text gate, the job submission's BUSY retry) adds 1,116 B of Shell text
(object text, Vita build, `-Os`: `shell/worker.c` +1,022, `shell/selftest.c` +129,
`shell/game_ocr_client.c` +66, `core/client.c` −101), paid for by building six more cold files with
`-Os` (`vita/CMakeLists.txt`: −1,204 B). The module stays at 299,008 B: the build log's module row
goes from 214,816 to 214,672 B of text and from 77,908 to 77,956 B of data and `.bss` (+48 B of
state) [measured: `.refs/ci.sh … vita` on main 94d608e and on this change].

Removing jpdb.io and jiten.moe, the online dictionaries with an API key, takes the packaged module
from 294,912 to 286,720 B, two pages (main `ba5f69a`, which declared 299,008 B), and lowers
`VJO_BUDGET_SHELL_MODULE_PAGES` from 299,008 to 286,720 B, so the pool's permanent pages go from
389,120 to 376,832 B and its worst case from 458,752 to 446,464 B. The packaged text segment loses
6,672 B (216,856 to 210,184 B: `.text` 149,336 to 143,792 B, `.rodata` 62,936 to 61,896 B, the rest the
three import stubs only the strip's readings used, 48 B, and section alignment): the two
clients (`core/jiten.c`, `core/jpdb.c`, 3,203 B of object text before the link drops unused code),
their request, auth and error paths in `core/client.c` and `core/config.c`, and the subtitle strip's
furigana (`shell/strip.cpp`, `vjo_strip_rubies`, `vjo_entries_rubies`; only those two services gave
readings). `.bss` loses 6,444 B (77,496 to 71,052 B): `shell/worker.c` 3,340 B (`VjoView`'s 64 strip
readings and the API keys, 512 B, of the `cfg` and `job_cfg` copies of `VjoConfig`),
`shell/strip.cpp` 2,584 B (its copy of the readings and their 64 text widgets' pointers),
`shell/anki.c` 512 B (`next_cfg`'s keys) [measured: the build log's module rows and
`arm-vita-eabi-size` of the module and its objects, main `ba5f69a` and this change; the kernel and
OCR worker SELFs are byte-identical to main's]. The PAF items are unchanged: the network path's two 256 KiB arenas and two-slot
connection pool stay for Google Lens (its response is up to 256 KiB) and the Hachidori relay (the next
section removes it and its slot), and a local lookup still takes one 64 KiB arena. Every TLS root stays: none was used only by the two
services (`api.jiten.moe` and `jpdb.io` chain to ISRG Root X1, which HTTPS audio sources such as
`upload.wikimedia.org` use too; Google Lens chains to GTS Root R4 [measured: `openssl s_client`,
2026-10-15]).

Removing the Hachidori relay client, which looked words up in the Hachidori app on a computer on
the local network over plain HTTP (`dictionary = hachidori`, `hachidori_host`), gives SceShell back
one more page: the module goes from 286,720 to 282,624 B (`VJO_BUDGET_SHELL_MODULE_PAGES` lowered
with it), so the pool's permanent pages go from 376,832 to 372,736 B and its worst case from
446,464 to 442,368 B. The packaged text segment loses 3,992 B (210,184 to 206,192 B: `.text`
143,792 to 140,448 B, `.rodata` 61,896 to 61,248 B): the client (`core/hachidori.c`, 2,478 B of
object text and 196 B of `.rodata` before the link drops unused code), its branch, error text and
host check in `core/client.c` and `core/config.c` (`vjo_hachidori_endpoint`, the `hachidori_host`
setting, the template's lines about it), the JSON helper only it used (`vjo_json_append_str`,
`core/json.c` -110 B) and the worker's relay paths (`shell/worker.c` -316 B of object text).
`.bss` loses 184 B (71,052 to 70,868 B): the 64 B `hachidori_host` of the `cfg`, `job_cfg` and
`next_cfg` copies of `VjoConfig` and the cached lookup's dictionary ID [measured: the build log's
module rows and `arm-vita-eabi-size` of the module and its objects, main `a5d79f2` and this
change; the kernel and game worker modules are byte-identical]. Its PAF item shrinks too: the
network path, now Google Lens alone, keeps one connection slot instead of two, so its result memory
goes from 571,888 to 548,096 B (`VJO_BUDGET_PAF_RESULTS_NET`; the host's `sizeof(VjoNetSlot)`,
23,800 B, rounded to 16 B; the Vita compiler's is 23,312 B) and the opt-in row's worst case from
731,632 to 707,840 B. The local path (one 64 KiB arena) is unchanged. No TLS root was the relay's:
it spoke plain HTTP.

The first scan's fixes (docs/first-ocr.md: the self-test's own background job, its records written
a record at a time, the press's settle cap, the done line's exchange and log times) take no
allocation and no item. A self-test record's group is a 2 KiB buffer on the stack of the thread
that writes it, and no thread's deepest chain changes: on VjoNet `vjo_selftest_stage_end`'s frame
is 2,432 B, called by `run_selftest_stages` after `vocr_run_one` (5,000 B) has returned; on
VjoControl the game record's chain is 3,324 B (`ctl_main` 136, `activate_game` 160,
`vjo_selftest_game` 2,120, `sample` 408, `report` 276, the group's flush and the logger's append
below it 224) of its 16 KiB, under the same thread's
config load (136 + 160 + `apply_config` 1,720 + `vjo_config_load` 128 + `vjo_config_parse`
1,760 = 3,904 B); `vjo_selftest_init`'s 3,148 B chain runs on the boot's thread after the same
config load [measured: `arm-vita-eabi-gcc -Os -fstack-usage -fcallgraph-info=su` with the build's
flags, the SDK's calls counted as 0]. The Shell's text goes from 206,192 to
206,240 B and its data and `.bss` from 71,020 to 71,072 B (the last text job's capture, the idle
time, the log's append counters), the same 282,624 B of pages: object text `shell/worker.c` +834,
`shell/selftest.c` +280, `shell/log_vita.c` +88, `core/config.c` +175 (the new key and its range),
paid for by building `core/jpegsw.c` (Lens's encoder, −888) and `core/utf.c` (−396; the Overlay
app's copy −392, not a plugin) with `-Os`. The worker's text goes from 70,124 to 70,812 B
(`game_ocr/main.c` +252, `core/vocr_ocr.c` +440: the warm-up and its steps, the NO_GAME grace, the
TEAR step's retry time) and its data and `.bss` from 14,200 to 14,208 B, the same 90,112 B of pages
[measured: the build log's module rows, main `f85b5d9` and this change, vita-vn-ocr `d214a05`].

The Overlay app check (#101, `core/app_stamp.h`) takes no allocation and no item: at each enabled
game's start, after `vjoSetGameActive`, the control thread reads two one-line stamp files into two
64-byte stack buffers (`shell/worker.c` `check_app`, `shell/files.c` `vjo_file_line`) and stats one file,
and the notice is a pointer to a constant string in the view model. `check_app` is inlined into
`activate_game`, whose frame grows from 160 to 240 B; its deepest new path, through `vjo_log`, is about
680 B, under the existing `apply_config` path (`apply_config` alone 1,400 B) [measured:
`arm-vita-eabi-gcc -Os -fstack-usage` with the Vita build's flags, main `e53d9fb` and this change; #123
and #133 changed neither `shell/worker.c` nor a header it includes but comments]. The
config warnings are compared with the previous load's in the two `VjoConfig` copies `apply_config`
already has. SceShell no longer writes a default `config.ini` when there is none (the installer, the
package and the Overlay app do), so the commented template (3,556 B with its NUL,
`.rodata.vjo_config_default_text.str1.1` of `core/config.c` under the Vita flags; `docs/config.example.ini`
is the same 3,555 B), its 8 B accessor and 60 B of `vjo_config_load` (180 to 120 B) leave the Shell, which
pays for the check: the build log's module row goes from 189,104 to 186,416 B of text and from 70,104 to
70,112 B of data and `.bss`, and the module from 266,240 to 262,144 B, one page fewer [measured: the Vita
builds of main `0b95c9d` (the same rows as on `c641bdf`) and of this change; the template string is in main's Shell ELF and not in this
change's]. `VJO_BUDGET_SHELL_MODULE_PAGES` stays 266,240 B, the ceiling, which concurrent changes to the
Shell share. The check's file I/O is not measured on a console: two open/read/close and one getstat on
`ux0:` at each enabled game start, and one log append when the answer changes, about 30-90 ms on
VjoControl after the game is active (an estimate from [docs/performance.md](performance.md)'s console
figures: 15.3 ms per open/seek/read/close, 9.4-47 ms per append); an L+R in the same control tick waits
that long (est.).

The bundled dictionary's frequency ranks (#127: the format 3 reader, the rank badge, its
`frequency_badge` switch and the ranked files' usual spellings) add no allocation, thread or table
held in memory: the rank is read with each payload the lookup already reads, and the tables are
constant strings in the module. The build log's module row goes from 186,416 to 187,552 B of text and
from 70,112 to 70,128 B of data and `.bss` (`VjoView.frequency_badge`), the same 262,144 B of pages
(864 and 3,600 B free) [measured: the Vita builds of main `ac905b1` and of this change].

Removing Google Lens (the overlay is offline; the last build with it is the tag
`archive/online-last`, branch `archive/online`) gives SceShell back 21 more pages: the module goes
from 262,144 B (266,240 B declared, the ceiling above) to 176,128 B, and
`VJO_BUDGET_SHELL_MODULE_PAGES` is lowered from 266,240 B to it, so the pool's permanent pages go
from 356,352 to 266,240 B and its worst case from 425,984 to 335,872 B. The packaged text segment
loses 87,208 B (188,000 to 100,792 B: `.text` 128,904 to 62,176 B, `.rodata` 54,544 to 34,120 B)
and data and `.bss` lose 72 B (70,132 to 70,060 B) [measured: the build log's module rows and
`arm-vita-eabi-size`, main `6020c0b` and this change; the kernel and the RCO are byte-identical, the
game worker has the same pages]. Where the 87,208 B went, by object: each object's input sections
that the link keeps in the code segment (`.text`, `.rodata`, `.ARM.exidx`, the import stubs), from
linker maps of the two builds (the same link commands with `-Wl,-Map`; the ELFs are byte-identical
to the builds'). An object's row has its own sections only: the string literals of every object
are merged into two pools (`.rodata.str1.1`, `.rodata.str1.4`) that the map lists under the first
object to have one, `shell/boot.c` and `buttons_vita.c`, so the literals that went with an object
are in those two rows. Kept input sections, the maps' fill and the gaps between output sections
(`readelf -S`) make the ELF's code segment [measured]:

| objects | main | this change | Δ |
|---|---:|---:|---:|
| BearSSL: its TLS engine, X.509, ciphers, curves, MD5, SHA-1 and SHA-384/512, HMAC, the DRBG (all of `third_party/bearssl`) | 66,553 | 1,496 | −65,057 |
| `core/tls_roots.c`: the 15 trust anchors | 6,324 | 0 | −6,324 |
| `core/http.c`, `core/net.c` | 3,996 | 0 | −3,996 |
| `core/jpegsw.c`, `shell/capture.c` | 3,350 | 0 | −3,350 |
| `core/lens.c`, `core/pb.c` | 2,138 | 0 | −2,138 |
| `core/tls.c` | 472 | 0 | −472 |
| the network pipeline: `shell/worker.c`, `core/local_dict.c`, `core/entries.c`, `core/client.c`, `core/textfilter.c` | 38,879 | 35,071 | −3,808 |
| the merged `.rodata.str1.1` pool, which the map lists under `shell/boot.c` (with boot.c's own 360 B of code, unchanged): the literals of Lens, HTTP, the connections, the JPEG encoder, the capture, the engine choice and the worker's network jobs | 17,223 | 15,662 | −1,561 |
| the merged `.rodata.str1.4` pool, listed under `buttons_vita.c` (with its own 120 B, unchanged): BearSSL's TLS PRF labels (`client finished`, `master secret`, …) and others | 6,741 | 6,615 | −126 |
| the rest: `anki.c` −60, `config.c` −44, the two capture syscall stubs −32, linker stubs −16, `selftest.c` −4 | | | −156 |
| kept input sections, total | 185,204 | 98,216 | −86,988 |
| alignment fill between them (the maps' `*fill*`) | 252 | 56 | −196 |
| gaps between output sections (`readelf -S`: `.text`'s end to the 16-aligned import stubs) | 8 | 0 | −8 |
| the ELF's code segment (`readelf -lW`) | 185,464 | 98,272 | −87,192 |
| the SCE module data the packaging adds (the packaged code segment less the ELF's; two capture syscall imports fewer) | 2,536 | 2,520 | −16 |
| the packaged code segment (the build log's module row) | 188,000 | 100,792 | −87,208 |

What is left of BearSSL is SHA-224/256 (`sha2small.c` and its two byte-order helpers, 1,496 B; its
`br_*` symbols 1,082 B), for the Anki queue's IDs. Its `br_*` symbols alone go from 35,526 to
1,082 B (`arm-vita-eabi-nm -S --defined-only`, 200 symbols to 10); the rest of its 65,057 B are
its static tables and helpers without the prefix. What went:

- the Lens client and its protobuf codec, the worker's capture → JPEG → Lens job and its
  connection warm-up, and the engine choice (`ocr_backend`) with its messages;
- the Shell's capture and JPEG encoder (1,392 B of `vjo_jpeg_encode` alone) and the capture lock a
  local OCR job held: AnkiConnect's removal had already taken the Anki screenshot, its other user;
- the worker's network pipeline: the two 256 KiB result arenas, the connection pool, the
  whole-sentence lookup only its jobs made (`vjo_overlay_lookup`, `vjo_local_lookup`,
  `vjo_entries_build`), the cached-sentence comparison;
- with Lens the Shell's last TLS connection, so the link (`--gc-sections`) no longer keeps
  BearSSL's TLS, the trust anchors or the HTTP client. The sources stay until PR-B
  (docs/shell-slim.md).

The PAF items lose the network row: `VJO_BUDGET_PAF_RESULTS_NET` (548,096 B with one slot) and
`VJO_WORST_SHELL_PAF_NET` (707,840 B) are gone, and a job's lookup takes the one 64 KiB arena. The
Shell still imports SceNet (the UDP log, `log_host`, and the cancel paths of a socket nothing
opens any more) until PR-B. The kernel's capture syscalls (`vjoRequestCapture`, `vjoReadRaw`)
and its full-frame block (`VjoCapture`, 2,088,960 B at most) stay, unused, until their own
removal, which changes the kernel API.

VjoNet's stack goes from 64 to 16 KiB (#132): 64 KiB was for BearSSL's TLS, and since Google Lens's
removal nothing on the thread makes a TLS connection or reaches BearSSL, the HTTP client or a TCP
socket (the UDP log's send is the one network call left, PR-B's). Its deepest chain of the
Shell's own code is 5,624 B: `net_main` 144, a job's `vocr_run_one` 5,000 (its log batch), then
`vjo_log` down to the file write 480 [measured: `arm-vita-eabi-gcc -fstack-usage
-fcallgraph-info=su`, main `6020c0b` with #130; the SDK's own code and the indirect-call sites are
not counted]. `VJO_BUDGET_SHELL_NET_STACK` goes from 69,632 to 20,480 B with its TLS, so SceShell's
permanent pages go from 266,240 to 217,088 B and the worst case from 335,872 to 286,720 B. The
module keeps its 176,128 B: 101,256 B of text (464 B more, the paint and the check) and 70,076 B
of data and `.bss` [measured: the build log's module row]; it imports one function more,
`sceKernelGetThreadInfo` (`sceKernelGetThreadId` was already imported, by `shell/log_vita.c`).
Because nothing measures the thread on a console yet, VjoNet paints its unused stack when it starts
(`shell/worker.c` `net_stack_paint`: from the stack's low end up to 512 B below its own frame, and
only when the kernel reports the 16,384 B it was created with and the frame inside it, else one
`VjoNet stack: not painted (…)` line) and at each game exit logs, when the figure grew,
`VjoNet stack: at most N of 16384 B used` (`net_stack_check`); `--collect`'s analysis keeps the
largest figure, WARNs when under a quarter is left, and WARNs for each boot that did not paint.
The paint takes no allocation and no item.

### Pool "kernel": `ScePhyMemPartKD` (36 MiB, shared by every driver)

| item | bytes | what | source |
|---|---|---|---|
| kernel module pages | 114,688 | `VitaJPOverlay_Kernel.skprx`: 36,864 text + 77,824 data/bss (the 32 KiB model staging buffer and the 4 KiB log ring are in it); API 22's frame-pacing step adds 272 B of text (36,608 B) and 48 B of data/bss, the same pages | packaged SELF segments [measured, API 22 build on main 8f1ec8f] |
| VjoWorker stack | 16,384 | permanent | `kernel/main.c` |
| VjoCapture | 2,088,960 at most (960 × 544 × 4) | no Shell code takes it any more (its users, Google Lens's JPEG and the Anki screenshot, were removed) since API 19: region-sized, `KERNEL_RW`, per capture, freed when the reader has the last row; 458,752 B at 917 × 125 [measured]; #74's console refused 2,088,960 B five times (`0x80024302`) [measured] | `kernel/buffers.c` |
| VjoOcrStrip | 65,536 at most | the OCR ladder's last rung: one strip for one `vjoOcrCaptureRows` call, only when the display hook could not write the game's memory; never held with `VjoCapture` (refused while that is), so it adds nothing to the worst case | `kernel/buffers.c` |
| **limit** | **2,220,032** | today's worst case: a **ratchet**, not a cited safe size. The pool's free space on a retail console is unmeasured; a 2,088,960 B capture failed there once (#14) and has succeeded since. The self-test reads the partition kernel-side and replaces this number | [vita-memory.md 5.3](vita-memory.md) |

### Pool "game": `ScePhyMemPartGame` (256 MiB minus the game's own use)

| item | bytes | what | source |
|---|---|---|---|
| OCR worker pages | 90,112 | the module, in each game it is registered for: 73,728 text + 16,384 data/bss; raised from 45,056 B for the in-game capture (strips, ladder, probe, step log), then from 53,248 B by two text pages for API 20's job policy (the stable-text wait and the blank-line check) and the text normalisation (`core/textnorm.c`, run in the game so that SceShell's pages stay as they are), then by two more for the beam + LM decoder (vita-vn-ocr's `runtime/vocr_lm.c` and `vocr.c`'s top-K extras) and the confidence cascade (`core/vocr_ocr.c` built for size: 59,692 B of text [`bcd3cf0`]), then by one more for the finder refinement (vita-vn-ocr #83's `runtime/vocr_lines_refine.c`: 65,084 B of text [`fec85af`], 452 B under the page), then by one text and two data/bss pages for API 22's parallel lines, line cache and clock (text 62,012 → 67,452 B on vita-vn-ocr a540027, after #108's `-Os` for `game_ocr/main.c`; data/bss 5,840 → 14,200 B with the 6,160 B line cache and the 2,080 B line queue and slots [main `8f1ec8f` and this build]), and by one more text page for vita-vn-ocr #85's faster W8I kernels, the runtime pin (text 67,452 → 70,124 B; the build's `vocr.c` object 15,288 → 17,952 B of code), in the game's partition, never SceShell's | packaged SELF segments [measured, API 22 build] |
| VjoGameVocr stack+TLS | 36,864 | 32,768 + 4,096 | `game_ocr/main.c` |
| VjoGameVocrWeights | 1,560,576 at most | F20, the largest accepted model, page-rounded; resident between jobs | `core/vocr_ocr.c`; asserted in `test_ledger.c` |
| VjoGameVocr workspace | 1,191,936 at most | per job, at any region, page-rounded: since API 19 it holds no grey copy of the region (770,048 B with one at 960 × 544); since API 20 with the beam + LM decoder's extras (the top-K of every frame, the LM context and the beam workspace, 56,928 B), 4 KiB for the greedy text, and for a model smaller than the cascade's its extras (VOCR4-W20's arena, structs and decoder, 296,192 B), and with the finder's boxes refined its arena (72,400 B at 960 × 544, 16,768 B at 883 × 112): 617,312 B with H15; since API 22 ended by the line slots of the job's helpers (`ocr_threads`, at most two: the worker's own slot, 68,160 B, and per helper its recognizer arena, context and slot, 253,104 B for H15), taken only when the memory check passes with them: 1,191,680 B with H15 and two | `vjo_vocr_job_bytes` + `vjo_vocr_refine_bytes` + `vjo_vocr_lm_job_bytes` + `vjo_vocr_cascade_job_bytes` + `vjo_vocr_helpers_bytes`; asserted in `test_ledger.c` |
| VjoGameVocrHelper stacks+TLS | 40,960 at most | API 22: 16,384 + 4,096 per helper thread, at most two, each created for one line and deleted when its line is placed; none between jobs | `game_ocr/main.c` `pool_run` |
| VjoGameVocrCascade | 1,560,576 | the confidence cascade's larger model, the next model size up of the job's since API 21 (VOCR4-W20, F20's 1,558,016 B, for Normal; VOCR3-H15 for Small; Large has none), page-rounded; `USER_RW` only, resident between jobs like the weights, read at the first doubtful line or by a job that found no line (the warm-up), taken only when the memory check passes with it | `include/vjo_vocr_sizes.h`; `test_ledger.c` |
| VjoGameVocrLM | 3,784,704 at most | the largest pinned LM (`vocr_lm_o3_4000k.vlm`, 3,781,072 B), page-rounded; `USER_RW` only, resident between jobs like the weights, taken only when the memory check passes with it (a smaller tier, or none, otherwise) | `include/vjo_vocr_sizes.h`; `tools/tests/test_vocr_defaults.py` |
| VjoGameVocrCapture | 65,536 at most | one strip of grey rows and, per region row, its checksum, its recent states (16, or as many as fit) and two bits: at most 65,536 B at any region (`VJO_VOCR_CAPTURE_LIMIT`; 65,480 B at 960 × 544, 61,388 B at 917 × 125), per job; `USER_RW`, else CDRAM, else phycont | `vjo_vocr_capture_bytes` |
| VjoGameVocrLine | 516,096 at most | a line taller than a strip (a finder band), kept whole at its own width × height, only when one occurs (41 of 372 linefind dev regions; 78,884 B at most there); what fits the job limit with F20, the workspace and the capture block | `core/vocr_ocr.h` `VJO_VOCR_LINE_MAX_BYTES` |
| **limits** | weights 1,601,536; workspace 1,191,936; job 8,720,384 | the recognizer's ceiling tier, which the preflight (free `USER` minus the job's reserve: 4, 8 or 16 MiB by the largest model size it holds since API 21, `VJO_GAME_RESERVE_*`; memory left to the game, never taken) has to cover before anything is allocated, and since API 20 the 4 MB LM and the cascade's model (the job limit raised from 2,392,064 B by their blocks and workspace; a job takes either only when it fits, and #81's CLANNAD log shows 90,177,536 B free at the least); since API 22 the helpers' slots and stacks (614,400 B more at the worst; a job takes one fewer helper at a time, down to none, when the game is short, at the same reserve) | vita-vn-ocr [`docs/vita-memory-budget.md`](https://github.com/bee-san/vita-vn-ocr/blob/main/docs/vita-memory-budget.md); #81 [measured] |

## The two builds

The release ships two builds of one tree (docs/release-variants.md): the debug build (`VJO_DEBUG=ON`,
the test builds') and the normal build (`OFF`), which compiles the debug-only logging, the
self-test and the traces out. Their module pages differ, so the Shell's and the kernel's module
budgets have an `#if VJO_DEBUG` branch each in `include/vjo_ledger_budget.h`, which
`tools/check_memory_ledger.py --variant debug|normal` reads (the C code sees the same branch; any other
conditional naming `VJO_DEBUG`, an `#elif` or a macro defined twice in one build's lines is refused).
Every other budget is the same. The normal build's check is exact (`--exact`): a module with fewer
pages than its budget fails too, so the normal budgets are always the measured pages.
[measured: both builds of main `3805a13` plus the variants change, the build log's module rows]

| module | debug | normal | |
|---|---:|---:|---|
| `VitaJPOverlay_Shell.suprx` | 176,128 B (100,792 + 70,060) | 167,936 B (90,768 + 69,988) | 2 SceShell pages fewer |
| `VitaJPOverlay_Kernel.skprx` | 114,688 B (36,640 + 77,768) | 110,592 B (32,332 + 76,080) | 1 page of the kernel pool fewer |
| `VitaJPOverlay_OCR.suprx` | 90,112 B (71,072 + 14,208) | 90,112 B (69,708 + 14,208) | the same pages (one budget for both since #124) |

The normal Shell's text is 90,768 B in 94,208 B of pages (3,440 B of room); its data/bss has 3,740 B.

The worker's budget is one value for both builds (their pages are the same since #124). A change
that adds a page to the debug worker only cannot just raise it: the normal build's exact check would
then fail. Split it into the two `#if VJO_DEBUG` branches first, as the Shell's and the kernel's are,
the debug branch raised and the normal one at its own pages (the ledger's message says so).

## The CI check

`tools/check_memory_ledger.py` runs in both CI jobs and as `ctest -R memory_ledger`:

1. **Allocation sites.** It strips comments and string literals from every file under `shell/`,
   `kernel/`, `game_ocr/`, `core/`, `vita/` and `include/`, and looks for `sceKernelAllocMemBlock`,
   `ksceKernelAllocMemBlock`, `sce_paf_memalign`/`malloc`, `sceKernelCreateThread`,
   `ksceKernelCreateThread`, libc `malloc`/`calloc`/`realloc`, C++ `new` and the other allocators
   listed in the script. Each (file, call) pair must be in `ALLOWED`, with at most the listed
   count; those are the wrappers. Anything else fails: "route it through a ledger wrapper". A
   wrapper that disappears fails too.
2. **Worst cases.** It evaluates the budget header and fails when a pool's worst case exceeds its
   limit, or a game item exceeds its tier limit.
3. **Packaged modules.** Given `--kernel`, `--shell` and `--worker`, it sums each module's loaded
   segments rounded to 4 KiB pages and fails when a module has more pages than its declared
   budget. Growing a module is therefore a deliberate act: raise the number in the header, cite
   where the bytes come from in the PR, and expect the question why they cannot live elsewhere.

`tools/tests/test_memory_ledger.py` proves each failure mode on a copy of the tree (an added raw
allocation in C and C++, a call in a comment or string that must not count, a removed wrapper, a
worst case over its limit, an item over its tier, a module one byte into the next page, a file
that is not a SELF).

## At run time

The Shell writes its ledger to `ux0:data/VitaJPOverlay/status.txt` at boot, at each game start and
at each game exit (`tools/install_ftp.sh --status VITA_IP` fetches it):

```
ledger boot pages: current=217088 peak=217088 limit=1048576
ledger boot paf: current=8192 peak=8192 limit=985088 free=2557952
ledger   Shell pages: current=176128 peak=176128
ledger   VjoNet stack: current=20480 peak=20480
ledger   VjoControl stack: current=20480 peak=20480
ledger   Paf plugin (est.): current=8192 peak=8192
```

`free=` is the PAF heap's free size as `GetFreeSize()` reports it (0 before Paf is ready). Stacks
include their 4 KiB of TLS. The kernel writes `ledger kernel: current=… peak=… limit=…` and its
items to `kernel.txt` at start and at every capture. The game worker keeps its ledger in the
game's process, which has no file access; its numbers reach the Shell log through the job result
(`game vocr memory: free USER=… need …`, `workspace_bytes`, `resident_bytes`).

Module pages and the Paf plugin are counted from the declared budget (the CI check ties the
declaration to the binary); stacks, blocks and arenas are counted as they are made and freed.

## What is still an estimate

- The Paf plugin and RCO (8,192 B) and whatever the overlay's widgets take from the PAF heap
  while a page is open: unmeasured. The self-test (plan item 5) records `free=` before and after
  opening the overlay.
- The kernel pool's free space, hence its real limit.
- The PAF limit is one console's best sample; the self-test records the free heap at boot and at
  game launch on every run.
