#!/usr/bin/env python3
"""Download the exact local OCR weights; for ncnn, also generate its build headers.

No ONNX/Paddle/Python runtime is needed on the Vita. Run without --ncnn-source
to prepare the model directory to copy to ux0:data/VitaJPOverlay/ocr.

--backend vocr fetches a vita-vn-ocr recognizer (default VOCR3_H15_w8.vocr, from
the bee-san/vita-vn-ocr v0.2.5-vocr3.1 pre-release; H15_w8.vocr, FL10_w8.vocr and
F20_w8.vocr from v0.2.0) instead; --backend vocr-tiers the three model sizes'
(tools/model_tiers.json: VOCR3_FL10_w8.vocr, VOCR3_H15_w8.vocr, VOCR4_W20_w8.vocr), which
a package ships. Each file comes from the release's public download; only when
that fails (a draft release) is an authenticated GitHub CLI (`gh auth login`)
tried. --from takes a copy downloaded by other means. Every file is checked
against its pinned size and SHA-256.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import urllib.request

MODEL_REV = '671ac4a72299a86ddee160131ba88fed748df425'
NCNN_REV = '3798be64cfeeebc0afcfcebfb43ed5c9e1b72e82'
MODEL_BASE = f'https://raw.githubusercontent.com/nihui/ncnn-android-ppocrv5/{MODEL_REV}/app/src/main/assets/'
ASSETS = {
    'PP_OCRv5_mobile_rec.ncnn.bin': (MODEL_BASE + 'PP_OCRv5_mobile_rec.ncnn.bin', 8242276,
        '49d9907a55ba20fa6637f9f788f66ab00793bc8ce57a733a09dbe86b9a2e3db0'),
    'PP_OCRv5_mobile_rec.ncnn.param': (MODEL_BASE + 'PP_OCRv5_mobile_rec.ncnn.param', 20031,
        'f52a6586ac3338d8c350db0c9f3c55ff2ecd3763327f9bc7f0efc091f8a63e74'),
    'ppocrv5_dict.h': (f'https://raw.githubusercontent.com/Tencent/ncnn/{NCNN_REV}/examples/ppocrv5_dict.h', 202891,
        'bb1f547301014b93bd1182af8f472b7df75330e46a392ebdf5c57a8ebcf57c91'),
}


# vita-vn-ocr int8 recognizers (the overlay's OCR engine), release v0.2.0 of
# github.com/bee-san/vita-vn-ocr. The plugin pins the same sizes and hashes
# (core/vocr_ocr.c).
VOCR_REPO = 'bee-san/vita-vn-ocr'
VOCR_TAG = 'v0.2.0'
VOCR_BASE = f'https://github.com/{VOCR_REPO}/releases/download/{VOCR_TAG}/'
# VOCR3 (vita-vn-ocr #67): H15's and FL10's shapes retrained, the same sizes and
# arenas; the Normal (default) and Small model sizes. Pre-release v0.2.5-vocr3.1, whose
# SHA256SUMS and vocr3-assets.json list these hashes (also the VOCR3_H15 and
# VOCR3_FL10 arms of vita-vn-ocr eval/vocr3_report_spec_20261013.json).
VOCR3_TAG = 'v0.2.5-vocr3.1'
VOCR3_BASE = f'https://github.com/{VOCR_REPO}/releases/download/{VOCR3_TAG}/'
VOCR3_ASSETS = {
    'VOCR3_H15_w8.vocr': (VOCR3_BASE + 'VOCR3_H15_w8.vocr', 1155968,
        'cb3c61a8f91f9aeeb3efcc0e79313f664112f25c4bd55865bae9df52522b360c'),
    'VOCR3_FL10_w8.vocr': (VOCR3_BASE + 'VOCR3_FL10_w8.vocr', 657664,
        '87fb4d0c18416f5f3771cecd274c8db3cf69b8b4a4ee9f204edebb4b82079154'),
}
VOCR_DEFAULT_MODEL = 'VOCR3_H15_w8.vocr'  # the default model size's (Normal's), tools/model_tiers.json
VOCR_ASSETS = {
    'H15_w8.vocr': (VOCR_BASE + 'H15_w8.vocr', 1155968,
        '316ebc377ec34520440991c5085199d8e74031f01a867938be90a60c1e3f03cd'),
    'FL10_w8.vocr': (VOCR_BASE + 'FL10_w8.vocr', 657664,
        '500e0b842e5f0cb326f55e7d991a52ab66dd24977ccc89bd188228aedc825e84'),
    'F20_w8.vocr': (VOCR_BASE + 'F20_w8.vocr', 1558016,
        'f8c1abe4743a71a742e718523f4206f992d3bd4fb69de321997b2bd6f16779c0'),
}

# The character LMs of the beam + LM decoder (vita-vn-ocr #82), pre-release
# v0.2.6-ctclm.2 (ctclm.1 is superseded: built before the protected-title filter), CC BY-SA 4.0: their ATTRIBUTION.md ships beside them. The
# plugin pins the same sizes and hashes (core/vocr_ocr.c, include/vjo_vocr_sizes.h).
VOCR_LM_TAG = 'v0.2.6-ctclm.2'
VOCR_LM_BASE = f'https://github.com/{VOCR_REPO}/releases/download/{VOCR_LM_TAG}/'
VOCR_LM_ASSETS = {
    'vocr_lm_o3_500k.vlm': (VOCR_LM_BASE + 'vocr_lm_o3_500k.vlm', 499824,
        '38ea94e0c4ec93863454387e7c2c469d209aaf21f2f6ee5d040e75a09d00d286'),
    'vocr_lm_o3_1000k.vlm': (VOCR_LM_BASE + 'vocr_lm_o3_1000k.vlm', 987600,
        '993000611555171daf6361e29a503b0871a5f3dd240a19555d55f16aaa3ab3d9'),
    'vocr_lm_o3_2000k.vlm': (VOCR_LM_BASE + 'vocr_lm_o3_2000k.vlm', 1971568,
        'ac33e461bd346617aae27168aee47230a758f4b301d78b59eb55ca25a21bcb34'),
    'vocr_lm_o3_4000k.vlm': (VOCR_LM_BASE + 'vocr_lm_o3_4000k.vlm', 3781072,
        '01f49be1691a1aab5a6d33414ce7d17c3e5b2daf97dfccb0d341a380a68e5898'),
}
VOCR_LM_ATTRIBUTION = 'ATTRIBUTION.md'
VOCR_LM_ATTRIBUTION_PIN = {VOCR_LM_ATTRIBUTION: (VOCR_LM_BASE + VOCR_LM_ATTRIBUTION, 2538,
    '1de91ab9c181e204c53ae7c2796af47366026901fc8b2c268cbcef820ebaf19c')}

# The Large model size: VOCR4-W20 (vita-vn-ocr #78, pre-release v0.2.5-vocr4.1, whose
# SHA256SUMS lists this hash; F20's shape). It is also the confidence cascade's model of
# Normal: it reads again the lines Normal reads with low confidence.
VOCR4_TAG = 'v0.2.5-vocr4.1'
VOCR4_BASE = f'https://github.com/{VOCR_REPO}/releases/download/{VOCR4_TAG}/'
VOCR4_ASSETS = {
    'VOCR4_W20_w8.vocr': (VOCR4_BASE + 'VOCR4_W20_w8.vocr', 1558016,
        '2745e69699ad7b2aba3092f83e5ddc1912f405f54b9df8a2e55be82c385b716e'),
}
VOCR_CASCADE_MODEL = 'VOCR4_W20_w8.vocr'  # the cascade's model of a request by name (host tools)

# The model generation of each release: vita-vn-ocr's name for the family of models a release
# publishes (it calls v0.2.0's FL10 "VOCR1"). A .vocr blob carries no generation of its own, so
# the pins carry it: core/vocr_models.c per file (the Overlay app names the local OCR engine
# after it) and package-manifest.json (package_release.py). The VOCR2 trial (v0.2.3-vocr2.1)
# was removed from the overlay after v0.8.0-test.1.
VOCR_GENERATION = {VOCR_TAG: 'VOCR1', VOCR3_TAG: 'VOCR3', VOCR4_TAG: 'VOCR4'}

# The model sizes the Overlay app offers (config.ini vocr_tier), each a pinned recognizer above:
# one table for the C pins' tests, the packager and the self-test analyser.
MODEL_TIERS = Path(__file__).resolve().parent / 'model_tiers.json'


def model_tiers():
    """tools/model_tiers.json: its 'default' tier id and its 'tiers', smallest first, each with its
    id and its pinned 'model', checked to be a recognizer of a release table here."""
    data = json.loads(MODEL_TIERS.read_text(encoding='utf-8'))
    ids = [tier['id'] for tier in data['tiers']]
    if ids != ['small', 'normal', 'large'] or data['default'] not in ids:
        raise ValueError(f'{MODEL_TIERS.name}: tiers {ids}, default {data["default"]!r}')
    for tier in data['tiers']:
        release_assets(tier['model'])
    return data


def release_assets(name):
    """(asset table, release tag) of a pinned recognizer."""
    for assets, tag in ((VOCR3_ASSETS, VOCR3_TAG), (VOCR4_ASSETS, VOCR4_TAG), (VOCR_ASSETS, VOCR_TAG)):
        if name in assets:
            return assets, tag
    raise ValueError(f'{name} is not a pinned recognizer')


def store(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix(path.suffix + '.tmp')
    temp.write_bytes(data)
    temp.replace(path)
    return path


def fetch(directory, name, assets=ASSETS):
    url, size, sha = assets[name]
    path = directory / name
    def valid(data):
        return len(data) == size and hashlib.sha256(data).hexdigest() == sha
    if path.exists() and valid(path.read_bytes()):
        return path
    request = urllib.request.Request(url, headers={'User-Agent': 'VitaJPOverlay-model-setup'})
    with urllib.request.urlopen(request, timeout=90) as response:
        data = response.read(size + 1)
    if not valid(data):
        raise RuntimeError('Incorrect or incomplete download: ' + name)
    return store(path, data)


def fetch_release_asset(directory, name, assets, tag, source=None):
    """A release asset: a local copy (--from), else the public download, else
    `gh release download` (private repository or draft release)."""
    url, size, sha = assets[name]
    path = directory / name
    def valid(data):
        return len(data) == size and hashlib.sha256(data).hexdigest() == sha
    if path.exists() and valid(path.read_bytes()):
        return path
    if source:
        source = Path(source)
        data = (source / name if source.is_dir() else source).read_bytes()
        if not valid(data):
            raise RuntimeError(f'{source} is not the {tag} {name} (size or SHA-256 differs)')
        return store(path, data)
    try:
        return fetch(directory, name, assets)
    except (OSError, RuntimeError) as public_error:
        if not shutil.which('gh'):
            raise RuntimeError(f'{url}: {public_error}. Check the network and retry, pass --from with a '
                               f'copy, or install the GitHub CLI (`gh auth login`) for a draft release.') from public_error
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(['gh', 'release', 'download', tag, '-R', VOCR_REPO, '-p', name, '-D', tmp],
                       check=True, stdin=subprocess.DEVNULL, timeout=300)
        data = (Path(tmp) / name).read_bytes()
    if not valid(data):
        raise RuntimeError('Incorrect or incomplete download: ' + name)
    return store(path, data)


def fetch_vocr(directory, name, source=None):
    assets, tag = release_assets(name)
    return fetch_release_asset(directory, name, assets, tag, source)


def fetch_tiers(directory, source=None):
    """The three model sizes' recognizers (tools/model_tiers.json), each pinned: what a package ships."""
    return [fetch_vocr(directory, tier['model'], source) for tier in model_tiers()['tiers']]


