# Local OCR sources

The release defaults to `ocr_backend = vocr`: the
[vita-vn-ocr](vita-vn-ocr.md) recognizer and polarity-agnostic line finder run
on the console. FL10 is the smaller default; FL10 and H15 int8 models are in
the complete release archive. With the bundled local JMdict, the whole
recognition/lookup pipeline needs no network. See the guide for real-screenshot
host measurements, memory bounds and the physical-console validation limits.

Google Lens remains opt-in with `ocr_backend = lens`. Local OCR never falls
back to uploading an image.

The earlier CPU-only ncnn adapter is available for host experiments with
`-DVJO_WITH_NCNN=ON` (`vjo-ocr --backend ncnn`). This does not enable ncnn in
the Shell worker. Meiki and native text extraction have separate experiment
branches; their game plugins are not part of this release. The FTP installer
comments out their old registrations when installing the mainline release.

[Local dictionary memory limits](local-dictionaries.md) and the historic
[Lens connection measurements](lens-performance.md) describe the two pipelines.
