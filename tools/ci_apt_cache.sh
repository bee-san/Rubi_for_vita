#!/usr/bin/env bash
# apt packages for a CI job from the .deb files kept in DIR (actions/cache), the mirror only when they are
# not enough (.github/workflows/release.yml):
#
#   bash tools/ci_apt_cache.sh DIR PACKAGE...
#
# With .deb files in DIR it installs those whose package this system lacks or has older (dpkg -i, no
# network), then checks that every PACKAGE is installed. Without them, or when that fails, it runs
# apt-get update and apt-get install with DIR as apt's archive directory, so DIR then holds every .deb
# the install fetched and actions/cache keeps them for the next run. The runner's Azure mirror gave
# about 120 kB/s on 2026-10-26: the ARM lane's 61.3 MB (gcc-14-arm-linux-gnueabihf, libc6-dev-armhf-cross,
# qemu-user-static) took about 8.5 minutes, and three attempts of one run's host job were cancelled in it.
# Runs as root, or with sudo.
set -euo pipefail
[ $# -ge 2 ] || { echo "usage: $0 DIR PACKAGE..." >&2; exit 2; }
DIR=$1; shift
mkdir -p "$DIR/partial"
# absolute: apt reads a relative Dir::Cache::Archives under its own Dir::Cache (/var/cache/apt/)
DIR=$(cd "$DIR" && pwd)
SUDO=; [ "$(id -u)" = 0 ] || SUDO=sudo
export DEBIAN_FRONTEND=noninteractive
installed() { # installed PACKAGE: its installed version, or nothing
  dpkg-query -W -f='${db:Status-Abbrev} ${Version}' "$1" 2> /dev/null | sed -n 's/^ii  *//p'
}
all_installed() {
  local p missing=0
  for p in "$@"; do [ -n "$(installed "$p")" ] || { echo "ci_apt_cache: $p is not installed" >&2; missing=1; }; done
  return $missing
}

from_cache() {
  local debs=() deb pkg ver have log=${TMPDIR:-/tmp}/ci_apt_cache-dpkg.log
  for deb in "$DIR"/*.deb; do
    [ -f "$deb" ] || continue
    pkg=$(dpkg-deb -f "$deb" Package) ver=$(dpkg-deb -f "$deb" Version)
    have=$(installed "$pkg")
    # never a downgrade: a newer runner image may have a package newer than the cached one
    if [ -z "$have" ] || dpkg --compare-versions "$have" lt "$ver"; then debs+=("$deb"); fi
  done
  if [ ${#debs[@]} -gt 0 ]; then
    echo "ci_apt_cache: installing ${#debs[@]} cached .deb file(s), no download"
    $SUDO dpkg -i "${debs[@]}" > "$log" 2>&1 || { tail -20 "$log" >&2; return 1; }
  fi
  all_installed "$@"
}

if ls "$DIR"/*.deb > /dev/null 2>&1 && from_cache "$@"; then
  exit 0
fi
echo "ci_apt_cache: from the mirror (no cache, or the cached .debs were not enough)"
rm -f "$DIR"/*.deb
# The runner image's sources go through /etc/apt/apt-mirrors.txt (Azure's mirror first, archive.ubuntu.com
# and security.ubuntu.com after it). apt moves to the next mirror only when one fails, never when it is
# slow, as Azure's was on 2026-10-26: archive.ubuntu.com goes first here (apt still checks every file
# against the signed indexes, whichever mirror serves it).
if [ -f /etc/apt/apt-mirrors.txt ] && grep -q 'archive\.ubuntu\.com' /etc/apt/apt-mirrors.txt; then
  printf 'http://archive.ubuntu.com/ubuntu/\tpriority:1\nhttp://azure.archive.ubuntu.com/ubuntu/\tpriority:2\nhttp://security.ubuntu.com/ubuntu/\tpriority:3\n' |
    $SUDO tee /etc/apt/apt-mirrors.txt > /dev/null
fi
# The .debs go to DIR (--download-only, DIR as apt's archive directory), then are installed from there.
opts=(-o "Dir::Cache::Archives=$DIR" -o Acquire::Retries=3 -o Acquire::http::Timeout=60)
$SUDO apt-get "${opts[@]}" update
for attempt in 1 2 3; do
  $SUDO apt-get "${opts[@]}" install -y --download-only --no-install-recommends "$@" && break
  [ "$attempt" = 3 ] && exit 1
  echo "ci_apt_cache: the download failed (attempt $attempt of 3); again in $((attempt * 15)) s" >&2
  sleep $((attempt * 15))
  $SUDO apt-get "${opts[@]}" update || true
done
# The runner's image (Keep-Downloaded-Packages false) and the vitasdk image (docker-clean) make apt delete
# the .debs it installed from its archive directory: a copy in DIR.keep goes back into DIR after the install.
keep="$DIR.keep"
$SUDO rm -rf "$keep" && $SUDO mkdir -p "$keep"
$SUDO find "$DIR" -maxdepth 1 -name '*.deb' -exec cp -p {} "$keep"/ \;
$SUDO apt-get "${opts[@]}" install -y --no-install-recommends "$@"
$SUDO find "$keep" -maxdepth 1 -name '*.deb' -exec cp -p {} "$DIR"/ \;
$SUDO rm -rf "$keep"
# actions/cache runs as the job's user: the .debs must be readable by it
$SUDO chmod -R a+rX "$DIR"
all_installed "$@"
kept=$(find "$DIR" -maxdepth 1 -name '*.deb' | wc -l)
echo "ci_apt_cache: $kept .deb file(s) kept for the cache"
