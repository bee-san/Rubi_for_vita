# The fast gate's host image (tools/fast_gate.sh): the tools of CI's `host` job
# (.github/workflows/release.yml) on Ubuntu 24.04, the runner's release: CMake, Ninja, GCC 13,
# Python 3.12 with Pillow, libjpeg's headers (the card picture's decoder test, test_card_picture.c), the ARM
# lane's GCC 14 cross compiler, its libc and qemu-arm, and the pinned anki package (tools/requirements-anki.txt)
# in /opt/anki for the .apkg import test.
# fast_gate.sh builds it once, tagged with the hash of this file and requirements-anki.txt.
FROM docker.io/library/ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends cmake ninja-build gcc g++ make python3 python3-pil python3-venv \
      git file curl ca-certificates zlib1g-dev libjpeg-turbo8-dev gcc-14-arm-linux-gnueabihf libc6-dev-armhf-cross \
      qemu-user-static \
 && rm -rf /var/lib/apt/lists/*
COPY requirements-anki.txt /opt/requirements-anki.txt
RUN python3 -m venv /opt/anki \
 && /opt/anki/bin/pip install --no-cache-dir --require-hashes --only-binary=:all: -r /opt/requirements-anki.txt
