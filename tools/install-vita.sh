#!/usr/bin/env bash
# Installs this release over VitaShell's FTP. Name each game that should have
# OCR with --title (repeatable): the OCR worker is registered for those games
# and the games registered before, never under *ALL. For Adrenaline's PSP
# games add PSPEMUCFW and each bubble's own ID (README 'Install'). --check-app only checks,
# read-only, that the Vita's Overlay app is this package's (the install ends with
# the same check).
set -euo pipefail
PACKAGE=$(cd "$(dirname "$0")" && pwd)
TITLES=()
HOST=
HOSTS=0
USAGE=0
CHECK_APP=0
while [ $# -gt 0 ]; do
  case $1 in
    --title)
      if [ $# -lt 2 ]; then USAGE=1; break; fi
      TITLES+=(--title "$2"); shift 2 ;;
    --check-app) CHECK_APP=1; shift ;;
    -*) USAGE=1; break ;;
    *) HOST=$1; HOSTS=$((HOSTS + 1)); shift ;;
  esac
done
if [ $USAGE = 1 ] || [ $HOSTS -ne 1 ]; then
  echo "Usage: ./install-vita.sh [--title TITLEID]... VITA_IP[:1337]"
  echo "       ./install-vita.sh --check-app VITA_IP[:1337]"
  echo "  e.g. ./install-vita.sh --title PCSG00415 192.168.1.50"
  exit 2
fi
if [ $CHECK_APP = 1 ]; then
  exec bash "$PACKAGE/tools/install_ftp.sh" --check-app "$HOST"
fi
python3 "$PACKAGE/tools/package_release.py" --verify "$PACKAGE"
# The package's own settings (package_release.py --set, e.g. a test build's anki_mining=on),
# applied after the defaults below; listed so the change to config.ini is never a surprise.
# Each must be a setting of the package's config template with a one-line value. A credential
# (an *api_key, anki_audio_url) is only "set" or "" in the manifest: a set one's value is the
# package's config.ini's, which --verify has just checked; it is shown only as set or empty.
PACKAGE_SETS=()
SHOWN=
while IFS=$'\t' read -r setting shown; do
  [ -n "$setting" ] || continue
  PACKAGE_SETS+=(--set "$setting")
  SHOWN="$SHOWN $shown"
done < <(python3 -c '
import json, re, sys
text = open(sys.argv[2], encoding="utf-8-sig", errors="replace").read()
keys = {m.group(1) for m in re.finditer(r"^(\w+)\s*=", text, re.M)}
values = {}
for m in re.finditer(r"^(\w+)[ \t]*=[ \t]*(.*?)[ \t\r]*$", text, re.M):
    values.setdefault(m.group(1), m.group(2))
secret = re.compile(r"api_key$|^anki_audio_url$")
def one_line(v):
    return len(("<" + v + ">").splitlines()) == 1 and all(ord(c) >= 32 and c != "\x7f" for c in v)
for k, v in json.load(open(sys.argv[1], encoding="utf-8")).get("install_settings", {}).items():
    if not (k in keys and re.fullmatch(r"[a-z][a-z0-9_]*", k) and isinstance(v, str)):
        continue
    if secret.search(k) and v == "set":
        v = values.get(k, "")
        if not v:
            continue
    if one_line(v):
        print(k + "=" + v + "\t" + k + "=" + ((("set" if v else "empty") if secret.search(k) else v)))
' "$PACKAGE/package-manifest.json" "$PACKAGE/config.ini")
[ -z "$SHOWN" ] || echo "package settings:$SHOWN"
# No model and no model size is set: the Overlay app's Model size (vocr_tier) is the user's, and
# an update keeps it; a first install gets the template's Normal, and an older build's model
# choice becomes its size (tools/migrate_config.py).
exec bash "$PACKAGE/tools/install_ftp.sh" --package-dir "$PACKAGE" ${TITLES[@]+"${TITLES[@]}"} \
  --set dictionary=local --set local_dictionaries=plaintext.vjdict \
  --set local_dictionary_dir=ux0:data/VitaJPOverlay/dictionaries \
  --set text_source=ocr \
  --set ocr_model_dir=ux0:data/VitaJPOverlay/ocr --set ocr_mode=auto --set log_file=on \
  ${PACKAGE_SETS[@]+"${PACKAGE_SETS[@]}"} "$HOST"
