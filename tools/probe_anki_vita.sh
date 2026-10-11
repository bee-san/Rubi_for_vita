#!/usr/bin/env bash
# Compile-check the pinned engine in an isolated scratch checkout. No device I/O.
# This is a porting diagnostic, not a build of a working sync client.
set -euo pipefail
if [[ ${1:-} == --help ]]; then
    printf '%s\n' 'Usage: VITASDK=/path/to/vitasdk bash tools/probe_anki_vita.sh [NEW_SCRATCH_DIR]'
    printf '%s\n' 'Needs git, Python 3, protoc, and Rust nightly-2026-10-11 with rust-src.'
    exit 0
fi
: "${VITASDK:?Set VITASDK to the VitaSDK directory}"
VJO_REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
VJO_PROBE_ROOT=${1:-"$VJO_REPO_ROOT/build/anki-vita-probe"}
VJO_ANKI_PIN=e64c6b1aee3e8d668fb8bbe084beada8e070d985
VJO_RUST=${VJO_RUST_TOOLCHAIN:-nightly-2026-10-11}
for program in cargo git python3 protoc; do command -v "$program" >/dev/null; done
if [[ -e "$VJO_PROBE_ROOT" ]]; then
    printf 'Scratch directory already exists: %s. Choose a new directory.\n' "$VJO_PROBE_ROOT" >&2
    exit 1
fi
mkdir -p "$VJO_PROBE_ROOT"
VJO_PROBE_ROOT=$(cd "$VJO_PROBE_ROOT" && pwd)
export PATH="$VITASDK/bin:$PATH"
export CC_armv7_sony_vita_newlibeabihf=arm-vita-eabi-gcc
export AR_armv7_sony_vita_newlibeabihf=arm-vita-eabi-ar
git init -q "$VJO_PROBE_ROOT/anki"
git -C "$VJO_PROBE_ROOT/anki" fetch -q --depth=1 https://github.com/ankitects/anki.git "$VJO_ANKI_PIN"
git -C "$VJO_PROBE_ROOT/anki" checkout -q --detach FETCH_HEAD
cd "$VJO_PROBE_ROOT/anki"
# English templates are sufficient for this compile check. No translations are shipped.
mkdir -p ftl/core-repo/core ftl/qt-repo/desktop out/rslib/proto out/extracted/protoc/bin
ln -s "$(command -v protoc)" out/extracted/protoc/bin/protoc
protoc -Iproto --include_imports --include_source_info \
    --descriptor_set_out=out/rslib/proto/descriptors.bin proto/anki/*.proto
cargo +"$VJO_RUST" update -p mio --precise 1.2.3
# Locate FSRS 5.2.0 so we can copy and patch it without modifying Cargo's cache.
cargo +"$VJO_RUST" metadata --format-version 1 > "$VJO_PROBE_ROOT/metadata.json"
python3 - "$VJO_PROBE_ROOT" <<'PY'
from pathlib import Path
import json, shutil, sys
root = Path(sys.argv[1])
metadata = json.loads((root / 'metadata.json').read_text())
package = next(p for p in metadata['packages'] if p['name'] == 'fsrs' and p['version'] == '5.2.0')
shutil.copytree(Path(package['manifest_path']).parent, root / 'vendor/fsrs-5.2.0')
manifest = Path('Cargo.toml')
manifest.write_text(manifest.read_text().replace('"macros", "signal"', '"macros"') +
                    '\n[patch.crates-io]\nfsrs = { path = "../vendor/fsrs-5.2.0" }\n')
library = Path('rslib/Cargo.toml')
library.write_text(library.read_text() + '\n[target.\'cfg(not(target_os = "vita"))\'.dependencies]\n'
                   'tokio = { workspace = true, features = ["signal"] }\n')
server = Path('rslib/src/sync/http_server/mod.rs')
server.write_text(server.read_text().replace('            let _ = tokio::signal::ctrl_c().await;',
    '            #[cfg(not(target_os = "vita"))]\n'
    '            let _ = tokio::signal::ctrl_c().await;\n'
    '            #[cfg(target_os = "vita")]\n'
    '            std::future::pending::<()>().await;'))
fsrs = root / 'vendor/fsrs-5.2.0/Cargo.toml'
fsrs.write_text(fsrs.read_text().replace('    "metrics",\n', ''))
PY
unset EXTRA_FTL_ROOT
# This excludes SQLite's Unix VFS. A real Vita VFS must supply sqlite3_os_init/end
# before anything can link or open a collection. Compile success is not runtime proof.
export CFLAGS_armv7_sony_vita_newlibeabihf='-DSQLITE_OS_OTHER=1 -DSQLITE_OMIT_LOAD_EXTENSION=1 -DSQLITE_TEMP_STORE=2'
cargo +"$VJO_RUST" check -p anki --features rustls -Z build-std=std,panic_abort \
    --target armv7-sony-vita-newlibeabihf
printf '%s\n' 'Compile check only. SQLite VFS, engine linking, sync streaming and Vita runtime validation remain.'
