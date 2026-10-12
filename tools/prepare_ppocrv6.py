#!/usr/bin/env python3
"""PP-OCRv6 small recognizer (ocr_backend = ppocrv6): fetch or rebuild its int8 model.

  prepare_ppocrv6.py fetch --output ocr [--from FILE]
      Download the pinned int8 weights (release asset RELEASE_TAG, through
      `gh release download` while it is a draft) or copy a local file (a
      download, or the output of `build`) into ocr/ after checking its size and
      SHA-256.

  prepare_ppocrv6.py build --work DIR --realdata DIR [--output ocr]
      Rebuild the weights from their upstream sources, every input pinned:
        1. PaddleOCR's PP-OCRv6 small rec as RapidOCR 3.10.0 publishes it (ONNX);
        2. its CTC head pruned to JIS X 0208 + ASCII + full-width forms (the class
           set vita-vn-ocr's eval masks with), the final Softmax dropped (greedy
           CTC decodes the same argmax);
        3. converted by pnnx 20260704 and rewritten for the overlay's pinned ncnn;
        4. int8-quantised by that ncnn's own ncnn2table (KL) and ncnn2int8, calibrated
           on real VN dialogue line crops from training-split games only
           (tools/ppocrv6_calibration.jsonl; never an eval test game);
        5. checked byte for byte against the pins below.
      Needs network access, cmake, a C++ compiler and Python with numpy, onnx and
      Pillow (tools/requirements-ppocrv6.txt). --realdata names a directory with
      vita-vn-ocr realdata's screenshots (shots/<kind>/<vn>_<id>.jpg); missing
      screenshots are downloaded from VNDB and checked against their SHA-256.

  prepare_ppocrv6.py header --ncnn-source DIR --output FILE
      Generate the build header (binary param, class list, pins) from the
      committed param and dictionary in third_party/ppocrv6/ (CMake runs this).

The weights and the architecture are PaddleOCR's (Apache-2.0); see
third_party/ppocrv6/README.md. They are distributed as a release asset and are
never committed to this repository.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
DATA_DIR = ROOT / 'third_party' / 'ppocrv6'
CALIBRATION = ROOT / 'tools' / 'ppocrv6_calibration.jsonl'
USER_AGENT = 'VitaJPOverlay-model-setup'

# Upstream inputs.
ONNX = ('https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv6/rec/'
        'PP-OCRv6_rec_small.onnx', 21234383,
        '6f327246b50388f3c176ae304bd95767ea6dc0c9ae92153ef8cbe210b3c14884')
PNNX = ('https://github.com/pnnx/pnnx/releases/download/20260704/pnnx-20260704-linux.zip', 28474269,
        '685ee1d718068809bdbbc7a9417fa4e2e98a9f2164e8ad9fad73f63377464066')
PNNX_BINARY_SHA256 = 'e639b4b6dd0c4922cde7a324385356f789fdd404507f7397a0b8412d7db86326'
NCNN_REV = '3798be64cfeeebc0afcfcebfb43ed5c9e1b72e82'  # cmake/LocalOCR.cmake
NCNN = (f'https://github.com/Tencent/ncnn/archive/{NCNN_REV}.tar.gz', None,
        '276ed0523e7a80048957c2bff89f37242cadd8858ed026a5538222af338e9fde')

# Outputs. The game worker embeds the param and classes (third_party/ppocrv6)
# and accepts only these weights (include/vjo_vocr_sizes.h, kernel broker).
MODEL_NAME = 'PP-OCRv6_small_jis_int8.bin'
PARAM_NAME = 'PP-OCRv6_small_jis_int8.ncnn.param'
DICT_NAME = 'ppocrv6_jis_dict.txt'
RELEASE_REPO = 'bee-san/vita-jp-overlay'
RELEASE_TAG = 'ppocrv6-small-jis-int8.1'
WEIGHTS = (f'https://github.com/{RELEASE_REPO}/releases/download/{RELEASE_TAG}/{MODEL_NAME}',
           3893668, '2555ebd7c7734032ad09a1ea5c63efaa690906af56947ed5203f19b9563b1320')

# The JIS X 0208 + ASCII + full-width set vita-vn-ocr's eval masks the head with.
def jis_classes():
    keep = set()
    for a in range(0xA1, 0xFF):
        for b in range(0xA1, 0xFF):
            try:
                keep.add(bytes([a, b]).decode('euc_jp'))
            except UnicodeDecodeError:
                pass
    keep |= {chr(c) for c in range(0x21, 0x7F)}
    keep |= {chr(c) for c in range(0xFF01, 0xFF5F)}
    keep |= set('♪♡♥☆★〜～')
    return keep


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def download(url, dest, sha256, size=None):
    """dest (cached) after checking its SHA-256 (and size); downloads when absent or wrong."""
    dest = Path(dest)
    if dest.exists() and sha256_file(dest) == sha256 and (size is None or dest.stat().st_size == size):
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + '.part')
    request = urllib.request.Request(url, headers={'User-Agent': USER_AGENT})
    with urllib.request.urlopen(request, timeout=600) as response, open(tmp, 'wb') as out:
        shutil.copyfileobj(response, out)
    got = sha256_file(tmp)
    if got != sha256 or (size is not None and tmp.stat().st_size != size):
        tmp.unlink()
        raise RuntimeError(f'{url}: sha256 {got} (size {tmp.stat().st_size if tmp.exists() else "?"}) '
                           f'is not the pinned {sha256}')
    tmp.replace(dest)
    return dest


def prune(onnx_path, out_onnx, out_dict):
    """Keep blank, the JIS classes and the trailing space; drop the final Softmax."""
    import numpy as np
    import onnx
    from onnx import numpy_helper
    model = onnx.load(str(onnx_path))
    meta = {p.key: p.value for p in model.metadata_props}
    vocab = meta['character'].split('\n') + [' ']   # class i = vocab[i-1]; 0 = CTC blank
    keep_chars = jis_classes()
    keep = [0] + [i + 1 for i, ch in enumerate(vocab) if ch in keep_chars or ch == ' ']
    nodes = list(model.graph.node)
    softmax = nodes[-1]
    assert softmax.op_type == 'Softmax' and softmax.output[0] == model.graph.output[0].name
    producer = {n.output[0]: n for n in nodes}
    tail = [softmax]
    add = producer[softmax.input[0]]
    if add.op_type == 'Identity':
        tail.append(add)
        add = producer[add.input[0]]
    assert add.op_type == 'Add', add.op_type
    matmul = producer[next(t for t in add.input if t in producer)]
    assert matmul.op_type == 'MatMul', matmul.op_type
    inits = {i.name: i for i in model.graph.initializer}
    w_name = next(t for t in matmul.input if t in inits)
    b_name = next(t for t in add.input if t in inits)
    weights, bias = numpy_helper.to_array(inits[w_name]), numpy_helper.to_array(inits[b_name])
    assert weights.shape[-1] == len(vocab) + 1 == bias.shape[-1], (weights.shape, len(vocab))
    index = np.array(keep)
    inits[w_name].CopyFrom(numpy_helper.from_array(np.ascontiguousarray(weights[..., index]), w_name))
    inits[b_name].CopyFrom(numpy_helper.from_array(np.ascontiguousarray(bias[..., index]), b_name))
    # Greedy CTC takes the argmax: the logits decode exactly as the softmax would.
    output = model.graph.output[0]
    for n in tail:
        model.graph.node.remove(n)
    add.output[0] = output.name
    for vi in list(model.graph.value_info):
        model.graph.value_info.remove(vi)
    output.type.tensor_type.shape.dim[-1].dim_value = len(keep)
    for p in list(model.metadata_props):
        if p.key == 'character':
            p.value = '\n'.join(vocab[i - 1] for i in keep[1:-1])
    onnx.checker.check_model(model)
    onnx.save(model, str(out_onnx))
    kept = [vocab[i - 1] for i in keep[1:]]
    Path(out_dict).write_text('\n'.join(kept) + '\n', encoding='utf-8')
    return len(vocab) + 1, len(keep)


def fix_param(text):
    """Make pnnx's ncnn param loadable by the overlay's pinned, batch-less ncnn.
    The weights file is unchanged: no rewritten layer carries weights.

      Reshape with batch-axis params 12=0 13=0: drop the batch dimension (1) and
          the two params, the form the pinned ncnn without NCNN_BATCH accepts.
      Reduction mean over (h, w) with keepdims (the squeeze-excitation squeeze):
          adaptive average Pooling to 1x1 (0=1 7=1 8=1 18=1), which keeps the
          [c, 1, 1] blob the excitation convolutions expect, so no convolution
          takes ncnn's 1-D input compatibility path (which breaks int8).
    """
    lines = text.strip().splitlines()
    assert lines[0] == '7767517'
    out = lines[:2]
    reshapes = reductions = 0
    for line in lines[2:]:
        w = line.split()
        kind, name, nb, nt = w[0], w[1], int(w[2]), int(w[3])
        ins, outs = w[4:4 + nb], w[4 + nb:4 + nb + nt]
        p = dict(x.split('=', 1) for x in w[4 + nb + nt:])
        if kind == 'Reshape' and ('12' in p or '13' in p):
            assert p.pop('12') == '0' and p.pop('13') == '0', line
            dims = [p[k] for k in ('0', '1', '11', '2') if k in p]  # w, h, d, c (inner -> outer)
            assert dims[-1] == '1', f'outermost (batch) dimension is not 1: {line}'
            dims = dims[:-1]
            p = dict(zip({1: ['0'], 2: ['0', '1'], 3: ['0', '1', '2']}[len(dims)], dims))
            reshapes += 1
        elif kind == 'Reduction':
            assert p == {'0': '3', '1': '0', '-23303': '2,1,2', '4': '1', '5': '1'}, line
            kind, p = 'Pooling', {'0': '1', '7': '1', '8': '1', '18': '1'}
            reductions += 1
        fields = [f'{kind:<24s}', f'{name:<24s}', str(nb), str(nt)] + ins + outs + [f'{k}={v}' for k, v in p.items()]
        out.append(' '.join(fields))
    assert reshapes == 2 and reductions == 5, (reshapes, reductions)
    return '\n'.join(out) + '\n'


def layer_types(ncnn_source):
    """ncnn's layer type indices: ncnn_add_layer order, disabled layers included."""
    return re.findall(r'^ncnn_add_layer\((\w+)', (Path(ncnn_source) / 'src/CMakeLists.txt').read_text(), re.M)


