#!/usr/bin/env bash
# Installs this release over VitaShell's FTP. Name each game that should have
# OCR with --title (repeatable): the OCR worker is registered for those games
# and the games registered before, never under *ALL. For Adrenaline's PSP
# games add NPXS10028 and PSPEMUCFW (README 'Install').
set -euo pipefail
PACKAGE=$(cd "$(dirname "$0")" && pwd)
TITLES=()
HOST=
HOSTS=0
USAGE=0
while [ $# -gt 0 ]; do
  case $1 in
    --title)
      if [ $# -lt 2 ]; then USAGE=1; break; fi
      TITLES+=(--title "$2"); shift 2 ;;
    -*) USAGE=1; break ;;
    *) HOST=$1; HOSTS=$((HOSTS + 1)); shift ;;
  esac
done
if [ $USAGE = 1 ] || [ $HOSTS -ne 1 ]; then
  echo "Usage: ./install-vita.sh [--title TITLEID]... VITA_IP[:1337]"
  echo "  e.g. ./install-vita.sh --title PCSG00415 192.168.1.50"
  exit 2
fi
python3 "$PACKAGE/tools/package_release.py" --verify "$PACKAGE"
exec bash "$PACKAGE/tools/install_ftp.sh" --package-dir "$PACKAGE" ${TITLES[@]+"${TITLES[@]}"} \
  --set dictionary=local --set local_dictionaries=plaintext.vjdict \
  --set local_dictionary_dir=ux0:data/VitaJPOverlay/dictionaries \
  --set text_source=ocr --set ocr_backend=vocr --set vocr_model=H15_w8.vocr \
  --set vocr_fallback_model=FL10_w8.vocr \
  --set ocr_model_dir=ux0:data/VitaJPOverlay/ocr --set ocr_mode=auto --set log_file=on "$HOST"
