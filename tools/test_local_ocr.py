#!/usr/bin/env python3
"""Run the real pinned model on existing synthetic Japanese dialogue fixtures.

--backend ncnn (default): PP-OCRv5 mobile, model directory from prepare_ocr.py.
--backend vocr: vita-vn-ocr, a directory holding the release's H15_w8.vocr
(tools/prepare_ocr.py --backend vocr).
--backend ppocrv6: PP-OCRv6 small int8, a directory holding the pinned
PP-OCRv6_small_jis_int8.bin (tools/prepare_ppocrv6.py fetch).
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
from PIL import Image, ImageOps

LINES = ('朝ごはん、もう冷めちゃったよ。', '早く起きないと学校に遅刻するってば')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cli', required=True)
    ap.add_argument('--model', type=Path, required=True)
    ap.add_argument('--backend', choices=('ncnn', 'vocr', 'ppocrv6'), default='ncnn')
    args = ap.parse_args()
    root = Path(__file__).resolve().parent.parent
    image = Image.open(root/'host/samples/vn_textbox.jpg').crop((45, 375, 920, 475)).convert('RGBA')
    weights_name = {'ncnn': 'PP_OCRv5_mobile_rec.ncnn.bin', 'vocr': 'H15_w8.vocr',
                    'ppocrv6': 'PP-OCRv6_small_jis_int8.bin'}[args.backend]
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        pixels = tmp/'input.rgba'
        def run(im, model=args.model):
            pixels.write_bytes(im.tobytes())
            cmd = [args.cli] + (['--backend', args.backend] if args.backend != 'ncnn' else [])
            return subprocess.run(cmd + [str(model), str(im.width), str(im.height), str(pixels)],
                                  text=True, capture_output=True, timeout=300)
        result = run(image)
        assert result.returncode == 0, result.stderr
        for line in LINES:
            assert line in result.stdout, result.stdout
        assert 'lines=2' in result.stderr, result.stderr
        # Same-size corruption exercises the runtime SHA-256 check, not just length.
        bad = tmp/'bad'; bad.mkdir()
        weights = bytearray((args.model/weights_name).read_bytes())
        weights[len(weights)//2] ^= 1
        (bad/weights_name).write_bytes(weights)
        result = run(image, bad)
        assert result.returncode == 1 and 'rc=-115' in result.stderr and not result.stdout, result
        result = run(image, tmp/'missing')
        assert result.returncode == 1 and 'rc=-115' in result.stderr, result
        result = run(Image.new('RGBA', (960, 100), (0,0,0,255)))
        assert result.returncode == 0 and not result.stdout.strip(), result
        # Full allowed line width and eight lines stress the fixed workspace.
        one = image.crop((0, 0, image.width, 43)).resize((960, 48))
        many = Image.new('RGBA', (960, 544), (0,0,0,255))
        for n in range(8): many.paste(one, (0, n*66))
        result = run(many)
        assert result.returncode == 0 and 'lines=8' in result.stderr, result.stderr
        if args.backend == 'vocr':
            check_vocr(run, image, many)
        if args.backend == 'ppocrv6':
            check_ppocrv6(run, image, many)
        print(f'Real-model OCR ({args.backend}), multiline bounds, blank input, and corrupt/missing model checks passed.')


def check_ppocrv6(run, image, many):
    """Polarity-agnostic finder, unread weights for an empty region, and the heap bound."""
    result = run(Image.new('RGBA', (960, 100), (0,0,0,255)))
    assert stat(result.stderr, 'loaded') == 0 and stat(result.stderr, 'lines') == 0, result.stderr
    dark = Image.merge('RGBA', (*ImageOps.invert(image.convert('RGB')).split(), image.getchannel('A')))
    result = run(dark)
    assert result.returncode == 0 and 'lines=2' in result.stderr, result.stderr
    for line in LINES:
        assert line in result.stdout, result.stdout
    # Eight full-width lines in one job stay inside the fixed ncnn heap.
    result = run(many)
    peak, heap = stat(result.stderr, 'tensor_heap_peak'), stat(result.stderr, 'heap')
    assert result.returncode == 0 and 'lines=8' in result.stderr, result.stderr
    assert 0 < peak <= heap, result.stderr


def stat(stderr, key):
    return int(re.search(rf'\b{key}=(\d+)', stderr).group(1))


def check_vocr(run, image, many):
    """What the bright-pixel finder cannot do, and the exact memory bound."""
    # An empty region never reads the weights file.
    result = run(Image.new('RGBA', (960, 100), (0,0,0,255)))
    assert stat(result.stderr, 'loaded') == 0 and stat(result.stderr, 'lines') == 0, result.stderr
    # Dark text on a light box: found as dark lines, inverted, read the same.
    dark = Image.merge('RGBA', (*ImageOps.invert(image.convert('RGB')).split(), image.getchannel('A')))
    result = run(dark)
    assert result.returncode == 0 and 'lines=2' in result.stderr and 'dark=2' in result.stderr, result.stderr
    for line in LINES:
        assert line in result.stdout, result.stdout
    # The workspace is the whole job's memory: weights + max(finder, recognizer
    # arena) + the grey region + the runtime's structs.
    result = run(many)
    used = stat(result.stderr, 'tensor_heap_peak')
    parts = stat(result.stderr, 'weights') + stat(result.stderr, 'scratch') + stat(result.stderr, 'region')
    assert used == stat(result.stderr, 'workspace') and 0 <= used - parts < 16384, result.stderr
    assert used < 2 * 1024 * 1024, result.stderr


if __name__ == '__main__':
    main()