def param_bin(param_text, ncnn_source):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from prepare_ocr import binary_param
    return binary_param(param_text, layer_types(ncnn_source))


def read_dict(path):
    entries = Path(path).read_text(encoding='utf-8').split('\n')
    if entries and entries[-1] == '':
        entries.pop()
    assert entries and entries[-1] == ' ' and all(entries), 'dictionary: one class per line, final space'
    return entries


def c_bytes(data, per_line=24):
    return [','.join(str(b) for b in data[i:i + per_line]) + ',' for i in range(0, len(data), per_line)]


# ------------------------------------------------------------------ rebuild

CALIB_WIDTH = 640  # ncnn2table needs one input shape: lines are padded/cut to it
# KL alone clips this model's activations hard (thresholds as low as 6% of the
# calibration maximum, median 44%) and loses 0.7-0.9 CER points. Every activation threshold
# is raised to at least CLIP_FLOOR x its calibration maximum; the fraction was
# chosen on realdata's held-out val split, not on eval's test sets.
CLIP_FLOOR = 0.67
QUANT_THREADS = int(os.environ.get('OMP_NUM_THREADS') or 4)


def run(cmd, log, **kw):
    with open(log, 'w') as out:
        subprocess.run([str(c) for c in cmd], check=True, stdout=out, stderr=subprocess.STDOUT,
                       stdin=subprocess.DEVNULL, **kw)


