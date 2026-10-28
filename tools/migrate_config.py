#!/usr/bin/env python3
"""Rebuilds config.ini from the template, keeping the values of an existing
file and applying key=value overrides.

    migrate_config.py OLD TEMPLATE OUT [key=value ...]

OLD may be missing (first install). Removed settings are dropped (the API keys of
jpdb.io and jiten.moe, the removed online dictionaries, among them: printed as removed,
never their values; the removed Hachidori relay's hachidori_host too; the engine choice,
ocr_backend: VOCR is the only engine; and log_host, the removed UDP log's computer), and
values of removed features (dictionary = jpdb,
jiten or hachidori) are replaced. An older build's model choice (vocr_model) becomes its
model size (vocr_tier) unless the file already names a valid one; vocr_model and
vocr_fallback_model go. A vocr_tier core/config.c cannot read is replaced by the size the
plugin would run (the old model's, else the template's), as the plugin ignores it. Prints
what changed (a credential, the audio source URL, only as set/empty). Exit status 0 = OUT
should be uploaded, 1 = OLD is already identical, 2 = migration failed.
"""
import os
import re
import sys

RENAMED = {}
# config.ini's removed settings: a copy of core/config.c's one list (removed_keys, core/config.h
# vjo_config_removed), which tools/tests/test_migrate_config.py checks it against. REMOVED: the names of every kind but
# the API keys' and the field mapping's prefix and the dictionary choice, dropped and reported by name.
REMOVED = {"frequency_filter", "font_size", "hw_jpeg", "combo_delay_ms",
           # Anki's field mapping, which one fixed note type replaced; with every anki_field_* key
           # (REMOVED_PREFIXES), as core/config.c ignores them.
           "anki_deck", "anki_note_type", "anki_tags",
           # AnkiConnect's address and the word audio source: AnkiConnect was removed, the
           # Rubi app exports the cards as an .apkg file instead (docs/anki.md).
           "anki_host", "anki_audio_url",
           # API 21: the model and its fallback became one model size (vocr_tier, MODEL_TIER below).
           "vocr_model", "vocr_fallback_model",
           # The Hachidori relay's computer (dictionary = hachidori, removed): core/config.c ignores it.
           "hachidori_host",
           # The engine choice: VOCR is the only engine since Google Lens was removed (the overlay
           # is offline); core/config.c ignores a vocr line and warns for a removed engine's.
           "ocr_backend",
           # The UDP log's computer: the log went with the network (the overlay is offline);
           # core/config.c warns once for a set one. log.txt stays.
           "log_host"}
REMOVED_PREFIXES = ("anki_field_",)
# The removed online dictionaries' API keys (secrets; api_key: jpdb_api_key's older name): a key
# that starts with one is dropped, and reported by that name only, as its line's text may hold the
# key (core/config.c's list, VJO_REMOVED_SECRET; no setting's name starts with one of them).
SECRETS = ("jpdb_api_key", "jiten_api_key", "api_key")
# Why a removed setting went, printed beside it (the log line the installer shows).
REMOVED_WHY = {"anki_host": "AnkiConnect was removed: the Rubi app exports the cards as an .apkg",
               "anki_audio_url": "AnkiConnect was removed: the Rubi app exports the cards as an .apkg",
               "log_host": "the UDP log was removed: the overlay is offline; log.txt stays"}
# Values whose feature was removed, with their replacement. dictionary offers local only;
# jpdb, jiten and hachidori load as local (core/config.c removed_dictionary).
REPLACED_VALUES = {"dictionary": {"jpdb": "local", "jiten": "local", "hachidori": "local"}}
# An older build's vocr_model, by the shape its file name carries (core/config.c vjo_vocr_tier_of_model):
# the FL10 family Small, H15 Normal, F20 and W20 Large; any other name keeps the template's size.
MODEL_TIER = (("FL10", "small"), ("H15", "normal"), ("W20", "large"), ("F20", "large"))
# What core/config.c reads: vjo_vocr_tier_parse's ids (any case), and valid_vocr_model's names
# (6-63 characters of [0-9A-Za-z._-], ending in .vocr).
TIERS = ("small", "normal", "large")
VALID_MODEL = re.compile(r"[0-9A-Za-z._-]{1,58}\.vocr")
# The bundled dictionaries: Bee's Ultimate Dictionary, then the PSP/Vita names.
BUNDLED = ("plaintext.vjdict", "vndb-names.vjdict")


def valid_tier(value):
    """Whether core/config.c reads value as a model size."""
    return value.lower() in TIERS


def model_tier(name):
    """The vocr_tier of an older vocr_model value core/config.c accepts, or None."""
    if not VALID_MODEL.fullmatch(name):
        return None
    return next((tier for shape, tier in MODEL_TIER if shape in name), None)


