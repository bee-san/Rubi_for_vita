# spotter

`textspot6_tiny_s1.vtsp` (3,256 B, SHA-256 `77b20d636a34e04744e8d3d81d81a64c485dc323af6c7c92ce649a88fe38972b`) is a
VTSP version-4 text spotter with **random weights**: vita-vn-ocr's `train/textspot6_model.random_blob` at commit
`a54a5b2` (feat/textspot6, the spotter runtime's pin when it was made; the runtime at today's pin,
`cmake/VocrOCR.cmake` `VJO_VOCR_SPOT_COMMIT`, reads it the same: its `vocr_textspot6.c` is the same file), for the
tiny trunk shape of vita-vn-ocr's runtime tests (`tests/textspot6_parity.py` `TINY_T`), seed 1, 4 tags:

```sh
# in a vita-vn-ocr checkout at a54a5b2, with its .venv-ci
python - <<'EOF'
import sys; sys.path[:0] = ["train", "synth", "tests"]
import textspot6_model as M6
TINY_T = {"name": "tinyt", "family": "T", "stride": 8, "c0": 4, "c1": 8, "c2": 16, "chans": [16, 16],
          "dil": [1, 2], "block": "sep", "head": "kroa_x2", "half_above": 480}
open("textspot6_tiny_s1.vtsp", "wb").write(M6.random_blob(TINY_T, 1, 4)[0])
EOF
```

It is not a trained model and finds no text: its boxes are whatever random weights give. It has a trunk's form
(`half_above` 480, `half_above_h` 0, `conf_min` 1), so a build with `VOCR_TEXTSPOT6_NO_MS` loads it too, and it halves a
960 x 544 frame (wider than 480) and reads a 480 x 272 one at its own size, as the trained pick does. On a 960 x 544
frame tiled with `../short_line/textbox.pgm` it returns 64 boxes, more than it keeps (`truncated`), horizontal and
vertical, of every class, which is what the host tests need: the job's spotter path must give the recognizer exactly
the post-pass (vita-vn-ocr's `vocr_textspot7_post`, decode D) of the runtime's own horizontal boxes on the whole frame,
in its order, capped at 16 lines (`host/tests/test_vocr_spot.c`, `host/tests/test_game_spot.c`, whose pins are
`host/tests/test_spot_pins.h`).
