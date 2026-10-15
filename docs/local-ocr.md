# Local OCR sources

The release defaults to `ocr_backend = vocr`: the
[vita-vn-ocr](vita-vn-ocr.md) recognizer and polarity-agnostic line finder run
inside each enabled game through the game OCR worker, registered for each game
(`*TITLEID`, the installer's `--title`); the `*main` plugin owns
the UI and dictionary lookup. The Overlay app's Model size chooses the
recognizer: Small (VOCR3-FL10), Normal (VOCR3-H15, the default) or Large
(VOCR4-W20); a job runs the next smaller size when the game has too little free
memory ([model sizes](vita-vn-ocr.md#model-sizes-small-normal-large-api-21)). With the bundled local JMdict, the whole
recognition/lookup pipeline needs no network. See the guide for real-screenshot
host measurements, memory bounds and the physical-console validation limits.

Google Lens remains opt-in with `ocr_backend = lens`. Local OCR never falls
back to uploading an image. These are the two engines the Overlay app offers;
it names the local one after its model's generation, VOCR3
([the engine's name](vita-vn-ocr.md#the-engines-name-the-model-generation)).

The earlier CPU-only ncnn adapter is available for host experiments with
`-DVJO_WITH_NCNN=ON` (`vjo-ocr --backend ncnn`). This does not enable ncnn in
the Shell worker: a saved `ocr_backend = ncnn` loads as `vocr`. Native text
extraction has a separate experiment branch; its code is not part of this
release. The Meiki single-line trial was removed after v0.7.12. The FTP installer
comments out their old registrations when installing the mainline release.
The VOCR2 trial, which ran in the same game worker, was removed after
v0.8.0-test.1; a saved `meiki` or `vocr2` loads as `vocr`, and the installer
writes `vocr` in its place.

[Local dictionary memory limits](local-dictionaries.md) and the historic
[Lens connection measurements](lens-performance.md) describe the two pipelines.
