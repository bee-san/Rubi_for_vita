#!/usr/bin/env python3
"""Download the exact local OCR weights; for ncnn, also generate its build headers.

No ONNX/Paddle/Python runtime is needed on the Vita. Run without --ncnn-source
to prepare the model directory to copy to ux0:data/VitaJPOverlay/ocr.

--backend vocr fetches a vita-vn-ocr recognizer (default VOCR6.2_N_w8g2.vocr, the Normal
model size's, from the bee-san/vita-vn-ocr v0.4.1-vocr6.2 pre-release, as are the other sizes' VOCR6.2_S_w8.vocr and
VOCR6.2_L_w8_2l.vocr; VOCR3_H15_w8.vocr and VOCR3_FL10_w8.vocr from the v0.2.5-vocr3.1 pre-release, VOCR4_W20_w8.vocr
from v0.2.5-vocr4.1, H15_w8.vocr, FL10_w8.vocr and F20_w8.vocr from v0.2.0) instead;
--backend vocr-tiers the three model sizes' (tools/model_tiers.json: VOCR6.2_S_w8.vocr,
VOCR6.2_N_w8g2.vocr, VOCR6.2_L_w8_2l.vocr) and their attributions, v0.4.1-vocr6.2's ATTRIBUTION.md kept as
VOCR6-ATTRIBUTION.md and v0.3.6-head2level.1's (the VOCR5 checkpoints they start from) as MODEL-ATTRIBUTION.md, and
the text-presence gate's weights
(textgate_p8_c8_w8.vtd, v0.3.2-textgate.1) with their release's ATTRIBUTION.md kept as GATE-ATTRIBUTION.md, which
a package ships, and (API 54) the text spotter's weights with their release's ATTRIBUTION.md kept as
SPOTTER-ATTRIBUTION.md, pinned in include/vjo_vocr_sizes.h. --backend vocr-tests
fetches what the host tests read from VJO_VOCR_MODEL_DIR (CMakeLists.txt: their real-model checks): every pinned
recognizer, the four LMs, the off-path check's references (VOCR6's plain Normal and Large in a v0.4.1-vocr6.2/
subdirectory) and the
gate's weights (which tools/fast_gate.sh's models lane hands to cmake/VocrGate.cmake); with --check it downloads
nothing, and says which of them the --output directory lacks or holds other bytes of (exit status 1), as
tools/fast_gate.sh's models lane asks first.
Each file comes from the release's public download; only when that fails (a draft release) is an authenticated
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
# v0.3.7-lm6.2, CC BY-SA 4.0: tiers 2 and 4 are VOCR6's (built with the hooked games' text), tiers 1 and 3
# ctclm.2's byte for byte; their ATTRIBUTION.md ships beside them. The plugin pins the same sizes and hashes
# (core/vocr_ocr.c, include/vjo_vocr_sizes.h) and each tier's decoder settings (the release's settings.json).
VOCR_LM_TAG = 'v0.3.7-lm6.2'
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
VOCR_LM_ATTRIBUTION_PIN = {VOCR_LM_ATTRIBUTION: (VOCR_LM_BASE + VOCR_LM_ATTRIBUTION, 6514,
    '9418ed87e6d9fa196aba2315e05a0cf0dca7056482cce03577ced423e5feb1d3')}

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

# VOCR5's head-gated pre-release v0.3.6-head2level.1 (vita-vn-ocr #117): its blobs, the model sizes from API 42 (and
# its VOCR5_S_w8g.vocr Small's until v0.4.1-vocr6.2), are not pinned any more. Its ATTRIBUTION.md is
# (VOCR_MODEL_ATTRIBUTION, below): the training data of the VOCR5 checkpoints every VOCR6 tier starts from.
VOCR5_TAG = 'v0.3.6-head2level.1'
VOCR5_BASE = f'https://github.com/{VOCR_REPO}/releases/download/{VOCR5_TAG}/'
# VOCR6 (vita-vn-ocr #125, docs/vocr6.md "The follow-up: stages 4c and 4d", eval/vocr6_protocol.json stage 4
# amendments 6 and 7): the three model sizes, Small, Normal (the default) and Large, VOCR5-S's, VOCR5-N's and
# VOCR5-L's shapes fine-tuned again on the owner's hooked PS Vita and PSP games (Small at half the learning rate,
# Normal and Large for 45,000 steps), pre-release v0.4.1-vocr6.2 (vita-vn-ocr #157's merge c933e15), whose SHA256SUMS
# lists these hashes. Each blob has the speed sections #120's and #122's rules kept for it: Small's none (plain),
# Normal's the head gate (ε = 1e-4, a 1,792 B VOCG section) and the two-level head (its output rows in frequency
# order, a 1,984 B VOC2 trailer), Large's the two-level head alone. The names carry the set's ".2": v0.4.0-vocr6.1's
# VOCR6_N_w8_2l.vocr and VOCR6_L_w8_2l.vocr (Normal and Large before it, not pinned any more) keep theirs.
VOCR6_TAG = 'v0.4.1-vocr6.2'
VOCR6_BASE = f'https://github.com/{VOCR_REPO}/releases/download/{VOCR6_TAG}/'
VOCR6_ASSETS = {
    'VOCR6.2_S_w8.vocr': (VOCR6_BASE + 'VOCR6.2_S_w8.vocr', 819792,
        '864bbd196d5bc5d3a7bd7918cf7274b3503dfe65d440017ef0781ea7e7ba37d2'),
    'VOCR6.2_N_w8g2.vocr': (VOCR6_BASE + 'VOCR6.2_N_w8g2.vocr', 1189408,
        '107cf084cc0c1a3a61ce4c3f9b9cf6b31d6dff80f0a1bdb94afd3f7ee9ae3bc9'),
    'VOCR6.2_L_w8_2l.vocr': (VOCR6_BASE + 'VOCR6.2_L_w8_2l.vocr', 2475680,
        '65310013e8f990886e4e8097b2fe55eb98cdf082d7193eea510601fc0ac26c82'),
}
VOCR_DEFAULT_MODEL = 'VOCR6.2_N_w8g2.vocr'  # the default model size's (Normal's), tools/model_tiers.json
# The off-path check's references (host/tests/test_vocr.c): each size's blob before its speed sections, which its
# pinned blob read with ocr_head_gate = off gives every logit of: VOCR6's plain Normal and Large, which
# v0.4.1-vocr6.2 publishes beside the blobs with their sections (under v0.4.1-vocr6.2/). Small's pinned blob is the
# plain one itself (None: there is no off path to compare, and the check holds the blob plain). The host tests' set
# only (--backend vocr-tests), never shipped.
VOCR6_PLAIN_DIR = VOCR6_TAG
VOCR6_PLAIN_ASSETS = {
    'VOCR6.2_N_w8.vocr': (VOCR6_BASE + 'VOCR6.2_N_w8.vocr', 1185632,
        'c8ce844ed0a637eb5d263dff7837d846bd2a520278da3b74c49c5ee33eaa24c9'),
    'VOCR6.2_L_w8.vocr': (VOCR6_BASE + 'VOCR6.2_L_w8.vocr', 2473696,
        'be5a8e4f3280b58edc8398616ce5068f9803551407fbc9a843c0a2963584b7b2'),
}
VOCR6_PLAIN_TEST_ASSETS = {f'{VOCR6_PLAIN_DIR}/{name}': pin for name, pin in VOCR6_PLAIN_ASSETS.items()}
# The off-path references by the size whose pinned blob each is read against, smallest first.
OFF_PATH_REFERENCES = (None, f'{VOCR6_PLAIN_DIR}/VOCR6.2_N_w8.vocr', f'{VOCR6_PLAIN_DIR}/VOCR6.2_L_w8.vocr')
# VOCR6-V (vita-vn-ocr #159): vertical text in VOCR5-S's shape, reading a column turned 90 degrees anticlockwise:
# VOCR5-V (#99, v0.3.2-vertical.1) fine-tuned again on the owner's hooked vertical game and new synthetic columns.
# No model size runs it: a job of columns does (config.ini ocr_vertical on or auto). Pre-release
# v0.4.2-vertical6.1, whose SHA256SUMS lists this hash.
VOCR6V_TAG = 'v0.4.2-vertical6.1'
VOCR6V_BASE = f'https://github.com/{VOCR_REPO}/releases/download/{VOCR6V_TAG}/'
VOCR6V_ASSETS = {
    'VOCR6_V_w8.vocr': (VOCR6V_BASE + 'VOCR6_V_w8.vocr', 819792,
        '40485fceaef46a666bcec7781eca88bc005e9211786638ae0dddfac2eb4eea81'),
}
VOCR_VERTICAL_MODEL = 'VOCR6_V_w8.vocr'
# Its release's ATTRIBUTION.md (in its SHA256SUMS): VOCR5-V's training data and terms (non-commercial use only)
# and VOCR6-V's own. The package ships it beside the model as ocr/VERTICAL-ATTRIBUTION.md, the
# other releases' attributions having the same name in their release.
VOCR_VERTICAL_ATTRIBUTION = 'VERTICAL-ATTRIBUTION.md'
VOCR_VERTICAL_ATTRIBUTION_PIN = {VOCR_VERTICAL_ATTRIBUTION: (VOCR6V_BASE + 'ATTRIBUTION.md', 4621,
    '4cee329d657fe6161a37c8c0746181efa3d474686425e4cd7086504a66c323fe')}
# VOCR5's attribution (the checkpoints every model size starts from; no size's blob comes from VOCR5's release any
# more): v0.3.6-head2level.1's ATTRIBUTION.md (in its SHA256SUMS; v0.3.0's, which it
# extends with the head gate's and the two-level head's fits), the licences of what the VOCR5 recognizers were trained on (NVIDIA OCR-Synthetic-Multilingual-v1, CC BY 4.0, among them) and
# their terms (non-commercial use only). The package ships it beside the weights as
# ocr/MODEL-ATTRIBUTION.md, and it is kept under that name in a models directory too: the LMs' release
# names its own attribution ATTRIBUTION.md as well (VOCR_LM_ATTRIBUTION). It covers the VOCR5 checkpoints every
# VOCR6 tier starts from: VOCR6's ATTRIBUTION.md refers to v0.3.0's ("everything v0.3.0's ATTRIBUTION.md lists",
# 3,158 B, not shipped), whose training sources this file, v0.3.6-head2level.1's, lists too.
VOCR_MODEL_ATTRIBUTION = 'MODEL-ATTRIBUTION.md'
VOCR_MODEL_ATTRIBUTION_TAG = VOCR5_TAG
VOCR_MODEL_ATTRIBUTION_ASSET = 'ATTRIBUTION.md'  # its name in the release
VOCR_MODEL_ATTRIBUTION_PIN = {VOCR_MODEL_ATTRIBUTION: (VOCR5_BASE + VOCR_MODEL_ATTRIBUTION_ASSET, 5536,
    '74b15e8a8ffb01a997ff15bff3ebc137c48a51439732fd838300a9ddff4953c3')}
# VOCR6's attribution: v0.4.1-vocr6.2's ATTRIBUTION.md (in its SHA256SUMS): what the three recognizers were
# fine-tuned on beyond VOCR5's data (the owner's hooked PS Vita and PSP games, copyrighted game data used for
# training only; console crops of the same pages; VOCR5's real pools and synthetic replay), their terms
# (non-commercial use only) and the disclosures for test-set numbers. The package ships it beside the weights as
# ocr/VOCR6-ATTRIBUTION.md (each release names its attribution ATTRIBUTION.md).
VOCR6_ATTRIBUTION = 'VOCR6-ATTRIBUTION.md'
VOCR6_ATTRIBUTION_ASSET = 'ATTRIBUTION.md'  # its name in the release
VOCR6_ATTRIBUTION_PIN = {VOCR6_ATTRIBUTION: (VOCR6_BASE + VOCR6_ATTRIBUTION_ASSET, 4442,
    '9a5ff2aee4cb8194f41e54f3a2dac88ce3de6ff4aa7b8ddd123dd5b792c9843b')}

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

# API 54: the text spotter (vita-vn-ocr #149, docs/textspot8.md), the game worker's default finder
# (ocr_finder = spotter; grad turns it off): its int8 weights (a VTSP blob) and its release's ATTRIBUTION.md, which ships beside them as
# ocr/SPOTTER-ATTRIBUTION.md. Their pins are the VJO_VOCR_SPOT_* lines of include/vjo_vocr_sizes.h, the one place
# that C, CMake and these tools all read (a copy of this script beside a copy of that header, as a release zip's
# tools/ holds them, reads that one). An unpublished entry (VJO_VOCR_SPOT_PUBLISHED 0) fetches nothing, and no
# package carries the spotter then.
VOCR_SPOT_HEADER_NAME = 'vjo_vocr_sizes.h'
VOCR_SPOT_HEADERS = (Path(__file__).resolve().parent.parent / 'include' / VOCR_SPOT_HEADER_NAME,
                     Path(__file__).resolve().parent / VOCR_SPOT_HEADER_NAME)
VOCR_SPOT_FIELDS = {  # the macro after VJO_VOCR_SPOT_, and the form of its value
    'TAG': r'"(v[0-9][0-9A-Za-z.\-]*)"', 'NAME': r'"([A-Za-z0-9_.\-]+\.vtsp)"', 'BYTES': r'(\d+)u',
    'SHA256': r'"([0-9a-f]{64})"', 'ATTRIBUTION_BYTES': r'(\d+)u', 'ATTRIBUTION_SHA256': r'"([0-9a-f]{64})"',
    'FORM': r'"(trunk|ms)"', 'PUBLISHED': r'([01])'}


def spot_pin(header=None):
    """The spotter's pins: include/vjo_vocr_sizes.h's VJO_VOCR_SPOT_* lines (each exactly once, in its form) as
    {'tag', 'name', 'bytes', 'sha256', 'attribution_bytes', 'attribution_sha256', 'form', 'published'}, or None when
    no copy of the header is here."""
    paths = [Path(header)] if header else [p for p in VOCR_SPOT_HEADERS if p.is_file()]
    if not paths:
        return None
    text = paths[0].read_text(encoding='utf-8')
    pin = {}
    for field, form in VOCR_SPOT_FIELDS.items():
        found = re.findall(rf'^#define VJO_VOCR_SPOT_{field} (\S+)$', text, re.M)
        value = re.fullmatch(form, found[0]) if len(found) == 1 else None
        if not value:
            raise ValueError(f'{paths[0]}: VJO_VOCR_SPOT_{field} is not one line of the form {form} ({found})')
        pin[field.lower()] = int(value.group(1)) if field.endswith('BYTES') or field == 'PUBLISHED' else value.group(1)
    pin['published'] = bool(pin['published'])
    if pin['published'] and (set(pin['sha256']) == {'0'} or set(pin['attribution_sha256']) == {'0'} or
                             not pin['attribution_bytes']):
        raise ValueError(f'{paths[0]}: VJO_VOCR_SPOT_PUBLISHED 1 with a placeholder hash or size')
    return pin


VOCR_SPOT_PIN = spot_pin()
VOCR_SPOT_PUBLISHED = bool(VOCR_SPOT_PIN and VOCR_SPOT_PIN['published'])
VOCR_SPOT_TAG = VOCR_SPOT_PIN['tag'] if VOCR_SPOT_PIN else None
VOCR_SPOT_BASE = f'https://github.com/{VOCR_REPO}/releases/download/{VOCR_SPOT_TAG}/'
VOCR_SPOT_MODEL = VOCR_SPOT_PIN['name'] if VOCR_SPOT_PIN else None
VOCR_SPOT_ASSETS = {VOCR_SPOT_MODEL: (VOCR_SPOT_BASE + VOCR_SPOT_MODEL, VOCR_SPOT_PIN['bytes'],
                                      VOCR_SPOT_PIN['sha256'])} if VOCR_SPOT_PIN else {}
VOCR_SPOT_ATTRIBUTION = 'SPOTTER-ATTRIBUTION.md'
VOCR_SPOT_ATTRIBUTION_ASSET = 'ATTRIBUTION.md'  # its name in the release
VOCR_SPOT_ATTRIBUTION_PIN = {VOCR_SPOT_ATTRIBUTION: (VOCR_SPOT_BASE + VOCR_SPOT_ATTRIBUTION_ASSET,
    VOCR_SPOT_PIN['attribution_bytes'], VOCR_SPOT_PIN['attribution_sha256'])} if VOCR_SPOT_PIN else {}

# The model generation of each release: vita-vn-ocr's name for the family of models a release
# publishes (it calls v0.2.0's FL10 "VOCR1"). A .vocr blob carries no generation of its own, so
# the pins carry it: core/vocr_models.c per file (the Rubi app names the local OCR engine
# after it) and package-manifest.json (package_release.py). The VOCR2 trial (v0.2.3-vocr2.1)
# was removed from the overlay after v0.8.0-test.1.
VOCR_GENERATION = {VOCR_TAG: 'VOCR1', VOCR3_TAG: 'VOCR3', VOCR4_TAG: 'VOCR4', VOCR6V_TAG: 'VOCR6', VOCR6_TAG: 'VOCR6'}

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
    for assets, tag in ((VOCR6_ASSETS, VOCR6_TAG), (VOCR3_ASSETS, VOCR3_TAG), (VOCR4_ASSETS, VOCR4_TAG),
                        (VOCR_ASSETS, VOCR_TAG), (VOCR6V_ASSETS, VOCR6V_TAG)):
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
    `asset` is its name in the release when that differs (MODEL-ATTRIBUTION.md is v0.3.6-head2level.1's
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
    """The three model sizes' recognizers (tools/model_tiers.json), each pinned, and their attributions
    (VOCR6-ATTRIBUTION.md: their release's; MODEL-ATTRIBUTION.md: VOCR5's, the checkpoints VOCR6 starts from),
    with the text-presence gate's weights and attribution
    (GATE-ATTRIBUTION.md), and the vertical model (VOCR_VERTICAL_MODEL, config.ini ocr_vertical) and its
    release's attribution (VERTICAL-ATTRIBUTION.md), and, once published, the text spotter's
    (SPOTTER-ATTRIBUTION.md): what a package ships. A model two sizes share is fetched once."""
    names = dict.fromkeys(tier['model'] for tier in model_tiers()['tiers'])
    paths = [fetch_vocr(directory, name, source) for name in names]
    paths.append(fetch_release_asset(directory, VOCR_MODEL_ATTRIBUTION, VOCR_MODEL_ATTRIBUTION_PIN,
                                     VOCR_MODEL_ATTRIBUTION_TAG, source, VOCR_MODEL_ATTRIBUTION_ASSET))
    paths.append(fetch_release_asset(directory, VOCR6_ATTRIBUTION, VOCR6_ATTRIBUTION_PIN, VOCR6_TAG, source,
                                     VOCR6_ATTRIBUTION_ASSET))
    paths += fetch_gate(directory, source)
    paths.append(fetch_vocr(directory, VOCR_VERTICAL_MODEL, source))
    paths.append(fetch_release_asset(directory, VOCR_VERTICAL_ATTRIBUTION, VOCR_VERTICAL_ATTRIBUTION_PIN,
                                     VOCR6V_TAG, source, 'ATTRIBUTION.md'))
    paths += fetch_spot(directory, source)
    return paths


def fetch_spot(directory, source=None):
    """The text spotter's weights (VOCR_SPOT_MODEL) and their attribution (SPOTTER-ATTRIBUTION.md), pinned; nothing
    while its pin is unpublished (VOCR_SPOT_PUBLISHED false)."""
    if not VOCR_SPOT_PUBLISHED:
        return []
    return [fetch_release_asset(directory, VOCR_SPOT_MODEL, VOCR_SPOT_ASSETS, VOCR_SPOT_TAG, source),
            fetch_release_asset(directory, VOCR_SPOT_ATTRIBUTION, VOCR_SPOT_ATTRIBUTION_PIN, VOCR_SPOT_TAG, source,
                                VOCR_SPOT_ATTRIBUTION_ASSET)]


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


def test_models():
    """(name, asset table, release tag) of every file the host tests read from VJO_VOCR_MODEL_DIR: each pinned
    recognizer, shipped or not (every release table here, which tools/tests/test_vocr_defaults.py holds equal to
    the plugin's, core/vocr_ocr.c), the four LMs, the off-path check's references (host/tests/test_vocr.c:
    VOCR6's plain Normal and Large in v0.4.1-vocr6.2/), and the text-presence gate's
    weights, which the host builds take from cmake/VocrGate.cmake (tools/fast_gate.sh's models lane gives it this
    copy: VJO_VOCR_GATE_BLOB), and (API 54) the text spotter's published weights (host/tests/test_spot_release.c). A
    name is the file's path under the directory. No attribution: no test reads one."""
    tables = ((VOCR6_ASSETS, VOCR6_TAG), (VOCR6V_ASSETS, VOCR6V_TAG),
              (VOCR4_ASSETS, VOCR4_TAG), (VOCR3_ASSETS, VOCR3_TAG), (VOCR_ASSETS, VOCR_TAG),
              (VOCR_LM_ASSETS, VOCR_LM_TAG), (VOCR6_PLAIN_TEST_ASSETS, VOCR6_TAG),
              (VOCR_GATE_ASSETS, VOCR_GATE_TAG)) + \
        (((VOCR_SPOT_ASSETS, VOCR_SPOT_TAG),) if VOCR_SPOT_PUBLISHED else ())
    return [(name, assets, tag) for assets, tag in tables for name in assets]


def fetch_test_models(directory, source=None):
    """test_models(), each fetched as the other backends fetch theirs (a valid copy in `directory` is kept); a file
    in a subdirectory (v0.4.1-vocr6.2/) is its release's asset of the file's own name."""
    return [fetch_release_asset(directory, name, assets, tag, source, Path(name).name)
            for name, assets, tag in test_models()]


def check_models(directory, models):
    """What `directory` lacks of `models` ((name, asset table, tag), as test_models()): one line per file that
    is missing or not its pinned size and SHA-256, [] when every one is the pinned file. Reads only."""
    problems = []
    for name, assets, tag in models:
        _, size, sha = assets[name]
        path = Path(directory) / name
        if not path.is_file():
            problems.append(f'{name}: missing ({tag})')
            continue
        data = path.read_bytes()
        if len(data) != size:
            problems.append(f'{name}: {len(data)} B, not the pinned {size} B ({tag})')
        elif hashlib.sha256(data).hexdigest() != sha:
            problems.append(f'{name}: not the pinned SHA-256 ({tag})')
    return problems



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
    ap.add_argument('--backend', choices=('ncnn', 'vocr', 'vocr-tiers', 'vocr-lm', 'vocr-tests'), default='ncnn',
                    help="vocr-tiers: the three model sizes' recognizers (Small, Normal, Large); "
                         "vocr-lm: the beam + LM decoder's four LMs and their ATTRIBUTION.md; "
                         "vocr-tests: every pinned recognizer, the LMs, the off-path references (v0.4.1-vocr6.2/) "
                         "and the gate's weights (VJO_VOCR_MODEL_DIR)")
    ap.add_argument('--ncnn-source', type=Path)
    ap.add_argument('--model', choices=sorted({**VOCR6_ASSETS, **VOCR3_ASSETS, **VOCR4_ASSETS, **VOCR_ASSETS,
                                               **VOCR6V_ASSETS}),
                    help=f'vocr: which recognizer (default: {VOCR_DEFAULT_MODEL})')
    ap.add_argument('--from', dest='source', help='vocr: an already downloaded asset (file or directory)')
    ap.add_argument('--check', action='store_true',
                    help='vocr-tests: download nothing; exit 1 unless --output holds every one, pinned')
    args = ap.parse_args()
    if args.check:
        if args.backend != 'vocr-tests' or args.source:
            ap.error('--check is for --backend vocr-tests, without --from')
        problems = check_models(args.output, test_models())
        for problem in problems:
            print(problem)
        print(f'{len(test_models()) - len(problems)} of {len(test_models())} pinned test models verified in',
              args.output)
        raise SystemExit(1 if problems else 0)
    args.output.mkdir(parents=True, exist_ok=True)
    if args.backend == 'vocr-tests':
        for path in fetch_test_models(args.output, args.source):
            print('Verified', path.name)
        print('Test models ready in', args.output, '- VJO_VOCR_MODEL_DIR for the host tests (tools/fast_gate.sh:',
              'the models lane)')
        return
    if args.backend == 'vocr-lm':
        for path in fetch_lms(args.output, args.source):
            print('Verified', path.name)
        print('LMs ready in', args.output, '- copy them to ux0:data/VitaJPOverlay/ocr/ (CC BY-SA 4.0: keep',
              VOCR_LM_ATTRIBUTION, 'beside them)')
        return
    if args.backend == 'vocr-tiers':
        for path in fetch_tiers(args.output, args.source):
            print('Verified', path.name)
        if not VOCR_SPOT_PUBLISHED:
            print('The text spotter (ocr_finder = spotter):', 'its pin is unpublished',
                  VOCR_SPOT_TAG or '', '(include/vjo_vocr_sizes.h VJO_VOCR_SPOT_PUBLISHED): not fetched')
        keep = [VOCR_MODEL_ATTRIBUTION, VOCR6_ATTRIBUTION, VOCR_GATE_ATTRIBUTION, VOCR_VERTICAL_ATTRIBUTION] + \
            ([VOCR_SPOT_ATTRIBUTION] if VOCR_SPOT_PUBLISHED else [])
        print('Models ready in', args.output, '- copy them to ux0:data/VitaJPOverlay/ocr/ (keep',
              ', '.join(keep[:-1]), 'and', keep[-1],
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
