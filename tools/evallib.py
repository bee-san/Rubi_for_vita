"""What the host eval and bench scripts share (tools/*_eval.py, bench_stages.py, capture_parity.py,
collect_word_lookup_lines.py, vocr_regions_pack.py): the protected sets they never read, the development
manifests, a screenshot found in --shots, edit distance, the UTF-16 length, and the overlay's LM decoder
settings."""
from pathlib import Path

# vita-vn-ocr's linefind development regions (eval/linefind/manifests): the only manifests these scripts read
DEV_MANIFESTS = ("vita_dev.jsonl", "psp_dev.jsonl")
# Never for scoring, tuning or a test (vita-vn-ocr eval/README): by set name and id, and CLANNAD by its VN
PROTECTED_SETS = {"vita-test", "psp-test", "vita_test", "psp_test", "native", "fresh-native", "fresh_native", "clannad"}
PROTECTED_VNS = {"v4"}  # CLANNAD


def protected_set(row):
    """A manifest row of a protected set, by its set name or id."""
    text = " ".join(str(row.get(k, "")) for k in ("set", "id")).lower()
    return str(row.get("set", "")).lower() in PROTECTED_SETS or "clannad" in text or "native" in text


def protected(row):
    """A protected set's row, or CLANNAD's by its VN."""
    return protected_set(row) or row.get("vn") in PROTECTED_VNS


def find_shot(name, dirs):
    """The screenshot `name` in the first of dirs that has it, or None."""
    return next((Path(d) / name for d in dirs if (Path(d) / name).exists()), None)


def lev(a, b):
    """The Levenshtein distance between two sequences."""
    if a == b:
        return 0
    if len(a) < len(b):
        a, b = b, a
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]


def utf16_len(text):
    """The length in UTF-16 code units, as the overlay counts it (a character above U+FFFF is two)."""
    return sum(2 if ord(c) > 0xFFFF else 1 for c in text)


# The game worker's beam + LM decode (core/vocr_ocr.c lm_opts(); tools/tests/test_vocr_defaults.py checks these
# against its tables): (alpha, beta, flags) per LM, core/vocr_ocr.c lms[] (VOCR6's two tiers and ctclm.2's two), and
# ctclm.2's own 1000k / 4000k files with their settings for the earlier measurements
LM_BOS, LM_EOS = 1, 2
LM_SETTINGS = {"vocr_lm_o3_500k.vlm": (500, 3000, LM_EOS), "vocr_lm6_o3_1000k.vlm": (600, 2500, LM_BOS),
               "vocr_lm_o3_2000k.vlm": (500, 3000, LM_EOS), "vocr_lm6_o3_4000k.vlm": (600, 2500, LM_BOS),
               "vocr_lm_o3_1000k.vlm": (500, 3500, LM_EOS), "vocr_lm_o3_4000k.vlm": (500, 3500, LM_EOS)}
# a job model's own settings for an LM, where they differ from its tier's (core/vocr_ocr.c lm_retuned: vita-vn-ocr
# #151, VOCR6-N with the 2000k LM, measured on v0.4.0-vocr6.1's and kept for v0.4.1-vocr6.2's); the worker's decoder
# and its cascade's take them
LM_MODEL_SETTINGS = {("VOCR6.2_N_w8g2.vocr", "vocr_lm_o3_2000k.vlm"): (400, 1500, LM_BOS | LM_EOS)}


def lm_settings(model, lm):
    """(alpha, beta, flags) a job on `model` decodes `lm` with (core/vocr_ocr.c vjo_vocr_lm_settings)."""
    return LM_MODEL_SETTINGS.get((model, lm), LM_SETTINGS[lm])
