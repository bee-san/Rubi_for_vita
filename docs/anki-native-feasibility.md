# Native Anki sync on Vita: investigation, 2026-10-11

The intended architecture is a **standalone Vita sync app** sharing the on-disk
mining queue. The overlay only saves bounded text. Full collection, login,
network and media work belongs in the app process, with its own memory budget.
A separately installed app devoted to sync is also possible. No PC/server
companion is part of the requested design.

**This branch does not implement AnkiWeb sync.** It provides the local queue,
on-device login entry/storage and the investigation below. Keep
[issue #29](https://github.com/bee-san/vita-jp-overlay/issues/29) open until the
engine, collection sync and device acceptance criteria are met. No physical
Vita was accessed or changed during this work.

## Memory findings

An isolated Linux probe with the official `anki` 26.9.2 Python package opened
a new collection and added 10,000 synthetic Basic notes:

| Stage | Peak host RSS, KiB |
| --- | ---: |
| Python process | 16,748 |
| Import Anki | 58,604 |
| Open collection | 68,584 |
| Add 10,000 notes | 77,800 |

The resulting collection was 7,520,256 bytes. Peak RSS was about **76 MiB**,
including Python, Linux's native backend and their allocators. An earlier
slightly different sample peaked around 79 MiB. Neither number measures
Vita RAM, full sync, a large existing collection or media transfer. The
Python package version also differs from the engine source pinned below.

Reproduce without opening a real profile or using credentials:

```sh
python3 tools/measure_anki_memory.py --notes 10000
```

There is a concrete collection-size-dependent RAM risk in the pinned engine:
[full upload](https://github.com/ankitects/anki/blob/e64c6b1aee3e8d668fb8bbe084beada8e070d985/rslib/src/sync/collection/upload.rs)
reads the whole database into a byte vector. The
[HTTP response monitor](https://github.com/ankitects/anki/blob/e64c6b1aee3e8d668fb8bbe084beada8e070d985/rslib/src/sync/http_client/io_monitor.rs)
preallocates a vector from the advertised uncompressed size and reads the
entire response into it; [full download](https://github.com/ankitects/anki/blob/e64c6b1aee3e8d668fb8bbe084beada8e070d985/rslib/src/sync/collection/download.rs)
writes that completed buffer to a temporary file. A large collection can
therefore exceed available memory even in a standalone app.

For Vita, full transfers need streaming to/from checked temporary files,
bounded decompression and response limits, followed by integrity verification
and atomic publication. Increasing a reservation alone does not fix this
growth. SQLite cache and media batches also need explicit budgets. Measure
actual peak app memory for first download, incremental upload, media, retries
and the user's collection before choosing a larger app heap.

The new Shell mining path adds no persistent memory block or thread. It uses
the existing result arena temporarily and a small stack buffer to scan the
queue. In equivalent local builds, Shell ELF sections changed as follows:

| Section | Before | After |
| --- | ---: | ---: |
| Text | 179,320 B | 182,472 B |
| Data | 132 B | 132 B |
| BSS | 167,888 B | 167,176 B |

These are executable section sizes, not runtime heap measurements. Existing
baseline AnkiConnect code was disabled, so removing it does not imply that its
old worker allocation was live or that this branch reclaimed that allocation.

## Cross-build findings

The investigation used Anki's AMGI-pinned revision
`e64c6b1aee3e8d668fb8bbe084beada8e070d985`, ARM VitaSDK GCC 15.2.0,
Rust `nightly-2026-10-11` (1.101.0-nightly, `6f5da406b`) and
`armv7-sony-vita-newlibeabihf`. Rust classifies this as a
[Tier 3 target with partial standard-library support](https://doc.rust-lang.org/rustc/platform-support/armv7-sony-vita-newlibeabihf.html),
so the standard library was built from source.

| Check | Result |
| --- | --- |
| Mio 1.2.3 polling/networking compile check | Passed |
| Engine with updated Mio | Failed: signal-hook-registry expects unavailable Vita signal types/fields |
| Disable server-only signals | Progressed; failed: FSRS/Burn system metrics require an unsupported systemstat backend |
| Disable FSRS system metrics | Progressed; failed: bundled SQLite's Unix VFS includes unavailable `sys/ioctl.h` |
| SQLite `SQLITE_OS_OTHER=1` | C compilation passed; engine needed its Qt English templates as well as core templates |
| Patched engine with both template sets | **Full `cargo check -p anki --features rustls` passed for Vita** |

The successful check covered the Rust sync client, Reqwest/Rustls, Tokio,
Rusqlite bindings and Zstandard dependencies. It did **not** link or execute an
Anki engine, initialize SQLite, establish TLS, log in or synchronize a card.
With `SQLITE_OS_OTHER=1`, the compiled SQLite object has unresolved
`sqlite3_os_init` and `sqlite3_os_end` symbols. A functional Vita filesystem
adapter is still required; excluding the Unix adapter is only a diagnostic.
English templates suffice for this compile check; translations were not built.

Reproduce in a new disposable checkout with git, protoc, Python, VitaSDK and
the pinned Rust nightly plus `rust-src` installed:

```sh
export VITASDK=/path/to/vitasdk
rustup toolchain install nightly-2026-10-11 --profile minimal --component rust-src
bash tools/probe_anki_vita.sh
```

The script fetches the pinned upstream source into `build/anki-vita-probe`,
updates Mio, copies FSRS before disabling metrics, excludes Unix signals on
Vita and generates the protobuf descriptor. It never patches Cargo's cached
source, the working repository, a real Anki collection or a device. An
existing scratch directory is rejected. The copied FSRS source is only for
this diagnostic; no engine dependencies are included in a release package.

## Remaining work before offering sync

1. Implement and test a writable Vita SQLite VFS with correct locking,
   journaling/WAL policy, randomness, timestamps, truncation and checked flushes.
   Use a current bundled SQLite rather than assuming the firmware database API
   satisfies Anki's version and feature requirements.
2. Link a minimal C ABI around the official engine in a standalone app. Verify
   backend initialization and a disposable collection, thread/waker behavior,
   native TLS roots, entropy and clock handling. Record binary size and actual
   Vita memory before changing heap budgets.
3. Replace collection-sized full-sync buffers with bounded file streaming and
   explicit limits. Preserve progress/cancellation and validate the download
   before replacing anything.
4. Implement the collection workflow: login, initial download, resolve
   deck/note type, add queued notes, persist record-to-note IDs, incremental
   collection/media sync, then acknowledge only confirmed uploads. Maintain
   separate imported-but-not-uploaded state across retries and reboot.
5. Back up before replacement and require an explicit direction for a full
   sync. Never upload a new empty collection over an existing account.
6. Add sync/retry/pending/deletion controls, authentication expiry and actionable
   error states. Keep credentials/auth separate from exported files and clear
   all recovery copies on sign-out.
7. Validate against disposable accounts/collections, then record physical Vita
   1000 RAM and end-to-end behavior. Check hosted AnkiWeb access requirements
   and account for Anki/AMGI licensing before distributing an embedded engine.

The current native C queue/Settings build passes the plugin syscall/ABI and
Settings asset checks. Host tests cover short I/O, interrupted appends,
corruption, exact-repeat deduplication, failed credential publication, logout,
stale selections and accepted saves across game exit. Those checks establish
the queue/UI foundation; they do not establish native collection sync.