def pnnx_binary(work):
    import zipfile
    archive = download(PNNX[0], work / 'dl' / 'pnnx-20260704-linux.zip', PNNX[2], PNNX[1])
    binary = work / 'pnnx' / 'pnnx'
    if not binary.exists() or sha256_file(binary) != PNNX_BINARY_SHA256:
        binary.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(archive) as z:
            binary.write_bytes(z.read('pnnx-20260704-linux/pnnx'))
        binary.chmod(0o755)
    if sha256_file(binary) != PNNX_BINARY_SHA256:
        raise RuntimeError('pnnx binary differs from its pin')
    return binary


def ncnn_tools(work):
    """ncnn2table and ncnn2int8 of the pinned ncnn, x86 SSE2 only (no runtime
    CPU dispatch), so every host computes the same calibration statistics."""
    import tarfile
    src = work / f'ncnn-{NCNN_REV}'
    if not (src / 'CMakeLists.txt').exists():
        archive = download(NCNN[0], work / 'dl' / f'ncnn-{NCNN_REV}.tar.gz', NCNN[2])
        with tarfile.open(archive) as t:
            t.extractall(work)
    build = work / 'ncnn-tools'
    off = ['VULKAN', 'BF16', 'RUNTIME_CPU', 'AVX', 'AVX2', 'AVX512', 'FMA', 'F16C', 'XOP', 'AVXVNNI',
           'AVXVNNIINT8', 'AVXVNNIINT16', 'AVXNECONVERT', 'AVX512VNNI', 'AVX512BF16', 'AVX512FP16',
           'SHARED_LIB', 'BUILD_EXAMPLES', 'BUILD_BENCHMARK', 'BUILD_TESTS', 'PYTHON']
    on = ['OPENMP', 'THREADS', 'INT8', 'BUILD_TOOLS', 'SIMPLEOCV', 'PIXEL']
    run(['cmake', '-S', src, '-B', build, '-DCMAKE_BUILD_TYPE=Release']
        + [f'-DNCNN_{o}=OFF' for o in off] + [f'-DNCNN_{o}=ON' for o in on], work / 'ncnn-tools-configure.log')
    run(['cmake', '--build', build, '--parallel', str(QUANT_THREADS), '--target', 'ncnn2table', 'ncnn2int8'],
        work / 'ncnn-tools-build.log')
    return src, build / 'tools' / 'quantize' / 'ncnn2table', build / 'tools' / 'quantize' / 'ncnn2int8'


