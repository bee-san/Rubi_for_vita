#!/bin/bash
# Installs Vita JP Overlay over VitaShell's FTP server (VitaShell: SELECT ->
# FTP). Backs up the taiHEN configs first (locally and as config.txt.vjo-bak on
# the Vita) and adds plugin entries once. config.ini is created on the
# first install; later installs keep its values and bring it up to the current
# set of settings. Local copies (backups, logs, dumps) go to build/device/.
# Needs bash, curl and python3.
#
# The game OCR worker is registered per game, one *TITLEID section each, never
# under *ALL (which would load it into SceShell and every app as well): name
# each game with --title. Games registered by an earlier install are kept, and
# an *ALL worker line is commented out. With no --title and no game registered
# yet, nothing is changed.
#
#   tools/install_ftp.sh --title PCSG00415 192.168.1.50[:1337]
#   tools/install_ftp.sh --title PCSG00415 --title NPXS10028 --title PSPEMUCFW 192.168.1.50
#     (NPXS10028 and PSPEMUCFW: Adrenaline's PSP games, README 'Install')
#   tools/install_ftp.sh --set dictionary=jiten --set jiten_api_key=KEY 192.168.1.50
#   tools/install_ftp.sh --add-kernel-plugin NoPowerLimits.skprx 192.168.1.50
#     (also uploads another kernel plugin to ur0:tai/ and adds it under *KERNEL;
#      --uninstall leaves such plugins in place)
#   tools/install_ftp.sh --vocr-model ocr/H15_w8.vocr --set ocr_backend=vocr 192.168.1.50
#     (also uploads vita-vn-ocr weights to ux0:data/VitaJPOverlay/ocr/, for a
#      build made with VJO_WITH_VOCR; see docs/vita-vn-ocr.md)
#   tools/install_ftp.sh --uninstall 192.168.1.50
#   tools/install_ftp.sh --purge 192.168.1.50     (uninstall, then delete the
#     plugin files and both VitaJPOverlay data folders; config.ini and
#     region.ini are copied to the local backup first)
#   tools/install_ftp.sh --status 192.168.1.50   (prints the plugin logs and the self-test report)
#   tools/install_ftp.sh --self-test 192.168.1.50 (arms the on-device self-test for the next
#      boot and game start; docs/self-test.md)
#   tools/install_ftp.sh --dumps 192.168.1.50    (downloads system crash dumps)
set -euo pipefail
cd "$(dirname "$0")/.."

UNINSTALL=0
PURGE=0
STATUS=0
DUMPS=0
SELFTEST=0
SETS=()
EXTRA_K=()
VOCR_MODELS=()
TITLES=()
PACKAGE_DIR=
while [ $# -gt 1 ]; do
  case $1 in
    --uninstall) UNINSTALL=1 ;;
    --purge) UNINSTALL=1; PURGE=1 ;;
    --dumps) DUMPS=1 ;;
    --status) STATUS=1 ;;
    --self-test) SELFTEST=1 ;;
    --set) SETS+=("$2"); shift ;;
    --title) TITLES+=("$2"); shift ;;
    --add-kernel-plugin) EXTRA_K+=("$2"); shift ;;
    --vocr-model) VOCR_MODELS+=("$2"); shift ;;
    --package-dir) PACKAGE_DIR=$(cd "$2" && pwd); shift ;;
    *) break ;;
  esac
  shift
done
HOST=${1:?usage: tools/install_ftp.sh [--title TITLEID]... [--set key=value]... [--uninstall|--status|--dumps|--self-test] VITA_IP[:PORT]}
[[ $HOST == *:* ]] || HOST=$HOST:1337
# How to install from here: the release package has install-vita.sh at its root.
INSTALL_CMD="bash tools/install_ftp.sh"
[ -f install-vita.sh ] && INSTALL_CMD="bash install-vita.sh"
if [ -n "${TITLES[*]+x}" ] && [ $((UNINSTALL + STATUS + DUMPS + SELFTEST)) -gt 0 ]; then
  echo "Note: --title takes effect when installing ($INSTALL_CMD --title ${TITLES[0]} ${HOST%%:*});"
  echo "this command does not change which games the OCR worker is registered for."
