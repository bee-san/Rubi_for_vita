# PP-OCRv6 small (JIS head) for ncnn

Derived from PaddleOCR's PP-OCRv6 small text recognition model
(https://github.com/PaddlePaddle/PaddleOCR, Copyright (c) 2020 PaddlePaddle
Authors), licensed under the Apache License 2.0
([../licenses/PaddleOCR.txt](../licenses/PaddleOCR.txt)), through RapidOCR
3.10.0's ONNX export (Apache-2.0).

- `PP-OCRv6_small_jis_int8.ncnn.param`: the int8 ncnn graph. Modified: CTC head
  pruned to 6,515 classes (blank, JIS X 0208, ASCII, full-width forms, a few
  symbols, space), final Softmax removed, converted by pnnx and ncnn's ncnn2int8,
  ReduceMean rewritten as adaptive pooling and batch axes removed from Reshape.
- `ppocrv6_jis_dict.txt`: the kept entries of the model's character dictionary,
  in class order (class 0, the CTC blank, is not listed).

The matching int8 weights `PP-OCRv6_small_jis_int8.bin` are distributed as a
release asset under the same licence. `tools/prepare_ppocrv6.py build` recreates
all three files from the pinned upstream inputs.
