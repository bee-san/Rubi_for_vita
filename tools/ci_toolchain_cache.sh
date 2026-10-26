#!/usr/bin/env bash
# tools/setup_toolchain.sh, its result kept in CACHE for the next run (CI's `vita` job, actions/cache):
#
#   WORK=DIR bash tools/ci_toolchain_cache.sh CACHE
#
# Without a snapshot in CACHE it runs tools/setup_toolchain.sh and records what it did to $VITASDK:
# every file it wrote (sdk-delta.tar), every file of the image's it removed (sdk-deleted.txt: the
# ScePaf install replaces the SDK's own paf headers and stubs), and from WORK the built psp2cxml-tool and
# the pinned settings packages (work.tar). With one it puts them back: removes those files, unpacks the
# rest, and then runs tools/setup_toolchain.sh, which finds every step done (taiHEN, the packages, checked
# against their pins again, the headers' marker, ScePaf, psp2cxml-tool on PATH) and changes nothing.
# The SDK is then the one a full setup gives in the same image (the cache key names the image's SDK
# and these scripts). Run as the container's root, as CI's job is.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
[ $# = 1 ] || { echo "usage: WORK=DIR $0 CACHE" >&2; exit 2; }
CACHE=$(mkdir -p "$1" && cd "$1" && pwd)
: "${VITASDK:?VITASDK must name the SDK (the vitasdk image sets it)}"
WORK=${WORK:-$ROOT/.refs/toolchain}
mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)
sdk_files() { find "$VITASDK" -xdev \( -type f -o -type l \) | LC_ALL=C sort; }

if [ -f "$CACHE/sdk-delta.tar" ] && [ -f "$CACHE/sdk-deleted.txt" ] && [ -f "$CACHE/work.tar" ]; then
  while IFS= read -r f; do rm -f -- "$f"; done < "$CACHE/sdk-deleted.txt"
  tar -xpf "$CACHE/sdk-delta.tar" -C /
  tar -xpf "$CACHE/work.tar" -C "$WORK"
  echo "toolchain cache: restored $(tar -tf "$CACHE/sdk-delta.tar" | wc -l) SDK files, removed $(wc -l < "$CACHE/sdk-deleted.txt")"
else
  rm -f "$CACHE/sdk-delta.tar" "$CACHE/sdk-deleted.txt" "$CACHE/work.tar" "$CACHE/sdk-delta.txt"
  sdk_files > "$CACHE/sdk-before.txt"
  touch "$CACHE/marker"
  sleep 1 # every file the setup writes is changed after the marker (find -cnewer)
  WORK=$WORK bash "$ROOT/tools/setup_toolchain.sh"
  sdk_files > "$CACHE/sdk-after.txt"
  LC_ALL=C comm -23 "$CACHE/sdk-before.txt" "$CACHE/sdk-after.txt" > "$CACHE/sdk-deleted.txt"
  find "$VITASDK" -xdev \( -type f -o -type l \) -cnewer "$CACHE/marker" | LC_ALL=C sort > "$CACHE/sdk-delta.txt"
  tar -cpf "$CACHE/sdk-delta.tar" -P -T "$CACHE/sdk-delta.txt"
  tar -cpf "$CACHE/work.tar" -C "$WORK" psp2cxml-tool/mbuild/psp2cxml-tool settings-packages
  rm -f "$CACHE/sdk-before.txt" "$CACHE/sdk-after.txt" "$CACHE/marker"
  echo "toolchain cache: recorded $(wc -l < "$CACHE/sdk-delta.txt") SDK files, $(wc -l < "$CACHE/sdk-deleted.txt") removed"
fi
# After a restore, the check: every step of the setup finds its result and does nothing.
export WORK
PATH="$WORK/psp2cxml-tool/mbuild:$PATH" bash "$ROOT/tools/setup_toolchain.sh"
