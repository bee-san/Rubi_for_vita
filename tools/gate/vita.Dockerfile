# The fast gate's Vita image (tools/fast_gate.sh): CI's `vita` job container (.github/workflows/release.yml),
# vitasdk/vitasdk:2026.08 by the digest CI pins, with the job's apt packages and tools/setup_toolchain.sh
# already run (taiHEN, the Rubi app's pinned packages, the pinned vita-headers' Sysmem stubs, ScePaf, and
# psp2cxml-tool in /opt/toolchain). fast_gate.sh builds it once, tagged with the hash of this file and the
# two scripts.
FROM docker.io/vitasdk/vitasdk@sha256:8227da1b596d981f4513cd9b36bda1e43857f6dae5bcf4312c1cd40167f7d9bb
USER root
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ patch python3 openssh-client \
 && rm -rf /var/lib/apt/lists/*
COPY setup_toolchain.sh setup_settings_deps.py /opt/rubi-tools/
RUN WORK=/opt/toolchain bash /opt/rubi-tools/setup_toolchain.sh
ENV PATH=/opt/toolchain/psp2cxml-tool/mbuild:$PATH