def fetch_lms(directory, source=None):
    """The four LM tiers of the beam + LM decoder and their CC BY-SA 4.0 attribution, each pinned."""
    paths = [fetch_release_asset(directory, name, VOCR_LM_ASSETS, VOCR_LM_TAG, source) for name in VOCR_LM_ASSETS]
    paths.append(fetch_release_asset(directory, VOCR_LM_ATTRIBUTION, VOCR_LM_ATTRIBUTION_PIN, VOCR_LM_TAG, source))
    return paths



def binary_param(text, layer_types):
    """Numeric subset of the official ncnn2mem format, for this pinned graph.

    Layer IDs follow ncnn_add_layer order, including disabled layers. Every
    graph input must already exist, and array lengths/counts are checked.
    """
    lines = text.strip().splitlines()
    assert lines[0] == '7767517'
    layers, blobs = map(int, lines[1].split())
    assert len(lines) == layers + 2
    out = bytearray(struct.pack('<iii', 7767517, layers, blobs))
    ids = {}
    def integer(n): out.extend(struct.pack('<i', n))
    def number(value):
        out.extend(struct.pack('<f', float(value)) if re.search(r'[.eE]', value) else struct.pack('<i', int(value)))
    for line in lines[2:]:
        words = line.split(); kind, _, nb, nt = words[:4]; nb, nt = int(nb), int(nt)
        integer(layer_types.index(kind)); integer(nb); integer(nt)
        for name in words[4:4+nb]: integer(ids[name])
        for name in words[4+nb:4+nb+nt]:
            assert name not in ids
            ids[name] = len(ids); integer(ids[name])
        for field in words[4+nb+nt:]:
            key, value = field.split('=', 1); key = int(key)
            assert key > -23400  # no strings/custom layers in this graph
            integer(key)
            if key <= -23300:
                values = value.split(','); n = int(values[0]); assert n == len(values)-1
                integer(n)
                for value in values[1:]: number(value)
            else: number(value)
        integer(-233)
    assert len(ids) == blobs
    return bytes(out), ids['in0'], ids['out0']