fi
FTP=ftp://$HOST
B=build/vita
RCO=$B/vitajpoverlay.rco
CONFIG_TEMPLATE=docs/config.example.ini
if [ -n "$PACKAGE_DIR" ]; then
  python3 tools/package_release.py --verify "$PACKAGE_DIR"
  B=$PACKAGE_DIR/ur0/tai
  RCO=$PACKAGE_DIR/ur0/data/VitaJPOverlay/vitajpoverlay.rco
  CONFIG_TEMPLATE=$PACKAGE_DIR/config.ini
  for model in "$PACKAGE_DIR"/ux0/data/VitaJPOverlay/ocr/*.vocr; do
    [ -f "$model" ] && VOCR_MODELS+=("$model")
  done
fi
PATCH_ARGS=()
for title in ${TITLES[@]+"${TITLES[@]}"}; do PATCH_ARGS+=(--title "$title"); done
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

c() { curl --fail -s --show-error --max-time 300 "$@"; }
exists() { c -o /dev/null "$FTP/$1" 2>/dev/null; }
get() { c -o "$2" "$FTP/$1"; }
put() {
  c --ftp-create-dirs -T "$1" "$FTP/$2"
  get "$2" "$TMP/readback"
  python3 - "$1" "$TMP/readback" "$2" <<'PY'
import hashlib, sys
def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.digest()
if digest(sys.argv[1]) != digest(sys.argv[2]):
    raise SystemExit("FTP readback checksum failed: " + sys.argv[3])
print("verified " + sys.argv[3])
PY
}

# Resets the boot guard's record (docs/boot-safety.md): an empty boot.txt is
# "no record" to the kernel plugin, so the next boot is a fresh normal one.
reset_boot_record() {
  : > "$TMP/boot.empty"
  put "$TMP/boot.empty" "ur0:/data/VitaJPOverlay/boot.txt"
}

python3 -c 'import socket, sys; socket.create_connection((sys.argv[1], int(sys.argv[2])), 5).close()' \
    "${HOST%%:*}" "${HOST##*:}" 2>/dev/null || {
  echo "Cannot reach $HOST. In VitaShell press SELECT to start FTP, and keep the screen on."; exit 1; }

if [ $DUMPS = 1 ]; then
  # Crash dumps written by the system (psp2core-*.psp2dmp in ux0:data).
  mkdir -p build/device/dumps
  # VitaShell's FTP has no NLST and curl's directory handling trips it up:
  # list with a plain LIST via ftplib (the name is the last field).
  list=$(python3 - "${HOST%%:*}" "${HOST##*:}" <<'PY'
import ftplib, sys
f = ftplib.FTP()
f.connect(sys.argv[1], int(sys.argv[2]), timeout=20)
f.login()
lines = []
for path in ("/ux0:/data", "ux0:/data/", "/ux0:/data/"):
    try:
        f.cwd(path)
        f.retrlines("LIST", lines.append)
        break
    except ftplib.all_errors:
        lines = []
for l in lines:
    name = l.split()[-1] if l.split() else ""
    if name.lower().endswith(".psp2dmp"):
        print(name)
PY
)
  [ -n "$list" ] || { echo "no crash dumps in ux0:data"; exit 0; }
  for f in $list; do get "ux0:/data/$f" "build/device/dumps/$f" && echo "downloaded build/device/dumps/$f"; done
  exit 0
fi

if [ $SELFTEST = 1 ]; then
  # One empty file arms the self-test (shell/selftest.c): the next boot and
  # the first game started write ux0:data/VitaJPOverlay/selftest-report.txt.
  : > "$TMP/selftest.txt"
  put "$TMP/selftest.txt" "ux0:/data/VitaJPOverlay/selftest.txt"
  echo "Self-test armed. Reboot the Vita, wait for the LiveArea, leave it for a minute, start the game to test,"
  echo "reach a text screen and wait 30 s. Then start VitaShell's FTP again and run: tools/install_ftp.sh --status ${HOST%%:*}"
  echo "Analyse build/device/status/selftest-report.txt with: python3 tools/analyse_selftest.py build/device/status/selftest-report.txt"
  exit 0
fi

if [ $STATUS = 1 ]; then
  mkdir -p build/device/status
  echo "=== selftest-report.txt"
  get "ux0:/data/VitaJPOverlay/selftest-report.txt" "build/device/status/selftest-report.txt" 2>/dev/null \
    && cat "build/device/status/selftest-report.txt" || echo "(none)"
  # The boot guard's record (docs/boot-safety.md) lives on ur0, which is
  # readable before ux0 is mounted.
  echo "=== boot.txt (ur0)"
  get "ur0:/data/VitaJPOverlay/boot.txt" "build/device/status/boot.txt" 2>/dev/null \
    && cat "build/device/status/boot.txt" || echo "(none)"
  for f in kernel.prev kernel status.prev status; do
    echo "=== $f.txt"
    get "ux0:/data/VitaJPOverlay/$f.txt" "build/device/status/$f.txt" 2>/dev/null \
      && cat "build/device/status/$f.txt" || echo "(none)"
  done
  exit 0
fi

# Ensō + SD2Vita: taiHEN reads ur0:tai/config.txt at boot (ux0 is not the SD
# card yet), so ur0 is always patched. ux0:tai/config.txt, if present, is
# patched too, since taiHEN prefers it when ux0 is available at boot.
CFGS=(ur0:/tai/config.txt)
exists "ux0:/tai/config.txt" && CFGS+=(ux0:/tai/config.txt)
mkdir -p build/device/backup
BK=$(mktemp -d "build/device/backup/$(date +%Y%m%d-%H%M%S)-XXXXXX")
for CFG in "${CFGS[@]}"; do
  n=${CFG%%:*}
  get "$CFG" "$TMP/$n.txt" || { echo "cannot read $CFG"; exit 1; }
  cp "$TMP/$n.txt" "$BK/$n-config.txt"
done
echo "backup: $BK"

if [ $UNINSTALL = 1 ]; then
  for CFG in "${CFGS[@]}"; do
    n=${CFG%%:*}
    python3 tools/patch_tai_config.py "$TMP/$n.txt" "$TMP/$n.new" --uninstall
    put "$TMP/$n.new" "$CFG"
    echo "removed the plugin lines from $CFG"
  done
  reset_boot_record
  if [ $PURGE = 0 ]; then
    echo "Reboot the Vita (plugin files are left in place)."
    exit 0
  fi
  for f in config.ini region.ini; do
    get "ux0:/data/VitaJPOverlay/$f" "$BK/$f" 2>/dev/null && echo "saved $BK/$f" || true
  done
  # VitaShell's FTP takes absolute paths as /ur0:/...; RMD needs an empty
  # directory, so each data folder's files go first.
  python3 - "${HOST%%:*}" "${HOST##*:}" <<'PY'
import ftplib, sys
f = ftplib.FTP()
f.connect(sys.argv[1], int(sys.argv[2]), timeout=20)
f.login()
def rm(path):
    # VitaShell confirms DELE with a code ftplib does not expect (error_reply)
    try:
        f.delete(path)
    except ftplib.error_reply:
        pass
    except ftplib.all_errors:
        return
    print("deleted", path)
for name in ("VitaJPOverlay_Kernel.skprx", "VitaJPOverlay_Shell.suprx", "VitaJPOverlay_Text.suprx", "VitaJPOverlay_OCR.suprx", "config.txt.vjo-bak"):
    rm("/ur0:/tai/" + name)
rm("/ux0:/tai/config.txt.vjo-bak")
for d in ("/ur0:/data/VitaJPOverlay", "/ux0:/data/VitaJPOverlay"):
    lines = []
    try:
        f.cwd(d)
        f.retrlines("LIST", lines.append)
    except ftplib.all_errors:
        continue
    for l in lines:
        if l.split() and not l.startswith("d"):
            rm(d + "/" + l.split()[-1])
    f.cwd("/")
    try:
        f.rmd(d)
        print("deleted", d)
    except ftplib.all_errors as e:
        print("could not delete", d, "-", e)
f.quit()
PY
  echo "Reboot the Vita."
  exit 0
fi

if [ ! -f "$CONFIG_TEMPLATE" ]; then
  echo "missing configuration template: $CONFIG_TEMPLATE"; exit 1
fi
for f in "$B/VitaJPOverlay_Kernel.skprx" "$B/VitaJPOverlay_Shell.suprx" "$B/VitaJPOverlay_OCR.suprx" "$RCO" \
         ${EXTRA_K[@]+"${EXTRA_K[@]}"}; do
  [ -f "$f" ] || { echo "missing $f: run cmake --build build/vita first"; exit 1; }
done
for f in ${EXTRA_K[@]+"${EXTRA_K[@]}"}; do
  [ "$(head -c 3 "$f")" = "SCE" ] || { echo "$f is not a Vita plugin (no SCE header)"; exit 1; }
  PATCH_ARGS+=(--kernel-plugin "ur0:tai/$(basename "$f")")
done

# Patch every config before anything is written to the Vita (CRLF-safe): the
# kernel line after *KERNEL, the shell after *main, and the OCR worker under
# *TITLEID for each --title and for each game already registered in either
# config, so both list the same games. Never *ALL: an *ALL worker line is
# commented out. With no game at all the patch is refused.
KEEP=()
for CFG in "${CFGS[@]}"; do KEEP+=(--keep-titles-from "$TMP/${CFG%%:*}.txt"); done
SUMMARY=()
for CFG in "${CFGS[@]}"; do
  n=${CFG%%:*}
  patch_status=0
  summary=$(python3 tools/patch_tai_config.py "$TMP/$n.txt" "$TMP/$n.new" ${PATCH_ARGS[@]+"${PATCH_ARGS[@]}"} \
    "${KEEP[@]}") || patch_status=$?
  if [ "$patch_status" -ne 0 ]; then
    if [ "$patch_status" -eq 3 ]; then
      if [ -n "${TITLES[*]+x}" ]; then
        echo "Nothing was changed on the Vita. Correct the --title value named above and run again, for example:"
      else
        echo "Nothing was changed on the Vita. Run again with --title for each game that should have OCR, for example:"
      fi
      echo "  $INSTALL_CMD --title PCSG00415 ${HOST%%:*}"
      echo "JP Overlay Settings shows each game's title ID; VitaShell shows them as the folder names in ux0:app (gro0:app for a game card)."
    fi
    exit "$patch_status"
  fi
  SUMMARY+=("$summary")
done
i=0
for CFG in "${CFGS[@]}"; do
  n=${CFG%%:*}
  echo "--- $CFG changes (${SUMMARY[$i]}):"; diff "$TMP/$n.txt" "$TMP/$n.new" || true
  i=$((i + 1))
  put "$TMP/$n.txt" "${CFG}.vjo-bak"
done

put "$B/VitaJPOverlay_Kernel.skprx" "ur0:/tai/VitaJPOverlay_Kernel.skprx"
# A new build starts with a clean boot record: the guard's safe mode was about
# the build being replaced.
reset_boot_record
put "$B/VitaJPOverlay_Shell.suprx" "ur0:/tai/VitaJPOverlay_Shell.suprx"
put "$B/VitaJPOverlay_OCR.suprx" "ur0:/tai/VitaJPOverlay_OCR.suprx"
put "$RCO" "ur0:/data/VitaJPOverlay/vitajpoverlay.rco"
for f in ${EXTRA_K[@]+"${EXTRA_K[@]}"}; do
  put "$f" "ur0:/tai/$(basename "$f")"
  echo "uploaded ur0:tai/$(basename "$f")"
done
if [ -n "$PACKAGE_DIR" ]; then
  for f in "$PACKAGE_DIR"/ux0/data/VitaJPOverlay/dictionaries/*.vjdict; do
    [ -f "$f" ] || continue
    put "$f" "ux0:/data/VitaJPOverlay/dictionaries/$(basename "$f")"
  done
  put "$PACKAGE_DIR/JPOverlaySettings.vpk" "ux0:/data/JPOverlaySettings.vpk"
  echo "In VitaShell install ux0:data/JPOverlaySettings.vpk to add or update the Overlay settings app."
fi
for f in ${VOCR_MODELS[@]+"${VOCR_MODELS[@]}"}; do
  # The plugin checks the file's size and SHA-256 before it parses it.
  put "$f" "ux0:/data/VitaJPOverlay/ocr/$(basename "$f")"
  echo "uploaded ux0:data/VitaJPOverlay/ocr/$(basename "$f")"
done

# config.ini: rebuilt from the source or package template with the values of the
# existing file (renamed/removed settings migrated), then any --set values.
# API keys are never printed.
INI=ux0:/data/VitaJPOverlay/config.ini
if exists "$INI"; then
  get "$INI" "$TMP/config.old"
  cp "$TMP/config.old" "$BK/config.ini"
fi
if python3 tools/migrate_config.py "$TMP/config.old" "$CONFIG_TEMPLATE" "$TMP/config.ini" ${SETS[@]+"${SETS[@]}"}; then
  put "$TMP/config.ini" "$INI"
  [ -f "$TMP/config.old" ] && echo "config.ini updated (previous copy in $BK)" || echo "config.ini created"
else
  migration_status=$?
  if [ "$migration_status" -ne 1 ]; then
    echo "config.ini migration failed; plugin configuration was not activated"
    exit "$migration_status"
  fi
  echo "config.ini is up to date"
fi
for CFG in "${CFGS[@]}"; do put "$TMP/${CFG%%:*}.new" "$CFG"; done
echo "Installed. Reboot the Vita, then leave it on for a minute. A boot that does not complete makes the next one a safe boot (no hooks, no overlay; see ux0:data/VitaJPOverlay/status.txt). If it fails to boot at all, hold L while powering on and run: tools/install_ftp.sh --uninstall ${HOST%%:*}"
