# Native hook titles

The games Rubi can read text from with a native hook instead of OCR, from
[LunaTranslator](https://github.com/HIllya51/LunaTranslator)'s PS Vita table (51884b6, v12.1.2.12,
GPL-3.0, vendored in [third_party/LunaTranslator](../third_party/LunaTranslator/README.md)), and Rubi's
own CLANNAD entry. Luna's entries are hooks for the Vita3K emulator: a guest address per title ID, the
register that holds the text there, its encoding, and a hookfunc and a filter per site. These table sites
remain unverified on a real Vita. CLANNAD's string-copy CALL source was verified on the owner's Vita with
the combined debug test build on 2026-10-20; its register site remains unverified on a console. The normal
release build has no reported console run. How the game worker uses the hooks, and when it installs one, is in
[native-hooks.md](native-hooks.md).

- **Address.** Luna's address is in Vita3K's guest space after `vita3k.cpp`'s `addr_offset`, where the
  game executable's first segment starts at 0x80004000: the site is that segment's start + (address −
  0x80004000) on a console. This is inferred from Vita3K's load address and Luna's offsets; **no table-address
  site has been verified on a console yet.** Some entries are older than Luna's `addr_offset` (f237a9f4,
  2026-05-20) and hold Vita3K's guest address itself. A Vita3K before 11dc3b8b (2026-05-27) loaded a fixed-address
  executable at its link address, 0x81000000, where a console must load it too, and a relocatable one elsewhere,
  so such an entry is the site's own address on a console only in a fixed-address executable. TIME TRAVELERS'
  `0x8119ADDA` and `0x811A6052` are such entries: less 0x80004000 they are past the end of its segment 0, which
  public Vita3K logs show at 0x81000000, `0x31258C` bytes long. The generator marks the 7 sites whose executable a
  public log shows this way (`VJO_LUNA_F_GUEST`, [listed with their evidence](#sites-at-vita3ks-guest-address)), and
  for those only, when Luna's space puts the site outside segment 0 and segment 0 is at 0x81000000, the worker
  reads the address less 0x81000000 (`vjo_luna_offset`, core/hook_luna.h; the table's Seg-0 offset column shows
  both readings). In a relocatable executable such an address is another module's (PCSG00708's Unity eboot: its
  Mono AOT module), which Rubi does not hook. Vita3K hooks a site where its JIT starts a block, which is a
  function's start or a block inside one: the worker checks the bytes before it patches anything.
- **Ported** means everything the site needs exists in C (`core/hook_luna.c`): the read, its hookfunc,
  its filter. **Refused** sites stay in the table (the worker can log why) and are never installed; the
  reasons are in the table below. A filter step that cannot run safely (its work budget, a pattern
  outside the regex subset) drops that hit, and OCR answers.
- **Version.** A site whose Luna comment names a game version ("// 1.01") is tried only in that version
  of the game (param.sfo's APP_VER); a site without one only when no site of its title names the game's
  version.
- **Filters** run on the decoded UTF-8 text, with Luna's patterns verbatim. Where Luna runs a byte
  pattern on a UTF-8 site, the port matches the same bytes, so the result is Luna's. On a CP932 site Luna
  matched Shift-JIS bytes and the port matches characters: the same result, except where one of a
  pattern's ASCII delimiters (`@ [ \ ] ^ _ { | }` or a letter) is the trail byte of a double-byte
  character (ソ 表 ポ: 83 5C, 95 5C, 83 7C), where Luna's match cuts that character and the port's does
  not. Luna's duplicate checks (`static std::string last`) are left out: Rubi reads the latest text
  when a job asks for it.
- **Decoding** (`core/hook_text.c`): bytes that are not valid in the encoding are dropped (Windows'
  decoder, Luna's, substitutes a character); CP932's user-defined rows (F040-F9FC) have no glyph and are
  dropped too.

Regenerate this file's table part with `python3 tools/gen_luna_vita.py --doc` (and the C table with
`python3 tools/gen_luna_vita.py`); ctest's `hook_luna_table` fails when either is stale.

## STEINS;GATE: no hook entry in any release

No Vita release of STEINS;GATE is in Luna's Vita table or in Rubi's own entries (Luna has only the PSP games
`ULJM05887` and `ULJM06040`, below), so Rubi reads them with OCR. A release's title ID and its executable's
module, from its public Vita3K log (Vita3K/compatibility; the European STEINS;GATE 0, PCSB01012, has no log there,
and the European STEINS;GATE was not looked up):

| Game | Title ID | `eboot.bin` module |
| --- | --- | --- |
| STEINS;GATE | PCSG00147 (JP), PCSE00644 (US) | `adv6` |
| STEINS;GATE 比翼恋理のだーりん | PCSG00146 | `adv7` |
| STEINS;GATE 線形拘束のフェノグラム | PCSG00250, PCSG90102 (its demo) | `sgfd2` |
| STEINS;GATE 0 | PCSG00673 (JP), PCSE00949 (US) | `Game` |
| STEINS;GATE ELITE | PCSG01166 | `SteinsGateElite` |

In 比翼恋理のだーりん (`adv7`, the farm's notes; also its ROBOTICS;NOTES ELITE and CHAOS;CHILD) MAGES' engine keeps a
line as SC3, big-endian 16-bit glyph indices into the game's font with control codes and NUL bytes inside the line,
not CP932 or UTF-8, so the string-copy hooks (which want two Japanese characters in CP932 or UTF-8) find nothing
either; for `adv6`, `sgfd2`, `Game` and `SteinsGateElite` that is likely, not checked. The psandvita farm has a
placeable site for 比翼恋理のだーりん (PCSG00146, `adv7` NID 0x2BAC7A6D, +0x2B018, a function's entry, r0), which
rubi-bucket-hooks left out ([native-hooks.md](native-hooks.md#farm-checks)): a hook there needs a reader that goes
through the NULs to the click-wait code and that game's charset (2,666 glyphs). Any other release needs a farm run of
its own (its site, module NID and bytes) and its own charset (#349).

<!-- Generated by tools/gen_luna_vita.py --doc from here on: do not edit by hand. -->

## Totals (LunaTranslator 51884b6, v12.1.2.12)

| | Vita (vita3k_1.cpp) |
| --- | ---: |
| Entries in Luna's table | 307 |
| Title IDs | 182 |
| Games named in Luna's comments | 186 |
| Sites (one per address and title ID) | 306 |
| Sites ported | 294 |
| Sites refused | 12 |
| Title IDs with at least one ported site | 175 |
| Entries left out (same address and title as an earlier one) | 2 |

Refused sites, by reason (a site can have several):

| Reason | Sites |
| --- | ---: |
| USING_CHAR | 4 |
| filter PCSG00451 | 2 |
| hookfunc PCSG00595 | 2 |
| filter PCSG01152 | 1 |
| hookfunc PCSB00985 | 1 |
| hookfunc TPCSG00291 | 1 |
| shadowed | 1 |

## Vita titles

Seg-0 offset: the site's offset in the game executable's first segment, Luna's address − 0x80004000 (not yet seen on a console). A site that Luna recorded at Vita3K's guest address in a fixed-address executable (listed [below](#sites-at-vita3ks-guest-address)) shows a second one, the address − 0x81000000, which the worker takes when the first is outside segment 0 and segment 0 is at 0x81000000 (`vjo_luna_offset`, core/hook_luna.h). Version: the game version Luna's comment names (the site is tried only in that version; a site without one only when no site of its title names the game's version). Reg: the register that holds the text's address at the site.

| Title ID | Region | Game (Luna's comment) | Address | Seg-0 offset | Version | Reg | Padding | Encoding | Flags | Hookfunc | Filter | Luna's note | Status |
| --- | --- | --- | --- | --- | --- | --- | ---: | --- | --- | --- | --- | --- | --- |
| PCSB00985 | EU | PSYCHO-PASS MANDATORY HAPPINESS | [0x8004D360](../third_party/LunaTranslator/vita3k_1.cpp#L1523) | 0x49360 |  | r0 | 0 | UTF-16LE |  | PCSB00985 |  |  | refused: hookfunc PCSB00985: sorts its hits into two text threads by content (Luna's split value), which one site's stream cannot represent; the European release, in English |
| PCSG00020 | JP | 極限脱出ＡＤＶ 善人シボウデス | [0x810DE344](../third_party/LunaTranslator/vita3k_1.cpp#L1103) | 0x10DA344 |  | r6 | 0 | UTF-8 |  |  | PCSG00020 |  | ported |
| PCSG00062 | JP | TIME TRAVELERS | [0x8119ADDA](../third_party/LunaTranslator/vita3k_1.cpp#L1090) | 0x1196DDA or 0x19ADDA |  | r7 | 0 | CP932 |  |  | PCSG00062 |  | ported |
| PCSG00062 | JP | TIME TRAVELERS | [0x811A6052](../third_party/LunaTranslator/vita3k_1.cpp#L1091) | 0x11A2052 or 0x1A6052 |  | r8 | 0 | CP932 |  |  | PCSG00062 |  | ported |
| PCSG00139 | JP | フォトカノ Kiss | [0x81161B0E](../third_party/LunaTranslator/vita3k_1.cpp#L1099) | 0x115DB0E or 0x161B0E |  | r0 | 0 | CP932 |  |  | NewLineCharFilterA |  | ported |
| PCSG00172 | JP | 神咒神威神楽 | [0x810A2486](../third_party/LunaTranslator/vita3k_1.cpp#L1381) | 0x109E486 or 0xA2486 |  | r0 | 0 | CP932 |  |  | PCSG00172 |  | ported |
| PCSG00216 | JP | るいは智を呼ぶ | [0x81003DB0](../third_party/LunaTranslator/vita3k_1.cpp#L1173) | 0xFFFDB0 or 0x3DB0 |  | r1 | 0 | UTF-8 |  |  | FPCSG00839 | dialogue | ported |
| PCSG00271 | JP | Princess Arthur | [0x80065554](../third_party/LunaTranslator/vita3k_1.cpp#L1063) | 0x61554 |  | r0 | 0 | CP932 | FULL_STRING |  | PCSG00766 |  | ported |
| PCSG00272 | JP | DIABOLIK LOVERS LIMITED V EDITION | [0x8105A176](../third_party/LunaTranslator/vita3k_1.cpp#L1265) | 0x1056176 or 0x5A176 |  | r5 | 0 | CP932 |  |  | PCSG00472 | prologue+text | ported |
| PCSG00272 | JP | DIABOLIK LOVERS LIMITED V EDITION | [0x8103416C](../third_party/LunaTranslator/vita3k_1.cpp#L1266) | 0x103016C or 0x3416C |  | r0 | 0 | CP932 |  |  | PCSG00272 | name+text | ported |
| PCSG00291 | JP | アマガミ | [0x80070658](../third_party/LunaTranslator/vita3k_1.cpp#L1171) | 0x6C658 |  | r0 | 0 | CP932 |  | TPCSG00291 |  |  | refused: hookfunc TPCSG00291: keeps state across calls: a counter that drops the hits after a PLAYER substitution, and a duplicate check (and decodes byte-swapped Shift-JIS) |
| PCSG00299 | JP | 俺たちに翼はない | [0x8003EC88](../third_party/LunaTranslator/vita3k_1.cpp#L1121) | 0x3AC88 |  | r7 | 0 | CP932 |  |  |  |  | ported |
| PCSG00367 | JP | Goes! | [0x8004D4A0](../third_party/LunaTranslator/vita3k_1.cpp#L1527) | 0x494A0 | 1.01 | lr | 0 | UTF-16LE | FULL_STRING |  | PCSG00367 |  | ported |
| PCSG00367 | JP | Goes! | [0x8004D2D4](../third_party/LunaTranslator/vita3k_1.cpp#L1528) | 0x492D4 | 1.00 | lr | 0 | UTF-16LE | FULL_STRING |  | PCSG00367 |  | ported |
| PCSG00370 | JP | デュラララ!! 3way standoff -alley- V | [0x80053BD0](../third_party/LunaTranslator/vita3k_1.cpp#L1376) | 0x4FBD0 |  | r0 | 0 | CP932 |  |  | PCSG00370 |  | ported |
| PCSG00382 | JP | RE:VICE[D] | [0x8002D4CA](../third_party/LunaTranslator/vita3k_1.cpp#L1530) | 0x294CA |  | r0 | 0 | CP932 |  |  | PCSG00472 |  | ported |
| PCSG00389 | JP | バイナリースター | [0x800B85C6](../third_party/LunaTranslator/vita3k_1.cpp#L1168) | 0xB45C6 | 1.00 | r0 | 0 | CP932 | FULL_STRING |  | PCSG00766 |  | ported |
| PCSG00389 | JP | バイナリースター | [0x800B849A](../third_party/LunaTranslator/vita3k_1.cpp#L1169) | 0xB449A | 1.01 | r0 | 0 | CP932 | FULL_STRING |  | PCSG00766 |  | ported |
| PCSG00397 | JP | ニセコイ　ヨメイリ！？ | [0x8189E60C](../third_party/LunaTranslator/vita3k_1.cpp#L1255) | 0x189A60C |  | r4 | 0 | UTF-8 |  |  |  |  | ported |
| PCSG00401 | JP | ソラユメ | [0x8000C1C8](../third_party/LunaTranslator/vita3k_1.cpp#L1061) | 0x81C8 |  | r0 | 0 | CP932 | FULL_STRING |  | PCSG00401 |  | ported |
| PCSG00401 | JP | ソラ*ユメ | [0x8000BAD4](../third_party/LunaTranslator/vita3k_1.cpp#L1185) | 0x7AD4 |  | r1 | 0 | CP932 |  |  | FPCSG00401 |  | ported |
| PCSG00402 | JP | Code：Realize ～創世の姫君～ | [0x800879DE](../third_party/LunaTranslator/vita3k_1.cpp#L1234) | 0x839DE |  | r0 | 0 | CP932 |  |  | PCSG00402 |  | ported |
| PCSG00402 | JP | Code：Realize ～創世の姫君～ | [0x8001A39A](../third_party/LunaTranslator/vita3k_1.cpp#L1235) | 0x1639A |  | r0 | 0x1c | CP932 |  |  | PCSG00402 |  | ported |
| PCSG00405 | JP | レンドフルール | [0x8001BFF2](../third_party/LunaTranslator/vita3k_1.cpp#L1175) | 0x17FF2 |  | r0 | 0 | CP932 |  |  | FPCSG00405 | dialogue,sjis | ported |
| PCSG00410 | JP | 熱血異能部活譚 Trigger Kiss | [0x8004E44A](../third_party/LunaTranslator/vita3k_1.cpp#L1166) | 0x4A44A |  | r0 | 0 | CP932 |  |  | FPCSG00410 | dialogue,sjis | ported |
| PCSG00417 | JP | クロガネ回姫譚 －閃夜一夜－ | [0x8003979C](../third_party/LunaTranslator/vita3k_1.cpp#L1498) | 0x3579C |  | r0 | 0 | CP932 |  |  | PCSG01249 |  | ported |
| PCSG00420 | JP | DRAMAtical Murder | [0x8004630A](../third_party/LunaTranslator/vita3k_1.cpp#L1357) | 0x4230A |  | r0 | 0 | CP932 |  |  | FPCSG00852 |  | ported |
| PCSG00420 | JP | DRAMAtical Murder | [0x8003EED2](../third_party/LunaTranslator/vita3k_1.cpp#L1358) | 0x3AED2 |  | r0 | 0 | CP932 |  |  | FPCSG00852 |  | ported |
| PCSG00425 | JP | 幕末Rock 超魂 | [0x8000BF18](../third_party/LunaTranslator/vita3k_1.cpp#L1460) | 0x7F18 |  | r4 | 0 | CP932 |  |  | PCSG01247 |  | ported |
| PCSG00428 | JP | 戦場の円舞曲 | [0x8006C08C](../third_party/LunaTranslator/vita3k_1.cpp#L1400) | 0x6808C |  | r5 | 0 | CP932 |  |  | FPCSG00448 |  | ported |
| PCSG00428 | JP | 戦場の円舞曲 | [0x8002B1DE](../third_party/LunaTranslator/vita3k_1.cpp#L1401) | 0x271DE |  | r7 | 0 | CP932 |  |  | FPCSG00448 |  | ported |
| PCSG00431 | JP | NORN9 VAR COMMONS | [0x80019DFA](../third_party/LunaTranslator/vita3k_1.cpp#L1273) | 0x15DFA |  | r4 | 0 | CP932 |  |  | PCSG00431 |  | ported |
| PCSG00431 | JP | NORN9 VAR COMMONS | [0x800338AA](../third_party/LunaTranslator/vita3k_1.cpp#L1274) | 0x2F8AA |  | sp | 0 | CP932 |  |  | FPCSG00855 |  | ported |
| PCSG00433 | JP | 絶対絶望少女　ダンガンロンパ Another Episode | [0x80086CB8](../third_party/LunaTranslator/vita3k_1.cpp#L1464) | 0x82CB8 |  | r1 | 0 | UTF-16LE |  |  | PCSG00433 |  | ported |
| PCSG00448 | JP | MARGINAL#4 IDOL OF SUPERNOVA | [0x800718F8](../third_party/LunaTranslator/vita3k_1.cpp#L1142) | 0x6D8F8 |  | r0 | 0 | CP932 |  |  | FPCSG00448 | dialogue,sjis | ported |
| PCSG00449 | JP | 神さまと恋ゴコロ | [0x8000E47A](../third_party/LunaTranslator/vita3k_1.cpp#L1487) | 0xA47A |  | r0 | 0 | CP932 |  |  | PCSG01144 |  | ported |
| PCSG00449 | JP | 神さまと恋ゴコロ | [0x80015842](../third_party/LunaTranslator/vita3k_1.cpp#L1488) | 0x11842 |  | r1 | 0 | CP932 |  |  | PCSG00449 |  | ported |
| PCSG00451 | JP | ハナヤマタ　よさこいLIVE！ | [0x810789EE](../third_party/LunaTranslator/vita3k_1.cpp#L1362) | 0x10749EE |  | r3 | 0 | UTF-16LE |  |  | PCSG00451<1> |  | refused: filter PCSG00451<1>: as PCSG00451<0> |
| PCSG00451 | JP | ハナヤマタ　よさこいLIVE！ | [0x81078B22](../third_party/LunaTranslator/vita3k_1.cpp#L1363) | 0x1074B22 |  | r1 | 0 | UTF-16LE |  |  | PCSG00451<0> |  | refused: filter PCSG00451<0>: keeps every second hit (a call counter): one hit cannot tell which half it is |
| PCSG00458 | JP | ROOT∞REXX | [0x8009F048](../third_party/LunaTranslator/vita3k_1.cpp#L1083) | 0x9B048 |  | r1 | 0 | CP932 |  |  | FPCSG00477 |  | ported |
| PCSG00458 | JP | ROOT∞REXX | [0x8000F3E0](../third_party/LunaTranslator/vita3k_1.cpp#L1084) | 0xB3E0 |  | r11 | 0 | CP932 |  |  | PCSG00826 |  | ported |
| PCSG00468 | JP | 黒蝶のサイケデリカ | [0x80043538](../third_party/LunaTranslator/vita3k_1.cpp#L1190) | 0x3F538 |  | r1 | 0 | UTF-8 |  |  | FPCSG00468 |  | ported |
| PCSG00469 | JP | CLOCK ZERO ～終焉の一秒～ ExTime | [0x8002BA9C](../third_party/LunaTranslator/vita3k_1.cpp#L1115) | 0x27A9C |  | r0 | 0 | UTF-8 |  |  | FPCSG00815 |  | ported |
| PCSG00472 | JP | DIABOLIK LOVERS VANDEAD CARNIVAL | [0x8007300E](../third_party/LunaTranslator/vita3k_1.cpp#L1259) | 0x6F00E |  | r5 | 0 | CP932 |  |  | PCSG00472 |  | ported |
| PCSG00476 | JP | DIABOLIK LOVERS MORE,BLOOD LIMITED V EDITION | [0x800285BE](../third_party/LunaTranslator/vita3k_1.cpp#L1268) | 0x245BE |  | r8 | 0 | CP932 |  |  | PCSG00472 |  | ported |
| PCSG00476 | JP | DIABOLIK LOVERS MORE,BLOOD LIMITED V EDITION | [0x80033F0E](../third_party/LunaTranslator/vita3k_1.cpp#L1269) | 0x2FF0E |  | r0 | 0 | CP932 |  |  | PCSG00272 |  | ported |
| PCSG00477 | JP | 百華夜光 | [0x80032B30](../third_party/LunaTranslator/vita3k_1.cpp#L1215) | 0x2EB30 |  | r8 | 0 | CP932 |  |  |  |  | ported |
| PCSG00477 | JP | 百華夜光 | [0x80019C5A](../third_party/LunaTranslator/vita3k_1.cpp#L1216) | 0x15C5A |  | r5 | 0 | CP932 |  |  |  |  | ported |
| PCSG00477 | JP | 百華夜光 | [0x80031A46](../third_party/LunaTranslator/vita3k_1.cpp#L1217) | 0x2DA46 |  | r6 | 0 | CP932 |  |  |  |  | ported |
| PCSG00477 | JP | 百華夜光 | [0x8003A49A](../third_party/LunaTranslator/vita3k_1.cpp#L1218) | 0x3649A |  | r0 | 0 | CP932 |  |  | FPCSG00477 |  | ported |
| PCSG00477 | JP | 百華夜光 | [0x80182532](../third_party/LunaTranslator/vita3k_1.cpp#L1219) | 0x17E532 |  | r7 | 0 | CP932 |  |  | FPCSG00477 |  | ported |
| PCSG00477 | JP | 百華夜光 | [0x8017D1DA](../third_party/LunaTranslator/vita3k_1.cpp#L1220) | 0x1791DA |  | r5 | 0 | CP932 |  |  |  |  | ported |
| PCSG00477 | JP | 百華夜光 | [0x8017D478](../third_party/LunaTranslator/vita3k_1.cpp#L1221) | 0x179478 |  | r4 | 0 | CP932 |  |  |  |  | ported |
| PCSG00477 | JP | 百華夜光 | [0x8017A6AA](../third_party/LunaTranslator/vita3k_1.cpp#L1222) | 0x1766AA |  | r6 | 0 | CP932 |  |  |  |  | ported |
| PCSG00481 | JP | 新装版クリムゾン・エンパイア | [0x800125AE](../third_party/LunaTranslator/vita3k_1.cpp#L1513) | 0xE5AE |  | r1 | 0 | UTF-8 |  |  | NewLineCharFilterA |  | ported |
| PCSG00482 | JP | デュラララ!! Relay | [0x80086798](../third_party/LunaTranslator/vita3k_1.cpp#L1378) | 0x82798 |  | r1 | 0 | UTF-8 |  |  | PCSG00482 |  | ported |
| PCSG00482 | JP | デュラララ!! Relay | [0x8005F0C4](../third_party/LunaTranslator/vita3k_1.cpp#L1379) | 0x5B0C4 |  | r1 | 0 | UTF-8 |  |  | PCSG00482 |  | ported |
| PCSG00502 | JP | 結城友奈は勇者である　樹海の記憶 | [0x800E954E](../third_party/LunaTranslator/vita3k_1.cpp#L1468) | 0xE554E |  | r0 | 0 | UTF-16LE |  |  | NewLineCharFilterW |  | ported |
| PCSG00505 | JP | 大図書館の羊飼い -Library Party- | [0x80031B24](../third_party/LunaTranslator/vita3k_1.cpp#L1071) | 0x2DB24 |  | r0 | 0 | CP932 | FULL_STRING |  |  |  | ported |
| PCSG00509 | JP | POSSESSION MAGENTA | [0x80016C76](../third_party/LunaTranslator/vita3k_1.cpp#L1101) | 0x12C76 |  | r0 | 0 | CP932 |  |  | FPCSG00852 |  | ported |
| PCSG00510 | JP | Starry☆Sky ~Spring Stories~ / Starry Sky ~Spring Stories~ | [0x8003542E](../third_party/LunaTranslator/vita3k_1.cpp#L1287) | 0x3142E |  | r3 | 0 | CP932 |  |  | FPCSG00912 |  | ported |
| PCSG00510 | JP | Starry☆Sky ~Spring Stories~ / Starry Sky ~Spring Stories~ | [0x80033F2E](../third_party/LunaTranslator/vita3k_1.cpp#L1288) | 0x2FF2E |  | r0 | 0 | CP932 |  |  | FPCSG00912 |  | ported |
| PCSG00510 | JP | Starry☆Sky ~Spring Stories~ / Starry Sky ~Spring Stories~ | [0x8002C5B8](../third_party/LunaTranslator/vita3k_1.cpp#L1289) | 0x285B8 |  | r0 | 0x8 | CP932 |  |  | FPCSG00912 |  | ported |
| PCSG00520 | JP | カレイドイヴ | [0x80018748](../third_party/LunaTranslator/vita3k_1.cpp#L1483) | 0x14748 |  | r0 | 0 | CP932 |  |  | FPCSG00852 |  | ported |
| PCSG00526 | JP | 新装版お菓子な島のピーターパン～Sweet Never Land～ | [0x80027140](../third_party/LunaTranslator/vita3k_1.cpp#L1507) | 0x23140 |  | r1 | 0 | UTF-8 |  |  |  |  | ported |
| PCSG00530 | JP | DIABOLIK LOVERS DARK FATE | [0x8002CF8E](../third_party/LunaTranslator/vita3k_1.cpp#L1257) | 0x28F8E |  | r1 | 0 | CP932 |  |  | PCSG00530 |  | ported |
| PCSG00534 | JP | カエル畑 ＤＥ つかまえて・夏　千木良参戦！ | [0x80014488](../third_party/LunaTranslator/vita3k_1.cpp#L1480) | 0x10488 |  | r6 | 0 | CP932 |  |  | PCSG01144_1 |  | ported |
| PCSG00534 | JP | カエル畑 ＤＥ つかまえて・夏　千木良参戦！ | [0x80011E7A](../third_party/LunaTranslator/vita3k_1.cpp#L1481) | 0xDE7A |  | r0 | 0 | CP932 |  |  |  |  | ported |
| PCSG00535 | JP | 学園ヘヴン BOY'S LOVE SCRAMBLE! | [0x80060E58](../third_party/LunaTranslator/vita3k_1.cpp#L1371) | 0x5CE58 |  | r2 | 0 | CP932 |  |  | PCSG00535_1 |  | ported |
| PCSG00535 | JP | 学園ヘヴン BOY'S LOVE SCRAMBLE! | [0x80083A94](../third_party/LunaTranslator/vita3k_1.cpp#L1372) | 0x7FA94 |  | r2 | 0 | CP932 |  |  | PCSG00535_2 |  | ported |
| PCSG00543 | JP | 冴えない彼女の育てかた -blessing flowers- | [0x80014426](../third_party/LunaTranslator/vita3k_1.cpp#L1065) | 0x10426 |  | r11 | 0 | UTF-16LE | FULL_STRING |  | PCSG00543 | 这作可以用来锚定不同版本模拟器的偏移量。搜索后这条很明显。然后再用任意另一作验证即可。 (this game anchors the offset of each Vita3K version: after a search this one is obvious; check with any other game) | ported |
| PCSG00561 | JP | １２時の鐘とシンデレラ～シンデレラシリーズ　トリプル全巻パック～ | [0x8001701C](../third_party/LunaTranslator/vita3k_1.cpp#L1331) | 0x1301C |  | r1 | 0 | UTF-8 |  |  | NewLineCharFilterA |  | ported |
| PCSG00574 | JP | スクール・ウォーズ～全巻パック　本編＆卒業戦線～ / ロミオVSジュリエット 全巻パック | [0x80017628](../third_party/LunaTranslator/vita3k_1.cpp#L1502) | 0x13628 |  | r1 | 0 | UTF-8 |  |  |  |  | ported |
| PCSG00580 | JP | 新装版魔法使いとご主人様～Wizard and The Master～ | [0x8001733C](../third_party/LunaTranslator/vita3k_1.cpp#L1336) | 0x1333C |  | r1 | 0 | UTF-8 |  |  | NewLineCharFilterA |  | ported |
| PCSG00585 | JP | 学園ヘヴン2 ～DOUBLE SCRAMBLE!～ | [0x80036616](../third_party/LunaTranslator/vita3k_1.cpp#L1374) | 0x32616 |  | r12 | 0 | CP932 |  |  | PCSG00585 |  | ported |
| PCSG00591 | JP | アブナイ☆恋の捜査室 ～Eternal Happiness～ | [0x800151A4](../third_party/LunaTranslator/vita3k_1.cpp#L1095) | 0x111A4 |  | r4 | 0 | UTF-16LE |  |  |  |  | ported |
| PCSG00592 | JP | I DOLL U | [0x8000AC70](../third_party/LunaTranslator/vita3k_1.cpp#L1321) | 0x6C70 |  | r0 | 0 | UTF-8 |  |  | PCSG00833 | 需要自己替换#Name[1] #Name[2] (#Name[1] #Name[2] must be replaced by hand) | ported |
| PCSG00592 | JP | I DOLL U | [0x8000AE7E](../third_party/LunaTranslator/vita3k_1.cpp#L1322) | 0x6E7E |  | r4 | 0 | UTF-8 |  |  | PCSG00592 |  | ported |
| PCSG00595 | JP | 神々の悪戯 InFinite | [0x8009730E](../third_party/LunaTranslator/vita3k_1.cpp#L1485) | 0x9330E |  | r0 | 0x20 | CP932 |  | PCSG00595 |  |  | refused: hookfunc PCSG00595: creates a new hook: a DIRECT_READ that Luna's reader thread polls every 500 ms, with a duplicate check (a hook that spawns another is not ported) |
| PCSG00604 | JP | 猛獣使いと王子様 ～Flower ＆ Snow～ | [0x80071E3C](../third_party/LunaTranslator/vita3k_1.cpp#L1504) | 0x6DE3C |  | r0 | 0 | UTF-8 |  |  | FPCSG00855 |  | ported |
| PCSG00604 | JP | 猛獣使いと王子様 ～Flower ＆ Snow～ | [0x80080BAA](../third_party/LunaTranslator/vita3k_1.cpp#L1505) | 0x7CBAA |  | r7 | 0 | UTF-8 |  |  | FPCSG00855 |  | ported |
| PCSG00611 | JP | 絶対迷宮　秘密のおやゆび姫 | [0x800B9DEA](../third_party/LunaTranslator/vita3k_1.cpp#L1097) | 0xB5DEA |  | r3 | 0 | CP932 |  |  |  |  | ported |
| PCSG00611 | JP | 絶対迷宮 秘密のおやゆび姫 | [0x8003F554](../third_party/LunaTranslator/vita3k_1.cpp#L1247) | 0x3B554 |  | r5 | 0 | CP932 |  |  |  |  | refused: shadowed: Luna's map keeps the first entry for 0x8003F554 (line 1245, PCSG01221), so it never used this one |
| PCSG00614 | JP | ハートの国のアリス～Wonderful Wonder World～ | [0x8100F0CA](../third_party/LunaTranslator/vita3k_1.cpp#L1333) | 0x100B0CA |  | r1 | 0 | UTF-8 | FULL_STRING |  | NewLineCharFilterA | 手动解压 (unpacked by hand) | ported |
| PCSG00614 | JP | ハートの国のアリス～Wonderful Wonder World～ | [0x8001290A](../third_party/LunaTranslator/vita3k_1.cpp#L1334) | 0xE90A |  | r1 | 0 | UTF-8 | FULL_STRING |  | NewLineCharFilterA |  | ported |
| PCSG00615 | JP | 東京喰種トーキョーグール JAIL | [0x800714BC](../third_party/LunaTranslator/vita3k_1.cpp#L1521) | 0x6D4BC |  | r4 | 0 | UTF-16LE |  |  | PCSG00615 | 时断时续 (intermittent) | ported |
| PCSG00618 | JP | ロミオVSジュリエット 全巻パック | [0x80017F50](../third_party/LunaTranslator/vita3k_1.cpp#L1329) | 0x13F50 |  | r1 | 0 | UTF-8 |  |  | NewLineCharFilterA |  | ported |
| PCSG00618 | JP | スクール・ウォーズ～全巻パック　本編＆卒業戦線～ / ロミオVSジュリエット 全巻パック | [0x80017628](../third_party/LunaTranslator/vita3k_1.cpp#L1502) | 0x13628 |  | r1 | 0 | UTF-8 |  |  |  |  | ported |
| PCSG00619 | JP | 源氏恋絵巻 | [0x8002C640](../third_party/LunaTranslator/vita3k_1.cpp#L1285) | 0x28640 |  | r1 | 0 | UTF-8 |  |  |  |  | ported |
| PCSG00640 | JP | SA7 -Silent Ability Seven- | [0x800291B2](../third_party/LunaTranslator/vita3k_1.cpp#L1532) | 0x251B2 |  | r2 | 0 | UTF-16LE |  |  | PCSG00367 |  | ported |
| PCSG00640 | JP | SA7 -Silent Ability Seven- | [0x80045C48](../third_party/LunaTranslator/vita3k_1.cpp#L1533) | 0x41C48 |  | r1 | 0 | UTF-16LE |  |  | PCSG00367 |  | ported |
| PCSG00648 | JP | 月に寄りそう乙女の作法 | [0x8002AEFA](../third_party/LunaTranslator/vita3k_1.cpp#L1164) | 0x26EFA |  | r2 | 0 | CP932 |  |  |  | dialogue,sjis | ported |
| PCSG00654 | JP | 学園K -Wonderful School Days- V  Edition | [0x80059946](../third_party/LunaTranslator/vita3k_1.cpp#L1369) | 0x55946 |  | r5 | 0 | UTF-8 |  |  | PCSG00654 |  | ported |
| PCSG00663 | JP | Rear pheles | [0x80BB4F70](../third_party/LunaTranslator/vita3k_1.cpp#L1086) | 0xBB0F70 |  | r1 | 0 | UTF-16LE | USING_CHAR DATA_INDIRECT |  | all_ascii_FilterW |  | refused: USING_CHAR: one character per hit, which Luna's text thread joins into a sentence across hits |
| PCSG00663 | JP | Rear pheles | [0x83CB10D4](../third_party/LunaTranslator/vita3k_1.cpp#L1087) | 0x3CAD0D4 | 1.00 | r1 | 0 | UTF-16LE | USING_CHAR DATA_INDIRECT |  | all_ascii_FilterW |  | refused: USING_CHAR: one character per hit, which Luna's text thread joins into a sentence across hits |
| PCSG00663 | JP | Rear pheles | [0x80848520](../third_party/LunaTranslator/vita3k_1.cpp#L1088) | 0x844520 | 1.02 | r1 | 0 | UTF-16LE | USING_CHAR DATA_INDIRECT |  | all_ascii_FilterW |  | refused: USING_CHAR: one character per hit, which Luna's text thread joins into a sentence across hits |
| PCSG00664 | JP | なないろ リンカネーション | [0x80035C34](../third_party/LunaTranslator/vita3k_1.cpp#L1075) | 0x31C34 |  | r0 | 0 | CP932 | FULL_STRING |  | PCSG00664 |  | ported |
| PCSG00667 | JP | LOVE:QUIZ ~恋する乙女のファイナルアンサー~ | [0x8003ACBA](../third_party/LunaTranslator/vita3k_1.cpp#L1354) | 0x36CBA |  | r0 | 0 | UTF-16LE |  |  |  |  | ported |
| PCSG00667 | JP | LOVE:QUIZ ~恋する乙女のファイナルアンサー~ | [0x80016DD6](../third_party/LunaTranslator/vita3k_1.cpp#L1355) | 0x12DD6 |  | r1 | 0 | UTF-16LE |  |  |  |  | ported |
| PCSG00689 | JP | ToLOVEる ダークネス トゥループリンセス | [0x8097D494](../third_party/LunaTranslator/vita3k_1.cpp#L1067) | 0x979494 |  | r1 | 0 | UTF-16LE | FULL_STRING |  | PCSG00689 |  | ported |
| PCSG00696 | JP | アンジェーリーク ルトゥール | [0x8008BD1A](../third_party/LunaTranslator/vita3k_1.cpp#L1160) | 0x87D1A |  | r1 | 0 | CP932 |  |  | FPCSG00696 | text1,sjis | ported |
| PCSG00696 | JP | アンジェーリーク ルトゥール | [0x8008CD48](../third_party/LunaTranslator/vita3k_1.cpp#L1161) | 0x88D48 |  | r0 | 0 | CP932 |  |  | FPCSG00696 | text2 | ported |
| PCSG00696 | JP | アンジェーリーク ルトゥール | [0x8008F75A](../third_party/LunaTranslator/vita3k_1.cpp#L1162) | 0x8B75A |  | r0 | 0 | CP932 |  |  | FPCSG00696 | choice | ported |
| PCSG00706 | JP | もし、この世界に神様がいるとするならば。 | [0x80C1F270](../third_party/LunaTranslator/vita3k_1.cpp#L1156) | 0xC1B270 |  | r0 | 0 | UTF-16LE |  | ReadU16TextAndLenDW | FPCSG00706 | dialogue | ported |
| PCSG00706 | JP | もし、この世界に神様がいるとするならば。 | [0x80D48BFC](../third_party/LunaTranslator/vita3k_1.cpp#L1157) | 0xD44BFC |  | r1 | 0 | UTF-16LE |  | ReadU16TextAndLenDW | FPCSG00706 | Dictionary1 | ported |
| PCSG00706 | JP | もし、この世界に神様がいるとするならば。 | [0x80D48C20](../third_party/LunaTranslator/vita3k_1.cpp#L1158) | 0xD44C20 |  | r0 | 0 | UTF-16LE |  | ReadU16TextAndLenDW | FPCSG00706 | Dictionary2 | ported |
| PCSG00708 | JP | 薔薇に隠されしヴェリテ | [0x81558104](../third_party/LunaTranslator/vita3k_1.cpp#L1053) | 0x1554104 |  | r0 | 0xc | UTF-16LE | FULL_STRING |  | PCSG00708 |  | ported |
| PCSG00708 | JP | 薔薇に隠されしヴェリテ | [0x81558264](../third_party/LunaTranslator/vita3k_1.cpp#L1054) | 0x1554264 | 1.01 | r0 | 0xc | UTF-16LE | FULL_STRING |  | PCSG00708 |  | ported |
| PCSG00728 | JP | 終わりのセラフ　運命の始まり | [0x8028394A](../third_party/LunaTranslator/vita3k_1.cpp#L1470) | 0x27F94A |  | r4 | 0 | UTF-8 |  |  | NewLineCharFilterA |  | ported |
| PCSG00745 | JP | スカーレッドライダーゼクス Rev. | [0x800BEE38](../third_party/LunaTranslator/vita3k_1.cpp#L1342) | 0xBAE38 |  | r0 | 0 | UTF-8 |  |  | NewLineCharFilterA |  | ported |
| PCSG00745 | JP | スカーレッドライダーゼクス | [0x8000C2D4](../third_party/LunaTranslator/vita3k_1.cpp#L1519) | 0x82D4 |  | r12 | 0x8 | UTF-8 |  |  | PCSG01247 |  | ported |
| PCSG00751 | JP | アルカナ・ファミリア -La storia della Arcana Famiglia- Ancora | [0x80070E30](../third_party/LunaTranslator/vita3k_1.cpp#L1153) | 0x6CE30 |  | r2 | 0 | CP932 |  |  | FPCSG00751 | all,sjis | ported |
| PCSG00751 | JP | アルカナ・ファミリア -La storia della Arcana Famiglia- Ancora | [0x80070CDC](../third_party/LunaTranslator/vita3k_1.cpp#L1154) | 0x6CCDC |  | r1 | 0 | CP932 |  |  | FPCSG00751 | text | ported |
| PCSG00755 | JP | BROTHERS CONFLICT　Precious Baby | [0x800B2C66](../third_party/LunaTranslator/vita3k_1.cpp#L1112) | 0xAEC66 |  | r5 | 0 | CP932 |  |  | PCSG00402 |  | ported |
| PCSG00755 | JP | BROTHERS CONFLICT　Precious Baby | [0x800B4548](../third_party/LunaTranslator/vita3k_1.cpp#L1113) | 0xB0548 |  | r1 | 0 | CP932 |  |  | PCSG00402 |  | ported |
| PCSG00766 | JP | ニル・アドミラリの天秤 帝都幻惑綺譚 | [0x80073862](../third_party/LunaTranslator/vita3k_1.cpp#L1107) | 0x6F862 |  | r0 | 0 | CP932 |  |  | PCSG00766 |  | ported |
| PCSG00768 | JP | ヴァルプルガの詩 | [0x8003345A](../third_party/LunaTranslator/vita3k_1.cpp#L1093) | 0x2F45A |  | r1 | 0 | CP932 |  |  |  |  | ported |
| PCSG00769 | JP | 逢魔が刻～かくりよの縁～ | [0x8003CA08](../third_party/LunaTranslator/vita3k_1.cpp#L1525) | 0x38A08 |  | r3 | 0 | CP932 |  | PCSG00769 |  |  | ported |
| PCSG00776 | JP | Muv-Luv | [0x80118F10](../third_party/LunaTranslator/vita3k_1.cpp#L1177) | 0x114F10 |  | r5 | 0 | CP932 |  |  | PCSG00776 | dialogue, choices | ported |
| PCSG00776 | JP | Muv-Luv | [0x80126E7E](../third_party/LunaTranslator/vita3k_1.cpp#L1178) | 0x122E7E |  | r0 | 0 | CP932 |  |  | PCSG00776 | dialogue | ported |
| PCSG00780 | JP | 赤い砂堕ちる月 | [0x8006DF02](../third_party/LunaTranslator/vita3k_1.cpp#L1535) | 0x69F02 |  | r1 | 0 | CP932 |  |  | PCSG00780 |  | ported |
| PCSG00787 | JP | 鏡界の白雪 | [0x810286C8](../third_party/LunaTranslator/vita3k_1.cpp#L1249) | 0x10246C8 |  | r0 | 0 | UTF-8 |  |  | NewLineCharFilterA | VPK版手动复制安装 (the VPK, copied in by hand) | ported |
| PCSG00787 | JP | 鏡界の白雪 | [0x8002BB78](../third_party/LunaTranslator/vita3k_1.cpp#L1250) | 0x27B78 |  | r0 | 0 | UTF-8 | FULL_STRING |  | NewLineCharFilterA | zip安装版 (the zip install) | ported |
| PCSG00787 | JP | 鏡界の白雪 | [0x80025F0E](../third_party/LunaTranslator/vita3k_1.cpp#L1251) | 0x21F0E |  | r5 | 0 | UTF-8 |  |  | NewLineCharFilterA |  | ported |
| PCSG00787 | JP | 鏡界の白雪 | [0x80141978](../third_party/LunaTranslator/vita3k_1.cpp#L1252) | 0x13D978 |  | r1 | 0 | UTF-8 |  |  | NewLineCharFilterA |  | ported |
| PCSG00787 | JP | 鏡界の白雪 | [0x8001D862](../third_party/LunaTranslator/vita3k_1.cpp#L1253) | 0x19862 |  | r4 | 0 | UTF-8 | FULL_STRING |  | PCSG00787 |  | ported |
| PCSG00789 | JP | オメルタ CODE:TYCOON 戒 | [0x800BB456](../third_party/LunaTranslator/vita3k_1.cpp#L1069) | 0xB7456 |  | r5 | 0 | CP932 | FULL_STRING |  |  |  | ported |
| PCSG00789 | JP | オメルタ CODE:TYCOON 戒 | [0x800BC462](../third_party/LunaTranslator/vita3k_1.cpp#L1243) | 0xB8462 |  | r3 | 0 | CP932 |  |  | F010088B01A8FC000 |  | ported |
| PCSG00794 | JP | 月影の鎖　～錯乱パラノイア～ | [0x8000E8E0](../third_party/LunaTranslator/vita3k_1.cpp#L1283) | 0xA8E0 |  | r0 | 0 | CP932 |  |  | F010052300F612000 |  | ported |
| PCSG00805 | JP | Code：Realize ～祝福の未来～ | [0x8008E566](../third_party/LunaTranslator/vita3k_1.cpp#L1237) | 0x8A566 |  | r1 | 0 | UTF-8 |  |  | FPCSG00815 |  | ported |
| PCSG00805 | JP | Code：Realize ～祝福の未来～ | [0x80024DE0](../third_party/LunaTranslator/vita3k_1.cpp#L1238) | 0x20DE0 |  | r11 | 0x1c | UTF-8 |  |  | FPCSG00815 |  | ported |
| PCSG00808 | JP | 悠久のティアブレイド -Lost Chronicle- | [0x8003542A](../third_party/LunaTranslator/vita3k_1.cpp#L1194) | 0x3142A |  | r10 | 0 | UTF-8 |  |  | FPCSG00808 |  | ported |
| PCSG00808 | JP | 悠久のティアブレイド -Lost Chronicle- | [0x8002A95A](../third_party/LunaTranslator/vita3k_1.cpp#L1195) | 0x2695A |  | r6 | 0 | UTF-8 |  |  | FPCSG00808 |  | ported |
| PCSG00808 | JP | 悠久のティアブレイド -Lost Chronicle- | [0x801A98AA](../third_party/LunaTranslator/vita3k_1.cpp#L1196) | 0x1A58AA |  | r9 | 0 | UTF-8 |  |  | FPCSG00808 |  | ported |
| PCSG00808 | JP | 悠久のティアブレイド -Lost Chronicle- | [0x801A42BC](../third_party/LunaTranslator/vita3k_1.cpp#L1197) | 0x1A02BC |  | r9 | 0 | UTF-8 |  |  | FPCSG00808 |  | ported |
| PCSG00808 | JP | 悠久のティアブレイド -Lost Chronicle- | [0x801A42D0](../third_party/LunaTranslator/vita3k_1.cpp#L1198) | 0x1A02D0 |  | r7 | 0 | UTF-8 |  |  | FPCSG00808 |  | ported |
| PCSG00812 | JP | 灰鷹のサイケデリカ | [0x80022C06](../third_party/LunaTranslator/vita3k_1.cpp#L1192) | 0x1EC06 |  | r4 | 0 | UTF-8 |  |  | FPCSG00468 |  | ported |
| PCSG00815 | JP | サイキックエモーション ムー | [0x80035948](../third_party/LunaTranslator/vita3k_1.cpp#L1231) | 0x31948 |  | r9 | 0 | UTF-8 |  |  | FPCSG00815 |  | ported |
| PCSG00815 | JP | サイキックエモーション ムー | [0x80034580](../third_party/LunaTranslator/vita3k_1.cpp#L1232) | 0x30580 |  | r6 | 0 | UTF-8 |  |  | FPCSG00815 |  | ported |
| PCSG00826 | JP | DIABOLIK LOVERS LUNATIC PARADE | [0x800579EE](../third_party/LunaTranslator/vita3k_1.cpp#L1263) | 0x539EE |  | r0 | 0 | CP932 |  |  | PCSG00826 |  | ported |
| PCSG00829 | JP | 喧嘩番長 乙女 | [0x80009722](../third_party/LunaTranslator/vita3k_1.cpp#L1150) | 0x5722 |  | r0 | 0 | UTF-16LE |  |  | FPCSG00839 |  | ported |
| PCSG00829 | JP | 喧嘩番長 乙女 | [0x800086C0](../third_party/LunaTranslator/vita3k_1.cpp#L1151) | 0x46C0 |  | r0 | 0 | UTF-16LE |  |  | PCSG00829 | 缺少部分 (some text missing) | ported |
| PCSG00833 | JP | Norn9 ~Norn + Nonette~ Act Tune | [0x8001E288](../third_party/LunaTranslator/vita3k_1.cpp#L1271) | 0x1A288 |  | r0 | 0 | UTF-8 |  |  | PCSG00833 |  | ported |
| PCSG00841 | JP | ダンジョントラベラーズ２－２ 闇堕ちの乙女とはじまりの書 | [0x800BE808](../third_party/LunaTranslator/vita3k_1.cpp#L1432) | 0xBA808 |  | r3 | 0 | UTF-8 |  |  | NewLineCharFilterA |  | ported |
| PCSG00852 | JP | マジきゅんっ！ルネッサンス | [0x8008375A](../third_party/LunaTranslator/vita3k_1.cpp#L1209) | 0x7F75A |  | r1 | 0 | CP932 |  |  | FPCSG00852 |  | ported |
| PCSG00852 | JP | マジきゅんっ！ルネッサンス | [0x8001C194](../third_party/LunaTranslator/vita3k_1.cpp#L1210) | 0x18194 |  | r1 | 0 | CP932 |  |  | FPCSG00852 |  | ported |
| PCSG00853 | JP | ピリオドキューブ～鳥籠のアマデウス～ | [0x8000BDF2](../third_party/LunaTranslator/vita3k_1.cpp#L1324) | 0x7DF2 |  | r5 | 0 | UTF-8 |  |  | PCSG00530 |  | ported |
| PCSG00855 | JP | 花朧 ～戦国伝乱奇～ | [0x80037600](../third_party/LunaTranslator/vita3k_1.cpp#L1224) | 0x33600 |  | r6 | 0 | UTF-8 |  |  | FPCSG00855 |  | ported |
| PCSG00855 | JP | 花朧 ～戦国伝乱奇～ | [0x80036580](../third_party/LunaTranslator/vita3k_1.cpp#L1225) | 0x32580 |  | r6 | 0 | UTF-8 |  |  | FPCSG00855 |  | ported |
| PCSG00855 | JP | 花朧 ～戦国伝乱奇～ | [0x801A2ADA](../third_party/LunaTranslator/vita3k_1.cpp#L1226) | 0x19EADA |  | r0 | 0 | UTF-8 |  |  | FPCSG00855_2<0> |  | ported |
| PCSG00855 | JP | 花朧 ～戦国伝乱奇～ | [0x801A2BA8](../third_party/LunaTranslator/vita3k_1.cpp#L1227) | 0x19EBA8 |  | r0 | 0 | UTF-8 |  |  | FPCSG00855_2<1> |  | ported |
| PCSG00855 | JP | 花朧 ～戦国伝乱奇～ | [0x801A2D9E](../third_party/LunaTranslator/vita3k_1.cpp#L1228) | 0x19ED9E |  | r0 | 0 | UTF-8 |  |  | FPCSG00855_2<2> |  | ported |
| PCSG00855 | JP | 花朧 ～戦国伝乱奇～ | [0x801A2E68](../third_party/LunaTranslator/vita3k_1.cpp#L1229) | 0x19EE68 |  | r0 | 0 | UTF-8 |  |  | FPCSG00855_2<3> |  | ported |
| PCSG00855 | JP | 花朧 ～戦国伝乱奇～ | [0x80024C08](../third_party/LunaTranslator/vita3k_1.cpp#L1326) | 0x20C08 |  | r5 | 0 | UTF-8 |  |  | PCSG00592 |  | ported |
| PCSG00855 | JP | 花朧 ～戦国伝乱奇～ | [0x8000D544](../third_party/LunaTranslator/vita3k_1.cpp#L1327) | 0x9544 |  | r0 | 0 | UTF-8 |  |  | PCSG00855 |  | ported |
| PCSG00866 | JP | Collar×Malice | [0x80030250](../third_party/LunaTranslator/vita3k_1.cpp#L1117) | 0x2C250 |  | r0 | 0 | UTF-8 |  |  |  |  | ported |
| PCSG00903 | JP | New Game! The Challenge Stage! | [0x8012674C](../third_party/LunaTranslator/vita3k_1.cpp#L1148) | 0x12274C |  | r0 | 0 | UTF-8 |  | TPCSG00903 | FPCSG00903 |  | ported |
| PCSG00910 | JP | DIABOLIK LOVERS LOST EDEN | [0x8007443E](../third_party/LunaTranslator/vita3k_1.cpp#L1261) | 0x7043E |  | r0 | 0 | CP932 |  |  |  |  | ported |
| PCSG00911 | JP | Re:BIRTHDAY SONG～恋を唄う死神～ | [0x80033AF6](../third_party/LunaTranslator/vita3k_1.cpp#L1180) | 0x2FAF6 |  | r0 | 0x2 | CP932 |  |  |  | dialogue | ported |
| PCSG00912 | JP | Un:BIRTHDAY SONG ～愛を唄う死神～ | [0x80038538](../third_party/LunaTranslator/vita3k_1.cpp#L1182) | 0x34538 |  | r0 | 0 | CP932 |  | PCSG00912 |  |  | ported |
| PCSG00912 | JP | Un:BIRTHDAY SONG ～愛を唄う死神～ | [0x80033D66](../third_party/LunaTranslator/vita3k_1.cpp#L1183) | 0x2FD66 |  | r3 | 0x4 | CP932 |  |  | FPCSG00912 |  | ported |
| PCSG00914 | JP | うたの☆プリンスさまっ♪Repeat LOVE | [0x8005A534](../third_party/LunaTranslator/vita3k_1.cpp#L1515) | 0x56534 |  | r1 | 0x20 | CP932 | FULL_STRING | PCSG00914 |  |  | ported |
| PCSG00915 | JP | DYNAMIC CHORD feat.apple-polisher V edition | [0x80033F6C](../third_party/LunaTranslator/vita3k_1.cpp#L1434) | 0x2FF6C |  | r0 | 0 | CP932 |  |  | FPCSG00912 |  | ported |
| PCSG00915 | JP | DYNAMIC CHORD feat.apple-polisher V edition | [0x8003C61E](../third_party/LunaTranslator/vita3k_1.cpp#L1435) | 0x3861E |  | r0 | 0 | CP932 |  |  | FPCSG00912 | prolog1 | ported |
| PCSG00915 | JP | DYNAMIC CHORD feat.apple-polisher V edition | [0x80035924](../third_party/LunaTranslator/vita3k_1.cpp#L1436) | 0x31924 |  | r6 | 0 | CP932 |  |  | FPCSG00912 | prolog2+name2 | ported |
| PCSG00915 | JP | DYNAMIC CHORD feat.apple-polisher V edition | [0x8003548C](../third_party/LunaTranslator/vita3k_1.cpp#L1437) | 0x3148C |  | r3 | 0 | CP932 |  |  | FPCSG00912 | prolog2 | ported |
| PCSG00916 | JP | Starry☆Sky ~Summer Stories~ / Starry Sky ~Summer Stories~ | [0x80035634](../third_party/LunaTranslator/vita3k_1.cpp#L1291) | 0x31634 |  | r3 | 0 | CP932 |  |  | FPCSG00912 |  | ported |
| PCSG00916 | JP | Starry☆Sky ~Summer Stories~ / Starry Sky ~Summer Stories~ | [0x80034114](../third_party/LunaTranslator/vita3k_1.cpp#L1292) | 0x30114 |  | r0 | 0 | CP932 |  |  | FPCSG00912 |  | ported |
| PCSG00916 | JP | Starry☆Sky ~Summer Stories~ / Starry Sky ~Summer Stories~ | [0x8002C77A](../third_party/LunaTranslator/vita3k_1.cpp#L1293) | 0x2877A |  | r0 | 0x8 | CP932 |  |  | FPCSG00912 |  | ported |
| PCSG00917 | JP | Starry☆Sky ~Autumn Stories~ / Starry Sky ~Autumn Stories~ | [0x80035B10](../third_party/LunaTranslator/vita3k_1.cpp#L1295) | 0x31B10 | 1.00 | r3 | 0 | CP932 |  |  | FPCSG00912 |  | ported |
| PCSG00917 | JP | Starry☆Sky ~Autumn Stories~ / Starry Sky ~Autumn Stories~ | [0x800345F0](../third_party/LunaTranslator/vita3k_1.cpp#L1296) | 0x305F0 | 1.00 | r0 | 0 | CP932 |  |  | FPCSG00912 |  | ported |
| PCSG00917 | JP | Starry☆Sky ~Autumn Stories~ / Starry Sky ~Autumn Stories~ | [0x8002CC56](../third_party/LunaTranslator/vita3k_1.cpp#L1297) | 0x28C56 | 1.00 | r0 | 0x8 | CP932 |  |  | FPCSG00912 |  | ported |
| PCSG00917 | JP | Starry☆Sky ~Autumn Stories~ / Starry Sky ~Autumn Stories~ | [0x80035B2C](../third_party/LunaTranslator/vita3k_1.cpp#L1298) | 0x31B2C | 1.01 | r3 | 0 | CP932 |  |  | FPCSG00912 |  | ported |
| PCSG00917 | JP | Starry☆Sky ~Autumn Stories~ / Starry Sky ~Autumn Stories~ | [0x8003460C](../third_party/LunaTranslator/vita3k_1.cpp#L1299) | 0x3060C | 1.01 | r0 | 0 | CP932 |  |  | FPCSG00912 |  | ported |
| PCSG00917 | JP | Starry☆Sky ~Autumn Stories~ / Starry Sky ~Autumn Stories~ | [0x8002CC72](../third_party/LunaTranslator/vita3k_1.cpp#L1300) | 0x28C72 | 1.01 | r0 | 0x8 | CP932 |  |  | FPCSG00912 |  | ported |
| PCSG00917 | JP | ワンドオブフォーチュン R | [0x8008128A](../third_party/LunaTranslator/vita3k_1.cpp#L1306) | 0x7D28A | 1.00 | r0 | 0 | UTF-8 |  |  | PCSG00917 |  | ported |
| PCSG00917 | JP | ワンドオブフォーチュン R | [0x8008134E](../third_party/LunaTranslator/vita3k_1.cpp#L1309) | 0x7D34E | 1.01 | r0 | 0 | UTF-8 |  |  | PCSG00917 |  | ported |
| PCSG00917 | JP | ワンドオブフォーチュン R | [0x80081378](../third_party/LunaTranslator/vita3k_1.cpp#L1310) | 0x7D378 | 1.01 | r0 | 0 | UTF-8 |  |  | PCSG00917 |  | ported |
| PCSG00917 | JP | ワンドオブフォーチュン R | [0x8002CB8C](../third_party/LunaTranslator/vita3k_1.cpp#L1311) | 0x28B8C | 1.01 | r8 | 0 | UTF-8 |  |  | PCSG00917 |  | ported |
| PCSG00918 | JP | Starry☆Sky ~Winter Stories~ / Starry Sky ~Winter Stories~ | [0x80035A20](../third_party/LunaTranslator/vita3k_1.cpp#L1302) | 0x31A20 |  | r3 | 0 | CP932 |  |  | FPCSG00912 |  | ported |
| PCSG00918 | JP | Starry☆Sky ~Winter Stories~ / Starry Sky ~Winter Stories~ | [0x80034500](../third_party/LunaTranslator/vita3k_1.cpp#L1303) | 0x30500 |  | r0 | 0 | CP932 |  |  | FPCSG00912 |  | ported |
| PCSG00918 | JP | Starry☆Sky ~Winter Stories~ / Starry Sky ~Winter Stories~ | [0x8002CB66](../third_party/LunaTranslator/vita3k_1.cpp#L1304) | 0x28B66 |  | r0 | 0x8 | CP932 |  |  | FPCSG00912 |  | ported |
| PCSG00921 | JP | 下天の華 with 夢灯り 愛蔵版 | [0x80129BE0](../third_party/LunaTranslator/vita3k_1.cpp#L1040) | 0x125BE0 |  | r0 | 0 | CP932 | FULL_STRING | PCSG01124 | FPCSG01124 | 下天の華 | ported |
| PCSG00921 | JP | 下天の華 with 夢灯り 愛蔵版 | [0x8014C56C](../third_party/LunaTranslator/vita3k_1.cpp#L1041) | 0x14856C |  | r0 | 0 | CP932 | FULL_STRING | PCSG01124 | FPCSG01124 | 下天の華 with 夢灯り | ported |
| PCSG00935 | JP | BLACK WOLVES SAGA  -Weiβ und Schwarz- | [0x800581A2](../third_party/LunaTranslator/vita3k_1.cpp#L1144) | 0x541A2 |  | r0 | 0 | UTF-8 |  |  | PCSG00766 | text | ported |
| PCSG00935 | JP | BLACK WOLVES SAGA  -Weiβ und Schwarz- | [0x800644F6](../third_party/LunaTranslator/vita3k_1.cpp#L1145) | 0x604F6 |  | r8 | 0 | UTF-8 |  |  | PCSG00935 |  | ported |
| PCSG00935 | JP | BLACK WOLVES SAGA  -Weiβ und Schwarz- | [0x800259DC](../third_party/LunaTranslator/vita3k_1.cpp#L1146) | 0x219DC |  | r1 | 0 | UTF-8 |  |  | PCSG00935_2 | 过场prolog (cutscene prologue) | ported |
| PCSG00938 | JP | ワンド オブ フォーチュン Ｒ２ ～時空に沈む黙示録～ | [0x8006C986](../third_party/LunaTranslator/vita3k_1.cpp#L1313) | 0x68986 |  | r0 | 0 | UTF-8 |  |  | PCSG00938 |  | ported |
| PCSG00938 | JP | ワンド オブ フォーチュン Ｒ２ ～時空に沈む黙示録～ | [0x8006C9B0](../third_party/LunaTranslator/vita3k_1.cpp#L1314) | 0x689B0 |  | r0 | 0 | UTF-8 |  |  | PCSG00938 |  | ported |
| PCSG00938 | JP | ワンド オブ フォーチュン Ｒ２ ～時空に沈む黙示録～ | [0x8001A860](../third_party/LunaTranslator/vita3k_1.cpp#L1315) | 0x16860 |  | r8 | 0 | UTF-8 |  |  | PCSG00938 |  | ported |
| PCSG00938 | JP | ワンド オブ フォーチュン Ｒ２ ～時空に沈む黙示録～ | [0x80022BD2](../third_party/LunaTranslator/vita3k_1.cpp#L1316) | 0x1EBD2 |  | r4 | 0x14 | UTF-8 |  |  | PCSG00938 |  | ported |
| PCSG00938 | JP | ワンド オブ フォーチュン Ｒ２ ～時空に沈む黙示録～ | [0x80022BF0](../third_party/LunaTranslator/vita3k_1.cpp#L1317) | 0x1EBF0 |  | r5 | 0 | UTF-8 |  |  | PCSG00938 |  | ported |
| PCSG00944 | JP | 白と黒のアリス | [0x80039656](../third_party/LunaTranslator/vita3k_1.cpp#L1077) | 0x35656 |  | r0 | 0 | UTF-8 |  |  | FPCSG01066 |  | ported |
| PCSG00944 | JP | 白と黒のアリス | [0x80012DFE](../third_party/LunaTranslator/vita3k_1.cpp#L1078) | 0xEDFE |  | r0 | 0 | UTF-8 |  |  | FPCSG01066 |  | ported |
| PCSG00991 | JP | 月影の鎖　～狂爛モラトリアム～ | [0x8007AA94](../third_party/LunaTranslator/vita3k_1.cpp#L1281) | 0x76A94 |  | r1 | 0 | CP932 |  |  | F010052300F612000 |  | ported |
| PCSG00992 | JP | 遙かなる時空の中で３ Ultimate | [0x800A927E](../third_party/LunaTranslator/vita3k_1.cpp#L1059) | 0xA527E |  | r1 | 0 | CP932 | FULL_STRING | PCSG00992 |  |  | ported |
| PCSG01001 | JP | Side Kicks! | [0x80020C78](../third_party/LunaTranslator/vita3k_1.cpp#L1119) | 0x1CC78 |  | r0 | 0x8 | UTF-8 |  |  | PCSG01001 |  | ported |
| PCSG01002 | JP | ダンジョンに出会いを求めるのは間違っているだろうか インフィニト・コンバーテ | [0x8232D5C0](../third_party/LunaTranslator/vita3k_1.cpp#L1430) | 0x23295C0 |  | r2 | 0 | UTF-16LE | FULL_STRING |  | PCSG01002 |  | ported |
| PCSG01008 | JP | Marginal #4 Road to Galaxy | [0x8002FF90](../third_party/LunaTranslator/vita3k_1.cpp#L1140) | 0x2BF90 |  | r0 | 0 | UTF-8 |  |  | PCSG00766 | text | ported |
| PCSG01011 | JP | 空蝉の廻 | [0x82535242](../third_party/LunaTranslator/vita3k_1.cpp#L1276) | 0x2531242 |  | r1 | 0 | UTF-16LE | USING_CHAR DATA_INDIRECT |  |  | 后缀有人名，需要额外过滤 (a name at the end, needs more filtering) | refused: USING_CHAR: one character per hit, which Luna's text thread joins into a sentence across hits |
| PCSG01011 | JP | 空蝉の廻 | [0x801AE35A](../third_party/LunaTranslator/vita3k_1.cpp#L1277) | 0x1AA35A |  | r7 | 0 | UTF-8 |  | PCSG01011 |  |  | ported |
| PCSG01014 | JP | ニル・アドミラリの天秤 クロユリ炎陽譚 | [0x80030CC8](../third_party/LunaTranslator/vita3k_1.cpp#L1109) | 0x2CCC8 |  | r5 | 0 | CP932 |  |  | PCSG00766 |  | ported |
| PCSG01014 | JP | ニル・アドミラリの天秤 クロユリ炎陽譚 | [0x80074F2A](../third_party/LunaTranslator/vita3k_1.cpp#L1110) | 0x70F2A |  | r0 | 0 | CP932 |  |  | PCSG00766 |  | ported |
| PCSG01019 | JP | 罪喰い～千の呪い、千の祈り～ for V | [0x80080CD0](../third_party/LunaTranslator/vita3k_1.cpp#L1351) | 0x7CCD0 |  | r0 | 0 | CP932 |  |  |  |  | ported |
| PCSG01019 | JP | 罪喰い～千の呪い、千の祈り～ for V | [0x8001C73E](../third_party/LunaTranslator/vita3k_1.cpp#L1352) | 0x1873E |  | r1 | 0 | CP932 |  |  |  |  | ported |
| PCSG01023 | JP | 追放選挙 | [0x8002E176](../third_party/LunaTranslator/vita3k_1.cpp#L1125) | 0x2A176 |  | r0 | 0 | CP932 | FULL_STRING |  | FPCSG01023 |  | ported |
| PCSG01023 | JP | 追放選挙 | [0x8003B256](../third_party/LunaTranslator/vita3k_1.cpp#L1126) | 0x37256 |  | r0 | 0 | CP932 | FULL_STRING |  | FPCSG01023 |  | ported |
| PCSG01023 | JP | 追放選挙 | [0x8003B4A0](../third_party/LunaTranslator/vita3k_1.cpp#L1127) | 0x374A0 |  | r0 | 0 | CP932 | FULL_STRING |  | FPCSG01023 |  | ported |
| PCSG01025 | JP | ツキトモ。 TSUKIUTA. １２memories | [0x8004D078](../third_party/LunaTranslator/vita3k_1.cpp#L1383) | 0x49078 |  | r0 | 0 | UTF-8 |  |  | NewLineCharFilterA |  | ported |
| PCSG01034 | JP | 戦極姫７～戦雲つらぬく紅蓮の遺志～ | [0x8276B2CE](../third_party/LunaTranslator/vita3k_1.cpp#L1511) | 0x27672CE |  | r9 | 0 | UTF-16LE |  |  | PCSG01034 |  | ported |
| PCSG01036 | JP | 緋色の欠片 ～おもいいろの記憶～ | [0x8007838C](../third_party/LunaTranslator/vita3k_1.cpp#L1344) | 0x7438C |  | r5 | 0 | UTF-8 |  |  | PCSG01036 |  | ported |
| PCSG01036 | JP | 緋色の欠片 ～おもいいろの記憶～ | [0x8001154C](../third_party/LunaTranslator/vita3k_1.cpp#L1345) | 0xD54C |  | r8 | 0 | UTF-8 |  |  | PCSG01036 |  | ported |
| PCSG01036 | JP | 緋色の欠片 ～おもいいろの記憶～ | [0x800879EE](../third_party/LunaTranslator/vita3k_1.cpp#L1346) | 0x839EE |  | r2 | 0 | UTF-8 |  |  | PCSG01036 |  | ported |
| PCSG01046 | JP | 嘘月シャングリラ | [0x81E1F5C8](../third_party/LunaTranslator/vita3k_1.cpp#L1348) | 0x1E1B5C8 |  | r0 | 0 | UTF-16LE |  | PCSG01046 |  |  | ported |
| PCSG01046 | JP | 嘘月シャングリラ | [0x81E4A514](../third_party/LunaTranslator/vita3k_1.cpp#L1349) | 0x1E46514 |  | r0 | 0 | UTF-16LE |  | PCSG01046 |  |  | ported |
| PCSG01049 | JP | TOKYOヤマノテBOYS for V FAN DISC | [0x81F38A40](../third_party/LunaTranslator/vita3k_1.cpp#L1073) | 0x1F34A40 |  | r0 | 0xc | UTF-16LE | FULL_STRING |  | PCSG01049 |  | ported |
| PCSG01063 | JP | イケメン戦国◆時をかける恋　新たなる出逢い | [0x8009B496](../third_party/LunaTranslator/vita3k_1.cpp#L1105) | 0x97496 |  | r8 | 0 | UTF-8 |  |  | PCSG01063 |  | ported |
| PCSG01065 | JP | 学園CLUB ～ヒミツのナイトクラブ～ | [0x80029854](../third_party/LunaTranslator/vita3k_1.cpp#L1367) | 0x25854 |  | r1 | 0 | CP932 |  |  | FPCSG00852 |  | ported |
| PCSG01066 | JP | 蝶々事件ラブソディック | [0x8008DEA2](../third_party/LunaTranslator/vita3k_1.cpp#L1212) | 0x89EA2 |  | r4 | 0 | UTF-8 |  |  | FPCSG01066 |  | ported |
| PCSG01066 | JP | 蝶々事件ラブソディック | [0x8008EB38](../third_party/LunaTranslator/vita3k_1.cpp#L1213) | 0x8AB38 |  | r0 | 0 | UTF-8 |  |  | FPCSG01066 |  | ported |
| PCSG01068 | JP | ネオ アンジェリーク 天使の涙 | [0x8005426C](../third_party/LunaTranslator/vita3k_1.cpp#L1340) | 0x5026C |  | r0 | 0 | UTF-8 |  |  | PCSG01068 |  | ported |
| PCSG01075 | JP | 悠久のティアブレイド -Fragments of Memory- | [0x80035F44](../third_party/LunaTranslator/vita3k_1.cpp#L1200) | 0x31F44 |  | r10 | 0 | UTF-8 |  |  | FPCSG01075 |  | ported |
| PCSG01075 | JP | 悠久のティアブレイド -Fragments of Memory- | [0x8000D868](../third_party/LunaTranslator/vita3k_1.cpp#L1201) | 0x9868 |  | r9 | 0 | UTF-8 |  |  | FPCSG01075 |  | ported |
| PCSG01075 | JP | 悠久のティアブレイド -Fragments of Memory- | [0x8004598E](../third_party/LunaTranslator/vita3k_1.cpp#L1202) | 0x4198E |  | r0 | 0 | UTF-8 |  |  | FPCSG01075 |  | ported |
| PCSG01075 | JP | 悠久のティアブレイド -Fragments of Memory- | [0x801B1D16](../third_party/LunaTranslator/vita3k_1.cpp#L1203) | 0x1ADD16 |  | r9 | 0 | UTF-8 |  |  | FPCSG01075 |  | ported |
| PCSG01075 | JP | 悠久のティアブレイド -Fragments of Memory- | [0x801AC31E](../third_party/LunaTranslator/vita3k_1.cpp#L1204) | 0x1A831E |  | r9 | 0 | UTF-8 |  |  | FPCSG01075 |  | ported |
| PCSG01075 | JP | 悠久のティアブレイド -Fragments of Memory- | [0x801AC33A](../third_party/LunaTranslator/vita3k_1.cpp#L1205) | 0x1A833A |  | r7 | 0 | UTF-8 |  |  | FPCSG01075 |  | ported |
| PCSG01075 | JP | 悠久のティアブレイド -Fragments of Memory- | [0x801B879A](../third_party/LunaTranslator/vita3k_1.cpp#L1206) | 0x1B479A |  | r5 | 0 | UTF-8 |  |  | FPCSG01075 |  | ported |
| PCSG01075 | JP | 悠久のティアブレイド -Fragments of Memory- | [0x8009F570](../third_party/LunaTranslator/vita3k_1.cpp#L1207) | 0x9B570 |  | r5 | 0 | UTF-8 |  |  | FPCSG01075 |  | ported |
| PCSG01081 | JP | うたの☆プリンスさまっ♪Amazing Aria & Sweet Serenade LOVE | [0x80052B34](../third_party/LunaTranslator/vita3k_1.cpp#L1517) | 0x4EB34 |  | r0 | 0x24 | CP932 |  | PCSG00595 |  |  | refused: hookfunc PCSG00595: creates a new hook: a DIRECT_READ that Luna's reader thread polls every 500 ms, with a duplicate check (a hook that spawns another is not ported) |
| PCSG01087 | JP | キミの瞳にヒットミー | [0x8006080E](../third_party/LunaTranslator/vita3k_1.cpp#L1490) | 0x5C80E |  | r0 | 0 | CP932 |  |  | PCSG01254 |  | ported |
| PCSG01092 | JP | ひめひび　-Princess Days- | [0x80C5A6EA](../third_party/LunaTranslator/vita3k_1.cpp#L1474) | 0xC566EA |  | r4 | 0 | CP932 |  |  | PCSG01144_1 |  | ported |
| PCSG01092 | JP | ひめひび　-Princess Days- | [0x80020EA0](../third_party/LunaTranslator/vita3k_1.cpp#L1475) | 0x1CEA0 |  | r1 | 0 | CP932 |  |  | PCSG01144 |  | ported |
| PCSG01094 | JP | アイドリッシュセブン　Twelve Fantasia! | [0x8011E570](../third_party/LunaTranslator/vita3k_1.cpp#L1385) | 0x11A570 |  | r4 | 0 | UTF-8 |  |  |  | 或0x8011e580。映射地址会发生切换，导致用特殊码搜索搜不到，且有好几个杂乱线程，但我也不想改框架了，很烦 (or 0x8011e580; the mapped address changes, so a search cannot find it, and there are several noisy threads) | ported |
| PCSG01096 | JP | 猛獣たちとお姫様 ～in blossom～ | [0x8001DD9E](../third_party/LunaTranslator/vita3k_1.cpp#L1496) | 0x19D9E |  | r0 | 0 | UTF-8 |  |  |  |  | ported |
| PCSG01110 | JP | Code:Realize ～白銀の奇跡～ | [0x80015BCC](../third_party/LunaTranslator/vita3k_1.cpp#L1240) | 0x11BCC |  | r0 | 0x1c | UTF-8 |  |  | F010088B01A8FC000 |  | ported |
| PCSG01110 | JP | Code:Realize ～白銀の奇跡～ | [0x80038E76](../third_party/LunaTranslator/vita3k_1.cpp#L1241) | 0x34E76 |  | r8 | 0 | UTF-8 |  |  | F010088B01A8FC000 |  | ported |
| PCSG01114 | JP | ワールドエンド・シンドローム | [0x80029E5C](../third_party/LunaTranslator/vita3k_1.cpp#L1390) | 0x25E5C |  | r4 | 0 | UTF-8 |  |  | PCSG01114 |  | ported |
| PCSG01122 | JP | D.S.-Dal Segno- | [0x80014AEC](../third_party/LunaTranslator/vita3k_1.cpp#L1428) | 0x10AEC |  | r0 | 0 | CP932 |  |  |  |  | ported |
| PCSG01124 | JP | 金色のコルダ２ff フォルテッシモ | [0x80202CA6](../third_party/LunaTranslator/vita3k_1.cpp#L1045) | 0x1FECA6 |  | r0 | 0 | CP932 | FULL_STRING | PCSG01124 | FPCSG01124 |  | ported |
| PCSG01127 | JP | 千の刃濤、桃花染の皇姫 | [0x802D44D4](../third_party/LunaTranslator/vita3k_1.cpp#L1405) | 0x2D04D4 |  | r6 | 0 | UTF-8 |  |  |  | 有注音时有乱码 (garbled where there is ruby) | ported |
| PCSG01139 | JP | ピオフィオーレの晩鐘 | [0x80034656](../third_party/LunaTranslator/vita3k_1.cpp#L1403) | 0x30656 |  | r0 | 0 | UTF-8 |  |  |  |  | ported |
| PCSG01144 | JP | ひめひび 続！二学期-New Princess Days!!- | [0x815F96EA](../third_party/LunaTranslator/vita3k_1.cpp#L1477) | 0x15F56EA |  | r4 | 0 | CP932 |  |  | PCSG01144_1 |  | ported |
| PCSG01144 | JP | ひめひび 続！二学期-New Princess Days!!- | [0x8000D3C2](../third_party/LunaTranslator/vita3k_1.cpp#L1478) | 0x93C2 |  | r8 | 0 | CP932 |  |  | PCSG01144 |  | ported |
| PCSG01150 | JP | CharadeManiacs | [0x800A0264](../third_party/LunaTranslator/vita3k_1.cpp#L1424) | 0x9C264 |  | r2 | 0 | UTF-8 |  |  | PCSG01150 |  | ported |
| PCSG01151 | JP | GALTIA V Edition | [0x8001B7AA](../third_party/LunaTranslator/vita3k_1.cpp#L1360) | 0x177AA |  | r0 | 0 | CP932 |  |  | PCSG01151 |  | ported |
| PCSG01152 | JP | Dance with Devils My Carol | [0x82303AB0](../third_party/LunaTranslator/vita3k_1.cpp#L1472) | 0x22FFAB0 |  | r0 | 0 | UTF-16LE |  |  | PCSG01152 | 会超前读取 (reads ahead) | refused: filter PCSG01152: keeps every other hit (a call counter: the game "reads ahead", Luna's note): one hit cannot tell which half it is |
| PCSG01153 | JP | カタハネ -An' call Belle- | [0x8005D784](../third_party/LunaTranslator/vita3k_1.cpp#L1455) | 0x59784 |  | r1 | 0 | CP932 |  |  | PCSG01198 |  | ported |
| PCSG01157 | JP | 遙かなる時空の中で Ultimate | [0x800608F0](../third_party/LunaTranslator/vita3k_1.cpp#L1056) | 0x5C8F0 |  | r4 | 0 | CP932 | FULL_STRING |  | NewLineCharFilterA |  | ported |
| PCSG01157 | JP | 遙かなる時空の中で Ultimate | [0x80060980](../third_party/LunaTranslator/vita3k_1.cpp#L1057) | 0x5C980 | 1.01 | r4 | 0 | CP932 | FULL_STRING |  | NewLineCharFilterA |  | ported |
| PCSG01158 | JP | 真紅の焔 真田忍法帳 | [0x80025064](../third_party/LunaTranslator/vita3k_1.cpp#L1279) | 0x21064 |  | r0 | 0 | UTF-8 |  |  | PCSG00833 |  | ported |
| PCSG01159 | JP | VARIABLE BARRICADE | [0x80031CDE](../third_party/LunaTranslator/vita3k_1.cpp#L1387) | 0x2DCDE |  | r6 | 0 | UTF-8 |  |  | FPCSG00855 |  | ported |
| PCSG01159 | JP | VARIABLE BARRICADE | [0x80038874](../third_party/LunaTranslator/vita3k_1.cpp#L1388) | 0x34874 |  | r6 | 0 | UTF-8 |  |  | FPCSG00855 |  | ported |
| PCSG01162 | JP | Collar×Malice -Unlimited- | [0x8000D3A4](../third_party/LunaTranslator/vita3k_1.cpp#L1426) | 0x93A4 |  | r0 | 0 | UTF-8 |  |  | PCSG00530 |  | ported |
| PCSG01167 | JP | 円環のメモーリア -カケラ灯し- | [0x80029AB2](../third_party/LunaTranslator/vita3k_1.cpp#L1338) | 0x25AB2 |  | r0 | 0 | CP932 |  |  | PCSG01167 |  | ported |
| PCSG01178 | JP | ナデレボ！ | [0x80043D1A](../third_party/LunaTranslator/vita3k_1.cpp#L1494) | 0x3FD1A |  | r0 | 0 | CP932 |  |  | PCSG01178 |  | ported |
| PCSG01180 | JP | 天涯ニ舞ウ、粋ナ花 | [0x8006808E](../third_party/LunaTranslator/vita3k_1.cpp#L1187) | 0x6408E |  | r0 | 0 | UTF-8 |  |  | FPCSG01180 |  | ported |
| PCSG01180 | JP | 天涯ニ舞ウ、粋ナ花 | [0x80089408](../third_party/LunaTranslator/vita3k_1.cpp#L1188) | 0x85408 |  | r0 | 0 | UTF-8 |  |  | FPCSG01180 |  | ported |
| PCSG01192 | JP | 白と黒のアリス -Twilight line- | [0x80013B9A](../third_party/LunaTranslator/vita3k_1.cpp#L1080) | 0xFB9A |  | r0 | 0 | UTF-8 |  |  | FPCSG01066 |  | ported |
| PCSG01192 | JP | 白と黒のアリス -Twilight line- | [0x800381E6](../third_party/LunaTranslator/vita3k_1.cpp#L1081) | 0x341E6 |  | r0 | 0 | UTF-8 |  |  | FPCSG01066 |  | ported |
| PCSG01194 | JP | 参千世界遊戯 ~MultiUniverse Myself~ | [0x8005AE24](../third_party/LunaTranslator/vita3k_1.cpp#L1138) | 0x56E24 |  | r0 | 0 | CP932 |  |  |  | dialouge+name,sjis,need remap jis char,to complex | ported |
| PCSG01196 | JP | sweet pool | [0x8003D5B6](../third_party/LunaTranslator/vita3k_1.cpp#L1365) | 0x395B6 |  | r0 | 0 | CP932 |  |  | PCSG01196 |  | ported |
| PCSG01197 | JP | Cafe Cuillere ～カフェ キュイエール～ | [0x8000DF58](../third_party/LunaTranslator/vita3k_1.cpp#L1421) | 0x9F58 |  | r0 | 0 | CP932 |  |  | PCSG01197 |  | ported |
| PCSG01197 | JP | Cafe Cuillere ～カフェ キュイエール～ | [0x800104FC](../third_party/LunaTranslator/vita3k_1.cpp#L1422) | 0xC4FC |  | r1 | 0 | CP932 |  |  | PCSG01197 |  | ported |
| PCSG01198 | JP | アマツツミ | [0x800785C8](../third_party/LunaTranslator/vita3k_1.cpp#L1419) | 0x745C8 |  | r2 | 0 | CP932 |  |  | PCSG01198 |  | ported |
| PCSG01202 | JP | フローラル・フローラブ | [0x80022C9C](../third_party/LunaTranslator/vita3k_1.cpp#L1439) | 0x1EC9C |  | r7 | 0 | CP932 |  |  | PCSG01202 |  | ported |
| PCSG01203 | JP | タユタマ２ -you're the only one- | [0x8004C0CA](../third_party/LunaTranslator/vita3k_1.cpp#L1509) | 0x480CA |  | r0 | 0 | CP932 |  |  | PCSG01203 |  | ported |
| PCSG01204 | JP | NG | [0x80063E54](../third_party/LunaTranslator/vita3k_1.cpp#L1413) | 0x5FE54 |  | r0 | 0 | CP932 |  |  | PCSG01204 |  | ported |
| PCSG01208 | JP | ワンド オブ フォーチュン Ｒ２ ＦＤ ～君に捧げるエピローグ～ | [0x80035B80](../third_party/LunaTranslator/vita3k_1.cpp#L1319) | 0x31B80 |  | r6 | 0 | UTF-8 |  |  | PCSG00855 |  | ported |
| PCSG01211 | JP | 金色のコルダ３ フルボイス Special | [0x801F64CE](../third_party/LunaTranslator/vita3k_1.cpp#L1051) | 0x1F24CE |  | r0 | 0 | CP932 | FULL_STRING | PCSG01124 | FPCSG01124 |  | ported |
| PCSG01212 | JP | 金色のコルダ３ AnotherSky feat.神南/至誠館/天音学園 | [0x8020BE6E](../third_party/LunaTranslator/vita3k_1.cpp#L1047) | 0x207E6E |  | r0 | 0 | CP932 | FULL_STRING | PCSG01124 | FPCSG01124 | 神南 | ported |
| PCSG01212 | JP | 金色のコルダ３ AnotherSky feat.神南/至誠館/天音学園 | [0x8020E876](../third_party/LunaTranslator/vita3k_1.cpp#L1048) | 0x20A876 |  | r0 | 0 | CP932 | FULL_STRING | PCSG01124 | FPCSG01124 | 至誠館 | ported |
| PCSG01212 | JP | 金色のコルダ３ AnotherSky feat.神南/至誠館/天音学園 | [0x80218E4A](../third_party/LunaTranslator/vita3k_1.cpp#L1049) | 0x214E4A |  | r0 | 0 | CP932 | FULL_STRING | PCSG01124 | FPCSG01124 | 天音学園 | ported |
| PCSG01213 | JP | 古書店街の橋姫 々 | [0x8000C74A](../third_party/LunaTranslator/vita3k_1.cpp#L1492) | 0x874A |  | r0 | 0 | CP932 |  |  |  |  | ported |
| PCSG01218 | JP | グリザイア ファントムトリガー ０３＆０４ | [0x80052EFE](../third_party/LunaTranslator/vita3k_1.cpp#L1450) | 0x4EEFE |  | r4 | 0 | CP932 |  |  | PCSG01289 |  | ported |
| PCSG01221 | JP | 紅色天井艶妖綺譚 二藍 | [0x8003F554](../third_party/LunaTranslator/vita3k_1.cpp#L1245) | 0x3B554 |  | r4 | 0 | CP932 |  |  | FPCSG00852 |  | ported |
| PCSG01235 | JP | ノラと皇女と野良猫ハート2 | [0x80027620](../third_party/LunaTranslator/vita3k_1.cpp#L1409) | 0x23620 |  | r11 | 0 | UTF-8 |  | PCSG01235 |  | 开头少十几句 (the first dozen or so lines missing) | ported |
| PCSG01245 | JP | 金色のコルダ オクターヴ | [0x8000854A](../third_party/LunaTranslator/vita3k_1.cpp#L1043) | 0x454A |  | r0 | 0 | CP932 | FULL_STRING | PCSG01124 | FPCSG01124 |  | ported |
| PCSG01247 | JP | Panic Palette～パニック パレット～ | [0x80010C96](../third_party/LunaTranslator/vita3k_1.cpp#L1407) | 0xCC96 |  | r0 | 0 | CP932 |  |  | PCSG01247 |  | ported |
| PCSG01249 | JP | EVE rebirth terror | [0x80030CAA](../third_party/LunaTranslator/vita3k_1.cpp#L1441) | 0x2CCAA |  | r1 | 0 | CP932 |  |  | PCSG01249 |  | ported |
| PCSG01250 | JP | スキとスキとでサンカク恋愛 | [0x80013ED6](../third_party/LunaTranslator/vita3k_1.cpp#L1397) | 0xFED6 |  | r4 | 0 | CP932 |  |  | PCSG01250_T |  | ported |
| PCSG01250 | JP | スキとスキとでサンカク恋愛 | [0x80013EE6](../third_party/LunaTranslator/vita3k_1.cpp#L1398) | 0xFEE6 |  | r4 | 0 | CP932 |  |  | PCSG01250_N |  | ported |
| PCSG01253 | JP | かりぐらし恋愛 | [0x800444CC](../third_party/LunaTranslator/vita3k_1.cpp#L1452) | 0x404CC |  | r0 | 0 | CP932 |  |  | PCSG00530 |  | ported |
| PCSG01253 | JP | かりぐらし恋愛 | [0x800443B8](../third_party/LunaTranslator/vita3k_1.cpp#L1453) | 0x403B8 |  | r0 | 0 | CP932 |  |  | PCSG00530 |  | ported |
| PCSG01254 | JP | 添いカノ ～ぎゅっと抱きしめて～ | [0x8007D250](../third_party/LunaTranslator/vita3k_1.cpp#L1392) | 0x79250 |  | r0 | 0 | CP932 |  |  | PCSG01254 |  | ported |
| PCSG01259 | JP | シルヴァリオ トリニティ -Beyond the Horizon- | [0x800B7702](../third_party/LunaTranslator/vita3k_1.cpp#L1123) | 0xB3702 |  | r3 | 0 | CP932 |  |  |  |  | ported |
| PCSG01260 | JP | フルキス | [0x801B28C8](../third_party/LunaTranslator/vita3k_1.cpp#L1443) | 0x1AE8C8 |  | r5 | 0 | UTF-8 |  |  | PCSG01260 |  | ported |
| PCSG01260 | JP | フルキス | [0x801B1FC0](../third_party/LunaTranslator/vita3k_1.cpp#L1444) | 0x1ADFC0 |  | r0 | 0 | UTF-8 |  |  | PCSG01260_T |  | ported |
| PCSG01266 | JP | 夢現Re:Master | [0x8000CD76](../third_party/LunaTranslator/vita3k_1.cpp#L1466) | 0x8D76 |  | r2 | 0 | UTF-8 |  |  |  |  | ported |
| PCSG01267 | JP | your diary + | [0x800482CE](../third_party/LunaTranslator/vita3k_1.cpp#L1462) | 0x442CE |  | r0 | 0 | CP932 |  |  | FPCSG00815 |  | ported |
| PCSG01268 | JP | 神凪ノ杜 五月雨綴り | [0x828BB50C](../third_party/LunaTranslator/vita3k_1.cpp#L1134) | 0x28B750C |  | r0 | 0 | UTF-16LE |  | ReadU16TextAndLenDW |  | dialogue | ported |
| PCSG01268 | JP | 神凪ノ杜 五月雨綴り | [0x828BA9B6](../third_party/LunaTranslator/vita3k_1.cpp#L1135) | 0x28B69B6 |  | r0 | 0 | UTF-16LE |  | ReadU16TextAndLenDW |  | name | ported |
| PCSG01268 | JP | 神凪ノ杜 五月雨綴り | [0x8060D376](../third_party/LunaTranslator/vita3k_1.cpp#L1136) | 0x609376 |  | r0 | 0 | UTF-8 |  |  |  | vita3k v0.2.0 can't find 0x828bb50c && 0x828ba9b6, unknown reason. | ported |
| PCSG01282 | JP | 死神と少女 | [0x800204BA](../third_party/LunaTranslator/vita3k_1.cpp#L1129) | 0x1C4BA |  | r2 | 0 | CP932 |  |  | FPCSG01282<0> | dialogueNVL,sjis | ported |
| PCSG01282 | JP | 死神と少女 | [0x8000F00E](../third_party/LunaTranslator/vita3k_1.cpp#L1130) | 0xB00E |  | r1 | 0 | CP932 |  |  | FPCSG01282<1> | dialogue main | ported |
| PCSG01282 | JP | 死神と少女 | [0x80011F1A](../third_party/LunaTranslator/vita3k_1.cpp#L1131) | 0xDF1A |  | r0 | 0 | CP932 |  |  | FPCSG01282<2> | Name | ported |
| PCSG01282 | JP | 死神と少女 | [0x8001EBAC](../third_party/LunaTranslator/vita3k_1.cpp#L1132) | 0x1ABAC |  | r1 | 0 | CP932 |  |  | FPCSG01282<3> | choices | ported |
| PCSG01283 | JP | アオナツライン | [0x8031E464](../third_party/LunaTranslator/vita3k_1.cpp#L1446) | 0x31A464 |  | r5 | 0 | UTF-8 |  |  | PCSG01314 |  | ported |
| PCSG01284 | JP | Making*Lovers | [0x808E1530](../third_party/LunaTranslator/vita3k_1.cpp#L1415) | 0x8DD530 |  | r6 | 0 | CP932 |  |  | PCSG01284 |  | ported |
| PCSG01289 | JP | 軍靴をはいた猫 | [0x800647D0](../third_party/LunaTranslator/vita3k_1.cpp#L1448) | 0x607D0 |  | r1 | 0 | CP932 |  |  | PCSG01289 |  | ported |
| PCSG01297 | JP | IxSHE Tell | [0x80027EB0](../third_party/LunaTranslator/vita3k_1.cpp#L1411) | 0x23EB0 |  | r11 | 0 | UTF-8 |  | PCSG01235 |  | 少很多句 (many lines missing) | ported |
| PCSG01314 | JP | LOVE CLEAR | [0x8031BFA2](../third_party/LunaTranslator/vita3k_1.cpp#L1417) | 0x317FA2 |  | r5 | 0 | UTF-8 |  |  | PCSG01314 |  | ported |
| PCSG01318 | JP | 金色ラブリッチェ | [0x800154CC](../third_party/LunaTranslator/vita3k_1.cpp#L1457) | 0x114CC |  | r0 | 0 | CP932 |  |  | PCSG01250_N<0> |  | ported |
| PCSG01318 | JP | 金色ラブリッチェ | [0x800158B8](../third_party/LunaTranslator/vita3k_1.cpp#L1458) | 0x118B8 |  | r4 | 0 | CP932 |  |  | PCSG01250_T |  | ported |
| PCSG01325 | JP | アイキス | [0x80322DDA](../third_party/LunaTranslator/vita3k_1.cpp#L1394) | 0x31EDDA |  | r5 | 0 | UTF-8 |  |  | PCSG01325 |  | ported |
| PCSG01325 | JP | アイキス | [0x801B8F9A](../third_party/LunaTranslator/vita3k_1.cpp#L1395) | 0x1B4F9A |  | r5 | 0 | UTF-8 |  |  | PCSG01325 |  | ported |

Left out of the table: the entries whose address and title ID an earlier entry already has (Luna's map keeps the earlier one, whose parameters the site has):

| Title ID | Game | Address | Line | Kept: line |
| --- | --- | --- | --- | --- |
| PCSG00917 | ワンドオブフォーチュン R | 0x800345F0 | [1307](../third_party/LunaTranslator/vita3k_1.cpp#L1307) | [1296](../third_party/LunaTranslator/vita3k_1.cpp#L1296) |
| PCSG00917 | ワンドオブフォーチュン R | 0x8002CC56 | [1308](../third_party/LunaTranslator/vita3k_1.cpp#L1308) | [1297](../third_party/LunaTranslator/vita3k_1.cpp#L1297) |

Title IDs whose sites Luna's comments name more than one game for:

- PCSG00401: ソラユメ; ソラ*ユメ
- PCSG00611: 絶対迷宮　秘密のおやゆび姫; 絶対迷宮 秘密のおやゆび姫
- PCSG00745: スカーレッドライダーゼクス Rev.; スカーレッドライダーゼクス
- PCSG00917: Starry☆Sky ~Autumn Stories~ / Starry Sky ~Autumn Stories~; ワンドオブフォーチュン R

### Sites at Vita3K's guest address

Luna recorded these before its `addr_offset` (f237a9f4, 2026-05-20), at Vita3K's guest address, in an executable that a public Vita3K log (Vita3K/compatibility; a Vita3K before 11dc3b8b, which loaded only a fixed-address executable at its link address) shows as fixed-address at 0x81000000: `VJO_LUNA_F_GUEST`, the only sites read at the address − 0x81000000 (#349). The other sites at or above 0x81000000 are read in Luna's space only, as on main: 8 are in relocatable executables, where such an address is another module's (PCSG00708's is in its Unity Mono AOT module); PCSG00614's executable is fixed-address but linked at 0x810007C0, and its other entry is the same site in Luna's space; 9 fit segment 0 in neither reading; PCSG00020 and PCSG01144 have no public log.

| Title ID | Game | Address | Guest offset | Evidence |
| --- | --- | --- | --- | --- |
| PCSG00062 | TIME TRAVELERS | 0x8119ADDA | 0x19ADDA | [#400](https://github.com/Vita3K/compatibility/issues/400): `tt1_rom` at 0x81000000, 0x31258C bytes (1.00, Vita3K 0.1.8 3217); 01.01, another build: `eboot`, 0x306C90 bytes (0.1 1403) |
| PCSG00062 | TIME TRAVELERS | 0x811A6052 | 0x1A6052 | the same |
| PCSG00139 | フォトカノ Kiss | 0x81161B0E | 0x161B0E | [#222](https://github.com/Vita3K/compatibility/issues/222), [#469](https://github.com/Vita3K/compatibility/issues/469): `GMV_Rom` at 0x81000000, 0x583FD8 bytes (1.00, 0.1.4 2408), 0x5A0398 (1.04, 0.1.3 2265 to 0.1.6 2897) |
| PCSG00172 | 神咒神威神楽 | 0x810A2486 | 0xA2486 | [#1825](https://github.com/Vita3K/compatibility/issues/1825): `malievita` at 0x81000000, 0x150554 bytes (1.00, 0.1.8 3242); the psandvita farm hooked 0x810A2486 there in Vita3K and read the game's lines |
| PCSG00216 | るいは智を呼ぶ | 0x81003DB0 | 0x3DB0 | [#1018](https://github.com/Vita3K/compatibility/issues/1018): `advengine` at 0x81000000, 0x16446C bytes (1.00, 0.1.6 2895) |
| PCSG00272 | DIABOLIK LOVERS LIMITED V EDITION | 0x8105A176 | 0x5A176 | [#452](https://github.com/Vita3K/compatibility/issues/452), [#2517](https://github.com/Vita3K/compatibility/issues/2517): `eboot` at 0x81000000, 0x1F0554 bytes (1.00, 0.1 1825 and 0.2.0 3596; the log attached to [#2478](https://github.com/Vita3K/compatibility/issues/2478) is another game's, PCSG00476) |
| PCSG00272 | DIABOLIK LOVERS LIMITED V EDITION | 0x8103416C | 0x3416C | the same |

## Rubi's own entry

| Title ID | Region | Game | Site | Encoding | Status |
| --- | --- | --- | --- | --- | --- |
| PCSG00415 | JP | CLANNAD | the CALL source (SceLibc `memcpy`/`strcpy`/`strncpy`/`memmove` imports of the game executable); the register site segment 0 + 0x2CA70 (Luna's space 0x80030A70), r6, module NID 0xC5187D60, 8-byte signature | CP932 (AUTO for the CALL source) | Rubi's own, not in Luna's table: the current CALL source was verified on the owner's Vita with the combined debug test build (2026-10-20); the register site was proven in Vita3K only (note 22) and remains unverified on a console; the normal release build has no reported console run |

## Hookfuncs and filters

| Luna's function | Kind | Sites | Port |
| --- | --- | ---: | --- |
| PCSB00985 | hookfunc | 1 | not ported: sorts its hits into two text threads by content (Luna's split value), which one site's stream cannot represent; the European release, in English |
| PCSG00595 | hookfunc | 2 | not ported: creates a new hook: a DIRECT_READ that Luna's reader thread polls every 500 ms, with a duplicate check (a hook that spawns another is not ported) |
| PCSG00769 | hookfunc | 1 | ported: back to the first of the NUL-separated strings around the pointer (at most the capture's size back), the strings concatenated; ㊤㊥㊦ become ―, 　 and newlines removed |
| PCSG00912 | hookfunc | 1 | ported: 47 FF FF searched in the 0x50 bytes at r1 (a hit at offset 0 counts as none, as in Luna's SearchPattern); the strings after it joined while 48 FF FF / 47 FF FF separate them |
| PCSG00914 | hookfunc | 1 | ported: the strings at the pointer + padding up to an empty one, each one's final newline dropped; ㌻ removed, ㌫ and ㍊ (the heroine's name glyphs) replaced by 七海 and 春歌 |
| PCSG00992 | hookfunc | 1 | ported: the strings at r1 up to an empty one, concatenated, newlines removed |
| PCSG01011 | hookfunc | 1 | ported: the string that holds r7's address (scanned back to its start, at most the capture's size); newlines removed and what precedes the first × cut; Luna's duplicate check dropped (Rubi reads the latest text) |
| PCSG01046 | hookfunc | 2 | ported: as ReadU16TextAndLenDW from r0, then <br> removed |
| PCSG01124 | hookfunc | 8 | ported: the 0xAA-marked strings after the register's pointer, concatenated; the escapes 1B B4 / 1B B6 (and 5 bytes) and 1B B5 (and 1) skipped |
| PCSG01235 | hookfunc | 2 | ported: the string 3 bytes before the register's pointer |
| ReadU16TextAndLenDW | hookfunc | 5 | ported: UTF-16 text at reg + 0xC, its length in units the 32-bit word at reg + 8 (NUL units removed, as LunaHost does) |
| TPCSG00291 | hookfunc | 1 | not ported: keeps state across calls: a counter that drops the hits after a PLAYER substitution, and a duplicate check (and decodes byte-swapped Shift-JIS) |
| TPCSG00903 | hookfunc | 1 | ported: UTF-8 text at r0 + 0x1C, its length the 32-bit word at r0 + 0x14 |
| F010052300F612000 | filter | 2 | ported: #rbase\|reading# and @rbase@n become base; \c and \n removed |
| F010088B01A8FC000 | filter | 3 | ported: #Cmd and #Cmd[..] removed; 　 removed on UTF-8 sites (Luna's UTF-8 pattern never matches CP932 text); Luna's duplicate check dropped |
| FPCSG00401 | filter | 1 | ported: spaces, \c and \n removed. Luna's second step, re::sub(s, R"(\c)"), is not a valid ECMAScript pattern (\c needs a letter): MSVC's std::regex throws, and TextHook::Send drops the hit, so this site most likely never showed text in Luna; the port removes the literal \c, which the pattern means (F010052300F612000 removes it with strReplace) |
| FPCSG00405 | filter | 1 | ported: spaces removed |
| FPCSG00410 | filter | 1 | ported: #Cmd[n], #Pos[..] removed, #n a space; ④②⑥⑪⑫⑤① become !? !! 。 【 】 、 ・・・ |
| FPCSG00448 | filter | 3 | ported: spaces and #n removed; #Cmd[n], #Pos[..] removed |
| FPCSG00468 | filter | 2 | ported: \n (and the 　 after it), \k, [, *..] and × removed (UTF-8 bytes) |
| FPCSG00477 | filter | 3 | ported: #n and the 　 after it removed; #X...] removed |
| FPCSG00696 | filter | 3 | ported: ㌔ and ㍉ removed |
| FPCSG00706 | filter | 3 | ported: <br> removed |
| FPCSG00751 | filter | 2 | ported: spaces, @x and ＄ removed |
| FPCSG00808 | filter | 5 | ported: trimmed; spaces and #n removed; #Cmd and #Cmd[..] removed |
| FPCSG00815 | filter | 6 | ported: spaces and #n removed; #Cmd and #Cmd[..] removed (UTF-8 bytes on UTF-8 sites) |
| FPCSG00839 | filter | 2 | ported: UTF-16 sites: [..]x, \k \x %C %B %p-1; and \n removed, #hex;text keeps text; Luna's duplicate check dropped. UTF-8 sites: nothing (Luna reads the UTF-8 bytes as UTF-16 units, where no pattern can match; it only loses an odd final byte) |
| FPCSG00852 | filter | 8 | ported: ^ removed |
| FPCSG00855 | filter | 7 | ported: #n and the 　 after it removed (CP932 sites: #n only, as Luna's UTF-8 　 never matches there); #X...] removed |
| FPCSG00855_2<0> | filter | 1 | ported: the game's stand-ins ΧΔΛ《》∫∨∴∵ПЦ become 、。っ（）「」『』【】, then as FPCSG00855; Luna's duplicate check dropped |
| FPCSG00855_2<1> | filter | 1 | ported: as FPCSG00855_2<0> |
| FPCSG00855_2<2> | filter | 1 | ported: as FPCSG00855_2<0> |
| FPCSG00855_2<3> | filter | 1 | ported: as FPCSG00855_2<0> |
| FPCSG00903 | filter | 1 | ported: \n becomes a space |
| FPCSG00912 | filter | 20 | ported: %N removed |
| FPCSG01023 | filter | 3 | ported: <br>, %CF11F, %CFFFF, %P, %K, %N (and the 　 after it) and newlines removed |
| FPCSG01066 | filter | 6 | ported: a newline and the 　 after it removed (UTF-8 bytes) |
| FPCSG01075 | filter | 8 | ported: as FPCSG00808; Luna's duplicate check dropped |
| FPCSG01124 | filter | 8 | ported: ESC + ｴ / ｵ removed; every 　 and newline removed |
| FPCSG01180 | filter | 2 | ported: \n becomes a space; from the first comma on, a space |
| FPCSG01282<0> | filter | 1 | ported: newlines become a space; @cmd, #..#, $ and a final digit removed; Luna's duplicate check dropped |
| FPCSG01282<1> | filter | 1 | ported: as FPCSG01282<0> |
| FPCSG01282<2> | filter | 1 | ported: as FPCSG01282<0> |
| FPCSG01282<3> | filter | 1 | ported: as FPCSG01282<0> |
| NewLineCharFilterA | filter | 17 | ported: newlines removed |
| NewLineCharFilterW | filter | 1 | ported: newlines removed |
| PCSG00020 | filter | 1 | ported: <tags> removed (UTF-8 bytes) |
| PCSG00062 | filter | 2 | ported: [base/reading] becomes base; <tags> removed; \n becomes a newline |
| PCSG00172 | filter | 1 | ported: stray strings around the text removed (file names, Mﾚ...); two of Luna's eight contain U+FFFD, Windows' stand-in for bytes it cannot decode, and cannot match here: the port drops such bytes |
| PCSG00272 | filter | 2 | ported: a name before #n# goes into 【】; then as PCSG00472 |
| PCSG00367 | filter | 4 | ported: \ and # removed |
| PCSG00370 | filter | 1 | ported: newline + 　, ▼, ＞ and newlines removed |
| PCSG00401 | filter | 1 | ported: spaces just inside 【】 trimmed |
| PCSG00402 | filter | 4 | ported: 　 and #n removed; #Cmd and #Cmd[..] removed |
| PCSG00431 | filter | 1 | ported: #n　 and #n removed; Luna then shows only what was added since its last hit (the line grows hit by hit): dropped, Rubi reads the whole latest text |
| PCSG00433 | filter | 1 | ported: newlines removed; <CLT n>text<CLT> becomes text |
| PCSG00449 | filter | 1 | ported: @w and 　 removed; only a text that starts with 【 is kept, but a final @ is cut off (and the text kept) either way, as Luna's code does |
| PCSG00451<0> | filter | 1 | not ported: keeps every second hit (a call counter): one hit cannot tell which half it is |
| PCSG00451<1> | filter | 1 | not ported: as PCSG00451<0> |
| PCSG00472 | filter | 4 | ported: ゞ ゝ (the game's !? !!) replaced, 　 and #n removed, #Cmd[n] removed |
| PCSG00482 | filter | 2 | ported: ▼ + form feed (its page end) and newlines removed; a ▼ alone kept |
| PCSG00530 | filter | 5 | ported: #n removed |
| PCSG00535_1 | filter | 1 | ported: \f removed, \n becomes a newline; Luna's duplicate check and the copy it keeps for the other site dropped |
| PCSG00535_2 | filter | 1 | ported: only a check against the other site's last text in Luna: dropped (each site is its own stream) |
| PCSG00543 | filter | 1 | ported: only a duplicate check in Luna: dropped (Rubi reads the latest text) |
| PCSG00585 | filter | 1 | ported: half-width katakana to hiragana; $[base$/reading$] becomes base, $C[ffffff]text@$C[] text; newlines removed |
| PCSG00592 | filter | 2 | ported: #Color[n] removed |
| PCSG00615 | filter | 1 | ported: ＿ removed |
| PCSG00654 | filter | 1 | ported: \ and $ removed |
| PCSG00664 | filter | 1 | ported: [n] and the 　 around it removed |
| PCSG00689 | filter | 1 | ported: only a duplicate check in Luna: dropped (Rubi reads the latest text) |
| PCSG00708 | filter | 2 | ported: \ and $ removed |
| PCSG00766 | filter | 8 | ported: a text that is one 　 dropped (UTF-8 sites only: Luna compares with the UTF-8 form); #Ruby[base,reading] becomes base; #n and the 　 around it removed; #Cmd[..] removed |
| PCSG00776 | filter | 2 | ported: STX (0x02) removed, then trimmed (Windows' iswspace: Unicode white space) |
| PCSG00780 | filter | 1 | ported: ^ removed; <base,reading> becomes base |
| PCSG00787 | filter | 1 | ported: \ removed |
| PCSG00826 | filter | 2 | ported: #n and 　 removed |
| PCSG00829 | filter | 1 | ported: \n　 and \n removed, [base,n] removed; Luna's duplicate check dropped |
| PCSG00833 | filter | 3 | ported: 　 removed (UTF-8 sites) |
| PCSG00855 | filter | 2 | ported: #n　 and #n removed |
| PCSG00917 | filter | 4 | ported: a 　 after anything but 。…？！ removed, a leading 　 and #n removed (UTF-8 bytes: as in Luna, the class [^。…？！] is a set of bytes, so a 　 after a character ending in one of them is kept) |
| PCSG00935 | filter | 1 | ported: a name after #n#Pos[..] moves to the front in 【】; then as PCSG00766 |
| PCSG00935_2 | filter | 1 | ported: as PCSG00766; Luna's duplicate check dropped |
| PCSG00938 | filter | 5 | ported: as PCSG00917, then #Cmd[n] and ! removed |
| PCSG01001 | filter | 1 | ported: @@ removed |
| PCSG01002 | filter | 1 | ported: newlines and <tags> removed |
| PCSG01034 | filter | 1 | ported: newlines removed; Luna's duplicate check dropped |
| PCSG01036 | filter | 3 | ported: #n removed; Luna's duplicate check dropped |
| PCSG01049 | filter | 1 | ported: <br>　*<br> becomes a newline; 　*<br>　* removed |
| PCSG01063 | filter | 1 | ported: newlines (and a 　 after one) removed |
| PCSG01068 | filter | 1 | ported: \\ becomes a newline, \ removed, a final ! after 。 removed |
| PCSG01114 | filter | 1 | ported: @ and {...} removed |
| PCSG01144 | filter | 3 | ported: \n and \c removed |
| PCSG01144_1 | filter | 3 | ported: @w, @y and 　 removed; only a text that starts with 【 is kept |
| PCSG01150 | filter | 1 | ported: as PCSG00855, then PCSG00592 |
| PCSG01151 | filter | 1 | ported: ^ and 　 removed |
| PCSG01152 | filter | 1 | not ported: keeps every other hit (a call counter: the game "reads ahead", Luna's note): one hit cannot tell which half it is |
| PCSG01167 | filter | 1 | ported: <reading>base\| becomes base |
| PCSG01178 | filter | 1 | ported: as FPCSG00855, then two spaces removed |
| PCSG01196 | filter | 1 | ported: ^ becomes a newline; <tags> removed |
| PCSG01197 | filter | 2 | ported: #rbase\|reading# becomes base; @w removed |
| PCSG01198 | filter | 2 | ported: half-width katakana to hiragana; `base@ becomes base, $[base$/reading$] base |
| PCSG01202 | filter | 1 | ported: /ruby:base&reading/ becomes base |
| PCSG01203 | filter | 1 | ported: #Ruby[n,reading] removed |
| PCSG01204 | filter | 1 | ported: newlines, @1r and @-1r removed |
| PCSG01247 | filter | 3 | ported: \n removed |
| PCSG01249 | filter | 2 | ported: [n] removed |
| PCSG01250_N | filter | 1 | ported: only <name ...> kept, its first 7 and last 2 bytes cut (CP932 bytes) |
| PCSG01250_N<0> | filter | 1 | ported: only <name ...> kept, its first 6 and last 1 bytes cut (CP932 bytes) |
| PCSG01250_T | filter | 2 | ported: only <text ...> kept, its first 6 and last 1 bytes cut (CP932 bytes, as Luna counts them); /ruby:base&reading/ becomes base; #n removed |
| PCSG01254 | filter | 2 | ported: half-width katakana to hiragana; @cmd removed |
| PCSG01260 | filter | 1 | ported: only a text that starts with 【 is kept, cut after its first 】 (none: dropped) |
| PCSG01260_T | filter | 1 | ported: @n removed |
| PCSG01284 | filter | 1 | ported: only a duplicate check in Luna: dropped (Rubi reads the latest text) |
| PCSG01289 | filter | 2 | ported: half-width katakana to hiragana (Luna's katakanaMap); $tbase@ becomes base, $[base$/reading$] base |
| PCSG01314 | filter | 2 | ported: @rbase@reading@ becomes base; @cmd removed (UTF-8 bytes) |
| PCSG01325 | filter | 2 | ported: @cmd removed (UTF-8 bytes) |
| all_ascii_FilterW | filter | 3 | ported: a text of ASCII characters only is dropped |

## PSP titles (Luna's PPSSPP table, ppsspp_1.cpp)

Status of every entry: PSP: not native yet (see the hooks-psp report in [#230](https://github.com/bee-san/Rubi_for_vita/issues/230)). Address: the PSP's own (PPSSPP's guest address); Reg: Luna's argument index for PPSSPP.

415 entries, 354 title IDs.

| Title ID | Region | Game (Luna's comment) | Address | Version | Reg | Padding | Encoding | Flags | Hookfunc | Filter | Luna's note | Status |
| --- | --- | --- | --- | --- | --- | ---: | --- | --- | --- | --- | --- | --- |
| ULJS00273 | JP | マリッジロワイヤル | [0x8808764](../third_party/LunaTranslator/ppsspp_1.cpp#L1704) |  | 0 | 0 | UTF-16LE | FULL_STRING | ULJS00273mes |  |  | PSP: not native yet |
| ULJS00203 | JP | ソラユメportable | [0x8828924](../third_party/LunaTranslator/ppsspp_1.cpp#L1706) |  | 1 | 0 | CP932 | FULL_STRING |  | ULJS00203 |  | PSP: not native yet |
| ULJM05555 | JP | Ｌの季節 ダブルポケット | [0x887B6E8](../third_party/LunaTranslator/ppsspp_1.cpp#L1708) |  | 0 | 0 | CP932 | FULL_STRING |  | ULJM06040_1 |  | PSP: not native yet |
| ULJM05589 | JP | あかね色に染まる坂ぽ～たぶる | [0x8832594](../third_party/LunaTranslator/ppsspp_1.cpp#L1710) |  | 0 | 0 | CP932 | DATA_INDIRECT USING_CHAR |  |  |  | PSP: not native yet |
| NPJH50654 | JP | AKB1/149 恋愛総選挙 | [0x89DB26C](../third_party/LunaTranslator/ppsspp_1.cpp#L1712) |  | 15 | 0 | UTF-8 | FULL_STRING | NPJH50654 | NewLineCharFilterA |  | PSP: not native yet |
| NPJH50655 | JP | AKB1/149 恋愛総選挙 | [0x89DB26C](../third_party/LunaTranslator/ppsspp_1.cpp#L1712) |  | 15 | 0 | UTF-8 | FULL_STRING | NPJH50654 | NewLineCharFilterA |  | PSP: not native yet |
| NPJH50627 | JP | 桜花センゴク Portable | [0x881441C](../third_party/LunaTranslator/ppsspp_1.cpp#L1714) |  | 1 | 0 | CP932 | FULL_STRING |  | NPJH50627 |  | PSP: not native yet |
| ULJM05839 | JP | 学園ヘタリア Portable | [0x88113B0](../third_party/LunaTranslator/ppsspp_1.cpp#L1716) |  | 0 | 0 | CP932 |  | ULJM05840 |  |  | PSP: not native yet |
| ULJM05840 | JP | 学園ヘタリア Portable | [0x88113B0](../third_party/LunaTranslator/ppsspp_1.cpp#L1716) |  | 0 | 0 | CP932 |  | ULJM05840 |  |  | PSP: not native yet |
| NPJH50715 | JP | 恋愛0キロメートル Portable | [0x881F154](../third_party/LunaTranslator/ppsspp_1.cpp#L1718) |  | 1 | 0 | CP932 | FULL_STRING |  | NPJH50715 |  | PSP: not native yet |
| NPJH50560 | JP | 文明開華 葵座異聞録 再演 | [0x886A55C](../third_party/LunaTranslator/ppsspp_1.cpp#L1720) |  | 0 | 0 | CP932 | FULL_STRING |  | FULJM05889 |  | PSP: not native yet |
| NPJH50560 | JP | 文明開華 葵座異聞録 再演 | [0x886A600](../third_party/LunaTranslator/ppsspp_1.cpp#L1721) |  | 12 | 0 | CP932 | FULL_STRING |  | FULJM05889 | name | PSP: not native yet |
| ULJM05491 | JP | サイファーPORTABLE | [0x880FF80](../third_party/LunaTranslator/ppsspp_1.cpp#L1723) |  | 3 | 0 | CP932 |  |  | ULJM05491 | 不可以快进 | PSP: not native yet |
| ULJM05962 | JP | 花と乙女に祝福を　～春風の贈り物～　portable | [0x8814B08](../third_party/LunaTranslator/ppsspp_1.cpp#L1725) |  | 0 | 0 | CP932 |  |  | ULJM05954 |  | PSP: not native yet |
| ULJM05683 | JP | Starry☆Sky ～in Spring～ Portable | [0x88649A0](../third_party/LunaTranslator/ppsspp_1.cpp#L1727) |  | 0 | 0 | CP932 |  |  | ULJM06397 |  | PSP: not native yet |
| ULJM05740 | JP | Starry☆Sky～in Summer～Portable | [0x88653D4](../third_party/LunaTranslator/ppsspp_1.cpp#L1729) |  | 0 | 0 | CP932 |  |  | FULJM05889 |  | PSP: not native yet |
| ULJM05809 | JP | Starry☆Sky～in Autumn～Portable | [0x8835E10](../third_party/LunaTranslator/ppsspp_1.cpp#L1731) |  | 13 | 0 | CP932 | DATA_INDIRECT USING_CHAR |  |  |  | PSP: not native yet |
| ULJM05861 | JP | Starry☆Sky～in Winter～Portable | [0x8835DD0](../third_party/LunaTranslator/ppsspp_1.cpp#L1733) |  | 13 | 0 | CP932 | DATA_INDIRECT USING_CHAR |  |  |  | PSP: not native yet |
| ULJM06111 | JP | しろくまベルスターズ♪ ハッピー・ホリデーズ！ | [0x88547A8](../third_party/LunaTranslator/ppsspp_1.cpp#L1743) |  | 1 | 0 | CP932 | FULL_STRING |  | ULJM06111 |  | PSP: not native yet |
| ULJM06286 | JP | 恋花デイズ | [0x883EA4C](../third_party/LunaTranslator/ppsspp_1.cpp#L1745) |  | 0 | 0 | CP932 |  |  | ULJM06286 |  | PSP: not native yet |
| ULJS00216 | JP | 雨格子の館 PORTABLE 一柳和、最初の受難 | [0x8861058](../third_party/LunaTranslator/ppsspp_1.cpp#L1747) |  | 4 | 0 | CP932 |  |  | ULJS00216 |  | PSP: not native yet |
| ULJS00216 | JP | 雨格子の館 PORTABLE 一柳和、最初の受難 | [0x8861044](../third_party/LunaTranslator/ppsspp_1.cpp#L1748) |  | 4 | 0 | CP932 |  |  | ULJS00216_1 |  | PSP: not native yet |
| ULJS00230 | JP | 奈落の城 PORTABLE 一柳和、2度目の受難 | [0x8869404](../third_party/LunaTranslator/ppsspp_1.cpp#L1750) |  | 4 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| NPJH50186 | JP | 氷の墓標　一柳和、３度目の受難 | [0x8826668](../third_party/LunaTranslator/ppsspp_1.cpp#L1752) |  | 0 | 0 | CP932 |  |  | NPJH50186 |  | PSP: not native yet |
| ULJM06192 | JP | 黄昏のシンセミア portable | [0x8852D04](../third_party/LunaTranslator/ppsspp_1.cpp#L1754) |  | 2 | 0 | UTF-16LE |  |  | ULJM06192 |  | PSP: not native yet |
| NPJH50836 | JP | 俺の彼女のウラオモテ ～Pure Sweet Heart～ | [0x8821550](../third_party/LunaTranslator/ppsspp_1.cpp#L1756) |  | 13 | 0 | CP932 |  |  | NPJH50836_1 |  | PSP: not native yet |
| NPJH50836 | JP | 俺の彼女のウラオモテ ～Pure Sweet Heart～ | [0x88208F8](../third_party/LunaTranslator/ppsspp_1.cpp#L1757) |  | 0 | 0 | CP932 |  |  | NPJH50836_2 |  | PSP: not native yet |
| NPJH50867 | JP | 越えざるは紅い花　大河は未来を紡ぐ | [0x8871340](../third_party/LunaTranslator/ppsspp_1.cpp#L1759) |  | 5 | 0 | CP932 |  |  |  | 需要自行将自定义人名占位符替换成自定义人名 | PSP: not native yet |
| ULJM06234 | JP | 下天の華 | [0x8915BB0](../third_party/LunaTranslator/ppsspp_1.cpp#L1761) |  | 0 | 0 | CP932 |  | ULJM05428 | ULJM06234 |  | PSP: not native yet |
| NPJH50864 | JP | 下天の華 夢灯り | [0x8841B70](../third_party/LunaTranslator/ppsspp_1.cpp#L1763) |  | 0 | 0 | CP932 |  | ULJM05428 |  |  | PSP: not native yet |
| ULJM06353 | JP | 忍び、恋うつつ | [0x88A3E28](../third_party/LunaTranslator/ppsspp_1.cpp#L1765) |  | 3 | 0 | CP932 |  |  | ULJM06289 |  | PSP: not native yet |
| ULJM06353 | JP | 忍び、恋うつつ | [0x8890858](../third_party/LunaTranslator/ppsspp_1.cpp#L1766) |  | 0 | 0 | CP932 |  |  | ULJM06353 |  | PSP: not native yet |
| ULJM05387 | JP | 蘭島物語 レアランドストーリー 少女の約定 | [0x880E840](../third_party/LunaTranslator/ppsspp_1.cpp#L1768) |  | 1 | 0 | UTF-16LE |  |  |  |  | PSP: not native yet |
| ULJM05387 | JP | 蘭島物語 レアランドストーリー 少女の約定 | [0x881542C](../third_party/LunaTranslator/ppsspp_1.cpp#L1769) |  | 1 | 0 | UTF-16LE |  |  |  |  | PSP: not native yet |
| ULJM06202 | JP | Solomon's Ring ～地の章～ / Solomon's Ring ～水の章～ / Solomon's Ring ～風の章～ | [0x88052C0](../third_party/LunaTranslator/ppsspp_1.cpp#L1773) |  | 2 | 0 | CP932 |  |  | ULJM06200 |  | PSP: not native yet |
| ULJM06203 | JP | Solomon's Ring ～地の章～ / Solomon's Ring ～水の章～ / Solomon's Ring ～風の章～ | [0x88052C0](../third_party/LunaTranslator/ppsspp_1.cpp#L1773) |  | 2 | 0 | CP932 |  |  | ULJM06200 |  | PSP: not native yet |
| ULJM06204 | JP | Solomon's Ring ～地の章～ / Solomon's Ring ～水の章～ / Solomon's Ring ～風の章～ | [0x88052C0](../third_party/LunaTranslator/ppsspp_1.cpp#L1773) |  | 2 | 0 | CP932 |  |  | ULJM06200 |  | PSP: not native yet |
| ULJM06200 | JP | Solomon's Ring ～火の章～ | [0x88061B0](../third_party/LunaTranslator/ppsspp_1.cpp#L1775) |  | 2 | 0 | CP932 |  |  | ULJM06200 |  | PSP: not native yet |
| ULJM05975 | JP | 神なる君と | [0x88F6660](../third_party/LunaTranslator/ppsspp_1.cpp#L1777) |  | 3 | 0 | CP932 | FULL_STRING |  | ULJM06196 |  | PSP: not native yet |
| ULJM05975 | JP | 神なる君と | [0x88F6648](../third_party/LunaTranslator/ppsspp_1.cpp#L1778) |  | 0 | 0 | CP932 | FULL_STRING |  | ULJM05975Name |  | PSP: not native yet |
| ULJM05819 | JP | DEARDROPS DISTORTION | [0x8814110](../third_party/LunaTranslator/ppsspp_1.cpp#L1780) |  | 4 | 0 | CP932 | DATA_INDIRECT USING_CHAR |  |  |  | PSP: not native yet |
| ULJS00035 | JP | 流行り神ＰＯＲＴＡＢＬＥ | [0x88081CC](../third_party/LunaTranslator/ppsspp_1.cpp#L1782) |  | 7 | 0 | CP932 |  | ULJS00035 | FULJS00204 | 1&3会有少量字符缺失 | PSP: not native yet |
| ULJS00149 | JP | 流行り神２ＰＯＲＴＡＢＬＥ | [0x883EAD0](../third_party/LunaTranslator/ppsspp_1.cpp#L1784) |  | 0 | 0 | CP932 |  | ULJS00149 | FULJS00204 |  | PSP: not native yet |
| ULJS00204 | JP | 流行り神３ | [0x885CB50](../third_party/LunaTranslator/ppsspp_1.cpp#L1786) |  | 3 | 0 | CP932 |  | ULJS00204 | FULJS00204 |  | PSP: not native yet |
| ULJS00403 | JP | 死神と少女 | [0x883BF34](../third_party/LunaTranslator/ppsspp_1.cpp#L1788) |  | 1 | 0 | CP932 |  |  | ULJS00403 |  | PSP: not native yet |
| ULJS00339 | JP | アマガミ | [0x886775C](../third_party/LunaTranslator/ppsspp_1.cpp#L1790) |  | 0 | 0 | CP932 |  | ULJS00339 |  | String.length() | PSP: not native yet |
| ULJM05878 | JP | 世界でいちばんNG（だめ）な恋 | [0x8814ADC](../third_party/LunaTranslator/ppsspp_1.cpp#L1792) |  | 0 | 0 | CP932 |  |  | NPJH50909_filter | name + dialouge | PSP: not native yet |
| ULJM05878 | JP | 世界でいちばんNG（だめ）な恋 | [0x8850B2C](../third_party/LunaTranslator/ppsspp_1.cpp#L1793) |  | 0 | 0 | CP932 |  |  | NPJH50909_filter | onscreen toast | PSP: not native yet |
| ULJM06119 | JP | Dunamis15 | [0x891D72C](../third_party/LunaTranslator/ppsspp_1.cpp#L1795) |  | 0 | 0 | UTF-8 |  |  | ULJM06119_filter |  | PSP: not native yet |
| ULJM05054 | JP | 金色のコルダ | [0x886162C](../third_party/LunaTranslator/ppsspp_1.cpp#L1797) |  | 1 | 0 | CP932 |  | ULJM05428 |  | dialogue: 0x886162c (x1), 0x889d5fc-0x889d520(a2) fullLine | PSP: not native yet |
| ULJM05054 | JP | 金色のコルダ | [0x8899E90](../third_party/LunaTranslator/ppsspp_1.cpp#L1798) |  | 0 | 0x3c | CP932 |  |  |  | name 0x88da57c, 0x8899ca4 (x0, oneTime), 0x8899e90 | PSP: not native yet |
| ULJM05428 | JP | 金色のコルダ2 f | [0x89B59DC](../third_party/LunaTranslator/ppsspp_1.cpp#L1800) |  | 1 | 0 | CP932 |  | ULJM05428 |  |  | PSP: not native yet |
| ULJM05508 | JP | 金色のコルダ２ f アンコール | [0x89D9FB0](../third_party/LunaTranslator/ppsspp_1.cpp#L1802) |  | 1 | 0 | CP932 |  | ULJM05428 |  |  | PSP: not native yet |
| ULJM05624 | JP | 金色のコルダ３ | [0x896C3B8](../third_party/LunaTranslator/ppsspp_1.cpp#L1804) |  | 1 | 0 | CP932 |  | ULJM05428 |  |  | PSP: not native yet |
| NPJH50821 | JP | 金色のコルダ３ フルボイス Special | [0x8A731B0](../third_party/LunaTranslator/ppsspp_1.cpp#L1806) |  | 1 | 0 | CP932 |  | ULJM05428 |  |  | PSP: not native yet |
| NPJH50845 | JP | 金色のコルダ３ AnotherSky feat.神南 | [0x883C940](../third_party/LunaTranslator/ppsspp_1.cpp#L1808) |  | 0 | 0 | CP932 |  | ULJM05428 |  |  | PSP: not native yet |
| NPJH50846 | JP | 金色のコルダ３ AnotherSky feat.至誠館 | [0x8A99BD8](../third_party/LunaTranslator/ppsspp_1.cpp#L1810) |  | 1 | 0 | CP932 |  | ULJM05428 |  |  | PSP: not native yet |
| NPJH50847 | JP | 金色のコルダ３ AnotherSky feat.天音学園 | [0x8AAB770](../third_party/LunaTranslator/ppsspp_1.cpp#L1812) |  | 1 | 0 | CP932 |  | ULJM05428 |  |  | PSP: not native yet |
| NPJH50619 | JP | Sol Trigger | [0x8952CFC](../third_party/LunaTranslator/ppsspp_1.cpp#L1814) |  | 0 | 0 | UTF-8 |  |  | NPJH50619F | dialog | PSP: not native yet |
| NPJH50619 | JP | Sol Trigger | [0x884AAD4](../third_party/LunaTranslator/ppsspp_1.cpp#L1815) |  | 0 | 0 | UTF-8 |  |  | NPJH50619F | description | PSP: not native yet |
| NPJH50619 | JP | Sol Trigger | [0x882E1B0](../third_party/LunaTranslator/ppsspp_1.cpp#L1816) |  | 0 | 0 | UTF-8 |  |  | NPJH50619F | system | PSP: not native yet |
| NPJH50619 | JP | Sol Trigger | [0x88BB108](../third_party/LunaTranslator/ppsspp_1.cpp#L1817) |  | 2 | 0 | UTF-8 |  |  | NPJH50619F | battle tutorial | PSP: not native yet |
| NPJH50619 | JP | Sol Trigger | [0x89526A0](../third_party/LunaTranslator/ppsspp_1.cpp#L1818) |  | 0 | 0 | UTF-8 |  |  | NPJH50619F | battle info | PSP: not native yet |
| NPJH50619 | JP | Sol Trigger | [0x88BCEF8](../third_party/LunaTranslator/ppsspp_1.cpp#L1819) |  | 1 | 0 | UTF-8 |  |  | NPJH50619F | battle talk | PSP: not native yet |
| NPJH50505 | JP | Fate/EXTRA CCC | [0x8958490](../third_party/LunaTranslator/ppsspp_1.cpp#L1821) |  | 0 | 0 | CP932 |  |  | NPJH50505F |  | PSP: not native yet |
| ULUS10576 | US | Fate/EXTRA | [0x882F980](../third_party/LunaTranslator/ppsspp_1.cpp#L1823) |  | 0 | 0 | CP932 | FULL_STRING |  | ULUS10576 |  | PSP: not native yet |
| NPJH50247 | JP | Fate/EXTRA | [0x88B87F0](../third_party/LunaTranslator/ppsspp_1.cpp#L1825) |  | 6 | 0 | CP932 |  |  | FNPJH50247 |  | PSP: not native yet |
| NPJH50809 | JP | 神々の悪戯 | [0x88663FC](../third_party/LunaTranslator/ppsspp_1.cpp#L1827) |  | 0 | 0 | CP932 |  | NPJH50809 | NPJH50809F | 缺少自定义人名 | PSP: not native yet |
| NPJH50909 | JP | 神々の悪戯 InFinite | [0x88630F8](../third_party/LunaTranslator/ppsspp_1.cpp#L1829) |  | 0 | 0 | CP932 |  | QNPJH50909 |  | text, choice (debounce trailing 400ms), TODO: better hook | PSP: not native yet |
| NPJH50909 | JP | 神々の悪戯 InFinite | [0x887813C](../third_party/LunaTranslator/ppsspp_1.cpp#L1830) |  | 3 | 0x4 | CP932 |  |  |  | Question YN | PSP: not native yet |
| NPJH50908 | JP | アンジェリーク ルトゥール | [0x88A4970](../third_party/LunaTranslator/ppsspp_1.cpp#L1832) |  | 1 | 0 | UTF-16LE |  |  | NPJH50908 |  | PSP: not native yet |
| ULJM05943 | JP | 月華繚乱ROMANCE | [0x88EEBA4](../third_party/LunaTranslator/ppsspp_1.cpp#L1834) |  | 0 | 0 | CP932 |  |  | ULJM05943F | a0 - monologue text | PSP: not native yet |
| ULJM05943 | JP | 月華繚乱ROMANCE | [0x8875E0C](../third_party/LunaTranslator/ppsspp_1.cpp#L1835) |  | 1 | 0x6 | CP932 |  |  | ULJM05943F | a1 - dialogue text | PSP: not native yet |
| ULJM05603 | JP | My Merry May with be | [0x886F014](../third_party/LunaTranslator/ppsspp_1.cpp#L1837) |  | 3 | 0 | CP932 |  |  | ULJM06040_1 |  | PSP: not native yet |
| ULJM06114 | JP | コープスパーティー -THE ANTHOLOGY- サチコの恋愛遊戯♥Hysteric Birthday 2U | [0x88517C8](../third_party/LunaTranslator/ppsspp_1.cpp#L1839) |  | 1 | 0 | CP932 |  |  | ULJM06040_1 |  | PSP: not native yet |
| ULJM06321 | JP | 向日葵の教会と長い夏休み | [0x881C444](../third_party/LunaTranslator/ppsspp_1.cpp#L1841) |  | 0 | 0 | CP932 | FULL_STRING |  |  | name+text,sjit,FULL_STRING to split name and text | PSP: not native yet |
| ULJM05889 | JP | ましろ色シンフォニー *mutsu-no-hana | [0x8868AB8](../third_party/LunaTranslator/ppsspp_1.cpp#L1843) |  | 0 | 0 | CP932 |  |  | FULJM05889 |  | PSP: not native yet |
| NPJH50530 | JP | シャイニング・ブレイド | [0x8AA3B70](../third_party/LunaTranslator/ppsspp_1.cpp#L1845) |  | 12 | 0 | CP932 |  |  | NPJH50530::T | text only | PSP: not native yet |
| NPJH50530 | JP | シャイニング・ブレイド | [0x884DB44](../third_party/LunaTranslator/ppsspp_1.cpp#L1846) |  | 1 | 0 | CP932 |  |  | NPJH50530::N | text+name | PSP: not native yet |
| NPJH50243 | JP | ティアーズ・トゥ・ティアラ 外伝 アヴァロンの謎 ＰＯＲＴＡＢＬＥ | [0x890A4BC](../third_party/LunaTranslator/ppsspp_1.cpp#L1848) |  | 1 | 0 | UTF-16LE |  |  | FNPJH50243 |  | PSP: not native yet |
| ULJM05802 | JP | 薔薇ノ木ニ薔薇ノ花咲ク | [0x881E560](../third_party/LunaTranslator/ppsspp_1.cpp#L1850) |  | 1 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| ULJM05690 | JP | D.C. Girl's Symphony Pocket ～ダ・カーポ～ ガールズシンフォニーポケット | [0x883C77C](../third_party/LunaTranslator/ppsspp_1.cpp#L1852) |  | 0 | 0 | CP932 |  |  | FULJM05690 |  | PSP: not native yet |
| ULJM06239 | JP | D.C.III Plus ～ダ・カーポIII～プラス | [0x881301C](../third_party/LunaTranslator/ppsspp_1.cpp#L1854) |  | 1 | 0 | CP932 |  |  | ULJM05770 |  | PSP: not native yet |
| ULJM05437 | JP | Ever17 -the out of infinity- Premium Edition | [0x881AD64](../third_party/LunaTranslator/ppsspp_1.cpp#L1856) |  | 13 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| ULJM05433 | JP | Never7 -the end of infinity- | [0x88196F0](../third_party/LunaTranslator/ppsspp_1.cpp#L1858) |  | 14 | 0 | CP932 |  |  | ULJM05433 |  | PSP: not native yet |
| ULJM06168 | JP | 24時の鐘とシンデレラ～Halloween Wedding～ | [0x8838304](../third_party/LunaTranslator/ppsspp_1.cpp#L1860) |  | 0 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| ULJM06023 | JP | １２時の鐘とシンデレラ～Halloween Wedding～ | [0x882A650](../third_party/LunaTranslator/ppsspp_1.cpp#L1862) |  | 1 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| ULJM06272 | JP | 0時の鐘とシンデレラ～Halloween Wedding～ | [0x8855CA0](../third_party/LunaTranslator/ppsspp_1.cpp#L1864) |  | 1 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| NPJH50459 | JP | セブンスドラゴン２０２０ | [0x88847A0](../third_party/LunaTranslator/ppsspp_1.cpp#L1866) |  | 1 | 0 | UTF-8 |  |  | FNPJH50459 |  | PSP: not native yet |
| NPJH50716 | JP | セブンスドラゴン２０２０-Ⅱ | [0x8889CCC](../third_party/LunaTranslator/ppsspp_1.cpp#L1868) |  | 1 | 0 | UTF-8 |  |  | FNPJH50459 | 会有两三条后续文本都被一次性提取到。 | PSP: not native yet |
| ULJM06341 | JP | マイナスエイト | [0x88DC218](../third_party/LunaTranslator/ppsspp_1.cpp#L1870) |  | 0 | 0 | CP932 |  |  | FULJM05690 |  | PSP: not native yet |
| NPJH50127 | JP | ときめきメモリアル4 | [0x899A510](../third_party/LunaTranslator/ppsspp_1.cpp#L1872) |  | 2 | 0 | CP932 |  |  | FNPJH50127 |  | PSP: not native yet |
| NPJH50127 | JP | ときめきメモリアル4 | [0x88719DC](../third_party/LunaTranslator/ppsspp_1.cpp#L1873) |  | 1 | 0 | CP932 |  |  | FNPJH50127 |  | PSP: not native yet |
| ULJM05976 | JP | ときめきメモリアル Girl's Side Premium 3 | [0x88F09F4](../third_party/LunaTranslator/ppsspp_1.cpp#L1875) |  | 0 | 0 | UTF-16LE |  |  | NewLineCharFilterW |  | PSP: not native yet |
| ULJM06393 | JP | オメルタ～沈黙の掟～ THE LEGACY | [0x88861C8](../third_party/LunaTranslator/ppsspp_1.cpp#L1877) |  | 3 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| ULJM06393 | JP | オメルタ～沈黙の掟～ THE LEGACY | [0x8885FD8](../third_party/LunaTranslator/ppsspp_1.cpp#L1878) |  | 0 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| ULJM06393 | JP | オメルタ～沈黙の掟～ THE LEGACY | [0x88AC3A8](../third_party/LunaTranslator/ppsspp_1.cpp#L1879) |  | 1 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| ULJM05697 | JP | S・Y・K ～新説西遊記～ ポータブル | [0x88DD918](../third_party/LunaTranslator/ppsspp_1.cpp#L1881) |  | 0 | 0 | CP932 | FULL_STRING |  | ULJM05697 |  | PSP: not native yet |
| ULJM05867 | JP | S.Y.K ～蓮咲伝～ Portable | [0x88FB080](../third_party/LunaTranslator/ppsspp_1.cpp#L1883) |  | 0 | 0 | CP932 |  |  | ULJM05867_1 | TEXT | PSP: not native yet |
| ULJM05867 | JP | S.Y.K ～蓮咲伝～ Portable | [0x88FB0B8](../third_party/LunaTranslator/ppsspp_1.cpp#L1884) |  | 0 | 0 | CP932 |  |  | ULJM05867_2 | NAME | PSP: not native yet |
| ULJM06131 | JP | L.G.S～新説 封神演義～ | [0x888A358](../third_party/LunaTranslator/ppsspp_1.cpp#L1886) |  | 0 | 0 | CP932 | FULL_STRING |  | ULJM06131 | NAME+TEXT | PSP: not native yet |
| ULJM06131 | JP | L.G.S～新説 封神演義～ | [0x88AF9AC](../third_party/LunaTranslator/ppsspp_1.cpp#L1887) |  | 1 | 0 | CP932 | FULL_STRING |  | ULJM06131 | PROLOG+NAME+TEXT | PSP: not native yet |
| ULJM06145 | JP | 源狼 GENROH | [0x888E494](../third_party/LunaTranslator/ppsspp_1.cpp#L1889) |  | 0 | 0 | CP932 | FULL_STRING |  | ULJM06145 | TEXT | PSP: not native yet |
| ULJM06145 | JP | 源狼 GENROH | [0x890D51C](../third_party/LunaTranslator/ppsspp_1.cpp#L1890) |  | 0 | 0 | CP932 | FULL_STRING |  | ULJM06145 | 交易物语 | PSP: not native yet |
| ULJM06129 | JP | 十鬼の絆 関ヶ原奇譚 | [0x891AAAC](../third_party/LunaTranslator/ppsspp_1.cpp#L1892) |  | 0 | 0 | CP932 |  |  | ULJM06129 | text | PSP: not native yet |
| ULJM06129 | JP | 十鬼の絆 関ヶ原奇譚 | [0x886E094](../third_party/LunaTranslator/ppsspp_1.cpp#L1893) |  | 0 | 0 | CP932 |  |  | ULJM06129 | name+text | PSP: not native yet |
| ULJM06301 | JP | 十鬼の絆 花結綴り | [0x886E354](../third_party/LunaTranslator/ppsspp_1.cpp#L1895) |  | 0 | 0 | CP932 |  |  | ULJM06289 | name+text | PSP: not native yet |
| ULJM06301 | JP | 十鬼の絆 花結綴り | [0x88F878C](../third_party/LunaTranslator/ppsspp_1.cpp#L1896) |  | 0 | 0 | CP932 |  |  | ULJM06289 |  | PSP: not native yet |
| ULJS00315 | JP | ティンクル☆くるせいだーす STARLIT BRAVE!! | [0x88A94BC](../third_party/LunaTranslator/ppsspp_1.cpp#L1898) |  | 4 | 0 | CP932 |  |  |  | text | PSP: not native yet |
| ULJS00316 | JP | ティンクル☆くるせいだーす GoGo! | [0x8822F24](../third_party/LunaTranslator/ppsspp_1.cpp#L1900) |  | 14 | 0 | CP932 |  |  |  | text | PSP: not native yet |
| NPJH50808 | JP | 明治東亰恋伽 | [0x886CA94](../third_party/LunaTranslator/ppsspp_1.cpp#L1902) |  | 0 | 0 | CP932 |  |  | NPJH50900 | text | PSP: not native yet |
| NPJH50900 | JP | 明治東亰恋伽 トワヰライト・キス | [0x884DE44](../third_party/LunaTranslator/ppsspp_1.cpp#L1904) |  | 0 | 0 | CP932 |  |  | NPJH50900 | text | PSP: not native yet |
| ULJM06302 | JP | 青春はじめました！ | [0x880A744](../third_party/LunaTranslator/ppsspp_1.cpp#L1906) |  | 0 | 0 | CP932 |  |  | ULJM05943F |  | PSP: not native yet |
| ULJM06303 | JP | 青春はじめました！ | [0x880A744](../third_party/LunaTranslator/ppsspp_1.cpp#L1906) |  | 0 | 0 | CP932 |  |  | ULJM05943F |  | PSP: not native yet |
| ULJM06307 | JP | 青春はじめました！ | [0x880A744](../third_party/LunaTranslator/ppsspp_1.cpp#L1906) |  | 0 | 0 | CP932 |  |  | ULJM05943F |  | PSP: not native yet |
| ULJM06302 | JP | 青春はじめました！ | [0x8804094](../third_party/LunaTranslator/ppsspp_1.cpp#L1907) |  | 1 | 0 | CP932 | FULL_STRING |  | ULJM06344 |  | PSP: not native yet |
| ULJM06303 | JP | 青春はじめました！ | [0x8804094](../third_party/LunaTranslator/ppsspp_1.cpp#L1907) |  | 1 | 0 | CP932 | FULL_STRING |  | ULJM06344 |  | PSP: not native yet |
| ULJM06307 | JP | 青春はじめました！ | [0x8804094](../third_party/LunaTranslator/ppsspp_1.cpp#L1907) |  | 1 | 0 | CP932 | FULL_STRING |  | ULJM06344 |  | PSP: not native yet |
| ULJM06064 | JP | アーメン・ノワール ポータブル | [0x883B6A8](../third_party/LunaTranslator/ppsspp_1.cpp#L1909) |  | 0 | 0 | CP932 |  |  | ULJM05943F |  | PSP: not native yet |
| ULJM05823 | JP | デス・コネクション　ポータブル | [0x8855594](../third_party/LunaTranslator/ppsspp_1.cpp#L1911) |  | 0 | 0 | CP932 | FULL_STRING |  | ULJM05823 |  | PSP: not native yet |
| ULJM06289 | JP | しらつゆの怪 | [0x888A26C](../third_party/LunaTranslator/ppsspp_1.cpp#L1913) |  | 0 | 0 | CP932 | FULL_STRING |  | ULJM06289 |  | PSP: not native yet |
| ULJM06289 | JP | しらつゆの怪 | [0x8919E30](../third_party/LunaTranslator/ppsspp_1.cpp#L1914) |  | 0 | 0 | CP932 | FULL_STRING |  | ULJM06289 |  | PSP: not native yet |
| ULJM06289 | JP | しらつゆの怪 | [0x8962EA0](../third_party/LunaTranslator/ppsspp_1.cpp#L1915) |  | 1 | 0 | CP932 | FULL_STRING |  | ULJM06289 |  | PSP: not native yet |
| ULJM05973 | JP | ジョーカーの国のアリス～Wonderful Wonder World～ | [0x880F8F0](../third_party/LunaTranslator/ppsspp_1.cpp#L1917) |  | 0 | 0 | CP932 | FULL_STRING |  | ULJM06344 |  | PSP: not native yet |
| ULJM06216 | JP | ダイヤの国のアリス～Wonderful Wonder World～ | [0x8857E3C](../third_party/LunaTranslator/ppsspp_1.cpp#L1919) |  | 0 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| ULJM06295 | JP | ダイヤの国のアリス～ Wonderful Mirror World ～ | [0x8855AE4](../third_party/LunaTranslator/ppsspp_1.cpp#L1921) |  | 1 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| NPJH50872 | JP | ハートの国のアリス～Wonderful Twin World～ | [0x8881CAC](../third_party/LunaTranslator/ppsspp_1.cpp#L1923) |  | 1 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| ULJM05995 | JP | おもちゃ箱の国のアリス～Wonderful Wonder World～ | [0x8884A0C](../third_party/LunaTranslator/ppsspp_1.cpp#L1925) |  | 5 | 0 | UTF-16LE |  |  | ULJM05995 |  | PSP: not native yet |
| ULJM05995 | JP | おもちゃ箱の国のアリス～Wonderful Wonder World～ | [0x88831BC](../third_party/LunaTranslator/ppsspp_1.cpp#L1926) |  | 12 | 0 | UTF-16LE |  |  | ULJM05995_2 |  | PSP: not native yet |
| ULJM06332 | JP | 新装版 ハートの国のアリス～Wonderful Wonder World～ | [0x886B610](../third_party/LunaTranslator/ppsspp_1.cpp#L1928) |  | 1 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| NPJH50894 | JP | 新装版クローバーの国のアリス～Wonderful Wonder World～ | [0x8875E50](../third_party/LunaTranslator/ppsspp_1.cpp#L1930) |  | 1 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| ULJM06196 | JP | Glass Heart Princess | [0x885FA30](../third_party/LunaTranslator/ppsspp_1.cpp#L1932) |  | 0 | 0 | CP932 | FULL_STRING |  | ULJM05943F |  | PSP: not native yet |
| ULJM06196 | JP | Glass Heart Princess | [0x8921A74](../third_party/LunaTranslator/ppsspp_1.cpp#L1933) |  | 0 | 0 | CP932 | FULL_STRING |  | ULJM06196 |  | PSP: not native yet |
| ULJM06309 | JP | Glass Heart Princess:PLATINUM | [0x885D4F0](../third_party/LunaTranslator/ppsspp_1.cpp#L1935) |  | 0 | 0 | CP932 | FULL_STRING |  | ULJM05943F |  | PSP: not native yet |
| ULJM06309 | JP | Glass Heart Princess:PLATINUM | [0x8921BF8](../third_party/LunaTranslator/ppsspp_1.cpp#L1936) |  | 0 | 0 | CP932 | FULL_STRING |  | ULJM06196 |  | PSP: not native yet |
| ULJM05447 | JP | ウィル・オ・ウィスプ ポータブル | [0x885DD04](../third_party/LunaTranslator/ppsspp_1.cpp#L1938) |  | 0 | 0 | CP932 |  |  | ULJM05943F |  | PSP: not native yet |
| ULJM05847 | JP | 華鬼 ～恋い初める刻 永久の印～ | [0x8829F14](../third_party/LunaTranslator/ppsspp_1.cpp#L1940) |  | 4 | 0 | CP932 |  |  | ULJM05943F |  | PSP: not native yet |
| ULJM05847 | JP | 華鬼 ～恋い初める刻 永久の印～ | [0x886D270](../third_party/LunaTranslator/ppsspp_1.cpp#L1941) |  | 0 | 0 | CP932 |  |  | ULJM05823_2 |  | PSP: not native yet |
| ULJM06048 | JP | 華鬼 ～夢のつづき～ | [0x885B7BC](../third_party/LunaTranslator/ppsspp_1.cpp#L1943) |  | 0 | 0 | CP932 | FULL_STRING |  | ULJM05943F | name+text | PSP: not native yet |
| ULJM06048 | JP | 華鬼 ～夢のつづき～ | [0x8912534](../third_party/LunaTranslator/ppsspp_1.cpp#L1944) |  | 0 | 0 | CP932 | FULL_STRING |  | ULJM06167 |  | PSP: not native yet |
| NPJH50380 | JP | サモンナイト３ | [0x89DCF90](../third_party/LunaTranslator/ppsspp_1.cpp#L1946) |  | 6 | 0 | CP932 |  |  | NPJH50380 |  | PSP: not native yet |
| NPJH50410 | JP | サモンナイト４ | [0x89E7760](../third_party/LunaTranslator/ppsspp_1.cpp#L1948) |  | 6 | 0 | CP932 |  |  | NPJH50380 |  | PSP: not native yet |
| NPJH50696 | JP | サモンナイト５ | [0x88C44CC](../third_party/LunaTranslator/ppsspp_1.cpp#L1950) |  | 1 | 0 | UTF-8 |  |  |  |  | PSP: not native yet |
| ULJM05887 | JP | STEINS;GATE | [0x8870320](../third_party/LunaTranslator/ppsspp_1.cpp#L1952) |  | 3 | 0 | CP932 |  |  | ULJM06040_1 |  | PSP: not native yet |
| ULJM06040 | JP | Steins;Gate 比翼恋理のだーりん | [0x8856968](../third_party/LunaTranslator/ppsspp_1.cpp#L1954) |  | 4 | 0 | CP932 |  |  | ULJM06040_1 |  | PSP: not native yet |
| ULJM06040 | JP | Steins;Gate 比翼恋理のだーりん | [0x889AD70](../third_party/LunaTranslator/ppsspp_1.cpp#L1955) |  | 1 | 0 | CP932 |  |  | ULJM06040_2 |  | PSP: not native yet |
| ULJM05456 | JP | 鋼鉄のガールフレンド特別編ポータブル | [0x882AAA4](../third_party/LunaTranslator/ppsspp_1.cpp#L1957) |  | 1 | 0 | CP932 |  |  | ULJM05456 |  | PSP: not native yet |
| ULJM05477 | JP | 鋼鉄のガールフレンド2ndポータブル | [0x8807FAC](../third_party/LunaTranslator/ppsspp_1.cpp#L1959) |  | 1 | 0 | CP932 |  |  | ULJM05477 |  | PSP: not native yet |
| ULJS00167 | JP | アイドルマスターSP パーフェクトサン | [0x8951A7C](../third_party/LunaTranslator/ppsspp_1.cpp#L1961) |  | 1 | 0 | CP932 |  |  | ULJS00169 |  | PSP: not native yet |
| ULJS00168 | JP | アイドルマスターSP ワンダリングスター | [0x8955E54](../third_party/LunaTranslator/ppsspp_1.cpp#L1963) |  | 0 | 0 | CP932 |  |  | ULJS00169 |  | PSP: not native yet |
| ULJS00169 | JP | アイドルマスターSP ミッシングムーン | [0x8951AE0](../third_party/LunaTranslator/ppsspp_1.cpp#L1965) |  | 1 | 0 | CP932 |  |  | ULJS00169 |  | PSP: not native yet |
| ULJM05796 | JP | カヌチ 二つの翼 | [0x88158A0](../third_party/LunaTranslator/ppsspp_1.cpp#L1967) |  | 0 | 0 | CP932 |  |  | ULJM05796 |  | PSP: not native yet |
| ULJM05797 | JP | カヌチ 二つの翼 | [0x88158A0](../third_party/LunaTranslator/ppsspp_1.cpp#L1967) |  | 0 | 0 | CP932 |  |  | ULJM05796 |  | PSP: not native yet |
| ULJM05458 | JP | うたわれるもの PORTABLE | [0x881CC54](../third_party/LunaTranslator/ppsspp_1.cpp#L1969) |  | 0 | 0 | CP932 |  |  | ULJM05458 |  | PSP: not native yet |
| ULJS00354 | JP | とある科学の超電磁砲 | [0x88363A8](../third_party/LunaTranslator/ppsspp_1.cpp#L1971) |  | 1 | 0 | CP932 | FULL_STRING |  | ULJS00354 |  | PSP: not native yet |
| ULJS00329 | JP | とある魔術の禁書目録 | [0x882BB24](../third_party/LunaTranslator/ppsspp_1.cpp#L1973) |  | 0 | 0 | CP932 |  |  | ULJS00329 |  | PSP: not native yet |
| NPJH50700 | JP | とある魔術と科学の群奏活劇 | [0x882A0C4](../third_party/LunaTranslator/ppsspp_1.cpp#L1975) |  | 1 | 0 | CP932 |  |  | NPJH50700 |  | PSP: not native yet |
| NPJH50535 | JP | 幻想水滸伝　紡がれし百年の時 | [0x893FF00](../third_party/LunaTranslator/ppsspp_1.cpp#L1977) |  | 0 | 0 | CP932 |  |  | NPJH50535 |  | PSP: not native yet |
| ULJM05756 | JP | アンチェインブレイズ レクス | [0x88FD624](../third_party/LunaTranslator/ppsspp_1.cpp#L1979) |  | 4 | 0 | UTF-8 |  |  | ULJM05756 |  | PSP: not native yet |
| NPJH50224 | JP | 密室のサクリファイス | [0x88057D4](../third_party/LunaTranslator/ppsspp_1.cpp#L1981) |  | 13 | 0 | CP932 |  |  | NPJH50224 |  | PSP: not native yet |
| NPJH00065 | JP | 密室のサクリファイス　～イトカ：ある閉鎖施設からの脱出～ | [0x8861A08](../third_party/LunaTranslator/ppsspp_1.cpp#L1983) |  | 1 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| NPJH50682 | JP | ココロコネクト ヨチランダム | [0x8837BB8](../third_party/LunaTranslator/ppsspp_1.cpp#L1985) |  | 1 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| NPJH50568 | JP | 俺の妹がこんなに可愛いわけがない ポータブル が続くわけがない Ｄｉｓｃ１ | [0x88608B8](../third_party/LunaTranslator/ppsspp_1.cpp#L1987) |  | 2 | 0 | UTF-16LE |  |  |  |  | PSP: not native yet |
| NPJH50569 | JP | 俺の妹がこんなに可愛いわけがない ポータブル が続くわけがない Ｄｉｓｃ２ | [0x8860724](../third_party/LunaTranslator/ppsspp_1.cpp#L1989) |  | 2 | 0 | UTF-16LE |  |  |  |  | PSP: not native yet |
| ULJM05282 | JP | AIR | [0x880C774](../third_party/LunaTranslator/ppsspp_1.cpp#L1991) |  | 0 | 0 | UTF-16LE |  |  | ULJM05282 |  | PSP: not native yet |
| ULJM06343 | JP | 蝶の毒 華の鎖～大正艶恋異聞～ | [0x883451C](../third_party/LunaTranslator/ppsspp_1.cpp#L1993) |  | 0 | 0 | CP932 |  |  | ULJM06343 |  | PSP: not native yet |
| NPJH50394 | JP | Black Robinia | [0x8850800](../third_party/LunaTranslator/ppsspp_1.cpp#L1995) |  | 0 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| ULJM05689 | JP | ワンド　オブ　フォーチュン　ポータブル | [0x88878A0](../third_party/LunaTranslator/ppsspp_1.cpp#L1997) |  | 0 | 0 | CP932 |  |  | ULJM05943F |  | PSP: not native yet |
| ULJM05444 | JP | Remember11 -the age of infinity- | [0x881BECC](../third_party/LunaTranslator/ppsspp_1.cpp#L1999) |  | 0 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| ULJM05610 | JP | のーふぇいと！ ～only the power of will～ | [0x889A888](../third_party/LunaTranslator/ppsspp_1.cpp#L2001) |  | 0 | 0 | CP932 |  |  | ULJM05610 |  | PSP: not native yet |
| ULJM05726 | JP | 薄桜鬼 随想録 ポータブル | [0x8874288](../third_party/LunaTranslator/ppsspp_1.cpp#L2003) |  | 4 | 0 | CP932 |  |  | ULJM05943F |  | PSP: not native yet |
| ULJM05917 | JP | 薄桜鬼 黎明録 ポータブル | [0x88AA0FC](../third_party/LunaTranslator/ppsspp_1.cpp#L2005) |  | 0 | 0 | CP932 |  |  | ULJM05943F |  | PSP: not native yet |
| ULJM05917 | JP | 薄桜鬼 黎明録 ポータブル | [0x88ABC14](../third_party/LunaTranslator/ppsspp_1.cpp#L2006) |  | 0 | 0 | CP932 |  |  | ULJM05823_2 |  | PSP: not native yet |
| ULJM05663 | JP | 薄桜鬼 遊戯録 | [0x8850720](../third_party/LunaTranslator/ppsspp_1.cpp#L2008) |  | 0 | 0 | CP932 |  |  | ULJM05943F |  | PSP: not native yet |
| ULJM05663 | JP | 薄桜鬼 遊戯録 | [0x884AB78](../third_party/LunaTranslator/ppsspp_1.cpp#L2009) |  | 0 | 0 | CP932 |  |  | ULJM05823_2 |  | PSP: not native yet |
| ULJM06165 | JP | 薄桜鬼 遊戯録弐　祭囃子と隊士達 | [0x883E84C](../third_party/LunaTranslator/ppsspp_1.cpp#L2011) |  | 1 | 0 | CP932 |  |  | ULJM05943F |  | PSP: not native yet |
| ULJM06281 | JP | 裏語 薄桜鬼 | [0x885DECC](../third_party/LunaTranslator/ppsspp_1.cpp#L2013) |  | 0 | 0 | CP932 |  |  | ULJM06281 |  | PSP: not native yet |
| ULJM06373 | JP | 裏語 薄桜鬼～暁の調べ～ | [0x88BDFB0](../third_party/LunaTranslator/ppsspp_1.cpp#L2015) |  | 1 | 0 | CP932 |  |  | ULJM06281 |  | PSP: not native yet |
| ULJM05874 | JP | メモリーズオフ ゆびきりの記憶 ふたりの風流庵 | [0x8863D5C](../third_party/LunaTranslator/ppsspp_1.cpp#L2017) |  | 3 | 0 | CP932 |  |  | ULJM05874 |  | PSP: not native yet |
| ULJM05875 | JP | メモリーズオフ ゆびきりの記憶 | [0x88A50B0](../third_party/LunaTranslator/ppsspp_1.cpp#L2019) |  | 1 | 0 | CP932 |  |  | ULJM06040_1 |  | PSP: not native yet |
| ULJM05350 | JP | Memories Off ～それから～ | [0x89435F8](../third_party/LunaTranslator/ppsspp_1.cpp#L2021) |  | 0 | 0 | CP932 | FULL_STRING |  |  |  | PSP: not native yet |
| ULJM05435 | JP | ユア・メモリーズオフ | [0x88EF260](../third_party/LunaTranslator/ppsspp_1.cpp#L2023) |  | 1 | 0 | CP932 |  |  | ULJM06040_1 |  | PSP: not native yet |
| ULJM05338 | JP | CLANNAD | [0x880F240](../third_party/LunaTranslator/ppsspp_1.cpp#L2025) |  | 0 | 0 | UTF-16LE |  |  | ULJM05282 |  | PSP: not native yet |
| ULJM05339 | JP | CLANNAD | [0x880F240](../third_party/LunaTranslator/ppsspp_1.cpp#L2025) |  | 0 | 0 | UTF-16LE |  |  | ULJM05282 |  | PSP: not native yet |
| NPJH50266 | JP | ＣＬＡＮＮＡＤ　光見守る坂道で　上巻 | [0x8850950](../third_party/LunaTranslator/ppsspp_1.cpp#L2027) |  | 12 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| NPJH50273 | JP | ＣＬＡＮＮＡＤ　光見守る坂道で　下巻 | [0x8853844](../third_party/LunaTranslator/ppsspp_1.cpp#L2029) |  | 12 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| NPJH50215 | JP | 東京鬼祓師　鴉乃杜學園奇譚 | [0x89F25C8](../third_party/LunaTranslator/ppsspp_1.cpp#L2031) |  | 1 | 0 | CP932 |  |  | NPJH50215 |  | PSP: not native yet |
| ULJM05770 | JP | 雅恋 ～MIYAKO～ | [0x8812514](../third_party/LunaTranslator/ppsspp_1.cpp#L2033) |  | 1 | 0 | CP932 |  |  | ULJM05770 |  | PSP: not native yet |
| ULJM06070 | JP | 雅恋 ～MIYAKO～ あわゆきのうたげ | [0x8811368](../third_party/LunaTranslator/ppsspp_1.cpp#L2035) |  | 1 | 0 | UTF-8 |  |  | ULJM06070 |  | PSP: not native yet |
| ULJM05674 | JP | ナルキッソス～もしも明日があるなら～Portable | [0x8857B28](../third_party/LunaTranslator/ppsspp_1.cpp#L2037) |  | 1 | 0 | CP932 |  |  | ULJM06040_1 |  | PSP: not native yet |
| ULJM05565 | JP | 未来日記　－１３人目の日記所有者－ | [0x884C30C](../third_party/LunaTranslator/ppsspp_1.cpp#L2039) |  | 0 | 0 | CP932 |  |  | ULJM05565 | 切换场景时会有很多辣鸡文本 | PSP: not native yet |
| ULJM06052 | JP | 未来日記　１３人目の日記所有者　ＲＥ：ＷＲＩＴＥ | [0x8816BFC](../third_party/LunaTranslator/ppsspp_1.cpp#L2041) |  | 0 | 0 | CP932 |  |  | ULJM06052 | 只能在下一句时提取到上一句 | PSP: not native yet |
| ULJM05821 | JP | CHAOS;HEAD らぶChu☆Chu! | [0x88B5AD8](../third_party/LunaTranslator/ppsspp_1.cpp#L2043) |  | 14 | 0 | CP932 |  |  | ULJM05821 |  | PSP: not native yet |
| ULJS00371 | JP | 涼宮ハルヒの追想 | [0x881AAD8](../third_party/LunaTranslator/ppsspp_1.cpp#L2045) |  | 1 | 0 | UTF-16LE | FULL_STRING |  |  |  | PSP: not native yet |
| ULJS00123 | JP | 涼宮ハルヒの約束 | [0x882C6B4](../third_party/LunaTranslator/ppsspp_1.cpp#L2047) |  | 6 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| ULJS00124 | JP | 涼宮ハルヒの約束 | [0x882C6B4](../third_party/LunaTranslator/ppsspp_1.cpp#L2047) |  | 6 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| ULJM05936 | JP | code_18 | [0x884B8B8](../third_party/LunaTranslator/ppsspp_1.cpp#L2049) |  | 0 | 0 | CP932 |  |  | ULJM05821 |  | PSP: not native yet |
| ULJM05691 | JP | 華ヤカ哉、我ガ一族 | [0x885138C](../third_party/LunaTranslator/ppsspp_1.cpp#L2051) |  | 1 | 0 | CP932 | FULL_STRING |  | ULJM05691 |  | PSP: not native yet |
| ULJM05998 | JP | 華ヤカ哉、我ガ一族 キネマモザイク | [0x8816CE0](../third_party/LunaTranslator/ppsspp_1.cpp#L2053) |  | 1 | 0 | CP932 | FULL_STRING |  | ULJM05691 |  | PSP: not native yet |
| ULJM06263 | JP | 華ヤカ哉、我ガ一族 黄昏ポウラスタ | [0x889DD34](../third_party/LunaTranslator/ppsspp_1.cpp#L2055) |  | 3 | 0 | CP932 |  |  | ULJM05691 |  | PSP: not native yet |
| ULJM05892 | JP | マザーグースの秘密の館～Nursery Rhymes for you～ | [0x8815B34](../third_party/LunaTranslator/ppsspp_1.cpp#L2057) |  | 1 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| ULJM05950 | JP | マザーグースの秘密の館～BLUE LABEL～ | [0x8831BB4](../third_party/LunaTranslator/ppsspp_1.cpp#L2059) |  | 1 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| ULJM06184 | JP | 百鬼夜行～怪談ロマンス～ | [0x884A634](../third_party/LunaTranslator/ppsspp_1.cpp#L2061) |  | 1 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| ULJM06039 | JP | 逢魔時～怪談ロマンス～ | [0x8833C64](../third_party/LunaTranslator/ppsspp_1.cpp#L2063) |  | 1 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| ULJM06323 | JP | 百物語～怪談ロマンス～ | [0x8843458](../third_party/LunaTranslator/ppsspp_1.cpp#L2065) |  | 1 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| ULJM06235 | JP | 黄昏時～怪談ロマンス～ | [0x8841A98](../third_party/LunaTranslator/ppsspp_1.cpp#L2067) |  | 8 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| ULJM06050 | JP | アブナイ★恋の捜査室 | [0x8842F84](../third_party/LunaTranslator/ppsspp_1.cpp#L2069) |  | 1 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| ULJM05374 | JP | ネオ アンジェリークSpecial | [0x8867018](../third_party/LunaTranslator/ppsspp_1.cpp#L2071) |  | 1 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| ULJM06252 | JP | 遙かなる時空の中で～八葉抄～ | [0x88C1290](../third_party/LunaTranslator/ppsspp_1.cpp#L2073) |  | 2 | 0 | CP932 |  |  | NewLineCharFilterA | 必须按一下按钮，才能显示 | PSP: not native yet |
| ULJM05019 | JP | 遙かなる時空の中で２ | [0x88C0410](../third_party/LunaTranslator/ppsspp_1.cpp#L2075) |  | 2 | 0 | CP932 |  |  | ULJM05019 |  | PSP: not native yet |
| ULJM05441 | JP | 遙かなる時空の中で３ with 十六夜記 愛蔵版 | [0x89024C8](../third_party/LunaTranslator/ppsspp_1.cpp#L2077) |  | 0 | 0 | CP932 |  | ULJM05441 |  |  | PSP: not native yet |
| ULJM05547 | JP | 遙かなる時空の中で３ 運命の迷宮 愛蔵版 | [0x89081F4](../third_party/LunaTranslator/ppsspp_1.cpp#L2079) |  | 0 | 0 | CP932 |  | ULJM05441 |  |  | PSP: not native yet |
| ULJM05810 | JP | 遙かなる時空の中で４ 愛蔵版 | [0x8955CE0](../third_party/LunaTranslator/ppsspp_1.cpp#L2081) |  | 0 | 0 | CP932 |  | ULJM05810 |  |  | PSP: not native yet |
| ULJM05843 | JP | 遙かなる時空の中で５ | [0x8A13278](../third_party/LunaTranslator/ppsspp_1.cpp#L2083) |  | 0 | 0 | CP932 |  | ULJM05428 | NewLineCharFilterA |  | PSP: not native yet |
| ULJM06025 | JP | 遙かなる時空の中で５ 風花記 | [0x8B0449C](../third_party/LunaTranslator/ppsspp_1.cpp#L2085) |  | 1 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| NPJH50901 | JP | 遙かなる時空の中で６ | [0x89FD41C](../third_party/LunaTranslator/ppsspp_1.cpp#L2087) |  | 15 | 0 | CP932 |  |  | NPJH50901 |  | PSP: not native yet |
| ULJM06328 | JP | SNOW BOUND LAND | [0x88D6180](../third_party/LunaTranslator/ppsspp_1.cpp#L2089) |  | 0 | 0 | CP932 |  |  | ULJM05943F | t | PSP: not native yet |
| ULJM06328 | JP | SNOW BOUND LAND | [0x888AD68](../third_party/LunaTranslator/ppsspp_1.cpp#L2090) |  | 0 | 0 | CP932 |  |  | ULJM05943F | n+t | PSP: not native yet |
| ULJM06143 | JP | Confidential Money ～300日で3000万ドル稼ぐ方法～ | [0x881BD00](../third_party/LunaTranslator/ppsspp_1.cpp#L2092) |  | 1 | 0 | UTF-16LE |  | ULJM06143 |  |  | PSP: not native yet |
| ULJM06143 | JP | Confidential Money ～300日で3000万ドル稼ぐ方法～ | [0x882555C](../third_party/LunaTranslator/ppsspp_1.cpp#L2093) |  | 2 | 0 | UTF-16LE |  | ULJM06143_1 |  |  | PSP: not native yet |
| ULJM06187 | JP | アルカナ・ファミリア フェスタ・レガーロ | [0x881A318](../third_party/LunaTranslator/ppsspp_1.cpp#L2095) |  | 0 | 0 | CP932 |  |  | ULJM06032 |  | PSP: not native yet |
| ULJM05956 | JP | アルカナ・ファミリア －La storia della Arcana Famiglia－ | [0x8817914](../third_party/LunaTranslator/ppsspp_1.cpp#L2097) |  | 0 | 0 | CP932 |  |  | ULJM06032 |  | PSP: not native yet |
| ULJM06032 | JP | アルカナ・ファミリア 幽霊船の魔術師 | [0x881A214](../third_party/LunaTranslator/ppsspp_1.cpp#L2099) |  | 0 | 0 | CP932 |  |  | ULJM06032 |  | PSP: not native yet |
| ULJM06291 | JP | アルカナ・ファミリア ２ | [0x887493C](../third_party/LunaTranslator/ppsspp_1.cpp#L2101) |  | 0 | 0 | CP932 |  |  | ULJM06032 |  | PSP: not native yet |
| NPJH50858 | JP | 里見八犬伝　八珠之記 | [0x887FF84](../third_party/LunaTranslator/ppsspp_1.cpp#L2103) |  | 1 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| NPJH50899 | JP | 里見八犬伝～村雨丸之記～ | [0x88750C0](../third_party/LunaTranslator/ppsspp_1.cpp#L2105) |  | 1 | 0 | CP932 |  |  | NPJH50899 |  | PSP: not native yet |
| NPJH50885 | JP | 里見八犬伝～浜路姫之記～ | [0x886C750](../third_party/LunaTranslator/ppsspp_1.cpp#L2107) |  | 1 | 0 | CP932 |  |  | NPJH50899 |  | PSP: not native yet |
| NPJH50886 | JP | 大正鬼譚～言ノ葉櫻～ | [0x88851E8](../third_party/LunaTranslator/ppsspp_1.cpp#L2109) |  | 1 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| NPJH50833 | JP | 大正鬼譚 | [0x88487A4](../third_party/LunaTranslator/ppsspp_1.cpp#L2111) |  | 1 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| NPJH50863 | JP | 大正メビウスライン PORTABLE | [0x887EA6C](../third_party/LunaTranslator/ppsspp_1.cpp#L2113) |  | 1 | 0 | CP932 |  |  | ULJM06397 |  | PSP: not native yet |
| ULJM05951 | JP | 魔法使いとご主人様～New Ground～ | [0x8844208](../third_party/LunaTranslator/ppsspp_1.cpp#L2115) |  | 0 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| NPJH50879 | JP | 魔女王 | [0x88644D4](../third_party/LunaTranslator/ppsspp_1.cpp#L2117) |  | 1 | 0 | CP932 |  |  | NPJH50899 |  | PSP: not native yet |
| ULJM06167 | JP | 白華の檻～緋色の欠片４～ | [0x88FE8C0](../third_party/LunaTranslator/ppsspp_1.cpp#L2119) |  | 0 | 0 | CP932 |  |  | ULJM05823_2 |  | PSP: not native yet |
| ULJM06167 | JP | 白華の檻～緋色の欠片４～ | [0x88FE8D8](../third_party/LunaTranslator/ppsspp_1.cpp#L2120) |  | 1 | 0 | CP932 | FULL_STRING |  | ULJM06167 |  | PSP: not native yet |
| ULJM06314 | JP | 白華の檻 ～緋色の欠片４～ 四季の詩 | [0x8851EA0](../third_party/LunaTranslator/ppsspp_1.cpp#L2122) |  | 0 | 0 | CP932 |  |  | ULJM06266 |  | PSP: not native yet |
| ULJM06314 | JP | 白華の檻 ～緋色の欠片４～ 四季の詩 | [0x89082AC](../third_party/LunaTranslator/ppsspp_1.cpp#L2123) |  | 0 | 0 | CP932 | FULL_STRING |  | ULJM06314 |  | PSP: not native yet |
| NPJH50609 | JP | 蒼黒の楔 緋色の欠片３ ポータブル | [0x888ACD4](../third_party/LunaTranslator/ppsspp_1.cpp#L2125) |  | 0 | 0 | CP932 |  |  | ULJM05823_2 |  | PSP: not native yet |
| NPJH50609 | JP | 蒼黒の楔 緋色の欠片３ ポータブル | [0x8885390](../third_party/LunaTranslator/ppsspp_1.cpp#L2126) |  | 4 | 0 | CP932 |  |  | ULJM06289 |  | PSP: not native yet |
| ULJM06072 | JP | 蒼黒の楔 緋色の欠片3 明日への扉 | [0x894C93C](../third_party/LunaTranslator/ppsspp_1.cpp#L2128) |  | 1 | 0 | CP932 |  |  | ULJM06289 |  | PSP: not native yet |
| ULJM05725 | JP | 真・翡翠の雫 緋色の欠片２ ポータブル | [0x887CEAC](../third_party/LunaTranslator/ppsspp_1.cpp#L2130) |  | 0 | 0 | CP932 |  |  | ULJM06289 |  | PSP: not native yet |
| ULJM05725 | JP | 真・翡翠の雫 緋色の欠片２ ポータブル | [0x8876794](../third_party/LunaTranslator/ppsspp_1.cpp#L2131) |  | 0 | 0 | CP932 |  |  | ULJM05725 |  | PSP: not native yet |
| ULJM05399 | JP | 緋色の欠片ポータブル | [0x88665E4](../third_party/LunaTranslator/ppsspp_1.cpp#L2133) |  | 0 | 0 | CP932 |  |  | ULJM05943F |  | PSP: not native yet |
| ULJM05399 | JP | 緋色の欠片ポータブル | [0x8858770](../third_party/LunaTranslator/ppsspp_1.cpp#L2134) |  | 4 | 0 | CP932 |  |  | ULJM05943F |  | PSP: not native yet |
| ULJM05741 | JP | ヒイロノカケラ 新玉依姫伝承 ポータブル | [0x883FA7C](../third_party/LunaTranslator/ppsspp_1.cpp#L2136) |  | 4 | 0 | CP932 |  |  | ULJM05943F |  | PSP: not native yet |
| ULJM05741 | JP | ヒイロノカケラ 新玉依姫伝承 ポータブル | [0x8813130](../third_party/LunaTranslator/ppsspp_1.cpp#L2137) |  | 12 | 0 | CP932 |  |  | ULJM05741 |  | PSP: not native yet |
| ULJM05913 | JP | ヒイロノカケラ-Piece of Future- | [0x88FAF08](../third_party/LunaTranslator/ppsspp_1.cpp#L2139) |  | 0 | 0 | CP932 |  |  | ULJM05823_2 |  | PSP: not native yet |
| ULJM05913 | JP | ヒイロノカケラ-Piece of Future- | [0x88FAF20](../third_party/LunaTranslator/ppsspp_1.cpp#L2140) |  | 1 | 0 | CP932 |  |  | ULJM05913 |  | PSP: not native yet |
| ULJM06104 | JP | アラビアンズ・ロスト | [0x884E020](../third_party/LunaTranslator/ppsspp_1.cpp#L2142) |  | 15 | 0 | CP932 | FULL_STRING |  | ULJM06104 | 这个只是补充部分sceFontGetCharInfo提取不到的。仍需要sceFontGetCharInfo | PSP: not native yet |
| NPJH50834 | JP | アラビアンズ・ダウト | [0x8846CF4](../third_party/LunaTranslator/ppsspp_1.cpp#L2144) |  | 1 | 0 | CP932 | FULL_STRING |  | ULJM06344 |  | PSP: not native yet |
| ULJM06346 | JP | いざ、出陣！恋戦 第二幕 ～甲斐編～ | [0x8945C20](../third_party/LunaTranslator/ppsspp_1.cpp#L2146) |  | 1 | 0 | UTF-16LE |  |  | ULJM06346 |  | PSP: not native yet |
| ULJM06347 | JP | いざ、出陣！恋戦 第二幕 ～甲斐編～ | [0x8804950](../third_party/LunaTranslator/ppsspp_1.cpp#L2147) |  | 1 | 0 | UTF-16LE |  |  | ULJM06346 |  | PSP: not native yet |
| ULJM05931 | JP | AMNESIA | [0x88DABC4](../third_party/LunaTranslator/ppsspp_1.cpp#L2149) |  | 0 | 0 | CP932 |  |  | ULJM06266 |  | PSP: not native yet |
| ULJM05931 | JP | AMNESIA | [0x88C9D44](../third_party/LunaTranslator/ppsspp_1.cpp#L2150) |  | 1 | 0 | CP932 |  |  | ULJM05943F |  | PSP: not native yet |
| ULJM06044 | JP | AMNESIA LATER | [0x8880448](../third_party/LunaTranslator/ppsspp_1.cpp#L2152) |  | 1 | 0 | CP932 |  |  | ULJM05823_2 |  | PSP: not native yet |
| ULJM06044 | JP | AMNESIA LATER | [0x88F44B4](../third_party/LunaTranslator/ppsspp_1.cpp#L2153) |  | 1 | 0 | CP932 |  |  | ULJM05943F |  | PSP: not native yet |
| ULJM06266 | JP | AMNESIA CROWD | [0x8912D30](../third_party/LunaTranslator/ppsspp_1.cpp#L2155) |  | 0 | 0 | CP932 |  |  | ULJM06266 |  | PSP: not native yet |
| ULJM06266 | JP | AMNESIA CROWD | [0x890088C](../third_party/LunaTranslator/ppsspp_1.cpp#L2156) |  | 1 | 0 | CP932 |  |  | ULJM05943F |  | PSP: not native yet |
| ULJM06394 | JP | 宵夜森ノ姫 | [0x884C7C8](../third_party/LunaTranslator/ppsspp_1.cpp#L2158) |  | 1 | 0 | CP932 |  | ULJS00124_1 |  |  | PSP: not native yet |
| ULJM05945 | JP | CLOCK ZERO ～終焉の一秒～ Portable | [0x886F114](../third_party/LunaTranslator/ppsspp_1.cpp#L2160) |  | 3 | 0 | CP932 |  |  | ULJM06289 |  | PSP: not native yet |
| ULJM06220 | JP | BLACK WOLVES SAGA -Last Hope- | [0x883131C](../third_party/LunaTranslator/ppsspp_1.cpp#L2162) |  | 1 | 0 | CP932 |  |  | ULJM06220 |  | PSP: not native yet |
| ULJM06220 | JP | BLACK WOLVES SAGA -Last Hope- | [0x8831324](../third_party/LunaTranslator/ppsspp_1.cpp#L2163) |  | 1 | 0 | CP932 |  |  | ULJM06266 |  | PSP: not native yet |
| ULJM06359 | JP | クロノスタシア | [0x8812600](../third_party/LunaTranslator/ppsspp_1.cpp#L2165) |  | 1 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| ULJS00378 | JP | フォトカノ | [0x88F2030](../third_party/LunaTranslator/ppsspp_1.cpp#L2167) |  | 1 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| NPJH50892 | JP | マーメイド・ゴシック | [0x888557C](../third_party/LunaTranslator/ppsspp_1.cpp#L2169) |  | 1 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| NPJH50831 | JP | your diary+ | [0x884D740](../third_party/LunaTranslator/ppsspp_1.cpp#L2171) |  | 1 | 0 | CP932 |  |  | NPJH50831 |  | PSP: not native yet |
| NPJH50831 | JP | your diary+ | [0x8829364](../third_party/LunaTranslator/ppsspp_1.cpp#L2172) |  | 1 | 0 | CP932 |  |  | NPJH50831_1 |  | PSP: not native yet |
| NPJH50342 | JP | シャイニング・ハーツ | [0x895A470](../third_party/LunaTranslator/ppsspp_1.cpp#L2174) |  | 1 | 0 | UTF-16LE |  |  | ULJM05282 |  | PSP: not native yet |
| ULJS00019 | JP | スクールランブル ～姉さん事件です！～ | [0x882C1C0](../third_party/LunaTranslator/ppsspp_1.cpp#L2176) |  | 1 | 0 | CP932 |  |  | ULJS00019 |  | PSP: not native yet |
| NPJH50711 | JP | 水平線まで何マイル？ - ORIGINAL FLIGHT - | [0x8922D2C](../third_party/LunaTranslator/ppsspp_1.cpp#L2178) |  | 3 | 0 | CP932 |  |  | NPJH50711 |  | PSP: not native yet |
| ULJM05858 | JP | ガーネット・クレイドル ポータブル ～鍵の姫巫女～ | [0x88970F4](../third_party/LunaTranslator/ppsspp_1.cpp#L2180) |  | 0 | 0 | CP932 |  |  | ULJM05823_2 |  | PSP: not native yet |
| ULJM05858 | JP | ガーネット・クレイドル ポータブル ～鍵の姫巫女～ | [0x8873EB4](../third_party/LunaTranslator/ppsspp_1.cpp#L2181) |  | 3 | 0 | CP932 |  |  | ULJM05943F |  | PSP: not native yet |
| ULJM05891 | JP | さかあがりハリケーン Portable | [0x880AF50](../third_party/LunaTranslator/ppsspp_1.cpp#L2183) |  | 0 | 0 | CP932 |  |  | ULJM05891 |  | PSP: not native yet |
| ULJM05908 | JP | 快盗天使ツインエンジェル～時とセカイの迷宮～ | [0x880838C](../third_party/LunaTranslator/ppsspp_1.cpp#L2185) |  | 1 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| ULJM05587 | JP | テガミバチ　こころ紡ぐ者へ | [0x883172C](../third_party/LunaTranslator/ppsspp_1.cpp#L2187) |  | 1 | 0 | UTF-16LE |  |  | ULJM05587_1 |  | PSP: not native yet |
| ULJM05587 | JP | テガミバチ　こころ紡ぐ者へ | [0x88316F8](../third_party/LunaTranslator/ppsspp_1.cpp#L2188) |  | 1 | 0 | UTF-16LE |  |  | ULJM05587_2 |  | PSP: not native yet |
| ULJM06147 | JP | 闇からのいざない TENEBRAE I | [0x88143A0](../third_party/LunaTranslator/ppsspp_1.cpp#L2190) |  | 2 | 0 | UTF-16LE |  |  | ULJM06147 |  | PSP: not native yet |
| ULJM05672 | JP | ＧＡ 芸術科アートデザインクラス Slapstick WONDERLAND | [0x8858E44](../third_party/LunaTranslator/ppsspp_1.cpp#L2192) |  | 0 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| ULJM06110 | JP | この部室は帰宅しない部が占拠しました。ぽーたぶる　学園ドッグ・イヤー編 | [0x88F91DC](../third_party/LunaTranslator/ppsspp_1.cpp#L2194) |  | 2 | 0 | CP932 |  |  | FNPJH50127 |  | PSP: not native yet |
| ULJM06261 | JP | 夏空のモノローグ portable | [0x88756D0](../third_party/LunaTranslator/ppsspp_1.cpp#L2196) |  | 3 | 0 | CP932 |  |  | NPJH50831 |  | PSP: not native yet |
| ULJS00599 | JP | 月影の鎖　～狂爛モラトリアム～ | [0x882CAE8](../third_party/LunaTranslator/ppsspp_1.cpp#L2198) |  | 4 | 0 | CP932 |  |  | ULJS00579 |  | PSP: not native yet |
| ULJS00579 | JP | 月影の鎖　～錯乱パラノイア～ | [0x882C010](../third_party/LunaTranslator/ppsspp_1.cpp#L2200) |  | 4 | 0 | CP932 |  |  | ULJS00579 |  | PSP: not native yet |
| ULJM05657 | JP | リアルロデ PORTABLE | [0x886A92C](../third_party/LunaTranslator/ppsspp_1.cpp#L2202) |  | 0 | 0 | CP932 |  |  | ULJM05657 |  | PSP: not native yet |
| ULJM06036 | JP | Princess Evangile | [0x88506D0](../third_party/LunaTranslator/ppsspp_1.cpp#L2204) |  | 2 | 0 | UTF-16LE |  |  | ULJM06036_filter |  | PSP: not native yet |
| ULJM06258 | JP | Princess Arthur | [0x8841D10](../third_party/LunaTranslator/ppsspp_1.cpp#L2206) |  | 14 | 0 | CP932 |  |  | ULJM06258_2 | name+text,显示完后 | PSP: not native yet |
| ULJM06258 | JP | Princess Arthur | [0x88A844C](../third_party/LunaTranslator/ppsspp_1.cpp#L2207) |  | 1 | 0 | CP932 |  |  | ULJM06258 | text | PSP: not native yet |
| ULJM05609 | JP | アルコバレーノ！ポータブル | [0x88AFECC](../third_party/LunaTranslator/ppsspp_1.cpp#L2209) |  | 4 | 0 | CP932 |  |  | ULJM05943F |  | PSP: not native yet |
| ULJM06090 | JP | 十三支演義 ～偃月三国伝～ | [0x891BC1C](../third_party/LunaTranslator/ppsspp_1.cpp#L2211) |  | 0 | 0 | CP932 |  |  | ULJM06289 |  | PSP: not native yet |
| ULJM06367 | JP | 十三支演義 偃月三国伝2 | [0x88CF2A4](../third_party/LunaTranslator/ppsspp_1.cpp#L2213) |  | 0 | 0 | CP932 |  |  | ULJM06289 |  | PSP: not native yet |
| ULJM05758 | JP | さくらさくら-HARU URARA- | [0x8817C98](../third_party/LunaTranslator/ppsspp_1.cpp#L2215) |  | 1 | 0 | CP932 |  |  | ULJM05758 |  | PSP: not native yet |
| ULJS00380 | JP | どきどきすいこでん | [0x88A8FC8](../third_party/LunaTranslator/ppsspp_1.cpp#L2217) |  | 1 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| ULJM06326 | JP | Jewelic Nightmare | [0x888BF24](../third_party/LunaTranslator/ppsspp_1.cpp#L2219) |  | 0 | 0 | CP932 |  |  | ULJM06289 |  | PSP: not native yet |
| ULJM05276 | JP | 怪盗アプリコット ポータブル | [0x8823474](../third_party/LunaTranslator/ppsspp_1.cpp#L2221) |  | 1 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| ULJM05276 | JP | 怪盗アプリコット ポータブル | [0x8823DF0](../third_party/LunaTranslator/ppsspp_1.cpp#L2222) |  | 1 | 0 | CP932 |  |  | ULJM05276 |  | PSP: not native yet |
| ULJM05252 | JP | EtudePrologue ポータブル | [0x881D160](../third_party/LunaTranslator/ppsspp_1.cpp#L2224) |  | 0 | 0 | CP932 |  |  | ULJM05276 |  | PSP: not native yet |
| ULJM05252 | JP | EtudePrologue ポータブル | [0x8822894](../third_party/LunaTranslator/ppsspp_1.cpp#L2225) |  | 0 | 0 | CP932 |  |  | ULJM05276 |  | PSP: not native yet |
| ULJM05252 | JP | EtudePrologue ポータブル | [0x881CA64](../third_party/LunaTranslator/ppsspp_1.cpp#L2226) |  | 6 | 0 | CP932 |  |  | ULJM05276 |  | PSP: not native yet |
| ULJM05258 | JP | for Symphony ポータブル | [0x8821B50](../third_party/LunaTranslator/ppsspp_1.cpp#L2228) |  | 1 | 0 | CP932 |  |  | ULJM05276 |  | PSP: not native yet |
| ULJM05258 | JP | for Symphony ポータブル | [0x881C354](../third_party/LunaTranslator/ppsspp_1.cpp#L2229) |  | 0 | 0 | CP932 |  |  | ULJM05276 |  | PSP: not native yet |
| ULJM05258 | JP | for Symphony ポータブル | [0x881BC74](../third_party/LunaTranslator/ppsspp_1.cpp#L2230) |  | 1 | 0 | CP932 |  |  | ULJM05276 |  | PSP: not native yet |
| ULJM05203 | JP | Kanon | [0x880B6E8](../third_party/LunaTranslator/ppsspp_1.cpp#L2232) |  | 0 | 0 | UTF-16LE |  |  | ULJM05203 |  | PSP: not native yet |
| ULJM05249 | JP | Ｌｉｔｔｌｅ　Ａｉｄ　ポータブル | [0x8822B6C](../third_party/LunaTranslator/ppsspp_1.cpp#L2234) |  | 1 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| ULJM05249 | JP | Ｌｉｔｔｌｅ　Ａｉｄ　ポータブル | [0x8823314](../third_party/LunaTranslator/ppsspp_1.cpp#L2235) |  | 1 | 0 | CP932 |  |  | ULJM05249 |  | PSP: not native yet |
| NPJH00122 | JP | とびたて！超時空トラぶる花札大作戦 | [0x8842E48](../third_party/LunaTranslator/ppsspp_1.cpp#L2237) |  | 2 | 0 | CP932 |  |  | NPJH00122 |  | PSP: not native yet |
| ULJM05175 | JP | ふしぎ遊戯 玄武開伝 外伝 鏡の巫女 | [0x884FC48](../third_party/LunaTranslator/ppsspp_1.cpp#L2239) |  | 0 | 0 | CP932 |  |  | ULJM05823_2 |  | PSP: not native yet |
| ULJM05175 | JP | ふしぎ遊戯 玄武開伝 外伝 鏡の巫女 | [0x889B8D4](../third_party/LunaTranslator/ppsspp_1.cpp#L2240) |  | 0 | 0 | CP932 |  |  | ULJM06258 |  | PSP: not native yet |
| ULJM05109 | JP | サクラ大戦１＆２ | [0x8A7F814](../third_party/LunaTranslator/ppsspp_1.cpp#L2242) |  | 6 | 0 | CP932 |  |  | ULJM05109 | 其一。不支持其二 | PSP: not native yet |
| ULJM05954 | JP | 白銀のカルと蒼空の女王 | [0x8859358](../third_party/LunaTranslator/ppsspp_1.cpp#L2244) |  | 3 | 0 | CP932 |  |  | ULJM05954 |  | PSP: not native yet |
| ULJM05954 | JP | 白銀のカルと蒼空の女王 | [0x884F338](../third_party/LunaTranslator/ppsspp_1.cpp#L2245) |  | 0 | 0 | CP932 |  |  | ULJM05954 |  | PSP: not native yet |
| ULJM06397 | JP | カレイドイヴ | [0x8837E98](../third_party/LunaTranslator/ppsspp_1.cpp#L2247) |  | 3 | 0 | CP932 |  |  | ULJM06397 |  | PSP: not native yet |
| ULJM05634 | JP | 天神乱漫 Happy Go Lucky !! | [0x885C730](../third_party/LunaTranslator/ppsspp_1.cpp#L2249) |  | 14 | 0 | CP932 |  |  | ULJM05634 |  | PSP: not native yet |
| ULJM06378 | JP | 学園K -Wonderful School Days- | [0x887F838](../third_party/LunaTranslator/ppsspp_1.cpp#L2251) |  | 1 | 0 | CP932 |  |  | ULJM06378 |  | PSP: not native yet |
| NPJH50754 | JP | 学☆王 -THE ROYAL SEVEN STARS- +METEOR | [0x880E458](../third_party/LunaTranslator/ppsspp_1.cpp#L2253) |  | 0 | 0 | CP932 |  |  | NPJH50754 |  | PSP: not native yet |
| NPJH50888 | JP | 黒雪姫～スノウ・マジック～ | [0x8886480](../third_party/LunaTranslator/ppsspp_1.cpp#L2255) |  | 1 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| NPJH50866 | JP | 黒雪姫～スノウ・ブラック～ | [0x887FBF0](../third_party/LunaTranslator/ppsspp_1.cpp#L2257) |  | 1 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| ULJM06318 | JP | ロミオVSジュリエット | [0x887B4A4](../third_party/LunaTranslator/ppsspp_1.cpp#L2259) |  | 1 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| NPJH50862 | JP | ロミオ＆ジュリエット | [0x88696AC](../third_party/LunaTranslator/ppsspp_1.cpp#L2261) |  | 1 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| NPJH50269 | JP | うたの☆プリンスさまっ♪ | [0x8854F28](../third_party/LunaTranslator/ppsspp_1.cpp#L2263) |  | 0 | 0 | CP932 | USING_CHAR |  | NPJH50269 |  | PSP: not native yet |
| NPJH50446 | JP | うたの☆プリンスさまっ♪Repeat | [0x8853A34](../third_party/LunaTranslator/ppsspp_1.cpp#L2265) |  | 0 | 0 | CP932 | USING_CHAR |  | NPJH50269 |  | PSP: not native yet |
| NPJH50734 | JP | うたの☆プリンスさまっ♪All Star | [0x88789BC](../third_party/LunaTranslator/ppsspp_1.cpp#L2267) |  | 2 | 0 | CP932 | USING_CHAR |  | NPJH50269 |  | PSP: not native yet |
| NPJH50902 | JP | うたの☆プリンスさまっ♪All Star After Secret | [0x885DF08](../third_party/LunaTranslator/ppsspp_1.cpp#L2269) |  | 0 | 0 | CP932 | USING_CHAR |  | NPJH50269 |  | PSP: not native yet |
| NPJH50381 | JP | うたの☆プリンスさまっ♪-Amazing Aria- | [0x88545C4](../third_party/LunaTranslator/ppsspp_1.cpp#L2271) |  | 0 | 0 | CP932 | USING_CHAR |  | NPJH50269 |  | PSP: not native yet |
| NPJH50500 | JP | うたの☆プリンスさまっ♪Debut | [0x8851D20](../third_party/LunaTranslator/ppsspp_1.cpp#L2273) |  | 0 | 0 | CP932 | USING_CHAR |  | NPJH50269 |  | PSP: not native yet |
| NPJH50393 | JP | うたの☆プリンスさまっ♪-Sweet Serenade- | [0x8854694](../third_party/LunaTranslator/ppsspp_1.cpp#L2275) |  | 0 | 0 | CP932 | USING_CHAR |  | NPJH50269 |  | PSP: not native yet |
| ULJM06006 | JP | スカーレッドライダーゼクス | [0x8863104](../third_party/LunaTranslator/ppsspp_1.cpp#L2277) |  | 1 | 0 | CP932 |  |  | ULJM06006 |  | PSP: not native yet |
| ULJM06007 | JP | スカーレッドライダーゼクス　スターダストラバーズ | [0x8862D80](../third_party/LunaTranslator/ppsspp_1.cpp#L2279) |  | 1 | 0 | CP932 |  |  | ULJM06006 |  | PSP: not native yet |
| ULJM06046 | JP | エルクローネのアトリエ ～Dear for Otomate～ | [0x8893418](../third_party/LunaTranslator/ppsspp_1.cpp#L2281) |  | 0 | 0 | CP932 |  |  | ULJM05943F |  | PSP: not native yet |
| ULJM06344 | JP | 放課後colorful＊step～うんどうぶ！～ | [0x882BDAC](../third_party/LunaTranslator/ppsspp_1.cpp#L2283) |  | 1 | 0 | CP932 |  |  | ULJM06344 |  | PSP: not native yet |
| ULJM06363 | JP | 放課後colorful＊step～ぶんかぶ！～ | [0x8817AD0](../third_party/LunaTranslator/ppsspp_1.cpp#L2285) |  | 1 | 0 | CP932 |  |  | ULJM06344 |  | PSP: not native yet |
| ULJM05949 | JP | お菓子な島のピーターパン～Sweet Never Land～ | [0x8883EE0](../third_party/LunaTranslator/ppsspp_1.cpp#L2287) |  | 0 | 0 | UTF-16LE | DATA_INDIRECT USING_CHAR |  |  |  | PSP: not native yet |
| ULJM06276 | JP | NORN9 ノルン＋ノネット | [0x88AF7DC](../third_party/LunaTranslator/ppsspp_1.cpp#L2289) |  | 0 | 0 | CP932 |  |  | ULJM05943F |  | PSP: not native yet |
| ULJM06276 | JP | NORN9 ノルン＋ノネット | [0x8852B38](../third_party/LunaTranslator/ppsspp_1.cpp#L2290) |  | 0 | 0 | CP932 |  |  | ULJM06289 |  | PSP: not native yet |
| ULJM06015 | JP | 官能昔話 ポータブル | [0x88764EC](../third_party/LunaTranslator/ppsspp_1.cpp#L2292) |  | 12 | 0 | CP932 |  |  | ULJM05867_1 |  | PSP: not native yet |
| ULJM06015 | JP | 官能昔話 ポータブル | [0x88764D4](../third_party/LunaTranslator/ppsspp_1.cpp#L2293) |  | 12 | 0 | CP932 |  |  | ULJM05823_2 |  | PSP: not native yet |
| ULJM05834 | JP | Wand of Fortune2 | [0x890EC60](../third_party/LunaTranslator/ppsspp_1.cpp#L2295) |  | 3 | 0 | CP932 |  |  | ULJM05943F |  | PSP: not native yet |
| ULJM06194 | JP | ワンド オブ フォーチュン2 FD ～君に捧げるエピローグ～ | [0x888B11C](../third_party/LunaTranslator/ppsspp_1.cpp#L2297) |  | 0 | 0 | CP932 |  |  | ULJM06289 |  | PSP: not native yet |
| ULJM05783 | JP | ワンド オブ フォーチュン ～未来へのプロローグ～ ポータブル | [0x887F2C0](../third_party/LunaTranslator/ppsspp_1.cpp#L2299) |  | 0 | 0 | CP932 |  |  | ULJM05943F |  | PSP: not native yet |
| ULJM05783 | JP | ワンド オブ フォーチュン ～未来へのプロローグ～ ポータブル | [0x88D4844](../third_party/LunaTranslator/ppsspp_1.cpp#L2300) |  | 0 | 0 | CP932 |  |  | ULJM05783 |  | PSP: not native yet |
| ULJM06116 | JP | グリム・ザ・バウンティハンター | [0x88385E0](../third_party/LunaTranslator/ppsspp_1.cpp#L2302) |  | 1 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| ULJM06249 | JP | デザート・キングダム ポータブル | [0x88274D0](../third_party/LunaTranslator/ppsspp_1.cpp#L2304) |  | 1 | 0 | CP932 |  |  | ULJM05823_2 |  | PSP: not native yet |
| ULJM06249 | JP | デザート・キングダム ポータブル | [0x88730AC](../third_party/LunaTranslator/ppsspp_1.cpp#L2305) |  | 1 | 0 | CP932 | FULL_STRING |  | ULJM05943F |  | PSP: not native yet |
| ULJM05895 | JP | 猛獣使いと王子様 Portable | [0x88FFEF0](../third_party/LunaTranslator/ppsspp_1.cpp#L2307) |  | 0 | 0 | CP932 |  |  | ULJM05943F |  | PSP: not native yet |
| ULJM05895 | JP | 猛獣使いと王子様 Portable | [0x8879C38](../third_party/LunaTranslator/ppsspp_1.cpp#L2308) |  | 0 | 0 | CP932 |  |  | ULJM05943F |  | PSP: not native yet |
| ULJM06030 | JP | 猛獣使いと王子様　～Snow Bride～　Portable | [0x88220A8](../third_party/LunaTranslator/ppsspp_1.cpp#L2310) |  | 1 | 0 | CP932 |  |  | ULJM05943F |  | PSP: not native yet |
| ULJM06073 | JP | 恋戦隊LOVE＆PEACE　THE Ｐ.Ｓ.Ｐ. | [0x8819D18](../third_party/LunaTranslator/ppsspp_1.cpp#L2312) |  | 1 | 0 | CP932 |  |  | ULJM05770 |  | PSP: not native yet |
| ULJM05411 | JP | 智代アフター ～It's a Wonderful Life～CS Edition | [0x880ED98](../third_party/LunaTranslator/ppsspp_1.cpp#L2314) |  | 0 | 0 | UTF-16LE |  |  | ULJM05282 |  | PSP: not native yet |
| ULJM06375 | JP | Enkeltbillet | [0x8922460](../third_party/LunaTranslator/ppsspp_1.cpp#L2316) |  | 1 | 0 | CP932 |  |  | ULJM06378 |  | PSP: not native yet |
| ULJM06237 | JP | 乙女的恋革命★ラブレボ!!　100kgからはじまる→恋物語 | [0x887FC28](../third_party/LunaTranslator/ppsspp_1.cpp#L2318) |  | 7 | 0 | UTF-16LE |  |  | NewLineCharFilterW |  | PSP: not native yet |
| ULJM05574 | JP | 11eyes CrossOver | [0x89A7C2C](../third_party/LunaTranslator/ppsspp_1.cpp#L2320) |  | 0 | 0 | CP932 |  |  | ULJM05574 |  | PSP: not native yet |
| NPJH50737 | JP | 1/2summer+ | [0x88225D4](../third_party/LunaTranslator/ppsspp_1.cpp#L2322) |  | 1 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| NPJH50753 | JP | TIGER & BUNNY ～HERO'S DAY～ | [0x88434E0](../third_party/LunaTranslator/ppsspp_1.cpp#L2324) |  | 13 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| NPJH50631 | JP | スーパーダンガンロンパ２　さよなら絶望学園 | [0x88D1A24](../third_party/LunaTranslator/ppsspp_1.cpp#L2326) |  | 0 | 0 | UTF-16LE |  |  | NPJH50515 |  | PSP: not native yet |
| NPJH50515 | JP | ダンガンロンパ　希望の学園と絶望の高校生　PSP® the Best | [0x88540B0](../third_party/LunaTranslator/ppsspp_1.cpp#L2328) |  | 0 | 0 | UTF-16LE |  |  | NPJH50515 |  | PSP: not native yet |
| ULJM05445 | JP | 12Riven　-the Ψcliminal of integral- | [0x8942AD4](../third_party/LunaTranslator/ppsspp_1.cpp#L2330) |  | 0 | 0 | CP932 |  |  | ULJM06040_1 |  | PSP: not native yet |
| ULJM06019 | JP | リトルウィッチ パルフェ　黒猫魔法店物語 | [0x8840D18](../third_party/LunaTranslator/ppsspp_1.cpp#L2332) |  | 0 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| NPJH50877 | JP | ＢＬＡＣＫ ＣＯＤＥ　ブラック・コード | [0x88733E4](../third_party/LunaTranslator/ppsspp_1.cpp#L2334) |  | 1 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| ULJS00600 | JP | 紫影のソナーニルRefrain | [0x88126C4](../third_party/LunaTranslator/ppsspp_1.cpp#L2336) |  | 1 | 0 | UTF-8 |  |  | ULJS00600 |  | PSP: not native yet |
| NPJH50796 | JP | スズノネセブン！ Portable | [0x883002C](../third_party/LunaTranslator/ppsspp_1.cpp#L2338) |  | 1 | 0 | CP932 |  |  | NPJH50796 | 有人名，但要显示完才输出 | PSP: not native yet |
| NPJH50796 | JP | スズノネセブン！ Portable | [0x8849710](../third_party/LunaTranslator/ppsspp_1.cpp#L2339) |  | 3 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| ULJM05251 | JP | シークレット オブ エヴァンゲリオン ポータブル | [0x883F774](../third_party/LunaTranslator/ppsspp_1.cpp#L2341) |  | 13 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| ULJM05562 | JP | 学園ヘヴン BOY'S LOVE SCRAMBLE! | [0x8811044](../third_party/LunaTranslator/ppsspp_1.cpp#L2343) |  | 0 | 0 | UTF-16LE |  |  | ULJM05203 |  | PSP: not native yet |
| ULJM05563 | JP | 学園ヘヴン BOY'S LOVE SCRAMBLE! | [0x8811044](../third_party/LunaTranslator/ppsspp_1.cpp#L2343) |  | 0 | 0 | UTF-16LE |  |  | ULJM05203 |  | PSP: not native yet |
| ULJM05729 | JP | 学園ヘヴン　おかわりっ！ | [0x8813880](../third_party/LunaTranslator/ppsspp_1.cpp#L2345) |  | 0 | 0 | UTF-16LE |  |  | ULJM05203 |  | PSP: not native yet |
| ULJM05795 | JP | 咎狗の血 T B P | [0x8836918](../third_party/LunaTranslator/ppsspp_1.cpp#L2347) |  | 13 | 0 | CP932 |  |  | ULJM05795 |  | PSP: not native yet |
| ULJM06231 | JP | いっしょにごはん。PORTABLE | [0x883E550](../third_party/LunaTranslator/ppsspp_1.cpp#L2349) |  | 0 | 0 | CP932 |  |  | ULJM06289 |  | PSP: not native yet |
| NPJH50714 | JP | 鬼ごっこ！Portable | [0x881D500](../third_party/LunaTranslator/ppsspp_1.cpp#L2351) |  | 3 | 0 | UTF-16LE |  |  |  |  | PSP: not native yet |
| ULJM05639 | JP | そらのおとしもの　－ドキドキサマーバケーション－ | [0x88196C0](../third_party/LunaTranslator/ppsspp_1.cpp#L2353) |  | 2 | 0 | CP932 |  |  | ULJM05639 |  | PSP: not native yet |
| ULJS00357 | JP | 俺の妹めいかぁEX | [0x88CFDA0](../third_party/LunaTranslator/ppsspp_1.cpp#L2355) |  | 0 | 0 | CP932 |  |  | ULJS00357 |  | PSP: not native yet |
| ULJS00471 | JP | VitaminX Detective B6 | [0x892BC1C](../third_party/LunaTranslator/ppsspp_1.cpp#L2357) |  | 12 | 0 | CP932 |  |  | ULJS00471 |  | PSP: not native yet |
| ULJS00325 | JP | VitaminX Evolution Plus | [0x884A210](../third_party/LunaTranslator/ppsspp_1.cpp#L2359) |  | 1 | 0 | CP932 |  |  | ULJS00471 |  | PSP: not native yet |
| ULJS00325 | JP | VitaminX Evolution Plus | [0x884A698](../third_party/LunaTranslator/ppsspp_1.cpp#L2360) |  | 1 | 0 | CP932 |  |  | ULJS00471 |  | PSP: not native yet |
| ULJS00347 | JP | VitaminXtoZ | [0x88A5278](../third_party/LunaTranslator/ppsspp_1.cpp#L2362) |  | 1 | 0 | CP932 |  |  | ULJS00471 |  | PSP: not native yet |
| ULJS00592 | JP | VitaminR | [0x885C180](../third_party/LunaTranslator/ppsspp_1.cpp#L2364) |  | 0 | 0 | CP932 |  |  | ULJS00592 |  | PSP: not native yet |
| ULJS00561 | JP | VitaminZ Graduation | [0x88E4490](../third_party/LunaTranslator/ppsspp_1.cpp#L2366) |  | 1 | 0 | CP932 |  |  | ULJS00471 |  | PSP: not native yet |
| ULJS00561 | JP | VitaminZ Graduation | [0x88C04C4](../third_party/LunaTranslator/ppsspp_1.cpp#L2367) |  | 1 | 0 | CP932 |  |  | ULJS00471 |  | PSP: not native yet |
| ULJS00278 | JP | VitaminZ Revolution | [0x88A9330](../third_party/LunaTranslator/ppsspp_1.cpp#L2369) |  | 1 | 0 | CP932 |  |  | ULJS00471 |  | PSP: not native yet |
| ULJS00278 | JP | VitaminZ Revolution | [0x88A674C](../third_party/LunaTranslator/ppsspp_1.cpp#L2370) |  | 1 | 0 | CP932 |  |  | ULJS00471 |  | PSP: not native yet |
| ULJM06115 | JP | あの日見た花の名前を僕達はまだ知らない。 | [0x8821DBC](../third_party/LunaTranslator/ppsspp_1.cpp#L2372) |  | 1 | 0 | CP932 |  |  | ULJM06115 |  | PSP: not native yet |
| ULJM06115 | JP | あの日見た花の名前を僕達はまだ知らない。 | [0x8821F58](../third_party/LunaTranslator/ppsspp_1.cpp#L2373) |  | 1 | 0 | CP932 |  |  | ULJM06115 |  | PSP: not native yet |
| NPJH50489 | JP | 青の祓魔師 幻刻の迷宮 | [0x88282D8](../third_party/LunaTranslator/ppsspp_1.cpp#L2375) |  | 2 | 0 | CP932 |  |  | NPJH50489 |  | PSP: not native yet |
| ULJM06078 | JP | 断罪のマリア　～ラ･カンパネラ～ | [0x8884610](../third_party/LunaTranslator/ppsspp_1.cpp#L2377) |  | 3 | 0 | CP932 |  |  |  | 缺少自定义人名，sceFontGetCharInfo有人名但有别的乱七八糟的东西 | PSP: not native yet |
| ULJM05625 | JP | 夜明け前より瑠璃色な PORTABLE | [0x8864438](../third_party/LunaTranslator/ppsspp_1.cpp#L2379) |  | 0 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| ULJS00154 | JP | To LOVEる-とらぶる-　ドキドキ！臨海学校編 | [0x8863424](../third_party/LunaTranslator/ppsspp_1.cpp#L2381) |  | 0 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| ULJM05968 | JP | うみねこのなく頃に Portable 1 | [0x884B138](../third_party/LunaTranslator/ppsspp_1.cpp#L2383) |  | 0 | 0 | CP932 |  |  | ULJM05968 |  | PSP: not native yet |
| ULJM05969 | JP | うみねこのなく頃に Portable 2 | [0x885DF94](../third_party/LunaTranslator/ppsspp_1.cpp#L2385) |  | 0 | 0 | CP932 |  |  | ULJM05968 |  | PSP: not native yet |
| ULJM06259 | JP | 死神稼業～怪談ロマンス～ | [0x8842490](../third_party/LunaTranslator/ppsspp_1.cpp#L2387) |  | 1 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| ULJM06224 | JP | Stellar☆Theater Portable | [0x88817F4](../third_party/LunaTranslator/ppsspp_1.cpp#L2389) |  | 0 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| ULJS00563 | JP | 英国探偵ミステリア | [0x887AE48](../third_party/LunaTranslator/ppsspp_1.cpp#L2391) |  | 3 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| ULJM06305 | JP | 変態王子と笑わない猫。 | [0x8964190](../third_party/LunaTranslator/ppsspp_1.cpp#L2393) |  | 2 | 0 | UTF-16LE |  |  | ULJM05282 |  | PSP: not native yet |
| ULJM05848 | JP | Are you Alice? | [0x897B7F4](../third_party/LunaTranslator/ppsspp_1.cpp#L2395) |  | 0 | 0 | CP932 |  |  | ULJM06289 |  | PSP: not native yet |
| ULJM05768 | JP | 加奈～いもうと～ | [0x8849070](../third_party/LunaTranslator/ppsspp_1.cpp#L2397) |  | 1 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| NPJH50745 | JP | さくら荘のペットな彼女 | [0x8855888](../third_party/LunaTranslator/ppsspp_1.cpp#L2399) |  | 1 | 0 | UTF-8 |  |  | NPJH50745 |  | PSP: not native yet |
| ULJM06311 | JP | DIABOLIK LOVERS MORE,BLOOD | [0x8837004](../third_party/LunaTranslator/ppsspp_1.cpp#L2401) |  | 3 | 0 | CP932 |  |  | ULJM06311 |  | PSP: not native yet |
| ULJM06311 | JP | DIABOLIK LOVERS MORE,BLOOD | [0x883AB80](../third_party/LunaTranslator/ppsspp_1.cpp#L2402) |  | 1 | 0 | CP932 |  |  | ULJM06311_1 |  | PSP: not native yet |
| ULJM06163 | JP | DIABOLIK LOVERS | [0x8837608](../third_party/LunaTranslator/ppsspp_1.cpp#L2404) |  | 0 | 0 | CP932 |  |  | ULJM06311 |  | PSP: not native yet |
| ULJS00459 | JP | 僕は友達が少ない　ぽーたぶる | [0x8816FD4](../third_party/LunaTranslator/ppsspp_1.cpp#L2406) |  | 2 | 0 | UTF-16LE |  |  | ULJS00459 |  | PSP: not native yet |
| ULJM05870 | JP | あさき、ゆめみし | [0x881F514](../third_party/LunaTranslator/ppsspp_1.cpp#L2408) |  | 0 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| ULJM05988 | JP | BEYOND THE FUTURE - FIX THE TIME ARROWS - | [0x8909E64](../third_party/LunaTranslator/ppsspp_1.cpp#L2410) |  | 14 | 0 | UTF-8 |  |  | ULJM05433 |  | PSP: not native yet |
| ULJS00318 | JP | デュラララ!! 3way standoff | [0x8858BA4](../third_party/LunaTranslator/ppsspp_1.cpp#L2412) |  | 2 | 0 | CP932 |  |  | NPJH50700 |  | PSP: not native yet |
| ULJM05960 | JP | Ｒ－１５ ぽーたぶる | [0x8820570](../third_party/LunaTranslator/ppsspp_1.cpp#L2414) |  | 13 | 0 | CP932 |  |  | ULJM05960 |  | PSP: not native yet |
| ULJM06296 | JP | キサラギGOLD★STAR - NONSTOP GO GO!! - | [0x897AFB0](../third_party/LunaTranslator/ppsspp_1.cpp#L2416) |  | 0 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| ULJM06191 | JP | スクール・ウォーズ | [0x883EE30](../third_party/LunaTranslator/ppsspp_1.cpp#L2418) |  | 1 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| ULJM06283 | JP | スクール・ウォーズ～卒業戦線～ | [0x885E3A0](../third_party/LunaTranslator/ppsspp_1.cpp#L2420) |  | 1 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| ULJM06316 | JP | BROTHERS CONFLICT  Brilliant Blue | [0x88E7100](../third_party/LunaTranslator/ppsspp_1.cpp#L2422) |  | 0 | 0 | CP932 |  |  | ULJM06316 |  | PSP: not native yet |
| ULJM06316 | JP | BROTHERS CONFLICT  Brilliant Blue | [0x89781F4](../third_party/LunaTranslator/ppsspp_1.cpp#L2423) |  | 0 | 0 | CP932 |  |  | ULJM06289 |  | PSP: not native yet |
| ULJM06066 | JP | BROTHERS CONFLICT Passion Pink | [0x88D85C8](../third_party/LunaTranslator/ppsspp_1.cpp#L2425) |  | 0 | 0 | UTF-16LE |  |  | ULJM06066 |  | PSP: not native yet |
| ULJM06174 | JP | 暁の護衛 トリニティ | [0x8920768](../third_party/LunaTranslator/ppsspp_1.cpp#L2427) |  | 0 | 0 | UTF-16LE |  |  | ULJM06174 |  | PSP: not native yet |
| ULJM06175 | JP | 暁の護衛 トリニティ | [0x8920768](../third_party/LunaTranslator/ppsspp_1.cpp#L2427) |  | 0 | 0 | UTF-16LE |  |  | ULJM06174 |  | PSP: not native yet |
| ULJM06123 | JP | 三国恋戦記 | [0x8896A0C](../third_party/LunaTranslator/ppsspp_1.cpp#L2429) |  | 0 | 0 | CP932 |  |  | ULJM05758 |  | PSP: not native yet |
| ULJM06124 | JP | 三国恋戦記 | [0x8896A0C](../third_party/LunaTranslator/ppsspp_1.cpp#L2429) |  | 0 | 0 | CP932 |  |  | ULJM05758 |  | PSP: not native yet |
| ULJM06232 | JP | グリザイアの果実 -LE FRUIT DE LA GRISAIA- | [0x8840324](../third_party/LunaTranslator/ppsspp_1.cpp#L2431) |  | 4 | 0 | CP932 |  |  | ULJM06232 |  | PSP: not native yet |
| ULJM06233 | JP | グリザイアの果実 -LE FRUIT DE LA GRISAIA- | [0x8840324](../third_party/LunaTranslator/ppsspp_1.cpp#L2431) |  | 4 | 0 | CP932 |  |  | ULJM06232 |  | PSP: not native yet |
| ULJM06107 | JP | Dies irae ～Amantes amentes～ | [0x88E08D0](../third_party/LunaTranslator/ppsspp_1.cpp#L2433) |  | 3 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| ULJM06108 | JP | Dies irae ～Amantes amentes～ | [0x88E08D0](../third_party/LunaTranslator/ppsspp_1.cpp#L2433) |  | 3 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| ULJM05701 | JP | 花帰葬 | [0x88139F4](../third_party/LunaTranslator/ppsspp_1.cpp#L2435) |  | 0 | 0 | UTF-16LE |  |  | ULJM05701 |  | PSP: not native yet |
| ULJM06334 | JP | 死神所業～怪談ロマンス～ | [0x8857390](../third_party/LunaTranslator/ppsspp_1.cpp#L2437) |  | 1 | 0 | CP932 |  |  |  |  | PSP: not native yet |
| ULJM05703 | JP | 二世の契り | [0x888D34C](../third_party/LunaTranslator/ppsspp_1.cpp#L2439) |  | 3 | 0 | CP932 |  |  | ULJM05703 |  | PSP: not native yet |
| ULJM05915 | JP | 二世の契り　想い出の先へ | [0x88744A8](../third_party/LunaTranslator/ppsspp_1.cpp#L2441) |  | 3 | 0 | CP932 |  |  | ULJM05915 |  | PSP: not native yet |
| ULJM06183 | JP | 君が主で執事が俺で～お仕え日記～ぽーたぶる | [0x882135C](../third_party/LunaTranslator/ppsspp_1.cpp#L2443) |  | 1 | 0 | CP932 |  |  | ULJM06183 |  | PSP: not native yet |
| ULJS00343 | JP | 探偵オペラ ミルキィホームズ | [0x88AF23C](../third_party/LunaTranslator/ppsspp_1.cpp#L2445) |  | 15 | 0 | UTF-8 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| ULJS00520 | JP | 探偵オペラ　ミルキィホームズ　２ | [0x88B3848](../third_party/LunaTranslator/ppsspp_1.cpp#L2447) |  | 15 | 0 | UTF-8 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| ULJM06173 | JP | TOKYOヤマノテBOYS Portable DARK CHERRY DISC | [0x8856FC8](../third_party/LunaTranslator/ppsspp_1.cpp#L2449) |  | 0 | 0 | CP932 | FULL_STRING |  | ULJM06173 |  | PSP: not native yet |
| ULJM06173 | JP | TOKYOヤマノテBOYS Portable DARK CHERRY DISC | [0x887666C](../third_party/LunaTranslator/ppsspp_1.cpp#L2450) |  | 0 | 0 | CP932 | FULL_STRING |  | ULJM06173Ex |  | PSP: not native yet |
| ULJM06171 | JP | TOKYOヤマノテBOYS Portable HONEY MILK DISC | [0x8856C80](../third_party/LunaTranslator/ppsspp_1.cpp#L2452) |  | 0 | 0 | CP932 | FULL_STRING |  | ULJM06173 |  | PSP: not native yet |
| ULJM06171 | JP | TOKYOヤマノテBOYS Portable HONEY MILK DISC | [0x88761A4](../third_party/LunaTranslator/ppsspp_1.cpp#L2453) |  | 0 | 0 | CP932 | FULL_STRING |  | ULJM06173Ex |  | PSP: not native yet |
| ULJM06172 | JP | TOKYOヤマノテBOYS Portable SUPER MINT DISC | [0x8856FC0](../third_party/LunaTranslator/ppsspp_1.cpp#L2455) |  | 0 | 0 | CP932 | FULL_STRING |  | ULJM06173 |  | PSP: not native yet |
| ULJM06172 | JP | TOKYOヤマノテBOYS Portable SUPER MINT DISC | [0x8876508](../third_party/LunaTranslator/ppsspp_1.cpp#L2456) |  | 0 | 0 | CP932 | FULL_STRING |  | ULJM06173Ex |  | PSP: not native yet |
| ULJM05986 | JP | アンジェリーク 魔恋の六騎士 | [0x889CBC8](../third_party/LunaTranslator/ppsspp_1.cpp#L2458) |  | 1 | 0 | CP932 |  |  | ULJM06129 |  | PSP: not native yet |
| NPJH50689 | JP | MISSINGPARTS the TANTEI stories Complete | [0x883F9F4](../third_party/LunaTranslator/ppsspp_1.cpp#L2460) |  | 1 | 0 | CP932 |  | TNPJH50689 | NPJH50689 |  | PSP: not native yet |
| ULJM05659 | JP | Canvas3 ～七色の奇跡～ | [0x886000C](../third_party/LunaTranslator/ppsspp_1.cpp#L2462) |  | 0 | 0 | CP932 |  |  | ULJM05659 |  | PSP: not native yet |
| UCJS10002 | JP | どこでもいっしょ | [0x8819C88](../third_party/LunaTranslator/ppsspp_1.cpp#L2464) |  | 1 | 0 | CP932 |  |  | NewLineCharFilterA |  | PSP: not native yet |
| ULJS00293 | JP | テイルズ オブ ファンタジア なりきりダンジョンX | [0x88F45D0](../third_party/LunaTranslator/ppsspp_1.cpp#L2466) |  | 5 | 0 | CP932 | FULL_STRING |  | ULJS00293 |  | PSP: not native yet |

Luna also names these PSP games, without an entry (its comment: text from its generic sceFontGetCharInfo hook):

- ULJM05746: L@ve once
- ULJM06207: Starry☆Sky～After Spring～Portable
- ULJM06208: Starry☆Sky～After Summer～Portable
- ULJM06209: Starry☆Sky～After Autumn～Portable
- ULJM06210: Starry☆Sky～After Winter～Portable
- ULJM05334: MEMORIES OFF
