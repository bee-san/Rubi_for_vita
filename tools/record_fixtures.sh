#!/bin/sh
# Records Google Lens's response for every host/samples/*.jpg into
# host/fixtures/<name>/lens.pb, and expected.txt from vjo-cli --replay: the
# recognized sentence, as the test replays it (host/tests/test_core.c
# test_fixtures). Review expected.txt before committing.
set -e
cd "$(dirname "$0")/.."
CLI=build/host/vjo-cli
[ -x "$CLI" ] || { cmake -S . -B build/host >/dev/null && cmake --build build/host >/dev/null; }
for img in host/samples/*.jpg; do
  name=$(basename "$img" .jpg)
  dir=host/fixtures/$name
  mkdir -p "$dir"
  rm -f "$dir/lens.pb"
  "$CLI" "$img" --record "$dir" >/dev/null 2>&1 || true
  [ -f "$dir/lens.pb" ] || { echo "$name: no Lens response recorded"; continue; }
  # Exits 1 when the replay shows an error (no Lens response).
  "$CLI" --replay "$dir" > "$dir/expected.txt" || true
  echo "$name: $(wc -c < "$dir/lens.pb") B lens, expected.txt"
done
