#!/usr/bin/env bash
set -euo pipefail
PACKAGE=$(cd "$(dirname "$0")" && pwd)
if [ $# -ne 1 ]; then
  echo "Usage: ./install-vita.sh VITA_IP[:1337]"
  exit 2
fi
python3 "$PACKAGE/tools/package_release.py" --verify "$PACKAGE"
exec bash "$PACKAGE/tools/install_ftp.sh" --package-dir "$PACKAGE" \
  --set dictionary=local --set local_dictionaries=jmdict.vjdict \
  --set local_dictionary_dir=ux0:data/VitaJPOverlay/dictionaries \
  --set text_source=ocr --set ocr_backend=vocr --set vocr_model=H15_w8.vocr \
  --set vocr_fallback_model=FL10_w8.vocr \
  --set ocr_model_dir=ux0:data/VitaJPOverlay/ocr --set ocr_mode=auto --set log_file=on "$1"
