#!/usr/bin/env python3
"""Rebuilds config.ini from the template, keeping the values of an existing
file and applying key=value overrides.

    migrate_config.py OLD TEMPLATE OUT [key=value ...]

OLD may be missing (first install). Renamed settings are carried over, removed
ones are dropped, and values of removed features (ocr_backend = vocr2, meiki or
ncnn) are replaced. An older build's model choice (vocr_model) becomes its model
size (vocr_tier) unless the file already has one; vocr_model and
vocr_fallback_model go. Prints what changed (credentials, an API key or the audio
source URL, only as set/empty). Exit
status 0 = OUT should be uploaded, 1 = OLD is already identical, 2 = migration failed.
"""
import os
import re
import sys

RENAMED = {"api_key": "jpdb_api_key"}
REMOVED = {"frequency_filter", "font_size", "hw_jpeg", "combo_delay_ms",
           # Anki's field mapping: the note is Kiku's, in the deck Kiku (docs/anki.md); with every
           # anki_field_* key (REMOVED_PREFIXES), as core/config.c ignores them.
           "anki_deck", "anki_note_type", "anki_tags",
           # API 21: the model and its fallback became one model size (vocr_tier, MODEL_TIER below).
           "vocr_model", "vocr_fallback_model"}
REMOVED_PREFIXES = ("anki_field_",)
# Values whose feature was removed, with their replacement. ocr_backend offers two engines,
# vocr and lens; the others load as vocr already (core/config.c removed_engine).
REPLACED_VALUES = {"ocr_backend": {"vocr2": "vocr", "meiki": "vocr", "ncnn": "vocr"}}
# An older build's vocr_model, by the shape its file name carries (core/config.c vjo_vocr_tier_of_model):
# the FL10 family Small, H15 Normal, F20 and W20 Large; any other name keeps the template's size.
MODEL_TIER = (("FL10", "small"), ("H15", "normal"), ("W20", "large"), ("F20", "large"))


def model_tier(name):
    """The vocr_tier of an older vocr_model value, or None."""
    return next((tier for shape, tier in MODEL_TIER if shape in name), None)


def parse(text):
    vals = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line[0] in ";#" or "=" not in line:
            continue
        k, v = line.split("=", 1)
        k = k.strip().lower()
        if not k.startswith("anki_"):  # anki_ values may contain ; and #, as in core/config.c
            v = re.split(r"\s[;#]", v, maxsplit=1)[0]  # inline comment
        vals[k] = v.strip()
    return vals


def show(k, v):
    """A value as the report prints it: a credential (an API key, or the audio source's URL, which can
    carry one) only as set or empty (package_release.py SECRET)."""
    if re.search(r"api_key$|^anki_audio_url$", k):
        return "set" if v else "empty"
    return v or "empty"


def main():
    old_path, tmpl_path, out_path, *sets = sys.argv[1:]
    old_text = open(old_path, encoding="utf-8-sig").read() if os.path.exists(old_path) else None
    old = parse(old_text) if old_text is not None else None

    vals = {}
    for k, v in (old or {}).items():
        k = RENAMED.get(k, k)
        if k not in REMOVED and not k.startswith(REMOVED_PREFIXES):
            vals[k] = REPLACED_VALUES.get(k, {}).get(v.lower(), v)
    # An older build's model choice is its model size, unless the file already names one.
    if old and "vocr_tier" not in old and model_tier(old.get("vocr_model", "")):
        vals["vocr_tier"] = model_tier(old["vocr_model"])
    explicit = set()
    for s in sets:
        k, _, v = s.partition("=")
        k, v = k.strip().lower(), v.strip()
        if k == "vocr_model":  # an older installer's model choice: its size (a vocr_tier set wins)
            if model_tier(v) and not any(o.partition("=")[0].strip().lower() == "vocr_tier" for o in sets):
                vals["vocr_tier"] = model_tier(v)
                explicit.add("vocr_tier")
            continue
        if k == "vocr_fallback_model":
            continue
        vals[k] = v
        explicit.add(k)

    out, used = [], set()
    for line in open(tmpl_path, encoding="utf-8").read().splitlines():
        m = re.match(r"^(\w+)\s*=", line)
        if m and m.group(1) in vals:
            line = ("%s = %s" % (m.group(1), vals[m.group(1)])).rstrip()
            used.add(m.group(1))
        out.append(line)
    # Settings outside the template (e.g. debugging switches) survive only
    # while they are set explicitly with --set or already present.
    extra = [k for k in vals if k not in used and (k in explicit or k in (old or {}))]
    if extra:
        out.append("")
        out.extend(("%s = %s" % (k, vals[k])).rstrip() for k in extra)
        used.update(extra)
    text = "\n".join(out) + "\n"
    open(out_path, "w", encoding="utf-8").write(text)

    new = parse(text)
    for k in sorted(set(old or {}) | set(new)):
        if old is not None and k in old and k not in new:
            print("config.ini: %s removed" % k)
        elif old is None or old.get(k) != new.get(k):
            print("config.ini: %s = %s" % (k, show(k, new.get(k, ""))))
    for k in sorted(set(vals) - used):
        print("config.ini: unknown setting %s dropped" % k)
    return 0 if old_text is None or old_text != text else 1


if __name__ == "__main__":
    try:
        status = main()
    except Exception as error:
        # Exit 1 is reserved for an unchanged config. Do not print file contents
        # or exception values: the existing config can contain API keys.
        print("config.ini migration failed (" + type(error).__name__ + ")", file=sys.stderr)
        status = 2
    sys.exit(status)
