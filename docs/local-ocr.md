# Local OCR sources

The overlay's only OCR engine is the
[vita-vn-ocr](vita-vn-ocr.md) recognizer and polarity-agnostic line finder run
inside each enabled game through the game OCR worker, which the kernel plugin
loads into each game ON in `games.ini` as it starts ([worker-loading.md](worker-loading.md));
the `*main` plugin owns
the UI and dictionary lookup. The Overlay app's Model size chooses the
recognizer: Small (VOCR5-S), Normal (VOCR5-N, the default) or Large
(VOCR5-L); a job runs the next smaller size when the game has too little free
memory ([model sizes](vita-vn-ocr.md#model-sizes-small-normal-large-api-21)). With the bundled local JMdict, the whole
recognition/lookup pipeline needs no network. See the guide for real-screenshot
host measurements, memory bounds and the physical-console validation limits.

Google Lens, the upstream's online engine, was removed (2026-10-15): the overlay
is offline, and no image ever leaves the Vita. A saved `ocr_backend` line is
ignored (`lens` with a config warning) and the Overlay app's next save and the
installer delete it; the last build with Google Lens is the tag
`archive/online-last`. The Overlay app names the engine after its model's
generation, VOCR5 ([the engine's name](vita-vn-ocr.md#the-engines-name-the-model-generation)).

The earlier CPU-only ncnn adapter is available for host experiments with
`-DVJO_WITH_NCNN=ON` (`vjo-ocr --backend ncnn`). This does not enable ncnn in
the Shell worker: a saved `ocr_backend = ncnn` loads as the local engine. Native text
extraction has a separate experiment branch; its code is not part of this
release. The Meiki single-line trial was removed after v0.7.12. The FTP installer
comments out their old registrations when installing the mainline release.
The VOCR2 trial, which ran in the same game worker, was removed after
v0.8.0-test.1; a saved `meiki` or `vocr2` loads as the local engine, and the
installer deletes the line.

[Local dictionary memory limits](local-dictionaries.md) describe the lookup's memory.
