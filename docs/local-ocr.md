# Local OCR sources

The release defaults to `ocr_backend = vocr`: the
[vita-vn-ocr](vita-vn-ocr.md) recognizer and polarity-agnostic line finder run
inside each enabled game through the game OCR worker, registered for each game
(`*TITLEID`, the installer's `--title`); the `*main` plugin owns
the UI and dictionary lookup. FL10 is the smaller default; FL10 and H15 int8 models are in
the complete release archive. With the bundled local JMdict, the whole
recognition/lookup pipeline needs no network. See the guide for real-screenshot
host measurements, memory bounds and the physical-console validation limits.

Google Lens remains opt-in with `ocr_backend = lens`. Local OCR never falls
back to uploading an image.

The earlier CPU-only ncnn adapter is available for host experiments with
`-DVJO_WITH_NCNN=ON` (`vjo-ocr --backend ncnn`). This does not enable ncnn in
the Shell worker. Native text extraction has a separate experiment branch; its
code is not part of this release. The Meiki single-line trial was removed after
v0.7.12. The FTP installer
comments out their old registrations when installing the mainline release.

[Local dictionary memory limits](local-dictionaries.md) and the historic
[Lens connection measurements](lens-performance.md) describe the two pipelines.
