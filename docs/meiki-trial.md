# Optional Meiki single-line trial

`v0.7.6-meiki.1` adds an optional recognizer to the working v0.7.5 game-process
OCR architecture. The default remains `vocr`; v0.7.5 remains the stable rollback.
Character navigation, one-entry local dictionary lookup and the 64 KiB Shell
lookup arena are retained. No Anki queue or cloud fallback is introduced.

Select **one horizontal dialogue line**, including its ruby if needed, and
exclude adjacent lines. The game worker rejects regions taller than 96 pixels
before model reads or allocations. A multi-line dialogue box is unsuitable for
this trial. Meiki processes the selected pixels directly instead of the white-text
line finder, so it can retain dim, coloured or dark-on-light text. This geometry
rule alone does not prove that the selected region contains exactly one line.

Set the existing config explicitly:

```ini
ocr_backend = meiki
ocr_mode = on_press
ocr_model_dir = ux0:data/VitaJPOverlay/ocr
dictionary = local
local_dictionaries = jmdict.vjdict
```

The Meiki backend is manual even if `ocr_mode=auto` was left in an existing
config. It performs no background or subtitle inference. If the screen changes
while recognition runs, close and reopen the overlay to request a fresh run;
it does not automatically reload the 5.4 MB model. Moving the character cursor
looks up saved OCR text and does not capture or run OCR again.

The optional archive contains the current API13 kernel, Shell and game worker,
plus `meiki/meiki-engine.suprx`. Install the engine at the fixed game-visible path
`app0:/VitaJPOverlay/meiki/meiki-engine.suprx`, for example through an existing
per-title rePatch mount. Retail app0 module visibility and successful engine
loading require an actual hardware test. Do not install the earlier API9 worker
or ABI2 engine alongside this build; the engine requires ABI3.

The recognizer belongs at
`ux0:data/VitaJPOverlay/ocr/meiki-stream-int8.mnn`. Reuse the evaluated release
asset: **5,392,856 bytes**, SHA-256
`b7ba3219cd1bfeef5cb6b42ef6686683b991c6561b7ad3b1c41d32817deba21e`.
Weights are distributed as release assets and are never committed to this repo.
The trial carries the model's [LGPL-3.0 text](../third_party/licenses/Meiki-model-LGPL-3.0.txt),
the full [GPL-3.0 text](../LICENSE), Meiki/MNN code licenses and MNN's embedded
[Flatbuffers](../third_party/licenses/MNN-Flatbuffers-Apache-2.0.txt) and
[Half](../third_party/licenses/MNN-Half-MIT.txt) notices. The code's Apache
license does not replace the model's LGPL license.
The SHA is validated before allocating inference buffers. Both validation and
MNN model loading read through the existing kernel model broker in chunks of
at most 8 KiB. The game never reopens a weight path with its own file imports.
The broker allowlist accepts this one additional pinned recognizer, not detector
weights or arbitrary paths.

The worker requires **27,648,000 free game USER bytes** before loading: a
16,777,216-byte neural arena, 4,194,304-byte isolated metadata heap,
385,024-byte page-rounded preprocessing block, 2,097,152 bytes of module
allowance and 4,194,304 bytes reserved for the game. The three private buffers
therefore request 21,356,544 USER bytes. These are explicit caller-buffer and
preflight bounds, not a complete system residency measurement. Kernel staging
stays at 8 KiB; inference allocates no Shell Paf heap. Allocation can still fail
after a successful free-memory query.

Allocation-free game TTY diagnostics report the free USER value, requested and
rounded allocations, required budget, reader error and measured runtime peaks.
The user SDK API used here does not report total USER budget or largest free
chunk, so those fields are explicitly `unavailable`; they are not inferred from
a game's advertised allocation mode. Failed stop/unload, descriptor cleanup or
unproved memblock invalidation retains ownership and refuses another OCR job.

Keep the v0.7.5 plugins, previous config and original per-game ROI before a
hardware trial. Switching `ocr_backend` back to `vocr` selects the existing
small recognizer; restoring the saved plugins/config/ROI restores the stable
build. This trial does not raise the game's memory quota or reduce the reserve.

Build with the checksum-pinned OCR checkout's `ports/meiki` and the pinned
MNN Vita cross-build listed in [the dependency manifest](meiki-dependencies.json):

```sh
cmake -S . -B build/host-meiki -DVJO_WITH_MEIKI=ON \
  -DVJO_VOCR_SOURCE_DIR=/path/to/vita-vn-ocr \
  -DVJO_MEIKI_SOURCE_DIR=/path/to/vita-vn-ocr/ports/meiki \
  -DVJO_MEIKI_MODEL_DIR=/path/to/release-models
cmake --build build/host-meiki -j4
ctest --test-dir build/host-meiki --output-on-failure

cmake -S vita -B build/vita-meiki -DVJO_WITH_MEIKI=ON \
  -DVJO_VOCR_SOURCE_DIR=/path/to/vita-vn-ocr \
  -DVJO_MEIKI_SOURCE_DIR=/path/to/vita-vn-ocr/ports/meiki \
  -DMNN_SOURCE=/path/to/MNN -DMNN_LIBRARY=/path/to/libMNN.a
cmake --build build/vita-meiki -j4 --target release
```

The [integration receipt](benchmarks/meiki-trial-integration-20261011.json) records
the completed host/native gates and their limits. Host worker tests run the actual API13 worker, SHA checker, C preprocessing and
reader bridge, with inference injected at the isolated module boundary. They
cover exact preflight and region bounds, short broker reads, native I/O failure,
cancellation during engine-side model streaming, all three allocation/mapping
failures, cleanup retention and retry. Those tests verify integration and
lifetime, not OCR accuracy. The full worker integration requires the pinned
model directory. Without weights, hosted CI registers only the separately named
`game_meiki_preflight` budget/region test plus the portable adapter tests; it
does not claim the SHA/reader/inference-boundary cases ran. Real MNN path/reader equality and allocator peaks are
measured separately in the pinned OCR source's reader verification report.
Earlier ARM crop accuracy results belong to their earlier engine revision;
this optional package's retail-game OCR quality and timing remain unmeasured
until hardware validation. A blank successful capture is not recognition proof.

The pinned runtime's [full reader/path report](https://github.com/bee-san/vita-vn-ocr/blob/39a3a9ae069180675c145f6df74c843dc2061967/ports/meiki/results-reader-matrix.json)
ran all 451 hand-checked real VN crops on the host, twice with each loader
(902 file-loader and 902 reader calls). Vita-size crops measured 36 edits over
2,564 reference characters, **1.404% CER**; PSP-size crops measured 89 over
2,510, **3.546% CER**. Raw and decoded outputs matched exactly between loaders.
The bounded neural pool peaked at 13,235,648 bytes and retained zero bytes.
These are host crop results, not current retail-game accuracy, speed or total
resident memory. The host used its system C++ metadata heap rather than the
isolated Vita heap; the cross-built ABI3 engine has not yet run on hardware.
