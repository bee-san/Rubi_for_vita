# Adrenaline console findings, 2026-10-23

Issue [#248](https://github.com/bee-san/Rubi_for_vita/issues/248#issuecomment-6369763418).
The installed app and all three plugins were read back and matched the published debug
`v0.9.1-test.8` package, source `e49251f59928eaeee89a75ef313eede8d045fb37`, API 35.
This is separate from the earlier native-hook session's mixed-build results.

The owner tested エビコレ+ アマガミ (bubble `PSPEMU006`) under Adrenaline. Its PSP disc ID
was not read, so the bubble title does not establish a Luna table match. Both probe runs found
ScePspemu NID `0x3F75D4D3`. Each UID query returned `0x80024501`, while mapping information
succeeded with readable access `0x6`, uncached `0x80`, type `0x0C208060`:

| Queried address | Mapping base | Mapping size |
|---|---|---|
| `0x70000000` | `0x70000000` | `0x02000000` |
| `0x70800000` | `0x70000000` | `0x02000000` |
| `0x73CDE000` | `0x73000000` | `0x01000000` |

The UID-required guard stopped before any PSP reads. Disc ID, site words, MIPS classes and
timed 1 MiB read remain unverified. VitaSDK names the error
[`SCE_KERNEL_ERROR_INVALID_UID`](https://github.com/vitasdk/vita-headers/blob/master/include/psp2/kernel/error.h);
its [mapping query](https://github.com/vitasdk/vita-headers/blob/master/include/psp2/kernel/sysmem.h)
reports bounds and read access separately.

OCR worked independently: the probe log has 47 worker jobs, 13 with rc 0, six with nonempty
text, and 34 refused captures (`VJO_E_SCREEN_CHANGED`, -120). Background retries are included;
these counts are not a per-button-press success rate. The owner reported stutter when leaving
Adrenaline and one late wait long enough to abandon. No new dump appeared in the collection;
that does not establish an absence of faults or a stability pass. Raw game text, cards and
console-specific files remain in the local collection, outside the repository.

The next test build combines main's native hooks with probe v2 and uses log format 3. Its
guard accepts the observed INVALID_UID case only behind successful readable mapping queries,
and validates all 256 page mappings before the bulk read. Other errors, missing/read-protected
pages, changed bounds, types or cache policies, and wrapping ranges refuse it. The overlay
stops its foreground changed-screen retry after three consecutive failures and shows a message;
Auto can recover once the overlay closes. Capture-consistency checks stay enabled. These are
code and host-test changes; neither fix is yet verified on the console, and the exit stutter's
cause remains unknown.