def bundled_dictionaries(value):
    """local_dictionaries with both bundled files, as the Rubi app's update writes it (settings/install.c
    dictionaries): 0.7.12's jmdict.vjdict becomes plaintext.vjdict, else plaintext comes first; the names
    come after the user's own. None when both are there or the list would hold more than eight."""
    names = [n.strip() for n in value.split(",") if n.strip()]
    if BUNDLED[0] in names and BUNDLED[1] in names:
        return None
    if BUNDLED[0] not in names:
        if "jmdict.vjdict" in names:
            names[names.index("jmdict.vjdict")] = BUNDLED[0]
        else:
            names.insert(0, BUNDLED[0])
    if BUNDLED[1] not in names:
        names.append(BUNDLED[1])
    return ",".join(names) if len(names) <= 8 else None


def secret(k):
    """The removed API key's name a key starts with, or None."""
    return next((name for name in SECRETS if k.startswith(name)), None)


def removed(k):
    return k in REMOVED or k.startswith(REMOVED_PREFIXES) or secret(k) is not None


def name(k):
    """A key as the report names it: a removed API key's line by the key's name only."""
    return secret(k) or k


def parse(text):
    vals = {}
    for line in text.splitlines():
        # a line's leading spaces and UTF-8 BOMs (a hand-joined file's, mid-file) are no part of its name, as for
        # core/config.c vjo_config_line_start: a BOM-led copy of a setting is that setting (audit-pr185 B2)
        line = re.sub(r"^[\s\ufeff]+", "", line).rstrip()
        if not line or line[0] in ";#" or "=" not in line:
            continue
        k, v = line.split("=", 1)
        k = k.strip().lower()
        if not k.startswith("anki_"):  # anki_ values may contain ; and #, as in core/config.c
            v = re.split(r"\s[;#]", v, maxsplit=1)[0]  # inline comment
        vals[k] = v.strip()
    return vals


def show(k, v):
    """A value as the report prints it: a credential (the audio source's URL, which can carry one;
    an API key, though none survives: REMOVED) only as set or empty (package_release.py SECRET)."""
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
        if not removed(k):
            vals[k] = REPLACED_VALUES.get(k, {}).get(v.lower(), v)
    # An update enables the bundled names after the user's dictionaries, as the Rubi app's does. Newly
    # enabled, a count of 1 (the default before the names) becomes 2, so a word's name is a page. A --set
    # below still wins.
    if old and "local_dictionaries" in vals:
        listed = bundled_dictionaries(vals["local_dictionaries"])
        if listed:
            vals["local_dictionaries"] = listed
            if vals.get("local_dictionary_results") == "1":
                vals["local_dictionary_results"] = "2"
    # An older build's model choice is its model size, unless the file already names a valid one.
    # core/config.c ignores a vocr_tier it cannot read (with a warning): so does this, in either
    # order of the two lines, so the installed file says the size the plugin would run.
    old_tier = (old or {}).get("vocr_tier")
    if old_tier is not None and not valid_tier(old_tier):
        del vals["vocr_tier"]  # the template's size, unless the old model's below
        old_tier = None
    if old and old_tier is None and model_tier(old.get("vocr_model", "")):
        vals["vocr_tier"] = model_tier(old["vocr_model"])
    explicit = set()
    set_tier = any(o.partition("=")[0].strip().lower() == "vocr_tier" and valid_tier(o.partition("=")[2].strip())
                   for o in sets)
    for s in sets:
        k, _, v = s.partition("=")
        k, v = k.strip().lower(), v.strip()
        if k == "vocr_model":  # an older installer's model choice: its size (a valid vocr_tier set wins)
            if model_tier(v) and not set_tier:
                vals["vocr_tier"] = model_tier(v)
                explicit.add("vocr_tier")
            continue
        if k == "vocr_tier" and not valid_tier(v):  # the plugin would ignore it: not written
            print("config.ini: vocr_tier %s is not a model size (small, normal or large); not set" % (v or "empty"))
            continue
        if k == "vocr_fallback_model":
            continue
        if removed(k):  # a removed setting (an API key: its value is never printed)
            print("config.ini: %s is no longer a setting, not set" % name(k))
            continue
        vals[k] = REPLACED_VALUES.get(k, {}).get(v.lower(), v)
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
    report = []
    for k in sorted(set(old or {}) | set(new)):
        if old is not None and k in old and k not in new:
            report.append("config.ini: %s removed%s" % (name(k), " (%s)" % REMOVED_WHY[k] if k in REMOVED_WHY else ""))
        elif old is None or old.get(k) != new.get(k):
            report.append("config.ini: %s = %s" % (name(k), show(k, new.get(k, ""))))
    for k in sorted(set(vals) - used):
        report.append("config.ini: unknown setting %s dropped" % name(k))
    for line in dict.fromkeys(report):  # once each (several lines can name one removed key)
        print(line)
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
