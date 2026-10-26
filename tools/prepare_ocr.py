#!/usr/bin/env python3
"""Download the exact local OCR weights; for ncnn, also generate its build headers.

No ONNX/Paddle/Python runtime is needed on the Vita. Run without --ncnn-source
to prepare the model directory to copy to ux0:data/VitaJPOverlay/ocr.

--backend vocr fetches a vita-vn-ocr recognizer (default VOCR5_N_w8g2.vocr, the Normal
model size's, from the bee-san/vita-vn-ocr v0.3.6-head2level.1 pre-release; VOCR3_H15_w8.vocr and
VOCR3_FL10_w8.vocr from the v0.2.5-vocr3.1 pre-release, VOCR4_W20_w8.vocr from
v0.2.5-vocr4.1, H15_w8.vocr, FL10_w8.vocr and F20_w8.vocr from v0.2.0) instead;
--backend vocr-tiers the three model sizes' (tools/model_tiers.json: VOCR5_S_w8g.vocr,
VOCR5_N_w8g2.vocr, VOCR5_L_w8g2.vocr) and their release's attribution, v0.3.6-head2level.1's ATTRIBUTION.md
kept as MODEL-ATTRIBUTION.md, and the text-presence gate's weights (textgate_p8_c8_w8.vtd, v0.3.2-textgate.1)
with their release's ATTRIBUTION.md kept as GATE-ATTRIBUTION.md, which a package ships. Each file comes from the
release's public download; only when that fails (a draft release) is an authenticated
GitHub CLI (`gh auth login`) tried. --from takes a copy downloaded by other means.
Every file is checked against its pinned size and SHA-256.
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
# arenas; the Normal and Small model sizes before VOCR5 (now the host tools'). Pre-release v0.2.5-vocr3.1, whose
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
VOCR_ASSETS = {
    'H15_w8.vocr': (VOCR_BASE + 'H15_w8.vocr', 1155968,
        '316ebc377ec34520440991c5085199d8e74031f01a867938be90a60c1e3f03cd'),
    'FL10_w8.vocr': (VOCR_BASE + 'FL10_w8.vocr', 657664,
        '500e0b842e5f0cb326f55e7d991a52ab66dd24977ccc89bd188228aedc825e84'),
    'F20_w8.vocr': (VOCR_BASE + 'F20_w8.vocr', 1558016,
        'f8c1abe4743a71a742e718523f4206f992d3bd4fb69de321997b2bd6f16779c0'),
}

# The character LMs of the beam + LM decoder (vita-vn-ocr #82, #127), pre-release
# v0.3.7-lm6.1, CC BY-SA 4.0: tiers 2 and 4 are VOCR6's (built with the hooked games' text), tiers 1 and 3
# ctclm.2's byte for byte; their ATTRIBUTION.md ships beside them. The plugin pins the same sizes and hashes
# (core/vocr_ocr.c, include/vjo_vocr_sizes.h) and each tier's decoder settings (the release's settings.json).
VOCR_LM_TAG = 'v0.3.7-lm6.1'
VOCR_LM_BASE = f'https://github.com/{VOCR_REPO}/releases/download/{VOCR_LM_TAG}/'
VOCR_LM_ASSETS = {
    'vocr_lm_o3_500k.vlm': (VOCR_LM_BASE + 'vocr_lm_o3_500k.vlm', 499824,
        '38ea94e0c4ec93863454387e7c2c469d209aaf21f2f6ee5d040e75a09d00d286'),
    'vocr_lm6_o3_1000k.vlm': (VOCR_LM_BASE + 'vocr_lm6_o3_1000k.vlm', 982320,
        '41591accf8499f1c34557e585cca382de2c4b5ca15f270c985612c32487e251f'),
    'vocr_lm_o3_2000k.vlm': (VOCR_LM_BASE + 'vocr_lm_o3_2000k.vlm', 1971568,
        'ac33e461bd346617aae27168aee47230a758f4b301d78b59eb55ca25a21bcb34'),
    'vocr_lm6_o3_4000k.vlm': (VOCR_LM_BASE + 'vocr_lm6_o3_4000k.vlm', 3603552,
        'a65841b5bdce3e3123b2d1e712deb857fb577cbd2fd8667e8fb06ab35d368335'),
}
VOCR_LM_ATTRIBUTION = 'ATTRIBUTION.md'
VOCR_LM_ATTRIBUTION_PIN = {VOCR_LM_ATTRIBUTION: (VOCR_LM_BASE + VOCR_LM_ATTRIBUTION, 3641,
    'c612199c55374b04cadfb43efa31f8b286f0391aa0d0cd69d5545b663551dc4d')}

# VOCR4-W20 (vita-vn-ocr #78, pre-release v0.2.5-vocr4.1, whose SHA256SUMS lists this hash;
# F20's shape): the Large model size before VOCR5, and the confidence cascade's model of a
# request by name (the host tools): it reads again the lines a smaller model reads with low
# confidence.
VOCR4_TAG = 'v0.2.5-vocr4.1'
VOCR4_BASE = f'https://github.com/{VOCR_REPO}/releases/download/{VOCR4_TAG}/'
VOCR4_ASSETS = {
    'VOCR4_W20_w8.vocr': (VOCR4_BASE + 'VOCR4_W20_w8.vocr', 1558016,
        '2745e69699ad7b2aba3092f83e5ddc1912f405f54b9df8a2e55be82c385b716e'),
}
VOCR_CASCADE_MODEL = 'VOCR4_W20_w8.vocr'  # the cascade's model of a request by name (host tools)

# VOCR5 (vita-vn-ocr eval/vocr5_protocol.json): the three model sizes, Small, Normal (the default)
# and Large, each its own shape; release v0.3.0's recognizers with their head gate (vita-vn-ocr #117,
# docs/headgate.md: v0.3.0's bytes and a 1,792 B gate section), Normal's and Large's also with their
# two-level head (#117 item 2, docs/head2level.md: the output rows in frequency order, the same gate, a
# 1,984 B VOC2 trailer), pre-release v0.3.6-head2level.1 (Small's file is v0.3.5-headgate.1's, byte for
# byte), whose SHA256SUMS lists these hashes.
VOCR5_TAG = 'v0.3.6-head2level.1'
VOCR5_BASE = f'https://github.com/{VOCR_REPO}/releases/download/{VOCR5_TAG}/'
VOCR5_ASSETS = {
    'VOCR5_S_w8g.vocr': (VOCR5_BASE + 'VOCR5_S_w8g.vocr', 821584,
        '76a1bc5de5cd0419a7edad31bc198af8016bda5592a7e595de75697671d3e928'),
    'VOCR5_N_w8g2.vocr': (VOCR5_BASE + 'VOCR5_N_w8g2.vocr', 1189408,
        'f0c8e52c9c1d9f2a714d5ca3ecfba9c0b031ee7ac51a522a2c29fd558d51761b'),
    'VOCR5_L_w8g2.vocr': (VOCR5_BASE + 'VOCR5_L_w8g2.vocr', 2477472,
        '41c3543ff8dddd45dc953a6a5f5d579ef9ca779f0e43196406be59c1d985c5b5'),
}
VOCR_DEFAULT_MODEL = 'VOCR5_N_w8g2.vocr'  # the default model size's (Normal's), tools/model_tiers.json
# The model sizes' attribution: v0.3.6-head2level.1's ATTRIBUTION.md (in its SHA256SUMS; v0.3.0's, which it
# extends with the head gate's and the two-level head's fits), the licences of what the VOCR5 recognizers were trained on (NVIDIA OCR-Synthetic-Multilingual-v1, CC BY 4.0, among them) and
# their terms (non-commercial use only). The package ships it beside the weights as
# ocr/MODEL-ATTRIBUTION.md, and it is kept under that name in a models directory too: the LMs' release
# names its own attribution ATTRIBUTION.md as well (VOCR_LM_ATTRIBUTION).
VOCR_MODEL_ATTRIBUTION = 'MODEL-ATTRIBUTION.md'
VOCR_MODEL_ATTRIBUTION_TAG = VOCR5_TAG
VOCR_MODEL_ATTRIBUTION_ASSET = 'ATTRIBUTION.md'  # its name in the release
VOCR_MODEL_ATTRIBUTION_PIN = {VOCR_MODEL_ATTRIBUTION: (VOCR5_BASE + VOCR_MODEL_ATTRIBUTION_ASSET, 5536,
    '74b15e8a8ffb01a997ff15bff3ebc137c48a51439732fd838300a9ddff4953c3')}

# The text-presence gate (vita-vn-ocr #100, docs/textgate.md): 1,540 B of int8 weights in front of the
# line finder (ocr_text_gate, off by default), pre-release v0.3.2-textgate.1, whose SHA256SUMS lists these
# hashes; the plugin pins the same size and hash (include/vjo_vocr_sizes.h, core/vocr_ocr.c). Its release's
# ATTRIBUTION.md (its training data's licences; the weights are for non-commercial use only) ships beside it
# as ocr/GATE-ATTRIBUTION.md.
VOCR_GATE_TAG = 'v0.3.2-textgate.1'
VOCR_GATE_BASE = f'https://github.com/{VOCR_REPO}/releases/download/{VOCR_GATE_TAG}/'
VOCR_GATE_MODEL = 'textgate_p8_c8_w8.vtd'
VOCR_GATE_ASSETS = {
    VOCR_GATE_MODEL: (VOCR_GATE_BASE + VOCR_GATE_MODEL, 1540,
        'a4e73ad78451c75a19f88a479fc0108daac85beaa88d875b73212ed3c09d85d1'),
}
VOCR_GATE_ATTRIBUTION = 'GATE-ATTRIBUTION.md'
VOCR_GATE_ATTRIBUTION_ASSET = 'ATTRIBUTION.md'  # its name in the release
VOCR_GATE_ATTRIBUTION_PIN = {VOCR_GATE_ATTRIBUTION: (VOCR_GATE_BASE + VOCR_GATE_ATTRIBUTION_ASSET, 3706,
    '44a353c76e19c6bdabcb9dbd06d14867696dcb59ce0460db22d09749af687f6e')}

# The model generation of each release: vita-vn-ocr's name for the family of models a release
# publishes (it calls v0.2.0's FL10 "VOCR1"). A .vocr blob carries no generation of its own, so
# the pins carry it: core/vocr_models.c per file (the Rubi app names the local OCR engine
# after it) and package-manifest.json (package_release.py). The VOCR2 trial (v0.2.3-vocr2.1)
# was removed from the overlay after v0.8.0-test.1.
VOCR_GENERATION = {VOCR_TAG: 'VOCR1', VOCR3_TAG: 'VOCR3', VOCR4_TAG: 'VOCR4', VOCR5_TAG: 'VOCR5'}

# The model sizes the Rubi app offers (config.ini vocr_tier), each a pinned recognizer above:
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
    for assets, tag in ((VOCR5_ASSETS, VOCR5_TAG), (VOCR3_ASSETS, VOCR3_TAG), (VOCR4_ASSETS, VOCR4_TAG),
                        (VOCR_ASSETS, VOCR_TAG)):
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
    request = urllib.request.Request(url, headers={'User-Agent': 'Rubi-model-setup'})
    with urllib.request.urlopen(request, timeout=90) as response:
        data = response.read(size + 1)
    if not valid(data):
        raise RuntimeError('Incorrect or incomplete download: ' + name)
    return store(path, data)


def fetch_release_asset(directory, name, assets, tag, source=None, asset=None):
    """A release asset: a local copy (--from), else the public download, else
    `gh release download` (private repository or draft release). It is kept as `name`;
    `asset` is its name in the release when that differs (MODEL-ATTRIBUTION.md is v0.3.0's
    ATTRIBUTION.md), and a --from directory may hold it under either name."""
    url, size, sha = assets[name]
    asset = asset or name
    path = directory / name
    def valid(data):
        return len(data) == size and hashlib.sha256(data).hexdigest() == sha
    if path.exists() and valid(path.read_bytes()):
        return path
    if source:
        source = Path(source)
        if not source.is_dir():
            data = source.read_bytes()
        else:
            # its own name first; the release's name only when that copy is this one (the LMs'
            # ATTRIBUTION.md has the same name as the models')
            copies = [source / n for n in dict.fromkeys((name, asset)) if (source / n).is_file()]
            data = next((d for d in (c.read_bytes() for c in copies) if valid(d)),
                        copies[0].read_bytes() if copies else b'')
        if not valid(data):
            raise RuntimeError(f'{source} is not the {tag} {asset} (size or SHA-256 differs)')
        return store(path, data)
    try:
        return fetch(directory, name, assets)
    except (OSError, RuntimeError) as public_error:
        if not shutil.which('gh'):
            raise RuntimeError(f'{url}: {public_error}. Check the network and retry, pass --from with a '
                               f'copy, or install the GitHub CLI (`gh auth login`) for a draft release.') from public_error
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(['gh', 'release', 'download', tag, '-R', VOCR_REPO, '-p', asset, '-D', tmp],
                       check=True, stdin=subprocess.DEVNULL, timeout=300)
        data = (Path(tmp) / asset).read_bytes()
    if not valid(data):
        raise RuntimeError('Incorrect or incomplete download: ' + name)
    return store(path, data)


def fetch_vocr(directory, name, source=None):
    assets, tag = release_assets(name)
    return fetch_release_asset(directory, name, assets, tag, source)


def fetch_tiers(directory, source=None):
    """The three model sizes' recognizers (tools/model_tiers.json), each pinned, and their release's
    attribution (MODEL-ATTRIBUTION.md), with the text-presence gate's weights and attribution
    (GATE-ATTRIBUTION.md): what a package ships. A model two sizes share is fetched once."""
    names = dict.fromkeys(tier['model'] for tier in model_tiers()['tiers'])
    paths = [fetch_vocr(directory, name, source) for name in names]
    paths.append(fetch_release_asset(directory, VOCR_MODEL_ATTRIBUTION, VOCR_MODEL_ATTRIBUTION_PIN,
                                     VOCR_MODEL_ATTRIBUTION_TAG, source, VOCR_MODEL_ATTRIBUTION_ASSET))
    paths += fetch_gate(directory, source)
    return paths


def fetch_gate(directory, source=None):
    """The text-presence gate's weights (VOCR_GATE_MODEL) and their attribution (GATE-ATTRIBUTION.md), pinned."""
    return [fetch_release_asset(directory, VOCR_GATE_MODEL, VOCR_GATE_ASSETS, VOCR_GATE_TAG, source),
            fetch_release_asset(directory, VOCR_GATE_ATTRIBUTION, VOCR_GATE_ATTRIBUTION_PIN, VOCR_GATE_TAG, source,
                                VOCR_GATE_ATTRIBUTION_ASSET)]


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
    ap.add_argument('--model', choices=sorted({**VOCR5_ASSETS, **VOCR3_ASSETS, **VOCR4_ASSETS, **VOCR_ASSETS}),
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
        print('Models ready in', args.output, '- copy them to ux0:data/VitaJPOverlay/ocr/ (keep',
              VOCR_MODEL_ATTRIBUTION, 'and', VOCR_GATE_ATTRIBUTION,
              'beside them: their training data\'s licences and their terms)')
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
