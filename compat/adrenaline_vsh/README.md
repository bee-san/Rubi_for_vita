# Small Adrenaline Shell bridge

This replacement `adrenaline_vsh.suprx` keeps Adrenaline and AdrBubbleBooter
catalogue handling while linking without newlib. It uses the Vita's native C
functions, reads only the four-byte magic needed to identify a bubble, and
starts no thread or heap. It also checks missing/oversized catalogue names and
clears released hook IDs so a later PAF load can install its hook again.

The installed bridge inspected on 2026-10-17 had 59,944 B of code and 280,300 B
of data/BSS: 344,064 B of page-rounded resident segments (measured from its
SELF). Its on-demand load failed with `ScePhyMemPartShell` reporting 45,056 B
free and a 262,144 B request in native TTY output; PSP games returned to
LiveArea before ScePspemu started. Adding the old kernel module to boot also
led to a Shell exit and was rolled back. The replacement's built SELF holds
8,192 B of pages. `check_adrenaline_vsh.py` refuses a larger build.

The release ZIP includes `compat/adrenaline_vsh.suprx` and this source. Before
replacing `ux0:app/PSPEMUCFW/sce_module/adrenaline_vsh.suprx`, keep a copy of
the existing file. Copy the replacement, read it back to check its checksum,
then restart. No taiHEN configuration edit is needed for this replacement.
Adrenaline's first launch after a reboot can still return to LiveArea while
it loads its kernel bridge; launch it again. Restore the original file to undo
the replacement. If the Vita cannot start, hold L while powering on and restore
it in VitaShell. An Adrenaline update may replace this file.

Standalone build:

```sh
VITASDK=/path/to/vitasdk cmake -S compat/adrenaline_vsh -B build/adrenaline-vsh -G Ninja
VITASDK=/path/to/vitasdk cmake --build build/adrenaline-vsh
```

The hook and catalogue logic derives from
[AdrBubbleBooter OSS, b603ca0](https://github.com/shoui520/AdrBubbleBooter-oss/tree/b603ca04740734abfab0fae192a2a7d2b3cdda2c/integration/adrenaline-v7/overlay/vsh)
and TheFloW's Adrenaline (2016-2018), with Leecherman's bubble support.
License: GPL-3.0-or-later, as in the repository's `LICENSE`.

On 2026-10-17, the replacement passed a normal console boot and loaded with
1,604 B of code and 44 B of data/BSS in native TTY output. A `PSPEMU004` bubble
then started `ScePspemu`, and the owner confirmed Boku no Natsuyasumi 4 was
open on screen. This proves startup for that installation; PSP OCR and other
Adrenaline versions remain unverified. Host tests exercise catalogue and hook behavior.