def calibration_images(work, shots_dir):
    """The calibration manifest's line crops, as the game worker feeds them:
    console luma in all three channels, bilinear to height 48 (width at most
    1024), then cut or edge-padded to CALIB_WIDTH."""
    import numpy as np
    from PIL import Image
    out = work / 'calib'
    out.mkdir(parents=True, exist_ok=True)
    paths = []
    for line in CALIBRATION.read_text().splitlines():
        row = json.loads(line)
        name = f"{row['vn']}_{row['url'].rsplit('/', 1)[1]}"
        shot = Path(shots_dir) / row['kind'] / name if shots_dir else None
        if not shot or not shot.exists() or sha256_file(shot) != row['sha256']:
            shot = download(row['url'], work / 'shots' / row['kind'] / name, row['sha256'])
        rgb = Image.open(shot).convert('RGB')
        if list(rgb.size) != row['dims']:
            raise RuntimeError(f'{shot}: size {rgb.size} is not {row["dims"]}')
        x0, y0, x1, y1 = row['ink']
        pad = row['pad']
        box = (max(0, x0 - pad), max(0, y0 - pad), min(rgb.width, x1 + pad), min(rgb.height, y1 + pad))
        px = np.asarray(rgb.crop(box), dtype=np.uint16)
        w, h = box[2] - box[0], box[3] - box[1]
        grey = Image.fromarray(((77 * px[..., 0] + 150 * px[..., 1] + 29 * px[..., 2]) >> 8).astype(np.uint8))
        tw = min(1024, max(8, (w * 48 + h // 2) // h))
        grey = grey.resize((tw, 48), Image.BILINEAR)
        canvas = Image.new('L', (CALIB_WIDTH, 48))
        canvas.paste(grey.crop((0, 0, min(tw, CALIB_WIDTH), 48)), (0, 0))
        if tw < CALIB_WIDTH:
            edge = grey.crop((tw - 1, 0, tw, 48)).resize((CALIB_WIDTH - tw, 48), Image.NEAREST)
            canvas.paste(edge, (tw, 0))
        path = out / f"{row['id']}.ppm"
        canvas.convert('RGB').save(path)
        paths.append(path)
    listing = work / 'calib_list.txt'
    listing.write_text(''.join(f'{p}\n' for p in paths))
    return listing, len(paths)


def floor_table(table, log, out, frac):
    """ncnn2table's KL table with every activation threshold raised to at least
    frac x that blob's calibration maximum (from ncnn2table's own log). Weight
    scales (`*_param_N`) are unchanged."""
    stats = {}
    for line in Path(log).read_text().splitlines():
        m = re.match(r'^(\S+)\s*: max = (\S+)\s+threshold = (\S+)\s+scale = (\S+)', line)
        if m:
            stats[m.group(1)] = (float(m.group(2)), float(m.group(3)))
    rows, changed = [], 0
    for line in Path(table).read_text().splitlines():
        w = line.split()
        if w and w[0] in stats and len(w) == 2:
            mx, threshold = stats[w[0]]
            w[1] = repr(127.0 / max(threshold, frac * mx))
            changed += 1
        if w:
            rows.append(' '.join(w))
    if not changed or changed != len(stats):
        raise RuntimeError(f'{table}: {changed} activation scales for {len(stats)} logged blobs')
    Path(out).write_text('\n'.join(rows) + '\n')
    return changed


def build(work, shots_dir, output):
    work.mkdir(parents=True, exist_ok=True)
    onnx_path = download(ONNX[0], work / 'dl' / 'PP-OCRv6_rec_small.onnx', ONNX[2], ONNX[1])
    conv = work / 'convert'
    conv.mkdir(exist_ok=True)
    classes, kept = prune(onnx_path, conv / 'v6s_jis.onnx', work / DICT_NAME)
    print(f'head pruned: {classes} -> {kept} classes')
    pnnx = pnnx_binary(work)
    run([pnnx, 'v6s_jis.onnx', 'inputshape=[1,3,48,320]', 'inputshape2=[1,3,48,640]', 'fp16=0'],
        work / 'pnnx.log', cwd=conv)
    fp32_param = work / 'PP-OCRv6_small_jis_fp32.ncnn.param'
    fp32_param.write_text(fix_param((conv / 'v6s_jis.ncnn.param').read_text()))
    fp32_bin = work / 'PP-OCRv6_small_jis_fp32.bin'
    shutil.copyfile(conv / 'v6s_jis.ncnn.bin', fp32_bin)
    src, ncnn2table, ncnn2int8 = ncnn_tools(work)
    listing, n = calibration_images(work, shots_dir)
    print(f'calibration: {n} line crops')
    kl_table, table = work / 'v6s_jis_kl.table', work / 'v6s_jis.table'
    norm = 1 / 127.5
    run([ncnn2table, fp32_param, fp32_bin, listing, kl_table, 'mean=[127.5,127.5,127.5]',
         f'norm=[{norm},{norm},{norm}]', f'shape=[{CALIB_WIDTH},48,3]', 'pixel=BGR',
         f'thread={QUANT_THREADS}', 'method=kl'], work / 'ncnn2table.log')
    print(f'activation thresholds floored at {CLIP_FLOOR} x max:',
          floor_table(kl_table, work / 'ncnn2table.log', table, CLIP_FLOOR))
    int8_param, int8_bin = work / PARAM_NAME, work / MODEL_NAME
    run([ncnn2int8, fp32_param, fp32_bin, int8_param, int8_bin, table], work / 'ncnn2int8.log')
    receipt = {
        'onnx': {'url': ONNX[0], 'sha256': ONNX[2]}, 'pnnx': {'url': PNNX[0], 'sha256': PNNX[2]},
        'ncnn': NCNN_REV, 'calibration': {'manifest_sha256': sha256_file(CALIBRATION), 'lines': n,
                                          'width': CALIB_WIDTH, 'method': 'kl',
                                          'clip_floor': CLIP_FLOOR},
        'classes': kept,
        'files': {p.name: {'bytes': p.stat().st_size, 'sha256': sha256_file(p)}
                  for p in (int8_bin, int8_param, work / DICT_NAME, fp32_bin, fp32_param, table)},
    }
    (work / 'build-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt['files'], indent=2))
    problems = []
    if WEIGHTS[2] != '0' * 64 and (int8_bin.stat().st_size != WEIGHTS[1] or sha256_file(int8_bin) != WEIGHTS[2]):
        problems.append(f'{MODEL_NAME} differs from the pinned weights')
    for name in (PARAM_NAME, DICT_NAME):
        committed = DATA_DIR / name
        if committed.exists() and committed.read_bytes() != (work / name).read_bytes():
            problems.append(f'{name} differs from third_party/ppocrv6/{name}')
    if problems:
        raise SystemExit('rebuild mismatch: ' + '; '.join(problems))
    if output:
        output.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(int8_bin, output / MODEL_NAME)
        print('Model ready in', output, '- copy', MODEL_NAME, 'to ux0:data/VitaJPOverlay/ocr/')


def header(param_path, dict_path, ncnn_source, weights_bytes, weights_sha256):
    """The build header: binary param, blob ids, classes and the weights' pin."""
    param, input_id, output_id = param_bin(Path(param_path).read_text(), ncnn_source)
    classes = read_dict(dict_path)
    chars, offsets = bytearray(), []
    for entry in classes:
        offsets.append(len(chars))
        chars += entry.encode('utf-8') + b'\0'
    assert len(chars) < 65536
    digest = bytes.fromhex(weights_sha256)
    lines = ['// Generated by tools/prepare_ppocrv6.py header; do not edit.',
             '// PP-OCRv6 small rec (PaddleOCR, Apache-2.0), head pruned to JIS X 0208: third_party/ppocrv6.',
             '#ifndef VJO_PPOCRV6_DATA_H', '#define VJO_PPOCRV6_DATA_H', '#include <stdint.h>',
             f'#define VJO_PPOCRV6_CLASSES {len(classes)}u',
             f'#define VJO_PPOCRV6_WEIGHTS_BYTES {weights_bytes}u',
             'static const unsigned char vjo_ppocrv6_weights_sha256[32] = {' + ','.join(str(b) for b in digest) + '};',
             'alignas(4) static const unsigned char vjo_ppocrv6_param[] = {']
    lines += c_bytes(param)
    lines += ['};', f'static const int vjo_ppocrv6_input = {input_id};',
              f'static const int vjo_ppocrv6_output = {output_id};',
              'static const unsigned char vjo_ppocrv6_chars[] = {']
    lines += c_bytes(chars, 32)
    lines += ['};', 'static const uint16_t vjo_ppocrv6_offsets[] = {']
    lines += [','.join(str(o) for o in offsets[i:i + 16]) + ',' for i in range(0, len(offsets), 16)]
    lines += ['};', '#endif', '']
    return '\n'.join(lines)


def fetch(output, source=None):
    """The pinned weights in output/: a local copy (--from FILE or DIRECTORY), the
    release download, or `gh release download` (a draft or private release)."""
    output.mkdir(parents=True, exist_ok=True)
    dest = output / MODEL_NAME
    def valid(path):
        return path.is_file() and path.stat().st_size == WEIGHTS[1] and sha256_file(path) == WEIGHTS[2]
    if valid(dest):
        return dest
    if source:
        source = Path(source)
        source = source / MODEL_NAME if source.is_dir() else source
        if not valid(source):
            raise SystemExit(f'{source} is not the pinned {MODEL_NAME} (size or SHA-256 differs)')
        shutil.copyfile(source, dest)
        return dest
    try:
        return download(WEIGHTS[0], dest, WEIGHTS[2], WEIGHTS[1])
    except (OSError, RuntimeError) as public_error:
        if not shutil.which('gh'):
            raise SystemExit(f'{WEIGHTS[0]}: {public_error}. Install the GitHub CLI (gh auth login), '
                             'pass --from FILE, or rebuild with `build`.') from public_error
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(['gh', 'release', 'download', RELEASE_TAG, '-R', RELEASE_REPO, '-p', MODEL_NAME, '-D', tmp],
                       check=True, stdin=subprocess.DEVNULL, timeout=600)
        got = Path(tmp) / MODEL_NAME
        if not valid(got):
            raise SystemExit(f'{RELEASE_TAG}: {MODEL_NAME} differs from its pin')
        shutil.copyfile(got, dest)
    return dest


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='command', required=True)
    p = sub.add_parser('fetch', help='the pinned int8 weights (release asset or a local copy)')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--from', dest='source', help='an already downloaded or rebuilt copy (file or directory)')
    p = sub.add_parser('prune', help='steps 1-2 only (for experiments)')
    p.add_argument('--work', type=Path, required=True)
    p = sub.add_parser('fix-param', help='step 3 rewrite of a pnnx param (for experiments)')
    p.add_argument('src', type=Path)
    p.add_argument('dst', type=Path)
    p = sub.add_parser('param-bin', help='binary param for host experiments (vjo-ocr --ppocr-param)')
    p.add_argument('param', type=Path)
    p.add_argument('--ncnn-source', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p = sub.add_parser('build', help='rebuild the int8 weights from the pinned upstream inputs')
    p.add_argument('--work', type=Path, required=True)
    p.add_argument('--realdata', type=Path, help="screenshots as shots/<kind>/<vn>_<id>.jpg (else downloaded)")
    p.add_argument('--output', type=Path)
    p = sub.add_parser('header', help='the build header (CMake)')
    p.add_argument('--ncnn-source', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--param', type=Path, default=DATA_DIR / PARAM_NAME)
    p.add_argument('--dict', type=Path, default=DATA_DIR / DICT_NAME)
    args = ap.parse_args()
    if args.command == 'fetch':
        path = fetch(args.output, args.source)
        print('Verified', path.name, f'({WEIGHTS[1]} bytes, sha256 {WEIGHTS[2]})')
        print('Model ready in', args.output, '- copy', path.name, 'to ux0:data/VitaJPOverlay/ocr/')
    elif args.command == 'prune':
        onnx_path = download(ONNX[0], args.work / 'dl' / 'PP-OCRv6_rec_small.onnx', ONNX[2], ONNX[1])
        classes, kept = prune(onnx_path, args.work / 'v6s_jis.onnx', args.work / 'v6s_jis_dict.txt')
        print(f'classes {classes} -> {kept}')
    elif args.command == 'build':
        build(args.work.resolve(), args.realdata / 'shots' if args.realdata else None, args.output)
    elif args.command == 'fix-param':
        args.dst.write_text(fix_param(args.src.read_text()))
    elif args.command == 'param-bin':
        data, input_id, output_id = param_bin(args.param.read_text(), args.ncnn_source)
        args.output.write_bytes(data)
        print(f'{args.output}: {len(data)} bytes, input {input_id}, output {output_id}')
    elif args.command == 'header':
        text = header(args.param, args.dict, args.ncnn_source, WEIGHTS[1], WEIGHTS[2])
        args.output.parent.mkdir(parents=True, exist_ok=True)
        tmp = args.output.with_name(args.output.name + '.tmp')
        tmp.write_text(text, encoding='utf-8')
        tmp.replace(args.output)


if __name__ == '__main__':
    main()