def generate(directory, ncnn_source):
    types = re.findall(r'^ncnn_add_layer\((\w+)', (ncnn_source/'src/CMakeLists.txt').read_text(), re.M)
    param, input_id, output_id = binary_param((directory/'PP_OCRv5_mobile_rec.ncnn.param').read_text(), types)
    source = (directory/'ppocrv5_dict.h').read_text()
    vocab = [json.loads(s) for s in re.findall(r'^\s*("(?:[^"\\]|\\.)*")\s*,?\s*$', source, re.M)]
    assert len(vocab) == 18383
    vocab.append(' ')  # Paddle CTC's final space; token zero is blank.
    lines = ['// Generated by tools/prepare_ocr.py; do not edit.',
             '// Dictionary: Copyright 2025 Tencent, BSD-3-Clause.',
             '// Model architecture: PaddleOCR, Apache-2.0.',
             '#ifndef VJO_PPOCR_DATA_H', '#define VJO_PPOCR_DATA_H',
             'alignas(64) static const unsigned char vjo_ppocr_param[] = {']
    for i in range(0, len(param), 24): lines.append(','.join(str(x) for x in param[i:i+24])+',')
    lines += ['};', f'static const int vjo_ppocr_input = {input_id};', f'static const int vjo_ppocr_output = {output_id};',
              'static const char *const vjo_ppocr_vocab[] = {']
    lines += [json.dumps(s, ensure_ascii=False)+',' for s in vocab]
    lines += ['};', '#endif', '']
    (directory/'vjo_ppocr_data.h').write_text('\n'.join(lines), encoding='utf-8')
    (directory/'PP_OCRv5_mobile_rec.ncnn.param.bin').write_bytes(param)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--backend', choices=('ncnn', 'vocr', 'vocr-tiers', 'vocr-lm'), default='ncnn',
                    help="vocr-tiers: the three model sizes' recognizers (Small, Normal, Large); "
                         "vocr-lm: the beam + LM decoder's four LMs and their ATTRIBUTION.md")
    ap.add_argument('--ncnn-source', type=Path)
    ap.add_argument('--model', choices=sorted({**VOCR3_ASSETS, **VOCR4_ASSETS, **VOCR_ASSETS}),
                    help=f'vocr: which recognizer (default: {VOCR_DEFAULT_MODEL})')
    ap.add_argument('--from', dest='source', help='vocr: an already downloaded asset (file or directory)')
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    if args.backend == 'vocr-lm':
        for path in fetch_lms(args.output, args.source):
            print('Verified', path.name)
        print('LMs ready in', args.output, '- copy them to ux0:data/VitaJPOverlay/ocr/ (CC BY-SA 4.0: keep',
              VOCR_LM_ATTRIBUTION, 'beside them)')
        return
    if args.backend == 'vocr-tiers':
        for path in fetch_tiers(args.output, args.source):
            print('Verified', path.name)
        print('Models ready in', args.output, '- copy them to ux0:data/VitaJPOverlay/ocr/')
        return
    if args.backend == 'vocr':
        model = args.model or VOCR_DEFAULT_MODEL
        fetch_vocr(args.output, model, args.source)
        print('Verified', model)
        print('Model ready in', args.output, '- copy', model, 'to ux0:data/VitaJPOverlay/ocr/')
        return
    names = ASSETS if args.ncnn_source else ['PP_OCRv5_mobile_rec.ncnn.bin']
    for name in names:
        fetch(args.output, name)
        print('Verified', name)
    if args.ncnn_source:
        generate(args.output, args.ncnn_source)
    print('Model ready in', args.output)


if __name__ == '__main__':
    main()
