# LunaTranslator (vendored, read by the generator)

[LunaTranslator](https://github.com/HIllya51/LunaTranslator) by HIllya51 and contributors. GPL-3.0, the
same licence as this project: [LICENSE](LICENSE) is LunaTranslator's own copy.

- Pin: release `v12.1.2.12`, commit `51884b6c88cba8c42e98ce3e43c4f408017082a1` (2026-10-19).
- These files are the pin's, byte for byte (from `src/NativeImpl/LunaHook/LunaHook/`), and
  `tools/gen_luna_vita.py` checks their SHA-256 before it reads them:

  | File | Path at the pin | SHA-256 |
  | --- | --- | --- |
  | `vita3k_1.cpp` | `emulators/vita3k_1.cpp` | `0049258358a63a6d29cc022954be6ce48b96211baff4328a3211301475dc1b1d` |
  | `vita3k.cpp` | `emulators/vita3k.cpp` | `9f8f34c304a025d0e5690f549660d8cb240db33e2a1aa5bb5da21c2a6ec26354` |
  | `ppsspp_1.cpp` | `emulators/ppsspp_1.cpp` | `08e6baa6d4075d7ece095797f3e3c0d921c6be974ac3c3bbdd82d449edad2b78` |
  | `stringfilters.cpp` | `util/stringfilters.cpp` | `8496f79ecf3dfd8e0bf00149de4844ce912e9eadcc2f534d3fce2eb557637b23` |
  | `LICENSE` | `LICENSE` | `3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986` |

- None of it is compiled. What is used:
  - `vita3k_1.cpp`: its Vita table `emfunctionhooks_1`, which `tools/gen_luna_vita.py` turns into
    `core/hook_luna_table.inc` (and `docs/hook-titles.md`), and its hookfuncs and filterfuns, ported by
    hand to C in `core/hook_luna.c` (their patterns verbatim);
  - `vita3k.cpp`: how Luna loads the table (an `unordered_map` with `emplace`: the first entry of an
    address wins), matches a title ID and normalises an address (`addr_offset`), read for the address
    mapping (Luna's address − 0x80004000 = the offset in the game executable's first segment; an entry
    older than `addr_offset`, f237a9f4, can be Vita3K's guest address, which in a fixed-address executable is
    − 0x81000000: the sites marked `VJO_LUNA_F_GUEST` by the generator's `GUEST_SITES`, `vjo_luna_offset`, #349);
  - `ppsspp_1.cpp`: its PSP table, listed in `docs/hook-titles.md` only;
  - `stringfilters.cpp`: the semantics of `StringFilter`, `CharFilter`, `StringFilterBetween` and the
    other helpers the filters call, and `katakanaMap` (`remapkatakana`), which the generator copies into
    `core/hook_luna_table.inc`.
- Read at the pin and not vendored: `LunaHook/texthook.cc` (`TextHook::Send`: the default read, the
  register and padding, `DATA_INDIRECT`, `USING_CHAR`, `commonfilter`, a filter's exception dropping the
  hit), `include/types.h` (`TextBuffer`: `strA`, `strAW`/`fromWA` as CP932), `include/const.h` (the hook
  type flags), `emulators/emujitarg.hpp` (`VITA3K::emu_arg`: a guest register), `include/stringutils.*`
  (`re::sub` as `std::regex_replace` with ECMAScript, `strReplace`, `Trim`), `LunaHost/textthread.cpp`
  (how `FULL_STRING` hits become sentences), `util/memdbg/memsearch.cc` and `util/ithsys/ithsys.cc`
  (`findBytes`, PCSG00912's search).
- Not ported: the emulator side (Vita3K's JIT hook, the window-title game ID, the generic hook finder),
  the PSP backend, and three hookfuncs and two filters (`PCSG00595`, `TPCSG00291`, `PCSB00985`;
  `PCSG00451`, `PCSG01152`) whose reasons are in `docs/hook-titles.md`.

To update: copy the new pin's files here, update the pins in `tools/gen_luna_vita.py` and this file,
port what changed in `core/hook_luna.c`, run the generator (and `--doc`), and the host tests.
