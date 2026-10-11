#!/usr/bin/env python3
"""Install checksum-pinned VitaSDK packages for the standalone settings app."""
import argparse
import hashlib
from pathlib import Path
import shutil
import tarfile
import urllib.request

PACKAGES = {
    "libvita2d": "f2688386ca15e030450223812c65beea4d1ce930a79b8a99d04f99b55ab1a455",
    "libpng": "e502fac413e17b75a06f8f2a88f87daec13d83a33e9206713a0612ee245b72b8",
    "libjpeg-turbo": "87511c384b36f944655e298140f24215f3da8b59821deddca0f6a9c5e3438b78",
    "freetype": "ad918e4075822109a61b9a19609ec0524d9b0fdc1878e5b74fc5c8cbc443f763",
    "zlib": "f39cea16c176f93dc744597113f093db0d48adf23dd25a14ee0d2ec47df17fa6",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    args = parser.parse_args()
    args.cache.mkdir(parents=True, exist_ok=True)
    for name, digest in PACKAGES.items():
        archive = args.cache / (name + ".tar.xz")
        if not archive.exists() or hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
            url = f"https://github.com/vitasdk/packages/releases/download/master/{name}.tar.xz"
            urllib.request.urlretrieve(url, archive)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
            raise ValueError(f"{name}: package changed; review and update the pin before using it")
        with tarfile.open(archive) as package:
            members = [m for m in package.getmembers()
                       if m.name.startswith(("include/", "lib/")) and not m.isdir()]
            if not members:
                raise ValueError(f"{name}: missing VitaSDK package files")
            for member in members:
                destination = args.sdk / "arm-vita-eabi" / member.name
                if not destination.resolve().is_relative_to((args.sdk / "arm-vita-eabi").resolve()):
                    raise ValueError(f"{name}: invalid package path")
                destination.parent.mkdir(parents=True, exist_ok=True)
                with package.extractfile(member) as source, destination.open("wb") as target:
                    shutil.copyfileobj(source, target)
        print(f"{name}: verified {digest}")


if __name__ == "__main__":
    main()
