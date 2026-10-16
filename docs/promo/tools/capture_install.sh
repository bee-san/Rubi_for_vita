#!/usr/bin/env bash
# Runs tools/capture_install.py in a private user and network namespace, where loopback also
# carries the video's console address (192.168.1.42): nothing is sent off the machine.
# Needs util-linux unshare, iproute2, curl, bash and a Python with pyftpdlib
# (tools/requirements-capture.txt); PYTHON names it (default python3).
set -euo pipefail
cd "$(dirname "$0")/.."
PY=${PYTHON:-python3}
exec unshare -rn sh -c 'ip link set lo up && ip addr add 192.168.1.42/32 dev lo && exec "$0" tools/capture_install.py "$@"' "$PY" "$@"
